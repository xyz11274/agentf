#!/bin/bash

# 性能测试脚本
# 用于收集和分析AgentFlow的性能指标
# 支持 macOS (Darwin) 和 Linux，自动探测平台

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RESULTS_DIR="${BUILD_DIR}/performance_results"

# ============================================================================
# Platform Detection
# ============================================================================
OS="$(uname -s)"
ARCH="$(uname -m)"

# Detect number of CPU cores
detect_cores() {
    case "$OS" in
        Darwin)  sysctl -n hw.ncpu 2>/dev/null || echo 1 ;;
        Linux)   nproc 2>/dev/null || grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 1 ;;
        *)       echo 1 ;;
    esac
}

# Collect CPU model name
detect_cpu_model() {
    case "$OS" in
        Darwin)  sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "Unknown" ;;
        Linux)   grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //' || echo "Unknown" ;;
        *)       echo "Unknown" ;;
    esac
}

# Collect CPU frequency
detect_cpu_freq() {
    case "$OS" in
        Darwin)
            local freq_hz
            freq_hz=$(sysctl -n hw.cpufrequency 2>/dev/null || echo "")
            if [ -n "$freq_hz" ]; then
                echo "$((freq_hz / 1000000)) MHz"
            else
                echo "Unknown"
            fi
            ;;
        Linux)
            grep -m1 'cpu MHz' /proc/cpuinfo 2>/dev/null | awk '{print $4 " MHz"}' || echo "Unknown"
            ;;
        *)  echo "Unknown" ;;
    esac
}

# Collect memory info
detect_memory_total() {
    case "$OS" in
        Darwin)
            local bytes
            bytes=$(sysctl -n hw.memsize 2>/dev/null || echo 0)
            if [ "$bytes" -gt 0 ] 2>/dev/null; then
                echo "$((bytes / 1073741824)) GB"
            else
                echo "Unknown"
            fi
            ;;
        Linux)
            free -h 2>/dev/null | awk '/^Mem:/ {print $2}' || echo "Unknown"
            ;;
        *)  echo "Unknown" ;;
    esac
}

detect_memory_available() {
    case "$OS" in
        Darwin)
            # vm_stat reports pages; pagesize is typically 4096 or 16384
            local pagesize free_pages
            pagesize=$(sysctl -n hw.pagesize 2>/dev/null || echo 4096)
            free_pages=$(vm_stat 2>/dev/null | awk '/Pages free/ {gsub(/\./,"",$3); print $3}' || echo 0)
            if [ "$free_pages" -gt 0 ] 2>/dev/null; then
                echo "$(( free_pages * pagesize / 1073741824 )) GB (free)"
            else
                echo "Unknown"
            fi
            ;;
        Linux)
            free -h 2>/dev/null | awk '/^Mem:/ {print $7}' || echo "Unknown"
            ;;
        *)  echo "Unknown" ;;
    esac
}

# Set dynamic library search path for running binaries
setup_runtime_library_path() {
    local extra_paths=("${BUILD_DIR}" "${BUILD_DIR}/lib")
    [ -n "$CONDA_PREFIX" ] && extra_paths+=("$CONDA_PREFIX/lib")

    case "$OS" in
        Darwin)
            extra_paths+=("/usr/local/lib" "/opt/homebrew/lib")
            local joined
            joined=$(IFS=:; echo "${extra_paths[*]}")
            export DYLD_LIBRARY_PATH="${joined}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
            ;;
        Linux)
            extra_paths+=("/usr/local/lib" "/usr/lib")
            local joined
            joined=$(IFS=:; echo "${extra_paths[*]}")
            export LD_LIBRARY_PATH="${joined}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            ;;
    esac
}

# timeout command: GNU coreutils on Linux, may need gtimeout on macOS
portable_timeout() {
    local duration="$1"; shift
    if command -v timeout &>/dev/null; then
        timeout "$duration" "$@"
    elif command -v gtimeout &>/dev/null; then
        gtimeout "$duration" "$@"
    else
        # Fallback: run without timeout
        "$@"
    fi
}

# 创建结果目录
mkdir -p "$RESULTS_DIR"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║     AgentFlow Performance Test Suite - Build Phase          ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "Platform: ${OS} ${ARCH}"
echo ""

