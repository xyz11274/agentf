#include "workflow/workflow.h"

#include <sys/time.h>

#include <algorithm>
#include <future>
#include <queue>
#include <sstream>
#include <unordered_set>

#include "utils/file_storage.h"
#include "utils/logger.h"

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

const char* control_type_to_string(MMEdge::ControlType type) {
  switch(type) {
    case MMEdge::ControlType::IF:
      return "if";
    case MMEdge::ControlType::SWITCH_CASE:
      return "switch-case";
    case MMEdge::ControlType::NONE:
    default:
      return "none";
  }
}

MMEdge::ControlType control_type_from_string(const std::string& type) {
  if(type == "if")
    return MMEdge::ControlType::IF;
  if(type == "switch-case")
    return MMEdge::ControlType::SWITCH_CASE;
  return MMEdge::ControlType::NONE;
}

const char* field_type_to_string(MMNode::FieldType type) {
  switch(type) {
    case MMNode::FieldType::INT:
      return "int";
    case MMNode::FieldType::FLOAT:
      return "float";
    case MMNode::FieldType::BOOL:
      return "bool";
    case MMNode::FieldType::LIST:
      return "list";
    case MMNode::FieldType::MAP:
      return "map";
    case MMNode::FieldType::STRING:
    default:
      return "string";
  }
}

MMNode::FieldType field_type_from_string(const std::string& type) {
  if(type == "int") {
    return MMNode::FieldType::INT;
  }
  if(type == "float") {
    return MMNode::FieldType::FLOAT;
  }
  if(type == "bool") {
    return MMNode::FieldType::BOOL;
  }
  if(type == "list") {
    return MMNode::FieldType::LIST;
  }
  if(type == "map") {
    return MMNode::FieldType::MAP;
  }
  return MMNode::FieldType::STRING;
}

const char* node_transfer_mode_to_string(MMNode::TransferMode mode) {
  switch(mode) {
    case MMNode::TransferMode::REFERENCE:
      return "ref";
    case MMNode::TransferMode::VALUE:
    default:
      return "value";
  }
}

const char* node_execution_risk_level_to_string(MMNode::ExecutionRiskLevel level) {
  switch(level) {
    case MMNode::ExecutionRiskLevel::RED:
      return "red";
    case MMNode::ExecutionRiskLevel::YELLOW:
      return "yellow";
    case MMNode::ExecutionRiskLevel::GREEN:
    default:
      return "green";
  }
}

MMNode::TransferMode node_transfer_mode_from_string(const std::string& mode) {
  if(mode == "ref") {
    return MMNode::TransferMode::REFERENCE;
  }
  return MMNode::TransferMode::VALUE;
}

const char* edge_transfer_mode_to_string(MMEdge::DataTransferMode mode) {
  switch(mode) {
    case MMEdge::DataTransferMode::REFERENCE:
      return "ref";
    case MMEdge::DataTransferMode::VALUE:
    default:
      return "value";
  }
}

MMEdge::DataTransferMode edge_transfer_mode_from_string(const std::string& mode) {
  if(mode == "ref") {
    return MMEdge::DataTransferMode::REFERENCE;
  }
  if(mode == "shared_memory" || mode == "message_queue" || mode == "remote_transfer") {
    return MMEdge::DataTransferMode::VALUE;
  }
  return MMEdge::DataTransferMode::VALUE;
}

