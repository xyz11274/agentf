#include "agentflow/core/workflow.h"

#include <algorithm>
#include <queue>
#include <unordered_set>

#include "agentflow/utils/logger.h"

namespace agentflow {
namespace framework {

Workflow::Workflow(const Config& config) : config_(config), state_(WorkflowState::CREATED) {
  // Initialize control flow manager
  control_flow_manager_.initialize(runtime::ControlContext());
}

void Workflow::add_node(std::shared_ptr<Node> node) {
  if(node) {
    node_map_[node->get_id()] = node;
    nodes_.push_back(node);
  }
}

void Workflow::remove_node(const std::string& node_id) {
  auto it = node_map_.find(node_id);
  if(it != node_map_.end()) {
    node_map_.erase(it);
    nodes_.erase(std::remove_if(
                     nodes_.begin(), nodes_.end(),
                     [&node_id](const std::shared_ptr<Node>& n) { return n->get_id() == node_id; }),
                 nodes_.end());
  }
}

std::shared_ptr<Node> Workflow::get_node(const std::string& node_id) {
  auto it = node_map_.find(node_id);
  return it != node_map_.end() ? it->second : nullptr;
}

void Workflow::add_edge(std::shared_ptr<Edge> edge) {
  if(edge) {
    edges_.push_back(edge);
  }
}

void Workflow::remove_edge(const std::string& edge_id) {
  edges_.erase(
      std::remove_if(edges_.begin(), edges_.end(),
                     [&edge_id](const std::shared_ptr<Edge>& e) { return e->get_id() == edge_id; }),
      edges_.end());
}

bool Workflow::load_from_dsl(const std::string& dsl_content) {
  // Parse DSL and build workflow
  // In real implementation, this would parse JSON or other DSL format
  state_ = WorkflowState::LOADED;
  return true;
}

bool Workflow::validate() {
  if(!validate_dag()) {
    return false;
  }
  state_ = WorkflowState::LOADED;
  return true;
}

bool Workflow::update_dynamic(const std::string& dsl_content) {
  // Dynamically update workflow DSL without recreating instance
  return load_from_dsl(dsl_content);
}

std::vector<std::string> Workflow::get_topological_order() {
  // If cached order is valid, return it
  if(topological_order_valid_) {
    return topological_order_;
  }

  // Compute topological order using Kahn's algorithm
  compute_topological_order();

  return topological_order_;
}

std::vector<std::string> Workflow::get_executable_nodes() {
  std::vector<std::string> executable;

  for(const auto& node : nodes_) {
    // Check if all dependencies are satisfied
    bool all_deps_ready = true;
    int  in_edge_count  = 0;

    for(const auto& edge : edges_) {
      if(edge->get_target() == node->get_id()) {
        in_edge_count++;
        // Check if source node is done
        auto source = get_node(edge->get_source());
        if(source && source->get_state() != NodeState::SUCCESS) {
          all_deps_ready = false;
          break;
        }
      }
    }

    // Nodes with no incoming edges (sources) are always ready to execute
    // if they are in READY state
    if(in_edge_count == 0) {
      all_deps_ready = true;
    }

    if(all_deps_ready && node->get_state() == NodeState::READY) {
      executable.push_back(node->get_id());
    }
  }

  return executable;
}

std::vector<std::string> Workflow::get_executable_nodes_with_control() {
  std::vector<std::string> executable;

  for(const auto& node : nodes_) {
    // Check if node is already executed
    if(executed_nodes_.count(node->get_id())) {
      continue;
    }

    // Check if all dependencies are satisfied
    bool all_deps_ready = true;
    int  in_edge_count  = 0;

    for(const auto& edge : edges_) {
      if(edge->get_target() == node->get_id()) {
        in_edge_count++;

        // Evaluate edge condition if present
        bool should_traverse = true;
        if(edge->has_condition()) {
          should_traverse = control_flow_manager_.should_traverse_edge(
              edge->get_id(), parse_condition_expression(edge->get_condition_expression()));
        }

        if(should_traverse) {
          auto source = get_node(edge->get_source());
          if(source && source->get_state() != NodeState::SUCCESS) {
            all_deps_ready = false;
            break;
          }
        }
      }
    }

    // Nodes with no incoming edges (sources) or all conditions met
    if(in_edge_count == 0 || all_deps_ready) {
      if(node->get_state() == NodeState::READY) {
        executable.push_back(node->get_id());
      }
    }
  }

  return executable;
}

std::string Workflow::get_next_node_after_execution(const std::string& current_node, bool success,
                                                    const std::string& error_msg) {
  // Determine control action after node execution
  NodeState execution_state = success ? NodeState::SUCCESS : NodeState::FAILED;
  auto      action =
      control_flow_manager_.determine_next_action(current_node, execution_state, error_msg);

  // Handle different control actions
  switch(action.type) {
    case runtime::ControlActionType::RETRY:
      // Retry the same node
      return current_node;

    case runtime::ControlActionType::SKIP:
      // Skip to next node in topological order
      return "";

    case runtime::ControlActionType::GOTO:
      // Jump to specific node
      return action.target_node_id;

    case runtime::ControlActionType::ABORT:
      // Workflow abort
      return "__ABORT__";

    case runtime::ControlActionType::RETURN:
      // Return from loop
      return "__RETURN__";

    case runtime::ControlActionType::BREAK:
      // Break from loop
      return "__BREAK__";

    case runtime::ControlActionType::CONTINUE:
      // Continue loop iteration
      return "__CONTINUE__";

    default:
      // Proceed to next node
      return "";
  }
}

void Workflow::mark_node_executed(const std::string& node_id, bool success,
                                  const std::string& error_msg) {
  executed_nodes_[node_id] = success;

  // Update node state
  auto node = get_node(node_id);
  if(node) {
    node->set_state(success ? NodeState::SUCCESS : NodeState::FAILED);
    if(!success) {
      node->reset_retry_count();
    }
  }

  // If failed, let control flow manager handle error
  if(!success) {
    // Determine error type from error message
    std::string error_type = "UnknownError";
    if(error_msg.find("timeout") != std::string::npos) {
      error_type = "TimeoutError";
    } else if(error_msg.find("memory") != std::string::npos) {
      error_type = "MemoryError";
    } else if(error_msg.find("permission") != std::string::npos) {
      error_type = "PermissionError";
    }

    auto action = control_flow_manager_.handle_error(node_id, error_type, error_msg);
  }
}

void Workflow::enter_loop(const std::string& loop_id, const runtime::LoopConfig& config) {
  control_flow_manager_.enter_loop(loop_id, config);
}

void Workflow::exit_loop_iteration(const std::string& loop_id) {
  control_flow_manager_.exit_loop_iteration(loop_id);
}

bool Workflow::should_continue_loop(const std::string& loop_id) {
  return control_flow_manager_.should_continue_loop(loop_id);
}

void Workflow::set_error_handler(const std::string&                 node_id,
                                 const runtime::ErrorHandlerConfig& handler) {
  control_flow_manager_.set_error_handler(node_id, handler);
}

void Workflow::reset_execution_state() {
  executed_nodes_.clear();
  node_retry_counts_.clear();
  topological_order_valid_ = false;

  // Reset all node states
  for(auto& node : nodes_) {
    node->set_state(NodeState::READY);
  }
}

bool Workflow::validate_dag() {
  // Check for cycles in DAG using DFS coloring
  enum class Color {
    WHITE,  // Not visited
    GRAY,   // Being visited (in recursion stack)
    BLACK   // Visited
  };

  std::map<std::string, Color> colors;

  // Initialize all nodes as WHITE
  for(const auto& node : nodes_) {
    colors[node->get_id()] = Color::WHITE;
  }

  // Build adjacency list for graph
  std::map<std::string, std::vector<std::string>> adjacency;
  for(const auto& edge : edges_) {
    adjacency[edge->get_source()].push_back(edge->get_target());
  }

  // DFS to detect cycles
  std::function<bool(const std::string&)> dfs = [&](const std::string& node_id) -> bool {
    colors[node_id] = Color::GRAY;  // Mark as being visited

    // Visit all neighbors
    if(adjacency.find(node_id) != adjacency.end()) {
      for(const auto& neighbor_id : adjacency[node_id]) {
        if(colors[neighbor_id] == Color::GRAY) {
          // Back edge found - cycle detected
          Logger::instance().error_f("Cycle detected in workflow: %s -> %s", node_id.c_str(),
                                     neighbor_id.c_str());
          return false;
        } else if(colors[neighbor_id] == Color::WHITE) {
          if(!dfs(neighbor_id)) {
            return false;
          }
        }
      }
    }

    colors[node_id] = Color::BLACK;  // Mark as fully visited
    return true;
  };

  // Run DFS from each unvisited node
  for(const auto& node : nodes_) {
    if(colors[node->get_id()] == Color::WHITE) {
      if(!dfs(node->get_id())) {
        return false;
      }
    }
  }

  Logger::instance().info("Workflow DAG validation passed - no cycles detected");
  return true;
}

bool Workflow::compute_topological_order() {
  // Use Kahn's algorithm to compute topological order
  std::map<std::string, int> in_degree;

  // Initialize in-degree of all nodes
  for(const auto& node : nodes_) {
    in_degree[node->get_id()] = 0;
  }

  // Calculate in-degree for each node
  for(const auto& edge : edges_) {
    in_degree[edge->get_target()]++;
  }

  // Queue for nodes with zero in-degree
  std::queue<std::string> zero_in_degree;
  for(const auto& node : nodes_) {
    if(in_degree[node->get_id()] == 0) {
      zero_in_degree.push(node->get_id());
    }
  }

  // Build adjacency list
  std::map<std::string, std::vector<std::string>> adjacency;
  for(const auto& edge : edges_) {
    adjacency[edge->get_source()].push_back(edge->get_target());
  }

  // Process nodes in topological order
  topological_order_.clear();
  int processed_count = 0;

  while(!zero_in_degree.empty()) {
    std::string current = zero_in_degree.front();
    zero_in_degree.pop();
    topological_order_.push_back(current);
    processed_count++;

    // Remove all outgoing edges from current node
    if(adjacency.find(current) != adjacency.end()) {
      for(const auto& neighbor : adjacency[current]) {
        in_degree[neighbor]--;
        if(in_degree[neighbor] == 0) {
          zero_in_degree.push(neighbor);
        }
      }
    }
  }

  // Check if topological sort is possible (no cycle)
  if(processed_count != static_cast<int>(nodes_.size())) {
    Logger::instance().error("Failed to compute topological order - cycle detected");
    topological_order_.clear();
    return false;
  }

  topological_order_valid_ = true;

  Logger::instance().info_f("Computed topological order with %zu nodes", topological_order_.size());

  return true;
}

runtime::ConditionExpression Workflow::parse_condition_expression(const std::string& expr_str) {
  // Cache lookup — avoid re-parsing the same expression
  auto cache_it = condition_cache_.find(expr_str);
  if(cache_it != condition_cache_.end()) {
    return cache_it->second;
  }

  // Simple parser for condition expressions
  // Format: "left_operand operator right_operand"
  // Operators: ==, !=, >, <, >=, <=, contains, not_contains

  runtime::ConditionExpression expr;

  // Parse operator
  if(expr_str.find("==") != std::string::npos) {
    auto pos               = expr_str.find("==");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 2);
    expr.op                = runtime::ConditionOperator::EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find("!=") != std::string::npos) {
    auto pos               = expr_str.find("!=");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 2);
    expr.op                = runtime::ConditionOperator::NOT_EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find(">=") != std::string::npos) {
    auto pos               = expr_str.find(">=");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 2);
    expr.op                = runtime::ConditionOperator::GREATER_EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find("<=") != std::string::npos) {
    auto pos               = expr_str.find("<=");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 2);
    expr.op                = runtime::ConditionOperator::LESS_EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find(">") != std::string::npos && expr_str.find(">=") == std::string::npos) {
    auto pos               = expr_str.find(">");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 1);
    expr.op                = runtime::ConditionOperator::GREATER_THAN;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find("<") != std::string::npos && expr_str.find("<=") == std::string::npos) {
    auto pos               = expr_str.find("<");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 1);
    expr.op                = runtime::ConditionOperator::LESS_THAN;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find("contains") != std::string::npos) {
    auto pos               = expr_str.find("contains");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 8);
    expr.op                = runtime::ConditionOperator::CONTAINS;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else {
    // Default: treat as equality
    expr.left_operand      = expr_str;
    expr.right_operand     = "";
    expr.op                = runtime::ConditionOperator::EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = false;
  }

  // Trim whitespace
  expr.left_operand.erase(0, expr.left_operand.find_first_not_of(" \t"));
  expr.left_operand.erase(expr.left_operand.find_last_not_of(" \t") + 1);
  expr.right_operand.erase(0, expr.right_operand.find_first_not_of(" \t"));
  expr.right_operand.erase(expr.right_operand.find_last_not_of(" \t") + 1);

  condition_cache_[expr_str] = expr;
  return expr;
}

}  // namespace framework
}  // namespace agentflow
