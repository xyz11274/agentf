# DaemonClient 通信协议设计

> **层级**: L4 · `architecture/modules/daemon-client-protocol.md`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What

`DaemonClient` 是 Bridge 与 `agentflowd` 守护进程之间的通信客户端，通过单个长连接 WebSocket 实现 JSON-RPC 2.0 协议通信。本文档定义其协议细节、连接生命周期、请求分派模型、通知消费模型和错误处理。

---

## 1. 通信协议

### 1.1 传输层

| 项目 | 值 |
|------|-----|
| 协议 | WebSocket (RFC 6455) |
| 连接地址 | `ws://{daemon_host}:{daemon_port}` |
| 默认地址 | `ws://localhost:17891` |
| 连接模式 | **单例长连接** — 所有请求（Agent 管理、聊天、会话管理、健康检查）共享同一连接 |

### 1.2 应用层

| 项目 | 值 |
|------|-----|
| 协议 | JSON-RPC 2.0 |
| 帧格式 | 单条 JSON（newline-delimited） |
| 请求 | `{"jsonrpc":"2.0", "id":<int>, "method":"<rpc_method>", "params":{…}}` |
| 响应 | `{"jsonrpc":"2.0", "id":<int>, "result":…}` |
| 错误 | `{"jsonrpc":"2.0", "id":<int>, "error":{"code":<int>,"message":<str>}}` |
| 通知（无 id） | `{"jsonrpc":"2.0", "method":"agent.response", "params":{…}}` |

---

## 2. 连接生命周期

```
  初始化             connect()             使用中               close()
 ────────┬─────────────────┬────────────────────┬──────────────────┬──────→
         │                 │                    │                  │
         │  未连接          │  WS connect        │   send_request   │  关闭帧
         │  reader_loop:无  │  reader_loop 启动  │   dispatch_frame │  cancel pending
         │  _notification   │                    │                  │
         │  _queue: 空      │                    │                  │
```

### 2.1 阶段说明

| 阶段 | 方法 | 行为 |
|------|------|------|
| 初始化 | `__init__()` | 设置 pending 字典、notification 队列，不建立连接 |
| 连接 | `connect()` | 建立 WebSocket 连接；启动后台 `_reader_loop` 协程 |
| 请求 | `send_request()` | 发送 JSON-RPC 请求，通过 future 等待响应（详见 §3） |
| 关闭 | `close()` | 发送 WS 关闭帧；取消所有 pending futures；关闭 reader loop |
| 属性查询 | `is_connected` | 返回当前连接状态（bool） |

### 2.2 自动重连

自动重连不做在 `DaemonClient` 内部，而是由上层 `_daemon_status_poll_loop`（在 `main.py` 中）在每次刷新缓存前检测并触发：

```python
if not client.is_connected:
    try:
        await client.connect()
    except Exception:
        pass  # 下次循环再试
```

---

## 3. 请求分派模型

### 3.1 核心结构

```
DaemonClient
├── _id_counter: itertools.count(1)    # 递增 request_id
├── _pending: dict[int, Future]        # 等待响应的请求
└── _notification_queue: asyncio.Queue  # 通知消息队列
```

### 3.2 请求流程

```
send_request(method, params)
├── 1. request_id = next(_id_counter)
├── 2. future = asyncio.get_event_loop().create_future()
├── 3. _pending[request_id] = future
├── 4. 发送 JSON-RPC 帧到 WebSocket
└── 5. await future  (等待 _dispatch_frame 设置结果)
```

### 3.3 响应分派流程

```
_reader_loop (后台持续读取)
└── 收到一行 JSON
    └── _dispatch_frame(text)
        ├── 非 dict → 丢弃
        ├── 无 "id" → 通知（notification）
        │   └── method == "agent.response" → 入队 _notification_queue
        │   └── 其他通知 → 忽略（日志 debug）
        ├── 有 "id" → 请求响应
        │   └── 从 _pending 中取出对应 future
        │       ├── future 已 done → 丢弃
        │       └── future.set_result(message) → 唤醒 send_request
        └── id 不合法 → 丢弃
```

### 3.4 并发支持

- 多个 `send_request` 可并发调用
- 每个请求通过独立 `request_id` + `Future` 区分
- 响应由 `_dispatch_frame` 按 `id` 分发到正确的 Future
- 无锁：asyncio 单线程模型下 `_pending` 字典操作天然安全

---

## 4. 通知消费模型

### 4.1 通知类型

Daemon 在 Agent 工作流执行期间推送 `agent.response` 通知：

```json
{
  "jsonrpc": "2.0",
  "method": "agent.response",
  "params": {
    "session_id": "sess_abc",
    "content": "部分响应文本",
    "final": false
  }
}
```

