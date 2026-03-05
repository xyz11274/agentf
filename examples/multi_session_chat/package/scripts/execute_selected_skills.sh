#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROOT}/runtime/skill_results.jsonl"
: > "$OUT"
USER_TEXT="$(cat "${ROOT}/runtime/latest_user_input.txt" 2>/dev/null || true)"
for sid in precreated_session_1 precreated_session_2 precreated_session_3 precreated_session_4; do
  f="${ROOT}/runtime/${sid}.skill.json"
  [ -f "$f" ] || continue
  skill="$(sed -n 's/.*"skill"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$f" | head -n1)"
  case "$skill" in
    skill_search) cmd="${ROOT}/scripts/skill_search.sh" ;;
    skill_summary) cmd="${ROOT}/scripts/skill_summary.sh" ;;
    skill_date) cmd="${ROOT}/scripts/skill_date.sh" ;;
    skill_delegate) cmd="${ROOT}/scripts/skill_delegate.sh" ;;
    *) cmd="" ;;
  esac
  [ -n "$cmd" ] || continue
  result="$(AF_USER_TEXT="$USER_TEXT" AF_SESSION_ID="$sid" bash "$cmd")"
  printf '{"session_id":"%s","skill":"%s","result":"%s"}\n' "$sid" "$skill" "${result//\"/\\\"}" >> "$OUT"
done
echo "skills executed"
