#!/bin/bash
# ============================================================================
# AgentFlow 回归测试脚本
# ----------------------------------------------------------------------------
# 自动编译并运行全量单元测试 + E2E 测试，输出彩色结果汇总。

# 加载环境配置
_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../configs" && pwd)" 2>/dev/null || _ENV_DIR=""
_ENV="${AGENTFLOW_ENV:-dev}"
if [ -n "$_ENV_DIR" ] && [ -f "$_ENV_DIR/$_ENV/.env" ]; then set -a; source "$_ENV_DIR/$_ENV/.env"; set +a; fi
unset _ENV_DIR _ENV
#
# 用法:
#   ./scripts/regression_test.sh              # 完整回归：fix → build → unit → e2e
#   ./scripts/regression_test.sh --skip-build # 跳过编译（已有最新二进制）
#   ./scripts/regression_test.sh --skip-fix   # 跳过换行符/权限修复
#   ./scripts/regression_test.sh --unit-only  # 仅运行单元测试
#   ./scripts/regression_test.sh --e2e-only   # 仅运行 E2E 测试
#
# 环境变量配置:
#   DOCKER_SANDBOX_ENABLED=1    # 启用 Docker 沙箱 E2E 测试（需 Docker daemon）
#   SANDBOX_PRESERVE=1          # 保留沙箱测试现场（不清理进程/容器，供调试）
# ============================================================================

set -uo pipefail
# ⚠ 不设 -e：回归脚本用 PASS/FAIL 计数器追踪结果，不依赖 set -e 退出。
# set -e 会与 exec > >(tee ...) 管道冲突，导致沙箱测试段在管道断开时被静默跳过。

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
# 辅助函数：带超时保护的命令执行（兼容 Linux/macOS）
# ---------------------------------------------------------------------------
_timeout() {
    local _sec="$1"; shift
    if command -v timeout &>/dev/null; then
        timeout "$_sec" "$@"
    elif command -v gtimeout &>/dev/null; then
        gtimeout "$_sec" "$@"
    else
        # 兜底：无 timeout 命令时直接执行（macOS 可 `brew install coreutils`）
        "$@"
    fi
}

