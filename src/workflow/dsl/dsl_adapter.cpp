// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DSL Adapter Implementation - Convert THIRD IR format to AgentFlow DSL format

#include "workflow/dsl/dsl_adapter.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "utils/logger.h"

namespace agentflow {
namespace dsl {

// Type aliases for brevity
using NodeId     = agentflow::NodeId;
using WorkflowId = agentflow::WorkflowId;
using EdgeId     = agentflow::EdgeId;

namespace {

// THIRD IR to AF DSL field mappings
constexpr const char* THIRDIR_WORKFLOW_ID       = "workflowId";
constexpr const char* THIRDIR_WORKFLOW_NAME     = "workflowName";
constexpr const char* THIRDIR_WORKFLOW_VERSION  = "workflowVersion";
constexpr const char* THIRDIR_COMPONENTS        = "components";
constexpr const char* THIRDIR_CONNECTIONS       = "connections";
constexpr const char* THIRDIR_COMPONENT_ID      = "id";
constexpr const char* THIRDIR_COMPONENT_TYPE    = "type";
constexpr const char* THIRDIR_COMPONENT_VERSION = "typeVersion";
constexpr const char* THIRDIR_COMPONENT_NAME    = "name";
constexpr const char* THIRDIR_INPUTS            = "inputs";
constexpr const char* THIRDIR_OUTPUTS           = "outputs";
constexpr const char* THIRDIR_CONFIGS           = "configs";
constexpr const char* THIRDIR_CLASS_INFO        = "classInfo";
constexpr const char* THIRDIR_AGENT_ID          = "agentID";
constexpr const char* THIRDIR_AGENT_VERSION     = "agentVersion";
constexpr const char* THIRDIR_WORKFLOW          = "workflow";

// Connection fields
constexpr const char* THIRDIR_SOURCE      = "source";
constexpr const char* THIRDIR_TARGET      = "target";
constexpr const char* THIRDIR_COMPONENT_ID_REF = "componentId";
constexpr const char* THIRDIR_BRANCH_ID   = "branchId";

// Value fields in THIRD IR
constexpr const char* THIRDIR_VALUE_ID          = "id";
constexpr const char* THIRDIR_VALUE_TYPE        = "type";
constexpr const char* THIRDIR_VALUE_DESCRIPTION = "description";

// ClassInfo fields
constexpr const char* THIRDIR_PROGRAM_LANGUAGE = "programLanguage";
constexpr const char* THIRDIR_MODULE_PATH      = "modulePath";
constexpr const char* THIRDIR_CLASS_NAME       = "className";
constexpr const char* THIRDIR_FILE_PATH        = "filePath";

// Execution mode mapping from THIRD IR programLanguage to AF execution_mode
framework::MMNode::ExecutionMode map_execution_mode(const std::string& program_language,
                                                      const std::string& file_path) {
  std::string lang = program_language;
  std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);

  if(lang == "c++" || lang == "cpp") {
    if(!file_path.empty() && file_path.find(".so") != std::string::npos) {
      return framework::MMNode::ExecutionMode::SO_LIBRARY;
    }
    return framework::MMNode::ExecutionMode::CPP_FUNCTION;
  } else if(lang == "python") {
    return framework::MMNode::ExecutionMode::PYTHON_SCRIPT;
  } else if(lang == "go") {
    return framework::MMNode::ExecutionMode::GO_SCRIPT;
  } else if(lang == "java") {
    return framework::MMNode::ExecutionMode::JAVA_JAR;
  } else if(lang == "shell" || lang == "bash") {
    return framework::MMNode::ExecutionMode::SHELL_SCRIPT;
  }

