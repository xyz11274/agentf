# 详细设计: 新建智能体包 UI — 三种创建模式

> **文档级别**: L4 技术设计
> **日期**: 2026-07-13
> **状态**: 草案
> **关联**: `frontend/web/src/components/PackageCreator.vue`（现有 DAG 编排器）、`frontend/web/src/pages/PackagesPage.vue`（包列表页）、`frontend/web/src/components/FileTree.vue`（文件树组件）、`docs/design/agent-template-delta.md`（Agent 四层架构）、`042-feature-generic-harness-agent-package`（Coordinator-driven Agent）、`039-feature-vue-frontend`（Vue 前端架构）

---

## 1. 背景

### 1.1 问题陈述

当前新建智能体包的入口单一：用户点击「新建」后通过路由 `/packages/create` 进入 `PackageCreator.vue` DAG 画布，对 `agent.json` 的 Task 拓扑进行可视化编排。

这种"全白盒编排"模式适用于高级用户需要精细控制 Task 拓扑的场景，但对两类用户场景覆盖不足：

| 用户场景 | 当前问题 |
|---------|---------|
| **快速创建通用 Harness Agent** | 用户只需要填写 prompt + 模型配置，不想理解 Task/Dispatcher/Coordinator/Skill 等概念 |
| **对话式创建** | 用户希望通过自然语言描述需求，由内置 Agent 智能体逐步引导完成创建 |

### 1.2 设计目标

提供**三种新建模式**，覆盖不同用户场景：

| 模式 | 目标用户 | 核心理念 | 复杂度 |
|------|---------|---------|--------|
| **模式一：快速创建** | 初级用户、快速原型 | 填空即用，参考"上传智能体"流程 | ★☆☆ |
| **模式二：白盒编排** | 高级用户、架构师 | DAG 拖拽编排（当前 PackageCreator） | ★★★ |
| **模式三：对话创建** | 所有用户 | 自然语言 + AI 引导，所见即所得 | ★★☆ |

三种模式共享同一个输出目标：生成一个完整的 agent package（`agent.json` + `agent.md` + `harness/` 结构）。

### 1.3 非目标

- 不修改已有的 `PackageCreator.vue` 内部逻辑（模式二直接复用）
- 不改动后端 package API（模式一复用已有 `create-from-template` 等接口）
- 不涉及 agent package 的加载/部署/运行
- 不涉及多租户/权限/版本管理

---

## 2. 整体方案

### 2.1 页面架构

在 `PackagesPage` 的「新建」按钮下拉菜单中选择创建模式：

```
PackagesPage
  └─ 新建 (dropdown)
       ├─ 🚀 快速创建 (Harness Agent)   → /packages/create/quick
       ├─ 🔧 白盒编排 (Workflow DAG)    → /packages/create   (已有路由)
       └─ 💬 对话创建                    → /packages/create/chat
```

### 2.2 路由设计

当前已有路由 `/packages/create` 直接挂载 `PackageCreator.vue` 组件。新模式各走独立路由：

| 路径 | 组件 | 模式 |
|------|------|------|
| `/packages/create` (已有) | 复用当前 `PackageCreator.vue`，进入 DAG 编排 | 白盒编排（默认） |
| `/packages/create/quick` | `QuickCreatePage.vue` (新建) | 快速创建 |
| `/packages/create/chat` | `ChatCreatePage.vue` (新建) | 对话创建 |

### 2.3 模式选择入口

在 `PackagesPage` 中，将「新建」按钮改为下拉菜单，包含三种模式：

```
 PackagesPage

    ┌─── 新建 ── ▼ ────────────────────────┐
    │ 🚀 快速创建                          │
    │     填 prompt + 模型 → 生成 Agent     │
    ├──────────────────────────────────────┤
    │ 🔧 白盒编排                          │
    │     拖拽 Task DAG → 精细控制         │
    ├──────────────────────────────────────┤
    │ 💬 对话创建                          │
    │     自然语言描述 → AI 帮你创建        │
    └──────────────────────────────────────┘

    [包卡片1]     [包卡片2]     [包卡片3]
```

**说明**：下拉菜单不与"新建"按钮嵌套在同一方框内，避免嵌套边框宽度失配导致溢出。按钮和下拉面板在视觉上通过纵向位置关联。

---

## 3. 模式一：快速创建（Quick Create）

### 3.1 交互流程