# ---------------------------------------------------------------------------
# 辅助函数：检测当前沙箱有效模式（综合配置文件 + 环境变量）
# ---------------------------------------------------------------------------
_detect_sandbox_mode() {
    local _cfg="$PROJECT_DIR/configs/test/agentflowd.json"
    local _cfg_mode="unknown"
    local _docker_enabled=false
    local _e2b_key=false

    # 1) 从配置文件读取 sandbox.mode
    if [ -f "$_cfg" ]; then
        _cfg_mode=$(python3 -c "
import json
with open('$_cfg') as f:
    cfg = json.load(f)
print(cfg.get('sandbox', {}).get('mode', 'unknown'))
" 2>/dev/null || echo "unknown")
    fi

    # 2) 检查环境变量 + 配置文件中的 e2b_api_key（开发/测试/生产环境由配置文件驱动）
    [ "${DOCKER_SANDBOX_ENABLED:-}" = "1" ] && _docker_enabled=true
    if [ -n "${E2B_API_KEY:-}" ]; then
        _e2b_key=true
    elif [ -f "$_cfg" ]; then
        # 配置文件中的 e2b_api_key 非空即视为已配置
        _cfg_key=$(python3 -c "
import json
with open('$_cfg') as f:
    cfg = json.load(f)
print(cfg.get('sandbox', {}).get('e2b_api_key', '') or '')
" 2>/dev/null)
        [ -n "$_cfg_key" ] && _e2b_key=true
    fi

    # 3) 综合推断有效模式
    local _effective="$_cfg_mode"
    local _hint=""
    if [ "$_cfg_mode" = "e2b" ] && ! $_e2b_key; then
        _effective="e2b (退化: 无 E2B_API_KEY，daemon 启动会回退到 none)"
        _hint="  ${YELLOW}提示: 设置 E2B_API_KEY 启用 e2b 沙箱${NC}"
    elif [ "$_cfg_mode" = "e2b" ] && $_e2b_key; then
        _effective="e2b (就绪)"
    elif [ "$_cfg_mode" = "docker" ] && ! $_docker_enabled; then
        _effective="docker (退化: Docker 引擎可用但可能未运行)"
    elif [ "$_cfg_mode" = "docker" ] && $_docker_enabled; then
        _effective="docker (就绪)"
    fi

    # 4) 额外提示
    local _extra=""
    $_docker_enabled && _extra="${_extra}  DOCKER_SANDBOX_ENABLED=1"
    $_e2b_key && _extra="${_extra}  E2B_API_KEY=已设置"

    echo "sandbox.mode=${_cfg_mode} | 有效=${_effective}${_extra:+ | env:${_extra}}"
    [ -n "$_hint" ] && echo "$_hint"
}

# ---------------------------------------------------------------------------
# 辅助函数：扫描 daemon.log 提取沙箱运行时证据
# ---------------------------------------------------------------------------
_check_sandbox_logs() {
    local _log_file="$1"
    local _label="$2"  # 描述标签，如"回归测试 daemon"或"开发 daemon"

    if [ ! -f "$_log_file" ]; then
        return
    fi

    # 提取关键信息（使用 Python 代替 macOS 不支持的 grep -P）
    local _extracted
    _extracted=$(python3 -c "
import re, sys
with open('$1') as f:
    content = f.read()

mode = ''
m = re.search(r'Initialized sandbox management: mode=([^,]+)', content)
if m: mode = m.group(1)

fail_count = content.count('Failed to launch sandbox')

fail_reasons = '; '.join(sorted(set(re.findall(r'Failed to launch sandbox.*?: ([^,]+)', content))))

timestamps = re.findall(r'\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}', content)
last_fail = timestamps[-1] if timestamps else ''

isolated = content.count('continuing without sandbox isolation')

print('{}||{}||{}||{}||{}'.format(mode, fail_count, fail_reasons, last_fail, isolated))
" 2>/dev/null || echo "||0|||0")
    local _mode; _mode="${_extracted%%||*}"
    local _remain="${_extracted#*||}"
    local _fail_count; _fail_count="${_remain%%||*}"
    _remain="${_remain#*||}"
    local _fail_reason; _fail_reason="${_remain%%||*}"
    _remain="${_remain#*||}"
    local _last_fail; _last_fail="${_remain%%||*}"
    _remain="${_remain#*||}"
    local _isolated; _isolated="${_remain}"

    if [ -n "$_mode" ]; then
        echo "  $_label: mode=${_mode}"
        if [ "$_fail_count" -gt 0 ] || [ "$_isolated" -gt 0 ]; then
            echo "    ${RED}⚠ 沙箱告警${NC}"
            echo "    ${RED}→${NC} 沙箱启动失败: ${_fail_count} 次"
            [ -n "$_fail_reason" ] && echo "    ${RED}→${NC} 原因: ${_fail_reason}"
            [ -n "$_last_fail" ] && echo "    ${RED}→${NC} 最近失败: ${_last_fail}"
            echo "    ${RED}→${NC} 影响: ${_isolated} 次请求在无沙箱隔离下执行"
            return 1
        else
            echo "    ${GREEN}✓${NC} 沙箱运行正常"
        fi
    fi
    return 0
}

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
FRONTEND_PASS=0
FRONTEND_FAIL=0
SANDBOX_PASS=0
SANDBOX_FAIL=0
SANDBOX_ALERT=false
TEST_TOTAL=0
TEST_FAILED=0
START_TIME=$(date +%s)

# Platform-specific library path (DYLD on macOS, LD on Linux)
case "$(uname -s)" in
    Darwin)
        # conda 的 libc++（2022）缺少 __from_native_exception_pointer 符号
        # 确保 /usr/lib（系统 libc++）在搜索路径最前面
        sys_lib="/usr/lib"
        raw="${DYLD_LIBRARY_PATH:-}"
        # 过滤掉 conda 的 lib 路径，避免 ABI 不匹配
        filtered=""
        if [ -n "$raw" ]; then
            filtered=$(echo "$raw" | tr ':' '\n' | grep -v "^/Users/.*/miniconda3/lib$" | grep -v "^$sys_lib$" | tr '\n' ':')
            filtered="${filtered%:}"
        fi
        export DYLD_LIBRARY_PATH="$sys_lib:$BUILD_DIR/lib:$BUILD_DIR/tests:$BUILD_DIR/tests/e2e${filtered:+:$filtered}"
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

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true; shift ;;
        --skip-fix)   SKIP_FIX=true; shift ;;
        --unit-only)  UNIT_ONLY=true; shift ;;
        --e2e-only)   E2E_ONLY=true; shift ;;
        --help|-h)
            echo "用法: $0 [--skip-build] [--skip-fix] [--unit-only] [--e2e-only]"
            echo ""
            echo "  远程 E2E 测试请使用: scripts/tests/remote_e2e_test.sh --bridge-url URL"
            exit 0
            ;;
        *)
            echo "未知参数: $1"
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

    if _timeout 600 cmake --build "$BUILD_DIR" -j"$NUM_CORES" 2>&1 | tail -3; then
        echo -e "       ${GREEN}编译成功${NC}"
    else
        echo -e "       ${RED}编译失败${NC}"
        exit 1
    fi

    # Sidecar build-dev（stub 模式 COREMIND_ENABLED=OFF）— 为 SidecarErrorSymmetry 护测供二进制
    # 本地无 CoreMind SDK，必须 stub 编译（对齐 PROJECT_MEMORY：本地走 COREMIND_ENABLED=OFF）
    local _sidecar_dir="$PROJECT_DIR/sidecar"
    local _sidecar_build="$_sidecar_dir/build-dev"
    if [ -f "$_sidecar_dir/CMakeLists.txt" ]; then
        if [ ! -d "$_sidecar_build" ]; then
            cmake -B "$_sidecar_build" -S "$_sidecar_dir" \
                -DCMAKE_BUILD_TYPE=Debug -DCOREMIND_ENABLED=OFF > /dev/null 2>&1
        fi
        if _timeout 120 cmake --build "$_sidecar_build" --target coremind_bridge \
                --target test_router_logic -j"$NUM_CORES" 2>&1 | tail -2; then
            echo -e "       ${GREEN}sidecar stub 编译成功${NC}"
        else
            echo -e "       ${YELLOW}⚠ sidecar stub 编译失败（SidecarErrorSymmetry 将 SKIP）${NC}"
        fi
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
    run_test_binary "AgentFlowGoogleTests"  "$BUILD_DIR/test_agentflow_gtest"
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

    # Sidecar 异步路径错误四分档对称性护测 — dev/prod 行为分叉回归门禁
    # 真启 sidecar（build-dev stub）+ mock Bridge + mock callback，覆盖错误四分档：
    #   ① bridge 不可达  ② HTTP 非 200  ③ SSE 流内 error payload  ④ 成功
    # 锁住 dev forward_to_bridge 与 prod sidecar_router 的错误处理行为对称
    # （F13 铁律：不 mock 关键集成点，真起 sidecar + 真发 HTTP + 真收回调）
    local _sym_script="$PROJECT_DIR/sidecar/tests/test_error_branch_symmetry.py"
    if [ -f "$_sym_script" ] && [ -f "$PROJECT_DIR/sidecar/build-dev/coremind_bridge" ]; then
        # 脚本自带 #!/usr/bin/env python3 shebang，直接执行（chmod +x 已设）
        run_legacy_e2e_binary "E2E SidecarErrorSymmetry" "$_sym_script"
        record_e2e
    else
        echo -e "  ${YELLOW}⚠ SKIP${NC} E2E SidecarErrorSymmetry (脚本或 sidecar 二进制不存在)"
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

    if [ -d "$web_dir" ]; then
        echo -e "  ${CYAN}[vitest]${NC} 前端单元测试..."
        if cd "$web_dir" && _timeout 120 npx vitest run --reporter=verbose 2>&1 | tail -3; then
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

    # ── 加载测试环境配置 ──
    # configs/test/bridge.env 中的变量会被 configs/test/bridge.yaml 的
    # ${VAR:-default} 展开捕获，同时被 Bridge 子进程继承。
    local _bridge_env="$PROJECT_DIR/configs/test/bridge.env"
    if [ -f "$_bridge_env" ]; then
        set -a  # 自动 export 后续变量
        source "$_bridge_env"
        set +a
        echo -e "  ${CYAN}[env]${NC} 已加载: $_bridge_env"
    fi

    # ── DYLD_LIBRARY_PATH 保护 ──
    # C++ 测试需要在 DYLD_LIBRARY_PATH 中注入 /usr/lib（系统 libc++），
    # 但 macOS 下该变量会污染 Python 的 sqlite3 加载——设置后
    # _sqlite3.cpython-312-darwin.so 链接到系统 /usr/lib/libsqlite3.dylib
    # （缺少 _sqlite3_enable_load_extension 符号），导致 ImportError。
    # 因此 Python 测试前必须清除，测试后恢复。
    local _saved_dyld="${DYLD_LIBRARY_PATH:-}"
    local _saved_ld="${LD_LIBRARY_PATH:-}"
    unset DYLD_LIBRARY_PATH
    unset LD_LIBRARY_PATH

    # 1) 单元 + 集成测试（mock daemon，无需 daemon 二进制）
    echo -e "  ${CYAN}[pytest]${NC} Bridge 集成测试..."
    local _pytest_out; _pytest_out="$(mktemp)"
    # -k 排除依赖 agentflow_runtime（bridge venv 未安装）的测试类
    if uv run --directory "$bridge_dir" pytest \
        tests/test_config.py \
        tests/test_chat_rest_sse.py \
        tests/test_chat.py \
        tests/test_routes.py \
        tests/test_e2e_comprehensive.py \
        tests/test_artifacts.py \
        tests/test_artifact_storage_e2e.py \
        tests/test_agent_env_inject.py \
        tests/test_content_serializer.py \
        tests/test_packages.py \
        tests/test_chat_artifact_contract.py \
        -v --no-header \
        -k "not TestUploadToStorage and not TestSandboxArtifactDownload and not TestArtifactUrlPipeline" \
        2>&1 | tee "$_pytest_out" | tail -5; then
        echo -e "  ${GREEN}✓ Bridge 集成测试通过${NC}"
        BRIDGE_PASS=$((BRIDGE_PASS + 1))
    else
        echo -e "  ${RED}✗ Bridge 集成测试失败${NC}"
        echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
        cat "$_pytest_out"
        BRIDGE_FAIL=$((BRIDGE_FAIL + 1))
    fi
    rm -f "$_pytest_out"
    echo ""

    # 2) 真实 daemon E2E（需要 daemon 二进制 + outfit-assistant/.venv）
    if [ -f "$BUILD_DIR/agentflowd" ]; then
        echo -e "  ${CYAN}[pytest]${NC} 真实 daemon E2E..."
        local _pytest_out; _pytest_out="$(mktemp)"
        if uv run --directory "$bridge_dir" pytest \
            tests/test_sysagent_reald_daemon_e2e.py \
            tests/test_e2e_rest_chat_session.py \
            -v --no-header -m e2e 2>&1 | tee "$_pytest_out" | tail -5; then
            echo -e "  ${GREEN}✓ 真实 daemon E2E 通过${NC}"
            BRIDGE_PASS=$((BRIDGE_PASS + 1))
        else
            echo -e "  ${RED}✗ 真实 daemon E2E 失败${NC}"
            echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
            cat "$_pytest_out"
            BRIDGE_FAIL=$((BRIDGE_FAIL + 1))
        fi
        rm -f "$_pytest_out"
    else
        echo -e "  ${YELLOW}⚠ 跳过真实 daemon E2E（agentflowd 未编译）${NC}"
    fi

    echo ""

    # 3) Outfit assistant 节点单元测试（mock databus，无需 daemon）
    local outfit_dir="$PROJECT_DIR/examples/agent/outfit-assistant"
    if [ -d "$outfit_dir" ]; then
        echo -e "  ${CYAN}[pytest]${NC} Outfit assistant 节点测试..."
        local _pytest_out; _pytest_out="$(mktemp)"
        if python -m pytest "$outfit_dir/tests" \
            -v --no-header 2>&1 | tee "$_pytest_out" | tail -5; then
            echo -e "  ${GREEN}✓ Outfit assistant 节点测试通过${NC}"
            BRIDGE_PASS=$((BRIDGE_PASS + 1))
        else
            echo -e "  ${RED}✗ Outfit assistant 节点测试失败${NC}"
            echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
            cat "$_pytest_out"
            BRIDGE_FAIL=$((BRIDGE_FAIL + 1))
        fi
        rm -f "$_pytest_out"
    else
        echo -e "  ${YELLOW}⚠ 跳过 Outfit assistant 测试（目录不存在）${NC}"
    fi

    # 4) 真实 daemon outfit E2E（需要 daemon 二进制，自动启停 daemon）
    if [ -f "$BUILD_DIR/agentflowd" ]; then
        for e2e_file in test_e2e_outfit_agent.py test_e2e_outfit_multiturn.py test_e2e_outfit_skill.py; do
            if [ -f "$bridge_dir/tests/$e2e_file" ]; then
                echo -e "  ${CYAN}[pytest]${NC} Outfit E2E: $e2e_file..."
                local _pytest_out; _pytest_out="$(mktemp)"
                if uv run --directory "$bridge_dir" pytest \
                    "tests/$e2e_file" \
                    -v --no-header -m e2e 2>&1 | tee "$_pytest_out" | tail -5; then
                    echo -e "  ${GREEN}✓ $e2e_file 通过${NC}"
                    BRIDGE_PASS=$((BRIDGE_PASS + 1))
                else
                    echo -e "  ${RED}✗ $e2e_file 失败${NC}"
                    echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
                    cat "$_pytest_out"
                    BRIDGE_FAIL=$((BRIDGE_FAIL + 1))
                fi
                rm -f "$_pytest_out"
            fi
        done

        # 5) Minimal agent 真实 LLM 连通性 E2E（替代旧的 e2e_agentflow_pipeline.py）
        #    极简 agent fixture（单 coordinator 节点）+ 真实/mock LLM 自动切换
        local e2e_minimal="$bridge_dir/tests/test_e2e_minimal_agent.py"
        if [ -f "$e2e_minimal" ]; then
            echo -e "  ${CYAN}[pytest]${NC} Minimal agent E2E (连通性)..."
            local _pytest_out; _pytest_out="$(mktemp)"
            if uv run --directory "$bridge_dir" pytest \
                "tests/test_e2e_minimal_agent.py" \
                -v --no-header -m e2e 2>&1 | tee "$_pytest_out" | tail -5; then
                echo -e "  ${GREEN}✓ Minimal agent E2E 通过${NC}"
                BRIDGE_PASS=$((BRIDGE_PASS + 1))
            else
                echo -e "  ${RED}✗ Minimal agent E2E 失败${NC}"
                echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
                cat "$_pytest_out"
                BRIDGE_FAIL=$((BRIDGE_FAIL + 1))
            fi
            rm -f "$_pytest_out"
        fi

        # 6) Docker 沙箱 + 穿搭助手 E2E（可选，需 DOCKER_SANDBOX_ENABLED=1）
        local e2e_docker_sandbox="$bridge_dir/tests/test_e2e_outfit_docker_sandbox.py"
        if [ -f "$e2e_docker_sandbox" ]; then
            local _sandbox_mode; _sandbox_mode="$(_detect_sandbox_mode)"
            echo -e "  ${CYAN}[pytest]${NC} Docker 沙箱 E2E"
            echo -e "    ${CYAN}[沙箱配置]${NC} $_sandbox_mode"
            local _pytest_out; _pytest_out="$(mktemp)"
            if DOCKER_SANDBOX_ENABLED=1 uv run --directory "$bridge_dir" pytest \
                "tests/test_e2e_outfit_docker_sandbox.py" \
                -v --no-header -m e2e 2>&1 | tee "$_pytest_out" | tail -5; then
                echo -e "  ${GREEN}✓ Docker 沙箱 E2E 通过${NC}"
                BRIDGE_PASS=$((BRIDGE_PASS + 1))
            else
                echo -e "  ${RED}✗ Docker 沙箱 E2E 失败${NC}"
                echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
                cat "$_pytest_out"
                BRIDGE_FAIL=$((BRIDGE_FAIL + 1))
            fi
            rm -f "$_pytest_out"
        fi
    else
        echo -e "  ${YELLOW}⚠ 跳过真实 daemon outfit E2E（agentflowd 未编译）${NC}"
    fi

    # ── 恢复 DYLD/LD_LIBRARY_PATH ──
    if [ -n "$_saved_dyld" ]; then
        export DYLD_LIBRARY_PATH="$_saved_dyld"
    else
        unset DYLD_LIBRARY_PATH
    fi
    if [ -n "$_saved_ld" ]; then
        export LD_LIBRARY_PATH="$_saved_ld"
    else
        unset LD_LIBRARY_PATH
    fi

    echo ""
}

