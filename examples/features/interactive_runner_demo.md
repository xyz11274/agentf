# InteractiveRunner 交互式流程演示

本文档使用 `nested_workflow_agent` 示例详细演示消息驱动的交互式执行流程。

## 1. nested_workflow_agent 结构

### Package 配置
```
examples/agent/nested_workflow_agent/
├── package.json              # Package 配置
├── main_workflow.json        # 主工作流
├── data_pipeline.json        # 数据管道子工作流
├── validation_workflow.json  # 验证子工作流
└── processing_workflow.json  # 处理子工作流
```

### MMWorkflow 层次结构

```
main-workflow (主工作流)
├── start
├── data_pipeline ──────────▶ data-pipeline-workflow (子工作流)
│                                ├── fetch_data (shell脚本)
│                                ├── validate_data ──▶ validation-workflow (嵌套子工作流)
│                                │                        ├── check_schema
│                                │                        ├── validate_fields
│                                │                        └── validation_report
│                                └── transform_data (shell脚本)
├── final_processing ───────▶ processing-workflow (子工作流)
│                                ├── aggregate_data
│                                ├── generate_output
│                                └── persist_results
└── end
```

## 2. 启动交互式模式

```bash
export LD_LIBRARY_PATH=$PWD/build/lib:$LD_LIBRARY_PATH
./build/agent_runner examples/agent/nested_workflow_agent --interactive
```

### 启动流程

```
┌────────────────────────────────────────────────────────────────────────┐
│                        InteractiveRunner::initialize()                  │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  1. 加载 MMAgent Package                                                 │
│     agent_ = MMAgent::create_from_package(package_path)                  │
│     - 解析 package.json                                                │
│     - 加载 4 个 workflow 定义                                          │
│     - 构建节点依赖图                                                   │
│                                                                        │
│  2. 创建 MMAgentRuntime                                                  │
│     runtime_ = std::make_unique<MMAgentRuntime>()                        │
│     - 初始化 Worker 线程池                                             │
│     - 创建 Scheduler 和 Deployer                                       │
│                                                                        │
│  3. 注册 MMAgent 到 Runtime                                              │
│     runtime_->register_agent(agent_)                                   │
│     - 创建 MMAgent 专属 MMDataBus                                          │
│     - 启动心跳监控                                                     │
│                                                                        │
│  4. 启动 Runtime 和 MMAgent                                              │
│     runtime_->start()  → 启动工作线程                                  │
│     agent_->start()    → 启动事件循环线程                              │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘

输出:
=== AgentFlow MMAgent Runner ===
Package: examples/agent/nested_workflow_agent
Package: nested-workflow-agent v1.0.0
MMAgent: nested-workflow-agent initialized

=== Starting Interactive Mode ===
Using message-driven architecture through MMAgent framework

=== AgentFlow Interactive Mode ===
Package: examples/agent/nested_workflow_agent
MMAgent: nested-workflow-agent
Type 'help' for available commands.

Available commands:
  execute <workflow_id> [--input <text>]  Execute a workflow
  list                                   List available workflows
  status                                 Show agent status
  help                                   Show this help
  quit / exit                            Exit interactive mode
```

## 3. 消息驱动执行流程详解

### 3.1 用户输入命令

```
> execute main-workflow --input "test data"
```

### 3.2 InteractiveRunner::handle_command() 解析命令

```cpp
// 解析参数
workflow_id = "main-workflow"
input = "test data"

// 验证 workflow 存在
auto workflow = agent_->get_workflow("main-workflow");
// workflow->get_id() = "main-workflow"
// workflow->get_nodes().size() = 4
```

### 3.3 InteractiveRunner::execute_workflow() 执行

```cpp
std::string InteractiveRunner::execute_workflow(const std::string& workflow_id,
                                                 const std::string& input) {
  // 1. 提交用户请求
  submit_user_request(workflow_id, input);

  // 2. 处理待处理任务
  process_pending_tasks();

  // 3. 轮询响应，直到 runtime 和 response queue 都进入安静态
  return poll_results(config_.timeout_ms);
}
```

### 3.4 步骤1: submit_user_request() - 消息构造与路由

