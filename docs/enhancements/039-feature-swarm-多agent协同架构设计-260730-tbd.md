## Swarm 多 Agent 动态协同架构设计（2026-07-30）

### 1. 背景与目标

AgentFlow 目前的协同能力是 **dispatcher-coordinator-skill 三层模型**：session 内拓扑静态（skill 集合必须在 `agent.json` 预声明），dispatcher 产出 `next_skills` 后由框架 fan-out 并行拉起 skill task、fan-in 经 `pending_skills_count` 计数聚合到 coordinator（详见 031 号文档与本文 §7）。这套"session 内静态拓扑 fan-out/fan-in"骨架运行良好，但不支持 **swarm 模式**：父 agent 在运行时根据任务需要**动态 fan-out 一批并行子 agent 任务**（数量、内容运行时才知道），并对子任务做**上下文隔离、独立生命周期管理、结构化 fan-in 聚合**。

业界已有多个可参考的 swarm/多 agent 实现：

- **kimi-code**（TypeScript）：`AgentSwarm` 工具以 `prompt_template + {{item}} + items` 批量模板一次派发最多 128 个子 agent，前台按序聚合 XML 结果；
- **codex**（Rust）：multi-agent V1（spawn/wait/close 工具）与 V2（层级 `AgentPath` 寻址 + mailbox 异步信箱 + `trigger_turn` 唤醒）两代并存；
- **openJiuwen jiuwenswarm**（Python）：Leader-Teammate 二元角色团队，声明式能力装配 + 可序列化蓝图跨进程重建 + A2X 注册中心分布式控制面；
- **openJiuwen agent-core-java**（Java）：`agentteams` 全功能 Leader-Teammate 框架（每成员单线程事件循环 + 任务看板 + 框架级结果捕获），是 Python 版的对齐移植。

本文档目标：在 AgentFlow（C++ 的 MMAgent→MMWorkflow→MMNode 三层 + dispatcher-coordinator-skill 协同 + Master/Worker runtime + MMDataBus 框架）上完成 swarm 模式多 agent 动态协同能力的**架构设计**（不启动编码），具体输出：

1. 四个参考项目的实现机制分析（§4）与横向对比矩阵（§5）；
2. 跨项目共性技术难点归纳（§6）；
3. AgentFlow 现状的可复用点与缺口清单（§7）；
4. 分两期落地的 swarm 架构设计方案（§8，核心章节）：一期 session 内 swarm（最短路径），二期跨 agent swarm；
5. 精确到文件的具体修改点清单（§9）与实施路线（§10）。

### 2. 方案简述

采用**两期递进**设计：

- **一期（session 内 swarm）**：把"子 agent 任务"建模为 skill task 的推广——新增 `SwarmManager` 组件负责 swarm 计划解析（prompt_template/item 展开）、槽位状态机、批量调度（ramp + 限流）与结构化聚合；fan-out 复用 `launch_skill_task`/`process_next_skills_launch` 链路，fan-in 复用 `pending_skills_count` + coordinator 触发链路；子任务上下文用 dialogue 键前缀子命名空间隔离。不引入跨 agent 寻址，改动集中在 `src/agent/`。
- **二期（跨 agent swarm）**：扩展 agent 寻址路由（`target_agent_id`）、父子数据面共享域约定（Redis/共享前缀）、级联中断树，使子 agent 可以是同一 runtime 内另一个 agent 实例（异进程/异机依赖 035 的 worker 分进程能力，本文不展开）。

设计总体原则：**借鉴机制而非移植代码**——四个参考项目语言与运行时各异（TS/Rust/Python/Java），本文只提取其机制设计（批量模板派发、ramp 限流、mailbox 通知、过滤式 fork、框架级结果捕获、三级并发闸门、声明式蓝图等），映射到 AgentFlow 的 session/task/DataBus/FlowTrace 既有概念上落地。

### 3. 约束与假设

- 分析基于的本地代码路径：
  - 参考项目：`~/gitProjects/kimi-code`（TypeScript monorepo，含 agent-core v1 与 agent-core-v2 两套引擎）、`~/gitProjects/codex`（Rust，`codex-rs/`）、`~/atomGitProjects/jiuwenswarm`（Python，团队内核在外部依赖 openjiuwen SDK）、`~/atomGitProjects/agent-core-java`（Maven，Java 17，Python 版 parity 移植）；
  - 本项目：AgentFlow `main` 分支 HEAD（e8ffbf0）。
- 本文中本项目结论均保留 `文件:行号` 证据；参考项目结论给文件路径（行号以其本地工作区当前版本为准，从略）。
- jiuwenswarm 的"团队内核"（TeamAgent/Runner/mailbox/任务状态机）在外部依赖 openjiuwen 中、本地无源码，相关结论以 jiuwenswarm 侧 import/调用点与其 Java 对齐移植（agent-core-java）交叉印证，不做臆测。
- **借鉴机制而非移植**：参考项目的语言机制（TS promise、Rust tokio、Python asyncio、Java 线程模型）不直接映射，本文只取其调度/隔离/聚合/控制语义。
- **当前仅设计不编码**：本文不含任何代码改动；§9 的修改点清单是后续实施 enhancement 的输入。
- 与既有 enhancement 的关系：031（coordinator 协同机制，护栏与 `spawn_sub_session` 规划）是本文的直接前置；024（消息定向路由）是一期父子消息通道的底座；034/035（DataBus 适配层、master/worker 分进程）是二期跨进程能力的前置；037（managed agent runtime）的父子会话管理与本文二期强相关；038（harness agent loop 样例）提供护栏实践参考。

### 4. 参考项目实现分析

#### 4.1 kimi-code：AgentSwarm 批量模板派发 + 前台聚合

##### 4.1.1 核心组件

仓库内有两套语义对齐的 agent 引擎：`packages/agent-core`（v1 主线）与 `packages/agent-core-v2`（DI 服务化重写，带 parity 测试）。组件地图（v1 → v2）：

| 层 | v1 | v2 | 职责 |
|---|---|---|---|
| LLM 工具 | `src/tools/builtin/collaboration/agent-swarm.ts`（`AgentSwarm`）、`agent.ts`（`Agent`） | `src/agent/tools/agent-swarm/agentSwarmTool.ts`、`agent/agentTool.ts` | 参数 schema、校验、结果渲染 |
| 批量调度 | `src/session/subagent-batch.ts`（`SubagentBatch`） | `src/session/swarm/agentRunBatch.ts` | 并发 ramp、限流恢复、逐任务超时/取消 |
| 子 agent 宿主 | `src/session/subagent-host.ts`（`SessionSubagentHost`） | `src/session/swarm/sessionSwarmService.ts` + `subagent/subagentService.ts` + `runAgentTurn.ts` | spawn/resume/retry、跑 turn、提取 summary |
| 模式状态 | `src/agent/swarm/index.ts`（`SwarmMode`） | `src/agent/swarm/swarmService.ts` | swarm mode 进入/退出、提醒注入、独占性 veto |
| 生命周期/隔离 | `src/session/index.ts` `createAgent`/`resumePersistedAgent`；每 agent 一个 `wire.jsonl` | `agentLifecycle/agentLifecycleService.ts`；每 agent `agents/<id>/wire.jsonl` | 独立上下文与持久化、按需恢复 |
| 后台任务 | `src/agent/background/index.ts` + `agent-task.ts` | `src/agent/task/*` | 前/后台统一任务化、完成通知、超时 kill |
| 权限 | `src/agent/permission/index.ts` + `policies/` | `permissionPolicy/*`、`permissionMode/*` | 策略链审批；子 agent 继承父级 |
| profile | `src/profile/`（`coder.yaml`/`explore.yaml` 等） | `app/agentProfileCatalog/*` | 子 agent 类型：系统提示词、工具白名单、可委派集合 |

职责划分要点：`AgentSwarm` 工具是**纯编排入口**（自身不创建 agent，只展开参数交给宿主并渲染 XML 结果）；`SessionSubagentHost` 是**子 agent 工厂+运行时**（每个 agent 实例自带一个，故子 agent 可再套子 agent）；`SubagentBatch` 是**调度器**（只管排队/并发/限流/超时/取消，经接口回调宿主 spawn/resume/retry）；`SwarmMode` 只是**模式标记+提示词注入**。

##### 4.1.2 发起机制

- 输入 schema：`description`（整批描述）、`subagent_type`（默认 `coder`）、`model`（primary/secondary）、`prompt_template` + `items`（`{{item}}` 占位符，字面 split/join 替换展开）、`resume_agent_ids`（恢复已有子 agent，**排在新建之前**）。
- 硬校验（任何子 agent 启动前抛出）：无 resume 时 `items >= 2`；总数 ≤ **128**（`MAX_AGENT_SWARM_SUBAGENTS`）；模板必须含 `{{item}}`；**展开后 prompt 两两不同**（防止同一 swarm 内重复干活）。
- 每任务带 `swarmIndex`（全局连续编号）与 `swarmItem`（原始 item 文本，持久化进 session metadata，供 UI 分组与 resume 回显）。
- swarm 内所有任务 `runInBackground: false`——**swarm 一律前台聚合**，工具调用阻塞到全部结束。
- 结果渲染：`<agent_swarm_result>` 包裹 `<summary>completed/failed/aborted</summary>` + 每子 agent 一个 `<subagent agent_id item outcome>` 元素；有未完成任务时追加 `<resume_hint>` 教父 agent 用 `resume_agent_ids` 续跑。

##### 4.1.3 并发与调度（SubagentBatch，调度契约固化在 `subagent-batch.ts` 头部注释）

- **Normal 阶段 ramp**：立即启动前 **5** 个（`INITIAL_LAUNCH_LIMIT`），之后每 **700ms** 再放 1 个；默认**不封顶**，仅当 `KIMI_CODE_AGENT_SWARM_MAX_CONCURRENCY` 设为正整数才封顶（非法值 fail-fast）。启动优先级：限流保存的同 agent 重试 > 显式 resume > 新 spawn。
- **Rate-limit 阶段**（首次 provider 429 进入）：被限流任务**保存 agentId 供同 agent 重试**、发 `subagent.suspended` 事件、**插回队首**、自身 3s/6s/12s 翻倍退避；若被限流的是唯一未完成任务则直接失败不挂起整批；容量管理为"进入时容量=已 ready 的正常启动数、每次再限流缩 1、连续 3 分钟无新限流则容量+1"；每轮最多放 1 个，受"容量+全局放行时刻+任务退避时刻"三重闸门控制。
- **结果与取消**：结果严格按输入序占位返回；单任务超时/失败只占自己的槽位；用户取消时已完成的保留、已启动标 `aborted/started`、未启动标 `aborted/not_started`（可安全 resume）。

##### 4.1.4 上下文隔离

- 每个子 agent 独立 id（`agent-N`）+ **独立目录** `agents/<id>` + 独立 `wire.jsonl` 追加式事件日志（执行与 resume 共用同一状态变更路径）；元数据 `{homedir, type, parentAgentId, swarmItem}` 持久化到 session metadata。
- 父子**不共享 context**：子 agent 上下文=自己的系统提示词（profile 渲染）+父下发的 prompt；父只能看到子 agent 最后一条 assistant 文本（`lastAssistantText`）。
- 共享的只有会话级服务：同一文件系统视图、MCP 管理器、hook 引擎、telemetry、RPC 通道（带 agentId 标记来源）。
- 例外：`btw`（侧问）子 agent 显式共享父上下文投影且不落盘——有意为之的轻量旁路。

##### 4.1.5 结果回传

- **前台（swarm 与默认 Agent 调用）**：promise 链等子 agent turn 完成 → 取最后一条 assistant 文本 → 沿 handle 返回 batch 占位。**summary 质量兜底**：结果文本 <200 字符自动给子 agent 追加一轮"请扩写"（最多 1 次）；turn 以 `max_tokens` 结束视为失败（宁失败也不把截断的 handoff 给父）；**完成前排干子 agent 自己的后台任务**（否则晚到的终态通知会在已完成的子 agent 上 steer 出无人观察的孤儿 turn）。
- **后台（仅 Agent 工具 `run_in_background:true`）**：统一走 BackgroundManager，立即返回 `task_id + agent_id`；完成后构造 `<notification type="task.completed">` XML 经 `turn.steer` 注入父 agent 后续 turn；有通知去重键。前台任务可 RPC `detach` 转后台（此后用户取消主 agent 不波及它）。

##### 4.1.6 resume / 超时 / 取消 / 权限

- **resume**：单次调用内 `Agent(resume=agent_id)`；前置校验 `ensureIdleSubagent`（必须是 sub 类型、属于当前父、空闲——禁止并发 resume）；限流重试走 `retry()`（重放最后一个失败请求，保留完整上下文）；跨会话按需懒加载重放 wire 恢复，父链递归恢复带环检测。
- **超时**：每子 agent run 默认 **2 小时**（env > config `[subagent] timeout_ms` > 默认；`0`=无超时且被刻意保留）；batch 层为**每个 attempt 单独 arm 定时器**，超时标 failed 不占兄弟槽位、不进入限流阶段。
- **取消**：用户中断主 agent turn → `cancelAll` **递归**先取消孙 agent 再 abort 每个前台子 agent；取消原因沿 signal 传递（在途工具能报告"用户主动中断"并要求"不要自动重试"）；每活跃子 agent 独立 AbortController 挂父 signal；批 signal/任务 signal/超时 timer 三路汇聚。
- **权限**：子 agent 有独立 PermissionManager 但**挂父链**（mode 未覆盖取父级，审批记忆沿父链合并；ask 经 RPC 冒泡到 UI 且带 agentId）；profile 做**工具裁剪**（allowlist/denylist，模型可见定义与执行前校验双重）；委派白名单（profile 的 `subagents` 集合，spawn 前强制校验）；**AgentSwarm 独占性硬约束**：`AgentSwarmExclusiveDenyPermissionPolicy`——一次响应里 AgentSwarm 必须是唯一工具调用，否则 deny 并附纠错文案；v1 默认放行列表不含 AgentSwarm（manual 模式需审批，进入 swarm mode 后自动 approve），v2 直接放进默认放行集（已知分叉点）。

##### 4.1.7 "父 agent 不做主活"的编排约束

