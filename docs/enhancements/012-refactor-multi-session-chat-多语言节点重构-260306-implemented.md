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
