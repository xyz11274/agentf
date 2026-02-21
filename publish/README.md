# API 参考手册

AgentFlow v0.1.0 — C++ 智能体工作流编排框架

---

## 目录

- [基础类型](#基础类型)
  - [类型别名](#类型别名)
  - [枚举类型](#枚举类型)
  - [配置结构体](#配置结构体)
- [框架层 API](#框架层-api)
  - [Agent](#agent)
  - [Workflow](#workflow)
  - [Node](#node)
  - [Edge](#edge)
- [运行时 API](#运行时-api)
  - [AgentRuntime](#agentruntime)
  - [HighSpeedDataBus](#highspeeddatabus)
  - [ControlFlowManager](#controlflowmanager)
- [使用示例](#使用示例)

---

## 基础类型

头文件：`agentflow/common.h`  
命名空间：`agentflow`

### 类型别名

| 别名 | 底层类型 | 说明 |
|------|---------|------|
| `AgentId` | `std::string` | Agent 唯一标识 |
| `WorkflowId` | `std::string` | Workflow 唯一标识 |
| `NodeId` | `std::string` | Node 唯一标识 |
| `EdgeId` | `std::string` | Edge 唯一标识 |
| `DataVariant` | `std::variant<int64_t, double, std::string, bool>` | 通用数据值类型 |
| `DataValue` | `DataVariant` | DataVariant 别名 |
| `DataMap` | `std::map<std::string, DataVariant>` | 键值数据映射 |

### 枚举类型

#### `Status` — 通用状态码

| 值 | 整数值 | 说明 |
|----|--------|------|
| `SUCCESS` | 0 | 执行成功 |
| `PENDING` | 1 | 等待执行 |
| `RUNNING` | 2 | 正在执行 |
| `FAILED` | 3 | 执行失败 |
| `TIMEOUT` | 4 | 执行超时 |
| `CANCELLED` | 5 | 已取消 |

#### `NodeState` — 节点状态

| 值 | 整数值 | 说明 |
|----|--------|------|
| `WAITING` | 0 | 等待依赖 |
| `READY` | 1 | 就绪可执行 |
| `EXECUTING` | 2 | 正在执行 |
| `SUCCESS` | 3 | 执行成功 |
| `FAILED` | 4 | 执行失败 |
| `RETRYING` | 5 | 正在重试 |

#### `AgentState` — Agent 状态

| 值 | 整数值 | 说明 |
|----|--------|------|
| `CREATED` | 0 | 已创建 |
| `INITIALIZED` | 1 | 已初始化 |
| `RUNNING` | 2 | 运行中 |
| `PAUSED` | 3 | 已暂停 |
| `STOPPED` | 4 | 已停止 |

#### `WorkflowState` — 工作流状态

| 值 | 整数值 | 说明 |
|----|--------|------|
| `CREATED` | 0 | 已创建 |
| `LOADED` | 1 | 已加载 |
| `RUNNING` | 2 | 运行中 |
| `PAUSED` | 3 | 已暂停 |
| `COMPLETED` | 4 | 已完成 |
| `FAILED` | 5 | 执行失败 |

#### `EventType` — 控制流事件类型

| 值 | 说明 |
|----|------|
| `NODE_START` | 节点开始执行 |
| `NODE_COMPLETE` | 节点完成 |
| `NODE_FAILURE` | 节点失败 |
| `NODE_RETRY` | 节点重试 |
| `DATA_READY` | 数据就绪 |
| `SYNC_WAIT` | 同步等待 |
| `DEPENDENCY_READY` | 依赖就绪 |

#### `ErrorStrategy` — 错误处理策略（common.h）

| 值 | 说明 |
|----|------|
| `FAIL_FAST` | 立即停止 |
| `RETRY` | 重试 |
| `FALLBACK` | 使用备用处理 |
| `COMPENSATION` | 执行补偿逻辑 |
| `IGNORE` | 忽略并继续 |

#### `AgentLifecyclePolicy` — Agent 生命周期策略

| 值 | 说明 |
|----|------|
| `PER_REQUEST` | 每次请求创建新实例 |
| `FACTORY` | 使用工厂管理实例 |
| `SINGLETON` | 全局单例 |

#### `DataTransferMode` — 数据传输模式（common.h）

| 值 | 说明 |
|----|------|
| `SHARED_MEMORY` | 零拷贝共享内存 |
| `MESSAGE_QUEUE` | 消息队列拷贝 |
| `REMOTE_TRANSFER` | 远程传输 |

### 配置结构体

#### `NodeConfig`

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | `std::string` | — | 节点名称 |
| `type` | `std::string` | — | 节点类型 |
| `priority` | `int` | `0` | 执行优先级 |
| `timeout_ms` | `int` | `30000` | 超时（毫秒） |
| `max_retries` | `int` | `0` | 最大重试次数 |
| `error_strategy` | `ErrorStrategy` | `FAIL_FAST` | 错误策略 |
| `metadata` | `map<string,string>` | — | 扩展元数据 |

构造函数：`NodeConfig()`, `NodeConfig(const string& name, const string& type)`

#### `WorkflowConfig`

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | `std::string` | — | 工作流名称 |
| `version` | `std::string` | `"1.0.0"` | 版本号 |
| `description` | `std::string` | — | 描述 |
| `timeout_ms` | `int` | `300000` | 超时（5 分钟） |
| `metadata` | `map<string,string>` | — | 扩展元数据 |

构造函数：`WorkflowConfig()`, `WorkflowConfig(const string& name)`

#### `AgentConfig`

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | `std::string` | — | Agent 名称 |
| `version` | `std::string` | `"1.0.0"` | 版本号 |
| `description` | `std::string` | — | 描述 |
| `lifecycle` | `AgentLifecyclePolicy` | `FACTORY` | 生命周期策略 |
| `tags` | `vector<string>` | — | 标签列表 |
| `metadata` | `map<string,string>` | — | 扩展元数据 |

构造函数：`AgentConfig()`, `AgentConfig(const string& name)`

---

## 框架层 API

### Agent

头文件：`agentflow/framework/agent.h`  
命名空间：`agentflow::framework`

Agent 是 AgentFlow 的基本执行主体，管理多个 Workflow 并提供共享资源。**线程安全**：内部使用 `std::mutex` 保护数据总线和事件队列。

#### `Agent::LifecyclePolicy` 枚举

| 值 | 说明 |
|----|------|
| `PER_REQUEST` | 每次请求创建新实例 |
| `FACTORY` | 从实例池复用 |
| `SINGLETON` | 全局单例 |

#### `Agent::Config` 结构体

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `id` | `AgentId` | — | Agent 唯一标识 |
| `name` | `std::string` | — | Agent 名称 |
| `version` | `std::string` | `"1.0"` | 版本号 |
| `description` | `std::string` | — | 描述 |
| `policy` | `LifecyclePolicy` | `PER_REQUEST` | 生命周期策略 |
| `max_workflows` | `int` | `10` | 最大工作流数量 |
| `max_concurrent_workflows` | `int` | `5` | 最大并发工作流数 |
| `data_bus_capacity` | `int` | `1048576` | 数据总线容量（1MB） |

#### 构造函数

```cpp
explicit Agent(const Config& config);
```

#### Getter 方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `get_id()` | `const AgentId&` | 获取 Agent ID |
| `get_name()` | `const std::string&` | 获取名称 |
| `get_state()` | `AgentState` | 获取当前状态 |
| `get_config()` | `const Config&` | 获取配置 |

#### 工作流管理

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `add_workflow` | `std::shared_ptr<Workflow> workflow` | `void` | 添加工作流 |
| `remove_workflow` | `const WorkflowId& workflow_id` | `void` | 移除工作流 |
| `get_workflow` | `const WorkflowId& workflow_id` | `std::shared_ptr<Workflow>` | 获取工作流（不存在返回 nullptr） |
| `get_workflows` | — | `const vector<shared_ptr<Workflow>>&` | 获取所有工作流 |

#### 资源管理

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `initialize_resources()` | `bool` | 初始化共享资源 |
| `cleanup_resources()` | `void` | 清理共享资源 |

#### 数据总线操作（线程安全）

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `publish_data` | `const string& key, const DataVariant& value` | `bool` | 发布数据到总线 |
| `get_data` | `const string& key, DataVariant& value` | `bool` | 从总线获取数据 |

#### 事件队列（线程安全）

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `post_event` | `const string& event` | `void` | 投递事件 |
| `get_event` | — | `std::string` | 获取队列中下一个事件 |

#### 生命周期管理

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `initialize()` | `bool` | 初始化 Agent（CREATED → INITIALIZED） |
| `start()` | `bool` | 启动 Agent（INITIALIZED → RUNNING） |
| `pause()` | `bool` | 暂停 Agent（RUNNING → PAUSED） |
| `resume()` | `bool` | 恢复 Agent（PAUSED → RUNNING） |
| `stop()` | `bool` | 停止 Agent（→ STOPPED） |

---

### Workflow

头文件：`agentflow/framework/workflow.h`  
命名空间：`agentflow::framework`

Workflow 表示由节点和边组成的 DAG（有向无环图），支持基于 DSL 的编排和动态加载。

#### `Workflow::Config` 结构体

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `id` | `WorkflowId` | — | 工作流唯一标识 |
| `name` | `std::string` | — | 工作流名称 |
| `version` | `std::string` | `"1.0"` | 版本号 |
| `dsl_file` | `std::string` | — | DSL 定义文件路径 |
| `description` | `std::string` | — | 描述 |
| `metadata` | `map<string,string>` | — | 元数据 |

#### 构造函数

```cpp
explicit Workflow(const Config& config);
```

#### Getter 方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `get_id()` | `const WorkflowId&` | 获取 Workflow ID |
| `get_name()` | `const std::string&` | 获取名称 |
| `get_state()` | `WorkflowState` | 获取当前状态 |

#### 节点管理

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `add_node` | `shared_ptr<Node> node` | `void` | 添加节点 |
| `remove_node` | `const NodeId& node_id` | `void` | 移除节点 |
| `get_node` | `const NodeId& node_id` | `shared_ptr<Node>` | 获取节点 |
| `get_nodes` | — | `const vector<shared_ptr<Node>>&` | 获取所有节点 |

#### 边管理

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `add_edge` | `shared_ptr<Edge> edge` | `void` | 添加边 |
| `remove_edge` | `const EdgeId& edge_id` | `void` | 移除边 |
| `get_edges` | — | `const vector<shared_ptr<Edge>>&` | 获取所有边 |

#### DSL 操作

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `load_from_dsl` | `const string& dsl_content` | `bool` | 从 DSL 字符串加载工作流 |
| `validate` | — | `bool` | 验证 DAG 合法性 |
| `update_dynamic` | `const string& dsl_content` | `bool` | 动态更新工作流定义 |

#### DAG 操作

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `get_topological_order()` | `vector<NodeId>` | 获取拓扑排序 |
| `get_executable_nodes()` | `vector<NodeId>` | 获取当前可执行节点 |
| `get_executable_nodes_with_control()` | `vector<NodeId>` | 获取可执行节点（含条件求值） |

#### 控制流集成

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `get_control_flow_manager()` | `runtime::ControlFlowManager&` | 获取控制流管理器 |
| `get_control_context()` | `runtime::ControlContext&` | 获取控制流上下文 |

#### 执行状态管理

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `mark_node_executed` | `const NodeId& node_id, bool success, const string& error_msg = ""` | `void` | 标记节点执行完成 |
| `get_next_node_after_execution` | `const NodeId& current_node, bool success, const string& error_msg = ""` | `NodeId` | 获取下一个执行节点 |
| `reset_execution_state` | — | `void` | 重置所有执行状态 |

#### 循环管理

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `enter_loop` | `const string& loop_id, const runtime::LoopConfig& config` | `void` | 进入循环 |
| `exit_loop_iteration` | `const string& loop_id` | `void` | 退出当前循环迭代 |
| `should_continue_loop` | `const string& loop_id` | `bool` | 是否继续循环 |

#### 错误处理

```cpp
void set_error_handler(const NodeId& node_id, const runtime::ErrorHandlerConfig& handler);
```

为指定节点设置错误处理器。

---

### Node

头文件：`agentflow/framework/node.h`  
命名空间：`agentflow::framework`

Node 表示工作流 DAG 中的一个任务执行单元，支持多种执行模式和热重载。

#### `Node::ExecutionMode` 枚举

| 值 | 说明 |
|----|------|
| `CPP_FUNCTION` | 本地 C++ 函数执行 |
| `SHELL_SCRIPT` | Shell 脚本执行 |
| `EXECUTABLE` | 外部程序执行 |
| `PYTHON_SCRIPT` | Python 脚本执行 |
| `SO_LIBRARY` | 共享库（.so）执行 |

#### `Node::Config` 结构体

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `id` | `NodeId` | — | 节点唯一标识 |
| `name` | `std::string` | — | 节点名称 |
| `type` | `std::string` | — | 节点类型 |
| `priority` | `int` | `0` | 执行优先级 |
| `input_params` | `vector<string>` | — | 输入参数名列表 |
| `output_params` | `vector<string>` | — | 输出参数名列表 |
| `execution_mode` | `ExecutionMode` | `CPP_FUNCTION` | 执行模式 |
| `command` | `std::string` | — | 外部执行命令/脚本路径 |
| `working_dir` | `std::string` | — | 工作目录 |
| `max_retries` | `int` | `3` | 最大重试次数 |
| `timeout_ms` | `int` | `0` | 超时（0 表示无限制） |
| `memory_limit_mb` | `int` | `0` | 内存限制（MB） |
| `cpu_limit` | `int` | `0` | CPU 限制 |
| `retry_delay_ms` | `int` | `1000` | 重试间隔（毫秒） |
| `enable_hot_reload` | `bool` | `true` | 是否启用热重载 |
| `hot_reload_config` | `runtime::HotReloadConfig` | — | 热重载配置 |
| `function_name` | `std::string` | — | SO 库函数名 |
| `main_class` | `std::string` | — | JAR 主类 |
| `args` | `vector<string>` | — | 脚本执行附加参数 |
| `cpp_function` | `function<bool(const DataMap&, DataMap&)>` | — | C++ 回调函数 |

#### 构造函数

```cpp
explicit Node(const Config& config);
```

#### Getter 方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `get_id()` | `const NodeId&` | 获取节点 ID |
| `get_name()` | `const std::string&` | 获取名称 |
| `get_type()` | `const std::string&` | 获取类型 |
| `get_priority()` | `int` | 获取优先级 |
| `get_state()` | `NodeState` | 获取当前状态 |
| `get_inputs()` | `const DataMap&` | 获取输入数据 |
| `get_outputs()` | `const DataMap&` | 获取输出数据 |
| `get_config()` | `const Config&` | 获取完整配置 |
| `get_input_params()` | `const vector<string>&` | 获取输入参数名列表 |
| `get_output_params()` | `const vector<string>&` | 获取输出参数名列表 |
| `get_execution_mode()` | `ExecutionMode` | 获取执行模式 |
| `get_command()` | `const std::string&` | 获取命令 |
| `get_timeout_ms()` | `int` | 获取超时时间 |
| `get_max_retries()` | `int` | 获取最大重试次数 |
| `get_retry_count()` | `int` | 获取当前重试计数 |

#### 状态管理

| 方法 | 参数 | 说明 |
|------|------|------|
| `set_state` | `NodeState state` | 设置节点状态 |
| `set_inputs` | `const DataMap& inputs` | 设置输入数据 |
| `set_outputs` | `const DataMap& outputs` | 设置输出数据 |
| `reset_retry_count` | — | 重置重试计数为 0 |

#### 控制面操作

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `initialize()` | `bool` | 初始化节点 |
| `execute()` | `bool` | 执行节点（根据 execution_mode 分派） |
| `cleanup()` | `bool` | 清理节点资源 |

#### 热重载操作

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `enable_code_reload()` | `bool` | 启用代码热重载 |
| `disable_code_reload()` | `bool` | 禁用代码热重载 |
| `reload_code()` | `bool` | 重新加载代码 |
| `check_and_reload_code()` | `bool` | 检查变更并按需重载 |
| `get_code_version()` | `shared_ptr<runtime::ExecutableVersion>` | 获取当前代码版本 |
| `rollback_to_version(const string& version_id)` | `bool` | 回滚到指定版本 |

#### 代码加载执行

```cpp
runtime::ExecutionResult execute_with_code_loader();
```

通过 CodeLoader 执行节点代码，返回 `ExecutionResult`。

---

### Edge

头文件：`agentflow/framework/edge.h`  
命名空间：`agentflow::framework`

Edge 定义节点间的依赖关系，支持数据传递和控制流事件。

#### `Edge::DataTransferMode` 枚举

| 值 | 说明 |
|----|------|
| `SHARED_MEMORY` | 共享内存（零拷贝） |
| `MESSAGE_QUEUE` | 消息队列 |
| `REMOTE_TRANSFER` | 远程传输 |

#### `Edge::Config` 结构体

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `id` | `std::string` | — | 边唯一标识 |
| `source_node` | `std::string` | — | 源节点 ID |
| `target_node` | `std::string` | — | 目标节点 ID |
| `source_outputs` | `vector<string>` | — | 源节点输出映射 |
| `target_inputs` | `vector<string>` | — | 目标节点输入映射 |
| `transfer_mode` | `DataTransferMode` | `SHARED_MEMORY` | 数据传输模式 |
| `data_formats` | `vector<string>` | — | 数据格式（text, binary 等） |
| `supported_events` | `vector<string>` | — | 支持的事件类型 |
| `condition_expression` | `std::string` | — | 条件表达式（由 Workflow 解析） |
| `priority` | `int` | `0` | 边优先级（多出边时使用） |

#### 构造函数

```cpp
explicit Edge(const Config& config);
```

#### Getter 方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `get_id()` | `const string&` | 获取边 ID |
| `get_source()` | `const string&` | 获取源节点 ID |
| `get_target()` | `const string&` | 获取目标节点 ID |
| `get_transfer_mode()` | `DataTransferMode` | 获取传输模式 |
| `get_source_outputs()` | `const vector<string>&` | 获取源输出列表 |
| `get_target_inputs()` | `const vector<string>&` | 获取目标输入列表 |

#### 条件执行

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `has_condition()` | `bool` | 是否包含条件表达式 |
| `get_condition_expression()` | `const string&` | 获取条件表达式字符串 |

#### 数据传递与事件

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `transfer_data` | `const DataMap& source_data, DataMap& target_data` | `bool` | 将源数据映射到目标 |
| `trigger_event` | `const string& event_type` | `bool` | 触发控制流事件 |

---

## 运行时 API

### AgentRuntime

头文件：`agentflow/runtime/agent_runtime.h`  
命名空间：`agentflow::runtime`

AgentRuntime 是核心调度执行引擎，采用三级 Master-Worker 架构：

- **Agent Level Master**：管理多个 Agent 实例
- **Workflow Level Master**：管理工作流 DAG 调度
- **Node Level Worker**：执行具体节点任务

**线程安全**：内部使用多个 `std::mutex` 保护 Agent 列表、任务队列和工作流状态。

#### 类型别名（命名空间内）

```cpp
using AgentPtr    = std::shared_ptr<framework::Agent>;
using WorkflowPtr = std::shared_ptr<framework::Workflow>;
using NodePtr     = std::shared_ptr<framework::Node>;
```

#### 构造函数与析构函数

```cpp
AgentRuntime();
~AgentRuntime();
```

#### Agent 管理（Agent Level Master）

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `register_agent` | `AgentPtr agent` | `bool` | 注册 Agent |
| `unregister_agent` | `const AgentId& agent_id` | `bool` | 注销 Agent |
| `get_agent` | `const AgentId& agent_id` | `AgentPtr` | 获取 Agent |
| `list_agents` | — | `vector<AgentPtr>` | 列出所有已注册 Agent |

#### 工作流调度（Workflow Level Master）

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `load_workflow` | `const AgentId& agent_id, WorkflowPtr workflow` | `bool` | 加载工作流到指定 Agent |
| `unload_workflow` | `const AgentId& agent_id, const WorkflowId& workflow_id` | `bool` | 卸载工作流 |
| `execute_workflow` | `const AgentId& agent_id, const WorkflowId& workflow_id, const DataMap& inputs` | `bool` | 执行工作流 |

#### 节点执行（Node Level Worker）

```cpp
void process_node(AgentPtr agent, WorkflowPtr workflow, NodePtr node);
```

处理单个节点的执行逻辑。

#### 事件处理

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `on_node_complete` | `const AgentId&, const WorkflowId&, const NodeId&, bool success` | `void` | 节点完成回调 |
| `on_workflow_complete` | `const AgentId&, const WorkflowId&, bool success` | `void` | 工作流完成回调 |

#### 负载均衡

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `update_worker_load` | `int worker_id, int load_change` | `void` | 更新 Worker 负载值 |
| `get_least_loaded_worker` | — | `int` | 获取负载最低的 Worker ID |

#### 运行时统计

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `get_active_agents()` | `int` | 活跃 Agent 数量 |
| `get_active_workflows()` | `int` | 活跃工作流数量 |
| `get_running_nodes()` | `int` | 运行中节点数量 |

#### 生命周期管理

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `start()` | `bool` | 启动运行时（启动 Worker 线程池） |
| `stop()` | `bool` | 停止运行时 |
| `is_running()` | `bool` | 是否正在运行 |

#### 共享资源访问

```cpp
HighSpeedDataBus& get_databus(const AgentId& agent_id);
```

获取指定 Agent 的高速数据总线引用。

---

### HighSpeedDataBus

头文件：`agentflow/runtime/databus.h`  
命名空间：`agentflow::runtime`

高性能数据共享总线，支持零拷贝数据共享和发布/订阅机制。**线程安全**：内部使用 `std::mutex` 保护所有操作。

#### 类型定义

```cpp
using DataChangeCallback = std::function<void(const std::string&, const DataVariant&)>;
```

数据变更回调函数，参数为键名和新值。

#### 构造函数

```cpp
explicit HighSpeedDataBus(size_t capacity = 1024 * 1024);  // 默认 1MB
```

#### 数据操作

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `put` | `const string& key, const DataVariant& value` | `bool` | 写入数据 |
| `get` | `const string& key, DataVariant& value` | `bool` | 读取数据 |
| `remove` | `const string& key` | `bool` | 删除数据 |
| `contains` | `const string& key` | `bool` | 检查键是否存在 |

#### 订阅机制

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `subscribe` | `const string& key, const DataChangeCallback& callback` | `void` | 订阅键的变更通知 |
| `unsubscribe` | `const string& key` | `void` | 取消订阅 |

#### 统计信息

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `get_used_capacity()` | `size_t` | 已使用容量 |
| `get_total_capacity()` | `size_t` | 总容量 |
| `get_data_count()` | `int` | 存储的数据条数 |

---

### ControlFlowManager

头文件：`agentflow/runtime/control_flow.h`  
命名空间：`agentflow::runtime`

管理条件执行、循环和错误处理的控制流引擎。

#### 关键类型

##### `ConditionOperator` 枚举

| 值 | 说明 |
|----|------|
| `EQUAL` / `NOT_EQUAL` | 相等/不等 |
| `GREATER_THAN` / `LESS_THAN` | 大于/小于 |
| `GREATER_EQUAL` / `LESS_EQUAL` | 大于等于/小于等于 |
| `CONTAINS` / `NOT_CONTAINS` | 包含/不包含 |
| `IS_NULL` / `IS_NOT_NULL` | 是否为空 |
| `AND` / `OR` / `NOT` | 逻辑运算 |

##### `ConditionExpression` 结构体

| 字段 | 类型 | 说明 |
|------|------|------|
| `left_operand` | `string` | 左操作数（变量名或值） |
| `op` | `ConditionOperator` | 运算符 |
| `right_operand` | `string` | 右操作数 |
| `is_left_variable` | `bool` | 左操作数是否为变量 |
| `is_right_variable` | `bool` | 右操作数是否为变量 |
| `left_expr` | `shared_ptr<ConditionExpression>` | 复合表达式左子树 |
| `right_expr` | `shared_ptr<ConditionExpression>` | 复合表达式右子树 |

##### `LoopType` 枚举

| 值 | 说明 |
|----|------|
| `FOR_LOOP` | 固定次数循环 |
| `WHILE_LOOP` | 条件循环 |
| `FOR_EACH_LOOP` | 遍历集合 |
| `REPEAT_LOOP` | 重复 N 次 |

##### `LoopConfig` 结构体

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | `LoopType` | 循环类型 |
| `start_value` / `end_value` / `step` | `int` | FOR 循环参数 |
| `loop_variable` | `string` | 循环索引变量名 |
| `condition` | `shared_ptr<ConditionExpression>` | WHILE 循环条件 |
| `collection_variable` | `string` | FOR_EACH 集合变量 |
| `item_variable` | `string` | FOR_EACH 当前元素变量 |
| `max_iterations` | `int` | 最大迭代次数（防死循环） |

##### `ErrorHandlerConfig` 结构体

| 字段 | 类型 | 说明 |
|------|------|------|
| `strategy` | `ErrorStrategy` | 错误策略（RETRY/SKIP/FAIL/CATCH/FALLBACK） |
| `max_retries` | `int` | 重试次数 |
| `retry_delay_ms` | `int` | 重试延迟（毫秒） |
| `fallback_node_id` | `string` | FALLBACK 策略目标节点 |
| `handler_node_id` | `string` | CATCH 策略处理节点 |
| `error_types` | `vector<string>` | 匹配的错误类型 |
| `condition` | `shared_ptr<ConditionExpression>` | 应用条件 |

> 注意：`runtime::ErrorStrategy` 与 `common.h` 中的 `agentflow::ErrorStrategy` 是不同的枚举，runtime 版本包含 `RETRY`, `SKIP`, `FAIL`, `CATCH`, `FALLBACK` 五个值。

#### `ControlContext` 类

执行上下文，存储条件、循环等运行时状态。

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `get_variable` | `const string& name, string& value` | `bool` | 获取变量值 |
| `set_variable` | `const string& name, const string& value` | `void` | 设置变量值 |
| `has_variable` | `const string& name` | `bool` | 检查变量是否存在 |
| `clear_variables` | — | `void` | 清除所有变量 |
| `get_loop_iteration` | `const string& loop_id` | `int` | 获取循环迭代次数 |
| `increment_loop_iteration` | `const string& loop_id` | `void` | 递增循环迭代 |
| `reset_loop_iteration` | `const string& loop_id` | `void` | 重置循环迭代 |
| `should_continue_loop` | `const string& loop_id, const LoopConfig& config` | `bool` | 判断是否继续循环 |
| `enter_loop` | `const string& loop_id, const LoopConfig& config` | `void` | 进入循环上下文 |
| `exit_loop` | `const string& loop_id` | `void` | 退出循环上下文 |
| `is_in_loop` | `const string& loop_id` | `bool` | 是否在循环中 |
| `get_active_loops` | — | `vector<string>` | 获取所有活跃循环 ID |

#### `ConditionEvaluator` 类

条件表达式求值器。

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `evaluate` | `const ConditionExpression& expr, const ControlContext& context, const DataMap& node_outputs` | `bool` | 求值条件表达式 |
| `evaluate_comparison` | `const string& left, ConditionOperator op, const string& right` | `bool` | 求值简单比较 |
| `resolve_value` | `const string& name, const ControlContext& context, const DataMap& node_outputs` | `string` | 从上下文或输出中解析变量值 |

#### `ControlFlowManager` 类

| 方法 | 参数 | 返回类型 | 说明 |
|------|------|---------|------|
| `initialize` | `const ControlContext& context` | `void` | 初始化控制流 |
| `get_context` | — | `ControlContext&` | 获取上下文引用 |
| `should_traverse_edge` | `const string& edge_id, const ConditionExpression& condition` | `bool` | 判断是否应遍历该边 |
| `determine_next_action` | `const NodeId& current_node, NodeState state, const string& error_msg` | `ControlAction` | 确定下一步动作 |
| `get_next_node` | `const NodeId& current_node, const ControlAction& action` | `NodeId` | 获取下一个执行节点 |
| `enter_loop` | `const string& loop_id, const LoopConfig& config` | `void` | 进入循环 |
| `exit_loop_iteration` | `const string& loop_id` | `void` | 退出当前迭代 |
| `should_continue_loop` | `const string& loop_id` | `bool` | 判断是否继续循环 |
| `handle_error` | `const NodeId& failed_node, const string& error_type, const string& error_msg` | `ControlAction` | 处理错误，返回控制动作 |
| `set_error_handler` | `const NodeId& node_id, const ErrorHandlerConfig& handler` | `void` | 设置节点错误处理器 |
| `get_error_handler` | `const NodeId& node_id, ErrorHandlerConfig& handler` | `bool` | 获取节点错误处理器 |

---

## 使用示例

### 示例 1：创建基本 Agent + Workflow + Node 流水线

```cpp
#include "agentflow/framework/agent.h"
#include "agentflow/framework/workflow.h"
#include "agentflow/framework/node.h"
#include "agentflow/framework/edge.h"

using namespace agentflow;
using namespace agentflow::framework;

// 创建 Agent
Agent::Config agent_cfg;
agent_cfg.id   = "agent-1";
agent_cfg.name = "数据处理 Agent";
auto agent = std::make_shared<Agent>(agent_cfg);

// 创建 Workflow
Workflow::Config wf_cfg;
wf_cfg.id   = "wf-1";
wf_cfg.name = "ETL 流水线";
auto workflow = std::make_shared<Workflow>(wf_cfg);

// 创建节点
Node::Config n1_cfg;
n1_cfg.id   = "extract";
n1_cfg.name = "数据提取";
n1_cfg.type = "extractor";
auto node1 = std::make_shared<Node>(n1_cfg);

Node::Config n2_cfg;
n2_cfg.id   = "transform";
n2_cfg.name = "数据转换";
n2_cfg.type = "transformer";
auto node2 = std::make_shared<Node>(n2_cfg);

// 添加节点和边
workflow->add_node(node1);
workflow->add_node(node2);

Edge::Config edge_cfg;
edge_cfg.id          = "e1";
edge_cfg.source_node = "extract";
edge_cfg.target_node = "transform";
workflow->add_edge(std::make_shared<Edge>(edge_cfg));

// 组装并启动
agent->add_workflow(workflow);
agent->initialize();
agent->start();
```

### 示例 2：使用 cpp_function 回调执行节点

```cpp
Node::Config cfg;
cfg.id             = "compute";
cfg.name           = "计算节点";
cfg.type           = "compute";
cfg.execution_mode = Node::ExecutionMode::CPP_FUNCTION;
cfg.input_params   = {"x", "y"};
cfg.output_params  = {"result"};

cfg.cpp_function = [](const DataMap& inputs, DataMap& outputs) -> bool {
    auto x = std::get<int64_t>(inputs.at("x"));
    auto y = std::get<int64_t>(inputs.at("y"));
    outputs["result"] = x + y;
    return true;
};

auto node = std::make_shared<Node>(cfg);
node->set_inputs({{"x", int64_t(10)}, {"y", int64_t(20)}});
node->execute();
// node->get_outputs()["result"] == 30
```

### 示例 3：条件边与控制流

```cpp
// 创建带条件的边
Edge::Config edge_cfg;
edge_cfg.id                   = "conditional-edge";
edge_cfg.source_node          = "validate";
edge_cfg.target_node          = "process";
edge_cfg.condition_expression = "status == success";

auto edge = std::make_shared<Edge>(edge_cfg);
workflow->add_edge(edge);

// 设置控制上下文变量
auto& ctx = workflow->get_control_context();
ctx.set_variable("status", "success");

// 获取含条件求值的可执行节点
auto executable = workflow->get_executable_nodes_with_control();
```

### 示例 4：数据总线发布/订阅

```cpp
using namespace agentflow::runtime;

HighSpeedDataBus bus(1024 * 1024);  // 1MB 容量

// 订阅数据变更
bus.subscribe("sensor.temperature", [](const std::string& key, const DataVariant& value) {
    double temp = std::get<double>(value);
    std::cout << key << " 更新: " << temp << "°C" << std::endl;
});

// 写入数据（触发回调）
bus.put("sensor.temperature", 36.5);

// 读取数据
DataVariant value;
if (bus.get("sensor.temperature", value)) {
    double temp = std::get<double>(value);
}

// 统计
std::cout << "数据条数: " << bus.get_data_count() << std::endl;
std::cout << "已用容量: " << bus.get_used_capacity() << std::endl;
```

---

## Publishing Your Agent

AgentFlow agents can be published and shared with the community. Use the following structure:

### Directory Structure
```
publish/
├── agent/
│   ├── package.json           # Agent metadata
│   ├── agentCard.md           # Agent documentation
│   ├── visualizer.html        # Workflow visualizer
│   └── [source files]
├── workflow/
│   ├── README.md              # Workflow development guide
│   └── [workflow files]
└── agent_build.sh             # Build script
```

### Publishing Workflow
1. Create your agent in `publish/agent/`
2. Update `package.json` with your agent details
3. Run `./publish/agent_build.sh build` to compile
4. Run `./publish/agent_build.sh package` to prepare for distribution
5. Run `./publish/agent_build.sh publish` to upload

### See Also
- Agent Card: `publish/agent/agentCard.md`
- Workflow Guide: `publish/workflow/README.md`
- Examples: `examples/`
