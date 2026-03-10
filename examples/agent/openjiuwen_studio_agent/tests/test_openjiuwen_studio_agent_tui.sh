#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"

rm -rf examples/agent/openjiuwen_studio_agent/runtime
OUTPUT=$(printf '你好
介绍一下你的 Studio 编排
总结上一条消息
/status
/quit
' | ./build/example_openjiuwen_studio_agent_tui examples/agent/openjiuwen_studio_agent 2>/dev/null)

grep -q 'OpenJiuwen Studio Agent TUI' <<<"$OUTPUT" || { echo 'missing tui banner'; exit 1; }
grep -q '默认 dispatcher/coordinator/skill/contextupdater 壳' <<<"$OUTPUT" || { echo 'missing greeting response'; exit 1; }
grep -q 'Studio bundle 中包含 1 个工作流' <<<"$OUTPUT" || { echo 'missing studio bundle summary'; exit 1; }
grep -q '你上一条消息是：介绍一下你的 Studio 编排' <<<"$OUTPUT" || { echo 'missing session memory summary'; exit 1; }
grep -q '已通过适配器加载 Studio bundle' <<<"$OUTPUT" || { echo 'missing status response'; exit 1; }
grep -q 'Bye from OpenJiuwen Studio Agent.' <<<"$OUTPUT" || { echo 'missing clean shutdown'; exit 1; }

echo 'openjiuwen studio agent tui test passed'
