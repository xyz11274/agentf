# 详细设计: AgentFlow Vue 前端 — Agent Package 编辑器 + Agent 问答聊天

> **文档级别**: L3 架构设计
> **日期**: 2026-06-22
> **状态**: tbd
> **关联**: `.sisyphus/plans/vue-frontend.md`（执行计划）

---

## 1. 背景

### 1.1 问题陈述

AgentFlow 框架是一个基于 C++ Actor 反应式编程框架的 MMAgent/MMWorkflow 开发编排与执行引擎。它通过 `agentflowd` daemon 以 JSON-RPC over TCP 对外暴露接口（`agent.load/unload/execute`、`session.list/destroy` 等）。

当前项目存在以下问题：

| 问题 | 现状 | 影响 |
|------|------|------|
| **无可视化界面** | 仅通过 `test_client.py` 脚本或 `nc` 命令与 daemon 交互 | 调试 agent 只能看日志，无法直观编辑 agent package 结构 |
| **Agent Package 编辑困难** | `agent.json`、`workflow.json`、`node` 脚本等为文件系统上的文件，需手动编辑 | 缺少语法高亮、JSON 校验和 workflow 可视化 |
| **无 Workflow DAG 可视化** | 虽然有 `visualizer.html`，但仅限静态显示 | 无法直观理解节点依赖和执行流 |
| **Agent 问答无 UI** | 通过 `agent.execute` RPC 调用或 TUI 程序 | 缺少类 ChatGPT 的对话界面 |

### 1.2 设计目标

- **Agent Package 编辑器**：文件树浏览 + 代码编辑（JSON/YAML/MD/Shell）+ Workflow DAG 可视化 + 打包/加载
- **Agent 问答聊天**：类 ChatGPT 对话 + 多 Agent 切换 + Session 管理
- **前后端分离**：Vue 3 前端 + Python FastAPI 桥接服务，不经修改 daemon 核心

### 1.3 非目标

- 用户认证/登录系统
- 工作流在线编辑（拖拽式节点编排）
- 多 daemon 集群管理
- i18n 国际化
- 深色模式
- Agent Package 版本管理/历史

---

## 2. 方案简述

### 2.1 整体架构

```
┌─────────────────┐      HTTP/WS       ┌──────────────────┐      JSON-RPC      ┌──────────────┐
│   Vue 3 Frontend │ ◄──────────────► │  Python Bridge    │ ◄──────────────► │  agentflowd   │
│   (port 15173)    │    REST + WS      │  FastAPI (:8000)  │    TCP :17891      │  (daemon)     │
└─────────────────┘                   └──────────────────┘                   └──────────────┘
        │                                      │                                      │
        │                                      │ 文件系统直读                          │ 框架核心
        │                                      ▼                                      │
        │                              ┌──────────────────┐                           │
        │                              │  Agent Package    │                           │
        │                              │  (文件系统)       │                           │
        │                              │  examples/agent/  │                           │
        │                              └──────────────────┘                           │
```

**核心原则**：
- 前端**不直连** daemon TCP/WS（必须经 bridge）
- Bridge 不做任何业务推理（仅 IO 转换与转发）
- Bridge 直读文件系统（Agent Package CRUD 不经过 daemon）
- Daemon 连接仅用于 `agent.execute`、`agent.load/unload`、session 管理

### 2.2 技术选型

| 层 | 选型 | 理由 |
|----|------|------|
| 前端框架 | Vue 3 + Vite + TypeScript | 轻量，Composition API，社区活跃 |
| UI 组件 | Element Plus | 成熟的 Vue 3 组件库，中文生态 |
| 状态管理 | Pinia | Vue 3 官方推荐，替代 Vuex |
| 代码编辑器 | CodeMirror 6 | 轻量、支持多语言模式、Vue 集成好 |
| DAG 可视化 | Vue-Flow | Vue 生态的节点图组件，支持 pan/zoom |
| 后端桥接 | Python FastAPI | 快速开发、原生 WebSocket 支持、类型安全 |
| 测试 (前端) | Vitest + Playwright | 与 Vite 深度集成、E2E 覆盖 |
| 测试 (后端) | pytest | Python 标准测试框架 |

