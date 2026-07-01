# LLM 执行轨迹模块设计

> **层级**: L4 · `docs/design/architecture/modules/llm-trace.md`
> **状态**: 已定稿（单阶段，daemon RPC 代理）
> **创建者**: `design-detailed-design-doc-writer`
> **更新日期**: 2026-06-25
> **更新原因**: 废弃阶段 1 UDS 直连方案，统一为 daemon RPC 代理

---

## 1. 场景视图

### 1.1 用户故事

| 角色 | 故事 |
|------|------|
| Harness 评测工程师 | 在 Web 界面选择某次 Agent 对话，查看每次 LLM 调用的请求 messages、响应内容、latency |
| 调优系统 | 通过 API 拉取多个 session 的 LLM 调用统计（平均延迟、token 数、模型版本） |
| Agent 开发者 | 在 agent 调试时实时观察当前 LLM 调用是否按预期构建了 messages |

### 1.2 主流程

```
用户输入 → harness agent 执行
         → llm_think_and_act.py: call_llm()
         → HTTP POST (LLM API)
         → 返回响应
         → 写入 DataBus（Python SDK → UDS → LocalDataBus）
           llm/traces/{call_id}/{field}
         → 继续 while True 循环 / 结束
                │
                ▼
    评测工具 HTTP GET /api/agents/{agent_id}/llm-traces?session_id=...
                │
                ▼
    Bridge → daemon TCP `databus.get` / `databus.list_keys`
                │
                ▼
    Daemon RpcHandler → MMAgentRuntime::get_databus(agent_id)
                      → MMDataBus::get(key) / MMDataBus::list_keys_by_prefix(prefix)
                │
                ▼
    返回 trace 列表
```

### 1.3 数据流（当前实现事实）

`call_llm()` 当前实现（`llm_think_and_act.py:92-119`）：

```
输入: messages, tools, model, base_url, api_key
过程:
  1. 构造 HTTP body {model, messages, max_completion_tokens, tools}
  2. requests.post(url, json=body, headers=headers, timeout=30)
  3. 返回 resp.json()
异常: 返回 {"error": ..., "_llm_error": true}
```

当前状态：LLM 调用完成后，响应直接交给 while True 循环处理，调用过程的元数据（latency、token用量、请求体）全部丢失。

### 1.4 不支持项（当前版本）

- 不支持 Agent 内部的 tool 执行轨迹记录（目标仅为 LLM 调用本身）
- 不支持 streaming 模式下的逐 chunk trace（LLM 当前为单次 request/response）
- 不支持 DataBus 自动数据过期（需要后续补充 TTL 策略）

---

## 2. 方案对比与决策

### 2.1 候选方案概览

| 方案 | 描述 | Daemon 改动 | Bridge 改动 | Agent 改动 | 实时性 |
|------|------|:----------:|:----------:|:----------:|:------:|
| **A: DataBus + Daemon RPC 代理** | Agent 写 DataBus，Bridge 通过 daemon TCP 发 `databus.get` 读取 | 新增 2 个 handler | 新增端点，复用 DaemonClient | 写 DataBus | 轮询 |
| B: Python→Bridge HTTP | Agent 直接 POST Bridge | 无 | 新增端点 | 高 | 实时 |
| C: EventBroker 扩展 | daemon 新增 llm.execution 事件 | 高 | 适配通知 | 低 | 实时 |
| D: Daemon RPC 原生 | daemon 监听 DataBus llm 命名空间并推送 | 高 | 适配通知 | 低 | 实时 |

### 2.2 方案 A 详细说明（选定方案）

**原理**：Agent 脚本将每次 LLM 调用的入参、出参、元数据通过 Python SDK 写入 DataBus。Bridge 通过已有 TCP JSON-RPC 连接向 daemon 发送 `databus.get` 请求；daemon 的 `RpcHandler` 内部调用 `runtime_.get_databus(agent_id).get(key)` 查询数据，返回结果。

