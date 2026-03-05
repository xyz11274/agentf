#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROMPT="$(cat "${ROOT}/prompts/coordinator_plan_prompt.txt")\n$(cat "${ROOT}/runtime/selected_skills.jsonl" 2>/dev/null || true)"
python3 "${ROOT}/scripts/llm_openai_compat.py" <<< "$PROMPT" > "${ROOT}/runtime/task_plan.json"
echo "dependency plan generated"
