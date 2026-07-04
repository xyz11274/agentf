#!/usr/bin/env python3
"""
LLM Call — LLM API call with full trace recording to DataBus.

Each call produces 3 keys under DataBus ``llm`` namespace:
  traces/{call_id}/request   — full HTTP request body
  traces/{call_id}/response  — full HTTP response body
  traces/{call_id}/meta      — latency, tokens, finish_reason, summaries
Plus traces/index — ordered list of call_ids for enumeration.

HTTP transport: curl subprocess (bypasses miniconda Python SSL cert issues;
curl uses the system keychain which trusts the LLM API's CA).

No mock mode — always calls the real LLM. For offline testing, point
base_url at a local stub server.
"""

import json
import os
import subprocess
import sys
import time
import traceback
import uuid

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
    print(f"[llm_call] WARN: {msg}", file=sys.stderr)


class _ApiError(RuntimeError):
    """Deterministic API-level error (auth/400/quota/payload-too-large).

    Distinct from transient transport errors so the retry loop can skip
    these without inspecting error strings.
    """


# curl exit codes that indicate TRANSIENT network failures worth retrying.
# Source: https://curl.se/docs/manpage.html (EXIT CODES)
#   6  — Couldn't resolve host (DNS hiccup)
#   7  — Failed to connect() to host (connection refused / ECONNREFUSED)
#   28 — Operation timeout (--max-time reached)
#   35 — SSL connect error (TLS handshake failed — the one we just hit)
#   52 — Got nothing; server returned nothing (no headers, no body)
#   56 — Failure with receiving network data (connection reset mid-stream)
# Not retried (deterministic):
#   3  — URL malformed, 4 — feature missing, 5 — proxy resolution,
#   22 — HTTP page not retrieved (4xx/5xx in --fail mode — but we don't
#        use --fail, so 4xx/5xx bodies come through as JSON API errors),
#   60 — peer cert cannot be authenticated (CA trust — won't recover)
_TRANSIENT_CURL_RCS = {6, 7, 28, 35, 52, 56}


