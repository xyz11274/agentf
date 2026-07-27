#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────
# 性能测试启动脚本
#
# 三层测试策略:
#   Layer 1 — Mock LLM，测试 Bridge + Daemon 调度能力（无需外部依赖）
#   Layer 2 — 真实 LLM + 简短问候，测试全链路基本性能
#   Layer 3 — 真实 LLM + 真实业务场景（outfit-assistant skill 调度）
#
# 用法:
#   ./scripts/tests/run_perf.sh              # 默认 Layer 1（MOCK）
#   ./scripts/tests/run_perf.sh layer1       # Mock LLM 调度能力
#   ./scripts/tests/run_perf.sh layer2       # 真实 LLM 简短问答
#   ./scripts/tests/run_perf.sh layer3       # 真实业务场景
#   ./scripts/tests/run_perf.sh --help       # 完整帮助
# ──────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PERF_SCRIPT="$PROJECT_ROOT/tests/perf/run_concurrent_perf.py"

# ── 颜色 ─────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ── 加载测试环境配置（含 LLM API Key） ───────────────────────────────

_ENV_FILE="$PROJECT_ROOT/configs/test/bridge.env"
if [[ -f "$_ENV_FILE" ]]; then
    set -a
    source "$_ENV_FILE"
    set +a
    echo -e "${GREEN}[config] sourced $_ENV_FILE${NC}"
else
    echo -e "${YELLOW}[config] $_ENV_FILE 不存在，LLM 配置需手动设置${NC}"
fi

# ── 帮助 ─────────────────────────────────────────────────────────────

usage() {
    cat <<EOF
性能测试启动脚本 — 三层测试策略

用法:
  $(basename "$0") [layer1|layer2|layer3] [--levels N,N,...]

层级说明:
  layer1   Mock LLM，测试调度能力        默认级别: 1,3,5,10,20
  layer2   真实 LLM + 简短问答           默认级别: 1,3,5
  layer3   真实 LLM + 真实业务场景         默认级别: 1,3

选项:
  --levels N,N,...   指定并发级别（覆盖默认）
  --help             显示此帮助

Layer 2/3 需要设置以下环境变量:
  export AF_LLM_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1
  export AF_LLM_API_KEY=sk-your-key
  export AF_LLM_MODEL=qwen-vl-max

示例:
  $(basename "$0") layer1                         # 跑全部 5 级调度测试
  $(basename "$0") layer2 --levels 1,3            # 真实 LLM 仅 1+3 并发
  $(basename "$0") layer3                          # 真实业务 1+3 并发
EOF
    exit 0
}

# ── 环境检查 ─────────────────────────────────────────────────────────

check_env() {
    if [[ -z "${AF_LLM_BASE_URL:-}" || -z "${AF_LLM_API_KEY:-}" ]]; then
        echo -e "${RED}[ERROR] Layer $1 需要真实 LLM 配置${NC}"
        echo ""
        echo "请设置以下环境变量:"
        echo "  export AF_LLM_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1"
        echo "  export AF_LLM_API_KEY=sk-你的Key"
        echo "  export AF_LLM_MODEL=qwen-vl-max"
        echo ""
        echo "或者先跑 Layer 1（Mock LLM，无需外部依赖）:"
        echo "  $(basename "$0") layer1"
        exit 1
    fi
}

# ── 主逻辑 ───────────────────────────────────────────────────────────

LAYER="${1:-layer1}"
shift 1 2>/dev/null || true

# 解析 --levels
EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --levels)
            EXTRA_ARGS+=("--levels" "$2")
            shift 2
            ;;
        --agent)
            EXTRA_ARGS+=("--agent" "$2")
            shift 2
            ;;
        --runs)
            EXTRA_ARGS+=("--runs" "$2")
            shift 2
            ;;
        --no-warmup)
            EXTRA_ARGS+=("--no-warmup")
            shift 1
            ;;
        --real)
            EXTRA_ARGS+=("--real")
            shift 1
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo -e "${RED}未知参数: $1${NC}"
            usage
            ;;
    esac
done

# 检查 daemon 是否已构建
if [[ ! -f "$PROJECT_ROOT/build/agentflowd" ]]; then
    echo -e "${RED}[ERROR] daemon 未构建: build/agentflowd 不存在${NC}"
    echo "请先执行 cmake + make 构建"
    exit 1
fi

echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║         AgentFlow 并发性能测试                          ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

cd "$PROJECT_ROOT"

case "$LAYER" in
    layer1)
        echo -e "${GREEN}[Layer 1] Mock LLM — 测试调度能力${NC}"
        echo -e "${GREEN}  无需外部 LLM 配置，自动使用 scripted mock server${NC}"
        echo ""
        _cmd_args=("${EXTRA_ARGS[@]:---levels 1,3,5,10,20}")
        uv run python "$PERF_SCRIPT" --agent agent-connectivity "${_cmd_args[@]}"
        ;;

    layer2)
        echo -e "${GREEN}[Layer 2] 真实 LLM + 简短问答 — 全链路基本性能${NC}"
        echo -e "${GREEN}  问题: 用三个字以内回答：你好${NC}"
        echo -e "${GREEN}  LLM: ${AF_LLM_MODEL:-未设置}${NC}"
        echo ""
        check_env 2
        if [[ ${#EXTRA_ARGS[@]} -eq 0 ]]; then
            _cmd_args=(--levels 1,3,5)
        else
            _cmd_args=("${EXTRA_ARGS[@]}")
        fi
        uv run python "$PERF_SCRIPT" \
            --real --agent agent-connectivity \
            --prompt "用三个字以内回答：你好" \
            "${_cmd_args[@]}"
        ;;

    layer3)
        echo -e "${GREEN}[Layer 3] 真实 LLM + 真实业务场景${NC}"
        echo -e "${GREEN}  Agent: outfit-assistant（含 skill 调度）${NC}"
        echo -e "${GREEN}  LLM: ${AF_LLM_MODEL:-未设置}${NC}"
        echo -e "${YELLOW}  提示: 此类测试耗时较长（单次请求 30-120s），建议用 --no-warmup --runs 1 缩减时间${NC}"
        echo ""
        check_env 3
        if [[ ${#EXTRA_ARGS[@]} -eq 0 ]]; then
            _cmd_args=(--levels 1,3)
        else
            _cmd_args=("${EXTRA_ARGS[@]}")
        fi
        uv run python "$PERF_SCRIPT" \
            --real --agent outfit-assistant \
            --prompt "今天北京穿什么？给我效果图" \
            "${_cmd_args[@]}"
        ;;

    *)
        echo -e "${RED}未知层级: $LAYER${NC}"
        usage
        ;;
esac
