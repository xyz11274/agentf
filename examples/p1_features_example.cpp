// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example demonstrating P1 priority features:
// 1. Complex flow control integration with Workflow scheduling
// 2. Zero-copy data sharing
// 3. Business data communication interface

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "agentflow/utils/logger.h"
#include "agentflow/core/agent.h"
#include "agentflow/core/node.h"
#include "agentflow/core/workflow.h"
#include "agentflow/network/private_protocol.h"
#include "agentflow/runtime/control_flow.h"
#include "agentflow/runtime/zero_copy_databus.h"

using namespace agentflow;

/**
 * Simple node execution function for testing
 */
bool execute_test_node(const std::string& name, const DataMap&, DataMap& outputs) {
  Logger::instance().info_f("Executing node: %s", name.c_str());

  // Simulate node execution
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Set output data
  outputs["result"] = "success";
  outputs["node"]   = name;

  return true;
}

/**
 * Example 1: Zero-copy data sharing
 */
void example_zero_copy_databus() {
  Logger::instance().info("=== Example 1: Zero-copy Data Bus ===");

  // Create zero-copy data bus
  runtime::ZeroCopyDataBus databus(1024 * 1024);  // 1MB

  Logger::instance().info_f("Data bus capacity: %zu bytes", databus.get_total_capacity());
  Logger::instance().info_f("Initial usage: %zu bytes (%.1f%%)", databus.get_used_capacity(),
                            databus.get_capacity_usage() * 100.0);

  // Put data
  databus.put("key1", "value1");
  databus.put("key2", "value2");
  databus.put("key3", "value3");

  Logger::instance().info_f("After put: used=%zu bytes (%.1f%%)", databus.get_used_capacity(),
                            databus.get_capacity_usage() * 100.0);

  // Get data (with zero-copy access if available)
  DataVariant value;
  if(databus.get("key1", value)) {
    std::string value_str = std::visit(
        [](auto&& arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, std::string>) {
            return arg;
          } else {
            return std::to_string(arg);
          }
        },
        value);
    Logger::instance().info_f("key1: %s", value_str.c_str());
  }

  // Get data pointer directly (zero-copy access)
  const DataVariant* value_ptr = databus.get_data_ptr("key2");
  if(value_ptr) {
    std::string value_str = std::visit(
        [](auto&& arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, std::string>) {
            return arg;
          } else {
            return std::to_string(arg);
          }
        },
        *value_ptr);
    Logger::instance().info_f("key2 (ptr): %s", value_str.c_str());
  }

  // Subscribe to data changes
  int notification_count = 0;
  databus.subscribe(
      "key3", [&notification_count](const std::string& key, const DataVariant& value) {
        std::string value_str = std::visit(
            [](auto&& arg) -> std::string {
              using T = std::decay_t<decltype(arg)>;
              if constexpr(std::is_same_v<T, std::string>) {
                return arg;
              } else {
                return std::to_string(arg);
              }
            },
            value);
        Logger::instance().info_f("Subscription notification #%d: %s = %s", ++notification_count,
                                  key.c_str(), value_str.c_str());
      });

  // Modify data and trigger notification
  databus.put("key3", "value3_modified");

  // Wait for notifications
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  Logger::instance().info_f("Data count: %d", databus.get_data_count());
}

/**
 * Example 2: Condition evaluation
 */
