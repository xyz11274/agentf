# Dialogue History API 接口契约

> **层级**: L4 · `architecture/modules/dialogue-history-api.md`
> **状态**: 草案
> **创建者**: design-doc-organizer
> **创建日期**: 2026-06-26

---

## What

本文档定义从 daemon DataBus 中拉取指定 session 的对话历史记录的 API 接口契约，包含 RPC 方法、Bridge HTTP 代理端点和前端集成方式。

## Why

当前聊天页面切换会话时，聊天区显示空白：

```
选择 Agent → 选择已有 Session → WebSocket 连接 → 聊天区空白
                                                  ↑ 没有历史消息
```

daemon 内部在每次 `agent.execute` 时已将对话数据存储在 DataBus 中，但没有一个结构化的 API 可以按 session 拉取历史消息列表。需要新增：

1. Daemon RPC `dialogue.history` — 从 DataBus 读取并组装历史消息
2. Bridge HTTP 代理端点 — 将 RPC 暴露为 REST API
3. 前端集成 — 选择会话时自动加载历史

---

## 1. 数据存储现状

每次聊天轮次，daemon 在 DataBus 中存储以下数据：

| 数据 | 存储 Key | 示例值 |
|------|---------|--------|
| 用户输入 | `{dialogue_id}/user_prompt` | `"你好，我的名字是张三"` |
| 助手回复 | `/session/{session_id}/sys/{dialogue_id}/responses/{n}/content` | `"你好张三，有什么可以帮助你的？"` |
| 回复完成标记 | `/session/{session_id}/sys/{dialogue_id}/responses/done` | `"true"` |
| 已消费数 | `/session/{session_id}/sys/{dialogue_id}/responses/consumed_count` | `"1"` |
| 总响应数 | `/session/{session_id}/sys/{dialogue_id}/responses/total_count` | `"1"` |

注意：`user_prompt` 存储在 agent 级 namespace（无 `/session/` 前缀），而 `responses` 存储在 session 级 namespace 下。

---

## 2. Daemon RPC: `dialogue.history`

### 2.1 请求参数

