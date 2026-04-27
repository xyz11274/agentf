// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// AgentRegistry implementation

#include "daemon/agent_registry.h"

#include <filesystem>

#include "agent/agent.h"
#include "runtime/agent_runtime.h"
#include "utils/logger.h"

namespace agentflow {
namespace daemon {

bool AgentRegistry::load_agent(const std::string& package_path,
                                const std::string& name_override,
                                runtime::MMAgentRuntime& runtime) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!std::filesystem::exists(package_path + "/package.json")) {
    Logger::instance().error_f("[AGENT_REGISTRY] No package.json at: %s", package_path.c_str());
    return false;
  }

  std::string original_cwd = std::filesystem::current_path().string();
  std::filesystem::current_path(package_path);

  auto agent = framework::MMAgent::create_from_package(package_path);
  if (!agent) {
    std::filesystem::current_path(original_cwd);
    Logger::instance().error_f("[AGENT_REGISTRY] Failed to create agent from: %s", package_path.c_str());
    return false;
  }

  if (!agent->initialize()) {
    std::filesystem::current_path(original_cwd);
    Logger::instance().error_f("[AGENT_REGISTRY] Failed to initialize agent from: %s", package_path.c_str());
    return false;
  }

  std::filesystem::current_path(original_cwd);

  if (!runtime.register_agent(agent)) {
    Logger::instance().error_f("[AGENT_REGISTRY] Failed to register agent: %s", agent->get_id().c_str());
    return false;
  }

  AgentEntry entry;
  entry.agent_id = agent->get_id();
  entry.agent = agent;
  entry.package_path = package_path;
  entry.name = agent->get_name();
  agents_[entry.agent_id] = std::move(entry);

  Logger::instance().info_f("[AGENT_REGISTRY] Agent loaded: %s (%s)", agent->get_id().c_str(),
                            agent->get_name().c_str());
  return true;
}

bool AgentRegistry::unload_agent(const std::string& agent_id, runtime::MMAgentRuntime& runtime) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = agents_.find(agent_id);
  if (it == agents_.end()) {
    Logger::instance().error_f("[AGENT_REGISTRY] Agent not found: %s", agent_id.c_str());
    return false;
  }

  it->second.agent->stop();
  runtime.unregister_agent(agent_id);
  agents_.erase(it);

  Logger::instance().info_f("[AGENT_REGISTRY] Agent unloaded: %s", agent_id.c_str());
  return true;
}

std::shared_ptr<framework::MMAgent> AgentRegistry::find(const std::string& agent_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agents_.find(agent_id);
  if (it != agents_.end()) return it->second.agent;
  return nullptr;
}

std::vector<AgentEntry> AgentRegistry::list_all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AgentEntry> result;
  for (const auto& [_, entry] : agents_) {
    result.push_back(entry);
  }
  return result;
}

}  // namespace daemon
}  // namespace agentflow
