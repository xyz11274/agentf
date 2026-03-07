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

2. **多轮对话包 + 同质化 session 池 + 顶层 contextupdater**
   - `agent.json` 设置 `session_pool_size: 5`，并只保留 1 份同质化 session 模板
   - 顶层 `tasks[0]` 使用 `contextupdater`，绑定 `session_context_workflow.json`
   - 每个 pooled session 固定包含 1 个 dispatcher、1 个 coordinator 与 4 个 skill 定义；每个 skill 默认展开 2 个任务池实例
   - dispatcher / coordinator 都通过 task 级双队列（`intent_request` / `data_exchange`）与 runtime worker 协作

3. **session_context_workflow**
    `session-context-workflow` 节点链路（**Go 实现**）：  
    读取 prompt 模板 → 读取历史 → LLM 摘要 → 填充 context  
    各节点通过 `AF_*` 环境变量接收 DataBus 输入，以 JSON 形式输出到 DataBus；如需跨语言共享运行态数据，还可以使用 `AF_DATABUS_DIR` 指向的文件桥接接口。

4. **4个不同 skill**
   - `skill-search-workflow`（shell_script）
   - `skill-summary-workflow`（shell_script）
   - `skill-date-workflow`（**Python** 实现）
   - `skill-delegate-workflow`（**Java** 实现）

5. **默认 session 响应编排（真实回复）**  
   `session-coordinator-workflow` 通过 C++ SO 节点 `node_apply_task_plan` 实现任务依赖图评估：
   - 分析跨 session 的技能选择结果
   - 对每个任务作 `start / waiting / skip / stop` 决策
   - 将编排计划写入 DataBus（键 `coordinator:orchestration_plan`）

---

## 多语言节点架构

本示例采用多语言节点实现，不同 workflow 的节点分别用不同语言编写：

| Workflow | 节点 | 实现语言 | 执行模式 |
|---|---|---|---|
| `default-dispatcher-workflow` | `pipeline` | **C++** | `so_library` |
| `session-coordinator-workflow` | `collect`, `plan`, `apply` | **C++** | `so_library` |
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
输入通过 `AF_<KEY>` 环境变量注入；输出以 JSON 对象写入 stdout（`json_output: true`）。如需主动访问共享总线，可使用 `publish/sdk/go/agentflow_databus.go`。

### Python 节点（skill-date-workflow）
文件：`nodes/python/skill_date_node.py`  
实现了完整的中文日期 NLP 解析（本周/下周 + 第N周），无需外部依赖。  
设置 `AF_OUTPUT_FORMAT=json` 时输出 `{"date_result": "...", "status": "ok"}`。若接入统一 LLM 服务，可直接读取 `AF_LLM_BASE_URL / AF_LLM_API_KEY / AF_LLM_MODEL`。

### Java 节点（skill-delegate-workflow）
文件：`nodes/java/SkillDelegateNode.java`  
编译产物：`nodes/java/skill_delegate.jar`  
无外部依赖，内置简单 JSON 序列化。构建命令：
```bash
bash examples/multi_session_chat/nodes/java/build.sh
```

---

## 消息流与 DataBus 集成

所有节点间的数据共享通过 **AgentFlow DataBus** 完成，用户请求通过 **SessionRouter + task 双队列** 驱动，workflow 侧已切换到 typed node I/O + `value/ref` 传递模型。

### 文件型 DataBus SDK

- Shell：`publish/sdk/shell/agentflow_databus.sh`
- Python：`publish/sdk/python/agentflow_databus.py`
- Go：`publish/sdk/go/agentflow_databus.go`
- Java：`publish/sdk/java/AgentFlowDataBus.java`
- C++：`include/external/runtime/databus_sdk.h`

worker 在执行脚本/外部进程节点时会自动准备 `AF_DATABUS_DIR` 目录：

- `kv/*.kv`：最新键值快照（Set/Get）
- `events/*.evt`：发布/订阅事件（Sub/Pub）

统一流式推理走 DataBus 事件：

