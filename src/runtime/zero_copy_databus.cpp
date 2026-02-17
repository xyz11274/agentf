// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Zero-copy data bus implementation using shared memory

#include "agentflow/runtime/zero_copy_databus.h"

#include <algorithm>
#include <cstring>

#include "agentflow/utils/logger.h"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>

#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#endif

namespace agentflow {
namespace runtime {

// ============================================================================
// In-process shared memory implementation (using heap allocation)
// ============================================================================

/**
 * In-process shared memory block using heap allocation
 * This is the simplest approach and works well for single-process scenarios
 */
class InProcessSharedMemoryBlock : public SharedMemoryBlock {
public:
  explicit InProcessSharedMemoryBlock(size_t capacity) : capacity_(capacity), used_capacity_(0) {
    data_ = new std::map<std::string, DataVariant>();
  }

  virtual ~InProcessSharedMemoryBlock() { delete data_; }

  bool write(const std::string& key, const DataVariant& value) override {
    size_t value_size = estimate_data_size(value);
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
      size_t value_size = estimate_data_size(it->second);
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
  std::map<std::string, DataVariant>* data_;
  size_t                              capacity_;
  size_t                              used_capacity_;

  size_t estimate_data_size(const DataVariant& data) const {
    // DataVariant is std::variant<int64_t, double, std::string, bool>
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
        data);
  }
};

// ============================================================================
// ZeroCopyDataBus implementation
// ============================================================================

ZeroCopyDataBus::ZeroCopyDataBus(size_t capacity) : capacity_(capacity) {
  shared_memory_ = SharedMemoryFactory::create_best(capacity);
}

ZeroCopyDataBus::~ZeroCopyDataBus() {
  // No manual cleanup needed for smart pointers
}

bool ZeroCopyDataBus::put(const std::string& key, const DataVariant& value) {
  bool result;
  {
    std::shared_lock<std::shared_mutex> lock(data_mutex_);
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
  std::shared_lock<std::shared_mutex> lock(data_mutex_);
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
  std::shared_lock<std::shared_mutex> lock(data_mutex_);
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
  // DataVariant is std::variant<int64_t, double, std::string, bool>
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
      data);
}

// ============================================================================
// SharedMemoryFactory implementation
// ============================================================================

std::unique_ptr<SharedMemoryBlock> SharedMemoryFactory::create_in_process(size_t capacity) {
  return std::unique_ptr<SharedMemoryBlock>(new InProcessSharedMemoryBlock(capacity));
}

std::unique_ptr<SharedMemoryBlock> SharedMemoryFactory::create_posix_shared(
    size_t capacity, const std::string& name) {
  // POSIX shared memory implementation (for multi-process scenarios)
#ifdef __linux__
  int shm_fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
  if(shm_fd == -1) {
    Logger::instance().error_f("Failed to open POSIX shared memory: %s", name.c_str());
    return nullptr;
  }

  // Truncate to capacity
  if(ftruncate(shm_fd, capacity) == -1) {
    Logger::instance().error_f("Failed to truncate shared memory: %s", name.c_str());
    close(shm_fd);
    return nullptr;
  }

  // Map shared memory
  void* shm_addr = mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  close(shm_fd);

  if(shm_addr == MAP_FAILED) {
    Logger::instance().error("Failed to map shared memory");
    return nullptr;
  }

  Logger::instance().info_f("Created POSIX shared memory: %s (size: %zu bytes)", name.c_str(),
                            capacity);
  return std::unique_ptr<SharedMemoryBlock>(nullptr);
#else
  Logger::instance().error("POSIX shared memory not supported on this platform");
  return nullptr;
#endif
}

std::unique_ptr<SharedMemoryBlock> SharedMemoryFactory::create_systemv_shared(
    size_t capacity, const std::string& name) {
  // System V shared memory implementation (for multi-process scenarios)
#ifdef __linux__
  key_t key = ftok(name.c_str(), IPC_CREAT | 0666);
  if(key == -1) {
    Logger::instance().error_f("Failed to create System V key: %s", name.c_str());
    return nullptr;
  }

  int shm_id = shmget(key, capacity, IPC_CREAT | 0666);
  if(shm_id == -1) {
    Logger::instance().error_f("Failed to create System V shared memory");
    return nullptr;
  }

  // Map shared memory
  void* shm_addr = shmat(shm_id, nullptr, 0);
  if(shm_addr == (void*)-1) {
    Logger::instance().error("Failed to attach System V shared memory");
    return nullptr;
  }

  Logger::instance().info_f("Created System V shared memory: %s (size: %zu bytes)", name.c_str(),
                            capacity);
  return std::unique_ptr<SharedMemoryBlock>(nullptr);
#else
  Logger::instance().error("System V shared memory not supported on this platform");
  return nullptr;
#endif
}

std::unique_ptr<SharedMemoryBlock> SharedMemoryFactory::create_best(size_t             capacity,
                                                                    const std::string& name) {
  // Start with in-process shared memory (always available)
  auto block = create_in_process(capacity);
  if(block) {
    return block;
  }

  // Try POSIX shared memory (Linux)
  auto posix_block = create_posix_shared(capacity, name);
  if(posix_block) {
    return posix_block;
  }

  // Try System V shared memory (Linux)
  auto sysv_block = create_systemv_shared(capacity, name);
  if(sysv_block) {
    return sysv_block;
  }

  // Fallback to in-process shared memory
  return create_in_process(capacity);
}

}  // namespace runtime
}  // namespace agentflow
