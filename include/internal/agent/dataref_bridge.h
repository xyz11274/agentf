#ifndef AGENTFLOW_INTERNAL_AGENT_DATAREF_BRIDGE_H
#define AGENTFLOW_INTERNAL_AGENT_DATAREF_BRIDGE_H

#include <map>
#include <string>
#include <vector>

namespace agentflow {
namespace framework {
namespace dataref_bridge {

std::string store_payload(const std::string& payload,
                          const std::map<std::string, std::string>& metadata,
                          const std::string& logical_key);
bool resolve_payload(const std::string& data_ref, std::string& payload);
std::vector<std::string> find_refs_by_owner(const std::string& owner_key,
                                            const std::string& owner_value);

}  // namespace dataref_bridge
}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_DATAREF_BRIDGE_H
