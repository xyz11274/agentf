## 2026-03-07 examples 目录结构整理

### 背景

`examples/` 根目录下存在多个松散的 `.cpp/.h/.md` 文件，`examples/agent/` 下存在不属于任何 agent 示例的零散 workflow JSON 和空包目录，影响目录可读性。

### 变更内容

#### 1. 新建 `examples/features/` — 单点功能 C++ 示例归集

将以下文件从 `examples/` 根目录移入 `examples/features/`：

| 文件 | 用途 |
|---|---|
| `adapter_example.cpp` | Actor 适配器调用示例 |
| `databus_example.cpp` | MMDataBus 读写/订阅示例 |
| `metrics_example.cpp` | 指标采集导出示例 |
| `storage_example.cpp` | 存储读写示例 |
| `agent_runner.cpp` | Agent 包加载运行工具主体 |
| `interactive_runner.cpp` / `interactive_runner.h` | 交互式运行器实现 |
| `agent_runner_design.md` | Agent Runner 设计文档 |
| `interactive_runner_demo.md` | 交互式运行器演示文档 |

#### 2. 新建 `examples/agent/workflow-patterns-agent/` — 零散 workflow JSON 归集

将 `examples/agent/` 下的零散 workflow 模式文件归集到新 agent 示例目录：

| 文件 | 原位置 |
|---|---|
| `conditional_workflow.json` | `examples/agent/conditional_workflow.json` |
| `loop_workflow.json` | `examples/agent/loop_workflow.json` |
| `sample_workflow.json` | `examples/agent/sample_workflow.json` |

新增目录文件：`agent.json`（包含3个 workflow 的 agent 配置）、`package.json`（合并原 `sample_package/package.json` 内容）、`agentCard.md`。

删除 `examples/agent/sample_package/`（原仅含孤立 package.json，内容已并入 workflow_patterns）。

#### 3. CMakeLists.txt 更新

`example_adapter`、`example_metrics`、`example_databus`、`example_storage`、`agent_runner` 的源文件路径从 `${AGENTFLOW_EXAMPLE_DIR}/` 更新为 `${AGENTFLOW_EXAMPLE_DIR}/features/`。

### 测试兼容性

E2E 测试引用的 `examples/agent/basic-agent` 和 `examples/agent/nested-workflow-agent` 路径**保持不变**，无需修改测试代码。

### 验证结果

- `./build.sh build`：通过
- `ctest --test-dir build -R AgentFlowGoogleTests`：通过



### 背景

在多语言节点重构的基础上，进一步完成三项深度重构：
1. 清理 `.bak` 等无用文件；
2. workflow 节点在运行时通过 agent msgque 消息驱动，节点间通过 DataBus 共享数据（取代文件 I/O）；
3. 默认 session coordinator 在处理其他 session 任务结果时实现真实的任务编排和启停控制。

### 框架增强（src/runtime/runtime.cpp + include/internal/runtime/agent_runtime.h）

#### 1. DataBus 输入注水（Input Hydration）

- **`src/runtime/runtime.cpp`**：在 `process_node()` 内 `node->execute()` 调用前，新增 DataBus 注水块：
  - 遍历节点声明的 `input_params`（`node->get_input_params()`）；
  - 对每个键调用 `bus.get(key, value)` 从 agent DataBus 读取值；
  - 将 DataBus 中的值 merge 到节点 `inputs_` 中（现有节点显式输入优先级更高的键不被覆盖）；
  - 保证节点不再需要通过文件读取上游节点的输出，DataBus 自动作为节点间共享通道。

#### 2. Agent 任务轮询线程（`agent_task_poller_loop`）

- **`include/internal/runtime/agent_runtime.h`**：新增 `agent_task_poller_loop()` 私有方法声明和 `agent_task_poller_thread_` 成员。
- **`src/runtime/runtime.cpp`**：
  - 实现 `agent_task_poller_loop()`：每 20ms 遍历所有 agent，调用 `agent->pop_generated_task()`；
    对取出的每个任务提取 `workflow_id` 和 `message_content`，调用 `execute_workflow(agent_id, workflow_id, inputs)` 分派到 worker 线程；
  - `start()` 中启动该线程；
  - `stop()` 中 join 该线程。

### 示例层重构（examples/agent/multi_session_chat/）

#### 1. 清理无用文件

删除以下文件（`.bak` 备份和已废弃脚本）：
- `nodes/go/session_context/main.go.bak`
- `nodes/python/skill_date_node.py.bak`
- `package/scripts/apply_task_plan.sh`（原 shell 版占位符）
- `package/scripts/collect_selected_skills.sh`
- `package/scripts/create_skill_task.sh`
- `package/scripts/default_dispatch_broadcast.sh`
- `package/scripts/execute_selected_skills.sh`
- `package/scripts/run_full_pipeline.sh`
- `package/scripts/session_select_skills.sh`

