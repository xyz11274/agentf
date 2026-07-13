#!/bin/bash
# python_wrapper.sh — self-bootstrapping Python wrapper.
# Creates .venv, installs deps on first run, then execs .venv's python.
# setup.sh writes a sentinel file so we skip re-install if already done.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
REQ_FILE="$SCRIPT_DIR/requirements.txt"
HASH_FILE="$VENV_DIR/.requirements_hash"
SENTINEL="$VENV_DIR/.agentflow_installed"

if [ ! -f "$VENV_DIR/bin/python3" ]; then
    python3 -m venv "$VENV_DIR" 2>/dev/null || python3 -m venv --without-pip "$VENV_DIR"
    "$VENV_DIR/bin/python3" -m ensurepip --upgrade --default-pip 2>/dev/null || true
fi

# Only install deps when sentinel is missing or requirements changed
if [ -f "$VENV_DIR/bin/pip" ] && [ -f "$REQ_FILE" ]; then
    if command -v md5sum &>/dev/null; then
        HASH=$(md5sum "$REQ_FILE" | cut -d' ' -f1)
    else
        HASH=$(md5 -r "$REQ_FILE" | cut -d' ' -f1)
    fi
    NEED_INSTALL=false
    if [ ! -f "$SENTINEL" ]; then
        NEED_INSTALL=true
    elif [ ! -f "$HASH_FILE" ] || [ "$(cat "$HASH_FILE")" != "$HASH" ]; then
        NEED_INSTALL=true
    fi
    if [ "$NEED_INSTALL" = true ]; then
        "$VENV_DIR/bin/pip" install -r "$REQ_FILE" --quiet
        echo "$HASH" > "$HASH_FILE"
        touch "$SENTINEL"
    fi
fi

exec "$VENV_DIR/bin/python3" "$@"
