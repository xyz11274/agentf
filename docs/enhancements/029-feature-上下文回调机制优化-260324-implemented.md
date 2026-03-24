# 上下文回调机制优化 Enhancement

## 1. 背景

当前上下文回调机制注册在 MMAgent 上，回调接口仅提供有限信息（user_id, agent_id, user_input, previous_context）。为支持更丰富的上下文注入场景，需要：

1. 将回调注册从 Agent 迁移到 Master Runtime
2. 扩展回调接口支持多种事件类型（agent/session/dialogue/task）
3. 提供完整的 Agent DSL 静态拓扑信息
4. 支持异步调用

## 2. 方案简述

### 问题拆解

1. **注册位置变更**：从 MMAgent 迁移到 MMAgentRuntime（仅 Master 生效）
2. **事件类型定义**：定义 EventType 枚举，支持 AGENT/SESSION/DIALOGUE/TASK 事件
3. **回调接口设计**：定义 EventHandler 接口，包含四个事件处理方法
4. **事件推送时机**：在 Runtime 生命周期关键点触发事件
5. **异步执行支持**：线程池异步调用回调

### 架构变更

```
┌─────────────────────────────────────────────────────────────┐
│                    MMAgentRuntime (Master)                   │
├─────────────────────────────────────────────────────────────┤
│  event_handlers_[]      callback_thread_pool_               │
│       │                         │                           │
│       ▼                         ▼                           │
│  ┌─────────────┐          ┌──────────────┐                  │
│  │ register_   │          │ Thread Pool  │                  │
│  │ event_      │          │ (size=5)     │                  │
│  │ handler()   │          └──────────────┘                  │
│  └─────────────┘                                             │
│                                                              │
│  Events:                                                     │
│  ├── register_agent() ─────► AGENT_REGISTERED              │
│  ├── unregister_agent() ───► AGENT_UNREGISTERED            │
│  ├── session bind ─────────► SESSION_CREATED               │
│  ├── session reset ────────► SESSION_DESTROYED             │
│  ├── dialogue generate ────► DIALOGUE_CREATED              │
│  ├── dialogue cleanup ─────► DIALOGUE_DESTROYED            │
│  ├── task add ─────────────► TASK_CREATED                  │
│  └── task complete ────────► TASK_DESTROYED                │
└─────────────────────────────────────────────────────────────┘
```

## 3. 约束与假设

### 生效场景
- 仅 Master Runtime 注册回调有效
- Worker Runtime 注册回调无效果（直接返回或记录警告）

### 依赖关系
- 依赖 MMAgentRuntime 的 Master 角色
- 依赖 AgentPackage 提供 Agent DSL 信息

### 兼容性
- 保留 MMAgent 上的 `set_context_callback()` 接口，标记废弃
- 新接口在 MMAgentRuntime 上注册

## 4. 当前实现分析

### 代码分析

**当前回调签名** (`include/internal/agent/agent.h:47-51`)：
```cpp
using ContextCallback = std::function<std::string(
    const std::string& user_id,
    const std::string& agent_id,
    const std::string& user_input,
    const std::string& previous_context)>;
```

**当前注册位置** (`include/internal/agent/agent.h:237`)：
```cpp
ContextCallbackConfig context_callback_config_;  // 在 MMAgent 类中
```

**当前调用位置** (`src/agent/agent.cpp:1214-1286`)：
- 在 `handle_task_intent_request` 中创建 dialogue 时调用

### Examples 分析

- `examples/agent/system_agent_mock/` - 使用 agent.json 配置
- 需要验证新回调机制在该示例中的集成

### 缺失功能分析

| 缺失功能 | 行业实践 |
|---------|---------|
| 事件类型扩展 | Spring Event、Node.js EventEmitter 支持多种事件类型 |
| 完整上下文信息 | Kubernetes Controller Runtime 提供 Reconcile 上下文 |
| Agent 拓扑信息 | Prometheus 提供完整指标模型 |
| 异步执行 | Reactor 模式、CompletableFuture 异步编排 |

## 5. 数据结构设计

### EventType 枚举

