# MMAgent 生命周期管理设计与实现

> 本文档描述 AgentFlow 中 MMAgent 实例的生命周期管理机制，包括状态定义、状态转换、自动部署和运行时监控。
> - 文档版本：v1.0
> - 更新日期：2026-02-27

---

## 1. 设计概述

### 1.1 架构位置

MMAgent 生命周期管理位于 **MMAgent Level Master** 层，由 `MMAgentActor` 和 `MMAgent` 两个核心类协同实现：

```
┌─────────────────────────────────────────────────────────────┐
│                    AGENT LEVEL MASTER                       │
│                                                             │
│  ┌─────────────────────┐      ┌─────────────────────┐      │
│  │     MMAgentActor      │      │       MMAgent         │      │
│  │   (管理多个Agent)    │─────▶│   (单个Agent实例)    │      │
│  │                     │      │                     │      │
│  │ - 包自动加载         │      │ - 生命周期状态机     │      │
│  │ - 多版本管理         │      │ - 组件初始化         │      │
│  │ - 心跳上报           │      │ - 消息处理          │      │
│  │ - 外部控制接口       │      │ - Workflow执行      │      │
│  └─────────────────────┘      └─────────────────────┘      │
│           │                            │                    │
│           └──────────  1:N  ───────────┘                    │
│              (一个Actor管理多个Agent实例)                     │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 核心职责划分

| 组件 | 职责 | 对应文件 |
|------|------|---------|
| **MMAgentActor** | MMAgent 实例的容器管理、自动加载、外部控制接口、状态上报 | `src/agent/agent_actor.cpp` |
| **MMAgent** | 单个 MMAgent 实例的生命周期状态机、组件管理、消息处理 | `src/agent/agent.cpp` |
| **AgentPackage** | MMAgent 包的加载、解析、验证 | `src/agent/package.cpp` |

---

## 2. 生命周期状态机

### 2.1 状态定义

```cpp
// include/internal/agent/common.h

enum class AgentState {
    CREATED      = 0,  // 已创建，组件未初始化
    INITIALIZED  = 1,  // 已初始化，组件就绪
    RUNNING      = 2,  // 运行中，可处理消息
    PAUSED       = 3,  // 已暂停，停止处理新消息
    STOPPED      = 4,  // 已停止，资源已释放
    FAILED       = 5   // 失败，发生不可恢复错误
};
```

### 2.2 状态转换图

```
                    ┌─────────────┐
         ┌─────────│   FAILED    │◄────────┐
         │         └─────────────┘         │
         │                ▲                │
         │                │ 初始化/运行失败  │
         ▼                │                │
    ┌─────────┐    ┌─────────────┐        │
    │ CREATED │───▶│ INITIALIZED │        │
    └─────────┘    └──────┬──────┘        │
         ▲                │               │
         │                │ start()       │
         │                ▼               │
         │           ┌─────────┐          │
         │     ┌────│ RUNNING │────┐     │
         │     │    └────┬────┘    │     │
         │     │         │         │     │
         │   pause()   resume()  stop()  │
         │     │         │         │     │
         │     ▼         │         ▼     │
         │    ┌──────┐   │      ┌──────┐ │
         └────│PAUSED│───┘      │STOPPED│─┘
              └──────┘          └──────┘
```

### 2.3 状态转换规则

| 转换 | 方法 | 条件 | 操作 |
|------|------|------|------|
| CREATED → INITIALIZED | `initialize()` | 组件初始化成功 | 初始化各组件，验证 MMWorkflow |
| INITIALIZED → RUNNING | `start()` | 当前状态为 INITIALIZED | 启动事件处理线程 |
| RUNNING → PAUSED | `pause()` | 当前状态为 RUNNING | 设置暂停标志，停止处理新消息 |
| PAUSED → RUNNING | `resume()` | 当前状态为 PAUSED | 清除暂停标志，恢复消息处理 |
| RUNNING/PAUSED → STOPPED | `stop()` | 当前状态为 RUNNING 或 PAUSED | 停止线程，释放资源 |
| * → FAILED | - | 发生不可恢复错误 | 记录错误，通知上层 |

---

## 3. 实现详解

### 3.1 MMAgent 实例创建

#### 3.1.1 从包创建（自动部署入口）

```cpp
// src/agent/agent_actor.cpp

