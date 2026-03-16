// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Local data bus implementation combining message channel and shared memory

#ifndef AGENTFLOW_RUNTIME_LOCAL_DATABUS_H
#define AGENTFLOW_RUNTIME_LOCAL_DATABUS_H

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace runtime {

// ============================================================================
// LocalChannel (Message Queue)
// ============================================================================

/// @brief Channel event types
enum class ChannelEvent : uint8_t {
  DATA_READY     = 0,
  DATA_CONSUMED  = 1,
  CHANNEL_CLOSED = 2,
  ERROR          = 3,
};

/// @brief Data packet with metadata
struct DataPacket {
  std::string key;
  DataValue   value;
  std::string source_id;
  int64_t     timestamp_ms;
  uint32_t    sequence_number;

  DataPacket() : timestamp_ms(0), sequence_number(0) {}

  DataPacket(const std::string& k, const DataValue& v, const std::string& src = "")
      : key(k),
        value(v),
        source_id(src),
        timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count()),
        sequence_number(0) {}
};

/// @brief Channel statistics
struct ChannelStats {
  uint64_t packets_sent;
  uint64_t packets_received;
  uint64_t bytes_sent;
  uint64_t bytes_received;
  uint64_t errors;
  int64_t  queue_depth;

  ChannelStats()
      : packets_sent(0),
        packets_received(0),
        bytes_sent(0),
        bytes_received(0),
        errors(0),
        queue_depth(0) {}
};

/// @brief Event callback type
using EventCallback = std::function<void(ChannelEvent, const DataPacket&)>;

/// @brief Local data channel for intra-process data sharing
class LocalChannel {
public:
  explicit LocalChannel(const std::string& channel_id, size_t capacity = 1000);
  ~LocalChannel();

  const std::string& get_channel_id() const { return channel_id_; }
  bool               is_open() const;
  bool               open();
  void               close();

  bool send(const DataPacket& packet);
  bool receive(DataPacket& packet, int timeout_ms = 0);
  bool try_receive(DataPacket& packet);

  bool send_to_key(const std::string& key, const DataValue& value, const std::string& source_id = "");
  bool receive_from_key(const std::string& key, DataPacket& packet, int timeout_ms = 0);

  size_t subscribe(EventCallback callback);
  bool   unsubscribe(size_t subscription_id);

  size_t       get_queue_depth() const;
  ChannelStats get_stats() const;
  void         reset_stats();
  void         clear();

  bool                    has_data(const std::string& key) const;
  DataValue               get_latest(const std::string& key) const;
  std::vector<DataPacket> get_all(const std::string& key) const;
  bool                    wait_for_data(const std::string& key, int timeout_ms = 0);

private:
  std::string channel_id_;
  size_t      capacity_;
  bool        open_ = false;

  mutable std::mutex      mutex_;
  std::condition_variable cv_;

  std::vector<DataPacket>                        queue_;
  std::map<std::string, std::vector<DataPacket>> key_data_;
  std::map<std::string, std::unique_ptr<std::condition_variable>> key_cvs_;

  std::map<size_t, EventCallback> subscribers_;
  size_t                          next_subscription_id_ = 1;

  ChannelStats stats_;

  void                     notify_subscribers(ChannelEvent event, const DataPacket& packet);
  std::condition_variable* get_key_cv(const std::string& key);
};

// ============================================================================
// LocalChannelRegistry
// ============================================================================

class LocalChannelRegistry {
public:
  static LocalChannelRegistry& instance();

  std::shared_ptr<LocalChannel> get_or_create(const std::string& channel_id, size_t capacity = 1000);
  std::shared_ptr<LocalChannel> get(const std::string& channel_id);
  bool                          remove(const std::string& channel_id);
  std::vector<std::string>      get_channel_ids() const;
  void                          close_all();
  size_t                        size() const;

private:
  LocalChannelRegistry() = default;
  ~LocalChannelRegistry();

  std::map<std::string, std::shared_ptr<LocalChannel>> channels_;
  mutable std::mutex                                   mutex_;
};

