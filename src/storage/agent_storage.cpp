// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Agent storage implementation

#include "agentflow/storage/agent_storage.h"
#include "agentflow/basic/logger.h"

#include <sstream>
#include <sys/time.h>

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>
#endif

namespace agentflow {
namespace storage {

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
std::string agent_config_to_json(const framework::Agent::Config& config,
                                const std::vector<std::shared_ptr<framework::Workflow>>& workflows) {
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
  for (const auto& workflow : workflows) {
    rapidjson::Value workflow_obj(rapidjson::kObjectType);
    workflow_obj.AddMember("id", rapidjson::StringRef(workflow->get_id().c_str()), allocator);
    workflow_obj.AddMember("name", rapidjson::StringRef(workflow->get_name().c_str()), allocator);
    // Note: version and description are not directly accessible, skip them
    workflows_array.PushBack(workflow_obj, allocator);
  }
  doc.AddMember("workflows", workflows_array, allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  return buffer.GetString();
#else
  std::ostringstream oss;
  oss << "{"
      << "\"id\":\"" << config.id << "\","
      << "\"name\":\"" << config.name << "\","
      << "\"version\":\"" << config.version << "\""
      << "}";
  return oss.str();
#endif
}

/**
 * Parse Agent::Config from JSON string
 */
bool agent_config_from_json(const std::string& json_str,
                           framework::Agent::Config& config,
                           std::vector<framework::Workflow::Config>& workflow_configs) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());

  if (doc.HasParseError() || !doc.HasMember("agent")) {
    return false;
  }

  const rapidjson::Value& agent_obj = doc["agent"];

  if (agent_obj.HasMember("id")) config.id = agent_obj["id"].GetString();
  if (agent_obj.HasMember("name")) config.name = agent_obj["name"].GetString();
  if (agent_obj.HasMember("version")) config.version = agent_obj["version"].GetString();
  if (agent_obj.HasMember("description")) config.description = agent_obj["description"].GetString();
  if (agent_obj.HasMember("policy")) config.policy = static_cast<framework::Agent::LifecyclePolicy>(agent_obj["policy"].GetInt());
  if (agent_obj.HasMember("max_workflows")) config.max_workflows = agent_obj["max_workflows"].GetInt();
  if (agent_obj.HasMember("max_concurrent_workflows")) config.max_concurrent_workflows = agent_obj["max_concurrent_workflows"].GetInt();
  if (agent_obj.HasMember("data_bus_capacity")) config.data_bus_capacity = agent_obj["data_bus_capacity"].GetInt();

  // Parse workflows
  if (doc.HasMember("workflows") && doc["workflows"].IsArray()) {
    const rapidjson::Value& workflows_array = doc["workflows"];
    for (rapidjson::SizeType i = 0; i < workflows_array.Size(); ++i) {
      const rapidjson::Value& workflow_obj = workflows_array[i];
      framework::Workflow::Config wf_config;

      if (workflow_obj.HasMember("id")) wf_config.id = workflow_obj["id"].GetString();
      if (workflow_obj.HasMember("name")) wf_config.name = workflow_obj["name"].GetString();
      if (workflow_obj.HasMember("version")) wf_config.version = workflow_obj["version"].GetString();
      if (workflow_obj.HasMember("dsl_file")) wf_config.dsl_file = workflow_obj["dsl_file"].GetString();
      if (workflow_obj.HasMember("description")) wf_config.description = workflow_obj["description"].GetString();

      workflow_configs.push_back(wf_config);
    }
  }

  return true;
#else
  // Simple JSON parsing for stub mode
  size_t pos = json_str.find("\"id\":\"");
  if (pos == std::string::npos) return false;
  size_t start = pos + 6;
  size_t end = json_str.find("\"", start);
  config.id = json_str.substr(start, end - start);
  return true;
#endif
}

}  // anonymous namespace

// ============================================================================
// AgentStorage implementation
// ============================================================================

/**
 * Agent storage implementation using file backend
 */
class AgentStorage : public IAgentStorage {
 public:
  explicit AgentStorage(const StorageConfig& config)
      : config_(config) {
    version_storage_ = create_versioned_storage(config);
  }

  ~AgentStorage() override = default;

  StorageResult save_agent(const framework::Agent::Config& config,
                         const std::vector<std::shared_ptr<framework::Workflow>>& workflows,
                         const VersionInfo& version_info) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    // Convert to JSON
    std::string json_data = agent_config_to_json(config, workflows);

