#ifndef AGENTFLOW_INTERNAL_AGENT_PACKAGE_H
#define AGENTFLOW_INTERNAL_AGENT_PACKAGE_H

#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <mutex>

namespace agentflow {
namespace framework {

/**
 * AgentPackage manages agent packages and their deployment
 * Handles package metadata, versioning, and resource bundling
 */
class AgentPackage {
public:
  enum class PackageState {
    CREATED     = 0,
    PACKAGED    = 1,
    DEPLOYED    = 2,
    ACTIVE      = 3,
    ARCHIVED    = 4,
  };

  struct Dependency {
    std::string name;
    std::string version;
  };

  struct Config {
    std::string package_id;
    std::string package_name;
    std::string version = "1.0.0";
    std::string description;
    std::string author;
    std::string entry_point;
    std::string package_root;
    std::vector<std::string> workflow_files;
    std::vector<std::string> builtin_workflows;
    std::string coordinator_workflow;
    std::map<std::string, std::string> task_workflows;
  };

  explicit AgentPackage(const Config& config);
  ~AgentPackage();

  // Static factory method
  static std::shared_ptr<AgentPackage> load_from_directory(const std::string& package_path);

  // Getters
  const std::string& get_package_id() const { return config_.package_id; }
  const std::string& get_package_name() const { return config_.package_name; }
  const std::string& get_version() const { return config_.version; }
  PackageState       get_state() const { return state_; }
  const Config&      get_config() const { return config_; }

  // Package lifecycle
  bool create();
  bool package();
  bool deploy(const std::string& deployment_path);
  bool activate();
  bool archive();

  // Resource management
  void                      add_resource(const std::string& resource_path);
  void                      remove_resource(const std::string& resource_path);
  const std::vector<std::string>& get_resources() const { return resources_; }

  // Dependency management
  void                      add_dependency(const Dependency& dependency);
  bool                      remove_dependency(const std::string& dep_name);
  const std::vector<Dependency>& get_dependencies() const { return dependencies_; }
  bool                      validate_dependencies() const;

  // Metadata
  void       set_metadata(const std::string& key, const std::string& value);
  bool       get_metadata(const std::string& key, std::string& value) const;
  const std::map<std::string, std::string>& get_all_metadata() const { return metadata_; }

  // Package information
  int64_t    get_package_size() const;
  int64_t    get_created_time() const { return created_time_; }
  bool       is_valid() const { return is_valid_; }

private:
  Config                                      config_;
  PackageState                                state_ = PackageState::CREATED;
  std::vector<std::string>                    resources_;
  std::vector<Dependency>                     dependencies_;
  std::map<std::string, std::string>          metadata_;
  int64_t                                     created_time_ = 0;
  bool                                        is_valid_ = true;
  mutable std::mutex                          mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_PACKAGE_H
