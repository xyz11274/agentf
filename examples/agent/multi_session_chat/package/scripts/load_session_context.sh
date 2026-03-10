#!/bin/sh
set -eu
. "$(cd "$(dirname "$0")" && pwd)/../../../../publish/sdk/shell/agentflow_databus.sh"

SESSION_ID="${AF_SESSION_ID:-default}"
USER_TEXT="${AF_USER_TEXT:-}"
CONTEXT="$(af_databus_get "session:${SESSION_ID}:context" 2>/dev/null || true)"

if [ -z "$CONTEXT" ]; then
  HISTORY_JSON="$(af_databus_get "session:${SESSION_ID}:history_json" 2>/dev/null || true)"
  CONTEXT="$(
    python3 - "$SESSION_ID" "$USER_TEXT" "$HISTORY_JSON" <<'PY'
import json
import sys

print(json.dumps({
    "session_id": sys.argv[1],
    "user_text": sys.argv[2],
    "history_json": sys.argv[3],
    "context_source": "bridge_fallback",
}, ensure_ascii=False))
PY
  )"
fi

python3 - "$CONTEXT" <<'PY'
import json
import sys

print(json.dumps({"session_context": sys.argv[1]}, ensure_ascii=False))
PY
