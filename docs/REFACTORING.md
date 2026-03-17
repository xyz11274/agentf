# 项目目录结构优化说明

## 2026-03-18 Workflow Input Dialogue 级别隔离

### 背景

当多个 dialogue 在同一 session 内并行执行时（如多轮对话、并发 skill 任务），workflow inputs 存储在 agent DataBus 中没有 dialogue_id 前缀，导致不同 dialogue 的输入数据可能相互覆盖。

### 问题分析

**第三层映射（Workflow Inputs → Node Inputs）**：
- `execute_workflow()` 将 inputs 存入 agent DataBus
- `process_node()` 从 agent DataBus 读取 inputs 注入节点
- 原实现：`agent->publish_data(key, value)` 无前缀
- 并发场景：dialogue A 的 `user_text` 可能被 dialogue B 的 `user_text` 覆盖

### 解决方案

在 workflow inputs 存取时增加 `{dialogue_id}/` 前缀：

| 操作 | 原实现 | 新实现 |
|------|--------|--------|
| 存储 | `key` | `{dialogue_id}/key` |
| 读取 | `key` | `{dialogue_id}/key`，失败则 fallback 到 `key` |

**代码变更**（`src/runtime/runtime.cpp`）：

```cpp
// execute_workflow(): 存储时添加前缀
std::string input_prefix = dialogue_id.empty() ? "" : dialogue_id + "/";
for(const auto& pair : inputs) {
  agent->publish_data(input_prefix + pair.first, pair.second);
}

// process_node(): 读取时添加前缀，失败则 fallback
std::string input_prefix = workflow_dialogue_id.empty() ? "" : workflow_dialogue_id + "/";
if(agent->get_data(input_prefix + key, val)) {
  hydrated[key] = val;
} else if(agent->get_data(key, val)) {
  // Fallback: backward compatibility
  hydrated[key] = val;
}
```

### 向后兼容

- `dialogue_id` 为空时不添加前缀
- 读取时 fallback 到无前缀键
- 现有代码无需修改

### 文档更新

- `docs/databus-session-isolation.md`：新增 Dialogue-Level Isolation 章节

### 验证结果

- `./build.sh build`：通过
- `./build.sh test`：19/19 测试通过

## 2026-03-07 src/agent session/task 配置模型重构

> **已废弃**：此配置模型已被 2026-03-17 的扁平化结构取代。参见下方章节。

### 背景

`src/agent` 原先依赖隐式约定：
- `agent.json` 只覆盖少量字段；
- 默认 0 号 session 之外的 session 主要通过运行时动态创建；
- session 内部固定使用 `task0` / `task1` 表示 dispatcher / coordinator；
- context workflow 通过旧的 context task 语义分散存在，skill 任务也缺乏实例池配置。

这使得多 session 协作、skill 扩容、context 任务演进都不够直观。

### 本次重构（已被扁平化结构取代）

- `agent.json` 改为 `session_pool_size + tasks + sessions` 组合：
  - `tasks[]` 中使用 `contextupdater` 声明 agent 级上下文更新任务；
  - `sessions[]` 固定只保留 1 份同质化 session 模板，由 `session_pool_size` 复制出整个 session 池；
  - session 只声明 `dispatcher` / `coordinator` / `skill` 三类任务，`skill.instance_pool_size` 默认值为 `2`，`task.workflow_pool_size` 默认值为 `1`。
- `SessionRouter` 统一维护默认请求接收队列、响应发送队列、路由线程和 aging 线程；消息按 `user_id + client_id` 命中或分配预创建 session。
- `skill` 任务新增 `skill.name`、`skill.config_dir`、`skill.instance_pool_size`，初始化时按配置创建任务实例池；所有 task 统一支持 `workflow_pool_size`，用于声明对应 workflow 的多实例池规模，runtime 启动时会把这些 workflow 实例池预注册到 deployer / scheduler。
- `MMAgent::initialize_components()` 改为优先按 `agent.json.sessions` 预创建 session pool；请求先进入 router，再映射到具体 session。
- examples / publish / tests 已同步切换到新 schema。

### 验证结果

- `./build.sh build`：通过
- `ctest --test-dir build -R 'E2EMessageRouting|E2EBasicAgent' --output-on-failure`：通过
- `./build.sh test`：在重构过程中发现并修复兼容性问题，最终全量验证通过

本文档说明了 AgentFlow 项目目录结构的优化工作。

## 2026-03-07 agent/runtime 双队列与执行风险等级重构

### 本次重构

- agent 侧：
  - session 删除自建收发队列，消息统一下沉到 dispatcher/coordinator task；
  - 每个 task 拥有 `intent_request` / `data_exchange` 双队列，以及超时监控/重发线程；
  - `SessionRouter` 负责默认请求接收、响应发送、路由线程和 session 老化线程。
- runtime 侧：
  - worker 改为 `control` / `business` 双队列模型；
  - workflow / node / edge 不再保留独立事件线程，执行控制统一收敛到 worker；
  - runtime 维护 `task -> workflow instance -> worker` 映射，并将 worker 回包回流到 task 数据队列。
- workflow 侧：
  - node 新增 `execution_risk_level`（`green` / `yellow` / `red`）；
  - workflow 在解析时聚合红灯节点，并暴露 `is_red_light()` 供 coordinator 判别 skill 是否可直接执行。
- tests/docs/examples：
  - 新增 workflow risk-level 解析测试；
  - 更新 multi-session-chat 文档与 package 说明，明确 session 池、skill 实例池和 task 双队列模型；
  - 清理示例目录遗留的 `.bak` 脚本文件。

### 验证结果

- `./build.sh build`：通过
- `ctest --test-dir build -R 'AgentTests|E2EMessageRouting' --output-on-failure`：通过
- `./build.sh test`：通过

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

#### 2. 新建 `examples/agent/workflow_patterns/` — 零散 workflow JSON 归集

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

E2E 测试引用的 `examples/agent/basic_agent` 和 `examples/agent/nested_workflow_agent` 路径**保持不变**，无需修改测试代码。

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

## 2026-03-06 multi_session_chat 多语言节点重构

### 背景

将 `examples/agent/multi_session_chat` 示例从纯 shell_script 节点重构为多语言节点，同时为框架补充跨 session 查询 API、结构化脚本输出协议和节点运行时自动注册等能力。

### 框架增强（src/ + include/）

#### 1. 跨 session DataBus 查询（`fw-cross-session-api`）

- **`include/internal/agent/agent.h`**：新增 `get_all_session_ids()` 和 `get_session_output(session_id, key, value)` 声明。
- **`src/agent/agent.cpp`**：实现两个方法；DataBus 键格式为 `session:<session_id>:<key>`。
- **`include/internal/agent/coordinator.h`**：新增 `aggregate_session_results(MMAgent&, vector<string>)` 声明。
- **`src/agent/coordinator.cpp`**：实现 `aggregate_session_results()`，遍历所有 session 批量读取指定键。

#### 2. 脚本节点 JSON 输出协议（`fw-script-json-output`）

- **`include/external/workflow/node.h`** 和 **`include/internal/workflow/node.h`**：  
  `MMNode::Config` 新增 `bool json_output = false`。
- **`src/workflow/node/node.cpp`**：  
  - 新增纯 C++ 实现的 `parse_flat_json_object()` 辅助函数（不依赖 RapidJSON，避免 macOS SDK 宏冲突）。
  - 执行脚本前注入 `AF_OUTPUT_FORMAT=json` 环境变量（当 `json_output=true`）。
  - 脚本执行成功后将 stdout JSON 的每个键写入 DataBus `outputs_`。
- **`src/workflow/dsl/workflow_parser.cpp`**：解析 `"json_output"` 布尔字段。

#### 3. 节点运行时自动注册（`fw-node-runtime-autostart`）

- **`include/external/workflow/node.h`** 和 **`include/internal/workflow/node.h`**：  
  `MMNode::Config` 新增 `bool auto_start_runtime = false` 和 `RuntimeConfig node_runtime_config`（`RuntimeConfig` 结构体前置声明）。
- **`src/workflow/workflow.cpp`**：  
  - `start_runtime()`：遍历节点，若 `auto_start_runtime=true` 则自动启动 `node_runtime_config` 指定的运行器。
  - `stop_runtime()`：对应自动停止。
- **`src/workflow/dsl/workflow_parser.cpp`**：解析 `"node_runtime"` 对象字段。

### 示例多语言节点（examples/agent/multi_session_chat/nodes/）

| 目录 | 语言 | 对应 workflow | 执行模式 |
|---|---|---|---|
| `nodes/cpp/multi_session_nodes.cpp` | **C++** | dispatcher/coordinator/session-dispatcher | `so_library` |
| `nodes/go/session_context/main.go` | **Go** | session-context-workflow | `go_script` |
| `nodes/python/skill_date_node.py` | **Python** | skill-date-workflow | `python_script` |
| `nodes/java/SkillDelegateNode.java` | **Java** | skill-delegate-workflow | `java_jar` |

**C++ SO 库节点**（`build/libmulti_session_nodes.dylib`）导出 7 个函数：
`node_pipeline`, `node_collect_skills`, `node_plan_dependencies`, `node_apply_task_plan`,
`node_load_context`, `node_select_skill`, `node_create_task`。  
其中 `node_apply_task_plan` 实现任务依赖图编排：解析 `task_plan` → 读取持久化状态 → 作 start/waiting/skip/stop 决策 → 输出 `orchestration_plan`。

**Go 节点**（`nodes/go/session_context/main.go`）通过 CLI arg 分派（`go run main.go <node_name>`），以 `json_output` 协议将结果写入 DataBus。

**Python 节点**（`nodes/python/skill_date_node.py`）内联完整中文日期 NLP 解析，无外部依赖。

**Java 节点**（`nodes/java/SkillDelegateNode.java`）内置 JSON 序列化，构建脚本：`nodes/java/build.sh`。

### Workflow JSON 更新（`examples/agent/multi_session_chat/package/`）

6 个 workflow JSON 全部从 `shell_script` 切换到对应多语言执行模式，新增 `inputs`/`outputs` DataBus 字段。

### CMakeLists.txt 更新

新增 `multi_session_nodes` shared library 目标：
```cmake
add_library(multi_session_nodes SHARED
    ${AGENTFLOW_EXAMPLE_DIR}/multi_session_chat/nodes/cpp/multi_session_nodes.cpp)
target_link_libraries(multi_session_nodes PRIVATE agentflow)
```

### .gitignore 更新

新增排除规则：
- `examples/agent/multi_session_chat/nodes/java/*.class`
- `examples/agent/multi_session_chat/nodes/java/skill_delegate.jar`
- `examples/agent/multi_session_chat/nodes/python/__pycache__/`
- `examples/agent/multi_session_chat/package/runtime/`

### 文档更新

- `docs/multi-session-chat-example.md`：全面重写，含多语言架构表、DataBus 键约定、编排逻辑。
- `docs/api-reference.md`：MMAgent API 补充跨 session 查询方法；MMNode 配置补充 `json_output` 和 `node_runtime` 字段。
- `examples/agent/multi_session_chat/README.md`：更新目录结构与快速启动说明。

### 验证结果

- `./build.sh build`：通过（`libmulti_session_nodes.dylib` 成功编译链接）。
- `ctest --test-dir build -R "AgentFlowGoogleTests|NodeTests|AdapterIsolationTests"`：3/3 通过。



- `src/agent/agent_actor.cpp` 重构为自动化实例管理能力：
  - 从 `AgentService.config`（JSON）读取 `package_root` 与 `manifests`；
  - 自动加载 agent 包并创建 MMAgent 实例；
  - 启动包监控线程，目录变化时增量加载新版本实例，支持多版本并行运行；
  - 维护 agent/task 实例清单，并提供 `agent.start/pause/resume/stop`、`task.start/pause/stop` 控制消息入口。
- 按要求将请求事件处理能力下沉到 `MMAgent`：
  - `include/external/agent/agent.h`、`src/agent/agent.cpp` 新增请求事件队列与监控线程；
  - 新请求入队后触发 MMAgent 内 `AgentCoordinator` 对默认会话及其他会话收发队列进行扫描处理，直到队列清空；
  - 处理生成的 task 通过 MMAgent 接口回传给 MMAgentActor 统一管理与上报。
- 新增独立 `MMRuntimeActor`：
  - 新增 `include/internal/runtime/runtime_actor.h`；
  - `src/runtime/runtime_actor.cpp` 新增 MMRuntimeActor 消息处理，接收 agent/task/worker 的注册、心跳、状态上报并对接 `MMAgentRuntime`。
- Worker 目录与 actor 化能力增强：
  - `src/runtime/worker.cpp` 迁移为 `src/runtime/worker/worker.cpp`；
  - 新增 `include/internal/runtime/worker/worker_actor.h` 与 `src/runtime/worker/worker_actor.cpp`；
  - WorkerActor 负责 worker 注册、心跳、状态同步消息上报到 MMRuntimeActor。
- 构建与测试验证：
  - `./build.sh build` 通过；
  - `./build.sh test` 通过（10/10）。

## MMNode 执行引擎下沉 + Coordinator Task0 默认化（2026-02-28）

- MMNode 多语言执行引擎下沉到 Worker：
  - `include/internal/runtime/worker.h` 增加 Worker 执行请求/结果模型与 `execute_code` 入口；
  - `src/runtime/worker/worker.cpp` 实现 shell/python/go/jar/so/executable 分发执行；
  - `src/workflow/node/node.cpp` 的外部执行模式统一改为委托 worker 执行，MMNode 保留状态机与输入输出管理。
- Coordinator 默认 task 语义统一为 task0：
  - `include/internal/agent/coordinator.h` / `src/agent/coordinator.cpp` 增加默认 task 类型配置，缺省改为 `task0`；
  - 新增 task0 workflow 解析逻辑：优先使用 `coordinator_workflow`，未配置时兼容回退 `task_workflows["0"]`，支持按 workflow id 或 name 匹配。
- Session/Coordinator 语义对齐：
  - 默认会话统一为 `0` 号 `MMAgentSession`；
  - 每个 session 创建时默认注入 `task0`（`<session_id>_task_0`）并创建该 session 专属 coordinator；
  - session 消息处理改为使用 session 级 coordinator，确保 coordinator 归属 session。
- MMAgent 包新增 task0 workflow 定制能力：
  - `include/internal/agent/package.h` / `src/agent/package.cpp` 新增 `coordinator_workflow` 配置解析；
  - `src/agent/agent.cpp` 初始化 coordinator 时注入包配置中的固定 workflow；
  - `publish/agent/package.json` 与 examples 增加 `coordinator_workflow.json`，用于演示 session 创建节点与 task 创建节点编排。
- 测试增强：
  - `tests/unit/src/test_agent_gtest.cpp` 新增 task0 默认行为和 package task0 workflow 映射测试；
  - `tests/unit/src/test_builtin_workflow_gtest.cpp` 增加 `coordinator_workflow` 解析断言；
  - `tests/unit/src/test_runtime_master_worker_gtest.cpp` 新增 worker 执行引擎 shell 路径测试。

