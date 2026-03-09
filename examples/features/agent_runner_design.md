# MMAgent Runner 设计文档

## 概述

MMAgent Runner 是 AgentFlow 框架的独立可执行程序，用于加载、验证和执行 MMAgent Package。它支持两种运行模式：

1. **批处理模式** - 执行一次命令后退出
2. **交互式模式** - 启动常驻进程，通过消息驱动架构接受用户输入并驱动 workflow 执行

## 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                      agent_runner                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐         │
│  │   CLI       │    │   Package   │    │   MMAgent     │         │
│  │   Parser    │───▶│   Loader    │───▶│   Engine    │         │
│  └─────────────┘    └─────────────┘    └─────────────┘         │
│         │                  │                  │                 │
│         ▼                  ▼                  ▼                 │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐         │
│  │   Command   │    │   MMWorkflow  │    │   Executor  │         │
│  │   Router    │    │   Manager   │    │   (Worker)  │         │
│  └─────────────┘    └─────────────┘    └─────────────┘         │
│         │                                                        │
│         ▼                                                        │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │               InteractiveRunner (Message-Driven)         │   │
│  │                                                          │   │
│  │  用户输入 → Message → MMAgent::route_request_message()     │   │
│  │      ↓                                                   │   │
│  │  Session.receive_queue                                   │   │
│  │      ↓                                                   │   │
│  │  SessionTaskCoordinator::process_session_messages()      │   │
│  │      ↓                                                   │   │
│  │  MMAgentTask 生成 (关联 workflow_id)                        │   │
│  │      ↓                                                   │   │
│  │  MMAgentRuntime::schedule_task()                           │   │
│  │      ↓                                                   │   │
│  │  MMWorkflow DAG 执行                                        │   │
│  │      ↓                                                   │   │
│  │  Agent.response_queue → 输出给用户                       │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 消息驱动架构

交互式模式现在使用正确的消息驱动架构，用户输入通过框架的消息队列驱动 workflow 执行：

```
┌─────────────────────────────────────────────────────────────────┐
│                    Message-Driven Execution Flow                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. InteractiveRunner::submit_user_request()                    │
│     ┌─────────────────────────────────────────────────────┐    │
│     │  构造 MMAgentMsgQue::Message                          │    │
│     │  - message_id: "msg_1_1234567890"                   │    │
│     │  - type: TASK_REQUEST                               │    │
│     │  - content: 用户输入                                 │    │
│     │  - metadata["workflow_id"]: 目标workflow            │    │
│     │  - metadata["user_id"]: "interactive_user"          │    │
│     │  - metadata["client_id"]: "cli"                     │    │
│     └─────────────────────────────────────────────────────┘    │
│                         │                                       │
│                         ▼                                       │
│  2. MMAgent::route_request_message(message)                       │
│     ┌─────────────────────────────────────────────────────┐    │
│     │  消息路由到 default_session                          │    │
│     │  或创建新的 user:client session                      │    │
│     └─────────────────────────────────────────────────────┘    │
│                         │                                       │
│                         ▼                                       │
│  3. MMAgent::process_all_sessions_until_idle()                    │
│     ┌─────────────────────────────────────────────────────┐    │
│     │  SessionTaskCoordinator 处理消息                     │    │
│     │  生成 MMAgentTask 并绑定 workflow                      │    │
│     └─────────────────────────────────────────────────────┘    │
│                         │                                       │
│                         ▼                                       │
│  4. MMAgentRuntime::schedule_task()                               │
│     ┌─────────────────────────────────────────────────────┐    │
│     │  通过三级调度执行 MMWorkflow DAG                       │    │
│     │  MMAgent -> MMWorkflow -> MMNode                           │    │
│     └─────────────────────────────────────────────────────┘    │
│                         │                                       │
│                         ▼                                       │
│  5. InteractiveRunner::poll_results()                           │
│     ┌─────────────────────────────────────────────────────┐    │
│     │  持续轮询 Agent response queue，直到执行安静         │    │
│     │  合并所有分批返回的响应后输出给用户                  │    │
│     └─────────────────────────────────────────────────────┘    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 关键接口复用

| 组件 | 接口 | 文件位置 |
|------|------|----------|
| 消息路由 | `MMAgent::route_request_message()` | `src/agent/agent.cpp` |
| 会话管理 | `MMAgent::get_or_create_session()` | `src/agent/agent.cpp` |
| 消息处理 | `MMAgent::process_all_sessions_until_idle()` | `src/agent/agent.cpp` |
| 任务获取 | `MMAgent::pop_generated_task()` | `src/agent/agent.cpp` |
| 任务调度 | `MMAgentRuntime::schedule_task()` | `src/runtime/runtime.cpp` |
| 输出收集 | `MMAgent::dequeue_response_message()` / `InteractiveRunner::poll_results()` | `src/agent/agent.cpp` / `examples/features/interactive_runner.cpp` |

## 运行模式

### 批处理模式 (Batch Mode)

执行一次命令后退出，适合自动化测试和脚本调用。

```bash
agent_runner <package_path> [options]
```

### 交互式模式 (Interactive Mode)

启动常驻进程，通过消息驱动架构接受用户命令并执行 workflow。

```bash
agent_runner <package_path> --interactive
agent_runner <package_path> --interactive --verbose  # 启用详细输出
```

交互式模式命令：

```
Available commands:
  execute <workflow_id> [--input <text>]  Execute a workflow
  list                                   List available workflows
  status                                 Show agent status
  help                                   Show this help
  quit / exit                            Exit interactive mode

