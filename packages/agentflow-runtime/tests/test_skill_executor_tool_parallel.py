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
            "batch_id": "batch:d_test_tool_parallel:r1",
            "request_index": 2,
            "force": True,
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
        if args.get("command") == "echo b":
            return json.dumps({"error": "simulated failure", "tool": name})
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
    llm_side_effect[0]["_agentflow_call_id"] = "llm-call-1"
    llm_side_effect[1]["_agentflow_call_id"] = "llm-call-2"

    llm_result_payloads = []
    effective_responses = []
    fake_recorder = MagicMock()

    def record_effective_response(call_id, response):
        snapshot = json.loads(json.dumps(response, ensure_ascii=False))
        snapshot.pop("_agentflow_call_id", None)
        effective_responses.append((call_id, snapshot))
        return snapshot

    fake_recorder.record_effective_response.side_effect = record_effective_response

    tool_result_payloads = []
    skill_start_payloads = []

    def fake_hook_trigger(event_name, payload, **kwargs):
        if event_name == skill_executor.hook_events.SKILL_EXECUTOR_TRIGGER:
            skill_start_payloads.append(payload)
        if event_name == skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_LLM_CALL:
            if payload["call_id"] == "llm-call-2":
                payload["raw_choices"]["message"]["content"] = "Hook 后完成"
        if event_name == skill_executor.hook_events.SKILL_EXECUTOR_LLM_RESULT:
            llm_result_payloads.append(payload)
        if event_name == skill_executor.hook_mounts.SKILL_EXECUTOR_AFTER_TOOL_CALL:
            return {**payload, "results": []}
        if event_name == skill_executor.hook_events.SKILL_EXECUTOR_TOOL_RESULT:
            tool_result_payloads.append(payload)
        return payload

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", side_effect=llm_side_effect) as mock_llm, \
         patch.object(skill_executor, "get_recorder", return_value=fake_recorder), \
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

    # Skill invocation preserves the dispatch identity and complete arguments.
    assert len(skill_start_payloads) == 1
    skill_start_payload = skill_start_payloads[0]
    assert set(skill_start_payload["skill_doc_digests"]) == {"outfit-stylist"}
    assert all(
        len(digest) == 64
        for digest in skill_start_payload["skill_doc_digests"].values()
    )
    assert len(skill_start_payload["system_prompt_sha256"]) == 64
    assert len(skill_start_payload["task_instruction_sha256"]) == 64
    assert skill_start_payload["tool_definition_digests"][0]["name"] == "bash"
    assert len(skill_start_payload["tool_definition_digests"][0]["sha256"]) == 64
    for key in (
        "skill_doc_digests", "system_prompt_sha256",
        "task_instruction_sha256", "tool_definition_digests",
    ):
        skill_start_payload.pop(key)
    assert skill_start_payload == {
        "session_id": "ses_test",
        "dialogue_id": "d_test_tool_parallel",
        "skill_name": "outfit-stylist",
        "skill_prompt": "请帮我查询天气并给出建议",
        "skill_prompt_preview": "请帮我查询天气并给出建议",
        "load_skills": [],
        "effective_load_skills": ["outfit-stylist"],
        "force": True, "request_index": 2, "exec_skill_key": "dispatch:1",
        "batch_id": "batch:d_test_tool_parallel:r1",
        "model": "test", "max_rounds": 5, "tools_count": 1,
        "tool_names": ["bash"],
    }

    # ④ 最终 Tool Result 观测保留原始顺序、调用身份、实参和完整返回值。
    assert len(tool_result_payloads) == 1
    assert tool_result_payloads[0]["after_hook_rewrite_status"] == "ignored_invalid"
    assert "count mismatch" in tool_result_payloads[0]["after_hook_rewrite_error"]
    result_records = tool_result_payloads[0]["tool_results"]
    assert [r["tool_call_id"] for r in result_records] == ["call_1", "call_2"]
    assert [r["ordinal"] for r in result_records] == [1, 2]
    assert [r["arguments"]["command"] for r in result_records] == ["echo a", "echo b"]
    assert [r["status"] for r in result_records] == ["completed", "error"]
    assert json.loads(result_records[0]["result"])["stdout"] == "ok:bash"
    assert json.loads(result_records[1]["result"])["error"] == "simulated failure"

    # ⑤ 结果写入 DataBus（status=completed）
    put_calls = dict(fake_bus.put_calls)
    result = json.loads(put_calls["skill_result:skill-runner"])
    assert result["status"] == "completed"

    # ⑥ 主线程记录一个 tool_batch span，父节点是 skill span。
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

    # Tool Call/Result 事件随 Skill span 写入 NDJSON，结果可直接按 call_id 回放。
    skill_events = {event["name"]: event for event in skill_spans[0]["events"]}
    # after_llm Hook 的实际执行值必须同时进入 Recorder 和最终观察事件。
    assert result["skill_result"] == "Hook 后完成"
    assert [call_id for call_id, _ in effective_responses] == ["llm-call-1", "llm-call-2"]
    assert effective_responses[-1][1]["choices"][0]["message"]["content"] == "Hook 后完成"
    assert [payload["call_id"] for payload in llm_result_payloads] == [
        "llm-call-1", "llm-call-2",
    ]
    assert llm_result_payloads[-1]["response"] == effective_responses[-1][1]
    call_records = json.loads(
        skill_events[skill_executor.hook_events.SKILL_EXECUTOR_TOOL_CALL]
        ["attributes"]["tool_calls_json"]
    )
    persisted_results = json.loads(
        skill_events[skill_executor.hook_events.SKILL_EXECUTOR_TOOL_RESULT]
        ["attributes"]["tool_results_json"]
    )
    assert [r["tool_call_id"] for r in call_records] == ["call_1", "call_2"]
    assert persisted_results == result_records
    assert SpanContext.current().current_span_id() == ""

