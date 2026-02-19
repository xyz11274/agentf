// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Control flow mechanisms for complex workflow execution

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace runtime {

/**
 * Condition operator types
 */
enum class ConditionOperator {
  EQUAL         = 0,
  NOT_EQUAL     = 1,
  GREATER_THAN  = 2,
  LESS_THAN     = 3,
  GREATER_EQUAL = 4,
  LESS_EQUAL    = 5,
  CONTAINS      = 6,
  NOT_CONTAINS  = 7,
  IS_NULL       = 8,
  IS_NOT_NULL   = 9,
  AND           = 10,
  OR            = 11,
  NOT           = 12,
};

/**
 * Condition expression for conditional execution
 */
struct ConditionExpression {
  std::string       left_operand;  // Variable name or value
  ConditionOperator op;
  std::string       right_operand;      // Variable name or value
  bool              is_left_variable;   // true if left_operand is a variable
  bool              is_right_variable;  // true if right_operand is a variable

  // For complex expressions (AND/OR)
  std::shared_ptr<ConditionExpression> left_expr;
  std::shared_ptr<ConditionExpression> right_expr;

  // Default constructor for simple expression
  ConditionExpression()
      : op(ConditionOperator::EQUAL), is_left_variable(true), is_right_variable(false) {}

  // Constructor for simple expression
  ConditionExpression(const std::string& left, ConditionOperator op, const std::string& right,
                      bool left_is_var = true, bool right_is_var = false)
      : left_operand(left),
        op(op),
        right_operand(right),
        is_left_variable(left_is_var),
        is_right_variable(right_is_var) {}
};

/**
 * Control flow action types
 */
enum class ControlActionType {
  PROCEED  = 0,  // Continue normal execution
  SKIP     = 1,  // Skip next node(s)
  GOTO     = 2,  // Jump to specific node
  RETRY    = 3,  // Retry current node
  ABORT    = 4,  // Abort workflow
  RETURN   = 5,  // Return from loop/sub-workflow
  BREAK    = 6,  // Break from loop
  CONTINUE = 7,  // Continue loop iteration
};

/**
 * Control flow action
 */
struct ControlAction {
  ControlActionType type;
  std::string       target_node_id;  // For GOTO action
  int               skip_count;      // For SKIP action
  std::string       reason;          // Reason for action

  ControlAction() : type(ControlActionType::PROCEED), skip_count(0) {}
};

/**
 * Loop types
 */
enum class LoopType {
  FOR_LOOP      = 0,  // Fixed count loop
  WHILE_LOOP    = 1,  // Conditional loop
  FOR_EACH_LOOP = 2,  // Iterate over collection
  REPEAT_LOOP   = 3,  // Repeat N times
};

/**
 * Loop configuration
 */
struct LoopConfig {
  LoopType type;

  // For FOR loop
  int         start_value;
  int         end_value;
  int         step;
  std::string loop_variable;  // Variable name for loop index

  // For WHILE loop
  std::shared_ptr<ConditionExpression> condition;

  // For FOR_EACH loop
  std::string collection_variable;  // Collection to iterate over
  std::string item_variable;        // Variable name for current item

  // Maximum iterations (to prevent infinite loops)
  int max_iterations;
};

/**
 * Error handling strategy
 */
enum class ErrorStrategy {
  RETRY    = 0,  // Retry the failed node
  SKIP     = 1,  // Skip failed node and continue
  FAIL     = 2,  // Fail the workflow
  CATCH    = 3,  // Catch and handle with specific handler
  FALLBACK = 4,  // Use fallback node
};

/**
 * Error handler configuration
 */
struct ErrorHandlerConfig {
  ErrorStrategy strategy;
  int           max_retries;       // For RETRY strategy
  int           retry_delay_ms;    // Delay between retries
  std::string   fallback_node_id;  // For FALLBACK strategy
  std::string   handler_node_id;   // For CATCH strategy

  // Conditions for applying this handler
  std::vector<std::string>             error_types;
  std::shared_ptr<ConditionExpression> condition;
};

/**
 * Execution context for control flow
 * Stores runtime state for conditions, loops, etc.
 */
class ControlContext {
public:
  ControlContext();
  ~ControlContext() = default;

  /**
   * Get variable value
   */
  bool get_variable(const std::string& name, std::string& value) const;

  /**
   * Set variable value
   */
  void set_variable(const std::string& name, const std::string& value);

  /**
   * Check if variable exists
   */
  bool has_variable(const std::string& name) const;

  /**
   * Clear all variables
   */
  void clear_variables();

  /**
   * Get loop iteration count
   */
  int get_loop_iteration(const std::string& loop_id) const;

  /**
   * Increment loop iteration
   */
  void increment_loop_iteration(const std::string& loop_id);

  /**
   * Reset loop iteration
   */
  void reset_loop_iteration(const std::string& loop_id);

  /**
   * Check if loop should continue
   */
  bool should_continue_loop(const std::string& loop_id, const LoopConfig& config);

  /**
   * Get current loop variable value
   */
  bool get_loop_variable(const std::string& loop_id, const std::string& var_name,
                         std::string& value) const;

  /**
   * Set loop variable value
   */
  void set_loop_variable(const std::string& loop_id, const std::string& var_name,
                         const std::string& value);

  /**
   * Enter loop context
   */
  void enter_loop(const std::string& loop_id, const LoopConfig& config);

  /**
   * Exit loop context
   */
  void exit_loop(const std::string& loop_id);

  /**
   * Check if in a loop
   */
  bool is_in_loop(const std::string& loop_id) const;

  /**
   * Get all active loops
   */
  std::vector<std::string> get_active_loops() const;

private:
  struct LoopState {
    LoopConfig                         config;
    int                                iteration;
    std::map<std::string, std::string> variables;
  };

  std::map<std::string, std::string> variables_;
  std::map<std::string, LoopState>   loops_;
};

/**
 * Condition evaluator
 * Evaluates condition expressions against context
 */
class ConditionEvaluator {
public:
  ConditionEvaluator()  = default;
  ~ConditionEvaluator() = default;

  /**
   * Evaluate a condition expression
   */
  bool evaluate(const ConditionExpression& expr, const ControlContext& context,
                const DataMap& node_outputs) const;

  /**
   * Evaluate simple comparison
   */
  bool evaluate_comparison(const std::string& left, ConditionOperator op,
                           const std::string& right) const;

  /**
   * Get variable value from context or outputs
   */
  std::string resolve_value(const std::string& name, const ControlContext& context,
                            const DataMap& node_outputs) const;
};

/**
 * Control flow manager
 * Manages conditional execution, loops, and error handling
 */
class ControlFlowManager {
public:
  ControlFlowManager();
  ~ControlFlowManager() = default;

  /**
   * Initialize control flow with workflow config
   */
  void initialize(const ControlContext& context);

  /**
   * Get current control context
   */
  ControlContext&       get_context() { return context_; }
  const ControlContext& get_context() const { return context_; }

  /**
   * Check edge condition
   */
  bool should_traverse_edge(const std::string& edge_id, const ConditionExpression& condition);

  /**
   * Determine next action after node execution
   */
  ControlAction determine_next_action(const NodeId& current_node, NodeState execution_state,
                                      const std::string& error_message);

  /**
   * Get next node to execute
   */
  NodeId get_next_node(const NodeId& current_node, const ControlAction& action);

  /**
   * Enter a loop
   */
  void enter_loop(const std::string& loop_id, const LoopConfig& config);

  /**
   * Exit current loop iteration
   */
  void exit_loop_iteration(const std::string& loop_id);

  /**
   * Check if should continue loop
   */
  bool should_continue_loop(const std::string& loop_id);

  /**
   * Handle error
   */
  ControlAction handle_error(const NodeId& failed_node, const std::string& error_type,
                             const std::string& error_message);

  /**
   * Set error handler for a node
   */
  void set_error_handler(const NodeId& node_id, const ErrorHandlerConfig& handler);

  /**
   * Get error handler for a node
   */
  bool get_error_handler(const NodeId& node_id, ErrorHandlerConfig& handler) const;

private:
  ControlContext                       context_;
  ConditionEvaluator                   evaluator_;
  std::map<NodeId, ErrorHandlerConfig> error_handlers_;
  std::map<std::string, LoopConfig>    active_loops_;
};

/**
 * Control flow node types
 * Special nodes for control flow operations
 */
namespace ControlNodes {

/**
 * Conditional node
 * Routes execution based on condition
 */
struct ConditionalNodeConfig {
  NodeId                                              node_id;
  std::vector<std::pair<ConditionExpression, NodeId>> branches;
  NodeId default_branch;  // Node to execute if no condition matches
};

/**
 * Loop node
 * Repeats execution of child nodes
 */
struct LoopNodeConfig {
  NodeId              node_id;
  LoopConfig          loop_config;
  std::vector<NodeId> body_nodes;  // Nodes in loop body
};

/**
 * Exception handler node
 * Handles errors from other nodes
 */
struct ExceptionHandlerNodeConfig {
  NodeId                   node_id;
  std::vector<NodeId>      target_nodes;  // Nodes this handler catches from
  std::vector<std::string> error_types;   // Error types to catch
  ErrorHandlerConfig       handler_config;
};

}  // namespace ControlNodes

}  // namespace runtime
}  // namespace agentflow
