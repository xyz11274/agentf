// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example demonstrating the Storage functionality

#include <iostream>
#include <string>
#include <vector>
#include <ctime>

#include "utils/logger.h"
#include "agent/agent.h"
#include "workflow/workflow.h"
#include "utils/file_storage.h"

using namespace agentflow;

int main() {
  // Initialize logger
  Logger::instance().info("Starting Storage Example...");

  // Configure storage
  utils::storage::StorageConfig config;
  config.base_path         = "./storage_data";
  config.enable_versioning = true;
  config.max_versions      = 10;

  Logger::instance().info("Storage configured");

  // Create an MMAgent configuration
  framework::MMAgent::Config agent_config;
  agent_config.id                       = "agent_001";
  agent_config.name                     = "Example MMAgent";
  agent_config.version                  = "1.0";
  agent_config.description              = "An example agent for testing";
  agent_config.policy                   = framework::MMAgent::LifecyclePolicy::FACTORY;
  agent_config.max_workflows            = 5;
  agent_config.max_concurrent_workflows = 3;

  // Create version info
  utils::storage::VersionInfo version_info;
  version_info.version     = "1.0.0";
  version_info.timestamp   = time(NULL) * 1000;
  version_info.description = "Initial version";
  version_info.author      = "AgentFlow System";
  version_info.commit_id   = "abc123";

  // Create MMAgent instance
  auto agent = std::make_shared<framework::MMAgent>(agent_config);

  // Save the agent
  auto result = agent->save(config, version_info);

  if(result.is_success()) {
    Logger::instance().info("MMAgent saved successfully: " + agent_config.id);
  } else {
    Logger::instance().error("Failed to save agent: " + result.message);
    // Don't return 1 here as directory might not exist yet if not created by save
    // In a real app we'd ensure directories exist
  }

  // Load the agent
  std::shared_ptr<framework::MMAgent> loaded_agent;
  result = framework::MMAgent::load("agent_001", config, loaded_agent);

  if(result.is_success() && loaded_agent) {
    Logger::instance().info("MMAgent loaded successfully: " + loaded_agent->get_name());
    Logger::instance().info("  Version: " + loaded_agent->get_config().version);
    Logger::instance().info("  Description: " + loaded_agent->get_config().description);
  } else {
    Logger::instance().error("Failed to load agent: " + result.message);
  }

  // List agents
  std::vector<std::string> agent_ids;
  result = framework::MMAgent::list_agents(config, agent_ids);

  if(result.is_success()) {
    Logger::instance().info("Found " + std::to_string(agent_ids.size()) + " agent(s):");
    for(const auto& id : agent_ids) {
      Logger::instance().info("  - " + id);
    }
  }

  // Get agent metadata
  framework::AgentMetadata metadata;
  result = framework::MMAgent::get_metadata("agent_001", config, metadata);
  if(result.is_success()) {
    Logger::instance().info("MMAgent metadata:");
    Logger::instance().info("  Updated at: " + std::to_string(metadata.updated_at));
  }

  // Create a workflow configuration
  framework::MMWorkflow::Config workflow_config;
  workflow_config.id          = "workflow_001";
  workflow_config.name        = "Example MMWorkflow";
  workflow_config.version     = "1.0";
  workflow_config.description = "An example workflow for testing";
  workflow_config.dsl_file    = "workflows/example.json";

  // Create MMWorkflow instance
  auto workflow = std::make_shared<framework::MMWorkflow>(workflow_config);

  // Save the workflow
  result = workflow->save(config, version_info);

  if(result.is_success()) {
    Logger::instance().info("MMWorkflow saved successfully: " + workflow_config.id);
  } else {
    Logger::instance().error("Failed to save workflow: " + result.message);
  }

  // Load the workflow
  std::shared_ptr<framework::MMWorkflow> loaded_workflow;
  result = framework::MMWorkflow::load("workflow_001", config, loaded_workflow);

  if(result.is_success() && loaded_workflow) {
    Logger::instance().info("MMWorkflow loaded successfully: " + loaded_workflow->get_name());
  } else {
    Logger::instance().error("Failed to load workflow: " + result.message);
  }

  // List workflows
  std::vector<std::string> workflow_ids;
  result = framework::MMWorkflow::list_workflows(config, workflow_ids);

  if(result.is_success()) {
    Logger::instance().info("Found " + std::to_string(workflow_ids.size()) + " workflow(s):");
    for(const auto& id : workflow_ids) {
      Logger::instance().info("  - " + id);
    }
  }

  Logger::instance().info("Storage Example completed successfully");

  return 0;
}
