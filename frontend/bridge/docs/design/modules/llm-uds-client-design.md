# LlmUdsClient — UDS LLM 客户端详细设计

> 领域：模块设计
> 状态：**已废弃**
> 最后更新：2026-07-07
>
> ⚠️ **本文档描述的是旧三段式协议（llm.task.create → llm.stream → llm.task.destroy + intentions/prompt 格式），
> 已于 2026-07-07 被新 llm.chat 单步协议（OpenAI 兼容格式透传）替代。**
>
> 新协议详见：[UDS Server 改造：支持 OpenAI 兼容格式](./uds-server-openai-compatible-design.md)
>
> 客户端实现源码（已切新协议）：`examples/agent/outfit-assistant/nodes/common/clients/llm_uds_client.py`

> 设计文档对应实现：旧协议已删除

---

## 1. 概述

`LlmUdsClient` 是 LLM 调用路径中的 **UDS 通道实现**，通过 Unix Domain Socket 与 UDS Plugin Server 通信，完成 LLM 推理。它是 `LlmClient` 抽象接口的一个实现，与 `OpenAiCompatibleClient`（HTTP 通道）并列。

### 设计目标

- 通过 UDS 提供与 HTTP OpenAI API **语义等价**的 LLM 调用接口
- 支持三段式协议：`create_task` → `stream` → `destroy_task`
- 请求体**全量透传**（`messages`、`tools` 等完整携带到服务端）
- 带 **HTTP 兜底**的双路径设计（UDS 不可用时降级）
- 与 `base.py` 的工厂路由 `aisf/` 前缀无缝配合

### 适用范围

| 场景 | 适用 |
|------|------|
| Agent 节点内调用 LLM | ✅ 通过 `create_llm_client()` 工厂 |
| 独立脚本调试 | ✅ 直接实例化 `LlmUdsClient()` |
| 无 UDS 环境 | ❌ 切回 HTTP 通道 |

---

## 2. 架构位置

```
create_llm_client(model)
    │
    ├── model.startswith("aisf/") ──► LlmUdsClient     ← 本文档
    │
    └── else ──► OpenAiCompatibleClient                  ← HTTP 通道
```

`LlmUdsClient` 位于以下调用链中：

```
节点代码 (coordinator_decide / skill_executor)
    │
    ▼
LlmClient.chat_completion()          ← 抽象接口
    │
    ▼
LlmUdsClient.chat_completion()       ← UDS 实现
    │
    ├── _try_uds() → _UdsTransport.connect()
    ├── _complete_uds()
    │   ├── llm.task.create
    │   ├── llm.stream
    │   ├── 接收流式 events
    │   └── llm.task.destroy
    └── 返回 OpenAI 兼容响应字典
```

---

## 3. 传输层：_UdsTransport

### 3.1 帧协议

```
TCP/UDS 字节流：
┌─────────────────┬────────────────────────────────────┐
│  4-byte 长度头   │          UTF-8 JSON 负载            │
│  (Big-Endian)    │                                    │
├─────────────────┼────────────────────────────────────┤
│ 00 00 00 XX      │ {"method":"llm.task.create", ...}  │
│                 │                                    │
│ 负载长度 (uint32) │    0 ~ MAX_FRAME_SIZE (10MB)      │
└─────────────────┴────────────────────────────────────┘
```

| 字段 | 字节数 | 编码 | 说明 |
|------|--------|------|------|
| Length | 4 | `>I` (struct pack big-endian) | 负载字节数（不含头部） |
| Payload | Length | UTF-8 JSON | 序列化的 JSON 对象 |

### 3.2 连接生命周期

