// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Zero-copy data bus implementation using shared memory

#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace runtime {

/**
 * Shared memory block for zero-copy data sharing
 */
class SharedMemoryBlock {
public:
  virtual ~SharedMemoryBlock() = default;

  /**
   * Write data to shared memory
   */
  virtual bool write(const std::string& key, const DataVariant& value) = 0;

  /**
   * Read data from shared memory
   */
  virtual bool read(const std::string& key, DataVariant& value) const = 0;

  /**
   * Get data pointer directly (for zero-copy access)
   */
  virtual const DataVariant* get_data_ptr(const std::string& key) const = 0;

  /**
   * Check if key exists
   */
  virtual bool contains(const std::string& key) const = 0;

  /**
   * Remove data
   */
  virtual bool remove(const std::string& key) = 0;

  /**
   * Get data count
   */
  virtual size_t get_data_count() const = 0;

  /**
   * Get used capacity in bytes
   */
  virtual size_t get_used_capacity() const = 0;

  /**
   * Get total capacity in bytes
   */
  virtual size_t get_total_capacity() const = 0;
};

/**
 * Zero-copy data bus for efficient data sharing between nodes
 * Uses shared memory to avoid unnecessary data copies
 */
class ZeroCopyDataBus {
public:
  using DataChangeCallback = std::function<void(const std::string&, const DataVariant&)>;

  /**
   * Constructor
   * @param capacity Maximum capacity in bytes
   */
  explicit ZeroCopyDataBus(size_t capacity = 1024 * 1024);

  /**
   * Destructor
   */
  ~ZeroCopyDataBus();

  // Disable copy constructor and assignment
  ZeroCopyDataBus(const ZeroCopyDataBus&)            = delete;
  ZeroCopyDataBus& operator=(const ZeroCopyDataBus&) = delete;

  // Data operations
  bool put(const std::string& key, const DataVariant& value);

  bool get(const std::string& key, DataVariant& value) const;

  bool remove(const std::string& key);

  bool contains(const std::string& key) const;

  /**
   * Get data pointer directly for zero-copy access
   */
  const DataVariant* get_data_ptr(const std::string& key) const;

  /**
   * Get data by reference for read-only zero-copy access
   */
  bool get_data_ref(const std::string& key, DataVariant& ref) const;

  // Subscription mechanism
  void subscribe(const std::string& key, const DataChangeCallback& callback);

  void unsubscribe(const std::string& key);

  // Statistics
  size_t get_used_capacity() const;
  size_t get_total_capacity() const;
  int    get_data_count() const;

  /**
   * Get shared memory block (for direct access)
   */
  SharedMemoryBlock* get_shared_memory_block() const { return shared_memory_.get(); }

  /**
   * Acquire read lock for concurrent access
   */
  void lock_read() const;

  /**
   * Release read lock
   */
  void unlock_read() const;

  /**
   * Acquire write lock for exclusive access
   */
  void lock_write();

  /**
   * Release write lock
   */
  void unlock_write();

  /**
   * Get capacity usage percentage
   */
  double get_capacity_usage() const;

  /**
   * Clear all data
   */
  void clear();

private:
  std::unique_ptr<SharedMemoryBlock> shared_memory_;
  size_t                             capacity_;

  mutable std::shared_mutex data_mutex_;

  struct Subscription {
    std::string        key;
    DataChangeCallback callback;
  };

  std::vector<Subscription> subscriptions_;
  mutable std::mutex        subscriptions_mutex_;

  /**
   * Notify subscribers of data change
   */
  void notify_subscribers(const std::string& key, const DataVariant& value);

  /**
   * Estimate data size
   */
  size_t estimate_data_size(const DataVariant& data) const;
};

/**
 * Factory for creating shared memory blocks
 */
class SharedMemoryFactory {
public:
  /**
   * Create in-process shared memory (using heap allocation)
   * This is the simplest approach and works well for single-process scenarios
   */
  static std::unique_ptr<SharedMemoryBlock> create_in_process(size_t capacity);

  /**
   * Create POSIX shared memory (for multi-process scenarios)
   */
  static std::unique_ptr<SharedMemoryBlock> create_posix_shared(size_t             capacity,
                                                                const std::string& name);

  /**
   * Create System V shared memory (for multi-process scenarios)
   */
  static std::unique_ptr<SharedMemoryBlock> create_systemv_shared(size_t             capacity,
                                                                  const std::string& name);

  /**
   * Get the best available shared memory implementation
   */
  static std::unique_ptr<SharedMemoryBlock> create_best(size_t             capacity,
                                                        const std::string& name = "");
};

}  // namespace runtime
}  // namespace agentflow
