#!/bin/bash
# scripts/start_upstream_mock.sh
# 启动 CoreMind 上游 Mock 服务器（Sidecar 模拟用）
#
# 前置条件:
#   bridge/daemon/sidecar 已运行
#
# 这个 Mock 服务器模拟 CoreMind 上游平台:
#   1. 接收前端发送指令（POST /send）
#   2. 构造 AccessCtrl 请求发送到 sidecar
#   3. 接收 sidecar 的异步回调
#   4. 通过 SSE 推回前端

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MOCK_HOST="${UPSTREAM_MOCK_HOST:-127.0.0.1}"
MOCK_PORT="${UPSTREAM_MOCK_PORT:-23768}"

# 日志目录
LOG_DIR="${PROJECT_ROOT}/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="${LOG_DIR}/upstream_mock.log"

RELOAD=""
for arg in "$@"; do
    if [ "$arg" = "--reload" ]; then
        RELOAD="--reload"
    fi
done

echo "[upstream-mock] Starting on $MOCK_HOST:$MOCK_PORT ..."
echo "[upstream-mock] Logging to $LOG_FILE"
cd "$PROJECT_ROOT"

# 端口占用则自动杀掉旧进程
if lsof -i :$MOCK_PORT > /dev/null 2>&1; then
    OLD_PID=$(lsof -ti :$MOCK_PORT)
    echo "[upstream-mock] Port $MOCK_PORT in use by PID $OLD_PID, killing..."
    kill "$OLD_PID" 2>/dev/null
    sleep 1
    # 如未释放则强制
    if lsof -i :$MOCK_PORT > /dev/null 2>&1; then
        kill -9 "$OLD_PID" 2>/dev/null
        sleep 1
    fi
    echo "[upstream-mock] Old process killed"
fi

# 确保依赖可用（FastAPI + uvicorn）
if ! python3 -c "import fastapi, uvicorn" 2>/dev/null; then
    echo "[upstream-mock] Installing dependencies..."
    pip install fastapi uvicorn httpx 2>&1 | tail -1
fi

if [ -n "$RELOAD" ]; then
    # --reload 需要 import string 格式
    exec python3 -m uvicorn frontend.mocks.upstream_mock.server:app \
        --host "$MOCK_HOST" --port "$MOCK_PORT" --reload \
        >> "$LOG_FILE" 2>&1
else
    exec python3 frontend/mocks/upstream_mock/server.py \
        >> "$LOG_FILE" 2>&1
fi
