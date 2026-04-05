## Coordinator 协同机制设计分析（2026-04-05）

### 1. 背景

Coordinator 是 AgentFlow 框架 dispatcher-coordinator-skill 三层协同架构中的核心编排组件。其设计目标是：

1. **结果判定**：在 dispatcher 分发 skill、skill 执行完成后，判断是否已达成用户请求的最终结果
2. **查漏补缺**：分析当前已完成的 skill 结果，判断是否还需要规划新的 skill 继续完成任务
3. **协同编排**：作为 dispatcher 与 skill 之间的协调层，管理多轮 skill 执行的生命周期

当前 coordinator 机制已具备基本的框架骨架和数据流通路，但在策略层面的实现完整性、与业界 agent loop 实践的对齐程度上仍有较大提升空间。

本文档从三个维度展开分析：
- **当前实现分析**：审视现有 coordinator 实现的完整性和问题
- **对标分析**：对比 OpenClaw 和 Claude Code 的 agent loop 实现
- **改进建议**：识别待优化和待实现的能力，给出改进方案

### 2. 方案简述

本文档为设计分析型 enhancement，核心输出为：

1. 对当前 coordinator 实现的全面审计报告
2. 与 OpenClaw、Claude Code 的 agent loop 实现的详细对比矩阵
3. 分层次的改进建议（已实现/需优化/待实现）

改进方向覆盖以下关键能力维度：
- 任务完成判定机制
- 多轮 skill 规划与调度
- 失败处理与重试
- 上下文管理与压缩
- 循环检测与死锁防护
- 并发控制与资源管理

### 3. 约束与假设

- 分析基于 AgentFlow 当前 main 分支代码
- OpenClaw 代码路径：`~/openclaw/openclaw`（TypeScript/Node.js 实现）
- Claude Code 代码路径：`~/ClaudeProjects/cc_src`（TypeScript/Bun 实现）
- 三个项目的设计目标和应用场景存在差异，对比重在机制借鉴而非直接移植

### 4. 当前实现分析

#### 4.1 架构概览

AgentFlow 的 coordinator 采用 **dispatcher-coordinator-skill 三层模型**：

```
用户请求 → SessionRouter → MMAgentSession
    → Dispatcher Task (intent_request)
        → Skill Tasks (并行执行)
    → Coordinator Task (data_exchange)
        → 结果判定 / 再规划
    → SessionRouter → 用户响应
```

每个 session 包含固定的三类 task：
- **Dispatcher**（task_index=0）：接收用户请求，解析 `next_skills` 并分发
- **Coordinator**（task_index=1）：接收 skill 完成通知，判定结果和再规划
- **Skill**（task_index≥2）：具体技能执行单元

#### 4.2 核心代码结构

| 文件 | 职责 |
|------|------|
| `include/internal/agent/coordinator.h` | `SessionTaskCoordinator` 类声明 |
| `src/agent/coordinator.cpp` | workflow 映射、消息构造、session 聚合 |
| `src/agent/agent.cpp` | **协调逻辑主体**：`handle_task_data_exchange` 中 coordinator 分支 |
| `include/internal/agent/builtin_dispatcher.h` | `BuiltinDispatcher` + `SkillSpec` 定义 |
| `src/agent/builtin_dispatcher.cpp` | `next_skills` 解析、skill 启动、结果转发 |
| `include/internal/agent/session.h` | session 配置与 coordinator/dispatcher task 引用 |
| `src/agent/task.h` / `task.cpp` | 三队列模型（intent/data_exchange/response） |

#### 4.3 Coordinator 的协同流程

**阶段一：Dispatcher 接收并分发**

1. 用户消息通过 `SessionRouter` 路由到 session 的 dispatcher task 的 `intent_request` 队列
2. `handle_task_intent_request` 创建 dispatcher workflow 实例并提交执行
3. Dispatcher workflow 执行完成后，在 `handle_task_data_exchange` 中读取 DataBus 中的 `next_skills`
4. 调用 `launch_skill_task` 启动指定的 skill

**阶段二：Skill 执行与结果通知**

1. Skill workflow 被调度执行，结果写入 DataBus
2. Skill 完成后，框架自动通知 coordinator：
   - 将完成消息写入 coordinator 的 `response` 队列
   - 向 coordinator 的 `data_exchange` 队列投递 `trigger_type=skill_response` 触发消息
3. `pending_skills_count` 计数器递减

**阶段三：Coordinator 判定与再规划**

1. Coordinator 在 `handle_task_data_exchange` 中处理触发消息
2. 从 `response` 队列取出 skill 完成通知
3. 读取 DataBus 中的 `next_skills`/`selected_skills` 判断是否需要再规划
4. 若有新的 `next_skills`，调用 `launch_skill_task` 继续执行
5. 若无新规划且 `pending_skills_count == 0`，读取 `response` 键生成最终响应

