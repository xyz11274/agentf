## 2026-04-23 agentflowd 可执行入口设计

### 1. 背景

AgentFlow 当前没有独立的可执行入口，使用者必须编写 C++ 代码调用框架 API（参考 `examples/features/agent_runner.cpp` 和 `publish/INTEGRATION_GUIDE.md`）。这意味着：

1. **集成门槛高** — 每个业务服务都需要重复实现 Runtime 创建、Agent 加载、消息路由、响应轮询等样板代码
2. **无标准运维接口** — 无法通过统一协议对运行中的 AgentFlow 实例进行管理（加载/卸载 Agent、查询状态、优雅关机等）
3. **多 Agent 部署复杂** — 每增加一个 Agent 都需要修改集成代码并重启服务
4. **不支持远程访问** — 当前所有交互都是进程内 C++ API 调用，外部服务（Python/Java/浏览器）无法直接使用

本设计引入 `agentflowd` 守护进程，将 AgentFlow 以独立进程形式运行，通过 JSON-RPC 2.0 over TCP 暴露完整的 Agent 管理和执行能力，使任何语言的客户端都能通过标准网络协议使用 AgentFlow。

---

### 2. 方案简述

将 AgentFlow 框架核心 API 封装为一个守护进程，对外提供网络 RPC 接口。核心思路：

1. **进程模型** — 单进程多 Agent，一个 `agentflowd` 实例加载多个 Agent 包，共享一个 `MMAgentRuntime`
2. **通信协议** — JSON-RPC 2.0 over TCP（与现有 UDSServer 协议一致），可选 WebSocket 升级支持浏览器客户端
3. **配置方式** — CLI 参数 + JSON 配置文件 + 环境变量，优先级：CLI > ENV > 配置文件 > 默认值
4. **响应模式** — `agent.execute` 异步返回，响应通过 JSON-RPC notification 推送；同时提供 `agent.poll_response` 同步轮询方式
5. **运维能力** — 动态 load/unload Agent、事件订阅、Metrics 导出、优雅关机

---

### 3. 约束与假设

- **依赖约束**：不引入 gRPC/protobuf/额外网络库，复用项目已有的 JSON adapter 和 socket API，遵循 `AGENTFLOW_WITH_*` 可选依赖模式
- **协议约束**：沿用 UDSServer 已验证的 JSON-RPC 2.0 协议格式，TCP 端仅增加连接管理和换行符帧
- **兼容性假设**：当前 `MMAgentRuntime` 已支持多 Agent 注册（`register_agent`），无需修改框架核心代码
- **不解决**：本方案不解决 Master-Worker 独立部署问题（enhancement 026），但协议设计为未来扩展预留空间（Worker 注册/心跳可建模为 JSON-RPC 方法）
- **配置格式**：使用 JSON（与 package.json / agent.json / workflow DSL 一致），使用已有的 `vendor/adapter/json/`

---

### 4. 当前实现分析

#### 代码分析

**现有入口模式**（`examples/features/agent_runner.cpp`）：

```
main()
  ├── 解析 CLI 参数
  ├── AgentPackage::load_from_directory(package_path)
  ├── MMAgent::create_from_package(package_path)
  ├── agent->initialize()
  ├── 三种模式: --list-workflows / --execute / --interactive
  └── InteractiveRunner (消息驱动模式)
       ├── 创建 MMAgentRuntime
       ├── runtime->register_agent(agent)
       ├── runtime->start()
       ├── agent->start()
       ├── 构造 Message, agent->route_request_message(msg)
       └── 轮询 agent->dequeue_response_message()
```

**集成指南模式**（`publish/INTEGRATION_GUIDE.md`）：

```
7 步生命周期: 创建 Agent → 初始化 → 创建 Runtime → 注册 Agent → 启动 Runtime
→ 提交请求 → 轮询响应
```

**现有 IPC 机制**：

| 组件 | 协议 | 作用域 | 限制 |
|------|------|--------|------|
| UDSServer | JSON-RPC 2.0 over UDS | DataBus 访问（per-session） | 仅限 DataBus 操作，不支持 Agent 管理/请求执行 |
| WorkerActor | 内存 ActorMessage | Worker → Master 心跳 | 进程内函数调用，无法跨进程 |
| MMAgentMsgQue | 内存队列 | 请求/响应消息传递 | 进程内队列，无法跨进程 |

**关键发现**：所有现有通信机制都是进程内的，没有任何网络级接口。

#### Examples 分析

| 示例 | 入口模式 | 启示 |
|------|---------|------|
| `agent_runner` | CLI 工具，单 Agent | 基础集成模式 |
| `interactive_runner` | 交互式消息驱动 | 请求/响应轮询模式 |
| `multi_session_chat` | TUI 多轮对话 | 多 Session 并发 |
| `llm-query-agent` | LLM 集成 | 长耗时 workflow 模式 |

#### 缺失功能分析

1. **网络监听** — 无 TCP/UDS 服务端监听
2. **请求路由** — 无法从网络接收请求并路由到 Agent
3. **响应推送** — 无法将 Agent 响应推送到远程客户端
4. **Agent 管理 RPC** — 无法远程加载/卸载/查询 Agent
5. **事件推送** — 无法将运行时事件（agent/session/dialogue/task）推送到远程订阅者
6. **配置持久化** — 无 JSON 配置文件加载机制
7. **信号处理** — 无 SIGINT/SIGTERM 优雅关机
8. **守护进程化** — 无 daemon fork / PID 文件管理

**行业实践参考**：

| 系统 | 协议 | 参考意义 |
|------|------|---------|
| PostgreSQL | TCP + UDS 双监听 | UDS 用于本地管理，TCP 用于远程连接 |
| Redis | RESP over TCP | 简单文本协议，换行符帧，易于调试 |
| Docker Engine | JSON-RPC over UDS + HTTP | UDS 本地管理 + HTTP 远程 API |
| LangServe | HTTP + SSE | 流式推送，但 SSE 单向限制明显 |

**具体参考点**：
- PostgreSQL 的 UDS/TCP 双通道模式 → 管理操作走 UDS，业务请求走 TCP
- Redis 的换行符帧 → 协议简洁，nc/telnet 可直接调试
- Docker Engine 的 UDS 优先 → 安全性：本地管理无需暴露到网络

---

### 5. 数据结构设计

#### 5.1 DaemonConfig

```cpp
struct DaemonConfig {
  // Server
  std::string listen = "0.0.0.0:17891";        // TCP 监听地址
  std::string uds_biz_path = "/tmp/agentflowd_biz.sock";   // UDS 业务套接字（请求执行、响应推送）
  std::string uds_ctrl_path = "/tmp/agentflowd_ctrl.sock";  // UDS 控制套接字（状态查询、Agent 管理、关机等）
  int         max_connections = 1024;           // 最大并发客户端连接

  // Runtime
  MMAgentRuntime::Config runtime_config;        // 复用现有 Runtime 配置
  Deployer::Config       deployer_config;       // 复用现有 Deployer 配置
  Scheduler::Config      scheduler_config;      // 复用现有 Scheduler 配置

  // Agents
  struct AgentEntry {
    std::string package_path;
    std::string name_override;
  };
  std::vector<AgentEntry> agents;               // 启动时加载的 Agent 列表

  // Logging
  std::string log_level = "info";
  std::string log_file;                         // 空 = stderr

  // Daemon
  std::string pid_file;
  bool        daemonize = false;
  int         drain_timeout_ms = 30000;         // 优雅关机超时
};
```

