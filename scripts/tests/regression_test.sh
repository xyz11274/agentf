#!/bin/bash
# ============================================================================
# AgentFlow 回归测试脚本
# ----------------------------------------------------------------------------
# 自动编译并运行全量单元测试 + E2E 测试，输出彩色结果汇总。
#
# 用法:
#   ./scripts/regression_test.sh              # 完整回归：fix → build → unit → e2e
#   ./scripts/regression_test.sh --skip-build # 跳过编译（已有最新二进制）
#   ./scripts/regression_test.sh --skip-fix   # 跳过换行符/权限修复
#   ./scripts/regression_test.sh --unit-only  # 仅运行单元测试
#   ./scripts/regression_test.sh --e2e-only   # 仅运行 E2E 测试
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
BRIDGE_PASS=0
BRIDGE_FAIL=0
TEST_TOTAL=0
TEST_FAILED=0
START_TIME=$(date +%s)

# Platform-specific library path (DYLD on macOS, LD on Linux)
case "$(uname -s)" in
    Darwin)
        export DYLD_LIBRARY_PATH="$BUILD_DIR/lib:$BUILD_DIR/tests:$BUILD_DIR/tests/e2e:${DYLD_LIBRARY_PATH:-}"
        ;;
    Linux)
        export LD_LIBRARY_PATH="$BUILD_DIR/lib:$BUILD_DIR/tests:$BUILD_DIR/tests/e2e:${LD_LIBRARY_PATH:-}"
        ;;
