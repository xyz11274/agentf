"""AgentFlow Bridge — session persistence providers.

Provides the persistence layer for Bridge-side user sessions. The Provider
abstracts whether sessions are stored locally (SQLite) or remotely (KMM),
so the REST routes and chat completion logic remain storage-agnostic.

See `docs/design/features/bridge-session-persistence.md` for the design.
"""

from __future__ import annotations

from .local_session_provider import LocalSessionProvider
from .session_provider import (
    ContextResult,
    SessionProvider,
)

__all__ = [
    "ContextResult",
    "SessionProvider",
    "LocalSessionProvider",
]
