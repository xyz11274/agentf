// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Local data bus implementation combining message channel and shared memory

#include "runtime/databus/local_databus.h"

#include "agent/data_variant_utils.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

#include "utils/logger.h"

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

  // unique_ptr entries are destroyed automatically when key_cvs_ is cleared
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
    key_cvs_[key] = std::make_unique<std::condition_variable>();
    return key_cvs_[key].get();
  }
  return it->second.get();
}

// ============================================================================
// LocalChannelRegistry Implementation
// ============================================================================

LocalChannelRegistry& LocalChannelRegistry::instance() {
  static LocalChannelRegistry instance;
  return instance;
}

LocalChannelRegistry::~LocalChannelRegistry() {
  close_all();
}

std::shared_ptr<LocalChannel> LocalChannelRegistry::get_or_create(const std::string& channel_id,
                                                                  size_t             capacity) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = channels_.find(channel_id);
  if(it != channels_.end()) {
    return it->second;
  }

  auto channel = std::make_shared<LocalChannel>(channel_id, capacity);
  channels_[channel_id] = channel;
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
    return true;
  }
  return false;
}

std::vector<std::string> LocalChannelRegistry::get_channel_ids() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  for(const auto& [id, _] : channels_) {
    ids.push_back(id);
  }
  return ids;
}

void LocalChannelRegistry::close_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  for(auto& [id, channel] : channels_) {
    channel->close();
  }
  channels_.clear();
}

size_t LocalChannelRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return channels_.size();
}

// ============================================================================
// ZeroCopyDataBus Implementation
// ============================================================================

namespace {
// Helper function to estimate data size
size_t estimate_variant_size(const DataVariant& data) {
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
        } else {
          return data_variant_to_string(DataVariant(arg)).size();
        }
      },
      data);
}
}  // namespace

/**
 * In-process shared memory block using heap allocation
 */
class InProcessSharedMemoryBlock : public SharedMemoryBlock {
public:
  explicit InProcessSharedMemoryBlock(size_t capacity) : capacity_(capacity), used_capacity_(0) {
    data_ = std::make_unique<std::map<std::string, DataVariant>>();
  }

  virtual ~InProcessSharedMemoryBlock() = default;

  bool write(const std::string& key, const DataVariant& value) override {
    size_t value_size = estimate_variant_size(value);
    size_t key_size   = key.size();
    size_t total_size = value_size + key_size;

    if(used_capacity_ + total_size > capacity_) {
      Logger::instance().error_f("Shared memory full: used=%zu, need=%zu", used_capacity_,
                                 total_size);
      return false;
    }

    (*data_)[key] = value;
    used_capacity_ += total_size;
    return true;
  }

  bool read(const std::string& key, DataVariant& value) const override {
    auto it = data_->find(key);
    if(it != data_->end()) {
      value = it->second;
      return true;
    }
    return false;
  }

  const DataVariant* get_data_ptr(const std::string& key) const override {
    auto it = data_->find(key);
    if(it != data_->end()) {
      return &(it->second);
    }
    return nullptr;
  }

  bool contains(const std::string& key) const override { return data_->find(key) != data_->end(); }

  bool remove(const std::string& key) override {
    auto it = data_->find(key);
    if(it != data_->end()) {
      size_t key_size   = key.size();
      size_t value_size = estimate_variant_size(it->second);
      used_capacity_ -= (key_size + value_size);
      data_->erase(it);
      return true;
    }
    return false;
  }

  size_t get_data_count() const override { return data_->size(); }

  size_t get_used_capacity() const override { return used_capacity_; }

  size_t get_total_capacity() const override { return capacity_; }

private:
  std::unique_ptr<std::map<std::string, DataVariant>> data_;
  size_t                                              capacity_;
  size_t                                              used_capacity_;
};

ZeroCopyDataBus::ZeroCopyDataBus(size_t capacity) : capacity_(capacity) {
  shared_memory_ = SharedMemoryFactory::create_best(capacity);
}

ZeroCopyDataBus::~ZeroCopyDataBus() {
}

