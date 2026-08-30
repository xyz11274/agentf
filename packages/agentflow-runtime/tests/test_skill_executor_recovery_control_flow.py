"""Regression tests for skill_executor semantic tool-call recovery."""

from __future__ import annotations

import io
import json
import os
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[3]
PKG_SRC = PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
if str(PKG_SRC) not in sys.path:
    sys.path.insert(0, str(PKG_SRC))


@pytest.fixture
def skill_env(monkeypatch):
    monkeypatch.setenv("AF_NODE_ID", "skill_executor")
    monkeypatch.setenv("AF_DIALOGUE_ID", "d_test_recovery")
    monkeypatch.setenv("AF_SESSION_ID", "ses_test_recovery")
    monkeypatch.setenv("AF_INPUT", "生成一个文件")
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:recovery:1")
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", "")
    monkeypatch.setenv("AF_SKIP_DOTENV", "1")
    monkeypatch.setenv(
        "AGENT_PACKAGE_DIR",
        str(PROJECT_ROOT / "examples" / "agent" / "outfit-assistant"),
    )
    monkeypatch.setenv("AF_LLM_API_KEY", "test-key")
    monkeypatch.setenv("AF_LLM_BASE_URL", "http://127.0.0.1:1/v1")
    return monkeypatch


class _FakeDataBus:
    def __init__(self):
        self.put_calls: list[tuple[str, str]] = []
        self.push_calls: list[tuple[str, dict]] = []
        self._dispatch = json.dumps({
            "skill_name": "file-writer",
            "prompt": "生成一个文件",
            "load_skills": [],
        })

    def is_connected(self) -> bool:
        return True

    def get(self, key: str) -> str | None:
        return None

    def get_global(self, key: str) -> str | None:
        if key.startswith("dispatch:"):
            return self._dispatch
        return None

    def put(self, key: str, value: str) -> bool:
        self.put_calls.append((key, value))
        return True

    def response_push(self, content: str, **kwargs) -> bool:
        self.push_calls.append((content, kwargs))
        return True


def _llm_response(*, content: str = "", tool_calls=None, finish: str = "stop") -> dict:
    message = {"role": "assistant", "content": content}
    if tool_calls:
        message["tool_calls"] = tool_calls
    return {
        "choices": [{"index": 0, "message": message, "finish_reason": finish}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
    }


def test_semantic_recovery_retries_when_progress_push_succeeds(skill_env):
    """A successful progress push must not bypass the semantic retry."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
    hook_calls = []

    def record_hook(name, payload, **_kwargs):
        hook_calls.append((name, payload))
        return payload
    truncated_call = [{
        "id": "call_broken",
        "type": "function",
        "function": {
            "name": "write_file",
            "arguments": '{"file_path":"out.txt","content":',
        },
    }]
    llm_responses = iter([
        _llm_response(tool_calls=truncated_call, finish="length"),
        _llm_response(content="恢复后完成"),
    ])
    observed_llm_timeouts: list[int] = []

    def llm_side_effect(*_args, **_kwargs):
        observed_llm_timeouts.append(int(os.environ["AF_LLM_TIMEOUT"]))
        return next(llm_responses)

    skill_env.setenv("AF_LLM_TIMEOUT", "7200")

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", side_effect=llm_side_effect) as mock_llm, \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "hook_trigger", side_effect=record_hook), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1",
         }):
        mock_cfg.skill_max_iterations.return_value = 5
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None
        mock_cfg.skill_llm_recovery_attempts.return_value = 2

        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    assert mock_llm.call_count == 2
    assert len(observed_llm_timeouts) == 2
    assert all(1 <= timeout < 3600 for timeout in observed_llm_timeouts)
    assert os.environ["AF_LLM_TIMEOUT"] == "7200"
    second_messages = mock_llm.call_args_list[1].args[0]
    assert any(
        message.get("content") == skill_executor.SKILL_RECOVERY_PROMPT
        for message in second_messages
    )
    assert mock_llm.call_args_list[1].kwargs["max_completion_tokens"] == 8000

    result = json.loads(dict(fake_bus.put_calls)["skill_result:skill-runner"])
    assert result["status"] == "completed"
    assert result["skill_result"] == "恢复后完成"
    assert any("正在恢复第 1 轮" in content for content, _ in fake_bus.push_calls)
    recovery_payloads = [
        payload for name, payload in hook_calls
        if name == skill_executor.hook_events.SKILL_EXECUTOR_RECOVERY_DECISION
    ]
    assert recovery_payloads == [{
        "skill_name": "file-writer",
        "round": 1,
        "next_round": 2,
        "action": "retry",
        "reason": "completion_truncated",
        "finish_reason": "length",
        "recovery_attempt": 1,
        "max_recovery_attempts": 2,
        "previous_completion_budget": 4000,
        "completion_budget": 8000,
        "stop_reason": "",
    }]


def test_nonzero_usage_empty_final_response_is_not_completed(skill_env):
    """A billed but content-free final response is a business failure."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
    hook_calls = []

    def record_hook(name, payload, **_kwargs):
        hook_calls.append((name, payload))
        return payload

    skill_span = MagicMock()
    skill_span_cm = MagicMock()
    skill_span_cm.__enter__.return_value = skill_span
    llm_span = MagicMock()
    llm_span_cm = MagicMock()
    llm_span_cm.__enter__.return_value = llm_span

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", return_value=_llm_response(content="")), \
         patch.object(skill_executor, "emit_span", side_effect=[skill_span_cm, llm_span_cm]), \
         patch.object(skill_executor, "emit_event") as emit_event, \
         patch.object(skill_executor, "hook_trigger", side_effect=record_hook), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1",
         }):
        mock_cfg.skill_max_iterations.return_value = 1
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None
        mock_cfg.skill_llm_recovery_attempts.return_value = 0

        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    result = json.loads(dict(fake_bus.put_calls)["skill_result:skill-runner"])
    assert result["status"] == "llm_error"
    assert result["skill_result"].startswith("LLM call failed:")
    assert result["error"] == (
        "LLM returned empty final response (no content and no tool calls)"
    )
    assert any(
        call.args[0] == skill_executor.hook_events.SKILL_EXECUTOR_EMPTY_RESULT
        for call in emit_event.call_args_list
    )
    after_payloads = [
        payload for name, payload in hook_calls
        if name == skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE
    ]
    assert after_payloads[0]["status"] == "llm_error"
    assert fake_bus.push_calls[-1][1]["status"] == "error"
    skill_span.set_status.assert_called_once_with(
        skill_executor.SpanStatus.ERROR,
        "llm_error empty_final_response at round 1",
    )


