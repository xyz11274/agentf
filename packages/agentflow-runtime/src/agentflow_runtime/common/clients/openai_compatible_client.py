#!/usr/bin/env python3
"""
OpenAI-compatible LLM client — 通过 HTTP API 调用。

支持流式与非流式两种模式。流式模式下逐 token 通过 response_push 推送。
"""

from __future__ import annotations

import json
import logging
import os
import random
import time
from collections.abc import Callable
from typing import Any

import requests

from .base import LlmClient
from agentflow_runtime.security import (
    SecretStreamRedactor,
    find_secret_exposures,
    redact_secrets,
    register_secret,
)
from agentflow_runtime.security.trusted_media import (
    TrustedMediaError,
    validate_credential_url,
)

logger = logging.getLogger("agentflow.node.openai_client")


def _safe_text(value: Any) -> str:
    safe_value, _redaction = redact_secrets(str(value))
    return safe_value

class _SafeStreamCallback:
    """Stateful DLP adapter for provider SSE callbacks."""

    def __init__(self, callback: Callable[[str], None]) -> None:
        self._callback = callback
        self._content = SecretStreamRedactor()
        self._tool_arguments: dict[str, SecretStreamRedactor] = {}
        self._tool_indices: dict[str, Any] = {}
        self._tool_choice_indices: dict[str, Any] = {}
        self._content_choice_index: Any = 0

    def _emit(self, payload: dict[str, Any]) -> None:
        safe_payload, _report = redact_secrets(payload)
        try:
            self._callback(json.dumps(safe_payload, ensure_ascii=False))
        except Exception as exc:
            logger.warning(
                "on_chunk callback raised: %s", type(exc).__name__
            )

    def feed(self, chunk: dict[str, Any]) -> None:
        """Release only fields that cannot complete a registered secret."""
        try:
            choices = chunk.get("choices") or []
            if not choices or not isinstance(choices[0], dict):
                return
            choice = choices[0]
            delta = choice.get("delta") or {}
            if not isinstance(delta, dict):
                return

            safe_chunk, _report = redact_secrets(chunk)
            safe_choice = safe_chunk["choices"][0]
            safe_delta = safe_choice.get("delta") or {}
            safe_choice["delta"] = safe_delta
            should_emit = False

            content = delta.get("content")
            if isinstance(content, str) and content:
                self._content_choice_index = choice.get("index", 0)
                released = self._content.feed(content)
                safe_delta["content"] = released
                should_emit = bool(released)

            tool_calls = delta.get("tool_calls")
            safe_tool_calls = safe_delta.get("tool_calls")
            if isinstance(tool_calls, list) and isinstance(safe_tool_calls, list):
                for original, safe_tool in zip(tool_calls, safe_tool_calls):
                    if not isinstance(original, dict) or not isinstance(safe_tool, dict):
                        continue
                    original_fn = original.get("function") or {}
                    safe_fn = safe_tool.get("function") or {}
                    safe_tool["function"] = safe_fn
                    fragment = original_fn.get("arguments")
                    if isinstance(fragment, str) and fragment:
                        raw_index = original.get("index", 0)
                        state_key = str(raw_index)
                        redactor = self._tool_arguments.setdefault(
                            state_key, SecretStreamRedactor()
                        )
                        self._tool_indices[state_key] = raw_index
                        self._tool_choice_indices[state_key] = choice.get("index", 0)
                        safe_fn["arguments"] = redactor.feed(fragment)
                should_emit = should_emit or bool(safe_tool_calls)

            if should_emit:
                self._emit(safe_chunk)
        except Exception as exc:
            logger.warning(
                "stream callback redaction failed: %s", type(exc).__name__
            )

    def flush(self) -> None:
        """Release sanitized suffixes after the provider stream ends."""
        released = self._content.flush()
        if released:
            self._emit({
                "choices": [{
                    "index": self._content_choice_index,
                    "delta": {"content": released},
                    "finish_reason": None,
                }],
            })
        for state_key, redactor in self._tool_arguments.items():
            released = redactor.flush()
            if not released:
                continue
            self._emit({
                "choices": [{
                    "index": self._tool_choice_indices.get(state_key, 0),
                    "delta": {
                        "tool_calls": [{
                            "index": self._tool_indices.get(state_key, 0),
                            "function": {"arguments": released},
                        }],
                    },
                    "finish_reason": None,
                }],
            })


