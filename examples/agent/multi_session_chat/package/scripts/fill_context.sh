#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SESSION_ID="${AF_SESSION_ID:-precreated_session_1}"
SUMMARY="$(cat "${ROOT}/runtime/history_summary.json" 2>/dev/null || echo '{}')"
echo "{\"session_id\":\"${SESSION_ID}\",\"context\":${SUMMARY}}" > "${ROOT}/runtime/${SESSION_ID}.context.json"
echo "context filled"
