# Bridge 设计文档索引

> **层级**: L2 · `docs/design/`
> **更新日期**: 2026-06-25
> **状态**: 已填充

---

## 设计文档总览

| 类别 | 层级 | 文档 | 状态 |
|------|------|------|------|
| 产品需求 | L1 | [PRD 概述](../requirements/prd/prd-overview.md) | ✅ 已填充 |
| 产品需求 | L1 | [Skill Hub PRD](../requirements/prd/skill-hub-prd.md) | 🟡 草案 |
| 产品需求 | L1 | [配置系统 PRD](../requirements/prd/config-system-prd.md) | 🟡 草案 |
| 总体架构 | L2 | [Bridge 总览](features/bridge-overview.md) | ✅ 已填充（含 4+1 视图） |
| 功能设计 | L2 | [Agent 管理](features/agent-management.md) | ✅ 已填充 |
| 功能设计 | L2 | [聊天功能](features/chat.md) | ✅ 已填充（含双模式） |
| 功能设计 | L2 | [Daemon 监控](features/daemon-monitoring.md) | ✅ 已填充 |
| 功能设计 | L2 | [包管理](features/package-management.md) | ✅ 已填充 |
| 功能设计 | L2 | [会话管理](features/session-management.md) | ✅ 已填充 |
| 功能设计 | L2 | [Skill Hub](features/skill-hub.md) | 🟡 草案 |
| 功能设计 | L2 | [配置系统](features/config-system.md) | 🟡 草案 |
| 接口契约 | L4 | [Agent API 契约](architecture/modules/agent-api.md) | ✅ 已填充 |
| 接口契约 | L4 | [Chat API 契约](architecture/modules/chat-api.md) | ✅ 已填充（含 WS + REST） |
| 接口契约 | L4 | [Daemon API 契约](architecture/modules/daemon-api.md) | ✅ 已填充 |
| 接口契约 | L4 | [Package API 契约](architecture/modules/package-api.md) | ✅ 已填充 |
| 接口契约 | L4 | [Session API 契约](architecture/modules/session-api.md) | ✅ 已填充 |
| 通信协议 | L4 | [DaemonClient 协议](architecture/modules/daemon-client-protocol.md) | ✅ 已填充 |
| 接口契约 | L4 | [LLM Trace 模块](architecture/modules/llm-trace.md) | 🟡 草案（可行性验证） |
| 接口契约 | L4 | [Skill Hub API 契约](architecture/modules/skill-hub-api.md) | 🟡 草案 |
| 接口契约 | L4 | [配置系统](architecture/modules/config-system.md) | 🟡 草案 |
| 架构决策 | L3 | [ADR-0001: FastAPI 选型](adrs/0001-fastapi-framework.md) | ✅ Accepted |
| 架构决策 | L3 | [ADR-0002: 单 WS 长连接](adrs/0002-single-websocket-connection.md) | ✅ Accepted |
| 架构决策 | L3 | [ADR-0003: 状态缓存策略](adrs/0003-daemon-status-cache.md) | ✅ Accepted |
| 架构决策 | L3 | [ADR-0004: LLM Trace 方案](adrs/0004-llm-trace-data-bus-bridge.md) | 🟡 Proposed |
| 归档 | — | [Archives](archives/README.md) | ✅ |

---

## 层级约束

依据 `constitution.md` 第 10.2 节文档层级约束：

- **L1**: `docs/requirements/prd/` — 产品需求、范围、角色
- **L2**: `docs/design/features/` — 领域概念、业务规则、功能流程（不含代码/JSON Schema/API 定义）
- **L3**: `docs/design/adrs/` + `docs/design/architecture/` — 架构决策和系统架构总览
- **L4**: `docs/design/architecture/modules/` — 模块内部设计、API/Schema、算法、接口契约

---

## 权威章节映射

| 概念 | 权威文档 | 章节 |
|------|----------|------|
| Agent API 端点定义 | `architecture/modules/agent-api.md` | §2 / §3 |
| Agent 状态转换 | `architecture/modules/agent-api.md` | §4 |
| WS 帧格式 | `architecture/modules/chat-api.md` | §1.2 |
| REST 请求/响应 Schema | `architecture/modules/chat-api.md` | §2.2–2.3 |
| 聊天双模式选择依据 | `features/chat.md` | How |
| Package API 端点定义 | `architecture/modules/package-api.md` | §2 / §3 |
| Package 安全校验规则 | `architecture/modules/package-api.md` | §1.2 |
| Session API 参数映射 | `architecture/modules/session-api.md` | §2 |
| Daemon 缓存数据结构 | `architecture/modules/daemon-api.md` | §2.1 |
| DaemonClient 协议 | `architecture/modules/daemon-client-protocol.md` | §1–§6 |
| 错误 → HTTP 映射 | `architecture/modules/agent-api.md` | §1.2 |
| Skill Hub API 端点定义 | `architecture/modules/skill-hub-api.md` | §2 / §3 |
| Skill Hub 功能流程 | `features/skill-hub.md` | How |

---

## 相关资源

- [PRD 概述](../requirements/prd/prd-overview.md) — 产品需求
- [项目级别 docs](../../../docs/README.md) — AgentFlow 项目全局文档
- [结构一致性审计报告](../review-report.md) — 历史审阅记录
