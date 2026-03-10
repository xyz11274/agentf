#!/bin/bash
# Coordinator Node - Coordinates LLM queries and forwards to skill
# Input: AF_INPUT environment variable (user message from dispatcher)
# Output: JSON with prompt to trigger skill execution

set -e

# Read input from environment variable
INPUT="${AF_INPUT:-${AGENTFLOW_INPUT:-}}"

# Debug output to stderr
echo "[COORDINATOR_NODE] Received input: '$INPUT'" >&2

# Forward to skill with prompt
echo "{\"status\": \"SUCCESS\", \"prompt\": \"${INPUT}\"}"

exit 0