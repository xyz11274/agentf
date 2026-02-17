#include "agentflow/runtime/databus.h"

#include <variant>

namespace agentflow {
namespace runtime {

namespace {
// Helper function to get the size of a DataVariant
size_t get_data_variant_size(const DataVariant& value) {
  return std::visit(
      [](auto&& arg) -> size_t {
        using T = std::decay_t<decltype(arg)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return arg.size();
        } else if constexpr(std::is_same_v<T, int64_t>) {
          return sizeof(int64_t);
        } else if constexpr(std::is_same_v<T, double>) {
          return sizeof(double);
        } else if constexpr(std::is_same_v<T, bool>) {
          return sizeof(bool);
        }
        return 0;
      },
      value);
}
}  // namespace

HighSpeedDataBus::HighSpeedDataBus(size_t capacity) : capacity_(capacity), used_capacity_(0) {
}

bool HighSpeedDataBus::put(const std::string& key, const DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if key already exists
  auto it = data_.find(key);
  if(it != data_.end()) {
    // Update existing value
    data_[key] = value;
  } else {
    // Add new key-value pair
    if(used_capacity_ + key.size() + get_data_variant_size(value) > capacity_) {
      return false;  // No space available
    }
    data_[key] = value;
    used_capacity_ += key.size() + get_data_variant_size(value);
  }

  // Trigger subscribed callbacks
  auto sub_it = subscriptions_.find(key);
  if(sub_it != subscriptions_.end()) {
    for(const auto& callback : sub_it->second) {
      callback(key, value);
    }
  }

  return true;
}

bool HighSpeedDataBus::get(const std::string& key, DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = data_.find(key);
  if(it != data_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

bool HighSpeedDataBus::remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = data_.find(key);
  if(it != data_.end()) {
    used_capacity_ -= key.size() + get_data_variant_size(it->second);
    data_.erase(it);
    return true;
  }
  return false;
}

bool HighSpeedDataBus::contains(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return data_.find(key) != data_.end();
}

void HighSpeedDataBus::subscribe(const std::string& key, const DataChangeCallback& callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  subscriptions_[key].push_back(callback);
}

void HighSpeedDataBus::unsubscribe(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        it = subscriptions_.find(key);
  if(it != subscriptions_.end()) {
    subscriptions_.erase(it);
  }
}

}  // namespace runtime
}  // namespace agentflow
