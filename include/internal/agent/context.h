#ifndef AGENTFLOW_INTERNAL_AGENT_CONTEXT_H
#define AGENTFLOW_INTERNAL_AGENT_CONTEXT_H

#include <any>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agent/msgque.h"

namespace agentflow {
namespace framework {

class MMAgentTask;

class AgentContext {
 public:
  struct Config {
    std::string context_id;
    std::string agent_id;
    std::string context_workflow;
    std::string contextupdater_workflow;
    std::string contextupdater_task_id;
    std::string contextupdater_description;
    int         contextupdater_workflow_pool_size = 1;
    int         max_context_variables = 1024;
  };

  struct ReservedPayloadRefs {
    std::string                       agent_payload_ref;
    std::map<std::string, std::string> session_payload_refs;
    std::map<std::string, std::string> task_payload_refs;
    std::map<std::string, std::string> workflow_payload_refs;
    std::map<std::string, std::string> node_payload_refs;
  };

  explicit AgentContext(const Config& config);
  ~AgentContext();

  const std::string&             get_context_id() const { return config_.context_id; }
  const std::string&             get_agent_id() const { return config_.agent_id; }
  const std::string&             get_context_workflow() const { return config_.context_workflow; }
  const Config&                  get_config() const { return config_; }
  std::shared_ptr<MMAgentTask>   get_context_updater_task() const { return context_updater_task_; }
  ReservedPayloadRefs            get_reserved_payloads() const;

  void set_variable(const std::string& key, const std::any& value);
  bool get_variable(const std::string& key, std::any& value) const;
  bool has_variable(const std::string& key) const;
  void remove_variable(const std::string& key);
  void clear_all_variables();

  void set_env_variable(const std::string& key, const std::string& value);
  bool get_env_variable(const std::string& key, std::string& value) const;
  void clear_env_variables();

  void set_property(const std::string& key, const std::string& value);
  bool get_property(const std::string& key, std::string& value) const;
  int  get_variable_count() const;
  bool is_variables_full() const;

  void reserve_session_payload(const std::string& session_id,
                               const MMAgentMsgQue::Message& message);
  void reserve_task_payload(const std::string& task_id);
  void reserve_workflow_payload(const std::string& workflow_id);
  void reserve_node_payload(const std::string& node_id);

  bool is_valid() const { return is_valid_; }
  void invalidate();

 private:
  Config                             config_;
  std::map<std::string, std::any>    context_variables_;
  std::map<std::string, std::string> env_variables_;
  std::map<std::string, std::string> properties_;
  ReservedPayloadRefs                reserved_payloads_;
  std::shared_ptr<MMAgentTask>       context_updater_task_;
  bool                               is_valid_ = true;
  mutable std::mutex                 mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_CONTEXT_H
