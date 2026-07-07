# 流式返回机制设计

**层级**: L4 技术设计（`docs/design/modules/`）
**日期**: 2026-07-05
**关联**: 串行锁（2026-07-04）、REST cancel 端点

---

## 1. 现状

### 1.1 当前数据流

```
Python node
  └─ 写 DataBus key:  dialogue_id/response = "进度：查询天气中…"
  └─ 正常退出 → coordinator 下一轮触发
    └─ COORDINATORDataExchange handler
      └─ get_framework_data("response") → 读到内容
        └─ session_router_->enqueue_response_message({content, final=false})
          └─ ResponsePoller (50ms)
            └─ agent.response → Bridge _response_queues
              └─ SSE/WebSocket → 客户端
```

### 1.2 问题

| 问题 | 表现 | 根因 |
|------|------|------|
| 延迟高 | 进度消息要等一个完整的 coordinator round | Python 节点不能主动推，必须等 daemon 下一轮轮询 |
| 非实时 | LLM token 流无法逐 token 推送 | DataBus 是 KV 存储，不是流式通道 |
| 不可靠 | 进度消息可能被后续写入覆盖 | 多个节点写同一个 `response` key 竞态 |

---

## 2. 设计目标

1. **Python 节点能主动推流式消息** —— 不等待 coordinator round，实时推送
2. **支持逐 chunk 推送** —— 最终响应也支持分段推（multi-chunk final）
3. **兼容现有机制** —— 现有 coordinator 单次响应路径不变
4. **低开销** —— 不引入新线程/进程，复用现有 `enqueue_response_message` 通道

---

## 3. 方案：统一消息信封 + `response_push`

### 3.1 核心思路

不引入两套数据格式。所有 Bridge ↔ daemon 之间的消息（流式 + 非流式）
统一使用同一信封结构。Python 节点通过 `response_push` 直接推送流式消息，
绕开 coordinator round-trip。

### 3.2 统一消息信封

Bridge ↔ daemon 所有交互（含 `agent.response` 通知 + `response_push` 消息）
统一使用以下信封：

```json
{
  "content": "<payload>",
  "metadata": {
    "dialogue_id": "<id>",
    "source": "coordinator|skill_executor",
    "skill": "weather_query",
    "round": 0,
    "final": "true|false",
    "type": "text|llm_chunk|progress",
    "stream": "true|false"
  }
}
```

**字段说明：**

| 字段 | 类型 | 来源 | 说明 |
|------|------|------|------|
| `content` | string | 节点/coordinator | 消息体 |
| `metadata.source` | string | 发送方 | **消息源**。显式标记消息由哪个组件产生，消除歧义 |
| `metadata.skill` | string | 发送方 | 消息所属技能名。空字符串 = coordinator 自身产生，非空 = skill_executor 在执行该技能时产生 |
| `metadata.round` | int | 发送方 | skill_executor 的 LLM 轮次号（0-based），coordinator 产生时固定为 0 |
| `metadata.type` | string | 发送方 | **消息类型**。统一字段，不因流式/非流式而异 |
| `metadata.stream` | string | `agent.execute` | **流式控制**。`true` 表示此 dialogue 启用流式模式 |
| `metadata.final` | string | 发送方 | `true` 表示对话结束（仅 coordinator 可发）。**唯一真相源** |

> **daemon 历史遗留说明**：daemon `build_response_notification`（`src/daemon/response_poller.cpp:155`）当前在构造 `agent.response` 通知 payload 时，会从 `metadata.final` 拷贝一份到顶层 `"final": bool` 字段。这是 daemon C++ 端的历史行为，**Bridge 已迁移到只读 `metadata.final`**，顶层 `final` 字段视为冗余注入，新代码不应依赖。后续 daemon C++ 重构时可清理该注入逻辑，当前不影响正确性。

**消息类型（type）：**

| type | 说明 | content 格式 | 来源 |
|------|------|-------------|------|
| `text` | 完整文本响应 | OpenAI 同步响应 JSON (`choices[0].message.content`) | `source=coordinator`，`final=true` |
| `llm_chunk` | LLM 流式 token 块 | OpenAI SSE chunk JSON (`choices[0].delta.content`) | `source=coordinator`（通过 `response_push`）|
| `progress` | 进度通知 | OpenAI 同步响应 JSON (`choices[0].message.content`) | `source=coordinator` 或 `source=skill_executor`（均通过 `response_push`）|

