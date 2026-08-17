"""HookRegistry — 订阅表 + HOOK_ORDER 排序 + 幂等注册。

设计文档：docs/design/features/hooks/hook-system.md §3.2/§3.3。
"""

from __future__ import annotations

from collections import defaultdict
from typing import Callable

# handler 签名：handle(event: HookEvent, ctx: AgentContext) -> None | dict
HookHandler = Callable[[object, object], object | None]


class HookRegistry:
    """事件订阅表。

    - register(hook_name, events, handler, order)：同名 hook 重复注册覆盖旧 handler（幂等）。
    - handlers_for(event_name)：按 HOOK_ORDER 小→大返回 handler 列表。
    - 进程级单例：``default_registry``（模块底部）。
    """

    def __init__(self) -> None:
        # event_name -> [(order, hook_name, handler), ...]
        self._subscriptions: dict[str, list[tuple[int, str, HookHandler]]] = defaultdict(list)
        # 已加载 hook 名（loader 幂等依据，H-2）
        self._loaded_hooks: set[str] = set()

    def register(
        self,
        hook_name: str,
        events: list[str],
        handler: HookHandler,
        order: int = 100,
    ) -> None:
        """注册 handler 到多个事件。

        同名 hook 重复注册时，先移除旧订阅再注册（H-2 幂等：不重复执行）。
        """
        for ev_name in list(self._subscriptions.keys()):
            self._subscriptions[ev_name] = [
                (o, n, h) for (o, n, h) in self._subscriptions[ev_name] if n != hook_name
            ]
        for ev_name in events:
            self._subscriptions[ev_name].append((order, hook_name, handler))

    def handlers_for(self, event_name: str) -> list[HookHandler]:
        """按 HOOK_ORDER 小→大返回该事件的 handler 列表（无订阅返回空）。"""
        subs = sorted(self._subscriptions.get(event_name, []), key=lambda t: t[0])
        return [h for (_, _, h) in subs]

    def is_loaded(self, hook_name: str) -> bool:
        return hook_name in self._loaded_hooks

    def mark_loaded(self, hook_name: str) -> None:
        self._loaded_hooks.add(hook_name)

    def clear(self) -> None:
        self._subscriptions.clear()
        self._loaded_hooks.clear()


# 进程级单例（与 C++ 侧观察性惯例对齐；测试可自行创建独立实例隔离）
default_registry = HookRegistry()
