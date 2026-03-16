# AgentFlow 使用手册

> 本手册面向 AgentFlow 开发者，涵盖 Agent 集成指导和 Node 开发指导两大主题。
> - 文档版本：v1.0
> - 更新日期：2026-03-16

---

# 第一部分：Agent 集成指导

本部分说明如何将 AgentFlow 框架集成到服务中，包括 Agent/Runtime 生命周期管理、消息接口和上下文回调机制。

## 1. 核心组件概述

### 1.1 集成架构

AgentFlow 采用三层 Master-Worker 架构：

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
└─────────────────────────────────────────────────────────────────────┘
                                 │
          ┌──────────────────────┼──────────────────────┐
          ▼                      ▼                      ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│  MMWorkerEngine │  │  MMWorkerEngine │  │  MMWorkerEngine │
│  (Worker 1)     │  │  (Worker 2)     │  │  (Worker N)     │
└─────────────────┘  └─────────────────┘  └─────────────────┘
```

### 1.2 核心组件职责

| 组件 | 职责 | 头文件 |
|------|------|--------|
| **MMAgent** | Agent 实例管理、Session 管理、消息路由 | `include/internal/agent/agent.h` |
| **MMAgentRuntime** | 三级调度引擎、Agent/Worker 注册、任务分发 | `include/internal/runtime/agent_runtime.h` |
| **MMWorkerEngine** | Node 执行、Workflow 实例管理、热重载 | `include/internal/runtime/worker.h` |
| **AgentPackage** | Agent 包加载、解析、验证 | `include/internal/agent/package.h` |
| **MMAgentSession** | 会话管理、任务协调 | `include/internal/agent/session.h` |

---

## 2. Agent 生命周期管理

### 2.1 创建 Agent

#### 方式一：从 AgentPackage 创建（推荐）

```cpp
#include "agent/agent.h"
#include "agent/package.h"

// 从包目录加载
auto agent = agentflow::framework::MMAgent::create_from_package("/path/to/agent_package");
if (!agent) {
    std::cerr << "Failed to create agent from package" << std::endl;
    return -1;
}
```

#### 方式二：通过 Config 创建

```cpp
#include "agent/agent.h"

agentflow::framework::MMAgent::Config config;
config.id = "my_agent";
config.name = "My Agent";
config.version = "1.0.0";
config.description = "Agent description";
config.policy = agentflow::framework::MMAgent::LifecyclePolicy::SINGLETON;
config.max_workflows = 10;
config.max_concurrent_workflows = 5;
config.data_bus_capacity = 1024 * 1024;

auto agent = std::make_shared<agentflow::framework::MMAgent>(config);
```

### 2.2 Agent 生命周期状态

```cpp
enum class AgentState {
    CREATED,    // 已创建，未初始化
    INITIALIZED,// 已初始化，未启动
    RUNNING,    // 运行中
    PAUSED,     // 已暂停
    STOPPED,    // 已停止
    ERROR       // 错误状态
};
```

### 2.3 生命周期方法

```cpp
// 初始化 Agent（加载工作流、初始化资源）
bool initialize();

// 启动 Agent（开始处理消息）
bool start();

// 暂停 Agent（暂停消息处理）
bool pause();

// 恢复 Agent（恢复消息处理）
bool resume();

// 停止 Agent（停止所有处理）
bool stop();

// 获取当前状态
AgentState get_state() const;
```

### 2.4 完整生命周期示例

```cpp
#include "agent/agent.h"
#include "runtime/agent_runtime.h"
#include <iostream>

int main() {
    // 1. 创建 Agent
    auto agent = agentflow::framework::MMAgent::create_from_package("/path/to/agent_package");
    if (!agent) {
        std::cerr << "Failed to create agent" << std::endl;
        return 1;
    }

    // 2. 初始化 Agent
    if (!agent->initialize()) {
        std::cerr << "Failed to initialize agent" << std::endl;
        return 1;
    }
    std::cout << "Agent initialized: " << agent->get_state() << std::endl;

    // 3. 创建并启动 Runtime
    auto runtime = std::make_shared<agentflow::runtime::MMAgentRuntime>();
    runtime->register_agent(agent);
    runtime->start();

    // 4. 启动 Agent
    if (!agent->start()) {
        std::cerr << "Failed to start agent" << std::endl;
        return 1;
    }
    std::cout << "Agent running: " << agent->get_state() << std::endl;

    // 5. 业务处理...

    // 6. 停止 Agent
    agent->stop();
    runtime->stop();

    std::cout << "Agent stopped" << std::endl;
    return 0;
}
```

---

## 3. Runtime 生命周期管理

### 3.1 创建 Runtime

```cpp
#include "runtime/agent_runtime.h"

