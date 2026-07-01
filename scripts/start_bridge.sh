#!/bin/bash
# Start AgentFlow Bridge (FastAPI)
# Usage: ./ar_scripts/start_bridge.sh [port]
set -euo pipefail

PORT="${1:-8199}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Starting AgentFlow Bridge ==="
echo "  Port: $PORT"
echo "  CWD:  $PROJECT_ROOT/bridge"

cd "$PROJECT_ROOT/bridge"

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

echo "[INFO] Logging to $LOG_DIR/bridge.log"

exec uvicorn main:app \
    --host 0.0.0.0 \
    --port "$PORT" \
    --reload \
    --log-level info \
    2>&1 | tee -a "$LOG_DIR/bridge.log"
