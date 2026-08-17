#!/bin/bash
# Auto-generated shell wrapper — do not modify
# 标准实现：docs/design/agent-template-delta.md §3.5
# 优先用 Agent 包内 .venv（setup.sh 装好后写 sentinel）；不可用则 fallback 到系统 python3
# 系统 python3 必须已预装 agentflow-runtime（见 §2.2 Bridge 启动时预装）

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PACKAGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# 加载 .env（如果存在），使 API_KEY 等环境变量在 Python 启动前就就位
if [ -f "$PACKAGE_DIR/.env" ]; then
  set -a
  source "$PACKAGE_DIR/.env"
  set +a
fi

VENV_PYTHON="$PACKAGE_DIR/.venv/bin/python3"
AF_SENTINEL="$PACKAGE_DIR/.venv/.agentflow_installed"

# ── A/B 切换：AF_COORDINATOR_VERSION 选 v1（框架驱动编排）vs v2（agent loop）──
# v2 见 docs/design/features/agent-runtime/coordinator-as-agent-loop.md
# 默认 v2（agent loop 为当前默认；设 AF_COORDINATOR_VERSION=v1 可切回框架驱动编排）
case "${AF_COORDINATOR_VERSION:-v2}" in
  v1)
    TARGET_SCRIPT="coordinator_decide.py"
    ;;
  v2|*)
    TARGET_SCRIPT="coordinator_decide_v2.py"
    ;;
esac

if [ -f "$VENV_PYTHON" ] && [ -f "$AF_SENTINEL" ]; then
    exec "$VENV_PYTHON" "$SCRIPT_DIR/$TARGET_SCRIPT" "$@"
fi

# fallback：系统 python3（依赖 §2.2 Bridge 启动时预装 agentflow-runtime）
exec python3 "$SCRIPT_DIR/$TARGET_SCRIPT" "$@"
