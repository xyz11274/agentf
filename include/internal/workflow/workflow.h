#ifndef AGENTFLOW_FRAMEWORK_WORKFLOW_H
#define AGENTFLOW_FRAMEWORK_WORKFLOW_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent/common.h"
#include "utils/thread_pool.h"
#include "workflow/edge.h"
#include "workflow/node.h"

// Forward declaration
namespace agentflow {
namespace utils {
namespace storage {
struct StorageConfig;
struct StorageResult;
struct VersionInfo;
struct StorageStats;
}  // namespace storage
}  // namespace utils
}  // namespace agentflow

namespace agentflow {
namespace framework {

// ============================================================================
// Control Flow Types
// ============================================================================

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
  ControlContext() = default;
  ~ControlContext() = default;

  bool get_variable(const std::string& name, std::string& value) const;
  void set_variable(const std::string& name, const std::string& value);
  bool has_variable(const std::string& name) const;
  void clear_variables();

  int  get_loop_iteration(const std::string& loop_id) const;
  void increment_loop_iteration(const std::string& loop_id);
  void reset_loop_iteration(const std::string& loop_id);
  bool should_continue_loop(const std::string& loop_id, const LoopConfig& config);

  bool get_loop_variable(const std::string& loop_id, const std::string& var_name,
                         std::string& value) const;
  void set_loop_variable(const std::string& loop_id, const std::string& var_name,
                         const std::string& value);

  void                     enter_loop(const std::string& loop_id, const LoopConfig& config);
  void                     exit_loop(const std::string& loop_id);
  bool                     is_in_loop(const std::string& loop_id) const;
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

struct WorkflowMetadata {
  std::string                        id;
  std::string                        name;
  std::string                        version;
  std::string                        description;
  std::string                        dsl_file;
  int64_t                            created_at;
  int64_t                            updated_at;
  std::string                        author;
  std::map<std::string, std::string> tags;
};

/**
 * MMWorkflow represents a directed workflow graph of nodes and edges.
 * Supports DSL-based orchestration, dynamic loading, and controlled cycles via edge control.
 */
class MMWorkflow {
public:
  struct Config {
    WorkflowId  id;
    std::string name;
    std::string version = "1.0";
    std::string dsl_file;  // Path to DSL definition file

    // MMWorkflow metadata
    std::string                        description;
    std::map<std::string, std::string> metadata;
    bool                               has_red_light_node = false;
  };

  enum class MessagePriority {
    NORMAL = 0,
    HIGH   = 1,
  };

  struct Message {
    std::string     message_id;
    DataMap         payload;
    MessagePriority priority = MessagePriority::NORMAL;
  };

  struct WorkflowEvent {
    std::string                        type;
    std::map<std::string, std::string> data;
  };

  struct RuntimeConfig {
    int    message_threads = 1;
    int    event_threads = 1;
    int    node_executor_threads = 4;
    size_t max_queue_size = 1024;
  };

  using WorkflowEventHandler = std::function<void(const WorkflowEvent&)>;

  explicit MMWorkflow(const Config& config);
  ~MMWorkflow();

  // Create a deep copy of this workflow with fresh node states
  // Each execution should use a cloned workflow to avoid state conflicts
  std::shared_ptr<MMWorkflow> clone() const;

  // Getters
  const WorkflowId&  get_id() const { return config_.id; }
  const std::string& get_name() const { return config_.name; }
  const Config&      get_config() const { return config_; }
  WorkflowState      get_state() const { return state_; }
  bool               is_red_light() const { return config_.has_red_light_node; }

  // MMNode and edge management
  void                                      add_node(std::shared_ptr<MMNode> node);
  void                                      remove_node(const NodeId& node_id);
  std::shared_ptr<MMNode>                     get_node(const NodeId& node_id);
  const std::vector<std::shared_ptr<MMNode>>& get_nodes() const { return nodes_; }

  void                                      add_edge(std::shared_ptr<MMEdge> edge);
  void                                      remove_edge(const EdgeId& edge_id);
  const std::vector<std::shared_ptr<MMEdge>>& get_edges() const { return edges_; }

  // MMWorkflow operations
  bool load_from_dsl(const std::string& dsl_content);
  bool validate();
  bool update_dynamic(const std::string& dsl_content);

  // Storage operations
  agentflow::utils::storage::StorageResult save(const agentflow::utils::storage::StorageConfig& config,
                                         const agentflow::utils::storage::VersionInfo&   version_info);

  static agentflow::utils::storage::StorageResult load(const std::string&                       workflow_id,
                                                const agentflow::utils::storage::StorageConfig& storage_config,
                                                std::shared_ptr<MMWorkflow>&               workflow);

