#ifndef AGENTFLOW_FRAMEWORK_NODE_H
#define AGENTFLOW_FRAMEWORK_NODE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent/common.h"
#include "runtime/exe_loader.h"
#include "runtime/databus/databus.h"
#include "agentflow/so_node.h"
#include "utils/thread_pool.h"

namespace agentflow {
namespace framework {


/**
 * MMNode represents a task execution unit in a workflow graph.
 * Each node has a control plane and a running plane
 */
class MMNode {
public:
  enum class FieldType {
    STRING    = 0,
    INT       = 1,
    FLOAT     = 2,
    BOOL      = 3,
    LIST      = 4,
    MAP       = 5,
    FILE      = 6,
    FILE_LIST = 7,
  };

  enum class TransferMode {
    VALUE     = 0,
    REFERENCE = 1,
  };

  enum class ExecutionRiskLevel {
    GREEN  = 0,
    YELLOW = 1,
    RED    = 2,
  };

  struct FieldDefinition {
    std::string  name;
    FieldType    type          = FieldType::STRING;
    TransferMode transfer_mode = TransferMode::VALUE;
    bool         required      = false;
  };

  enum class ExecutionMode {
    CPP_FUNCTION,   // Local C++ function execution
    SHELL_SCRIPT,   // Shell script execution
    EXECUTABLE,     // External program execution
    GO_SCRIPT,      // Go script execution
    PYTHON_SCRIPT,  // Python script execution
    JAVA_JAR,       // Java JAR execution
    SO_LIBRARY      // Shared object library execution
  };

  struct RuntimeConfig {
    int    message_threads = 1;
    int    event_threads = 1;
    int    executor_threads = 2;
    size_t max_queue_size = 1024;
  };

  struct Config {
    NodeId      id;
    std::string name;
    std::string type;
    int         priority = 0;

    // Input/Output interface definitions
    std::vector<FieldDefinition> input_definitions;
    std::vector<FieldDefinition> output_definitions;

    // Compatibility accessors for legacy code paths.
    std::vector<std::string> input_params;
    std::vector<std::string> output_params;

    // Execution mode configuration
    ExecutionMode execution_mode = ExecutionMode::CPP_FUNCTION;
    std::string   command;      // Command or script path for external execution
    std::string   working_dir;  // Working directory for external execution

    // Error handling policy
    int max_retries = 3;
    int timeout_ms  = 0;

    // Resource limits
    int memory_limit_mb = 0;
    int cpu_limit       = 0;

    // Retry configuration
    int retry_delay_ms = 1000;  // Delay between retries

    // Hot reload configuration (worker-side; enabled by default for executable nodes)
    bool                     enable_hot_reload = true;  // Legacy compatibility flag
    runtime::HotReloadConfig hot_reload_config;         // Hot reload settings

    // Code execution configuration
    std::string              function_name;  // Function name for SO library execution
    std::string              main_class;     // Main class for JAR execution
    std::vector<std::string> args;           // Additional arguments for script execution
    ExecutionRiskLevel       execution_risk_level = ExecutionRiskLevel::GREEN;

    // C++ function callback for CPP_FUNCTION execution mode
    std::function<bool(const DataMap& inputs, DataMap& outputs)> cpp_function;

    // If true, stdout from script execution is parsed as a JSON object and each
    // top-level key is stored in outputs_ DataMap (instead of a single "stdout" key).
    // The framework also injects AF_OUTPUT_FORMAT=json into the subprocess environment.
    bool json_output = false;

    // Optional per-node runtime configuration. When set, MMWorkflow::start_runtime()
    // will automatically call node->start_runtime(node_runtime_config) for this node.
    bool          auto_start_runtime = false;
    RuntimeConfig node_runtime_config;

    // MMWorkflow reference for nested workflow node (type == "workflow")
    std::string workflow_ref;
    std::string node_context_workflow;
  };

  enum class MessagePriority {
    NORMAL = 0,
    HIGH   = 1,
  };

  struct Message {
    std::string     message_id;
    DataMap         inputs;
    MessagePriority priority = MessagePriority::NORMAL;
  };

  struct NodeEvent {
    std::string                        type;
    std::map<std::string, std::string> data;
  };

  using NodeEventHandler = std::function<void(const NodeEvent&)>;

  explicit MMNode(const Config& config);
  ~MMNode();

  // Create a deep copy of this node with fresh state (READY)
  // Optionally inherit databus and databus_client from source
  std::shared_ptr<MMNode> clone(bool inherit_databus = false) const;

