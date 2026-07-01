# Bridge 配置系统 — 详细设计

> **层级**: L4 · `architecture/modules/config-system.md`
> **状态**: 草案
> **创建日期**: 2026-06-25

---

## 1. What

本文档定义 Bridge 配置系统的完整详细设计，包括 `BridgeSettings` 类的字段定义、类型约束、环境变量映射、派生属性实现和校验规则。同时包含 `.env` 文件格式和配置加载机制。

## 2. 配置类设计

### 2.1 类结构

```python
class BridgeSettings(BaseSettings):
    """Bridge 运行时配置。

    配置分组：
    - daemon_:    daemon 连接参数
    - bridge_:    Bridge 自身绑定参数
    - *_:         文件系统路径（agent_packages_dir, skill_hub_dir）
    - log_level:  日志级别
    - request_*:  超时参数
    - daemon_*:   缓存刷新参数
    - cors_*:     CORS 配置
    """

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        env_prefix="",
        case_sensitive=False,
        extra="ignore",
    )

    # --- daemon connectivity ---
    daemon_host: str = Field(default="localhost")
    daemon_port: int = Field(default=17891, ge=1, le=65535)

    # --- bridge server bind ---
    bridge_host: str = Field(default="0.0.0.0")
    bridge_port: int = Field(default=8199, ge=1, le=65535)

    # --- filesystem ---
    agent_packages_dir: str = Field(default="examples/agent")
    skill_hub_dir: str = Field(default="resource/skill_hub")

    # --- observability ---
    log_level: str = Field(default="INFO")
    request_timeout_seconds: float = Field(default=5.0, gt=0)
    daemon_status_refresh_interval: float = Field(default=10.0, gt=0)

    # --- CORS ---
    cors_allowed_origins: list[str] = Field(default_factory=lambda: [
        "http://localhost:15173",
        "http://127.0.0.1:15173",
    ])
```

### 2.2 字段完整定义

| # | 字段名 | 类型 | 约束 | 默认值 | 环境变量 | 分组 | 使用方 |
|---|--------|------|------|--------|----------|------|--------|
| 1 | `daemon_host` | str | 任意字符串 | `"localhost"` | `DAEMON_HOST` | 🔌 | `client.py` 连接地址 |
| 2 | `daemon_port` | int | 1-65535 | `17891` | `DAEMON_PORT` | 🔌 | `client.py` WS 连接 |
| 3 | `bridge_host` | str | 任意字符串 | `"0.0.0.0"` | `BRIDGE_HOST` | 🌐 | `main.py` uvicorn |
| 4 | `bridge_port` | int | 1-65535 | `8199` | `BRIDGE_PORT` | 🌐 | `main.py` uvicorn |
| 5 | `agent_packages_dir` | str | 任意路径字符串 | `"examples/agent"` | `AGENT_PACKAGES_DIR` | 📂 | `routes/packages.py`, `routes/agents.py` |
| 6 | `skill_hub_dir` | str | 任意路径字符串 | `"resource/skill_hub"` | `SKILL_HUB_DIR` | 📂 | `routes/skill_hub.py` |
| 7 | `log_level` | str | `"DEBUG"`/`"INFO"`/`"WARNING"`/`"ERROR"`/`"CRITICAL"` | `"INFO"` | `LOG_LEVEL` | 🪵 | `main.py` logging |
| 8 | `request_timeout_seconds` | float | > 0 | `5.0` | `REQUEST_TIMEOUT_SECONDS` | 🚦 | `client.py` 单次 RPC |
| 9 | `daemon_status_refresh_interval` | float | > 0 | `10.0` | `DAEMON_STATUS_REFRESH_INTERVAL` | 🚦 | `main.py` 后台轮询 |
| 10 | `cors_allowed_origins` | list[str] | JSON 数组 | `["localhost:15173", "127.0.0.1:15173"]` | `CORS_ALLOWED_ORIGINS` | 🌍 | `main.py` CORSMiddleware |

### 2.3 派生属性

```python
@property
def daemon_ws_url(self) -> str:
    """daemon WebSocket 连接地址。
    
    推导：f"ws://{self.daemon_host}:{self.daemon_port}"
    """
    return f"ws://{self.daemon_host}:{self.daemon_port}"

@property
def agent_packages_path(self) -> Path:
    """Agent Package 根目录的解析绝对路径。
    
    相对路径锚定到项目根目录（frontend/bridge/ 的父目录）。
    绝对路径直接使用并 resolve。
    """
    p = Path(self.agent_packages_dir)
    if p.is_absolute():
        return p.resolve()
    return (_PROJECT_ROOT / p).resolve()

@property
def skill_hub_path(self) -> Path:
    """技能中心仓库目录的解析绝对路径。
    
    解析规则同 agent_packages_path。
    """
    p = Path(self.skill_hub_dir)
    if p.is_absolute():
        return p.resolve()
    return (_PROJECT_ROOT / p).resolve()
```