def test_successful_same_tool_with_other_arguments_does_not_hide_failure(skill_env):
    """Only an exact operation retry may clear an earlier Tool failure."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
    fake_registry.execute.side_effect = [
        json.dumps({
            "status": "security_policy_blocked",
            "error": "command rejected",
        }),
        json.dumps({"status": "succeeded", "success": True}),
    ]
    first_call = [{
        "id": "call_blocked",
        "type": "function",
        "function": {
            "name": "bash",
            "arguments": '{"command":"generate --output outputs/a.png"}',
        },
    }]
    second_call = [{
        "id": "call_ok",
        "type": "function",
        "function": {
            "name": "bash",
            "arguments": '{"command":"generate --output outputs/b.png"}',
        },
    }]
    llm_responses = iter([
        _llm_response(tool_calls=first_call, finish="tool_calls"),
        _llm_response(tool_calls=second_call, finish="tool_calls"),
        _llm_response(content="第二个文件已生成"),
    ])
    hook_calls = []

    def record_hook(name, payload, **_kwargs):
        hook_calls.append((name, payload))
        return payload

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", side_effect=llm_responses), \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "hook_trigger", side_effect=record_hook), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1",
         }):
        mock_cfg.skill_max_iterations.return_value = 3
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None
        mock_cfg.skill_llm_recovery_attempts.return_value = 0

        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    result = json.loads(dict(fake_bus.put_calls)["skill_result:skill-runner"])
    assert result["status"] == "tool_error"
    assert "unresolved failure" in result["error"]
    end_payload = next(
        payload for name, payload in hook_calls
        if name == skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE
    )
    assert end_payload["status"] == "tool_error"
    assert end_payload["error"] == result["error"]


def test_forced_termination_summary_ignores_internal_user_messages():
    """Forced-stop summaries contain evidence, not recovery/reminder prompts."""
    from agentflow_runtime import skill_executor

    messages = [
        {"role": "user", "content": skill_executor.SKILL_RECOVERY_PROMPT},
        {
            "role": "assistant",
            "content": "",
            "tool_calls": [{
                "id": "call_weather",
                "type": "function",
                "function": {"name": "weather", "arguments": "{}"},
            }],
        },
        {
            "role": "tool",
            "tool_call_id": "call_weather",
            "content": '{"temperature": 25}',
        },
    ]

    summary = skill_executor._summarize_tool_history(messages)

    assert skill_executor.SKILL_RECOVERY_PROMPT not in summary
    assert "【工具 call_weather 的输出】" in summary
    assert '{"temperature": 25}' in summary
    assert summary != '{"temperature": 25}'


def test_in_place_hook_artifact_is_serialized(skill_env):
    """In-place ctx.artifacts.append() must reach the persisted Skill result."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []

    def append_artifact(name, payload, **kwargs):
        if name == skill_executor.hook_mounts.SKILL_EXECUTOR_BEFORE_SKILL_COMPLETE:
            kwargs["ctx"].artifacts.append("outputs/hook-result.png")
        return payload

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm",
                      return_value=_llm_response(content="完成")), \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "hook_trigger", side_effect=append_artifact), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1",
         }):
        mock_cfg.skill_max_iterations.return_value = 1
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None
        mock_cfg.skill_llm_recovery_attempts.return_value = 0

        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    result = json.loads(dict(fake_bus.put_calls)["skill_result:skill-runner"])
    assert json.loads(result["artifacts_json"]) == ["outputs/hook-result.png"]



