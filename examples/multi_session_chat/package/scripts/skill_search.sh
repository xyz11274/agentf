#!/bin/sh
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/../../../../publish/sdk/shell/agentflow_databus.sh"

SESSION_ID="${AF_SESSION_ID:-default}"
QUERY="${AF_USER_TEXT:-$(af_databus_get user_text 2>/dev/null || true)}"
RESULT="$(
  python3 - "$QUERY" "$SCRIPT_DIR" <<'PY'
import json
import sys
from pathlib import Path

query = (sys.argv[1] or "query").strip()
script_dir = Path(sys.argv[2])
sys.path.insert(0, str(script_dir))

from skill_contracts import assess_search_query  # noqa: E402

assessment = assess_search_query(query)
if assessment["can_handle"]:
    if "新闻" in query or "热点" in query:
        result = f"可执行有限检索支持：建议优先查看近 7 天内与“{query}”相关的权威新闻摘要。"
    else:
        result = f"可执行有限检索支持：建议围绕“{query}”补充权威来源、时间范围和事实核验。"
else:
    result = ""
payload = {
    "search_result": result,
    "answer_text": result,
    "can_handle": assessment["can_handle"],
    "status": assessment["status"],
    "grounded": assessment["grounded"],
    "confidence": assessment["confidence"],
    "reason": assessment["reason"],
}
print(json.dumps(payload, ensure_ascii=False))
PY
)"
SEARCH_RESULT="$(python3 - "$RESULT" <<'PY'
import json
import sys

payload = json.loads(sys.argv[1])
print(payload.get("search_result", ""))
PY
)"
af_databus_set "session:${SESSION_ID}:last_search_result" "$SEARCH_RESULT" >/dev/null 2>&1 || true
printf '%s\n' "$RESULT"