```json
{
  "agent_id": "harness-agent",
  "user_id": "web-user",
  "client_id": "client_a1b2c3"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `agent_id` | string | 是 | 目标 Agent 标识符 |
| `user_id` | string | 是 | 用户标识符 |
| `client_id` | string | 是 | 客户端标识符 |

### 2.2 响应结果

```json
{
  "session_id": "sess_uuid",
  "dialogues": [
    {
      "dialogue_id": "1234567890",
      "messages": [
        {"role": "user",      "content": "你好",           "timestamp": 1719298800000},
        {"role": "assistant", "content": "你好，有什么可以帮助你的？", "timestamp": 1719298801000}
      ]
    },
    {
      "dialogue_id": "9876543210",
      "messages": [
        {"role": "user",      "content": "继续",           "timestamp": 1719298802000},
        {"role": "assistant", "content": "好的，让我们继续",  "timestamp": 1719298803000}
      ]
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `session_id` | string | daemon 内部 session ID（由 user_id+client_id 解析得到）|
| `dialogues` | array | 对话列表，按创建时间升序排列 |
| `dialogues[].dialogue_id` | string | 对话 ID |
| `dialogues[].messages` | array | 消息列表，按时间顺序排列 |
| `messages[].role` | string | `"user"` 或 `"assistant"` |
| `messages[].content` | string | 消息文本 |
| `messages[].timestamp` | int | 毫秒级 Unix 时间戳（可选） |

### 2.3 实现逻辑

```python
def handle_dialogue_history(agent_id, user_id, client_id):
    # 1. 解析 session_id
    session_id = resolve_session_id(user_id, client_id)
    if not session_id:
        return error("session not found")

    # 2. 列出所有 dialogue_id
    # DataBus keys:
    #   {dialogue_id}/user_prompt              (agent 级)
    #   /session/{session_id}/sys/{dialogue_id}/responses/...
    # 通过 list_keys_by_prefix("/session/{session_id}/sys/")
    # 可以得到 /session/{session_id}/sys/{dialogue_id}/...
    # 从中提取所有 dialogue_id
    prefix = f"/session/{session_id}/sys/"
    keys = databus.list_keys_by_prefix(prefix)
    dialogue_ids = extract_dialogue_ids(keys)  # 去重

    # 3. 对每个 dialogue_id，读取消息
    dialogues = []
    for did in sorted(dialogue_ids):
        # 读 user_prompt（agent 级 namespace）
        user_prompt = databus.get(f"{did}/user_prompt")

        # 读 responses（session 级 namespace）
        # /session/{session_id}/sys/{did}/responses/{n}/content
        total = databus.get(f"/session/{session_id}/sys/{did}/responses/total_count")
        responses = []
        for i in range(int(total or 0)):
            content = databus.get(
                f"/session/{session_id}/sys/{did}/responses/{i}/content"
            )
            if content:
                responses.append(content)

        messages = []
        if user_prompt:
            messages.append({"role": "user", "content": user_prompt})
        for resp in responses:
            messages.append({"role": "assistant", "content": resp})

        dialogues.append({
            "dialogue_id": did,
            "messages": messages,
        })

    return {"session_id": session_id, "dialogues": dialogues}
```

### 2.4 错误码

| 条件 | code | 说明 |
|------|------|------|
| `agent_id` 为空 | -32602 | Invalid params |
| Agent 未找到 | -2 | Not found |
| `user_id`+`client_id` 无法解析到 session | -32602 | 无法找到对话记录 |
| DataBus 读取失败 | -1 | 内部错误 |

---

## 3. Bridge HTTP 代理

### 3.1 端点

| 项目 | 值 |
|------|-----|
| 方法 | `GET` |
| 路径 | `/api/sessions/{session_id}/history` |
| 查询参数 | `agent_id` (必填) |
| 成功响应 | `200 OK` |

### 3.2 请求

```
GET /api/sessions/{session_id}/history?agent_id={agent_id}
```

由于前端不感知 `session_id`，也支持通过 `user_id` + `client_id` 查询：

```
GET /api/sessions/history?agent_id={agent_id}&user_id={user_id}&client_id={client_id}
```

### 3.3 响应

```json
{
  "success": true,
  "data": {
    "session_id": "sess_uuid",
    "dialogues": [
      {
        "dialogue_id": "1234567890",
        "messages": [
          {"role": "user", "content": "你好"},
          {"role": "assistant", "content": "你好，有什么可以帮助你的？"}
        ]
      }
    ]
  },
  "error": null
}
```

### 3.4 Bridge DaemonClient 方法

```python
async def dialogue_history(
    self,
    agent_id: str,
    user_id: str,
    client_id: str,
) -> dict[str, Any]:
    return await self.send_request(
        "dialogue.history",
        {"agent_id": agent_id, "user_id": user_id, "client_id": client_id},
    )
```

---

## 4. 前端集成

### 4.1 加载时机

选择已有会话时（`selectSession()`），在连接 WebSocket **之前**调用：

```typescript
async function selectSession(sessionId: string) {
  const session = sessionStore.sessions.find(s => s.session_id === sessionId)
  if (!session) return

  chatStore.clearMessages()

  // 加载历史消息
  try {
    const history = await fetchDialogueHistory(
      agentId, session.user_id, session.client_id
    )
    chatStore.loadHistory(history)  // 将历史消息填入 chatStore.messages
  } catch {
    // 没有历史记录是正常情况（新会话）
  }

  connectChat()
}
```

### 4.2 chatStore 新增方法

```typescript
function loadHistory(dialogues: Dialogue[]): void {
  // 将 dialogues 展平为 ChatMessage[] 填入 messages
  for (const d of dialogues) {
    for (const m of d.messages) {
      messages.value.push({
        id: nextId(),
        role: m.role,
        content: m.content,
        timestamp: m.timestamp ?? Date.now(),
      })
    }
  }
}
```

### 4.3 API 客户端

```typescript
// web/src/api/chatHistory.ts
export async function fetchDialogueHistory(
  agentId: string,
  userId: string,
  clientId: string,
): Promise<DialogueHistoryResponse> {
  const res = await getApiClient().get<DialogueHistoryResponse>(
    `/api/sessions/history?agent_id=${encodeURIComponent(agentId)}`
    + `&user_id=${encodeURIComponent(userId)}`
    + `&client_id=${encodeURIComponent(clientId)}`,
  )
  return res.data
}
```

---

## 5. 涉及文件

| 层 | 文件 | 改动 |
|----|------|------|
| Daemon | `src/daemon/rpc_handler.h` | 声明 `handle_dialogue_history` |
| Daemon | `src/daemon/rpc_handler.cpp` | 实现 `handle_dialogue_history`，在 `register_all()` 中注册 |
| Bridge | `frontend/bridge/client.py` | 新增 `dialogue_history()` 方法 |
| Bridge | `frontend/bridge/routes/sessions.py` | 新增 `GET /api/sessions/history` 端点 |
| Frontend | `frontend/web/src/api/chatHistory.ts` | 新增 `fetchDialogueHistory()` |
| Frontend | `frontend/web/src/stores/chatStore.ts` | 新增 `loadHistory()` |
| Frontend | `frontend/web/src/pages/ChatPage.vue` | 选会话时调用加载历史 |

---

**相关文档**:
- `architecture/modules/session-api.md` — Session API 接口契约
- `features/session-management.md` — 会话管理功能设计
- `frontend/bridge/docs/design/architecture/modules/chat-api.md` — Chat API 接口契约
