## 2026-03-26 Node异步请求外部资源机制设计

### 背景

当前node执行是同步阻塞的，当node需要访问外部资源（如LLM流式响应）时，会阻塞worker thread，导致并发能力下降。

**问题**：
- Worker thread被耗时远端请求阻塞
- 无法实现真正的非阻塞流式响应
- 多个并发请求时容易耗尽worker pool

**需求**：
实现node非阻塞式处理外部流响应，支持：
1. 异步调用外部资源
2. 流式chunk回调到node自身
3. 异步操作期间workflow不提前结束

### 方案简述

1. **AsyncPending机制**：在WorkflowExecutionState中新增`async_pending_count`计数器，防止workflow在异步操作未完成时提前结束
2. **AsyncCall SDK**：提供`AsyncCall(key, func)`、`SendMsg()`、`SendToSelf()`接口
3. **短路路由**：SendToSelf直接将消息放入task队列，跳过agent路由层
4. **多语言支持**：为Python/Shell/Go提供异步调用SDK

### 约束与假设

- 本次只实现node自我异步回调流程
- 异步函数通过独立线程执行
- SendToSelf采用短路路由，不经过agent层
- SDK通过INodeDataBusClient.async()暴露，保持V3签名兼容
- 流式chunk通过多次SendToSelf发送

### 当前实现分析

#### 代码分析

**WorkflowExecutionState** (`include/internal/runtime/agent_runtime.h:158`):
```cpp
struct WorkflowExecutionState {
  WorkflowId workflow_id;
  AgentId agent_id;
  std::string source_task_id;
  std::string session_id;
  std::string dialogue_id;
  std::string task_type;
  std::string skill_task_id;
  std::map<NodeId, bool> completed_nodes;
  std::map<NodeId, bool> failed_nodes;
  std::set<NodeId> queued_nodes;
  std::set<NodeId> running_nodes;
  int total_nodes = 0;
  int completed_count = 0;
  bool in_progress = false;
};
```

**workflow完成条件** (`src/runtime/runtime.cpp:1091`):
```cpp
if(state.completed_count + static_cast<int>(state.failed_nodes.size()) >= state.total_nodes) {
  state.in_progress = false;
  // ... 触发workflow完成
}
```

**消息定向路由** (`src/agent/agent.cpp:891`):
- 已实现基于`dialogue_id`和`task_id`的定向路由
- 可直接将消息放入目标task队列

**Task消息队列** (`include/internal/agent/task.h`):
- `enqueue_data_exchange_message()` - 数据交换消息队列
- `enqueue_intent_request_message()` - 意图请求消息队列
- 多线程安全

#### 缺失功能分析

| 缺失项 | 说明 |
|--------|------|
| 异步计数机制 | 无法阻止workflow在异步操作期间完成 |
| AsyncCall SDK | 无异步调用接口 |
| 短路路由 | SendToSelf需跳过agent路由层 |
| ASYNC_PENDING状态 | Node需要新状态区分异步等待 |
| 多语言SDK | Python/Shell/Go缺少异步调用支持 |

**行业实践参考**：

| 方案 | 代表实现 | 异步模型 | 适用场景 |
|------|---------|---------|---------|
| Callback-based | Node.js, libuv | 回调函数 | I/O密集型 |
| Future/Promise | C++ std::future, Python asyncio | 异步等待 | 通用 |
| Actor消息 | Erlang, Akka | 消息传递 | 分布式系统 |
| Coroutine | Go goroutine, Kotlin | 轻量级线程 | 高并发 |

**选择**：采用Actor消息模型，异步操作完成后通过消息回调到node自身，与现有消息路由机制一致。

### 数据结构设计

#### 1. WorkflowExecutionState扩展

```cpp
// 已有实现
struct WorkflowExecutionState {
  // ... existing fields ...

  // 新增：异步操作计数
  std::atomic<int> async_pending_count{0};

  bool has_async_pending() const { return async_pending_count > 0; }
  void increment_async_pending() { async_pending_count++; }
  void decrement_async_pending() { async_pending_count--; }
};
```

#### 2. NodeState扩展

```cpp
// 已有实现
enum class NodeState {
  WAITING = 0,
  READY = 1,
  EXECUTING = 2,
  COMPLETED = 3,
  FAILED = 4,

  // 新增：异步等待状态
  ASYNC_PENDING = 5
};
```

#### 3. IAsyncCallSDK接口

```cpp
// 新增接口
class IAsyncCallSDK {
public:
  virtual ~IAsyncCallSDK() = default;

  virtual bool AsyncCall(const std::string& key,
                         std::function<void()> async_func) = 0;

  virtual bool SendMsg(const std::string& session_id,
                       const std::string& dialogue_id,
                       const std::string& target_task_id,
                       const std::string& target_node_id,
                       const std::string& key,
                       const std::string& msg) = 0;

  virtual bool SendToSelf(const std::string& key, const std::string& msg) = 0;
};
```

#### 4. 异步回调消息结构

**消息格式**：回调消息使用`MMAgentMsgQue::Message`结构，通过metadata字段携带路由信息。

```cpp
// 回调消息结构
struct AsyncCallbackMessage {
  // Message标准字段
  std::string message_id;    // "async-callback-{uuid}"
  std::string content;       // 回调内容（chunk数据）
  std::string receiver;      // 目标node_id

  // Metadata路由字段
  std::string async_call_key;     // 调用标识（node设置的key）
  std::string execution_id;       // workflow执行ID
  std::string session_id;         // 会话ID
  std::string dialogue_id;        // 对话ID
  std::string task_id;            // 任务ID
  std::string node_id;            // 目标node ID
  std::string message_type;       // "async_callback"
};
```

**消息入队**：
```cpp
// SendToSelf实现
bool SendToSelf(const std::string& key, const std::string& msg) {
  MMAgentMsgQue::Message message;
  message.message_id = "async-callback-" + generate_uuid();
  message.content = msg;
  message.receiver = node_id_;

  // 路由元数据
  message.metadata["async_call_key"] = key;
  message.metadata["execution_id"] = execution_id_;
  message.metadata["session_id"] = session_id_;
  message.metadata["dialogue_id"] = dialogue_id_;
  message.metadata["task_id"] = task_id_;
  message.metadata["node_id"] = node_id_;
  message.metadata["message_type"] = "async_callback";

  // 直接入队到目标task
  return runtime_->enqueue_async_callback(message);
}
```

