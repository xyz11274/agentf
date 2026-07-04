"""In-memory conversation store for the bridge.

Stores completed turn responses keyed by ``(agent_id, user_id, client_id)``
so the frontend can list and retrieve past conversations without relying on
the daemon's ephemeral dialogue lifecycle.
"""

from __future__ import annotations

import logging
import threading
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

logger = logging.getLogger("agentflow.bridge.conversation_store")


@dataclass
class ConversationRecord:
    """A single completed conversation turn."""

    agent_id: str
    user_id: str
    client_id: str
    dialogue_id: str
    content: str
    created_at: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat(timespec="seconds"))


class ConversationStore:
    """Thread-safe in-memory store for completed conversations.

    Each conversation is keyed by ``(agent_id, user_id, client_id, dialogue_id)``.
    A secondary index by ``(agent_id, user_id, client_id)`` enables listing.
    """

    def __init__(self, max_per_user: int = 100) -> None:
        self._lock = threading.Lock()
        self._max_per_user = max_per_user
        # Primary: dialogue_id → ConversationRecord
        self._by_id: dict[str, ConversationRecord] = {}
        # Secondary: (agent_id, user_id, client_id) → list of dialogue_ids (newest first)
        self._by_user: dict[tuple[str, str, str], list[str]] = {}

    def save(self, record: ConversationRecord) -> None:
        """Save a conversation record."""
        if not record.dialogue_id:
            return
        with self._lock:
            self._by_id[record.dialogue_id] = record
            key = (record.agent_id, record.user_id, record.client_id)
            if key not in self._by_user:
                self._by_user[key] = []
            # Prepend to keep newest first
            self._by_user[key].insert(0, record.dialogue_id)
            # Enforce max per user
            while len(self._by_user[key]) > self._max_per_user:
                old_id = self._by_user[key].pop()
                self._by_id.pop(old_id, None)

    def get(self, dialogue_id: str) -> ConversationRecord | None:
        """Get a conversation by dialogue_id."""
        with self._lock:
            return self._by_id.get(dialogue_id)

    def list_by_user(self, agent_id: str, user_id: str,
                     client_id: str, limit: int = 20) -> list[ConversationRecord]:
        """List conversations for a user, newest first."""
        key = (agent_id, user_id, client_id)
        with self._lock:
            ids = self._by_user.get(key, [])
            records = []
            for did in ids[:limit]:
                record = self._by_id.get(did)
                if record:
                    records.append(record)
            return records

    def delete(self, dialogue_id: str) -> None:
        """Delete a conversation by dialogue_id."""
        with self._lock:
            record = self._by_id.pop(dialogue_id, None)
            if record:
                key = (record.agent_id, record.user_id, record.client_id)
                ids = self._by_user.get(key, [])
                if dialogue_id in ids:
                    ids.remove(dialogue_id)


# Singleton
_store: ConversationStore | None = None


def get_store() -> ConversationStore:
    global _store
    if _store is None:
        _store = ConversationStore()
    return _store
