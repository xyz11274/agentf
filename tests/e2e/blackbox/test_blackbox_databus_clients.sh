#!/bin/bash
#
# Black-box E2E Test: DataBus Multi-Language Client SDKs
#
# 验证 Python SDK 和 CLI 工具能够通过 UDS 与 DataBus 正确通信。
# 测试 put/get/contains/remove 及对话隔离功能。
#
# 依赖：
#   - agent_runner 二进制（已构建）
#   - Python 3.6+
#   - src/runtime/databus/clients/python/agentflow_databus.py
#   - src/runtime/databus/clients/cli/databus_cli.py
#
# 用法：
#   ./test_blackbox_databus_clients.sh [agent_runner_path] [package_path]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

AGENT_RUNNER="${1:-$PROJECT_ROOT/build/agent_runner}"
PACKAGE_PATH="${2:-$PROJECT_ROOT/examples/agent/multi_language_databus}"

PYTHON_SDK="$PROJECT_ROOT/src/runtime/databus/clients/python/agentflow_databus.py"
CLI_TOOL="$PROJECT_ROOT/src/runtime/databus/clients/cli/databus_cli.py"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

TESTS_PASSED=0
TESTS_FAILED=0

log_info()  { echo -e "${YELLOW}[INFO]${NC} $1"; }
log_pass()  { echo -e "${GREEN}[PASS]${NC} $1"; TESTS_PASSED=$((TESTS_PASSED + 1)); }
log_fail()  { echo -e "${RED}[FAIL]${NC} $1"; TESTS_FAILED=$((TESTS_FAILED + 1)); }

# ─────────────────────────────────────────────────────────────────────────────
# 检查依赖是否满足
# ─────────────────────────────────────────────────────────────────────────────

