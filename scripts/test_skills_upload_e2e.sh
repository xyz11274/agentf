#!/bin/bash
# ============================================================================
# AgentFlow Skills Upload E2E Test
# ----------------------------------------------------------------------------
# Validates: agent.upload_skills, agent.list_skills, agent.remove_skill
#
# Usage:    ./scripts/test_skills_upload_e2e.sh
# Env vars: AF_HOST (127.0.0.1) | AF_PORT (7890) | AF_AGENT_ID (harness-agent)
# Exit:     0 = all pass, 1 = some fail, 2 = missing prerequisite tool
# ============================================================================

set -euo pipefail

HOST="${AF_HOST:-127.0.0.1}"
PORT="${AF_PORT:-7890}"
AGENT_ID="${AF_AGENT_ID:-harness-agent}"

SKILL_NAME="test-skill"
PASS=0
FAIL=0
SKIP=0

record() {
    local status="$1"
    local desc="$2"
    case "$status" in
        PASS) echo "  ✅ PASS: $desc"; PASS=$((PASS + 1)) ;;
        FAIL) echo "  ❌ FAIL: $desc"; FAIL=$((FAIL + 1)) ;;
        SKIP) echo "  ⚠️  SKIP: $desc"; SKIP=$((SKIP + 1)) ;;
    esac
}

# Embed a string into a JSON value: backslash + double-quote + newline escapes.
json_escape() {
    local s="$1"
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import json,sys; print(json.dumps(sys.argv[1])[1:-1])' "$s"
    else
        printf '%s' "$s" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/\\t/g' -e ':a;N;$!ba;s/\n/\\n/g'
    fi
}

rpc_call() {
    local method="$1"
    local params="$2"
    local req
    req=$(printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":1}' "$method" "$params")
    # -N: half-close stdin so server sees end-of-request and replies.
    printf '%s\n' "$req" | nc -w 5 -N "$HOST" "$PORT" 2>/dev/null \
        || echo '{"jsonrpc":"2.0","error":{"code":-1,"message":"connection failed"},"id":1}'
}

require_tool() {
    local tool="$1"
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "  ❌ FAIL: required tool '$tool' not found in PATH"
        echo ""
        echo "=== Aborted: missing prerequisites ($FAIL failed, $SKIP skipped) ==="
        exit 2
    fi
}

echo "=== AgentFlow Skills Upload E2E Test ==="
echo ""
echo "  Target : http://$HOST:$PORT"
echo "  Agent  : $AGENT_ID"
echo "  Skill  : $SKILL_NAME"
echo ""

echo "[1/8] Checking prerequisites..."
require_tool nc
record PASS "nc available"
require_tool zip
record PASS "zip available"
require_tool base64
record PASS "base64 available"

TMP_DIR="$(mktemp -d -t af-skills-e2e.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT
record PASS "temp dir created ($TMP_DIR)"

echo ""
echo "[2/8] Building test skill package..."

SKILL_DIR="$TMP_DIR/$SKILL_NAME"
mkdir -p "$SKILL_DIR"

cat > "$SKILL_DIR/SKILL.md" << 'EOF'
---
name: test-skill
description: E2E test skill
compatibility: opencode
---
# Test Skill
This is an E2E test skill.
EOF

if [ -s "$SKILL_DIR/SKILL.md" ]; then
    record PASS "SKILL.md written"
else
    record FAIL "SKILL.md written"
fi

ZIP_FILE="$TMP_DIR/skills-upload.zip"
(
    cd "$TMP_DIR"
    zip -qr "$ZIP_FILE" "$SKILL_NAME"
)

if [ -s "$ZIP_FILE" ]; then
    SIZE=$(stat -f%z "$ZIP_FILE" 2>/dev/null || stat -c%s "$ZIP_FILE" 2>/dev/null)
    record PASS "skill archived ($SIZE bytes)"
else
    record FAIL "skill archived"
fi

echo ""
echo "[3/8] Uploading skill via agent.upload_skills..."

B64_PAYLOAD="$(base64 < "$ZIP_FILE" | tr -d '\n')"
B64_ESCAPED="$(json_escape "$B64_PAYLOAD")"

UPLOAD_PARAMS="{\"agent_id\":\"$AGENT_ID\",\"skills_data\":\"$B64_ESCAPED\"}"
UPLOAD_RES="$(rpc_call "agent.upload_skills" "$UPLOAD_PARAMS" || true)"

if [ -z "$UPLOAD_RES" ]; then
    record FAIL "agent.upload_skills returned a response"
    echo "         (empty response - is the server reachable?)"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
    exit 1
fi
record PASS "agent.upload_skills returned a response"

if printf '%s' "$UPLOAD_RES" | grep -q '"error"'; then
    record FAIL "agent.upload_skills succeeded (no error)"
    echo "         response: $UPLOAD_RES"
else
    record PASS "agent.upload_skills succeeded (no error)"
fi

if printf '%s' "$UPLOAD_RES" | grep -q "uploaded_skills"; then
    record PASS "response contains 'uploaded_skills'"
else
    record FAIL "response contains 'uploaded_skills'"
    echo "         response: $UPLOAD_RES"
fi

echo ""
echo "[4/8] Verifying skill appears in agent.list_skills..."

LIST_PARAMS="{\"agent_id\":\"$AGENT_ID\"}"
LIST_RES="$(rpc_call "agent.list_skills" "$LIST_PARAMS" || true)"

if [ -n "$LIST_RES" ]; then
    record PASS "agent.list_skills returned a response"
else
    record FAIL "agent.list_skills returned a response"
fi

if printf '%s' "$LIST_RES" | grep -q "test-skill"; then
    record PASS "test-skill appears in list_skills"
else
    record FAIL "test-skill appears in list_skills"
    echo "         response: $LIST_RES"
fi

echo ""
echo "[5/8] Removing skill via agent.remove_skill..."

REMOVE_PARAMS="{\"agent_id\":\"$AGENT_ID\",\"skill_name\":\"$SKILL_NAME_ESCAPED\"}"
REMOVE_RES="$(rpc_call "agent.remove_skill" "$REMOVE_PARAMS" || true)"

if [ -n "$REMOVE_RES" ]; then
    record PASS "agent.remove_skill returned a response"
else
    record FAIL "agent.remove_skill returned a response"
fi

if printf '%s' "$REMOVE_RES" | grep -q '"error"'; then
    record FAIL "agent.remove_skill succeeded (no error)"
    echo "         response: $REMOVE_RES"
else
    record PASS "agent.remove_skill succeeded (no error)"
fi

echo ""
echo "[6/8] Confirming skill is gone after remove..."

LIST_PARAMS="{\"agent_id\":\"$AGENT_ID\"}"
LIST_RES2="$(rpc_call "agent.list_skills" "$LIST_PARAMS" || true)"

if printf '%s' "$LIST_RES2" | grep -q "test-skill"; then
    record FAIL "test-skill no longer in list_skills"
else
    record PASS "test-skill no longer in list_skills"
fi

echo ""
echo "[7/8] Cleaning up..."
rm -rf "$TMP_DIR"
trap - EXIT
record PASS "temp files removed"

echo ""
echo "[8/8] Done"
echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ "$FAIL" -eq 0 ]; then
    exit 0
else
    exit 1
fi