  static agentflow::utils::storage::StorageResult delete_workflow(
      const std::string&                       workflow_id,
      const agentflow::utils::storage::StorageConfig& storage_config);

  static agentflow::utils::storage::StorageResult list_workflows(
      const agentflow::utils::storage::StorageConfig& storage_config,
      std::vector<std::string>&                workflow_ids);

  static agentflow::utils::storage::StorageResult get_metadata(
      const std::string&                       workflow_id,
      const agentflow::utils::storage::StorageConfig& storage_config,
      WorkflowMetadata&                        metadata);

  // Graph operations with control flow support
  std::vector<NodeId> get_topological_order();
  std::vector<NodeId> get_executable_nodes();               // Nodes ready to execute
  std::vector<NodeId> get_executable_nodes_with_control();  // With condition evaluation

  // Control flow integration
  ControlContext& get_control_context() { return control_context_; }

  // Execution state management with control flow
  void   mark_node_executed(const NodeId& node_id, bool success, const std::string& error_msg = "");
  NodeId get_next_node_after_execution(const NodeId& current_node, bool success,
                                       const std::string& error_msg = "");

  // Loop management
  void enter_loop(const std::string& loop_id, const LoopConfig& config);
  void exit_loop_iteration(const std::string& loop_id);
  bool should_continue_loop(const std::string& loop_id);

  // Error handler management
  void set_error_handler(const NodeId& node_id, const ErrorHandlerConfig& handler);

  // Reset workflow execution state
  void reset_execution_state();

  // Runtime execution model
  bool start_runtime();
  bool start_runtime(const RuntimeConfig& config);
  void stop_runtime();
  bool is_runtime_running() const { return runtime_running_; }

  bool enqueue_message(const Message& message);
  bool enqueue_event(const WorkflowEvent& event);
  void register_event_handler(const std::string& event_type, WorkflowEventHandler handler);

private:
  void                     message_dispatch_loop();
  void                     event_dispatch_loop();
  std::vector<std::string> get_start_nodes() const;
  bool                     execute_message_through_workflow(const Message& message);

  Config                                  config_;
  WorkflowState                           state_ = WorkflowState::CREATED;
  std::map<NodeId, std::shared_ptr<MMNode>> node_map_;
  std::vector<std::shared_ptr<MMNode>>      nodes_;
  std::vector<std::shared_ptr<MMEdge>>      edges_;

  // Control flow context
  ControlContext                       control_context_;
  std::map<NodeId, ErrorHandlerConfig> error_handlers_;
  std::map<std::string, LoopConfig>    active_loops_;

  // Execution tracking
  std::map<NodeId, bool> executed_nodes_;
  std::map<NodeId, int>  node_retry_counts_;

  // Graph validation and computation
  bool validate_dag();
  bool compute_topological_order();

  // Condition expression parsing (with cache)
  ConditionExpression parse_condition_expression(const std::string& expr_str);
  std::unordered_map<std::string, ConditionExpression> condition_cache_;

  // Cached topological order (computed once after nodes/edges are finalized)
  std::vector<NodeId> topological_order_;
  bool                topological_order_valid_ = false;

  RuntimeConfig runtime_config_;
  std::atomic<bool> runtime_running_{false};
  std::deque<Message> high_priority_messages_;
  std::deque<Message> normal_priority_messages_;
  std::queue<WorkflowEvent> event_queue_;
  std::mutex                message_queue_mutex_;
  std::mutex                event_queue_mutex_;
  std::condition_variable   message_cv_;
  std::condition_variable   event_cv_;
  std::thread               message_thread_;
  std::thread               event_thread_;
  std::unique_ptr<basic::ThreadPool> message_worker_pool_;
  std::unique_ptr<basic::ThreadPool> event_worker_pool_;
  std::unique_ptr<basic::ThreadPool> node_executor_pool_;
  std::unordered_map<std::string, std::vector<WorkflowEventHandler>> event_handlers_;
  std::mutex                                                     event_handler_mutex_;

  // Condition evaluation
  bool evaluate_condition(const ConditionExpression& expr, const ControlContext& context,
                          const DataMap& node_outputs) const;
  bool evaluate_comparison(const std::string& left, ConditionOperator op,
                           const std::string& right) const;
  std::string resolve_value(const std::string& name, const ControlContext& context,
                            const DataMap& node_outputs) const;

  ControlAction determine_next_action(const NodeId& current_node, NodeState execution_state,
                                      const std::string& error_message);
  ControlAction handle_error(const NodeId& failed_node, const std::string& error_type,
                             const std::string& error_message);
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_FRAMEWORK_WORKFLOW_H