```
快速创建 (三步式表单)
┌────────────────────────────────────────────────────────┐
│ Step 1: 基本信息                                        │
│                                                         │
│  ┌─────────────────────────────────────────────────────┐│
│  │ Agent 名称: [____ my-weather-assistant ____________] ││
│  │ 描述:       [____ 天气查询助手，支持全球天气查询 _]  ││
│  └─────────────────────────────────────────────────────┘│
│                                                         │
│ Step 2: 模型配置                                        │
│  ┌─────────────────────────────────────────────────────┐│
│  │ 提供商:   [openai ▼]                                 ││
│  │ 模型名:   [____ gpt-4o-mini _____________________]  ││
│  │ 接口地址: [____ https://api.openai.com/v1 ________] ││
│  │ API Key:  [____ sk-... (password) _______________]  ││
│  └─────────────────────────────────────────────────────┘│
│                                                         │
│ Step 3: 系统提示词 (System Prompt)                       │
│  ┌─────────────────────────────────────────────────────┐│
│  │                                                      ││
│  │ ┌──────────────────────────────────────────────────┐ ││
│  │ │ 你是一个天气助手，负责查询全球天气。           │ ││
│  │ │ 你可以：                                       │ ││
│  │ │ - 查询当前天气                                 │ ││
│  │ │ - 查询未来天气预报                             │ ││
│  │ │ - 比较多个城市的天气                           │ ││
│  │ │ 请用友好的语气回复用户。                       │ ││
│  │ └──────────────────────────────────────────────────┘ ││
│  │                                                      ││
│  │ 提示: 你可以在这里描述 Agent 的行为和知识             ││
│  └─────────────────────────────────────────────────────┘│
│                                                         │
│  [上一步]                          [创建 Agent 🚀]     │
└────────────────────────────────────────────────────────┘
```

### 3.2 生成的 Agent Package

快速创建生成标准的 `harness_agent` 模板包：

```
{agent-name}/
├── agent.json           ← 从 template 复制，填充 name/description
├── agent.md             ← 从用户填写的 info + model + prompt 生成 frontmatter + body
├── harness/
│   ├── tools/           ← 从 template 复制（标准工具集：bash, read_file, write_file, ...）
│   └── skills/          ← 空目录（用户后续可通过 skills 上传或模式二补充）
├── nodes/               ← 从 template 复制（shell wrapper + 入口文件）
└── .env                 ← 从 template 复制 + 合并用户 model api_key
```

**关键点**：
- **不走 DAG 编排**：用户不需要知道 Task/Coordinator/Dispatcher 概念
- **复用 `create-from-template` API**：后端流程与现有"上传智能体"一致
- **生成标准 harness-agent**：包含 coordinator-driven loop（V2 架构），`agent.md` 中的 system prompt 直接作为 coordinator 的业务意图

### 3.3 前端组件设计

**`QuickCreatePage.vue`** — 三步式表单页面：

```
QuickCreatePage
├── StepIndicator (1→2→3 进度条)
├── Step1BasicInfo
│   ├── el-form
│   │   ├── el-form-item(label="Agent 名称") > el-input
│   │   ├── el-form-item(label="描述") > el-input
│   │   └── el-button (下一步)
├── Step2ModelConfig
│   ├── el-form
│   │   ├── el-form-item(label="提供商") > el-select
│   │   ├── el-form-item(label="模型名称") > el-input
│   │   ├── el-form-item(label="接口地址") > el-input
│   │   ├── el-form-item(label="API Key") > el-input(type=password)
│   │   └── el-button (上一步 / 下一步)
├── Step3SystemPrompt
│   ├── CodeMirror (prompt 编辑器，带 markdown 预览)
│   ├── 预设 prompt 模板下拉
│   ├── el-button (上一步 / 创建)
└── ResultPreview (折叠)
    ├── agent.json 预览
    └── agent.md 预览
```

**Prompt 模板预设**（可选）：
- 通用助手：`"你是一个通用 AI 助手，通过工具调用完成任务..."`
- 代码审查：`"你是一个代码审查专家，重点关注可读性、性能、安全性..."`
- 数据分析：`"你是一个数据分析师，擅长处理 CSV/JSON 数据..."`
- 客服机器人：`"你是一个友好的客服代表，负责回答用户咨询..."`

### 3.4 后端复用

快速创建复用后端 `POST /api/packages/create-from-template` 端点，一次性完成模板合并：

| 步骤 | 后端调用 | 说明 |
|------|---------|------|
| 调用模板合并 | `POST /api/packages/create-from-template` | 直接调用已有端点，传入 `file`（zip/tar.gz 含 `agent.md` + `harness/skills/`）、`name`（包名）、`template=harness_agent` |
| 生成 agent.md | 前端生成 frontmatter + body 后打包到 zip 的 `agent.md` | 用户填写的 name/model/prompt → YAML frontmatter + Markdown body |
| 复制模板文件 | 后端 `create-from-template` 内部 merger 逻辑 | 自动合并 `_TEMPLATE_ONLY` 文件和用户文件 |

**为什么直接调用 `create-from-template` 而不是分步 API**：该端点已封装模板合并（`merger.py`）、入口文件生成、`.env` 合并、`setup.sh` 执行等完整流程，复用避免重复实现。

