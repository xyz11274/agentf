# AgentFlow Assistant — Agent Package 创建助手

AgentFlow Assistant 是一个**对话式引导助手**，帮助用户通过问答交互逐步创建 AgentFlow agent package。它运行在 AgentFlow 框架的 harness 模式（LLM 内部循环）之上，通过工具调用自动生成完整的 agent package 文件。

## 设计目的

在 **Agent Flow UI** 界面中，用户可以通过与 Assistant 的对话，**渐进式地**创建一个完整的 agent package，而无需手动编写配置文件。

### 用户交互流程

```
用户打开 Agent Flow UI
    │
    ▼
用户选择 AgentFlow Assistant
    │
    ▼
Assistant 欢迎用户，询问想要创建什么类型的 agent
    │
    ▼
    ┌─ Q&A 循环 ─────────────────────────────────┐
    │                                             │
    │  Assistant: "你想给这个 agent 起什么名字？"   │
    │  用户: "my_data_processor"                   │
    │                                              │
    │  Assistant: "你想要哪种架构类型？"            │
    │  Assistant: 展示模板列表...                   │
    │  用户: "harness"                             │
    │                                              │
    │  Assistant: "需要哪些工具？"                  │
    │  用户: "bash, read_file"                     │
    │                                              │
    │  ... 继续对话收集所有必要信息                  │
    │                                              │
    └──────────────────────────────────────────────┘
    │
    ▼
Assistant 调用 create_agent_package 工具
    │
    ▼
所有文件生成到 examples/agent/{agent_name}/
    │
    ▼
Assistant 告知用户生成结果
```

## 架构

```
用户请求 (通过 UI)
    │
    ▼
dispatcher (next_skills: ["agentflow_assistant_run"])
    │  框架路由 → 启动 skill task
    ▼
skill: agentflow_assistant_run (assistant_workflow.json)
    │  ┌──────────────────────────────────────────────────────┐
    │  │  assistant_loop.py                                   │
    │  │                                                      │
    │  │  1. 读取 agent.md → 模型配置 + 系统提示词             │
    │  │  2. 从 DataBus 读取历史消息 (ctx/session_messages)    │
    │  │  3. while True:                                      │
    │  │    ├── LLM 思考（带 tools 定义）                      │
    │  │    ├── tool_calls → tool_registry.execute()          │
    │  │    │   (list_templates / read_example /               │
    │  │    │    create_agent_package / validate_package)      │
    │  │    └── respond → 写回历史 → 退出循环                  │
    │  └──────────────────────────────────────────────────────┘
    │
    ▼
coordinator (coordinator_collect.json)
    │  coordinator_collect.py → 透传 skill 输出
    ▼
  UI 收到响应
```

## 目录结构

```
agentflow_assistant/
├── agent.json                     # 任务定义（dispatcher + coordinator + skill）
├── agent.md                       # 系统提示词 + 模型配置（YAML frontmatter）
├── package.json                   # 包元数据
├── assistant_workflow.json        # skill workflow（单节点：assistant_loop）
├── coordinator_collect.json       # coordinator workflow（单节点：coordinator_collect）
├── README.md                      # 本文档
├── .env.example                   # 环境变量模板
├── nodes/
│   ├── assistant_loop.py          # LLM 内部循环（核心 — Q&A 引导逻辑）
│   ├── databus_client.py          # DataBus 客户端（复用框架官方 UDS SDK）
│   └── coordinator_collect.py     # coordinator 透传节点
└── assistant/
    └── tools/
        ├── __init__.py            # 自动发现并注册工具
        ├── list_templates.py      # 列出可用的 agent 模板类型
        ├── read_example.py        # 读取现有 agent 作为参考
        ├── create_agent_package.py# 根据用户需求生成完整的 agent package
        └── validate_package.py    # 验证生成的 agent package 结构
```

## 可用工具

| 工具 | 用途 |
|------|------|
| `list_templates` | 列出可用的 agent 模板类型（harness/basic/multi-session/nested-workflow/custom） |
| `read_example` | 读取 `examples/agent/` 下已有 agent 的目录结构或文件内容，供用户参考 |
| `create_agent_package` | **核心工具**：根据用户通过问答提供的需求，生成完整的 agent package 并保存到 `examples/agent/` |
| `validate_package` | 验证生成的 agent package 结构是否完整（必选文件、JSON 合法性、workflow 引用等） |

## Q&A 引导流程

Assistant 通过以下阶段逐步引导用户：

### 阶段 1：欢迎与目标确认
- 自我介绍
- 询问用户想要创建什么类型的 agent

### 阶段 2：渐进式信息收集
- 每轮只问 1-2 个问题
- 先问名称、描述等基本信息
- 展示模板类型供用户选择
- 根据选择深入询问具体需求（工具、节点、MCP 等）

### 阶段 3：生成与验证
- 收集完所有信息后调用 `create_agent_package`
- 告知生成结果和文件列表
- 调用 `validate_package` 验证
- 询问是否需要调整

## 支持的模板类型

| 类型 | 描述 | 适用场景 |
|------|------|----------|
| `harness` | LLM 内部循环模式 | 单一 LLM 驱动、工具调用串行 |
| `basic` | 基础 DAG 模式 | 数据处理、条件分支 |
| `multi-session` | 多会话聊天模式 | 复杂的对话场景 |
| `nested-workflow` | 嵌套工作流模式 | 复杂任务拆解 |
| `custom` | 自定义组合 | 灵活混合各种模式 |

## 配置

### agent.md YAML frontmatter

| 字段 | 说明 |
|------|------|
| `model.name` | 模型名称 |
| `model.base_url` | API 地址 |
| `model.api_key` | API 密钥（支持 `${ENV_VAR}` 格式展开） |

### 环境变量

| 变量 | 说明 |
|------|------|
| `API_KEY` | LLM API 密钥（必需） |
| `AF_SESSION_ID` | 由框架自动注入 |
| `AF_DATABUS_UDS_FILE` | 由框架自动注入 |
| `AF_INPUT` | 用户输入，由 workflow inputs 注入 |

## 运行

```bash
# 方式一：agent_runner 交互模式（无需 daemon）
./build/agent_runner examples/agent/agentflow_assistant --interactive

# 方式二：daemon + bridge + UI
./build/agentflowd --package "$(pwd)/examples/agent/agentflow_assistant" \
  --listen 127.0.0.1:17891 --log-file "$(pwd)/logs/agentflowd.log" &
cd bridge && python3 main.py &
```

## 生成的 Agent Package 示例

当用户完成所有问答后，Assistant 会调用 `create_agent_package` 生成类似以下结构的文件：

```
examples/agent/{agent_name}/
├── agent.json
├── package.json
├── {agent_name}_run.json         # (harness 模式) skill workflow
├── coordinator_collect.json
├── agent.md                      # (含 LLM 配置)
├── .env.example
├── README.md
├── nodes/
│   ├── {agent_name}_loop.py      # LLM 内部循环
│   ├── databus_client.py
│   └── coordinator_collect.py
└── tools/
    ├── __init__.py
    ├── bash.py
    ├── read_file.py
    └── ...
```
