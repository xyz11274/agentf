#include "agent/session.h"

#include <sys/time.h>
#include <mutex>

#include "agent/context.h"
#include "agent/task.h"

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
}  // namespace

MMAgentSession::MMAgentSession(const Config& config)
    : config_(config),
      state_(SessionState::CREATED),
      receive_queue_(MMAgentMsgQue::Config{config.max_queue_size}),
      send_queue_(MMAgentMsgQue::Config{config.max_queue_size}) {
  created_time_ = get_current_time_ms();
  last_activity_time_ = created_time_;
  AgentContext::Config context_config;
  context_config.context_id = config_.session_id + "_context";
  context_config.agent_id = config_.session_id;
  context_config.context_workflow = config_.session_context_workflow;
  context_ = std::make_shared<AgentContext>(context_config);
}

MMAgentSession::~MMAgentSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_.clear();
  tasks_.clear();
}

bool MMAgentSession::activate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != SessionState::CREATED) {
    return false;
  }
  state_ = SessionState::ACTIVE;
  last_activity_time_ = get_current_time_ms();
  return true;
}

bool MMAgentSession::pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != SessionState::ACTIVE) {
    return false;
  }
  state_ = SessionState::PAUSED;
  last_activity_time_ = get_current_time_ms();
  return true;
}

bool MMAgentSession::resume() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ != SessionState::PAUSED) {
    return false;
  }
  state_ = SessionState::ACTIVE;
  last_activity_time_ = get_current_time_ms();
  return true;
}

bool MMAgentSession::complete() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == SessionState::TERMINATED || state_ == SessionState::COMPLETED) {
    return false;
  }
  state_ = SessionState::COMPLETED;
  last_activity_time_ = get_current_time_ms();
  return true;
}

bool MMAgentSession::terminate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == SessionState::TERMINATED) {
    return false;
  }
  state_ = SessionState::TERMINATED;
  last_activity_time_ = get_current_time_ms();
  return true;
}

void MMAgentSession::set_metadata(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_[key] = value;
}

bool MMAgentSession::get_metadata(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = metadata_.find(key);
  if(it != metadata_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

bool MMAgentSession::has_metadata(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metadata_.count(key) > 0;
}

void MMAgentSession::clear_metadata() {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_.clear();
}

void MMAgentSession::update_activity() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_activity_time_ = get_current_time_ms();
}

bool MMAgentSession::is_timeout_expired() const {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t current_time = get_current_time_ms();
  return (current_time - last_activity_time_) > config_.timeout_ms;
}

bool MMAgentSession::enqueue_received_message(const MMAgentMsgQue::Message& message) {
  update_activity();
  return receive_queue_.enqueue_message(message);
}

bool MMAgentSession::dequeue_received_message(MMAgentMsgQue::Message& message) {
  bool ok = receive_queue_.dequeue_message(message);
  if(ok) {
    update_activity();
  }
  return ok;
}

bool MMAgentSession::dequeue_intent_request_message(MMAgentMsgQue::Message& message) {
  bool ok = receive_queue_.dequeue_message_by_type(MMAgentMsgQue::MessageQueueType::INTENT_REQUEST, message);
  if(ok) {
    update_activity();
  }
  return ok;
}

bool MMAgentSession::dequeue_output_result_message(MMAgentMsgQue::Message& message) {
  bool ok = receive_queue_.dequeue_message_by_type(MMAgentMsgQue::MessageQueueType::OUTPUT_RESULT, message);
  if(ok) {
    update_activity();
  }
  return ok;
}

bool MMAgentSession::enqueue_send_message(const MMAgentMsgQue::Message& message) {
  update_activity();
  return send_queue_.enqueue_message(message);
}

bool MMAgentSession::dequeue_send_message(MMAgentMsgQue::Message& message) {
  bool ok = send_queue_.dequeue_message(message);
  if(ok) {
    update_activity();
  }
  return ok;
}

int MMAgentSession::get_receive_queue_size() const {
  return receive_queue_.get_queue_size();
}

int MMAgentSession::get_send_queue_size() const {
  return send_queue_.get_queue_size();
}

void MMAgentSession::add_task(const std::shared_ptr<MMAgentTask>& task) {
  if(!task) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  tasks_[task->get_task_id()] = task;
}

std::shared_ptr<MMAgentTask> MMAgentSession::get_task(const std::string& task_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        it = tasks_.find(task_id);
  if(it == tasks_.end()) {
    return nullptr;
  }
  return it->second;
}

std::vector<std::shared_ptr<MMAgentTask>> MMAgentSession::get_tasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<MMAgentTask>> result;
  result.reserve(tasks_.size());
  for(const auto& kv : tasks_) {
    result.push_back(kv.second);
  }
  return result;
}

void MMAgentSession::set_task_coordinator(
    const std::shared_ptr<SessionTaskCoordinator>& coordinator) {
  std::lock_guard<std::mutex> lock(mutex_);
  task_coordinator_ = coordinator;
}

void MMAgentSession::set_task_dispatcher(
    const std::shared_ptr<SessionTaskCoordinator>& dispatcher) {
  std::lock_guard<std::mutex> lock(mutex_);
  task_dispatcher_ = dispatcher;
}

}  // namespace framework
}  // namespace agentflow
