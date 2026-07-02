#!/bin/bash
# Start AgentFlow Web Frontend (Vue 3 + Vite)
# Usage: ./ar_scripts/start_web.sh [port]
set -euo pipefail

PORT="${1:-15173}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Starting AgentFlow Web Frontend ==="
echo "  Port: $PORT"
echo "  CWD:  $PROJECT_ROOT/web"

cd "$PROJECT_ROOT/frontend/web"

# Check node_modules
if [ ! -d node_modules ]; then
    echo "[INFO] Installing web dependencies..."
    npm install 2>&1 | tail -3
fi

exec npx vite --port "$PORT" --host 2>&1