见 `frontend/bridge/routes/packages.py:837` 的 `@router.post("/create-from-template")`，接受参数：
- `file` (UploadFile)：用户提供的 zip/tar.gz 归档
- `name` (str, optional)：包名，不传则从归档文件名推断
- `template` (str, default="harness_agent")：使用的模板名称

---

## 4. 模式二：白盒编排（White-box DAG Orchestration）

### 4.1 交互流程

复用现有 `PackageCreator.vue` 的 DAG 编排模式，入口为默认创建方式：

```
白盒编排 (DAG 画布)
┌────────────────────────────────────────────────────────────┐
│ Header: [Agent 名称] [版本] [描述] [会话池] [最大工作流]  │
│                                                             │
│  ┌──────┐  ┌──────────────────────────────┐  ┌──────────┐ │
│  │工具箱 │  │                              │  │ 属性面板  │ │
│  │       │  │   ┌──────────┐               │  │          │ │
│  │ agent │  │   │Agent 定义│               │  │ 模型配置  │ │
│  │  context│  │   └────┬─────┘               │  │          │ │
│  │       │  │        │                      │  │ 技能清单  │ │
│  │  dispatcher│  │   ┌──────────┐               │  │          │ │
│  │       │  │   │Dispatcher│               │  │          │ │
│  │  skill│  │   └────┬─────┘               │  │          │ │
│  │       │  │     ┌──┴──┐                  │  │          │ │
│  │  coordinator│  │     │                    │  │          │ │
│  │       │  │  ┌─────┐ ┌─────┐             │  │          │ │
│  │       │  │  │skill│ │skill│             │  │          │ │
│  │       │  │  └──┬──┘ └──┬──┘             │  │          │ │
│  │       │  │     └──┬──┘                  │  │          │ │
│  │       │  │   ┌──────────┐               │  │          │ │
│  │       │  │   │Coordinator               │  │          │ │
│  │       │  │   └──────────┘               │  │          │ │
│  │       │  └──────────────────────────────┘  │          │ │
│  └──────┘                        [自动布局]   └──────────┘ │
│                                                             │
│ [ 3 个 Task · 2 条边 · 拖拽添加 · 连线编排 · Delete 删除 ] │
└────────────────────────────────────────────────────────────┘
```

### 4.2 当前实现状态

`PackageCreator.vue` 已完整实现：
- Vue-Flow 画布 + 5 种节点类型（agent_def / contextupdater / dispatcher / coordinator / skill）
- 拖拽放置、连线、右键删除、自动布局
- 属性面板（模型配置、技能清单、system prompt、workflow 引用等）
- agent.json / agent.md / package.json 实时预览
- 最终通过 API 创建 package

**本轮无需修改此组件**，仅将其作为模式二的子路由承载。

---

## 5. 模式三：对话创建（Conversational Creation）

### 5.1 交互逻辑

用户通过自然语言描述想要创建的 Agent，由内置的"创建智能体"Agent 逐步引导，实时展示生成的包结构。

**核心思想**：左侧对话面板进行需求沟通，右侧文件树面板实时反映生成的 agent package 结构。

```
对话创建 (双面板布局)
┌───────────────────────────────────────────────────────────────┐
│ 对话创建智能体                                                  │
│                                                                │
│  ┌──────────────────────────┐  ┌─────────────────────────────┐ │
│  │   对话面板 (60%)          │  │   文件树面板 (40%)           │ │
│  │                          │  │                             │ │
│  │  系统: 欢迎！需要         │  │  📁 my-trading-assistant/   │ │
│  │       创建一个什么类型    │  │  ├── agent.json             │ │
│  │       的 Agent？          │  │  ├── agent.md               │ │
│  │                          │  │  ├── harness/               │ │
│  │  用户: 我想创建一         │  │  │   ├── tools/             │ │
│  │       个量化交易辅助      │  │  │   │   ├── bash.py       │ │
│  │       Agent               │  │  │   │   ├── read_file.py  │ │
│  │                          │  │  │   │   └── ...            │ │
│  │  系统: 好的！需要         │  │  │   └── skills/           │ │
│  │       哪些市场？A股、     │  │  │       └── analyze/       │ │
│  │       美股、加密货币？    │  │  │           └── SKILL.md  │ │
│  │                          │  │  ├── nodes/                 │ │
│  │  用户: A股和美股          │  │  │   ├── coordinator.sh    │ │
│  │                          │  │  │   ├── coordinator.py     │ │
│  │ [输入消息...] [发送]      │  │  │   ├── skill_executor.sh │ │
│  └──────────────────────────┘  │  │   └── ...                │ │
│                                │  ├── .env                   │ │
│                                │  └── requirements.txt       │ │
│                                │                             │ │
│                                │  [预览 agent.json]          │ │
│                                │  [预览 agent.md]            │ │
│                                │                             │ │
│                                │  [保存]  [取消]              │ │
│                                └─────────────────────────────┘ │
└───────────────────────────────────────────────────────────────┘
```

