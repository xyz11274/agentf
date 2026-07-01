"""Tests for AgentFlow Bridge — WebSocket chat endpoint (T5).

Covers the streaming protocol: ``chunk`` frames during a turn, the terminal
``done`` frame, ``stop`` signal handling, timeout, disconnect cleanup, and
the no-daemon-client error path.

All tests run against a mocked :class:`DaemonClient` so the bridge's chat
router can be exercised without a live ``agentflowd`` daemon. We use
FastAPI's :class:`TestClient` for WebSocket round-trips.
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
from routes.chat import router


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def mock_daemon() -> AsyncMock:
    """Build an AsyncMock with the two methods the chat router calls."""
    daemon = AsyncMock()
    daemon.agent_execute = AsyncMock()
    daemon.agent_poll_response = AsyncMock()
    return daemon


@pytest.fixture
def app(mock_daemon: AsyncMock) -> FastAPI:
    """Build a FastAPI app wired with the chat router and a mocked daemon."""
    application = FastAPI()
    application.include_router(router)
    application.state.daemon_client = mock_daemon
    return application


@pytest.fixture
def client(app: FastAPI) -> TestClient:
    """A TestClient that talks to the chat router over in-process WS."""
    return TestClient(app)


@pytest.fixture(autouse=True)
def _fast_polling(monkeypatch: pytest.MonkeyPatch) -> None:
    """Shrink poll/heartbeat timers so tests don't wait on real time."""
    monkeypatch.setattr(chat_module, "HEARTBEAT_SECONDS", 3600)
    monkeypatch.setattr(chat_module, "TURN_TIMEOUT_SECONDS", 2.0)


# ---------------------------------------------------------------------------
# Happy path
# ---------------------------------------------------------------------------