> **核心原则**：
> 1. **agentflow daemon ↔ Bridge 内部协议**：所有消息类型的 `content` 字段统一为 OpenAI-compatible JSON 对象，禁止裸字符串。
> 2. **Bridge → 前端/外部客户端（统一数据格式）**：WebSocket 和 SSE 两个通道使用完全相同的 OpenAI JSON 格式。前端和外部客户端用同一套解析逻辑。**按 `_chunk_type` 分流解析路径**：
>    - `llm_chunk` → `choices[0].delta.content`（OpenAI SSE chunk 格式，提示调用方这是流式增量内容，需自行组装）
>    - `text` / `progress` → `choices[0].message.content`（OpenAI 同步响应格式，完整内容，直接渲染）
> 3. **Bridge → 数据库**：存纯文本（从 `choices[0].message.content` 提取，仅 `type=text` 的最终回复入库），不存 OpenAI 信封。

OpenAI 两种响应格式的对照：

| 场景 | OpenAI 原文 | 本协议中的 `content` |
|------|-------------|---------------------|
| 同步（非流式） | `{"choices":[{"message":{"content":"北京25度"}}]}` | 同上，透传 |
| 流式 SSE chunk | `{"choices":[{"delta":{"content":"北京"}}]}` | 同上，透传 |

source + skill 联合判断逻辑：

| source | skill | 语义 |
|--------|-------|------|
| `coordinator` | `""` | coordinator 自身产生（决策进度 / 最终回复）|
| `coordinator` | `"weather_query"` | coordinator 通报 skill 的调度信息（即将执行/已派发）|
| `skill_executor` | `"weather_query"` | skill_executor 在该技能执行过程中产生 |

**Bridge 处理流程：统一格式 → 分发**

Bridge 收到 `agent.response` / `response_push` 通知后，执行两个动作：

```
agent.response notification
  └─ content = OpenAI JSON（daemon → Bridge 内部协议）
  └─ metadata = {source, type, final, skill, round, ...}

  │
  ├─ ① 转发给前端（WebSocket）和外部客户端（SSE）
  │     统一输出格式：content 保持 OpenAI JSON 不变
  │     按 `_chunk_type` 分流 JSON 结构（守核心原则 #2）：
  │       `llm_chunk` → `choices[0].delta.content`（OpenAI SSE chunk 格式，流式增量）
  │       `text` / `progress` → `choices[0].message.content`（OpenAI 同步响应格式，完整内容）
  │
  │   WebSocket 帧（字段统一使用 `_` 前缀，与 SSE 通道格式完全一致）:
  │   {
  │     "type": "chunk",
  │     "content": '{"choices":[{"message":{"content":"北京25度"}}]}',  # text/progress
  │     "_final": true,
  │     "_progress": false,
  │     "_chunk_type": "text",
  │     "_source": "coordinator",
  │     "_skill": "weather_query",
  │     "_round": "0",
  │   }
  │
  │   # llm_chunk 的 content 使用 delta（OpenAI SSE chunk 格式）:
  │   # {"choices":[{"delta":{"content":"北京"}}]}
  │
  │   SSE data: 行（与 WebSocket 等价内容，格式完全一致）:
  │   data: {"choices":[{"message":{"content":"北京25度"}}],
  │          "_chunk_type":"text", "_source":"coordinator", ...}
  │
  │  ┌─ 前端/外部客户端统一解析（WebSocket 和 SSE 使用完全相同的解析逻辑）:
  │  │  **按 `_chunk_type` 分流解析路径：**
  │  │  - `llm_chunk` → `choices[0].delta.content`（OpenAI SSE chunk，增量 token）
  │  │    → 追加到 (source, skill, round) 分组
  │  │  - `text` → `choices[0].message.content`（同步响应，完整文本）
  │  │    → 填入对话气泡，clearLlmChunks()
  │  │  - `progress` → `choices[0].message.content`（同步响应，进度文本）
  │  │    → 进度条显示 source + skill + round
  │  │
  │  └ `_` 前缀字段：WebSocket 和 SSE 通道统一使用 `_` 前缀标注非标准元数据。
  │     SSE 通道的原因是兼容 OpenAI parser（标准 SDK 零默忽略 `_` 前缀字段）；
  │     WebSocket 通道虽不面对外部 SDK，但为统一两通道解析逻辑亦采用 `_` 前缀。
  │     **Bridge 不剥壳**——content 原样透传给消费者，剥壳只发生在数据库持久化路径。
  │
  └─ ② 保存到数据库
       └─ 存纯文本（从 choices[0].message.content 提取，仅 `type=text` 的最终回复入库）
       └─ 不存 OpenAI 信封
```