```
┌────────────────────────────────────────────────────────────────────────┐
│              InteractiveRunner::submit_user_request()                   │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ 构造 Message                                                     │  │
│  │                                                                  │  │
│  │ MMAgentMsgQue::Message message;                                    │  │
│  │ message.message_id = "msg_1_1735932904558"                       │  │
│  │ message.type = MessageType::TASK_REQUEST                         │  │
│  │ message.priority = MessagePriority::NORMAL                       │  │
│  │ message.sender_id = "interactive_user"                           │  │
│  │ message.receiver_id = "nested-workflow-agent"                    │  │
│  │ message.content = "test data"                                    │  │
│  │ message.timestamp = 1735932904558                                │  │
│  │                                                                  │  │
│  │ message.metadata["user_id"] = "interactive_user"                 │  │
│  │ message.metadata["client_id"] = "cli"                            │  │
│  │ message.metadata["workflow_id"] = "main-workflow"                │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                              │                                         │
│                              ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ MMAgent::route_request_message(message)                            │  │
│  │                                                                  │  │
│  │ 1. 检查事件循环是否运行                                          │  │
│  │    if (!request_event_running_) {                                │  │
│  │      handle_request_event(message);  // 直接处理                 │  │
│  │    }                                                             │  │
│  │                                                                  │  │
│  │ 2. 如果事件循环运行，将消息加入队列                              │  │
│  │    request_event_queue_.push(message);                           │  │
│  │    request_event_cv_.notify_all();                               │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                              │                                         │
│                              ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ MMAgent::handle_request_event(message)                             │  │
│  │                                                                  │  │
│  │ 1. 将消息加入 default_session 的接收队列                         │  │
│  │    default_session_->enqueue_received_message(message)           │  │
│  │                                                                  │  │
│  │ 2. 根据 user_id 和 client_id 获取或创建目标 session              │  │
│  │    target_session = get_or_create_session("interactive_user",    │  │
│  │                                            "cli")                │  │
│  │    // 如果 session 不存在，创建新的 session 并                   │  │
│  │    // 初始化 SessionTaskCoordinator                              │  │
│  │                                                                  │  │
│  │ 3. 将消息转发到目标 session                                      │  │
│  │    target_session->enqueue_received_message(message)             │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### 3.5 步骤2: process_pending_tasks() - 任务生成与调度

```
┌────────────────────────────────────────────────────────────────────────┐
│              InteractiveRunner::process_pending_tasks()                 │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ MMAgent::process_all_sessions_until_idle()                         │  │
│  │                                                                  │  │
│  │ while (rounds++ < 64) {                                          │  │
│  │   for (const auto& session : sessions) {                         │  │
│  │     auto coordinator = session->get_task_coordinator();          │  │
│  │     auto tasks = coordinator->process_session_messages(          │  │
│  │                     *session, workflows_);                       │  │
│  │     // 将生成的任务加入 generated_tasks_ 队列                    │  │
│  │   }                                                              │  │
│  │   if (queues_empty && round_tasks == 0) break;                   │  │
│  │ }                                                                │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                              │                                         │
│                              ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ SessionTaskCoordinator::process_session_messages()               │  │
│  │                                                                  │  │
│  │ while (session.dequeue_received_message(message)) {              │  │
│  │   auto task = build_task_for_message(session_id, message, ...);  │  │
│  │   session.add_task(task);                                        │  │
│  │   tasks.push_back(task);                                         │  │
│  │ }                                                                │  │
│  │                                                                  │  │
│  │ // build_task_for_message() 构建 MMAgentTask:                      │  │
│  │ // - task_id: "interactive_user:cli_task_1"                      │  │
│  │ // - task_type: "task0" (默认)                                   │  │
│  │ // - workflow_id: "main-workflow" (从 message.metadata 获取)     │  │
│  │ // - workflow_file: "main_workflow.json"                         │  │
│  │ // - parameters["message_content"]: "test data"                  │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                              │                                         │
│                              ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ 调度任务到 Runtime                                               │  │
│  │                                                                  │  │
│  │ while (agent_->pop_generated_task(task)) {                       │  │
│  │   TaskScheduleRequest request;                                   │  │
│  │   request.task_id = task->get_task_id();                         │  │
│  │   request.workflow_id = task->get_workflow_id();                 │  │
│  │   request.workflow_file = task->get_workflow_file();             │  │
│  │   request.agent_object_id = agent_->get_id();                    │  │
│  │                                                                  │  │
│  │   auto result = runtime_->schedule_task(request);                │  │
│  │   // result.workflow_instance_id = "wf_inst_001"                 │  │
│  │   // result.worker_id = "local_worker"                           │  │
│  │   // result.scheduled = true                                     │  │
│  │ }                                                                │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### 3.6 Runtime 三级调度执行

