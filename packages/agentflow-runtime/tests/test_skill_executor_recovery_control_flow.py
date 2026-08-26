"""Regression tests for skill_executor semantic tool-call recovery."""

from __future__ import annotations

import io
import json
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
    truncated_call = [{
        "id": "call_broken",
        "type": "function",
        "function": {
            "name": "write_file",
            "arguments": '{"file_path":"out.txt","content":',
        },
    }]
    llm_side_effect = [
        _llm_response(tool_calls=truncated_call, finish="length"),
        _llm_response(content="恢复后完成"),
    ]

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", side_effect=llm_side_effect) as mock_llm, \
         patch.object(skill_executor, "emit_event"), \
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


def test_recovery_exhaustion_closes_skill_span_with_error(skill_env):
    """Exhausted semantic recovery must finalize the outer skill span."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
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
        mock_cfg.skill_llm_recovery_attempts.return_value = 0

        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    llm_span_cm.__exit__.assert_called_once_with(None, None, None)
    skill_span_cm.__exit__.assert_called_once_with(None, None, None)
    status, error_message = skill_span.set_status.call_args.args
    assert status == skill_executor.SpanStatus.ERROR
    assert "tool_call_error at round 1" in error_message

    result = json.loads(dict(fake_bus.put_calls)["skill_result:skill-runner"])
    assert result["status"] == "tool_call_error"


def test_unhandled_exception_closes_outer_skill_span(skill_env):
    """An unexpected post-start exception must close the outer skill span."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_registry = MagicMock()
    fake_registry.get_tools.return_value = []
    skill_span = MagicMock()
    skill_span_cm = MagicMock()
    skill_span_cm.__enter__.return_value = skill_span

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "emit_span", return_value=skill_span_cm), \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_registry), \
         patch.object(skill_executor, "load_skills_from_list", return_value={}), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
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
