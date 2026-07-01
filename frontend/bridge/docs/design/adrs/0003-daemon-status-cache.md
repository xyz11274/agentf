# ADR-0003: Daemon 状态缓存策略（后台刷新，路由不阻塞）

> **层级**: L3 · `docs/design/adrs/0003-daemon-status-cache.md`
> **状态**: Accepted
> **创建者**: `design-doc-organizer`
> **创建日期**: 2026-06-25

---

## Context

Bridge 提供 Daemon 监控端点（`/api/daemon/status`、`/api/daemon/uptime`），前端 `daemonStore` 会高频轮询。同时监控系统（K8s liveness probe、uptime monitor）也会定期访问。

如果每个请求都实时查询 daemon，后果：
- Daemon 延迟会使 Bridge HTTP 响应变慢
- 大量轮询请求会放大 daemon 负载
- Daemon 不可达时监控端点会全部超时

## Decision

采用 **后台缓存 + 路由无阻塞** 策略：

1. FastAPI lifespan hook 启动一个后台 `asyncio.Task`
2. 后台任务以固定间隔（默认 10s）调用 daemon 刷新缓存
3. 路由端点直接读取缓存返回，不发起任何 daemon 请求
4. 后台循环中检测到连接断开时自动调用 `client.connect()` 重连
5. 两次 daemon 调用（`health`、`runtime_status`）独立失败处理

## Consequences

- ✅ 路由响应时间恒定（毫秒级），不受 daemon 延迟影响
- ✅ Daemon 离线时 HTTP 端点仍可正常返回（`running: false`）
- ✅ 自愈：daemon 恢复后后台循环自动重连并刷新缓存
- ⚠️ 监控数据有最多 10s 的滞后（与刷新间隔相当）
- ❌ `daemon_status_refresh_interval` 的默认值（10s）需要在实时性和 daemon 负载之间权衡