```
┌────────────────────────────────────────────────────────────────────────┐
│                    MMAgentRuntime 三级调度执行                            │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  Level 1: MMAgent Level Master (MMAgent 实例生命周期管理)                  │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ - 监控 MMAgent 健康状态                                            │  │
│  │ - 管理所有 MMWorkflow 实例                                         │  │
│  │ - 协调多个 MMAgent 实例间的负载均衡                                │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                              │                                         │
│                              ▼                                         │
│  Level 2: MMWorkflow Level Master (MMWorkflow 图解析和调度)                │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ - 解析 main-workflow 的 DAG 结构                                 │  │
│  │ - 计算拓扑排序: [start → data_pipeline → final_processing → end]│  │
│  │ - 获取可执行节点列表                                             │  │
│  │ - 处理嵌套 workflow 调用                                         │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                              │                                         │
│                              ▼                                         │
│  Level 3: Worker (MMNode 执行)                                           │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ - 执行具体节点                                                   │  │
│  │ - 处理 shell_script 节点 (运行 .sh 脚本)                         │  │
│  │ - 处理 workflow 类型节点 (递归调用子 workflow)                   │  │
│  │ - 通过 MMDataBus 交换数据                                          │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### 3.7 main-workflow 执行流程

```
┌────────────────────────────────────────────────────────────────────────┐
│              main-workflow DAG 执行流程                                 │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  输入: "test data"                                                     │
│                                                                        │
│  ┌─────────┐                                                           │
│  │  start  │ (跳过)                                                    │
│  └────┬────┘                                                           │
│       │                                                                │
│       ▼                                                                │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ data_pipeline (workflow_ref: data_pipeline.json)                 │  │
│  │                                                                  │  │
│  │   ┌─────────────┐                                                │  │
│  │   │ fetch_data  │ 执行 fetch_data.sh                             │  │
│  │   │ (script)    │ Input: "test data"                             │  │
│  │   └──────┬──────┘ Output: fetched_data                           │  │
│  │          │                                                       │  │
│  │          ▼                                                       │  │
│  │   ┌─────────────────────────────────────────────────────────┐   │  │
│  │   │ validate_data (workflow_ref: validation_workflow.json)   │   │  │
│  │   │                                                          │   │  │
│  │   │  ┌─────────────┐                                         │   │  │
│  │   │  │ check_schema│ 验证数据模式                            │   │  │
│  │   │  └──────┬──────┘                                         │   │  │
│  │   │         │                                                │   │  │
│  │   │         ▼                                                │   │  │
│  │   │  ┌──────────────┐                                        │   │  │
│  │   │  │validate_fields│ 验证字段                              │   │  │
│  │   │  └───────┬──────┘                                        │   │  │
│  │   │          │                                               │   │  │
│  │   │          ▼                                               │   │  │
│  │   │  ┌─────────────────┐                                     │   │  │
│  │   │  │validation_report │ 生成验证报告                       │   │  │
│  │   │  └─────────────────┘                                     │   │  │
│  │   └──────────────────────────────────────────────────────────┘   │  │
│  │          │                                                       │  │
│  │          ▼                                                       │  │
│  │   ┌────────────────┐                                             │  │
│  │   │ transform_data │ 执行 transform_data.sh                     │  │
│  │   │ (script)       │ Input: validated_data                      │  │
│  │   └────────────────┘ Output: transformed_data                   │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│       │                                                                │
│       ▼                                                                │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │ final_processing (workflow_ref: processing_workflow.json)        │  │
│  │                                                                  │  │
│  │   ┌────────────────┐                                             │  │
│  │   │ aggregate_data │ 聚合数据                                    │  │
│  │   └───────┬────────┘                                             │  │
│  │           │                                                      │  │
│  │           ▼                                                      │  │
│  │   ┌─────────────────┐                                            │  │
│  │   │ generate_output │ 生成输出                                   │  │
│  │   └────────┬────────┘                                            │  │
│  │            │                                                     │  │
│  │            ▼                                                     │  │
│  │   ┌─────────────────┐                                            │  │
│  │   │ persist_results │ 持久化结果                                 │  │
│  │   └─────────────────┘                                            │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│       │                                                                │
│       ▼                                                                │
│  ┌─────────┐                                                           │
│  │   end   │ (跳过)                                                    │
│  └─────────┘                                                           │
│                                                                        │
│  输出: 最终处理结果 → agent.response_queue                            │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### 3.8 步骤3: poll_results() - 轮询直到执行安静

