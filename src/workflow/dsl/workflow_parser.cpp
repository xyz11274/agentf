// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DSL MMWorkflow Parser Implementation

#include "workflow/dsl/workflow_parser.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <unordered_set>

#include "utils/logger.h"
#include "workflow/dsl/json_adapter.h"
#include "agent/agent.h"
#include "workflow/edge.h"
#include "workflow/node.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace dsl {

// Type aliases for brevity
using NodeId     = agentflow::NodeId;
using WorkflowId = agentflow::WorkflowId;
using EdgeId     = agentflow::EdgeId;

namespace {
framework::MMEdge::ControlType parse_edge_control_type(const std::string& type) {
  if(type == "if") return framework::MMEdge::ControlType::IF;
  if(type == "switch-case") return framework::MMEdge::ControlType::SWITCH_CASE;
  return framework::MMEdge::ControlType::NONE;
}

framework::MMNode::FieldType parse_node_field_type(const std::string& type) {
  if(type == "int") return framework::MMNode::FieldType::INT;
  if(type == "float") return framework::MMNode::FieldType::FLOAT;
  if(type == "bool") return framework::MMNode::FieldType::BOOL;
  if(type == "list") return framework::MMNode::FieldType::LIST;
  if(type == "map") return framework::MMNode::FieldType::MAP;
  return framework::MMNode::FieldType::STRING;
}

framework::MMNode::TransferMode parse_node_transfer_mode(const std::string& mode) {
  if(mode == "ref" || mode == "reference") {
    return framework::MMNode::TransferMode::REFERENCE;
  }
  return framework::MMNode::TransferMode::VALUE;
}
}  // namespace

// ============================================================================
// ParseError Implementation
// ============================================================================

