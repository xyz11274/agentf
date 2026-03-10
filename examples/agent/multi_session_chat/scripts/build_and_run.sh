#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"

# -------------------------------------------------------------------
# 1. Build the C++ AgentFlow framework + multi_session_nodes SO library
# -------------------------------------------------------------------
./build.sh build

# -------------------------------------------------------------------
# 2. Build the Java skill-delegate JAR (requires a working JDK in PATH)
# -------------------------------------------------------------------
JAVA_DIR="$ROOT/examples/agent/multi_session_chat/nodes/java"
if javac -version &>/dev/null 2>&1; then
  echo "[build] Compiling SkillDelegateNode.java ..."
  (cd "$JAVA_DIR" && javac -encoding UTF-8 SkillDelegateNode.java && \
    jar cfe skill_delegate.jar SkillDelegateNode SkillDelegateNode.class)
  echo "[build] skill_delegate.jar ready."
else
  echo "[build] WARNING: Java JDK not found — skipping Java node build (skill-delegate-workflow will fall back to shell mode)."
fi

# -------------------------------------------------------------------
# 3. Verify Go is available for session-context-workflow (no pre-compile needed;
#    nodes/go/session_context/main.go is executed with 'go run' at runtime)
# -------------------------------------------------------------------
if ! command -v go &>/dev/null; then
  echo "[build] WARNING: go not found — session-context-workflow Go nodes will fail at runtime."
fi

# -------------------------------------------------------------------
# 4. Clear any stale server processes that may hold the listen ports
# -------------------------------------------------------------------
HTTP_PORT=8088
WS_PORT=8765
for PORT in $HTTP_PORT $WS_PORT; do
  STALE_PID=$(lsof -ti :$PORT 2>/dev/null | head -1 || true)
  if [ -n "$STALE_PID" ]; then
    echo "[build] Stopping stale process on port $PORT (PID $STALE_PID) ..."
    kill "$STALE_PID" 2>/dev/null || true
    sleep 1
  fi
done

# -------------------------------------------------------------------
# 5. Start the chat server
# -------------------------------------------------------------------
./build/example_chat_server \
  --package examples/agent/multi_session_chat/package \
  --web-root examples/agent/multi_session_chat/server/webui \
  --host 127.0.0.1 --http-port 8088 --ws-port 8765