#### 5. 回调消息路由机制（短路路由）

**设计目标**：回调消息直接触发目标node执行，不走workflow的node调度链条。

```
正常workflow调度：
  workflow.schedule() -> select_ready_nodes() -> enqueue_node_task() -> process_node()

回调消息短路路由：
  task_queue收到async_callback消息 -> 直接触发目标node重新执行 -> process_node()
```

**路由流程**：

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         短路路由流程                                      │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  async_thread                                                            │
│      │                                                                   │
│      │ SendToSelf(key, chunk)                                            │
│      ▼                                                                   │
│  ┌───────────────────┐                                                   │
│  │ 构造回调消息       │                                                   │
│  │ - message_type    │                                                   │
│  │ - async_call_key  │                                                   │
│  │ - execution_id    │                                                   │
│  │ - node_id         │                                                   │
│  └─────────┬─────────┘                                                   │
│            │                                                             │
│            ▼                                                             │
│  ┌───────────────────┐                                                   │
│  │ Task消息队列       │ ◄── 跳过workflow调度，直接入队                     │
│  │ data_exchange_q   │                                                   │
│  └─────────┬─────────┘                                                   │
│            │                                                             │
│            │ agent_task_poller_loop() 检测到消息                          │
│            ▼                                                             │
│  ┌───────────────────────────────────────────────────────────────┐       │
│  │ runtime.enqueue_async_node_execution()                         │       │
│  │ 1. 从metadata提取execution_id, node_id                         │       │
│  │ 2. 查找WorkflowExecutionState                                  │       │
│  │ 3. 验证node处于ASYNC_PENDING状态                               │       │
│  │ 4. 直接将node加入执行队列（不经过workflow调度）                  │       │
│  └─────────┬─────────────────────────────────────────────────────┘       │
│            │                                                             │
│            ▼                                                             │
│  ┌───────────────────┐                                                   │
│  │ worker_thread     │                                                   │
│  │ process_node()    │                                                   │
│  └───────────────────┘                                                   │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

**关键实现点**：

1. **消息识别**：通过`message.metadata["message_type"] == "async_callback"`识别
2. **目标定位**：通过`execution_id`和`node_id`定位目标node
3. **状态验证**：确认node处于`ASYNC_PENDING`状态才触发重新执行
4. **直接入队**：跳过`workflow.schedule()`，直接构造`QueuedNodeTask`入队

```cpp
// runtime.cpp - agent_task_poller_loop()
void MMAgentRuntime::agent_task_poller_loop() {
  while (is_running_) {
    // ... 检查task消息队列 ...
    auto msg = task->dequeue_data_exchange_message();

    // 检查是否为异步回调消息
    if (msg.metadata.count("message_type") &&
        msg.metadata["message_type"] == "async_callback") {
      handle_async_callback(msg);
      continue;  // 不走正常消息处理流程
    }

    // ... 正常消息处理 ...
  }
}

void MMAgentRuntime::handle_async_callback(const Message& msg) {
  auto execution_id = msg.metadata.at("execution_id");
  auto node_id = msg.metadata.at("node_id");

  std::lock_guard<std::mutex> lock(workflow_state_mutex_);
  auto it = workflow_states_.find(execution_id);
  if (it == workflow_states_.end()) return;

  auto& state = it->second;
  auto node_it = cloned_workflows_[execution_id]->get_node(node_id);
  if (!node_it) return;

  // 验证node状态
  if (node_it->get_state() != NodeState::ASYNC_PENDING) return;

  // 存储回调数据到databus（供node读取）
  auto& databus = get_databus(state.agent_id);
  std::string callback_key = "/async_callback/" + node_id + "/" + msg.metadata["async_call_key"];
  databus.put(callback_key + "/msg", msg.content);
  databus.put(callback_key + "/key", msg.metadata["async_call_key"]);

  // 直接触发node重新执行
  QueuedNodeTask task;
  task.agent = get_agent(state.agent_id);
  task.workflow = cloned_workflows_[execution_id];
  task.node = node_it;
  task.execution_id = execution_id;

  {
    std::lock_guard<std::mutex> queue_lock(task_queue_mutex_);
    node_task_queue_.push(task);
  }
}
```

#### 6. Node回调入口设计

**设计目标**：node能识别这是回调调用，并获取回调的key和消息内容。

**方案**：通过`inputs`参数注入回调信息。

```cpp
// process_node() 注入回调信息
void MMAgentRuntime::process_node(AgentPtr agent, WorkflowPtr workflow,
                                   NodePtr node, const std::string& execution_id) {
  DataMap inputs;
  DataMap outputs;

  // ... 正常inputs填充 ...

  // 检查是否为异步回调触发
  if (node->get_state() == NodeState::ASYNC_PENDING) {
    auto& databus = get_databus(agent->get_id());
    std::string callback_prefix = "/async_callback/" + node->get_id();

    SoDataVariant key, msg;
    if (databus.get(callback_prefix + "/key", key) &&
        databus.get(callback_prefix + "/msg", msg)) {
      // 注入回调信息到inputs
      inputs["__async_callback__"] = true;
      inputs["__async_key__"] = std::get<std::string>(key);
      inputs["__async_msg__"] = std::get<std::string>(msg);

      // 清理回调数据
      databus.remove(callback_prefix + "/key");
      databus.remove(callback_prefix + "/msg");
    }
  }

  // 执行node
  node->set_state(NodeState::EXECUTING);
  bool success = node->execute(inputs, outputs, databus_client.get());

  // 处理异步pending状态
  if (outputs.count("__async_pending__") &&
      std::holds_alternative<bool>(outputs["__async_pending__"]) &&
      std::get<bool>(outputs["__async_pending__"])) {
    node->set_state(NodeState::ASYNC_PENDING);
    // 保存call_key用于后续回调匹配
    node->set_async_call_key(std::get<std::string>(outputs["call_key"]));
    return;  // 不标记为完成
  }

  // ... 正常完成处理 ...
}
```

**Node处理回调示例**：

