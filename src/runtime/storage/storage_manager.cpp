// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Storage manager implementation

#include "runtime/storage/storage_manager.h"

#include <dirent.h>
#include <fstream>
#include <sstream>

#include <sys/types.h>

#include "utils/logger.h"

namespace agentflow {
namespace storage {

// ============================================================================
// StorageManager implementation
// ============================================================================

StorageManager::StorageManager(const StorageConfig& config) : config_(config) {
}

StorageManager::~StorageManager() {
  cleanup();
}

StorageResult StorageManager::initialize() {
  // Create Agent storage
  agent_storage_ = create_agent_storage(config_);
  if(!agent_storage_) {
    return StorageResult{ StorageError::STORAGE_ERROR_IO, "Failed to create agent storage" };
  }

  // Create Workflow storage
  workflow_storage_ = create_workflow_storage(config_);
  if(!workflow_storage_) {
    return StorageResult{ StorageError::STORAGE_ERROR_IO, "Failed to create workflow storage" };
  }

  return StorageResult{ StorageError::STORAGE_OK, "" };
}

void StorageManager::cleanup() {
  gray_releases_.clear();
  agent_storage_.reset();
  workflow_storage_.reset();
}

StorageResult StorageManager::start_gray_release(const GrayReleaseConfig& config) {
  if(!agent_storage_) {
    return StorageResult{ StorageError::STORAGE_ERROR_IO, "Agent storage not initialized" };
  }

  // Check if agent exists
  if(!agent_storage_->agent_exists(config.agent_id)) {
    return StorageResult{ StorageError::STORAGE_ERROR_NOT_FOUND, "Agent not found" };
  }

  // Validate percentage
  if(config.percentage < 0.0 || config.percentage > 1.0) {
    return StorageResult{ StorageError::STORAGE_ERROR_INVALID_DATA, "Invalid percentage" };
  }

  // Store gray release configuration
  gray_releases_[config.agent_id] = config;

  // Set gray version
  return agent_storage_->set_active_agent_version(config.agent_id, config.target_version);
}

StorageResult StorageManager::update_gray_release(const std::string& agent_id, double percentage) {
  if(percentage < 0.0 || percentage > 1.0) {
    return StorageResult{ StorageError::STORAGE_ERROR_INVALID_DATA, "Invalid percentage" };
  }

  auto it = gray_releases_.find(agent_id);
  if(it == gray_releases_.end()) {
    return StorageResult{ StorageError::STORAGE_ERROR_NOT_FOUND, "Gray release not found" };
  }

  it->second.percentage = percentage;

  return StorageResult{ StorageError::STORAGE_OK, "" };
}

StorageResult StorageManager::get_gray_release_status(const std::string& agent_id,
                                                      GrayReleaseStatus& status) {
  auto it = gray_releases_.find(agent_id);
  if(it == gray_releases_.end()) {
    return StorageResult{ StorageError::STORAGE_ERROR_NOT_FOUND, "Gray release not found" };
  }

  const auto& config = it->second;

  // Get current active version
  std::string active_version;
  if(!agent_storage_->get_active_agent_version(agent_id, active_version).is_success()) {
    active_version = "1.0";
  }

  status.active_version  = active_version;
  status.gray_version    = config.target_version;
  status.gray_percentage = config.percentage;
  status.start_time      = config.start_time;
  status.end_time        = config.end_time;
  status.description     = config.description;

  return StorageResult{ StorageError::STORAGE_OK, "" };
}

StorageResult StorageManager::complete_gray_release(const std::string& agent_id) {
  auto it = gray_releases_.find(agent_id);
  if(it == gray_releases_.end()) {
    return StorageResult{ StorageError::STORAGE_ERROR_NOT_FOUND, "Gray release not found" };
  }

  // Make gray version the new active version
  const auto&   config = it->second;
  StorageResult result = agent_storage_->set_active_agent_version(agent_id, config.target_version);

  if(result.is_success()) {
    gray_releases_.erase(it);
  }

  return result;
}

StorageResult StorageManager::cancel_gray_release(const std::string& agent_id) {
  auto it = gray_releases_.find(agent_id);
  if(it == gray_releases_.end()) {
    return StorageResult{ StorageError::STORAGE_ERROR_NOT_FOUND, "Gray release not found" };
  }

  // Rollback to previous version
  (void)it->second;  // Suppress unused variable warning
  StorageResult result = agent_storage_->set_active_agent_version(agent_id, "1.0");

  if(result.is_success()) {
    gray_releases_.erase(it);
  }

  return result;
}

StorageResult StorageManager::get_overall_stats(OverallStats& stats) {
  if(!agent_storage_ || !workflow_storage_) {
    return StorageResult{ StorageError::STORAGE_ERROR_IO, "Storage not initialized" };
  }

  StorageResult result = agent_storage_->get_storage_stats(stats.agent_stats);
  if(!result.is_success()) {
    return result;
  }

  result = workflow_storage_->get_storage_stats(stats.workflow_stats);
  if(!result.is_success()) {
    return result;
  }

  return StorageResult{ StorageError::STORAGE_OK, "" };
}

StorageResult StorageManager::export_data(const std::string& output_path) {
  std::ofstream output_file(output_path);
  if(!output_file.is_open()) {
    return StorageResult{ StorageError::STORAGE_ERROR_IO, "Failed to open output file" };
  }

  // Export agents
  std::vector<std::string> agent_ids;
  StorageResult            result = agent_storage_->list_agents(agent_ids);
  if(!result.is_success()) {
    return result;
  }

  output_file << "[AGENTS]\n";
  for(const auto& agent_id : agent_ids) {
    output_file << "agent:" << agent_id << "\n";
  }

  // Export workflows
  std::vector<std::string> workflow_ids;
  result = workflow_storage_->list_workflows(workflow_ids);
  if(!result.is_success()) {
    return result;
  }

  output_file << "[WORKFLOWS]\n";
  for(const auto& workflow_id : workflow_ids) {
    output_file << "workflow:" << workflow_id << "\n";
  }

  output_file.close();

  return StorageResult{ StorageError::STORAGE_OK, "" };
}

StorageResult StorageManager::import_data(const std::string& input_path) {
  std::ifstream input_file(input_path);
  if(!input_file.is_open()) {
    return StorageResult{ StorageError::STORAGE_ERROR_IO, "Failed to open input file" };
  }

  std::string line;
  std::string section;

  while(std::getline(input_file, line)) {
    if(line.empty())
      continue;

    if(line[0] == '[' && line.back() == ']') {
      section = line.substr(1, line.length() - 2);
    } else if(section == "AGENTS") {
      if(line.find("agent:") == 0) {
        std::string agent_id = line.substr(7);
        // Import agent (placeholder for actual implementation)
      }
    } else if(section == "WORKFLOWS") {
      if(line.find("workflow:") == 0) {
        std::string workflow_id = line.substr(10);
        // Import workflow (placeholder for actual implementation)
      }
    }
  }

  input_file.close();

  return StorageResult{ StorageError::STORAGE_OK, "" };
}

// ============================================================================
// Factory function
// ============================================================================

std::unique_ptr<StorageManager> create_storage_manager(const StorageConfig& config) {
  auto manager = std::unique_ptr<StorageManager>(new StorageManager(config));
  if(!manager->initialize().is_success()) {
    return nullptr;
  }
  return manager;
}

}  // namespace storage
}  // namespace agentflow
