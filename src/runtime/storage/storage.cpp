// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Storage implementation using file-based backend

#include "runtime/storage/storage.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include "utils/logger.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/filewritestream.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#endif

namespace agentflow {
namespace storage {

namespace {

/**
 * Create directory recursively
 */
bool create_directory(const std::string& path) {
  struct stat st;
  if(stat(path.c_str(), &st) == 0) {
    return S_ISDIR(st.st_mode);
  }

  // Create parent directories
  size_t pos = path.find_last_of('/');
  if(pos != std::string::npos && pos > 0) {
    std::string parent = path.substr(0, pos);
    if(!create_directory(parent)) {
      return false;
    }
  }

  // Create directory
  return mkdir(path.c_str(), 0755) == 0;
}

/**
 * Get current timestamp in milliseconds
 */
int64_t get_current_timestamp_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

}  // anonymous namespace

// ============================================================================
// FileVersionStorage implementation
// ============================================================================

/**
 * File-based versioned storage implementation
 */
class FileVersionStorage : public IVersionStorage {
public:
  explicit FileVersionStorage(const StorageConfig& config)
      : config_(config), last_error_(StorageError::STORAGE_OK) {}

  virtual ~FileVersionStorage() { cleanup(); }

  StorageResult initialize() override {
    // Create base directory if it doesn't exist
    if(!create_directory(config_.base_path)) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to create base directory: " + config_.base_path;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    // Create subdirectories
    std::string data_dir     = config_.base_path + "/data";
    std::string versions_dir = config_.base_path + "/versions";
    std::string meta_dir     = config_.base_path + "/meta";

    if(!create_directory(data_dir) || !create_directory(versions_dir)
       || !create_directory(meta_dir)) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to create storage subdirectories";
      return StorageResult{ last_error_, last_error_msg_ };
    }

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  void cleanup() override {
    // No resources to cleanup for file-based storage
  }