#### 4.4 关键数据契约（DataBus Keys）

| 键名 | 写入方 | 读取方 | 作用 |
|------|--------|--------|------|
| `next_skills` | workflow 节点 | coordinator/dispatcher | 下一批待执行的 skill 列表 |
| `selected_skills` | workflow 节点 | coordinator/dispatcher | 兼容旧格式的 skill 列表 |
| `response` | workflow 节点 | coordinator | 最终响应内容 |
| `multi_response` | workflow 节点 | coordinator | 流式多轮响应 |
| `pending_skills_count` | 框架 | coordinator | 待完成 skill 计数 |
| `launched_skills` | 框架 | coordinator | 已启动的 skill 列表（逗号分隔） |
| `skills_scheduled` | 框架 | coordinator | 标记 skill 已调度 |
| `candidate_skills` | workflow 节点 | dispatcher | 限制可选 skill 范围 |

#### 4.5 已识别的问题

**问题 1：SessionTaskCoordinator 与 MMAgentSession 队列脱节**

`SessionTaskCoordinator::process_session_messages` 依赖 `session.dequeue_intent_request_message` / `session.dequeue_data_exchange_message`，但这两个方法在 `session.cpp` 中是空实现（恒返回 `false`），导致该路径不可用。

```cpp
// src/agent/session.cpp:226-233
bool MMAgentSession::dequeue_intent_request_message(MMAgentMsgQue::Message& message) {
  (void)message;
  return false;  // ← 始终返回 false
}
```

实际的消息处理逻辑已迁移到 `MMAgent::handle_task_data_exchange` 中直接实现，`SessionTaskCoordinator::process_session_messages` 成为死代码。

**问题 2：BuiltinDispatcher 实例未参与主路径**

`BuiltinDispatcher` 的 `process_request` / `process_skill_response` 方法未在 `MMAgent` 中被调用。`builtin_dispatchers_` map 只有插入操作而无读取。实际的 dispatcher 逻辑已在 `agent.cpp` 中以内联方式实现，导致存在两套逻辑（`BuiltinDispatcher` 类方法 vs `agent.cpp` 内联代码）。

**问题 3：终止判定缺乏语义层**

当前 coordinator 判定任务完成的方式纯粹基于 DataBus 键的存在性检查（`response` 是否存在、`pending_skills_count` 是否为 0），缺乏：
- 结果质量评估机制
- 语义级别的完成度判断
- 多轮对话的上下文关联判定

**问题 4：同名方法导致的维护混淆**

`MMAgent::process_session_messages` 与 `SessionTaskCoordinator::process_session_messages` 职责不同但名称相同，前者是对 task 的 `process_pending_messages_once` 的封装，后者是从 session 队列构建 task 的逻辑。

**问题 5：规划与终止语义完全依赖 DataBus 约定**

C++ 层缺乏统一的状态机模型来描述 coordinator 的生命周期状态（如 `idle → dispatching → waiting_skills → evaluating → completed/re_planning`）。所有状态通过 DataBus 键的组合隐式表达，调试成本高且容易因写入顺序或竞态条件导致状态不一致。

**问题 6：缺少循环和死锁防护**

当前没有 skill 执行轮次上限、coordinator 循环检测、超时中断等保护机制。如果 coordinator workflow 持续产生 `next_skills`，理论上会形成无限循环。

**问题 7：缺少失败恢复机制**

skill 执行失败时，框架通过 `skill_launch_failed` 消息通知 coordinator，但 coordinator 侧缺乏系统性的失败处理策略（如重试、降级、跳过）。

### 5. 对标项目分析

#### 5.1 OpenClaw Agent Loop 实现分析

**项目定位**：TypeScript/Node.js 实现的 AI Agent 平台，通过 Pi Coding Agent SDK 驱动 LLM 多轮工具调用。

**核心架构**：

```
用户消息 → auto-reply → runAgentTurnWithFallback
    → runEmbeddedPiAgent（外层重试循环 while(true)）
        → runEmbeddedAttempt（单次尝试）
            → session.prompt（Pi SDK 内部工具循环）
    → subscribeEmbeddedPiSession（事件聚合）
    → buildEmbeddedRunPayloads（结果构建）
```

**关键实现细节**：

