# AgentFlow 配置体系

本目录存放所有组件的环境差异化配置文件，实现 **dev / test / prod 配置隔离**。

---

## 目录结构

```
configs/
├── dev/                          # 开发环境配置
│   ├── agentflowd.json           # daemon 配置
│   └── bridge.yaml               # Bridge 配置
├── test/                         # 测试 / CI 环境配置
│   ├── agentflowd.json           # daemon 配置
│   └── bridge.yaml               # Bridge 配置
├── prod/                         # 生产环境配置
│   ├── agentflowd.json           # daemon 配置
│   └── bridge.yaml               # Bridge 配置
└── README.md                     # 本文档
```

新增环境（如 staging）：`mkdir configs/staging`，放入对应文件即可。

---

## 组件配置方式

### agentflowd daemon

两种方式等效：

```bash
# 方式 A：通过脚本（推荐）
# 自动按 AGENTFLOW_ENV 选取 configs/{env}/agentflowd.json
./scripts/start_daemon.sh                        # AGENTFLOW_ENV=dev  → configs/dev/agentflowd.json
AGENTFLOW_ENV=prod ./scripts/start_daemon.sh     # 显式指定生产
./scripts/start_daemon.sh --config configs/staging/agentflowd.json  # 自定义

# 方式 B：直接启动
./build/agentflowd --config configs/dev/agentflowd.json
```

配置优先级：**环境变量 > 配置文件 > CLI 参数 > 代码默认值**

### Bridge (Python FastAPI)

**配置文件搜索顺序**（先匹配者生效）：

```python
_CONFIG_FILE_CANDIDATES = [
    configs/{AGENTFLOW_ENV}/bridge.yaml,    # ← 新增：环境特定配置
    config.yaml,                             # 当前目录
    frontend/bridge/config.yaml,             # 相对路径
    .../frontend/bridge/config.yaml,         # 项目根绝对路径
    /opt/agentflow/config.yaml,              # 系统路径
]
```

`AGENTFLOW_ENV` 控制加载的环境：

| `AGENTFLOW_ENV` | 加载的配置 |
|----------------|-----------|
| 未设置（默认） | `configs/dev/bridge.yaml` |
| `dev` | `configs/dev/bridge.yaml` |
| `test` | `configs/test/bridge.yaml` |
| `prod` | `configs/prod/bridge.yaml` |

实际使用：

```bash
# 方式 A：通过脚本（推荐）
./scripts/start_bridge.sh dev          # AGENTFLOW_ENV=dev  → configs/dev/bridge.yaml
./scripts/start_bridge.sh test         # AGENTFLOW_ENV=test → configs/test/bridge.yaml
./scripts/start_bridge.sh production   # AGENTFLOW_ENV=prod → configs/prod/bridge.yaml

# 方式 B：手动启动
AGENTFLOW_ENV=prod python -m uvicorn main:app --app-dir frontend/bridge

# 方式 C：全栈启动（daemon + bridge + sidecar）
./scripts/start_production.sh          # 内部已 export AGENTFLOW_ENV=prod
```

Bridge 配置优先级：**系统环境变量 > configs/{env}/bridge.yaml > 代码默认值**

### Sidecar (coremind_bridge)

纯 CLI 参数 + 环境变量，无需配置文件：

```bash
./scripts/start_sidecar.sh production 23767
```

---

## Bridge 配置文件的精确关系

Bridge 的配置加载机制是 **first-match-wins**（`_CONFIG_FILE_CANDIDATES` 顺序扫描，命中第一个即返回，不会合并多个文件）。

```python
_CONFIG_FILE_CANDIDATES = [
    configs/{AGENTFLOW_ENV}/bridge.yaml,    # ① 环境配置（最新增）
    config.yaml,                             # ② CWD 回退
    frontend/bridge/config.yaml,             # ③ 相对路径回退
    .../frontend/bridge/config.yaml,         # ④ 绝对路径回退
    /opt/agentflow/config.yaml,              # ⑤ 系统路径回退
]
```

### 实际效果

```
AGENTFLOW_ENV=dev                           AGENTFLOW_ENV=prod
       │                                           │
       ▼                                           ▼
┌──────────────────────┐                 ┌──────────────────────┐
│ configs/dev/         │  ← 命中，返回    │ configs/prod/        │  ← 命中，返回
│ bridge.yaml          │                 │ bridge.yaml          │
└──────────────────────┘                 └──────────────────────┘
       │                                           │
       ✗ frontend/bridge/config.yaml               ✗ frontend/bridge/config.yaml
         永远不会被读取                                 永远不会被读取
```

在 `configs/dev/bridge.yaml` 存在时，`frontend/bridge/config.yaml` 永远不会被加载。

### 这意味着什么

| 对于 | 结论 |
|------|------|
| **日常修改配置** | 只改 `configs/dev/bridge.yaml`、`configs/test/bridge.yaml` 或 `configs/prod/bridge.yaml` |
| **新增字段** | 必须同时写入 4 个文件：`configs/dev/bridge.yaml` + `configs/test/bridge.yaml` + `configs/prod/bridge.yaml` + `frontend/bridge/config.yaml`（作为其他环境回退） |
| **删除旧文件** | 可以删，但建议保留 `frontend/bridge/config.yaml` 作为参考模板和 CI 回退 |

