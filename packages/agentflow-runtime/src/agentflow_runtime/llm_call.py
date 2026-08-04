#!/usr/bin/env python3
"""
LLM Call — LLM API call with full trace recording to DataBus.

Each call produces 3 keys under DataBus ``llm`` namespace:
  traces/{call_id}/request   — full HTTP request body
  traces/{call_id}/response  — full HTTP response body
  traces/{call_id}/meta      — latency, tokens, finish_reason, summaries
Plus traces/index — ordered list of call_ids for enumeration.

HTTP transport: requests library (verify=False bypasses miniconda Python
SSL cert issues, matching the old curl -k behavior).

No mock mode — always calls the real LLM. For offline testing, point
base_url at a local stub server.
"""

import json
import os
import sys
import time
import traceback
import uuid
from copy import deepcopy

import urllib3

from agentflow_runtime.common.agent_config import AgentConfig
from agentflow_runtime.common.runtime_context import (
    completed_skill_name as _completed_skill_name,
    databus_uds_file as _databus_uds_file,
    dialogue_id as _dialogue_id,
    node_id as _node_id,
    self_task_id as _self_task_id,
    session_id as _session_id,
    skill_name as _skill_name,
)

# ── Add nodes/ to sys.path for local imports ──
_nodes_dir = os.path.dirname(os.path.abspath(__file__))
if _nodes_dir not in sys.path:
    sys.path.insert(0, _nodes_dir)

# Suppress InsecureRequestWarning when verify=False (intentional, matches old curl -k)
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

from agentflow_runtime.common.clients import OpenAiCompatibleClient, resolve_llm_config as _resolve_llm_config

# Re-export for backward compatibility
resolve_llm_config = _resolve_llm_config

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
    print(f"[llm_call] WARN: {msg}", file=sys.stderr, flush=True)


