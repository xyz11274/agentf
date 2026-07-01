# ADR-0002: 单 WebSocket 长连接共享所有 daemon 请求

> **层级**: L3 · `docs/design/adrs/0002-single-websocket-connection.md`
> **状态**: Accepted
> **创建者**: `design-doc-organizer`
> **创建日期**: 2026-06-25

---

## Context

Bridge 需要与 `agentflowd` daemon 通信，发送 JSON-RPC 请求并接收响应。需要考虑连接模型：是为每个请求创建新连接，还是维护一个长连接共享所有请求？

## Decision

维护 **单个长连接 WebSocket**，所有请求（Agent 管理、聊天、会话管理、健康检查）共享同一连接。

## Consequences

- ✅ 单连接意味着只有一个 reader loop，减少连接开销
- ✅ 请求通过递增 `request_id` 区分，`_pending` 字典分派，天然支持并发
- ✅ 后台通知消息（`agent.poll_response`）通过 `_notification_queue` 统一消费
- ⚠️ `_reader_loop` 是单点故障——连接断开时所有 pending 请求都会失败
- ⚠️ 需要实现自动重连逻辑（已在 `_daemon_status_poll_loop` 中实现）

## Alternatives Considered

- **每个请求一个连接**: 简单但开销大（TCP 握手 + WS upgrade），不适合流式聊天场景
- **连接池**: 多个长连接复用，复杂度高，在单进程场景下收益不大