**统一方案，不拆分阶段**。最初设计的阶段 1（Bridge 直连 DataBus UDS）在审阅中被否决，原因：
1. DataBus UDS 路径 per-session 动态生成，Bridge 无法自动发现
2. Web 前端需要查询不同 session 的 trace，阶段 1 绑定单个 UDS 路径无法满足
3. daemon RPC 代理天然支持动态 session 查询（通过 `session_id` 参数）

### 2.3 被拒绝方案与不采用原因

| 方案 | 不采用原因 |
|------|-----------|
| **B: Python→Bridge HTTP** | Agent 需要知道 Bridge 地址（配置耦合）；Bridge 离线时 Agent 的 HTTP 调用会异常或阻塞；违反"Agent 只通过 DataBus 与框架通信"的现有约定 |
| **C: EventBroker 扩展** | 需要修改 daemon C++ EventBroker 和 RpcHandler，且 EventBroker 与 DataBus 之间无桥接机制 |
| **D: Daemon RPC 原生** | 需要在 daemon 新增 DataBus 监听+推送模块，复杂度最高，但最终暴露效果与 A 相同 |
| **E: Bridge 直连 DataBus UDS** | UDS 路径 per-session 动态生成，Bridge 无法自动发现；绑定单个 UDS 路径无法满足跨 session 查询需求；UDS 生命周期随 worker 销毁 |

---

## 3. 逻辑视图

### 3.1 模块职责

```
┌─ agentflowd 进程 ─────────────────────────────────────────────────────┐
│                                                                        │
│  ┌─────────────────────┐    ┌──────────────────────────────────┐      │
│  │  RpcServer          │    │  MMAgentRuntime                  │      │
│  │  (daemon RPC 服务器) │    │  ├─ get_databus(agent_id)→DataBus│      │
│  │  TCP :17891 ← Bridge│    │  └─ DataBus::get / list_keys    │      │
│  │  UDS biz / ctrl      │    │                                  │      │
│  └────────┬────────────┘    └──────────┬───────────────────────┘      │
│           │                            │                               │
│  ┌────────▼────────────┐              │                               │
│  │  RpcHandler         │──────────────┘                               │
│  │  databus.get        │     runtime_.get_databus(agent_id)           │
│  │  databus.list_keys  │                                              │
│  └─────────────────────┘                                              │
│                                                                        │
│           │ 阶段 2: daemon TCP `databus.get`                          │
│           ▼                                                           │
│  ┌──────────────────┐                                                 │
│  │  Trace Reader    │ ← Bridge 通过已有 TCP 连接发送 `databus.get`    │
│  │  (bridge 侧)      │                                                 │
│  └──────────────────┘                                                 │
└────────────────────────────────────────────────────────────────────────┘
```

**模块职责**：

| 模块 | 归属 | 职责 |
|------|------|------|
| `Trace Recorder` | `llm_think_and_act.py` | 在 `call_llm()` 调用前后采集入参、响应、latency、token，写入 DataBus |
| `MMDataBus` | Daemon C++ | 内存 KV 存储，支持 `get/put/remove/contains/list_keys_by_prefix` |
| `RpcHandler` | Daemon C++ | 新增 `databus.get` 和 `databus.list_keys` handler，内部调用 `runtime_.get_databus(agent_id)` 查询 |
| `Trace Reader` | Bridge 新增 | 通过 `DaemonClient.send_request()` 发送 `databus.get` / `databus.list_keys`；组装 JSON 响应 |

### 3.2 依赖约束

- `Trace Recorder` 依赖 DataBus UDS（Python SDK `client.llm()`）
- `Trace Reader` 依赖 `DaemonClient`（已有 TCP 连接），不需要 DataBus UDS 路径配置
- `RpcHandler::handle_databus_get/list_keys` 依赖 `MMAgentRuntime::get_databus(agent_id)`
- `Trace Recorder` 不依赖 Bridge，Bridge 不依赖 Trace Recorder（松耦合）

