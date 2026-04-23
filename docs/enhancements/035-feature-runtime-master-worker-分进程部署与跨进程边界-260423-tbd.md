## Runtime Master / Worker 分进程部署与消除同进程函数级耦合（2026-04-23）

### 1. 背景

当前 AgentFlow Runtime 在同进程内同时承载 **调度与编排（偏 Master）** 与 **工作流实例与节点执行（偏 Worker）** 能力；`MMAgentRuntime` 等组件仍存在对 `MMNode`、Agent 内部 DataBus 的**直接 C++ 调用链**（参见 [026-feature-master-worker-架构边界问题-260320-tbd.md](026-feature-master-worker-架构边界问题-260320-tbd.md) 中的问题表与示例）。

集成文档已说明：若要将 Runtime 部署到独立进程或主机，**网络传输层需由集成方实现**（`publish/INTEGRATION_GUIDE.md`）。本 enhancement 将「分进程 / 分主机部署」提升为一等目标：在框架侧明确 **Master 与 Worker 的进程边界与 RPC 契约**，逐步去掉二者之间的**直接函数级依赖**，使 Worker 可作为**独立进程/二进制**启动，Master 仅通过 **消息或 RPC** 驱动执行。

本 issue 与 [034-feature-agentflow-双集成与跨节点databus适配层设计-260423-tbd.md](034-feature-agentflow-双集成与跨节点databus适配层设计-260423-tbd.md) 互补：**034** 侧重 DataBus 与 in-process/UDS 集成面；**035** 侧重 **Runtime 控制面** 的 Master↔Worker 解耦。

### 2. 目标

| 目标 | 说明 |
|------|------|
| 分进程部署 | Master 进程与 Worker 进程可独立启动、独立扩缩容；Worker 可多实例。 |
| 无直接函数耦合 | Master 不持有 `MMNode*` 执行路径、不直接 `node->set_*` / `process_node` 式调用 Worker 域对象；跨边界仅通过 **接口 + 序列化消息**。 |
| 契约稳定 | 定义版本化的 **控制面协议**（任务下发、心跳、取消、进度、错误码）；与 DataBus 数据面解耦。 |
| 渐进迁移 | 默认保留同进程「本地 Worker 适配器」实现，与「远程 Worker 客户端」共享同一套接口，便于分阶段替换。 |

### 3. 非目标（首阶段）

- 不强制规定具体网络栈（gRPC / HTTP2 / 自定义 TCP）；仅规定 **边界接口与消息语义**。
- 不改变 SO/脚本节点对 DataBus 的既有 UDS/环境变量契约（由 034/019 等覆盖），除非为跨进程路径统一错误模型所必需。

### 4. 现状与问题

#### 4.1 与 026 的关系

[026](026-feature-master-worker-架构边界问题-260320-tbd.md) 已列出 Master 侧越界：`process_node`、`set_databus_client`、`cloned_workflows_` 等应在 Worker 域。**035** 在 026 的职责模型之上，增加 **部署维度**：即使职责拆清，若仍链接在同一二进制且直接调用，则无法满足独立进程升级、资源隔离与故障域隔离。

#### 4.2 典型直接耦合形式（需消除）

- Master 线程直接调用 `MMWorkerEngine::execute_code` 同进程静态路径（见 [034](034-feature-agentflow-双集成与跨节点databus适配层设计-260423-tbd.md) 附录 A 对 `MMNode::execute_with_worker_engine` 的梳理）：分进程后应变为 **向 Worker 发送 ExecuteNodeRequest 并异步接收 ExecutionResponse**。
- Master 直接操作 `MMNode` 输入输出与 `MMDataBus` 指针：应收口为 Worker 侧本地逻辑或通过 **DataBus 远程/UDS**（034）访问。

### 5. 方案方向

#### 5.1 抽象「Worker 运行时端口」

引入稳定接口（命名示例，实施时以代码为准）：

- `IWorkerRuntime`：接收 `ScheduleTask`、`CancelTask`、`ReportHealth`；返回 `Ack` / 流式 `TaskEvent`。
- Master 侧仅依赖 `IWorkerRuntime`，由工厂注入：
  - `InProcessWorkerRuntime`：当前行为包装为适配器（同进程调用，便于回归）。
  - `RpcWorkerRuntime`：通过 UDS 或 gRPC 等发送 protobuf/JSON 消息。

