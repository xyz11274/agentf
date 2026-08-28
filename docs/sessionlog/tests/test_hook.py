"""hook 单元测试 — 事件采集与落盘（mock 引擎环境）。

依赖处理：
- AF_SESSIONLOG_ENABLED 是模块级（加载时读）→ 用 reload + monkeypatch 控制
- _get_databus() 连接 UDS → mock 掉，返回假 DataBus（记录 llm/traces 读取）
- _current_span_id() 依赖 SpanContext → mock 掉
- _output_dir() 依赖 AF_TRACE_OUTPUT_DIR → 指向 tmp_path
"""

from __future__ import annotations

import importlib
import json
import os
import sys

import pytest

from conftest import HOOK_PATH


def _load_hook(monkeypatch, tmp_path, enabled="1"):
    """加载 hook 模块（每次 reload，读最新环境变量）。"""
    monkeypatch.setenv("AF_SESSIONLOG_ENABLED", enabled)
    monkeypatch.setenv("AF_TRACE_OUTPUT_DIR", str(tmp_path))
    monkeypatch.setenv("AF_DIALOGUE_ID", "d1")
    monkeypatch.setenv("AF_SESSION_ID", "s0")
    monkeypatch.setenv("AF_NODE_ID", "coordinator_decide")
    monkeypatch.setenv("AF_SELF_TASK_ID", "task-0")
    spec = importlib.util.spec_from_file_location("sessionlog_hook_ut", HOOK_PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["sessionlog_hook_ut"] = mod
    spec.loader.exec_module(mod)
    yield mod
    sys.modules.pop("sessionlog_hook_ut", None)


def _make_event(name, payload=None):
    """构造 HookEvent 兼容对象（engine.py HookEvent 的简化）。"""
    payload = payload or {}

    class _Event:
        def __init__(self):
            self.name = name
            self.payload = payload
    return _Event()


def _make_ctx(messages=None):
    """构造 AgentContext 兼容对象（context.py 的简化）。"""
    class _Ctx:
        def __init__(self):
            self.messages = messages or []
            self.artifacts = []
            self.blocked = False
    return _Ctx()


def _read_output(tmp_path):
    """读 tmp_path 下的 sessionlog 文件（d1.jsonl），返回事件列表。"""
    p = tmp_path / "sessionlogs" / "d1.jsonl"
    if not p.exists():
        return []
    return [json.loads(l) for l in p.read_text(encoding="utf-8").splitlines() if l.strip()]


# ═══════════════════════════════════════════════════════════
# 开关
# ═══════════════════════════════════════════════════════════

def test_disabled_no_write(monkeypatch, tmp_path):
    """AF_SESSIONLOG_ENABLED=0 → 不落盘。"""
    mod = next(_load_hook(monkeypatch, tmp_path, enabled="0"))
    mod.handle(_make_event("agent.dialogue.start", {"trigger_type": "first"}), _make_ctx())
    assert _read_output(tmp_path) == [], "禁用时不应有任何输出"


# ═══════════════════════════════════════════════════════════
# session.start
# ═══════════════════════════════════════════════════════════

def test_session_start_writes_event(monkeypatch, tmp_path):
    """AGENT_DIALOGUE_START → 整个 dialogue 唯一的 session.start。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod.handle(_make_event("agent.dialogue.start", {
        "input": "用户输入", "trigger_type": "first",
        "coordinator_version": "v1", "code_revision": "abc123",
        "agent_definition_sha256": "a" * 64,
        "model": "m", "max_iterations": 10,
        "max_completion_tokens": 4000,
    }), _make_ctx())
    evs = _read_output(tmp_path)
    assert len(evs) == 2  # header + session.start
    assert evs[0]["type"] == "session.header"
    assert evs[1]["type"] == "session.start"
    assert evs[1]["data"]["trigger_type"] == "first"
    assert evs[1]["data"]["input"] == "用户输入"
    assert evs[1]["data"]["coordinator_version"] == "v1"
    assert evs[1]["data"]["code_revision"] == "abc123"
    assert evs[1]["data"]["agent_definition_sha256"] == "a" * 64
    assert evs[1]["data"]["max_iterations"] == 10
    assert evs[1]["data"]["max_completion_tokens"] == 4000
    assert "completed_skill" not in evs[1]["data"]
    assert evs[1]["trace_id"]  # trace_id 必须有


def test_skill_completion_writes_activation_not_session_start(monkeypatch, tmp_path):
    """Skill 回流是 Coordinator 激活，不是新 dialogue。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod.handle(_make_event("coordinator.trigger", {
        "trigger_type": "skill_completion",
        "model": "m",
        "completed_skill": "skill-batch",
        "completed_batch_key": "completion_batch:batch:d1:r1",
    }), _make_ctx())

    evs = _read_output(tmp_path)
    assert [event["type"] for event in evs] == [
        "session.header", "coordinator.activation",
    ]
    activation = evs[1]["data"]
    assert activation["completed_skill"] == "skill-batch"
    assert activation["completed_batch_key"] == "completion_batch:batch:d1:r1"
    assert activation["batch_id"] == "batch:d1:r1"


# ═══════════════════════════════════════════════════════════
# llm.request / llm.response
# ═══════════════════════════════════════════════════════════

def test_llm_request_ctx_messages(monkeypatch, tmp_path):
    """BEFORE_LLM_CALL → llm.request 带 ctx.messages 全量（实时）。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._current_span_id = lambda: "sp_llm_1"
    msgs = [{"role": "system", "content": "sys"}, {"role": "user", "content": "hi"}]
    mod.handle(_make_event("coordinator_v1.before_llm_call", {
        "round": 1,
        "model": "m",
        "activation_index": 1,
        "decision_index": 2,
        "skill_completion_index": 0,
        "internal_tool_correction_index": 1,
    }),
               _make_ctx(messages=msgs))
    evs = _read_output(tmp_path)
    llm_req = [e for e in evs if e["type"] == "llm.request"]
    assert len(llm_req) == 1
    assert llm_req[0]["data"]["messages"] == msgs  # 全量
    assert llm_req[0]["data"]["msg_count"] == 2
    assert llm_req[0]["data"]["request_stage"] == "hook_observation"
    assert llm_req[0]["data"]["skill_name"] == "coordinator"
    assert llm_req[0]["data"]["task_type"] == "coordinator"
    assert llm_req[0]["data"]["activation_index"] == 1
    assert llm_req[0]["data"]["decision_index"] == 2
    assert llm_req[0]["data"]["skill_completion_index"] == 0
    assert llm_req[0]["data"]["internal_tool_correction_index"] == 1
    assert llm_req[0]["span_id"] == "sp_llm_1"


def test_llm_response_reads_databus(monkeypatch, tmp_path):
    """最终 LLM 观察使用 Hook 后响应，并按 call_id 读取 Recorder meta。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._current_span_id = lambda: "sp_llm_1"

    # mock DataBus：dialogue 最新项属于另一个并行调用
    class FakeNS:
        def __init__(self, store):
            self._store = store

        def get(self, key, default=None):
            return self._store.get(key, default)

    class FakeDB:
        def __init__(self):
            self._store = {
                "traces/index/d1": ["c1", "other-call"],
                "traces/c2/request": {
                    "model": "m",
                    "messages": [{"role": "user", "content": "hi"}],
                    "enable_thinking": False,
                    "temperature": 0.7,
                },
                "traces/c2/provider_response": {
                    "choices": [{"message": {"content": "Provider 原始答案"}}],
                },
                "traces/c2/response": {"choices": [{"finish_reason": "tool_calls"}], "usage": {}},
                "traces/c2/meta": {"task_type": "coordinator", "node_id": "coordinator_decide"},
            }

        def llm(self, dialogue_id=None):
            return FakeNS(self._store)

    mod._get_databus = lambda: FakeDB()

    # Hook 输入 → 精确传输请求 → 有效响应，三段血缘必须连续。
    mod.handle(_make_event("coordinator_v1.before_llm_call", {"round": 1, "model": "m"}),
               _make_ctx(messages=[{"role": "user", "content": "hi"}]))
    mod.handle(_make_event("llm.request.prepared", {
        "call_id": "c2",
        "model": "m",
        "request": {
            "model": "m",
            "messages": [{"role": "user", "content": "hi"}],
            "enable_thinking": False,
            "temperature": 0.7,
        },
        "meta": {
            "task_type": "coordinator",
            "round": 1,
            "request_url": "https://llm.invalid/v1/chat/completions",
        },
    }), _make_ctx())
    mod.handle(_make_event("coordinator.llm_result", {
        "round": 1, "call_id": "c2",
        "response": {
            "choices": [{
                "message": {"role": "assistant", "content": "Hook 后最终回答"},
                "finish_reason": "stop",
            }],
        },
    }),
               _make_ctx())

    evs = _read_output(tmp_path)
    transport = [e for e in evs if e["type"] == "llm.transport.request"]
    llm_resp = [e for e in evs if e["type"] == "llm.response"]
    assert len(transport) == 1
    assert transport[0]["data"]["call_id"] == "c2"
    assert transport[0]["data"]["request"]["temperature"] == 0.7
    assert transport[0]["data"]["request_stage"] == "transport_prepared"
    assert transport[0]["data"]["skill_name"] == "coordinator"
    assert transport[0]["data"]["task_type"] == "coordinator"
    assert transport[0]["source_event_seqs"] == [1]
    assert len(llm_resp) == 1
    assert llm_resp[0]["data"]["call_id"] == "c2"
    assert llm_resp[0]["data"]["skill_name"] == "coordinator"
    assert llm_resp[0]["data"]["task_type"] == "coordinator"
    assert llm_resp[0]["data"]["response"]["choices"][0]["finish_reason"] == "stop"
    assert llm_resp[0]["data"]["response"]["choices"][0]["message"]["content"] == "Hook 后最终回答"
    assert llm_resp[0]["data"]["request"]["enable_thinking"] is False
    assert llm_resp[0]["data"]["request"]["temperature"] == 0.7
    assert (
        llm_resp[0]["data"]["provider_response"]["choices"][0]["message"]["content"]
        == "Provider 原始答案"
    )
    assert llm_resp[0]["data"]["meta"]["task_type"] == "coordinator"
    # 响应血缘指向精确传输请求，而非较早的 Hook 输入观察。
    assert llm_resp[0]["source_event_seqs"] == [2]
    assert llm_resp[0]["source_event_ids"] == [
        f'{llm_resp[0]["producer_id"]}:2']


def test_skill_llm_events_promote_tree_identity(monkeypatch, tmp_path):
    """Skill 三段 LLM 事件都直接携带建树身份，不依赖解析 task_id。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:d1:r1:1")
    monkeypatch.setenv("AF_BATCH_ID", "batch:d1:r1")
    mod._current_span_id = lambda: "sp_llm_skill"
    mod._read_llm_call = lambda call_id: {
        "request": {"messages": [{"role": "user", "content": "hi"}]},
        "provider_response": {"choices": []},
        "response": {"choices": []},
        "meta": {
            "task_type": "skill",
            "skill_name": "outfit-stylist",
            "exec_skill_key": "dispatch:d1:r1:1",
            "batch_id": "batch:d1:r1",
        },
    }

    mod.handle(_make_event("skill_executor.before_llm_call", {
        "round": 1, "model": "m", "skill_name": "outfit-stylist",
    }), _make_ctx(messages=[{"role": "user", "content": "hi"}]))
    mod.handle(_make_event("llm.request.prepared", {
        "call_id": "skill-call", "model": "m",
        "request": {"messages": [{"role": "user", "content": "hi"}]},
        "meta": {
            "task_type": "skill", "round": 1,
            "skill_name": "outfit-stylist",
            "exec_skill_key": "dispatch:d1:r1:1",
            "batch_id": "batch:d1:r1",
        },
    }), _make_ctx())
    mod.handle(_make_event("skill_executor.llm_result", {
        "round": 1, "call_id": "skill-call",
        "skill_name": "outfit-stylist", "response": {"choices": []},
    }), _make_ctx())

    events = [
        event for event in _read_output(tmp_path)
        if event["type"] in {"llm.request", "llm.transport.request", "llm.response"}
    ]
    assert [event["data"]["skill_name"] for event in events] == [
        "outfit-stylist", "outfit-stylist", "outfit-stylist",
    ]
    assert [event["data"]["exec_skill_key"] for event in events] == [
        "dispatch:d1:r1:1", "dispatch:d1:r1:1", "dispatch:d1:r1:1",
    ]
    assert [event["data"]["batch_id"] for event in events] == [
        "batch:d1:r1", "batch:d1:r1", "batch:d1:r1",
    ]
    assert events[1]["data"]["call_id"] == events[2]["data"]["call_id"]
    assert events[1]["source_event_seqs"] == [events[0]["seq"]]


# ═══════════════════════════════════════════════════════════
# skill 事件
# ═══════════════════════════════════════════════════════════

def test_skill_end_has_exec_key(monkeypatch, tmp_path):
    """skill.end 带 exec_skill_key（AF_EXEC_SKILL_KEY）。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:d1:r1:1")
    monkeypatch.setenv("AF_BATCH_ID", "batch:d1:r1")
    # 先触发 skill.start（让 _last_skill_start_* 非空）
    mod._current_span_id = lambda: "sp_skill_0"
    mod.handle(_make_event("skill_executor.trigger", {
        "skill_name": "echo", "skill_prompt": "执行 echo",
        "load_skills": ["helper"], "effective_load_skills": ["echo", "helper"],
        "force": True, "request_index": 2,
        "exec_skill_key": "dispatch:d1:r1:1", "batch_id": "batch:d1:r1",
        "model": "m", "max_rounds": 20, "tools_count": 7,
        "tool_names": ["bash"],
        "skill_doc_digests": {"echo": "b" * 64},
        "system_prompt_sha256": "c" * 64,
        "task_instruction_sha256": "d" * 64,
        "tool_definition_digests": [{"name": "bash", "sha256": "e" * 64}],
    }), _make_ctx())
    end_ctx = _make_ctx()
    end_ctx.artifacts = ["outputs/look.png"]
    mod.handle(_make_event("skill_executor.after_skill_complete", {
        "skill_name": "echo", "round": 1, "status": "completed",
        "skill_result": "完整结果", "error": "",
    }), end_ctx)
    evs = _read_output(tmp_path)
    skill_start = [e for e in evs if e["type"] == "skill.start"][0]
    assert skill_start["data"]["skill_prompt"] == "执行 echo"
    assert skill_start["data"]["load_skills"] == ["helper"]
    assert skill_start["data"]["effective_load_skills"] == ["echo", "helper"]
    assert skill_start["data"]["force"] is True
    assert skill_start["data"]["request_index"] == 2
    assert skill_start["data"]["batch_id"] == "batch:d1:r1"
    assert skill_start["data"]["model"] == "m"
    assert skill_start["data"]["tool_names"] == ["bash"]
    assert skill_start["data"]["skill_doc_digests"] == {"echo": "b" * 64}
    assert skill_start["data"]["system_prompt_sha256"] == "c" * 64
    assert skill_start["data"]["task_instruction_sha256"] == "d" * 64
    assert skill_start["data"]["tool_definition_digests"][0]["name"] == "bash"
    skill_end = [e for e in evs if e["type"] == "skill.end"]
    assert len(skill_end) == 1
    assert skill_end[0]["data"]["exec_skill_key"] == "dispatch:d1:r1:1"
    assert skill_end[0]["data"]["batch_id"] == "batch:d1:r1"
    assert skill_end[0]["data"]["artifacts"] == ["outputs/look.png"]
    assert skill_end[0]["data"]["skill_result"] == "完整结果"
    assert skill_end[0]["data"]["error"] == ""
    assert skill_end[0].get("parent_span_id") != skill_end[0].get("span_id")


def test_batch_result_preserves_framework_terminal_results(monkeypatch, tmp_path):
    """batch.result 保存未必进入 Python Skill Executor 的框架终态。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod.handle(_make_event("coordinator.trigger", {
        "trigger_type": "skill_completion",
        "completed_batch_key": "completion_batch:batch:d1:r1",
    }), _make_ctx())
    results = [{
        "exec_skill_key": "dispatch:d1:r1:1",
        "business_skill_name": "forge-image-gen",
        "workflow_success": False,
        "skill_success": False,
        "skill_status": "launch_failed",
        "skill_result": "worker did not start",
    }]
    mod.handle(_make_event("coordinator.batch_result", {
        "completed_batch_key": "completion_batch:batch:d1:r1",
        "batch_id": "batch:d1:r1",
        "status": "false",
        "successful_count": 0,
        "total_count": 1,
        "results": results,
    }), _make_ctx())
    events = _read_output(tmp_path)
    batch_result = [e for e in events if e["type"] == "batch.result"]
    assert len(batch_result) == 1
    assert batch_result[0]["data"]["results"] == results
    assert batch_result[0]["source_event_seqs"] == [1]


def test_messages_snapshot_is_append_only(monkeypatch, tmp_path):
    """每次保存都追加事件，不覆盖上一次有效消息。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    first = [{"role": "user", "content": "first"}]
    second = first + [{"role": "assistant", "content": "second"}]
    mod.handle(_make_event("coordinator.messages_snapshot", {
        "messages": first,
        "message_count": len(first),
    }), _make_ctx())
    mod.handle(_make_event("coordinator.messages_snapshot", {
        "messages": second,
        "message_count": len(second),
    }), _make_ctx())
    snapshots = [
        event for event in _read_output(tmp_path)
        if event["type"] == "messages.snapshot"
    ]
    assert [event["data"]["messages"] for event in snapshots] == [first, second]


def test_tool_call_result(monkeypatch, tmp_path):
    """Tool Call 与最终有效 Tool Result 均完整落盘。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._current_span_id = lambda: "sp_llm_1"
    # 需要先有 llm.response 的 span 作 parent（_last_llm_response_span）
    mod._last_llm_response_span = "sp_llm_1"
    mod._last_llm_response_seq = 3
    tool_calls = [{
        "tool_call_id": "call_1",
        "ordinal": 1,
        "tool_name": "bash",
        "arguments_json": '{"command":"echo ok"}',
    }]
    tool_results = [{
        "tool_call_id": "call_1",
        "ordinal": 1,
        "tool_name": "bash",
        "arguments": {"command": "echo ok"},
        "status": "completed",
        "result": '{"stdout":"ok","exit_code":0}',
    }]
    mod.handle(_make_event("skill_executor.before_tool_call", {
        "skill_name": "echo", "tool_names": ["bash"], "tool_calls": tool_calls, "round": 1,
    }), _make_ctx())
    mod.handle(_make_event("skill_executor.tool_result", {
        "skill_name": "echo", "tool_names": ["bash"], "results_count": 1,
        "tool_results": tool_results, "round": 1,
    }), _make_ctx())
    evs = _read_output(tmp_path)
    tc = [e for e in evs if e["type"] == "tool.call"]
    tr = [e for e in evs if e["type"] == "tool.result"]
    assert len(tc) == 1 and len(tr) == 1
    assert tc[0]["data"]["tool_names"] == ["bash"]
    assert tc[0]["data"]["tool_calls"] == tool_calls
    assert tc[0]["parent_span_id"] == "sp_llm_1"  # 嵌套在 LLM 下
    assert tr[0]["data"]["results_count"] == 1
    assert tr[0]["data"]["tool_results"] == tool_results
    assert tr[0]["data"]["duration_ms"] is not None
    # 血缘：tool.result → tool.call
    assert tr[0]["source_event_seqs"] == [tc[0]["seq"]]


def test_artifact_persist_records_content_identity(monkeypatch, tmp_path):
    """Artifact 上传结果应与最近 Tool Result 相连并保留内容身份。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._last_llm_response_span = "sp_llm_1"
    mod.handle(_make_event("skill_executor.tool_result", {
        "skill_name": "report-writer",
        "tool_results": [{"tool_name": "write_file", "status": "completed"}],
    }), _make_ctx())
    tool_result = next(
        event for event in _read_output(tmp_path)
        if event["type"] == "tool.result"
    )

    entries = [{
        "source_path": "/tmp/report.pdf",
        "status": "uploaded",
        "storage_path": "outputs/report.pdf",
        "size_bytes": 128,
        "sha256": "a" * 64,
        "artifact_id": "outfit/s0/outputs/report.pdf",
    }]
    mod.handle(_make_event("artifact.storage_result", {
        "status": "uploaded",
        "skill_name": "report-writer",
        "attempted_count": 1,
        "uploaded_count": 1,
        "entries": entries,
    }), _make_ctx())

    persist = next(
        event for event in _read_output(tmp_path)
        if event["type"] == "artifact.persist"
    )
    assert persist["data"]["status"] == "uploaded"
    assert persist["data"]["entries"] == entries
    assert persist["source_event_seqs"] == [tool_result["seq"]]
    assert persist["primary_parent_event_id"] == tool_result["event_id"]

# ═══════════════════════════════════════════════════════════
# skill.dispatch 成功/失败轨迹
# ═══════════════════════════════════════════════════════════

def test_partial_dispatch_records_rejected_manifest(monkeypatch, tmp_path):
    """部分成功时同时记录 requested、accepted、rejected 和执行身份。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._last_llm_response_span = "sp_llm_1"
    mod._last_llm_response_seq = 3
    mod._get_databus = lambda: pytest.fail(
        "payload 已携带 exec_skill_keys 时不应回读 next_skills"
    )
    rejected = [{
        "name": "skill-b",
        "request_index": 2,
        "stage": "manifest_write",
        "error": "DataBus put_global returned false",
    }]
    dispatches = [{
        "request_index": 1,
        "skill_name": "skill-a",
        "prompt": "A",
        "load_skills": ["helper"],
        "force": False,
        "exec_skill_key": "dispatch:d1:r1:1",
        "batch_id": "batch:d1:r1",
    }]

    mod.handle(_make_event("coordinator.after_skill_dispatch", {
        "status": "partially_dispatched",
        "requested_skill_names": ["skill-a", "skill-b"],
        "skill_names": ["skill-a"],
        "batch_id": "batch:d1:r1",
        "exec_skill_keys": ["dispatch:d1:r1:1"],
        "dispatches": dispatches,
        "rejected": rejected,
        "warnings": [],
        "round": 1,
    }), _make_ctx())

    event = [e for e in _read_output(tmp_path) if e["type"] == "skill.dispatch"][0]
    data = event["data"]
    assert data["status"] == "partially_dispatched"
    assert data["requested_skill_names"] == ["skill-a", "skill-b"]
    assert data["skill_names"] == ["skill-a"]
    assert data["batch_id"] == "batch:d1:r1"
    assert data["exec_skill_keys"] == ["dispatch:d1:r1:1"]
    assert data["dispatches"] == dispatches
    assert data["rejected"] == rejected
    assert event["parent_span_id"] == "sp_llm_1"
    assert event["source_event_seqs"] == [3]


def test_failed_dispatch_records_failure_without_stale_exec_key(monkeypatch, tmp_path):
    """整批失败仍落 skill.dispatch，且不从旧 next_skills 拼接错误执行身份。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._get_databus = lambda: pytest.fail(
        "dispatch_failed 不应回读可能残留的 next_skills"
    )
    rejected = [{
        "name": "skill-a",
        "request_index": 1,
        "stage": "manifest_write",
        "error": "DataBus put_global returned false",
    }]

    mod.handle(_make_event("coordinator.after_skill_dispatch", {
        "status": "dispatch_failed",
        "requested_skill_names": ["skill-a"],
        "skill_names": [],
        "batch_id": "batch:d1:r2",
        "exec_skill_keys": [],
        "rejected": rejected,
        "warnings": [],
        "round": 2,
    }), _make_ctx())

    event = [e for e in _read_output(tmp_path) if e["type"] == "skill.dispatch"][0]
    data = event["data"]
    assert data["status"] == "dispatch_failed"
    assert data["requested_skill_names"] == ["skill-a"]
    assert data["skill_names"] == []
    assert data["exec_skill_keys"] == []
    assert data["exec_skill_key"] == ""
    assert data["rejected"] == rejected


# ═══════════════════════════════════════════════════════════
# 事件信封
# ═══════════════════════════════════════════════════════════

def test_event_envelope_schema(monkeypatch, tmp_path):
    """信封含稳定身份、微秒时间和旧版兼容字段。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod.handle(_make_event("agent.dialogue.start", {"trigger_type": "first"}), _make_ctx())
    evs = _read_output(tmp_path)
    start = [e for e in evs if e["type"] == "session.start"][0]
    for field in (
        "schema_version", "event_id", "idempotency_key", "type",
        "semantic_kind", "lifecycle_phase", "seq", "producer_sequence",
        "timestamp_us", "ts", "producer_id", "pid", "trace_id", "run_id",
        "dialogue_id", "session_id", "task_id", "node_id",
        "primary_parent_event_id", "causal_parent_event_ids", "attributes",
        "redaction", "data",
    ):
        assert field in start, f"信封缺字段 {field}: {start}"
    assert start["schema_version"] == 1
    assert start["event_id"] == f'{start["producer_id"]}:{start["seq"]}'
    assert start["idempotency_key"] == start["event_id"]
    assert start["producer_sequence"] == start["seq"]
    assert start["run_id"] == "d1"
    assert start["task_id"] == "task-0"
    assert start["semantic_kind"] == "session"
    assert start["lifecycle_phase"] == "start"
    assert start["primary_parent_event_id"] == ""
    assert start["causal_parent_event_ids"] == []
    assert start["redaction"] == {"applied": False, "policy": "none"}
    assert start["producer_id"].startswith(f'python:{start["pid"]}:')
    assert start["ts"] == start["timestamp_us"] // 1000
    assert start["dialogue_id"] == "d1"
    assert start["session_id"] == "s0"
    assert start["node_id"] == "coordinator_decide"


def test_torn_sessionlog_line_does_not_consume_next_event(monkeypatch, tmp_path):
    """前一进程留下半行后，后续进程事件必须从新行开始并可解析。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    output_dir = tmp_path / "sessionlogs"
    output_dir.mkdir()
    path = output_dir / "d1.jsonl"
    path.write_bytes(b'{"type":"broken"')

    mod._write_jsonl_line(
        str(path),
        json.dumps({"type": "session.start", "seq": 1}, ensure_ascii=False),
    )

    lines = path.read_text(encoding="utf-8").splitlines()


def test_dialogue_error_writes_session_terminal(monkeypatch, tmp_path):
    """Coordinator 未处理异常必须留下可建树的 session.end。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod.handle(_make_event("agent.dialogue.error", {
        "dialogue_id": "d1",
        "status": "unhandled_exception",
        "error_type": "RuntimeError",
        "error": "coordinator failure",
        "elapsed_s": 1.2,
    }), _make_ctx())

    terminal = [
        event for event in _read_output(tmp_path)
        if event["type"] == "session.end"
    ]
    assert len(terminal) == 1
    assert terminal[0]["data"] == {
        "status": "unhandled_exception",
        "response_text": "",
        "error_type": "RuntimeError",
        "error": "coordinator failure",
        "elapsed_s": 1.2,
        "terminal_source": "agent.dialogue.error",
    }


def test_loop_decisions_write_normalized_control_events(monkeypatch, tmp_path):
    """Skill 语义恢复与 Coordinator 纠错使用同一可建树事件形状。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod.handle(_make_event("skill_executor.recovery_decision", {
        "round": 1,
        "next_round": 2,
        "action": "retry",
        "reason": "completion_truncated",
        "skill_name": "file-writer",
        "recovery_attempt": 1,
        "max_recovery_attempts": 2,
        "previous_completion_budget": 4000,
        "completion_budget": 8000,
        "finish_reason": "length",
    }), _make_ctx())
    mod.handle(_make_event("coordinator.tool_loop_retry", {
        "round": 1,
        "tool_loop_round": 0,
        "action": "retry",
        "reason": "no_skill_dispatched",
        "tool_calls": [{"id": "call-dispatch"}],
    }), _make_ctx())

    controls = [
        event for event in _read_output(tmp_path)
        if event["type"] == "loop.control"
    ]
    assert [event["data"]["scope"] for event in controls] == [
        "skill", "coordinator",
    ]
    assert controls[0]["data"]["next_round"] == 2
    assert controls[0]["data"]["completion_budget"] == 8000
    assert controls[0]["data"]["reason"] == "completion_truncated"
    assert controls[1]["data"]["tool_loop_round"] == 0
    assert controls[1]["data"]["reason"] == "no_skill_dispatched"
    assert controls[1]["data"]["tool_calls"] == [{"id": "call-dispatch"}]