agentflow::runtime::MMAgentRuntime::Config config;
config.role = agentflow::runtime::MMAgentRuntime::RuntimeRole::MASTER;
config.worker_threads = 4;
config.local_worker_id = "runtime-local-worker";
config.worker_default_workflow_threads = 1;
config.worker_control_threads = 1;
config.worker_business_threads = 1;
config.worker_queue_size = 1024;

auto runtime = std::make_shared<agentflow::runtime::MMAgentRuntime>(config);
```

### 3.2 Runtime 核心方法

```cpp
// Agent 管理
bool register_agent(AgentPtr agent);
bool unregister_agent(const AgentId& agent_id);
AgentPtr get_agent(const AgentId& agent_id);
std::vector<AgentPtr> list_agents();

// Workflow 管理
bool load_workflow(const AgentId& agent_id, WorkflowPtr workflow);
bool unload_workflow(const AgentId& agent_id, const WorkflowId& workflow_id);

// Workflow 执行
bool execute_workflow(
    const AgentId& agent_id,
    const WorkflowId& workflow_id,
    const DataMap& inputs,
    const std::string& source_task_id = "",
    const std::string& session_id = "",
    const std::string& dialogue_id = "",
    const std::string& task_type = "",
    const std::string& skill_task_id = ""
);

// 生命周期
bool start();
bool stop();
bool is_running() const;
```

### 3.3 Runtime 执行示例

```cpp
// 注册 Agent
runtime->register_agent(agent);

// 启动 Runtime
if (!runtime->start()) {
    std::cerr << "Failed to start runtime" << std::endl;
    return 1;
}

// 执行 Workflow
agentflow::framework::DataMap inputs;
inputs["user_text"] = std::string("Hello, AgentFlow!");
inputs["session_id"] = std::string("session_001");

runtime->execute_workflow(
    agent->get_id(),
    "dispatcher-workflow",
    inputs,
    "",  // source_task_id
    "session_001",  // session_id
    "",  // dialogue_id
    "dispatcher",  // task_type
    ""  // skill_task_id
);

// 停止 Runtime
runtime->stop();
```

---

## 4. 消息队列接口

### 4.1 消息数据结构

```cpp
struct Message {
    std::string                        message_id;      // 消息唯一标识
    MessageType                        type;            // 消息类型
    std::string                        sender_id;       // 发送者 ID
    std::string                        receiver_id;     // 接收者 ID
    std::string                        content;         // 消息内容
    std::string                        data_ref;        // DataBus 数据引用
    int64_t                            timestamp;       // 时间戳
    std::map<std::string, std::string> metadata;        // 元数据
};

enum class MessageType {
    TASK_REQUEST  = 0,   // 任务请求
    TASK_RESPONSE = 1,   // 任务响应
    EVENT         = 2,   // 事件
    NOTIFICATION  = 3,   // 通知
    ERROR         = 4    // 错误
};
```

### 4.2 消息队列类型

```cpp
enum class MessageQueueType {
    INTENT_REQUEST = 0,  // 意图请求队列
    DATA_EXCHANGE  = 1   // 数据交换队列
};
```

### 4.3 发送请求消息

```cpp
// 构造请求消息
agentflow::framework::MMAgentMsgQue::Message request;
request.message_id = "msg_001";
request.type = agentflow::framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
request.sender_id = "client_001";
request.receiver_id = "agent_001";
request.content = "你好，请帮我处理这个请求";
request.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
).count();

// 设置元数据
request.metadata["message_type"] = "intent_request";
request.metadata["user_id"] = "user_123";
request.metadata["client_id"] = "client_001";
request.metadata["workflow_id"] = "dispatcher-workflow";