**说明**：使用简单的缩进代替嵌套子边框。左侧对话气泡以缩进段落表示（不嵌套子 `┌──┐` 框），避免 CJK 内容溢出内层边框。

### 5.2 交互流程（详细）

```
用户打开对话创建页面
  │
  ▼
系统发送欢迎消息:
  "你好！我来帮你创建新的 Agent。请描述你想要创建什么样的智能体？"
  "例如：'一个穿搭顾问 Agent' 或 '一个代码审查助手'"
  │
  ▼ 用户输入: "我想创建一个量化交易助手"
  │
  ▼ 内置创建 Agent 分析需求
  ├─ 询问: "好的！需要支持哪些市场？(A股/美股/加密货币)"
  ├─ 用户: "A股和美股"
  ├─ 询问: "需要哪些功能？(选股分析/技术指标/风险评分/回测)"
  ├─ 用户: "选股分析和风险评分"
  ├─ 询问: "使用什么模型？(默认 gpt-4o-mini)"
  │
  ▼ 需求收集完毕 → 内置 Agent 开始生成
  ├─ 生成 agent.json（从 template 复制并填充）
  ├─ 生成 agent.md（包含量化交易相关的 system prompt）
  ├─ 生成初始 SKILL.md（选股分析 + 风险评分的技能文档）
  │   ├─ harness/skills/stock-analysis/SKILL.md
  │   └─ harness/skills/risk-scoring/SKILL.md
  │
  ▼ 右侧文件树实时刷新
  ├─ 用户可以看到完整的包结构生成
  ├─ 展开文件可预览内容
  │
  ▼ 内置 Agent 询问:
  "创建完成！我为你生成了以下内容：
  1. 选股分析 skill（stock-analysis）
  2. 风险评分 skill（risk-scoring）
  
  你可以：
  - 直接在右侧文件树中编辑文件
  - 让我调整某些配置
  - 点击「保存」完成创建"
  │
  ├─ 用户: "给 stock-analysis 加个 technical-indicators 子技能"
  │     → Agent 生成新的 SKILL.md → 文件树刷新
  │
  └─ 用户点击「保存」→ 调用后端 API 创建 package → 跳转到编辑页面
```

### 5.3 架构设计

#### 5.3.1 整体架构

对话创建模式引入一个**内置的"创建智能体"Agent**（SYS_AGENT），其职责是：

1. **需求引导**：通过多轮对话收集用户意图（Agent 类型、功能、模型等）
2. **结构生成**：根据需求生成完整的 agent package 结构（文件树）
3. **实时反馈**：每次生成/修改后，刷新右侧文件树面板
4. **所见即所得**：用户可随时预览和编辑生成的文件

```
┌──────────────────────────────────────────────────────────┐
│                       浏览器端                             │
│                                                           │
│  ChatCreatePage.vue                                       │
│  ├── ChatPanel.vue (左侧)                                  │
│  │   ├── 消息列表 (用户 + 创建 Agent 对话)               │
│  │   ├── 输入框 + 发送按钮                                │
│  │   └── 建议操作按钮 (快速选择)                          │
│  │                                                         │
│  ├── FileTreePreview.vue (右侧)                           │
│  │   ├── 文件树 (实时更新的包结构)                        │
│  │   ├── 文件预览 (选中文件时显示内容)                    │
│  │   └── 状态指示 (生成中/已保存)                         │
│  │                                                         │
│  └── ActionBar.vue (底部)                                  │
│      ├── 保存按钮                                          │
│      └── 取消按钮                                          │
│                                                           │
│  存储: chatCreateStore (Pinia)                             │
│  ├── messages: 对话历史                                   │
│  ├── fileTree: 当前生成的文件树                           │
│  ├── packageName: 包名                                    │
│  └── isSaving, isGenerating 等状态                        │
└──────────────────────────┬───────────────────────────────┘
                           │ HTTP SSE → /api/chat/create-agent
                           ▼
┌──────────────────────────────────────────────────────────┐
│                    桥接服务 (FastAPI)                       │
│                                                           │
│  POST /api/chat/create-agent (SSE endpoint)               │
│  ├─ 接收用户消息                                          │
│  ├─ 调用 LLM (创建 Agent 的 system prompt + 用户消息)    │
│  ├─ LLM 返回结构化命令:                                    │
│  │   - add_file(path, content)  ─ 新增/修改文件           │
│  │   - delete_file(path)        ─ 删除文件                │
│  │   - ask_question(question)   ─ 向用户提问               │
│  │   - finalize()               ─ 完成创建                │
│  ├─ 执行命令 (修改内存中的文件树)                          │
│  └─ SSE 推送增量更新到前端                                  │
│     - {type: "think", content: "正在生成..."}              │
│     - {type: "file_changed", path, content}               │
│     - {type: "question", content: "...是否需要调整?"}      │
│     - {type: "done"}                                       │
└──────────────────────────────────────────────────────────┘
```

