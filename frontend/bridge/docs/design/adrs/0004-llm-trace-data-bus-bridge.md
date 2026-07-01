# ADR-0004: LLM 执行轨迹通过 DataBus + Bridge 方案暴露

> **层级**: L3 · `docs/design/adrs/0004-llm-trace-data-bus-bridge.md`
> **状态**: Accepted
> **创建者**: `design-detailed-design-doc-writer`
> **更新日期**: 2026-06-25
> **更新原因**: 取消阶段 1（Bridge 直连 DataBus UDS），统一走 daemon RPC 代理路径

---

## Context

Harness Agent (`llm_think_and_act.py`) 内部通过 `requests.post()` 直接调用 LLM API，每次调用的 prompt、response、latency、token 用量对外完全不可见。需要建立一条通道将这些执行轨迹暴露给 harness 评测与调优工具。

核心约束：
- Bridge 是外部消费者唯一入口（HTTP REST + WebSocket）
- DataBus 是 Agent 唯一的轨迹存储（Agent 通过 Python SDK 写入，C++ 侧通过 `MMDataBus` 管理）
- Bridge 当前通过 TCP JSON-RPC 与 daemon 通信
- `databus.*` handler 未在 `rpc_handler.cpp` 中注册，需新增
- **Web 前端需要按 session_id 动态查询不同 session 的 trace**，不允许绑定固定 UDS 路径

## Decision

选择 **方案 A: DataBus 记录 + Daemon RPC 代理查询**。

**单阶段实施，无阶段拆分**。Bridge 不直连 DataBus UDS，而是通过已有的 TCP JSON-RPC 连接向 daemon 发送 `databus.get` / `databus.list_keys` 请求，daemon 内部通过 `MMAgentRuntime::get_databus(agent_id)` 获取对应的 `MMDataBus` 实例完成查询。

注册 handler：
- `databus.get` — 读取指定键的值
- `databus.list_keys` — 按前缀列出键（替代阶段 1 方案中缺失的 `list_keys` 能力）

### 被否决：阶段 1 Bridge 直连 DataBus UDS

最初设计两阶段方案（阶段 1 Bridge 直连 UDS，阶段 2 切 daemon 代理），但在审阅中发现以下不可行因素：

| 问题 | 细节 |
|------|------|
| **UDS 路径动态生成** | DataBus UDS 是 per-worker/per-session 的动态路径（`/tmp/agentflow_{session_id}.sock`），不存在 daemon 级固定路径 |
| **无法按 session 查询** | Bridge 连接一个 UDS 只能读该 session 的数据，Web 前端需要跨 session 查询时无法工作 |
| **配置耦合** | Bridge 需要知道 UDS 路径，但 daemon 不通过 RPC 暴露该信息，只能依赖外部配置传递 |
| **生命周期不同步** | UDS 服务器随 worker 退出而销毁，Bridge 无法感知何时重连哪个路径 |

**结论**：阶段 1 仅对 per-workflow 级 UDS 场景有限可行，不适合 Web 后端查询场景。整个方案统一为 daemon RPC 代理模式。

## Consequences

- ✅ Agent Python 代码单独完成轨迹记录，不依赖 daemon 新增 RPC（Agent 通过 DataBus UDS 写入，走 Python SDK）
- ✅ Bridge 零配置变更：不依赖 `AF_DATABUS_UDS_FILE`，不新增配置项
- ✅ 动态 session 查询：daemon 通过 `runtime_.get_databus(agent_id)` 获取任意 session 的数据
- ✅ 已有 `DaemonClient.send_request()` 支持任意 method 名，Bridge 侧无需新增客户端类型
- ✅ TCP 通道已允许所有方法（`rpc_server.cpp:332: return true`），无需修改白名单
- ⚠️ 需要在 `rpc_handler.cpp` 中新增 `handle_databus_get()` 和 `handle_databus_list_keys()`，注册到 `register_all()`。涉及约 50 行 C++ 代码
- ⚠️ 轨迹数据没有自动过期策略，需要后续补充 TTL 或手动清理机制
- ⚠️ 当前 `list_keys_by_prefix` 在 `MMDataBus` 已实现，但 `uds_server.cpp` 中的 `handle_list_keys` 是未完成的 stub——这里走的是 C++ API，不经过 UDS stub

## Alternatives Considered

### 方案 B: Python 直接 HTTP 上报 Bridge

Agent 脚本在每个 `call_llm()` 完成后直接 `POST` 到 Bridge 端点。

**不采用原因**：
- Agent 脚本需要知道 Bridge 地址（环境变量注入），引入配置依赖
- Bridge 重启或离线时 Agent 的 HTTP 调用会阻塞或抛异常
- 日志采集与主流程耦合过紧，try/except 方式难以覆盖所有失败路径
- 不符合"Agent 只通过 DataBus 与框架通信"的现有架构约定

### 方案 C: 扩展 Daemon EventBroker

在 daemon C++ 层新增 `llm.execution` 事件类型，Python 通过 DataBus→EventBroker 桥接发送事件。

**不采用原因**：
- 需要修改 daemon C++ 代码（EventBroker、RpcHandler、事件类型枚举），周期长
- EventBroker 当前只轮询框架生命周期事件，与 DataBus 之间无桥接机制，需重新设计
- 方案 A 已能解决问题，没必要引入 EventBroker 的复杂度

### 方案 D: 扩展 Daemon RPC 原生支持

在 daemon 的 ResponsePoller 或新模块中监听 DataBus 的 `llm` 命名空间，通过 `agent.response` 或新通知推送。

**不采用原因**：
- ResponsePoller 当前只处理 agent response message queue，DataBus 监听机制不存在
- 需要 C++ 多线程改动，复杂度最高
- 收益与 A 方案相同（最终都通过 Bridge 暴露），但实施成本高出数倍

---

**关联文档**：
- L4 模块设计: `architecture/modules/llm-trace.md`
- Bridge 总体架构: `features/bridge-overview.md` L2
- Harness Agent 源码: `examples/agent/harness-agent/nodes/llm_think_and_act.py`
