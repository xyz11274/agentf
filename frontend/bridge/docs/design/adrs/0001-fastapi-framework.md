# ADR-0001: 选择 FastAPI 作为 Web 框架

> **层级**: L3 · `docs/design/adrs/0001-fastapi-framework.md`
> **状态**: Accepted
> **创建者**: `design-doc-organizer`
> **创建日期**: 2026-06-25

---

## Context

Bridge 需要提供一个同时支持 HTTP REST 和 WebSocket 的 Web 服务，作为 Vue 前端和 agentflowd 守护进程之间的中间层。需要选择 ASGI Web 框架。

关键需求：
- 原生支持 HTTP REST + WebSocket
- 异步 I/O（与 daemon 的 WebSocket 通信需要 asyncio）
- 自动 OpenAPI 文档生成（方便前端调试）
- 类型安全（Pydantic 集成）
- 测试友好（依赖覆盖支持）

## Decision

选择 **FastAPI** 作为 Web 框架。

## Consequences

- ✅ 原生异步支持，与 `DaemonClient` 的 `asyncio` 模型一致
- ✅ 自动 OpenAPI 文档（`/docs`、`/openapi.json`）
- ✅ Pydantic v2 集成，请求/响应自动校验
- ✅ `app.dependency_overrides` 使测试 mock 非常方便
- ⚠️ 需要 `websockets` 库补充 FastAPI 的 WebSocket 支持
- ⚠️ 自动 OpenAPI 文档会暴露内部接口，生产部署应注意

## Alternatives Considered

- **Flask + flask-socketio**: 同步框架，WebSocket 支持不原生，且与 asyncio daemon 客户端集成复杂
- **Starlette**: FastAPI 的底层框架，缺少自动校验和文档能力，需额外集成 Pydantic
- **Sanic**: 功能类似，但社区规模和生态不如 FastAPI
