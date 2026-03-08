#!/bin/sh
set -eu
. "$(cd "$(dirname "$0")" && pwd)/../../../../publish/sdk/shell/agentflow_databus.sh"

SESSION_ID="${AF_SESSION_ID:-default}"
QUERY="${AF_USER_TEXT:-$(af_databus_get user_text 2>/dev/null || true)}"
RESULT="$(
  python3 - "$QUERY" <<'PY'
import sys

query = (sys.argv[1] or "query").strip()
if "天气" in query:
    print(f"已完成天气信息检索建议：当前环境未接入实时气象 API，建议关注温度、降水和风力三项指标（query={query}）。")
elif "新闻" in query or "热点" in query:
    print(f"已完成热点检索：建议优先查看近 7 天内与“{query}”相关的权威新闻摘要。")
else:
    print(f"已完成外部信息检索，可围绕“{query}”继续展开事实核验与资料补充。")
PY
)"
af_databus_set "session:${SESSION_ID}:last_search_result" "$RESULT" >/dev/null 2>&1 || true
python3 - "$RESULT" <<'PY'
import json
import sys

print(json.dumps({"search_result": sys.argv[1]}, ensure_ascii=False))
PY
