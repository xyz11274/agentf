"""agentflow trace module — Python side of TraceSystem span tree.

镜像 C++ ``include/internal/runtime/trace/`` 模块，为 harness agent Python 链路
（coordinator/skill_executor 等）提供统一可观测性 API：

- ``SpanContext``：进程内 span 栈（仿 C++ TraceContext），读 AF_TRACE_ID/AF_PARENT_SPAN_ID
- ``emit_span()``：生命周期 span API（context manager，RAII 配对）
- ``span_event()``：瞬时事件 API（挂父 span 的 events）
- ``SpanWriter``：落盘 NDJSON（与 C++ trace_output/ 合并检索）

设计文档：``docs/design/architecture/observability-design.md`` §6 落地架构。
"""

from .types import SpanKind, SpanStatus, TraceSpan, SpanEvent
from .span_context import SpanContext, current_trace_id, current_span_id, parent_span_id
from .span_writer import SpanWriter
from .emit import emit_span, span_event

__all__ = [
    "SpanKind",
    "SpanStatus",
    "TraceSpan",
    "SpanEvent",
    "SpanContext",
    "SpanWriter",
    "emit_span",
    "span_event",
    "current_trace_id",
    "current_span_id",
    "parent_span_id",
]
