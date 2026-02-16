#ifndef AGENTFLOW_FRAMEWORK_WORKFLOW_H
#define AGENTFLOW_FRAMEWORK_WORKFLOW_H

#include "agentflow/common.h"
#include "agentflow/framework/node.h"
#include "agentflow/framework/edge.h"
#include <map>
#include <vector>

namespace agentflow {
namespace framework {

/**
 * Workflow represents a DAG (Directed Acyclic Graph) of nodes and edges
 * Supports DSL-based orchestration and dynamic loading
 */
class Workflow {
public:
    struct Config {
        WorkflowId id;
        std::string name;
        std::string version = "1.0";
        std::string dsl_file;  // Path to DSL definition file
        
        // Workflow metadata
        std::string description;
        std::map<std::string, std::string> metadata;
    };
    
    explicit Workflow(const Config& config);
    ~Workflow() = default;
    
    // Getters
    const WorkflowId& get_id() const { return config_.id; }
    const std::string& get_name() const { return config_.name; }
    WorkflowState get_state() const { return state_; }
    
    // Node and edge management
    void add_node(std::shared_ptr<Node> node);
    void remove_node(const NodeId& node_id);
    std::shared_ptr<Node> get_node(const NodeId& node_id);
    const std::vector<std::shared_ptr<Node>>& get_nodes() const { return nodes_; }
    
    void add_edge(std::shared_ptr<Edge> edge);
    void remove_edge(const EdgeId& edge_id);
    const std::vector<std::shared_ptr<Edge>>& get_edges() const { return edges_; }
    
    // Workflow operations
    bool load_from_dsl(const std::string& dsl_content);
    bool validate();
    bool update_dynamic(const std::string& dsl_content);
    
    // DAG operations
    std::vector<NodeId> get_topological_order();
    std::vector<NodeId> get_executable_nodes();  // Nodes ready to execute
    
private:
    Config config_;
    WorkflowState state_ = WorkflowState::CREATED;
    std::map<NodeId, std::shared_ptr<Node>> node_map_;
    std::vector<std::shared_ptr<Node>> nodes_;
    std::vector<std::shared_ptr<Edge>> edges_;
    
    // DAG validation and computation
    bool validate_dag();
    bool compute_topological_order();
};

} // namespace framework
} // namespace agentflow

#endif // AGENTFLOW_FRAMEWORK_WORKFLOW_H