# ── Task context (read once per call from framework-injected env vars) ──
#
# The framework injects node inputs as AF_<KEY> environment variables
# (worker.cpp: to_env_suffix + env_vars.push_back). We read them here so each
# LLM trace record carries its caller identity, enabling downstream analysis
# to reconstruct the coordinator → skill → coordinator control flow WITHOUT
# any framework-side change.
#
# Verified env vars actually injected (confirmed via agentflowd CODE_LOADER_DEBUG
# logs, 2026-07-02):
#   coordinator_decide node receives:
#     AF_NODE_ID=coordinator_decide, AF_DIALOGUE_ID, AF_SESSION_ID,
#     AF_COMPLETED_SKILL_NAME (non-empty on re-entry), AF_SKILL_SUCCESS
#   skill_executor node receives:
#     AF_NODE_ID=skill_executor, AF_DIALOGUE_ID, AF_SESSION_ID,
#     AF_EXEC_SKILL_KEY=dispatch:N
#
# Known framework gaps (env vars NOT injected despite node authors expecting them):
#   - AF_SELF_TASK_ID  — runtime.cpp:1847 sets inputs["self_task_id"] but it
#                        does not reach the node env (only 11 vars injected,
#                        self_task_id missing). task_id left empty.
#   - AF_SKILL_NAME    — skill_executor.py docstring claims "injected by
#                        framework" but it is NOT. Infer task_type from
#                        AF_NODE_ID instead.
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

    Returns a dict with keys: dialogue_id, session_id, node_id, task_id,
    skill_name, completed_skill, task_type. Empty string for any missing
    field — downstream consumers should treat empty as "unknown", not error.

    Note: task_id is currently always empty (AF_SELF_TASK_ID not injected by
    framework — known gap). Use node_id + dialogue_id for caller correlation.
    """
    node_id = _node_id()
    completed_skill = _completed_skill_name()
    return {
        "dialogue_id": _dialogue_id(),
        "session_id": _session_id(),
        "node_id": node_id,                          # reliable caller identifier
        "task_id": _self_task_id(),                   # currently always "" (framework gap)
        "skill_name": _skill_name(),                  # currently always "" (framework gap)
        "completed_skill": completed_skill,          # non-empty ⇒ control returned to coordinator
        "task_type": _infer_task_type(node_id, completed_skill),
    }


class _TraceRecorder:
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
            return json.loads(raw) if isinstance(raw, str) else raw
        except Exception as exc:
            _log_warn(f"read_session_messages failed: {exc}")
            return []

    def write_session_messages(self, messages: list) -> bool:
        """Write conversation history to DataBus llm/session_messages."""
        if not self._client:
            return False
        try:
            return self._client.llm().put("session_messages",
                                          json.dumps(messages, ensure_ascii=False))
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
        stream_enabled = stream
        _log_warn(f"[STREAM_DEBUG] stream={stream} → stream_enabled={stream_enabled}")
        self._call_count += 1
        seq = self._call_count
        call_id = uuid.uuid4().hex[:12]
        start_time = time.time()
        timestamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

        # Build request body
        request_body = {
            "model": model,
            "messages": messages,
            "max_completion_tokens": max_completion_tokens,
        }
        if tools:
            request_body["tools"] = tools
            request_body["tool_choice"] = "auto"

        # Record request to DataBus (before HTTP call)
        if self._client:
            self._put(f"traces/{call_id}/request", request_body)

        # ── HTTP call via requests ──
        url = base_url.rstrip("/")
        if not url.endswith("/chat/completions"):
            url += "/chat/completions"

        # Unified entry log — one line per model call, visible across ALL nodes
        # (coordinator_decide + skill_executor). This is the single source of
        # truth for "which node called the LLM when". Captures node_id from
        # framework-injected AF_NODE_ID so callers don't need to log it themselves.
        ctx = _task_context()
        _log_warn(
            f"[#{seq}] >>> call_id={call_id} node={ctx.get('node_id','?')} "
            f"task_type={ctx.get('task_type','?')} model={model} "
            f"msgs={len(messages)} tools={bool(tools)} "
            f"budget={max_completion_tokens} "
            f"connect_timeout={connect_timeout}s timeout={timeout}s"
        )

        attempt = 0
        _http_client = OpenAiCompatibleClient(
            model=model, base_url=base_url, api_key=api_key,
            connect_timeout=connect_timeout,
            request_timeout=timeout,
        )

        # ── Build chunk publisher for streaming mode ──
        # on_chunk callback fires once per non-empty SSE delta. We translate
        # that into response_push on the DataBus so the Bridge can forward
        # tokens to the websocket in near real-time.
        # See streaming-response.md §3.5 / §3.6 for the full contract.
        chunk_publisher = None
        if stream_enabled and source and self._client:
            # 所有 LLM 流式输出统一走 llm_chunk(原始 delta JSON,增量组装)——
            # skill_executor 与 coordinator 同通道,用 source 区分消费行为;
            # 不再把 skill 的 LLM 增量降级为 progress(会导致逐 token 碎片化)。
            _chunk_type = "llm_chunk"
            _push_client = self._client

            def _on_chunk(raw_json: str) -> None:
                # 异常全部吞掉（守 §3.7：trace 路径失败不中断主流程）
                try:
                    # DEBUG: 验证 _on_chunk 是否被执行
                    import sys as _sys
                    print(f"[LLM_CALL_DEBUG] _on_chunk called: type={_chunk_type} json_len={len(raw_json)} source={source}", file=_sys.stderr, flush=True)
                    
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
                    content = raw_json  # llm_chunk: 原始 delta JSON，Bridge 透传

                    kwargs: dict = {"type": _chunk_type, "source": source}
                    if skill:
                        kwargs["skill"] = skill
                    if round_i:
                        kwargs["round"] = str(round_i)
                    # Guard: 空 content（如 delta 只含 role/tool_calls/finish_reason）跳过推送，
                    # 避免 daemon 对空 content 返回 -32602 error 触发无意义异常
                    if not content:
                        return
                    _push_client.response_push(content, **kwargs)
                except Exception:
                    import traceback
                    print(
                        f"[flow] WARN: chunk_publisher failed\n{traceback.format_exc()}",
                        file=sys.stderr, flush=True,
                    )

            chunk_publisher = _on_chunk

        response_data = _http_client.chat_completion(
            messages, tools=tools,
            max_completion_tokens=max_completion_tokens,
            stream=stream_enabled,
            on_chunk=chunk_publisher,
            extra_body=extra_body,
        )
        # If the client returned an error, wrap it with _llm_error
        if response_data.get("_llm_error") or "error" in response_data:
            last_error = response_data.get("error", "Unknown error")
            latency_ms = int((time.time() - start_time) * 1000)
            if self._client:
                error_meta = {
                    "model": model, "latency_ms": latency_ms,
                    "timestamp": timestamp, "error": last_error,
                    "prompt_tokens": 0, "completion_tokens": 0,
                    "total_tokens": 0, "finish_reason": "error",
                    "msg_count": len(messages),
                    "tools_enabled": bool(tools),
                    "retries_attempted": 0,
                    "request_summary": self._summarize_messages(messages)[:200],
                    "response_summary": f"error: {last_error[:200]}",
                }
                error_meta.update(_task_context())
                self._put(f"traces/{call_id}/meta", error_meta)
                self._verify_meta_written(call_id)
                self._append_to_index(call_id)
            _log_warn(
                f"[#{seq}] <<< {call_id} FAILED "
                f"node={ctx.get('node_id','?')} url={_http_client.base_url} "
                f"err={last_error}"
            )
            return {"error": f"LLM call failed: {last_error}",
                    "_llm_error": True}

        latency_ms = int((time.time() - start_time) * 1000)

        # Record full response + meta to DataBus
        if self._client:
            self._put(f"traces/{call_id}/response", response_data)
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
                "retries_used": attempt,
            }
            meta.update(_task_context())
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
            f"retries={attempt}"
        )
        return response_data

    # ── DataBus helpers ──

    def _put(self, key: str, value) -> None:
        if not self._client:
            return
        try:
            self._client.llm().put(key, value)
        except Exception as exc:
            # Log diagnostic info for missing-params errors
            sid = self._client.session_id if self._client else "no-client"
            _log_warn(f"put '{key}' failed: {exc} (session_id={sid!r})")

    def _append_to_index(self, call_id: str) -> None:
        if not self._client:
            return
        try:
            dialogue_id = _dialogue_id()
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


# ── Token estimation & message compaction ──

def estimate_message_tokens(messages: list) -> int:
    """Estimate total tokens in a message list (conservative overestimate).

    Each message carries ~8 tokens of structural overhead (role key, newlines).
    Content tokens estimated at 2 chars/token for mixed Chinese/English.
    Tool-call arguments add extra token cost.

    The estimate is intentionally conservative (over-counts) to trigger
    compaction slightly earlier than strictly necessary — safer for preventing
    finish=length truncation.
    """
    total = 0
    for msg in messages:
        total += 8  # structural overhead
        content = msg.get("content", "")
        if isinstance(content, str):
            # ~2 chars per token for mixed Chinese/English (conservative)
            total += len(content) // 2
        # Tool calls content
        for tc in msg.get("tool_calls") or []:
            fn = tc.get("function", {})
            total += len(fn.get("name", "")) // 2
            total += len(str(fn.get("arguments", ""))) // 3
    return total


def compact_messages(messages: list, max_tokens: int,
                     compaction_threshold: float = 0.8) -> list:
    """Compact message list by dropping oldest non-system messages.

    Keeps all system messages, then keeps as many of the newest non-system
    messages as fit within ``max_tokens * compaction_threshold``.

    Returns the compacted list (original message objects, not copies).
    If no compaction is needed, returns the original list unchanged.
    """
    budget = int(max_tokens * compaction_threshold)
    current = estimate_message_tokens(messages)
    if current <= budget:
        return messages

    # Separate system vs non-system
    system_msgs = [m for m in messages if m.get("role") == "system"]
    other_msgs = [m for m in messages if m.get("role") != "system"]

    # Build result: start with system, add as many newest non-system messages
    # as fit within budget. Skip (don't break) messages that exceed budget
    # so smaller older messages can still be kept.
    kept = list(system_msgs)
    for msg in reversed(other_msgs):
        candidate = kept + [msg]
        if estimate_message_tokens(candidate) <= budget:
            kept = candidate

    # kept is [system, newest_msg, ...] — reverse non-system part to
    # restore chronological order: [system, oldest_kept, ..., newest]
    kept_system = [m for m in kept if m.get("role") == "system"]
    kept_other  = [m for m in kept if m.get("role") != "system"]
    kept_other.reverse()
    return kept_system + kept_other


# ═══════════════════════════════════════════════════════════════════
# Context compression: fold-to-file + priority-based drop
# (design: docs/design/context-compression.md)
# ═══════════════════════════════════════════════════════════════════

def compress_messages(
    messages: list,
    budget: int,
    session_id: str = "",
    dialogue_id: str = "",
    fold_dir: str = "/tmp/agentflow/folded",
    protect_last_rounds: int = 3,
    hard_compression_enabled: bool = False,
    hard_compression_model: str = "",
    node_type: str = "",
) -> tuple[list, str]:
    """按优先级从低到高逐级压缩消息列表。

    压缩序列（每步完成后检查 budget，≤ budget 即停）：
      1. 丢弃 reminder（P5）
      2. 折叠 tool_result 到文件（P4）
      3. 折叠 tool_call 到文件（P3）
      4. 硬压缩：LLM 语义摘要旧对话轮次（P2，可选，默认关闭）
      5. 丢弃 assistant_content，保留最近 2 条（P2 降级）
      6. 丢弃 user_instruction，保留最近 2 条（P1）
      7. 极端兜底

    返回 (compressed_messages, level)：
      level = "skip" / "drop_reminder" / "fold_tool_result" / "fold_tool_call"
            / "hard_summary" / "drop_assistant" / "drop_user" / "emergency"
    """
    current = estimate_message_tokens(messages)
    if current <= budget:
        return messages, "skip"

    # 深拷贝元素：折叠/丢弃会原地改 msg，不能污染调用方的 messages
    result = [deepcopy(m) for m in messages]
    seq = [0]
    before_tok = current

    def _ratio(after_tok: int) -> float:
        return round(1 - after_tok / before_tok, 2) if before_tok else 0.0

    # ── Step 1: 丢弃 reminder（P5，本轮会重新生成） ──
    before_count = len(result)
    result = [m for m in result if not _is_reminder(m)]
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        dropped = before_count - len(result)
        print(f"[flow]   compaction: drop_reminder "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"dropped={dropped})", file=sys.stderr)
        return result, "drop_reminder"

    # ── Step 2: 折叠 tool_result 到文件（P4） ──
    before_count = len(result)
    result, folded = _fold_by_role(result, "tool", "tool_result", budget, seq,
                                   session_id, dialogue_id, fold_dir)
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        print(f"[flow]   compaction: fold_tool_result "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"folded={folded})", file=sys.stderr)
        return result, "fold_tool_result"

    # ── Step 3: 折叠 tool_call 到文件（P3，保留最近 max(3, N) 条） ──
    # N = protect_last_rounds，保证最近轮次内的 tool_call 链完整
    tool_call_protect = max(3, protect_last_rounds)
    before_count = len(result)
    result, folded = _fold_by_role(result, "assistant", "tool_call", budget, seq,
                                   session_id, dialogue_id, fold_dir,
                                   protect_field="tool_calls", protect_last=tool_call_protect)
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        print(f"[flow]   compaction: fold_tool_call "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"folded={folded})", file=sys.stderr)
        return result, "fold_tool_call"

    # ── Step 4: 硬压缩（P2，LLM 语义摘要，可选，默认关闭） ──
    if hard_compression_enabled:
        before = estimate_message_tokens(result)
        result = _hard_summarize(result, budget, protect_last_rounds,
                                hard_compression_model, node_type)
        after_tok = estimate_message_tokens(result)
        if after_tok < before:
            print(f"[flow]   compaction: hard_summary "
                  f"({before}→{after_tok} tok, ratio={_ratio(after_tok)})",
                  file=sys.stderr)
            return result, "hard_summary"

    # ── Step 5: 丢弃 assistant_content（P2 降级，保留最近 2 条） ──
    # 注意：已折叠的 assistant（_folded=True）不能再丢弃——其 content 是文件索引引用
    before_count = len(result)
    result, dropped = _drop_by_role(result, "assistant", budget, protect_last=2,
                                    exclude_if_has="_folded")
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        print(f"[flow]   compaction: drop_assistant "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"dropped={dropped})", file=sys.stderr)
        return result, "drop_assistant"

    # ── Step 6: 丢弃 user_instruction（P1，保留最近 2 条） ──
    before_count = len(result)
    result, dropped = _drop_by_role(result, "user", budget, protect_last=2,
                                    exclude_if=_is_reminder)
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        print(f"[flow]   compaction: drop_user "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"dropped={dropped})", file=sys.stderr)
        return result, "drop_user"

    # ── Step 7: 极端兜底 ──
    result = _emergency_trim(result, budget)
    return result, "emergency"


def _is_reminder(msg: dict) -> bool:
    """判断是否为 reminder 消息（system-reminder 注入的 user 消息）。"""
    return (msg.get("role") == "user"
            and isinstance(msg.get("content"), str)
            and "<system-reminder>" in msg["content"])


def _fold_by_role(result: list, role: str, ftype: str, budget: int, seq: list,
                  session_id: str, dialogue_id: str, fold_dir: str,
                  protect_field: str = None, protect_last: int = 0) -> tuple[list, int]:
    """按 role 折叠消息到文件。从最旧到最新。返回 (result, folded_count)。"""
    candidates = []
    for i, msg in enumerate(result):
        if msg.get("role") != role:
            continue
        if msg.get("_folded"):
            continue
        if protect_field and not msg.get(protect_field):
            continue
        candidates.append(i)

    # 保护最近 N 条
    if protect_last > 0 and len(candidates) > protect_last:
        candidates = candidates[:-protect_last]

    # 从最旧到最新折叠
    folded = 0
    for i in candidates:
        _fold_one(result[i], seq, session_id, dialogue_id, fold_dir, ftype)
        folded += 1
        if estimate_message_tokens(result) <= budget:
            return result, folded

    return result, folded  # 折叠后仍超


def _drop_by_role(result: list, role: str, budget: int, protect_last: int = 0,
                  exclude_if_has: str = None, exclude_if=None) -> tuple[list, int]:
    """按 role 丢弃消息。从最旧到最新，保留最近 N 条。返回 (result, dropped_count)。"""
    candidates = []
    for i, msg in enumerate(result):
        if msg.get("role") != role:
            continue
        if exclude_if_has and msg.get(exclude_if_has):
            continue  # 跳过已折叠的 tool_call
        if exclude_if and exclude_if(msg):
            continue
        candidates.append(i)

    # 保留最近 N 条
    if protect_last > 0 and len(candidates) > protect_last:
        candidates = candidates[:-protect_last]

    # 从最旧到最新丢弃（从后往前删，避免索引偏移）
    dropped = 0
    for i in reversed(candidates):
        del result[i]
        dropped += 1
        if estimate_message_tokens(result) <= budget:
            return result, dropped

    return result, dropped  # 丢弃后仍超


def _emergency_trim(result: list, budget: int) -> list:
    """极端兜底：只保留 system + 第一轮 + 最近 1 轮。

    第一轮 = 最早一组 user/assistant（R1 保护用户原始意图）。
    最近 1 轮 = 最晚一组 user/assistant（保局部连贯性）。
    """
    system = [m for m in result if m.get("role") == "system"]
    non_sys = [m for m in result if m.get("role") != "system"]

    # 找第一轮和最近 1 轮的边界
    first_user_idx = next((i for i, m in enumerate(non_sys) if m["role"] == "user"), None)
    # 最近一轮的 user 起点（从尾往前找第一个 user）
    last_user_idx = None
    for i in range(len(non_sys) - 1, -1, -1):
        if non_sys[i]["role"] == "user" and not _is_reminder(non_sys[i]):
            last_user_idx = i
            break

    kept = list(system)
    if first_user_idx is not None:
        # 第一轮到下一个 user 之前（或整个尾部如果只有一轮）
        first_round_end = next((i for i in range(first_user_idx + 1, len(non_sys))
                               if non_sys[i]["role"] == "user"), len(non_sys))
        kept.extend(non_sys[first_user_idx:first_round_end])
    if last_user_idx is not None and last_user_idx != first_user_idx:
        kept.extend(non_sys[last_user_idx:])

    print(f"[flow]   compaction: EMERGENCY trim — {len(result)}→{len(kept)} msgs "
          f"({estimate_message_tokens(result)}→{estimate_message_tokens(kept)} tok)",
          file=sys.stderr)
    return kept


def _hard_summarize(result: list, budget: int, protect_last_rounds: int = 3,
                    model: str = "", node_type: str = "") -> list:
    """硬压缩：LLM 语义摘要旧对话轮次。

    保护规则：
      1. 第一轮（用户原始意图）— 不压缩
      2. 最近 N 轮（局部连贯性）— 不压缩
      3. 保护轮次之间的老旧轮次 — 压缩为摘要
    tool_call/tool_result 已在 Step 2-3 中折叠到文件，不参与硬压缩。
    如果 LLM 调用失败，静默跳过，返回原列表。
    """
    # 识别 user/assistant 对话轮次对
    rounds = []
    current_round = []
    for msg in result:
        role = msg.get("role", "")
        if role == "user" and current_round:
            rounds.append(current_round)
            current_round = [msg]
        elif role == "assistant" and not msg.get("tool_calls"):
            current_round.append(msg)
        elif role == "user":
            current_round = [msg]
    if current_round:
        rounds.append(current_round)

    # 保护轮次：第一轮 + 最近 N 轮
    if len(rounds) <= 1 + protect_last_rounds:
        return result  # 不够保护轮次，不压缩

    # 第一轮保护
    first_round = [rounds[0]]
    # 最近 N 轮保护
    last_rounds = rounds[-protect_last_rounds:] if protect_last_rounds > 0 else []
    # 中间可压缩的轮次
    summary_rounds = rounds[1:-protect_last_rounds] if protect_last_rounds > 0 else rounds[1:]

    if not summary_rounds:
        return result  # 没有可压缩的轮次

    # 提取摘要源文本
    source_text = ""
    for r in summary_rounds:
        for msg in r:
            content = msg.get("content", "")
            if content:
                source_text += f"[{msg['role']}]: {content}\n"

    # 调用 LLM 生成摘要
    summary = _call_summary_llm(source_text, model, node_type)
    if not summary:
        return result  # LLM 调用失败，静默跳过

    # 替换原始轮次为摘要消息
    summary_msg = {
        "role": "assistant",
        "content": f"[上下文压缩摘要] {summary}",
        "_meta": {"kind": "context_summary"},
    }

    # 构建新列表：system + 摘要 + 第一轮 + 最近 N 轮
    system = [m for m in result if m.get("role") == "system"]
    other = []
    for r in first_round + last_rounds:
        other.extend(r)

    compressed = system + [summary_msg] + other

    if estimate_message_tokens(compressed) <= budget:
        return compressed

    return result  # 摘要后仍超，返回原列表


def _call_summary_llm(source_text: str, model: str = "", node_type: str = "") -> str | None:
    """调用 LLM 生成对话摘要。失败时返回 None，调用方静默跳过。

    注意：source_text 可能很大。如果估计 token 超过模型窗口，
    从最旧部分截断，保留最新部分。截断后仍超窗口则静默跳过。

    模型选择：显式参数 > 当前节点模型（AgentConfig.llm_model(node_type=...)）
    """
    # 估算 source_text 的 token 数
    # prompt 模板固定部分约 200 tok，输出预留 500 tok
    prompt_overhead = 200
    output_reserve = 500
    model_max = _get_model_max_for_summary(model)
    available = model_max - prompt_overhead - output_reserve

    if available <= 0:
        return None  # 模型窗口太小，无法压缩

    estimated = len(source_text) // 2  # 保守估算：2 字符/token
    if estimated > available:
        # source_text 太长，从最旧部分截断，保留尾部（最新部分）
        ratio = available / estimated
        cut = int(len(source_text) * ratio)
        source_text = f"...(省略前 {len(source_text) - cut} 字符)...\n{source_text[-cut:]}"
        print(f"[flow]   compaction: hard_summary source truncated "
              f"({estimated}→{available} tok)", file=sys.stderr)

    prompt = (
        "你是 AgentFlow 的上下文压缩器。将以下历史对话压缩为结构化的摘要。\n\n"
        "提取以下四类信息：\n"
        "1. 用户的原始输入是什么——第一轮用户的原始诉求，以及被压缩轮次中用户的后续输入\n"
        "2. 已经做完了什么、结论是什么——已执行了哪些工具/技能，得到了什么结果/结论，已决策的事项\n"
        "3. 已经进行过什么尝试、失败的原因——尝试过哪些方案，哪些失败了/被放弃了，原因是什么\n"
        "4. 接下来需要做什么——未完成的事项，下一步计划，需要用户提供什么信息\n\n"
        "要求：\n"
        "- 输出不超过 500 字符\n"
        "- 只总结已有信息，不引入新内容\n"
        "- 不确定的信息标注\"不确定\"\n"
        "- 忽略工具调用的技术细节，只关注业务语义\n\n"
        f"历史对话：\n{source_text}\n\n结构化摘要："
    )
    try:
        # 压缩用模型：显式参数 > 当前节点模型（llm_model 永不返回空，有兜底）
        resolved_model = model or AgentConfig.llm_model(node_type=node_type)
        resp = call_llm(
            messages=[{"role": "user", "content": prompt}],
            model=resolved_model,
            max_completion_tokens=500,
            temperature=0.3,
        )
        # 检查 finish_reason
        finish = resp.get("choices", [{}])[0].get("finish_reason", "")
        if finish == "length":
            print(f"[flow]   compaction: hard_summary output truncated (finish=length)", file=sys.stderr)
            # 截断的输出仍可用，返回已有内容
        content = resp.get("choices", [{}])[0].get("message", {}).get("content", "")
        return content.strip() if content else None
    except Exception as exc:
        print(f"[flow]   compaction: hard_summary LLM call failed: {exc}", file=sys.stderr)
        return None


def _get_model_max_for_summary(model: str) -> int:
    """获取压缩模型的最大上下文窗口（简化版）。"""
    known = {
        "gpt-4o": 128000,
        "gpt-4o-mini": 128000,
        "deepseek-v4-flash": 128000,
        "qwen-vl-max": 32000,
    }
    for prefix, size in known.items():
        if model.startswith(prefix):
            return size
    return 128000  # 默认 128K


def _fold_one(msg: dict, seq: list, session_id: str, dialogue_id: str,
              fold_dir: str, ftype: str = "tool_result") -> None:
    """将单条消息折叠到文件，替换 content 为索引引用（原地修改 msg）。"""
    content = msg.get("content", "")
    tool_call_id = msg.get("tool_call_id", "")
    tool_calls = msg.get("tool_calls")

    n = seq[0]
    seq[0] += 1

    # sanitize session_id/dialogue_id 防路径注入（替换 ../ / 等危险字符）
    safe_session = "".join(c if c.isalnum() or c in "-_" else "_" for c in (session_id or "_"))
    safe_dialog = "".join(c if c.isalnum() or c in "-_" else "_" for c in (dialogue_id or "_"))
    subdir = os.path.join(fold_dir, safe_session, safe_dialog)
    os.makedirs(subdir, exist_ok=True)

    file_path = os.path.join(subdir, f"{ftype}_{n}.txt")
    with open(file_path, "w", encoding="utf-8") as f:
        if tool_calls:
            for tc in tool_calls:
                fn = tc.get("function", {})
                f.write(f"[function: {fn.get('name', '?')}]\n")
                f.write(f"arguments: {fn.get('arguments', '')}\n")
        else:
            f.write(content)

    _update_manifest(subdir, n, ftype, tool_call_id,
                     estimate_message_tokens([msg]), session_id, dialogue_id)

    msg["content"] = f"[{ftype} saved to {file_path}]"
    msg["_folded"] = True
    msg["_folded_file"] = file_path
    if tool_calls:
        msg.pop("tool_calls", None)


def _update_manifest(subdir: str, seq_n: int, ftype: str, tool_call_id: str,
                     tokens: int, session_id: str = "", dialogue_id: str = "") -> None:
    """更新 manifest.json（折叠文件索引清单）。"""
    manifest_path = os.path.join(subdir, "manifest.json")
    manifest = {"session_id": session_id, "dialogue_id": dialogue_id, "folded_items": []}
    if os.path.exists(manifest_path):
        try:
            with open(manifest_path, "r") as f:
                manifest = json.load(f)
        except (json.JSONDecodeError, IOError):
            pass
    manifest["session_id"] = session_id
    manifest["dialogue_id"] = dialogue_id
    manifest["folded_items"].append({
        "seq": seq_n,
        "type": ftype,
        "tool_call_id": tool_call_id,
        "original_tokens": tokens,
        "file": f"{ftype}_{seq_n}.txt",
    })
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)


def get_model_max_tokens() -> int:
    """Read AF_MODEL_MAX_TOKENS env var, defaulting to a safe 128k.

    Delegates to :class:`common.agent_config.AgentConfig`.
    """
    from agentflow_runtime.common.agent_config import AgentConfig
    return AgentConfig.model_max_tokens()


def get_compaction_threshold() -> float:
    """Read AF_COMPACTION_THRESHOLD env var (0.0–1.0), default 0.8.

    Delegates to :class:`common.agent_config.AgentConfig`.
    """
    from agentflow_runtime.common.agent_config import AgentConfig
    return AgentConfig.compaction_threshold()


# ═══════════════════════════════════════════════════════════════════
# Fallback LLM config resolution
# ═══════════════════════════════════════════════════════════════════
#
# Priority chain for each setting (high → low):
#
#   LLM model:    agent.md frontmatter > AF_LLM_MODEL > AF_FALLBACK_LLM_MODEL
#   LLM key:      AF_LLM_API_KEY > API_KEY > AF_FALLBACK_LLM_API_KEY
#   LLM base_url: AF_LLM_BASE_URL > AF_FALLBACK_LLM_BASE_URL
#
#   VL model:    VISION_MODEL > VISION_FALLBACK_MODEL
#   VL key:      VISION_API_KEY > API_KEY > VISION_FALLBACK_API_KEY
#   VL base_url: VISION_BASE_URL > VISION_FALLBACK_BASE_URL

_PLACEHOLDER_PREFIXES = ("${", "sk-your-", "your-")


def _is_placeholder(val: str) -> bool:
    """Check if a value is empty or a placeholder template."""
    if not val:
        return True
    return any(val.startswith(p) for p in _PLACEHOLDER_PREFIXES)


def resolve_vision_config() -> dict[str, str]:
    """Resolve Vision model config (model, base_url, api_key) with fallback.

    Delegates to :class:`common.agent_config.AgentConfig`.
    """
    from agentflow_runtime.common.agent_config import AgentConfig
    return AgentConfig.resolve_vision_config()


# ── Public API ──

_recorder: _TraceRecorder | None = None


def get_recorder() -> _TraceRecorder:
    """Get the singleton TraceRecorder (shared DataBus connection)."""
    global _recorder
    if _recorder is None:
        _recorder = _TraceRecorder()
    return _recorder


def call_llm(messages: list, model: str = "gpt-4o-mini",
             base_url: str = "", api_key: str = "",
             tools: list | None = None,
             max_completion_tokens: int = 2000,
             timeout: int = 120,
             connect_timeout: int = 5,
             stream: bool = False,
             source: str = "",
             skill: str = "",
             round_i: int = 0,
             extra_body: dict | None = None) -> dict:
    """Call LLM API with full trace recording. Returns OpenAI-compatible dict.

    Two-stage timeout (see trace_call docstring):
      connect_timeout — TCP+TLS handshake, fast-fail when network is down
      timeout         — whole request, generous for slow LLM generation
    Both overrideable via env vars AF_LLM_CONNECT_TIMEOUT / AF_LLM_TIMEOUT.

    ``stream`` is passed from the parsed input payload — the bridge includes
    it in the ``{stream, messages}`` wrapper object.  The callee
    (coordinator_decide / skill_executor) extracts ``stream`` via
    ``parse_stream(AF_INPUT)`` and passes it here.

    ``source`` / ``skill`` / ``round_i`` identify the caller for the
    streaming chunk classification:
      source="coordinator" / "skill_executor" → on_chunk pushes type="llm_chunk"
        (统一增量通道;消费端按 source 区分:coordinator 进对话正文,
         skill_executor 进思考/过程通道。汇总进度文本由各节点单独推 type="progress")
    Empty source → no chunk push (the on_chunk callback is not wired).

    ``extra_body`` — additional LLM request body parameters (temperature,
    top_p, frequency_penalty, etc.). Passed as-is to the HTTP/UDS client.
    Callers should read values from ``AgentConfig`` before calling.
    """
    # Env override (M3: configurable, no hidden config). Callers passing
    # explicit args win; env vars fill the default for callers that don't.
    if "AF_LLM_CONNECT_TIMEOUT" in os.environ:
        try:
            connect_timeout = int(os.environ["AF_LLM_CONNECT_TIMEOUT"])
        except ValueError:
            pass
    if "AF_LLM_TIMEOUT" in os.environ:
        try:
            timeout = int(os.environ["AF_LLM_TIMEOUT"])
        except ValueError:
            pass
    return get_recorder().trace_call(
        messages, model, base_url, api_key, tools,
        max_completion_tokens, timeout, connect_timeout, stream,
        source=source, skill=skill, round_i=round_i,
        extra_body=extra_body)