class OpenAiCompatibleClient(LlmClient):
    """OpenAI 兼容 API 的 HTTP 客户端。"""

    def __init__(
        self,
        model: str,
        base_url: str,
        api_key: str,
        max_retries: int = 4,
        retry_base_delay: float = 2.0,
        connect_timeout: int = 5,
        request_timeout: int = 120,
        retry_jitter: float = 0.25,
    ):
        # Strip internal AISF prefix — upstream APIs (DashScope, OpenAI, etc.)
        # do not understand the "aisf/" routing convention.
        if model.startswith("aisf/"):
            model = model[len("aisf/"):]
        register_secret("llm.api_key", api_key)
        if find_secret_exposures(base_url).applied:
            raise ValueError("provider URL must not contain credential material")
        try:
            validated_base_url = validate_credential_url(base_url)
        except TrustedMediaError as exc:
            raise ValueError(str(exc)) from exc

        self.model = model
        self.base_url = validated_base_url.rstrip("/")
        if not self.base_url.endswith("/chat/completions"):
            self.base_url += "/chat/completions"
        self.api_key = api_key
        self.max_retries = max_retries
        self.retry_base_delay = retry_base_delay
        # Small per-attempt jitter prevents parallel workers from retrying in
        # lockstep after a provider capacity/rate-limit response.  Keep this
        # transport-local and opt-out via env for deterministic test clients.
        self.retry_jitter = max(0.0, float(retry_jitter))
        self.connect_timeout = connect_timeout
        self.request_timeout = request_timeout
        # Per-call transport telemetry. TraceRecorder creates one client for
        # each LLM call, so this does not alter the OpenAI-compatible response.
        self.last_attempts = 0
        self.last_retry_reasons: list[str] = []
        self.last_http_status: int | None = None
        self.last_usage_source = "missing"
        self.last_usage_estimator_version = ""

        # Env override
        self.max_retries = int(os.environ.get("AF_LLM_MAX_RETRIES", str(self.max_retries)))
        self.retry_base_delay = float(os.environ.get("AF_LLM_RETRY_BASE_DELAY", str(self.retry_base_delay)))
        self.retry_jitter = max(0.0, float(os.environ.get("AF_LLM_RETRY_JITTER", str(self.retry_jitter))))
        self.connect_timeout = int(os.environ.get("AF_LLM_CONNECT_TIMEOUT", str(self.connect_timeout)))
        self.request_timeout = int(os.environ.get("AF_LLM_TIMEOUT", str(self.request_timeout)))

    def build_request_body(
        self,
        messages: list[dict[str, Any]],
        *,
        tools: list[dict[str, Any]] | None = None,
        max_completion_tokens: int = 8192,
        stream: bool = False,
        extra_body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Build the exact JSON body used by chat_completion."""
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
            # DashScope rejects non-stream requests when thinking stays enabled.
            body["enable_thinking"] = False
        # Reasoning models need thinking explicitly enabled for streaming.
        if "enable_thinking" not in body:
            body["enable_thinking"] = True
        # Generation parameters cannot override transport/core request fields.
        if extra_body:
            core_keys = {
                "model", "messages", "max_completion_tokens", "tools",
                "tool_choice", "stream", "enable_thinking",
            }
            for key, value in extra_body.items():
                if value is not None and key not in core_keys:
                    body[key] = value
        # Opt-in provider thinking mode; preserve existing defaults when unset.
        thinking_mode = os.environ.get("AF_LLM_THINKING_MODE", "").strip()
        if thinking_mode:
            if thinking_mode not in {"enabled", "disabled", "auto"}:
                raise ValueError("AF_LLM_THINKING_MODE must be enabled, disabled, or auto")
            body["thinking"] = {"type": thinking_mode}
            if thinking_mode != "auto":
                body["enable_thinking"] = thinking_mode == "enabled"
        return body

    def chat_completion(
        self,
        messages: list[dict[str, Any]],
        *,
        tools: list[dict[str, Any]] | None = None,
        max_completion_tokens: int = 8192,
        stream: bool = False,
        on_chunk: Callable[[str], None] | None = None,
        extra_body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """调用 OpenAI 兼容 API，返回 OpenAI 格式响应字典。

        流式模式下，会通过 ``on_chunk`` 回调把每个非空 SSE chunk 脱敏后
        的等价 delta JSON 字符串传给调用方（典型用法：调用方在闭包里
        ``response_push(raw_json, type=..., source=...)``，见
        streaming-response.md §3.5/§3.6）。``on_chunk`` 内的异常会被吞掉
        （守 §3.7：trace 路径失败不中断主流程）。
        """
        # ── Build request body ──
        body = self.build_request_body(
            messages,
            tools=tools,
            max_completion_tokens=max_completion_tokens,
            stream=stream,
            extra_body=extra_body,
        )

        exposure = find_secret_exposures(body)
        if exposure.applied:
            self.last_attempts = 0
            self.last_retry_reasons = []
            self.last_http_status = None
            return {
                "error": "LLM request blocked by credential isolation policy",
                "_llm_error": True,
                "security": exposure.to_dict(),
            }

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
        self.last_attempts = 0
        self.last_retry_reasons = []
        self.last_http_status = None
        self.last_usage_source = "missing"
        self.last_usage_estimator_version = ""
        request_deadline = time.monotonic() + max(0.001, float(self.request_timeout))
        response_data = None
        last_error = None
        attempt = 0
        _data_received = False  # 流式模式：是否已收到 SSE chunk
        _safe_callback = (
            _SafeStreamCallback(on_chunk) if stream and on_chunk else None
        )

        for attempt in range(max_attempts):
            remaining_timeout = request_deadline - time.monotonic()
            if remaining_timeout <= 0:
                last_error = requests.Timeout("LLM request total timeout exhausted")
                break
            attempts_left = max_attempts - attempt
            attempt_timeout = max(0.001, remaining_timeout / attempts_left)
            connect_timeout = min(max(0.001, float(self.connect_timeout)), attempt_timeout)
            try:
                self.last_attempts += 1
                resp = requests.post(
                    self.base_url,
                    headers=headers,
                    json=body,
                    stream=stream,
                    timeout=(connect_timeout, attempt_timeout),
                    verify=True,
                    allow_redirects=False,
                    proxies={"http": "", "https": ""},
                )
                response_status = getattr(resp, "status_code", 200)
                if not isinstance(response_status, int):
                    response_status = 200
                self.last_http_status = response_status
                if time.monotonic() >= request_deadline:
                    raise requests.Timeout("LLM request total timeout exhausted")

                if not stream:
                    if response_status != 200:
                        raise _api_error_from_response(resp, response_status)
                    resp_data = resp.json()
                    if not isinstance(resp_data, dict):
                        raise _ApiError(
                            "Malformed chat completion response: root must be an object"
                        )
                    if "error" in resp_data and "choices" not in resp_data:
                        raise _ApiError(f"API error: {resp_data.get('error')}")
                    choices = resp_data.get("choices")
                    if not isinstance(choices, list) or not choices:
                        raise _ApiError(
                            "Malformed chat completion response: choices must be non-empty"
                        )
                    choice = choices[0]
                    if not isinstance(choice, dict) or not isinstance(
                        choice.get("message"), dict
                    ):
                        raise _ApiError(
                            "Malformed chat completion response: choices[0].message "
                            "must be an object"
                        )
                    usage = resp_data.get("usage")
                    if usage is None:
                        resp_data["usage"] = {}
                    elif not isinstance(usage, dict):
                        raise _ApiError(
                            "Malformed chat completion response: usage must be an object"
                        )
                    self.last_usage_source = "provider" if resp_data.get("usage") else "missing"
                    response_data = resp_data
                    break

                # ── 流式模式 ──
                # 先检查 HTTP 状态码：非 200 时错误体不是 SSE 格式，
                # iter_lines 会因缺少 "data: " 前缀而静默跳过，导致返回空内容。
                if response_status != 200:
                    raise _api_error_from_response(resp, response_status)

                accumulated: list[str] = []
                finish_reason = "stop"
                tool_calls_acc: dict[int, dict] = {}
                usage = None
                resp.encoding = "utf-8"

                for sse_line in resp.iter_lines(decode_unicode=True):
                    if time.monotonic() >= request_deadline:
                        raise requests.Timeout("LLM request total timeout exhausted")
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
                    chunk_usage = chunk.get("usage")
                    if chunk_usage:
                        usage = chunk_usage
                    choices = chunk.get("choices", [])
                    if not choices:
                        continue
                    delta = choices[0].get("delta", {}) or {}
                    token = delta.get("content", "")
                    if token:
                        accumulated.append(token)
                    if _safe_callback:
                        _safe_callback.feed(chunk)
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

                full_text = "".join(accumulated)
                tool_calls_text = json.dumps([v["function"] for v in tool_calls_acc.values()]) if tool_calls_acc else ""
                all_text = full_text + tool_calls_text
                if usage is None:
                    # 部分 API（如 MiniMax 流式）不返回 usage 字段。
                    # 用文本长度估算 token 数（~1 token / 2 chars，中英文混合粗略估计）。
                    estimated = max(1, len(all_text) // 2)
                    usage = {"prompt_tokens": 0, "completion_tokens": estimated, "total_tokens": estimated}
                    self.last_usage_source = "estimated"
                    self.last_usage_estimator_version = "chars_div_2_v1"
                else:
                    self.last_usage_source = "provider"
                    self.last_usage_estimator_version = ""
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


            except Exception as e:
                last_error = e
                retriable = _is_transient_http_error(e)
                if not _data_received and attempt < max_attempts - 1 and retriable:
                    backoff = min(self.retry_base_delay * (2 ** attempt), 8.0)
                    retry_after = (
                        e.retry_after if isinstance(e, _ApiError) else None
                    )
                    if retry_after is not None:
                        backoff = max(backoff, retry_after)
                    if self.retry_jitter and backoff > 0:
                        backoff += random.uniform(
                            0.0, min(self.retry_jitter, backoff)
                        )
                    retry_reason = (f"http_{e.status_code}" if isinstance(e, _ApiError)
                                    and e.status_code is not None else type(e).__name__)
                    self.last_retry_reasons.append(retry_reason)
                    remaining_timeout = request_deadline - time.monotonic()
                    if remaining_timeout <= backoff:
                        last_error = requests.Timeout("LLM request total timeout exhausted")
                        break
                    logger.warning(
                        "transient error attempt=%d/%d, retry in %.1fs: %s (data_received=%s)",
                        attempt + 1, max_attempts, backoff, _safe_text(e), _data_received,
                    )
                    time.sleep(backoff)
                    continue
                logger.warning(
                    "FAILED attempt=%d/%d retriable=%s url=%s err=%s",
                    attempt + 1, max_attempts, retriable, self.base_url, _safe_text(e),
                )
                break
        if _safe_callback:
            _safe_callback.flush()


        if response_data is None:
            return {
                "error": f"LLM call failed: {_safe_text(last_error)}",
                "_llm_error": True,
            }

        safe_response, _redaction = redact_secrets(response_data)
        return safe_response


def _api_error_from_response(resp, status_code: int) -> "_ApiError":
    """Build a status-aware provider error without assuming an OpenAI body shape."""
    try:
        err_data = resp.json()
        raw_error = err_data.get("error", getattr(resp, "text", "")[:200])
        if isinstance(raw_error, dict):
            err_msg = raw_error.get("message", str(raw_error))
        else:
            err_msg = str(raw_error)
    except Exception:
        err_msg = str(getattr(resp, "text", ""))[:200]
    err_msg = _safe_text(err_msg)
    retry_after = None
    if status_code == 429:
        response_headers = getattr(resp, "headers", {}) or {}
        raw_retry_after = response_headers.get("Retry-After")
        try:
            if raw_retry_after is not None:
                retry_after = max(0.0, float(raw_retry_after))
        except (TypeError, ValueError):
            # Date-form Retry-After is provider-specific; exponential retry
            # remains the safe fallback when it cannot be parsed locally.
            retry_after = None
    return _ApiError(
        f"API error (HTTP {status_code}): {err_msg}",
        status_code=status_code,
        retry_after=retry_after,
    )


class _ApiError(Exception):
    """Provider API error with explicit retry classification."""

    def __init__(self, message: str, status_code: int | None = None,
                 retry_after: float | None = None) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.retry_after = retry_after

    @property
    def retriable(self) -> bool:
        return self.status_code in {408, 429, 500, 502, 503, 504}


def _is_transient_http_error(e: Exception) -> bool:
    """判断是否为可重试的网络错误。

    注意：``requests.SSLError`` 继承自 ``requests.ConnectionError``，所以必须
    先检查 SSL 失败信号再走 ConnectionError 通用分支，否则 SSL 证书错误会被
    误判为可重试。

    2026-08-14 修正：原来用 ``"ssl" in msg`` 一刀切排除所有 SSL 错误，
    但 SSLEOFError（如 [SSL: UNEXPECTED_EOF_WHILE_READING]）是**瞬时**错误
    （连接被对端关闭），应当可重试。真正不可重试的只有证书验证错误
    （certificate verify failed / unknown ca）——证书本身有问题，重试无用。
    """
    if isinstance(e, _ApiError):
        return e.retriable
    msg = str(e).lower()
    # SSL 证书错误不重试（先于 isinstance 检查，避免被 SSLError ⊂ ConnectionError 吞掉）。
    # 只排除证书类错误；SSLEOFError 等瞬时 SSL 错误不在此列，走下方 ConnectionError 分支重试。
    if "certificate verify failed" in msg or "certificate_verify_failed" in msg or "unknown ca" in msg:
        return False
    if isinstance(e, (requests.ConnectionError, requests.Timeout)):
        return True
    # DNS / 连接重置 / 超时可重试
    keywords = ("connection", "timeout", "reset", "dns", "econnrefused", "econnreset")
    return any(k in msg for k in keywords)