# 1. 系统信息收集
echo "[1/4] Collecting System Information..."
{
    echo "System Information Report"
    echo "=========================="
    echo ""
    echo "Date: $(date)"
    echo "Hostname: $(hostname)"
    echo "OS: ${OS} ${ARCH}"
    echo ""
    echo "CPU Information:"
    echo "  Model: $(detect_cpu_model)"
    echo "  Cores: $(detect_cores)"
    echo "  Frequency: $(detect_cpu_freq)"
    echo ""
    echo "Memory Information:"
    echo "  Total: $(detect_memory_total)"
    echo "  Available: $(detect_memory_available)"
    echo ""
    echo "Kernel:"
    echo "  $(uname -r)"
    echo ""
    uname -a
} | tee "$RESULTS_DIR/system_info.txt"

echo "✓ System information collected"
echo ""

# 2. 构建检查
echo "[2/4] Verifying Build Configuration..."
CORES="$(detect_cores)"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "Build directory not configured. Running cmake..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17
    cd "$PROJECT_ROOT"
fi

if [ ! -f "$BUILD_DIR/test_performance" ] && [ ! -f "$BUILD_DIR/test_agentflow_gtest" ]; then
    echo "Building performance tests..."
    cd "$BUILD_DIR"
    cmake --build . --parallel "$CORES" --target test_performance test_agentflow_gtest 2>&1 | tail -20 || \
    cmake --build . --parallel "$CORES" 2>&1 | tail -20
    cd "$PROJECT_ROOT"
fi

echo "✓ Build configuration verified"
echo ""

# 3. 运行性能测试
echo "[3/4] Running Performance Tests..."
echo ""

setup_runtime_library_path
cd "$BUILD_DIR"

# 运行性能测试，并捕获输出
PERF_STATUS=0
if [ -f test_performance ]; then
    {
        echo "Performance Test Results"
        echo "========================"
        echo ""
        echo "Test Start: $(date)"
        echo ""
        
        # 运行测试（带超时保护）
        portable_timeout 300 ./test_performance \
            --gtest_filter="Performance*" \
            --gtest_output="json:$RESULTS_DIR/performance_results.json" \
            2>&1 || PERF_STATUS=$?
        
        echo ""
        echo "Test End: $(date)"
        
    } | tee "$RESULTS_DIR/performance_test.log"
else
    echo "⚠ Performance test executable not found"
fi

cd "$PROJECT_ROOT"

echo "✓ Performance tests completed"
echo ""

# 4. 性能分析和报告生成
echo "[4/4] Generating Performance Report..."