std::string ParseResult::get_error_summary() const {
  if(errors.empty()) {
    return "No errors";
  }

  std::stringstream ss;
  ss << "Parse failed with " << errors.size() << " error(s):\n";
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
// DslVersion Implementation
// ============================================================================

DslVersion DslVersion::from_string(const std::string& version_str) {
  DslVersion  version;
  std::string s   = version_str;
  size_t      pos = 0;

  // Parse major
  size_t dot_pos = s.find('.');
  if(dot_pos != std::string::npos) {
    version.major = std::stoi(s.substr(0, dot_pos));
    pos           = dot_pos + 1;
    s             = s.substr(pos);

    // Parse minor
    dot_pos = s.find('.');
    if(dot_pos != std::string::npos) {
      version.minor = std::stoi(s.substr(0, dot_pos));
      pos           = dot_pos + 1;
      s             = s.substr(pos);

      // Parse patch
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
// WorkflowParser Implementation
// ============================================================================

ParseResult WorkflowParser::parse_workflow(const std::string& json_content) {
  Logger::instance().info("Parsing workflow content...");
  ParseResult result;

  current_result_   = ParseResult();
  current_workflow_ = nullptr;
  current_agent_    = nullptr;
  current_workflow_file_.clear();
  node_id_to_name_.clear();

  auto doc = JsonDocumentFactory::create();
  if(!doc->parse(json_content)) {
    result.errors.emplace_back(doc->get_error(), "root");
    return result;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    result.errors.emplace_back("Root must be an object", "root");
    return result;
  }

  if(!parse_workflow_root(root, current_result_)) {
    return current_result_;
  }

  result = current_result_;
  return result;
}

ParseResult WorkflowParser::parse_agent(const std::string& json_content) {
  ParseResult result;
  current_result_   = ParseResult();
  current_workflow_ = nullptr;
  current_agent_    = nullptr;
  node_id_to_name_.clear();

  auto doc = JsonDocumentFactory::create();
  if(!doc->parse(json_content)) {
    result.errors.emplace_back(doc->get_error(), "root");
    return result;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    result.errors.emplace_back("Root must be an object", "root");
    return result;
  }

  if(!parse_agent_root(root, current_result_)) {
    return current_result_;
  }

  result = current_result_;
  return result;
}

ParseResult WorkflowParser::parse_workflow_file(const std::string& file_path) {
  namespace fs = std::filesystem;
  ParseResult result;
  current_result_   = ParseResult();
  current_workflow_ = nullptr;
  current_agent_    = nullptr;
  node_id_to_name_.clear();

  const std::string normalized_path = fs::absolute(fs::path(file_path)).lexically_normal().string();
  if(include_guard_.count(normalized_path) > 0) {
    result.errors.emplace_back("Detected recursive include cycle: " + normalized_path,
                               "file:" + file_path);
    return result;
  }
  include_guard_.insert(normalized_path);
  const std::string previous_file = current_workflow_file_;
  current_workflow_file_          = normalized_path;

  auto doc = JsonDocumentFactory::create();
  if(!doc->parse_file(file_path)) {
    include_guard_.erase(normalized_path);
    current_workflow_file_ = previous_file;
    result.errors.emplace_back(doc->get_error(), "file:" + file_path);
    return result;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    include_guard_.erase(normalized_path);
    current_workflow_file_ = previous_file;
    result.errors.emplace_back("Root must be an object", "file:" + file_path);
    return result;
  }

  if(!parse_workflow_root(root, current_result_)) {
    include_guard_.erase(normalized_path);
    current_workflow_file_ = previous_file;
    return current_result_;
  }

  result = current_result_;
  include_guard_.erase(normalized_path);
  current_workflow_file_ = previous_file;
  return result;
}

ParseResult WorkflowParser::parse_agent_file(const std::string& file_path) {
  ParseResult result;
  current_result_   = ParseResult();
  current_workflow_ = nullptr;
  current_agent_    = nullptr;
  node_id_to_name_.clear();

  auto doc = JsonDocumentFactory::create();
  if(!doc->parse_file(file_path)) {
    result.errors.emplace_back(doc->get_error(), "file:" + file_path);
    return result;
  }

  auto root = doc->root();
  if(!root || !root->is_object()) {
    result.errors.emplace_back("Root must be an object", "file:" + file_path);
    return result;
  }

  if(!parse_agent_root(root, current_result_)) {
    return current_result_;
  }

  result = current_result_;
  return result;
}

bool WorkflowParser::validate_workflow(std::shared_ptr<framework::MMWorkflow> workflow,
                                       ParseResult&                         result) {
  if(!workflow) {
    result.errors.emplace_back("MMWorkflow is null");
    return false;
  }

  return validate_workflow_structure(workflow, result);
}

bool WorkflowParser::validate_agent(std::shared_ptr<framework::MMAgent> agent, ParseResult& result) {
  if(!agent) {
    result.errors.emplace_back("MMAgent is null");
    return false;
  }

  // Basic validation - agent must have at least one workflow
  const auto& workflows = agent->get_workflows();
  if(workflows.empty()) {
    result.warnings.push_back("MMAgent has no workflows");
  }

  return true;
}

bool WorkflowParser::parse_workflow_root(std::shared_ptr<const IJsonValue> root,
                                         ParseResult&                      result) {
  namespace fs = std::filesystem;
  // Check DSL version
  auto dsl_version_obj = root->get_member("dsl_version");
  if(dsl_version_obj && dsl_version_obj->is_string()) {
    std::string version_str = dsl_version_obj->as_string();
    DslVersion  version     = DslVersion::from_string(version_str);
    if(version.major > 1) {
      result.warnings.push_back("DSL version " + version_str + " may not be fully supported");
    }
  }

  // Parse metadata
  framework::MMWorkflow::Config config;
  config.id = JsonHelper::get_string(root, "id", "");
  if(config.id.empty()) {
    config.id = "workflow_" + std::to_string(std::rand());
  }

  config.name        = JsonHelper::get_string(root, "name", "unnamed_workflow");
  config.version     = JsonHelper::get_string(root, "version", "1.0");
  config.description = JsonHelper::get_string(root, "description", "");
  config.dsl_file    = JsonHelper::get_string(root, "dsl_file", "");

  if(!parse_workflow_metadata(root, config, result, "root")) {
    return false;
  }

  // Create workflow object
  current_workflow_ = std::make_shared<framework::MMWorkflow>(config);

  // Parse includes and merge nested workflows
  auto includes_array = JsonHelper::get_array(root, "includes");
  if(includes_array) {
    const fs::path base_dir = current_workflow_file_.empty()
                                  ? fs::current_path()
                                  : fs::path(current_workflow_file_).parent_path();
    const size_t include_count = includes_array->array_size();
    for(size_t i = 0; i < include_count; ++i) {
      auto include_item = includes_array->array_at(i);
      if(!include_item || !include_item->is_string()) {
        result.errors.emplace_back("includes item must be string",
                                   "root.includes[" + std::to_string(i) + "]");
        return false;
      }
      fs::path include_path(include_item->as_string());
      if(include_path.is_relative()) {
        include_path = base_dir / include_path;
      }
      include_path = fs::absolute(include_path).lexically_normal();

      WorkflowParser include_parser;
      include_parser.include_guard_ = include_guard_;
      auto include_result = include_parser.parse_workflow_file(include_path.string());
      include_guard_      = include_parser.include_guard_;
      if(!include_result.is_valid() || !include_result.workflow) {
        result.errors.emplace_back("Failed to parse include workflow: " + include_path.string(),
                                   "root.includes[" + std::to_string(i) + "]");
        if(!include_result.errors.empty()) {
          result.errors.push_back(include_result.errors.front());
        }
        return false;
      }

      for(const auto& node : include_result.workflow->get_nodes()) {
        if(node && !current_workflow_->get_node(node->get_id())) {
          current_workflow_->add_node(node);
          node_id_to_name_[node->get_id()] = node->get_name();
        }
      }

      for(const auto& edge : include_result.workflow->get_edges()) {
        if(!edge) {
          continue;
        }
        bool exists = false;
        for(const auto& existing_edge : current_workflow_->get_edges()) {
          if(existing_edge && existing_edge->get_id() == edge->get_id()) {
            exists = true;
            break;
          }
        }
        if(!exists) {
          current_workflow_->add_edge(edge);
        }
      }
    }
  }


  // Parse nodes
  auto nodes_array = JsonHelper::get_array(root, "nodes");
  if(nodes_array) {
    if(!parse_nodes(nodes_array, result, "root.nodes")) {
      return false;
    }
  } else {
    // If nodes are missing, it might be a valid partial workflow definition (e.g. metadata only)
    // especially when used inside an MMAgent definition.
    // So we don't fail here, but validation might warn or fail depending on context.
  }

  // Parse edges
  auto edges_array = JsonHelper::get_array(root, "edges");
  if(edges_array) {
    if(!parse_edges(edges_array, result, "root.edges")) {
      return false;
    }
  }

  // Set workflow in result before validation so it's available even if validation fails
  result.workflow = current_workflow_;

  // Validate
  if(!validate_node_ids_unique(result)) {
    return false;
  }

  if(!validate_edge_references(current_workflow_, result)) {
    return false;
  }

  // Final validation
  if(!validate_workflow_structure(current_workflow_, result)) {
    return false;
  }

  result.workflow = current_workflow_;
  return result.errors.empty();
}

bool WorkflowParser::parse_agent_root(std::shared_ptr<const IJsonValue> root, ParseResult& result) {
  // Parse agent config
  framework::MMAgent::Config config;
  config.id = JsonHelper::get_string(root, "id", "");
  if(config.id.empty()) {
    result.errors.emplace_back("MMAgent missing required 'id' field", "root");
    return false;
  }

  config.name        = JsonHelper::get_string(root, "name", "unnamed_agent");
  config.version     = JsonHelper::get_string(root, "version", "1.0");
  config.description = JsonHelper::get_string(root, "description", "");

  // Parse lifecycle policy
  std::string policy_str = JsonHelper::get_string(root, "lifecycle_policy", "");
  if(policy_str.empty()) {
    policy_str = JsonHelper::get_string(root, "policy", "factory");
  }
  
  // Convert to lowercase for comparison
  std::transform(policy_str.begin(), policy_str.end(), policy_str.begin(), ::tolower);
  
  if(policy_str == "per_request") {
    config.policy = framework::MMAgent::LifecyclePolicy::PER_REQUEST;
  } else if(policy_str == "singleton") {
    config.policy = framework::MMAgent::LifecyclePolicy::SINGLETON;
  } else if(policy_str == "factory") {
    config.policy = framework::MMAgent::LifecyclePolicy::FACTORY;
  } else {
    result.errors.emplace_back("Invalid agent policy: " + policy_str, "root.policy");
    return false;
  }

  // Parse resource limits
  config.max_workflows            = JsonHelper::get_int(root, "max_workflows", 10);
  if(config.max_workflows <= 0) {
    result.errors.emplace_back("max_workflows must be positive", "root.max_workflows");
    return false;
  }

  config.max_concurrent_workflows = JsonHelper::get_int(root, "max_concurrent_workflows", 5);
  if(config.max_concurrent_workflows <= 0) {
    result.errors.emplace_back("max_concurrent_workflows must be positive", "root.max_concurrent_workflows");
    return false;
  }

  config.data_bus_capacity        = JsonHelper::get_int(root, "data_bus_capacity", 1024 * 1024);
  if(config.data_bus_capacity <= 0) {
    result.errors.emplace_back("data_bus_capacity must be positive", "root.data_bus_capacity");
    return false;
  }

  if(!parse_agent_config(root, config, result, "root")) {
    return false;
  }

  // Create agent object
  current_agent_ = std::make_shared<framework::MMAgent>(config);

  // Parse workflows
  auto workflows_array = JsonHelper::get_array(root, "workflows");
  if(workflows_array) {
    if(!parse_agent_workflows(workflows_array, result, "root.workflows")) {
      return false;
    }
  }

  // Validate
  if(!validate_agent(current_agent_, result)) {
    return false;
  }

  result.agent = current_agent_;
  return result.errors.empty();
}

bool WorkflowParser::parse_workflow_metadata(std::shared_ptr<const IJsonValue> obj,
                                             framework::MMWorkflow::Config&      config,
                                             ParseResult& /* result */,
                                             const std::string& /* path */) {
  // Parse metadata map
  auto metadata_obj = JsonHelper::get_object(obj, "metadata");
  if(metadata_obj) {
    auto keys = metadata_obj->object_keys();
    for(const auto& key : keys) {
      auto value = metadata_obj->get_member(key);
      if(value && value->is_string()) {
        config.metadata[key] = value->as_string();
      }
    }
  }

  return true;
}

bool WorkflowParser::parse_nodes(std::shared_ptr<const IJsonValue> nodes_array, ParseResult& result,
                                 const std::string& path) {
  size_t num_nodes = nodes_array->array_size();
  if(num_nodes == 0) {
    result.errors.emplace_back("Nodes array cannot be empty", path);
    return false;
  }

  for(size_t i = 0; i < num_nodes; ++i) {
    auto node_obj = nodes_array->array_at(i);
    if(!node_obj || !node_obj->is_object()) {
      result.errors.emplace_back("MMNode at index " + std::to_string(i) + " is not an object",
                                 path + "[" + std::to_string(i) + "]");
      return false;
    }

    if(!parse_node(node_obj, result, path + "[" + std::to_string(i) + "]")) {
      return false;
    }
  }

  return true;
}

bool WorkflowParser::parse_edges(std::shared_ptr<const IJsonValue> edges_array, ParseResult& result,
                                 const std::string& path) {
  size_t num_edges = edges_array->array_size();

  for(size_t i = 0; i < num_edges; ++i) {
    auto edge_obj = edges_array->array_at(i);
    if(!edge_obj || !edge_obj->is_object()) {
      result.errors.emplace_back("MMEdge at index " + std::to_string(i) + " is not an object",
                                 path + "[" + std::to_string(i) + "]");
      return false;
    }

    if(!parse_edge(edge_obj, result, path + "[" + std::to_string(i) + "]")) {
      return false;
    }
  }

  return true;
}

bool WorkflowParser::parse_node(std::shared_ptr<const IJsonValue> node_obj, ParseResult& result,
                                const std::string& path) {
  framework::MMNode::Config config;

  if(!parse_node_config(node_obj, config, result, path)) {
    return false;
  }

  if(!parse_node_inputs(node_obj, config, result, path)) {
    return false;
  }

  if(!parse_node_outputs(node_obj, config, result, path)) {
    return false;
  }

  // Create node and add to workflow
  auto node = std::make_shared<framework::MMNode>(config);
  current_workflow_->add_node(node);

  // Track node ID mapping
  node_id_to_name_[config.id] = config.name;

  return true;
}

bool WorkflowParser::parse_node_config(std::shared_ptr<const IJsonValue> node_obj,
                                       framework::MMNode::Config& config, ParseResult& result,
                                       const std::string& path) {
  config.id = JsonHelper::get_string(node_obj, "id", "");
  if(config.id.empty()) {
    result.errors.emplace_back("MMNode missing required 'id' field", path);
    return false;
  }

  config.name            = JsonHelper::get_string(node_obj, "name", config.id);
  config.type            = JsonHelper::get_string(node_obj, "type", "default");

  // Check for nested config object
  auto nested_config_obj = JsonHelper::get_object(node_obj, "config");
  auto config_source     = nested_config_obj ? nested_config_obj : node_obj;

  config.priority        = static_cast<int>(JsonHelper::get_int(config_source, "priority", 0));
  config.max_retries     = static_cast<int>(JsonHelper::get_int(config_source, "max_retries", 3));
  config.timeout_ms      = static_cast<int>(JsonHelper::get_int(config_source, "timeout_ms", 30000));
  config.memory_limit_mb = static_cast<int>(JsonHelper::get_int(config_source, "memory_limit_mb", 0));
  config.cpu_limit       = static_cast<int>(JsonHelper::get_int(config_source, "cpu_limit", 0));

  // Parse execution mode
  std::string exec_mode_str = JsonHelper::get_string(config_source, "execution_mode", "cpp_function");
  if(exec_mode_str == "shell_script") {
    config.execution_mode = framework::MMNode::ExecutionMode::SHELL_SCRIPT;
  } else if(exec_mode_str == "executable") {
    config.execution_mode = framework::MMNode::ExecutionMode::EXECUTABLE;
  } else if(exec_mode_str == "go_script") {
    config.execution_mode = framework::MMNode::ExecutionMode::GO_SCRIPT;
  } else if(exec_mode_str == "python_script") {
    config.execution_mode = framework::MMNode::ExecutionMode::PYTHON_SCRIPT;
  } else if(exec_mode_str == "java_jar") {
    config.execution_mode = framework::MMNode::ExecutionMode::JAVA_JAR;
  } else if(exec_mode_str == "so_library") {
    config.execution_mode = framework::MMNode::ExecutionMode::SO_LIBRARY;
  } else {
    config.execution_mode = framework::MMNode::ExecutionMode::CPP_FUNCTION;
  }

  config.command          = JsonHelper::get_string(config_source, "command", "");
  config.working_dir      = JsonHelper::get_string(config_source, "working_dir", "");
  config.retry_delay_ms   = static_cast<int>(JsonHelper::get_int(config_source, "retry_delay_ms", 1000));
  config.enable_hot_reload = JsonHelper::get_bool(config_source, "enable_hot_reload", true);
  config.function_name    = JsonHelper::get_string(config_source, "function_name", "");
  config.main_class       = JsonHelper::get_string(config_source, "main_class", "");
  config.workflow_ref     = JsonHelper::get_string(config_source, "workflow_ref", "");
  config.node_context_workflow = JsonHelper::get_string(config_source, "node_context_workflow", "");
  config.json_output      = JsonHelper::get_bool(config_source, "json_output", false);
  if(config.type == "workflow" && config.workflow_ref.empty()) {
    result.errors.emplace_back("workflow node requires 'workflow_ref' field", path);
    return false;
  }

  // Parse optional per-node runtime configuration.
  // Example JSON: "node_runtime": {"message_threads": 1, "event_threads": 1, "executor_threads": 2}
  auto node_runtime_obj = JsonHelper::get_object(config_source, "node_runtime");
  if(node_runtime_obj) {
    config.auto_start_runtime = true;
    config.node_runtime_config.message_threads =
        static_cast<int>(JsonHelper::get_int(node_runtime_obj, "message_threads", 1));
    config.node_runtime_config.event_threads =
        static_cast<int>(JsonHelper::get_int(node_runtime_obj, "event_threads", 1));
    config.node_runtime_config.executor_threads =
        static_cast<int>(JsonHelper::get_int(node_runtime_obj, "executor_threads", 2));
    config.node_runtime_config.max_queue_size =
        static_cast<size_t>(JsonHelper::get_int(node_runtime_obj, "max_queue_size", 1024));
  }

  // Parse script/command args array
  auto args_array = JsonHelper::get_array(config_source, "args");
  if(args_array) {
    size_t num_args = args_array->array_size();
    for(size_t i = 0; i < num_args; ++i) {
      auto arg = args_array->array_at(i);
      if(arg && arg->is_string()) {
        config.args.push_back(arg->as_string());
      }
    }
  }

  return true;
}

bool WorkflowParser::parse_node_inputs(std::shared_ptr<const IJsonValue> node_obj,
                                       framework::MMNode::Config& config, ParseResult& result,
                                       const std::string& path) {
  auto inputs_array = JsonHelper::get_array(node_obj, "inputs");
  if(inputs_array) {
    size_t num_inputs = inputs_array->array_size();
    for(size_t i = 0; i < num_inputs; ++i) {
      auto input = inputs_array->array_at(i);
      if(input && input->is_string()) {
        config.input_params.push_back(input->as_string());
        config.input_definitions.push_back(
            framework::MMNode::FieldDefinition{input->as_string(),
                                               framework::MMNode::FieldType::STRING,
                                               framework::MMNode::TransferMode::VALUE,
                                               false});
      } else if(input && input->is_object()) {
        framework::MMNode::FieldDefinition field;
        field.name = JsonHelper::get_string(input, "name", "");
        field.type = parse_node_field_type(JsonHelper::get_string(input, "type", "string"));
        field.transfer_mode =
            parse_node_transfer_mode(JsonHelper::get_string(input, "transfer_mode", "value"));
        field.required = JsonHelper::get_bool(input, "required", false);
        if(field.name.empty()) {
          result.errors.emplace_back("node input definition requires non-empty name",
                                     path + ".inputs[" + std::to_string(i) + "]");
          return false;
        }
        config.input_params.push_back(field.name);
        config.input_definitions.push_back(field);
      }
    }
    return true;
  }

  auto inputs_obj = JsonHelper::get_object(node_obj, "inputs");
  if(inputs_obj) {
    auto keys = inputs_obj->object_keys();
    for(const auto& key : keys) {
      config.input_params.push_back(key);
      auto input = inputs_obj->get_member(key);
      framework::MMNode::FieldDefinition field;
      field.name = key;
      if(input && input->is_string()) {
        field.type = parse_node_field_type(input->as_string());
      } else if(input && input->is_object()) {
        field.type = parse_node_field_type(JsonHelper::get_string(input, "type", "string"));
        field.transfer_mode =
            parse_node_transfer_mode(JsonHelper::get_string(input, "transfer_mode", "value"));
        field.required = JsonHelper::get_bool(input, "required", false);
      }
      config.input_definitions.push_back(field);
    }
    return true;
  }

  return true;
}

bool WorkflowParser::parse_node_outputs(std::shared_ptr<const IJsonValue> node_obj,
                                        framework::MMNode::Config& config, ParseResult& result,
                                        const std::string& path) {
  auto outputs_array = JsonHelper::get_array(node_obj, "outputs");
  if(outputs_array) {
    size_t num_outputs = outputs_array->array_size();
    for(size_t i = 0; i < num_outputs; ++i) {
      auto output = outputs_array->array_at(i);
      if(output && output->is_string()) {
        config.output_params.push_back(output->as_string());
        config.output_definitions.push_back(
            framework::MMNode::FieldDefinition{output->as_string(),
                                               framework::MMNode::FieldType::STRING,
                                               framework::MMNode::TransferMode::VALUE,
                                               false});
      } else if(output && output->is_object()) {
        framework::MMNode::FieldDefinition field;
        field.name = JsonHelper::get_string(output, "name", "");
        field.type = parse_node_field_type(JsonHelper::get_string(output, "type", "string"));
        field.transfer_mode =
            parse_node_transfer_mode(JsonHelper::get_string(output, "transfer_mode", "value"));
        field.required = JsonHelper::get_bool(output, "required", false);
        if(field.name.empty()) {
          result.errors.emplace_back("node output definition requires non-empty name",
                                     path + ".outputs[" + std::to_string(i) + "]");
          return false;
        }
        config.output_params.push_back(field.name);
        config.output_definitions.push_back(field);
      }
    }
    return true;
  }

  auto outputs_obj = JsonHelper::get_object(node_obj, "outputs");
  if(outputs_obj) {
    auto keys = outputs_obj->object_keys();
    for(const auto& key : keys) {
      config.output_params.push_back(key);
      auto output = outputs_obj->get_member(key);
      framework::MMNode::FieldDefinition field;
      field.name = key;
      if(output && output->is_string()) {
        field.type = parse_node_field_type(output->as_string());
      } else if(output && output->is_object()) {
        field.type = parse_node_field_type(JsonHelper::get_string(output, "type", "string"));
        field.transfer_mode =
            parse_node_transfer_mode(JsonHelper::get_string(output, "transfer_mode", "value"));
        field.required = JsonHelper::get_bool(output, "required", false);
      }
      config.output_definitions.push_back(field);
    }
    return true;
  }

  return true;
}

bool WorkflowParser::parse_edge(std::shared_ptr<const IJsonValue> edge_obj, ParseResult& result,
                                const std::string& path) {
  framework::MMEdge::Config config;

  if(!parse_edge_config(edge_obj, config, result, path)) {
    return false;
  }

  // Create edge and add to workflow
  auto edge = std::make_shared<framework::MMEdge>(config);
  current_workflow_->add_edge(edge);

  return true;
}

bool WorkflowParser::parse_edge_config(std::shared_ptr<const IJsonValue> edge_obj,
                                       framework::MMEdge::Config& config, ParseResult& result,
                                       const std::string& path) {
  config.id = JsonHelper::get_string(edge_obj, "id", "");
  if(config.id.empty()) {
    result.errors.emplace_back("MMEdge missing required 'id' field", path);
    return false;
  }

  config.source_node = JsonHelper::get_string(edge_obj, "source", "");
  if(config.source_node.empty()) {
    result.errors.emplace_back("MMEdge missing required 'source' field", path);
    return false;
  }

  config.target_node = JsonHelper::get_string(edge_obj, "target", "");
  if(config.target_node.empty()) {
    result.errors.emplace_back("MMEdge missing required 'target' field", path);
    return false;
  }

  // Parse data transfer mode
  std::string mode_str = JsonHelper::get_string(edge_obj, "transfer_mode", "value");
  config.transfer_mode =
      (mode_str == "ref" || mode_str == "reference")
          ? framework::MMEdge::DataTransferMode::REFERENCE
          : framework::MMEdge::DataTransferMode::VALUE;

  // Parse source outputs
  auto source_outputs = JsonHelper::get_array(edge_obj, "source_outputs");
  if(source_outputs) {
    size_t num = source_outputs->array_size();
    for(size_t i = 0; i < num; ++i) {
      auto output = source_outputs->array_at(i);
      if(output && output->is_string()) {
        config.source_outputs.push_back(output->as_string());
      }
    }
  }

  // Parse target inputs
  auto target_inputs = JsonHelper::get_array(edge_obj, "target_inputs");
  if(target_inputs) {
    size_t num = target_inputs->array_size();
    for(size_t i = 0; i < num; ++i) {
      auto input = target_inputs->array_at(i);
      if(input && input->is_string()) {
        config.target_inputs.push_back(input->as_string());
      }
    }
  }

  // Parse data formats
  auto formats = JsonHelper::get_array(edge_obj, "data_formats");
  if(formats) {
    size_t num = formats->array_size();
    for(size_t i = 0; i < num; ++i) {
      auto format = formats->array_at(i);
      if(format && format->is_string()) {
        config.data_formats.push_back(format->as_string());
      }
    }
  }

  // Parse supported events
  auto events = JsonHelper::get_array(edge_obj, "supported_events");
  if(events) {
    size_t num = events->array_size();
    for(size_t i = 0; i < num; ++i) {
      auto event = events->array_at(i);
      if(event && event->is_string()) {
        config.supported_events.push_back(event->as_string());
      }
    }
  }

  // Check for 'condition' object (nested) or 'condition_expression' string
  auto condition_obj = JsonHelper::get_object(edge_obj, "condition");
  if(condition_obj) {
    config.condition_expression = JsonHelper::get_string(condition_obj, "expression", "");
  } else {
    config.condition_expression = JsonHelper::get_string(edge_obj, "condition_expression", "");
  }

  // Parse unified edge control config
  auto control_obj = JsonHelper::get_object(edge_obj, "control");
  if(control_obj) {
    const std::string control_type = JsonHelper::get_string(control_obj, "type", "");
    if(control_type == "loop" || control_type == "fork" || control_type == "join"
       || control_type == "async" || control_type == "goto") {
      result.errors.emplace_back(
          "edge control types loop/fork/join/async/goto are no longer supported; use if or switch-case",
          path + ".control.type");
      return false;
    }
    config.control.type       = parse_edge_control_type(control_type);
    config.control.expression = JsonHelper::get_string(control_obj, "expression", "");
    config.control.case_value = JsonHelper::get_string(control_obj, "case_value", "");
    config.control.is_default = JsonHelper::get_bool(control_obj, "is_default", false);
  }
  if(config.control.type == framework::MMEdge::ControlType::IF) {
    if(config.control.expression.empty()) {
      config.control.expression = config.condition_expression;
    }
    config.condition_expression = config.control.expression;
  } else if(config.control.type == framework::MMEdge::ControlType::SWITCH_CASE) {
    if(config.control.expression.empty()) {
      result.errors.emplace_back("switch-case control requires expression", path + ".control.expression");
      return false;
    }
    if(config.control.case_value.empty() && !config.control.is_default) {
      result.errors.emplace_back("switch-case control requires case_value or is_default=true",
                                 path + ".control.case_value");
      return false;
    }
  } else if(config.control.type == framework::MMEdge::ControlType::NONE &&
            !config.condition_expression.empty()) {
    // Backward compatibility: old condition_expression maps to IF edge.
    config.control.type       = framework::MMEdge::ControlType::IF;
    config.control.expression = config.condition_expression;
  }
  
  config.priority             = static_cast<int>(JsonHelper::get_int(edge_obj, "priority", 0));

  return true;
}

bool WorkflowParser::parse_agent_config(std::shared_ptr<const IJsonValue> obj,
                                        framework::MMAgent::Config& config, ParseResult& result,
                                        const std::string& path) {
  // MMAgent metadata parsing is not currently supported in MMAgent::Config
  // This can be added in future if needed

  return true;
}

bool WorkflowParser::parse_agent_workflows(std::shared_ptr<const IJsonValue> workflows_array,
                                           ParseResult& result, const std::string& path) {
  size_t num_workflows = workflows_array->array_size();

  for(size_t i = 0; i < num_workflows; ++i) {
    auto workflow_obj = workflows_array->array_at(i);
    if(!workflow_obj) {
      result.errors.emplace_back("MMWorkflow reference at index " + std::to_string(i) + " is null",
                                 path + "[" + std::to_string(i) + "]");
      return false;
    }

    if(!parse_workflow_ref(workflow_obj, result, path + "[" + std::to_string(i) + "]")) {
      return false;
    }
  }

  return true;
}

bool WorkflowParser::parse_workflow_ref(std::shared_ptr<const IJsonValue> ref_obj,
                                        ParseResult& result, const std::string& path) {
  // Check if it's a workflow reference (string) or inline workflow definition (object)
  if(ref_obj->is_string()) {
    // MMWorkflow file reference - load from file
    std::string file_path    = ref_obj->as_string();
    auto        parse_result = parse_workflow_file(file_path);
    if(!parse_result.is_valid()) {
      result.errors.push_back(parse_result.errors[0]);
      return false;
    }
    current_agent_->add_workflow(parse_result.workflow);
  } else if(ref_obj->is_object()) {
    // Inline workflow definition
    // Save current workflow state
    auto saved_workflow = current_workflow_;
    
    // Parse inline workflow
    if(!parse_workflow_root(ref_obj, result)) {
        current_workflow_ = saved_workflow;
        return false;
    }
    
    // Add parsed workflow to agent
    if(current_workflow_) {
        current_agent_->add_workflow(current_workflow_);
    }
    
    // Restore workflow state
    current_workflow_ = saved_workflow;
    return true;
  } else {
    result.errors.emplace_back("MMWorkflow reference must be string or object", path);
    return false;
  }

  return true;
}

bool WorkflowParser::validate_workflow_structure(std::shared_ptr<framework::MMWorkflow> workflow,
                                                 ParseResult&                         result) {
  // Validate workflow has nodes
  if(workflow->get_nodes().empty()) {
    // result.warnings.emplace_back("MMWorkflow has no nodes");
    // Empty workflow is allowed (e.g. reference only)
  }

  // Validate workflow graph. Controlled cycles are no longer supported.
  if(!workflow->validate()) {
    result.errors.emplace_back("MMWorkflow graph validation failed - workflow must be acyclic");
    return false;
  }

  return true;
}

bool WorkflowParser::validate_node_ids_unique(ParseResult& result) {
  std::unordered_set<NodeId> node_ids;
  bool                       unique = true;

  for(const auto& node : current_workflow_->get_nodes()) {
    if(node_ids.find(node->get_id()) != node_ids.end()) {
      unique = false;
      result.errors.emplace_back("Duplicate node ID found: " + node->get_id());
      break;
    }
    node_ids.insert(node->get_id());
  }

  return unique;
}

bool WorkflowParser::validate_edge_references(std::shared_ptr<framework::MMWorkflow> workflow,
                                              ParseResult&                         result) {
  const auto&                nodes = workflow->get_nodes();
  std::unordered_set<NodeId> node_ids;
  for(const auto& node : nodes) {
    node_ids.insert(node->get_id());
  }

  bool valid = true;
  for(const auto& edge : workflow->get_edges()) {
    if(node_ids.find(edge->get_source()) == node_ids.end()) {
      result.errors.emplace_back("MMEdge '" + edge->get_id()
                                 + "' references non-existent source node '" + edge->get_source()
                                 + "'");
      valid = false;
    }
    if(node_ids.find(edge->get_target()) == node_ids.end()) {
      result.errors.emplace_back("MMEdge '" + edge->get_id()
                                 + "' references non-existent target node '" + edge->get_target()
                                 + "'");
      valid = false;
    }
  }

  return valid;
}

// ============================================================================
// WorkflowParserFactory Implementation
// ============================================================================

std::unique_ptr<WorkflowParser> WorkflowParserFactory::create() {
  return std::make_unique<WorkflowParser>();
}

std::string WorkflowParserFactory::get_supported_dsl_version() {
  return "1.0.0";
}

}  // namespace dsl
}  // namespace agentflow