| 机制 | 实现方式 |
|------|----------|
| **Agent Loop 主循环** | `runEmbeddedPiAgent` 中 `while(true)` 循环，上限 `MAX_RUN_LOOP_ITERATIONS`（最大 160 次） |
| **任务分发** | 由 LLM 自主选择工具（`tool_choice`），OpenClaw 控制可用工具列表 |
| **任务协调** | 无独立 coordinator 组件；`sessions_spawn` + `subagents` 工具实现子代理编排 |
| **终止判定** | 模型停止产生 `tool_calls` 即终止；框架侧通过 `stopReason` 分类（`end_turn`/`tool_calls`） |
| **重试机制** | 上下文溢出 → compaction/截断；超时 → compaction；认证失败 → profile 轮换；模型失败 → fallback |
| **循环检测** | `tool-loop-detection.ts` 检测重复无进展的工具调用，分 warning/critical 两级阻断 |
| **工具策略** | `applyToolPolicyPipeline` 按渠道/模型/沙箱/用户策略动态裁剪工具集 |
| **子代理** | `sessions_spawn` 创建子运行，`subagentRole: "orchestrator" | "leaf"` 标识角色 |
| **上下文管理** | `contextEngine.compact` 在 overflow/timeout 路径触发，`resolveContextEngine` 动态选择策略 |

**核心代码路径**：

- 外层重试循环：`src/agents/pi-embedded-runner/run.ts`
- 单次尝试：`src/agents/pi-embedded-runner/run/attempt.ts`
- 工具循环检测：`src/agents/tool-loop-detection.ts`
- 工具钩子：`src/agents/pi-tools.before-tool-call.ts`
- 子代理编排：`src/agents/tools/sessions-spawn-tool.ts`

#### 5.2 Claude Code Agent Loop 实现分析

**项目定位**：TypeScript/Bun 实现的 AI 编程助手客户端，围绕单一 `queryLoop` 异步生成器构建。

**核心架构**：

```
QueryEngine → query()
    → queryLoop()  while(true)
        → 消息预处理（snip / microcompact / collapse / autocompact）
        → callModel（流式 + fallback 重试）
        → 有 tool_use?
            → 是：执行工具 → 合并结果 → continue
            → 否：stopHooks → 判定完成 → return
```

**关键实现细节**：

| 机制 | 实现方式 |
|------|----------|
| **Agent Loop 主循环** | `query.ts` 中 `queryLoop` 单一 `while(true)` 异步生成器 |
| **任务分发** | LLM 产生 `tool_use` block → 框架执行 → 结果回填 `tool_result` → 下一轮 |
| **Coordinator 模式** | 环境变量 `CLAUDE_CODE_COORDINATOR_MODE` 切换，专用 system prompt + 工具白名单 |
| **终止判定** | `needsFollowUp === false`（无 `tool_use`）→ `return { reason: 'completed' }` |
| **重试机制** | `FallbackTriggeredError` → 切换模型 + 补充 `tool_result` 错误块；最大输出 token 恢复（最多 3 次） |
| **上下文压缩** | 四级压缩链：snip → microcompact → context collapse → autocompact |
| **Token 预算** | `tokenBudget.ts` 按全局 turn token 与 budget 比较，低于 90% 时注入 meta 续跑消息 |
| **流式工具执行** | `StreamingToolExecutor` 并发执行只读安全工具，排队独占工具 |
| **非流式工具编排** | `partitionToolCalls` 分批并行/串行执行，并发上限 10 |
| **子代理** | `runAgent.ts` 复用 `query()` 并传入 `agentId`，通过 `ToolUseContext` 隔离 |
| **工具白名单** | Coordinator 模式仅允许 `Agent`/`TaskStop`/`SendMessage`/`SyntheticOutput` |
| **Stop Hooks** | 回合结束钩子：记忆提取、prompt 建议等，可阻止 continuation |
| **maxTurns** | 子代理可配置最大轮次，超限返回 `max_turns` |
| **阻塞限制** | `calculateTokenWarningState` 达硬阻塞上限时直接 `return { reason: 'blocking_limit' }` |

**核心代码路径**：

- 主循环：`query.ts`（`queryLoop`）
- 流式工具执行：`services/tools/StreamingToolExecutor.ts`
- 非流式工具编排：`services/tools/toolOrchestration.ts`
- Coordinator 模式：`coordinator/coordinatorMode.ts`
- 工具池过滤：`utils/toolPool.ts`
- Stop Hooks：`query/stopHooks.ts`
- Token 预算：`query/tokenBudget.ts`
- 子代理：`tools/AgentTool/runAgent.ts`

### 6. 三项目对比分析

#### 6.1 架构模式对比

