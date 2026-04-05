## Dispatcher / Coordinator / Skill 调度协同与 DataBus 可观测性分析（2026-04-05）

### 1. 背景

dispatcher、coordinator、skill 多任务协同与 **MMDataBus** 共享数据是 AgentFlow 的核心路径；workflow 与多线程 worker 并发读写 DataBus 时，问题定界依赖日志与 Trace。需要通过 **静态代码分析** 与 **examples 行为归纳**，明确当前实现的 **风险点、可观测性缺口与运维短板**，为后续实施提供依据。

**希望达到的效果**：

- 调度与 DataBus 使用方式 **文档化、可审计**；
- 生产环境具备 **关联 ID、队列深度、DataBus 容量与调度类 metrics** 等定界手段；
- 双总线（runtime vs `dataref_bridge`）、并发安全等 **架构风险** 有明确处置路径。

### 2. 方案简述

本 enhancement 定位为 **分析型 tbd 文档**：**不**在本文档状态下直接改代码（符合 `tbd` 流程）。整体拆解为：

1. **调度与协同**：梳理 `MMAgentTask` 队列、`handle_task_intent_request` / `handle_task_data_exchange` 与 DataBus 契约。
2. **DataBus**：分层（MMDataBus / ZeroCopyDataBus）、Agent 锁、`store_with_ref` / `key_to_ref_`、与 `dataref_bridge` 的关系。
3. **示例与测试缺口**：happy path 与负例/压测覆盖评估。
4. **可观测性与运维**：日志、Trace、Metrics 现状评估 + **可执行**改进清单（实施时落入独立 PR）。

### 3. 约束与假设

- 分析基于仓库 **main** 分支当时代码路径；行号可能随提交变化。
- 具体代码修复需 **人工确认后** 另开 `wip` 实施，并补充 §5–§8 中的结构设计细节。
- 与 [031-feature-coordinator-协同机制设计分析-260405-tbd.md](./031-feature-coordinator-协同机制设计分析-260405-tbd.md)（协调策略）、[025-feature-session-老化-databus-清理问题-260320-implemented.md](./025-feature-session-老化-databus-清理问题-260320-implemented.md)（清理）、[016-feature-全链路跟踪系统tracesystem-260307-implemented.md](./016-feature-全链路跟踪系统tracesystem-260307-implemented.md)（Trace）可交叉引用，避免重复造轮子。

### 4. 当前实现分析

#### 4.1 代码分析

##### 4.1.1 调度与协同架构（静态）

**角色与队列**

| 组件 | Task 类型 | 主要队列 | 典型处理入口 |
|------|-----------|----------|----------------|
| Dispatcher | `dispatcher` | `intent_request`、`data_exchange` | `handle_task_intent_request`、`handle_task_data_exchange` |
| Coordinator | `coordinator` | `intent_request`、`data_exchange`、`response` | 同上；`response` 承载 skill 完成通知 |
| Skill | `skill`（含 runtime 实例） | 同上 | workflow 完成后进入 `data_exchange`，触发后续逻辑 |

每个 `MMAgentTask` 可启动 **三条消息线程**（`src/agent/task.cpp`）：

- `intent_request_loop` / `data_exchange_loop` / `monitor_loop`  
- 队列操作均受 **`MMAgentTask::mutex_`** 保护；`running_skills_count_` 为 **atomic**，与 `max_concurrent_skills` 配合限流。

**协同主路径（概念顺序）**

1. **入站**：用户/路由 → dispatcher 的 `intent_request` → `handle_task_intent_request` → 构建 runtime task → 提交 **dispatcher workflow**。
2. **dispatcher 完成**：`data_exchange` → `handle_task_data_exchange`：读 DataBus **`next_skills` / `selected_skills`**（经 `BuiltinDispatcher::get_framework_data` 多键格式）→ **`launch_skill_task`**。
3. **skill 并行**：多个 skill 可同时运行（受 `max_concurrent_skills` 约束）；完成后向 coordinator **`enqueue_response_message`** + **`enqueue_data_exchange_message`**（`trigger_type` 如 `skill_response`）。
4. **coordinator**：消费 `response` / 触发消息，跑 **coordinator workflow**，再写 **`next_skills`**、**`response`**、**`multi_response`** 等；框架据此继续调度或 **`SessionRouter` 回包**。
5. **清理**：`BuiltinDispatcher::cleanup_dialogue_data` 等按 dialogue 清理 sys/user 命名空间键。