esac

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
    elif [ -f /proc/cpuinfo ]; then
        NUM_CORES=$(grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 4)
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
    local env_vars="${4:-}"

    if [ ! -f "$binary" ]; then
        echo -e "  ${YELLOW}⚠ SKIP${NC} $name (二进制不存在)"
        GTEST_OUTPUT=""
        return 0
    fi

    # 局部环境注入：env_vars 用换行分隔的 "KEY=val" 条目（值可含空格）。
    # 用数组传递，避免 word splitting 破坏含空格的值；保留当前 shell 已 export
    # 的环境（如 DYLD_LIBRARY_PATH），仅在其上叠加。
    local -a env_args=()
    if [ -n "$env_vars" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && env_args+=("$line")
        done <<< "$env_vars"
    fi

    if [ -n "$filter" ]; then
        if [ ${#env_args[@]} -gt 0 ]; then
            GTEST_OUTPUT=$(env "${env_args[@]}" "$binary" --gtest_filter="$filter" 2>&1) || true
        else
            GTEST_OUTPUT=$("$binary" --gtest_filter="$filter" 2>&1) || true
        fi
    else
        if [ ${#env_args[@]} -gt 0 ]; then
            GTEST_OUTPUT=$(env "${env_args[@]}" "$binary" 2>&1) || true
        else
            GTEST_OUTPUT=$("$binary" 2>&1) || true
        fi
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
    TEST_TOTAL=$(echo "$total_line" | grep -oE '[0-9]+ tests?' | grep -oE '[0-9]+' | head -1 || echo 0)
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

# 用于非 GoogleTest 格式的 E2E 二进制（custom main()，无 [PASSED] N tests 输出）。
# 通过 exit code 判断成败，以 "通过/失败" 二元计数。
run_legacy_e2e_binary() {
    local name="$1"
    local binary="$2"

    if [ ! -f "$binary" ]; then
        echo -e "  ${YELLOW}⚠ SKIP${NC} $name (二进制不存在)"
        TEST_TOTAL=0
        TEST_FAILED=0
        return 0
    fi

    local output
    output=$("$binary" 2>&1) || true

    # Parse "Tests passed/failed" from custom output
    local passed_count
    local failed_count
    passed_count=$(echo "$output" | grep -oE 'Tests passed: [0-9]+' | grep -oE '[0-9]+' | tail -1 || echo 0)
    failed_count=$(echo "$output" | grep -oE 'Tests failed: [0-9]+' | grep -oE '[0-9]+' | tail -1 || echo 0)

    # Fallback: use exit code
    local exit_code=${PIPESTATUS[0]:-$?}
    if [ "$failed_count" -gt 0 ] || [ "$exit_code" -ne 0 ]; then
        echo -e "  ${RED}✗${NC} $name ${RED}${failed_count}/${passed_count} FAILED${NC}"
        # Print first few error lines
        echo "$output" | grep -iE 'FAILED|ERROR' | head -5 | while read -r line; do
            echo -e "    ${RED}→${NC} $line"
        done
        TEST_TOTAL=$((passed_count + failed_count))
        TEST_FAILED=$failed_count
    else
        if [ "$passed_count" -gt 0 ]; then
            echo -e "  ${GREEN}✓${NC} $name ${GREEN}${passed_count}/${passed_count}${NC}"
        else
            echo -e "  ${GREEN}✓${NC} $name"
        fi
        TEST_TOTAL=$((passed_count + failed_count))
        TEST_FAILED=0
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
    run_test_binary "DataBusConcurrentTests" "$BUILD_DIR/tests/test_databus_concurrent"
    record_unit

    # AgentFlow (JSON parsing, UDS, combined)
    run_test_binary "AgentFlowGoogleTests"  "$BUILD_DIR/tests/test_agentflow_gtest"
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

    # FlowTrace —— 必须分两个独立进程运行：
    # FlowTraceWriter 是单例，仅在 instance() 首次访问时读取 FLOW_TRACE_DIR，
    # 因此两个 TEST 必须各自在「环境已定」的独立进程中执行（与 CMakeLists.txt
    # 中 FlowTraceWriter_NoFlowTraceDir / FlowTraceWriter_WithFlowTraceDir 两条
    # ctest 的设计一致）。
    if [ -f "$BUILD_DIR/test_flow_trace_gtest" ]; then
        local _flow_trace_ut_dir="$BUILD_DIR/flow_trace_ut_out"
        mkdir -p "$_flow_trace_ut_dir"

        # NoFlowTraceDir：显式清空 FLOW_TRACE_DIR（覆盖脚本外部可能已设置的值）
        run_test_binary "FlowTrace (no-dir)" \
            "$BUILD_DIR/test_flow_trace_gtest" \
            "FlowTraceWriterTest.WriteWithoutConfiguredDir" \
            "FLOW_TRACE_DIR="
        record_unit

        # WithFlowTraceDir：设置指向构建目录下的输出目录
        run_test_binary "FlowTrace (with-dir)" \
            "$BUILD_DIR/test_flow_trace_gtest" \
            "FlowTraceWriterTest.WriteWithFlowTraceDirEnv" \
            "FLOW_TRACE_DIR=$_flow_trace_ut_dir"
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

    # EventHandler Blackbox — REMOVED: test data (system_agent_mock) deleted in commit 61fb5cb
    # Stale binary (if present) should be deleted manually or via clean build.

    # Multi-Session Concurrent (custom main, 非 GTest)
    if [ -f "$BUILD_DIR/tests/e2e/test_e2e_multi_session_concurrent" ]; then
        run_legacy_e2e_binary "E2E MultiSession" "$BUILD_DIR/tests/e2e/test_e2e_multi_session_concurrent"
        record_e2e
    fi

    # SystemAgentMock — coordinator 多轮循环 E2E（disp→skill→re-trigger→final）
    if [ -f "$BUILD_DIR/tests/e2e/test_e2e_system_agent_mock" ]; then
        run_legacy_e2e_binary "E2E SystemAgentMock" "$BUILD_DIR/tests/e2e/test_e2e_system_agent_mock"
        record_e2e
    fi

    echo ""
    echo -e "  E2E 测试: ${GREEN}${E2E_PASS} 通过${NC} / ${RED}${E2E_FAIL} 失败${NC} (共 ${E2E_TOTAL})"
    echo ""
}

# ---------------------------------------------------------------------------
# Step 3.5: 前端 Vitest 测试
# ---------------------------------------------------------------------------
run_frontend_tests() {
    echo -e "${BOLD}${CYAN}━━━ 前端测试 ━━━${NC}"
    echo ""

    local web_dir="$PROJECT_DIR/frontend/web"
    local failed=0
    FRONTEND_PASS=0
    FRONTEND_FAIL=0

    if [ -d "$web_dir" ]; then
        echo -e "  ${CYAN}[vitest]${NC} 前端单元测试..."
        if cd "$web_dir" && npx vitest run --reporter=verbose 2>&1 | tail -3; then
            echo -e "  ${GREEN}✓ 前端测试通过${NC}"
            FRONTEND_PASS=1
        else
            echo -e "  ${RED}✗ 前端测试失败${NC}"
            failed=1
        fi
        cd "$PROJECT_DIR"
    else
        echo -e "  ${YELLOW}⚠ 跳过前端测试（frontend/web 不存在）${NC}"
    fi

    FRONTEND_FAIL=$failed
    echo ""
}

# ---------------------------------------------------------------------------
# Step 4: Python Bridge 测试（sysagent 回调 + config + SSL）
# ---------------------------------------------------------------------------
run_python_bridge_tests() {
    echo -e "${BOLD}${CYAN}━━━ Python Bridge 测试 ━━━${NC}"
    echo ""

    local bridge_dir="$PROJECT_DIR/frontend/bridge"
    local failed=0

    # 1) 单元 + 集成测试（mock daemon，无需 daemon 二进制）
    echo -e "  ${CYAN}[pytest]${NC} Bridge 集成测试..."
    if uv run --directory "$bridge_dir" pytest \
        tests/test_aisf_tasks.py \
        tests/test_config.py \
        tests/test_upstream_callback_ssl.py \
        tests/test_sysagent_callback_e2e.py \
        tests/test_chat_rest_sse.py \
        tests/test_chat.py \
        tests/test_routes.py \
        -v --no-header 2>&1 | tail -5; then
        echo -e "  ${GREEN}✓ Bridge 集成测试通过${NC}"
        BRIDGE_PASS=1
    else
        echo -e "  ${RED}✗ Bridge 集成测试失败${NC}"
        failed=1
    fi
    echo ""

    # 2) 真实 daemon E2E（需要 daemon 二进制 + outfit-assistant/.venv）
    if [ -f "$BUILD_DIR/agentflowd" ]; then
        echo -e "  ${CYAN}[pytest]${NC} 真实 daemon E2E..."
        if uv run --directory "$bridge_dir" pytest \
            tests/test_sysagent_reald_daemon_e2e.py \
            tests/test_e2e_rest_chat_session.py \
            -v --no-header -m e2e 2>&1 | tail -5; then
            echo -e "  ${GREEN}✓ 真实 daemon E2E 通过${NC}"
        else
            echo -e "  ${RED}✗ 真实 daemon E2E 失败${NC}"
            failed=1
        fi
    else
        echo -e "  ${YELLOW}⚠ 跳过真实 daemon E2E（agentflowd 未编译）${NC}"
    fi

    echo ""

    # 3) Outfit assistant 节点单元测试（mock databus，无需 daemon）
    local outfit_dir="$PROJECT_DIR/examples/agent/outfit-assistant"
    if [ -d "$outfit_dir" ]; then
        echo -e "  ${CYAN}[pytest]${NC} Outfit assistant 节点测试..."
        if python -m pytest "$outfit_dir/tests" \
            -v --no-header 2>&1 | tail -5; then
            echo -e "  ${GREEN}✓ Outfit assistant 节点测试通过${NC}"
            BRIDGE_PASS=$((BRIDGE_PASS + 1))
        else
            echo -e "  ${RED}✗ Outfit assistant 节点测试失败${NC}"
            failed=1
        fi
    else
        echo -e "  ${YELLOW}⚠ 跳过 Outfit assistant 测试（目录不存在）${NC}"
    fi

    # 4) 真实 daemon outfit E2E（需要 daemon 二进制，自动启停 daemon）
    if [ -f "$BUILD_DIR/agentflowd" ]; then
        local e2e_passed=true

        for e2e_file in test_e2e_outfit_agent.py test_e2e_outfit_multiturn.py test_e2e_outfit_skill.py; do
            if [ -f "$bridge_dir/tests/$e2e_file" ]; then
                echo -e "  ${CYAN}[pytest]${NC} Outfit E2E: $e2e_file..."
                if uv run --directory "$bridge_dir" pytest \
                    "tests/$e2e_file" \
                    -v --no-header -m e2e 2>&1 | tail -5; then
                    echo -e "  ${GREEN}✓ $e2e_file 通过${NC}"
                    BRIDGE_PASS=$((BRIDGE_PASS + 1))
                else
                    echo -e "  ${RED}✗ $e2e_file 失败${NC}"
                    e2e_passed=false
                fi
            fi
        done

        # 5) Agentflow 完整执行链路 E2E（真实 LLM + daemon）
        local e2e_pipeline="$PROJECT_DIR/frontend/bridge/tests/e2e_agentflow_pipeline.py"
        if [ -f "$e2e_pipeline" ]; then
            echo -e "  ${CYAN}[E2E]${NC} Agentflow pipeline (real LLM)..."
            if cd "$PROJECT_DIR" && python3 "$e2e_pipeline" 2>&1 | tail -3; then
                echo -e "  ${GREEN}✓ Agentflow pipeline 通过${NC}"
                BRIDGE_PASS=$((BRIDGE_PASS + 1))
            else
                echo -e "  ${RED}✗ Agentflow pipeline 失败${NC}"
                e2e_passed=false
            fi
        fi

        if ! $e2e_passed; then
            failed=1
        fi
    else
        echo -e "  ${YELLOW}⚠ 跳过真实 daemon outfit E2E（agentflowd 未编译）${NC}"
    fi

    BRIDGE_FAIL=$failed
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
        run_frontend_tests
    else
        run_unit_tests
        run_e2e_tests
        run_frontend_tests
        run_python_bridge_tests
    fi

    # -----------------------------------------------------------------------
    # 最终汇总
    # -----------------------------------------------------------------------
    local elapsed=$(($(date +%s) - START_TIME))
    local total_pass=$((UNIT_PASS + E2E_PASS + (FRONTEND_PASS) + (BRIDGE_PASS)))
    local total_fail=$((UNIT_FAIL + E2E_FAIL + FRONTEND_FAIL + BRIDGE_FAIL))
    local total_all=$((total_pass + total_fail))

    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  测试结果汇总${NC}"
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "  单元:       ${GREEN}${UNIT_PASS} 通过${NC} / ${RED}${UNIT_FAIL} 失败${NC}"
    echo -e "  E2E:        ${GREEN}${E2E_PASS} 通过${NC} / ${RED}${E2E_FAIL} 失败${NC}"
    echo -e "  Frontend:   ${GREEN}${FRONTEND_PASS:-0} 通过${NC} / ${RED}${FRONTEND_FAIL:-0} 失败${NC}"
    echo -e "  Bridge:     ${GREEN}${BRIDGE_PASS} 通过${NC} / ${RED}${BRIDGE_FAIL} 失败${NC}"
    echo -e "  ───────────────────────────"
    echo -e "  合计:       ${GREEN}${total_pass} 通过${NC} / ${RED}${total_fail} 失败${NC} (共 ${total_all})"
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