std::string trim_copy(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\n\r");
  if(start == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\n\r");
  return value.substr(start, end - start + 1);
}

std::string strip_quotes(std::string value) {
  value = trim_copy(value);
  if(value.size() >= 2
     && ((value.front() == '\'' && value.back() == '\'')
         || (value.front() == '"' && value.back() == '"'))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

/**
 * Convert MMWorkflow::Config to JSON string
 */
std::string workflow_config_to_json(const MMWorkflow::Config&                   config,
                                    const std::vector<std::shared_ptr<MMNode>>& nodes,
                                    const std::vector<std::shared_ptr<MMEdge>>& edges) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

  // MMWorkflow config
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
      case MMNode::ExecutionMode::CPP_FUNCTION:
        exec_mode = "cpp_function";
        break;
      case MMNode::ExecutionMode::SHELL_SCRIPT:
        exec_mode = "shell_script";
        break;
      case MMNode::ExecutionMode::EXECUTABLE:
        exec_mode = "executable";
        break;
      case MMNode::ExecutionMode::GO_SCRIPT:
        exec_mode = "go_script";
        break;
      case MMNode::ExecutionMode::PYTHON_SCRIPT:
        exec_mode = "python_script";
        break;
      case MMNode::ExecutionMode::JAVA_JAR:
        exec_mode = "java_jar";
        break;
      case MMNode::ExecutionMode::SO_LIBRARY:
        exec_mode = "so_library";
        break;
    }
    node_obj.AddMember("execution_mode", rapidjson::StringRef(exec_mode), allocator);

    // External execution config
    if(!node->get_command().empty()) {
      node_obj.AddMember("command", rapidjson::StringRef(node->get_command().c_str()), allocator);
    }
    const auto& node_cfg = node->get_config();

    if(!node_cfg.working_dir.empty()) {
      node_obj.AddMember("working_dir", rapidjson::StringRef(node_cfg.working_dir.c_str()),
                         allocator);
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
      node_obj.AddMember("function_name", rapidjson::StringRef(node_cfg.function_name.c_str()),
                         allocator);
    }

    if(!node_cfg.main_class.empty()) {
      node_obj.AddMember("main_class", rapidjson::StringRef(node_cfg.main_class.c_str()),
                         allocator);
    }

    node_obj.AddMember("max_retries", node_cfg.max_retries, allocator);
    node_obj.AddMember("retry_delay_ms", node_cfg.retry_delay_ms, allocator);
    node_obj.AddMember("timeout_ms", node_cfg.timeout_ms, allocator);
    node_obj.AddMember("memory_limit_mb", node_cfg.memory_limit_mb, allocator);
    node_obj.AddMember("cpu_limit", node_cfg.cpu_limit, allocator);
    node_obj.AddMember("enable_hot_reload", node_cfg.enable_hot_reload, allocator);
    node_obj.AddMember("execution_risk_level",
                       rapidjson::StringRef(
                           node_execution_risk_level_to_string(node_cfg.execution_risk_level)),
                       allocator);

    // Input parameters
    rapidjson::Value inputs_array(rapidjson::kArrayType);
    for(const auto& field : node->get_input_definitions()) {
      rapidjson::Value field_obj(rapidjson::kObjectType);
      field_obj.AddMember("name", rapidjson::StringRef(field.name.c_str()), allocator);
      field_obj.AddMember("type", rapidjson::StringRef(field_type_to_string(field.type)), allocator);
      field_obj.AddMember("transfer_mode",
                          rapidjson::StringRef(node_transfer_mode_to_string(field.transfer_mode)),
                          allocator);
      field_obj.AddMember("required", field.required, allocator);
      inputs_array.PushBack(field_obj, allocator);
    }
    node_obj.AddMember("inputs", inputs_array, allocator);

    // Output parameters
    rapidjson::Value outputs_array(rapidjson::kArrayType);
    for(const auto& field : node->get_output_definitions()) {
      rapidjson::Value field_obj(rapidjson::kObjectType);
      field_obj.AddMember("name", rapidjson::StringRef(field.name.c_str()), allocator);
      field_obj.AddMember("type", rapidjson::StringRef(field_type_to_string(field.type)), allocator);
      field_obj.AddMember("transfer_mode",
                          rapidjson::StringRef(node_transfer_mode_to_string(field.transfer_mode)),
                          allocator);
      field_obj.AddMember("required", field.required, allocator);
      outputs_array.PushBack(field_obj, allocator);
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
    edge_obj.AddMember("transfer_mode",
                       rapidjson::StringRef(edge_transfer_mode_to_string(edge->get_transfer_mode())),
                       allocator);

    // MMEdge config
    const auto& edge_cfg = edge->get_config();

    if(!edge_cfg.source_outputs.empty()) {
      rapidjson::Value source_outputs_arr(rapidjson::kArrayType);
      for(const auto& val : edge_cfg.source_outputs) {
        rapidjson::Value v;
        v.SetString(val.c_str(), allocator);
        source_outputs_arr.PushBack(v, allocator);
      }
      edge_obj.AddMember("source_outputs", source_outputs_arr, allocator);
    }

    if(!edge_cfg.target_inputs.empty()) {
      rapidjson::Value target_inputs_arr(rapidjson::kArrayType);
      for(const auto& val : edge_cfg.target_inputs) {
        rapidjson::Value v;
        v.SetString(val.c_str(), allocator);
        target_inputs_arr.PushBack(v, allocator);
      }
      edge_obj.AddMember("target_inputs", target_inputs_arr, allocator);
    }

    if(!edge_cfg.data_formats.empty()) {
      rapidjson::Value data_formats_arr(rapidjson::kArrayType);
      for(const auto& val : edge_cfg.data_formats) {
        rapidjson::Value v;
        v.SetString(val.c_str(), allocator);
        data_formats_arr.PushBack(v, allocator);
      }
      edge_obj.AddMember("data_formats", data_formats_arr, allocator);
    }

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
    if(edge->has_control()) {
      rapidjson::Value control_obj(rapidjson::kObjectType);
      const auto& c = edge_cfg.control;
      control_obj.AddMember("type", rapidjson::StringRef(control_type_to_string(c.type)), allocator);
      if(!c.expression.empty()) {
        control_obj.AddMember("expression", rapidjson::StringRef(c.expression.c_str()), allocator);
      }
      if(!c.case_value.empty()) {
        control_obj.AddMember("case_value", rapidjson::StringRef(c.case_value.c_str()), allocator);
      }
      control_obj.AddMember("is_default", c.is_default, allocator);
      edge_obj.AddMember("control", control_obj, allocator);
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
 * Parse MMWorkflow::Config from JSON string
 */
bool workflow_config_from_json(const std::string& json_str, MMWorkflow::Config& config,
                               std::vector<MMNode::Config>& node_configs,
                               std::vector<MMEdge::Config>& edge_configs) {
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
      MMNode::Config            node_config;

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
          node_config.execution_mode = MMNode::ExecutionMode::SHELL_SCRIPT;
        } else if(mode_str == "executable") {
          node_config.execution_mode = MMNode::ExecutionMode::EXECUTABLE;
        } else if(mode_str == "go_script") {
          node_config.execution_mode = MMNode::ExecutionMode::GO_SCRIPT;
        } else if(mode_str == "python_script") {
          node_config.execution_mode = MMNode::ExecutionMode::PYTHON_SCRIPT;
        } else if(mode_str == "java_jar") {
          node_config.execution_mode = MMNode::ExecutionMode::JAVA_JAR;
        } else if(mode_str == "so_library") {
          node_config.execution_mode = MMNode::ExecutionMode::SO_LIBRARY;
        } else {
          node_config.execution_mode = MMNode::ExecutionMode::CPP_FUNCTION;
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

      auto parse_fields = [](const rapidjson::Value& field_array,
                             std::vector<MMNode::FieldDefinition>& field_defs,
                             std::vector<std::string>& field_names) {
        for(rapidjson::SizeType j = 0; j < field_array.Size(); ++j) {
          const auto& field_value = field_array[j];
          MMNode::FieldDefinition field_def;
          if(field_value.IsString()) {
            field_def.name = field_value.GetString();
          } else if(field_value.IsObject()) {
            if(field_value.HasMember("name") && field_value["name"].IsString()) {
              field_def.name = field_value["name"].GetString();
            }
            if(field_value.HasMember("type") && field_value["type"].IsString()) {
              field_def.type = field_type_from_string(field_value["type"].GetString());
            }
            if(field_value.HasMember("transfer_mode") && field_value["transfer_mode"].IsString()) {
              field_def.transfer_mode =
                  node_transfer_mode_from_string(field_value["transfer_mode"].GetString());
            }
            if(field_value.HasMember("required") && field_value["required"].IsBool()) {
              field_def.required = field_value["required"].GetBool();
            }
          } else {
            continue;
          }

          if(field_def.name.empty()) {
            continue;
          }
          field_defs.push_back(field_def);
          field_names.push_back(field_def.name);
        }
      };

      // Parse input parameters
      if(node_obj.HasMember("inputs") && node_obj["inputs"].IsArray()) {
        parse_fields(node_obj["inputs"], node_config.input_definitions, node_config.input_params);
      } else if(node_obj.HasMember("input_params") && node_obj["input_params"].IsArray()) {
        parse_fields(node_obj["input_params"], node_config.input_definitions, node_config.input_params);
      }

      // Parse output parameters
      if(node_obj.HasMember("outputs") && node_obj["outputs"].IsArray()) {
        parse_fields(node_obj["outputs"], node_config.output_definitions, node_config.output_params);
      } else if(node_obj.HasMember("output_params") && node_obj["output_params"].IsArray()) {
        parse_fields(node_obj["output_params"], node_config.output_definitions,
                     node_config.output_params);
      }

      node_configs.push_back(node_config);
    }
  }

  // Parse edges
  if(doc.HasMember("edges") && doc["edges"].IsArray()) {
    const rapidjson::Value& edges_array = doc["edges"];
    for(rapidjson::SizeType i = 0; i < edges_array.Size(); ++i) {
      const rapidjson::Value& edge_obj = edges_array[i];
      MMEdge::Config            edge_config;

      if(edge_obj.HasMember("id"))
        edge_config.id = edge_obj["id"].GetString();
      if(edge_obj.HasMember("source_node"))
        edge_config.source_node = edge_obj["source_node"].GetString();
      if(edge_obj.HasMember("target_node"))
        edge_config.target_node = edge_obj["target_node"].GetString();
      if(edge_obj.HasMember("priority"))
        edge_config.priority = edge_obj["priority"].GetInt();

      if(edge_obj.HasMember("transfer_mode")) {
        edge_config.transfer_mode = edge_transfer_mode_from_string(edge_obj["transfer_mode"].GetString());
      }

      if(edge_obj.HasMember("condition"))
        edge_config.condition_expression = edge_obj["condition"].GetString();
      if(edge_obj.HasMember("condition_expression"))
        edge_config.condition_expression = edge_obj["condition_expression"].GetString();
      if(edge_obj.HasMember("control") && edge_obj["control"].IsObject()) {
        const rapidjson::Value& control_obj = edge_obj["control"];
        if(control_obj.HasMember("type")) {
          edge_config.control.type = control_type_from_string(control_obj["type"].GetString());
        }
        if(control_obj.HasMember("expression")) {
          edge_config.control.expression = control_obj["expression"].GetString();
        }
        if(control_obj.HasMember("case_value")) {
          edge_config.control.case_value = control_obj["case_value"].GetString();
        }
        if(control_obj.HasMember("is_default")) {
          edge_config.control.is_default = control_obj["is_default"].GetBool();
        }
      }
      if(edge_config.control.type == MMEdge::ControlType::IF &&
         edge_config.control.expression.empty()) {
        edge_config.control.expression = edge_config.condition_expression;
      }
      if(edge_config.control.type == MMEdge::ControlType::NONE &&
         !edge_config.condition_expression.empty()) {
        edge_config.control.type       = MMEdge::ControlType::IF;
        edge_config.control.expression = edge_config.condition_expression;
      }

      if(edge_obj.HasMember("source_outputs") && edge_obj["source_outputs"].IsArray()) {
        const rapidjson::Value& source_outputs_arr = edge_obj["source_outputs"];
        for(rapidjson::SizeType j = 0; j < source_outputs_arr.Size(); ++j) {
          edge_config.source_outputs.push_back(source_outputs_arr[j].GetString());
        }
      }

      if(edge_obj.HasMember("target_inputs") && edge_obj["target_inputs"].IsArray()) {
        const rapidjson::Value& target_inputs_arr = edge_obj["target_inputs"];
        for(rapidjson::SizeType j = 0; j < target_inputs_arr.Size(); ++j) {
          edge_config.target_inputs.push_back(target_inputs_arr[j].GetString());
        }
      }

      if(edge_obj.HasMember("data_formats") && edge_obj["data_formats"].IsArray()) {
        const rapidjson::Value& data_formats_arr = edge_obj["data_formats"];
        for(rapidjson::SizeType j = 0; j < data_formats_arr.Size(); ++j) {
          edge_config.data_formats.push_back(data_formats_arr[j].GetString());
        }
      }

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

}  // namespace

}  // namespace framework



namespace framework {

MMWorkflow::MMWorkflow(const Config& config) : config_(config), state_(WorkflowState::CREATED) {
  Logger::instance().info("MMWorkflow created: " + config.id);
}

MMWorkflow::~MMWorkflow() {
  stop_runtime();
}

bool MMWorkflow::start_runtime() {
  return start_runtime(RuntimeConfig{});
}

bool MMWorkflow::start_runtime(const RuntimeConfig& config) {
  bool expected = false;
  if(!runtime_running_.compare_exchange_strong(expected, true)) {
    return false;
  }

  runtime_config_ = config;
  message_worker_pool_ = std::make_unique<basic::ThreadPool>(
      static_cast<size_t>(std::max(1, runtime_config_.message_threads)));
  node_executor_pool_ = std::make_unique<basic::ThreadPool>(
      static_cast<size_t>(std::max(1, runtime_config_.node_executor_threads)));

  message_thread_ = std::thread(&MMWorkflow::message_dispatch_loop, this);

  // Auto-start per-node runtime loops for nodes that declare node_runtime config.
  for(auto& node : nodes_) {
    if(node->get_config().auto_start_runtime) {
      node->start_runtime(node->get_config().node_runtime_config);
    }
  }

  return true;
}

void MMWorkflow::stop_runtime() {
  bool expected = true;
  if(!runtime_running_.compare_exchange_strong(expected, false)) {
    return;
  }

  // Stop per-node runtime loops before shutting down workflow threads.
  for(auto& node : nodes_) {
    if(node->get_config().auto_start_runtime) {
      node->stop_runtime();
    }
  }

  message_cv_.notify_all();

  if(message_thread_.joinable()) {
    message_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(message_queue_mutex_);
    high_priority_messages_.clear();
    normal_priority_messages_.clear();
  }
  message_worker_pool_.reset();
  node_executor_pool_.reset();
}

bool MMWorkflow::enqueue_message(const Message& message) {
  std::lock_guard<std::mutex> lock(message_queue_mutex_);
  const size_t queued_size = high_priority_messages_.size() + normal_priority_messages_.size();
  if(queued_size >= runtime_config_.max_queue_size) {
    return false;
  }

  if(message.priority == MessagePriority::HIGH) {
    high_priority_messages_.push_back(message);
  } else {
    normal_priority_messages_.push_back(message);
  }
  message_cv_.notify_all();
  return true;
}

bool MMWorkflow::enqueue_event(const WorkflowEvent& event) {
  (void)event;
  return true;
}

void MMWorkflow::register_event_handler(const std::string& event_type, WorkflowEventHandler handler) {
  (void)event_type;
  (void)handler;
}

void MMWorkflow::message_dispatch_loop() {
  while(runtime_running_) {
    Message message;
    {
      std::unique_lock<std::mutex> lock(message_queue_mutex_);
      message_cv_.wait(lock, [this]() {
        return !high_priority_messages_.empty() || !normal_priority_messages_.empty()
               || !runtime_running_;
      });
      if(!runtime_running_) {
        break;
      }

      if(!high_priority_messages_.empty()) {
        message = high_priority_messages_.front();
        high_priority_messages_.pop_front();
      } else if(!normal_priority_messages_.empty()) {
        message = normal_priority_messages_.front();
        normal_priority_messages_.pop_front();
      } else {
        continue;
      }
    }

    if(message_worker_pool_) {
      message_worker_pool_->submit([this, message]() { execute_message_through_workflow(message); });
    } else {
      execute_message_through_workflow(message);
    }
  }
}

void MMWorkflow::event_dispatch_loop() {
  // Workflow event dispatch has been retired in favor of worker control/business queues.
}

std::vector<std::string> MMWorkflow::get_start_nodes() const {
  std::unordered_set<std::string> targets;
  for(const auto& edge : edges_) {
    targets.insert(edge->get_target());
  }

  std::vector<std::string> starts;
  for(const auto& node : nodes_) {
    if(targets.count(node->get_id()) == 0) {
      starts.push_back(node->get_id());
    }
  }
  return starts;
}

bool MMWorkflow::execute_message_through_workflow(const Message& message) {
  (void)message;
  state_ = WorkflowState::RUNNING;

  std::map<std::string, int> in_degree;
  std::map<std::string, std::vector<std::string>> adjacency;

  for(const auto& node : nodes_) {
    in_degree[node->get_id()] = 0;
  }
  for(const auto& edge : edges_) {
    adjacency[edge->get_source()].push_back(edge->get_target());
    in_degree[edge->get_target()]++;
  }

  std::vector<std::string> current_level;
  for(const auto& node : nodes_) {
    if(in_degree[node->get_id()] == 0) {
      current_level.push_back(node->get_id());
    }
  }

  bool success = true;
  while(!current_level.empty()) {
    std::vector<std::future<bool>> futures;
    for(const auto& node_id : current_level) {
      auto node = get_node(node_id);
      if(!node) {
        continue;
      }

      futures.push_back(node_executor_pool_->submit([node, message]() mutable {
        if(node->get_state() == NodeState::WAITING) {
          node->initialize();
        } else if(node->get_state() != NodeState::READY && node->get_state() != NodeState::RETRYING) {
          node->set_state(NodeState::READY);
        }
        if(!message.payload.empty()) {
          node->set_inputs(message.payload);
        }
        const bool ok = node->execute();
        node->cleanup();
        return ok;
      }));
    }

    for(auto& future : futures) {
      if(!future.get()) {
        success = false;
      }
    }

    std::vector<std::string> next_level;
    for(const auto& node_id : current_level) {
      auto it = adjacency.find(node_id);
      if(it == adjacency.end()) {
        continue;
      }
      for(const auto& target : it->second) {
        in_degree[target]--;
        if(in_degree[target] == 0) {
          next_level.push_back(target);
        }
      }
    }
    current_level.swap(next_level);
  }

  state_ = success ? WorkflowState::COMPLETED : WorkflowState::FAILED;

  WorkflowEvent event;
  event.type          = success ? "workflow_message_success" : "workflow_message_failed";
  event.data["workflow_id"] = get_id();
  event.data["message_id"]  = message.message_id;
  enqueue_event(event);
  return success;
}


void MMWorkflow::add_node(std::shared_ptr<MMNode> node) {
  if(node) {
    node_map_[node->get_id()] = node;
    nodes_.push_back(node);
  }
}

void MMWorkflow::remove_node(const std::string& node_id) {
  auto it = node_map_.find(node_id);
  if(it != node_map_.end()) {
    node_map_.erase(it);
    nodes_.erase(std::remove_if(
                     nodes_.begin(), nodes_.end(),
                     [&node_id](const std::shared_ptr<MMNode>& n) { return n->get_id() == node_id; }),
                 nodes_.end());
  }
}

std::shared_ptr<MMNode> MMWorkflow::get_node(const std::string& node_id) {
  auto it = node_map_.find(node_id);
  return it != node_map_.end() ? it->second : nullptr;
}

void MMWorkflow::add_edge(std::shared_ptr<MMEdge> edge) {
  if(edge) {
    edges_.push_back(edge);
  }
}

void MMWorkflow::remove_edge(const std::string& edge_id) {
  edges_.erase(
      std::remove_if(edges_.begin(), edges_.end(),
                     [&edge_id](const std::shared_ptr<MMEdge>& e) { return e->get_id() == edge_id; }),
      edges_.end());
}

bool MMWorkflow::load_from_dsl(const std::string& dsl_content) {
  // Parse DSL and build workflow
  // In real implementation, this would parse JSON or other DSL format
  state_ = WorkflowState::LOADED;
  return true;
}

bool MMWorkflow::validate() {
  if(!validate_dag()) {
    return false;
  }

  // Initialize all nodes to be ready for execution
  for(auto& node : nodes_) {
    node->initialize();
  }

  state_ = WorkflowState::LOADED;
  return true;
}

bool MMWorkflow::update_dynamic(const std::string& dsl_content) {
  // Dynamically update workflow DSL without recreating instance
  return load_from_dsl(dsl_content);
}

std::vector<std::string> MMWorkflow::get_topological_order() {
  // If cached order is valid, return it
  if(topological_order_valid_) {
    return topological_order_;
  }

  // Compute topological order using Kahn's algorithm
  compute_topological_order();

  return topological_order_;
}

std::vector<std::string> MMWorkflow::get_executable_nodes() {
  std::vector<std::string> executable;

  for(const auto& node : nodes_) {
    // Check if all dependencies are satisfied
    bool all_deps_ready = true;
    int  in_edge_count  = 0;

    for(const auto& edge : edges_) {
      if(edge->get_target() == node->get_id()) {
        in_edge_count++;
        // Check if source node is done
        auto source = get_node(edge->get_source());
        if(source && source->get_state() != NodeState::SUCCESS) {
          all_deps_ready = false;
          break;
        }
      }
    }

    // Nodes with no incoming edges (sources) are always ready to execute
    // if they are in READY state
    if(in_edge_count == 0) {
      all_deps_ready = true;
    }

    if(all_deps_ready && node->get_state() == NodeState::READY) {
      executable.push_back(node->get_id());
    }
  }

  return executable;
}

std::vector<std::string> MMWorkflow::get_executable_nodes_with_control() {
  std::vector<std::string> executable;
  std::unordered_map<std::string, bool> selected_edges;

  for(const auto& edge : edges_) {
    selected_edges[edge->get_id()] = edge->get_control_type() != MMEdge::ControlType::SWITCH_CASE;
  }

  std::map<std::string, std::map<std::string, std::vector<std::shared_ptr<MMEdge>>>> switch_groups;
  for(const auto& edge : edges_) {
    if(edge->get_control_type() != MMEdge::ControlType::SWITCH_CASE) {
      continue;
    }
    const auto& control_cfg = edge->get_control_config();
    const std::string group_expression =
        !control_cfg.expression.empty() ? control_cfg.expression : "__missing_switch_expression__";
    switch_groups[edge->get_source()][group_expression].push_back(edge);
  }

  for(const auto& source_entry : switch_groups) {
    auto          source         = get_node(source_entry.first);
    const DataMap source_outputs = source ? source->get_outputs() : DataMap{};

    for(const auto& group_entry : source_entry.second) {
      const auto& expression = group_entry.first;
      if(expression == "__missing_switch_expression__") {
        continue;
      }

      const std::string selector_value =
          strip_quotes(resolve_value(expression, control_context_, source_outputs));
      bool matched = false;
      for(const auto& edge : group_entry.second) {
        const auto& control_cfg = edge->get_control_config();
        const auto  case_value  = strip_quotes(control_cfg.case_value);
        if(!control_cfg.is_default && selector_value == case_value) {
          selected_edges[edge->get_id()] = true;
          matched = true;
        } else {
          selected_edges[edge->get_id()] = false;
        }
      }

      if(!matched) {
        for(const auto& edge : group_entry.second) {
          if(edge->get_control_config().is_default) {
            selected_edges[edge->get_id()] = true;
          }
        }
      }
    }
  }

  for(const auto& node : nodes_) {
    // Check if node is already executed
    if(executed_nodes_.count(node->get_id())) {
      continue;
    }

    // Check if all dependencies are satisfied
    bool all_deps_ready      = true;
    int  selected_edge_count = 0;
    int  in_edge_count       = 0;

    for(const auto& edge : edges_) {
      if(edge->get_target() == node->get_id()) {
        in_edge_count++;

        // Evaluate edge condition if present
        bool should_traverse = selected_edges[edge->get_id()];
        const auto& control_cfg = edge->get_control_config();
        if(control_cfg.type == MMEdge::ControlType::IF
           || (control_cfg.type == MMEdge::ControlType::NONE
               && (!control_cfg.expression.empty() || edge->has_condition()))) {
          const std::string expr =
              !control_cfg.expression.empty() ? control_cfg.expression : edge->get_condition_expression();
          auto source = get_node(edge->get_source());
          should_traverse =
              source && evaluate_condition(parse_condition_expression(expr), control_context_,
                                           source->get_outputs());
        }

        if(should_traverse) {
          selected_edge_count++;
          auto source = get_node(edge->get_source());
          if(source && source->get_state() != NodeState::SUCCESS) {
            all_deps_ready = false;
            break;
          } else if(!source) {
            all_deps_ready = false;
            break;
          }
        }
      }
    }

    // Nodes with no incoming edges (sources) or all conditions met
    if((in_edge_count == 0 || (selected_edge_count > 0 && all_deps_ready))
       && node->get_state() == NodeState::READY) {
      executable.push_back(node->get_id());
    }
  }

  return executable;
}

std::string MMWorkflow::get_next_node_after_execution(const std::string& current_node, bool success,
                                                    const std::string& error_msg) {
  // Determine control action after node execution
  NodeState execution_state = success ? NodeState::SUCCESS : NodeState::FAILED;
  auto      action =
      determine_next_action(current_node, execution_state, error_msg);

  // Handle different control actions
  switch(action.type) {
    case ControlActionType::RETRY:
      // Retry the same node
      return current_node;

    case ControlActionType::SKIP:
      // Skip to next node in topological order
      return "";

    case ControlActionType::GOTO:
      // Jump to specific node
      return action.target_node_id;

    case ControlActionType::ABORT:
      // MMWorkflow abort
      return "__ABORT__";

    case ControlActionType::RETURN:
      // Return from loop
      return "__RETURN__";

    case ControlActionType::BREAK:
      // Break from loop
      return "__BREAK__";

    case ControlActionType::CONTINUE:
      // Continue loop iteration
      return "__CONTINUE__";

    default:
      // Proceed to next node
      return "";
  }
}

void MMWorkflow::mark_node_executed(const std::string& node_id, bool success,
                                  const std::string& error_msg) {
  executed_nodes_[node_id] = success;

  // Update node state
  auto node = get_node(node_id);
  if(node) {
    node->set_state(success ? NodeState::SUCCESS : NodeState::FAILED);
    if(!success) {
      node->reset_retry_count();
    }
  }

  // If failed, let control flow logic handle error
  if(!success) {
    // Determine error type from error message
    std::string error_type = "UnknownError";
    if(error_msg.find("timeout") != std::string::npos) {
      error_type = "TimeoutError";
    } else if(error_msg.find("memory") != std::string::npos) {
      error_type = "MemoryError";
    } else if(error_msg.find("permission") != std::string::npos) {
      error_type = "PermissionError";
    }

    // Just logging here, actual action taken in get_next_node_after_execution
  }
}



void MMWorkflow::set_error_handler(const std::string&                 node_id,
                                 const ErrorHandlerConfig& handler) {
  error_handlers_[node_id] = handler;
}

void MMWorkflow::reset_execution_state() {
  executed_nodes_.clear();
  node_retry_counts_.clear();
  topological_order_valid_ = false;

  // Reset all node states
  for(auto& node : nodes_) {
    node->set_state(NodeState::READY);
  }
}

bool MMWorkflow::validate_dag() {
  // Check for cycles in workflow graph using DFS coloring
  enum class Color {
    WHITE,  // Not visited
    GRAY,   // Being visited (in recursion stack)
    BLACK   // Visited
  };

  std::map<std::string, Color> colors;

  // Initialize all nodes as WHITE
  for(const auto& node : nodes_) {
    colors[node->get_id()] = Color::WHITE;
  }

  // Build adjacency list for graph
  std::map<std::string, std::vector<std::string>> adjacency;
  for(const auto& edge : edges_) {
    adjacency[edge->get_source()].push_back(edge->get_target());
  }

  // DFS to detect cycles
  std::function<bool(const std::string&)> dfs = [&](const std::string& node_id) -> bool {
    colors[node_id] = Color::GRAY;  // Mark as being visited

    // Visit all neighbors
    if(adjacency.find(node_id) != adjacency.end()) {
      for(const auto& neighbor_id : adjacency[node_id]) {
        if(colors[neighbor_id] == Color::GRAY) {
          // Back edge found - cycle detected
          Logger::instance().error_f("Cycle detected in workflow: %s -> %s", node_id.c_str(),
                                     neighbor_id.c_str());
          return false;
        } else if(colors[neighbor_id] == Color::WHITE) {
          if(!dfs(neighbor_id)) {
            return false;
          }
        }
      }
    }

    colors[node_id] = Color::BLACK;  // Mark as fully visited
    return true;
  };

  // Run DFS from each unvisited node
  for(const auto& node : nodes_) {
    if(colors[node->get_id()] == Color::WHITE) {
      if(!dfs(node->get_id())) {
        return false;
      }
    }
  }

  Logger::instance().info("MMWorkflow graph validation passed - no cycles detected");
  return true;
}

bool MMWorkflow::compute_topological_order() {
  // Use Kahn's algorithm to compute topological order
  std::map<std::string, int> in_degree;

  // Initialize in-degree of all nodes
  for(const auto& node : nodes_) {
    in_degree[node->get_id()] = 0;
  }

  // Calculate in-degree for each node
  for(const auto& edge : edges_) {
    in_degree[edge->get_target()]++;
  }

  // Queue for nodes with zero in-degree
  std::queue<std::string> zero_in_degree;
  for(const auto& node : nodes_) {
    if(in_degree[node->get_id()] == 0) {
      zero_in_degree.push(node->get_id());
    }
  }

  // Build adjacency list
  std::map<std::string, std::vector<std::string>> adjacency;
  for(const auto& edge : edges_) {
    adjacency[edge->get_source()].push_back(edge->get_target());
  }

  // Process nodes in topological order
  topological_order_.clear();
  int processed_count = 0;

  while(!zero_in_degree.empty()) {
    std::string current = zero_in_degree.front();
    zero_in_degree.pop();
    topological_order_.push_back(current);
    processed_count++;

    // Remove all outgoing edges from current node
    if(adjacency.find(current) != adjacency.end()) {
      for(const auto& neighbor : adjacency[current]) {
        in_degree[neighbor]--;
        if(in_degree[neighbor] == 0) {
          zero_in_degree.push(neighbor);
        }
      }
    }
  }

  // Check if topological sort is possible (no cycle)
  if(processed_count != static_cast<int>(nodes_.size())) {
    Logger::instance().error("Failed to compute topological order - cycle detected");
    topological_order_.clear();
    return false;
  }

  topological_order_valid_ = true;

  Logger::instance().info_f("Computed topological order with %zu nodes", topological_order_.size());

  return true;
}

ConditionExpression MMWorkflow::parse_condition_expression(const std::string& expr_str) {
  // Cache lookup — avoid re-parsing the same expression
  auto cache_it = condition_cache_.find(expr_str);
  if(cache_it != condition_cache_.end()) {
    return cache_it->second;
  }

  // Simple parser for condition expressions
  // Format: "left_operand operator right_operand"
  // Operators: ==, !=, >, <, >=, <=, contains, not_contains

  ConditionExpression expr;

  // Parse operator
  if(expr_str.find("==") != std::string::npos) {
    auto pos               = expr_str.find("==");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 2);
    expr.op                = ConditionOperator::EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find("!=") != std::string::npos) {
    auto pos               = expr_str.find("!=");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 2);
    expr.op                = ConditionOperator::NOT_EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find(">=") != std::string::npos) {
    auto pos               = expr_str.find(">=");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 2);
    expr.op                = ConditionOperator::GREATER_EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find("<=") != std::string::npos) {
    auto pos               = expr_str.find("<=");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 2);
    expr.op                = ConditionOperator::LESS_EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find(">") != std::string::npos && expr_str.find(">=") == std::string::npos) {
    auto pos               = expr_str.find(">");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 1);
    expr.op                = ConditionOperator::GREATER_THAN;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find("<") != std::string::npos && expr_str.find("<=") == std::string::npos) {
    auto pos               = expr_str.find("<");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 1);
    expr.op                = ConditionOperator::LESS_THAN;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else if(expr_str.find("contains") != std::string::npos) {
    auto pos               = expr_str.find("contains");
    expr.left_operand      = expr_str.substr(0, pos);
    expr.right_operand     = expr_str.substr(pos + 8);
    expr.op                = ConditionOperator::CONTAINS;
    expr.is_left_variable  = true;
    expr.is_right_variable = true;
  } else {
    // Default: treat as equality
    expr.left_operand      = expr_str;
    expr.right_operand     = "";
    expr.op                = ConditionOperator::EQUAL;
    expr.is_left_variable  = true;
    expr.is_right_variable = false;
  }

  // Trim whitespace
  expr.left_operand.erase(0, expr.left_operand.find_first_not_of(" \t"));
  expr.left_operand.erase(expr.left_operand.find_last_not_of(" \t") + 1);
  expr.right_operand.erase(0, expr.right_operand.find_first_not_of(" \t"));
  expr.right_operand.erase(expr.right_operand.find_last_not_of(" \t") + 1);

  condition_cache_[expr_str] = expr;
  return expr;
}

