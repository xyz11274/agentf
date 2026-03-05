#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SESSION_ID="${AF_SESSION_ID:-precreated_session_1}"
if [[ "$SESSION_ID" == *"1" ]]; then SKILL="skill_search";
elif [[ "$SESSION_ID" == *"2" ]]; then SKILL="skill_summary";
elif [[ "$SESSION_ID" == *"3" ]]; then SKILL="skill_date";
else SKILL="skill_delegate"; fi
echo "{\"session_id\":\"${SESSION_ID}\",\"skill\":\"${SKILL}\",\"params\":{}}" > "${ROOT}/runtime/${SESSION_ID}.skill.json"
echo "skill selected ${SKILL}"
