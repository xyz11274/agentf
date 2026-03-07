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

bool parse_string_member(const rapidjson::Value& object, const char* name, std::string& value) {
  if(!object.HasMember(name)) {
    return false;
  }
  const auto& member = object[name];
  if(!member.IsString()) {
    return false;
  }
  value = member.GetString();
  return true;
}

bool parse_int_member(const rapidjson::Value& object, const char* name, int& value) {
  if(!object.HasMember(name)) {
    return false;
  }
  const auto& member = object[name];
  if(!member.IsInt()) {
    return false;
  }
  value = member.GetInt();
  return true;
}

bool fail_agent_json_parse(const std::string& message) {
  std::cerr << "Invalid agent.json: " << message << std::endl;
  return false;
}

bool parse_task_definition(const rapidjson::Value& task_value,
                           int expected_index,
                           AgentPackage::TaskDefinition& task_definition) {
  if(!task_value.IsObject()) {
    return fail_agent_json_parse("task entries must be objects");
  }

  if(!task_value.HasMember("id") || !task_value["id"].IsInt()) {
    return fail_agent_json_parse("task.id must be an integer");
  }
  task_definition.task_index = task_value["id"].GetInt();
  if(task_definition.task_index != expected_index) {
    return fail_agent_json_parse("task ids must be contiguous and start from 0");
  }

  if(!parse_string_member(task_value, "type", task_definition.task_type)) {
    return fail_agent_json_parse("task.type must be a string");
  }
  if(task_definition.task_type != "dispatcher" && task_definition.task_type != "coordinator"
      && task_definition.task_type != "contextkeeper" && task_definition.task_type != "skill") {
    return fail_agent_json_parse("task.type must be dispatcher, coordinator, contextkeeper, or skill");
  }

  if(!parse_string_member(task_value, "description", task_definition.description)
      || task_definition.description.empty()) {
    return fail_agent_json_parse("task.description must be a non-empty string");
  }

  if(!parse_string_member(task_value, "workflow_file", task_definition.workflow_file)
      || task_definition.workflow_file.empty()) {
    return fail_agent_json_parse("task.workflow_file must be a non-empty string");
  }

  if(task_definition.task_type == "skill") {
    if(!task_value.HasMember("skill") || !task_value["skill"].IsObject()) {
      return fail_agent_json_parse("skill tasks must define a skill object");
    }
    const auto& skill_object = task_value["skill"];
    if(!parse_string_member(skill_object, "name", task_definition.skill.skill_name)
        || task_definition.skill.skill_name.empty()) {
      return fail_agent_json_parse("skill.name must be a non-empty string");
    }
    if(!parse_string_member(skill_object, "config_dir", task_definition.skill.skill_config_dir)
        || task_definition.skill.skill_config_dir.empty()) {
      return fail_agent_json_parse("skill.config_dir must be a non-empty string");
    }
    if(!parse_int_member(skill_object, "instance_pool_size", task_definition.skill.instance_pool_size)
        || task_definition.skill.instance_pool_size < 1) {
      return fail_agent_json_parse("skill.instance_pool_size must be an integer >= 1");
    }
  }

  return true;
}

bool parse_session_definition(const rapidjson::Value& session_value,
                              int expected_index,
                              AgentPackage::SessionDefinition& session_definition) {
  if(!session_value.IsObject()) {
    return fail_agent_json_parse("session entries must be objects");
  }

  if(!session_value.HasMember("id") || !session_value["id"].IsInt()) {
    return fail_agent_json_parse("session.id must be an integer");
  }
  session_definition.session_index = session_value["id"].GetInt();
  if(session_definition.session_index != expected_index) {
    return fail_agent_json_parse("session ids must be contiguous and start from 0");
  }

  if(!parse_string_member(session_value, "type", session_definition.session_type)) {
    return fail_agent_json_parse("session.type must be a string");
  }
  if(session_definition.session_type != "talker" && session_definition.session_type != "thinker") {
    return fail_agent_json_parse("session.type must be talker or thinker");
  }

  if(!parse_string_member(session_value, "description", session_definition.description)
      || session_definition.description.empty()) {
    return fail_agent_json_parse("session.description must be a non-empty string");
  }

  if(!parse_int_member(session_value, "task_count", session_definition.task_count)
      || session_definition.task_count < 1) {
    return fail_agent_json_parse("session.task_count must be an integer >= 1");
  }
  if(!session_value.HasMember("tasks") || !session_value["tasks"].IsArray()) {
    return fail_agent_json_parse("session.tasks must be an array");
  }

  const auto& tasks = session_value["tasks"];
  if(static_cast<int>(tasks.Size()) != session_definition.task_count) {
    return fail_agent_json_parse("session.task_count must match the number of tasks");
  }

  int dispatcher_count = 0;
  int coordinator_count = 0;
  int contextkeeper_count = 0;
  for(rapidjson::SizeType i = 0; i < tasks.Size(); ++i) {
    AgentPackage::TaskDefinition task_definition;
    if(!parse_task_definition(tasks[i], static_cast<int>(i), task_definition)) {
      return false;
    }
    if(task_definition.task_type == "dispatcher") {
      ++dispatcher_count;
    } else if(task_definition.task_type == "coordinator") {
      ++coordinator_count;
    } else if(task_definition.task_type == "contextkeeper") {
      ++contextkeeper_count;
    }
    session_definition.tasks.push_back(task_definition);
  }

  if(dispatcher_count != 1) {
    return fail_agent_json_parse("each session must define exactly one dispatcher task");
  }
  if(coordinator_count != 1) {
    return fail_agent_json_parse("each session must define exactly one coordinator task");
  }
  if(contextkeeper_count != 1) {
    return fail_agent_json_parse("each session must define exactly one contextkeeper task");
  }

  return true;
}

