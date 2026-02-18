// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Workflow storage implementation

#include "storage/workflow_storage.h"

#include <sstream>

#include <sys/time.h>

#include "utils/logger.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
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
 * Convert Workflow::Config to JSON string
 */
std::string workflow_config_to_json(const framework::Workflow::Config&                   config,
                                    const std::vector<std::shared_ptr<framework::Node>>& nodes,
                                    const std::vector<std::shared_ptr<framework::Edge>>& edges) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

  // Workflow config
  rapidjson::Value workflow_obj(rapidjson::kObjectType);
  workflow_obj.AddMember("id", rapidjson::StringRef(config.id.c_str()), allocator);
  workflow_obj.AddMember("name", rapidjson::StringRef(config.name.c_str()), allocator);
  workflow_obj.AddMember("version", rapidjson::StringRef(config.version.c_str()), allocator);
  workflow_obj.AddMember("dsl_file", rapidjson::StringRef(config.dsl_file.c_str()), allocator);
  workflow_obj.AddMember("description", rapidjson::StringRef(config.description.c_str()),
                         allocator);

  doc.AddMember("workflow", workflow_obj, allocator);

  // Nodes
  rapidjson::Value nodes_array(rapidjson::kArrayType);
  for(const auto& node : nodes) {
    rapidjson::Value node_obj(rapidjson::kObjectType);
    node_obj.AddMember("id", rapidjson::StringRef(node->get_id().c_str()), allocator);
    node_obj.AddMember("name", rapidjson::StringRef(node->get_name().c_str()), allocator);
    node_obj.AddMember("type", rapidjson::StringRef(node->get_type().c_str()), allocator);
    node_obj.AddMember("timeout_ms", node->get_timeout_ms(), allocator);

    // Input parameters (as array for compatibility)
    rapidjson::Value inputs_array(rapidjson::kArrayType);
    for(const auto& param : node->get_input_params()) {
      rapidjson::Value value_str;
      value_str.SetString(param.c_str(), allocator);
      inputs_array.PushBack(value_str, allocator);
    }
    node_obj.AddMember("inputs", inputs_array, allocator);

    // Output parameters (as array for compatibility)
    rapidjson::Value outputs_array(rapidjson::kArrayType);
    for(const auto& param : node->get_output_params()) {
      rapidjson::Value value_str;
      value_str.SetString(param.c_str(), allocator);
      outputs_array.PushBack(value_str, allocator);
    }
    node_obj.AddMember("outputs", outputs_array, allocator);

    nodes_array.PushBack(node_obj, allocator);
  }
  doc.AddMember("nodes", nodes_array, allocator);

  // Edges
  rapidjson::Value edges_array(rapidjson::kArrayType);
  for(const auto& edge : edges) {
    rapidjson::Value edge_obj(rapidjson::kObjectType);
    edge_obj.AddMember("id", rapidjson::StringRef(edge->get_id().c_str()), allocator);
    edge_obj.AddMember("source_node", rapidjson::StringRef(edge->get_source().c_str()), allocator);
    edge_obj.AddMember("target_node", rapidjson::StringRef(edge->get_target().c_str()), allocator);

    // Transfer mode
    const char* transfer_mode = "shared_memory";
    switch(edge->get_transfer_mode()) {
      case framework::Edge::DataTransferMode::SHARED_MEMORY:
        transfer_mode = "shared_memory";
        break;
      case framework::Edge::DataTransferMode::MESSAGE_QUEUE:
        transfer_mode = "message_queue";
        break;
      case framework::Edge::DataTransferMode::REMOTE_TRANSFER:
        transfer_mode = "remote_transfer";
        break;
    }
    edge_obj.AddMember("transfer_mode", rapidjson::StringRef(transfer_mode), allocator);

    // Condition
    if(edge->has_condition()) {
      edge_obj.AddMember("condition",
                         rapidjson::StringRef(edge->get_condition_expression().c_str()), allocator);
    }

    edges_array.PushBack(edge_obj, allocator);
  }
  doc.AddMember("edges", edges_array, allocator);

  rapidjson::StringBuffer                          buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  return buffer.GetString();
#else
  std::ostringstream oss;
  oss << "{" << "\"id\":\"" << config.id << "\"," << "\"name\":\"" << config.name << "\","
      << "\"version\":\"" << config.version << "\"," << "\"nodes\":[]," << "\"edges\":[]" << "}";
  return oss.str();
#endif
}

/**
 * Parse Workflow::Config from JSON string
 */
