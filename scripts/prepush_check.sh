#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════
# 推送前检查 — 本地快速发现常见问题
# ═══════════════════════════════════════════════════════════════════════
#
# 用法：
#   bash scripts/prepush_check.sh          # 检查所有
#   bash scripts/prepush_check.sh --quick  # 只查改动的文件

# 加载环境配置
_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../configs" && pwd)" 2>/dev/null || _ENV_DIR=""
_ENV="${AGENTFLOW_ENV:-dev}"
if [ -n "$_ENV_DIR" ] && [ -f "$_ENV_DIR/$_ENV/.env" ]; then set -a; source "$_ENV_DIR/$_ENV/.env"; set +a; fi
unset _ENV_DIR _ENV

#
# 检查项：
#   1. 已跟踪源码中的硬编码凭据
#   2. Python 语法
#   3. vendor/SysAgent 头文件自包含性（缺少 include）
#   4. F7.1 外部调用落盘合规
# ═══════════════════════════════════════════════════════════════════════

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PASS=0
FAIL=0

check() {
    local name="$1"
    shift
    if "$@" 2>&1; then
        echo "  ✅ $name"
        PASS=$((PASS + 1))
    else
        echo "  ❌ $name"
        FAIL=$((FAIL + 1))
    fi
}

echo "═══════════════════════════════════════════════════════════════"
echo "  🔍 推送前检查"
echo "═══════════════════════════════════════════════════════════════"
echo ""

QUICK="${1:-}"

# ── 1. 硬编码凭据 ───────────────────────────────────────────────────
echo "--- 凭据安全 ---"
if [ -f "$PROJECT_ROOT/scripts/quality/scan_secrets.py" ]; then
    check "工作区无硬编码凭据" python3 "$PROJECT_ROOT/scripts/quality/scan_secrets.py" --tracked
    if ! git -C "$PROJECT_ROOT" diff --cached --quiet; then
        check "暂存区无硬编码凭据" python3 "$PROJECT_ROOT/scripts/quality/scan_secrets.py" --staged
    fi
else
    echo "  ❌ scan_secrets.py 不存在"
    FAIL=$((FAIL + 1))
fi

# ── 2. Python 语法 ──────────────────────────────────────────────────
echo "--- Python 语法 ---"
PY_FILES=()
if [ "$QUICK" = "--quick" ]; then
    while IFS= read -r -d '' f; do PY_FILES+=("$f"); done < <(
        git diff HEAD --name-only --diff-filter=ACM | grep '\.py$' | tr '\n' '\0'
    )
else
    while IFS= read -r -d '' f; do PY_FILES+=("$f"); done < <(
        find "$PROJECT_ROOT/examples" "$PROJECT_ROOT/frontend" "$PROJECT_ROOT/tests" \
             -name "*.py" \
             -not -path "*/__pycache__/*" \
             -not -path "*/.venv/*" \
             -not -path "*/venv/*" \
             -not -path "*/site-packages/*" \
             -not -path "*/node_modules/*" \
             -not -path "*/.git/*" \
             -not -path "*/build/*" \
             -print0 2>/dev/null
    )
fi

if [ ${#PY_FILES[@]} -eq 0 ]; then
    echo "  ⏭️  无 Python 文件需检查"
else
    for f in "${PY_FILES[@]}"; do
        check "$(basename "$f")" python3 -c "
import py_compile
py_compile.compile('$f', doraise=True)
"
    done
fi

# ── 3. vendor/SysAgent 头文件自包含性 ──────────────────────────────
echo ""
echo "--- vendor/SysAgent 头文件检查 ---"

SYSAGENT_INCLUDE="$PROJECT_ROOT/vendor/SysAgent/include"
if [ -d "$SYSAGENT_INCLUDE" ]; then
    SYSAGENT_HEADERS=()
    if [ "$QUICK" = "--quick" ]; then
        while IFS= read -r -d '' f; do SYSAGENT_HEADERS+=("$f"); done < <(
            git diff HEAD --name-only --diff-filter=ACM |
                grep '^vendor/SysAgent/include/.*\.h$' | tr '\n' '\0'
        )
    else
        while IFS= read -r -d '' f; do SYSAGENT_HEADERS+=("$f"); done < <(
            find "$SYSAGENT_INCLUDE" -name "*.h" -print0 2>/dev/null
        )
    fi

    if [ ${#SYSAGENT_HEADERS[@]} -eq 0 ]; then
        echo "  ⏭️  无 SysAgent 头文件需检查"
    else
        for f in "${SYSAGENT_HEADERS[@]}"; do
            rel="${f#$PROJECT_ROOT/}"
            # 检查常见的标准库 include 遗漏
            # 规则：如果使用了 std::once_flag，必须包含 <mutex>
            if grep -q "std::once_flag" "$f" 2>/dev/null; then
                if ! grep -q '#include.*<mutex>' "$f" 2>/dev/null; then
                    echo "  ❌ $rel — 使用了 std::once_flag 但缺少 #include <mutex>"
                    FAIL=$((FAIL + 1))
                    continue
                fi
            fi
            # 规则：如果使用了 std::lock_guard / std::unique_lock，必须包含 <mutex>
            if grep -q "std::lock_guard\|std::unique_lock" "$f" 2>/dev/null; then
                if ! grep -q '#include.*<mutex>' "$f" 2>/dev/null; then
                    echo "  ❌ $rel — 使用了 std::lock_guard 但缺少 #include <mutex>"
                    FAIL=$((FAIL + 1))
                    continue
                fi
            fi
            # 规则：如果使用了 std::vector，必须包含 <vector>
            if grep -q "std::vector" "$f" 2>/dev/null; then
                if ! grep -q '#include.*<vector>' "$f" 2>/dev/null; then
                    echo "  ❌ $rel — 使用了 std::vector 但缺少 #include <vector>"
                    FAIL=$((FAIL + 1))
                    continue
                fi
            fi
            # 规则：如果使用了 std::map，必须包含 <map>
            if grep -q "std::map" "$f" 2>/dev/null; then
                if ! grep -q '#include.*<map>' "$f" 2>/dev/null; then
                    echo "  ❌ $rel — 使用了 std::map 但缺少 #include <map>"
                    FAIL=$((FAIL + 1))
                    continue
                fi
            fi
            # 规则：如果使用了 std::thread，必须包含 <thread>
            if grep -q "std::thread" "$f" 2>/dev/null; then
                if ! grep -q '#include.*<thread>' "$f" 2>/dev/null; then
                    echo "  ❌ $rel — 使用了 std::thread 但缺少 #include <thread>"
                    FAIL=$((FAIL + 1))
                    continue
                fi
            fi
            echo "  ✅ $rel"
            PASS=$((PASS + 1))
        done
    fi
else
    echo "  ⏭️  vendor/SysAgent 目录不存在"
fi

# ── 4. F7.1 外部调用落盘合规检查 ─────────────────────────────────
echo ""
echo "--- F7.1 外部调用落盘合规检查 ---"

if [ -f "$PROJECT_ROOT/scripts/quality/scan_f71.py" ]; then
    check "F7.1 外部调用落盘" python3 "$PROJECT_ROOT/scripts/quality/scan_f71.py" --quick
else
    echo "  ⏭️  scan_f71.py 不存在"
fi

# ── 4. 总结 ───────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "  结果: $PASS 通过, $FAIL 失败"
echo "═══════════════════════════════════════════════════════════════"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "❌ 存在失败项，请修复后重新检查"
    exit 1
fi
echo "✅ 全部通过"
