// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Local data channel implementation with zero-copy support

#include "agentflow/runtime/local_channel.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include "agentflow/utils/logger.h"

namespace agentflow {
namespace runtime {

// ============================================================================
// LocalChannel Implementation
// ============================================================================

LocalChannel::LocalChannel(const std::string& channel_id, size_t capacity)
    : channel_id_(channel_id), capacity_(capacity), open_(false) {
  // Pre-allocate queue capacity
  queue_.reserve(capacity > 0 ? capacity : 100);
}

LocalChannel::~LocalChannel() {
  close();
}

bool LocalChannel::is_open() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return open_;
}

bool LocalChannel::open() {
  std::lock_guard<std::mutex> lock(mutex_);

  if(open_) {
    return true;
  }

  open_ = true;
  Logger::instance().info("LocalChannel [" + channel_id_ + "] opened");
  return true;
}

void LocalChannel::close() {
  std::lock_guard<std::mutex> lock(mutex_);

  if(!open_) {
    return;
  }

  open_ = false;

  // Notify all waiting threads
  cv_.notify_all();

  // Notify key-specific condition variables
  for(auto& [key, cv_ptr] : key_cvs_) {
    if(cv_ptr) {
      cv_ptr->notify_all();
    }
  }

  // Clean up key condition variables
  for(auto& [key, cv_ptr] : key_cvs_) {
    delete cv_ptr;
  }
  key_cvs_.clear();

  // Notify subscribers
  DataPacket empty_packet;
  empty_packet.key = channel_id_;
  notify_subscribers(ChannelEvent::CHANNEL_CLOSED, empty_packet);

  Logger::instance().info("LocalChannel [" + channel_id_ + "] closed");
}

bool LocalChannel::send(const DataPacket& packet) {
  std::unique_lock<std::mutex> lock(mutex_);

  if(!open_) {
    return false;
  }

  // Check capacity
  if(capacity_ > 0 && queue_.size() >= capacity_) {
    stats_.errors++;
    Logger::instance().warn("LocalChannel [" + channel_id_ + "] queue full, send failed");
    return false;
  }

  // Add to queue (zero-copy: move packet)
  queue_.push_back(packet);
  stats_.packets_sent++;
  stats_.bytes_sent += sizeof(packet);

  // Store by key for key-based access
  key_data_[packet.key].push_back(packet);

  // Notify receivers
  cv_.notify_one();
  notify_subscribers(ChannelEvent::DATA_READY, packet);

  // Notify key-specific waiters
  auto key_cv = get_key_cv(packet.key);
  if(key_cv) {
    key_cv->notify_one();
  }

  return true;
}

bool LocalChannel::receive(DataPacket& packet, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);

  if(!open_ && queue_.empty()) {
    return false;
  }

  // Wait for data
  auto pred = [this] { return !queue_.empty() || !open_; };

  if(timeout_ms <= 0) {
    cv_.wait(lock, pred);
  } else {
    if(!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred)) {
      // Timeout
      return false;
    }
  }

  if(!open_ || queue_.empty()) {
    return false;
  }

  // Get packet (zero-copy: move)
  packet = std::move(queue_.front());
  queue_.erase(queue_.begin());

  // Remove from key data (keep latest for get_latest)
  auto& key_packets = key_data_[packet.key];
  if(!key_packets.empty()) {
    key_packets.erase(key_packets.begin());
  }

  stats_.packets_received++;
  stats_.bytes_received += sizeof(packet);
  stats_.queue_depth = static_cast<int64_t>(queue_.size());

  // Notify subscribers
  notify_subscribers(ChannelEvent::DATA_CONSUMED, packet);

  return true;
}

bool LocalChannel::try_receive(DataPacket& packet) {
  std::lock_guard<std::mutex> lock(mutex_);

  if(queue_.empty()) {
    return false;
  }

  packet = std::move(queue_.front());
  queue_.erase(queue_.begin());

  // Remove from key data
  auto& key_packets = key_data_[packet.key];
  if(!key_packets.empty()) {
    key_packets.erase(key_packets.begin());
  }

  stats_.packets_received++;
  stats_.bytes_received += sizeof(packet);
  stats_.queue_depth = static_cast<int64_t>(queue_.size());

  notify_subscribers(ChannelEvent::DATA_CONSUMED, packet);

  return true;
}

bool LocalChannel::send_to_key(const std::string& key, const DataValue& value,
                               const std::string& source_id) {
  DataPacket packet(key, value, source_id);
  return send(packet);
}

bool LocalChannel::receive_from_key(const std::string& key, DataPacket& packet, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);

  if(!open_) {
    return false;
  }

  auto key_cv = get_key_cv(key);

  // Wait for data on specific key
  auto pred = [this, &key] {
    auto it = key_data_.find(key);
    return (it != key_data_.end() && !it->second.empty()) || !open_;
  };

  if(timeout_ms <= 0) {
    if(key_cv) {
      key_cv->wait(lock, pred);
    } else {
      cv_.wait(lock, pred);
    }
  } else {
    bool success;
    if(key_cv) {
      success = key_cv->wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
    } else {
      success = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
    }

    if(!success) {
      return false;  // Timeout
    }
  }

  if(!open_) {
    return false;
  }

  // Get packet from key data
  auto it = key_data_.find(key);
  if(it == key_data_.end() || it->second.empty()) {
    return false;
  }

  packet = std::move(it->second.front());
  it->second.erase(it->second.begin());

  // Remove from main queue if present
  auto queue_it = std::find_if(queue_.begin(), queue_.end(),
                               [&key](const DataPacket& p) { return p.key == key; });
  if(queue_it != queue_.end()) {
    queue_.erase(queue_it);
  }

  stats_.packets_received++;
  stats_.bytes_received += sizeof(packet);

  notify_subscribers(ChannelEvent::DATA_CONSUMED, packet);

  return true;
}