`params` 包含的字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `session_id` | string | 会话 ID |
| `content` | string | 本次推送的文本片段 |
| `text` | string | 部分通知中可能出现（与 `content` 等价） |
| `final` | bool | `true` 表示这是最后一帧 |

### 4.2 wait_for_response 机制

```python
async def wait_for_response(agent_id, *, timeout=30.0):
    msg = await asyncio.wait_for(
        self._notification_queue.get(), timeout=timeout
    )
    return msg.get("params", {})
```

- 从 `_notification_queue` 中读取下一条通知
- 超时则抛出 `DaemonTimeoutError`
- 调用者需要循环调用直到收到 `final: true`

### 4.3 agent_poll_response（备选轮询）

```python
async def agent_poll_response(agent_id, session_id, task_id, timeout_ms=15000):
    # 发送 JSON-RPC 请求等待 daemon 响应
    return await self.send_request("agent.poll_response", {...})
```

`agent.poll_response` 是备选方案。当前聊天实现优先使用 `wait_for_response`（基于 notification 推送），因为推送模式比轮询模式延迟更低。`agent_poll_response` 作为 fallback 保留。

---

## 5. RPC 方法清单

| RPC 方法 | 用途 | 调用者 |
|----------|------|--------|
| `health` | 健康检查 | 监控缓存 |
| `agent.list` | 列出已加载 Agent | Agent 管理 |
| `agent.info` | Agent 详情 | Agent 管理 |
| `agent.load` | 加载 Agent | Agent 管理 |
| `agent.unload` | 卸载 Agent | Agent 管理 |
| `agent.pause` | 暂停 Agent | Agent 管理 |
| `agent.resume` | 恢复 Agent | Agent 管理 |
| `agent.execute` | 执行 Agent 工作流（聊天核心） | 聊天 |
| `agent.poll_response` | 轮询 Agent 响应（备选） | 聊天（备选） |
| `session.list` | 列出会话 | 会话管理 |
| `session.destroy` | 销毁会话 | 会话管理 |
| `session.clear_dialogue` | 清除对话记录 | 会话管理 |
| `workflow.list` | 列出工作流 | （预留） |
| `workflow.info` | 工作流详情 | （预留） |
| `runtime.status` | 运行时状态 | 监控缓存 |

---

## 6. 错误处理

### 6.1 异常类层次

```
DaemonError (base)
├── DaemonConnectionError   — 连接失败（WS 无法建立）
├── DaemonTimeoutError      — 请求超时（超过 timeout 无响应）
├── InvalidParamsError      — 无效参数（JSON-RPC code -32602）
└── NotFoundError           — 资源不存在
```

### 6.2 触发条件

| 异常类 | 触发条件 | code 属性 |
|--------|----------|-----------|
| `DaemonConnectionError` | WebSocket `connect()` 抛出异常 | `-32000` |
| `DaemonTimeoutError` | `asyncio.wait_for` 超时 | `-32000` |
| `InvalidParamsError` | JSON-RPC error code `-32602` | `-32602` |
| `NotFoundError` | JSON-RPC error message 匹配特定模式 | `-32602` |
| `DaemonError` | 其他 JSON-RPC error（code ≠ -32602） | 原始 code |

### 6.3 pending 请求清理

- `close()` 时遍历 `_pending`，用 `CancelledError` 取消所有 Future
- `_dispatch_frame` 发现 Future 已 `done` 时静默丢弃
- 任意未捕获异常通过 `_reader_loop` 传递，最终关闭连接并 `_fail_all_pending`

---

## 7. 配置项

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `daemon_host` | `localhost` | daemon 主机名 |
| `daemon_port` | `17891` | daemon WS 端口 |
| `request_timeout_seconds` | `5.0` | 单次 JSON-RPC 请求超时（秒） |

Daemon 连接地址推导：`ws://{daemon_host}:{daemon_port}`

---

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 传输层协议 | 本文档 §1.1 |
| 应用层协议 | 本文档 §1.2 |
| 连接生命周期 | 本文档 §2 |
| 请求分派模型 | 本文档 §3 |
| 通知消费模型 | 本文档 §4 |
| RPC 方法清单 | 本文档 §5 |
| 错误处理与异常类 | 本文档 §6 |
| 配置项 | 本文档 §7 |
| 单连接 ADR | `adrs/0002-single-websocket-connection.md` L3 |

**相关文档**:
- `docs/design/adrs/0002-single-websocket-connection.md` — 单 WS 长连接架构决策
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `frontend/bridge/client.py` — 源码实现
- `frontend/bridge/config.py` — 配置定义
