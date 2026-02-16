#!/bin/bash
# Test shell script for hot reload functionality
# Version: 1.0

set -e

# Print configuration
echo "[SHELL_SCRIPT] Version 1.0"
echo "[SHELL_SCRIPT] Arguments: $@"
echo "[SHELL_SCRIPT] Environment:"
env | grep -E "^(HOME|USER|PATH)" | while read -r line; do
    echo "[SHELL_SCRIPT]   $line"
done

# Output some data
echo "[SHELL_SCRIPT] Output: result=success"
echo "[SHELL_SCRIPT] Output: status=ok"
echo "[SHELL_SCRIPT] Output: timestamp=$(date +%s)"

# Sleep briefly to simulate work
sleep 0.1

echo "[SHELL_SCRIPT] Execution complete"