```cpp
// Thread-local storage for accumulating response across callbacks
static thread_local std::string t_accumulated_response;

extern "C" void skill_node_v3(const DataMap& inputs, DataMap& outputs,
                               INodeDataBusClient* client) {
  IAsyncCallSDK* async_sdk = client ? client->async() : nullptr;
  if (!async_sdk) {
    outputs["status"] = std::string("error");
    outputs["error"] = std::string("Async SDK not available");
    return;
  }

  // ===== 1. Check if this is a callback invocation =====
  bool is_callback = false;
  auto cb_it = inputs.find("__async_callback__");
  if (cb_it != inputs.end()) {
    if (auto* b = std::get_if<bool>(&cb_it->second)) is_callback = *b;
  }

  if (is_callback) {
    std::string callback_key, callback_msg;
    auto key_it = inputs.find("__async_key__");
    auto msg_it = inputs.find("__async_msg__");
    if (key_it != inputs.end()) {
      if (auto* s = std::get_if<std::string>(&key_it->second)) callback_key = *s;
    }
    if (msg_it != inputs.end()) {
      if (auto* s = std::get_if<std::string>(&msg_it->second)) callback_msg = *s;
    }

    if (callback_key == "llm-stream") {
      if (callback_msg == "[DONE]") {
        // Stream complete
        outputs["status"] = std::string("success");
        outputs["response"] = t_accumulated_response;
        t_accumulated_response.clear();
        // Don't set __async_pending__ - workflow will continue
        return;
      } else {
        // Accumulate chunk, continue waiting
        t_accumulated_response += callback_msg;
        outputs["__async_pending__"] = true;
        outputs["call_key"] = callback_key;
        return;
      }
    }
    return;
  }

  // ===== 2. Initial invocation - start async call =====
  t_accumulated_response.clear();
  async_sdk->AsyncCall("llm-stream", [async_sdk]() {
    // In async thread: perform operation and send chunks
    async_sdk->SendToSelf("llm-stream", chunk);
    async_sdk->SendToSelf("llm-stream", "[DONE]");
  });

  outputs["__async_pending__"] = true;
  outputs["call_key"] = std::string("llm-stream");
}
```

**inputs字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `__async_callback__` | bool | true表示这是回调调用 |
| `__async_key__` | string | 回调的key（node在AsyncCall时设置） |
| `__async_msg__` | string | 回调消息内容（chunk或[DONE]） |

#### 7. Skill Node向Coordinator透传Chunk机制

**机制说明**：利用已有的消息定向路由机制（Enhancement 024），skill node可通过`SendMsg()`将每个chunk直接发送给coordinator。

**前提条件**：
- Coordinator task_id采用约定命名：`{session_id}_coordinator`
- Skill node通过`client->async()`获取的上下文信息

**数据流**：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    Skill Node 透传 Chunk 到 Coordinator                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  skill_node                                                                  │
│      │                                                                       │
│      │ 1. AsyncCall("llm-query", [&]() {                                    │
│      │      llm.stream_prompt(...,                                          │
│      │          on_chunk = [&](chunk) {                                     │
│      │              // 发送给自己处理                                        │
│      │              SendToSelf("llm-query", chunk);                          │
│      │                                                                       │
│      │              // 透传给coordinator                                     │
│      │              SendMsg(session_id, dialogue_id,                         │
│      │                    coordinator_task_id, "coordinator",                │
│      │                    "stream-chunk", chunk);                            │
│      │          });                                                          │
│      │    })                                                                 │
│      │                                                                       │
│      ▼                                                                       │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │ SendMsg 实现细节                                                     │    │
│  │                                                                      │    │
│  │ bool SendMsg(session_id, dialogue_id, target_task_id,               │    │
│  │             target_node_id, key, msg) {                              │    │
│  │   MMAgentMsgQue::Message message;                                    │    │
│  │   message.message_id = "stream-chunk-" + uuid();                     │    │
│  │   message.content = msg;  // chunk内容                               │    │
│  │   message.receiver = target_node_id;  // "coordinator"               │    │
│  │   message.metadata["session_id"] = session_id;                       │    │
│  │   message.metadata["dialogue_id"] = dialogue_id;                     │    │
│  │   message.metadata["task_id"] = target_task_id;                      │    │
│  │   message.metadata["stream_key"] = key;  // "llm-query"              │    │
│  │   message.metadata["message_type"] = "stream_chunk";                 │    │
│  │   return runtime_->send_routed_message(message);                     │    │
│  │ }                                                                    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│      │                                                                       │
│      │ 消息定向路由（Enhancement 024）                                       │
│      ▼                                                                       │
│  ┌───────────────────┐                                                       │
│  │ Coordinator Task  │ ◄── 直接入队到coordinator的data_exchange_queue       │
│  │ Message Queue     │                                                       │
│  └─────────┬─────────┘                                                       │
│            │                                                                 │
│            ▼                                                                 │
│  ┌───────────────────┐                                                       │
│  │ coordinator_node  │                                                       │
│  │ 接收chunk消息      │                                                       │
│  └───────────────────┘                                                       │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

**完整示例：Skill Node异步LLM调用 + 透传Coordinator**

