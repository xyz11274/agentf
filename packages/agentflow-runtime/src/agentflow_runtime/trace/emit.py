"""emit_span() / span_event() — Python 侧统一可观测性 API。

设计文档 §6.3：接口契约。
- ``emit_span()``：生命周期 span API（context manager，RAII 配对）
- ``span_event()``：瞬时事件 API（挂父 span 的 events）

与现有 ``observability.py::emit_event`` 的关系：
- emit_event() 内部转调 span_event()（兼容入口，消除 F4 双轨）
- stderr [obs] 行保留为 preview（§6.8）
"""

from __future__ import annotations

import json
import sys
import threading
from contextlib import contextmanager
from typing import Any, Iterator

from .types import SpanStatus, TraceSpan, SpanEvent, now_us, generate_span_id
from .span_context import SpanContext
from .span_writer import SpanWriter


# SpanContext 只保存 trace/span ID；这里暂存活动对象，让 span_event() 能把事件
# 追加到退出时由 SpanWriter 落盘的同一个 Span。
_active_spans: dict[str, TraceSpan] = {}
_active_spans_lock = threading.Lock()


# ── emit_span()：生命周期 API（context manager）──

@contextmanager
def emit_span(
    kind: str,
    name: str,
    tags: dict[str, str] | None = None,
) -> Iterator[TraceSpan]:
    """Context manager：进入=span 开始，退出=span 结束并落盘。

    自动从 SpanContext 取 trace_id + parent_span_id（进程内嵌套靠栈）。
    落盘到 NDJSON 文件（SpanWriter）。

    用法::

        with emit_span(SpanKind.LLM, "coordinator_llm_round") as sp:
            sp.add_event("llm_request_sent")
            # ... LLM 调用 ...
            sp.set_status(SpanStatus.OK)
        # 退出时自动 end + 落盘

    stderr [obs] preview：进入/退出各打一行（开发态速读，§6.8 preview）。
    """
    ctx = SpanContext.current()
    ctx.read_env_if_stale()  # 幂等读环境变量（进程入口）

    trace_id = ctx.trace_id() or _fallback_trace_id()
    # parent = 当前栈顶活跃 span（即"我即将挂的父"），而非 parent_span_id()（那是"当前活跃 span 的父"=栈下一层）。
    # 栈空时退回根 AF_PARENT_SPAN_ID（跨进程父链）。
    parent_span_id = ctx.current_span_id() or ctx.root_parent_span_id()

    span = TraceSpan(
        trace_id=trace_id,
        span_id=generate_span_id(),
        parent_span_id=parent_span_id,
        kind=kind,
        name=name,
        start_us=now_us(),
        status=SpanStatus.RUNNING,
        tags=tags or {},
    )

    ctx.push(trace_id, span.span_id)
    with _active_spans_lock:
        _active_spans[span.span_id] = span
    _emit_stderr_preview("span_start", span)

    try:
        yield span
        if span.status == SpanStatus.RUNNING:
            span.status = SpanStatus.OK
    except Exception as exc:
        span.status = SpanStatus.ERROR
        span.error_msg = str(exc)
        raise
    finally:
        span.end_us = now_us()
        ctx.pop(span.span_id)
        with _active_spans_lock:
            _active_spans.pop(span.span_id, None)
        try:
            SpanWriter.current().write_span(span)
        except Exception as exc:
            _emit_span_write_failure_preview(span, exc)
        _emit_stderr_preview("span_end", span)


# ── span_event()：瞬时事件 API ──

def span_event(
    name: str,
    data: dict[str, Any] | None = None,
    *,
    level: str = "INFO",
) -> None:
    """在当前 span 内打瞬时事件（SpanEvent）。

    自动挂到 SpanContext.current_span_id() 的活跃 span 的 events 列表。
    若无活跃 span（如进程入口未 emit_span 就打 event），退化为 stderr [obs] 行（兼容旧 emit_event）。

    stderr [obs] preview 保留（§6.8）。
    """
    ctx = SpanContext.current()
    ctx.read_env_if_stale()

    attrs = {k: str(v) for k, v in (data or {}).items() if v is not None}
    event = SpanEvent(
        timestamp_us=now_us(),
        name=name,
        attributes=attrs,
    )

    active_span_id = ctx.current_span_id()
    if active_span_id:
        with _active_spans_lock:
            active_span = _active_spans.get(active_span_id)
            if active_span is not None:
                _append_span_event(active_span, event, "span_event")

    _emit_event_stderr_preview(event, level)


# ── TraceSpan.add_event() 辅助（monkey-patch 到 dataclass）──
# TraceSpan 是 dataclass，无法在 class body 外加方法 → 用闭包

