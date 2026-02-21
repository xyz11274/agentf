// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Remote data bus implementation for cross-node data sharing

#ifndef AGENTFLOW_RUNTIME_REMOTE_DATABUS_H
#define AGENTFLOW_RUNTIME_REMOTE_DATABUS_H

#include <functional>
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
  void subscribe(const std::string& key, DataChangeCallback callback);
  void unsubscribe(const std::string& key);

private:
  bool connected_ = false;
  std::string address_;
  mutable std::mutex mutex_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_REMOTE_DATABUS_H
