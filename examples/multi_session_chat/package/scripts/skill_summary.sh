#!/bin/sh
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/../../../../publish/sdk/shell/agentflow_databus.sh"

SESSION_ID="${AF_SESSION_ID:-default}"
TEXT="${AF_USER_TEXT:-$(af_databus_get user_text 2>/dev/null || true)}"
PARTIAL="$(af_databus_get partial_skill_results 2>/dev/null || true)"
HISTORY_JSON="${AF_HISTORY_JSON:-$(af_databus_get "session:${SESSION_ID}:history_json" 2>/dev/null || true)}"
RESULT="$(
  python3 - "$TEXT" "$PARTIAL" "$HISTORY_JSON" "$SCRIPT_DIR" <<'PY'
import json
import sys
from pathlib import Path

text = sys.argv[1]
partial = sys.argv[2]
history_json = sys.argv[3]
script_dir = Path(sys.argv[4])
sys.path.insert(0, str(script_dir))

from skill_contracts import assess_summary_query  # noqa: E402

assessment = assess_summary_query(text)
result = ""
if assessment["can_handle"] and partial.strip():
    lines = []
    for raw in partial.splitlines():
        raw = raw.strip()
        if not raw:
            continue
        try:
            item = json.loads(raw)
        except Exception:
            continue
        if item.get("can_handle") and item.get("result"):
            lines.append(item["result"])
    if lines:
        result = "已完成阶段性总结：" + "；".join(lines[:3])

if assessment["can_handle"] and not result and history_json.strip():
    try:
        history = json.loads(history_json)
    except Exception:
        history = []
    contents = []
    for item in history:
        if not isinstance(item, dict):
            continue
        content = (item.get("content") or "").strip()
        if content and content != text:
            contents.append(content)
    if contents:
        last_content = contents[-1]
        result = f"上条消息主要内容是：{last_content}"

payload = {
    "summary_result": result,
    "answer_text": result,
    "can_handle": assessment["can_handle"] and bool(result),
    "status": "ok" if result else "unsupported",
    "grounded": bool(result),
    "confidence": 0.9 if result else 0.0,
    "reason": assessment["reason"] if not result else "总结技能已基于历史或中间结果生成摘要。",
}
print(json.dumps(payload, ensure_ascii=False))
PY
)"
SUMMARY_RESULT="$(python3 - "$RESULT" <<'PY'
import json
import sys

payload = json.loads(sys.argv[1])
print(payload.get("summary_result", ""))
PY
)"
af_databus_set "session:${SESSION_ID}:last_summary_result" "$SUMMARY_RESULT" >/dev/null 2>&1 || true
printf '%s\n' "$RESULT"
