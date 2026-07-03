#!/bin/bash
# ============================================================================
# AgentFlow 回归测试脚本
# ----------------------------------------------------------------------------
# 自动编译并运行全量单元测试 + E2E 测试，输出彩色结果汇总。
#
# 用法:
#   ./ar_scripts/regression_test.sh              # 完整回归：fix → build → unit → e2e
#   ./ar_scripts/regression_test.sh --skip-build # 跳过编译（已有最新二进制）
#   ./ar_scripts/regression_test.sh --skip-fix   # 跳过换行符/权限修复
#   ./ar_scripts/regression_test.sh --unit-only  # 仅运行单元测试
#   ./ar_scripts/regression_test.sh --e2e-only   # 仅运行 E2E 测试
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

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
# 计数器
# ---------------------------------------------------------------------------
UNIT_PASS=0
UNIT_FAIL=0
E2E_PASS=0
E2E_FAIL=0
E2E_TOTAL=0
TEST_TOTAL=0
TEST_FAILED=0
START_TIME=$(date +%s)

# macOS library path
export DYLD_LIBRARY_PATH="$BUILD_DIR/lib:$BUILD_DIR/tests:$BUILD_DIR/tests/e2e:${DYLD_LIBRARY_PATH:-}"

# ---------------------------------------------------------------------------
# 参数解析
# ---------------------------------------------------------------------------
SKIP_BUILD=false
SKIP_FIX=false
UNIT_ONLY=false
E2E_ONLY=false

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
        --skip-fix)   SKIP_FIX=true ;;
        --unit-only)  UNIT_ONLY=true ;;
        --e2e-only)   E2E_ONLY=true ;;
        --help|-h)
            echo "用法: $0 [--skip-build] [--skip-fix] [--unit-only] [--e2e-only]"
            exit 0
            ;;
        *)
            echo "未知参数: $arg"
            echo "用法: $0 [--skip-build] [--skip-fix] [--unit-only] [--e2e-only]"
            exit 1
            ;;
    esac
done

if $UNIT_ONLY && $E2E_ONLY; then
    echo "错误: --unit-only 和 --e2e-only 不能同时使用"
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 0: 修复常见问题（换行符 + 执行权限）
# ---------------------------------------------------------------------------
fix_common_issues() {
    if $SKIP_FIX; then
        echo -e "${YELLOW}[SKIP]${NC} 跳过换行符/权限修复"
        return
    fi

    echo -e "${CYAN}[FIX]${NC} 修复 CRLF 换行符..."
    local fixed=0
    while IFS= read -r -d '' f; do
        if file "$f" | grep -q "CRLF"; then
            sed -i.bak 's/\r$//' "$f"
            rm -f "${f}.bak"
            ((fixed++)) || true
        fi
    done < <(find "$PROJECT_DIR" \( -name "*.sh" -o -name "*.py" \) -type f \
             -not -path "*/build/*" -not -path "*/.git/*" -not -path "*/vendor/*" -print0)
    echo -e "       转换了 ${GREEN}${fixed}${NC} 个文件"

    echo -e "${CYAN}[FIX]${NC} 修复 .sh 执行权限..."
    local perm=0
    while IFS= read -r -d '' f; do
        if [ ! -x "$f" ]; then
            chmod +x "$f"
            ((perm++)) || true
        fi
    done < <(find "$PROJECT_DIR" -name "*.sh" -type f \
             -not -path "*/build/*" -not -path "*/.git/*" -not -path "*/vendor/*" -print0)
    echo -e "       添加了 ${GREEN}${perm}${NC} 个文件的执行权限"
    echo ""
}

# ---------------------------------------------------------------------------
# Step 1: 编译
# ---------------------------------------------------------------------------
do_build() {
    if $SKIP_BUILD; then
        echo -e "${YELLOW}[SKIP]${NC} 跳过编译"
        return
    fi

    echo -e "${CYAN}[BUILD]${NC} 编译项目..."

    if command -v nproc &> /dev/null; then
        NUM_CORES=$(nproc)
    else
        NUM_CORES=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
    fi

    if [ ! -d "$BUILD_DIR" ]; then
        cmake -B build -DCMAKE_BUILD_TYPE=Release -DAGENTFLOW_WITH_RAPIDJSON=ON > /dev/null 2>&1
    fi

    if cmake --build "$BUILD_DIR" -j"$NUM_CORES" 2>&1 | tail -3; then
        echo -e "       ${GREEN}编译成功${NC}"
    else
        echo -e "       ${RED}编译失败${NC}"
        exit 1
    fi
    echo ""
}

# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------
run_test_binary() {
    local name="$1"
    local binary="$2"
    local filter="${3:-}"

    if [ ! -f "$binary" ]; then
        echo -e "  ${YELLOW}⚠ SKIP${NC} $name (二进制不存在)"
        GTEST_OUTPUT=""
        return 0
    fi

    if [ -n "$filter" ]; then
        GTEST_OUTPUT=$("$binary" --gtest_filter="$filter" 2>&1) || true
    else
        GTEST_OUTPUT=$("$binary" 2>&1) || true
    fi

    # 解析 GoogleTest 输出
    local total_line
    total_line=$(echo "$GTEST_OUTPUT" | grep -E '^\[=+\] .* tests? from .* ran' | tail -1 || true)

    if [ -z "$total_line" ]; then
        # 没有匹配到测试 — 不是错误
        echo -e "  ${YELLOW}⚠ SKIP${NC} $name (无匹配测试)"
        TEST_TOTAL=0
        TEST_FAILED=0
        return 0
    fi

    # 格式: [  PASSED  ] 7 tests.
    TEST_TOTAL=$(echo "$total_line" | grep -oE '[0-9]+ tests' | grep -oE '[0-9]+' | head -1 || echo 0)
    # 只匹配包含 (N ms) 的失败行（排除汇总行 "[  FAILED  ] N tests, listed below:"）
    TEST_FAILED=$(echo "$GTEST_OUTPUT" | grep -cE '^\[  FAILED  \].* \([0-9]+ ms\)' || true)

    if [ "$TEST_FAILED" -eq 0 ]; then
        echo -e "  ${GREEN}✓${NC} $name ${GREEN}${TEST_TOTAL}/${TEST_TOTAL}${NC}"
    else
        echo -e "  ${RED}✗${NC} $name ${RED}${TEST_FAILED}/${TEST_TOTAL} FAILED${NC}"
        # 打印失败用例名（排除汇总行）
        echo "$GTEST_OUTPUT" | grep -E '^\[  FAILED  \].* \([0-9]+ ms\)' | while read -r line; do
            echo -e "    ${RED}→${NC} $line"
        done
    fi

    return 0
}

record_unit() {
    local total="${TEST_TOTAL:-0}"
    local failed="${TEST_FAILED:-0}"
    UNIT_PASS=$((UNIT_PASS + total - failed))
    UNIT_FAIL=$((UNIT_FAIL + failed))
}

record_e2e() {
    local total="${TEST_TOTAL:-0}"
    local failed="${TEST_FAILED:-0}"
    E2E_PASS=$((E2E_PASS + total - failed))
    E2E_FAIL=$((E2E_FAIL + failed))
    E2E_TOTAL=$((E2E_TOTAL + total))
}

# ---------------------------------------------------------------------------
# Step 2: 单元测试
# ---------------------------------------------------------------------------
run_unit_tests() {
    echo -e "${BOLD}${CYAN}━━━ 单元测试 ━━━${NC}"
    echo ""

    local f total

    # Agent
    run_test_binary "AgentTests"            "$BUILD_DIR/tests/test_agent_gtest"
    record_unit

    # Workflow
    run_test_binary "WorkflowTests"         "$BUILD_DIR/tests/test_workflow_gtest"
    record_unit

    # Node
    run_test_binary "NodeTests"             "$BUILD_DIR/tests/test_node_gtest"
    record_unit

    # DataBus
    run_test_binary "DataBusTests"          "$BUILD_DIR/tests/test_databus_gtest"
    record_unit

    # DSL Adapter
    run_test_binary "DSLAdapterTests"       "$BUILD_DIR/tests/test_dsl_adapter_gtest"
    record_unit

    # Control Message
    run_test_binary "ControlMessageTests"   "$BUILD_DIR/tests/test_control_message_gtest"
    record_unit

    # ExeLoader
    run_test_binary "ExeLoaderTests"        "$BUILD_DIR/tests/test_exe_loader_gtest"
    record_unit

    # EventHandler
    run_test_binary "EventHandlerTests"     "$BUILD_DIR/tests/test_event_handler_gtest"
    record_unit

    # FlowTrace
    if [ -f "$BUILD_DIR/test_flow_trace_gtest" ]; then
        run_test_binary "FlowTrace" "$BUILD_DIR/test_flow_trace_gtest"
        record_unit
    fi

    echo ""
    echo -e "  单元测试: ${GREEN}${UNIT_PASS} 通过${NC} / ${RED}${UNIT_FAIL} 失败${NC}"
    echo ""
}

