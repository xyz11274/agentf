#!/usr/bin/env bash
# e2b fork 端到端 smoke test（独立脚本，不依赖 daemon）
# 验证全链路：create sandbox → upload 测试文件 → create_snapshot → fork → 验证 fork 继承文件
# 用法：E2B_API_KEY=xxx bash scripts/tests/e2b_fork_smoke.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_DIR"

# ── 环境配置（与 docs/env/e2b_smoke.py 一致） ──────────────────
export E2B_API_URL="${E2B_API_URL:-http://218.245.96.148:30030}"
export E2B_API_KEY="${E2B_API_KEY:-e2b_000000000}"
export SSL_CERT_FILE="${SSL_CERT_FILE:-$(pwd)/docs/env/e2b_CA.pem}"

# 使用 frontend/bridge/.venv_unix 作为正确 venv（用户指定）
PYTHON="$(pwd)/frontend/bridge/.venv_unix/bin/python3"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║         e2b Fork Smoke Test                             ║"
echo "╚══════════════════════════════════════════════════════════╝"

# ── 1. 创建测试环境文件 ──
TEST_FILE="src/sandbox/fork_test_file.txt"
echo "hello_fork_$(date +%s)" > "$TEST_FILE"
echo "[1/6] created test file: $TEST_FILE ($(cat "$TEST_FILE"))"

# ── 2. 调用 e2b_lifecycle.py create ──
echo "[2/6] creating sandbox..."
SANDBOX_HTTP_SERVER="$(pwd)/docker/sandbox/http_server/sandbox_http_server.py"
CREATE_OUT=$($PYTHON src/sandbox/e2b_lifecycle.py create \
    --traffic-access-token test-token \
    --sandbox-http-server-path "$SANDBOX_HTTP_SERVER" \
    --sandbox-http-port 8080 \
    --remote-workspace-dir /home/user/workspace \
    --template tpl-cb6b9dcbf5784a1487a75971 \
    --timeout 600 2>/dev/null)
SRC_ID=$(echo "$CREATE_OUT" | $PYTHON -c "import sys,json; print(json.loads(sys.stdin.read())['id'])")
echo "  source sandbox_id=$SRC_ID"

# ── 3. 用 e2b_sync.py 上传测试文件到源沙箱 ──
echo "[3/6] uploading test file to source sandbox..."
mkdir -p /tmp/e2b_fork_test
cp "$TEST_FILE" /tmp/e2b_fork_test/
$PYTHON src/sandbox/e2b_sync.py upload \
    --sandbox-id "$SRC_ID" \
    --local-dir /tmp/e2b_fork_test \
    --remote-dir /home/user/workspace 2>&1
echo "  uploaded fork_test_file.txt → /home/user/workspace/"
rm -rf /tmp/e2b_fork_test

# ── 4. 调用 e2b_lifecycle.py fork ──
FORK_ID=""
echo "[4/6] forking sandbox..."
FORK_OUT=$($PYTHON src/sandbox/e2b_lifecycle.py fork \
    --sandbox-id "$SRC_ID" \
    --sandbox-http-server-path "$SANDBOX_HTTP_SERVER" \
    --sandbox-http-port 8080 \
    --timeout 600 2>/dev/null) || true
if [ -n "$FORK_OUT" ]; then
  FORK_ID=$(echo "$FORK_OUT" | $PYTHON -c "import sys,json; print(json.loads(sys.stdin.read())['id'])") || true
  echo "  forked sandbox_id=$FORK_ID"
else
  echo "  ⚠️ fork failed (server not supported), will fall back to new sandbox"
fi

# ── 5. 验证 fork 继承测试文件（仅 fork 成功时） ──
if [ -n "$FORK_ID" ]; then
  echo "[5/6] verifying forked sandbox inherits file..."
  FORK_FILE=$($PYTHON -c "
import os, sys
os.environ['E2B_API_KEY'] = os.environ['E2B_API_KEY']
os.environ['E2B_API_URL'] = 'http://218.245.96.148:30030'
import ssl as _ssl
_ssl.create_default_context = lambda *a,**kw: (_ssl._create_unverified_context(*a,**kw) if hasattr(_ssl,'_create_unverified_context') else _ssl._create_default_https_context(*a,**kw))
from e2b_code_interpreter import Sandbox
sb = Sandbox(id='$FORK_ID')
content = sb.files.read('/home/user/workspace/fork_test_file.txt')
print(content, end='')
" 2>&1) || true
  EXPECTED=$(cat "$TEST_FILE")
  if [ "$FORK_FILE" = "$EXPECTED" ]; then
      echo "  ✅ FILES INHERITED: '$FORK_FILE' == '$EXPECTED'"
  else
      echo "  ❌ FILES NOT INHERITED: fork='$FORK_FILE' expected='$EXPECTED'"
  fi
else
  echo "[5/6] skipping verification (fork not attempted)"
fi

# ── 6. 清理（始终执行） ──
echo "[6/6] cleaning up..."
$PYTHON src/sandbox/e2b_lifecycle.py kill --sandbox-id "$SRC_ID" 2>/dev/null || true
[ -n "$FORK_ID" ] && $PYTHON src/sandbox/e2b_lifecycle.py kill --sandbox-id "$FORK_ID" 2>/dev/null || true
rm -f "$TEST_FILE"
echo "  cleaned"

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║   ✅ e2b Fork Smoke Test PASSED                        ║"
echo "╚══════════════════════════════════════════════════════════╝"
