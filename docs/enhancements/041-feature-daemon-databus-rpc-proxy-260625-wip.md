# 041: Daemon DataBus RPC 代理

> **类型**: feature
> **状态**: wip
> **创建日期**: 2026-06-25
> **更新日期**: 2026-06-25

---

## 1. 背景

Bridge 需要动态查询任意会话在 DataBus 中存储的 LLM 执行轨迹数据。当前 daemon 的 JSON-RPC 方法列表中没有与 DataBus 交互的方法——"DataBus 是独立于 daemon RPC 之外的子系统"（ADR-0004）。

Bridge 仅有的一条 TCP JSON-RPC 连接通向 daemon，无法绕过 daemon 直接访问 DataBus（DataBus UDS 是 per-session 动态生成，Bridge 无法自动发现）。

目标：在 daemon RPC handler 层新增 `databus.get` 和 `databus.list_keys` 两个方法，Bridge 通过现有 TCP 连接发送 JSON-RPC 请求，daemon 内部完成寻址和查询。

## 2. 设计意图（重要）

### 2.1 DataBus 的设计定位

DataBus 是**内部组件**，不是外部 API。DataBus 的 key 路径 `/session/{session_id}/...` 中的 `session_id` 是框架内部分配的 pool index（如 `"0"`、`"1"`），外部客户端不可见。

内部调用方知道 session_id 的途径：

| 调用方 | 怎么知道 session_id |
|--------|-------------------|
| C++ Node | 框架注入 `databus_client_->session_id()` |
| Python/Shell 节点 | 环境变量 `AF_SESSION_ID` |
| daemon 框架代码 | `session->get_session_id()` |

### 2.2 外部调用方的视角

外部调用方（bridge/web）只知道 `(user_id, client_id)`。这两个字段通过 `route_key = user_id + ":" + client_id` 由 SessionRouter 映射到内部 session_id。DataBus 本身不认 `(user_id, client_id)`。

### 2.3 核心设计约束

**`databus.get` 和 `databus.list_keys` 不是 DataBus 内部接口的透明代理，而是带 SessionRouter 桥接的业务 API。**

这意味着：
- 外部调用方传 `(user_id, client_id)`（跟 `agent.execute` 的参数一致），**不需要知道 session_id**
- daemon handler 内部通过 SessionRouter 将 `(user_id, client_id)` 解析为 `session_id`
- 再构造 `/session/{session_id}/{key}` 路径查询 DataBus

### 2.4 这样设计的原因

若将 DataBus 的内部 key 结构直接暴露给外部，会产生两个问题：

1. **强迫外部调用方理解内部实现**：调用方需要知道 session_id 的存在和含义，需要从某处获取它
2. **破坏封装**：DataBus 的 key 结构（pool index 形式、命名空间约定）成为外部契约，无法在不破坏兼容性的前提下改变内部实现

正确做法：**daemon RPC handler 是外部 API 和内部 DataBus 之间的适配层**，负责参数转换和寻址。

## 3. 约束与假设

- **生效条件**：daemon 已启动，`MMAgentRuntime` 已初始化且至少一个 agent 已注册，`SessionRouter` 可访问
- **依赖**：`MMAgentRuntime::get_databus(agent_id)`、`MMAgentRuntime::get_agent(agent_id)`、`SessionRouter::get_session_by_route_key()`、`MMDataBus::get(key)`、`MMDataBus::list_keys_by_prefix(prefix)`
- **兼容性**：新增方法不影响已有 RPC 方法
- **安全问题**：目前不做权限校验——任何通过 TCP 连接的客户端都可以查任意 agent_id 的数据。生产部署应在 RPC 层增加鉴权

## 4. 方案简述

### 4.1 外部 API

在 `rpc_handler.cpp` 中新增两个 handler 函数，注册到 `register_all()` 中：

- `handle_databus_get`：接收 `{agent_id, (user_id+client_id | session_id), key}` → 若传了 user_id+client_id 则内部通过 SessionRouter 解析 session_id → 构建完整键路径 `/session/{session_id}/{key}` → 调 DataBus → 返回结果
- `handle_databus_list_keys`：同理

**优先使用 `(user_id, client_id)` 寻址，`session_id` 作为 fallback 保留（给内部工具和测试用）。**

### 4.2 `poll_response` 的修正

