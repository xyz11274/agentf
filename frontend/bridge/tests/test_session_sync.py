"""Unit tests for `services.AgentflowSessionSync`.

Covers the Phase-2 background sync that mirrors agentflow daemon session
status into the Bridge `SessionProvider`. The sync is best-effort (I-5):
all daemon/provider errors are swallowed so a sync outage never breaks
the chat main flow or the poll loop.

Design ref: ``docs/design/features/bridge-session-persistence.md`` §5.2.
"""

from __future__ import annotations

from unittest.mock import AsyncMock

import pytest

from services import AgentflowSessionSync


class _FakeProvider:
    """Minimal in-memory provider stub for sync tests."""

    def __init__(self, by_af: dict[str, dict]) -> None:
        self._by_af = by_af
        self.updates: list[tuple[str, str]] = []

    async def find_by_agentflow_session(self, af_sid: str) -> dict | None:
        return self._by_af.get(af_sid)

    async def update_session_status(self, session_id: str, status: str) -> None:
        self.updates.append((session_id, status))


@pytest.fixture
def daemon() -> AsyncMock:
    d = AsyncMock()
    d.agent_list = AsyncMock()
    d.session_list = AsyncMock()
    return d


@pytest.fixture
def provider() -> _FakeProvider:
    return _FakeProvider({
        "af-1": {"id": "b-1", "status": "active"},
        "af-2": {"id": "b-2", "status": "active"},
    })


# ── Happy path ────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_sync_updates_changed_status(daemon: AsyncMock, provider: _FakeProvider):
    daemon.agent_list.return_value = [{"id": "agent_a"}]
    daemon.session_list.return_value = [
        {"id": "af-1", "status": "completed"},
        {"id": "af-2", "status": "active"},  # unchanged
    ]

    syncer = AgentflowSessionSync(daemon, provider)
    updated = await syncer.sync()

    assert updated == 1
    assert provider.updates == [("b-1", "completed")]


@pytest.mark.asyncio
async def test_sync_skips_unknown_af_session(daemon: AsyncMock, provider: _FakeProvider):
    daemon.agent_list.return_value = [{"id": "agent_a"}]
    daemon.session_list.return_value = [
        {"id": "af-1", "status": "completed"},
        {"id": "af-unknown", "status": "active"},  # no Bridge session
    ]

    syncer = AgentflowSessionSync(daemon, provider)
    updated = await syncer.sync()

    assert updated == 1
    assert provider.updates == [("b-1", "completed")]


@pytest.mark.asyncio
async def test_sync_handles_session_list_dict_payload(daemon: AsyncMock, provider: _FakeProvider):
    """Some daemon versions return ``{"sessions": [...]}`` instead of a bare list."""
    daemon.agent_list.return_value = [{"id": "agent_a"}]
    daemon.session_list.return_value = {"sessions": [{"id": "af-1", "status": "archived"}]}

    syncer = AgentflowSessionSync(daemon, provider)
    updated = await syncer.sync()

    assert updated == 1
    assert provider.updates == [("b-1", "archived")]


@pytest.mark.asyncio
async def test_sync_multi_agent(daemon: AsyncMock, provider: _FakeProvider):
    daemon.agent_list.return_value = [{"id": "agent_a"}, {"id": "agent_b"}]
    daemon.session_list.side_effect = [
        [{"id": "af-1", "status": "completed"}],
        [{"id": "af-2", "status": "archived"}],
    ]

    syncer = AgentflowSessionSync(daemon, provider)
    updated = await syncer.sync()

    assert updated == 2
    assert ("b-1", "completed") in provider.updates
    assert ("b-2", "archived") in provider.updates


# ── Resilience / I-5 ──────────────────────────────────────────────


@pytest.mark.asyncio
async def test_sync_swallows_agent_list_error(daemon: AsyncMock, provider: _FakeProvider):
    daemon.agent_list.side_effect = RuntimeError("daemon down")

    syncer = AgentflowSessionSync(daemon, provider)
    updated = await syncer.sync()  # must not raise

    assert updated == 0
    assert provider.updates == []


@pytest.mark.asyncio
async def test_sync_swallows_session_list_error(daemon: AsyncMock, provider: _FakeProvider):
    daemon.agent_list.return_value = [{"id": "agent_a"}]
    daemon.session_list.side_effect = RuntimeError("daemon down")

    syncer = AgentflowSessionSync(daemon, provider)
    updated = await syncer.sync()  # must not raise

    assert updated == 0


@pytest.mark.asyncio
async def test_sync_no_agents(daemon: AsyncMock, provider: _FakeProvider):
    daemon.agent_list.return_value = []

    syncer = AgentflowSessionSync(daemon, provider)
    updated = await syncer.sync()

    assert updated == 0
    daemon.session_list.assert_not_called()


@pytest.mark.asyncio
async def test_sync_skips_agent_without_id(daemon: AsyncMock, provider: _FakeProvider):
    daemon.agent_list.return_value = [{"foo": "bar"}]  # no id/agent_id

    syncer = AgentflowSessionSync(daemon, provider)
    updated = await syncer.sync()

    assert updated == 0
    daemon.session_list.assert_not_called()
