#!/bin/bash
# Daemon lifecycle manager — start / stop / restart agentflowd.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# ── Helper: resolve running PID ─────────────────────────────────────
_resolve_pid() {
    local -a pids=()
    if [ -f /tmp/agentflowd.pid ]; then
        local pid_file
        pid_file=$(cat /tmp/agentflowd.pid)
        if kill -0 "$pid_file" 2>/dev/null; then
            pids+=("$pid_file")
        else
            rm -f /tmp/agentflowd.pid
        fi
    fi
    if [ ${#pids[@]} -eq 0 ]; then
        # pgrep may return multiple PIDs (one per line); read them all
        while IFS= read -r line; do
            [ -n "$line" ] && pids+=("$line")
        done < <(pgrep -x agentflowd 2>/dev/null || true)
    fi
    # Return space-separated list so callers can iterate
    echo "${pids[*]:-}"
}

# ── Helper: stop daemon ─────────────────────────────────────────────
_stop_daemon() {
    local pids="$1"
    echo "[INFO] Stopping daemon (PID=$pids)..."
    # Kill all PIDs (space-separated list)
    kill $pids 2>/dev/null || true
    for _ in $(seq 1 10); do
        local all_dead=true
        for pid in $pids; do
            if kill -0 "$pid" 2>/dev/null; then
                all_dead=false
                break
            fi
        done
        $all_dead && break
        sleep 0.3
    done
    # Force-kill any survivors
    for pid in $pids; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    rm -f /tmp/agentflowd.pid
    echo "[INFO] Daemon stopped"
}

# ── Parse arguments ─────────────────────────────────────────────────
ACTION="start"
PACKAGE="examples/agent/outfit-assistant"
PORT="17891"
PROFILE="dev"
KEEP_LOGS=false
BUILD_FIRST=false
BUILD_OPTION_SET=false  # whether user explicitly passed --build / --no-build

while [ "$#" -gt 0 ]; do
    case "$1" in
        start|stop|restart)
            ACTION="$1"; shift ;;
        --package) PACKAGE="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
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
            echo "  --package PATH   Agent package directory (default: examples/agent/outfit-assistant)"
            echo "  --port PORT      Listen port (default: 17891)"
            echo "  --profile PROF   Build profile: dev|release|production (default: dev)"
            echo "  --build          Build binary first"
            echo "  --no-build       Skip build (e.g. quick config-only restart)"
            echo "  --keep-logs      Keep previous log instead of archiving"
            echo "  -r, --restart    Alias for restart command"
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

# restart 默认重新编译旧；显式 --no-build 可跳过
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
fi

# ═══════════════════════════════════════════════════════════════════
#  start
# ═══════════════════════════════════════════════════════════════════

# ── Guard: already running? ───────────────────────────────────────
PID=$(_resolve_pid)
if [ -n "$PID" ]; then
    echo "[WARN] Daemon is already running (PID=$PID)"
    echo "       To restart, use:  $0 restart"
    echo "       To stop, use:     $0 stop"
    exit 1
fi

# ── Resolve binary ──────────────────────────────────────────────────
case "$PROFILE" in
    dev) BUILD_DIR="$AR_PROJECT_ROOT/build/" ;;
    release) BUILD_DIR="$AR_PROJECT_ROOT/build" ;;
    production) BUILD_DIR="$AR_PROJECT_ROOT/build/production" ;;
    *) echo "[ERROR] Invalid profile: $PROFILE" >&2; exit 2 ;;
esac

if $BUILD_FIRST; then
    "$SCRIPT_DIR/build.sh" "$PROFILE" agentflowd
fi
DAEMON="$BUILD_DIR/agentflowd"
if [ ! -x "$DAEMON" ]; then
    echo "[ERROR] Missing daemon binary: $DAEMON" >&2
    echo "        Run: ./ar_scripts/build.sh $PROFILE agentflowd" >&2
    exit 1
fi

ar_load_env
LOG_FILE="$AR_PROJECT_ROOT/logs/agentflowd.log"
ar_archive_log "$LOG_FILE" "$KEEP_LOGS"

if [[ "$PACKAGE" = /* ]]; then PACKAGE_PATH="$PACKAGE"
else PACKAGE_PATH="$AR_PROJECT_ROOT/$PACKAGE"
fi

# ── Launch ──────────────────────────────────────────────────────────
echo "[INFO] Starting daemon in background..."
echo "[INFO] Daemon logs → $LOG_FILE"
echo "[INFO] View live: tail -f $LOG_FILE"
nohup "$DAEMON" \
    --package "$PACKAGE_PATH" \
    --listen "127.0.0.1:$PORT" \
    >> "$LOG_FILE" 2>&1 &
DAEMON_PID=$!
echo "$DAEMON_PID" > /tmp/agentflowd.pid
echo "[INFO] Daemon started (PID=$DAEMON_PID)"
echo "[INFO] Stop: $0 stop"