| 维度 | AgentFlow | OpenClaw | Claude Code |
|------|-----------|----------|-------------|
| **语言** | C++ | TypeScript/Node.js | TypeScript/Bun |
| **Agent Loop 模型** | 消息驱动 + 三层 task（dispatcher/coordinator/skill） | 外层重试循环 + Pi SDK 内部工具循环 | 单一 `queryLoop` while(true) 生成器 |
| **协调方式** | 独立 coordinator task + DataBus 状态 | 无独立 coordinator；子代理树 + 工具循环检测 | Coordinator 模式（环境变量切换）+ 工具白名单 |
| **工具/Skill 选择** | 框架通过 `next_skills` DataBus 键驱动 | LLM 自主选择（`tool_choice`） | LLM 自主选择（`tool_use`） |
| **执行引擎** | MMWorkerEngine（C++ SO/Python/Shell/Go/Java） | Pi SDK session.prompt | callModel 流式 + 本地工具执行 |

#### 6.2 核心能力对比矩阵

| 能力 | AgentFlow | OpenClaw | Claude Code |
|------|-----------|----------|-------------|
| **基础 agent loop** | ✅ 消息驱动循环 | ✅ while(true) + Pi SDK | ✅ while(true) 生成器 |
| **任务分发** | ✅ next_skills 驱动 | ✅ LLM tool_choice | ✅ LLM tool_use |
| **结果通知** | ✅ response 队列 + trigger | ✅ subscribe 事件聚合 | ✅ tool_result 回填 |
| **终止判定** | ⚠️ 仅 DataBus 键检查 | ✅ 模型 end_turn + 框架侧检查 | ✅ needsFollowUp + stopHooks |
| **多轮 skill 规划** | ✅ next_skills 循环 | ✅ 模型自主多轮 | ✅ 模型自主多轮 |
| **循环上限** | ❌ 无保护 | ✅ MAX_RUN_LOOP_ITERATIONS=160 | ✅ maxTurns 可配置 |
| **循环检测** | ❌ 无 | ✅ tool-loop-detection（warning/critical） | ❌ 无显式循环检测 |
| **模型 fallback** | ❌ 不涉及 | ✅ runWithModelFallback | ✅ FallbackTriggeredError 切换 |
| **上下文压缩** | ❌ 无 | ✅ contextEngine.compact | ✅ 四级压缩链 |
| **Token 预算** | ❌ 无 | ❌ 无显式 | ✅ tokenBudget 控制 |
| **流式响应** | ⚠️ multi_response 基础支持 | ✅ 流式 block reply | ✅ 流式 generator yield |
| **并发工具执行** | ⚠️ max_concurrent_skills 计数 | ✅ Pi SDK 内部并发 | ✅ 流式并发 + 批量并发（上限 10） |
| **失败重试** | ⚠️ skill_launch_failed 通知 | ✅ 多层重试（auth/model/context） | ✅ 多层重试（fallback/compact/recovery） |
| **超时控制** | ❌ 无 | ✅ 超时 → compaction | ❌ 无显式超时（依赖模型 API） |
| **子代理** | ❌ 无 | ✅ sessions_spawn + subagentRole | ✅ runAgent 复用 query |
| **Coordinator 模式** | ✅ 固定三层架构 | ❌ 无独立 coordinator | ✅ 可选 coordinator 模式 |
| **工具策略** | ⚠️ candidate_skills 过滤 | ✅ 多层 policy pipeline | ✅ coordinator 白名单 + 工具池过滤 |
| **数据清理** | ✅ cleanup_dialogue_data | ❌ 依赖 session 生命周期 | ❌ 依赖 context 压缩 |
| **多 session 聚合** | ✅ aggregate_session_results | ⚠️ 子代理通信 | ❌ 单会话 |
| **状态机模型** | ❌ 隐式状态 | ❌ 无显式状态机 | ⚠️ QueryGuard idle/dispatching/running |

**图例**：✅ 已实现 | ⚠️ 部分实现 | ❌ 未实现

#### 6.3 实现深度对比

**6.3.1 终止判定**

- **AgentFlow**：通过 DataBus 键 `response` 的存在性 + `pending_skills_count == 0` 进行判定。纯粹的"条件满足即结束"，**不涉及结果质量评估**。
- **OpenClaw**：模型自主决定停止产生 `tool_calls` → Pi SDK 返回 → OpenClaw 侧检查 `stopReason`（`end_turn`/`tool_calls`/`yieldDetected`），支持 **yield 中断**让 agent 主动让出控制。
- **Claude Code**：`needsFollowUp` 标志（有 `tool_use` 即为 true）→ 无工具时进入完成判定链：`handleStopHooks`（记忆/建议等回合末钩子）→ `checkTokenBudget`（预算检查）→ `return { reason: 'completed' }`。

**差距分析**：AgentFlow 的终止判定最为简单，缺乏 **stop hooks**（完成前的检查链）和 **模型自主终止语义**。在 LLM 驱动场景下，coordinator workflow 需要自行实现"是否结束"的判断逻辑并写入 DataBus，框架不提供辅助。

