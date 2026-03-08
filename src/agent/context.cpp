#include "agent/context.h"

#include <algorithm>

#include "agent/dataref_bridge.h"
#include "agent/task.h"

namespace agentflow {
namespace framework {

AgentContext::AgentContext(const Config& config) : config_(config), is_valid_(true) {
  const std::string workflow =
      config_.contextupdater_workflow.empty() ? config_.context_workflow : config_.contextupdater_workflow;
  if(!workflow.empty()) {
    MMAgentTask::Config updater_config;
    updater_config.task_id =
        config_.contextupdater_task_id.empty() ? config_.context_id + "_contextupdater"
                                               : config_.contextupdater_task_id;
    updater_config.task_type = "contextupdater";
    updater_config.description =
        config_.contextupdater_description.empty() ? "Agent context updater"
                                                   : config_.contextupdater_description;
    updater_config.session_id = config_.agent_id;
    updater_config.context_workflow = workflow;
    updater_config.workflow_id = workflow;
    updater_config.workflow_file = workflow;
    updater_config.workflow_pool_size =
        std::max(1, config_.contextupdater_workflow_pool_size);
    updater_config.disable_context = true;
    context_updater_task_ = std::make_shared<MMAgentTask>(updater_config);
  }
}

AgentContext::~AgentContext() {
  std::lock_guard<std::mutex> lock(mutex_);
  context_variables_.clear();
  env_variables_.clear();
  properties_.clear();
  reserved_payloads_ = ReservedPayloadRefs{};
}

AgentContext::ReservedPayloadRefs AgentContext::get_reserved_payloads() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return reserved_payloads_;
}

void AgentContext::set_variable(const std::string& key, const std::any& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(static_cast<int>(context_variables_.size()) < config_.max_context_variables) {
    context_variables_[key] = value;
  }
}

bool AgentContext::get_variable(const std::string& key, std::any& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = context_variables_.find(key);
  if(it == context_variables_.end()) {
    return false;
  }
  value = it->second;
  return true;
}

bool AgentContext::has_variable(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return context_variables_.count(key) > 0;
}

void AgentContext::remove_variable(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  context_variables_.erase(key);
}

void AgentContext::clear_all_variables() {
  std::lock_guard<std::mutex> lock(mutex_);
  context_variables_.clear();
}

void AgentContext::set_env_variable(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  env_variables_[key] = value;
}

bool AgentContext::get_env_variable(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = env_variables_.find(key);
  if(it == env_variables_.end()) {
    return false;
  }
  value = it->second;
  return true;
}

void AgentContext::clear_env_variables() {
  std::lock_guard<std::mutex> lock(mutex_);
  env_variables_.clear();
}

void AgentContext::set_property(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  properties_[key] = value;
}

bool AgentContext::get_property(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = properties_.find(key);
  if(it == properties_.end()) {
    return false;
  }
  value = it->second;
  return true;
}

int AgentContext::get_variable_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(context_variables_.size());
}

bool AgentContext::is_variables_full() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(context_variables_.size()) >= config_.max_context_variables;
}

void AgentContext::reserve_session_payload(const std::string& session_id,
                                           const MMAgentMsgQue::Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string payload_ref = message.data_ref;
  if(payload_ref.empty() && !message.content.empty()) {
    payload_ref = dataref_bridge::store_payload(
        message.content, message.metadata,
        "context:" + config_.agent_id + ":session:" + session_id + ":" + message.message_id);
  }
  reserved_payloads_.session_payload_refs[session_id] = payload_ref;
}

void AgentContext::reserve_task_payload(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  reserved_payloads_.task_payload_refs.try_emplace(task_id, "");
}

void AgentContext::reserve_workflow_payload(const std::string& workflow_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  reserved_payloads_.workflow_payload_refs.try_emplace(workflow_id, "");
}

void AgentContext::reserve_node_payload(const std::string& node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  reserved_payloads_.node_payload_refs.try_emplace(node_id, "");
}

void AgentContext::invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  is_valid_ = false;
}

}  // namespace framework
}  // namespace agentflow
