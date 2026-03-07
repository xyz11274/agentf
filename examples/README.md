# AgentFlow 示例说明

## 目录结构

```
examples/
├── features/              # 单点功能 C++ 示例
├── agent/                 # Agent 包示例
│   ├── basic_agent/
│   ├── calculator_agent/
│   ├── nested_workflow_agent/
│   └── workflow_patterns/
└── multi_session_chat/    # 多轮多 session 对话完整示例
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
| `loop_workflow.json` | FOR_EACH 循环：`loop_config` 遍历集合逐条处理 |
| `sample_workflow.json` | 顺序管道：验证 → 转换 → 增强 → 过滤 → 输出（含条件边） |

---

## multi_session_chat/ — 多 session 对话示例

多轮对话、多语言节点、DataBus 驱动的完整示例，详见 [`multi_session_chat/README.md`](multi_session_chat/README.md)。



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

业务侧通过 Actor 消息投递 `request.message`，`MMAgent` 内部事件队列触发会话处理，默认进入 0 号 `talker` session，并生成 `dispatcher` 类型任务，由 `MMAgentActor` 统一维护生命周期并继续上报 Runtime。

### 4) dispatcher / coordinator 固定流程定制

可在 MMAgent 包 `package.json` 中配置：

```json
{
  "coordinator_workflow": "basic-coordinator-workflow"
}
```

当请求消息未显式指定 `workflow_id` 时，系统会优先使用 session 中 dispatcher / coordinator 任务绑定的 workflow。示例中的 `coordinator_workflow.json` 展示了结果编排方式。

## WebSocket 对话样例（multi_session_chat）

### 1) 构建 AgentFlow

```bash
./build.sh build
```

### 2) 启动样例进程

```bash
./build/example_chat_server \
  --package examples/multi_session_chat/package \
  --host 127.0.0.1 --http-port 8088 --ws-port 8765 \
  --web-root examples/multi_session_chat/server/webui
```

打开 `http://127.0.0.1:8088` 即可通过 WebUI 与进程进行 WebSocket 对话。

### 4) 样例能力说明

- 对话管理模块将消息按 `session_id` 追加保存到 `examples/multi_session_chat/package/history/*.jsonl`。
- 默认 talker session 的 dispatcher 通过 workflow 节点示例脚本把请求广播到其他 thinker session inbox。
- thinker session 的 dispatcher 示例节点会读取 session context、执行 skill 选择并创建“未启动”的 skill task 计划文件。
- `contextkeeper` 任务对应的 `session_context_workflow` 示例节点包含：模板读取、历史读取、LLM 摘要、context 填充。
- 四类 skill workflow：联网搜索、结果总结、日期计算、Agent 分发（样例脚本可替换为真实实现）。
- 默认 talker session 的 coordinator 示例节点会收集已选 skill、调用 LLM 规划依赖并落盘 task 状态编排结果。

### 5) 构建脚本与自动化对话测试

```bash
bash examples/multi_session_chat/scripts/build_and_run.sh
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```
