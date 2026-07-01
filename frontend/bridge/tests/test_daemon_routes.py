"""Tests for AgentFlow Bridge — daemon monitoring routes (T7).

The daemon routes serve cached status / uptime data:

  * ``GET /api/daemon/status`` — current daemon snapshot (running flag + payload).
  * ``GET /api/daemon/uptime`` — daemon uptime in seconds.

The cache itself is populated by a background task in ``main.lifespan`` every
``daemon_status_refresh_interval`` seconds. These tests inject a fully-mocked
:class:`DaemonClient` and a pre-seeded cache on ``app.state``, so the daemon is
never actually hit by the route handlers.
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
)
from dependencies import get_daemon_client
from routes import daemon as daemon_routes


def _build_app(mock_client: AsyncMock, cache: dict[str, Any] | None) -> FastAPI:
    app = FastAPI()
    app.include_router(daemon_routes.router)

    async def _override() -> DaemonClient:
        return mock_client  # type: ignore[return-value]

    app.dependency_overrides[get_daemon_client] = _override

    if cache is not None:
        app.state.daemon_status_cache = cache
    else:
        app.state.daemon_status_cache = {"running": False, "daemon": None, "uptime_seconds": 0}
    return app


@pytest.fixture
def mock_client() -> AsyncMock:
    return AsyncMock(spec=DaemonClient)


@pytest.fixture
async def client(mock_client: AsyncMock) -> AsyncGenerator[AsyncClient, None]:
    cache = {
        "running": True,
        "daemon": {"status": "ok", "version": "0.1.0", "uptime": 42},
        "active_agents": 3,
        "uptime_seconds": 42,
        "last_updated": 1700000000.0,
    }
    app = _build_app(mock_client, cache)
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as ac:
        yield ac


async def test_status_returns_cached_payload(client: AsyncClient, mock_client: AsyncMock) -> None:
    resp = await client.get("/api/daemon/status")
    assert resp.status_code == 200
    body = resp.json()
    assert body["running"] is True
    assert body["daemon"] == {"status": "ok", "version": "0.1.0", "uptime": 42}
    assert body["active_agents"] == 3
    assert body["uptime_seconds"] == 42
    mock_client.health.assert_not_called()
    mock_client.runtime_status.assert_not_called()


async def test_status_reports_down_when_cache_marks_down(mock_client: AsyncMock) -> None:
    cache = {
        "running": False,
        "daemon": None,
        "active_agents": 0,
        "uptime_seconds": 0,
        "last_updated": 1700000000.0,
        "error": "connection refused",
    }
    app = _build_app(mock_client, cache)
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as ac:
        resp = await ac.get("/api/daemon/status")
    assert resp.status_code == 200
    body = resp.json()
    assert body["running"] is False
    assert body["daemon"] is None
    assert body["active_agents"] == 0
    assert body["uptime_seconds"] == 0
    assert body.get("error") == "connection refused"
    mock_client.health.assert_not_called()


async def test_status_falls_back_to_defaults_when_cache_missing(mock_client: AsyncMock) -> None:
    app = FastAPI()
    app.include_router(daemon_routes.router)

    async def _override() -> DaemonClient:
        return mock_client  # type: ignore[return-value]

    app.dependency_overrides[get_daemon_client] = _override

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as ac:
        resp = await ac.get("/api/daemon/status")
    assert resp.status_code == 200
    body = resp.json()
    assert body["running"] is False
    assert body["daemon"] is None


async def test_uptime_returns_seconds_from_cache(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    resp = await client.get("/api/daemon/uptime")
    assert resp.status_code == 200
    assert resp.json() == {"uptime_seconds": 42, "running": True}
    mock_client.runtime_status.assert_not_called()


async def test_uptime_zero_when_daemon_down(mock_client: AsyncMock) -> None:
    cache = {
        "running": False,
        "daemon": None,
        "active_agents": 0,
        "uptime_seconds": 0,
        "last_updated": 1700000000.0,
    }
    app = _build_app(mock_client, cache)
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as ac:
        resp = await ac.get("/api/daemon/uptime")
    assert resp.status_code == 200
    body = resp.json()
    assert body["uptime_seconds"] == 0
    assert body["running"] is False


async def test_update_cache_merges_health_and_runtime(mock_client: AsyncMock) -> None:
    mock_client.health.return_value = {"status": "ok"}
    mock_client.runtime_status.return_value = {
        "version": "0.2.0",
        "uptime": 100,
        "active_agents": 7,
    }
    app = FastAPI()
    app.state.daemon_status_cache = {"running": False, "daemon": None}
    await daemon_routes.update_daemon_status_cache(app.state, mock_client)

    cache = app.state.daemon_status_cache
    assert cache["running"] is True
    assert cache["daemon"]["status"] == "ok"
    assert cache["daemon"]["version"] == "0.2.0"
    assert cache["daemon"]["uptime"] == 100
    assert cache["active_agents"] == 7
    assert cache["uptime_seconds"] == 100
    assert "last_updated" in cache
    mock_client.health.assert_awaited_once_with()
    mock_client.runtime_status.assert_awaited_once_with()


async def test_update_cache_marks_down_on_connection_error(mock_client: AsyncMock) -> None:
    mock_client.health.side_effect = DaemonConnectionError("connection refused")
    mock_client.runtime_status.return_value = {"version": "0.2.0", "uptime": 1, "active_agents": 0}

    app = FastAPI()
    app.state.daemon_status_cache = {"running": True, "daemon": {"status": "ok"}}
    await daemon_routes.update_daemon_status_cache(app.state, mock_client)

    cache = app.state.daemon_status_cache
    assert cache["running"] is False
    assert cache["daemon"] is None
    assert cache["uptime_seconds"] == 0
    assert "error" in cache
    assert "connection refused" in cache["error"]
    mock_client.runtime_status.assert_not_called()


async def test_update_cache_marks_down_on_timeout(mock_client: AsyncMock) -> None:
    mock_client.health.side_effect = DaemonTimeoutError("timed out")
    app = FastAPI()
    app.state.daemon_status_cache = {"running": True, "daemon": {"status": "ok"}}
    await daemon_routes.update_daemon_status_cache(app.state, mock_client)
    cache = app.state.daemon_status_cache
    assert cache["running"] is False
    assert cache["daemon"] is None
    assert "error" in cache
    assert "timed out" in cache["error"]


async def test_update_cache_marks_down_on_generic_daemon_error(mock_client: AsyncMock) -> None:
    mock_client.health.side_effect = DaemonError("boom")
    app = FastAPI()
    app.state.daemon_status_cache = {"running": True, "daemon": {"status": "ok"}}
    await daemon_routes.update_daemon_status_cache(app.state, mock_client)
    cache = app.state.daemon_status_cache
    assert cache["running"] is False
    assert cache["daemon"] is None
    assert "boom" in cache["error"]