```
┌─────────────────────────────────────────────────┐
│  connect()                                       │
│    socket.socket(AF_UNIX, SOCK_STREAM)           │
│    socket.settimeout(300.0)                      │
│    socket.connect(sock_path)                     │
│    └─ OSError / ConnectionRefusedError            │
│       → LlmUdsError("UDS connect failed")        │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│  send_json(data)                                 │
│    payload = json.dumps(data, ensure_ascii=False) │
│    header = struct.pack(">I", len(payload))       │
│    sock.sendall(header + payload)                 │
│    └─ OSError → LlmUdsError("UDS send failed")   │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│  recv_json()                                     │
│    len_buf = sock.recv(4)                        │
│    └─ < 4 bytes → LlmUdsError("connection closed")│
│    frame_len = struct.unpack(">I", len_buf)[0]    │
│    └─ > MAX_FRAME_SIZE → LlmUdsError("too large") │
│    循环 recv() 直到收满 frame_len                  │
│    return json.loads(raw)                         │
│    └─ OSError → LlmUdsError("recv body failed")   │
│    └─ 空 chunk → LlmUdsError("closed mid-frame")  │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│  close()                                         │
│    sock.close()                                   │
│    sock = None                                    │
└─────────────────────────────────────────────────┘
```

### 3.3 超时与限制

| 参数 | 值 | 说明 |
|------|-----|------|
| `UDS_TIMEOUT` | 300s | 单次 socket 读写超时 |
| `MAX_FRAME_SIZE` | 10 MB | 单帧最大负载长度（防御性限制） |
| 默认 socket 路径 | `/tmp/agentflow_plugin.sock` | 可通过 `AF_UDS_SOCKET_PATH` 覆盖 |

---

## 4. 应用层协议

### 4.1 三段式调用流程

```
客户端 (LlmUdsClient)                 UDS Plugin Server
─────────────────────                 ─────────────────

Step 1: CREATE TASK
  POST {"request_id": "py-1-xxxx",
        "method": "llm.task.create",
        "plugin_type": "LLM",
        "plugin_name": "LLMPlugin",
        "params": {
          "callback_channel": "stream.channel",
          "plugin_name": "LLMPlugin",
          "model": {"model_name": "aisf/deepseek-v4-flash"}}}
       ────────────────────────────────────────────────►
  ◄──── {"data": {"task_id": "t_xxxx"}}
  
Step 2: STREAM
  POST {"request_id": "py-1-xxxx_stream",
        "method": "llm.stream",
        "plugin_type": "LLM",
        "plugin_name": "LLMPlugin",
        "params": {
          "task_id": "t_xxxx",
          "request": {
            "intentions": [{"query": "..."}],
            "prompt": ["..."],
            "_messages": [...],     ← 透传
            "tools": [...]          ← 透传
          },
          "model": {"model_name": "aisf/deepseek-v4-flash"}}}
       ────────────────────────────────────────────────►
  ◄──── 事件流 (多条 recv_json)

Step 3: DESTROY TASK（finally 块，best-effort）
  POST {"request_id": "py-1-xxxx_destroy",
        "method": "llm.task.destroy",
        "plugin_type": "LLM",
        "plugin_name": "LLMPlugin",
        "params": {"task_id": "t_xxxx"}}
       ────────────────────────────────────────────────►
```

### 4.2 支持的方法列表

| 方法 | 客户端调用点 | 用途 | 关键请求字段 | 关键响应字段 |
|------|-------------|------|-------------|-------------|
| `llm.task.create` | `_complete_uds()` 第1步 | 创建推理任务 | `model.model_name` | `data.task_id` |
| `llm.stream` | `_complete_uds()` 第2步 | 发起流式推理 | `task_id`, `request.*` | 事件流 |
| `llm.task.destroy` | `_complete_uds()` finally | 清理任务 | `task_id` | — |

### 4.3 请求体构造策略

`UDS` 是传输封装层，客户端应**全量发送**而**不自限**：

```python
uds_request = {
    "intentions": [{"query": last_user_message}],
}
if system_prompt:
    uds_request["prompt"] = [system_prompt]
# 以下为透传字段：服务端能解析多少是服务端的事
uds_request["_messages"] = messages      # 完整消息历史
uds_request["tools"] = tools              # 工具定义
```

**分层原则**：

| 层 | 内容 | 客户端责任 |
|----|------|-----------|
| UDS 原生字段 | `intentions`, `prompt` | 提取并发送 |
| 透传字段 | `_messages`, `tools`, 未来扩展 | 无条件携带 |
| 服务端实现 | 解析哪些字段 | 客户端不关心 |

### 4.4 模型名分辨链

