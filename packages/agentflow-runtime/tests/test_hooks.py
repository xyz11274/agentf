"""Hook 机制测试 — 生命周期事件目录 + 注册/加载/触发骨架。

设计文档：docs/design/features/hooks/hook-system.md
不变量（§4）：H-1 零干预零回归 / H-2 幂等加载 / H-3 异常隔离 / H-4 干预回写 / H-5 事件名唯一。
"""

from __future__ import annotations

import importlib.util
import sys
import types
from unittest.mock import MagicMock

import pytest

from agentflow_runtime.hooks.registry import HookRegistry
from agentflow_runtime.hooks.engine import trigger
from agentflow_runtime.hooks.loader import load_hooks_from_dir


# ═══════════════════════════════════════════════════════════════════
# 事件目录（H-5 事件名唯一 + 可干预集）
# ═══════════════════════════════════════════════════════════════════

def test_event_catalog_unique() -> None:
    """事件目录内事件名不重复，且与设计文档 §2.5 全量清单一致（27 项）。"""
    from agentflow_runtime import events as ev

    assert len(ev.ALL_EVENTS) == 27
    assert len(set(ev.ALL_EVENTS)) == 27  # 唯一


def test_span_names_unique() -> None:
    """emit_span 生命周期单元名目录唯一（5 项）。"""
    from agentflow_runtime import events as ev

    assert len(ev.ALL_SPAN_NAMES) == 5
    assert len(set(ev.ALL_SPAN_NAMES)) == 5
    # span 名与事件名不冲突（各自命名空间）
    assert ev.ALL_SPAN_NAMES.isdisjoint(ev.ALL_EVENTS)


def test_intervenable_event_subset() -> None:
    """可干预事件 = 设计确认的 5 个候选。"""
    from agentflow_runtime import events as ev

    assert ev.INTERVENABLE_EVENTS == frozenset({
        ev.AGENT_DIALOGUE_END,
        ev.COORDINATOR_FINAL_RESPONSE,
        ev.COORDINATOR_SKILLS_DISPATCHED,
        ev.SKILL_EXECUTOR_TOOL_CALL,
        ev.SKILL_EXECUTOR_COMPLETED,
    })
    # 可干预事件必须是目录内真实事件
    assert ev.INTERVENABLE_EVENTS <= ev.ALL_EVENTS


def test_existing_emit_event_names_preserved() -> None:
    """现有 emit_event 事件名全部保留（Q2 决策：不改名只集中定义）。"""
    from agentflow_runtime import events as ev

    existing = [
        "coordinator.trigger", "coordinator.llm_result", "coordinator.llm_error",
        "coordinator.skills_dispatched", "coordinator.dispatch_loop_end",
        "coordinator.final_response", "coordinator.max_iterations",
        "coordinator.max_rounds_forced_output", "coordinator.duplicate_skill_dispatch_blocked",
        "coordinator.tool_loop_max_rounds", "coordinator.tool_loop_retry",
        "coordinator.dispatch_progress_push_failed", "coordinator.interrupted",
        "skill_executor.trigger", "skill_executor.llm_round_start", "skill_executor.llm_result",
        "skill_executor.llm_error", "skill_executor.tool_call",
        "skill_executor.tool_call_loop_detected", "skill_executor.completed",
        "skill_executor.node_timeout", "skill_executor.max_rounds_reached",
        "skill_executor.empty_result", "skill_executor.interrupted",
    ]
    for name in existing:
        assert name in ev.ALL_EVENTS, f"现有事件 {name} 必须保留在目录中"


# ═══════════════════════════════════════════════════════════════════
# HookEvent payload schema
# ═══════════════════════════════════════════════════════════════════

def test_hook_event_defaults() -> None:
    """HookEvent 默认值：can_intervene=False，occurred_at 自动打点。"""
    from agentflow_runtime.events import HookEvent

    e = HookEvent(name="agent.dialogue.start", payload={"k": "v"})
    assert e.can_intervene is False
    assert e.occurred_at > 0
    assert e.payload == {"k": "v"}


# ═══════════════════════════════════════════════════════════════════
# H-1 零干预零回归
# ═══════════════════════════════════════════════════════════════════