size_t LocalChannel::subscribe(EventCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t subscription_id        = next_subscription_id_++;
  subscribers_[subscription_id] = callback;

  Logger::instance().debug("LocalChannel [" + channel_id_ + "] subscription "
                           + std::to_string(subscription_id) + " added");

  return subscription_id;
}

bool LocalChannel::unsubscribe(size_t subscription_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscribers_.find(subscription_id);
  if(it != subscribers_.end()) {
    subscribers_.erase(it);
    Logger::instance().debug("LocalChannel [" + channel_id_ + "] subscription "
                             + std::to_string(subscription_id) + " removed");
    return true;
  }

  return false;
}

size_t LocalChannel::get_queue_depth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

ChannelStats LocalChannel::get_stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ChannelStats                stats = stats_;
  stats.queue_depth                 = static_cast<int64_t>(queue_.size());
  return stats;
}

void LocalChannel::reset_stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_ = ChannelStats();
}

void LocalChannel::clear() {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t cleared = queue_.size();
  queue_.clear();
  key_data_.clear();

  stats_.packets_sent += cleared;
  Logger::instance().info("LocalChannel [" + channel_id_ + "] cleared " + std::to_string(cleared)
                          + " packets");
}

bool LocalChannel::has_data(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = key_data_.find(key);
  return it != key_data_.end() && !it->second.empty();
}

DataValue LocalChannel::get_latest(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = key_data_.find(key);
  if(it != key_data_.end() && !it->second.empty()) {
    return it->second.back().value;
  }

  return std::string();  // Return empty variant
}

std::vector<DataPacket> LocalChannel::get_all(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = key_data_.find(key);
  if(it != key_data_.end()) {
    return it->second;
  }

  return std::vector<DataPacket>();
}

bool LocalChannel::wait_for_data(const std::string& key, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);

  if(!open_) {
    return false;
  }

  auto key_cv = get_key_cv(key);
  auto pred   = [this, &key] {
    auto it = key_data_.find(key);
    return (it != key_data_.end() && !it->second.empty()) || !open_;
  };

  if(timeout_ms <= 0) {
    if(key_cv) {
      key_cv->wait(lock, pred);
    } else {
      cv_.wait(lock, pred);
    }
  } else {
    if(key_cv) {
      return key_cv->wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
    } else {
      return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
    }
  }

  return has_data(key);
}

void LocalChannel::notify_subscribers(ChannelEvent event, const DataPacket& packet) {
  // Notify all subscribers (make copies of callback to avoid holding lock)
  std::map<size_t, EventCallback> subscribers_copy = subscribers_;

  for(auto& [id, callback] : subscribers_copy) {
    try {
      callback(event, packet);
    } catch(const std::exception& e) {
      Logger::instance().error("LocalChannel [" + channel_id_
                               + "] subscriber callback error: " + std::string(e.what()));
    }
  }
}

std::condition_variable* LocalChannel::get_key_cv(const std::string& key) {
  auto it = key_cvs_.find(key);
  if(it == key_cvs_.end()) {
    key_cvs_[key] = new std::condition_variable();
    return key_cvs_[key];
  }
  return it->second;
}

// ============================================================================
// LocalChannelRegistry Implementation
// ============================================================================

LocalChannelRegistry& LocalChannelRegistry::instance() {
  static LocalChannelRegistry registry;
  return registry;
}

std::shared_ptr<LocalChannel> LocalChannelRegistry::get_or_create(const std::string& channel_id,
                                                                  size_t             capacity) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = channels_.find(channel_id);
  if(it != channels_.end()) {
    return it->second;
  }

  auto channel          = std::make_shared<LocalChannel>(channel_id, capacity);
  channels_[channel_id] = channel;

  Logger::instance().info("LocalChannelRegistry: created channel [" + channel_id + "]");
  return channel;
}

std::shared_ptr<LocalChannel> LocalChannelRegistry::get(const std::string& channel_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = channels_.find(channel_id);
  if(it != channels_.end()) {
    return it->second;
  }

  return nullptr;
}

bool LocalChannelRegistry::remove(const std::string& channel_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = channels_.find(channel_id);
  if(it != channels_.end()) {
    it->second->close();
    channels_.erase(it);

    Logger::instance().info("LocalChannelRegistry: removed channel [" + channel_id + "]");
    return true;
  }

  return false;
}

std::vector<std::string> LocalChannelRegistry::get_channel_ids() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> ids;
  ids.reserve(channels_.size());

  for(const auto& [id, channel] : channels_) {
    ids.push_back(id);
  }

  return ids;
}

size_t LocalChannelRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return channels_.size();
}

void LocalChannelRegistry::close_all() {
  std::lock_guard<std::mutex> lock(mutex_);

  for(auto& [id, channel] : channels_) {
    channel->close();
  }

  channels_.clear();

  Logger::instance().info("LocalChannelRegistry: closed all channels");
}

}  // namespace runtime
}  // namespace agentflow
