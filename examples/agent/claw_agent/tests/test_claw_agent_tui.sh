#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"

rm -rf examples/agent/claw_agent/runtime/history examples/agent/claw_agent/runtime/state
OUTPUT=$(printf '你好
总结上一条消息
/status
/quit
' | ./build/example_claw_agent_tui examples/agent/claw_agent 2>/dev/null)

grep -q 'ClawAgent TUI' <<<"$OUTPUT" || { echo 'missing tui banner'; exit 1; }
grep -q 'dispatcher 直连 jiuwenclaw' <<<"$OUTPUT" || { echo 'missing greeting response'; exit 1; }
grep -q '你上一条消息是：你好' <<<"$OUTPUT" || { echo 'missing session summary response'; exit 1; }
grep -q 'vendored jiuwenclaw 运行' <<<"$OUTPUT" || { echo 'missing status response'; exit 1; }
grep -q 'Bye from ClawAgent.' <<<"$OUTPUT" || { echo 'missing clean shutdown'; exit 1; }

echo 'claw agent tui test passed'
