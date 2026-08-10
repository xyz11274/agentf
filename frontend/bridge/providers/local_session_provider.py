"""SQLite-backed `SessionProvider` for local / single-host deployments.

Schema (per `docs/design/features/bridge-session-persistence.md` §6.1):
    * ``sessions``   — Bridge-side persistent session metadata
    * ``dialogue_rounds`` — one-question / one-answer pairs (一期必建)
    * ``traces``     — LLM execution traces (Bridge-only, never sent to KMM)

The provider is safe to construct concurrently but the underlying SQLite
connection is guarded by an ``asyncio.Lock`` so writes serialise. SQLite's
default ``journal_mode=wal`` is enabled to allow concurrent readers.

Invariants:
    * I-1: Bridge session IDs are UUID4 strings; the agentflow session id
      is stored only as a non-key reference field.
    * I-5: All write operations catch exceptions and log; they do not raise
      to the chat main flow. Read operations surface errors to the caller
      because a missing session on read is meaningful.
"""

from __future__ import annotations

import contextlib
import json
import logging
import math
import sqlite3
import uuid
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from .session_provider import ContextResult, SessionProvider

logger = logging.getLogger("agentflow.bridge.providers.local")


_SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL,
    client_id TEXT,
    agentflow_session_id TEXT,
    title TEXT,
    created_at TEXT NOT NULL,
    last_active_at TEXT NOT NULL,
    status TEXT DEFAULT 'active',
    metadata TEXT
);

CREATE TABLE IF NOT EXISTS dialogue_rounds (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    dialogue_id TEXT NOT NULL,
    user_input TEXT NOT NULL,
    agent_response TEXT NOT NULL,
    agent_name TEXT DEFAULT '',
    artifacts TEXT DEFAULT '[]',
    images TEXT DEFAULT '[]',
    created_at TEXT NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);

CREATE TABLE IF NOT EXISTS traces (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    dialogue_id TEXT,
    trace_type TEXT NOT NULL,
    trace_data TEXT NOT NULL,
    created_at TEXT NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);

