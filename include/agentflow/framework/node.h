#ifndef AGENTFLOW_FRAMEWORK_NODE_H
#define AGENTFLOW_FRAMEWORK_NODE_H

#include "agentflow/common.h"
#include <functional>

namespace agentflow {
namespace framework {

/**
 * Node represents a task execution unit in a workflow DAG
 * Each node has a control plane and a running plane
 */
class Node {
public:
    enum class ExecutionMode {
        CPP_FUNCTION,    // Local C++ function execution
        SHELL_SCRIPT,   // Shell script execution
        EXECUTABLE,     // External program execution
        PYTHON_SCRIPT,   // Python script execution
        SO_LIBRARY       // Shared object library execution
    };

    struct Config {
        NodeId id;
        std::string name;
        std::string type;
        int priority = 0;

        // Input/Output interface definitions
        std::vector<std::string> input_params;
        std::vector<std::string> output_params;

        // Execution mode configuration
        ExecutionMode execution_mode = ExecutionMode::CPP_FUNCTION;
        std::string command;        // Command or script path for external execution
        std::string working_dir;   // Working directory for external execution

        // Error handling policy
        int max_retries = 3;
        int timeout_ms = 0;

        // Resource limits
        int memory_limit_mb = 0;
        int cpu_limit = 0;

        // Retry configuration
        int retry_delay_ms = 1000;  // Delay between retries
    };
    
    explicit Node(const Config& config);
    ~Node() = default;
    
    // Getters
    const NodeId& get_id() const { return config_.id; }
    const std::string& get_name() const { return config_.name; }
    const std::string& get_type() const { return config_.type; }
    int get_priority() const { return config_.priority; }
    NodeState get_state() const { return state_; }
    const DataMap& get_inputs() const { return inputs_; }
    const DataMap& get_outputs() const { return outputs_; }

    // Config getters for storage
    const std::vector<std::string>& get_input_params() const { return config_.input_params; }
    const std::vector<std::string>& get_output_params() const { return config_.output_params; }
    ExecutionMode get_execution_mode() const { return config_.execution_mode; }
    const std::string& get_command() const { return config_.command; }
    int get_timeout_ms() const { return config_.timeout_ms; }
    int get_max_retries() const { return config_.max_retries; }

    // Retry counter
    int get_retry_count() const { return retry_count_; }
    void reset_retry_count() { retry_count_ = 0; }
    
    // State management
    void set_state(NodeState state) { state_ = state; }
    void set_inputs(const DataMap& inputs) { inputs_ = inputs; }
    void set_outputs(const DataMap& outputs) { outputs_ = outputs; }
    
    // Control plane operations
    bool initialize();
    bool execute();
    bool cleanup();
    
private:
    Config config_;
    NodeState state_ = NodeState::WAITING;
    DataMap inputs_;
    DataMap outputs_;
    int retry_count_ = 0;

    // Execution result tracking
    std::string last_error_;

    // Helper methods for execution
    bool execute_cpp_function();
    bool execute_shell_command();
    bool execute_external_program();
    bool execute_with_timeout(int timeout_ms, std::function<bool()> executor);
};

} // namespace framework
} // namespace agentflow

#endif // AGENTFLOW_FRAMEWORK_NODE_H
