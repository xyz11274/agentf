// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "runtime/databus/databus.h"

#include "agent/data_variant_utils.h"

#include <chrono>
#include <sstream>
#include <type_traits>

namespace agentflow {
namespace runtime {
namespace {
void add_owner_index(RemoteDatabus* remote, const std::string& owner_key, const std::string& owner_value,
                     const std::string& ref) {
  if(remote == nullptr || owner_value.empty()) {
    return;
  }
  remote->index_data_ref(owner_key + ":" + owner_value, ref);
}
}  // namespace

MMDataBus::MMDataBus(size_t capacity) {
  local_bus_  = std::make_shared<LocalDatabus>();
  remote_bus_ = std::make_shared<RemoteDatabus>();
  (void)capacity;
}

bool MMDataBus::put(const std::string& key, const DataVariant& value) {
  // Put into local shared memory
  bool local_success = local_bus_->get_shared_memory()->put(key, value);

  // If connected, put into remote bus for global synchronization
  if(remote_bus_->is_connected()) {
    remote_bus_->publish(key, value);
  }

  return local_success;
}

bool MMDataBus::get(const std::string& key, DataVariant& value) const {
  // Try local cache first.
  if(local_bus_->get_shared_memory()->get(key, value)) {
    return true;
  }

  // Local miss, auto pull from data service and backfill local cache.
  if(remote_bus_->is_connected() && remote_bus_->fetch(key, value)) {
    local_bus_->get_shared_memory()->put(key, value);
    return true;
  }

  return false;
}

bool MMDataBus::remove(const std::string& key) {
  const bool local_removed = local_bus_->get_shared_memory()->remove(key);
  if(remote_bus_->is_connected()) {
    remote_bus_->remove(key);
  }
  return local_removed;
}

bool MMDataBus::contains(const std::string& key) const {
  return local_bus_->get_shared_memory()->contains(key);
}

int MMDataBus::remove_by_prefix(const std::string& prefix) {
  return local_bus_->get_shared_memory()->remove_by_prefix(prefix);
}

std::vector<std::string> MMDataBus::list_keys_by_prefix(const std::string& prefix) const {
  return local_bus_->get_shared_memory()->list_keys_by_prefix(prefix);
}

void MMDataBus::subscribe(const std::string& key, const DataChangeCallback& callback) {
  local_bus_->get_shared_memory()->subscribe(key, callback);

  if(remote_bus_->is_connected()) {
    remote_bus_->subscribe(key, callback);
  }
}

void MMDataBus::unsubscribe(const std::string& key) {
  local_bus_->get_shared_memory()->unsubscribe(key);

  if(remote_bus_->is_connected()) {
    remote_bus_->unsubscribe(key);
  }
}

bool MMDataBus::store_with_ref(const std::string& key, const DataVariant& value, const OwnerContext& owner,
                             DataRef& out_ref) {
  const std::string data_id = build_data_id(value);
  out_ref.data_id           = data_id;
  out_ref.address           = build_address(data_id);
  out_ref.checksum          = data_id;

  // Cache ref by key for quick lookups.
  key_to_ref_[key] = out_ref;

  // Store payload in local cache by both logical key and data_id address key.
  const std::string data_key = "data_ref:" + data_id;
  if(!put(data_key, value)) {
    return false;
  }
  put(key, value);

  if(remote_bus_->is_connected()) {
    const std::string ref_address = out_ref.address;
    add_owner_index(remote_bus_.get(), "request_id", owner.request_id, ref_address);
    add_owner_index(remote_bus_.get(), "user_id", owner.user_id, ref_address);
    add_owner_index(remote_bus_.get(), "client_id", owner.client_id, ref_address);
    add_owner_index(remote_bus_.get(), "agent_id", owner.agent_id, ref_address);
    add_owner_index(remote_bus_.get(), "session_id", owner.session_id, ref_address);
    add_owner_index(remote_bus_.get(), "task_id", owner.task_id, ref_address);
    add_owner_index(remote_bus_.get(), "workflow_id", owner.workflow_id, ref_address);
    add_owner_index(remote_bus_.get(), "node_id", owner.node_id, ref_address);
    add_owner_index(remote_bus_.get(), "master_id", owner.master_id, ref_address);
    add_owner_index(remote_bus_.get(), "worker_id", owner.worker_id, ref_address);
  }

  return true;
}

bool MMDataBus::resolve_ref(const std::string& data_ref, DataVariant& value) const {
  // data_ref format: databus://global/<data_id>
  const std::string prefix = "databus://global/";
  if(data_ref.rfind(prefix, 0) != 0) {
    return false;
  }
  const std::string data_id  = data_ref.substr(prefix.size());
  const std::string data_key = "data_ref:" + data_id;
  return get(data_key, value);
}

std::vector<std::string> MMDataBus::find_refs_by_owner(const std::string& owner_key,
                                                     const std::string& owner_value) const {
  if(owner_key.empty() || owner_value.empty() || !remote_bus_->is_connected()) {
    return {};
  }
  return remote_bus_->find_indexed_refs(owner_index_key(owner_key, owner_value));
}

bool MMDataBus::connect_remote(const std::string& address) {
  return remote_bus_->connect(address);
}

void MMDataBus::disconnect_remote() {
  remote_bus_->disconnect();
}

size_t MMDataBus::get_used_capacity() const {
  return local_bus_->get_shared_memory()->get_used_capacity();
}

size_t MMDataBus::get_total_capacity() const {
  return local_bus_->get_shared_memory()->get_total_capacity();
}

int MMDataBus::get_data_count() const {
  return local_bus_->get_shared_memory()->get_data_count();
}

std::string MMDataBus::build_data_id(const DataVariant& value) {
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  std::hash<std::string> hasher;
  const auto digest = hasher(data_variant_to_string(value));
  std::ostringstream oss;
  oss << std::hex << digest << "-" << now_ns;
  return oss.str();
}

std::string MMDataBus::build_address(const std::string& data_id) {
  return std::string("databus://global/") + data_id;
}

std::string MMDataBus::owner_index_key(const std::string& owner_key, const std::string& owner_value) {
  return owner_key + ":" + owner_value;
}

}  // namespace runtime
}  // namespace agentflow
