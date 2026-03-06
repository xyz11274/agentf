# Multi-Session Chat Example (AgentFlow)

## 目录
- 根目录：`examples/multi_session_chat/`
- 进程入口：`examples/multi_session_chat/server/chat_server.cpp`
- WebUI：`examples/multi_session_chat/server/webui/index.html`
- Agent包：`examples/multi_session_chat/package/`
- 多语言节点：`examples/multi_session_chat/nodes/`
- 脚本：`examples/multi_session_chat/scripts/`
- 自动化测试：`examples/multi_session_chat/tests/`

## 功能映射
1. **独立 agent 进程 + WebUI + 对话管理落盘**  
   `example_chat_server` 直接加载 MMAgent 包并初始化 MMAgentRuntime，提供 HTTP + WebSocket，并将消息按 `session_id` 存到 `package/history/*.jsonl`；WebUI 通过 `/api/chat` 和 `/api/history` 展示多轮消息。

2. **多轮对话包 + precreate_session_count=4 + task0 dispatcher**
   - `agent.json` 设置 `precreate_session_count: 4`
   - 默认 session 使用 `default-dispatcher-workflow`（C++ SO 节点）广播请求到其他 session
   - 非默认 session 使用 `session-dispatcher-workflow`（C++ SO 节点）：读取 context、选择 skill、生成任务计划
   - 默认 session 的 `default-coordinator-workflow`（C++ SO 节点）实现跨 session 编排

3. **session_context_workflow**
   `session-context-workflow` 节点链路（**Go 实现**）：  
   读取 prompt 模板 → 读取历史 → LLM 摘要 → 填充 context  
   各节点通过 `AF_*` 环境变量接收 DataBus 输入，以 JSON 形式输出到 DataBus。

4. **4个不同 skill**
   - `skill-search-workflow`（shell_script）
   - `skill-summary-workflow`（shell_script）
   - `skill-date-workflow`（**Python** 实现）
   - `skill-delegate-workflow`（**Java** 实现）

5. **默认 session 响应编排（真实回复）**  
   `default-coordinator-workflow` 通过 C++ SO 节点 `node_apply_task_plan` 实现任务依赖图评估：
   - 分析跨 session 的技能选择结果
   - 对每个任务作 `start / waiting / skip / stop` 决策
   - 将编排计划写入 DataBus（键 `coordinator:orchestration_plan`）

---

## 多语言节点架构

本示例采用多语言节点实现，不同 workflow 的节点分别用不同语言编写：

| Workflow | 节点 | 实现语言 | 执行模式 |
|---|---|---|---|
| `default-dispatcher-workflow` | `pipeline` | **C++** | `so_library` |
| `default-coordinator-workflow` | `collect`, `plan`, `apply` | **C++** | `so_library` |
| `session-dispatcher-workflow` | `load_context`, `select_skill`, `create_task` | **C++** | `so_library` |
| `session-context-workflow` | `read_prompt`, `read_history`, `summarize`, `fill_context` | **Go** | `go_script` |
| `skill-date-workflow` | `date-calc` | **Python** | `python_script` |
| `skill-delegate-workflow` | `agent-delegate` | **Java** | `java_jar` |

### C++ SO 库节点
文件：`nodes/cpp/multi_session_nodes.cpp`  
构建产物：`build/libmulti_session_nodes.dylib`（macOS）/ `build/libmulti_session_nodes.so`（Linux）

导出函数签名：
```cpp
using SoFunction = void (*)(const DataMap& inputs, DataMap& outputs);
// DataMap = std::map<std::string, DataVariant>
// DataVariant = std::variant<int64_t, double, std::string, bool>

extern "C" void node_pipeline(const DataMap& inputs, DataMap& outputs);
extern "C" void node_collect_skills(const DataMap& inputs, DataMap& outputs);
extern "C" void node_plan_dependencies(const DataMap& inputs, DataMap& outputs);
extern "C" void node_apply_task_plan(const DataMap& inputs, DataMap& outputs);
extern "C" void node_load_context(const DataMap& inputs, DataMap& outputs);
extern "C" void node_select_skill(const DataMap& inputs, DataMap& outputs);
extern "C" void node_create_task(const DataMap& inputs, DataMap& outputs);
```

### Go 节点（session-context-workflow）
文件：`nodes/go/session_context/main.go`  
节点名通过第一个 CLI 参数传入（`go run main.go <node_name>`）。  
输入通过 `AF_<KEY>` 环境变量注入；输出以 JSON 对象写入 stdout（`json_output: true`）。

