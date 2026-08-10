#!/bin/bash
# Start daemon + internal Bridge + external SideCar as one production stack.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# 生产环境使用 prod 配置（在 ar_load_env 之前设置，确保加载 configs/prod/.env）
export AGENTFLOW_ENV="prod"
# 日志目录可通过环境变量覆盖（Pod 上若 logs/ 不可写，可设为 /tmp/）
export AF_LOG_FILE="${AF_LOG_FILE:-$AR_PROJECT_ROOT/logs/agentflowd.log}"
export AF_BRIDGE_LOG_DIR="${AF_BRIDGE_LOG_DIR:-$AR_PROJECT_ROOT/logs}"
export AF_SIDECAR_LOG_DIR="${AF_SIDECAR_LOG_DIR:-$AR_PROJECT_ROOT/logs}"

# 加载环境配置
ar_load_env

CHECK_ONLY=false
if [ "${1:-}" = "--check" ]; then CHECK_ONLY=true
elif [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    echo "Usage: $0 [--check]"
    exit 0
elif [ "$#" -gt 0 ]; then
    echo "[ERROR] Unknown argument: $1" >&2
    exit 2
fi

# 默认加载 ③ workspace 运行实例（examples/agent/{name}/，三态模型见
# docs/design/features/agent-package/agent-package-three-state.md §2.1）。
# 启动脚本绕过 bridge 的 ②→③ 同步直接加载 ③ 实例（by-design）；改 ② 固化态后需手动 load 或 rsync 同步。
PACKAGE="${AGENT_PACKAGE:-examples/agent/outfit-assistant}"
# daemon 端口统一 17891（与 start_daemon.sh / start_dev.sh / configs/{dev,test}/agentflowd.json / config.py 默认值一致）
DAEMON_PORT="${DAEMON_PORT:-17891}"
BRIDGE_PORT="${PYTHON_BRIDGE_PORT:-8199}"
SIDECAR_PORT="${COREMIND_BRIDGE_PORT:-23767}"

# daemon 二进制：优先 production 构建，fallback 到 release
DAEMON_BINARY="$AR_PROJECT_ROOT/build/production/agentflowd"
if [ ! -x "$DAEMON_BINARY" ]; then
    DAEMON_BINARY="$AR_PROJECT_ROOT/build/agentflowd"
fi

# sidecar 二进制：仅 production 构建存在时才要求
# 生产栈故意只认 build-production/，不 fallback 到 build-dev/build-release stub binary——
# stub 永远返回空服务发现，会立刻触发 REQUIRED_SERVICE_DISCOVERY_FAILED，不能服务生产请求。
# 本地开发用 ./scripts/build.sh dev 构建的 stub sidecar 应通过 scripts/start_sidecar.sh local 单独启动，
# 不要塞进生产栈。详见 docs/design/architecture/modules/coremind-bridge.md §6.1
SIDECAR_BINARY="$AR_PROJECT_ROOT/sidecar/build-production/coremind_bridge"
SIDECAR_AVAILABLE=false
if [ -x "$SIDECAR_BINARY" ]; then
    SIDECAR_AVAILABLE=true
fi

if [[ "$PACKAGE" = /* ]]; then PACKAGE_PATH="$PACKAGE"
else PACKAGE_PATH="$AR_PROJECT_ROOT/$PACKAGE"
fi
# 三态模型：启动前自动从 ② 同步重建 ③（删 examples 不崩不变量）
# 缺 ③ 或 ② 比 ③ 新时 rsync ②→③，排除运行时数据保留 ③ 已有现场。
_sync_2_to_3() {
    local pkg_name="${PACKAGE_PATH##*/}"
    local source_2="$AR_PROJECT_ROOT/resource/agent-packages/$pkg_name"
    if [ ! -d "$source_2" ]; then
        echo "[WARN] ② 固化态不存在: $source_2（跳过同步）"; return 0
    fi
    if [ ! -d "$PACKAGE_PATH" ] || [ "$source_2" -nt "$PACKAGE_PATH" ]; then
        echo "[INFO] 同步 ②→③: $source_2 → $PACKAGE_PATH"
        mkdir -p "$PACKAGE_PATH"
        rsync -a --delete \
            --exclude='.venv/' --exclude='outputs/' --exclude='workspace/' \
            --exclude='.agentfoundry/' --exclude='preferences.json' \
            --exclude='manifest.jsonc' --exclude='dist/' \
            "$source_2/" "$PACKAGE_PATH/"
        echo "[INFO] 同步完成（运行时数据保留）"
    fi
}
_sync_2_to_3
if [ ! -f "$PACKAGE_PATH/agent.json" ]; then
    echo "[ERROR] Agent package not found: $PACKAGE_PATH" >&2
    exit 1
fi
if [ ! -x "$DAEMON_BINARY" ]; then
    echo "[ERROR] Daemon binary not found: $DAEMON_BINARY" >&2
    echo "        Run: ./scripts/build.sh" >&2
    exit 1
fi
if ! PYTHONPATH="$AR_PROJECT_ROOT/frontend/bridge" python3 -c "import fastapi, uvicorn" 2>/dev/null; then
    echo "[ERROR] Python Bridge production dependencies are missing" >&2
    exit 1
fi
if $CHECK_ONLY; then
    echo "[OK] Production stack prerequisites are complete"
    exit 0
fi

PIDS=()
cleanup() {
    local pid
    # 兼容 bash < 4.4：set -u 下空数组展开会报 unbound variable，
    # ${arr[@]+"${arr[@]}"} 语法在空数组时安全展开为空。
    # 第 1 轮：SIGTERM 让服务有机会优雅退出（如 uvicorn lifespan shutdown、daemon drain phase）。
    for pid in "${PIDS[@]+"${PIDS[@]}"}"; do kill "$pid" 2>/dev/null || true; done
    # 等待最多 3s 让服务 graceful shutdown。
    for _ in $(seq 1 10); do
        local all_dead=true
        for pid in "${PIDS[@]+"${PIDS[@]}"}"; do
            if kill -0 "$pid" 2>/dev/null; then all_dead=false; break; fi
        done
        $all_dead && break
        sleep 0.3
    done
    # 第 2 轮：SIGKILL 兜底——部分服务（如 uvicorn startup 阶段）不响应 SIGTERM，必须 SIGKILL。
    # 对齐 start_bridge.sh 的 _stop_bridge / start_sidecar.sh 的 _stop_sidecar / start_daemon.sh 的 _stop_daemon 的两段式逻辑。
    for pid in "${PIDS[@]+"${PIDS[@]}"}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    # 不 wait：PIDS 里是 nohup 起的服务进程（被 PID 1 收养），不是当前 shell 的子进程，wait 会失败。
}
trap cleanup EXIT INT TERM

"$SCRIPT_DIR/start_daemon.sh" --profile production --package "$PACKAGE" --port "$DAEMON_PORT" &
# start_daemon.sh 是 fire-and-forget 设计：启动 daemon 到 nohup 后台、写 PID 文件、然后自己退出。
# 所以 $! 是 wrapper 脚本的 PID，不是 daemon 进程的 PID——不能用 $! 做长期存活检测。
# 必须等 wrapper 退出后，从它写入的 /tmp/agentflowd.pid 拿真实 daemon PID。
WRAPPER_PID=$!
wait "$WRAPPER_PID" || {
    echo "[ERROR] Daemon wrapper exited with code $? (binary missing / port conflict / config error)" >&2
    exit 1
}
DAEMON_PID_FILE="/tmp/agentflowd.pid"
if [ ! -f "$DAEMON_PID_FILE" ]; then
    echo "[ERROR] Daemon wrapper exited cleanly but no PID file at $DAEMON_PID_FILE" >&2
    exit 1
fi
DAEMON_PID=$(cat "$DAEMON_PID_FILE")
PIDS+=("$DAEMON_PID")

# 等待 daemon 就绪（TCP 端口可达）后再启动 Bridge。
# daemon 是 JSON-RPC over raw TCP（日志为 [RPC_SERVER]，不是 HTTP_SERVER），
# 不能用 curl 检测——curl 发 HTTP GET 会被 RPC_SERVER 视为非法 frame 而 reset，
# curl 永远返回非 0，readiness 永远 false。
# 用 bash 内置 /dev/tcp 做纯 TCP 连接检测，无外部依赖、对协议透明。
DAEMON_READY=false
for _ in $(seq 1 15); do
    if (exec 3<>/dev/tcp/127.0.0.1/$DAEMON_PORT) 2>/dev/null; then
        DAEMON_READY=true
        break
    fi
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "[ERROR] Daemon (PID=$DAEMON_PID) exited during startup" >&2
        exit 1
    fi
    sleep 1
done
if [ "$DAEMON_READY" != "true" ]; then
    echo "[ERROR] Daemon readiness timed out after 15s" >&2
    exit 1
fi

DAEMON_HOST=127.0.0.1 DAEMON_PORT="$DAEMON_PORT" \
    "$SCRIPT_DIR/start_bridge.sh" production --port "$BRIDGE_PORT" &
# 同 daemon 块：start_bridge.sh 也是 fire-and-forget，$! 是 wrapper PID 不是 bridge 进程 PID。
WRAPPER_PID=$!
wait "$WRAPPER_PID" || {
    echo "[ERROR] Bridge wrapper exited with code $? (deps missing / port conflict / config error)" >&2
    exit 1
}
BRIDGE_PID_FILE="/tmp/agentflow-bridge.pid"
if [ ! -f "$BRIDGE_PID_FILE" ]; then
    echo "[ERROR] Bridge wrapper exited cleanly but no PID file at $BRIDGE_PID_FILE" >&2
    exit 1
fi
BRIDGE_PID=$(cat "$BRIDGE_PID_FILE")
PIDS+=("$BRIDGE_PID")

BRIDGE_READY=false
for _ in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:$BRIDGE_PORT/health" >/dev/null; then
        BRIDGE_READY=true
        break
    fi
    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        echo "[ERROR] Python Bridge (PID=$BRIDGE_PID) exited during startup" >&2
        exit 1
    fi
    sleep 1
done
if [ "$BRIDGE_READY" != "true" ]; then
    echo "[ERROR] Python Bridge readiness timed out" >&2
    exit 1
fi

if $SIDECAR_AVAILABLE; then
    # SideCar 继承 caller 的 APPID / COREMIND_REQUIRED_SERVICES / COREMIND_OPTIONAL_SERVICES
    # （start_sidecar.sh 内会设默认值，caller 可通过环境变量覆盖）
    PYTHON_BRIDGE_URL="http://127.0.0.1:$BRIDGE_PORT/health" \
        "$SCRIPT_DIR/start_sidecar.sh" production --port "$SIDECAR_PORT" &
    # 同 daemon/bridge 块：start_sidecar.sh 也是 fire-and-forget，$! 是 wrapper PID。
    WRAPPER_PID=$!
    wait "$WRAPPER_PID" || {
        echo "[ERROR] SideCar wrapper exited with code $? (binary missing / CoreMind init failed / config error)" >&2
        exit 1
    }
    SIDECAR_PID_FILE="/tmp/agentflow-sidecar.pid"
    if [ ! -f "$SIDECAR_PID_FILE" ]; then
        echo "[ERROR] SideCar wrapper exited cleanly but no PID file at $SIDECAR_PID_FILE" >&2
        exit 1
    fi
    SIDECAR_PID=$(cat "$SIDECAR_PID_FILE")
    PIDS+=("$SIDECAR_PID")
    SIDECAR_READY=false
    for _ in $(seq 1 15); do
        if curl -sf "http://127.0.0.1:$SIDECAR_PORT/health" >/dev/null; then
            SIDECAR_READY=true
            break
        fi
        if ! kill -0 "$SIDECAR_PID" 2>/dev/null; then
            echo "[ERROR] C++ SideCar (PID=$SIDECAR_PID) exited during startup" >&2
            exit 1
        fi
        sleep 1
    done
    if [ "$SIDECAR_READY" != "true" ]; then
        echo "[ERROR] C++ SideCar readiness timed out" >&2
        exit 1
    fi
    echo "[READY] daemon=$DAEMON_PORT bridge=127.0.0.1:$BRIDGE_PORT sidecar=0.0.0.0:$SIDECAR_PORT"
else
    echo "[READY] daemon=$DAEMON_PORT bridge=127.0.0.1:$BRIDGE_PORT (sidecar skipped)"
fi

while true; do
    for index in "${!PIDS[@]}"; do
        if ! kill -0 "${PIDS[$index]}" 2>/dev/null; then
            # PIDS 里是真实服务进程 PID（由 wrapper 写入 PID 文件），不是当前 shell 的子进程，
            # 所以无法用 wait 拿退出码。kill -0 失败即可断定服务已退出。
            echo "[ERROR] Production component $index (PID=${PIDS[$index]}) exited" >&2
            exit 1
        fi
    done
    sleep 1
done
