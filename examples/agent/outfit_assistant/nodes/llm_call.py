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
                   timeout: int = 120) -> dict:
        """Execute LLM call via curl with full trace recording.

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

        _log_warn(f"[#{seq}] call_id={call_id} model={model} msgs={len(messages)} tools={bool(tools)}")

        try:
            result = subprocess.run(
                ["curl", "-s", "-X", "POST", url,
                 "-H", "Content-Type: application/json",
                 "-H", f"Authorization: Bearer {api_key}",
                 "-d", json.dumps(request_body, ensure_ascii=False),
                 "--max-time", str(timeout)],
                capture_output=True, text=True, timeout=timeout + 5,
            )
            if result.returncode != 0:
                raise RuntimeError(f"curl rc={result.returncode}: {result.stderr[:200]}")

            response_data = json.loads(result.stdout)
            if "error" in response_data and "choices" not in response_data:
                raise RuntimeError(f"API error: {response_data['error']}")

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
                }
                meta.update(_task_context())
                self._put(f"traces/{call_id}/meta", meta)
                self._append_to_index(call_id)

            _log_warn(f"[#{seq}] {call_id} OK latency={latency_ms}ms finish={response_data.get('choices',[{}])[0].get('finish_reason','?')}")
            return response_data

        except Exception as e:
            latency_ms = int((time.time() - start_time) * 1000)
            if self._client:
                error_meta = {
                    "model": model, "latency_ms": latency_ms, "timestamp": timestamp,
                    "error": str(e),
                }
                error_meta.update(_task_context())
                self._put(f"traces/{call_id}/meta", error_meta)
                self._append_to_index(call_id)
            _log_warn(f"[#{seq}] {call_id} FAILED after {latency_ms}ms: {e}")
            return {"error": f"LLM call failed: {e}", "_llm_error": True}

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
             timeout: int = 120) -> dict:
    """Call LLM API with full trace recording. Returns OpenAI-compatible dict."""
    return get_recorder().trace_call(
        messages, model, base_url, api_key, tools,
        max_completion_tokens, timeout)