**关键 DataBus 契约（框架层）**

与调度强相关的键包括但不限于：`next_skills`、`selected_skills`、`response`、`multi_response`、`pending_skills_count`、`launched_skills`、`skills_scheduled`、`candidate_skills`；会话隔离键前缀形如 `/session/{session_id}/sys/{dialogue_id}/...`（见 `src/agent/builtin_dispatcher.cpp`）。

**调度层结论**

| 观察 | 说明 | 风险/改进方向 |
|------|------|----------------|
| 逻辑集中在 `agent.cpp` | dispatcher/coordinator/skill 分支体量大，与 `BuiltinDispatcher` 部分能力重复 | 可维护性与单测难度高；建议收敛（对齐 031） |
| `coord_forwarded` 等防重 | dispatcher → coordinator 转发有 DataBus 标记 | 竞态依赖写入顺序；需 Trace 串联 message_id + dialogue_id |
| 无统一「协调轮次」指标 | 框架未内置 coordinator 轮次计数 | 运维难以发现「慢循环」与异常重规划 |

##### 4.1.2 DataBus 实现与多线程 / Workflow

**分层结构**

- **`MMDataBus`**（`src/runtime/databus/databus.cpp`）：组合 **LocalDatabus** 与 **RemoteDatabus**；`put` 先写本地 **ZeroCopyDataBus**，再 `remote_bus_->publish`。
- **`ZeroCopyDataBus`**（`src/runtime/databus/local_databus.cpp`）：**`std::shared_mutex data_mutex_`**（多读单写）；订阅列表单独 `subscriptions_mutex_`。
- **节点侧**：`MMNode` 携带 **`databus_`** 或 **`databus_client_`**；SO 经 **`NodeDataBusAdapter`** 调用 `MMDataBus::put/get`。

**`MMAgent` 与 Runtime DataBus**

- `MMAgent::publish_data` / `get_data`（`src/agent/agent.cpp`）在 **`MMAgent::mutex_`** 下访问 **`runtime_databus_`** 或回退 `data_bus_`。
- **影响**：经 Agent 的 DataBus 访问与其他互斥区绑定；高并发下可能拉长锁持有时间（若锁内逻辑过重）。
- **Worker 节点**：直接操作注入的 **`MMDataBus*`**，与 Agent 线程并发读写 **ZeroCopyDataBus**，依赖 **shared_mutex**。

**`MMDataBus::mutex_` 与 `key_to_ref_`**

- `include/internal/runtime/databus/databus.h` 声明 **`mutable std::mutex mutex_`**，但 **`databus.cpp` 中 `put/get/remove` 未使用该锁**。
- **`store_with_ref`** 写入 **`key_to_ref_`**（`std::map`）**无显式同步** → **潜在数据竞争**（需审计或加锁）。

**独立 DataBus：`dataref_bridge`**

- `src/agent/dataref_bridge.cpp` 使用 **函数内 static `MMDataBus`** + `std::call_once` 连接 Redis。
- 与 **`MMAgentRuntime::get_databus(agent_id)`** **不是同一实例** → **双总线**；大消息 DataRef 与业务键查询易不一致，运维需区分路径。

**Workflow 多节点 / 多线程**

| 场景 | 行为 | 注意点 |
|------|------|--------|
| 同 workflow 多节点顺序执行 | 边传输 `DataMap`，部分边可写 **`databus://global/...`** 引用（`src/workflow/edge/edge.cpp`） | 键与 node id 绑定，避免跨 dialogue 误读 |
| 多 skill 并行 | 多 worker 同时对同一 **`MMDataBus`** `put/get` | 同一 key **后写覆盖**，无版本向量 |
| 多 dialogue | `/session/.../sys/{dialogue_id}/` | 错误 dialogue_id → 读空或旧数据 |

**DataBus 结论**

