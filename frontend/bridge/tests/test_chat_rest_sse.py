"""Tests for AgentFlow Bridge — REST SSE streaming chat endpoint.

Covers the ``_chat_turn_events`` async generator and the ``stream=true``
SSE streaming mode of the ``chat_rest`` endpoint.
"""

from __future__ import annotations

import json
from typing import Any
from unittest.mock import AsyncMock, Mock

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from client import DaemonError
from dependencies import get_daemon_client
from routes.chat_rest import (
    _SSE_DONE,
    _chat_turn_events,
    _sse_data_json,
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
        assert events[0] == {"type": "chunk", "content": "Hello world!", "final": True, "task_id": "t1", "artifacts": None, "progress": False, "chunk_type": None}
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
        assert events[0] == {"type": "chunk", "content": "", "final": True, "task_id": "t1", "artifacts": None, "progress": False, "chunk_type": None}
        assert events[1] == {"type": "done", "final": True}


class TestChatTurnEventsContextInjection:
    """``_chat_turn_events`` with ``history_messages`` (messages-list protocol).

    Per ``docs/design/features/bridge-session-persistence.md`` §2.4 / §3.1
    the Bridge assembles history + this turn's user input into a messages
    array, JSON-stringifies it, and passes it through the daemon's single
    ``content`` string field. The daemon forwards it verbatim to the node.
    """

    @pytest.mark.asyncio
    async def test_history_messages_json_stringified(
        self, mock_daemon: AsyncMock,
    ) -> None:
        """With ``history_messages``, the daemon's ``content`` is a JSON
        object ``{"stream": false, "messages": [...]}``."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {"content": "ok", "final": True}

        history = [
            {"role": "user", "content": "你好"},
            {"role": "assistant", "content": "你好！"},
        ]
        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="我昨天感冒了",
            dialogue_id="test-dlg-ctx-1",
            turn_timeout=10.0,
            history_messages=history,
        ):
            events.append(event)

        # agent_execute was called with a JSON object with stream + messages
        call_kwargs = mock_daemon.agent_execute.call_args.kwargs
        sent_content = call_kwargs.get("content") or mock_daemon.agent_execute.call_args.args[2]
        parsed = json.loads(sent_content)
        assert parsed["stream"] is False, "stream defaults to false"
        msgs = parsed["messages"]
        assert msgs[-1] == {"role": "user", "content": "我昨天感冒了"}
        assert msgs[:-1] == history
        assert len(msgs) == 3

    @pytest.mark.asyncio
    async def test_no_history_sends_json_protocol(
        self, mock_daemon: AsyncMock,
    ) -> None:
        """Without ``history_messages``, the daemon still receives the
        JSON protocol ``{"stream": false, "messages": [...]}``."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {"content": "ok", "final": True}

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hello",
            dialogue_id="test-dlg-ctx-2",
            turn_timeout=10.0,
        ):
            events.append(event)

        call_kwargs = mock_daemon.agent_execute.call_args.kwargs
        sent_content = call_kwargs.get("content") or mock_daemon.agent_execute.call_args.args[2]
        parsed = json.loads(sent_content)
        assert parsed["stream"] is False
        assert parsed["messages"] == [{"role": "user", "content": "hello"}]

    @pytest.mark.asyncio
    async def test_empty_history_sends_json_protocol(
        self, mock_daemon: AsyncMock,
    ) -> None:
        """An empty ``history_messages`` list still sends JSON protocol."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {"content": "ok", "final": True}

        events: list[dict[str, Any]] = []
        async for event in _chat_turn_events(
            daemon_client=mock_daemon,
            agent_id="agent_a",
            user_id="u1",
            client_id="c1",
            content="hello",
            dialogue_id="test-dlg-ctx-3",
            turn_timeout=10.0,
            history_messages=[],
        ):
            events.append(event)

        call_kwargs = mock_daemon.agent_execute.call_args.kwargs
        sent_content = call_kwargs.get("content") or mock_daemon.agent_execute.call_args.args[2]
        parsed = json.loads(sent_content)
        assert parsed["stream"] is False
        assert parsed["messages"] == [{"role": "user", "content": "hello"}]


class TestChatTurnEventsErrors:
    """Error paths in ``_chat_turn_events``."""

    @pytest.mark.asyncio
    async def test_execute_timeout(self, mock_daemon: AsyncMock) -> None:
        mock_daemon.agent_execute.side_effect = TimeoutError()

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
        mock_daemon.wait_for_response.side_effect = TimeoutError()

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
        assert events[0] == {"type": "chunk", "content": "ok", "final": True, "task_id": "t1", "artifacts": None, "progress": False, "chunk_type": None}
        assert events[1] == {"type": "done", "final": True}


# ---------------------------------------------------------------------------
# _sse_data_json + _SSE_DONE — pure function tests
# ---------------------------------------------------------------------------


class TestSseFormat:
    """``_sse_data_json`` and ``_SSE_DONE`` produce OpenAI-compatible SSE."""

    def test_data_json_chunk(self) -> None:
        """Content chunk uses ``choices[].delta.content`` format."""
        payload = {"choices": [{"delta": {"content": "Hello"}, "index": 0}]}
        frame = _sse_data_json(payload)
        assert frame == "data: " + json.dumps(payload, ensure_ascii=False) + "\n\n"

    def test_data_json_error(self) -> None:
        """Error frame wraps in ``error`` key."""
        payload = {"error": {"message": "something", "code": -32000}}
        frame = _sse_data_json(payload)
        assert frame == "data: " + json.dumps(payload, ensure_ascii=False) + "\n\n"

    def test_done_constant(self) -> None:
        """Terminal signal is ``data: [DONE]``."""
        assert _SSE_DONE == "data: [DONE]\n\n"

    def test_unicode_content(self) -> None:
        """SSE frame correctly handles Unicode."""
        payload = {"choices": [{"delta": {"content": "你好世界"}, "index": 0}]}
        frame = _sse_data_json(payload)
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
        """SSE response delivers one chunk per notification."""
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

        # 验证 agent_execute 被调用了 stream=True
        mock_daemon.agent_execute.assert_awaited_once()
        _, exec_kwargs = mock_daemon.agent_execute.await_args
        assert exec_kwargs.get("stream") is True, \
            "SSE mode must pass stream=True to daemon"

        body = response.text
        events = self._parse_sse(body)
        assert len(events) == 3, f"expected 3 events, got {len(events)}: {events}"
        # Two content chunks, then [DONE]
        # The first chunk now includes _dialogue_id (used for trace filtering).
        assert events[0]["choices"] == [{"message": {"content": "Hello"}, "index": 0}]
        assert "_dialogue_id" in events[0]
        assert events[1] == {"choices": [{"message": {"content": " world"}, "index": 0}]}
        assert events[2] == "[DONE]"

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

        # 验证 agent_execute 被调用了 stream=True
        mock_daemon.agent_execute.assert_awaited_once()
        _, exec_kwargs = mock_daemon.agent_execute.await_args
        assert exec_kwargs.get("stream") is True

        assert response.status_code == 200
        events = self._parse_sse(response.text)
        assert len(events) == 2
        assert events[0]["choices"] == [{"message": {"content": "Complete response"}, "index": 0}]
        assert "_dialogue_id" in events[0]
        assert events[1] == "[DONE]"

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
        assert "error" in events[0]
        assert "no such agent" in events[0]["error"]["message"]
        assert events[1] == "[DONE]"
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
        assert "error" in events[0]
        assert "task_id" in events[0]["error"]["message"]
        assert events[1] == "[DONE]"

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

        # 同步模式不应传 stream=True
        mock_daemon.agent_execute.assert_awaited_once()
        _, exec_kwargs = mock_daemon.agent_execute.await_args
        assert exec_kwargs.get("stream") is False or "stream" not in exec_kwargs, \
            "sync mode must NOT pass stream=True"

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
    def _parse_sse(text: str) -> list[dict[str, Any] | str]:
        """Parse SSE text into ``[data_dict_or_DONE_string, ...]``.

        The new format is OpenAI-compatible: bare ``data:`` lines, no ``event:``,
        and ``data: [DONE]`` as the terminal signal.
        """
        events: list[dict[str, Any] | str] = []
        current_data_lines: list[str] = []

        for line in text.split("\n"):
            if line.startswith("data: "):
                payload = line[6:]
                if payload == "[DONE]":
                    events.append(payload)
                else:
                    current_data_lines.append(payload)
            elif line == "":
                if current_data_lines:
                    data_str = "\n".join(current_data_lines)
                    events.append(json.loads(data_str))
                    current_data_lines = []

        return events


# ---------------------------------------------------------------------------
# Context injection integration (chat_rest + SessionProvider)
# ---------------------------------------------------------------------------


class _MockProvider:
    """Minimal in-memory SessionProvider stub for chat_rest integration tests."""

    def __init__(self, history: list[dict], memories: list[str] | None = None) -> None:
        self._history = history
        self._memories = memories or []
        self.save_dialogue_calls: list[dict] = []
        self.update_af_ref_calls: list[tuple[str, str]] = []

    async def get_or_create_session(self, user_id: str, client_id: str) -> dict:
        return {"id": "b-sess-1", "user_id": user_id, "client_id": client_id}

    async def get_context(self, session_id: str, user_id: str,
                          query: str = "", limit_rounds: int = 5):
        from providers import ContextResult
        return ContextResult(messages=list(self._history), memories=list(self._memories))

    async def update_agentflow_session_ref(self, session_id: str, af_sid: str) -> None:
        self.update_af_ref_calls.append((session_id, af_sid))

    async def save_dialogue(self, session_id: str, user_input: str, agent_response: str,
                            dialogue_id: str, agent_name: str = "",
                            artifacts: str = "[]") -> str:
        self.save_dialogue_calls.append({
            "session_id": session_id, "user_input": user_input,
            "agent_response": agent_response, "dialogue_id": dialogue_id,
        })
        return dialogue_id


class TestChatRestContextInjectionIntegration:
    """End-to-end: chat_rest route injects history from SessionProvider."""

    def _mk_app(self, mock_daemon: AsyncMock, provider: _MockProvider) -> FastAPI:
        from session_dependencies import get_session_provider
        app = FastAPI()
        app.include_router(router)

        async def override_daemon() -> AsyncMock:
            return mock_daemon

        async def override_provider() -> _MockProvider:
            return provider

        app.dependency_overrides[get_daemon_client] = override_daemon  # type: ignore[arg-type]
        app.dependency_overrides[get_session_provider] = override_provider  # type: ignore[arg-type]
        return app

    def test_history_injected_into_daemon_content(self, mock_daemon: AsyncMock) -> None:
        """Provider returns history → daemon receives JSON object with messages array."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {"content": "ok", "final": True}

        history = [{"role": "user", "content": "你好"}, {"role": "assistant", "content": "你好！"}]
        provider = _MockProvider(history=history)
        app = self._mk_app(mock_daemon, provider)

        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a",
                json={"user_id": "u1", "client_id": "c1", "content": "我昨天感冒了"},
            )

        assert response.status_code == 200
        sent_content = mock_daemon.agent_execute.call_args.kwargs.get("content") or \
            mock_daemon.agent_execute.call_args.args[2]
        parsed = json.loads(sent_content)
        assert parsed["stream"] is False
        msgs = parsed["messages"]
        assert msgs[-1] == {"role": "user", "content": "我昨天感冒了"}
        assert msgs[:-1] == history

    def test_no_history_sends_json_protocol(self, mock_daemon: AsyncMock) -> None:
        """Without history, daemon still receives JSON protocol instead of plain text."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {"content": "ok", "final": True}

        provider = _MockProvider(history=[])
        app = self._mk_app(mock_daemon, provider)

        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a",
                json={"user_id": "u1", "client_id": "c1", "content": "hello"},
            )

        assert response.status_code == 200
        sent_content = mock_daemon.agent_execute.call_args.kwargs.get("content") or \
            mock_daemon.agent_execute.call_args.args[2]
        parsed = json.loads(sent_content)
        assert parsed["stream"] is False
        assert parsed["messages"] == [{"role": "user", "content": "hello"}]  # plain text, not JSON

    def test_provider_failure_does_not_break_chat(self, mock_daemon: AsyncMock) -> None:
        """I-5: provider outage during context injection → chat still completes."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {"content": "ok", "final": True}

        class _FailingProvider(_MockProvider):
            async def get_or_create_session(self, user_id: str, client_id: str) -> dict:
                raise RuntimeError("provider down")

        provider = _FailingProvider(history=[])
        app = self._mk_app(mock_daemon, provider)

        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a",
                json={"user_id": "u1", "client_id": "c1", "content": "hi"},
            )

        assert response.status_code == 200
        assert response.json()["data"]["content"] == "ok"

    def test_dialogue_persisted_after_sync_chat(self, mock_daemon: AsyncMock) -> None:
        """Sync chat turn → dialogue saved to provider after [DONE]."""
        mock_daemon.agent_execute.return_value = {"task_id": "t1"}
        mock_daemon.wait_for_response.return_value = {"content": "final answer", "final": True}

        provider = _MockProvider(history=[])
        app = self._mk_app(mock_daemon, provider)

        with TestClient(app) as client:
            response = client.post(
                "/api/chat/agent_a",
                json={"user_id": "u1", "client_id": "c1", "content": "question"},
            )

        assert response.status_code == 200
        assert len(provider.save_dialogue_calls) == 1
        call = provider.save_dialogue_calls[0]
        assert call["user_input"] == "question"
        assert call["agent_response"] == "final answer"
        assert call["session_id"] == "b-sess-1"

