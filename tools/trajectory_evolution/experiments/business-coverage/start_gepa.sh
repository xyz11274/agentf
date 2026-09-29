#!/usr/bin/env bash
# 仅重建 GEPA；保留 training、数据集和权威记忆。
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$TASK_ROOT"
if [[ "${1:-}" != "--fresh" ]]; then
  echo "开始新 GEPA：bash tools/trajectory_evolution/experiments/business-coverage/start_gepa.sh --fresh"
  echo "查看运行：tmux attach -t business-gepa"
  exit 0
fi
if [[ $# -ne 1 && ( $# -ne 3 || "$2" != "--baseline-report" || ! -f "$3" ) ]]; then
  echo "用法：bash tools/trajectory_evolution/experiments/business-coverage/start_gepa.sh --fresh [--baseline-report 已完成的L0报告]" >&2
  exit 2
fi
if tmux has-session -t business-gepa 2>/dev/null; then
  echo "business-gepa 已在运行，请先查看；本次未清理任何节点。"
  exit 1
fi
mkdir -p trace_output/business-simulator
TASK_COMMAND="exec env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY -u http_proxy -u https_proxy -u all_proxy -u BUSINESS_BROWSER_PROXY -u AF_LLM_API_KEY -u AF_LLM_BASE_URL -u AF_LLM_MODEL -u OPENAI_API_KEY -u OPENAI_BASE_URL -u OPENAI_MODEL NO_PROXY='*' no_proxy='*' python3 -u tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --fresh"
if [[ $# -eq 3 ]]; then
  printf -v QUOTED_BASELINE '%q' "$3"
  TASK_COMMAND+=" --baseline-report=$QUOTED_BASELINE"
fi
tmux new-session -d -s business-gepa -c "$TASK_ROOT" "$TASK_COMMAND > trace_output/business-simulator/gepa-launch.log 2>&1"
echo "新 GEPA 已交给 tmux；启动与预检进度见 trace_output/business-simulator/gepa-launch.log。"
