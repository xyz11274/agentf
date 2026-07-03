"""Tests for DaemonClient message dispatch (WebSocket JSON-RPC).

These tests replaced the old raw-TCP frame-reassembly tests after the
transport was switched from newline-delimited TCP to WebSocket (RFC 6455).
WebSocket provides complete text frames, so no manual ``\\n`` delimiter
handling, buffering, or ``_looks_like_complete_json`` heuristics are needed.

What we keep testing:
  1. ``_looks_like_complete_json`` — standalone utility, still exported.
  2. ``_handle_message`` — the JSON-RPC dispatch kernel (response → future,
     notification → queue).  Transport-agnostic, runs against a bare instance.

What was removed (TCP-specific, no longer applicable):
  - ``TestDispatchFrameConcatenated`` (raw_decode over glued JSON objects)
  - ``TestReaderLoopReassembly`` (readline + buf + \\n delimiter logic)
"""

from __future__ import annotations

import asyncio
import json
from typing import List, Optional

import pytest

from client import DaemonClient, _looks_like_complete_json


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _new_future() -> asyncio.Future:
    loop = asyncio.get_running_loop()
    fut: asyncio.Future = loop.create_future()
    return fut


async def _drain_notifications(cli: DaemonClient, count: int, timeout: float = 1.0) -> List[dict]:
    """Collect up to ``count`` notifications from the queue."""
    out = []
    try:
        for _ in range(count):
            msg = await asyncio.wait_for(cli._notification_queue.get(), timeout=timeout)
            out.append(msg)
    except asyncio.TimeoutError:
        pass
    return out


async def _resolve_future(cli: DaemonClient, req_id: int, timeout: float = 1.0) -> Optional[dict]:
    """Pop a resolved future response by id."""
    fut = cli._pending.pop(req_id, None)
    if fut is None or not fut.done():
        return None
    try:
        return await asyncio.wait_for(fut, timeout=timeout)
    except asyncio.TimeoutError:
        return None


# ---------------------------------------------------------------------------
# Tests — _looks_like_complete_json (standalone utility)
# ---------------------------------------------------------------------------


class TestLooksLikeCompleteJson:
    """Unit tests for the JSON-completeness heuristic."""

    def test_complete_object(self):
        assert _looks_like_complete_json('{"a": 1}') is True

    def test_complete_array(self):
        assert _looks_like_complete_json('[1, 2, 3]') is True

    def test_empty_string_is_complete(self):
        assert _looks_like_complete_json("") is True

    def test_truncated_tail_from_logs(self):
        # Real log sample: ',"id":52}' — the orphaned tail of a split frame
        assert _looks_like_complete_json(',"id":52}') is False

    def test_unbalanced_brace(self):
        assert _looks_like_complete_json('{"jsonrpc":"2.0","result":{"x":1}') is False

    def test_brace_inside_string_not_counted(self):
        # A '}' inside a JSON string must not close the object
        assert _looks_like_complete_json('{"a": "}"}') is True

    def test_escaped_quote_in_string(self):
        assert _looks_like_complete_json('{"a": "\\""}') is True

    def test_garbage_prefix_is_incomplete(self):
        assert _looks_like_complete_json("not json at all") is False


# ---------------------------------------------------------------------------
# Tests — _handle_message (JSON-RPC dispatch, transport-agnostic)
# ---------------------------------------------------------------------------


