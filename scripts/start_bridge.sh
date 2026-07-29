#!/bin/bash
# Bridge lifecycle manager — start / stop / restart the Python Bridge (uvicorn).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# 加载环境配置
ar_load_env

PID_FILE="/tmp/agentflow-bridge.pid"

# ── Helper: resolve running PID ─────────────────────────────────────
_resolve_pid() {
    local pid=""
    if [ -f "$PID_FILE" ]; then
        pid=$(cat "$PID_FILE")
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$PID_FILE"
            pid=""
        fi
    fi
    if [ -z "$pid" ]; then
        pid=$(pgrep -f "uvicorn.*main:app.*frontend/bridge" 2>/dev/null || true)
    fi
    echo "$pid"
}

# ── Helper: stop bridge ─────────────────────────────────────────────
_stop_bridge() {
    local pid="$1"
    echo "[INFO] Stopping bridge (PID=$pid)..."
    kill "$pid" 2>/dev/null
    for _ in $(seq 1 10); do
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        sleep 0.3
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
    echo "[INFO] Bridge stopped"
}

# ── Helper: wait for port to become available ──────────────────────
_wait_port_free() {
    local port="$1"
    local timeout="${2:-5}"  # default 5 seconds
    local max_steps=$((timeout * 10))  # 0.1s per step
    local step=0
    echo "[INFO] Waiting for port $port to become available (timeout=${timeout}s)..."
    while [ "$step" -lt "$max_steps" ]; do
        if ! lsof -i :"$port" -P -n 2>/dev/null | grep -q LISTEN; then
            local elapsed
            elapsed=$(echo "scale=1; $step / 10" | bc 2>/dev/null || echo "$step")
            echo "[INFO] Port $port is free (after ${elapsed}s)"
            return 0
        fi
        sleep 0.1
        step=$((step + 1))
    done
    echo "[WARN] Port $port still in use after ${timeout}s timeout (continuing anyway)"
    return 1
}

# ── Parse arguments ─────────────────────────────────────────────────
ACTION="start"
MODE="dev"
PORT="8199"
LOG_LEVEL="debug"
NO_RELOAD=false
KEEP_LOGS=false

while [ $# -gt 0 ]; do
    case "$1" in
        start|stop|restart)     ACTION="$1"; shift ;;
        --port|-p)              PORT="${2:?Missing port value}"; shift 2 ;;
        --log-level|-l)         LOG_LEVEL="${2:?Missing log level}"; shift 2 ;;
        --no-reload|-r)         NO_RELOAD=true; shift ;;
        --keep-logs)            KEEP_LOGS=true; shift ;;
        -h|--help)
            echo "Usage: $0 [start|stop|restart] [options]"
            echo ""
            echo "Commands:"
            echo "  start    Start bridge (default; fails if already running)"
            echo "  stop     Stop running bridge"
            echo "  restart  Stop then start"
            echo ""
            echo "Options:"
            echo "  --port|-p PORT         Listen port (default: 8199)"
            echo "  --log-level|-l LEVEL   uvicorn log level: debug|info|warning (default: info)"
            echo "  --no-reload|-r         Disable hot-reload (dev mode only)"
            echo "  --keep-logs            Keep previous log instead of archiving"
            echo "  dev|test|production    Run mode (default: dev)"
            exit 0 ;;
        dev|test|production)    MODE="$1"; shift ;;
        *)                      echo "[ERROR] Unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$MODE" in
    dev)
        HOST="0.0.0.0"
        export AGENTFLOW_ENV="dev" AF_BR_ENV="dev"
        if [ "$NO_RELOAD" = false ]; then
            RELOAD=(--reload --reload-dir "$AR_PROJECT_ROOT/frontend/bridge")
        else
            RELOAD=()
        fi
        ;;
    test)
            HOST="127.0.0.1"
            export AGENTFLOW_ENV="test" AF_BR_ENV="test"
            RELOAD=()
            ;;
        production)
            HOST="127.0.0.1"
            export AGENTFLOW_ENV="prod" AF_BR_ENV="prod"
            RELOAD=()
            ;;
esac

ar_require_command python3
if ! PYTHONPATH="$AR_PROJECT_ROOT/frontend/bridge" python3 -c "import fastapi, uvicorn" 2>/dev/null; then
    echo "[ERROR] Bridge dependencies are missing; install frontend/bridge/requirements.txt" >&2
    exit 1
fi

export BRIDGE_HOST="$HOST" BRIDGE_PORT="$PORT"

# ═══════════════════════════════════════════════════════════════════
#  stop
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "stop" ]; then
    PID=$(_resolve_pid)
    if [ -z "$PID" ]; then
        echo "[INFO] No running bridge found"
        exit 0
    fi
    _stop_bridge "$PID"
    exit 0
fi

# ═══════════════════════════════════════════════════════════════════
#  restart → stop first, then fall through to start
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "restart" ]; then
    PID=$(_resolve_pid)
    if [ -n "$PID" ]; then
        _stop_bridge "$PID"
    fi
    _wait_port_free "$PORT" 5
fi

# ═══════════════════════════════════════════════════════════════════
#  start
# ═══════════════════════════════════════════════════════════════════

# ── Guard: already running? ───────────────────────────────────────
PID=$(_resolve_pid)
if [ -n "$PID" ]; then
    echo "[WARN] Bridge is already running (PID=$PID)"
    echo "       To restart, use:  $0 restart"
    echo "       To stop, use:     $0 stop"
    exit 1
fi

# ── 确保端口可用 ─────────────────────────────────────────────────────
_wait_port_free "$PORT" 5 || true

# ── 日志文件 ─────────────────────────────────────────────────────────
LOG_DIR="${AF_BRIDGE_LOG_DIR:-$AR_PROJECT_ROOT/logs}"
LOG_FILE="$LOG_DIR/bridge.log"
mkdir -p "$LOG_DIR"
ar_archive_log "$LOG_FILE" "$KEEP_LOGS"

# ── 后台运行 ─────────────────────────────────────────────────────────
echo "[INFO] Starting bridge in background..."
echo "[INFO] Bridge logs → $LOG_FILE"
echo "[INFO] View live: tail -f $LOG_FILE"
nohup python3 -m uvicorn main:app \
    --app-dir "$AR_PROJECT_ROOT/frontend/bridge" \
    --host "$HOST" --port "$PORT" \
    "${RELOAD[@]+"${RELOAD[@]}"}" \
    --log-level "$LOG_LEVEL" \
    >> "$LOG_FILE" 2>&1 &
BRIDGE_PID=$!
echo "$BRIDGE_PID" > "$PID_FILE"
echo "[INFO] Bridge started (PID=$BRIDGE_PID)"
echo "[INFO] Stop: $0 stop"