**设计思路**：复用现有 Config 结构体（`MMAgentRuntime::Config`, `Deployer::Config`, `Scheduler::Config`），DaemonConfig 仅添加守护进程层特有的配置，避免重复定义。

**兼容性**：新增结构体，不影响现有 API。

#### 5.2 JSON 配置文件格式

```json
{
  "server": {
    "listen": "0.0.0.0:17891",
    "uds_biz_path": "/tmp/agentflowd_biz.sock",
    "uds_ctrl_path": "/tmp/agentflowd_ctrl.sock",
    "max_connections": 1024
  },
  "runtime": {
    "role": "master",
    "worker_threads": 4,
    "local_worker_id": "runtime-local-worker",
    "worker_default_workflow_threads": 1,
    "worker_control_threads": 1,
    "worker_business_threads": 1,
    "worker_queue_size": 1024,
    "plugin_uds_path": ""
  },
  "deployer": {
    "schedule_interval_ms": 200,
    "heartbeat_timeout_ms": 10000
  },
  "scheduler": {
    "schedule_interval_ms": 100
  },
  "agents": [
    { "package_path": "/opt/agentflow/agents/travel-agent" },
    { "package_path": "/opt/agentflow/agents/shopping-agent" }
  ],
  "logging": { "level": "info", "file": "" },
  "daemon": { "pid_file": "", "drain_timeout_ms": 30000 }
}
```

字段与 `DaemonConfig` 一一对应，使用已有的 `vendor/adapter/json/` 解析。

#### 5.3 三路配置交叉映射与合并规则

**配置来源优先级**：CLI 参数 > 环境变量 > JSON 配置文件 > 内置默认值

**合并逻辑**：逐字段覆盖（非全量替换）。每个字段独立判断：高优先级来源设置了该字段则使用高优先级值，未设置则降级到下一优先级。空字符串/零值视为"未设置"。

**交叉映射表**：

| DaemonConfig 字段 | CLI 参数 | 环境变量 | JSON 配置键 | 默认值 | 备注 |
|---|---|---|---|---|---|
| `listen` | `--listen` | `AF_LISTEN` | `server.listen` | `0.0.0.0:17891` | |
| `uds_biz_path` | `--uds-biz-path` | `AF_UDS_BIZ_PATH` | `server.uds_biz_path` | `/tmp/agentflowd_biz.sock` | 业务 UDS |
| `uds_ctrl_path` | `--uds-ctrl-path` | `AF_UDS_CTRL_PATH` | `server.uds_ctrl_path` | `/tmp/agentflowd_ctrl.sock` | 控制 UDS |
| `max_connections` | `--max-connections` | — | `server.max_connections` | `1024` | 无环境变量 |
| `runtime_config.role` | `--role` | — | `runtime.role` | `master` | 无环境变量 |
| `runtime_config.worker_threads` | `--worker-threads` | — | `runtime.worker_threads` | `4` | |
| `runtime_config.local_worker_id` | `--worker-id` | — | `runtime.local_worker_id` | `runtime-local-worker` | |
| `runtime_config.worker_default_workflow_threads` | `--workflow-threads` | — | `runtime.worker_default_workflow_threads` | `1` | |
| `runtime_config.worker_control_threads` | `--control-threads` | — | `runtime.worker_control_threads` | `1` | |
| `runtime_config.worker_business_threads` | `--business-threads` | — | `runtime.worker_business_threads` | `1` | |
| `runtime_config.worker_queue_size` | `--queue-size` | — | `runtime.worker_queue_size` | `1024` | |
| `runtime_config.plugin_uds_path` | `--plugin-uds-path` | `AF_PLUGIN_UDS_FILE` | `runtime.plugin_uds_path` | `""` | 复用已有环境变量 |
| `deployer_config.schedule_interval_ms` | `--deploy-interval` | — | `deployer.schedule_interval_ms` | `200` | |
| `deployer_config.heartbeat_timeout_ms` | `--heartbeat-timeout` | — | `deployer.heartbeat_timeout_ms` | `10000` | |
| `scheduler_config.schedule_interval_ms` | `--schedule-interval` | — | `scheduler.schedule_interval_ms` | `100` | |
| `agents[]` | `--package`（可重复） | — | `agents[]` | `[]` | 仅 CLI 和配置文件，CLI 追加到文件列表后 |
| `log_level` | `--log-level` | `AF_LOG_LEVEL` | `logging.level` | `info` | |
| `log_file` | `--log-file` | `AF_LOG_FILE` | `logging.file` | `""` (stderr) | |
| `pid_file` | `--pid-file` | — | `daemon.pid_file` | `""` | |
| `drain_timeout_ms` | `--drain-timeout` | — | `daemon.drain_timeout_ms` | `30000` | |
| `daemonize` | `--daemonize` | — | — | `false` | **仅 CLI** |
| `config_file` | `--config` | `AF_CONFIG_FILE` | — | `""` | **配置源定位，不参与合并** |
| `validate` | `--validate` | — | — | `false` | **仅 CLI，一次性动作** |

**单源配置项说明**：

| 配置项 | 仅可用来源 | 原因 |
|--------|-----------|------|
| `daemonize` | 仅 CLI | 涉及 fork 行为，必须在进程启动最早期决定，不适合放配置文件或环境变量（启动时配置文件尚未加载） |
| `validate` | 仅 CLI | 一次性校验动作，不是运行时配置 |
| `config_file` | 仅 CLI / ENV | 配置文件路径本身是配置源的定位器，不能写在配置文件内（循环依赖） |
| `agents[]` | 仅 CLI / 配置文件 | 列表类型，环境变量不适合表达；CLI `--package` 可多次指定，追加到配置文件 `agents[]` 列表后面 |

**`agents[]` 合并规则**：CLI 的 `--package` 和配置文件的 `agents[]` 为**追加关系**（非覆盖），最终列表 = 文件列表 + CLI 列表。若同一 `package_path` 出现在两处，去重（以先出现的为准）。

**冲突示例**：

```
场景: config.json 中 "worker_threads": 8, CLI 传 --worker-threads 4
结果: worker_threads = 4  (CLI 优先)

场景: config.json 中 "listen": "0.0.0.0:9000", 无 CLI 参数, AF_LISTEN=127.0.0.1:17891
结果: listen = "127.0.0.1:17891"  (ENV 优先于文件)

场景: config.json 中无 deployer 节, CLI 传 --heartbeat-timeout 5000
结果: heartbeat_timeout_ms = 5000  (CLI), 其余 deployer 字段用默认值
```

#### 5.4 InFlightRequest（响应路由）

```cpp
struct InFlightRequest {
  std::string message_id;              // 消息唯一 ID，用于响应关联
  std::string agent_id;                // 目标 Agent ID
  int         client_connection_fd;     // 发起请求的客户端连接 FD（TCP 或 UDS）
  int64_t     submitted_at_ms;         // 提交时间
  std::string dialogue_id;             // 对话 ID（multi-response 场景下用于关联同一对话的所有响应）
};
```