bool workflow_config_from_json(const std::string& json_str, framework::Workflow::Config& config,
                               std::vector<framework::Node::Config>& node_configs,
                               std::vector<framework::Edge::Config>& edge_configs) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());

  if(doc.HasParseError() || !doc.HasMember("workflow")) {
    return false;
  }

  const rapidjson::Value& workflow_obj = doc["workflow"];

  if(workflow_obj.HasMember("id"))
    config.id = workflow_obj["id"].GetString();
  if(workflow_obj.HasMember("name"))
    config.name = workflow_obj["name"].GetString();
  if(workflow_obj.HasMember("version"))
    config.version = workflow_obj["version"].GetString();
  if(workflow_obj.HasMember("dsl_file"))
    config.dsl_file = workflow_obj["dsl_file"].GetString();
  if(workflow_obj.HasMember("description"))
    config.description = workflow_obj["description"].GetString();

  // Parse nodes
  if(doc.HasMember("nodes") && doc["nodes"].IsArray()) {
    const rapidjson::Value& nodes_array = doc["nodes"];
    for(rapidjson::SizeType i = 0; i < nodes_array.Size(); ++i) {
      const rapidjson::Value& node_obj = nodes_array[i];
      framework::Node::Config node_config;

      if(node_obj.HasMember("id"))
        node_config.id = node_obj["id"].GetString();
      if(node_obj.HasMember("name"))
        node_config.name = node_obj["name"].GetString();
      if(node_obj.HasMember("type"))
        node_config.type = node_obj["type"].GetString();
      if(node_obj.HasMember("timeout_ms"))
        node_config.timeout_ms = node_obj["timeout_ms"].GetInt();

      // Parse input parameters
      if(node_obj.HasMember("inputs") && node_obj["inputs"].IsArray()) {
        const rapidjson::Value& inputs_array = node_obj["inputs"];
        for(rapidjson::SizeType j = 0; j < inputs_array.Size(); ++j) {
          node_config.input_params.push_back(inputs_array[j].GetString());
        }
      }

      // Parse output parameters
      if(node_obj.HasMember("outputs") && node_obj["outputs"].IsArray()) {
        const rapidjson::Value& outputs_array = node_obj["outputs"];
        for(rapidjson::SizeType j = 0; j < outputs_array.Size(); ++j) {
          node_config.output_params.push_back(outputs_array[j].GetString());
        }
      }

      node_configs.push_back(node_config);
    }
  }

  // Parse edges
  if(doc.HasMember("edges") && doc["edges"].IsArray()) {
    const rapidjson::Value& edges_array = doc["edges"];
    for(rapidjson::SizeType i = 0; i < edges_array.Size(); ++i) {
      const rapidjson::Value& edge_obj = edges_array[i];
      framework::Edge::Config edge_config;

      if(edge_obj.HasMember("id"))
        edge_config.id = edge_obj["id"].GetString();
      if(edge_obj.HasMember("source_node"))
        edge_config.source_node = edge_obj["source_node"].GetString();
      if(edge_obj.HasMember("target_node"))
        edge_config.target_node = edge_obj["target_node"].GetString();

      if(edge_obj.HasMember("transfer_mode")) {
        const std::string& transfer_mode = edge_obj["transfer_mode"].GetString();
        if(transfer_mode == "shared_memory") {
          edge_config.transfer_mode = framework::Edge::DataTransferMode::SHARED_MEMORY;
        } else if(transfer_mode == "message_queue") {
          edge_config.transfer_mode = framework::Edge::DataTransferMode::MESSAGE_QUEUE;
        } else if(transfer_mode == "remote_transfer") {
          edge_config.transfer_mode = framework::Edge::DataTransferMode::REMOTE_TRANSFER;
        }
      }

      if(edge_obj.HasMember("condition"))
        edge_config.condition_expression = edge_obj["condition"].GetString();

      edge_configs.push_back(edge_config);
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

}  // anonymous namespace

// ============================================================================
// WorkflowStorage implementation
// ============================================================================

/**
 * Workflow storage implementation using file backend
 */
class WorkflowStorage : public IWorkflowStorage {
public:
  explicit WorkflowStorage(const StorageConfig& config) : config_(config) {
    version_storage_ = create_versioned_storage(config);
  }

  ~WorkflowStorage() override = default;

  StorageResult save_workflow(const framework::Workflow::Config&                   config,
                              const std::vector<std::shared_ptr<framework::Node>>& nodes,
                              const std::vector<std::shared_ptr<framework::Edge>>& edges,
                              const VersionInfo& version_info) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    // Convert to JSON
    std::string json_data = workflow_config_to_json(config, nodes, edges);

    // Save version
    StorageResult result = version_storage_->save_version(config.id, json_data, version_info);
    if(!result.is_success()) {
      return result;
    }

    // Save as current data
    result = version_storage_->save(config.id, json_data);
    if(!result.is_success()) {
      return result;
    }

    // Set as active version
    return version_storage_->set_active_version(config.id, version_info.version);
  }

  StorageResult load_workflow(const std::string& workflow_id, framework::Workflow::Config& config,
                              std::vector<framework::Node::Config>& node_configs,
                              std::vector<framework::Edge::Config>& edge_configs) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    std::string   json_data;
    StorageResult result = version_storage_->load(workflow_id, json_data);
    if(!result.is_success()) {
      return result;
    }

    if(!workflow_config_from_json(json_data, config, node_configs, edge_configs)) {
      return StorageResult{ StorageError::STORAGE_ERROR_INVALID_DATA,
                            "Failed to parse workflow config" };
    }

    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult load_workflow_version(const std::string& workflow_id, const std::string& version,
                                      framework::Workflow::Config&          config,
                                      std::vector<framework::Node::Config>& node_configs,
                                      std::vector<framework::Edge::Config>& edge_configs) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    std::string   json_data;
    StorageResult result = version_storage_->load_version(workflow_id, version, json_data);
    if(!result.is_success()) {
      return result;
    }

    if(!workflow_config_from_json(json_data, config, node_configs, edge_configs)) {
      return StorageResult{ StorageError::STORAGE_ERROR_INVALID_DATA,
                            "Failed to parse workflow config" };
    }

    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult delete_workflow(const std::string& workflow_id) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    return version_storage_->remove(workflow_id);
  }

  StorageResult list_workflows(std::vector<std::string>& workflow_ids) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    return version_storage_->list("", workflow_ids);
  }

  StorageResult get_workflow_metadata(const std::string& workflow_id,
                                      WorkflowMetadata&  metadata) override {
    framework::Workflow::Config          config;
    std::vector<framework::Node::Config> node_configs;
    std::vector<framework::Edge::Config> edge_configs;

    StorageResult result = load_workflow(workflow_id, config, node_configs, edge_configs);
    if(!result.is_success()) {
      return result;
    }

    metadata.id          = config.id;
    metadata.name        = config.name;
    metadata.version     = config.version;
    metadata.description = config.description;
    metadata.dsl_file    = config.dsl_file;
    metadata.created_at  = 0;  // Could be stored separately
    metadata.updated_at  = get_current_timestamp_ms();
    metadata.author      = "";

    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

  StorageResult list_workflow_versions(const std::string&        workflow_id,
                                       std::vector<VersionInfo>& versions) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    return version_storage_->list_versions(workflow_id, versions);
  }

  StorageResult set_active_workflow_version(const std::string& workflow_id,
                                            const std::string& version) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    return version_storage_->set_active_version(workflow_id, version);
  }

  StorageResult get_active_workflow_version(const std::string& workflow_id,
                                            std::string&       version) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    return version_storage_->get_active_version(workflow_id, version);
  }

  StorageResult rollback_workflow(const std::string& workflow_id,
                                  const std::string& version) override {
    if(!version_storage_) {
      return StorageResult{ StorageError::STORAGE_ERROR_IO, "Version storage not initialized" };
    }

    return version_storage_->rollback_to_version(workflow_id, version);
  }

  bool workflow_exists(const std::string& workflow_id) override {
    if(!version_storage_) {
      return false;
    }

    return version_storage_->exists(workflow_id);
  }

  StorageResult get_storage_stats(StorageStats& stats) override {
    std::vector<std::string> workflow_ids;
    StorageResult            result = list_workflows(workflow_ids);
    if(!result.is_success()) {
      return result;
    }

    stats.total_workflows = workflow_ids.size();
    stats.total_versions  = 0;

    for(const auto& workflow_id : workflow_ids) {
      std::vector<VersionInfo> versions;
      result = list_workflow_versions(workflow_id, versions);
      if(result.is_success()) {
        stats.total_versions += versions.size();
      }
    }

    stats.storage_size_bytes = 0;

    return StorageResult{ StorageError::STORAGE_OK, "" };
  }

private:
  StorageConfig                    config_;
  std::unique_ptr<IVersionStorage> version_storage_;
};

// ============================================================================
// Factory function
// ============================================================================

std::unique_ptr<IWorkflowStorage> create_workflow_storage(const StorageConfig& config) {
  return std::unique_ptr<IWorkflowStorage>(new WorkflowStorage(config));
}

}  // namespace storage
}  // namespace agentflow
