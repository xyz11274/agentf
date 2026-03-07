#include "agent/package.h"

#include <sys/time.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
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

bool parse_task_definition(const rapidjson::Value& task_value, int expected_index,
                           AgentPackage::TaskDefinition& task_definition,
                           bool allow_contextupdater = false) {
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

  const bool is_session_task = task_definition.task_type == "dispatcher"
                               || task_definition.task_type == "coordinator"
                               || task_definition.task_type == "skill";
  if(!is_session_task && !(allow_contextupdater && task_definition.task_type == "contextupdater")) {
    return fail_agent_json_parse(
        "task.type must be dispatcher, coordinator, skill, or top-level contextupdater");
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
    task_definition.skill.instance_pool_size = 2;
    if(skill_object.HasMember("instance_pool_size")
       && (!parse_int_member(skill_object, "instance_pool_size",
                             task_definition.skill.instance_pool_size)
           || task_definition.skill.instance_pool_size < 1)) {
      return fail_agent_json_parse("skill.instance_pool_size must be an integer >= 1");
    }
  }

  return true;
}

bool parse_session_definition(const rapidjson::Value& session_value, int expected_index,
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
  if(!parse_string_member(session_value, "description", session_definition.description)
      || session_definition.description.empty()) {
    return fail_agent_json_parse("session.description must be a non-empty string");
  }
  if(!parse_int_member(session_value, "task_count", session_definition.task_count)
      || session_definition.task_count < 3) {
    return fail_agent_json_parse("session.task_count must be an integer >= 3");
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
  int skill_count = 0;
  for(rapidjson::SizeType i = 0; i < tasks.Size(); ++i) {
    AgentPackage::TaskDefinition task_definition;
    if(!parse_task_definition(tasks[i], static_cast<int>(i), task_definition, false)) {
      return false;
    }
    if(task_definition.task_type == "dispatcher") {
      ++dispatcher_count;
    } else if(task_definition.task_type == "coordinator") {
      ++coordinator_count;
    } else if(task_definition.task_type == "skill") {
      ++skill_count;
    }
    session_definition.tasks.push_back(task_definition);
  }

  if(dispatcher_count != 1) {
    return fail_agent_json_parse("each session must define exactly one dispatcher task");
  }
  if(coordinator_count != 1) {
    return fail_agent_json_parse("each session must define exactly one coordinator task");
  }
  if(skill_count < 1) {
    return fail_agent_json_parse("each session must define at least one skill task");
  }
  return true;
}

bool parse_agent_sessions(const rapidjson::Document& agent_doc,
                          std::vector<AgentPackage::SessionDefinition>& sessions,
                          int& session_pool_size) {
  if(!agent_doc.HasMember("sessions")) {
    return true;
  }
  if(!agent_doc["sessions"].IsArray()) {
    return fail_agent_json_parse("sessions must be an array");
  }

  const auto& session_array = agent_doc["sessions"];
  if(session_array.Size() != 1) {
    return fail_agent_json_parse("sessions must contain exactly one homogeneous session template");
  }
  AgentPackage::SessionDefinition session_definition;
  if(!parse_session_definition(session_array[0], 0, session_definition)) {
    return false;
  }
  sessions.push_back(session_definition);

  if(agent_doc.HasMember("session_pool_size")) {
    if(!agent_doc["session_pool_size"].IsInt()) {
      return fail_agent_json_parse("session_pool_size must be an integer");
    }
    session_pool_size = agent_doc["session_pool_size"].GetInt();
    if(session_pool_size < 1) {
      return fail_agent_json_parse("session_pool_size must be >= 1");
    }
  } else {
    session_pool_size = 1;
  }
  return true;
}

bool parse_top_level_tasks(const rapidjson::Document& agent_doc,
                            AgentPackage::TaskDefinition& task_definition,
                            bool& has_contextupdater) {
  has_contextupdater = false;
  if(agent_doc.HasMember("tasks")) {
    if(!agent_doc["tasks"].IsArray()) {
      return fail_agent_json_parse("tasks must be an array");
    }
    const auto& tasks = agent_doc["tasks"];
    for(rapidjson::SizeType i = 0; i < tasks.Size(); ++i) {
      AgentPackage::TaskDefinition parsed_task;
      if(!parse_task_definition(tasks[i], static_cast<int>(i), parsed_task, true)) {
        return false;
      }
      if(parsed_task.task_type != "contextupdater") {
        return fail_agent_json_parse("top-level tasks currently only support contextupdater");
      }
      if(has_contextupdater) {
        return fail_agent_json_parse("top-level tasks must not contain more than one contextupdater");
      }
      task_definition = parsed_task;
      has_contextupdater = true;
    }
    return true;
  }

  if(agent_doc.HasMember("contextupdater")) {
    if(!agent_doc["contextupdater"].IsObject()) {
      return fail_agent_json_parse("contextupdater must be an object");
    }
    task_definition.task_index = 0;
    if(!parse_task_definition(agent_doc["contextupdater"], 0, task_definition, true)) {
      return false;
    }
    if(task_definition.task_type != "contextupdater") {
      return fail_agent_json_parse("contextupdater.type must be contextupdater");
    }
    has_contextupdater = true;
  }
  return true;
}

bool parse_agent_limits(const rapidjson::Document& agent_doc, AgentPackage::Config& config) {
  config.max_workflows = 10;
  if(agent_doc.HasMember("max_workflows")) {
    if(!agent_doc["max_workflows"].IsInt()) {
      return fail_agent_json_parse("max_workflows must be an integer");
    }
    config.max_workflows = agent_doc["max_workflows"].GetInt();
    if(config.max_workflows < 1) {
      return fail_agent_json_parse("max_workflows must be >= 1");
    }
  }

  config.max_concurrent_workflows = 5;
  if(agent_doc.HasMember("max_concurrent_workflows")) {
    if(!agent_doc["max_concurrent_workflows"].IsInt()) {
      return fail_agent_json_parse("max_concurrent_workflows must be an integer");
    }
    config.max_concurrent_workflows = agent_doc["max_concurrent_workflows"].GetInt();
    if(config.max_concurrent_workflows < 1) {
      return fail_agent_json_parse("max_concurrent_workflows must be >= 1");
    }
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

  if(!fs::exists(root_path) || !fs::is_directory(root_path)) {
    std::cerr << "Package directory not found: " << package_path << std::endl;
    return nullptr;
  }

  fs::path package_json_path = root_path / "package.json";
  if(!fs::exists(package_json_path)) {
    std::cerr << "package.json not found in: " << package_path << std::endl;
    return nullptr;
  }

  std::ifstream ifs(package_json_path);
  if(!ifs.is_open()) {
    std::cerr << "Failed to open package.json: " << package_json_path << std::endl;
    return nullptr;
  }

  rapidjson::IStreamWrapper isw(ifs);
  rapidjson::Document doc;
  doc.ParseStream(isw);
  if(doc.HasParseError()) {
    std::cerr << "Failed to parse package.json" << std::endl;
    return nullptr;
  }

  Config config;
  config.package_root = fs::absolute(root_path).string();

  if(doc.HasMember("name") && doc["name"].IsString()) {
    config.package_name = doc["name"].GetString();
    config.package_id = config.package_name;
  }
  if(doc.HasMember("version") && doc["version"].IsString()) {
    config.version = doc["version"].GetString();
  }
  if(doc.HasMember("description") && doc["description"].IsString()) {
    config.description = doc["description"].GetString();
  }
  if(doc.HasMember("author") && doc["author"].IsString()) {
    config.author = doc["author"].GetString();
  }
  if(doc.HasMember("entry_point") && doc["entry_point"].IsString()) {
    config.entry_point = doc["entry_point"].GetString();
  }

  if(doc.HasMember("workflows") && doc["workflows"].IsArray()) {
    const rapidjson::Value& workflows = doc["workflows"];
    for(rapidjson::SizeType i = 0; i < workflows.Size(); ++i) {
      if(workflows[i].IsString()) {
        config.workflow_files.push_back(workflows[i].GetString());
      }
    }
  } else if(fs::exists(root_path / "workflow.json")) {
    config.workflow_files.push_back("workflow.json");
  }

  if(doc.HasMember("builtin_workflows") && doc["builtin_workflows"].IsArray()) {
    const rapidjson::Value& builtins = doc["builtin_workflows"];
    for(rapidjson::SizeType i = 0; i < builtins.Size(); ++i) {
      if(!builtins[i].IsString()) {
        continue;
      }
      const std::string builtin_name = builtins[i].GetString();
      config.builtin_workflows.push_back(builtin_name);
      const std::string builtin_file = resolve_builtin_workflow_file(root_path, builtin_name);
      if(!builtin_file.empty()) {
        config.workflow_files.push_back(builtin_file);
      }
    }
  }

  if(doc.HasMember("dispatcher_workflow") && doc["dispatcher_workflow"].IsString()) {
    config.dispatcher_workflow = doc["dispatcher_workflow"].GetString();
  }
  if(doc.HasMember("coordinator_workflow") && doc["coordinator_workflow"].IsString()) {
    config.coordinator_workflow = doc["coordinator_workflow"].GetString();
  }
  if(doc.HasMember("task_workflows") && doc["task_workflows"].IsObject()) {
    for(auto& m : doc["task_workflows"].GetObject()) {
      if(m.value.IsString()) {
        config.task_workflows[m.name.GetString()] = m.value.GetString();
      }
    }
  }
  if(doc.HasMember("agent_context_workflow") && doc["agent_context_workflow"].IsString()) {
    config.agent_context_workflow = doc["agent_context_workflow"].GetString();
  }
  if(doc.HasMember("session_context_workflow") && doc["session_context_workflow"].IsString()) {
    config.session_context_workflow = doc["session_context_workflow"].GetString();
  }
  if(doc.HasMember("task_context_workflow") && doc["task_context_workflow"].IsString()) {
    config.task_context_workflow = doc["task_context_workflow"].GetString();
  }
  if(doc.HasMember("node_context_workflow") && doc["node_context_workflow"].IsString()) {
    config.node_context_workflow = doc["node_context_workflow"].GetString();
  }

  const fs::path agent_json_path = root_path / "agent.json";
  if(fs::exists(agent_json_path)) {
    std::ifstream agent_ifs(agent_json_path);
    if(!agent_ifs.is_open()) {
      std::cerr << "Failed to open agent.json: " << agent_json_path << std::endl;
      return nullptr;
    }

    rapidjson::IStreamWrapper agent_isw(agent_ifs);
    rapidjson::Document agent_doc;
    agent_doc.ParseStream(agent_isw);
    if(agent_doc.HasParseError()) {
      std::cerr << "Failed to parse agent.json" << std::endl;
      return nullptr;
    }

    if(!parse_agent_sessions(agent_doc, config.sessions, config.session_pool_size)) {
      return nullptr;
    }
    if(!parse_agent_limits(agent_doc, config)) {
      return nullptr;
    }
    if(!parse_top_level_tasks(agent_doc, config.contextupdater_task, config.has_contextupdater)) {
      return nullptr;
    }
    if(agent_doc.HasMember("dispatcher_workflow") && agent_doc["dispatcher_workflow"].IsString()) {
      config.dispatcher_workflow = agent_doc["dispatcher_workflow"].GetString();
    }
    if(agent_doc.HasMember("coordinator_workflow") && agent_doc["coordinator_workflow"].IsString()) {
      config.coordinator_workflow = agent_doc["coordinator_workflow"].GetString();
    }
  }

  {
    std::unordered_set<std::string> seen;
    std::vector<std::string> unique_files;
    for(const auto& wf : config.workflow_files) {
      if(seen.insert(wf).second) {
        unique_files.push_back(wf);
      }
    }
    config.workflow_files.swap(unique_files);
  }

  auto package = std::make_shared<AgentPackage>(config);

  if(doc.HasMember("dependencies") && doc["dependencies"].IsObject()) {
    for(auto& m : doc["dependencies"].GetObject()) {
      if(m.value.IsString()) {
        Dependency dep;
        dep.name = m.name.GetString();
        dep.version = m.value.GetString();
        package->add_dependency(dep);
      }
    }
  }

  if(doc.HasMember("metadata") && doc["metadata"].IsObject()) {
    for(auto& m : doc["metadata"].GetObject()) {
      if(m.value.IsString()) {
        package->set_metadata(m.name.GetString(), m.value.GetString());
      }
    }
  }

  package->state_ = PackageState::DEPLOYED;
  return package;
}

AgentPackage::AgentPackage(const Config& config) : config_(config), state_(PackageState::CREATED) {
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
  const auto it = std::find(resources_.begin(), resources_.end(), resource_path);
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
  const auto it = std::find_if(dependencies_.begin(), dependencies_.end(),
                               [&dep_name](const Dependency& dep) { return dep.name == dep_name; });
  if(it == dependencies_.end()) {
    return false;
  }
  dependencies_.erase(it);
  return true;
}

bool AgentPackage::validate_dependencies() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !dependencies_.empty() || true;
}

void AgentPackage::set_metadata(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_[key] = value;
}

bool AgentPackage::get_metadata(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = metadata_.find(key);
  if(it == metadata_.end()) {
    return false;
  }
  value = it->second;
  return true;
}

int64_t AgentPackage::get_package_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return 0;
}

}  // namespace framework
}  // namespace agentflow
