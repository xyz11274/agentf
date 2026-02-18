// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Control flow management implementation

#include "runtime/scheduler/control_flow.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "utils/logger.h"

namespace agentflow {
namespace runtime {

// ============================================================================
// ControlContext implementation
// ============================================================================

ControlContext::ControlContext() {
  // Initialize with empty state
}

bool ControlContext::get_variable(const std::string& name, std::string& value) const {
  auto it = variables_.find(name);
  if(it != variables_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

void ControlContext::set_variable(const std::string& name, const std::string& value) {
  variables_[name] = value;
}

bool ControlContext::has_variable(const std::string& name) const {
  return variables_.find(name) != variables_.end();
}

void ControlContext::clear_variables() {
  variables_.clear();
}

int ControlContext::get_loop_iteration(const std::string& loop_id) const {
  auto it = loops_.find(loop_id);
  if(it != loops_.end()) {
    return it->second.iteration;
  }
  return 0;
}

void ControlContext::increment_loop_iteration(const std::string& loop_id) {
  auto it = loops_.find(loop_id);
  if(it != loops_.end()) {
    it->second.iteration++;
  }
}

void ControlContext::reset_loop_iteration(const std::string& loop_id) {
  auto it = loops_.find(loop_id);
  if(it != loops_.end()) {
    it->second.iteration = 0;
  }
}

bool ControlContext::should_continue_loop(const std::string& loop_id, const LoopConfig& config) {
  auto it = loops_.find(loop_id);
  if(it == loops_.end()) {
    return false;
  }

  const LoopState& state = it->second;

  switch(config.type) {
    case LoopType::FOR_LOOP: {
      int current = state.iteration;
      int end     = config.end_value;
      int step    = config.step;

      if(step > 0) {
        return current < end;
      } else if(step < 0) {
        return current > end;
      }
      return false;
    }

    case LoopType::WHILE_LOOP: {
      if(!config.condition) {
        return false;
      }
      // Evaluate condition
      ConditionEvaluator evaluator;
      return evaluator.evaluate(*config.condition, *this, DataMap());
    }

    case LoopType::FOR_EACH_LOOP: {
      // Iterate over collection stored in a context variable.
      // The collection is a comma-separated string in collection_variable.
      // Each iteration sets item_variable to the current element.
      std::string collection_str;
      if(!get_variable(config.collection_variable, collection_str)) {
        return false;
      }

      // Count elements in the comma-separated collection
      int element_count = 1;
      for(char c : collection_str) {
        if(c == ',') {
          element_count++;
        }
      }

      if(state.iteration >= element_count) {
        return false;
      }

      // Extract current element and set as item variable
      int         idx = 0;
      std::string current_item;
      size_t      start = 0;
      for(size_t i = 0; i <= collection_str.size(); ++i) {
        if(i == collection_str.size() || collection_str[i] == ',') {
          if(idx == state.iteration) {
            current_item = collection_str.substr(start, i - start);
            // Trim whitespace
            size_t first = current_item.find_first_not_of(" \t");
            size_t last  = current_item.find_last_not_of(" \t");
            if(first != std::string::npos) {
              current_item = current_item.substr(first, last - first + 1);
            }
            break;
          }
          idx++;
          start = i + 1;
        }
      }

      // Set the item variable in context and loop state
      set_variable(config.item_variable, current_item);
      auto mut_it = loops_.find(loop_id);
      if(mut_it != loops_.end()) {
        mut_it->second.variables[config.item_variable] = current_item;
      }

      return true;
    }

    case LoopType::REPEAT_LOOP: {
      return state.iteration < config.max_iterations;
    }

    default:
      return false;
  }
}

bool ControlContext::get_loop_variable(const std::string& loop_id, const std::string& var_name,
                                       std::string& value) const {
  auto it = loops_.find(loop_id);
  if(it == loops_.end()) {
    return false;
  }

  auto var_it = it->second.variables.find(var_name);
  if(var_it != it->second.variables.end()) {
    value = var_it->second;
    return true;
  }

  return false;
}

void ControlContext::set_loop_variable(const std::string& loop_id, const std::string& var_name,
                                       const std::string& value) {
  loops_[loop_id].variables[var_name] = value;
}

void ControlContext::enter_loop(const std::string& loop_id, const LoopConfig& config) {
  LoopState state;
  state.config    = config;
  state.iteration = 0;
  loops_[loop_id] = state;
}

void ControlContext::exit_loop(const std::string& loop_id) {
  loops_.erase(loop_id);
}

bool ControlContext::is_in_loop(const std::string& loop_id) const {
  return loops_.find(loop_id) != loops_.end();
}

std::vector<std::string> ControlContext::get_active_loops() const {
  std::vector<std::string> loop_ids;
  for(const auto& pair : loops_) {
    loop_ids.push_back(pair.first);
  }
  return loop_ids;
}

// ============================================================================
// ConditionEvaluator implementation
// ============================================================================

bool ConditionEvaluator::evaluate(const ConditionExpression& expr, const ControlContext& context,
                                  const DataMap& node_outputs) const {
  switch(expr.op) {
    case ConditionOperator::AND: {
      if(expr.left_expr && expr.right_expr) {
        return evaluate(*expr.left_expr, context, node_outputs)
               && evaluate(*expr.right_expr, context, node_outputs);
      }
      return false;
    }

    case ConditionOperator::OR: {
      if(expr.left_expr && expr.right_expr) {
        return evaluate(*expr.left_expr, context, node_outputs)
               || evaluate(*expr.right_expr, context, node_outputs);
      }
      return false;
    }

    case ConditionOperator::NOT: {
      if(expr.left_expr) {
        return !evaluate(*expr.left_expr, context, node_outputs);
      }
      return false;
    }

    case ConditionOperator::IS_NULL: {
      std::string value = resolve_value(expr.left_operand, context, node_outputs);
      return value.empty();
    }

    case ConditionOperator::IS_NOT_NULL: {
      std::string value = resolve_value(expr.left_operand, context, node_outputs);
      return !value.empty();
    }

    default: {
      // Comparison operators
      std::string left  = resolve_value(expr.left_operand, context, node_outputs);
      std::string right = resolve_value(expr.right_operand, context, node_outputs);
      return evaluate_comparison(left, expr.op, right);
    }
  }
}

bool ConditionEvaluator::evaluate_comparison(const std::string& left, ConditionOperator op,
                                             const std::string& right) const {
  // Try numeric comparison
  double left_num   = 0.0;
  double right_num  = 0.0;
  bool   is_numeric = false;

  try {
    left_num   = std::stod(left);
    right_num  = std::stod(right);
    is_numeric = true;
  } catch(const std::exception&) {
    // Not numeric, use string comparison
  }

  switch(op) {
    case ConditionOperator::EQUAL:
      return is_numeric ? (left_num == right_num) : (left == right);

    case ConditionOperator::NOT_EQUAL:
      return is_numeric ? (left_num != right_num) : (left != right);

    case ConditionOperator::GREATER_THAN:
      return is_numeric ? (left_num > right_num) : (left > right);

    case ConditionOperator::LESS_THAN:
      return is_numeric ? (left_num < right_num) : (left < right);

    case ConditionOperator::GREATER_EQUAL:
      return is_numeric ? (left_num >= right_num) : (left >= right);

    case ConditionOperator::LESS_EQUAL:
      return is_numeric ? (left_num <= right_num) : (left <= right);

    case ConditionOperator::CONTAINS:
      return left.find(right) != std::string::npos;

    case ConditionOperator::NOT_CONTAINS:
      return left.find(right) == std::string::npos;

    default:
      return false;
  }
}

std::string ConditionEvaluator::resolve_value(const std::string&    name,
                                              const ControlContext& context,
                                              const DataMap&        node_outputs) const {
  // Try to resolve as variable from context
  std::string value;
  if(context.get_variable(name, value)) {
    return value;
  }

  // Try to resolve as output from node
  auto it = node_outputs.find(name);
  if(it != node_outputs.end()) {
    // Convert DataVariant to string
    return std::visit(
        [](auto&& arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, std::string>) {
            return arg;
          } else if constexpr(std::is_same_v<T, int64_t>) {
            return std::to_string(arg);
          } else if constexpr(std::is_same_v<T, double>) {
            return std::to_string(arg);
          } else if constexpr(std::is_same_v<T, bool>) {
            return arg ? "true" : "false";
          }
          return "";
        },
        it->second);
  }

  // Return name itself if not found (literal value)
  return name;
}

// ============================================================================
// ControlFlowManager implementation
// ============================================================================

ControlFlowManager::ControlFlowManager() {
}

void ControlFlowManager::initialize(const ControlContext& context) {
  context_ = context;
}

bool ControlFlowManager::should_traverse_edge(const std::string&         edge_id,
                                              const ConditionExpression& condition) {
  return evaluator_.evaluate(condition, context_, DataMap());
}

ControlAction ControlFlowManager::determine_next_action(const NodeId&      current_node,
                                                        NodeState          execution_state,
                                                        const std::string& error_message) {
  ControlAction action;

  if(execution_state == NodeState::SUCCESS) {
    action.type = ControlActionType::PROCEED;
  } else if(execution_state == NodeState::FAILED) {
    // Check if there's an error handler
    auto it = error_handlers_.find(current_node);
    if(it != error_handlers_.end()) {
      const ErrorHandlerConfig& handler = it->second;

      switch(handler.strategy) {
        case ErrorStrategy::RETRY:
          action.type = ControlActionType::RETRY;
          break;

        case ErrorStrategy::SKIP:
          action.type       = ControlActionType::SKIP;
          action.skip_count = 1;
          break;

        case ErrorStrategy::FAIL:
          action.type   = ControlActionType::ABORT;
          action.reason = error_message;
          break;

        case ErrorStrategy::CATCH:
          action.type           = ControlActionType::GOTO;
          action.target_node_id = handler.handler_node_id;
          break;

        case ErrorStrategy::FALLBACK:
          action.type           = ControlActionType::GOTO;
          action.target_node_id = handler.fallback_node_id;
          break;
      }
    } else {
      // No error handler, abort workflow
      action.type   = ControlActionType::ABORT;
      action.reason = error_message;
    }
  }

  return action;
}

NodeId ControlFlowManager::get_next_node(const NodeId& current_node, const ControlAction& action) {
  switch(action.type) {
    case ControlActionType::GOTO:
      return action.target_node_id;

    case ControlActionType::RETRY:
      return current_node;

    case ControlActionType::SKIP:
      // Return empty to skip (caller should determine next node)
      return "";

    case ControlActionType::PROCEED:
    case ControlActionType::CONTINUE:
      // Return empty to proceed normally (caller should use edge logic)
      return "";

    case ControlActionType::BREAK:
    case ControlActionType::ABORT:
      // Return empty to signal termination
      return "";

    case ControlActionType::RETURN:
      // Signal return from loop
      return "__RETURN__";

    default:
      return "";
  }
}

void ControlFlowManager::enter_loop(const std::string& loop_id, const LoopConfig& config) {
  active_loops_[loop_id] = config;
  context_.enter_loop(loop_id, config);
}

void ControlFlowManager::exit_loop_iteration(const std::string& loop_id) {
  context_.increment_loop_iteration(loop_id);
}

bool ControlFlowManager::should_continue_loop(const std::string& loop_id) {
  auto it = active_loops_.find(loop_id);
  if(it == active_loops_.end()) {
    return false;
  }

  return context_.should_continue_loop(loop_id, it->second);
}

ControlAction ControlFlowManager::handle_error(const NodeId&      failed_node,
                                               const std::string& error_type,
                                               const std::string& error_message) {
  // Find error handler for this node
  auto it = error_handlers_.find(failed_node);
  if(it == error_handlers_.end()) {
    // No handler, abort
    ControlAction action;
    action.type   = ControlActionType::ABORT;
    action.reason = error_message;
    return action;
  }

  const ErrorHandlerConfig& handler = it->second;

  // Check if error type matches
  if(!handler.error_types.empty()) {
    bool matches = false;
    for(const auto& type : handler.error_types) {
      if(error_type.find(type) != std::string::npos) {
        matches = true;
        break;
      }
    }
    if(!matches) {
      // Error type doesn't match, abort
      ControlAction action;
      action.type   = ControlActionType::ABORT;
      action.reason = error_message;
      return action;
    }
  }

  // Evaluate condition if present
  if(handler.condition) {
    if(!evaluator_.evaluate(*handler.condition, context_, DataMap())) {
      // Condition doesn't match, abort
      ControlAction action;
      action.type   = ControlActionType::ABORT;
      action.reason = error_message;
      return action;
    }
  }

  // Execute handler strategy
  ControlAction action;
  switch(handler.strategy) {
    case ErrorStrategy::RETRY:
      action.type = ControlActionType::RETRY;
      break;

    case ErrorStrategy::SKIP:
      action.type       = ControlActionType::SKIP;
      action.skip_count = 1;
      break;

    case ErrorStrategy::FAIL:
      action.type   = ControlActionType::ABORT;
      action.reason = error_message;
      break;

    case ErrorStrategy::CATCH:
      action.type           = ControlActionType::GOTO;
      action.target_node_id = handler.handler_node_id;
      break;

    case ErrorStrategy::FALLBACK:
      action.type           = ControlActionType::GOTO;
      action.target_node_id = handler.fallback_node_id;
      break;
  }

  return action;
}

void ControlFlowManager::set_error_handler(const NodeId&             node_id,
                                           const ErrorHandlerConfig& handler) {
  error_handlers_[node_id] = handler;
}

bool ControlFlowManager::get_error_handler(const NodeId&       node_id,
                                           ErrorHandlerConfig& handler) const {
  auto it = error_handlers_.find(node_id);
  if(it != error_handlers_.end()) {
    handler = it->second;
    return true;
  }
  return false;
}

}  // namespace runtime
}  // namespace agentflow