### 3.3 数据契约

#### 存储键路径

DataBus 存储键由 `build_key()` 拼接（`uds_server.cpp:700-704`）：

```
/session/{session_id}/llm/{key}
```

| SDK 调用 | 实际存储键 | 内容 | 写入时机 |
|----------|-----------|------|----------|
| `client.llm().put("traces/{call_id}/request", ...)` | `/session/{session_id}/llm/traces/{call_id}/request` | 完整的 HTTP 请求体 JSON | `call_llm()` 调用前 |
| `client.llm().put("traces/{call_id}/response", ...)` | `/session/{session_id}/llm/traces/{call_id}/response` | 完整的 HTTP 响应体 JSON | `call_llm()` 返回后 |
| `client.llm().put("traces/{call_id}/meta", ...)` | `/session/{session_id}/llm/traces/{call_id}/meta` | 元数据：latency_ms、model、timestamp | `call_llm()` 返回后 |
| `client.llm().put("traces/index", [...])` | `/session/{session_id}/llm/traces/index` | 所有 call_id 的有序列表 | 每次 trace 写入后追加 |

Bridge 通过 daemon RPC 查询时，传递完整键路径（含 `/session/{session_id}/llm/` 前缀给 `MMDataBus::get()`），或使用 `list_keys_by_prefix` 按前缀枚举。

> **注意**：`list_keys_by_prefix` 是 `MMDataBus` 原生 C++ API（`databus.h:55`），不走 UDS 的 `handle_list_keys` stub。因此查询能力不受 UDS 层限制。

#### API 响应结构

| 层级 | 字段 | 对外可见 | 说明 |
|------|------|:--------:|------|
| 运行时内部 | `call_llm()` 的入参/返回值 | 否 | Python 进程内部的局部变量 |
| DataBus 存储键 | `…/llm/traces/{call_id}/{request|response|meta}` | 仅 daemon RPC 可见 | 通过 `databus.get` 查询 |
| Bridge REST 响应 | `traces[].request_summary/response_summary/token_usage` | 是 | 对外 API 的最终交付字段 |

---

## 4. 开发视图

### 4.1 新增文件清单

| 文件 | 归属 | 说明 |
|------|------|------|
| `examples/agent/harness-agent/nodes/llm_trace_recorder.py` | Agent package | Trace Recorder 实现，可被 `call_llm()` 导入 |
| `frontend/bridge/routes/llm_trace.py` | Bridge | Trace Reader 的路由模块 |
| `frontend/bridge/tests/test_llm_trace.py` | Bridge | 新增路由的测试 |

### 4.2 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `examples/agent/harness-agent/nodes/llm_think_and_act.py` | 在 `call_llm()` 中嵌入 `TraceRecorder.trace()` 调用 |
| `frontend/bridge/main.py` | 挂载 `llm_trace` 路由 |
| `src/daemon/rpc_handler.cpp` | 新增 `handle_databus_get()` / `handle_databus_list_keys()`，注册到 `register_all()` |
| `frontend/bridge/docs/design/README.md` | 追加 LLM Trace 模块索引行 |

### 4.3 开发视图（代码依赖）

```
frontend/bridge/routes/llm_trace.py
  → frontend/bridge/client.py (DaemonClient.send_request — 发送 databus.get / databus.list_keys)

src/daemon/rpc_handler.cpp
  → runtime/agent_runtime.h (MMAgentRuntime::get_databus)
  → runtime/databus/databus.h (MMDataBus::get / list_keys_by_prefix)

examples/agent/harness-agent/nodes/llm_think_and_act.py
  → examples/agent/harness-agent/nodes/llm_trace_recorder.py
  → src/runtime/databus/clients/python/agentflow_databus.py (DataBusClient)
```

### 4.4 现有文件无需修改确认

