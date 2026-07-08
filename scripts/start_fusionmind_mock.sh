#!/bin/bash
# FusionMind Mock Server lifecycle manager — start / stop / restart.
#
# 用法：
#   bash ar_scripts/start_fusionmind_mock.sh start                  # 默认端口 8085，echo 模式
#   bash ar_scripts/start_fusionmind_mock.sh start --port 8085 --mode fixed --response "你好"
#   bash ar_scripts/start_fusionmind_mock.sh start --proxy          # 透传模式
#   bash ar_scripts/start_fusionmind_mock.sh stop
#   bash ar_scripts/start_fusionmind_mock.sh restart

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

PID_FILE="/tmp/fusionmind_mock.pid"

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
        pid=$(pgrep -f "fusionmind_mock.*server\.py" 2>/dev/null || true)
    fi
    echo "$pid"
}

# ── Helper: stop FMM ───────────────────────────────────────────────
_stop_fmm() {
    local pid="$1"
    echo "[INFO] Stopping FusionMind Mock Server (PID=$pid)..."
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
    echo "[INFO] FusionMind Mock Server stopped"
}

# ── Helper: wait for port ──────────────────────────────────────────
_wait_for_port() {
    local port="$1"
    local timeout="${2:-15}"
    for i in $(seq 1 "$timeout"); do
        if command -v nc >/dev/null 2>&1; then
            nc -z 127.0.0.1 "$port" 2>/dev/null && return 0
        else
            (echo > /dev/tcp/127.0.0.1/"$port") 2>/dev/null && return 0
        fi
        sleep 1
    done
    echo "[WARN] FusionMind Mock Server 启动超时（继续）"
    return 1
}

# ── Parse arguments ─────────────────────────────────────────────────
ACTION="start"
PORT=8085
HOST="0.0.0.0"
MODE="echo"
RESPONSE=""
MODEL="aisf/deepseek-v4-flash"
TOKEN_DELAY=0.02
COMPLETION_DELAY=0
ERROR_RATE=0.0
MOCK_USAGE=true
USE_RESULT_WRAPPER=false
PROXY=false
UPSTREAM_URL=""
API_KEY=""
LOG_LEVEL="info"
KEEP_LOGS=false

while [ $# -gt 0 ]; do
    case "$1" in
        start|stop|restart)     ACTION="$1"; shift ;;
        --port)                 PORT="${2:?}"; shift 2 ;;
        --host)                 HOST="${2:?}"; shift 2 ;;
        --mode)                 MODE="${2:?}"; shift 2 ;;
        --response)             RESPONSE="${2:?}"; shift 2 ;;
        --model)                MODEL="${2:?}"; shift 2 ;;
        --token-delay)          TOKEN_DELAY="${2:?}"; shift 2 ;;
        --completion-delay)     COMPLETION_DELAY="${2:?}"; shift 2 ;;
        --error-rate)           ERROR_RATE="${2:?}"; shift 2 ;;
        --no-mock-usage)        MOCK_USAGE=false; shift ;;
        --use-result-wrapper)   USE_RESULT_WRAPPER=true; shift ;;
        --proxy)                PROXY=true; shift ;;
        --upstream-url)         UPSTREAM_URL="${2:?}"; shift 2 ;;
        --api-key)              API_KEY="${2:?}"; shift 2 ;;
        --log-level)            LOG_LEVEL="${2:?}"; shift 2 ;;
        --keep-logs)            KEEP_LOGS=true; shift ;;
        -h|--help)
            echo "Usage: $0 [start|stop|restart] [options]"
            echo ""
            echo "Commands:"
            echo "  start    Start FusionMind Mock Server (default; fails if already running)"
            echo "  stop     Stop running server"
            echo "  restart  Stop then start"
            echo ""
            echo "Options:"
            echo "  --port PORT              Listen port (default: 8085)"
            echo "  --host HOST              Listen address (default: 0.0.0.0)"
            echo "  --mode MODE              Mock mode: echo|fixed|template|tool_calls (default: echo)"
            echo "  --response TEXT          Fixed mode response content"
            echo "  --model NAME             Model name (default: aisf/deepseek-v4-flash)"
            echo "  --token-delay SEC        Token interval (default: 0.02)"
            echo "  --completion-delay SEC   Non-streaming response delay (default: 0)"
            echo "  --error-rate RATE        Error injection probability 0-1"
            echo "  --no-mock-usage          Skip usage statistics simulation"
            echo "  --use-result-wrapper     Use FusionMind 'result' wrapper format"
            echo "  --proxy                  Proxy mode (passthrough to real LLM)"
            echo "  --upstream-url URL       Upstream LLM API base URL"
            echo "  --api-key KEY            Upstream LLM API key"
            echo "  --log-level LEVEL        Log level: debug|info|warning|error"
            echo "  --keep-logs              Keep previous log instead of archiving"
            exit 0 ;;
        *)  echo "[ERROR] Unknown argument: $1" >&2; exit 2 ;;
    esac
