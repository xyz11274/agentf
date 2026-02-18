// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Agent storage interface for persisting Agent definitions

#pragma once

#include <memory>
#include <vector>

#include "agent/agent.h"
#include "runtime/storage/storage.h"

namespace agentflow {
namespace storage {

/**
 * Agent metadata for storage
 */
struct AgentMetadata {
  std::string                        id;
  std::string                        name;
  std::string                        version;
  std::string                        description;
  int64_t                            created_at;
  int64_t                            updated_at;
  std::string                        author;
  std::vector<std::string>           workflow_ids;
  std::map<std::string, std::string> tags;
};

/**
 * Agent storage interface
 * Provides methods for persisting and loading Agent definitions
 */
class IAgentStorage {
public:
  virtual ~IAgentStorage() = default;

  /**
   * Save Agent definition to storage
   */
  virtual StorageResult save_agent(
      const framework::Agent::Config&                          config,
      const std::vector<std::shared_ptr<framework::Workflow>>& workflows,
      const VersionInfo&                                       version_info) = 0;

  /**
   * Load Agent definition from storage
   */
  virtual StorageResult load_agent(const std::string& agent_id, framework::Agent::Config& config,
                                   std::vector<framework::Workflow::Config>& workflow_configs) = 0;

  /**
   * Load Agent by specific version
   */
  virtual StorageResult load_agent_version(
      const std::string& agent_id, const std::string& version, framework::Agent::Config& config,
      std::vector<framework::Workflow::Config>& workflow_configs) = 0;

  /**
   * Delete Agent from storage
   */
  virtual StorageResult delete_agent(const std::string& agent_id) = 0;

  /**
   * List all Agent IDs
   */
  virtual StorageResult list_agents(std::vector<std::string>& agent_ids) = 0;

  /**
   * Get Agent metadata
   */
  virtual StorageResult get_agent_metadata(const std::string& agent_id,
                                           AgentMetadata&     metadata) = 0;

  /**
   * List all versions for an Agent
   */
  virtual StorageResult list_agent_versions(const std::string&        agent_id,
                                            std::vector<VersionInfo>& versions) = 0;

  /**
   * Set active version for an Agent (for gray release)
   */
  virtual StorageResult set_active_agent_version(const std::string& agent_id,
                                                 const std::string& version) = 0;

  /**
   * Get active version for an Agent
   */
  virtual StorageResult get_active_agent_version(const std::string& agent_id,
                                                 std::string&       version) = 0;

  /**
   * Rollback Agent to specific version
   */
  virtual StorageResult rollback_agent(const std::string& agent_id, const std::string& version) = 0;

  /**
   * Check if Agent exists
   */
  virtual bool agent_exists(const std::string& agent_id) = 0;

  /**
   * Get storage statistics
   */
  struct StorageStats {
    size_t total_agents;
    size_t total_versions;
    size_t storage_size_bytes;
  };

  virtual StorageResult get_storage_stats(StorageStats& stats) = 0;
};

/**
 * Create Agent storage instance with file-based backend
 */
std::unique_ptr<IAgentStorage> create_agent_storage(const StorageConfig& config);

}  // namespace storage
}  // namespace agentflow
