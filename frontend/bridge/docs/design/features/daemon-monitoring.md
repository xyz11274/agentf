# Daemon 监控功能设计

> **层级**: L2 · `docs/design/features/daemon-monitoring.md`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What

Daemon 监控模块提供两个 REST 端点，返回后台缓存的 daemon 健康状态、版本、运行时长和活动 Agent 计数。缓存由 `main.py` 的 lifespan hook 周期性刷新，路由本身不发起任何 daemon 请求。

## Why

- **过载保护**: 大量监控客户端（如 K8s 探针、前端轮询）不应直接请求 daemon
- **容错**: Daemon 离线时缓存仍可返回最后已知状态
- **自愈**: 后台刷新循环在 daemon 不可达时自动重试连接
- **响应恒定**: 路由响应时间不受 daemon 延迟影响（毫秒级）

## How

### 缓存架构

```
  ┌──────────────────┐      ┌─────────────────────────────┐
  │  前端/监控客户端    │      │  Bridge                      │
  │                   │      │  ┌──────────────────┐        │
  │  GET /api/daemon/ │─────→│  │ daemon_status_cache│        │
  │  status           │←─────│  │ (dict, 无阻塞读取)  │        │
  │                   │      │  └────────┬─────────┘        │
  └──────────────────┘      │           │ 后台刷新            │
                             │           ▼                    │
                             │  ┌──────────────────┐        │
                             │  │ poll_loop         │        │
                             │  │ (每 10s 刷新)     │────────→│ agentflowd
                             │  └──────────────────┘        │  (daemon)
                             └─────────────────────────────┘
```

两层分离：

1. **后台刷新层**（`_daemon_status_poll_loop`）— 异步后台任务，定期调用 daemon
2. **路由读取层**（`daemon_status` / `daemon_uptime`）— 直接从内存字典读取，毫秒级响应

这种分离保证：路由不发起任何 daemon 请求，因此不受 daemon 延迟影响。

### 缓存生命周期

```
  应用启动                 每次间隔                     应用关闭
─────┬────────────────────────┬──────────────────────────┬──────→
     │                        │                          │
     │  lifespan:             │  poll_loop:              │  cancel task
     │  1. connect()          │  1. is_connected?         │  close client
     │  2. 首次刷新缓存       │     ├─ 否 → connect()
     │  3. 启动后台任务       │     └─ 是 → 继续
     │                        │  2. health()
     │                        │     ├─ 失败 → caching=False
     │                        │     └─ 成功 → runtime_status()
     │                        │  3. 写入缓存
```

| 阶段 | 操作 | 失败处理 |
|------|------|----------|
| 初始化 | `lifespan` 内建立连接并首次刷新缓存 | 连接失败不阻塞启动，缓存保持 EMPTY_CACHE |
| 刷新 | 每 `daemon_status_refresh_interval`（默认 10s）执行一次 | 独立失败：health 失败则不请求 runtime_status |
| 自愈 | 检测到连接断开时自动调用 `client.connect()` | 重连失败被捕获并记录，下次循环重试 |
| 关闭 | 取消后台任务，关闭 daemon 连接 | — |

### 缓存内容

缓存字典存储以下字段（详见 L4 接口契约）：

| 字段 | 类型 | 来源 | 说明 |
|------|------|------|------|
| `running` | bool | 探针结果 | daemon 是否可达 |
| `daemon` | dict | health + runtime_status | daemon 完整 payload |
| `active_agents` | int | runtime_status | 当前活跃 Agent 数 |
| `uptime_seconds` | int | runtime_status | daemon 运行时长 |
| `last_updated` | float | 系统时间戳 | 缓存最后刷新时间 |
| `error` | string | 异常捕获 | 失败时的错误描述 |

### 端点

| 端点 | 用途 | 响应时间 | 依赖 |
|------|------|----------|------|
| `GET /api/daemon/status` | 完整状态快照 | 毫秒级（无 daemon 请求） | 后台缓存 |
| `GET /api/daemon/uptime` | 精简健康探针 | 毫秒级（无 daemon 请求） | 后台缓存 |

### 错误处理策略

- **隔离性**: Daemon 离线时 Bridge HTTP 服务器完全不受影响，缓存仍可返回最后已知状态（`running: false`）
- **独立失败**: 两次 daemon 调用（`health` 和 `runtime_status`）独立处理失败。`health` 失败则不请求 `runtime_status`
- **自愈**: 后台循环未捕获 daemon 连接错误，下次循环自动重试

### 不支持项 / 边界

| 场景 | 状态 | 说明 |
|------|------|------|
| 实时 daemon 状态查询 | ❌ 不支持 | 默认只有缓存数据，滞后 ≤ 10s |
| 主动 daemon 健康探测端点 | ❌ 不支持 | 所有监控端点只读缓存 |
| 自定义刷新间隔 | ✅ 支持 | 通过 `daemon_status_refresh_interval` 配置 |

---

**接口契约（缓存 JSON 形状、端点响应格式）请参见 `architecture/modules/daemon-api.md`**

**相关设计文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/adrs/0003-daemon-status-cache.md` — 缓存策略架构决策
- `docs/design/architecture/modules/daemon-api.md` — Daemon 监控 API 契约（L4）
