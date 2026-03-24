# AgentFlow 服务集成指南

> 本文档面向服务开发者，说明如何将 AgentFlow 框架集成到一个服务中。
> - 文档版本：v1.4
> - 更新日期：2026-03-24

---

## 1. 概述

### 1.1 集成架构

AgentFlow 采用三层 Master-Worker 架构，服务集成涉及三个核心组件：

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Service Layer                                │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Request Handler  ─────▶  MMAgent  ◀─────  Response Poller   │   │
│  │        │                      │                     │        │   │
│  │        │    route_request     │   dequeue_response  │        │   │
│  │        └──────────────────────┴─────────────────────┘        │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                │                                    │
└────────────────────────────────┼────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      MMAgentRuntime (Master)                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐   │
│  │  Agent       │  │  Scheduler   │  │       Deployer           │   │
│  │  Registry    │  │              │  │  (Workflow Distribution) │   │
│  └──────────────┘  └──────────────┘  └──────────────────────────┘   │
│                                │                                    │
└────────────────────────────────┼────────────────────────────────────┘
                                 │
          ┌──────────────────────┼──────────────────────┐
          ▼                      ▼                      ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│  MMWorkerEngine │  │  MMWorkerEngine │  │  MMWorkerEngine │
│  (Worker 1)     │  │  (Worker 2)     │  │  (Worker N)     │
│                 │  │                 │  │                 │
│  - Node 执行    │  │  - Node 执行    │  │  - Node 执行    │
│  - Workflow 实例│  │  - Workflow 实例│  │  - Workflow 实例│
└─────────────────┘  └─────────────────┘  └─────────────────┘
```

### 1.2 核心组件职责

| 组件 | 职责 | 头文件 |
|------|------|--------|
| **MMAgent** | Agent 实例管理、Session 管理、消息路由 | `include/internal/agent/agent.h` |
| **MMAgentRuntime** | 三级调度引擎、Agent/Worker 注册、任务分发 | `include/internal/runtime/agent_runtime.h` |
| **MMWorkerEngine** | Node 执行、Workflow 实例管理、热点重载 | `include/internal/runtime/worker.h` |
| **AgentPackage** | Agent 包加载、解析、验证 | `include/internal/agent/package.h` |
| **MessageDispatcher** | 消息驱动执行中心枢纽、消息路由与分发 | `include/internal/runtime/message_dispatcher.h` |
| **WorkflowScheduler** | 嵌套 Workflow 执行、深度限制、父子关系追踪 | `include/internal/runtime/workflow_scheduler.h` |

---

## 2. 快速开始

### 2.1 最小可运行示例

```cpp
#include "agent/agent.h"
#include "runtime/agent_runtime.h"

int main() {
    // 1. 从包创建 Agent
    auto agent = agentflow::framework::MMAgent::create_from_package("/path/to/agent_package");
    if (!agent) return 1;

    // 2. 初始化 Agent
    if (!agent->initialize()) return 1;

    // 3. 创建并启动 Runtime
    auto runtime = std::make_unique<agentflow::runtime::MMAgentRuntime>();
    runtime->register_agent(agent);
    runtime->start();

    // 4. 启动 Agent
    agent->start();

    // 5. 提交请求并获取响应
    agentflow::framework::MMAgentMsgQue::Message request;
    request.message_id = "msg_001";
    request.type = agentflow::framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
    request.metadata["message_type"] = "intent_request";
    request.metadata["user_id"] = "user_001";
    request.metadata["client_id"] = "cli";
    request.metadata["workflow_id"] = "main-workflow";
    request.content = "Hello, Agent!";

    agent->route_request_message(request);

    // 6. 轮询响应
    agentflow::framework::MMAgentMsgQue::Message response;
    while (agent->dequeue_response_message(response)) {
        std::cout << "Response: " << response.content << std::endl;
    }

    // 7. 清理
    agent->stop();
    runtime->stop();
    return 0;
}
```

### 2.2 从 Agent 包创建服务

Agent 包是预打包的 Agent 定义，包含：
- `package.json` - 包描述
- `agent.json` - Agent 配置
- `workflow_*.json` - Workflow 定义
- `node_*.sh/py/so` - 可执行节点

```cpp
// 创建 Agent 并初始化
auto agent = framework::MMAgent::create_from_package(package_path);
agent->initialize();
agent->start();

// 查看 Agent 信息
std::cout << "Agent: " << agent->get_name() << std::endl;
std::cout << "ID: " << agent->get_id() << std::endl;
std::cout << "Workflows: " << agent->get_workflows().size() << std::endl;
```

---

## 3. Agent 集成

### 3.1 配置结构

```cpp
// include/internal/agent/agent.h:48-57
struct Config {
    AgentId     id;                              // Agent 唯一标识
    std::string name;                            // Agent 名称
    std::string version = "1.0";                 // 版本号
    std::string description;                     // 描述
    LifecyclePolicy policy = LifecyclePolicy::PER_REQUEST;  // 生命周期策略
    int max_workflows = 10;                      // 最大 Workflow 数量
    int max_concurrent_workflows = 5;            // 最大并发 Workflow 数
    int data_bus_capacity = 1024 * 1024;         // DataBus 容量
};
```

**生命周期策略**：

| 策略 | 说明 |
|------|------|
| `PER_REQUEST` | 每个请求创建新实例 |
| `FACTORY` | 工厂模式创建实例 |
| `SINGLETON` | 单例模式 |

### 3.2 生命周期管理

```cpp
// 状态定义 (include/internal/agent/common.h)
enum class AgentState {
    CREATED      = 0,  // 已创建，组件未初始化
    INITIALIZED  = 1,  // 已初始化，组件就绪
    RUNNING      = 2,  // 运行中，可处理消息
    PAUSED       = 3,  // 已暂停，停止处理新消息
    STOPPED      = 4,  // 已停止，资源已释放
};
```

**生命周期方法**：

```cpp
// 初始化：CREATED → INITIALIZED
bool initialize();

// 启动：INITIALIZED → RUNNING
bool start();

// 暂停：RUNNING → PAUSED
bool pause();

// 恢复：PAUSED → RUNNING
bool resume();

// 停止：RUNNING/PAUSED → STOPPED
bool stop();
```

### 3.3 消息处理

#### 3.3.1 路由请求消息

```cpp
// include/internal/agent/agent.h:76
bool route_request_message(const MMAgentMsgQue::Message& message);
```

**消息结构**：

```cpp
struct Message {
    std::string message_id;      // 消息唯一 ID
    MessageType type;            // 消息类型
    std::string sender_id;       // 发送者 ID
    std::string receiver_id;     // 接收者 ID
    std::string content;         // 消息内容
    int64_t timestamp;           // 时间戳
    std::map<std::string, std::string> metadata;  // 元数据
};
```

**必需的元数据字段**：

| 字段 | 说明 | 示例 |
|------|------|------|
| `message_type` | 消息子类型 | `intent_request`, `data_exchange` |
| `user_id` | 用户标识 | `"user_001"` |
| `client_id` | 客户端标识 | `"web_client"` |
| `workflow_id` | 目标 Workflow ID | `"main-workflow"` |

**示例**：

```cpp
framework::MMAgentMsgQue::Message message;
message.message_id = generate_message_id();
message.type = framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
message.metadata["message_type"] = "intent_request";
message.sender_id = "service";
message.receiver_id = agent->get_id();
message.content = user_input;
message.timestamp = current_timestamp_ms();
message.metadata["user_id"] = user_id;
message.metadata["client_id"] = client_id;
message.metadata["workflow_id"] = workflow_id;

agent->route_request_message(message);
```

#### 3.3.2 获取响应消息

```cpp
// include/internal/agent/agent.h:77
bool dequeue_response_message(MMAgentMsgQue::Message& message);

