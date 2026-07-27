// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "runtime/databus/remote_databus.h"

#include "agent/data_variant_utils.h"
#include "utils/logger.h"
#include "vendor/cache/redis_adapter.h"
#include <type_traits>

namespace agentflow {
namespace runtime {
namespace {
agentflow::vendor::adapter::RedisAdapter& redis_adapter_instance() {
  static agentflow::vendor::adapter::RedisAdapter adapter;
  return adapter;
}

}  // namespace

RemoteDatabus::RemoteDatabus() {
}

RemoteDatabus::~RemoteDatabus() {
  disconnect();
}

bool RemoteDatabus::connect(const std::string& address) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_) {
    return true;
  }
  address_ = address;
  if (!redis_adapter_instance().connect(address_)) {
    Logger::instance().error("RemoteDatabus failed to connect to " + address);
    return false;
  }
  connected_ = true;
  Logger::instance().info("RemoteDatabus connected to " + address);
  return true;
}

void RemoteDatabus::disconnect() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) {
    return;
  }
  redis_adapter_instance().disconnect();
  connected_ = false;
  Logger::instance().info("RemoteDatabus disconnected from " + address_);
}

bool RemoteDatabus::is_connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connected_;
}

bool RemoteDatabus::publish(const std::string& key, const DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) {
    return false;
  }
  data_store_[key] = value;
  redis_adapter_instance().set(key, data_variant_to_string(value));
  auto sub_it = subscriptions_.find(key);
  if(sub_it != subscriptions_.end()) {
    sub_it->second(key, value);
  }
  return true;
}

bool RemoteDatabus::fetch(const std::string& key, DataVariant& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = data_store_.find(key);
  if(it == data_store_.end()) {
    return false;
  }
  value = it->second;
  return true;
}

bool RemoteDatabus::remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(!connected_) {
    return false;
  }
  data_store_.erase(key);
  redis_adapter_instance().del(key);
  return true;
}

void RemoteDatabus::subscribe(const std::string& key, DataChangeCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) {
    return;
  }
  subscriptions_[key] = callback;
  Logger::instance().debug("RemoteDatabus subscribed to key: " + key);
}

void RemoteDatabus::unsubscribe(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) {
    return;
  }
  subscriptions_.erase(key);
  Logger::instance().debug("RemoteDatabus unsubscribed from key: " + key);
}

bool RemoteDatabus::index_data_ref(const std::string& owner_index_key, const std::string& data_ref) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(!connected_) {
    return false;
  }
  owner_index_[owner_index_key].insert(data_ref);
  redis_adapter_instance().sadd(owner_index_key, data_ref);
  return true;
}

std::vector<std::string> RemoteDatabus::find_indexed_refs(const std::string& owner_index_key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> refs;
  auto it = owner_index_.find(owner_index_key);
  if(it != owner_index_.end()) {
    for(const auto& ref : it->second) {
      refs.push_back(ref);
    }
  }
  return refs;
}

void RemoteDatabus::clear_owner_index(const std::string& owner_index_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  owner_index_.erase(owner_index_key);
  if(connected_) {
    redis_adapter_instance().del(owner_index_key);
  }
}

}  // namespace runtime
}  // namespace agentflow