### 2.3 端口约定

| 服务 | 端口 | 协议 |
|------|------|------|
| agentflowd (daemon) | 17891 | JSON-RPC over TCP (WebSocket 升级兼容) |
| Python Bridge | 8000 | HTTP REST + WebSocket |
| Vue 前端 (dev) | 15173 | HTTP (Vite dev server) |

### 2.4 跨域方案

Vite proxy `/api` → `http://localhost:8000`（开发时），生产环境使用 nginx 反向代理。

---

## 3. 约束与假设

### 3.1 约束

- **不修改 daemon 核心**：所有新功能通过 bridge + 前端实现
- **前端不直连 daemon**：必须经 bridge 中转（无论是 HTTP REST 还是 WebSocket）
- **仅 Element Plus**：不同时引入两套 UI 组件库
- **Vue Composition API 仅**：禁止 Options API，仅 `<script setup>` + Composition API
- **状态管理仅 Pinia**：禁止 Vuex
- **YAGNI**：不为"未来扩展"预留抽象层/适配器层
- **文件安全**：bridge 文件 API 必须做路径穿越防护（拒绝 `../`）

### 3.2 假设

- Daemon 已在 TCP 17891 监听（standard RFC 6455 WebSocket 升级支持）
- Agent Package 目录结构遵循 `AGENT_PACKAGE_SPEC.md` 规范
- `agent_build.sh` 脚本可在 `publish/` 下执行，接受 source dir + output 参数
- 浏览器原生 WebSocket API 可直接连接 `ws://localhost:17891`（通过 bridge 代理）

---

## 4. 当前实现分析

### 4.1 可复用的已有资源

| 资源 | 位置 | 说明 |
|------|------|------|
| JSON-RPC API | `src/daemon/rpc_handler.cpp` | 已完整映射：agent.list/load/unload/execute/poll_response, session.list/destroy, runtime.status, health |
| WebSocket 升级 | `src/daemon/ws_upgrade.cpp` | 标准 RFC 6455，HTTP 升级握手 + text frame |
| Agent Package 规范 | `publish/agent/AGENT_PACKAGE_SPEC.md` | agent.json 字段定义，tarball 结构 |
| 打包脚本 | `publish/agent_build.sh` | 构建 agent package tarball（需确认调用方式） |
| Workflow 可视化 | `publish/agent/visualizer.html` | 静态 DAG 显示（参考用，不用 Vue-Flow 重写） |
| 示例 Agent | `examples/agent/*/package.json` | 9 个示例，可作为包结构参考 |

### 4.2 Daemon JSON-RPC API 映射

| 方法 | 用途 | 前端使用场景 |
|------|------|-------------|
| `agent.list` | 列出所有已加载 agent | Agent 列表页 |
| `agent.info` | 获取 agent 详细信息 | Agent 详情 |
| `agent.load` | 从 package path 加载 agent | 打包→加载流程 |
| `agent.unload` | 卸载 agent | Agent 管理 |
| `agent.pause` | 暂停 agent | Agent 管理 |
| `agent.resume` | 恢复 agent | Agent 管理 |
| `agent.execute` | 向 agent 发送消息 | 聊天发送消息 |
| `agent.poll_response` | 获取 agent 回复 | 聊天接收回复 |
| `session.list` | 列出 session | Session 管理 |
| `session.destroy` | 销毁 session | Session 管理 |
| `session.clear_dialogue` | 清空对话 | Session 管理 |
| `runtime.status` | daemon 状态 | 连接状态指示器 |
| `health` | 心跳检测 | 连接状态指示器 |

### 4.3 方案选型分析

#### Bridge 后端语言选择

| 选项 | 优势 | 劣势 |
|------|------|------|
| **Python FastAPI** ✅ | 快速开发，原生 WS 支持，pydantic 类型校验，生态丰富 | 需要安装 Python 依赖 |
| Node.js Express | 前端团队共享语言 | 新增运行时依赖 |
| Go Gin | 高性能 | 开发速度较慢 |