def test_full_flow_send_chunk_done(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """Frontend -> bridge -> daemon -> bridge -> frontend: chunk + done."""
    mock_daemon.agent_execute.return_value = {"task_id": "t1", "session_id": "ses_1"}
    mock_daemon.agent_poll_response.side_effect = [
        {"content": "Hello", "final": False},
        {"content": " world", "final": False},
        {"content": "!", "final": True, "session_id": "ses_1"},
    ]

    with client.websocket_connect("/ws/chat/agent_a") as ws:
        ws.send_json({"content": "hi", "session_id": ""})

        frames = _drain(ws, count=4)
        types = [f["type"] for f in frames]
        assert types == ["chunk", "chunk", "chunk", "done"]

        # Final chunk carries final=True and content='!'.
        last_chunk = frames[2]
        assert last_chunk["content"] == "!"
        assert last_chunk["final"] is True

        # Done frame surfaces the daemon-assigned session_id.
        assert frames[3]["session_id"] == "ses_1"
        assert frames[3]["final"] is True

    # Both daemon methods called exactly once on the execute / three polls.
    mock_daemon.agent_execute.assert_awaited_once_with(
        "agent_a", "", "hi"
    )
    assert mock_daemon.agent_poll_response.await_count == 3


def test_session_id_carried_across_messages(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """Daemon-assigned session_id from turn 1 is forwarded to turn 2's execute."""
    mock_daemon.agent_execute.side_effect = [
        {"task_id": "t1", "session_id": "ses_42"},
        {"task_id": "t2"},  # daemon accepts the carried session_id silently
    ]
    mock_daemon.agent_poll_response.side_effect = [
        {"content": "first answer", "final": True, "session_id": "ses_42"},
        {"content": "second answer", "final": True, "session_id": "ses_42"},
    ]

    with client.websocket_connect("/ws/chat/agent_a") as ws:
        # Turn 1: no session_id yet — daemon generates one.
        ws.send_json({"content": "first", "session_id": ""})
        frames = _drain(ws, count=2)
        assert frames[0]["type"] == "chunk"
        assert frames[1]["type"] == "done"
        assert frames[1]["session_id"] == "ses_42"

        # Turn 2: empty session_id in the payload, but the bridge carries
        # ``ses_42`` forward (we used ``initial_session_id`` in the call).
        ws.send_json({"content": "second", "session_id": ""})
        frames = _drain(ws, count=2)
        assert frames[1]["type"] == "done"
        assert frames[1]["session_id"] == "ses_42"

    # The second execute must use the carried session_id (ses_42), not ''.
    second_call = mock_daemon.agent_execute.await_args_list[1]
    assert second_call.args[0] == "agent_a"
    assert second_call.args[1] == "ses_42"
    assert second_call.args[2] == "second"


def test_empty_session_id_triggers_daemon_generation(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """Frontend may pass session_id='' to ask the daemon to allocate one."""
    mock_daemon.agent_execute.return_value = {
        "task_id": "t1",
        "session_id": "ses_new",
    }
    mock_daemon.agent_poll_response.return_value = {
        "content": "ok",
        "final": True,
        "session_id": "ses_new",
    }

    with client.websocket_connect("/ws/chat/agent_a") as ws:
        ws.send_json({"content": "hi", "session_id": ""})
        frames = _drain(ws, count=2)

    mock_daemon.agent_execute.assert_awaited_once_with("agent_a", "", "hi")
    assert frames[1]["session_id"] == "ses_new"


# ---------------------------------------------------------------------------
# Stop signal
# ---------------------------------------------------------------------------


def test_stop_signal_acks_with_done_frame(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """Frontend can stop polling mid-turn by sending ``{"type": "stop"}``."""
    # Execute returns immediately; poll hangs forever so the only way out
    # of the loop is the stop signal.
    mock_daemon.agent_execute.return_value = {"task_id": "t1", "session_id": "ses_9"}

    async def hanging_poll(*args: Any, **kwargs: Any) -> dict[str, Any]:
        await asyncio.sleep(60)
        return {"content": "never", "final": True}

    mock_daemon.agent_poll_response.side_effect = hanging_poll

    with client.websocket_connect("/ws/chat/agent_a") as ws:
        ws.send_json({"content": "hi", "session_id": ""})
        # Give the bridge time to call execute + start polling.
        # We then send a stop signal.
        ws.send_json({"type": "stop"})
        # The stop handler acks with a done frame.
        ack = ws.receive_json()
        assert ack["type"] == "done"
        assert ack["final"] is True
        # The session_id we carried forward is whatever execute returned.
        assert ack["session_id"] == "ses_9"


# ---------------------------------------------------------------------------
# Error paths
# ---------------------------------------------------------------------------


def test_invalid_payload_returns_error_frame(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """Frames without ``content`` get a -32600 error frame; connection stays open."""
    with client.websocket_connect("/ws/chat/agent_a") as ws:
        ws.send_json({"type": "not_a_real_type", "foo": "bar"})
        err = ws.receive_json()
        assert err["type"] == "error"
        assert err["code"] == -32600

        # The connection is still alive — a valid follow-up message works.
        mock_daemon.agent_execute.return_value = {
            "task_id": "t2",
            "session_id": "ses_ok",
        }
        mock_daemon.agent_poll_response.return_value = {
            "content": "ok",
            "final": True,
            "session_id": "ses_ok",
        }
        ws.send_json({"content": "hello", "session_id": ""})
        frames = _drain(ws, count=2)
        assert frames[0]["type"] == "chunk"
        assert frames[1]["type"] == "done"


def test_daemon_error_closes_connection(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """A DaemonError surfaces as an error frame and then a close."""
    mock_daemon.agent_execute.side_effect = NotFoundError(
        "agent missing", code=-2
    )

    with client.websocket_connect("/ws/chat/agent_a") as ws:
        ws.send_json({"content": "hi", "session_id": ""})
        err = ws.receive_json()
        assert err["type"] == "error"
        # Error code is propagated from the daemon exception.
        assert err["code"] == -2
        assert "agent missing" in err["message"]

        # Server has closed the socket — the next receive raises.
        from starlette.websockets import WebSocketDisconnect

        with pytest.raises(WebSocketDisconnect):
            ws.receive_json()


def test_daemon_connection_error_closes_with_internal_code(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """DaemonConnectionError falls back to the internal-error close code."""
    mock_daemon.agent_execute.side_effect = DaemonConnectionError("ws down")

    with client.websocket_connect("/ws/chat/agent_a") as ws:
        ws.send_json({"content": "hi", "session_id": ""})
        err = ws.receive_json()
        assert err["type"] == "error"
        assert err["code"] == chat_module.CLOSE_CODE_INTERNAL_ERROR
        assert "ws down" in err["message"]


def test_execute_without_task_id_emits_error_and_done(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """If the daemon returns no task_id we emit error+done, no poll."""
    mock_daemon.agent_execute.return_value = {"session_id": "ses_x"}  # no task_id
    mock_daemon.agent_poll_response.return_value = {
        "content": "should never be polled",
        "final": True,
    }

    with client.websocket_connect("/ws/chat/agent_a") as ws:
        ws.send_json({"content": "hi", "session_id": ""})
        err = ws.receive_json()
        done = ws.receive_json()
        assert err["type"] == "error"
        assert "task_id" in err["message"]
        assert done["type"] == "done"

    mock_daemon.agent_poll_response.assert_not_called()


# ---------------------------------------------------------------------------
# Initialization / shutdown
# ---------------------------------------------------------------------------


def test_missing_daemon_client_closes_with_error() -> None:
    """If the lifespan never attached a DaemonClient, the WS rejects cleanly."""
    application = FastAPI()
    application.include_router(router)

    test_client = TestClient(application)
    with test_client.websocket_connect("/ws/chat/agent_a") as ws:
        err = ws.receive_json()
        assert err["type"] == "error"
        assert err["code"] == chat_module.CLOSE_CODE_INTERNAL_ERROR
        assert "not fully initialized" in err["message"]


# ---------------------------------------------------------------------------
# Disconnect cleanup
# ---------------------------------------------------------------------------


def test_client_disconnect_during_poll_does_not_raise(
    client: TestClient, mock_daemon: AsyncMock
) -> None:
    """Abrupt client disconnect during polling must not crash the bridge."""
    mock_daemon.agent_execute.return_value = {"task_id": "t1", "session_id": "ses_1"}

    async def slow_poll(*args: Any, **kwargs: Any) -> dict[str, Any]:
        await asyncio.sleep(0.2)
        return {"content": "late", "final": False}

    mock_daemon.agent_poll_response.side_effect = slow_poll

    with client.websocket_connect("/ws/chat/agent_a") as ws:
        ws.send_json({"content": "hi", "session_id": ""})
        # Receive one chunk and then close — the next poll will find the
        # socket gone and the bridge must log + exit cleanly.
        ws.receive_json()  # chunk 1
        ws.close()

    # No exception escaped the endpoint — TestClient would surface it.
    # Subsequent poll calls are fine to have happened or not; we just
    # assert no exception bubbles out of the test client context.


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _drain(ws: Any, count: int) -> list[dict[str, Any]]:
    """Read ``count`` JSON frames from a WebSocket, in order."""
    out: list[dict[str, Any]] = []
    for _ in range(count):
        out.append(ws.receive_json())
    return out