**响应完成判定**：单次 `agent.execute` 可能触发多轮 skill 执行（dispatcher → skill-selection → skill1 + skill2 → coordinator → skill3 → coordinator），产生多条中间响应和一条最终响应。判定完成的方式：
1. `final: true` 标记 — Coordinator 完成聚合后推送的最终 notification 携带此标记
2. quiet window — `runtime->get_running_nodes() == 0` 且 `agent->get_response_queue_size() == 0` 持续 200ms
3. 超时 — `agent.poll_response` 的 `timeout_ms` 参数

#### 5.5 EventSubscription（事件订阅）

```cpp
struct EventSubscription {
  uint64_t                         subscription_id;
  int                              client_connection_fd;
  std::set<std::string>            event_types;  // "agent","session","dialogue","task"
  std::string                      agent_id;     // 空 = 所有 Agent
};
```

---

### 6. 流程设计

#### 6.1 启动流程

```
main()
  │
  ├── 1. 解析 CLI 参数
  ├── 2. 加载配置（CLI > ENV > 配置文件 > 默认值）
  ├── 3. 初始化日志（Logger 设置 level/file）
  ├── 4. daemonize（如果 --daemonize）
  ├── 5. 注册信号处理（SIGINT/SIGTERM → 优雅关机）
  ├── 6. 创建 MMAgentRuntime(runtime_config)
  ├── 7. 加载 Agent 包（遍历 agents[]）
  │     ├── 切换 CWD 到 package_path（Agent 包内路径为相对路径）
  │     ├── create_from_package(package_path)
  │     ├── agent->initialize()
  │     ├── 恢复 CWD
  │     └── register_agent(agent)
  ├── 8. runtime->start()        ← 启动 task poller、worker 线程
  ├── 9. agent->start()          ← 启动 intent_request/data_exchange 消息处理线程
  │     （必须在 runtime->start() 之后，否则 task poller 未就绪）
  ├── 10. 创建 EventBroker → register_event_handler
  ├── 11. 创建 ResponsePoller → 启动轮询线程
  ├── 12. 启动 RPC Server（TCP listener + UDS 业务 listener + UDS 控制 listener）
  ├── 13. 写 PID 文件
  └── 14. 主线程阻塞等待关机信号
```

**关键顺序约束**：
1. `runtime->start()` 必须在 `agent->start()` 之前 — Runtime 的 `agent_task_poller_loop` 消费 `agent->pop_generated_task()`，如果 Agent 先启动，生成的 task 无人消费会堆积
2. Agent 包加载时必须切换 CWD — `create_from_package` 内部解析 workflow JSON 和 SO library 路径时使用相对路径（如 `workflow_file: "coordinator_workflow.json"`），CWD 必须指向 Agent 包目录
3. 多 Agent 场景下逐个加载 — 每次加载切换到对应包目录，加载完成后恢复

#### 6.2 请求执行流程

```
客户端                        agentflowd                         AgentFlow
  │                              │                                  │
  │ 1. agent.execute             │                                  │
  │ {agent_id, user_id,          │                                  │
  │  client_id, content,         │                                  │
  │  trace_labels?, dialogue_id?}│                                  │
  │ ──────────────────────────►  │                                  │
  │                              │ 2. 查找 Agent                    │
  │                              │ 3. 构造 intent_request Message   │
  │                              │ 4. 记录 InFlightRequest          │
  │                              │ 5. agent->route_request_message  │
  │                              │ ────────────────────────────────► │
  │                              │                                  │
  │ 6. 返回 {message_id,         │                    7. SessionRouter 路由
  │    status: "submitted"}      │                    8. Dispatcher 处理
  │ ◄──────────────────────────  │                    9. next_skills → launch skill
  │                              │                   10. Runtime 执行 skill workflow
  │                              │                   11. Coordinator 聚合结果
  │                              │                   12. Agent 结果入 response queue
  │                              │                                  │
  │                              │ 13. ResponsePoller 轮询          │
  │                              │ ◄──────────────────────────────── │
  │                              │ 14. 匹配 InFlightRequest         │
  │                              │ 15. 构造 JSON-RPC notification   │
  │                              │                                  │
  │ 16. notification             │                                  │
  │ {method: "agent.response",   │                                  │
  │  params: {message_id,        │                                  │
  │   content, metadata,         │                                  │
  │   final: true/false}}        │                                  │
  │ ◄──────────────────────────  │                                  │
  │                              │                                  │
  │ ... (multi-response 重复推送) │                                  │
  │                              │                                  │
  │ 17. final notification       │                                  │
  │ {final: true, content, ...}  │                                  │
  │ ◄──────────────────────────  │                                  │
```

**响应完成判定**：
- `final: true` 标记表示该请求的所有响应已推送完毕
- 客户端也可使用 `agent.poll_response` 同步等待，其中 `complete=true` 由以下条件综合判定：
  - `runtime->get_running_nodes() == 0`
  - `agent->get_response_queue_size() == 0`
  - quiet window（默认 200ms 无新响应）

#### 6.3 优雅关机流程

```
SIGINT/SIGTERM
      │
      v
Phase 1: DRAINING (drain_timeout_ms)
  ├── 停止接受新连接和新请求（返回 "shutting_down" 错误）
  ├── 等待 runtime_->get_running_nodes() == 0
  ├── 等待所有 agent->get_response_queue_size() == 0
  └── 超时后强制进入 Phase 2
      │
      v
Phase 2: STOPPING
  ├── agent_->stop() 逐个停止
  ├── runtime_->stop()
  ├── 关闭所有客户端连接
  ├── 清理 UDS socket 文件（biz + ctrl）
  └── 删除 PID 文件
      │
      v
Phase 3: EXIT (_exit(0))

第二次信号 → 强制退出
```

---

### 7. 接口设计

#### 7.1 CLI 接口

```
agentflowd [options]

Agent 加载:
  --package PATH           Agent 包目录（可重复，多 Agent）
  --validate               校验包结构后退出

服务:
  --listen ADDR:PORT       TCP 监听地址（默认 0.0.0.0:17891）
  --uds-biz-path PATH      UDS 业务套接字路径（默认 /tmp/agentflowd_biz.sock）
  --uds-ctrl-path PATH     UDS 控制套接字路径（默认 /tmp/agentflowd_ctrl.sock）
  --max-connections N      最大并发客户端连接（默认 1024）

Runtime:
  --role ROLE              master|worker（默认 master）
  --worker-threads N       Worker 线程池大小（默认 4）
  --worker-id ID           本地 Worker ID

运维:
  --config FILE            从 JSON 配置文件加载
  --daemonize              以守护进程模式运行
  --pid-file FILE          PID 文件路径
  --log-level LEVEL        trace|debug|info|warn|error（默认 info）
  --log-file FILE          日志文件（默认 stderr）
  --drain-timeout MS       优雅关机超时（默认 30000）

  --version / --help
```

环境变量沿用 `AF_` 前缀：`AF_CONFIG_FILE`, `AF_LISTEN`, `AF_UDS_BIZ_PATH`, `AF_UDS_CTRL_PATH`, `AF_LOG_LEVEL`, `AF_LOG_FILE`

#### 7.2 RPC 方法接口

协议：JSON-RPC 2.0 over TCP/UDS，换行符分隔帧。可选 WebSocket 升级。

**三通道设计**：