1. 节点发布 `agentflow.llm.request`
2. payload 是 JSON 字符串，至少包含 `request_id`、`prompt`、`channel`
3. runtime 发布 `agentflow.llm.chunk.<id>`、`agentflow.llm.done.<id>`、`agentflow.llm.error.<id>`

### 请求流水线（ChatAgentBridge）

```
用户请求
    ↓
Phase 1: SessionRouter 按 user_id + client_id 匹配/分配 pooled session
         → 请求进入 dispatcher task.intent_request
    ↓
Phase 2: dispatcher workflow 在 worker 侧执行
         → skills 选择结果通过 dispatcher task.data_exchange 回流
    ↓
Phase 3: dispatcher 将 selected_skills + session context 附加信息
         转发到 coordinator task.intent_request
    ↓
Phase 4: coordinator 依据 workflow 红/黄/绿等级决定可直接启动的 skill
         → 启动的 skill workflow 在 worker 侧执行
    ↓
Phase 5: 各 skill 结果通过 coordinator task.data_exchange 汇总
         → coordinator workflow 重新编排并生成最终响应
```

### DataBus 键约定

| 键名 | 写入方 | 读取方 | 说明 |
|---|---|---|---|
| `user_text` | ChatAgentBridge | `node_pipeline` | 原始用户输入 |
| `input` | ChatAgentBridge | `node_pipeline` | 多行 payload（session/user/history） |
| `dispatcher_done` | `node_pipeline` outputs | ChatAgentBridge Phase 2 | 值为 1，作分阶段完成信号 |
| `session:<sid>:skill_selected` | `node_pipeline` outputs | `node_collect_skills` | 各 session 选中的技能名 |
| `selected_skills` | `node_collect_skills` outputs | `node_plan_dependencies` | 所有 session 技能选择 JSONL |
| `task_plan` | `node_plan_dependencies` outputs | `node_apply_task_plan` | LLM 生成的任务依赖图 JSON |
| `orchestration_plan` | `node_apply_task_plan` outputs | ChatAgentBridge Phase 3 | start/waiting/skip/stop 决策 JSON |
| `session:<sid>:skill_result` | skill workflow 节点 | ChatAgentBridge Phase 5 | 技能执行结果文本 |
| `session:<sid>:context` | `fill_context`（Go）outputs | Go 节点内部 | session 上下文摘要 |
| `final_response` | ChatAgentBridge Phase 5 | HTTP/WS 响应层 | 最终汇总响应文本 |

### DataBus 输入注水（Input Hydration）

框架 `process_node()` 在执行节点 `execute()` 前，自动将 DataBus 中与节点声明 `inputs` 字段对应的值合并到节点 `inputs_` 中：
- 节点显式传入的 input 优先级不变（输入非空时不被 DataBus 值覆盖）；
- 节点可在 workflow JSON 中声明 typed `inputs` / `outputs`，框架按字段名自动注水，无需文件读取。

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
4. **输出**：将 `orchestration_plan` JSON 写入 DataBus，ChatAgentBridge 读取后串行调用对应技能工作流

ChatAgentBridge 根据 `orchestration_plan` 中每条 `action="start"` 的记录，将 `skill` 字段映射到技能工作流：

| skill 字段 | 对应工作流 | 执行模式 |
|---|---|---|
| `skill_search` | `skill-search-workflow` | shell_script |
| `skill_summary` | `skill-summary-workflow` | shell_script |
| `skill_date` | `skill-date-workflow` | python_script |
| `skill_delegate` | `skill-delegate-workflow` | java_jar |

> **串行限制**：同一 agent 的同一 workflow 共享 node 对象，并发执行会导致状态竞争，因此技能工作流顺序执行。

---

## LLM配置
采用 OpenAI Chat Completions 兼容接口，环境变量：
- `AF_LLM_BASE_URL`
- `AF_LLM_API_KEY`
- `AF_LLM_MODEL`

默认无 `AF_LLM_API_KEY` 时会返回 mock 文本，但仍走完整 agent+skills 流程。

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
- `session_coordinator_workflow.json`
- `session_dispatcher_workflow.json`

---

## 自动化对话测试
```bash
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```
