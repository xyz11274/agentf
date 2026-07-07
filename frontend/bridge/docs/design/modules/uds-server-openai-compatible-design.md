# UDS Server 改造：支持 OpenAI 兼容格式

> 领域：技术设计
> 状态：**定稿**
> 最后更新：2026-07-07
> 关联文档：`llm-uds-client-design.md`、`llm-client-provider-integration.md`
> 后端服务：`vendor/SysAgent/src/sys_agent/UDSPluginServer.cpp`、`IMInferServiceClient.cpp`

---

## 1. 改造目标

将 UDS 通道改造成 **OpenAI Chat Completions 格式的透明代理**，使 `LlmUdsClient` 能平替 `OpenAiCompatibleClient`，UDS 和 HTTP 两条路径对外暴露完全一致的接口语义。

### 核心原则

- UDS 是**传输封装层**，不是协议限制。请求/响应格式应与 OpenAI API 等价
- 客户端代码不感知后端是 FusionMind / DashScope / 其他推理引擎
- 保持向后兼容：现有只传 `intentions` + `prompt` 的客户端仍能工作

---

## 2. 现状分析

### 2.1 FusionMind 提供的端点

FusionMind 已经提供了两个接口：

| 端点 | 说明 |
|------|------|
| `/infermind/llm/v1/chat/openai/completions` | **标准的 OpenAI 规范接口**，完全兼容 OpenAI Chat Completions 格式 |
| `/infermind/llm/v1/chat/completions` | 也支持 tools，但格式与 OpenAI 稍有不同（当前 SysAgent 使用的端点） |

**关键结论**：FusionMind 本身已经支持 OpenAI 格式，改造不需要改推理侧。

### 2.2 数据流瓶颈

```
LlmUdsClient                           UDSPluginServer               IMInferServiceClient
─────────────────                      ────────────────               ────────────────────
chat_completion(messages, tools)        HandleLLMStream()              BuildRequestBody()
  │                                       │                              │
  ├─ intentions[0].query  ───────────►    ├─ query  ───────────────►     ├─ userInput
  ├─ prompt[0]           ───────────►    ├─ prompt ───────────────►     ├─ sysPrompt
  ├─ _messages           ───────────►    └─ ✗ 忽略                       └─ contextInfo ← 支持但没被用
  ├─ tools               ───────────►    (完全丢弃)
  └─ max_completion_tokens               (完全丢弃)
```

**三层都有瓶颈，但根因在 UDSPluginServer 层**：`IMInferServiceClient::BuildRequestBody()` 底层已经支持完整消息 + `contextInfo`（含 `tool_calls`、`tool_call_id`），但 UDSPluginServer 只传了 `query` + `prompt` 过去。

### 2.3 SSE 响应解析瓶颈

```
FusionMind SSE 响应格式：
  data: {"result": {"choices": [{"message": {"content": "你好"}}]}}
  data: {"result": {"choices": [{"delta": {"content": "你"}, "finish_reason": null}]}}
  data: [DONE]

当前 ParseSingleSSEChunk 只提取：
  result.choices[0].message.content     ← 非流式结构
  ↓ 不解析：
  - delta.content                       ← 流式结构
  - delta.tool_calls                    ← function calling
  - finish_reason                       ← stop/length/tool_calls
  - usage                               ← token 统计
```

---

## 3. 改造方案

### 3.1 整体架构（目标态）

```
LlmUdsClient                              UDSPluginServer               IMInferServiceClient
─────────────────                         ────────────────               ────────────────────
chat_completion(messages, tools, ...)       HandleLLMStream()              ForwardOpenAI()
  │                                          │                              │
  └─ request body (OpenAI 格式) ──────────►  └─ 透传 request ──────────►   ├─ POST /openai/completions
     {                                        body 到                     ├─ 解析完整 SSE
       "messages": [...],                     IMInferServiceClient         │  - delta.content
       "tools": [...],                        新方法                       │  - delta.tool_calls
       "tool_choice": "auto",                                              │  - finish_reason
       "max_completion_tokens": 8192,                                      │  - usage
       "temperature": 0.7,                                                └─ 回调推送完整事件
       "stream": true
     }
```

### 3.2 改造分解

#### 3.2.1 IMInferServiceClient：新增 `ForwardOpenAI()` 方法

**现有问题**：`BuildRequestBody()` + `InvokeStream()` 组合已能构造完整请求体，但：
- 请求被 UDSPluginServer 掐断了（只传了 `query` + `prompt`）
- SSE 解析只取 `message.content` 一个字段
- 使用 `/infermind/llm/v1/chat/completions` 而非 OpenAI 标准端点

**改造**：

