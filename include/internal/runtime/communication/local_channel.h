// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Local data channel for business data communication

#ifndef AGENTFLOW_RUNTIME_LOCAL_CHANNEL_H
#define AGENTFLOW_RUNTIME_LOCAL_CHANNEL_H

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace runtime {

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
/// Provides high-performance, zero-copy data passing between actors
class LocalChannel {
public:
  /// @brief Constructor
  /// @param channel_id Unique channel identifier
  /// @param capacity Maximum number of packets in queue (0 = unlimited)
  explicit LocalChannel(const std::string& channel_id, size_t capacity = 1000);

  /// @brief Destructor
  ~LocalChannel();

  /// @brief Get channel ID
  const std::string& get_channel_id() const { return channel_id_; }

  /// @brief Check if channel is open
  bool is_open() const;

  /// @brief Open the channel
  /// @return true if successful
  bool open();

  /// @brief Close the channel
  void close();

  /// @brief Send data packet to channel
  /// @param packet The data packet
  /// @return true if successful (false if channel closed or full)
  bool send(const DataPacket& packet);

  /// @brief Receive data packet from channel (blocking)
  /// @param packet Output parameter for received packet
  /// @param timeout_ms Timeout in milliseconds (0 = infinite)
  /// @return true if packet received, false if timeout or channel closed
  bool receive(DataPacket& packet, int timeout_ms = 0);

  /// @brief Try to receive without blocking
  /// @param packet Output parameter for received packet
  /// @return true if packet was available
  bool try_receive(DataPacket& packet);

  /// @brief Send data to a specific key in the channel
  /// @param key The data key
  /// @param value The data value
  /// @param source_id Source identifier
  /// @return true if successful
  bool send_to_key(const std::string& key, const DataValue& value,
                   const std::string& source_id = "");

  /// @brief Receive data from a specific key
  /// @param key The data key
  /// @param packet Output parameter for received packet
  /// @param timeout_ms Timeout in milliseconds (0 = infinite)
  /// @return true if packet received
  bool receive_from_key(const std::string& key, DataPacket& packet, int timeout_ms = 0);

  /// @brief Subscribe to channel events
  /// @param callback The event callback function
  /// @return Subscription ID (for unsubscribing)
  size_t subscribe(EventCallback callback);

  /// @brief Unsubscribe from channel events
  /// @param subscription_id The subscription ID to remove
  /// @return true if successful
  bool unsubscribe(size_t subscription_id);

  /// @brief Get current queue depth
  size_t get_queue_depth() const;

  /// @brief Get channel statistics
  ChannelStats get_stats() const;

  /// @brief Reset statistics
  void reset_stats();

  /// @brief Clear all pending data in the channel
  void clear();

  /// @brief Check if channel has data for a specific key
  /// @param key The data key
  /// @return true if data is available
  bool has_data(const std::string& key) const;

  /// @brief Get latest data for a key (non-blocking)
  /// @param key The data key
  /// @return Data value or empty variant if not found
  DataValue get_latest(const std::string& key) const;

  /// @brief Get all data for a key
  /// @param key The data key
  /// @return Vector of data packets
  std::vector<DataPacket> get_all(const std::string& key) const;

  /// @brief Wait for data to be available on a key
  /// @param key The data key
  /// @param timeout_ms Timeout in milliseconds (0 = infinite)
  /// @return true if data became available
  bool wait_for_data(const std::string& key, int timeout_ms = 0);

private:
  std::string channel_id_;
  size_t      capacity_;
  bool        open_ = false;

  mutable std::mutex      mutex_;
  std::condition_variable cv_;

  std::vector<DataPacket>                         queue_;
  std::map<std::string, std::vector<DataPacket>>  key_data_;
  std::map<std::string, std::condition_variable*> key_cvs_;

  std::map<size_t, EventCallback> subscribers_;
  size_t                          next_subscription_id_ = 1;

  ChannelStats stats_;

  /// @brief Notify subscribers
  void notify_subscribers(ChannelEvent event, const DataPacket& packet);

  /// @brief Get or create condition variable for a key
  std::condition_variable* get_key_cv(const std::string& key);
};

/// @brief Local channel registry for managing multiple channels
class LocalChannelRegistry {
public:
  /// @brief Get singleton instance
  static LocalChannelRegistry& instance();

  /// @brief Create or get a channel
  /// @param channel_id Channel identifier
  /// @param capacity Channel capacity
  /// @return Shared pointer to the channel
  std::shared_ptr<LocalChannel> get_or_create(const std::string& channel_id,
                                              size_t             capacity = 1000);

  /// @brief Get an existing channel
  /// @param channel_id Channel identifier
  /// @return Shared pointer to the channel or nullptr if not found
  std::shared_ptr<LocalChannel> get(const std::string& channel_id);

  /// @brief Remove a channel
  /// @param channel_id Channel identifier
  /// @return true if channel was removed
  bool remove(const std::string& channel_id);

  /// @brief Get all channel IDs
  std::vector<std::string> get_channel_ids() const;

  /// @brief Get number of channels
  size_t size() const;

  /// @brief Close all channels
  void close_all();

private:
  LocalChannelRegistry()  = default;
  ~LocalChannelRegistry() = default;

  // Non-copyable, non-movable
  LocalChannelRegistry(const LocalChannelRegistry&)            = delete;
  LocalChannelRegistry& operator=(const LocalChannelRegistry&) = delete;

  mutable std::mutex                                   mutex_;
  std::map<std::string, std::shared_ptr<LocalChannel>> channels_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_LOCAL_CHANNEL_H