三层约束，全是"提示词+单点硬规则"组合：① 进入 swarm mode 注入 `enter-reminder.md`（"do not handle the main work yourself"、子 agent 职责不重叠、默认不要节省子 agent 数量、最多 128 自动排队）；② 三种触发（manual/task/tool）+ turn 结束自动退出（tool 触发不注入 reminder）；③ 工具文档声明独占性由策略硬执行。**"父 agent 不做主活"没有任何代码级强制**，完全靠 reminder 引导；代码强制的只有独占响应、128 上限、prompt 去重、类型白名单。

##### 4.1.8 技术难点与解法（该项目实证）

1. provider 限流下 128 个子 agent 的批量调度 → 保守 ramp + 可选硬上限 + 限流状态机（容量收缩/静默恢复）+ 重试绑定回原 agent（省 token 且避免重复劳动）；
2. 子 agent 后台任务的"孤儿 turn" → 完成判定前循环排干 + 双向 suppress 通知；
3. handoff 质量 → profile 提示词明示"final message is the entire handoff" + 短结果自动续问（有界 1 次）+ 截断判失败；
4. resume 正确性 → 调用内幂等校验 + 同 agent 重试 + 每 agent 独立 wire 懒重放 + 父链递归+环检测，元数据持久化是关键；
5. 取消语义分层 → 原因对象沿 signal 传递，用户取消保留已完成结果、未启动标 not_started；
6. "主 agent 不干活"无法硬约束 → 只硬约束能硬约束的，编排行为交给提示词+自动退出降低模式残留。

##### 4.1.9 局限取舍

swarm 只能前台（大批量时主 agent turn 被长时间占用，上限 2h/任务）；单调度器、跨 swarm 不共享限流记忆；并发上限是 env 而非常规配置且默认不设限；嵌套深度无硬上限（靠 profile 白名单间接控制）；结果聚合是文本 XML 需父自行解析；summary 单点回传（中间产物只能靠写文件共享）；v1/v2 行为漂移（审批默认集、resume 模型继承两点已知分叉）；swarm 内所有任务共享一个 timeout 值。

#### 4.2 codex：multi-agent V1/V2 与 mailbox 通信模型

##### 4.2.1 总体形态与核心组件

codex **完整支持多 agent 协同**，存在两代并存实现加一个特殊委派机制：

| 机制 | 开关 | 寻址 | 通信模型 | 代码位置 |
|---|---|---|---|---|
| Multi-agent V1（collab） | `Feature::Collab` | 裸 ThreadId(UUID) | spawn/wait/close 工具+完成通知注入 | `core/src/tools/handlers/multi_agents/` |
| Multi-agent V2 | `Feature::MultiAgentV2` | 层级 `AgentPath`（`/root/task1/task_3`） | mailbox 异步信箱+`trigger_turn` 唤醒 | `core/src/tools/handlers/multi_agents_v2/` |
| Delegate（review/compact/guardian 专用） | 无 | 内部子 Session | 父子 channel 直连，审批代理给父 | `core/src/codex_delegate.rs` |

组件分层：工具层（`tools/handlers/multi_agents{,_v2}/`）→ 控制面 `AgentControl`（`core/src/agent/control.rs`，**每个根 session 树创建一次**、克隆给全树，持有共享 session_id、`Weak<ThreadManagerState>` 防循环引用、`AgentRegistry`、`V2Residency`、`AgentExecutionLimiter`、全树共享 token 预算 `RolloutBudget`）→ `ThreadManagerState`（全局活跃线程表）→ `CodexThread`（**每个子 agent 是一条完整 Codex 会话**：自己的事件循环、历史、rollout 文件）。agent 状态机 `AgentStatus` **从事件流推导**（TurnStarted→Running、TurnComplete→Completed(最后一条 assistant 消息) 等），每线程一条 watch channel 供父订阅。

##### 4.2.2 发起机制（工具面即控制面）

- V1 五工具：`spawn_agent`（`message|items`、`agent_type`、`model`、`fork_context`；深度超限直接回错让模型自己干）、`send_input`（可带 interrupt）、`wait_agent`（对多 target 等任一终态，超时 clamp 在 [10s,1h] 默认 30s）、`close_agent`（Completed agent 不关就占并发名额）、`resume_agent`。
- V2 六工具：`spawn_agent`（必传 `task_name` 拼成层级路径；`fork_turns` ∈ none/all/N，默认 all）、`send_message`（QueueOnly：不唤醒，turn 边界投递）与 `followup_task`（TriggerTurn：空闲即拉起新 turn）、`wait_agent`（**不指定目标、不回传内容**——只等"本线程 mailbox 有活动"，内容经邮件自然进上下文）、`interrupt_agent`、`list_agents`。
- 角色体系：内置 `default`/`explorer`/`worker`，角色即 config layer（TOML，可锁模型参数）；worker 角色的描述文本直接规定协作纪律（明确 ownership、不 revert 他人改动）——**共享工作区下的冲突避免靠 prompt 约定而非机制**。
- 可用性门控：V1 默认深度 1 时子 agent 自己看不到 spawn 工具（机制强制不可再分裂）；V2 工具总是可用（深度靠模型提示约束）。

##### 4.2.3 子 agent 创建与上下文 fork

入口 `AgentControl::spawn_agent_internal`：查执行名额 → 预约驻留槽 → 构造 `SessionSource::SubAgent(ThreadSpawn{parent, depth, agent_path, nickname, role})` → **继承父线程运行时环境**（turn environments 快照、exec policy 按配置同源共享）→ 无 fork 建全新历史 / 有 fork 走过滤式 fork → **RAII 名额预留**（`SpawnReservation`，中途失败 Drop 回滚，commit 才落账）→ 持久化 spawn 边到 agent-graph-store（供崩溃恢复）→ 投递初始输入。

config 继承：从父 **live turn**（非磁盘配置）克隆有效 config，把**审批策略、permission profile（含 sandbox）、cwd** 从父 turn 活体状态拷给子 agent。**子与父同 cwd、同 sandbox 策略、同文件系统——无目录隔离，隔离仅在会话/历史/rollout 层面**。

上下文 fork（过滤式）：先 flush 父 rollout 落盘；`keep_forked_rollout_item` **保留** system/developer/user 消息与 final_answer 阶段 assistant 消息，**丢弃** reasoning、function call/output、web search、compaction 等中间过程——子 agent 拿到"浓缩版"父上下文；`LastNTurns` 模式只留最近 N 轮；V2 还做 usage-hint 剔除与 developer instructions 文本级替换；全量 fork 保留 prompt-cache 前缀基线，截断 fork 放弃缓存前缀换上下文新鲜度。

##### 4.2.4 通信：mailbox 模型（V2 核心）

- 协议：`Op::InterAgentCommunication{id, author, recipient(AgentPath), content, trigger_turn, ...}`；`AgentPath` 层级路径支持相对/绝对引用；消息可密文。
- 投递：入目标 Session 的 mailbox（InputQueue）；`trigger_turn` 或线程休眠时**立即唤醒一个合成 turn**；运行中的 turn 在每个模型采样步之间排干 pending input；**assistant 已输出最终答案后邮件推迟到下一 turn**（投递相位控制）。
- 入史：邮件记录为 `ResponseItem::AgentMessage` 并持久化——**agent 间消息是一等历史公民**，可进 rollout、可恢复；每次收发打结构化 tracing。

##### 4.2.5 并发控制：三道闸门

1. **总线程数**：V1 由 registry CAS 计数限 `agent_max_threads`（默认 6）；V2 由 V2Residency 限 `max_concurrent_threads_per_session`（默认 4，即 root+3 子）；
2. **同时执行数**：`AgentExecutionLimiter` 信号量，只对 V2 且 SubAgent 来源生效，获取点在每次 turn 启动前，guard 挂 RunningTask 上 RAII 释放——**允许很多驻留 agent，但同时跑 turn 的受限**；
3. **驻留 LRU**：resident 满时挑"终态+无 active turn+mailbox 空"的线程卸载（materialize rollout → shutdown → remove），之后收到消息再冷加载恢复。即"**名义并发无限、内存驻留有界、执行并发受限**"三级设计。

##### 4.2.6 结果回传与聚合

- V1：`wait_agent` 阻塞等终态（`Completed(msg)` 携带最终消息）+ spawn 时启动 detached watcher，子到终态后向父**注入 user 角色通知消息**（不触发新 turn，下次父 turn 采样自然看到）。
- V2：子在终态事件时经 session 内钩子向父发 **queue-only 完成邮件**（含最终消息或截断到 1000 token 的错误+"如还需要它，用协作工具再派任务"的引导）；父要么正在 `wait_agent`（mailbox 活动即醒）要么下一 turn 边界收到。**聚合权完全在父模型手里——框架不做自动合并**。
- 完成通知统一走通信协议（结果与普通消息同构、同入历史、同可持久化恢复），而非特设通道。

##### 4.2.7 生命周期 / 取消 / 超时 / 权限

- 关闭：`close_agent` = 持久化边置 Closed + **递归**关闭所有后代 + flush rollout → shutdown → remove → 释放名额与驻留记录。
- 恢复：`resume_agent` 恢复单线程后从 agent-graph-store 读 Open 边 **BFS 恢复整棵后代树**；任何 send 返回 `InternalAgentDied` 时自动清理名额；所有工具错误转译为模型可读字符串让模型自救。
- 取消：`interrupt_agent` → `Op::Interrupt` → CancellationToken 层级 abort；V2 interrupt 不杀 agent（保持可联系）。
- 超时：**agent 执行本身无超时**——只有 wait 工具有超时；失控 agent 靠父 interrupt/close 或整树共享 `RolloutBudget`（token 预算超限 → TurnAborted(BudgetLimited)）兜底。
- 权限：子继承父 approval policy 与 permission profile（运行时活体值）；delegate 机制中子审批路由回父 session。
- 调度提示在 prompt 层：工具描述写完整委派方法论（只委派可并行 sidecar 任务、写集不相交、wait 要克制）；`MultiAgentMode` 指令热切换"是否主动委派"；每 turn 环境上下文注入活 agent 列表让模型知道可联系谁。

##### 4.2.8 技术难点与解法（该项目实证）

1. "完成通知"与"不打扰"的矛盾 → mailbox+`trigger_turn` 双模投递 + answer 边界后推迟；wait 抽象为"等信箱活动"避免按 target 轮询；
2. 并发上限 vs 长期存活 agent → 总数 CAS / 执行信号量 RAII / 驻留 LRU+冷加载三级模型；
3. 上下文共享的成本与污染 → rollout 过滤式 fork + 文本级指令清洗 + 截断 fork 放弃缓存前缀；
4. 动态 agent 集合下模型如何寻址 → 层级 AgentPath + 每 turn 注入活 agent 列表 + 人类可读昵称；
5. 父子配置一致性 → spawn 时从父 live turn 刷新继承清单（approval/sandbox/cwd/provider/exec_policy/environments 显式枚举）；
6. 失败可恢复 → spawn 边持久化 + BFS 子树恢复 + RAII 名额预留防部分失败泄漏；
7. 循环引用 → 控制面持 Weak 引用；watcher detached 且终态自退。

##### 4.2.9 局限取舍

同进程共享工作区（文件冲突靠 prompt 纪律，无 worktree 隔离）；无自动结果聚合；V1 寻址脆弱（裸 UUID 给模型拼写）；执行无超时（靠父判断+共享 token 预算）；V2 wait 语义弱化对模型工具使用能力要求更高；V1/V2/delegate 三代并存行为分叉；无跨机分布（cloud-tasks 是分离的另一套）。

#### 4.3 jiuwenswarm：Leader-Teammate 团队 + 声明式装配 + 分布式控制面

##### 4.3.1 代码边界与核心组件

**关键事实**：swarm 的"团队内核"（TeamAgent、Runner、TeamRuntimePool、TeamMonitor、mailbox、任务/成员状态机）**不在本仓库**，而在外部依赖 openjiuwen SDK；jiuwenswarm 是它的**产品化装配层+生命周期管理层+分布式控制面+多端接入层**。分层：

```
IM/Web/TUI 频道 (channels/, gateway/)
  → AgentServer: server/runtime/agent_adapter/team_helpers.py（请求分流、waiter/广播、流式回传）
  → TeamManager: agents/harness/team/team_manager.py（会话级 team 生命周期、spec 构建）
      ├─ config_loader.py（配置 → TeamAgentSpec dict）
      ├─ agents/swarm/（声明式能力装配：rails/tools/subagents → DeepAgentSpec）
      ├─ distributed_runtime.py（分布式判定/传输字段归一化）
      └─ remote_member_bootstrap.py（分布式控制面：A2X 预约/bootstrap/ACK/destroy）
  → openjiuwen SDK（Runner.run_agent_team_streaming / TeamAgent / TeamMonitor / messager / 共享存储）
  → Monitor handlers（SDK 事件 → 前端事件）
```

##### 4.3.2 顶层抽象与角色定义

- 团队只有 **Leader / Teammate 二元角色**（外加 HumanAgent 人类席位）：Leader 每团队一个（配置于 `modes.team.<template>.leader`）；Teammate 动态 spawn（配置模板在 `modes.team.<template>.agents`，`$` 引用共享模板）；`predefined_members` 可静态预声明。
- `TeamAgentSpec` 蓝图关键字段（pydantic，可 JSON round-trip）：`lifecycle`（persistent|temporary）、`teammate_mode`、`spawn_mode`（inprocess）、`enable_hitt`、`enable_permissions`、`transport.type`（inprocess|pyzmq）、`storage.type`（sqlite|postgresql）、`enable_swarmflow` + `swarmflow_concurrency`（三层并发闸门）、`memory`（团队共享记忆 TEAM_MEMORY.md，默认关）。
- 运行时对象：`TeamAgent` 由 `spec.build()` 构造；teammate 侧经 `TeamAgent.from_spawn_payload(payload)` 从**序列化 spec+context 重建**——这是跨进程成员重建的核心机制。

##### 4.3.3 声明式能力装配（该项目最有特色的设计）

