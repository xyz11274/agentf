#!/bin/bash
# Start AgentFlow Daemon
# Usage: ./ar_scripts/start_daemon.sh [agent_package] [port]
set -euo pipefail

AGENT_PACKAGE="${1:-examples/agent/agentflow_assistant}"
PORT="${2:-17891}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Starting AgentFlow Daemon ==="
echo "  Package: $AGENT_PACKAGE"
echo "  Port:    $PORT"
echo "  PID:     $$"

cd "$PROJECT_ROOT"

# Prepare logs directory
LOG_DIR="$PROJECT_ROOT/logs"
mkdir -p "$LOG_DIR"

echo "[INFO] Logging to $LOG_DIR/agentflowd.log"

./build/agentflowd \
    --package "$(pwd)/$AGENT_PACKAGE" \
    --listen "127.0.0.1:$PORT" \
    --log-file "$LOG_DIR/agentflowd.log" \
    2>&1
