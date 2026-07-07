#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════
# 端到端本地调试启动脚本
# ═══════════════════════════════════════════════════════════════════════
#
# 启动完整的调试链路：
#   Frontend (Vite:15173)
#     → Bridge (uvicorn:8199)
#       → AgentFlow Daemon (agentflowd:17891)
#         → Python Node
#           → LlmUdsClient (.sock)
#             → Mock UDS Server (Python UDS)     ← 模拟 UDSPluginServer
#               → [mock 模式] 回显/固定文本/模板
#               → [proxy 模式] FusionMind Mock (:8085) → 真实 LLM
#
# FusionMind Mock Server (:8085) 始终启动，支持两种子模式：
#   - mock 模式：返回预设响应（echo/fixed/template/tool_calls）
#   - proxy 模式：透传到真实 LLM（需配置 --upstream-url / --api-key）
#
# 用法：
#   bash ar_scripts/start_e2e.sh                                    # 默认 mock 模式
#   bash ar_scripts/start_e2e.sh --proxy                            # Proxy → DashScope
#   bash ar_scripts/start_e2e.sh --proxy \\
#       --upstream-url https://dashscope.aliyuncs.com/compatible-mode/v1 \\
#       --api-key $DASHSCOPE_API_KEY
#   bash ar_scripts/start_e2e.sh --mode fixed --response "你好"
#   bash ar_scripts/start_e2e.sh --no-frontend                       # 无前端
#   bash ar_scripts/start_e2e.sh --no-mock-uds                       # 无 mock UDS
#
# 停止：Ctrl+C 自动清理所有进程
# ═══════════════════════════════════════════════════════════════════════

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# ── 默认配置 ──────────────────────────────────────────────────────────

# 组件开关
RUN_FMM=true           # FusionMind Mock Server
RUN_MOCK_UDS=true      # Mock UDS Server (Python)
RUN_DAEMON=true        # agentflowd
RUN_BRIDGE=true        # Bridge
RUN_FRONTEND=true      # Frontend web

# 端口
FMM_PORT=8085
DAEMON_PORT=17891
BRIDGE_PORT=8199
FRONTEND_PORT=15173

# Sockets
MOCK_UDS_SOCKET="/tmp/mock_agentflow.sock"

# FusionMind 配置
FMM_MODE="echo"
FMM_RESPONSE=""
FMM_UPSTREAM=""
FMM_API_KEY=""
FMM_MODEL="aisf/deepseek-v4-flash"
FMM_TOKEN_DELAY=0.02
FMM_ERROR_RATE=0.0

# Agent 配置
AGENT_PACKAGE="$PROJECT_ROOT/examples/agent/outfit-assistant"
AGENTFLOWD_BIN=""

# 日志
LOG_DIR="$PROJECT_ROOT/logs"
mkdir -p "$LOG_DIR"

# PID 文件
PID_DIR="/tmp"
FMM_PID_FILE="$PID_DIR/fusionmind_mock.pid"
MOCK_UDS_PID_FILE="$PID_DIR/mock_uds_server.pid"

# ── 解析参数 ──────────────────────────────────────────────────────────