- 成员能力 = 一组**纯声明式 spec**（RailSpec/BuiltinToolSpec/SubAgentSpec，pydantic 可序列化），按 mode 分档（team/code.team/team.plan）、按 role 区分（leader 挂团队技能进化+创建 rail，teammate 挂成员技能进化 rail）；
- **属性 vs 环境分离**：config 派生的设定在 spec-build 期"烘焙"进 spec 参数；随请求变化的环境值由 `SwarmBuildContext` 构造期注入；
- **跨序列化边界靠 seed 重建**：`spec.build_context_seed = ctx.to_seed()`，接收侧注册工厂从 seed+本地句柄重建活 context——**分布式 spawn 不传闭包、只传可序列化蓝图的关键**；
- provider 注册表：`@harness_element` 声明的元素幂等注册进 SDK 的 provider registry，`RailSpec.build()` 按 type 查表调工厂。

##### 4.3.4 任务分解与分发

- **Leader 驱动模式（主模式）**：用户目标 → Leader 分析 → Leader 组队（`build_team` 工具）→ 拆解任务 → Teammate **认领** → 执行 → 汇报 → Leader 汇总。组队/拆解/认领工具由 openjiuwen TeamAgent 提供，jiuwenswarm 通过 monkey-patch 包装挂分布式钩子（`attach_*_wrapper` 系列）。任务状态机投影含 **review/verify 回环**（submitted_for_review→verified 或 revision_requested）——leader 对 teammate 结果有验收机制。会话隔离队名（`build_session_scoped_team_name`）避免跨会话冲突。
- **SwarmFlow**（自然语言驱动的多阶段工作流）：Leader 经 SwarmflowTool 启动工作流生成 run_id；任务分解为有序 Phase，Phase 内若干 agent 并行/串行；生命周期事件流 workflow_started→phase→agent_started/completed/failed→workflow_completed；并发治理三层闸门 `swarmflow_concurrency`（L1 max_workflows / L2 agents_per_run / L3 max_agents_total 全局准入）；进度经 WorkflowMonitorHandler 聚合为增量 delta 推送；支持 worktree 隔离选项与 run 状态持久化/恢复。
- **team.plan 模式**：规划先行（`enable_team_plan`）。

##### 4.3.5 通信与分布式控制面

- 三种通信面：① 业务数据面（共享存储 sqlite/PG + openjiuwen messager mailbox，transport inprocess 或 pyzmq）；② 分布式控制面（direct ZMQ + A2X 注册中心）；③ 人机消息面（`$member_name` 前缀注入发言人身份、@mention 路由、godview/private/mention 扇出）。
- 事件总线：SDK TeamMonitor 事件 → 归一化为 `team.<category>.<action>` 字符串（新增 SDK 事件零改动）→ 扇出到该 session 所有 waiter 的 asyncio.Queue；**单 waiter 模型**（follow-up 请求只 interact() 注入不建 waiter，事件由长 waiter 产出——注释详述了重复消息事故教训）。
- **分布式组队时序**：teammate（blank agent）启动时把自己注册到 A2X 注册中心为空闲节点；leader 的 spawn 工具被包装为：① 向注册中心**预约**空闲 teammate（TTL 默认 30s）→ ② 调原始 spawn 写 roster 后**强制把成员状态回拨为 UNSTARTED** 等 ACK → ③ 经 direct ZMQ 发 bootstrap envelope（team/session/member/leader 路由/messager 引导/prompt/装配 hint）→ ④ teammate 收到后临时构建辅助 TeamAgent 读共享 DB 取成员上下文、**丢弃辅助实例**、`TeamAgent.from_spawn_payload` 重建正式成员 → 发 direct ACK → **replace_agent_card 把注册卡片从 idle 换成 busy**（防 TTL 过期被重复预约）→ 跑成员执行循环；leader 侧 ACK 监听器置成员 READY。失败路径：成员标 ERROR 并立即释放预约。
- 解散清理：leader direct 发 destroy envelope；`clean_team` 前确保远程 teammate 全部 shutdown（带超时与 stuck 通知）。

##### 4.3.6 结果聚合 / 并发 / 上下文

- **结果聚合是"leader 汇总"单模式**：teammate 汇报 → leader（LLM）产出最终交付物，含 review 回环；前端侧做角色过滤（可只显示 leader 输出，**审批只经 leader 路由到前端**）；SwarmFlow 为增量 delta+终态 delta。**无投票/多路归并等可编程聚合策略**。
- **并发模型**：业务进程内全 asyncio 单事件循环；`spawn_mode=inprocess` 成员 runtime 内嵌同进程；分布式模式 leader/teammate 为独立进程（可跨机）经 pyzmq+共享 PostgreSQL 协作；**分布式强制单活 session**（切换会话先停旧会话，有切换锁）；生命周期四操作（pause/terminate/cancel/delete）均过 lifecycle lock 串行化，cancel 可抢占 pause。
- **上下文与状态**：session_id 贯穿、轮次号递增；团队工作区共享技能目录经**符号链接视图**维护（成员 spawn 时只暴露其配置的 skill links）；团队共享记忆默认关；暂停="拆前台流+Runner 池内驻留"可恢复；KV cache 亲和（terminate/delete 前 dispatch offload/prefetch 信号）。

##### 4.3.7 失败处理 / 超时 / 护栏

- LLM 输出护栏：`llm_retry_rail`（重复片段/流式帧超时检测，命中重发模型请求，max_retries=2）与 `circuit_breaker`（工具重复/ping-pong/无进展循环强制收尾，**默认关**）；
- 分布式失败：bootstrap 未送达→成员 ERROR+释放预约；外部命令 120s 超时、PG 启动后 30s 退避探活；
- 成员 `completion_timeout` 默认 600s、`max_iterations` 默认 200；monitor 单事件处理失败不终止收集循环；
- 生命周期：delete 时 KVC 亲和开启则先保留 Runner entry 供快照绑定关系。

##### 4.3.8 技术难点与解法（该项目实证）

| 难点 | 解法 |
|---|---|
| 跨进程传递"装配好"的 agent（闭包不可序列化） | 纯声明式 spec + build_context_seed + 注册工厂重建（from_spawn_payload） |
| 分布式节点发现与防重复预约 | 外置 A2X 注册中心：blank 注册 → TTL 预约 → bootstrap 后卡片置忙；失败即 release |
| 控制面与业务面混扰 | 明确分层：bootstrap/ACK/destroy 走 direct ZMQ，任务/消息走共享 DB+team 链路 |
| 分布式辅助实例资源泄漏 | 辅助 TeamAgent 用完即弃+释放端口；动态成员重分 loopback 端口 |
| 多请求并发下事件重复/闪烁 | 单 waiter 模型 + deferred processing_status + 退出后 drain 残留事件 |
| cancel 被长 pause 阻塞 | cancel 标志+抢占式取消 pause 任务+锁外先行 stop |
| 大规模技能上下文爆炸 | Symphony 技能树检索+can_feed 编排图（独立子系统） |

##### 4.3.9 局限取舍

内核黑盒化（大量 monkey-patch 包装 SDK 私有接口，升级回归风险高）；分布式仅扩展 teammate 侧算力、**leader 单点**；分布式会话单活（多窗口互踢）；状态一致性靠约定（成员状态回拨等是对 SDK 行为的补偿性修补）；共享记忆默认关、语义化共享上下文弱；断路器默认关、任务级失败重试靠 leader 的 review 回环（LLM 驱动）无确定性重试；聚合只有 leader 汇总单模式；A2X 注册中心需独立部署，分布式落地门槛高。

#### 4.4 agent-core-java：agentteams 框架级团队协同

##### 4.4.1 整体架构

Java 17 Maven 项目（约 2261 个文件），是 Python openjiuwen agent-core 的**对齐移植**（大量类注释明示 "mirrors Python's xxx"）。agent 体系四层：L4 多智能体编排（**两套并存**：`core.multiagent` 轻量 Team 门面 Handoff/Hierarchical；`agentteams` 全功能 Leader-Teammate 框架≈swarm）、L3 单 agent 内核（BaseAgent/ReActAgent/AbilityManager/Rail + DeepAgent）、L2 运行时与资源（Runner/ResourceMgr/spawn、Session、ContextEngine）、L1 基础设施（Model/Tool/memory/retrieval）。**没有独立的 Planner/Executor 抽象**——规划与执行合并在 ReAct 循环里，多 agent 编排靠三种手段：显式路由规则（handoff 路由图）、agents-as-tools、事件驱动+任务看板（agentteams）。

核心组件：`TeamAgent`（`agentteams/agent/TeamAgent.java`，每成员一个实例，聚合 Messager/TeamBackend/CoordinationManager/RecoveryManager/SpawnManager/StreamController/EventDispatcher/CoordinatorLoop）、`CoordinatorLoop`（单线程事件循环+周期轮询，"只唤醒、不决策"）、`TeamBackend`（成员/消息/任务数据中枢）、`TeamTools`（团队工具集按角色裁剪）、`TeamRail`（向成员 DeepAgent 注入团队 prompt 段）。

##### 4.4.2 发起机制与角色裁剪

- `LeaderTeammateAgentTeam.builder()` 声明式构建：lifecycle（temporary|persistent）/teammateMode/spawnMode（inprocess|process）/transport（inprocess|pyzmq）/storage（sqlite|memory|postgresql|mysql）/teamMode（default|predefined|hybrid）/预定义成员；两段式 build 先生成 `TeamAgentSpec` 再创建 TeamAgent。
- 角色四元 LEADER/MEMBER/HUMAN_AGENT/USER；状态机 `MemberStatus`（UNSTARTED→READY→BUSY→SHUTDOWN_REQUESTED→SHUTDOWN，异常走 RESTARTING/ERROR）与 `ExecutionStatus`（IDLE→STARTING→RUNNING→COMPLETING→COMPLETED+CANCEL/FAILED/TIMED_OUT），枚举自带 `canTransitionTo` 校验。
- **团队工具按角色裁剪**：LEADER_ONLY={build_team, clean_team, spawn_member, shutdown_member, approve_plan, approve_tool, create_task, update_task, list_members}；MEMBER_ONLY={claim_task, enter_worktree, exit_worktree}；SHARED={view_task, send_message, workspace_meta}；HUMAN_AGENT 仅 send_message。
- 任务认领防囤积：**一个成员同时只能持有一个 claimed 任务**，DB 层 claim 做状态前置校验（失败视为并发争抢）；`clean_team` 有守卫（存在未完成/被取消任务时拒绝清理）。

##### 4.4.3 并发与调度模型

- **每成员一个单线程事件循环**（CoordinatorLoop：LinkedBlockingQueue+单 daemon 线程，事件逐个交给 EventDispatcher；两个周期任务每 30s 各入队 POLL_MAILBOX/POLL_TASK；异常被吞掉保持 loop 存活）；成员在共享 cached thread pool 上跑；事件队列即天然串行化（每成员同时只有一个 in-flight round 防重入，round 中到达的任务事件直接丢弃）。
- spawn：`inprocess` 模式**新建一个 TeamAgent 作为成员宿主**提交线程池，**只共享 DB 句柄**（shareDb；messageManager/taskManager 保持成员独立以保证身份正确），成员进入自己的协调循环**阻塞直到被 shutdown**；`process` 模式经 `Runner.spawnAgent` 起子进程、对端 `TeamAgent.fromSpawnPayload` 重建。
- 健康检查：默认 50s 间隔，不健康触发清理+重启（最多 3 次，指数退避 2/4/8s），广播 member_restarted。
- 兜底调度：stale claimed 任务 60s 催办/自动完成；stale pending 任务 10min 提醒 leader；**广播时自动拉起所有 UNSTARTED 成员**（单播给未启动成员也触发 auto-launch）。
- ReAct 单轮内多个 tool call **串行**执行（无并行 tool call）；多成员并行靠"每成员一线程"。

##### 4.4.4 上下文与隔离

- LLM 上下文每 agent 独立（ContextEngine/ModelContext，带 token 计数、压缩/卸载处理器）；`TeamRail` 按 section 注入 team_role/team_workflow/team_lifecycle/team_persona/team_info/team_members 等 prompt 段；
- 会话传播：`SpawnContext`（InheritableThreadLocal 传 sessionId，executor 任务开头显式 set/reset 防线程池复用串扰）；
- 成员间隔离：独立 TeamAgent/DeepAgent/TeamBackend/Messager 订阅，仅共享 TeamDatabase 句柄；handoff 模式每跳新建 session 防上下文污染；AgentAsTool 子会话 id=`父sessionId:toolCallId`；
- 文件隔离：团队 workspace 按 team 分目录 + `WorkspaceFileLock` 文件锁 + 冲突策略；编码场景 `WorktreeManager` 为成员建独立 **git worktree** 物理隔离；
- 工具 ID 全局唯一化：`qualifyTeamToolIds` 把工具 card id 改为 `id.team.member`，避免全局 ResourceMgr 互相覆盖。

##### 4.4.5 结果回传与聚合（框架级，非 LLM 自觉）

- `TeamResultCollector`：静态 ConcurrentHashMap\<team, Map\<member, content\>\>，成员给 leader 发 send_message 时**框架捕获内容**——注释明说 "replaces the unreliable 'LLM checks its own inbox' pattern with guaranteed framework-driven delivery"；
- `tryAutoCompleteMemberTasks`：成员轮次结束回调里，只要成员给 leader 发过消息就**自动 claim+complete 其名下未完成任务**，防止 leader 汇总被永远阻塞；
- 消息双轨：先落库再总线 publish（"DB 持久化+总线通知"，成员靠轮询/事件触发处理未读）；`member_results_delivery` 事件做多阶段任务的定向投递（静态去重集合保证每阶段全团队恰好投递一次）。

##### 4.4.6 取消 / 超时 / 权限 / 恢复

- 取消：用户侧 `cancelAgent()` 置标志位+中断当前 round；远程取消经 `member_canceled`/`member_shutdown` 事件；stream iterator 关闭即 interrupt 后台 LLM 线程。**取消粒度粗**：标志位+线程 interrupt，无结构化 deadline 传播链。
- 超时与兜底（配置化常量表）：TeamRuntime 调用深度上限 64 跳（防循环）、handoff 最大移交 10 次、mailbox/task 轮询 30s、stale claimed 60s、stale pending 10min、spawn 健康检查 50s、成员重启 3 次退避、流错误重试 10 次（仅特定错误码）、ReAct maxIterations。注意 `GroupConfig.maxConcurrentMessages/messageTimeout` **配置存在但执行层未兑现**。
- 权限：工具面按角色裁剪；teamMode=predefined 时移除 spawn_member；危险操作走审批（`TeamToolApprovalRail` 产生 interrupt → leader approve_tool → `tool_approval_result` 事件 → resumeInterrupt 恢复被中断的工具调用）。
- 恢复：RecoveryManager（recoverTeam/persistLeaderConfig 等）；`TeamAgent.snapshot()/restoreFromSnapshot`；handoff orchestrator 状态存 session state 可续接。

