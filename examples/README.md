# AgentFlow 示例说明

## 目录结构

```
examples/
├── features/              # 单点功能 C++ 示例（DataBus、Adapter、Metrics 等）
├── agent/                 # Agent 包示例
│   ├── basic_agent/           — 基础 agent（shell_script 节点）
│   ├── calculator_agent/      — 计算器 agent（Python 脚本节点）
│   ├── nested_workflow_agent/ — 嵌套 workflow_ref 示例
│   └── workflow_patterns/     — Workflow 模式参考模板（条件/循环/顺序）
└── multi_session_chat/    # 多轮多 session 对话完整示例
```

## 构建与运行

```bash
./build.sh build

./build.sh example databus
./build.sh example adapter
./build.sh example metrics
./build.sh example storage
```

## features/ — 单点功能示例

| 文件 | 说明 |
|---|---|
| `databus_example.cpp` | MMDataBus 零拷贝读写、订阅、指针访问 |
| `adapter_example.cpp` | Actor 适配器调用示例 |
| `metrics_example.cpp` | 指标采集与导出示例 |
| `storage_example.cpp` | MMAgent/MMWorkflow 存储读写示例 |
| `agent_runner.cpp` + `interactive_runner.cpp` | 加载并交互运行 Agent 包的命令行工具 |

相关设计文档：`agent_runner_design.md`、`interactive_runner_demo.md`

## agent/ — Agent 包示例

### basic_agent
基础 agent 包，含 shell_script 节点，供 E2E 测试使用。

### calculator_agent
Python 脚本节点示例，演示数学表达式计算。

### nested_workflow_agent
演示 `workflow_ref` 嵌套调用，含数据管道、验证、处理三层 workflow。

### workflow_patterns
**Workflow 模式参考模板**（不依赖外部脚本，仅演示 DSL 编排方式）：

| 文件 | 模式 |
|---|---|
| `conditional_workflow.json` | 条件分支：按 request_type 路由到不同处理节点（A/B/C 三路 + 聚合） |
| `loop_workflow.json` | 循环迭代：FOR_EACH 遍历集合，配置 loop_config 逐条处理 |
| `sample_workflow.json` | 顺序处理管道：输入验证 → 数据转换 → 增强 → 过滤 → 输出（含条件边） |

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

业务侧通过 Actor 消息投递 `request.message`，`MMAgent` 内部事件队列触发 `AgentCoordinator` 会话处理，默认生成 `task0` 类型 task，并由 `MMAgentActor` 统一维护生命周期并继续上报 Runtime。

### 4) coordinator(task0) 固定流程定制

可在 MMAgent 包 `package.json` 中配置：

```json
{
  "coordinator_workflow": "basic-coordinator-workflow"
}
```

当请求消息未显式指定 `workflow_id` 时，系统会优先使用 `coordinator_workflow`。示例中的 `coordinator_workflow.json` 展示了 session 创建节点与 task 创建节点的编排方式。

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

### 3) 构建脚本与自动化对话测试

```bash
bash examples/multi_session_chat/scripts/build_and_run.sh
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```


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

业务侧通过 Actor 消息投递 `request.message`，`MMAgent` 内部事件队列触发 `AgentCoordinator` 会话处理，默认生成 `task0` 类型 task，并由 `MMAgentActor` 统一维护生命周期并继续上报 Runtime。

### 4) coordinator(task0) 固定流程定制

可在 MMAgent 包 `package.json` 中配置：

```json
{
  "coordinator_workflow": "basic-coordinator-workflow"
}
```

当请求消息未显式指定 `workflow_id` 时，系统会优先使用 `coordinator_workflow`。示例中的 `coordinator_workflow.json` 展示了 session 创建节点与 task 创建节点的编排方式。

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
- 默认 session dispatcher（task0）通过 workflow 节点示例脚本把请求广播到其他 session inbox。
- 非默认 session dispatcher（task0）示例节点会读取 session context、执行 skill 选择并创建“未启动”的 skill task 计划文件。
- session_context_workflow 示例节点包含：模板读取、历史读取、LLM 摘要、context 填充。
- 四类 skill workflow：联网搜索、结果总结、日期计算、Agent 分发（样例脚本可替换为真实实现）。
- 默认 session coordinator（task1）示例节点会收集已选 skill、调用 LLM 规划依赖并落盘 task 状态编排结果。

### 5) 构建脚本与自动化对话测试

```bash
bash examples/multi_session_chat/scripts/build_and_run.sh
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```
