#!/bin/bash
# Start the externally exposed C++ SideCar.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# 加载环境配置
ar_load_env

PROFILE="${1:-production}"
PORT="${2:-23767}"
BRIDGE_URL="${PYTHON_BRIDGE_URL:-http://127.0.0.1:8199/health}"
case "$PROFILE" in
    production) BINARY="$AR_PROJECT_ROOT/sidecar/build-production/coremind_bridge" ;;
    local) BINARY="$AR_PROJECT_ROOT/sidecar/build-dev/coremind_bridge" ;;
    -h|--help) echo "Usage: $0 [production|local] [port]"; exit 0 ;;
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

# CoreMind 服务发现环境变量（生产部署时由 caller 通过环境变量覆盖默认值）
#   APPID                      — CoreMind 租户/应用 ID，缺省 "0"
#   COREMIND_REQUIRED_SERVICES — 必需服务白名单，逗号分隔
#   COREMIND_OPTIONAL_SERVICES — 可选服务白名单，逗号分隔
# 这些 env var 在 production profile 下必须正确配置，否则 SideCar 发现不到任何服务。
# 本地 stub 模式（local profile）不读这些变量，改用 $COREMIND_STUB_SERVICES 注入伪实例。
export APPID="${APPID:-0}"
export COREMIND_REQUIRED_SERVICES="${COREMIND_REQUIRED_SERVICES:-}"
export COREMIND_OPTIONAL_SERVICES="${COREMIND_OPTIONAL_SERVICES:-}"

LOG_DIR="${AF_SIDECAR_LOG_DIR:-$AR_PROJECT_ROOT/logs}"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/sidecar.log"
# 归档旧日志
[ -f "$LOG_FILE" ] && mv "$LOG_FILE" "${LOG_FILE}.$(date +%Y%m%d-%H%M%S)" 2>/dev/null

echo "[sidecar] Starting on 0.0.0.0:$PORT, logging to $LOG_FILE"
echo "[sidecar] Binary: $BINARY"
echo "[sidecar] Bridge: $BRIDGE_URL"

# 运行时库路径：优先 build/production/（sidecar 构建产物目录），
# 次选 platform/lib/（CoreMind 平台库），可通过 AF_LIBRARY_PATH 环境变量覆盖
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

exec "$BINARY" --listen "0.0.0.0:$PORT" --bridge-url "$BRIDGE_URL" \
    $( [ "$PROFILE" = "production" ] && echo "--coremind-mode" ) >> "$LOG_FILE" 2>&1
