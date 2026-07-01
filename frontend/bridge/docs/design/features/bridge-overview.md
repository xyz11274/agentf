# Bridge 总体架构设计

> **层级**: L2 · `docs/design/features/bridge-overview.md`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What

Bridge 是一个独立的 FastAPI Web 服务，作为 AgentFlow Vue 前端与 `agentflowd` 本地守护进程之间的 HTTP + WebSocket 中间层。它对外暴露 RESTful API 和 WebSocket 端点，对内通过 JSON-RPC over WebSocket 与 daemon 通信。

## Why

- **浏览器限制**: Vue 前端（浏览器环境）无法直接建立到 daemon WS 端口的原始 WebSocket 连接
- **协议转换**: 需要将前端 HTTP REST 请求转换为 daemon 的 JSON-RPC 调用
- **文件操作代理**: Package 管理需要服务端文件系统权限
- **故障隔离**: Daemon 离线时 Bridge 仍应正常提供 HTTP 服务

## How

### 系统上下文（场景视图）

```
┌────────────────────────────────────────────────────────────────────┐
│                         AgentFlow 系统边界                          │
│                                                                    │
│  ┌──────────┐  HTTP REST     ┌──────────┐  JSON-RPC  ┌──────────┐ │
│  │ Vue 前端  │──────────────→│  Bridge   │───────────→│agentflowd│ │
│  │(:15173)   │  WebSocket    │ (:8199)   │ WS 长连接   │(:17891)  │ │
│  │           │←──────────────│           │←───────────│          │ │
│  └──────────┘               └─────┬─────┘            └──────────┘ │
│                                   │                                │
│                                   │ 直接读取                       │
│                                   ▼                                │
│                          ┌────────────────┐                       │
│                          │ Agent Package   │                       │
│                          │ 文件系统         │                       │
│                          └────────────────┘                       │
└────────────────────────────────────────────────────────────────────┘
```

### 逻辑视图（职责分解）

```
┌─────────────────────────────────────────────────────────────┐
│  Bridge 应用                                                  │
│                                                              │
│  ┌─────────────┐  ┌─────────────┐  ┌────────────────────┐   │
│  │ HTTP 路由层   │  │ WebSocket   │  │ 文件系统操作层      │   │
│  │             │  │ 路由层       │  │                    │   │
│  │ agents.py  │  │ chat.py     │  │ packages.py        │   │
│  │ sessions.py│  │             │  │ (CRUD + 构建 + 上传) │   │
│  │ daemon.py  │  │             │  └────────────────────┘   │
│  │ chat_rest..│  │             │                            │
│  └──────┬──────┘  └──────┬──────┘                            │
│         │                │                                    │
│         └────────┬───────┘                                    │
│                  ▼                                            │
│  ┌─────────────────────────────┐                              │
│  │ DaemonClient（通信抽象层）    │                              │
│  │ send_request() → JSON-RPC   │                              │
│  │ wait_for_response() → notify│                              │
│  └─────────────┬───────────────┘                              │
│                │                                              │
│         WebSocket (ws://localhost:17891)                       │
└────────────────┼──────────────────────────────────────────────┘
                 │
          agentflowd (daemon)
```

**模块职责**：

| 模块 | 职责 | 通信方式 |
|------|------|----------|
| `main.py` | 应用骨架、CORS、lifespan、路由挂载 | — |
| `config.py` | 运行时配置（环境变量 + Pydantic Settings） | — |
| `client.py` | Daemon JSON-RPC 客户端（长连接 + 通知队列） | WebSocket |
| `models.py` | 共享数据模型（Pydantic v2） | — |
| `dependencies.py` | FastAPI 依赖注入（DaemonClient 获取） | — |
| `monitor.py` | Daemon 连接后台监控 | HTTP/WS |
| `routes/agents.py` | Agent 管理代理（list/info/load/unload/pause/resume） | HTTP REST |
| `routes/chat.py` | WebSocket 流式聊天 | WebSocket |
| `routes/chat_rest.py` | REST 同步聊天 | HTTP REST |
| `routes/daemon.py` | Daemon 监控缓存路由 | HTTP REST |
| `routes/packages.py` | Package 文件 CRUD + 构建 + 上传下载 | HTTP REST |
| `routes/sessions.py` | 会话管理代理 | HTTP REST |

### 开发视图（代码组织）

```
frontend/bridge/
├── main.py                    # 应用入口 + lifespan
├── config.py                  # 配置（环境变量驱动）
├── client.py                  # DaemonClient（JSON-RPC over WS）
├── models.py                  # Pydantic 数据模型
├── dependencies.py            # FastAPI 依赖
├── monitor.py                 # Daemon 监控
├── routes/
│   ├── agents.py              # Agent 管理（JSON-RPC 代理）
│   ├── chat.py                # WebSocket 聊天
│   ├── chat_rest.py           # REST 聊天
│   ├── daemon.py              # Daemon 监控缓存
│   ├── packages.py            # Package 文件系统 CRUD
│   └── sessions.py            # 会话管理
├── tests/                     # 测试
├── docs/                      # 设计文档
├── pyproject.toml             # 项目配置
└── requirements.txt           # 依赖
```