```cpp
std::string InteractiveRunner::poll_results(int timeout_ms) {
  auto start = std::chrono::steady_clock::now();
  const int poll_interval_ms = 50;
  const auto quiet_window = std::chrono::milliseconds(200);
  std::ostringstream result;
  std::chrono::steady_clock::time_point last_response_at{};
  bool saw_response = false;

  while (true) {
    MMAgentMsgQue::Message response;
    while (agent_->dequeue_response_message(response)) {
      saw_response = true;
      last_response_at = std::chrono::steady_clock::now();
      result << response.content << "\n";
    }

    const auto now = std::chrono::steady_clock::now();
    const bool runtime_quiet = runtime_->get_running_nodes() == 0;
    const bool response_queue_empty = agent_->get_response_queue_size() == 0;
    if (saw_response && runtime_quiet && response_queue_empty
        && (now - last_response_at) >= quiet_window) {
      return result.str();
    }

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout_ms) {
      return saw_response ? result.str() : "";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
  }
}
```

## 4. 完整交互示例

```bash
$ ./build/agent_runner examples/agent/nested_workflow_agent --interactive

=== AgentFlow MMAgent Runner ===
Package: examples/agent/nested_workflow_agent
Package: nested-workflow-agent v1.0.0
MMAgent: nested-workflow-agent initialized

=== Starting Interactive Mode ===
Using message-driven architecture through MMAgent framework

=== AgentFlow Interactive Mode ===
Package: examples/agent/nested_workflow_agent
MMAgent: nested-workflow-agent
Type 'help' for available commands.

Available commands:
  execute <workflow_id> [--input <text>]  Execute a workflow
  list                                   List available workflows
  status                                 Show agent status
  help                                   Show this help
  quit / exit                            Exit interactive mode

> list

=== Workflows ===
  [main-workflow] Main MMWorkflow (4 nodes)
    └─> workflow_ref: data_pipeline.json
    └─> workflow_ref: processing_workflow.json
  [data-pipeline-workflow] Data Pipeline MMWorkflow (3 nodes)
    └─> workflow_ref: validation_workflow.json
  [validation-workflow] Validation MMWorkflow (3 nodes)
  [processing-workflow] Processing MMWorkflow (3 nodes)

> status

=== Status ===
  MMAgent: nested-workflow-agent
  MMAgent ID: nested-workflow-agent
  State: 2
  Workflows: 4
  Runtime running: yes
  Active agents: 1
  Active workflows: 0
  Running nodes: 0

> execute main-workflow --input "test data"

Executing workflow: main-workflow
--- Output ---
[MMWorkflow execution results...]

> quit

Goodbye!
```

## 5. 关键优势总结

### 5.1 消息驱动架构的好处

1. **正确的框架使用**:
   - 使用 `MMAgent::route_request_message()` 而非直接调用 `MMWorkerEngine::execute_code()`
   - 利用 Session 管理用户上下文
   - 利用 Coordinator 协调任务生成

2. **完整的功能支持**:
   - Session 管理: 多用户、多客户端隔离
   - Coordinator: 根据消息自动生成任务并绑定 workflow
   - 三级调度: MMAgent → MMWorkflow → MMNode

