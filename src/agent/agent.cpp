#include "agent/agent.h"

#include <sys/time.h>

#include <algorithm>
#include <sstream>

#include "utils/file_storage.h"
#include "utils/logger.h"

// Include internal agent components
#include "agent/session.h"
#include "agent/task.h"
#include "agent/messages.h"
#include "agent/context.h"
#include "agent/event.h"
#include "agent/package.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#endif

namespace agentflow {
namespace framework {

namespace storage = agentflow::utils::storage;

namespace {

/**
 * Get current timestamp in milliseconds
 */
int64_t get_current_timestamp_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * Convert Agent::Config to JSON string
 */
std::string agent_config_to_json(const Agent::Config&                            config,
                                 const std::vector<std::shared_ptr<Workflow>>& workflows) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

  // Agent config
  rapidjson::Value agent_obj(rapidjson::kObjectType);
  agent_obj.AddMember("id", rapidjson::StringRef(config.id.c_str()), allocator);
  agent_obj.AddMember("name", rapidjson::StringRef(config.name.c_str()), allocator);
  agent_obj.AddMember("version", rapidjson::StringRef(config.version.c_str()), allocator);
  agent_obj.AddMember("description", rapidjson::StringRef(config.description.c_str()), allocator);
  agent_obj.AddMember("policy", static_cast<int>(config.policy), allocator);
  agent_obj.AddMember("max_workflows", config.max_workflows, allocator);
  agent_obj.AddMember("max_concurrent_workflows", config.max_concurrent_workflows, allocator);
  agent_obj.AddMember("data_bus_capacity", config.data_bus_capacity, allocator);

  doc.AddMember("agent", agent_obj, allocator);

  // Workflows
  rapidjson::Value workflows_array(rapidjson::kArrayType);
  for(const auto& workflow : workflows) {
    rapidjson::Value workflow_obj(rapidjson::kObjectType);
    workflow_obj.AddMember("id", rapidjson::StringRef(workflow->get_id().c_str()), allocator);
    workflow_obj.AddMember("name", rapidjson::StringRef(workflow->get_name().c_str()), allocator);
    // Note: version and description are not directly accessible, skip them
    workflows_array.PushBack(workflow_obj, allocator);
  }
  doc.AddMember("workflows", workflows_array, allocator);

  rapidjson::StringBuffer                          buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  return buffer.GetString();
#else
  std::ostringstream oss;
  oss << "{" << "\"id\":\"" << config.id << "\"," << "\"name\":\"" << config.name << "\","
      << "\"version\":\"" << config.version << "\"" << "}";
  return oss.str();
#endif
}

/**
 * Parse Agent::Config from JSON string
 */
bool agent_config_from_json(const std::string& json_str, Agent::Config& config,
                            std::vector<Workflow::Config>& workflow_configs) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());

  if(doc.HasParseError() || !doc.HasMember("agent")) {
    return false;
  }

  const rapidjson::Value& agent_obj = doc["agent"];

  if(agent_obj.HasMember("id"))
    config.id = agent_obj["id"].GetString();
  if(agent_obj.HasMember("name"))
    config.name = agent_obj["name"].GetString();
  if(agent_obj.HasMember("version"))
    config.version = agent_obj["version"].GetString();
  if(agent_obj.HasMember("description"))
    config.description = agent_obj["description"].GetString();
  if(agent_obj.HasMember("policy"))
    config.policy = static_cast<Agent::LifecyclePolicy>(agent_obj["policy"].GetInt());
  if(agent_obj.HasMember("max_workflows"))
    config.max_workflows = agent_obj["max_workflows"].GetInt();
  if(agent_obj.HasMember("max_concurrent_workflows"))
    config.max_concurrent_workflows = agent_obj["max_concurrent_workflows"].GetInt();
  if(agent_obj.HasMember("data_bus_capacity"))
    config.data_bus_capacity = agent_obj["data_bus_capacity"].GetInt();

