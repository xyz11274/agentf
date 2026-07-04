# Session / Dialogue 映射关系与生命周期

> 本文档描述 `user_id + client_id`、`session_id`、`dialogue_id` 三层概念的映射关系、生命周期管理，以及各层的责任边界。

## 1. 三层映射链

```
┌─────────────────────────────────────────────────────────────────┐
│                       三层映射链                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  user_id + client_id  ──→  route_key  ──→  session_id  ──→  dialogue_id  │
│      (前端会话标识)         路由键          框架内部会话         每次请求的对话        │
│      user_id: "alice"      "alice:web"    UUID / 池分配        每请求唯一           │
│      client_id: "web"                    长生命周期            短生命周期          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 1.1 user_id + client_id

| 属性 | 说明 |
|------|------|
| 生成者 | 前端 |
| 生命周期 | **永久** — 前端持久维护 |
| 唯一性 | `(user_id, client_id)` 对唯一标识一个用户会话 |
| 作用 | 前端会话列表的 key；提供给 daemon 用于路由 |

前端维护一个以 `(user_id, client_id)` 为 key 的会话列表，不因后端 session/dialogue 的生命周期而清除。

### 1.2 route_key = "user_id:client_id"

| 属性 | 说明 |
|------|------|
| 生成者 | `SessionRouter::handle_route_message()` |
| 生命周期 | session 活跃期间 (`aging_loop` 超时后释放) |
| 存储 | `active_routes_` map |
| 映射关系 | 1:1 映射到 `session_id` |

### 1.3 session_id

| 属性 | 说明 |
|------|------|
| 生成者 | SessionRouter（池分配或 UUID 工厂） |
| 生命周期 | 创建 → 活跃 → **超时后 reset_for_pool** |
| 默认超时 | **30s** → 已调大为 **5min** (`session.h:timeout_ms=300000`) |
| 存储位置 | `session_pool_` / `physical_sessions_` |
| 数据隔离 | session 级别的 DataBus key 前缀 `/session/{session_id}/...` |

session 是 **route_key 的后端承载者**，所有会话状态（DataBus 数据、对话历史）都挂载在 session 下。

### 1.4 dialogue_id

| 属性 | 说明 |
|------|------|
| 生成者 | **Bridge**（`chat_rest.py` / `chat.py`）使用 `datetime + uuid` |
| 生命周期 | 用户请求 → coordinator final response → **cleanup_dialogue_data** |
| 存储 | DataBus 的 `/session/{sid}/sys/{dialogue_id}/...` 命名空间 |
| 销毁 | `DIALOGUE_DESTROYED` 事件 + `cleanup_dialogue_data`（清除 DataBus key） |

> **注意**：`dialogue_id` 必须在桥接层生成并传入 daemon。daemon 的 fallback 逻辑（`hash(user_text + message_id)`）仅用于没有传入的兼容场景，不应依赖。

---

## 2. 生命周期详解

### 2.1 Session 生命周期

```
  用户请求
    │
    ├─ get_or_create_session("alice", "web")
    │   ├─ active_routes_ 命中 → 复用 session
    │   └─ 未命中 → allocate_session_for_route()
    │       ├─ 池中有空闲 → 取池中 session
    │       └─ 池中无空闲 → factory 创建新 uuid session
    │
    ├─ session->bind_route("alice", "web")
    │
    ├─ [对话执行...]
    │
    ├─ aging_loop 每 5s 检查
    │   └─ session->is_timeout_expired() (5min 无活动)
    │       └─ release_route_locked(route_key)
    │           ├─ session_to_route_.erase(session_id)
    │           ├─ session->reset_for_pool()  ← 清除路由绑定、清空运行时任务
    │           └─ active_routes_.erase(route_key)
    │
    └─ 新请求
        └─ get_or_create_session 未命中
            └─ 分配新 session（池中或 factory 创建）
