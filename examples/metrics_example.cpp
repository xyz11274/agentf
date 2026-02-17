// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example demonstrating Metrics Collector functionality

#include <chrono>
#include <iostream>
#include <thread>

#include "agentflow/basic/logger.h"
#include "agentflow/runtime/metrics.h"

using namespace agentflow;
using namespace agentflow::runtime;

int main() {
  Logger::instance().info("AgentFlow Metrics Collector Example");
  Logger::instance().info("===================================");

  // Get the default metrics collector
  auto collector = MetricsCollectorFactory::get_default();

  // Create counters
  auto requests_total = collector->create_counter("workflow_requests_total",
                                                  "Total number of workflow execution requests");

  auto requests_success = collector->create_counter("workflow_requests_success",
                                                    "Number of successful workflow executions");

  auto requests_failed =
      collector->create_counter("workflow_requests_failed", "Number of failed workflow executions");

  // Create gauges
  auto active_workflows =
      collector->create_gauge("active_workflows", "Number of currently active workflows");

  auto running_nodes =
      collector->create_gauge("running_nodes", "Number of nodes currently executing");

  auto queue_size = collector->create_gauge("node_queue_size", "Number of nodes waiting in queue");

  // Create histograms
  auto execution_time = collector->create_histogram(
      "workflow_execution_time_ms", "Workflow execution time in milliseconds",
      { 1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000 });

  auto node_latency = collector->create_histogram(
      "node_execution_latency_ms", "Node execution latency in milliseconds",
      { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000 });

  Logger::instance().info("Metrics created successfully");

  // Simulate some metrics collection
  Logger::instance().info("\n--- Simulating Metrics Collection ---");

  // Simulate workflow requests
  for(int i = 0; i < 10; ++i) {
    requests_total->increment();

    // Simulate workflow execution
    bool success = (i % 3 != 0);  // Fail every 3rd request

    // Record execution time
    int exec_time = 10 + (std::rand() % 200);  // 10-210ms
    execution_time->record(exec_time);

    // Update active workflows gauge
    active_workflows->increment();

    // Simulate node execution
    int node_count = 3 + (std::rand() % 5);  // 3-7 nodes
    running_nodes->set(node_count);

    for(int j = 0; j < node_count; ++j) {
      int latency = 1 + (std::rand() % 50);  // 1-51ms
      node_latency->record(latency);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Update gauges
    active_workflows->decrement();
    running_nodes->set(0);

    // Update counters
    if(success) {
      requests_success->increment();
    } else {
      requests_failed->increment();
    }

    // Simulate queue size
    queue_size->set(std::rand() % 10);
  }

  Logger::instance().info("Metrics collection simulation completed");

  // Export metrics in different formats
  Logger::instance().info("\n--- Metrics Export ---");

  // Export as text
  Logger::instance().info("\nText Format:");
  std::cout << collector->export_text() << std::endl;

  // Export as JSON
  Logger::instance().info("\nJSON Format:");
  std::cout << collector->export_json() << std::endl;

  // Export as Prometheus
  Logger::instance().info("\nPrometheus Format:");
  std::cout << collector->export_prometheus() << std::endl;

  // Display histogram statistics
  Logger::instance().info("\n--- Histogram Statistics ---");
  Logger::instance().info_f(
      "Workflow Execution Time - P50: %.2f ms, P90: %.2f ms, P95: %.2f ms, P99: %.2f ms, Avg: %.2f "
      "ms",
      execution_time->get_p50(), execution_time->get_p90(), execution_time->get_p95(),
      execution_time->get_p99(), execution_time->get_average());

  Logger::instance().info_f(
      "Node Execution Latency - P50: %.2f ms, P90: %.2f ms, P95: %.2f ms, P99: %.2f ms, Avg: %.2f "
      "ms",
      node_latency->get_p50(), node_latency->get_p90(), node_latency->get_p95(),
      node_latency->get_p99(), node_latency->get_average());

  // Display counter values
  Logger::instance().info("\n--- Counter Values ---");
  Logger::instance().info_f("Total Requests: %lld",
                            static_cast<long long>(requests_total->get_value()));
  Logger::instance().info_f("Successful Requests: %lld",
                            static_cast<long long>(requests_success->get_value()));
  Logger::instance().info_f("Failed Requests: %lld",
                            static_cast<long long>(requests_failed->get_value()));

  Logger::instance().info("\n===================================");
  Logger::instance().info("Example completed successfully!");
  return 0;
}
