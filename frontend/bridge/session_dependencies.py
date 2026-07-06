"""Dependency provider for the Bridge `SessionProvider`.

Mirrors `dependencies.get_daemon_client`: the provider instance is owned
by `app.state.session_provider` (set up by `main.lifespan`) so it is shared
across requests and can be overridden in tests via `app.dependency_overrides`.
"""

from __future__ import annotations

from fastapi import Request

from providers import LocalSessionProvider, SessionProvider


def get_session_provider(request: Request) -> SessionProvider:
    """Return the process-wide :class:`SessionProvider`.

    Resolution order:
        1. ``request.app.state.session_provider`` (set by ``main.lifespan``).
        2. A freshly-constructed :class:`LocalSessionProvider` (test-only
           fallback, so unit tests can build a bare router without lifespan).
    """
    provider: SessionProvider | None = getattr(request.app.state, "session_provider", None)
    if provider is None:
        # Test fallback: use an in-memory temp path. Tests should override
        # this via app.dependency_overrides for predictable isolation.
        from config import settings
        provider = LocalSessionProvider(settings.session_db_path_resolved)
    return provider


__all__ = ["get_session_provider"]