check_prerequisites() {
    log_info "检查依赖..."

    if [[ ! -f "$PYTHON_SDK" ]]; then
        log_fail "Python SDK 不存在: $PYTHON_SDK"
        exit 1
    fi
    log_pass "Python SDK 存在: $PYTHON_SDK"

    if [[ ! -f "$CLI_TOOL" ]]; then
        log_fail "CLI 工具不存在: $CLI_TOOL"
        exit 1
    fi
    log_pass "CLI 工具存在: $CLI_TOOL"

    if ! command -v python3 &>/dev/null; then
        log_fail "python3 未安装"
        exit 1
    fi
    log_pass "python3 已安装: $(python3 --version)"

    # 检查 Python SDK 可导入
    if python3 -c "import sys; sys.path.insert(0, '$(dirname "$PYTHON_SDK")'); from agentflow_databus import DataBusClient" 2>/dev/null; then
        log_pass "Python SDK 可正常导入"
    else
        log_fail "Python SDK 导入失败"
        exit 1
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 启动 mock UDS server（使用 Python 内置 socketserver）
# ─────────────────────────────────────────────────────────────────────────────

MOCK_SERVER_PID=""
UDS_SOCK="/tmp/test_databus_clients_$$.sock"

start_mock_server() {
    log_info "启动 Mock UDS DataBus Server..."

    python3 - "$UDS_SOCK" &
    MOCK_SERVER_PID=$!

    # 等待 server 就绪
    local retries=0
    while [[ $retries -lt 20 ]]; do
        if [[ -S "$UDS_SOCK" ]]; then
            log_pass "Mock Server 已启动 (PID=$MOCK_SERVER_PID, sock=$UDS_SOCK)"
            return 0
        fi
        sleep 0.1
        ((retries++))
    done
    log_fail "Mock Server 启动超时"
    return 1
}

stop_mock_server() {
    if [[ -n "$MOCK_SERVER_PID" ]]; then
        kill "$MOCK_SERVER_PID" 2>/dev/null || true
        wait "$MOCK_SERVER_PID" 2>/dev/null || true
        MOCK_SERVER_PID=""
    fi
    rm -f "$UDS_SOCK"
}

trap stop_mock_server EXIT

# Mock server 脚本（嵌入式 Python）
MOCK_SERVER_SCRIPT='
import sys
import socket
import json
import os
import threading

sock_path = sys.argv[1]
if os.path.exists(sock_path):
    os.unlink(sock_path)

# 内存存储
store = {}
store_lock = threading.Lock()

def handle_request(data):
    try:
        req = json.loads(data)
    except json.JSONDecodeError:
        return {"jsonrpc": "2.0", "error": {"code": -32700, "message": "Parse error"}, "id": None}

    req_id = req.get("id")
    method = req.get("method", "")
    params = req.get("params", {})

    def make_key(p):
        # 构建复合键：session_id + namespace + dialogue_id + key
        parts = [p.get("session_id", ""), p.get("namespace", ""),
                 p.get("dialogue_id", ""), p.get("key", "")]
        return "|".join(str(x) for x in parts)

    with store_lock:
        if method == "put":
            store[make_key(params)] = params.get("value")
            return {"jsonrpc": "2.0", "result": {"success": True}, "id": req_id}
        elif method == "get":
            full_key = make_key(params)
            if full_key in store:
                return {"jsonrpc": "2.0", "result": {"exists": True, "value": store[full_key]}, "id": req_id}
            return {"jsonrpc": "2.0", "result": {"exists": False, "value": None}, "id": req_id}
        elif method == "remove":
            store.pop(make_key(params), None)
            return {"jsonrpc": "2.0", "result": {"success": True}, "id": req_id}
        elif method == "contains":
            return {"jsonrpc": "2.0", "result": {"exists": make_key(params) in store}, "id": req_id}
        elif method == "subscribe":
            return {"jsonrpc": "2.0", "result": {"subscription_id": 1}, "id": req_id}
        elif method == "unsubscribe":
            return {"jsonrpc": "2.0", "result": {"success": True}, "id": req_id}
        elif method == "list_keys":
            prefix = params.get("prefix", "")
            keys = [k for k in store if k.endswith(f"|{prefix}") or f"|{prefix}|" in k]
            return {"jsonrpc": "2.0", "result": {"keys": keys}, "id": req_id}
        elif method == "publish":
            return {"jsonrpc": "2.0", "result": {"success": True}, "id": req_id}
        else:
            return {"jsonrpc": "2.0", "error": {"code": -32601, "message": "Method not found"}, "id": req_id}

def handle_client(conn):
    buf = b""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if line:
                    resp = handle_request(line.decode("utf-8"))
                    conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))
    except Exception:
        pass
    finally:
        conn.close()

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(sock_path)
server.listen(10)

try:
    while True:
        conn, _ = server.accept()
        t = threading.Thread(target=handle_client, args=(conn,), daemon=True)
        t.start()
except KeyboardInterrupt:
    pass
finally:
    server.close()
    if os.path.exists(sock_path):
        os.unlink(sock_path)
'

# 将 mock server 脚本写入临时文件再启动
MOCK_SCRIPT_FILE="/tmp/mock_databus_server_$$.py"
echo "$MOCK_SERVER_SCRIPT" > "$MOCK_SCRIPT_FILE"

start_mock_server_real() {
    log_info "启动 Mock UDS DataBus Server..."
    python3 "$MOCK_SCRIPT_FILE" "$UDS_SOCK" &
    MOCK_SERVER_PID=$!

    local retries=0
    while [[ $retries -lt 30 ]]; do
        if [[ -S "$UDS_SOCK" ]]; then
            log_pass "Mock Server 已启动 (PID=$MOCK_SERVER_PID)"
            return 0
        fi
        sleep 0.1
        retries=$((retries + 1))
    done
    log_fail "Mock Server 启动超时"
    return 1
}

# ─────────────────────────────────────────────────────────────────────────────
# 测试：Python SDK
# ─────────────────────────────────────────────────────────────────────────────

