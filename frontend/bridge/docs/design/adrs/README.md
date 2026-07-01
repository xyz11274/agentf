# Bridge 架构决策记录（ADR）

> **层级**: L3 · `docs/design/adrs/`
> **创建者**: `design-doc-organizer`
> **更新日期**: 2026-06-25

---

## 什么是 ADR

架构决策记录（Architecture Decision Record）用于记录 Bridge 项目中每个重要技术决策的背景、取舍和结论。每篇 ADR 遵循 `NNNN-title-with-slug.md` 命名规则。

## ADR 索引

| 编号 | 标题 | 状态 | 日期 |
|------|------|------|------|
| 0001 | [FastAPI 框架选型](0001-fastapi-framework.md) | Accepted | 2026-06-25 |
| 0002 | [单 WS 长连接通信](0002-single-websocket-connection.md) | Accepted | 2026-06-25 |
| 0003 | [Daemon 状态缓存策略](0003-daemon-status-cache.md) | Accepted | 2026-06-25 |

## ADR 模板

每篇 ADR 包含以下章节：

- **Title**: 决策标题（`ADR-NNNN: 决策标题`）
- **Status**: Proposed / Accepted / Deprecated / Superseded
- **Context**: 决策背景和动因
- **Decision**: 选定的方案
- **Consequences**: 后果和影响
- **Alternatives Considered**: 备选方案（可选）

---

**相关文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
