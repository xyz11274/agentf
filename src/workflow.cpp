#include "agentflow/framework/workflow.h"
#include <algorithm>

namespace agentflow {
namespace framework {

Workflow::Workflow(const Config& config)
    : config_(config), state_(WorkflowState::CREATED) {
}

void Workflow::add_node(std::shared_ptr<Node> node) {
    if (node) {
        node_map_[node->get_id()] = node;
        nodes_.push_back(node);
    }
}

void Workflow::remove_node(const NodeId& node_id) {
    auto it = node_map_.find(node_id);
    if (it != node_map_.end()) {
        node_map_.erase(it);
        nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                    [&node_id](const std::shared_ptr<Node>& n) {
                                        return n->get_id() == node_id;
                                    }),
                     nodes_.end());
    }
}

std::shared_ptr<Node> Workflow::get_node(const NodeId& node_id) {
    auto it = node_map_.find(node_id);
    return it != node_map_.end() ? it->second : nullptr;
}

void Workflow::add_edge(std::shared_ptr<Edge> edge) {
    if (edge) {
        edges_.push_back(edge);
    }
}

void Workflow::remove_edge(const EdgeId& edge_id) {
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&edge_id](const std::shared_ptr<Edge>& e) {
                                    return e->get_id() == edge_id;
                                }),
                 edges_.end());
}

bool Workflow::load_from_dsl(const std::string& dsl_content) {
    // Parse DSL and build workflow
    // In real implementation, this would parse JSON or other DSL format
    state_ = WorkflowState::LOADED;
    return true;
}

bool Workflow::validate() {
    if (!validate_dag()) {
        return false;
    }
    state_ = WorkflowState::LOADED;
    return true;
}

bool Workflow::update_dynamic(const std::string& dsl_content) {
    // Dynamically update workflow DSL without recreating instance
    return load_from_dsl(dsl_content);
}

std::vector<NodeId> Workflow::get_topological_order() {
    std::vector<NodeId> result;
    // Implement topological sort based on edges
    // Simplified implementation
    for (const auto& node : nodes_) {
        result.push_back(node->get_id());
    }
    return result;
}

std::vector<NodeId> Workflow::get_executable_nodes() {
    std::vector<NodeId> executable;
    
    for (const auto& node : nodes_) {
        // Check if all dependencies are satisfied
        bool all_deps_ready = true;
        for (const auto& edge : edges_) {
            if (edge->get_target() == node->get_id()) {
                // Check if source node is done
                auto source = get_node(edge->get_source());
                if (source && source->get_state() != NodeState::SUCCESS) {
                    all_deps_ready = false;
                    break;
                }
            }
        }
        
        if (all_deps_ready && node->get_state() == NodeState::READY) {
            executable.push_back(node->get_id());
        }
    }
    
    return executable;
}

bool Workflow::validate_dag() {
    // Check for cycles in DAG
    // Simplified implementation
    return true;
}

bool Workflow::compute_topological_order() {
    // Compute and cache topological order
    return true;
}

} // namespace framework
} // namespace agentflow
