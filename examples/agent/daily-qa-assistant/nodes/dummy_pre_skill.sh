#!/bin/bash
# Auto-generated shell wrapper — do not modify
# 标准实现：docs/design/agent-template-delta.md §3.5
# 优先用 Agent 包内 .venv（setup.sh 装好后写 sentinel）；不可用则 fallback 到系统 python3
# 系统 python3 必须已预装 agentflow-runtime（见 §2.2 Bridge 启动时预装）

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PACKAGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VENV_PYTHON="$PACKAGE_DIR/.venv/bin/python3"
AF_SENTINEL="$PACKAGE_DIR/.venv/.agentflow_installed"

if [ -f "$VENV_PYTHON" ] && [ -f "$AF_SENTINEL" ]; then
    exec "$VENV_PYTHON" "$SCRIPT_DIR/dummy_pre_skill.py" "$@"
fi

# fallback：系统 python3（依赖 §2.2 Bridge 启动时预装 agentflow-runtime）
exec python3 "$SCRIPT_DIR/dummy_pre_skill.py" "$@"