## MMNode 热更新能力下沉到 Worker（2026-02-28）

- 热更新控制面迁移到 Worker：
  - `include/internal/runtime/worker.h` 新增 `check_and_reload_code/get_code_version/rollback_code_version/stop_hot_reload`；
  - `src/runtime/worker/worker.cpp` 统一执行与热更新能力，支持运行中版本更新与回退。
- MMNode 热更新 API 保持兼容但改为 Worker 委托：
  - `src/workflow/node/node.cpp` 中 `reload/check/version/rollback` 统一转发到 Worker；
  - 外部执行请求携带 `enable_hot_reload/hot_reload_config`，默认配置无需额外 `enable_code_reload` 调用。
- 代码归属调整：
  - `src/workflow/node/exe_loader.cpp` 迁移到 `src/runtime/worker/exe_loader.cpp`，热更新实现与 Worker 执行引擎同域管理。
- 测试补充：
  - `tests/unit/src/test_runtime_master_worker_gtest.cpp` 新增 Worker 热更新+回退用例；
  - `tests/unit/src/test_hot_reload.cpp` 刷新 MMNode 场景为默认热更新语义。

## Runtime Master 自主调度重构（2026-02-23）

- `src/runtime/runtime.cpp` 增强全局 MMAgent 管理能力：
  - 新增 MMAgent 注册请求队列与注册处理线程（`agent_registration_loop`）；
  - 新增 MMAgent 心跳上报与监控线程（`report_agent_heartbeat` / `agent_monitor_loop`）；
  - MMAgent 故障时联动 Deployer/Scheduler 执行 workflow 注销与 task 映射刷新。
- `include/internal/runtime/shared_runtime_state.h` 扩展共享状态：
  - 增加 `TaskInstanceInfo`；
  - 增加 workflow 按 agent/workflow 检索接口；
  - 增加 task 实例增删改查接口，支撑调度状态追踪。
- `src/runtime/master/deployer.cpp` / `include/internal/runtime/master/deployer.h` 重构：
  - 增加 worker 注册队列与处理线程、部署调度线程、心跳超时故障隔离；
  - 支持 workflow 增量部署与 `rebalance`；
  - 增加 so 可插拔部署算法加载（失败自动回退内置算法）。
- `src/runtime/master/scheduler.cpp` / `include/internal/runtime/master/scheduler.h` 重构：
  - 增加 task 注册队列与调度线程、task->workflow 映射缓存和生命周期状态更新；
  - 默认调度策略优先同 agent instance，再同 agent object，最后全局最小负载；
  - 增加 so 可插拔调度算法加载（失败自动回退内置算法）。
- 验证结果：
  - `./build.sh build` 通过；
  - `./build.sh test` 受既有 HotReload 并行抖动影响失败；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。

## Visualizer GOTO 环路可视化修复（2026-02-23）

- 优化 `publish/agent/visualizer.html` 的边绘制逻辑，新增对回跳边（`goto` 或目标层级在左侧）的专用贝塞尔路径路由：
  - 回跳边从源节点左侧出发，目标节点右侧入边，并抬升弧线高度，避免与前向边重叠；
  - 解决 `publish/workflow/pattern/cycled_workflow.json` 中 `evaluate -> decision` 的 `goto` 控制边不可辨识问题。
- 标签与箭头计算改为基于贝塞尔曲线采样点：
  - 箭头方向使用曲线末端切线，保证回跳边箭头方向正确；
  - 控制类型标签（如 `GOTO`）在曲线上居中显示，回跳边额外上移，提升可读性。
- 边命中检测改为复用统一曲线路径计算，保证回跳边可点击选中并查看属性。
- `fitView` 增加视图留白，避免环路弧线被视口裁切。

## Worker/MMWorkflow/MMNode/MMEdge 自主执行重构（2026-02-23）

- `runtime/worker` 增强为自驱动执行模型：
  - 新增高优先级/普通任务队列、workflow 实例登记队列、事件队列；
  - 新增事件监控线程、事件处理线程池和 workflow 处理线程池；
  - 新 task 入队会触发事件并优先调度高优任务；
  - 新 workflow 实例登记时会按配置动态创建对应处理线程池。
- `workflow/workflow` 增强运行时能力：
  - 新增 workflow 级高优先级/普通消息队列与事件队列；
  - 新增消息调度线程、事件调度线程、消息处理线程池和 node 执行线程池；
  - 消息执行采用主控调度逻辑，按 edge 依赖进行分层串并执行，并推送完成/失败事件。
- `workflow/node` 增强运行时能力：
  - 新增 node 级消息队列、事件队列、executor 线程池和事件处理线程池；
  - 支持消息驱动执行与事件处理器注册。
- `workflow/edge` 增强运行时能力：
  - 新增 edge 级消息队列、事件队列、processor 线程池和事件处理线程池；
  - 支持消息驱动的 edge 处理与事件处理器注册。
- 线程/并发配置入口统一放在 `runtime::MMAgentRuntime::Config`，新增：
  - `worker_default_workflow_threads`
  - `worker_event_threads`
  - `worker_queue_size`
- 验证结果：
  - `./build.sh build` 通过；
  - `./build.sh test` 仍存在既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。

## Vendor 依赖配置与文档刷新（2026-02-23）

- `vendor/vendor.conf` 补充两项依赖配置：
  - `Protoactor-cpp|https://github.com/DanielKernel/Protoactor-cpp.git|main|Protoactor-cpp`
  - `RapidJSON|https://github.com/Tencent/rapidjson.git|master|rapidjson`
- `vendor/README.md` 已刷新并中文化，统一说明目录结构、配置格式、`vendor.sh` 用法和适配器依赖映射关系。
- 构建与测试验证：
  - `./build.sh build` 通过；
  - `./build.sh test` 出现既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。

## 文档治理与架构/API 对齐（2026-02-22）

- 删除重复文档：`docs/contributing.md`，并将保留的贡献规范收敛到根目录 `CONTRIBUTING.md`。
- 修正全仓链接：将原 `docs/CONTRIBUTING.md` 引用统一到 `CONTRIBUTING.md`，避免失效链接。
- 重写 `docs/ARCHITECTURE.md`，按当前实现统一为：
  - Framework 两层：MMAgent + MMWorkflow（MMNode/MMEdge 为 MMWorkflow 组成）
  - Runtime 两类调度：`task -> workflow instance` 与 `workflow instance -> worker`
  - 共享基础设施：MMDataBus
- 重组 `docs/api-reference.md`，按 Framework / Runtime / MMDataBus 三层组织接口说明，并补充 MMAgent 包、MMWorkflow DSL 与 MMNode 开发 API 的索引关系。
- 构建与测试回归：
  - `./build.sh build` 通过；
  - `./build.sh test` 出现既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。

## MMDataBus 全局共享重构（2026-02-22）

- MMDataBus 增强为“本地缓存优先 + 远端服务同步”模型：支持进程内、跨进程、跨节点共享链路。
- 新增 DataRef 流程：`store_with_ref / resolve_ref / find_refs_by_owner`。
- owner 维度索引支持：`request_id/user_id/client_id/agent_id/session_id/task_id/workflow_id/node_id/master_id/worker_id`。
- `msgque` 控制面仅携带 `data_ref`，payload 由 MMDataBus 本地缓存读取，本地 miss 自动回源并回填。
- Redis 依赖接入：`vendor/vendor.conf` 增加 redis 仓库条目，新增 `vendor/adapter/cache/redis_adapter.cpp` 适配实现。
- 回归结果：
  - `./build.sh build` 通过；
  - `./build.sh test` 存在既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行 10/10 通过。

## 历史 TODO 合并记录（2026-02-22）

- 原根目录 `TODO.md` 已合并并下线，规划内容统一收敛到本文件与 `docs/README.md` 导航。
- 历史状态快照（来自旧 TODO）：
  - 开发编排框架：约 85%
  - 运行框架：约 88%
  - 监控和日志：约 68%
- 已完成主线（P0/P1）：MMAgentRuntime 调度闭环、C++ 节点执行、循环控制、序列化链路、MMDataBus 通知等。
- 后续主线（P2）：性能与稳定性增强（共享内存、worker 分组、热更新稳定性、测试抖动治理）。

## 目录结构总览

```
AgentFlow/
├── CMakeLists.txt        # 顶层构建入口
├── build.sh              # 构建入口脚本
├── build/                # 构建输出目录和CMake配置
│   ├── cmake/           # CMake 配置文件
│   └── install_dependencies.sh
├── include/             # 头文件目录
│   ├── external/        # 公共 API 头文件（随库发布）
│   └── internal/        # 内部实现头文件（仅供内部使用）
├── src/                 # 核心业务源代码
├── vendor/              # 三方依赖和适配器
│   ├── vendor.conf     # 三方依赖配置文件
│   ├── vendor.sh       # 三方依赖下载脚本
│   ├── README.md       # 使用文档
│   └── adapter/        # 三方依赖适配器实现
├── tests/               # 测试目录
│   ├── unit/           # 单元测试
│   └── functional/     # 功能测试
├── examples/           # 示例代码
├── docs/               # 项目文档
```

## 优化内容

### 0. MMAgent 消息链路与会话协同增强（2026-02-21）

本次优化将 MMAgent 内部消息处理从单消息容器升级为会话化消息队列协同模型，主链路为：

`reqmsg -> agent.defaultsession -> agent.newsession -> agent.coordinator -> agent.newsession.task -> agent.workflow`

核心变更：
- `src/agent/messages.cpp` 迁移为 `src/agent/msgque.cpp`，核心类升级为 `MMAgentMsgQue`（保留 `AgentMessages` 兼容别名）。
- 每个 MMAgent 实例持有默认会话 `default_session`，请求消息先进入默认会话接收队列。
- 会话按 `user_id + client_id` 动态创建，子会话持有独立接收/发送队列与任务集合。
- 新增 `AgentCoordinator`（`src/agent/coordinator.cpp`），统一处理会话消息并生成任务：
  - 执行任务：映射到某个 workflow 类型；
  - 协同任务：支持通过 so 路径指定协同算法扩展。
- 任务归属模型调整：task 必须绑定其触发会话，并保留 workflow 映射信息供 runtime scheduler 消费。

### 0.1 Runtime 全局 Master/Worker 能力增强（2026-02-21）

本次补齐 Runtime 的全局部署与调度能力，支持 master/worker 角色化运行：

- 新增 `runtime/worker`：Worker 作为 workflow/node 执行引擎抽象，维护负载、状态、心跳与支持的 workflow 类型集合。
- 新增 `runtime/master/deployer`：负责全局 worker 注册、心跳/状态更新、workflow 实例到 worker 的部署映射、映射刷新与版本更新。
- 新增 `runtime/master/scheduler`：负责全局 task 调度到 workflow 实例，再间接落到对应 worker。
  - 调度策略：优先同 agent 实例，再回退到同 agent 对象的其他实例共享 workflow。
- 新增 `runtime/shared_runtime_state` 抽象：本轮实现“接口 + 本地内存默认实现”，用于多实例共享语义与 workflow 实例引用计数。
- `MMAgentRuntime` 增强：
  - 支持 role 配置（`MASTER` 包含 `WORKER` 能力，`WORKER` 不包含 master 能力）；
  - 暴露 worker 注册/心跳/状态接口；
  - 暴露 workflow 实例部署与销毁、task 调度接口，形成 task -> workflow instance -> worker 闭环。

### 0.2 MMWorkflow 内建模板能力增强（2026-02-21）

新增内建 workflow 模板目录，支持开箱即用与业务定制增强：

- `publish/workflow/memory/`
- `publish/workflow/skills/`
- `publish/workflow/tools/`

每个目录均包含默认 `workflow.json` 与 `nodes/` 节点脚本，可在业务侧直接替换节点脚本并扩展流程图。

打包策略采用按需选择（opt-in）：
- MMAgent `package.json` 可配置 `builtin_workflows`（如 `["memory","skills","tools"]`）。
- 构建脚本 `publish/agent_build.sh` 在打包时仅注入声明的内建 workflow 到包内 `workflows/builtin/<name>/`。
- AgentPackage 加载时支持解析 `builtin_workflows` 并将可解析的内建流程文件加入 workflow 列表。

### 0.3 今日优化交付与验证快照（2026-02-21）

本轮代码已完成“MMAgent 消息链路 + Runtime 全局调度 + MMWorkflow 内建模板”三条主线的端到端打通，关键更新如下：

- **MMAgent 路径闭环**：`reqmsg -> default_session -> dynamic_session -> coordinator -> session_task -> workflow` 已落到代码与测试。
- **会话与任务归属强化**：每个 MMAgent 固定默认会话，动态会话按 `user_id + client_id` 拆分；Task 仅归属触发会话并携带 workflow 映射元数据。
- **Runtime 主从能力成型**：`MASTER` 包含 `WORKER` 能力，新增 deployer/scheduler/worker/shared state 协作链路，支持 worker 注册、心跳、负载调度与 workflow 实例映射刷新。
- **内建 workflow 资产化**：新增 `publish/workflow/{memory,skills,tools}`，支持 `builtin_workflows` 按需打包、解析和加载，并保留业务定制入口。
- **测试与构建验证**：最新全量回归通过（`ctest --output-on-failure`，10/10 passed）。

### 0.4 下一步继续优化方向（基于当前代码状态）

1. **共享状态后端外置化**：将 `IRuntimeSharedState` 从内存实现扩展到可插拔外部存储（如 Redis/etcd），支撑多 runtime 实例真实共享。
2. **deployer 调度策略增强**：在状态/负载基础上补充容量阈值、故障隔离与预热策略，提升高并发下的稳定性。
3. **scheduler 可观测性与可解释性**：补充 task->workflow->worker 的调度决策日志与指标，支持按会话和 agent 维度追踪。
4. **内建 workflow 版本化管理**：为 memory/skills/tools 引入版本字段与兼容策略，支持灰度升级与回滚。
5. **协同任务插件治理**：对 so 协同算法增加加载约束、校验与隔离机制，降低线上扩展风险。

### 1. 头文件目录重组

**目录结构：**
```
include/
├── external/           # 公共 API 头文件（供外部调用方使用）
│   ├── agent/          # agent实例化：提供agent实例生命周期管理接口
│   │   ├── agent.h
│   │   └── common.h
│   └── node/           # node定制开发：基于node控制面/运行面接口开发定制node
│       ├── node.h
│       ├── hot_reload.h
│       └── node/
│           └── code_loader.h
└── internal/           # 内部实现头文件（仅供内部系统使用）
    ├── agent/          # agent实现，单agent实例、多会话、多消息、多workflow任务
    ├── workflow/       # workflow实现，包含node和edge
    ├── runtime/        # master/worker部署，多agent、多workflow、多worker调度
    ├── vendor/         # 三方依赖适配
    │   ├── json_adapter/
    │   └── private_protocol.h
    ├── serialization/
    ├── actor_adapter/  # Actor 适配器实现
    └── testing/        # 测试工具
```

**说明：**
- `external/` 仅暴露 `agent/` 与 `node/` 最小外部接口
- `internal/` 承载 `runtime`、`storage`、`workflow` 等全部内部实现能力

