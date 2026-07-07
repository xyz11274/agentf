# LLM Client Provider 集成设计

> 领域：模块设计
> 状态：定稿
> 最后更新：2026-07-07

---

## 1. 概述

本文档描述 `outfit-assistant`（及其他 Agent）的 LLM 调用方式——通过**统一抽象接口**将节点代码与具体的 LLM 后端解耦，支持**两条独立路径**，由模型名前缀决定路由。

### 设计目标

- 节点代码只依赖 `LlmClient` 抽象接口，不依赖具体后端
- 模型名前缀决定路由：`aisf/<model>` → UDS，`<model>` → HTTP
- 两条路径**完全独立，无回退逻辑**
- 两种路径对外暴露完全一致的接口语义

---

## 2. 架构总览

```
┌─────────────────────────────────────────────────────────────────────┐
│                       节点 (coordinator_decide / skill_executor)      │
│   并发安全：同一 agentflowd 下多个 Python 子进程各自独立 LlmUdsClient  │
│                                                                     │
│   from common.clients import create_llm_client                      │
│   client = create_llm_client(model="aisf/deepseek-v4-flash")         │
│   resp = client.chat_completion(messages=[...], tools=[...])        │
└────────────────────────┬────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    LlmClient 抽象接口 (base.py)                       │
│                                                                     │
│   chat_completion(messages, *, tools, max_completion_tokens, stream)│
│   → OpenAI 兼容响应字典                                               │
└────────────────────────┬────────────────────────────────────────────┘
                         │
            ┌────────────┴────────────┐
            │ 模型名前缀路由           │
            │ aisf/ → UDS, 其他 → HTTP│
            └────────────┬────────────┘
                         │
              ┌──────────┴──────────┐
              ▼                     ▼
┌─────────────────────┐  ┌──────────────────────────────────────────────┐
│ OpenAiCompatible    │  │ LlmUdsClient                                  │
│ Client (HTTP)       │  │ (UDS → UDS Server)                            │
│                     │  │                                               │
│ HTTPS POST          │  │ UDS socket: /tmp/agentflow_plugin.sock        │
│ /chat/completions   │  │ 4-byte BE len + UTF-8 JSON 帧                 │
│ SSE 流式 (data:)    │  │ llm.chat 单步协议                             │
│ 重试: 指数退避 3 次 │  │ request_id 配对（防并发串线，§4.5）            │
└────────┬────────────┘  └────┬─────────────────────────────────────────┘
         │                    │ req+req_id
         │                    ▼
         │            ┌──────────────────────────────────────────────────┐
         │            │  UDS Server (vendor C++ UDSPluginServer)         │
         │            │  per-connection 顺序处理                          │
         │            │                                                  │
         │            │  支持方法:                                        │
         │            │    ├── llm.chat（主路径，OpenAI 格式透传）        │
         │            │    ├── tool.list / tool.call                     │
         │            │    └── context.* / databus.*                     │
         │            │                                                  │
         │            │  旧三段式 llm.task.* 已移除（2026-07-07）         │
         │            └────┬─────────────────────────────────────────┘
         │                 │ HTTP POST + SSE
         │                 ▼
         ▼            ┌──────────────────────────────────────────────────┐
┌──────────────────┐  │  IMInferServiceClient (vendor C++ 内部)          │
│ DashScope /        │  │  HTTP 代理层：转发 llm.chat 给推理引擎          │
│ OpenAI /           │  └────┬─────────────────────────────────────────┘
│ 其他 OpenAI 兼容   │       │ HTTPS POST + SSE
│ （可替换外部 LLM） │       ▼
│                    │  ┌──────────────────────────────────────────────────┐
│ 端点: /chat/      │  │  FusionMind / 其他推理引擎（可替换）              │
│        completions │  │  端点: /infermind/llm/v1/chat/                  │
│                    │  │         openai/completions                      │
│ 认证: Bearer key   │  │         completions（遗留格式）                  │
│                    │  │                                                 │
│                    │  │  两种格式：                                     │
│                    │  │    - delta（OpenAI 标准流式）                   │
│                    │  │    - result 包装（FusionMind 遗留）             │
│                    │  │    - message（同步响应）                        │
└──────────────────┘  └──────────────────────────────────────────────────┘
```