bool MMWorkflow::evaluate_condition(const ConditionExpression& expr, const ControlContext& context,
                                  const DataMap& node_outputs) const {
  switch(expr.op) {
    case ConditionOperator::AND: {
      if(expr.left_expr && expr.right_expr) {
        return evaluate_condition(*expr.left_expr, context, node_outputs) &&
               evaluate_condition(*expr.right_expr, context, node_outputs);
      }
      return false;
    }

    case ConditionOperator::OR: {
      if(expr.left_expr && expr.right_expr) {
        return evaluate_condition(*expr.left_expr, context, node_outputs) ||
               evaluate_condition(*expr.right_expr, context, node_outputs);
      }
      return false;
    }

    case ConditionOperator::NOT: {
      if(expr.left_expr) {
        return !evaluate_condition(*expr.left_expr, context, node_outputs);
      }
      return false;
    }

    case ConditionOperator::IS_NULL: {
      std::string value = resolve_value(expr.left_operand, context, node_outputs);
      return value.empty();
    }

    case ConditionOperator::IS_NOT_NULL: {
      std::string value = resolve_value(expr.left_operand, context, node_outputs);
      return !value.empty();
    }

    default: {
      std::string left  = resolve_value(expr.left_operand, context, node_outputs);
      std::string right = resolve_value(expr.right_operand, context, node_outputs);
      return evaluate_comparison(left, expr.op, right);
    }
  }
}