// 发送消息到 Agent
agent->route_request_message(request);
```

### 4.4 接收响应消息

```cpp
// 方式一：轮询单条消息
agentflow::framework::MMAgentMsgQue::Message response;
while (agent->dequeue_response_message(response)) {
    std::cout << "Response: " << response.content << std::endl;
    std::cout << "Message ID: " << response.message_id << std::endl;

    // 检查元数据
    if (response.metadata.count("dialogue_id")) {
        std::cout << "Dialogue ID: " << response.metadata["dialogue_id"] << std::endl;
    }
}

// 方式二：检查队列大小
int queue_size = agent->get_response_queue_size();
if (queue_size > 0) {
    std::cout << "Pending responses: " << queue_size << std::endl;
}
```

### 4.5 完整消息交互示例

```cpp
void process_user_request(
    const std::shared_ptr<agentflow::framework::MMAgent>& agent,
    const std::string& user_input,
    const std::string& user_id
) {
    // 构造请求
    agentflow::framework::MMAgentMsgQue::Message request;
    request.message_id = generate_uuid();
    request.type = agentflow::framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
    request.content = user_input;
    request.metadata["message_type"] = "intent_request";
    request.metadata["user_id"] = user_id;
    request.metadata["client_id"] = "default_client";

    // 发送请求
    agent->route_request_message(request);
    std::cout << "Request sent: " << request.message_id << std::endl;

    // 等待响应
    int max_wait_seconds = 30;
    int waited = 0;
    while (waited < max_wait_seconds) {
        agentflow::framework::MMAgentMsgQue::Message response;
        while (agent->dequeue_response_message(response)) {
            if (!response.content.empty()) {
                std::cout << "Response: " << response.content << std::endl;
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        waited++;
    }
    std::cout << "Timeout waiting for response" << std::endl;
}
```

---

## 5. Session 管理

### 5.1 Session 数据结构

```cpp
class MMAgentSession {
public:
    enum class SessionState {
        CREATED = 0,    // 已创建
        ACTIVE = 1,     // 活跃
        PAUSED = 2,     // 暂停
        COMPLETED = 3,  // 完成
        TERMINATED = 4  // 终止
    };

    struct Config {
        int         session_index = -1;
        std::string session_id;
        std::string description;
        int         max_queue_size = 1024;
        int64_t     timeout_ms = 30000;
        int         max_retries = 3;
        std::string dispatcher_workflow;
        std::string coordinator_workflow;
        std::map<std::string, std::string> task_workflows;
        std::string task_context_workflow;
    };
};
```

### 5.2 Session 生命周期

```cpp
// 获取或创建 Session
auto session = agent->get_or_create_session("user_001", "client_001");

// 获取 Session 信息
std::string session_id = session->get_session_id();
std::string user_id = session->get_user_id();
std::string client_id = session->get_client_id();
SessionState state = session->get_state();

// Session 状态控制
session->activate();
session->pause();
session->resume();
session->complete();
session->terminate();

// 检查 Session 状态
bool is_assigned = session->is_assigned();
bool is_available = session->is_available();
bool is_timeout = session->is_timeout_expired();
```

### 5.3 Session 元数据管理

```cpp
// 设置元数据
session->set_metadata("custom_key", "custom_value");
session->set_metadata("preference", "zh-CN");

// 获取元数据
std::string value;
if (session->get_metadata("custom_key", value)) {
    std::cout << "Custom key: " << value << std::endl;
}

// 检查元数据是否存在
if (session->has_metadata("preference")) {
    // ...
}

// 清除所有元数据
session->clear_metadata();
```

---

## 6. DataBus 数据访问

### 6.1 Agent 级别 DataBus 访问

```cpp
// 存储数据
agent->publish_data("custom_key", std::string("custom_value"));

// 读取数据
agentflow::framework::DataVariant value;
if (agent->get_data("custom_key", value)) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        std::cout << "Value: " << *text << std::endl;
    }
}
```

### 6.2 Session 级别数据访问

```cpp
auto session = agent->get_session("session_001");

// 获取 Session 输出
agentflow::framework::DataVariant output;
if (agent->get_session_output(session->get_session_id(), "response", output)) {
    if (const auto* text = std::get_if<std::string>(&output)) {
        std::cout << "Response: " << *text << std::endl;
    }
}

