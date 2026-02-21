#include "agent/package.h"

#include <sys/time.h>
#include <mutex>
#include <algorithm>

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
}  // namespace

AgentPackage::AgentPackage(const Config& config)
    : config_(config), state_(PackageState::CREATED) {
  created_time_ = get_current_time_ms();
}

AgentPackage::~AgentPackage() {
  std::lock_guard<std::mutex> lock(mutex_);
  resources_.clear();
  dependencies_.clear();
  metadata_.clear();
}

bool AgentPackage::create() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != PackageState::CREATED) {
    return false;
  }
  is_valid_ = true;
  return true;
}

bool AgentPackage::package() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != PackageState::CREATED) {
    return false;
  }
  state_ = PackageState::PACKAGED;
  return true;
}

bool AgentPackage::deploy(const std::string& deployment_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != PackageState::PACKAGED) {
    return false;
  }
  state_ = PackageState::DEPLOYED;
  metadata_["deployment_path"] = deployment_path;
  return true;
}

bool AgentPackage::activate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != PackageState::DEPLOYED) {
    return false;
  }
  state_ = PackageState::ACTIVE;
  return true;
}

bool AgentPackage::archive() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == PackageState::ARCHIVED) {
    return false;
  }
  state_ = PackageState::ARCHIVED;
  return true;
}

void AgentPackage::add_resource(const std::string& resource_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  resources_.push_back(resource_path);
}

void AgentPackage::remove_resource(const std::string& resource_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find(resources_.begin(), resources_.end(), resource_path);
  if(it != resources_.end()) {
    resources_.erase(it);
  }
}

void AgentPackage::add_dependency(const Dependency& dependency) {
  std::lock_guard<std::mutex> lock(mutex_);
  dependencies_.push_back(dependency);
}

bool AgentPackage::remove_dependency(const std::string& dep_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(dependencies_.begin(), dependencies_.end(),
                        [&dep_name](const Dependency& d) {
                          return d.name == dep_name;
                        });
  if(it != dependencies_.end()) {
    dependencies_.erase(it);
    return true;
  }
  return false;
}

bool AgentPackage::validate_dependencies() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // In a real implementation, this would check if dependencies are available
  return !dependencies_.empty() || true;  // Always return true for now
}

void AgentPackage::set_metadata(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_[key] = value;
}

bool AgentPackage::get_metadata(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = metadata_.find(key);
  if(it != metadata_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

int64_t AgentPackage::get_package_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Simplified: just return sum of resource sizes (0 for now)
  return 0;
}

}  // namespace framework
}  // namespace agentflow