> **前端/外部客户端视角**：
> - **WebSocket 和 SSE 数据格式统一**：`content` 均为 OpenAI JSON，字段命名统一使用 `_` 前缀
> - **解析路径按 `_chunk_type` 分流**：
>   - `llm_chunk` → `choices[0].delta.content`（增量 token，追加到分组缓存）
>   - `text` / `progress` → `choices[0].message.content`（完整内容，直接渲染）
> - `_chunk_type` + `_source` 供精细化渲染，不影响内容解析
> - **Bridge 不剥壳**：content 原样透传给前端，剥壳只发生在数据库持久化路径
> - 刷新页面从数据库读取时，`content` 是纯文本（DB 剥壳存纯文本，不存 OpenAI 信封）

### 3.2.1 外部客户端视角：SSE 接口的线格式

外部客户端通过 `POST /api/chat/{agent_id}?stream=true` 使用 SSE 流式接口时，
收到的是兼容 OpenAI Chat Completions SSE 格式的数据流：

```
# ── progress (type=progress) ──
data: {"choices":[{"message":{"content":"coordinator 决策中…"},"index":0}],
       "_progress":true, "_chunk_type":"progress",
       "_source":"coordinator",
       "_skill":"", "_round":"0",
       "_dialogue_id":"0616142301-a1b2c3d4e5f6"}

# ── skill_executor 中间输出 (type=progress, skill_executor 执行中) ──
# 按 §3.6 约束：skill_executor 的 LLM 输出推 type=progress，不入对话气泡
data: {"choices":[{"message":{"content":"weather_query 第 1 轮输出：北京"}}],
       "_chunk_type":"progress", "_source":"skill_executor",
       "_skill":"weather_query", "_round":"0"}

data: {"choices":[{"message":{"content":"weather_query 第 2 轮输出：25度"}}],
       "_chunk_type":"progress", "_source":"skill_executor",
       "_skill":"weather_query", "_round":"1"}

# ── coordinator_decide 流式 token (type=llm_chunk) ──
data: {"choices":[{"delta":{"content":"北京今天25度，适合"}}],
       "_chunk_type":"llm_chunk", "_source":"coordinator"}

# ── 最终回复 (type=text, coordinator 返回) ──
data: {"choices":[{"message":{"content":"北京今天25度，适合穿短袖"},"index":0}],
       "_chunk_type":"text", "_source":"coordinator",
       "_artifacts":[{"type":"image","url":"/api/artifacts/...","name":"outfit.jpg"}]}

# ── 终止信号 ──
data: [DONE]
```

**线格式设计要点：**

| 要点 | 说明 |
|------|------|
| **OpenAI 兼容** | `choices[0].delta.content`（`llm_chunk`）或 `choices[0].message.content`（`text`/`progress`），与 `chat/completions` SSE 格式对齐 |
| **分流解析** | `_chunk_type` 指示 content 的 JSON 结构：`llm_chunk` → `delta`（需自行组装），`text`/`progress` → `message`（完整内容，直接使用）|
| **_ 前缀字段** | 非标准元数据以 `_` 前缀标注，标准 OpenAI parser 会静默忽略，不影响第三方集成 |
| `_chunk_type` | `text` / `llm_chunk` / `progress`，前端渲染分支，外部客户端可忽略 |
| `_source` | `coordinator` / `skill_executor`，消息来源，前端精细化渲染用 |
| `_skill` / `_round` | 技能执行元数据，前端按技能分组展示 LLM token 流 |
| `_progress` | `true` 表示进度消息，前端显示进度条 |
| `[DONE]` | SSE 流终止信号，遵循 OpenAI 协议 |

**外部客户端解析示例**（最小兼容实现）：

```
line = read line from SSE stream
if line starts with "data: ":
    payload = JSON.parse(line.slice(6))
    if payload === "[DONE]": break
    
    chunk_type = payload._chunk_type
    if chunk_type === "llm_chunk":
        text = payload.choices?.[0]?.delta?.content   # 增量 token，需自行组装
    else:
        text = payload.choices?.[0]?.message?.content  # 完整内容（text/progress），直接使用
    if text:
        process(text)
```

> 最小兼容实现可简化为只读 `choices[0].delta.content`（`message.content` 在 delta 中不存在时返回 `undefined`），但需注意 `text`/`progress` 时 `delta.content` 为空。

> 这意味着任何兼容 OpenAI SSE 的客户端 SDK（如 `openai` Python 库的 `stream=True`）
> 可以直接对接本接口，无需适配。`_` 前缀字段由 SDK 自动忽略。

### 3.3 流式控制（stream 标志）

`stream` 标志在 `agent.execute` 请求中设置，贯穿整个 dialogue：

```json
{
  "jsonrpc": "2.0",
  "method": "agent.execute",
  "params": {
    "agent_id": "outfit-assistant",
    "content": "...",
    "user_id": "demo",
    "client_id": "demo",
    "dialogue_id": "...",
    "stream": true
  }
}
```