def _rewrite_fixture():
    tool_calls = [
        {
            "id": "call_1",
            "function": {
                "name": "read_file",
                "arguments": json.dumps({"path": "a.txt"}),
            },
        },
        {
            "id": "call_2",
            "function": {
                "name": "bash",
                "arguments": json.dumps({"command": "echo ok"}),
            },
        },
    ]
    original = [
        ("read_file", {"path": "a.txt"}, "old-a"),
        ("bash", {"command": "echo ok"}, "old-b"),
    ]
    return tool_calls, original


def test_after_tool_rewrite_is_reordered_by_tool_call_id():
    """Structured records may be returned in any order but bind by stable call ID."""
    from agentflow_runtime import skill_executor

    tool_calls, original = _rewrite_fixture()
    rewritten = [
        {
            "tool_call_id": "call_2",
            "tool_name": "bash",
            "arguments": {"command": "echo ok"},
            "result": "new-b",
        },
        {
            "tool_call_id": "call_1",
            "tool_name": "read_file",
            "arguments": {"path": "a.txt"},
            "result": "new-a",
        },
    ]

    normalized, error = skill_executor._normalize_after_tool_rewrite(
        tool_calls, original, rewritten
    )

    assert error == ""
    assert normalized == [
        ("read_file", {"path": "a.txt"}, "new-a"),
        ("bash", {"command": "echo ok"}, "new-b"),
    ]


@pytest.mark.parametrize(
    ("rewritten", "error_fragment"),
    [
        ([], "count mismatch"),
        ([
            {
                "tool_call_id": "unknown",
                "tool_name": "read_file",
                "arguments": {"path": "a.txt"},
                "result": "new-a",
            },
            {
                "tool_call_id": "call_2",
                "tool_name": "bash",
                "arguments": {"command": "echo ok"},
                "result": "new-b",
            },
        ], "unknown tool_call_id"),
        ([
            {
                "tool_call_id": "call_1",
                "tool_name": "read_file",
                "arguments": {"path": "other.txt"},
                "result": "new-a",
            },
            {
                "tool_call_id": "call_2",
                "tool_name": "bash",
                "arguments": {"command": "echo ok"},
                "result": "new-b",
            },
        ], "arguments mismatch"),
        ([
            {
                "tool_call_id": "call_1",
                "tool_name": "read_file",
                "arguments": {"path": "a.txt"},
                "result": {"not": "a string"},
            },
            {
                "tool_call_id": "call_2",
                "tool_name": "bash",
                "arguments": {"command": "echo ok"},
                "result": "new-b",
            },
        ], "result must be a string"),
        ([
            ("bash", {"command": "echo ok"}, "new-b"),
            ("read_file", {"path": "a.txt"}, "new-a"),
        ], "legacy tool_name mismatch"),
    ],
)
def test_after_tool_invalid_rewrite_is_rejected(rewritten, error_fragment):
    from agentflow_runtime import skill_executor

    tool_calls, original = _rewrite_fixture()
    normalized, error = skill_executor._normalize_after_tool_rewrite(
        tool_calls, original, rewritten
    )

    assert normalized is None
    assert error_fragment in error


