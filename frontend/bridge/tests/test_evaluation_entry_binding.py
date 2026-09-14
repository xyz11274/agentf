"""Optional task delivery keeps the real chat/session handler and normal startup."""
import asyncio
import sys
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import pytest

import main
from routes import chat_rest


@pytest.mark.asyncio
@pytest.mark.parametrize("enabled", [False, True])
async def test_optional_entry_uses_existing_chat(monkeypatch, tmp_path, enabled):
    client = AsyncMock()
    monkeypatch.setattr(main, "DaemonClient", lambda: client)
    monkeypatch.setattr(main.daemon_routes, "update_daemon_status_cache", AsyncMock())
    monkeypatch.setattr(main, "AgentflowSessionSync", lambda *_: SimpleNamespace(sync=AsyncMock()))
    monkeypatch.setattr(main.subprocess, "run", Mock())
    monkeypatch.setattr(main.settings, "session_db_path", str(tmp_path / "sessions.db"))
    handler = AsyncMock(return_value={"data": {"content": "original answer"}})
    monkeypatch.setattr(chat_rest, "chat_rest", handler)
    calls = []
    entered = asyncio.Event()
    cancelled = asyncio.Event()

    async def evaluate(receiver):
        calls.append(await receiver({"agent_id": "existing-agent", "body": {
            "user_id": "test-user", "client_id": "test-conversation", "content": "hello"}}))
        entered.set()
        try:
            await asyncio.Event().wait()
        finally:
            cancelled.set()

    monkeypatch.setitem(sys.modules, "evolution_entry", SimpleNamespace(aevaluate=evaluate))
    if enabled:
        monkeypatch.setenv("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY", "1")
    else:
        monkeypatch.delenv("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY", raising=False)
    async with main.lifespan(main.app):
        if enabled:
            await asyncio.wait_for(entered.wait(), timeout=1)
        assert bool(calls) is enabled
        if enabled:
            arguments = handler.await_args.kwargs
            assert arguments["agent_id"] == "existing-agent"
            assert arguments["body"].content == "hello"
            assert arguments["client"] is client
            assert arguments["provider"] is main.app.state.session_provider
            assert arguments["stream"] is False
        else:
            handler.assert_not_awaited()
    client.close.assert_awaited_once()
    assert cancelled.is_set() is enabled