##### 4.4.7 技术难点与解法（该项目实证）

1. Java 无 mixin/多继承 → CommunicableAgent 改基类、TeamAgent"宿主+组合"；
2. 全局 ResourceMgr 工具 ID 冲突 → 工具 ID 加 `team.member` 后缀；
3. 消息回声（自己收自己的事件） → 每成员 nodeId=memberName+senderId 过滤；
4. 成员只共享数据不共享身份 → shareDb 只共享 DB 句柄；
5. **LLM 不可靠行为兜底** → 忘记 complete_task 则轮末自动完成、"LLM 自查 inbox"不可靠则 TeamResultCollector 框架级捕获+定向投递、成员卡住则 stale 催办+健康检查重启；
6. handoff 信号序列化差异（Python dict repr vs Java Map.toString） → 手写解析器兼容；
7. 跨线程 session 传播 → InheritableThreadLocal+显式 set/reset；
8. 多阶段任务数据流 → member_results_delivery 事件+静态去重集合。

##### 4.4.8 局限取舍

TeamRuntime 同步阻塞（send/publish 全部同步 invoke，无异步/背压/超时，LinkedHashMap 非线程安全）；**跨进程能力半成品**（PyZmqMessager 实为 JVM 内静态 Map 仿真，跨进程消息流未真正打通）；工具调用串行；静态全局状态泄漏风险（同 JVM 多团队相互污染需手工清理）；错误处理偏"吞掉保活"（故障可见性依赖日志）；文档与代码漂移；大量"窄切片"移植妥协（如 invokeForSpawn 结束后 `Thread.sleep(Long.MAX_VALUE)` 保持线程存活）；任务级超时只有状态枚举未见强制执行点。

### 5. 实现差异对比矩阵

下表按 swarm 设计关键维度横向对比四个参考项目（依据 §4 各节证据）：

| 维度 | kimi-code | codex | jiuwenswarm | agent-core-java |
|---|---|---|---|---|
| **子 agent 发起方式** | `AgentSwarm` 工具：prompt_template+{{item}}+items **批量模板**一次派发（≤128），resume_agent_ids 混合续跑；`Agent` 工具单个 spawn | **spawn 工具单个派发**：V1 `spawn_agent(message|items)`；V2 `spawn_agent(task_name)` 层级路径寻址 | **leader 动态组队**：leader LLM 调 `build_team`/`spawn_teammate` 逐个拉起；SwarmFlow 按 Phase 批量 | leader 调 `spawn_member` 逐个拉起（落库 UNSTARTED，消息触发 auto-launch）；也支持预定义成员静态组队 |
| **并发上限与限流** | ramp（5 起步+700ms/个）默认不封顶，env 可设硬上限；429 后容量状态机（收缩/3min 静默恢复/插回队首/同 agent 重试） | **三道闸门**：总数 CAS（默认 6 或 4）+ 同时执行信号量（RAII guard）+ 驻留 LRU（可卸载冷加载） | **三层闸门**：L1 max_workflows / L2 agents_per_run / L3 max_agents_total 全局准入 | 每成员单线程事件循环天然串行化；成员数受线程池限制；`maxConcurrentMessages` 配置未兑现（无限流实现） |
| **上下文隔离** | **独立 homedir+独立 wire.jsonl**；父子不共享 context，父只见子最后一条 assistant 文本；共享会话级服务（fs 视图/MCP/hooks） | **会话级隔离**（独立历史/rollout/context window）；同 cwd 同 sandbox **无文件隔离**；可选过滤式 fork 继承浓缩父上下文 | 成员独立 DeepAgent 上下文；共享存储+团队工作区（符号链接视图按需暴露）；团队共享记忆默认关 | 每成员独立 TeamAgent/DeepAgent/ModelContext，仅共享 DB 句柄；handoff 每跳新建 session；worktree 物理隔离（编码场景） |
| **结果聚合** | **前台 XML 按序占位聚合**（`<agent_swarm_result>`），阻塞到全部结束；后台任务经通知 XML steer 注入；短 summary 自动续问兜底 | **mailbox 通知**（V2 完成邮件 queue-only 回父）；**聚合权全在父模型**，框架不做合并 | **leader 汇总单模式**（LLM 行为）+ review/verify 验收回环；前端增量 delta | **TeamResultCollector 框架级捕获**（成员发消息即被框架收集）+ 轮末自动 complete 任务兜底；聚合仍由 leader LLM 完成 |
| **取消与超时** | 取消树递归（先孙后子），原因沿 signal 传递；每任务独立超时（默认 2h，可配，0=无超时）；用户取消保留已完成结果 | `Op::Interrupt`+CancellationToken 层级；close 递归关整棵子树；**执行无超时**，靠父 interrupt+整树共享 token 预算兜底 | 生命周期四操作过锁串行化，cancel 可抢占 pause；成员 completion_timeout 默认 600s、max_iterations 200；分布式 destroy envelope | cancelAgent 标志位+线程 interrupt；member_canceled/shutdown 事件传播；任务级超时只有状态枚举无强制；深度上限 64 跳防循环 |
| **权限继承** | 子独立 PermissionManager **挂父链**（mode/审批记忆合并，ask 冒泡带 agentId）；profile 工具白名单+委派白名单双重裁剪；AgentSwarm 独占响应硬策略 | 子继承父 live turn 的 approval/sandbox/cwd（运行时活体值）；角色=config layer 可锁模型；delegate 子审批路由回父 | `enable_permissions` 全局∧团队；审批只经 leader 路由到前端；技能链接按成员配置暴露 | **工具面按角色硬裁剪**（LEADER_ONLY/MEMBER_ONLY/SHARED/HUMAN）；危险操作 TeamToolApprovalRail→leader approve→事件恢复 |
| **跨进程能力** | 无（同进程；子 agent 是同进程内独立实例） | 无（同进程 tokio 任务；cloud-tasks 是另一套分离机制） | **完整分布式控制面**：pyzmq transport + 共享 PG + A2X 注册中心（预约/bootstrap/ACK/destroy）；声明式蓝图+seed 跨进程重建 | **半成品**：spawnMode=process 可起子进程+fromSpawnPayload 重建，但 Messager 跨进程是 JVM 内仿真，消息流未打通 |
| **对模型的依赖程度** | 提示词约束为主（enter-reminder 教编排、"不做主活"无代码强制）+ 少量机制强制（独占响应/128 上限/prompt 去重/类型白名单） | **提示词约束为主**（工具描述写委派方法论、角色描述写协作纪律、MultiAgentMode 指令热切换）；机制强制深度门控（V1）与并发闸门 | **提示词+框架各半**：组队/拆解/汇总靠 leader LLM；任务状态机/成员状态机/生命周期锁由框架管理 | **机制强制偏多**：角色工具裁剪、任务认领防囤积、结果框架级捕获、自动 complete 兜底——明确不信任 LLM 自觉 |

**矩阵点评**：

- **发起方式**上，kimi-code 的"批量模板一次派发"最贴合"父 agent 运行时动态 fan-out N 个同构子任务"的 swarm 本义；codex 的单个 spawn+层级寻址更适合"长期存活、可互发消息"的协作网络；jiuwenswarm/agent-core-java 的 leader 组队介于两者之间（动态但逐个）。AgentFlow 一期应优先吸收 kimi-code 的批量模板语义（与 `next_skills` 批量格式天然同构）。
- **限流**上三家成熟方案一致指向"初始并发保守+动态调整"：kimi-code 是限流驱动的容量状态机，codex 是静态三级闸门，jiuwenswarm 是配置化三层准入。AgentFlow 已有 `max_concurrent_skills` 单点限流，需扩展为"ramp+限流恢复"组合。
- **隔离**的谱系从强到弱：kimi-code（独立存储+独立上下文）> agent-core-java（独立实例+共享 DB）> codex（会话隔离+共享工作区）> jiuwenswarm（共享工作区为主）。隔离越强，上下文继承越要显式契约（kimi-code 完全不继承、codex 过滤式 fork 是两种可借鉴端点）。
- **聚合**的核心分歧是"框架帮多少"：codex 全交给父模型，agent-core-java 框架级捕获最激进（明确不信任 LLM 自查 inbox），kimi-code 居中（框架按序占位渲染、父模型消费）。AgentFlow 的 coordinator 本身就是 workflow，适合"框架做结构化槽位聚合、coordinator workflow 做语义汇总"的分工。
- **模型依赖**维度对 AgentFlow 最有参考价值：AgentFlow 的协同拓扑是配置/workflow 驱动而非纯 LLM 驱动，天然更接近 agent-core-java 的"机制强制"一侧，可把更多可靠性放进框架（状态机、计数、自动兜底），把更少的自由度留给模型。

### 6. 关键技术难点归纳（跨项目共性）

以下 7 个难点在至少两个参考项目中以不同形式出现，是 swarm 设计的共性核心问题；每个难点列出各项目解法对照，作为 §8 方案设计的选型依据。

#### 6.1 批量派发的限流与背压

难点：批量 fan-out 瞬间放大下游压力（provider 配额、worker 线程、内存），需要"启动不过冲、超限能收缩、恢复可渐进"。

| 项目 | 解法 |
|---|---|
| kimi-code | ramp（5+700ms）+ 可选硬上限 + 429 驱动的容量状态机（容量=已成功数、再限流缩 1、3min 静默+1）+ 被限流任务插回队首、同 agent 重试复用上下文 |
| codex | 静态三级闸门：总数 CAS / 同时执行信号量（turn 启动前获取、RAII 释放）/ 驻留 LRU 卸载冷加载 |
| jiuwenswarm | 配置化三层准入闸门（workflow 数/单 run agent 数/全局 agent 数） |
| agent-core-java | 无真正限流（每成员单循环串行化兜底；配置项未兑现）——反例 |

启示：**ramp 起步+超限收缩+静默恢复**是被验证的动态策略；静态闸门（总量/执行/驻留）是正交的另一层。两者可叠加。

#### 6.2 handoff 结果质量（子→父的信息断层）

难点：父 agent 只能拿到子任务的"最终陈述"，子任务一句话交差会造成信息断层。

| 项目 | 解法 |
|---|---|
| kimi-code | profile 提示词明示"final message is the entire handoff"；<200 字符自动续问一轮（有界 1 次）；max_tokens 截断直接判失败 |
| codex | 完成邮件内含"如还需要它，用协作工具再派任务"的引导；错误截断到 1000 token |
| agent-core-java | TeamResultCollector 框架级捕获全部汇报内容；轮末自动 complete 防汇总阻塞 |
| jiuwenswarm | leader review/verify 回环（submitted_for_review→verified/revision_requested）验收 |

启示：提示词约定（"最后消息即交付"）+ 框架质量兜底（过短续问/截断判败）+ 可选验收回环，三层可独立采用。

#### 6.3 上下文隔离与继承的边界

难点：子任务上下文完全独立则重复劳动（父已掌握的信息要重给），完全共享则相互污染。

| 项目 | 解法 |
|---|---|
| kimi-code | 默认完全隔离（独立 wire），输入只有 profile 提示词+父下发 prompt；例外（btw）显式共享投影 |
| codex | 过滤式 fork：保骨架（system/user/final_answer）去过程（reasoning/tool call），可全量/最近 N 轮/不 fork；文本级清洗指令 |
| agent-core-java | 成员独立上下文+TeamRail 注入团队关系段；handoff 每跳新建 session 防污染 |
| jiuwenswarm | 共享工作区+符号链接视图按需暴露；团队共享记忆（默认关） |

启示：**默认隔离+显式继承契约**是主流；codex 的"过滤式 fork"证明继承必须带过滤规则而非全量拷贝。

#### 6.4 取消级联与语义分层

难点：父取消要级联到整棵子树；用户取消≠超时≠系统 abort，语义要分层（已完成的保留、未启动的标记可恢复）。

| 项目 | 解法 |
|---|---|
| kimi-code | 递归先孙后子；原因对象沿 signal 传递（在途工具报告"用户主动中断"）；已完成保留/已启动 aborted/未启动 not_started |
| codex | Op::Interrupt+CancellationToken 层级；close_agent 递归关整棵子树；RAII 名额预留回滚 |
| jiuwenswarm | 生命周期锁串行化四操作；cancel 可抢占 pause；分布式 destroy envelope |
| agent-core-java | 标志位+事件传播（member_canceled/member_shutdown）；粒度粗 |

启示：级联靠"父子注册表+递归遍历"，语义分层靠"取消原因随信号传递+槽位分态标记"。

#### 6.5 生命周期与恢复

难点：子 agent 是动态创建的，其元数据必须可持久化、可恢复（进程重启、限流重试、显式 resume），且部分失败不能泄漏名额。

| 项目 | 解法 |
|---|---|
| kimi-code | 每 agent 独立 wire.jsonl 懒重放；元数据（含 swarmItem）持久化进 session metadata；父链递归恢复+环检测；ensureIdleSubagent 防并发 resume |
| codex | spawn 边持久化 agent-graph-store，崩溃后 BFS 恢复整树；SpawnReservation RAII 防名额泄漏；InternalAgentDied 自动清理；驻留 LRU 卸载后可冷加载 |
| jiuwenswarm | Runner 池内驻留可恢复；辅助实例用完即弃防泄漏；分布式预约 TTL+失败 release |
| agent-core-java | snapshot/restoreFromSnapshot；健康检查+重启（3 次退避）；静态全局状态需手工清理（反例） |

启示：动态实例的**元数据先行持久化**（id、父链、输入、状态）是一切恢复能力的前提；名额/资源预留要 RAII 化。

#### 6.6 护栏防循环

