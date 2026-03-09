#!/bin/bash
# Echo Node - Simple echo implementation
# Input: AF_INPUT environment variable
# Output: JSON to stdout
#
# For dispatcher task: output selected_skills to trigger coordinator
# For coordinator task: output final response
# For skill task: output skill result

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
# Include 'selected_skills' for dispatcher workflow to trigger coordinator
# The framework checks if selected_skills is set to route message to coordinator
echo "{\"status\": \"SUCCESS\", \"selected_skills\": \"echo-skill\", \"response\": \"${RESPONSE}\", \"input\": \"${INPUT}\"}"

exit 0