def test_trigger_no_subscription_returns_payload_unchanged() -> None:
    """无订阅时 payload 原样通过（零开销，行为与现状一致）。"""
    reg = HookRegistry()
    payload = {"response_text": "你好", "round": 1}
    result = trigger("agent.dialogue.end", payload, registry=reg)
    assert result == payload
    assert result["response_text"] == "你好"


# ═══════════════════════════════════════════════════════════════════
# H-4 干预回写
# ═══════════════════════════════════════════════════════════════════

def test_trigger_handler_returning_none_keeps_payload() -> None:
    """handler 返回 None = 不干预，payload 原样。"""
    reg = HookRegistry()
    reg.register("noop", ["agent.dialogue.end"], lambda ev, ctx: None)
    payload = {"response_text": "原文"}
    assert trigger("agent.dialogue.end", payload, registry=reg) == {"response_text": "原文"}


def test_trigger_handler_returning_dict_merges_payload() -> None:
    """handler 返回 dict = 合并/覆盖 payload 字段（H-4 干预回写）。"""
    reg = HookRegistry()

    def rewrite(ev, ctx):
        ev.payload["response_text"] = "（已被拦截）"
        return {"extra": "hook 标记"}

    reg.register("guard", ["agent.dialogue.end"], rewrite)
    payload = {"response_text": "原始回答", "round": 2}
    result = trigger("agent.dialogue.end", payload, registry=reg)
    assert result["response_text"] == "（已被拦截）"
    assert result["extra"] == "hook 标记"
    assert result["round"] == 2


# ═══════════════════════════════════════════════════════════════════
# H-3 异常隔离
# ═══════════════════════════════════════════════════════════════════

def test_trigger_handler_exception_does_not_block() -> None:
    """handler 抛异常不阻断主流程：后续 handler 继续执行，最终 payload 正常返回。"""
    reg = HookRegistry()

    def boom(ev, ctx):
        raise RuntimeError("hook 内部错误")

    def after(ev, ctx):
        return {"status": "ok"}

    reg.register("bad", ["agent.dialogue.end"], boom)
    reg.register("good", ["agent.dialogue.end"], after)
    result = trigger("agent.dialogue.end", {"response_text": "x"}, registry=reg)
    assert result["status"] == "ok"
    assert result["response_text"] == "x"


# ═══════════════════════════════════════════════════════════════════
# 注册语义：HOOK_ORDER 排序 + 幂等
# ═══════════════════════════════════════════════════════════════════

def test_handlers_execute_in_order() -> None:
    """同事件 handler 按 HOOK_ORDER 小→大执行。"""
    reg = HookRegistry()
    calls: list[str] = []

    reg.register("first", ["skill_executor.before_skill_complete"], lambda ev, ctx: calls.append("first") or None, order=10)
    reg.register("second", ["skill_executor.before_skill_complete"], lambda ev, ctx: calls.append("second") or None, order=20)
    trigger("skill_executor.before_skill_complete", {}, registry=reg)
    assert calls == ["first", "second"]


def test_register_same_hook_name_is_idempotent() -> None:
    """同名 hook 重复注册覆盖旧 handler（H-2 幂等：不重复执行）。"""
    reg = HookRegistry()
    calls: list[str] = []
    reg.register("h", ["agent.dialogue.end"], lambda ev, ctx: calls.append("v1") or None)
    reg.register("h", ["agent.dialogue.end"], lambda ev, ctx: calls.append("v2") or None)
    trigger("agent.dialogue.end", {}, registry=reg)
    assert calls == ["v2"]


def test_registry_clear() -> None:
    """clear() 清空订阅。"""
    reg = HookRegistry()
    reg.register("h", ["agent.dialogue.end"], lambda ev, ctx: None)
    reg.clear()
    assert trigger("agent.dialogue.end", {"a": 1}, registry=reg) == {"a": 1}


# ═══════════════════════════════════════════════════════════════════
# loader：扫描 harness/hooks/*.py
# ═══════════════════════════════════════════════════════════════════

def _write_hook(tmp_path, filename: str, body: str) -> str:
    path = tmp_path / filename
    path.write_text(body, encoding="utf-8")
    return str(tmp_path)