```cpp
// llm_node.cpp - 异步LLM调用示例
#include "agentflow/so_node.h"

using DataMap = agentflow::DataMap;
using INodeDataBusClient = agentflow::INodeDataBusClient;
using IAsyncCallSDK = agentflow::IAsyncCallSDK;
using SoDataVariant = agentflow::SoDataVariant;

// Thread-local storage for accumulating response across callbacks
static thread_local std::string t_accumulated_response;

static std::string get_string_from_map(const DataMap& map, const std::string& key, const std::string& default_val = "") {
    auto it = map.find(key);
    if (it != map.end()) {
        if (auto* s = std::get_if<std::string>(&it->second)) return *s;
    }
    return default_val;
}

extern "C" void llm_query_node_async_v3(const DataMap& inputs, DataMap& outputs, INodeDataBusClient* client) {
    IAsyncCallSDK* async_sdk = client ? client->async() : nullptr;
    if (!async_sdk) {
        outputs["status"] = std::string("error");
        outputs["error"] = std::string("Async SDK not available");
        return;
    }

    // ===== 1. Check if this is a callback invocation =====
    bool is_callback = false;
    auto cb_it = inputs.find("__async_callback__");
    if (cb_it != inputs.end()) {
        if (auto* b = std::get_if<bool>(&cb_it->second)) is_callback = *b;
    }

    if (is_callback) {
        std::string callback_key = get_string_from_map(inputs, "__async_key__");
        std::string callback_msg = get_string_from_map(inputs, "__async_msg__");

        if (callback_key == "llm-stream") {
            if (callback_msg == "[DONE]") {
                // Stream complete
                outputs["status"] = std::string("success");
                outputs["response"] = t_accumulated_response;

                // Store in sys namespace with dialogue isolation
                if (client && client->sys()) {
                    std::string dialogue_id = get_string_from_map(inputs, "dialogue_id");
                    std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
                    client->sys()->put(response_key, SoDataVariant(t_accumulated_response));
                }

                // Forward [DONE] to coordinator
                std::string session_id = get_string_from_map(inputs, "session_id");
                std::string dialogue_id = get_string_from_map(inputs, "dialogue_id");
                if (!session_id.empty()) {
                    client->async()->SendMsg(session_id, dialogue_id,
                        session_id + "_coordinator", "coordinator", "llm-stream", "[DONE]");
                }

                t_accumulated_response.clear();
                return;
            } else {
                // Accumulate chunk
                t_accumulated_response += callback_msg;

                // Forward chunk to coordinator
                std::string session_id = get_string_from_map(inputs, "session_id");
                std::string dialogue_id = get_string_from_map(inputs, "dialogue_id");
                if (!session_id.empty()) {
                    client->async()->SendMsg(session_id, dialogue_id,
                        session_id + "_coordinator", "coordinator", "llm-stream", callback_msg);
                }

                outputs["__async_pending__"] = true;
                outputs["call_key"] = callback_key;
                return;
            }
        }
        return;
    }

    // ===== 2. Initial invocation - start async call =====
    t_accumulated_response.clear();
    const std::string call_key = "llm-stream";

    // Capture variables needed in async thread
    IAsyncCallSDK* captured_async_sdk = async_sdk;

    async_sdk->AsyncCall(call_key, [captured_async_sdk]() {
        // Async thread: perform external operation
        // Send chunks back to self via SendToSelf
        for (const auto& chunk : get_streaming_chunks()) {
            captured_async_sdk->SendToSelf("llm-stream", chunk);
        }
        captured_async_sdk->SendToSelf("llm-stream", "[DONE]");
    });

    outputs["__async_pending__"] = true;
    outputs["call_key"] = call_key;
    outputs["status"] = std::string("streaming");
}
```

**Coordinator Node接收流式消息**：

```cpp
// coordinator_node.cpp - 接收skill透传的chunk
extern "C" void coordinator_node_v3(const DataMap& inputs, DataMap& outputs,
                                     INodeDataBusClient* client) {

  // Helper to get string from DataMap
  auto get_string = [&inputs](const std::string& key, const std::string& default_val = "") -> std::string {
    auto it = inputs.find(key);
    if (it != inputs.end()) {
      if (auto* s = std::get_if<std::string>(&it->second)) {
        return *s;
      }
    }
    return default_val;
  };

  // Check for stream key in inputs (sent via SendMsg)
  std::string stream_key = get_string("stream_key", "");
  std::string chunk = get_string("content", "");  // Message content from SendMsg

  if (!stream_key.empty() && stream_key == "llm-stream") {
    if (chunk == "[DONE]") {
      // Stream complete - send final response to client
      SoDataVariant buffer;
      if (client->user()->get("stream_buffer", buffer)) {
        if (auto* text = std::get_if<std::string>(&buffer)) {
          outputs["final_response"] = *text;
        }
      }
      outputs["stream_status"] = std::string("completed");
    } else if (chunk.find("[ERROR]") == 0) {
      // Error occurred
      outputs["stream_status"] = std::string("error");
      outputs["error"] = chunk.substr(7);  // Remove "[ERROR]" prefix
    } else {
      // Accumulate chunk
      SoDataVariant buffer;
      std::string current = "";
      if (client->user()->get("stream_buffer", buffer)) {
        if (auto* text = std::get_if<std::string>(&buffer)) {
          current = *text;
        }
      }
      client->user()->put("stream_buffer", SoDataVariant(current + chunk));

      // Real-time output to user (optional)
      outputs["stream_chunk"] = chunk;
      outputs["stream_status"] = std::string("streaming");
    }
    return;
  }

  // Normal coordinator logic...
}
```

**关键设计点**：

| 设计点 | 说明 |
|--------|------|
| Coordinator Task ID | 采用约定命名`{session_id}_coordinator`，skill可直接构造 |
| 消息类型 | `message_type = "stream_chunk"`，coordinator据此识别 |
| 流程标记 | `[START]` / chunk / `[DONE]` 三阶段标记 |
| 数据隔离 | 使用`dialogue_id`确保多对话场景的chunk不会混淆 |

**Skill Node Output样例（每次收到chunk回调时）**：

```cpp
// ===== 回调时 outputs 示例 =====
// 当 skill_node 收到一个 chunk 回调时，典型的 outputs 内容：

outputs = {
    // 必需：标记继续异步等待
    "__async_pending__": true,
    "call_key": "llm-query",

    // 可选：当前chunk内容（用于调试或本地累积）
    "current_chunk": "这是LLM返回的一个文本片段",

    // 可选：已累积的响应（如果需要在本地维护状态）
    "accumulated_response": "这是LLM返回的一个文本片段...之前累积的内容",

    // 注意：透传给 coordinator 是通过 SendMsg() 完成，不走 outputs
};

// ===== 透传给 coordinator 的消息结构 =====
// 通过 SendMsg() 发送的消息：
{
    "message_id": "stream-chunk-uuid-xxx",
    "content": "这是LLM返回的一个文本片段",  // chunk内容
    "receiver": "coordinator",
    "metadata": {
        "session_id": "session-123",
        "dialogue_id": "dialogue-456",
        "task_id": "session-123_coordinator",
        "stream_key": "llm-stream",
        "message_type": "stream_chunk"
    }
}
```

**完整数据流示例**：

