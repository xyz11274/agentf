# Agent 管理功能设计

> **层级**: L2 · `docs/design/features/agent-management.md`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What

Agent 管理是一组 REST 端点，作为 HTTP-to-JSON-RPC 代理，将前端请求透传到 daemon 的 `agent.*` RPC 方法。它不缓存、不做业务逻辑、不避免任何 daemon 往返。

## Why

前端 `agentStore`（Vue Pinia store）通过 HTTP 调用管理 Agent 生命周期。Bridge 作为中间层，需要将 HTTP 请求转换为 daemon 可理解的 JSON-RPC 格式，并将错误映射为适当的 HTTP 状态码。

代理层隔离了前端对 daemon WS/JSON-RPC 的直接依赖，并使前端可以用标准 HTTP 语义（GET/POST）操作 Agent。

## How

### 功能流程

Agent 管理模块是纯 HTTP-to-JSON-RPC 代理，其流程如下：

```
前端 (Vue agentStore)     Bridge                  agentflowd (daemon)
       │                     │                          │
       │ GET /api/agents     │                          │
       │────────────────────→│                          │
       │                     │── agent.list ───────────→│
       │                     │←── agent list ───────────│
       │←─ 200 {data: [...]} │                          │
       │                     │                          │
       │ POST /api/agents/   │                          │
       │   {id}/load         │                          │
       │────────────────────→│                          │
       │                     │── agent.load ───────────→│
       │                     │   (package_path)         │
       │                     │←── load result ─────────│
       │←─ 200 {data: {...}} │                          │
       │                     │                          │
       │ POST /api/agents/   │                          │
       │   {id}/pause        │                          │
       │────────────────────→│                          │
       │                     │── agent.pause ──────────→│
       │                     │←── pause result ────────│
       │←─ 200 {data: {...}} │                          │
```

所有 6 个端点遵循同一透传模式：

1. 前端通过 HTTP 请求操作 Agent（列出、详情、加载、卸载、暂停、恢复）
2. Bridge 将 HTTP 请求转换为对应的 daemon JSON-RPC 调用
3. Daemon 执行操作并返回结果
4. Bridge 将 daemon 返回结果包装为标准 `{success, data, error}` 格式返回前端
5. 如果 daemon 返回错误，Bridge 按错误类型映射为适当的 HTTP 状态码

### 关键设计点

- **纯透传代理**：不缓存 Agent 元数据，每次请求都经过 daemon
- **错误映射**：Daemon 错误类按语义映射为 HTTP 4xx/5xx（见 L4 接口契约 §1.2）
- **默认路径**：`load` 操作支持省略 `package_path`，自动拼接 `{agent_packages_dir}/{agent_id}`，前端只需传包目录名

### 端点一览

| 方法 | 路径 | 对应 RPC | 用途 |
|------|------|----------|------|
| GET | `/api/agents` | `agent.list` | 列出所有已加载 Agent |
| GET | `/api/agents/{id}` | `agent.info` | 获取单个 Agent 详情 |
| POST | `/api/agents/{id}/load` | `agent.load` | 加载 Agent（从磁盘到内存） |
| POST | `/api/agents/{id}/unload` | `agent.unload` | 卸载 Agent（从内存清除） |
| POST | `/api/agents/{id}/pause` | `agent.pause` | 暂停 Agent |
| POST | `/api/agents/{id}/resume` | `agent.resume` | 恢复被暂停的 Agent |

### 状态转换

```
  |--- load ---> loaded --- unload ---> unloaded
                    |
              agent.execute
                    |
                 running
                    |
              pause/resume
                    |
                 paused
```

- `loaded`：已加载到内存，可执行工作流
- `running`：正在执行工作流
- `paused`：已暂停，不接受新执行请求
- `unloaded`：未加载

`list` 和 `info` 是只读查询，不改变 Agent 状态。

### 不支持项 / 边界

| 场景 | 状态 | 说明 |
|------|------|------|
| Agent 元数据缓存 | ❌ 不支持 | 每次请求都经过 daemon |
| Agent 创建/注册 | ❌ 不支持 | Agent 由 daemon 管理，Bridge 不提供创建端点 |
| 批量操作 | ❌ 不支持 | 无批量 load/unload 端点 |

---

**接口契约（端点路径、请求/响应 Schema、错误代码映射）请参见 `architecture/modules/agent-api.md`**

**相关设计文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/features/session-management.md` — 会话管理与 Agent 的关系
- `docs/design/architecture/modules/agent-api.md` — Agent API 接口契约（L4）