while [ $# -gt 0 ]; do
    case "$1" in
        # 组件开关
        --no-fmm)             RUN_FMM=false; shift ;;
        --no-mock-uds)        RUN_MOCK_UDS=false; shift ;;
        --no-daemon)          RUN_DAEMON=false; shift ;;
        --no-bridge)          RUN_BRIDGE=false; shift ;;
        --no-frontend)        RUN_FRONTEND=false; shift ;;
        --fmm-only)           RUN_MOCK_UDS=false; RUN_DAEMON=false; RUN_BRIDGE=false; RUN_FRONTEND=false; shift ;;

        # 端口
        --fmm-port)           FMM_PORT="${2:?}"; shift 2 ;;
        --daemon-port)        DAEMON_PORT="${2:?}"; shift 2 ;;
        --bridge-port)        BRIDGE_PORT="${2:?}"; shift 2 ;;
        --frontend-port)      FRONTEND_PORT="${2:?}"; shift 2 ;;
        --uds-socket)         MOCK_UDS_SOCKET="${2:?}"; shift 2 ;;

        # FusionMind Mock 模式
        --proxy)              FMM_MODE="proxy"; shift ;;
        --mode)               FMM_MODE="${2:?}"; shift 2 ;;
        --response)           FMM_RESPONSE="${2:?}"; shift 2 ;;
        --model)              FMM_MODEL="${2:?}"; shift 2 ;;
        --upstream-url)       FMM_UPSTREAM="${2:?}"; shift 2 ;;
        --api-key)            FMM_API_KEY="${2:?}"; shift 2 ;;
        --token-delay)        FMM_TOKEN_DELAY="${2:?}"; shift 2 ;;
        --error-rate)         FMM_ERROR_RATE="${2:?}"; shift 2 ;;

        # 其他
        --package)            AGENT_PACKAGE="${2:?}"; shift 2 ;;
        --agentflowd-bin)     AGENTFLOWD_BIN="${2:?}"; shift 2 ;;

        -h|--help)
            sed -n '3,29p' "$0"
            echo ""
            echo "选项："
            echo "  组件控制："
            echo "    --no-fmm              不启动 FusionMind Mock"
            echo "    --no-mock-uds         不启动 Mock UDS Server"
            echo "    --no-daemon           不启动 agentflowd"
            echo "    --no-bridge           不启动 Bridge"
            echo "    --no-frontend         不启动前端"
            echo "    --fmm-only           仅启动 FusionMind Mock"
            echo ""
            echo "  FusionMind Mock："
            echo "    --proxy               proxy 模式（透传到真实 LLM）"
            echo "    --mode MODE           mock 模式: echo|fixed|template|tool_calls"
            echo "    --response TEXT       fixed 模式响应内容"
            echo "    --model NAME          模型名"
            echo "    --upstream-url URL    上游 LLM API 地址（默认 DashScope）"
            echo "    --api-key KEY         上游 API Key（默认 $DASHSCOPE_API_KEY）"
            echo "    --token-delay SEC     token 间隔（默认 0.02）"
            echo "    --error-rate RATE     错误注入概率 0-1"
            echo ""
            echo "  端口 / 路径："
            echo "    --fmm-port PORT       FusionMind Mock 端口（默认 8085）"
            echo "    --daemon-port PORT    daemon 端口（默认 17891）"
            echo "    --bridge-port PORT    Bridge 端口（默认 8199）"
            echo "    --frontend-port PORT  前端端口（默认 15173）"
            echo "    --uds-socket PATH     Mock UDS socket 路径"
            echo "    --package PATH        Agent 包目录"
            echo "    --agentflowd-bin PATH agentflowd 二进制路径"
            exit 0 ;;
        *)
            echo "[ERROR] 未知参数: $1" >&2
            echo "用法: $0 [--help]" >&2
            exit 2 ;;
    esac
done

# ── 环境检测 ──────────────────────────────────────────────────────────

ar_require_command python3
ar_load_env

# ── 帮助函数 ──────────────────────────────────────────────────────────

_pids=()  # 跟踪启动的进程，按启动顺序

