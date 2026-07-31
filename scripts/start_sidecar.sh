#!/bin/bash
# Start / stop / restart the C++ CoreMind SideCar (coremind_bridge).
# Aligned with start_daemon.sh / start_bridge.sh lifecycle pattern.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# 加载环境配置
ar_load_env

PID_FILE="/tmp/agentflow-sidecar.pid"

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
        pid=$(pgrep -x coremind_bridge 2>/dev/null || true)
    fi
    echo "$pid"
}

# ── Helper: stop sidecar ────────────────────────────────────────────
_stop_sidecar() {
    local pid="$1"
    echo "[INFO] Stopping sidecar (PID=$pid)..."
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
    echo "[INFO] SideCar stopped"
}

# ── Parse arguments ─────────────────────────────────────────────────
ACTION="start"
PROFILE="${1:-production}"
PORT="${2:-23767}"
KEEP_LOGS=false

while [ $# -gt 0 ]; do
    case "$1" in
        start|stop|restart) ACTION="$1"; shift ;;
        --port|-p)          PORT="${2:?Missing port value}"; shift 2 ;;
        --profile)          PROFILE="$2"; shift 2 ;;
        --keep-logs)        KEEP_LOGS=true; shift ;;
        -h|--help)
            echo "Usage: $0 [start|stop|restart] [options]"
            echo ""
            echo "Commands:"
            echo "  start    Start sidecar (default; fails if already running)"
            echo "  stop     Stop running sidecar"
            echo "  restart  Stop then start"
            echo ""
            echo "Options:"
            echo "  --port PORT       Listen port (default: 23767)"
            echo "  --profile PROF    Build profile: production|local (default: production)"
            echo "  --keep-logs       Keep previous log instead of archiving"
            echo ""
            echo "Environment:"
            echo "  AF_SIDECAR_LOG_DIR   SideCar log directory (default: logs/)"
            exit 0 ;;
        *) echo "[ERROR] Unknown argument: $1" >&2; exit 2 ;;
    esac
done

# ═══════════════════════════════════════════════════════════════════
#  stop
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "stop" ]; then
    PID=$(_resolve_pid)
    if [ -z "$PID" ]; then
        echo "[INFO] No running sidecar found"
        exit 0
    fi
    _stop_sidecar "$PID"
    exit 0
fi

# ═══════════════════════════════════════════════════════════════════
#  restart → stop first, then fall through to start
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "restart" ]; then
    PID=$(_resolve_pid)
    if [ -n "$PID" ]; then
        _stop_sidecar "$PID"
    fi
fi

# ═══════════════════════════════════════════════════════════════════
#  start
# ═══════════════════════════════════════════════════════════════════

# ── Guard: already running? ───────────────────────────────────────
PID=$(_resolve_pid)
if [ -n "$PID" ]; then
    echo "[WARN] SideCar is already running (PID=$PID)"
    echo "       To restart, use:  $0 restart"
    echo "       To stop, use:     $0 stop"
    exit 1
fi

# ── Resolve binary ──────────────────────────────────────────────────
BRIDGE_URL="${PYTHON_BRIDGE_URL:-http://127.0.0.1:8199/health}"
case "$PROFILE" in
    production) BINARY="$AR_PROJECT_ROOT/sidecar/build-production/coremind_bridge" ;;
    local)      BINARY="$AR_PROJECT_ROOT/sidecar/build-dev/coremind_bridge" ;;
    *) echo "[ERROR] Profile must be production or local" >&2; exit 2 ;;
esac

if [ "$PROFILE" = "local" ]; then
    echo "[sidecar] Building for local profile..."
    BUILD_DIR="$AR_PROJECT_ROOT/sidecar/build-dev"
    if [ ! -d "$BUILD_DIR" ]; then
        cmake -S "$AR_PROJECT_ROOT/sidecar" -B "$BUILD_DIR"
    fi
    cmake --build "$BUILD_DIR" --target coremind_bridge -j4
    if [ ! -x "$BINARY" ]; then
        echo "[ERROR] Build failed: $BINARY not produced" >&2
        exit 1
    fi
    echo "[sidecar] Build succeeded"
elif [ ! -x "$BINARY" ]; then
    echo "[ERROR] Missing SideCar binary: $BINARY" >&2
    exit 1
fi

# ── CoreMind 运行时环境变量 ─────────────────────────────────────────
# CPU 架构检测（与工作版 start.sh 对齐）
CPU_ARCH=$(arch)
export CPU_ARCHITECTURE="${CPU_ARCHITECTURE:-X86}"
if [ "$CPU_ARCH" == "aarch64" ]; then
    export CPU_ARCHITECTURE="${CPU_ARCHITECTURE:-ARM}"
fi

export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-1}"
export START_UDS_SERVER="${START_UDS_SERVER:-on}"

