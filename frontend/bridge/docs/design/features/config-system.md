# Bridge 配置系统功能设计

> **层级**: L2 · `docs/design/features/config-system.md`
> **状态**: 草案
> **创建日期**: 2026-06-25

---

## 1. What

Bridge 配置系统负责所有运行时配置的管理，包括：daemon 连接地址、Bridge 自身绑定地址、文件系统路径、日志级别、CORS、超时等。配置来源于环境变量 + `.env` 文件，通过 Pydantic Settings 加载为类型安全的单例对象。

## 2. Why

- 当前配置分散在 `config.py` 中，没有集中可见的清单
- 无 `.env.example` 文件，部署前需要读源码了解配置项
- 环境变量名依赖 Pydantic 隐式推导（`daemon_host` → `DAEMON_HOST`），不直观
- Skill Hub 等新增功能需配置项，需先完善配置体系
- 一些配置项（如 `bridge_port`）的默认值与 docstring 不一致

## 3. How

### 3.1 配置层次

```
配置来源优先级（低 → 高）：
┌──────────────────┐
│  代码默认值       │  ← BridgeSettings 字段的 default=
├──────────────────┤
│  .env 文件        │  ← 项目根目录或当前目录下的 .env
├──────────────────┤
│  系统环境变量     │  ← export DAEMON_HOST=10.0.0.1
└──────────────────┘
```

高优先级覆盖低优先级。所有配置在应用启动时（`lifespan` 前）一次性加载，运行时不变。

### 3.2 配置分组

配置按用途分为 6 组：

| 分组 | 前缀 | 条目数 | 典型用途 |
|------|------|--------|----------|
| 🔌 Daemon 连接 | `DAEMON_*` | 2 | daemon 地址和端口 |
| 🌐 Bridge 服务 | `BRIDGE_*` | 2 | 自身监听地址 |
| 📂 文件系统路径 | `*_DIR` | 2 | agent 包和技能仓库目录 |
| 🪵 日志 | `LOG_*` | 1 | 日志级别 |
| 🚦 超时与间隔 | `*_TIMEOUT_*` / `*_INTERVAL` | 2 | 请求超时、缓存刷新 |
| 🌍 CORS | `CORS_*` | 1 | 允许的前端来源 |

### 3.3 配置组设计

```
┌────────────────────────────────────────────────────────┐
│  BridgeSettings                                         │
│                                                        │
│  ┌─ daemon ────────────────────────────────────────┐   │
│  │  daemon_host : str       = "localhost"          │   │
│  │  daemon_port : int       = 17891                │   │
│  │  → daemon_ws_url (property)                     │   │
│  └─────────────────────────────────────────────────┘   │
│                                                        │
│  ┌─ bridge ────────────────────────────────────────┐   │
│  │  bridge_host : str       = "0.0.0.0"            │   │
│  │  bridge_port : int       = 8199                 │   │
│  │  → uvicorn.run(host=, port=)                    │   │
│  └─────────────────────────────────────────────────┘   │
│                                                        │
│  ┌─ filesystem ───────────────────────────────────┐   │
│  │  agent_packages_dir : str  = "examples/agent"   │   │
│  │  skill_hub_dir : str       = "resource/skill_   │   │
│  │                              hub"               │   │
│  │  → agent_packages_path (property)               │   │
│  │  → skill_hub_path (property)                    │   │
│  └─────────────────────────────────────────────────┘   │
│                                                        │
│  ┌─ observability ────────────────────────────────┐   │
│  │  log_level : str           = "INFO"             │   │
│  │  request_timeout_seconds : float = 5.0          │   │
│  │  daemon_status_refresh_interval : float = 10.0  │   │
│  └─────────────────────────────────────────────────┘   │
│                                                        │
│  ┌─ cors ─────────────────────────────────────────┐   │
│  │  cors_allowed_origins : list[str]               │   │
│  │    = ["http://localhost:15173",                 │   │
│  │       "http://127.0.0.1:15173"]                │   │
│  └─────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────┘
```

### 3.4 配置项全表