### Python 节点（skill-date-workflow）
文件：`nodes/python/skill_date_node.py`  
实现了完整的中文日期 NLP 解析（本周/下周 + 第N周），无需外部依赖。  
设置 `AF_OUTPUT_FORMAT=json` 时输出 `{"date_result": "...", "status": "ok"}`。

### Java 节点（skill-delegate-workflow）
文件：`nodes/java/SkillDelegateNode.java`  
编译产物：`nodes/java/skill_delegate.jar`  
无外部依赖，内置简单 JSON 序列化。构建命令：
```bash
bash examples/multi_session_chat/nodes/java/build.sh
```

---

## 消息/事件驱动与 DataBus 集成

节点间数据通过 **DataBus** 流转，替代早期的文件传递方式：

- SO 库节点的 `inputs` / `outputs` 直接读写 DataBus `DataMap`
- 脚本节点（Go/Python/Java）通过 `json_output: true` 协议：框架注入 `AF_OUTPUT_FORMAT=json` 环境变量，节点将输出写为扁平 JSON 对象，框架解析后将每个键写入 DataBus
- 节点可通过 `node_runtime` DSL 字段在 workflow 启动时自动注册消息/事件处理器

### DataBus 键约定

| 键名 | 写入节点 | 用途 |
|---|---|---|
| `session:<sid>:context` | `fill_context`（Go） | session 上下文摘要 |
| `session:<sid>:skill_selected` | `select_skill`（C++） | 选中的技能名称 |
| `session:<sid>:skill_result` | skill 节点 stdout | 技能执行结果 |
| `coordinator:selected_skills` | `collect`（C++） | 所有 session 汇总技能 |
| `coordinator:task_plan` | `plan`（C++） | 依赖计划 |
| `coordinator:orchestration_plan` | `apply`（C++） | 编排决策（start/stop/skip） |

跨 session DataBus 访问通过以下框架 API 实现：
```cpp
// MMAgent 新增 API
std::vector<std::string> get_all_session_ids() const;
bool get_session_output(const std::string& session_id, const std::string& key, DataVariant& value) const;

// SessionTaskCoordinator 新增 API
void aggregate_session_results(MMAgent& agent, const std::vector<std::string>& output_keys);
```

---

## 协调器编排增强

`node_apply_task_plan` 实现了基于依赖图的任务编排：

1. **解析依赖图**：从 `task_plan` JSON 读取任务列表和依赖关系
2. **状态检查**：读取 `package/runtime/<task_id>_status.txt` 中的持久化状态
3. **决策逻辑**：
   - `start`：所有前置依赖已完成
   - `waiting`：有前置依赖尚未完成
   - `skip`：有前置依赖已失败
   - `stop`：任务结果已可用（成功/失败）
   - `already_done`：历史已完成
4. **输出**：将 `orchestration_plan` JSON 写入 DataBus，供 agent 回复生成使用

---

## LLM配置
采用 OpenAI Chat Completions 兼容接口，环境变量：
- `LLM_BASE_URL`
- `LLM_API_KEY`
- `LLM_MODEL`

默认无 `LLM_API_KEY` 时会返回 mock 文本，但仍走完整 agent+skills 流程。

---

## 构建与运行

### 一键构建启动
```bash
bash examples/multi_session_chat/scripts/build_and_run.sh
```
该脚本会：
1. 构建 C++ 框架和 `libmulti_session_nodes` SO 库
2. 编译 Java `SkillDelegateNode.jar`（若 `javac` 可用）
3. 验证 `go` 命令可用（Go 节点在运行时通过 `go run` 执行）
4. 启动 chat server

### 手动构建
```bash
# 构建 C++ + SO 库
./build.sh build

# 构建 Java JAR
bash examples/multi_session_chat/nodes/java/build.sh

# 启动服务器
./build/example_chat_server \
  --package examples/multi_session_chat/package \
  --web-root examples/multi_session_chat/server/webui \
  --host 127.0.0.1 --http-port 8088 --ws-port 8765
```

### Linux 注意事项
Linux 上 SO 库后缀为 `.so`，需将以下 workflow JSON 中的 `command` 字段路径从 `.dylib` 改为 `.so`：
- `default_dispatcher_workflow.json`
- `default_coordinator_workflow.json`
- `session_dispatcher_workflow.json`

---

## 自动化对话测试
```bash
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```

