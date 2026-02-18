#include "runtime/hot_reload.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "utils/logger.h"

namespace agentflow {
namespace runtime {

namespace fs = std::filesystem;

// ============================================================================
// Executable type detection
// ============================================================================

std::string executable_type_to_string(ExecutableType type) {
  switch(type) {
    case ExecutableType::SO_LIBRARY:
      return "SO_LIBRARY";
    case ExecutableType::JAR_FILE:
      return "JAR_FILE";
    case ExecutableType::SHELL_SCRIPT:
      return "SHELL_SCRIPT";
    case ExecutableType::PYTHON_SCRIPT:
      return "PYTHON_SCRIPT";
    case ExecutableType::EXECUTABLE:
      return "EXECUTABLE";
    default:
      return "UNKNOWN";
  }
}

ExecutableType detect_executable_type(const std::string& file_path) {
  std::string ext = fs::path(file_path).extension().string();

  // Convert to lowercase for case-insensitive comparison
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if(ext == ".so" || ext == ".dylib" || ext == ".dll") {
    return ExecutableType::SO_LIBRARY;
  } else if(ext == ".jar") {
    return ExecutableType::JAR_FILE;
  } else if(ext == ".sh" || ext == ".bash") {
    return ExecutableType::SHELL_SCRIPT;
  } else if(ext == ".py") {
    return ExecutableType::PYTHON_SCRIPT;
  } else if(ext.empty() || ext == ".exe" || ext == ".bin") {
    // Check if file has execute permission
    std::error_code ec;
    auto            perms = fs::status(file_path, ec).permissions();
    if(!ec && (perms & fs::perms::owner_exec) != fs::perms::none) {
      return ExecutableType::EXECUTABLE;
    }
  }

  return ExecutableType::UNKNOWN;
}

// ============================================================================
// FileWatcherImpl - Platform-specific file watching implementation
// ============================================================================

struct HotReloadManager::FileWatcherImpl {
  std::map<std::string, std::chrono::system_clock::time_point> last_modified_times_;
  std::thread                                                  watcher_thread_;
  std::atomic<bool>                                            running_{ false };
  mutable std::mutex                                           mutex_;
  HotReloadManager*                                            manager_;

  FileWatcherImpl(HotReloadManager* mgr) : manager_(mgr) {}

  ~FileWatcherImpl() { stop(); }

  void start(const HotReloadConfig& config) {
    running_        = true;
    watcher_thread_ = std::thread(&FileWatcherImpl::watch_loop, this, config);
    Logger::instance().info("[FILE_WATCHER] File watcher started");
  }

  void stop() {
    running_ = false;
    if(watcher_thread_.joinable()) {
      watcher_thread_.join();
    }
    Logger::instance().info("[FILE_WATCHER] File watcher stopped");
  }

  void watch_loop(const HotReloadConfig& config) {
    while(running_) {
      check_files(config);
      std::this_thread::sleep_for(std::chrono::milliseconds(config.check_interval_ms));
    }
  }

  void check_files(const HotReloadConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    for(auto& pair : last_modified_times_) {
      const std::string& file_path     = pair.first;
      auto&              last_modified = pair.second;

      std::error_code ec;
      auto            fs_time = fs::last_write_time(file_path, ec);

      // Convert filesystem time to system time
      auto current_time = std::chrono::system_clock::now();
      auto fs_time_sys  = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          fs_time - std::filesystem::file_time_type::clock::now() + current_time);

      if(!ec && fs_time_sys > last_modified) {
        last_modified = fs_time_sys;

        FileChange change;
        change.file_path  = file_path;
        change.event_type = FileChangeEvent::MODIFIED;
        change.timestamp  = current_time;

        Logger::instance().info_f("[FILE_WATCHER] File changed: %s", file_path.c_str());

        manager_->on_file_changed(change);

        // Auto reload if enabled
        if(config.auto_reload && config.enabled) {
          Logger::instance().info_f("[HOT_RELOAD] Auto reloading: %s", file_path.c_str());
          auto result = manager_->reload(file_path);
          if(result.success) {
            Logger::instance().info_f("[HOT_RELOAD] Reload success: %s, Version=%s",
                                      file_path.c_str(), result.version_id.c_str());
          } else {
            Logger::instance().error_f("[HOT_RELOAD] Reload failed: %s, Error=%s",
                                       file_path.c_str(), result.message.c_str());
          }
        }
      }
    }
  }