# ---------------------------------------------------------------------------
# Step 3: E2E 测试
# ---------------------------------------------------------------------------
run_e2e_tests() {
    echo -e "${BOLD}${CYAN}━━━ E2E 测试 ━━━${NC}"
    echo ""

    local f total

    # Message Routing
    run_test_binary "E2E MessageRouting"    "$BUILD_DIR/tests/e2e/test_e2e_message_routing"
    record_e2e

    # Basic Agent
    run_test_binary "E2E BasicAgent"        "$BUILD_DIR/tests/e2e/test_e2e_basic_agent"
    record_e2e

    # Nested Workflow
    run_test_binary "E2E NestedWorkflow"    "$BUILD_DIR/tests/e2e/test_e2e_nested_workflow"
    record_e2e

    # Interactive Runner
    if [ -f "$BUILD_DIR/tests/e2e/test_e2e_interactive_runner" ]; then
        run_test_binary "E2E InteractiveRunner" "$BUILD_DIR/tests/e2e/test_e2e_interactive_runner"
        record_e2e
    fi

    # DataBus Clients
    if [ -f "$BUILD_DIR/tests/e2e/test_e2e_databus_clients" ]; then
        run_test_binary "E2E DataBusClients" "$BUILD_DIR/tests/e2e/test_e2e_databus_clients"
        record_e2e
    fi

    # EventHandler Blackbox
    if [ -f "$BUILD_DIR/tests/e2e/test_e2e_event_handler_blackbox" ]; then
        run_test_binary "E2E EventHandler"  "$BUILD_DIR/tests/e2e/test_e2e_event_handler_blackbox"
        record_e2e
    fi

    # Multi-Session Concurrent
    if [ -f "$BUILD_DIR/tests/e2e/test_e2e_multi_session_concurrent" ]; then
        run_test_binary "E2E MultiSession"  "$BUILD_DIR/tests/e2e/test_e2e_multi_session_concurrent"
        record_e2e
    fi

    echo ""
    echo -e "  E2E 测试: ${GREEN}${E2E_PASS} 通过${NC} / ${RED}${E2E_FAIL} 失败${NC} (共 ${E2E_TOTAL})"
    echo ""
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  AgentFlow 回归测试${NC}"
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "项目目录: ${CYAN}$PROJECT_DIR${NC}"
    echo ""

    fix_common_issues
    do_build

    if $E2E_ONLY; then
        run_e2e_tests
    elif $UNIT_ONLY; then
        run_unit_tests
    else
        run_unit_tests
        run_e2e_tests
    fi

    # -----------------------------------------------------------------------
    # 最终汇总
    # -----------------------------------------------------------------------
    local elapsed=$(($(date +%s) - START_TIME))
    local total_pass=$((UNIT_PASS + E2E_PASS))
    local total_fail=$((UNIT_FAIL + E2E_FAIL))
    local total_all=$((total_pass + total_fail))

    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  测试结果汇总${NC}"
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "  单元: ${GREEN}${UNIT_PASS} 通过${NC} / ${RED}${UNIT_FAIL} 失败${NC}"
    echo -e "  E2E:  ${GREEN}${E2E_PASS} 通过${NC} / ${RED}${E2E_FAIL} 失败${NC}"
    echo -e "  ───────────────────────────"
    echo -e "  合计: ${GREEN}${total_pass} 通过${NC} / ${RED}${total_fail} 失败${NC} (共 ${total_all})"
    echo -e "  耗时: ${elapsed}s"
    echo ""

    if [ "$total_fail" -eq 0 ]; then
        echo -e "  ${GREEN}${BOLD}✅ 全部通过！${NC}"
        exit 0
    else
        echo -e "  ${RED}${BOLD}❌ 有 ${total_fail} 个测试失败${NC}"
        exit 1
    fi
}

main
