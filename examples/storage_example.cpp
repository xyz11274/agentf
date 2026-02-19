// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Example demonstrating the Storage functionality

#include <iostream>
#include <string>

#include "utils/logger.h"
#include "agent/agent.h"
#include "workflow/workflow.h"
#include "runtime/storage/agent_storage.h"
#include "runtime/storage/storage.h"
#include "runtime/storage/storage_manager.h"
#include "workflow/workflow_storage.h"

using namespace agentflow;

int main() {
  // Initialize logger
  Logger::instance().info("Starting Storage Example...");

  Logger::instance().info("Starting Storage Example...");

  // Configure storage
  storage::StorageConfig config;
  config.base_path         = "./storage_data";
  config.enable_versioning = true;
  config.max_versions      = 10;

  // Create storage manager
  auto storage_manager = storage::create_storage_manager(config);
  if(!storage_manager) {
    Logger::instance().error("Failed to create storage manager");
    return 1;
  }

  Logger::instance().info("Storage manager created successfully");

  // Create an Agent configuration
  framework::Agent::Config agent_config;
  agent_config.id                       = "agent_001";
  agent_config.name                     = "Example Agent";
  agent_config.version                  = "1.0";
  agent_config.description              = "An example agent for testing";
  agent_config.policy                   = framework::Agent::LifecyclePolicy::FACTORY;
  agent_config.max_workflows            = 5;
  agent_config.max_concurrent_workflows = 3;

  // Create version info
  storage::VersionInfo version_info;
  version_info.version     = "1.0.0";
  version_info.timestamp   = time(NULL) * 1000;
  version_info.description = "Initial version";
  version_info.author      = "AgentFlow System";
  version_info.commit_id   = "abc123";

  // Save the agent
  std::vector<std::shared_ptr<framework::Workflow>> workflows;
  auto result = storage_manager->agent_storage()->save_agent(agent_config, workflows, version_info);

  if(result.is_success()) {
    Logger::instance().info("Agent saved successfully: " + agent_config.id);
  } else {
    Logger::instance().error("Failed to save agent: " + result.message);
    return 1;
  }

  // Load the agent
  framework::Agent::Config                 loaded_config;
  std::vector<framework::Workflow::Config> loaded_workflow_configs;

  result = storage_manager->agent_storage()->load_agent("agent_001", loaded_config,
                                                        loaded_workflow_configs);

  if(result.is_success()) {
    Logger::instance().info("Agent loaded successfully: " + loaded_config.name);
    Logger::instance().info("  Version: " + loaded_config.version);
    Logger::instance().info("  Description: " + loaded_config.description);
  } else {
    Logger::instance().error("Failed to load agent: " + result.message);
    return 1;
  }

  // List agents
  std::vector<std::string> agent_ids;
  result = storage_manager->agent_storage()->list_agents(agent_ids);

  if(result.is_success()) {
    Logger::instance().info("Found " + std::to_string(agent_ids.size()) + " agent(s):");
    for(const auto& id : agent_ids) {
      Logger::instance().info("  - " + id);
    }
  }

  // Get storage statistics
  storage::IAgentStorage::StorageStats agent_stats;
  result = storage_manager->agent_storage()->get_storage_stats(agent_stats);

  if(result.is_success()) {
    Logger::instance().info("Storage statistics:");
    Logger::instance().info("  Total agents: " + std::to_string(agent_stats.total_agents));
    Logger::instance().info("  Total versions: " + std::to_string(agent_stats.total_versions));
  }

  // Test versioning
  std::vector<storage::VersionInfo> versions;
  result = storage_manager->agent_storage()->list_agent_versions("agent_001", versions);

  if(result.is_success()) {
    Logger::instance().info("Agent versions:");
    for(const auto& v : versions) {
      Logger::instance().info("  - " + v.version + " (" + v.description + ")");
    }
  }

  // Create a workflow configuration
  framework::Workflow::Config workflow_config;
  workflow_config.id          = "workflow_001";
  workflow_config.name        = "Example Workflow";
  workflow_config.version     = "1.0";
  workflow_config.description = "An example workflow for testing";
  workflow_config.dsl_file    = "workflows/example.json";

  std::vector<std::shared_ptr<framework::Node>> nodes;
  std::vector<std::shared_ptr<framework::Edge>> edges;

  // Save the workflow
  result = storage_manager->workflow_storage()->save_workflow(workflow_config, nodes, edges,
                                                              version_info);

  if(result.is_success()) {
    Logger::instance().info("Workflow saved successfully: " + workflow_config.id);
  } else {
    Logger::instance().error("Failed to save workflow: " + result.message);
    return 1;
  }

  // Load the workflow
  std::vector<framework::Node::Config> node_configs;
  std::vector<framework::Edge::Config> edge_configs;
  framework::Workflow::Config          loaded_workflow_config;

  result = storage_manager->workflow_storage()->load_workflow(
      "workflow_001", loaded_workflow_config, node_configs, edge_configs);

  if(result.is_success()) {
    Logger::instance().info("Workflow loaded successfully: " + loaded_workflow_config.name);
    Logger::instance().info("  Version: " + loaded_workflow_config.version);
  } else {
    Logger::instance().error("Failed to load workflow: " + result.message);
    return 1;
  }

  // List workflows
  std::vector<std::string> workflow_ids;
  result = storage_manager->workflow_storage()->list_workflows(workflow_ids);

  if(result.is_success()) {
    Logger::instance().info("Found " + std::to_string(workflow_ids.size()) + " workflow(s):");
    for(const auto& id : workflow_ids) {
      Logger::instance().info("  - " + id);
    }
  }

  // Cleanup
  storage_manager->cleanup();

  Logger::instance().info("Storage Example completed successfully");

  return 0;
}