  void add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::error_code ec;
    auto            fs_time = fs::last_write_time(file_path, ec);
    if(!ec) {
      auto current_time = std::chrono::system_clock::now();
      auto sys_time     = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          fs_time - std::filesystem::file_time_type::clock::now() + current_time);
      last_modified_times_[file_path] = sys_time;
      Logger::instance().info_f("[FILE_WATCHER] Started watching: %s", file_path.c_str());
    }
  }

  void remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_modified_times_.erase(file_path);
    Logger::instance().info_f("[FILE_WATCHER] Stopped watching: %s", file_path.c_str());
  }

  bool is_watching(const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_modified_times_.find(file_path) != last_modified_times_.end();
  }

  size_t get_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_modified_times_.size();
  }
};

// ============================================================================
// VersionManagerImpl - Version management implementation
// ============================================================================

struct HotReloadManager::VersionManagerImpl {
  struct VersionHistory {
    std::vector<std::shared_ptr<ExecutableVersion>> versions;
    size_t                                          max_versions;

    VersionHistory(size_t max_vers = 5) : max_versions(max_vers) {}

    void add_version(std::shared_ptr<ExecutableVersion> version) {
      // Mark all existing versions as inactive
      for(auto& v : versions) {
        v->is_active = false;
      }

      // Add new version as active
      version->is_active = true;
      versions.push_back(version);

      // Keep only max_versions
      while(versions.size() > max_versions) {
        versions.erase(versions.begin());
      }
    }

    std::shared_ptr<ExecutableVersion> get_active() const {
      for(auto it = versions.rbegin(); it != versions.rend(); ++it) {
        if((*it)->is_active) {
          return *it;
        }
      }
      return nullptr;
    }

    std::shared_ptr<ExecutableVersion> find_version(const std::string& version_id) const {
      for(const auto& v : versions) {
        if(v->version_id == version_id) {
          return v;
        }
      }
      return nullptr;
    }
  };

  std::map<std::string, VersionHistory> file_versions_;
  mutable std::mutex                    mutex_;
  std::atomic<uint64_t>                 version_counter_{ 0 };

  void add_version(const std::string& file_path, std::shared_ptr<ExecutableVersion> version,
                   size_t max_versions) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& history        = file_versions_[file_path];
    history.max_versions = max_versions;
    history.add_version(version);
  }

  std::shared_ptr<ExecutableVersion> get_current_version(const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_versions_.find(file_path);
    if(it != file_versions_.end()) {
      return it->second.get_active();
    }
    return nullptr;
  }

  std::vector<std::shared_ptr<ExecutableVersion>> get_all_versions(
      const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_versions_.find(file_path);
    if(it != file_versions_.end()) {
      return it->second.versions;
    }
    return {};
  }

  bool rollback_to_version(const std::string& file_path, const std::string& version_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_versions_.find(file_path);
    if(it == file_versions_.end()) {
      return false;
    }

    auto& history = it->second;
    auto  target  = history.find_version(version_id);
    if(!target) {
      return false;
    }

    // Mark all versions as inactive
    for(auto& v : history.versions) {
      v->is_active = false;
    }

    // Mark target as active
    target->is_active = true;

    Logger::instance().info_f("[VERSION_MANAGER] Rolled back %s to version %s", file_path.c_str(),
                              version_id.c_str());
    return true;
  }

  std::string generate_version_id() { return "v" + std::to_string(++version_counter_); }
};

// ============================================================================
// HotReloadManager implementation
// ============================================================================

HotReloadManager::HotReloadManager()
    : status_(HotReloadStatus::IDLE),
      watcher_impl_(std::make_unique<FileWatcherImpl>(this)),
      version_manager_impl_(std::make_unique<VersionManagerImpl>()) {
}