| stream | 行为差异 |
|--------|---------|
| `false`（默认）| 现有行为。coordinator 完成后一次性推 `type=text, final=true` |
| `true` | 流式模式。coordinator 路径不变，但 Python 节点可主动推 `llm_chunk`/`progress` |

`stream=true` 时，一条 dialogue 的消息序列按执行流程展开（标注 source 和 skill）：

```
# ── coordinator 第 1 轮：规划 ──
response_push(source=coordinator, skill="", type=progress,
              content='{"choices":[{"message":{"content":"coordinator 决策中…"}}]}')

# ── coordinator 派发技能 ──
response_push(source=coordinator, skill="weather_query", type=progress,
              content='{"choices":[{"message":{"content":"正在执行 weather_query"}}]}')

# ── skill_executor 第 1 轮 LLM ──
response_push(source=skill_executor, skill="weather_query", round=0, type=progress,
              content='{"choices":[{"message":{"content":"weather_query 第 1/5 轮：调用 LLM…"}}]}')
# skill_executor 的 LLM token 按 §3.6 约束推 type=progress（中间产物不入对话）
response_push(source=skill_executor, skill="weather_query", round=0, type=progress,
              content='{"choices":[{"message":{"content":"weather_query 第 1 轮输出：北京"}}]}')

# ── skill_executor 工具调用 ──
response_push(source=skill_executor, skill="weather_query", round=0, type=progress,
              content='{"choices":[{"message":{"content":"weather_query → 正在调用 get_weather"}}]}')

# ── skill_executor 第 2 轮 LLM（汇总结果）──
response_push(source=skill_executor, skill="weather_query", round=1, type=progress,
              content='{"choices":[{"message":{"content":"weather_query 第 2/5 轮：调用 LLM…"}}]}')
response_push(source=skill_executor, skill="weather_query", round=1, type=progress,
              content='{"choices":[{"message":{"content":"weather_query 第 2 轮输出：25度"}}]}')

# ── coordinator 第 2 轮：生成最终回复 ──
response_push(source=coordinator, skill="", type=progress,
              content='{"choices":[{"message":{"content":"coordinator 生成回复…"}}]}')

# ── 最终响应 ──
coordinator(source=coordinator, type=text,
            content='{"choices":[{"message":{"content":"北京今天25度，适合穿短袖"}}]}',
            final=true)
```

前端收到 `type=llm_chunk` 时解析 `delta.content` 按 `(source, skill, round)` 分组追加到 `chatStore.llmChunks`；收到 `type=text` 时读取 `message.content` 以完整文本替换，并 `clearLlmChunks()`。

### 3.4 工具调用场景

工具调用（function calling）在流式模式下通过标准 OpenAI SSE 格式传递。
Python 节点透传 LLM 返回的原始 chunk，不额外处理：

```
LLM 返回:
  data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_xxx",
         "function":{"name":"get_weather","arguments":""}}]},"index":0}]}

Python 节点:
  # skill_executor 中：按 §3.6 约束推 type=progress（含 tool_calls chunk）
  response_push(type=progress, content=<原始 JSON>)
  # coordinator_decide 中：推 type=llm_chunk

Bridge → 前端:
  ChatChunk {type: "llm_chunk" 或 "progress", content: <原始 JSON>}  # type 跟随节点推送
  
前端:
  OpenAI SSE parser 原生解析 choices[0].delta.tool_calls
  → 触发工具调用 UI 展示
```

**不需要新增消息类型**——`llm_chunk` 天然支持 `delta.content` 和 `delta.tool_calls` 两种形式，前端 OpenAI parser 一视同仁。

### 3.5 Python 节点 LLM 配置与 stream 控制传递

Python 节点调用 LLM 需要以下配置。传递方式统一为**环境变量**（子进程继承）：

| 变量 | 来源 | 说明 |
|------|------|------|
| `AF_LLM_BASE_URL` | daemon 进程环境 | OpenAI 兼容 API 地址 |
| `AF_LLM_API_KEY` | daemon 进程环境 | API 密钥 |
| `AF_LLM_MODEL` | daemon 进程环境 | 模型名 |
| `AF_STREAM_ENABLED` | `agent.execute` 的 `stream` 参数 | `true/false`，Python 节点据此决定是否以流式调 LLM |

**daemon 侧改动**（已存在部分）：

```cpp
// worker.cpp — 设置 Python 子进程环境变量
exec_config.env_vars.push_back("AF_LLM_BASE_URL=" + request.llm_config.base_url);
exec_config.env_vars.push_back("AF_LLM_API_KEY=" + request.llm_config.api_key);
exec_config.env_vars.push_back("AF_LLM_MODEL=" + request.llm_config.model);
exec_config.env_vars.push_back("AF_STREAM_ENABLED=" + (request.stream ? "true" : "false"));
```

