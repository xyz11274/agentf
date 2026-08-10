"""tool_policies 单元测试：混合批次调度（只读并行、写串行、保序、分类判定）。

对应设计文档 tools/system_tools/2.工具执行并行优化.md §3：
- classify_tool:   只读/写判定（只看参数，不猜语义，保守算写）
- build_batches:   连续只读归并、写/慢单独串行批
- execute_in_batches: 批次间串行、批次内并行、pool.map 保序
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PKG_SRC = PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
if str(PKG_SRC) not in sys.path:
    sys.path.insert(0, str(PKG_SRC))

from agentflow_runtime.tool_policies import (
    build_batches,
    classify_tool,
    execute_in_batches,
)


def _mk_tc(name: str, command: str = "cat x", tc_id: str = "t"):
    """构造一个 tool_call dict（OpenAI 兼容格式）。"""
    return {
        "id": tc_id,
        "type": "function",
        "function": {
            "name": name,
            "arguments": json.dumps({"command": command}) if name == "bash" else "{}",
        },
    }


# ════════════════════════════════════════════════════════════════════
# classify_tool
# ════════════════════════════════════════════════════════════════════


def test_classify_tool_explicit_names():
    assert classify_tool("read_file", {}) == "read"
    assert classify_tool("list_skills", {}) == "read"
    assert classify_tool("load_skill", {}) == "read"
    assert classify_tool("write_file", {}) == "write"
    assert classify_tool("edit_file", {}) == "write"


def test_classify_tool_bash_read_markers():
    assert classify_tool("bash", {"command": "cat a.md"}) == "read"
    assert classify_tool("bash", {"command": "ls -la"}) == "read"
    assert classify_tool("bash", {"command": "echo hello"}) == "read"
    assert classify_tool("bash", {"command": "python3 weather.py 北京"}) == "read"


def test_classify_tool_bash_write_markers():
    assert classify_tool("bash", {"command": "mv a b"}) == "write"
    assert classify_tool("bash", {"command": "rm -rf /tmp/x"}) == "write"
    assert classify_tool("bash", {"command": "mkdir -p out"}) == "write"
    assert classify_tool("bash", {"command": "python3 generate.py --prompt x"}) == "write"
    assert classify_tool("bash", {"command": "echo x > /tmp/a.txt"}) == "write"


def test_classify_tool_unknown_conservative():
    # 未知命令/未知工具 → 保守算 write（串行）
    assert classify_tool("bash", {"command": "some_unknown_cmd"}) == "write"
    assert classify_tool("unknown_tool", {}) == "write"


# ════════════════════════════════════════════════════════════════════
# build_batches
# ════════════════════════════════════════════════════════════════════


def test_build_batches_read_write_groups():
    """[读,读,写,读,读] → [[读,读],[写],[读,读]]——连续只读归并、写单独串行批。"""
    tcs = [
        _mk_tc("bash", "cat a", "t1"),
        _mk_tc("bash", "cat b", "t2"),
        _mk_tc("write_file", tc_id="t3"),
        _mk_tc("bash", "cat c", "t4"),
        _mk_tc("bash", "cat d", "t5"),
    ]
    batches = build_batches(tcs)
    assert len(batches) == 3, f"应 3 个批次，实际 {batches}"
    assert [t["id"] for t in batches[0]] == ["t1", "t2"]   # 并行批
    assert [t["id"] for t in batches[1]] == ["t3"]        # 写串行批
    assert [t["id"] for t in batches[2]] == ["t4", "t5"]  # 并行批


def test_build_batches_all_read_single_batch():
    """全只读 → 单一批次（最优）。"""
    tcs = [_mk_tc("bash", "cat a"), _mk_tc("bash", "cat b"), _mk_tc("bash", "cat c")]
    assert len(build_batches(tcs)) == 1


def test_build_batches_all_write_all_serial():
    """全写 → 每工具一个串行批（最安全）。"""
    tcs = [_mk_tc("write_file", tc_id="a"), _mk_tc("write_file", tc_id="b")]
    batches = build_batches(tcs)
    assert len(batches) == 2
    assert all(len(b) == 1 for b in batches)


# ════════════════════════════════════════════════════════════════════
# execute_in_batches：并行/串行/保序
# ════════════════════════════════════════════════════════════════════


def _timed_execute(calls: list):
    """返回一个 execute_fn：记录 (id, start, end)，sleep 0.2s 模拟工具耗时。"""
    def execute_fn(tc):
        start = time.monotonic()
        time.sleep(0.2)
        end = time.monotonic()
        calls.append((tc["id"], start, end))
        return f"ok:{tc['id']}"
    return execute_fn


def test_execute_in_batches_parallel_reads():
    """只读工具并行：总耗时 < 串行和（时间戳重叠）。"""
    calls: list = []
    tcs = [_mk_tc("bash", "cat a", "r1"), _mk_tc("bash", "cat b", "r2"),
           _mk_tc("bash", "cat c", "r3")]
    results = execute_in_batches(tcs, _timed_execute(calls))

    assert len(results) == 3
    assert results == ["ok:r1", "ok:r2", "ok:r3"]  # 保序
    total = max(e for _, _, e in calls) - min(s for _, s, _ in calls)
    assert total < 0.5, f"3 个只读应并行（<0.5s），实际 {total:.3f}s"


def test_execute_in_batches_serial_writes():
    """写工具串行：时间戳不重叠（第二个开始 >= 第一个结束）。"""
    calls: list = []
    tcs = [_mk_tc("write_file", tc_id="w1"), _mk_tc("write_file", tc_id="w2")]
    execute_in_batches(tcs, _timed_execute(calls))

    assert len(calls) == 2
    _, s1, e1 = calls[0]
    _, s2, _ = calls[1]
    assert s2 >= e1, f"写工具应串行（s2 >= e1），实际 s2={s2:.3f} e1={e1:.3f}"


def test_execute_in_batches_mixed_preserves_order():
    """混合批次：[读,读,写,读] → 结果保序（results[i] 对应 tool_calls[i]）。"""
    def execute_fn(tc):
        return f"result:{tc['id']}"

    tcs = [
        _mk_tc("bash", "cat a", "r1"),
        _mk_tc("bash", "cat b", "r2"),
        _mk_tc("write_file", tc_id="w1"),
        _mk_tc("bash", "cat c", "r3"),
    ]
    results = execute_in_batches(tcs, execute_fn)
    assert results == ["result:r1", "result:r2", "result:w1", "result:r3"]