  StorageResult save(const std::string& key, const std::string& data) override {
    std::string file_path = get_data_file_path(key);

    std::ofstream file(file_path);
    if(!file.is_open()) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to open file for writing: " + file_path;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    file << data;
    if(!file.good()) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to write data to file: " + file_path;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult load(const std::string& key, std::string& data) override {
    std::string file_path = get_data_file_path(key);

    std::ifstream file(file_path);
    if(!file.is_open()) {
      last_error_     = StorageError::STORAGE_ERROR_NOT_FOUND;
      last_error_msg_ = "File not found: " + file_path;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    data = buffer.str();

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult remove(const std::string& key) override {
    std::string file_path = get_data_file_path(key);

    if(unlink(file_path.c_str()) != 0) {
      if(errno == ENOENT) {
        last_error_     = StorageError::STORAGE_ERROR_NOT_FOUND;
        last_error_msg_ = "File not found: " + file_path;
      } else {
        last_error_     = StorageError::STORAGE_ERROR_IO;
        last_error_msg_ = "Failed to delete file: " + file_path;
      }
      return StorageResult{ last_error_, last_error_msg_ };
    }

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  bool exists(const std::string& key) override {
    std::string file_path = get_data_file_path(key);
    struct stat st;
    return stat(file_path.c_str(), &st) == 0;
  }

  StorageResult list(const std::string& prefix, std::vector<std::string>& keys) override {
    keys.clear();

    std::string dir_path = config_.base_path + "/data";
    DIR*        dir      = opendir(dir_path.c_str());
    if(!dir) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to open directory: " + dir_path;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    struct dirent* entry;
    while((entry = readdir(dir)) != NULL) {
      std::string name = entry->d_name;
      if(name != "." && name != "..") {
        if(prefix.empty() || name.find(prefix) == 0) {
          keys.push_back(name);
        }
      }
    }

    closedir(dir);

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult save_version(const std::string& key, const std::string& data,
                             const VersionInfo& version) override {
    std::string version_dir = get_version_dir_path(key);
    if(!create_directory(version_dir)) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to create version directory: " + version_dir;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    std::string version_file = version_dir + "/" + version.version + ".json";

    std::ofstream file(version_file);
    if(!file.is_open()) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to open version file for writing: " + version_file;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    // Write data with version metadata
#ifdef AGENTFLOW_WITH_RAPIDJSON
    rapidjson::Document doc;
    doc.SetObject();
    doc.AddMember("data", rapidjson::StringRef(data.c_str()), doc.GetAllocator());
    doc.AddMember("version", rapidjson::StringRef(version.version.c_str()), doc.GetAllocator());
    doc.AddMember("timestamp", version.timestamp, doc.GetAllocator());
    doc.AddMember("description", rapidjson::StringRef(version.description.c_str()),
                  doc.GetAllocator());
    doc.AddMember("author", rapidjson::StringRef(version.author.c_str()), doc.GetAllocator());
    doc.AddMember("commit_id", rapidjson::StringRef(version.commit_id.c_str()), doc.GetAllocator());

    rapidjson::StringBuffer                          buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    file << buffer.GetString();
#else
    file << data;
#endif

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult load_version(const std::string& key, const std::string& version,
                             std::string& data) override {
    std::string version_dir  = get_version_dir_path(key);
    std::string version_file = version_dir + "/" + version + ".json";

    std::ifstream file(version_file);
    if(!file.is_open()) {
      last_error_     = StorageError::STORAGE_ERROR_NOT_FOUND;
      last_error_msg_ = "Version file not found: " + version_file;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

#ifdef AGENTFLOW_WITH_RAPIDJSON
    rapidjson::Document doc;
    doc.Parse(content.c_str());
    if(doc.HasParseError()) {
      last_error_     = StorageError::STORAGE_ERROR_INVALID_DATA;
      last_error_msg_ = "Failed to parse version file: " + version_file;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    if(doc.HasMember("data")) {
      data = doc["data"].GetString();
    } else {
      data = content;
    }
#else
    data = content;
#endif

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult get_latest_version(const std::string& key, VersionInfo& version) override {
    std::vector<VersionInfo> versions;
    StorageResult            result = list_versions(key, versions);
    if(!result.is_success()) {
      return result;
    }

    if(versions.empty()) {
      last_error_     = StorageError::STORAGE_ERROR_NOT_FOUND;
      last_error_msg_ = "No versions found for key: " + key;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    // Find latest version by timestamp
    auto latest = std::max_element(
        versions.begin(), versions.end(),
        [](const VersionInfo& a, const VersionInfo& b) { return a.timestamp < b.timestamp; });

    version     = *latest;
    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult list_versions(const std::string& key, std::vector<VersionInfo>& versions) override {
    versions.clear();

    std::string version_dir = get_version_dir_path(key);
    DIR*        dir         = opendir(version_dir.c_str());
    if(!dir) {
      last_error_     = StorageError::STORAGE_ERROR_NOT_FOUND;
      last_error_msg_ = "Version directory not found: " + version_dir;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    struct dirent* entry;
    while((entry = readdir(dir)) != NULL) {
      std::string name = entry->d_name;
      if(name != "." && name != ".." && name.find(".json") != std::string::npos) {
        // Extract version string (remove .json extension)
        std::string version_str = name.substr(0, name.length() - 5);

        // Load version info from file
        std::string   version_file = version_dir + "/" + name;
        std::ifstream file(version_file);
        if(file.is_open()) {
          std::stringstream buffer;
          buffer << file.rdbuf();
          std::string content = buffer.str();

#ifdef AGENTFLOW_WITH_RAPIDJSON
          rapidjson::Document doc;
          doc.Parse(content.c_str());
          if(!doc.HasParseError()) {
            VersionInfo info;
            info.version     = doc.HasMember("version") ? doc["version"].GetString() : version_str;
            info.timestamp   = doc.HasMember("timestamp") ? doc["timestamp"].GetInt64() : 0;
            info.description = doc.HasMember("description") ? doc["description"].GetString() : "";
            info.author      = doc.HasMember("author") ? doc["author"].GetString() : "";
            info.commit_id   = doc.HasMember("commit_id") ? doc["commit_id"].GetString() : "";
            versions.push_back(info);
          }
#else
          VersionInfo info;
          info.version   = version_str;
          info.timestamp = 0;
          versions.push_back(info);
#endif
        }
      }
    }

    closedir(dir);

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult delete_version(const std::string& key, const std::string& version) override {
    std::string version_dir  = get_version_dir_path(key);
    std::string version_file = version_dir + "/" + version + ".json";

    if(unlink(version_file.c_str()) != 0) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to delete version file: " + version_file;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult set_active_version(const std::string& key, const std::string& version) override {
    std::string meta_dir    = config_.base_path + "/meta";
    std::string active_file = meta_dir + "/" + key + "_active.json";

    std::ofstream file(active_file);
    if(!file.is_open()) {
      last_error_     = StorageError::STORAGE_ERROR_IO;
      last_error_msg_ = "Failed to open active version file: " + active_file;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    file << version;
    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult get_active_version(const std::string& key, std::string& version) override {
    std::string meta_dir    = config_.base_path + "/meta";
    std::string active_file = meta_dir + "/" + key + "_active.json";

    std::ifstream file(active_file);
    if(!file.is_open()) {
      last_error_     = StorageError::STORAGE_ERROR_NOT_FOUND;
      last_error_msg_ = "Active version file not found: " + active_file;
      return StorageResult{ last_error_, last_error_msg_ };
    }

    file >> version;
    last_error_ = StorageError::STORAGE_OK;
    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult rollback_to_version(const std::string& key, const std::string& version) override {
    // Load version data
    std::string   data;
    StorageResult result = load_version(key, version, data);
    if(!result.is_success()) {
      return result;
    }

    // Save as current data
    result = save(key, data);
    if(!result.is_success()) {
      return result;
    }

    // Set as active version
    return set_active_version(key, version);
  }

  StorageResult get_last_error() const override {
    return StorageResult{ last_error_, last_error_msg_ };
  }

private:
  StorageConfig config_;
  StorageError  last_error_;
  std::string   last_error_msg_;

  std::string get_data_file_path(const std::string& key) const {
    return config_.base_path + "/data/" + key + ".json";
  }

  std::string get_version_dir_path(const std::string& key) const {
    return config_.base_path + "/versions/" + key;
  }
};

// ============================================================================
// Factory functions
// ============================================================================

std::unique_ptr<IVersionStorage> create_versioned_storage(const StorageConfig& config) {
  return std::unique_ptr<IVersionStorage>(new FileVersionStorage(config));
}

}  // namespace storage
}  // namespace agentflow