#### 5.3.2 创建 Agent 的 System Prompt

内置的"创建智能体"Agent 使用专门的 system prompt，定义其职责和能力：

```markdown
# 创建智能体 (Agent Creator)

你是一个专门帮助用户创建新的 Agent 包的 AI 助手。
你的职责是：

## 1. 需求引导
通过对话逐步了解用户的需求：
- Agent 的核心功能（做什么）
- 是否需要集成外部工具/API
- 使用什么模型
- 是否需要自定义技能 (SKILL.md)

## 2. 生成 Agent Package

当你收集到足够信息时，使用以下命令来生成文件结构：

### add_file(path, content)
在当前创建会话中添加/更新一个文件。
- path: 相对于包根目录的路径（如 agent.json, harness/skills/xxx/SKILL.md）
- content: 文件内容

### delete_file(path)
删除一个已创建的文件。

### ask_question(question)
向用户提问，需要用户回答后才能继续。

### finalize()
确认创建完成。调用此命令后，用户可以「保存」将包写入磁盘。

## 3. 生成策略

- 为复杂功能创建独立的 SKILL.md（放在 harness/skills/{name}/ 下）
- agent.md 中的 body 作为 coordinator 的 system prompt
- agent.json 使用标准 harness_agent 模板
- 需要 API Key 的配置放在 .env 中
```

#### 5.3.3 SSE 协议设计

前端与 Bridge 之间的 SSE 协议设计如下。**注意**：由于前端 `EventSource` API 原生只支持 GET 请求，此处使用 `fetch` + `ReadableStream` 实现 POST 请求的 SSE 流式读取。

**请求**:
```
POST /api/chat/create-agent
Content-Type: application/json
Accept: text/event-stream

{
  "message": "我想创建一个量化交易助手",
  "session_id": "create-session-001"
}
```

**响应 (SSE stream)**:
```
event: think
data: {"content": "好的！我来帮你创建量化交易助手。首先，需要知道你想交易哪些市场？"}

event: question
data: {"content": "需要支持哪些市场？（A股、美股、加密货币）", "suggestions": ["A股", "美股", "A股+美股", "加密货币"]}

event: file_changed
data: {"path": "agent.json", "content": "{...}", "operation": "add"}

event: file_changed
data: {"path": "agent.md", "content": "---\nname: trading-assistant\n...", "operation": "add"}

event: file_changed
data: {"path": "harness/skills/stock-analysis/SKILL.md", "content": "# Stock Analysis Skill\n...", "operation": "add"}

event: tree_snapshot
data: {"tree": [{"path": "agent.json", "size": 1234}, ...]}

event: done
data: {"message": "创建完成！你可以点击「保存」将包写入磁盘。"}
```

**事件类型**：

| 事件 | 说明 | 字段 |
|------|------|------|
| `think` | AI 正在思考/生成 | `content` |
| `question` | AI 向用户提问 | `content`, `suggestions[]` (可选) |
| `file_changed` | 文件被新增/修改 | `path`, `content`, `operation`("add"/"update"/"delete") |
| `tree_snapshot` | 完整文件树快照 | `tree: [{path, size?}]` |
| `error` | 错误 | `content` |
| `done` | 本轮处理完成 | `message` |

### 5.4 前端组件设计

#### 5.4.1 `ChatCreatePage.vue`

页面级容器，管理双面板布局：

```
ChatCreatePage
├── PageHeader
│   ├── title: "对话创建智能体"
│   └── 说明文字
├── DualPanelLayout
│   ├── ChatPanel (左侧, 60%)
│   │   ├── ChatMessageList
│   │   │   ├── MessageBubble (user) — 用户消息
│   │   │   ├── MessageBubble (assistant) — AI 回复（含 think 过程）
│   │   │   ├── FileChangeNotice — 文件变更通知
│   │   │   └── QuestionCard — AI 提问 + 建议选项
│   │   ├── QuickSuggestions — 快捷建议按钮
│   │   └── MessageInput — 文本输入框 + 发送按钮
│   │
│   └── FileTreePreview (右侧, 40%)
│       ├── FileTree — 文件树组件（复用已有 FileTree.vue 或创建子版本）
│       │   ├── 目录展开/折叠
│       │   ├── 文件图标（新增/修改/删除 状态标记）
│       │   └── 点击文件 → 预览内容
│       ├── FilePreview — 文件内容预览（语法高亮）
│       └── StatusBar — 状态指示（N 个文件 · N 次修改）
│
└── ActionBar (底部)
    ├── 保存 按钮（将当前文件树写入磁盘，调用后端 API）
    ├── 重置 按钮（清空当前会话）
    └── 取消 按钮（返回包列表页）
```