```python
model = (
    sock_path 参数                          # 显式传入
    or os.environ.get("SYS_AGENT_LLM_MODEL")  # UDS 服务端指定（最高优先级）
    or os.environ.get("AF_LLM_MODEL")         # 通用模型名
    or os.environ.get("ANTHROPIC_MODEL")      # 遗留兼容
    or DEFAULT_MODEL                          # "qwen3.5-flash"
)
```

> `chat_completion()` 入口处，模型名已经过 `create_llm_client()` 的前缀路由判断（`aisf/` 前缀），但 `LlmUdsClient` 本身从环境变量及兜底值重新构建模型名。

---

## 4.5 request_id 配对机制（并发防串线）

### 4.5.1 问题背景

同一 agentflowd 实例下，**coordinator_decide 与 skill_executor 是两个独立 Python 子进程**，各自持有自己的 `_UdsTransport` socket 连到同一 UDSPluginServer。

```
coordinator_decide (子进程 A) ──┐
                                │
skill_executor (子进程 B)     ──┤──► UDSPluginServer (单进程)
                                │      ├── per-connection 顺序处理 ✅
其他 skill_executor (子进程 C)──┘      └ 但跨连接共享全局状态（tasks/tool_registry）
```

**UDS socket 是全双工字节流**——客户端 `send_json` 写入和 `recv_json` 读取是两个独立方向。当多个子进程并发调用 LLM 时，存在两类串线风险：

| 风险 | 描述 | 当前防护 |
|------|------|---------|
| 请求串线 | A 的请求被 B 的 recv 拿走 | 无（每连接独立 socket，server per-connection 顺序处理） |
| 响应串线 | A 的 `recv_json` 拿到 B 请求的响应帧 | **无防护** ❌ |

### 4.5.2 request_id 的生成与携带

每次 `chat_completion()` 调用生成唯一 `request_id`，贯穿该次通信的所有帧：

```python
def _next_req_id(self) -> str:
    """生成唯一 request_id：py-{counter}-{uuid8}"""
    self._req_counter += 1
    return f"py-{self._req_counter}-{uuid.uuid4().hex[:8]}"

# _chat() 内构造 chat_payload 时携带
chat_payload = {
    "request_id": req_id,          # ← 客户端生成
    "method": "llm.chat",
    "params": {"request": request_body},
    ...
}
t.send_json(chat_payload)
```

### 4.5.3 配对规则（客户端侧）

`_chat` 非流式路径和 `_receive_chat_events` 流式路径，**recv 后必须校验 `request_id` 匹配**，不匹配则跳过该帧继续 recv：

```python
# _chat 非流式
resp = t.recv_json()
if resp.get("request_id") != req_id:
    raise LlmUdsError(
        f"UDS response mismatch: expected {req_id}, "
        f"got {resp.get('request_id')}"
    )

# _receive_chat_events 流式
while True:
    event = t.recv_json()
    if event.get("request_id") != req_id:
        continue   # 跳过别人的帧，等自己的
    event_type = event.get("event_type", "")
    # ... 处理自己的事件
```

> 流式循环用 `continue` 而非抛错——因为同一 socket 上可能混入其他连接的事件帧（极少情况，但防御性处理）。抛错只在非流式路径，因为非流式期望一对一响应。

### 4.5.4 服务端契约

UDSPluginServer（含 mock_uds_server）**必须**在所有响应帧中回带 `request_id` 字段：

| 帧类型 | 必须包含 request_id | 说明 |
|--------|-------------------|------|
| 非流式响应（`llm.chat` stream=false） | ✅ | `_wrap_response()` 已支持 |
| 流式事件帧（`llm_token` / `llm_tool_calls` / `llm_final` / `llm_error`） | ✅ | **新增要求**——所有事件帧回带 |
| 错误帧 | ✅ | 含错误响应 |

### 4.5.5 不串不丢的保证

**不串**（请求/响应不会错配）：
1. 每次调用生成唯一 `request_id`（`uuid4` 保证跨进程全局唯一）
2. 客户端 recv 后校验 `request_id`，不匹配则跳过
3. 服务端所有响应帧（含流式事件）回带 `request_id`

**不丢**（不会漏接收响应帧）：
1. 流式循环只在收到 `llm_final` 或 `llm_error` 时退出，不会提前返回
2. 即使收到 `request_id` 不匹配的帧，用 `continue` 跳过而非 `return`——确保自己的帧最终会被读到
3. `recv_json()` 是阻塞读取，不会返回空帧（除非连接关闭，此时抛 `LlmUdsError`）

