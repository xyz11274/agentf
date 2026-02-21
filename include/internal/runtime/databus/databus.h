#ifndef AGENTFLOW_RUNTIME_DATABUS_H
#define AGENTFLOW_RUNTIME_DATABUS_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/common.h"
#include "runtime/databus/local_databus.h"
#include "runtime/databus/remote_databus.h"

namespace agentflow {
namespace runtime {

/**
 * DataBus provides unified data sharing interface for both local and remote communication.
 * It encapsulates LocalDatabus (intra-node) and RemoteDatabus (cross-node).
 */
class DataBus {
public:
  using DataChangeCallback = std::function<void(const std::string&, const DataVariant&)>;

  explicit DataBus(size_t capacity = 1024 * 1024);
  ~DataBus() = default;

  // Data operations (delegates to shared memory / remote bus)
  bool put(const std::string& key, const DataVariant& value);
  bool get(const std::string& key, DataVariant& value) const;
  bool remove(const std::string& key);
  bool contains(const std::string& key) const;

  // Subscription mechanism
  void subscribe(const std::string& key, const DataChangeCallback& callback);
  void unsubscribe(const std::string& key);

  // Connection management for remote bus
  bool connect_remote(const std::string& address);
  void disconnect_remote();

  // Access underlying components
  std::shared_ptr<LocalDatabus>  local() const { return local_bus_; }
  std::shared_ptr<RemoteDatabus> remote() const { return remote_bus_; }

  // Statistics
  size_t get_used_capacity() const;
  size_t get_total_capacity() const;
  int    get_data_count() const;

private:
  std::shared_ptr<LocalDatabus>  local_bus_;
  std::shared_ptr<RemoteDatabus> remote_bus_;
  mutable std::mutex             mutex_;
};

// Alias for backward compatibility if needed, or just replace usages
using HighSpeedDataBus = DataBus;

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_DATABUS_H
