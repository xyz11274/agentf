#!/bin/bash
# ============================================================================
# AgentFlow 远程 E2E 测试脚本
# ----------------------------------------------------------------------------
# 连接已部署的远程 bridge endpoint，执行远程兼容的 E2E 测试。

# 加载环境配置
_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../configs" && pwd)" 2>/dev/null || _ENV_DIR=""
_ENV="${AGENTFLOW_ENV:-dev}"
if [ -n "$_ENV_DIR" ] && [ -f "$_ENV_DIR/$_ENV/.env" ]; then set -a; source "$_ENV_DIR/$_ENV/.env"; set +a; fi
unset _ENV_DIR _ENV
# 不编译、不启动本地 daemon/bridge——只通过网络调用远程 REST API。
#
# 与 regression_test.sh 的区别：
#   regression_test.sh  → 本地全量回归（build + unit + E2E + bridge）
#   remote_e2e_test.sh  → 仅远程 REST/SSE 验证（不 build，不自启动服务）
#
# 用法:
#   ./scripts/tests/remote_e2e_test.sh --bridge-url https://preprod:8199
#   ./scripts/tests/remote_e2e_test.sh --bridge-url https://preprod:8199 -v
#   E2E_BRIDGE_URL=https://preprod:8199 ./scripts/tests/remote_e2e_test.sh
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BRIDGE_DIR="$PROJECT_DIR/frontend/bridge"

# ---------------------------------------------------------------------------
# 颜色
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ---------------------------------------------------------------------------
# 参数解析
# ---------------------------------------------------------------------------
BRIDGE_URL="${E2E_BRIDGE_URL:-}"
VERBOSE=""
PYTEST_EXTRA=""

while [ $# -gt 0 ]; do
    case "$1" in
        --bridge-url) BRIDGE_URL="${2:?--bridge-url requires a URL}"; shift 2 ;;
        -v|--verbose) VERBOSE="-v"; shift ;;
        --pytest)     PYTEST_EXTRA="${2:?}"; shift 2 ;;
        --help|-h)
            sed -n '3,20p' "$0"
            echo ""
            echo "选项："
            echo "  --bridge-url URL  远程 bridge endpoint（必填，或设 E2E_BRIDGE_URL 环境变量）"
            echo "  -v, --verbose     pytest 详细输出"
            echo "  --pytest OPTS     附加 pytest 参数（如 '-k test_sse'）"
            echo ""
            echo "示例："
            echo "  $0 --bridge-url https://preprod.example.com:8199"
            echo "  $0 --bridge-url https://preprod.example.com:8199 -v"
            echo "  $0 --bridge-url https://preprod.example.com:8199 --pytest '-k test_sse_chunk'"
            exit 0
            ;;
        *)
            echo "未知参数: $1" >&2
            echo "用法: $0 --bridge-url URL [-v] [--pytest OPTS]" >&2
            exit 2
            ;;
    esac
done

if [ -z "$BRIDGE_URL" ]; then
    echo -e "${RED}错误: 必须指定 --bridge-url 或设置 E2E_BRIDGE_URL 环境变量${NC}" >&2
    echo "用法: $0 --bridge-url https://preprod.example.com:8199"
    exit 2
fi

BRIDGE_URL="${BRIDGE_URL%/}"
export E2E_BRIDGE_URL="$BRIDGE_URL"

# ---------------------------------------------------------------------------
# 健康检查
# ---------------------------------------------------------------------------
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo -e "${BOLD}  远程 E2E 测试${NC}"
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo -e "  Endpoint: ${CYAN}${BRIDGE_URL}${NC}"
echo ""

echo -ne "  健康检查... "
HEALTH_OK=false
for path in "/api/health" "/health" "/health/"; do
    HTTP_CODE=$(curl -sk --max-time 10 -o /dev/null -w "%{http_code}" \
        "${BRIDGE_URL}${path}" 2>/dev/null || echo "000")
    if [ "$HTTP_CODE" = "200" ]; then
        echo -e "${GREEN}✓ (${path})${NC}"
        HEALTH_OK=true
        break
    fi
done

if ! $HEALTH_OK; then
    echo -e "${YELLOW}⚠ 远程 bridge 不可达（最后 HTTP ${HTTP_CODE}）${NC}"
    echo -e "  测试将在 fixture 层 SKIP（不可达 ≠ 失败）"
    echo ""
fi

# ---------------------------------------------------------------------------
# 执行远程兼容的 pytest 测试
# ---------------------------------------------------------------------------
echo ""
echo -e "${BOLD}${CYAN}━━━ 远程 E2E ━━━${NC}"
echo ""

START_TIME=$(date +%s)
FAILED=0

PYTEST_ARGS=(
    "tests/test_e2e_rest_chat_session.py"
    "-m" "e2e"
    $VERBOSE
    $PYTEST_EXTRA
)

if uv run --directory "$BRIDGE_DIR" pytest "${PYTEST_ARGS[@]}" 2>&1; then
    echo -e "  ${GREEN}✓ 远程 E2E 通过${NC}"
else
    echo -e "  ${RED}✗ 远程 E2E 失败${NC}"
    FAILED=1
fi

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
ELAPSED=$(($(date +%s) - START_TIME))
echo ""
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo -e "  耗时: ${ELAPSED}s"
echo ""

if [ "$FAILED" -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}✅ 远程 E2E 完成${NC}"
    exit 0
else
    echo -e "  ${RED}${BOLD}❌ 远程 E2E 有失败${NC}"
    exit 1
fi