```cpp
// 新增：直接接收完整 OpenAI 请求体，透传给 FusionMind
struct OpenAIStreamResult {
    std::string content;        // delta.content / message.content
    std::string finish_reason;  // "stop" | "length" | "tool_calls" | ...
    std::vector<OpenAIToolCall> tool_calls;  // delta.tool_calls
    struct Usage {
        int prompt_tokens;
        int completion_tokens;
        int total_tokens;
    } usage;
    bool is_error;
    std::string error_message;
};

// 流式回调增强：传递完整结果
using OpenAIStreamCallback = std::function<void(const OpenAIStreamResult& result)>;

// 新增方法：直接透传 OpenAI 格式请求体
bool ForwardOpenAIStream(
    const std::string& request_body_json,     // 完整的 OpenAI 请求体
    const OpenAIStreamCallback& on_event,
    uint32_t& error_code
);

// 新增方法：直接透传同步请求
std::string ForwardOpenAIComplete(
    const std::string& request_body_json,
    int timeout_seconds
);
```

**SSE 解析改造**（`ParseSingleOpenAIChunk`）：

当前 FusionMind 返回的 SSE 结构可能是两种格式之一：

```
// OpenAI 标准流式格式（delta）
data: {"choices":[{"delta":{"content":"你好"},"finish_reason":null,"index":0}]}
data: {"choices":[{"delta":{"tool_calls":[{"id":"call_xxx","type":"function","function":{"name":"get_weather","arguments":"{\"loc\":\"北京\"}"}}]},"finish_reason":null,"index":0}]}
data: {"choices":[{"delta":{},"finish_reason":"tool_calls","index":0}]}
data: [DONE]

// FusionMind 包装格式（message，可能同时存在）
data: {"result":{"choices":[{"message":{"content":"你好"},"finish_reason":"stop","index":0}]}}

// 同步响应格式
data: {"result":{"choices":[{"message":{"content":"你好"},"finish_reason":"stop","index":0}],"usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}}}
data: [DONE]
```

需要同时兼容这两种格式，提取完整字段：

```cpp
OpenAIStreamResult ParseOpenAIChunk(const std::string& sse_line) {
    OpenAIStreamResult result;
    // 1. 跳过 data: 前缀
    // 2. 检查 [DONE]
    // 3. 解析 JSON
    // 4. 查找 result.choices[0] 或 choices[0]
    //    - delta.content 或 message.content
    //    - delta.tool_calls 或 message.tool_calls
    //    - finish_reason
    // 5. 查找 usage
    return result;
}
```

#### 3.2.2 UDSPluginServer：透传完整请求体

**`HandleLLMStream()` 改造**：

```cpp
std::string UDSPluginServer::HandleLLMStream(const std::string& params_json, int client_fd) {
    // ... 解析 task_id、校验任务 ...
    
    // 从 params.request 中取完整请求体
    Document doc;
    doc.Parse(params_json.c_str());
    
    std::string request_body_str;
    if (doc.HasMember("request") && doc["request"].IsObject()) {
        // 直接序列化整个 request 对象为 JSON 字符串
        request_body_str = ValueToString(doc["request"]);
    }
    
    // 调用新的 ForwardOpenAIStream，传入完整 request body
    uint32_t error_code = 0;
    bool success = IMInferServiceClient::Instance()->ForwardOpenAIStream(
        request_body_str,
        [this, client_fd, task_id](const OpenAIStreamResult& event) {
            // 构造事件推回给 UDS 客户端
            
            // llm_token: content 非空时
            if (!event.content.empty()) {
                SendEvent(client_fd, {event_type: "llm_token", data_json: {token: event.content}});
            }
            
            // tool_calls 事件：tool_calls 非空时
            if (!event.tool_calls.empty()) {
                SendEvent(client_fd, {event_type: "llm_tool_calls", data_json: {tool_calls: event.tool_calls}});
            }
            
            // llm_final: finish_reason 非空时
            if (!event.finish_reason.empty() || event.usage.total_tokens > 0) {
                Json::Value data;
                data["answer"] = event.content;
                data["finish_reason"] = event.finish_reason;
                data["usage"] = event.usage;  // {prompt_tokens, completion_tokens, total_tokens}
                SendEvent(client_fd, {event_type: "llm_final", data_json: data});
            }
            
            // llm_error
            if (event.is_error) {
                SendEvent(client_fd, {event_type: "llm_error", data_json: {error_message: event.error_message}});
            }
        },
        error_code
    );
    
    // ...
}
```

**`HandleLLMComplete()` 改造**（非流式路径）：

```cpp
std::string UDSPluginServer::HandleLLMComplete(const std::string& params_json) {
    // 提取 request 对象，作为完整 OpenAI 请求体
    // 调用 ForwardOpenAIComplete
    // 返回完整响应：content + finish_reason + tool_calls + usage
}
```