  // Parse workflows
  if(doc.HasMember("workflows") && doc["workflows"].IsArray()) {
    const rapidjson::Value& workflows_array = doc["workflows"];
    for(rapidjson::SizeType i = 0; i < workflows_array.Size(); ++i) {
      const rapidjson::Value& workflow_obj = workflows_array[i];
      Workflow::Config        wf_config;

      if(workflow_obj.HasMember("id"))
        wf_config.id = workflow_obj["id"].GetString();
      if(workflow_obj.HasMember("name"))
        wf_config.name = workflow_obj["name"].GetString();
      if(workflow_obj.HasMember("version"))
        wf_config.version = workflow_obj["version"].GetString();
      if(workflow_obj.HasMember("dsl_file"))
        wf_config.dsl_file = workflow_obj["dsl_file"].GetString();
      if(workflow_obj.HasMember("description"))
        wf_config.description = workflow_obj["description"].GetString();

      workflow_configs.push_back(wf_config);
    }
  }

  return true;
#else
  // Simple JSON parsing for stub mode
  size_t pos = json_str.find("\"id\":\"");
  if(pos == std::string::npos)
    return false;
  size_t start = pos + 6;
  size_t end   = json_str.find("\"", start);
  config.id    = json_str.substr(start, end - start);
  return true;
#endif
}

}  // namespace

Agent::Agent(const Config& config) : config_(config), state_(AgentState::CREATED) {
  initialize_components();
}

Agent::~Agent() {
  // Clean up components
  session_.reset();
  current_task_.reset();
  messages_.reset();
  context_.reset();
  event_.reset();
  package_.reset();
  
  cleanup_resources();
}

bool Agent::initialize_components() {
  // Initialize session component
  AgentSession::Config session_config;
  session_config.session_id = config_.id + "_session";
  session_ = std::make_shared<AgentSession>(session_config);

  // Initialize task component
  AgentTask::Config task_config;
  task_config.task_id = config_.id + "_task";
  task_config.task_type = "default";
  current_task_ = std::make_shared<AgentTask>(task_config);

  // Initialize messages component
  AgentMessages::Config messages_config;
  messages_config.max_queue_size = 1024;
  messages_ = std::make_shared<AgentMessages>(messages_config);

  // Initialize context component
  AgentContext::Config context_config;
  context_config.context_id = config_.id + "_context";
  context_config.agent_id = config_.id;
  context_ = std::make_shared<AgentContext>(context_config);

  // Initialize event component
  AgentEvent::Config event_config;
  event_config.handler_id = config_.id + "_event_handler";
  event_config.async_dispatch = false;
  event_ = std::make_shared<AgentEvent>(event_config);

  // Initialize package component
  AgentPackage::Config package_config;
  package_config.package_id = config_.id + "_package";
  package_config.package_name = config_.name;
  package_config.version = config_.version;
  package_ = std::make_shared<AgentPackage>(package_config);

  return true;
}

void Agent::add_workflow(std::shared_ptr<Workflow> workflow) {
  if(workflow && workflow_map_.size() < static_cast<size_t>(config_.max_workflows)) {
    workflow_map_[workflow->get_id()] = workflow;
    workflows_.push_back(workflow);
  }
}

void Agent::remove_workflow(const WorkflowId& workflow_id) {
  auto it = workflow_map_.find(workflow_id);
  if(it != workflow_map_.end()) {
    workflow_map_.erase(it);
    workflows_.erase(std::remove_if(workflows_.begin(), workflows_.end(),
                                    [&workflow_id](const std::shared_ptr<Workflow>& w) {
                                      return w->get_id() == workflow_id;
                                    }),
                     workflows_.end());
  }
}

std::shared_ptr<Workflow> Agent::get_workflow(const WorkflowId& workflow_id) {
  auto it = workflow_map_.find(workflow_id);
  return it != workflow_map_.end() ? it->second : nullptr;
}

bool Agent::initialize_resources() {
  // Initialize shared resources
  state_ = AgentState::INITIALIZED;
  return true;
}

void Agent::cleanup_resources() {
  // Clean up shared resources
  data_bus_.clear();
  while(!event_queue_.empty()) {
    event_queue_.pop();
  }
}

bool Agent::publish_data(const std::string& key, const DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  data_bus_[key] = value;
  return true;
}