| 文件 | 说明 |
|------|------|
| `frontend/bridge/config.py` | 不需要新增 DataBus 相关配置项 |
| `frontend/bridge/databus_client.py` | 不需要；Bridge 不复用 DataBus UDS SDK |
| `frontend/bridge/client.py` | 已有 `send_request()` 足以发送 `databus.get` / `databus.list_keys` |
| `src/daemon/rpc_server.cpp` | TCP 通道已允许所有方法（`is_method_allowed` 对 TCP 返回 `true`），无需改白名单 |
| `src/runtime/databus/uds_server.cpp` | 查询不经过 UDS 层，走 C++ API 直接调用 |

---

## 5. 接口设计

### 5.1 REST API

**`GET /api/agents/{agent_id}/llm-traces`**

获取指定 agent 在某 session 中的 LLM 调用轨迹列表。

请求参数：

| 名称 | 位置 | 类型 | 必需 | 默认值 | 说明 |
|------|------|------|:----:|--------|------|
| `session_id` | query | string | 是 | — | 会话 ID |
| `dialogue_id` | query | string | 否 | `session_id` | 对话 ID，默认与 session_id 相同 |
| `limit` | query | integer | 否 | 50 | 最多返回的 trace 数，范围 [1, 200] |
| `offset` | query | integer | 否 | 0 | 分页偏移 |

响应体：

```json
{
  "success": true,
  "data": {
    "traces": [
      {
        "call_id": "a1b2c3d4",
        "model": "gpt-4o-mini",
        "latency_ms": 1234,
        "request_summary": "system: ..., user: ...",
        "response_summary": "content: ...",
        "timestamp": "2026-06-25T12:34:56Z",
        "token_usage": {
          "prompt_tokens": 450,
          "completion_tokens": 120,
          "total_tokens": 570
        }
      }
    ],
    "total": 3
  },
  "error": null
}
```

**`GET /api/agents/{agent_id}/llm-traces/{call_id}`**

获取单次 LLM 调用的完整请求/响应体。

响应体：

```json
{
  "success": true,
  "data": {
    "call_id": "a1b2c3d4",
    "request": { "model": "...", "messages": [...], ... },
    "response": { "id": "...", "choices": [...], ... },
    "meta": { "latency_ms": 1234, "model": "gpt-4o-mini", "timestamp": "..." }
  },
  "error": null
}
```

### 5.2 Daemon RPC 协议

新增两个 JSON-RPC 方法，通过已有 TCP 通道发送：

**`databus.get`**

请求：
```json
{
  "jsonrpc": "2.0",
  "method": "databus.get",
  "params": {
    "agent_id": "harness-agent",
    "session_id": "sess_abc123",
    "key": "llm/traces/call-1/meta"
  },
  "id": 1
}
```

响应：
```json
{
  "jsonrpc": "2.0",
  "result": { "exists": true, "value": { "latency_ms": 1234, "model": "gpt-4o-mini" } },
  "id": 1
}
```

**`databus.list_keys`**

请求：
```json
{
  "jsonrpc": "2.0",
  "method": "databus.list_keys",
  "params": {
    "agent_id": "harness-agent",
    "session_id": "sess_abc123",
    "prefix": "llm/traces/"
  },
  "id": 2
}
```

响应：
```json
{
  "jsonrpc": "2.0",
  "result": {
    "keys": [
      "llm/traces/call-1/request",
      "llm/traces/call-1/response",
      "llm/traces/call-1/meta",
      "llm/traces/call-2/request",
      "llm/traces/call-2/response",
      "llm/traces/call-2/meta"
    ]
  },
  "id": 2
}
```

### 5.3 错误码

| HTTP 状态码 | JSON-RPC 错误码 | 触发条件 |
|:-----------:|:----------------:|----------|
| 400 | -32602 | `session_id` 参数缺失 |
| 404 | -32001 | 指定 session_id 下无 trace 记录 |
| 502 | -32003 | DataBus 连接失败或读取异常（daemon 内部错误） |

### 5.4 错误模型

错误响应体：

