#ifndef AGENTFLOW_INTERNAL_AGENT_COORDINATOR_H
#define AGENTFLOW_INTERNAL_AGENT_COORDINATOR_H

#include <memory>
#include <map>
#include <string>
#include <vector>

#include "agent/msgque.h"

namespace agentflow {
namespace framework {

class AgentTask;
class AgentSession;
class Workflow;

class AgentCoordinator {
public:
  struct Config {
    std::string coordinator_id;
    std::string default_task_type = "task0";
    std::map<std::string, std::string> task_workflows;
  };

  explicit AgentCoordinator(const Config& config);
  ~AgentCoordinator() = default;

  std::vector<std::shared_ptr<AgentTask>> process_session_messages(
      AgentSession& session, const std::vector<std::shared_ptr<Workflow>>& workflows);

  const std::string& get_coordinator_id() const { return config_.coordinator_id; }

private:
  std::shared_ptr<AgentTask> build_task_for_message(
      const std::string& session_id, const AgentMsgQue::Message& message,
      const std::vector<std::shared_ptr<Workflow>>& workflows, int64_t suffix_counter) const;
  std::string resolve_task_workflow(const std::string& task_type,
                                    const std::vector<std::shared_ptr<Workflow>>& workflows) const;

  Config config_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_COORDINATOR_H