def test_loader_loads_valid_hook(tmp_path) -> None:
    """合法 hook 模块（HOOK_NAME/HOOK_EVENTS/handle）被加载并注册。"""
    hook_dir = _write_hook(tmp_path, "guardrails.py", '''
HOOK_NAME = "guardrails"
HOOK_EVENTS = ["agent.dialogue.end", "skill_executor.before_skill_complete"]
HOOK_ORDER = 10

def handle(event, ctx):
    event.payload["response_text"] = "改写"
''')
    reg = HookRegistry()
    loaded = load_hooks_from_dir(hook_dir, reg)
    assert loaded == ["guardrails"]
    assert len(reg.handlers_for("agent.dialogue.end")) == 1
    assert len(reg.handlers_for("skill_executor.before_skill_complete")) == 1
    result = trigger("agent.dialogue.end", {"response_text": "原始"}, registry=reg)
    assert result["response_text"] == "改写"


def test_loader_skips_underscore_and_non_py(tmp_path) -> None:
    """跳过 _ 前缀文件与非 .py 文件。"""
    hook_dir = _write_hook(tmp_path, "_private.py", "HOOK_NAME='x'\nHOOK_EVENTS=[]\ndef handle(ev, ctx): pass\n")
    (tmp_path / "README.md").write_text("not a hook", encoding="utf-8")
    reg = HookRegistry()
    loaded = load_hooks_from_dir(hook_dir, reg)
    assert loaded == []


def test_loader_skips_invalid_module_without_blocking(tmp_path) -> None:
    """缺导出/import 失败的模块被跳过并告警，不阻断其他 hook 加载。"""
    hook_dir = _write_hook(tmp_path, "bad1.py", "raise ImportError('broken')\n")
    hook_dir = _write_hook(tmp_path, "bad2.py", 'HOOK_NAME = "bad2"\nHOOK_EVENTS = []  # 缺 handle\n')
    hook_dir = _write_hook(tmp_path, "good.py", '''
HOOK_NAME = "good"
HOOK_EVENTS = ["coordinator.before_final_response"]
def handle(event, ctx):
    return {"hooked": True}
''')
    reg = HookRegistry()
    loaded = load_hooks_from_dir(hook_dir, reg)
    assert loaded == ["good"]
    assert trigger("coordinator.before_final_response", {"a": 1}, registry=reg)["hooked"] is True


def test_loader_double_load_is_idempotent(tmp_path) -> None:
    """同一 hooks 目录加载两次，只注册一次（H-2）。"""
    hook_dir = _write_hook(tmp_path, "h.py", '''
HOOK_NAME = "h"
HOOK_EVENTS = ["agent.dialogue.end"]
def handle(event, ctx):
    event.payload["n"] = event.payload.get("n", 0) + 1
''')
    reg = HookRegistry()
    load_hooks_from_dir(hook_dir, reg)
    load_hooks_from_dir(hook_dir, reg)
    calls = trigger("agent.dialogue.end", {"n": 0}, registry=reg)
    assert calls["n"] == 1  # 只执行一次


def test_loader_orders_handlers(tmp_path) -> None:
    """多个 hook 按 HOOK_ORDER 排序执行。"""
    hook_dir = _write_hook(tmp_path, "a.py", '''
HOOK_NAME = "a"
HOOK_EVENTS = ["skill_executor.before_tool_call"]
HOOK_ORDER = 30
def handle(event, ctx):
    event.payload.setdefault("seq", []).append("a")
''')
    _write_hook(tmp_path, "b.py", '''
HOOK_NAME = "b"
HOOK_EVENTS = ["skill_executor.before_tool_call"]
HOOK_ORDER = 10
def handle(event, ctx):
    event.payload.setdefault("seq", []).append("b")
''')
    reg = HookRegistry()
    load_hooks_from_dir(hook_dir, reg)
    result = trigger("skill_executor.before_tool_call", {"seq": []}, registry=reg)
    assert result["seq"] == ["b", "a"]


# ═══════════════════════════════════════════════════════════════════
# 默认 registry（进程级单例）与 __init__ 导出
# ═══════════════════════════════════════════════════════════════════

