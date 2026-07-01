"""Tests for AgentFlow Bridge — agent & session proxy routers (T6).

Each test:
  * mounts the router under a fresh ``FastAPI`` app;
  * overrides ``get_daemon_client`` with a fully-mocked :class:`DaemonClient`;
  * issues an HTTP request via ``httpx.AsyncClient`` and asserts the response.

Covers:
  * happy-path success for every endpoint;
  * the 5-element error-mapping table
    (InvalidParams→400, NotFound→404, Timeout→504, Connection→502, other→500);
  * that ``agent_id`` / ``session_id`` / ``package_path`` are forwarded to
    the daemon client unchanged.
"""

from __future__ import annotations

from typing import Any, AsyncGenerator
from unittest.mock import AsyncMock

import pytest
from fastapi import FastAPI
from httpx import ASGITransport, AsyncClient

from client import (
    DaemonClient,
    DaemonConnectionError,
    DaemonError,
    DaemonTimeoutError,
    InvalidParamsError,
    NotFoundError,
)
from dependencies import get_daemon_client
from routes import agents as agents_routes
from routes import sessions as sessions_routes


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


def _build_app(mock_client: AsyncMock) -> FastAPI:
    """Build a fresh FastAPI app with both routers and an overridden client."""
    app = FastAPI()
    app.include_router(agents_routes.router)
    app.include_router(sessions_routes.router)

    async def _override() -> DaemonClient:
        return mock_client  # type: ignore[return-value]

    app.dependency_overrides[get_daemon_client] = _override
    return app


@pytest.fixture
def mock_client() -> AsyncMock:
    """A bare ``AsyncMock`` — only methods the route under test calls are set."""
    return AsyncMock(spec=DaemonClient)


@pytest.fixture
async def client(mock_client: AsyncMock) -> AsyncGenerator[AsyncClient, None]:
    app = _build_app(mock_client)
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as ac:
        yield ac


# ---------------------------------------------------------------------------
# Agent endpoints — success path
# ---------------------------------------------------------------------------


async def test_list_agents_returns_daemon_payload(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_list.return_value = [{"id": "a1"}, {"id": "a2"}]
    resp = await client.get("/api/agents")
    assert resp.status_code == 200
    body = resp.json()
    assert body == {"success": True, "data": [{"id": "a1"}, {"id": "a2"}], "error": None}
    mock_client.agent_list.assert_awaited_once_with()


async def test_get_agent_forwards_id(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_info.return_value = {"id": "a1", "name": "demo", "status": "running"}
    resp = await client.get("/api/agents/a1")
    assert resp.status_code == 200
    assert resp.json() == {
        "success": True,
        "data": {"id": "a1", "name": "demo", "status": "running"},
        "error": None,
    }
    mock_client.agent_info.assert_awaited_once_with("a1")


async def test_load_agent_falls_back_to_agent_id_when_body_empty(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/a1/load")
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"loaded": True}, "error": None}
    from config import settings
    expected_path = str(settings.agent_packages_path / "a1")
    mock_client.agent_load.assert_awaited_once_with(expected_path)


async def test_load_agent_uses_explicit_package_path(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/a1/load", json={"package_path": "/opt/a1"})
    assert resp.status_code == 200
    mock_client.agent_load.assert_awaited_once_with("/opt/a1")


async def test_unload_agent_forwards_id(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_unload.return_value = {"unloaded": True}
    resp = await client.post("/api/agents/a1/unload")
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"unloaded": True}, "error": None}
    mock_client.agent_unload.assert_awaited_once_with("a1")


async def test_pause_agent_forwards_id(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_pause.return_value = {"paused": True}
    resp = await client.post("/api/agents/a1/pause")
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"paused": True}, "error": None}
    mock_client.agent_pause.assert_awaited_once_with("a1")


async def test_resume_agent_forwards_id(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_resume.return_value = {"resumed": True}
    resp = await client.post("/api/agents/a1/resume")
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"resumed": True}, "error": None}
    mock_client.agent_resume.assert_awaited_once_with("a1")


# ---------------------------------------------------------------------------
# Session endpoints — success path
# ---------------------------------------------------------------------------


