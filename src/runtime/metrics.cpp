// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Metrics Collection and Monitoring Implementation

#include "agentflow/runtime/metrics.h"

#include <sstream>

namespace agentflow {
namespace runtime {

// ============================================================================
// MetricsCollector Implementation
// ============================================================================

MetricsCollector::MetricsCollector() {
}

void MetricsCollector::register_metric(std::shared_ptr<Metric> metric) {
  if(!metric)
    return;

  std::lock_guard<std::mutex> lock(metrics_mutex_);
  metrics_[metric->get_name()] = metric;
}

void MetricsCollector::unregister_metric(const std::string& name) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  metrics_.erase(name);
}

std::shared_ptr<Metric> MetricsCollector::get_metric(const std::string& name) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);

  auto it = metrics_.find(name);
  return it != metrics_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<Metric>> MetricsCollector::get_all_metrics() {
  std::lock_guard<std::mutex> lock(metrics_mutex_);

  std::vector<std::shared_ptr<Metric>> result;
  for(const auto& pair : metrics_) {
    result.push_back(pair.second);
  }
  return result;
}

std::shared_ptr<CounterMetric> MetricsCollector::create_counter(
    const std::string& name, const std::string& description,
    const std::map<std::string, std::string>& labels) {
  auto counter = std::make_shared<CounterMetric>(name, description, labels);
  register_metric(counter);
  return counter;
}

std::shared_ptr<GaugeMetric> MetricsCollector::create_gauge(
    const std::string& name, const std::string& description,
    const std::map<std::string, std::string>& labels) {
  auto gauge = std::make_shared<GaugeMetric>(name, description, labels);
  register_metric(gauge);
  return gauge;
}

std::shared_ptr<HistogramMetric> MetricsCollector::create_histogram(
    const std::string& name, const std::string& description, const std::vector<double>& buckets,
    const std::map<std::string, std::string>& labels) {
  auto histogram = std::make_shared<HistogramMetric>(name, description, buckets, labels);
  register_metric(histogram);
  return histogram;
}

std::string MetricsCollector::export_prometheus() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);

  std::stringstream ss;

  for(const auto& pair : metrics_) {
    const auto& metric = pair.second;

    // Add HELP line
    ss << "# HELP " << metric->get_name() << " " << metric->get_description() << "\n";

    // Add TYPE line
    std::string type_str;
    switch(metric->get_type()) {
      case MetricType::COUNTER:
        type_str = "counter";
        break;
      case MetricType::GAUGE:
        type_str = "gauge";
        break;
      case MetricType::HISTOGRAM:
        type_str = "histogram";
        break;
    }
    ss << "# TYPE " << metric->get_name() << " " << type_str << "\n";

    // Add metric value
    ss << metric->get_name() << " " << metric->to_string() << "\n";
  }

  return ss.str();
}

std::string MetricsCollector::export_json() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);

  std::stringstream ss;
  ss << "{\n";
  ss << "  \"metrics\": [\n";

  bool first = true;
  for(const auto& pair : metrics_) {
    const auto& metric = pair.second;

    if(!first)
      ss << ",\n";
    first = false;

    ss << "    {\n";
    ss << "      \"name\": \"" << metric->get_name() << "\",\n";
    ss << "      \"description\": \"" << metric->get_description() << "\",\n";

    std::string type_str;
    switch(metric->get_type()) {
      case MetricType::COUNTER:
        type_str = "counter";
        break;
      case MetricType::GAUGE:
        type_str = "gauge";
        break;
      case MetricType::HISTOGRAM:
        type_str = "histogram";
        break;
    }
    ss << "      \"type\": \"" << type_str << "\",\n";
    ss << "      \"value\": \"" << metric->to_string() << "\"\n";
    ss << "    }";
  }

  ss << "\n  ]\n";
  ss << "}\n";

  return ss.str();
}

std::string MetricsCollector::export_text() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);

  std::stringstream ss;

  for(const auto& pair : metrics_) {
    ss << pair.second->to_string() << "\n";
  }

  return ss.str();
}

void MetricsCollector::reset_all() {
  std::lock_guard<std::mutex> lock(metrics_mutex_);

  for(auto& pair : metrics_) {
    pair.second->reset();
  }
}

// ============================================================================
// MetricsCollectorFactory Implementation
// ============================================================================

namespace {
std::shared_ptr<MetricsCollector> g_default_collector = nullptr;
}

std::shared_ptr<MetricsCollector> MetricsCollectorFactory::create() {
  return std::make_shared<MetricsCollector>();
}

std::shared_ptr<MetricsCollector> MetricsCollectorFactory::get_default() {
  static std::mutex init_mutex;

  std::lock_guard<std::mutex> lock(init_mutex);
  if(!g_default_collector) {
    g_default_collector = std::make_shared<MetricsCollector>();
  }
  return g_default_collector;
}

}  // namespace runtime
}  // namespace agentflow
