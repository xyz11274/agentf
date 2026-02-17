// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example demonstrating control flow features

#include <iostream>

#include "agentflow/basic/logger.h"
#include "agentflow/runtime/control_flow.h"
#include "agentflow/runtime/zero_copy_databus.h"

using namespace agentflow;

/**
 * Example 1: Condition operators
 */
void example_condition_operators() {
  Logger::instance().info("=== Example 1: Condition Operators ===");

  runtime::ControlContext     context;
  runtime::ConditionEvaluator evaluator;

  // Set test data
  context.set_variable("score", "85");
  context.set_variable("count", "10");
  context.set_variable("name", "test");
  context.set_variable("threshold", "80");

  // Test different operators
  Logger::instance().info("Testing condition operators...");

  // Equality
  runtime::ConditionExpression expr_eq("score", runtime::ConditionOperator::EQUAL, "85");
  Logger::instance().info_f("score == 85: %s",
                            evaluator.evaluate(expr_eq, context, DataMap()) ? "true" : "false");

  // Not equal
  runtime::ConditionExpression expr_ne("score", runtime::ConditionOperator::NOT_EQUAL, "85");
  Logger::instance().info_f("score != 85: %s",
                            evaluator.evaluate(expr_ne, context, DataMap()) ? "true" : "false");

  // Greater than
  runtime::ConditionExpression expr_gt("score", runtime::ConditionOperator::GREATER_THAN, "80");
  Logger::instance().info_f("score > 80: %s",
                            evaluator.evaluate(expr_gt, context, DataMap()) ? "true" : "false");

  // Less than
  runtime::ConditionExpression expr_lt("score", runtime::ConditionOperator::LESS_THAN, "80");
  Logger::instance().info_f("score < 80: %s",
                            evaluator.evaluate(expr_lt, context, DataMap()) ? "true" : "false");

  // Greater or equal
  runtime::ConditionExpression expr_ge("score", runtime::ConditionOperator::GREATER_EQUAL, "80");
  Logger::instance().info_f("score >= 80: %s",
                            evaluator.evaluate(expr_ge, context, DataMap()) ? "true" : "false");

  // Less or equal
  runtime::ConditionExpression expr_le("score", runtime::ConditionOperator::LESS_EQUAL, "80");
  Logger::instance().info_f("score <= 80: %s",
                            evaluator.evaluate(expr_le, context, DataMap()) ? "true" : "false");

  // Contains
  runtime::ConditionExpression expr_contains("name", runtime::ConditionOperator::CONTAINS, "test");
  Logger::instance().info_f(
      "name contains 'test': %s",
      evaluator.evaluate(expr_contains, context, DataMap()) ? "true" : "false");

  // Does not contain
  runtime::ConditionExpression expr_not_contains("name", runtime::ConditionOperator::NOT_CONTAINS,
                                                 "test");
  Logger::instance().info_f(
      "name not contains 'test': %s",
      evaluator.evaluate(expr_not_contains, context, DataMap()) ? "true" : "false");

  // Is null
  runtime::ConditionExpression expr_null("name", runtime::ConditionOperator::IS_NULL, "");
  Logger::instance().info_f("name is null: %s",
                            evaluator.evaluate(expr_null, context, DataMap()) ? "true" : "false");

  // Is not null
  runtime::ConditionExpression expr_not_null("name", runtime::ConditionOperator::IS_NOT_NULL, "");
  Logger::instance().info_f(
      "name is not null: %s",
      evaluator.evaluate(expr_not_null, context, DataMap()) ? "true" : "false");

  // Complex conditions with AND/OR
  auto left_expr = std::make_shared<runtime::ConditionExpression>(
      "status", runtime::ConditionOperator::EQUAL, "active");
  auto right_expr = std::make_shared<runtime::ConditionExpression>(
      "name", runtime::ConditionOperator::CONTAINS, "test");

  // AND
  runtime::ConditionExpression expr_and;
  expr_and.left_expr  = left_expr;
  expr_and.right_expr = right_expr;
  expr_and.op         = runtime::ConditionOperator::AND;
  Logger::instance().info_f("(status == 'active') AND (name contains 'test'): %s",
                            evaluator.evaluate(expr_and, context, DataMap()) ? "true" : "false");

  // OR
  runtime::ConditionExpression expr_or;
  expr_or.left_expr  = left_expr;
  expr_or.right_expr = right_expr;
  expr_or.op         = runtime::ConditionOperator::OR;
  Logger::instance().info_f("(status == 'active') OR (name contains 'test'): %s",
                            evaluator.evaluate(expr_or, context, DataMap()) ? "true" : "false");

  Logger::instance().info("All condition operators tested");
}

/**
 * Example 2: Loop control
 */