**Python 节点使用示例**：

```python
import os
import json
from openai import OpenAI

client = OpenAI(
    base_url=os.environ["AF_LLM_BASE_URL"],
    api_key=os.environ["AF_LLM_API_KEY"],
)

stream_enabled = os.environ.get("AF_STREAM_ENABLED", "false") == "true"

if stream_enabled:
    # 流式调用：逐 chunk 透传
    response = client.chat.completions.create(
        model=os.environ["AF_LLM_MODEL"],
        messages=[{"role": "user", "content": prompt}],
        stream=True,
    )
    for chunk in response:
        # chunk 是 OpenAI SSE data: 行内容，原始 JSON
        raw = chunk.model_dump_json()
        # type/source 按调用方设置（守 §3.6）：
        # skill_executor → type="progress", source="skill_executor"（中间产物不入对话）
        # coordinator_decide → type="llm_chunk", source="coordinator"（对话 token，前端增量组装）
        client_databus._call("response_push", {
            "dialogue_id": os.environ["AF_DIALOGUE_ID"],
            "source": "...",       # 由调用方设置（详见 §3.6 约束）
            "skill": os.environ.get("AF_SKILL_NAME", ""),
            "round": round_i,      # 当前 LLM 轮次（0-based）
            "type": "...",         # 由调用方设置（详见 §3.6 约束）
            "content": raw,
            # 不传 final —— daemon 侧校验拒绝 final=true（守 §3.7 不变量 S-1），
            # 调用方传 false 或不传均合法，传 true 会报错。仅 coordinator 完成路径可设 final=true
        })
else:
    # 非流式：一次调用，结果走 coordinator
    response = client.chat.completions.create(
        model=os.environ["AF_LLM_MODEL"],
        messages=[{"role": "user", "content": prompt}],
        stream=False,
    )
    # 结果写入 DataBus — 使用完整的 OpenAI 响应 JSON（choices[0].message.content）
    # coordinator 读到时通过 get_framework_data("response") 取到完整 JSON，
    # COORDINATORDataExchange 将其作为 type=text 的 content 透传给前端
    client_databus.put("response", response.model_dump_json())
```

### 3.6 关键约束

| 约束 | 说明 |
|------|------|
| **`final=true` 仅 coordinator 可发** | `response_push` 拒绝 `final=true`——生命周期管理必须走 coordinator 完成路径 |
| **`type=text` 始终存在** | 无论是否流式，coordinator 完成时一定发一条 `type=text, final=true` |
| **`llm_chunk` ≠ 完整内容** | 前端以最终 `type=text` 为准，`llm_chunk` 仅用于预览 |
| **`source` 必须显式设置** | 每条消息必须显式标注 `source`，禁止缺省或隐式推导。coordinator 产生的消息 source="coordinator"；skill_executor 产生的 source="skill_executor" |
| **`skill` 字段的填充规则** | source=coordinator 时 skill 可为空（自身进度）或填入技能名（通报技能调度）；source=skill_executor 时 skill 必须为当前执行技能名 |
| **`skill_executor` 的 LLM token 推为 `progress` 类型** | skill_executor 的中间 LLM 输出（思辨、工具调用、产物汇总）推 `type=progress`，前端归过程展示通道，被 coordinator final 回答覆盖。`coordinator_decide` 的 LLM 输出推 `type=llm_chunk`。守不变量 I-1：skill 产物不应被前端当作对话最终回答渲染 |
| **stream 标志不可中途变更** | 同一 dialogue 的 `stream` 值在 `agent.execute` 时固定 |

### 3.7 不变量

| ID | 不变量 | 实现状态 |
|----|--------|---------|
| S-1 | `response_push` 只接受 `final=false`，拒绝 `final=true` | `[daemon 侧校验待实现]` 当前 daemon UDS handler 无 `response_push` 处理代码，校验逻辑后续落地 |
| S-2 | `response_push` 不入 DataBus、不触发 coordinator、不释放串行锁 | `[已实现]` `runtime.cpp` response_push_callback 直接调 `session_router->enqueue_response_message()`，不写 DataBus、不走 COORDINATORDataExchange drain |
| S-3 | coordinator 最终路径不受影响——`final=true` + cleanup + 锁释放仍由 coordinator 处理 | 已实现（`response_poller.cpp` / `rpc_handler.cpp` 读 `metadata.final`） |
| S-4 | 同一 dialogue 的 `response_push` 消息和 coordinator 最终消息在 Bridge 端按入队顺序到达 | `[daemon 侧校验待实现]` |
| S-5 | `source` 必须为枚举值 `coordinator` 或 `skill_executor`，拒绝未知值 | `[daemon 侧校验待实现]` |
| S-6 | `source=skill_executor` 时 `skill` 字段不可为空，`round` 字段不可缺省 | `[daemon 侧校验待实现]` |

