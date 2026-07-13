#!/bin/bash
#
# Black-box E2E Test for AgentFlow Interactive Runner
#
# This test drives agent_runner in interactive mode and validates the
# command shell surfaces expected workflow/status output.
#
# Usage: ./test_blackbox_interactive_runner.sh [agent_runner_path] [package_path]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AGENT_RUNNER="${1:-$PROJECT_ROOT/build/agent_runner}"
PACKAGE_PATH="${2:-$PROJECT_ROOT/examples/agent/outfit-test-agent}"

log() {
  echo "[interactive-test] $1"
}

assert_contains() {
  local output="$1"
  local expected="$2"
  local name="$3"
  if echo "$output" | grep -q "$expected"; then
    log "PASS: $name"
  else
    log "FAIL: $name"
    echo "$output" | head -40
    exit 1
  fi
}

if [ ! -x "$AGENT_RUNNER" ]; then
  log "agent_runner not found or not executable: $AGENT_RUNNER"
  exit 1
fi

if [ ! -d "$PACKAGE_PATH" ]; then
  log "Package directory not found: $PACKAGE_PATH"
  exit 1
fi

log "Running interactive shell smoke flow"
OUTPUT=$(printf 'list\nstatus\nquit\n' | "$AGENT_RUNNER" "$PACKAGE_PATH" --interactive 2>/dev/null)

assert_contains "$OUTPUT" "AgentFlow Interactive Mode" "interactive banner"
assert_contains "$OUTPUT" "Available commands:" "help text printed"
assert_contains "$OUTPUT" "=== Workflows ===" "workflow listing"
assert_contains "$OUTPUT" "\[coordinator-workflow\]" "coordinator workflow available"
assert_contains "$OUTPUT" "=== Status ===" "status block"
assert_contains "$OUTPUT" "Runtime running: yes" "runtime status"
assert_contains "$OUTPUT" "Goodbye!" "clean shutdown"

log "All interactive runner checks passed"