void example_loop_control() {
  Logger::instance().info("=== Example 2: Loop Control ===");

  runtime::ControlFlowManager manager;
  manager.initialize(runtime::ControlContext());

  // Test FOR loop
  runtime::LoopConfig for_loop;
  for_loop.type           = runtime::LoopType::FOR_LOOP;
  for_loop.start_value    = 0;
  for_loop.end_value      = 10;
  for_loop.step           = 1;
  for_loop.loop_variable  = "i";
  for_loop.max_iterations = 20;

  manager.enter_loop("example_for_loop", for_loop);

  int iterations = 0;
  while(iterations < 5) {
    Logger::instance().info_f("Iteration %d", iterations);

    // Check if should continue
    if(manager.should_continue_loop("example_for_loop")) {
      // Get loop variable
      std::string current_val;
      manager.get_context().get_loop_variable("example_for_loop", "i", current_val);
      int current_value = 0;
      if(!current_val.empty()) {
        current_value = std::stoi(current_val);
      }
      Logger::instance().info_f("Loop variable i = %d", current_value);

      // Exit current iteration
      manager.exit_loop_iteration("example_for_loop");
      iterations++;
    } else {
      Logger::instance().info("Loop condition met, breaking");
      break;
    }
  }

  // Test WHILE loop
  runtime::LoopConfig while_loop;
  while_loop.type = runtime::LoopType::WHILE_LOOP;

  // Create a condition for the while loop
  auto while_condition = std::make_shared<runtime::ConditionExpression>(
      "count", runtime::ConditionOperator::LESS_THAN, "5");

  while_loop.condition = while_condition;

  manager.enter_loop("example_while_loop", while_loop);

  // Simulate while loop with condition
  manager.get_context().set_variable("count", "0");

  iterations = 0;
  while(iterations < 5) {
    Logger::instance().info_f("While loop iteration %d", iterations);

    if(manager.should_continue_loop("example_while_loop")) {
      // Increment and check condition
      std::string current_val;
      manager.get_context().get_variable("count", current_val);
      int current_count = std::stoi(current_val) + 1;
      manager.get_context().set_variable("count", std::to_string(current_count));

      manager.exit_loop_iteration("example_while_loop");
      iterations++;
    } else {
      Logger::instance().info("While condition false, breaking");
      break;
    }
  }

  // Test REPEAT loop
  runtime::LoopConfig repeat_loop;
  repeat_loop.type           = runtime::LoopType::REPEAT_LOOP;
  repeat_loop.max_iterations = 5;

  manager.enter_loop("example_repeat_loop", repeat_loop);

  for(int i = 0; i < 5; ++i) {
    Logger::instance().info_f("Repeat loop iteration %d", i);

    if(manager.should_continue_loop("example_repeat_loop")) {
      manager.exit_loop_iteration("example_repeat_loop");
    } else {
      Logger::instance().info("Repeat loop completed");
      break;
    }
  }

  Logger::instance().info("All loop control types tested");
}

/**
 * Example 3: Error handling strategies
 */
void example_error_handling() {
  Logger::instance().info("=== Example 3: Error Handling Strategies ===");

  runtime::ControlFlowManager manager;
  manager.initialize(runtime::ControlContext());

  // Test different error strategies

  // Strategy 1: RETRY
  {
    runtime::ErrorHandlerConfig retry_handler;
    retry_handler.strategy       = runtime::ErrorStrategy::RETRY;
    retry_handler.max_retries    = 3;
    retry_handler.retry_delay_ms = 500;

    manager.set_error_handler("node_retry", retry_handler);

    // Simulate failures
    for(int attempt = 1; attempt <= 4; ++attempt) {
      auto action = manager.handle_error("node_retry", "TestError", "Test error message");

      Logger::instance().info_f("Retry attempt %d, Action: %d", attempt,
                                static_cast<int>(action.type));

      if(action.type == runtime::ControlActionType::RETRY) {
        Logger::instance().info("Will retry");
      } else {
        Logger::instance().info_f("Action: %d, stopping retries", static_cast<int>(action.type));
        break;
      }
    }
  }

  // Strategy 2: SKIP
  {
    runtime::ErrorHandlerConfig skip_handler;
    skip_handler.strategy = runtime::ErrorStrategy::SKIP;

    manager.set_error_handler("node_skip", skip_handler);

    auto action = manager.handle_error("node_skip", "TestError", "Test error message");
    Logger::instance().info_f("Skip action: %d", static_cast<int>(action.type));
  }

  // Strategy 3: FAIL (abort)
  {
    runtime::ErrorHandlerConfig fail_handler;
    fail_handler.strategy = runtime::ErrorStrategy::FAIL;

    manager.set_error_handler("node_fail", fail_handler);

    auto action = manager.handle_error("node_fail", "TestError", "Test error message");
    Logger::instance().info_f("Fail action: %d", static_cast<int>(action.type));
  }

  Logger::instance().info("All error handling strategies tested");
}

/**
 * Example 4: Control actions
 */
