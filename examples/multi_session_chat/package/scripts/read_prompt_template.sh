#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cp "${ROOT}/prompts/session_summary_prompt.txt" "${ROOT}/runtime/prompt_loaded.txt"
echo "prompt loaded"
