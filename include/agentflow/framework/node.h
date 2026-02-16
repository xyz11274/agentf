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
    struct Config {
        NodeId id;
        std::string name;
        std::string type;
        int priority = 0;
        
        // Input/Output interface definitions
        std::vector<std::string> input_params;
        std::vector<std::string> output_params;
        
        // Error handling policy
        int max_retries = 3;
        int timeout_ms = 0;
        
        // Resource limits
        int memory_limit_mb = 0;
        int cpu_limit = 0;
    };
    
    explicit Node(const Config& config);
    ~Node() = default;
    
    // Getters
    const NodeId& get_id() const { return config_.id; }
    const std::string& get_name() const { return config_.name; }
    NodeState get_state() const { return state_; }
    const DataMap& get_inputs() const { return inputs_; }
    const DataMap& get_outputs() const { return outputs_; }
    
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
};

} // namespace framework
} // namespace agentflow

#endif // AGENTFLOW_FRAMEWORK_NODE_H