bool ZeroCopyDataBus::put(const std::string& key, const DataVariant& value) {
  bool result;
  {
    std::unique_lock<std::shared_mutex> lock(data_mutex_);
    result = shared_memory_->write(key, value);
  }
  if(result) {
    notify_subscribers(key, value);
  }
  return result;
}

bool ZeroCopyDataBus::get(const std::string& key, DataVariant& value) const {
  std::shared_lock<std::shared_mutex> lock(data_mutex_);
  return shared_memory_->read(key, value);
}

bool ZeroCopyDataBus::remove(const std::string& key) {
  std::unique_lock<std::shared_mutex> lock(data_mutex_);
  return shared_memory_->remove(key);
}

bool ZeroCopyDataBus::contains(const std::string& key) const {
  std::shared_lock<std::shared_mutex> lock(data_mutex_);
  return shared_memory_->contains(key);
}

const DataVariant* ZeroCopyDataBus::get_data_ptr(const std::string& key) const {
  std::shared_lock<std::shared_mutex> lock(data_mutex_);
  return shared_memory_->get_data_ptr(key);
}

bool ZeroCopyDataBus::get_data_ref(const std::string& key, DataVariant& ref) const {
  std::shared_lock<std::shared_mutex> lock(data_mutex_);
  const DataVariant*                  ptr = shared_memory_->get_data_ptr(key);
  if(ptr) {
    ref = *ptr;
    return true;
  }
  return false;
}

void ZeroCopyDataBus::subscribe(const std::string& key, const DataChangeCallback& callback) {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);

  // Remove existing subscription for same key
  subscriptions_.erase(std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                                      [&key](const Subscription& sub) { return sub.key == key; }),
                       subscriptions_.end());

  // Add new subscription
  Subscription sub;
  sub.key      = key;
  sub.callback = callback;
  subscriptions_.push_back(sub);

  Logger::instance().info_f("Subscribed to data key: %s", key.c_str());
}

void ZeroCopyDataBus::unsubscribe(const std::string& key) {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);

  subscriptions_.erase(std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                                      [&key](const Subscription& sub) { return sub.key == key; }),
                       subscriptions_.end());

  Logger::instance().info_f("Unsubscribed from data key: %s", key.c_str());
}

size_t ZeroCopyDataBus::get_used_capacity() const {
  return shared_memory_->get_used_capacity();
}

size_t ZeroCopyDataBus::get_total_capacity() const {
  return shared_memory_->get_total_capacity();
}

int ZeroCopyDataBus::get_data_count() const {
  return shared_memory_->get_data_count();
}

void ZeroCopyDataBus::lock_read() const {
  data_mutex_.lock_shared();
}

void ZeroCopyDataBus::unlock_read() const {
  data_mutex_.unlock_shared();
}

void ZeroCopyDataBus::lock_write() {
  data_mutex_.lock();
}

void ZeroCopyDataBus::unlock_write() {
  data_mutex_.unlock();
}

double ZeroCopyDataBus::get_capacity_usage() const {
  size_t total = get_total_capacity();
  if(total == 0)
    return 0.0;
  return static_cast<double>(get_used_capacity()) / total * 100.0;
}

void ZeroCopyDataBus::clear() {
  std::unique_lock<std::shared_mutex> lock(data_mutex_);
  shared_memory_ = SharedMemoryFactory::create_best(capacity_);
}

void ZeroCopyDataBus::notify_subscribers(const std::string& key, const DataVariant& value) {
  std::lock_guard<std::mutex> lock(subscriptions_mutex_);

  for(const auto& sub : subscriptions_) {
    if(sub.key == key || sub.key == "*") {
      sub.callback(key, value);
    }
  }
}

size_t ZeroCopyDataBus::estimate_data_size(const DataVariant& data) const {
  return estimate_variant_size(data);
}

// ============================================================================
// SharedMemoryFactory implementation
// ============================================================================

std::unique_ptr<SharedMemoryBlock> SharedMemoryFactory::create_in_process(size_t capacity) {
  return std::unique_ptr<SharedMemoryBlock>(new InProcessSharedMemoryBlock(capacity));
}