**6.3.2 重试与恢复**

- **AgentFlow**：skill 启动失败时发送 `skill_launch_failed` 消息到 coordinator，但 coordinator 侧无系统性处理。skill 执行失败的场景（如 node 执行出错）目前通过 workflow 状态传播，coordinator 侧缺乏统一的失败感知和恢复策略。
- **OpenClaw**：多层嵌套重试——HTTP 瞬时重试 → 模型 fallback → session reset → 上下文 compaction → 认证 profile 轮换 → 最终 retry limit exhaustion。
- **Claude Code**：`FallbackTriggeredError` 触发模型切换 + 已发出 `tool_use` 的错误补偿（`yieldMissingToolResultBlocks`）→ 最大输出 token 恢复（最多 3 次 escalation）→ reactive compact → `executeStopFailureHooks`。

**差距分析**：AgentFlow 在 C++ 框架层不直接调用 LLM API，因此 LLM 层面的 fallback/重试不在框架范围内。但在 **skill 执行失败的框架级恢复**（如重试、降级、标记跳过）方面，当前实现不够完善。

**6.3.3 循环与安全防护**

- **AgentFlow**：无显式的 skill 执行轮次上限或 coordinator 循环检测。理论上 coordinator workflow 可通过持续产生 `next_skills` 形成无限循环。`force` 标记的 `reset_force_flags` 机制仅防止单个 skill 的无限重跑，不防止宏观层面的循环。
- **OpenClaw**：`tool-loop-detection.ts` 实现了两级循环检测——warning 级别输出告警、critical 级别直接阻断工具调用。外层 `MAX_RUN_LOOP_ITERATIONS` 硬性保护。
- **Claude Code**：`maxTurns` 对子代理设置轮次上限（`max_turns_reached` 时 break）。`blocking_limit` 在 token 达到硬上限时直接终止。但无显式的"工具循环检测"。

**差距分析**：AgentFlow 需要引入 **coordinator 最大轮次限制**和 **skill 调度循环检测**。

### 7. 改进分类与建议

#### 7.1 已实现且运行良好的能力

以下能力已在当前框架中正确实现，无需修改：

| 能力 | 实现位置 | 评价 |
|------|----------|------|
| 每 session 固定 dispatcher/coordinator/skill 三层 task | `agent.cpp` `create_session_from_tasks` | 结构清晰 |
| 消息驱动的三队列模型（intent/data_exchange/response） | `task.h`/`task.cpp` | 异步解耦良好 |
| `next_skills` 驱动的 skill 调度 | `agent.cpp` + `builtin_dispatcher.cpp` | 支持多格式 |
| `pending_skills_count` 并发计数 | `agent.cpp` handle_task_data_exchange | 原子性保证 |
| `launched_skills` 去重 + `force` 强制重跑 | `agent.cpp` | 防重复执行 |
| `cleanup_dialogue_data` 对话数据清理 | `builtin_dispatcher.cpp` | 防数据泄露 |
| `aggregate_session_results` 多 session 聚合 | `coordinator.cpp` | 跨 session 协调 |
| `candidate_skills` 白名单过滤 | `builtin_dispatcher.cpp` | 安全约束 |
| DataBus 键格式兼容（新格式/legacy/无前缀） | `get_framework_data` | 平滑迁移 |

#### 7.2 已实现但需要优化的能力

**优化项 1：消除 SessionTaskCoordinator 死代码路径**

当前状态：`SessionTaskCoordinator::process_session_messages` 依赖的 session dequeue 方法为 stub，实际协调逻辑已迁移到 `MMAgent::handle_task_data_exchange`。

建议方案：
- 方案 A（推荐）：实现 `MMAgentSession::dequeue_*` 方法，将 `handle_task_data_exchange` 中的 coordinator 分支逻辑回迁到 `SessionTaskCoordinator`，恢复 `process_session_messages` 的设计意图。这样能使 `SessionTaskCoordinator` 成为独立可测试的组件。
- 方案 B：删除 `process_session_messages` 及相关 dequeue stub，明确 `SessionTaskCoordinator` 仅作为 workflow 映射与 task 构造的辅助工具类，避免名实不符。

**优化项 2：统一 BuiltinDispatcher 与 agent.cpp 逻辑**

当前状态：`BuiltinDispatcher` 类的 `process_request` / `process_skill_response` 未在主路径调用，`builtin_dispatchers_` map 为死存储。dispatcher 逻辑在 `agent.cpp` 中以内联方式重新实现。