```json
{
  "success": false,
  "data": null,
  "error": {
    "code": -32602,
    "message": "session_id query parameter is required"
  }
}
```

---

## 6. 实施步骤

### 6.1 Step 1: Daemon 侧注册 handler

1. 在 `rpc_handler.cpp` 中新增 `RpcHandler::handle_databus_get(const std::string& params, int64_t id)`
2. 新增 `RpcHandler::handle_databus_list_keys(const std::string& params, int64_t id)`
3. 在 `register_all()` 中注册 `databus.get` 和 `databus.list_keys` 到 `RpcServer`
4. Handler 实现：从 params 中提取 `{agent_id, session_id, key}` → `runtime_.get_databus(agent_id).get(full_key)`

### 6.2 Step 2: Bridge 侧路由切换

1. `frontend/bridge/routes/llm_trace.py` 将数据源从 `DataBusUdsClient` 切换到 `DaemonClient.send_request("databus.get", ...)`
2. 移除 `frontend/bridge/databus_client.py`（不再需要独立的 UDS 包装器）
3. `frontend/bridge/main.py` 的 lifespan 中移除 DataBus UDS 初始化逻辑

### 6.3 Step 3: Agent 侧 TraceRecorder

1. 创建 `llm_trace_recorder.py`，提供 `trace(messages, response, latency_ms, model)` 方法
2. 在 `call_llm()` 调用前后插入 `TraceRecorder.record_start()` / `TraceRecorder.record_end()`
3. 写入三条 key：`traces/{call_id}/request`（调用前）、`traces/{call_id}/response`（返回后）、`traces/{call_id}/meta`（返回后）
4. 维护索引 key：`traces/index`（追加每次的 call_id）

### 6.4 验证通过标准

- Step 1-3 完成后，通过 `curl` 调用 Bridge 端点返回正确的 trace 数据
- 不同 session_id 返回不同 trace 记录
- Agent 侧 trace 写入耗时 < 50ms
- Agent 主流程在 DataBus 不可用时无异常（写入失败被静默吞掉）

---

## 7. 待确认问题

| 编号 | 问题 | 影响范围 | 建议决策方 |
|:----:|------|----------|:----------:|
| Q1 | DataBus trace 记录的数据生命周期（自动过期 TTL）未定义。当前无清理策略，长期运行可能积累大量历史数据 | DataBus 存储 | 产品/运维 |
| Q2 | `token_usage` 字段并非所有 LLM API 都返回，当前选择原样透传。如果后续需要统一归一化，需新增处理层 | API 响应格式 | 开发 |
| Q3 | `request` 和 `response` 完整体可能包含敏感信息（API key、用户隐私）。当前未做脱敏处理 | API 安全 | 产品/安全 |
| Q4 | 分页参数 `limit` 的 200 上限仅为经验值，非正式容量规格 | API 规格 | 开发 |
| Q5 | `RpcHandler` 获取 `MMDataBus` 时需要 `agent_id` 参数。当前 Web 前端通过 `agent_id` 路径参数传入，但 Web 用户可能不知道 agent_id。是否需要提供按 session_id 查 agent_id 的辅助接口？ | 接口易用性 | 产品/开发 |

---

## 8. 相关文档索引

| 文档 | 层级 | 说明 |
|------|:----:|------|
| `adrs/0004-llm-trace-data-bus-bridge.md` | L3 | 方案选型决策记录 |
| `features/bridge-overview.md` | L2 | Bridge 总体架构 |
| `architecture/modules/daemon-client-protocol.md` | L4 | Daemon 通信协议 |
| `examples/agent/harness-agent/nodes/llm_think_and_act.py` | — | Harness Agent LLM 循环源码 |
| `src/runtime/databus/clients/python/agentflow_databus.py` | — | DataBus Python SDK 源码 |
| `docs/enhancements/041-feature-daemon-databus-rpc-proxy-260625-wip.md` | — | Daemon DataBus RPC 代理 enhancement（rpc_handler 新增 handler） |