bool MMWorkflow::evaluate_comparison(const std::string& left, ConditionOperator op,
                                   const std::string& right) const {
  double left_num   = 0.0;
  double right_num  = 0.0;
  bool   is_numeric = false;

  try {
    left_num   = std::stod(left);
    right_num  = std::stod(right);
    is_numeric = true;
  } catch(const std::exception&) {
    // Not numeric
  }

  switch(op) {
    case ConditionOperator::EQUAL:
      return is_numeric ? (left_num == right_num) : (left == right);

    case ConditionOperator::NOT_EQUAL:
      return is_numeric ? (left_num != right_num) : (left != right);

    case ConditionOperator::GREATER_THAN:
      return is_numeric ? (left_num > right_num) : (left > right);

    case ConditionOperator::LESS_THAN:
      return is_numeric ? (left_num < right_num) : (left < right);

    case ConditionOperator::GREATER_EQUAL:
      return is_numeric ? (left_num >= right_num) : (left >= right);

    case ConditionOperator::LESS_EQUAL:
      return is_numeric ? (left_num <= right_num) : (left <= right);

    case ConditionOperator::CONTAINS:
      return left.find(right) != std::string::npos;

    case ConditionOperator::NOT_CONTAINS:
      return left.find(right) == std::string::npos;

    default:
      return false;
  }
}

