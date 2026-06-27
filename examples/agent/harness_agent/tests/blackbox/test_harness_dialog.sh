#!/usr/bin/env bash
# Black-box end-to-end test for the harness_agent loop.
#
# Drives the REAL framework loop via the standalone harness_runner and asserts
# the harness semantics: multi-iteration loop, result-aware progression,
# termination, dialogue isolation, and the max_iterations guard.
#
# Deterministic & offline (no LLM). Assumes the driver is already built by
# run_tests.sh. All UTF-8 content assertions are done in Python (locale-safe);
# bash greps stay ASCII-only.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AGENT_DIR="$(cd "$HERE/../.." && pwd)"
ROOT="$(cd "$AGENT_DIR/../../.." && pwd)"
cd "$ROOT"

PKG="examples/agent/harness_agent/package"
DRIVER="$AGENT_DIR/build/harness_runner"
LIBDIR="$ROOT/build/lib"
export DYLD_LIBRARY_PATH="$LIBDIR:${DYLD_LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="$LIBDIR:${LD_LIBRARY_PATH:-}"

fail() { echo "[BLACKBOX][FAIL] $1"; exit 1; }
pass() { echo "[BLACKBOX][PASS] $1"; }

[ -x "$DRIVER" ] || fail "driver not built: $DRIVER (run run_tests.sh)"

clean_state() { rm -rf "$PKG/runtime/state" "$PKG/runtime/markers"; }

resp_line() { echo "$1" | grep "HARNESS_RESPONSE:" | head -1; }

# ----------------------------------------------------------------------------
# T1: normal multi-iteration success (retrieve -> analyze -> compose -> finalize)
# ----------------------------------------------------------------------------
clean_state
OUT="$("$DRIVER" "$PKG" --input "PLAN_A_TRIP" --timeout 20 2>/dev/null)"
LINE="$(resp_line "$OUT")"
[ -n "$LINE" ] || fail "T1 no HARNESS_RESPONSE line"
case "$LINE" in *"[no response]"*) fail "T1 driver returned [no response]";; esac

STATE_FILE="$(ls "$PKG"/runtime/state/*.json 2>/dev/null | head -1)"
[ -n "$STATE_FILE" ] || fail "T1 no state file produced"
python3 - "$STATE_FILE" "$LINE" <<'PY' || exit 1
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
line = sys.argv[2]
results = data.get("results", {})
for s in ("retrieve", "analyze", "compose", "finalize"):
    assert s in results, "missing skill result: " + s
state = data.get("state", {})
assert int(state.get("iteration", 0)) >= 4, "expected >=4 iterations, got %s" % state.get("iteration")
# result-aware: analyze derived from retrieve (not a static sequence)
assert "依据" in results["analyze"]["result"], "analyze not derived from retrieve"
# final response present and references the loop closure + user request
assert "最终答复" in line, "response missing final marker"
assert "闭环" in line, "response missing loop-closure marker"
assert "PLAN_A_TRIP" in line, "response did not reference user request"
print("[BLACKBOX] T1 assertions ok")
PY
pass "T1 normal multi-iteration loop + termination"

# ----------------------------------------------------------------------------
# T2: two dialogues in one session stay isolated
# ----------------------------------------------------------------------------
clean_state
OUT="$(printf 'TRIP_REQUEST\nSUMMARY_REQUEST\n/quit\n' | "$DRIVER" "$PKG" --timeout 20 2>/dev/null)"
N=$(echo "$OUT" | grep -c "HARNESS_RESPONSE:")
[ "$N" -eq 2 ] || fail "T2 expected 2 responses, got $N"
RESP_LINES="$(echo "$OUT" | grep "HARNESS_RESPONSE:")"
python3 - "$RESP_LINES" <<'PY' || exit 1
import sys
lines = [l.strip() for l in sys.argv[1].splitlines() if l.strip()]
assert len(lines) == 2, "expected 2 response lines, got %d" % len(lines)
assert "TRIP_REQUEST" in lines[0], "first dialogue response contaminated: " + lines[0]
assert "SUMMARY_REQUEST" in lines[1], "second dialogue response contaminated: " + lines[1]
assert "TRIP_REQUEST" not in lines[1] and "SUMMARY_REQUEST" not in lines[0], "cross-dialogue contamination"
print("[BLACKBOX] T2 isolation assertions ok")
PY
pass "T2 two-dialogue isolation"

# ----------------------------------------------------------------------------
# T3: max_iterations guard emits a fallback response + terminated_reason
# ----------------------------------------------------------------------------
CFG="$PKG/config/harness_config.json"
BACKUP="$(cat "$CFG")"
trap 'printf "%s" "$BACKUP" > "$CFG"' EXIT
python3 - "$CFG" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p, encoding="utf-8"))
d["max_iterations"] = 2
json.dump(d, open(p, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
PY
clean_state
OUT="$("$DRIVER" "$PKG" --input "COMPLEX_ITINERARY" --timeout 20 2>/dev/null)"
LINE="$(resp_line "$OUT")"
case "$LINE" in *"[no response]"*) fail "T3 no response under guard";; esac
STATE_FILE="$(ls "$PKG"/runtime/state/*.json 2>/dev/null | head -1)"
python3 - "$STATE_FILE" <<'PY' || exit 1
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
reason = data.get("state", {}).get("terminated_reason", "")
assert reason == "max_iterations", "expected max_iterations guard, got '%s'" % reason
print("[BLACKBOX] T3 guard assertion ok (terminated_reason=max_iterations)")
PY
printf "%s" "$BACKUP" > "$CFG"
trap - EXIT
pass "T3 max_iterations guard with fallback response"

clean_state
echo "[BLACKBOX] all harness dialog tests passed"