```cpp
enum class EventType {
  // Agent lifecycle
  AGENT_REGISTERED = 0,
  AGENT_UNREGISTERED = 1,

  // Session lifecycle
  SESSION_CREATED = 10,
  SESSION_DESTROYED = 11,

  // Dialogue lifecycle
  DIALOGUE_CREATED = 20,
  DIALOGUE_DESTROYED = 21,

  // Task lifecycle
  TASK_CREATED = 30,
  TASK_DESTROYED = 31,
};
```

### Event 结构体

```cpp
// Node info in workflow topology
struct NodeInfo {
  std::string id;
  std::string name;
  std::string type;
  int         priority = 0;

  // Input/Output definitions
  std::vector<std::string> input_params;
  std::vector<std::string> output_params;
};

// Edge info in workflow topology
struct EdgeInfo {
  std::string id;
  std::string source_node_id;
  std::string target_node_id;
  std::string condition;  // Optional condition expression
};

// Workflow info - expanded workflow content
struct WorkflowInfo {
  std::string id;
  std::string name;
  std::string version;
  std::string description;

  // Expanded topology
  std::vector<NodeInfo> nodes;
  std::vector<EdgeInfo> edges;
};

// Agent event - 包含 Agent DSL 静态拓扑（workflow 内容展开）
struct AgentEvent {
  EventType event_type;  // AGENT_REGISTERED, AGENT_UNREGISTERED
  std::string agent_id;
  std::string name;
  std::string version;
  std::string description;

  // Expanded workflow topology (not just file names)
  std::vector<WorkflowInfo> workflows;
};

// Session event
struct SessionEvent {
  EventType event_type;  // SESSION_CREATED, SESSION_DESTROYED
  std::string agent_id;
  std::string user_id;
  std::string client_id;
  std::string session_id;
};

// Dialogue event
struct DialogueEvent {
  EventType event_type;  // DIALOGUE_CREATED, DIALOGUE_DESTROYED
  std::string agent_id;
  std::string user_id;
  std::string client_id;
  std::string session_id;
  std::string dialogue_id;
  std::string user_input;
};

// Task event - 包含 skill 信息
struct TaskEvent {
  EventType event_type;  // TASK_CREATED, TASK_DESTROYED
  std::string agent_id;
  std::string user_id;
  std::string client_id;
  std::string session_id;
  std::string dialogue_id;
  std::string task_id;
  std::string workflow_id;
  std::string task_type;      // dispatcher, coordinator, skill
  std::string skill_name;     // 仅 skill 类型有效
  std::string skill_config_dir; // 仅 skill 类型有效
};
```

### EventHandler 接口

```cpp
class IEventHandler {
public:
  virtual ~IEventHandler() = default;

  virtual void handle_agent_event(const AgentEvent& event) = 0;

  virtual void handle_session_event(const SessionEvent& event) = 0;

  virtual void handle_dialogue_event(const DialogueEvent& event) = 0;

  virtual void handle_task_event(const TaskEvent& event) = 0;
};
```

## 6. 流程设计

### Agent 事件流程

```
┌──────────────┐     register_agent()     ┌──────────────────┐
│   External   │ ───────────────────────► │ MMAgentRuntime   │
│              │                          │                  │
│              │     unregister_agent()   │  if(MASTER) {    │
│              │ ───────────────────────► │    emit_event()  │
└──────────────┘                          │  }               │
                                          └──────────────────┘
                                                   │
                                                   ▼
                                          ┌──────────────────┐
                                          │ IEventHandler    │
                                          │ handle_agent_    │
                                          │ event()          │
                                          └──────────────────┘
```

### Session/Dialogue/Task 事件流程

```
Session: bind_route() / reset_for_pool()
    │
    ├──► SESSION_CREATED (user_id, client_id, session_id)
    │
    └──► SESSION_DESTROYED (session_id)

Dialogue: generate in dispatcher
    │
    ├──► DIALOGUE_CREATED (dialogue_id, user_input)
    │
    └──► DIALOGUE_DESTROYED (cleanup_dialogue_data)

Task: add_task() / task completion
    │
    ├──► TASK_CREATED (task_id, workflow_id)
    │
    └──► TASK_DESTROYED (task_id)
```

## 7. 接口设计

### MMAgentRuntime 新增接口