CREATE INDEX IF NOT EXISTS idx_dialogue_session ON dialogue_rounds(session_id, created_at);
CREATE INDEX IF NOT EXISTS idx_traces_session ON traces(session_id, dialogue_id);
"""


def _now_iso() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds")


def _new_session_id() -> str:
    return uuid.uuid4().hex


class LocalSessionProvider(SessionProvider):
    """SQLite file-based `SessionProvider`.

    Constructed with a filesystem path (defaults to ``<uploads>/sessions.db``).
    The schema is created lazily on first access if the file did not exist.
    """

    def __init__(self, db_path: str | Path) -> None:
        self._db_path = str(db_path)
        self._init_done = False

    # ------------------------------------------------------------------
    # Connection helpers
    # ------------------------------------------------------------------

    def _connect(self) -> sqlite3.Connection:
        db_path = Path(self._db_path)
        db_path.parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(str(db_path), isolation_level=None)
        conn.row_factory = sqlite3.Row
        # WAL allows concurrent readers while a writer holds the lock.
        with contextlib.suppress(sqlite3.Error):
            conn.execute("PRAGMA journal_mode=WAL")
        if not self._init_done:
            conn.executescript(_SCHEMA)
            # 存量库迁移：旧版 dialogue_rounds 无 images 列，ALTER 补上
            # （CREATE TABLE IF NOT EXISTS 不会给已存在的表加列）
            try:
                conn.execute(
                    "ALTER TABLE dialogue_rounds ADD COLUMN images TEXT DEFAULT '[]'"
                )
            except sqlite3.OperationalError:
                pass  # 列已存在（新库）
            self._init_done = True
        return conn

    @staticmethod
    def _row_to_session(row: sqlite3.Row) -> dict[str, Any]:
        return {
            "id": row["id"],
            "user_id": row["user_id"],
            "client_id": row["client_id"] or "",
            "agentflow_session_id": row["agentflow_session_id"] or "",
            "title": row["title"] or "",
            "created_at": row["created_at"],
            "last_active_at": row["last_active_at"],
            "status": row["status"] or "active",
            "metadata": json.loads(row["metadata"]) if row["metadata"] else {},
        }

    # ------------------------------------------------------------------
    # Session management
    # ------------------------------------------------------------------

    async def get_or_create_session(
        self, user_id: str, client_id: str,
    ) -> dict:
        now = _now_iso()
        with self._connect() as conn:
            row = conn.execute(
                "SELECT * FROM sessions WHERE user_id=? AND client_id=? ORDER BY created_at DESC LIMIT 1",
                (user_id, client_id),
            ).fetchone()
            if row is not None:
                # bump last_active_at
                conn.execute(
                    "UPDATE sessions SET last_active_at=? WHERE id=?",
                    (now, row["id"]),
                )
                sess = self._row_to_session(row)
                sess["last_active_at"] = now
                logger.info("local_provider.session_found id=%s user=%s client=%s",
                            row["id"], user_id, client_id)
                return sess

            sid = _new_session_id()
            conn.execute(
                "INSERT INTO sessions (id, user_id, client_id, created_at, last_active_at, status)"
                " VALUES (?, ?, ?, ?, ?, 'active')",
                (sid, user_id, client_id, now, now),
            )
        logger.info("local_provider.session_created id=%s user=%s client=%s", sid, user_id, client_id)
        return {
            "id": sid, "user_id": user_id, "client_id": client_id,
            "agentflow_session_id": "", "title": "",
            "created_at": now, "last_active_at": now, "status": "active",
            "metadata": {},
        }

    async def list_sessions(
        self, user_id: str = "", limit: int = 20, offset: int = 0,
    ) -> list[dict]:
        with self._connect() as conn:
            if user_id:
                rows = conn.execute(
                    "SELECT * FROM sessions WHERE user_id=? AND status!='deleted'"
                    " ORDER BY last_active_at DESC LIMIT ? OFFSET ?",
                    (user_id, limit, offset),
                ).fetchall()
            else:
                rows = conn.execute(
                    "SELECT * FROM sessions WHERE status!='deleted'"
                    " ORDER BY last_active_at DESC LIMIT ? OFFSET ?",
                    (limit, offset),
                ).fetchall()
        return [self._row_to_session(r) for r in rows]

    async def get_session(self, session_id: str) -> dict | None:
        with self._connect() as conn:
            row = conn.execute(
                "SELECT * FROM sessions WHERE id=?", (session_id,),
            ).fetchone()
        return self._row_to_session(row) if row else None

    async def find_by_agentflow_session(
        self, agentflow_session_id: str,
    ) -> dict | None:
        with self._connect() as conn:
            row = conn.execute(
                "SELECT * FROM sessions WHERE agentflow_session_id=?"
                " ORDER BY last_active_at DESC LIMIT 1",
                (agentflow_session_id,),
            ).fetchone()
        return self._row_to_session(row) if row else None

    async def update_agentflow_session_ref(
        self, session_id: str, agentflow_session_id: str,
    ) -> None:
        try:
            with self._connect() as conn:
                conn.execute(
                    "UPDATE sessions SET agentflow_session_id=? WHERE id=?",
                    (agentflow_session_id, session_id),
                )
        except sqlite3.Error as exc:
            logger.warning("local_provider.update_af_ref failed id=%s: %s", session_id, exc)

    async def update_session_status(
        self, session_id: str, status: str,
    ) -> None:
        try:
            with self._connect() as conn:
                conn.execute(
                    "UPDATE sessions SET status=? WHERE id=?",
                    (status, session_id),
                )
        except sqlite3.Error as exc:
            logger.warning("local_provider.update_status failed id=%s: %s", session_id, exc)

    async def update_session_title(
        self, session_id: str, title: str,
    ) -> None:
        try:
            with self._connect() as conn:
                conn.execute(
                    "UPDATE sessions SET title=? WHERE id=?",
                    (title, session_id),
                )
        except sqlite3.Error as exc:
            logger.warning("local_provider.update_title failed id=%s: %s", session_id, exc)

    # ------------------------------------------------------------------
    # Message / dialogue storage
    # ------------------------------------------------------------------

    async def delete_session(self, session_id: str) -> bool:
        """Physically delete the session and cascade-delete all dialogues
        and traces. Returns True if the session existed, False otherwise."""
        try:
            with self._connect() as conn:
                # Check existence first.
                row = conn.execute(
                    "SELECT 1 FROM sessions WHERE id=?", (session_id,),
                ).fetchone()
                if row is None:
                    return False
                # Cascade: traces → dialogue_rounds → sessions
                conn.execute("DELETE FROM traces WHERE session_id=?", (session_id,))
                conn.execute("DELETE FROM dialogue_rounds WHERE session_id=?", (session_id,))
                conn.execute("DELETE FROM sessions WHERE id=?", (session_id,))
            logger.info("local_provider.session_deleted id=%s", session_id)
            return True
        except sqlite3.Error as exc:
            logger.warning("local_provider.delete_session failed id=%s: %s", session_id, exc)
            return False

    async def save_dialogue(
        self, session_id: str,
        user_input: str, agent_response: str,
        dialogue_id: str, agent_name: str = "",
        artifacts: str = "[]",
        images: str = "[]",
    ) -> str:
        now = _now_iso()
        try:
            with self._connect() as conn:
                # Auto-title: first dialogue sets the title from user_input[:50].
                sess = conn.execute(
                    "SELECT title FROM sessions WHERE id=?", (session_id,),
                ).fetchone()
                if sess is not None and not (sess["title"] or ""):
                    title = user_input.strip()[:50]
                    conn.execute(
                        "UPDATE sessions SET title=?, last_active_at=? WHERE id=?",
                        (title, now, session_id),
                    )
                else:
                    conn.execute(
                        "UPDATE sessions SET last_active_at=? WHERE id=?",
                        (now, session_id),
                    )
                conn.execute(
                    "INSERT INTO dialogue_rounds"
                    " (session_id, dialogue_id, user_input, agent_response,"
                    "  agent_name, artifacts, images, created_at)"
                    " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                    (session_id, dialogue_id, user_input, agent_response,
                     agent_name, artifacts, images, now),
                )
                logger.info("local_provider.save_dialogue ok id=%s dialogue=%s input_len=%d resp_len=%d",
                            session_id, dialogue_id, len(user_input), len(agent_response))
        except sqlite3.Error as exc:
            # I-5: silent skip on persistence failure — never break the chat flow.
            logger.warning(
                "local_provider.save_dialogue failed id=%s dialogue=%s: %s",
                session_id, dialogue_id, exc,
            )
        return dialogue_id

    async def get_messages(
        self, session_id: str, limit: int = 50,
    ) -> list[dict]:
        """Return messages oldest-first as ``[{role, content, ...}]``.

        Each dialogue round yields two messages (user + assistant) in order.
        ``limit`` is the max number of **messages** returned, not rounds,
        so we translate to a row cap of ``ceil(limit / 2)`` on the SQL side.
        """
        row_cap = max(1, math.ceil(limit / 2))
        with self._connect() as conn:
            rows = conn.execute(
                "SELECT user_input, agent_response, agent_name, artifacts, images, dialogue_id, created_at"
                " FROM dialogue_rounds WHERE session_id=?"
                " ORDER BY created_at ASC LIMIT ?",
                (session_id, row_cap),
            ).fetchall()
        messages: list[dict[str, Any]] = []
        for r in rows:
            artifacts = json.loads(r["artifacts"]) if r["artifacts"] else []
            images = json.loads(r["images"]) if r["images"] else []
            messages.append({
                "role": "user",
                "content": r["user_input"],
                "images": images,
                "dialogue_id": r["dialogue_id"],
                "artifacts": [],
                "created_at": r["created_at"],
            })
            messages.append({
                "role": "assistant",
                "content": r["agent_response"],
                "dialogue_id": r["dialogue_id"],
                "artifacts": artifacts,
                "agent_name": r["agent_name"] or "",
                "created_at": r["created_at"],
            })
        # Trim to the exact message cap in case the last round overflowed.
        return messages[:limit]

    # ------------------------------------------------------------------
    # Context injection
    # ------------------------------------------------------------------

    async def get_context(
        self, session_id: str, user_id: str,
        query: str = "", limit_rounds: int = 5,
    ) -> ContextResult:
        """Return the last ``limit_rounds`` dialogue pairs as messages.

        No long-term memory concept locally — ``memories`` is always empty.
        """
        # Each round = 2 messages, so fetch 2 * limit_rounds messages from the tail.
        # We reuse get_messages but need only the last N pairs.
        with self._connect() as conn:
            rows = conn.execute(
                "SELECT user_input, agent_response, dialogue_id, created_at"
                " FROM dialogue_rounds WHERE session_id=?"
                " ORDER BY created_at DESC LIMIT ?",
                (session_id, limit_rounds),
            ).fetchall()
        # Reverse to oldest-first for the LLM input.
        rows.reverse()
        messages: list[dict[str, Any]] = []
        for r in rows:
            messages.append({"role": "user", "content": r["user_input"]})
            messages.append({"role": "assistant", "content": r["agent_response"]})
        logger.info("local_provider.get_context session=%s rounds=%d msgs=%d",
                    session_id, len(rows), len(messages))
        return ContextResult(messages=messages, memories=[])

    # ------------------------------------------------------------------
    # Trace storage
    # ------------------------------------------------------------------

    async def save_trace(
        self, session_id: str,
        dialogue_id: str, trace_type: str, trace_data: dict,
    ) -> None:
        now = _now_iso()
        try:
            with self._connect() as conn:
                conn.execute(
                    "INSERT INTO traces (session_id, dialogue_id, trace_type, trace_data, created_at)"
                    " VALUES (?, ?, ?, ?, ?)",
                    (session_id, dialogue_id, trace_type, json.dumps(trace_data, ensure_ascii=False), now),
                )
            logger.info(
                "local_provider.save_trace_ok id=%s dialogue=%s type=%s call_id=%s",
                session_id, dialogue_id, trace_type,
                trace_data.get("call_id", "?"),
            )
        except sqlite3.Error as exc:
            # I-5: trace persistence is non-critical — log and continue.
            logger.warning(
                "local_provider.save_trace_failed id=%s dialogue=%s: %s",
                session_id, dialogue_id, exc,
            )

    async def get_traces(
        self, session_id: str, dialogue_id: str = "",
    ) -> list[dict]:
        with self._connect() as conn:
            if dialogue_id:
                rows = conn.execute(
                    "SELECT * FROM traces WHERE session_id=? AND dialogue_id=? ORDER BY id ASC",
                    (session_id, dialogue_id),
                ).fetchall()
            else:
                rows = conn.execute(
                    "SELECT * FROM traces WHERE session_id=? ORDER BY id ASC",
                    (session_id,),
                ).fetchall()
        traces: list[dict[str, Any]] = []
        for r in rows:
            try:
                data = json.loads(r["trace_data"])
            except (json.JSONDecodeError, TypeError):
                data = {}
            traces.append({
                "id": r["id"],
                "session_id": r["session_id"],
                "dialogue_id": r["dialogue_id"] or "",
                "trace_type": r["trace_type"],
                "trace_data": data,
                "created_at": r["created_at"],
            })
        return traces


__all__ = ["LocalSessionProvider"]