| 通道 | 用途 | 允许的 RPC 方法 | 说明 |
|------|------|----------------|------|
| TCP (`--listen`) | 业务请求 + 远程访问 | 全部 | 支持跨机器访问，可选 WebSocket 升级 |
| UDS 业务 (`--uds-biz-path`) | 业务请求 + 本地服务集成 | `agent.execute`, `agent.poll_response`, `agent.send_control`, `databus.*`, `event.*` | 本地零网络开销，af-broker 等本地适配层连接此通道 |
| UDS 控制 (`--uds-ctrl-path`) | 运维管理 + 本地管理 | `agent.load`, `agent.unload`, `agent.list`, `agent.info`, `agent.pause`, `agent.resume`, `session.*`, `workflow.*`, `worker.*`, `runtime.*`, `metrics.*`, `health` | 仅本地访问，不暴露到网络；控制操作与业务流量隔离，互不影响 |

**设计理由**：

1. **业务/控制隔离** — 高频业务请求（`agent.execute`）和低频管理操作（`agent.load`、`runtime.shutdown`）走不同通道，避免管理操作被业务流量挤占或阻塞
2. **安全边界** — 控制 UDS 仅本地可访问（文件权限控制），不暴露到网络；业务 UDS 可通过 af-broker 等适配层转换为 WebSocket/HTTP 对外
3. **适配层模式** — 外部平台（Web 应用、移动端、第三方系统）不直接连 agentflowd，而是通过 af-broker 等中间适配层，适配层连接本地 UDS 通道

**af-broker 适配示意**：

```
┌──────────────┐    WebSocket     ┌──────────────┐    UDS 业务    ┌──────────────┐
│  Web/移动端   │ ◄─────────────► │  af-broker   │ ◄────────────► │  agentflowd  │
│  外部客户端   │    (业务流量)    │  (Go 进程)   │   (JSON-RPC)   │  (C++ 守护)  │
└──────────────┘                  └──────┬───────┘                └──────────────┘
                                         │
                                         │ UDS 控制
                                         │ (JSON-RPC)
                                         ▼
                                  ┌──────────────┐    HTTP       ┌──────────────┐
                                  │  af-broker   │ ◄─────────── │  运维/管理    │
                                  │  (HTTP API)  │   (管理操作)  │  工具/脚本    │
                                  └──────────────┘              └──────────────┘
```

**Agent 管理**

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `agent.load` | `{package_path, name_override?}` | `{agent_id, name, version, workflows[]}` | 加载 Agent 包并启动 |
| `agent.unload` | `{agent_id}` | `{success}` | 停止并卸载 Agent |
| `agent.list` | `{}` | `{agents: [{agent_id, name, version, state, workflow_count}]}` | 列出已加载 Agent |
| `agent.info` | `{agent_id}` | `{agent_id, name, version, state, config, workflows[]}` | Agent 详情 |
| `agent.pause` | `{agent_id}` | `{success}` | 暂停 Agent |
| `agent.resume` | `{agent_id}` | `{success}` | 恢复 Agent |

**请求执行**

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `agent.execute` | `{agent_id, user_id, client_id, content, trace_labels?, dialogue_id?, skill_name?}` | `{message_id, status: "submitted"}` | 提交 intent_request，由 dispatcher 自动路由到 skill workflow，响应通过 notification 推送 |
| `agent.poll_response` | `{agent_id, timeout_ms?, quiet_window_ms?}` | `{messages: [{content, metadata}], complete}` | 同步轮询响应（complete=true 表示 workflow 执行完毕） |
| `agent.send_control` | `{agent_id, command: "SESSION-INTERRUPT"\|"SESSION-DESTROY", session_id, user_id?}` | `{success}` | 发送控制命令 |

**`agent.execute` 参数说明**：

| 参数 | 必需 | 说明 |
|------|------|------|
| `agent_id` | 是 | 目标 Agent ID |
| `user_id` | 是 | 用户标识，用于 Session 路由（同 user_id + client_id 复用同一 Session） |
| `client_id` | 是 | 客户端标识，用于 Session 路由 |
| `content` | 是 | 用户输入内容 |
| `trace_labels` | 否 | 全链路跟踪标签，格式 `"key1=val1\|key2=val2"`，传递给 FlowTrace 和 TraceManager |
| `dialogue_id` | 否 | 对话 ID，用于多对话隔离。不提供时由框架自动生成 |
| `skill_name` | 否 | 指定目标 skill 名称（绕过 dispatcher 直接触发 skill，仅限特殊场景） |

**注意**：业务客户端不应直接指定 `workflow_id`——消息类型固定为 `intent_request`，由 Agent 内部的 dispatcher 自动选择 skill workflow 执行。

**Session 管理**

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `session.list` | `{agent_id}` | `{sessions: [{session_id, user_id, client_id}]}` | 列出 Session |
| `session.destroy` | `{agent_id, session_id}` | `{success, keys_removed}` | 销毁 Session |
| `session.clear_dialogue` | `{agent_id, session_id, dialogue_id}` | `{success, keys_removed}` | 清理 Dialogue 数据 |

**Workflow / Worker / Runtime**

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `workflow.list` | `{agent_id}` | `{workflows: [{id, name, node_count}]}` | 列出 Workflow |
| `workflow.info` | `{agent_id, workflow_id}` | `{id, name, nodes[], edges[]}` | Workflow 详情 |
| `workflow.list_instances` | `{}` | `{instances: [{instance_id, workflow_id, worker_id}]}` | 已部署实例 |
| `worker.list` | `{}` | `{workers: [{worker_id, status, load, last_heartbeat_ms}]}` | Worker 列表 |
| `runtime.status` | `{}` | `{role, running, active_agents, active_workflows, running_nodes, uptime_ms}` | 守护进程状态 |
| `runtime.shutdown` | `{drain_timeout_ms?}` | `{success}` | 触发优雅关机 |
| `health` | `{}` | `{status: "ok", version, uptime_ms}` | 健康检查 |

**DataBus 远程访问**（复用 UDSServer 方法定义）

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `databus.put` | `{agent_id, session_id, namespace, key, value}` | `{success}` | 存储值 |
| `databus.get` | `{agent_id, session_id, namespace, key}` | `{exists, value}` | 读取值 |
| `databus.remove` | `{agent_id, session_id, namespace, key}` | `{success}` | 删除键 |
| `databus.contains` | `{agent_id, session_id, namespace, key}` | `{exists}` | 检查存在 |
| `databus.subscribe` | `{agent_id, session_id, namespace, key}` | `{subscription_id}` | 订阅变更 |
| `databus.unsubscribe` | `{subscription_id}` | `{success}` | 取消订阅 |
| `databus.publish` | `{agent_id, session_id, namespace, topic, value}` | `{success}` | 发布事件 |
| `databus.list_keys` | `{agent_id, session_id, namespace, prefix?}` | `{keys[]}` | 列举键 |

**事件订阅**（基于 `IEventHandler` 调度事件回调）

框架提供 `IEventHandler` 接口（`include/internal/runtime/event_handler.h`）用于订阅 4 类调度生命周期事件。`EventBroker` 实现此接口，将事件转换为 JSON-RPC notification 推送给订阅客户端。

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `event.subscribe` | `{event_types: ["agent","session","dialogue","task"], agent_id?}` | `{subscription_id}` | 订阅调度生命周期事件 |
| `event.unsubscribe` | `{subscription_id}` | `{success}` | 取消订阅 |

**事件类型与 notification 格式**（映射自 `IEventHandler` 的 4 个虚方法）：