| 问题类型 | 描述 | 建议 |
|----------|------|------|
| 并发安全 | `key_to_ref_` 未保护；`MMDataBus::mutex_` 未用于 `put/get` | 审计后加锁或文档声明单线程约束 |
| 双总线 | runtime bus vs `dataref_bridge` | 架构图与键命名规范；长期是否合并 |
| 可观测性 | put/get 默认无日志 | 采样日志或 metrics（按 key 前缀聚合） |
| 容量 | `data_bus_capacity_mb` | 阈值告警 + gauge |

#### 4.2 Examples 分析

| 示例 | 与调度 / DataBus 相关的动态特征 |
|------|----------------------------------|
| **system_agent_mock** | dispatcher 选 skill → 多 skill 写 user/sys → coordinator 聚合 / `next_skills` / `force`；E2E 覆盖较全 |
| **multi_session_chat** | `session-coordinator-workflow` 多分支 merge；shell + DataBus；**dialogue_id** 明显 |
| **multi_language_databus** | 链式 coordinator + 多语言共享 DataBus |
| **basic_agent** | 入门：`basic-coordinator-workflow` |
| **llm_query_agent** | 异步 chunk / `[DONE]` 与 coordinator 消息联动 |

**结论**：**happy path** 与契约验证较强；**并发同键、bridge/runtime 混用、队列积压** 等 **负例与压测** 在 examples 中不足。

#### 4.3 缺失功能分析

**相对「可运维、可定界」目标的缺失**

- **统一 correlation id**（贯穿 enqueue 与日志）。
- **任务队列深度、data_exchange 耗时** 的结构化输出与 metrics。
- **dispatcher/coordinator 消息路径** 与 workflow/node trace 的 **同一 trace 树** 串联。
- **调度与 DataBus** 的 **默认 metrics**（counter/gauge/histogram）。
- **MMDataBus** 层 **并发与双总线** 的官方说明与测试。

**行业实践参考意义**：将「编排热点」与「数据面」指标分离（队列深度 vs databus 命中率/容量），便于 SLO 与告警分层。

### 5. 数据结构设计

实施阶段再定。候选方向：

- `message.metadata` 增加 **`request_trace_id`**（或对齐现有 id 字段）；
- Metrics 标签枚举与 **基数控制**（`key_prefix` 聚合而非全 key）。

### 6. 流程设计

调度主路径见 **§4.1.1**；实施增强后可补充「带 trace_id 的消息时序图」。

### 7. 接口设计

| 类别 | 建议（实施时细化） |
|------|-------------------|
| 日志 | 统一前缀 + `trace_id`、`task_id`、`dialogue_id`、`task_type` |
| Trace | `handle_task_*` 关键分支 **Span**；attribute 与 `flow_trace` 字段对齐 |
| Metrics | 见 **§8.3** 指标表示例 |

### 8. 实现方案

#### 8.1 调度与协同

1. 收敛 `agent.cpp` 与 `BuiltinDispatcher`（对齐 031）。
2. coordinator **轮次/代数** 计数 + 可选导出 metrics。
3. 文档化 `max_concurrent_skills` 与 pending 触发语义；日志 **采样** 打印 pending/launched 快照。

#### 8.2 DataBus

1. 修复或文档化 **`key_to_ref_` / `mutex_`**。
2. 文档化 **dataref_bridge vs per-agent MMDataBus**。
3. 关键框架键长期可考虑 **串行化写或版本号**；容量 **warn + gauge**。

#### 8.3 运维能力（可执行清单）

**日志**

| 措施 | 说明 |
|------|------|
| **request_trace_id** | SessionRouter 入站生成，写入 `message.metadata`，日志统一前缀 |
| **队列监控** | 周期性 `intent_request` / `data_exchange` **depth**（per task_id） |
| **DATA_EXCHANGE 耗时** | `handle_task_data_exchange` 入口/出口毫秒级（可采样） |
| **敏感与体积** | payload **截断**；禁止完整 user prompt 上 info |

**Trace**

| 措施 | 说明 |
|------|------|
| **Span 覆盖消息路径** | `handle_task_intent_request` / `handle_task_data_exchange` 子 span |
| **与 flow_trace 对齐** | `session`、`dialogue`、`from`、`to`（参考 `examples/agent/system_agent_mock/testing/flow_trace`） |

