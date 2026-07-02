#!/bin/bash
# Start AgentFlow Daemon
# Usage: ./ar_scripts/start_daemon.sh [agent_package] [port] [--keep-logs]
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

AGENT_PACKAGE="${ARGS[0]:-examples/agent/agentflow_assistant}"
PORT="${ARGS[1]:-17891}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Starting AgentFlow Daemon ==="
echo "  Package: $AGENT_PACKAGE"
echo "  Port:    $PORT"

cd "$PROJECT_ROOT"

# Auto-build all targets before starting
echo "[INFO] Building all targets..."
bash "$SCRIPT_DIR/build.sh" 2>&1

# Prepare logs directory
LOG_DIR="$PROJECT_ROOT/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/agentflowd.log"

# Clean previous logs (unless --keep-logs)
if [ "$KEEP_LOGS" = false ] && [ -f "$LOG_FILE" ]; then
  mv "$LOG_FILE" "${LOG_FILE}.$(date +%Y%m%d-%H%M%S)" 2>/dev/null && \
    echo "[INFO] Archived previous log to ${LOG_FILE}.$(date +%Y%m%d-%H%M%S)"
fi

echo "[INFO] Logging to $LOG_FILE"

./build/agentflowd \
    --package "$(pwd)/$AGENT_PACKAGE" \
    --listen "127.0.0.1:$PORT" \
    --log-file "$LOG_FILE" \
    2>&1