def test_default_registry_is_process_singleton() -> None:
    """hooks/__init__.py 导出 default_registry 单例与 trigger。"""
    from agentflow_runtime.hooks import default_registry, trigger as module_trigger

    assert isinstance(default_registry, HookRegistry)
    assert module_trigger is trigger


def test_default_registry_isolated_between_tests() -> None:
    """默认 registry 在测试间隔离（不残留订阅）。"""
    from agentflow_runtime.hooks import default_registry

    payload = {"response_text": "clean"}
    result = trigger("agent.dialogue.end", payload, registry=default_registry)
    assert result == payload


# ═══════════════════════════════════════════════════════════════════
# AgentContext — 注入 / 修改 / 阻断（§2.4.1）
# ═══════════════════════════════════════════════════════════════════

def test_ctx_inject_message() -> None:
    """handler 可向 ctx.messages 注入消息（注入语义）。"""
    from agentflow_runtime.context import AgentContext

    reg = HookRegistry()

    def injector(event, ctx):
        ctx.messages.append({"role": "system", "content": "hook 注入的约束"})

    reg.register("injector", ["skill_executor.before_llm_call"], injector)
    ctx = AgentContext(messages=[{"role": "user", "content": "原始问题"}])
    trigger("skill_executor.before_llm_call", {}, ctx=ctx, registry=reg)
    assert len(ctx.messages) == 2
    assert ctx.messages[-1]["content"] == "hook 注入的约束"


def test_ctx_modify_messages() -> None:
    """handler 可改写 ctx.messages 内容（修改语义）。"""
    from agentflow_runtime.context import AgentContext

    reg = HookRegistry()

    def modifier(event, ctx):
        for m in ctx.messages:
            if m.get("role") == "system":
                m["content"] = m["content"] + "（hook 补充）"

    reg.register("modifier", ["coordinator.before_final_response"], modifier)
    ctx = AgentContext(messages=[{"role": "system", "content": "原始 system"}])
    trigger("coordinator.before_final_response", {"response_text": "x"}, ctx=ctx, registry=reg)
    assert ctx.messages[0]["content"] == "原始 system（hook 补充）"


def test_ctx_append_artifact() -> None:
    """handler 可向 ctx.artifacts 追加产物（注入语义）。"""
    from agentflow_runtime.context import AgentContext

    reg = HookRegistry()

    def appender(event, ctx):
        ctx.artifacts.append("outputs/result.png")

    reg.register("appender", ["skill_executor.before_skill_complete"], appender)
    ctx = AgentContext(artifacts=["outputs/a.txt"])
    trigger("skill_executor.before_skill_complete", {"skill_result": "ok"}, ctx=ctx, registry=reg)
    assert ctx.artifacts == ["outputs/a.txt", "outputs/result.png"]


def test_ctx_block() -> None:
    """handler 置 ctx.blocked 阻断当前动作（阻断语义）。"""
    from agentflow_runtime.context import AgentContext

    reg = HookRegistry()

    def blocker(event, ctx):
        ctx.blocked = True
        ctx.block_reason = "guardrails: 敏感内容"

    reg.register("blocker", ["coordinator.before_skill_dispatch"], blocker)
    ctx = AgentContext()
    trigger("coordinator.before_skill_dispatch", {"skill_names": ["echo"]}, ctx=ctx, registry=reg)
    assert ctx.blocked is True
    assert ctx.block_reason == "guardrails: 敏感内容"


def test_ctx_passed_to_loader_hook(tmp_path) -> None:
    """loader 加载的 hook 也能收到 ctx（双参签名完整链路）。"""
    from agentflow_runtime.context import AgentContext

    hook_dir = _write_hook(tmp_path, "ctx_hook.py", '''
HOOK_NAME = "ctx_hook"
HOOK_EVENTS = ["skill_executor.before_skill_complete"]
def handle(event, ctx):
    ctx.artifacts.append("outputs/hooked.txt")
    ctx.blocked = True
''')
    reg = HookRegistry()
    load_hooks_from_dir(hook_dir, reg)
    ctx = AgentContext(artifacts=[])
    trigger("skill_executor.before_skill_complete", {"skill_result": "ok"}, ctx=ctx, registry=reg)
    assert ctx.artifacts == ["outputs/hooked.txt"]
    assert ctx.blocked is True


