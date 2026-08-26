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
    """AGENT_DIALOGUE_START → session.start + header 落盘，含 completed_skill。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod.handle(_make_event("agent.dialogue.start", {
        "trigger_type": "skill_completion", "model": "m",
    }), _make_ctx())
    evs = _read_output(tmp_path)
    assert len(evs) == 2  # header + session.start
    assert evs[0]["type"] == "session.header"
    assert evs[1]["type"] == "session.start"
    assert evs[1]["data"]["completed_skill"] == ""  # AF_COMPLETED_SKILL_NAME 未设
    assert evs[1]["trace_id"]  # trace_id 必须有


# ═══════════════════════════════════════════════════════════
# llm.request / llm.response
# ═══════════════════════════════════════════════════════════

def test_llm_request_ctx_messages(monkeypatch, tmp_path):
    """BEFORE_LLM_CALL → llm.request 带 ctx.messages 全量（实时）。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._current_span_id = lambda: "sp_llm_1"
    msgs = [{"role": "system", "content": "sys"}, {"role": "user", "content": "hi"}]
    mod.handle(_make_event("coordinator_v1.before_llm_call", {"round": 1, "model": "m"}),
               _make_ctx(messages=msgs))
    evs = _read_output(tmp_path)
    llm_req = [e for e in evs if e["type"] == "llm.request"]
    assert len(llm_req) == 1
    assert llm_req[0]["data"]["messages"] == msgs  # 全量
    assert llm_req[0]["data"]["msg_count"] == 2
    assert llm_req[0]["span_id"] == "sp_llm_1"


def test_llm_response_reads_databus(monkeypatch, tmp_path):
    """AFTER_LLM_CALL → llm.response 从 DataBus 读 response/meta（mock）。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._current_span_id = lambda: "sp_llm_1"

    # mock DataBus：index 尾部 call_id + response/meta
    class FakeNS:
        def __init__(self, store):
            self._store = store

        def get(self, key, default=None):
            return self._store.get(key, default)

    class FakeDB:
        def __init__(self):
            self._store = {
                "traces/index/d1": ["c1", "c2"],  # 尾部 = c2
                "traces/c2/response": {"choices": [{"finish_reason": "stop"}], "usage": {}},
                "traces/c2/meta": {"task_type": "coordinator", "node_id": "coordinator_decide"},
            }

        def llm(self, dialogue_id=None):
            return FakeNS(self._store)

    mod._get_databus = lambda: FakeDB()

    # 先触发 llm.request（让 _last_llm_request_seq 非空，血缘才有引用）
    mod.handle(_make_event("coordinator_v1.before_llm_call", {"round": 1, "model": "m"}),
               _make_ctx(messages=[{"role": "user", "content": "hi"}]))
    mod.handle(_make_event("coordinator_v1.after_llm_call", {"round": 1}),
               _make_ctx())

    evs = _read_output(tmp_path)
    llm_resp = [e for e in evs if e["type"] == "llm.response"]
    assert len(llm_resp) == 1
    assert llm_resp[0]["data"]["call_id"] == "c2"
    assert llm_resp[0]["data"]["response"]["choices"][0]["finish_reason"] == "stop"
    assert llm_resp[0]["data"]["meta"]["task_type"] == "coordinator"
    # 血缘指向 llm.request 的 seq
    assert llm_resp[0]["source_event_seqs"] == [1]


# ═══════════════════════════════════════════════════════════
# skill 事件
# ═══════════════════════════════════════════════════════════

def test_skill_end_has_exec_key(monkeypatch, tmp_path):
    """skill.end 带 exec_skill_key（AF_EXEC_SKILL_KEY）。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:d1:r1:1")
    # 先触发 skill.start（让 _last_skill_start_* 非空）
    mod._current_span_id = lambda: "sp_skill_0"
    mod.handle(_make_event("skill_executor.trigger", {
        "skill_name": "echo", "model": "m", "max_rounds": 20, "tools_count": 7,
    }), _make_ctx())
    mod.handle(_make_event("skill_executor.after_skill_complete", {
        "skill_name": "echo", "round": 1, "status": "completed",
    }), _make_ctx())
    evs = _read_output(tmp_path)
    skill_end = [e for e in evs if e["type"] == "skill.end"]
    assert len(skill_end) == 1
    assert skill_end[0]["data"]["exec_skill_key"] == "dispatch:d1:r1:1"


def test_tool_call_result(monkeypatch, tmp_path):
    """BEFORE/AFTER_TOOL_CALL → tool.call/tool.result（含 duration_ms）。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod._current_span_id = lambda: "sp_llm_1"
    # 需要先有 llm.response 的 span 作 parent（_last_llm_response_span）
    mod._last_llm_response_span = "sp_llm_1"
    mod._last_llm_response_seq = 3
    mod.handle(_make_event("skill_executor.before_tool_call", {
        "skill_name": "echo", "tool_names": ["bash"], "round": 1,
    }), _make_ctx())
    mod.handle(_make_event("skill_executor.after_tool_call", {
        "skill_name": "echo", "tool_names": ["bash"], "results_count": 1, "round": 1,
    }), _make_ctx())
    evs = _read_output(tmp_path)
    tc = [e for e in evs if e["type"] == "tool.call"]
    tr = [e for e in evs if e["type"] == "tool.result"]
    assert len(tc) == 1 and len(tr) == 1
    assert tc[0]["data"]["tool_names"] == ["bash"]
    assert tc[0]["parent_span_id"] == "sp_llm_1"  # 嵌套在 LLM 下
    assert tr[0]["data"]["results_count"] == 1
    assert tr[0]["data"]["duration_ms"] is not None
    # 血缘：tool.result → tool.call
    assert tr[0]["source_event_seqs"] == [tc[0]["seq"]]


# ═══════════════════════════════════════════════════════════
# 事件信封
# ═══════════════════════════════════════════════════════════

def test_event_envelope_schema(monkeypatch, tmp_path):
    """信封字段全集：type/seq/ts/pid/trace_id/dialogue_id/session_id/node_id/data。"""
    mod = next(_load_hook(monkeypatch, tmp_path))
    mod.handle(_make_event("agent.dialogue.start", {"trigger_type": "first"}), _make_ctx())
    evs = _read_output(tmp_path)
    start = [e for e in evs if e["type"] == "session.start"][0]
    for field in ("type", "seq", "ts", "pid", "trace_id", "dialogue_id", "session_id", "node_id", "data"):
        assert field in start, f"信封缺字段 {field}: {start}"
    assert start["dialogue_id"] == "d1"
    assert start["session_id"] == "s0"
    assert start["node_id"] == "coordinator_decide"