难点：动态协同放大循环风险（agent 互相调用、handoff 打环、coordinator 无限再规划）。

| 项目 | 解法 |
|---|---|
| kimi-code | prompt 去重防同批重复；嵌套深度靠 profile 委派白名单间接控制（无硬上限——弱） |
| codex | V1 深度门控（默认 1，子 agent 看不到 spawn 工具——机制强制）；共享 token 预算超限熔断 |
| agent-core-java | TeamRuntime 调用深度上限 64 跳；handoff 最大移交 10 次；任务认领防囤积；maxIterations |
| jiuwenswarm | llm_retry_rail（重复输出检测）+ circuit_breaker（ping-pong/无进展循环强制收尾，默认关）；max_iterations 200 |

启示：**深度上限+轮次上限+重复检测**三件套是底线；codex 的"深度超限则子 agent 根本看不到 spawn 工具"是最干净的机制强制（不可用即不可滥用）。

#### 6.7 "父 agent 不做主活"的约束手段

难点：swarm 模式下父 agent 应退居编排，但模型可能自己下场干活，使 swarm 形同虚设。

| 项目 | 解法 |
|---|---|
| kimi-code | enter-reminder 提示词（"do not handle the main work yourself"）+ 自动退出降残留；**承认无代码级强制**，只硬约束独占响应/上限/去重 |
| codex | orchestrator 模板（"你的角色是协调者"）+ 每 turn 注入活 agent 列表引导委派；MultiAgentMode 指令热切换 |
| jiuwenswarm | 角色二元化（leader 工具集里没有执行类工具的可选裁剪）+ leader 人设配置 |
| agent-core-java | **角色工具硬裁剪**（LEADER_ONLY/MEMBER_ONLY 工具集不重叠）——leader 物理上没有成员的干活工具 |

启示：纯提示词约束（kimi-code/codex）已被证明"够用但不保证"；agent-core-java 的"工具集不重叠"是最强约束。AgentFlow 的 skill 白名单机制（`candidate_skills`）天然支持后者。

### 7. AgentFlow 现状与差距分析

本节基于对 `main` 分支（e8ffbf0）的实证分析。核心结论：**现有 dispatcher-coordinator-skill 模型已实现"session 内静态拓扑的 fan-out/fan-in"，swarm 所需的"并行子任务派发+计数聚合"骨架已存在且运行良好；真正的缺口在"跨 agent 动态性"——动态创建子 agent/子 session、跨 agent 数据面隔离、父子生命周期级联、结构化聚合语义。**

#### 7.1 现有协同机制速览（代码实证）

协同三阶段（与 031 §4.3 一致，以下为代码落点）：

1. **分发**：`handle_task_intent_request`（`src/agent/agent.cpp:1227`）——dispatcher 生成 dialogue_id、写 `user_prompt`；有 workflow 时 `build_runtime_task`（`agent.cpp:1106-1225`）创建 runtime task 压入 agent 级 `generated_tasks_` 队列（`agent.cpp:1485-1490`），由 runtime 的 `agent_task_poller_loop`（`src/runtime/runtime.cpp:1872-2030`，20ms 轮询）取走、`ensure_workflow_pool` 预建实例后 `execute_workflow`（每次执行 `workflow_template->clone()`，`runtime.cpp:608`）。
2. **fan-out**：`handle_task_data_exchange`（`agent.cpp:1493`）在 dispatcher workflow 完成后调 `process_next_skills_launch`（`agent.cpp:2372-2609`）——读 `next_skills`（兼容 `selected_skills`）、`parse_next_skills`（`src/agent/builtin_dispatcher.cpp:385`，支持字符串数组/对象数组/逗号分隔三种格式）、`launched_skills` 去重（`agent.cpp:2417-2434`）、`force=true` 一次性重跑（`agent.cpp:2436-2496, 2572-2585`）、`launch_skill_task`（`agent.cpp:2056`）逐个拉起（参数槽位校验 `agent.cpp:2093-2237`；`pending_skills_count` +1 在 `agent.cpp:2323-2343`），受 `MMAgentTask::can_launch_more_skills()`（`src/agent/task.cpp:264`，`max_concurrent_skills` 默认 10）限流，未拉起的留待下次触发补拉（`agent.cpp:2539-2562`）。
3. **fan-in 与判定**：skill workflow 完成回投 data_exchange 消息 → skill 完成检测（`agent.cpp:1518-1537`）→ `pending_skills_count` -1（`agent.cpp:1547-1563` 与 `agent.cpp:1791-1808`）→ 结果消息写 coordinator 的 response 队列（`agent.cpp:1647` 或 `agent.cpp:2022-2035`）+ 投 `trigger_type=skill_completion` 触发消息（`agent.cpp:1566-1594`）→ coordinator 分支（`agent.cpp:1772-2001`）取结果、触发 coordinator workflow 判定 → 再规划（回到 fan-out）或 `pending==0` 且有 `response`/`multi_response` 时经 `SessionRouter::enqueue_response_message` 回包并 `cleanup_dialogue_data`（`agent.cpp:1869-1925, 1966-2000`）。

底层支撑：每个 agent 一个独立 MMDataBus 实例（`runtime.cpp:1724`）；session 级键前缀 `/session/{sid}/{namespace}/{key}`（namespace ∈ sys/user/ctx/llm/tool）、dialogue 级前缀 `/session/{sid}/sys/{dialogue_id}/{key}`（同 session 多 dialogue 并行隔离）；框架键三级回退读写 `BuiltinDispatcher::get_framework_data`（`builtin_dispatcher.cpp:498-537`）；node 输出按框架键约定回写（`runtime.cpp:1226-1237`）；消息定向路由（metadata 带 `dialogue_id+task_id` 直达目标 task 队列，`agent.cpp:918-959`，024 号增强）；AsyncCall `SendMsg` 跨 task 定向消息（`runtime.cpp:2604-2634`）；`SESSION-INTERRUPT`/`SESSION-DESTROY` 控制消息（`agent.cpp:1011-1104`）；FlowTrace 事件（`include/internal/runtime/flow_trace.h`、`runtime.cpp:1746-1822`）。

#### 7.2 可直接复用点清单

| 能力 | 复用点 | 位置 |
|---|---|---|
| 动态任务创建 | `build_runtime_task` + `session->add_task` + `generated_tasks_` → `agent_task_poller_loop` 派发链路，运行时可任意创建 task | `agent.cpp:1106,1444`、`runtime.cpp:1889` |
| 并行 fan-out | `process_next_skills_launch`：批量拉起、去重、force 重跑、限流、超限补拉——skill 即"子任务"，语义可提升为"子 agent 任务" | `agent.cpp:2372-2609` |
| fan-in 计数与聚合触发 | `pending_skills_count` 增减 + skill 完成自动触发 coordinator | `agent.cpp:1540-1594, 1791-1837, 2323-2343` |
| 结果聚合落点 | coordinator workflow + response/multi_response 回包 + `aggregate_session_results`（跨 session 聚合雏形，主路径未调用） | `agent.cpp:1846-2000`、`src/agent/coordinator.cpp:154-170` |
| 子任务上下文隔离 | dialogue_id 键前缀（并行隔离）+ workflow clone（执行隔离）+ 输入经 task parameters/inputs 注入 | `runtime.cpp:608,794-822` |
| 父子消息通道 | 定向路由（`dialogue_id+task_id` 直达）+ AsyncCall `SendMsg` | `agent.cpp:918-959`、`src/runtime/async_call.cpp:69-102`、`runtime.cpp:2604` |
| 子会话动态创建 | `SessionRouter` session factory（UUID 按需创建）+ `target_session_id` 元数据雏形（`build_runtime_task` 已识别并拼入 task_id） | `agent.cpp:565-613, 1151-1154`、`src/agent/session_router.cpp:218-228` |
| 多 agent 托管 | `agentflowd AgentRegistry`（load/unload/find）+ `MMAgentRuntime::register_agent`（共享 scheduler/deployer/worker 池） | `src/daemon/agent_registry.cpp:17-81`、`runtime.cpp:374` |
| 跨进程数据面 | RemoteDatabus（Redis）+ UDS + data_ref 大对象引用 | `src/runtime/databus/databus.cpp`、`uds_server.cpp` |
| 权限白名单雏形 | `candidate_skills` 限制可选 skill 范围；skill 参数槽位校验 | `builtin_dispatcher.cpp:265-304`、`agent.cpp:2093-2237` |
| 可观测 | FlowTrace 事件 + session/dialogue/task 三级生命周期回调 | `flow_trace.h`、`runtime.cpp:1746-1822` |

#### 7.3 缺口清单（G1-G9）

| 缺口 | 现状证据 | 对 swarm 的影响 |
|---|---|---|
| **G1 跨 agent 寻址与消息路由** | `send_async_message` 仅按 session_id 反查 agent，无 agent_id 寻址（`runtime.cpp:2614-2623`）；agent 间无引用、无协议 | 父 agent 无法向"另一个 agent"派任务；需 agent 级 mailbox 或路由键扩展（`agent_id+session_id`）——**二期阻塞项** |
| **G2 跨 agent 数据面隔离** | 每 agent 独立本地 MMDataBus（`runtime.cpp:1724`）；`remove_by_prefix` 只清本地（`databus.cpp:68-70`） | 父子 agent 默认互不可见数据；需"共享域"约定（Redis 前缀/父 bus 引用传递）与上下文继承契约——**二期阻塞项** |
| **G3 动态子 agent/子 session 生命周期** | skill 模板必须 agent.json 预声明（`launch_skill_task` 找不到模板即失败，`agent.cpp:2074-2088`）；session factory 无 parent 指针、无兄弟索引；agent 包只能 Registry 显式 load | 无法"运行时按任务动态 spawn 子 agent"；031 §7.3 待实现 7（`spawn_sub_session`）正是此缺口的既有规划——**一期需部分解决（动态模板注册）** |
| **G4 父子级联控制** | `SESSION-INTERRUPT/DESTROY` 只作用于本 session（`agent.cpp:1011-1104`）；无取消树、无子任务超时 | 父取消无法级联；需 parent→children 注册表与级联 interrupt |
| **G5 结构化聚合原语** | fan-in 只有计数器+键约定；无 future/promise 式等待句柄；`execute_workflow` fire-and-forget；无部分失败策略（N 选 M、best-effort） | 聚合逻辑全压在 coordinator workflow，框架不提供帮助；需 SwarmHandle/join 语义与失败策略枚举——**一期核心新增** |
| **G6 结果聚合通知的可组合性** | 子任务完成通知写死为"投给 source_task/coordinator"（`agent.cpp:1566-1594, 2017-2051`）；`coordinator_task_id` 单值参数 | 多级 swarm（子再有子）需通知链/事件订阅；一期单级可接受，二期需泛化 |
| **G7 护栏** | 无 max_coordination_rounds、无循环检测、无 dialogue 总超时（031 §4.5/§6.6 已列方案未实施） | swarm 的 fan-out 放大循环风险，**护栏应先于 swarm 落地** |
| **G8 跨进程控制面** | Master/Worker 同进程直调（035 未实施）；worker 不可独立部署 | 子 agent 独立进程/机需先做 035 或走"另一 agentflowd+Redis/UDS 数据面+自定义控制消息"弱集成——**二期以后** |
| **G9 权限边界** | 仅 session 内 `candidate_skills` 白名单；无 agent 级能力/数据权限模型 | 子 agent 可调什么 skill、可见哪些 DataBus 域，需新策略层 |

#### 7.4 差距结论与路径选择

- **session 内 swarm（一期）不依赖 G1/G2/G8**：把"子 agent 任务"建模为 skill task 的推广即可复用 fan-out/fan-in 全链路；必须补的是 G3 的"动态模板/动态子任务注册"、G5 的"结构化聚合原语"、G7 的"护栏"，以及 G4 的"swarm 内级联"（同 session 内，范围可控）。
- **跨 agent swarm（二期）** 必须先补 G1（agent 寻址，机制上只是 `send_async_message` 扩展 `target_agent_id` + runtime `agents_` 表查找）与 G2（共享域约定）；G4/G6 决定可用性下限；G8 决定能否跨进程。
- 天然挂载点：`launch_skill_task`（`agent.cpp:2056`）是 fan-out 挂载点；`handle_task_data_exchange` 的 coordinator 分支（`agent.cpp:1772`）是聚合器挂载点；`AgentRegistry`/`MMAgentRuntime::agents_` 是子 agent 注册与寻址挂载点。

### 8. Swarm 架构设计方案

#### 8.1 总体架构

新增组件 **SwarmManager**（每 agent 一个实例，隶属 MMAgent），负责 swarm 计划的解析与展开、子任务槽位状态机、批量调度（ramp+限流）、结构化聚合与级联控制。一期全部落在 session 内；二期经路由扩展跨 agent。

```
┌────────────────────────────────── agentflowd ──────────────────────────────────┐
│  AgentRegistry（多 agent 托管，二期：target_agent_id 寻址）                      │
│                                                                                 │
│  ┌──────────────────────── 父 agent (MMAgent) ────────────────────────────┐    │
│  │                                                                         │    │
│  │   MMAgentSession（父 session）                                          │    │
│  │   ┌──────────────┐   next_skills /    ┌──────────────────────────┐      │    │
│  │   │ dispatcher    │──swarm 计划──────▶│  SwarmManager（新增）      │      │    │
│  │   │ task          │                   │  ・SwarmTaskSpec 解析/展开 │      │    │
│  │   └──────────────┘                   │  ・槽位状态机 slots[]      │      │    │
│  │   ┌──────────────┐   聚合完成通知     │  ・ramp/限流调度器         │      │    │
│  │   │ coordinator   │◀─────────────────│  ・SwarmHandle 聚合       │      │    │
│  │   │ task          │                   │  ・级联取消树/护栏计数      │      │    │
│  │   └──────────────┘                   └───────────┬──────────────┘      │    │
│  │        ▲  trigger: swarm_progress                │ launch（复用        │    │
│  │        │  skill 结果（response 队列）             │ launch_skill_task） │    │
│  │   ┌────┴───────────────────────────────────────▼───────────────┐       │    │
│  │   │  子任务 skill tasks（swarm item 实例，每 item 一个            │       │    │
│  │   │  runtime task + 独立 dialogue 子前缀）fan-out/fan-in         │       │    │
│  │   └────────────────────────────────────────────────────────────┘       │    │
│  │                                                                         │    │
│  │   MMDataBus（本 agent）：/session/{sid}/sys/{dlg}/swarm/{swarm_id}/...   │    │
│  └─────────────────────────────────────────────────────────────────────────┘    │
│            │ （二期）target_agent_id 路由 + 共享域键约定 + 级联 interrupt        │
│  ┌────────▼─────────────── 子 agent (MMAgent) ────────────────────────────┐    │
│  │   子 session（父链 parent_swarm_ref）→ 子 agent 自己的 dispatcher/       │    │
│  │   coordinator/skill 三层，可再嵌套 swarm（深度受护栏限制）                │    │
│  │   MMDataBus（子 agent）＋ 共享域（Redis 前缀 /session/{sid}/shared/...） │    │
│  └─────────────────────────────────────────────────────────────────────────┘    │
│                                                                                 │
│  Runtime 层：agent_task_poller_loop / workflow clone 并行执行 / worker 池        │
│  （对 swarm 透明，复用现有派发与执行隔离能力）                                    │
└─────────────────────────────────────────────────────────────────────────────────┘
```

