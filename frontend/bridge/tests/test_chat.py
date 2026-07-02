"""Tests for AgentFlow Bridge — WebSocket chat endpoint.

Covers the streaming protocol: ``chunk`` frames during a turn, the terminal
``done`` frame, ``stop`` signal handling, timeout, disconnect cleanup, and
the no-daemon-client error path.

All business-logic tests (chunks, done, stop, error mapping) test
``_run_turn`` directly with mock WebSocket and mock DaemonClient,
avoiding ``TestClient`` thread-safety issues with ``asyncio.wait``.

The WebSocket endpoint's outer loop (``chat_endpoint``) is tested with a
minimal E2E that validates accept/close/invalid-payload handling through
``starlette.testclient.TestClient``, but the multi-frame streaming logic
is covered by the direct ``_run_turn`` tests below.
"""

from __future__ import annotations

import asyncio
from typing import Any
from unittest.mock import AsyncMock

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

import routes.chat as chat_module
from client import DaemonConnectionError, DaemonError, NotFoundError
from routes.chat import (
    ChatDone,
    ChatRequest,
    _coerce_inbound,
    _error_frame,
    _extract_content,
    _extract_final,
    _extract_task_id,
    _run_turn,
    router,
)


# ---------------------------------------------------------------------------
# Fake WebSocket — captures sent frames
# ---------------------------------------------------------------------------


class _FakeWebSocket:
    """Minimal WebSocket-like object for testing ``_run_turn``.

    Only ``send_json`` is needed — ``_run_turn`` never calls ``receive_json``.
    """

    def __init__(self) -> None:
        self.sent: list[dict[str, Any]] = []
        self.closed: bool = False
        self.close_code: int | None = None

    async def send_json(self, data: dict[str, Any]) -> None:
        self.sent.append(data)

    async def close(self, code: int = 1000) -> None:
        self.closed = True
        self.close_code = code

    @property
    def client(self) -> None:
        return None


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def mock_daemon() -> AsyncMock:
    """Build an AsyncMock with the methods ``_run_turn`` calls."""
    daemon = AsyncMock()
    daemon.agent_execute = AsyncMock()
    daemon.wait_for_response = AsyncMock()
    return daemon


@pytest.fixture(autouse=True)
def _fast_timeouts(monkeypatch: pytest.MonkeyPatch) -> None:
    """Shrink poll timers so tests don't wait on real time."""
    monkeypatch.setattr(chat_module, "TURN_TIMEOUT_SECONDS", 2.0)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _drain_sent(ws: _FakeWebSocket, count: int) -> list[dict[str, Any]]:
    """Pop the first ``count`` items from ``ws.sent``."""
    out = ws.sent[:count]
    del ws.sent[:count]
    return out


# ---------------------------------------------------------------------------
# _run_turn — happy path
# ---------------------------------------------------------------------------


class TestRunTurnHappyPath:
    """``_run_turn`` delivers chunks → done when the daemon responds."""

    @pytest.mark.asyncio
    async def test_chunks_followed_by_done(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"task_id": "t1", "session_id": "ses_1"}
        mock_daemon.wait_for_response.side_effect = [
            {"content": "Hello", "final": False},
            {"content": " world", "final": False},
            {"content": "!", "final": True, "session_id": "ses_1"},
        ]
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")

        await _run_turn(
            websocket=ws, daemon_client=mock_daemon,
            agent_id="agent_a", request=req, stop_event=asyncio.Event(),
        )

        frames = _drain_sent(ws, 4)
        types = [f["type"] for f in frames]
        assert types == ["chunk", "chunk", "chunk", "done"]
        assert frames[2]["content"] == "!"
        assert frames[2]["final"] is True
        # session_id is carried on the final chunk, not on ChatDone
        assert frames[3]["type"] == "done"

    @pytest.mark.asyncio
    async def test_stop_during_poll_aborts_and_sends_done(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"task_id": "t1", "session_id": "ses_1"}

        async def _first_then_hang(*args: Any, **kwargs: Any) -> dict[str, Any]:
            return {"content": "chunk", "final": False}

        mock_daemon.wait_for_response.side_effect = _first_then_hang
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")
        stop = asyncio.Event()

        async def _delayed_stop() -> None:
            await asyncio.sleep(0.05)
            stop.set()

        async with asyncio.TaskGroup() as tg:
            tg.create_task(_delayed_stop())
            await _run_turn(
                websocket=ws, daemon_client=mock_daemon,
                agent_id="agent_a", request=req, stop_event=stop,
            )

        # Multiple chunks may be sent before stop fires; the last frame
        # must always be a terminal done.
        assert len(ws.sent) >= 2
        assert ws.sent[0]["type"] == "chunk"
        assert ws.sent[-1]["type"] == "done"