**向后兼容**：如果 request 中没有 `messages` 字段但有 `intentions` + `prompt`（老格式），则走原来的 `SyncInvokeComplete` / `InvokeStream` 路径。

#### 3.2.3 LlmUdsClient：完整 OpenAI 请求体

**`chat_completion()` 改造**：

```python
def chat_completion(
    self,
    messages: list[dict[str, Any]],
    *,
    tools: list[dict[str, Any]] | None = None,
    max_completion_tokens: int = 8192,
    stream: bool = False,
    on_chunk: Callable[[str], None] | None = None,   # ★ 新增
) -> dict[str, Any]:
    # 构造完整的 OpenAI 格式请求体
    request_body = {
        "messages": messages,
    }
    if tools:
        request_body["tools"] = tools
        request_body["tool_choice"] = "auto"
    if max_completion_tokens:
        request_body["max_completion_tokens"] = max_completion_tokens
    if stream:
        request_body["stream"] = True
    
    # 直接透传完整 request_body
    return self._complete_uds(request_body, stream, on_chunk)
```

**`_complete_uds()` 改造**：

```python
def _complete_uds(self, request_body: dict, stream: bool, on_chunk=None) -> dict:
    # ...
    # llm.stream 的 request 直接就是完整的 OpenAI 请求体
    stream_payload["params"]["request"] = request_body
    
    t.send_json(stream_payload)
    
    if stream:
        # 流式：事件循环 + on_chunk 回调
        return self._receive_events(t, on_chunk)
    else:
        # 非流式：等最终响应
        return self._receive_complete(t)
```

**事件解析改造**（`_receive_events`）：

```python
def _receive_events(self, t: _UdsTransport, on_chunk=None) -> dict:
    full_answer = ""
    tool_calls_acc: dict[int, dict] = {}
    finish_reason = "stop"
    usage = None
    
    while True:
        event = t.recv_json()
        event_type = event.get("event_type", "")
        data = event.get("data", {})
        if isinstance(data, str):
            data = json.loads(data)
        
        if event_type == "llm_token":
            token = data.get("token", "")
            full_answer += token
            if on_chunk and token:
                try:
                    on_chunk(token)
                except Exception:
                    pass
        
        elif event_type == "llm_tool_calls":
            # 累加 tool_calls delta
            for tc in data.get("tool_calls", []):
                idx = tc.get("index", 0)
                if idx not in tool_calls_acc:
                    tool_calls_acc[idx] = {
                        "id": tc.get("id", ""),
                        "type": tc.get("type", "function"),
                        "function": {"name": "", "arguments": ""},
                    }
                existing = tool_calls_acc[idx]
                if tc.get("id"):
                    existing["id"] = tc["id"]
                fn = tc.get("function", {})
                if fn.get("name"):
                    existing["function"]["name"] = fn["name"]
                if fn.get("arguments"):
                    existing["function"]["arguments"] += fn["arguments"]
        
        elif event_type == "llm_final":
            finish_reason = data.get("finish_reason", "stop")
            usage = data.get("usage")
            # 如果 llm_final 带有 answer 且没有累加过
            answer = data.get("answer", "")
            if answer and not full_answer:
                full_answer = answer
        
        elif event_type == "llm_error":
            raise LlmUdsError(f"LLM error: {data.get('error_message', 'unknown')}")
    
    # 构造 OpenAI 兼容响应
    message = {"content": full_answer, "role": "assistant"}
    if tool_calls_acc:
        message["tool_calls"] = list(tool_calls_acc.values())
    result = {
        "choices": [{"message": message, "finish_reason": finish_reason, "index": 0}],
    }
    if usage:
        result["usage"] = usage
    return result
```

---

## 4. 协议变更：新事件格式

### 4.1 新增事件 `llm_tool_calls`

```json
{
  "event_type": "llm_tool_calls",
  "data": {
    "tool_calls": [
      {
        "index": 0,
        "id": "call_xxx",
        "type": "function",
        "function": {
          "name": "get_weather",
          "arguments": "{\"location\": \"北京\"}"
        }
      }
    ]
  }
}
```

对同一个 task，服务端可能推送多条 `llm_tool_calls` 事件（逐 token 累加 arguments），客户端按 `index` 累加。

### 4.2 增强事件 `llm_final`

```json
{
  "event_type": "llm_final",
  "data": {
    "answer": "北京今天25度",
    "finish_reason": "stop",
    "usage": {
      "prompt_tokens": 120,
      "completion_tokens": 35,
      "total_tokens": 155
    }
  }
}
```

