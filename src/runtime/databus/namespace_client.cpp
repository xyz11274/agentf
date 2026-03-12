// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// NamespaceClientImpl implementation

#include "runtime/databus/namespace_client.h"

namespace agentflow {
namespace runtime {

NamespaceClientImpl::NamespaceClientImpl(INodeDataBus* databus, const std::string& prefix)
    : databus_(databus), prefix_(prefix) {}

bool NamespaceClientImpl::put(const std::string& key, const SoDataVariant& value) {
  if (!databus_) {
    return false;
  }
  return databus_->put(build_key(key), value);
}

bool NamespaceClientImpl::get(const std::string& key, SoDataVariant& value) const {
  if (!databus_) {
    return false;
  }
  return databus_->get(build_key(key), value);
}

bool NamespaceClientImpl::contains(const std::string& key) const {
  if (!databus_) {
    return false;
  }
  return databus_->contains(build_key(key));
}

bool NamespaceClientImpl::remove(const std::string& key) {
  if (!databus_) {
    return false;
  }
  return databus_->remove(build_key(key));
}

SubscriptionHandle NamespaceClientImpl::subscribe(const std::string& key, SoDataBusCallback callback) {
  if (!databus_ || !callback) {
    return SubscriptionHandle{};
  }
  return databus_->subscribe(build_key(key), std::move(callback));
}

void NamespaceClientImpl::unsubscribe(const SubscriptionHandle& handle) {
  if (!databus_) {
    return;
  }
  databus_->unsubscribe(handle);
}

}  // namespace runtime
}  // namespace agentflow