# FusionMind 本地 Mock Server 设计

> 层级：L4 技术设计
> 状态：草稿
> 最后更新：2026-07-07
> 关联文档：`uds-server-openai-compatible-design.md`、`llm-uds-client-design.md`、`llm-client-provider-integration.md`、`streaming-response.md`
> 相关实现：`vendor/SysAgent/src/sys_agent/IMInferServiceClient.cpp`、`vendor/SysAgent/include/sys_agent/IMInferServiceClient.h`

---

## 1. 概述

FusionMind Mock Server 是一个轻量级 HTTP 服务，模拟 FusionMind 推理引擎的 HTTP API，用于本地开发、集成测试和 CI 环境。

### 设计目标

- 提供与真实 FusionMind **语义等价**的 HTTP 推理接口
- 支持流式（SSE）和非流式两种调用模式
- 支持 OpenAI 标准格式和 FusionMind 遗留格式两套端点
- 可配置响应内容（echo / fixed / template / tool_calls）
- 支持延迟模拟、错误注入、tool_calls 模拟

### 与 mock_uds_model_server 的分工

`mock_uds_model_server` 模拟 UDS 协议层（替代 UDSPluginServer），FusionMind Mock 模拟 HTTP 推理层。
由于项目中已有 `vendor/SysAgent/src/sys_agent/UDSPluginServer.cpp` 完整源码，通常直接编译运行
真实 C++ UDSPluginServer + FusionMind Mock 即可覆盖全链路，无需 Python UDS mock 介入。
`mock_uds_model_server` 仅在需快速调试 LlmUdsClient 时作为轻量替代。

| 维度 | mock_uds_model_server | FusionMind Mock Server（本设计） |
|------|----------------------|--------------------------------|
| 协议层 | UDS (Unix Domain Socket) | HTTP |
| 模拟对象 | UDSPluginServer（UDS 传输层） | FusionMind（HTTP 推理服务）|
| 核心使用场景 | 快速调试 `LlmUdsClient`（可选） | 测试 `IMInferServiceClient` / `OpenAiCompatibleClient` |
| 独立运行 | ✅ | ✅ |

### 适用场景

| 场景 | 说明 |
|------|------|
| 测试 `IMInferServiceClient::ForwardOpenAIStream()`（C++ 新路径） | 让 C++ 端通过 HTTP 连 mock，验证 SSE 解析正确性 |
| 测试 `OpenAiCompatibleClient`（Python HTTP 路径） | 脱离真实 API Key，测试流式/非流式/重试逻辑 |
| 测试 function calling 全链路 | 模拟 delta.tool_calls 逐帧推送，验证累加逻辑 |
| 测试超时/错误恢复 | error_rate / http_error / timeout_delay 注入 |

---

## 2. 模拟的 FusionMind 端点

### 2.1 OpenAI 标准接口

```
POST /infermind/llm/v1/chat/openai/completions
```

完全兼容 OpenAI Chat Completions API 格式。这是 `uds-server-openai-compatible-design.md` §3.2.1 中 `ForwardOpenAIStream()` 方法的目标端点。

**请求体**（与 OpenAI 一致）：

```json
{
  "model": "aisf/deepseek-v4-flash",
  "messages": [
    {"role": "system", "content": "你是一个穿搭顾问。"},
    {"role": "user", "content": "今天穿什么？"}
  ],
  "tools": [{
    "type": "function",
    "function": {
      "name": "get_weather",
      "description": "查询天气",
      "parameters": {
        "type": "object",
        "properties": {"city": {"type": "string"}},
        "required": ["city"]
      }
    }
  }],
  "tool_choice": "auto",
  "max_completion_tokens": 8192,
  "temperature": 0.7,
  "stream": true
}
```

### 2.2 FusionMind 遗留推理接口

```
POST /infermind/llm/v1/chat/completions
```

当前 `IMInferServiceClient::BuildRequestBody()` 使用的端点。支持 tools 但格式与 OpenAI 稍有不同（`uds-server-openai-compatible-design.md` §2.1）。

---

## 3. 流式协议（SSE）

### 3.1 标准格式

两套端点均输出标准 SSE 流：

```
data: {"choices":[{"delta":{"content":"你"},"finish_reason":null,"index":0}]}

data: {"choices":[{"delta":{"content":"好"},"finish_reason":null,"index":0}]}

data: {"choices":[{"delta":{},"finish_reason":"stop","index":0}]}

data: [DONE]
```

