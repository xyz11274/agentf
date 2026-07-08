"""
TDD: Red phase — test that ``_pull_and_save_traces`` persists LLM trace data
from the daemon's DataBus to the SQLite-backed SessionProvider.

The production database at ``data/sessions.db`` has 124 dialogue rounds
but **zero** trace records, even though ``save_dialogue`` works fine.
This test validates the trace-pulling code path end-to-end so we can
diagnose why traces are not persisted in production.

Invariants:
  I-1: _pull_and_save_traces saves one row per LLM call in the DataBus.
  I-2: The saved trace_data preserves call_id, request, response, and meta.
  I-3: Getting traces back via provider.get_traces() returns the same data.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from client import DaemonClient
from providers import LocalSessionProvider
from services.trace_puller import _pull_and_save_traces


# ---------------------------------------------------------------------------
# Mock DaemonClient — responds to DataBus RPC with canned trace data
# ---------------------------------------------------------------------------


class _MockDaemonClient(DaemonClient):
    """DaemonClient stub that answers ``databus.list_keys`` and ``databus.get``
    with synthetic trace entries, simulating a daemon that has recorded
    LLM execution traces for a completed dialogue."""

    def __init__(self, trace_entries: list[dict[str, Any]]) -> None:
        self._trace_entries = trace_entries
        self._connected = True
        self._sent_requests: list[tuple[str, dict[str, Any] | None]] = []

    @property
    def is_connected(self) -> bool:
        return True

    async def connect(self) -> None:
        pass

    async def close(self) -> None:
        pass

    async def send_request(
        self, method: str, params: dict[str, Any] | None = None, **kwargs: Any,
    ) -> Any:
        self._sent_requests.append((method, params))

        if method == "databus.list_keys":
            # Return synthetic keys for each trace entry.
            prefix = (params or {}).get("prefix", "")
            keys: list[str] = []
            for entry in self._trace_entries:
                call_id = entry.get("call_id", "unknown")
                keys.append(f"{prefix}{call_id}/meta")
            return {"keys": keys}

        if method == "databus.get":
            key: str = (params or {}).get("key", "")
            # Parse call_id and sub-key from the full key path.
            # Only respond to the trace-query prefix (llm/traces/...).
            for entry in self._trace_entries:
                call_id = entry.get("call_id", "")
                if call_id and call_id in key:
                    # key format: {prefix}{call_id}/{sub_key}
                    sub_key = key.rsplit("/", 1)[-1]  # request | response | meta
                    value = entry.get(sub_key)
                    if value is not None:
                        return {"exists": True, "value": value}
                    # missing sub-key → the entry doesn't exist
                    return {"exists": False, "value": None}
            return {"exists": False, "value": None}

        if method == "databus.remove_by_prefix":
            # Return a canned response — we track the call via _sent_requests
            return {"removed": len(self._trace_entries)}

        return {}


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def provider(tmp_path: Path) -> LocalSessionProvider:
    """Fresh SQLite provider per test."""
    return LocalSessionProvider(str(tmp_path / "sessions.db"))


def _make_trace_entry(call_id: str, model: str = "gpt-4o") -> dict[str, Any]:
    """Build a synthetic trace entry matching what the daemon's
    _TraceRecorder writes to the DataBus."""
    return {
        "call_id": call_id,
        "request": {
            "model": model,
            "messages": [{"role": "user", "content": "Hello"}],
            "max_tokens": 512,
        },
        "response": {
            "choices": [{"message": {"content": "Hi there!"}}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
        },
        "meta": {
            "model": model,
            "latency_ms": 1234,
            "request_summary": "Hello",
            "response_summary": "Hi there!",
            "timestamp": "2026-07-05T12:00:00Z",
            "prompt_tokens": 10,
            "completion_tokens": 5,
            "total_tokens": 15,
            "dialogue_id": "d-1",
            "session_id": "mock-ses-001",
            "node_id": "coordinator_decide",
            "task_id": "task-001",
            "skill_name": "",
            "completed_skill": "",
            "task_type": "coordinator",
        },
    }


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_pull_and_save_traces_persists_one_entry(provider: LocalSessionProvider) -> None:
    """I-1: one LLM call in DataBus → one row in the traces table."""
    daemon = _MockDaemonClient([
        _make_trace_entry("call-001"),
    ])
    sess = await provider.get_or_create_session("alice", "web")

    count = await _pull_and_save_traces(
        daemon, provider,
        agent_id="test-agent",
        user_id="alice",
        client_id="web",
        dialogue_id="d-1",
        session_id=sess["id"],
    )

    assert count == 1, f"Expected 1 persisted trace, got {count}"

    # Verify via the provider read path
    traces = await provider.get_traces(sess["id"], dialogue_id="d-1")
    assert len(traces) == 1
    assert traces[0]["trace_type"] == "llm_call"
    assert traces[0]["dialogue_id"] == "d-1"
    # trace_data.call_id must be preserved (I-2)
    assert traces[0]["trace_data"]["call_id"] == "call-001"
    assert traces[0]["trace_data"]["meta"]["model"] == "gpt-4o"
    assert traces[0]["trace_data"]["meta"]["latency_ms"] == 1234


@pytest.mark.asyncio
async def test_pull_and_save_traces_persists_multiple_entries(
    provider: LocalSessionProvider,
) -> None:
    """I-1: multiple LLM calls in DataBus → multiple rows."""
    daemon = _MockDaemonClient([
        _make_trace_entry("call-001", model="gpt-4o"),
        _make_trace_entry("call-002", model="gpt-4o-mini"),
    ])
    sess = await provider.get_or_create_session("bob", "mobile")

    count = await _pull_and_save_traces(
        daemon, provider,
        agent_id="test-agent",
        user_id="bob",
        client_id="mobile",
        dialogue_id="d-1",
        session_id=sess["id"],
    )

    assert count == 2, f"Expected 2 persisted traces, got {count}"

    traces = await provider.get_traces(sess["id"], dialogue_id="d-1")
    assert len(traces) == 2
    # Each call_id must be preserved (I-2)
    call_ids = [t["trace_data"]["call_id"] for t in traces]
    assert "call-001" in call_ids
    assert "call-002" in call_ids


@pytest.mark.asyncio
async def test_pull_and_save_traces_empty_daemon(provider: LocalSessionProvider) -> None:
    """No traces in daemon DataBus → 0 persisted, no crash."""
    daemon = _MockDaemonClient([])  # no trace entries
    sess = await provider.get_or_create_session("carol", "web")

    count = await _pull_and_save_traces(
        daemon, provider,
        agent_id="test-agent",
        user_id="carol",
        client_id="web",
        dialogue_id="d-1",
        session_id=sess["id"],
    )

    assert count == 0

    traces = await provider.get_traces(sess["id"], dialogue_id="d-1")
    assert len(traces) == 0


@pytest.mark.asyncio
async def test_pull_and_save_traces_works_without_agentflow_session_id(
    provider: LocalSessionProvider,
) -> None:
    """Empty agentflow_session_id → still pulls via ``session_id="0"`` fallback."""
    daemon = _MockDaemonClient([_make_trace_entry("call-001")])
    sess = await provider.get_or_create_session("dave", "web")

    count = await _pull_and_save_traces(
        daemon, provider,
        agent_id="test-agent",
        user_id="dave",
        client_id="web",
        dialogue_id="d-1",
        session_id=sess["id"],
    )

    assert count == 1, f"Expected 1 persisted trace with empty agentflow_session_id, got {count}"
    traces = await provider.get_traces(sess["id"])
    assert len(traces) == 1


@pytest.mark.asyncio
async def test_pull_and_save_traces_persisted_data_roundtrip(
    provider: LocalSessionProvider,
) -> None:
    """I-2 & I-3: trace_data preserves nested fields (request, response, meta)."""
    daemon = _MockDaemonClient([_make_trace_entry("call-roundtrip")])
    sess = await provider.get_or_create_session("eve", "web")

    count = await _pull_and_save_traces(
        daemon, provider,
        agent_id="test-agent",
        user_id="eve",
        client_id="web",
        dialogue_id="d-roundtrip",
        session_id=sess["id"],
    )

    assert count == 1

    traces = await provider.get_traces(sess["id"], dialogue_id="d-roundtrip")
    assert len(traces) == 1

    td = traces[0]["trace_data"]
    # Full roundtrip: request, response, meta all preserved
    assert td["call_id"] == "call-roundtrip"
    assert td["request"]["model"] == "gpt-4o"
    assert td["response"]["choices"][0]["message"]["content"] == "Hi there!"
    assert td["meta"]["latency_ms"] == 1234
    assert td["meta"]["prompt_tokens"] == 10
    assert td["meta"]["completion_tokens"] == 5
    assert td["meta"]["total_tokens"] == 15


@pytest.mark.asyncio
async def test_pull_and_save_traces_requires_dialogue_id(
    provider: LocalSessionProvider,
) -> None:
    """Empty dialogue_id → 0 persisted (dialogue_id is always required)."""
    daemon = _MockDaemonClient([_make_trace_entry("call-001")])
    sess = await provider.get_or_create_session("frank", "web")

    count = await _pull_and_save_traces(
        daemon, provider,
        agent_id="test-agent",
        user_id="frank",
        client_id="web",
        dialogue_id="",
        session_id=sess["id"],
    )

    assert count == 0
    traces = await provider.get_traces(sess["id"])
    assert len(traces) == 0


@pytest.mark.asyncio
async def test_pull_and_save_traces_cleans_up_databus_after_success(
    provider: LocalSessionProvider,
) -> None:
    """I-1 + cleanup: persisted > 0 → databus.remove_by_prefix is called."""
    daemon = _MockDaemonClient([_make_trace_entry("call-001")])
    sess = await provider.get_or_create_session("grace", "web")

    count = await _pull_and_save_traces(
        daemon, provider,
        agent_id="test-agent",
        user_id="grace",
        client_id="web",
        dialogue_id="d-cleanup",
        session_id=sess["id"],
    )

    assert count == 1, f"Expected 1 trace, got {count}"

    # Verify databus.remove_by_prefix was called with the right prefix
    cleanup_calls = [
        (m, p) for m, p in daemon._sent_requests  # noqa: SLF001
        if m == "databus.remove_by_prefix"
    ]
    assert len(cleanup_calls) == 1, (
        f"Expected 1 databus.remove_by_prefix call, got {len(cleanup_calls)}"
    )
    _, params = cleanup_calls[0]
    assert params is not None
    assert params.get("prefix") == "llm/traces/", (
        f"Expected prefix='llm/traces/', got {params.get('prefix')!r}"
    )
    assert params.get("agent_id") == "test-agent"


@pytest.mark.asyncio
async def test_pull_and_save_traces_skips_cleanup_on_empty(
    provider: LocalSessionProvider,
) -> None:
    """persisted == 0 → databus.remove_by_prefix is NOT called."""
    daemon = _MockDaemonClient([])  # no trace entries
    sess = await provider.get_or_create_session("heidi", "web")

    count = await _pull_and_save_traces(
        daemon, provider,
        agent_id="test-agent",
        user_id="heidi",
        client_id="web",
        dialogue_id="d-empty",
        session_id=sess["id"],
    )

    assert count == 0

    cleanup_calls = [
        (m, p) for m, p in daemon._sent_requests  # noqa: SLF001
        if m == "databus.remove_by_prefix"
    ]
    assert len(cleanup_calls) == 0, (
        f"Expected 0 cleanup calls when persisted=0, got {len(cleanup_calls)}"
    )