def test_recovery_exhaustion_closes_skill_span_with_error(skill_env):
    """A final-round parse failure must stop instead of requesting a phantom retry."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
    hook_calls = []

    def record_hook(name, payload, **_kwargs):
        hook_calls.append((name, payload))
        return payload

    truncated_call = [{
        "id": "call_broken",
        "type": "function",
        "function": {
            "name": "write_file",
            "arguments": '{"file_path":"out.txt","content":',
        },
    }]

    skill_span = MagicMock()
    skill_span_cm = MagicMock()
    skill_span_cm.__enter__.return_value = skill_span
    llm_span = MagicMock()
    llm_span_cm = MagicMock()
    llm_span_cm.__enter__.return_value = llm_span

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", return_value=_llm_response(
             tool_calls=truncated_call, finish="length",
         )), \
         patch.object(skill_executor, "emit_span",
                      side_effect=[skill_span_cm, llm_span_cm]), \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "hook_trigger", side_effect=record_hook), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1",
         }):
        mock_cfg.skill_max_iterations.return_value = 1
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None
        mock_cfg.skill_llm_recovery_attempts.return_value = 2

        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    llm_span_cm.__exit__.assert_called_once_with(None, None, None)
    skill_span_cm.__exit__.assert_called_once_with(None, None, None)
    status, error_message = skill_span.set_status.call_args.args
    assert status == skill_executor.SpanStatus.ERROR
    assert "tool_call_error at round 1" in error_message

    result = json.loads(dict(fake_bus.put_calls)["skill_result:skill-runner"])
    assert result["status"] == "tool_call_error"
    recovery_calls = [
        payload for name, payload in hook_calls
        if name == skill_executor.hook_events.SKILL_EXECUTOR_RECOVERY_DECISION
    ]
    assert recovery_calls == [{
        "skill_name": "file-writer",
        "round": 1,
        "next_round": None,
        "action": "stop",
        "reason": "completion_truncated",
        "finish_reason": "length",
        "recovery_attempt": 0,
        "max_recovery_attempts": 2,
        "previous_completion_budget": 4000,
        "completion_budget": 4000,
        "stop_reason": "max_rounds_reached",
    }]
    after_calls = [
        payload for name, payload in hook_calls
        if name == skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE
    ]
    assert after_calls == [{
        "skill_name": "file-writer",
        "round": 1,
        "status": "tool_call_error",
        "skill_result": "Tool call recovery exhausted: completion_truncated",
        "error": "completion_truncated",
        "artifacts_count": 0,
    }]


def test_unhandled_exception_closes_outer_skill_span(skill_env):
    """An unexpected post-start exception must close the outer skill span."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
    hook_calls = []

    def record_hook(name, payload, **_kwargs):
        hook_calls.append((name, payload))
        return payload
    skill_span = MagicMock()
    skill_span_cm = MagicMock()
    skill_span_cm.__enter__.return_value = skill_span

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "emit_span", return_value=skill_span_cm), \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "hook_trigger", side_effect=record_hook), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "extract_artifact_paths",
                      side_effect=RuntimeError("artifact failure")), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1",
         }):
        mock_cfg.skill_max_iterations.return_value = 5

        with pytest.raises(RuntimeError, match="artifact failure"):
            skill_executor.execute()

    skill_span_cm.__exit__.assert_called_once()
    exc_type, exc_value, traceback = skill_span_cm.__exit__.call_args.args
    assert exc_type is RuntimeError
    assert str(exc_value) == "artifact failure"
    assert traceback is not None
    after_calls = [
        payload for name, payload in hook_calls
        if name == skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE
    ]
    assert after_calls == [{
        "skill_name": "file-writer",
        "round": 0,
        "status": "unhandled_exception",
        "skill_result": "",
        "error": "RuntimeError: artifact failure",
        "artifacts_count": 0,
    }]
    assert "skill_result:skill-runner" not in dict(fake_bus.put_calls)