// 获取所有 Session ID
auto session_ids = agent->get_all_session_ids();
for (const auto& sid : session_ids) {
    std::cout << "Session: " << sid << std::endl;
}
```

---

## 7. 上下文回调机制

### 7.1 回调签名

```cpp
using ContextCallback = std::function<std::string(
    const std::string& user_id,      // 用户标识
    const std::string& agent_id,     // Agent 标识
    const std::string& user_input,   // 用户输入
    const std::string& previous_context  // 之前的上下文
)>;
```

### 7.2 配置回调

```cpp
// 方式一：完整配置
agentflow::framework::ContextCallbackConfig config;
config.callback = [](const std::string& user_id,
                     const std::string& agent_id,
                     const std::string& user_input,
                     const std::string& previous_context) {
    // 生成上下文逻辑
    std::string context = "User: " + user_id + "\n";
    context += "Query: " + user_input + "\n";
    if (!previous_context.empty()) {
        context += "Previous: " + previous_context + "\n";
    }
    return context;
};
config.async = true;  // 异步调用
config.timeout_ms = 5000;  // 同步超时时间

agent->set_context_callback(config);

// 方式二：简化配置
agent->set_context_callback([](const std::string& user_id,
                               const std::string& agent_id,
                               const std::string& user_input,
                               const std::string& previous_context) {
    return "Context for " + user_input;
}, true);  // async = true

// 清除回调
agent->clear_context_callback();

// 检查是否设置了回调
if (agent->has_context_callback()) {
    std::cout << "Context callback is configured" << std::endl;
}
```

### 7.3 上下文存储位置

回调生成的上下文存储在 DataBus 的 ctx 命名空间：

```
/session/{session-id}/ctx/{dialogue-id}/content       // 上下文内容
/session/{session-id}/ctx/{dialogue-id}/time-created  // 创建时间戳
```

### 7.4 同步与异步模式

| 模式 | 特点 | 适用场景 |
|------|------|----------|
| **异步** (async=true) | 不阻塞消息处理，在独立线程执行 | 耗时的上下文生成（如调用外部服务） |
| **同步** (async=false) | 阻塞直到回调完成，支持超时 | 快速的上下文生成，需要立即使用上下文 |

---

## 8. 完整集成示例

```cpp
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

#include "agent/agent.h"
#include "runtime/agent_runtime.h"

using namespace agentflow;