# ═══════════════════════════════════════════════════════════════════
# Hook 挂载点目录（§2.7 before_*/after_* 动作边界语义）
# ═══════════════════════════════════════════════════════════════════

def test_hook_mounts_catalog_unique() -> None:
    """挂载点目录 16 项唯一，且 before/after 集合不重叠。

    coordinator v1/v2 事件拆分后（coordinator_v1.* / coordinator_v2.* 各一对
    before/after LLM 挂载点），目录从 14 项增至 16 项（2026-08-16 用户确认）。
    """
    from agentflow_runtime import mounts as m

    assert len(m.ALL_HOOK_MOUNTS) == 16
    assert len(set(m.ALL_HOOK_MOUNTS)) == 16
    assert m.BEFORE_MOUNTS.isdisjoint(m.AFTER_MOUNTS)
    assert m.BEFORE_MOUNTS | m.AFTER_MOUNTS == m.ALL_HOOK_MOUNTS


def test_hook_mounts_before_after_paired() -> None:
    """每个动作都有成对 before/after 挂载点（Q2 决策：全部成对）。

    coordinator v1（coordinator_decide）与 v2（coordinator_agent_loop）是两套差别较大
    的体系（v1=决策循环+skill 委派；v2=agent_loop 直接执行工具），事件按版本拆分
    （coordinator_v1.* / coordinator_v2.*），各自成对（2026-08-16 用户确认）。
    """
    from agentflow_runtime import mounts as m

    # 7 对动作挂载点 + 回合级 start/end（回合级本身是入口/出口，不成对 but 是生命周期的开始/结束）
    assert m.SKILL_EXECUTOR_BEFORE_TOOL_CALL in m.BEFORE_MOUNTS
    assert m.SKILL_EXECUTOR_AFTER_TOOL_CALL in m.AFTER_MOUNTS
    assert m.SKILL_EXECUTOR_BEFORE_LLM_CALL in m.BEFORE_MOUNTS
    assert m.SKILL_EXECUTOR_AFTER_LLM_CALL in m.AFTER_MOUNTS
    assert m.SKILL_EXECUTOR_BEFORE_SKILL_COMPLETE in m.BEFORE_MOUNTS
    assert m.SKILL_EXECUTOR_AFTER_SKILL_COMPLETE in m.AFTER_MOUNTS
    # coordinator v1/v2 LLM 调用挂载点各自成对
    assert m.COORDINATOR_V1_BEFORE_LLM_CALL in m.BEFORE_MOUNTS
    assert m.COORDINATOR_V1_AFTER_LLM_CALL in m.AFTER_MOUNTS
    assert m.COORDINATOR_V2_BEFORE_LLM_CALL in m.BEFORE_MOUNTS
    assert m.COORDINATOR_V2_AFTER_LLM_CALL in m.AFTER_MOUNTS
    # v1/v2 事件名必须不同（两套体系区分开，不得共用）
    assert m.COORDINATOR_V1_BEFORE_LLM_CALL != m.COORDINATOR_V2_BEFORE_LLM_CALL
    assert m.COORDINATOR_V1_AFTER_LLM_CALL != m.COORDINATOR_V2_AFTER_LLM_CALL
    assert m.COORDINATOR_BEFORE_SKILL_DISPATCH in m.BEFORE_MOUNTS
    assert m.COORDINATOR_AFTER_SKILL_DISPATCH in m.AFTER_MOUNTS
    assert m.COORDINATOR_BEFORE_FINAL_RESPONSE in m.BEFORE_MOUNTS
    assert m.COORDINATOR_AFTER_FINAL_RESPONSE in m.AFTER_MOUNTS


def test_hook_mounts_disjoint_from_events() -> None:
    """挂载点与可观测性事件并存、命名空间不重叠（Q3 决策：并存职责分离）。"""
    from agentflow_runtime import events as ev
    from agentflow_runtime import mounts as m

    # 动作挂载点与可观测性事件不重名（回合级 start/end 两边同名属预期）
    action_mounts = m.ALL_HOOK_MOUNTS - {m.AGENT_DIALOGUE_START, m.AGENT_DIALOGUE_END}
    assert action_mounts.isdisjoint(ev.ALL_EVENTS)


