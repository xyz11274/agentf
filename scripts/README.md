# AgentFlow 构建与启动脚本

`ar_scripts/` 是项目构建和服务启动的权威入口。当前生产拓扑：

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
| `start_web.sh` | 启动 Vue/Vite 开发服务器 |
| `run_agent.sh` | 旧 harness_agent 交互演示，仅保留开发用途 |
| `regression_test.sh` | C++ 单元与 E2E 回归 |

## 构建

```bash
# Release 全量构建，输出 build/
./ar_scripts/build.sh

# Debug 构建，输出 build/dev/
./ar_scripts/build.sh dev

# 单目标增量构建（兼容旧用法）
./ar_scripts/build.sh agentflowd
./ar_scripts/build.sh dev agentflowd

# 强制重新配置并清理旧产物
./ar_scripts/build.sh release --clean
```

每次执行都会重新运行 CMake configure，避免已有 `build/` 时忽略新的构建选项。

### 生产构建

生产构建包含 Release `agentflowd` 和启用真实 CoreMind 的 C++ SideCar：

```bash
PLATFORM_DIR=/opt/coremind/platform \
  ./ar_scripts/build.sh production --clean
```

产物：

```text
build/production/agentflowd
frontend/bridge/coremind_bridge/build-production/coremind_bridge
```

生产构建不允许使用 `COREMIND_ENABLED=OFF` 的本地 stub。

## 配置

```bash
cp ar_scripts/.env.example ar_scripts/.env
```

至少设置：

```bash
AISF_INTERNAL_TOKEN="replace-with-deployment-secret"
COREMIND_REQUIRED_SERVICES="IMInferScheduler"
```

`.env` 仅供本地或受控部署使用，不应提交密钥。

## 启动

### 开发组件

```bash
./ar_scripts/start_daemon.sh --profile release --package examples/agent/outfit_assistant
./ar_scripts/start_bridge.sh dev 8199
./ar_scripts/start_web.sh 15173
```

Bridge 开发模式使用 Uvicorn reload，但监听范围固定为 `frontend/bridge/`。
Agent package/skill 工作空间位于 `examples/agent/`，其中产生或更新源码及中间文件不会重启
Bridge，也不会因此断开浏览器 WebSocket。

启动脚本默认不编译。需要启动前构建时显式添加 `--build`：

```bash
./ar_scripts/start_daemon.sh --build --profile release
```

### 生产栈

完成 production 构建和 `.env` 配置后：

```bash
./ar_scripts/start_production.sh --check
./ar_scripts/start_production.sh
```

该脚本统一管理 daemon、Bridge、SideCar 生命周期；退出时清理三个子进程。

也可单独启动：

```bash
./ar_scripts/start_daemon.sh --profile production --package examples/agent/outfit_assistant
./ar_scripts/start_bridge.sh production 8199
./ar_scripts/start_sidecar.sh production 23767
```

`start_bridge.sh production` 不传递 `--reload`，生产 Bridge 不启动文件监控进程。

## 旧入口

- 根目录 `build.sh` 仍作为通用开发/CTest 工具保留，不作为生产构建入口。
- `run_agent.sh` 仍指向 `examples/agent/harness_agent`，属于示例工具，不参与生产启动。
- `frontend/bridge/coremind_bridge/build.sh` 是 SideCar 模块级构建入口；统一生产构建应使用 `ar_scripts/build.sh production`。