std::string MMWorkflow::resolve_value(const std::string& name, const ControlContext& context,
                                    const DataMap& node_outputs) const {
  std::string value;
  if(context.get_variable(name, value)) {
    return value;
  }

  auto it = node_outputs.find(name);
  if(it != node_outputs.end()) {
    return std::visit(
        [](auto&& arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, std::string>) return arg;
          else if constexpr(std::is_same_v<T, int64_t>) return std::to_string(arg);
          else if constexpr(std::is_same_v<T, double>) return std::to_string(arg);
          else if constexpr(std::is_same_v<T, bool>) return arg ? "true" : "false";
          return "";
        },
        it->second);
  }

  return name;
}

ControlAction MMWorkflow::determine_next_action(const NodeId& current_node, NodeState execution_state,
                                              const std::string& error_message) {
  ControlAction action;

  if(execution_state == NodeState::SUCCESS) {
    action.type = ControlActionType::PROCEED;
  } else if(execution_state == NodeState::FAILED) {
    auto it = error_handlers_.find(current_node);
    if(it != error_handlers_.end()) {
      const ErrorHandlerConfig& handler = it->second;

      switch(handler.strategy) {
        case ErrorStrategy::RETRY:
          action.type = ControlActionType::RETRY;
          break;
        case ErrorStrategy::SKIP:
          action.type       = ControlActionType::SKIP;
          action.skip_count = 1;
          break;
        case ErrorStrategy::FAIL:
          action.type   = ControlActionType::ABORT;
          action.reason = error_message;
          break;
        case ErrorStrategy::CATCH:
          action.type           = ControlActionType::GOTO;
          action.target_node_id = handler.handler_node_id;
          break;
        case ErrorStrategy::FALLBACK:
          action.type           = ControlActionType::GOTO;
          action.target_node_id = handler.fallback_node_id;
          break;
      }
    } else {
      action.type   = ControlActionType::ABORT;
      action.reason = error_message;
    }
  }
  return action;
}

