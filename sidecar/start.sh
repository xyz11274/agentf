#!/bin/bash
# Start the internal Python Bridge and the externally exposed C++ SideCar.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(realpath "$SCRIPT_DIR/..")"
SIDECAR_BIN="${SCRIPT_DIR}/build/coremind_bridge"

# 加载环境配置
_ENV="${AGENTFLOW_ENV:-dev}"
_ENV_FILE="$PROJECT_ROOT/configs/$_ENV/.env"
if [ -f "$_ENV_FILE" ]; then set -a; source "$_ENV_FILE"; set +a; fi
unset _ENV _ENV_FILE

SIDECAR_PORT="${COREMIND_BRIDGE_PORT:-23767}"
PYTHON_BRIDGE_PORT="${PYTHON_BRIDGE_PORT:-8199}"
DAEMON_HOST="${DAEMON_HOST:-127.0.0.1}"
DAEMON_PORT="${DAEMON_PORT:-17891}"

if [ ! -x "$SIDECAR_BIN" ]; then
    echo "[start] SideCar binary not found: $SIDECAR_BIN" >&2
    exit 1
fi

cleanup() {
    if [ -n "${SIDECAR_PID:-}" ]; then
        kill "$SIDECAR_PID" 2>/dev/null || true
        wait "$SIDECAR_PID" 2>/dev/null || true
    fi
    if [ -n "${BRIDGE_PID:-}" ]; then
        kill "$BRIDGE_PID" 2>/dev/null || true
        wait "$BRIDGE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

echo "[start] Starting internal Python Bridge on 127.0.0.1:${PYTHON_BRIDGE_PORT}"
PYTHONPATH="${PROJECT_ROOT}/frontend/bridge" \
BRIDGE_HOST=127.0.0.1 \
BRIDGE_PORT="$PYTHON_BRIDGE_PORT" \
DAEMON_HOST="$DAEMON_HOST" \
DAEMON_PORT="$DAEMON_PORT" \
python -m uvicorn main:app \
    --app-dir "${PROJECT_ROOT}/frontend/bridge" \
    --host 127.0.0.1 \
    --port "$PYTHON_BRIDGE_PORT" &
BRIDGE_PID=$!

for _ in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:${PYTHON_BRIDGE_PORT}/health/api/health" >/dev/null; then
        break
    fi
    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        echo "[start] Python Bridge exited during startup" >&2
        exit 1
    fi
    sleep 1
done

# ── 运行模式：生产环境走 CoreMind 框架（HTTPS HTTP/2），本地走 httplib ──
# 生产部署时设置 COREMIND_MODE=true 启用 --coremind-mode
COREMIND_MODE="${COREMIND_MODE:-false}"

"$SIDECAR_BIN" \
    --listen "0.0.0.0:${SIDECAR_PORT}" \
    --bridge-url "http://127.0.0.1:${PYTHON_BRIDGE_PORT}/health" \
    $( $COREMIND_MODE && echo "--coremind-mode" ) &
SIDECAR_PID=$!
wait "$SIDECAR_PID"
