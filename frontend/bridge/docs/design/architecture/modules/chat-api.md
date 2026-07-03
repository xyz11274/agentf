# Chat API 接口契约

> **层级**: L4 · `architecture/modules/chat-api.md`
> **状态**: 已更新 · 2026-07-02 增加 Artifact 文件访问契约
> **创建者**: `design-doc-organizer`

---

## What

本文档定义 Bridge 聊天功能的完整 API 接口契约，包含 WebSocket 和 REST 两种模式的端点、帧格式、请求/响应体 Schema 和错误码。

**核心设计原则**: `session_id` 是 daemon 内部概念，外部客户端不感知。客户端通过 `user_id` + `client_id` 标识身份，daemon 内部按 `user_id:client_id` 复合键自动分配/复用 session。

---

## 1. WebSocket 流式聊天

### 1.1 端点

| 项目 | 值 |
|------|-----|
| 端点 | `ws://<bridge_host>:<bridge_port>/ws/chat/{agent_id}` |
| 协议 | JSON-RPC 2.0 格式消息 over WebSocket (RFC 6455) |
| 路径参数 | `agent_id` — 目标 Agent 标识符 |
| 超时 | 120s（单次 turn） |
| 保活 | 服务端每 30s 发送 `ping` 帧 |

### 1.2 帧格式

#### 客户端 → Bridge

**ChatRequest**（用户消息）：

```json
{
  "user_id": "web-user",
  "client_id": "client_a1b2c3",
  "content": "用户输入的消息文本"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `user_id` | string | 是 | 用户标识符（前端内置固定值） |
| `client_id` | string | 是 | 客户端/会话标识符（新建会话时生成） |
| `content` | string | 是 | 用户消息内容 |

**StopSignal**（中断当前 turn）：

```json
{"type": "stop"}
```

**PingSignal**（心跳保活）：

```json
{"type": "ping"}
```

#### Bridge → 客户端

**ChatChunk**（流式输出块）：

```json
{"type": "chunk", "content": "部分响应文本", "final": false}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"chunk"` |
| `content` | string | 本次推送的文本片段；可包含 Markdown Artifact URL |
| `final` | bool | `true` 表示本轮最后一块 |

**ChatDone**（本轮结束信号）：

```json
{"type": "done", "final": true}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"done"` |
| `final` | bool | 固定值 `true` |

> 不再返回 `session_id` — session 是 daemon 内部概念，前端不需要感知。

**ErrorFrame**（错误信号）：

```json
{"type": "error", "message": "错误描述", "code": -32000}
```

错误帧发送后 Bridge 会关闭 WebSocket 连接。错误码语义：

| code | 语义 |
|------|------|
| -32600 | 无效消息格式 |
| -32000 | 通用 daemon 错误 / 超时 |
| 1011 | Bridge 内部错误 |

### 1.3 连接生命周期

```
客户端                          Bridge
  |                               |
  |-- WS connect ----------------->|
  |<-- accept ---------------------|
  |-- {"user_id":"web-user",      >|
  |    "client_id":"c_abc",        |
  |    "content":"你好"}           |
  |<-- {"type":"chunk", ...} ------|  (0..N 次)
  |<-- {"type":"chunk", "final":t} |
  |<-- {"type":"done"} ------------|
  |-- {"user_id":"web-user",      >|  (多轮复用同一连接)
  |    "client_id":"c_abc",        |
  |    "content":"继续"}           |
  |<-- ... ------------------------|
  |-- {"type":"stop"} ------------>|  (中断本轮)
  |<-- {"type":"done"} ------------|
