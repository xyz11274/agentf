# AgentFlow 构建与启动脚本

`scripts/` 是项目构建和服务启动的权威入口。当前生产拓扑：

```text
外部请求 → C++ SideCar :23767 → Python Bridge :8199
                                      ↓
                               agentflowd :17891 → Node
```

Python Bridge 生产模式只监听 loopback；SideCar 是唯一外部入口。

## 脚本目录

| 脚本 | 用途 |
|------|------|
| `build.sh` | dev/release/production 分 profile 构建 |
| `start_daemon.sh` | 单独启动 agentflowd，不隐式编译 |
| `start_bridge.sh` | 启动开发或内部生产 Bridge |
| `start_sidecar.sh` | 启动本地或生产 SideCar |
| `start_production.sh` | 统一启动生产三进程栈 |
| `start_e2e.sh` | 本地端到端调试：启动完整链路（FusionMind Mock + Mock UDS + daemon + Bridge + 前端） |
| `start_mock_uds_server.sh` | 单独启动 Mock UDS Server（替代 UDSPluginServer，用于本地 LLM 调用调试） |
| `start_web.sh` | 启动 Vue/Vite 开发服务器 |
| `run_agent.sh` | 旧 harness_agent 交互演示，仅保留开发用途 |
| `regression_test.sh` | C++ 单元与 E2E 回归 |

## 配置文件

| 文件 | 用途 |
|------|------|
| `.env` / `.env.example` | 部署配置（密钥、服务地址） |
| `config.json` | daemon 本地开发配置文件，被 `start_daemon.sh` 和 `start_e2e.sh` 引用 |

## 构建

```bash
# Release 全量构建，输出 build/
./scripts/build.sh

# Debug 构建，输出 build/dev/
./scripts/build.sh dev

# 单目标增量构建（兼容旧用法）
./scripts/build.sh agentflowd
./scripts/build.sh dev agentflowd

# 强制重新配置并清理旧产物
./scripts/build.sh release --clean
```

每次执行都会重新运行 CMake configure，避免已有 `build/` 时忽略新的构建选项。

### 生产构建

生产构建包含 Release `agentflowd` 和启用真实 CoreMind 的 C++ SideCar：

```bash
PLATFORM_DIR=/opt/coremind/platform \
  ./scripts/build.sh production --clean
```

产物：

```text
build/production/agentflowd
sidecar/build-production/coremind_bridge
```

生产构建不允许使用 `COREMIND_ENABLED=OFF` 的本地 stub。

## 配置

脚本启动参数（端口、package 路径）已在各脚本的 `${VAR:-default}` 默认值中，可通过 CLI 参数或环境变量覆盖，无需配置文件。

Bridge / Agent 节点的运行时配置统一在 `configs/{env}/bridge.yaml`（按 `AGENTFLOW_ENV` 选环境），详见 `configs/README.md`。

## 启动

### 端到端调试（推荐）

一键启动完整本地调试链路：FusionMind Mock + Mock UDS Server + agentflowd + Bridge + 前端。

```bash
# 默认 Mock 模式（回显）
./scripts/start_e2e.sh

# Proxy 模式（透传到真实 LLM）
./scripts/start_e2e.sh --proxy

# 仅启动 Mock UDS + daemon + Bridge（无前端）
./scripts/start_e2e.sh --no-frontend

# 仅启动 FusionMind Mock 和 Mock UDS Server
./scripts/start_e2e.sh --fmm-only
```

启动链路：

```text
Frontend (Vite:15173)
  → Bridge (uvicorn:8199)
    → AgentFlow Daemon (agentflowd:17891)
      → Python Node
        → LlmUdsClient (.sock)
          → Mock UDS Server (Python UDS)     ← 模拟 UDSPluginServer
            → [mock] FusionMind Mock (:8085)  ← 回显/固定响应/工具调用
            → [proxy] 真实 LLM API            ← DashScope 等上游
```

停止：`Ctrl+C` 自动逆序清理所有子进程。

### 单独启动组件

```bash
./scripts/start_daemon.sh --profile release --package examples/agent/outfit-assistant
./scripts/start_bridge.sh dev 8199
./scripts/start_web.sh 15173
```

### Mock UDS Server（单独调试 LLM 调用）

