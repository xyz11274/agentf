#!/usr/bin/env python3
"""
LLM trace recorder — DataBus trace recording for LLM API calls.

Each call produces 3 keys under DataBus ``llm`` namespace:
  traces/{call_id}/request   — full HTTP request body
  traces/{call_id}/provider_response — original HTTP response before Hooks
  traces/{call_id}/response  — effective response used after Hooks
  traces/{call_id}/meta      — latency, tokens, finish_reason, summaries
Plus traces/index — ordered list of call_ids for enumeration.

Split out of ``llm_call.py`` (F7.1 合规: 请求/响应完整落盘 + 写后读回校验).
This module owns the trace-recording responsibility only; the LLM call
entry point ``call_llm`` stays in ``llm_call.py``.
"""

import copy
import json
import os
import sys
import time
import uuid

from agentflow_runtime.security import (
    SecretStreamRedactor,
    find_secret_exposures,
    redact_secrets,
    register_environment_secrets,
    register_secret,
)

from agentflow_runtime.common.runtime_context import (
    batch_id as _batch_id,
    completed_skill_name as _completed_skill_name,
    databus_uds_file as _databus_uds_file,
    dialogue_id as _dialogue_id,
    exec_skill_key as _exec_skill_key,
    node_id as _node_id,
    self_task_id as _self_task_id,
    session_id as _session_id,
    skill_name as _skill_name,
)

_SDK_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "..", "..", "src", "runtime", "databus", "clients", "python"
))
if _SDK_DIR not in sys.path:
    sys.path.insert(0, _SDK_DIR)

try:
    from agentflow_databus import DataBusClient as _RealDataBusClient
    from agentflow_databus import DataBusError
    _DATABUS_AVAILABLE = True
except ImportError:
    _DATABUS_AVAILABLE = False


def _log_warn(msg: str) -> None:
    safe_msg, _ = redact_secrets(str(msg))
    print(f"[llm_call] WARN: {safe_msg}", file=sys.stderr, flush=True)


# ── Task context (read once per call from framework-injected env vars) ──
#
# The framework injects node inputs as AF_<KEY> environment variables
# (worker.cpp: to_env_suffix + env_vars.push_back). We read them here so each
# LLM trace record carries its caller identity, enabling downstream analysis
# to reconstruct the coordinator → skill → coordinator control flow WITHOUT
# any framework-side change.
#
# Framework inputs currently injected by Runtime and Worker:
#   coordinator_decide node receives:
#     AF_NODE_ID=coordinator_decide, AF_DIALOGUE_ID, AF_SESSION_ID,
#     AF_SELF_TASK_ID, AF_COMPLETED_SKILL_NAME (non-empty on re-entry)
#   skill_executor node receives:
#     AF_NODE_ID=skill_executor, AF_DIALOGUE_ID, AF_SESSION_ID,
#     AF_SELF_TASK_ID, AF_EXEC_SKILL_KEY, AF_BATCH_ID
#
# AF_SKILL_NAME is resolved from the dispatch manifest by Skill Executor for
# nested LLM calls; task_type still falls back to AF_NODE_ID inference.
#
# task_type inference (no AF_TASK_TYPE env var exists):
#   - AF_NODE_ID contains "skill_executor"  → "skill"        (subagent executing)
#   - AF_COMPLETED_SKILL_NAME non-empty      → "coordinator"  (control returned after skill)
#   - AF_NODE_ID contains "coordinator"      → "coordinator"  (first dispatch / final reply)
#   - otherwise                              → ""             (unknown)
def _infer_task_type(node_id: str, completed_skill: str) -> str:
    nid = (node_id or "").lower()
    if "skill_executor" in nid or "skill" in nid:
        return "skill"
    if completed_skill:
        return "coordinator"
    if "coordinator" in nid:
        return "coordinator"
    return ""


def _task_context() -> dict:
    """Collect framework-injected task identity from environment.

    Missing fields remain empty strings so trace recording never blocks a call.
    """
    node_id = _node_id()
    completed_skill = _completed_skill_name()
    return {
        "dialogue_id": _dialogue_id(),
        "session_id": _session_id(),
        "node_id": node_id,                          # reliable caller identifier
        "task_id": _self_task_id(),
        "skill_name": _skill_name(),
        "exec_skill_key": _exec_skill_key(),
        "batch_id": _batch_id(),
        "completed_skill": completed_skill,          # non-empty ⇒ control returned to coordinator
        "task_type": _infer_task_type(node_id, completed_skill),
    }


