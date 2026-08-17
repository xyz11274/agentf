"""trace.emit 测试 — fallback trace_id 进程级稳定（数据零散根因回归）。

设计文档 §6.2：Python 子进程由 daemon 注入 AF_TRACE_ID / AF_PARENT_SPAN_ID；
未注入时退化到 _fallback_trace_id()。退化也必须是**进程级稳定**——
同进程多个 span 共享同一 trace_id，viewer 才能按 trace_id 聚合 span 树
（F3 不变量 I-1 不丢失业务数据：python_spans_.ndjson 48 span 全独立 trace_id
= 每 span 一个独立 session，无法聚合展示）。
"""

from __future__ import annotations

import json
import os

import pytest

from agentflow_runtime.trace import SpanKind, SpanStatus
from agentflow_runtime.trace.emit import emit_span
from agentflow_runtime.trace.span_context import SpanContext
from agentflow_runtime.trace.span_writer import SpanWriter


@pytest.fixture(autouse=True)
def reset_trace_singletons(tmp_path: "object", monkeypatch: "object") -> "object":
    """每个测试隔离：重置单例 + 输出目录指向 tmp_path。"""
    monkeypatch.setenv("AF_TRACE_OUTPUT_DIR", str(tmp_path))
    SpanContext.current().clear()
    SpanWriter._instance = None  # type: ignore[attr-defined]
    yield tmp_path
    SpanContext.current().clear()
    SpanWriter._instance = None  # type: ignore[attr-defined]


def test_fallback_trace_id_process_stable(tmp_path) -> None:
    """未注入 AF_TRACE_ID 时，同进程连续 emit_span 必须共享同一 trace_id。

    回归：_fallback_trace_id() 每次 now_us() 生成新 trace_id → 每 span 独立
    session，viewer 无法聚合。
    """
    os.environ.pop("AF_TRACE_ID", None)
    os.environ.pop("AF_PARENT_SPAN_ID", None)

    trace_ids: list[str] = []
    with emit_span(SpanKind.LLM, "coordinator.llm_round") as sp1:
        sp1.set_status(SpanStatus.OK)
        trace_ids.append(sp1.trace_id)
    with emit_span(SpanKind.LLM, "coordinator.llm_round") as sp2:
        sp2.set_status(SpanStatus.OK)
        trace_ids.append(sp2.trace_id)
    with emit_span(SpanKind.COORDINATOR, "coordinator") as sp3:
        sp3.set_status(SpanStatus.OK)
        trace_ids.append(sp3.trace_id)

    assert len(set(trace_ids)) == 1, f"同进程 span 必须共享同一 trace_id: {trace_ids}"


def test_nested_span_parent_chain(tmp_path) -> None:
    """嵌套 emit_span：子 span 的 parent 应为父 span，trace_id 一致。"""
    os.environ.pop("AF_TRACE_ID", None)
    os.environ.pop("AF_PARENT_SPAN_ID", None)

    with emit_span(SpanKind.COORDINATOR, "coordinator") as parent:
        with emit_span(SpanKind.LLM, "llm_round_1") as child:
            child.set_status(SpanStatus.OK)
            assert child.parent_span_id == parent.span_id
            assert child.trace_id == parent.trace_id
        parent.set_status(SpanStatus.OK)


def test_written_spans_share_trace_id(tmp_path) -> None:
    """落盘 NDJSON 的同进程 span 必须共享同一 trace_id（viewer 聚合前提）。"""
    os.environ.pop("AF_TRACE_ID", None)
    os.environ.pop("AF_PARENT_SPAN_ID", None)
    os.environ.pop("AF_DIALOGUE_ID", None)

    with emit_span(SpanKind.LLM, "coordinator.llm_round") as sp1:
        sp1.set_status(SpanStatus.OK)
    with emit_span(SpanKind.LLM, "coordinator.llm_round") as sp2:
        sp2.set_status(SpanStatus.OK)

    files = list(tmp_path.rglob("python_spans_*.ndjson"))
    assert files, f"应有落盘文件，实际: {list(tmp_path.rglob('*'))}"
    lines = [json.loads(line) for f in files for line in open(f) if line.strip()]
    assert len(lines) == 2
    trace_ids = {s["trace_id"] for s in lines}
    assert len(trace_ids) == 1, f"落盘 span 必须共享同一 trace_id: {trace_ids}"


def test_injected_trace_id_wins(tmp_path) -> None:
    """注入 AF_TRACE_ID 时使用注入值（不退化到 fallback）。"""
    os.environ["AF_TRACE_ID"] = "trace_abc_123"
    os.environ["AF_PARENT_SPAN_ID"] = "span_parent"

    with emit_span(SpanKind.LLM, "coordinator.llm_round") as sp:
        sp.set_status(SpanStatus.OK)
        assert sp.trace_id == "trace_abc_123"
        assert sp.parent_span_id == "span_parent"
