#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROMPT="$(cat "${ROOT}/prompts/session_summary_prompt.txt")\n$(cat "${ROOT}/runtime/history_tail.jsonl" 2>/dev/null || true)"
python3 "${ROOT}/scripts/llm_openai_compat.py" <<< "$PROMPT" > "${ROOT}/runtime/history_summary.json"
echo "history summarized"