std::unique_ptr<SharedMemoryBlock> SharedMemoryFactory::create_posix_shared(
    size_t capacity, const std::string& name) {
#ifdef __linux__
  int shm_fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
  if(shm_fd == -1) {
    Logger::instance().error_f("Failed to open POSIX shared memory: %s", name.c_str());
    return nullptr;
  }

  if(ftruncate(shm_fd, capacity) == -1) {
    Logger::instance().error_f("Failed to truncate shared memory: %s", name.c_str());
    close(shm_fd);
    return nullptr;
  }

  void* shm_addr = mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  close(shm_fd);

  if(shm_addr == MAP_FAILED) {
    Logger::instance().error("Failed to map shared memory");
    return nullptr;
  }

  // InProcessSharedMemoryBlock uses C++ STL containers that cannot span process boundaries.
  // Release the OS segment and fall back to the in-process block.
  munmap(shm_addr, capacity);
  Logger::instance().warn_f(
      "POSIX shared memory allocated (%s, %zu bytes) but cross-process layout is not yet "
      "implemented; falling back to in-process block.",
      name.c_str(), capacity);
  return create_in_process(capacity);
#else
  Logger::instance().error("POSIX shared memory not supported on this platform");
  return nullptr;
#endif
}

std::unique_ptr<SharedMemoryBlock> SharedMemoryFactory::create_systemv_shared(
    size_t capacity, const std::string& name) {
#ifdef __linux__
  key_t key = ftok(name.c_str(), 1);
  if(key == -1) {
    Logger::instance().error_f("Failed to create System V key: %s", name.c_str());
    return nullptr;
  }

  int shm_id = shmget(key, capacity, IPC_CREAT | 0666);
  if(shm_id == -1) {
    Logger::instance().error_f("Failed to create System V shared memory");
    return nullptr;
  }

  void* shm_addr = shmat(shm_id, nullptr, 0);
  if(shm_addr == (void*)-1) {
    Logger::instance().error("Failed to attach System V shared memory");
    return nullptr;
  }

  // InProcessSharedMemoryBlock uses C++ STL containers that cannot span process boundaries.
  // Detach and release the OS segment, then fall back to the in-process block.
  shmdt(shm_addr);
  shmctl(shm_id, IPC_RMID, nullptr);
  Logger::instance().warn_f(
      "System V shared memory allocated (%s, %zu bytes) but cross-process layout is not yet "
      "implemented; falling back to in-process block.",
      name.c_str(), capacity);
  return create_in_process(capacity);
#else
  Logger::instance().error("System V shared memory not supported on this platform");
  return nullptr;
#endif
}

std::unique_ptr<SharedMemoryBlock> SharedMemoryFactory::create_best(size_t             capacity,
                                                                    const std::string& name) {
  auto block = create_in_process(capacity);
  if(block) {
    return block;
  }

  auto posix_block = create_posix_shared(capacity, name);
  if(posix_block) {
    return posix_block;
  }

  auto sysv_block = create_systemv_shared(capacity, name);
  if(sysv_block) {
    return sysv_block;
  }

  return create_in_process(capacity);
}

// ============================================================================
// LocalDatabus Implementation
// ============================================================================

LocalDatabus::LocalDatabus() {
  shared_memory_ = std::make_shared<ZeroCopyDataBus>();
}

LocalDatabus::~LocalDatabus() {
  for(auto& [id, channel] : channels_) {
    channel->close();
  }
  channels_.clear();
}

std::shared_ptr<LocalChannel> LocalDatabus::get_channel(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = channels_.find(id);
  if(it != channels_.end()) {
    return it->second;
  }

  auto channel = std::make_shared<LocalChannel>(id);
  channel->open();
  channels_[id] = channel;
  return channel;
}

std::shared_ptr<ZeroCopyDataBus> LocalDatabus::get_shared_memory() {
  return shared_memory_;
}

}  // namespace runtime
}  // namespace agentflow
