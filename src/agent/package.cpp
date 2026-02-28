#include "agent/package.h"

#include <sys/time.h>
#include <mutex>
#include <algorithm>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

std::string resolve_builtin_workflow_file(const std::filesystem::path& root_path,
                                          const std::string& builtin_name) {
  namespace fs = std::filesystem;
  const std::vector<fs::path> candidates = {
      root_path / "workflows" / "builtin" / builtin_name / "workflow.json",
      root_path / "publish" / "workflow" / builtin_name / "workflow.json",
      root_path / ".." / "publish" / "workflow" / builtin_name / "workflow.json",
      root_path / ".." / ".." / "publish" / "workflow" / builtin_name / "workflow.json",
      fs::current_path() / "publish" / "workflow" / builtin_name / "workflow.json",
      fs::current_path() / ".." / "publish" / "workflow" / builtin_name / "workflow.json",
      fs::current_path() / ".." / ".." / "publish" / "workflow" / builtin_name / "workflow.json"};
  for(const auto& candidate : candidates) {
    if(fs::exists(candidate)) {
      return fs::absolute(candidate).string();
    }
  }
  return "";
}
}  // namespace

std::shared_ptr<AgentPackage> AgentPackage::load_from_directory(const std::string& package_path) {
  namespace fs = std::filesystem;
  fs::path root_path(package_path);
  
  if (!fs::exists(root_path) || !fs::is_directory(root_path)) {
    std::cerr << "Package directory not found: " << package_path << std::endl;
    return nullptr;
  }

  fs::path package_json_path = root_path / "package.json";
  if (!fs::exists(package_json_path)) {
    std::cerr << "package.json not found in: " << package_path << std::endl;
    return nullptr;
  }

  std::ifstream ifs(package_json_path);
  if (!ifs.is_open()) {
    std::cerr << "Failed to open package.json: " << package_json_path << std::endl;
    return nullptr;
  }

  rapidjson::IStreamWrapper isw(ifs);
  rapidjson::Document doc;
  doc.ParseStream(isw);

  if (doc.HasParseError()) {
    std::cerr << "Failed to parse package.json" << std::endl;
    return nullptr;
  }

  Config config;
  config.package_root = fs::absolute(root_path).string();
  
  if (doc.HasMember("name") && doc["name"].IsString()) {
    config.package_name = doc["name"].GetString();
    config.package_id = config.package_name; // Default ID to name
  }
  
  if (doc.HasMember("version") && doc["version"].IsString()) {
    config.version = doc["version"].GetString();
  }
  
  if (doc.HasMember("description") && doc["description"].IsString()) {
    config.description = doc["description"].GetString();
  }

  if (doc.HasMember("author") && doc["author"].IsString()) {
    config.author = doc["author"].GetString();
  }

  if (doc.HasMember("entry_point") && doc["entry_point"].IsString()) {
    config.entry_point = doc["entry_point"].GetString();
  }

  // Handle workflow files
  // If workflows are explicitly listed
  if (doc.HasMember("workflows") && doc["workflows"].IsArray()) {
    const rapidjson::Value& workflows = doc["workflows"];
    for (rapidjson::SizeType i = 0; i < workflows.Size(); i++) {
      if (workflows[i].IsString()) {
        config.workflow_files.push_back(workflows[i].GetString());
      }
    }
  } else {
    // Auto-discover workflow.json if present
    if (fs::exists(root_path / "workflow.json")) {
      config.workflow_files.push_back("workflow.json");
    }
  }

  // Handle builtin workflow selection (opt-in)
  if (doc.HasMember("builtin_workflows") && doc["builtin_workflows"].IsArray()) {
    const rapidjson::Value& builtins = doc["builtin_workflows"];
    for (rapidjson::SizeType i = 0; i < builtins.Size(); i++) {
      if (!builtins[i].IsString()) {
        continue;
      }
      const std::string builtin_name = builtins[i].GetString();
      config.builtin_workflows.push_back(builtin_name);
      const std::string builtin_file = resolve_builtin_workflow_file(root_path, builtin_name);
      if (!builtin_file.empty()) {
        config.workflow_files.push_back(builtin_file);
      }
    }
  }

  if (doc.HasMember("task_workflows") && doc["task_workflows"].IsObject()) {
    for (auto& m : doc["task_workflows"].GetObject()) {
      if (m.value.IsString()) {
        config.task_workflows[m.name.GetString()] = m.value.GetString();
      }
    }
  }

  // Deduplicate workflow file entries while preserving order.
  {
    std::unordered_set<std::string> seen;
    std::vector<std::string> unique_files;
    for (const auto& wf : config.workflow_files) {
      if (seen.insert(wf).second) {
        unique_files.push_back(wf);
      }
    }
    config.workflow_files.swap(unique_files);
  }

  auto package = std::make_shared<AgentPackage>(config);

  // Load dependencies
  if (doc.HasMember("dependencies") && doc["dependencies"].IsObject()) {
    for (auto& m : doc["dependencies"].GetObject()) {
      if (m.value.IsString()) {
        Dependency dep;
        dep.name = m.name.GetString();
        dep.version = m.value.GetString();
        package->add_dependency(dep);
      }
    }
  }

  // Load metadata
  if (doc.HasMember("metadata") && doc["metadata"].IsObject()) {
    for (auto& m : doc["metadata"].GetObject()) {
      if (m.value.IsString()) {
        package->set_metadata(m.name.GetString(), m.value.GetString());
      }
    }
  }

  package->state_ = PackageState::DEPLOYED;
  return package;
}

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
