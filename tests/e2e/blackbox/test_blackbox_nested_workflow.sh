#!/bin/bash
#
# Black-box E2E Test for AgentFlow Nested Workflow Agent
#
# This test runs agent_runner as an independent process and validates
# the execution output matches expected results without any knowledge
# of the internal implementation.
#
# Usage: ./test_blackbox_nested_workflow.sh [agent_runner_path] [package_path]
#

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AGENT_RUNNER="${1:-$PROJECT_ROOT/build/agent_runner}"
PACKAGE_PATH="${2:-$PROJECT_ROOT/examples/agent/nested_workflow_agent}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
TESTS_PASSED=0
TESTS_FAILED=0

# Helper functions
log_info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

check_output_contains() {
    local output="$1"
    local expected="$2"
    local test_name="$3"

    if echo "$output" | grep -q "$expected"; then
        log_pass "$test_name"
        return 0
    else
        log_fail "$test_name - expected: '$expected'"
        echo "  Actual output snippet:"
        echo "$output" | head -20 | sed 's/^/    /'
        return 1
    fi
}

# Verify prerequisites
log_info "=== Black-box E2E Test: Nested Workflow Agent ==="
log_info "Agent Runner: $AGENT_RUNNER"
log_info "Package Path: $PACKAGE_PATH"

if [ ! -x "$AGENT_RUNNER" ]; then
    log_fail "agent_runner not found or not executable: $AGENT_RUNNER"
    exit 1
fi

if [ ! -d "$PACKAGE_PATH" ]; then
    log_fail "Package directory not found: $PACKAGE_PATH"
    exit 1
fi

# ============================================================
# Test 1: Package Validation
# ============================================================
log_info "\n--- Test 1: Package Validation ---"

OUTPUT=$("$AGENT_RUNNER" "$PACKAGE_PATH" --validate 2>/dev/null || true)

check_output_contains "$OUTPUT" "nested-workflow-agent" "Package name correct"
check_output_contains "$OUTPUT" "1.0.0" "Package version correct"
check_output_contains "$OUTPUT" "Workflows: 4" "Workflow count correct"
check_output_contains "$OUTPUT" "Package validation passed" "Validation success"

# ============================================================
# Test 2: List Workflows
# ============================================================
log_info "\n--- Test 2: List Workflows ---"

OUTPUT=$("$AGENT_RUNNER" "$PACKAGE_PATH" --list-workflows 2>/dev/null || true)

check_output_contains "$OUTPUT" "\[main-workflow\]" "Main workflow listed"
check_output_contains "$OUTPUT" "\[data-pipeline-workflow\]" "Data pipeline workflow listed"
check_output_contains "$OUTPUT" "\[validation-workflow\]" "Validation workflow listed"
check_output_contains "$OUTPUT" "\[processing-workflow\]" "Processing workflow listed"

# Check workflow_ref references
check_output_contains "$OUTPUT" "workflow_ref: data_pipeline.json" "Main -> data_pipeline reference"
check_output_contains "$OUTPUT" "workflow_ref: processing_workflow.json" "Main -> processing reference"
check_output_contains "$OUTPUT" "workflow_ref: validation_workflow.json" "Data pipeline -> validation reference"

# ============================================================
# Test 3: Execute Main Workflow
# ============================================================
log_info "\n--- Test 3: Execute Main Workflow ---"

OUTPUT=$("$AGENT_RUNNER" "$PACKAGE_PATH" --execute main-workflow 2>/dev/null || true)

# Verify execution started
check_output_contains "$OUTPUT" "Executing Workflow: main-workflow" "Workflow execution started"

# Verify execution order (topological)
check_output_contains "$OUTPUT" "\[SKIP\] start" "Start node skipped"
check_output_contains "$OUTPUT" "\[NESTED\] data_pipeline" "Data pipeline nested node"
check_output_contains "$OUTPUT" "\[NESTED\] final_processing" "Final processing nested node"
check_output_contains "$OUTPUT" "\[SKIP\] end" "End node skipped"

# Verify execution completed
check_output_contains "$OUTPUT" "Workflow execution completed" "Workflow completed"

# ============================================================
# Test 4: Execute Data Pipeline Workflow
# ============================================================
log_info "\n--- Test 4: Execute Data Pipeline Workflow ---"

OUTPUT=$("$AGENT_RUNNER" "$PACKAGE_PATH" --execute data-pipeline-workflow 2>/dev/null || true)

# Verify shell script execution output
check_output_contains "$OUTPUT" "Fetching data from source" "fetch_data.sh executed"
check_output_contains "$OUTPUT" "Transforming data" "transform_data.sh executed"

# Verify nested workflow reference
check_output_contains "$OUTPUT" "\[NESTED\] validate_data" "Nested validation workflow referenced"

# ============================================================
# Test 5: Execute Validation Workflow (Deepest Level)
# ============================================================
log_info "\n--- Test 5: Execute Validation Workflow (Deepest Level) ---"

OUTPUT=$("$AGENT_RUNNER" "$PACKAGE_PATH" --execute validation-workflow 2>/dev/null || true)

# Verify all shell scripts executed with correct output
check_output_contains "$OUTPUT" "Checking data schema" "check_schema.sh executed"
check_output_contains "$OUTPUT" "Validating field values" "validate_fields.sh executed"
check_output_contains "$OUTPUT" "Validation passed: all checks completed" "validation_report.sh executed"

# Verify execution success count
check_output_contains "$OUTPUT" "Success: 3 nodes" "All 3 nodes executed successfully"

# ============================================================
# Test 6: Execute Processing Workflow
# ============================================================
log_info "\n--- Test 6: Execute Processing Workflow ---"

OUTPUT=$("$AGENT_RUNNER" "$PACKAGE_PATH" --execute processing-workflow 2>/dev/null || true)

# Verify all shell scripts executed
check_output_contains "$OUTPUT" "Aggregating processed data" "aggregate_data.sh executed"
check_output_contains "$OUTPUT" "Generating final output" "generate_output.sh executed"
check_output_contains "$OUTPUT" "Results persisted successfully" "persist_results.sh executed"

check_output_contains "$OUTPUT" "Success: 3 nodes" "All 3 nodes executed successfully"

# ============================================================
# Test 7: Return Code Validation
# ============================================================
log_info "\n--- Test 7: Return Code Validation ---"

# Valid package should return 0 for --validate
"$AGENT_RUNNER" "$PACKAGE_PATH" --validate >/dev/null 2>&1
if [ $? -eq 0 ]; then
    log_pass "Validate returns 0 for valid package"
else
    log_fail "Validate should return 0 for valid package"
fi

# Execute should return 0 for successful execution
"$AGENT_RUNNER" "$PACKAGE_PATH" --execute validation-workflow >/dev/null 2>&1
if [ $? -eq 0 ]; then
    log_pass "Execute returns 0 for successful workflow"
else
    log_fail "Execute should return 0 for successful workflow"
fi

# Invalid package should return non-zero
set +e
"$AGENT_RUNNER" "/nonexistent/path" --validate >/dev/null 2>&1
EXIT_CODE=$?
set -e
if [ $EXIT_CODE -ne 0 ]; then
    log_pass "Returns non-zero for invalid package path"
else
    log_fail "Should return non-zero for invalid package path"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "============================================"
echo "Test Summary: $TESTS_PASSED passed, $TESTS_FAILED failed"
echo "============================================"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}All black-box tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi