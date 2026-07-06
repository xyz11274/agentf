"""AgentFlow Bridge — FastAPI dependency providers.

This module is the single place where the bridge obtains its
:class:`~client.DaemonClient`. Centralising the dependency makes it
trivial to swap in a fake client in tests via ``app.dependency_overrides``
without touching route code.

Lifecycle ownership
-------------------

The long-lived :class:`~client.DaemonClient` is created lazily on the
first dependency resolution and re-used for the lifetime of the FastAPI
process. The instance is stored on ``app.state`` (set up by ``main.py``
in its ``lifespan`` hook) so that:

* tests can override the dependency with a fully mocked client;
* the lifespan hook can call ``connect()`` at startup and ``close()`` at
  shutdown without the route layer having to know about it.

The dependency below falls back to constructing a brand-new client if
``app.state`` has no ``daemon_client`` attribute — that path exists for
unit tests that build a bare ``APIRouter`` without going through the
full lifespan.
"""

from __future__ import annotations

from fastapi import Request

from client import DaemonClient


def get_daemon_client(request: Request) -> DaemonClient:
    """Return the process-wide :class:`DaemonClient`.

    Resolution order
    ----------------
    1. ``request.app.state.daemon_client`` (set by ``main.lifespan``).
    2. A freshly-constructed :class:`DaemonClient` that auto-connects
       on first use (test-only fallback, no lifespan context).

    FastAPI's dependency-injection machinery will call this once per
    request, but the underlying client object is shared across all
    requests, so connections and reconnect state persist between calls.
    """
    client: DaemonClient | None = getattr(request.app.state, "daemon_client", None)
    if client is None:
        client = DaemonClient()
        request.app.state.daemon_client = client
    return client


__all__ = ["get_daemon_client"]
