// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// NodeDataBusAdapter - Adapts MMDataBus to INodeDataBus interface for SO library nodes

#ifndef AGENTFLOW_RUNTIME_NODE_DATABUS_ADAPTER_H
#define AGENTFLOW_RUNTIME_NODE_DATABUS_ADAPTER_H

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "agentflow/so_node.h"
#include "runtime/databus/databus.h"

namespace agentflow {
namespace runtime {

/**
 * @brief NodeDataBusAdapter wraps MMDataBus to implement INodeDataBus interface.
 *
 * This adapter allows SO library nodes to access the framework's DataBus
 * through a stable ABI interface. The adapter translates between the
 * INodeDataBus virtual interface (using SoDataVariant) and the MMDataBus
 * concrete implementation (using framework DataVariant).
 *
 * Usage:
 * \code
 * MMDataBus databus;
 * NodeDataBusAdapter adapter(&databus);
 *
 * // Pass to SO library node
 * SoNodeFunctionV2 func = (SoNodeFunctionV2)dlsym(handle, "my_node_v2");
 * func(inputs, outputs, &adapter);
 * \endcode
 */
class NodeDataBusAdapter : public INodeDataBus {
public:
  /// Construct adapter wrapping the given MMDataBus.
  /// The databus pointer must remain valid for the lifetime of this adapter.
  explicit NodeDataBusAdapter(MMDataBus* databus) : databus_(databus) {}

  ~NodeDataBusAdapter() override { unsubscribe_all(); }

  // ---- INodeDataBus Interface Implementation ----

  bool put(const std::string& key, const SoDataVariant& value) override {
    if (!databus_) {
      return false;
    }
    return databus_->put(key, to_framework_variant(value));
  }

  bool get(const std::string& key, SoDataVariant& value) const override {
    if (!databus_) {
      return false;
    }
    DataVariant fw_value;
    if (databus_->get(key, fw_value)) {
      value = to_so_variant(fw_value);
      return true;
    }
    return false;
  }

  bool contains(const std::string& key) const override {
    if (!databus_) {
      return false;
    }
    return databus_->contains(key);
  }

  bool remove(const std::string& key) override {
    if (!databus_) {
      return false;
    }
    return databus_->remove(key);
  }

  bool publish(const std::string& topic, const SoDataVariant& value) override {
    if (!databus_) {
      return false;
    }
    return databus_->put(topic, to_framework_variant(value));
  }

  SubscriptionHandle subscribe(const std::string& key, SoDataBusCallback callback) override {
    if (!databus_ || !callback) {
      return SubscriptionHandle{};
    }

    std::lock_guard<std::mutex> lock(subscription_mutex_);

    SubscriptionHandle handle{next_subscription_id_++};

    SubscriptionInfo info;
    info.key = key;
    info.callback = std::move(callback);
    info.is_topic = false;
    subscriptions_[handle.id] = std::move(info);

    // Register with MMDataBus and translate callbacks
    databus_->subscribe(key, [this, handle](const std::string& k, const DataVariant& v) {
      notify_callback(handle, k, to_so_variant(v));
    });

    return handle;
  }

  SubscriptionHandle subscribe_topic(const std::string& topic, SoDataBusCallback callback) override {
    if (!callback) {
      return SubscriptionHandle{};
    }

    std::lock_guard<std::mutex> lock(subscription_mutex_);

    SubscriptionHandle handle{next_subscription_id_++};

    SubscriptionInfo info;
    info.key = topic;
    info.callback = std::move(callback);
    info.is_topic = true;
    subscriptions_[handle.id] = std::move(info);

    return handle;
  }

  void unsubscribe(const SubscriptionHandle& handle) override {
    if (!handle.is_valid()) {
      return;
    }

    std::lock_guard<std::mutex> lock(subscription_mutex_);

    auto it = subscriptions_.find(handle.id);
    if (it != subscriptions_.end()) {
      if (databus_ && !it->second.is_topic) {
        databus_->unsubscribe(it->second.key);
      }
      subscriptions_.erase(it);
    }
  }

