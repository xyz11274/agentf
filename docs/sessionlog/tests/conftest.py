"""sessionlog 测试公共 fixture。

- tmp 输出目录隔离（每个测试独立，不污染真实 trace_output）
- 环境变量隔离（AF_SESSIONLOG_* / AF_TRACE_OUTPUT_DIR / AF_* 节点 env）
- hook 模块状态重置（_ENABLED 是模块级，需 reload）
"""

from __future__ import annotations

import importlib
import os
import sys

import pytest

# 被测模块路径（docs/sessionlog/）
HOOK_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "sessionlog_hook.py")
READER_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "sessionlog_reader.py")


def _load_module(name: str, path: str):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def hook_mod(tmp_path, monkeypatch):
    """加载 hook 模块（输出目录指向 tmp_path，启用开关）。"""
    monkeypatch.setenv("AF_SESSIONLOG_ENABLED", "1")
    monkeypatch.setenv("AF_TRACE_OUTPUT_DIR", str(tmp_path))
    monkeypatch.setenv("AF_DIALOGUE_ID", "test-dialogue")
    monkeypatch.setenv("AF_SESSION_ID", "test-session")
    monkeypatch.setenv("AF_NODE_ID", "coordinator_decide")
    mod = _load_module("sessionlog_hook_test", HOOK_PATH)
    yield mod
    # 清理：重置单例状态
    sys.modules.pop("sessionlog_hook_test", None)


@pytest.fixture()
def reader_mod():
    """加载 reader 模块（纯函数，无状态）。"""
    mod = _load_module("sessionlog_reader_test", READER_PATH)
    yield mod
    sys.modules.pop("sessionlog_reader_test", None)


@pytest.fixture()
def serial_events():
    """一条完整串行链（A→B→C）的构造事件：coordinator 决策 → skill → coordinator 收尾。

    对应真实任务形态（echo 串行），字段与 hook 落盘一致。
    ts 递增模拟真实时序（reader 的边匹配要求 start.ts > dispatch.ts）。
    """
    import copy
    base = {
        "type": None, "seq": None, "ts": None, "pid": None,
        "trace_id": None, "dialogue_id": "d1", "session_id": "s0",
        "node_id": None, "data": {},
    }
    _ts = iter(range(1000, 1200, 10))

    def ev(**kw):
        d = copy.deepcopy(base)
        d.update(kw)
        if d.get("ts") is None:
            d["ts"] = next(_ts)
        return d

    return [
        # coordinator A（pid 668, trace tA）
        ev(type="session.start", seq=1, pid=668, trace_id="tA", node_id="coordinator_decide",
           data={"trigger_type": "first", "model": "m", "completed_skill": ""}),
        ev(type="llm.request", seq=2, pid=668, trace_id="tA", node_id="coordinator_decide", round=1,
           span_id="spA1", data={"model": "m", "messages": [{"role": "user", "content": "hi"}], "msg_count": 1}),
        ev(type="llm.response", seq=3, pid=668, trace_id="tA", node_id="coordinator_decide", round=1,
           span_id="spA1", source_event_seqs=[2],
           data={"call_id": "c1", "response": {"choices": [{"finish_reason": "tool_calls"}]},
                 "meta": {"task_type": "coordinator", "node_id": "coordinator_decide"}}),
        ev(type="skill.dispatch", seq=4, pid=668, trace_id="tA", node_id="coordinator_decide", round=1,
           parent_span_id="spA1", source_event_seqs=[3],
           data={"skill_names": ["echo"], "round": 1, "exec_skill_keys": ["dispatch:d1:r1:1"]}),
        # skill B（pid 685, trace tB）
        ev(type="skill.start", seq=1, pid=685, trace_id="tB", node_id="skill_executor",
           span_id="spB0", data={"skill_name": "echo", "exec_skill_key": "dispatch:d1:r1:1"}),
        ev(type="llm.request", seq=2, pid=685, trace_id="tB", node_id="skill_executor", round=1,
           span_id="spB1", parent_span_id="spB0",
           data={"model": "m", "messages": [{"role": "user", "content": "echo"}], "msg_count": 1}),
        ev(type="llm.response", seq=3, pid=685, trace_id="tB", node_id="skill_executor", round=1,
           span_id="spB1", parent_span_id="spB0", source_event_seqs=[2],
           data={"call_id": "c2", "response": {"choices": [{"finish_reason": "stop"}]},
                 "meta": {"task_type": "skill", "node_id": "skill_executor"}}),
        ev(type="skill.end", seq=4, pid=685, trace_id="tB", node_id="skill_executor", round=1,
           span_id="spB0", parent_span_id="spB0", source_event_seqs=[1],
           data={"skill_name": "echo", "status": "completed", "exec_skill_key": "dispatch:d1:r1:1"}),
        # coordinator C（pid 700, trace tC）——skill 完成触发的新决策段
        ev(type="session.start", seq=1, pid=700, trace_id="tC", node_id="coordinator_decide",
           data={"trigger_type": "skill_completion", "model": "m", "completed_skill": "echo"}),
        ev(type="llm.request", seq=2, pid=700, trace_id="tC", node_id="coordinator_decide", round=2,
           span_id="spC1", data={"model": "m", "messages": [{"role": "user", "content": "hi"}], "msg_count": 1}),
        ev(type="llm.response", seq=3, pid=700, trace_id="tC", node_id="coordinator_decide", round=2,
           span_id="spC1", source_event_seqs=[2],
           data={"call_id": "c3", "response": {"choices": [{"finish_reason": "stop"}]},
                 "meta": {"task_type": "coordinator", "node_id": "coordinator_decide"}}),
        ev(type="session.end", seq=4, pid=700, trace_id="tC", node_id="coordinator_decide",
           source_event_seqs=[3], data={"status": "normal", "response_text": "ok"}),
    ]