bool Agent::get_data(const std::string& key, DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        it = data_bus_.find(key);
  if(it != data_bus_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

void Agent::post_event(const std::string& event) {
  std::lock_guard<std::mutex> lock(event_mutex_);
  event_queue_.push(event);
}

std::string Agent::get_event() {
  std::lock_guard<std::mutex> lock(event_mutex_);
  if(!event_queue_.empty()) {
    std::string event = event_queue_.front();
    event_queue_.pop();
    return event;
  }
  return "";
}

bool Agent::initialize() {
  if(!initialize_resources()) {
    return false;
  }

  for(auto& workflow : workflows_) {
    if(!workflow->validate()) {
      return false;
    }
  }

  state_ = AgentState::INITIALIZED;
  return true;
}

bool Agent::start() {
  if(state_ != AgentState::INITIALIZED) {
    return false;
  }
  state_ = AgentState::RUNNING;
  return true;
}

bool Agent::pause() {
  if(state_ != AgentState::RUNNING) {
    return false;
  }
  state_ = AgentState::PAUSED;
  return true;
}

bool Agent::resume() {
  if(state_ != AgentState::PAUSED) {
    return false;
  }
  state_ = AgentState::RUNNING;
  return true;
}

bool Agent::stop() {
  if(state_ != AgentState::RUNNING && state_ != AgentState::PAUSED) {
    return false;
  }
  cleanup_resources();
  state_ = AgentState::STOPPED;
  return true;
}

storage::StorageResult Agent::save(const storage::StorageConfig& config,
                                   const storage::VersionInfo&   version_info) {
  auto version_storage = utils::storage::create_versioned_storage(config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  // Convert to JSON
  std::string json_data = agent_config_to_json(config_, workflows_);

  // Save version
  utils::storage::StorageResult result =
      version_storage->save_version(config_.id, json_data, version_info);
  if(!result.is_success()) {
    return result;
  }

  // Save as current data
  result = version_storage->save(config_.id, json_data);
  if(!result.is_success()) {
    return result;
  }

  // Set as active version
  return version_storage->set_active_version(config_.id, version_info.version);
}

utils::storage::StorageResult Agent::load(const std::string&            agent_id,
                                   const utils::storage::StorageConfig& storage_config,
                                   std::shared_ptr<Agent>&       agent) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  std::string            json_data;
  utils::storage::StorageResult result = version_storage->load(agent_id, json_data);
  if(!result.is_success()) {
    return result;
  }

  Agent::Config                 config;
  std::vector<Workflow::Config> workflow_configs;

  if(!agent_config_from_json(json_data, config, workflow_configs)) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_INVALID_DATA,
                                   "Failed to parse agent config" };
  }

  // Create agent
  agent = std::make_shared<Agent>(config);

  // Note: Workflows are not fully restored here as Workflow::Config only has basic info
  // and we don't have the Workflow factory or registry here to recreate them.
  // In a real implementation, we would need to load workflows from their DSL files
  // or store full workflow definitions.
  // For now, we just restore the agent config.

  return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_OK, "" };
}

utils::storage::StorageResult Agent::delete_agent(const std::string&            agent_id,
                                           const utils::storage::StorageConfig& storage_config) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  return version_storage->remove(agent_id);
}

utils::storage::StorageResult Agent::list_agents(const utils::storage::StorageConfig& storage_config,
                                          std::vector<std::string>&     agent_ids) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  return version_storage->list("", agent_ids);
}

utils::storage::StorageResult Agent::get_metadata(const std::string&            agent_id,
                                           const utils::storage::StorageConfig& storage_config,
                                           AgentMetadata&                metadata) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  std::string            json_data;
  utils::storage::StorageResult result = version_storage->load(agent_id, json_data);
  if(!result.is_success()) {
    return result;
  }

  Agent::Config                 config;
  std::vector<Workflow::Config> workflow_configs;

  if(!agent_config_from_json(json_data, config, workflow_configs)) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_INVALID_DATA,
                                   "Failed to parse agent config" };
  }

  metadata.id          = config.id;
  metadata.name        = config.name;
  metadata.version     = config.version;
  metadata.description = config.description;
  metadata.created_at  = 0;
  metadata.updated_at  = get_current_timestamp_ms();
  metadata.author      = "";

  metadata.workflow_ids.clear();
  for(const auto& wf_config : workflow_configs) {
    metadata.workflow_ids.push_back(wf_config.id);
  }

  return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_OK, "" };
}

utils::storage::StorageResult Agent::set_active_version(const AgentId&                agent_id,
                                                 const std::string&            version,
                                                 const utils::storage::StorageConfig& storage_config) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }
  return version_storage->set_active_version(agent_id, version);
}

utils::storage::StorageResult Agent::get_active_version(const AgentId&                agent_id,
                                                 const utils::storage::StorageConfig& storage_config,
                                                 std::string&                  version) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }
  return version_storage->get_active_version(agent_id, version);
}

}  // namespace framework
}  // namespace agentflow