export APPID="${APPID:-0}"
export COREMIND_REQUIRED_SERVICES="${COREMIND_REQUIRED_SERVICES:-}"
export COREMIND_OPTIONAL_SERVICES="${COREMIND_OPTIONAL_SERVICES:-}"
export SERVICENAME="${SERVICENAME:-OutfitAgent}"
export PROCESS_TYPE="${PROCESS_TYPE:-OutfitAgent}"
export CSP_PROCESS_ID="${CSP_PROCESS_ID:-OutfitAgent}"
export SYSTEM_AGENT_NAME="${SYSTEM_AGENT_NAME:-OutfitAgent}"
export TZ="${TZ:-Asia/Shanghai}"
export RUNLOG_CONFIG="${RUNLOG_CONFIG:-/opt/coremind/conf/log.json}"
export LOG_CONFIG="${LOG_CONFIG:-/opt/coremind/conf/log.json}"
export NON_BYPASS_BBOX_PATH="${NON_BYPASS_BBOX_PATH:-/opt/coremind/logs/bbox}"
export SSLPATH="${SSLPATH:-/opt/sharevolume/ssl/cert}"
export SSL_CERT_PATH="${SSL_CERT_PATH:-/opt/sharevolume/ssl/cert}"
export CIPHER_ROOT="${CIPHER_ROOT:-/opt/sharevolume/ssl/cipher}"
export CIPHER_PATH="${CIPHER_PATH:-/opt/sharevolume/ssl/cipher}"
export CIPHERPATH="${CIPHERPATH:-/opt/sharevolume/ssl/cipher}"
export ROOT_KEY_HOME="${ROOT_KEY_HOME:-/opt/sharevolume/ssl/cipher}"
export WORK_KEY_HOME="${WORK_KEY_HOME:-/opt/sharevolume/ssl/cipher}"
export INNER_TLS_ENABLE="${INNER_TLS_ENABLE:-true}"
export SSL_ENABLE="${SSL_ENABLE:-false}"

# 打印 CoreMind 框架关键 env vars
echo "[sidecar] CoreMind init env vars:"
echo "  SSLPATH=$SSLPATH"
echo "  SSL_CERT_PATH=$SSL_CERT_PATH"
echo "  SSL_ENABLE=$SSL_ENABLE"
echo "  INNER_TLS_ENABLE=$INNER_TLS_ENABLE"
echo "  SERVICENAME=$SERVICENAME"
echo "  PROCESS_TYPE=$PROCESS_TYPE"
echo "  APPID=$APPID"
echo "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "  TZ=$TZ"
echo "  RUNLOG_CONFIG=$RUNLOG_CONFIG"
echo "  NON_BYPASS_BBOX_PATH=$NON_BYPASS_BBOX_PATH"
echo "  CIPHER_PATH=$CIPHER_PATH"
echo "  ROOT_KEY_HOME=$ROOT_KEY_HOME"
if [ -d "$SSLPATH" ]; then
    echo "  [ok] SSLPATH directory exists"
    ls -la "$SSLPATH/" | head -10
else
    echo "  [WARN] SSLPATH directory MISSING"
fi

# ── 运行时库路径 ────────────────────────────────────────────────────
_LIB_DIRS=(
    "$AR_PROJECT_ROOT/build/production"
    "$AR_PROJECT_ROOT/../platform/lib"
)
for _dir in "${_LIB_DIRS[@]}"; do
    if [ -d "$_dir" ]; then
        _RESOLVED_DIR=$(cd "$_dir" 2>/dev/null && pwd)
        if [ -n "$_RESOLVED_DIR" ]; then
            LIBRARY_PATH="${LIBRARY_PATH:+$LIBRARY_PATH:}$_RESOLVED_DIR"
        fi
    fi
done
export LD_LIBRARY_PATH="${AF_LIBRARY_PATH:-$LIBRARY_PATH}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# CoreMind 框架 initServiceConfig 要求 SSLPATH 目录存在
mkdir -p "$SSLPATH"

# ── 同步 serviceconfig 到共享卷（与工作版 start.sh 对齐）──
if [ -d "/opt/coremind/serviceconfig" ]; then
    echo "[sidecar] Syncing serviceconfig to /opt/sharevolume..."
    rm -rf /opt/sharevolume/serviceconfig
    cp -R /opt/coremind/serviceconfig /opt/sharevolume/
    if [ -f "$SCRIPT_DIR/serviceconfig.py" ]; then
        python "$SCRIPT_DIR/serviceconfig.py" 2>/dev/null || true
    fi
fi

# ── 日志文件 ─────────────────────────────────────────────────────────
LOG_DIR="${AF_SIDECAR_LOG_DIR:-$AR_PROJECT_ROOT/logs}"
LOG_FILE="$LOG_DIR/sidecar.log"
mkdir -p "$LOG_DIR"
ar_archive_log "$LOG_FILE" "$KEEP_LOGS"

# ── 后台启动 ─────────────────────────────────────────────────────────
echo "[sidecar] Starting on 0.0.0.0:$PORT, logging to $LOG_FILE"
echo "[sidecar] Binary: $BINARY"
echo "[sidecar] Bridge: $BRIDGE_URL"
echo "[INFO] Starting sidecar in background..."
echo "[INFO] SideCar logs → $LOG_FILE"
echo "[INFO] View live: tail -f $LOG_FILE"

nohup "$BINARY" \
    --listen "0.0.0.0:$PORT" \
    --bridge-url "$BRIDGE_URL" \
    -processType "${SERVICENAME:-OutfitAgent}" \
    $( [ "$PROFILE" = "production" ] && echo "--coremind-mode" ) \
    >> "$LOG_FILE" 2>&1 &
SIDECAR_PID=$!
echo "$SIDECAR_PID" > "$PID_FILE"
echo "[INFO] SideCar started (PID=$SIDECAR_PID)"
echo "[INFO] Stop: $0 stop"