def _is_transient_curl_error(exc: Exception) -> bool:
    """Return True if `exc` looks like a transient network error worth retrying.

    Inspects curl's "curl rc=N" marker in the RuntimeError message. Any
    non-curl error (e.g. JSONDecodeError from a garbled but HTTP-200 body)
    is conservatively treated as non-transient — retrying won't help and
    may loop on a deterministic parse failure.
    """
    msg = str(exc)
    # Match "curl rc=NN" — only the numeric rc tells us if it's transient.
    import re as _re
    m = _re.search(r"curl rc=(\d+)", msg)
    if not m:
        return False
    return int(m.group(1)) in _TRANSIENT_CURL_RCS


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
    node_id = os.environ.get("AF_NODE_ID", "")
    completed_skill = os.environ.get("AF_COMPLETED_SKILL_NAME", "")
    return {
        "dialogue_id": os.environ.get("AF_DIALOGUE_ID", ""),
        "session_id": os.environ.get("AF_SESSION_ID", ""),
        "node_id": node_id,                          # reliable caller identifier
        "task_id": os.environ.get("AF_SELF_TASK_ID", ""),  # currently always "" (framework gap)
        "skill_name": os.environ.get("AF_SKILL_NAME", ""), # currently always "" (framework gap)
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
        self._session_id = os.environ.get("AF_SESSION_ID", "")
        self._uds_path = os.environ.get("AF_DATABUS_UDS_FILE", "")
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
                   connect_timeout: int = 5) -> dict:
        """Execute LLM call via curl with full trace recording.

        Two-stage timeout (curl native):
          connect_timeout  — TCP connect + TLS handshake (network layer).
                             5s is ~25x normal handshake (~0.2s实测 minimaxi),
                             enough for cross-border jitter; network-down
                             fails fast at 5s instead of hanging 60s on
                             SSL_ERROR_SYSCALL.
          timeout          — Whole request including LLM generation.
                             Kept generous (120s) for slow streaming responses.

        Returns OpenAI-compatible response dict, or
        ``{"error": ..., "_llm_error": true}`` on failure.
        """
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

        # ── HTTP call via curl ──
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

        # ── Transport-layer retry ──
        #
        # Single source of truth for HTTP retry. Transient network errors
        # (SSL_ERROR_SYSCALL, connection reset, DNS hiccup, curl timeouts)
        # are retried here with exponential backoff. Deterministic errors
        # (auth/400/payload-too-large/business API errors) are NOT retried —
        # they will fail identically on every attempt.
        #
        # Callers' outer "recovery" loops handle SEMANTIC issues (truncated
        # tool args, invalid JSON) by rewriting messages — those must NOT
        # touch messages on pure network errors, since the messages are fine.
        # Network errors that survive all retries bubble up as _llm_error and
        # callers fall back without corrupting conversation history.
        max_retries = int(os.environ.get("AF_LLM_MAX_RETRIES", "2"))
        base_delay = float(os.environ.get("AF_LLM_RETRY_BASE_DELAY", "1.0"))

        response_data = None
        last_error = None
        for attempt in range(max_retries + 1):
            try:
                result = subprocess.run(
                    ["curl", "-k","-sS", "-X", "POST", url,
                     "-H", "Content-Type: application/json",
                     "-H", f"Authorization: Bearer {api_key}",
                     "-d", json.dumps(request_body, ensure_ascii=False),
                     "--connect-timeout", str(connect_timeout),
                     "--max-time", str(timeout)],
                    capture_output=True, text=True, timeout=timeout + 5,
                )
                if result.returncode != 0:
                    raise RuntimeError(
                        f"curl rc={result.returncode}: {result.stderr}")

                response_data = json.loads(result.stdout)
                if "error" in response_data and "choices" not in response_data:
                    # Business/API error (auth, bad request, quota) — NOT
                    # transient. Capture the error text BEFORE nulling
                    # response_data (so the post-loop `response_data is None`
                    # guard routes to the error path), then raise to break.
                    api_err_text = response_data.get("error")
                    response_data = None
                    raise _ApiError(f"API error: {api_err_text}")

                break  # success

            except _ApiError as e:
                # Deterministic API error — do not retry, do not mask as transient.
                last_error = e
                _log_warn(
                    f"[#{seq}] <<< {call_id} API_ERROR (non-retriable) "
                    f"attempt={attempt+1}/{max_retries+1}: {e}"
                )
                break

            except Exception as e:
                last_error = e
                retriable = _is_transient_curl_error(e)
                if attempt < max_retries and retriable:
                    backoff = min(base_delay * (2 ** attempt), 8.0)
                    _log_warn(
                        f"[#{seq}] <<< {call_id} transient error "
                        f"attempt={attempt+1}/{max_retries+1}, "
                        f"retrying in {backoff:.1f}s: {e}"
                    )
                    time.sleep(backoff)
                    continue
                # Either non-retriable, or retries exhausted.
                _log_warn(
                    f"[#{seq}] <<< {call_id} FAILED "
                    f"attempt={attempt+1}/{max_retries+1} "
                    f"retriable={retriable} node={ctx.get('node_id','?')} "
                    f"url={url} err={e}"
                )
                break

        if response_data is None:
            latency_ms = int((time.time() - start_time) * 1000)
            if self._client:
                error_meta = {
                    "model": model, "latency_ms": latency_ms,
                    "timestamp": timestamp, "error": str(last_error),
                    "retries_attempted": attempt + 1,
                }
                error_meta.update(_task_context())
                self._put(f"traces/{call_id}/meta", error_meta)
                self._verify_meta_written(call_id)
                self._append_to_index(call_id)
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
            raw = self._client.llm().get("traces/index", "[]")
            index = json.loads(raw) if isinstance(raw, str) else (raw or [])
            if not isinstance(index, list):
                index = []
            index.append(call_id)
            self._client.llm().put("traces/index", json.dumps(index))
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


def get_model_max_tokens() -> int:
    """Read AF_MODEL_MAX_TOKENS env var, defaulting to a safe 128k."""
    try:
        return int(os.environ.get("AF_MODEL_MAX_TOKENS", "128000"))
    except (ValueError, TypeError):
        return 128000


def get_compaction_threshold() -> float:
    """Read AF_COMPACTION_THRESHOLD env var (0.0–1.0), default 0.8."""
    try:
        val = float(os.environ.get("AF_COMPACTION_THRESHOLD", "0.8"))
        return max(0.1, min(1.0, val))
    except (ValueError, TypeError):
        return 0.8


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
             connect_timeout: int = 5) -> dict:
    """Call LLM API with full trace recording. Returns OpenAI-compatible dict.

    Two-stage timeout (see trace_call docstring):
      connect_timeout — TCP+TLS handshake, fast-fail when network is down
      timeout         — whole request, generous for slow LLM generation
    Both overrideable via env vars AF_LLM_CONNECT_TIMEOUT / AF_LLM_TIMEOUT.
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
        max_completion_tokens, timeout, connect_timeout)
