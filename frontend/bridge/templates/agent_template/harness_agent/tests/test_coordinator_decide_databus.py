"""Tests for coordinator_decide.py — databus interaction patterns.

Covers:
  - P0: collect_skill_results NOT called when completed_skill_result env var is set
        (avoids 30s databus.get timeout)
  - P1: response key cleaned up after skill dispatch
        (prevents stale progress message leaking into next round)
"""

import importlib.util
import json
import sys
from pathlib import Path
from typing import Any

import pytest


MODULE_PATH = Path(__file__).parents[1] / "nodes" / "coordinator_decide.py"
SPEC = importlib.util.spec_from_file_location("coordinator_decide_databus", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


# ── Helpers ──


_SAMPLE_LLM_FINAL = json.dumps({
    "choices": [{
        "message": {
            "role": "assistant",
            "content": "这是最终穿搭方案。",
            "tool_calls": None,
        },
        "finish_reason": "stop",
    }],
    "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
})

_SAMPLE_LLM_DISPATCH = json.dumps({
    "choices": [{
        "message": {
            "role": "assistant",
            "content": "",
            "tool_calls": [{
                "id": "call_1",
                "type": "function",
                "function": {
                    "name": "run_skills",
                    "arguments": json.dumps({
                        "skills": [{"name": "outfit-stylist", "prompt": "test prompt"}],
                        "reason": "test",
                    }),
                },
            }],
        },
        "finish_reason": "tool_calls",
    }],
    "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
})


def _make_fake_llm(output_json: str):
    """Return a call_llm mock that returns a dict with the given JSON."""
    def fake_call_llm(*args, **kwargs):
        return json.loads(output_json)
    return fake_call_llm


# ═══════════════════════════════════════════════════════════════════
# P0: collect_skill_results skipped when AF_SKILL_RESULT is set
# ═══════════════════════════════════════════════════════════════════


def test_collect_skill_results_not_called_when_env_var_has_result(monkeypatch):
    """Red → Green: collect_skill_results is NOT called when AF_SKILL_RESULT is set.

    Current behavior (bug): collect_skill_results() is always evaluated as a
    function argument, even when completed_skill_result is already available
    via the AF_SKILL_RESULT env var. This causes a 30s databus.get() timeout
    in production when the key doesn't exist.

    Fixed behavior: skip collect_skill_results() when env var has the data.
    """
    # Arrange: track calls to collect_skill_results
    call_count = {"n": 0}
    original = MODULE.collect_skill_results

    def tracking(*args, **kwargs):
        call_count["n"] += 1
        return original(*args, **kwargs)

    monkeypatch.setattr(MODULE, "collect_skill_results", tracking)

    # Prevent actual LLM call and exit
    monkeypatch.setattr(MODULE, "call_llm", _make_fake_llm(_SAMPLE_LLM_FINAL))

    # Simulate skill completion with env var set
    monkeypatch.setenv("AF_INPUT", "推荐登山穿搭")
    monkeypatch.setenv("AF_DIALOGUE_ID", "test-dialogue-p0")
    monkeypatch.setenv("AF_SESSION_ID", "test-session-p0")
    monkeypatch.setenv("AF_COMPLETED_SKILL_NAME", "outfit-stylist")
    monkeypatch.setenv("AF_SKILL_SUCCESS", "true")
    monkeypatch.setenv("AF_SKILL_RESULT", json.dumps({"status": "completed", "data": "方案A"}))
    monkeypatch.setenv("AF_LLM_API_KEY", "test-key")

    # Act: run execute() — this calls build_skill_completion_message internally
    MODULE.execute()

    # Assert: collect_skill_results should NOT have been called
    # (env var AF_SKILL_RESULT is the authoritative source, databus is fallback)
    assert call_count["n"] == 0, (
        f"collect_skill_results was called {call_count['n']} time(s), "
        f"expected 0 when AF_SKILL_RESULT is set"
    )


def test_collect_skill_results_called_when_env_var_empty(monkeypatch):
    """collect_skill_results IS called when AF_SKILL_RESULT is not set (fallback).

    This proves the test methodology works: when the env var is absent,
    the databus fallback IS invoked.
    """
    # Arrange
    call_count = {"n": 0}
    original = MODULE.collect_skill_results

    def tracking(*args, **kwargs):
        call_count["n"] += 1
        return original(*args, **kwargs)

    monkeypatch.setattr(MODULE, "collect_skill_results", tracking)
    monkeypatch.setattr(MODULE, "call_llm", _make_fake_llm(_SAMPLE_LLM_FINAL))

    monkeypatch.setenv("AF_INPUT", "推荐登山穿搭")
    monkeypatch.setenv("AF_DIALOGUE_ID", "test-dialogue-p0b")
    monkeypatch.setenv("AF_SESSION_ID", "test-session-p0b")
    monkeypatch.setenv("AF_COMPLETED_SKILL_NAME", "outfit-stylist")
    monkeypatch.setenv("AF_SKILL_SUCCESS", "true")
    # Intentionally NOT setting AF_SKILL_RESULT
    monkeypatch.setenv("AF_LLM_API_KEY", "test-key")

    # Act
    MODULE.execute()

    # Assert: fallback path invoked collect_skill_results
    assert call_count["n"] >= 1, (
        "collect_skill_results should be called when AF_SKILL_RESULT is not set"
    )


# ═══════════════════════════════════════════════════════════════════
# P1: response key cleanup after skill dispatch
# ═══════════════════════════════════════════════════════════════════


def test_response_key_deleted_after_dispatch(monkeypatch):
    """Red → Green: response key is cleaned up after dispatching skills.

    After round 1 dispatches skills, the framework stores a progress message
    under the "response" databus key. If not cleaned up, round 2's framework
    handler finds the stale response and sends it to the client pre-emptively.

    Fixed behavior: dispatch path calls databus.delete("response") to avoid
    leaking stale progress into the next round.
    """
    # Arrange: track databus operations
    deleted_keys = []

    class FakeDataBus:
        """Minimal mock that only exposes what the dispatch path needs."""

        def __init__(self):
            self._fake_client = FakeRawClient()
            self._connected = True

        def is_connected(self):
            return self._connected

        def get(self, key: str) -> str | None:
            return None

        def put(self, key: str, value: str) -> bool:
            return True

        def delete(self, key: str) -> bool:
            deleted_keys.append(key)
            return True

        @property
        def _client(self):
            return self._fake_client

    class FakeRawClient:
        def raw(self):
            return self

        def put(self, key, value):
            pass

    monkeypatch.setattr(MODULE, "DataBusClient", lambda: FakeDataBus())
    monkeypatch.setattr(MODULE, "call_llm", _make_fake_llm(_SAMPLE_LLM_DISPATCH))

    # Prevent handle_run_skills_call from doing real work
    monkeypatch.setattr(MODULE, "handle_run_skills_call",
                        lambda *a, **kw: json.dumps({"status": "dispatched", "skills": ["outfit-stylist"]}))

    monkeypatch.setenv("AF_INPUT", "推荐登山穿搭")
    monkeypatch.setenv("AF_DIALOGUE_ID", "test-dialogue-p1")
    monkeypatch.setenv("AF_SESSION_ID", "test-session-p1")
    # First trigger (no completed_skill) — should enter dispatch path
    monkeypatch.setenv("AF_LLM_API_KEY", "test-key")

    # Act
    MODULE.execute()

    # Assert: response key was deleted
    assert "response" in deleted_keys, (
        "dispatch path must delete the 'response' databus key to prevent stale "
        "progress messages leaking into the next coordinator round"
    )
