"""
LLM Trace Recorder — records LLM call execution traces to DataBus.

Each ``call_llm()`` invocation produces three keys under the ``llm`` namespace:

  traces/{call_id}/request   — HTTP request body (JSON) — full, no truncation
  traces/{call_id}/response  — HTTP response body (JSON) — full, no truncation
  traces/{call_id}/meta      — metadata + summaries for list view

An index key ``traces/index`` maintains an ordered list of all call_ids
so the Bridge can enumerate traces without ``list_keys``.

Usage::

    recorder = TraceRecorder()
    result = recorder.trace_call(messages, model, base_url, api_key, tools=tools)
    if result.get("_llm_error"):
        ...  # handle error

All DataBus write failures are silently swallowed — trace recording never
interrupts the LLM call flow.
"""

import json
import logging
import os
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

from agentflow_databus import DataBusClient as _RealDataBusClient
from agentflow_databus import DataBusError

logger = logging.getLogger("llm_trace_recorder")


class TraceRecorder:
    """Records LLM call trace data into DataBus.

    Gracefully handles missing DataBus — all errors are logged and swallowed.
    """

    def __init__(self):
        self._session_id = os.environ.get("AF_SESSION_ID", "")
        self._uds_path = os.environ.get("AF_DATABUS_UDS_FILE", "")
        self._client: _RealDataBusClient | None = None
        self._call_count = 0  # monotonic counter for logging

        if self._uds_path:
            try:
                self._client = _RealDataBusClient(
                    self._uds_path,
                    session_id=self._session_id,
                    timeout_seconds=5.0,
                )
                logger.info("connected to DataBus at %s (session=%s)",
                            self._uds_path, self._session_id)
            except (DataBusError, OSError) as exc:
                logger.warning("DataBus init failed at %s: %s", self._uds_path, exc)
                self._client = None
        else:
            logger.info("AF_DATABUS_UDS_FILE not set — trace recording disabled")

    @property
    def is_connected(self) -> bool:
        return self._client is not None

    # ── Session message persistence (multi-turn) ─────────────────────
    # Stored in the session-persistent ``llm`` namespace so data survives
    # across workflow executions within the same session.
    _SESSION_MSG_KEY = "session_messages"

    def read_session_messages(self) -> list:
        """Read conversation history from session-persistent storage."""
        if not self._client:
            return []
        try:
            raw = self._client.llm().get(self._SESSION_MSG_KEY, "[]")
            if raw is None or raw == "":
                return []
            # SDK may return bytes or string; normalize to string
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8")
            if isinstance(raw, str):
                return json.loads(raw)
            # Already a list/dict (SDK may auto-deserialize)
            if isinstance(raw, list):
                return raw
            return []
        except Exception as exc:
            logger.warning("read_session_messages failed: %s", exc)
            return []

    def write_session_messages(self, messages: list) -> bool:
        """Write conversation history to session-persistent storage."""
        if not self._client:
            return False
        try:
            # Pass list directly — SDK's put() handles serialization
            # (same pattern as _put which passes dict, not pre-serialized string)
            self._client.llm().put(self._SESSION_MSG_KEY, messages)
            logger.info("saved %d messages for next turn", len(messages))
            return True
        except Exception as exc:
            logger.warning("write_session_messages failed: %s", exc)
            return False

    def trace_call(self, messages: list, model: str,
                   base_url: str, api_key: str,
                   tools: list | None = None,
                   iteration: int | None = None) -> dict:
        """Execute an LLM call with full trace recording.

        Parameters
        ----------
        messages : list[dict]
            The messages list to send to the LLM (will be deep-copied for the
            trace before the HTTP call, so the caller may safely mutate it
            afterwards without affecting the trace).
        model : str
            Model identifier (e.g. "gpt-4o-mini").
        base_url : str
            OpenAI-compatible API base URL.
        api_key : str
            API key.
        tools : list[dict] | None
            OpenAI-compatible tool definitions.
        iteration : int | None
            Optional iteration number for multi-turn loops (included in meta).

        Returns
        -------
        dict
            The LLM API response dict, or ``{"error": ..., "_llm_error": true}``
            on failure.
        """
        self._call_count += 1
        seq = self._call_count

        if not self._client:
            logger.info("[#%d] DataBus unavailable — falling back to raw HTTP", seq)
            return self._raw_call(messages, model, base_url, api_key, tools)

        call_id = uuid.uuid4().hex[:12]
        start_time = time.time()
        timestamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        msg_count = len(messages)
        iter_info = f"iter={iteration}" if iteration is not None else "single"
        logger.info(
            "[#%d] %s call_id=%s model=%s messages=%d tools=%s",
            seq, iter_info, call_id, model, msg_count,
            bool(tools))
        # Log message roles + snippet for debugging multi-turn
        for mi, m in enumerate(messages):
            role = m.get("role", "?")
            content = m.get("content", "")
            snippet = content[:80].replace("\n", " ") if content else "(tool_calls)" if m.get("tool_calls") else "(empty)"
            logger.info("  msg[%d] role=%s: %s", mi, role, snippet)

        # --- Build & record request body (full, no truncation) ---
        request_body = self._build_request_body(messages, model, tools)
        self._put(f"traces/{call_id}/request", request_body)

        # --- Execute LLM call ---
        import requests as http_requests
        url = base_url.rstrip("/")
        if not url.endswith("/chat/completions"):
            url += "/chat/completions"
        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        }
        try:
            resp = http_requests.post(
                url, json=request_body, headers=headers, timeout=60
            )
            resp.raise_for_status()
            response_data = resp.json()
            latency_ms = int((time.time() - start_time) * 1000)

            # --- Record full response ---
            self._put(f"traces/{call_id}/response", response_data)

            # --- Build meta (summaries are truncated for list-view only) ---
            usage = response_data.get("usage", {})
            choice = response_data.get("choices", [{}])[0]
            finish_reason = choice.get("finish_reason", "unknown")
            meta = {
                "model": model,
                "latency_ms": latency_ms,
                "timestamp": timestamp,
                "request_summary": self._summarize_messages(messages),
                "response_summary": self._summarize_response(response_data),
                "prompt_tokens": usage.get("prompt_tokens", 0),
                "completion_tokens": usage.get("completion_tokens", 0),
                "total_tokens": usage.get("total_tokens", 0),
                "iteration": iteration,
                "finish_reason": finish_reason,
                "msg_count": msg_count,
                "tools_enabled": bool(tools),
            }
            self._put(f"traces/{call_id}/meta", meta)

            # Update index
            self._append_to_index(call_id)

            logger.info(
                "[#%d] %s OK latency=%dms tokens=%d finish=%s",
                seq, call_id, latency_ms,
                usage.get("total_tokens", 0), finish_reason)

            return response_data

        except Exception as e:
            latency_ms = int((time.time() - start_time) * 1000)
            error_meta = {
                "model": model,
                "latency_ms": latency_ms,
                "timestamp": timestamp,
                "error": str(e),
                "iteration": iteration,
            }
            self._put(f"traces/{call_id}/meta", error_meta)
            self._append_to_index(call_id)

            logger.error("[#%d] %s FAILED after %dms: %s: %s",
                         seq, call_id, latency_ms, type(e).__name__, e)

            return {"error": f"LLM call failed: {e}", "_llm_error": True}

    def _raw_call(self, messages: list, model: str,
                  base_url: str, api_key: str,
                  tools: list | None = None) -> dict:
        """Fallback: call LLM without tracing (DataBus unavailable)."""
        import requests as http_requests
        url = base_url.rstrip("/")
        if not url.endswith("/chat/completions"):
            url += "/chat/completions"
        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        }
        body = self._build_request_body(messages, model, tools)
        try:
            resp = http_requests.post(url, json=body, headers=headers, timeout=60)
            resp.raise_for_status()
            return resp.json()
        except Exception as e:
            return {"error": f"LLM call failed: {e}", "_llm_error": True}

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _build_request_body(messages: list, model: str,
                            tools: list | None = None) -> dict:
        """Build the HTTP request body (full messages, no truncation)."""
        body = {
            "model": model,
            "messages": messages,
            "max_completion_tokens": 1000,
        }
        if tools:
            body["tools"] = tools
            body["tool_choice"] = "auto"
        return body

    def _put(self, key: str, value) -> None:
        """Write to DataBus ``llm`` namespace; swallow errors."""
        if not self._client:
            return
        try:
            self._client.llm().put(key, value)
        except Exception as exc:
            logger.warning("put '%s' failed: %s", key, exc)
            traceback.print_exc(file=sys.stderr)

    def _append_to_index(self, call_id: str) -> None:
        """Append a call_id to the traces/index list."""
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
            logger.warning("update traces/index failed: %s", exc)

    @staticmethod
    def _summarize_messages(messages: list) -> str:
        """Short summary of the message list for list-view display."""
        parts = []
        for m in messages:
            role = m.get("role", "?")
            content = m.get("content", "")
            if isinstance(content, str):
                # Truncate to 120 chars for readability, full data in /request
                content = content[:120].replace("\n", " ")
            # For tool results, show a short marker instead of full content
            if role == "tool":
                content = content[:60]
            parts.append(f"{role}: {content}")
        summary = " | ".join(parts[:8])
        if len(parts) > 8:
            summary += f" ... (+{len(parts) - 8} more)"
        return summary

    @staticmethod
    def _summarize_response(response: dict) -> str:
        """Short summary of the LLM response for list-view display."""
        choices = response.get("choices", [])
        if not choices:
            return "(no choices)"
        choice = choices[0]
        msg = choice.get("message", {})
        finish_reason = choice.get("finish_reason", "?")
        content = msg.get("content", "")
        if isinstance(content, str) and content:
            truncated = content[:200].replace("\n", " ")
            return f"[{finish_reason}] {truncated}"
        tool_calls = msg.get("tool_calls")
        if tool_calls:
            names = [tc.get("function", {}).get("name", "?") for tc in tool_calls]
            return f"[{finish_reason}] tool_calls: {', '.join(names)}"
        return f"[{finish_reason}] (empty)"