def test_before_tool_call_can_block_and_inject() -> None:
    """before_tool_call 挂载点：注入 ctx + 阻断工具调用（用户点名的关键挂载点）。"""
    from agentflow_runtime.context import AgentContext

    reg = HookRegistry()

    def guard(event, ctx):
        ctx.artifacts.append("outputs/audit.log")
        if "dangerous" in str(event.payload.get("tool_names", [])):
            ctx.blocked = True
            ctx.block_reason = "tool 被 guardrails 拦截"

    reg.register("guard", ["skill_executor.before_tool_call"], guard)
    ctx = AgentContext(artifacts=[])
    trigger("skill_executor.before_tool_call", {"tool_names": ["dangerous_tool"]},
            ctx=ctx, can_intervene=True, registry=reg)
    assert ctx.blocked is True
    assert ctx.block_reason == "tool 被 guardrails 拦截"
    assert "outputs/audit.log" in ctx.artifacts


# ═══════════════════════════════════════════════════════════════════
# loader 规范校验（§3.2：HOOK_NAME 必填 / HOOK_EVENTS 合法 / handle 双参）
# ═══════════════════════════════════════════════════════════════════

def test_loader_rejects_invalid_event_name(tmp_path) -> None:
    """HOOK_EVENTS 含非法事件名 → 整个 hook 跳过（防手写错名静默失效）。"""
    hook_dir = _write_hook(tmp_path, "bad_event.py", '''
HOOK_NAME = "bad_event"
HOOK_EVENTS = ["skill_executor.before_tool_cal"]  # 拼写错误，非合法挂载点/事件
def handle(event, ctx):
    return None
''')
    reg = HookRegistry()
    loaded = load_hooks_from_dir(hook_dir, reg)
    assert loaded == []
    assert reg.handlers_for("skill_executor.before_tool_cal") == []


def test_loader_rejects_single_param_handler(tmp_path) -> None:
    """handle 只接受单参 → 跳过（防运行时 TypeError 被异常隔离吞掉）。"""
    hook_dir = _write_hook(tmp_path, "single_param.py", '''
HOOK_NAME = "single_param"
HOOK_EVENTS = ["skill_executor.before_llm_call"]
def handle(event):
    return None
''')
    reg = HookRegistry()
    loaded = load_hooks_from_dir(hook_dir, reg)
    assert loaded == []


def test_loader_accepts_varargs_handler(tmp_path) -> None:
    """handle 用 *args/**kwargs 可变签名 → 接受（可兼容双参调用）。"""
    hook_dir = _write_hook(tmp_path, "varargs.py", '''
HOOK_NAME = "varargs"
HOOK_EVENTS = ["skill_executor.before_llm_call"]
def handle(event, *args, **kwargs):
    return None
''')
    reg = HookRegistry()
    loaded = load_hooks_from_dir(hook_dir, reg)
    assert loaded == ["varargs"]


def test_loader_multiple_hooks_same_event_chain(tmp_path) -> None:
    """多个 hook 挂同一事件 → 按 HOOK_ORDER 链式执行（Q1 决策）。"""
    hook_dir = _write_hook(tmp_path, "a.py", '''
HOOK_NAME = "a"
HOOK_EVENTS = ["skill_executor.before_llm_call"]
HOOK_ORDER = 20
def handle(event, ctx):
    event.payload.setdefault("seq", []).append("a")
''')
    _write_hook(tmp_path, "b.py", '''
HOOK_NAME = "b"
HOOK_EVENTS = ["skill_executor.before_llm_call"]
HOOK_ORDER = 10
def handle(event, ctx):
    event.payload.setdefault("seq", []).append("b")
''')
    reg = HookRegistry()
    loaded = load_hooks_from_dir(hook_dir, reg)
    assert sorted(loaded) == ["a", "b"]
    result = trigger("skill_executor.before_llm_call", {"seq": []}, registry=reg)
    assert result["seq"] == ["b", "a"]  # HOOK_ORDER 小→大


