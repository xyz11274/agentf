# Daemon 监控 API 接口契约

> **层级**: L4 · `architecture/modules/daemon-api.md`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What

本文档定义 Daemon 监控模块的 API 接口契约和缓存数据结构。

监控模块采用**后台缓存 + 无阻塞请求**模式：路由端点直接从 `app.state.daemon_status_cache` 读取缓存返回，不发起任何 daemon 请求。缓存由 `main.py` 的 lifespan hook 周期性刷新。

---

## 1. API 端点

### 1.1 获取完整状态快照

```
GET /api/daemon/status
```

**说明**: 返回 daemon 连接状态、版本、运行时长、活动 Agent 计数及最后更新时间戳。

**响应**（无需 `ApiResponse` 信封，直接返回 JSON）：

```json
{
  "running": true,
  "daemon": {
    "status": "ok",
    "version": "0.1.0",
    ……
  },
  "active_agents": 3,
  "uptime_seconds": 86400,
  "last_updated": 1719298800.0,
  "error": null
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `running` | bool | daemon 是否可达 |
| `daemon` | object | daemon `health` + `runtime_status` 的合并 payload |
| `active_agents` | int | 当前活跃 Agent 数 |
| `uptime_seconds` | int | daemon 运行时长（秒） |
| `last_updated` | float | 缓存最后更新时间戳（`time.time()`） |
| `error` | string | daemon 不可达时的错误消息；正常时为 `null` |

**错误时的响应**（daemon 不可达）：

```json
{
  "running": false,
  "daemon": null,
  "active_agents": 0,
  "uptime_seconds": 0,
  "last_updated": 1719298800.0,
  "error": "WebSocket connection failed: Connection refused"
}
```

---

### 1.2 获取精简健康探针

```
GET /api/daemon/uptime
```

**说明**: 轻量级健康检查端点，适合 K8s liveness probe 或外部监控系统。

**响应**：

```json
{
  "uptime_seconds": 86400,
  "running": true
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `uptime_seconds` | int | daemon 运行时长（秒） |
| `running` | bool | daemon 是否可达 |

---

## 2. 缓存机制

### 2.1 缓存数据结构

`app.state.daemon_status_cache` 是 `dict[str, Any]`，定义如下：

```
{
    "running": bool,          // daemon 是否可达
    "daemon": dict | None,    // daemon health + runtime_status 合并 payload
    "active_agents": int,     // 活跃 Agent 数
    "uptime_seconds": int,    // 运行时长（秒）
    "last_updated": float,    // 时间戳（time.time()）
    "error": str | None,      // 错误描述
}
```

初始状态（`EMPTY_CACHE`）：

```python
EMPTY_CACHE = {
    "running": False,
    "daemon": None,
    "active_agents": 0,
    "uptime_seconds": 0,
    "last_updated": 0.0,
    "error": None,
}
```

### 2.2 缓存刷新流程

`update_daemon_status_cache(state, client)` 是唯一的写入口：

```
1. 读取或初始化 cache 对象
2. 调用 client.health()
   ├─ 失败 → 写入 running=False, error=消息，返回
   └─ 成功 → 继续
3. 调用 client.runtime_status()
   ├─ 失败 → 写入 running=False, error=消息，返回
   └─ 成功 → 继续
4. 合并 health + runtime_status 为 daemon payload
5. 写入 running=True, daemon=payload, active_agents, uptime_seconds, last_updated
```

### 2.3 后台轮询循环

lifespan 启动的 `_daemon_status_poll_loop`：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 刷新间隔 | `daemon_status_refresh_interval` = 10s | 两次缓存刷新之间的间隔 |
| 空闲切片 | 0.5s | 循环内睡眠切片粒度，保证 shutdown 响应迅速 |
| 自动重连 | 是 | 检测到客户端断开时自动调用 `client.connect()` |

### 2.4 隔离性保证

- Daemon 离线时 Bridge HTTP 服务器完全不受影响
- 缓存返回最后已知状态（`running: false` + 错误消息）
- 路由自身不发起任何 daemon 请求
- 后台循环中 `health()` 和 `runtime_status()` 独立失败：`health` 失败则不请求 `runtime_status`

---

## 3. 关键配置

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `daemon_status_refresh_interval` | 10.0s | 后台缓存刷新间隔 |

---

## 4. 端点汇总

| 方法 | 路径 | 延迟 | 缓存依赖 |
|------|------|------|----------|
| GET | `/api/daemon/status` | 毫秒级（无 daemon 请求） | 是 |
| GET | `/api/daemon/uptime` | 毫秒级（无 daemon 请求） | 是 |

---

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 端点定义 | 本文档 §1 |
| 缓存数据结构 | 本文档 §2.1 |
| 缓存刷新流程 | 本文档 §2.2 |
| 状态缓存 ADR | `adrs/0003-daemon-status-cache.md` L3 |
| 功能设计 | `features/daemon-monitoring.md` L2 |
| 源码实现 | `frontend/bridge/routes/daemon.py` |

**相关文档**:
- `docs/design/features/daemon-monitoring.md` — Daemon 监控功能设计（L2）
- `docs/design/adrs/0003-daemon-status-cache.md` — 缓存策略架构决策
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
