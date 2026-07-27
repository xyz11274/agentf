#!/bin/bash
set -e

# 加载环境配置
_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../configs" && pwd)" 2>/dev/null || _ENV_DIR=""
_ENV="${AGENTFLOW_ENV:-dev}"
if [ -n "$_ENV_DIR" ] && [ -f "$_ENV_DIR/$_ENV/.env" ]; then set -a; source "$_ENV_DIR/$_ENV/.env"; set +a; fi
unset _ENV_DIR _ENV

# ============================================================
# run_agent.sh — 启动 harness_agent 交互模式
# 用法:
#   ./scripts/run_agent.sh         交互模式
#   ./scripts/run_agent.sh validate 仅验证包结构
#   ./scripts/run_agent.sh list    列出 workflow
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

AGENT_PATH="$PROJECT_DIR/examples/agent/harness_agent"
RUNNER="$PROJECT_DIR/build/agent_runner"

# ---------- 检查编译产物 ----------
if [ ! -f "$RUNNER" ]; then
    echo "[ERROR] agent_runner not found at: $RUNNER"
    echo ""
    echo "Please build first:"
    echo "  ./scripts/build.sh agent_runner"
    echo ""
    echo "Or build all:"
    echo "  ./scripts/build.sh"
    exit 1
fi

# ---------- 检查 agent 包 ----------
if [ ! -f "$AGENT_PATH/package.json" ]; then
    echo "[ERROR] Agent package not found at: $AGENT_PATH"
    exit 1
fi

# ---------- 解析参数 ----------
MODE="${1:-interactive}"

case "$MODE" in
    validate)
        echo "=== Validating agent package ==="
        exec "$RUNNER" "$AGENT_PATH" --validate
        ;;
    list)
        echo "=== Listing workflows ==="
        exec "$RUNNER" "$AGENT_PATH" --list-workflows
        ;;
    interactive|run)
        echo ""
        echo "╔══════════════════════════════════════════════════╗"
        echo "║   Harness Agent — coordinator-only agent loop   ║"
        echo "╠══════════════════════════════════════════════════╣"
        echo "║   Inside interactive mode, type:                ║"
        echo "║                                                ║"
        echo "║     execute coordinator-loop-workflow           ║"
        echo "║       --input \"你的消息\"                        ║"
        echo "║                                                ║"
        echo "║   LLM config:                                  ║"

        # Show LLM config status
        if [ -n "$AF_LLM_BASE_URL" ] && [ -n "$AF_LLM_API_KEY" ]; then
            echo "║     ✅ LLM configured: ${AF_LLM_MODEL:-gpt-4o-mini}"
            echo "║        ${AF_LLM_BASE_URL}"
        else
            echo "║     ⚠️  Using mock LLM (no real API calls)"
            echo "║        Set AF_LLM_BASE_URL + AF_LLM_API_KEY env vars"
            echo "║        or configs/{env}/bridge.yaml → agent_defaults.llm"
        fi

        echo "║                                                ║"
        echo "║   Other commands: list / status / help / quit  ║"
        echo "╚══════════════════════════════════════════════════╝"
        echo ""

        # Must cd to package dir so relative paths in workflow JSON resolve correctly
        cd "$AGENT_PATH"
        exec "$RUNNER" "." --interactive
        ;;
    *)
        echo "Usage: $0 [validate|list|interactive]"
        echo ""
        echo "  validate     验证 agent 包结构"
        echo "  list         列出所有 workflow"
        echo "  interactive  交互模式 (default)"
        exit 1
        ;;
esac
