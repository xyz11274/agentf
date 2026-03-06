#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

# -------------------------------------------------------------------
# 1. Build the C++ AgentFlow framework + multi_session_nodes SO library
# -------------------------------------------------------------------
./build.sh build

# -------------------------------------------------------------------
# 2. Build the Java skill-delegate JAR (requires javac + jar in PATH)
# -------------------------------------------------------------------
JAVA_DIR="$ROOT/examples/multi_session_chat/nodes/java"
if command -v javac &>/dev/null; then
  echo "[build] Compiling SkillDelegateNode.java ..."
  (cd "$JAVA_DIR" && javac -encoding UTF-8 SkillDelegateNode.java && \
    jar cfe skill_delegate.jar SkillDelegateNode SkillDelegateNode.class)
  echo "[build] skill_delegate.jar ready."
else
  echo "[build] WARNING: javac not found — skipping Java node build (skill-delegate-workflow will fall back to shell mode)."
fi

# -------------------------------------------------------------------
# 3. Verify Go is available for session-context-workflow (no pre-compile needed;
#    nodes/go/session_context/main.go is executed with 'go run' at runtime)
# -------------------------------------------------------------------
if ! command -v go &>/dev/null; then
  echo "[build] WARNING: go not found — session-context-workflow Go nodes will fail at runtime."
fi

# -------------------------------------------------------------------
# 4. Start the chat server
# -------------------------------------------------------------------
./build/example_chat_server \
  --package examples/multi_session_chat/package \
  --web-root examples/multi_session_chat/server/webui \
  --host 127.0.0.1 --http-port 8088 --ws-port 8765