#### 5.4.2 `ChatCreateStore` (Pinia Store)

```typescript
// stores/chatCreateStore.ts
interface ChatCreateState {
  // 对话历史
  messages: Array<{
    role: 'user' | 'assistant' | 'system'
    content: string
    type?: 'think' | 'question' | 'file_change'
    suggestions?: string[]
    fileChanges?: Array<{ path: string; operation: 'add' | 'update' | 'delete' }>
  }>

  // 当前生成的文件树（内存中）
  files: Map<string, {
    path: string
    content: string
    status: 'new' | 'modified' | 'unchanged'
  }>

  // 包元数据
  packageName: string

  // 状态
  isGenerating: boolean
  isSaving: boolean
  sessionId: string  // 对话 session

  // SSE 连接
  eventSource: EventSource | null
}
```

### 5.5 后端设计

#### 5.5.1 SSE 端点

```
POST /api/chat/create-agent
```

在 `chat.py`（或新建 `create_agent_chat.py`）中新增 SSE 端点。注意：SSE 长连接场景下 `background_tasks` 的绑定生命周期存在问题，应使用 `StreamingResponse` + 异步生成器：

```python
from fastapi.responses import StreamingResponse

@router.post("/chat/create-agent")
async def chat_create_agent(
    body: CreateAgentChatRequest,
):
    """
    SSE endpoint for conversational agent creation.
    
    接收用户消息，调用 LLM（创建 Agent 专用），
    解析 LLM 返回的结构化命令，执行文件操作，
    通过 SSE 推送增量更新到前端。

    使用 StreamingResponse + 异步生成器而非 background_tasks，
    避免长连接下 tasks 生命周期竞态。
    """
    async def event_generator():
        session_id = body.session_id
        message = body.message
        
        # 1. 构造 LLM 调用
        # 2. 解析命令
        # 3. 执行文件操作
        # 4. yield SSE events
        
        yield f"event: done\ndata: {json.dumps({'message': '完成'})}\n\n"
    
    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
            "X-Accel-Buffering": "no",  # 禁用 nginx 缓冲
        }
    )
```

#### 5.5.2 LLM 调用与命令解析

后端维护一个"创建 Agent"的 LLM 会话，每次用户消息到来时：

1. 追加用户消息到对话历史
2. 调用 LLM，使用专用的 system prompt（§5.3.2）
3. 解析 LLM 响应中的结构化命令（通过 function calling 或 JSON 格式）
4. 执行命令（修改内存中的文件树）
5. 通过 SSE 推送增量更新

**命令格式**（通过 function calling）：

```json
// LLM 调用 add_file 工具
{
  "function": "add_file",
  "arguments": {
    "path": "harness/skills/stock-analysis/SKILL.md",
    "content": "---\nname: stock-analysis\n...\n# Stock Analysis\n..."
  }
}

// LLM 调用 ask_question 工具
{
  "function": "ask_question",
  "arguments": {
    "question": "需要支持哪些市场？",
    "suggestions": ["A股", "美股", "A股+美股"]
  }
}
```

#### 5.5.3 文件操作接口

后端 `chat_create_store`（内存中）维护文件树状态，最终「保存」时通过已有 API 写入磁盘：

> **注意**：`_normalize_permissions` 是 `frontend/bridge/routes/packages.py:457` 中的私有函数，跨模块不可直接引用。实施时需将其提取为公共 util（如放到 `frontend/bridge/utils.py`）或直接在保存时调用 `packages.py` 中的既存写入路径。

```python
# 内存中的文件操作（不直接写磁盘）
class CreateAgentSession:
    def __init__(self, session_id: str):
        self.session_id = session_id
        self.files: dict[str, str] = {}  # path → content
        self.messages: list = []
        self.package_name: str = ""
    
    def add_file(self, path: str, content: str):
        self.files[path] = content
    
    def delete_file(self, path: str):
        self.files.pop(path, None)
    
    async def save_to_disk(self, packages_dir: Path) -> str:
        """将内存中的文件树写入磁盘，创建 agent package。"""
        pkg_name = self.package_name or f"chat-created-{uuid4().hex[:8]}"
        pkg_dir = packages_dir / pkg_name
        pkg_dir.mkdir(parents=True, exist_ok=True)
        
        for path, content in self.files.items():
            target = pkg_dir / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        
        # 标准化权限
        _normalize_permissions(pkg_dir)
        
        return pkg_name
```

---

## 6. 三种模式的比较

