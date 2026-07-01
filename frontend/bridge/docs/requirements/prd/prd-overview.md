# Bridge — 产品需求概述

> **层级**: L1 · `docs/requirements/prd/`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What（解决什么问题）

Bridge 是 AgentFlow 体系中的一个轻量级 HTTP + WebSocket 中间层，位于 Vue 前端与 `agentflowd` 守护进程之间。它需要解决：

- 前端无法直接访问本地 daemon WebSocket/JSON-RPC 端口的问题
- 大前端（Vue）需要 RESTful API 与流式 WebSocket 两种交互模式
- Agent Package 文件的读取、编辑、打包等文件操作需要由服务端代理执行
- Daemon 连接状态的透明监控与故障自愈

## Why（为什么需要）

- **架构约束**: 前后端分离架构下，浏览器无法建立到 daemon 的原始 WebSocket 连接
- **协议适配**: 前端 `daemonStore` 需要一个稳定的 HTTP 端点来轮询健康状态
- **双模式支持**: 流式聊天需要 WebSocket 长连接，但简单查询只需要 REST 请求-响应
- **文件操作代理**: Package 管理需要服务端文件系统操作，不能交给浏览器
- **故障隔离**: Daemon 离线时监控和管理操作不应完全不可用

## How（如何实现 — 概要）

Bridge 是一个独立的 FastAPI 应用，提供：

| 能力 | 协议 | 端点 |
|------|------|------|
| 健康检查 | HTTP GET | `/health`、`/api/health` |
| Agent 代理 | HTTP REST | `/api/agents/*` |
| 聊天（流式） | WebSocket | `/ws/chat/{agent_id}` |
| 聊天（同步） | HTTP POST | `/api/chat/{agent_id}` |
| Daemon 监控 | HTTP REST | `/api/daemon/*` |
| Package 管理 | HTTP REST | `/api/packages/*` |
| 会话管理 | HTTP REST | `/api/sessions/*` |

## In Scope

- 为 Vue 前端提供 HTTP REST API（Agent 管理、会话管理、Package 管理、Daemon 监控）
- 为 Vue 前端提供 WebSocket 流式聊天
- 为程序化调用提供 REST 同步聊天
- Agent Package 文件 CRUD（读取、写入、重命名、删除、目录操作）
- Agent Package 构建与分发（`.tar.gz` 打包）
- Agent Package 上传与解压（`.zip` / `.tar.gz` / `.tgz`）
- Daemon 连接状态缓存与自动重连
- CORS 配置（本地开发默认允许 `:15173`）

## Out of Scope

- 不维护自己的会话状态（会话由 daemon 管理）
- 不缓存 agent/workflow 元数据（纯透传代理）
- 不提供认证/授权（当前默认同一内网信任）
- 不提供持久化存储
- 不提供 Agent 创建/注册能力
- 不提供文件搜索/全文搜索
- 不提供二进制文件编辑

## 成功指标

| 指标 | 目标值 |
|------|--------|
| Bridge 在 daemon 离线时正常启动 | ✅ 不阻塞启动 |
| `/api/daemon/status` 响应时间 | < 10ms（缓存隔离，无 daemon 请求） |
| WebSocket 聊天端到端延迟（同机） | < 500ms |
| REST 聊天端到端延迟（同机） | 与 daemon 工作流执行时间一致 |
| Package CRUD 操作响应 | < 200ms（本地文件系统） |
| daemon 缓存滞后 | ≤ 10s（默认刷新间隔） |
| 服务可用性 | daemon 故障不影响 HTTP 路由可用性 |

---

**相关设计文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体设计
- `docs/design/features/agent-management.md` — Agent 管理
- `docs/design/features/chat.md` — 聊天功能
- `docs/design/features/daemon-monitoring.md` — Daemon 监控
- `docs/design/features/package-management.md` — Package 管理
- `docs/design/features/session-management.md` — 会话管理
