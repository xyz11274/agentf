// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DSL Workflow Parser - Parse JSON DSL to create Workflow objects

#ifndef AGENTFLOW_DSL_WORKFLOW_PARSER_H
#define AGENTFLOW_DSL_WORKFLOW_PARSER_H

#include <memory>
#include <string>
#include <vector>

#include "workflow/dsl/json_adapter.h"
#include "agent/agent.h"
#include "workflow/edge.h"
#include "workflow/node.h"
#include "workflow/workflow.h"

namespace agentflow {
namespace dsl {

/**
 * @brief Parser error information
 */
struct ParseError {
  int         line   = -1;
  int         column = -1;
  std::string message;
  std::string path;  // JSON path to error location

  ParseError() = default;
  ParseError(const std::string& msg, const std::string& p = "") : message(msg), path(p) {}

  std::string to_string() const {
    std::string result = message;
    if(!path.empty()) {
      result += " (at: " + path + ")";
    }
    if(line > 0) {
      result += " line: " + std::to_string(line);
    }
    return result;
  }
};

/**
 * @brief Parse result with workflow and optional errors
 */
struct ParseResult {
  std::shared_ptr<framework::Workflow> workflow;
  std::shared_ptr<framework::Agent>    agent;
  std::vector<ParseError>              errors;
  std::vector<std::string>             warnings;

  bool        is_valid() const { return errors.empty() && workflow != nullptr; }
  std::string get_error_summary() const;
};

/**
 * @brief DSL format version
 */
struct DslVersion {
  int major = 1;
  int minor = 0;
  int patch = 0;

  std::string to_string() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  }

  static DslVersion from_string(const std::string& version_str);
};

/**
 * @brief Workflow DSL Parser
 * Parses JSON DSL format to create Workflow, Node, Edge objects
 */
class WorkflowParser {
public:
  WorkflowParser()  = default;
  ~WorkflowParser() = default;

  /**
   * @brief Parse workflow from JSON string
   * @param json_content JSON string content
   * @return Parse result with workflow object and errors
   */
  ParseResult parse_workflow(const std::string& json_content);

  /**
   * @brief Parse agent from JSON string
   * @param json_content JSON string content
   * @return Parse result with agent object and errors
   */
  ParseResult parse_agent(const std::string& json_content);

  /**
   * @brief Parse workflow from JSON file
   * @param file_path Path to JSON file
   * @return Parse result with workflow object and errors
   */
  ParseResult parse_workflow_file(const std::string& file_path);

  /**
   * @brief Parse agent from JSON file
   * @param file_path Path to JSON file
   * @return Parse result with agent object and errors
   */
  ParseResult parse_agent_file(const std::string& file_path);

  /**
   * @brief Validate parsed workflow
   * @param workflow Workflow to validate
   * @return true if valid, errors populated in result
   */
  bool validate_workflow(std::shared_ptr<framework::Workflow> workflow, ParseResult& result);

  /**
   * @brief Validate parsed agent
   * @param agent Agent to validate
   * @return true if valid, errors populated in result
   */
  bool validate_agent(std::shared_ptr<framework::Agent> agent, ParseResult& result);

private:
  // Core parsing methods
  bool parse_workflow_root(std::shared_ptr<const IJsonValue> root, ParseResult& result);
  bool parse_agent_root(std::shared_ptr<const IJsonValue> root, ParseResult& result);

  // Workflow parsing
  bool parse_workflow_metadata(std::shared_ptr<const IJsonValue> obj,
                               framework::Workflow::Config& config, ParseResult& result,
                               const std::string& path);
  bool parse_nodes(std::shared_ptr<const IJsonValue> nodes_array, ParseResult& result,
                   const std::string& path);
  bool parse_edges(std::shared_ptr<const IJsonValue> edges_array, ParseResult& result,
                   const std::string& path);

  // Node parsing
  bool parse_node(std::shared_ptr<const IJsonValue> node_obj, ParseResult& result,
                  const std::string& path);
  bool parse_node_config(std::shared_ptr<const IJsonValue> node_obj,
                         framework::Node::Config& config, ParseResult& result,
                         const std::string& path);
  bool parse_node_inputs(std::shared_ptr<const IJsonValue> node_obj,
                         framework::Node::Config& config, ParseResult& result,
                         const std::string& path);
  bool parse_node_outputs(std::shared_ptr<const IJsonValue> node_obj,
                          framework::Node::Config& config, ParseResult& result,
                          const std::string& path);

  // Edge parsing
  bool parse_edge(std::shared_ptr<const IJsonValue> edge_obj, ParseResult& result,
                  const std::string& path);
  bool parse_edge_config(std::shared_ptr<const IJsonValue> edge_obj,
                         framework::Edge::Config& config, ParseResult& result,
                         const std::string& path);

  // Agent parsing
  bool parse_agent_config(std::shared_ptr<const IJsonValue> obj, framework::Agent::Config& config,
                          ParseResult& result, const std::string& path);
  bool parse_agent_workflows(std::shared_ptr<const IJsonValue> workflows_array, ParseResult& result,
                             const std::string& path);
  bool parse_workflow_ref(std::shared_ptr<const IJsonValue> ref_obj, ParseResult& result,
                          const std::string& path);

  // Validation methods
  bool validate_workflow_structure(std::shared_ptr<framework::Workflow> workflow,
                                   ParseResult&                         result);
  bool validate_node_ids_unique(const ParseResult& result);
  bool validate_edge_references(std::shared_ptr<framework::Workflow> workflow, ParseResult& result);

  // Member variables
  ParseResult                          current_result_;
  std::shared_ptr<framework::Workflow> current_workflow_;
  std::shared_ptr<framework::Agent>    current_agent_;
  std::map<NodeId, std::string>        node_id_to_name_;
};

/**
 * @brief Factory for creating workflow parsers
 */
class WorkflowParserFactory {
public:
  static std::unique_ptr<WorkflowParser> create();
  static std::string                     get_supported_dsl_version();
};

}  // namespace dsl
}  // namespace agentflow

#endif  // AGENTFLOW_DSL_WORKFLOW_PARSER_H