### 2. 测试目录重组

**目录结构：**
```
tests/
├── unit/               # 单元测试
│   ├── agent/          # C++ 单元测试代码（当前预留空目录）
│   ├── workflow/       # 测试数据/脚本/测试库与单元测试代码（当前承载既有 unit 内容）
│   │   ├── cpp/
│   │   ├── data/
│   │   ├── jars/
│   │   ├── lib/
│   │   └── scripts/
│   └── runtime/        # 测试库（如 hot_reload_test，当前预留空目录）
└── functional/         # 功能测试
    ├── cpp/            # 功能测试代码（test_performance.cpp 等）
    └── scripts/        # 功能测试脚本
```

**说明：**
- `unit/agent` 与 `unit/runtime` 当前为预留目录
- 现有 unit 测试内容按约定统一落在 `unit/workflow/`（含 cpp/data/lib/scripts/jars）
- `functional/` - 功能测试，包括性能测试、集成测试等

### 3. 三方依赖与适配器目录

**目录结构：**
```
src/                    # 业务代码相关实现，和三方实现独立目录
vendor/                 # 三方依赖相关实现
├── vendor/vendor.conf  # 三方依赖配置文件
├── vendor.sh           # 三方依赖下载脚本
├── README.md           # 使用文档
├── adapter/            # 三方依赖适配器实现
│   ├── actor/          # Actor 框架适配器
│   │   ├── protoactor_impl.cpp
│   │   └── stub_impl.cpp
│   ├── dsl/            # DSL 解析器适配器
│   │   ├── json_adapter_rapidjson.cpp
│   │   └── json_adapter_stub.cpp
│   └── serialization/  # 序列化适配器（adapter_factory.cpp）
└── <downloaded>/       # 下载的三方依赖（gitignore）
```

**说明：**
- `src/` - 保留核心业务代码，不包含适配器实现
- `vendor/` - 所有三方依赖相关内容统一放置
- `vendor/vendor.conf` - 三方依赖下载配置
- `vendor/adapter/` - 与三方依赖相关的适配器代码

### 4. 源代码目录

**目录结构：**
```
src/
├── agent/              # MMAgent 实例管理实现（agent.cpp）
├── workflow/           # MMWorkflow 管理实现
│   ├── dsl/            # DSL 解析实现
│   ├── edge/           # MMEdge 子模块实现
│   ├── node/           # MMNode 子模块实现（code_loader, node_actor 等）
│   ├── workflow.cpp    # MMWorkflow 核心实现
│   ├── node/
│   │   └── node.cpp    # MMNode 核心实现
│   └── edge/
│       └── edge.cpp    # MMEdge 核心实现
├── runtime/            # 运行时实现
│   ├── scheduler/      # 调度器（agent_runtime, control_flow）
│   ├── communication/  # 通信（databus, hot_reload, local_channel, zero_copy_databus）
│   ├── storage/        # 存储（agent_storage, storage_manager）
│   └── actor/          # Actor 系统运行时（actor_system, node_actor 等）
├── vendor/             # 三方适配实现（actor/dsl/event/serialization/storage）
└── utils/              # 工具实现（logger, thread_pool）
```

**说明：**
- `agent/`：实现以下模块：1、agent实例管理模块：支持agent生命周期及状态管理，比如创建（参考agent包里面包含的workflow流程文件和node二进制实现进行实例化）->CREATED，agent实例运行->RUNNING、暂停->PAUSED、停止->STOPPED；2、Agent实例对应会话管理，创建、销毁会话等；3、Agent收发消息管理：agent实例接收的消息缓存、映射到会话及任务；4、Agent任务管理：创建执行任务实例，任务实例与workflow实例映射，Agent只做任务的状态管理，任务到workflow实例需要等待调度
- `workflow/`：实现以下模块：1、workflow管理模块：比如创建workflow实例（低码化DSL、高码化C++接口）；2、node实现（code_loader、node_actor等）；3、edge实现；4、DSL解析
- `runtime/`：实现调度和执行：1、scheduler/：master将workflow到worker调度，所有agent实例生成的workflow统一调度到worker上；control_flow控制流；2、communication/：databus高速数据共享、hot_reload热重载、local_channel本地通道；3、storage/：agent和workflow持久化；4、actor/：运行时Actor系统
- `vendor/` 目录已移出 `src/`，三方依赖适配器统一在 `vendor/adapter/` 中管理

### 5. 构建系统

**构建入口：**
- `build.sh` - 项目根目录的构建入口脚本

**构建输出：**
- `build/` - 构建输出目录，包含编译产物和 CMake 配置

## 使用说明

### 构建项目

```bash
# 构建项目
./build.sh build

# 清理构建
./build.sh clean

# 重新构建
./build.sh rebuild

# 运行测试
./build.sh test

# 运行示例
./build.sh example basic
```

### 下载三方依赖

```bash
# 下载所有配置的三方依赖
./vendor/vendor.sh

# 查看已配置的依赖
./vendor/vendor.sh --list

# 更新已下载的依赖
./vendor/vendor.sh --update

# 清理并重新下载
./vendor/vendor.sh --clean
```

### 安装构建依赖

```bash
# 安装系统依赖和开发环境
./build/install_dependencies.sh
```

### 运行测试

```bash
# 运行单元测试
./build.sh test

# 运行功能测试
./tests/functional/scripts/run_performance_tests.sh
```

## CMake 配置说明

### Include 目录配置

```cmake
# 公共 API（用于外部用户）
include_directories(${CMAKE_SOURCE_DIR}/include/external)

# 内部实现（仅用于源代码编译）
include_directories(${CMAKE_SOURCE_DIR}/include/internal)
```

### 源文件组织

```cmake
# 核心业务代码
set(AGENTFLOW_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/agent/...
    ${AGENTFLOW_SOURCE_DIR}/workflow/...
    ${AGENTFLOW_SOURCE_DIR}/runtime/...
    ${AGENTFLOW_SOURCE_DIR}/vendor/...
    ${AGENTFLOW_SOURCE_DIR}/utils/...
)

# 适配器代码（三方依赖，条件编译）
set(AGENTFLOW_ADAPTER_SOURCES
    ${AGENTFLOW_VENDOR_DIR}/adapter/actor/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/dsl/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/event/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/network/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/serialization/...
)
```

## 设计原则

1. **关注点分离**：
   - `include/external/agent` + `include/external/workflow` - 公共 API，面向外部用户
   - `include/internal/` - 内部实现，面向开发者
   - `src/` - 核心业务逻辑
   - `vendor/` - 三方依赖适配

2. **模块化**：
   - 适配器与核心业务分离
   - 单元测试与功能测试分离

3. **清晰的边界**：
   - 外部用户只能使用 `external/agent` 与 `external/workflow` 接口
   - 内部实现细节对用户透明

---

**更新日期**：2026 年 2 月 19 日


---

## 合并内容：原 planning.md

# AgentFlow 项目规划

> 本文档整合了 AgentFlow 项目的功能实现状态、开发规划、优先级和代码完备性评估。

**最后更新**: 2026年2月18日

---

## 目录