```cpp
// include/internal/runtime/agent_runtime.h

class MMAgentRuntime {
public:
  // Event handler registration (Master only)
  void register_event_handler(std::shared_ptr<IEventHandler> handler);
  void unregister_event_handler(std::shared_ptr<IEventHandler> handler);
  void clear_event_handlers();

  // Configuration
  void set_callback_thread_pool_size(size_t size);
  size_t get_callback_thread_pool_size() const;

private:
  // Event emission (internal)
  void emit_agent_event(const AgentEvent& event);
  void emit_session_event(const SessionEvent& event);
  void emit_dialogue_event(const DialogueEvent& event);
  void emit_task_event(const TaskEvent& event);

  // Handler storage
  std::vector<std::shared_ptr<IEventHandler>> event_handlers_;
  std::mutex event_handlers_mutex_;

  // Async execution
  size_t callback_thread_pool_size_ = 5;
};
```

### 配置扩展

```json
// agent.json (可选配置)
{
  "name": "MyAgent",
  "event_callback": {
    "thread_pool_size": 5,
    "async": true
  }
}
```

## 8. 实现方案

### 功能点实现优先级

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | EventType 枚举和事件结构体 | 无副作用 | 单元测试 |
| P1 | IEventHandler 接口定义 | 无副作用 | 编译验证 |
| P2 | MMAgentRuntime 注册接口 | 无副作用 | 单元测试 |
| P2 | 线程池实现 | 无副作用 | 单元测试 |
| P3 | WorkflowInfo 构建逻辑 | 无副作用 | 单元测试 |
| P3 | AGENT 事件推送 | 修改 register_agent | 集成测试 |
| P3 | SESSION 事件推送 | 修改 session lifecycle | 集成测试 |
| P3 | DIALOGUE 事件推送 | 修改 dialogue lifecycle | 集成测试 |
| P3 | TASK 事件推送 | 修改 task lifecycle | 集成测试 |
| P4 | 废弃 MMAgent 接口 | 兼容性影响 | 回归测试 |

### 待修改文件清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `include/internal/runtime/event_handler.h` | 新增 | EventType、NodeInfo、EdgeInfo、WorkflowInfo、AgentEvent、SessionEvent、DialogueEvent、TaskEvent、IEventHandler 定义 |
| `include/internal/runtime/agent_runtime.h` | 修改 | 添加注册接口和事件推送方法 |
| `src/runtime/runtime.cpp` | 修改 | 实现注册、推送、线程池逻辑 |
| `src/runtime/event_handler.cpp` | 新增 | WorkflowInfo 构建逻辑（从 AgentPackage 提取 workflow 节点/边信息） |
| `src/agent/agent.cpp` | 修改 | 在 session/dialogue/task 生命周期调用 runtime 事件推送 |
| `include/internal/agent/agent.h` | 修改 | 标记旧接口废弃 |
| `examples/agent/system_agent_mock/` | 修改 | 添加事件处理器示例 |

## 9. 测试方案

### Examples 黑盒测试覆盖分析

| 功能点 | 现有 Examples 覆盖 | 更新建议 |
|--------|-------------------|---------|
| Agent 事件 | ❌未覆盖 | 新增 test_event_handler |
| Session 事件 | ❌未覆盖 | 新增 test_event_handler |
| Dialogue 事件 | ❌未覆盖 | 新增 test_event_handler |
| Task 事件 | ❌未覆盖 | 新增 test_event_handler |

### 测试用例设计

1. **单元测试**：
   - EventType 枚举值验证
   - NodeInfo/EdgeInfo/WorkflowInfo 结构体构建
   - WorkflowInfo 从 AgentPackage 提取逻辑
   - IEventHandler 注册/注销
   - 线程池异步执行

2. **集成测试**：
   - register_agent 触发 AGENT_REGISTERED（验证 workflow 拓扑完整）
   - session bind 触发 SESSION_CREATED
   - dialogue generate 触发 DIALOGUE_CREATED
   - task add 触发 TASK_CREATED

3. **独立性要求**：
   - 测试使用 Mock EventHandler
   - 不修改功能代码

## 10. 文档

### 需要变更的文档

| 文档 | 变更说明 |
|------|---------|
| [docs/api-reference.md](../../api-reference.md) | 添加 IEventHandler 接口文档 |
| [publish/INTEGRATION_GUIDE.md](../../../publish/INTEGRATION_GUIDE.md) | 更新上下文回调章节 |
| [docs/ARCHITECTURE.md](../../ARCHITECTURE.md) | 添加事件机制架构说明 |