### 3.2 支持的数据字段

| 字段 | 位置 | 说明 |
|------|------|------|
| `delta.content` | `choices[0].delta.content` | 流式 token 内容 |
| `delta.tool_calls` | `choices[0].delta.tool_calls` | function calling delta（逐 token 累加 arguments） |
| `message.content` | `choices[0].message.content` | 非流式/最终完整内容 |
| `message.tool_calls` | `choices[0].message.tool_calls` | 非流式工具调用 |
| `finish_reason` | `choices[0].finish_reason` | `"stop"` / `"length"` / `"tool_calls"` |
| `usage` | `usage` | token 统计（非流式或最终帧） |

### 3.3 兼容双格式

模拟的 SSE 输出应兼容两种结构（`uds-server-openai-compatible-design.md` §3.2.1）：

```
// OpenAI 标准流式格式（delta）
data: {"choices":[{"delta":{"content":"你好"},"finish_reason":null,"index":0}]}

// FusionMind 包装格式（result 外层）
data: {"result":{"choices":[{"message":{"content":"你好"},"finish_reason":"stop","index":0}]}}
```

mock server 默认使用 OpenAI 标准格式（`delta`），可通过配置切换到 `result` 包装格式。

---

## 4. 整体架构

### 4.1 模块结构

```
mocks_fusionmind/
├── __init__.py
├── server.py                      # HTTP 服务入口（aiohttp / FastAPI）
├── handler_openai.py              # OpenAI 端点处理器
├── handler_legacy.py              # 遗留端点处理器
├── response_builder.py            # Mock 响应生成器（复用 MockResponseBuilder 设计）
├── sse_formatter.py               # SSE 格式化和发送
└── config.py                      # 配置模型
```

### 4.2 数据流

```
客户端（OpenAiCompatibleClient / IMInferServiceClient）
  │
  │ HTTP POST /infermind/llm/v1/chat/openai/completions
  │ {messages, tools, stream: true/false, ...}
  ▼
FusionMind Mock Server
  │
  ├─ route → handler_openai.py
  │     │
  │     ├─ 解析请求体
  │     │   ├─ messages → 提取 system prompt + user content
  │     │   ├─ tools → 检测是否需要响应 tool_calls
  │     │   └─ stream → 决定输出模式
  │     │
  │     ├─ response_builder.py
  │     │   └─ build_response(request, config) → 生成 mock 响应计划
  │     │
  │     ├─ [stream=true] sse_formatter.py
  │     │   └─ 逐 token 发送 SSE data: 行
  │     │
  │     └─ [stream=false] 直接返回完整 JSON 响应
  │
  └─ route → handler_legacy.py
        └─ 类似处理，但使用遗留格式
```

### 4.3 真实 UDSPluginServer + FusionMind Mock（推荐路径）

项目中已有 `vendor/SysAgent/src/sys_agent/UDSPluginServer.cpp` 完整源码，推荐直接编译运行
真实 UDSPluginServer，将其 HTTP 调用目标指向 FusionMind Mock 即可覆盖全链路：

```
LlmUdsClient (Python)
  → UDS socket
  → UDSPluginServer (vendor C++, 编译运行)
  → IMInferServiceClient (vendor C++)
  → HTTP → FusionMind Mock (localhost:8085)
```

与 `mock_uds_model_server` 的串联**不做默认支持**——如测试中需要 Python UDS mock + FusionMind Mock
两级串联，可自行启动两个独立进程，FusionMind Mock 不包含对此的内置编排逻辑。

---

## 5. 响应生成器设计

### 5.1 配置文件