| 事件类型 | EventType 枚举 | notification method | params 字段 |
|---------|---------------|-------------------|------------|
| agent | `AGENT_REGISTERED`, `AGENT_UNREGISTERED` | `event.agent` | `type`, `agent_id`, `name`, `version`, `workflows[]` |
| session | `SESSION_CREATED`, `SESSION_DESTROYED` | `event.session` | `type`, `agent_id`, `user_id`, `client_id`, `session_id` |
| dialogue | `DIALOGUE_CREATED`, `DIALOGUE_DESTROYED` | `event.dialogue` | `type`, `agent_id`, `user_id`, `client_id`, `session_id`, `dialogue_id`, `user_input` |
| task | `TASK_CREATED`, `TASK_DESTROYED` | `event.task` | `type`, `agent_id`, `user_id`, `session_id`, `dialogue_id`, `task_id`, `workflow_id`, `task_type`, `skill_name` |

**notification 示例**：

```json
{"jsonrpc": "2.0", "method": "event.agent", "params": {"type": "AGENT_REGISTERED", "agent_id": "agent-1", "name": "TravelBot", "version": "1.0"}}
{"jsonrpc": "2.0", "method": "event.task", "params": {"type": "TASK_CREATED", "agent_id": "agent-1", "session_id": "sess-0", "task_id": "task-3", "workflow_id": "travel_workflow", "task_type": "skill", "skill_name": "travel"}}
```

**EventBroker 实现**：通过 `MMAgentRuntime::register_event_handler()` 注册，事件投递使用 Runtime 的 callback thread pool（线程池大小可通过 `set_callback_thread_pool_size()` 配置），不阻塞调度主路径。

**Metrics**

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `metrics.get` | `{format: "json"\|"prometheus"}` | `{metrics: ...}` | 导出指标 |

#### 7.3 协议设计

**传输层**：TCP 长连接，换行符 `\n` 分隔 JSON 帧

**帧格式**：与 UDSServer 一致，每行一个 JSON-RPC 2.0 消息

```
请求:  {"jsonrpc":"2.0","method":"agent.execute","params":{...},"id":1}\n
响应:  {"jsonrpc":"2.0","result":{"message_id":"...","status":"submitted"},"id":1}\n
通知:  {"jsonrpc":"2.0","method":"agent.response","params":{...}}\n
```

**WebSocket 升级**：客户端在同一端口发送 HTTP Upgrade 请求，服务端完成握手后切换为 WebSocket 帧格式，JSON-RPC 消息体不变。为浏览器客户端提供通路。

**UDS 本地通道**：与 TCP 相同的 JSON-RPC 协议，通过 UDS 套接字提供本地访问，适合同机服务集成（零网络开销、同进程级延迟）。

---

### 8. 实现方案

#### 8.1 核心组件

**Daemon** — 编排引擎，负责初始化、启动、关机协调

```cpp
class Daemon {
public:
  explicit Daemon(const DaemonConfig& config);
  bool initialize();    // 创建 Runtime, 加载 Agents, 启动 RPC Server
  bool run();           // 主循环，阻塞等待关机
  void shutdown();      // 触发优雅关机

private:
  DaemonConfig                        config_;
  std::unique_ptr<MMAgentRuntime>     runtime_;
  std::unique_ptr<AgentRegistry>      registry_;
  std::unique_ptr<RpcServer>          rpc_server_;
  std::unique_ptr<ResponsePoller>     response_poller_;
  std::unique_ptr<EventBroker>        event_broker_;
  std::atomic<ShutdownPhase>          shutdown_phase_{ShutdownPhase::NONE};
};
```

**RpcServer** — TCP + 双 UDS 监听 + JSON-RPC 分发，三通道方法访问控制

```cpp
// 通道类型，用于方法访问控制
enum class ChannelType {
  TCP,       // 全部方法
  UDS_BIZ,   // 业务方法: agent.execute/poll_response/send_control, databus.*, event.*
  UDS_CTRL   // 控制方法: agent.load/unload/list/info/pause/resume, session.*, workflow.*, worker.*, runtime.*, metrics.*, health
};

class RpcServer {
public:
  bool start(const std::string& listen_addr,
             const std::string& uds_biz_path,
             const std::string& uds_ctrl_path);
  void stop();
  void register_handler(const std::string& method, RpcMethodHandler handler);
  void send_notification(int client_fd, const std::string& method, const std::string& params_json);

  // 方法访问控制：每个通道维护允许的方法集合
  static bool is_method_allowed(ChannelType channel, const std::string& method);

private:
  void tcp_accept_loop();
  void uds_biz_accept_loop();
  void uds_ctrl_accept_loop();
  void client_handler(int fd, ChannelType channel);
  // 三个监听线程，共享 handler 注册表和 notification 通道
};
```

**AgentRegistry** — 多 Agent 管理

```cpp
class AgentRegistry {
public:
  bool load_agent(const std::string& package_path, const std::string& name_override,
                  MMAgentRuntime& runtime);
  bool unload_agent(const std::string& agent_id, MMAgentRuntime& runtime);
  std::shared_ptr<MMAgent> find(const std::string& agent_id);
  std::vector<AgentEntry> list_all() const;

private:
  struct AgentEntry {
    std::string agent_id;
    std::shared_ptr<MMAgent> agent;
    std::string package_path;     // 绝对路径，用于 CWD 切换和 SO 库校验
    std::string name;
    std::string version;
  };
  std::map<std::string, AgentEntry> agents_;
  mutable std::mutex mutex_;
};
```

**CWD 处理**：`load_agent` 在创建 Agent 时临时切换到 `package_path`（Agent 包内使用相对路径引用 workflow JSON 和 SO library），加载完成后恢复原 CWD。多 Agent 场景下每个 Agent 的 SO library 路径在 `process_node` 时通过绝对路径解析。

**ResponsePoller** — 后台轮询所有 Agent 响应并路由到客户端

```cpp
class ResponsePoller {
public:
  void start(AgentRegistry& registry, RpcServer& server);
  void stop();
  void register_request(const std::string& message_id, int client_fd,
                        const std::string& agent_id, const std::string& dialogue_id);

private:
  void poll_loop();  // 每 50ms 轮询所有 agent 的 dequeue_response_message()
  std::map<std::string, InFlightRequest> in_flight_;
};
```

**关于 `process_all_sessions_until_idle()`**：现有 e2e 测试在轮询响应前调用此方法来驱动 session 消息处理。在守护进程模式下，`agent->start()` 已启动 intent_request/data_exchange 处理线程，`runtime->start()` 已启动 `agent_task_poller_loop`，这些后台线程持续驱动消息流转，因此 ResponsePoller **不需要**再调用 `process_all_sessions_until_idle()`。但如果在极端场景下响应卡住（coordinator 协同等待），可考虑在 poll_loop 中增加超时后的 fallback 调用。

**EventBroker** — 实现 `IEventHandler`，桥接到 JSON-RPC notification

```cpp
class EventBroker : public IEventHandler {
public:
  void start(MMAgentRuntime& runtime, RpcServer& server);
  uint64_t subscribe(int client_fd, const std::set<std::string>& event_types, const std::string& agent_id);
  void unsubscribe(uint64_t subscription_id);

  // IEventHandler 接口
  void on_agent_event(const AgentEvent& event) override;
  void on_session_event(const SessionEvent& event) override;
  void on_dialogue_event(const DialogueEvent& event) override;
  void on_task_event(const TaskEvent& event) override;
};
```

#### 8.2 信号处理