父子 swarm 会话树（二期视角）：

```
父 agent A / session S / dialogue D
 └── swarm_1（SwarmManager 管理，SwarmHandle 聚合）
      ├─ item[1] ─ 一期：S 内子 dialogue D/swarm_1/items/1 ｜ 二期：子 agent B 的子 session S₁
      ├─ item[2] ─ ...                                  ｜     子 agent C 的子 session S₂
      └─ item[3] ─ ...（每个子会话可再发起自己的 swarm，深度 ≤ max_swarm_depth）
```

#### 8.2 核心概念定义

| 概念 | 定义 | 落地形态 |
|---|---|---|
| **SwarmTaskSpec** | 一次 swarm 批量派发的声明式描述：`{swarm_id, prompt_template, items[], item_skill(目标 skill/模板), failure_policy, max_concurrency, item_timeout_ms, inherit_keys[], allowed_skills[]}` | 由 coordinator/dispatcher workflow 写入 DataBus 扩展 `next_skills` 格式（见 8.3.2），SwarmManager 解析 |
| **SwarmItem（子任务）** | `prompt_template` 中 `{{item}}` 占位符被 items[i] 字面替换后的一个可执行单元；带 `swarm_index`（1 起始连续编号）与原始 item 文本 | 一期 = 父 session 内一个动态注册的 swarm skill runtime task（独立 dialogue 子前缀）；二期 = 子 agent 上一个子 session 的用户请求 |
| **槽位（slot）** | 每个 SwarmItem 在聚合侧的固定位置，承载状态机与结果引用；**结果按输入序占位**，单 item 失败只占自己的槽位 | DataBus 键 `/session/{sid}/sys/{dlg}/swarm/{swarm_id}/slots/{i}/{status,result_ref,error}`（见 8.6） |
| **SwarmHandle（聚合句柄）** | swarm 批次的生命周期句柄：`{swarm_id, total, succeeded, failed, cancelled, slots[]}`，提供 join（等全部终态）与部分失败策略判定 | SwarmManager 内存对象 + DataBus 镜像（`/swarm/{swarm_id}/summary`）；coordinator workflow 经框架键读取 |
| **子 agent 会话** | 子任务的执行上下文载体 | 一期：同 session 的子 dialogue 命名空间（零新对象）；二期：子 agent 上的独立 session（带 parent_swarm_ref 父链元数据） |
| **SwarmManager** | 每 agent 一个的 swarm 控制面组件 | 新类 `SwarmManager`，作为 `MMAgent` 成员（与 dispatcher/coordinator 逻辑同级） |

概念映射参考：SwarmTaskSpec ↔ kimi-code 的 AgentSwarm 工具入参 / jiuwenswarm 的 TeamAgentSpec 子集；SwarmItem ↔ kimi-code 的 QueuedSubagentTask；slot ↔ kimi-code 的按序占位结果槽；SwarmHandle ↔ codex 的 status watch 订阅 + agent-core-java 的 TeamResultCollector 的句柄化。

#### 8.3 一期：session 内 swarm（最短路径）

##### 8.3.1 设计要点

把"子 agent 任务"建模为 **skill task 的推广**：一个 swarm 批次 = 一个动态注册的 swarm 模板 + N 个 item 实例。完全复用现有 fan-out/fan-in 链路，新增的只有"批量模板展开、槽位聚合、ramp 调度"三件事。与 031 §7.3 待实现 7（spawn_sub_session）对齐但更小：一期甚至不新建 session，用 dialogue 子前缀隔离。

**动态模板注册（解 G3 的一期部分）**：允许 workflow 在运行时注册"swarm 临时模板"——本质是复用某个已声明 skill 的执行定义（workflow_file + parameters schema），但以 swarm 命名空间实例化，不受"skill 模板必须 agent.json 预声明且名字固定"的限制。若 `agent.json` 已预声明了目标 skill，则零模板改动，仅需 `next_skills` 格式扩展。

##### 8.3.2 swarm 计划的 DataBus 契约（next_skills 扩展格式）

coordinator/dispatcher workflow 在现有 `next_skills` 键中写扩展对象格式（兼容 023 号格式扩展的解析路径，`builtin_dispatcher.cpp:385` 的 `parse_next_skills` 增加第四种格式分支）：

```json
{
  "swarm": {
    "swarm_id": "sw_01",
    "item_skill": "code_review",
    "prompt_template": "审查文件 {{item}} 并输出问题清单",
    "items": ["src/a.cpp", "src/b.cpp", "src/c.cpp"],
    "failure_policy": "best_effort",
    "max_concurrency": 4,
    "item_timeout_ms": 120000,
    "inherit_keys": ["user_prompt", "repo_root"],
    "allowed_skills": ["code_review"]
  }
}
```

解析校验（fail-fast，借鉴 kimi-code 的启动前硬校验）：items 数 ≥1 且 ≤ `max_swarm_items`（默认 32，agent.json 可配）；prompt_template 必含 `{{item}}`；展开后 prompt 两两不同；`item_skill` 必须在 `candidate_skills`/`allowed_skills` 白名单内；嵌套深度 ≤ `max_swarm_depth`。

##### 8.3.3 交互流程（一期时序）

```
[父 session，dialogue D]
1. coordinator workflow（或 dispatcher）判定需要批量并行 → 写 swarm 计划到 next_skills
2. handle_task_data_exchange → process_next_skills_launch（agent.cpp:2372）
   识别 swarm 格式 → SwarmManager::submit_swarm(spec)
3. SwarmManager：
   a. 展开 items → N 个 SwarmItem（prompt 字面替换，swarm_index 编号）
   b. 在 DataBus 建槽位：/swarm/{swarm_id}/slots/{i}/status=QUEUED，写 summary{total=N,...}
   c. 注册 SwarmHandle；把 swarm 纳入本 dialogue 的 pending 体系（pending_skills_count += N，
      复用 agent.cpp:2323-2343 的计数通路，保证 fan-in 主链路不变）
4. ramp 调度器（见 8.5）按并发闸门逐个 launch_item：
   复用 launch_skill_task（agent.cpp:2056）拉起 item 的 runtime task，
   task parameters 注入 {swarm_id, swarm_index, item_text, expanded_prompt}，
   目标 dialogue 前缀 = {D}/swarm/{swarm_id}/items/{i}（见 8.7）
   → FlowTrace: swarm.item_launched
5. 每个 item 的 skill workflow 并行执行（workflow clone，runtime.cpp:608 保证隔离）；
   item 完成 → 现有 skill 完成检测路径（agent.cpp:1518-1537）
   → SwarmManager::on_item_finished 捕获（参数带 swarm_id 即路由到聚合器）：
   写 slots/{i}/{status,result_ref}，summary 计数更新 → FlowTrace: swarm.item_completed/failed
6. 部分失败策略在槽位更新时判定（见 8.6）：全部终态（或策略提前满足/判死）后：
   SwarmManager 渲染结构化聚合文本 → 写 /swarm/{swarm_id}/aggregate
   → 向 coordinator 投 trigger_type=swarm_completed 触发消息（复用 agent.cpp:1566-1594 通路）
   → FlowTrace: swarm.completed
7. coordinator workflow 读 aggregate（含每槽位状态与结果引用），做语义汇总/再规划：
   - 可写新的 swarm 计划（下一批）→ 回到 1（轮次受护栏限制）
   - 或产出 response 正常收尾（复用 agent.cpp:1869-1925）
8. 对话结束 cleanup：清理 /swarm/ 前缀（并入 cleanup_dialogue_data 路径，
   builtin_dispatcher.cpp:611-654 增加 swarm 前缀）
```

关键不变量：**fan-in 主链路（pending_skills_count + coordinator 触发）不改语义**——swarm 只是"批量化的 skill 组 + 额外的结构化聚合层"，swarm 机制失效时可回退为普通多 skill 模式（聚合层缺席时 coordinator 仍能看到各 item 的 `{skill}_result`）。

##### 8.3.4 一期边界（明确不做）

- 不跨 agent（无 G1/G2 依赖）；子任务不具备独立 agent 身份，不能被子任务之外寻址；
- 子任务不可再嵌套 swarm（`max_swarm_depth=1` 硬限制，二期放开）；
- 不做后台 swarm（一期 swarm 聚合阻塞当前 dialogue 的 coordinator 轮次，与 kimi-code"swarm 一律前台"一致）。

#### 8.4 二期：跨 agent swarm

##### 8.4.1 agent 寻址路由扩展（解 G1）

- `send_async_message`（`runtime.cpp:2604-2634`）扩展 `target_agent_id`：先按 agent_id 在 runtime `agents_` 表直查，命中则调目标 agent 的 `route_request_message`；未指定 agent_id 时保持现有 session_id 反查行为（向后兼容）。
- 消息 metadata 增加 `parent_swarm_ref = {parent_agent_id, parent_session_id, swarm_id, swarm_index}`：子 agent 侧完成回投时按父链寻址回家（复用定向路由 `agent.cpp:918-959` 的 metadata 通路）。
- 子 agent 的动态获得方式分两档（一期半/二期）：(a) 同 runtime 已注册的另一个 agent（AgentRegistry 显式 load 的 agent 包）——机制上只需寻址扩展；(b) 运行时动态加载 agent 包——依赖 Registry 增加动态 load API，列入二期可选。

##### 8.4.2 父子数据面共享域约定（解 G2）

- **共享域**：约定 `/session/{sid}/shared/{ns}/{key}` 前缀为父子可见域。实现上两条路（按部署形态选一）：
  - 有 Redis（RemoteDatabus 已连接）：共享域键强制写穿远端、读走远端，父子 agent 经同一 Redis 前缀互见（现有写穿/读回填语义 `databus.cpp:29-54` 已具备，仅需键约定 + 强制远端标志）；
  - 无 Redis：父 agent 把所需数据打包为 `data_ref`（`databus.cpp:92-124` 的 store_with_ref 大对象引用）随派发消息传递，子 agent 侧经 UDS 读父 agent 的 data_ref——弱共享，适合单进程多 agent。
- **隔离域**：子 agent 私有键仍在其自身 MMDataBus 的 `/session/{child_sid}/...` 下，父不可见；聚合结果经消息回传（引用或内联），不做数据面直读。
- 附带修复点：`remove_by_prefix` 只清本地不清远端（`databus.cpp:68-70`），共享域清理需补远端删除，否则共享域成为泄漏点。

##### 8.4.3 级联中断（解 G4 的二期部分）

- SwarmManager 维护 **parent→children 注册表**（内存）：`{swarm_id → [(child_agent_id, child_session_id, item), ...]}`；
- `SESSION-INTERRUPT` 到达父 session 时（`agent.cpp:1011-1051` 现有分支内挂接），先对注册表中本 session 的活跃 swarm 逐个发级联 interrupt：一期=对 item runtime task 调 `MMAgentTask::interrupt`（`task.cpp:72`）；二期=向子 agent 投 `SESSION-INTERRUPT` 控制消息（经 8.4.1 路由）；
- 子 agent 的 swarm 再向下级联，形成取消树（语义对齐 kimi-code"递归先孙后子"；槽位标记分态：已完成保留/RUNNING→CANCELLED/QUEUED→CANCELLED_NOT_STARTED，对齐 6.4 的语义分层）；
- 二期以后跨进程：控制面依赖 035 的 `IWorkerRuntime` 或"子 agent 跑另一 agentflowd + 控制消息经 UDS/Redis 通道"弱集成，本文不展开。

#### 8.5 批量调度策略（并发与限流）

综合三家验证过的策略（kimi-code ramp+限流恢复、codex 三级闸门、jiuwenswarm 三层准入），适配 AgentFlow 现有两级资源（task 级 `max_concurrent_skills` 与 runtime 级 workflow 池/worker 线程）：

| 层 | 机制 | 默认值 | 借鉴来源 |
|---|---|---|---|
| **L0 总量闸门** | 单 swarm `max_swarm_items` 硬上限（解析期 fail-fast）；单 session 同时活跃 swarm 数 `max_active_swarms` | 32 / 4 | kimi-code 128 上限；codex 总数 CAS；jiuwenswarm L3 全局准入 |
| **L1 ramp 起步** | 每 swarm 初始并发 `swarm_ramp_initial`，之后每 `swarm_ramp_interval_ms` 增放 1 个，直至 `max_concurrency`（spec 级）∩ `max_concurrent_skills`（task 级现有闸门，`task.cpp:264`） | 4 / 500ms | kimi-code 5+700ms |
| **L2 超限补拉** | 并发满时未拉起 item 保持 QUEUED，item 完成释放槽位后补拉 | 复用现有"留待下次触发补拉"（`agent.cpp:2539-2562`）语义，改为槽位释放事件驱动 | AgentFlow 现状 |
| **L3 限流自适应** | item 失败类型为"下游限流/超时簇"（workflow 报 429 类错误、或连续超时 ≥2 个）时：当前 swarm 有效容量 = 已成功数（≥1），再触发缩 1；连续 `swarm_ratelimit_recover_ms` 无新限流则容量+1 | recover 180s | kimi-code 容量状态机（进入容量=ready 数/再限流缩 1/3min 静默+1） |
| **重试绑定** | 限流类失败的重试**绑定原 item 槽位**重放（保留 task parameters 与已注入上下文），不占新 swarm_index；重试次数 ≤ `swarm_item_max_retries`（默认 1，非限流类失败不重试） | 1 次 | kimi-code 同 agent 重试复用上下文 |

