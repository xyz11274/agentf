#!/bin/bash
# scripts/start_upstream_mock.sh
# CoreMind 上游 Mock 服务器 lifecycle manager — start / stop / restart.
#
# 用法：
#   bash scripts/start_upstream_mock.sh start                # 默认 127.0.0.1:23768
#   bash scripts/start_upstream_mock.sh start --reload       # uvicorn --reload 模式
#   bash scripts/start_upstream_mock.sh stop
#   bash scripts/start_upstream_mock.sh restart
#
# 前置条件:
#   bridge/daemon/sidecar 已运行
#
# 这个 Mock 服务器模拟 CoreMind 上游平台:
#   1. 接收前端发送指令（POST /send）
#   2. 构造 AccessCtrl 请求发送到 sidecar
#   3. 接收 sidecar 的异步回调
#   4. 通过 SSE 推回前端

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MOCK_HOST="${UPSTREAM_MOCK_HOST:-127.0.0.1}"
MOCK_PORT="${UPSTREAM_MOCK_PORT:-23768}"
PID_FILE="/tmp/upstream_mock.pid"

# ── 参数解析 ─────────────────────────────────────────────────────────
ACTION="start"
RELOAD=""
while [ $# -gt 0 ]; do
    case "$1" in
        start|stop|restart) ACTION="$1"; shift ;;
        --reload)           RELOAD="--reload"; shift ;;
        -h|--help)
            echo "Usage: $0 [start|stop|restart] [--reload]"
            echo ""
            echo "Commands:"
            echo "  start    Start Upstream Mock Server (default; fails if already running)"
            echo "  stop     Stop running server"
            echo "  restart  Stop then start"
            echo ""
            echo "Options:"
            echo "  --reload       uvicorn --reload 模式（import string 方式启动）"
            echo ""
            echo "Environment:"
            echo "  UPSTREAM_MOCK_HOST   Listen host (default: 127.0.0.1)"
            echo "  UPSTREAM_MOCK_PORT   Listen port (default: 23768)"
            exit 0 ;;
        *)  echo "[ERROR] Unknown argument: $1" >&2; exit 2 ;;
    esac
done

# ── Helper: resolve running PID ──────────────────────────────────────
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
        pid=$(pgrep -f "upstream_mock.*server\.py" 2>/dev/null || true)
    fi
    echo "$pid"
}

# ── Helper: stop upstream mock ───────────────────────────────────────
_stop_upstream_mock() {
    local pid="$1"
    echo "[INFO] Stopping Upstream Mock (PID=$pid)..."
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
    echo "[INFO] Upstream Mock stopped"
}

# ═══════════════════════════════════════════════════════════════════
#  stop
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "stop" ]; then
    PID=$(_resolve_pid)
    if [ -z "$PID" ]; then
        echo "[INFO] No running Upstream Mock found"
        exit 0
    fi
    _stop_upstream_mock "$PID"
    exit 0
fi

# ═══════════════════════════════════════════════════════════════════
#  restart → stop first, then fall through to start
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "restart" ]; then
    PID=$(_resolve_pid)
    if [ -n "$PID" ]; then
        _stop_upstream_mock "$PID"
    fi
fi

# ═══════════════════════════════════════════════════════════════════
#  start
# ═══════════════════════════════════════════════════════════════════

# ── Guard: already running? ─────────────────────────────────────────
PID=$(_resolve_pid)
if [ -n "$PID" ]; then
    echo "[WARN] Upstream Mock is already running (PID=$PID)"
    echo "       To restart, use:  $0 restart"
    echo "       To stop, use:     $0 stop"
    exit 1
fi

# ── 端口占用兜底清理（PID 无法解析时的孤儿进程）────────────────────
if lsof -i :$MOCK_PORT > /dev/null 2>&1; then
    OLD_PID=$(lsof -ti :$MOCK_PORT)
    echo "[upstream-mock] Port $MOCK_PORT in use by PID $OLD_PID, killing..."
    kill "$OLD_PID" 2>/dev/null
    sleep 1
    # 如未释放则强制
    if lsof -i :$MOCK_PORT > /dev/null 2>&1; then
        kill -9 "$OLD_PID" 2>/dev/null
        sleep 1
    fi
    echo "[upstream-mock] Old process killed"
fi

# ── Log file ─────────────────────────────────────────────────────────
LOG_DIR="${PROJECT_ROOT}/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="${LOG_DIR}/upstream_mock.log"

# ── 确保依赖可用（FastAPI + uvicorn）────────────────────────────────
if ! python3 -c "import fastapi, uvicorn" 2>/dev/null; then
    echo "[upstream-mock] Installing dependencies..."
    pip install fastapi uvicorn httpx 2>&1 | tail -1
fi

# ── Launch ───────────────────────────────────────────────────────────
cd "$PROJECT_ROOT"
echo "[INFO] Starting Upstream Mock on $MOCK_HOST:$MOCK_PORT ..."
echo "[INFO] Logs → $LOG_FILE"
echo "[INFO] View: tail -f $LOG_FILE"

if [ -n "$RELOAD" ]; then
    # --reload 需要 import string 格式
    nohup python3 -m uvicorn frontend.mocks.upstream_mock.server:app \
        --host "$MOCK_HOST" --port "$MOCK_PORT" --reload \
        >> "$LOG_FILE" 2>&1 &
else
    nohup python3 frontend/mocks/upstream_mock/server.py \
        >> "$LOG_FILE" 2>&1 &
fi
MOCK_PID=$!
echo "$MOCK_PID" > "$PID_FILE"
echo "[INFO] Upstream Mock started (PID=$MOCK_PID)"
echo "[INFO] Stop: $0 stop"