ControlAction MMWorkflow::handle_error(const NodeId& failed_node, const std::string& error_type,
                                     const std::string& error_message) {
  auto it = error_handlers_.find(failed_node);
  if(it == error_handlers_.end()) {
    ControlAction action;
    action.type   = ControlActionType::ABORT;
    action.reason = error_message;
    return action;
  }

  const ErrorHandlerConfig& handler = it->second;

  if(!handler.error_types.empty()) {
    bool matches = false;
    for(const auto& type : handler.error_types) {
      if(error_type.find(type) != std::string::npos) {
        matches = true;
        break;
      }
    }
    if(!matches) {
      ControlAction action;
      action.type   = ControlActionType::ABORT;
      action.reason = error_message;
      return action;
    }
  }

  if(handler.condition) {
    if(!evaluate_condition(*handler.condition, control_context_, DataMap())) {
      ControlAction action;
      action.type   = ControlActionType::ABORT;
      action.reason = error_message;
      return action;
    }
  }

  ControlAction action;
  switch(handler.strategy) {
    case ErrorStrategy::RETRY:
      action.type = ControlActionType::RETRY;
      break;
    case ErrorStrategy::SKIP:
      action.type       = ControlActionType::SKIP;
      action.skip_count = 1;
      break;
    case ErrorStrategy::FAIL:
      action.type   = ControlActionType::ABORT;
      action.reason = error_message;
      break;
    case ErrorStrategy::CATCH:
      action.type           = ControlActionType::GOTO;
      action.target_node_id = handler.handler_node_id;
      break;
    case ErrorStrategy::FALLBACK:
      action.type           = ControlActionType::GOTO;
      action.target_node_id = handler.fallback_node_id;
      break;
  }
  return action;
}