| # | 字段名 | 环境变量 | 类型 | 默认值 | 分组 | 用途 |
|---|--------|----------|------|--------|------|------|
| 1 | `daemon_host` | `DAEMON_HOST` | str | `"localhost"` | 🔌 Daemon | daemon 主机名 |
| 2 | `daemon_port` | `DAEMON_PORT` | int | `17891` (1-65535) | 🔌 Daemon | daemon WS 端口 |
| 3 | `bridge_host` | `BRIDGE_HOST` | str | `"0.0.0.0"` | 🌐 Bridge | FastAPI 绑定地址 |
| 4 | `bridge_port` | `BRIDGE_PORT` | int | `8199` (1-65535) | 🌐 Bridge | FastAPI 绑定端口 |
| 5 | `agent_packages_dir` | `AGENT_PACKAGES_DIR` | str | `"examples/agent"` | 📂 路径 | Agent Package 根目录 |
| 6 | `skill_hub_dir` | `SKILL_HUB_DIR` | str | `"resource/skill_hub"` | 📂 路径 | 技能中心仓库目录 |
| 7 | `log_level` | `LOG_LEVEL` | str | `"INFO"` | 🪵 日志 | Python 日志级别 |
| 8 | `request_timeout_seconds` | `REQUEST_TIMEOUT_SECONDS` | float | `5.0` (gt 0) | 🚦 超时 | daemon RPC 请求超时 |
| 9 | `daemon_status_refresh_interval` | `DAEMON_STATUS_REFRESH_INTERVAL` | float | `10.0` (gt 0) | 🚦 超时 | daemon 缓存刷新间隔 |
| 10 | `cors_allowed_origins` | `CORS_ALLOWED_ORIGINS` | list[str] | 见 §3.5 | 🌍 CORS | 允许的 CORS 来源 |

### 3.5 复杂配置说明

**cors_allowed_origins**：JSON 数组格式，环境变量中设置：

```bash
export CORS_ALLOWED_ORIGINS='["http://localhost:15173","http://127.0.0.1:15173"]'
```

默认值：
```python
[
    "http://localhost:15173",
    "http://127.0.0.1:15173",
]
```

### 3.6 派生属性

| 属性名 | 类型 | 推导逻辑 | 用途 |
|--------|------|----------|------|
| `daemon_ws_url` | str | `f"ws://{daemon_host}:{daemon_port}"` | DaemonClient 连接 |
| `agent_packages_path` | Path | 相对路径锚定项目根目录解析 | 文件系统操作 |
| `skill_hub_path` | Path | 相对路径锚定项目根目录解析 | 技能仓库文件操作 |

### 3.7 .env 文件格式

`frontend/bridge/.env`（不提交 git）或 `frontend/bridge/.env.example`（提交 git）：

```bash
# ============================================================================
# AgentFlow Bridge — 配置文件
# ============================================================================
# 此文件由 BridgeSettings 自动加载。同名的系统环境变量会覆盖此文件的值。
# 复制此文件为 .env 并修改：
#   cp .env.example .env
# ============================================================================

# --- 🔌 Daemon 连接 -----------------------------------------------------------
DAEMON_HOST=localhost
DAEMON_PORT=17891

# --- 🌐 Bridge 服务 -----------------------------------------------------------
BRIDGE_HOST=0.0.0.0
BRIDGE_PORT=8199

# --- 📂 文件系统路径（相对路径锚定到项目根目录）--------------------------------
AGENT_PACKAGES_DIR=examples/agent
SKILL_HUB_DIR=resource/skill_hub

# --- 🪵 日志 ----------------------------------------------------------------
LOG_LEVEL=INFO

# --- 🚦 超时与间隔 ------------------------------------------------------------
REQUEST_TIMEOUT_SECONDS=5.0
DAEMON_STATUS_REFRESH_INTERVAL=10.0

# --- 🌍 CORS（JSON 数组格式）------------------------------------------------
CORS_ALLOWED_ORIGINS=["http://localhost:15173","http://127.0.0.1:15173"]
```

### 3.8 模块划分

| 组件 | 位置 | 职责 |
|------|------|------|
| 配置类 | `frontend/bridge/config.py` | Pydantic Settings 定义 + 派生属性 |
| .env 模板 | `frontend/bridge/.env.example` | 所有配置项的示例值 + 注释 |
| .env 文件 | `frontend/bridge/.env` | 本地覆写（已 gitignore） |
| 配置文档 | `docs/design/architecture/modules/config-system.md` | 配置项全表 + 说明 |

---

**接口契约（配置项完整定义、环境变量名、类型与约束）请参见 `architecture/modules/config-system.md`**

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 配置分组与设计 | 本文档 §3.2–3.3 |
| 配置项全表 | 本文档 §3.4 |
| .env 文件格式 | 本文档 §3.7 |
| 派生属性 | 本文档 §3.6 |
| 类型与约束定义 | `architecture/modules/config-system.md` L4 |

**相关设计文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/architecture/modules/config-system.md` — 配置系统详细设计（L4）
- `docs/requirements/prd/config-system-prd.md` — 配置系统 PRD（L1）