调度器实现位置：SwarmManager 内的定时器线程（或挂在现有 task monitor 线程节拍上，`task.cpp:344-378` 已有 500ms 循环可复用），避免新增全局线程。一期不做跨 swarm 限流记忆（与 kimi-code 一致的单调度器取舍），L3 状态 per-swarm。

#### 8.6 结果聚合设计（结构化聚合）

##### 8.6.1 DataBus 键契约

| 键（前缀 `/session/{sid}/sys/{dlg}/`） | 写入方 | 读取方 | 内容 |
|---|---|---|---|
| `swarm/{swarm_id}/spec` | SwarmManager | coordinator workflow | 规格快照（含展开后 items 数） |
| `swarm/{swarm_id}/slots/{i}/status` | SwarmManager | coordinator workflow | QUEUED→LAUNCHING→RUNNING→SUCCEEDED/FAILED/TIMED_OUT/CANCELLED/CANCELLED_NOT_STARTED |
| `swarm/{swarm_id}/slots/{i}/result_ref` | SwarmManager | coordinator workflow | item 结果的 data_ref 或内联小结果（引用 `{item_skill}_result` 实际键） |
| `swarm/{swarm_id}/slots/{i}/error` | SwarmManager | coordinator workflow | 失败原因分类（launch_failed/workflow_failed/timeout/cancelled/rate_limited） |
| `swarm/{swarm_id}/summary` | SwarmManager | coordinator workflow | `{total, succeeded, failed, timed_out, cancelled, pending}` 计数镜像 |
| `swarm/{swarm_id}/aggregate` | SwarmManager | coordinator workflow | 渲染完成的结构化聚合文本（见 8.6.3） |

##### 8.6.2 槽位状态机与部分失败策略

槽位状态机：`QUEUED → LAUNCHING → RUNNING → {SUCCEEDED | FAILED | TIMED_OUT}`；取消路径：QUEUED→CANCELLED_NOT_STARTED、RUNNING→CANCELLED（已完成 SUCCEEDED 不被取消覆盖——对齐 kimi-code"用户取消保留已完成结果"）。所有迁移由 SwarmManager 单点写（避免现状"多触发源改同一键"的竞态，见 7.1 局限）。

`failure_policy` 三档（在每次槽位更新时判定）：

| 策略 | 判定 | 适用 |
|---|---|---|
| `all_required`（默认） | 任一 item 终态为 FAILED/TIMED_OUT → 批次标 PARTIAL_FAILED，但仍等其余终态后聚合（聚合文本如实标注失败槽位） | 结果缺一不可 |
| `best_effort` | 等全部终态，成功多少算多少，批次恒标 COMPLETED | 探索/收集类 |
| `quorum:k` | 成功数 ≥k 即可提前聚合（其余 RUNNING 取消、QUEUED 标 CANCELLED_NOT_STARTED）；若剩余理论可达成功数 <k 则判 FAILED | N 选 M、投票类 |

##### 8.6.3 聚合渲染与"框架级捕获"

- **框架级捕获（借鉴 agent-core-java TeamResultCollector 的核心思想：结果由框架收集，不依赖子任务自觉汇报）**：item 完成走现有 skill 完成检测路径时，SwarmManager 在框架侧截取结果写槽位——子任务的 workflow 不需要知道自己在 swarm 里，也无需"记得汇报"。
- **按序占位聚合（借鉴 kimi-code renderSwarmResults）**：`aggregate` 键渲染为确定性结构文本——头部 summary 行（各状态计数）+ 每槽位一节（swarm_index、item 原文摘要、状态、结果引用/错误 + resume 提示"可用 swarm_retry 计划重跑失败槽位"）。coordinator workflow 拿聚合文本做语义汇总，不需要逐个键拼装。
- **handoff 质量兜底（借鉴 kimi-code）**：item 结果文本长度 < `swarm_summary_min_chars`（默认 200）且状态为 SUCCEEDED 时，在聚合文本中对该槽位标注 `low_content_warning`，由 coordinator workflow 决定是否以 `force` 重跑该槽位（复用现有 force 重跑机制 `agent.cpp:2436-2496`）；一期不做自动续问（保持简单，二期评估）。

#### 8.7 上下文隔离与继承

原则：**默认隔离 + 显式继承白名单**（kimi-code 独立 wire 与 codex 过滤式 fork 的共同端点；AgentFlow 没有对话历史对象可 fork，继承的载体是 DataBus 键与 task parameters）：

| 方面 | 设计 |
|---|---|
| **执行隔离** | 每 item 一次 workflow clone 执行（`runtime.cpp:608`），执行状态互不干扰（现有保证） |
| **数据隔离** | item 的 dialogue 子前缀：`/session/{sid}/sys/{D}/swarm/{swarm_id}/items/{i}/...`——item workflow 的框架键读写（含 `{skill}_result`）落在各自子前缀下，互不覆盖（复用 dialogue 级隔离语义，021 号增强） |
| **输入注入契约** | item 上下文 = item_skill 的系统/工作流定义 + 展开后 prompt（task parameters 注入 `expanded_prompt`、`item_text`）+ **`inherit_keys` 白名单**：SwarmManager 把父 dialogue 中列名键的值拷贝进 item 子前缀（显式声明才继承——对应 codex"过滤式 fork"的键空间版本；未声明一律不继承——对应 kimi-code 默认不共享） |
| **父对子的可见性** | 父侧只见槽位聚合（状态+结果引用+错误），不见 item 的过程键（对应 kimi-code"父只见最后一条 assistant 文本"） |
| **中间产物共享** | item 需要共享大产物时写 data_ref（`databus.cpp:92-124`），引用经 result_ref 透出（对应 kimi-code"中间产物靠写文件/引用共享"） |
| **user 命名空间** | item 的 `{skill}_result/{skill}_confidence` 在子前缀下，`cleanup_dialogue_data`（`builtin_dispatcher.cpp:611-654`）扩展递归清理 `/swarm/` 前缀 |

二期（跨 agent）继承：inherit_keys 值经 8.4.2 共享域传递，契约不变。

#### 8.8 取消、超时与护栏

| 机制 | 设计 | 落点 |
|---|---|---|
| **级联取消树** | SESSION-INTERRUPT → 本 session 活跃 swarm 逐个级联（一期=item task interrupt；二期=子 agent 控制消息），槽位分态标记（已完成保留/RUNNING→CANCELLED/QUEUED→CANCELLED_NOT_STARTED） | `agent.cpp:1011-1051` 分支挂接 SwarmManager::cancel_swarm；8.4.3 |
| **批次取消** | 新增控制语义 swarm_id 级取消（control 消息扩展 `SWARM-INTERRUPT {swarm_id}`），只取消该批次 | 控制消息处理（`agent.cpp:906-1104`）扩展 |
| **子任务超时** | `item_timeout_ms`（spec 级，默认 120s）：item 的 workflow 执行超 deadline → 标 TIMED_OUT 占槽（不株连兄弟）——复用节点 `timeout_ms`（`worker.h:47`）之上加 workflow 级 deadline 检查（item launch 时记起始时刻，monitor 节拍扫描） | SwarmManager 定时器 + `runtime.cpp` workflow 状态查询 |
| **最大轮次** | `max_swarm_rounds`（默认 8）：同 dialogue 内 swarm 批次序号超限 → 新 swarm 计划被拒并回写错误键（防 coordinator 无限再规划放大）——与 031 §6.6 的 max_coordination_rounds 共用计数 | SwarmManager + 框架键 `swarm/round_count` |
| **循环检测** | ① 同 dialogue 内 swarm 计划指纹（item_skill+items 哈希）重复 ≥2 次 → 拒绝并告警（对齐 jiuwenswarm 重复检测思想）；② 嵌套深度 `max_swarm_depth`：一期=1（item 的 workflow 无 swarm 计划写权限——机制强制，对齐 codex"深度超限则子看不到 spawn 工具"），二期放开到 3 | 计划解析期校验 |
| **深度机制强制** | item runtime task 的 task parameters 标记 `in_swarm=true`，其 workflow 写 `next_skills` swarm 格式被 SwarmManager 拒绝（一期） | 解析期检查 |

**护栏落地顺序**：G7 已明确"护栏先于 swarm"——`max_swarm_rounds`/指纹去重/深度限制随一期第一批交付（实现成本低，且独立于 swarm 功能本身可单独验证）。

#### 8.9 权限模型

| 维度 | 设计 |
|---|---|
| **子任务可用 skill 白名单** | swarm spec 的 `allowed_skills` ⊆ session 级 `candidate_skills`（现有白名单机制 `builtin_dispatcher.cpp:265-304` 的子集校验）；item_skill 本身必须在白名单内；item 的 workflow 内部不再做二次裁剪（一期 item 即单一 skill，无内部再分发） |
| **编排/执行角色分离** | 一期由机制自然达成：coordinator/dispatcher 才能发起 swarm（只有它们的 data_exchange 路径接 SwarmManager），skill task 无 swarm 计划写入权限（8.8 深度强制）——对应 agent-core-java 角色工具裁剪的弱化版 |
| **DataBus 域可见性** | 一期：item 可见 = 自身子前缀 + inherit_keys 拷贝 + agent 级公共域（现状）；不可见兄弟 item 子前缀（键前缀天然隔离）。二期：子 agent 可见 = 自身 DataBus + 共享域 `/shared/`（8.4.2），父私有域不可见 |
| **审批** | AgentFlow 框架层无认证授权（接入层职责，ARCHITECTURE §7.1），swarm 不新增审批语义；若接入层需要"批量派发需审批"，可在 agentflowd 控制消息层挂 hook（二期评估） |

#### 8.10 可观测性（FlowTrace 扩展）

在现有 FlowTrace 事件族（`include/internal/runtime/flow_trace.h`，事件发射点 `runtime.cpp:1746-1822`）上扩展 swarm 事件，全部携带 `{session_id, dialogue_id, swarm_id}` 标签：

| 事件 | 触发点 | 附加字段 |
|---|---|---|
| `swarm.submitted` | SwarmManager::submit_swarm 解析完成 | total、item_skill、failure_policy |
| `swarm.item_launched` | ramp 调度器实际拉起 item | swarm_index、item 摘要（前 N 字符）、当前并发 |
| `swarm.item_completed` / `swarm.item_failed` / `swarm.item_timed_out` | on_item_finished | swarm_index、耗时、error 分类 |
| `swarm.item_rate_limited` | L3 限流判定 | 当前有效容量（对齐 kimi-code `subagent.suspended`） |
| `swarm.completed` / `swarm.partial_failed` / `swarm.cancelled` | 批次终态 | summary 计数、总耗时 |
| `swarm.quorum_early_exit` | quorum 提前聚合 | 达成 k 值、被取消槽位数 |

另：`swarm/{swarm_id}/summary` 键本身即实时进度面（供 daemon 侧 ResponsePoller 或 TUI 轮询展示，对齐 jiuwenswarm 的 team 状态快照思想）；tracing 标签打通现有 session/dialogue/task 三级生命周期回调，使 swarm 进度可挂在现有 flow_trace_viewer 工具上呈现。

### 9. 具体修改点清单

以下文件路径均来自 §7 现状分析的实证清单；改动量为设计估计，实施时以评审为准。标注 **[一期]** / **[二期]**。

#### 9.1 新增文件

| 文件 | 内容 | 分期 |
|---|---|---|
| `include/internal/agent/swarm_manager.h` | `SwarmManager` 类声明：`submit_swarm`（解析/校验/展开/建槽）、ramp 调度器（定时节拍）、`on_item_finished`（槽位更新+策略判定）、`cancel_swarm`/`cascade_interrupt`、SwarmHandle/槽位状态机/限流状态机（L3）数据结构 | 一期 |
| `src/agent/swarm_manager.cpp` | 上述实现；DataBus 键契约读写（经 `BuiltinDispatcher` 框架键 helpers）；聚合文本渲染（8.6.3）；护栏计数（轮次/指纹/深度） | 一期 |
| `include/internal/agent/swarm_types.h` | `SwarmTaskSpec`、`SwarmItem`、槽位状态枚举、`FailurePolicy` 枚举、错误分类枚举（供 agent.cpp / builtin_dispatcher.cpp / flow_trace 共用） | 一期 |
| `tests/unit/swarm_manager_ut.cpp`（或并入现有 unit 目录约定） | 展开/校验/槽位状态机/失败策略/限流收缩恢复/聚合渲染 单测 | 一期 |
| `tests/functional/`（或 e2e）swarm 场景用例 | 多 item 并行、部分失败、取消级联、超时占槽、护栏拒绝 | 一期 |
| `examples/features/` 下 swarm 示例（agent.json + coordinator workflow DSL 产出 swarm 计划） | 端到端演示与回归基准 | 一期 |

#### 9.2 修改文件（agent 层）