utils::storage::StorageResult MMWorkflow::save(const utils::storage::StorageConfig& config,
                                      const utils::storage::VersionInfo&   version_info) {
  auto version_storage = utils::storage::create_versioned_storage(config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  // Convert to JSON
  std::string json_data = workflow_config_to_json(config_, nodes_, edges_);

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

utils::storage::StorageResult MMWorkflow::load(const std::string&            workflow_id,
                                      const utils::storage::StorageConfig& storage_config,
                                      std::shared_ptr<MMWorkflow>&    workflow) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  std::string            json_data;
  utils::storage::StorageResult result = version_storage->load(workflow_id, json_data);
  if(!result.is_success()) {
    return result;
  }

  MMWorkflow::Config          config;
  std::vector<MMNode::Config> node_configs;
  std::vector<MMEdge::Config> edge_configs;

  if(!workflow_config_from_json(json_data, config, node_configs, edge_configs)) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_INVALID_DATA,
                                   "Failed to parse workflow config" };
  }

  // Create workflow
  workflow = std::make_shared<MMWorkflow>(config);

  // Restore nodes
  // Note: We don't have the node factory here, so we can't easily recreate nodes
  // In a real implementation, we would need a NodeFactory to create nodes from configs
  // For now, we just log a warning that nodes are not fully restored
  Logger::instance().warn("Nodes are not fully restored in MMWorkflow::load (missing NodeFactory)");

  return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_OK, "" };
}

