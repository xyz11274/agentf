# Agent API 接口契约

> **层级**: L4 · `architecture/modules/agent-api.md`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What

本文档定义 Bridge Agent 管理模块的完整 API 接口契约，包含端点路径、请求/响应体格式、错误代码映射和 JSON-RPC 映射关系。

所有端点均为纯 HTTP-to-JSON-RPC 代理——不缓存、不做业务逻辑、每次请求都经过 daemon。

---

## 1. 全局约定

### 1.1 响应信封

所有端点统一返回以下信封：

```json
{
  "success": true,
  "data": { … },
  "error": null
}
```

- 成功时：`success: true`，结果在 `data` 中
- 失败时：`success: false`，错误消息在 `error` 中，HTTP 状态码反映错误类型

### 1.2 Daemon 错误 → HTTP 状态码映射

| 错误场景 | 异常类 | HTTP |
|----------|--------|------|
| 无效参数 | `InvalidParamsError` | 400 Bad Request |
| 资源不存在 | `NotFoundError` | 404 Not Found |
| 请求超时 | `DaemonTimeoutError` | 504 Gateway Timeout |
| Daemon 连接失败 | `DaemonConnectionError` | 502 Bad Gateway |
| 其他 Daemon 错误 | `DaemonError` | 500 Internal Server Error |

---

## 2. API 端点

### 2.1 列出已加载 Agent

```
GET /api/agents
```

**对应 RPC**: `agent.list`

**请求参数**: 无

**响应 `data`**:

```json
[
  {
    "id": "harness-agent",
    "name": "Harness Agent",
    "workflow_count": 5,
    "status": "loaded"
  }
]
```

`data` 是 `AgentInfo` 数组：

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | string | Agent 唯一标识符 |
| `name` | string | 展示名称 |
| `workflow_count` | int | 已注册工作流数量 |
| `status` | string | 枚举值：`"loaded"` / `"running"` / `"paused"` / `"unloaded"` |

---

### 2.2 获取单个 Agent 详情

```
GET /api/agents/{agent_id}
```

**对应 RPC**: `agent.info`

**路径参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `agent_id` | string | Agent 标识符 |

**响应 `data`**: 单个 `AgentInfo` 对象（格式同 §2.1）。

---

### 2.3 加载 Agent

```
POST /api/agents/{agent_id}/load
```

**对应 RPC**: `agent.load`

**路径参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `agent_id` | string | Agent 标识符 |

**请求体**:

```json
{
  "package_path": "harness-agent"
}
```

`_LoadAgentBody`：

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `package_path` | string | 否 | 同 `agent_id` | 包目录名；省略时自动拼接 `{agent_packages_dir}/{agent_id}` |

**响应 `data`**: daemon 返回的原始 load 结果。

---

### 2.4 卸载 Agent

```
POST /api/agents/{agent_id}/unload
```

**对应 RPC**: `agent.unload`

**路径参数**: `agent_id`

**请求体**: 无

**响应 `data`**: daemon 返回的原始 unload 结果。

---

### 2.5 暂停 Agent

```
POST /api/agents/{agent_id}/pause
```

**对应 RPC**: `agent.pause`

**路径参数**: `agent_id`

**请求体**: 无

**响应 `data`**: daemon 返回的原始 pause 结果。

---

### 2.6 恢复 Agent

```
POST /api/agents/{agent_id}/resume
```

**对应 RPC**: `agent.resume`

**路径参数**: `agent_id`

**请求体**: 无

**响应 `data`**: daemon 返回的原始 resume 结果。

---

## 3. 端点汇总

| 方法 | 路径 | 对应 RPC | 请求体 |
|------|------|----------|--------|
| GET | `/api/agents` | `agent.list` | 无 |
| GET | `/api/agents/{agent_id}` | `agent.info` | 无 |
| POST | `/api/agents/{agent_id}/load` | `agent.load` | `{package_path?: string}` |
| POST | `/api/agents/{agent_id}/unload` | `agent.unload` | 无 |
| POST | `/api/agents/{agent_id}/pause` | `agent.pause` | 无 |
| POST | `/api/agents/{agent_id}/resume` | `agent.resume` | 无 |

---

## 4. Agent 状态转换

```
  |--- load ---> loaded --- unload ---> unloaded
                    |
                 execute
                    |
                 running
                    |
              pause/resume
                    |
                 paused
```

- `load` 将 Agent 从磁盘加载到内存（状态 → `loaded`）
- `unload` 从内存卸载（状态 → `unloaded`）
- Agent 在 `execute` 时临时进入 `running`
- `pause` / `resume` 在 `loaded` 和 `paused` 之间切换
- 端点 `list` / `info` 为只读查询，不改变状态

---

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 端点定义 | 本文档 §2 / §3 |
| 错误码映射 | 本文档 §1.2 |
| Agent 状态模型 | 本文档 §4 |
| 功能流程 | `features/agent-management.md` L2 |
| Pydantic 模型定义 | `frontend/bridge/models.py` |

**相关文档**:
- `docs/design/features/agent-management.md` — Agent 管理功能设计（L2）
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/architecture/modules/session-api.md` — 会话管理 API（会话隶属于 Agent）
