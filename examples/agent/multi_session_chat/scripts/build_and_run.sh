#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"

./build.sh build

JAVA_DIR="$ROOT/examples/agent/multi_session_chat/nodes/java"
if javac -version &>/dev/null 2>&1; then
  echo "[build] Compiling SkillDelegateNode.java ..."
  (cd "$JAVA_DIR" && javac -encoding UTF-8 SkillDelegateNode.java && \
    jar cfe skill_delegate.jar SkillDelegateNode SkillDelegateNode.class)
  echo "[build] skill_delegate.jar ready."
else
  echo "[build] WARNING: Java JDK not found — skipping Java node build (skill-delegate-workflow will fall back if selected)."
fi

if ! command -v go &>/dev/null; then
  echo "[build] WARNING: go not found — session-context-workflow Go nodes will fail at runtime."
fi

./build/example_multi_session_chat_tui examples/agent/multi_session_chat/package
