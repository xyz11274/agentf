// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DSL Adapter - Convert XY IR format to AgentFlow DSL format

#ifndef AGENTFLOW_DSL_DSL_ADAPTER_H
#define AGENTFLOW_DSL_DSL_ADAPTER_H

#include <memory>
#include <string>
#include <vector>

#include "workflow/dsl/json_adapter.h"
#include "workflow/dsl/workflow_parser.h"

namespace agentflow {
namespace dsl {

/**
 * @brief XY IR format version information
 */
struct XYIRVersion {
  int major = 0;
  int minor = 6;  // Current XY IR version is 0.6
  int patch = 0;

  std::string to_string() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  }

  static XYIRVersion from_string(const std::string& version_str);
};

/**
 * @brief Adapter conversion error
 */
struct AdapterError {
  int         line   = -1;
  int         column = -1;
  std::string message;
  std::string path;

  AdapterError() = default;
  AdapterError(const std::string& msg, const std::string& p = "")
      : message(msg), path(p) {}

  std::string to_string() const {
    std::string result = message;
    if(!path.empty()) {
      result += " (at: " + path + ")";
    }
    return result;
  }
};

/**
 * @brief Adapter conversion result
 */
struct AdapterResult {
  // Converted objects
  std::shared_ptr<framework::MMWorkflow> workflow;
  std::shared_ptr<framework::MMAgent>    agent;

  // AF DSL JSON string output (optional)
  std::string workflow_json;
  std::string agent_json;

  // Errors and warnings
  std::vector<AdapterError> errors;
  std::vector<std::string>  warnings;

  bool is_valid() const {
    return errors.empty() && (workflow != nullptr || agent != nullptr ||
                              !workflow_json.empty() || !agent_json.empty());
  }

  std::string get_error_summary() const;
};

/**
 * @brief DSLAdapter converts XY IR format to AgentFlow DSL format
 *
 * XY IR format uses "components" and "connections" concepts.
 * AF DSL format uses "nodes" and "edges" concepts.
 * This adapter handles the conversion between these two formats.
 */
class DSLAdapter {
public:
  DSLAdapter()  = default;
  ~DSLAdapter() = default;

  // =========================================================================
  // XY IR to AF DSL Conversion - Object Output
  // =========================================================================

  /**
   * @brief Convert XY IR workflow to AF Workflow object
   * @param xyir_content XY IR JSON string
   * @return AdapterResult with workflow object
   */
  AdapterResult convert_workflow(const std::string& xyir_content);

  /**
   * @brief Convert XY IR agent to AF Agent object
   * @param xyir_content XY IR JSON string
   * @return AdapterResult with agent object
   */
  AdapterResult convert_agent(const std::string& xyir_content);

  /**
   * @brief Convert XY IR workflow from file to AF Workflow object
   * @param file_path Path to XY IR JSON file
   * @return AdapterResult with workflow object
   */
  AdapterResult convert_workflow_file(const std::string& file_path);

  /**
   * @brief Convert XY IR agent from file to AF Agent object
   * @param file_path Path to XY IR JSON file
   * @return AdapterResult with agent object
   */
  AdapterResult convert_agent_file(const std::string& file_path);

  // =========================================================================
  // XY IR to AF DSL Conversion - JSON Output
  // =========================================================================

  /**
   * @brief Convert XY IR workflow JSON to AF DSL workflow JSON string
   * @param xyir_content XY IR JSON string
   * @return AdapterResult with workflow_json field populated
   */
  AdapterResult convert_workflow_to_json(const std::string& xyir_content);

  /**
   * @brief Convert XY IR agent JSON to AF DSL agent JSON string
   * @param xyir_content XY IR JSON string
   * @return AdapterResult with agent_json field populated
   */
  AdapterResult convert_agent_to_json(const std::string& xyir_content);

  // =========================================================================
  // Utility Methods
  // =========================================================================