  return framework::MMNode::ExecutionMode::CPP_FUNCTION;
}

framework::MMNode::FieldType parse_thirdir_field_type(const std::string& type) {
  std::string t = type;
  std::transform(t.begin(), t.end(), t.begin(), ::tolower);

  // Handle generic types like List<T>, Map<T>
  if(t.find("int") == 0) return framework::MMNode::FieldType::INT;
  if(t.find("float") == 0) return framework::MMNode::FieldType::FLOAT;
  if(t.find("bool") == 0) return framework::MMNode::FieldType::BOOL;
  if(t.find("list") == 0) return framework::MMNode::FieldType::LIST;
  if(t.find("map") == 0) return framework::MMNode::FieldType::MAP;

  return framework::MMNode::FieldType::STRING;
}

std::string generate_edge_id(const std::string& source, const std::string& target,
                              const std::string& branch = "") {
  std::string id = "edge_" + source + "_to_" + target;
  if(!branch.empty()) {
    id += "_" + branch;
  }
  return id;
}

// Generate AF DSL JSON string from workflow
std::string workflow_to_afdsl_json(std::shared_ptr<framework::MMWorkflow> workflow) {
  if(!workflow) return "{}";

  std::ostringstream ss;
  ss << "{\n";
  ss << "  \"id\": \"" << workflow->get_id() << "\",\n";
  ss << "  \"name\": \"" << workflow->get_name() << "\",\n";
  ss << "  \"version\": \"" << workflow->get_config().version << "\",\n";

  if(!workflow->get_config().description.empty()) {
    ss << "  \"description\": \"" << workflow->get_config().description << "\",\n";
  }

  // Nodes
  ss << "  \"nodes\": [\n";
  const auto& nodes = workflow->get_nodes();
  for(size_t i = 0; i < nodes.size(); ++i) {
    const auto& node = nodes[i];
    ss << "    {\n";
    ss << "      \"id\": \"" << node->get_id() << "\",\n";
    ss << "      \"name\": \"" << node->get_name() << "\",\n";
    ss << "      \"type\": \"" << node->get_type() << "\"";

    // Execution mode
    const auto& config = node->get_config();
    std::string exec_mode = "cpp_function";
    switch(config.execution_mode) {
      case framework::MMNode::ExecutionMode::SHELL_SCRIPT:
        exec_mode = "shell_script"; break;
      case framework::MMNode::ExecutionMode::EXECUTABLE:
        exec_mode = "executable"; break;
      case framework::MMNode::ExecutionMode::GO_SCRIPT:
        exec_mode = "go_script"; break;
      case framework::MMNode::ExecutionMode::PYTHON_SCRIPT:
        exec_mode = "python_script"; break;
      case framework::MMNode::ExecutionMode::JAVA_JAR:
        exec_mode = "java_jar"; break;
      case framework::MMNode::ExecutionMode::SO_LIBRARY:
        exec_mode = "so_library"; break;
      default:
        exec_mode = "cpp_function"; break;
    }
    ss << ",\n      \"execution_mode\": \"" << exec_mode << "\"";

    if(!config.command.empty()) {
      ss << ",\n      \"command\": \"" << config.command << "\"";
    }

    // Input params
    if(!config.input_params.empty()) {
      ss << ",\n      \"input_params\": [";
      for(size_t j = 0; j < config.input_params.size(); ++j) {
        if(j > 0) ss << ", ";
        ss << "\"" << config.input_params[j] << "\"";
      }
      ss << "]";
    }

    // Output params
    if(!config.output_params.empty()) {
      ss << ",\n      \"output_params\": [";
      for(size_t j = 0; j < config.output_params.size(); ++j) {
        if(j > 0) ss << ", ";
        ss << "\"" << config.output_params[j] << "\"";
      }
      ss << "]";
    }

    ss << "\n    }";
    if(i < nodes.size() - 1) ss << ",";
    ss << "\n";
  }
  ss << "  ],\n";

  // Edges
  ss << "  \"edges\": [\n";
  const auto& edges = workflow->get_edges();
  for(size_t i = 0; i < edges.size(); ++i) {
    const auto& edge = edges[i];
    ss << "    {\n";
    ss << "      \"id\": \"" << edge->get_id() << "\",\n";
    ss << "      \"source\": \"" << edge->get_source() << "\",\n";
    ss << "      \"target\": \"" << edge->get_target() << "\"";

    if(edge->has_condition()) {
      ss << ",\n      \"condition_expression\": \"" << edge->get_condition_expression() << "\"";
    }
    if(edge->has_control()) {
      ss << ",\n      \"control\": {";
      std::string ctrl_type = "none";
      switch(edge->get_control_type()) {
        case framework::MMEdge::ControlType::IF:
          ctrl_type = "if"; break;
        case framework::MMEdge::ControlType::SWITCH_CASE:
          ctrl_type = "switch-case"; break;
        default:
          ctrl_type = "none"; break;
      }
      ss << "\"type\": \"" << ctrl_type << "\"";
      if(!edge->get_control_config().expression.empty()) {
        ss << ", \"expression\": \"" << edge->get_control_config().expression << "\"";
      }
      if(!edge->get_control_config().case_value.empty()) {
        ss << ", \"case_value\": \"" << edge->get_control_config().case_value << "\"";
      }
      if(edge->get_control_config().is_default) {
        ss << ", \"is_default\": true";
      }
      ss << "}";
    }

    ss << "\n    }";
    if(i < edges.size() - 1) ss << ",";
    ss << "\n";
  }
  ss << "  ]\n";

  ss << "}\n";
  return ss.str();
}

// Generate AF DSL JSON string from agent
std::string agent_to_afdsl_json(std::shared_ptr<framework::MMAgent> agent) {
  if(!agent) return "{}";

  std::ostringstream ss;
  ss << "{\n";
  ss << "  \"id\": \"" << agent->get_id() << "\",\n";
  ss << "  \"name\": \"" << agent->get_name() << "\",\n";
  ss << "  \"version\": \"" << agent->get_config().version << "\",\n";

  if(!agent->get_config().description.empty()) {
    ss << "  \"description\": \"" << agent->get_config().description << "\",\n";
  }

  ss << "  \"max_workflows\": " << agent->get_config().max_workflows << ",\n";
  ss << "  \"max_concurrent_workflows\": " << agent->get_config().max_concurrent_workflows << ",\n";

  // Workflows
  const auto& workflows = agent->get_workflows();
  ss << "  \"workflows\": [\n";
  for(size_t i = 0; i < workflows.size(); ++i) {
    ss << "    " << workflow_to_afdsl_json(workflows[i]);
    if(i < workflows.size() - 1) ss << ",";
    ss << "\n";
  }
  ss << "  ]\n";

  ss << "}\n";
  return ss.str();
}

}  // anonymous namespace

// ============================================================================
// THIRDIRVersion Implementation
// ============================================================================

THIRDIRVersion THIRDIRVersion::from_string(const std::string& version_str) {
  THIRDIRVersion version;
  std::string s   = version_str;
  size_t      pos = 0;

  size_t dot_pos = s.find('.');
  if(dot_pos != std::string::npos) {
    version.major = std::stoi(s.substr(0, dot_pos));
    pos           = dot_pos + 1;
    s             = s.substr(pos);

    dot_pos = s.find('.');
    if(dot_pos != std::string::npos) {
      version.minor = std::stoi(s.substr(0, dot_pos));
      pos           = dot_pos + 1;
      s             = s.substr(pos);

      if(!s.empty()) {
        version.patch = std::stoi(s);
      }
    } else {
      version.minor = std::stoi(s);
    }
  }

  return version;
}

// ============================================================================
// AdapterResult Implementation
// ============================================================================

std::string AdapterResult::get_error_summary() const {
  if(errors.empty()) {
    return "No errors";
  }

  std::stringstream ss;
  ss << "Conversion failed with " << errors.size() << " error(s):\n";
  for(const auto& error : errors) {
    ss << "  - " << error.to_string() << "\n";
  }
  if(!warnings.empty()) {
    ss << "\n" << warnings.size() << " warning(s):\n";
    for(const auto& warning : warnings) {
      ss << "  - " << warning << "\n";
    }
  }
  return ss.str();
}

// ============================================================================
// DSLAdapter Implementation
// ============================================================================

AdapterResult DSLAdapter::convert_workflow(const std::string& thirdir_content) {
  Logger::instance().info("Converting THIRD IR workflow to AF DSL...");
  current_result_    = AdapterResult();
  current_workflow_  = nullptr;
  current_agent_     = nullptr;
  current_file_.clear();
  component_id_to_name_.clear();

  auto doc = JsonDocumentFactory::create();
  if(!doc->parse(thirdir_content)) {
    current_result_.errors.emplace_back(doc->get_error(), "root");
    return current_result_;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    current_result_.errors.emplace_back("Root must be an object", "root");
    return current_result_;
  }

  if(!parse_thirdir_workflow_root(root, current_result_)) {
    return current_result_;
  }

  return current_result_;
}

AdapterResult DSLAdapter::convert_agent(const std::string& thirdir_content) {
  Logger::instance().info("Converting THIRD IR agent to AF DSL...");
  current_result_    = AdapterResult();
  current_workflow_  = nullptr;
  current_agent_     = nullptr;
  component_id_to_name_.clear();

  auto doc = JsonDocumentFactory::create();
  if(!doc->parse(thirdir_content)) {
    current_result_.errors.emplace_back(doc->get_error(), "root");
    return current_result_;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    current_result_.errors.emplace_back("Root must be an object", "root");
    return current_result_;
  }

  if(!parse_thirdir_agent_root(root, current_result_)) {
    return current_result_;
  }

  return current_result_;
}

AdapterResult DSLAdapter::convert_workflow_file(const std::string& file_path) {
  namespace fs = std::filesystem;

  Logger::instance().info("Converting THIRD IR workflow file: " + file_path);
  current_result_    = AdapterResult();
  current_workflow_  = nullptr;
  current_agent_     = nullptr;
  component_id_to_name_.clear();

  auto doc = JsonDocumentFactory::create();
  if(!doc->parse_file(file_path)) {
    current_result_.errors.emplace_back(doc->get_error(), "file:" + file_path);
    return current_result_;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    current_result_.errors.emplace_back("Root must be an object", "file:" + file_path);
    return current_result_;
  }

  current_file_ = file_path;

  if(!parse_thirdir_workflow_root(root, current_result_)) {
    return current_result_;
  }

  return current_result_;
}

AdapterResult DSLAdapter::convert_agent_file(const std::string& file_path) {
  Logger::instance().info("Converting THIRD IR agent file: " + file_path);
  current_result_    = AdapterResult();
  current_workflow_  = nullptr;
  current_agent_     = nullptr;
  component_id_to_name_.clear();

  auto doc = JsonDocumentFactory::create();
  if(!doc->parse_file(file_path)) {
    current_result_.errors.emplace_back(doc->get_error(), "file:" + file_path);
    return current_result_;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    current_result_.errors.emplace_back("Root must be an object", "file:" + file_path);
    return current_result_;
  }

  current_file_ = file_path;

  if(!parse_thirdir_agent_root(root, current_result_)) {
    return current_result_;
  }

  return current_result_;
}

AdapterResult DSLAdapter::convert_workflow_to_json(const std::string& thirdir_content) {
  auto result = convert_workflow(thirdir_content);
  if(result.is_valid() && result.workflow) {
    result.workflow_json = workflow_to_afdsl_json(result.workflow);
  }
  return result;
}

AdapterResult DSLAdapter::convert_agent_to_json(const std::string& thirdir_content) {
  auto result = convert_agent(thirdir_content);
  if(result.is_valid() && result.agent) {
    result.agent_json = agent_to_afdsl_json(result.agent);
  }
  return result;
}

std::string DSLAdapter::detect_format(const std::string& json_content) {
  auto doc = JsonDocumentFactory::create();
  if(!doc->parse(json_content)) {
    return "unknown";
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    return "unknown";
  }

  // Check for THIRD IR specific fields
  if(root->has_member(THIRDIR_AGENT_ID) ||
     root->has_member(THIRDIR_WORKFLOW_ID) ||
     root->has_member(THIRDIR_COMPONENTS)) {
    return "thirdir";
  }

  // Check for AF DSL specific fields
  if(root->has_member("nodes") || root->has_member("edges")) {
    return "afdsl";
  }

  return "unknown";
}

bool DSLAdapter::validate_thirdir(const std::string& thirdir_content, AdapterResult& result) {
  auto doc = JsonDocumentFactory::create();
  if(!doc->parse(thirdir_content)) {
    result.errors.emplace_back(doc->get_error(), "root");
    return false;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    result.errors.emplace_back("Root must be an object", "root");
    return false;
  }

  // Check for schema version
  auto schema_version = root->get_member("schemaVersion");
  if(schema_version && schema_version->is_string()) {
    THIRDIRVersion version = THIRDIRVersion::from_string(schema_version->as_string());
    if(version.major > 0 || version.minor > 6) {
      result.warnings.push_back("THIRD IR version " + schema_version->as_string() +
                                " may not be fully supported");
    }
  }

  return true;
}

// ============================================================================
// THIRD IR Parsing Implementation
// ============================================================================

bool DSLAdapter::parse_thirdir_workflow_root(std::shared_ptr<const IJsonValue> root,
                                            AdapterResult&                      result) {
  // Parse workflow metadata
  framework::MMWorkflow::Config config;

  // THIRD IR uses workflowId, workflowName, workflowVersion
  config.id = JsonHelper::get_string(root, THIRDIR_WORKFLOW_ID, "");
  if(config.id.empty()) {
    // Try alternative field names
    config.id = JsonHelper::get_string(root, "id", "");
  }
  if(config.id.empty()) {
    result.errors.emplace_back("MMWorkflow missing required 'workflowId' or 'id' field", "root");
    return false;
  }

  config.name = JsonHelper::get_string(root, THIRDIR_WORKFLOW_NAME, "");
  if(config.name.empty()) {
    config.name = JsonHelper::get_string(root, "name", "unnamed_workflow");
  }

  config.version = JsonHelper::get_string(root, THIRDIR_WORKFLOW_VERSION, "1.0");
  if(config.version.empty()) {
    config.version = JsonHelper::get_string(root, "version", "1.0");
  }

  config.description = JsonHelper::get_string(root, "description", "");
  config.dsl_file    = current_file_;

  if(!parse_thirdir_workflow_metadata(root, config, result, "root")) {
    return false;
  }

  // Create workflow object
  current_workflow_ = std::make_shared<framework::MMWorkflow>(config);

  // Parse components (THIRD IR nodes)
  auto components_array = JsonHelper::get_array(root, THIRDIR_COMPONENTS);
  if(components_array) {
    if(!parse_thirdir_components(components_array, result, "root.components")) {
      return false;
    }
  } else {
    result.warnings.push_back("MMWorkflow has no components array");
  }

  // Parse connections (THIRD IR edges)
  auto connections_array = JsonHelper::get_array(root, THIRDIR_CONNECTIONS);
  if(connections_array) {
    if(!parse_thirdir_connections(connections_array, result, "root.connections")) {
      return false;
    }
  }

  result.workflow = current_workflow_;
  return result.errors.empty();
}

bool DSLAdapter::parse_thirdir_agent_root(std::shared_ptr<const IJsonValue> root,
                                        AdapterResult&                     result) {
  // Parse agent config
  framework::MMAgent::Config config;

  // THIRD IR uses agentID
  config.id = JsonHelper::get_string(root, THIRDIR_AGENT_ID, "");
  if(config.id.empty()) {
    config.id = JsonHelper::get_string(root, "id", "");
  }
  if(config.id.empty()) {
    result.errors.emplace_back("MMAgent missing required 'agentID' or 'id' field", "root");
    return false;
  }

  config.name = JsonHelper::get_string(root, "name", config.id);
  config.version = JsonHelper::get_string(root, THIRDIR_AGENT_VERSION, "1.0");
  if(config.version.empty()) {
    config.version = JsonHelper::get_string(root, "version", "1.0");
  }
  config.description = JsonHelper::get_string(root, "description", "");

  if(!parse_thirdir_agent_config(root, config, result, "root")) {
    return false;
  }

  // Create agent object
  current_agent_ = std::make_shared<framework::MMAgent>(config);

  // Parse nested workflow
  auto workflow_obj = JsonHelper::get_object(root, THIRDIR_WORKFLOW);
  if(workflow_obj) {
    // Save current workflow state
    auto saved_workflow = current_workflow_;

    if(!parse_thirdir_workflow_root(workflow_obj, result)) {
      current_workflow_ = saved_workflow;
      return false;
    }

    // Add workflow to agent
    if(current_workflow_) {
      current_agent_->add_workflow(current_workflow_);
    }

    // Restore workflow state
    current_workflow_ = saved_workflow;
  }

  result.agent = current_agent_;
  return result.errors.empty();
}

bool DSLAdapter::parse_thirdir_components(std::shared_ptr<const IJsonValue> components_array,
                                        AdapterResult&                     result,
                                        const std::string&                 path) {
  size_t num_components = components_array->array_size();
  if(num_components == 0) {
    result.warnings.push_back("Components array is empty");
    return true;
  }

  for(size_t i = 0; i < num_components; ++i) {
    auto component_obj = components_array->array_at(i);
    if(!component_obj || !component_obj->is_object()) {
      result.errors.emplace_back("Component at index " + std::to_string(i) + " is not an object",
                                  path + "[" + std::to_string(i) + "]");
      return false;
    }

    if(!parse_thirdir_component(component_obj, result, path + "[" + std::to_string(i) + "]")) {
      return false;
    }
  }

  return true;
}

bool DSLAdapter::parse_thirdir_component(std::shared_ptr<const IJsonValue> component_obj,
                                       AdapterResult&                     result,
                                       const std::string&                 path) {
  framework::MMNode::Config config;

  // Parse component ID
  config.id = JsonHelper::get_string(component_obj, THIRDIR_COMPONENT_ID, "");
  if(config.id.empty()) {
    result.errors.emplace_back("Component missing required 'id' field", path);
    return false;
  }

  // Reserved keywords check (from THIRD IR spec)
  std::unordered_set<std::string> reserved = {"start", "end", "input", "output",
                                                "result", "_finish", "_sink"};
  if(reserved.find(config.id) != reserved.end()) {
    result.errors.emplace_back("Component ID uses reserved keyword: " + config.id, path);
    return false;
  }

  // Parse component type
  config.type = JsonHelper::get_string(component_obj, THIRDIR_COMPONENT_TYPE, "default");

  // Parse component name (optional in THIRD IR)
  config.name = JsonHelper::get_string(component_obj, THIRDIR_COMPONENT_NAME, config.id);

  // Parse classInfo for execution mode (optional in Component Instance)
  // classInfo is defined in Component Type, not in Component Instance
  // But for convenience, we allow it in Component Instance as well
  auto class_info_obj = JsonHelper::get_object(component_obj, THIRDIR_CLASS_INFO);
  if(class_info_obj) {
    if(!parse_thirdir_component_class_info(class_info_obj, config, result, path + ".classInfo")) {
      return false;
    }
  }

  // Parse inputs - Component Instance uses Map format (THIRD IR 6.2)
  // Try Map format first (Component Instance), then fallback to array format (Component Type)
  auto inputs_obj = JsonHelper::get_object(component_obj, THIRDIR_INPUTS);
  if(inputs_obj) {
    // Component Instance: inputs is a Map (key-value pairs)
    if(!parse_thirdir_value_map(inputs_obj, config.input_params, config.input_definitions,
                              result, path + ".inputs")) {
      return false;
    }
  } else {
    // Fallback: Component Type uses array format
    auto inputs_array = JsonHelper::get_array(component_obj, THIRDIR_INPUTS);
    if(inputs_array) {
      if(!parse_thirdir_value_array(inputs_array, config.input_params, config.input_definitions,
                                  result, path + ".inputs")) {
        return false;
      }
    }
  }

  // Parse outputs - Component Instance uses Map format (THIRD IR 6.2)
  auto outputs_obj = JsonHelper::get_object(component_obj, THIRDIR_OUTPUTS);
  if(outputs_obj) {
    // Component Instance: outputs is a Map (key-value pairs)
    if(!parse_thirdir_value_map(outputs_obj, config.output_params, config.output_definitions,
                              result, path + ".outputs")) {
      return false;
    }
  } else {
    // Fallback: Component Type uses array format
    auto outputs_array = JsonHelper::get_array(component_obj, THIRDIR_OUTPUTS);
    if(outputs_array) {
      if(!parse_thirdir_value_array(outputs_array, config.output_params, config.output_definitions,
                                  result, path + ".outputs")) {
        return false;
      }
    }
  }

  // Parse configs - Component Instance uses Map format (THIRD IR 6.2)
  auto configs_obj = JsonHelper::get_object(component_obj, THIRDIR_CONFIGS);
  if(configs_obj && configs_obj->object_size() > 0) {
    result.warnings.push_back("Component '" + config.id + "' has configs field which is not "
                              "directly supported; consider using node input parameters");
  } else {
    auto configs_array = JsonHelper::get_array(component_obj, THIRDIR_CONFIGS);
    if(configs_array && configs_array->array_size() > 0) {
      result.warnings.push_back("Component '" + config.id + "' has configs field which is not "
                                "directly supported; consider using node input parameters");
    }
  }

  // Check for switchCase type (branch component)
  if(config.type == "switchCase") {
    // Handle switchCase component specially
    auto branches_array = JsonHelper::get_array(component_obj, "branches");
    if(branches_array) {
      // Store branch information for edge creation
      // This will be handled during connection parsing
    }
  }

  // Create node and add to workflow
  auto node = std::make_shared<framework::MMNode>(config);
  current_workflow_->add_node(node);

  // Track component ID to name mapping
  component_id_to_name_[config.id] = config.name;

  return true;
}

bool DSLAdapter::parse_thirdir_component_class_info(std::shared_ptr<const IJsonValue> class_info_obj,
                                                   framework::MMNode::Config&          config,
                                                   AdapterResult&                      result,
                                                   const std::string&                  path) {
  std::string program_language = JsonHelper::get_string(class_info_obj, THIRDIR_PROGRAM_LANGUAGE, "");
  std::string file_path        = JsonHelper::get_string(class_info_obj, THIRDIR_FILE_PATH, "");
  std::string class_name       = JsonHelper::get_string(class_info_obj, THIRDIR_CLASS_NAME, "");
  std::string module_path      = JsonHelper::get_string(class_info_obj, THIRDIR_MODULE_PATH, "");

  if(program_language.empty()) {
    result.warnings.push_back("Component classInfo missing programLanguage, defaulting to cpp_function");
    program_language = "cpp";
  }

  config.execution_mode = map_execution_mode(program_language, file_path);

  // Set command based on execution mode
  if(!file_path.empty()) {
    config.command = file_path;
  } else if(!module_path.empty()) {
    config.command = module_path;
  }

  // Set function/class name for SO/JAR execution
  if(!class_name.empty()) {
    config.function_name = class_name;
    config.main_class    = class_name;
  }

  return true;
}

bool DSLAdapter::parse_thirdir_value_array(std::shared_ptr<const IJsonValue> values_array,
                                         std::vector<std::string>&          param_names,
                                         std::vector<framework::MMNode::FieldDefinition>& field_defs,
                                         AdapterResult&                     result,
                                         const std::string&                 path) {
  size_t num_values = values_array->array_size();

  for(size_t i = 0; i < num_values; ++i) {
    auto value_obj = values_array->array_at(i);
    if(!value_obj) {
      continue;
    }

    framework::MMNode::FieldDefinition field;

    if(value_obj->is_string()) {
      // Simple string value
      std::string name = value_obj->as_string();
      field.name = name;
      field.type = framework::MMNode::FieldType::STRING;
    } else if(value_obj->is_object()) {
      // Complex value object (Component Type format)
      field.name = JsonHelper::get_string(value_obj, THIRDIR_VALUE_ID, "");
      if(field.name.empty()) {
        result.errors.emplace_back("Value missing required 'id' field",
                                    path + "[" + std::to_string(i) + "]");
        return false;
      }

      std::string type_str = JsonHelper::get_string(value_obj, THIRDIR_VALUE_TYPE, "String");
      field.type = parse_thirdir_field_type(type_str);
      field.required = false;  // THIRD IR doesn't have required field for values
    } else {
      // Skip invalid entries
      continue;
    }

    param_names.push_back(field.name);
    field_defs.push_back(field);
  }

  return true;
}

bool DSLAdapter::parse_thirdir_value_map(std::shared_ptr<const IJsonValue> values_map,
                                       std::vector<std::string>&          param_names,
                                       std::vector<framework::MMNode::FieldDefinition>& field_defs,
                                       AdapterResult&                     result,
                                       const std::string&                 path) {
  // Component Instance format: inputs/outputs are Map (key-value pairs)
  // The key is the field name, the value is the field value (can be any type)

  if(!values_map->is_object()) {
    result.errors.emplace_back("Expected object for value map", path);
    return false;
  }

  auto keys = values_map->object_keys();
  for(const auto& key : keys) {
    framework::MMNode::FieldDefinition field;
    field.name = key;

    // Try to determine the type from the value
    auto value = values_map->get_member(key);
    if(value) {
      if(value->is_string()) {
        field.type = framework::MMNode::FieldType::STRING;
      } else if(value->is_number()) {
        // Check if it's an integer or float
        double d = value->as_double();
        if(d == static_cast<int64_t>(d)) {
          field.type = framework::MMNode::FieldType::INT;
        } else {
          field.type = framework::MMNode::FieldType::FLOAT;
        }
      } else if(value->is_bool()) {
        field.type = framework::MMNode::FieldType::BOOL;
      } else if(value->is_array()) {
        field.type = framework::MMNode::FieldType::LIST;
      } else if(value->is_object()) {
        field.type = framework::MMNode::FieldType::MAP;
      } else {
        field.type = framework::MMNode::FieldType::STRING;
      }
    } else {
      field.type = framework::MMNode::FieldType::STRING;
    }

    field.required = false;

    param_names.push_back(field.name);
    field_defs.push_back(field);
  }

  return true;
}

bool DSLAdapter::parse_thirdir_connections(std::shared_ptr<const IJsonValue> connections_array,
                                         AdapterResult&                      result,
                                         const std::string&                  path) {
  size_t num_connections = connections_array->array_size();

  for(size_t i = 0; i < num_connections; ++i) {
    auto connection_obj = connections_array->array_at(i);
    if(!connection_obj || !connection_obj->is_object()) {
      result.errors.emplace_back("Connection at index " + std::to_string(i) + " is not an object",
                                  path + "[" + std::to_string(i) + "]");
      return false;
    }

    if(!parse_thirdir_connection(connection_obj, result, path + "[" + std::to_string(i) + "]")) {
      return false;
    }
  }

  return true;
}

bool DSLAdapter::parse_thirdir_connection(std::shared_ptr<const IJsonValue> connection_obj,
                                        AdapterResult&                     result,
                                        const std::string&                 path) {
  // Parse source
  auto source_obj = JsonHelper::get_object(connection_obj, THIRDIR_SOURCE);
  if(!source_obj) {
    result.errors.emplace_back("Connection missing required 'source' object", path);
    return false;
  }

  std::string source_id = JsonHelper::get_string(source_obj, THIRDIR_COMPONENT_ID_REF, "");
  if(source_id.empty()) {
    result.errors.emplace_back("Connection source missing 'componentId'", path + ".source");
    return false;
  }

  std::string source_branch = JsonHelper::get_string(source_obj, THIRDIR_BRANCH_ID, "");

  // Parse target
  auto target_obj = JsonHelper::get_object(connection_obj, THIRDIR_TARGET);
  if(!target_obj) {
    result.errors.emplace_back("Connection missing required 'target' object", path);
    return false;
  }

  std::string target_id = JsonHelper::get_string(target_obj, THIRDIR_COMPONENT_ID_REF, "");
  if(target_id.empty()) {
    result.errors.emplace_back("Connection target missing 'componentId'", path + ".target");
    return false;
  }

  // Check that source != target
  if(source_id == target_id) {
    result.errors.emplace_back("Connection source and target cannot be the same: " + source_id, path);
    return false;
  }

  // Create edge config
  framework::MMEdge::Config config;
  config.id          = generate_edge_id(source_id, target_id, source_branch);
  config.source_node = source_id;
  config.target_node = target_id;

  // Handle branch (condition) for switchCase
  if(!source_branch.empty()) {
    config.condition_expression = source_branch;
    config.control.type         = framework::MMEdge::ControlType::SWITCH_CASE;
    config.control.case_value   = source_branch;

    if(source_branch == "default") {
      config.control.is_default = true;
    }
  }

  // Create edge and add to workflow
  auto edge = std::make_shared<framework::MMEdge>(config);
  current_workflow_->add_edge(edge);

  return true;
}

bool DSLAdapter::parse_thirdir_workflow_metadata(std::shared_ptr<const IJsonValue> obj,
                                               framework::MMWorkflow::Config&     config,
                                               AdapterResult&                     result,
                                               const std::string&                 path) {
  // Parse schema version
  auto schema_version = obj->get_member("schemaVersion");
  if(schema_version && schema_version->is_string()) {
    THIRDIRVersion version = THIRDIRVersion::from_string(schema_version->as_string());
    if(version.major > 0 || version.minor > 6) {
      result.warnings.push_back("THIRD IR version " + schema_version->as_string() +
                                " may not be fully supported");
    }
  }

  // Parse config object if present
  auto config_obj = JsonHelper::get_object(obj, "config");
  if(config_obj) {
    auto keys = config_obj->object_keys();
    for(const auto& key : keys) {
      auto value = config_obj->get_member(key);
      if(value && value->is_string()) {
        config.metadata[key] = value->as_string();
      }
    }
  }

  return true;
}

bool DSLAdapter::parse_thirdir_agent_config(std::shared_ptr<const IJsonValue> obj,
                                          framework::MMAgent::Config&        config,
                                          AdapterResult&                     result,
                                          const std::string&                 path) {
  // THIRD IR agents don't have direct max_workflows or max_concurrent_workflows fields
  // Use defaults
  config.max_workflows            = JsonHelper::get_int(obj, "max_workflows", 10);
  config.max_concurrent_workflows = JsonHelper::get_int(obj, "max_concurrent_workflows", 5);
  config.data_bus_capacity        = JsonHelper::get_int(obj, "data_bus_capacity", 1024 * 1024);

  // Parse lifecycle policy if present
  std::string policy_str = JsonHelper::get_string(obj, "lifecycle_policy", "factory");
  std::transform(policy_str.begin(), policy_str.end(), policy_str.begin(), ::tolower);

  if(policy_str == "per_request") {
    config.policy = framework::MMAgent::LifecyclePolicy::PER_REQUEST;
  } else if(policy_str == "singleton") {
    config.policy = framework::MMAgent::LifecyclePolicy::SINGLETON;
  } else {
    config.policy = framework::MMAgent::LifecyclePolicy::FACTORY;
  }

  return true;
}

bool DSLAdapter::validate_thirdir_workflow_structure(std::shared_ptr<framework::MMWorkflow> workflow,
                                                   AdapterResult&                         result) {
  if(workflow->get_nodes().empty()) {
    result.warnings.push_back("MMWorkflow has no nodes");
  }

  if(!workflow->validate()) {
    result.errors.emplace_back("MMWorkflow graph validation failed - workflow must be acyclic");
    return false;
  }

  return true;
}

bool DSLAdapter::validate_thirdir_agent_structure(std::shared_ptr<framework::MMAgent> agent,
                                                AdapterResult&                       result) {
  if(agent->get_workflows().empty()) {
    result.warnings.push_back("MMAgent has no workflows");
  }

  return true;
}

// ============================================================================
// DSLAdapterFactory Implementation
// ============================================================================

std::unique_ptr<DSLAdapter> DSLAdapterFactory::create() {
  return std::make_unique<DSLAdapter>();
}

std::string DSLAdapterFactory::get_supported_thirdir_version() {
  return "0.6";
}

std::string DSLAdapterFactory::get_output_afdsl_version() {
  return "1.0";
}

}  // namespace dsl
}  // namespace agentflow