#!/bin/bash
# ── 启动 Mock UDS Model Server ──────────────────────────────────────
#
# 用于本地调试，模拟 UDS PluginServer 的协议行为。
# 默认使用回显模式，可通过参数修改。
#
# 用法：
#   bash ar_scripts/start_mock_uds_server.sh
#   bash ar_scripts/start_mock_uds_server.sh --mode fixed --response "hello"
#   bash ar_scripts/start_mock_uds_server.sh --socket /tmp/agentflow_plugin.sock
#
# 停止：Ctrl+C 或 kill $(cat /tmp/mock_uds_server.pid)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MOCK_SERVER="$PROJECT_ROOT/frontend/mocks/mock_uds_model_server/mock_uds_server.py"
PID_FILE="/tmp/mock_uds_server.pid"

# 清理旧进程
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "🔄 正在停止旧 Mock UDS Server (PID=$OLD_PID)..."
        kill "$OLD_PID" 2>/dev/null || true
        sleep 0.5
    fi
    rm -f "$PID_FILE"
fi

echo "🚀 启动 Mock UDS Model Server..."
echo "   脚本: $MOCK_SERVER"
echo "   参数: $*"

python3 "$MOCK_SERVER" "$@" &

MOCK_PID=$!
echo "$MOCK_PID" > "$PID_FILE"
echo "   PID: $MOCK_PID (已写入 $PID_FILE)"
echo ""
echo "💡 默认监听 /tmp/mock_agentflow.sock"
echo "   用 --socket 参数可自定义路径"
echo "   停止: kill $MOCK_PID"
echo ""