```jsonc
{
  // 全局配置
  "host": "0.0.0.0",
  "port": 8085,
  "model": "aisf/deepseek-v4-flash",

  // 响应模式
  "mode": "echo",             // echo | fixed | template | tool_calls | tool_call_stream
  "response": "这是一条 mock 回复。",  // fixed 模式时使用

  // 流式参数
  "stream": true,             // 默认是否流式
  "token_delay": 0.02,        // 每 token 延迟（秒）
  "completion_delay": 1.0,    // 非流式响应延迟（秒）

  // 错误注入
  "error_rate": 0.0,          // 错误概率（0.0 ~ 1.0）
  "error_code": 500,          // 错误时返回的 HTTP 状态码
  "error_message": "Mock internal error",

  // 模拟 usage
  "mock_usage": true,
  "prompt_tokens_min": 50,
  "prompt_tokens_max": 200,
  "completion_tokens_min": 10,
  "completion_tokens_max": 100,

  // Tool calls（tool_call_stream 模式时使用）
  "tools": [
    {
      "name": "get_weather",
      "arguments": {"city": "北京"},
      "output": "北京今天25度，晴"
    }
  ],

  // 响应模板（template 模式时使用）
  "templates": [
    {"pattern": ".*天气.*", "response": "北京今天25度，晴"},
    {"pattern": ".*你好.*", "response": "你好！我是穿搭顾问。"},
    {"pattern": ".*",       "response": "收到你的查询（根据意图规划中…）"}
  ],

  // SSE 格式开关
  "use_result_wrapper": false  // true = 使用 FusionMind 的 result 包装格式
}
```

### 5.2 响应模式

| 模式 | 行为 | 流式支持 | 适用场景 |
|------|------|---------|---------|
| `echo` | 回显用户的最后一条消息 | ✅ | 基础连通性测试 |
| `fixed` | 返回固定的 mock 文本 | ✅ | 预期响应测试 |
| `template` | 按正则匹配模板库，返回匹配的响应 | ✅ | 多意图场景模拟 |
| `tool_calls` | 模拟 LLM 调用工具（非流式）| ❌ | tool calling 全链路测试 |
| `tool_call_stream` | 逐 delta 推送 tool_calls（流式）| ✅ | 流式 tool calling 测试 |
| `slow` | 极慢响应（测试超时处理）| ✅ | 超时/取消测试 |

### 5.3 Tool Calls 模拟

**非流式**（`mode: "tool_calls"`）：

```json
{
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": null,
      "tool_calls": [
        {
          "id": "call_mock_001",
          "type": "function",
          "function": {
            "name": "get_weather",
            "arguments": "{\"city\": \"北京\"}"
          }
        }
      ]
    },
    "finish_reason": "tool_calls"
  }],
  "usage": {
    "prompt_tokens": 50,
    "completion_tokens": 15,
    "total_tokens": 65
  }
}
```

**流式**（`mode: "tool_call_stream"`）——逐帧推送 delta：

```
# 帧 1：tool_call 开始 (id + name)
data: {"choices":[{"delta":{"role":"assistant","content":null,"tool_calls":[{"index":0,"id":"call_mock_001","type":"function","function":{"name":"get_weather","arguments":""}}]},"finish_reason":null,"index":0}]}

# 帧 2-N：逐字符 arguments
data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"cit"}}]},"finish_reason":null,"index":0}]}

data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"y\": \"北"}}]},"finish_reason":null,"index":0}]}

data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"京\"}"}}]},"finish_reason":null,"index":0}]}

# 最后一帧：finish_reason = tool_calls
data: {"choices":[{"delta":{},"finish_reason":"tool_calls","index":0}]}

data: [DONE]
```

### 5.4 错误注入

| 配置 | 行为 | 客户端表现 |
|------|------|-----------|
| `error_rate: 0.1` | 10% 概率在流式中间推送 SSE error | 触发 `llm_error` 事件 |
| `http_error: 429` | 立即返回 HTTP 429 | 触发客户端 4xx 不重试逻辑 |
| `http_error: 500` | 立即返回 HTTP 500 | 触发服务端错误处理 |
| `timeout_delay: 999` | 不响应，等待客户端超时 | 触发 `socket.timeout` |

---

## 6. 快速启动 CLI

```bash
# 默认模式（echo + stream）
python -m frontend.mocks.mocks_fusionmind

# 指定端口和模式
python -m frontend.mocks.mocks_fusionmind --port 8085 --mode fixed --response "你好！"

# 使用配置文件
python -m frontend.mocks.mocks_fusionmind --config config.json

# 非流式
python -m frontend.mocks.mocks_fusionmind --stream false

# 模拟 tool calls
python -m frontend.mocks.mocks_fusionmind --mode tool_call_stream

# 错误注入
python -m frontend.mocks.mocks_fusionmind --error-rate 0.3

# 与真实模型相同的延迟（约 0.05s/token）
python -m frontend.mocks.mocks_fusionmind --token-delay 0.05
```

---

## 7. 与 OpenAiCompatibleClient 的适配

