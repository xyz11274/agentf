// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Storage interface definitions for MMAgent and MMWorkflow persistence

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentflow {
namespace utils {
namespace storage {

/**
 * Storage error codes
 */
enum class StorageError {
  STORAGE_OK                     = 0,
  STORAGE_ERROR_IO               = 1,
  STORAGE_ERROR_NOT_FOUND        = 2,
  STORAGE_ERROR_ALREADY_EXISTS   = 3,
  STORAGE_ERROR_INVALID_DATA     = 4,
  STORAGE_ERROR_VERSION_CONFLICT = 5,
  STORAGE_ERROR_UNKNOWN          = 6,
};

/**
 * Storage operation result
 */
struct StorageResult {
  StorageError error = StorageError::STORAGE_OK;
  std::string  message;

  bool is_success() const { return error == StorageError::STORAGE_OK; }
};

/**
 * Version information
 */
struct VersionInfo {
  std::string version;
  int64_t     timestamp;  // Unix timestamp in milliseconds
  std::string description;
  std::string author;
  std::string commit_id;  // Git commit ID if applicable

  bool operator==(const VersionInfo& other) const {
    return version == other.version && timestamp == other.timestamp;
  }
};

/**
 * Generic storage interface for key-value operations
 */
class IStorage {
public:
  virtual ~IStorage() = default;

  /**
   * Initialize storage backend
   */
  virtual StorageResult initialize() = 0;

  /**
   * Save data with key
   */
  virtual StorageResult save(const std::string& key, const std::string& data) = 0;

  /**
   * Load data by key
   */
  virtual StorageResult load(const std::string& key, std::string& data) = 0;

  /**
   * Delete data by key
   */
  virtual StorageResult remove(const std::string& key) = 0;

  /**
   * Check if key exists
   */
  virtual bool exists(const std::string& key) = 0;

  /**
   * List all keys matching a prefix
   */
  virtual StorageResult list(const std::string& prefix, std::vector<std::string>& keys) = 0;

  /**
   * Get last error
   */
  virtual StorageResult get_last_error() const = 0;

  /**
   * Cleanup and close storage
   */
  virtual void cleanup() = 0;
};

/**
 * Versioned storage interface
 * Adds version management capabilities
 */
class IVersionStorage : public IStorage {
public:
  virtual ~IVersionStorage() = default;

  /**
   * Save data with version
   */
  virtual StorageResult save_version(const std::string& key, const std::string& data,
                                     const VersionInfo& version) = 0;

  /**
   * Load data by key and version
   */
  virtual StorageResult load_version(const std::string& key, const std::string& version,
                                     std::string& data) = 0;

  /**
   * Get latest version info for a key
   */
  virtual StorageResult get_latest_version(const std::string& key, VersionInfo& version) = 0;

  /**
   * List all versions for a key
   */
  virtual StorageResult list_versions(const std::string&        key,
                                      std::vector<VersionInfo>& versions) = 0;

  /**
   * Delete a specific version
   */
  virtual StorageResult delete_version(const std::string& key, const std::string& version) = 0;

  /**
   * Set version as active (for canary/gray release)
   */
  virtual StorageResult set_active_version(const std::string& key, const std::string& version) = 0;

  /**
   * Get active version for a key
   */
  virtual StorageResult get_active_version(const std::string& key, std::string& version) = 0;

  /**
   * Rollback to a specific version
   */
  virtual StorageResult rollback_to_version(const std::string& key, const std::string& version) = 0;
};

/**
 * Storage configuration
 */
struct StorageConfig {
  std::string base_path;  // Base directory for file-based storage
  bool        use_compression   = false;
  int         max_versions      = 10;  // Maximum number of versions to keep
  bool        enable_versioning = true;
};

/**
 * Factory function to create versioned storage instance
 */
std::unique_ptr<IVersionStorage> create_versioned_storage(const StorageConfig& config);

}  // namespace storage
}  // namespace utils
}  // namespace agentflow