### 2.4 校验规则

Pydantic 内置校验：

| 字段 | 校验器 | 违反行为 |
|------|--------|----------|
| `daemon_port` | `ge=1, le=65535` | Pydantic `ValidationError` |
| `bridge_port` | `ge=1, le=65535` | Pydantic `ValidationError` |
| `request_timeout_seconds` | `gt=0` | Pydantic `ValidationError` |
| `daemon_status_refresh_interval` | `gt=0` | Pydantic `ValidationError` |

`cors_allowed_origins` 从环境变量读取时，需解析 JSON 数组格式。Pydantic v2 的 `list[str]` 类型注解 + `json_schema` 自动处理 JSON 数组字符串 -> Python list 的转换。

### 2.5 单例与访问

```python
@lru_cache(maxsize=1)
def get_settings() -> BridgeSettings:
    """进程级缓存单例。"""
    return BridgeSettings()

# ``from config import settings``
settings = get_settings()
```

- 首次调用 `BridgeSettings()` 时自动读取 `.env` 文件和环境变量
- `lru_cache` 保证整个进程生命周期内只有一个实例
- 测试可构造 `BridgeSettings(_env_file="test.env")` 覆写

## 3. .env.example 模板

文件位置：`frontend/bridge/.env.example`

```bash
# ============================================================================
# AgentFlow Bridge — Configuration Template
# ============================================================================
# Copy this file to .env and modify for your environment:
#   cp .env.example .env
#
# Environment variables override .env values. The .env file is NOT committed.
# ============================================================================

# --- daemon connectivity ------------------------------------------------
DAEMON_HOST=localhost
DAEMON_PORT=17891

# --- bridge server bind -------------------------------------------------
BRIDGE_HOST=0.0.0.0
BRIDGE_PORT=8199

# --- filesystem paths ---------------------------------------------------
# Relative paths are resolved relative to the project root
# (the parent directory of frontend/bridge/).
AGENT_PACKAGES_DIR=examples/agent
SKILL_HUB_DIR=resource/skill_hub

# --- logging ------------------------------------------------------------
# Valid values: DEBUG, INFO, WARNING, ERROR, CRITICAL
LOG_LEVEL=INFO

# --- timeouts & intervals -----------------------------------------------
# Per-request timeout for daemon JSON-RPC calls (seconds)
REQUEST_TIMEOUT_SECONDS=5.0
# Background daemon status cache refresh interval (seconds)
DAEMON_STATUS_REFRESH_INTERVAL=10.0

# --- CORS ---------------------------------------------------------------
# JSON array of allowed origins. Default allows Vue dev server on :15173.
CORS_ALLOWED_ORIGINS=["http://localhost:15173","http://127.0.0.1:15173"]
```

## 4. 测试设计

### 4.1 测试矩阵

| 层级 | 测试点 | 验证内容 |
|------|--------|----------|
| Unit | 默认值 | 未设置任何环境变量时，所有字段使用默认值 |
| Unit | 环境变量覆盖 | `DAEMON_HOST=10.0.0.1` 后读取为 `10.0.0.1` |
| Unit | 端口校验 | `daemon_port=99999` 抛出 `ValidationError` |
| Unit | 派生属性 | `daemon_ws_url` = `"ws://localhost:17891"` |
| Unit | 路径解析（相对） | `agent_packages_dir="foo"` → `.../project_root/foo` |
| Unit | 路径解析（绝对） | `agent_packages_dir="/data/agents"` → `/data/agents` |
| Unit | `.env` 加载 | 设置 `.env` 文件后读取其中的值 |
| Unit | CORS 解析 | 环境变量 JSON 数组 → Python list |
| Unit | 单例 | 多次调用 `get_settings()` 返回同一对象 |
| Regression | 现有配置 | 所有现有 `settings.*` 使用点不受影响 |

---

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 字段定义与约束 | 本文档 §2.2 |
| 派生属性实现 | 本文档 §2.3 |
| 校验规则 | 本文档 §2.4 |
| .env 文件格式 | 本文档 §3 |
| 测试设计 | 本文档 §4 |
| 配置分组与功能设计 | `features/config-system.md` L2 |

**相关文档**:
- `docs/design/features/config-system.md` — 配置系统功能设计（L2）
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `frontend/bridge/config.py` — 源码实现（最终以此为准）
- `frontend/bridge/.env.example` — 配置文件模板
