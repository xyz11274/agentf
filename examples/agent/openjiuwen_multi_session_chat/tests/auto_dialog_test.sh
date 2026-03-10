#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"

LOG_DIR="examples/agent/openjiuwen_multi_session_chat/package/runtime/logs"
mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/openjiuwen_multi_session_chat_tui.log

OUTPUT=$(printf 'hello\n请帮我总结一下上条消息\n今天天气？\n/status\n/quit\n' |
  ./build/example_openjiuwen_multi_session_chat_tui examples/agent/openjiuwen_multi_session_chat/package 2>/dev/null)

grep -q 'OpenJiuwen Multi-Session Chat TUI' <<<"$OUTPUT" || { echo 'missing tui banner'; exit 1; }
grep -q '你好！我可以帮你做总结、检索、日期时间计算和任务规划。' <<<"$OUTPUT" || { echo 'missing greeting response'; exit 1; }
grep -q '上条消息主要内容是：hello' <<<"$OUTPUT" || { echo 'missing summary response'; exit 1; }
grep -q 'status> session: tui:default' <<<"$OUTPUT" || { echo 'missing local status output'; exit 1; }
grep -q 'Bye from openjiuwen_multi_session_chat.' <<<"$OUTPUT" || { echo 'missing clean shutdown'; exit 1; }
if grep -q 'DeprecationWarning' <<<"$OUTPUT"; then
  echo 'tui output leaked runtime warning'
  exit 1
fi
if grep -q '\[INFO\]\|\[ERROR\]' <<<"$OUTPUT"; then
  echo 'tui output leaked runtime log lines'
  exit 1
fi
if [ ! -f "$LOG_DIR"/openjiuwen_multi_session_chat_tui.log ]; then
  echo 'runtime log file not created'
  exit 1
fi

echo 'openjiuwen multi_session_chat tui test passed'
