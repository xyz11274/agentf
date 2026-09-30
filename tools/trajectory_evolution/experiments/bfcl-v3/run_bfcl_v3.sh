#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../" && pwd)"
ENTRY="$ROOT/tools/trajectory_evolution/experiments/bfcl-v3/bfcl_experiment.py"
SESSION="bfcl-v3"
MODE="${1:-resume}"
INTERPRETER="python3"
LOG_REDIRECT=""

case "$MODE" in
  seen-start|seen-resume)
    export BFCL_SPLIT_ID="seen-scenarios-v1"
    SESSION="bfcl-v3-seen"
    FLAG="--start"
    INTERPRETER="/root/miniconda3/envs/agentflow-dev/bin/python3"
    LOG_REDIRECT=" >> '$ROOT/trace_output/bfcl-v3/seen-scenarios-v1-runner.log' 2>&1"
    ;;
  seen-status|seen-report)
    export BFCL_SPLIT_ID="seen-scenarios-v1"
    exec python3 "$ROOT/tools/trajectory_evolution/experiments/bfcl-v3/bfcl_progress_report.py"
    ;;
  seen-check)
    export BFCL_SPLIT_ID="seen-scenarios-v1"
    exec /root/miniconda3/envs/agentflow-dev/bin/python3 "$ENTRY" --check
    ;;
  fresh)  FLAG="--fresh" ;;
  resume) FLAG="--resume" ;;
  status|report) exec python3 "$ROOT/tools/trajectory_evolution/experiments/bfcl-v3/bfcl_progress_report.py" ;;
  check)  exec python3 "$ENTRY" --check ;;
  *) echo "用法: $0 {seen-start|seen-resume|seen-status|seen-check|fresh|resume|status|report|check}" >&2; exit 2 ;;
esac

if tmux has-session -t "$SESSION" 2>/dev/null; then
  echo "BFCL 会话已存在: $SESSION"
  exit 0
fi
# tmux servers retain their own environment.  Source the current private
# business .env inside the new shell so a provider/model switch cannot be
# silently masked by stale variables inherited by an older tmux server.
tmux new-session -d -s "$SESSION" "export BFCL_SPLIT_ID='${BFCL_SPLIT_ID:-}'; cd '$ROOT' && set -a && . '$ROOT/examples/agent/business-simulator/.env' && set +a && unset HTTP_PROXY HTTPS_PROXY ALL_PROXY http_proxy https_proxy all_proxy BUSINESS_BROWSER_PROXY && export NO_PROXY='*' no_proxy='*' && exec '$INTERPRETER' '$ENTRY' '$FLAG'$LOG_REDIRECT"
echo "已在 tmux 会话 $SESSION 启动 BFCL v3 全流程；查看: tmux attach -t $SESSION"