def test_outfit_preload_hook_spec_valid() -> None:
    """垂域 hook（outfit-assistant/preload_user_context）符合规范：合法事件 + 双参签名。"""
    import os

    from agentflow_runtime.hooks.loader import _validate_events, _validate_handler_signature

    hooks_dir = os.path.join(
        os.path.dirname(__file__), "..", "..", "..",
        "examples", "agent", "outfit-assistant", "harness", "hooks",
    )
    hook_path = os.path.join(hooks_dir, "preload_user_context.py")
    if not os.path.exists(hook_path):
        pytest.skip("outfit-assistant 垂域 hook 未就位")

    import importlib.util
    spec = importlib.util.spec_from_file_location("_spec_outfit_preload", hook_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    assert mod.HOOK_NAME == "preload_user_context"
    assert mod.HOOK_EVENTS == [
        "skill_executor.before_llm_call",
        "coordinator_v1.before_llm_call",
        "coordinator_v2.before_llm_call",
    ]
    # 事件名合法（挂载点目录中）
    events = _validate_events(mod.HOOK_EVENTS, "preload_user_context")
    assert events == [
        "skill_executor.before_llm_call",
        "coordinator_v1.before_llm_call",
        "coordinator_v2.before_llm_call",
    ]
    # handler 双参签名合法
    assert _validate_handler_signature(mod.handle) is True


# ═══════════════════════════════════════════════════════════════════
# preload_user_context hook 注入内容（upload_images / env / 最近天气）
# ═══════════════════════════════════════════════════════════════════
# 设计（2026-08-16 用户确认）：hook 对齐工具版全字段——upload_images（透传 executor
# 已翻译路径）+ env（DASHSCOPE_API_KEY 可用性 + forge-image-gen/video-gen 挂载）+
# 最近天气（读 history 最近卡片"地点天气"字段）。SKILL 不感知 hook（SKILL.md 不加
# "已由 hook 注入"字样）——本测试只验证注入到 system prompt 的数据本身。

import importlib.util as _ilu  # noqa: E402


def _load_outfit_preload_hook():
    """加载 outfit-assistant 垂域 hook 模块（复用 spec 校验的加载方式）。"""
    import os
    hooks_dir = os.path.join(
        os.path.dirname(__file__), "..", "..", "..",
        "examples", "agent", "outfit-assistant", "harness", "hooks",
    )
    hook_path = os.path.join(hooks_dir, "preload_user_context.py")
    if not os.path.exists(hook_path):
        pytest.skip("outfit-assistant 垂域 hook 未就位")
    spec = _ilu.spec_from_file_location("_spec_outfit_preload_behavior", hook_path)
    mod = _ilu.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run_hook_inject(mod, tmp_path: Path, monkeypatch) -> str:
    """构造 round=1 的 before_llm_call 场景，返回注入后的 system prompt content。"""
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.events import HookEvent

    ctx = AgentContext(messages=[{"role": "system", "content": "base sys"}])
    event = HookEvent(
        name="skill_executor.before_llm_call",
        payload={"round": 1, "skill_name": "outfit-stylist", "messages_count": 1},
        can_intervene=True,
    )
    mod.handle(event, ctx)
    return ctx.messages[0]["content"]


def test_preload_hook_injects_upload_images_and_env(
    tmp_path, monkeypatch,
) -> None:
    """hook 注入含 upload_images（透传 executor path）+ env（key 可用性 + 技能挂载）。"""
    pkg = tmp_path / "agent-pkg"
    (pkg / "workspace" / "u1_c1" / "upload").mkdir(parents=True)
    (pkg / "workspace" / "u1_c1" / "upload" / "a.png").write_bytes(b"x")
    (pkg / "harness" / "skills" / "forge-image-gen").mkdir(parents=True)
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(pkg))
    monkeypatch.setenv("AF_USER_ID", "u1")
    monkeypatch.setenv("AF_CLIENT_ID", "c1")
    monkeypatch.setenv("DASHSCOPE_API_KEY", "sk-test-not-real")
    monkeypatch.delenv("AF_WORKDIR", raising=False)

    mod = _load_outfit_preload_hook()
    injected = _run_hook_inject(mod, tmp_path, monkeypatch)

    # upload_images：upload/ 内图片的确定性路径（executor 已翻译）
    assert "upload_images" in injected, f"注入缺 upload_images: {injected[:300]}"
    assert "upload/a.png" in injected, f"注入缺 upload 图片路径: {injected[:300]}"
    # env：key 可用性 + 技能挂载
    assert "dashscope_key_available" in injected, f"注入缺 dashscope_key_available"
    assert "forge_image_gen" in injected, f"注入缺 forge_image_gen"
    assert "video_gen" in injected, f"注入缺 video_gen"


