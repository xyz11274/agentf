// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Workflow storage implementation

#include "workflow/workflow_storage.h"

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
    node_obj.AddMember("priority", node->get_priority(), allocator);

    // Execution mode
    const char* exec_mode = "cpp_function";
    switch(node->get_execution_mode()) {
      case framework::Node::ExecutionMode::CPP_FUNCTION:
        exec_mode = "cpp_function";
        break;
      case framework::Node::ExecutionMode::SHELL_SCRIPT:
        exec_mode = "shell_script";
        break;
      case framework::Node::ExecutionMode::EXECUTABLE:
        exec_mode = "executable";
        break;
      case framework::Node::ExecutionMode::PYTHON_SCRIPT:
        exec_mode = "python_script";
        break;
      case framework::Node::ExecutionMode::SO_LIBRARY:
        exec_mode = "so_library";
        break;
    }
    node_obj.AddMember("execution_mode", rapidjson::StringRef(exec_mode), allocator);

    // External execution config
    if(!node->get_command().empty()) {
      node_obj.AddMember("command", rapidjson::StringRef(node->get_command().c_str()), allocator);
    }
    // Note: working_dir, args, function_name, main_class, enable_hot_reload, memory_limit_mb, cpu_limit
    // are missing from node interface getters, so we can't serialize them unless we update Node class
    // or add config getter. 
    // Wait, Node has get_config() which returns const Config&.
    const auto& node_cfg = node->get_config();
    
    if(!node_cfg.working_dir.empty()) {
      node_obj.AddMember("working_dir", rapidjson::StringRef(node_cfg.working_dir.c_str()), allocator);
    }
    
    if(!node_cfg.args.empty()) {
      rapidjson::Value args_array(rapidjson::kArrayType);
      for(const auto& arg : node_cfg.args) {
        rapidjson::Value arg_val;
        arg_val.SetString(arg.c_str(), allocator);
        args_array.PushBack(arg_val, allocator);
      }
      node_obj.AddMember("args", args_array, allocator);
    }

    if(!node_cfg.function_name.empty()) {
      node_obj.AddMember("function_name", rapidjson::StringRef(node_cfg.function_name.c_str()), allocator);
    }

    if(!node_cfg.main_class.empty()) {
      node_obj.AddMember("main_class", rapidjson::StringRef(node_cfg.main_class.c_str()), allocator);
    }

    node_obj.AddMember("max_retries", node_cfg.max_retries, allocator);
    node_obj.AddMember("retry_delay_ms", node_cfg.retry_delay_ms, allocator);
    node_obj.AddMember("timeout_ms", node_cfg.timeout_ms, allocator);
    node_obj.AddMember("memory_limit_mb", node_cfg.memory_limit_mb, allocator);
    node_obj.AddMember("cpu_limit", node_cfg.cpu_limit, allocator);
    node_obj.AddMember("enable_hot_reload", node_cfg.enable_hot_reload, allocator);

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
    edge_obj.AddMember("priority", edge->get_priority(), allocator);

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

    // Edge config for extra fields
    const auto& edge_cfg = edge->get_config();

    // Source outputs
    if(!edge_cfg.source_outputs.empty()) {
      rapidjson::Value source_outputs_arr(rapidjson::kArrayType);
      for(const auto& val : edge_cfg.source_outputs) {
        rapidjson::Value v;
        v.SetString(val.c_str(), allocator);
        source_outputs_arr.PushBack(v, allocator);
      }
      edge_obj.AddMember("source_outputs", source_outputs_arr, allocator);
    }

    // Target inputs
    if(!edge_cfg.target_inputs.empty()) {
      rapidjson::Value target_inputs_arr(rapidjson::kArrayType);
      for(const auto& val : edge_cfg.target_inputs) {
        rapidjson::Value v;
        v.SetString(val.c_str(), allocator);
        target_inputs_arr.PushBack(v, allocator);
      }
      edge_obj.AddMember("target_inputs", target_inputs_arr, allocator);
    }

    // Data formats
    if(!edge_cfg.data_formats.empty()) {
      rapidjson::Value data_formats_arr(rapidjson::kArrayType);
      for(const auto& val : edge_cfg.data_formats) {
        rapidjson::Value v;
        v.SetString(val.c_str(), allocator);
        data_formats_arr.PushBack(v, allocator);
      }
      edge_obj.AddMember("data_formats", data_formats_arr, allocator);
    }

    // Supported events
    if(!edge_cfg.supported_events.empty()) {
      rapidjson::Value events_arr(rapidjson::kArrayType);
      for(const auto& val : edge_cfg.supported_events) {
        rapidjson::Value v;
        v.SetString(val.c_str(), allocator);
        events_arr.PushBack(v, allocator);
      }
      edge_obj.AddMember("supported_events", events_arr, allocator);
    }

    // Condition
    if(edge->has_condition()) {
      edge_obj.AddMember("condition_expression",
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
      if(node_obj.HasMember("priority"))
        node_config.priority = node_obj["priority"].GetInt();

      // Execution mode
      if(node_obj.HasMember("execution_mode")) {
        std::string mode_str = node_obj["execution_mode"].GetString();
        if(mode_str == "shell_script") {
          node_config.execution_mode = framework::Node::ExecutionMode::SHELL_SCRIPT;
        } else if(mode_str == "executable") {
          node_config.execution_mode = framework::Node::ExecutionMode::EXECUTABLE;
        } else if(mode_str == "python_script") {
          node_config.execution_mode = framework::Node::ExecutionMode::PYTHON_SCRIPT;
        } else if(mode_str == "so_library") {
          node_config.execution_mode = framework::Node::ExecutionMode::SO_LIBRARY;
        } else {
          node_config.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
        }
      }

      if(node_obj.HasMember("command"))
        node_config.command = node_obj["command"].GetString();
      if(node_obj.HasMember("working_dir"))
        node_config.working_dir = node_obj["working_dir"].GetString();
      
      if(node_obj.HasMember("args") && node_obj["args"].IsArray()) {
        const rapidjson::Value& args_array = node_obj["args"];
        for(rapidjson::SizeType j = 0; j < args_array.Size(); ++j) {
          node_config.args.push_back(args_array[j].GetString());
        }
      }

      if(node_obj.HasMember("function_name"))
        node_config.function_name = node_obj["function_name"].GetString();
      if(node_obj.HasMember("main_class"))
        node_config.main_class = node_obj["main_class"].GetString();
      
      if(node_obj.HasMember("max_retries"))
        node_config.max_retries = node_obj["max_retries"].GetInt();
      if(node_obj.HasMember("retry_delay_ms"))
        node_config.retry_delay_ms = node_obj["retry_delay_ms"].GetInt();
      if(node_obj.HasMember("memory_limit_mb"))
        node_config.memory_limit_mb = node_obj["memory_limit_mb"].GetInt();
      if(node_obj.HasMember("cpu_limit"))
        node_config.cpu_limit = node_obj["cpu_limit"].GetInt();
      if(node_obj.HasMember("enable_hot_reload"))
        node_config.enable_hot_reload = node_obj["enable_hot_reload"].GetBool();

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
      if(edge_obj.HasMember("priority"))
        edge_config.priority = edge_obj["priority"].GetInt();

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
      if(edge_obj.HasMember("condition_expression"))
        edge_config.condition_expression = edge_obj["condition_expression"].GetString();

      // Source outputs
      if(edge_obj.HasMember("source_outputs") && edge_obj["source_outputs"].IsArray()) {
        const rapidjson::Value& source_outputs_arr = edge_obj["source_outputs"];
        for(rapidjson::SizeType j = 0; j < source_outputs_arr.Size(); ++j) {
          edge_config.source_outputs.push_back(source_outputs_arr[j].GetString());
        }
      }

      // Target inputs
      if(edge_obj.HasMember("target_inputs") && edge_obj["target_inputs"].IsArray()) {
        const rapidjson::Value& target_inputs_arr = edge_obj["target_inputs"];
        for(rapidjson::SizeType j = 0; j < target_inputs_arr.Size(); ++j) {
          edge_config.target_inputs.push_back(target_inputs_arr[j].GetString());
        }
      }

      // Data formats
      if(edge_obj.HasMember("data_formats") && edge_obj["data_formats"].IsArray()) {
        const rapidjson::Value& data_formats_arr = edge_obj["data_formats"];
        for(rapidjson::SizeType j = 0; j < data_formats_arr.Size(); ++j) {
          edge_config.data_formats.push_back(data_formats_arr[j].GetString());
        }
      }

      // Supported events
      if(edge_obj.HasMember("supported_events") && edge_obj["supported_events"].IsArray()) {
        const rapidjson::Value& supported_events_arr = edge_obj["supported_events"];
        for(rapidjson::SizeType j = 0; j < supported_events_arr.Size(); ++j) {
          edge_config.supported_events.push_back(supported_events_arr[j].GetString());
        }
      }

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