class TestMessageDispatch:
    """Verifies the JSON-RPC dispatch logic used by both TCP and WebSocket."""

    @pytest.mark.asyncio
    async def test_response_resolves_pending_future(self):
        cli = DaemonClient.__new__(DaemonClient)
        cli._notification_queue = asyncio.Queue()
        cli._pending = {}

        fut = _new_future()
        cli._pending[42] = fut
        await cli._handle_message({"jsonrpc": "2.0", "result": {"ok": True}, "id": 42})

        assert fut.done()
        assert fut.result()["id"] == 42

    @pytest.mark.asyncio
    async def test_notification_queued(self):
        cli = DaemonClient.__new__(DaemonClient)
        cli._notification_queue = asyncio.Queue()
        cli._pending = {}

        await cli._handle_message({
            "jsonrpc": "2.0", "method": "agent.response",
            "params": {"content": "hello", "final": True},
        })
        notifs = await _drain_notifications(cli, 1, timeout=0.3)
        assert len(notifs) == 1
        assert notifs[0]["method"] == "agent.response"

    @pytest.mark.asyncio
    async def test_unknown_notification_ignored(self):
        cli = DaemonClient.__new__(DaemonClient)
        cli._notification_queue = asyncio.Queue()
        cli._pending = {}

        await cli._handle_message({
            "jsonrpc": "2.0", "method": "some.event",
            "params": {"x": 1},
        })
        notifs = await _drain_notifications(cli, 1, timeout=0.2)
        assert len(notifs) == 0

    @pytest.mark.asyncio
    async def test_response_with_no_pending_future_silently_dropped(self):
        """A response for a request id that has no pending future must not crash."""
        cli = DaemonClient.__new__(DaemonClient)
        cli._notification_queue = asyncio.Queue()
        cli._pending = {}

        # Should not raise
        await cli._handle_message({"jsonrpc": "2.0", "result": {"ok": True}, "id": 999})

    @pytest.mark.asyncio
    async def test_non_integer_id_dropped(self):
        """A response with a non-integer id (e.g. string) must be dropped."""
        cli = DaemonClient.__new__(DaemonClient)
        cli._notification_queue = asyncio.Queue()
        cli._pending = {}
        fut = _new_future()
        cli._pending[42] = fut

        await cli._handle_message({"jsonrpc": "2.0", "result": {"ok": True}, "id": "42"})

        assert not fut.done(), "future with int id=42 should not be resolved by string-id response"

    @pytest.mark.asyncio
    async def test_response_and_notification_in_order(self):
        """A response followed by a notification — both must be handled."""
        cli = DaemonClient.__new__(DaemonClient)
        cli._notification_queue = asyncio.Queue()
        cli._pending = {}

        fut = _new_future()
        cli._pending[10] = fut

        await cli._handle_message({"jsonrpc": "2.0", "result": {"v": 1}, "id": 10})
        await cli._handle_message({
            "jsonrpc": "2.0", "method": "agent.response",
            "params": {"content": "done", "final": True},
        })

        assert fut.done()
        assert fut.result()["id"] == 10

        notifs = await _drain_notifications(cli, 1, timeout=0.3)
        assert len(notifs) == 1
        assert notifs[0]["method"] == "agent.response"

    @pytest.mark.asyncio
    async def test_concurrent_notifications_are_demultiplexed_by_dialogue(self):
        """Concurrent turns must not consume each other's agent.response."""
        cli = DaemonClient.__new__(DaemonClient)
        cli._notification_queue = asyncio.Queue()
        cli._response_queues = {
            "dialogue-a": asyncio.Queue(),
            "dialogue-b": asyncio.Queue(),
        }
        cli._pending = {}

        await cli._handle_message({
            "jsonrpc": "2.0",
            "method": "agent.response",
            "params": {
                "content": "response-b",
                "final": True,
                "metadata": {"dialogue_id": "dialogue-b"},
            },
        })
        await cli._handle_message({
            "jsonrpc": "2.0",
            "method": "agent.response",
            "params": {
                "content": "response-a",
                "final": True,
                "metadata": {"dialogue_id": "dialogue-a"},
            },
        })

        a = await cli._response_queues["dialogue-a"].get()
        b = await cli._response_queues["dialogue-b"].get()
        assert a["params"]["content"] == "response-a"
        assert b["params"]["content"] == "response-b"
        assert cli._notification_queue.empty()

    @pytest.mark.asyncio
    async def test_late_correlated_notification_is_dropped_after_queue_discard(self):
        cli = DaemonClient.__new__(DaemonClient)
        cli._notification_queue = asyncio.Queue()
        cli._response_queues = {"expired-dialogue": asyncio.Queue()}
        cli._pending = {}

        cli.discard_response_queue("expired-dialogue")
        await cli._handle_message({
            "jsonrpc": "2.0",
            "method": "agent.response",
            "params": {
                "content": "late",
                "final": True,
                "metadata": {"dialogue_id": "expired-dialogue"},
            },
        })

        assert "expired-dialogue" not in cli._response_queues
        assert cli._notification_queue.empty()
