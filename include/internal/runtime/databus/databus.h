#ifndef AGENTFLOW_RUNTIME_DATABUS_H
#define AGENTFLOW_RUNTIME_DATABUS_H

#include <memory>
#include <map>
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
  struct OwnerContext {
    std::string request_id;
    std::string user_id;
    std::string client_id;
    std::string agent_id;
    std::string session_id;
    std::string task_id;
    std::string workflow_id;
    std::string node_id;
    std::string master_id;
    std::string worker_id;
  };

  struct DataRef {
    std::string data_id;
    std::string address;
    std::string checksum;
  };

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

  // Data-reference operations (payload is in databus cache/service, control plane carries only ref)
  bool store_with_ref(const std::string& key, const DataVariant& value, const OwnerContext& owner,
                      DataRef& out_ref);
  bool resolve_ref(const std::string& data_ref, DataVariant& value) const;
  std::vector<std::string> find_refs_by_owner(const std::string& owner_key,
                                              const std::string& owner_value) const;

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
  std::map<std::string, DataRef> key_to_ref_;

  static std::string build_data_id(const DataVariant& value);
  static std::string build_address(const std::string& data_id);
  static std::string owner_index_key(const std::string& owner_key, const std::string& owner_value);
};

// Alias for backward compatibility if needed, or just replace usages
using HighSpeedDataBus = DataBus;

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_DATABUS_H
