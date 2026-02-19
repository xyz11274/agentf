// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Storage manager for unified management of Agent and Workflow storage

#pragma once

#include <memory>

#include "runtime/storage/agent_storage.h"
#include "runtime/storage/storage.h"
#include "workflow/workflow_storage.h"

namespace agentflow {
namespace storage {

/**
 * Gray release configuration
 */
struct GrayReleaseConfig {
  std::string agent_id;
  std::string target_version;  // Version to release to gray
  double      percentage;      // Percentage of traffic (0.0 - 1.0)
  int64_t     start_time;      // Unix timestamp
  int64_t     end_time;        // Unix timestamp
  std::string description;
};

/**
 * Storage manager for unified Agent and Workflow storage management
 */
class StorageManager {
public:
  explicit StorageManager(const StorageConfig& config);
  ~StorageManager();

  /**
   * Initialize storage manager
   */
  StorageResult initialize();

  /**
   * Cleanup and close storage
   */
  void cleanup();

  /**
   * Get Agent storage interface
   */
  IAgentStorage* agent_storage() { return agent_storage_.get(); }

  /**
   * Get Workflow storage interface
   */
  IWorkflowStorage* workflow_storage() { return workflow_storage_.get(); }

  /**
   * Get storage configuration
   */
  const StorageConfig& config() const { return config_; }

  /**
   * Gray release management
   */

  /**
   * Start gray release for an Agent
   */
  StorageResult start_gray_release(const GrayReleaseConfig& config);

  /**
   * Update gray release percentage
   */
  StorageResult update_gray_release(const std::string& agent_id, double percentage);

  /**
   * Get current gray release status
   */
  struct GrayReleaseStatus {
    std::string active_version;
    std::string gray_version;
    double      gray_percentage;
    int64_t     start_time;
    int64_t     end_time;
    std::string description;
  };

  StorageResult get_gray_release_status(const std::string& agent_id, GrayReleaseStatus& status);

  /**
   * Complete gray release and make gray version active
   */
  StorageResult complete_gray_release(const std::string& agent_id);

  /**
   * Cancel gray release and rollback to active version
   */
  StorageResult cancel_gray_release(const std::string& agent_id);

  /**
   * Get storage statistics
   */
  struct OverallStats {
    IAgentStorage::StorageStats    agent_stats;
    IWorkflowStorage::StorageStats workflow_stats;
  };

  StorageResult get_overall_stats(OverallStats& stats);

  /**
   * Export data for backup or CI/CD
   */
  StorageResult export_data(const std::string& output_path);

  /**
   * Import data from backup or CI/CD
   */
  StorageResult import_data(const std::string& input_path);

private:
  StorageConfig                     config_;
  std::unique_ptr<IAgentStorage>    agent_storage_;
  std::unique_ptr<IWorkflowStorage> workflow_storage_;

  // Gray release state (could be persisted)
  std::map<std::string, GrayReleaseConfig> gray_releases_;
};

/**
 * Create storage manager instance
 */
std::unique_ptr<StorageManager> create_storage_manager(const StorageConfig& config);

}  // namespace storage
}  // namespace agentflow