def _append_span_event(
    span: TraceSpan, event: SpanEvent, origin: str,
) -> SpanEvent:
    """追加事件；仅合并旧调用点产生的双 API 相邻写入。"""
    if span.events:
        previous = span.events[-1]
        previous_origin = getattr(previous, "_agentflow_origin", "")
        if previous.name == event.name and previous_origin and previous_origin != origin:
            previous.attributes.update(event.attributes)
            return previous
    setattr(event, "_agentflow_origin", origin)
    span.events.append(event)
    return event


def _span_add_event(self: TraceSpan, name: str, attrs: dict[str, str] | None = None) -> SpanEvent:
    ev = SpanEvent(
        timestamp_us=now_us(),
        name=name,
        attributes=attrs or {},
    )
    return _append_span_event(self, ev, "direct")

def _span_set_status(self: TraceSpan, status: str, error_msg: str = "") -> None:
    self.status = status
    if error_msg:
        self.error_msg = error_msg

def _span_set_tag(self: TraceSpan, key: str, value: str) -> None:
    self.tags[key] = value

# monkey-patch（dataclass 不能在 class body 外加方法，但可设实例属性 — 这里用 type 注入）
TraceSpan.add_event = _span_add_event  # type: ignore[attr-defined]
TraceSpan.set_status = _span_set_status  # type: ignore[attr-defined]
TraceSpan.set_tag = _span_set_tag  # type: ignore[attr-defined]


# ── stderr preview（§6.8：[obs] 行保留为 preview，真相源在 NDJSON）──

_emit_lock = threading.Lock()
def _emit_span_write_failure_preview(span: TraceSpan, exc: Exception) -> None:
    """Best-effort warning: trace persistence must never change business flow."""
    try:
        preview = {
            "stage": "span_write_failed",
            "trace_id": span.trace_id,
            "span_id": span.span_id,
            "name": span.name,
            "error_type": type(exc).__name__,
            "error": str(exc)[:500],
        }
        line = json.dumps(preview, ensure_ascii=False, separators=(",", ":"))
        with _emit_lock:
            print(f"[obs] {line}", file=sys.stderr, flush=True)
    except Exception:
        # stderr/serialization may fail together with storage; observability remains optional.
        pass


def _emit_stderr_preview(stage: str, span: TraceSpan) -> None:
    """emit_span 进入/退出时打 stderr [obs] 行（preview，开发态速读）。"""
    preview = {
        "stage": stage,
        "trace_id": span.trace_id,
        "span_id": span.span_id,
        "parent_span_id": span.parent_span_id,
        "kind": span.kind,
        "name": span.name,
        "start_us": span.start_us,
    }
    if stage == "span_end":
        preview["duration_us"] = span.duration_us
        preview["status"] = span.status
        if span.error_msg:
            preview["error"] = span.error_msg[:200]  # preview 可截断，NDJSON 完整
    line = json.dumps(preview, ensure_ascii=False, separators=(",", ":"))
    with _emit_lock:
        print(f"[obs] {line}", file=sys.stderr, flush=True)


def _emit_event_stderr_preview(event: SpanEvent, level: str) -> None:
    """span_event 退化场景的 stderr [obs] 行（preview）。"""
    preview = {
        "event": event.name,
        "level": level,
        "trace_id": current_trace_id_safe(),
        "span_id": current_span_id_safe(),
        "timestamp_us": event.timestamp_us,
        "data": event.attributes,
    }
    line = json.dumps(preview, ensure_ascii=False, separators=(",", ":"))
    with _emit_lock:
        print(f"[obs] {line}", file=sys.stderr, flush=True)


# ── 安全访问器（避免循环 import）──

def current_trace_id_safe() -> str:
    try:
        return SpanContext.current().trace_id()
    except Exception:
        return ""

def current_span_id_safe() -> str:
    try:
        return SpanContext.current().current_span_id()
    except Exception:
        return ""

# 进程级 fallback trace_id：首次生成后缓存复用。
# 未注入 AF_TRACE_ID 时（python_direct 直跑 / 无 daemon 注入），同一进程内
# 所有 span 必须共享同一 trace_id，viewer 才能按 trace_id 聚合 span 树；
# 否则每 span 一个独立 trace_id（每次 now_us() 不同）→ viewer 每 span 一个
# session，数据零散无法展示（F3 不变量 I-1：trace 关联信息不丢失）。
_fallback_trace_id_cache: str | None = None
_fallback_trace_id_lock = threading.Lock()


def _fallback_trace_id() -> str:
    """SpanContext 无 trace_id 时的退化（进程未被注入 AF_TRACE_ID）。

    进程级稳定：首次调用生成 "py_local_<pid>_<ts>" 后缓存复用，
    同进程后续 span 共享同一 trace_id；跨进程仍各不同（天然隔离）。
    """
    global _fallback_trace_id_cache
    with _fallback_trace_id_lock:
        if _fallback_trace_id_cache is None:
            import os
            _fallback_trace_id_cache = f"py_local_{os.getpid()}_{now_us()}"
        return _fallback_trace_id_cache
