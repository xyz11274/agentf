#!/usr/bin/env python3
"""
Mock UDS Model Server — 本地调试用

模拟 UDS PluginServer 的协议行为，支持：
  - llm.task.create / llm.stream / llm.task.destroy
  - tool.list / tool.call
  - 可配置的响应内容、延迟、错误模拟

用法：
  python3 mock_uds_server.py                          # 默认 /tmp/mock_agentflow.sock
  python3 mock_uds_server.py --socket /tmp/test.sock  # 自定义路径
  python3 mock_uds_server.py --model echo             # 回显模式
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import random
import signal
import socket
import struct
import sys
import threading
import time
import uuid
from typing import Any

logging.basicConfig(
    level=logging.DEBUG,
    format="[MOCK_UDS] %(asctime)s.%(msecs)03d %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("mock_uds_server")

DEFAULT_SOCKET = "/tmp/mock_agentflow.sock"
MAX_FRAME_SIZE = 10 * 1024 * 1024

# ── 帧协议：4-byte BE 长度前缀 + JSON ─────────────────────────────


def send_frame(sock: socket.socket, data: dict[str, Any]) -> None:
    payload = json.dumps(data, ensure_ascii=False).encode("utf-8")
    header = struct.pack(">I", len(payload))
    try:
        sock.sendall(header + payload)
    except OSError as e:
        logger.warning("send_frame failed: %s", e)


def recv_frame(sock: socket.socket) -> dict[str, Any] | None:
    try:
        len_buf = sock.recv(4)
    except OSError:
        return None
    if len(len_buf) < 4:
        return None
    frame_len = struct.unpack(">I", len_buf)[0]
    if frame_len > MAX_FRAME_SIZE:
        logger.error("frame too large: %d", frame_len)
        return None
    chunks: list[bytes] = []
    remaining = frame_len
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return json.loads(b"".join(chunks))


# ── Mock 响应生成器 ────────────────────────────────────────────────


class MockResponseBuilder:
    """根据配置生成 mock LLM 响应。"""

    def __init__(self, config: dict[str, Any]):
        self.config = config

    def build_answer(self, query: str, prompt: str) -> str:
        mode = self.config.get("mode", "echo")

        if mode == "echo":
            return f"你说了: {query}"

        if mode == "fixed":
            return self.config.get("response", "这是一条 mock 回复。")

        if mode == "stream_demo":
            tokens = list("你好！我是 mock 模型。")
            time.sleep(0.3)
            return "".join(tokens)

        if mode == "weather_tool":
            # 模拟 LLM 调用 get_weather 工具
            return json.dumps({
                "name": "get_weather",
                "arguments": {"city": query}
            })

        # default: 简单回复
        return f"收到你的消息（len={len(query)}），prompt 长度={len(prompt)}"


# ── Mock UDS Server ──────────────────────────────────────────────


class MockUdsServer:
    """Mock UDS 模型服务端。"""

    def __init__(
        self,
        socket_path: str = DEFAULT_SOCKET,
        config: dict[str, Any] | None = None,
    ):
        self.socket_path = socket_path
        self.config = config or {}
        self._server_fd: socket.socket | None = None
        self._running = False
        self._tasks: dict[str, dict] = {}
        self._responder = MockResponseBuilder(self.config)

    # ── 生命周期 ─────────────────────────────────────────────────

    def start(self) -> None:
        """启动 UDS 服务端。"""
        # 清理旧 socket
        if os.path.exists(self.socket_path):
            os.remove(self.socket_path)

        self._server_fd = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._server_fd.bind(self.socket_path)
        self._server_fd.listen(10)
        self._server_fd.settimeout(1.0)  # 1s timeout so we can check _running
        self._running = True

        logger.info("Mock UDS Server 启动: %s", self.socket_path)
        logger.info("  配置: %s", json.dumps(self.config, ensure_ascii=False))
        logger.info("  按 Ctrl+C 停止")

        while self._running:
            try:
                client_fd, addr = self._server_fd.accept()
                logger.debug("新客户端连接")
                client_thread = threading.Thread(
                    target=self._handle_client,
                    args=(client_fd,),
                    daemon=True,
                )
                client_thread.start()
            except socket.timeout:
                continue
            except OSError as e:
                if self._running:
                    logger.error("accept 错误: %s", e)

        self._cleanup()

    def stop(self) -> None:
        """停止服务端。"""
        self._running = False
        logger.info("Mock UDS Server 停止中...")

    def _cleanup(self) -> None:
        if self._server_fd:
            try:
                self._server_fd.close()
            except OSError:
                pass
        if os.path.exists(self.socket_path):
            os.remove(self.socket_path)
        logger.info("已清理 socket: %s", self.socket_path)

    # ── 客户端处理 ───────────────────────────────────────────────

    def _handle_client(self, client_fd: socket.socket) -> None:
        """处理单个客户端连接。"""
        try:
            while self._running:
                req = recv_frame(client_fd)
                if req is None:
                    break

                method = req.get("method", "")
                request_id = req.get("request_id", "")
                params = req.get("params", {})

                logger.debug("→ method=%s request_id=%s", method, request_id)

                if method == "llm.chat":
                    # llm.chat 是单步方法，内部处理事件流或同步回复
                    self._handle_chat(client_fd, params, request_id)
                    continue
                elif method == "llm.task.create":
                    resp = self._handle_create(params)
                elif method == "llm.stream":
                    resp = self._handle_stream(client_fd, params)
                    # llm.stream 通过事件流回复，resp 是确认帧
                    if resp:
                        send_frame(client_fd, resp)
                    continue
                elif method == "llm.task.destroy":
                    resp = self._handle_destroy(params)
                elif method == "llm.complete":
                    resp = self._handle_complete(params)
                elif method == "tool.list":
                    resp = self._handle_tool_list(params)
                elif method == "tool.call":
                    resp = self._handle_tool_call(params)
                elif method == "heartbeat":
                    resp = {"pong": True, "server_time": int(time.time())}
                else:
                    resp = {"error_code": 404, "error_message": f"unknown method: {method}"}

                send_frame(client_fd, self._wrap_response(request_id, resp))

        except Exception as e:
            logger.error("client error: %s", e)
        finally:
            try:
                client_fd.close()
            except OSError:
                pass

    @staticmethod
    def _wrap_response(request_id: str, data: dict) -> dict:
        return {
            "request_id": request_id,
            "status": "success" if data.get("error_code", 0) == 0 else "error",
            "error_code": data.get("error_code", 0),
            "error_message": data.get("error_message", ""),
            "data_json": json.dumps(data, ensure_ascii=False),
            "timestamp": int(time.time() * 1000),
        }

    # ── llm.chat 处理器（OpenAI 格式透传）────────────────────────

    def _handle_chat(self, client_fd: socket.socket, params: dict, request_id: str) -> None:
        """处理 ``llm.chat`` 请求——OpenAI 兼容格式单步方法。"""
        request = params.get("request", {})
        if not isinstance(request, dict):
            send_frame(client_fd, self._wrap_response(request_id, {
                "error_code": 3, "error_message": "Missing request body",
            }))
            return

        is_stream = request.get("stream", True)
        messages = request.get("messages", [])
        tools = request.get("tools", [])

        # 从 messages 中提取 system prompt 和 user query
        prompt = ""
        query = ""
        for msg in messages:
            role = msg.get("role", "")
            content = msg.get("content", "")
            if isinstance(content, list):
                continue
            if role == "system":
                prompt = (prompt + "\n" + content).strip()
            elif role == "user" and not query:
                query = content

        logger.info("llm.chat: stream=%s query_len=%d prompt_len=%d tools=%d",
                     is_stream, len(query), len(prompt), len(tools) if tools else 0)

        # 生成回答
        answer = self._responder.build_answer(query or "你好", prompt)

        if is_stream:
            self._handle_chat_stream(client_fd, request_id, answer, tools)
        else:
            self._handle_chat_complete(client_fd, request_id, answer, tools)

    def _handle_chat_stream(self, client_fd: socket.socket, request_id: str,
                            answer: str, tools: list) -> None:
        """llm.chat 流式模式：逐 token 推送事件（每帧回带 request_id）。
        
        所有流式事件帧携带 request_id 字段，与 vendor C++ UDSPluginServer 行为一致。
        UDSEvent 结构体（UDSInterface.h:133）含 request_id，ToJson() 序列化该字段。
        """
        delay = self.config.get("token_delay", 0.02)

        # 如果请求中有 tools 且配置了 tool 模式，模拟 tool_calls 响应
        if tools and self.config.get("mode") == "weather_tool":
            # 模拟 tool_calls 响应（不输出文本）
            tc_event = {
                    "event_id": f"evt_{uuid.uuid4().hex[:8]}",
                "event_type": "llm_tool_calls",
                "request_id": request_id,
                "channel": "llm.chat",
                "data": json.dumps({
                    "tool_calls": [{
                        "index": 0,
                        "id": "call_mock_001",
                        "type": "function",
                        "function": {
                            "name": "get_weather",
                            "arguments": '{"city": "北京"}',
                        },
                    }],
                }, ensure_ascii=False),
                "done": False,
                "timestamp": int(time.time() * 1000),
            }
            send_frame(client_fd, tc_event)

            final_event = {
                "event_id": f"evt_{uuid.uuid4().hex[:8]}",
                "event_type": "llm_final",
                "request_id": request_id,
                "channel": "llm.chat",
                "data": json.dumps({
                    "answer": "",
                    "finish_reason": "tool_calls",
                    "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
                }, ensure_ascii=False),
                "done": True,
                "timestamp": int(time.time() * 1000),
            }
            send_frame(client_fd, final_event)
            logger.info("llm.chat stream done (tool_calls): request_id=%s", request_id)
            return

        # 逐 token 推送文本回答
        for i, ch in enumerate(answer):
            token_event = {
                    "event_id": f"evt_{uuid.uuid4().hex[:8]}",
                "event_type": "llm_token",
                "request_id": request_id,
                "channel": "llm.chat",
                "data": json.dumps({"token": ch}, ensure_ascii=False),
                "done": False,
                "timestamp": int(time.time() * 1000),
            }
            send_frame(client_fd, token_event)
            if delay > 0:
                time.sleep(delay)

        # final 事件
        final_event = {
            "request_id": request_id,
            "event_id": f"evt_{uuid.uuid4().hex[:8]}",
            "event_type": "llm_final",
            "channel": "llm.chat",
            "data": json.dumps({
                "answer": answer,
                "finish_reason": "stop",
                "usage": {"prompt_tokens": 5, "completion_tokens": len(answer), "total_tokens": 5 + len(answer)},
            }, ensure_ascii=False),
            "done": True,
            "timestamp": int(time.time() * 1000),
        }
        send_frame(client_fd, final_event)
        logger.info("llm.chat stream done: request_id=%s answer_len=%d", request_id, len(answer))

    def _handle_chat_complete(self, client_fd: socket.socket, request_id: str,
                              answer: str, tools: list) -> None:
        """llm.chat 非流式模式：单次回复。"""
        delay = self.config.get("completion_delay", 0)
        if delay > 0:
            time.sleep(delay)

        data = {
            "content": answer,
            "finish_reason": "stop",
            "usage": {"prompt_tokens": 5, "completion_tokens": len(answer), "total_tokens": 5 + len(answer)},
        }
        send_frame(client_fd, self._wrap_response(request_id, data))
        logger.info("llm.chat complete: request_id=%s answer_len=%d", request_id, len(answer))

    # ── 方法处理器 ───────────────────────────────────────────────

    def _handle_create(self, params: dict) -> dict:
        model_name = (
            params.get("model", {}).get("model_name", "")
            or self.config.get("model", "mock-model")
        )
        task_id = f"mock_task_{uuid.uuid4().hex[:8]}"
        self._tasks[task_id] = {
            "model_name": model_name,
            "created_at": time.time(),
            "active": True,
        }
        logger.info("task created: %s model=%s", task_id, model_name)
        return {"code": 0, "message": "ok", "data": {"task_id": task_id}}

    def _handle_stream(self, client_fd: socket.socket, params: dict) -> dict | None:
        task_id = params.get("task_id", "")
        request = params.get("request", {})
        query = ""
        prompt = ""

        if isinstance(request, dict):
            intentions = request.get("intentions", [])
            if intentions:
                query = intentions[0].get("query", "") if isinstance(intentions[0], dict) else ""
            prompts = request.get("prompt", [])
            if prompts:
                prompt = prompts[0] if isinstance(prompts[0], str) else ""

        # 检查 task 是否存在
        task = self._tasks.get(task_id)
        if not task:
            logger.warning("stream: task not found: %s", task_id)
            return {"error_code": 3, "error_message": "Task not found"}

        logger.info("stream: task=%s query_len=%d prompt_len=%d", task_id, len(query), len(prompt))

        # 发送确认帧
        send_frame(client_fd, {
            "request_id": f"{params.get('request_id', '')}_ack",
            "status": "success",
            "code": 0,
            "message": "streaming",
        })

        # 模拟流式延迟
        delay = self.config.get("token_delay", 0.02)
        err_rate = self.config.get("error_rate", 0.0)

        # 生成回答
        answer = self._responder.build_answer(query, prompt)

        # 逐 token 发送
        for i, ch in enumerate(answer):
            if random.random() < err_rate:
                self._send_event(client_fd, task_id, "llm_error", {
                    "task_id": task_id,
                    "error_code": "500",
                    "error_message": "mock error",
                })
                return None
            self._send_event(client_fd, task_id, "llm_token", {
                "task_id": task_id,
                "token": ch,
            })
            if delay > 0:
                time.sleep(delay)

        # 流式结束
        final_data = {
            "task_id": task_id,
            "answer": answer,
            "model": task.get("model_name", ""),
        }
        self._send_event(client_fd, task_id, "llm_final", final_data)
        logger.info("stream done: task=%s answer_len=%d", task_id, len(answer))
        return None

    def _handle_destroy(self, params: dict) -> dict:
        task_id = params.get("task_id", "")
        self._tasks.pop(task_id, None)
        logger.info("task destroyed: %s", task_id)
        return {"code": 0, "message": "ok", "data": {}}

    def _handle_complete(self, params: dict) -> dict:
        request = params.get("request", {})
        intentions = request.get("intentions", []) if isinstance(request, dict) else []
        query = intentions[0].get("query", "") if intentions and isinstance(intentions[0], dict) else ""
        prompts = request.get("prompt", []) if isinstance(request, dict) else []
        prompt = prompts[0] if prompts and isinstance(prompts[0], str) else ""

        model_name = (
            params.get("model", {}).get("model_name", "")
            or self.config.get("model", "mock-model")
        )
        answer = self._responder.build_answer(query, prompt)
        delay = self.config.get("completion_delay", 0)
        if delay > 0:
            time.sleep(delay)

        logger.info("complete: model=%s answer_len=%d", model_name, len(answer))
        return {
            "response": answer,
            "model": model_name,
            "finish_reason": "stop",
        }

    def _handle_tool_list(self, params: dict) -> dict:
        tools = self.config.get("tools", [
            {
                "name": "get_weather",
                "description": "查询天气",
                "input_schema": json.dumps({
                    "type": "object",
                    "properties": {"city": {"type": "string"}},
                    "required": ["city"],
                }),
                "output_schema": "{}",
            },
            {
                "name": "search_knowledge",
                "description": "搜索知识库",
                "input_schema": json.dumps({
                    "type": "object",
                    "properties": {"query": {"type": "string"}},
                    "required": ["query"],
                }),
                "output_schema": "{}",
            },
        ])
        return {"tools": tools}

    def _handle_tool_call(self, params: dict) -> dict:
        tool_name = params.get("tool_name", "")
        tool_params = params.get("params", {})
        logger.info("tool call: %s params=%s", tool_name, tool_params)

        return {
            "content": [{
                "type": "text",
                "text": json.dumps({
                    "result": f"mock result for {tool_name}",
                    "params": tool_params,
                }, ensure_ascii=False),
            }],
            "error_code": 0,
        }

    def _send_event(
        self,
        client_fd: socket.socket,
        task_id: str,
        event_type: str,
        data: dict,
    ) -> None:
        event = {
            "event_id": f"evt_{uuid.uuid4().hex[:8]}",
            "event_type": event_type,
            "channel": "stream.channel",
            "data": json.dumps(data, ensure_ascii=False),
            "done": event_type in ("llm_final", "llm_error"),
            "timestamp": int(time.time() * 1000),
        }
        send_frame(client_fd, event)


# ── CLI ───────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description="Mock UDS Model Server")
    parser.add_argument("--socket", default=DEFAULT_SOCKET, help=f"UDS socket 路径（默认 {DEFAULT_SOCKET}）")
    parser.add_argument("--model", default="mock-model", help="模型名")
    parser.add_argument("--mode", default="echo", choices=["echo", "fixed", "stream_demo", "weather_tool"],
                        help="响应模式")
    parser.add_argument("--response", default="", help="fixed 模式下返回的文本")
    parser.add_argument("--token-delay", type=float, default=0.02, help="每 token 延迟（秒）")
    parser.add_argument("--completion-delay", type=float, default=0, help="非流式完成延迟（秒）")
    parser.add_argument("--error-rate", type=float, default=0.0, help="模拟错误概率（0-1）")
    args = parser.parse_args()

    config = {
        "mode": args.mode,
        "model": args.model,
        "token_delay": args.token_delay,
        "completion_delay": args.completion_delay,
        "error_rate": args.error_rate,
    }
    if args.response:
        config["response"] = args.response

    server = MockUdsServer(socket_path=args.socket, config=config)

    def _signal_handler(signum, frame):
        logger.info("收到信号 %d，停止服务...", signum)
        server.stop()

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    try:
        server.start()
    except KeyboardInterrupt:
        server.stop()


if __name__ == "__main__":
    main()
