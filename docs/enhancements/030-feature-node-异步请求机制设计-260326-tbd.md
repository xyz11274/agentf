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
| `include/internal/runtime/agent_runtime.h` | 修改 | WorkflowExecutionState新增字段 |
| `include/internal/workflow/node.h` | 修改 | 新增ASYNC_PENDING状态 |
| `src/runtime/async_call.cpp` | 新增 | NodeAsyncCallSDK实现 |
| `src/runtime/databus/node_databus_client.cpp` | 修改 | 实现async()方法 |
| `src/runtime/runtime.cpp` | 修改 | 注入SDK，处理异步状态 |
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