#### 2. ChatAgentBridge：msgque 消息驱动 + 多阶段编排

**`examples/features/chat_bridge_support.h`**（由 `multi_session_chat_tui.cpp` / `openjiuwen_multi_session_chat_tui.cpp` 复用）
— `execute()` 方法全面重构为五阶段流水线：

**Phase 1 — 消息路由（msgque）**
- 从 `payload` 解析 `user_text` 和 `client_session_id`；
- 构造 `MMAgentMsgQue::Message`（`type=TASK_REQUEST`, `metadata["task_type"]="task0"`）；
- 调用 `agent_->route_request_message(msg)` 将用户输入路由到对应 session；
- 调用 `agent_->process_all_sessions_until_idle()` 驱动 session 状态机，生成 dispatcher 任务到 `generated_tasks_`；
- 后台 `agent_task_poller_loop()` 线程负责消费任务，调用 `execute_workflow("default-dispatcher-workflow", inputs)`。

**Phase 2 — 等待 dispatcher 完成**
- 轮询 DataBus 的 `dispatcher_done` key；
- 设有 `CHAT_WORKFLOW_TIMEOUT_SEC` 环境变量可覆盖最大等待时间（默认 45s）；
- 若 dispatcher 直接写入 `final_response`（legacy 模式），立即返回（向后兼容）。

**Phase 3 — Coordinator 编排**
- 调用 `runtime_->execute_workflow(agent_id, "session-coordinator-workflow", {})` 驱动三节点协调器（collect → plan → apply）；
- 等待 `orchestration_plan` 出现在 DataBus（最长 15s）；
- `orchestration_plan` 是 JSON 数组，每个条目含 `action`、`skill`、`session_id` 字段。

**Phase 4 — 技能工作流串行执行**
- 解析 `orchestration_plan` JSON，筛选 `action="start"` 的条目；
- 根据 `skill` 字段映射到对应技能工作流（`skill-search-workflow`/`skill-summary-workflow`/`skill-date-workflow`/`skill-delegate-workflow`）；
- 为每个技能调用 `runtime_->execute_workflow(agent_id, wf_id, skill_inputs)`（串行执行，避免共享 workflow 对象并发竞争）；
- 每个技能输出写入 DataBus key `session:<sid>:skill_result`。

**Phase 5 — 聚合结果**
- 等待各 `session:<sid>:skill_result` 出现（最长 30s）；
- 拼接多技能结果，写入 DataBus `final_response` 并返回给客户端。

#### 3. node_pipeline 纯化为分派器

**`nodes/cpp/multi_session_nodes.cpp`** — `node_pipeline` 重构：
- **去除**所有技能执行逻辑（不再调用 `skill_search.sh`/`skill_summary.sh`/`compose_final_response.sh`）；
- 仅做：用户输入预处理 → 每 session 调用 `select_skill.sh` 选择技能 → 将选择结果写入 DataBus outputs（`session:<sid>:skill_selected`）；
- 输出 `dispatcher_done=1` 作为 Phase 2 的完成信号；
- 输出 `selected_skills` JSONL 供 `node_collect_skills` 读取。

#### 4. node_collect_skills DataBus 优先

**`nodes/cpp/multi_session_nodes.cpp`** — `node_collect_skills` 重构三级降级策略：
1. **DataBus 直接传入**：若 inputs 中有 `selected_skills`（由 `node_pipeline` 输出并注入），直接复用；
2. **DataBus 逐 session 键**：遍历 `kSessionIds`，从 `inputs["session:<sid>:skill_selected"]` 读取；
3. **文件降级**：读取 `runtime/<sid>.skill.json` 文件（保持向后兼容）。

### DataBus 键约定（更新）

| 键名 | 写入节点 | 读取方 |
|---|---|---|
| `user_text` | ChatAgentBridge | node_pipeline |
| `dispatcher_done` | `node_pipeline` (outputs) | ChatAgentBridge Phase 2 |
| `session:<sid>:skill_selected` | `node_pipeline` (outputs) | `node_collect_skills` |
| `selected_skills` | `node_collect_skills` (outputs) | `node_plan_dependencies` |
| `task_plan` | `node_plan_dependencies` (outputs) | `node_apply_task_plan` |
| `orchestration_plan` | `node_apply_task_plan` (outputs) | ChatAgentBridge Phase 3 |
| `session:<sid>:skill_result` | skill workflows | ChatAgentBridge Phase 5 |
| `final_response` | ChatAgentBridge Phase 5 | HTTP/WS 响应 |

### 验证结果

- `./build.sh build`：通过（`libmulti_session_nodes.dylib` + 多个示例 TUI 编译链接成功）。
- `ctest --test-dir build -R AgentFlowGoogleTests`：通过（3.30s）。
- `E2EBasicAgent` 超时是存量问题（重构前即已超时，与本次改动无关）。
