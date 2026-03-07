#!/bin/sh
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SESSION_ID="${AF_SESSION_ID:-default}"

python3 "$SCRIPT_DIR/skill_plan_tool.py" merge-dispatcher "$SESSION_ID" \
  "${AF_SEMANTIC_CANDIDATES:-}" \
  "${AF_TEMPORAL_CANDIDATES:-}" \
  "${AF_PLANNER_CANDIDATES:-}"
