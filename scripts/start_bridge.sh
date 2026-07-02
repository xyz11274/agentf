#!/bin/bash
# Start AgentFlow Bridge (FastAPI)
# Usage: ./ar_scripts/start_bridge.sh [port] [--keep-logs]
#
#   --keep-logs   保留旧日志文件，默认每次启动会清理前一次的日志
set -euo pipefail

KEEP_LOGS=false
ARGS=()
for arg in "$@"; do
  if [ "$arg" = "--keep-logs" ]; then
    KEEP_LOGS=true
  else
    ARGS+=("$arg")
  fi
done

PORT="${ARGS[0]:-8199}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Starting AgentFlow Bridge ==="
echo "  Port: $PORT"
echo "  CWD:  $PROJECT_ROOT/frontend/bridge"

cd "$PROJECT_ROOT/frontend/bridge"

# Activate virtual environment if it exists
if [ -d ".venv_unix" ]; then
    source .venv_unix/bin/activate
    echo "[INFO] Virtual environment activated (.venv_unix)"
fi

# Check if bridge dependencies are installed
if ! python3 -c "import fastapi" 2>/dev/null; then
    echo "[INFO] Installing bridge dependencies..."
    python3 -m pip install -r requirements.txt 2>/dev/null || \
        python3 -m pip install fastapi uvicorn httpx websockets 2>&1 | tail -1
fi

# Prepare logs directory
LOG_DIR="$PROJECT_ROOT/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/bridge.log"

# Clean previous logs (unless --keep-logs)
if [ "$KEEP_LOGS" = false ] && [ -f "$LOG_FILE" ]; then
  mv "$LOG_FILE" "${LOG_FILE}.$(date +%Y%m%d-%H%M%S)" 2>/dev/null && \
    echo "[INFO] Archived previous log to ${LOG_FILE}.$(date +%Y%m%d-%H%M%S)"
fi

echo "[INFO] Logging to $LOG_FILE"

exec uvicorn main:app \
    --host 0.0.0.0 \
    --port "$PORT" \
    --reload \
    --log-level info \
    2>&1 | tee -a "$LOG_FILE"