```
LLM Service
    │
    │ stream: ["你", "好", "，", "我", "是", "AI", "助手", "[DONE]"]
    ▼
async_thread (skill_node内部)
    │
    │ SendToSelf("llm-query", chunk) × 8次
    ▼
skill_node (回调入口)
    │
    ├── outputs["__async_pending__"] = true
    ├── outputs["current_chunk"] = "你"
    │
    └── SendMsg(..., "你") ──────────────────────┐
                                                  │
    (重复7次，每个chunk触发一次回调)              │
                                                  ▼
                              coordinator_task.data_exchange_queue
                                                  │
                                                  ▼
                              coordinator_node
                                  │
                                  ├── 收到 "你": outputs["stream_chunk"] = "你"
                                  ├── 收到 "好": outputs["stream_chunk"] = "好"
                                  ├── ... (累积每个chunk)
                                  └── 收到 "[DONE]": outputs["final_response"] = "你好，我是AI助手"
```

### 流程设计

#### 异步调用流程时序图

```
skill_node           runtime            async_thread         task_queue
    │                   │                    │                   │
    │ execute()         │                    │                   │
    ├──────────────────>│                    │                   │
    │                   │                    │                   │
    │ AsyncCall("llm")  │                    │                   │
    ├──────────────────>│                    │                   │
    │                   │ spawn thread       │                   │
    │                   ├──────────────────->│                   │
    │                   │                    │                   │
    │                   │ increment_async_pending()              │
    │                   ├────────────────────┼──────────────────>│
    │                   │                    │                   │
    │ return false      │                    │                   │
    │ (ASYNC_PENDING)   │                    │                   │
    │<──────────────────┤                    │                   │
    │                   │                    │                   │
    │                   │                    │ LLM request       │
    │                   │                    ├──────────────────>│
    │                   │                    │                   │
    │                   │                    │ on_chunk(chunk)   │
    │                   │                    ├──────────────────>│ SendToSelf
    │                   │                    │                   │
    │                   │                    │                   │ enqueue
    │                   │<───────────────────┼───────────────────┤
    │                   │                    │                   │
    │ process_callback  │                    │                   │
    ├──────────────────>│                    │                   │
    │                   │                    │                   │
    │ decrement_async_pending()              │                   │
    │                   ├────────────────────┼──────────────────>│
    │                   │                    │                   │
    │ return true       │                    │                   │
    │ (COMPLETED)       │                    │                   │
    │<──────────────────┤                    │                   │
```

### 接口设计

#### 1. C++ SO Node V3签名

```cpp
extern "C" void skill_node_v3(const DataMap& inputs, DataMap& outputs,
                              INodeDataBusClient* client) {
  // 发起异步LLM调用
  client->async()->AsyncCall("llm-query", [&]() {
    StreamingLLMClient llm;
    llm.stream_prompt(prompt, config,
      on_chunk = [&](const std::string& chunk) {
        client->async()->SendToSelf("llm-query", chunk);
      },
      on_finish = [&](const std::string&) {
        client->async()->SendToSelf("llm-query", "[DONE]");
      }
    );
  });

  // 设置异步等待状态
  outputs["__async_pending__"] = true;
  outputs["call_key"] = "llm-query";
}
```

#### 2. Python Node签名

```python
def skill_node(inputs: dict, outputs: dict, client: NodeClient) -> None:
    async_sdk = client.async_call()

    def async_llm():
        for chunk in llm_stream(prompt):
            async_sdk.send_to_self("llm-query", chunk)
        async_sdk.send_to_self("llm-query", "[DONE]")

    threading.Thread(target=async_llm, daemon=True).start()
    outputs["__async_pending__"] = True
    outputs["call_key"] = "llm-query"
```

#### 3. Shell Node签名

```bash
#!/bin/bash
source vendor/shell/agentflow_async.sh

# 后台异步执行
(
    curl -sS -N "$AF_LLM_BASE_URL/chat/completions" | while read line; do
        send_to_self "llm-query" "$chunk"
    done
    send_to_self "llm-query" "[DONE]"
) &

async_pending "llm-query"
```

### 实现方案

#### Phase 1: 核心接口定义（无副作用）

**文件**: `include/external/agentflow/so_node.h`

```cpp
// 新增 IAsyncCallSDK 接口定义
class IAsyncCallSDK {
public:
  virtual ~IAsyncCallSDK() = default;
  virtual bool AsyncCall(const std::string& key, std::function<void()> async_func) = 0;
  virtual bool SendMsg(const std::string& session_id, const std::string& dialogue_id,
                       const std::string& target_task_id, const std::string& target_node_id,
                       const std::string& key, const std::string& msg) = 0;
  virtual bool SendToSelf(const std::string& key, const std::string& msg) = 0;
};

// 扩展 INodeDataBusClient
class INodeDataBusClient {
  // ... existing methods ...
  virtual IAsyncCallSDK* async() = 0;  // 新增
};
```

**文件**: `include/internal/workflow/node.h`

```cpp
enum class NodeState {
  // ... existing states ...
  ASYNC_PENDING = 5  // 新增：异步等待状态
};
```

#### Phase 2: 生命周期管理

**文件**: `include/internal/runtime/agent_runtime.h`

```cpp
struct WorkflowExecutionState {
  // ... existing fields ...
  std::atomic<int> async_pending_count{0};

  bool has_async_pending() const { return async_pending_count.load() > 0; }
  void increment_async_pending() { async_pending_count++; }
  void decrement_async_pending() {
    if(async_pending_count > 0) async_pending_count--;
  }
};
```

**文件**: `src/runtime/runtime.cpp`

```cpp
// update_workflow_state() 修改
bool can_complete = (state.completed_count + state.failed_nodes.size() >= state.total_nodes)
                    && !state.has_async_pending();
```

#### Phase 3: SDK实现

**文件**: `src/runtime/async_call.cpp` (新增)

```cpp
class NodeAsyncCallSDK : public IAsyncCallSDK {
public:
  NodeAsyncCallSDK(MMAgentRuntime* runtime, const std::string& execution_id,
                   const std::string& session_id, const std::string& dialogue_id,
                   const std::string& task_id, const std::string& node_id);

  bool AsyncCall(const std::string& key, std::function<void()> async_func) override;
  bool SendMsg(...) override;
  bool SendToSelf(const std::string& key, const std::string& msg) override;

private:
  MMAgentRuntime* runtime_;
  std::string execution_id_, session_id_, dialogue_id_, task_id_, node_id_;
};
```