**边界情况**：

| 场景 | 行为 | 处理 |
|------|------|------|
| 收到别人的 `llm_final` 帧 | `request_id` 不匹配 | `continue` 跳过，继续等自己的 |
| 收到别人的 `llm_error` 帧 | `request_id` 不匹配 | `continue` 跳过（**不抛错**——别人的错误不关我事） |
| 连接断开 | `recv_json()` 抛 `LlmUdsError` | 向上抛出，chat_completion 失败 |
| 服务端不回带 `request_id` | 字段缺失 | 视为不匹配跳过。vendor C++ `UDSEvent::ToJson()` 已序列化 `request_id`，正常情况下必回带；缺失视为协议异常，由 `UDS_TIMEOUT` 兜底防止死循环 |

### 4.5.6 与 socket 超时的协作

`recv_json()` 受 `UDS_TIMEOUT=300s` socket 级超时保护。如果服务端迟迟不回带匹配 `request_id` 的帧，`socket.timeout` 会抛错让客户端退出——**不会死循环**。

```python
# _UdsTransport.connect() 内
self._sock.settimeout(UDS_TIMEOUT)   # 300s，覆盖所有后续 recv
```

---

## 5. 事件流处理

### 5.1 事件类型

| 事件类型 | 触发时机 | data 字段 | 处理逻辑 |
|---------|---------|----------|---------|
| `llm_token` | 每生成一个 token | `{"token": "你好"}` | 累加到 `full_answer` |
| `llm_final` | 生成完成 | `{"answer": "你好！"}` | 返回 `(full_answer, answer)` |
| `llm_error` | 发生错误 | `{"error_code": "500"}` | 抛出 `LlmUdsError` |
| 其他 | 未知事件 | — | 静默忽略 |

### 5.2 事件接收循环

```python
def _receive_events(self, t: _UdsTransport) -> tuple[str, str]:
    full_answer = ""
    while True:
        event = t.recv_json()
        event_type = event.get("event_type", "")
        data_str = event.get("data", "")
        
        # data 字段可能是 string（JSON 序列化）或 dict
        data = json.loads(data_str) if isinstance(data_str, str) else data_str
        
        if event_type == "llm_token":
            full_answer += data.get("token", "")
        elif event_type == "llm_final":
            return full_answer, data.get("answer", "")
        elif event_type == "llm_error":
            raise LlmUdsError(f"LLM error: {data.get('error_code', 'unknown')}")
        # 其他 event_type → 忽略
```

### 5.3 返回值策略

`chat_completion()` 返回 OpenAI 兼容格式：

```python
{
    "choices": [{
        "message": {"content": final_answer, "role": "assistant"},
        "finish_reason": "stop",
        "index": 0,
    }],
}
```

其中 `final_answer` 优先取 `llm_final` 事件的 `answer` 字段；若为 `""`，则兜底使用累加的 `full_answer`。

---

## 6. 错误处理与边界情况

### 6.1 错误场景矩阵

| 场景 | 表现 | 处理策略 |
|------|------|---------|
| UDS socket 文件不存在 | `connect()` 抛 `OSError` | `_try_uds()` 返回 `False`，`chat_completion()` 抛 `LlmUdsError` |
| 连接被拒绝 | `ConnectionRefusedError` | 同上 |
| 发送时连接断开 | `sendall()` 抛 `OSError` | `_complete_uds()` 抛 `LlmUdsError` |
| 接收超时 | `recv()` 抛 `socket.timeout` | 透传为 `LlmUdsError` |
| 服务端返回空帧 | `recv()` 返回 `b""` | `LlmUdsError("UDS connection closed mid-frame")` |
| 服务端返回 `error` 字段 | `stream_resp["error"]` | `LlmUdsError("UDS stream error: ...")` |
| 服务端返回 `llm_error` 事件 | 事件循环中识别 | `LlmUdsError("LLM error: {error_code}")` |
| create_task 响应缺 `task_id` | `data` 中无此字段 | `LlmUdsError("missing task_id")` |
| destroy_task 失败 | 发送异常 | 静默吞没（best-effort） |

