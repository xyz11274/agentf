#!/usr/bin/env bash
# Dedicated test entry for harness_agent.
#
# Runs two layers (see docs/enhancements/038-*.md §9):
#   1. Unit tests over the pure decide_next core (loop / guards / termination).
#   2. Black-box end-to-end test driving the REAL framework loop.
#
# Self-contained: builds the AgentFlow library if needed and compiles the
# standalone driver. No AgentFlow source or root CMake changes.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AGENT_DIR="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$AGENT_DIR/../../.." && pwd)"
cd "$ROOT"

RC=0

echo "=============================================="
echo "[harness-tests] Layer 1: unit tests (decide_next core)"
echo "=============================================="
( cd "$AGENT_DIR" && python3 -m unittest discover -s tests/unit -v ) || RC=1

echo ""
echo "=============================================="
echo "[harness-tests] Layer 2: black-box loop test"
echo "=============================================="

LIBDIR="$ROOT/build/lib"
if [ ! -f "$LIBDIR/libagentflow.dylib" ] && [ ! -f "$LIBDIR/libagentflow.so" ]; then
  echo "[harness-tests] Building AgentFlow library (one-time)..."
  ./build.sh build || { echo "[harness-tests] library build failed"; exit 1; }
fi

# Compile the standalone driver (idempotent).
mkdir -p "$AGENT_DIR/build"
DRIVER="$AGENT_DIR/build/harness_runner"
echo "[harness-tests] Compiling harness_runner..."
if ! c++ -std=c++17 -O2 \
      -I "$ROOT/include/external" -I "$ROOT/include/internal" \
      "$AGENT_DIR/driver/harness_runner.cpp" \
      -L "$LIBDIR" -lagentflow -Wl,-rpath,"$LIBDIR" \
      -o "$DRIVER"; then
  echo "[harness-tests] driver compile failed"
  exit 1
fi

bash "$HERE/blackbox/test_harness_dialog.sh" || RC=1

echo ""
if [ "$RC" -eq 0 ]; then
  echo "[harness-tests] ALL TESTS PASSED"
else
  echo "[harness-tests] SOME TESTS FAILED"
fi
exit "$RC"