建议方案：
- 将 `agent.cpp` 中分散的 dispatcher/coordinator 逻辑收敛到 `BuiltinDispatcher` 和 `SessionTaskCoordinator` 中
- `agent.cpp` 中 `handle_task_data_exchange` 的 coordinator 分支应委托给 `SessionTaskCoordinator` 的方法
- 消除 `builtin_dispatchers_` 的死存储

**优化项 3：同名方法消歧**

当前状态：`MMAgent::process_session_messages` 与 `SessionTaskCoordinator::process_session_messages` 容易混淆。

建议方案：
- `SessionTaskCoordinator::process_session_messages` → `SessionTaskCoordinator::build_tasks_from_queue` 或 `drain_and_build_tasks`
- 或合并优化项 1 后，明确前者为"轮询驱动"、后者为"消息消费"

**优化项 4：增强 skill 失败处理**

当前状态：`skill_launch_failed` 消息会通知 coordinator，但缺乏系统性的处理策略。

建议方案：
- 在 coordinator 的 `handle_task_data_exchange` 中增加 `skill_launch_failed` / `skill_execution_failed` 的专门处理分支
- 支持可配置的失败策略：`retry`（重试，可配最大次数）、`skip`（跳过，降级执行）、`abort`（中止整个任务）
- 在 `agent.json` 的 coordinator 配置中增加 `failure_policy` 字段

#### 7.3 待实现的能力

**待实现 1：Coordinator 状态机模型**

参考 Claude Code 的 `QueryGuard`（idle/dispatching/running）和 OpenClaw 的循环计数，为 coordinator 引入显式状态机：

```
IDLE → DISPATCHING → WAITING_SKILLS → EVALUATING
  ↑                                       |
  |          ┌────────────────────────────┘
  |          ↓
  |     RE_PLANNING ─→ WAITING_SKILLS
  |          |
  └── COMPLETED / FAILED / ABORTED
```

实现建议：
- 在 `SessionTaskCoordinator` 中增加 `CoordinatorState` 枚举和状态转换方法
- 状态转换触发日志，便于追踪和调试
- 非法状态转换抛出异常或告警

**待实现 2：最大轮次限制**

参考 Claude Code 的 `maxTurns` 和 OpenClaw 的 `MAX_RUN_LOOP_ITERATIONS`：

```cpp
struct Config {
  // ...
  int max_coordination_rounds = 10;   // coordinator 最大协调轮次
  int max_skill_retries = 3;          // 单个 skill 最大重试次数
};
```

实现建议：
- 在 coordinator 的 `handle_task_data_exchange` 中维护 `coordination_round_count`
- 每次产生新的 `next_skills` 时递增
- 达到上限时强制终止并返回错误或部分结果
- 通过 `agent.json` 配置 `max_coordination_rounds`

**待实现 3：循环检测**

参考 OpenClaw 的 `tool-loop-detection.ts`：

```cpp
struct SkillExecutionRecord {
  std::string skill_name;
  std::string input_hash;    // skill 输入的哈希
  std::string output_hash;   // skill 输出的哈希
  int execution_count = 0;
};
```

实现建议：
- 在 coordinator 维护 `std::map<std::string, SkillExecutionRecord>` 记录各 skill 的执行历史
- 检测相同输入的重复执行（input_hash 相同且 execution_count > 阈值）
- 分级处理：warning（日志告警）→ critical（阻断 skill 启动）
- 与 `force` 标记协同：`force=true` 的 skill 跳过循环检测

**待实现 4：Coordinator 完成度评估钩子（Stop Hooks）**

参考 Claude Code 的 `handleStopHooks`，在 coordinator 判定"完成"前提供可扩展的检查链：

```cpp
class CoordinatorStopHook {
public:
  virtual ~CoordinatorStopHook() = default;
  struct Result {
    bool prevent_completion = false;
    std::string reason;
    std::vector<SkillSpec> additional_skills;
  };
  virtual Result evaluate(const CoordinatorContext& context) = 0;
};
```

实现建议：
- coordinator 在判定 `response` 存在且 `pending_skills_count == 0` 后，依次调用注册的 stop hooks
- stop hook 可以阻止 completion 并请求额外 skill
- 通过 SO/Python 扩展实现具体的评估逻辑
- 内置 hook 示例：完整性检查、质量评分、关键字段校验

**待实现 5：Coordinator 超时控制**

参考 OpenClaw 的超时 → compaction 机制：

```cpp
struct Config {
  // ...
  int coordination_timeout_ms = 30000;  // 单轮协调超时
  int total_timeout_ms = 300000;        // 总超时
};
```

实现建议：
- coordinator 启动时记录时间戳
- 超时后触发特定的终止流程（区别于正常完成和错误终止）
- 超时时收集已有结果生成部分响应
- 与 monitor 线程协同工作