> **实现状态说明**：S-1/S-4/S-5/S-6 的 daemon 侧校验逻辑待落地。S-2 已实现——`runtime.cpp` response_push_callback（2026-07-07 修复）直接调 `session_router->enqueue_response_message()`，不再写入 DataBus `/session/{sid}/stream/{did}/chunk_{seq}`，也不经过 COORDINATORDataExchange drain_lambda。S-3 已实现——daemon `response_poller.cpp:133` 和 `rpc_handler.cpp:413` 均从 `metadata.final` string 读取 final 标记。

---

### 3.8 数据流示意

#### 3.8.1 流式 API（SSE, `?stream=true`）

```
外部客户端                  Bridge                          daemon (agentflow)                 Python 节点
    │                        │                                  │                                  │
    │① POST /api/chat/{id}   │                                  │                                  │
    │   ?stream=true         │                                  │                                  │
    │──────────────────────► │                                  │                                  │
    │                        │                                  │                                  │
    │                        │② agent.execute(stream=true)      │                                  │
    │                        │─────────────────────────────────►│                                  │
    │                        │                                  │                                  │
    │                        │                                  │③ 启动 coordinator workflow        │
    │                        │                                  │─────────────────────────────────►│
    │                        │                                  │                                  │
    │                        │                                  │                                  │ ④ coordinator_decide.py
    │                        │                                  │                                  │    调 LLM 规划…
    │                        │                                  │                                  │    response_push(progress)
    │                        │                                  │◄── DataBus stream/chunk ──────────│
    │                        │                                  │                                  │
    │                        │                                  │⑤ COORDINATORDataExchange         │
    │                        │                                  │   drain_lambda → enqueue_response │
    │                        │◄── agent.response (progress) ────│                                  │
    │                        │                                  │                                  │
    │⑥ SSE data:             │                                  │                                  │
    │  {choices[0].message,  │                                  │                                  │
    │   _chunk_type=progress,│                                  │                                  │
    │   _source=coordinator} │                                  │                                  │
    │◄───────────────────────│                                  │                                  │
    │                        │                                  │                                  │
    │                        │                                  │ ⑦ 派发技能 → 启动 skill workflow  │
    │                        │                                  │─────────────────────────────────►│
    │                        │                                  │                                  │
    │                        │                                  │                                  │ ⑧ skill_executor.py
    │                        │                                  │                                  │    response_push(progress)
    │                        │                                  │◄── DataBus stream/chunk ──────────│
    │                        │                                  │                                  │
    │                        │                                  │ ⑨ COORDINATORDataExchange         │
    │                        │                                  │    drain_lambda (每轮)             │
    │                        │◄── agent.response (progress) ────│                                  │
    │                        │                                  │                                  │
    │⑩ SSE data:             │                                  │                                  │
    │  {_chunk_type=progress,│                                  │                                  │
    │   _source=skill_executor}                                │                                  │
    │◄───────────────────────│                                  │                                  │
    │                        │                                  │                                  │
    │                        │                                  │ ⑪ skill 完成 → 写 DataBus         │
    │                        │                                  │    coordinator 第 2 轮触发         │
    │                        │                                  │─────────────────────────────────►│
    │                        │                                  │                                  │
    │                        │                                  │                                  │ ⑫ coordinator_decide.py
    │                        │                                  │                                  │    生成最终回复
    │                        │                                  │                                  │    stdout → DataBus "response"
    │                        │                                  │◄── coordinator 完成 ──────────────│
    │                        │                                  │                                  │
    │                        │                                  │ ⑬ COORDINATORDataExchange         │
    │                        │                                  │    读 response key → enqueue      │
    │                        │◄── agent.response (text,         │                                  │
    │                        │    metadata.final=true) ──────────│                                  │
    │                        │                                  │                                  │
    │⑭ SSE data:             │                                  │                                  │
    │  {choices[0].message,  │                                  │                                  │
    │   _chunk_type=text,    │                                  │                                  │
    │   _source=coordinator} │                                  │                                  │
    │  data: [DONE]          │                                  │                                  │
    │◄───────────────────────│                                  │                                  │
    │                        │                                  │                                  │
    │                        │ ⑮ 持久化到 DB                    │                                  │
    │                        │    存纯文本（从 message.content 剥壳，不存 OpenAI 信封）│                                  │
```

#### 3.8.2 同步 API（`?stream=false` 或缺省）

