"""trace.emit 测试 — fallback trace_id 进程级稳定（数据零散根因回归）。

设计文档 §6.2：Python 子进程由 daemon 注入 AF_TRACE_ID / AF_PARENT_SPAN_ID；
未注入时退化到 _fallback_trace_id()。退化也必须是**进程级稳定**——
同进程多个 span 共享同一 trace_id，viewer 才能按 trace_id 聚合 span 树
（F3 不变量 I-1 不丢失业务数据：python_spans_.ndjson 48 span 全独立 trace_id
= 每 span 一个独立 session，无法聚合展示）。
"""

from __future__ import annotations

import json
import multiprocessing
import os

import pytest

from agentflow_runtime.trace import SpanKind, SpanStatus, TraceSpan
from agentflow_runtime.trace.emit import emit_span
from agentflow_runtime.trace.span_context import SpanContext
from agentflow_runtime.trace.span_writer import SpanWriter
from agentflow_runtime.observability import emit_event


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


def test_emit_event_is_persisted_in_active_span(tmp_path) -> None:
    """emit_span 内的 emit_event 必须能从 NDJSON Span 回放。"""
    os.environ["AF_DIALOGUE_ID"] = "dialogue-event"

    with emit_span(SpanKind.TOOL, "tool_batch"):
        emit_event("tool.called", {"tool_name": "read_file"}, level="WARN")

    files = list(tmp_path.rglob("python_spans_*.ndjson"))
    assert len(files) == 1
    spans = [json.loads(line) for line in open(files[0]) if line.strip()]
    assert len(spans) == 1
    assert len(spans[0]["events"]) == 1
    event = spans[0]["events"][0]
    assert event["name"] == "tool.called"
    assert event["attributes"] == {
        "tool_name": "read_file",
        "dialogue_id": "dialogue-event",
        "session_id": "",
        "node_id": "",
        "task_id": "",
        "level": "WARN",
    }


def test_legacy_dual_event_write_is_not_duplicated(tmp_path) -> None:
    """同一事件经两个兼容 API 相邻写入时只能生成一条记录。"""
    os.environ["AF_DIALOGUE_ID"] = "dialogue-event-dedup"

    with emit_span(SpanKind.LLM, "llm_round") as span:
        span.add_event("llm.result", {"finish_reason": "stop"})
        emit_event("llm.result", {"round": 1})

    files = list(tmp_path.rglob("python_spans_*.ndjson"))
    spans = [json.loads(line) for line in open(files[0]) if line.strip()]
    events = spans[0]["events"]
    assert len(events) == 1
    assert events[0]["name"] == "llm.result"
    assert events[0]["attributes"]["finish_reason"] == "stop"
    assert events[0]["attributes"]["round"] == "1"


def _write_spans_in_child(output_dir: str, dialogue_id: str, worker: int,
                          count: int, start_event) -> None:
    """Process target: all children append large records to one dialogue file."""
    os.environ["AF_DIALOGUE_ID"] = dialogue_id
    writer = SpanWriter(output_dir=output_dir)
    start_event.wait()
    for index in range(count):
        writer.write_span(TraceSpan(
            trace_id="trace-multiprocess",
            span_id=f"worker-{worker}-{index}",
            kind=SpanKind.SKILL,
            name="multiprocess-write",
            start_us=1,
            end_us=2,
            status=SpanStatus.OK,
            tags={"payload": "x" * 65536},
        ))


def test_torn_span_line_does_not_consume_next_record(tmp_path, monkeypatch) -> None:
    """被杀进程的半行必须和后续完整 Span 隔离。"""
    dialogue_id = "dialogue-torn-span"
    monkeypatch.setenv("AF_DIALOGUE_ID", dialogue_id)
    path = tmp_path / f"python_spans_{dialogue_id}.ndjson"
    path.write_bytes(b'{"trace_id":"torn"')

    SpanWriter(output_dir=str(tmp_path)).write_span(TraceSpan(
        trace_id="trace-ok",
        span_id="span-ok",
        kind=SpanKind.LLM,
        name="after-torn",
        start_us=1,
        end_us=2,
        status=SpanStatus.OK,
    ))

    lines = path.read_text(encoding="utf-8").splitlines()
    assert len(lines) == 2
    with pytest.raises(json.JSONDecodeError):
        json.loads(lines[0])
    assert json.loads(lines[1])["span_id"] == "span-ok"


@pytest.mark.skipif(os.name != "posix", reason="flock is a POSIX runtime contract")
def test_multiple_processes_write_parseable_spans(tmp_path) -> None:
    """兄弟 Skill 进程并发写同一 dialogue 时，每条 Span 都独立可解析。"""
    process_count = 3
    spans_per_process = 12
    ctx = multiprocessing.get_context("fork")
    start_event = ctx.Event()
    processes = [
        ctx.Process(
            target=_write_spans_in_child,
            args=(str(tmp_path), "dialogue-multiprocess", worker,
                  spans_per_process, start_event),
        )
        for worker in range(process_count)
    ]
    for process in processes:
        process.start()
    start_event.set()
    for process in processes:
        process.join(timeout=15)
        assert process.exitcode == 0

    path = tmp_path / "python_spans_dialogue-multiprocess.ndjson"
    spans = [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
    ]
    assert len(spans) == process_count * spans_per_process


def test_span_writer_failure_is_isolated_and_preserves_business_error(
    monkeypatch, capsys,
) -> None:
    """Trace persistence failure cannot fail success or mask a business exception."""

    class _BrokenWriter:
        def __init__(self) -> None:
            self.spans = []

        def write_span(self, span) -> None:
            self.spans.append(span)
            raise PermissionError("trace directory is read-only")

    writer = _BrokenWriter()
    monkeypatch.setattr(SpanWriter, "current", staticmethod(lambda: writer))

    with emit_span(SpanKind.SKILL, "successful-business") as span:
        span.set_status(SpanStatus.OK)

    with pytest.raises(RuntimeError, match="business failed"):
        with emit_span(SpanKind.SKILL, "failed-business"):
            raise RuntimeError("business failed")

    assert [span.status for span in writer.spans] == [
        SpanStatus.OK,
        SpanStatus.ERROR,
    ]
    assert SpanContext.current().current_span_id() == ""
    stderr = capsys.readouterr().err
    assert stderr.count('"stage":"span_write_failed"') == 2
    assert "PermissionError" in stderr
