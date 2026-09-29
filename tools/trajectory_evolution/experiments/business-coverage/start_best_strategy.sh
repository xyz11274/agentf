#!/usr/bin/env bash
# 从已完成的 GEPA 树生成架构师展示；不重启搜索或清理既有产物。
set -euo pipefail

TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$TASK_ROOT"

if [[ $# -ne 0 && ( $# -ne 2 || "$1" != "--job-id" || -z "$2" ) ]]; then
  echo "用法：bash tools/trajectory_evolution/experiments/business-coverage/start_best_strategy.sh [--job-id JOB_ID]" >&2
  exit 2
fi
if tmux has-session -t business-gepa-showcase 2>/dev/null; then
  echo "business-gepa-showcase 已在运行；查看：tmux attach -t business-gepa-showcase" >&2
  exit 1
fi

mkdir -p trace_output/business-simulator
PYTHON_ENTRY='import os, sys; sys.path.insert(0, "tools/trajectory_evolution/experiments/business-coverage"); import offline_gepa; os.environ.update(offline_gepa.environment()); import best_strategy; raise SystemExit(best_strategy.main())'
printf -v QUOTED_ENTRY '%q' "$PYTHON_ENTRY"
TASK_COMMAND="exec env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY -u http_proxy -u https_proxy -u all_proxy -u BUSINESS_BROWSER_PROXY -u AF_LLM_API_KEY -u AF_LLM_BASE_URL -u AF_LLM_MODEL -u OPENAI_API_KEY -u OPENAI_BASE_URL -u OPENAI_MODEL NO_PROXY='*' no_proxy='*' python3 -u -c $QUOTED_ENTRY"
if [[ $# -eq 2 ]]; then
  printf -v QUOTED_JOB '%q' "$2"
  TASK_COMMAND+=" --job-id=$QUOTED_JOB"
fi
TASK_COMMAND+=' > trace_output/business-simulator/best-strategy-launch.log 2>&1'
tmux new-session -d -s business-gepa-showcase -c "$TASK_ROOT" "$TASK_COMMAND"
echo "架构师展示已交给 tmux；查看日志：trace_output/business-simulator/best-strategy-launch.log"
