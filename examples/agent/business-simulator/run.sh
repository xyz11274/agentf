#!/usr/bin/env bash
set -euo pipefail
PACKAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$PACKAGE_DIR/../../.." && pwd)"
if [[ -f "$PACKAGE_DIR/.env" ]]; then
  set -a
  source "$PACKAGE_DIR/.env"
  set +a
fi
if [[ -n "${BUSINESS_SIM_PYTHON:-}" ]]; then
  PYTHON_BIN="$BUSINESS_SIM_PYTHON"
elif [[ -x "$PACKAGE_DIR/.venv/bin/python3" ]]; then
  PYTHON_BIN="$PACKAGE_DIR/.venv/bin/python3"
else
  PYTHON_BIN="${BUSINESS_SIM_PYTHON:-python3}"
fi
PYTHON_BIN="$(command -v "$PYTHON_BIN")"
"$PYTHON_BIN" -c 'import agentflow_runtime, yaml, openai'
if [[ -z "${AF_LLM_API_KEY:-${API_KEY:-}}" || -z "${AF_LLM_BASE_URL:-}" ]]; then
  echo '请在 .env 或环境变量中配置 AF_LLM_API_KEY 和 AF_LLM_BASE_URL。' >&2
  exit 2
fi
export PATH="$(dirname "$PYTHON_BIN"):$PATH"
export PYTHON_PATH="$(dirname "$(dirname "$PYTHON_BIN")")"
export AF_TRACE_OUTPUT_DIR="${AF_TRACE_OUTPUT_DIR:-$REPO_DIR/trace_output/business-simulator}"
export AF_MAX_SKILL_TASK_ITERATIONS="${AF_MAX_SKILL_TASK_ITERATIONS:-24}"
export AF_SESSIONLOG_ENABLED="${AF_SESSIONLOG_ENABLED:-1}"
export BUSINESS_SIM_DB="${BUSINESS_SIM_DB:-$PACKAGE_DIR/outputs/business.sqlite3}"
cd "$REPO_DIR"
exec "$PYTHON_BIN" "$PACKAGE_DIR/scripts/launch.py" "${AGENTFLOW_DAEMON:-$REPO_DIR/build/agentflowd}" --config "$PACKAGE_DIR/daemon.json" --listen "${BUSINESS_SIM_LISTEN:-127.0.0.1:17893}" --package "$PACKAGE_DIR" "$@"
