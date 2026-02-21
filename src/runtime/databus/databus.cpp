// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "runtime/databus/databus.h"

namespace agentflow {
namespace runtime {

DataBus::DataBus(size_t capacity) {
  local_bus_  = std::make_shared<LocalDatabus>();
  remote_bus_ = std::make_shared<RemoteDatabus>();
}

bool DataBus::put(const std::string& key, const DataVariant& value) {
  // Put into local shared memory
  bool local_success = local_bus_->get_shared_memory()->put(key, value);

  // If connected, put into remote bus
  if(remote_bus_->is_connected()) {
    remote_bus_->publish(key, value);
  }

  return local_success;
}

bool DataBus::get(const std::string& key, DataVariant& value) const {
  // Try local first
  if(local_bus_->get_shared_memory()->get(key, value)) {
    return true;
  }

  // Remote get not supported yet
  return false;
}

bool DataBus::remove(const std::string& key) {
  return local_bus_->get_shared_memory()->remove(key);
}

bool DataBus::contains(const std::string& key) const {
  return local_bus_->get_shared_memory()->contains(key);
}

void DataBus::subscribe(const std::string& key, const DataChangeCallback& callback) {
  local_bus_->get_shared_memory()->subscribe(key, callback);

  if(remote_bus_->is_connected()) {
    remote_bus_->subscribe(key, callback);
  }
}

void DataBus::unsubscribe(const std::string& key) {
  local_bus_->get_shared_memory()->unsubscribe(key);

  if(remote_bus_->is_connected()) {
    remote_bus_->unsubscribe(key);
  }
}

bool DataBus::connect_remote(const std::string& address) {
  return remote_bus_->connect(address);
}

void DataBus::disconnect_remote() {
  remote_bus_->disconnect();
}

size_t DataBus::get_used_capacity() const {
  return local_bus_->get_shared_memory()->get_used_capacity();
}

size_t DataBus::get_total_capacity() const {
  return local_bus_->get_shared_memory()->get_total_capacity();
}

int DataBus::get_data_count() const {
  return local_bus_->get_shared_memory()->get_data_count();
}

}  // namespace runtime
}  // namespace agentflow
