# 聊天功能设计

> **层级**: L2 · `docs/design/features/chat.md`
> **状态**: 已更新 · 2026-06-26 重构: 用 `user_id` + `client_id` 替换 `session_id`
> **创建者**: `design-doc-organizer`

---

## What

Bridge 提供两种聊天模式：

1. **WebSocket 流式聊天** (`/ws/chat/{agent_id}`) — 长连接，多轮对话，流式返回
2. **REST 同步聊天** (`POST /api/chat/{agent_id}`) — 请求-响应，单轮对话，等完成后返回

两种模式共享同一个底层 `DaemonClient` 和 daemon `agent.execute` RPC。

**关键设计变更**: `session_id` 已从客户端协议中移除。客户端通过 `user_id` + `client_id` 标识身份，daemon 内部按 `user_id:client_id` 复合键自动管理 session 生命周期。

## Why

- **WebSocket 模式**：前端需要流式实时展示 Agent 思考过程，适合交互式对话
- **REST 模式**：程序化调用场景不需要流式能力和持久连接，简单请求-响应即可
- 两种模式从不同的接入需求出发，底层复用相同的 daemon 通信管道
- **移除 session_id 的理由**：`session_id` 是 daemon 内部实现细节，外部客户端不应感知。暴露它增加了前端复杂度，且与 daemon 实际按 `user_id:client_id` 路由的设计不一致。

## How

### 双模式架构

系统上下文：Bridge 同时提供 WS 和 HTTP 两种接入方式，统一通过 `DaemonClient` 与 `agentflowd` 通信。

```
┌──────────┐  WS /api/ws/chat/{id}  ┌──────────┐  JSON-RPC   ┌─────────────┐
│  前端     │───────────────────────→│  Bridge   │────────────→│ agentflowd  │
│ (Vue)     │  HTTP POST /api/chat/  │          │  agent.     │ (daemon)    │
│           │───────────────────────→│ Daemon   │  execute    │             │
└──────────┘                        │ Client   │←────────────│             │
                                    │          │  notify     │             │
                                    └──────────┘             └─────────────┘
```

两种模式的交互流程存在以下区别：

| 维度 | WebSocket 模式 | REST 模式 |
|------|---------------|-----------|
| 连接持久性 | 长连接（多轮复用） | 无状态（每轮独立请求） |
| 流式输出 | 支持（逐帧推送 ChatChunk） | 不支持（等完整返回） |
| 心跳保活 | 支持（ping/pong） | 不需要 |
| 中断本轮 | 支持（stop 信号） | 不支持 |
| 适用客户端 | Vue 前端交互 | 程序化调用、测试 |

### WebSocket 模式 — 协议流程

1. 客户端建立 WebSocket 连接到 `/ws/chat/{agent_id}`
2. 连接建立后，Bridge 确保底层 `DaemonClient` 已连接到 daemon
3. 客户端发送 `ChatRequest` 消息（含 `user_id`、`client_id`、`content`）
4. Bridge 调用 daemon 的 `agent.execute` RPC，传入 `user_id`、`client_id`、`content`
5. Daemon 按 `user_id:client_id` 查找/创建 session，流式返回 `agent.response` notification
6. Bridge 解析为 `ChatChunk` 逐帧转发给客户端
7. Daemon 返回 `final` 标记后，Bridge 发送 `ChatDone` 帧关闭本轮
8. 客户端可继续发送下一轮请求（多轮对话通过 `user_id` + `client_id` 串联）

**关键设计点**：

- 每个 WebSocket 连接绑定一个 `agent_id`
- 多轮对话通过 `user_id` + `client_id` 串联（daemon 内部映射到同一个 session）
- 同一连接同一时刻只能有一个 in-flight turn
- 客户端可发送 `{"type": "stop"}` 提前终止本轮，Bridge 立即回 `ChatDone`
- 服务端每 30s 发送 ping 帧保活；超时默认 120s
- 检测到客户端断开时，in-flight turn 自动取消
- 错误时发送错误帧并关闭连接（code 1011）

**dialogue_id 生成**（防止 coord_forwarded 冲突）：