bool parse_agent_sessions(const rapidjson::Document& agent_doc,
                          std::vector<AgentPackage::SessionDefinition>& sessions) {
  if(!agent_doc.HasMember("sessions")) {
    return true;
  }
  if(!agent_doc["sessions"].IsArray()) {
    return fail_agent_json_parse("sessions must be an array");
  }

  const auto& session_array = agent_doc["sessions"];
  if(session_array.Empty()) {
    return fail_agent_json_parse("sessions must contain at least one configured session");
  }

  int talker_count = 0;
  for(rapidjson::SizeType i = 0; i < session_array.Size(); ++i) {
    AgentPackage::SessionDefinition session_definition;
    if(!parse_session_definition(session_array[i], static_cast<int>(i), session_definition)) {
      return false;
    }
    if(session_definition.session_type == "talker") {
      ++talker_count;
    }
    sessions.push_back(session_definition);
  }

  if(talker_count != 1) {
    return fail_agent_json_parse("sessions must contain exactly one talker session");
  }
  if(sessions.front().session_type != "talker") {
    return fail_agent_json_parse("session 0 must be the talker session");
  }

  return true;
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

  if (doc.HasMember("dispatcher_workflow") && doc["dispatcher_workflow"].IsString()) {
    config.dispatcher_workflow = doc["dispatcher_workflow"].GetString();
  }
  if (doc.HasMember("coordinator_workflow") && doc["coordinator_workflow"].IsString()) {
    config.coordinator_workflow = doc["coordinator_workflow"].GetString();
  }

  if (doc.HasMember("task_workflows") && doc["task_workflows"].IsObject()) {
    for (auto& m : doc["task_workflows"].GetObject()) {
      if (m.value.IsString()) {
        config.task_workflows[m.name.GetString()] = m.value.GetString();
      }
    }
  }
  if (doc.HasMember("agent_context_workflow") && doc["agent_context_workflow"].IsString()) {
    config.agent_context_workflow = doc["agent_context_workflow"].GetString();
  }
  if (doc.HasMember("session_context_workflow") && doc["session_context_workflow"].IsString()) {
    config.session_context_workflow = doc["session_context_workflow"].GetString();
  }
  if (doc.HasMember("task_context_workflow") && doc["task_context_workflow"].IsString()) {
    config.task_context_workflow = doc["task_context_workflow"].GetString();
  }
  if (doc.HasMember("node_context_workflow") && doc["node_context_workflow"].IsString()) {
    config.node_context_workflow = doc["node_context_workflow"].GetString();
  }

  // precreate_session_count and structured session topology are sourced from agent.json.
  fs::path agent_json_path = root_path / "agent.json";
  if (fs::exists(agent_json_path)) {
    std::ifstream agent_ifs(agent_json_path);
    if (!agent_ifs.is_open()) {
      std::cerr << "Failed to open agent.json: " << agent_json_path << std::endl;
      return nullptr;
    }

    rapidjson::IStreamWrapper agent_isw(agent_ifs);
    rapidjson::Document agent_doc;
    agent_doc.ParseStream(agent_isw);
    if (agent_doc.HasParseError()) {
      std::cerr << "Failed to parse agent.json" << std::endl;
      return nullptr;
    }

    if(!parse_agent_sessions(agent_doc, config.sessions)) {
      return nullptr;
    }

    if (agent_doc.HasMember("precreate_session_count")
        && agent_doc["precreate_session_count"].IsInt()) {
      config.precreate_session_count = std::max(0, agent_doc["precreate_session_count"].GetInt());
    }
    if (agent_doc.HasMember("dispatcher_workflow")
        && agent_doc["dispatcher_workflow"].IsString()) {
      config.dispatcher_workflow = agent_doc["dispatcher_workflow"].GetString();
    }
    if (agent_doc.HasMember("coordinator_workflow")
        && agent_doc["coordinator_workflow"].IsString()) {
      config.coordinator_workflow = agent_doc["coordinator_workflow"].GetString();
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
