# 会话管理功能设计

> **层级**: L2 · `docs/design/features/session-management.md`
> **状态**: 已更新 · 2026-06-26 重构: session_id 为 daemon 内部概念, 前端以 client_id 为会话标识
> **创建者**: `design-doc-organizer`

---

## What

会话管理模块提供对 daemon 侧 `session.*` RPC 的 HTTP 代理端点。会话由 daemon 的 Agent 拥有，Bridge 不维护独立会话表、不缓存、不做业务逻辑。

**核心变更**:
- `session_id` 是 daemon 内部概念，外部不感知
- 前端通过 `user_id` + `client_id` 标识身份
- 前端会话列表以 `client_id` 作为会话的外部标识

## Why

- 前端需要查看 Agent 的活跃会话列表（以 `client_id` 展示）
- 需要清理/销毁过期会话
- 需要清除特定会话的对话记录（dialogue）
- Session 由 daemon 全权管理，Bridge 只需透传
- 将 `session_id` 隐藏在前端之外，降低耦合

## How

### 前端会话模型

- **`user_id`**: 前端内置固定值（如 `"web-user"`），表示当前用户
- **`client_id`**: 每个会话的唯一外部标识，新建会话时生成一个 UUID
- 会话列表中展示的是 `<user_id, client_id>` 对，`session_id` 仅用于后端销毁/清理操作
- 用户必须先选择（或创建）一个会话才能开始聊天

```
前端会话管理流程:

1. 页面加载 → 调用 GET /api/sessions?agent_id=X（不传 user_id，返回该 agent 全部会话）
                                  → daemon session.list → 返回活跃会话列表
2. 用户点击"新建会话" → 弹出对话框，填写 client_id 和 user_id（可选，有默认值）→ 选中 → 启用聊天
3. 用户选择已有会话 → 选中 → 启用聊天
4. 用户点击"销毁" → DELETE /api/sessions/{session_id}?agent_id=X
                                     → daemon session.destroy
5. 用户点击"清除对话" → DELETE /api/sessions/{session_id}/dialogue?agent_id=X
```

### 功能流程

会话管理模块与 Agent 管理类似，是纯 HTTP-to-JSON-RPC 代理：

```
前端                     Bridge                  agentflowd (daemon)
  │                        │                          │
  │ GET /api/sessions?     │                          │
  │   agent_id=harness&    │                          │
  │   user_id=web-user     │                          │
  │───────────────────────→│                          │
  │                        │── session.list ─────────→│
  │                        │←── session list ────────│
  │←─ 200 {data: [...]}   │                          │
  │                        │                          │
  │ DELETE /api/sessions/  │                          │
  │   sess_abc?agent_id=X  │                          │
  │───────────────────────→│                          │
  │                        │── session.destroy ──────→│
  │                        │←── destroy result ──────│
  │←─ 200 {data: {...}}   │                          │
```

1. 前端通过 HTTP 请求操作 Session（列表、销毁、清除对话）
2. 所有请求需要携带 `agent_id` 查询参数（会话由 Agent 拥有）
3. 列表请求可附带 `user_id` 参数过滤指定用户的会话
4. Bridge 将 HTTP 请求转换为对应的 daemon `session.*` RPC 调用
5. Daemon 返回结果，Bridge 包装为标准格式返回前端

### 关键设计点

- **会话归属**: 所有会话由 daemon 侧的 Agent 管理，Bridge 不维护独立会话表
- **无缓存**: 每次 Session 请求都经过 daemon，不做缓存
- **错误映射**: 与 Agent 管理模块完全一致的 daemon 错误 → HTTP 状态码映射
- **清除对话**: 支持清除特定 `dialogue_id` 的对话记录；`dialogue_id` 为空时清除该 Session 下所有对话
- **agent_id 必填**: 所有端点都必须携带父 Agent 标识符作为查询参数

### 端点一览

| 方法 | 路径 | 对应 RPC | 必填参数 |
|------|------|----------|----------|
| GET | `/api/sessions` | `session.list` | `agent_id` |
| DELETE | `/api/sessions/{id}` | `session.destroy` | `agent_id` |
| DELETE | `/api/sessions/{id}/dialogue` | `session.clear_dialogue` | `agent_id`, `dialogue_id`（可选） |

### Session 与 Chat 的关系

- 聊天端点（WebSocket 和 REST）通过 `user_id` + `client_id` 标识身份，不再传递 `session_id`
- `agent.execute` 首次调用时 daemon 按 `user_id:client_id` 复合键自动创建 session
- 前端聊天时使用已选择的会话的 `client_id`，`user_id` 使用内置固定值
- 清除对话（`clear_dialogue`）不会销毁 session 本身，只是清空其对话记录

### 不支持项

| 场景 | 状态 | 说明 |
|------|------|------|
| 会话创建 | ❌ 不支持 | 会话由 daemon 在 `agent.execute` 时自动创建 |
| 会话缓存 | ❌ 不支持 | 每次请求都经过 daemon |
| 会话过期自动清理 | ❌ 不支持 | 由 daemon 侧管理 |
| 前端感知 session_id | ❌ 不支持 | `session_id` 是 daemon 内部概念 |

---

**接口契约（端点路径、查询参数、响应格式）请参见 `architecture/modules/session-api.md`**

**相关设计文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/features/agent-management.md` — Agent 管理（会话隶属于 Agent）
- `docs/design/features/chat.md` — 聊天功能（会话 ID 串联多轮对话）
- `docs/design/architecture/modules/session-api.md` — Session API 接口契约（L4）
