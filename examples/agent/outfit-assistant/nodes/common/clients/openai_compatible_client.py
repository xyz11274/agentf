#!/usr/bin/env python3
"""
OpenAI-compatible LLM client — 通过 HTTP API 调用。

支持流式与非流式两种模式。流式模式下逐 token 通过 response_push 推送。
"""

from __future__ import annotations

import json
import logging
import os
import time
from collections.abc import Callable
from typing import Any

import requests

from .base import LlmClient

logger = logging.getLogger("agentflow.node.openai_client")


class OpenAiCompatibleClient(LlmClient):
    """OpenAI 兼容 API 的 HTTP 客户端。"""

    def __init__(
        self,
        model: str,
        base_url: str,
        api_key: str,
        max_retries: int = 0,
        retry_base_delay: float = 1.0,
        connect_timeout: int = 5,
        request_timeout: int = 120,
    ):
        # Strip internal AISF prefix — upstream APIs (DashScope, OpenAI, etc.)
        # do not understand the "aisf/" routing convention.
        if model.startswith("aisf/"):
            model = model[len("aisf/"):]
        self.model = model
        self.base_url = base_url.rstrip("/")
        if not self.base_url.endswith("/chat/completions"):
            self.base_url += "/chat/completions"
        self.api_key = api_key
        self.max_retries = max_retries
        self.retry_base_delay = retry_base_delay
        self.connect_timeout = connect_timeout
        self.request_timeout = request_timeout

        # Env override
        self.max_retries = int(os.environ.get("AF_LLM_MAX_RETRIES", str(self.max_retries)))
        self.retry_base_delay = float(os.environ.get("AF_LLM_RETRY_BASE_DELAY", str(self.retry_base_delay)))
        self.connect_timeout = int(os.environ.get("AF_LLM_CONNECT_TIMEOUT", str(self.connect_timeout)))
        self.request_timeout = int(os.environ.get("AF_LLM_TIMEOUT", str(self.request_timeout)))

    def chat_completion(
        self,
        messages: list[dict[str, Any]],
        *,
        tools: list[dict[str, Any]] | None = None,
        max_completion_tokens: int = 8192,
        stream: bool = False,
        on_chunk: Callable[[str], None] | None = None,
    ) -> dict[str, Any]:
        """调用 OpenAI 兼容 API，返回 OpenAI 格式响应字典。

        流式模式下，会通过 ``on_chunk`` 回调把每个非空 SSE chunk 的**原始
        payload JSON 字符串**透传给调用方（典型用法：调用方在闭包里
        ``response_push(raw_json, type=..., source=...)``，见
        streaming-response.md §3.5/§3.6）。``on_chunk`` 内的异常会被吞掉
        （守 §3.7：trace 路径失败不中断主流程）。
        """
        # ── Build request body ──
        body: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
            "max_completion_tokens": max_completion_tokens,
        }
        if tools:
            body["tools"] = tools
            body["tool_choice"] = "auto"
        if stream:
            body["stream"] = True
        else:
            # DashScope（通义千问）要求非流式请求显式关闭 enable_thinking，
            # 否则默认 true 时 API 会拒绝非流式调用。这个参数对其他 provider
            # 无影响（会被静默忽略）。
            body["enable_thinking"] = False

        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.api_key}",
        }

        # ── Log raw request key fields ──
        # 打印请求摘要，方便定位 max_tokens / model / tools 等参数问题
        msg_count = len(messages)
        msg_chars = sum(len(json.dumps(m, ensure_ascii=False)) for m in messages) if messages else 0
        tool_count = len(tools) if tools else 0
        logger.info(
            "LLM_REQ url=%s model=%s max_tokens=%s msgs=%d msgs_chars=%d "
            "tools=%d stream=%s enable_thinking=%s",
            self.base_url, self.model, max_completion_tokens,
            msg_count, msg_chars, tool_count, stream,
            body.get("enable_thinking", "N/A"),
        )

        # ── Transport-layer retry ──
        # 流式模式也允许重试，但仅限于「还未收到任何 SSE chunk」的连接阶段错误。
        # 一旦收到第一个 SSE 数据，mid-stream 错误不再重试（避免向客户端推送重复流）。
        max_attempts = self.max_retries + 1
        response_data = None
        last_error = None
        attempt = 0
        _data_received = False  # 流式模式：是否已收到 SSE chunk

        for attempt in range(max_attempts):
            try:
                resp = requests.post(
                    self.base_url,
                    headers=headers,
                    json=body,
                    stream=stream,
                    timeout=(self.connect_timeout, self.request_timeout),
                    verify=False,
                )

                if not stream:
                    resp_data = resp.json()
                    if "error" in resp_data and "choices" not in resp_data:
                        raise _ApiError(f"API error: {resp_data.get('error')}")
                    response_data = resp_data
                    break

                # ── 流式模式 ──
                accumulated: list[str] = []
                finish_reason = "stop"
                tool_calls_acc: dict[int, dict] = {}
                usage = None
                resp.encoding = "utf-8"

                for sse_line in resp.iter_lines(decode_unicode=True):
                    if not sse_line or not sse_line.startswith("data: "):
                        continue
                    _data_received = True  # 已收到第一条 SSE 数据
                    payload = sse_line[6:].strip()
                    if payload == "[DONE]":
                        break
                    try:
                        chunk = json.loads(payload)
                    except json.JSONDecodeError:
                        continue
                    if "error" in chunk:
                        raise _ApiError(f"API error: {chunk['error']}")
                    choices = chunk.get("choices", [])
                    if not choices:
                        continue
                    delta = choices[0].get("delta", {}) or {}
                    token = delta.get("content", "")
                    if token:
                        accumulated.append(token)
                    # 透传原始 SSE chunk 给 on_chunk 回调（content 或 tool_calls delta 都触发）。
                    # 异常被吞（守 §3.7：trace 失败不中断主流程）。
                    if on_chunk and (delta.get("content") or delta.get("tool_calls")):
                        try:
                            on_chunk(payload)
                        except Exception:
                            logger.debug("on_chunk callback raised", exc_info=True)
                    # 累加 tool_calls
                    tc_deltas = delta.get("tool_calls")
                    if tc_deltas:
                        for tc in tc_deltas:
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
                            if fn.get("name"):
                                existing["function"]["name"] = fn["name"]
                            if fn.get("arguments"):
                                existing["function"]["arguments"] += fn["arguments"]
                    fr = choices[0].get("finish_reason")
                    if fr:
                        finish_reason = fr
                    usage = chunk["usage"] if "usage" in chunk and chunk["usage"] else None

                full_text = "".join(accumulated)
                if usage is None:
                    usage = {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
                response_message: dict = {"content": full_text, "role": "assistant"}
                if tool_calls_acc:
                    response_message["tool_calls"] = list(tool_calls_acc.values())
                response_data = {
                    "choices": [{
                        "message": response_message,
                        "finish_reason": finish_reason,
                        "index": 0,
                    }],
                    "usage": usage,
                }
                break

            except _ApiError as e:
                last_error = e
                logger.warning(
                    "API_ERROR (non-retriable) attempt=%d/%d: %s",
                    attempt + 1, max_attempts, e,
                )
                break

            except Exception as e:
                last_error = e
                retriable = _is_transient_http_error(e)
                if not _data_received and attempt < max_attempts - 1 and retriable:
                    backoff = min(self.retry_base_delay * (2 ** attempt), 8.0)
                    logger.warning(
                        "transient error attempt=%d/%d, retry in %.1fs: %s (data_received=%s)",
                        attempt + 1, max_attempts, backoff, e, _data_received,
                    )
                    time.sleep(backoff)
                    continue
                logger.warning(
                    "FAILED attempt=%d/%d retriable=%s url=%s err=%s",
                    attempt + 1, max_attempts, retriable, self.base_url, e,
                )
                break

        if response_data is None:
            return {
                "error": f"LLM call failed: {last_error}",
                "_llm_error": True,
            }

        return response_data


class _ApiError(Exception):
    """非重试性 API 错误。"""


def _is_transient_http_error(e: Exception) -> bool:
    """判断是否为可重试的网络错误。

    注意：``requests.SSLError`` 继承自 ``requests.ConnectionError``，所以必须
    先检查 SSL 失败信号再走 ConnectionError 通用分支，否则 SSL 证书错误会被
    误判为可重试。
    """
    msg = str(e).lower()
    # SSL 证书错误不重试（先于 isinstance 检查，避免被 SSLError ⊂ ConnectionError 吞掉）
    if "certificate verify failed" in msg or "ssl" in msg:
        return False
    if isinstance(e, (requests.ConnectionError, requests.Timeout)):
        return True
    # DNS / 连接重置 / 超时可重试
    keywords = ("connection", "timeout", "reset", "dns", "econnrefused", "econnreset")
    return any(k in msg for k in keywords)