### 配置内容必须一致

因为不存在合并，`configs/dev/bridge.yaml` 必须包含 `frontend/bridge/config.yaml` **所有字段的副本**，否则缺的字段会回退到 Pydantic 代码默认值（可能为空字符串或错误值）。

当前两个文件都已同步为完整副本，`agent_defaults`（LLM 密钥、模型名、vision 配置等）已包含在内。

### daemon 侧同理

| 配置 | 路径 | 加载方式 |
|------|------|---------|
| **环境配置（权威）** | `configs/dev/agentflowd.json` / `configs/test/agentflowd.json` / `configs/prod/agentflowd.json` | `scripts/start_daemon.sh` 自动传 `--config` |

---

## 组件与配置对照

| 组件 | 配置方式 | 开发配置 | 生产配置 | 脚本启动 |
|------|---------|---------|---------|---------|
| agentflowd daemon | JSON + 环境变量 | `configs/dev/agentflowd.json` | `configs/prod/agentflowd.json` | `scripts/start_daemon.sh`（自动补 `--config`） |
| Bridge (Python) | YAML + 环境变量 | `configs/dev/bridge.yaml` | `configs/prod/bridge.yaml` | `scripts/start_bridge.sh`（导出 `AGENTFLOW_ENV`） |
| Sidecar (C++ bridge) | CLI 参数 | 无 | 无 | `scripts/start_sidecar.sh` |

---

## 相对路径的基准目录

配置文件中的相对路径，解析基准因组件而异：

### agentflowd daemon（JSON 配置）

**所有相对路径基准 = CWD（启动 daemon 时的工作目录）**

| 字段 | 示例 | 解析方式 |
|------|------|---------|
| `agents[].package_path` | `"./examples/agent/outfit-assistant"` | 直接传 `filesystem::exists()` | 
| `logging.file` | `"logs/agentflowd.log"` | Logger 直接使用 |

因为 `start_daemon.sh` 在项目根执行，所以相对路径从项目根开始解析。

### Bridge（YAML 配置）

**大部分路径基准 = 项目根（`frontend/bridge/../../`），但有一个例外**

| 字段 | 示例 | 基准目录 | 代码 |
|------|------|---------|------|
| `agent_packages_dir` | `"examples/agent"` | **项目根** | `_PROJECT_ROOT / p` |
| `skill_hub_dir` | `"resource/skill_hub"` | **项目根** | `_PROJECT_ROOT / p` |
| `agent_template_dir` | `"frontend/bridge/templates/agent_template"` | **项目根** | `_PROJECT_ROOT / p` |
| `session_db_path` | `"data/sessions.db"` | **项目根** | `_PROJECT_ROOT / p` |
| `uploads_dir` | `"/tmp/agentflow/uploads"` | **绝对路径（无歧义）** | 固定值，不受影响 |
| `certs/*`（mTLS） | `"/etc/agentflow/certs/"` | **绝对路径（无歧义）** | 固定值，不受影响 |

> ⚠️ 注意：`uploads_dir` 如果填相对路径，会解析到 **CWD** 而非项目根（`p.resolve()` 直接返回，无 `_PROJECT_ROOT` 前缀）。当前已使用绝对路径，无歧义。

---

## 关键差异（dev vs prod）

| 配置项 | dev | test | prod | 原因 |
|--------|-----|------|------|------|
| daemon listen | `127.0.0.1:17891` | `127.0.0.1:17891` | `0.0.0.0:7890` | 开发/测试本地绑定，生产全接口 |
| daemon worker_threads | 4 | 2 | 8 | 测试最小资源，生产高并发 |
| dataref_bridge.capacity_mb | 512 | 128 | 1024 | 测试最小资源 |
| dataref_bridge.redis_url | 空（禁用） | 空（禁用） | `redis://127.0.0.1:6379` | 开发/测试不依赖 Redis |
| bridge log_level | DEBUG | DEBUG | INFO | 开发/测试需详细日志 |
| bridge sidecar.enabled | false | false | true | 仅生产启用外部入口 |
| bridge upstream.scheme | http | http | https | 生产上游回调必须加密 |
| bridge CORS origins | localhost:15173 | `*` | 生产域名 | 测试全开放，生产限制 |

---

## 快速参考

```bash
# ── 开发 ──
./scripts/start_daemon.sh                  # daemon, 自动 configs/dev/agentflowd.json
./scripts/start_bridge.sh dev              # bridge, 自动 configs/dev/bridge.yaml

# ── 测试 ──
AGENTFLOW_ENV=test ./scripts/start_daemon.sh                # test daemon config
./scripts/start_bridge.sh test              # test bridge config

# ── 生产 ──
./scripts/start_production.sh              # daemon + bridge + sidecar, 自动 prod 配置

# ── 覆盖个别字段（不修改配置文件）──
export AF_DATAREF_CAPACITY_MB=2048         # daemon 环境变量覆盖
export AGENTFLOW_ENV=prod                  # bridge 环境选择
export DAEMON_PORT=7890                    # bridge 连接不同 daemon 端口
```