Example:
  execute my-workflow --input "hello world"
```

交互式模式架构：

```
┌─────────────────────────────────────────────────────────────────┐
│                    Interactive Mode Flow                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. 启动常驻进程                                                 │
│     $ agent_runner package_path --interactive                   │
│                                                                 │
│  2. 初始化 MMAgent 和 Runtime                                      │
│     > MMAgent loaded: basic-agent                                 │
│     > Runtime started with worker threads                       │
│     > Ready. Type 'help' for commands.                          │
│                                                                 │
│  3. 等待用户输入                                                 │
│     > _                                                         │
│                                                                 │
│  4. 用户输入命令                                                 │
│     > execute basic-workflow-v2 --input "hello world"           │
│                                                                 │
│  5. 消息驱动执行流程                                             │
│     Message → Session → Coordinator → Task → Runtime            │
│                                                                 │
│  6. 输出结果                                                     │
│     > Result: processed output                                  │
│     > _                                                         │
│                                                                 │
│  7. 继续等待输入 (循环)                                          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 命令行接口

### 用法

```bash
# 批处理模式
agent_runner <package_path> [options]

# 交互式模式
agent_runner <package_path> --interactive
agent_runner <package_path> -i
```

### 全局选项

| 选项 | 参数 | 说明 |
|------|------|------|
| `--validate` | - | 验证 package 结构 |
| `--list-workflows` | - | 列出所有 workflow |
| `--execute` | `[workflow_id]` | 执行 workflow |
| `--output-file` | `<file>` | 输出重定向到文件 |
| `--interactive`, `-i` | - | 启动交互式模式 |
| `--help`, `-h` | - | 显示帮助信息 |

### 交互式命令

进入交互式模式后支持的命令：

| 命令 | 语法 | 说明 |
|------|------|------|
| `help` | `help` | 显示帮助信息 |
| `list` | `list` | 列出所有 workflow |
| `info` | `info <workflow_id>` | 显示 workflow 详情 |
| `execute` | `execute <workflow_id> [input]` | 执行 workflow |
| `input` | `input <workflow_id> <data>` | 设置 workflow 输入数据 |
| `run` | `run <workflow_id>` | 使用已设置的输入运行 workflow |
| `status` | `status` | 显示当前状态 |
| `clear` | `clear` | 清除屏幕 |
| `quit` | `quit` / `exit` | 退出程序 |

## 交互式模式设计

### 输入处理流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    Input Processing                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  用户输入                                                        │
│  "execute basic-workflow --input 'hello'"                       │
│       │                                                         │
│       ▼                                                         │
│  ┌─────────────────┐                                            │
│  │   Tokenizer     │  分词: ["execute", "basic-workflow",       │
│  └────────┬────────┘           "--input", "hello"]              │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────┐                                            │
│  │   Parser        │  解析: command="execute",                  │
│  └────────┬────────┘         args={workflow: "basic-workflow",  │
│                                input: "hello"}                  │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────┐                                            │
│  │   Validator     │  验证: workflow 存在? 参数有效?            │
│  └────────┬────────┘                                            │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────┐                                            │
│  │   Executor      │  执行: 调用 workflow engine                │
│  └────────┬────────┘                                            │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────┐                                            │
│  │   Output        │  输出: 结果格式化显示                       │
│  └─────────────────┘                                            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 数据传递机制

