#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SESSION_ID="${AF_SESSION_ID:-precreated_session_1}"
USER_TEXT="$(printf '%s\n' "${AF_INPUT:-}" | sed -n 's/^user=//p' | head -n1)"
[ -z "$USER_TEXT" ] && USER_TEXT="${AF_INPUT:-hello}"
if [[ "$SESSION_ID" == *"1" ]]; then FALLBACK="skill_search";
elif [[ "$SESSION_ID" == *"2" ]]; then FALLBACK="skill_summary";
elif [[ "$SESSION_ID" == *"3" ]]; then FALLBACK="skill_date";
else FALLBACK="skill_delegate"; fi
PROMPT="You are selecting a skill for session ${SESSION_ID}.\nAllowed skills: skill_search, skill_summary, skill_date, skill_delegate.\nUser request: ${USER_TEXT}\nReturn JSON: {\"skill\":\"...\",\"params\":{}}"
RAW="$(python3 "${ROOT}/scripts/llm_openai_compat.py" <<< "$PROMPT" 2>/dev/null || true)"
TEXT="$(printf '%s' "$RAW" | sed -n 's/.*"text"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
SKILL="$FALLBACK"
for s in skill_search skill_summary skill_date skill_delegate; do
  if printf '%s' "$TEXT" | grep -q "$s"; then SKILL="$s"; break; fi
done
echo "{\"session_id\":\"${SESSION_ID}\",\"skill\":\"${SKILL}\",\"params\":{\"query\":\"${USER_TEXT//\"/}\"}}" > "${ROOT}/runtime/${SESSION_ID}.skill.json"
echo "skill selected ${SKILL}"