### 6.2 关键不变量

| ID | 不变量 | 验证 |
|----|--------|------|
| I-UDS-1 | `create_task` 必须在 `stream` 之前调用 | 代码顺序保证 |
| I-UDS-2 | `stream` 完成后始终调用 `destroy_task` | `try/finally` 块保证 |
| I-UDS-3 | `create_task` 响应缺少 `task_id` → 立即报错，不继续 | `_extract_task_id()` 检查 |
| I-UDS-4 | 事件流必须收敛：收到 `llm_final` 或 `llm_error` 时退出 | 循环终止条件 |
| I-UDS-5 | `chat_completion()` 返回值始终为 OpenAI 兼容格式 | 方法（`__init__`）后不可变 |
| I-UDS-6 | 同一客户端实例可多次调用 `chat_completion()` | `_try_uds()` 每次检查连接状态 |

---

## 7. 双路径设计（UDS + HTTP 兜底）

### 7.1 当前实现

`LlmUdsClient` 默认只走 UDS。构造函数中内置了 HTTP 兜底逻辑声明（注释），但实际实现中**没有自动降级到 HTTP**。

```python
# 当前行为：
if not transport.connected:
    raise LlmUdsError("UDS not available")
```

### 7.2 兜底逻辑设计（待实现）

如果未来需要实现降级，链路如下：

```
chat_completion()
    ├── _try_uds()      ← 尝试 UDS
    │   └── 成功 → _complete_uds()
    └── 失败 → HTTP 兜底（通过 OpenAiCompatibleClient）
```

> 注意：当前路由决策在 `create_llm_client()` 工厂中完成 —— 模型名以 `aisf/` 开头就走 `LlmUdsClient`，不以 `aisf/` 开头就走 HTTP。如果需要在 UDS 不可用时自动切 HTTP，需要重新设计路由逻辑（或由调用方自行处理）。

---

## 8. 配置项

### 8.1 构造函数参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `sock_path` | `str` | `""` → 回退到环境变量 | UDS socket 路径 |
| `model` | `str` | `""` → 回退到环境变量链 | 模型名 |

### 8.2 环境变量

| 变量 | 优先级 | 默认值 | 说明 |
|------|--------|--------|------|
| `AF_UDS_SOCKET_PATH` | sock_path 替代 | `/tmp/agentflow_plugin.sock` | UDS socket 路径 |
| `SYS_AGENT_LLM_MODEL` | 最高 | — | UDS 服务端模型名 |
| `AF_LLM_MODEL` | 次高 | — | 通用模型名 |
| `ANTHROPIC_MODEL` | 最低 | — | 遗留兼容 |

---

## 9. 调用详细（一次完整通信的运行时视图）

```
LlmUdsClient.chat_completion(messages, tools)
    │
    ├─ 1. 从 messages 中提取 system prompt 和 user query
    │
    ├─ 2. 构造 uds_request = {intentions, prompt, _messages, tools}
    │
    ├─ 3. _try_uds()
    │      ├─ _UdsTransport(sock_path)
    │      ├─ connect() → socket.connect()
    │      └─ 成功：self._transport = transport
    │
    ├─ 4. _complete_uds(query, prompt, uds_request)
    │      │
    │      ├─ 4a. req_id = _next_req_id()  → "py-1-xxxxxxxx"
    │      │
    │      ├─ 4b. llm.task.create
    │      │      ├─ 构造 create_payload
    │      │      ├─ t.send_json(create_payload)
    │      │      ├─ t.recv_json() → 解析 task_id
    │      │      └─ 提取 task_id，失败则抛异常
    │      │
    │      ├─ 4c. llm.stream
    │      │      ├─ 构造 req_body（合并透传字段）
    │      │      ├─ 构造 stream_payload {task_id, request, model}
    │      │      ├─ t.send_json(stream_payload)
    │      │      ├─ t.recv_json() → 检查 error 字段
    │      │      └─ 正常 → 继续
    │      │
    │      ├─ 4d. _receive_events(t)
    │      │      ├─ recv_json() → llm_token → 累加
    │      │      ├─ recv_json() → llm_token → 累加
    │      │      ├─ recv_json() → llm_final → return (full, final)
    │      │      └─ 循环直到 llm_final 或 llm_error
    │      │
    │      └─ 4e. finally: llm.task.destroy (best-effort)
    │
    └─ 5. 返回 OpenAI 格式响应 {choices: [{message: {content: ans}}]}
```

