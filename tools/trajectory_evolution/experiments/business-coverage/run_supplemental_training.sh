#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/../../../.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"
SESSION="business-supplement-20260929"
OUTPUT="$REPO_DIR/trace_output/business-simulator/training-supplement-20260929"
cd "$REPO_DIR"
case "${1:-status}" in
  prepare|--prepare) exec "$PYTHON_BIN" "$SCRIPT_DIR/supplemental_training.py" --prepare ;;
  status|--status) exec "$PYTHON_BIN" "$SCRIPT_DIR/supplemental_training.py" --status ;;
  run|--run)
    "$PYTHON_BIN" "$SCRIPT_DIR/supplemental_training.py" --prepare >/dev/null
    mkdir -p "$OUTPUT"
    exec "$PYTHON_BIN" -u "$SCRIPT_DIR/supplemental_training.py" --run >>"$OUTPUT/run.log" 2>&1 ;;
  start)
    if tmux has-session -t "$SESSION" 2>/dev/null; then
      printf 'Already running in tmux: %s\n' "$SESSION"
    else
      tmux new-session -d -s "$SESSION" -c "$REPO_DIR" bash "$SCRIPT_DIR/run_supplemental_training.sh" run
      printf 'Started in tmux: %s\nLog: %s/run.log\n' "$SESSION" "$OUTPUT"
    fi ;;
  *) printf 'Usage: bash %s {prepare|start|status|run}\n' "$0" >&2; exit 2 ;;
esac