```bash
# 默认回显模式
./scripts/start_mock_uds_server.sh

# 固定文本模式
./scripts/start_mock_uds_server.sh --mode fixed --response "你好世界"

# 指定 socket 路径
./scripts/start_mock_uds_server.sh --socket /tmp/agentflow_plugin.sock
```

Mock UDS Server 支持 `llm.chat`（OpenAI 格式透传）、`llm.task.create/stream/destroy`、`llm.complete`、`tool.list/call` 等全部 UDS 方法，可配合 `LlmUdsClient` 做本地验证。

Bridge 开发模式使用 Uvicorn reload，但监听范围固定为 `frontend/bridge/`。
Agent package/skill 工作空间位于 `examples/agent/`，其中产生或更新源码及中间文件不会重启
Bridge，也不会因此断开浏览器 WebSocket。

启动脚本默认不编译。需要启动前构建时显式添加 `--build`：

```bash
./scripts/start_daemon.sh --build --profile release
```

### 生产栈

完成 production 构建和 `.env` 配置后：

```bash
./scripts/start_production.sh --check
./scripts/start_production.sh
```

该脚本统一管理 daemon、Bridge、SideCar 生命周期；退出时清理三个子进程。

也可单独启动：

```bash
./scripts/start_daemon.sh --profile production --package examples/agent/outfit-assistant
./scripts/start_bridge.sh production 8199
./scripts/start_sidecar.sh production 23767
```

`start_bridge.sh production` 不传递 `--reload`，生产 Bridge 不启动文件监控进程。

## 旧入口

- 根目录 `build.sh`：已废弃，不再维护。原因是该脚本用字符串拼接 cmake 参数，当 generator 名为 `"Unix Makefiles"`（无 Ninja 时）会产生 shell 参数分裂 bug。所有功能已被 `scripts/build.sh` 替代。新人直接忽略即可。
- `run_agent.sh` 仍指向 `examples/agent/harness_agent`，属于示例工具，不参与生产启动。
- `sidecar/build.sh` 是 SideCar 模块级构建入口；统一生产构建应使用 `scripts/build.sh production`。

---

## 脚本关系全景

```text
项目根
├── build.sh               ← 已废弃，勿用（参数分裂 bug）
├── scripts/
│   ├── build.sh            ← 权威构建入口（dev / release / production）
│   ├── start_daemon.sh      ── 启动 agentflowd 守护进程
│   ├── start_bridge.sh      ── 启动 Python Bridge（dev / production）
│   ├── start_sidecar.sh     ── 启动 C++ SideCar
│   ├── start_production.sh  ── 统一启动 daemon + Bridge + SideCar
│   ├── start_e2e.sh         ── 本地端到端调试（FMM Mock + Mock UDS + daemon + Bridge + 前端）
│   ├── start_mock_uds_server.sh ── 单独启动 Mock UDS Server（替代 UDSPluginServer）
│   ├── start_web.sh         ── 启动 Vue/Vite 开发服务器
│   ├── run_agent.sh         ── harness_agent 示例演示（开发用）
│   ├── common.sh            ── 被以上脚本 source 的公共函数
│   ├── config.json          ── daemon 本地开发默认配置
│   ├── .env / .env.example  ── 部署配置（密钥、服务地址）
│   └── tests/
│       ├── regression_test.sh          ← 回归测试入口（C++ unit + E2E + daemon E2E）
│       └── test_skills_upload_e2e.sh   ← Skill 上传 E2E 测试
└── (其他目录)
```

### 构建 vs 启动 vs 测试

| 操作 | 入口 |
|------|------|
| **编译项目** | `scripts/build.sh [dev\|release\|production]` |
| **启动开发栈** | `start_daemon.sh → start_bridge.sh dev → start_web.sh` |
| **启动生产栈** | `start_production.sh`（统一管理三进程生命周期） |
| **运行回归测试** | `scripts/tests/regression_test.sh` |
| **运行指定测试** | `build/tests/test_databus_concurrent`（直接运行二进制） |

### 为什么有两个 `build.sh`？

历史上根目录 `build.sh` 是先写的简易版本，`scripts/build.sh` 是后续重构的项目级构建入口。由于文档和 CI 历史原因两者并存，但根目录版本存在已知 shell 参数分裂 bug，**所有日常开发请使用 `scripts/build.sh`**。