// 查看队列大小
int get_response_queue_size() const;
```

**轮询示例**：

```cpp
std::string poll_results(int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    const int poll_interval_ms = 50;
    std::ostringstream result;

    while (true) {
        framework::MMAgentMsgQue::Message response;
        while (agent->dequeue_response_message(response)) {
            if (!response.content.empty()) {
                result << response.content;
            }
        }

        if (!result.str().empty()) {
            return result.str();
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            return "";  // Timeout
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }
}
```

#### 3.3.3 发送控制命令

控制命令（CONTROL）用于会话生命周期管理，包括中断运行中的任务和销毁会话。

**控制命令类型**：

```cpp
// 控制命令常量
struct ControlCommand {
    static constexpr const char* SESSION_DESTROY   = "SESSION-DESTROY";   // 销毁会话
    static constexpr const char* SESSION_INTERRUPT = "SESSION-INTERRUPT"; // 中断任务
};
```

**SESSION-INTERRUPT 示例**：

```cpp
// 中断会话中正在运行的技能任务
framework::MMAgentMsgQue::Message interrupt_msg;
interrupt_msg.message_id = generate_message_id();
interrupt_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
interrupt_msg.content = framework::MMAgentMsgQue::ControlCommand::SESSION_INTERRUPT;
interrupt_msg.metadata["session_id"] = session_id;
interrupt_msg.metadata["user_id"] = user_id;

agent->route_request_message(interrupt_msg);
```

**SESSION-DESTROY 示例**：

```cpp
// 销毁会话并清理所有资源
framework::MMAgentMsgQue::Message destroy_msg;
destroy_msg.message_id = generate_message_id();
destroy_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
destroy_msg.content = framework::MMAgentMsgQue::ControlCommand::SESSION_DESTROY;
destroy_msg.metadata["session_id"] = session_id;

agent->route_request_message(destroy_msg);
```

**行为说明**：

| 命令 | 行为 |
|------|------|
| `SESSION-INTERRUPT` | 停止所有运行中的技能任务，向协调器发送中断状态，任务状态设为 `INTERRUPTED` |
| `SESSION-DESTROY` | 停止所有任务（包括调度器和协调器），清理 DataBus 中 `/session/{session-id}/*` 数据 |

### 3.4 Session 管理

```cpp
// 获取或创建 Session（按 user_id + client_id）
std::shared_ptr<MMAgentSession> get_or_create_session(
    const std::string& user_id,
    const std::string& client_id);

// 按 session_key 获取 Session
std::shared_ptr<MMAgentSession> get_session_by_key(const std::string& session_key) const;

// 获取所有 Session ID
std::vector<std::string> get_all_session_ids() const;
```

---

## 4. Runtime 集成

### 4.1 配置结构

```cpp
// include/internal/runtime/agent_runtime.h:43-51
struct Config {
    RuntimeRole role = RuntimeRole::MASTER;  // 角色：MASTER 或 WORKER
    int worker_threads = 4;                   // Worker 线程数
    std::string local_worker_id = "runtime-local-worker";  // 本地 Worker ID
    int worker_default_workflow_threads = 1;  // Workflow 默认线程数
    int worker_control_threads = 1;           // 控制线程数
    int worker_business_threads = 1;          // 业务线程数
    size_t worker_queue_size = 1024;          // Worker 队列大小
    std::string plugin_uds_path;              // Plugin UDS 路径（注入到节点进程）
};
```

**RuntimeRole**：

| 角色 | 说明 |
|------|------|
| `MASTER` | 主节点，包含 Worker 能力，负责调度 |
| `WORKER` | 工作节点，仅执行任务 |

### 4.2 Agent 注册

```cpp
// include/internal/runtime/agent_runtime.h:58-61
bool register_agent(AgentPtr agent);
bool unregister_agent(const AgentId& agent_id);
AgentPtr get_agent(const AgentId& agent_id);
std::vector<AgentPtr> list_agents();
```

**示例**：

```cpp
// 创建 Runtime
runtime::MMAgentRuntime runtime;

// 注册 Agent
if (!runtime.register_agent(agent)) {
    std::cerr << "Failed to register agent" << std::endl;
    return 1;
}

// 启动 Runtime
runtime.start();
```

### 4.3 Worker 注册

```cpp
// include/internal/runtime/agent_runtime.h:95-99
bool register_worker_engine(const std::shared_ptr<MMWorkerEngine>& worker);
bool unregister_worker_engine(const std::string& worker_id);
bool heartbeat_worker(const std::string& worker_id);
bool update_worker_engine_status(const std::string& worker_id, MMWorkerEngine::WorkerStatus status);
std::vector<master::WorkerSnapshot> list_worker_engines() const;
```

**示例**：

```cpp
// 创建 Worker
runtime::MMWorkerEngine::Config worker_config;
worker_config.worker_id = "worker-001";
worker_config.default_workflow_threads = 2;
worker_config.control_threads = 1;
worker_config.business_threads = 2;

auto worker = std::make_shared<runtime::MMWorkerEngine>(worker_config);
worker->start();

// 注册 Worker 到 Runtime
runtime.register_worker_engine(worker);
```

### 4.4 Workflow 部署

```cpp
// include/internal/runtime/agent_runtime.h:101-107
bool deploy_workflow_instance(const WorkflowInstanceInfo& info,
                              const std::string& preferred_worker_id = "");
bool destroy_workflow_instance(const std::string& workflow_instance_id);
master::TaskScheduleResult schedule_task(const master::TaskScheduleRequest& request);
std::vector<WorkflowInstanceInfo> list_workflow_instances() const;
std::map<std::string, master::TaskScheduleResult> list_task_assignments() const;
```

**WorkflowInstanceInfo 结构**：

```cpp
// include/internal/runtime/shared_runtime_state.h
struct WorkflowInstanceInfo {
    std::string workflow_instance_id;
    std::string workflow_id;
    std::string workflow_file;
    std::string agent_object_id;
    std::string agent_instance_id;
    std::string worker_id;
    int         ref_count = 0;
};
```

**查询方法说明**：

| 方法 | 说明 |
|------|------|
| `list_workflow_instances()` | 获取所有已部署的 Workflow 实例列表 |
| `list_task_assignments()` | 获取任务分配映射（task_id → TaskScheduleResult） |

**示例**：

```cpp
// 查询已部署的 Workflow 实例
auto instances = runtime.list_workflow_instances();
for (const auto& info : instances) {
    std::cout << "Workflow: " << info.workflow_id
              << " Instance: " << info.workflow_instance_id
              << " Agent: " << info.agent_instance_id
              << " Worker: " << info.worker_id << std::endl;
}

// 查询任务分配情况
auto assignments = runtime.list_task_assignments();
for (const auto& [task_id, result] : assignments) {
    std::cout << "Task: " << task_id
              << " Worker: " << result.worker_id << std::endl;
}
```

---

### 4.5 消息驱动架构

AgentFlow 支持消息驱动的执行模式，通过 `MessageDispatcher` 和 `WorkflowScheduler` 实现组件间的松耦合通信。

#### 4.5.1 MessageDispatcher

`MessageDispatcher` 是消息驱动执行的中心枢纽，负责消息的路由与分发。

**配置结构**：

```cpp
// include/internal/runtime/message_dispatcher.h:31-36
struct MessageDispatcherConfig {
  int worker_threads = 4;              // 工作线程数
  size_t max_queue_size = 1024;        // 最大队列大小
  int64_t default_message_ttl_ms = 60000;  // 默认消息 TTL（毫秒）
  bool enable_priority_queue = true;   // 启用优先级队列
};
```

**核心接口**：

```cpp
// Subscribe/Publish 模式
SubscriptionId subscribe(const std::string& message_type, MessageHandler handler);
SubscriptionId subscribe(const std::string& message_type, MessageHandler handler,
                         MessageFilter filter);
bool unsubscribe(SubscriptionId subscription_id);
bool publish(std::shared_ptr<MessageBase> message);
bool publish(std::shared_ptr<MessageBase> message, MessagePriority priority);

// Request-Response 模式
std::shared_ptr<MessageBase> request(std::shared_ptr<MessageBase> message,
                                     int64_t timeout_ms = 5000);
void respond(std::shared_ptr<MessageBase> request, std::shared_ptr<MessageBase> response);

// DataBus 集成
DataRef store_data(const std::string& key, const DataVariant& value,
                   const DataBus::OwnerContext& owner);
bool resolve_data(const DataRef& ref, DataVariant& value);
```

**消息优先级**：

```cpp
enum class MessagePriority {
  LOW    = 0,
  NORMAL = 1,
  HIGH   = 2,
  URGENT = 3,
};
```

**使用示例**：

```cpp
// 创建 MessageDispatcher
runtime::MessageDispatcherConfig config;
config.worker_threads = 4;
auto dispatcher = std::make_shared<runtime::MessageDispatcher>(config);
dispatcher->start();

// 订阅 Node 完成消息
auto sub_id = dispatcher->subscribe("node.completion",
    [](std::shared_ptr<runtime::MessageBase> msg) {
        auto completion = std::dynamic_pointer_cast<runtime::NodeCompletionMessage>(msg);
        if (completion) {
            std::cout << "Node " << completion->node_id << " completed with status: "
                      << static_cast<int>(completion->status) << std::endl;
        }
    });

// 发布 Node 执行消息
auto exec_msg = std::make_shared<runtime::NodeExecutionMessage>(
    "node_001", "workflow_instance_001");
exec_msg->inputs["query"] = "Hello";
dispatcher->publish(exec_msg);

// 取消订阅
dispatcher->unsubscribe(sub_id);
```

#### 4.5.2 WorkflowScheduler

`WorkflowScheduler` 处理嵌套 Workflow 的执行，支持父子关系追踪和深度限制。

**配置结构**：

```cpp
// include/internal/runtime/workflow_scheduler.h:36-40
struct Config {
  int max_depth = 10;                  // 最大嵌套深度
  int64_t default_timeout_ms = 300000; // 默认超时（5 分钟）
  bool enable_parallel_nested = true;  // 允许并行嵌套执行
};
```

**核心接口**：

```cpp
// 设置依赖
void set_dispatcher(std::shared_ptr<MessageDispatcher> dispatcher);
void set_databus(std::shared_ptr<DataBus> databus);
void set_workflow_factory(WorkflowFactory factory);

// 处理消息
void handle_schedule_request(const WorkflowScheduleMessage& msg);
void handle_completion(const WorkflowCompletionMessage& msg);

// 启动嵌套 Workflow
std::string start_nested_workflow(const std::string& workflow_id,
                                  const std::string& parent_instance_id,
                                  const std::string& parent_node_id,
                                  const DataMap& inputs);

// 深度控制
int get_nesting_depth(const std::string& instance_id) const;
bool is_depth_limit_reached(const std::string& instance_id) const;
```

**使用示例**：

```cpp
// 创建 WorkflowScheduler
runtime::WorkflowScheduler::Config config;
config.max_depth = 5;
auto scheduler = std::make_shared<runtime::WorkflowScheduler>(config);

// 设置依赖
scheduler->set_dispatcher(dispatcher);
scheduler->set_databus(databus);
scheduler->set_workflow_factory([](const std::string& workflow_id) {
    return load_workflow_by_id(workflow_id);
});

// 启动嵌套 Workflow
std::string child_instance_id = scheduler->start_nested_workflow(
    "child_workflow",
    "parent_instance_001",
    "workflow_node_001",
    {{"input", "data"}}
);

// 检查嵌套深度
int depth = scheduler->get_nesting_depth(child_instance_id);
if (scheduler->is_depth_limit_reached(child_instance_id)) {
    std::cerr << "Maximum nesting depth reached!" << std::endl;
}
```

#### 4.5.3 消息类型

所有消息类型定义在 `include/internal/runtime/message_types.h`：

| 消息类型 | 结构体 | 说明 |
|----------|--------|------|
| `node.execution` | `NodeExecutionMessage` | 触发 Node 执行 |
| `node.completion` | `NodeCompletionMessage` | Node 执行完成通知 |
| `workflow.schedule` | `WorkflowScheduleMessage` | 调度嵌套 Workflow |
| `workflow.completion` | `WorkflowCompletionMessage` | Workflow 完成通知 |
| `control.flow` | `ControlFlowMessage` | 控制流操作 |

**NodeExecutionMessage**：

```cpp
struct NodeExecutionMessage : public MessageBase {
  std::string node_id;               // 目标 Node ID
  std::string workflow_instance_id;  // Workflow 实例 ID
  DataMap inputs;                    // 输入数据
  std::vector<DataRef> input_refs;   // DataBus 引用
  std::map<std::string, std::string> metadata;
};
```

**NodeCompletionMessage**：

```cpp
struct NodeCompletionMessage : public MessageBase {
  std::string node_id;
  std::string workflow_instance_id;
  Status status;                     // 执行状态
  DataMap outputs;                   // 输出数据
  std::vector<DataRef> output_refs;  // DataBus 引用
  int64_t execution_time_ms;         // 执行耗时
  std::string error_message;         // 错误信息
};
```

**ControlFlowMessage**：

```cpp
// 控制流动作类型
enum class ControlFlowAction {
  PROCEED,    // 继续到下一个节点
  SKIP,       // 跳过当前分支
  GOTO,       // 跳转到指定节点
  LOOP_BACK,  // 返回循环起点
  BREAK,      // 跳出循环
  CONTINUE,   // 继续下一次迭代
  FORK,       // 分支并行执行
  JOIN,       // 合并并行分支
  ABORT,      // 终止 Workflow
};

struct ControlFlowMessage : public MessageBase {
  std::string workflow_instance_id;
  std::string node_id;
  std::string edge_id;
  ControlFlowAction action;
  std::string target_node_id;    // GOTO 目标
  std::string loop_id;           // 循环标识
  int iteration;                 // 当前迭代次数
  // ... 其他字段
};
```

#### 4.5.4 消息驱动执行流程

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  WorkflowActor  │     │MessageDispatcher│     │   NodeWorker    │
└────────┬────────┘     └────────┬────────┘     └────────┬────────┘
         │                       │                       │
         │  NodeExecutionMessage │                       │
         │──────────────────────►│                       │
         │                       │                       │
         │                       │  dispatch to handler  │
         │                       │──────────────────────►│
         │                       │                       │
         │                       │                       │  execute
         │                       │                       │  node
         │                       │                       │
         │  NodeCompletionMessage│                       │
         │◄──────────────────────│◄──────────────────────│
         │                       │                       │
         │  process completion   │                       │
         │                       │                       │
```

---

### 4.6 事件处理器集成

AgentFlow 提供事件处理器机制，允许服务开发者监听 Agent、Session、Dialogue、Task 的生命周期事件。

#### 4.6.1 事件类型

| 事件类型 | 触发时机 | 用途 |
|----------|----------|------|
| `AGENT_REGISTERED` | Agent 注册成功 | 初始化 Agent 相关资源 |
| `AGENT_UNREGISTERED` | Agent 注销成功 | 清理 Agent 相关资源 |
| `SESSION_CREATED` | Session 绑定路由 | 记录会话创建日志 |
| `SESSION_DESTROYED` | Session 返回池 | 清理会话资源 |
| `DIALOGUE_CREATED` | Dialogue 创建 | 追踪对话开始 |
| `DIALOGUE_DESTROYED` | Dialogue 清理 | 追踪对话结束 |
| `TASK_CREATED` | Task 创建 | 监控任务启动 |
| `TASK_DESTROYED` | Task 完成 | 监控任务结束 |

#### 4.6.2 实现 IEventHandler 接口

```cpp
#include "runtime/event_handler.h"

class MyEventHandler : public agentflow::runtime::IEventHandler {
public:
  void handle_agent_event(const AgentEvent& event) override {
    if (event.event_type == EventType::AGENT_REGISTERED) {
      // Agent 注册时的处理逻辑
      std::cout << "Agent registered: " << event.name
                << " (id=" << event.agent_id << ")" << std::endl;

      // event.workflows 包含完整的 workflow 拓扑信息
      for (const auto& wf : event.workflows) {
        std::cout << "  Workflow: " << wf.name << std::endl;
      }
    }
  }

  void handle_session_event(const SessionEvent& event) override {
    if (event.event_type == EventType::SESSION_CREATED) {
      std::cout << "Session created: " << event.session_id
                << " user=" << event.user_id << std::endl;
    }
  }

  void handle_dialogue_event(const DialogueEvent& event) override {
    // 追踪对话生命周期
  }

  void handle_task_event(const TaskEvent& event) override {
    // 追踪任务执行
    std::cout << "Task: " << event.task_id
              << " type=" << event.task_type
              << " skill=" << event.skill_name << std::endl;
  }
};
```

#### 4.6.3 注册事件处理器

```cpp
// 1. 创建事件处理器
auto handler = std::make_shared<MyEventHandler>();

// 2. 配置回调线程池大小（可选，默认 5 个线程）
runtime->set_callback_thread_pool_size(10);

// 3. 注册处理器（必须在 runtime->start() 之前注册）
runtime->register_event_handler(handler);

// 4. 启动 runtime
runtime->start();

// 5. 注销处理器（可选）
runtime->unregister_event_handler(handler);

// 6. 清除所有处理器
runtime->clear_event_handlers();
```

#### 4.6.4 完整集成示例

```cpp
class MonitoringService {
public:
  bool initialize(const std::string& package_path) {
    // 创建 runtime
    runtime_ = std::make_unique<runtime::MMAgentRuntime>();

    // 注册事件处理器
    event_handler_ = std::make_shared<MonitoringEventHandler>();
    runtime_->register_event_handler(event_handler_);
    runtime_->set_callback_thread_pool_size(4);

    // 启动 runtime
    if (!runtime_->start()) return false;

    // 创建并注册 agent
    agent_ = framework::MMAgent::create_from_package(package_path);
    if (!agent_ || !agent_->initialize()) return false;

    runtime_->register_agent(agent_);
    agent_->start();

    return true;
  }

  void shutdown() {
    if (agent_) agent_->stop();
    if (runtime_) {
      runtime_->clear_event_handlers();  // 清理事件处理器
      runtime_->stop();
    }
  }

private:
  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
  std::shared_ptr<framework::MMAgent> agent_;
  std::shared_ptr<MonitoringEventHandler> event_handler_;
};
```

#### 4.6.5 注意事项

1. **异步执行**：事件回调在独立线程池中异步执行，不会阻塞主流程
2. **线程安全**：回调中访问共享资源需使用互斥锁保护
3. **避免阻塞**：回调应避免长时间阻塞操作，以免影响事件分发
4. **注册时机**：建议在 `runtime->start()` 之前注册事件处理器
5. **生命周期**：事件处理器生命周期应长于 runtime

---

## 5. Worker 集成

### 5.1 配置结构

```cpp
// include/internal/runtime/worker.h:77-84
struct Config {
    std::string worker_id;                    // Worker 唯一标识
    bool preconfigured = false;               // 是否预配置
    int default_workflow_threads = 1;         // 默认 Workflow 线程数
    int control_threads = 1;                  // 控制线程数
    int business_threads = 1;                 // 业务线程数
    size_t max_queue_size = 1024;             // 最大队列大小
    std::string plugin_uds_path;              // Plugin UDS 路径（注入到节点进程）
};
```

### 5.2 启动与心跳

```cpp
// 启动 Worker
bool start();

// 停止 Worker
void stop();

// 心跳（更新活跃时间）
void heartbeat();

// 获取状态
WorkerStatus get_status() const;
int get_load() const;
int64_t get_last_heartbeat_ms() const;
```

**WorkerStatus**：

```cpp
enum class WorkerStatus {
    STARTING = 0,  // 启动中
    RUNNING  = 1,  // 运行中
    DEGRADED = 2,  // 降级
    STOPPED  = 3   // 已停止
};
```

### 5.3 Workflow 实例注册

```cpp
// include/internal/runtime/worker.h:143-144
bool register_workflow_instance(const WorkflowRegistration& registration);
bool unregister_workflow_instance(const std::string& workflow_instance_id);
```

**WorkflowRegistration 结构**：

```cpp
struct WorkflowRegistration {
    std::string workflow_id;
    std::string workflow_instance_id;
    int thread_count = 0;
};
```

### 5.4 执行请求

```cpp
// include/internal/runtime/worker.h:148
static ExecutionResponse execute_code(const ExecutionRequest& request);
```

**ExecutionRequest 结构**：

```cpp
struct ExecutionRequest {
    ExecutionMode mode = ExecutionMode::CPP_FUNCTION;  // 执行模式
    std::string command;              // 命令/脚本路径
    std::string function_name;        // 函数名（CPP_FUNCTION 模式）
    std::string main_class;           // 主类（JAVA_JAR 模式）
    std::string working_dir;          // 工作目录
    std::vector<std::string> args;    // 参数
    int timeout_ms = 30000;           // 超时时间
    DataMap inputs;                   // 输入数据
    std::string databus_root_dir;     // DataBus 根目录
    bool keep_databus_files = false;  // 保留 DataBus 文件
    bool enable_hot_reload = false;   // 启用热点重载
    HotReloadConfig hot_reload_config; // 热点重载配置
    std::string plugin_uds_path;      // Plugin UDS 路径（注入为 AF_PLUGIN_UDS_FILE）
    struct LLMServiceConfig {
        std::string base_url;
        std::string api_key;
        std::string model = "gpt-4.1";
        int timeout_ms = 30000;
    } llm_config;
};
```

**ExecutionMode**：

| 模式 | 说明 |
|------|------|
| `CPP_FUNCTION` | C++ 函数调用 |
| `SHELL_SCRIPT` | Shell 脚本 |
| `EXECUTABLE` | 可执行文件 |
| `GO_SCRIPT` | Go 脚本 |
| `PYTHON_SCRIPT` | Python 脚本 |
| `JAVA_JAR` | Java JAR |
| `SO_LIBRARY` | 动态库 |

### 5.5 Plugin UDS 注入机制

框架支持将插件 Unix Domain Socket (UDS) 路径注入到节点进程中，使节点能够通过 UDS 与外部插件服务通信。

#### 5.5.1 配置方式

**方式一：Runtime 配置（推荐）**

在初始化 Runtime 时配置 `plugin_uds_path`：

```cpp
runtime::MMAgentRuntime::Config config;
config.worker_threads = 4;
config.plugin_uds_path = "/var/run/my_plugin.sock";

auto runtime = std::make_shared<runtime::MMAgentRuntime>(config);
```

**方式二：环境变量覆盖**

框架启动时检查 `AF_PLUGIN_UDS_FILE` 环境变量：

```bash
export AF_PLUGIN_UDS_FILE=/var/run/my_plugin.sock
./my_agent_service
```

#### 5.5.2 配置传递链路

```
MMAgentRuntime::Config.plugin_uds_path
         ↓
MMWorkerEngine::Config.plugin_uds_path
         ↓
ExecutionRequest.plugin_uds_path
         ↓
AF_PLUGIN_UDS_FILE 环境变量 (注入到节点进程)
```

#### 5.5.3 节点进程访问

节点进程通过环境变量 `AF_PLUGIN_UDS_FILE` 获取插件 UDS 路径：

**Shell Script**：

```bash
#!/bin/bash
PLUGIN_UDS="${AF_PLUGIN_UDS_FILE:-}"
if [ -n "$PLUGIN_UDS" ]; then
  echo "Plugin UDS path: $PLUGIN_UDS"
  # 通过 socat 或 nc 与插件服务通信
fi
```

**Python Script**：

```python
import os

plugin_uds = os.environ.get('AF_PLUGIN_UDS_FILE')
if plugin_uds:
    import socket
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(plugin_uds)
    # 与插件服务通信...
```

**Go Script**：

```go
package main

import (
    "os"
    "net"
)

func main() {
    pluginUDS := os.Getenv("AF_PLUGIN_UDS_FILE")
    if pluginUDS != "" {
        conn, err := net.Dial("unix", pluginUDS)
        if err == nil {
            defer conn.Close()
            // 与插件服务通信...
        }
    }
}
```

#### 5.5.4 SO Library 节点

SO Library 节点可通过 `getenv` 获取插件 UDS 路径：

```cpp
#include <cstdlib>

extern "C" void my_node_v3(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBusClient* client) {

  // 通过环境变量获取插件 UDS 路径
  const char* plugin_uds = std::getenv("AF_PLUGIN_UDS_FILE");
  if(plugin_uds) {
    // 连接插件服务...
  }
}
```

#### 5.5.5 向后兼容性

- `plugin_uds_path` 为可选配置，未配置时不注入 `AF_PLUGIN_UDS_FILE` 环境变量
- 现有节点代码无需修改，`AF_PLUGIN_UDS_FILE` 环境变量不存在时节点正常运行
- 完全向后兼容现有 Runtime、Worker 和节点实现

---

## 6. 远程 Worker 注册

### 6.1 同进程注册

最简单的模式是 Agent、Runtime、Worker 在同一进程内：

```cpp
// 1. 创建 Runtime（MASTER 角色）
runtime::MMAgentRuntime runtime;

// 2. 创建本地 Worker
runtime::MMWorkerEngine::Config worker_config;
worker_config.worker_id = "local-worker";
auto local_worker = std::make_shared<runtime::MMWorkerEngine>(worker_config);
local_worker->start();

// 3. 注册 Worker
runtime.register_worker_engine(local_worker);

// 4. 创建并注册 Agent
auto agent = framework::MMAgent::create_from_package(package_path);
agent->initialize();
runtime.register_agent(agent);

// 5. 启动
runtime.start();
agent->start();
```

### 6.2 远程注册模式

分布式部署时，远程 Worker 需要通过网络注册到 Master Runtime：

```
┌─────────────────────┐              ┌─────────────────────┐
│   Master Runtime    │              │   Remote Worker     │
│                     │              │                     │
│  ┌───────────────┐  │   Register   │  ┌───────────────┐  │
│  │   Deployer    │◄─┼──────────────┼──│ MMWorkerEngine│  │
│  │               │  │              │  │               │  │
│  └───────────────┘  │   Heartbeat  │  └───────────────┘  │
│         │           │◄─────────────┼─        │          │
│         ▼           │              │          ▼          │
│  ┌───────────────┐  │   Dispatch   │  ┌───────────────┐  │
│  │   Scheduler   │──┼──────────────┼─►│  Task Queue   │  │
│  │               │  │              │  │               │  │
│  └───────────────┘  │              │  └───────────────┘  │
└─────────────────────┘              └─────────────────────┘
```

**远程 Worker 注册步骤**：

1. Worker 启动后，通过网络向 Master 发送注册请求
2. Master 的 Deployer 验证并注册 Worker
3. Worker 定期发送心跳保持连接
4. Scheduler 分发任务到已注册的 Worker

**代码示例（简化）**：

```cpp
// 远程 Worker 侧
class RemoteWorkerClient {
public:
    bool register_to_master(const std::string& master_address) {
        // 发送注册请求到 Master
        // 包含：worker_id, supported_workflows, capabilities
        return true;
    }

    void start_heartbeat(int interval_ms) {
        heartbeat_thread_ = std::thread([this, interval_ms]() {
            while (running_) {
                send_heartbeat();
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
        });
    }

private:
    void send_heartbeat() {
        // 发送心跳到 Master
        // 更新 last_heartbeat_ms
    }
};

// Master 侧（在 MMAgentRuntime 中已实现）
bool MMAgentRuntime::heartbeat_worker(const std::string& worker_id) {
    // 更新 Worker 心跳时间
    // 检查是否超时
}
```

### 6.3 心跳与故障检测

```cpp
// Master 端心跳检测
void MMAgentRuntime::check_worker_health() {
    auto now = current_time_ms();
    for (auto& snapshot : list_worker_engines()) {
        auto elapsed = now - snapshot.last_heartbeat_ms;
        if (elapsed > heartbeat_timeout_ms_) {
            // 标记 Worker 为 DEGRADED 或移除
            update_worker_engine_status(snapshot.worker_id, MMWorkerEngine::WorkerStatus::DEGRADED);
        }
    }
}

// Worker 端心跳发送
void RemoteWorker::heartbeat_loop() {
    while (running_) {
        // 更新本地心跳时间
        worker_->heartbeat();

        // 发送到 Master
        send_heartbeat_to_master();

        std::this_thread::sleep_for(std::chrono::milliseconds(heartbeat_interval_ms_));
    }
}
```

### 6.4 独立 Worker 节点启动

独立 Worker 节点不包含 Agent 和 Scheduler，仅负责执行任务。

**Worker 节点架构**：

```
┌─────────────────────────────────────────────────────────────┐
│                      Worker Node                             │
│                                                              │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────┐  │
│  │ MMWorkerEngine  │  │ Workflow Cache  │  │ Node Cache  │  │
│  │                 │  │                 │  │             │  │
│  │ - Task Queue    │  │ - Workflow 解析 │  │ - SO/Script │  │
│  │ - Control Queue │  │ - 实例管理      │  │ - 热更新    │  │
│  └─────────────────┘  └─────────────────┘  └─────────────┘  │
│          │                                                   │
│          ▼                                                   │
│  ┌─────────────────┐                                        │
│  │ Network Client  │◄─────── Master 连接（需集成方实现）    │
│  │                 │                                        │
│  │ - Register      │                                        │
│  │ - Heartbeat     │                                        │
│  │ - Task Receive  │                                        │
│  └─────────────────┘                                        │
└─────────────────────────────────────────────────────────────┘
```

**启动代码框架**：

```cpp
// worker_node.cpp - 独立 Worker 节点
#include "runtime/worker.h"
#include <iostream>
#include <csignal>

using namespace agentflow;

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 1. 创建 Worker Engine
    runtime::MMWorkerEngine::Config config;
    config.worker_id = "worker-001";
    config.default_workflow_threads = 4;
    config.business_threads = 8;

    auto worker = std::make_shared<runtime::MMWorkerEngine>(config);
    if (!worker->start()) {
        std::cerr << "Failed to start Worker Engine" << std::endl;
        return 1;
    }

    // 2. 连接 Master 并注册（需集成方实现）
    // - 建立 TCP/gRPC/HTTP 连接到 Master
    // - 发送注册请求（worker_id, capabilities）
    // - 接收注册响应

    // 3. 启动心跳线程（需集成方实现）
    // - 定期向 Master 发送心跳
    // - 包含：worker_id, load, status

    // 4. 启动任务接收循环（需集成方实现）
    // - 从 Master 接收任务
    // - 调用 worker->enqueue_task(task)

    std::cout << "Worker started: " << config.worker_id << std::endl;
    while (g_running) {
        worker->heartbeat();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    worker->stop();
    return 0;
}
```

### 6.5 Master-Worker 通信集成指南

> **重要说明**：AgentFlow 框架当前版本未内置 Master-Worker 网络通信实现。
> 集成方需要根据自身技术栈和部署环境自行实现网络通信层。

**通信层职责**：

| 功能 | Master 端 | Worker 端 |
|------|-----------|-----------|
| **注册** | 接收 Worker 注册请求，维护 Worker 列表 | 发送注册请求，包含 worker_id 和能力 |
| **心跳** | 监控 Worker 心跳，检测故障 | 定期发送心跳，报告负载和状态 |
| **任务分发** | 将任务分发到选定的 Worker | 接收任务并放入执行队列 |
| **结果返回** | 接收任务执行结果 | 发送执行结果和状态更新 |

**推荐的网络协议**：

| 协议 | 适用场景 | 特点 |
|------|----------|------|
| gRPC | 生产环境 | 高性能、类型安全、支持流式传输 |
| TCP Socket | 简单部署 | 轻量级、易于调试 |
| WebSocket | Web 集成 | 支持浏览器、穿透防火墙 |
| ZeroMQ | 高吞吐场景 | 低延迟、支持多种模式 |
| HTTP/REST | 跨语言集成 | 简单易用、广泛支持 |

**集成方需要实现的关键接口**：

```cpp
// Master 端接口（需集成方实现）
class IMasterNetworkService {
public:
    virtual ~IMasterNetworkService() = default;
    virtual bool start(int port) = 0;
    virtual void stop() = 0;
    virtual bool dispatch_task(const std::string& worker_id, const TaskData& task) = 0;
    virtual void broadcast_workflow(const std::string& workflow_content) = 0;
};

// Worker 端接口（需集成方实现）
class IWorkerNetworkClient {
public:
    virtual ~IWorkerNetworkClient() = default;
    virtual bool connect(const std::string& master_address) = 0;
    virtual bool register_worker(const std::string& worker_id, const WorkerCapabilities& caps) = 0;
    virtual bool send_heartbeat(const HeartbeatData& data) = 0;
    virtual bool send_task_result(const std::string& task_id, const TaskResult& result) = 0;
};
```

**与 AgentFlow 框架的集成点**：

- Master 端通过 `MMAgentRuntime::register_worker_engine()` 注册 Worker
- Master 端通过 `MMAgentRuntime::heartbeat_worker()` 更新 Worker 状态
- Worker 端通过 `MMWorkerEngine::enqueue_task()` 接收任务
- Worker 端通过 `MMWorkerEngine::heartbeat()` 更新本地心跳

---

## 7. 消息驱动执行流程

### 7.1 请求处理流程

```
┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
│  Client  │     │  Service │     │  Agent   │     │ Runtime  │
└────┬─────┘     └────┬─────┘     └────┬─────┘     └────┬─────┘
     │                │                │                │
     │  HTTP/WS       │                │                │
     │  Request       │                │                │
     │───────────────►│                │                │
     │                │                │                │
     │                │  route_request │                │
     │                │  _message()    │                │
     │                │───────────────►│                │
     │                │                │                │
     │                │                │  Session       │
     │                │                │  Routing       │
     │                │                │────────┐       │
     │                │                │        │       │
     │                │                │◄───────┘       │
     │                │                │                │
     │                │                │  Task          │
     │                │                │  Generation    │
     │                │                │────────┐       │
     │                │                │        │       │
     │                │                │◄───────┘       │
     │                │                │                │
     │                │                │  pop_generated │
     │                │                │  _task()       │
     │                │                │───────────────►│
     │                │                │                │
     │                │                │                │  Schedule
     │                │                │                │  to Worker
     │                │                │                │─────┐
     │                │                │                │     │
     │                │                │                │◄────┘
     │                │                │                │
     │                │  dequeue       │                │
     │                │  _response     │                │
     │                │◄───────────────│                │
     │                │                │                │
     │  HTTP/WS       │                │                │
     │  Response      │                │                │
     │◄───────────────│                │                │
     │                │                │                │
```

### 7.2 Session 管理

Session 通过 `user_id + client_id` 进行路由：

```cpp
// SessionRouter 内部逻辑
std::shared_ptr<MMAgentSession> SessionRouter::get_or_create_session(
    const std::string& user_id,
    const std::string& client_id) {

    std::string session_key = user_id + ":" + client_id;

    // 尝试复用已存在的 Session
    auto it = sessions_.find(session_key);
    if (it != sessions_.end()) {
        return it->second;
    }

    // 从 Session Pool 分配新 Session
    auto session = allocate_from_pool();
    sessions_[session_key] = session;
    return session;
}
```

### 7.3 Task 调度

```cpp
// Agent 生成 Task
void MMAgent::handle_routed_request_message(
    const std::shared_ptr<MMAgentSession>& session,
    const MMAgentMsgQue::Message& message) {

    // 1. 创建 Task
    auto task = build_runtime_task(session, ...);

    // 2. 放入生成队列
    {
        std::lock_guard<std::mutex> lock(generated_tasks_mutex_);
        generated_tasks_.push(task);
    }
}

// Runtime 轮询 Task
void MMAgentRuntime::agent_task_poller_loop() {
    while (running_) {
        for (auto& agent : list_agents()) {
            std::shared_ptr<MMAgentTask> task;
            if (agent->pop_generated_task(task)) {
                // 调度执行
                execute_workflow(agent->get_id(), task->workflow_id, inputs);
            }
        }
    }
}
```

### 7.4 上下文回调机制

AgentFlow 提供上下文回调机制，允许服务在对话创建时注入自定义上下文。这对于以下场景非常有用：
- 用户画像注入
- 历史会话上下文加载
- 动态上下文生成

#### 7.4.1 回调函数签名

```cpp
using ContextCallback = std::function<std::string(
    const std::string& user_id,      // 用户标识
    const std::string& agent_id,     // Agent 标识
    const std::string& user_input,   // 用户输入文本
    const std::string& previous_context  // 上一轮上下文（可能为空）
)>;
```

#### 7.4.2 回调配置

```cpp
// 配置结构
struct ContextCallbackConfig {
  ContextCallback callback;  // 回调函数
  bool async = true;         // 是否异步调用（默认 true）
  int timeout_ms = 5000;     // 同步调用超时时间（毫秒）
};
```

#### 7.4.3 设置回调

**同步模式**（推荐用于快速上下文生成）：

```cpp
auto agent = std::make_shared<framework::MMAgent>(config);

// 设置同步回调
agent->set_context_callback(
    [](const std::string& user_id,
       const std::string& agent_id,
       const std::string& user_input,
       const std::string& previous_context) -> std::string {
        // 从数据库或缓存加载用户上下文
        std::string user_profile = load_user_profile(user_id);
        std::string session_history = load_recent_history(user_id);

        // 构建上下文字符串
        return "UserProfile: " + user_profile + "\n"
               "RecentHistory: " + session_history + "\n"
               "PreviousContext: " + previous_context;
    },
    false  // async=false，同步模式
);
```

**异步模式**（适用于耗时操作，如外部 API 调用）：

```cpp
agent->set_context_callback(
    [](const std::string& user_id,
       const std::string& agent_id,
       const std::string& user_input,
       const std::string& previous_context) -> std::string {
        // 注意：异步模式下，回调结果不会自动存储到 DataBus
        // 回调函数应自行处理上下文存储
        std::string context = fetch_context_from_external_api(user_id);

        // 可选：存储到外部系统
        store_context_to_cache(user_id, context);

        return context;
    },
    true  // async=true，异步模式（默认）
);
```

#### 7.4.4 上下文存储位置

同步模式下，生成的上下文会自动存储到 DataBus：

| 键 | 说明 |
|----|------|
| `/session/{session-id}/ctx/{dialogue-id}/content` | 上下文内容 |
| `/session/{session-id}/ctx/{dialogue-id}/time-created` | 创建时间戳（毫秒） |

#### 7.4.5 在 Skill 节点中访问上下文

Skill 节点可以通过 DataBus 访问上下文：

```cpp
// 在 Node 实现中
std::string get_context_from_databus(
    const std::string& session_id,
    const std::string& dialogue_id,
    DataBus& databus) {

    std::string ctx_key = "/session/" + session_id +
                          "/ctx/" + dialogue_id + "/content";

    DataVariant value;
    if (databus.get(ctx_key, value)) {
        if (std::holds_alternative<std::string>(value)) {
            return std::get<std::string>(value);
        }
    }
    return "";
}
```

#### 7.4.6 清除回调

```cpp
agent->clear_context_callback();
```

#### 7.4.7 完整示例

```cpp
class ContextAwareAgentService {
public:
    bool initialize(const std::string& package_path) {
        agent_ = framework::MMAgent::create_from_package(package_path);
        if (!agent_ || !agent_->initialize()) {
            return false;
        }

        // 设置上下文回调
        agent_->set_context_callback(
            [this](const std::string& user_id,
                   const std::string& agent_id,
                   const std::string& user_input,
                   const std::string& previous_context) {
                return this->build_user_context(user_id, previous_context);
            },
            false  // 同步模式，确保上下文在对话处理前可用
        );

        return true;
    }

private:
    std::string build_user_context(
        const std::string& user_id,
        const std::string& previous_context) {

        // 加载用户画像
        auto profile = user_profile_service_.get_profile(user_id);

        // 加载最近对话摘要
        auto summary = conversation_service_.get_summary(user_id);

        // 构建上下文
        std::ostringstream ctx;
        ctx << "=== User Context ===\n";
        ctx << "Name: " << profile.name << "\n";
        ctx << "Preferences: " << profile.preferences << "\n";
        ctx << "Recent Topics: " << summary.topics << "\n";

        if (!previous_context.empty()) {
            ctx << "\n=== Previous Context ===\n";
            ctx << previous_context << "\n";
        }

        return ctx.str();
    }

    std::shared_ptr<framework::MMAgent> agent_;
    UserProfileService user_profile_service_;
    ConversationService conversation_service_;
};
```

#### 7.4.8 注意事项

1. **同步 vs 异步选择**：
   - 同步模式：上下文在对话处理前可用，但会阻塞请求处理
   - 异步模式：不阻塞请求处理，但上下文可能无法立即用于当前对话

2. **异步模式限制**：
   - 异步回调的结果不会自动存储到 DataBus
   - 回调函数应自行处理上下文的存储和分发

3. **线程安全**：
   - 回调函数可能在不同线程中执行
   - 确保回调中访问的共享资源是线程安全的

4. **错误处理**：
   - 回调抛出异常时，框架会记录错误日志并继续处理对话
   - 建议在回调内部捕获并处理所有异常

### 7.5 Skill 参数槽位校验

框架提供参数槽位校验机制，允许在 skill 启动前验证其执行所需的参数是否可用。

#### 7.5.1 配置 expected_parameters

在 `agent.json` 中为 skill 任务声明预期参数：

```json
{
  "tasks": [
    {
      "id": 3,
      "type": "skill",
      "description": "Travel booking skill",
      "workflow_file": "skill_travel_workflow.json",
      "skill": {
        "name": "travel",
        "config_dir": "runtime",
        "instance_pool_size": 2,
        "expected_parameters": ["destination", "date", "travelers"]
      }
    }
  ]
}
```

**字段说明**：

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `expected_parameters` | `string[]` | 否 | `[]` | Skill 执行所需的参数名列表 |

#### 7.5.2 输出 available_parameters

Workflow 完成时输出 `available_parameters`，提供当前可用的参数值：

**优先级策略**：

1. **消息 metadata（推荐）**：放入 workflow 完成消息的 metadata 中
2. **DataBus（向后兼容）**：存入 DataBus，用于兼容旧版本

**消息 metadata 方式**：

```cpp
// 在 workflow 完成时设置
MMAgentMsgQue::Message completion_message;
completion_message.content = "workflow_completed";
completion_message.metadata["next_skills"] = R"(["travel"])";
completion_message.metadata["available_parameters"] = R"({
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20",
    "travelers": "2"
  }
})";
```

**DataBus 方式**：

```cpp
// 存储到 DataBus
std::string dialogue_id = "abc123";
std::string params_json = R"({
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20"
  }
})";
databus.put(dialogue_id + "/available_parameters", params_json);
```

**数据结构**：

```typescript
// available_parameters 结构（参数值仅支持 string 类型）
{
  [skill_name: string]: {
    [parameter_name: string]: string
  }
}
```

#### 7.5.3 校验流程

框架在启动 skill 前自动执行校验：

```
┌─────────────────────────────────────────────────────────────────┐
│                     Skill Launch with Validation                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. Get skill template's expected_parameters                    │
│                                                                  │
│  2. If expected_parameters is empty                             │
│     └─> Skip validation, launch skill directly                  │
│                                                                  │
│  3. Get available_parameters (priority: metadata > DataBus)     │
│                                                                  │
│  4. For each expected_parameter:                                │
│     └─> Check if exists in available_parameters[skill_name]     │
│                                                                  │
│  5. If all expected parameters are covered:                     │
│     └─> Launch skill task                                       │
│                                                                  │
│  6. If any expected parameter is missing:                       │
│     └─> Skip skill launch                                       │
│     └─> Send failure message to coordinator                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 7.5.4 失败消息格式

参数校验失败时，框架向 coordinator 发送失败消息：

```cpp
MMAgentMsgQue::Message failure_message;
failure_message.message_id = "skill-failure-travel-abc123";
failure_message.sender_id = "framework";
failure_message.receiver_id = coordinator_task_id;
failure_message.content = "skill_launch_failed";

// Metadata fields
failure_message.metadata["task_type"] = "skill_response";
failure_message.metadata["message_type"] = "skill_launch_failed";
failure_message.metadata["skill_name"] = "travel";
failure_message.metadata["failure_reason"] = "parameter_validation_failed";
failure_message.metadata["expected_parameters"] = R"(["destination", "date", "travelers"])";
failure_message.metadata["available_parameters"] = R"({"destination": "Beijing", "date": "2024-03-20"})";
failure_message.metadata["missing_parameters"] = R"(["travelers"])";
```

**Metadata 字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `task_type` | string | 固定为 `"skill_response"` |
| `message_type` | string | `"skill_launch_failed"` 标识启动失败 |
| `skill_name` | string | 失败的 skill 名称 |
| `failure_reason` | string | 失败原因，此处为 `"parameter_validation_failed"` |
| `expected_parameters` | string | JSON 序列化的预期参数列表 |
| `available_parameters` | string | JSON 序列化的可用参数对象 |
| `missing_parameters` | string | JSON 序列化的缺失参数列表 |

#### 7.5.5 Coordinator 处理示例

Coordinator 可以处理失败消息并采取相应行动：

```cpp
void handle_skill_launch_failure(const MMAgentMsgQue::Message& message) {
    if (message.metadata.at("message_type") == "skill_launch_failed") {
        std::string skill_name = message.metadata.at("skill_name");
        std::string missing_params = message.metadata.at("missing_parameters");

        // 选项1：向用户请求补充信息
        std::string prompt = "Please provide the following information: " + missing_params;
        send_user_prompt(prompt);

        // 选项2：记录日志并降级处理
        LOG(INFO) << "Skill " << skill_name << " launch failed. Missing: " << missing_params;

        // 选项3：尝试使用默认值重试
        // ...
    }
}
```

#### 7.5.6 向后兼容性

- `expected_parameters` 为可选字段，未配置时行为与当前一致
- `available_parameters` 为可选输出，未提供时视为空对象
- 框架优先从消息 metadata 读取，DataBus 作为 fallback
- 完全向后兼容现有 agent.json 和 workflow

### 7.6 Multi-Response 流式输出

当 coordinator 需要返回多条响应时（如流式输出、渐进式结果推送），使用 `responses` 输出：

#### Node 实现

```cpp
// 第一次输出
set_output("responses", R"({"content": "处理中...", "done": false})");

// 第二次输出
set_output("responses", R"({"content": "中间结果", "done": false})");

// 最终输出
set_output("responses", R"({"content": "完成！", "done": true})");
```

也可以使用简化字符串格式（框架自动处理）：

```cpp
set_output("responses", "这是响应内容");  // done 默认为 false
```

#### DataBus Key 结构

```
/session/{session-id}/sys/{dialogue-id}/
├── response                    # 单响应（向后兼容）
├── responses/                  # 多响应
│   ├── total_count = N         # 当前响应总数
│   ├── consumed_count = M      # 已消费响应数
│   ├── done = true/false       # 会话是否完成
│   ├── 0/
│   │   ├── content = "响应1"
│   │   └── consumed = false
│   └── ...
```

#### 消费流程

框架自动消费多响应，每次 `data_exchange` 触发时：
1. 读取 `consumed_count` 定位下一个响应
2. 发送 `responses/{consumed_count}/content` 给客户端
3. 标记 `consumed=true`，递增 `consumed_count`
4. 若所有响应已消费且 `done=true`，清理 dialogue 数据

#### 向后兼容性

- 单响应 `response` key 继续工作
- 多响应 `responses` 为新增功能
- 两者可同时使用，框架优先处理多响应

### 7.7 next_skills 格式扩展

`next_skills` 支持为每个 skill 指定专属数据源，实现 skill 输入数据的精准分发。

#### 格式对比

**旧格式（向后兼容）**：

```json
["skill1", "skill2"]
```

**新格式**：

```json
[
  {"name": "skill1", "exec_skill_key": "travel_data"},
  {"name": "skill2", "exec_skill_key": "shopping_data"},
  {"name": "skill3"}
]
```

**混合格式**：

```json
["skill1", {"name": "skill2", "exec_skill_key": "key2"}, "skill3"]
```

#### 字段说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | Skill 名称 |
| `exec_skill_key` | string | 否 | DataBus key，用于读取该 skill 的专属数据 |

#### 使用场景

Dispatcher 解析用户输入后，将不同类型的数据存入 DataBus 不同 key，每个被调用的 skill 从各自的 `exec_skill_key` 读取其专属数据：

```
用户输入: "帮我规划北京-上海行程，顺便推荐一些当地美食"

Dispatcher 解析后存入 DataBus:
  /session/{id}/sys/{dialogue}/travel_data = "北京-上海行程规划..."
  /session/{id}/sys/{dialogue}/foodie_data = "当地美食推荐..."

Dispatcher 输出 next_skills:
  [
    {"name": "travel", "exec_skill_key": "travel_data"},
    {"name": "foodie", "exec_skill_key": "foodie_data"}
  ]

Skill Node 通过 inputs["exec_skill_key"] 读取各自的专属数据
```

#### Node 访问 exec_skill_key

在 skill node 中，通过 `inputs["exec_skill_key"]` 获取数据源 key，再从 DataBus 读取数据：

```cpp
// SO Library Node 示例
std::string exec_skill_key;
if(inputs.count("exec_skill_key")) {
  exec_skill_key = std::get<std::string>(inputs["exec_skill_key"]);
  // 从 DataBus 读取 skill 专属数据
  DataVariant skill_data;
  if(get_data(exec_skill_key, skill_data)) {
    // 处理数据...
  }
}
```

```bash
# Shell Node 示例
EXEC_SKILL_KEY="${inputs[exec_skill_key]}"
if [ -n "$EXEC_SKILL_KEY" ]; then
  SKILL_DATA=$(af_databus_get "$EXEC_SKILL_KEY")
  # 处理数据...
fi
```

```python
# Python Node 示例
exec_skill_key = inputs.get("exec_skill_key", "")
if exec_skill_key:
    skill_data = af_databus_get(exec_skill_key)
    # 处理数据...
```

#### 数据流

```
Dispatcher Node 输出:
  │
  ├── next_skills = [{"name": "travel", "exec_skill_key": "travel_data"}, ...]
  │
  └── DataBus:
        /session/{id}/sys/{dialogue}/travel_data = "..."
        /session/{id}/sys/{dialogue}/shopping_data = "..."

框架处理:
  │
  ├── parse_next_skills() 解析为 SkillSpec 列表
  │
  ├── launch_skill_task(travel, "travel_data", ...)
  │     └── skill_message.metadata["exec_skill_key"] = "travel_data"
  │     └── skill_task->set_parameter("exec_skill_key", "travel_data")
  │
  └── runtime.cpp:
        inputs["exec_skill_key"] = "travel_data"
        // Node 可通过 inputs 访问
```

#### 向后兼容性

- 旧格式字符串数组继续工作
- `exec_skill_key` 为可选字段，默认为空
- 混合格式（字符串和对象并存）完全支持

### 7.8 消息定向路由

默认情况下，外部消息经过 SessionRouter 路由到 session 的 dispatcher 队列，然后由框架遍历分发到目标 task。

如果外部消息携带了 `dialogue_id` 和 `task_id`，框架会直接将消息发送到目标 task 队列，跳过中间遍历，实现精确定向。

#### 消息格式

```json
{
  "session_id": "sess-123",
  "dialogue_id": "dialogue-456",
  "task_id": "sess-123_coordinator",
  "user_id": "user-001",
  "client_id": "client-001",
  "content": "消息内容"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `session_id` | 否 | Session ID，用于快速定位 session |
| `dialogue_id` | 是 | Dialogue ID，用于 dialogue 隔离 |
| `task_id` | 是 | 目标 task 的 ID |
| `user_id` | 否 | 用户标识 |
| `client_id` | 否 | 客户端标识 |

#### task_id 获取方式

**dispatcher / coordinator**：约定命名，可直接构造

```cpp
std::string dispatcher_task_id = session_id + "_dispatcher";
std::string coordinator_task_id = session_id + "_coordinator";
```

| Task 类型 | task_id 命名规则 | 示例 |
|-----------|-----------------|------|
| Dispatcher | `{session_id}_dispatcher` | `sess-123_dispatcher` |
| Coordinator | `{session_id}_coordinator` | `sess-123_coordinator` |

**runtime skill**：从 node 输出获取

Skill node 执行时可通过环境变量 `AF_SELF_TASK_ID` 获取自身 task_id，并在输出中返回给外部服务：

```json
{
  "outputs": {
    "self_task_id": "sess-123_task_3_runtime_abc_travel",
    "result": "执行结果"
  }
}
```

#### 使用示例

```cpp
// 外部服务向 coordinator 发送定向消息
MMAgentMsgQue::Message message;
message.message_id = "msg-001";
message.content = "用户输入";
message.metadata["session_id"] = session_id;
message.metadata["dialogue_id"] = dialogue_id;
message.metadata["task_id"] = session_id + "_coordinator";

agent->route_request_message(message);
```

#### 消息类型与队列选择

| MessageType | 目标队列 |
|-------------|---------|
| TASK_REQUEST, CONTROL | intent_request 队列 |
| TASK_RESPONSE, EVENT, NOTIFICATION, ERROR | data_exchange 队列 |

#### 降级策略

如果定向路由失败（task 不存在或未运行），框架自动降级到默认路由流程，保证消息不会丢失。

---

## 8. 部署模式

### 8.1 单进程部署

所有组件在同一进程内，适合开发测试和小规模部署：

```cpp
// 单进程部署示例
class SingleProcessService {
public:
    bool initialize(const std::string& package_path) {
        // 创建 Agent
        agent_ = framework::MMAgent::create_from_package(package_path);
        if (!agent_ || !agent_->initialize()) return false;

        // 创建 Runtime
        runtime_ = std::make_unique<runtime::MMAgentRuntime>();
        runtime_->register_agent(agent_);

        // 创建本地 Worker
        runtime::MMWorkerEngine::Config worker_config;
        worker_config.worker_id = "local-worker";
        local_worker_ = std::make_shared<runtime::MMWorkerEngine>(worker_config);
        local_worker_->start();
        runtime_->register_worker_engine(local_worker_);

        // 启动
        runtime_->start();
        agent_->start();

        return true;
    }

private:
    std::shared_ptr<framework::MMAgent> agent_;
    std::unique_ptr<runtime::MMAgentRuntime> runtime_;
    std::shared_ptr<runtime::MMWorkerEngine> local_worker_;
};
```

### 8.2 分布式部署

> **说明**：分布式部署需要集成方自行实现 Master-Worker 网络通信层。
> 详见 Section 6.5 "Master-Worker 通信集成指南"。

Master 和 Worker 分离部署：

```
┌─────────────────────────────────────────────────────────────┐
│                        Master Node                           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │   Agent     │  │  Runtime    │  │     Deployer        │  │
│  │  (N 个)     │  │  (MASTER)   │  │     Scheduler       │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│                         │                                   │
│                         ▼                                   │
│              ┌─────────────────────┐                        │
│              │ Network Service     │ ◄──需集成方实现        │
│              │ (Worker 注册/心跳)   │                        │
│              └─────────────────────┘                        │
└─────────────────────────────────────────────────────────────┘
            │                    │                    │
            ▼                    ▼                    ▼
     ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
     │ Worker Node 1│    │ Worker Node 2│    │ Worker Node N│
     │              │    │              │    │              │
     │ MMWorkerEngine   │ MMWorkerEngine   │ MMWorkerEngine │
     │ Network Client   │ Network Client   │ Network Client │◄─需集成方实现
     └──────────────┘    └──────────────┘    └──────────────┘
```

**Master 节点组件**：

| 组件 | 状态 | 说明 |
|------|------|------|
| MMAgentRuntime | ✅ 框架提供 | MASTER 角色 |
| Agent | ✅ 框架提供 | 业务逻辑 |
| Deployer/Scheduler | ✅ 框架提供 | 任务调度 |
| 网络服务 | ⚠️ 需集成方实现 | Worker 注册、心跳、任务分发 |

**Worker 节点组件**：

| 组件 | 状态 | 说明 |
|------|------|------|
| MMWorkerEngine | ✅ 框架提供 | 任务执行引擎 |
| 网络客户端 | ⚠️ 需集成方实现 | 连接 Master、注册、心跳、接收任务 |

**Master 配置示例**：

```cpp
runtime::MMAgentRuntime::Config config;
config.role = runtime::MMAgentRuntime::RuntimeRole::MASTER;
config.worker_threads = 8;

// 创建 Runtime 和 Agent
auto runtime = std::make_unique<runtime::MMAgentRuntime>(config);
auto agent = framework::MMAgent::create_from_package(package_path);
agent->initialize();
runtime->register_agent(agent);
runtime->start();
agent->start();

// 网络服务需集成方自行实现
// - 监听端口，接收 Worker 注册
// - 调用 runtime->register_worker_engine() 注册 Worker
// - 调用 runtime->heartbeat_worker() 更新心跳
```

**Worker 配置示例**：

```cpp
runtime::MMWorkerEngine::Config config;
config.worker_id = "worker-node-001";
config.default_workflow_threads = 4;
config.business_threads = 8;

auto worker = std::make_shared<runtime::MMWorkerEngine>(config);
worker->start();

// 网络客户端需集成方自行实现
// - 连接 Master 地址
// - 发送注册请求
// - 定期发送心跳：worker->heartbeat()
// - 接收任务：worker->enqueue_task(task)
```

### 8.3 容器化部署

**Dockerfile 示例**：

```dockerfile
FROM ubuntu:22.04

# 安装依赖
RUN apt-get update && apt-get install -y \
    build-essential cmake \
    && rm -rf /var/lib/apt/lists/*

# 复制构建产物
COPY build/lib/ /opt/agentflow/lib/
COPY build/bin/ /opt/agentflow/bin/
COPY examples/ /opt/agentflow/examples/

# 设置环境变量
ENV LD_LIBRARY_PATH=/opt/agentflow/lib:$LD_LIBRARY_PATH

# 启动命令
ENTRYPOINT ["/opt/agentflow/bin/agent_runner"]
```

**docker-compose.yml 示例**：

```yaml
version: '3.8'

services:
  master:
    build: .
    command: ["./bin/agent_runner", "/data/agent_package", "--interactive"]
    environment:
      - AGENTFLOW_ROLE=MASTER
    ports:
      - "8080:8080"
    volumes:
      - ./data:/data

  worker:
    build: .
    command: ["./bin/worker_daemon"]
    environment:
      - AGENTFLOW_ROLE=WORKER
      - AGENTFLOW_MASTER_ADDRESS=master:8080
    depends_on:
      - master
    deploy:
      replicas: 3
```

---

## 9. 完整示例

### 9.1 最小服务示例

参考 `examples/features/agent_runner.cpp` 和 `examples/features/interactive_runner.cpp`：

```cpp
// 完整的服务集成示例
class AgentService {
public:
    struct Config {
        std::string package_path;
        int runtime_threads = 4;
        int worker_threads = 2;
        int request_timeout_ms = 30000;
        bool verbose = false;
    };

    explicit AgentService(const Config& config) : config_(config) {}

    bool initialize() {
        // 1. 创建 Agent
        agent_ = framework::MMAgent::create_from_package(config_.package_path);
        if (!agent_) {
            std::cerr << "Failed to create agent from package" << std::endl;
            return false;
        }

        if (!agent_->initialize()) {
            std::cerr << "Failed to initialize agent" << std::endl;
            return false;
        }

        // 2. 创建 Runtime
        runtime::MMAgentRuntime::Config runtime_config;
        runtime_config.worker_threads = config_.runtime_threads;
        runtime_ = std::make_unique<runtime::MMAgentRuntime>(runtime_config);

        if (!runtime_->register_agent(agent_)) {
            std::cerr << "Failed to register agent" << std::endl;
            return false;
        }

        // 3. 创建本地 Worker
        runtime::MMWorkerEngine::Config worker_config;
        worker_config.worker_id = "local-worker";
        worker_config.default_workflow_threads = config_.worker_threads;
        worker_ = std::make_shared<runtime::MMWorkerEngine>(worker_config);
        worker_->start();
        runtime_->register_worker_engine(worker_);

        // 4. 启动
        if (!runtime_->start()) {
            std::cerr << "Failed to start runtime" << std::endl;
            return false;
        }

        if (!agent_->start()) {
            std::cerr << "Failed to start agent" << std::endl;
            return false;
        }

        if (config_.verbose) {
            std::cout << "Agent initialized: " << agent_->get_name() << std::endl;
            std::cout << "Workflows: " << agent_->get_workflows().size() << std::endl;
        }

        return true;
    }

    std::string execute(const std::string& workflow_id,
                       const std::string& user_id,
                       const std::string& client_id,
                       const std::string& input) {
        // 构造请求消息
        framework::MMAgentMsgQue::Message message;
        message.message_id = generate_message_id();
        message.type = framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
        message.metadata["message_type"] = "intent_request";
        message.sender_id = "service";
        message.receiver_id = agent_->get_id();
        message.content = input;
        message.timestamp = current_timestamp_ms();
        message.metadata["user_id"] = user_id;
        message.metadata["client_id"] = client_id;
        message.metadata["workflow_id"] = workflow_id;

        // 提交请求
        agent_->route_request_message(message);

        // 轮询响应
        return poll_response(config_.request_timeout_ms);
    }

    void shutdown() {
        if (agent_) agent_->stop();
        if (runtime_) runtime_->stop();
        if (worker_) worker_->stop();
    }

private:
    Config config_;
    std::shared_ptr<framework::MMAgent> agent_;
    std::unique_ptr<runtime::MMAgentRuntime> runtime_;
    std::shared_ptr<runtime::MMWorkerEngine> worker_;
    int64_t message_counter_ = 0;

    std::string generate_message_id() {
        return "msg_" + std::to_string(++message_counter_) + "_" +
               std::to_string(current_timestamp_ms());
    }

    int64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string poll_response(int timeout_ms) {
        auto start = std::chrono::steady_clock::now();
        std::ostringstream result;

        while (true) {
            framework::MMAgentMsgQue::Message response;
            while (agent_->dequeue_response_message(response)) {
                if (!response.content.empty()) {
                    result << response.content;
                }
            }

            if (!result.str().empty()) {
                return result.str();
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_ms) {
                return "[TIMEOUT] No response received";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
};
```

### 9.2 TUI 交互服务示例

参考 `examples/features/chat_bridge_support.h` 与
`examples/agent/multi_session_chat/multi_session_chat_tui.cpp`：

```cpp
// 简化的 TUI 集成
class TuiChatService {
public:
    bool initialize(const std::string& package_path) {
        bridge_ = std::make_unique<agentflow::examples::ChatAgentBridge>(package_path);
        return bridge_->initialize();
    }

    std::string handle_user_message(const std::string& session_id,
                                    const std::string& message,
                                    const std::string& history_json) {
        const std::string payload =
            "session=" + session_id + "\nuser=" + message + "\nhistory=" + history_json;
        return bridge_->execute("session-dispatcher-workflow", payload);
    }

private:
    std::unique_ptr<agentflow::examples::ChatAgentBridge> bridge_;
};
```

---

## 10. 相关文档

| 文档 | 说明 |
|------|------|
| `docs/ARCHITECTURE.md` | 架构设计 |
| `docs/agent-lifecycle.md` | Agent 生命周期管理 |
| `docs/api-reference.md` | API 参考 |
| `publish/agent/AGENT_PACKAGE_SPEC.md` | Agent 包规范 |
| `publish/workflow/NODE_DEVELOPMENT_SPEC.md` | Node 开发规范 |
| `include/internal/runtime/message_dispatcher.h` | MessageDispatcher 接口定义 |
| `include/internal/runtime/workflow_scheduler.h` | WorkflowScheduler 接口定义 |
| `include/internal/runtime/message_types.h` | 消息类型定义 |
| `examples/features/agent_runner.cpp` | Agent Runner 示例 |
| `examples/features/interactive_runner.cpp` | 交互式运行器示例 |
| `examples/agent/multi_session_chat/multi_session_chat_tui.cpp` | TUI 交互示例 |

---

*文档生成时间：2026-03-09*