_cleanup() {
    echo ""
    echo "🛑 正在停止所有组件（逆序）..."

    # 逆序停止
    for ((i = ${#_pids[@]} - 1; i >= 0; i--)); do
        local pid="${_pids[i]}"
        local name="${_names[i]}"
        if kill -0 "$pid" 2>/dev/null; then
            echo "  ⏹  停止 $name (PID=$pid)..."
            kill "$pid" 2>/dev/null || true
            # 等 3 秒，不行就强杀
            for _ in $(seq 1 6); do
                if ! kill -0 "$pid" 2>/dev/null; then
                    break
                fi
                sleep 0.5
            done
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
                echo "  💀 强杀 $name (PID=$pid)"
            else
                echo "  ✅ $name 已停止"
            fi
        fi
    done

    echo "✅ 所有组件已停止"
}

_register_pid() {
    local pid="$1"
    local name="$2"
    _pids+=("$pid")
    _names+=("$name")
}

_wait_for_port() {
    local port="$1"
    local name="$2"
    local timeout="${3:-30}"
    echo "  ⏳ 等待 $name 启动 (port=$port, timeout=${timeout}s)..."
    for i in $(seq 1 "$timeout"); do
        if command -v nc >/dev/null 2>&1; then
            nc -z 127.0.0.1 "$port" 2>/dev/null && return 0
        else
            (echo > /dev/tcp/127.0.0.1/"$port") 2>/dev/null && return 0
        fi
        sleep 1
    done
    echo "  ⚠️  $name 启动超时（继续）"
    return 1
}

# ── 信号处理 ──────────────────────────────────────────────────────────

trap _cleanup EXIT
trap 'echo ""; echo "收到中断信号..."; exit 0' INT TERM

# ═══════════════════════════════════════════════════════════════════════
#  1. FusionMind Mock Server
# ═══════════════════════════════════════════════════════════════════════

if $RUN_FMM; then
    FMM_SCRIPT="$PROJECT_ROOT/frontend/mocks/mocks_fusionmind/server.py"
    if [ ! -f "$FMM_SCRIPT" ]; then
        echo "[ERROR] FusionMind Mock 脚本不存在: $FMM_SCRIPT" >&2
        exit 1
    fi

    FMM_ARGS=("--port" "$FMM_PORT" "--log-level" "info")

    if [ "$FMM_MODE" = "proxy" ]; then
        # Proxy 模式：透传到真实 LLM（默认 DashScope）
        # DashScope OpenAI 兼容端点：https://dashscope.aliyuncs.com/compatible-mode/v1
        # 支持模型：qwen-plus、qwen-turbo、qwen-max 等
        FMM_UPSTREAM="${FMM_UPSTREAM:-${AF_LLM_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}}"
        FMM_API_KEY="${FMM_API_KEY:-${AF_LLM_API_KEY:-${DASHSCOPE_API_KEY:-}}}"
        if [ -z "$FMM_API_KEY" ]; then
            echo "[WARN] Proxy 模式但未设置 API Key。"
            echo "  设置方式:"
            echo "    export DASHSCOPE_API_KEY=sk-xxx   # DashScope"
            echo "    或: --api-key sk-xxx"
            echo "    或: export AF_LLM_API_KEY=sk-xxx"
        fi
        FMM_ARGS+=(--proxy --upstream-url "$FMM_UPSTREAM" --api-key "$FMM_API_KEY")
        echo "🔌 FusionMind Mock → PROXY → $FMM_UPSTREAM"
    else
        # Mock 模式
        FMM_ARGS+=(--mode "$FMM_MODE" --model "$FMM_MODEL")
        FMM_ARGS+=(--token-delay "$FMM_TOKEN_DELAY" --error-rate "$FMM_ERROR_RATE")
        if [ -n "$FMM_RESPONSE" ]; then
            FMM_ARGS+=(--response "$FMM_RESPONSE")
        fi
        echo "🧪 FusionMind Mock → MOCK 模式 ($FMM_MODE)"
    fi

    echo "🚀 启动 FusionMind Mock Server (端口 $FMM_PORT)..."
    nohup python3 "$FMM_SCRIPT" "${FMM_ARGS[@]}" \
        >> "$LOG_DIR/fusionmind_mock.log" 2>&1 &
    FMM_PID=$!
    echo "$FMM_PID" > "$FMM_PID_FILE"
    _register_pid "$FMM_PID" "fusionmind_mock"
    echo "   PID: $FMM_PID | 日志: $LOG_DIR/fusionmind_mock.log"
    _wait_for_port "$FMM_PORT" "FusionMind Mock" 10
fi

# ═══════════════════════════════════════════════════════════════════════
#  2. Mock UDS Server (Python, 模拟 UDSPluginServer)
# ═══════════════════════════════════════════════════════════════════════

if $RUN_MOCK_UDS; then
    MOCK_UDS_SCRIPT="$PROJECT_ROOT/frontend/mocks/mock_uds_model_server/mock_uds_server.py"
    if [ ! -f "$MOCK_UDS_SCRIPT" ]; then
        echo "[ERROR] Mock UDS Server 脚本不存在: $MOCK_UDS_SCRIPT" >&2
        exit 1
    fi

    # 清理旧 socket
    if [ -e "$MOCK_UDS_SOCKET" ]; then
        rm -f "$MOCK_UDS_SOCKET"
    fi

    echo "🚀 启动 Mock UDS Server (socket=$MOCK_UDS_SOCKET)..."
    nohup python3 "$MOCK_UDS_SCRIPT" \
        --socket "$MOCK_UDS_SOCKET" \
        --mode echo \
        --token-delay "$FMM_TOKEN_DELAY" \
        >> "$LOG_DIR/mock_uds_server.log" 2>&1 &
    MOCK_UDS_PID=$!
    echo "$MOCK_UDS_PID" > "$MOCK_UDS_PID_FILE"
    _register_pid "$MOCK_UDS_PID" "mock_uds_server"
    echo "   PID: $MOCK_UDS_PID | 日志: $LOG_DIR/mock_uds_server.log"

    # 等 socket 就绪
    for i in $(seq 1 10); do
        if [ -e "$MOCK_UDS_SOCKET" ]; then
            break
        fi
        sleep 0.5
    done
    if [ ! -e "$MOCK_UDS_SOCKET" ]; then
        echo "  ⚠️  Mock UDS Socket 未就绪（继续）"
    else
        echo "   ✅ Socket 就绪: $MOCK_UDS_SOCKET"
    fi
fi

# ═══════════════════════════════════════════════════════════════════════
#  3. 设置环境变量（影响 Python Node 子进程）
# ═══════════════════════════════════════════════════════════════════════

# 让 LlmUdsClient 连接到 Mock UDS Server 而非默认 /tmp/agentflow_plugin.sock
export AF_UDS_SOCKET_PATH="$MOCK_UDS_SOCKET"

# FusionMind Mock HTTP 地址（供 OpenAiCompatibleClient 或 UDSPluginServer 使用）
export AF_LLM_BASE_URL="http://127.0.0.1:$FMM_PORT/infermind/llm/v1/chat"
export AF_LLM_API_KEY="${AF_LLM_API_KEY:-mock-key}"
export AF_LLM_MODEL="$FMM_MODEL"

echo ""
echo "📋 环境变量设置："
echo "   AF_UDS_SOCKET_PATH=$AF_UDS_SOCKET_PATH"
echo "   AF_LLM_BASE_URL=$AF_LLM_BASE_URL"
echo "   AF_LLM_MODEL=$AF_LLM_MODEL"
echo ""

# ═══════════════════════════════════════════════════════════════════════
#  4. AgentFlow Daemon (agentflowd)
# ═══════════════════════════════════════════════════════════════════════

if $RUN_DAEMON; then
    if [ -n "$AGENTFLOWD_BIN" ]; then
        DAEMON_BIN="$AGENTFLOWD_BIN"
    else
        # 从默认 build 目录查找
        for dir in "$PROJECT_ROOT/build/dev" "$PROJECT_ROOT/build"; do
            if [ -x "$dir/agentflowd" ]; then
                DAEMON_BIN="$dir/agentflowd"
                break
            fi
        done
    fi

    if [ -z "${DAEMON_BIN:-}" ] || [ ! -x "${DAEMON_BIN:-}" ]; then
        echo "[WARN] agentflowd 未找到，尝试编译..."
        echo "   Running: bash $SCRIPT_DIR/build.sh dev agentflowd"
        bash "$SCRIPT_DIR/build.sh" dev agentflowd 2>&1 | tail -5
        DAEMON_BIN="$PROJECT_ROOT/build/dev/agentflowd"
        if [ ! -x "$DAEMON_BIN" ]; then
            echo "[ERROR] agentflowd 编译失败或不存在" >&2
            echo "  手动编译: bash $SCRIPT_DIR/build.sh dev agentflowd" >&2
            exit 1
        fi
    fi

    echo "🚀 启动 AgentFlow Daemon (port $DAEMON_PORT)..."
    nohup "$DAEMON_BIN" \
        --package "$AGENT_PACKAGE" \
        --listen "127.0.0.1:$DAEMON_PORT" \
        >> "$LOG_DIR/agentflowd.log" 2>&1 &
    DAEMON_PID=$!
    echo "$DAEMON_PID" > /tmp/agentflowd.pid
    _register_pid "$DAEMON_PID" "agentflowd"
    echo "   PID: $DAEMON_PID | 日志: $LOG_DIR/agentflowd.log"
    sleep 2
fi

# ═══════════════════════════════════════════════════════════════════════
#  5. Bridge
# ═══════════════════════════════════════════════════════════════════════

if $RUN_BRIDGE; then
    echo "🚀 启动 Bridge (port $BRIDGE_PORT)..."
    bash "$SCRIPT_DIR/start_bridge.sh" start --port "$BRIDGE_PORT" --log-level info --no-reload
    # 从中获取 PID
    sleep 2
    BRIDGE_PID=$(pgrep -f "uvicorn.*main:app.*frontend/bridge" 2>/dev/null || true)
    if [ -n "$BRIDGE_PID" ]; then
        _register_pid "$BRIDGE_PID" "bridge"
    fi
    _wait_for_port "$BRIDGE_PORT" "Bridge" 15
fi

# ═══════════════════════════════════════════════════════════════════════
#  6. 前端
# ═══════════════════════════════════════════════════════════════════════

if $RUN_FRONTEND; then
    if [ -d "$PROJECT_ROOT/frontend/web" ]; then
        echo "🚀 启动前端 (port $FRONTEND_PORT)..."
        nohup bash "$SCRIPT_DIR/start_web.sh" "$FRONTEND_PORT" \
            >> "$LOG_DIR/frontend.log" 2>&1 &
        FRONTEND_PID=$!
        _register_pid "$FRONTEND_PID" "frontend"
        echo "   PID: $FRONTEND_PID | 日志: $LOG_DIR/frontend.log"
    else
        echo "[WARN] frontend/web 目录不存在，跳过前端"
    fi
fi

# ═══════════════════════════════════════════════════════════════════════
#  7. 启动摘要
# ═══════════════════════════════════════════════════════════════════════

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "  ✅ 端到端调试环境已启动"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "  📍 前端:       http://localhost:$FRONTEND_PORT"
echo "  📍 Bridge API: http://localhost:$BRIDGE_PORT"
echo "  📍 Daemon:     127.0.0.1:$DAEMON_PORT"
echo "  📍 FusionMind:  http://localhost:$FMM_PORT"
echo "       POST  /infermind/llm/v1/chat/openai/completions"
echo "       POST  /infermind/llm/v1/chat/completions"
echo "       GET   /health"
echo ""
echo "  🔌 UDS Socket: $MOCK_UDS_SOCKET"
echo "  🧪 Mock 模式:  $([ "$FMM_MODE" = "proxy" ] && echo "PROXY → $FMM_UPSTREAM" || echo "MOCK ($FMM_MODE)")"
echo ""
echo "  日志文件:"
echo "    FusionMind Mock:  tail -f $LOG_DIR/fusionmind_mock.log"
echo "    Mock UDS Server:  tail -f $LOG_DIR/mock_uds_server.log"
echo "    Daemon:           tail -f $LOG_DIR/agentflowd.log"
echo "    Bridge:           tail -f $LOG_DIR/bridge.log"
echo ""
echo "  按 Ctrl+C 停止所有组件"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# 保持运行，等待 Ctrl+C
wait
