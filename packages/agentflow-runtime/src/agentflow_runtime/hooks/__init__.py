"""Harness Agent Hook 机制 — 生命周期事件目录 + 注册/加载/触发。

设计文档：docs/design/features/hooks/hook-system.md。

用法::

    from agentflow_runtime import events
    from agentflow_runtime.hooks import trigger, load_hooks_from_dir

    # runtime 启动时加载 agent 包 hooks
    load_hooks_from_dir("<agent>/harness/hooks")

    # 挂载点触发（可干预事件）
    payload = trigger(events.AGENT_DIALOGUE_END, {"response_text": response_text},
                      can_intervene=True)
"""

from __future__ import annotations

from agentflow_runtime import events, mounts
from agentflow_runtime.context import AgentContext
from agentflow_runtime.events import HookEvent
from agentflow_runtime.hooks.registry import HookRegistry, default_registry
from agentflow_runtime.hooks.loader import load_hooks_from_dir
from agentflow_runtime.hooks.engine import trigger

__all__ = [
    "events",
    "mounts",
    "AgentContext",
    "HookEvent",
    "HookRegistry",
    "default_registry",
    "load_hooks_from_dir",
    "trigger",
]
