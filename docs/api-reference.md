# AgentFlow API Reference

> 本文档按“Framework（MMAgent/MMWorkflow）- Runtime（调度）- MMDataBus（共享总线）”组织。

## 1. Framework 层

### 1.1 MMAgent API

头文件：`include/external/agent/agent.h`  
命名空间：`agentflow::framework`

核心能力：
- 生命周期：`initialize/start/pause/resume/stop`
- workflow 管理：`add_workflow/remove_workflow/get_workflow/get_workflows`
- 会话与消息路由：`get_or_create_session/route_request_message/process_session_messages`
- Coordinator 默认任务：未显式声明 `task_type` 时按 `task0` 处理
- 包内 coordinator 固定映射：`package.json.coordinator_workflow` 可配置 task0 默认 workflow
- 共享数据：`publish_data/get_data`
- 包加载：`create_from_package`
- 控制面协同：`msgque` 仅传 `data_ref`，payload 通过 MMDataBus 获取
- **跨 session 查询**（2026-03-06 新增）：
  - `get_all_session_ids() -> std::vector<std::string>`：返回当前 agent 所有 session ID
  - `get_session_output(session_id, key, value) -> bool`：从 agent DataBus 读取指定 session 的输出值，key 格式为 `session:<session_id>:<key>`

关联规范：
- MMAgent 包结构：`publish/agent/AGENT_PACKAGE_SPEC.md`
- 打包脚本：`publish/agent_build.sh`

### 1.2 MMWorkflow API

头文件：`include/external/workflow/workflow.h`  
命名空间：`agentflow::framework`

核心能力：
- 图装配：`add_node/add_edge/remove_node/remove_edge`
- DSL：`load_from_dsl/update_dynamic/validate`
- 执行态：`get_executable_nodes_with_control/mark_node_executed/reset_execution_state`
- 循环与控制上下文：`enter_loop/exit_loop_iteration/should_continue_loop`

DSL 关键字段：
- `nodes[]`：执行节点定义。
- `edges[]`：依赖与控制流定义。
- `edge.control.type`：`if/switch-case`。
- `includes[]`：引用外部 workflow 片段。
- `node.type=workflow + workflow_ref`：嵌套 workflow。

参考：`publish/workflow/README.md`、`publish/workflow/pattern/*.json`。

### 1.3 MMNode 开发 API

头文件：`include/external/workflow/node.h`

执行模式：
- `CPP_FUNCTION`
- `SHELL_SCRIPT`
- `EXECUTABLE`
- `GO_SCRIPT`
- `PYTHON_SCRIPT`
- `JAVA_JAR`
- `SO_LIBRARY`

配置重点：
- 输入输出：`input_params/output_params`（DSL 字段 `inputs`/`outputs`，字段类型支持 `string/int/float/bool/list/map`）
- 执行入口：`command/function_name/main_class/args`
- 可靠性：`timeout_ms/max_retries/retry_delay_ms`
- Worker 可执行文件热更新：`enable_hot_reload/hot_reload_config`（默认关闭；仅在节点显式配置 `enable_hot_reload: true` 时生效）
- 嵌套 workflow 引用：`workflow_ref`（当 `type=workflow`）
- **结构化输出**（2026-03-06 新增）：`json_output: true` — 脚本节点（Go/Python/Java）输出 JSON 时框架自动将每个键映射到 DataBus，无需文件中转
- **节点运行时自动注册**（2026-03-06 新增）：`node_runtime` 对象 — 在 workflow `start_runtime()` 时自动启动指定执行器，在 `stop_runtime()` 时自动停止
- **文件总线桥接**（2026-03-07 新增）：worker 执行脚本/二进制节点时会注入 `AF_DATABUS_DIR / AF_DATABUS_KV_DIR / AF_DATABUS_EVENTS_DIR`，多语言节点可通过 `publish/sdk/{shell,python,go,java}` 或 `include/external/runtime/databus_sdk.h` 访问 Set/Get/Sub/Pub
- **统一 LLM 配置**（2026-03-07 新增）：worker 注入 `AF_LLM_BASE_URL / AF_LLM_API_KEY / AF_LLM_MODEL`，脚本节点可通过 DataBus 发布 `agentflow.llm.request` 请求流式推理

SO 库节点开发约定（`execution_mode: so_library`）：
```cpp
// 导出函数签名
using SoFunction = void (*)(const DataMap& inputs, DataMap& outputs);
// DataMap = std::map<std::string, DataVariant>
extern "C" void my_node_func(const DataMap& inputs, DataMap& outputs);
```

MMNode 规范与模板：
- `publish/workflow/node_patterns/`
- `src/workflow/node/skill.md`

### 1.4 MMEdge API

头文件：`include/external/workflow/edge.h`