```cpp
std::atomic<ShutdownPhase> g_shutdown_phase{ShutdownPhase::NONE};

void signal_handler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    if (g_shutdown_phase == ShutdownPhase::NONE) {
      g_shutdown_phase = ShutdownPhase::DRAINING;
    } else {
      g_shutdown_phase = ShutdownPhase::FORCED;  // 第二次信号强制退出
    }
  }
}

// main 中注册
struct sigaction sa{};
sa.sa_handler = signal_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
sigaction(SIGINT, &sa, nullptr);
sigaction(SIGTERM, &sa, nullptr);
```

#### 8.3 JSON-RPC 方法分发复用

UDSServer 已有 `handle_request(const std::string& request_json) -> std::string` 的纯函数模式。RPC Server 的方法分发可直接复用此模式：

```cpp
// RpcHandler 方法签名，与 UDSServer::handle_request 一致
using RpcMethodHandler = std::function<std::string(const std::string& params, int64_t id)>;

// 注册方法
handler.register_handler("agent.list", [this, &registry](const std::string& params, int64_t id) {
  auto agents = registry.list_all();
  // 构造 JSON 响应...
  return build_success_response(result_json, id);
});
```

#### 8.4 WebSocket 升级

最小实现：检测 HTTP Upgrade 请求，完成握手后切换为 WebSocket 帧格式。

```cpp
// 检测 WebSocket 升级请求
bool is_ws_upgrade(const std::string& first_line) {
  return first_line.find("Upgrade: websocket") != std::string::npos ||
         first_line.find("GET /") != std::string::npos;
}

// 握手响应
std::string ws_handshake_response(const std::string& sec_ws_key);

// WebSocket 帧解包/打包（mask/unmask，~50 行代码）
```

#### 8.5 af-broker 适配层服务

af-broker 是一个 Go 语言实现的适配层服务，演示外部平台如何通过标准 Web 协议接入 agentflowd。其核心职责是将外部平台的请求翻译为 agentflowd 的 JSON-RPC 调用。

**设计目标**：
1. **外部接入** — 对外暴露 WebSocket（业务流量）和 HTTP（管理操作），使浏览器/移动端/第三方系统能使用 AgentFlow
2. **协议转换** — WebSocket ↔ JSON-RPC over UDS-biz，HTTP ↔ JSON-RPC over UDS-ctrl
3. **会话映射** — 将 WebSocket 连接映射为 agentflowd 的 user_id + client_id，维持会话亲和性
4. **流式推送** — agentflowd 的 JSON-RPC notification 通过 WebSocket 推送到对应客户端

**项目结构**：

```
examples/features/service/af-broker/
  go.mod                   # module github.com/agentflow/af-broker
  main.go                  # 入口: 配置加载, 启动 WebSocket + HTTP 服务
  config.go                # BrokerConfig: 监听地址, UDS 路径, 日志等
  uds_client.go            # UDS JSON-RPC 2.0 客户端（连接管理, 换行符帧, 请求/响应/通知）
  ws_handler.go            # WebSocket 连接处理, 会话映射, 消息转发
  http_handler.go          # HTTP API 路由, 参数转换, JSON-RPC 调用
  session_map.go           # WebSocket 连接 ↔ agentflowd session 映射
```

**配置**（`BrokerConfig`）：

```go
type BrokerConfig struct {
    // 对外监听
    WSListen   string // WebSocket 监听地址 (默认 "0.0.0.0:8080")
    HTTPListen string // HTTP 监听地址 (默认 "127.0.0.1:8081")

    // agentflowd UDS 连接
    UDSBizPath  string // 业务 UDS 路径 (默认 "/tmp/agentflowd_biz.sock")
    UDSCtrlPath string // 控制 UDS 路径 (默认 "/tmp/agentflowd_ctrl.sock")

    // 转发配置
    AgentID      string // 默认 Agent ID (多 Agent 时按需指定)
    PollTimeout  int    // agent.poll_response 超时 ms (默认 5000)
    QuietWindow  int    // quiet window ms (默认 200)

    // 会话配置
    SessionTTL   int    // WebSocket 断开后 session 保留秒数 (默认 300)
}
```

**WebSocket 协议（业务通道）**：

WebSocket 连接用于双向业务通信，消息格式为 JSON，映射到 agentflowd 的 JSON-RPC 方法：

| 客户端 → af-broker (WebSocket JSON) | af-broker → agentflowd (UDS-biz JSON-RPC) | 说明 |
|-------------------------------------|-------------------------------------------|------|
| `{"type": "execute", "content": "...", "trace_labels?": "...", "dialogue_id?": "..."}` | `agent.execute` | 提交用户请求 |
| `{"type": "poll", "timeout_ms?": N}` | `agent.poll_response` | 主动轮询响应 |
| `{"type": "control", "command": "SESSION_INTERRUPT", "session_id": "..."}` | `agent.send_control` | 控制命令 |
| `{"type": "subscribe", "event_types": [...]}` | `event.subscribe` | 事件订阅 |
| `{"type": "databus.put", ...}` | `databus.put` | DataBus 操作 |

af-broker → 客户端推送：

| af-broker ← agentflowd (UDS-biz notification) | 客户端 ← af-broker (WebSocket JSON) | 说明 |
|-----------------------------------------------|-------------------------------------|------|
| `agent.response` notification | `{"type": "response", "content": "...", "metadata": {...}, "final": true/false}` | 响应推送 |
| `event.*` notification | `{"type": "event", "event_type": "task", "params": {...}}` | 事件推送 |

**HTTP API（控制通道）**：

| HTTP 方法 | 路径 | 映射到 UDS-ctrl JSON-RPC | 说明 |
|-----------|------|--------------------------|------|
| GET | `/api/agents` | `agent.list` | 列出 Agent |
| GET | `/api/agents/{id}` | `agent.info` | Agent 详情 |
| POST | `/api/agents` | `agent.load` | 加载 Agent |
| DELETE | `/api/agents/{id}` | `agent.unload` | 卸载 Agent |
| POST | `/api/agents/{id}/pause` | `agent.pause` | 暂停 |
| POST | `/api/agents/{id}/resume` | `agent.resume` | 恢复 |
| GET | `/api/agents/{id}/sessions` | `session.list` | 列出 Session |
| DELETE | `/api/agents/{id}/sessions/{sid}` | `session.destroy` | 销毁 Session |
| DELETE | `/api/agents/{id}/sessions/{sid}/dialogues/{did}` | `session.clear_dialogue` | 清理 Dialogue |
| GET | `/api/agents/{id}/workflows` | `workflow.list` | 列出 Workflow |
| GET | `/api/agents/{id}/workflows/{wid}` | `workflow.info` | Workflow 详情 |
| GET | `/api/workflow-instances` | `workflow.list_instances` | 已部署实例 |
| GET | `/api/workers` | `worker.list` | Worker 列表 |
| GET | `/api/runtime/status` | `runtime.status` | 运行状态 |
| POST | `/api/runtime/shutdown` | `runtime.shutdown` | 优雅关机 |
| GET | `/api/health` | `health` | 健康检查 |
| GET | `/api/metrics` | `metrics.get` (format=json) | Metrics |
| GET | `/api/metrics/prometheus` | `metrics.get` (format=prometheus) | Prometheus 格式 |

**会话映射**：