void example_condition_evaluation() {
  Logger::instance().info("=== Example 2: Condition Evaluation ===");

  runtime::ControlContext     context;
  runtime::ConditionEvaluator evaluator;

  // Set test variables
  context.set_variable("status", "active");
  context.set_variable("count", "10");
  context.set_variable("name", "test_agent");
  context.set_variable("enabled", "true");

  // Test equality condition
  runtime::ConditionExpression eq_expr("status", runtime::ConditionOperator::EQUAL, "active");
  bool                         eq_result = evaluator.evaluate(eq_expr, context, DataMap());
  Logger::instance().info_f("status == 'active': %s", eq_result ? "true" : "false");

  // Test greater than condition
  runtime::ConditionExpression gt_expr("count", runtime::ConditionOperator::GREATER_THAN, "5");
  bool                         gt_result = evaluator.evaluate(gt_expr, context, DataMap());
  Logger::instance().info_f("count > 5: %s", gt_result ? "true" : "false");

  // Test contains condition
  runtime::ConditionExpression contains_expr("name", runtime::ConditionOperator::CONTAINS, "test");
  bool contains_result = evaluator.evaluate(contains_expr, context, DataMap());
  Logger::instance().info_f("name contains 'test': %s", contains_result ? "true" : "false");

  // Test AND operator
  auto left_expr = std::make_shared<runtime::ConditionExpression>(
      "status", runtime::ConditionOperator::EQUAL, "active");
  auto right_expr = std::make_shared<runtime::ConditionExpression>(
      "enabled", runtime::ConditionOperator::EQUAL, "true");
  runtime::ConditionExpression and_expr;
  and_expr.left_expr  = left_expr;
  and_expr.right_expr = right_expr;
  and_expr.op         = runtime::ConditionOperator::AND;

  bool and_result = evaluator.evaluate(and_expr, context, DataMap());
  Logger::instance().info_f("status == 'active' AND enabled == 'true': %s",
                            and_result ? "true" : "false");

  // Test NOT operator
  runtime::ConditionExpression not_expr;
  not_expr.left_expr = left_expr;
  not_expr.op        = runtime::ConditionOperator::NOT;

  bool not_result = evaluator.evaluate(not_expr, context, DataMap());
  Logger::instance().info_f("NOT (status == 'active'): %s", not_result ? "true" : "false");
}

/**
 * Example 3: Control flow manager with workflow
 */
void example_control_flow_workflow() {
  Logger::instance().info("=== Example 3: Control Flow with Workflow ===");

  // Create workflow
  framework::Workflow::Config wf_config;
  wf_config.id          = "workflow_001";
  wf_config.name        = "Control Flow Example";
  wf_config.description = "Example workflow with control flow";

  framework::Workflow workflow(wf_config);

  // Create nodes
  auto node1 = std::make_shared<framework::Node>(framework::Node::Config{
      "node_001",
      "Start Node",
      "Start",
      0,   // priority
      {},  // input_params
      {},  // output_params
      framework::Node::ExecutionMode::CPP_FUNCTION,
      "",   // command
      "",   // working_dir
      3,    // max_retries
      0,    // timeout_ms
      0,    // memory_limit_mb
      0,    // cpu_limit
      1000  // retry_delay_ms
  });

  auto node2 = std::make_shared<framework::Node>(
      framework::Node::Config{ "node_002",
                               "Condition Node",
                               "Condition",
                               0,
                               {},
                               {},
                               framework::Node::ExecutionMode::CPP_FUNCTION,
                               "",
                               "",
                               3,
                               0,
                               0,
                               0,
                               1000 });

  auto node3 = std::make_shared<framework::Node>(
      framework::Node::Config{ "node_003",
                               "Success Node A",
                               "Success A",
                               0,
                               {},
                               {},
                               framework::Node::ExecutionMode::CPP_FUNCTION,
                               "",
                               "",
                               3,
                               0,
                               0,
                               0,
                               1000 });

  auto node4 = std::make_shared<framework::Node>(
      framework::Node::Config{ "node_004",
                               "Success Node B",
                               "Success B",
                               0,
                               {},
                               {},
                               framework::Node::ExecutionMode::CPP_FUNCTION,
                               "",
                               "",
                               3,
                               0,
                               0,
                               0,
                               1000 });

  auto node5 = std::make_shared<framework::Node>(
      framework::Node::Config{ "node_005",
                               "End Node",
                               "End",
                               0,
                               {},
                               {},
                               framework::Node::ExecutionMode::CPP_FUNCTION,
                               "",
                               "",
                               3,
                               0,
                               0,
                               0,
                               1000 });

  workflow.add_node(node1);
  workflow.add_node(node2);
  workflow.add_node(node3);
  workflow.add_node(node4);
  workflow.add_node(node5);

  // Create edges with conditions
  auto edge1 = std::make_shared<framework::Edge>(framework::Edge::Config{
      "edge_001",
      "node_001",
      "node_002",
      {},  // source_outputs
      {},  // target_inputs
      framework::Edge::DataTransferMode::SHARED_MEMORY,
      {},                    // data_formats
      {},                    // supported_events
      "status == 'active'",  // condition_expression
      0                      // priority
  });

  auto edge2 = std::make_shared<framework::Edge>(
      framework::Edge::Config{ "edge_002",
                               "node_002",
                               "node_003",
                               {},
                               {},
                               framework::Edge::DataTransferMode::SHARED_MEMORY,
                               {},
                               {},
                               "status == 'success'",
                               0 });

  auto edge3 = std::make_shared<framework::Edge>(
      framework::Edge::Config{ "edge_003",
                               "node_002",
                               "node_004",
                               {},
                               {},
                               framework::Edge::DataTransferMode::SHARED_MEMORY,
                               {},
                               {},
                               "status == 'fail'",
                               0 });

  auto edge4 = std::make_shared<framework::Edge>(
      framework::Edge::Config{ "edge_004",
                               "node_003",
                               "node_005",
                               {},
                               {},
                               framework::Edge::DataTransferMode::SHARED_MEMORY,
                               {},
                               {},
                               "result == 'success'",
                               0 });

  auto edge5 = std::make_shared<framework::Edge>(
      framework::Edge::Config{ "edge_005",
                               "node_004",
                               "node_005",
                               {},
                               {},
                               framework::Edge::DataTransferMode::SHARED_MEMORY,
                               {},
                               {},
                               "",  // No condition
                               0 });

  // Add edges to workflow
  workflow.add_edge(edge1);
  workflow.add_edge(edge2);
  workflow.add_edge(edge3);
  workflow.add_edge(edge4);
  workflow.add_edge(edge5);

  Logger::instance().info_f("Workflow: %s created with %zu nodes and %zu edges",
                            workflow.get_name().c_str(), workflow.get_nodes().size(),
                            workflow.get_edges().size());

  // Get executable nodes using control flow
  auto executable = workflow.get_executable_nodes_with_control();
  Logger::instance().info_f("Executable nodes: %zu", executable.size());

  // Set up control flow context for condition evaluation
  workflow.get_control_context().set_variable("status", "active");

  // Mark a node as executed and get next node
  workflow.mark_node_executed("node_001", true);
  auto next_node = workflow.get_next_node_after_execution("node_001", true);
  if(!next_node.empty()) {
    Logger::instance().info_f("Next node after node_001: %s", next_node.c_str());
  }
}

