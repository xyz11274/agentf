# AgentFlow 对接演示指南

> 本文档面向对接方，演示 AgentFlow 平台的核心流程：上传 agent package → 触发请求 → 收集 trace → 更新技能 → 再次触发。
>
> 以 **穿搭助手（outfit-assistant）** 为示例 agent。
>
> 外部服务只需访问 Bridge，不需要直接连接 daemon。文本、图片和 HTML 产物均通过 Bridge API 交付。

---

## 目录

1. [环境说明](#1-环境说明)
2. [Agent Package 格式说明](#2-agent-package-格式说明)
3. [第一步：上传 Agent Package](#3-第一步上传-agent-package)
4. [第二步：触发请求（对话）](#4-第二步触发请求对话)
5. [第三步：收集 Trace](#5-第三步收集-trace)
6. [第四步：更新技能（更新 Agent Package）](#6-第四步更新技能更新-agent-package)
7. [第五步：再次触发](#7-第五步再次触发)
8. [附录：完整接口定义](#8-附录完整接口定义)

---

## 1. 环境说明

| 组件 | 地址 |
|------|------|
| Bridge (REST API) | `http://127.0.0.1:8199` |
| Bridge 业务路由前缀 | `/health`（所有 `/api/*` 与 `/ws/*` 路由均挂在此前缀下） |
| Bridge 公网地址（示例） | `https://agent.example.com/health` |
| Daemon (RPC) | `127.0.0.1:17891`（bridge 自动转发，不需直接连接） |
| Agent 包存储目录 | `examples/agent/` |

### 1.1 路由前缀说明

Bridge 通过 `app.mount("/health", bridge_app)` 将全部业务路由挂载在 `/health` 前缀下。这是对接 AISF 网关的统一入口约定，网关在转发时已自动剥离或保留该前缀（取决于网关配置）。

**前缀应用规则：**

| 场景 | 请求路径 |
|------|---------|
| 直接访问 Bridge（绕过网关） | `http://127.0.0.1:8199/health/api/...` |
| 经 AISF 网关访问 | 由网关决定（通常对外仍暴露为 `/health/api/...`） |
| WebSocket | `ws://127.0.0.1:8199/health/ws/chat/{agent_id}` |

**对接方须知：** 本文后续所有示例（第 3-7 步及附录）的请求路径均为业务路径（如 `/api/chat/{agent_id}`），实际调用时**必须前置 `/health` 前缀**，即完整路径为 `/health/api/chat/{agent_id}`。

例外：上游 SysAgent 适配层另有独立任务入口 `/health/v1/tasks`（POST，转发至 Bridge 内部回调），与本节所述业务路由前缀语义不同，详见 `frontend/bridge/aisf_adapter/sysagent/README.md`。

本机演示使用 `http://127.0.0.1:8199`，所有请求均带 `/health` 前缀。外部服务对接时，应将本文所有请求地址替换为部署方提供的 Bridge 公网地址（前缀保持不变）。

如果 Agent 会生成图片或 HTML，Bridge 必须配置对外可访问的基地址：

```bash
# frontend/bridge/.env
ARTIFACT_PUBLIC_BASE_URL=https://agent.example.com
```

该地址必须能被对接方网络实际访问。配置项只负责生成绝对 URL，不会自动配置 DNS、TLS、防火墙或反向代理。

启动顺序：

```bash
# 1. 启动 daemon（使用 outfit_assistant 自带的 Python venv）
export PATH="$(pwd)/examples/agent/outfit_assistant/.venv_unix/bin:$PATH"
./build/agentflowd --package "$(pwd)/examples/agent/outfit_assistant" \
  --listen 127.0.0.1:17891 --log-file "$(pwd)/logs/agentflowd.log" &

# 2. 启动 bridge
cd frontend/bridge && python3 main.py &
```

> 包内 venv 目录名为 `.venv_unix`（跨平台脚本 `setup.sh` 创建）。如果你的环境用其他 venv，替换 `PATH` 即可。

---

## 2. Agent Package 格式说明

一个 agent package 是磁盘上的一个目录，包含 agent 的完整定义。以 `outfit_assistant` 为例：

```
outfit_assistant/
├── package.json                   # 包元信息
├── agent.json                     # agent 定义（4 个 task）
├── agent.md                       # 模型配置 + 业务意图 + skills 列表
├── dummy_pre_skill_workflow.json  # 前置 hook workflow（dummy_pre_skill task）
├── coordinator_workflow.json      # coordinator 决策 workflow（coordinator_decide 节点）
├── skill_workflow.json            # 通用 skill 执行 workflow（skill_executor 节点）
├── setup.sh                       # 环境初始化脚本（创建 venv、装依赖）
├── nodes/
│   ├── dummy_pre_skill.py         # 前置 hook 占位节点
│   ├── coordinator_decide.py      # coordinator LLM 决策核心（run_skills 工具）
│   ├── skill_executor.py          # 通用 skill 执行器 + 双层 tool 加载
│   ├── databus_client.py          # DataBus 封装（dialogue_id 前缀对齐 + global 方法）
│   └── llm_call.py                # LLM 调用 (curl) + trace 写入（_TraceRecorder）
├── harness/
│   ├── skills/
│   │   ├── dummy_pre_skill/SKILL.md       # hook 占位 skill
│   │   ├── skill-runner/SKILL.md          # 通用 skill-runner 占位
│   │   ├── outfit-stylist/                # 综合穿搭 skill（含 weather.py 等 scripts/）
│   │   ├── forge-image-gen/               # 图片生成 skill
│   │   └── video-gen/                     # 视频生成 skill
│   └── tools/
│       ├── __init__.py            # 工具自动发现入口
│       ├── bash.py                # bash 执行
│       ├── read_file.py           # 读文件
│       ├── write_file.py          # 写文件
│       ├── web_fetch.py           # 网页抓取
│       ├── list_skills.py         # 列出技能
│       ├── load_skill.py          # 加载技能文档
│       └── mcp_bridge.py          # MCP 桥接
├── data/
│   └── occasion_guide.json        # 穿搭指南数据
├── .env.example                   # 环境变量模板
└── requirements.txt               # Python 依赖
```

### 2.1 `package.json` — 包元信息

```json
{
  "name": "outfit-assistant",
  "version": "3.0.0",
  "description": "穿搭助手 — dispatcher → dummy_pre_skill hook → coordinator loop",
  "author": "AgentFlow",
  "workflows": [
    "dummy_pre_skill_workflow.json",
    "coordinator_workflow.json",
    "skill_workflow.json"
  ],
  "metadata": {
    "type": "business-agent",
    "category": "lifestyle",
    "tags": ["outfit", "weather", "fashion", "style-memory"]
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | **Agent 标识符**。创建后作为 daemon 中的 agent_id。命名用连字符（如 `outfit-assistant`） |
| `version` | string | 版本号 |
| `description` | string | 简要描述 |
| `workflows` | string[] | 引用的 workflow 文件列表 |
| `metadata` | object | 标签、分类等可扩展元数据 |

### 2.2 `agent.json` — Agent 定义

这是 agent 的核心配置文件，定义了 agent 的 tasks（任务通道）：

```json
{
  "name": "OutfitAssistantAgent",
  "version": "3.0.0",
  "description": "穿搭助手 — dispatcher → dummy_pre_skill hook → coordinator loop",
  "session_pool_size": 3,
  "max_workflows": 10,
  "max_concurrent_workflows": 5,
  "data_bus_capacity_mb": 5,
  "tasks": [
    {
      "id": 0,
      "type": "dispatcher",
      "description": "Routes to dummy_pre_skill hook via next_skills",
      "next_skills": ["dummy_pre_skill"],
      "max_concurrent_skills": 5,
      "workflow_pool_size": 1
    },
    {
      "id": 1,
      "type": "coordinator",
      "description": "LLM decision loop — dispatches skills, evaluates results, outputs final response",
      "workflow_file": "coordinator_workflow.json",
      "workflow_pool_size": 1
    },
    {
      "id": 2,
      "type": "skill",
      "description": "Pre-processing hook — passthrough before coordinator",
      "workflow_file": "dummy_pre_skill_workflow.json",
      "skill": {
        "name": "dummy_pre_skill",
        "config_dir": "harness/skills/dummy_pre_skill",
        "instance_pool_size": 1
      },
      "workflow_pool_size": 1
    },
    {
      "id": 3,
      "type": "skill",
      "description": "Generic skill runner — serves ALL skill names via exec_skill_key routing",
      "workflow_file": "skill_workflow.json",
      "skill": {
        "name": "skill-runner",
        "config_dir": "harness/skills/skill-runner",
        "instance_pool_size": 5
      },
      "workflow_pool_size": 5
    }
  ]
}
```

#### task 类型说明

| type | 说明 | 必备字段 |
|------|------|----------|
| `dispatcher` | 内置调度器。会话首次进入的入口，通过 `next_skills` 把请求路由到前置 skill | `next_skills` |
| `skill` | 执行一段 workflow（hook、通用 runner 等）。多个 skill task 可共存，由 `dispatcher.next_skills` 与 coordinator 派发的 `next_skills` 触发 | `workflow_file`, `skill.name` |
| `coordinator` | LLM 决策循环核心：评估用户输入与 skill 回流结果，决定派发哪些 skill 或输出最终回复 | `workflow_file` |

#### V3 架构控制流

```
用户输入 → dispatcher(id=0) ─next_skills─▶ dummy_pre_skill(id=2, 前置 hook)
                                              │
                                              ▼
                                    coordinator(id=1) ◀─────── skill 完成回流
                                              │
                                   ┌──────────┴──────────┐
                              run_skills tool          final response
                                   │                       │
                                   ▼                       ▼
                            skill-runner(id=3)      返回客户端
                                   │
                              执行任意业务 skill
                            (outfit-stylist / forge-image-gen / …)
```

- coordinator 通过 `run_skills` 工具把 skill 派发到 `skill-runner`（id=3），由 `exec_skill_key` 路由区分不同业务 skill
- skill 完成后框架自动回流到 coordinator，coordinator 评估结果后决定再次派发或输出最终回复
- `dummy_pre_skill`（id=2）是前置 hook 占位，dispatcher 首次路由到它，它再触发 coordinator

#### 关键约束

- 必须且只能有一个 `dispatcher` 类型的 task
- 必须且只能有一个 `coordinator` 类型的 task
- skill 通过 `dispatcher.next_skills` 数组或 coordinator 派发的 `next_skills` 被引用，名称需与 skill 的 `skill.name` 一致
- 目录名为下划线（`outfit_assistant`），但 `package.json` 中的 `name` 和 daemon 的 agent_id 用连字符（`outfit-assistant`）

### 2.3 `agent.md` — 模型配置 + 业务意图 + skills 列表

使用 YAML frontmatter 格式。frontmatter 之后的正文作为业务意图描述，会被 coordinator 拼进 system prompt：

```markdown
---
name: 穿搭顾问 · Outfit Stylist
model: MiniMax-M3
description: 个性化穿搭顾问：根据真实天气、场合和个人风格给出可落地的搭配
skills:
  - name: outfit-stylist
  - name: forge-image-gen
  - name: video-gen
---

你是一个穿搭顾问。遵循已挂载的 outfit-stylist 技能：先读取 .agentfoundry/memory/outfit-profile/ 里的用户档案与历史，
再用 weather.py 查真实天气、用 outfit_vision.py 识别用户上传的图片，结合场合与个性化给出有理由的搭配...
```

| frontmatter 字段 | 说明 |
|------------------|------|
| `name` | Agent 展示名 |
| `model` | LLM 模型名（字符串，如 `MiniMax-M3`、`gpt-4o-mini`）；也可写成对象 `model: {name: ..., base_url: ...}` |
| `description` | Agent 简介 |
| `skills` | 该 agent 可派发的 skill 列表，coordinator 据此构建 `run_skills` 工具的可选项 |

> **API key 与 base_url 不写在 frontmatter 里**。它们来自环境变量：优先级 `AF_LLM_API_KEY` > `API_KEY`（key）；`AF_LLM_BASE_URL`（base_url）。模型名优先级：`AF_LLM_MODEL` 环境变量 > `agent.md` frontmatter `model`。业务环境变量统一放在包根目录的 `.env`，节点脚本会自动 `dotenv` 加载（框架不负责业务 env 注入）。

### 2.4 Workflow 文件格式

每个 workflow JSON 定义了一个工作流 DAG，包含 nodes 和 edges。当前 outfit_assistant 用 3 个 workflow：

#### coordinator 决策 workflow（`coordinator_workflow.json`）

coordinator 的 LLM 决策节点，单节点 DAG：

```json
{
  "id": "coordinator-workflow",
  "version": "3.0",
  "name": "Coordinator Loop",
  "nodes": [
    {
      "id": "coordinator_decide",
      "name": "Coordinator Decide",
      "type": "task",
      "execution_mode": "python_script",
      "command": "./nodes/coordinator_decide.py",
      "working_dir": "examples/agent/outfit_assistant",
      "json_output": true,
      "enable_hot_reload": false,
      "inputs": [
        {"name": "input",               "type": "string", "required": true},
        {"name": "session_id",          "type": "string", "required": true},
        {"name": "dialogue_id",         "type": "string", "required": false},
        {"name": "completed_skill_name", "type": "string", "required": false},
        {"name": "skill_success",       "type": "string", "required": false},
        {"name": "skill_result",        "type": "string", "required": false},
        {"name": "skill_artifacts",     "type": "string", "required": false}
      ],
      "outputs": [
        {"name": "response", "type": "string"},
        {"name": "next_skills", "type": "string"},
        {"name": "coordinator_status", "type": "string"}
      ],
      "timeout_ms": 180000
    }
  ],
  "edges": []
}
```

coordinator 节点的两种出口：
- LLM 调用 `run_skills` 工具 → 通过 DataBus 写 `next_skills`，输出 `{"coordinator_status": "skills_dispatched"}`，框架接管后续 skill 调度
- LLM 无 tool_call → 输出 `{"response": "...最终回复..."}`，框架把 `response` 写到 DataBus 返回客户端

#### 通用 skill 执行 workflow（`skill_workflow.json`）

`skill-runner` task 使用的通用执行器，所有业务 skill（outfit-stylist / forge-image-gen / …）都经它执行，靠 `exec_skill_key` 路由区分：

```json
{
  "id": "skill-workflow",
  "version": "2.0",
  "name": "Generic Skill Executor",
  "nodes": [
    {
      "id": "skill_executor",
      "name": "Universal Skill Executor",
      "type": "task",
      "execution_mode": "python_script",
      "command": "./nodes/skill_executor.py",
      "working_dir": "examples/agent/outfit_assistant",
      "json_output": true,
      "enable_hot_reload": false,
      "inputs": [
        {"name": "input",          "type": "string", "required": true},
        {"name": "session_id",     "type": "string", "required": true},
        {"name": "dialogue_id",    "type": "string", "required": false},
        {"name": "exec_skill_key", "type": "string", "required": false}
      ],
      "outputs": [
        {"name": "response", "type": "string"},
        {"name": "status",   "type": "string"},
        {"name": "artifacts_json", "type": "string"}
      ],
      "timeout_ms": 240000
    }
  ],
  "edges": []
}
```

`skill_executor` 从 DataBus 读 `exec_skill_key` 指向的 dispatch manifest（含 skill_name / prompt / load_skills），加载对应 skill 的 SKILL.md 与 scripts/ 工具，跑 LLM while-loop，完成后把结果写回 DataBus，框架回流到 coordinator。

#### 前置 hook workflow（`dummy_pre_skill_workflow.json`）

dispatcher 首次路由的占位 hook，单节点 `dummy_pre_skill.py`，输出为空，仅作为"先经过 hook 再进 coordinator"的固定编排点。

| node 字段 | 说明 |
|-----------|------|
| `id` | 节点唯一标识 |
| `execution_mode` | `shell_script` / `python_script` / `so` |
| `command` | 执行命令（相对 `working_dir`） |
| `working_dir` | 工作目录（相对于 daemon 启动目录） |
| `json_output` | `true` 表示 stdout 输出 JSON，框架自动解析 |
| `inputs` | 入参声明。框架自动注入同名 `AF_INPUT`、`AF_SESSION_ID`、`AF_DIALOGUE_ID` 等环境变量；额外入参（如 `completed_skill_name`、`exec_skill_key`）也以 `AF_*` 大写形式注入 |
| `outputs` | 出参声明。`name="response"` 的输出会被写到 DataBus 返回客户端；`next_skills` 触发框架调度 |
| `timeout_ms` | 节点超时 |

### 2.5 LLM 节点脚本格式

节点脚本（如 `coordinator_decide.py`、`skill_executor.py`）通过环境变量接收输入，通过 stdout 输出 JSON。框架把 workflow `inputs` 声明的入参以 `AF_*` 大写形式注入为环境变量。

**输入环境变量（coordinator_decide 节点示例）：**

| 变量 | 说明 |
|------|------|
| `AF_INPUT` | 用户输入文本（首次触发）或 coordinator 消息 |
| `AF_SESSION_ID` | 会话 ID |
| `AF_DIALOGUE_ID` | 对话 ID |
| `AF_DATABUS_UDS_FILE` | DataBus UDS socket 路径 |
| `AF_COMPLETED_SKILL_NAME` | 刚完成的 skill 名（skill 回流触发时） |
| `AF_SKILL_SUCCESS` | 上一个 skill 是否成功（`"true"` / `"false"`） |
| `AF_SKILL_RESULT` | 上一个 skill 的结果文本 |
| `AF_SKILL_ARTIFACTS` | 上一个 skill 产出的 artifacts（JSON 字符串） |
| `AF_LLM_API_KEY` / `API_KEY` | LLM API Key（优先级：前者 > 后者；来自包内 `.env`） |
| `AF_LLM_BASE_URL` | API 端点（来自 `.env`） |
| `AF_LLM_MODEL` | 模型名（覆盖 `agent.md` frontmatter） |

`skill_executor` 节点额外有 `AF_EXEC_SKILL_KEY`（dispatch manifest 的 DataBus key）。

业务环境变量（`API_KEY`、`AF_LLM_*`、`QWEATHER_API_KEY` 等）不由框架注入，而是节点脚本在启动时用 `dotenv` 从包根目录 `.env` 自行加载。

**输出格式（stdout JSON）：**

coordinator 决策派发 skill 时：
```json
{"coordinator_status": "skills_dispatched"}
```

coordinator 输出最终回复时：
```json
{"response": "这里是LLM返回的文本内容"}
```

skill_executor 完成时：
```json
{"response": "...", "status": "ok", "artifacts_json": "[...]"}
```

`name="response"` 的输出会被框架写到 DataBus 返回客户端。发生错误时节点同样输出纯文本错误描述（无特殊 error 封装），例如：

```json
{"response": "API调用失败：Connection timeout"}
```

---

## 3. 第一步：上传 Agent Package

### 3.1 接口定义

```
POST /api/packages/upload
Content-Type: multipart/form-data
```

**请求参数：**

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `file` | file | 是 | .zip 或 .tar.gz 格式的 agent package 压缩包 |
| `name` | string | 否 | 包名。省略时从文件名自动推断（去掉扩展名） |

**响应示例（HTTP 201）：**

```json
{
  "name": "outfit_assistant",
  "path": "/path/to/agent_packages/outfit_assistant",
  "file_count": 24,
  "last_modified": "2026-06-27T10:00:00+08:00"
}
```

### 3.2 数据组织

将 agent package 目录打包为 zip 或 tar.gz。**打包时注意不要包含外层目录**，确保包内的顶级文件就是 agent 的各个文件：

```bash
# 正确做法：cd 到目录再打包
cd examples/agent/outfit_assistant
zip -r ../outfit_assistant.zip .

# 错误做法：打包了外层目录（解压后多了 outfit_assistant/ 层级）
zip -r outfit_assistant.zip examples/agent/outfit_assistant/
```

### 3.3 示例请求

```bash
# 上传 agent package
curl -X POST http://127.0.0.1:8199/health/api/packages/upload \
  -F "file=@outfit_assistant.zip" \
  -F "name=outfit_assistant"
```

### 3.4 加载到 Daemon

上传后还需将 agent 加载到 daemon：

```bash
# 加载 agent
curl -X POST http://127.0.0.1:8199/health/api/agents/outfit-assistant/load \
  -H "Content-Type: application/json" \
  -d '{}'
```

接口说明：

```
POST /api/agents/{agent_id}/load
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `agent_id` | path | agent 标识符（`package.json` 中的 `name` 字段） |
| `body.package_path` | string (可选) | 包目录名，省略时默认用 `agent_id` 替换 `-` 为 `_` |

加载成功后返回 agent 的信息：

```json
{
  "success": true,
  "data": {
    "id": "outfit-assistant",
    "name": "OutfitAssistantAgent",
    "workflow_count": 3,
    "status": "loaded"
  },
  "error": null
}
```

---

## 4. 第二步：触发请求（对话）

### 4.1 REST 接口

```
POST /api/chat/{agent_id}
Content-Type: application/json

{
  "user_id": "user_demo",
  "client_id": "client_demo",
  "content": "我叫张三，今天要去上海出差，帮我推荐穿搭"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `agent_id` | path | agent 标识符（如 `outfit-assistant`） |
| `user_id` | string | 用户标识（用于会话路由） |
| `client_id` | string | 客户端标识 |
| `content` | string | 用户输入文本 |

**响应：**

```json
{
  "success": true,
  "data": {
    "task_id": "0_task_2_runtime_skill-...",
    "content": "根据您的出差需求，建议穿着商务休闲装，上海今天气温..."
  },
  "error": null
}
```

| 响应字段 | 说明 |
|---------|------|
| `success` | `true` 表示请求成功 |
| `data.task_id` | 本次任务的唯一标识 |
| `data.content` | Agent 的完整回复；可能包含图片或 HTML 的 Markdown URL |
| `error` | 成功时为 `null` |

### 4.2 使用 curl 测试

```bash
curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id": "user_demo", "client_id": "client_demo", "content": "我今天要去上海出差，推荐穿搭"}'
```

示例响应：

```json
{
  "success": true,
  "data": {
    "task_id": "0_task_3_runtime_skill-skill-runner-msg_1_392234567_skill-runner",
    "content": "上海今天预计25°C，建议穿着轻薄西装外套搭配休闲裤，适合商务出差场合。"
  },
  "error": null
}
```

### 4.3 获取图片和 HTML 产物

当任务生成图片或 HTML 时，`data.content` 仍是字符串，产物以 Markdown URL 返回，不会以 Base64 或独立二进制字段返回。例如：

```json
{
  "success": true,
  "data": {
    "task_id": "task_abc123",
    "content": "穿搭方案已生成：\n\n![穿搭效果图](https://agent.example.com/health/api/artifacts/outfit-assistant/files/outfit_assistant/outputs/task-abc123/look.png)\n\n[查看完整 HTML 报告](https://agent.example.com/health/api/artifacts/outfit-assistant/files/outfit_assistant/outputs/task-abc123/report.html)"
  },
  "error": null
}
```

外部服务的处理步骤：

1. 读取 `data.content`，按 Markdown 渲染，或自行提取其中的 URL。
2. 使用普通 HTTP `GET` 请求访问 `/api/artifacts/{agent_id}/files/{relative_path}`。
3. 按响应的 `Content-Type` 处理图片或 HTML；不要根据文件扩展名猜测图片编码。
4. HTML 报告引用的图片、CSS、JavaScript 等相对资源保持原目录层级，浏览器直接打开 HTML URL 即可继续加载这些资源。

直接下载示例：

```bash
curl -L \
  "https://agent.example.com/health/api/artifacts/outfit-assistant/files/outfit_assistant/outputs/task-abc123/look.png" \
  -o look.png
```

Artifact 文件接口可能返回：

| HTTP 状态码 | 含义 |
|-------------|------|
| 200 | 文件存在，返回文件流 |
| 403 | 路径越过 Agent Package 根目录 |
| 404 | 文件不存在 |
| 415 | 文件类型不支持 |

### 4.4 外部服务接入检查

对接前应确认：

- Bridge 公网地址可从外部服务所在网络访问；
- `ARTIFACT_PUBLIC_BASE_URL` 配置为同一公网入口，且使用 `http://` 或 `https://` 完整绝对地址；
- 如 Bridge 位于反向代理子路径下，配置值包含该路径前缀；
- 网关认证策略同时覆盖聊天接口和 Artifact 文件接口；
- 外部服务不要保存或拼接服务器本地文件路径，只使用聊天响应中返回的 URL。

当前 Artifact URL 没有签名和过期时间，`agent_id` 也不是权限校验。公网部署必须由反向代理或 API Gateway 提供认证和访问控制；产物清理周期需由部署方另行管理。

---

## 5. 第三步：收集 Trace

### 5.1 Trace 数据说明

LLM 调用轨迹（trace）由节点脚本（`coordinator_decide.py` / `skill_executor.py`）通过 `llm_call.py` 的 `_TraceRecorder` 写入 DataBus `llm/` 命名空间。每次 LLM 调用产生 3 条记录 + 1 条索引：

| DataBus key | 内容 |
|-------------|------|
| `llm/traces/{call_id}/request` | 完整 HTTP 请求体（messages + tools + model） |
| `llm/traces/{call_id}/response` | 完整 HTTP 响应体 |
| `llm/traces/{call_id}/meta` | 调用元数据 + **任务上下文**（见下表） |
| `llm/traces/index` | call_id 有序列表，供枚举 |

**meta 字段**：

| 字段 | 来源 | 说明 |
|------|------|------|
| `model` / `latency_ms` / `timestamp` | LLM 调用 | 模型名、延迟、时间戳 |
| `prompt_tokens` / `completion_tokens` / `total_tokens` | LLM 响应 | token 用量 |
| `finish_reason` / `msg_count` / `tools_enabled` | LLM 响应 | 结束原因、消息数、是否带工具 |
| `request_summary` / `response_summary` | 节点侧摘要 | 列表视图用的简短摘要 |
| `dialogue_id` | `AF_DIALOGUE_ID` | 当前对话 ID，用于把一次对话的所有 LLM 调用聚成一组 |
| `session_id` | `AF_SESSION_ID` | agent session ID |
| `node_id` | `AF_NODE_ID` | 节点 ID（`coordinator_decide` / `skill_executor`），**可靠调用方标识** |
| `task_id` | `AF_SELF_TASK_ID` | task ID（当前框架未注入此环境变量，恒为空串） |
| `skill_name` | `AF_SKILL_NAME` | skill 名（当前框架未注入此环境变量，恒为空串） |
| `completed_skill` | `AF_COMPLETED_SKILL_NAME` | 刚完成的 skill 名；非空 ⇒ 控制权回到 coordinator |
| `task_type` | 推断 | `"skill"` / `"coordinator"` / `""`（由 `node_id` + `completed_skill` 推断） |

**控制流还原判据**（下游分析用）：

- `node_id` 含 `skill_executor` → subagent 正在执行
- `completed_skill` 非空 → coordinator 重新拿到控制权（skill 完成回流）
- 同一 `dialogue_id` 下按时序排列 → 完整的 coordinator → skill → coordinator 调用链

### 5.2 查询 Trace 列表

```
GET /api/agents/{agent_id}/llm-traces?user_id={user_id}&client_id={client_id}&limit=50&offset=0
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `agent_id` | path | 是 | agent 标识符 |
| `user_id` | query | 是 | 用户 ID |
| `client_id` | query | 是 | 客户端 ID |
| `dialogue_id` | query | 否 | 按对话 ID 过滤 |
| `limit` | query | 否 | 返回条数（默认 50，最大 200） |
| `offset` | query | 否 | 分页偏移（默认 0） |

**响应：**

```json
{
  "success": true,
  "data": {
    "traces": [
      {
        "call_id": "a1b2c3d4e5f6",
        "model": "gpt-4o-mini",
        "latency_ms": 3200,
        "request_summary": "system: ... | user: 我今天要去上海出差",
        "response_summary": "finish=tool_calls | tool_calls: ['run_skills']",
        "timestamp": "2026-07-02T04:07:06Z",
        "token_usage": {
          "prompt_tokens": 1092,
          "completion_tokens": 269,
          "total_tokens": 1361
        },
        "dialogue_id": "10427195618320926604",
        "session_id": "0",
        "node_id": "coordinator_decide",
        "task_id": "",
        "skill_name": "",
        "completed_skill": "dummy_pre_skill",
        "task_type": "coordinator"
      },
      {
        "call_id": "d4e5f6a7b8c9",
        "model": "gpt-4o-mini",
        "latency_ms": 5487,
        "request_summary": "system: ## Skill: outfit-stylist ...",
        "response_summary": "finish=tool_calls | tool_calls: ['bash']",
        "timestamp": "2026-07-02T04:07:11Z",
        "token_usage": {
          "prompt_tokens": 1924,
          "completion_tokens": 121,
          "total_tokens": 2045
        },
        "dialogue_id": "10427195618320926604",
        "session_id": "0",
        "node_id": "skill_executor",
        "task_id": "",
        "skill_name": "",
        "completed_skill": "",
        "task_type": "skill"
      }
    ],
    "total": 2
  },
  "error": null
}
```

> 上例展示了一次完整控制流的两条 LLM 调用：第 1 条 `task_type=coordinator` + `completed_skill=dummy_pre_skill`（coordinator 首次决策），第 2 条 `task_type=skill` + `node_id=skill_executor`（subagent 执行）。两条共享同一 `dialogue_id`，下游可据此还原 coordinator → skill 调用链。

### 5.3 查询 Trace 详情

```
GET /api/agents/{agent_id}/llm-traces/{call_id}?user_id={user_id}&client_id={client_id}
```

**响应：**

```json
{
  "success": true,
  "data": {
    "call_id": "a1b2c3d4e5f6",
    "request": {
      "model": "gpt-4o-mini",
      "messages": [
        {"role": "system", "content": "..."},
        {"role": "user", "content": "我今天要去上海出差"}
      ],
      "tools": [...]
    },
    "response": {
      "id": "chatcmpl-xxx",
      "choices": [{"message": {"content": "..."}}],
      "usage": {"prompt_tokens": 850, "completion_tokens": 320}
    },
    "meta": {
      "model": "gpt-4o-mini",
      "latency_ms": 3200,
      "request_summary": "...",
      "response_summary": "...",
      "timestamp": "2026-07-02T04:07:06Z",
      "prompt_tokens": 1092,
      "completion_tokens": 269,
      "total_tokens": 1361,
      "finish_reason": "tool_calls",
      "msg_count": 2,
      "tools_enabled": true,
      "dialogue_id": "10427195618320926604",
      "session_id": "0",
      "node_id": "coordinator_decide",
      "task_id": "",
      "skill_name": "",
      "completed_skill": "dummy_pre_skill",
      "task_type": "coordinator"
    }
  },
  "error": null
}
```

### 5.4 示例请求

```bash
# 查询 trace 列表
curl "http://127.0.0.1:8199/health/api/agents/outfit-assistant/llm-traces?user_id=user_demo&client_id=client_demo&limit=5"

# 查询 trace 详情
curl "http://127.0.0.1:8199/health/api/agents/outfit-assistant/llm-traces/a1b2c3d4e5f6?user_id=user_demo&client_id=client_demo"
```

---

## 6. 第四步：更新技能（更新 Agent Package）

### 6.1 接口定义

```
PUT /api/packages/{name}/update
Content-Type: multipart/form-data
```

**请求参数：**

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | path | 是 | 包名（目录名） |
| `file` | file | 是 | 新的 .zip 或 .tar.gz 压缩包 |
| `mode` | string | 否 | 更新模式：`incremental`（增量，默认）/ `full`（全量覆盖） |

**`incremental` 模式：** 在已有文件上覆盖/新增，**不删除**未在压缩包中的文件。

**`full` 模式：** 先清空目录（保留 `dist/` 子目录），再解压。**注意：会删除旧文件。**

### 6.2 自动热更新

更新成功后，如果该 agent 已加载到 daemon，bridge 会自动执行 **unload → load** 实现热更新：

1. 解压新文件到包目录
2. 保持原有目录结构
3. 如果 agent 已加载 → 调用 `agent.unload` → 调用 `agent.load` 加载新版本
4. 如果 agent 未加载 → 静默跳过，后续手动加载

### 6.3 示例请求

```bash
# 更新 agent package（增量模式）
curl -X PUT http://127.0.0.1:8199/health/api/packages/outfit_assistant/update \
  -F "file=@outfit_assistant_v2.zip" \
  -F "mode=incremental"

# 更新 agent package（全量模式）
curl -X PUT http://127.0.0.1:8199/health/api/packages/outfit_assistant/update \
  -F "file=@outfit_assistant_v2.zip" \
  -F "mode=full"
```

**响应：**

```json
{
  "name": "outfit_assistant",
  "path": "/path/to/agent_packages/outfit_assistant",
  "file_count": 26,
  "last_modified": "2026-06-27T11:00:00+08:00"
}
```

### 6.4 典型更新场景

| 场景 | 需要更新的文件 | 说明 |
|------|---------------|------|
| 修改业务意图/系统提示词 | `agent.md`（frontmatter 之后的正文） | 改变 coordinator 的 LLM 行为 |
| 切换模型 | `agent.md`（frontmatter `model`）或 `.env` 的 `AF_LLM_MODEL` | 更换 LLM 通道 |
| 修改 API key / base_url | `.env`（`API_KEY` / `AF_LLM_BASE_URL`） | 改 LLM 凭证 |
| 添加新工具 | `harness/tools/my_tool.py` + 更新 `__init__.py` | 扩展全局工具能力 |
| 添加新 skill | `harness/skills/<name>/SKILL.md` + `scripts/` | 扩展可派发 skill；同时在 `agent.md` 的 `skills` 列表登记 |
| 修改 coordinator 决策逻辑 | `nodes/coordinator_decide.py` | 改变派发/评估流程 |
| 修改 skill 执行逻辑 | `nodes/skill_executor.py` | 改变通用 skill runner 行为 |
| 修改工作流配置 | `*.json` workflow 文件 | 调整 DAG 拓扑 |

---

## 7. 第五步：再次触发

更新成功后，重复第二步的流程即可使用更新后的 agent：

```bash
curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id": "user_demo", "client_id": "client_demo", "content": "我今天要去上海出差，推荐穿搭"}'
```

更新后再次触发，可对比两次 trace 数据来验证变更效果。

---

## 8. 附录：完整接口定义

### 8.1 Package 管理

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/packages` | 列出所有 package |
| `GET` | `/api/packages/{name}` | 获取 package 详情 |
| `POST` | `/api/packages/create` | 创建空 package（body: `{"name": "..."}`） |
| `POST` | `/api/packages/upload` | 上传并解压 package |
| `PUT` | `/api/packages/{name}/update` | 更新已存在的 package |
| `GET` | `/api/packages/{name}/package` | 获取已构建的 tarball 元信息 |
| `GET` | `/api/packages/{name}/tree` | 查看 package 文件树 |
| `GET` | `/api/packages/{name}/file/{path}` | 读取文件内容 |
| `PUT` | `/api/packages/{name}/file/{path}` | 写入文件内容（创建或覆盖） |
| `POST` | `/api/packages/{name}/file/{path}` | 创建空文件（已存在则 409） |
| `POST` | `/api/packages/{name}/file/{path}/rename` | 重命名文件（body: `{"new_name": "..."}`） |
| `DELETE` | `/api/packages/{name}/file/{path}` | 删除文件 |
| `POST` | `/api/packages/{name}/dir/{path}` | 创建目录 |
| `DELETE` | `/api/packages/{name}/dir/{path}` | 删除空目录 |
| `GET` | `/api/packages/{name}/package/download` | 下载 package 压缩包 |
| `POST` | `/api/packages/{name}/package` | 构建 package（生成 tarball） |

### 8.2 Agent 管理

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/agents` | 列出所有已加载 agent |
| `GET` | `/api/agents/{agent_id}` | 获取 agent 详情 |
| `POST` | `/api/agents/{agent_id}/load` | 加载 agent 到 daemon |
| `POST` | `/api/agents/{agent_id}/unload` | 从 daemon 卸载 agent |
| `POST` | `/api/agents/{agent_id}/pause` | 暂停 agent |
| `POST` | `/api/agents/{agent_id}/resume` | 恢复 agent |

### 8.3 Chat

| 方法 | 路径 | 说明 |
|------|------|------|
| `POST` | `/api/chat/{agent_id}` | 对话请求 |

### 8.4 Artifact 文件

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/artifacts/{agent_id}/files/{relative_path}` | 获取图片、HTML 及其相对资源（推荐） |
| `GET` | `/api/artifacts/{agent_id}?path={path}` | 旧版图片兼容接口，不建议新接入使用 |

### 8.5 LLM Trace

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/agents/{agent_id}/llm-traces` | 查询 trace 列表 |
| `GET` | `/api/agents/{agent_id}/llm-traces/{call_id}` | 查询 trace 详情 |

### 8.6 Session 管理

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/sessions?agent_id=...&user_id=...` | 列出某 agent 的会话（`user_id` 可选过滤） |
| `DELETE` | `/api/sessions/{session_id}?agent_id=...` | 销毁指定会话 |
| `DELETE` | `/api/sessions/{session_id}/dialogue?agent_id=...` | 清空会话对话历史（保留会话） |

### 8.7 Daemon 监控

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/daemon/status` | daemon 状态快照（缓存刷新，约 10s） |
| `GET` | `/api/daemon/uptime` | daemon 运行时长 + 是否在运行 |

### 8.8 Skill Hub（技能中心）

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/skill-hub` | 列出所有 skill 元信息 |
| `GET` | `/api/skill-hub/{name}` | 获取 skill 详情（含 SKILL.md 全文） |
| `POST` | `/api/skill-hub/upload` | 上传 SKILL.md 或压缩包到 skill hub |
| `DELETE` | `/api/skill-hub/{name}` | 从 hub 删除 skill |
| `POST` | `/api/skill-hub/{name}/install` | 把 skill 安装到指定 agent package |

### 8.9 返回格式规范

**成功响应：**

```json
{
  "success": true,
  "data": { ... },
  "error": null
}
```

**错误响应：**

```json
{
  "success": false,
  "data": null,
  "error": {
    "code": -32001,
    "message": "错误描述"
  }
}
```

HTTP 状态码映射：

| 场景 | HTTP 状态码 |
|------|------------|
| 成功 | 200 |
| 创建成功 | 201 |
| 参数错误 | 400 |
| 已存在 | 409 |
| 未找到 | 404 |
| 服务不可用 | 502 |
| 超时 | 504 |

---

## 完整演示流程（一行一条）

```bash
# 0. 确保 daemon 和 bridge 已启动

# 1. 上传 agent package
curl -X POST http://127.0.0.1:8199/health/api/packages/upload \
  -F "file=@outfit_assistant.zip" -F "name=outfit_assistant"

# 2. 加载到 daemon
curl -X POST http://127.0.0.1:8199/health/api/agents/outfit-assistant/load \
  -H "Content-Type: application/json" -d '{}'

# 3. 触发对话
curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"今天上海的天气适合穿什么？"}'

# 4. 查询 trace
curl "http://127.0.0.1:8199/health/api/agents/outfit-assistant/llm-traces?user_id=demo&client_id=demo"

# 5. 更新 agent package（修改 agent.md 后重新打包）
curl -X PUT http://127.0.0.1:8199/health/api/packages/outfit_assistant/update \
  -F "file=@outfit_assistant_v2.zip" -F "mode=incremental"

# 6. 再次触发
curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"今天上海的天气适合穿什么？"}'
```
