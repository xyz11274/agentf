#!/bin/sh
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/../../../../publish/sdk/shell/agentflow_databus.sh"

PHASE="${1:-dispatcher}"
PATH_NAME="${2:-semantic}"
SESSION_ID="${AF_SESSION_ID:-default}"
USER_TEXT="${AF_USER_TEXT:-$(af_databus_get user_text 2>/dev/null || true)}"
SESSION_CONTEXT="${AF_SESSION_CONTEXT:-$(af_databus_get "session:${SESSION_ID}:context" 2>/dev/null || true)}"
SKILL_CATALOG="${AF_SKILL_CATALOG:-$(af_databus_get skill_catalog 2>/dev/null || true)}"
SELECTED_SKILLS="${AF_SELECTED_SKILLS:-$(af_databus_get selected_skills 2>/dev/null || true)}"
PARTIAL_RESULTS="${AF_PARTIAL_SKILL_RESULTS:-$(af_databus_get partial_skill_results 2>/dev/null || true)}"
REQUEST_ID="${PHASE}_${PATH_NAME}_$(date +%s%N)"

PROMPT="$(
  python3 - "$PHASE" "$PATH_NAME" "$USER_TEXT" "$SESSION_CONTEXT" "$SKILL_CATALOG" "$SELECTED_SKILLS" "$PARTIAL_RESULTS" <<'PY'
import json
import sys

phase, path_name, user_text, session_context, skill_catalog, selected, partial = sys.argv[1:8]
prompt = {
    "phase": phase,
    "path": path_name,
    "task": "从技能清单中选择0~N个可完成请求的技能，并给出优先级和状态。",
    "user_text": user_text,
    "session_context": session_context,
    "skill_catalog": skill_catalog,
    "selected_skills": selected,
    "partial_skill_results": partial,
    "response_format": {
        "skills": [{
            "skill": "skill_search",
            "priority": 80,
            "status": "start",
            "reason": "...",
        }]
    },
}
print(json.dumps(prompt, ensure_ascii=False))
PY
)"

af_databus_request_llm "$REQUEST_ID" "$PROMPT" "agentflow.llm.chunk.$REQUEST_ID"
RAW="$(af_databus_next_event "${PHASE}-${PATH_NAME}-${REQUEST_ID}" "agentflow.llm.done.$REQUEST_ID" 2500 || true)"

if [ "$PHASE" = "dispatcher" ]; then
  python3 "$SCRIPT_DIR/skill_plan_tool.py" dispatcher-path "$SESSION_ID" "$USER_TEXT" "$PATH_NAME" "$RAW"
else
  python3 "$SCRIPT_DIR/skill_plan_tool.py" coordinator-path "$SESSION_ID" "$PATH_NAME" "$SELECTED_SKILLS" "$PARTIAL_RESULTS" "$RAW"
fi
