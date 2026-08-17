#!/usr/bin/env python3
"""
Observability utilities for agent nodes — span_event 兼容入口。

本模块原是 harness agent 唯一可观测性入口（stderr [obs] JSON 行），现已收敛到
``agentflow_runtime.trace`` 模块（设计文档 §6 落地架构）：

- ``emit_event()`` 是瞬时事件的兼容入口，内部转调 ``trace.span_event()``
- 成对事件（生命周期 span）已迁为 ``trace.emit_span()``（coordinator llm_round/skill_dispatch、
  skill_executor execute/llm_round，见 P2.2/P2.3）
- stderr [obs] 行保留为 preview（§6.8），真相源在 NDJSON（SpanWriter 落盘）

**当前状态**（P2.4/D1）：
- emit_event 保留为瞬时事件入口（11 类瞬时事件 24 处调用点仍走此入口）
- _node_context() 保留：补 dialogue_id/session_id/node_id/task_id 到 payload，保持旧 schema 兼容
- 未来若业务层瞬时事件也改直调 trace.span_event，本模块可仅剩 re-export
"""

from __future__ import annotations

from typing import Any

from agentflow_runtime.trace import (
    span_event as _span_event,
    SpanContext,
)
from agentflow_runtime.common.runtime_context import (
    dialogue_id,
    node_id,
    self_task_id,
    session_id,
)


def _node_context() -> dict[str, str]:
    """Gather node context from env vars (consistent across all nodes).

    瞬时事件 payload 仍需 node_context 补 dialogue_id/session_id/node_id/task_id，
    保持与旧调用方 payload schema 兼容（span_event 内部不自动补这些字段）。
    """
    return {
        "dialogue_id": dialogue_id(),
        "session_id": session_id(),
        "node_id": node_id(),
        "task_id": self_task_id(),
    }


def emit_event(
    event: str,
    data: dict[str, Any] | None = None,
    *,
    level: str = "INFO",
    duration_ms: float | None = None,
) -> None:
    """Emit a structured observability event — 瞬时事件入口，转调 trace.span_event()。

    内部调 span_event() 挂活跃 span 的 events（无活跃 span 时退化 stderr preview）。
    补 _node_context() 字段到 data，保持与旧调用方 payload schema 一致。

    Args:
        event: Event name — 引用 ``agentflow_runtime.events`` 领域目录常量，
            禁止散落字符串（H-5）。示例：``hook_events.COORDINATOR_TRIGGER``。
        data: Event-specific key-value payload
        level: Log level (INFO, WARN, ERROR)
        duration_ms: Optional duration（秒/毫秒，由调用方负责校正，§5.2.4 H3）
    """
    # 幂等读环境变量建立根父链（如进程入口未显式调 SpanContext.read_env()）
    SpanContext.current().read_env_if_stale()

    # 合并 node_context 到 data（保持旧 payload schema 兼容）
    merged: dict[str, Any] = data or {}
    merged.update(_node_context())
    if duration_ms is not None:
        merged["duration_ms"] = round(duration_ms, 1)
    merged["level"] = level

    # 转调统一 trace 模块（span_event 内部打 stderr [obs] preview + 挂活跃 span events）
    _span_event(event, merged, level=level)