```
┌─────────────────────────────────────────────────────────────────┐
│                    Data Flow in MMWorkflow                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  用户输入: "hello world"                                         │
│       │                                                         │
│       ▼                                                         │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    MMDataBus                                │   │
│  │                                                          │   │
│  │   Key                    Value                           │   │
│  │   ───                    ─────                           │   │
│  │   "user_input"    ───▶   "hello world"                   │   │
│  │   "request.id"    ───▶   "req-001"                       │   │
│  │                                                          │   │
│  └─────────────────────────────────────────────────────────┘   │
│       │                                                         │
│       ▼                                                         │
│  ┌─────────────────┐                                            │
│  │     MMNode 1      │  读取: MMDataBus.get("user_input")          │
│  │   (process)     │  写入: MMDataBus.put("node1.output", ...)   │
│  └────────┬────────┘                                            │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────┐                                            │
│  │     MMNode 2      │  读取: MMDataBus.get("node1.output")        │
│  │   (transform)   │  写入: MMDataBus.put("node2.output", ...)   │
│  └────────┬────────┘                                            │
│           │                                                     │
│           ▼                                                     │
│       输出结果                                                   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 会话状态管理

```cpp
struct InteractiveSession {
    std::string current_workflow;       // 当前选中的 workflow
    DataMap     pending_inputs;         // 待执行的输入数据
    bool        running;                // 会话运行状态
    std::vector<std::string> history;   // 命令历史
};
```

## 示例会话

```
$ agent_runner examples/agent/basic_agent --interactive

=== AgentFlow MMAgent Runner (Interactive Mode) ===
Package: basic-agent v1.0.0
Loaded 3 workflows

Type 'help' for available commands.

> list
Workflows:
  [1] basic-workflow (3 nodes)
  [2] basic-workflow-v2 (1 node, shell_script)
  [3] basic-coordinator-workflow (2 nodes)

> info basic-workflow-v2
MMWorkflow: basic-workflow-v2
  Name: Basic MMWorkflow V2
  Nodes: 1
    - node_basic (shell_script: node_basic.sh)

> execute basic-workflow-v2 --input "test data"
[EXEC] node_basic: node_basic.sh
  >> basic node
Result: Success
  stdout: basic node
  exit_code: 0

> input basic-workflow "hello world"
Input set for workflow 'basic-workflow': "hello world"

> run basic-workflow
[SKIP] start (start)
[SKIP] process (task) - no shell_script
[SKIP] end (end)
MMWorkflow completed

> status
Session Status:
  Current MMWorkflow: basic-workflow
  Pending Inputs: 1
  Commands Executed: 5

> quit
Goodbye!
```

## 执行流程

### 流程图

```
┌──────────────┐
│   启动       │
└──────┬───────┘
       │
       ▼
┌──────────────┐     ┌──────────────┐
│  参数解析    │────▶│  显示帮助    │
└──────┬───────┘     └──────────────┘
       │
       ▼
┌──────────────┐     ┌──────────────┐
│  路径解析    │────▶│  错误退出    │
└──────┬───────┘     │  (code: 1)   │
       │             └──────────────┘
       ▼
┌──────────────┐     ┌──────────────┐
│ 加载 Package │────▶│  错误退出    │
└──────┬───────┘     │  (code: 1)   │
       │             └──────────────┘
       ▼
┌──────────────┐     ┌──────────────┐
│  创建 MMAgent  │────▶│  错误退出    │
└──────┬───────┘     │  (code: 1)   │
       │             └──────────────┘
       ▼
┌──────────────┐
│  初始化 MMAgent│
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ 执行命令模式 │
└──────┬───────┘
       │
       ├──────▶ --validate: 验证 package
       │
       ├──────▶ --list-workflows: 列出 workflow
       │
       └──────▶ --execute: 执行 workflow
                      │
                      ▼
              ┌──────────────┐
              │  拓扑排序    │
              └──────┬───────┘
                     │
                     ▼
              ┌──────────────┐
              │  遍历节点    │
              └──────┬───────┘
                     │
       ┌─────────────┼─────────────┐
       │             │             │
       ▼             ▼             ▼
┌──────────┐  ┌──────────┐  ┌──────────┐
│ start/end│  │ workflow │  │  script  │
│  (跳过)  │  │  (嵌套)  │  │  (执行)  │
└──────────┘  └──────────┘  └──────────┘
                     │
                     ▼
              ┌──────────────┐
              │  输出结果    │
              └──────┬───────┘
                     │
                     ▼
              ┌──────────────┐
              │  正常退出    │
              │  (code: 0)   │
              └──────────────┘
```

## 输出格式

### Package 验证输出

```
=== AgentFlow MMAgent Runner ===
Package: examples/agent/nested_workflow_agent
Package: nested-workflow-agent v1.0.0

=== Package Validation ===
  Name: nested-workflow-agent
  Version: 1.0.0
  Description: Nested workflow agent example
  Workflows: 4
    - main_workflow.json
    - data_pipeline.json
    - validation_workflow.json
    - processing_workflow.json
  Entry Point: nested_workflow_agent.so
  Coordinator: main-workflow

[OK] Package validation passed
```

### MMWorkflow 列表输出

```
=== AgentFlow MMAgent Runner ===
Package: examples/agent/nested_workflow_agent
Package: nested-workflow-agent v1.0.0
MMAgent: nested-workflow-agent initialized