### 2.1 关键边说明

| 边 | 协议 | 备注 |
|----|------|------|
| 节点 → LlmClient | Python 函数调用 | 抽象接口，不含网络协议 |
| LlmClient → 路由 | 工厂内决策 | `model.startswith("aisf/")` |
| OpenAiCompatibleClient → DashScope | HTTPS POST + SSE | `data:` 行流式，Bearer auth |
| LlmUdsClient → UDS Server | UDS socket + 4-byte BE len 帧 | 每请求唯一 `request_id`，recv 校验防串线 |
| UDS Server → IMInferServiceClient | vendor C++ 内部转发 | `llm.chat` → HTTP POST |
| IMInferServiceClient → FusionMind | HTTPS POST + SSE | `/infermind/llm/v1/chat/openai/completions` |

### 2.2 并发安全模型

同一 agentflowd 下 `coordinator_decide` 和 `skill_executor` 是**独立 Python 子进程**，各自持有自己的 `_UdsTransport` socket 连到同一 UDS Server：

- **不串线**：每次请求生成唯一 `request_id`（`uuid4` 全局唯一），recv 后校验匹配（详见 `llm-uds-client-design.md` §4.5）
- **不丢帧**：流式循环只在收到自己 `request_id` 的 `llm_final` / `llm_error` 时退出，不匹配的帧用 `continue` 跳过
- **不阻塞**：`UDS_TIMEOUT=300s` socket 级超时兜底，服务端迟迟不回匹配帧时退出而非死循环

### 2.3 UDS 事件回流（数据流闭环）

UDS 通道是**全双工**——请求下行和事件上行在同一 socket 的两个独立方向：

```
LlmUdsClient                              UDS Server
    │                                        │
    │── send_json({request_id, llm.chat}) ──►│
    │                                        │
    │◄── recv_json({request_id, llm_token}) ─┤  ← 事件回流
    │◄── recv_json({request_id, llm_token}) ─┤     (每帧带 request_id)
    │◄── recv_json({request_id, llm_final}) ─┤  ← 收尾帧退出循环
    │                                        │
```

---

## 3. 模型命名规范与路由规则

模型名前缀是路由的唯一依据：

| 前缀 | 路由目标 | 示例 |
|------|---------|------|
| `aisf/` | UDS 通道（`LlmUdsClient`） | `aisf/deepseek-v4-flash` |
| 不带前缀 | HTTP 通道（`OpenAiCompatibleClient`） | `deepseek-v4-flash` |

`_ensure_aisf_model()` 辅助函数用于在需要时添加 `aisf/` 前缀，但不会自动执行——是否加前缀由调用方（用户配置）决定。

### 优先级链（resolve_llm_config）

```
agent.md 的 model 字段
    → AF_LLM_MODEL 环境变量
        → AF_FALLBACK_LLM_MODEL 环境变量
            → "gpt-4o-mini"（兜底，无 aisf/ 前缀 → 走 HTTP）
```

### 工厂路由（create_llm_client）

```python
def create_llm_client(model, base_url, api_key):
    if model.startswith("aisf/"):
        return LlmUdsClient(model=model)          # UDS 路径
    else:
        return OpenAiCompatibleClient(...)          # HTTP 路径
```

---

## 4. 抽象接口：LlmClient

```python
class LlmClient(ABC):
    @abstractmethod
    def chat_completion(
        self,
        messages: list[dict],
        *,
        tools: list[dict] | None = None,
        max_completion_tokens: int = 8192,
        stream: bool = False,
    ) -> dict:
        """返回 OpenAI 兼容响应字典。
        
        成功：{"choices": [{"message": {...}, "finish_reason": "stop"}], "usage": {...}}
        失败：{"error": "...", "_llm_error": true}
        """
```

