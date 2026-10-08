#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = '--worker' ]; then
  # Detached workers and their children survive a terminal hangup.
  trap '' HUP
fi
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"
if [ "${1:-}" = '--worker' ]; then
  mkdir -p trace_output/appworld/training
  exec >>trace_output/appworld/training/run.log 2>&1
  trap 'appworld_worker_exit=$?; printf "appworld_controller_exit_code=%s\n" "$appworld_worker_exit" >&2' EXIT
  if [ -n "${TMUX_PANE:-}" ]; then
    tmux set-window-option -t "$TMUX_PANE" remain-on-exit on
  fi
fi
set -a
[ -f examples/agent/business-simulator/.env ] && . examples/agent/business-simulator/.env
set +a
unset HTTP_PROXY HTTPS_PROXY ALL_PROXY http_proxy https_proxy all_proxy BUSINESS_BROWSER_PROXY
export NO_PROXY='*' no_proxy='*' PYTHONDONTWRITEBYTECODE=1 TRAJECTORY_UNCONDITIONAL_RESUME=1
# New seeds use the validated observed-window recipe; frozen artifacts retain their own mode.
export APPWORLD_SEED_L1_WINDOW_MODE="${APPWORLD_SEED_L1_WINDOW_MODE:-observed_phase_v3}"
PYTHON_BIN="${AGENTFLOW_PYTHON:-python3}"
SCRIPT='tools/trajectory_evolution/experiments/appworld/reproduce.py'
SESSION="${APPWORLD_SESSION:-appworld-experiment}"
MODE="${1:---resume}"
case "$MODE" in
  --worker)
    # Load the current .env inside tmux too: its server may hold stale vars.
    case "${2:-}" in
      --all|--gepa-run|--publish|--deliver-best-available) "$PYTHON_BIN" -B -u "$SCRIPT" "$2" "${@:3}" ;;
      *) exit 2 ;;
    esac
    ;;
  --status|--plan)
    shift
    exec "$PYTHON_BIN" -B "$SCRIPT" "$MODE" "$@"
    ;;
  --fresh|--resume|--gepa|--publish|--deliver-best-available)
    if [ "$#" -gt 0 ]; then shift; fi
    APPWORLD_TMUX_ACTION='new-session'
    if tmux has-session -t "$SESSION" 2>/dev/null; then
      if [ "$(tmux list-panes -s -t "$SESSION" -F '#{pane_dead}')" != '1' ]; then
        echo "实验仍在 tmux 会话 $SESSION 中运行；查看进度请执行本命令 --status。"
        exit 0
      fi
      # A single retained terminal pane can be resumed without deleting a
      # session. respawn-pane refuses a live pane unless -k is supplied; we
      # never supply it, so a racing live writer remains protected.
      APPWORLD_TMUX_ACTION='respawn-pane'
    fi
    if [ "$MODE" = '--fresh' ]; then
      # Caller explicitly selects a new run. Generated results are rebuilt.
      "$PYTHON_BIN" -B "$SCRIPT" --assert-idle-for-fresh
      rm -rf -- "$ROOT/trace_output/appworld"
    fi
    case "$MODE" in
      --fresh|--resume) ACTION='--all' ;;
      --gepa) ACTION='--gepa-run' ;;
      --publish) ACTION='--publish' ;;
      --deliver-best-available) ACTION='--deliver-best-available' ;;
    esac
    mkdir -p trace_output/appworld/{training,gepa,published}
    # Multiple command arguments let tmux exec Bash directly, bypassing its
    # configured default shell and preserving every argument without shell
    # interpolation. The worker records its real exit status and retains the
    # pane after completion, including a failed Python startup.
    if [ "$APPWORLD_TMUX_ACTION" = 'respawn-pane' ]; then
      TMUX_TARGET=(-t "$SESSION")
    else
      TMUX_TARGET=(-d -s "$SESSION")
    fi
    tmux "$APPWORLD_TMUX_ACTION" "${TMUX_TARGET[@]}" env AGENTFLOW_PYTHON="$PYTHON_BIN" \
      "$BASH" --noprofile --norc "$ROOT/tools/trajectory_evolution/experiments/appworld/reproduce.sh" \
      --worker "$ACTION" "$@"
    echo "已创建 tmux 会话 $SESSION；流程配置16路，实际进度和退出码见 trace_output/appworld/training/run.log。"
    ;;
  *)
    echo "用法: $0 [--fresh|--resume|--gepa|--publish|--deliver-best-available|--status|--plan]" >&2
    exit 2
    ;;
esac
