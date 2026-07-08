#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════
# UDS 协议一致性检查 — 统一入口
# ═══════════════════════════════════════════════════════════════════════
#
# 对目标 UDS Server 执行标准化探测，检查：
#   1. UDS 协议格式是否正确（事件字段、帧结构、done 标记）
#   2. UDS → SysAgent → FusionMind 转发是否一致（指定 --fusionmind-url 时）
#
# 用法：
#   bash scripts/check_uds_protocol.sh                # 只测 UDS 协议
#   bash scripts/check_uds_protocol.sh --full          # UDS + FusionMind 转发对比
#   bash scripts/check_uds_protocol.sh --socket /tmp/agentflow_plugin.sock --tag production
#   bash scripts/check_uds_protocol.sh --help          # 完整参数
#
# 常用场景：
#
#   # 场景 1：本地开发 — 对 Mock UDS Server 做基本检查
#   bash scripts/check_uds_protocol.sh
#
#   # 场景 2：生产环境 — 检查 SysAgent 是否正常工作
#   bash scripts/check_uds_protocol.sh --socket /tmp/agentflow_plugin.sock --tag production
#
#   # 场景 3：生产环境 — 全面检查 UDS 协议 + 转发一致性
#   bash scripts/check_uds_protocol.sh \\
#       --socket /tmp/agentflow_plugin.sock \\
#       --fusionmind-url http://127.0.0.1:27321/infermind/llm/v1/chat \\
#       --tag production
#
#   # 场景 4：对比 Mock vs 生产（先分别保存报告，再 diff）
#   bash scripts/check_uds_protocol.sh --output /tmp/report_mock.json
#   bash scripts/check_uds_protocol.sh --socket /tmp/agentflow_plugin.sock \\
#       --tag production --output /tmp/report_prod.json
#   diff /tmp/report_mock.json /tmp/report_prod.json
#
# ═══════════════════════════════════════════════════════════════════════

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PROBE_SCRIPT="$PROJECT_ROOT/tests/unit/test_uds_protocol_consistency.py"
DEFAULT_SOCKET="/tmp/mock_agentflow.sock"

# ── 参数解析 ──────────────────────────────────────────────────────────

SOCKET=""
TAG=""
FUSIONMIND_URL=""
OUTPUT=""
FULL_MODE=false

while [ $# -gt 0 ]; do
    case "$1" in
        --socket)     SOCKET="$2";  shift 2 ;;
        --tag)        TAG="$2";     shift 2 ;;
        --fusionmind-url) FUSIONMIND_URL="$2"; shift 2 ;;
        --output)     OUTPUT="$2";  shift 2 ;;
        --full)       FULL_MODE=true; shift ;;
        -h|--help)
            sed -n '3,35p' "$0"
            echo ""
            echo "选项："
            echo "  --socket PATH          UDS socket 路径（默认 /tmp/mock_agentflow.sock）"
            echo "  --tag NAME             目标标识（如 mock / production，默认从 socket 路径推断）"
            echo "  --fusionmind-url URL   FusionMind HTTP 端点（启用转发一致性对比）"
            echo "  --output PATH          输出 JSON 报告路径"
            echo "  --full                 快捷方式 = --socket /tmp/agentflow_plugin.sock"
            echo "                                     --tag production"
            echo "                                     --fusionmind-url 自动推断"
            echo ""
            echo "示例："
            echo "  bash check_uds_protocol.sh"
            echo "  bash check_uds_protocol.sh --full"
            echo "  bash check_uds_protocol.sh --socket /tmp/agentflow_plugin.sock --tag production"
            echo "  bash check_uds_protocol.sh --socket /tmp/agentflow_plugin.sock \\"
            echo "        --fusionmind-url http://127.0.0.1:27321/infermind/llm/v1/chat"
            exit 0 ;;
        *)
            echo "[ERROR] 未知参数: $1" >&2
            echo "用法: $0 [--help]" >&2
            exit 2 ;;
    esac
done

# ── --full 模式自动填充参数 ───────────────────────────────────────────

if $FULL_MODE; then
    SOCKET="${SOCKET:-/tmp/agentflow_plugin.sock}"
    TAG="${TAG:-production}"
    if [ -z "$FUSIONMIND_URL" ]; then
        # 尝试从 daemon 配置推断 FusionMind 地址
        FUSIONMIND_URL="${AF_LLM_BASE_URL:-http://127.0.0.1:27321/infermind/llm/v1/chat}"
    fi
fi

# 默认 socket
SOCKET="${SOCKET:-$DEFAULT_SOCKET}"

# ── 检查依赖 ──────────────────────────────────────────────────────────

if [ ! -f "$PROBE_SCRIPT" ]; then
    echo "[ERROR] 探测脚本不存在: $PROBE_SCRIPT" >&2
    exit 1
fi

if [ ! -e "$SOCKET" ]; then
    echo "⚠️  UDS socket 不存在: $SOCKET"
    echo "   尝试自动启动 Mock UDS Server..."
    if [ -f "$SCRIPT_DIR/start_mock_uds_server.sh" ]; then
        bash "$SCRIPT_DIR/start_mock_uds_server.sh" --socket "$SOCKET" &
        MOCK_PID=$!
        # 等 socket 就绪
        for i in $(seq 1 10); do
            if [ -e "$SOCKET" ]; then
                break
            fi
            sleep 0.5
        done
        if [ ! -e "$SOCKET" ]; then
            echo "[ERROR] Mock UDS Server 启动失败" >&2
            exit 1
        fi
        echo "   ✅ Mock UDS Server 已启动 (PID=$MOCK_PID)"
    else
        echo "[ERROR] 找不到 start_mock_uds_server.sh，请手动启动 UDS Server" >&2
        exit 1
    fi
fi

# ── 构建命令 ──────────────────────────────────────────────────────────

CMD=("python3" "$PROBE_SCRIPT" "--socket" "$SOCKET")
[ -n "$TAG" ] && CMD+=("--tag" "$TAG")
[ -n "$FUSIONMIND_URL" ] && CMD+=("--fusionmind-url" "$FUSIONMIND_URL")
[ -n "$OUTPUT" ] && CMD+=("--output" "$OUTPUT")

# ── 执行探测 ──────────────────────────────────────────────────────────

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "  UDS 协议一致性检查"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "  Socket:       $SOCKET"
echo "  Tag:          ${TAG:-$(echo "$SOCKET" | grep -q mock && echo mock || echo unknown)}"
if [ -n "$FUSIONMIND_URL" ]; then
    echo "  FusionMind:   $FUSIONMIND_URL"
fi
if [ -n "$OUTPUT" ]; then
    echo "  报告输出:     $OUTPUT"
fi
echo ""

"${CMD[@]}"
EXIT_CODE=$?

# ── 输出摘要 ──────────────────────────────────────────────────────────

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo "✅ 全部检查通过"
else
    echo "❌ 存在失败项，详情见上"
fi
echo ""

# 如果是自动启动的 Mock Server，停止它
if [ -n "${MOCK_PID:-}" ]; then
    echo "🛑 停止 Mock UDS Server (PID=$MOCK_PID)..."
    kill "$MOCK_PID" 2>/dev/null || true
fi

exit $EXIT_CODE