```
外部客户端                  Bridge                          daemon (agentflow)
    │                        │                                  │
    │① POST /api/chat/{id}   │                                  │
    │──────────────────────► │                                  │
    │                        │                                  │
    │                        │② agent.execute(stream=false)     │
    │                        │─────────────────────────────────►│
    │                        │                                  │
    │                        │      ┌───────────────────────────┤
    │                        │      │ daemon 内部执行完整流程     │
    │                        │      │ （与流式 API 的 ③-⑫ 相同， │
    │                        │      │  但不含 response_push 推送）│
    │                        │      │                           │
    │                        │      │ coordinator_decide → LLM   │
    │                        │      │ → 派发技能 → skill_executor │
    │                        │      │ → coordinator 第 2 轮      │
    │                        │      │ → 最终回复写 DataBus       │
    │                        │      └───────────────────────────┤
    │                        │                                  │
    │                        │③ COORDINATORDataExchange         │
    │                        │   读 response key (仅 final)     │
    │                        │◄── agent.response (text,         │
    │                        │    metadata.final=true) ──────────│
    │                        │                                  │
    │④ HTTP 200              │                                  │
    │  {                     │                                  │
    │   "data": {            │                                  │
    │    "content": "北京今天25度",│                              │
    │    "artifacts": [...]  │                                  │
    │   }                    │                                  │
    │  }                     │                                  │
    │◄───────────────────────│                                  │
    │                        │                                  │
    │                        │⑤ 持久化到 DB                     │
    │                        │   存纯文本                        │
```

**同步/流式关键差异：**

| 维度 | 同步 API | 流式 API (SSE) |
|------|---------|---------------|
| Bridge 收到通知 | 仅等待 `final=true` 的一条 | 逐条转发（progress / llm_chunk / text）|
| 中间状态 | 不累积，丢弃非 final chunk | 逐条 SSE data: 推送 |
| 响应格式 | 单次 JSON body | `text/event-stream`，每条 `data:` 行 |
| 外部客户端体验 | 等待完整响应后一次性拿到 | 实时看到进度→流式 token→最终结果 |
| DB 持久化 | 仅存最终回复纯文本 | 仅存最终回复纯文本（与同步一致）|

## 4. 兼容性

| 场景 | 现有路径 | 新路径 | 选择依据 |
|------|---------|--------|---------|
| 最终响应（单次） | coordinator → DataBus → daemon | — | 保持现有 |
| 最终响应（流式多 chunk） | coordinator → multi_response | `response_push(type=llm_chunk)` × N | 新路径低延迟 + 原生流式 |
| 进度消息 | coordinator 写 `response` key | `response_push(type=progress)` | 新路径低延迟 |
| LLM 流式调用 | 不存在（Python 节点自行聚合后写 DataBus） | `response_push(type=llm_chunk)`（coordinator_decide）或 `response_push(type=progress)`（skill_executor）逐 chunk 透传 | 新增能力。按 §3.6 分流：coordinator_decide 用 `llm_chunk`（增量 token，前端组装），skill_executor 用 `progress`（中间产物，仅过程展示）|

---

## 5. 改动清单

| 层 | 改动 | 风险 | 覆盖文件 | 状态 |
|----|------|------|---------|------|
| C++ UDS handler | 新增 `response_push` 方法注册 + handler；消息信封增加 `source`/`skill`/`round` 字段透传 | 低——纯入队，无副作用 | `uds_server.h`, `uds_server.cpp` | `[已实现]` |
| C++ runtime callback | `response_push_callback` 直接 enqueue 到 `session_router`（不写 DataBus） | 中——绕过 coordinator round-trip | `runtime.cpp` L662-702 | `[已实现]`（2026-07-07 修复） |
| Python DataBus SDK | 新增 `response_push()` 便捷方法；参数表增加 `source`/`skill`/`round` | 低——复用 `_call` 通道 | `agentflow_databus.py` | `[未实现]` |
| Bridge `chat.py` | `_run_turn` 按 `source`+`type` 组合转换帧 | 低 | `chat.py` | `[未实现]` |
| Bridge `chat_rest.py` | 同上，SSE 流式通道 | 低 | `chat_rest.py` | `[未实现]` |
| Bridge `aisf_tasks.py` | 按 `(userName, deviceId)` 串行化排队 | 低 | `aisf_tasks.py` | 已完成 |
| 前端 `useChatWebSocket.ts` | 支持 `progress=true` 帧、解析 `llm_chunk` 的 OpenAI JSON；llmChunks 分组键从 `(skill, round)` 扩展为 `(source, skill, round)` | 低 | `useChatWebSocket.ts` | `[未实现]` |
| 前端 `ChatView.vue` | 展示 progress 指示器，按 source 区分规划阶段/执行阶段的显示样式 | 低 | `ChatView.vue` | `[未实现]` |
| Python 节点 `coordinator_decide.py` | `response_push` 调用时携带 `source=coordinator` | 低 | `coordinator_decide.py` | `[部分实现]` — `skill`/`round`/`status` 已传，`source` 待补 |
| Python 节点 `skill_executor.py` | `response_push` 调用时携带 `source=skill_executor` + `skill` + `round` | 低 | `skill_executor.py` | `[部分实现]` — `skill`/`round`/`status` 已传，`source` 待补 |