HotReloadManager::~HotReloadManager() {
  if(status_ == HotReloadStatus::WATCHING) {
    watcher_impl_->stop();
  }
}

void HotReloadManager::set_config(const HotReloadConfig& config) {
  config_ = config;

  if(config_.enabled && status_ == HotReloadStatus::IDLE) {
    status_ = HotReloadStatus::WATCHING;
    watcher_impl_->start(config_);
    Logger::instance().info("[HOT_RELOAD] Hot reload enabled");
  } else if(!config_.enabled && status_ == HotReloadStatus::WATCHING) {
    watcher_impl_->stop();
    status_ = HotReloadStatus::IDLE;
    Logger::instance().info("[HOT_RELOAD] Hot reload disabled");
  }
}

bool HotReloadManager::start_watching(const std::string& file_path) {
  if(!fs::exists(file_path)) {
    Logger::instance().error_f("[HOT_RELOAD] File not found: %s", file_path.c_str());
    return false;
  }

  watcher_impl_->add_file(file_path);

  // Create initial version
  auto version        = std::make_shared<ExecutableVersion>();
  version->version_id = version_manager_impl_->generate_version_id();
  version->file_path  = file_path;
  version->checksum   = calculate_checksum(file_path);

  std::error_code ec;
  version->file_size     = fs::file_size(file_path, ec);
  auto fs_time           = fs::last_write_time(file_path, ec);
  auto current_time      = std::chrono::system_clock::now();
  version->modified_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      fs_time - std::filesystem::file_time_type::clock::now() + current_time);

  version_manager_impl_->add_version(file_path, version, config_.max_versions);

  Logger::instance().info_f("[HOT_RELOAD] Started watching: %s (Version=%s)", file_path.c_str(),
                            version->version_id.c_str());

  return true;
}

bool HotReloadManager::stop_watching(const std::string& file_path) {
  watcher_impl_->remove_file(file_path);
  return true;
}

bool HotReloadManager::is_watching(const std::string& file_path) const {
  return watcher_impl_->is_watching(file_path);
}

void HotReloadManager::register_file_change_callback(FileChangeCallback callback) {
  file_change_callbacks_.push_back(callback);
}

std::shared_ptr<ExecutableVersion> HotReloadManager::get_current_version(
    const std::string& file_path) const {
  return version_manager_impl_->get_current_version(file_path);
}

std::vector<std::shared_ptr<ExecutableVersion>> HotReloadManager::get_all_versions(
    const std::string& file_path) const {
  return version_manager_impl_->get_all_versions(file_path);
}

bool HotReloadManager::rollback_to_version(const std::string& file_path,
                                           const std::string& version_id) {
  return version_manager_impl_->rollback_to_version(file_path, version_id);
}

ReloadResult HotReloadManager::reload(const std::string& file_path) {
  auto start_time = std::chrono::steady_clock::now();

  Logger::instance().info_f("[HOT_RELOAD] Reloading: %s", file_path.c_str());

  ReloadResult result;

  // Validate file if required
  if(config_.validate_before_reload) {
    if(!validate_file(file_path)) {
      result.success = false;
      result.message = "File validation failed";
      Logger::instance().error_f("[HOT_RELOAD] Validation failed: %s", file_path.c_str());
      return result;
    }
  }

  // Create backup if required
  if(config_.backup_before_reload) {
    std::string backup_path =
        config_.backup_dir.empty()
            ? file_path + ".bak"
            : config_.backup_dir + "/" + fs::path(file_path).filename().string() + ".bak";

    if(!create_backup(file_path, backup_path)) {
      Logger::instance().warn_f("[HOT_RELOAD] Failed to create backup: %s", backup_path.c_str());
    }
  }

  // Create new version
  auto version        = std::make_shared<ExecutableVersion>();
  version->version_id = version_manager_impl_->generate_version_id();
  version->file_path  = file_path;
  version->checksum   = calculate_checksum(file_path);

  std::error_code ec;
  version->file_size     = fs::file_size(file_path, ec);
  auto fs_time           = fs::last_write_time(file_path, ec);
  auto current_time      = std::chrono::system_clock::now();
  version->modified_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      fs_time - std::filesystem::file_time_type::clock::now() + current_time);

  version_manager_impl_->add_version(file_path, version, config_.max_versions);

  result.success    = true;
  result.version_id = version->version_id;
  result.message    = "Reload successful";

  auto end_time   = std::chrono::steady_clock::now();
  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  Logger::instance().info_f("[HOT_RELOAD] Reload complete: %s, Version=%s, Duration=%ldms",
                            file_path.c_str(), version->version_id.c_str(),
                            result.duration.count());

  return result;
}

