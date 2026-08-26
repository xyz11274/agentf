"""
P1 基线测试：锁住 skill_executor 多 tool_calls 的**串行执行**行为。

背景（2026-08-07 执行效率分析）：
- skill_executor 主循环 `for tc in tool_calls:`（skill_executor.py:1341）逐个同步执行工具，
  不支持并行。日志观察：LLM 常一次返回 2-10 个独立 tool_calls（read_file×10、bash×4 等），
  串行执行浪费了可并行的机会（慢 I/O 阻塞快 I/O）。
- 本测试用 fake 工具注册表记录每个工具的执行时间戳，断言：
  ① 多个 tool_calls 被全部执行（tool_call_id 顺序正确回灌）
  ② 执行是**串行**的（第二个工具开始时间 >= 第一个工具结束时间）——锁住当前行为。
- P1 实施并行化后，断言 ② 应改为"时间戳重叠"（并行），此测试即为 Red-Green 的基线。

不依赖 daemon / 真实 LLM / 沙箱：mock call_llm + fake 工具注册表。
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PKG_SRC = PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
if str(PKG_SRC) not in sys.path:
    sys.path.insert(0, str(PKG_SRC))


@pytest.fixture
def skill_env(monkeypatch):
    """环境变量基线：让 skill_executor 进入 LLM while-loop（不触发 interrupt）。"""
    monkeypatch.setenv("AF_NODE_ID", "skill_executor")
    monkeypatch.setenv("AF_DIALOGUE_ID", "d_test_tool_parallel")
    monkeypatch.setenv("AF_SESSION_ID", "ses_test")
    monkeypatch.setenv("AF_INPUT", "请帮我查询")
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:1")
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", "")  # 禁用真实 UDS
    monkeypatch.setenv("AGENT_PACKAGE_DIR",
                       str(PROJECT_ROOT / "examples" / "agent" / "outfit-assistant"))
    # 禁用真实 LLM 配置读取（resolve_llm_config 读 .env/agent.md，测试环境不可控）
    monkeypatch.setenv("AF_LLM_API_KEY", "test-key")
    monkeypatch.setenv("AF_LLM_BASE_URL", "http://127.0.0.1:1/v1")
    return monkeypatch


class _FakeDataBus:
    """模拟 DataBusClient：connected + 可读 dispatch manifest + 可写结果。"""

    def __init__(self):
        self.connected = True
        self.put_calls: list[tuple[str, str]] = []
        self.push_calls: list[tuple] = []
        self._dispatch = json.dumps({
            "skill_name": "outfit-stylist",
            "prompt": "请帮我查询天气并给出建议",
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


class _FakeToolRegistry:
    """记录工具执行时间戳的 fake 工具注册表。

    execute() 每次 sleep 0.2s 模拟工具耗时，并记录 (name, start_monotonic, end_monotonic)。
    测试据此断言串行（时间戳不重叠）vs 并行（时间戳重叠）。
    """

    TOOL_SCHEMA = [
        {"type": "function", "function": {"name": "bash", "description": "exec", "parameters": {"type": "object", "properties": {"command": {"type": "string"}}}}},
    ]

    def __init__(self, skill_name: str = ""):
        self.calls: list[tuple[str, float, float]] = []
        self._tools = list(self.TOOL_SCHEMA)

    def get_tools(self):
        return self._tools

    def execute(self, name: str, args) -> str:
        start = time.monotonic()
        time.sleep(0.2)  # 模拟工具耗时（如沙箱网络 I/O）
        end = time.monotonic()
        self.calls.append((name, start, end))
        return json.dumps({"stdout": f"ok:{name}", "exit_code": 0, "stderr": "", "tool": name})


def _make_llm_response(content: str = "", tool_calls=None, finish: str = "stop") -> dict:
    msg = {"role": "assistant", "content": content}
    if tool_calls:
        msg["tool_calls"] = tool_calls
    return {
        "choices": [{"index": 0, "message": msg, "finish_reason": finish}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
    }


def test_multiple_tool_calls_execute_in_parallel(skill_env, tmp_path):
    """P1 落地：一次返回 2 个只读 tool_calls 时，混合批次并行执行（时间戳重叠）。

    batch 策略（tool_policies.execute_in_batches）将只读工具（echo 命令）归入
    同一并行批次 → 时间戳重叠。写工具仍串行（见 test_tool_policies.py）。
    """
    from agentflow_runtime import skill_executor
    from agentflow_runtime.trace.span_context import SpanContext
    from agentflow_runtime.trace.span_writer import SpanWriter

    skill_env.setenv("AF_SKIP_DOTENV", "1")
    skill_env.setenv("AF_TRACE_OUTPUT_DIR", str(tmp_path))
    SpanContext.current().clear()
    skill_env.setattr(SpanWriter, "_instance", None)

    fake_bus = _FakeDataBus()
    fake_reg = _FakeToolRegistry()

    # call_llm 序列：第 1 轮返回 2 个 tool_calls → 第 2 轮返回最终答案
    tool_calls = [
        {"id": "call_1", "type": "function", "function": {"name": "bash", "arguments": json.dumps({"command": "echo a"})}},
        {"id": "call_2", "type": "function", "function": {"name": "bash", "arguments": json.dumps({"command": "echo b"})}},
    ]
    llm_side_effect = [
        _make_llm_response(tool_calls=tool_calls, finish="tool_calls"),
        _make_llm_response(content="完成"),
    ]

    def fake_hook_trigger(event_name, payload, **kwargs):
        if event_name == skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_TOOL_CALL:
            return {**payload, "results": []}
        return payload

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", side_effect=llm_side_effect) as mock_llm, \
         patch.object(skill_executor, "emit_event"), \
         patch.object(skill_executor, "hook_trigger", side_effect=fake_hook_trigger), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_reg), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config",
                      return_value={"model": "test", "api_key": "k", "base_url": "http://127.0.0.1:1/v1"}):

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

        import io
        buf = io.StringIO()
        with patch("sys.stdout", buf):
            skill_executor.execute()

    # ① 两个 tool_calls 都被执行
    assert len(fake_reg.calls) == 2, f"应执行 2 个工具，实际 {len(fake_reg.calls)}"
    names = [c[0] for c in fake_reg.calls]
    assert names == ["bash", "bash"]

    # ② 并行：两个只读工具（echo）同一并行批次 → 时间戳重叠（start2 < end1）
    _, start1, end1 = fake_reg.calls[0]
    _, start2, end2 = fake_reg.calls[1]
    assert start2 < end1, (
        f"预期并行执行（start2 < end1），实际 start2={start2:.3f} end1={end1:.3f}"
    )

    # ③ LLM 被调用 2 轮（工具轮 + 最终答案轮）
    assert mock_llm.call_count == 2, f"call_llm 应 2 轮，实际 {mock_llm.call_count}"
    second_round_messages = mock_llm.call_args_list[1].args[0]
    tool_messages = [m for m in second_round_messages if m.get("role") == "tool"]
    assert [m["tool_call_id"] for m in tool_messages] == ["call_1", "call_2"]

    # ④ 结果写入 DataBus（status=completed）
    put_calls = dict(fake_bus.put_calls)
    result = json.loads(put_calls["skill_result:skill-runner"])
    assert result["status"] == "completed"

    # ⑤ 主线程记录一个 tool_batch span，父节点是 skill span。
    span_files = list(tmp_path.rglob("python_spans_*.ndjson"))
    spans = [json.loads(line) for path in span_files for line in path.read_text().splitlines()]
    skill_spans = [s for s in spans if s["name"] == skill_executor.hook_events.SPAN_SKILL_EXECUTOR_EXECUTE]
    tool_spans = [s for s in spans if s["name"] == skill_executor.hook_events.SPAN_SKILL_EXECUTOR_TOOL_BATCH]
    assert len(skill_spans) == 1
    assert len(tool_spans) == 1
    assert tool_spans[0]["kind"] == "TOOL"
    assert tool_spans[0]["parent_span_id"] == skill_spans[0]["span_id"]
    assert tool_spans[0]["status"] == "OK"
    assert tool_spans[0]["tags"]["tool_count"] == "2"
    assert SpanContext.current().current_span_id() == ""
