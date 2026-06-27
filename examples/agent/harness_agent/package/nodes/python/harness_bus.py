#!/usr/bin/env python3
"""DataBus + environment bridge for harness_agent nodes.

Two kinds of cross-node state, handled by the right mechanism:

  * **Loop control** (``next_skills`` / ``response`` / ``coordinator_done``):
    emitted as declared ``json_output`` keys. The framework publishes these to
    the namespaced central DataBus (``/session/{sid}/sys/{dialogue}/{key}``)
    where the agent loop reads them. (Verified against the runtime.)

  * **Loop accumulation** (per-skill result envelopes + loop state): the worker
    gives each node a *per-execution* temp file DataBus, so it cannot be used to
    accumulate state across nodes. Instead we use a tiny file-based store under
    ``package/runtime/state/{session}_{dialogue}.json`` — reliable for the local
    single-host runtime, and keyed by session+dialogue for isolation.

Nodes receive ``session_id`` / ``dialogue_id`` / ``user_text`` as declared
inputs (exposed as ``AF_SESSION_ID`` / ``AF_DIALOGUE_ID`` / ``AF_USER_TEXT``).
"""
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict

NODE_DIR = Path(__file__).resolve().parent
PACKAGE_DIR = NODE_DIR.parent.parent
STATE_DIR = PACKAGE_DIR / "runtime" / "state"
MARKER_DIR = PACKAGE_DIR / "runtime" / "markers"


def env(name: str, default: str = "") -> str:
    return os.environ.get(name, default).strip()


def session_id() -> str:
    return env("AF_SESSION_ID", "default")


def dialogue_id() -> str:
    return env("AF_DIALOGUE_ID", "nodlg")


def user_text() -> str:
    return env("AF_USER_TEXT") or env("AF_INPUT") or env("AF_USER_PROMPT")


def completed_skill_name() -> str:
    return env("AF_COMPLETED_SKILL_NAME")


def emit_json(payload: Dict[str, Any]) -> None:
    print(json.dumps(payload, ensure_ascii=False))


def _sanitize(part: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in part)



def _marker_base() -> str:
    return "%s__%s" % (_sanitize(session_id()), _sanitize(dialogue_id()))


def write_marker(suffix: str, content: str) -> None:
    MARKER_DIR.mkdir(parents=True, exist_ok=True)
    (MARKER_DIR / ("%s.%s" % (_marker_base(), suffix))).write_text(
        content, encoding="utf-8")


class Store:
    """File-based per-(session,dialogue) shared store for loop accumulation."""

    def __init__(self) -> None:
        STATE_DIR.mkdir(parents=True, exist_ok=True)
        fname = "%s__%s.json" % (_sanitize(session_id()), _sanitize(dialogue_id()))
        self._path = STATE_DIR / fname

    def _load(self) -> Dict[str, Any]:
        try:
            return json.loads(self._path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return {"results": {}, "state": {}}

    def _save(self, data: Dict[str, Any]) -> None:
        tmp = self._path.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
        tmp.replace(self._path)

    # --- results ---------------------------------------------------------- #
    def save_result(self, skill: str, envelope: Dict[str, Any]) -> None:
        data = self._load()
        data.setdefault("results", {})[skill] = envelope
        self._save(data)

    def load_results(self) -> Dict[str, Dict[str, Any]]:
        return self._load().get("results", {})

    # --- loop state ------------------------------------------------------- #
    def load_state(self) -> Dict[str, Any]:
        return self._load().get("state", {})

    def save_state(self, state: Dict[str, Any]) -> None:
        data = self._load()
        data["state"] = state
        self._save(data)

    # --- original user_text ---------------------------------------------- #
    def save_user_text(self, text: str) -> None:
        if not text:
            return
        data = self._load()
        if not data.get("user_text"):
            data["user_text"] = text
            self._save(data)

    def load_user_text(self) -> str:
        return self._load().get("user_text", "")

    def reset(self) -> None:
        try:
            self._path.unlink()
        except OSError:
            pass
