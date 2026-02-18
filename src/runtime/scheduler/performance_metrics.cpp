#include "testing/performance_metrics.h"

#include <algorithm>
#include <fstream>
#include <numeric>

#ifdef _WIN32
#include <psapi.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#else
// Linux
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace agentflow {
namespace testing {

int SystemMonitor::get_thread_count() {
#ifdef _WIN32
  return static_cast<int>(std::thread::hardware_concurrency());
#elif defined(__APPLE__)
  // macOS: use task_info to get thread count
  mach_msg_type_number_t count;
  task_basic_info_data_t info;
  count = TASK_BASIC_INFO_COUNT;
  if(task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
    // task_basic_info doesn't give thread count directly; use thread_info
    thread_array_t    thread_list;
    mach_msg_type_number_t thread_count;
    if(task_threads(mach_task_self(), &thread_list, &thread_count) == KERN_SUCCESS) {
      // Deallocate the thread list
      for(mach_msg_type_number_t i = 0; i < thread_count; ++i) {
        mach_port_deallocate(mach_task_self(), thread_list[i]);
      }
      vm_deallocate(mach_task_self(), (vm_address_t)thread_list,
                    thread_count * sizeof(thread_act_t));
      return static_cast<int>(thread_count);
    }
  }
  return static_cast<int>(std::thread::hardware_concurrency());
#else
  // Linux: read from /proc/self/status
  int           count = 0;
  std::ifstream status_file("/proc/self/status");
  if(status_file.is_open()) {
    std::string line;
    while(std::getline(status_file, line)) {
      if(line.find("Threads:") == 0) {
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
  if(GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return pmc.WorkingSetSize / (1024.0 * 1024.0);
  }
  return 0.0;
#elif defined(__APPLE__)
  // macOS: use mach task_info for resident memory
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
  if(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count)
     == KERN_SUCCESS) {
    return info.resident_size / (1024.0 * 1024.0);
  }
  return 0.0;
#else
  // Linux: read from /proc/self/status
  double        rss_mb = 0.0;
  std::ifstream status_file("/proc/self/status");
  if(status_file.is_open()) {
    std::string line;
    while(std::getline(status_file, line)) {
      if(line.find("VmRSS:") == 0) {
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
  return 0.0;  // TODO: Windows CPU monitoring
#elif defined(__APPLE__)
  // macOS: use thread_info to sum CPU usage across all threads
  thread_array_t         thread_list;
  mach_msg_type_number_t thread_count;

  if(task_threads(mach_task_self(), &thread_list, &thread_count) != KERN_SUCCESS) {
    return 0.0;
  }

  double total_cpu = 0.0;
  for(mach_msg_type_number_t i = 0; i < thread_count; ++i) {
    thread_basic_info_data_t tinfo;
    mach_msg_type_number_t   tinfo_count = THREAD_BASIC_INFO_COUNT;
    if(thread_info(thread_list[i], THREAD_BASIC_INFO, (thread_info_t)&tinfo, &tinfo_count)
       == KERN_SUCCESS) {
      total_cpu += tinfo.cpu_usage / (double)TH_USAGE_SCALE * 100.0;
    }
    mach_port_deallocate(mach_task_self(), thread_list[i]);
  }
  vm_deallocate(mach_task_self(), (vm_address_t)thread_list,
                thread_count * sizeof(thread_act_t));
  return total_cpu;
#else
  // Linux: read from /proc/stat
  static unsigned long last_idle = 0, last_total = 0;

  std::ifstream stat_file("/proc/stat");
  if(!stat_file.is_open())
    return 0.0;

  unsigned long user, nice, system, idle, iowait, irq, softirq;
  std::string   cpu_label;

  stat_file >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq;

  unsigned long total       = user + nice + system + idle + iowait + irq + softirq;
  unsigned long idle_delta  = idle - last_idle;
  unsigned long total_delta = total - last_total;

  last_idle  = idle;
  last_total = total;

  if(total_delta == 0)
    return 0.0;
  return (100.0 * (total_delta - idle_delta)) / total_delta;
#endif
}

std::vector<SystemMonitor::Sample> SystemMonitor::collect_samples(int duration_ms,
                                                                  int interval_ms) {
  std::vector<Sample> samples;
  auto                start = std::chrono::steady_clock::now();

  while(true) {
    auto now     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

    if(elapsed >= duration_ms)
      break;

    Sample sample;
    sample.threads     = get_thread_count();
    sample.memory_mb   = get_memory_usage_mb();
    sample.cpu_percent = get_cpu_usage_percent();
    sample.timestamp   = now;

    samples.push_back(sample);

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }

  return samples;
}

void PerformanceTester::record_event_timestamp(const std::string& event_name) {
  events_.emplace_back(event_name, std::chrono::steady_clock::now());
}

double PerformanceTester::calculate_percentile(const std::vector<double>& values,
                                               double                     percentile) {
  if(values.empty())
    return 0.0;

  std::vector<double> sorted_values = values;
  std::sort(sorted_values.begin(), sorted_values.end());

  int index = static_cast<int>((percentile / 100.0) * (sorted_values.size() - 1));
  return sorted_values[index];
}

PerformanceMetrics PerformanceTester::compute_metrics(
    int num_agents, int num_workflows, int num_nodes, int num_iterations,
    const std::vector<SystemMonitor::Sample>& samples,
    const std::vector<double>& schedule_latencies, const std::vector<double>& execution_latencies) {
  PerformanceMetrics metrics;
  metrics.test_name      = test_name_;
  metrics.num_agents     = num_agents;
  metrics.num_workflows  = num_workflows;
  metrics.num_nodes      = num_nodes;
  metrics.num_iterations = num_iterations;

  // 计算线程指标
  if(!samples.empty()) {
    std::vector<int> thread_counts;
    for(const auto& sample : samples) {
      thread_counts.push_back(sample.threads);
    }
    metrics.thread_metrics.peak_threads =
        *std::max_element(thread_counts.begin(), thread_counts.end());
    metrics.thread_metrics.min_threads =
        *std::min_element(thread_counts.begin(), thread_counts.end());
    metrics.thread_metrics.avg_threads =
        std::accumulate(thread_counts.begin(), thread_counts.end(), 0)
        / static_cast<int>(thread_counts.size());

    // 计算资源指标
    std::vector<double> memory_values;
    std::vector<double> cpu_values;

    for(const auto& sample : samples) {
      memory_values.push_back(sample.memory_mb);
      cpu_values.push_back(sample.cpu_percent);
    }

    metrics.resource_metrics.peak_memory_mb =
        *std::max_element(memory_values.begin(), memory_values.end());
    metrics.resource_metrics.avg_memory_mb =
        std::accumulate(memory_values.begin(), memory_values.end(), 0.0) / memory_values.size();
    metrics.resource_metrics.peak_cpu_percent =
        *std::max_element(cpu_values.begin(), cpu_values.end());
    metrics.resource_metrics.avg_cpu_percent =
        std::accumulate(cpu_values.begin(), cpu_values.end(), 0.0) / cpu_values.size();

    // 计算总执行时间
    if(!samples.empty()) {
      metrics.latency_metrics.total_duration_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(samples.back().timestamp
                                                                - samples.front().timestamp)
              .count();
    }
  }

  // 计算延迟指标
  if(!schedule_latencies.empty()) {
    metrics.latency_metrics.schedule_latency_ms =
        std::accumulate(schedule_latencies.begin(), schedule_latencies.end(), 0.0)
        / schedule_latencies.size();
    metrics.latency_metrics.p50_latency_ms = calculate_percentile(schedule_latencies, 50.0);
    metrics.latency_metrics.p95_latency_ms = calculate_percentile(schedule_latencies, 95.0);
    metrics.latency_metrics.p99_latency_ms = calculate_percentile(schedule_latencies, 99.0);
  }

  if(!execution_latencies.empty()) {
    metrics.latency_metrics.execution_latency_ms =
        std::accumulate(execution_latencies.begin(), execution_latencies.end(), 0.0)
        / execution_latencies.size();
  }

  return metrics;
}

}  // namespace testing
}  // namespace agentflow