`OpenAiCompatibleClient` 的调用方式：

```python
client = OpenAI(
    base_url=os.environ["AF_LLM_BASE_URL"],  # → http://localhost:8085/infermind/llm/v1/chat
    api_key="mock-key",
)

response = client.chat.completions.create(
    model="aisf/deepseek-v4-flash",
    messages=[...],
    stream=True,
)
```

**环境变量配置**：

```bash
# 指向本地 mock
export AF_LLM_BASE_URL="http://localhost:8085/infermind/llm/v1/chat"
export AF_LLM_API_KEY="mock-key"
export AF_LLM_MODEL="aisf/deepseek-v4-flash"
```

**关键兼容点**：

| 项目 | 官方 OpenAI API | FusionMind Mock | 兼容？ |
|------|----------------|-----------------|--------|
| 端点路径 | `/v1/chat/completions` | `/infermind/llm/v1/chat/openai/completions` | ✅ 客户端需配置 base_url 带路径 |
| SSE 格式 | `data: {"choices":[{"delta":...}]}` | 同上 | ✅ 完全一致 |
| `[DONE]` | ✅ | ✅ | ✅ |
| `tool_calls` | `delta.tool_calls` | 同上 | ✅ |
| Bearer auth | `Authorization: Bearer <key>` | 接受任意 key | ✅ 不做校验 |
| 模型名 | 任意字符串 | 接受任意值 | ✅ |

---

## 8. 健康检查

```
GET /health
```

响应：

```json
{
  "status": "ok",
  "server": "FusionMind Mock Server",
  "version": "1.0.0",
  "config": {
    "mode": "echo",
    "stream": true,
    "token_delay": 0.02
  },
  "stats": {
    "total_requests": 0,
    "active_connections": 0
  }
}
```

---

## 9. 实施计划

| 步骤 | 内容 | 工作量 | 依赖 |
|------|------|--------|------|
| 1 | `server.py` — aiohttp/FastAPI 服务骨架 + CLI arg parse | 小 | 无 |
| 2 | `response_builder.py` — 响应生成器（echo/fixed/template/tool_calls） | 中 | 步骤 1 |
| 3 | `sse_formatter.py` — SSE 格式化（delta/message/result 包装） | 小 | 无 |
| 4 | `handler_openai.py` — OpenAI 端点 handler（stream + non-stream） | 中 | 步骤 2, 3 |
| 5 | `handler_legacy.py` — 遗留端点 handler | 小 | 步骤 2, 3 |
| 6 | `config.py` — 配置模型 + JSON 配置文件加载 | 小 | 无 |
| 7 | 错误注入机制（error_rate / http_error / timeout） | 小 | 步骤 2 |
| 8 | `__init__.py` — 包导出 + `python -m` 入口 | 小 | 步骤 1 |

---

## 10. 不变量

| ID | 不变量 | 验证方式 |
|----|--------|---------|
| I-MOCK-FM-1 | `stream=true` 时响应为 `text/event-stream`，MIME 类型必须正确 | 测试响应头 |
| I-MOCK-FM-2 | 流式 SSE 必须以 `[DONE]` 结尾 | 验证流末尾 |
| I-MOCK-FM-3 | `stream=false` 时响应为 `application/json` | 测试响应头 |
| I-MOCK-FM-4 | `delta.content` 在所有 token 发送完毕后，`finish_reason` 非 null | SSE 序列验证 |
| I-MOCK-FM-5 | `tool_call_stream` 模式下，最后一条 delta 的 `finish_reason` 为 `"tool_calls"` | SSE 序列验证 |
| I-MOCK-FM-6 | `error_rate > 0` 时错误不导致连接关闭（HTTP 200 持续，SSE 内推 error） | 流完整性 |
| I-MOCK-FM-7 | 非流式响应包含 `usage` 字段（当 `mock_usage=true`）| 响应体验证 |

---

## 11. 不做的事

- 不覆盖 UDS 协议（已有 `mock_uds_model_server` 和 `vendor/SysAgent/src/sys_agent/UDSPluginServer.cpp`）
- 不做与 `mock_uds_model_server` 的内置串联编排（需串联时用户自行启动两个独立进程）
- 不做真实模型推理
- 不支持认证/鉴权（mock 接受任意 API Key）
- 不模拟多轮对话状态保持（每次请求独立）
- 不模拟 FusionMind 的负载均衡/限流