```

### 2.2 Dialogue 生命周期

```
  Bridge 生成 dialogue_id (datetime+uuid)
    │
    ├─ handle_routed_request_message()
    │   ├─ publish_data(dialogue_id + "/dialogue_id", ...)
    │   ├─ publish_data(dialogue_id + "/user_prompt", ...)
    │   └─ dialogue_event_callback(DIALOGUE_CREATED)
    │
    ├─ [多轮 skill 执行...]
    │
    ├─ coordinator 决定 final response
    │
    ├─ COORDINATORDataExchange
    │   ├─ enqueue_response(final=true)
    │   ├─ dialogue_event_callback(DIALOGUE_DESTROYED)
    │   └─ cleanup_dialogue_data()
    │       ├─ remove_framework_data(response, next_skills, ...)
    │       ├─ remove_user_data(skill_result, ...)
    │       └─ DataBus key 全部清除
    │
    └─ Bridge conversation store
        └─ save(agent_id, user_id, client_id, dialogue_id, content)
            └─ 内存存储，GET /api/conversations/{agent_id} 可查
```

---

## 3. 各层责任边界

| 层 | 负责 | 不负责 |
|----|------|--------|
| **前端** | 维护 `(user_id, client_id)` 会话列表；调用 `GET /api/conversations` 获取历史 | 管理 session_id、dialogue_id |
| **Bridge** | 生成 `dialogue_id`；透传 `user_id + client_id` 到 daemon；保存对话到 `conversation store` | 管理 daemon 内部 session |
| **Daemon** | 维护 `route_key → session_id` 映射；生成 `session_id`；执行对话 | 持久化对话历史（数据在 dialogue cleanup 时清除） |
| **Conversation Store** | 按 `(agent_id, user_id, client_id)` 索引持久化已完成对话 | 管理 daemon 内部状态 |

---

## 4. 关键设计决策

### 4.1 为什么前端用 (user_id, client_id) 而非 session_id

- `session_id` 是 daemon 内部标识，会因超时释放而重置
- `(user_id, client_id)` 由前端控制，**不会因后端超时而丢失**
- 前端以 `(user_id, client_id)` 为 key 维护会话列表，每次请求传入相同的 `user_id` + `client_id` 即可复用同一会话

### 4.2 为什么 dialogue_id 必须在 Bridge 生成

- daemon 的 fallback `hash(user_text + message_id)` 在相同文本下可能碰撞
- Bridge 生成的 `datetime + uuid` 保证全局唯一
- 统一的 `dialogue_id` 让 bridge 的 `conversation store` 和 daemon 的 DataBus 使用同一 ID

### 4.3 session 超时为什么调大到 5 分钟

默认 30 秒超时会导致：
- 用户阅读回复超过 30 秒 → session 被释放
- 下一条消息创建新 session → 对话上下文丢失
- `conversation store` 虽然保存了历史文本，但 daemon 内部的上下文（context callback 结果、DataBus 数据）已丢失

5 分钟覆盖多数真实使用场景，同时 `aging_loop` 仍能清理不再活动的 session 防止内存泄漏。

---

## 5. API 参考

### Conversation Store API

```
GET  /api/conversations/{agent_id}?user_id=...&client_id=...&limit=20
  → 列举用户的最近对话（按 `(user_id, client_id)` 索引）

GET  /api/conversations/{agent_id}/{dialogue_id}
  → 获取单条对话完整内容
```

### 前端使用模式

```javascript
// 1. 用户发送消息
fetch(`/api/chat/${agentId}`, {
  body: JSON.stringify({ user_id, client_id, content: msg }),
})

// 2. SSE 流接收 chunk
// event: message  data: {"content":"...", "final":false}
// event: done     data: {"final":true}

// 3. 完成后，对话已持久化，可重新加载
fetch(`/api/conversations/${agentId}?user_id=${userId}&client_id=${clientId}`)
  .then(r => r.json())
  .then(data => displayHistory(data.conversations))
```

---

## 6. 未来改进

| 改进 | 说明 |
|------|------|
| Conversation Store 持久化到磁盘 | 当前为内存存储，重启后丢失 |
| 多 dialogue 在同一个 session 中的隔离 | 当前 dialogue cleanup 后 session 仍可复用，但 DataBus 数据已被清除 |
| WebSocket 断线重连 | 前端重连时传入相同的 `(user_id, client_id)` 即可恢复到同一个 session |
