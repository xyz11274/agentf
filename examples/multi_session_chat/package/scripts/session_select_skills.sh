#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
msg="${AF_INPUT:-$(cat "${ROOT}/runtime/latest_user_input.txt" 2>/dev/null || true)}"
for sid in precreated_session_1 precreated_session_2 precreated_session_3 precreated_session_4; do
  AF_SESSION_ID="$sid" AF_INPUT="$msg" "${ROOT}/scripts/select_skill.sh" >/dev/null
  AF_SESSION_ID="$sid" "${ROOT}/scripts/create_skill_task.sh" >/dev/null
done
echo "session skills selected"
