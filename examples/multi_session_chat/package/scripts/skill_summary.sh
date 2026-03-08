#!/bin/sh
set -eu
. "$(cd "$(dirname "$0")" && pwd)/../../../../publish/sdk/shell/agentflow_databus.sh"

SESSION_ID="${AF_SESSION_ID:-default}"
TEXT="${AF_USER_TEXT:-$(af_databus_get user_text 2>/dev/null || true)}"
PARTIAL="$(af_databus_get partial_skill_results 2>/dev/null || true)"
RESULT="$(
  python3 - "$TEXT" "$PARTIAL" <<'PY'
import json
import sys

text = sys.argv[1]
partial = sys.argv[2]
if partial.strip():
    lines = []
    for raw in partial.splitlines():
        raw = raw.strip()
        if not raw:
            continue
        try:
            item = json.loads(raw)
        except Exception:
            continue
        if item.get("result"):
            lines.append(item["result"])
    if lines:
        print("已完成阶段性总结：" + "；".join(lines[:3]))
        raise SystemExit(0)
print(f"已对“{text}”相关信息完成摘要，建议输出 2-3 条关键结论。")
PY
)"
af_databus_set "session:${SESSION_ID}:last_summary_result" "$RESULT" >/dev/null 2>&1 || true
python3 - "$RESULT" <<'PY'
import json
import sys

print(json.dumps({"summary_result": sys.argv[1]}, ensure_ascii=False))
PY