# ---------------------------------------------------------------------------
# Step 5: Sandbox E2E 测试（LocalSandbox + DockerSandbox）
# ---------------------------------------------------------------------------
run_sandbox_e2e_tests() {
    echo -e "${BOLD}${CYAN}━━━ 沙箱 E2E 测试 ━━━${NC}"
    # 显示当前沙箱模式
    local _sandbox_mode; _sandbox_mode="$(_detect_sandbox_mode)"
    echo -e "  ${CYAN}[沙箱配置]${NC} $_sandbox_mode"
    echo ""

    local sandbox_dir="$PROJECT_DIR/tests/e2e"

    if [ ! -d "$sandbox_dir" ]; then
        echo -e "  ${YELLOW}⚠ 跳过沙箱测试（tests/e2e 不存在）${NC}"
        echo ""
        return
    fi

    # ── DYLD/LD_LIBRARY_PATH 保护（同 Bridge 测试） ──
    local _saved_dyld="${DYLD_LIBRARY_PATH:-}"
    local _saved_ld="${LD_LIBRARY_PATH:-}"
    unset DYLD_LIBRARY_PATH
    unset LD_LIBRARY_PATH

    # 1) LocalSandbox 测试（始终执行）
    local _pytest_out; _pytest_out="$(mktemp)"
    echo -e "  ${CYAN}[pytest]${NC} LocalSandbox E2E..."
    if uv run --directory "$PROJECT_DIR" pytest \
        "$sandbox_dir/sandbox_local_test.py" \
        -v --no-header 2>&1 | tee "$_pytest_out" | tail -5; then
        echo -e "  ${GREEN}✓ LocalSandbox 测试通过${NC}"
        SANDBOX_PASS=$((SANDBOX_PASS + 1))
    else
        echo -e "  ${RED}✗ LocalSandbox 测试失败${NC}"
        echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
        cat "$_pytest_out"
        SANDBOX_FAIL=$((SANDBOX_FAIL + 1))
    fi
    rm -f "$_pytest_out"
    echo ""

    # 2) DockerSandbox 测试（可选，需 DOCKER_SANDBOX_ENABLED=1）
    if [ "${DOCKER_SANDBOX_ENABLED:-}" = "1" ]; then
        echo -e "  ${CYAN}[pytest]${NC} DockerSandbox E2E..."
        local _pytest_out; _pytest_out="$(mktemp)"
        if uv run --directory "$PROJECT_DIR" pytest \
            "$sandbox_dir/sandbox_docker_test.py" \
            -v --no-header 2>&1 | tee "$_pytest_out" | tail -5; then
            echo -e "  ${GREEN}✓ DockerSandbox 测试通过${NC}"
            SANDBOX_PASS=$((SANDBOX_PASS + 1))
        else
            echo -e "  ${RED}✗ DockerSandbox 测试失败${NC}"
            echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
            cat "$_pytest_out"
            SANDBOX_FAIL=$((SANDBOX_FAIL + 1))
        fi
        rm -f "$_pytest_out"

        # 3) 跨模式一致性测试（Docker 可用时执行）
        echo -e "  ${CYAN}[pytest]${NC} 跨模式一致性测试..."
        local _pytest_out; _pytest_out="$(mktemp)"
        if uv run --directory "$PROJECT_DIR" pytest \
            "$sandbox_dir/sandbox_consistency_test.py" \
            -v --no-header 2>&1 | tee "$_pytest_out" | tail -5; then
            echo -e "  ${GREEN}✓ 一致性测试通过${NC}"
            SANDBOX_PASS=$((SANDBOX_PASS + 1))
        else
            echo -e "  ${RED}✗ 一致性测试失败${NC}"
            echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
            cat "$_pytest_out"
            SANDBOX_FAIL=$((SANDBOX_FAIL + 1))
        fi
        rm -f "$_pytest_out"
    else
        echo -e "  ${YELLOW}⚠ 跳过 DockerSandbox（设置 DOCKER_SANDBOX_ENABLED=1 启用）${NC}"
    fi
    echo ""

    # 3) e2b Sandbox 全生命周期测试（需 E2B_API_KEY 或配置文件有 e2b_api_key）
    # 验证：create → upload → kill 全链路（fork 降级日志）
    local _has_e2b_key=false
    [ -n "${E2B_API_KEY:-}" ] && _has_e2b_key=true
    if ! $_has_e2b_key && [ -f "$PROJECT_DIR/configs/test/agentflowd.json" ]; then
        _cfg_key=$(python3 -c "
import json
with open('$PROJECT_DIR/configs/test/agentflowd.json') as f:
    cfg = json.load(f)
print(cfg.get('sandbox', {}).get('e2b_api_key', '') or '')
" 2>/dev/null) || true
        [ -n "$_cfg_key" ] && _has_e2b_key=true
    fi
    if $_has_e2b_key; then
        echo -e "  ${CYAN}[e2b_smoke]${NC} e2b Sandbox 全生命周期..."
        local _e2b_out; _e2b_out="$(mktemp)"
        if bash "$PROJECT_DIR/scripts/tests/e2b_fork_smoke.sh" 2>&1 | tee "$_e2b_out" | tail -3; then
            echo -e "  ${GREEN}✓ e2b Sandbox 测试通过${NC}"
            SANDBOX_PASS=$((SANDBOX_PASS + 1))
        else
            echo -e "  ${RED}✗ e2b Sandbox 测试失败${NC}"
            echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
            cat "$_e2b_out"
            SANDBOX_FAIL=$((SANDBOX_FAIL + 1))
        fi
        rm -f "$_e2b_out"

        # e2b 冷启动性能基准：create + skills 同步分段计时，阈值断言防回归
        echo -e "  ${CYAN}[perf_cold_start]${NC} e2b 冷启动性能基准..."
        local _perf_py="$PROJECT_DIR/frontend/bridge/.venv_unix/bin/python3"
        if [ ! -x "$_perf_py" ]; then
            echo -e "  ${YELLOW}⚠ 跳过冷启动基准（venv python 不存在: $_perf_py）${NC}"
        else
            local _perf_out; _perf_out="$(mktemp)"
            if "$_perf_py" "$PROJECT_DIR/tests/perf/perf_cold_start.py" 2>&1 | tee "$_perf_out" | tail -3; then
                echo -e "  ${GREEN}✓ e2b 冷启动性能基准通过${NC}"
                SANDBOX_PASS=$((SANDBOX_PASS + 1))
            else
                echo -e "  ${RED}✗ e2b 冷启动性能基准失败（可能回归）${NC}"
                echo -e "  ${YELLOW}━━ 完整输出 ━━${NC}"
                cat "$_perf_out"
                SANDBOX_FAIL=$((SANDBOX_FAIL + 1))
            fi
            rm -f "$_perf_out"
        fi
    else
        echo -e "  ${YELLOW}⚠ 跳过 e2b Sandbox（设置 E2B_API_KEY 环境变量启用）${NC}"
    fi
    echo ""

    # ── 清理：确保没有任何 sandbox_http_server.py 残留进程 ──
    # 设置 SANDBOX_PRESERVE=1 可保留现场（容器 + 日志）用于调试
    if [ "${SANDBOX_PRESERVE:-}" != "1" ]; then
        local _leftover
        _leftover=$(pgrep -f "sandbox_http_server" 2>/dev/null || true)
        if [ -n "$_leftover" ]; then
            echo -e "  ${YELLOW}[cleanup]${NC} 清理残留的 sandbox_http_server 进程..."
            pkill -f "sandbox_http_server" 2>/dev/null || true
            sleep 1
            _leftover=$(pgrep -f "sandbox_http_server" 2>/dev/null || true)
            if [ -n "$_leftover" ]; then
                echo -e "  ${YELLOW}[cleanup]${NC} 强制终止残留进程..."
                pkill -9 -f "sandbox_http_server" 2>/dev/null || true
            fi
        fi

        # ── 清理 Docker 残留容器 ──
        if command -v docker &>/dev/null; then
            local _docker_leftover
            _docker_leftover=$(_timeout 10 docker ps -a --filter "name=agentflow-sandbox" --format "{{.ID}}" 2>/dev/null || true)
            if [ -n "$_docker_leftover" ]; then
                echo -e "  ${YELLOW}[cleanup]${NC} 清理残留的 Docker 沙箱容器..."
                echo "$_docker_leftover" | while read -r _cid; do
                    echo -e "    ${YELLOW}→${NC} 停止并移除容器 ${_cid}..."
                    _timeout 10 docker stop "$_cid" 2>/dev/null || true
                    _timeout 10 docker rm -f "$_cid" 2>/dev/null || true
                done
            fi
        fi
    else
        echo -e "  ${YELLOW}[preserve]${NC} SANDBOX_PRESERVE=1，跳过清理"
        echo -e "  ${YELLOW}         ${NC} 残留的 sandbox_http_server 进程和 Docker 容器已保留供调试"
    fi

    # ── 恢复 DYLD/LD_LIBRARY_PATH ──
    if [ -n "$_saved_dyld" ]; then
        export DYLD_LIBRARY_PATH="$_saved_dyld"
    else
        unset DYLD_LIBRARY_PATH
    fi
    if [ -n "$_saved_ld" ]; then
        export LD_LIBRARY_PATH="$_saved_ld"
    else
        unset LD_LIBRARY_PATH
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    # ── 日志持久化 ──
    # 创建 logs/ 目录，按时间戳保存完整输出，同时仍显示在终端
    local _log_dir="$PROJECT_DIR/logs"
    mkdir -p "$_log_dir"
    local _log_file="$_log_dir/regression_$(date +%Y%m%d_%H%M%S).log"
    # 将 stdout/stderr 同时写入终端和日志文件（去掉颜色转义序列以保日志可读）
    # 使用 Python 逐行处理，避免 BSD sed 块缓冲导致管道死锁
    exec > >(tee >(python3 -c '
import sys, re
for line in sys.stdin:
    sys.stdout.write(re.sub(r"\x1b\[[0-9;]*m", "", line))
    sys.stdout.flush()
' >> "$_log_file")) 2>&1

    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  AgentFlow 回归测试${NC}"
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "项目目录: ${CYAN}$PROJECT_DIR${NC}"
    echo -e "日志文件: ${CYAN}$_log_file${NC}"
    echo ""
    echo -e "${BOLD}配置提示:${NC}"
    echo -e "  ${CYAN}DOCKER_SANDBOX_ENABLED=1${NC}    启用 Docker 沙箱 E2E 测试"
    echo -e "  ${CYAN}SANDBOX_PRESERVE=1${NC}          保留沙箱测试现场（不清理进程/容器）"
    echo ""

    # ── 启动时沙箱预检：检查开发 daemon 的沙箱状态 ──
    local _dev_daemon_log="$_log_dir/agentflowd.log"
    if [ -f "$_dev_daemon_log" ]; then
        echo -e "${BOLD}开发环境 daemon 沙箱状态:${NC}"
        if _check_sandbox_logs "$_dev_daemon_log" "开发 daemon"; then
            :  # ok
        else
            SANDBOX_ALERT=true
        fi
        echo ""
    fi

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
        run_sandbox_e2e_tests || true
    fi

    # -----------------------------------------------------------------------
    # 最终汇总
    # -----------------------------------------------------------------------
    local elapsed=$(($(date +%s) - START_TIME))
    local total_pass=$((UNIT_PASS + E2E_PASS + (FRONTEND_PASS) + (BRIDGE_PASS) + (SANDBOX_PASS)))
    local total_fail=$((UNIT_FAIL + E2E_FAIL + FRONTEND_FAIL + BRIDGE_FAIL + SANDBOX_FAIL))
    local total_all=$((total_pass + total_fail))

    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  测试结果汇总${NC}"
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "  单元:       ${GREEN}${UNIT_PASS} 通过${NC} / ${RED}${UNIT_FAIL} 失败${NC}"
    echo -e "  E2E:        ${GREEN}${E2E_PASS} 通过${NC} / ${RED}${E2E_FAIL} 失败${NC}"
    echo -e "  Frontend:   ${GREEN}${FRONTEND_PASS:-0} 通过${NC} / ${RED}${FRONTEND_FAIL:-0} 失败${NC}"
    echo -e "  Bridge:     ${GREEN}${BRIDGE_PASS} 通过${NC} / ${RED}${BRIDGE_FAIL} 失败${NC}"
    echo -e "  Sandbox:    ${GREEN}${SANDBOX_PASS:-0} 通过${NC} / ${RED}${SANDBOX_FAIL:-0} 失败${NC}"
    echo -e "  ───────────────────────────"
    echo -e "  合计:       ${GREEN}${total_pass} 通过${NC} / ${RED}${total_fail} 失败${NC} (共 ${total_all})"
    echo -e "  耗时: ${elapsed}s"
    echo ""

    # ── 测试 daemon/bridge 日志目录索引 ──
    # 由 e2e_helpers.py 的 _test_log_dir() + bridge_daemon fixture 落盘到
    # logs/tests/<ts>_<cls>_<fixture>/{daemon.log,bridge.log}
    # 在汇总里列出本轮所有子目录,方便定位失败现场
    local _tests_log_root="$_log_dir/tests"
    if [ -d "$_tests_log_root" ]; then
        echo -e "  ${BOLD}测试 daemon/bridge 日志目录${NC} (${CYAN}$_tests_log_root${NC}):"
        # 列出本轮回归期间（START_TIME 之后）创建的子目录
        # 注意：BSD find（macOS 默认）不支持 GNU find 的 `-newermt "@epoch"` 语法，会报
        # `find: Can't parse date/time: @1784398501`。改用 BSD 兼容方式：先 touch 建一个
        # 时间参考文件（epoch 秒），再用 `-newer` 比参考文件新。GNU find 也支持 `-newer`，
        # 所以这个写法跨 BSD/GNU 都工作。
        # 列出本轮回归期间（START_TIME 之后）创建的子目录
        # 注意：BSD find（macOS 默认）不支持 GNU find 的 `-newermt "@epoch"` 语法，会报
        # `find: Can't parse date/time: @1784398501`；BSD touch 也不支持 `touch -d "@epoch"`
        # （报 `out of range or illegal time specification`）。改用 Python 一次性过滤子目录
        # （比较 os.path.getmtime > START_TIME），避免 BSD/GNU touch/find 的所有跨平台差异。
        if [ -n "$START_TIME" ]; then
            python3 - "$_tests_log_root" "$START_TIME" <<'PYEOF' | sort | while read -r _sub; do
import os, sys
root, start = sys.argv[1], float(sys.argv[2])
if not os.path.isdir(root):
    sys.exit(0)
for name in os.listdir(root):
    sub = os.path.join(root, name)
    if os.path.isdir(sub) and os.path.getmtime(sub) > start:
        print(sub)
PYEOF
                local _daemon_log="${_sub}/daemon.log"
                local _bridge_log="${_sub}/bridge.log"
                local _daemon_size=""
                local _bridge_size=""
                [ -f "$_daemon_log" ] && _daemon_size=$(wc -c < "$_daemon_log" | tr -d ' ')
                [ -f "$_bridge_log" ] && _bridge_size=$(wc -c < "$_bridge_log" | tr -d ' ')
                echo -e "    ${CYAN}$(basename "$_sub")${NC}  daemon=${_daemon_size:-0}B  bridge=${_bridge_size:-0}B"
            done
        else
            # START_TIME 缺失兜底：列出所有子目录（不按时间过滤）
            find "$_tests_log_root" -maxdepth 1 -mindepth 1 -type d | sort | while read -r _sub; do
                local _daemon_log="${_sub}/daemon.log"
                local _bridge_log="${_sub}/bridge.log"
                local _daemon_size=""
                local _bridge_size=""
                [ -f "$_daemon_log" ] && _daemon_size=$(wc -c < "$_daemon_log" | tr -d ' ')
                [ -f "$_bridge_log" ] && _bridge_size=$(wc -c < "$_bridge_log" | tr -d ' ')
                echo -e "    ${CYAN}$(basename "$_sub")${NC}  daemon=${_daemon_size:-0}B  bridge=${_bridge_size:-0}B"
            done
        fi
        echo ""
    fi

    # ── 沙箱运行时告警 ──
    # 扫描本轮测试 daemon 日志，检查沙箱是否真正生效
    local _sandbox_alert_shown=false
    if [ -d "$_tests_log_root" ] && [ -n "$START_TIME" ]; then
        while IFS= read -r _alert_sub; do
            local _alert_daemon="${_alert_sub}/daemon.log"
            if [ -f "$_alert_daemon" ] && grep -q "Failed to launch sandbox" "$_alert_daemon" 2>/dev/null; then
                if ! $_sandbox_alert_shown; then
                    echo -e "  ${BOLD}${RED}━━━ 沙箱运行时告警 ━━━${NC}"
                    _sandbox_alert_shown=true
                fi
                local _alert_name; _alert_name=$(basename "$_alert_sub")
                # 使用 Python 提取（不支持 -P）
                local _alert_info
                _alert_info=$(python3 -c "
import re,sys
with open('$_alert_daemon') as f:
    content = f.read()
mode = ''
m = re.search(r'mode=([^,]+)', content)
if m: mode = m.group(1)
reasons = sorted(set(re.findall(r'Failed to launch sandbox.*?: ([^,]+)', content)))
isolated = content.count('continuing without sandbox isolation')
print('{}||{}||{}'.format(mode, '; '.join(reasons), isolated))
" 2>/dev/null || echo "?|||0")
                local _alert_mode; _alert_mode="${_alert_info%%||*}"
                local _remain="${_alert_info#*||}"
                local _alert_reason; _alert_reason="${_remain%%||*}"
                _remain="${_remain#*||}"
                local _alert_isolated; _alert_isolated="${_remain}"
                echo -e "  ${RED}╔══════════════════════════════════════════════════╗${NC}"
                echo -e "  ${RED}║${NC}  测试: ${CYAN}${_alert_name}${NC}"
                echo -e "  ${RED}║${NC}  mode=${_alert_mode}  |  启动失败: 有  |  无隔离请求: ${_alert_isolated}"
                echo -e "  ${RED}║${NC}  原因: ${_alert_reason}"
                echo -e "  ${RED}╚══════════════════════════════════════════════════╝${NC}"
                SANDBOX_ALERT=true
            fi
        done < <(python3 - "$_tests_log_root" "$START_TIME" <<'PYEOF'
import os, sys
root, start = sys.argv[1], float(sys.argv[2])
if os.path.isdir(root):
    for name in os.listdir(root):
        sub = os.path.join(root, name)
        if os.path.isdir(sub) and os.path.getmtime(sub) > start:
            print(sub)
PYEOF
)
    fi

    # ── 汇总沙箱告警状态 ──
    if $SANDBOX_ALERT; then
        echo ""
        echo -e "  ${RED}${BOLD}⚠ 沙箱告警: 有沙箱未正常运行，技能在无隔离环境下执行${NC}"
        echo -e "  ${YELLOW}  解决方法: 设置 E2B_API_KEY 环境变量后重启 daemon${NC}"
        echo ""
    fi

    if [ "$total_fail" -eq 0 ]; then
        echo -e "  ${GREEN}${BOLD}✅ 全部通过！${NC}"
        exit 0
    else
        echo -e "  ${RED}${BOLD}❌ 有 ${total_fail} 个测试失败${NC}"
        exit 1
    fi
}

main
