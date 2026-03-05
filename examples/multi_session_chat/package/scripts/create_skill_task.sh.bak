#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SESSION_ID="${AF_SESSION_ID:-precreated_session_1}"
SKILL_JSON="${ROOT}/runtime/${SESSION_ID}.skill.json"
TASK_ID="${SESSION_ID}_skill_task"
echo "{\"task_id\":\"${TASK_ID}\",\"status\":\"created_not_started\",\"skill\":$(cat "$SKILL_JSON")}" > "${ROOT}/runtime/${TASK_ID}.json"
echo "task created not started"