核心能力：
- 数据映射：`source_outputs -> target_inputs`
- 传输模式：`SHARED_MEMORY/MESSAGE_QUEUE/REMOTE_TRANSFER`
- 控制流：`control.type` 及附加参数（`expression` 或 `switch-case` 的 `variable/cases/default_target`）

---

## 2. Runtime 层

### 2.1 MMAgentRuntime

头文件：`include/internal/runtime/agent_runtime.h`  
命名空间：`agentflow::runtime`

运行角色：
- `RuntimeRole::MASTER`
- `RuntimeRole::WORKER`

关键接口：
- MMAgent 注册：`register_agent/unregister_agent/list_agents`
- workflow 执行：`load_workflow/execute_workflow/unload_workflow`
- worker 管理：`register_worker_engine/heartbeat_worker/update_worker_engine_status`
- workflow 部署：`deploy_workflow_instance/destroy_workflow_instance`
- task 调度：`schedule_task`

### 2.2 workflow instance -> worker（Deployer）

头文件：`include/internal/runtime/master/deployer.h`

职责：
- 维护 worker 注册、状态、心跳、负载。
- 维护 workflow 实例到 worker 的映射。
- 支持按偏好 worker 或负载策略选择执行节点。

### 2.3 task -> workflow instance（Scheduler）

头文件：`include/internal/runtime/master/scheduler.h`

输入：`TaskScheduleRequest`  
输出：`TaskScheduleResult`

职责：
- 按 agent 对象/实例与 workflow 信息选择可执行 workflow 实例。
- 返回最终 workflow instance 与 worker 定位信息。

### 2.4 Worker 执行

头文件：`include/internal/runtime/worker.h`
实现：`src/runtime/worker/worker.cpp`

职责：
- 通过 `MMWorkerEngine::execute_code` 统一执行节点多语言任务。
- 通过 `check_and_reload_code/get_code_version/rollback_code_version` 统一处理热更新与回退。
- 为外部节点准备文件型 DataBus 桥接目录，并向进程注入 `AF_DATABUS_*`。
- 监听 `agentflow.llm.request` 事件，并通过 runtime 统一的流式 LLM 客户端把 chunk / done / error 重新发布回 DataBus。
- 上报执行状态。
- 配合 runtime 推进 workflow 状态机。

### 2.5 事件处理器

头文件：`include/internal/runtime/event_handler.h`
命名空间：`agentflow::runtime`

**事件类型**：

```cpp
enum class EventType {
  // Agent 生命周期事件 (0-9)
  AGENT_REGISTERED = 0,
  AGENT_UNREGISTERED = 1,

  // Session 生命周期事件 (10-19)
  SESSION_CREATED = 10,
  SESSION_DESTROYED = 11,

  // Dialogue 生命周期事件 (20-29)
  DIALOGUE_CREATED = 20,
  DIALOGUE_DESTROYED = 21,

  // Task 生命周期事件 (30-39)
  TASK_CREATED = 30,
  TASK_DESTROYED = 31,
};
```

**事件结构体**：

| 结构体 | 说明 | 关键字段 |
|--------|------|----------|
| `AgentEvent` | Agent 注册/注销事件 | agent_id, name, version, workflows |
| `SessionEvent` | Session 创建/销毁事件 | agent_id, session_id, user_id, client_id |
| `DialogueEvent` | Dialogue 创建/销毁事件 | agent_id, session_id, dialogue_id, user_input |
| `TaskEvent` | Task 创建/销毁事件 | agent_id, session_id, task_id, workflow_id, task_type, skill_name |

**IEventHandler 接口**：

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

**Runtime 注册接口**：

```cpp
// MMAgentRuntime 方法
void register_event_handler(std::shared_ptr<IEventHandler> handler);
void unregister_event_handler(std::shared_ptr<IEventHandler> handler);
void clear_event_handlers();
size_t get_event_handler_count() const;
void set_callback_thread_pool_size(size_t size);
```

**使用示例**：

```cpp
// 1. 实现 IEventHandler 接口
class MyEventHandler : public runtime::IEventHandler {
public:
  void handle_agent_event(const runtime::AgentEvent& event) override {
    if (event.event_type == runtime::EventType::AGENT_REGISTERED) {
      std::cout << "Agent registered: " << event.name << std::endl;
    }
  }
  void handle_session_event(const runtime::SessionEvent& event) override {
    // 处理 session 事件
  }
  void handle_dialogue_event(const runtime::DialogueEvent& event) override {
    // 处理 dialogue 事件
  }
  void handle_task_event(const runtime::TaskEvent& event) override {
    // 处理 task 事件
  }
};

// 2. 注册事件处理器
auto handler = std::make_shared<MyEventHandler>();
runtime->register_event_handler(handler);

// 3. 配置线程池大小（默认 5 个线程）
runtime->set_callback_thread_pool_size(10);
```

**事件触发时机**：

