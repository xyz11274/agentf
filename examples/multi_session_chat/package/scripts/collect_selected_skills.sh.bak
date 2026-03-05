#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROOT}/runtime/selected_skills.jsonl"
: > "$OUT"
for sid in precreated_session_1 precreated_session_2 precreated_session_3 precreated_session_4; do
  f="${ROOT}/runtime/${sid}.skill.json"
  [ -f "$f" ] && cat "$f" >> "$OUT"
done
echo "skills collected"
