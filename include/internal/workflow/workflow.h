#ifndef AGENTFLOW_FRAMEWORK_WORKFLOW_H
#define AGENTFLOW_FRAMEWORK_WORKFLOW_H

#include <map>
#include <unordered_map>
#include <vector>

#include "agent/common.h"
#include "workflow/edge.h"
#include "workflow/node.h"
#include "runtime/scheduler/control_flow.h"

namespace agentflow {
namespace framework {

/**
 * Workflow represents a DAG (Directed Acyclic Graph) of nodes and edges
 * Supports DSL-based orchestration and dynamic loading
 */
class Workflow {
public:
  struct Config {
    WorkflowId  id;
    std::string name;
    std::string version = "1.0";
    std::string dsl_file;  // Path to DSL definition file

    // Workflow metadata
    std::string                        description;
    std::map<std::string, std::string> metadata;
  };

  explicit Workflow(const Config& config);
  ~Workflow() = default;

  // Getters
  const WorkflowId&  get_id() const { return config_.id; }
  const std::string& get_name() const { return config_.name; }
  WorkflowState      get_state() const { return state_; }

  // Node and edge management
  void                                      add_node(std::shared_ptr<Node> node);
  void                                      remove_node(const NodeId& node_id);
  std::shared_ptr<Node>                     get_node(const NodeId& node_id);
  const std::vector<std::shared_ptr<Node>>& get_nodes() const { return nodes_; }

  void                                      add_edge(std::shared_ptr<Edge> edge);
  void                                      remove_edge(const EdgeId& edge_id);
  const std::vector<std::shared_ptr<Edge>>& get_edges() const { return edges_; }

  // Workflow operations
  bool load_from_dsl(const std::string& dsl_content);
  bool validate();
  bool update_dynamic(const std::string& dsl_content);

  // DAG operations with control flow support
  std::vector<NodeId> get_topological_order();
  std::vector<NodeId> get_executable_nodes();               // Nodes ready to execute
  std::vector<NodeId> get_executable_nodes_with_control();  // With condition evaluation

  // Control flow integration
  runtime::ControlFlowManager& get_control_flow_manager() { return control_flow_manager_; }
  runtime::ControlContext&     get_control_context() { return control_flow_manager_.get_context(); }

  // Execution state management with control flow
  void   mark_node_executed(const NodeId& node_id, bool success, const std::string& error_msg = "");
  NodeId get_next_node_after_execution(const NodeId& current_node, bool success,
                                       const std::string& error_msg = "");

  // Loop management
  void enter_loop(const std::string& loop_id, const runtime::LoopConfig& config);
  void exit_loop_iteration(const std::string& loop_id);
  bool should_continue_loop(const std::string& loop_id);

  // Error handler management
  void set_error_handler(const NodeId& node_id, const runtime::ErrorHandlerConfig& handler);

  // Reset workflow execution state
  void reset_execution_state();

private:
  Config                                  config_;
  WorkflowState                           state_ = WorkflowState::CREATED;
  std::map<NodeId, std::shared_ptr<Node>> node_map_;
  std::vector<std::shared_ptr<Node>>      nodes_;
  std::vector<std::shared_ptr<Edge>>      edges_;

  // Control flow manager integration
  runtime::ControlFlowManager control_flow_manager_;

  // Execution tracking
  std::map<NodeId, bool> executed_nodes_;
  std::map<NodeId, int>  node_retry_counts_;

  // DAG validation and computation
  bool validate_dag();
  bool compute_topological_order();

  // Condition expression parsing (with cache)
  runtime::ConditionExpression parse_condition_expression(const std::string& expr_str);
  std::unordered_map<std::string, runtime::ConditionExpression> condition_cache_;

  // Cached topological order (computed once after nodes/edges are finalized)
  std::vector<NodeId> topological_order_;
  bool                topological_order_valid_ = false;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_FRAMEWORK_WORKFLOW_H