1. [功能实现状态概览](#功能实现状态概览)
2. [已完成功能详情](#已完成功能详情)
3. [待实现功能（按优先级）](#待实现功能按优先级)
4. [近期开发重点](#近期开发重点)
5. [版本规划](#版本规划)
6. [代码完备性评估](#代码完备性评估)
7. [代码质量目标](#代码质量目标)
8. [文档目标](#文档目标)
9. [构建和部署目标](#构建和部署目标)
10. [更新日志](#更新日志)

---

## 功能实现状态概览

| 功能模块 | 完成度 | 状态说明 |
|---------|--------|---------|
| 开发编排框架 | 85% | MMAgent、MMWorkflow、MMNode、MMEdge 定义完成，存储功能已实现 |
| 运行框架 | **88%** | 核心调度完成，控制流完成，P0/P1 缺陷已修复 |
| 监控和日志 | **68%** | 基础监控完成，跨平台实现完成 |

**进度提升**:
- 运行框架：75% -> 88% (+13%)
- 基础设施：48% -> 58% (+10%)
- 监控和日志：58% -> 68% (+10%)

---

## 已完成功能详情

### 第一阶段：开发编排框架（85% 完成）

#### MMAgent 编排
- 已完成 MMAgent 对象定义
- 已完成 MMAgent 生命周期策略（PER_REQUEST、FACTORY、SINGLETON）
- 已完成 MMAgent 版本管理基础字段
- 待完成 MMAgent 资源访问权限管理
- 待完成 MMAgent 版本管理逻辑（版本比较、兼容性检查）
- 待完成 MMAgent 灰度发布配置逻辑

#### MMWorkflow 编排
- 已完成 完整的 DSL 解析实现（WorkflowParser）
- 已完成 DSL 验证功能
- 已完成 MMWorkflow 版本管理基础字段
- 已完成 灰度发布支持
- 已完成 版本回滚机制
- 待完成 MMWorkflow 版本管理逻辑（版本比较、兼容性检查）

#### MMNode 实现
- 已完成 MMNode 控制面结构
- 已完成 MMNode 输入输出接口定义
- 已完成 MMNode 错误处理策略（重试、超时）
- 已完成 MMNode 资源限制
- 已完成 控制面/运行面分离架构
- 已完成 多语言 MMNode 支持（Shell、Python、SO、JAR）
- 已完成 C++ 函数执行（cpp_function 回调 + CodeLoader 回退）
- 已完成 运行面 DSL 配置

#### MMEdge 定义
- 已完成 依赖关系定义
- 已完成 数据类型定义
- 已完成 数据传递方式（DataTransferMode）
- 已完成 控制流事件定义
- 已完成 MMEdge 条件表达式支持
- 已完成 MMEdge 条件评估实现

#### 持久化
- 已完成 本地存储 MMAgent 定义
- 已完成 本地存储 MMWorkflow 定义
- 已完成 版本管理和变更历史
- 已完成 灰度发布配置存储
- 待完成 CI/CD 流程集成接口
- 待完成 远程存储支持（S3、HDFS）

---

### 第二阶段：运行框架（88% 完成）

#### MMAgent 运行管理
- 已完成 MMAgent 实例定义
- 已完成 MMAgent 生命周期和状态管理
- 已完成 MMAgent 内部资源管理
- 已完成 MMAgent 实例策略
- 已完成 微服务层接口封装（内部函数）
- 待完成 MMAgent 对外统一访问接口（依赖微服务框架）
- 待完成 多种协议支持（依赖微服务框架）

#### MMWorkflow 运行和 MMNode 调度
- 已完成 DSL 动态加载和解析
- 已完成 MMWorkflow 实例创建
- 已完成 MMWorkflow 动态加载、卸载和版本更新接口
- 已完成工作流图调度算法（Kahn 算法拓扑排序）
- 已完成 并行执行互不依赖的 MMNode
- 已完成 MMNode 执行状态管理
- 已完成 MMNode 输入输出数据处理
- 已完成 MMNode 生命周期管理
- 已完成 复杂流程控制（条件、循环、异常处理）
  - FOR_EACH 循环实现
  - 条件表达式解析缓存
- 已完成 复杂流程控制与 MMWorkflow 调度集成
  - on_node_complete 调度闭环修复
- 待完成 MMWorkflow 动态更新无需重新创建实例

#### MMAgentRuntime - 核心分层调度
- 已完成 MMAgent 层 Master 实现
- 已完成 MMAgent 生命周期管理、状态监控、资源分配
- 已完成 故障转移基础
- 已完成 MMWorkflow 层 Master 实现
- 已完成 MMWorkflow 依赖关系计算
- 已完成 MMWorkflow 调度决策和状态管理
- 已完成 MMWorkflow 动态加载、卸载和版本更新
- 已完成 MMNode 层 Worker 池
- 已完成 MMNode 多线程并发执行
- 已完成 动态负载均衡
- 已完成 线程安全（std::atomic 替换 std::rand()）
- 已完成 优雅关闭（drain 模式 + 5s 超时）

#### 系统通信机制
- 已完成 MMAgent 间通信基础
- 已完成 微服务层接口封装
- 已完成 Request/Response 序列化
  - to_json/from_json 实现
  - to_binary/from_binary 实现
- 已完成 高速数据总线（InProcess）
- 已完成 ZeroCopyDataBus 订阅通知
- 待完成 零拷贝跨进程（POSIX/SystemV，P2 规划）
- 待完成 分布式 MMAgent 间通信

#### 监控和日志
- 已完成 基础监控接口
- 已完成 性能指标收集
  - 跨平台实现（Linux + macOS）
  - Mach API 实现（macOS）
- 已完成 日志系统
- 已完成 调试信息输出
- 待完成 分布式监控
- 待完成 第三方监控系统对接（Prometheus、Grafana）

#### 分布式执行支持
- 待完成 多 MMAgent 实例协同
- 待完成 负载均衡策略
- 待完成 容错和故障转移
- 待完成 分布式事务支持

---

### 第三阶段：基础设施（58% 完成）

#### Actor 机制
- 已完成 Actor 系统基础框架
- 已完成 Protoactor-cpp 适配实现
- 已完成 消息序列化和反序列化
- 已完成 Ask 模式实现
- 已完成 本地 Actor 通信
- 待完成 远程 Actor 通信
- 待完成 Actor 监督策略

#### 网络适配器
- 已完成 网络接口定义
- 已完成 私有协议实现
- 已完成 SSL 适配器接口
- 待完成 服务器/客户端完整实现
- 待完成 负载均衡支持
- 待完成 连接池管理

#### 事件循环适配器
- 已完成 EventLoop 接口定义
- 已完成 定时器支持
- 已完成 文件描述符事件
- 待完成 其他事件库适配（libuv、libev）

#### 序列化适配器
- 已完成 序列化接口定义
- 待完成 JSON 序列化优化
- 待完成 其他序列化格式支持

#### JSON 适配器
- 已完成 JSON 适配器接口定义
- 已完成 RapidJSON 适配器实现
- 已完成 Stub 实现（无依赖回退）
- 已完成 WorkflowParser JSON 支持

---

### 第四阶段：可选独立组件

#### DSL 工具项目
- 待完成 DSL 编辑器
- 待完成 DSL 可视化工具
- 待完成 DSL 语法高亮支持
- 待完成 DSL 实时验证

> 注意：DSL 解析和验证功能已在 AgentFlow 中实现（WorkflowParser）

#### 监控日志收集器
- 待完成 监控数据采集增强
- 待完成 日志数据汇聚
- 待完成 第三方运维系统对接
- 待完成 监控仪表板（Dashboard）
- 待完成 告警规则配置
- 待完成 告警通知机制

#### 后端服务
- 待完成 文件存储服务集成
- 待完成 大语言模型推理服务集成
- 待完成 其他第三方 MMAgent 或工具服务集成

---

## 待实现功能（按优先级）

### P0 - 核心功能（已全部完成）

所有 P0 缺陷已在 2026-02-18 修复：
- MMAgentRuntime 调度闭环
- 线程安全（atomic worker ID）
- C++ 函数执行
- FOR_EACH 循环
- 性能指标跨平台
- Request/Response 序列化

### P1 - 高优先级（已全部完成）

所有 P1 功能已在 2026-02-18 实现：
- ZeroCopyDataBus 订阅通知
- 条件表达式解析缓存
- 优雅关闭机制
- 本地数据通道

### P2 - 中优先级（下一步规划）

#### 性能优化
1. **零拷贝共享内存**
   - POSIX 共享内存真实实现
   - SystemV 共享内存支持
   - 跨进程数据总线

2. **调度器优化**
   - Worker 池按语言分组
   - 动态 Worker 池扩缩容
   - 智能负载均衡

3. **MMWorkflow 优化**
   - MMWorkflow 热替换
   - 动态更新无需重启
   - ZeroCopyDataBus 分段锁

#### 分布式支持
4. **跨进程通信**
   - 消息序列化完整实现
   - 远程 Actor 通信
   - 分布式调度

5. **高可用**
   - 故障检测和自动恢复
   - 状态复制和同步
   - 分布式协调（Raft/Paxos）

### P3 - 低优先级（长期规划）

#### 开发工具
1. **DSL 工具链**
   - 可视化编排工具
   - DSL 语法检查器
   - DSL 到 JSON 转换器

2. **调试工具**
   - MMWorkflow 执行追踪
   - 可视化监控面板
   - 性能分析工具

#### 生态集成
3. **云原生支持**
   - Kubernetes 集成
   - Docker 容器化
   - Helm Chart

4. **第三方集成**
   - Prometheus 监控
   - Grafana 可视化
   - ELK 日志收集

### P4 - 外部依赖相关

1. **可选依赖完整实现**
   - Protoactor-cpp 远程通信
   - OpenSSL 完整 SSL/TLS

2. **新依赖引入评估**
   - gRPC 支持
   - HTTP/2 支持
   - MessagePack 序列化

---

## 近期开发重点

### 当前阶段（已完成）
- P0 缺陷修复（6/6）
- P1 功能补全（6/6）
- 跨平台构建支持
- 文档重组和刷新
- 代码结构重组

### 下一阶段（P2 性能优化）
1. **POSIX 共享内存实现**
   - 工作量：大
   - 优先级：高
   - 预计时间：2-3周

2. **Worker 池按语言分组**
   - 工作量：大
   - 优先级：高
   - 预计时间：1-2周

3. **MMWorkflow 热替换**
   - 工作量：大
   - 优先级：中
   - 预计时间：2-3周

4. **修复预存在测试失败**
   - AgentFlowGoogleTests SEGFAULT
   - HotReloadTests 时序问题
   - EventLoopTests SEGFAULT
   - 工作量：中
   - 优先级：高

---

## 版本规划

### v0.1.0（当前版本）
- 核心框架实现
- 基础功能完成
- P0/P1 缺陷修复
- 跨平台支持

### v0.2.0（计划中）
- 目标：P2 性能优化
- 预计发布：2026 Q2
- 主要特性：
  - POSIX 共享内存
  - Worker 池优化
  - MMWorkflow 热替换
  - 测试失败修复

### v0.3.0（规划中）
- 目标：分布式支持
- 预计发布：2026 Q3
- 主要特性：
  - 远程 Actor 通信
  - 分布式调度
  - 高可用支持

### v1.0.0（远期目标）
- 目标：生产就绪
- 预计发布：2026 Q4
- 主要特性：
  - 完整功能集
  - 生产级稳定性
  - 完整文档和工具链
  - 云原生支持

---

## 代码完备性评估

> 版本: 1.0
> 评估日期: 2026-02-17
> 评估方法: 对照功能清单，逐一审查源码实现

### 构建状态

| 项目 | 状态 | 说明 |
|------|------|------|
| 编译 | 通过 | 38个源文件全部编译成功 |
| 链接 | 通过 | agentflow 库和 8 个示例程序链接成功 |
| 测试运行 | 失败 | macOS dyld 找不到 libgtest_main.1.11.0.dylib（RPATH 问题） |
| 测试性能工具 | 已迁移 | `tests/unit/workflow/lib/performance_metrics.{h,cpp}` 已与测试代码同目录 |

### 文档与代码一致性审查

以下功能在文档中标记为已完成，但代码实际为 stub 或不完备：

#### 严重不一致（核心功能不可用）

**ZeroCopyDataBus POSIX/SystemV 共享内存**
- **文档声称**: "本地零拷贝共享具体实现 - 已实现 ZeroCopyDataBus，支持共享内存和读写锁"
- **代码现状**: `src/runtime/communication/zero_copy_databus.cpp` 中：
  - `SharedMemoryFactory::create_posix_shared()` -> 返回 nullptr
  - `SharedMemoryFactory::create_systemv_shared()` -> 返回 nullptr
  - 仅 `create_in_process()` (堆内存模拟) 可用，不是真正的共享内存
- **影响**: 跨进程零拷贝数据共享完全不可用
- **修复建议**: 使用 `shm_open()/mmap()` 实现 POSIX 共享内存块，或明确文档标注为 "仅进程内可用"

#### 中等不一致（功能部分缺失）

**Request/Response 反序列化**
- **文件**: `src/runtime/network/request.cpp`, `src/runtime/network/response.cpp`
- **问题**: `from_json()` 和 `from_binary()` 方法为空 stub
- **影响**: 无法从 JSON/Binary 格式恢复 Request/Response 对象
- **状态**: 已修复

**MMAgentRuntime 节点调度闭环断裂**
- **文件**: `src/runtime/scheduler/agent_runtime.cpp` 中 `on_node_complete()`
- **问题**: 方法获取了后续可执行节点列表，但没有将其入队到 worker 线程
- **影响**: 工作流图执行在第一批节点完成后停止
- **状态**: 已修复

**ZeroCopyDataBus 订阅通知未触发**
- **文件**: `src/runtime/communication/zero_copy_databus.cpp`
- **问题**: `subscribe()` 注册了回调，但 `put()` 方法中从未调用 `notify_subscribers()`
- **影响**: 数据变更订阅机制完全无效
- **状态**: 已修复

**ControlFlow FOR_EACH 循环**
- **文件**: `src/runtime/scheduler/control_flow.cpp`
- **问题**: `FOR_EACH_LOOP` 分支直接 `return false`
- **影响**: FOR_EACH 循环类型完全不可用
- **状态**: 已修复

**跨进程消息序列化**
- **文件**: `vendor/adapter/actor/message_serializer.cpp`
- **问题**: 仅 RAW 格式可用，跨进程序列化路径为 stub
- **影响**: Actor 消息无法跨进程传递
- **状态**: 待实现（P2）

**StorageManager 工厂函数未定义**
- **文件**: `src/runtime/storage/storage_manager.cpp`
- **问题**: `create_agent_storage()` 和 `create_workflow_storage()` 工厂函数体未实现
- **影响**: StorageManager 初始化时无法创建实际的存储实例
- **状态**: 待实现

### 代码 Bug 清单

#### BUG-001: std::rand() 线程安全问题
- **文件**: `src/runtime/scheduler/agent_runtime.cpp`
- **严重度**: 中
- **描述**: `get_least_loaded_worker()` 中使用 `std::rand()`，该函数非线程安全
- **状态**: 已修复 - 使用 std::atomic<int> 替换

#### BUG-002: execute_with_timeout lambda 引用悬空
- **文件**: `src/workflow/node/node.cpp` 中 `execute_with_timeout()`
- **严重度**: 中
- **描述**: lambda 捕获 `executor` 引用，如果超时后线程仍在运行，executor 生命周期结束后引用悬空
- **状态**: 已修复

#### BUG-003: on_node_complete 调度闭环断裂
- **文件**: `src/runtime/scheduler/agent_runtime.cpp` 中 `on_node_complete()`
- **严重度**: 中（功能性 Bug）
- **描述**: 获取后续可执行节点后未入队调度
- **状态**: 已修复

### 测试覆盖度评估

#### 当前测试统计

| 测试文件 | 用例数 | 覆盖组件 | 质量 |
|---------|--------|---------|------|
| test_local_channel_gtest.cpp | 43 | LocalChannel 完整功能 | 完善 |
| test_hot_reload.cpp | 44 | 多语言热重载、版本管理 | 完善 |
| test_workflow_json_gtest.cpp | 29 | MMWorkflow JSON 解析 | 完善 |
| test_agent_json_gtest.cpp | 27 | MMAgent JSON 解析 | 完善 |
| test_adapter_isolation.cpp | 6 | Actor 适配器隔离 | 基本 |
| test_node_gtest.cpp | 5 | MMNode 基本创建和状态 | 基础 |
| test_agent_gtest.cpp | 3 | MMAgent 创建 | stub |
| test_workflow_gtest.cpp | 3 | MMWorkflow 创建 | stub |
| **合计** | **258** | | |

#### 缺失测试（按优先级排序）

**必须补充（P0）**

| 组件 | 需要测试的内容 |
|------|--------------|
| **MMAgentRuntime** | 三层调度、workflow 执行、node 并发调度、负载均衡 |
| **MMWorkflow 执行** | 工作流图拓扑排序、get_executable_nodes、mark_node_executed、循环/条件 |
| **MMAgent 生命周期** | 状态机转换（CREATED->INITIALIZED->RUNNING->PAUSED->STOPPED） |
| **ControlFlowManager** | 条件评估、循环控制、错误处理策略 |
| **MMDataBus** | put/get/remove、订阅回调、线程安全、容量限制 |

### 代码优化建议

#### 架构层面优化

**OPT-A1: Worker 池按语言分组**
- **当前问题**: 所有 MMNode（C++/Python/Shell）共享同一个 worker 线程池
- **建议**: 按 ExecutionMode 分组，C++ 节点使用高优先级池，脚本节点使用隔离池
- **优先级**: P2

**OPT-A2: 优雅关闭机制**
- **当前问题**: `MMAgentRuntime::stop()` 直接标记 `running_ = false`，不等待进行中的 workflow 完成
- **建议**: 实现 drain 模式：停止接受新任务 -> 等待进行中任务完成 -> 关闭 worker 线程
- **优先级**: P1
- **状态**: 已完成

**OPT-A3: MMWorkflow 热替换**
- **当前问题**: `MMWorkflow::update_dynamic()` 仅调用 `load_from_dsl()`，需要重建实例
- **建议**: 支持原地更新工作流图定义，保持执行状态
- **优先级**: P2

#### 性能层面优化

**OPT-P1: 线程安全随机数**
- **当前问题**: `std::rand()` 非线程安全
- **建议**: 使用 `thread_local std::mt19937`
- **优先级**: P0
- **状态**: 已完成

**OPT-P2: 条件表达式解析缓存**
- **当前问题**: `MMWorkflow::parse_condition_expression()` 每次评估都重新解析条件字符串
- **建议**: 首次解析后缓存 `ConditionExpression` 对象
- **优先级**: P1
- **状态**: 已完成

**OPT-P3: ZeroCopyDataBus 分段锁**
- **当前问题**: InProcessSharedMemoryBlock 使用单一 `shared_mutex`，高并发下写操作会阻塞所有读操作
- **建议**: 按 key 的 hash 分段，每段独立锁，降低锁竞争
- **优先级**: P2

**OPT-P4: MMDataBus 容量预分配**
- **当前问题**: `HighSpeedDataBus` 的 `std::unordered_map` 在数据量增长时频繁 rehash
- **建议**: 构造时根据 `max_capacity` 预留 bucket 数量 (`reserve()`)
- **优先级**: P3

### 优化实施路线

#### 第一阶段：P0 缺陷修复（已全部完成）

| 编号 | 任务 | 涉及文件 | 状态 |
|------|------|---------|------|
| OPT-B1 | performance_metrics.cpp 加入编译 | CMakeLists.txt | 已完成 |
| OPT-B2 | 修复 gtest RPATH | CMakeLists.txt | 已完成 |
| OPT-P1 | std::rand() -> std::atomic<int> | agent_runtime.cpp | 已完成 |
| OPT-R1 | on_node_complete 调度闭环 | agent_runtime.cpp | 已完成 |
| BUG-002 | lambda 引用悬空修复 | node.cpp | 已完成 |

#### 第二阶段：P1 功能补全（已全部完成）

| 编号 | 任务 | 涉及文件 | 状态 |
|------|------|---------|------|
| OPT-R5 | execute_cpp_function 真实实现 | node.cpp, node.h | 已完成 (cpp_function 回调 + CodeLoader 回退) |
| OPT-R3 | ZeroCopyDataBus 订阅通知 | zero_copy_databus.cpp | 已完成 (put()后调用notify_subscribers) |
| OPT-R4 | Request/Response 反序列化 | request.cpp, response.cpp | 已完成 (from_json + from_binary) |
| OPT-R2 | FOR_EACH 循环实现 | control_flow.cpp | 已完成 (逗号分隔集合迭代) |
| OPT-P2 | 条件表达式解析缓存 | workflow.cpp, workflow.h | 已完成 (unordered_map缓存) |
| OPT-A2 | 优雅关闭机制 | agent_runtime.cpp, agent_runtime.h | 已完成 (drain模式+超时) |
| 测试 | 修复 NodeLifecycle 测试 | test_node_gtest.cpp | 已完成 |

#### 第三阶段：P2 性能与架构优化（建议中期规划）

| 编号 | 任务 | 涉及文件 | 工作量 |
|------|------|---------|--------|
| 2.1.1 | POSIX 共享内存真实实现 | zero_copy_databus.cpp | 大 |
| OPT-A1 | Worker 池按语言分组 | agent_runtime.cpp | 大 |
| OPT-A3 | MMWorkflow 热替换 | workflow.cpp | 大 |
| OPT-P3 | ZeroCopyDataBus 分段锁 | zero_copy_databus.cpp | 中 |
| 2.2.5 | 跨进程消息序列化 | message_serializer.cpp | 大 |
| 2.2.6 | StorageManager 工厂函数 | storage_manager.cpp | 中 |

---

## 代码质量目标

### 测试覆盖
- 已完成 单元测试框架（GTest）
- 已完成 集成测试（6/10 通过）
- 已完成 性能测试框架
- 待完成 代码覆盖率 > 80%
- 待完成 所有核心模块单元测试
- 待完成 端到端测试套件

### 代码规范
- 已完成 C++17 标准
- 已完成 Google C++ 风格指南
- 已完成 clang-format 配置
- 已完成 clang-tidy 静态分析
- 已完成 Cppcheck 配置
- 待完成 代码审查流程
- 待完成 贡献者指南完善

### CI/CD
- 已完成 GitHub Actions 工作流
- 已完成 多平台构建（Linux/macOS AMD64/ARM64）
- 已完成 自动化测试
- 已完成 代码质量检查
- 已完成 自动化发布
- 待完成 Docker 镜像构建
- 待完成 性能基准测试自动化

---

## 文档目标

### 用户文档
- 已完成 README.md（项目概览）
- 已完成 快速入门指南
- 已完成 API 参考手册
- 已完成 架构设计文档
- 待完成 用户手册（完整版）
- 待完成 最佳实践指南
- 待完成 故障排查手册

### 开发者文档
- 已完成 开发指南
- 已完成 贡献指南
- 已完成 测试指南
- 已完成 CI/CD 指南
- 待完成 设计模式文档
- 待完成 性能优化指南
- 待完成 插件开发指南

### 示例和教程
- 已完成 基础示例（hello_world 等）
- 待完成 进阶示例
- 待完成 分布式部署示例
- 待完成 视频教程
- 待完成 在线演示环境

---

## 构建和部署目标

### 构建系统
- 已完成 CMake 构建系统
- 已完成 跨平台支持（Linux/macOS）
- 已完成 依赖管理
- 已完成 构建脚本（build.sh）
- 待完成 Windows 支持
- 待完成 交叉编译支持
- 待完成 构建缓存优化

### 部署方式
- 已完成 本地部署
- 待完成 Docker 容器化
- 待完成 Kubernetes 部署
- 待完成 云平台支持（AWS、Azure、GCP）
- 待完成 一键部署脚本

### 发布管理
- 已完成 版本号管理
- 已完成 GitHub Release
- 已完成 变更日志
- 待完成 软件包管理（apt、yum、brew）
- 待完成 二进制分发优化

---

## 变更记录：移除网络模块

### 移除内容

**2026-02-21**: 移除了 `src/runtime/network` 目录，包括：
- `src/runtime/network/private_protocol.cpp` - 私有协议编码解码器
- `src/runtime/network/request.cpp` - Request 类实现
- `src/runtime/network/response.cpp` - Response 类实现
- `tests/unit/workflow/cpp/test_request_response_gtest.cpp` - Request/Response 测试用例（56个测试）

### 移除原因

网络模块（Request/Response/PrivateProtocol）在当前阶段不是核心功能，优先级较低。移除该模块以：
1. 简化核心功能结构
2. 减少代码维护负担
3. 专注于 MMAgent、MMWorkflow、Runtime 等核心模块的完善
4. 为将来的网络功能设计留出空间

### 后续计划

- 网络通信功能可在未来版本中重新设计和实现
- 将根据实际需求确定通信协议和 API 设计

---

## 最新变更总结 (2026-02-21)

### 事件适配器 (Event Adapter)

**新增内容：**
- `include/internal/vendor/event_adapter.h` - 事件适配器接口定义
- 支持将 MMAgent 运行时事件转换为可观察的事件流
- 提供统一的事件发送和订阅机制

**功能特性：**
- 事件类型：MMAgent 生命周期事件（启动、停止、错误等）
- 事件过滤和优先级支持
- 异步事件处理机制

### MMAgent 组件完善

**更新内容：**
- MMAgent 组件加强了生命周期管理
- 改进了 MMWorkflow 任务调度和执行
- 完善了错误处理和日志记录机制

### 示例代码重组

**调整内容：**
- 将 `examples/dsl` 目录中的所有 DSL 示例文件移动到 `examples/workflow/dsl/`
- 包含文件：
  - `conditional_workflow.json` - 条件流程示例
  - `loop_workflow.json` - 循环流程示例
  - `sample_workflow.json` - 基础流程示例
  - `visualizer.html` - 流程可视化工具
- 目的：统一整理示例代码结构，使 DSL 示例与 MMWorkflow 示例位置更加紧密

**新的目录结构：**
```
examples/
├── workflow/
│   ├── dsl/                      # DSL 相关示例（新位置）
│   │   ├── conditional_workflow.json
│   │   ├── loop_workflow.json
│   │   ├── sample_workflow.json
│   │   └── visualizer.html
│   ├── dsl_example.cpp
│   └── node_execution_example.cpp
├── agent/
├── adapter_example.cpp
├── basic_example.cpp
├── metrics_example.cpp
├── p1_features_example.cpp
├── storage_example.cpp
└── README.md
```

---

## 更新日志

| 日期 | 变更 |
|------|------|
| 2026-02-21 | 刷新 MMAgent/Runtime/MMWorkflow 当日优化快照：补充消息链路、master/worker 全局调度、内建 workflow 资产化与全量测试结果（ctest 10/10）并更新下一步优化方向 |
| 2026-02-21 | 新增事件适配器 (Event Adapter) 和示例代码重组：DSL 示例从 examples/dsl 移动到 examples/workflow/dsl |
| 2026-02-21 | 移除网络模块 (Request/Response/PrivateProtocol) 以简化核心功能结构，优先专注于 MMAgent、MMWorkflow、Runtime 等核心模块 |
| 2026-02-21 | 完成网络协议和Request/Response实现：Protocol Encoder/Decoder、Request/Response类、JSON序列化支持 |
| 2026-02-18 | 合并 roadmap.md 和 code-review.md 到 planning.md，整合项目规划文档 |
| 2026-02-17 | 代码完备性评估：修正功能状态标注，标记 stub/不完备功能 |
| 2026-02-17 | 整理功能规划文档，更新至 docs 目录 |
| 2026-02-16 | 重新梳理功能实现状态，更新优先级排序 |

---

## Refactoring Iteration 3 - MMAgent Framework Enhancement & Build System

### Overview
This iteration focuses on enhancing the agent framework with practical examples, improving the publish/build system, and documenting core enhancements made to the node adapter and session management components.

### Changes Implemented

#### 1. Core Enhancements in `src/agent`

**MMNode Adapter (`node_adapter.cc`, `node_adapter.so`)**
- Implemented complete node execution environment for agents
- Added support for multiple node types: start, end, task, condition, loop
- Implemented data flow between connected nodes
- Added error handling and node state management
- Compiled as shared library (`node_adapter.so`) for runtime loading

**Session Management (`session.h`, `session.cc`)**
- Implemented `MMAgentSession` class for managing agent lifecycle
- Added session state tracking (IDLE, RUNNING, PAUSED, COMPLETED, FAILED)
- Implemented context preservation across multiple agent invocations
- Added session storage and retrieval mechanisms

**Task Management (`task.h`, `task.cc`)**
- Implemented `Task` class for individual workflow tasks
- Added task scheduling and execution tracking
- Implemented task dependency resolution
- Added input/output parameter handling

#### 2. Enhanced `publish` Directory Structure

**`publish/agent_build.sh`**
- Enhanced script with support for parameterized agent building
- Added source directory and output zip file specification
- Supports building any agent from `examples/agent/*` directories
- Creates distributable zip packages with all necessary files
- New usage: `./publish/agent_build.sh build <source> <output>`

**Benefits:**
- Decoupled from hardcoded publish/agent directory
- Flexible build targeting for multiple agent examples
- Easy packaging for agent distribution

#### 3. New `examples/agent` Examples

**Directory Structure:**
```
examples/agent/
├── basic_agent/
│   ├── package.json          # MMAgent metadata
│   └── workflow.json         # Basic workflow DSL example
├── basic_agent/
│   ├── package.json          # MMAgent metadata
│   ├── workflow.json         # Calculator workflow DSL
│   └── calc.py              # Python implementation
└── sample_package/
    └── package.json         # Reference sample
```

**`basic_agent` Example**
- Simple entry-level agent example
- Demonstrates basic workflow structure with 3 nodes (start, process, end)
- Package metadata in JSON format
- Supports easy extension with new nodes

**`basic_agent` Example**
- Advanced example with data flow and validation
- Demonstrates multi-step workflow: validate input → add numbers → end
- Includes Python implementation (`calc.py`) for actual computation
- Shows parameter passing between workflow nodes
- Example usage: `./examples/agent/basic_agent/calc.py 5 3` → Output: `5 + 3 = 8`

#### 4. Build System Integration

**Build Verification:**
```bash
# Build examples
./build.sh build

# Run tests
./build.sh test

# Package agents
./publish/agent_build.sh build examples/agent/basic_agent build/basic_agent.zip
./publish/agent_build.sh build examples/agent/basic_agent build/basic_agent.zip
```

**Generated Artifacts:**
- `build/basic_agent.zip` - Packaged basic agent (785 bytes)
- `build/basic_agent.zip` - Packaged calculator agent (1.3 KB)

### Documentation Updates

- Added comprehensive agent example documentation
- Updated build system documentation
- Documented node adapter architecture
- Created workflow DSL examples for reference

### Related Components

**Files Modified/Created:**
- `src/agent/node_adapter.cc` - MMNode execution engine
- `src/agent/session.h/cc` - Session management
- `src/agent/task.h/cc` - Task execution
- `publish/agent_build.sh` - Enhanced build script
- `examples/agent/basic_agent/*` - Basic agent example
- `examples/agent/basic_agent/*` - Merged basic execution example
- `docs/REFACTORING.md` - This documentation

### Testing

All examples have been validated:
- ✅ Directory structure creation
- ✅ JSON files validation (package.json, workflow.json)
- ✅ Python script execution (calc.py adds two numbers correctly)
- ✅ agent_build.sh script execution
- ✅ Zip package creation and contents verification
- ✅ Build system integration (./build.sh build, ./build.sh test)

### Future Work

- Implement C++ versions of agent adapters
- Add more complex workflow examples (loops, conditions)
- Implement workflow visualization tools
- Add agent testing framework
- Create agent deployment documentation

---

## 相关文档

- [架构设计](ARCHITECTURE.md) - Framework/Runtime/MMDataBus 架构
- [开发指南](../CONTRIBUTING.md) - 开发和构建说明
- [测试指南](README.md) - 测试方法
- [贡献指南](../CONTRIBUTING.md) - 贡献流程

---

**项目维护者**: @DanielKernel

## MMAgent 包管理与加载重构 (2026-02-21)

### 功能概述
为了支持 MMAgent 的包管理与加载功能，进行了以下重构工作：
1. **支持目录方式加载 MMAgent**：
   - 在 `src/agent` 下新增 `package.h` 和 `package.cpp`，支持解析 `package.json` 和加载 MMAgent 包目录。
   - `package.json` 定义了 MMAgent 的名称、版本、入口点和工作流文件列表。
2. **简化 MMAgent 实例化**：
   - 在 `MMAgent` 类中新增 `create_from_package` 静态工厂方法，支持直接从目录创建 MMAgent 实例。
   - 更新了 `MMAgent` 类的成员函数，支持加载多个工作流。
3. **构建系统更新**：
   - 更新 `CMakeLists.txt` 链接 `dl` 库，支持动态加载。
   - 新增 `test_agent_loading` 测试目标。
4. **功能验证**：
   - 新增 `tests/functional/src/test_agent_loading.cpp` 功能测试，验证 MMAgent 包加载、生命周期管理和工作流执行。
   - 修复了 `AgentState` 命名空间引用的编译错误。
5. **示例增强**：
   - 在 `examples/agent` 目录下新增 `basic_agent` 和 `basic_agent` 示例包，包含完整的 `package.json` 和 `workflow.json`。

### 文件变更
- `include/internal/agent/package.h` (新增)
- `src/agent/package.cpp` (新增)
- `include/external/agent/agent.h` (修改)
- `src/agent/agent.cpp` (修改)
- `CMakeLists.txt` (修改)
- `tests/functional/src/test_agent_loading.cpp` (新增)
- `examples/agent/basic_agent/workflow.json` (新增)
- `examples/agent/basic_agent/workflow.json` (新增)

### 2026-06-25 Agent核心重构与事件系统合并

1. **运行时清理**：
   - 移除了未使用的 `src/runtime/network` 目录。
   - 删除了旧的 `src/runtime/event.cpp` 和 `include/internal/vendor/event/event.h`。
   - 删除了 `Testing` 目录及冗余的测试目录 (`tests/unit/workflow`, `tests/unit/runtime`, `tests/unit/agent`)。

2. **消息链路收敛**：
   - agent 侧不再保留独立事件机制，统一走 `msgque + databus` 协同链路。
   - 控制面消息仅承载路由与 `data_ref` 等元信息，payload 通过 MMDataBus 读取。
   - 相关 `test_event_gtest.cpp` 已移除，验证收敛到 agent/runtime 集成测试。

3. **MMAgent 核心组件增强**：
   - 实现了 `MMAgentSession` (会话管理), `MMAgentTask` (任务管理), `AgentMessages` (消息队列), `AgentContext` (上下文管理)。
   - `MMAgent` 类集成所有新组件，并在 `initialize_components` 中统一初始化。
   - 更新了 `src/agent/agent.cpp` 和 `include/external/agent/agent.h`。

4. **示例与目录重构**：
   - 将 `examples/dsl` 目录重构为 `examples/workflow`，包含原来的 DSL 示例和 JSON 工作流文件。
   - 将 `visualizer.html` 移至 `publish/agent` 目录。
   - 确保 `examples/agent` 包含 MMAgent 包示例 (`basic_agent`, `basic_agent`)。

### 文件变更
- `src/runtime/event.cpp` (删除)
- `include/internal/vendor/event/event.h` (删除)
- `src/agent/session.cpp` / `include/internal/agent/session.h` (新增/更新)
- `src/agent/task.cpp` / `include/internal/agent/task.h` (新增/更新)
- `src/agent/messages.cpp` / `include/internal/agent/messages.h` (新增/更新)
- `src/agent/context.cpp` / `include/internal/agent/context.h` (新增/更新)
- `src/agent/event.cpp` / `include/internal/agent/event.h` (删除)
- `src/agent/agent.cpp` (更新)
- `CMakeLists.txt` (更新)

### 2026-02-22 MMAgent 发布规范重构

1. **MMAgent 包规范落地**
   - 在 `publish/agent/AGENT_PACKAGE_SPEC.md` 新增 MMAgent 双层打包规范说明。
   - 在 `publish/agent` 补充 `agent.json`、`workflow_01.json`、`node_demo.sh` 模板。

2. **打包脚本升级**
   - `publish/agent_build.sh` 从 zip 产物调整为双层 `tar.gz` 产物：
     - `$xxxAgent_$PKGTYPE_$PKGVER.tar.gz`
     - `$xxxAgent_$AGTVER.tar.gz`

3. **开发文档重写**
   - `publish/README.md` 改为业务 MMAgent 包开发指南，覆盖 MMAgent/MMWorkflow/MMNode 开发流程。
   - 在 `publish/workflow` 新增 MMNode 规范与 API 手册：
     - `NODE_DEVELOPMENT_SPEC.md`
     - `NODE_API_REFERENCE.md`

4. **MMWorkflow 样例迁移**
   - `workflow_sample.jsonc` 迁移到 `publish/workflow/workflow_sample.jsonc`。
   - `src/workflow/dsl/README.md` 已更新引用路径。

5. **示例刷新**
   - 更新 `examples/agent/basic_agent` 与 `examples/agent/basic_agent`，补充规范化示例文件并保留现有加载兼容字段。

### 2026-03-07 MMWorkflow edge/node typed IO 重构

1. **MMEdge 控制模型收敛**
   - 删除 `loop/fork/join/async/goto` 控制语义，仅保留 `if` 与 `switch-case`。
   - `switch-case` 采用“多条 edge 共享 `expression`、每条 edge 自带 `case_value` / `is_default`”的分支模型。

2. **MMNode 输入输出类型化**
   - node 新增结构化 `inputs` / `outputs` 字段，支持 `string/int/float/bool/list/map`。
   - 每个字段支持 `transfer_mode: value|ref`，其中 `ref` 用于传递 DataBus 引用。

3. **样例/测试/发布资产同步**
   - 更新 `examples/`、`publish/workflow/pattern/`、`publish/workflow/workflow_sample.jsonc` 与 `tests/unit/src/test_workflow_json_gtest.cpp`。
   - 清理 `examples/agent/multi_session_chat` 下遗留的 `.bak` 备份文件。

### 2026-02-22 MMWorkflow MMEdge 控制与模式化重构

1. **MMEdge 控制统一化**
   - 扩展 MMEdge 配置，新增 `control.type + control.*` 机制，支持 `if/loop/fork/join/async/goto`。
   - DSL 解析器支持新控制字段，并保留 `condition_expression` 向 `if` 控制映射的兼容行为。

2. **MMWorkflow 流程能力增强**
   - 执行路径增加 edge 控制判断（含 async 软依赖、loop/goto 控制判定）。
   - 工作流图校验与拓扑排序对 `loop/goto` 控制边放宽，支持显式环回流程。

3. **模式资产新增**
   - 新增 `publish/workflow/pattern/serial_workflow.json`。
   - 新增 `publish/workflow/pattern/parallel_workflow.json`。
   - 新增 `publish/workflow/pattern/cycled_workflow.json`。

4. **MMNode 模式模板新增**
   - `publish/workflow/node_patterns/` 新增问答/API工具/MCP/skills 四类模板文档。

5. **文档同步**
   - 更新 `publish/README.md`、`publish/workflow/README.md`、`publish/workflow/workflow_sample.jsonc`、`src/workflow/dsl/README.md`。

6. **测试增强**
   - `tests/unit/src/test_workflow_json_gtest.cpp` 增加 edge control 解析与 goto 环回流程解析测试。

### 2026-02-22 MMWorkflow 动态生成与嵌套重构

1. **MMNode/MMWorkflow Skill 化**
   - 新增 `src/workflow/node/skill.md`，定义 node 能力说明、调用方法、输入输出契约。
   - 新增 `src/workflow/dsl/skill.md`，定义 workflow 编排技能、include 调用方式与控制约定。

2. **嵌套 MMWorkflow 支持**
   - 扩展 `MMNode::Config`，新增 `workflow_ref` 字段，支持 `type: "workflow"` 节点引用子工作流。
   - `WorkflowParser` 支持顶层 `includes`（相对路径）并合并子工作流节点/边。
   - 增加 include 循环引用检测，防止递归 include 造成死循环。

3. **动态流程模式新增**
   - 新增 `publish/workflow/pattern/dynamic_workflow.json`：
     - `start -> selector -> (dynamic node or dynamic workflow) -> end`
     - 使用 `goto` 控制边表达动态跳转。

4. **文档与测试同步**
   - 更新 `publish/workflow/README.md`、`publish/README.md`、`src/workflow/dsl/README.md`。
   - 增强 `tests/unit/src/test_workflow_json_gtest.cpp`：覆盖 include 解析、workflow 类型节点解析、dynamic pattern 文件解析。

5. **验证结果**
   - `./build.sh build` 通过。
   - `./build.sh test` 并行存在热重载波动；串行兜底 `ctest --test-dir build --output-on-failure -j 1` 全量通过。

### 2026-02-22 MMWorkflow 重构后兼容性巡检修复

1. **Visualizer 新 DSL 兼容修复**
   - 修复 `publish/agent/visualizer.html` 对新 workflow 字段的兼容：
     - 支持 `.json/.jsonc` 读取（轻量 JSONC 去注释）。
     - 支持 edge `control.type + control.*` 展示与边样式区分。
     - 支持 node `workflow_ref` 和 workflow `includes` 元信息展示。

2. **有环流程可视化修复**
   - 解决旧工作流图入度布局在 `goto/loop` 环回流程上的坐标缺失问题。
   - 增加有环图 fallback 层级计算，保证节点可渲染。

3. **文档同步**
   - 更新 `publish/workflow/README.md` 与 `publish/README.md`，补充 visualizer 对新 DSL 的支持说明。

4. **验证**
   - 执行构建与测试回归，确认修复不影响主流程。

## 示例与对外接口收敛重构（2026-02-23）

- 示例体系收敛：
  - `examples/p1_features_example.cpp` 重构为 `examples/databus_example.cpp`，仅保留 databus 用法；
  - 删除 `examples/basic_example.cpp`、`examples/workflow/{dsl_example,node_execution_example}.cpp`；
  - 删除 `examples/agent/{agent_example,agent_loader_example}.cpp`；
  - 将 `examples/workflow/*.json` 合并到 `examples/agent/`，统一为 agent 包侧示例资产。
- 构建脚本与目标调整：
  - `CMakeLists.txt` 移除 basic/workflow/agent 代码示例目标，新增 `example_databus`；
  - `build.sh example` 默认示例调整为 `databus`。
- 对外头文件发布面收敛：
  - `include/external` 仅保留 node 定制开发接口；
  - 删除 `include/external/agent/agent.h` 与 `include/external/workflow/{workflow.h,edge.h,control_flow.h}`；
  - `include/internal/agent/agent.h` 承接 MMAgent 声明能力，保证内部编译路径稳定。
- 文档与测试：
  - `examples/README.md` 改为 agent 包视角说明，补充 `AgentService.config`、自动加载与请求处理链路说明；
  - `tests/functional/src/test_agent_loading.cpp` 新增 MMAgentActor + MMRuntimeActor 自动加载/注册链路测试（RapidJSON 开启时执行）。

## 2026-03-01 SessionTaskCoordinator & Context 分层重构

- `MMAgentTask` 增加 `workflow_file`，并在任务实例化时透传，供调度阶段做 workflow 一致性校验。
- `AgentCoordinator` 重命名为 `SessionTaskCoordinator`，作为 session 0 号任务化协调单元，下沉至 `MMAgentSession` 持有。
- `AgentContext` 增加可选 `context_workflow` 及默认 `context_updater` 任务。
- 上下文能力扩展至 `MMAgent/Session/Task/MMNode` 四级作用域，缺失 workflow 配置时走默认处理模式。
- package 增加可选字段：`agent_context_workflow`、`session_context_workflow`、`task_context_workflow`、`node_context_workflow`。

## 2026-03-07 全链路跟踪系统（TraceSystem）

新增了一套零外部依赖的全链路跟踪基础设施，覆盖 Agent → Session → Workflow → Node 全执行链路。

### 新增文件

| 文件 | 说明 |
|------|------|
| `include/internal/runtime/trace/trace_types.h` | 核心数据类型：`SpanKind`、`SpanStatus`、`TraceSpan`、`TraceSession`、`TraceTask`、`TraceTaskFilter`、`SamplingContext` |
| `include/internal/runtime/trace/trace_context.h` | 线程局部 `TraceContext`（span 栈）和 RAII `ScopedSpan` |
| `include/internal/runtime/trace/trace_manager.h` | `TraceManager` 单例：任务 CRUD、session 生命周期、NDJSON 文件输出 |
| `include/internal/runtime/trace/trace_exporter.h` | 静态导出工具：`spans_to_json`、`spans_to_csv`、`span_to_ndjson` |
| `src/runtime/trace/trace_context.cpp` | TraceContext & ScopedSpan 完整实现 |
| `src/runtime/trace/trace_manager.cpp` | TraceManager 完整实现 |
| `src/runtime/trace/trace_exporter.cpp` | 导出格式实现 |
| `publish/tools/trace/index.html` | 零依赖单文件 Web 可视化工具（甘特图 + 火焰图 + Span 列表 + 任务管理）|
| `publish/tools/trace/README.md` | Web 工具使用说明 |
| `docs/TRACING.md` | 全链路跟踪系统完整文档 |

### 修改文件

- `CMakeLists.txt`：新增三个 trace 源文件到 `agentflow` 静态库
- `src/runtime/runtime.cpp`：
  - `#include "runtime/trace/trace_context.h"` 和 `"runtime/trace/trace_manager.h"`
  - `execute_workflow()` 开启 WORKFLOW 级 span
  - `process_node()` 使用 `ScopedSpan(SpanKind::NODE, ...)` RAII 封装节点执行，失败时调用 `set_status(ERROR)`
- `src/agent/agent.cpp`：
  - `handle_request_event()` 通过 `TraceManager::find_task()` 采样判断，匹配时调用 `begin_session()`/`end_session()`

### 设计原则

- **按需采样**：无活跃 TraceTask 时 `ScopedSpan` 为纯零开销 no-op
- **线程安全**：`thread_local TraceContext` 无锁传播，`TraceManager` 内部双 mutex（tasks / sessions 分离）
- **NDJSON 持久化**：每行一个 span，append 写，多请求并发不截断文件
- **最小侵入**：RAII ScopedSpan 不改变现有 try/catch/return 逻辑

## 2026-03-17 agent.json 配置结构冗余分析

### 背景

对 agent.json 配置结构进行冗余分析，回答以下问题：
1. 顶层的 `sessions` 数组是否有必要
2. skill task 的 `skill` 属性做什么用的，是否有必要
3. task 定义是否可以融合到 workflow 定义中

**约束：仅更新 REFACTORING.md，不变更代码。**

### Session 在 AgentFlow 框架中的作用

Session 是 AgentFlow 中**消息路由和状态隔离**的核心单元：

```
┌─────────────────────────────────────────────────────────────────┐
│                         MMAgent                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    SessionRouter                         │   │
│  │  • 按 user_id:client_id 路由消息到 Session               │   │
│  │  • 管理 Session 池（分配/回收/老化）                      │   │
│  │  • 维护 route_key -> Session 映射                        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│          ┌───────────────────┼───────────────────┐              │
│          ▼                   ▼                   ▼              │
│  ┌───────────────┐   ┌───────────────┐   ┌───────────────┐     │
│  │   Session 0   │   │   Session 1   │   │   Session N   │     │
│  │ ┌───────────┐ │   │ ┌───────────┐ │   │ ┌───────────┐ │     │
│  │ │Dispatcher │ │   │ │Dispatcher │ │   │ │Dispatcher │ │     │
│  │ └───────────┘ │   │ └───────────┘ │   │ └───────────┘ │     │
│  │ ┌───────────┐ │   │ ┌───────────┐ │   │ ┌───────────┐ │     │
│  │ │Coordinator│ │   │ │Coordinator│ │   │ │Coordinator│ │     │
│  │ └───────────┘ │   │ └───────────┘ │   │ └───────────┘ │     │
│  │ ┌─────┐┌─────┐│   │ ┌─────┐┌─────┐│   │ ┌─────┐┌─────┐│     │
│  │ │Skill││Skill││   │ │Skill││Skill││   │ │Skill││Skill││     │
│  │ └─────┘└─────┘│   │ └─────┘└─────┘│   │ └─────┘└─────┘│     │
│  └───────────────┘   └───────────────┘   └───────────────┘     │
│                                                                 │
│  DataBus: /session/{session_id}/... 隔离各 Session 数据         │
└─────────────────────────────────────────────────────────────────┘
```

### 分析结果

#### 问题1：顶层的 `sessions` 是否有必要

**结论：`sessions` 数组结构冗余，可简化为扁平化 `tasks` 结构**

**分析依据：**

1. 解析代码强制要求只有一个 session 模板（`src/agent/package.cpp:217-218`）:
```cpp
if(session_array.Size() != 1) {
  return fail_agent_json_parse("sessions must contain exactly one homogeneous session template");
}
```

2. 当前结构：
```json
{
  "session_pool_size": 2,
  "sessions": [
    { "id": 0, "tasks": [...] }  // 只允许一个模板，id 永远是 0
  ]
}
```

3. 问题：
   - 数组结构暗示可以定义多个 session，但实际被限制为只能有一个
   - `id` 字段在这种情况下无意义（永远是0）
   - 增加了不必要的嵌套层级

#### 问题2：skill task 的 `skill` 属性用途

**结论：`skill` 属性必要，不可移除**

**分析依据：**

`skill` 属性包含三个字段（`include/internal/agent/package.h:29-33`）：

| 字段 | 用途 | 代码位置 |
|------|------|----------|
| `skill_name` | 标识技能，dispatcher 通过此名称查找并启动技能 | `src/agent/agent.cpp:62-63`, `agent.cpp:80-86` |
| `skill_config_dir` | 技能配置文件目录路径 | `src/agent/package.cpp:131-133` |
| `instance_pool_size` | 创建技能任务实例池，支持并行执行 | `src/agent/agent.cpp:475`, `agent.cpp:502-505` |

#### 问题3：skill 配置放 workflow 还是 task 更合适

**结论：skill 配置应保留在 task 中，不应移到 workflow**

**核心问题：多个 skill 可以共享同一个 workflow**

```json
// agent.json 中多个 skill 使用同一个 workflow 文件
{
  "tasks": [
    {
      "type": "skill",
      "workflow_file": "generic_skill_workflow.json",  // 同一个 workflow
      "skill": { "name": "travel", "config_dir": "skills/travel" }
    },
    {
      "type": "skill",
      "workflow_file": "generic_skill_workflow.json",  // 同一个 workflow
      "skill": { "name": "shopping", "config_dir": "skills/shopping" }
    }
  ]
}
```

**分离设计的优势：**
- **Workflow** = 执行逻辑（nodes、edges、控制流）
- **Task** = 角色配置（哪个技能、配置路径、并行度）
- 一个 workflow 可被多个 skill 共享，各 skill 有独立配置

### 修改项一：agent.json 结构扁平化

**核心思路**：代码中 Session 机制保持不变，只是 JSON 配置扁平化。JSON 中完全移除 "session" 命名和层级。

```json
// 当前结构（冗余嵌套）
{
  "name": "BasicAgent",
  "version": "1.0.0",
  "session_pool_size": 2,
  "tasks": [                           // Agent 级 tasks
    { "type": "contextupdater", ... }
  ],
  "sessions": [
    {
      "id": 0,                         // 无意义，永远是 0
      "tasks": [                       // Session 级 tasks
        { "type": "dispatcher", ... },
        { "type": "coordinator", ... },
        { "type": "skill", ... }
      ]
    }
  ]
}

// 扁平化后（JSON 中无 session 层级和命名）
{
  "name": "BasicAgent",
  "version": "1.0.0",
  "session_pool_size": 2,
  "tasks": [
    { "type": "contextupdater", "scope": "agent", ... },  // Agent 级
    { "type": "dispatcher", ... },                         // Session 级（缺省 scope="session"）
    { "type": "coordinator", ... },                        // Session 级（缺省 scope="session"）
    { "type": "skill", ... }                               // Session 级（缺省 scope="session"）
  ]
}
```

**Task 新增 `scope` 属性**：

| 属性 | 类型 | 缺省值 | 说明 |
|------|------|--------|------|
| `scope` | string | `"session"` | 任务作用域：`"agent"` 或 `"session"` |

**验证规则**：
- `scope: "agent"` 的任务：任意类型，按需配置
- `scope: "session"` 或缺省的任务：必须包含 1 个 dispatcher + 1 个 coordinator + 至少 1 个 skill

**优点**：
1. JSON 完全无 "session" 层级和命名，更简洁
2. 消除 `id`、`task_count`、`description` 等冗余字段
3. 代码中 Session 创建逻辑保持不变
4. 向后兼容：可同时支持新旧格式

### 修改项二：package.json 简化

**移除字段**：

| 字段 | 原因 |
|------|------|
| `task_workflows` | 与 `agent.json` 的 `tasks[].workflow_file` 重复 |
| `dispatcher_workflow` | 从未被使用的 fallback，应在 `agent.json` 定义 |
| `coordinator_workflow` | 从未被使用的 fallback，应在 `agent.json` 定义 |
| `package_type` | 未在解析代码中生效 |
| `agent_version` | 与 `version` 含义重复 |

**简化后的 package.json**：

```json
{
  "name": "basic-agent",
  "version": "1.0.0",
  "description": "...",
  "author": "AgentFlow Team",
  "entry_point": "basic_agent.so",
  "workflows": [
    "workflow.json",
    "coordinator_workflow.json"
  ],
  "builtin_workflows": ["memory"],
  "dependencies": {
    "core-lib": "1.0.0"
  },
  "metadata": {
    "type": "agent",
    "category": "example"
  }
}
```

### 影响范围

**需更新的源码文件**：

| 文件 | 修改内容 |
|------|----------|
| `include/internal/agent/package.h` | `TaskDefinition` 新增 `scope` 字段，`Config::sessions` 改为扁平化 `Config::tasks` |
| `src/agent/package.cpp` | 解析逻辑：按 scope 分类 tasks，新增 scope 字段解析 |
| `src/agent/agent.cpp` | 访问 `sessions.front().tasks` 改为 `session_tasks` |

**需更新的配置文件**：

**agent.json 文件（10个）**：
- `examples/agent/basic_agent/agent.json`
- `examples/agent/nested_workflow_agent/agent.json`
- `examples/agent/workflow_patterns/agent.json`
- `examples/agent/claw_agent/agent.json`
- `examples/agent/llm_query_agent/agent.json`
- `examples/agent/system_agent_mock/agent.json`
- `examples/agent/openjiuwen_studio_agent/agent.json`
- `examples/agent/multi_session_chat/package/agent.json`
- `examples/agent/openjiuwen_multi_session_chat/package/agent.json`
- `examples/agent/multi_language_databus/agent.json`

**package.json 文件（9个）**：
- `examples/agent/basic_agent/package.json`
- `examples/agent/nested_workflow_agent/package.json`
- `examples/agent/workflow_patterns/package.json`
- `examples/agent/claw_agent/package.json`
- `examples/agent/llm_query_agent/package.json`
- `examples/agent/system_agent_mock/package.json`
- `examples/agent/openjiuwen_studio_agent/package.json`
- `examples/agent/multi_session_chat/package/package.json`
- `examples/agent/openjiuwen_multi_session_chat/package/package.json`

**需更新的文档**：
- `publish/agent/AGENT_PACKAGE_SPEC.md` - Agent 包规范
- `publish/INTEGRATION_GUIDE.md` - 集成指南
- `docs/agentflow_user_manual.md` - 用户手册

### 不建议修改

1. **`skill` 属性**：保留在 task 中，支持 workflow 复用
2. **Task 与 Workflow 分离**：保留，架构合理
3. **Session 池机制**：代码层面完全保留，仅 JSON 结构扁平化

### 验证方案

如果未来实施代码修改：
1. 修改解析代码后运行单元测试
2. 更新所有示例 agent.json 和 package.json
3. 运行 `./build.sh test` 验证全量测试通过

### 实施状态（2026-03-17）

**已完成实施**

1. **源码修改**：
   - `include/internal/agent/package.h`：`TaskDefinition` 新增 `scope` 字段，`Config` 新增 `session_tasks` 字段，移除冗余字段
   - `src/agent/package.cpp`：新增 `parse_flattened_tasks()` 和 `validate_session_tasks()` 函数，支持新旧格式向后兼容
   - `src/agent/agent.cpp`：新增 `create_session_from_tasks()` 函数，移除对 package.json 冗余字段的依赖
   - `include/internal/agent/agent.h`：更新函数声明

2. **测试更新**：
   - `tests/unit/src/test_builtin_workflow_gtest.cpp`：移除对已删除字段的断言
   - `tests/e2e/workflow_execution/test_basic_agent_e2e.cpp`：移除对 `coordinator_workflow` 的断言
   - `examples/features/agent_runner.cpp`：移除对 `coordinator_workflow` 的依赖

3. **配置文件更新**：
   - 10 个 agent.json 文件已转换为扁平化格式
   - 9 个 package.json 文件已移除冗余字段
   - `publish/agent/agent.json` 和 `publish/agent/package.json` 已同步更新

4. **向后兼容**：
   - 解析代码同时支持新旧格式
   - 旧格式（`sessions` 数组）仍可正常解析
   - 新格式（扁平化 `tasks` + `scope` 属性）为推荐格式

5. **验证结果**：
   - `./build.sh build`：通过
   - `ctest --test-dir build --output-on-failure`：18/19 测试通过（E2ELLMQueryAgent 因 API Key 缺失失败，与本次修改无关）

## 2026-03-17 Skill 参数槽位校验功能

> **状态：已完成** - 2026-03-17

### 背景

当前框架在 `next_skills` 输出后直接启动对应的 skill tasks，缺乏对 skill 执行前提条件的校验能力。某些 skill 可能需要特定的输入参数才能正常执行（如 "travel" skill 需要 destination 和 date 参数）。如果这些参数在运行时不可用，skill 会执行失败或产生无意义结果。

本设计引入参数槽位校验机制，允许在 agent.json 中声明 skill 的预期参数，框架在启动 skill 前校验这些参数是否可用，校验失败时跳过执行并通知 coordinator。

---

### 接口设计

#### 1. agent.json 新增 `expected_parameters` 属性

在 `skill` task 的 `skill` 配置对象中新增可选属性：

```json
{
  "tasks": [
    {
      "id": 3,
      "type": "skill",
      "description": "Travel booking skill",
      "workflow_file": "skill_travel_workflow.json",
      "skill": {
        "name": "travel",
        "config_dir": "runtime",
        "instance_pool_size": 2,
        "expected_parameters": ["destination", "date", "travelers"]
      }
    },
    {
      "id": 4,
      "type": "skill",
      "description": "Search skill - no parameters required",
      "workflow_file": "skill_search_workflow.json",
      "skill": {
        "name": "search",
        "config_dir": "runtime",
        "instance_pool_size": 2
      }
    }
  ]
}
```

**字段定义**：

| 属性 | 类型 | 必填 | 缺省值 | 说明 |
|------|------|------|--------|------|
| `expected_parameters` | `string[]` | 否 | `[]` | Skill 执行所需的参数名列表 |

**解析逻辑**：
- 未配置时解析为空数组 `[]`
- 空数组表示该 skill 无参数要求，框架不做校验

---

#### 2. Workflow 输出新增 `available_parameters`

当 workflow 完成并输出 `next_skills` 时，需同时输出 `available_parameters`。

**传递机制（优先级从高到低）**：

1. **消息 metadata（推荐）**：放入 workflow 完成消息的 metadata 中
2. **DataBus（向后兼容）**：存入 DataBus，用于兼容旧版本

**重要**：`next_skills` 与 `available_parameters` 同步采用此优先级策略，保持一致性。

**优先级设计原因**：
- 同一 dialogue 可能触发多个 workflow（如 dispatcher 和 skill 都输出 next_skills）
- DataBus 存储会导致后执行的 workflow 覆盖前者的输出
- 消息 metadata 与特定 workflow 实例绑定，避免并发覆盖问题

**消息 metadata 结构**：

```cpp
// Workflow 完成消息
MMAgentMsgQue::Message completion_message;
completion_message.content = "workflow_completed";
completion_message.metadata["next_skills"] = R"(["travel", "shopping"])";
completion_message.metadata["available_parameters"] = R"({
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20"
  },
  "shopping": {
    "category": "electronics"
  }
})";
```

**DataBus 存储（向后兼容）**：

```
/{dialogue_id}/next_skills = ["travel", "shopping"]
/{dialogue_id}/available_parameters = {
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20"
  }
}
```

**数据结构定义**：

```typescript
// available_parameters 结构（参数值仅支持 string 类型）
{
  [skill_name: string]: {
    [parameter_name: string]: string
  }
}
```

**要点**：
- 参数值仅支持 string 类型
- `available_parameters` 为可选输出，未提供时视为空对象 `{}`
- 框架优先从消息 metadata 读取，其次从 DataBus 读取

---

#### 3. 参数校验逻辑

**校验位置**：`src/agent/agent.cpp` 的 `launch_skill_task()` 函数或其调用处

**校验流程**：

```
┌─────────────────────────────────────────────────────────────────┐
│                     Skill Launch with Validation                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. Get skill template from session                             │
│     └─> skill_template->get_expected_parameters()               │
│                                                                  │
│  2. If expected_parameters is empty                             │
│     └─> Skip validation, launch skill directly                  │
│                                                                  │
│  3. Get available_parameters (priority: metadata > DataBus)     │
│     ├─> First: message.metadata["available_parameters"]         │
│     └─> Fallback: DataBus {dialogue_id}/available_parameters    │
│                                                                  │
│  4. Get next_skills (same priority: metadata > DataBus)         │
│     ├─> First: message.metadata["next_skills"]                  │
│     └─> Fallback: DataBus {dialogue_id}/next_skills             │
│                                                                  │
│  5. For each expected_parameter:                                │
│     └─> Check if exists in available_parameters[skill_name]     │
│                                                                  │
│  6. If all expected parameters are covered:                     │
│     └─> Launch skill task                                       │
│                                                                  │
│  7. If any expected parameter is missing:                       │
│     └─> Skip skill launch                                       │
│     └─> Send failure message to coordinator                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

**伪代码**：

```cpp
bool MMAgent::launch_skill_task_with_validation(
    const std::shared_ptr<MMAgentSession>& session,
    const std::string& skill_name,
    const MMAgentMsgQue::Message& context,
    const std::shared_ptr<MMAgentTask>& source_task) {

  // 1. Find skill template
  auto skill_template = find_skill_template(session, skill_name);
  if (!skill_template) return false;

  // 2. Get expected_parameters
  const auto& expected_params = skill_template->get_expected_parameters();

  // 3. If empty, skip validation
  if (expected_params.empty()) {
    return launch_skill_task(session, skill_name, context, source_task);
  }

  // 4. Get available_parameters (priority: metadata > DataBus)
  std::map<std::string, std::string> available_params;

  // 4.1 Try metadata first
  if (context.metadata.count("available_parameters")) {
    available_params = parse_available_parameters(
        context.metadata.at("available_parameters"), skill_name);
  } else {
    // 4.2 Fallback to DataBus
    std::string dialogue_id = context.metadata.count("dialogue_id")
        ? context.metadata.at("dialogue_id") : "";
    std::string params_key = dialogue_id.empty()
        ? "available_parameters"
        : dialogue_id + "/available_parameters";

    DataVariant params_value;
    if (get_data(params_key, params_value)) {
      available_params = parse_available_parameters(params_value, skill_name);
    }
  }

  // 5. Validate: check if all expected parameters are available
  std::vector<std::string> missing_params;
  for (const auto& expected : expected_params) {
    if (available_params.find(expected) == available_params.end()) {
      missing_params.push_back(expected);
    }
  }

  // 6. If validation fails, notify coordinator and return
  if (!missing_params.empty()) {
    send_skill_launch_failure(
        session, skill_name, source_task,
        expected_params, available_params, missing_params);
    return false;
  }

  // 7. Validation passed, inject parameters and launch skill
  inject_skill_parameters(skill_template, available_params);
  return launch_skill_task(session, skill_name, context, source_task);
}
```

---

#### 4. 失败消息格式

当参数校验失败时，框架向 coordinator 发送失败消息：

**消息结构**：

```cpp
MMAgentMsgQue::Message failure_message;
failure_message.message_id = "skill-failure-" + skill_name + "-" + generate_uuid();
failure_message.sender_id = "framework";
failure_message.receiver_id = coordinator_task_id;
failure_message.content = "skill_launch_failed";

// Metadata fields
failure_message.metadata["task_type"] = "skill_response";
failure_message.metadata["message_type"] = "skill_launch_failed";
failure_message.metadata["skill_name"] = skill_name;
failure_message.metadata["failure_reason"] = "parameter_validation_failed";

// Expected vs Available (JSON serialized)
failure_message.metadata["expected_parameters"] = R"(["destination", "date", "travelers"])";
failure_message.metadata["available_parameters"] = R"({"destination": "Beijing", "date": "2024-03-20"})";
failure_message.metadata["missing_parameters"] = R"(["travelers"])";
```

**Metadata 字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `task_type` | string | 固定为 `"skill_response"` |
| `message_type` | string | `"skill_launch_failed"` 标识启动失败 |
| `skill_name` | string | 失败的 skill 名称 |
| `failure_reason` | string | 失败原因，此处为 `"parameter_validation_failed"` |
| `expected_parameters` | string | JSON 序列化的预期参数列表 |
| `available_parameters` | string | JSON 序列化的可用参数对象 |
| `missing_parameters` | string | JSON 序列化的缺失参数列表 |

**Coordinator 处理建议**：
- 可根据 `missing_parameters` 向用户请求补充信息
- 可记录日志用于问题诊断
- 可选择降级到其他 skill 或返回错误响应

---

### 数据流示例

#### 场景：Travel Skill 参数缺失

**1. Dispatcher workflow 执行完成，输出消息：**

```cpp
// Workflow 完成消息
MMAgentMsgQue::Message completion_message;
completion_message.content = "workflow_completed";
completion_message.metadata["next_skills"] = R"(["travel"])";
completion_message.metadata["available_parameters"] = R"({
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20"
  }
})";
```

**2. agent.json 配置：**

```json
{
  "skill": {
    "name": "travel",
    "expected_parameters": ["destination", "date", "travelers"]
  }
}
```

**3. 框架校验：**
- expected: `["destination", "date", "travelers"]`
- available: `{"destination": "Beijing", "date": "2024-03-20"}`
- missing: `["travelers"]`

**4. 发送给 Coordinator 的失败消息：**

```json
{
  "message_id": "skill-failure-travel-abc123",
  "sender_id": "framework",
  "receiver_id": "coordinator-0",
  "content": "skill_launch_failed",
  "metadata": {
    "task_type": "skill_response",
    "message_type": "skill_launch_failed",
    "skill_name": "travel",
    "failure_reason": "parameter_validation_failed",
    "expected_parameters": "[\"destination\", \"date\", \"travelers\"]",
    "available_parameters": "{\"destination\": \"Beijing\", \"date\": \"2024-03-20\"}",
    "missing_parameters": "[\"travelers\"]"
  }
}
```

---

### 代码变更清单

#### 头文件变更

**`include/internal/agent/package.h`**

```cpp
struct SkillTaskConfig {
  std::string skill_name;
  std::string skill_config_dir;
  int         instance_pool_size = 1;
  std::vector<std::string> expected_parameters;  // 新增
};
```

**`include/internal/agent/task.h`**

```cpp
class MMAgentTask {
public:
  // 新增方法
  const std::vector<std::string>& get_expected_parameters() const;
  void set_expected_parameters(const std::vector<std::string>& params);

private:
  std::vector<std::string> expected_parameters_;  // 新增成员
};
```

#### 源文件变更

**`src/agent/package.cpp`**

- 解析 `expected_parameters` 数组字段

**`src/agent/agent.cpp`**

- 在 `launch_skill_task()` 前增加参数校验逻辑
- 新增 `send_skill_launch_failure()` 方法
- 新增 `parse_available_parameters()` 辅助函数

#### 示例配置变更

需更新的 `agent.json` 示例（按需添加 `expected_parameters`）：

- `examples/agent/system_agent_mock/agent.json`
- 其他包含 skill task 的 agent.json

---

### 文档更新清单

| 文档 | 更新内容 | 状态 |
|------|----------|------|
| `publish/agent/AGENT_PACKAGE_SPEC.md` | 新增 `expected_parameters` 字段说明 | ✅ 已完成 |
| `publish/INTEGRATION_GUIDE.md` | 新增参数校验机制说明、失败消息格式 | ✅ 已完成 |
| `docs/agentflow_user_manual.md` | 新增 workflow 输出 `available_parameters` 说明 | ✅ 已完成 |
| `publish/workflow/NODE_API_REFERENCE.md` | 新增节点如何输出 `available_parameters` | ✅ 已完成 |
| `docs/REFACTORING.md` | 本设计文档 | ✅ 已完成 |

---

### 验证方案

#### 单元测试用例

已实现 8 个单元测试用例：

1. **EmptyExpectedParametersSkipsValidation** - 空 expected_parameters 不触发校验
2. **ParseAvailableParametersFromMetadata** - 从 metadata 解析 available_parameters
3. **ValidationPassWhenAllParametersAvailable** - expected_parameters 全部满足，正常启动 skill
4. **ValidationFailsWhenParametersMissing** - expected_parameters 部分缺失，发送失败消息
5. **NoAvailableParametersTreatsAllAsMissing** - available_parameters 未提供，所有 expected 视为缺失
6. **ExtraAvailableParametersIgnored** - available_parameters 包含额外参数，不影响校验
7. **DataBusFallbackWhenNoMetadataAvailableParameters** - DataBus 回退读取
8. **DialogueIdPrefixedAvailableParametersKey** - dialogue_id 前缀的 DataBus key

#### 验证结果

- `./build.sh build`：✅ 通过
- `ctest --test-dir build -R AgentFlowGoogleTests --output-on-failure`：✅ 通过
- `./build.sh test`：✅ 18/19 测试通过（E2ELLMQueryAgent 因 API Key 缺失失败，与本次修改无关）

#### 提交记录

- `ef98064` - feat: Implement skill parameter slot validation
- `f574039` - test: Add unit tests for skill parameter validation
- `021ac9b` - docs: Add expected_parameters field to agent package spec
- `bb04c8e` - docs: Add skill parameter validation to integration guide
- `c98985f` - docs: Add skill parameter validation to user manual
- `625e062` - docs: Add skill parameter validation to NODE_API_REFERENCE

---

### 待确认事项

**已确认**：
- 参数值仅支持 string 类型
- 不支持参数默认值
- next_skills 与 available_parameters 同步采用 metadata 优先策略
- 失败消息格式符合预期

---

### 向后兼容性

- `expected_parameters` 为可选字段，未配置时行为与当前一致
- `available_parameters` 为可选输出，未提供时视为空对象
- 框架优先从消息 metadata 读取，DataBus 作为 fallback
- 完全向后兼容现有 agent.json 和 workflow

---

### 实施完成

功能已完整实现并验证通过：

1. ✅ 代码变更：`package.h`, `task.h`, `package.cpp`, `agent.cpp`
2. ✅ 单元测试：8 个测试用例全部通过
3. ✅ 文档更新：5 份文档已更新
4. ✅ 集成测试：18/19 测试通过

---

## 2026-03-17 Plugin UDS 注入机制设计

### 背景

当前框架在 `AF_DATABUS_UDS_FILE` 环境变量中注入 DataBus UDS 路径，使 Python/Go/Shell 等外部节点进程能够通过 Unix Domain Socket 访问 DataBus。但框架缺少通用的 Plugin UDS 注入机制，集成者无法为自定义插件配置 UDS 路径并在节点运行时获取该路径。

本设计引入 `AF_PLUGIN_UDS_FILE` 环境变量注入机制，参考现有 `AF_DATABUS_UDS_FILE` 实现模式，使集成者能够配置插件 UDS 路径，框架在节点运行时自动注入。

---

### 现有机制分析

#### AF_DATABUS_UDS_FILE 注入流程

位置：`src/runtime/worker/worker.cpp:68-84`

```cpp
// Create UDS server for real-time DataBus access when databus and session_id are provided
std::unique_ptr<UDSServer> uds_server;
if(request.databus && !request.session_id.empty()) {
  // Generate unique socket path based on session_id
  std::string uds_socket_path = "/tmp/agentflow_" + request.session_id + ".sock";
  uds_server = std::make_unique<UDSServer>(request.databus, uds_socket_path);
  if(uds_server->start()) {
    exec_config.env_vars.push_back("AF_DATABUS_UDS_FILE=" + uds_socket_path);
  }
}
```

**关键点**：
1. UDSServer 在节点执行时按需创建
2. UDS 路径基于 session_id 动态生成
3. 通过 `exec_config.env_vars` 注入环境变量
4. 外部进程通过该环境变量获取 UDS 路径

#### 环境变量注入点

所有环境变量通过 `ExecutionConfig.env_vars` 传递：

```cpp
exec_config.env_vars.push_back("AF_DATABUS_DIR=" + file_databus.root_dir().string());
exec_config.env_vars.push_back("AF_DATABUS_KV_DIR=" + file_databus.kv_dir().string());
exec_config.env_vars.push_back("AF_DATABUS_EVENTS_DIR=" + file_databus.events_dir().string());
exec_config.env_vars.push_back("AF_LLM_BASE_URL=" + request.llm_config.base_url);
exec_config.env_vars.push_back("AF_LLM_API_KEY=" + request.llm_config.api_key);
exec_config.env_vars.push_back("AF_LLM_MODEL=" + request.llm_config.model);
```

---

### 接口设计

#### 1. 运行时配置扩展

**MMAgentRuntime::Config 新增字段**：

```cpp
struct Config {
  RuntimeRole role = RuntimeRole::MASTER;
  int         worker_threads = 4;
  std::string local_worker_id = "runtime-local-worker";
  int         worker_default_workflow_threads = 1;
  int         worker_control_threads = 1;
  int         worker_business_threads = 1;
  size_t      worker_queue_size = 1024;
  // 新增
  std::string plugin_uds_path;  // 集成者配置的插件 UDS 路径
};
```

**ExecutionRequest 新增字段**：

```cpp
struct ExecutionRequest {
  // ... 现有字段 ...
  std::string plugin_uds_path;  // 新增：传递给节点的插件 UDS 路径
};
```

#### 2. 环境变量注入

在 `worker.cpp` 的 `execute_code()` 函数中添加注入逻辑：

```cpp
// Inject plugin UDS path if configured
if(!request.plugin_uds_path.empty()) {
  exec_config.env_vars.push_back("AF_PLUGIN_UDS_FILE=" + request.plugin_uds_path);
}
```

#### 3. 配置传递链路

```
MMAgentRuntime::Config.plugin_uds_path
         ↓
MMWorkerEngine (存储配置)
         ↓
ExecutionRequest.plugin_uds_path (传递到 execute_code)
         ↓
AF_PLUGIN_UDS_FILE 环境变量 (注入到节点进程)
```

---

### 配置方式

#### 方式一：运行时配置（推荐）

集成者在初始化 Runtime 时配置：

```cpp
runtime::MMAgentRuntime::Config config;
config.plugin_uds_path = "/var/run/my_plugin.sock";

auto runtime = std::make_shared<runtime::MMAgentRuntime>(config);
```

#### 方式二：环境变量覆盖

框架启动时检查 `AF_PLUGIN_UDS_FILE` 环境变量：

```cpp
// 在 Runtime 初始化时
const char* env_plugin_uds = std::getenv("AF_PLUGIN_UDS_FILE");
if(env_plugin_uds && config.plugin_uds_path.empty()) {
  config.plugin_uds_path = env_plugin_uds;
}
```

---

### SO Library 注入

SO Library 节点通过 V3 签名接收 `INodeDataBusClient*` 参数，已支持 DataBus 访问。插件 UDS 路径的注入方式：

#### 方案：通过环境变量

SO Library 节点在加载执行时，同样通过环境变量获取插件路径：

```cpp
// worker.cpp SO_LIBRARY 分支
case ExecutionMode::SO_LIBRARY: {
  // 环境变量已注入，SO 代码可通过 getenv("AF_PLUGIN_UDS_FILE") 获取
  if(request.databus && !request.session_id.empty()) {
    NodeDataBusAdapter adapter(request.databus);
    NodeDataBusClientImpl client(&adapter, request.session_id);
    result = code_loader.load_and_execute_so_v3(
        request.command, request.function_name.empty() ? "execute" : request.function_name,
        exec_config, &client);
  }
  // ...
}
```

**SO Library 节点获取插件路径示例**：

```cpp
extern "C" void my_node_v3(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBusClient* client) {

  // 通过环境变量获取插件 UDS 路径
  const char* plugin_uds = std::getenv("AF_PLUGIN_UDS_FILE");
  if(plugin_uds) {
    // 连接插件服务...
  }
}
```

---

### 节点进程访问示例

#### Shell Script

```bash
#!/bin/bash
PLUGIN_UDS="${AF_PLUGIN_UDS_FILE:-}"
if [ -n "$PLUGIN_UDS" ]; then
  echo "Plugin UDS path: $PLUGIN_UDS"
  # 通过 socat 或 nc 与插件服务通信
fi
```

#### Python Script

```python
import os

plugin_uds = os.environ.get('AF_PLUGIN_UDS_FILE')
if plugin_uds:
    import socket
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(plugin_uds)
    # 与插件服务通信...
```

#### Go Script

```go
package main

import (
    "os"
    "net"
)

func main() {
    pluginUDS := os.Getenv("AF_PLUGIN_UDS_FILE")
    if pluginUDS != "" {
        conn, err := net.Dial("unix", pluginUDS)
        if err == nil {
            defer conn.Close()
            // 与插件服务通信...
        }
    }
}
```

---

### 代码变更清单

#### 头文件变更

**`include/internal/runtime/agent_runtime.h`**

```cpp
struct Config {
  // ... 现有字段 ...
  std::string plugin_uds_path;  // 新增
};
```

**`include/internal/runtime/worker.h`**

```cpp
struct ExecutionRequest {
  // ... 现有字段 ...
  std::string plugin_uds_path;  // 新增
};
```

#### 源文件变更

**`src/runtime/worker/worker.cpp`**

- 在 `execute_code()` 函数中添加 `AF_PLUGIN_UDS_FILE` 环境变量注入

**`src/runtime/runtime.cpp`**

- 在构造 MMWorkerEngine 时传递 `plugin_uds_path` 配置

**`src/workflow/node/node.cpp`**

- 在 `execute_with_worker_engine()` 中填充 `request.plugin_uds_path`

---

### 文档更新清单

| 文档 | 更新内容 | 状态 |
|------|----------|------|
| `publish/INTEGRATION_GUIDE.md` | 新增 `AF_PLUGIN_UDS_FILE` 环境变量说明、配置方式和节点访问示例 | ✅ 已完成 |
| `publish/workflow/NODE_API_REFERENCE.md` | 新增 SO Library 节点获取插件 UDS 路径的方法说明 | ✅ 已完成 |
| `docs/REFACTORING.md` | 本设计文档 | ✅ 已完成 |

---

### 验证方案

#### 单元测试用例

已实现 9 个单元测试用例：

**NodeTest (3 个)**：
1. **PluginUdsPathGetterSetter** - 测试节点 plugin_uds_path 的 getter/setter
2. **CloneInheritsPluginUdsPath** - 测试节点克隆时继承 plugin_uds_path
3. **CloneWithEmptyPluginUdsPath** - 测试空 plugin_uds_path 的克隆

**RuntimeMasterWorkerTest (6 个)**：
4. **WorkerEngineConfigPluginUdsPath** - 测试 WorkerEngine 配置中设置 plugin_uds_path
5. **RuntimeConfigPluginUdsPath** - 测试 Runtime 配置中设置 plugin_uds_path
6. **PluginUdsPathInjectedAsEnvVar** - 测试 AF_PLUGIN_UDS_FILE 环境变量正确注入
7. **NoPluginUdsPathWhenNotSet** - 测试未配置时不注入环境变量
8. **PluginUdsPathWithPythonScript** - 测试 Python 脚本可通过 os.environ 获取
9. **PluginUdsPathSpecialCharacters** - 测试特殊字符路径处理

#### 验证结果

- `./build.sh build`：✅ 通过
- `ctest --test-dir build -R AgentFlowGoogleTests --output-on-failure`：✅ 全部通过
- 9 个 Plugin UDS 相关测试全部通过

---

### 向后兼容性

- `plugin_uds_path` 为可选配置，未配置时不注入 `AF_PLUGIN_UDS_FILE` 环境变量
- 现有节点代码无需修改，`AF_PLUGIN_UDS_FILE` 环境变量不存在时节点正常运行
- 完全向后兼容现有 Runtime、Worker 和节点实现

---

### 实施状态

**设计阶段**：✅ 已完成

**代码变更**：✅ 已完成

已完成的代码变更：
- `include/internal/runtime/agent_runtime.h` - 添加 `Config.plugin_uds_path` 字段
- `include/internal/runtime/worker.h` - 添加 `ExecutionRequest.plugin_uds_path` 和 `Config.plugin_uds_path` 字段
- `include/internal/workflow/node.h` - 添加 `plugin_uds_path_` 成员和 getter/setter 方法
- `include/external/workflow/node.h` - 同步更新外部 API
- `src/runtime/worker/worker.cpp` - 添加 `AF_PLUGIN_UDS_FILE` 环境变量注入
- `src/runtime/runtime.cpp` - 传递 `plugin_uds_path` 到 MMWorkerEngine 并在 process_node 中注入
- `src/workflow/node/node.cpp` - 在 execute_with_worker_engine 和 clone 中支持 plugin_uds_path

**验证结果**：
- `./build.sh build`：✅ 通过
- `ctest --test-dir build --output-on-failure`：✅ 18/19 测试通过（E2ELLMQueryAgent 因 API Key 缺失失败，与本次修改无关）

**文档更新**：✅ 已完成
- `publish/INTEGRATION_GUIDE.md` - 新增 Plugin UDS 注入机制说明
- `publish/workflow/NODE_API_REFERENCE.md` - 新增 SO Library 节点获取插件 UDS 路径方法