**Metrics（建议名）**

| 指标 | 类型 | 标签建议 |
|------|------|----------|
| `agent_dispatch_messages_total` | Counter | `task_type`, `result` |
| `agent_task_queue_depth` | Gauge | `task_id`, `queue`=`intent|data` |
| `agent_skill_launch_total` | Counter | `skill_name`, `result` |
| `databus_put_total` / `databus_get_miss_total` | Counter | `key_prefix`（脱敏聚合） |
| `databus_used_bytes` | Gauge | `agent_id` |
| `coordinator_rounds` | Histogram | `session_id`（控制基数） |

**运维流程**

1. Runbook：「无 response」→ `dialogue_id` → `pending_skills_count` → `next_skills` → coordinator 日志 → Trace node。
2. 健康检查：runtime 存活、databus 使用率、最深队列 task。
3. 告警：queue 超阈、databus >80%、`skill_launch_failed` 突增。

#### 8.4 待修改文件清单（实施时刷新）

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/agent/agent.cpp` | 修改 | trace_id、耗时日志、可选 metrics |
| `src/agent/task.cpp` | 修改 | 队列 depth 查询/上报 |
| `src/runtime/databus/databus.cpp` / `databus.h` | 修改 | `key_to_ref_` 同步、可选 put/get 采样 |
| `docs/ARCHITECTURE.md` 等 | 修改 | 双总线与调度观测说明 |

**功能优先级**：先 **无行为变更** 的日志/trace_id 与文档 → 再 metrics → 再 DataBus 锁与语义变更（需回归）。

### 9. 测试方案

| 功能点 | 建议 |
|--------|------|
| 队列与 trace | 单测或集成：metadata 携带 trace_id 后日志可 grep 串联 |
| `max_concurrent_skills` | CTest 边界；与 `system_agent_mock` E2E 互补 |
| DataBus 并发 | 负例 example 或测试：同 key 并行写预期 |
| Metrics | 拉取/导出冒烟测试（若接入 Prometheus 等） |

### 10. 文档

| 文档 | 变更说明 |
|------|---------|
| [ARCHITECTURE.md](../ARCHITECTURE.md) | DataBus 双实例、调度与观测章节 |
| [publish/INTEGRATION_GUIDE.md](../../publish/INTEGRATION_GUIDE.md) | 运维 Runbook 链接（若适用） |

**关联方案**

| 关联方案 | 关系 |
|----------|------|
| [031-feature-coordinator-协同机制设计分析-260405-tbd.md](./031-feature-coordinator-协同机制设计分析-260405-tbd.md) | 协调层策略与收敛 |
| [025-feature-session-老化-databus-清理问题-260320-implemented.md](./025-feature-session-老化-databus-清理问题-260320-implemented.md) | 清理与生命周期 |
| [016-feature-全链路跟踪系统tracesystem-260307-implemented.md](./016-feature-全链路跟踪系统tracesystem-260307-implemented.md) | Trace 基础 |
| `examples/agent/system_agent_mock/FLOW_ANALYSIS.md` | 消息流参考 |

### 11. 进展跟踪

- [ ] 背景分析归档（本文档）
- [ ] 人工确认后择项进入 `wip`
- [ ] trace_id + 队列日志 PoC
- [ ] metrics 注册与仪表盘
- [ ] MMDataBus `key_to_ref_` 安全修复
- [ ] 架构文档更新
- [ ] 验证通过

### 12. 后续计划

| 优化项 | 说明 |
|--------|------|
| DataBus 写冲突检测 | 关键键版本号或 CAS（长期） |
| 与 026 Master-Worker 边界 | 调度指标与 worker 队列联动 |

### 13. 修订记录

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.0 | 2026-04-05 | 由 `issues/001-analysis-*.md` 迁入 enhancement 032，按模板重组 |

---

**附录：原分析正文结构映射**

- 原 §1–2 → 本文 §4.1.1  
- 原 §3 → 本文 §4.1.2  
- 原 §4 → 本文 §4.2  
- 原 §5 → 融入 §4.3 与 §8.3  
- 原 §6–7 → 本文 §8.1–8.3  