**选择 Python FastAPI**：开发速度最快，WebSocket 原生支持，与团队现有技能匹配。

#### DAG 可视化方案

| 选项 | 优势 | 劣势 |
|------|------|------|
| **Vue-Flow** ✅ | Vue 3 原生，交互丰富（pan/zoom/drag） | 需要额外包 (~50KB) |
| 原生 Canvas/SVG | 无额外依赖 | 实现成本高 |
| 复用 visualizer.html | 已有代码 | 不是 Vue 组件，难以集成 |

**选择 Vue-Flow**：与 Vue 3 深度集成，交互体验好。

#### 文件操作路径

| 选项 | 优势 | 劣势 |
|------|------|------|
| **Bridge 直读文件系统** ✅ | 简单直接，不依赖 daemon | 需要在 bridge 配置 package 根路径 |
| 通过 daemon 间接读写 | 统一入口 | daemon 缺少文件 API，需新增 RPC 方法 |

**选择 Bridge 直读文件系统**：不修改 daemon 核心，文件操作不走 RPC。

---

## 5. 接口设计

### 5.1 Bridge REST API

#### 文件系统操作

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/packages` | 列出可用 agent packages |
| GET | `/api/packages/{name}/tree` | 获取包内文件树 |
| GET | `/api/packages/{name}/file/{path}` | 读取文件内容 |
| PUT | `/api/packages/{name}/file/{path}` | 写入文件内容（新建/更新） |
| DELETE | `/api/packages/{name}/file/{path}` | 删除文件 |
| POST | `/api/packages/{name}/file/{path}/rename` | 重命名文件 |

#### Agent/Session 代理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/agents` | 代理 agent.list |
| POST | `/api/agents/{id}/load` | 代理 agent.load |
| POST | `/api/agents/{id}/unload` | 代理 agent.unload |
| GET | `/api/sessions?agent_id=X` | 代理 session.list |

#### WebSocket 聊天

| 路径 | 说明 |
|------|------|
| `/ws/chat/{agent_id}` | 聊天端点，接收 JSON 消息，返回流式回复 |

#### 系统状态

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health` | Bridge 健康检查 |
| GET | `/api/daemon/status` | Daemon 状态 |

### 5.2 前端页面路由

| 路径 | 页面 | 说明 |
|------|------|------|
| `/` | 重定向 | → `/packages` |
| `/packages` | Agent Package 列表 | 浏览/选择 package |
| `/packages/:name` | Package 编辑器 | 文件树 + 代码编辑 + DAG |
| `/chat` | Agent 问答 | Agent 列表 + 聊天 |

### 5.3 前端组件树

```
App.vue
└── MainLayout.vue
    ├── Sidebar (可折叠 ~280px)
    │   ├── DaemonStatus → 连接状态指示器
    │   ├── NavMenu → 页面导航
    │   └── AgentList → Agent 列表（chat 页）
    │
    └── RouterView (主内容区)
        ├── PackagePage (/packages/:name)
        │   ├── FileTree (左) → 文件树浏览器
        │   ├── CodeEditor (中) → CodeMirror 编辑器
        │   └── WorkflowDAG (弹出) → Vue-Flow DAG 可视化
        │
        └── ChatPage (/chat)
            ├── AgentList (左) → Agent 选择 + 管理
            ├── SessionList (左) → Session 选择 + 管理
            └── ChatView (右) → 对话气泡 + 输入框
```

---

## 6. 流程设计

### 6.1 聊天消息流

```
用户输入消息
    │
    ▼ ChatPage.vue
  chatStore.send(content)
    │
    ▼ useChatWebSocket composable
  WebSocket.send({content, session_id})
    │
    ▼ bridge /ws/chat/{agent_id}
  FastAPI WebSocket 端点
    │
    ├─ daemon_client.execute(agent_id, content)
    │     → JSON-RPC: agent.execute({user_id, client_id, content})
    │
    ├─ 循环 poll_response():
    │     → JSON-RPC: agent.poll_response(...)
    │
    └─ 逐块推送到前端 WebSocket
          → {"type": "chunk", "content": "..."}   (流式)
          → {"type": "done", "final": true}       (完成)
    │
    ▼ chatStore
  追加到 messages 数组
    │
    ▼ ChatView.vue
  Vue 响应式更新对话气泡