bool MMAgentActor::create_agent_from_package(const std::string& package_path) {
    // 1. 检查文件修改时间，避免重复加载同一版本
    int64_t mtime_ms = 0;
    if (!should_reload(package_path, mtime_ms)) {
        return false;
    }

    // 2. 从包目录加载 MMAgent
    auto agent = framework::MMAgent::create_from_package(package_path);
    if (!agent) {
        return false;
    }

    // 3. 执行生命周期初始化
    agent->initialize();  // CREATED → INITIALIZED
    agent->start();       // INITIALIZED → RUNNING

    // 4. 构建 ManagedAgent 结构
    ManagedAgent managed;
    managed.package_id = agent->get_id();
    managed.version = agent->get_config().version;
    managed.package_path = package_path;
    // 实例ID格式：package_id:version:timestamp
    managed.instance_id = managed.package_id + ":" +
                         managed.version + ":" +
                         std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch().count());
    managed.agent = agent;
    managed.last_mtime_ms = mtime_ms;

    // 5. 注册到管理表
    {
        std::lock_guard<std::mutex> lock(mutex_);
        managed_agents_[managed.instance_id] = managed;
    }

    // 6. 上报 Runtime
    report_agent_registered(managed);
    report_agent_status(managed);
    return true;
}
```

#### 3.1.2 MMAgent 包加载详细流程

```cpp
// src/agent/agent.cpp

std::shared_ptr<MMAgent> MMAgent::create_from_package(const std::string& package_path) {
    // 1. 加载包配置
    auto package = AgentPackage::load_from_directory(package_path);
    if (!package) {
        return nullptr;
    }

    // 2. 构建 MMAgent 配置
    MMAgent::Config config;
    config.id = package->get_package_id();
    config.name = package->get_package_name();
    config.version = package->get_version();
    config.description = package->get_config().description;
    config.policy = MMAgent::LifecyclePolicy::PER_REQUEST;
    config.max_workflows = 10;
    config.max_concurrent_workflows = 5;

    // 3. 创建 MMAgent 实例
    auto agent = std::make_shared<MMAgent>(config);
    agent->initialize_components(package);

    // 4. 加载 MMWorkflow 定义
    auto parser = dsl::WorkflowParserFactory::create();
    for (const auto& wf_file : package->get_config().workflow_files) {
        auto result = parser->parse_workflow_file(wf_path.string());
        if (result.is_valid() && result.workflow) {
            agent->add_workflow(result.workflow);
        }
    }

    return agent;
}
```

### 3.2 组件初始化

```cpp
// src/agent/agent.cpp

bool MMAgent::initialize_components(std::shared_ptr<AgentPackage> package) {
    // 1. 默认会话组件 - 处理入站消息
    MMAgentSession::Config session_config;
    session_config.session_id = config_.id + "_default_session";
    default_session_ = std::make_shared<MMAgentSession>(session_config);
    sessions_[session_config.session_id] = default_session_;

    // 2. 任务组件 - 当前执行的任务
    MMAgentTask::Config task_config;
    task_config.task_id = config_.id + "_task";
    task_config.task_type = "default";
    current_task_ = std::make_shared<MMAgentTask>(task_config);

    // 3. 消息队列组件 - 消息收发管理
    MMAgentMsgQue::Config messages_config;
    messages_config.max_queue_size = 1024;
    messages_ = std::make_shared<MMAgentMsgQue>(messages_config);

    // 4. 协调器组件 - 意图识别、任务生成
    AgentCoordinator::Config coordinator_config;
    coordinator_config.coordinator_id = config_.id + "_coordinator";
    coordinator_ = std::make_shared<AgentCoordinator>(coordinator_config);

    // 5. 上下文组件 - 运行时数据存储
    AgentContext::Config context_config;
    context_config.context_id = config_.id + "_context";
    context_config.agent_id = config_.id;
    context_ = std::make_shared<AgentContext>(context_config);

    // 6. 事件组件 - 事件分发处理
    MMAgentEvent::Config event_config;
    event_config.handler_id = config_.id + "_event_handler";
    event_config.async_dispatch = false;
    event_ = std::make_shared<MMAgentEvent>(event_config);

    // 7. 包组件 - 包元数据管理
    if (package) {
        package_ = package;
    } else {
        AgentPackage::Config package_config;
        package_config.package_id = config_.id + "_package";
        package_config.package_name = config_.name;
        package_config.version = config_.version;
        package_ = std::make_shared<AgentPackage>(package_config);
    }

    return true;
}
```

### 3.3 生命周期方法实现

#### 3.3.1 初始化（initialize）

```cpp
bool MMAgent::initialize() {
    // 1. 初始化资源
    if (!initialize_resources()) {
        return false;
    }

    // 2. 验证所有 MMWorkflow
    for (auto& workflow : workflows_) {
        if (!workflow->validate()) {
            return false;
        }
    }

    // 3. 状态转换
    state_ = AgentState::INITIALIZED;
    return true;
}
```

#### 3.3.2 启动（start）

```cpp
bool MMAgent::start() {
    // 前置条件检查
    if (state_ != AgentState::INITIALIZED) {
        return false;
    }

    // 启动请求事件处理线程
    if (!request_event_running_.exchange(true)) {
        request_event_thread_ = std::thread(&MMAgent::request_event_loop, this);
    }

    state_ = AgentState::RUNNING;
    return true;
}