### 待修改文件清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `include/external/agentflow/so_node.h` | 修改 | 新增IAsyncCallSDK接口 |
| `include/internal/runtime/agent_runtime.h` | 修改 | WorkflowExecutionState新增async_pending_count |
| `include/internal/agent/common.h` | 修改 | 新增ASYNC_PENDING状态 |
| `include/internal/workflow/node.h` | 修改 | 新增async_call_key_成员和setter/getter |
| `src/runtime/async_call.cpp` | 新增 | NodeAsyncCallSDK实现 |
| `src/runtime/databus/node_databus_client.cpp` | 修改 | 实现async()方法 |
| `src/runtime/runtime.cpp` | 修改 | 注入SDK、处理异步状态、handle_async_callback()、短路路由 |
| `src/runtime/worker/worker.cpp` | 修改 | 注入AF_ASYNC_*环境变量 |

### 功能点实现优先级

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | IAsyncCallSDK接口定义 | 无副作用 | 编译检查 |
| P1 | NodeState::ASYNC_PENDING | 无副作用 | 编译检查 |
| P2 | async_pending_count | 无副作用，新增字段 | 单元测试 |
| P2 | workflow完成判断修改 | 有依赖，需兼容 | 单元测试 |
| P3 | NodeAsyncCallSDK实现 | 有依赖 | 单元测试+集成测试 |
| P3 | process_node注入SDK | 有影响 | 集成测试 |
| P4 | async_callback消息处理 | 有影响 | E2E测试 |
| P4 | 多语言SDK | 有依赖 | 集成测试 |

### 测试方案

#### 单元测试

```cpp
TEST(AsyncCallTest, PendingCountIncrementDecrement) {
  WorkflowExecutionState state;
  EXPECT_FALSE(state.has_async_pending());
  state.increment_async_pending();
  EXPECT_TRUE(state.has_async_pending());
  state.decrement_async_pending();
  EXPECT_FALSE(state.has_async_pending());
}

TEST(AsyncCallTest, WorkflowWaitsForAsync) {
  // 验证workflow不会在async_pending时完成
}

TEST(AsyncCallTest, SendToSelfRouting) {
  // 验证SendToSelf消息能到达task队列
}
```

#### 集成测试

在`examples/agent/system_agent_mock`中新增`async_skill_node.cpp`:
- 模拟异步LLM流式响应
- 验证chunk回调流程

#### E2E测试

修改`tests/e2e/system_agent_mock/test_multi_session_concurrent.cpp`:
- 新增异步场景测试用例

### 文档

| 文档 | 变更说明 |
|------|---------|
| [NODE_DEVELOPMENT_GUIDE.md](../../publish/workflow/NODE_DEVELOPMENT_GUIDE.md) | 新增异步调用章节 |
| [NODE_API_REFERENCE.md](../../publish/workflow/NODE_API_REFERENCE.md) | 新增async() API参考 |
| [Python SDK README.md](../../src/runtime/databus/clients/python/README.md) | 新增Python异步SDK说明 |

### 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [024-feature-消息定向路由机制设计](024-feature-消息定向路由机制设计-260319-implemented.md) | SendToSelf基于定向路由实现 |
| [028-feature-node-模型推理服务流式访问设计](028-feature-node-模型推理服务流式访问设计-260323-tbd.md) | 异步LLM调用的应用场景 |

### 进展跟踪

- [x] 代码实现
  - [x] Phase 1: 接口定义 (IAsyncCallSDK, NodeState::ASYNC_PENDING)
  - [x] Phase 2: 生命周期管理 (async_pending_count)
  - [x] Phase 3: SDK实现 (NodeAsyncCallSDK)
  - [x] Phase 4: 回调处理 (短路路由, inputs注入)
  - [ ] 多语言SDK (Python/Shell/Go - 待实现)
- [x] 单元测试 (编译通过)
- [x] 集成测试 (LLM Query Agent E2E测试通过)
- [x] 文档更新 (示例代码已同步)
- [x] 验证通过 (2026-03-26)

### 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-03-26 | xl4872 | 初始版本 |
| v1.1 | 2026-03-26 | xl4872 | 更新示例代码与实际实现一致，移除无关细节 |
| v1.2 | 2026-03-26 | xl4872 | 新增多语言SDK设计方案；标注环境变量注入状态，修正命名 |

---

## 多语言SDK设计（复用DataBus UDS）

### 背景

C++ SO节点的异步调用机制已实现，但Python/Shell/Go等多语言节点尚不支持异步回调。

**当前状况**：
- 多语言节点通过 `AF_DATABUS_UDS_FILE` 环境变量访问DataBus UDS服务
- UDS服务已实现JSON-RPC 2.0协议，支持 `put/get/subscribe/publish` 等方法
- 缺少异步回调机制，多语言节点无法实现非阻塞流式响应

**目标**：
复用现有DataBus UDS通道，为Python/Shell/Go节点提供异步回调能力。

### 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    Multi-Language Async SDK Architecture                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐              │
│  │ Python Node     │  │ Shell Node      │  │ Go Node         │              │
│  │                 │  │                 │  │                 │              │
│  │ AsyncCallSDK    │  │ async_call.sh   │  │ AsyncCallSDK    │              │
│  │  - send_to_self │  │  - send_to_self │  │  - SendToSelf   │              │
│  │  - send_msg     │  │  - send_msg     │  │  - SendMsg      │              │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘              │
│           │                    │                    │                        │
│           └────────────────────┼────────────────────┘                        │
│                                │                                             │
│                                ▼                                             │
│           ┌──────────────────────────────────────┐                          │
│           │        UDS JSON-RPC 2.0              │                          │
│           │    (AF_DATABUS_UDS_FILE)             │                          │
│           │                                      │                          │
│           │    Methods:                          │                          │
│           │    - put/get/remove/contains         │                          │
│           │    - subscribe/unsubscribe           │                          │
│           │    - publish                         │                          │
│           │    - async_callback (NEW)            │◄── 扩展方法              │
│           └──────────────┬───────────────────────┘                          │
│                          │                                                   │
│                          ▼                                                   │
│           ┌──────────────────────────────────────┐                          │
│           │         MMAgentRuntime               │                          │
│           │    - handle_async_callback()         │                          │
│           │    - 触发节点重新执行                 │                          │
│           └──────────────────────────────────────┘                          │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### UDS协议扩展

#### 新增方法：`async_callback`