### 4.3 向后兼容

旧版客户端（只关心 `token` 和 `answer`）不受影响：
- `llm_tool_calls` → 忽略（switch 默认分支已忽略未知事件）
- `llm_final` 新增 `finish_reason` / `usage` → 旧客户端不读这些字段
- `llm_token` 不变

---

## 5. 事件流对比（改造前后）

### 改造前：纯文本

```
服务端 → 客户端：
  {"event_type":"llm_token",     "data":{"token":"你"}}
  {"event_type":"llm_token",     "data":{"token":"好"}}
  {"event_type":"llm_final",     "data":{"answer":"你好！"}}
```

### 改造后：完整 OpenAI 格式

```
服务端 → 客户端：
  {"event_type":"llm_token",       "data":{"token":"你"}}
  {"event_type":"llm_token",       "data":{"token":"好"}}
  {"event_type":"llm_tool_calls",  "data":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"get_weather","arguments":"{\"location\":\""}}]}}
  {"event_type":"llm_tool_calls",  "data":{"tool_calls":[{"index":0,"function":{"arguments":"北京\"}"}}]}}
  {"event_type":"llm_final",       "data":{"answer":"","finish_reason":"tool_calls","usage":{"prompt_tokens":120,"completion_tokens":35,"total_tokens":155}}}
```

---

## 6. 向后兼容策略

| 场景 | 处理 |
|------|------|
| 旧客户端 + 旧服务端 | ✅ 不变，三段式协议 + 纯文本回答 |
| 旧客户端 + 新服务端 | ✅ 客户端不读 `tool_calls`/`usage`，纯文本路径正常工作 |
| 新客户端 + 旧服务端 | ⚠️ 客户端发送 `messages` 格式，旧服务端只读 `intentions[0].query` → 退化为老路径 |
| 新客户端 + 新服务端 | ✅ 完整 OpenAI 兼容 |

**UDSPluginServer 的双路径识别**：

```cpp
if (request.HasMember("messages")) {
    // 新格式：完整 OpenAI 请求体
    ForwardOpenAIStream(ValueToString(request), callback, error_code);
} else if (request.HasMember("intentions")) {
    // 旧格式：只取 query + prompt
    query = request["intentions"][0]["query"];
    prompt = request["prompt"][0];
    InvokeStream(task_id, query, prompt, error_code);
}
```

---

## 7. 影响范围

| 组件 | 改动量 | 说明 |
|------|--------|------|
| `IMInferServiceClient` | **中**（~150 行） | 新增 `ForwardOpenAIStream()` + `ForwardOpenAIComplete()` + `ParseOpenAIChunk()`；新增 OpenAI 端点常量 |
| `UDSPluginServer` | **中**（~100 行） | `HandleLLMStream()` 增加新路径；`HandleLLMComplete()` 增加新路径；新增 `llm_tool_calls` 事件发送 |
| `UDSPluginServer.h` | **小**（~10 行） | 可能需要新增回调类型 `OpenAIStreamCallback` 的 typedef |
| `LlmUdsClient` | **中**（~120 行） | `chat_completion()` 改构造完整 request body；`_receive_events()` 改解析 `tool_calls` + `usage`；新增 `on_chunk` 参数 |
| `_UdsTransport` | **无** | 传输层不变，仍是 4-byte 长度前缀帧 |

---

## 8. 实施步骤

| 步骤 | 内容 | 验证方式 |
|------|------|---------|
| 1 | `IMInferServiceClient` 新增 `ForwardOpenAIStream()`，直接接收完整 JSON 请求体透传给 FusionMind | 单测：伪造 HTTP 响应验证解析正确性 |
| 2 | `IMInferServiceClient` 新增 `ParseOpenAIChunk()`，解析 `delta.content` / `delta.tool_calls` / `finish_reason` / `usage` | 单测：各种 SSE 样本解析 |
| 3 | `UDSPluginServer::HandleLLMStream()` 增加新分支：检测 `messages` 字段存在时走新路径 | 集成测试：Mock 客户端发送 messages 格式 |
| 4 | `UDSPluginServer::HandleLLMComplete()` 增加新分支：同步完整 OpenAI 请求 | 集成测试 |
| 5 | `LlmUdsClient::chat_completion()` 改构造完整 OpenAI 请求体 | 单测：验证请求体结构 |
| 6 | `LlmUdsClient._receive_events()` 改解析 `llm_tool_calls` + `usage` | 单测：Mock 事件流解析 |
| 7 | `LlmUdsClient` 新增 `on_chunk` 回调参数 | 单测：验证回调是否触发 |
| 8 | E2E 测试：UDS 通道完整 OpenAI 兼容调用 | mock FusionMind 服务 |
