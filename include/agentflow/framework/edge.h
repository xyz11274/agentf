#ifndef AGENTFLOW_FRAMEWORK_EDGE_H
#define AGENTFLOW_FRAMEWORK_EDGE_H

#include "agentflow/common.h"
#include <vector>

namespace agentflow {
namespace framework {

/**
 * Edge defines the dependency relationship between nodes
 * Supports data passing and control flow events
 */
class Edge {
public:
    enum class DataTransferMode {
        SHARED_MEMORY = 0,
        MESSAGE_QUEUE = 1,
        REMOTE_TRANSFER = 2,
    };
    
    struct Config {
        EdgeId id;
        NodeId source_node;
        NodeId target_node;
        
        // Dependency relationships
        std::vector<std::string> source_outputs;
        std::vector<std::string> target_inputs;
        
        // Data transfer configuration
        DataTransferMode transfer_mode = DataTransferMode::SHARED_MEMORY;
        std::vector<std::string> data_formats;  // text, binary, audio, video, etc.
        
        // Control flow events
        std::vector<std::string> supported_events;  // start, end, fail, data_ready, etc.
    };
    
    explicit Edge(const Config& config);
    ~Edge() = default;
    
    // Getters
    const EdgeId& get_id() const { return config_.id; }
    const NodeId& get_source() const { return config_.source_node; }
    const NodeId& get_target() const { return config_.target_node; }
    DataTransferMode get_transfer_mode() const { return config_.transfer_mode; }
    
    // Data passing
    bool transfer_data(const DataMap& source_data, DataMap& target_data);
    
    // Event management
    bool trigger_event(const std::string& event_type);
    
private:
    Config config_;
};

} // namespace framework
} // namespace agentflow

#endif // AGENTFLOW_FRAMEWORK_EDGE_H