```go
// SessionMap 维护 WebSocket 连接与 agentflowd session 的映射
type SessionMap struct {
    mu       sync.RWMutex
    connMap  map[string]*SessionInfo  // ws_conn_id → SessionInfo
}

type SessionInfo struct {
    WsConnID   string           // WebSocket 连接 ID
    AgentID    string           // 目标 Agent ID
    UserID     string           // 由 af-broker 分配（如 ws_conn_id 或客户端指定）
    ClientID   string           // 由 af-broker 分配（标识 af-broker 实例）
    SessionID  string           // agentflowd 返回的 session ID
    DialogueID string           // 当前对话 ID
    UDSConn    *UDSConnection   // 对应的 UDS-biz 连接（可复用）
    LastActive time.Time        // 最后活跃时间
}
```

**映射策略**：
- 每个 WebSocket 连接自动分配 `user_id = "ws-{conn_id}"`，`client_id = "af-broker-{instance}"`
- 同一 WebSocket 连接的多次 `execute` 请求复用同一 session（通过 `get_or_create_session` 自动实现）
- `dialogue_id` 可由客户端指定（多对话隔离），不指定时使用默认对话
- WebSocket 断开后 session 保留 `SessionTTL` 秒，超时后通过 `session.destroy` 清理

**UDS 客户端连接管理**：

```go
// UDSConnection 管理 JSON-RPC 2.0 over UDS 连接
type UDSConnection struct {
    conn       net.Conn           // UDS 连接
    encoder    *json.Encoder      // JSON 编码器（写入请求）
    scanner    *bufio.Scanner     // 换行符分隔扫描器（读取响应/通知）
    pending    map[int64]chan *RPCResponse  // id → 等待通道
    notifyChan chan *RPCNotification         // notification 广播通道
    mu         sync.Mutex
    nextID     int64
}

// Connect 建立到 UDS 的连接，启动读取 goroutine
func Connect(udsPath string) (*UDSConnection, error)

// Call 发送 JSON-RPC 请求并等待响应
func (c *UDSConnection) Call(method string, params interface{}) (*RPCResponse, error)

// Notifications 返回 notification 通道（用于推送）
func (c *UDSConnection) Notifications() <-chan *RPCNotification
```

**业务 UDS 连接池**：多个 WebSocket 客户端共享少量 UDS-biz 连接（默认 1 个，可配置），notification 从共享连接广播到对应 WebSocket 客户端。UDS-ctrl 连接按需创建（HTTP 请求到达时调用，无状态）。

**部署示意**：

```
┌──────────────┐  WebSocket      ┌──────────────────┐  UDS-biz     ┌──────────────┐
│  Web 应用     │ ◄────────────► │  af-broker       │ ◄──────────► │  agentflowd  │
│  (浏览器)     │  :8080         │  (Go 进程)       │  JSON-RPC    │  (C++ 守护)  │
└──────────────┘                 │                  │              └──────────────┘
                                 │                  │
┌──────────────┐  WebSocket      │                  │  UDS-ctrl    ┌──────────────┐
│  移动端 SDK   │ ◄────────────► │                  │ ◄──────────► │  agentflowd  │
│              │  :8080         │                  │  JSON-RPC    │  (同上)       │
└──────────────┘                 │                  │              └──────────────┘
                                 │                  │
┌──────────────┐  HTTP           │                  │
│  运维脚本     │ ◄────────────► │                  │
│  curl/监控    │  :8081         │                  │
└──────────────┘                 └──────────────────┘
```

#### 8.6 待修改文件清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/daemon/agentflowd_main.cpp` | 新增 | main(), CLI 解析, daemonization, 信号处理 |
| `src/daemon/daemon.h` | 新增 | Daemon 类声明 |
| `src/daemon/daemon.cpp` | 新增 | Daemon 类实现（编排逻辑） |
| `src/daemon/config.h` | 新增 | DaemonConfig 声明 |
| `src/daemon/config.cpp` | 新增 | 配置加载、合并、校验 |
| `src/daemon/rpc_server.h` | 新增 | RpcServer 声明 |
| `src/daemon/rpc_server.cpp` | 新增 | TCP/UDS 监听, 连接管理, 帧解析 |
| `src/daemon/rpc_handler.h` | 新增 | RpcHandler 声明 |
| `src/daemon/rpc_handler.cpp` | 新增 | JSON-RPC 方法路由, 各方法实现 |
| `src/daemon/agent_registry.h` | 新增 | AgentRegistry 声明 |
| `src/daemon/agent_registry.cpp` | 新增 | 多 Agent 加载/卸载/查询 |
| `src/daemon/response_poller.h` | 新增 | ResponsePoller 声明 |
| `src/daemon/response_poller.cpp` | 新增 | 响应轮询 + 客户端路由 |
| `src/daemon/event_broker.h` | 新增 | EventBroker 声明 |
| `src/daemon/event_broker.cpp` | 新增 | IEventHandler → JSON-RPC notification |
| `src/daemon/connection.h` | 新增 | Connection 客户端连接抽象 |
| `src/daemon/connection.cpp` | 新增 | 读写缓冲, notification 队列 |
| `src/daemon/ws_upgrade.h` | 新增 | WebSocket 升级握手（可选） |
| `src/daemon/ws_upgrade.cpp` | 新增 | HTTP 升级 + frame masking |
| `CMakeLists.txt` | 修改 | 新增 agentflowd 可执行目标 |
| `examples/features/service/af-broker/go.mod` | 新增 | Go module 定义 |
| `examples/features/service/af-broker/main.go` | 新增 | af-broker 入口，配置加载，服务启动 |
| `examples/features/service/af-broker/config.go` | 新增 | BrokerConfig 结构体与配置加载 |
| `examples/features/service/af-broker/uds_client.go` | 新增 | UDS JSON-RPC 2.0 客户端，连接管理，换行符帧 |
| `examples/features/service/af-broker/ws_handler.go` | 新增 | WebSocket 连接处理，会话映射，消息转发 |
| `examples/features/service/af-broker/http_handler.go` | 新增 | HTTP API 路由，参数转换，JSON-RPC 调用 |
| `examples/features/service/af-broker/session_map.go` | 新增 | WebSocket 连接 ↔ agentflowd session 映射 |

#### 8.7 功能点实现优先级排序

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | DaemonConfig + CLI 解析 + 配置文件加载 | 无副作用 | 单元测试配置合并逻辑 |
| P1 | Daemon 骨架：main, signal, Runtime 创建, Agent 加载 | 无副作用 | 启动/关机冒烟测试 |
| P2 | RpcServer：TCP 监听, 连接管理, 换行符帧 | 无副作用 | nc 连接测试 |
| P2 | RpcHandler：health, runtime.status, agent.list, agent.info | 无副作用 | JSON-RPC 调用测试 |
| P2 | agent.load, agent.unload (AgentRegistry) | 无副作用 | 动态加载/卸载测试 |
| P3 | agent.execute + ResponsePoller | 有依赖 P2 | 完整请求/响应链路测试 |
| P3 | agent.poll_response, agent.send_control | 有依赖 P3 | 同步轮询 + 控制命令测试 |
| P3 | session.*, workflow.*, worker.*, databus.* | 无副作用 | 各 RPC 方法逐一测试 |
| P4 | EventBroker + event.subscribe | 有依赖 P2 | 事件推送测试 |
| P4 | metrics.get | 无副作用 | Metrics 导出测试 |
| P5 | WebSocket 升级 | 无副作用 | 浏览器客户端测试 |
| P5 | Daemonization + PID 文件 | 无副作用 | 守护进程模式测试 |
| P5 | af-broker (Go) | 有依赖 P3 | WebSocket + HTTP → UDS 转发测试 |

