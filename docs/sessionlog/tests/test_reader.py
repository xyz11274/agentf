"""reader 单元测试 — 校验 / 重放 / 编排图（纯函数，不依赖引擎）。

覆盖：
- validate：seq 递增、llm.request/response 配对、血缘引用
- build_graph：节点=trace_id、派发边 exec_key 匹配、回传边、并行 skill 区分
- load：torn 行丢弃
- replay：恢复 messages 序列
"""

from __future__ import annotations

import json
import os

import pytest

from conftest import READER_PATH


def _load_reader():
    import importlib.util
    spec = importlib.util.spec_from_file_location("sessionlog_reader_ut", READER_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ═══════════════════════════════════════════════════════════
# validate
# ═══════════════════════════════════════════════════════════

def test_validate_serial_chain_passes(serial_events):
    """完整串行链（A→B→C）校验通过：seq 递增、llm 配对、血缘完整。"""
    r = _load_reader()
    issues = r.validate(serial_events)
    assert issues == [], f"串行链应校验通过，问题: {issues}"


def test_validate_seq_not_monotonic(serial_events):
    """同一 trace_id 内 seq 乱序 → 报错。"""
    r = _load_reader()
    evs = [dict(e) for e in serial_events]
    # 把 tA 的 seq2/seq3 对调
    evs[1]["seq"] = 3
    evs[2]["seq"] = 2
    issues = r.validate(evs)
    assert any("seq 不递增" in i for i in issues), f"应报 seq 不递增: {issues}"


def test_validate_seq_duplicate(serial_events):
    """同一 trace_id 内 seq 重复 → 报错。"""
    r = _load_reader()
    evs = [dict(e) for e in serial_events]
    evs[2]["seq"] = 1  # tA 出现两个 seq=1
    issues = r.validate(evs)
    assert any("seq 重复" in i for i in issues), f"应报 seq 重复: {issues}"


def test_validate_llm_request_without_response(serial_events):
    """llm.request 无配对 llm.response（同 span_id 缺失）→ 报错。"""
    r = _load_reader()
    evs = [dict(e) for e in serial_events]
    # 删掉 tA 的 llm.response
    evs = [e for e in evs if not (e["type"] == "llm.response" and e["trace_id"] == "tA")]
    issues = r.validate(evs)
    assert any("无配对" in i for i in issues), f"应报 llm 未配对: {issues}"


# ═══════════════════════════════════════════════════════════
# build_graph
# ═══════════════════════════════════════════════════════════

def _graph_summary(evs):
    """调 build_graph 并捕获打印输出（节点/边）。"""
    r = _load_reader()
    import io
    from contextlib import redirect_stdout
    buf = io.StringIO()
    with redirect_stdout(buf):
        r.build_graph(evs)
    return buf.getvalue()


def test_build_graph_nodes_are_trace_ids(serial_events):
    """节点 = trace_id（3 个执行单元）。"""
    out = _graph_summary(serial_events)
    assert "3 节点" in out, f"应有 3 节点: {out}"
    assert "tA" in out and "tB" in out and "tC" in out


def test_build_graph_dispatch_edge_exec_key(serial_events):
    """派发边：exec_key 精确匹配 A→B。"""
    out = _graph_summary(serial_events)
    assert "dispatch" in out
    assert "tA" in out and "tB" in out
    assert "skill=echo" in out


def test_build_graph_return_edge_serial(serial_events):
    """回传边：B→C（skill.end 连到新决策段，非 B→A）。"""
    out = _graph_summary(serial_events)
    assert "return" in out
    # 回传边 from=B(tB) to=C(tC)
    assert "tB" in out and "tC" in out


def test_build_graph_parallel_skills_distinct():
    """一次派发 2 个并行 skill（exec_key 不同）→ 2 条派发边，不串。"""
    r = _load_reader()
    # 构造：coordinator 派发 echo×2（exec_key :1 和 :2），两个 skill 各自执行
    evs = [
        {"type": "session.start", "seq": 1, "pid": 1, "trace_id": "tA", "ts": 0,
         "dialogue_id": "d1", "node_id": "coordinator_decide", "data": {}},
        {"type": "llm.request", "seq": 2, "pid": 1, "trace_id": "tA", "ts": 1,
         "span_id": "spA", "node_id": "coordinator_decide", "data": {"msg_count": 1}},
        {"type": "llm.response", "seq": 3, "pid": 1, "trace_id": "tA", "ts": 2,
         "span_id": "spA", "source_event_seqs": [2], "node_id": "coordinator_decide",
         "data": {"response": {"choices": [{"finish_reason": "tool_calls"}]}, "meta": {}}},
        {"type": "skill.dispatch", "seq": 4, "pid": 1, "trace_id": "tA", "ts": 3,
         "parent_span_id": "spA", "source_event_seqs": [3], "node_id": "coordinator_decide",
         "data": {"skill_names": ["echo"], "round": 1,
                  "exec_skill_keys": ["dispatch:d1:r1:1", "dispatch:d1:r1:2"]}},
        # skill 1（tB）
        {"type": "skill.start", "seq": 1, "pid": 2, "trace_id": "tB", "ts": 10,
         "node_id": "skill_executor", "data": {"skill_name": "echo", "exec_skill_key": "dispatch:d1:r1:1"}},
        {"type": "skill.end", "seq": 2, "pid": 2, "trace_id": "tB", "ts": 20,
         "span_id": "spB", "parent_span_id": "spB", "node_id": "skill_executor",
         "data": {"skill_name": "echo", "status": "completed", "exec_skill_key": "dispatch:d1:r1:1"}},
        # skill 2（tC）
        {"type": "skill.start", "seq": 1, "pid": 3, "trace_id": "tC", "ts": 11,
         "node_id": "skill_executor", "data": {"skill_name": "echo", "exec_skill_key": "dispatch:d1:r1:2"}},
        {"type": "skill.end", "seq": 2, "pid": 3, "trace_id": "tC", "ts": 21,
         "span_id": "spC", "parent_span_id": "spC", "node_id": "skill_executor",
         "data": {"skill_name": "echo", "status": "completed", "exec_skill_key": "dispatch:d1:r1:2"}},
    ]
    out = _graph_summary(evs)
    # 2 条派发边（tA→tB 和 tA→tC），exec_key 区分
    assert "2 边" in out or "dispatch" in out
    assert out.count("dispatch") >= 2, f"应有 2 条派发边: {out}"


def test_build_graph_dispatch_fallback_name(serial_events):
    """无 exec_key 时派发边用名称+时序兜底（向后兼容旧数据）。"""
    r = _load_reader()
    evs = [dict(e) for e in serial_events]
    for e in evs:
        if e["type"] == "skill.dispatch":
            e["data"] = dict(e["data"])
            e["data"].pop("exec_skill_keys", None)
    out = _graph_summary(evs)
    assert "dispatch" in out, f"名称兜底应仍能建派发边: {out}"


# ═══════════════════════════════════════════════════════════
# load / replay
# ═══════════════════════════════════════════════════════════

def test_load_torn_line_dropped(tmp_path):
    """torn 尾行（坏 JSON）被丢弃，正常行保留。"""
    r = _load_reader()
    p = tmp_path / "d1.jsonl"
    good = {"type": "session.start", "seq": 1, "ts": 1, "pid": 1, "trace_id": "tA",
            "dialogue_id": "d1", "node_id": "x", "data": {}}
    p.write_text(
        json.dumps(good) + "\n" + '{"type":"broken","seq":2' + "\n",
        encoding="utf-8",
    )
    evs = r.load("d1", str(p))
    assert len(evs) == 1
    assert evs[0]["type"] == "session.start"


def test_replay_restores_messages(serial_events):
    """replay 输出 llm.request 的 messages 摘要（恢复模型看到的一切）。"""
    r = _load_reader()
    import io
    from contextlib import redirect_stdout
    buf = io.StringIO()
    with redirect_stdout(buf):
        r.replay(serial_events)
    out = buf.getvalue()
    assert "llm.request" in out
    assert "msgs=1" in out  # 每个 request 的 msg_count