两条路径各自独立实现此接口，不存在互相回退的关系。

---

## 5. 路径一：OpenAiCompatibleClient（HTTP）

### 通信协议

| 项目 | 说明 |
|------|------|
| 传输 | HTTPS |
| 端点 | `{base_url}/chat/completions` |
| 认证 | `Authorization: Bearer {api_key}` |
| 请求体 | OpenAI Chat Completion JSON |
| 响应 | OpenAI 兼容 JSON |
| 流式 | SSE（`data:` 行） |

### 请求示例

```json
{
  "model": "deepseek-v4-flash",
  "messages": [
    {"role": "system", "content": "你是一个穿搭顾问。"},
    {"role": "user", "content": "今天穿什么？"}
  ],
  "tools": [{"type": "function", "function": {...}}],
  "tool_choice": "auto",
  "max_completion_tokens": 24000
}
```

> HTTP 路径的 model 名**不带** `aisf/` 前缀——直接透传给上游 API。

### 重试策略

| 错误类型 | 重试 |
|---------|------|
| 4xx（auth/400/模型不存在） | ❌ 不重试 |
| DNS / 连接重置 / 超时 | ✅ 指数退避（最多 3 次）|
| SSL 证书错误 | ❌ 不重试 |

---

## 6. 路径二：LlmUdsClient（UDS → UDS Server）

### 通信协议

| 项目 | 说明 |
|------|------|
| 传输 | Unix Domain Socket |
| 端点 | `/tmp/agentflow_plugin.sock` |
| 帧协议 | 4-byte big-endian length prefix + UTF-8 JSON |
| 最大帧 | 10 MB |

### 协议流程（llm.chat 单步协议）

```
客户端                             UDS Server
  │                                      │
  │  1. llm.chat                         │
  │  ──────────────────────────────────→ │
  │  {"method":"llm.chat",               │
  │   "params":{                          │
  │    "request":{                        │
  │     "messages":[...],     ← 完整消息   │
  │     "tools":[...],        ← 工具定义   │
  │     "stream": true,                   │
  │     "max_completion_tokens": 8192     │
  │   }}}                                 │
  │                                      │
  │  ←── 事件流 ──────────────────────── │
  │  {"event_type":"llm_token",           │
  │   "data":{"token":"你"}}              │
  │  {"event_type":"llm_token",           │
  │   "data":{"token":"好"}}              │
  │  {"event_type":"llm_final",           │
  │   "data":{"answer":"你好！",           │
  │          "finish_reason":"stop",      │
  │          "usage":{"prompt_tokens":120}}}│
  └───────────────────────────────────────
```

> 旧三段式协议（llm.task.create → llm.stream → llm.task.destroy + intentions/prompt 格式）
> 已于 2026-07-07 全部移除。客户端和 UDS Server 均只支持 llm.chat 单步协议。

### 6.2 UDS 支持的方法列表

| 方法 | 功能 | 请求关键字段 | 响应关键字段 |
|------|------|-------------|-------------|
| `llm.chat` | 单步 OpenAI 兼容调用（新协议主路径） | `request.*`（完整 OpenAI 请求体） | 事件流（流式）或 `data_json`（非流式） |
| `tool.list` | 列出可用工具 | `plugin_name` | `tools[]` |
| `tool.call` | 调用工具 | `plugin_name`, `tool_name`, `params` | `content[]`, `error_code` |
| `context.*` | 上下文操作 | — | — |
| `databus.*` | DataBus 操作 | — | — |

> 旧方法 llm.task.create / llm.stream / llm.task.destroy / llm.complete / llm.interrupt / llm.context.* 已全部移除。

---

## 7. 事件格式（UDS 流式）