=== Workflows ===
  [main-workflow] Main MMWorkflow (4 nodes)
    └─> workflow_ref: data_pipeline.json
    └─> workflow_ref: processing_workflow.json
  [data-pipeline-workflow] Data Pipeline MMWorkflow (3 nodes)
    └─> workflow_ref: validation_workflow.json
  [validation-workflow] Validation MMWorkflow (3 nodes)
  [processing-workflow] Processing MMWorkflow (3 nodes)
```

### MMWorkflow 执行输出

```
=== AgentFlow MMAgent Runner ===
Package: examples/agent/nested_workflow_agent
Package: nested-workflow-agent v1.0.0
MMAgent: nested-workflow-agent initialized

=== Executing MMWorkflow: validation-workflow ===
Execution order: 3 nodes

--- Execution Output ---
[EXEC] check_schema: check_schema.sh
  >> Checking data schema...
[EXEC] validate_fields: validate_fields.sh
  >> Validating field values...
[EXEC] validation_report: validation_report.sh
  >> Validation passed: all checks completed

--- Execution Summary ---
Executed: 3 nodes
Success: 3 nodes

[OK] MMWorkflow execution completed
```

## 返回码

| 返回码 | 说明 |
|--------|------|
| 0 | 成功 |
| 1 | 错误 (package 未找到、加载失败、执行失败等) |

## 节点类型处理

| 节点类型 | 执行模式 | 处理方式 |
|---------|---------|---------|
| `start` | - | 跳过，输出 `[SKIP]` |
| `end` | - | 跳过，输出 `[SKIP]` |
| `task` | `shell_script` | 执行脚本，输出 `[EXEC]` |
| `task` | `cpp_function` | 跳过，输出 `[SKIP]` |
| `workflow` | - | 标记嵌套引用，输出 `[NESTED]` |

## 路径解析

MMAgent Runner 支持多种路径格式：

```bash
# 绝对路径
agent_runner /path/to/agent/package

# 相对于当前目录
agent_runner examples/agent/basic_agent

# 相对于可执行文件目录
agent_runner ../examples/agent/basic_agent
```

路径解析顺序：
1. 原始路径
2. 相对于可执行文件目录
3. 相对于可执行文件目录的父目录

## 黑盒测试集成

### 测试脚本模板

```bash
#!/bin/bash

AGENT_RUNNER="./agent_runner"
PACKAGE_PATH="examples/agent/basic_agent"

# 执行并捕获输出
OUTPUT=$("$AGENT_RUNNER" "$PACKAGE_PATH" --execute workflow-id 2>/dev/null)

# 验证输出
if echo "$OUTPUT" | grep -q "expected content"; then
    echo "PASS"
else
    echo "FAIL"
fi

# 验证返回码
"$AGENT_RUNNER" "$PACKAGE_PATH" --validate >/dev/null 2>&1
EXIT_CODE=$?
if [ $EXIT_CODE -eq 0 ]; then
    echo "PASS: exit code is 0"
fi
```

### 验证点

1. **输出内容验证**
   - Package 名称、版本
   - MMWorkflow 列表
   - 脚本执行输出
   - 嵌套引用关系

2. **返回码验证**
   - 有效 package 返回 0
   - 成功执行返回 0
   - 无效路径返回非 0
   - 执行失败返回非 0

3. **执行顺序验证**
   - 拓扑排序正确
   - 节点按序执行

## 扩展性

### 添加新的执行模式

```cpp
// 在 execute 分支添加新模式
if (node->get_execution_mode() == ExecutionMode::PYTHON_SCRIPT) {
    // Python 脚本执行逻辑
}
```

### 添加新的命令选项

```cpp
// 参数解析添加新选项
} else if (arg == "--new-option") {
    new_option = true;
}

// 命令路由添加新分支
if (new_option) {
    // 新选项处理逻辑
}
```

## 文件结构

```
AgentFlow/
├── examples/
│   └── agent_runner.cpp      # 源代码
│
├── build/
│   └── agent_runner          # 编译产物
│
└── tests/
    └── e2e/
        └── blackbox/
            ├── test_blackbox_basic_agent.sh
            └── test_blackbox_nested_workflow.sh
```

## 依赖关系

```
agent_runner
    │
    ├── agentflow (静态库)
    │     ├── agent (MMAgent, Package)
    │     ├── workflow (MMWorkflow, MMNode, Parser)
    │     └── runtime (MMWorkerEngine)
    │
    ├── std::filesystem (路径处理)
    │
    └── pthreads (线程)
```

## 设计原则

1. **独立性** - 作为独立可执行程序运行，不依赖测试框架
2. **标准化** - 输出格式标准化，便于解析和验证
3. **可扩展** - 支持添加新的执行模式和命令选项
4. **错误处理** - 明确的错误信息和返回码
5. **黑盒友好** - 设计为黑盒测试的理想目标
