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

所有节点间的数据共享通过 **AgentFlow DataBus** 完成，用户请求通过 **agent msgque 消息队列**驱动，彻底取代了旧的文件交换方式。

### 请求流水线（ChatAgentBridge）

```
用户请求
    ↓
Phase 1: route_request_message(TASK_REQUEST)
         process_all_sessions_until_idle()
         → generated_tasks_ 入队
    ↓
后台轮询线程 agent_task_poller_loop() 消费任务
         → execute_workflow("default-dispatcher-workflow")
    ↓
Phase 2: 等待 dispatcher_done=1 出现在 DataBus
    ↓
Phase 3: execute_workflow("default-coordinator-workflow")
         → collect → plan → apply
         → 等待 orchestration_plan 出现在 DataBus
    ↓
Phase 4: 遍历 orchestration_plan，对 action="start" 的任务
         串行执行对应技能工作流
    ↓
Phase 5: 聚合所有 session:<sid>:skill_result
         → 返回最终响应
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
- 节点只需在 workflow JSON 中声明 `"inputs": ["key1", "key2"]`，框架自动注水，无需文件读取。

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

