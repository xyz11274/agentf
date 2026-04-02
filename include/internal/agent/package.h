#ifndef AGENTFLOW_INTERNAL_AGENT_PACKAGE_H
#define AGENTFLOW_INTERNAL_AGENT_PACKAGE_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentflow {
namespace framework {

class AgentPackage {
 public:
  enum class PackageState {
    CREATED = 0,
    PACKAGED = 1,
    DEPLOYED = 2,
    ACTIVE = 3,
    ARCHIVED = 4,
  };

  struct Dependency {
    std::string name;
    std::string version;
  };

  struct SkillTaskConfig {
    std::string skill_name;
    std::string skill_config_dir;
    int         instance_pool_size = 1;
    std::vector<std::string> expected_parameters;  // Skill execution prerequisites
  };

  struct TaskDefinition {
    int         task_index = -1;
    std::string task_type;
    std::string scope;  // "agent" or "session" (default: "session")
    std::string description;
    std::string workflow_file;
    int         workflow_pool_size = 1;
    SkillTaskConfig skill;
    // Dispatcher and skill management fields
    std::vector<std::string> next_skills;       // Skills to launch (for dispatcher type)
    int         max_concurrent_skills = 10;      // Max concurrent skills for dispatcher
  };

  struct SessionDefinition {
    int                         session_index = -1;
    std::string                 description;
    int                         task_count = 0;
    std::vector<TaskDefinition> tasks;
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
    int         max_workflows = 10;
    int         max_concurrent_workflows = 5;
    int         data_bus_capacity_mb = 1;  // Default 1MB
    int         precreate_session_count = 0;
    int         session_pool_size = 0;
    // Flattened task structure (new format)
    std::vector<TaskDefinition> session_tasks;  // Tasks for session template
    bool        has_contextupdater = false;
    TaskDefinition               contextupdater_task;
    // Legacy format (backward compatibility)
    std::vector<SessionDefinition> sessions;
  };

  explicit AgentPackage(const Config& config);
  ~AgentPackage();

  static std::shared_ptr<AgentPackage> load_from_directory(const std::string& package_path);

  const std::string& get_package_id() const { return config_.package_id; }
  const std::string& get_package_name() const { return config_.package_name; }
  const std::string& get_version() const { return config_.version; }
  PackageState       get_state() const { return state_; }
  const Config&      get_config() const { return config_; }

  bool create();
  bool package();
  bool deploy(const std::string& deployment_path);
  bool activate();
  bool archive();

  void                            add_resource(const std::string& resource_path);
  void                            remove_resource(const std::string& resource_path);
  const std::vector<std::string>& get_resources() const { return resources_; }

  void                            add_dependency(const Dependency& dependency);
  bool                            remove_dependency(const std::string& dep_name);
  const std::vector<Dependency>&  get_dependencies() const { return dependencies_; }
  bool                            validate_dependencies() const;

  void                                  set_metadata(const std::string& key, const std::string& value);
  bool                                  get_metadata(const std::string& key, std::string& value) const;
  const std::map<std::string, std::string>& get_all_metadata() const { return metadata_; }

  int64_t get_package_size() const;
  int64_t get_created_time() const { return created_time_; }
  bool    is_valid() const { return is_valid_; }

 private:
  Config                             config_;
  PackageState                       state_ = PackageState::CREATED;
  std::vector<std::string>           resources_;
  std::vector<Dependency>            dependencies_;
  std::map<std::string, std::string> metadata_;
  int64_t                            created_time_ = 0;
  bool                               is_valid_ = true;
  mutable std::mutex                 mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_PACKAGE_H
