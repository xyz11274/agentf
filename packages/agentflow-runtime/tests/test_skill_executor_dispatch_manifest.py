"""Dispatch-manifest preflight regression tests for Skill Executor."""

from __future__ import annotations

import io
import json
from unittest.mock import MagicMock, patch

import pytest


class _FakeDataBus:
    def __init__(self, raw=None, connected=True):
        self.raw = raw
        self.connected = connected
        self.put_calls = []

    def is_connected(self):
        return self.connected

    def get(self, _key):
        return None

    def get_global(self, _key):
        return self.raw

    def put(self, key, value):
        self.put_calls.append((key, value))
        return True


@pytest.mark.parametrize(
    ("exec_skill_key", "raw", "connected", "expected_error"),
    [
        ("", None, True, "missing_exec_skill_key"),
        ("dispatch:1", None, False, "databus_unavailable"),
        ("dispatch:1", None, True, "manifest_not_found"),
        ("dispatch:1", "{broken", True, "manifest_invalid_json"),
        ("dispatch:1", "[]", True, "manifest_not_object"),
        (
            "dispatch:1",
            {"prompt": "do work", "load_skills": []},
            True,
            "manifest_missing_skill_name",
        ),
        (
            "dispatch:1",
            {"skill_name": "worker", "prompt": "", "load_skills": []},
            True,
            "manifest_invalid_prompt",
        ),
        (
            "dispatch:1",
            {"skill_name": "worker", "prompt": "do work", "load_skills": "worker"},
            True,
            "manifest_invalid_load_skills",
        ),
    ],
)
def test_dispatch_manifest_validation(
    exec_skill_key, raw, connected, expected_error
):
    from agentflow_runtime import skill_executor

    manifest, error = skill_executor._read_dispatch_manifest(
        _FakeDataBus(raw=raw, connected=connected), exec_skill_key
    )

    assert expected_error in error
    if expected_error.startswith("manifest_invalid_") or expected_error == "manifest_missing_skill_name":
        assert manifest is None or isinstance(manifest, dict)


def test_valid_dispatch_manifest_is_preserved():
    from agentflow_runtime import skill_executor

    expected = {
        "skill_name": "outfit-stylist",
        "prompt": "推荐穿搭",
        "load_skills": ["outfit-stylist"],
        "batch_id": "batch:dialogue:round",
    }

    manifest, error = skill_executor._read_dispatch_manifest(
        _FakeDataBus(raw=json.dumps(expected, ensure_ascii=False)),
        "dispatch:dialogue:round:1",
    )

    assert error == ""
    assert manifest == expected


def test_missing_manifest_ends_before_llm_or_tools(monkeypatch):
    """A broken dispatch becomes a replayable business failure, not unknown-skill."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus(raw=None, connected=True)
    span = MagicMock()
    span_cm = MagicMock()
    span_cm.__enter__.return_value = span
    hook_events = []

    def capture_hook(name, payload, **_kwargs):
        hook_events.append((name, payload))
        return payload

    monkeypatch.setenv("AF_INPUT", "原始用户输入不能被当成 Skill prompt")
    monkeypatch.setenv("AF_DIALOGUE_ID", "dialogue-manifest-error")
    monkeypatch.setenv("AF_SESSION_ID", "session-manifest-error")
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:missing:1")
    monkeypatch.setenv("AF_SKIP_DOTENV", "1")

    with patch.object(skill_executor, "_load_dotenv"), \
         patch.object(skill_executor, "_inject_venv_env"), \
         patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "emit_span", return_value=span_cm), \
         patch.object(skill_executor, "load_hooks_from_dir"), \
         patch.object(skill_executor, "hook_trigger", side_effect=capture_hook), \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "load_skills_from_list") as load_docs, \
         patch.object(skill_executor, "_LocalToolRegistry") as registry, \
         patch.object(skill_executor, "resolve_llm_config") as resolve_model, \
         patch.object(skill_executor, "call_llm") as call_llm, \
         patch("sys.stdout", io.StringIO()) as stdout:
        skill_executor.execute()

    output = json.loads(stdout.getvalue().strip())
    assert output["status"] == "dispatch_manifest_error"
    assert output["skill_name"] == ""
    assert output["rounds_used"] == 0
    assert output["error"] == "manifest_not_found"
    assert "unknown-skill" not in output["skill_result"]

    stored = json.loads(dict(fake_bus.put_calls)["skill_result:skill-runner"])
    assert stored == output
    load_docs.assert_not_called()
    registry.assert_not_called()
    resolve_model.assert_not_called()
    call_llm.assert_not_called()

    assert [name for name, _ in hook_events] == [
        skill_executor.hook_events.SKILL_EXECUTOR_TRIGGER,
        skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE,
    ]
    assert hook_events[-1][1]["status"] == "dispatch_manifest_error"
    assert hook_events[-1][1]["skill_result"] == output["skill_result"]
    assert hook_events[-1][1]["error"] == "manifest_not_found"
    span.set_status.assert_called_once()
    assert span.set_status.call_args.args[0] == skill_executor.SpanStatus.ERROR