| 事件 | 触发时机 |
|------|----------|
| AGENT_REGISTERED | `runtime->register_agent()` 成功后 |
| AGENT_UNREGISTERED | `runtime->unregister_agent()` 成功后 |
| SESSION_CREATED | `session->bind_route()` 时 |
| SESSION_DESTROYED | `session->reset_for_pool()` 时 |
| DIALOGUE_CREATED | dialogue_id 生成时 |
| DIALOGUE_DESTROYED | `cleanup_dialogue_data()` 时 |
| TASK_CREATED | `session->add_task()` 时 |
| TASK_DESTROYED | workflow 完成时 |

**注意事项**：
- 事件回调在独立线程池中异步执行
- 回调中应避免长时间阻塞操作
- 回调中访问共享资源需注意线程安全

### 2.6 异步调用 SDK（AsyncCall）

头文件：`include/external/agentflow/so_node.h`
命名空间：`agentflow`

**用途**：SO Library 节点发起异步操作（如 LLM 流式调用），避免阻塞 worker thread。

#### IAsyncCallSDK 接口

```cpp
class IAsyncCallSDK {
public:
  virtual ~IAsyncCallSDK() = default;

  // 发起异步调用，async_func 在独立线程执行
  virtual bool AsyncCall(const std::string& key,
                         std::function<void()> async_func) = 0;

  // 发送消息给自己（短路路由，直接入队到当前 task）
  virtual bool SendToSelf(const std::string& key,
                          const std::string& msg) = 0;

  // 发送消息给指定 node（跨 task）
  virtual bool SendMsg(const std::string& session_id,
                       const std::string& dialogue_id,
                       const std::string& target_task_id,
                       const std::string& target_node_id,
                       const std::string& key,
                       const std::string& msg) = 0;
};
```

#### 使用方式

通过 `INodeDataBusClient::async()` 获取 SDK 实例：

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
  auto async_sdk = client->async();

  // 发起异步操作
  async_sdk->AsyncCall("my-op", [&]() {
    // 在独立线程执行耗时操作
    auto result = external_service_call();

    // 发送结果给自己
    async_sdk->SendToSelf("my-op", result);
  });

  // 设置异步 pending 状态
  outputs["__async_pending__"] = SoDataVariant(std::string("true"));
  outputs["__async_key__"] = SoDataVariant(std::string("my-op"));
}
```

#### 异步回调处理

当异步消息到达时，框架会重新执行 node，并在 inputs 中注入：

| 字段名 | 类型 | 说明 |
|--------|------|------|
| `__async_callback__` | string | "true" 表示这是异步回调 |
| `__async_key__` | string | 对应 AsyncCall 时指定的 key |
| `__async_msg__` | string | 回调消息内容 |

```cpp
// 判断是否为异步回调
bool is_callback = inputs.count("__async_callback__") &&
                   std::get<std::string>(inputs.at("__async_callback__")) == "true";

if (is_callback) {
  std::string key = std::get<std::string>(inputs.at("__async_key__"));
  std::string msg = std::get<std::string>(inputs.at("__async_msg__"));
  // 处理回调消息
}
```

#### Workflow 生命周期

- `async_pending_count`：WorkflowExecutionState 中的计数器
- workflow 完成条件：`completed_count >= total_nodes && async_pending_count == 0`
- 每次节点设置 `__async_pending__` 时，计数器 +1
- 节点完成（不再设置 pending）时，计数器 -1

---

## 3. 高速数据总线（MMDataBus）

头文件：`include/internal/runtime/databus/databus.h`

核心接口：
- 数据操作：`put/get/remove/contains`
- 订阅：`subscribe/unsubscribe`
- DataRef：`store_with_ref/resolve_ref/find_refs_by_owner`
- 远端连接：`connect_remote/disconnect_remote`
- 统计：`get_used_capacity/get_total_capacity/get_data_count`

实现组件：
- `LocalDatabus`（本地共享）
- `RemoteDatabus`（跨节点/远端，默认 Redis 适配）
- `FileDataBusBridge`（面向 shell / Go / Java / Python / C++ 节点的共享目录桥接）
- 统一封装类型：`MMDataBus`（兼容别名 `HighSpeedDataBus`）

使用建议：
- 统一 key 命名，避免 workflow 间冲突。
- 对大对象优先使用可复用键并控制生命周期。
- 在控制流条件读取与节点输出写入之间保持键语义一致。
- 首条请求原始数据应先落本地缓存并生成 `data_ref`，再由 MMDataBus 自动同步远端服务。
- 需要流式推理时，节点向 `agentflow.llm.request` 发布 JSON 字符串：`{"request_id":"...","prompt":"...","channel":"agentflow.llm.chunk.<id>"}`；runtime 会继续发布 `agentflow.llm.done.<id>` / `agentflow.llm.error.<id>`