3. **可扩展性**:
   - 支持多 MMAgent 实例
   - 支持分布式部署
   - 支持热加载

### 5.2 与旧实现的对比

| 方面 | 旧实现 (直接执行) | 新实现 (消息驱动) |
|------|------------------|------------------|
| 执行方式 | 直接调用 `MMWorkerEngine::execute_code()` | 通过消息队列驱动 |
| Session 管理 | 不使用 | 完整的 Session 生命周期管理 |
| Coordinator | 不使用 | 自动任务生成和 workflow 绑定 |
| 三级调度 | 不使用 | MMAgent → MMWorkflow → MMNode 完整调度 |
| 嵌套 MMWorkflow | 手动处理 | 框架自动处理 DAG 依赖 |
| 可扩展性 | 单进程 | 支持分布式部署 |

## 6. InteractiveRunner 与框架组件交互图

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                              InteractiveRunner                                   │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │                           用户交互层                                       │  │
│  │  run() → handle_command() → execute_workflow()                            │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
│                    │                      │                      │               │
│                    ▼                      ▼                      ▼               │
│  ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐      │
│  │ submit_user_request │  │ process_pending_    │  │ poll_results        │      │
│  │                     │  │ tasks               │  │                     │      │
│  └──────────┬──────────┘  └──────────┬──────────┘  └──────────┬──────────┘      │
└─────────────┼────────────────────────┼────────────────────────┼─────────────────┘
              │                        │                        │
              ▼                        ▼                        ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                                  MMAgent                                          │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │ route_request_message() ─▶ handle_request_event()                         │  │
