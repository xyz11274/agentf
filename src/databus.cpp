#include "agentflow/runtime/databus.h"

namespace agentflow {
namespace runtime {

HighSpeedDataBus::HighSpeedDataBus(size_t capacity)
    : capacity_(capacity), used_capacity_(0) {
}

bool HighSpeedDataBus::put(const std::string& key, const DataVariant& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if key already exists
    auto it = data_.find(key);
    if (it != data_.end()) {
        // Update existing value
        data_[key] = value;
    } else {
        // Add new key-value pair
        if (used_capacity_ + key.size() + value.size() > capacity_) {
            return false;  // No space available
        }
        data_[key] = value;
        used_capacity_ += key.size() + value.size();
    }
    
    // Trigger subscribed callbacks
    auto sub_it = subscriptions_.find(key);
    if (sub_it != subscriptions_.end()) {
        for (const auto& callback : sub_it->second) {
            callback(key, value);
        }
    }
    
    return true;
}

bool HighSpeedDataBus::get(const std::string& key, DataVariant& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = data_.find(key);
    if (it != data_.end()) {
        value = it->second;
        return true;
    }
    return false;
}

bool HighSpeedDataBus::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = data_.find(key);
    if (it != data_.end()) {
        used_capacity_ -= key.size() + it->second.size();
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
    auto it = subscriptions_.find(key);
    if (it != subscriptions_.end()) {
        subscriptions_.erase(it);
    }
}

} // namespace runtime
} // namespace agentflow
