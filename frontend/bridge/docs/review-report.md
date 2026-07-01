# Bridge 文档体系 — 结构一致性审计报告

> **审计者**: `design-doc-organizer`
> **审计日期**: 2026-06-25
> **依据**: 设计文档组织 skill · references/review-checklists.md 参照

---

## 1. 文档清单

| # | 文件 | 层级 | 范围 | 状态 |
|---|------|------|------|------|
| 1 | `requirements/prd/prd-overview.md` | L1 | 产品范围、In/Out Scope、成功指标 | ✅ |
| 2 | `requirements/prd/reference/README.md` | L1 | 参考材料目录 | ✅ |
| 3 | `requirements/prd/versions/README.md` | L1 | 版本快照 | ✅ |
| 4 | `design/README.md` | L2 | 设计文档总索引 | ✅ |
| 5 | `design/features/bridge-overview.md` | L2 | Bridge 定位、架构总览、模块边界 | ⚠️ 见下 |
| 6 | `design/features/agent-management.md` | L2 | Agent 代理路由功能描述 | ❌ 混层 |
| 7 | `design/features/chat.md` | L2 | 聊天功能设计 | ❌ 混层 |
| 8 | `design/features/daemon-monitoring.md` | L2 | Daemon 状态缓存与监控 | ❌ 混层 |
| 9 | `design/features/package-management.md` | L2 | Package 文件 CRUD、构建 | ❌ 混层 |
| 10 | `design/features/session-management.md` | L2 | Session 代理路由 | ❌ 混层 |
| 11 | `design/adrs/README.md` | L3 | ADR 索引（空） | ⚠️ 无条目 |
| 12 | `design/architecture/README.md` | L3/L4 | 架构索引（空） | ❌ 无内容 |
| 13 | `design/archives/README.md` | — | 归档说明 | ✅ |

---

## 2. 混层检查（按 constitution 10.2）

| 文档 | 问题内容 | 问题类型 | 建议拆分去向 |
|------|----------|----------|-------------|
| `agent-management.md` | 端点路径表（GET/POST /api/agents/*）、错误代码映射表 | L3/L4 API 契约 | `architecture/modules/agent-api.md` |
| `chat.md` | 请求/响应体字段描述 `{ content, session_id?, dialogue_id? }` | L4 接口契约 | `architecture/modules/chat-api.md` |
| `daemon-monitoring.md` | Python 伪代码缓存形状数据结构定义 | L4 数据定义 | `architecture/modules/daemon-api.md` |
| `package-management.md` | 完整 REST 端点路径表 | L3/L4 API 契约 | `architecture/modules/package-api.md` |
| `session-management.md` | 端点路径表 + RPC 方法名 | L3/L4 API 契约 | `architecture/modules/session-api.md` |
| `bridge-overview.md` | ASCII 系统上下文图 | 图表未委托 | 替换为 mermaid-drawer 委托占位 |
| `chat.md` | ASCII 序列图 | 图表未委托 | 替换为 mermaid-drawer 委托占位 |

---

## 3. 索引覆盖检查

- `design/README.md` 覆盖 9 个文档入口 ✅
- 缺失对 `architecture/modules/` 子目录的索引 ❌

---

## 4. 缺失内容清单

| 缺失内容 | 建议位置 | 优先级 |
|----------|----------|--------|
| Agent API 接口契约 | `architecture/modules/agent-api.md` | 高 |
| Chat API 接口契约（WS + REST） | `architecture/modules/chat-api.md` | 高 |
| Daemon 监控 API 接口契约 | `architecture/modules/daemon-api.md` | 高 |
| Package 管理 API 接口契约 | `architecture/modules/package-api.md` | 高 |
| Session API 接口契约 | `architecture/modules/session-api.md` | 高 |
| DaemonClient 通信协议 | `architecture/modules/daemon-client-protocol.md` | 高 |
| 配置系统设计 | `architecture/modules/config-system.md` | 中 |
| ADR: FastAPI 框架选型 | `adrs/0001-fastapi-framework.md` | 中 |
| ADR: 单 WS 长连接通信 | `adrs/0002-single-websocket-connection.md` | 中 |
| ADR: Daemon 状态缓存策略 | `adrs/0003-daemon-status-cache.md` | 中 |
| CORS 中间件设计说明 | `architecture/modules/cors-config.md` | 低 |

---

## 5. 不变量检查

| 不变量 | 符合 | 说明 |
|--------|------|------|
| 不删除原文内容 | ✅ | 未执行删除操作 |
| 索引与文档一致 | ⚠️ | architecture/modules/ 缺失索引 |
| 引用可追溯 | ✅ | 所有引用路径有效 |
| 正文写作委托 | ✅ | 所有 skeleton 有委托声明 |
| 图绘制委托 | ❌ | ASCII 图未委托 mermaid-drawer |
| 层级约束 | ❌ | 5 个 L2 文档含 L3/L4 内容 |

---

## 修复计划

1. **剥离 L2 混层内容**：从 features 文档中移除端点表、伪代码、接口字段描述，改为纯功能描述骨架
2. **创建 L4 模块文档**：在 `architecture/modules/` 下创建对应接口契约文档
3. **创建 ADR**：记录 3 个关键架构决策
4. **替换 ASCII 图为委托占位**：委托 mermaid-drawer 绘制系统上下文图和序列图
5. **更新索引**：`design/README.md` 补充 architecture/modules/ 条目
