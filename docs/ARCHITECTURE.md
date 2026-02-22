# AgentFlow 架构文档

> 本文档按当前代码（`include/external`、`include/internal/runtime`、`src/runtime`、`src/workflow`）与 `docs/REQUIREMENTS_ANALYSIS.md` 对齐，聚焦可落地实现。

## 1. 架构总览

AgentFlow 由两层开发框架 + 一层运行时组成：

1. **Framework 层（开发编排）**
   - `Agent`：管理会话、消息、任务与 workflow 集合。
   - `Workflow`：由 node + edge 构成执行图，edge 承担流程控制。
2. **Runtime 层（执行调度）**
   - 负责 `task -> workflow instance` 调度。
   - 负责 `workflow instance -> worker` 部署与调度。
3. **共享基础设施（DataBus）**
   - 为 Agent/Workflow/Node 提供统一数据读写与订阅接口。

## 2. Framework 层

### 2.1 Agent

代码位置：
- `include/external/agent/agent.h`
- `src/agent/*`

职责：
- 管理 agent 生命周期与 workflow 挂载。
- 维护默认会话与按 `user_id + client_id` 创建的会话。
- 将会话消息交给 `AgentCoordinator` 生成 task。

### 2.2 Workflow

代码位置：
- `include/external/workflow/workflow.h`
- `src/workflow/workflow.cpp`
- `src/workflow/dsl/workflow_parser.cpp`

职责：
- 承载 node/edge 拓扑与执行状态。
- 解析 DSL（含 `includes`、`type=workflow`、`workflow_ref`）。
- 支持由 edge 控制流驱动（if/loop/fork/join/async/goto）。

### 2.3 Node 与 Edge 的边界

- **Node 只负责动作执行**：脚本、可执行文件、SO、Python、C++ 回调等。
- **Edge 负责流程控制**：通过 `edge.control` 定义分支、循环、并行、同步、异步与跳转。
- 这与需求分析中的“node 执行、edge 控制流”一致。

## 3. Runtime 层

代码位置：
- `include/internal/runtime/agent_runtime.h`
- `include/internal/runtime/master/deployer.h`
- `include/internal/runtime/master/scheduler.h`
- `include/internal/runtime/worker.h`
- `src/runtime/master/*`
- `src/runtime/worker.cpp`

### 3.1 角色模型

`AgentRuntime` 支持：
- `MASTER`：包含调度能力，也可执行 worker 能力。
- `WORKER`：仅执行能力，不包含全局调度能力。

### 3.2 两类核心调度

1. **task -> workflow instance**
   - 由 `runtime::master::Scheduler` 决策具体 workflow 实例。
2. **workflow instance -> worker**
   - 由 `runtime::master::Deployer` 维护 worker 注册、心跳、负载与映射。

### 3.3 Worker 执行

- Worker 执行 workflow 中可运行节点。
- 执行结果通过 runtime 回调更新 workflow 状态并触发后续调度。

## 4. 高速数据总线（DataBus）

代码位置：
- `include/internal/runtime/databus/databus.h`
- `src/runtime/databus/*`

能力：
- `put/get/remove/contains`
- key 级订阅回调（`subscribe/unsubscribe`）
- 本地与远端数据总线统一抽象（`LocalDatabus` + `RemoteDatabus`）
- DataRef 引用模型（消息/事件只传引用，不传大 payload）
- owner 索引检索（request/user/client/agent/session/task/workflow/node/master/worker）

使用边界：
- Agent、Workflow、Node 在执行链路中共享数据。
- 控制流求值与节点参数传递通过统一数据键空间协作。
- `msgque/event` 只做控制面协同，payload 统一写入 DataBus 本地缓存；本地未命中时由 DataBus 自动回源远端数据服务并回填。

## 5. 与需求分析文档的对齐结论

与 `docs/REQUIREMENTS_ANALYSIS.md` 的一致项：
- Agent 侧会话化消息处理与 task 生成。
- Runtime 的 master/worker 角色化部署。
- scheduler 与 deployer 分工：task 调度与 workflow 部署分离。
- DataBus 作为共享能力支撑流程执行。

## 6. 当前约束与后续建议

- 当前实现采用“本地缓存优先 + Redis 数据服务同步”的混合模式；当 Redis 不可用时保持本地缓存可用。
- 对包含复杂环回的 workflow，建议配合 `publish/workflow/pattern` 样例验证 DSL 与可视化一致性。