// 请求事件处理循环
void MMAgent::request_event_loop() {
    while (request_event_running_) {
        MMAgentMsgQue::Message message;
        bool has_message = false;

        // 从队列获取消息
        {
            std::unique_lock<std::mutex> lock(event_mutex_);
            request_event_cv_.wait_for(lock, std::chrono::milliseconds(100),
                [this]() { return !request_event_queue_.empty() || !request_event_running_; });

            if (!request_event_running_) break;

            if (!request_event_queue_.empty()) {
                message = request_event_queue_.front();
                request_event_queue_.pop();
                has_message = true;
            }
        }

        if (has_message) {
            handle_request_event(message);
            process_all_sessions_until_idle();
        }
    }
}
```

#### 3.3.3 暂停与恢复

```cpp
bool MMAgent::pause() {
    if (state_ != AgentState::RUNNING) {
        return false;
    }
    state_ = AgentState::PAUSED;
    return true;
}

bool MMAgent::resume() {
    if (state_ != AgentState::PAUSED) {
        return false;
    }
    state_ = AgentState::RUNNING;
    return true;
}

// 消息处理中的暂停检查
void MMAgent::handle_request_event(const MMAgentMsgQue::Message& message) {
    // 如果处于 PAUSED 状态，可以选择丢弃或缓存消息
    if (state_ == AgentState::PAUSED) {
        // 实现策略：缓存消息待恢复后处理
        return;
    }
    // ... 正常处理
}
```

#### 3.3.4 停止（stop）

```cpp
bool MMAgent::stop() {
    // 前置条件检查
    if (state_ != AgentState::RUNNING && state_ != AgentState::PAUSED) {
        return false;
    }

    // 1. 停止事件处理线程
    request_event_running_ = false;
    request_event_cv_.notify_all();

    if (request_event_thread_.joinable()) {
        request_event_thread_.join();
    }

    // 2. 清理资源
    cleanup_resources();

    // 3. 状态转换
    state_ = AgentState::STOPPED;
    return true;
}

void MMAgent::cleanup_resources() {
    // 清理数据总线
    data_bus_.clear();

    // 清空事件队列
    while (!event_queue_.empty()) {
        event_queue_.pop();
    }
}
```

### 3.4 自动部署与热加载

#### 3.4.1 配置格式

```json
// AgentService.config
{
    "package_root": "/opt/agentflow/agents",
    "manifests": [
        "customer_service/package.json",
        "data_analysis/package.json"
    ],
    "scan_interval_ms": 5000,
    "heartbeat_interval_ms": 3000
}
```

#### 3.4.2 包监控循环

```cpp
void MMAgentActor::monitor_packages_loop() {
    while (running_) {
        // 1. 重新解析配置（支持配置热更新）
        parse_service_config();

        // 2. 加载配置的 MMAgent 包
        load_agents_from_config();

        // 3. 收集并上报生成的任务
        collect_agent_tasks_and_report();

        // 4. 等待下一次扫描
        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                std::max(100, service_config_.scan_interval_ms)
            )
        );
    }
}
```

#### 3.4.3 版本管理

```cpp
// 实例ID格式：package_id:version:timestamp
// 示例："CustomerService:1.2.0:1709011200000"