| 维度 | 模式一：快速创建 | 模式二：白盒编排 | 模式三：对话创建 |
|------|----------------|----------------|----------------|
| **复杂度** | ★☆☆ (三步表单) | ★★★ (DAG 画布) | ★★☆ (对话界面) |
| **目标用户** | 初级/快速原型 | 高级/架构师 | 所有用户 |
| **需要理解的概念** | 无（只需 prompt） | Task/Dispatcher/Skill 等 | 无（自然语言） |
| **灵活性** | 低（模板化） | 高（完全可控） | 中（AI 生成 + 可调整） |
| **学习成本** | ~1 分钟 | ~15 分钟 | ~1 分钟 |
| **生成速度** | 即时 | 即时 | 多轮对话 (1-3 分钟) |
| **最终产出** | 标准 harness_agent | 自定义 agent.json | 标准或自定义 |
| **是否支持自定义技能** | 否（后续补充） | 是 | 是（AI 自动生成） |
| **复用现有代码** | 复用模板 API + 新前端 | 复用 PackageCreator.vue | 新前端 + 新 SSE 端点 |

---

## 7. 路由与导航变更

### 7.1 路由文件变更

```typescript
// router/index.ts — 新增路由（使用顶层 import 风格，与已有路由一致）
import QuickCreatePage from '../pages/QuickCreatePage.vue'
import ChatCreatePage from '../pages/ChatCreatePage.vue'

const routes: RouteRecordRaw[] = [
  {
    path: '/',
    component: MainLayout,
    redirect: '/packages',
    children: [
      // ... 已有路由（保持不变）
      
      // 原有创建路由保持（直接指向 PackageCreator）
      { path: 'packages/create', name: 'package-create', component: PackageCreator, meta: { title: '创建智能体包' } },
      
      // 新增：快速创建
      { path: 'packages/create/quick', name: 'package-create-quick', component: QuickCreatePage, meta: { title: '快速创建智能体' } },
      
      // 新增：对话创建
      { path: 'packages/create/chat', name: 'package-create-chat', component: ChatCreatePage, meta: { title: '对话创建智能体' } },
    ],
  },
]
```

### 7.2 PackagesPage 新建按钮变更

```vue
<!-- PackagesPage.vue — 新建按钮改为下拉菜单 -->
<el-dropdown @command="handleCreateMode">
  <el-button type="primary" :icon="Plus">新建</el-button>
  <template #dropdown>
    <el-dropdown-menu>
      <el-dropdown-item command="quick">
        🚀 快速创建
        <span class="mode-desc">填 prompt + 模型 → 生成 Agent</span>
      </el-dropdown-item>
      <el-dropdown-item command="dag">
        🔧 白盒编排
        <span class="mode-desc">拖拽 Task DAG → 精细控制</span>
      </el-dropdown-item>
      <el-dropdown-item command="chat">
        💬 对话创建
        <span class="mode-desc">自然语言描述 → AI 帮你创建</span>
      </el-dropdown-item>
    </el-dropdown-menu>
  </template>
</el-dropdown>
```

---

## 8. 文件变更清单

### 8.1 新增文件

| 文件 | 说明 | 预估行数 |
|------|------|---------|
| `frontend/web/src/pages/QuickCreatePage.vue` | 快速创建页面（三步表单） | ~300 |
| `frontend/web/src/pages/ChatCreatePage.vue` | 对话创建页面（双面板布局） | ~200 |
| `frontend/web/src/components/ChatPanel.vue` | 对话面板组件（消息列表 + 输入框） | ~250 |
| `frontend/web/src/components/FileTreePreview.vue` | 文件树预览面板（含文件预览） | ~200 |
| `frontend/web/src/stores/chatCreateStore.ts` | 对话创建状态管理 | ~150 |
| `frontend/bridge/routes/create_agent_chat.py` | SSE 端点 + 创建 Agent 会话管理 | ~200 |

### 8.2 修改文件

| 文件 | 变更说明 | 预估行数 |
|------|---------|---------|
| `frontend/web/src/router/index.ts` | 新增快速创建和对话创建路由 | +15 |
| `frontend/web/src/pages/PackagesPage.vue` | 新建按钮改为下拉菜单，新增 `handleCreateMode` 方法 | +40 |
| `frontend/bridge/routes/chat.py`（或新建 `create_agent_chat.py`） | 新增 SSE 端点注册 | +5 |

> **说明**：文档不修改 `frontend/web/src/components/PackageCreator.vue` — 模式二直接复用；不删除已有路由 `/packages/create`（保留为白盒编排入口）。

### 8.3 不修改的文件

- `frontend/web/src/components/PackageCreator.vue` — 模式二直接复用
- `frontend/web/src/components/FileTree.vue` — 模式三可复用
- `frontend/web/src/components/ChatView.vue` — 模式三使用新的 ChatPanel，不直接复用
- `frontend/bridge/templates/` — 快速创建复用现有模板
- `frontend/bridge/routes/packages.py` — 最终保存调用已有 API

---

## 9. 实施优先级

| 优先级 | 模式 | 依赖 | 工作量 |
|--------|------|------|--------|
| P0 | 模式一：快速创建 | 无（独立） | ~1 天 |
| P1 | 模式三：对话创建 | SSE 端点 + LLM 对话 | ~3 天 |
| P2 | 模式二入口优化 | 无（已有组件） | ~0.5 天 |