def test_preload_hook_injects_recent_weather_from_history(
    tmp_path, monkeypatch,
) -> None:
    """hook 读 history 最近卡片"地点天气"字段注入（持久化天气复用，避免 LLM 再查）。"""
    # hook 的 history/天气读取用相对路径 _MEMORY_DIR（与 history_files 同基准，
    # HostExecutor 下解析到进程 cwd）→ chdir 到 tmp_path 使相对路径命中测试目录。
    hist = tmp_path / ".agentfoundry" / "memory" / "outfit-profile" / "history"
    hist.mkdir(parents=True)
    (hist / "2026-08-16-beijing.md").write_text(
        "# 历史搭配记录：2026-08-16\n- **地点天气**：北京，晴，25°C，湿度50%\n",
        encoding="utf-8",
    )
    monkeypatch.chdir(tmp_path)

    mod = _load_outfit_preload_hook()
    injected = _run_hook_inject(mod, tmp_path, monkeypatch)

    assert "weather" in injected or "地点天气" in injected, (
        f"注入缺最近天气: {injected[:300]}"
    )
    assert "北京" in injected, f"注入缺天气地点: {injected[:300]}"


def test_preload_hook_skips_non_first_round(tmp_path, monkeypatch) -> None:
    """round != 1 不重复注入（I-4 幂等——多轮续接时 system prompt 已带上下文）。"""
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.events import HookEvent

    mod = _load_outfit_preload_hook()
    ctx = AgentContext(messages=[{"role": "system", "content": "base sys"}])
    event = HookEvent(
        name="skill_executor.before_llm_call",
        payload={"round": 2, "skill_name": "outfit-stylist", "messages_count": 1},
        can_intervene=True,
    )
    mod.handle(event, ctx)
    assert ctx.messages[0]["content"] == "base sys", "round!=1 不应注入"


@pytest.mark.parametrize("coord_event", [
    "coordinator_v1.before_llm_call",
    "coordinator_v2.before_llm_call",
])
def test_preload_hook_injects_on_coordinator_events(
    tmp_path, monkeypatch, coord_event,
) -> None:
    """hook 在 coordinator v1/v2 各自的 before_llm_call 事件下均注入。

    背景（2026-08-16 根因修复 + 事件拆分）：
    - UC2 走 coordinator v2（coordinator_agent_loop.py:357），原 hook 只监听
      skill_executor.before_llm_call → 永不触发、上下文从未注入（LLM 首轮说
      "我先查一下档案/天气"）。
    - v1（coordinator_decide.py:597）与 v2 是两套差别较大的体系，事件按版本拆分
      （coordinator_v1.* / coordinator_v2.*，用户确认）——hook 分别监听两者，
      任一触发都在首轮注入预加载上下文。
    """
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.events import HookEvent

    pkg = tmp_path / "agent-pkg"
    (pkg / "workspace" / "u1_c1" / "upload").mkdir(parents=True)
    (pkg / "workspace" / "u1_c1" / "upload" / "a.png").write_bytes(b"x")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(pkg))
    monkeypatch.setenv("AF_USER_ID", "u1")
    monkeypatch.setenv("AF_CLIENT_ID", "c1")
    monkeypatch.delenv("AF_WORKDIR", raising=False)

    mod = _load_outfit_preload_hook()
    ctx = AgentContext(messages=[{"role": "system", "content": "base sys"}])
    event = HookEvent(
        name=coord_event,
        payload={"round": 1, "model": "deepseek-v4-flash", "messages_count": 1},
        can_intervene=True,
    )
    mod.handle(event, ctx)
    injected = ctx.messages[0]["content"]
    assert "upload_images" in injected, f"{coord_event} 未注入: {injected[:200]}"
    assert "预加载" in injected, f"{coord_event} 注入缺预加载段: {injected[:200]}"
