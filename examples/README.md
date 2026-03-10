# AgentFlow 示例说明

## 目录结构

```
examples/
├── features/              # 单点功能 C++ 示例
├── agent/                 # Agent 包示例
│   ├── basic_agent/
│   ├── calculator_agent/
│   ├── claw_agent/
│   ├── multi_session_chat/
│   ├── openjiuwen_multi_session_chat/
│   ├── openjiuwen_studio_agent/
│   ├── nested_workflow_agent/
│   └── workflow_patterns/
```

---

## features/ — 单点功能示例

| 文件 | 说明 |
|---|---|
| `databus_example.cpp` | MMDataBus 零拷贝读写、订阅、指针访问 |
| `adapter_example.cpp` | Actor 适配器调用示例 |
| `metrics_example.cpp` | 指标采集与导出示例 |
| `storage_example.cpp` | MMAgent/MMWorkflow 存储读写示例 |
| `agent_runner.cpp` / `interactive_runner.cpp` | 加载并交互运行 Agent 包的命令行工具 |

构建运行：

```bash
./build.sh build
./build.sh example databus   # 或 adapter / metrics / storage
./build/agent_runner --package examples/agent/basic_agent
```

设计文档：`agent_runner_design.md`、`interactive_runner_demo.md`

---

## agent/ — Agent 包示例

每个子目录都是一个完整的 Agent 包（含 `agent.json`、`package.json`、workflow JSON、脚本）。

### basic_agent

最小化 Agent 示例，含一个 shell_script 节点（`node_basic.sh`）和 coordinator workflow。  
E2E 测试的基础数据包。

### calculator_agent

Python 脚本节点示例（`calc.py`），演示通过 workflow 驱动 Python 脚本执行数学表达式计算。

### nested_workflow_agent

演示 `workflow_ref` 嵌套 workflow 调用，三层嵌套结构：

```
main_workflow → data_pipeline (workflow_ref)
                  └→ validation_workflow (workflow_ref)
             → processing_workflow (workflow_ref)
```

E2E 测试的嵌套 workflow 数据包。

### workflow_patterns

Workflow 模式参考模板（DSL 示例，不依赖外部脚本）：

| 文件 | 模式 |
|---|---|
| `conditional_workflow.json` | 条件分支：按 `request_type` 三路路由 + 聚合 |
| `loop_workflow.json` | `switch-case` 分支：同一表达式驱动多条 case/default 出边 |
| `sample_workflow.json` | 顺序管道：验证 → 转换 → 增强 → 过滤 → 输出（含条件边） |

---

## agent/openjiuwen_studio_agent/ — Studio bundle 适配示例

将 agent-studio 编排结果作为 bundled artifact 装入 AgentFlow 包，并通过包内 Python 适配器 node 运行。包含专用 TUI 和 E2E 冒烟测试。

## agent/llm_query_agent/ — LLM Query 示例

依赖 `llm-query` 动态库的 LLM 查询示例，现提供专用 TUI 入口；运行时日志与交互窗口隔离。

## agent/multi_session_chat/ — 多 session 对话示例

多轮对话、多语言节点、DataBus 驱动的完整示例，现以专用 TUI 作为交互入口，详见 [`agent/multi_session_chat/README.md`](agent/multi_session_chat/README.md)。



## MMAgent 包创建与运行（推荐）

外部业务不再通过代码直接创建 `MMAgent/MMWorkflow/MMNode/MMEdge`，而是提供 MMAgent 包并由 `MMAgentActor` 自动加载。

### 1) 准备 AgentService.config

```json
{
  "package_root": "examples/agent",
  "manifests": [
    "basic_agent",
    "calculator_agent"
  ],
  "scan_interval_ms": 1000,
  "heartbeat_interval_ms": 1000
}
```

### 2) 启动后自动加载

`MMAgentActor` 启动后会读取上述配置，自动创建 MMAgent 实例并向 `MMRuntimeActor` 上报注册/心跳/状态。

### 3) 业务请求处理

业务侧通过 Actor 消息投递 `request.message`，`MMAgent` 通过 `SessionRouter` 按 `user_id + client_id` 命中或分配预创建 session，并生成 `dispatcher` 类型任务，由 `MMAgentActor` 统一维护生命周期并继续上报 Runtime。

### 4) dispatcher / coordinator 固定流程定制

可在 MMAgent 包 `package.json` 中配置：

```json
{
  "coordinator_workflow": "basic-coordinator-workflow"
}
```

当请求消息未显式指定 `workflow_id` 时，系统会优先使用 session 中 dispatcher / coordinator 任务绑定的 workflow。示例中的 `coordinator_workflow.json` 展示了结果编排方式。

## TUI 对话样例（multi_session_chat / openjiuwen_multi_session_chat / llm_query_agent）

### 1) 构建 AgentFlow

```bash
./build.sh build
```

### 2) 启动 multi_session_chat TUI

```bash
./build/example_multi_session_chat_tui examples/agent/multi_session_chat/package
```

### 3) 启动 openjiuwen_multi_session_chat TUI

```bash
./build/example_openjiuwen_multi_session_chat_tui examples/agent/openjiuwen_multi_session_chat/package
```

### 4) 启动 llm_query_agent TUI

```bash
LD_LIBRARY_PATH=/root/projects/llm-query/build/src/lib:$LD_LIBRARY_PATH \
  ./build/example_llm_query_agent_tui examples/agent/llm_query_agent
```

### 5) 样例能力说明

- 对话管理模块将消息按 `session_id` 追加保存到 `examples/agent/multi_session_chat/package/history/*.jsonl`。
- 主 dispatcher workflow 会把请求广播到可复用 skill session。
- skill session 的 dispatcher 示例节点会读取 session context、执行 skill 选择并创建“未启动”的 skill task 计划文件。
- agent 级 `contextupdater` 对应的 `session_context_workflow` 示例节点包含：模板读取、历史读取、LLM 摘要、context 填充。
- 四类 skill workflow：联网搜索、结果总结、日期计算、Agent 分发（样例脚本可替换为真实实现）。
- 主 coordinator 示例节点会收集已选 skill、调用 LLM 规划依赖并落盘 task 状态编排结果。
- `llm_query_agent` 提供独立 TUI，可直接验证 dispatcher/coordinator/skill 到 `llm_query_node` 的完整链路。

### 6) 构建脚本与自动化对话测试

```bash
bash examples/agent/multi_session_chat/scripts/build_and_run.sh
bash examples/agent/multi_session_chat/tests/auto_dialog_test.sh
```
