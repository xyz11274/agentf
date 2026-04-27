// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// AgentRegistry - Multi-agent management for agentflowd

#ifndef AGENTFLOW_DAEMON_AGENT_REGISTRY_H
#define AGENTFLOW_DAEMON_AGENT_REGISTRY_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentflow {

namespace framework {
class MMAgent;
}

namespace runtime {
class MMAgentRuntime;
}

namespace daemon {

struct AgentEntry {
  std::string agent_id;
  std::shared_ptr<framework::MMAgent> agent;
  std::string package_path;
  std::string name;
};

class AgentRegistry {
public:
  AgentRegistry() = default;

  bool load_agent(const std::string& package_path, const std::string& name_override,
                  runtime::MMAgentRuntime& runtime);
  bool unload_agent(const std::string& agent_id, runtime::MMAgentRuntime& runtime);
  std::shared_ptr<framework::MMAgent> find(const std::string& agent_id);
  std::vector<AgentEntry> list_all() const;

private:
  std::map<std::string, AgentEntry> agents_;
  mutable std::mutex mutex_;
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_AGENT_REGISTRY_H