/**
 * Example 4: Business data communication via private protocol
 */
void example_private_protocol() {
  Logger::instance().info("=== Example 4: Private Protocol Communication ===");

  // Create protocol header
  network::ProtocolHeader header;
  header.set_header(static_cast<uint16_t>(network::MessageType::DATA_REQUEST), 128, 1001);

  Logger::instance().info_f("Protocol header created: magic=0x%08X, version=0x%04X, type=0x%04X",
                            header.magic, header.version, header.message_type);
  Logger::instance().info_f("Header is valid: %s", header.is_valid() ? "true" : "false");

  // Create and encode a data request
  network::DataRequest data_request{ "sender_agent_001", "receiver_agent_001", "data_key_123",
                                     "request_id_456" };

  // Encode the data request
  auto encoded = network::ProtocolEncoder::encode_data_request(data_request);
  Logger::instance().info_f("Encoded data request: %zu bytes", encoded.size());

  // Decode the data request back
  network::DataRequest decoded_request;
  bool                 decode_success =
      network::ProtocolDecoder::decode_message(encoded.data(), encoded.size(), decoded_request);

  if(decode_success) {
    Logger::instance().info_f(
        "Decoded request: sender=%s, receiver=%s, key=%s", decoded_request.sender_id.c_str(),
        decoded_request.receiver_id.c_str(), decoded_request.data_key.c_str());
  } else {
    Logger::instance().error("Failed to decode data request");
  }

  // Create and encode a control start message
  network::ControlStart control_start{ "controller_001", "target_001", "start_workflow" };

  auto encoded_control = network::ProtocolEncoder::encode_control_start(control_start);
  Logger::instance().info_f("Encoded control start: %zu bytes", encoded_control.size());

  // Create and encode a flow condition message
  network::FlowCondition flow_condition{ "workflow_001", "node_002", "status", "active", true };

  auto encoded_flow = network::ProtocolEncoder::encode_flow_condition(flow_condition);
  Logger::instance().info_f("Encoded flow condition: %zu bytes", encoded_flow.size());
}

/**
 * Main function - run all examples
 */
int main() {
  Logger::instance().info("AgentFlow P1 Features Examples");
  Logger::instance().info("===============================");

  example_zero_copy_databus();
  Logger::instance().info("");

  example_condition_evaluation();
  Logger::instance().info("");

  example_control_flow_workflow();
  Logger::instance().info("");

  example_private_protocol();
  Logger::instance().info("");

  Logger::instance().info("All examples completed successfully!");
  return 0;
}