---

### 9. 测试方案

#### Examples 黑盒测试覆盖分析

| 功能点 | 现有 Examples 覆盖 | 更新建议 |
|--------|-------------------|---------|
| Agent 加载/执行 | ✅ agent_runner, interactive_runner | 新增 agentflowd 启动示例 |
| 多 Agent 管理 | ❌ 未覆盖 | 新增多 Agent 配置示例 |
| JSON-RPC 调用 | ❌ 未覆盖 | 新增 nc/curl/Python 客户端示例 |
| 事件订阅 | ❌ 未覆盖 | 新增 event.subscribe 示例 |
| 优雅关机 | ❌ 未覆盖 | 新增 SIGTERM 测试脚本 |

#### 测试用例设计

**单元测试**（GoogleTest）：

| 用例 | 测试目标 |
|------|---------|
| ConfigMergeTest | CLI > ENV > File > Default 优先级 |
| RpcHandlerTest | 各 RPC 方法参数校验 + 返回格式 |
| AgentRegistryTest | load/unload/list 并发安全 |
| ResponsePollerTest | message_id 关联 + 客户端路由 |
| EventBrokerTest | IEventHandler → notification 转换 |

**集成测试**：

| 用例 | 测试目标 |
|------|---------|
| DaemonLifecycleTest | 启动 → 加载 Agent → 执行请求 → 获取响应 → 关机 |
| MultiAgentTest | 加载 2 个 Agent → 分别执行 → 响应正确路由 |
| DynamicLoadTest | 运行中 agent.load → 执行 → agent.unload |
| GracefulShutdownTest | 执行中 SIGTERM → 等待完成 → 退出 |
| EventSubscriptionTest | subscribe → 触发事件 → 收到 notification |
| WebSocketTest | WS 升级 → JSON-RPC 调用 → notification 推送 |
| af-broker Test | WebSocket → UDS-biz → agentflowd execute/response 全链路；HTTP → UDS-ctrl → agent.list/info 全链路 |

**独立性要求**：所有测试通过启动/停止 agentflowd 子进程进行，不修改框架核心代码。

---

### 10. 文档

#### 需要变更的文档

| 文档 | 变更说明 |
|------|---------|
| [INTEGRATION_GUIDE.md](../../publish/INTEGRATION_GUIDE.md) | 新增 "使用 agentflowd 守护进程" 章节，说明 JSON-RPC 集成方式；新增 "使用 af-broker 接入" 章节 |
| [ARCHITECTURE.md](../../docs/ARCHITECTURE.md) | 部署视图新增 agentflowd 进程架构 + af-broker 适配层；逻辑视图新增 RPC Server 组件 |
| [README.md](../../README.md) | 快速开始新增 agentflowd 使用示例 |

#### 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [026-Master-Worker 架构边界问题](026-feature-master-worker-架构边界问题-260320-tbd.md) | 协议设计为 Master-Worker 独立部署预留扩展空间：Worker 注册/心跳/任务派发可建模为 JSON-RPC 方法 |
| [022-Multi-Response 机制设计](022-feature-multi-response-机制设计-260319-implemented.md) | agent.execute 的响应通过 notification 推送，支持 multi-response 流式返回 |
| [030-Node 异步请求机制设计](030-feature-node-异步请求机制设计-260326-implemented.md) | async callback 的结果通过 ResponsePoller 路由到客户端 |
| [019-Plugin UDS 注入机制设计](019-feature-plugin-uds-注入机制设计-260317-implemented.md) | agentflowd 通过 `runtime_config.plugin_uds_path` 传递 Plugin UDS 路径 |

**af-broker 适配层**（`examples/features/service/af-broker/`）：Go 语言实现的服务化适配层，演示外部平台如何接入 agentflowd。对外开放 WebSocket 接口（连接 agentflowd 业务 UDS），本地开放 HTTP 接口（连接 agentflowd 控制 UDS），负责协议转换和会话映射。

---

### 11. 进展跟踪

```markdown
### 进展跟踪

- [x] 代码实现
- [x] 单元测试
- [x] 集成测试
- [x] 文档更新
- [x] 验证通过
```

**详细进度**：

| 功能点 | 功能 | 用例 | 文档 | 状态 |
|--------|------|------|------|------|
| DaemonConfig + CLI | ✅已完成 | ✅5个GTest用例 | ✅已完成 | ✅已验证 |
| Daemon 骨架 | ✅已完成 | ✅已覆盖 | ✅已完成 | ✅已验证 |
| RpcServer | ✅已完成 | ✅4个GTest用例 | ✅已完成 | ✅已验证 |
| RpcHandler (查询类) | ✅已完成 | ✅已覆盖 | ✅已完成 | ✅已验证 |
| AgentRegistry | ✅已完成 | ✅6个GTest用例 | ✅已完成 | ✅已验证 |
| agent.execute + ResponsePoller | ✅已完成 | ✅集成测试通过 | ✅已完成 | ✅已验证 |
| EventBroker | ✅已完成 | ✅5个GTest用例 | ✅已完成 | ✅已验证 |
| WebSocket 升级 | ✅已完成 | ✅WS握手+JSON-RPC集成测试 | ✅已完成 | ✅已验证 |
| af-broker (Go) | ✅已完成 | — | ✅已完成 | ✅已验证 |

---

### 12. 后续计划

| 优化项 | 预期效果 | 方案简述 |
|--------|---------|---------|
| TLS 支持 | 加密通信 | `AGENTFLOW_WITH_OPENSSL` 条件编译，TCP 连接 TLS 握手 |
| 认证/鉴权 | 安全访问控制 | JSON-RPC 扩展字段 `auth_token`，可插拔认证 Handler |
| Worker 远程注册 | Master-Worker 独立部署 | 新增 `worker.register` / `worker.task_result` 方法，远程 Worker 通过 TCP 连接 Master |
| 配置热更新 | 运行时调整参数 | `runtime.reload_config` 触发部分参数重新加载（线程池大小、日志级别等） |
| 健康检查端点 | K8s/容器编排友好 | HTTP GET `/health` 返回 200，与 JSON-RPC `health` 方法并存 |

---

### 13. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-04-23 | xi | 初始版本 |
| v1.1 | 2026-04-23 | xi | 基于 system_agent_mock e2e 测试修正：1) agent.execute 移除 workflow_id，明确 intent_request 语义；2) UDS 改为业务通道（与 TCP 双通道对等）；3) 启动顺序约束 + CWD 切换；4) 响应完成判定（final 标记 + quiet window）；5) 事件订阅映射到 IEventHandler 调度事件回调；6) process_all_sessions_until_idle 分析 |
| v1.2 | 2026-04-23 | xi | 补充三路配置交叉映射表、合并规则（逐字段覆盖）、单源配置项说明、agents[] 追加合并规则、冲突示例 |
| v1.3 | 2026-04-23 | xi | 补充三通道设计：UDS 拆分为业务 UDS + 控制 UDS，RpcServer 支持三通道方法访问控制；新增 af-broker Go 适配层服务设计（WebSocket 业务接口 + HTTP 管理接口，会话映射，UDS 客户端连接管理） |
