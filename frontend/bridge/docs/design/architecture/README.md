# Bridge 架构详细设计

> **层级**: L3/L4 · `docs/design/architecture/`
> **创建者**: `design-doc-organizer`
> **更新日期**: 2026-06-25
> **状态**: 已填充

---

## 范围

本目录存放 Bridge 的架构总览与技术设计：

| 子目录 | 层级 | 内容 |
|--------|------|------|
| `architecture/` 根目录 | L3 | 系统架构总览、模块边界、技术选型 |
| `architecture/modules/` | L4 | 模块内部设计、API/Schema、算法、接口契约 |

## 文档索引

| 文档 | 层级 | 说明 | 状态 |
|------|------|------|------|
| [Agent API 契约](modules/agent-api.md) | L4 | Agent 管理 REST 端点、请求/响应 Schema、错误映射 | ✅ 已填充 |
| [Chat API 契约](modules/chat-api.md) | L4 | WebSocket + REST 聊天接口帧格式、消息类型、错误码 | ✅ 已填充 |
| [Daemon API 契约](modules/daemon-api.md) | L4 | 监控缓存数据结构、端点响应格式、刷新机制 | ✅ 已填充 |
| [Package API 契约](modules/package-api.md) | L4 | 包管理 CRUD + 构建/上传 API、安全校验规则、响应模型 | ✅ 已填充 |
| [Session API 契约](modules/session-api.md) | L4 | 会话管理 REST 端点、参数映射、错误处理 | ✅ 已填充 |
| [DaemonClient 协议](modules/daemon-client-protocol.md) | L4 | JSON-RPC 2.0 协议、连接生命周期、请求分派模型、通知消费 | ✅ 已填充 |

## 当前关键架构要点

- **框架**: FastAPI（异步 ASGI）
- **Daemon 通信**: WebSocket + JSON-RPC 2.0，单个长连接共享所有请求
- **通知消费**: Daemon 推送的 `agent.response` 通过 `_notification_queue` 队列消费
- **配置**: Pydantic Settings，环境变量驱动
- **部署**: `agentflow-bridge` CLI 入口 → uvicorn
- **CORS**: 默认允许 `localhost:15173` 和 `127.0.0.1:15173`
- **错误传播**: `DaemonError` → HTTP 状态码（400/404/502/504/500）

---

**相关文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体设计（L2）
- `docs/design/adrs/README.md` — 架构决策记录
