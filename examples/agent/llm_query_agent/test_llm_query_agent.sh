#!/bin/bash
# Black-box test script for LLM Query Agent
#
# This script tests the agent through its message routing interface,
# simulating real user interaction through the dispatcher/coordinator/skill flow.
#
# Input:  User prompt text (command line argument or stdin)
# Output: LLM response text
#
# Usage:
#   ./test_llm_query_agent.sh "What is 2+2?"
#   echo "What is 2+2?" | ./test_llm_query_agent.sh
#
# Environment variables:
#   LLM_API_KEY   - API key (default: test key)
#   LLM_MODEL     - Model name (default: qwen3.5-plus)

set -e

# Configuration
API_KEY="${LLM_API_KEY:-sk-07d3257b46ca4f1d83340a46ed2c7ec5}"
MODEL="${LLM_MODEL:-qwen3.5-plus}"
BASE_URL="${LLM_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}"

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
AGENT_DIR="${SCRIPT_DIR}"
LLM_QUERY_LIB="/root/projects/llm-query/build/src/lib"

# Environment
export LLM_API_KEY="${API_KEY}"
export LLM_MODEL="${MODEL}"
export LLM_BASE_URL="${BASE_URL}"
export LD_LIBRARY_PATH="${LLM_QUERY_LIB}:${BUILD_DIR}/lib:${LD_LIBRARY_PATH:-}"

# INPUT: Get prompt from argument or stdin
if [ -n "$1" ]; then
    INPUT="$1"
else
    INPUT=$(cat)
fi

# Validate input
if [ -z "$INPUT" ]; then
    echo "Error: No input provided" >&2
    exit 1
fi

# Change to agent directory (required for shell scripts to be found)
cd "${AGENT_DIR}"

# Run interactive runner and send user message through agent message interface
# The agent will route through dispatcher -> coordinator -> skill
OUTPUT=$(
    echo "${INPUT}" | \
    "${BUILD_DIR}/agent_runner" . --interactive 2>&1
)

# Print OUTPUT
echo "${OUTPUT}"