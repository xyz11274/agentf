"""Hook 挂载点目录 — 动作边界语义（领域对象，hook 专用）。

设计文档：docs/design/features/hooks/hook-system.md §2.7。

与 ``agentflow_runtime.events``（28 项可观测性事件）**并存、职责分离**：
- ``events``：可观测性通知（emit_event/emit_span 消费端），hook 只读观察，不承担挂载职责。
- ``mounts``：**hook 专用动作边界**，handler 在此注入/修改/阻断（before=动作前，after=动作后）。

挂载点命名：``<node>.<before|after>_<action>``，全部成对（用户 2026-08-14 确认）。
"""

from __future__ import annotations


# ── skill_executor 侧 ──
SKILL_EXECUTOR_BEFORE_TOOL_CALL = "skill_executor.before_tool_call"
SKILL_EXECUTOR_AFTER_TOOL_CALL = "skill_executor.after_tool_call"
SKILL_EXECUTOR_BEFORE_LLM_CALL = "skill_executor.before_llm_call"
SKILL_EXECUTOR_AFTER_LLM_CALL = "skill_executor.after_llm_call"
SKILL_EXECUTOR_BEFORE_SKILL_COMPLETE = "skill_executor.before_skill_complete"
SKILL_EXECUTOR_AFTER_SKILL_COMPLETE = "skill_executor.after_skill_complete"

# ── coordinator 侧 ──
# v1（coordinator_decide.py）与 v2（coordinator_agent_loop.py）是两套差别较大的体系
#（v1 = 决策循环 + skill 委派；v2 = agent_loop 直接执行工具），事件必须区分开——
# payload 结构相同但语义不同（v1 current_round vs v2 round_i+1），共用一个事件名
# 会让 hook 无法区分触发来源（2026-08-16 用户确认拆分）。
COORDINATOR_V1_BEFORE_LLM_CALL = "coordinator_v1.before_llm_call"
COORDINATOR_V1_AFTER_LLM_CALL = "coordinator_v1.after_llm_call"
COORDINATOR_V2_BEFORE_LLM_CALL = "coordinator_v2.before_llm_call"
COORDINATOR_V2_AFTER_LLM_CALL = "coordinator_v2.after_llm_call"
COORDINATOR_BEFORE_SKILL_DISPATCH = "coordinator.before_skill_dispatch"
COORDINATOR_AFTER_SKILL_DISPATCH = "coordinator.after_skill_dispatch"
COORDINATOR_BEFORE_FINAL_RESPONSE = "coordinator.before_final_response"
COORDINATOR_AFTER_FINAL_RESPONSE = "coordinator.after_final_response"

# ── 回合级（与 events 同名，兼作挂载点）──
AGENT_DIALOGUE_START = "agent.dialogue.start"
AGENT_DIALOGUE_END = "agent.dialogue.end"


# ── 全量挂载点目录（16 项，唯一）──
ALL_HOOK_MOUNTS: frozenset[str] = frozenset({
    SKILL_EXECUTOR_BEFORE_TOOL_CALL, SKILL_EXECUTOR_AFTER_TOOL_CALL,
    SKILL_EXECUTOR_BEFORE_LLM_CALL, SKILL_EXECUTOR_AFTER_LLM_CALL,
    SKILL_EXECUTOR_BEFORE_SKILL_COMPLETE, SKILL_EXECUTOR_AFTER_SKILL_COMPLETE,
    COORDINATOR_V1_BEFORE_LLM_CALL, COORDINATOR_V1_AFTER_LLM_CALL,
    COORDINATOR_V2_BEFORE_LLM_CALL, COORDINATOR_V2_AFTER_LLM_CALL,
    COORDINATOR_BEFORE_SKILL_DISPATCH, COORDINATOR_AFTER_SKILL_DISPATCH,
    COORDINATOR_BEFORE_FINAL_RESPONSE, COORDINATOR_AFTER_FINAL_RESPONSE,
    AGENT_DIALOGUE_START, AGENT_DIALOGUE_END,
})

# ── before 挂载点集合（动作前，可注入/阻断）──
BEFORE_MOUNTS: frozenset[str] = frozenset({
    SKILL_EXECUTOR_BEFORE_TOOL_CALL,
    SKILL_EXECUTOR_BEFORE_LLM_CALL,
    SKILL_EXECUTOR_BEFORE_SKILL_COMPLETE,
    COORDINATOR_V1_BEFORE_LLM_CALL,
    COORDINATOR_V2_BEFORE_LLM_CALL,
    COORDINATOR_BEFORE_SKILL_DISPATCH,
    COORDINATOR_BEFORE_FINAL_RESPONSE,
    AGENT_DIALOGUE_START,
})

# ── after 挂载点集合（动作后，可检查/改写）──
AFTER_MOUNTS: frozenset[str] = frozenset({
    SKILL_EXECUTOR_AFTER_TOOL_CALL,
    SKILL_EXECUTOR_AFTER_LLM_CALL,
    SKILL_EXECUTOR_AFTER_SKILL_COMPLETE,
    COORDINATOR_V1_AFTER_LLM_CALL,
    COORDINATOR_V2_AFTER_LLM_CALL,
    COORDINATOR_AFTER_SKILL_DISPATCH,
    COORDINATOR_AFTER_FINAL_RESPONSE,
    AGENT_DIALOGUE_END,
})