bool MMAgentActor::should_reload(const std::string& package_path,
                                int64_t& out_mtime_ms) const {
    // 获取 package.json 修改时间
    out_mtime_ms = file_mtime_ms(package_path + "/package.json");
    if (out_mtime_ms == 0) {
        return false;
    }

    // 检查是否已加载相同版本
    for (const auto& kv : managed_agents_) {
        if (kv.second.package_path == package_path &&
            kv.second.last_mtime_ms == out_mtime_ms) {
            return false;  // 已是最新版本，无需重载
        }
    }
    return true;  // 需要加载新版本
}
```

### 3.5 外部控制接口

#### 3.5.1 控制消息类型

```cpp
void MMAgentActor::on_message(std::shared_ptr<actor::adapter::IContext> ctx) {
    auto msg = ctx->current_message();

    // MMAgent 生命周期控制
    if (msg.message_type == "agent.start") {
        apply_agent_control("start", instance_id);
    } else if (msg.message_type == "agent.pause") {
        apply_agent_control("pause", instance_id);
    } else if (msg.message_type == "agent.resume") {
        apply_agent_control("resume", instance_id);
    } else if (msg.message_type == "agent.stop") {
        apply_agent_control("stop", instance_id);
    }

    // Task 生命周期控制
    else if (msg.message_type == "task.start") {
        apply_task_control("start", task_id);
    } else if (msg.message_type == "task.pause") {
        apply_task_control("pause", task_id);
    } else if (msg.message_type == "task.stop") {
        apply_task_control("stop", task_id);
    }

    // 请求消息路由
    else if (msg.message_type == "request.message") {
        route_request_message(*payload);
    }
}
```

#### 3.5.2 控制命令执行

```cpp
void MMAgentActor::apply_agent_control(const std::string& action,
                                      const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& kv : managed_agents_) {
        // 如果指定了 instance_id，只操作匹配的实例
        if (!instance_id.empty() && kv.first != instance_id) {
            continue;
        }

        if (!kv.second.agent) continue;

        // 执行生命周期操作
        if (action == "start") {
            kv.second.agent->start();
        } else if (action == "pause") {
            kv.second.agent->pause();
        } else if (action == "resume") {
            kv.second.agent->resume();
        } else if (action == "stop") {
            kv.second.agent->stop();
        }

        // 上报状态变更
        report_agent_status(kv.second);
    }
}
```

---

## 4. 时序图

### 4.1 MMAgent 实例创建与启动

```mermaid
sequenceDiagram
    participant Config as AgentService.config
    participant Actor as MMAgentActor
    participant Package as AgentPackage
    participant MMAgent as MMAgent
    participant Runtime as MMRuntimeActor

    Actor->>Config: 读取配置
    Actor->>Actor: parse_service_config()

    loop 每个配置的包
        Actor->>Package: load_from_directory(path)
        Package-->>Actor: package

        Actor->>Actor: should_reload() 检查版本

        alt 是新版本
            Actor->>MMAgent: create_from_package()
            MMAgent->>MMAgent: initialize_components()
            MMAgent->>MMAgent: initialize()
            Note over MMAgent: CREATED → INITIALIZED

            MMAgent->>MMAgent: start()
            Note over MMAgent: INITIALIZED → RUNNING

            Actor->>Actor: 注册到 managed_agents_
            Actor->>Runtime: report_agent_registered()
            Actor->>Runtime: report_agent_status()
        end
    end

    Actor->>Actor: 启动 monitor_packages_loop()
    Actor->>Actor: 启动 heartbeat_loop()
```

### 4.2 生命周期状态转换

```mermaid
sequenceDiagram
    participant Client as 外部控制
    participant Actor as MMAgentActor
    participant MMAgent as MMAgent

    %% 启动流程
    Client->>Actor: agent.start 消息
    Actor->>Actor: apply_agent_control("start")
    Actor->>MMAgent: start()

    alt 状态为 INITIALIZED
        MMAgent->>MMAgent: 启动 request_event_thread_
        MMAgent->>MMAgent: state_ = RUNNING
        MMAgent-->>Actor: true
    else 状态错误
        MMAgent-->>Actor: false
    end

    Actor->>Runtime: report_agent_status()

    %% 暂停流程
    Client->>Actor: agent.pause 消息
    Actor->>MMAgent: pause()

    alt 状态为 RUNNING
        MMAgent->>MMAgent: state_ = PAUSED
        MMAgent-->>Actor: true
    else 状态错误
        MMAgent-->>Actor: false
    end

    Actor->>Runtime: report_agent_status()

    %% 恢复流程
    Client->>Actor: agent.resume 消息
    Actor->>MMAgent: resume()

    alt 状态为 PAUSED
        MMAgent->>MMAgent: state_ = RUNNING
        MMAgent-->>Actor: true
    else 状态错误
        MMAgent-->>Actor: false
    end

    Actor->>Runtime: report_agent_status()

    %% 停止流程
    Client->>Actor: agent.stop 消息
    Actor->>MMAgent: stop()

    alt 状态为 RUNNING 或 PAUSED
        MMAgent->>MMAgent: request_event_running_ = false
        MMAgent->>MMAgent: join(request_event_thread_)
        MMAgent->>MMAgent: cleanup_resources()
        MMAgent->>MMAgent: state_ = STOPPED
        MMAgent-->>Actor: true
    else 状态错误
        MMAgent-->>Actor: false
    end

    Actor->>Runtime: report_agent_status()