**依赖关系**：

```
routes/* → client.py → WebSocket → agentflowd
routes/* → config.py (settings)
routes/agents.py, routes/chat.py, routes/chat_rest.py,
routes/sessions.py → client.py (DaemonClient)
routes/daemon.py → client.py (仅后台刷新)
routes/packages.py → config.py (agent_packages_path) + OS 文件系统
main.py → routes/* + models.py + config.py
client.py → config.py + asyncio
```

### 进程视图（运行时）

```
┌─────────────────────────────────────────────┐
│  Bridge 进程（单进程）                         │
│                                              │
│  asyncio 事件循环                               │
│  ├── FastAPI ASGI 服务器 (uvicorn)             │
│  │   ├── HTTP 请求处理（多个路由并发）            │
│  │   └── WebSocket 连接（长连接）               │
│  ├── DaemonClient                             │
│  │   ├── send_request() — 任意路由调用           │
│  │   ├── _reader_loop — 后台持续读取 daemon 消息  │
│  │   └── _notification_queue — 通知消息队列       │
│  └── _daemon_status_poll_loop — 后台缓存刷新    │
│      └── 每 10s 调用 health + runtime_status    │
└─────────────────────────────────────────────┘
               │
          WS 连接
               │
┌──────────────▼──────────────────────────────┐
│  agentflowd (daemon, 独立进程)               │
│  └── Agent 管理 + 工作流执行 + 会话管理       │
└─────────────────────────────────────────────┘
```

- 单进程 asyncio 事件循环
- HTTP 请求、WebSocket 连接、缓存刷新共享同一事件循环
- `DaemonClient.send_request()` 支持并发（通过递增 request_id + Future）
- `_reader_loop` 是单点——连接断开时所有 pending 请求都会失败

### 物理视图（部署）

```
┌─────────────────────────────────────────┐
│  本地开发环境（同机部署）                    │
│                                          │
│  localhost:15173                         │
│  ┌──────────┐                            │
│  │ Vue 前端  │  (Vite 开发服务器)           │
│  └────┬─────┘                            │
│       │                                  │
│       │ HTTP / WS                        │
│       │                                  │
│  localhost:8199                          │
│  ┌────▼─────┐                            │
│  │  Bridge  │  (uvicorn)                  │
│  └────┬─────┘                            │
│       │                                  │
│       │ WS / JSON-RPC                    │
│       │                                  │
│  localhost:17891                         │
│  ┌────▼──────┐                           │
│  │ agentflowd │                           │
│  └───────────┘                           │
└─────────────────────────────────────────┘
```

- 默认绑定 `0.0.0.0:8199`（可通过环境变量配置）
- CORS 默认允许 `localhost:15173` 和 `127.0.0.1:15173`
- 部署入口：`agentflow-bridge` CLI → uvicorn

### 关键技术选型

| 选型 | 选择 | 理由 |
|------|------|------|
| Web 框架 | FastAPI | ASGI + 自动 OpenAPI + Pydantic v2 集成 |
| Daemon 通信 | WebSocket + JSON-RPC 2.0 | daemon 原生协议，单连接复用 |
| 配置 | Pydantic Settings | 环境变量驱动，类型安全 |
| 部署 | `agentflow-bridge` CLI → uvicorn | 单命令启动，无需额外配置 |

### 外部依赖

| 依赖 | 用途 | 版本约束 |
|------|------|----------|
| `fastapi` | Web 框架 | — |
| `uvicorn` | ASGI 服务器 | — |
| `pydantic` / `pydantic-settings` | 数据模型与配置 | v2 |
| `websockets` | WebSocket 客户端（DaemonClient） | — |
| `httpx` | HTTP 客户端（预留） | — |

---

**相关设计文档索引**：

| 文档 | 层级 | 说明 |
|------|------|------|
| `features/agent-management.md` | L2 | Agent 管理功能设计 |
| `features/chat.md` | L2 | 聊天功能设计 |
| `features/daemon-monitoring.md` | L2 | Daemon 监控功能设计 |
| `features/package-management.md` | L2 | Package 管理功能设计 |
| `features/session-management.md` | L2 | 会话管理功能设计 |
| `architecture/modules/agent-api.md` | L4 | Agent API 接口契约 |
| `architecture/modules/chat-api.md` | L4 | Chat API 接口契约 |
| `architecture/modules/daemon-api.md` | L4 | Daemon 监控 API 契约 |
| `architecture/modules/package-api.md` | L4 | Package API 接口契约 |
| `architecture/modules/session-api.md` | L4 | Session API 接口契约 |
| `architecture/modules/daemon-client-protocol.md` | L4 | DaemonClient 通信协议 |
| `adrs/0001-fastapi-framework.md` | L3 | FastAPI 选型决策 |
| `adrs/0002-single-websocket-connection.md` | L3 | 单连接决策 |
| `adrs/0003-daemon-status-cache.md` | L3 | 状态缓存策略 |
