#!/bin/bash
# Start daemon + internal Bridge + external SideCar as one production stack.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"
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
DAEMON_PORT="${DAEMON_PORT:-17891}"
BRIDGE_PORT="${PYTHON_BRIDGE_PORT:-8199}"
SIDECAR_PORT="${COREMIND_BRIDGE_PORT:-23767}"

if [ -z "${AISF_INTERNAL_TOKEN:-}" ]; then
    echo "[ERROR] AISF_INTERNAL_TOKEN is required" >&2
    exit 1
fi

DAEMON_BINARY="$AR_PROJECT_ROOT/build/production/agentflowd"
SIDECAR_BINARY="$AR_PROJECT_ROOT/frontend/bridge/coremind_bridge/build-production/coremind_bridge"
if [[ "$PACKAGE" = /* ]]; then PACKAGE_PATH="$PACKAGE"
else PACKAGE_PATH="$AR_PROJECT_ROOT/$PACKAGE"
fi
for required_file in "$DAEMON_BINARY" "$SIDECAR_BINARY" "$PACKAGE_PATH/agent.json"; do
    if [ ! -e "$required_file" ]; then
        echo "[ERROR] Production prerequisite missing: $required_file" >&2
        exit 1
    fi
done
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

PYTHON_BRIDGE_URL="http://127.0.0.1:$BRIDGE_PORT/health" \
    "$SCRIPT_DIR/start_sidecar.sh" production "$SIDECAR_PORT" &
PIDS+=("$!")

echo "[READY] daemon=$DAEMON_PORT bridge=127.0.0.1:$BRIDGE_PORT sidecar=0.0.0.0:$SIDECAR_PORT"
while true; do
    for index in 0 1 2; do
        if ! kill -0 "${PIDS[$index]}" 2>/dev/null; then
            wait "${PIDS[$index]}" || exit_code=$?
            echo "[ERROR] Production component $index exited (${exit_code:-0})" >&2
            exit "${exit_code:-1}"
        fi
    done
    sleep 1
done