def _emit_llm_request_prepared(
        call_id: str,
        request_body: dict,
        task_context: dict,
        *,
        model: str,
        request_url: str,
        stream: bool,
        connect_timeout: int,
        request_timeout: int) -> None:
    """Publish the exact request after serialization and before HTTP starts."""
    try:
        from agentflow_runtime import events, hooks
        from agentflow_runtime.context import AgentContext

        meta = copy.deepcopy(task_context)
        meta.update({
            "request_url": request_url,
            "stream": stream,
            "connect_timeout_s": connect_timeout,
            "request_timeout_s": request_timeout,
        })
        hooks.trigger(
            events.LLM_REQUEST_PREPARED,
            {
                "dialogue_id": task_context.get("dialogue_id", ""),
                "session_id": task_context.get("session_id", ""),
                "call_id": call_id,
                "model": model,
                "request": copy.deepcopy(request_body),
                "meta": meta,
            },
            ctx=AgentContext(
                messages=copy.deepcopy(request_body.get("messages") or []),
            ),
        )
    except Exception as exc:
        _log_warn(f"llm request prepared hook failed: {exc}")


class TraceRecorder:
    """Records LLM call traces to DataBus ``llm`` namespace.

    Singleton — one DataBus connection shared across all call_llm() invocations
    within a node process. All write failures are silently swallowed (trace
    recording never interrupts the LLM call flow).
    """

    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._init()
        return cls._instance

    def _init(self):
        self._session_id = _session_id()
        self._uds_path = _databus_uds_file()
        self._client = None
        self._call_count = 0
        if _DATABUS_AVAILABLE and self._uds_path:
            try:
                self._client = _RealDataBusClient(
                    self._uds_path, session_id=self._session_id, timeout_seconds=5.0)
            except (DataBusError, OSError) as exc:
                _log_warn(f"DataBus init failed: {exc}")
                self._client = None

    @property
    def is_connected(self) -> bool:
        return self._client is not None

    def read_session_messages(self) -> list:
        """Read conversation history from DataBus llm/session_messages."""
        if not self._client:
            return []
        try:
            raw = self._client.llm().get("session_messages", "[]")
            if raw is None or raw == "":
                return []
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8")
            messages = json.loads(raw) if isinstance(raw, str) else raw
            safe_messages, _ = redact_secrets(messages)
            return safe_messages
        except Exception as exc:
            _log_warn(f"read_session_messages failed: {exc}")
            return []

    def write_session_messages(self, messages: list) -> bool:
        """Write the compatibility cache and a dialogue-scoped effective snapshot."""
        if not self._client:
            return False
        safe_messages, _ = redact_secrets(messages)
        payload = json.dumps(safe_messages, ensure_ascii=False)
        try:
            namespace = self._client.llm()
        except Exception as exc:
            _log_warn(f"write_session_messages failed: {exc}")
            return False

        dialogue_id = _dialogue_id()
        if dialogue_id:
            scoped_key = f"conversations/{dialogue_id}/effective_messages"
            try:
                if namespace.put(scoped_key, payload) is False:
                    _log_warn(
                        f"write dialogue effective_messages returned false: {dialogue_id}")
            except Exception as exc:
                _log_warn(f"write dialogue effective_messages failed: {exc}")

        try:
            return namespace.put("session_messages", payload)
        except Exception as exc:
            _log_warn(f"write_session_messages failed: {exc}")
            return False

    def trace_call(self, messages: list, model: str, base_url: str,
                   api_key: str, tools: list | None = None,
                   max_completion_tokens: int = 2000,
                   timeout: int = 120,
                   connect_timeout: int = 5,
                   stream: bool = False,
                   source: str = "",
                   skill: str = "",
                   round_i: int = 0,
                   extra_body: dict | None = None) -> dict:
        """Execute LLM call via requests with full trace recording.

        Two-stage timeout (requests ``timeout=(connect, read)`` tuple):
          connect_timeout  — TCP connect + TLS handshake (network layer).
                             5s is ~25x normal handshake (~0.2s实测 minimaxi),
                             enough for cross-border jitter; network-down
                             fails fast at 5s instead of hanging 60s on
                             SSL_ERROR_SYSCALL.
          timeout          — Whole request including LLM generation.
                             Kept generous (120s) for slow streaming responses.

        **Streaming** — controlled by the ``stream`` parameter (passed from
        the bridge's ``?stream=true`` query param via the messages payload).
        When enabled:
          * Uses ``requests.post(..., stream=True)`` (SSE chunked transfer).
          * Each non-empty ``delta.content`` token is pushed in near real-time
            via ``response_push(type="llm_chunk"|"progress")`` so the Bridge
            can forward it to the frontend before the full response is
            assembled.
          * ``timeout`` still guards the *entire* stream — if the provider
            hangs mid-stream the connection is abandoned after ``timeout`` s.

        **Streaming chunk classification** (streaming-response.md §3.6):
          all sources (coordinator / skill_executor) → type="llm_chunk"
            (原始 delta JSON,增量组装;消费端用 source 区分——coordinator 进正文,
             skill_executor 进思考/过程通道)
          other / empty source         → no chunk push

        Returns OpenAI-compatible response dict, or
        ``{"error": ..., "_llm_error": true}`` on failure.
        """
        register_environment_secrets()
        register_secret("llm.api_key", api_key)
        stream_enabled = stream
        _log_warn(f"[STREAM_DEBUG] stream={stream} → stream_enabled={stream_enabled}")
        self._call_count += 1
        seq = self._call_count
        call_id = uuid.uuid4().hex[:12]
        start_time = time.time()
        timestamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

        # Unified entry log — one line per model call, visible across ALL nodes
        # (coordinator_decide + skill_executor). This is the single source of
        # truth for "which node called the LLM when". Captures node_id from
        # framework-injected AF_NODE_ID so callers don't need to log it themselves.
        ctx = _task_context()
        # The runtime does not currently inject AF_SKILL_NAME into skill
        # executor processes. Prefer the per-call identity resolved from the
        # dispatch manifest so every LLM trace remains attributable.
        if skill:
            ctx["skill_name"] = skill
        ctx.update({
            "source": source,
            "round": round_i,
        })
        _log_warn(
            f"[#{seq}] >>> call_id={call_id} node={ctx.get('node_id','?')} "
            f"task_type={ctx.get('task_type','?')} model={model} "
            f"msgs={len(messages)} tools={bool(tools)} "
            f"budget={max_completion_tokens} "
            f"connect_timeout={connect_timeout}s timeout={timeout}s"
        )

        from agentflow_runtime.common.clients import OpenAiCompatibleClient

        _http_client = OpenAiCompatibleClient(
            model=model, base_url=base_url, api_key=api_key,
            connect_timeout=connect_timeout,
            request_timeout=timeout,
        )

        # Record the exact transport body from the same builder used by the
        # HTTP client. This includes stream/thinking and generation parameters.
        request_body = _http_client.build_request_body(
            messages,
            tools=tools,
            max_completion_tokens=max_completion_tokens,
            stream=stream_enabled,
            extra_body=extra_body,
        )
        request_exposure = find_secret_exposures({
            "request": request_body,
            "request_url": _http_client.base_url,
        })
        if request_exposure.applied:
            safe_request, _ = redact_secrets(request_body)
            safe_url, _ = redact_secrets(_http_client.base_url)
            blocked_response = {
                "error": "LLM request blocked: secret material detected",
                "status": "security_policy_blocked",
                "_llm_error": True,
            }
            _emit_llm_request_prepared(
                call_id,
                safe_request,
                ctx,
                model=model,
                request_url=safe_url,
                stream=stream_enabled,
                connect_timeout=connect_timeout,
                request_timeout=timeout,
            )
            if self._client:
                self._put(f"traces/{call_id}/request", safe_request)
                self._put(f"traces/{call_id}/response", blocked_response)
                blocked_meta = {
                    "model": model,
                    "latency_ms": int((time.time() - start_time) * 1000),
                    "timestamp": timestamp,
                    "prompt_tokens": 0,
                    "completion_tokens": 0,
                    "total_tokens": 0,
                    "finish_reason": "security_policy_blocked",
                    "msg_count": len(messages),
                    "tools_enabled": bool(tools),
                    "transport_attempts": 0,
                    "retries_attempted": 0,
                    "retries_used": 0,
                    "retry_reasons": [],
                    "http_status": None,
                    "usage_source": "blocked",
                    "estimator_version": "",
                    "request_summary": self._summarize_messages(
                        safe_request.get("messages", [])),
                    "response_summary": "blocked by secret egress policy",
                    "request_url": safe_url,
                    "stream": stream_enabled,
                    "connect_timeout_s": connect_timeout,
                    "request_timeout_s": timeout,
                    "security": request_exposure.to_dict(),
                }
                blocked_meta.update(ctx)
                self._put(f"traces/{call_id}/meta", blocked_meta)
                self._verify_meta_written(call_id)
                self._append_to_index(call_id)
            _log_warn(
                f"[#{seq}] <<< {call_id} BLOCKED by secret egress policy "
                f"node={ctx.get('node_id','?')}"
            )
            blocked_response["_agentflow_call_id"] = call_id
            return blocked_response

        _emit_llm_request_prepared(
            call_id,
            request_body,
            ctx,
            model=model,
            request_url=_http_client.base_url,
            stream=stream_enabled,
            connect_timeout=connect_timeout,
            request_timeout=timeout,
        )
        if self._client:
            self._put(
                f"traces/{call_id}/request", copy.deepcopy(request_body)
            )

        # ── Build chunk publisher for streaming mode ──
        # on_chunk callback fires once per non-empty SSE delta. We translate
        # that into response_push on the DataBus so the Bridge can forward
        # tokens to the websocket in near real-time.
        # See streaming-response.md §3.5 / §3.6 for the full contract.
        chunk_publisher = None
        chunk_flusher = None
        if stream_enabled and source and self._client:
            # 所有 LLM 流式输出统一走 llm_chunk(原始 delta JSON,增量组装)——
            # skill_executor 与 coordinator 同通道,用 source 区分消费行为;
            # 不再把 skill 的 LLM 增量降级为 progress(会导致逐 token 碎片化)。
            _chunk_type = "llm_chunk"
            _push_client = self._client
            _stream_redactor = SecretStreamRedactor()
            _last_chunk: dict = {"parsed": None}

            # TTFT（首 token 延迟）统计：首个有效内容 chunk 到达时间 vs 请求开始。
            # 用可变容器避免 nonlocal 声明；失败不中断主流程（§3.7）。
            _ttft = {"logged": False}

            def _push_delta(parsed: dict, delta_content: str) -> None:
                if not delta_content:
                    return
                safe_parsed, _report = redact_secrets(copy.deepcopy(parsed))
                source_choices = safe_parsed.get("choices", [])
                choice_index = 0
                if source_choices and isinstance(source_choices[0], dict):
                    choice_index = source_choices[0].get("index", 0)
                payload = {
                    key: value for key, value in safe_parsed.items() if key != "choices"
                }
                payload["choices"] = [{
                    "index": choice_index,
                    "delta": {"content": delta_content},
                    "finish_reason": None,
                }]
                kwargs: dict = {"type": _chunk_type, "source": source}
                if skill:
                    kwargs["skill"] = skill
                if round_i:
                    kwargs["round"] = str(round_i)
                _push_client.response_push(json.dumps(payload, ensure_ascii=False), **kwargs)

            def _on_chunk(raw_json: str) -> None:
                # 异常全部吞掉（守 §3.7：trace 路径失败不中断主流程）
                try:
                    # DEBUG: 验证 _on_chunk 是否被执行
                    # import sys as _sys
                    # print(f"[LLM_CALL_DEBUG] _on_chunk called: type={_chunk_type} json_len={len(raw_json)} source={source}", file=_sys.stderr, flush=True)

                    # 守 streaming-response.md §3.2 核心原则 #2：
                    #   llm_chunk → 原始 OpenAI delta JSON（choices[0].delta.content），
                    #                Bridge 透传，前端/外部客户端自行组装增量 token
                    # Guard: 跳过无 content 的 chunk（delta 只含 role/tool_calls/finish_reason
                    # 或 content 为空时），避免把原始 JSON 当 token 推给消费端——
                    # tool_calls 增量不推流（前端以最终响应为准），空 content 推送会
                    # 触发 daemon 对空 content 返回 -32602 error。
                    parsed = json.loads(raw_json)
                    delta_content = ""
                    choices = parsed.get("choices", [])
                    if choices:
                        delta = choices[0].get("delta", {})
                        if isinstance(delta, dict):
                            delta_content = delta.get("content") or ""
                    if not delta_content:
                        return
                    if not _ttft["logged"]:
                        _ttft["logged"] = True
                        print(
                            f"[timing] ttft_ms={(time.time() - start_time) * 1000:.1f} "
                            f"call_id={call_id} node={ctx.get('node_id', '?')}",
                            file=sys.stderr, flush=True,
                        )
                    _last_chunk["parsed"] = parsed
                    released = _stream_redactor.feed(delta_content)
                    _push_delta(parsed, released)
                except Exception:
                    import traceback
                    safe_traceback, _report = redact_secrets(traceback.format_exc())
                    print(
                        f"[flow] WARN: chunk_publisher failed\n{safe_traceback}",
                        file=sys.stderr, flush=True,
                    )

            def _flush_chunks() -> None:
                try:
                    released = _stream_redactor.flush()
                    if not released:
                        return
                    parsed = _last_chunk.get("parsed")
                    if not isinstance(parsed, dict):
                        parsed = {"choices": [{"index": 0, "delta": {}}]}
                    _push_delta(parsed, released)
                except Exception:
                    import traceback
                    safe_traceback, _report = redact_secrets(traceback.format_exc())
                    print(
                        f"[flow] WARN: chunk_flusher failed\n{safe_traceback}",
                        file=sys.stderr, flush=True,
                    )

            chunk_publisher = _on_chunk
            chunk_flusher = _flush_chunks

        try:
            response_data = _http_client.chat_completion(
                messages, tools=tools,
                max_completion_tokens=max_completion_tokens,
                stream=stream_enabled,
                on_chunk=chunk_publisher,
                extra_body=extra_body,
            )
        finally:
            if chunk_flusher is not None:
                chunk_flusher()
        response_data, response_redaction = redact_secrets(response_data)
        # The transport owns retries. Read the number of HTTP attempts it
        # actually started instead of maintaining a disconnected outer counter.
        transport_attempts = max(0, int(getattr(_http_client, "last_attempts", 1)))
        retries_used = max(0, transport_attempts - 1)
        retry_reasons = list(getattr(_http_client, "last_retry_reasons", []) or [])
        http_status = getattr(_http_client, "last_http_status", None)
        usage_source = str(
            getattr(_http_client, "last_usage_source", "")
            or ("provider" if response_data.get("usage") else "missing")
        )
        estimator_version = str(
            getattr(_http_client, "last_usage_estimator_version", "") or "")
        # If the client returned an error, wrap it with _llm_error
        if response_data.get("_llm_error") or "error" in response_data:
            last_error = str(response_data.get("error", "Unknown error"))
            latency_ms = int((time.time() - start_time) * 1000)
            if self._client:
                provider_snapshot = copy.deepcopy(response_data)
                self._put(
                    f"traces/{call_id}/provider_response", provider_snapshot)
                self._put(f"traces/{call_id}/response", provider_snapshot)
                error_meta = {
                    "model": model, "latency_ms": latency_ms,
                    "timestamp": timestamp, "error": last_error,
                    "prompt_tokens": 0, "completion_tokens": 0,
                    "total_tokens": 0, "finish_reason": "error",
                    "msg_count": len(messages),
                    "tools_enabled": bool(tools),
                    "transport_attempts": transport_attempts,
                    "retries_attempted": retries_used,
                    "retries_used": retries_used,
                    "retry_reasons": retry_reasons,
                    "http_status": http_status,
                    "usage_source": usage_source,
                    "estimator_version": estimator_version,
                    "request_summary": self._summarize_messages(messages)[:200],
                    "response_summary": f"error: {last_error[:200]}",
                    "request_url": _http_client.base_url,
                    "stream": stream_enabled,
                    "connect_timeout_s": connect_timeout,
                    "request_timeout_s": timeout,
                }
                error_meta.update(ctx)
                if response_redaction.applied:
                    error_meta["security"] = response_redaction.to_dict()
                self._put(f"traces/{call_id}/meta", error_meta)
                self._verify_meta_written(call_id)
                self._append_to_index(call_id)
            _log_warn(
                f"[#{seq}] <<< {call_id} FAILED "
                f"node={ctx.get('node_id','?')} url={_http_client.base_url} "
                f"err={last_error}"
            )
            return {"error": f"LLM call failed: {last_error}",
                    "_llm_error": True,
                    "_agentflow_call_id": call_id}

        latency_ms = int((time.time() - start_time) * 1000)

        # Record full response + meta to DataBus
        if self._client:
            provider_snapshot = copy.deepcopy(response_data)
            self._put(
                f"traces/{call_id}/provider_response", provider_snapshot)
            self._put(
                f"traces/{call_id}/response", copy.deepcopy(provider_snapshot))
            usage = response_data.get("usage", {})
            choice = response_data.get("choices", [{}])[0]
            meta = {
                "model": model,
                "latency_ms": latency_ms,
                "timestamp": timestamp,
                "request_summary": self._summarize_messages(messages),
                "response_summary": self._summarize_response(response_data),
                "prompt_tokens": usage.get("prompt_tokens", 0),
                "completion_tokens": usage.get("completion_tokens", 0),
                "total_tokens": usage.get("total_tokens", 0),
                "finish_reason": choice.get("finish_reason", "unknown"),
                "msg_count": len(messages),
                "tools_enabled": bool(tools),
                "transport_attempts": transport_attempts,
                "retries_used": retries_used,
                "retry_reasons": retry_reasons,
                "http_status": http_status,
                "usage_source": usage_source,
                "estimator_version": estimator_version,
                "request_url": _http_client.base_url,
                "stream": stream_enabled,
                "connect_timeout_s": connect_timeout,
                "request_timeout_s": timeout,
            }
            meta.update(ctx)
            if response_redaction.applied:
                meta["security"] = response_redaction.to_dict()
            self._put(f"traces/{call_id}/meta", meta)
            self._verify_meta_written(call_id)
            self._append_to_index(call_id)

        # Unified success log — tool_calls name list + token usage + finish
        _choice = response_data.get("choices", [{}])[0]
        _msg = _choice.get("message", {})
        _tcs = _msg.get("tool_calls") or []
        _tc_names = [tc.get("function", {}).get("name", "?") for tc in _tcs] if _tcs else []
        _usage = response_data.get("usage", {}) or {}
        _log_warn(
            f"[#{seq}] <<< {call_id} OK latency={latency_ms}ms "
            f"finish={_choice.get('finish_reason','?')} "
            f"tool_calls={_tc_names or 'none'} "
            f"tok=prompt:{_usage.get('prompt_tokens',0)}/"
            f"completion:{_usage.get('completion_tokens',0)}/"
            f"total:{_usage.get('total_tokens',0)} "
            f"retries={retries_used}"
        )
        # Internal correlation field for the immediate after-LLM hook. Add it
        # only after recording so the stored provider response stays clean.
        response_data["_agentflow_call_id"] = call_id
        return response_data

    def record_effective_response(self, call_id: str, response: dict) -> dict:
        """Persist the post-Hook response while preserving the provider response."""
        try:
            clean_response = copy.deepcopy({
                key: value
                for key, value in response.items()
                if key != "_agentflow_call_id"
            })
            clean_response, response_redaction = redact_secrets(clean_response)
        except Exception as exc:
            _log_warn(f"snapshot effective response failed: {exc}")
            return {}

        if not self._client or not call_id:
            return clean_response

        def _decode(value):
            if isinstance(value, bytes):
                value = value.decode("utf-8", errors="replace")
            if isinstance(value, str):
                try:
                    return json.loads(value)
                except (TypeError, ValueError):
                    pass
            return value

        try:
            namespace = self._client.llm()
            response_key = f"traces/{call_id}/response"
            provider_key = f"traces/{call_id}/provider_response"
            provider_response = _decode(namespace.get(provider_key))
            if provider_response is None:
                provider_response = _decode(namespace.get(response_key))
                if provider_response is not None:
                    self._put(provider_key, provider_response)

            self._put(response_key, clean_response)
            meta = _decode(namespace.get(f"traces/{call_id}/meta"))
            if isinstance(meta, dict):
                meta.setdefault("provider_finish_reason", meta.get("finish_reason", "unknown"))
                meta.setdefault("provider_response_summary", meta.get("response_summary", ""))
                choice = (clean_response.get("choices") or [{}])[0]
                effective_finish = choice.get("finish_reason", "unknown")
                if not clean_response.get("choices") and clean_response.get("_llm_error"):
                    effective_finish = "error"
                meta["response_summary"] = self._summarize_response(clean_response)
                meta["finish_reason"] = effective_finish
                meta["hook_modified"] = (
                    provider_response != clean_response
                    if provider_response is not None
                    else None
                )
                if response_redaction.applied:
                    meta["security"] = response_redaction.to_dict()
                self._put(f"traces/{call_id}/meta", meta)
        except Exception as exc:
            _log_warn(f"record effective response failed for call_id={call_id}: {exc}")
        return clean_response

    # ── DataBus helpers ──

    def _put(self, key: str, value) -> None:
        if not self._client:
            return
        try:
            safe_value, _ = redact_secrets(value)
            self._client.llm().put(key, safe_value)
        except Exception as exc:
            # Log diagnostic info for missing-params errors
            sid = self._client.session_id if self._client else "no-client"
            _log_warn(f"put '{key}' failed: {exc} (session_id={sid!r})")

    def _append_to_index(self, call_id: str) -> None:
        if not self._client:
            return
        dialogue_id = _dialogue_id()
        # Concurrent calls use distinct authoritative keys; the legacy list
        # below remains for compatibility but is no longer the truth source.
        entry_scope = dialogue_id or "_global"
        self._put(f"traces/index_entries/{entry_scope}/{call_id}", call_id)
        try:
            key = f"traces/index/{dialogue_id}" if dialogue_id else "traces/index"
            raw = self._client.llm().get(key, "[]")
            index = json.loads(raw) if isinstance(raw, str) else (raw or [])
            if not isinstance(index, list):
                index = []
            index.append(call_id)
            self._client.llm().put(key, json.dumps(index))
        except Exception as exc:
            _log_warn(f"update traces/index failed: {exc}")

    def _verify_meta_written(self, call_id: str) -> None:
        """Read-back check: confirm ``traces/{call_id}/meta`` was written.

        Catches the case where a ``_put`` creates the DataBus key but the
        value ends up empty ({}), which causes blank entries in the trace UI.
        Logs a CRITICAL warning so the root cause can be traced in runtime logs.
        """
        if not self._client:
            return
        try:
            stored = self._client.llm().get(f"traces/{call_id}/meta")
            if stored is None or stored == {}:
                _log_warn(
                    f"CRITICAL: meta for call_id={call_id} appears empty after write! "
                    f"stored={stored!r} node_id={os.environ.get('AF_NODE_ID', '?')}"
                )
        except Exception as exc:
            _log_warn(f"verify meta for call_id={call_id} failed: {exc}")

    @staticmethod
    def _summarize_messages(messages: list) -> str:
        parts = []
        for m in messages:
            role = m.get("role", "?")
            content = m.get("content", "")
            if isinstance(content, str):
                content = content[:120].replace("\n", " ")
            if role == "tool":
                content = str(content)[:60]
            parts.append(f"{role}: {content}")
        summary = " | ".join(parts[:8])
        if len(parts) > 8:
            summary += f" ... (+{len(parts) - 8} more)"
        return summary

    @staticmethod
    def _summarize_response(response: dict) -> str:
        choice = response.get("choices", [{}])[0]
        msg = choice.get("message", {})
        content = msg.get("content", "")
        if isinstance(content, str):
            content = content[:200].replace("\n", " ")
        tool_calls = msg.get("tool_calls")
        if tool_calls:
            names = [tc.get("function", {}).get("name", "?") for tc in tool_calls]
            content = f"tool_calls: {names}"
        return f"finish={choice.get('finish_reason','?')} | {content}"


# ── Public API ──

_recorder: TraceRecorder | None = None


def get_recorder() -> TraceRecorder:
    """Get the singleton TraceRecorder (shared DataBus connection)."""
    global _recorder
    if _recorder is None:
        _recorder = TraceRecorder()
    return _recorder
