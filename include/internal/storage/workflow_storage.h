// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Workflow storage interface for persisting Workflow definitions

#pragma once

#include <memory>
#include <vector>

#include "workflow/workflow.h"
#include "runtime/storage/storage.h"

namespace agentflow {
namespace storage {

/**
 * Workflow metadata for storage
 */
struct WorkflowMetadata {
  std::string                        id;
  std::string                        name;
  std::string                        version;
  std::string                        description;
  std::string                        dsl_file;
  int64_t                            created_at;
  int64_t                            updated_at;
  std::string                        author;
  std::map<std::string, std::string> tags;
};

/**
 * Workflow storage interface
 * Provides methods for persisting and loading Workflow definitions
 */
class IWorkflowStorage {
public:
  virtual ~IWorkflowStorage() = default;

  /**
   * Save Workflow definition to storage
   */
  virtual StorageResult save_workflow(const framework::Workflow::Config&                   config,
                                      const std::vector<std::shared_ptr<framework::Node>>& nodes,
                                      const std::vector<std::shared_ptr<framework::Edge>>& edges,
                                      const VersionInfo& version_info) = 0;

  /**
   * Load Workflow definition from storage
   */
  virtual StorageResult load_workflow(const std::string&                    workflow_id,
                                      framework::Workflow::Config&          config,
                                      std::vector<framework::Node::Config>& node_configs,
                                      std::vector<framework::Edge::Config>& edge_configs) = 0;

  /**
   * Load Workflow by specific version
   */
  virtual StorageResult load_workflow_version(
      const std::string& workflow_id, const std::string& version,
      framework::Workflow::Config& config, std::vector<framework::Node::Config>& node_configs,
      std::vector<framework::Edge::Config>& edge_configs) = 0;

  /**
   * Delete Workflow from storage
   */
  virtual StorageResult delete_workflow(const std::string& workflow_id) = 0;

  /**
   * List all Workflow IDs
   */
  virtual StorageResult list_workflows(std::vector<std::string>& workflow_ids) = 0;

  /**
   * Get Workflow metadata
   */
  virtual StorageResult get_workflow_metadata(const std::string& workflow_id,
                                              WorkflowMetadata&  metadata) = 0;

  /**
   * List all versions for a Workflow
   */
  virtual StorageResult list_workflow_versions(const std::string&        workflow_id,
                                               std::vector<VersionInfo>& versions) = 0;

  /**
   * Set active version for a Workflow (for gray release)
   */
  virtual StorageResult set_active_workflow_version(const std::string& workflow_id,
                                                    const std::string& version) = 0;

  /**
   * Get active version for a Workflow
   */
  virtual StorageResult get_active_workflow_version(const std::string& workflow_id,
                                                    std::string&       version) = 0;

  /**
   * Rollback Workflow to specific version
   */
  virtual StorageResult rollback_workflow(const std::string& workflow_id,
                                          const std::string& version) = 0;

  /**
   * Check if Workflow exists
   */
  virtual bool workflow_exists(const std::string& workflow_id) = 0;

  /**
   * Get storage statistics
   */
  struct StorageStats {
    size_t total_workflows;
    size_t total_versions;
    size_t storage_size_bytes;
  };

  virtual StorageResult get_storage_stats(StorageStats& stats) = 0;
};

/**
 * Create Workflow storage instance with file-based backend
 */
std::unique_ptr<IWorkflowStorage> create_workflow_storage(const StorageConfig& config);

}  // namespace storage
}  // namespace agentflow