### 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [Session/Task 生命周期参考](011-refactor-sessiontaskcoordinator-context-分层重构-260301-implemented.md) | Session/Task 生命周期定义 |
| [事件追踪模式参考](016-feature-全链路跟踪系统tracesystem-260307-implemented.md) | FlowTrace 事件追踪模式 |

## 11. 进展跟踪

- [x] 代码实现
- [x] 单元测试
- [x] 集成测试
- [x] 黑盒测试
- [x] 文档更新
- [x] 验证通过

### 详细进度

| 功能点 | 功能 | 用例 | 文档 | 状态 |
|--------|------|------|------|------|
| EventType 枚举 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| NodeInfo/EdgeInfo/WorkflowInfo 结构体 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| IEventHandler 接口 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| Runtime 注册接口 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| EventBuilder 辅助类 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| AGENT_REGISTERED/UNREGISTERED 事件推送 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| SESSION_CREATED/DESTROYED 事件推送 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| Session 事件回调机制 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| DIALOGUE_CREATED/DESTROYED 事件推送 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| TASK_CREATED/DESTROYED 事件推送 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |
| 线程池异步执行 | ✅已就绪 | ✅已就绪 | ✅已就绪 | ✅已完成 |

### 验证结果

| 验证项 | 结果 | 说明 |
|--------|------|------|
| 编译验证 | ✅通过 | 无编译错误 |
| 单元测试 | ✅通过 | 25 tests passed |
| 集成测试 | ✅通过 | 7 tests passed |
| 黑盒测试 | ✅通过 | 4 tests passed (E2E with SystemAgentMock) |

### 集成测试设计

**测试文件**: `tests/functional/src/test_event_handler_integration.cpp`

| 测试用例 | 验证内容 |
|----------|----------|
| `AgentRegisteredEvent` | Agent 注册后触发 AGENT_REGISTERED 事件 |
| `AgentUnregisteredEvent` | Agent 注销后触发 AGENT_UNREGISTERED 事件 |
| `SessionCreatedEvent` | Session 创建后触发 SESSION_CREATED 事件 |
| `SessionDestroyedEvent` | Session 销毁后触发 SESSION_DESTROYED 事件 |
| `AsyncCallbackExecution` | 回调在线程池中异步执行，与主线程分离 |
| `MultipleEventTypesInSequence` | 多种事件类型按序触发并正确记录 |
| `HandlerRegistrationUnregistration` | 动态注册/注销 handler 后事件分发正确 |

**测试关键验证点**:
- 事件类型与 EventType 枚举值匹配
- 事件数据完整（agent_id, session_id, user_id 等）
- 回调在独立线程执行（非主线程）
- 多 handler 时事件正确分发给所有 handler

### 黑盒测试设计

**测试文件**: `tests/e2e/event_handler/test_event_handler_blackbox.cpp`

使用 SystemAgentMock 进行端到端黑盒测试，验证真实对话流程中的事件推送。

| 测试用例 | 验证内容 |
|----------|----------|
| `AgentLifecycleEvents` | Agent 注册时触发 AGENT_REGISTERED |
| `SessionLifecycleEvents` | Session 创建时触发 SESSION_CREATED |
| `FullDialogueFlow` | 完整对话流程中触发所有事件（SESSION/DIALOGUE/TASK） |
| `EventOrderConsistency` | 事件时序正确（AGENT < SESSION < DIALOGUE） |

**测试流程**:
1. 加载 SystemAgentMock
2. 注册事件处理器
3. 发送用户消息触发对话
4. 等待响应完成
5. 验证事件记录和时序

## 12. 后续计划（可选）

| 优化项 | 预期效果 | 方案简述 |
|--------|---------|---------|
| 事件过滤 | 减少无效回调 | 支持按 EventType 订阅 |
| 事件优先级 | 高优先级事件优先处理 | 优先级队列 |
| 批量事件 | 减少回调次数 | 合并短时间内的同类事件 |

## 13. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-03-24 | xl4872 | 初始版本：定义事件类型、事件结构体（AgentEvent 含展开的 workflow 拓扑）、IEventHandler 接口、线程池异步执行机制 |