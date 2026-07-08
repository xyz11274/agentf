#!/usr/bin/env python3
"""
UDS LLM Client — 通过 Unix Domain Socket 调用 LLM。

实现 ``LlmClient`` 抽象接口：
  - ``chat_completion(messages, *, tools, max_completion_tokens, stream)``

双路径（内置）：
  1. UDS 优先：连接 /tmp/agentflow_plugin.sock（agentflow 插件模式）
  2. HTTP 兜底：使用 AF_LLM_API_KEY / AF_LLM_BASE_URL 直接调用

UDS 协议：
  - 4-byte big-endian length prefix + JSON payload
  - 单步 llm.chat 协议（OpenAI 兼容格式透传，旧三段式 create_task → stream → destroy 已移除）

用法：
    from common.clients.base import create_llm_client
    client = create_llm_client()
    resp = client.chat_completion(messages=[{"role": "user", "content": "你好"}])
"""

from __future__ import annotations

import json
import logging
import os
import socket
import struct
import sys
import uuid
from typing import Any, Callable

from .base import LlmClient

logger = logging.getLogger("agentflow.node.llm_uds_client")

# UDS 默认路径（与 HealthAgent 一致）
DEFAULT_SOCKET_PATH = "/tmp/agentflow_plugin.sock"
DEFAULT_MODEL = "qwen3.5-flash"
MAX_FRAME_SIZE = 10 * 1024 * 1024  # 10 MB
UDS_TIMEOUT = 300.0  # 单次 UDS 读写超时（秒）


class LlmUdsError(Exception):
    """UDS LLM 调用异常。"""


class _UdsTransport:
    """UDS 传输层：4-byte length-prefix 帧协议。"""

    def __init__(self, sock_path: str = DEFAULT_SOCKET_PATH):
        self.sock_path = sock_path
        self._sock: socket.socket | None = None

    def connect(self) -> None:
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(UDS_TIMEOUT)
        try:
            self._sock.connect(self.sock_path)
        except (OSError, ConnectionRefusedError) as e:
            self._sock.close()
            self._sock = None
            raise LlmUdsError(f"UDS connect failed: {e}") from e

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def send_json(self, data: dict[str, Any]) -> None:
        """发送 JSON 帧：4-byte BE 长度 + UTF-8 JSON。"""
        if not self._sock:
            raise LlmUdsError("UDS not connected")
        payload = json.dumps(data, ensure_ascii=False).encode("utf-8")
        header = struct.pack(">I", len(payload))
        try:
            self._sock.sendall(header + payload)
        except OSError as e:
            raise LlmUdsError(f"UDS send failed: {e}") from e

    def recv_json(self) -> dict[str, Any]:
        """接收 JSON 帧：先读 4-byte 长度，再读负载。"""
        if not self._sock:
            raise LlmUdsError("UDS not connected")
        try:
            len_buf = self._sock.recv(4)
        except OSError as e:
            raise LlmUdsError(f"UDS recv length failed: {e}") from e
        if len(len_buf) < 4:
            raise LlmUdsError(f"UDS connection closed (received {len(len_buf)}/4 bytes)")
        frame_len = struct.unpack(">I", len_buf)[0]
        if frame_len > MAX_FRAME_SIZE:
            raise LlmUdsError(
                f"UDS frame too large: {frame_len} bytes (max {MAX_FRAME_SIZE})"
            )
        chunks: list[bytes] = []
        remaining = frame_len
        while remaining > 0:
            try:
                chunk = self._sock.recv(remaining)
            except OSError as e:
                raise LlmUdsError(f"UDS recv body failed: {e}") from e
            if not chunk:
                raise LlmUdsError("UDS connection closed mid-frame")
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks).decode("utf-8")
        return json.loads(raw)

    @property
    def connected(self) -> bool:
        return self._sock is not None


