// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Remote data bus implementation for cross-node data sharing

#ifndef AGENTFLOW_RUNTIME_REMOTE_DATABUS_H
#define AGENTFLOW_RUNTIME_REMOTE_DATABUS_H

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <mutex>

#include "agent/common.h"

namespace agentflow {
namespace runtime {

class RemoteDatabus {
public:
  using DataChangeCallback = std::function<void(const std::string&, const DataVariant&)>;

  RemoteDatabus();
  ~RemoteDatabus();

  bool connect(const std::string& address);
  void disconnect();
  bool is_connected() const;

  bool publish(const std::string& key, const DataVariant& value);
  bool fetch(const std::string& key, DataVariant& value) const;
  bool remove(const std::string& key);
  void subscribe(const std::string& key, DataChangeCallback callback);
  void unsubscribe(const std::string& key);
  bool index_data_ref(const std::string& owner_index_key, const std::string& data_ref);
  std::vector<std::string> find_indexed_refs(const std::string& owner_index_key) const;

private:
  bool connected_ = false;
  std::string address_;
  std::map<std::string, DataVariant> data_store_;
  std::map<std::string, std::set<std::string>> owner_index_;
  std::map<std::string, DataChangeCallback> subscriptions_;
  mutable std::mutex mutex_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_REMOTE_DATABUS_H
