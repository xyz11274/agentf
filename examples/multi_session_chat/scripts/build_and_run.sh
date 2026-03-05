#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
./build.sh build
./build/example_chat_server \
  --package examples/multi_session_chat/package \
  --web-root examples/multi_session_chat/server/webui \
  --host 127.0.0.1 --http-port 8088 --ws-port 8765