| 事件类型 | 触发时机 | data 字段 |
|---------|---------|----------|
| `llm_token` | 每生成一个 token | `{"token": "..."}` |
| `llm_final` | 生成完成 | `{"answer": "..."}` |
| `llm_error` | 发生错误 | `{"error_code": "500", "error_message": "..."}` |

---

## 8. 配置项

### 环境变量

| 变量 | 作用域 | 说明 |
|------|--------|------|
| `AF_LLM_API_KEY` | HTTP | LLM API 密钥 |
| `AF_LLM_BASE_URL` | HTTP | OpenAI 兼容 API 地址 |
| `AF_LLM_MODEL` | 通用 | 模型名（覆盖 agent.md） |
| `AF_UDS_SOCKET_PATH` | UDS | UDS socket 路径，默认 `/tmp/agentflow_plugin.sock` |
| `AF_FALLBACK_LLM_API_KEY` | HTTP | LLM API 密钥（备选） |
| `AF_FALLBACK_LLM_BASE_URL` | HTTP | LLM API 地址（备选） |
| `AF_FALLBACK_LLM_MODEL` | 通用 | LLM 模型名（备选） |
| `SYS_AGENT_LLM_MODEL` | UDS | UDS 服务端模型名（高于 `AF_LLM_MODEL`） |
| `AF_LLM_CONNECT_TIMEOUT` | HTTP | 连接超时（秒） |
| `AF_LLM_TIMEOUT` | HTTP | 请求超时（秒） |
| `AF_LLM_MAX_RETRIES` | HTTP | 最大重试次数 |
| `AF_LLM_RETRY_BASE_DELAY` | HTTP | 重试基础延迟（秒） |

### 配置文件（agent.md）

```yaml
---
name: 穿搭顾问
model: aisf/deepseek-v4-flash  # aisf/ 前缀 → 走 UDS 通道
---
```

---

## 9. 文件结构

```
nodes/
├── llm_call.py                     # 兼容包装层（保持 call_llm() 签名）
│   ├── call_llm()                  # → delegate to OpenAiCompatibleClient
│   ├── resolve_llm_config()        # → re-export from common.clients
│   └── _TraceRecorder              # DataBus trace 录制（保留）
│
└── common/
    └── clients/
        ├── __init__.py             # 统一导出
        ├── base.py                 # LlmClient 抽象接口 + 工厂 + 配置解析
        ├── openai_compatible_client.py  # HTTP OpenAI 实现
        └── llm_uds_client.py       # UDS UDS Server 实现
```

---

## 10. 不变量

| ID | 不变量 | 验证方式 |
|----|--------|---------|
| I-1 | 模型名以 `aisf/` 开头 → `create_llm_client()` 返回 `LlmUdsClient` | `isinstance(client, LlmUdsClient)` |
| I-2 | 模型名不以 `aisf/` 开头 → `create_llm_client()` 返回 `OpenAiCompatibleClient` | `isinstance(client, OpenAiCompatibleClient)` |
| I-3 | `chat_completion()` 返回的响应始终为 OpenAI 兼容格式 | `"choices" in resp` |
| I-4 | 两种路径对外暴露完全一致的接口签名 | 共享 `LlmClient` 基类 |

---

## 11. 演进路线

| 阶段 | 内容 | 优先级 |
|------|------|--------|
| P0 | HTTP 路径（`OpenAiCompatibleClient`） | ✅ 已完成 |
| P1 | UDS 路径 `llm.chat` 单步协议（替换旧三段式） | ✅ 已完成 |
| P2 | 模型名前缀路由（`aisf/` → UDS，其他 → HTTP） | ✅ 已完成 |
| P3 | UDS 路径全量透传（完整 OpenAI 请求体 `messages` + `tools`） | ✅ 已完成 |
| P4 | 服务端支持解析 `tools` 字段 | 📋 待完成 |

> 旧三段式协议（llm.task.create → llm.stream → llm.task.destroy）已于 P1 阶段整体移除。
