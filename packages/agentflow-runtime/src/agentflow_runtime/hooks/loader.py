"""hooks_loader — 扫描 agent 包 harness/hooks/*.py 动态加载。

设计文档：docs/design/features/hooks/hook-system.md §3.2。

Hook 定义规范（用户 2026-08-14 确认）：
- ``HOOK_NAME``：必填，非空字符串（幂等注册依据）。
- ``HOOK_EVENTS``：必填，list[str]；每项必须是合法挂载点（ALL_HOOK_MOUNTS）
  或可观测性事件（ALL_EVENTS），非法名 → 告警跳过该 hook（防手写错名静默失效）。
  多个 hook 可监听同一事件，按 HOOK_ORDER 链式执行。
- ``handle(event, ctx)``：必填，可接受双参 (HookEvent, AgentContext)；单参/不兼容
  签名 → 告警跳过该 hook（防运行时 TypeError 被异常隔离吞掉、hook 静默失效）。
- ``HOOK_ORDER``：可选，同事件 handler 执行顺序（小→大，默认 100）。

其余约定（模仿 tools 加载机制 _load_dir）：
- 跳过 ``_`` 前缀文件与非 .py 文件。
- 加载失败（import error）→ 告警跳过。
- 幂等：同一 hook 名只注册一次（H-2，依据 registry.is_loaded/mark_loaded）。
- 单个 hook 校验失败不阻断启动（M3：异常不影响后续流程）。
"""

from __future__ import annotations

import importlib.util
import inspect
import logging
import os
from typing import Any

from agentflow_runtime.events import ALL_EVENTS
from agentflow_runtime.mounts import ALL_HOOK_MOUNTS
from agentflow_runtime.hooks.registry import HookRegistry, default_registry, HookHandler

logger = logging.getLogger(__name__)

# 合法监听事件集合：挂载点 ∪ 可观测性事件
_VALID_EVENTS = ALL_HOOK_MOUNTS | ALL_EVENTS


def _load_module_from_file(module_name: str, path: str) -> Any | None:
    """从文件路径加载 Python 模块；失败返回 None（不抛）。"""
    try:
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None or spec.loader is None:
            return None
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod
    except Exception as exc:  # noqa: BLE001 — loader 不得因单个 hook 失败阻断启动
        logger.warning("hook_loader.skip module=%s error=%s", module_name, exc)
        return None


def _validate_handler_signature(handle: Any) -> bool:
    """校验 handle 可接受双参 ``(event, ctx)``。

    允许：显式双参 ``def handle(event, ctx)``；带 *args/**kwargs 的可变签名；
    或三参以上但前两参为位置参数。单参（event）→ 运行时 TypeError 会被 engine
    异常隔离吞掉造成静默失效，必须加载期拦截。
    """
    try:
        sig = inspect.signature(handle)
    except (TypeError, ValueError):
        return False  # 无法解析签名（如 builtin）→ 视为不兼容，保守跳过
    positional = [
        p for p in sig.parameters.values()
        if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)
    ]
    has_varargs = any(
        p.kind == p.VAR_POSITIONAL for p in sig.parameters.values()
    )
    has_kwargs = any(
        p.kind == p.VAR_KEYWORD for p in sig.parameters.values()
    )
    # 前两参必须是位置可接收（或 *args 兜底）
    if has_varargs:
        return True
    return len(positional) >= 2 or (len(positional) >= 1 and has_kwargs)


def _validate_events(events: Any, module_name: str) -> list[str] | None:
    """校验 HOOK_EVENTS：list[str] 且每项 ∈ 合法事件集合。返回清洗后的列表或 None。"""
    if not isinstance(events, (list, tuple, set)) or not events:
        logger.warning(
            "hook_loader.skip_invalid_events module=%s (HOOK_EVENTS must be non-empty list)",
            module_name,
        )
        return None
    cleaned: list[str] = []
    for ev_name in events:
        if not isinstance(ev_name, str) or ev_name not in _VALID_EVENTS:
            logger.warning(
                "hook_loader.skip_invalid_event module=%s event=%r (not in ALL_HOOK_MOUNTS/ALL_EVENTS)",
                module_name, ev_name,
            )
            return None
        cleaned.append(ev_name)
    return cleaned


def load_hooks_from_dir(
    hooks_dir: str,
    registry: HookRegistry | None = None,
) -> list[str]:
    """加载 ``<hooks_dir>/*.py`` 中的 hook 模块，注册到 registry。

    返回成功加载的 hook 名列表。目录不存在 / 为空 → 返回空列表（无 hook 环境零开销）。
    """
    reg = registry if registry is not None else default_registry
    loaded: list[str] = []

    if not hooks_dir or not os.path.isdir(hooks_dir):
        return loaded

    for entry in sorted(os.listdir(hooks_dir)):
        if not entry.endswith(".py") or entry.startswith("_"):
            continue
        mod_name = entry[:-3]
        mod = _load_module_from_file(f"_hook_{mod_name}", os.path.join(hooks_dir, entry))
        if mod is None:
            continue

        hook_name = getattr(mod, "HOOK_NAME", "")
        handle = getattr(mod, "handle", None)

        # ── 规范校验 1：HOOK_NAME 必填非空 ──
        if not hook_name or not isinstance(hook_name, str):
            logger.warning(
                "hook_loader.skip_invalid_name module=%s (HOOK_NAME required non-empty str)",
                mod_name,
            )
            continue
        # ── 规范校验 2：HOOK_EVENTS 非空且全为合法事件 ──
        events = _validate_events(getattr(mod, "HOOK_EVENTS", None), mod_name)
        if events is None:
            continue
        # ── 规范校验 3：handle 可调用且接受双参 ──
        if not callable(handle) or not _validate_handler_signature(handle):
            logger.warning(
                "hook_loader.skip_invalid_handler module=%s (handle(event, ctx) required)",
                mod_name,
            )
            continue

        if reg.is_loaded(hook_name):
            logger.info("hook_loader.skip_duplicate hook=%s", hook_name)
            continue

        order = int(getattr(mod, "HOOK_ORDER", 100))
        reg.register(hook_name, events, handle, order=order)
        reg.mark_loaded(hook_name)
        loaded.append(hook_name)
        logger.info("hook_loader.loaded hook=%s events=%s order=%s", hook_name, events, order)

    return loaded
