#!/usr/bin/env bash
#
# trace.sh — AgentFlow trace_id 日志查询工具
#
# 用法:
#   ./trace.sh <trace_id>             从默认日志查询 trace
#   ./trace.sh <trace_id> <logfile>   从指定文件查询
#   ./trace.sh -l                     列出最近的 trace_id
#
# 示例:
#   ./trace.sh trc_1524029646_1
#   ./trace.sh trc_1524029646_1 /tmp/agentflowd_test.log
#   ./trace.sh -l | head -20

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# 默认日志路径探测
DEFAULT_LOG="$PROJECT_ROOT/logs/agentflowd.log"
if [ ! -f "$DEFAULT_LOG" ]; then
  # 尝试 tmp 路径（测试环境）
  for f in /tmp/agentflowd_test.log /tmp/agentflowd_new.log /tmp/agentflowd.log; do
    if [ -f "$f" ]; then
      DEFAULT_LOG="$f"
      break
    fi
  done
fi

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

list_traces() {
  local logfile="${1:-$DEFAULT_LOG}"
  if [ ! -f "$logfile" ]; then
    echo "错误: 日志文件不存在: $logfile" >&2
    exit 1
  fi
  echo -e "${BOLD}最近的 trace_id（${logfile}）:${NC}"
  grep -oE 'trace=(trc_[0-9]+_[0-9]+)' "$logfile" | sort -u -t_ -k2,2rn -k3,3rn | head -20 | while read -r tid; do
    first_seen=$(grep "trace=$tid" "$logfile" | head -1 | grep -oE '^[^ ]+ [^ ]+' || echo "?")
    echo "  $tid  (首次出现: $first_seen)"
  done
}

query_trace() {
  local trace_id="$1"
  local logfile="${2:-$DEFAULT_LOG}"

  if [ ! -f "$logfile" ]; then
    echo "错误: 日志文件不存在: $logfile" >&2
    exit 1
  fi

  # 收集该 trace_id 的所有日志行
  mapfile -t lines < <(grep "trace=${trace_id}" "$logfile" || grep "$trace_id" "$logfile" || true)

  if [ ${#lines[@]} -eq 0 ]; then
    echo -e "${RED}未找到 trace_id=${trace_id}${NC}"
    echo "日志文件: $logfile"
    exit 1
  fi

  # 提取时间范围
  first_ts=$(echo "${lines[0]}" | grep -oE '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}' || echo "?")
  last_ts=$(echo "${lines[-1]}" | grep -oE '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}' || echo "?")
  total_ms=0
  if [ "$first_ts" != "?" ] && [ "$last_ts" != "?" ]; then
    f=$(date -j -f "%Y-%m-%d %H:%M:%S" "$first_ts" +%s 2>/dev/null || echo 0)
    l=$(date -j -f "%Y-%m-%d %H:%M:%S" "$last_ts" +%s 2>/dev/null || echo 0)
    total_ms=$(( (l - f) * 1000 ))
  fi

  echo ""
  echo -e "${BOLD}━━━ AgentFlow Trace: ${CYAN}${trace_id}${NC}${BOLD} ━━━${NC}"
  echo -e "  日志文件: ${BLUE}${logfile}${NC}"
  echo -e "  时间范围: ${first_ts} → ${last_ts}  (${YELLOW}~${total_ms}ms${NC})"
  echo ""

  # 分阶段处理
  declare -A STAGE_NAMES=(
    ["USER_INPUT"]="① 用户输入"
    ["LAUNCH_SKILL"]="② 技能启动"
    ["PROCESS_NODE_START"]="  节点启动"
    ["PROCESS_NODE_SUCCESS"]="  节点完成"
    ["WORKFLOW_COMPLETE"]="  工作流完成"
    ["HANDLE_DATA_EXCHANGE"]="③ 消息交换"
    ["HANDLE_INTENT_REQUEST"]="  意图处理"
    ["BUILD_RUNTIME_TASK"]="  运行时任务"
    ["COORDINATOR_SEND"]="④ 最终响应"
    ["SKILL_COMPLETION"]="  技能完成"
    ["E2E_SUMMARY"]="⑤ 摘要"
    ["CLEANUP_DIALOGUE"]="⑥ 清理"
  )

  prev_stage=""
  for line in "${lines[@]}"; do
    ts=$(echo "$line" | grep -oE '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+' || echo "")
    ts_short=$(echo "$ts" | grep -oE '[0-9]{2}:[0-9]{2}:[0-9]{2}' || echo "")
    tag=$(echo "$line" | sed -nE 's/.*\[([A-Z_]+)\].*/\1/p' | head -1 || echo "")
    content=$(echo "$line" | sed -E 's/^\[[^\]]+\] \[[^\]]+\] \[[^\]]+\] //' 2>/dev/null || echo "$line")

    stage_prefix="${STAGE_NAMES[$tag]:-   }"
    if [ "$tag" != "$prev_stage" ] && [ -n "$stage_prefix" ]; then
      echo -e "${BOLD}${stage_prefix}${NC}"
      prev_stage="$tag"
    fi

    # 截断过长行
    if [ ${#content} -gt 160 ]; then
      content="${content:0:157}..."
    fi
    echo -e "  ${ts_short:+$ts_short }${content}"
  done

  echo ""
  echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo -e "  共 ${YELLOW}${#lines[@]}${NC} 条日志, 耗时 ${YELLOW}~${total_ms}ms${NC}"
  echo ""
}

# --- Main ---

if [ $# -eq 0 ]; then
  echo "用法:"
  echo "  $0 <trace_id> [logfile]   查询指定 trace_id 的日志"
  echo "  $0 -l [logfile]           列出最近的 trace_id"
  echo ""
  echo "默认日志路径: $DEFAULT_LOG"
  exit 0
fi

if [ "$1" = "-l" ]; then
  list_traces "${2:-$DEFAULT_LOG}"
else
  query_trace "$1" "${2:-$DEFAULT_LOG}"
fi