void example_control_actions() {
  Logger::instance().info("=== Example 4: Control Actions ===");

  runtime::ControlFlowManager manager;
  manager.initialize(runtime::ControlContext());

  // Test PROCEED action
  {
    runtime::ControlAction action;
    action.type = runtime::ControlActionType::PROCEED;
    Logger::instance().info_f("PROCEED action test");
  }

  // Test SKIP action
  {
    runtime::ControlAction action;
    action.type       = runtime::ControlActionType::SKIP;
    action.skip_count = 1;
    Logger::instance().info_f("SKIP action test, skip_count: %d", action.skip_count);
  }

  // Test GOTO action
  {
    runtime::ControlAction action;
    action.type           = runtime::ControlActionType::GOTO;
    action.target_node_id = "target_node_001";
    Logger::instance().info_f("GOTO action test, target: %s", action.target_node_id.c_str());
  }

  // Test ABORT action
  {
    runtime::ControlAction action;
    action.type   = runtime::ControlActionType::ABORT;
    action.reason = "Test abort";
    Logger::instance().info_f("ABORT action test, reason: %s", action.reason.c_str());
  }

  // Test RETRY action
  {
    runtime::ControlAction action;
    action.type = runtime::ControlActionType::RETRY;
    Logger::instance().info_f("RETRY action test");
  }

  // Test RETURN action
  {
    runtime::ControlAction action;
    action.type = runtime::ControlActionType::RETURN;
    Logger::instance().info_f("RETURN action test");
  }

  // Test BREAK action
  {
    runtime::ControlAction action;
    action.type = runtime::ControlActionType::BREAK;
    Logger::instance().info_f("BREAK action test");
  }

  // Test CONTINUE action
  {
    runtime::ControlAction action;
    action.type = runtime::ControlActionType::CONTINUE;
    Logger::instance().info_f("CONTINUE action test");
  }

  Logger::instance().info("All control actions tested");
}

/**
 * Example 5: Next node determination
 */
void example_next_node_determination() {
  Logger::instance().info("=== Example 5: Next Node Determination ===");

  runtime::ControlFlowManager manager;
  manager.initialize(runtime::ControlContext());

  // Test next node determination after different execution states

  // Test after successful execution
  {
    auto action = manager.determine_next_action("node_001", NodeState::SUCCESS, "");
    Logger::instance().info_f("Action after success: %d", static_cast<int>(action.type));

    switch(action.type) {
      case runtime::ControlActionType::PROCEED:
        Logger::instance().info("PROCEED to next node");
        break;
      case runtime::ControlActionType::RETRY:
        Logger::instance().info("RETRY current node");
        break;
      case runtime::ControlActionType::GOTO:
        Logger::instance().info_f("GOTO to: %s", action.target_node_id.c_str());
        break;
      case runtime::ControlActionType::ABORT:
        Logger::instance().info_f("ABORT workflow");
        break;
      default:
        Logger::instance().info("Unknown action");
        break;
    }
  }

  // Test after failed execution
  {
    auto action = manager.determine_next_action("node_002", NodeState::FAILED, "Timeout error");
    Logger::instance().info_f("Action after failure: %d, error: %s", static_cast<int>(action.type),
                              action.reason.c_str());

    // Test with error handler set
    runtime::ErrorHandlerConfig retry_handler;
    retry_handler.strategy = runtime::ErrorStrategy::RETRY;
    manager.set_error_handler("node_002", retry_handler);

    auto action_with_handler = manager.handle_error("node_002", "TimeoutError", "Timeout error");
    Logger::instance().info_f("Action with handler: %d",
                              static_cast<int>(action_with_handler.type));

    Logger::instance().info("All next node determination scenarios tested");
  }
}

/**
 * Example 6: Integration with data bus
 */
void example_data_bus_integration() {
  Logger::instance().info("=== Example 6: Data Bus Integration ===");

  runtime::ZeroCopyDataBus databus(1024 * 1024);  // 1MB

  // Simulate data updates
  databus.put("key1", "initial_value_1");

  // Subscribe to data changes
  int update_count = 0;
  databus.subscribe("key1", [&update_count](const std::string& key, const DataVariant& value) {
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
    Logger::instance().info_f("Data update #%d: %s = %s", ++update_count, key.c_str(),
                              value_str.c_str());
  });

  // Simulate multiple updates
  for(int i = 1; i <= 3; ++i) {
    databus.put("key1", "value_" + std::to_string(i));
  }

  Logger::instance().info_f("Final data count: %d", update_count);

  // Unsubscribe
  databus.unsubscribe("key1");

  // Test lock/unlock operations
  databus.lock_write();
  Logger::instance().info("Write lock acquired");

  databus.put("key2", "locked_value");
  databus.unlock_write();

  databus.lock_read();
  Logger::instance().info("Read lock acquired");

  DataVariant value;
  if(databus.get("key2", value)) {
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
    Logger::instance().info_f("Read key2: %s", value_str.c_str());
  }

  databus.unlock_read();

  Logger::instance().info_f("Capacity usage: %.1f%%", databus.get_capacity_usage() * 100.0);
}

/**
 * Main function
 */
int main() {
  Logger::instance().info("AgentFlow Control Flow Features Example");

  // Run all examples
  example_condition_operators();
  example_loop_control();
  example_error_handling();
  example_control_actions();
  example_next_node_determination();
  example_data_bus_integration();

  Logger::instance().info("All control flow examples completed");

  return 0;
}