**待实现 6：Coordinator 上下文管理**

参考 Claude Code 的四级压缩链和 OpenClaw 的 contextEngine：

在多轮 coordinator 协调中，随着 skill 结果的累积，DataBus 中的数据量会持续增长。需要：
- 对话级别的上下文摘要机制（将多轮 skill 结果压缩为摘要）
- 选择性保留关键数据，清理中间数据
- 配合已有的 `task_context_workflow` 配置点实现

实现建议：
- 在 `SessionTaskCoordinator::Config` 中已有 `task_context_workflow`（当前未接线）
- 实现 context workflow 在每轮协调后执行，压缩/摘要 DataBus 数据
- `create_session_from_tasks` 中补全 `task_context_workflow` 的赋值

**待实现 7：子代理编排能力**

参考 OpenClaw 的 `sessions_spawn` / `subagentRole` 和 Claude Code 的 `runAgent`：

当前 AgentFlow 的多 session 是通过 `session_pool_size` 静态复制 session template 实现的，缺乏运行时动态创建子 agent/子 session 的能力。

实现建议：
- 在 coordinator workflow 中支持 `spawn_sub_session` 动作
- 子 session 可继承父 session 的上下文
- 子 session 完成后结果汇聚到父 coordinator
- 通过 `aggregate_session_results`（已实现）实现结果聚合

### 8. 实现方案

根据对比分析，将改进分为以下阶段：

#### 阶段一：代码治理（消除技术债务）

