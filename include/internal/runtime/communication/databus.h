#ifndef AGENTFLOW_RUNTIME_DATABUS_H
#define AGENTFLOW_RUNTIME_DATABUS_H

#include <map>
#include <memory>
#include <mutex>

#include "common.h"

namespace agentflow {
namespace runtime {

/**
 * HighSpeedDataBus provides high-performance data sharing between nodes
 * Supports zero-copy data sharing and subscription/publication mechanism
 */
class HighSpeedDataBus {
public:
  using DataChangeCallback = std::function<void(const std::string&, const DataVariant&)>;

  explicit HighSpeedDataBus(size_t capacity = 1024 * 1024);
  ~HighSpeedDataBus() = default;

  // Data operations
  bool put(const std::string& key, const DataVariant& value);
  bool get(const std::string& key, DataVariant& value);
  bool remove(const std::string& key);
  bool contains(const std::string& key) const;

  // Subscription mechanism
  void subscribe(const std::string& key, const DataChangeCallback& callback);
  void unsubscribe(const std::string& key);

  // Statistics
  size_t get_used_capacity() const { return used_capacity_; }
  size_t get_total_capacity() const { return capacity_; }
  int    get_data_count() const { return static_cast<int>(data_.size()); }

private:
  const size_t                                           capacity_;
  mutable std::mutex                                     mutex_;
  std::map<std::string, DataVariant>                     data_;
  std::map<std::string, std::vector<DataChangeCallback>> subscriptions_;
  size_t                                                 used_capacity_ = 0;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_DATABUS_H