// ============================================================================
// ZeroCopyDataBus (Shared Memory)
// ============================================================================

class SharedMemoryBlock {
public:
  virtual ~SharedMemoryBlock() = default;
  virtual bool               write(const std::string& key, const DataVariant& value) = 0;
  virtual bool               read(const std::string& key, DataVariant& value) const  = 0;
  virtual const DataVariant* get_data_ptr(const std::string& key) const              = 0;
  virtual bool               contains(const std::string& key) const                  = 0;
  virtual bool               remove(const std::string& key)                          = 0;
  virtual int                remove_by_prefix(const std::string& prefix)             = 0;
  virtual std::vector<std::string> list_keys_by_prefix(const std::string& prefix) const = 0;
  virtual size_t             get_data_count() const                                  = 0;
  virtual size_t             get_used_capacity() const                               = 0;
  virtual size_t             get_total_capacity() const                              = 0;
};

class ZeroCopyDataBus {
public:
  using DataChangeCallback = std::function<void(const std::string&, const DataVariant&)>;

  explicit ZeroCopyDataBus(size_t capacity = 1024 * 1024);
  ~ZeroCopyDataBus();

  // Disable copy constructor and assignment
  ZeroCopyDataBus(const ZeroCopyDataBus&)            = delete;
  ZeroCopyDataBus& operator=(const ZeroCopyDataBus&) = delete;

  bool put(const std::string& key, const DataVariant& value);
  bool get(const std::string& key, DataVariant& value) const;
  bool remove(const std::string& key);
  bool contains(const std::string& key) const;

  // Remove all keys matching prefix
  int remove_by_prefix(const std::string& prefix);
  // Get all keys matching prefix
  std::vector<std::string> list_keys_by_prefix(const std::string& prefix) const;

  const DataVariant* get_data_ptr(const std::string& key) const;
  bool               get_data_ref(const std::string& key, DataVariant& ref) const;

  void subscribe(const std::string& key, const DataChangeCallback& callback);
  void unsubscribe(const std::string& key);

  size_t             get_used_capacity() const;
  size_t             get_total_capacity() const;
  int                get_data_count() const;
  SharedMemoryBlock* get_shared_memory_block() const { return shared_memory_.get(); }

  void   lock_read() const;
  void   unlock_read() const;
  void   lock_write();
  void   unlock_write();
  double get_capacity_usage() const;
  void   clear();

private:
  std::unique_ptr<SharedMemoryBlock> shared_memory_;
  size_t                             capacity_;
  mutable std::shared_mutex          data_mutex_;

  struct Subscription {
    std::string        key;
    DataChangeCallback callback;
  };

  std::vector<Subscription> subscriptions_;
  mutable std::mutex        subscriptions_mutex_;

  void   notify_subscribers(const std::string& key, const DataVariant& value);
  size_t estimate_data_size(const DataVariant& data) const;
};

class SharedMemoryFactory {
public:
  static std::unique_ptr<SharedMemoryBlock> create_in_process(size_t capacity);
  static std::unique_ptr<SharedMemoryBlock> create_posix_shared(size_t             capacity,
                                                                const std::string& name);
  static std::unique_ptr<SharedMemoryBlock> create_systemv_shared(size_t             capacity,
                                                                  const std::string& name);
  static std::unique_ptr<SharedMemoryBlock> create_best(size_t             capacity,
                                                        const std::string& name = "");
};

// ============================================================================
// LocalDatabus (Unified Interface)
// ============================================================================

class LocalDatabus {
public:
  LocalDatabus();
  ~LocalDatabus();

  std::shared_ptr<LocalChannel>    get_channel(const std::string& id);
  std::shared_ptr<ZeroCopyDataBus> get_shared_memory();

private:
  std::map<std::string, std::shared_ptr<LocalChannel>> channels_;
  std::shared_ptr<ZeroCopyDataBus>                     shared_memory_;
  mutable std::mutex                                   mutex_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_LOCAL_DATABUS_H
