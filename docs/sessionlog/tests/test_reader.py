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


def test_validate_shared_trace_uses_process_sequence(serial_events):
    """同一请求跨多个进程共享 trace_id 时，各进程 seq 可分别从 1 开始。"""
    r = _load_reader()
    evs = [dict(e, trace_id="shared-trace") for e in serial_events]
    assert r.validate(evs) == []


def test_validate_missing_source_sequence(serial_events):
    """source_event_seqs 必须引用同一进程中已存在的事件。"""
    r = _load_reader()
    evs = [dict(e) for e in serial_events]
    evs[2]["source_event_seqs"] = [999]
    issues = r.validate(evs)
    assert any("血缘 seq 不存在" in issue for issue in issues)


def test_validate_dispatch_without_start_or_batch_terminal(serial_events):
    """已成功 dispatch 的 exec key 不能在轨迹中凭空消失。"""
    r = _load_reader()
    evs = [
        e for e in serial_events
        if e.get("trace_id") != "tB"
    ]
    issues = r.validate(evs)
    assert any("无 Skill 启动或 Batch 终态" in issue for issue in issues)


def test_validate_batch_terminal_covers_framework_only_failure(serial_events):
    """未进入 Python 的失败分支可由完整 batch.result 提供权威终态。"""
    r = _load_reader()
    evs = [
        dict(e) for e in serial_events
        if e.get("trace_id") != "tB"
    ]
    for event in evs:
        if event.get("trace_id") == "tC" and event.get("seq", 0) >= 2:
            event["seq"] += 1
    activation_index = next(
        index for index, event in enumerate(evs)
        if event.get("type") == "coordinator.activation"
    )
    activation = evs[activation_index]
    evs.insert(activation_index + 1, {
        **activation,
        "type": "batch.result",
        "seq": 2,
        "source_event_seqs": [1],
        "data": {
            "batch_id": "batch:d1:r1",
            "status": "false",
            "results": [{
                "exec_skill_key": "dispatch:d1:r1:1",
                "business_skill_name": "echo",
                "skill_success": False,
                "skill_status": "launch_failed",
                "skill_result": "worker did not start",
            }],
        },
    })
    assert r.validate(evs) == []


def test_validate_skill_start_without_terminal(serial_events):
    """进入 Python 的 Skill 必须有 skill.end 或 Batch 终态。"""
    r = _load_reader()
    evs = [
        e for e in serial_events
        if not (e.get("type") == "skill.end" and e.get("trace_id") == "tB")
    ]
    issues = r.validate(evs)
    assert any("skill.start 无终态" in issue for issue in issues)


def test_validate_tool_call_without_result(serial_events):
    """Tool 调用不能只有 call 而没有可回放的最终 result。"""
    r = _load_reader()
    evs = [dict(e) for e in serial_events]
    skill_end_index = next(
        index for index, event in enumerate(evs)
        if event.get("type") == "skill.end"
    )
    for event in evs:
        if event.get("trace_id") == "tB" and event.get("seq", 0) >= 4:
            event["seq"] += 1
    evs.insert(skill_end_index, {
        "type": "tool.call",
        "seq": 4,
        "pid": 685,
        "trace_id": "tB",
        "ts": 1065,
        "node_id": "skill_executor",
        "source_event_seqs": [3],
        "data": {
            "tool_calls": [{"tool_call_id": "call-1", "tool_name": "bash"}],
        },
    })
    issues = r.validate(evs)
    assert any("tool.call 无配对 tool.result" in issue for issue in issues)


def test_validate_dialogue_requires_terminal_event(serial_events):
    """完整性检查应识别丢失的 dialogue.end。"""
    r = _load_reader()
    evs = [e for e in serial_events if e.get("type") != "dialogue.end"]
    assert any("dialogue.start 无配对 dialogue.end" in issue for issue in r.validate(evs))


def test_validate_accepts_legacy_session_lifecycle(serial_events):
    """Reader 保持对已落盘 session.* 生命周期事件的只读兼容。"""
    r = _load_reader()
    legacy_events = []
    for event in serial_events:
        legacy = dict(event)
        if legacy.get("type") == "dialogue.start":
            legacy["type"] = "session.start"
        elif legacy.get("type") == "dialogue.end":
            legacy["type"] = "session.end"
        legacy_events.append(legacy)

    assert r.validate(legacy_events) == []


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


