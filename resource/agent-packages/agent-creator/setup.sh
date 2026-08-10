#!/bin/bash
# setup.sh — one-shot setup for the agent-creator agent.
# Installs .venv + dependencies, then writes a sentinel file so
# python_wrapper.sh can skip re-installation on subsequent runs.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
REQ_FILE="$SCRIPT_DIR/requirements.txt"
SENTINEL="$VENV_DIR/.agentflow_installed"

# Create .venv if not present
if [ ! -f "$VENV_DIR/bin/python3" ]; then
    python3 -m venv "$VENV_DIR"
fi

# Install dependencies & write sentinel on success
"$VENV_DIR/bin/pip" install -r "$REQ_FILE" --quiet
touch "$SENTINEL"

echo "agent-creator: setup complete (sentinel=$SENTINEL)"
