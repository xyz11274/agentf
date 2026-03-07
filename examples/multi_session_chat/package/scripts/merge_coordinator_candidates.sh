#!/bin/sh
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SESSION_ID="${AF_SESSION_ID:-default}"

python3 "$SCRIPT_DIR/skill_plan_tool.py" merge-coordinator "$SESSION_ID" \
  "${AF_SELECTED_SKILLS:-}" \
  "${AF_PARTIAL_SKILL_RESULTS:-}" \
  "${AF_RESULT_AWARE_CANDIDATES:-}" \
  "${AF_DEPENDENCY_CANDIDATES:-}" \
  "${AF_FINALIZER_CANDIDATES:-}"