---

## 10. 集成边界

### 客户端职责（LlmUdsClient）

| 职责 | 说明 |
|------|------|
| 建立/维护 UDS 连接 | 每次 `chat_completion()` 按需连接 |
| 全量透传请求体 | `_messages` + `tools` 必须携带 |
| 三段式协议编排 | create → stream → destroy 顺序保证 |
| 事件流解析 | 区分 llm_token / llm_final / llm_error |
| 错误分级 | 连接级、协议级、LLM 级分别处理 |
| 超时控制 | socket 级 300s 读写超时 |

### 服务端职责（UDS Plugin Server）

| 职责 | 说明 |
|------|------|
| 监听 UDS socket | accept 客户端连接 |
| 协议解析 | 解析 llm.task.create / stream / destroy |
| 模型调用 | 转发给 IMInferServiceClient → HTTP/2 → 推理服务 |
| 流式推送 | 通过 send_event 推送 llm_token / llm_final / llm_error |
| 任务生命周期管理 | 维护 task_id 状态，清理过期任务 |

### 非职责（客户端不关心的）

- 服务端如何解析 `tools` 字段（当前仅解析 `query` + `prompt`）
- 服务端使用什么推理引擎
- 服务端超时策略
- 服务端内部重试/容错

---

## 11. 不变量

| ID | 不变量 | 验证方式 |
|----|--------|---------|
| I-UDS-1 | create → stream → destroy 三段顺序严格保证 | 代码结构 |
| I-UDS-2 | stream 成功后才进入事件接收循环 | `recv_json()` 后检查 `error` |
| I-UDS-3 | destroy 在 finally 块执行，不因异常跳过 | 代码结构 |
| I-UDS-4 | chat_completion 返回值不含 UDS 协议细节 | 返回值仅为 OpenAI 兼容 dict |
| I-UDS-5 | 服务端缺 task_id 时提前抛错，不继续 stream | `_extract_task_id()` 检查 |
| I-UDS-6 | 请求体全量透传，不因服务端实现裁剪 | `uds_request` 构造逻辑 |
| I-UDS-7 | 同一 transport 实例可多次 send/recv | `_UdsTransport` 无状态限制 |
| I-UDS-8 | recv_json 支持任意长度帧（≤10MB） | `while remaining > 0` 循环 |
| I-UDS-9 | 每次请求响应可按 request_id 配对，并发时不串线 | `_chat` / `_receive_chat_events` recv 后校验 |
| I-UDS-10 | 流式循环只在自己 `request_id` 的 `llm_final` / `llm_error` 时退出，不丢帧 | `continue` 跳过不匹配帧 |

---

## 12. 演进路线

| 阶段 | 内容 | 优先级 | 状态 |
|------|------|--------|------|
| P0 | 基础 UDS 传输层（`_UdsTransport`） | P0 | ✅ 已完成 |
| P1 | 三段式协议（create → stream → destroy） | P0 | ✅ 已完成 |
| P2 | `chat_completion()` 全量透传（`_messages` + `tools`） | P0 | ✅ 已完成 |
| P3 | 完整的错误场景处理 | P1 | ✅ 已完成 |
| P4 | 服务端支持解析 `tools` 字段（服务端侧任务） | P1 | 📋 待完成 |
| P5 | UDS 不可用时自动降级到 HTTP 兜底 | P2 | 📋 待评估 |
| P6 | 单连接复用（keep-alive，避免每次 `chat_completion` 重连） | P2 | 📋 待评估 |

---

## 13. 参考

- [LLM Client Provider 集成设计](./llm-client-provider-integration.md) — 上层架构与路由
- [`llm_uds_client.py`](../../../examples/agent/outfit-assistant/nodes/common/clients/llm_uds_client.py) — 实现源码
- [`base.py`](../../../examples/agent/outfit-assistant/nodes/common/clients/base.py) — 抽象接口与工厂
- [`UDSPluginServer`](../../../../vendor/SysAgent/src/sys_agent/UDSPluginServer.cpp) — 服务端实现