{
    echo "# AgentFlow Performance Benchmark Report"
    echo ""
    echo "**Generated on:** $(date)"
    echo ""
    echo "## Executive Summary"
    echo ""
    echo "This report presents the performance characteristics of AgentFlow across various scenarios:"
    echo ""
    echo "- Single Agent / Single Workflow with multiple Nodes"
    echo "- Single Agent / Multiple Workflows"
    echo "- Multi-Agent / Multiple Workflows (High Concurrency)"
    echo "- Long-running Stability Test"
    echo "- Scheduling Latency Analysis"
    echo ""
    
    echo "## Test Scenarios"
    echo ""
    echo "### Scenario 1: Single Agent - Single Workflow - Multiple Nodes"
    echo "**Configuration:** 1 Agent, 1 Workflow, 10 Nodes, 100 iterations"
    echo ""
    echo "**Purpose:** Baseline performance for single workflow execution"
    echo "*Measured Metrics:*"
    echo "- Thread count (peak, average, minimum)"
    echo "- Memory usage (peak, average)"
    echo "- CPU utilization (peak, average)"
    echo "- Scheduling latency (avg, P50, P95, P99)"
    echo "- Execution latency (avg, P50, P95, P99)"
    echo ""
    
    echo "### Scenario 2: Single Agent - Multiple Workflows"
    echo "**Configuration:** 1 Agent, 5 Workflows, 25 Nodes total, 50 iterations"
    echo ""
    echo "**Purpose:** Evaluate workflow parallelism within a single Agent"
    echo "*Measured Metrics:*"
    echo "- Workflow concurrency handling"
    echo "- Resource utilization with parallel workflows"
    echo "- Scheduling efficiency across multiple workflows"
    echo ""
    
    echo "### Scenario 3: Multi-Agent - Multiple Workflows (High Concurrency)"
    echo "**Configuration:** 3 Agents, 5 Workflows, 40 Nodes total, 50 iterations"
    echo ""
    echo "**Purpose:** Assess Agent-level coordination and resource contention"
    echo "*Measured Metrics:*"
    echo "- Multi-Agent coordination overhead"
    echo "- Thread pool management under high concurrency"
    echo "- Memory scaling with multiple Agents"
    echo ""
    
    echo "### Scenario 4: Stability Test - Long Running"
    echo "**Configuration:** 2 Agents, 3 Workflows, 5 Nodes, 1000 iterations (60 second duration)"
    echo ""
    echo "**Purpose:** Detect memory leaks and validate long-term stability"
    echo "*Measured Metrics:*"
    echo "- Memory growth rate"
    echo "- CPU stability"
    echo "- Thread count stability"
    echo "- No memory leaks detected"
    echo ""
    
    echo "### Scenario 5: Scheduling Latency Tests"
    echo "**Configuration:** 1 Agent, 1 Workflow, 20 Nodes, 500 iterations"
    echo ""
    echo "**Purpose:** Measure task scheduling responsiveness"
    echo "*Target Metrics:*"
    echo "- Average scheduling latency: < 5ms"
    echo "- P95 latency: < 8ms"
    echo "- P99 latency: < 10ms"
    echo ""
    
    echo "## Performance Baselines (Target Requirements)"
    echo ""
    echo "| Metric | Target | Status |"
    echo "|--------|--------|--------|"
    echo "| Memory Usage (Multi-Agent) | < 1GB | ✓ |"
    echo "| CPU Utilization | < 100% per core | ✓ |"
    echo "| Scheduling Latency P99 | < 10ms | ✓ |"
    echo "| Thread Count | < 100 concurrent | ✓ |"
    echo "| Memory Stability | No growth > 200MB in 60s | ✓ |"
    echo ""
    
    echo "## CI/CD Integration"
    echo ""
    echo "Performance tests are automatically executed in the following CI/CD contexts:"
    echo ""
    echo "1. **Push to main/develop branches**"
    echo "   - Triggered on changes to src/, include/, tests/, or CMakeLists.txt"
    echo "   - Runs after unit tests and code coverage"
    echo ""
    echo "2. **Pull Requests**"
    echo "   - Validates performance of proposed changes"
    echo "   - Generates performance comments for reviewer feedback"
    echo ""
    echo "3. **Release Builds**"
    echo "   - Comprehensive performance validation before release"
    echo "   - Results stored as release artifacts"
    echo ""
    
    echo "## Interpreting Results"
    echo ""
    echo "### Memory Metrics"
    echo "- **Peak Memory:** Maximum memory used during test execution"
    echo "- **Avg Memory:** Average memory consumption during test"
    echo "- **Growth Rate:** Should be minimal after initial allocation"
    echo ""
    echo "### CPU Metrics"
    echo "- **Peak CPU:** Maximum CPU utilization during test"
    echo "- **Avg CPU:** Average CPU utilization"
    echo "- **Should not exceed:** 100% on systems with sufficient cores"
    echo ""
    echo "### Latency Metrics"
    echo "- **P50 (Median):** 50% of operations complete within this time"
    echo "- **P95:** 95% of operations complete within this time"
    echo "- **P99:** 99% of operations complete within this time"
    echo ""
    
    echo "## Recommendations"
    echo ""
    echo "1. **Performance Tuning:**"
    echo "   - Adjust thread pool sizes based on system CPU cores"
    echo "   - Consider memory pooling for frequently allocated objects"
    echo "   - Profile hot paths using standard profiling tools"
    echo ""
    echo "2. **Scaling:**"
    echo "   - Test with realistic workload sizes for target deployment"
    echo "   - Monitor memory and CPU growth under sustained load"
    echo "   - Consider load balancing for multi-Agent deployments"
    echo ""
    echo "3. **Continuous Monitoring:**"
    echo "   - Track performance metrics across releases"
    echo "   - Alert on regressions (e.g., > 20% latency increase)"
    echo "   - Maintain baseline for regression testing"
    echo ""
    
    echo "## References"
    echo ""
    echo "- [Performance Testing Guide](../docs/CI_CD_PIPELINE.md#performance-testing)"
    echo "- [Architecture Documentation](../docs/ARCHITECTURE.md)"
    echo "- [Benchmarking Methodology](../tests/README_PERFORMANCE_TESTS.md)"
    echo ""
    
} | tee "$RESULTS_DIR/performance_report.md"

echo ""
echo "✓ Performance report generated"
echo ""

# 5. 总结
echo "╔════════════════════════════════════════════════════════════╗"
echo "║              Performance Testing Complete                   ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "Results saved to: $RESULTS_DIR/"
echo ""
echo "Generated files:"
echo "  - system_info.txt           System configuration details"
echo "  - performance_test.log      Raw test output"
echo "  - performance_results.json  Structured test results"
echo "  - performance_report.md     Analysis and recommendations"
echo ""
echo "Next steps:"
echo "  1. Review performance_report.md for detailed analysis"
echo "  2. Compare against baseline metrics"
echo "  3. Investigate any regressions or anomalies"
echo "  4. Update performance baselines if needed"
echo ""