之前版本在 `handle_agent_poll_response` 中增加了 `session_id` 返回字段。根据上述设计意图，**这个改动应该撤回**——外部调用方不需要 session_id，它有 `(user_id, client_id)` 就已足够。`databus.get/list_keys` 接受 `(user_id, client_id)` 后，调用方可通过熟悉的身份标识直接查询。

## 5. 当前实现分析

### 5.1 代码分析

**现有 RPC handler 注册流程**（`rpc_handler.cpp:96-164`）：

```
register_all(RpcServer& server)
  → server.register_handler("health", ...)
  → server.register_handler("runtime.status", ...)
  → server.register_handler("agent.list", ...)
  → ... 共 18 个 handler
  → server.register_handler("metrics.get", ...)
```

每个 handler 通过 lambda 绑定到 `RpcHandler` 的成员函数，签名统一为 `(const std::string& params, int64_t id)`。

**RpcHandler 构造**（`rpc_handler.h` / `daemon.cpp:145`）：

```cpp
rpc_handler_ = std::make_unique<RpcHandler>(*runtime_, *registry_,
                                            *response_poller_, *event_broker_);
```

`RpcHandler` 持有 `MMAgentRuntime& runtime_` 和 `AgentRegistry& registry_`，可获取 agent 对象和 session router。

**SessionRouter 接口**（`session_router.h:60`）：

```cpp
std::shared_ptr<MMAgentSession> get_session_by_route_key(const std::string& route_key) const;
```

通过 `route_key = user_id + ":" + client_id` 查找已绑定的 session。

**MMDataBus 接口**（`databus.h:47-55`）：

```cpp
bool get(const std::string& key, DataVariant& value) const;
std::vector<std::string> list_keys_by_prefix(const std::string& prefix) const;
```

**键路径结构**（`uds_server.cpp:700-704`）：

```cpp
std::string UDSServer::build_key(session_id, namespace_str, key) {
  return "/session/" + session_id + "/" + namespace_str + "/" + key;
}
```

### 5.2 关键变更点

| 变更点 | 说明 |
|--------|------|
| 修改 `handle_databus_get` | 新增 `user_id`+`client_id` 参数；优先用它们查 session，再查 DataBus |
| 修改 `handle_databus_list_keys` | 同上 |
| 撤回 `poll_response` 的 session_id 改动 | 废除之前的不正确设计；调用方不需要 session_id |
| 注册到 `register_all` | 已在 `metrics.get` 之后注册 |

## 6. 数据结构设计

无需新增数据结构。复用已有：

- `SessionRouter::get_session_by_route_key(route_key)` → `MMAgentSession` → `get_session_id()`
- `MMDataBus` 的键路径格式：`/session/{session_id}/{namespace}/{key}`
- JSON-RPC 请求/响应格式：与现有 handler 一致（`extract_json_string` / `build_success_response`）

## 7. 流程设计

### 7.1 通过 user_id+client_id 查询（推荐方式）

```
Bridge                          Daemon TCP                        SessionRouter    MMDataBus
  │                                │                                  │               │
  │── databus.list_keys ──────────→│                                  │               │
  │  {agent_id,                    │                                  │               │
  │   user_id, client_id,          │                                  │               │
  │   prefix}                      │                                  │               │
  │                                │── router.get_session_by_route(   │               │
  │                                │    user_id+":"+client_id) ──────→│               │
  │                                │←─ session( session_id="0" ) ────│               │
  │                                │── databus.list_keys_by_prefix(   │               │
  │                                │    "/session/0/"+prefix) ───────→│               │
  │                                │←─ [keys...] ────────────────────│               │
  │←─ {keys: [...]} ──────────────│                                  │               │
```

### 7.2 通过 session_id 查询（fallback，仅内部工具/测试使用）

```
  │── databus.list_keys ──────────→│                                  │               │
  │  {agent_id,                    │                                  │               │
  │   session_id,                  │                                  │               │
  │   prefix}                      │                                  │               │
  │                                │── databus.list_keys_by_prefix(   │               │
  │                                │    "/session/{sid}/"+prefix) ───→│               │
  │                                │←─ [keys...] ────────────────────│               │
```

## 8. 接口设计

### 8.1 寻址参数说明

两个方法都支持两种寻址方式，**二选一**：

| 参数组 | 适用场景 | 示例调用方 |
|--------|---------|-----------|
| `user_id` + `client_id` | 推荐。外部客户端（bridge/web）使用其已知的身份标识 | `agent.execute` 的调用方 |
| `session_id` | fallback。内部工具、测试、知道 session_id 的场景 | `test_e2e.py` 直接测试 |

优先级：`(user_id+client_id)` > `session_id`。若同时传了两种，优先用 `(user_id+client_id)` 解析。

### 8.2 `databus.get`

请求：

```json
{
  "jsonrpc": "2.0",
  "method": "databus.get",
  "params": {
    "agent_id": "harness-agent",
    "user_id": "user123",
    "client_id": "cli456",
    "key": "llm/traces/call-1/meta"
  },
  "id": 1
}
```

或使用 session_id：

```json
{
  "params": {
    "agent_id": "harness-agent",
    "session_id": "0",
    "key": "llm/traces/call-1/meta"
  }
}
```

响应（成功）：

```json
{
  "jsonrpc": "2.0",
  "result": {
    "exists": true,
    "value": { "latency_ms": 1234, "model": "gpt-4o-mini" }
  },
  "id": 1
}
```

响应（键不存在）：

```json
{
  "jsonrpc": "2.0",
  "result": { "exists": false, "value": null },
  "id": 1
}
```

错误码：

| 错误码 | 条件 |
|:------:|------|
| -32602 | 缺少 agent_id / key；缺少(user_id+client_id) 和 session_id |
| -32001 | agent_id 未注册 |
| -32002 | 通过(user_id+client_id)查询但未找到 session |

### 8.3 `databus.list_keys`

请求：

```json
{
  "jsonrpc": "2.0",
  "method": "databus.list_keys",
  "params": {
    "agent_id": "harness-agent",
    "user_id": "user123",
    "client_id": "cli456",
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
      "llm/traces/call-1/meta"
    ]
  },
  "id": 2
}
```

## 9. 实现方案

### 9.1 daemon 侧：`rpc_handler.cpp`

新增内部辅助函数 `resolve_session_id`，统一处理寻址逻辑：

```cpp
// 解析 session_id：优先用 user_id+client_id 查 SessionRouter，fallback 到 session_id 参数
std::string RpcHandler::resolve_session_id(
    const std::string& params,
    const std::shared_ptr<framework::MMAgent>& agent)
{
    std::string user_id = extract_json_string_local(params, "user_id");
    std::string client_id = extract_json_string_local(params, "client_id");
    std::string session_id = extract_json_string_local(params, "session_id");

    if (!user_id.empty() && !client_id.empty()) {
        auto router = agent->get_session_router();
        if (router) {
            auto session = router->get_session_by_route_key(user_id + ":" + client_id);
            if (session) return session->get_session_id();
            // 路由存在但 session 未找到 → 调用方身份无效
        }
    }
    return session_id;  // fallback：直接用传入的 session_id
}
```

`handle_databus_get` 和 `handle_databus_list_keys` 的实现：

```cpp
std::string RpcHandler::handle_databus_get(const std::string& params, int64_t id) {
    std::string agent_id = extract_json_string_local(params, "agent_id");
    std::string key = extract_json_string_local(params, "key");
    if (agent_id.empty() || key.empty()) {
        return build_error(-32602, "Invalid params: missing agent_id or key", id);
    }

    auto agent = registry_.find(agent_id);
    if (!agent) return build_error(-32001, "Agent not found: " + agent_id, id);

    std::string session_id = resolve_session_id(params, agent);
    if (session_id.empty()) {
        return build_error(-32602, "Invalid params: provide (user_id+client_id) or session_id", id);
    }

    std::string full_key = "/session/" + session_id + "/" + key;
    auto& databus = runtime_.get_databus(agent_id);
    DataVariant value;
    bool exists = databus.get(full_key, value);
    // ... 构建响应
}
```

`handle_databus_list_keys` 同理。

### 9.2 撤回 `poll_response` 的 session_id 字段

`handle_agent_poll_response` 中之前添加的 user_id/client_id 解析逻辑和 session_id 返回应移除。外部调用方不需要通过 poll_response 获取 session_id，因为 databus RPC 接受 (user_id, client_id) 作为身份标识。

### 9.3 Bridge 侧

`bridge/routes/llm_trace.py` 中调用 daemon RPC 时，传入 `user_id` 和 `client_id` 参数替代 `session_id`。调用方从当前聊天的上下文中获取 `user_id`+`client_id`：

```python
result = await daemon.send_request("databus.list_keys", {
    "agent_id": agent_id,
    "user_id": user_id,
    "client_id": client_id,
    "prefix": "llm/traces/",
})
```

