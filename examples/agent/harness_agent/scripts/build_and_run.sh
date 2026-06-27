#!/usr/bin/env bash
# Self-contained build + run for harness_agent.
# - Ensures libagentflow is built (existing project build; no source changes).
# - Compiles the standalone harness_runner driver against the prebuilt library.
# - Runs the harness agent interactively.
#
# All artifacts stay under examples/agent/harness_agent (no root CMake edits).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AGENT_DIR="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$AGENT_DIR/../../.." && pwd)"
cd "$ROOT"

# 1. Build the AgentFlow library if needed (builds existing targets only).
if [ ! -f "$ROOT/build/lib/libagentflow.dylib" ] && [ ! -f "$ROOT/build/lib/libagentflow.so" ]; then
  echo "[harness] Building AgentFlow (one-time)..."
  ./build.sh build
fi

# 2. Compile the standalone driver.
BIN_DIR="$AGENT_DIR/build"
mkdir -p "$BIN_DIR"
DRIVER_BIN="$BIN_DIR/harness_runner"

LIBDIR="$ROOT/build/lib"
echo "[harness] Compiling harness_runner..."
c++ -std=c++17 -O2 \
  -I "$ROOT/include/external" \
  -I "$ROOT/include/internal" \
  "$AGENT_DIR/driver/harness_runner.cpp" \
  -L "$LIBDIR" -lagentflow \
  -Wl,-rpath,"$LIBDIR" \
  -o "$DRIVER_BIN"

echo "[harness] Driver built: $DRIVER_BIN"

# 3. Run.
export DYLD_LIBRARY_PATH="$LIBDIR:${DYLD_LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="$LIBDIR:${LD_LIBRARY_PATH:-}"
exec "$DRIVER_BIN" "examples/agent/harness_agent/package" "$@"