```

- 每个连接绑定一个 `agent_id`
- 多轮对话通过 `user_id` + `client_id` 串联（daemon 内部映射到同一个 session）
- 同一连接同一时刻只能有一个 in-flight turn
- 客户端可发送 `stop` 提前终止本轮
- 连接断开时 in-flight turn 自动取消

---

## 2. REST 同步聊天

### 2.1 端点

| 项目 | 值 |
|------|-----|
| 方法 | `POST` |
| 路径 | `/api/chat/{agent_id}` |
| 路径参数 | `agent_id` — 目标 Agent 标识符 |
| 请求 Content-Type | `application/json` |
| 超时 | 120s |
| 成功响应 | `200 OK` |

### 2.2 请求体

```json
{
  "user_id": "web-user",
  "client_id": "client_a1b2c3",
  "content": "用户消息文本"
}
```

ChatRestRequest：

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `user_id` | string | 是 | — | 用户标识符 |
| `client_id` | string | 是 | — | 客户端/会话标识符 |
| `content` | string | 是 | — | 用户消息内容 |

### 2.3 响应体（成功）

```json
{
  "success": true,
  "data": {
    "task_id": "task_xyz789",
    "content": "Agent 的完整响应文本"
  },
  "error": null
}
```

ChatRestResponse：

| 字段 | 类型 | 说明 |
|------|------|------|
| `task_id` | string | daemon 侧的任务 ID |
| `content` | string | 完整响应文本（已拼接所有 chunk）；可包含 Markdown Artifact URL |

### 2.4 执行流程

```
客户端                          Bridge                        Daemon
  |                               |                              |
  |-- POST /api/chat/{id} ------->|                              |
  |   {user_id, client_id,        |                              |
  |    content}                    |                              |
  |                               |-- agent.execute ------------>|
  |                               |   {user_id, client_id,       |
  |                               |    content}                   |
  |                               |<-- {message_id, status} -----|
  |                               |                              |
  |                               |  (轮询: wait_for_response)   |
  |                               |<-- {content, final: false} --|  (0..N 次)
  |                               |<-- {content, final: true}  --|
  |                               |                              |
  |<-- {success, data: {content}} |                              |
