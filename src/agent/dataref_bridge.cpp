#include "agent/dataref_bridge.h"

#include <mutex>

#include "runtime/databus/databus.h"

namespace agentflow {
namespace framework {
namespace dataref_bridge {
namespace {
runtime::MMDataBus& control_databus() {
  static runtime::MMDataBus bus(4 * 1024 * 1024);
  static std::once_flag once;
  std::call_once(once, [] {
    // Default local redis endpoint; MMDataBus remains usable even if remote is unavailable.
    bus.connect_remote("redis://127.0.0.1:6379");
  });
  return bus;
}

runtime::MMDataBus::OwnerContext build_owner(const std::map<std::string, std::string>& metadata) {
  runtime::MMDataBus::OwnerContext owner;
  const auto copy = [&metadata](const char* key, std::string& target) {
    auto it = metadata.find(key);
    if(it != metadata.end()) {
      target = it->second;
    }
  };
  copy("request_id", owner.request_id);
  copy("user_id", owner.user_id);
  copy("client_id", owner.client_id);
  copy("agent_id", owner.agent_id);
  copy("session_id", owner.session_id);
  copy("task_id", owner.task_id);
  copy("workflow_id", owner.workflow_id);
  copy("node_id", owner.node_id);
  copy("master_id", owner.master_id);
  copy("worker_id", owner.worker_id);
  return owner;
}
}  // namespace

std::string store_payload(const std::string& payload,
                          const std::map<std::string, std::string>& metadata,
                          const std::string& logical_key) {
  if(payload.empty()) {
    return "";
  }
  runtime::MMDataBus::DataRef out;
  if(!control_databus().store_with_ref(logical_key, payload, build_owner(metadata), out)) {
    return "";
  }
  return out.address;
}

bool resolve_payload(const std::string& data_ref, std::string& payload) {
  if(data_ref.empty()) {
    return false;
  }
  agentflow::DataVariant value;
  if(!control_databus().resolve_ref(data_ref, value)) {
    return false;
  }
  if(const auto* text = std::get_if<std::string>(&value)) {
    payload = *text;
    return true;
  }
  return false;
}

std::vector<std::string> find_refs_by_owner(const std::string& owner_key,
                                            const std::string& owner_value) {
  return control_databus().find_refs_by_owner(owner_key, owner_value);
}

}  // namespace dataref_bridge
}  // namespace framework
}  // namespace agentflow
