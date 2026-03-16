#!/bin/bash
# Shell Data Verifier Node
# Reads data from Go processor, verifies the chain, and outputs final result
# Uses databus-cli for DataBus access via UDS

set -e

# Get inputs from environment (AF_* convention)
SESSION_ID="${AF_SESSION_ID:-}"
DIALOGUE_ID="${AF_DIALOGUE_ID:-}"
DATABUS_UDS_FILE="${AF_DATABUS_UDS_FILE:-}"

# Log function
log() {
    echo "[SHELL] $1" >&2
}

log "Starting Shell Verifier (session=$SESSION_ID, dialogue=$DIALOGUE_ID)"

# Build chain key
if [ -n "$DIALOGUE_ID" ]; then
    CHAIN_KEY="${DIALOGUE_ID}/chain_data"
    STEP_KEY="${DIALOGUE_ID}/chain_data_step"
else
    CHAIN_KEY="chain_data"
    STEP_KEY="chain_data_step"
fi

# Read data from DataBus via databus-cli (if available)
# Otherwise, fall back to environment variables
if [ -x "$(command -v databus-cli)" ] && [ -n "$DATABUS_UDS_FILE" ]; then
    log "Using databus-cli for DataBus access"
    CHAIN_DATA=$(databus-cli -s "$SESSION_ID" -d "$DIALOGUE_ID" -ns user get "$CHAIN_KEY" 2>/dev/null || echo "")
    CURRENT_STEP=$(databus-cli -s "$SESSION_ID" -d "$DIALOGUE_ID" -ns user get "$STEP_KEY" 2>/dev/null || echo "")
else
    log "Using environment variables for data (UDS not available)"
    CHAIN_DATA="${CHAIN_DATA:-test_data}"
    CURRENT_STEP="${CURRENT_STEP:-unknown}"
fi

log "Read data: '$CHAIN_DATA' (step: $CURRENT_STEP)"

# Verify the chain transformation
VERIFICATION="UNKNOWN"

if [ -n "$CHAIN_DATA" ]; then
    # Check if data has been processed through the expected chain
    if echo "$CHAIN_DATA" | grep -q "GO_PROCESSED"; then
        VERIFICATION="PASS"
        log "Chain verification PASSED - data went through all nodes"
    elif echo "$CHAIN_DATA" | grep -q "PYTHON_TRANSFORMED"; then
        VERIFICATION="PARTIAL"
        log "Chain verification PARTIAL - data only went through C++ and Python"
    elif echo "$CHAIN_DATA" | grep -q "CPP_DATA"; then
        VERIFICATION="INCOMPLETE"
        log "Chain verification INCOMPLETE - data only from C++ producer"
    else
        VERIFICATION="UNEXPECTED"
        log "Chain verification UNEXPECTED - data format not recognized"
    fi
else
    VERIFICATION="NO_DATA"
    log "Chain verification FAILED - no data found"
fi

# Write verification result to DataBus
if [ -x "$(command -v databus-cli)" ] && [ -n "$DATABUS_UDS_FILE" ]; then
    if [ -n "$DIALOGUE_ID" ]; then
        databus-cli -s "$SESSION_ID" -d "$DIALOGUE_ID" -ns user put "chain_verification" "$VERIFICATION" 2>/dev/null || true
        databus-cli -s "$SESSION_ID" -d "$DIALOGUE_ID" -ns user put "chain_final_data" "$CHAIN_DATA" 2>/dev/null || true
    else
        databus-cli -s "$SESSION_ID" -ns user put "chain_verification" "$VERIFICATION" 2>/dev/null || true
        databus-cli -s "$SESSION_ID" -ns user put "chain_final_data" "$CHAIN_DATA" 2>/dev/null || true
    fi
fi

# Output result as JSON
cat <<EOF
{
  "status": "verified",
  "verification_result": "$VERIFICATION",
  "final_data": "$CHAIN_DATA",
  "session_id": "$SESSION_ID",
  "dialogue_id": "$DIALOGUE_ID"
}
EOF

log "Shell Verifier completed with status: $VERIFICATION"
exit 0