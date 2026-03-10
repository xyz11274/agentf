#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"

./build.sh build

HTTP_PORT=8088
WS_PORT=8765
for PORT in $HTTP_PORT $WS_PORT; do
  STALE_PID=$(lsof -ti :$PORT 2>/dev/null | head -1 || true)
  if [ -n "$STALE_PID" ]; then
    echo "[build] Stopping stale process on port $PORT (PID $STALE_PID) ..."
    kill "$STALE_PID" 2>/dev/null || true
    sleep 1
  fi
done

./build/example_chat_server \
  --package examples/agent/openjiuwen_multi_session_chat/package \
  --web-root examples/agent/openjiuwen_multi_session_chat/server/webui \
  --host 127.0.0.1 --http-port 8088 --ws-port 8765