def test_unhandled_exception_closes_llm_round_span(skill_env):
    """An unexpected exception inside one LLM round must close both spans."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
    skill_span = MagicMock()
    skill_span_cm = MagicMock()
    skill_span_cm.__enter__.return_value = skill_span
    llm_span = MagicMock()
    llm_span_cm = MagicMock()
    llm_span_cm.__enter__.return_value = llm_span

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "emit_span",
                      side_effect=[skill_span_cm, llm_span_cm]), \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "build_round_reminder",
                      side_effect=RuntimeError("reminder failure")), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1",
         }):
        mock_cfg.skill_max_iterations.return_value = 5
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_recovery_attempts.return_value = 2

        with pytest.raises(RuntimeError, match="reminder failure"):
            skill_executor.execute()

    llm_span_cm.__exit__.assert_called_once()
    llm_exc_type, llm_exc_value, llm_traceback = llm_span_cm.__exit__.call_args.args
    assert llm_exc_type is RuntimeError
    assert str(llm_exc_value) == "reminder failure"
    assert llm_traceback is not None

    skill_span_cm.__exit__.assert_called_once()
    skill_exc_type, skill_exc_value, skill_traceback = skill_span_cm.__exit__.call_args.args
    assert skill_exc_type is RuntimeError
    assert str(skill_exc_value) == "reminder failure"
    assert skill_traceback is not None


def test_max_rounds_emits_after_skill_complete_hook(skill_env):
    """Loop exhaustion must emit the Hook consumed as sessionlog skill.end."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
    fake_registry.execute.return_value = "ok"
    hook_calls = []
    tool_calls = [{
        "id": "call_continue",
        "type": "function",
        "function": {"name": "bash", "arguments": '{"command":"echo ok"}'},
    }]

    def record_hook(name, payload, **_kwargs):
        hook_calls.append((name, payload))
        return payload

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", return_value=_llm_response(
             tool_calls=tool_calls, finish="tool_calls",
         )), \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "hook_trigger", side_effect=record_hook), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1",
         }):
        mock_cfg.skill_max_iterations.return_value = 1
        mock_cfg.node_execution_timeout.return_value = 3600
        mock_cfg.skill_max_completion_tokens.return_value = 4000
        mock_cfg.skill_llm_temperature.return_value = 0.7
        mock_cfg.skill_llm_top_p.return_value = 1.0
        mock_cfg.skill_llm_frequency_penalty.return_value = 0.0
        mock_cfg.skill_llm_presence_penalty.return_value = 0.0
        mock_cfg.skill_llm_top_k.return_value = 0
        mock_cfg.skill_llm_stop.return_value = None
        mock_cfg.skill_llm_recovery_attempts.return_value = 2

        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    after_calls = [
        payload for name, payload in hook_calls
        if name == skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE
    ]
    assert after_calls == [{
        "skill_name": "file-writer",
        "round": 1,
        "status": "max_rounds_reached",
        "skill_result": "【工具 call_continue 的输出】\nok",
        "error": "",
        "artifacts_count": 0,
    }]
    result = json.loads(dict(fake_bus.put_calls)["skill_result:skill-runner"])
    assert result["status"] == "max_rounds_reached"
    assert "【工具 call_continue 的输出】\nok" in result["skill_result"]
    assert skill_executor.SKILL_RECOVERY_PROMPT not in result["skill_result"]
