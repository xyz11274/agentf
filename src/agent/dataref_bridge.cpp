#include "agent/dataref_bridge.h"

#include <atomic>
#include <mutex>

#include "runtime/databus/databus.h"
#include "utils/logger.h"

namespace agentflow {
namespace framework {
namespace dataref_bridge {
namespace {

struct DataRefBridgeState {
  std::once_flag init_once;
  std::unique_ptr<runtime::MMDataBus> bus;
  std::string redis_url;
  std::atomic<bool> initialized{false};
};

DataRefBridgeState& state() {
  static DataRefBridgeState s;
  return s;
}

void init_impl(int capacity_mb, const std::string& redis_url) {
  auto& s = state();
  std::call_once(s.init_once, [&]() {
    size_t capacity = static_cast<size_t>(capacity_mb) * 1024 * 1024;
    s.bus = std::make_unique<runtime::MMDataBus>(capacity);
    s.redis_url = redis_url;
    s.initialized.store(true);
    if (!redis_url.empty()) {
      s.bus->connect_remote(redis_url);
    }
    Logger::instance().info_f(
        "[dataref_bridge] Initialized: capacity=%d MB, redis=%s",
        capacity_mb, redis_url.empty() ? "disabled" : redis_url.c_str());
  });
}

runtime::MMDataBus& control_databus() {
  auto& s = state();
  if (!s.initialized.load()) {
    Logger::instance().warn_f(
        "[dataref_bridge] control_databus() called before init() — "
        "using fallback (capacity=512 MB, redis=redis://127.0.0.1:6379). "
        "Call dataref_bridge::init() earlier to honour user configuration.");
    init_impl(512, "redis://127.0.0.1:6379");
  }
  return *s.bus;
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
  copy("dialogue_id", owner.dialogue_id);
  copy("task_id", owner.task_id);
  copy("workflow_id", owner.workflow_id);
  copy("node_id", owner.node_id);
  copy("master_id", owner.master_id);
  copy("worker_id", owner.worker_id);
  return owner;
}
}  // namespace

void init(int capacity_mb, const std::string& redis_url) {
  init_impl(capacity_mb, redis_url);
}

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

int cleanup_dialogue(const std::string& dialogue_id) {
  if(dialogue_id.empty()) return 0;

  auto& bus = control_databus();

  // 依赖 remote redis owner index 做按 dialogue_id 的精准清理。
  // 无 remote 时不泄露（数据仍可被 databus 生命周期管理回收），但无法按对话维度清理。
  if(!bus.remote() || !bus.remote()->is_connected()) {
    Logger::instance().warn_f(
        "[dataref_bridge] cleanup_dialogue: remote bus not connected — "
        "cannot enumerate DataRefs for dialogue=%s. "
        "Data will remain in local bus until process restart.",
        dialogue_id.c_str());
    return -1;
  }

  auto refs = bus.find_refs_by_owner("dialogue_id", dialogue_id);
  if(refs.empty()) {
    Logger::instance().debug_f(
        "[dataref_bridge] cleanup_dialogue: no DataRefs for dialogue=%s",
        dialogue_id.c_str());
    return 0;
  }

  int cleared = 0;
  const std::string addr_prefix = "databus://global/";
  for(const auto& ref : refs) {
    if(ref.rfind(addr_prefix, 0) != 0) continue;

    std::string data_id = ref.substr(addr_prefix.size());
    std::string data_key = "data_ref:" + data_id;

    // bus.remove() 同时清理本地共享内存与 remote redis
    if(bus.remove(data_key)) {
      ++cleared;
    }

    // 通过反向映射找出 logical key 份，一并清理
    std::string logical_key_str;
    {
      DataVariant lk_val;
      if(bus.get("data_ref_logical:" + data_id, lk_val)) {
        if(const auto* lk = std::get_if<std::string>(&lk_val)) {
          logical_key_str = *lk;
        }
      }
    }
    if(!logical_key_str.empty()) {
      bus.remove(logical_key_str);
    }
    // 清除反向映射本身
    bus.remove("data_ref_logical:" + data_id);
  }

  bus.clear_owner_index("dialogue_id", dialogue_id);

  Logger::instance().info_f(
      "[dataref_bridge] cleanup_dialogue: dialogue=%s, found=%zu, cleared=%d",
      dialogue_id.c_str(), refs.size(), cleared);

  return cleared;
}

}  // namespace dataref_bridge
}  // namespace framework
}  // namespace agentflow
