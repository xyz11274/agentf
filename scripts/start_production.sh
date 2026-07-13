#!/bin/bash
# Start daemon + internal Bridge + external SideCar as one production stack.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

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
DAEMON_PORT="${DAEMON_PORT:-7890}"
BRIDGE_PORT="${PYTHON_BRIDGE_PORT:-8199}"
SIDECAR_PORT="${COREMIND_BRIDGE_PORT:-23767}"

# 生产环境使用 prod 配置
export AGENTFLOW_ENV="prod"

# daemon 二进制：优先 production 构建，fallback 到 release
DAEMON_BINARY="$AR_PROJECT_ROOT/build/production/agentflowd"
if [ ! -x "$DAEMON_BINARY" ]; then
    DAEMON_BINARY="$AR_PROJECT_ROOT/build/agentflowd"
fi

# sidecar 二进制：仅 production 构建存在时才要求
SIDECAR_BINARY="$AR_PROJECT_ROOT/frontend/bridge/coremind_bridge/build-production/coremind_bridge"
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
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM

"$SCRIPT_DIR/start_daemon.sh" --profile production --package "$PACKAGE" --port "$DAEMON_PORT" &
PIDS+=("$!")

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
    PYTHON_BRIDGE_URL="http://127.0.0.1:$BRIDGE_PORT/health" \
        "$SCRIPT_DIR/start_sidecar.sh" production "$SIDECAR_PORT" &
    PIDS+=("$!")
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