**用途**：异步回调消息路由，将消息发送到目标节点的Task队列。

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "async_callback",
  "params": {
    "session_id": "<session_id>",
    "dialogue_id": "<dialogue_id>",
    "task_id": "<task_id>",
    "node_id": "<node_id>",
    "call_key": "<call_key>",
    "content": "<message_content>"
  },
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "queued": true
  },
  "id": 1
}
```

**参数说明**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `session_id` | string | 是 | Session ID，从环境变量获取 |
| `dialogue_id` | string | 是 | Dialogue ID，从环境变量获取 |
| `task_id` | string | 是 | Task ID，从环境变量获取 |
| `node_id` | string | 是 | Node ID，从环境变量获取 |
| `call_key` | string | 是 | 异步调用标识，node设置 |
| `content` | string | 是 | 回调消息内容 |

### 环境变量注入

Worker在执行多语言节点时注入环境变量，分两类：

#### 已有注入 - 直接注入（worker.cpp）

| 变量名 | 说明 | 来源 |
|--------|------|------|
| `AF_DATABUS_DIR` | DataBus文件根目录 | worker.cpp:54 |
| `AF_DATABUS_KV_DIR` | DataBus KV目录 | worker.cpp:55 |
| `AF_DATABUS_EVENTS_DIR` | DataBus事件目录 | worker.cpp:56 |
| `AF_LLM_BASE_URL` | LLM服务地址 | worker.cpp:58 |
| `AF_LLM_API_KEY` | LLM API密钥 | worker.cpp:61 |
| `AF_LLM_MODEL` | LLM模型名称 | worker.cpp:64 |
| `AF_PLUGIN_UDS_FILE` | 插件UDS路径 | worker.cpp:70 |
| `AF_DATABUS_UDS_FILE` | DataBus UDS路径 | worker.cpp:81 |

#### 已有注入 - inputs自动转换（worker.cpp:91-95）

Worker 会将 `request.inputs` 中的所有字段自动转换为环境变量 `AF_<KEY>`：

| 变量名 | 说明 | inputs来源 |
|--------|------|------------|
| `AF_SESSION_ID` | 当前Session ID | inputs["session_id"] (runtime.cpp:1713) |
| `AF_DIALOGUE_ID` | 当前Dialogue ID | inputs["dialogue_id"] (runtime.cpp:1718) |
| `AF_SELF_TASK_ID` | 当前Task ID | inputs["self_task_id"] (runtime.cpp:1728) |

**转换逻辑**（worker.cpp:91-95）：
```cpp
for(const auto& pair : request.inputs) {
    std::string env_key = "AF_" + to_env_suffix(pair.first);
    exec_config.env_vars.push_back(env_key + "=" + data_variant_to_string(pair.second));
}
```

#### 新增注入（需修改代码）

| 变量名 | 说明 | 修改位置 |
|--------|------|----------|
| `AF_NODE_ID` | 当前Node ID | runtime.cpp: 注入到inputs；worker.cpp: 自动转换 |

**修改方案**：

在 `src/runtime/runtime.cpp` 中注入 `node_id` 到 inputs：

```cpp
// 在 inputs 注入部分（约1730行附近）新增：
inputs["node_id"] = node->get_id();  // 当前执行节点ID
```

Worker 会自动将其转换为 `AF_NODE_ID` 环境变量。

### Python SDK实现

**文件**: `src/runtime/databus/clients/python/agentflow_databus.py`

```python
class AsyncCallSDK:
    """Async call SDK for Python nodes using DataBus UDS channel."""

    def __init__(self, client: 'DataBusClient'):
        self._client = client
        self._session_id = os.environ.get("AF_SESSION_ID", "")
        self._dialogue_id = os.environ.get("AF_DIALOGUE_ID", "")
        self._task_id = os.environ.get("AF_SELF_TASK_ID", "")  # 注意：使用 AF_SELF_TASK_ID
        self._node_id = os.environ.get("AF_NODE_ID", "")

    def send_to_self(self, call_key: str, content: str) -> bool:
        """Send a callback message to the current node."""
        params = {
            "session_id": self._session_id,
            "dialogue_id": self._dialogue_id,
            "task_id": self._task_id,
            "node_id": self._node_id,
            "call_key": call_key,
            "content": content
        }
        result = self._client._call("async_callback", params)
        return result.get("success", False)

    def send_msg(self, target_task_id: str, target_node_id: str,
                 call_key: str, content: str) -> bool:
        """Send a message to another node."""
        params = {
            "session_id": self._session_id,
            "dialogue_id": self._dialogue_id,
            "task_id": target_task_id,
            "node_id": target_node_id,
            "call_key": call_key,
            "content": content
        }
        result = self._client._call("async_callback", params)
        return result.get("success", False)
```

**Python Node使用示例**：

```python
#!/usr/bin/env python3
"""LLM Streaming Node - Python with async callbacks"""

import json
import os
import httpx
from agentflow_databus import DataBusClient

_accumulated_response = ""

def llm_stream_node(inputs: dict, outputs: dict, client: DataBusClient):
    global _accumulated_response
    async_sdk = client.async_sdk()

    # 1. Check if callback
    is_callback = inputs.get("__async_callback__", "false") == "true"

    if is_callback:
        callback_key = inputs.get("__async_key__", "")
        callback_msg = inputs.get("__async_msg__", "")

        if callback_key == "llm-stream":
            if callback_msg == "[DONE]":
                outputs["status"] = "success"
                outputs["response"] = _accumulated_response
                _accumulated_response = ""
            else:
                _accumulated_response += callback_msg
                outputs["__async_pending__"] = "true"
                outputs["call_key"] = callback_key
        return

    # 2. Initial invocation - start async LLM call
    api_key = client.sys().get("api_key", "")
    prompt = inputs.get("prompt") or inputs.get("input", "")

    def async_llm_call():
        with httpx.Client() as http:
            with http.stream("POST", llm_url, json=request) as resp:
                for line in resp.iter_lines():
                    if line.startswith("data: "):
                        chunk = parse_chunk(line)
                        if chunk:
                            async_sdk.send_to_self("llm-stream", chunk)
                async_sdk.send_to_self("llm-stream", "[DONE]")

    import threading
    threading.Thread(target=async_llm_call, daemon=True).start()

    outputs["__async_pending__"] = "true"
    outputs["call_key"] = "llm-stream"