```
{dialogue_id} = {MMDDHHmmSS}-{uuid4.hex[:12]}
```

### REST 模式 — 协议流程

1. 客户端发送 `POST /api/chat/{agent_id}` 请求（JSON body，含 `user_id`、`client_id`、`content`）
2. Bridge 调用 daemon 的 `agent.execute` RPC，传入 `user_id`、`client_id`、`content`
3. Bridge 进入轮询模式：循环调用 `wait_for_response` 从 notification 队列中消费所有 `agent.response` 通知
4. 逐帧累积 `content` 片段
5. 收到 `final: true` 或总超时（120s）后，拼接完整 `content` 返回给客户端
6. 响应包含 `task_id` 和完整 `content` 文本（不再返回 `session_id`）

**关键设计点**：

- `user_id` 和 `client_id` 必填（daemon 要求）
- `dialogue_id` 可选：省略时 Bridge 自动生成（同上格式）
- 不支持中断——请求发起后必须等 daemon 完成或超时
- 错误通过 HTTP 状态码表达（400/404/502/504/500）

### Daemon 通信抽象

两种模式共享同一底层通信抽象：

```
┌──────────────────────────────────────────────────────────┐
│  DaemonClient                                            │
│  ┌──────────────────┐   ┌─────────────────────────────┐  │
│  │ send_request()    │   │ _notification_queue         │  │
│  │ (JSON-RPC 请求)   │   │ (agent.response 通知队列)    │  │
│  └──────┬───────────┘   └──────────┬──────────────────┘  │
│         │                          │                      │
│         │     ┌──────────────┐     │                      │
│         └────→│_dispatch_frame│←────┘                      │
│               └──────┬───────┘                             │
│                      │                                     │
│              WS 连接 / reader_loop                          │
└──────────────────────┼─────────────────────────────────────┘
                       │
                agentflowd (daemon)
```

- `send_request()` 用于所有 JSON-RPC 请求（包括 `agent.execute`）
- `wait_for_response()` 从 `_notification_queue` 消费 daemon 推送的 `agent.response` 通知
- `agent.poll_response` RPC 作为备选轮询方式保留，当前未使用

### 时序图

> 时序图委托 `mermaid-drawer` 绘制。应包含两个独立时序：
>
> **WebSocket 模式**：
> 1. 前端→Bridge: WS connect
> 2. 前端→Bridge: {user_id, client_id, content}
> 3. Bridge→Daemon: agent.execute(user_id, client_id, content)
> 4. Daemon→Bridge: agent.response（逐帧推送）
> 5. Bridge→前端: chunk 帧（逐帧转发）
> 6. Daemon→Bridge: agent.response（final=true）
> 7. Bridge→前端: done 帧
>
> **REST 模式**：
> 1. 前端→Bridge: POST /api/chat/{id}（含 user_id, client_id, content）
> 2. Bridge→Daemon: agent.execute(user_id, client_id, content)
> 3. Daemon→Bridge: agent.response（逐帧推送）
> 4. Bridge 内部累积 content（循环 wait_for_response）
> 5. Daemon→Bridge: agent.response（final=true）
> 6. Bridge→前端: 200 {content}

### 不支持项 / 边界

| 场景 | 状态 | 说明 |
|------|------|------|
| REST 模式流式返回 | ❌ 不支持 | REST 模式始终等待完整响应后一次性返回 |
| 多 Agent 合并聊天 | ❌ 不支持 | 每个端点绑定单一 agent_id |
| 消息编辑/撤回 | ❌ 不支持 | 无此能力 |
| 前端暴露 session_id | ❌ 不支持 | session_id 是 daemon 内部概念，外部不感知 |
| 并发多 turn | ❌ 不支持 | 同一连接同一时刻只能有一个 in-flight turn |

---

**接口契约（帧格式、请求/响应 Schema、错误码）请参见 `architecture/modules/chat-api.md`**

**相关设计文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/features/agent-management.md` — Agent 管理（聊天依赖 Agent 状态）
- `docs/design/architecture/modules/chat-api.md` — Chat API 接口契约（L4）
