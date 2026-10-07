#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"
# 真实 API 凭据由 examples/agent/business-simulator/.env 读取；不把 key 放入命令或日志。
unset HTTP_PROXY HTTPS_PROXY ALL_PROXY http_proxy https_proxy all_proxy BUSINESS_BROWSER_PROXY
export NO_PROXY='*' no_proxy='*'
export PYTHONDONTWRITEBYTECODE=1
PYTHON_BIN="${AGENTFLOW_PYTHON:-/root/miniconda3/envs/agentflow-dev/bin/python3}"
if [[ ! -x "$PYTHON_BIN" ]]; then PYTHON_BIN=python3; fi
if [[ $# -eq 0 ]]; then set -- --fresh; fi
exec "$PYTHON_BIN" -u tools/trajectory_evolution/experiments/business-coverage/reproduce.py "$@"
