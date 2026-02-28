# AgentFlow API Reference

> 本文档按“Framework（Agent/Workflow）- Runtime（调度）- DataBus（共享总线）”组织。

## 1. Framework 层

### 1.1 Agent API

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
- 控制面协同：`msgque/event` 仅传 `data_ref`，payload 通过 DataBus 获取

关联规范：
- Agent 包结构：`publish/agent/AGENT_PACKAGE_SPEC.md`
- 打包脚本：`publish/agent_build.sh`

### 1.2 Workflow API

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
- `edge.control.type`：`if/loop/fork/join/async/goto`。
- `includes[]`：引用外部 workflow 片段。
- `node.type=workflow + workflow_ref`：嵌套 workflow。

参考：`publish/workflow/README.md`、`publish/workflow/pattern/*.json`。

### 1.3 Node 开发 API

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
- 输入输出：`input_params/output_params`
- 执行入口：`command/function_name/main_class/args`
- 可靠性：`timeout_ms/max_retries/retry_delay_ms`
- 热更新：`enable_hot_reload/hot_reload_config`
- 嵌套 workflow 引用：`workflow_ref`（当 `type=workflow`）

Node 规范与模板：
- `publish/workflow/node_patterns/`
- `src/workflow/node/skill.md`

### 1.4 Edge API

头文件：`include/external/workflow/edge.h`

核心能力：
- 数据映射：`source_outputs -> target_inputs`
- 传输模式：`SHARED_MEMORY/MESSAGE_QUEUE/REMOTE_TRANSFER`
- 控制流：`control.type` 及附加参数（如 `expression/goto_target/loop_id/fork_group/join_group/callback_node`）

---

## 2. Runtime 层

### 2.1 AgentRuntime

头文件：`include/internal/runtime/agent_runtime.h`  
命名空间：`agentflow::runtime`

运行角色：
- `RuntimeRole::MASTER`
- `RuntimeRole::WORKER`

关键接口：
- Agent 注册：`register_agent/unregister_agent/list_agents`
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
- 通过 `WorkerEngine::execute_code` 统一执行节点多语言任务。
- 上报执行状态。
- 配合 runtime 推进 workflow 状态机。

---

## 3. 高速数据总线（DataBus）

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
- 统一封装类型：`DataBus`（兼容别名 `HighSpeedDataBus`）

使用建议：
- 统一 key 命名，避免 workflow 间冲突。
- 对大对象优先使用可复用键并控制生命周期。
- 在控制流条件读取与节点输出写入之间保持键语义一致。
- 首条请求原始数据应先落本地缓存并生成 `data_ref`，再由 DataBus 自动同步远端服务。
