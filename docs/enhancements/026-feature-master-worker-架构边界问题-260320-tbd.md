## 2026-03-20 Master-Worker 架构边界问题

### 正确架构

**Master 层级**: Agent → Session → Task

**Worker 层级**: Workflow → WorkflowInstance → Node

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              Master                                      │
│                                                                         │
│  层级: Agent → Session → Task                                           │
│                                                                         │
│  组件:                                                                   │
│  ├── Deployer: Workflow 从 Master 部署到 Worker                        │
│  └── Scheduler: 通知 Worker 启动 WorkflowInstance (与 Task 对应)        │
└─────────────────────────────────────────────────────────────────────────┘
           │                              │
           │ Deployer.deploy(workflow)    │ Scheduler.schedule(task)
           ▼                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                              Worker                                      │
│                                                                         │
│  层级: Workflow → WorkflowInstance → Node                               │
│                                                                         │
│  组件:                                                                   │
│  └── Executor: 执行 Node，从 DataBus 读写数据                           │
└─────────────────────────────────────────────────────────────────────────┘
```

### 当前实现问题

MMAgentRuntime 混合了 Master 和 Worker 的职责：

**错误地放在 Master 上的 Worker 职责**：

| 职责 | 代码位置 | 问题 |
|------|---------|------|
| `process_node()` | `runtime.cpp:527` | Node 处理应该在 Worker |
| 从 Agent 读取数据注入 Node | `runtime.cpp:596-617` | 数据准备应该在 Worker |
| 设置 Node databus_client | `runtime.cpp:643-645` | 应该在 Worker |
| `cloned_workflows_` | `runtime.cpp` | Workflow 实例应该在 Worker |
| `workflow_states_` | `runtime.cpp` | 执行状态应该在 Worker |

**越界访问示例**：

```cpp
// runtime.cpp:596-617 - Master 直接访问 Agent 内部数据
void MMAgentRuntime::process_node(AgentPtr agent, ...) {
  // ❌ Master 越界访问 Agent::data_bus_
  agent->get_data(input_prefix + key, val);
  
  // ❌ Master 直接操作 Node
  node->set_inputs(hydrated);
  node->set_databus(&get_databus(agent->get_id()));
  node->set_databus_client(client);
}
```

### 正确的职责划分

| 职责 | Master | Worker |
|------|--------|--------|
| Agent 生命周期 | ✅ | ❌ |
| Session 生命周期 | ✅ | ❌ |
| Task 生命周期 | ✅ | ❌ |
| Workflow 部署 | ✅ Deployer | ❌ |
| WorkflowInstance 创建 | ❌ | ✅ |
| Node 执行 | ❌ | ✅ |
| 从 DataBus 读数据 | ❌ | ✅ |
| Node inputs 准备 | ❌ | ✅ |
| 设置 databus_client | ❌ | ✅ |
| 写结果到 DataBus | ❌ | ✅ |
| Task → WorkflowInstance 映射 | ✅ Scheduler | ✅ 执行 |

### 正确数据流

```
Master                              Worker
  │                                   │
  │ 1. Deployer.deploy(workflow)      │
  │ ─────────────────────────────►    │ Workflow 注册到 Worker
  │                                   │
  │ 2. Scheduler.schedule(task)       │
  │ ─────────────────────────────►    │ 创建 WorkflowInstance
  │   {task_id, workflow_id,          │ 关联 Master task_id
  │    session_id, dialogue_id,       │
  │    databus_ref}                   │
  │                                   │
  │                                   │ 3. Executor 执行 Node
  │                                   │    - 从 DataBus 读数据
  │                                   │    - 设置 Node inputs
  │                                   │    - 设置 databus_client
  │                                   │    - 执行 Node
  │                                   │    - 写结果到 DataBus
  │                                   │
  │ 4. Worker 报告完成                │
  │ ◄─────────────────────────────    │
```

### Task 与 WorkflowInstance 的关系

```
Master Task ◄──────────────► Worker WorkflowInstance
     │                                │
     │ task_id ←→ instance_id         │
     │                                │
     └──────── 一一对应 ──────────────┘
```

### 建议重构

**1. Master (MMAgentRuntime) 只保留调度职责**：

```cpp
class MMAgentRuntime {
  // 保留
  std::map<AgentId, AgentPtr> agents_;
  std::map<AgentId, MMDataBus*> databuses_;
  
  // 移除 (应该在 Worker)
  // std::map<std::string, WorkflowPtr> cloned_workflows_;
  // std::map<std::string, WorkflowExecutionState> workflow_states_;
  
  // 调度方法
  void schedule_task(const TaskScheduleRequest& request);
  void deploy_workflow(const WorkflowPtr& workflow, const std::string& worker_id);
};
```

**2. Worker (MMWorkerEngine) 负责 Node 执行**：

```cpp
class MMWorkerEngine {
  // 新增
  std::map<std::string, WorkflowPtr> workflows_;  // 已部署的 workflow
  std::map<std::string, WorkflowInstance> instances_;  // 运行中的实例
  
  // 从 Master 移入
  void process_node(NodePtr node, const ExecutionContext& ctx);
  void prepare_node_inputs(NodePtr node, const std::string& session_id, const std::string& dialogue_id);
  void execute_node(NodePtr node);
};
```

**3. 数据准备在 Worker 中进行**：

```cpp
// Worker 端
void MMWorkerEngine::prepare_node_inputs(NodePtr node, const std::string& session_id, ...) {
  DataMap inputs;
  
  // 从 DataBus 读取数据 (Worker 有 databus_client)
  databus_client_->sys()->get("input", val);
  databus_client_->user()->get("xxx", val);
  
  // 设置 Node inputs
  node->set_inputs(inputs);
}
```

### 实施状态

**问题识别**：✅ 已确认

**代码重构**：待实施

---
