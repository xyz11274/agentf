#ifndef AGENTFLOW_INTERNAL_AGENT_COORDINATOR_H
#define AGENTFLOW_INTERNAL_AGENT_COORDINATOR_H

#include <memory>
#include <map>
#include <string>
#include <vector>

#include "agent/msgque.h"
#include "agent/common.h"

namespace agentflow {
namespace framework {

class MMAgentTask;
class MMAgentSession;
class MMWorkflow;
class MMAgent;

class SessionTaskCoordinator {
public:
  enum class InputQueueType {
    INTENT_REQUEST = 0,
    OUTPUT_RESULT  = 1,
  };

  struct Config {
    std::string coordinator_id;
    std::string default_task_type = "coordinator";
    InputQueueType input_queue_type = InputQueueType::OUTPUT_RESULT;
    std::string dispatcher_workflow;
    std::string coordinator_workflow;
    std::map<std::string, std::string> task_workflows;
    std::string task_context_workflow;
  };

  SessionTaskCoordinator(const Config& config, const std::shared_ptr<MMAgentTask>& coordinator_task);
  ~SessionTaskCoordinator() = default;

  std::vector<std::shared_ptr<MMAgentTask>> process_session_messages(
      MMAgentSession& session, const std::vector<std::shared_ptr<MMWorkflow>>& workflows);

  // Aggregate outputs from all sessions into a DataMap for use in coordinator workflow inputs.
  // Reads keys "session:<session_id>:<output_key>" from the agent's shared data bus.
  // output_keys: list of per-session output keys to collect (e.g. {"skill_selected", "skill_result"})
  DataMap aggregate_session_results(MMAgent& agent,
                                    const std::vector<std::string>& output_keys) const;

  const std::string& get_coordinator_id() const { return config_.coordinator_id; }
  const std::shared_ptr<MMAgentTask>& get_coordinator_task() const { return coordinator_task_; }

private:
  std::shared_ptr<MMAgentTask> build_task_for_message(
      const std::string& session_id, const MMAgentMsgQue::Message& message,
      const std::vector<std::shared_ptr<MMWorkflow>>& workflows, int64_t suffix_counter) const;
  std::string resolve_task_workflow(const std::string& task_type,
                                    const std::vector<std::shared_ptr<MMWorkflow>>& workflows) const;

  Config config_;
  std::shared_ptr<MMAgentTask> coordinator_task_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_COORDINATOR_H
