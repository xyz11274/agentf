"""Tests for AgentFlow Bridge — REST SSE streaming chat endpoint.

Covers the ``_chat_turn_events`` async generator and the ``stream=true``
SSE streaming mode of the ``chat_rest`` endpoint.
"""

from __future__ import annotations

import asyncio
import json
from typing import Any
from unittest.mock import AsyncMock, Mock

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from client import DaemonError
from dependencies import get_daemon_client
from routes.chat_rest import (
    _chat_turn_events,
    _sse_format,
    router,
)

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def mock_daemon() -> AsyncMock:
    """Build an AsyncMock for DaemonClient methods consumed by ``_chat_turn_events``."""
    daemon = AsyncMock()
    daemon.agent_execute = AsyncMock()
    daemon.wait_for_response = AsyncMock()
    daemon.discard_response_queue = Mock()
    return daemon


@pytest.fixture(autouse=True)
def _fast_timeouts(monkeypatch: pytest.MonkeyPatch) -> None:
    """Shrink the turn timeout so tests don't wait on real time."""
    monkeypatch.setattr(
        "routes.chat_rest.settings.chat_turn_timeout_seconds", 2.0,
    )


# ---------------------------------------------------------------------------
# _chat_turn_events — unit tests
# ---------------------------------------------------------------------------


class TestChatTurnEventsHappyPath:
    """``_chat_turn_events`` yields chunk events then done."""

    @pytest.mark.asyncio
    async def test_single_chunk(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {
            "content": "Hello world!", "final": True,
        }

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-1",
            turn_timeout=10.0,
        ):
            events.append(event)

        assert len(events) == 2
        assert events[0] == {"type": "chunk", "content": "Hello world!", "final": True, "task_id": "t1"}
        assert events[1] == {"type": "done", "final": True}

    @pytest.mark.asyncio
    async def test_task_id_only_on_first_chunk(self, mock_daemon: AsyncMock) -> None:
        """I-3 metadata consistency: task_id appears on the first chunk only."""
        mock_daemon.agent_execute.return_value = {"task_id": "task-xyz"}
        mock_daemon.wait_for_response.side_effect = [
            {"content": "a", "final": False},
            {"content": "b", "final": False},
            {"content": "c", "final": True},
        ]

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-task-id",
            turn_timeout=10.0,
        ):
            events.append(event)

        chunks = [e for e in events if e["type"] == "chunk"]
        assert len(chunks) == 3
        assert chunks[0]["task_id"] == "task-xyz", "first chunk must carry task_id"
        assert chunks[1]["task_id"] is None, "subsequent chunks must not carry task_id"
        assert chunks[2]["task_id"] is None, "final chunk must not carry task_id"

    @pytest.mark.asyncio
    async def test_multiple_chunks(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.side_effect = [
            {"content": "Hello", "final": False},
            {"content": " world", "final": False},
            {"content": "!", "final": True},
        ]

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-2",
            turn_timeout=10.0,
        ):
            events.append(event)

        assert len(events) == 4, f"expected 4 events, got {len(events)}: {events}"
        types = [e["type"] for e in events]
        assert types == ["chunk", "chunk", "chunk", "done"], f"got types: {types}"
        assert events[0]["content"] == "Hello"
        assert events[1]["content"] == " world"
        assert events[2]["content"] == "!"
        assert events[2]["final"] is True

    @pytest.mark.asyncio
    async def test_no_content_in_chunk(self, mock_daemon: AsyncMock) -> None:
        """A chunk with empty content should still yield."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {"content": "", "final": True}

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-3",
            turn_timeout=10.0,
        ):
            events.append(event)

        assert len(events) == 2
        assert events[0] == {"type": "chunk", "content": "", "final": True, "task_id": "t1"}
        assert events[1] == {"type": "done", "final": True}


class TestChatTurnEventsErrors:
    """Error paths in ``_chat_turn_events``."""

    @pytest.mark.asyncio
    async def test_execute_timeout(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.side_effect = asyncio.TimeoutError()

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-err-1",
            turn_timeout=2.0,
        ):
            events.append(event)

        assert len(events) == 2
        assert events[0]["type"] == "error"
        assert "timed out" in events[0]["message"]
        assert events[1] == {"type": "done", "final": True}

    @pytest.mark.asyncio
    async def test_execute_daemon_error(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.side_effect = DaemonError("agent not found")

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-err-2",
            turn_timeout=2.0,
        ):
            events.append(event)

        assert len(events) == 2
        assert events[0]["type"] == "error"
        assert "agent not found" in events[0]["message"]
        assert events[1] == {"type": "done", "final": True}

    @pytest.mark.asyncio
    async def test_missing_task_id(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"session_id": "ses_x"}

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-err-3",
            turn_timeout=2.0,
        ):
            events.append(event)

        assert len(events) == 2
        assert events[0]["type"] == "error"
        assert "task_id" in events[0]["message"]
        assert events[1] == {"type": "done", "final": True}
        mock_daemon.wait_for_response.assert_not_called()

    @pytest.mark.asyncio
    async def test_wait_for_response_timeout(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.side_effect = asyncio.TimeoutError()

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-err-4",
            turn_timeout=2.0,
        ):
            events.append(event)

        assert len(events) == 2
        assert events[0]["type"] == "error"
        assert events[1] == {"type": "done", "final": True}
        mock_daemon.discard_response_queue.assert_called_once()

    @pytest.mark.asyncio
    async def test_wait_for_response_daemon_error(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.side_effect = DaemonError("daemon down")

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-err-5",
            turn_timeout=2.0,
        ):
            events.append(event)

        assert len(events) == 2
        assert events[0]["type"] == "error"
        assert "daemon down" in events[0]["message"]
        assert events[1] == {"type": "done", "final": True}
        mock_daemon.discard_response_queue.assert_called_once()

    @pytest.mark.asyncio
    async def test_empty_notification_continues(self, mock_daemon: AsyncMock) -> None:
        """An empty (falsy) notification must not crash or yield a spurious chunk."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.side_effect = [
            {},  # empty notification → continue
            {"content": "ok", "final": True},
        ]

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hi",
            dialogue_id="test-dlg-err-6",
            turn_timeout=10.0,
        ):
            events.append(event)

        assert len(events) == 2
        assert events[0] == {"type": "chunk", "content": "ok", "final": True, "task_id": "t1"}
        assert events[1] == {"type": "done", "final": True}


