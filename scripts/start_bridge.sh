#!/bin/bash
# Start the Python Bridge in development or internal production mode.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

MODE="dev"
PORT="8199"
if [ "${1:-}" = "dev" ] || [ "${1:-}" = "production" ]; then
    MODE="$1"
    PORT="${2:-8199}"
elif [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    echo "Usage: $0 [dev|production] [port]"
    exit 0
elif [ -n "${1:-}" ]; then
    PORT="$1"
fi
case "$MODE" in
    dev)
        HOST="0.0.0.0"
        RELOAD=(--reload --reload-dir "$AR_PROJECT_ROOT/frontend/bridge")
        ;;
    production) HOST="127.0.0.1"; RELOAD=() ;;
    *) echo "[ERROR] Mode must be dev or production" >&2; exit 2 ;;
esac

ar_load_env
if [ "$MODE" = "production" ] && [ -z "${AISF_INTERNAL_TOKEN:-}" ]; then
    echo "[ERROR] AISF_INTERNAL_TOKEN is required in production" >&2
    exit 1
fi
ar_require_command python3
if ! PYTHONPATH="$AR_PROJECT_ROOT/frontend/bridge" python3 -c "import fastapi, uvicorn" 2>/dev/null; then
    echo "[ERROR] Bridge dependencies are missing; install frontend/bridge/requirements.txt" >&2
    exit 1
fi

export BRIDGE_HOST="$HOST" BRIDGE_PORT="$PORT"
exec python3 -m uvicorn main:app \
    --app-dir "$AR_PROJECT_ROOT/frontend/bridge" \
    --host "$HOST" --port "$PORT" \
    "${RELOAD[@]}" --log-level info
