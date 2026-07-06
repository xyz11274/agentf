"""Abstract session persistence provider interface.

The Bridge owns its own sessions (Bridge sessions) decoupled from the
agentflow daemon's ephemeral session pool. A `SessionProvider` persists
those Bridge sessions, their messages, and execution traces so the
frontend can list past conversations across restarts.

Two implementations:
    * `LocalSessionProvider` — SQLite file (default, single-host deploys)
    * `KMMSessionProvider`   — aisf platform KMM HTTP service (optional)

See `docs/design/features/bridge-session-persistence.md` §6 Provider layer.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field


@dataclass
class ContextResult:
    """Unified output of `get_context()`.

    `LocalSessionProvider` returns messages-only with an empty `memories`
    list (no long-term memory concept). `KMMSessionProvider` populates
    `memories` with relevant long-term memory entries from the KMM service.
    """

    messages: list[dict] = field(default_factory=list)
    """Historical chat messages as ``[{"role": "user|assistant", "content": "..."}, ...]``."""

    memories: list[str] = field(default_factory=list)
    """Relevant long-term memory entries as plain text strings."""


class SessionProvider(ABC):
    """Bridge-side session persistence contract.

    The contract is intentionally storage-agnostic: implementations may
    use SQLite, an HTTP service (KMM), or anything else. Routes consume
    this interface so they can be swapped via configuration without
    touching business logic.

    Invariants enforced across implementations:
        * Bridge session IDs are stable across agentflow session recycling
          (I-1: Bridge session does not depend on agentflow session ID).
        * Provider failures MUST NOT raise to the chat main flow
          (I-5: silent skip on persistence failure).
    """

    # ── Session management ──────────────────────────────────────────

    @abstractmethod
    async def get_or_create_session(
        self, user_id: str, client_id: str,
    ) -> dict:
        """Find or create a Bridge session keyed by ``(user_id, client_id)``.

        Returns the session dict (``id, user_id, client_id, title,
        created_at, last_active_at, status``).
        """
        ...

    @abstractmethod
    async def list_sessions(
        self, user_id: str = "", limit: int = 20, offset: int = 0,
    ) -> list[dict]:
        """List sessions for display, newest first.

        If ``user_id`` is empty, list across all users (admin/debug use).
        """
        ...

    @abstractmethod
    async def get_session(self, session_id: str) -> dict | None:
        """Return one session dict by id, or ``None`` if not found."""
        ...

    @abstractmethod
    async def find_by_agentflow_session(
        self, agentflow_session_id: str,
    ) -> dict | None:
        """Return the Bridge session currently associated with an
        agentflow session id, or ``None``."""
        ...

    @abstractmethod
    async def update_agentflow_session_ref(
        self, session_id: str, agentflow_session_id: str,
    ) -> None:
        """Update the ``agentflow_session_id`` reference on a Bridge session."""
        ...

    @abstractmethod
    async def update_session_status(
        self, session_id: str, status: str,
    ) -> None:
        """Update session status: ``active`` / ``archived`` / ``deleted``."""
        ...

    @abstractmethod
    async def delete_session(
        self, session_id: str,
    ) -> None:
        """Hard-delete a session and all its associated traces + dialogue data."""
        ...

    @abstractmethod
    async def update_session_title(
        self, session_id: str, title: str,
    ) -> None:
        """Update session title."""
        ...

    @abstractmethod
    async def delete_session(self, session_id: str) -> bool:
        """Physically delete a persistent session and all its associated
        data (dialogues, traces) from the provider.

        Returns ``True`` if the session existed and was deleted, ``False``
        if no session with that id was found.
        """
        ...

    # ── Message / dialogue storage ──────────────────────────────────

    @abstractmethod
    async def save_dialogue(
        self, session_id: str,
        user_input: str, agent_response: str,
        dialogue_id: str, agent_name: str = "",
        artifacts: str = "[]",
    ) -> str:
        """Persist one dialogue round (user + assistant message pair).

        ``artifacts`` is a JSON-encoded list of ``ArtifactInfo`` dicts
        extracted from ``agent_response``. Defaults to ``"[]"``.

        Returns the persisted dialogue_id. Title is auto-set from
        ``user_input``'s first 50 chars on the session's first dialogue.
        """
        ...

    @abstractmethod
    async def get_messages(
        self, session_id: str, limit: int = 50,
    ) -> list[dict]:
        """Return historical messages for a session, oldest-first within
        the returned window.

        Each entry: ``{role, content, dialogue_id, artifacts, created_at}``.
        """
        ...

    # ── Context injection (before chat) ─────────────────────────────

    @abstractmethod
    async def get_context(
        self, session_id: str, user_id: str,
        query: str = "", limit_rounds: int = 5,
    ) -> ContextResult:
        """Return historical context + relevant memories for the next turn.

        ``LocalSessionProvider`` returns recent messages with empty memories.
        ``KMMSessionProvider`` delegates to the KMM context/query endpoint.
        """
        ...

    # ── Trace storage ───────────────────────────────────────────────

    @abstractmethod
    async def save_trace(
        self, session_id: str,
        dialogue_id: str, trace_type: str, trace_data: dict,
    ) -> None:
        """Persist one LLM/execution trace entry."""
        ...

    @abstractmethod
    async def get_traces(
        self, session_id: str, dialogue_id: str = "",
    ) -> list[dict]:
        """Return traces for a session (optionally filtered by dialogue)."""
        ...


__all__ = ["ContextResult", "SessionProvider"]
