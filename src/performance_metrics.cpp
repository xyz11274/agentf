#include "agentflow/testing/performance_metrics.h"

#include <algorithm>
#include <fstream>
#include <numeric>
#include <platform.h>  // 根据实际情况调整

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
#else
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <thread>
#endif

namespace agentflow {
namespace testing {

int SystemMonitor::get_thread_count() {
#ifdef _WIN32
    return static_cast<int>(std::thread::hardware_concurrency());
#else
    // 在Linux上，可以通过/proc/self/status读取线程数
    int count = 0;
    std::ifstream status_file("/proc/self/status");
    if (status_file.is_open()) {
        std::string line;
        while (std::getline(status_file, line)) {
            if (line.find("Threads:") == 0) {
                count = std::stoi(line.substr(8));
                break;
            }
        }
    }
    return count > 0 ? count : static_cast<int>(std::thread::hardware_concurrency());
#endif
}

double SystemMonitor::get_memory_usage_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    // 在Linux上，从/proc/self/status读取内存信息
    double rss_mb = 0.0;
    std::ifstream status_file("/proc/self/status");
    if (status_file.is_open()) {
        std::string line;
        while (std::getline(status_file, line)) {
            if (line.find("VmRSS:") == 0) {
                rss_mb = std::stod(line.substr(6)) / 1024.0;
                break;
            }
        }
    }
    return rss_mb;
#endif
}

double SystemMonitor::get_cpu_usage_percent() {
#ifdef _WIN32
    // Windows CPU使用率较复杂，可以使用简化方法或跳过
    return 0.0;  // TODO: 实现Windows CPU监控
#else
    // 从/proc/stat读取CPU信息
    static unsigned long last_idle = 0, last_total = 0;
    
    std::ifstream stat_file("/proc/stat");
    if (!stat_file.is_open()) return 0.0;
    
    unsigned long user, nice, system, idle, iowait, irq, softirq;
    std::string cpu_label;
    
    stat_file >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
    
    unsigned long total = user + nice + system + idle + iowait + irq + softirq;
    unsigned long idle_delta = idle - last_idle;
    unsigned long total_delta = total - last_total;
    
    last_idle = idle;
    last_total = total;
    
    if (total_delta == 0) return 0.0;
    return (100.0 * (total_delta - idle_delta)) / total_delta;
#endif
}

std::vector<SystemMonitor::Sample> SystemMonitor::collect_samples(int duration_ms, int interval_ms) {
    std::vector<Sample> samples;
    auto start = std::chrono::steady_clock::now();
    
    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        
        if (elapsed >= duration_ms) break;
        
        Sample sample;
        sample.threads = get_thread_count();
        sample.memory_mb = get_memory_usage_mb();
        sample.cpu_percent = get_cpu_usage_percent();
        sample.timestamp = now;
        
        samples.push_back(sample);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    
    return samples;
}

void PerformanceTester::record_event_timestamp(const std::string& event_name) {
    events_.emplace_back(event_name, std::chrono::steady_clock::now());
}

double PerformanceTester::calculate_percentile(const std::vector<double>& values, double percentile) {
    if (values.empty()) return 0.0;
    
    std::vector<double> sorted_values = values;
    std::sort(sorted_values.begin(), sorted_values.end());
    
    int index = static_cast<int>((percentile / 100.0) * (sorted_values.size() - 1));
    return sorted_values[index];
}

PerformanceMetrics PerformanceTester::compute_metrics(
    int num_agents,
    int num_workflows,
    int num_nodes,
    int num_iterations,
    const std::vector<SystemMonitor::Sample>& samples,
    const std::vector<double>& schedule_latencies,
    const std::vector<double>& execution_latencies) {
    
    PerformanceMetrics metrics;
    metrics.test_name = test_name_;
    metrics.num_agents = num_agents;
    metrics.num_workflows = num_workflows;
    metrics.num_nodes = num_nodes;
    metrics.num_iterations = num_iterations;
    
    // 计算线程指标
    if (!samples.empty()) {
        std::vector<int> thread_counts;
        for (const auto& sample : samples) {
            thread_counts.push_back(sample.threads);
        }
        metrics.thread_metrics.peak_threads = *std::max_element(thread_counts.begin(), thread_counts.end());
        metrics.thread_metrics.min_threads = *std::min_element(thread_counts.begin(), thread_counts.end());
        metrics.thread_metrics.avg_threads = std::accumulate(thread_counts.begin(), thread_counts.end(), 0) / 
                                              static_cast<int>(thread_counts.size());
        
        // 计算资源指标
        std::vector<double> memory_values;
        std::vector<double> cpu_values;
        
        for (const auto& sample : samples) {
            memory_values.push_back(sample.memory_mb);
            cpu_values.push_back(sample.cpu_percent);
        }
        
        metrics.resource_metrics.peak_memory_mb = *std::max_element(memory_values.begin(), memory_values.end());
        metrics.resource_metrics.avg_memory_mb = std::accumulate(memory_values.begin(), memory_values.end(), 0.0) / 
                                                  memory_values.size();
        metrics.resource_metrics.peak_cpu_percent = *std::max_element(cpu_values.begin(), cpu_values.end());
        metrics.resource_metrics.avg_cpu_percent = std::accumulate(cpu_values.begin(), cpu_values.end(), 0.0) / 
                                                    cpu_values.size();
        
        // 计算总执行时间
        if (!samples.empty()) {
            metrics.latency_metrics.total_duration_ms = 
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    samples.back().timestamp - samples.front().timestamp).count();
        }
    }
    
    // 计算延迟指标
    if (!schedule_latencies.empty()) {
        metrics.latency_metrics.schedule_latency_ms = 
            std::accumulate(schedule_latencies.begin(), schedule_latencies.end(), 0.0) / schedule_latencies.size();
        metrics.latency_metrics.p50_latency_ms = calculate_percentile(schedule_latencies, 50.0);
        metrics.latency_metrics.p95_latency_ms = calculate_percentile(schedule_latencies, 95.0);
        metrics.latency_metrics.p99_latency_ms = calculate_percentile(schedule_latencies, 99.0);
    }
    
    if (!execution_latencies.empty()) {
        metrics.latency_metrics.execution_latency_ms = 
            std::accumulate(execution_latencies.begin(), execution_latencies.end(), 0.0) / execution_latencies.size();
    }
    
    return metrics;
}

} // namespace testing
} // namespace agentflow