```

### 6.2 编辑-打包-加载流程

```
用户打开 PackagePage
    │
    ▼ packageStore.loadPackages()
  GET /api/packages → 列出可用 package
    │
    ▼ 用户点击某个 package
  packageStore.loadFileTree(name) → 显示文件树
    │
    ▼ 用户双击 agent.json
  packageStore.readFile(name, "agent.json") → CodeEditor 显示内容
    │
    ▼ 用户编辑并保存
  packageStore.writeFile(name, "agent.json", content) → PUT API
    │
    ▼ 用户双击 workflow.json
  弹出 WorkflowDAG 模态框 → 解析 workflow.nodes/edges → Vue-Flow 渲染
    │
    ▼ 用户点击 "Package"
  POST /api/packages/{name}/package → 调用 agent_build.sh
    │
    ▼ 用户点击 "Load to Daemon"
  POST /api/agents/{id}/load → daemon agent.load
    │
    ▼ 用户切换到 Chat 页
  Agent 出现在 AgentList 中 → 可开始对话
```

### 6.3 Daemon 断连处理

```
daemonStore.checkHealth()（每 15s 轮询 /api/daemon/status）
    │
    ├─ Daemon 在线 → 绿色指示器 + 操作正常
    │
    └─ Daemon 断开 → 红色 banner "Daemon disconnected"
         │
         ├─ 文件编辑操作（bridge 直读文件系统）→ 不受影响
         └─ 聊天/Agent 管理 → 按钮禁用 + 提示
              │
              └─ Daemon 恢复 → 绿色 banner "Daemon reconnected"（3s 自动消失）
```

---

## 7. 实现方案

### 7.1 项目结构

```
bridge/                            ← Python FastAPI 桥接服务
├── main.py                       ← FastAPI app 入口 + CORS
├── config.py                     ← 配置（DAEMON_HOST/PORT, PACKAGES_DIR）
├── requirements.txt              ← Python 依赖
├── clients/
│   └── daemon_client.py          ← JSON-RPC over TCP 客户端
├── routes/
│   ├── __init__.py
│   ├── packages.py               ← 文件系统 CRUD API
│   ├── agents.py                 ← Agent 代理 API
│   ├── sessions.py               ← Session 代理 API
│   ├── chat.py                   ← WebSocket 聊天端点
│   └── daemon.py                 ← Daemon 状态/健康检查
├── monitor.py                    ← 后台 Daemon 连接监控
└── tests/
    └── test_bridge.py

web/                               ← Vue 3 前端
├── package.json
├── vite.config.ts                 ← Vite 配置（含 proxy）
├── tsconfig.json
├── playwright.config.ts
├── src/
│   ├── main.ts
│   ├── App.vue
│   ├── router/
│   │   └── index.ts              ← 路由定义
│   ├── layouts/
│   │   └── MainLayout.vue        ← 全局布局
│   ├── pages/
│   │   ├── PackagePage.vue       ← Package 编辑页
│   │   └── ChatPage.vue          ← 聊天页
│   ├── components/
│   │   ├── FileTree.vue          ← 文件树浏览器
│   │   ├── CodeEditor.vue        ← CodeMirror 编辑器
│   │   ├── WorkflowDAG.vue       ← Vue-Flow 可视化
│   │   ├── AgentList.vue         ← Agent 列表
│   │   ├── SessionList.vue       ← Session 列表
│   │   ├── ChatView.vue          ← 聊天界面
│   │   └── DaemonStatus.vue      ← Daemon 连接状态
│   ├── stores/
│   │   ├── agentStore.ts         ← Agent 状态
│   │   ├── sessionStore.ts       ← Session 状态
│   │   ├── packageStore.ts       ← Package 状态
│   │   ├── chatStore.ts          ← 聊天状态
│   │   └── daemonStore.ts        ← Daemon 连接状态
│   ├── composables/
│   │   └── useChatWebSocket.ts   ← WebSocket 连接管理
│   └── utils/
│       └── workflow-parser.ts    ← Workflow JSON → Vue-Flow 数据转换
└── e2e/
    ├── package-flow.spec.ts
    ├── chat-flow.spec.ts
    └── daemon-status.spec.ts

