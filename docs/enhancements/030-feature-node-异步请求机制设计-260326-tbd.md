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
extern "C" void skill_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                               INodeDataBusClient* client) {
  // 检查是否为回调调用
  if (so_node::get_bool(inputs, "__async_callback__", false)) {
    // 这是回调
    std::string key = so_node::get_string(inputs, "__async_key__");
    std::string msg = so_node::get_string(inputs, "__async_msg__");

    if (key == "llm-query") {
      if (msg == "[DONE]") {
        // 流式响应完成
        outputs["response"] = accumulated_response;
        // 不再设置__async_pending__，workflow将继续
      } else {
        // 处理chunk，继续等待更多回调
        accumulated_response += msg;
        outputs["__async_pending__"] = SoDataVariant(true);
        outputs["call_key"] = SoDataVariant(std::string("llm-query"));
      }
    }
    return;
  }

  // 首次调用：发起异步请求
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

  outputs["__async_pending__"] = SoDataVariant(true);
  outputs["call_key"] = SoDataVariant(std::string("llm-query"));
}
```

**inputs字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `__async_callback__` | bool | true表示这是回调调用 |
| `__async_key__` | string | 回调的key（node在AsyncCall时设置） |
| `__async_msg__` | string | 回调消息内容（chunk或[DONE]） |

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
| `publish/workflow/NODE_DEVELOPMENT_GUIDE.md` | 新增异步调用章节 |
| `publish/workflow/NODE_API_REFERENCE.md` | 新增async() API参考 |
| `src/runtime/databus/clients/python/README.md` | 新增Python异步SDK说明 |

### 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [024-feature-消息定向路由机制设计](024-feature-消息定向路由机制设计-260319-implemented.md) | SendToSelf基于定向路由实现 |
| [028-feature-node-模型推理服务流式访问设计](028-feature-node-模型推理服务流式访问设计-260323-tbd.md) | 异步LLM调用的应用场景 |

### 进展跟踪

- [ ] 代码实现
  - [ ] Phase 1: 接口定义
  - [ ] Phase 2: 生命周期管理
  - [ ] Phase 3: SDK实现
  - [ ] Phase 4: 回调处理
  - [ ] 多语言SDK
- [ ] 单元测试
- [ ] 集成测试
- [ ] 文档更新
- [ ] 验证通过

### 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-03-26 | Claude | 初始版本 |
| v1.1 | 2026-03-26 | Claude | 补充短路路由设计和回调入口设计 |