class LlmUdsClient(LlmClient):
    """UDS LLM 客户端——实现 ``LlmClient`` 抽象接口。

    双路径：UDS 优先 → HTTP 兜底。

    使用方式：
        from common.clients.base import create_llm_client
        client = create_llm_client()
        resp = client.chat_completion(messages=[...])
    """

    def __init__(
        self,
        sock_path: str = "",
        model: str = "",
    ):
        self.sock_path = sock_path or os.environ.get(
            "AF_UDS_SOCKET_PATH", DEFAULT_SOCKET_PATH
        )
        self.model = (
            model
            or os.environ.get("SYS_AGENT_LLM_MODEL")
            or os.environ.get("AF_LLM_MODEL")
            or os.environ.get("ANTHROPIC_MODEL")
            or DEFAULT_MODEL
        )
        self._transport: _UdsTransport | None = None
        self._req_counter: int = 0

    # ── LlmClient 抽象接口实现 ────────────────────────────────────────

    def chat_completion(
        self,
        messages: list[dict[str, Any]],
        *,
        tools: list[dict[str, Any]] | None = None,
        max_completion_tokens: int = 8192,
        stream: bool = False,
        on_chunk: Callable[[str], None] | None = None,
    ) -> dict[str, Any]:
        """实现 ``LlmClient.chat_completion``。

        UDS 是传输封装层，请求体按 OpenAI Chat Completions 格式构造，
        通过 ``llm.chat`` 方法透传给服务端。服务端负责转发给 FusionMind。

        Args:
            messages: OpenAI 格式消息列表。
            tools: 工具定义列表。
            max_completion_tokens: 最大输出 token 数。
            stream: 是否流式（True = 逐 token 返回，False = 等完整响应）。
            on_chunk: 流式回调，每收到一个 token 时触发（仅在 stream=True 时有效）。

        Returns:
            OpenAI 兼容响应字典。
        """
        if not self._transport or not self._transport.connected:
            self._try_uds()

        if not self._transport or not self._transport.connected:
            raise LlmUdsError(
                f"UDS not available: {self.sock_path}"
            )

        # 构造完整 OpenAI 格式请求体
        request_body: dict[str, Any] = {
            "messages": messages,
            "max_completion_tokens": max_completion_tokens,
        }
        # 剥离 aisf/ 前缀（服务端不需要）
        model_name = self.model
        if model_name.startswith("aisf/"):
            model_name = model_name[len("aisf/"):]
        if model_name:
            request_body["model"] = model_name

        if tools:
            request_body["tools"] = tools
            request_body["tool_choice"] = "auto"

        if stream:
            request_body["stream"] = True
            return self._chat(request_body, on_chunk=on_chunk)
        else:
            request_body["stream"] = False
            return self._chat(request_body, on_chunk=None)

    # ── UDS 路径 ─────────────────────────────────────────────────────

    def _try_uds(self) -> bool:
        """尝试连接 UDS socket。"""
        transport = _UdsTransport(self.sock_path)
        try:
            transport.connect()
            self._transport = transport
            logger.info("UDS connected: %s", self.sock_path)
            return True
        except LlmUdsError as e:
            logger.warning("UDS unavailable: %s", e)
            return False

    # ── llm.chat 路径（OpenAI 格式透传）───────────────────────────

    def _chat(
        self,
        request_body: dict[str, Any],
        on_chunk: Callable[[str], None] | None = None,
    ) -> dict[str, Any]:
        """通过 ``llm.chat`` 方法发送完整 OpenAI 格式请求体。

        Args:
            request_body: 完整的 OpenAI Chat Completions 请求体。
            on_chunk: 流式回调（stream=True 时有效）。

        Returns:
            OpenAI 兼容响应字典。
        """
        t = self._transport
        if not t or not t.connected:
            raise LlmUdsError("UDS not connected")

        req_id = self._next_req_id()
        is_stream = request_body.get("stream", True)

        chat_payload = {
            "request_id": req_id,
            "method": "llm.chat",
            "plugin_type": "LLM",
            "plugin_name": "LLMPlugin",
            "params": {
                "request": request_body,
            },
        }

        logger.debug(
            "UDS -> llm.chat stream=%s body_keys=%s",
            is_stream, list(request_body.keys()),
        )

        logger.info(
            "→ UDS llm.chat REQ raw  %s",
            json.dumps(chat_payload, ensure_ascii=False),
        )

        t.send_json(chat_payload)

        if is_stream:
            return self._receive_chat_events(t, req_id, on_chunk)
        else:
            resp = t.recv_json()

            logger.info(
                "← UDS llm.chat RESP raw (non-stream)  %s",
                json.dumps(resp, ensure_ascii=False),
            )

            # 校验 request_id 配对（防并发串线）
            if resp.get("request_id") and resp["request_id"] != req_id:
                raise LlmUdsError(
                    f"UDS response mismatch: expected request_id={req_id}, "
                    f"got {resp['request_id']}"
                )
            return self._build_from_complete(resp)

    def _receive_chat_events(
        self,
        t: _UdsTransport,
        req_id: str,
        on_chunk: Callable[[str], None] | None = None,
    ) -> dict[str, Any]:
        """接收 llm.chat 的流式事件，返回 OpenAI 兼容响应。

        Args:
            t: UDS 传输层实例。
            req_id: 本请求的 request_id，用于校验配对（不匹配的帧跳过）。
            on_chunk: 流式回调。

        Returns:
            OpenAI 兼容响应字典。
        """
        full_answer = ""
        tool_calls_acc: dict[int, dict[str, Any]] = {}
        finish_reason = "stop"
        usage: dict[str, int] | None = None

        while True:
            event = t.recv_json()

            logger.info(
                "← UDS llm.chat RESP raw event  %s",
                json.dumps(event, ensure_ascii=False),
            )

            # 校验 request_id 配对：不匹配则跳过（并发安全，防串线）
            if event.get("request_id") and event["request_id"] != req_id:
                logger.debug(
                    "skip event: expected request_id=%s, got %s (event_type=%s)",
                    req_id, event.get("request_id"), event.get("event_type", "?"),
                )
                continue
            event_type = event.get("event_type", "")
            data_str = event.get("data", "")
            data: dict[str, Any] = {}
            if data_str:
                try:
                    data = json.loads(data_str) if isinstance(data_str, str) else data_str
                except json.JSONDecodeError:
                    data = {}

            if event_type == "llm_token":
                token = data.get("token", "")
                if token:
                    full_answer += token
                    if on_chunk:
                        # 构造 OpenAI 兼容 SSE chunk 格式，与 HTTP client 行为一致
                        chunk = json.dumps({
                            "choices": [{
                                "delta": {"content": token},
                                "finish_reason": None,
                                "index": 0,
                            }]
                        })
                        try:
                            on_chunk(chunk)
                        except Exception:
                            logger.debug("on_chunk callback raised", exc_info=True)

            elif event_type == "llm_tool_calls":
                tc_list = data.get("tool_calls", [])
                for tc in tc_list:
                    idx = tc.get("index", 0)
                    if idx not in tool_calls_acc:
                        tool_calls_acc[idx] = {
                            "id": tc.get("id", ""),
                            "type": tc.get("type", "function"),
                            "function": {"name": "", "arguments": ""},
                        }
                    existing = tool_calls_acc[idx]
                    if tc.get("id"):
                        existing["id"] = tc["id"]
                    fn = tc.get("function", {})
                    if fn:
                        if fn.get("name"):
                            existing["function"]["name"] = fn["name"]
                        if fn.get("arguments"):
                            existing["function"]["arguments"] += fn["arguments"]
                    # tool_calls delta 也触发 on_chunk（与 HTTP client 一致）
                    if on_chunk:
                        chunk = json.dumps({
                            "choices": [{
                                "delta": {"tool_calls": [tc]},
                                "finish_reason": None,
                                "index": 0,
                            }]
                        })
                        try:
                            on_chunk(chunk)
                        except Exception:
                            logger.debug("on_chunk callback raised", exc_info=True)

            elif event_type == "llm_final":
                answer = data.get("answer", "")
                if answer and not full_answer:
                    full_answer = answer
                finish_reason = data.get("finish_reason", "stop")
                usage_data = data.get("usage")
                if usage_data:
                    usage = {
                        "prompt_tokens": usage_data.get("prompt_tokens", 0),
                        "completion_tokens": usage_data.get("completion_tokens", 0),
                        "total_tokens": usage_data.get("total_tokens", 0),
                    }
                # 组装响应
                result = self._build_openai_response(
                    content=full_answer or answer,
                    finish_reason=finish_reason,
                    tool_calls_acc=tool_calls_acc,
                    usage=usage,
                )
                # 响应日志（完整响应体 JSON）
                logger.info(
                    "← LLM response  %s",
                    json.dumps(result, ensure_ascii=False),
                )
                return result

            elif event_type == "llm_error":
                err_msg = data.get("error_message", "unknown")
                raise LlmUdsError(f"LLM error: {err_msg}")

            # 其他 event_type 忽略

    @staticmethod
    def _build_from_complete(resp: dict[str, Any]) -> dict[str, Any]:
        """从非流式 llm.chat 响应中构造 OpenAI 兼容格式。"""
        data_str = resp.get("data_json", "") or resp.get("data", "")
        if isinstance(data_str, str):
            try:
                data = json.loads(data_str)
            except json.JSONDecodeError:
                data = {}
        else:
            data = data_str or {}

        content = data.get("content", "")
        finish_reason = data.get("finish_reason", "stop")
        tool_calls = data.get("tool_calls")
        usage = data.get("usage")

        message: dict[str, Any] = {"content": content, "role": "assistant"}
        if tool_calls:
            message["tool_calls"] = tool_calls

        result: dict[str, Any] = {
            "choices": [{
                "message": message,
                "finish_reason": finish_reason,
                "index": 0,
            }],
        }
        if usage:
            result["usage"] = usage

        # 响应日志（完整响应体 JSON）
        logger.info(
            "← LLM response (non-stream)  %s",
            json.dumps(result, ensure_ascii=False),
        )
        return result

    @staticmethod
    def _build_openai_response(
        content: str,
        finish_reason: str,
        tool_calls_acc: dict[int, dict[str, Any]] | None = None,
        usage: dict[str, int] | None = None,
    ) -> dict[str, Any]:
        """从流式事件累加结果构造 OpenAI 兼容响应。"""
        message: dict[str, Any] = {"content": content, "role": "assistant"}
        if tool_calls_acc:
            message["tool_calls"] = list(tool_calls_acc.values())

        result: dict[str, Any] = {
            "choices": [{
                "message": message,
                "finish_reason": finish_reason,
                "index": 0,
            }],
        }
        if usage:
            result["usage"] = usage
        return result

    # ── 内部工具 ─────────────────────────────────────────────────────

    def _next_req_id(self) -> str:
        self._req_counter += 1
        return f"py-{self._req_counter}-{uuid.uuid4().hex[:8]}"

    def _close(self) -> None:
        if self._transport:
            self._transport.close()
            self._transport = None

    def __del__(self) -> None:
        self._close()


# ── CLI 快速测试 ────────────────────────────────────────────────────

if __name__ == "__main__":
    logging.basicConfig(level=logging.DEBUG)
    q = sys.argv[1] if len(sys.argv) > 1 else "你好"
    messages = [{"role": "user", "content": q}]
    client = LlmUdsClient()
    try:
        resp = client.chat_completion(messages=messages)
        ans = resp.get("choices", [{}])[0].get("message", {}).get("content", "")
        print(f"\n=== 回答 ===\n{ans}")
    except LlmUdsError as e:
        print(f"❌ {e}", file=sys.stderr)
        sys.exit(1)