verify.sh                          ← 端到端验收脚本
```

### 7.2 关键文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `bridge/main.py` | 新建 | FastAPI app 入口 |
| `bridge/config.py` | 新建 | 配置管理 |
| `bridge/clients/daemon_client.py` | 新建 | JSON-RPC over TCP 客户端 |
| `bridge/routes/packages.py` | 新建 | 文件系统 CRUD API |
| `bridge/routes/agents.py` | 新建 | Agent API 代理 |
| `bridge/routes/sessions.py` | 新建 | Session API 代理 |
| `bridge/routes/chat.py` | 新建 | WebSocket 聊天端点 |
| `bridge/routes/daemon.py` | 新建 | 健康检查路由 |
| `bridge/monitor.py` | 新建 | Daemon 连接监控 |
| `web/src/App.vue` | 新建 | Vue 根组件 |
| `web/src/layouts/MainLayout.vue` | 新建 | 全局布局 |
| `web/src/router/index.ts` | 新建 | 路由配置 |
| `web/src/pages/PackagePage.vue` | 新建 | Package 编辑页 |
| `web/src/pages/ChatPage.vue` | 新建 | 聊天页 |
| `web/src/components/FileTree.vue` | 新建 | 文件树 |
| `web/src/components/CodeEditor.vue` | 新建 | 代码编辑器 |
| `web/src/components/WorkflowDAG.vue` | 新建 | DAG 可视化 |
| `web/src/components/AgentList.vue` | 新建 | Agent 列表 |
| `web/src/components/SessionList.vue` | 新建 | Session 列表 |
| `web/src/components/ChatView.vue` | 新建 | 聊天界面 |
| `web/src/components/DaemonStatus.vue` | 新建 | 连接状态 |
| `web/src/stores/*.ts` | 新建 | 5 个 Pinia store |
| `web/src/composables/useChatWebSocket.ts` | 新建 | WebSocket composable |
| `web/src/utils/workflow-parser.ts` | 新建 | Workflow JSON 解析 |
| `verify.sh` | 新建 | 验收脚本 |

**不修改的文件**：
- `src/daemon/` — daemon 核心不变
- `publish/agent/` — Agent package 格式不变
- `examples/agent/` — 已有示例不变

### 7.3 功能点实现优先级

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | Bridge 脚手架 + 基础路由 | 无副作用 | pytest 测试 bridge 启动和路由响应 |
| P1 | 前端脚手架 + 布局 + 路由 | 无副作用 | 页面可访问、路由跳转正常 |
| P1 | 文件树浏览 + 代码编辑 | 无副作用 | 文件可读写，路径穿越被拒绝 |
| P2 | Workflow DAG 可视化 | 无副作用 | 双击 workflow.json 显示 DAG |
| P2 | Agent/Session 列表管理 | 依赖 daemon | 调用 daemon API 返回正确结果 |
| P2 | Agent 问答聊天 | 依赖 daemon | WebSocket 连接 → 发送 → 收到回复 |
| P3 | 打包 + 加载完整流程 | 依赖 agent_build.sh | 编辑 → 保存 → 打包 → 加载 → 聊天 |
| P3 | Daemon 断连处理 | 依赖 daemon | 断连 UI 变化 / 重连恢复 |
| P3 | Vitest + Playwright 自动化测试 | 无副作用 | 所有测试 PASS |

---

## 8. 测试方案

### 8.1 测试分层

| 层 | 工具 | 覆盖范围 | 数量目标 |
|----|------|---------|---------|
| 单元测试 (前端) | Vitest | Store, composables, utils | ≥20 个 |
| 单元测试 (后端) | pytest | Bridge 路由、daemon client mock | ≥12 个 |
| E2E (前端) | Playwright | 页面加载、交互、完整流程 | ~5 个 spec |

### 8.2 E2E 测试场景

| TC | 场景 | 步骤 | 预期 |
|----|------|------|------|
| 1 | Package 编辑 | 打开 package → 编辑 agent.json → 保存 | 文件保存成功 |
| 2 | DAG 可视化 | 双击 workflow.json | 显示 DAG 图 |
| 3 | Agent 列表 | Chat 页显示 agent 列表 | 加载的 agent 可见 |
| 4 | Agent 聊天 | 选择 agent → 发送消息 → 接收回复 | 对话正常 |
| 5 | Daemon 断连 | 停止 daemon → 检查 UI | 红色 banner 显示 |

### 8.3 独立性要求

- 前端测试使用 mock WebSocket 和 mock API 调用，不依赖 daemon 实时运行
- Bridge 测试使用 mock daemon TCP 响应
- E2E 测试依赖 daemon + bridge + frontend 全部运行

---

## 9. 文档

### 9.1 需要变更的文档

| 文档 | 变更说明 |
|------|---------|
| `README.md` | 在功能列表中增加 "Vue 前端" 条目 |
| `docs/enhancements/README.md` | 增加 039 索引条目 |

### 9.2 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [036-feature-agentflowd-可执行入口设计](036-feature-agentflowd-可执行入口设计-260423-implemented.md) | agentflowd daemon 是本前端聊天功能的 RPC 目标 |
| [037-feature-skills-dynamic-upload](037-feature-skills-dynamic-upload.md) | Skills 上传接口是前端 "Package 编辑"的扩展能力来源 |
| [038-feature-harness-agent-能力升级](038-feature-harness-agent-能力升级-260621-tbd.md) | Harness Agent 升级后, 前端聊天对话可展示更丰富的 agent 行为 |

---

## 10. 进展跟踪

- [ ] Bridge 项目脚手架 + 基础配置
- [ ] Web 前端项目脚手架（Vite + Vue 3 + Element Plus + Pinia + Vue-Flow + CodeMirror）
- [ ] 共享类型定义（TypeScript + pydantic）
- [ ] Bridge → Daemon JSON-RPC 客户端
- [ ] Bridge 文件系统 CRUD API
- [ ] Bridge Agent/Session 代理 API
- [ ] Bridge WebSocket 聊天端点
- [ ] Bridge 打包 API
- [ ] 前端全局布局 + 路由 + Store
- [ ] Daemon 连接状态管理 + 健康检查
- [ ] Package 文件树浏览器
- [ ] 代码编辑器组件（CodeMirror）
- [ ] Workflow DAG 可视化（Vue-Flow）
- [ ] Agent 列表 + 管理
- [ ] Session 列表 + 管理
- [ ] 聊天界面组件
- [ ] 聊天 WebSocket 客户端逻辑
- [ ] 聊天完整集成（Agent + Session + Chat）
- [ ] 打包 + 加载完整流程
- [ ] Daemon 断连处理 + 重连
- [ ] Vitest 单元测试
- [ ] Playwright E2E 测试
- [ ] 集成验收脚本 verify.sh
- [ ] 验证通过

### 详细进度

| 模块 | 功能 | 用例 | 文档 | 状态 |
|------|------|------|------|------|
| Bridge 基础 | ✅ 脚手架 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| Bridge 文件 API | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| Bridge 代理 API | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| Bridge WS 聊天 | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| 前端布局/路由 | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| 文件树/编辑器 | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| DAG 可视化 | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| Agent/Session 管理 | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| 聊天界面 | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| 打包/加载 | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |
| 测试 | ⏳ 待实施 | ⏳ 待实施 | ⏳ 待实施 | 待开始 |

---

## 11. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-06-22 | Prometheus | 初始版本 |
