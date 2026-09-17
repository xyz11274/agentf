"""Transactional, conversation-scoped simulator state. No business service I/O."""
from __future__ import annotations

import copy
import hashlib
import json
import os
import sqlite3
from contextlib import contextmanager
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[2]


SHARED_KEYS = {'devices', 'world', 'home_history', 'home_energy', 'home_intervals',
               'preferences', 'records', 'reminders', 'appointments', 'knowledge_documents',
               'virtual_device_world'}


class IdentityScope(str):
    def __new__(cls, value, owner):
        obj = super().__new__(cls, value)
        obj.owner = owner
        return obj


def scope_key(user: str | None = None, client: str | None = None) -> str:
    user = os.environ.get("AF_USER_ID", "") if user is None else user
    client = os.environ.get("AF_CLIENT_ID", "") if client is None else client
    if not user or not client:
        raise ValueError("AF_USER_ID and AF_CLIENT_ID are required; session slot is not a user identity")
    value = hashlib.sha256(json.dumps([user, client], ensure_ascii=False).encode()).hexdigest()
    owner = 'user-' + hashlib.sha256(user.encode()).hexdigest()
    return IdentityScope(value, owner)


def database_path() -> Path:
    return Path(os.environ.get("BUSINESS_SIM_DB", str(PACKAGE / "outputs/business.sqlite3"))).resolve()


def fixtures() -> dict:
    return json.loads((PACKAGE / "data/fixtures.json").read_text(encoding="utf-8"))


def fresh_state() -> dict:
    devices = copy.deepcopy(fixtures()["devices"]) if os.environ.get("BUSINESS_EXECUTION_MODE", "real") == "fixture" else []
    state = {"devices": devices, "preferences": {},
            "records": {}, "reminders": {}, "appointments": {}, "home_history": [],
            "last_movies": [], "player": {}, "gui": {"app": "", "plans": []},
            "documents": {}, "plans": {}, "services": {}, "counters": {}}
    if os.environ.get("BUSINESS_EXECUTION_MODE", "real") == "real":
        from .device_world import ensure
        ensure(state)
    return state


def next_id(state: dict, kind: str) -> str:
    state["counters"][kind] = state["counters"].get(kind, 0) + 1
    return f"sim-{kind}-{state['counters'][kind]:04d}"


class Store:
    def __init__(self, path: Path | None = None):
        self.path = path or database_path()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS state (scope TEXT PRIMARY KEY, payload TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS scope_owners (scope TEXT PRIMARY KEY, owner TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS events (
                    id INTEGER PRIMARY KEY, scope TEXT NOT NULL, skill TEXT NOT NULL,
                    action TEXT NOT NULL, result TEXT NOT NULL,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
                CREATE INDEX IF NOT EXISTS events_scope ON events(scope, id);
                CREATE TABLE IF NOT EXISTS requests (
                    scope TEXT NOT NULL, skill TEXT NOT NULL, request_id TEXT NOT NULL,
                    fingerprint TEXT NOT NULL, result TEXT NOT NULL,
                    PRIMARY KEY(scope, skill, request_id));
            """)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=30)
        try:
            with db:
                yield db
        finally:
            db.close()

    def owner_scope(self, scope, db=None):
        if os.environ.get('BUSINESS_EXECUTION_MODE','real') == 'fixture':
            return str(scope)
        if db is None:
            with self.connect() as connection:
                return self.owner_scope(scope, connection)
        if isinstance(scope, IdentityScope):
            old = db.execute('SELECT owner FROM scope_owners WHERE scope=?', (str(scope),)).fetchone()
            if old and old[0] != scope.owner:
                raise ValueError('scope identity does not match registered owner')
            db.execute('INSERT OR IGNORE INTO scope_owners VALUES (?,?)', (str(scope),scope.owner))
            return scope.owner
        row = db.execute('SELECT owner FROM scope_owners WHERE scope=?', (scope,)).fetchone()
        return row[0] if row else str(scope)

    def _load(self, db, scope):
        owner = self.owner_scope(scope, db)
        row = db.execute('SELECT payload FROM state WHERE scope=?', (str(scope),)).fetchone()
        state = json.loads(row[0]) if row else fresh_state()
        if owner != scope:
            shared = db.execute('SELECT payload FROM state WHERE scope=?', (owner,)).fetchone()
            if shared:
                state.update(json.loads(shared[0]))
        return state

    def _persist(self, db, scope, state):
        owner = self.owner_scope(scope, db)
        if owner != scope:
            assets = {k:v for k,v in state.items() if k in SHARED_KEYS}
            db.execute('INSERT OR REPLACE INTO state VALUES (?,?)', (owner,json.dumps(assets,ensure_ascii=False)))
            state = {k:v for k,v in state.items() if k not in SHARED_KEYS}
        db.execute('INSERT OR REPLACE INTO state VALUES (?,?)', (str(scope),json.dumps(state,ensure_ascii=False)))

    def read(self, scope: str) -> dict:
        with self.connect() as db:
            return self._load(db, scope)

    def reset(self, scope: str, state: dict | None = None) -> None:
        # Operator/evaluation reset of a user's assets, never exposed to the LLM.
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            owner = self.owner_scope(scope,db)
            related = [scope,owner] + [row[0] for row in db.execute('SELECT scope FROM scope_owners WHERE owner=?',(owner,)).fetchall()]
            for identity in set(related):
                for table in ('events','requests','state'):
                    db.execute('DELETE FROM '+table+' WHERE scope=?',(identity,))
                for table in ('clinic_reservations','notifications'):
                    if db.execute('SELECT 1 FROM sqlite_master WHERE name=?',(table,)).fetchone():
                        db.execute('DELETE FROM '+table+' WHERE scope=?',(identity,))
            self._persist(db,scope,state if state is not None else fresh_state())

    def run(self, scope: str, skill: str, args: dict, handler, *, transactional=False) -> dict:
        fingerprint = hashlib.sha256(json.dumps(args, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        request_id = args.get("request_id", "")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            if request_id:
                old = db.execute("SELECT fingerprint,result FROM requests WHERE scope=? AND skill=? AND request_id=?",
                                 (scope, skill, request_id)).fetchone()
                if old:
                    if old[0] != fingerprint:
                        raise ValueError("request_id was already used with different arguments")
                    return {**json.loads(old[1]), "replayed": True}
            state = self._load(db, scope)
            db.execute("SAVEPOINT business_operation")
            result = handler(state, db) if transactional else handler(state)
            # Failed or incomplete operations never leave partial writes behind.
            if result["status"] == "succeeded":
                self._persist(db, scope, state)
                if request_id:
                    db.execute("INSERT INTO requests VALUES (?, ?, ?, ?, ?)",
                               (scope, skill, request_id, fingerprint, json.dumps(result, ensure_ascii=False)))
            else:
                # Roll back inventory alongside its owner state on incomplete operations.
                db.execute("ROLLBACK TO business_operation")
            db.execute("RELEASE business_operation")
            db.execute("INSERT INTO events(scope,skill,action,result) VALUES (?, ?, ?, ?)",
                       (scope, skill, args["action"], json.dumps(result, ensure_ascii=False)))
            return result

    def events(self, scope: str) -> list:
        with self.connect() as db:
            rows = db.execute("SELECT skill,action,result FROM events WHERE scope=? ORDER BY id", (scope,)).fetchall()
        return [{"skill": r[0], "action": r[1], "result": json.loads(r[2])} for r in rows]
