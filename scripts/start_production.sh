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

PACKAGE="${AGENT_PACKAGE:-examples/agent/outfit-assistant}"
# daemon 端口统一 17891（与 start_daemon.sh / start_e2e.sh / configs/{dev,test}/agentflowd.json / config.py 默认值一致）
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
    for pid in "${PIDS[@]+"${PIDS[@]}"}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${PIDS[@]+"${PIDS[@]}"}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM

"$SCRIPT_DIR/start_daemon.sh" --profile production --package "$PACKAGE" --port "$DAEMON_PORT" &
PIDS+=("$!")

# 等待 daemon 就绪（TCP 端口可达）后再启动 Bridge
# 注意：daemon 只暴露 JSON-RPC 接口，没有 REST /health 端点。
# 这里用 curl -s（非 -sf）只检查 TCP 连接是否建立，不检查 HTTP 状态码。
# 即使返回 404 也认为 daemon 已就绪——因为 daemon 的 HTTP 服务器会响应 404 页，
# 但连接失败（connection refused）才说明 daemon 未就绪。
DAEMON_READY=false
for _ in $(seq 1 15); do
    if curl -s --connect-timeout 1 "http://127.0.0.1:$DAEMON_PORT/" >/dev/null 2>&1; then
        DAEMON_READY=true
        break
    fi
    if ! kill -0 "${PIDS[0]}" 2>/dev/null; then
        echo "[ERROR] Daemon exited during startup" >&2
        exit 1
    fi
    sleep 1
done
if [ "$DAEMON_READY" != "true" ]; then
    echo "[ERROR] Daemon readiness timed out after 15s" >&2
    exit 1
fi

DAEMON_HOST=127.0.0.1 DAEMON_PORT="$DAEMON_PORT" \
    "$SCRIPT_DIR/start_bridge.sh" production "$BRIDGE_PORT" &
PIDS+=("$!")

BRIDGE_READY=false
for _ in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:$BRIDGE_PORT/health" >/dev/null; then
        BRIDGE_READY=true
        break
    fi
    if ! kill -0 "${PIDS[1]}" 2>/dev/null; then
        echo "[ERROR] Python Bridge exited during startup" >&2
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
        "$SCRIPT_DIR/start_sidecar.sh" production "$SIDECAR_PORT" &
    PIDS+=("$!")
    SIDECAR_READY=false
    for _ in $(seq 1 15); do
        if curl -sf "http://127.0.0.1:$SIDECAR_PORT/health" >/dev/null; then
            SIDECAR_READY=true
            break
        fi
        if ! kill -0 "${PIDS[2]}" 2>/dev/null; then
            echo "[ERROR] C++ SideCar exited during startup" >&2
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
            wait "${PIDS[$index]}" || exit_code=$?
            echo "[ERROR] Production component $index exited (${exit_code:-0})" >&2
            exit "${exit_code:-1}"
        fi
    done
    sleep 1
done
