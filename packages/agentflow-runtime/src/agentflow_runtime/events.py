"""生命周期事件目录 — 领域对象（单一真相源）。

设计文档：docs/design/features/hooks/hook-system.md §2。

定位（领域层）：
- 事件是 agent 问答运行时的**领域概念**（回合级 / coordinator / skill 三层生命周期），
  不属于任何消费端（hook 引擎、可观测性 emit 都是事件的消费端之一）。
- 事件名是唯一权威定义，调用点引用常量而非字符串（F3 I-5 / H-5）。
- INTERVENABLE_EVENTS 为可干预事件集合（Q6 决策：5 个候选）。
- span 名 = emit_span 的生命周期单元名（同样收拢，禁止散落字符串）。
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any


# ── 回合级（对齐框架 dialogue_id 术语）──
AGENT_DIALOGUE_START = "agent.dialogue.start"
AGENT_DIALOGUE_END = "agent.dialogue.end"
AGENT_DIALOGUE_ERROR = "agent.dialogue.error"

# ── LLM 传输层（Coordinator / Skill 共用）──
LLM_REQUEST_PREPARED = "llm.request.prepared"

# ── Artifact 持久化（Skill 产物上传）──
ARTIFACT_STORAGE_RESULT = "artifact.storage_result"

# ── coordinator 决策循环（现有事件名保留）──
COORDINATOR_TRIGGER = "coordinator.trigger"
COORDINATOR_LLM_RESULT = "coordinator.llm_result"
COORDINATOR_LLM_ERROR = "coordinator.llm_error"
COORDINATOR_BATCH_RESULT = "coordinator.batch_result"
COORDINATOR_MESSAGES_SNAPSHOT = "coordinator.messages_snapshot"
COORDINATOR_SKILLS_DISPATCHED = "coordinator.skills_dispatched"
COORDINATOR_DISPATCH_LOOP_END = "coordinator.dispatch_loop_end"
COORDINATOR_FINAL_RESPONSE = "coordinator.final_response"
COORDINATOR_MAX_ITERATIONS = "coordinator.max_iterations"
COORDINATOR_MAX_ROUNDS_FORCED_OUTPUT = "coordinator.max_rounds_forced_output"
COORDINATOR_DUPLICATE_SKILL_DISPATCH_BLOCKED = "coordinator.duplicate_skill_dispatch_blocked"
COORDINATOR_TOOL_LOOP_MAX_ROUNDS = "coordinator.tool_loop_max_rounds"
COORDINATOR_TOOL_LOOP_RETRY = "coordinator.tool_loop_retry"
COORDINATOR_DISPATCH_PROGRESS_PUSH_FAILED = "coordinator.dispatch_progress_push_failed"
COORDINATOR_INTERRUPTED = "coordinator.interrupted"

# ── skill 执行循环（现有事件名保留）──
SKILL_EXECUTOR_TRIGGER = "skill_executor.trigger"
SKILL_EXECUTOR_LLM_ROUND_START = "skill_executor.llm_round_start"
SKILL_EXECUTOR_LLM_RESULT = "skill_executor.llm_result"
SKILL_EXECUTOR_LLM_ERROR = "skill_executor.llm_error"
SKILL_EXECUTOR_RECOVERY_DECISION = "skill_executor.recovery_decision"
SKILL_EXECUTOR_TOOL_CALL = "skill_executor.tool_call"
SKILL_EXECUTOR_TOOL_RESULT = "skill_executor.tool_result"
SKILL_EXECUTOR_TOOL_CALL_LOOP_DETECTED = "skill_executor.tool_call_loop_detected"
SKILL_EXECUTOR_COMPLETED = "skill_executor.completed"
SKILL_EXECUTOR_NODE_TIMEOUT = "skill_executor.node_timeout"
SKILL_EXECUTOR_MAX_ROUNDS_REACHED = "skill_executor.max_rounds_reached"
SKILL_EXECUTOR_EMPTY_RESULT = "skill_executor.empty_result"
SKILL_EXECUTOR_INTERRUPTED = "skill_executor.interrupted"

# ── span 名（emit_span 的生命周期单元名，禁止散落字符串）──
SPAN_COORDINATOR_LLM_ROUND = "coordinator.llm_round"
SPAN_COORDINATOR_SKILL_DISPATCH = "coordinator.skill_dispatch"
SPAN_SKILL_EXECUTOR_EXECUTE = "skill_executor.execute"
SPAN_SKILL_EXECUTOR_LLM_ROUND = "skill_executor.llm_round"
SPAN_SKILL_EXECUTOR_TOOL_BATCH = "skill_executor.tool_batch"


# ── 全量事件目录（事件名，唯一；不含 span 名）──
ALL_EVENTS: frozenset[str] = frozenset({
    AGENT_DIALOGUE_START, AGENT_DIALOGUE_END, AGENT_DIALOGUE_ERROR,
    LLM_REQUEST_PREPARED,
    ARTIFACT_STORAGE_RESULT,
    COORDINATOR_TRIGGER, COORDINATOR_LLM_RESULT, COORDINATOR_LLM_ERROR,
    COORDINATOR_BATCH_RESULT,
    COORDINATOR_MESSAGES_SNAPSHOT,
    COORDINATOR_SKILLS_DISPATCHED, COORDINATOR_DISPATCH_LOOP_END,
    COORDINATOR_FINAL_RESPONSE, COORDINATOR_MAX_ITERATIONS,
    COORDINATOR_MAX_ROUNDS_FORCED_OUTPUT, COORDINATOR_DUPLICATE_SKILL_DISPATCH_BLOCKED,
    COORDINATOR_TOOL_LOOP_MAX_ROUNDS, COORDINATOR_TOOL_LOOP_RETRY,
    COORDINATOR_DISPATCH_PROGRESS_PUSH_FAILED, COORDINATOR_INTERRUPTED,
    SKILL_EXECUTOR_TRIGGER, SKILL_EXECUTOR_LLM_ROUND_START, SKILL_EXECUTOR_LLM_RESULT,
    SKILL_EXECUTOR_LLM_ERROR, SKILL_EXECUTOR_RECOVERY_DECISION,
    SKILL_EXECUTOR_TOOL_CALL, SKILL_EXECUTOR_TOOL_RESULT,
    SKILL_EXECUTOR_TOOL_CALL_LOOP_DETECTED, SKILL_EXECUTOR_COMPLETED,
    SKILL_EXECUTOR_NODE_TIMEOUT, SKILL_EXECUTOR_MAX_ROUNDS_REACHED,
    SKILL_EXECUTOR_EMPTY_RESULT, SKILL_EXECUTOR_INTERRUPTED,
})

# ── 全量 span 名（唯一）──
ALL_SPAN_NAMES: frozenset[str] = frozenset({
    SPAN_COORDINATOR_LLM_ROUND,
    SPAN_COORDINATOR_SKILL_DISPATCH,
    SPAN_SKILL_EXECUTOR_EXECUTE,
    SPAN_SKILL_EXECUTOR_LLM_ROUND,
    SPAN_SKILL_EXECUTOR_TOOL_BATCH,
})


# ── 可干预事件集合（Q6 决策：5 个候选）──
INTERVENABLE_EVENTS: frozenset[str] = frozenset({
    AGENT_DIALOGUE_END,          # 改写最终响应文本
    COORDINATOR_FINAL_RESPONSE,  # 改写无 tool_call 出口的响应文本
    COORDINATOR_SKILLS_DISPATCHED,  # 阻断本轮 skill 分发
    SKILL_EXECUTOR_TOOL_CALL,    # 跳过/改写工具调用
    SKILL_EXECUTOR_COMPLETED,    # 改写返回给 coordinator 的 skill 结果
})


@dataclass
class HookEvent:
    """事件负载：name + 可变 payload + 可干预标记 + 触发时刻。

    handler 返回 None = 不干预；返回 dict = 合并/覆盖 payload 字段（H-4 干预回写）。
    handler 也可直接原地修改 payload 字段。
    """

    name: str
    payload: dict[str, Any]
    can_intervene: bool = False
    occurred_at: float = field(default_factory=lambda: time.time())