  /**
   * @brief Detect input format (XY IR or AF DSL)
   * @param json_content JSON content to analyze
   * @return "xyir", "afdsl", or "unknown"
   */
  static std::string detect_format(const std::string& json_content);

  /**
   * @brief Validate XY IR content
   * @param xyir_content XY IR JSON string
   * @return true if valid, errors in result
   */
  bool validate_xyir(const std::string& xyir_content, AdapterResult& result);

private:
  // Internal parsing state
  AdapterResult                          current_result_;
  std::shared_ptr<framework::MMWorkflow> current_workflow_;
  std::shared_ptr<framework::MMAgent>    current_agent_;
  std::string                            current_file_;
  std::map<std::string, std::string>    component_id_to_name_;

  // XY IR parsing methods
  bool parse_xyir_workflow_root(std::shared_ptr<const IJsonValue> root,
                                 AdapterResult&                      result);
  bool parse_xyir_agent_root(std::shared_ptr<const IJsonValue> root,
                              AdapterResult&                     result);

  // Component/Connection parsing (XY IR specific)
  bool parse_xyir_components(std::shared_ptr<const IJsonValue> components_array,
                              AdapterResult&                     result,
                              const std::string&                 path);
  bool parse_xyir_component(std::shared_ptr<const IJsonValue> component_obj,
                             AdapterResult&                     result,
                             const std::string&                 path);
  bool parse_xyir_connections(std::shared_ptr<const IJsonValue> connections_array,
                               AdapterResult&                      result,
                               const std::string&                  path);
  bool parse_xyir_connection(std::shared_ptr<const IJsonValue> connection_obj,
                              AdapterResult&                     result,
                              const std::string&                 path);

  // Value parsing methods (XY IR supports both Map and Array formats)
  bool parse_xyir_value_array(std::shared_ptr<const IJsonValue> values_array,
                               std::vector<std::string>&          param_names,
                               std::vector<framework::MMNode::FieldDefinition>& field_defs,
                               AdapterResult&                     result,
                               const std::string&                 path);
  bool parse_xyir_value_map(std::shared_ptr<const IJsonValue> values_map,
                             std::vector<std::string>&          param_names,
                             std::vector<framework::MMNode::FieldDefinition>& field_defs,
                             AdapterResult&                     result,
                             const std::string&                 path);

  // Workflow metadata parsing (XY IR specific)
  bool parse_xyir_workflow_metadata(std::shared_ptr<const IJsonValue> obj,
                                     framework::MMWorkflow::Config&     config,
                                     AdapterResult&                     result,
                                     const std::string&                 path);

  // Component Type parsing (for classInfo, inputs, outputs)
  bool parse_xyir_component_class_info(std::shared_ptr<const IJsonValue> class_info_obj,
                                        framework::MMNode::Config&          config,
                                        AdapterResult&                      result,
                                        const std::string&                  path);

  // Agent parsing (XY IR specific)
  bool parse_xyir_agent_config(std::shared_ptr<const IJsonValue> obj,
                                framework::MMAgent::Config&        config,
                                AdapterResult&                     result,
                                const std::string&                 path);

  // JSON generation methods
  std::string generate_workflow_json(std::shared_ptr<framework::MMWorkflow> workflow);
  std::string generate_agent_json(std::shared_ptr<framework::MMAgent> agent);

  // Validation methods
  bool validate_xyir_workflow_structure(std::shared_ptr<framework::MMWorkflow> workflow,
                                         AdapterResult&                         result);
  bool validate_xyir_agent_structure(std::shared_ptr<framework::MMAgent> agent,
                                      AdapterResult&                       result);
};

/**
 * @brief Factory for creating DSL adapters
 */
class DSLAdapterFactory {
public:
  static std::unique_ptr<DSLAdapter> create();
  static std::string                 get_supported_xyir_version();
  static std::string                 get_output_afdsl_version();
};

}  // namespace dsl
}  // namespace agentflow

#endif  // AGENTFLOW_DSL_DSL_ADAPTER_H