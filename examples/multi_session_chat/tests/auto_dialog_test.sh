#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

./build.sh build >/dev/null

LOG_DIR="examples/multi_session_chat/package/history"
mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/webui_default.jsonl

./build/example_chat_server \
  --package examples/multi_session_chat/package \
  --web-root examples/multi_session_chat/server/webui \
  --history-dir "$LOG_DIR" \
  --host 127.0.0.1 --http-port 18088 --ws-port 18765 >/tmp/multichat_server.log 2>&1 &
SERVER_PID=$!
cleanup(){ kill "$SERVER_PID" >/dev/null 2>&1 || true; }
trap cleanup EXIT
sleep 1

curl --noproxy "*" -sf "http://127.0.0.1:18088/" >/tmp/multichat_index.html
curl --noproxy "*" -sf -X POST "http://127.0.0.1:18088/api/chat" \
  -H 'Content-Type: application/json' \
  -d '{"session_id":"webui:default","text":"hello","workflow_id":"default-dispatcher-workflow"}' \
  >/tmp/multichat_chat.json
curl --noproxy "*" -sf -X POST "http://127.0.0.1:18088/api/chat" \
  -H 'Content-Type: application/json' \
  -d '{"session_id":"webui:default","text":"请帮我总结一下上条消息","workflow_id":"default-dispatcher-workflow"}' \
  >/tmp/multichat_chat2.json
curl --noproxy "*" -sf "http://127.0.0.1:18088/api/history?session_id=webui%3Adefault&limit=20" \
  >/tmp/multichat_history.json

if ! grep -q "session_id" /tmp/multichat_chat.json; then
  echo "chat API failed"
  exit 1
fi

if grep -q "\\[ok\\] processed" /tmp/multichat_chat.json; then
  echo "chat API returned placeholder response"
  exit 1
fi

if ! grep -q '"text":"' /tmp/multichat_chat2.json; then
  echo "second round chat failed"
  exit 1
fi

if ! grep -q '"role":"assistant"' /tmp/multichat_history.json; then
  echo "history API missing assistant messages"
  exit 1
fi

if [ ! -f "$LOG_DIR"/webui_default.jsonl ]; then
  echo "history file not created"
  exit 1
fi

if ! grep -q '"role":"user"' "$LOG_DIR"/webui_default.jsonl; then
  echo "user message not persisted"
  exit 1
fi

if ! grep -q '"role":"assistant"' "$LOG_DIR"/webui_default.jsonl; then
  echo "assistant message not persisted"
  exit 1
fi

echo "auto dialog test passed"