> 注：`coordinator_decide.py` 和 `skill_executor.py` 的改动在概念上属于"消费者"侧——它们只需在调 `response_push` 时补上正确的 `source` / `skill` / `round` 即可。信封的 C++/Bridge 侧是接收方，已经支持透传这些字段。

---

## 6. 不做的事

- 不引入独立 streaming 线程/进程（复用现有 ResponsePoller）
- 不改动 `consume_multi_response` 机制（保持兼容）
- 不改变 Coordinator 的最终响应路径（保持对齐）

---

## 7. Daemon → Bridge 消息推送链路

### 7.1 完整链路

```
Python 节点
  │
  ├─[非流式] 写 DataBus key "response"
  │   → 节点退出，coordinator 下一轮触发
  │     → COORDINATORDataExchange handler
  │       → get_framework_data("response") 读 DataBus
  │         → session_router_->enqueue_response_message({content, final=true/false})
  │
  └─[流式] response_push(type="llm_chunk")
      → [设计/已修复] 直接 session_router_->enqueue_response_message()  ← 绕过 coordinator
              │
              ▼
    Agent response 消息队列 (MMAgentMsgQue)
              │
              ▼
    ResponsePoller (独立线程，50ms 轮询)
      → agent->dequeue_response_message(response)
      → 按 message_id / dialogue_id / agent_id 匹配 in_flight_ 请求
      → 构建 JSON-RPC notification:
          {"content":"...", "metadata":{...}, "final":true/false}
      → rpc_server_->send_notification(client_fd, "agent.response", params)
              │
              ▼
    ═══ WebSocket (JSON-RPC 2.0 notification, 无 id) ═══
              │
              ▼
    Bridge DaemonClient._reader_loop
      → _dispatch_frame(text)
        → 无 "id" → 通知 → method == "agent.response"
          → 入队 _notification_queue (asyncio.Queue)
              │
              ▼
    SSE 生成器 (chat_rest.py)
      → wait_for_response() 从 _notification_queue.get()
      → yield SSE 帧 → 前端
      → final=true 时结束循环
```

### 7.2 关键组件

| 组件 | 位置 | 职责 |
|------|------|------|
| `session_router->enqueue_response_message()` | `session_router.cpp` | 把消息放入 agent 的 response 消息队列 |
| `ResponsePoller` | `response_poller.cpp` | 独立线程，50ms 轮询，从队列取消息通过 WebSocket 推送给 bridge |
| `DaemonClient._notification_queue` | `client.py` | bridge 端 asyncio.Queue，缓冲 daemon 推送的 agent.response |
| `wait_for_response()` | `client.py` | 从 _notification_queue 取一条通知给 SSE 生成器 |

### 7.3 历史根因：response_push 写入 DataBus（已修复）

**2026-07-07 发现并修复的 bug**：

`runtime.cpp` 的 `response_push_callback` 原实现将 chunk 写入 DataBus `/session/{sid}/stream/{did}/chunk_{seq}`，由 `agent.cpp` 的 `COORDINATORDataExchange` drain_lambda 在 coordinator 完成后批量 drain。这导致：

1. **延迟高**：LLM 调用过程中的 chunk 全部缓存到 DataBus，等 coordinator workflow 完成后才被 drain → 前端在 5 秒 LLM 调用期间无任何 chunk 推送，完成后 300ms 内涌入 278 条洪水
2. **违反 S-2**：chunk 经过 DataBus + coordinator round-trip，而非直接 enqueue
3. **与设计意图矛盾**：SDK docstring 声称 "bypasses the coordinator round-trip"，实际实现绑定 coordinator drain

**修复方案**：`response_push_callback` 改为捕获 `this`（MMAgentRuntime）和 `agent_id`，直接调 `get_agent(agent_id)->get_session_router()->enqueue_response_message(msg)`。chunk 在写入后 ≤50ms（ResponsePoller 轮询间隔）到达 bridge。

**agent.cpp 的 drain_lambda 保留为 no-op 安全网**——由于 response_push 不再写 DataBus，drain_lambda 的 `list_keys_by_prefix` 将找不到任何 key，自动跳过。
