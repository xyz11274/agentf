"""AgentFlow Bridge — daemon monitoring routes.

This router serves cached snapshots of the ``agentflowd`` daemon's
health, version, uptime and active-agent count. The cache is populated
by a background task in :func:`bridge.main.lifespan` — the routes
themselves never touch the daemon on a per-request basis, so a swarm
of monitoring clients cannot overload it.

Endpoints
---------

``GET /api/daemon/status``
    Snapshot of the daemon: ``running`` flag, daemon payload, active
    agents, last refresh timestamp. ``running`` is ``false`` whenever
    the most recent cache update failed to reach the daemon.

``GET /api/daemon/uptime``
    Just the uptime in seconds plus the ``running`` flag — convenient
    for lightweight health probes.

Cache shape
-----------

``app.state.daemon_status_cache`` is a mutable dict of the form::

    {
        "running": bool,
        "daemon": dict | None,
        "active_agents": int,
        "uptime_seconds": int,
        "last_updated": float,
        "error": str | None,
    }

:func:`update_daemon_status_cache` is the single writer; it is
exported so ``main.lifespan`` can both call it directly (initial
prime) and schedule it on a loop.
"""

from __future__ import annotations

import logging
import time
from typing import Any

from fastapi import APIRouter, Request

from client import DaemonClient, DaemonError

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/aisf/healthagent/api/daemon", tags=["daemon"])


EMPTY_CACHE: dict[str, Any] = {
    "running": False,
    "daemon": None,
    "active_agents": 0,
    "uptime_seconds": 0,
    "last_updated": 0.0,
    "error": None,
}


def _read_cache(state: Any) -> dict[str, Any]:
    cache = getattr(state, "daemon_status_cache", None)
    if not cache:
        return dict(EMPTY_CACHE)
    return cache


async def update_daemon_status_cache(state: Any, client: DaemonClient) -> None:
    """Refresh ``state.daemon_status_cache`` from a live daemon call.

    On any :class:`~client.DaemonError` from ``health()`` the cache is
    flipped to ``running: False`` with the error message preserved;
    ``runtime_status()`` is then skipped because there is no daemon to
    ask. On success the merged ``health + runtime_status`` payload is
    written into the cache and ``last_updated`` is stamped with the
    current monotonic time.

    The function mutates ``state.daemon_status_cache`` in place so
    concurrent readers see a consistent snapshot of either the old or
    the new payload (a dict assignment in CPython is atomic for the
    reference itself; the contents are also a single literal).
    """
    cache = getattr(state, "daemon_status_cache", None)
    if cache is None:
        cache = dict(EMPTY_CACHE)
        state.daemon_status_cache = cache

    try:
        health = await client.health()
    except DaemonError as exc:
        logger.warning("daemon health probe failed: %s", exc)
        cache["running"] = False
        cache["daemon"] = None
        cache["active_agents"] = 0
        cache["uptime_seconds"] = 0
        cache["last_updated"] = time.time()
        cache["error"] = str(exc)
        return

    try:
        runtime = await client.runtime_status()
    except DaemonError as exc:
        logger.warning("daemon runtime.status failed: %s", exc)
        cache["running"] = False
        cache["daemon"] = None
        cache["active_agents"] = 0
        cache["uptime_seconds"] = 0
        cache["last_updated"] = time.time()
        cache["error"] = str(exc)
        return

    daemon_payload: dict[str, Any] = dict(health)
    for key, value in (runtime or {}).items():
        daemon_payload.setdefault(key, value)

    cache["running"] = True
    cache["daemon"] = daemon_payload
    cache["active_agents"] = int(runtime.get("active_agents", 0)) if isinstance(runtime, dict) else 0
    cache["uptime_seconds"] = int(runtime.get("uptime", 0)) if isinstance(runtime, dict) else 0
    cache["last_updated"] = time.time()
    cache["error"] = None


@router.get("/status", name="daemon_status")
async def daemon_status(request: Request) -> dict[str, Any]:
    """``GET /api/daemon/status`` — cached daemon snapshot."""
    return _read_cache(request.app.state)


@router.get("/uptime", name="daemon_uptime")
async def daemon_uptime(request: Request) -> dict[str, Any]:
    """``GET /api/daemon/uptime`` — daemon uptime in seconds + running flag."""
    cache = _read_cache(request.app.state)
    return {
        "uptime_seconds": int(cache.get("uptime_seconds", 0) or 0),
        "running": bool(cache.get("running", False)),
    }


__all__ = ["router", "update_daemon_status_cache"]