```

### 4.3 心跳与状态监控

```mermaid
sequenceDiagram
    participant Monitor as monitor_packages_loop
    participant Heartbeat as heartbeat_loop
    participant Actor as MMAgentActor
    participant Agents as ManagedAgents
    participant Runtime as MMRuntimeActor

    par 包监控线程
        loop 定期执行 (scan_interval_ms)
            Monitor->>Actor: parse_service_config()
            Monitor->>Actor: load_agents_from_config()
            Monitor->>Actor: collect_agent_tasks_and_report()
            Monitor->>Monitor: sleep(scan_interval_ms)
        end
    and 心跳线程
        loop 定期执行 (heartbeat_interval_ms)
            Heartbeat->>Agents: 获取所有 MMAgent 快照

            loop 每个 MMAgent
                Heartbeat->>Runtime: report_agent_heartbeat()
                Heartbeat->>Runtime: report_agent_status()
            end

            Heartbeat->>Heartbeat: sleep(heartbeat_interval_ms)
        end
    end
```

---

## 5. 关键代码清单

| 功能 | 文件路径 | 关键类/方法 |
|------|---------|------------|
| 生命周期状态定义 | `include/internal/agent/common.h` | `enum class AgentState` |
| MMAgent 实例管理 | `include/internal/agent/agent_actor.h` | `MMAgentActor` |
| MMAgent 实例 | `include/internal/agent/agent.h` | `class MMAgent` |
| 包加载实现 | `src/agent/agent_actor.cpp` | `create_agent_from_package()` |
| 自动部署循环 | `src/agent/agent_actor.cpp` | `monitor_packages_loop()` |
| 心跳上报 | `src/agent/agent_actor.cpp` | `heartbeat_loop()` |
| 生命周期方法 | `src/agent/agent.cpp` | `initialize/start/pause/resume/stop()` |
| 组件初始化 | `src/agent/agent.cpp` | `initialize_components()` |
| 外部控制接口 | `src/agent/agent_actor.cpp` | `apply_agent_control()` |
| 状态上报 | `src/agent/agent_actor.cpp` | `report_agent_*()` |

---

## 6. 配置参考

### 6.1 AgentService.config

```json
{
    "package_root": "/opt/agentflow/agents",
    "manifests": [
        "agent1/package.json",
        "agent2/package.json"
    ],
    "scan_interval_ms": 5000,
    "heartbeat_interval_ms": 3000
}
```

### 6.2 MMAgent 配置（agent.json）

```json
{
    "id": "customer_service_agent",
    "name": "Customer Service MMAgent",
    "version": "1.2.0",
    "description": "Handles customer inquiries",
    "policy": 0,
    "max_workflows": 10,
    "max_concurrent_workflows": 5,
    "data_bus_capacity": 1048576,
    "precreate_session_count": 3
}
```

- `precreate_session_count`：初始化默认 session 后，额外预创建的 session 数量（默认 `0`）。

---

## 7. 注意事项

1. **线程安全**：所有状态转换操作都有锁保护，`managed_agents_` 访问需要加锁
2. **资源释放**：`stop()` 方法必须等待事件处理线程结束后再返回
3. **状态检查**：每个生命周期方法都会检查前置状态，非法状态转换会返回 false
4. **多版本支持**：相同 MMAgent 的不同版本可以同时运行，通过 `instance_id` 区分
5. **故障处理**：当前实现中 FAILED 状态主要用于错误标识，需要上层进行恢复处理

### 7.1 Session 消息类型队列与任务分工（2026-03）

- Session 接收队列由优先级模型调整为消息类型模型：
  - `intent_request`
  - `output_result`
  - `extensible_reserved`
- 消息根据消息头 `metadata["message_type"]` 路由；缺失该头时默认按 `intent_request` 处理。
- 每个 session 默认创建两个固定任务：
  - `task0`：dispatcher（优先 `dispatcher_workflow`）
  - `task1`：coordinator（优先 `coordinator_workflow`）
- 默认 session、预创建 session、动态 session 均采用同一 task0/task1 初始化策略。

---

*文档生成时间：2026-02-27*