void HotReloadManager::reload_async(const std::string& file_path, ReloadCallback callback) {
  std::thread([this, file_path, callback]() {
    auto result = reload(file_path);
    if(callback) {
      callback(result);
    }
  }).detach();
}

size_t HotReloadManager::get_watched_files_count() const {
  return watcher_impl_->get_count();
}

void HotReloadManager::on_file_changed(const FileChange& change) {
  for(const auto& callback : file_change_callbacks_) {
    try {
      callback(change);
    } catch(const std::exception& e) {
      Logger::instance().error_f("[HOT_RELOAD] Callback exception: %s", e.what());
    } catch(...) {
      Logger::instance().error("[HOT_RELOAD] Callback: unknown exception");
    }
  }
}

std::string HotReloadManager::calculate_checksum(const std::string& file_path) const {
  // Simple checksum implementation (can be replaced with MD5/SHA256)
  std::ifstream file(file_path, std::ios::binary);
  if(!file) {
    return "";
  }

  uint32_t checksum = 0;
  char     buffer[4096];

  while(file.read(buffer, sizeof(buffer))) {
    for(size_t i = 0; i < file.gcount(); ++i) {
      checksum = (checksum << 8) | (checksum >> 24);
      checksum += static_cast<uint8_t>(buffer[i]);
    }
  }

  // Handle remaining bytes
  for(size_t i = 0; i < file.gcount(); ++i) {
    checksum = (checksum << 8) | (checksum >> 24);
    checksum += static_cast<uint8_t>(buffer[i]);
  }

  char checksum_str[16];
  snprintf(checksum_str, sizeof(checksum_str), "%08x", checksum);
  return std::string(checksum_str);
}

bool HotReloadManager::validate_file(const std::string& file_path) const {
  // Basic file validation
  std::error_code ec;
  if(!fs::exists(file_path, ec)) {
    return false;
  }

  auto file_size = fs::file_size(file_path, ec);
  if(ec || file_size == 0) {
    return false;
  }

  // Check if file is readable
  std::ifstream file(file_path, std::ios::binary);
  if(!file) {
    return false;
  }

  return true;
}

bool HotReloadManager::create_backup(const std::string& file_path,
                                     const std::string& backup_path) const {
  try {
    // Create backup directory if it doesn't exist
    fs::create_directories(fs::path(backup_path).parent_path());

    // Copy file
    fs::copy_file(file_path, backup_path, fs::copy_options::overwrite_existing);

    Logger::instance().info_f("[HOT_RELOAD] Backup created: %s -> %s", file_path.c_str(),
                              backup_path.c_str());
    return true;
  } catch(const std::exception& e) {
    Logger::instance().error_f("[HOT_RELOAD] Backup failed: %s", e.what());
    return false;
  }
}

// ============================================================================
// Global hot reload manager instance
// ============================================================================

namespace {
std::unique_ptr<HotReloadManager> g_global_hot_reload_manager;
std::mutex                        g_global_manager_mutex;
}  // namespace

HotReloadManager& global_hot_reload_manager() {
  std::lock_guard<std::mutex> lock(g_global_manager_mutex);

  if(!g_global_hot_reload_manager) {
    g_global_hot_reload_manager = std::make_unique<HotReloadManager>();
  }

  return *g_global_hot_reload_manager;
}

}  // namespace runtime
}  // namespace agentflow