done

# ═══════════════════════════════════════════════════════════════════
#  stop
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "stop" ]; then
    PID=$(_resolve_pid)
    if [ -z "$PID" ]; then
        echo "[INFO] No running FusionMind Mock Server found"
        exit 0
    fi
    _stop_fmm "$PID"
    exit 0
fi

# ═══════════════════════════════════════════════════════════════════
#  restart → stop first, then fall through to start
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "restart" ]; then
    PID=$(_resolve_pid)
    if [ -n "$PID" ]; then
        _stop_fmm "$PID"
    fi
fi

# ═══════════════════════════════════════════════════════════════════
#  start
# ═══════════════════════════════════════════════════════════════════

# ── Guard: already running? ───────────────────────────────────────
PID=$(_resolve_pid)
if [ -n "$PID" ]; then
    echo "[WARN] FusionMind Mock Server is already running (PID=$PID)"
    echo "       To restart, use:  $0 restart"
    echo "       To stop, use:     $0 stop"
    exit 1
fi

# ── Resolve script path ────────────────────────────────────────────
FMM_SCRIPT="$AR_PROJECT_ROOT/frontend/mocks/mocks_fusionmind/server.py"
if [ ! -f "$FMM_SCRIPT" ]; then
    echo "[ERROR] FusionMind Mock 脚本不存在: $FMM_SCRIPT" >&2
    exit 1
fi

ar_load_env

# ── Build arguments ─────────────────────────────────────────────────
FMM_ARGS=("--port" "$PORT" "--host" "$HOST" "--log-level" "$LOG_LEVEL")

if [ "$PROXY" = true ]; then
    # Proxy 模式：从参数 / env 读取上游地址
    UPSTREAM_URL="${UPSTREAM_URL:-${AF_LLM_BASE_URL:-}}"
    API_KEY="${API_KEY:-${AF_LLM_API_KEY:-}}"
    [ -n "$UPSTREAM_URL" ] && FMM_ARGS+=(--upstream-url "$UPSTREAM_URL")
    [ -n "$API_KEY" ] && FMM_ARGS+=(--api-key "$API_KEY")
    FMM_ARGS+=(--proxy)
    MODE_LABEL="PROXY → ${UPSTREAM_URL:-<no upstream>}"
else
    FMM_ARGS+=(--mode "$MODE" --model "$MODEL")
    FMM_ARGS+=(--token-delay "$TOKEN_DELAY" --error-rate "$ERROR_RATE")
    [ -n "$RESPONSE" ] && FMM_ARGS+=(--response "$RESPONSE")
    [ "$MOCK_USAGE" = false ] && FMM_ARGS+=(--no-mock-usage)
    [ "$USE_RESULT_WRAPPER" = true ] && FMM_ARGS+=(--use-result-wrapper)
    MODE_LABEL="MOCK ($MODE)"
fi

# ── Log file ────────────────────────────────────────────────────────
LOG_DIR="$AR_PROJECT_ROOT/logs"
LOG_FILE="$LOG_DIR/fusionmind_mock.log"
mkdir -p "$LOG_DIR"
ar_archive_log "$LOG_FILE" "$KEEP_LOGS"

# ── Launch ──────────────────────────────────────────────────────────
echo "[INFO] Starting FusionMind Mock Server..."
echo "[INFO]   Mode:    $MODE_LABEL"
echo "[INFO]   Listen:  http://$HOST:$PORT"
echo "[INFO]   Logs →   $LOG_FILE"
echo "[INFO]   View:    tail -f $LOG_FILE"

nohup python3 "$FMM_SCRIPT" "${FMM_ARGS[@]}" \
    >> "$LOG_FILE" 2>&1 &
FMM_PID=$!
echo "$FMM_PID" > "$PID_FILE"
echo "[INFO] FusionMind Mock Server started (PID=$FMM_PID)"
echo "[INFO] Stop: $0 stop"

_wait_for_port "$PORT" 15