async def test_list_sessions_requires_agent_id(client: AsyncClient) -> None:
    resp = await client.get("/api/sessions")
    assert resp.status_code == 422


async def test_list_sessions_forwards_agent_id(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.session_list.return_value = [{"session_id": "s1"}, {"session_id": "s2"}]
    resp = await client.get("/api/sessions", params={"agent_id": "a1"})
    assert resp.status_code == 200
    assert resp.json() == {
        "success": True,
        "data": [{"session_id": "s1"}, {"session_id": "s2"}],
        "error": None,
    }
    mock_client.session_list.assert_awaited_once_with("a1", "")


async def test_destroy_session_forwards_ids(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.session_destroy.return_value = {"destroyed": True}
    resp = await client.delete("/api/sessions/s1", params={"agent_id": "a1"})
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"destroyed": True}, "error": None}
    mock_client.session_destroy.assert_awaited_once_with("a1", "s1")


async def test_clear_session_dialogue_forwards_params(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.session_clear_dialogue.return_value = {"cleared": True}
    resp = await client.delete(
        "/api/sessions/s1/dialogue",
        params={"agent_id": "a1", "dialogue_id": "d9"},
    )
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"cleared": True}, "error": None}
    mock_client.session_clear_dialogue.assert_awaited_once_with("a1", "s1", "d9")


async def test_clear_session_dialogue_defaults_empty_dialogue_id(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.session_clear_dialogue.return_value = {"cleared": True}
    resp = await client.delete("/api/sessions/s1/dialogue", params={"agent_id": "a1"})
    assert resp.status_code == 200
    mock_client.session_clear_dialogue.assert_awaited_once_with("a1", "s1", "")


# ---------------------------------------------------------------------------
# Error mapping — one representative endpoint per status code
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("exc", "expected_status"),
    [
        (InvalidParamsError("bad params", code=-32602), 400),
        (NotFoundError("no such agent", code=-2), 404),
        (DaemonTimeoutError("slow", code=-32000), 504),
        (DaemonConnectionError("offline"), 502),
        (DaemonError("boom", code=-1), 500),
    ],
)
async def test_agent_info_error_mapping(
    client: AsyncClient,
    mock_client: AsyncMock,
    exc: DaemonError,
    expected_status: int,
) -> None:
    mock_client.agent_info.side_effect = exc
    resp = await client.get("/api/agents/whatever")
    assert resp.status_code == expected_status
    assert exc.message in resp.json()["detail"]


@pytest.mark.parametrize(
    ("exc", "expected_status"),
    [
        (InvalidParamsError("bad params", code=-32602), 400),
        (NotFoundError("no such session", code=-2), 404),
        (DaemonTimeoutError("slow", code=-32000), 504),
        (DaemonConnectionError("offline"), 502),
        (DaemonError("boom", code=-1), 500),
    ],
)
async def test_session_destroy_error_mapping(
    client: AsyncClient,
    mock_client: AsyncMock,
    exc: DaemonError,
    expected_status: int,
) -> None:
    mock_client.session_destroy.side_effect = exc
    resp = await client.delete("/api/sessions/s1", params={"agent_id": "a1"})
    assert resp.status_code == expected_status
    assert exc.message in resp.json()["detail"]


# ---------------------------------------------------------------------------
# Sanity: routes are wired with the expected paths
# ---------------------------------------------------------------------------


async def test_unknown_agent_id_subpath_returns_404(client: AsyncClient) -> None:
    """Routes only exist for the documented suffixes — extras should 404."""
    resp = await client.post("/api/agents/a1/nonsense")
    assert resp.status_code == 404


async def test_routes_propagate_typed_payload_unmodified(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    """Daemon payloads must be forwarded verbatim — no field rewriting."""
    payload: dict[str, Any] = {
        "id": "a1",
        "name": "demo",
        "workflow_count": 3,
        "status": "running",
        "extra_field": ["kept", "as", "is"],
    }
    mock_client.agent_info.return_value = payload
    resp = await client.get("/api/agents/a1")
    assert resp.status_code == 200
    assert resp.json()["data"] == payload