# ---------------------------------------------------------------------------
# _sse_format — pure function tests
# ---------------------------------------------------------------------------


class TestSseFormat:
    """``_sse_format`` produces correctly-formatted SSE frames."""

    def test_message_event(self) -> None:
        frame = _sse_format("message", {"content": "Hello", "final": False})
        assert frame == "event: message\ndata: " + json.dumps(
            {"content": "Hello", "final": False}, ensure_ascii=False,
        ) + "\n\n"

    def test_done_event(self) -> None:
        frame = _sse_format("done", {"final": True})
        assert frame == "event: done\ndata: " + json.dumps(
            {"final": True}, ensure_ascii=False,
        ) + "\n\n"

    def test_error_event(self) -> None:
        frame = _sse_format("error", {"message": "something", "code": -32000})
        assert frame == "event: error\ndata: " + json.dumps(
            {"message": "something", "code": -32000}, ensure_ascii=False,
        ) + "\n\n"

    def test_unicode_content(self) -> None:
        """SSE frame correctly handles Unicode."""
        frame = _sse_format("message", {"content": "你好世界", "final": True})
        assert "你好世界" in frame
        assert frame.endswith("\n\n")


# ---------------------------------------------------------------------------
# Integration: SSE streaming via TestClient
# ---------------------------------------------------------------------------