│  │        │                          │                                        │  │
│  │        ▼                          ▼                                        │  │
│  │  request_event_queue_     default_session_                                │  │
│  │        │                          │                                        │  │
│  │        └────────────┬─────────────┘                                        │  │
│  │                     ▼                                                      │  │
│  │     process_all_sessions_until_idle()                                      │  │
│  │                     │                                                      │  │
│  │                     ▼                                                      │  │
│  │     pop_generated_task() ─▶ generated_tasks_ 队列                         │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
│                              │                                                  │
│                              ▼                                                  │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │                            MMAgentSession                                    │  │
│  │                                                                            │  │
│  │  receive_queue_ ◀── enqueue_received_message()                            │  │
│  │        │                                                                   │  │
│  │        ▼                                                                   │  │
│  │  SessionTaskCoordinator::process_session_messages()                        │  │
│  │        │                                                                   │  │
│  │        ▼                                                                   │  │
│  │  response_queue_ ──▶ dequeue_response_message()                           │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
│                              │                                                  │
│                              ▼                                                  │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │                     SessionTaskCoordinator                                 │  │
│  │                                                                            │  │
│  │  process_session_messages()                                                │  │
│  │        │                                                                   │  │
│  │        ├──▶ build_task_for_message()                                       │  │
│  │        │         │                                                         │  │
│  │        │         ├──▶ 解析 message.metadata["workflow_id"]                │  │
│  │        │         ├──▶ 查找对应 MMWorkflow                                    │  │
│  │        │         └──▶ 创建 MMAgentTask                                       │  │
│  │        │                                                                   │  │
│  │        └──▶ session.add_task(task)                                        │  │
│  │                   返回 tasks 列表                                          │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                              MMAgentRuntime                                       │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │                         三级调度架构                                       │  │
│  │                                                                            │  │
│  │  Level 1: MMAgent Master                                                     │  │
│  │  ┌─────────────────────────────────────────────────────────────────────┐  │  │
│  │  │ register_agent() ─▶ agents_ map                                     │  │  │
│  │  │ monitor_agents()  ─▶ 心跳监控、健康检查                              │  │  │
│  │  └─────────────────────────────────────────────────────────────────────┘  │  │
│  │                              │                                             │  │
│  │                              ▼                                             │  │
│  │  Level 2: MMWorkflow Master                                                  │  │
│  │  ┌─────────────────────────────────────────────────────────────────────┐  │  │
│  │  │ schedule_task(request)                                               │  │  │
│  │  │       │                                                              │  │  │
│  │  │       ├──▶ Scheduler::schedule_task()                                │  │  │
│  │  │       │         │                                                    │  │  │
│  │  │       │         ├──▶ 分配 workflow_instance_id                       │  │  │
│  │  │       │         └──▶ 分配 worker_id                                  │  │  │
│  │  │       │                                                              │  │  │
│  │  │       └──▶ shared_state_->add_or_update_task_instance()              │  │  │
│  │  └─────────────────────────────────────────────────────────────────────┘  │  │
│  │                              │                                             │  │
│  │                              ▼                                             │  │
│  │  Level 3: Worker (MMNode Executor)                                          │  │
│  │  ┌─────────────────────────────────────────────────────────────────────┐  │  │
│  │  │ worker_thread_main()                                                 │  │  │
│  │  │       │                                                              │  │  │
│  │  │       ├──▶ 从 node_task_queue_ 获取任务                              │  │  │
│  │  │       │                                                              │  │  │
│  │  │       └──▶ process_node(agent, workflow, node)                       │  │  │
│  │  │                 │                                                    │  │  │
│  │  │                 ├──▶ node->execute()                                 │  │  │
│  │  │                 │         │                                          │  │  │
│  │  │                 │         ├──▶ shell_script: 执行脚本                │  │  │
│  │  │                 │         ├──▶ workflow: 递归调用子workflow          │  │  │
│  │  │                 │         └──▶ callback:  执行回调函数               │  │  │
│  │  │                 │                                                    │  │  │
│  │  │                 └──▶ update_workflow_state()                         │  │  │
│  │  └─────────────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                                 MMWorkflow                                        │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │                            DAG 执行引擎                                    │  │
│  │                                                                            │  │
│  │  get_topological_order() ─▶ [start → node1 → node2 → ... → end]          │  │
│  │                                                                            │  │
│  │  get_executable_nodes() ─▶ 返回当前可执行的节点列表                        │  │
│  │                                                                            │  │
│  │  mark_node_executed(node_id, success) ─▶ 更新执行状态                     │  │
│  │                                                                            │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                                  MMNode                                           │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │                            节点执行器                                      │  │
│  │                                                                            │  │
│  │  execute()                                                                 │  │
│  │      │                                                                     │  │
│  │      ├──▶ ExecutionMode::SHELL_SCRIPT                                      │  │
│  │      │         └──▶ MMWorkerEngine::execute_code()                           │  │
│  │      │                     └──▶ fork + exec 脚本                           │  │
│  │      │                                                                     │  │
│  │      ├──▶ ExecutionMode::WORKFLOW                                          │  │
│  │      │         └──▶ 递归加载并执行子 workflow                              │  │
│  │      │                                                                     │  │
│  │      └──▶ ExecutionMode::CALLBACK                                          │  │
│  │                └──▶ 执行注册的回调函数                                     │  │
│  │                                                                            │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────┘
```

## 7. 数据流向图

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                                数据流向                                          │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│  用户输入 "test data"                                                           │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                                │
│  │   Message   │  content = "test data"                                        │
│  │             │  metadata["workflow_id"] = "main-workflow"                    │
│  └──────┬──────┘                                                                │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                                │
│  │   Session   │  receive_queue_ ◀── Message                                   │
│  │             │  session_id = "interactive_user:cli"                          │
│  └──────┬──────┘                                                                │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                                │
│  │ MMAgentTask   │  task_id = "interactive_user:cli_task_1"                      │
│  │             │  workflow_id = "main-workflow"                                │
│  │             │  parameters["message_content"] = "test data"                  │
│  └──────┬──────┘                                                                │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                                │
│  │  MMWorkflow   │  main-workflow DAG                                            │
│  │   Instance  │  instance_id = "wf_inst_001"                                  │
│  └──────┬──────┘                                                                │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                                │
│  │    MMNode     │  节点执行，通过 MMDataBus 传递数据                               │
│  │ Execution   │  node_id → node_output                                        │
│  └──────┬──────┘                                                                │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                                │
│  │  MMDataBus    │  key-value 数据存储                                           │
│  │             │  "fetch_data/output" → fetched_data                           │
│  │             │  "transform_data/output" → transformed_data                   │
│  └──────┬──────┘                                                                │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                                │
│  │   Agent     │  response_queue_ ◀── 响应 Message                              │
│  │             │  response.content = "执行结果"                                │
│  └──────┬──────┘                                                                │
│         │                                                                       │
│         ▼                                                                       │
│  输出给用户 "执行结果"                                                          │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

## 8. 接口清单

### 8.1 InteractiveRunner 调用的框架接口

| 接口 | 类 | 作用 | 文件位置 |
|------|-----|------|----------|
| `MMAgent::create_from_package()` | MMAgent | 从 package 创建 MMAgent 实例 | `src/agent/agent.cpp:167` |
| `MMAgent::initialize()` | MMAgent | 初始化 MMAgent | `src/agent/agent.cpp:529` |
| `MMAgent::start()` | MMAgent | 启动 MMAgent 事件循环 | `src/agent/agent.cpp:544` |
| `MMAgent::stop()` | MMAgent | 停止 MMAgent | `src/agent/agent.cpp:571` |
| `MMAgent::route_request_message()` | MMAgent | 路由消息到 Session | `src/agent/agent.cpp:368` |
| `MMAgent::process_all_sessions_until_idle()` | MMAgent | 处理所有 Session 消息 | `src/agent/agent.cpp:397` |
| `MMAgent::pop_generated_task()` | MMAgent | 获取生成的任务 | `src/agent/agent.cpp:450` |
| `MMAgent::get_default_session()` | MMAgent | 获取默认 Session | `include/internal/agent/agent.h:86` |
| `MMAgent::get_workflow()` | MMAgent | 获取指定 MMWorkflow | `src/agent/agent.cpp:479` |
| `MMAgent::get_workflows()` | MMAgent | 获取所有 MMWorkflow | `include/internal/agent/agent.h:89` |
| `MMAgentRuntime::register_agent()` | MMAgentRuntime | 注册 MMAgent 到 Runtime | `src/runtime/runtime.cpp:171` |
| `MMAgentRuntime::start()` | MMAgentRuntime | 启动 Runtime | `src/runtime/runtime.cpp:722` |
| `MMAgentRuntime::stop()` | MMAgentRuntime | 停止 Runtime | `src/runtime/runtime.cpp:755` |
| `MMAgentRuntime::schedule_task()` | MMAgentRuntime | 调度任务执行 | `src/runtime/runtime.cpp:125` |
| `MMAgentRuntime::get_running_nodes()` | MMAgentRuntime | 获取正在执行的节点数 | `src/runtime/runtime.cpp:702` |
| `MMAgent::dequeue_response_message()` | MMAgent | 从响应队列获取消息 | `src/agent/agent.cpp` |
| `MMAgent::get_response_queue_size()` | MMAgent | 获取响应队列大小 | `src/agent/agent.cpp` |
| `MMAgentRuntime::get_running_nodes()` | MMAgentRuntime | 判断 runtime 是否仍有节点在执行 | `src/runtime/runtime.cpp` |

### 8.2 消息结构

```cpp
struct Message {
  std::string                        message_id;      // 唯一消息ID
  MessageType                        type;            // TASK_REQUEST, TASK_RESPONSE, etc.
  MessagePriority                    priority;        // LOW, NORMAL, HIGH
  std::string                        sender_id;       // 发送者ID
  std::string                        receiver_id;     // 接收者ID
  std::string                        content;         // 消息内容
  std::string                        data_ref;        // MMDataBus 数据引用
  int64_t                            timestamp;       // 时间戳
  std::map<std::string, std::string> metadata;        // 元数据 (user_id, client_id, workflow_id)
};
```

### 8.3 任务调度请求

```cpp
struct TaskScheduleRequest {
  std::string task_id;            // 任务ID
  std::string agent_object_id;    // Agent对象ID
  std::string agent_instance_id;  // Agent实例ID
  std::string workflow_id;        // MMWorkflow ID
  std::string workflow_file;      // Workflow文件路径
};

struct TaskScheduleResult {
  bool        scheduled;              // 是否成功调度
  std::string workflow_instance_id;   // Workflow实例ID
  std::string worker_id;              // 分配的Worker ID
};
```