**建议顺序**：
1. 先实施模式一（快速创建），独立、低风险、立即可用
2. 再实施入口菜单调整（PackagesPage 新建按钮改为下拉菜单）
3. 最后实施模式三（对话创建），需前后端协作

---

## 10. 测试方案

### 10.1 模式一：快速创建

| TC | 场景 | 预期 |
|----|------|------|
| 1 | 填写完整表单并创建 | 成功创建 agent package，agent.json 和 agent.md 内容正确 |
| 2 | 校验：名称为空时按钮禁用 | 创建按钮灰色不可点击 |
| 3 | 校验：API Key 为空时提示 | 警告但允许创建（存入占位符） |
| 4 | 模板预设选择 | system prompt 被预设内容填充 |

### 10.2 模式三：对话创建

| TC | 场景 | 预期 |
|----|------|------|
| 1 | SSE 连接与消息发送 | 消息发送后收到 SSE 流式响应 |
| 2 | AI 生成文件 | 右侧文件树实时更新，文件内容正确 |
| 3 | AI 提问与回复 | 问题显示在对话区，用户回答后继续生成 |
| 4 | 保存到磁盘 | 调用后端 API 将内存文件树写入磁盘 |
| 5 | 文件预览 | 点击文件树中的文件，显示语法高亮内容 |

### 10.3 集成测试

| TC | 场景 | 预期 |
|----|------|------|
| 1 | 三种模式切换 | 从下拉菜单选择不同模式，正确跳转到对应页面 |
| 2 | 创建后跳转到编辑器 | 创建成功后跳转到 PackagePage 编辑页面 |
| 3 | 模式三保存后文件完整性 | 保存后在后端确认所有生成的文件都存在 |

---

## 11. 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [agent-template-delta.md](agent-template-delta.md) | 模式一生成的 agent package 遵循四层架构（pip 包 + 入口文件） |
| [042-feature-generic-harness-agent-package](042-feature-generic-harness-agent-package-260630-tbd.md) | 模式一生成的是 Coordinator-driven Agent Loop 架构 |
| [039-feature-vue-frontend](039-feature-vue-frontend-260622-tbd.md) | 本设计是 039 前端架构的具体扩展 |
| [037-feature-skills-dynamic-upload](037-feature-skills-dynamic-upload.md) | 模式一创建后的 Agent 可通过 skills 上传接口补充技能 |

---

## 12. 待确认事项

| # | 事项 | 建议 | 备注 |
|---|------|------|------|
| 1 | 对话创建模式下，内置"创建 Agent"使用什么模型？ | 复用前端配置的默认 LLM（当前对话使用的模型） | 需要前端传递模型配置 |
| 2 | 对话创建 session 的过期时间 | 浏览器关闭即丢弃；用户可手工「重置」 | 不持久化，保存后才写入磁盘 |
| 3 | 文件树预览能否复用已有 `FileTree.vue`？ | 可以，但需要支持只读模式和文件状态标记 | 新建一个 `FileTreePreview` 子组件或加 props |
| 4 | SSE 断开后如何恢复？ | 不支持恢复（设计上为一次性会话）；用户可「重置」重新开始 | 简化实现 |
| 5 | 对话创建 Agent 的 SKILL.md **模板生成规范** | 应定义 SKILL.md 的 frontmatter 字段集（name/description/model 等），LLM 按规范生成 | 影响 LLM 生成的 skill 质量和一致性 |
| 6 | `chatCreateStore` 中文件树的**响应式数据结构** | 建议用 `Record<string, FileState>` 而非 `Map` | Pinia 对 `Map` 的响应式代理有边界情况 |
| 7 | 对话创建中 LLM 生成 SKILL.md 的**内容安全审查** | 应添加预检规则，拒绝生成 system prompt/工具定义等框架级覆盖内容 | LLM 可能生成有害指令 |
| 8 | 三步式快速创建的**步骤间跳转** | 当前设计仅支持上一步/下一步；是否允许 Step 3 直接回到 Step 1？ | UX 决策，需确认 |

---

## 13. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-07-13 | AtomCode | 初始版本 |
| v1.1 | 2026-07-13 | AtomCode | 审阅修正：修正 `PackageCreatePage.vue` 不存在的问题（§1.1, §2.2, §8.2）；修正 `create-from-template` API 调用方式（§3.4）；修正 SSE 端点为 `StreamingResponse` + 异步生成器（§5.5.1）；补充 `_normalize_permissions` 跨模块引用说明（§5.5.3）；修正路由 import 风格（§7.1）；统一组件设计粒度（§3.3）；补充 SSE 使用 fetch + ReadableStream 说明（§5.3.3）；补充待确认事项 Q5-Q8 |
