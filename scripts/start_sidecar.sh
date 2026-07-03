#!/bin/bash
# Start the externally exposed C++ SideCar.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

PROFILE="${1:-production}"
PORT="${2:-23767}"
BRIDGE_URL="${PYTHON_BRIDGE_URL:-http://127.0.0.1:8199/health}"
case "$PROFILE" in
    production) BINARY="$AR_PROJECT_ROOT/frontend/bridge/coremind_bridge/build-production/coremind_bridge" ;;
    local) BINARY="$AR_PROJECT_ROOT/frontend/bridge/coremind_bridge/build/coremind_bridge" ;;
    -h|--help) echo "Usage: $0 [production|local] [port]"; exit 0 ;;
    *) echo "[ERROR] Profile must be production or local" >&2; exit 2 ;;
esac

ar_load_env
if [ -z "${AISF_INTERNAL_TOKEN:-}" ]; then
    echo "[ERROR] AISF_INTERNAL_TOKEN is required" >&2
    exit 1
fi
if [ ! -x "$BINARY" ]; then
    echo "[ERROR] Missing SideCar binary: $BINARY" >&2
    exit 1
fi

exec "$BINARY" --listen "0.0.0.0:$PORT" --bridge-url "$BRIDGE_URL"