| 文件 | 改动 | 分期 |
|---|---|---|
| `include/internal/agent/agent.h`（类声明在 :72 起） | 新增成员 `std::unique_ptr<SwarmManager> swarm_manager_`；声明 swarm 相关私有方法（`handle_swarm_plan`、`on_swarm_item_finished`、级联入口）；swarm 配置项成员 | 一期 |
| `src/agent/agent.cpp`（3178 行，协同逻辑主体） | ① `process_next_skills_launch`（:2372-2609）增加 swarm 格式分支：识别 `{"swarm": ...}` 后移交 `SwarmManager::submit_swarm`，普通 next_skills 路径不变；② `launch_skill_task`（:2056）支持 swarm item 调用：task parameters 注入 `{swarm_id, swarm_index, item_text, expanded_prompt, in_swarm}`，dialogue 目标子前缀解析（8.7）；③ skill 完成检测路径（:1518-1537, :1791-1808）：参数带 `swarm_id` 时额外回调 `SwarmManager::on_item_finished`；④ coordinator 分支（:1772-2001）：识别 `trigger_type=swarm_completed`，聚合键可读后再触发 coordinator workflow；⑤ `SESSION-INTERRUPT` 分支（:1011-1051）挂接级联取消；⑥ `build_runtime_task`（:1106-1225）透传 swarm 元数据与 dialogue 子前缀 | 一期 |
| `include/internal/agent/builtin_dispatcher.h` / `src/agent/builtin_dispatcher.cpp` | `parse_next_skills`（:385）增加 swarm 对象格式解析；`get_framework_data`（:498-537）/`publish_framework_data`（:539）支持 swarm 槽位键族；`cleanup_dialogue_data`（:611-654）增加 `/swarm/` 前缀递归清理 | 一期 |
| `include/internal/agent/task.h` / `src/agent/task.cpp` | `can_launch_more_skills()`（:264）计数口径与 swarm 并发闸门对接（swarm 占用与独立 skill 统一计数）；monitor 节拍（:344-378）复用为 SwarmManager 定时器挂载点（item 超时扫描、ramp 步进） | 一期 |
| `src/agent/session.cpp` / `include/internal/agent/session.h` | 一期基本不改（子前缀复用 dialogue 语义）；二期：`parent_swarm_ref` 元数据字段与父链查询接口 | 二期 |
| `src/agent/coordinator.cpp` / `include/internal/agent/coordinator.h` | `aggregate_session_results`（:154-170，当前未接主路径）评估复用为 swarm 聚合的 workflow 侧入口；workflow 映射增加 swarm aggregate 键说明 | 一期（小改） |

#### 9.3 修改文件（runtime / daemon / databus 层）

| 文件 | 改动 | 分期 |
|---|---|---|
| `include/internal/runtime/flow_trace.h` + `src/runtime/runtime.cpp`（事件发射点 :1746-1822） | 新增 §8.10 的 swarm.* 事件类型与发射点（发射调用由 SwarmManager 经 agent 回调 runtime 的 trace 接口） | 一期 |
| `src/runtime/runtime.cpp` `agent_task_poller_loop`（:1872-2030） | 一期不改主路径（swarm item 就是普通 runtime task）；仅确认 swarm 元数据在 inputs 组装（:1944 附近）中透传 | 一期（核对） |
| `src/runtime/runtime.cpp` `send_async_message`（:2604-2634） | 扩展 `target_agent_id` 寻址：agent_id 直查 `agents_` 表 → 目标 agent `route_request_message`；无 agent_id 保持现状 | 二期 |
| `src/daemon/agent_registry.cpp`（:17-81） | 二期：按 agent_id find 用于跨 agent 派发的校验与能力查询；可选：运行时动态 load agent 包 API | 二期 |
| `src/runtime/databus/databus.cpp` | 二期：共享域键约定（`/session/{sid}/shared/` 强制远端写穿标志）；修复 `remove_by_prefix`（:68-70）只清本地问题（远端同步删除） | 二期 |
| `src/agent/dataref_bridge.cpp` | 二期：跨 agent data_ref 传递的桥接核对（大对象引用随派发消息） | 二期 |

#### 9.4 配置与文档

| 文件 | 改动 | 分期 |
|---|---|---|
| `agent.json` schema（examples/ 与 docs/api-reference.md 同步） | 新增 `swarm` 配置段：`max_swarm_items`(32)、`max_active_swarms`(4)、`swarm_ramp_initial`(4)、`swarm_ramp_interval_ms`(500)、`swarm_item_max_retries`(1)、`swarm_ratelimit_recover_ms`(180000)、`item_timeout_ms` 默认(120000)、`max_swarm_rounds`(8)、`max_swarm_depth`(一期 1/二期 3)、`swarm_summary_min_chars`(200) | 一期 |
| `docs/ARCHITECTURE.md` / `docs/agentflow_user_manual.md` | swarm 模式章节：概念、next_skills swarm 格式、键契约、配置项、FlowTrace 事件 | 一期 |
| `docs/databus-session-isolation.md` | 补充 `/swarm/` 前缀与（二期）`/shared/` 共享域的隔离语义 | 一期/二期 |
| 本文件状态 | 一期落地后改 implemented | — |

#### 9.5 与既有 enhancement 的依赖关系

| 依赖 | 关系 | 处理 |
|---|---|---|
| 031（coordinator 护栏、`spawn_sub_session` 规划） | G7 护栏是一期前置；一期方案是 031 待实现 7 的最小落地形态 | 一期第一批先交护栏（8.8），可独立验收 |
| 024（消息定向路由） | 一/二期父子消息通道底座 | 直接复用，无改动 |
| 023（next_skills 格式扩展） | swarm 格式是其解析路径的第四分支 | 沿用扩展点 |
| 021（dialogue 级隔离） | item 子前缀隔离的语义基础 | 直接复用 |
| 034/035（DataBus 适配层、worker 分进程） | 二期跨进程（G8）的前置 | 二期以后，不在本方案承诺范围 |
| 037（managed agent runtime） | 二期父子会话管理与 agent 动态加载 | 二期设计时对齐 |
| 038（harness agent loop 样例） | 护栏实践参考 | 参考 |

### 10. 实施路线与风险

#### 10.1 分期里程碑

**M0：护栏先行（前置，约 0.5 个迭代）**

- 交付：`max_swarm_rounds` 轮次上限、swarm 计划指纹去重、`max_swarm_depth=1` 机制强制（8.8）、配置段落地。
- 验收：构造"coordinator 持续产出相同 swarm 计划"用例，第 8 轮被拒且 FlowTrace 有告警事件；item 的 workflow 写 swarm 计划被拒。

**M1：session 内 swarm 核心（一期主体）**

- 交付：SwarmManager（解析/展开/槽位状态机）、next_skills swarm 格式、item 子前缀隔离与输入注入契约、聚合键契约与按序占位聚合渲染、框架级捕获接入 skill 完成路径。
- 验收：示例（coordinator 产出 3-8 个 item 的 swarm 计划）端到端跑通：并行执行、聚合文本完整（槽位按序、状态正确）、结果经 coordinator 汇总回包；`all_required`/`best_effort`/`quorum:k` 三策略各有用例；fan-in 回退验证（聚合一层的键被删除时，coordinator 仍能从各 item `{skill}_result` 工作）。

**M2：调度、取消与可观测（一期收尾）**

- 交付：ramp+L3 限流状态机、item 超时、`SWARM-INTERRUPT` 批次取消与 SESSION-INTERRUPT 级联、FlowTrace swarm.* 事件族、低质量结果标注（8.6.3）。
- 验收：并发不超过闸门且逐步爬升；注入 429 类失败观察容量收缩与静默恢复；超时 item 只占自己槽位；取消后已完成槽位保留、未启动标 CANCELLED_NOT_STARTED；flow_trace_viewer 可见完整 swarm 时间线。

**M3：跨 agent swarm（二期）**

- 交付：`target_agent_id` 路由、`parent_swarm_ref` 父链、共享域约定（Redis 路径优先）+ `remove_by_prefix` 远端修复、跨 agent 级联中断、`max_swarm_depth` 放开到 3。
- 验收：同 runtime 两 agent（父 A/子 B）跑通"父 swarm 派发 → 子 session 执行 → 结果经父链回传聚合"；父 SESSION-INTERRUPT 级联到子；共享域键双向可见、私有域互不可见；嵌套 swarm（子的子）深度超限被拒。

#### 10.2 主要风险与缓解

| 风险 | 说明 | 缓解 |
|---|---|---|
| **协同状态竞态** | 现状协同状态是 DataBus 键组合隐式表达、非原子读改写（`agent.cpp:1547-1563, 2323-2343` 的 get-then-publish），swarm 增加触发源后竞态窗口放大 | 槽位状态与计数**单点写**（SwarmManager 是唯一写者，8.6.2）；summary 计数由槽位推导而非独立增减；必要时给 swarm 键族加版本号/compare-and-swap 语义（034 适配层的 CAS 能力可复用） |
| **资源耗尽** | fan-out 放大 workflow 实例/worker 线程/内存占用；默认不设限会拖垮整个 agent | L0 硬上限 fail-fast；ramp 保守起步；与 `max_concurrent_skills`/`workflow_pool_size` 统一计数；压测确定默认值（先保守后放宽） |
| **编排可靠性依赖 LLM** | swarm 计划由 coordinator/dispatcher workflow（LLM 节点）产出，可能产出非法/低质计划 | 解析期 fail-fast 校验+错误键回写让 workflow 重试（对齐 codex"错误转译为模型可读文本让编排者自救"）；聚合渲染确定性、不依赖 LLM 自觉（8.6.3 框架级捕获）；示例 workflow DSL 固化最佳实践 |
| **聚合语义膨胀** | 部分失败策略/取消分态/嵌套组合状态多，易出边缘 case | 一期只承诺单级、前台、三策略；槽位状态机表格化并配全迁移单测；嵌套二期才放开 |
| **代码债叠加** | `agent.cpp` 已 3178 行且两套 dispatcher/coordinator 逻辑并存（031 §4.5），swarm 逻辑再内联会恶化 | SwarmManager 独立成文件，agent.cpp 只做挂接（每个挂接点 <20 行）；不顺手重构旧债（保持 diff 可评审），旧债清理由 031 后续跟踪 |
| **二期范围蔓延** | 跨进程（G8）依赖 035、动态 agent 加载依赖 037，均为大工程 | 二期承诺范围收敛到"同 runtime 多 agent"；跨进程仅给出弱集成说明（8.4.3），显式列入非目标 |
| **键契约兼容性** | swarm 键族进入 dialogue 前缀空间，与现有清理/老化路径交互 | cleanup/aging 路径统一评审（session_router.cpp:248-275 的老化释放与 builtin_dispatcher.cpp:611-654 的清理都要覆盖 `/swarm/` 前缀）；新增键全部走 `get_framework_data` 三级回退体系，不发明平行通道 |
| **轮询时延叠加** | 现有全链路多轮询点（poller 20ms/ResponsePoller 50ms），ramp 节拍再加一档 | ramp/超时扫描挂现有 task monitor 节拍（9.2 task.cpp 行），不新增全局线程；事件驱动化留待 037 统一处理 |

#### 10.3 非目标（明确不做）

- 不做后台 swarm（批次异步派发+完成通知 steer 回父 dialogue）——一期前台聚合，二期评估；
- 不做跨进程/跨机 swarm 的控制面——依赖 035，本文仅给数据面共享约定与弱集成说明；
- 不做可编程聚合策略（投票/多路归并）——quorum:k 已覆盖主要场景，其余由 coordinator workflow 的 LLM 语义汇总承担（对齐"框架做结构化、workflow 做语义"分工）；
- 不做 agent 间自由消息网络（codex V2 的 mailbox/互发消息全量形态）——二期只做父子定向通道，peer 互发不在范围；
- 不重构 031 已识别的旧债（SessionTaskCoordinator 死代码、builtin_dispatchers_ 死存储、隐式状态机泛化）——仅保证 swarm 新代码不走这些路径。

### 附录 A：参考项目代码索引（本地路径）

| 项目 | 路径 | 关键文件 |
|---|---|---|
| kimi-code | `~/gitProjects/kimi-code` | `packages/agent-core/src/tools/builtin/collaboration/agent-swarm.ts`、`agent.ts`、`src/session/subagent-batch.ts`、`subagent-host.ts`、`src/agent/swarm/index.ts`、`src/agent/permission/policies/`；v2 对应 `packages/agent-core-v2/src/session/swarm/*`、`src/agent/tools/agent-swarm/*` |
| codex | `~/gitProjects/codex` | `codex-rs/core/src/agent/control.rs`（+`control/spawn.rs`、`control/execution.rs`、`control/residency.rs`）、`core/src/tools/handlers/multi_agents/`、`multi_agents_v2/`、`core/src/thread_manager.rs`、`core/src/session/input_queue.rs`、`core/src/codex_delegate.rs`、`protocol/src/protocol.rs` |
| jiuwenswarm | `~/atomGitProjects/jiuwenswarm` | `jiuwenswarm/agents/harness/team/team_manager.py`、`remote_member_bootstrap.py`、`config_loader.py`、`jiuwenswarm/agents/swarm/`（assembly.py、context.py、DESIGN.md）、`server/runtime/agent_adapter/team_helpers.py`、`agents/harness/team/handlers/`、`resources/config.yaml` |
| agent-core-java | `~/atomGitProjects/agent-core-java` | `agentteams/agent/TeamAgent.java`、`CoordinatorLoop.java`、`EventDispatcher.java`、`SpawnManager.java`、`agentteams/tools/TeamTools.java`、`TeamResultCollector.java`、`TeamBackend.java`、`core/multiagent/`（BaseTeam、TeamRuntime、teams/handoff、hierarchical_*）、`core/singleagent/`（BaseAgent、ReActAgent、AbilityManager） |

### 附录 B：本文术语对照

| 本文 | kimi-code | codex | jiuwenswarm / agent-core-java | AgentFlow 现状对应 |
|---|---|---|---|---|
| SwarmTaskSpec | AgentSwarm 工具入参 | spawn_agent 入参集合 | TeamAgentSpec（子集） | next_skills 扩展对象 |
| SwarmItem | QueuedSubagentTask | ThreadSpawn 子线程 | teammate（单次任务维度） | skill runtime task |
| 槽位 slot | 按序占位结果槽 | status watch + 完成邮件 | TeamResultCollector 条目 | `{skill}_result` 键（推广） |
| SwarmHandle | SubagentBatch 句柄 | AgentControl 订阅面 | 任务看板 + TeamResultCollector | pending_skills_count（推广） |
| SwarmManager | SessionSubagentHost + SubagentBatch | AgentControl | TeamManager（会话级部分） | 新增组件（MMAgent 成员） |
| ramp/L3 限流 | INITIAL_LAUNCH_LIMIT + 容量状态机 | 三级闸门 | swarmflow_concurrency 三层 | max_concurrent_skills（扩展） |
| 子 dialogue 子前缀 | 子 agent 独立 wire/homedir | 子线程独立 rollout | 成员独立上下文 | dialogue 键前缀（扩展） |
| 共享域 | 会话级共享服务 | 共享工作区 | 共享存储 + 团队工作区 | /shared/ 前缀（二期新增） |