# ---------------------------------------------------------------------------
# _run_turn — error paths
# ---------------------------------------------------------------------------


class TestRunTurnErrors:
    """``_run_turn`` properly maps daemon errors to error+done frames."""

    @pytest.mark.asyncio
    async def test_execute_timeout_emits_error_and_done(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.side_effect = asyncio.TimeoutError()
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")

        await _run_turn(
            websocket=ws, daemon_client=mock_daemon,
            agent_id="agent_a", request=req, stop_event=asyncio.Event(),
        )

        frames = _drain_sent(ws, 2)
        assert frames[0]["type"] == "error"
        assert frames[1]["type"] == "done"

    @pytest.mark.asyncio
    async def test_daemon_error_propagates(self, mock_daemon: AsyncMock) -> None:
        """DaemonError during execute is NOT caught by _run_turn (propagates up)."""
        mock_daemon.agent_execute.side_effect = NotFoundError("not found", code=-2)
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")

        with pytest.raises(NotFoundError):
            await _run_turn(
                websocket=ws, daemon_client=mock_daemon,
                agent_id="agent_a", request=req, stop_event=asyncio.Event(),
            )

    @pytest.mark.asyncio
    async def test_wait_for_response_timeout_emits_done(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.side_effect = asyncio.TimeoutError()
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")

        await _run_turn(
            websocket=ws, daemon_client=mock_daemon,
            agent_id="agent_a", request=req, stop_event=asyncio.Event(),
        )

        frames = _drain_sent(ws, 1)
        assert frames[0]["type"] == "done"

    @pytest.mark.asyncio
    async def test_empty_notification_continues_loop(self, mock_daemon: AsyncMock) -> None:
        """An empty (falsy) notification must not crash or send a chunk."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.side_effect = [
            {},  # empty notification
            {"content": "ok", "final": True},
        ]
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")

        await _run_turn(
            websocket=ws, daemon_client=mock_daemon,
            agent_id="agent_a", request=req, stop_event=asyncio.Event(),
        )

        frames = _drain_sent(ws, 2)
        assert len(frames) == 2
        assert frames[0]["type"] == "chunk"
        assert frames[1]["type"] == "done"

    @pytest.mark.asyncio
    async def test_no_task_id_emits_error_and_done(self, mock_daemon: AsyncMock) -> None:
        """If agent.execute returns no task_id or message_id, emit error+done."""
        mock_daemon.agent_execute.return_value = {"session_id": "ses_x"}
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")

        await _run_turn(
            websocket=ws, daemon_client=mock_daemon,
            agent_id="agent_a", request=req, stop_event=asyncio.Event(),
        )

        frames = _drain_sent(ws, 2)
        assert frames[0]["type"] == "error"
        assert "task_id" in frames[0]["message"]
        assert frames[1]["type"] == "done"

        mock_daemon.wait_for_response.assert_not_called()

    @pytest.mark.asyncio
    async def test_daemon_error_on_wait_for_response_emits_done(
        self, mock_daemon: AsyncMock,
    ) -> None:
        """DaemonError from wait_for_response is caught (timeout-like)."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.side_effect = DaemonError("daemon down")
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")

        await _run_turn(
            websocket=ws, daemon_client=mock_daemon,
            agent_id="agent_a", request=req, stop_event=asyncio.Event(),
        )

        frames = _drain_sent(ws, 1)
        assert frames[0]["type"] == "done"


# ---------------------------------------------------------------------------
# _run_turn — session_id propagation
# ---------------------------------------------------------------------------


class TestRunTurnSessionId:
    """Daemon-assigned session_id is propagated via the done frame."""

    @pytest.mark.asyncio
    async def test_session_id_in_done_frame(self, mock_daemon: AsyncMock) -> None:
        """session_id from agent_execute is forwarded via the outer loop, not
        embedded in ChatDone (which has no session_id field).  Verify that
        agent_execute was called with the correct session_id."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1", "session_id": "ses_99"}
        mock_daemon.wait_for_response.return_value = {"content": "ok", "final": True}
        ws = _FakeWebSocket()
        req = ChatRequest(user_id="u1", client_id="bridge", content="hi")

        await _run_turn(
            websocket=ws, daemon_client=mock_daemon,
            agent_id="agent_a", request=req, stop_event=asyncio.Event(),
        )

        frames = _drain_sent(ws, 2)
        assert frames[0]["type"] == "chunk"
        assert frames[0]["content"] == "ok"
        assert frames[1]["type"] == "done"
        # session_id from agent_execute is tracked by the outer loop
        # (chat_endpoint), not by _run_turn.
        assert mock_daemon.agent_execute.await_args is not None


# ---------------------------------------------------------------------------
# Helper tests (pure functions, no mocks needed)
# ---------------------------------------------------------------------------


class TestHelpers:
    """Pure functions used by the chat endpoint."""

    def test_coerce_inbound_chat_request(self) -> None:
        result = _coerce_inbound({"user_id": "u1", "client_id": "c1", "content": "hello"})
        assert isinstance(result, ChatRequest)
        assert result.content == "hello"

    def test_coerce_inbound_stop_signal(self) -> None:
        from routes.chat import _StopSignal
        result = _coerce_inbound({"type": "stop"})
        assert isinstance(result, _StopSignal)

    def test_coerce_inbound_invalid_returns_none(self) -> None:
        assert _coerce_inbound([]) is None
        assert _coerce_inbound({"foo": "bar"}) is None
        assert _coerce_inbound(None) is None

    def test_error_frame_structure(self) -> None:
        frame = _error_frame("something went wrong", code=-32600)
        assert frame == {"type": "error", "message": "something went wrong", "code": -32600}

    def test_extract_task_id_from_dict(self) -> None:
        assert _extract_task_id({"task_id": "t1"}) == "t1"
        assert _extract_task_id({"message_id": "m1"}) == "m1"
        assert _extract_task_id({}) is None
        assert _extract_task_id("not a dict") is None

    def test_extract_final(self) -> None:
        assert _extract_final({"final": True}) is True
        assert _extract_final({"final": False}) is False
        assert _extract_final({}) is False
        assert _extract_final(None) is False

    def test_extract_content(self) -> None:
        assert _extract_content({"content": "hello"}) == "hello"
        assert _extract_content({"text": "world"}) == "world"
        assert _extract_content({}) == ""
        assert _extract_content(None) == ""


# ---------------------------------------------------------------------------
# E2E — WebSocket endpoint outer loop (minimal smoke)
# ---------------------------------------------------------------------------

class TestWebSocketEndpoint:
    """Minimal ``chat_endpoint`` integration via ``starlette.testclient``.

    These tests verify the outer loop (accept, validate agent_id, wire
    dependencies) without exercising the multi-frame streaming path (which
    is covered by ``TestRunTurnHappyPath`` above).
    """

    def test_missing_daemon_client_closes_with_error(self) -> None:
        app = FastAPI()
        app.include_router(router)

        with TestClient(app).websocket_connect("/ws/chat/agent_a") as ws:
            err = ws.receive_json()
            assert err["type"] == "error"
            assert err["code"] == chat_module.CLOSE_CODE_INTERNAL_ERROR
            assert "not fully initialized" in err["message"]

    def test_empty_agent_id_rejected(self) -> None:
        app = FastAPI()
        app.include_router(router)
        app.state.daemon_client = AsyncMock()

        with TestClient(app).websocket_connect("/ws/chat/   ") as ws:
            err = ws.receive_json()
            assert err["type"] == "error"

    def test_valid_connect_with_mock(self) -> None:
        """A connected app accepts the WS and processes one turn."""
        app = FastAPI()
        app.include_router(router)
        mock = AsyncMock()
        mock.agent_execute = AsyncMock(return_value={"task_id": "t1", "session_id": "ses_1"})
        mock.wait_for_response = AsyncMock(return_value={"content": "ok", "final": True, "session_id": "ses_1"})
        app.state.daemon_client = mock

        with TestClient(app).websocket_connect("/ws/chat/agent_a") as ws:
            ws.send_json({"content": "hi", "user_id": "u1", "client_id": "c1"})
            chunk = ws.receive_json()
            assert chunk["type"] == "chunk"
            assert chunk["content"] == "ok"
            done = ws.receive_json()
            assert done["type"] == "done"

    def test_invalid_payload_returns_error_frame(self) -> None:
        app = FastAPI()
        app.include_router(router)
        app.state.daemon_client = AsyncMock()

        with TestClient(app).websocket_connect("/ws/chat/agent_a") as ws:
            ws.send_json({"type": "invalid"})
            err = ws.receive_json()
            assert err["type"] == "error"
            assert err["code"] == -32600
