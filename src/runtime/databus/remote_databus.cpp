// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "runtime/databus/remote_databus.h"
#include "utils/logger.h"

namespace agentflow {
namespace runtime {

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
  // Stub connection
  connected_ = true;
  Logger::instance().info("RemoteDatabus connected to " + address);
  return true;
}

void RemoteDatabus::disconnect() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) {
    return;
  }
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
  // Stub publish
  Logger::instance().debug("RemoteDatabus published key: " + key);
  return true;
}

void RemoteDatabus::subscribe(const std::string& key, DataChangeCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) {
    return;
  }
  // Stub subscribe
  Logger::instance().debug("RemoteDatabus subscribed to key: " + key);
}

void RemoteDatabus::unsubscribe(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) {
    return;
  }
  // Stub unsubscribe
  Logger::instance().debug("RemoteDatabus unsubscribed from key: " + key);
}

}  // namespace runtime
}  // namespace agentflow