  // Getters
  const NodeId&      get_id() const { return config_.id; }
  const std::string& get_name() const { return config_.name; }
  const std::string& get_type() const { return config_.type; }
  int                get_priority() const { return config_.priority; }
  NodeState          get_state() const { return state_; }
  const DataMap&     get_inputs() const { return inputs_; }
  const DataMap&     get_outputs() const { return outputs_; }
  const Config&      get_config() const { return config_; }

  // Config getters for storage
  const std::vector<FieldDefinition>& get_input_definitions() const {
    return config_.input_definitions;
  }
  const std::vector<FieldDefinition>& get_output_definitions() const {
    return config_.output_definitions;
  }
  const std::vector<std::string>& get_input_params() const { return config_.input_params; }
  const std::vector<std::string>& get_output_params() const { return config_.output_params; }
  ExecutionMode                   get_execution_mode() const { return config_.execution_mode; }
  const std::string&              get_command() const { return config_.command; }
  int                             get_timeout_ms() const { return config_.timeout_ms; }
  int                             get_max_retries() const { return config_.max_retries; }
  ExecutionRiskLevel              get_execution_risk_level() const {
    return config_.execution_risk_level;
  }

  // Retry counter
  int  get_retry_count() const { return retry_count_; }
  void reset_retry_count() { retry_count_ = 0; }

  // State management
  void set_state(NodeState state) { state_ = state; }
  void set_inputs(const DataMap& inputs) { inputs_ = inputs; }
  void set_outputs(const DataMap& outputs) { outputs_ = outputs; }

  // Control plane operations
  bool initialize();
  bool execute();
  bool cleanup();

  // Hot reload operations
  bool                                        enable_code_reload();
  bool                                        disable_code_reload();
  bool                                        reload_code();
  bool                                        check_and_reload_code();
  std::shared_ptr<runtime::ExecutableVersion> get_code_version() const;
  bool                                        rollback_to_version(const std::string& version_id);

  // Code loader access
  runtime::ExecutionResult execute_with_code_loader();

  // DataBus injection for SO library V2 signature
  void set_databus(runtime::MMDataBus* databus) { databus_ = databus; }
  runtime::MMDataBus* get_databus() const { return databus_; }

  // Session-isolated DataBus client for V3 signature
  void set_databus_client(std::shared_ptr<INodeDataBusClient> client) { databus_client_ = std::move(client); }
  INodeDataBusClient* get_databus_client() const { return databus_client_.get(); }

  // MMNode runtime queue + event loop
  bool start_runtime();
  bool start_runtime(const RuntimeConfig& config);
  void stop_runtime();
  bool is_runtime_running() const { return runtime_running_; }

  bool enqueue_message(const Message& message);
  bool enqueue_event(const NodeEvent& event);
  void register_event_handler(const std::string& event_type, NodeEventHandler handler);
  void bind_context_workflow(const std::string& workflow_file);

private:
  void message_dispatch_loop();
  void event_dispatch_loop();
  void process_message(const Message& message);

  Config    config_;
  NodeState state_ = NodeState::WAITING;
  DataMap   inputs_;
  DataMap   outputs_;
  int       retry_count_ = 0;

  // Execution result tracking
  std::string last_error_;

  // Hot reload state
  bool        hot_reload_enabled_ = false;
  std::string current_version_id_;

  // Code loader reference
  runtime::CodeLoader* code_loader_ = nullptr;

  RuntimeConfig runtime_config_;
  std::atomic<bool> runtime_running_{false};
  std::deque<Message> high_priority_messages_;
  std::deque<Message> normal_priority_messages_;
  std::queue<NodeEvent> event_queue_;
  std::mutex             queue_mutex_;
  std::condition_variable message_cv_;
  std::condition_variable event_cv_;
  std::thread             message_thread_;
  std::thread             event_thread_;
  std::mutex              execution_mutex_;
  std::unique_ptr<basic::ThreadPool> executor_pool_;
  std::unique_ptr<basic::ThreadPool> event_pool_;
  std::unordered_map<std::string, std::vector<NodeEventHandler>> event_handlers_;
  std::mutex event_handler_mutex_;

  // Injected DataBus for SO library V2 signature
  runtime::MMDataBus* databus_ = nullptr;

  // Session-isolated DataBus client for V3 signature
  std::shared_ptr<INodeDataBusClient> databus_client_;

  // Helper methods for execution
  bool execute_cpp_function();
  bool execute_shell_command();
  bool execute_external_program();
  bool execute_with_worker_engine();
  bool execute_with_timeout(int timeout_ms, std::function<bool()> executor);
  bool execute_so_library();
  bool execute_jar_file();
  bool execute_script(runtime::ExecutableType type);

  // Hot reload helper methods
  void setup_hot_reload();
  void cleanup_hot_reload();
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_FRAMEWORK_NODE_H
