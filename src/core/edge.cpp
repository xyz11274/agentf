#include "agentflow/core/edge.h"

#include <algorithm>

namespace agentflow {
namespace framework {

Edge::Edge(const Config& config) : config_(config) {
}

bool Edge::transfer_data(const DataMap& source_data, DataMap& target_data) {
  // Transfer data from source to target node
  // Based on configured data transfer mode

  for(const auto& output : config_.source_outputs) {
    auto it = source_data.find(output);
    if(it != source_data.end()) {
      // Find corresponding input parameter
      if(!config_.target_inputs.empty()) {
        target_data[config_.target_inputs[0]] = it->second;
      }
    }
  }

  return true;
}

bool Edge::trigger_event(const std::string& event_type) {
  // Trigger control flow event
  auto it = std::find(config_.supported_events.begin(), config_.supported_events.end(), event_type);
  return it != config_.supported_events.end();
}

}  // namespace framework
}  // namespace agentflow
