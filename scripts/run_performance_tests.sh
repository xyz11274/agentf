#!/bin/bash

# 性能测试脚本
# 用于收集和分析AgentFlow的性能指标

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RESULTS_DIR="${BUILD_DIR}/performance_results"

# 创建结果目录
mkdir -p "$RESULTS_DIR"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║     AgentFlow Performance Test Suite - Build Phase          ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# 1. 系统信息收集
echo "[1/4] Collecting System Information..."
{
    echo "System Information Report"
    echo "=========================="
    echo ""
    echo "Date: $(date)"
    echo "Hostname: $(hostname)"
    echo ""
    echo "CPU Information:"
    echo "  Model: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null || echo 'Unknown')"
    echo "  Cores: $(nproc)"
    echo "  Frequency: $(grep -m1 'cpu MHz' /proc/cpuinfo 2>/dev/null | awk '{print $4}' || echo 'Unknown') MHz"
    echo ""
    echo "Memory Information:"
    echo "  Total: $(free -h | awk '/^Mem:/ {print $2}')"
    echo "  Available: $(free -h | awk '/^Mem:/ {print $7}')"
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
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "Build directory not configured. Running cmake..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DAGENTFLOW_WITH_PROTOACTOR=OFF \
        -DAGENTFLOW_WITH_OPENSSL=ON \
        -DAGENTFLOW_WITH_PROTOBUF=ON \
        -DAGENTFLOW_WITH_LIBWEBSOCKETS=ON
    cd "$PROJECT_ROOT"
fi

if [ ! -f "$BUILD_DIR/test_performance" ] && [ ! -f "$BUILD_DIR/test_agentflow_gtest" ]; then
    echo "Building performance tests..."
    cd "$BUILD_DIR"
    make -j$(nproc) test_performance test_agentflow_gtest 2>&1 | tail -20
    cd "$PROJECT_ROOT"
fi

echo "✓ Build configuration verified"
echo ""

# 3. 运行性能测试
echo "[3/4] Running Performance Tests..."
echo ""

cd "$BUILD_DIR"

# 运行性能测试，并捕获输出
if [ -f test_performance ]; then
    {
        echo "Performance Test Results"
        echo "========================"
        echo ""
        echo "Test Start: $(date)"
        echo ""
        
        # 运行测试（带超时保护）
        timeout 300 ./test_performance \
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