class TestSseStreamingEndpoint:
    """End-to-end SSE streaming via ``POST /api/chat/{id}?stream=true``.

    Uses ``starlette.testclient.TestClient`` with a mocked DaemonClient
    dependency to validate the full HTTP response pipeline.
    """

    def _mk_app(self, mock_daemon: AsyncMock) -> FastAPI:
        app = FastAPI()
        app.include_router(router)

        async def override_get_daemon_client() -> AsyncMock:
            return mock_daemon

        app.dependency_overrides[get_daemon_client] = override_get_daemon_client  # type: ignore[arg-type]
        return app

    def test_streaming_multiple_chunks(self, mock_daemon: AsyncMock) -> None:
        """SSE response delivers one event per notification."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.side_effect = [
            {"content": "Hello", "final": False},
            {"content": " world", "final": True},
        ]

        app = self._mk_app(mock_daemon)
        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a?stream=true",
                json={"user_id": "u1", "client_id": "c1", "content": "hi"},
            )

        assert response.status_code == 200
        assert response.headers["content-type"].startswith("text/event-stream")
        assert response.headers["cache-control"] == "no-cache"

        body = response.text
        events = self._parse_sse(body)
        assert len(events) == 3, f"expected 3 events, got {len(events)}: {events}"
        # task_id is attached to the first chunk only.
        assert events[0] == ("message", {"content": "Hello", "final": False, "task_id": "t1"})
        assert events[1] == ("message", {"content": " world", "final": True})
        assert events[2] == ("done", {"final": True})

    def test_streaming_single_final_chunk(self, mock_daemon: AsyncMock) -> None:
        """When the daemon returns final=true immediately."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {
            "content": "Complete response", "final": True,
        }

        app = self._mk_app(mock_daemon)
        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a?stream=true",
                json={"user_id": "u1", "client_id": "c1", "content": "hi"},
            )

        assert response.status_code == 200
        events = self._parse_sse(response.text)
        assert len(events) == 2
        assert events[0] == ("message", {"content": "Complete response", "final": True, "task_id": "t1"})
        assert events[1] == ("done", {"final": True})

    def test_streaming_execute_error(self, mock_daemon: AsyncMock) -> None:
        """SSE error event when agent.execute fails."""
        mock_daemon.agent_execute.side_effect = DaemonError("no such agent")
        # wait_for_response should not be called
        mock_daemon.wait_for_response = AsyncMock()  # just to be safe

        app = self._mk_app(mock_daemon)
        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a?stream=true",
                json={"user_id": "u1", "client_id": "c1", "content": "hi"},
            )

        assert response.status_code == 200
        events = self._parse_sse(response.text)
        assert len(events) == 2
        assert events[0][0] == "error"
        assert "no such agent" in events[0][1]["message"]
        assert events[1] == ("done", {"final": True})
        mock_daemon.wait_for_response.assert_not_called()

    def test_streaming_missing_task_id(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.return_value = {"session_id": "ses_x"}

        app = self._mk_app(mock_daemon)
        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a?stream=true",
                json={"user_id": "u1", "client_id": "c1", "content": "hi"},
            )

        assert response.status_code == 200
        events = self._parse_sse(response.text)
        assert len(events) == 2
        assert events[0][0] == "error"
        assert "task_id" in events[0][1]["message"]
        assert events[1] == ("done", {"final": True})

    def test_synchronous_mode_unaffected(self, mock_daemon: AsyncMock) -> None:
        """Default mode (no stream=true) still returns the original JSON envelope."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {
            "content": "Hello world!", "final": True,
        }

        app = self._mk_app(mock_daemon)
        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a",
                json={"user_id": "u1", "client_id": "c1", "content": "hi"},
            )

        assert response.status_code == 200
        body = response.json()
        assert body["success"] is True
        assert body["data"]["content"] == "Hello world!"

    def test_synchronous_mode_returns_task_id(self, mock_daemon: AsyncMock) -> None:
        """Regression for P0 #1: synchronous mode must populate task_id."""
        mock_daemon.agent_execute.return_value = {"task_id": "task-sync-42"}
        mock_daemon.wait_for_response.return_value = {
            "content": "Hello world!", "final": True,
        }

        app = self._mk_app(mock_daemon)
        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a",
                json={"user_id": "u1", "client_id": "c1", "content": "hi"},
            )

        assert response.status_code == 200
        body = response.json()
        assert body["data"]["task_id"] == "task-sync-42", \
            "synchronous mode must return the daemon task_id, not None"

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _parse_sse(text: str) -> list[tuple[str, dict[str, Any]]]:
        """Parse SSE text into ``[(event_type, data_dict), ...]``."""
        events: list[tuple[str, dict[str, Any]]] = []
        current_event = ""
        current_data_lines: list[str] = []

        for line in text.split("\n"):
            if line.startswith("event: "):
                current_event = line[7:]
            elif line.startswith("data: "):
                current_data_lines.append(line[6:])
            elif line == "":
                if current_data_lines:
                    data_str = "\n".join(current_data_lines)
                    events.append((current_event, json.loads(data_str)))
                current_event = ""
                current_data_lines = []

        return events