utils::storage::StorageResult MMWorkflow::delete_workflow(const std::string&            workflow_id,
                                                 const utils::storage::StorageConfig& storage_config) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  return version_storage->remove(workflow_id);
}

utils::storage::StorageResult MMWorkflow::list_workflows(const utils::storage::StorageConfig& storage_config,
                                                std::vector<std::string>&     workflow_ids) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  return version_storage->list("", workflow_ids);
}

utils::storage::StorageResult MMWorkflow::get_metadata(const std::string&            workflow_id,
                                              const utils::storage::StorageConfig& storage_config,
                                              WorkflowMetadata&             metadata) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  std::string            json_data;
  utils::storage::StorageResult result = version_storage->load(workflow_id, json_data);
  if(!result.is_success()) {
    return result;
  }

  MMWorkflow::Config          config;
  std::vector<MMNode::Config> node_configs;
  std::vector<MMEdge::Config> edge_configs;

  if(!workflow_config_from_json(json_data, config, node_configs, edge_configs)) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_INVALID_DATA,
                                   "Failed to parse workflow config" };
  }

  metadata.id          = config.id;
  metadata.name        = config.name;
  metadata.version     = config.version;
  metadata.description = config.description;
  metadata.dsl_file    = config.dsl_file;
  metadata.created_at  = 0;
  metadata.updated_at  = get_current_timestamp_ms();
  metadata.author      = "";

  return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_OK, "" };
}

void MMWorkflow::enter_loop(const std::string& loop_id, const LoopConfig& config) {
  active_loops_[loop_id] = config;
  control_context_.enter_loop(loop_id, config);
}

void MMWorkflow::exit_loop_iteration(const std::string& loop_id) {
  control_context_.increment_loop_iteration(loop_id);
}

bool MMWorkflow::should_continue_loop(const std::string& loop_id) {
  auto it = active_loops_.find(loop_id);
  if(it == active_loops_.end()) {
    return false;
  }
  return control_context_.should_continue_loop(loop_id, it->second);
}

// ============================================================================
// ControlContext Implementation
// ============================================================================

bool ControlContext::get_variable(const std::string& name, std::string& value) const {
  // Check global variables
  auto it = variables_.find(name);
  if(it != variables_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

void ControlContext::set_variable(const std::string& name, const std::string& value) {
  variables_[name] = value;
}

bool ControlContext::has_variable(const std::string& name) const {
  return variables_.find(name) != variables_.end();
}

void ControlContext::clear_variables() {
  variables_.clear();
}

int ControlContext::get_loop_iteration(const std::string& loop_id) const {
  auto it = loops_.find(loop_id);
  if(it != loops_.end()) {
    return it->second.iteration;
  }
  return 0;
}

void ControlContext::increment_loop_iteration(const std::string& loop_id) {
  auto it = loops_.find(loop_id);
  if(it != loops_.end()) {
    it->second.iteration++;
  }
}

void ControlContext::reset_loop_iteration(const std::string& loop_id) {
  auto it = loops_.find(loop_id);
  if(it != loops_.end()) {
    it->second.iteration = 0;
  }
}

bool ControlContext::should_continue_loop(const std::string& loop_id, const LoopConfig& config) {
  // Simple implementation for now
  int iteration = get_loop_iteration(loop_id);
  
  if(config.max_iterations > 0 && iteration >= config.max_iterations) {
    return false;
  }

  if(config.type == LoopType::FOR_LOOP) {
    long long current = config.start_value + (long long)iteration * config.step;
    if(config.step > 0 && current >= config.end_value) return false;
    if(config.step < 0 && current <= config.end_value) return false;
  }
  
  return true;
}

bool ControlContext::get_loop_variable(const std::string& loop_id, const std::string& var_name,
                                       std::string& value) const {
  auto it = loops_.find(loop_id);
  if(it != loops_.end()) {
    auto var_it = it->second.variables.find(var_name);
    if(var_it != it->second.variables.end()) {
      value = var_it->second;
      return true;
    }
  }
  return false;
}

void ControlContext::set_loop_variable(const std::string& loop_id, const std::string& var_name,
                                       const std::string& value) {
  auto it = loops_.find(loop_id);
  if(it != loops_.end()) {
    it->second.variables[var_name] = value;
  }
}

void ControlContext::enter_loop(const std::string& loop_id, const LoopConfig& config) {
  LoopState state;
  state.config = config;
  state.iteration = 0;
  loops_[loop_id] = state;
}

void ControlContext::exit_loop(const std::string& loop_id) {
  loops_.erase(loop_id);
}

bool ControlContext::is_in_loop(const std::string& loop_id) const {
  return loops_.find(loop_id) != loops_.end();
}

std::vector<std::string> ControlContext::get_active_loops() const {
  std::vector<std::string> active;
  for(const auto& pair : loops_) {
    active.push_back(pair.first);
  }
  return active;
}


}  // namespace framework
}  // namespace agentflow
