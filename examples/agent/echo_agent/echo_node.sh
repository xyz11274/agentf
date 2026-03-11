#!/bin/bash
# Echo Node - Simple echo implementation
# Input: AF_INPUT environment variable
# Output: JSON to stdout
#
# For skill task: output skill result with response
# For coordinator task: output final response
# Note: Dispatcher now uses built-in next_skills mechanism (no workflow execution)

set -e

# Read input from environment variable
INPUT="${AF_INPUT:-${AGENTFLOW_INPUT:-}}"

# Debug output to stderr
echo "[ECHO_NODE] Received input: '$INPUT'" >&2

# Generate response
if [ -n "$INPUT" ]; then
    RESPONSE="Hello ${INPUT}!"
else
    RESPONSE="Hello World!"
fi

# Output JSON result
# For skill: output 'response' field (framework forwards to Coordinator)
# For coordinator: output final response
echo "{\"status\": \"SUCCESS\", \"response\": \"${RESPONSE}\", \"input\": \"${INPUT}\"}"

exit 0