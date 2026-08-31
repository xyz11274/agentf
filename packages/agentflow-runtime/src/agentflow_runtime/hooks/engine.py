"""hook_engine.trigger() — 分发 + 可干预回写 + 异常隔离。

设计文档：docs/design/features/hooks/hook-system.md §3.3/§4。

触发语义：
1. 无订阅 → 直接返回原 payload（零开销，行为与现状一致，H-1）。
2. 有订阅 → 按 HOOK_ORDER 依次执行 handler(event, ctx)：
   - handler 返回 None → 不干预；
   - handler 返回 dict → 合并/覆盖 payload（H-4 干预回写）；
   - handler 操作 ctx（AgentContext）→ 注入/修改运行时状态（对话、产物、阻断标志）；
   - handler 抛异常 → logger.warning + 继续下一个 handler（H-3 异常隔离）。
3. 返回最终 payload 供调用点输出；ctx.blocked 由调用点判断是否阻断当前动作。
"""

from __future__ import annotations

import logging
from typing import Any

from agentflow_runtime.context import AgentContext
from agentflow_runtime.events import HookEvent
from agentflow_runtime.hooks.registry import HookRegistry, default_registry
from agentflow_runtime.security import redact_secrets

logger = logging.getLogger(__name__)


def _sanitize_context(context: AgentContext) -> None:
    safe_messages, _ = redact_secrets(context.messages)
    safe_artifacts, _ = redact_secrets(context.artifacts)
    safe_reason, _ = redact_secrets(context.block_reason)
    context.messages[:] = safe_messages
    context.artifacts[:] = safe_artifacts
    context.block_reason = safe_reason


def _sanitize_payload(payload: dict[str, Any]) -> dict[str, Any]:
    safe_payload, _ = redact_secrets(payload)
    payload.clear()
    payload.update(safe_payload)
    return payload


def trigger(
    event_name: str,
    payload: dict[str, Any],
    *,
    can_intervene: bool = False,
    ctx: AgentContext | None = None,
    registry: HookRegistry | None = None,
) -> dict[str, Any]:
    """触发一个生命周期事件，返回最终 payload（可被 handler 改写）。

    参数:
        event_name: 事件名（events 目录常量）。
        payload: 事件负载（调用点构造，handler 可原地修改或返回 dict 合并）。
        can_intervene: 本事件是否允许 handler 干预（默认 False = 观察）。
        ctx: AgentContext 运行时上下文（handler 注入/修改/阻断的载体；
            未传则内部创建空上下文，调用点事后可读 ctx.blocked 判断是否阻断）。
        registry: 订阅表（默认进程级单例）。
    """
    reg = registry if registry is not None else default_registry
    context = ctx if ctx is not None else AgentContext()
    _sanitize_payload(payload)
    _sanitize_context(context)
    handlers = reg.handlers_for(event_name)
    if not handlers:
        return payload

    event = HookEvent(name=event_name, payload=payload, can_intervene=can_intervene)
    for handler in handlers:
        try:
            result = handler(event, context)
            if isinstance(result, dict):
                safe_result, _ = redact_secrets(result)
                event.payload.update(safe_result)
        except Exception as exc:  # noqa: BLE001 — 异常隔离：不阻断主流程
            safe_error, _ = redact_secrets(str(exc))
            logger.warning(
                "hook_engine.handler_error event=%s handler=%s error=%s",
                event_name,
                getattr(handler, "__name__", "?"),
                safe_error,
            )
        finally:
            _sanitize_payload(event.payload)
            _sanitize_context(context)
    return event.payload