### 9.4 待修改文件清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/daemon/rpc_handler.cpp` | 修改 | 新增 `resolve_session_id`；修改 `handle_databus_get/list_keys` 支持用户身份寻址；撤回 `poll_response` 的 session_id 字段 |
| `src/daemon/rpc_handler.h` | 修改 | 新增 `resolve_session_id` 方法声明 |
| `bridge/routes/llm_trace.py` | 修改 | 传入 `(user_id, client_id)` 替代 `session_id` |
| `examples/agent/outfit-assistant/tests/test_e2e.py` | 修改 | 使用 `(user_id, client_id)` 查询 trace，移除 session_id 依赖 |

### 9.5 功能点实现优先级

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | `resolve_session_id` + daemon handler 修改 | 无副作用，新增寻址路径 | 编译通过 + E2E 测试验证 |
| P2 | 撤回 `poll_response` 的 session_id | 需要确认无其他调用方依赖此字段 | 搜索 `session_id` 在 bridge/web 中的引用 |
| P3 | Bridge 侧路由切换 | 传递参数变更 | Python import 检查 |

## 10. 测试方案

### 10.1 测试用例设计

| 编号 | 用例 | 输入 | 预期结果 |
|------|------|------|---------|
| TC1 | `databus.get` 通过 user_id+client_id 查询 | user_id+client_id 对应活跃 session | 返回 `exists: true` + value |
| TC2 | `databus.get` 通过 session_id 查询（fallback） | session_id 有效且有数据 | 返回 `exists: true` + value |
| TC3 | `databus.get` 两种寻址都不传 | 只传 agent_id 和 key | 返回 `-32602` 错误 |
| TC4 | `databus.get` user_id+client_id 无对应 session | 路由不存在 | 返回 `exists: false` |
| TC5 | `databus.list_keys` 通过 user_id+client_id 查询 | prefix 匹配部分键 | 返回匹配的键列表 |
| TC6 | `databus.list_keys` 通过 session_id 查询 | 同上 | 同 TC5 |
| TC7 | E2E 多轮：发送消息 → `databus.list_keys`（user+client）| 先 execute，再 list_keys | 返回 ≥1 条 trace |
| TC8 | `poll_response` 不再返回 session_id | 调用 poll_response | 响应中无 session_id 字段 |

## 11. 文档

### 11.1 需要变更的文档

| 文档 | 变更说明 |
|------|---------|
| 本文档 | 当前修改 |
| `bridge/docs/design/adrs/0004-llm-trace-data-bus-bridge.md` | 已更新：取消阶段 1，统一为 daemon 代理 |
| `bridge/docs/design/architecture/modules/llm-trace.md` | 已更新：新增 daemon RPC 协议定义（§5.2） |
| `docs/enhancements/README.md` | 追加此 enhancement 的索引行 |

### 11.2 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [ADR-0004: LLM Trace 方案](../bridge/docs/design/adrs/0004-llm-trace-data-bus-bridge.md) | 本 enhancement 实现 ADR-0004 的阶段 2（daemon RPC 代理） |
| [LLM Trace 模块设计](../bridge/docs/design/architecture/modules/llm-trace.md) | Bridge 侧的接口定义和时序设计与本 enhancement 配套 |

## 12. 进展跟踪

- [ ] 设计文档确认
- [ ] 代码实现
  - [ ] src/daemon/rpc_handler.cpp — 修改 handler + 新增 resolve_session_id
  - [ ] src/daemon/rpc_handler.h — 新增方法声明
  - [ ] 撤回 handle_agent_poll_response 的 session_id 字段
  - [ ] bridge/routes/llm_trace.py — 切换为 (user_id, client_id) 参数
  - [ ] examples/agent/outfit-assistant/tests/test_e2e.py — 更新 trace 查询方式
- [ ] 编译验证
- [ ] E2E 测试验证
- [ ] 文档更新
  - [x] bridge/docs/design/adrs/0004-llm-trace-data-bus-bridge.md
  - [x] bridge/docs/design/architecture/modules/llm-trace.md
  - [ ] docs/enhancements/README.md — 索引行

## 13. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-06-25 | design-agent | 初始版本 |
| v2.0 | 2026-06-25 | sisyphus | 修正设计意图：DataBus 是内部组件，外部 API 应通过 SessionRouter 桥接 `(user_id, client_id)`；撤回 `poll_response` 的 session_id 暴露 |
