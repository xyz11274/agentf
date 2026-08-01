#!/bin/bash
# Daemon lifecycle manager — start / stop / restart agentflowd.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# 加载环境配置
ar_load_env

# ── Helper: is PID a zombie? ──────────────────────────────────────────
_is_zombie() {
    local pid="$1"
    # macOS: ps -o state= 返回进程状态首字母，Z = zombie
    local state
    state=$(ps -o state= -p "$pid" 2>/dev/null | head -1 | tr -d ' ')
    [ "$state" = "Z" ]
}

# ── Helper: check if PID is alive and not zombie ──────────────────────
_is_alive() {
    local pid="$1"
    kill -0 "$pid" 2>/dev/null || return 1
    _is_zombie "$pid" && return 1 || return 0
}

# ── Helper: check if port is in use (any address) ─────────────────────
# Returns "PID PROCESS_NAME" if occupied, empty string if free.
_check_port() {
    local port="$1"
    lsof -i "tcp:${port}" -s TCP:LISTEN 2>/dev/null | awk 'NR>1 {print $2, $1}' || true
}

# ── Helper: resolve running PID ─────────────────────────────────────
_resolve_pid() {
    local -a pids=()
    # 优先读 PID 文件
    if [ -f /tmp/agentflowd.pid ]; then
        local pid_file
        pid_file=$(cat /tmp/agentflowd.pid)
        if _is_alive "$pid_file"; then
            pids+=("$pid_file")
        else
            rm -f /tmp/agentflowd.pid
        fi
    fi
    # 再 pgrep 全系统查（排除已死的僵尸）
    if [ ${#pids[@]} -eq 0 ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && _is_alive "$line" && pids+=("$line")
        done < <(pgrep -x agentflowd 2>/dev/null || true)
    fi
    echo "${pids[*]:-}"
}

# ── Helper: stop daemon ─────────────────────────────────────────────
_stop_daemon() {
    local pids="$1"
    echo "[INFO] Stopping daemon (PID=$pids)..."
    # SIGTERM first
    kill $pids 2>/dev/null || true
    for _ in $(seq 1 10); do
        local all_dead=true
        for pid in $pids; do
            if _is_alive "$pid"; then
                all_dead=false
                break
            fi
        done
        $all_dead && break
        sleep 0.3
    done
    # SIGKILL survivors (僵尸进程 kill -9 无效，忽略)
    for pid in $pids; do
        if _is_alive "$pid"; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    # 清理 PID 文件
    rm -f /tmp/agentflowd.pid
    echo "[INFO] Daemon stopped"
}

# ── Config file path (per environment) ─────────────────────────────
_AGENTFLOW_ENV="${AGENTFLOW_ENV:-dev}"
_CONFIG_FILE="$AR_PROJECT_ROOT/configs/$_AGENTFLOW_ENV/agentflowd.json"

# ── Parse arguments ─────────────────────────────────────────────────
ACTION="start"
PACKAGE="examples/agent/outfit-assistant"
PORT="17891"
case "$_AGENTFLOW_ENV" in
    prod) PROFILE="production" ;;
    *)    PROFILE="dev" ;;
esac
CONFIG_FILE="$_CONFIG_FILE"
KEEP_LOGS=false
BUILD_FIRST=false
BUILD_OPTION_SET=false

while [ "$#" -gt 0 ]; do
    case "$1" in
        start|stop|restart)
            ACTION="$1"; shift ;;
        --package) PACKAGE="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --config) CONFIG_FILE="$2"; shift 2 ;;
        --keep-logs) KEEP_LOGS=true; shift ;;
        --build) BUILD_FIRST=true; BUILD_OPTION_SET=true; shift ;;
        --no-build) BUILD_FIRST=false; BUILD_OPTION_SET=true; shift ;;
        --restart|-r) ACTION="restart"; shift ;;
        -h|--help)
            echo "Usage: $0 [start|stop|restart] [options]"
            echo ""
            echo "Commands:"
            echo "  start    Start daemon (default; fails if already running)"
            echo "  stop     Stop running daemon"
            echo "  restart  Stop, rebuild, then start (default: --build)"
            echo ""
            echo "Options:"
            echo "  --package PATH   Agent package directory"
            echo "  --port PORT      Listen port (default: 17891)"
            echo "  --profile PROF   Build profile: dev|release|production (default: dev)"
            echo "  --config PATH    Config file (default: configs/\$AGENTFLOW_ENV/agentflowd.json)"
            echo "  --build          Build binary first"
            echo "  --no-build       Skip build"
            echo "  --keep-logs      Keep previous log instead of archiving"
            echo "  -r, --restart    Alias for restart command"
            echo ""
            echo "Environment:"
            echo "  AGENTFLOW_ENV    dev|prod (default: dev)"
            echo "  AF_LOG_FILE      Daemon log path (default: logs/agentflowd.log)"
            exit 0 ;;
        *)
            if [ "$PACKAGE" = "examples/agent/agentflow_assistant" ]; then PACKAGE="$1"
            elif [ "$PORT" = "17891" ]; then PORT="$1"
            else echo "[ERROR] Unknown argument: $1" >&2; exit 2
            fi
            shift
            ;;
    esac
done

# restart 默认重新编译
if [ "$ACTION" = "restart" ] && [ "$BUILD_OPTION_SET" = false ]; then
    BUILD_FIRST=true
fi

# ═══════════════════════════════════════════════════════════════════
#  stop
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "stop" ]; then
    PID=$(_resolve_pid)
    if [ -z "$PID" ]; then
        echo "[INFO] No running daemon found"
        exit 0
    fi
    _stop_daemon "$PID"
    exit 0