```

### 2.5 错误码

| 条件 | HTTP | `error` 消息 |
|------|------|-------------|
| Daemon 返回 `InvalidParamsError` | 400 | daemon 错误消息 |
| Daemon 返回 `NotFoundError` | 404 | daemon 错误消息 |
| Daemon 连接失败 | 502 | 连接失败描述 |
| `agent.execute` 超时 (120s) | 504 | `"agent.execute timed out after 120.0s"` |
| `wait_for_response` 超时 (120s) | 504 | 超时描述 |
| agent 未返回 `task_id` | 500 | `"agent.execute did not return a task_id"` |
| 其他 Daemon 错误 | 500 | daemon 错误消息 |

---

## 3. 双模式比较

| 维度 | WebSocket 模式 | REST 模式 |
|------|---------------|-----------|
| 连接持久性 | 长连接（多轮复用） | 无状态（每轮独立请求） |
| 流式输出 | ✅ 逐帧推送 ChatChunk | ❌ 等完整返回 |
| 心跳保活 | ✅ 双向 ping/pong | 不需要 |
| 中断本轮 | ✅ 发送 `stop` 信号 | ❌ 不支持（发起了就必须等） |
| 适用客户端 | Vue 前端交互式对话 | 程序化调用、自动化脚本 |

---

## 4. 通用契约

### 4.1 Daemon 错误 → HTTP 状态码映射

| 错误场景 | 异常类 | HTTP |
|----------|--------|------|
| 无效参数 | `InvalidParamsError` | 400 Bad Request |
| 资源不存在 | `NotFoundError` | 404 Not Found |
| 请求超时 | `DaemonTimeoutError` | 504 Gateway Timeout |
| 连接失败 | `DaemonConnectionError` | 502 Bad Gateway |
| 其他错误 | `DaemonError` | 500 Internal Server Error |

### 4.2 dialogue_id 生成规则

当客户端未显式提供 `dialogue_id` 时，Bridge 自动生成：

```
{dialogue_id} = {MMDDHHmmSS}-{uuid4.hex[:12]}
```

示例：`0625152345-abc123def456`

此规则确保每轮对话的 `dialogue_id` 唯一，避免 `coord_forwarded` 冲突。

## 5. Artifact 文件访问契约

### 5.1 推荐端点

| 项目 | 值 |
|------|-----|
| 方法 | `GET` |
| 路径 | `/api/artifacts/{agent_id}/files/{relative_path:path}` |
| 路径参数 | `agent_id` — 生成产物的 Agent 标识；`relative_path` — 相对于 Agent Package 根目录的文件路径 |
| 成功响应 | `200` 文件流 |

示例：

```text
GET /api/artifacts/outfit-assistant/files/outfit_assistant/outputs/task-123/report.html
GET /api/artifacts/outfit-assistant/files/outfit_assistant/outputs/task-123/assets/look.png
```

Bridge 保留目录层级。若 `report.html` 使用 `assets/look.png`，浏览器会请求同级目录下的 `assets/look.png`，无需重写 HTML 内容。

兼容端点 `GET /api/artifacts/{agent_id}?path=...` 仍可读取包根目录内文件，但它允许传入服务端路径，仅用于兼容已有调用，不应作为新的外部集成契约。

### 5.2 路径与文件类型约束

- 服务根目录固定为 `settings.agent_packages_path`；Bridge 对目标路径做规范化并验证其仍位于该根目录内。
- `agent_id` 当前仅用于 URL 语义，不是授权边界；不能依赖它隔离不同租户或 Agent 的文件。
- 支持图片以及 `html`、`htm`、`css`、`js`、`json`、`svg` 等网页产物类型。
- 图片响应优先按文件魔数识别 MIME，避免扩展名与实际编码不一致导致浏览器无法显示。

| 条件 | HTTP | 说明 |
|------|------|------|
| 文件存在且位于包根目录 | 200 | 返回文件流与对应 Content-Type |
| 路径逃逸包根目录 | 403 | 拒绝目录穿越和根目录外文件 |
| 文件不存在或不是普通文件 | 404 | 不暴露宿主机其他路径信息 |
| 文件类型不允许 | 415 | 拒绝未列入白名单的类型 |

### 5.3 聊天消息中的 URL

WS 与 REST 不新增结构化 `artifacts` 字段，Artifact URL 直接包含在 `content` Markdown 中：

```json
{
  "type": "chunk",
  "content": "搭配结果：![look](https://agent.example.com/api/artifacts/outfit-assistant/files/outfit_assistant/outputs/task-123/look.png)",
  "final": true
}
```

URL 生成规则：

- `ARTIFACT_PUBLIC_BASE_URL` 为空：保留 `/api/artifacts/...` 相对 URL，适用于内置 UI 或同源反向代理。
- `ARTIFACT_PUBLIC_BASE_URL` 非空：WS 与 REST 在返回前补全为绝对 URL，适用于外部服务调用。
- 公网基地址应指向外部客户端实际可访问的 Bridge 入口；如反向代理挂载在子路径，配置中需包含该路径前缀。

外部客户端能否访问图片取决于 Bridge 是否真正对该客户端可达。仅配置 URL 不会自动建立公网入口、防火墙规则或反向代理。

### 5.4 安全与生命周期

当前实现的安全边界是“只允许读取 Agent Package 根目录内的白名单文件”，尚未实现以下生产能力：

- Artifact 身份认证与按用户/Agent 授权；
- 不透明 Artifact ID 与签名、限时 URL；
- 产物保留期限、配额和自动清理；
- 独立对象存储或 CDN 分发。

因此，公网部署时应由反向代理或 API Gateway 增加认证和访问控制；不能把当前 `agent_id` 路径参数视为权限校验。

---

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| WS 帧格式（ChatRequest/ChatChunk/ChatDone） | 本文档 §1.2 |
| REST 请求/响应 Schema | 本文档 §2.2–2.3 |
| 错误码映射 | 本文档 §2.5，§4.1 |
| dialogue_id 生成规则 | 本文档 §4.2 |
| Artifact 文件端点与安全约束 | 本文档 §5.1–5.4 |
| 双模式选择依据 | `features/chat.md` L2 |
| Pydantic 模型定义 | `frontend/bridge/models.py` |

**相关文档**:
- `docs/design/features/chat.md` — 聊天功能设计（L2）
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