    // Save version
    StorageResult result = version_storage_->save_version(config.id, json_data, version_info);
    if (!result.is_success()) {
      return result;
    }

    // Save as current data
    result = version_storage_->save(config.id, json_data);
    if (!result.is_success()) {
      return result;
    }

    // Set as active version
    return version_storage_->set_active_version(config.id, version_info.version);
  }

  StorageResult load_agent(const std::string& agent_id,
                         framework::Agent::Config& config,
                         std::vector<framework::Workflow::Config>& workflow_configs) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    std::string json_data;
    StorageResult result = version_storage_->load(agent_id, json_data);
    if (!result.is_success()) {
      return result;
    }

    if (!agent_config_from_json(json_data, config, workflow_configs)) {
      return StorageResult{StorageError::STORAGE_ERROR_INVALID_DATA, "Failed to parse agent config"};
    }

    return StorageResult{StorageError::STORAGE_OK, ""};
  }

  StorageResult load_agent_version(const std::string& agent_id,
                                  const std::string& version,
                                  framework::Agent::Config& config,
                                  std::vector<framework::Workflow::Config>& workflow_configs) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    std::string json_data;
    StorageResult result = version_storage_->load_version(agent_id, version, json_data);
    if (!result.is_success()) {
      return result;
    }

    if (!agent_config_from_json(json_data, config, workflow_configs)) {
      return StorageResult{StorageError::STORAGE_ERROR_INVALID_DATA, "Failed to parse agent config"};
    }

    return StorageResult{StorageError::STORAGE_OK, ""};
  }

  StorageResult delete_agent(const std::string& agent_id) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    return version_storage_->remove(agent_id);
  }

  StorageResult list_agents(std::vector<std::string>& agent_ids) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    return version_storage_->list("", agent_ids);
  }

  StorageResult get_agent_metadata(const std::string& agent_id, AgentMetadata& metadata) override {
    framework::Agent::Config config;
    std::vector<framework::Workflow::Config> workflow_configs;

    StorageResult result = load_agent(agent_id, config, workflow_configs);
    if (!result.is_success()) {
      return result;
    }

    metadata.id = config.id;
    metadata.name = config.name;
    metadata.version = config.version;
    metadata.description = config.description;
    metadata.created_at = 0;  // Could be stored separately
    metadata.updated_at = get_current_timestamp_ms();
    metadata.author = "";

    metadata.workflow_ids.clear();
    for (const auto& wf_config : workflow_configs) {
      metadata.workflow_ids.push_back(wf_config.id);
    }

    return StorageResult{StorageError::STORAGE_OK, ""};
  }

  StorageResult list_agent_versions(const std::string& agent_id,
                                   std::vector<VersionInfo>& versions) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    return version_storage_->list_versions(agent_id, versions);
  }

  StorageResult set_active_agent_version(const std::string& agent_id,
                                       const std::string& version) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    return version_storage_->set_active_version(agent_id, version);
  }

  StorageResult get_active_agent_version(const std::string& agent_id,
                                       std::string& version) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    return version_storage_->get_active_version(agent_id, version);
  }

  StorageResult rollback_agent(const std::string& agent_id,
                              const std::string& version) override {
    if (!version_storage_) {
      return StorageResult{StorageError::STORAGE_ERROR_IO, "Version storage not initialized"};
    }

    return version_storage_->rollback_to_version(agent_id, version);
  }

  bool agent_exists(const std::string& agent_id) override {
    if (!version_storage_) {
      return false;
    }

    return version_storage_->exists(agent_id);
  }

  StorageResult get_storage_stats(StorageStats& stats) override {
    std::vector<std::string> agent_ids;
    StorageResult result = list_agents(agent_ids);
    if (!result.is_success()) {
      return result;
    }

    stats.total_agents = agent_ids.size();
    stats.total_versions = 0;
    stats.storage_size_bytes = 0;

    for (const auto& agent_id : agent_ids) {
      std::vector<VersionInfo> versions;
      result = list_agent_versions(agent_id, versions);
      if (result.is_success()) {
        stats.total_versions += versions.size();
      }
    }

    return StorageResult{StorageError::STORAGE_OK, ""};
  }

 private:
  StorageConfig config_;
  std::unique_ptr<IVersionStorage> version_storage_;
};

// ============================================================================
// Factory function
// ============================================================================

std::unique_ptr<IAgentStorage> create_agent_storage(const StorageConfig& config) {
  return std::unique_ptr<IAgentStorage>(new AgentStorage(config));
}

}  // namespace storage
}  // namespace agentflow
