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
3.5 [对接模式与响应数据格式](#35-对接模式与响应数据格式)
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

### 1.2 认证方式

> ⚠️ **当前版本无内置认证**：Bridge 本身不校验请求来源。本文所有示例均可直接访问。
>
> 生产部署时必须在反向代理层（Nginx / API Gateway）添加认证。当前文档的认证方案尚待设计——需要定制的对接方可联系部署方单独配置访问控制。

### 1.3 路由前缀说明

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
# configs/{env}/bridge.yaml → artifact_public_base_url
# 或环境变量 ARTIFACT_PUBLIC_BASE_URL
ARTIFACT_PUBLIC_BASE_URL=https://agent.example.com
```

该地址必须能被对接方网络实际访问。配置项只负责生成绝对 URL，不会自动配置 DNS、TLS、防火墙或反向代理。

### 1.4 本地前端连接远程 Bridge

开发场景中，前端（Vite 开发服务器，默认 `http://localhost:5173`）需要访问部署在远程服务器上的 Bridge。需要以下配置：

#### 前端侧：指定 Bridge 地址

`ChatPage.vue` 中 `useChatSse(baseUrl?)` 默认连接到同源地址（`window.location.origin` 或 `http://localhost:8000`）。

改为传入远程 Bridge URL：

```typescript
// ChatPage.vue 第 34 行
const BRIDGE_URL = 'https://your-bridge-server:8199/health'  // 部署方提供的 Bridge 公网地址
const sse = useChatSse(BRIDGE_URL)
```

`useChatSse` 内部用此 `baseUrl` 构造所有请求路径（SSE、REST API、会话列表、历史消息等）。

如果使用 WebSocket 通道：

```typescript
// AssistantWidget.vue 或 ChatPage.vue
const wsUrl = 'wss://your-bridge-server:8199'
const ws = useChatWebSocket({ baseUrl: wsUrl })
```

#### Bridge 侧：配置 CORS

Bridge 的 CORS 白名单默认只允许 `localhost:15173` 和 `127.0.0.1:15173`。前端跑在不同源时会被浏览器拦截。

在 Bridge 服务器上设置环境变量：

```bash
# 方式 A：指定前端源（推荐）
export CORS_ALLOWED_ORIGINS='["http://localhost:5173","https://your-frontend.com"]'

# 方式 B：允许所有源（仅开发环境）
export CORS_ALLOWED_ORIGINS='["*"]'
```

或在 `frontend/bridge/config.yaml` 中写入：

```yaml
cors_allowed_origins:
  - "http://localhost:5173"
  - "https://your-frontend.com"
```

#### 方式二：Vite proxy（无需改前端代码）

在 `frontend/web/vite.config.ts` 中添加 proxy 规则，将 `/api` 和 `/ws` 代理到远程 Bridge，前端代码完全不用改：

```typescript
export default defineConfig({
  server: {
    proxy: {
      '/api': {
        target: 'https://your-bridge-server:8199',
        changeOrigin: true,
      },
      '/ws': {
        target: 'wss://your-bridge-server:8199',
        ws: true,
      },
    },
  },
})
```

这种方式下前期开发最便捷——不需要修改任何业务代码，Bridge 也不需要额外配置 CORS（浏览器认为请求来自同源）。

#### 注意事项

| 项 | 说明 |
|---|------|
| **路由前缀** | 如果 Bridge 挂载在 `/health` 前缀下（如 `https://host:8199/health/api/...`），`baseUrl` 必须包含该前缀 |
| **WebSocket** | WSS 需要 Bridge 服务器配置 TLS 证书（可通过反向代理如 Nginx 完成） |
| **SSE 推荐** | SSE 走 HTTP 协议，比 WebSocket 更容易通过反向代理部署，且自带断线重连语义 |
| **产物 URL** | 确保 `ARTIFACT_PUBLIC_BASE_URL` 配置为前端可访问的公网地址，否则图片/HTML 无法加载 |

启动顺序：

```bash
# 1. 启动 daemon（使用 outfit-assistant 自带的 Python venv）
export PATH="$(pwd)/examples/agent/outfit-assistant/.venv_unix/bin:$PATH"
./build/agentflowd --package "$(pwd)/examples/agent/outfit-assistant" \
  --listen 127.0.0.1:17891 --log-file "$(pwd)/logs/agentflowd.log" &

# 2. 启动 bridge
cd frontend/bridge && python3 main.py &
```

> 包内 venv 目录名为 `.venv_unix`（跨平台脚本 `setup.sh` 创建）。如果你的环境用其他 venv，替换 `PATH` 即可。

---

## 2. Agent Package 格式说明

一个 agent package 是磁盘上的一个目录，包含 agent 的完整定义。以 `outfit-assistant` 为例：

```
outfit-assistant/
├── package.json                   # 包元信息
├── agent.json                     # agent 定义（4 个 task）
├── agent.md                       # 模型配置 + 业务意图 + skills 列表
├── coordinator_workflow.json      # coordinator 决策 workflow（coordinator_decide 节点）
├── dummy_pre_skill_workflow.json  # 前置 hook workflow（dummy_pre_skill task）
├── skill_workflow.json            # 通用 skill 执行 workflow（skill_executor 节点）
├── setup.sh                       # 环境初始化脚本（创建 .venv、装依赖、写 sentinel）
├── nodes/
│   ├── coordinator_decide.sh      # wrapper：load .env → 包内 .venv / fallback 系统 python3
│   ├── coordinator_decide.py      # coordinator LLM 决策核心（run_skills 工具）
│   ├── dummy_pre_skill.sh         # wrapper
│   ├── dummy_pre_skill.py         # 前置 hook 占位节点
│   ├── skill_executor.sh          # wrapper
│   ├── skill_executor.py          # 通用 skill 执行器 + 双层 tool 加载
│   └── common/                    # 节点共享内部模块（DataBus 客户端、LLM 调用、trace 记录等由 agentflow-runtime 提供）
├── harness/
│   ├── skills/
│   │   ├── dummy_pre_skill/SKILL.md       # hook 占位 skill
│   │   ├── skill-runner/SKILL.md          # 通用 skill-runner 占位
│   │   └── minimax_outfit_skills/         # 业务 skills 子目录
│   │       ├── outfit-stylist/            # 综合穿搭 skill（含 weather.py / outfit_vision.py 等 scripts/）
│   │       ├── forge-image-gen/           # 图片生成 skill（含 generate.py）
│   │       └── video-gen/                 # 视频生成 skill（含 make_video.py）
│   └── tools/
│       └── __init__.py            # 工具自动发现入口（具体工具由 agentflow-runtime 提供）
├── assets/                        # 静态素材（示例图等）
├── outputs/                       # 业务产物输出目录（按 task/UUID 分桶）
├── .env.example                   # 环境变量模板
├── .env_minimax / .env_dashqueue  # 模型专用 .env 变体
└── requirements.txt               # Python 依赖
```

> 说明：早期版本中的 `nodes/databus_client.py`、`nodes/llm_call.py` 已收敛到 `agentflow-runtime` Python 包，节点脚本通过 `import agentflow_runtime` 引用；harness/tools 下的具体工具（`bash.py`、`read_file.py`、`web_fetch.py` 等）同样由 `agentflow-runtime` 提供，包内只保留 `__init__.py` 自动发现入口。业务 skills 实际下挂在 `harness/skills/minimax_outfit_skills/` 子目录而非直接挂在 `harness/skills/` 下。

### 2.1 `package.json` — 包元信息

```json
{
  "name": "outfit-assistant",
  "version": "3.0.0",
  "description": "穿搭助手 — dispatcher → dummy_pre_skill hook → coordinator loop",
  "author": "AgentFlow",
  "workflows": [
    "coordinator_workflow.json",
    "dummy_pre_skill_workflow.json",
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
  "version": "3.2.0",
  "description": "穿搭助手 — dispatcher → dummy_pre_skill hook → coordinator loop",
  "session_pool_size": 3,
  "max_workflows": 10,
  "max_concurrent_workflows": 5,
  "data_bus_capacity_mb": 500,
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
- 目录名为下划线（`outfit-assistant`），但 `package.json` 中的 `name` 和 daemon 的 agent_id 用连字符（`outfit-assistant`）

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

每个 workflow JSON 定义了一个工作流 DAG，包含 nodes 和 edges。当前 outfit-assistant 用 3 个 workflow：

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
      "execution_mode": "shell_script",
      "command": "bash nodes/coordinator_decide.sh",
      "working_dir": "examples/agent/outfit-assistant",
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
      "execution_mode": "shell_script",
      "command": "bash nodes/skill_executor.sh",
      "working_dir": "examples/agent/outfit-assistant",
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

dispatcher 首次路由的占位 hook，单节点 `dummy_pre_skill.sh`（wrapper 转 `dummy_pre_skill.py`），输出为空，仅作为"先经过 hook 再进 coordinator"的固定编排点。

| node 字段 | 说明 |
|-----------|------|
| `id` | 节点唯一标识 |
| `execution_mode` | `shell_script` / `python_script` / `so` |
| `command` | 执行命令（相对 `working_dir`）。当前 outfit-assistant 三个 workflow 均用 `shell_script` + `bash nodes/xxx.sh`，由 .sh wrapper 加载 `.env` 后转调同名 .py 节点脚本 |
| `working_dir` | 工作目录（相对于 daemon 启动目录） |
| `json_output` | `true` 表示 stdout 输出 JSON，框架自动解析 |
| `inputs` | 入参声明。框架自动注入同名 `AF_INPUT`、`AF_SESSION_ID`、`AF_DIALOGUE_ID` 等环境变量；额外入参（如 `completed_skill_name`、`exec_skill_key`）也以 `AF_*` 大写形式注入 |
| `outputs` | 出参声明。`name="response"` 的输出会被写到 DataBus 返回客户端；`next_skills` 触发框架调度 |
| `timeout_ms` | 节点超时 |

### 2.5 LLM 节点脚本格式

节点脚本（如 `coordinator_decide.py`、`skill_executor.py`）通过环境变量接收输入，通过 stdout 输出 JSON。框架把 workflow `inputs` 声明的入参以 `AF_*` 大写形式注入为环境变量。

> **wrapper 层**：当前所有节点都走 `bash nodes/xxx.sh` wrapper 而非直接调 `.py`。wrapper 优先用包内 `.venv`（由 `setup.sh` 装好后写 `.agentflow_installed` sentinel），不可用则 fallback 到系统 `python3`（依赖 bridge 启动时预装 `agentflow-runtime`）。wrapper 启动前会 `source` 包根 `.env`，使 `API_KEY` 等业务环境变量在 Python 启动前就就位。节点脚本本身的输入/输出协议不变。

**输入环境变量（coordinator_decide 节点示例）：**

| 变量 | 说明 |
|------|------|
| `AF_INPUT` | JSON 字符串 `{"stream": bool, "messages": [{role, content}, ...]}`。首次触发时 Bridge 组装历史上下文 + 本次输入；skill 回流时框架注入 coordinator 消息 |
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
  "name": "outfit-assistant",
  "path": "/path/to/agent_packages/outfit-assistant",
  "file_count": 24,
  "last_modified": "2026-06-27T10:00:00+08:00"
}
```

### 3.2 数据组织

将 agent package 目录打包为 zip 或 tar.gz。**打包时注意不要包含外层目录**，确保包内的顶级文件就是 agent 的各个文件：

```bash
# 正确做法：cd 到目录再打包
cd examples/agent/outfit-assistant
zip -r ../outfit-assistant.zip .

# 错误做法：打包了外层目录（解压后多了 outfit-assistant/ 层级）
zip -r outfit-assistant.zip examples/agent/outfit-assistant/
```

### 3.3 示例请求

```bash
# 上传 agent package
curl -X POST http://127.0.0.1:8199/health/api/packages/upload \
  -F "file=@outfit-assistant.zip" \
  -F "name=outfit-assistant"
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

## 3.5 对接模式与响应数据格式

Bridge 支持两种对接模式，**响应数据格式完全不同**，对接方必须按自己所在模式解析：

| 模式 | 入口 | 触发方 | 响应方式 | 响应格式 |
|------|------|--------|---------|---------|
| **A. 直连 Bridge** | `POST /api/chat/{agent_id}` / `WS /ws/chat/{agent_id}` | 对接方主动 HTTP/WS 调用 | 同步 JSON 或流式 chunk（OpenAI-compatible） | Bridge 原生 `ApiResponse` 信封 |
| **B. SystemAgent 路由** | `POST /aisf/healthagent/v1/tasks` | 上游 SysAgent 调度 Bridge | 异步回调（POST 到上游 `SysAgentEntity`） | AISF AccessCtrl 报文（PascalCase） |

### 3.5.A 模式 A：直连 Bridge

对接方直接调用 `/api/chat/{agent_id}`（REST 同步/SSE）或 `/ws/chat/{agent_id}`（WebSocket），**响应由 Bridge 在同一连接返回**。

#### A.1 同步 REST（`?stream=false` 或省略）

完整回复包在标准 `ApiResponse` 信封里一次性返回：

```json
{
  "success": true,
  "data": {
    "task_id": "0_task_2_runtime_skill-...",
    "content": "根据您的出差需求，建议穿着商务休闲装，上海今天气温...",
    "artifacts": [
      {"type": "image", "url": "/api/artifacts/outfit-assistant/files/.../look.png",
       "name": "look.png", "mime_type": "image/png"}
    ]
  },
  "error": null
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `success` | bool | `true` 表示请求成功 |
| `data.task_id` | string | 本次任务的唯一标识 |
| `data.content` | string | Agent 的完整回复；可能含图片/HTML 的 Markdown URL |
| `data.artifacts` | ArtifactInfo[] | 结构化产物元数据（仅 final 才有，可能为空数组） |
| `error` | object \| null | 失败时 `{code, message}`，成功时 `null` |

#### A.2 SSE 流式（`?stream=true`）

响应为 `text/event-stream`，每行一个 `data:` 事件，**OpenAI Chat Completions SSE 兼容格式**：

```
data: {"choices":[{"delta":{"content":"正在执行 outfit-stylist…"},"index":0}],"_dialogue_id":"0704123456-abc123def","_progress":true,"_chunk_type":"progress"}

data: {"choices":[{"delta":{"content":"好的，已为您生成方案。"},"index":0}],"_chunk_type":"llm_chunk"}

data: {"choices":[{"delta":{"content":"\n\n## 生成产物"},"index":0}],"_chunk_type":"llm_chunk"}

data: {"choices":[{"message":{"content":"\n\n[📥 下载文件](/api/artifacts/.../look.png)"}}],"_artifacts":[{"type":"image","url":"/api/artifacts/.../look.png","name":"look.png","mime_type":"image/png"}]}

data: [DONE]
```

| 字段 | 出现时机 | 说明 |
|------|---------|------|
| `choices[].delta.content` | llm_chunk / progress chunk | 流式 token（逐段推送） |
| `choices[].message.content` | text chunk（final） | 完整内容（同步响应格式） |
| `choices[].index` | 每个 chunk | 恒为 0（单 choice） |
| `_dialogue_id` | **首个 chunk** | 对话 ID，用于后续 trace 过滤 |
| `_chunk_type` | 每个 chunk | `text` / `llm_chunk` / `progress`，前端据此分流 |
| `_progress` | progress chunk | `true` 表示系统进度消息（非模型输出） |
| `_source` | 部分 chunk | `coordinator` / `skill_executor`，消息来源 |
| `_artifacts` | **final chunk** | 结构化产物元数据数组 |
| `error` | 错误时 | `{"message":"...","code":-32000}` |
| `[DONE]` | 对话完成 | 终止信号，无 data body |

#### A.3 WebSocket（`/ws/chat/{agent_id}`）

逐帧推送，**Bridge 自定义 ChatChunk 帧**（非 OpenAI 格式）：

```json
{"type":"chunk","content":"正在执行 outfit-stylist…","final":false,"progress":true,"chunk_type":"progress","_source":"coordinator"}
{"type":"chunk","content":"好的，已为您生成方案。","final":false,"progress":false,"chunk_type":"llm_chunk"}
{"type":"chunk","content":"\n\n[📥 下载文件](...)","final":true,"progress":false,"artifacts":[{"type":"image","url":"...","name":"look.png"}]}
{"type":"done","final":true}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | `"chunk"` / `"done"` / `"error"` / `"ping"` |
| `content` | string | 文本片段 |
| `final` | bool | `true` 时为本轮最后一条 chunk |
| `progress` | bool | `true` 时为系统进度消息（非模型输出） |
| `chunk_type` | string | `text` / `llm_chunk` / `progress` |
| `artifacts` | ArtifactInfo[] | 仅 final chunk 携带 |
| `_source` / `_skill` / `_round` / `_status` | string | 技能执行元数据（部分 chunk） |

> **WS vs SSE 格式差异**：WebSocket 帧用 `type:"chunk"` + 顶层字段；SSE 用 OpenAI `choices[].delta.content` + `_` 前缀扩展字段。两者内容来源相同，只是序列化信封不同。

### 3.5.B 模式 B：SystemAgent 路由（异步回调）

上游 SysAgent 调度 Bridge 的 `POST /aisf/healthagent/v1/tasks` 端点（AISF 协议）。Bridge **立即返回 202 accepted**，后台执行 agent，完成后**主动 POST 回上游** `http://{SysAgentEntity}:23767/coremind/systemagent/v1/callback`。

> **本模式不走 `/api/chat/*` 路由**。请求入口和回调格式都是 AISF AccessCtrl 协议（PascalCase），与模式 A 的 OpenAI-compatible 格式完全不同。

#### B.1 入站请求（SysAgent → Bridge）

`POST /aisf/healthagent/v1/tasks`，body 为 AccessCtrl 报文：

```json
{
  "Header": {
    "MessageType": "TASKASSIGNMENT",
    "Sender": "SysAgent",
    "Receiver": "HealthAgent",
    "UserName": "zhangsan",
    "DeviceId": "device-123",
    "TaskID": "task-abc-001",
    "SysAgentEntity": "10.0.0.5:23767",
    "ClientSessionId": "sess-xyz",
    "EventTimeStamp": "2026-07-28T10:00:00Z"
  },
  "Data": {
    "UserInputText": "帮我推荐穿搭",
    "UserInputObject": {"data": {"content": [{"type": "image", "data": "<base64>"}]}}
  }
}
```

Bridge 立即返回（不等待 agent 执行）：

```json
{"status": "accepted", "task_id": "task-abc-001", "mode": "async"}
```

#### B.2 出站回调（Bridge → 上游 SysAgent）

agent 执行完成后，Bridge 主动 **POST 两份报文**到上游 `SysAgentEntity`：

**① UserOutput（最终文本，可能多次推送）**

```json
{
  "Header": {
    "MessageType": "DataExchange",
    "Sender": "HealthAgent",
    "Receiver": "SysAgent",
    "UserName": "zhangsan",
    "DeviceId": "device-123",
    "TaskID": "task-abc-001",
    "EventTimeStamp": "2026-07-28T10:00:05Z"
  },
  "Data": {
    "MessageType": "UserOutput",
    "UserOutputText": "上海今天25°C，建议轻薄西装外套搭配休闲裤...",
    "UserOutputObject": "",
    "StreamStatus": "Completed",
    "IsVoiceBroadcast": 0,
    "UserOutputFlag": 0
  },
  "Payload": {"Function": "", "Action": ""}
}
```

| `Data` 字段 | 说明 |
|-------------|------|
| `StreamStatus` | `Running`（流式中间态）/ `Completed`（最终） |
| `UserOutputFlag` | `0` = 用户可见文本；`1` = progress（技能执行通知） |
| `UserOutputObject` | 预留（skill 返回的 code/data JSON，当前空） |

**② TaskStatusReport（任务完成信号，最后一条）**

```json
{
  "Header": {
    "MessageType": "TaskStatusManagement",
    "Sender": "HealthAgent",
    "Receiver": "SystemAgent",
    "TaskID": "task-abc-001",
    ...
  },
  "Data": {
    "MessageType": "TaskStatusReport",
    "TaskStatusCode": 200,
    "TaskStatusReport": "TaskCompleted"
  },
  "Payload": {"Function": "", "Action": ""}
}
```

| `Data.TaskStatusCode` | 含义 |
|----------------------|------|
| `200` | 任务完成 |
| `500` | 任务失败（`TaskStatusReport` 字段含错误描述） |

> **sidecar 路径**：若对接方通过 sidecar（`sidecar/main.cpp`）转发，sidecar 会把入站 AccessCtrl 转为 Bridge 的 REST SSE 调用，再把 SSE chunk 转回 AccessCtrl UserOutput 回调上游。sidecar 的 UserOutput 帧的 `Header.Sender` 为 `"SysAgent"`、`Header.Receiver` 为 `"APP"`（与 Bridge 直发的 `HealthAgent`/`SysAgent` 不同），但 `Data` 结构一致。

### 3.5.C 两种模式对照速查

| 维度 | 模式 A（直连 Bridge） | 模式 B（SystemAgent 路由） |
|------|---------------------|--------------------------|
| 入口端点 | `/api/chat/{agent_id}` / `/ws/chat/{agent_id}` | `/aisf/healthagent/v1/tasks` |
| 触发方 | 对接方主动调用 | 上游 SysAgent 调度 |
| 响应方式 | 同一连接返回（同步 JSON / SSE / WS 帧） | 202 accepted 后异步 POST 回上游 |
| 响应格式 | OpenAI-compatible（`choices[].delta`）或 Bridge ChatChunk | AISF AccessCtrl（PascalCase `Header`/`Data`） |
| 流式支持 | SSE `data:` 行 / WS 帧 | UserOutput 多次推送（`StreamStatus: Running`） |
| 产物元数据 | `data.artifacts` / `_artifacts` / `artifacts` 字段 | 需从 `UserOutputText` 的 Markdown URL 自行解析 |
| 完成信号 | `data: [DONE]` / `{"type":"done"}` | TaskStatusReport（`TaskStatusCode: 200`） |
| 错误信号 | `error` 字段 / `{"type":"error"}` | TaskStatusReport（`TaskStatusCode: 500`） |
| 认证 | 无内置（反代层加） | 无内置（AISF 系统内信任） |

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
| `content` | string 或 ContentPart[] | **用户输入**。纯文本用字符串；多模态用 content array（含 `text` + `image_url`），详见 [§4.7 实战场景](#47-实战场景上传图片获取穿搭推荐) |

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

### 4.3 流式聊天（WebSocket / SSE）

除同步 REST 接口外，Bridge 还提供 **流式 WebSocket** 和 **SSE（REST 流式）** 两种实时通道。流式接口将 LLM 的回复以 chunk 形式逐段推送，并附带结构化的产物元数据。

#### WebSocket

```
ws://127.0.0.1:8199/health/ws/chat/{agent_id}
```

**客户端发送（纯文本）：**

```json
{
  "user_id": "user_demo",
  "client_id": "client_demo",
  "content": "帮我生成一份北京商务出差的穿搭PPT"
}
```

**客户端发送（带图片的多模态输入）：**

Content 同样支持 content array 格式，与 REST 接口一致：

```json
{
  "user_id": "user_demo",
  "client_id": "client_demo",
  "content": [
    {"type": "text", "text": "看我这张照片，帮我推荐适合上班穿的搭配"},
    {"type": "image_url", "image_url": {"url": "data:image/png;base64,iVBORw0KGgoA..."}}
  ]
}
```

图片格式与 [§4.6 实战场景](#46-实战场景上传图片获取穿搭推荐) 完全一致：所有图片统一用 base64 data URL 内联在 content array 中，Bridge 对 ≥ 256KB 的大图自动落盘替换为 `file_url`，客户端无需先上传。

**Bridge 逐帧推送：**

```json
{"type":"chunk","content":"正在执行 outfit-stylist…","final":false,"progress":true}
{"type":"chunk","content":"好的，已为您生成商务出差方案。","final":false,"progress":false}
{"type":"chunk","content":"\n\n## 生成产物","final":false,"progress":false}
{
  "type": "chunk",
  "content": "\n\n[📥 下载文件 1](/api/artifacts/outfit-assistant/files/outfit-assistant/outputs/presentation.pptx)",
  "final": true,
  "artifacts": [
    {"type": "download", "url": "/api/artifacts/outfit-assistant/files/.../presentation.pptx", "name": "presentation.pptx", "mime_type": "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"type": "image", "url": "/api/artifacts/outfit-assistant/files/.../look.png", "name": "look.png", "mime_type": "image/png"}
  ]
}
{"type":"done","final":true}
```

**流式帧类型：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | `"chunk"` / `"done"` / `"error"` / `"llm_chunk"` | 帧类型 |
| `content` | string | 文本内容片段 |
| `final` | bool | `true` 时为本轮最后一条 chunk |
| `progress` | bool | `true` 时是系统进度消息（如"正在执行 skill…"），非模型输出 |
| `artifacts` | ArtifactInfo[] | **结构化产物元数据**，仅 final chunk 携带（见下文） |

> **ℹ️ `llm_chunk` 帧类型**：当 LLM 节点以流式输出时，Bridge 会推送 `type="llm_chunk"` 的帧，其 `content` 为原始 OpenAI SSE JSON 格式（`{"choices":[{"delta":{"content":"片段的token"}}]}`）。前端/外部客户端应解析 JSON 提取 `delta.content` 来拼接完整回复：
>
> ```typescript
> function extractLlmChunk(frameContent: string): string {
>   try {
>     const parsed = JSON.parse(frameContent);
>     return parsed?.choices?.[0]?.delta?.content
>         || parsed?.choices?.[0]?.message?.content
>         || frameContent;
>   } catch {
>     return frameContent; // 纯文本，直接使用
>   }
> }
> ```
>
> `chunk` 类型帧的 `progress=true` 时，其 `content` 也可能是 OpenAI JSON 格式，同样需要上述解析逻辑。

#### SSE (REST 流式)

Bridge 的 SSE（Server-Sent Events）流式接口遵循 OpenAI Chat Completions SSE 格式，并扩展了 `_artifacts` / `_dialogue_id` 非标准字段。

```
POST /api/chat/{agent_id}?stream=true
Content-Type: application/json
```

请求体与同步接口一致，加 `stream=true` 查询参数：

```bash
curl -N -X POST "http://127.0.0.1:8199/health/api/chat/outfit-assistant?stream=true" \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"帮我生成一份北京商务出差的穿搭PPT"}'
```

SSE 事件流格式（每行以 `data: ` 开头，两条换行 `\n\n` 分隔事件）：

```
data: {"choices":[{"delta":{"content":"进度：正在查询天气…"},"index":0}],"_dialogue_id":"0705123456-abc123"}

data: {"choices":[{"delta":{"content":"方案已生成，包含 PPT 文件。"},"index":0}]}

data: {"choices":[{"delta":{"content":""},"index":0}],"_artifacts":[{"type":"download","url":"/api/artifacts/...","name":"presentation.pptx","mime_type":"application/vnd..."}]}

data: [DONE]
```

> **ℹ️ Progress 消息的 content 格式**：Bridge 推送的 progress 消息中 `delta.content` 可能是纯文本（如 `"正在查询天气…"`），也可能是 OpenAI 格式 JSON 字符串（如 `{"choices":[{"message":{"content":"coordinator 决策中…"}}]}`）。外部客户端应按以下逻辑解析：
>
> ```typescript
> function extractProgress(rawContent: string): string {
>   try {
>     const parsed = JSON.parse(rawContent);
>     return parsed?.choices?.[0]?.message?.content
>         || parsed?.choices?.[0]?.delta?.content
>         || rawContent;
>   } catch {
>     return rawContent; // 纯文本，直接使用
>   }
> }
> ```

**SSE 事件字段：**

| 字段 | 类型 | 出现时机 | 说明 |
|------|------|---------|------|
| `choices[].delta.content` | string | `llm_chunk` 帧 | LLM 实时 token（流式格式 `delta.content`） |
| `choices[].message.content` | string | `text` / `progress` 帧 | 完整文本或进度消息（同步格式 `message.content`） |
| `choices[].index` | int | 每个 chunk | 恒为 0（单 choice） |
| `_dialogue_id` | string | **首个 chunk** | 对话 ID，可用于后续 trace 过滤 |
| `_artifacts` | ArtifactInfo[] | **final chunk** | 结构化产物元数据（见下文） |
| `_progress` | bool | progress 帧 | `true` 时是系统进度消息，非 LLM 输出。外部服务可据此过滤进度消息 |
| `_chunk_type` | string | 每个 chunk | 帧子类型：`"text"`（完整响应）／`"llm_chunk"`（流式 token）／`"progress"`（进度通知） |
| `error` | object | 错误时 | `{"message":"...","code":-32000}` |
| `[DONE]` | 终止信号 | 对话完成 | 无 data 体，直接作为 `data: [DONE]` 发送 |

**客户端解析要点：**

- 按 `\n\n` 分割事件，每行去掉 `data: ` 前缀后 JSON 解析
- 遇到 `data: [DONE]` 终止读取
- **`_chunk_type` 决定内容位置**：`llm_chunk` 读 `choices[0].delta.content`，`text`/`progress` 读 `choices[0].message.content`
- 首个 chunk 的 `_dialogue_id` 可用于后续 trace 查询
- 仅 final chunk 的 `_artifacts` 非空；中间 chunk 可以通过正则从 `delta.content` 中的 Markdown `![](url)` 提取 URL
- 进度消息可通过 `_progress===true` 或 `_chunk_type==="progress"` 识别并过滤

#### 结构化产物元数据（ArtifactInfo）

从 final chunk 的 `artifacts` 数组可程序化读取所有生成文件，无需解析 Markdown：

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | `"image"` / `"html"` / `"download"` | 语义类型 |
| `url` | string | 文件下载 URL（相对或绝对） |
| `name` | string | 文件名（如 `deck.pptx`） |
| `mime_type` | string | MIME 类型（如 `application/pdf`） |

```typescript
// 外部服务消费示例
interface ArtifactInfo {
  type: 'image' | 'html' | 'download';
  url: string;
  name: string;
  mime_type: string;
}
// 直接使用 url 下载文件
// 使用 type 决定展示方式：
//   image → 渲染 <img>
//   html  → 打开 iframe/新窗口
//   download → 提供下载按钮
```

### 4.4 获取图片和 HTML 产物

当 Agent 生成图片、HTML 报告或 PPT/PDF 等文件时，产物的交付通道分为两层：

- **聊天响应中的文本引用**：`content` 字段中嵌入 Markdown 格式的文件链接
- **结构化产物元数据**（流式接口）：`ChatChunk.artifacts` 数组提供程序化可读的文件清单

两种方式指向相同的文件，外部服务可选择任一种消费方式。

### 产物示例（同步 REST）

```json
{
  "success": true,
  "data": {
    "task_id": "task_abc123",
    "content": "穿搭方案已生成：\n\n![穿搭效果图](https://agent.example.com/health/api/artifacts/outfit-assistant/files/outfit-assistant/outputs/task-abc123/look.png)\n\n[查看完整 HTML 报告](https://agent.example.com/health/api/artifacts/outfit-assistant/files/outfit-assistant/outputs/task-abc123/report.html)\n\n[📥 下载文件 1](https://agent.example.com/health/api/artifacts/outfit-assistant/files/outfit-assistant/outputs/task-abc123/deck.pptx)"
  },
  "error": null
}
```

### 产物示例（流式接口）

final chunk 附带 `artifacts` 字段：

```json
{
  "type": "chunk",
  "content": "…",
  "final": true,
  "artifacts": [
    {"type": "image",    "url": "https://agent.example.com/health/api/artifacts/.../look.png",     "name": "look.png",     "mime_type": "image/png"},
    {"type": "html",     "url": "https://agent.example.com/health/api/artifacts/.../report.html",  "name": "report.html",   "mime_type": "text/html"},
    {"type": "download", "url": "https://agent.example.com/health/api/artifacts/.../deck.pptx",    "name": "deck.pptx",     "mime_type": "application/vnd.openxmlformats-officedocument.presentationml.presentation"}
  ]
}
```

外部服务的处理步骤：

1. **同步 REST**：读取 `data.content` 中的 Markdown 链接，或直接使用流式接口获取结构化 `artifacts` 数组。
2. **流式接口**：从 final chunk 的 `artifacts` 字段直接读取文件 URL、类型和文件名，无需正则解析 Markdown。
3. 使用普通 HTTP `GET` 请求访问 `/api/artifacts/{agent_id}/files/{relative_path}`。
4. 按响应的 `Content-Type` 处理文件内容；图片直接展示，HTML 打开浏览器，PPT/PDF 触发下载。
5. HTML 报告引用的图片、CSS、JavaScript 等相对资源保持原目录层级，浏览器直接打开 HTML URL 即可继续加载这些资源。

**外部服务推荐接入方式：**

| 场景 | 推荐接口 | 原因 |
|------|---------|------|
| 前端 UI 对话 | WebSocket (`/ws/chat/{agent_id}`) | 实时推送、支持进度提示 |
| 后端服务调用 | SSE (`POST ...?stream=true`) | 流式拿到结构化产物，无需轮询 |
| 简单集成/测试 | 同步 REST (`POST /api/chat/{agent_id}`) | 一次请求拿到完整回复和 Markdown URL |

文件下载示例：

```bash
curl -L \
  "https://agent.example.com/health/api/artifacts/outfit-assistant/files/outfit-assistant/outputs/task-abc123/look.png" \
  -o look.png
```

Artifact 文件接口可能返回：

| HTTP 状态码 | 含义 |
|-------------|------|
| 200 | 文件存在，返回文件流 |
| 403 | 路径越过 Agent Package 根目录 |
| 404 | 文件不存在 |
| 415 | 文件类型不支持 |

> **ℹ️ 产物收集边界（I-6）**：Agent 技能执行时，`skill_executor.extract_artifact_paths` 负责从文本输出中扫描文件路径并收为产物。为避免将**用户上传的输入文件**误报为**本次新生成的产物**，框架对以下输入目录做排除过滤：
>
> | 输入目录 | 说明 |
> |---------|------|
> | `workspace/upload/` | Bridge 自动落盘的 API 传入图片（`content_serializer._compute_upload_dir`） |
> | `uploads/` | 兼容旧版上传路径 |
>
> 这两个目录下的文件**仅作为 skill 的输入素材存在**，不会被收进 artifact 清单。外部服务的消费逻辑不受此影响——产物清单中只出现技能本次真正生成的文件。

### 4.5 外部服务接入检查

对接前应确认：

- Bridge 公网地址可从外部服务所在网络访问；
- `ARTIFACT_PUBLIC_BASE_URL` 配置为同一公网入口，且使用 `http://` 或 `https://` 完整绝对地址；
- 如 Bridge 位于反向代理子路径下，配置值包含该路径前缀；
- 网关认证策略同时覆盖聊天接口和 Artifact 文件接口；
- 外部服务不要保存或拼接服务器本地文件路径，只使用聊天响应中返回的 URL。

当前 Artifact URL 没有签名和过期时间，`agent_id` 也不是权限校验。公网部署必须由反向代理或 API Gateway 提供认证和访问控制；产物清理周期需由部署方另行管理。

### 4.6 实战场景：上传图片获取穿搭推荐

#### 场景描述

用户上传一张自己穿着日常便装的全身照，附加文字 `"帮我推荐一套适合今天上班穿的搭配，并生成效果图"`，期望 Agent 分析照片中的身材和风格后返回文字推荐 + 一张穿搭效果图。

#### 场景预览

```
用户输入                                  Agent 处理                                Agent 返回
┌──────────────────┐                    ┌────────────────────┐                   ┌──────────────────┐
│ 文本: "帮我推荐   │                    │ 1. 读图片分析身型   │                   │ 文字推荐方案      │
│  上班穿搭+效果图" │  ─── POST ───▶     │ 2. 查天气          │  ─── response ──▶ │ + 效果图 URL     │
│ 图片: 全身照.png  │                    │ 3. LLM 决策        │                   │                  │
│ (转 base64)      │                    │ 4. 生成图片        │                   │                  │
└──────────────────┘                    └────────────────────┘                   └──────────────────┘
```

#### 前提

1. `outfit-assistant` agent 已加载到 daemon（如未加载，先执行 [§3 上传与加载](#3-第一步上传-agent-package)）
2. 准备一张本地全身照（大小建议 < 5MB，太大时 Bridge 会自动落地为文件引用）

#### 方式一：同步 REST（小图片，base64 内联）

对于 base64 后 < 256KB 的小图片，可以直接把图片数据嵌在请求的 content array 中。

将图片转为 base64 data URL：

```bash
# macOS / Linux：将图片转成 data URL 字符串
IMAGE_B64="data:image/png;base64,$(base64 -i ~/Pictures/my_outfit.png | tr -d '\n')"
```

发送同步请求（`content` 为 content array）：

```bash
IMAGE_B64="data:image/png;base64,$(base64 -i ~/Pictures/my_outfit.png | tr -d '\n')"

curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d "{
    \"user_id\": \"demo\",
    \"client_id\": \"demo\",
    \"content\": [
      {\"type\": \"text\", \"text\": \"帮我推荐一套适合今天上班穿的搭配，并生成效果图\"},
      {\"type\": \"image_url\", \"image_url\": {\"url\": \"$IMAGE_B64\"}}
    ]
  }"
```

**请求字段说明**：

| 字段路径 | 类型 | 说明 |
|---------|------|------|
| `content` | `ContentPart[]` | 内容数组，至少包含 1 个 part |
| `content[].type` | `"text"` / `"image_url"` | part 类型 |
| `content[].text` | string | 文本内容（type=text 时必填） |
| `content[].image_url.url` | string | `data:image/...;base64,...` 或 `http(s)://` URL（type=image_url 时必填） |

**响应**（同步模式）：

```json
{
  "success": true,
  "data": {
    "task_id": "task_abc123",
    "content": "根据您上传的照片分析，您属于中等偏瘦体型，肤色偏暖。\n\n"
              "今日北京 26°C 晴，推荐以下商务休闲搭配：\n\n"
              "| 单品 | 推荐 | 理由 |\n"
              "|------|------|------|\n"
              "| 上装 | 浅蓝色牛津纺衬衫 | 与您的暖调肤色相配，商务感强 |\n"
              "| 下装 | 卡其色休闲西裤 | 修饰腿型，舒适透气 |\n"
              "| 鞋履 | 深棕色乐福鞋 | 与卡其色形成高级感配色 |\n"
              "| 外套 | 藏青色轻薄西装（可选） | 应对空调房 |\n\n"
              "![生成图片 1](https://agent.example.com/health/api/artifacts/outfit-assistant/files/"
              "outfit-assistant/outputs/task-abc123/look.png)\n\n"
              "效果图已生成，点击图片查看大图 📸",
    "artifacts": [
      {"type": "image", "url": "https://agent.example.com/health/api/artifacts/outfit-assistant/files/"
       "outfit-assistant/outputs/task-abc123/look.png",
       "name": "look.png", "mime_type": "image/png"}
    ]
  }
}
```

响应中的 `content` 包含 Markdown 格式的推荐表格和图片链接，`artifacts` 数组提供结构化图片元数据。外部服务可任选一种方式消费。

#### 方式二：SSE 流式（大图片，自动落盘）

对于 base64 后 ≥ 256KB 的大图片（如高清全身照），**Bridge 会自动将其落盘到 `uploads_dir`（默认 `/tmp/agentflow/uploads`）并把 content 中的 data URL 替换为 `file:///...` 形式的 `file_url`**，daemon 侧的 skill 通过 `bash` 工具调用 `outfit_vision.py` 识别该本地路径。客户端**无需先上传**——直接把 base64 data URL 内联在 content array 中发 SSE 请求即可，Bridge 在转发给 daemon 前完成落盘替换。

> ⚠️ **`POST /api/chat/uploads` 端点尚未上线**（设计文档 `frontend/bridge/docs/design/features/multimodal-chat-input.md` 标注为 Phase B 待启动）。当前 Phase A：所有图片统一走 content array 内联 base64，Bridge 自动按 256KB 阈值落盘。Phase B 上线后才会提供独立的先上传再引用通道。

**步骤 1：发送 SSE 流式请求，图片以 base64 内联**

```bash
# 大图同样直接 base64 内联（Bridge 会自动落盘替换为 file_url）
IMG_DATA="data:image/png;base64,$(base64 -i ~/Pictures/my_outfit_hires.png | tr -d '\n')"

curl -N -X POST "http://127.0.0.1:8199/health/api/chat/outfit-assistant?stream=true" \
  -H "Content-Type: application/json" \
  -d '{
    "user_id": "demo",
    "client_id": "demo",
    "content": [
      {"type": "text", "text": "帮我推荐一套适合今天上班穿的搭配，并生成效果图"},
      {"type": "image_url", "image_url": {"url": "'"$IMG_DATA"'"}}
    ]
  }'
```

落盘后的路径形如 `file:///tmp/agentflow/uploads/<sha256>.png`，skill 节点可通过 `outfit_vision.py` 直接读取该本地文件做视觉识别。

**步骤 3：实时接收 SSE 事件流**

```
# 进度通知（首个 chunk 携带 _dialogue_id）
data: {"choices":[{"delta":{"content":"进度：outfit-stylist 正在分析照片…"},"index":0}],"_dialogue_id":"0704123456-abc123def"}

# LLM 流式文字（逐 chunk 推送）
data: {"choices":[{"delta":{"content":"根据您上传的照片分析"},"index":0}]}
data: {"choices":[{"delta":{"content":"，您属于中等偏瘦体型"},"index":0}]}
data: {"choices":[{"delta":{"content":"。今日北京 26°C 晴，推荐以下搭配：\n\n"},"index":0}]}
data: {"choices":[{"delta":{"content":"| 单品 | 推荐 | 理由 |\n|------|------|------|\n| 上装 | 浅蓝色牛津纺衬衫 |..."},"index":0}]}

# LLM 输出完后，forge-image-gen 执行，返回进度
data: {"choices":[{"delta":{"content":"进度：forge-image-gen 正在渲染效果图…"},"index":0}]}

# final chunk：文字收尾 + 结构化 artifact 元数据
data: {"choices":[{"delta":{"content":"\n\n效果图已生成，点击查看 📸"},"index":0}],"_artifacts":[
       {"type":"image","url":"https://agent.example.com/health/api/artifacts/outfit-assistant/files/.../look.png",
        "name":"look.png","mime_type":"image/png"}
     ]}

# 完成信号
data: [DONE]
```

**SSE 事件字段**：

| 字段 | 类型 | 出现时机 | 说明 |
|------|------|---------|------|
| `choices[].delta.content` | string | 每个 chunk | 文本内容片段（逐段推送）。可能为空字符串 |
| `choices[].index` | int | 每个 chunk | 恒为 0（单 choice） |
| `_dialogue_id` | string | **首个 chunk** | 对话 ID，可用于后续 trace 过滤 |
| `_artifacts` | ArtifactInfo[] | **final chunk** | 结构化产物元数据（见下文） |
| `error` | object | 错误时 | `{"message":"...","code":-32000}` |
| `[DONE]` | 终止信号 | 对话完成 | 无 data 体，直接作为 `data: [DONE]` 发送 |

#### 处理产物

无论同步还是流式，收到响应后按同一方式处理生成的图片：

```bash
# 从 content 或 artifacts 中提取 URL，下载图片
curl -L -o look.png \
  "https://agent.example.com/health/api/artifacts/outfit-assistant/files/outfit-assistant/outputs/task-abc123/look.png"

# 查看图片
open look.png
```

#### 场景涉及的完整协议关键点

| 环节 | 关键点 |
|------|--------|
| 输入格式 | `content` 支持 `ContentPart[]`：`text` + `image_url`（data: / file: / http(s):） |
| 图片阈值 | base64 < 256KB 直接内联透传；≥ 256KB Bridge 自动落盘替换为 `file_url`（无需客户端先上传） |
| 进度通知 | `progress=true` 的 chunk 在技能执行期间推送，前端可展示进度条 |
| 产物交付 | final chunk 的 `_artifacts`（SSE）或 `data.artifacts`（同步）提供结构化元数据 |
| 产物下载 | HTTP GET `/api/artifacts/{agent_id}/files/{path}`，无需认证签名 |
| trace 关联 | 首个 chunk 的 `_dialogue_id` 可用于后续查询 LLM trace 列表 |

#### 完整场景 curl 速查

##### 方式一：同步 REST + base64 内联（小图 < 256KB）

```bash
# 1. 图片转 base64（使用项目示例图片）
IMG_DATA="data:image/jpeg;base64,$(base64 -i tmp/outfit_male_loungewear.jpeg | tr -d '\n')"

# 2. 发送请求，要求 Agent 描述图片中的穿搭
curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d "{
    \"user_id\":\"demo\",
    \"client_id\":\"demo\",
    \"content\":[
      {\"type\":\"text\",\"text\":\"帮我点评一下我的穿搭，必须描述我的穿了什么\"},
      {\"type\":\"image_url\",\"image_url\":{\"url\":\"$IMG_DATA\"}}
    ]
  }" | jq .

# 3. 从响应 data.content 中提取 artifact URL → 下载效果图
```

##### 方式二：SSE 流式 + base64 内联（大图 ≥ 256KB，Bridge 自动落盘）

```bash
# 1. 大图同样直接 base64 内联（Bridge 会按 256KB 阈值自动落盘替换为 file_url）
IMG_DATA="data:image/jpeg;base64,$(base64 -i tmp/outfit_male_loungewear.jpeg | tr -d '\n')"

# 2. 发送 SSE 流式请求，图片以 base64 内联
curl -N -X POST "http://127.0.0.1:8199/health/api/chat/outfit-assistant?stream=true" \
  -H "Content-Type: application/json" \
  -d '{
    "user_id": "demo",
    "client_id": "demo",
    "content": [
      {"type": "text", "text": "帮我点评一下我的穿搭，必须描述我的穿了什么"},
      {"type": "image_url", "image_url": {"url": "'"$IMG_DATA"'"}}
    ]
  }'

# 3. 实时接收 SSE 事件流，从 final chunk 的 _artifacts 提取产物 URL
```

### 4.7 对话取消与串行锁

> **背景**：同一 `(user_id, client_id)` 的多个请求路由到 daemon 同一 session。
> 为保护对话有序语义和 Task 层状态一致性，session 内对话必须串行执行——
> 新请求排队等待前一个对话完成后才执行。
>
> 串行锁由 `dialogue.interrupt`（DIALOGUE-INTERRUPT 控制命令）主动释放。
> 以下四种场景均能触发锁释放：

| 触发方式 | 适用通道 | 调用方 |
|---------|---------|--------|
| WebSocket `{"type":"stop"}` | 前端 WebSocket | 用户在对话框点"停止" |
| REST `POST /api/chat/{agent_id}/cancel` | REST SSE | 外部服务主动调用 |
| SSE 连接断开 | REST SSE | 客户端意外断线或主动关闭 |
| Daemon RPC `agent.send_control` | 直连 daemon | 管理端/调试工具 |

#### 路径一：WebSocket 停止

WebSocket 协议内置停止信号。用户在对话过程中发送 `{"type":"stop"}`，
Bridge 自动下发 `DIALOGUE-INTERRUPT` 到 daemon，释放串行锁。

```json
// 客户端 → Bridge
{"type": "stop"}
```

不需传入 `user_id` 或 `client_id`——WebSocket 连接已绑定用户标识。

#### 路径二：REST 取消端点

**端点**：`POST /api/chat/{agent_id}/cancel`

**场景**：两个浏览器窗口使用相同 `(user_id, client_id)`。A 窗口的 SSE
流尚未结束（模型耗时较长），B 窗口无法发起新对话。此时 B 可通过 cancel
端点主动中断 A 的流，释放锁后 B 即可执行。

**请求体**：

```json
{
  "user_id": "demo",
  "client_id": "demo"
}
```

**响应**：

```json
{
  "success": true,
  "data": {
    "cancelled": true,
    "dialogue_id": "0704123456-abc123def"
  }
}
```

**说明**：

- `cancelled: true` —— 对话被成功取消；daemon 中断技能任务、释放串行锁
- `cancelled: false` —— 未找到活跃对话（可能已结束或从未存在）
- 取消后，A 窗口的 SSE 流会立即收到 `data: [DONE]` 终止信号，不会继续挂等

#### 路径三：SSE 断开自动取消

REST SSE 流式请求的客户端意外断线（浏览器关闭、网络中断、主动断开连接等）
时，Bridge 自动下发 `DIALOGUE-INTERRUPT`，释放串行锁。外部服务无需额外操作。

#### 路径四：直连 daemon RPC（管理/调试）

直接操作 daemon JSON-RPC（绕过 Bridge）：

```json
{
  "jsonrpc": "2.0",
  "method": "agent.send_control",
  "params": {
    "agent_id": "outfit-assistant",
    "command": "DIALOGUE-INTERRUPT",
    "session_id": "ses_xxx",
    "user_id": "demo",
    "dialogue_id": "0704123456-abc123def"
  },
  "id": 1
}
```

此方式需要知晓内部 `session_id` 和 `dialogue_id`，推荐仅用于管理/调试场景。

#### 取消后的行为

1. daemon 中断当前运行的所有技能任务
2. 清理该对话关联的 DataBus 数据
3. 向 Bridge 推送一条 `final=true` 的终止通知
4. 释放串行锁
5. 下一个排队的对话立即开始执行

A 窗口（被取消方）的 SSE 流收到 `data: [DONE]` 后正常结束，
B 窗口（取消方）的新请求获取锁后开始流式响应。

---

## 5. 第三步：收集 Trace

### 5.1 Trace 数据说明

LLM 调用轨迹（trace）由节点脚本（`coordinator_decide.py` / `skill_executor.py`，实现收敛在 `agentflow-runtime` Python 包）通过 `_TraceRecorder` 写入 DataBus `llm/` 命名空间。每次 LLM 调用产生 3 条记录 + 1 条索引：

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
curl -X PUT http://127.0.0.1:8199/health/api/packages/outfit-assistant/update \
  -F "file=@outfit-assistant_v2.zip" \
  -F "mode=incremental"

# 更新 agent package（全量模式）
curl -X PUT http://127.0.0.1:8199/health/api/packages/outfit-assistant/update \
  -F "file=@outfit-assistant_v2.zip" \
  -F "mode=full"
```

**响应：**

```json
{
  "name": "outfit-assistant",
  "path": "/path/to/agent_packages/outfit-assistant",
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
| `POST` | `/api/chat/{agent_id}` | 同步对话请求（返回完整回复，含 Markdown 产物链接 + `data.artifacts`） |
| `POST` | `/api/chat/{agent_id}?stream=true` | SSE 流式对话（OpenAI-compatible 格式，final chunk 带 `_artifacts`） |
| `WS` | `/ws/chat/{agent_id}` | WebSocket 流式对话（实时逐帧推送，含 `progress`/`artifacts` 字段） |
| — | `/api/chat/uploads` | ⚠️ 待 Phase B 上线。当前大图走 content array 内联 base64，Bridge 按 256KB 阈值自动落盘 |

**ChatChunk 响应格式（流式）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | `"chunk"` | 帧类型标识 |
| `content` | string | 文本片段 |
| `final` | bool | 是否为本轮最后一条 chunk |
| `progress` | bool | 是否为系统进度消息（非模型输出） |
| `artifacts` | ArtifactInfo[] | 结构化产物元数据，仅 final chunk 携带 |

**ArtifactInfo：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | `"image"` / `"html"` / `"download"` | 产物语义类型 |
| `url` | string | 文件下载 URL |
| `name` | string | 文件名 |
| `mime_type` | string | MIME 类型 |

### 8.4 Artifact 文件

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/artifacts/{agent_id}/files/{relative_path}` | 获取图片、HTML 及其相对资源（legacy 兼容，内部转 artifact_id 解析） |
| `GET` | `/api/artifacts/{artifact_id}` | 按 artifact_id（形如 `{agent_id}/{session_id}/{path}`）服务文件，推荐使用 |
| `POST` | `/api/artifacts/upload` | 上传产物到 bridge 存储中心（body: `{agent_name, session_id, files:[{path, data_b64}]}`），非 chat 图片上传 |

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

**错误码说明（错误响应中的 `error.code`）：**

| code | 含义 | 说明 |
|------|------|------|
| `-32000` | 超时 | agent.execute 或 turn 超时 |
| `-32001` | daemon 错误 | daemon 返回了错误响应 |
| `-32602` | 参数校验失败 | `content` 格式错误、缺少必填字段 |
| `-32603` | 内部错误 | 服务器内部异常 |
| `-32050` | artifact 不存在 | 请求的产物文件未找到 |
| `-32051` | artifact 类型不支持 | 文件后缀不在白名单内 |
| `-32052` | artifact 路径被拒绝 | 路径穿越尝试 |

> 错误码遵循 JSON-RPC 2.0 规范中的 `Server error (-32000 to -32099)` 范围。

---

## 完整演示流程（一行一条）

> 以下提供两套速查命令：**本地 Bridge** 用于开发调试，**外部 Bridge** 用于远程演示。
> 外部地址需替换为部署方提供的实际 Bridge 公网地址。

### 本地演示（`127.0.0.1:8199`）

```bash
# 0. 确保 daemon 和 bridge 已启动

# 1. 上传 agent package
curl -k -X POST http://127.0.0.1:8199/health/api/packages/upload \
  -F "file=@tmp/outfit-test-agent.zip" -F "name=outfit-test-agent"

# 2. 加载到 daemon
curl -k -X POST http://127.0.0.1:8199/health/api/agents/outfit-test-agent/load \
  -H "Content-Type: application/json" -d '{}'

# 3. 触发对话（文本）

curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"你好"}'

curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"这两天深圳下雨，帮我推荐一套穿搭，要效果图，不要问其他问题"}'

# 3b. 图片识别：上传穿搭照并让 Agent 点评（需先有图片 base64）
IMG_B64="data:image/jpeg;base64,$(base64 -i tmp/outfit_male_loungewear.jpeg | tr -d '\n')"
curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d "{
    \"user_id\":\"demo\",
    \"client_id\":\"demo\",
    \"content\":[
      {\"type\":\"text\",\"text\":\"帮我点评一下我的穿搭，必须描述我的穿了什么,需要使用视觉理解\"},
      {\"type\":\"image_url\",\"image_url\":{\"url\":\"$IMG_B64\"}}
    ]
  }" | jq .

# 4. 查询 trace
curl "http://127.0.0.1:8199/health/api/agents/outfit-assistant/llm-traces?user_id=demo&client_id=demo"

# 5. 更新 agent package（修改 agent.md 后重新打包）
curl -X PUT http://127.0.0.1:8199/health/api/packages/outfit-assistant/update \
  -F "file=@outfit-assistant_v2.zip" -F "mode=incremental"

# 6. 再次触发
curl -X POST http://127.0.0.1:8199/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"今天上海的天气适合穿什么？"}'
```

### 外部演示（远程 Bridge）

```bash
# 0. 确保远程 daemon 和 bridge 已启动（外部地址替换为实际值）

# 1. 上传 agent package
curl -k -X POST https://115.120.249.45:40092/health/api/packages/upload \
  -F "file=@tmp/outfit-test-agent.zip" -F "name=outfit-test-agent"

# 2. 加载到 daemon
curl -k -X POST https://115.120.249.45:40092/health/api/agents/outfit-test-agent/load \
  -H "Content-Type: application/json" -d '{}'

# 3. 触发对话（快速测试用简洁 prompt）
curl -k -X POST https://115.120.249.45:40092/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"你好"}'


curl -k -X POST https://115.120.249.45:40092/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"帮我推荐一套居家穿搭，男生。给我效果图，其他不要问，用于快速测试"}'

curl -k -X POST https://115.120.249.45:40092/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demox","client_id":"demox","content":"这两天深圳下雨，帮我推荐一套穿搭，要效果图，不要问其他问题"}'



# 3b. 图片识别：上传穿搭照并让 Agent 点评（需先有图片 base64）
IMG_B64="data:image/jpeg;base64,$(base64 -i tmp/outfit_male_loungewear.jpeg | tr -d '\n')"
curl -k -X POST https://115.120.249.45:40092/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d "{
    \"user_id\":\"demo\",
    \"client_id\":\"demo\",
    \"content\":[
      {\"type\":\"text\",\"text\":\"帮我点评一下我的穿搭，必须描述我的穿了什么\"},
      {\"type\":\"image_url\",\"image_url\":{\"url\":\"$IMG_B64\"}}
    ]
  }" | jq .

# 4. 查询 trace
curl "https://115.120.249.45:40092/health/api/agents/outfit-assistant/llm-traces?user_id=demo&client_id=demo"

# 5. 更新 agent package（修改 agent.md 后重新打包）
curl -X PUT https://115.120.249.45:40092/health/api/packages/outfit-assistant/update \
  -F "file=@outfit-assistant_v2.zip" -F "mode=incremental"

# 6. 再次触发
curl -k -X POST https://115.120.249.45:40092/health/api/chat/outfit-assistant \
  -H "Content-Type: application/json" \
  -d '{"user_id":"demo","client_id":"demo","content":"帮我推荐一套居家穿搭，男生。给我效果图，其他不要问，用于快速测试"}'
```