  void unsubscribe_all() override {
    std::lock_guard<std::mutex> lock(subscription_mutex_);

    if (databus_) {
      for (const auto& [id, info] : subscriptions_) {
        if (!info.is_topic) {
          databus_->unsubscribe(info.key);
        }
      }
    }
    subscriptions_.clear();
  }

  const char* kv_dir() const override {
    return nullptr;
  }

  const char* events_dir() const override {
    return nullptr;
  }

private:
  // Convert from framework DataVariant to SoDataVariant
  static SoDataVariant to_so_variant(const DataVariant& v) {
    if (std::holds_alternative<std::string>(v)) {
      return SoDataVariant(std::get<std::string>(v));
    } else if (std::holds_alternative<int64_t>(v)) {
      return SoDataVariant(std::get<int64_t>(v));
    } else if (std::holds_alternative<double>(v)) {
      return SoDataVariant(std::get<double>(v));
    } else if (std::holds_alternative<bool>(v)) {
      return SoDataVariant(std::get<bool>(v));
    }
    return SoDataVariant{};
  }

  // Convert from SoDataVariant to framework DataVariant
  static DataVariant to_framework_variant(const SoDataVariant& v) {
    if (std::holds_alternative<std::string>(v)) {
      return DataVariant(std::get<std::string>(v));
    } else if (std::holds_alternative<int64_t>(v)) {
      return DataVariant(std::get<int64_t>(v));
    } else if (std::holds_alternative<double>(v)) {
      return DataVariant(std::get<double>(v));
    } else if (std::holds_alternative<bool>(v)) {
      return DataVariant(std::get<bool>(v));
    }
    return DataVariant{};
  }

  void notify_callback(const SubscriptionHandle& handle, const std::string& key,
                       const SoDataVariant& value) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);

    auto it = subscriptions_.find(handle.id);
    if (it != subscriptions_.end() && it->second.callback) {
      it->second.callback(key, value);
    }
  }

  MMDataBus* databus_;

  struct SubscriptionInfo {
    std::string key;
    SoDataBusCallback callback;
    bool is_topic = false;
  };

  std::mutex subscription_mutex_;
  std::unordered_map<uint64_t, SubscriptionInfo> subscriptions_;
  uint64_t next_subscription_id_ = 1;
};

/**
 * @brief FileDataBusAdapter provides file-based DataBus paths.
 */
class FileDataBusAdapter : public INodeDataBus {
public:
  FileDataBusAdapter() = default;

  FileDataBusAdapter(const std::string& kv_dir, const std::string& events_dir)
      : kv_dir_(kv_dir), events_dir_(events_dir) {}

  ~FileDataBusAdapter() override = default;

  bool put(const std::string& key, const SoDataVariant& value) override {
    (void)key; (void)value;
    return false;
  }

  bool get(const std::string& key, SoDataVariant& value) const override {
    (void)key; (void)value;
    return false;
  }

  bool contains(const std::string& key) const override {
    (void)key;
    return false;
  }

  bool remove(const std::string& key) override {
    (void)key;
    return false;
  }

  bool publish(const std::string& topic, const SoDataVariant& value) override {
    (void)topic; (void)value;
    return false;
  }

  SubscriptionHandle subscribe(const std::string& key, SoDataBusCallback callback) override {
    (void)key; (void)callback;
    return SubscriptionHandle{};
  }

  SubscriptionHandle subscribe_topic(const std::string& topic, SoDataBusCallback callback) override {
    (void)topic; (void)callback;
    return SubscriptionHandle{};
  }

  void unsubscribe(const SubscriptionHandle& handle) override { (void)handle; }
  void unsubscribe_all() override {}

  const char* kv_dir() const override {
    return kv_dir_.empty() ? nullptr : kv_dir_.c_str();
  }

  const char* events_dir() const override {
    return events_dir_.empty() ? nullptr : events_dir_.c_str();
  }

private:
  std::string kv_dir_;
  std::string events_dir_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_NODE_DATABUS_ADAPTER_H