#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "${ROOT}/runtime"
"${ROOT}/scripts/default_dispatch_broadcast.sh" >/dev/null
"${ROOT}/scripts/session_select_skills.sh" >/dev/null
"${ROOT}/scripts/collect_selected_skills.sh" >/dev/null
"${ROOT}/scripts/execute_selected_skills.sh" >/dev/null
"${ROOT}/scripts/compose_final_response.sh"