fi

# ═══════════════════════════════════════════════════════════════════
#  restart  → stop first, then fall through to start
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "restart" ]; then
    PID=$(_resolve_pid)
    if [ -n "$PID" ]; then
        _stop_daemon "$PID"
    fi
    # After stopping daemon, check if port is still occupied by another process
    OCCUPIER=$(_check_port "$PORT")
    if [ -n "$OCCUPIER" ]; then
        OCCUPIER_PID=$(echo "$OCCUPIER" | awk '{print $1}')
        OCCUPIER_NAME=$(echo "$OCCUPIER" | awk '{print $2}')
        echo "[WARN] Port $PORT is occupied by PID=$OCCUPIER_PID ($OCCUPIER_NAME)"
        echo "[INFO] Attempting to free port $PORT..."
        kill "$OCCUPIER_PID" 2>/dev/null || true
        for _ in $(seq 1 5); do
            OCCUPIER=$(_check_port "$PORT")
            [ -z "$OCCUPIER" ] && break
            sleep 0.3
        done
        OCCUPIER=$(_check_port "$PORT")
        if [ -n "$OCCUPIER" ]; then
            OCCUPIER_PID=$(echo "$OCCUPIER" | awk '{print $1}')
            echo "[WARN] Process $OCCUPIER_PID did not exit gracefully, sending SIGKILL..."
            kill -9 "$OCCUPIER_PID" 2>/dev/null || true
            sleep 0.3
        fi
        # Final check
        OCCUPIER=$(_check_port "$PORT")
        if [ -n "$OCCUPIER" ]; then
            OCCUPIER_PID=$(echo "$OCCUPIER" | awk '{print $1}')
            OCCUPIER_NAME=$(echo "$OCCUPIER" | awk '{print $2}')
            echo "[ERROR] Cannot free port $PORT (PID=$OCCUPIER_PID, $OCCUPIER_NAME)"
            exit 1
        fi
        echo "[INFO] Port $PORT freed"
    fi
fi

# ═══════════════════════════════════════════════════════════════════
#  start
# ═══════════════════════════════════════════════════════════════════

# ── Guard: already running? ───────────────────────────────────────
PID=$(_resolve_pid)
if [ -n "$PID" ]; then
    echo "[WARN] Daemon is already running (PID=$PID)"
    echo "       To restart, use:  $0 restart"
    exit 1
fi

# ── Guard: port already in use by another process? ────────────────
OCCUPIER=$(_check_port "$PORT")
if [ -n "$OCCUPIER" ]; then
    OCCUPIER_PID=$(echo "$OCCUPIER" | awk '{print $1}')
    OCCUPIER_NAME=$(echo "$OCCUPIER" | awk '{print $2}')
    echo "[ERROR] Port $PORT is already in use by PID=$OCCUPIER_PID ($OCCUPIER_NAME)"
    echo "       Use '$0 restart' to force-clear the port, or kill the process manually"
    exit 1
fi

# ── Resolve binary ──────────────────────────────────────────────────
case "$PROFILE" in
    dev)
        if [ -x "$AR_PROJECT_ROOT/build/agentflowd" ]; then
            BUILD_DIR="$AR_PROJECT_ROOT/build"
        else
            BUILD_DIR="$AR_PROJECT_ROOT/build/dev"
        fi
        ;;
    release)
        BUILD_DIR="$AR_PROJECT_ROOT/build"
        ;;
    production)
        if [ -x "$AR_PROJECT_ROOT/build/production/agentflowd" ]; then
            BUILD_DIR="$AR_PROJECT_ROOT/build/production"
        else
            BUILD_DIR="$AR_PROJECT_ROOT/build"
        fi
        ;;
    *) echo "[ERROR] Invalid profile: $PROFILE" >&2; exit 2 ;;
esac

if $BUILD_FIRST; then
    "$SCRIPT_DIR/build.sh" "$PROFILE" agentflowd
fi
DAEMON="$BUILD_DIR/agentflowd"
if [ ! -x "$DAEMON" ]; then
    echo "[ERROR] Missing daemon binary: $DAEMON" >&2
    echo "        Run: ./scripts/build.sh $PROFILE agentflowd" >&2
    exit 1
fi

LOG_FILE="${AF_LOG_FILE:-$AR_PROJECT_ROOT/logs/agentflowd.log}"
ar_archive_log "$LOG_FILE" "$KEEP_LOGS"

if [[ "$PACKAGE" = /* ]]; then PACKAGE_PATH="$PACKAGE"
else PACKAGE_PATH="$AR_PROJECT_ROOT/$PACKAGE"
fi

# ── Launch (注入 AF_LOG_FILE 确保 skill_stderr 日志落 logs/) ──────────
echo "[INFO] Starting daemon in background..."
echo "[INFO] Daemon logs → $LOG_FILE"
echo "[INFO] Config file → $CONFIG_FILE"
echo "[INFO] View live: tail -f $LOG_FILE"
export AF_LOG_FILE="$LOG_FILE"
nohup "$DAEMON" \
    --config "$CONFIG_FILE" \
    --package "$PACKAGE_PATH" \
    --listen "127.0.0.1:$PORT" \
    >> "$LOG_FILE" 2>&1 &
DAEMON_PID=$!
echo "$DAEMON_PID" > /tmp/agentflowd.pid
echo "[INFO] Daemon started (PID=$DAEMON_PID)"
echo "[INFO] Stop: $0 stop"