```

### Shell SDK实现

**文件**: `src/runtime/databus/clients/shell/agentflow_async.sh`

```bash
#!/bin/bash
# AgentFlow Async Call SDK for Shell nodes

# Send callback message to current node
# Usage: send_to_self <call_key> <content>
send_to_self() {
    local call_key="$1"
    local content="$2"
    local uds_path="${AF_DATABUS_UDS_FILE}"

    local params="{\"session_id\":\"${AF_SESSION_ID}\",\"dialogue_id\":\"${AF_DIALOGUE_ID}\",\"task_id\":\"${AF_SELF_TASK_ID}\",\"node_id\":\"${AF_NODE_ID}\",\"call_key\":\"${call_key}\",\"content\":\"${content}\"}"

    local request="{\"jsonrpc\":\"2.0\",\"method\":\"async_callback\",\"params\":${params},\"id\":$$}"
    echo "$request" | nc -U "$uds_path"
}

# Output async pending state
# Usage: async_pending <call_key>
async_pending() {
    echo "__async_pending__=true"
    echo "call_key=$1"
}

# Check if callback
is_async_callback() {
    [[ "${__async_callback__}" == "true" ]]
}
```

**Shell Node使用示例**：

```bash
#!/bin/bash
source "${AF_SDK_DIR}/agentflow_async.sh"

ACCUMULATOR="/tmp/llm_acc_${AF_SELF_TASK_ID}.txt"

if is_async_callback; then
    if [[ "$CALLBACK_MSG" == "[DONE]" ]]; then
        echo "status=success"
        echo "response=$(cat $ACCUMULATOR)"
        rm -f "$ACCUMULATOR"
    else
        echo -n "$CALLBACK_MSG" >> "$ACCUMULATOR"
        async_pending "llm-stream"
    fi
    exit 0
fi

# Start background LLM streaming
(
    curl -sS -N "$LLM_URL" | while read line; do
        send_to_self "llm-stream" "$chunk"
    done
    send_to_self "llm-stream" "[DONE]"
) &

async_pending "llm-stream"
```

### Go SDK实现

**文件**: `src/runtime/databus/clients/golang/async/async.go`

```go
package async

import (
	"encoding/json"
	"net"
	"os"
)

type AsyncCallSDK struct {
	sessionID  string
	dialogueID string
	taskID     string
	nodeID     string
	udsPath    string
}

func NewAsyncCallSDK() *AsyncCallSDK {
	return &AsyncCallSDK{
		sessionID:  os.Getenv("AF_SESSION_ID"),
		dialogueID: os.Getenv("AF_DIALOGUE_ID"),
		taskID:     os.Getenv("AF_SELF_TASK_ID"),  // 注意：使用 AF_SELF_TASK_ID
		nodeID:     os.Getenv("AF_NODE_ID"),
		udsPath:    os.Getenv("AF_DATABUS_UDS_FILE"),
	}
}

func (s *AsyncCallSDK) SendToSelf(callKey, content string) error {
	return s.sendAsyncCallback(s.taskID, s.nodeID, callKey, content)
}

func (s *AsyncCallSDK) SendMsg(targetTaskID, targetNodeID, callKey, content string) error {
	return s.sendAsyncCallback(targetTaskID, targetNodeID, callKey, content)
}

func (s *AsyncCallSDK) sendAsyncCallback(taskID, nodeID, callKey, content string) error {
	params := map[string]interface{}{
		"session_id":  s.sessionID,
		"dialogue_id": s.dialogueID,
		"task_id":     taskID,
		"node_id":     nodeID,
		"call_key":    callKey,
		"content":     content,
	}
	request := map[string]interface{}{
		"jsonrpc": "2.0",
		"method":  "async_callback",
		"params":  params,
		"id":      1,
	}

	conn, _ := net.Dial("unix", s.udsPath)
	defer conn.Close()
	json.NewEncoder(conn).Encode(request)
	return nil
}
```

### UDSServer实现

**文件**: `src/runtime/databus/uds_server.cpp`

```cpp
std::string UDSServer::handle_async_callback(const std::string& params, int64_t id) {
  // 1. 解析参数
  std::string session_id = extract_string_param(params, "session_id");
  std::string dialogue_id = extract_string_param(params, "dialogue_id");
  std::string task_id = extract_string_param(params, "task_id");
  std::string node_id = extract_string_param(params, "node_id");
  std::string call_key = extract_string_param(params, "call_key");
  std::string content = extract_string_param(params, "content");

  // 2. 构造回调消息
  MMAgentMsgQue::Message message;
  message.message_id = "async-callback-" + generate_uuid();
  message.content = content;
  message.metadata["message_type"] = "async_callback";
  message.metadata["async_call_key"] = call_key;
  message.metadata["session_id"] = session_id;
  message.metadata["dialogue_id"] = dialogue_id;
  message.metadata["task_id"] = task_id;
  message.metadata["node_id"] = node_id;

  // 3. 通过Runtime路由
  bool success = runtime_->enqueue_async_callback(message);

  return success ? build_success_response(R"({"success": true})", id)
                 : build_error_response(-100, "Task not found", id);
}
```

### 多语言SDK文件清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/runtime/databus/uds_server.cpp` | 修改 | 新增 `handle_async_callback` 方法 |
| `src/runtime/runtime.cpp` | 修改 | 提供 `enqueue_async_callback` 接口 |
| `src/runtime/worker/worker.cpp` | 修改 | 注入 `AF_ASYNC_*` 环境变量 |
| `src/runtime/databus/clients/python/agentflow_databus.py` | 修改 | 新增 `AsyncCallSDK` 类 |
| `src/runtime/databus/clients/shell/agentflow_async.sh` | 新增 | Shell异步调用SDK |
| `src/runtime/databus/clients/golang/async/async.go` | 新增 | Go异步调用SDK |
| `docs/UDS_PROTOCOL.md` | 修改 | 新增 `async_callback` 方法文档 |

### 多语言SDK实现优先级

| 优先级 | 功能点 | 说明 |
|--------|--------|------|
| P1 | UDSServer新增async_callback方法 | 核心协议支持 |
| P2 | Worker环境变量注入 | 所有语言需要 |
| P2 | Python SDK AsyncCallSDK | 最常用 |
| P3 | Shell SDK | 轻量级脚本节点 |
| P3 | Go SDK | 高性能节点 |