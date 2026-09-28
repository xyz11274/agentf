#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../" && pwd)"
ENTRY="$ROOT/tools/trajectory_evolution/experiments/bfcl-v3/bfcl_experiment.py"
SESSION="bfcl-v3"
MODE="${1:-resume}"

case "$MODE" in
  fresh)  FLAG="--fresh" ;;
  resume) FLAG="--resume" ;;
  status|report) exec python3 "$ROOT/tools/trajectory_evolution/experiments/bfcl-v3/bfcl_progress_report.py" ;;
  check)  exec python3 "$ENTRY" --check ;;
  *) echo "用法: $0 {fresh|resume|status|report|check}" >&2; exit 2 ;;
esac

if tmux has-session -t "$SESSION" 2>/dev/null; then
  echo "BFCL 会话已存在: $SESSION"
  exit 0
fi
# tmux servers retain their own environment.  Source the current private
# business .env inside the new shell so a provider/model switch cannot be
# silently masked by stale variables inherited by an older tmux server.
tmux new-session -d -s "$SESSION" "cd '$ROOT' && set -a && . '$ROOT/examples/agent/business-simulator/.env' && set +a && exec python3 '$ENTRY' '$FLAG'"
echo "已在 tmux 会话 $SESSION 启动 BFCL v3 全流程；查看: tmux attach -t $SESSION"
