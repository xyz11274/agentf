#include "agentflow/framework/workflow.h"
#include "agentflow/basic/logger.h"
#include <algorithm>
#include <unordered_set>
#include <queue>

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
    // If cached order is valid, return it
    if (topological_order_valid_) {
        return topological_order_;
    }

    // Compute topological order using Kahn's algorithm
    compute_topological_order();

    return topological_order_;
}

std::vector<NodeId> Workflow::get_executable_nodes() {
    std::vector<NodeId> executable;

    for (const auto& node : nodes_) {
        // Check if all dependencies are satisfied
        bool all_deps_ready = true;
        int in_edge_count = 0;

        for (const auto& edge : edges_) {
            if (edge->get_target() == node->get_id()) {
                in_edge_count++;
                // Check if source node is done
                auto source = get_node(edge->get_source());
                if (source && source->get_state() != NodeState::SUCCESS) {
                    all_deps_ready = false;
                    break;
                }
            }
        }

        // Nodes with no incoming edges (sources) are always ready to execute
        // if they are in READY state
        if (in_edge_count == 0) {
            all_deps_ready = true;
        }

        if (all_deps_ready && node->get_state() == NodeState::READY) {
            executable.push_back(node->get_id());
        }
    }

    return executable;
}

bool Workflow::validate_dag() {
    // Check for cycles in DAG using DFS coloring
    enum class Color {
        WHITE,  // Not visited
        GRAY,   // Being visited (in recursion stack)
        BLACK    // Visited
    };

    std::map<NodeId, Color> colors;

    // Initialize all nodes as WHITE
    for (const auto& node : nodes_) {
        colors[node->get_id()] = Color::WHITE;
    }

    // Build adjacency list for the graph
    std::map<NodeId, std::vector<NodeId>> adjacency;
    for (const auto& edge : edges_) {
        adjacency[edge->get_source()].push_back(edge->get_target());
    }

    // DFS to detect cycles
    std::function<bool(const NodeId&)> dfs = [&](const NodeId& node_id) -> bool {
        colors[node_id] = Color::GRAY;  // Mark as being visited

        // Visit all neighbors
        if (adjacency.find(node_id) != adjacency.end()) {
            for (const auto& neighbor_id : adjacency[node_id]) {
                if (colors[neighbor_id] == Color::GRAY) {
                    // Back edge found - cycle detected
                    Logger::instance().error_f("Cycle detected in workflow: %s -> %s",
                                            node_id.c_str(), neighbor_id.c_str());
                    return false;
                } else if (colors[neighbor_id] == Color::WHITE) {
                    if (!dfs(neighbor_id)) {
                        return false;
                    }
                }
            }
        }

        colors[node_id] = Color::BLACK;  // Mark as fully visited
        return true;
    };

    // Run DFS from each unvisited node
    for (const auto& node : nodes_) {
        if (colors[node->get_id()] == Color::WHITE) {
            if (!dfs(node->get_id())) {
                return false;
            }
        }
    }

    Logger::instance().info("Workflow DAG validation passed - no cycles detected");
    return true;
}

bool Workflow::compute_topological_order() {
    // Use Kahn's algorithm to compute topological order
    std::map<NodeId, int> in_degree;

    // Initialize in-degree of all nodes
    for (const auto& node : nodes_) {
        in_degree[node->get_id()] = 0;
    }

    // Calculate in-degree for each node
    for (const auto& edge : edges_) {
        in_degree[edge->get_target()]++;
    }

    // Queue for nodes with zero in-degree
    std::queue<NodeId> zero_in_degree;
    for (const auto& node : nodes_) {
        if (in_degree[node->get_id()] == 0) {
            zero_in_degree.push(node->get_id());
        }
    }

    // Build adjacency list
    std::map<NodeId, std::vector<NodeId>> adjacency;
    for (const auto& edge : edges_) {
        adjacency[edge->get_source()].push_back(edge->get_target());
    }

    // Process nodes in topological order
    topological_order_.clear();
    int processed_count = 0;

    while (!zero_in_degree.empty()) {
        NodeId current = zero_in_degree.front();
        zero_in_degree.pop();
        topological_order_.push_back(current);
        processed_count++;

        // Remove all outgoing edges from current node
        if (adjacency.find(current) != adjacency.end()) {
            for (const auto& neighbor : adjacency[current]) {
                in_degree[neighbor]--;
                if (in_degree[neighbor] == 0) {
                    zero_in_degree.push(neighbor);
                }
            }
        }
    }

    // Check if topological sort is possible (no cycle)
    if (processed_count != static_cast<int>(nodes_.size())) {
        Logger::instance().error("Failed to compute topological order - cycle detected");
        topological_order_.clear();
        return false;
    }

    topological_order_valid_ = true;

    Logger::instance().info_f("Computed topological order with %zu nodes",
                            topological_order_.size());

    return true;
}

} // namespace framework
} // namespace agentflow
