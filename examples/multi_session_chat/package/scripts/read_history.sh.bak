#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SESSION_ID="${AF_SESSION_ID:-precreated_session_1}"
HFILE="${ROOT}/history/${SESSION_ID}.jsonl"
if [ ! -f "$HFILE" ]; then
  echo '{"role":"system","content":"empty history"}' > "$HFILE"
fi
tail -n 30 "$HFILE" > "${ROOT}/runtime/history_tail.jsonl"
echo "history loaded"
