# Session API 接口契约

> **层级**: L4 · `architecture/modules/session-api.md`
> **状态**: 已更新 · 2026-06-26 重构: `session_id` 为 daemon 内部概念, 外部通过 `user_id` + `client_id` 标识
> **创建者**: `design-doc-organizer`

---

## What

本文档定义会话管理模块的 API 接口契约，包含端点路径、查询参数、请求/响应体格式和错误映射。

所有端点均为纯 HTTP-to-JSON-RPC 代理：Bridge 不维护独立会话表、不缓存、不做业务逻辑。会话由 daemon 侧的 Agent 全权管理。

**核心设计**: 外部客户端通过 `user_id` + `client_id` 标识身份。daemon 内部按 `user_id:client_id` 复合键管理 session，`session_id` 是 daemon 内部生成的 UUID，客户端不应感知或依赖。

---

## 1. 全局约定

### 1.1 响应信封

所有端点统一返回：

```json
{
  "success": true,
  "data": { … },
  "error": null
}
```

### 1.2 Daemon 错误 → HTTP 状态码映射

与 Agent API 完全一致（定义见 `architecture/modules/agent-api.md §1.2`）：

| 错误场景 | HTTP |
|----------|------|
| `InvalidParamsError` | 400 Bad Request |
| `NotFoundError` | 404 Not Found |
| `DaemonTimeoutError` | 504 Gateway Timeout |
| `DaemonConnectionError` | 502 Bad Gateway |
| 其他 Daemon 错误 | 500 Internal Server Error |

### 1.3 通用查询参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `agent_id` | string | 是 | 父 Agent 标识符（会话由 Agent 拥有） |

---

## 2. API 端点

### 2.1 列出会话

```
GET /api/sessions?agent_id={agent_id}[&user_id={user_id}]
```

**对应 RPC**: `session.list`

**查询参数**:

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `agent_id` | string | 是 | 目标 Agent 标识符 |
| `user_id` | string | 否 | 过滤指定用户的会话；不传时返回所有会话 |

**响应 `data`**: `SessionInfo` 数组。

```json
[
  {
    "session_id": "sess_abc123",
    "user_id": "web-user",
    "client_id": "client_a1b2c3",
    "age_ms": 45200,
    "status": "active"
  }
]
```

SessionInfo 字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `session_id` | string | daemon 内部会话 ID（前端不应依赖此值，仅供展示） |
| `user_id` | string | 关联用户 ID |
| `client_id` | string | 客户端标识符（前端作为"会话"的外部标识） |
| `age_ms` | int | 会话已存在时间（毫秒） |
| `status` | string | 当前状态（由 daemon 定义） |

---

### 2.2 销毁会话

```
DELETE /api/sessions/{session_id}?agent_id={agent_id}
```

**对应 RPC**: `session.destroy`

**路径参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `session_id` | string | 要销毁的会话 ID（daemon 内部 ID） |

**查询参数**:

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `agent_id` | string | 是 | 父 Agent 标识符 |

**请求体**: 无

**响应 `data`**: daemon 返回的原始 destroy 结果。

---

### 2.3 清除对话记录

```
DELETE /api/sessions/{session_id}/dialogue?agent_id={agent_id}&dialogue_id={dialogue_id}
```

**对应 RPC**: `session.clear_dialogue`

**路径参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `session_id` | string | 目标会话 ID（daemon 内部 ID） |

**查询参数**:

| 参数 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `agent_id` | string | 是 | — | 父 Agent 标识符 |
| `dialogue_id` | string | 否 | `""` | 要清除的对话 ID；空字符串表示清除所有对话 |

**请求体**: 无

**响应 `data`**: daemon 返回的原始 clear_dialogue 结果。

---

## 3. 端点汇总

| 方法 | 路径 | 对应 RPC | 必填查询参数 |
|------|------|----------|-------------|
| GET | `/api/sessions?agent_id=X[&user_id=Y]` | `session.list` | `agent_id` |
| DELETE | `/api/sessions/{id}?agent_id=X` | `session.destroy` | `agent_id` |
| DELETE | `/api/sessions/{id}/dialogue?agent_id=X&dialogue_id=Y` | `session.clear_dialogue` | `agent_id` |

---

## 4. Session 与 Chat 的关系

- 聊天端点（WebSocket 和 REST）通过 `user_id` + `client_id` 标识身份，不再传递 `session_id`
- `session_id` 是 daemon 内部概念，由 daemon 在首次 `agent.execute` 时按 `user_id:client_id` 复合键自动分配/复用
- 清除对话（`clear_dialogue`）不会销毁 session 本身，只是清空其中的对话记录
- 前端会话管理界面以 `client_id` 作为会话的外部标识，`session_id` 仅供展示/调试使用

---

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 端点定义与参数映射 | 本文档 §2 / §3 |
| 错误码映射 | 本文档 §1.2（引用 agent-api.md） |
| 会话与聊天关系 | 本文档 §4 |
| 功能流程 | `features/session-management.md` L2 |
| Pydantic 模型定义 | `frontend/bridge/models.py` |

**相关文档**:
- `docs/design/features/session-management.md` — 会话管理功能设计（L2）
- `docs/design/features/agent-management.md` — Agent 管理（会话隶属于 Agent）
- `docs/design/features/chat.md` — 聊天功能（会话 ID 串联多轮对话）
- `docs/design/architecture/modules/agent-api.md` — 通用错误映射定义