def test_build_graph_shared_trace_keeps_process_nodes_distinct(serial_events):
    """端到端 trace 共享后，编排节点仍按执行进程区分。"""
    evs = [dict(e, trace_id="shared-trace") for e in serial_events]
    out = _graph_summary(evs)
    assert "3 节点" in out
    assert "shared-trace:pid668" in out
    assert "shared-trace:pid685" in out
    assert "shared-trace:pid700" in out


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


def test_build_graph_parallel_batch_returns_to_one_activation():
    """同批多个 Skill 应各自回连到唯一的 ALL-join Coordinator 激活。"""
    batch_id = "batch:d1:r1"
    evs = [
        {
            "type": "skill.end", "seq": 1, "pid": 2, "trace_id": "tB",
            "ts": 20, "node_id": "skill_executor",
            "data": {
                "skill_name": "skill-a",
                "exec_skill_key": "dispatch:d1:r1:1",
                "batch_id": batch_id,
            },
        },
        {
            "type": "skill.end", "seq": 1, "pid": 3, "trace_id": "tC",
            "ts": 21, "node_id": "skill_executor",
            "data": {
                "skill_name": "skill-b",
                "exec_skill_key": "dispatch:d1:r1:2",
                "batch_id": batch_id,
            },
        },
        {
            "type": "coordinator.activation", "seq": 1, "pid": 4,
            "trace_id": "tD", "ts": 30, "node_id": "coordinator_decide",
            "data": {
                "trigger_type": "skill_completion",
                "completed_skill": "skill-batch",
                "batch_id": batch_id,
            },
        },
    ]

    out = _graph_summary(evs)

    assert "3 节点，2 边" in out
    assert out.count("return") == 2
    assert "skill=skill-a" in out
    assert "skill=skill-b" in out