int main() {
    // 1. 创建 Agent
    auto agent = framework::MMAgent::create_from_package("./agent_package");
    if (!agent) {
        std::cerr << "Failed to create agent" << std::endl;
        return 1;
    }

    // 2. 设置上下文回调
    agent->set_context_callback([](
        const std::string& user_id,
        const std::string& agent_id,
        const std::string& user_input,
        const std::string& previous_context
    ) {
        return "Context for user " + user_id + ": " + user_input;
    }, true);

    // 3. 初始化 Agent
    if (!agent->initialize()) {
        std::cerr << "Failed to initialize agent" << std::endl;
        return 1;
    }

    // 4. 创建并启动 Runtime
    auto runtime = std::make_shared<runtime::MMAgentRuntime>();
    runtime->register_agent(agent);
    runtime->start();

    // 5. 启动 Agent
    agent->start();
    std::cout << "Agent started: " << agent->get_id() << std::endl;

    // 6. 发送用户请求
    framework::MMAgentMsgQue::Message request;
    request.message_id = "msg_001";
    request.type = framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
    request.content = "你好，请帮我处理这个问题";
    request.metadata["message_type"] = "intent_request";
    request.metadata["user_id"] = "user_001";
    request.metadata["client_id"] = "client_001";

    agent->route_request_message(request);

    // 7. 等待并获取响应
    for (int i = 0; i < 30; ++i) {
        framework::MMAgentMsgQue::Message response;
        while (agent->dequeue_response_message(response)) {
            if (!response.content.empty()) {
                std::cout << "Response: " << response.content << std::endl;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 8. 清理
    agent->stop();
    runtime->stop();

    return 0;
}
```

---

# 第二部分：Node 开发指导

本部分说明 Node 的数据格式标准、预置关键字及其使用方法。

## 9. 数据结构基础

### 9.1 核心数据类型

```cpp
// 定义于 include/external/agentflow/so_node.h
using SoDataVariant = std::variant<std::monostate, bool, int64_t, double, std::string>;
```

**支持的类型：**

| 类型 | C++ 类型 | 说明 |
|------|----------|------|
| 空值 | `std::monostate` | 表示未设置或无效值 |
| 布尔 | `bool` | true/false |
| 整数 | `int64_t` | 64位有符号整数 |
| 浮点 | `double` | 双精度浮点数 |
| 字符串 | `std::string` | UTF-8 字符串 |

### 9.2 数据映射类型

```cpp
using SoDataMap = std::map<std::string, SoDataVariant>;
```

### 9.3 字段定义结构

```cpp
struct FieldDefinition {
    std::string  name;                              // 字段名称
    FieldType    type          = FieldType::STRING; // 字段类型
    TransferMode transfer_mode = TransferMode::VALUE; // 传输模式
    bool         required      = false;             // 是否必需
};

enum class FieldType {
    STRING    = 0,  // 字符串
    INT       = 1,  // 整数
    FLOAT     = 2,  // 浮点数
    BOOL      = 3,  // 布尔值
    LIST      = 4,  // 列表（JSON序列化存储）
    MAP       = 5,  // 映射（JSON序列化存储）
    FILE      = 6,  // 文件路径
    FILE_LIST = 7   // 文件列表
};
```

---

## 10. 节点签名规范

### 10.1 V3 签名（推荐）

Session 隔离的 DataBus 访问：

```cpp
#include "agentflow/so_node.h"

extern "C" void my_node_v3(
    const SoDataMap& inputs,      // 输入数据
    SoDataMap& outputs,           // 输出数据
    INodeDataBusClient* client    // Session 隔离的 DataBus 客户端
) {
    // 使用 DataBus Client
    std::string result = "processed";

    // 存储到 user namespace
    client->user()->put("result", SoDataVariant(result));

    // 设置输出
    outputs["status"] = SoDataVariant(std::string("SUCCESS"));
    outputs["response"] = SoDataVariant(result);
}
```

### 10.2 V2 签名（向后兼容）

```cpp
extern "C" void my_node_v2(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBus* databus
) {
    if (databus) {
        databus->put("key", SoDataVariant(std::string("value")));
    }
}
```

### 10.3 V1 签名（基础）

```cpp
extern "C" void my_node(
    const SoDataMap& inputs,
    SoDataMap& outputs
) {
    outputs["result"] = SoDataVariant(std::string("ok"));
}
```

### 10.4 函数加载优先级

框架按以下顺序尝试加载：
1. `{function_name}_v3` - Session 隔离的 DataBus client
2. `{function_name}_v2` - 原始 DataBus 访问
3. `{function_name}` - 基础输入输出

---

## 11. 框架预置关键字详解

### 11.1 输入关键字（框架自动注入）

| 关键字 | 类型 | 来源 | 功能说明 | 使用方式 |
|--------|------|------|----------|----------|
| `session_id` | string | 运行时 | 当前会话ID，用于会话隔离 | 只读，用于构建 DataBus key |
| `dialogue_id` | string | Agent | 对话ID，用于多对话并行隔离 | 只读，用于构建 DataBus key |
| `user_text` | string | 消息 | 用户输入的原始文本 | 只读，用于理解用户意图 |
| `user_prompt` | string | Agent | 用户提示词（存储在DataBus） | 通过 DataBus 读取 |
| `message_id` | string | 消息 | 消息唯一标识 | 只读，用于日志追踪 |
| `message_content` | string | 消息 | 消息内容 | 只读，获取完整消息体 |
| `trace_id` | string | 运行时 | 追踪ID，用于日志关联 | 只读，用于日志 |
| `workflow_id` | string | 运行时 | 当前工作流ID | 只读 |
| `node_id` | string | 运行时 | 当前节点ID | 只读 |
| `task_id` | string | 运行时 | 当前任务ID | 只读 |
| `agent_id` | string | 运行时 | 当前Agent ID | 只读 |

**使用示例：**

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    // 获取框架注入的关键字
    std::string session_id = agentflow::so_node::get_string(inputs, "session_id", "");
    std::string dialogue_id = agentflow::so_node::get_string(inputs, "dialogue_id", "");
    std::string user_text = agentflow::so_node::get_string(inputs, "user_text", "");

    // 使用 dialogue_id 构建 DataBus key
    std::string result_key = dialogue_id.empty() ? "result" : dialogue_id + "/result";
    client->user()->put(result_key, SoDataVariant(std::string("processed")));
}
```

### 11.2 输出关键字（框架特殊处理）

| 关键字 | 类型 | 命名空间 | 功能说明 | 效果 |
|--------|------|----------|----------|------|
| `response` | string | sys | Skill 执行结果 | 框架检测到此字段后，将其作为响应返回给用户 |
| `next_skills` | string | sys | 后续要拉起的 skill 列表 | 框架解析此字段并启动指定的 skills |
| `selected_skills` | string | sys | 已选择的 skill 列表 | 向后兼容字段，同 next_skills |
| `skill_completed` | bool | sys | Skill 是否完成 | 标记 skill 完成状态，默认 true |
| `status` | string | - | 节点执行状态 | 用于流程控制和日志 |
| `stdout` | string | - | 脚本标准输出 | 自动填充 |
| `stderr` | string | - | 脚本标准错误 | 自动填充 |
| `exit_code` | int | - | 进程退出码 | 自动填充 |

**response 字段示例：**

```cpp
// 在 skill workflow 最后一个节点设置 response
outputs["response"] = SoDataVariant(std::string("处理完成，结果如下..."));

// 同时写入 DataBus 的 sys namespace
client->sys()->put("response", SoDataVariant(std::string("处理完成，结果如下...")));
```

**next_skills 字段示例：**

```cpp
// 设置后续要启动的 skills
// JSON 数组格式（推荐）
outputs["next_skills"] = SoDataVariant(std::string("[\"search\", \"summary\"]"));

// 或逗号分隔格式
outputs["next_skills"] = SoDataVariant(std::string("search, summary"));

// 同时写入 DataBus
client->sys()->put(dialogue_id + "/next_skills", SoDataVariant(std::string("[\"search\", \"summary\"]")));
```

### 11.3 DataBus 命名空间关键字

| 命名空间 | 用途 | 预置关键字 | 说明 |
|----------|------|------------|------|
| `sys` | 框架通信 | `response`, `api_key`, `next_skills`, `skills_scheduled`, `pending_skills_count` | 框架级别数据 |
| `ctx` | 上下文存储 | `{dialogue_id}/content`, `{dialogue_id}/time-created` | 上下文回调生成的内容 |
| `llm` | LLM 交互 | `last_prompt`, `last_response` | LLM 调用记录 |
| `tool` | 工具调用 | `call_result`, `tool_list` | 工具调用结果 |
| `user` | 自定义数据 | 自定义 keys | 节点自定义数据 |

**DataBus 关键字详解：**

| 关键字 | 命名空间 | 完整 Key 格式 | 功能说明 | 读写方式 |
|--------|----------|---------------|----------|----------|
| `response` | sys | `{dialogue_id}/response` | 最终响应内容 | Node 写入，框架读取返回用户 |
| `dialogue_id` | sys | `{dialogue_id}/dialogue_id` | 对话唯一标识 | 框架写入，Node 读取 |
| `user_prompt` | sys | `{dialogue_id}/user_prompt` | 用户输入提示词 | 框架写入，Node 读取 |
| `next_skills` | sys | `{dialogue_id}/next_skills` | 待拉起的 skill 列表 | Node 写入，框架读取并调度 |
| `skills_scheduled` | sys | `{dialogue_id}/skills_scheduled` | 防止重复调度标志 | 框架读写，Node 可读取 |
| `pending_skills_count` | sys | `{dialogue_id}/pending_skills_count` | 正在执行的 skill 计数 | 框架读写，Coordinator 读取判断 |
| `content` | ctx | `/session/{sid}/ctx/{did}/content` | 对话上下文内容 | 回调写入，Node 读取 |
| `time-created` | ctx | `/session/{sid}/ctx/{did}/time-created` | 上下文创建时间戳 | 回调写入 |

---

## 12. 输入数据处理

### 12.1 获取输入值

```cpp
#include "agentflow/so_node.h"

extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    // 使用 helper 函数获取值
    std::string text = agentflow::so_node::get_string(inputs, "text", "default");
    int64_t count = agentflow::so_node::get_int(inputs, "count", 0);
    double score = agentflow::so_node::get_double(inputs, "score", 0.0);
    bool flag = agentflow::so_node::get_bool(inputs, "flag", false);

    // 检查 key 是否存在
    if (agentflow::so_node::has_key(inputs, "optional_param")) {
        // 存在则处理
    }
}
```

### 12.2 直接访问输入

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    auto it = inputs.find("custom_key");
    if (it != inputs.end()) {
        // 根据类型处理
        if (const auto* text = std::get_if<std::string>(&it->second)) {
            // 字符串类型
        } else if (const auto* num = std::get_if<int64_t>(&it->second)) {
            // 整数类型
        }
    }
}
```

### 12.3 从 DataBus 读取数据

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    std::string dialogue_id = agentflow::so_node::get_string(inputs, "dialogue_id", "");

    // 从 user namespace 读取
    std::string result_key = dialogue_id.empty() ? "travel_result" : dialogue_id + "/travel_result";
    SoDataVariant value;
    if (client->user()->get(result_key, value)) {
        if (const auto* text = std::get_if<std::string>(&value)) {
            outputs["travel_result"] = SoDataVariant(*text);
        }
    }

    // 从 sys namespace 读取配置
    SoDataVariant api_key;
    if (client->sys()->get("api_key", api_key)) {
        // 使用 api_key
    }
}
```

---

## 13. 输出数据处理

### 13.1 设置输出值

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    // 设置各种类型的输出
    outputs["text_result"] = SoDataVariant(std::string("hello"));
    outputs["count"] = SoDataVariant(int64_t(42));
    outputs["score"] = SoDataVariant(0.95);
    outputs["success"] = SoDataVariant(true);
    outputs["status"] = SoDataVariant(std::string("SUCCESS"));
}
```

### 13.2 写入 DataBus

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    std::string dialogue_id = agentflow::so_node::get_string(inputs, "dialogue_id", "");

    // 写入 user namespace
    std::string result_key = dialogue_id.empty() ? "result" : dialogue_id + "/result";
    client->user()->put(result_key, SoDataVariant(std::string("processed")));

    // 写入 response 到 sys namespace（返回给用户）
    std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
    client->sys()->put(response_key, SoDataVariant(std::string("处理完成")));

    // 设置 next_skills（启动后续 skills）
    std::string next_skills_key = dialogue_id.empty() ? "next_skills" : dialogue_id + "/next_skills";
    client->sys()->put(next_skills_key, SoDataVariant(std::string("[\"search\", \"summary\"]")));
}
```

---

## 14. 多对话数据处理

### 14.1 使用 dialogue_id 隔离数据

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    // 获取 dialogue_id
    std::string dialogue_id = agentflow::so_node::get_string(inputs, "dialogue_id", "");

    // 构建 dialogue 隔离的 key
    std::string user_result_key = dialogue_id.empty() ? "result" : dialogue_id + "/result";
    std::string sys_response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";

    // 存储数据
    client->user()->put(user_result_key, SoDataVariant(std::string("processed")));
    client->sys()->put(sys_response_key, SoDataVariant(std::string("done")));

    // 设置输出
    outputs["response"] = SoDataVariant(std::string("done"));
}
```

### 14.2 Coordinator 聚合多 Skill 结果

```cpp
extern "C" void coordinator_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                                    INodeDataBusClient* client) {
    std::string dialogue_id = agentflow::so_node::get_string(inputs, "dialogue_id", "");

    // 检查是否所有 skills 完成
    std::string pending_key = dialogue_id.empty() ? "pending_skills_count" : dialogue_id + "/pending_skills_count";
    SoDataVariant pending_value;
    long pending_count = 0;
    if (client->sys()->get(pending_key, pending_value)) {
        if (const auto* count = std::get_if<int64_t>(&pending_value)) {
            pending_count = *count;
        }
    }

    if (pending_count > 0) {
        outputs["status"] = SoDataVariant(std::string("waiting_for_skills"));
        return;
    }

    // 聚合所有 skill 结果
    std::string travel_key = dialogue_id.empty() ? "travel_result" : dialogue_id + "/travel_result";
    std::string shopping_key = dialogue_id.empty() ? "shopping_result" : dialogue_id + "/shopping_result";

    SoDataVariant travel_result, shopping_result;
    std::string aggregated;

    if (client->user()->get(travel_key, travel_result)) {
        if (const auto* text = std::get_if<std::string>(&travel_result)) {
            aggregated += "[Travel] " + *text + "\n";
        }
    }

    if (client->user()->get(shopping_key, shopping_result)) {
        if (const auto* text = std::get_if<std::string>(&shopping_result)) {
            aggregated += "[Shopping] " + *text + "\n";
        }
    }

    // 设置最终响应
    outputs["response"] = SoDataVariant(aggregated);
    client->sys()->put(dialogue_id + "/response", SoDataVariant(aggregated));
}
```

---

## 15. Workflow DSL 示例

### 15.1 基础节点定义

```json
{
  "id": "process_node",
  "name": "数据处理节点",
  "inputs": [
    { "name": "input_data", "type": "STRING", "required": true },
    { "name": "dialogue_id", "type": "STRING", "required": false }
  ],
  "outputs": [
    { "name": "result", "type": "STRING" },
    { "name": "status", "type": "STRING" }
  ],
  "execution_mode": "so_library",
  "command": "lib/process_node.so",
  "function_name": "process"
}
```

### 15.2 Skill Workflow 完整示例

```json
{
  "id": "search_skill",
  "nodes": [
    {
      "id": "search",
      "inputs": ["query", "dialogue_id"],
      "outputs": ["search_result"]
    },
    {
      "id": "format_response",
      "inputs": ["search_result", "dialogue_id"],
      "outputs": ["response", "next_skills"]
    }
  ],
  "edges": [
    {
      "source_node": "search",
      "target_node": "format_response",
      "source_outputs": ["search_result"],
      "target_inputs": ["search_result"]
    }
  ]
}
```

---

## 16. 最佳实践

### 16.1 命名规范

- 使用 snake_case 命名变量和字段
- 名称应具有描述性
- 避免使用框架保留关键字作为自定义字段名

### 16.2 错误处理

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    // 检查必需参数
    if (!agentflow::so_node::has_key(inputs, "required_param")) {
        outputs["error"] = SoDataVariant(std::string("Missing required parameter"));
        outputs["status"] = SoDataVariant(std::string("FAILED"));
        return;
    }

    // 处理逻辑...

    outputs["status"] = SoDataVariant(std::string("SUCCESS"));
}
```