#### 5.2 控制面与数据面分离

```mermaid
flowchart LR
  master[MMAgentRuntime_Master]
  rpc[WorkerControlChannel]
  workerProc[WorkerProcess]
  databus[MMDataBus_RemoteOrUDS]
  master --> rpc
  rpc --> workerProc
  workerProc --> databus
  master -.->|不直接调用MMNode| workerProc
```

- **控制面**：调度、部署、取消、版本协商。
- **数据面**：节点仍通过 DataBus（本地 + 远程适配 + UDS）读写；跨进程时依赖 034 的远程存储抽象与 UDS 协议。

#### 5.3 参考实践（真实链接）

- gRPC 作为 **进程间 / 机间** 统一 RPC 层，并支持 UDS 传输（与本仓库已有 UDS 文化一致）：  
  - <https://learn.microsoft.com/en-us/aspnet/core/grpc/interprocess-uds?view=aspnetcore-10.0>  
  - <https://chromium.googlesource.com/external/github.com/grpc/grpc/+/HEAD/examples/cpp/unix_abstract_sockets/README.md>
- **Sidecar / 多进程** 部署模型（职责隔离、独立生命周期）：  
  - <https://docs.dapr.io/concepts/overview/>（Dapr sidecar 与应用进程分离）
- **控制面与数据面分离** 的组件化思路（类比）：  
  - <https://opentelemetry.io/docs/collector/architecture/>（receiver / processor / exporter 分层）

### 6. 分阶段实施

| 阶段 | 内容 | 验收 |
|------|------|------|
| P1 | 提炼 `IWorkerRuntime` 与消息模型；`InProcessWorkerRuntime` 包装现有调用链 | 单测行为与现状一致 |
| P2 | 实现一种 **本地 UDS** 控制通道 + 独立 `agentflow_worker` 可执行入口（最小：执行单节点 smoke） | e2e：双进程启动跑通 |
| P3 | Master 侧删除对 Worker 域对象的直接头文件依赖（依赖倒置 + 消息-only） | 链接图/包含关系审计通过 |
| P4 | 文档：`INTEGRATION_GUIDE`、`ARCHITECTURE` 更新部署拓扑与配置开关 | 评审通过 |

### 7. 待修改文件（预期，随审计调整）

| 区域 | 说明 |
|------|------|
| `include/internal/runtime/`、`src/runtime/` | `MMAgentRuntime`、`Deployer`、`Scheduler` 与 Worker 启动路径 |
| `include/internal/runtime/worker.h`、`src/runtime/worker/` | `MMWorkerEngine` 拆分为库内核心与进程外入口或 RPC 适配层 |
| `CMakeLists.txt` | 可选 `agentflow_worker` 目标 |
| `publish/INTEGRATION_GUIDE.md`、`docs/ARCHITECTURE.md` | 分进程部署说明 |

### 8. 测试方案

- **回归**：P1 完成后全量现有单测/e2e 无行为变化。
- **双进程 e2e**：启动 worker 监听 UDS → master 下发简单 workflow → 断言 DataBus 结果。
- **故障**：Worker 崩溃重启、Master 取消任务、超时与背压。

### 9. 关联 enhancement

| 文档 | 关系 |
|------|------|
| [026](026-feature-master-worker-架构边界问题-260320-tbd.md) | 职责边界与错误放置代码清单 |
| [034](034-feature-agentflow-双集成与跨节点databus适配层设计-260423-tbd.md) | DataBus 远程适配与 UDS 数据面 |
| [019](019-feature-plugin-uds-注入机制设计-260317-implemented.md) | Plugin UDS |
| [027](027-feature-udsserver-线程池改造-260322-implemented.md) | UDS 服务端并发基础 |

### 10. 进展跟踪

- [ ] `IWorkerRuntime` 接口与消息 schema 评审
- [ ] `InProcessWorkerRuntime` 落地 + 测试绿
- [ ] 双进程最小 e2e
- [ ] Master 侧去除对 Worker 实现类型的直接依赖（链接/包含审计）
- [ ] 文档更新

### 11. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-04-23 | AI | 初始 issue：Master/Worker 分进程部署与消除直接函数耦合 |