def test_build_graph_parallel_skills_distinct():
    """一次派发 2 个并行 skill（exec_key 不同）→ 2 条派发边，不串。"""
    r = _load_reader()
    # 构造：coordinator 派发 echo×2（exec_key :1 和 :2），两个 skill 各自执行
    evs = [
        {"type": "dialogue.start", "seq": 1, "pid": 1, "trace_id": "tA", "ts": 0,
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


def test_build_graph_parallel_dispatch_uses_matching_skill_name():
    """不同并行 Skill 的派发边必须按 exec key 使用各自名称。"""
    evs = [
        {
            "type": "skill.dispatch", "seq": 1, "pid": 1,
            "trace_id": "tA", "ts": 1, "node_id": "coordinator_decide",
            "data": {
                "skill_names": ["skill-a", "skill-b"],
                "exec_skill_keys": ["dispatch:d:r:1", "dispatch:d:r:2"],
            },
        },
        {
            "type": "skill.start", "seq": 1, "pid": 2,
            "trace_id": "tB", "ts": 2, "node_id": "skill_executor",
            "data": {
                "skill_name": "skill-a",
                "exec_skill_key": "dispatch:d:r:1",
            },
        },
        {
            "type": "skill.start", "seq": 1, "pid": 3,
            "trace_id": "tC", "ts": 3, "node_id": "skill_executor",
            "data": {
                "skill_name": "skill-b",
                "exec_skill_key": "dispatch:d:r:2",
            },
        },
    ]
    out = _graph_summary(evs)
    assert out.count("skill=skill-a") >= 1
    assert out.count("skill=skill-b") >= 1


def test_build_graph_framework_only_failure_uses_batch_result():
    """launch_failed 分支无 skill.start/end 时仍形成 dispatch/return 两条边。"""
    exec_key = "dispatch:d:r:1"
    evs = [
        {
            "type": "skill.dispatch", "seq": 1, "pid": 1,
            "trace_id": "tA", "ts": 1, "node_id": "coordinator_decide",
            "data": {
                "skill_names": ["forge-image-gen"],
                "exec_skill_keys": [exec_key],
            },
        },
        {
            "type": "coordinator.activation", "seq": 1, "pid": 2,
            "trace_id": "tB", "ts": 10, "node_id": "coordinator_decide",
            "data": {
                "trigger_type": "skill_completion",
                "batch_id": "batch:d:r",
            },
        },
        {
            "type": "batch.result", "seq": 2, "pid": 2,
            "trace_id": "tB", "ts": 11, "node_id": "coordinator_decide",
            "data": {
                "batch_id": "batch:d:r",
                "status": "false",
                "results": [{
                    "exec_skill_key": exec_key,
                    "business_skill_name": "forge-image-gen",
                    "skill_status": "launch_failed",
                }],
            },
        },
    ]
    out = _graph_summary(evs)
    assert "3 节点，2 边" in out
    lines = out.splitlines()
    assert sum(line.startswith("  dispatch") for line in lines) == 1
    assert sum(line.startswith("  return") for line in lines) == 1
    assert "skill=forge-image-gen" in out


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
    good = {"type": "dialogue.start", "seq": 1, "ts": 1, "pid": 1, "trace_id": "tA",
            "dialogue_id": "d1", "node_id": "x", "data": {}}
    p.write_text(
        json.dumps(good) + "\n" + '{"type":"broken","seq":2',
        encoding="utf-8",
    )
    evs = r.load("d1", str(p))
    assert len(evs) == 1
    assert evs[0]["type"] == "dialogue.start"


@pytest.mark.parametrize("broken_line", [
    '{"type":"broken","seq":2\n',
    b'{"type":"broken","seq":"\xff"}\n',
])
def test_load_complete_corrupt_record_raises(tmp_path, broken_line):
    """中段或带换行的坏记录必须显式失败，不能静默形成缺事件轨迹。"""
    r = _load_reader()
    p = tmp_path / "d1.jsonl"
    good = json.dumps({"type": "dialogue.header", "dialogue_id": "d1"})
    if isinstance(broken_line, bytes):
        p.write_bytes(good.encode() + b"\n" + broken_line)
    else:
        p.write_text(good + "\n" + broken_line, encoding="utf-8")

    with pytest.raises(ValueError, match="corruption at line 2"):
        r.load("d1", str(p))


def test_validate_strict_header_identity_and_unique_ids(serial_events):
    """真实文件严格模式要求唯一 Header、身份一致且事件 ID 幂等唯一。"""
    r = _load_reader()
    events = [{
        "type": "dialogue.header",
        "version": 2,
        "dialogue_id": "d1",
    }]
    for index, event in enumerate(serial_events, 1):
        item = dict(event)
        item["event_id"] = f"producer:{index}"
        item["idempotency_key"] = item["event_id"]
        events.append(item)

    assert r.validate(events, require_header=True) == []

    duplicate = [dict(event) for event in events]
    duplicate[2]["event_id"] = duplicate[1]["event_id"]
    duplicate[2]["idempotency_key"] = duplicate[1]["idempotency_key"]
    duplicate[2]["dialogue_id"] = "other-dialogue"
    issues = r.validate(duplicate, require_header=True)

    assert any("event_id 重复" in issue for issue in issues)
    assert any("idempotency_key 重复" in issue for issue in issues)
    assert any("dialogue_id 与 header 不一致" in issue for issue in issues)


def test_validate_strict_rejects_duplicate_dialogue_terminal(serial_events):
    r = _load_reader()
    events = [
        {"type": "dialogue.header", "version": 2, "dialogue_id": "d1"},
        *serial_events,
        dict(serial_events[-1], seq=5),
    ]

    issues = r.validate(events, require_header=True)

    assert any("dialogue.end 终态重复" in issue for issue in issues)


def test_load_finds_hierarchical_dialogue_by_default(tmp_path, monkeypatch):
    """只给 dialogue_id 时，Reader 能发现 User/Conversation 下的唯一实体文件。"""
    r = _load_reader()
    path = (
        tmp_path / "trace_output" / "sessionlogs" / "by-user"
        / "user-a" / "client-a" / "d1.jsonl"
    )
    path.parent.mkdir(parents=True)
    path.write_text(
        json.dumps({"type": "dialogue.header", "dialogue_id": "d1"}) + "\n",
        encoding="utf-8",
    )
    monkeypatch.chdir(tmp_path)

    assert r.load("d1") == [{"type": "dialogue.header", "dialogue_id": "d1"}]


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
