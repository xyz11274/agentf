#!/bin/bash
# DEPRECATED: This file is no longer used. Dispatcher now uses built-in functionality with next_skills configuration.
#
# Dispatcher Node - Routes user requests to coordinator
# Input: AF_INPUT environment variable (user message)
# Output: JSON with selected_skills to trigger coordinator -> skill

set -e

# Read input from environment variable
INPUT="${AF_INPUT:-${AGENTFLOW_INPUT:-}}"

# Debug output to stderr
echo "[DISPATCHER_NODE] Received input: '$INPUT'" >&2

# Route to llm-query-skill
# selected_skills format: JSON object with "skill" key for each skill
# The coordinator parses this to schedule the skill workflow
echo "{\"status\": \"SUCCESS\", \"selected_skills\": \"{\\\"skill\\\": \\\"llm-query-skill\\\"}\", \"input\": \"${INPUT}\"}"

exit 0