#!/usr/bin/env bash
# Simulator business tools use only the standard library. The host runs the LLM runtime.
set -euo pipefail
python3 -c 'import json, sqlite3; print("业务模拟工具依赖检查通过")'
