#!/usr/bin/env bash
set -euo pipefail
PACKAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$PACKAGE_DIR/../../.." && pwd)"
PYTHON_BIN="${BUSINESS_SIM_PYTHON:-python3}"
"$PYTHON_BIN" -c 'import sys; assert sys.version_info >= (3, 12), "agentflow-runtime requires Python >= 3.12"'
"$PYTHON_BIN" -m venv "$PACKAGE_DIR/.venv"
"$PACKAGE_DIR/.venv/bin/python3" -m pip install -e "$REPO_DIR/packages/agentflow-runtime"
"$PACKAGE_DIR/.venv/bin/python3" "$PACKAGE_DIR/scripts/setup_browser.py"
touch "$PACKAGE_DIR/.venv/.agentflow_installed"
echo "环境就绪。填好 .env 后运行 bash $PACKAGE_DIR/run.sh"