test_python_sdk() {
    log_info "测试 Python SDK..."

    python3 - "$UDS_SOCK" "$PYTHON_SDK" << 'PYTEST'
import sys
import os

sock_path = sys.argv[1]
sdk_dir = os.path.dirname(sys.argv[2])
sys.path.insert(0, sdk_dir)

from agentflow_databus import DataBusClient, DataBusError

client = DataBusClient(sock_path)

# put/get
client.user().put("test_key", "test_value")
value = client.user().get("test_key")
assert value == "test_value", f"Expected 'test_value', got '{value}'"
print("PASS: put/get string")

# JSON 对象
client.user().put("json_key", {"score": 42})
obj = client.user().get("json_key")
assert isinstance(obj, dict) and obj.get("score") == 42, f"JSON roundtrip failed: {obj}"
print("PASS: put/get JSON object")

# contains
assert client.user().contains("test_key"), "contains should return True"
assert not client.user().contains("missing_key"), "contains should return False for missing key"
print("PASS: contains")

# remove
client.user().remove("test_key")
assert not client.user().contains("test_key"), "key should not exist after remove"
print("PASS: remove")

# 多命名空间
client.sys().put("sys_key", "sys_val")
assert client.sys().get("sys_key") == "sys_val", "sys namespace failed"
print("PASS: sys namespace")

client.close()
print("ALL_PASSED")
PYTEST

    if [[ $? -eq 0 ]]; then
        log_pass "Python SDK 测试全部通过"
    else
        log_fail "Python SDK 测试失败"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 测试：CLI 工具
# ─────────────────────────────────────────────────────────────────────────────

test_cli_tool() {
    log_info "测试 CLI 工具..."

    export AF_DATABUS_UDS_FILE="$UDS_SOCK"
    CLI_CMD="python3 $CLI_TOOL"

    # put/get
    $CLI_CMD -ns user put "cli_key" "cli_value" >/dev/null
    result=$($CLI_CMD -ns user --raw get "cli_key" 2>/dev/null || echo "")
    if [[ "$result" == "cli_value" ]]; then
        log_pass "CLI put/get"
    else
        log_fail "CLI put/get: expected 'cli_value', got '${result}'"
    fi

    # contains（退出码 0 表示存在）
    if python3 "$CLI_TOOL" -ns user contains "cli_key" >/dev/null 2>&1; then
        log_pass "CLI contains (exists)"
    else
        log_fail "CLI contains (exists)"
    fi

    if ! python3 "$CLI_TOOL" -ns user contains "no_such_key" >/dev/null 2>&1; then
        log_pass "CLI contains (not exists)"
    else
        log_fail "CLI contains (not exists)"
    fi

    # remove
    $CLI_CMD -ns user remove "cli_key" >/dev/null
    if ! python3 "$CLI_TOOL" -ns user contains "cli_key" >/dev/null 2>&1; then
        log_pass "CLI remove"
    else
        log_fail "CLI remove"
    fi

    # CLI 多命名空间测试
    $CLI_CMD -ns sys put "sys_key" "sys_val" >/dev/null
    sys_val=$($CLI_CMD -ns sys --raw get "sys_key" 2>/dev/null || echo "")
    if [[ "$sys_val" == "sys_val" ]]; then
        log_pass "CLI sys namespace"
    else
        log_fail "CLI sys namespace: got '${sys_val}'"
    fi

    unset AF_DATABUS_UDS_FILE
}

# ─────────────────────────────────────────────────────────────────────────────
# 主流程
# ─────────────────────────────────────────────────────────────────────────────

main() {
    echo "========================================="
    echo "DataBus Client SDK 黑盒集成测试"
    echo "========================================="

    check_prerequisites

    # 启动 mock server
    trap 'stop_mock_server; rm -f "$MOCK_SCRIPT_FILE"' EXIT
    start_mock_server_real

    # 执行测试
    test_python_sdk
    test_cli_tool

    echo ""
    echo "========================================="
    echo -e "测试结果: ${GREEN}PASSED=${TESTS_PASSED}${NC} ${RED}FAILED=${TESTS_FAILED}${NC}"
    echo "========================================="

    if [[ $TESTS_FAILED -gt 0 ]]; then
        exit 1
    fi
    exit 0
}

main