def test_mixed_skip_messages_keep_original_tool_call_order(skill_env):
    """Skipped and executed results must follow the assistant tool_calls order."""
    from agentflow_runtime import skill_executor

    fake_bus = _FakeDataBus()
    fake_reg = _FakeToolRegistry()
    tool_calls = [
        {
            "id": "call_1",
            "type": "function",
            "function": {
                "name": "read_file",
                "arguments": json.dumps({"path": "a.txt"}),
            },
        },
        {
            "id": "call_2",
            "type": "function",
            "function": {
                "name": "bash",
                "arguments": json.dumps({"command": "echo skip"}),
            },
        },
        {
            "id": "call_3",
            "type": "function",
            "function": {
                "name": "write_file",
                "arguments": json.dumps({"file_path": "b.txt", "content": "b"}),
            },
        },
    ]
    llm_side_effect = [
        _make_llm_response(tool_calls=tool_calls, finish="tool_calls"),
        _make_llm_response(content="完成"),
    ]
    result_payloads = []
    fake_recorder = MagicMock()
    fake_recorder.record_effective_response.side_effect = (
        lambda _call_id, response: response
    )

    def fake_hook_trigger(event_name, payload, **_kwargs):
        if event_name == skill_executor.hook_mounts.SKILL_EXECUTOR_BEFORE_TOOL_CALL:
            return {**payload, "skip_tool_names": ["bash"]}
        if event_name == skill_executor.hook_events.SKILL_EXECUTOR_TOOL_RESULT:
            result_payloads.append(payload)
        return payload

    with patch.object(skill_executor, "DataBusClient", return_value=fake_bus), \
         patch.object(skill_executor, "call_llm", side_effect=llm_side_effect) as mock_llm, \
         patch.object(skill_executor, "get_recorder", return_value=fake_recorder), \
         patch.object(skill_executor, "hook_trigger", side_effect=fake_hook_trigger), \
         patch.object(skill_executor, "AgentConfig") as mock_cfg, \
         patch.object(skill_executor, "_LocalToolRegistry", return_value=fake_reg), \
         patch.object(skill_executor, "_ensure_artifacts_in_package",
                      side_effect=lambda artifacts: artifacts), \
         patch.object(skill_executor, "resolve_llm_config", return_value={
             "model": "test",
             "api_key": "k",
             "base_url": "http://127.0.0.1:1/v1",
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

        import io
        with patch("sys.stdout", io.StringIO()):
            skill_executor.execute()

    second_round_messages = mock_llm.call_args_list[1].args[0]
    tool_messages = [
        message for message in second_round_messages
        if message.get("role") == "tool"
    ]
    assert [message["tool_call_id"] for message in tool_messages] == [
        "call_1", "call_2", "call_3",
    ]
    assert json.loads(tool_messages[1]["content"]) == {
        "skipped_by_hook": True,
        "tool": "bash",
    }
    assert [call[0] for call in fake_reg.calls] == ["read_file", "write_file"]
    assert len(result_payloads) == 1
    assert [record["tool_call_id"] for record in result_payloads[0]["tool_results"]] == [
        "call_1", "call_2", "call_3",
    ]
    assert [record["status"] for record in result_payloads[0]["tool_results"]] == [
        "completed", "skipped", "completed",
    ]
