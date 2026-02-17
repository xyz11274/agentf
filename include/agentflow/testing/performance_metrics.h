#pragma once

#include <chrono>
#include <vector>
#include <string>
#include <memory>
#include <iostream>
#include <iomanip>

namespace agentflow {
namespace testing {

// 性能指标结构体
struct PerformanceMetrics {
    struct ThreadMetrics {
        int peak_threads = 0;
        int avg_threads = 0;
        int min_threads = 0;
    };

    struct ResourceMetrics {
        double peak_memory_mb = 0.0;    // Peak memory in MB
        double avg_memory_mb = 0.0;     // Average memory in MB
        double peak_cpu_percent = 0.0;  // Peak CPU usage percentage
        double avg_cpu_percent = 0.0;   // Average CPU usage percentage
    };

    struct LatencyMetrics {
        double total_duration_ms = 0.0;      // Total execution time
        double schedule_latency_ms = 0.0;    // Average scheduling latency
        double execution_latency_ms = 0.0;   // Average execution latency
        double p50_latency_ms = 0.0;         // 50th percentile
        double p95_latency_ms = 0.0;         // 95th percentile
        double p99_latency_ms = 0.0;         // 99th percentile
    };

    std::string test_name;
    int num_agents = 0;
    int num_workflows = 0;
    int num_nodes = 0;
    int num_iterations = 0;

    ThreadMetrics thread_metrics;
    ResourceMetrics resource_metrics;
    LatencyMetrics latency_metrics;

    // Print metrics in CSV format
    void print_csv_header() const {
        std::cout << "TestName,NumAgents,NumWorkflows,NumNodes,NumIterations,"
                  << "PeakThreads,AvgThreads,MinThreads,"
                  << "PeakMemoryMB,AvgMemoryMB,PeakCPU%,AvgCPU%,"
                  << "TotalDurationMs,ScheduleLatencyMs,ExecutionLatencyMs,"
                  << "P50LatencyMs,P95LatencyMs,P99LatencyMs" << std::endl;
    }

    void print_csv_row() const {
        std::cout << test_name << ","
                  << num_agents << ","
                  << num_workflows << ","
                  << num_nodes << ","
                  << num_iterations << ","
                  << thread_metrics.peak_threads << ","
                  << thread_metrics.avg_threads << ","
                  << thread_metrics.min_threads << ","
                  << std::fixed << std::setprecision(2)
                  << resource_metrics.peak_memory_mb << ","
                  << resource_metrics.avg_memory_mb << ","
                  << resource_metrics.peak_cpu_percent << ","
                  << resource_metrics.avg_cpu_percent << ","
                  << latency_metrics.total_duration_ms << ","
                  << latency_metrics.schedule_latency_ms << ","
                  << latency_metrics.execution_latency_ms << ","
                  << latency_metrics.p50_latency_ms << ","
                  << latency_metrics.p95_latency_ms << ","
                  << latency_metrics.p99_latency_ms << std::endl;
    }

    void print_summary() const {
        std::cout << "\n========== Performance Test Summary: " << test_name << " ==========" << std::endl;
        std::cout << "Configuration:" << std::endl;
        std::cout << "  Agents: " << num_agents << ", Workflows: " << num_workflows 
                  << ", Nodes: " << num_nodes << ", Iterations: " << num_iterations << std::endl;
        
        std::cout << "\nThread Metrics:" << std::endl;
        std::cout << "  Peak threads: " << thread_metrics.peak_threads << std::endl;
        std::cout << "  Avg threads:  " << thread_metrics.avg_threads << std::endl;
        std::cout << "  Min threads:  " << thread_metrics.min_threads << std::endl;
        
        std::cout << "\nResource Metrics:" << std::endl;
        std::cout << "  Peak memory: " << std::fixed << std::setprecision(2) 
                  << resource_metrics.peak_memory_mb << " MB" << std::endl;
        std::cout << "  Avg memory:  " << resource_metrics.avg_memory_mb << " MB" << std::endl;
        std::cout << "  Peak CPU:    " << resource_metrics.peak_cpu_percent << "%" << std::endl;
        std::cout << "  Avg CPU:     " << resource_metrics.avg_cpu_percent << "%" << std::endl;
        
        std::cout << "\nLatency Metrics (ms):" << std::endl;
        std::cout << "  Total duration:     " << latency_metrics.total_duration_ms << std::endl;
        std::cout << "  Schedule latency:   " << latency_metrics.schedule_latency_ms << std::endl;
        std::cout << "  Execution latency:  " << latency_metrics.execution_latency_ms << std::endl;
        std::cout << "  P50:  " << latency_metrics.p50_latency_ms << " ms" << std::endl;
        std::cout << "  P95:  " << latency_metrics.p95_latency_ms << " ms" << std::endl;
        std::cout << "  P99:  " << latency_metrics.p99_latency_ms << " ms" << std::endl;
        std::cout << "========================================================\n" << std::endl;
    }
};

// 系统资源监控器
class SystemMonitor {
public:
    // 获取当前线程数
    static int get_thread_count();
    
    // 获取当前内存使用量（MB）
    static double get_memory_usage_mb();
    
    // 获取当前CPU使用率（百分比）
    static double get_cpu_usage_percent();
    
    // 采样系统资源
    struct Sample {
        int threads;
        double memory_mb;
        double cpu_percent;
        std::chrono::steady_clock::time_point timestamp;
    };
    
    static std::vector<Sample> collect_samples(int duration_ms, int interval_ms);
};

// 性能测试工具类
class PerformanceTester {
public:
    explicit PerformanceTester(const std::string& test_name)
        : test_name_(test_name) {}

    // 记录事件时间戳
    void record_event_timestamp(const std::string& event_name);
    
    // 计算延迟指标
    double calculate_percentile(const std::vector<double>& values, double percentile);
    
    // 从采样数据计算指标
    PerformanceMetrics compute_metrics(
        int num_agents,
        int num_workflows,
        int num_nodes,
        int num_iterations,
        const std::vector<SystemMonitor::Sample>& samples,
        const std::vector<double>& schedule_latencies,
        const std::vector<double>& execution_latencies);

private:
    std::string test_name_;
    std::chrono::steady_clock::time_point start_time_;
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> events_;
};

} // namespace testing
} // namespace agentflow