### 16.3 数据大小建议

- 小于 1MB：直接使用值传递
- 大于 1MB：使用引用传递或文件路径
- 大数据传输：考虑使用 UDS 或共享内存

### 16.4 日志记录

```cpp
#include "utils/logger.h"

extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    std::string dialogue_id = agentflow::so_node::get_string(inputs, "dialogue_id", "");

    agentflow::utils::Logger::instance().info_f(
        "[MY_NODE] Processing for dialogue_id=%s", dialogue_id.c_str());

    // 处理逻辑...

    agentflow::utils::Logger::instance().info_f(
        "[MY_NODE] Completed for dialogue_id=%s", dialogue_id.c_str());
}
```

---

## 附录：快速参考

### A. 框架预置关键字速查表

| 类别 | 关键字 | 类型 | 作用 | 方向 |
|------|--------|------|------|------|
| **会话** | session_id | string | 会话标识 | 输入 |
| **对话** | dialogue_id | string | 对话标识 | 输入 |
| **消息** | user_text | string | 用户输入 | 输入 |
| **消息** | message_id | string | 消息标识 | 输入 |
| **响应** | response | string | 最终响应 | 输出 |
| **调度** | next_skills | string | 后续 skills | 输出 |
| **状态** | skill_completed | bool | 完成标志 | 输出 |
| **执行** | status | string | 执行状态 | 输出 |

### B. DataBus 命名空间速查表

| Namespace | 用途 | 访问方法 |
|-----------|------|----------|
| sys | 框架通信 | `client->sys()` |
| ctx | 上下文存储 | `client->ctx()` |
| llm | LLM 交互 | `client->llm()` |
| tool | 工具调用 | `client->tool()` |
| user | 自定义数据 | `client->user()` |
| raw | 原始访问 | `client->raw()` |

### C. Helper 函数速查表

```cpp
// 获取值
agentflow::so_node::get_string(inputs, "key", "default");
agentflow::so_node::get_int(inputs, "key", 0);
agentflow::so_node::get_double(inputs, "key", 0.0);
agentflow::so_node::get_bool(inputs, "key", false);

// 检查存在
agentflow::so_node::has_key(inputs, "key");

// 转换为字符串
agentflow::so_node::to_string(variant);
```