解决当前实现中的死代码、逻辑分散、命名混淆问题。

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/agent/session.cpp` | 修改 | 实现 `dequeue_intent_request_message` / `dequeue_data_exchange_message` 或删除声明 |
| `include/internal/agent/coordinator.h` | 修改 | 方法重命名消歧；可选增加状态枚举 |
| `src/agent/coordinator.cpp` | 修改 | 补全 `process_session_messages` 逻辑或重构 |
| `src/agent/agent.cpp` | 修改 | 将 coordinator 分支逻辑委托给 `SessionTaskCoordinator` |

#### 阶段二：安全防护机制

引入循环检测、轮次限制、超时控制。

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `include/internal/agent/coordinator.h` | 修改 | 增加 `max_coordination_rounds`、`coordination_timeout_ms` 配置 |
| `src/agent/coordinator.cpp` | 修改 | 实现轮次计数、循环检测、超时逻辑 |
| `src/agent/agent.cpp` | 修改 | 在 coordinator 分支中检查轮次和超时 |
| `src/agent/package.cpp` | 修改 | 解析新增配置字段 |

#### 阶段三：策略增强

完成度评估、失败恢复、上下文管理。

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `include/internal/agent/coordinator.h` | 修改 | 增加 `CoordinatorStopHook` 接口和失败策略配置 |
| `src/agent/coordinator.cpp` | 修改 | 实现 stop hooks 链、失败策略执行、上下文 workflow 接线 |
| `src/agent/agent.cpp` | 修改 | 集成 stop hooks 和 context workflow |

#### 阶段四：高级编排

子代理、动态 session 创建。

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `include/internal/agent/coordinator.h` | 修改 | 增加子 session 创建接口 |
| `src/agent/coordinator.cpp` | 修改 | 实现动态子 session 生命周期管理 |
| `src/agent/agent.cpp` | 修改 | 支持运行时 session 创建和结果汇聚 |

**功能点实现优先级排序**：

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | 代码治理（死代码/逻辑统一/命名消歧） | 无副作用，内部重构 | 现有单元测试 + E2E 测试回归 |
| P1 | 最大轮次限制 | 无副作用，新增保护 | 新增 coordinator 循环测试 |
| P1 | Coordinator 状态机模型 | 无副作用，增强可观测性 | 状态转换单元测试 |
| P2 | 循环检测 | 无副作用，新增保护 | 模拟重复 skill 场景测试 |
| P2 | 超时控制 | 有行为变化（超时中断） | 超时场景 E2E 测试 |
| P2 | 失败处理策略 | 有行为变化（新增失败路径） | 失败场景单元测试 |
| P3 | Stop Hooks 扩展点 | 需定义接口契约 | Hook 注册与触发测试 |
| P3 | 上下文管理（context workflow 接线） | 依赖已有配置点 | context 压缩效果测试 |
| P4 | 子代理编排 | 较大架构扩展 | 多 session 协调 E2E 测试 |

### 9. 测试方案

**Examples 黑盒测试覆盖分析**：

| 功能点 | 现有 Examples 覆盖 | 更新建议 |
|--------|-------------------|---------|
| 基础 coordinator 流程 | ⚠️ `basic_agent`/`multi_session_chat` 部分覆盖 | 增加明确的 coordinator 多轮协调 example |
| 循环检测 | ❌ 未覆盖 | 新增 `coordinator_loop_protection` example |
| 失败恢复 | ❌ 未覆盖 | 新增 skill 失败场景 example |
| Stop Hooks | ❌ 未覆盖 | 新增自定义 stop hook example |

**测试用例设计**：

| 测试场景 | 验证点 | 预期结果 |
|----------|--------|---------|
| Coordinator 正常完成 | skill 全部完成后返回 response | response 正确返回 |
| Coordinator 多轮规划 | 第一轮 skill 完成后产生新 next_skills | 自动启动新 skill |
| 最大轮次限制 | 连续 N 轮产生 next_skills 超过上限 | 强制终止并返回错误 |
| 循环检测 | 相同输入重复执行同一 skill | warning 后 critical 阻断 |
| Skill 启动失败 | skill template 不存在 | 按策略处理（重试/跳过/中止） |
| Coordinator 超时 | skill 执行时间超过限制 | 超时中断并返回部分结果 |
| Stop Hook 阻止完成 | hook 返回 prevent_completion | coordinator 继续等待 |

### 10. 文档

**需要变更的文档**：

| 文档 | 变更说明 |
|------|---------|
| [ARCHITECTURE.md](../ARCHITECTURE.md) | 补充 coordinator 状态机模型、安全防护机制的描述 |
| [agent-lifecycle.md](../agent-lifecycle.md) | 增加 coordinator 多轮协调的生命周期说明 |
| [AGENT_PACKAGE_SPEC.md](../../publish/agent/AGENT_PACKAGE_SPEC.md) | 增加 coordinator 配置字段说明（max_rounds/timeout/failure_policy） |

**关联方案**：

| 关联方案 | 关系说明 |
|---------|---------|
| [009-feature-mmnode-执行引擎下沉-coordinator-task0-默认化](009-feature-mmnode-执行引擎下沉-coordinator-task0-默认化-260228-implemented.md) | Coordinator task0 默认化的基础设计 |
| [011-refactor-sessiontaskcoordinator-context-分层重构](011-refactor-sessiontaskcoordinator-context-分层重构-260301-implemented.md) | SessionTaskCoordinator 的分层重构基础 |
| [023-feature-next-skills-格式扩展](023-feature-next-skills-格式扩展-260319-implemented.md) | next_skills 格式扩展（含 force 标记） |
| [022-feature-multi-response-机制设计](022-feature-multi-response-机制设计-260319-implemented.md) | multi_response 流式响应机制 |
| [025-feature-session-老化-databus-清理问题](025-feature-session-老化-databus-清理问题-260320-implemented.md) | DataBus 数据清理与 coordinator 生命周期相关 |

### 11. 进展跟踪

- [ ] 代码治理（死代码清理、逻辑统一、命名消歧）
- [ ] Coordinator 状态机模型实现
- [ ] 最大轮次限制
- [ ] 循环检测机制
- [ ] 超时控制
- [ ] 失败处理策略
- [ ] Stop Hooks 扩展点
- [ ] 上下文管理（context workflow 接线）
- [ ] 子代理编排能力
- [ ] 单元测试
- [ ] 集成测试
- [ ] 文档更新
- [ ] 验证通过

**详细进度**：

| 功能点 | 功能 | 用例 | 文档 | 状态 |
|--------|------|------|------|------|
| 代码治理 | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |
| 状态机模型 | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |
| 最大轮次限制 | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |
| 循环检测 | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |
| 超时控制 | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |
| 失败处理策略 | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |
| Stop Hooks | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |
| 上下文管理 | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |
| 子代理编排 | ⏳待实施 | ⏳待实施 | ⏳待实施 | 待实施 |

### 12. 后续计划

| 优化项 | 预期效果 | 方案简述 |
|--------|---------|---------|
| 基于 LLM 的语义完成度评估 | coordinator 可调用 LLM 判断结果质量 | 集成 028 流式推理能力，coordinator 节点调用 LLM 评估 skill 结果 |
| 动态工具策略 | 根据 skill 执行历史动态调整可用 skill | 参考 OpenClaw 的 tool policy pipeline，引入多层策略栈 |
| 跨 Agent 协调 | 多个 Agent 实例间的任务协调 | 通过 Actor 系统实现 Agent 间消息路由 |
| 可视化协调流程 | 协调过程的实时可视化 | 集成 TraceSystem，输出 coordinator 状态转换链 |

### 13. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-04-05 | AI | 初始版本：当前实现分析 + OpenClaw/Claude Code 对标分析 + 改进建议 |
