#include "agent/session.h"

#include <chrono>
#include <random>

#include "agent/task.h"

namespace agentflow {
namespace framework {

namespace {
int64_t get_current_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
  ).count();
}
}  // namespace

/// Generate a UUID v4 format string.
/// Uses random number generation to create a UUID-like identifier.
/// Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx where:
/// - 4 indicates UUID version 4
/// - y is one of 8, 9, a, or b
std::string generate_session_uuid() {
  // Use random device for seed
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  // Generate two 64-bit random numbers for the UUID
  uint64_t part1 = dis(gen);
  uint64_t part2 = dis(gen);

  // Format as UUID v4: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
  // Set version bits (4 in position 48-51 of part1)
  part1 = (part1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  // Set variant bits (10xx in position 62-63 of part2)
  part2 = (part2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  // Convert to hex string
  char buf[37];
  snprintf(buf, sizeof(buf),
           "%08x-%04x-%04x-%04x-%012llx",
           static_cast<uint32_t>(part1 >> 32),
           static_cast<uint16_t>((part1 >> 16) & 0xFFFF),
           static_cast<uint16_t>(part1 & 0xFFFF),
           static_cast<uint16_t>(part2 >> 48),
           static_cast<unsigned long long>(part2 & 0xFFFFFFFFFFFFULL));

  return std::string(buf);
}

MMAgentSession::MMAgentSession(const Config& config)
    : config_(config),
      state_(SessionState::CREATED) {
  // Generate UUID session_id if not provided
  if (config_.session_id.empty()) {
    config_.session_id = generate_session_uuid();
  }
  created_time_ = get_current_time_ms();
  last_activity_time_ = created_time_;
}

MMAgentSession::~MMAgentSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_.clear();
  template_tasks_.clear();
  runtime_tasks_.clear();
}

bool MMAgentSession::activate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(state_ == SessionState::TERMINATED) {
    return false;
  }
  state_ = SessionState::ACTIVE;
  active_seq_.fetch_add(1);  // P0-1 方案 D：每次激活自增，用于 aging callback 校验复用竞态
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

void MMAgentSession::bind_route(const std::string& user_id, const std::string& client_id) {
  SessionEventCallback callback_copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_user_id_ = user_id;
    active_client_id_ = client_id;
    active_route_key_ = user_id.empty() && client_id.empty() ? std::string() : user_id + ":" + client_id;
    if(!user_id.empty()) {
      metadata_["user_id"] = user_id;
    }
    if(!client_id.empty()) {
      metadata_["client_id"] = client_id;
    }
    if(!active_route_key_.empty()) {
      metadata_["route_key"] = active_route_key_;
    }
    state_ = SessionState::ACTIVE;
    last_activity_time_ = get_current_time_ms();
    callback_copy = session_event_callback_;
  }
  // Call callback outside the lock to avoid potential deadlocks
  if(callback_copy) {
    callback_copy(config_.session_id, user_id, client_id, SessionEventType::CREATED);
  }
}

void MMAgentSession::reset_for_pool() {
  std::string user_id_copy;
  std::string client_id_copy;
  SessionEventCallback callback_copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    user_id_copy = active_user_id_;
    client_id_copy = active_client_id_;
    state_ = SessionState::CREATED;
    active_route_key_.clear();
    active_user_id_.clear();
    active_client_id_.clear();
    metadata_.clear();
    runtime_tasks_.clear();
    // Clear running skills count on dispatcher and coordinator
    if(dispatcher_task_) {
      dispatcher_task_->clear_running_skills();
    }
    if(coordinator_task_) {
      coordinator_task_->clear_running_skills();
    }
    last_activity_time_ = get_current_time_ms();
    callback_copy = session_event_callback_;
  }
  // Call callback outside the lock to avoid potential deadlocks
  if(callback_copy && !user_id_copy.empty()) {
    callback_copy(config_.session_id, user_id_copy, client_id_copy, SessionEventType::DESTROYED);
  }
}

void MMAgentSession::set_metadata(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_[key] = value;
}

bool MMAgentSession::get_metadata(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = metadata_.find(key);
  if(it == metadata_.end()) {
    return false;
  }
  value = it->second;
  return true;
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
  if(active_route_key_.empty()) {
    return false;
  }
  const int64_t current_time = get_current_time_ms();
  return (current_time - last_activity_time_) > config_.timeout_ms;
}

bool MMAgentSession::enqueue_received_message(const MMAgentMsgQue::Message& message) {
  update_activity();
  auto dispatcher = get_dispatcher_task();
  return dispatcher ? dispatcher->enqueue_intent_request_message(message) : false;
}

bool MMAgentSession::dequeue_received_message(MMAgentMsgQue::Message& message) {
  return dequeue_intent_request_message(message) || dequeue_data_exchange_message(message);
}

bool MMAgentSession::dequeue_intent_request_message(MMAgentMsgQue::Message& message) {
  (void)message;
  return false;
}

bool MMAgentSession::dequeue_data_exchange_message(MMAgentMsgQue::Message& message) {
  (void)message;
  return false;
}

bool MMAgentSession::enqueue_send_message(const MMAgentMsgQue::Message& message) {
  update_activity();
  auto coordinator = get_coordinator_task();
  return coordinator ? coordinator->enqueue_data_exchange_message(message) : false;
}

bool MMAgentSession::dequeue_send_message(MMAgentMsgQue::Message& message) {
  (void)message;
  return false;
}

int MMAgentSession::get_receive_queue_size() const {
  int total = 0;
  if(auto dispatcher = get_dispatcher_task(); dispatcher) {
    total += dispatcher->get_intent_request_queue_size();
    total += dispatcher->get_data_exchange_queue_size();
  }
  if(auto coordinator = get_coordinator_task(); coordinator) {
    total += coordinator->get_intent_request_queue_size();
    total += coordinator->get_data_exchange_queue_size();
  }
  return total;
}

int MMAgentSession::get_send_queue_size() const {
  return 0;
}

void MMAgentSession::add_template_task(const std::shared_ptr<MMAgentTask>& task) {
  if(!task) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  template_tasks_[task->get_task_id()] = task;
}

void MMAgentSession::add_task(const std::shared_ptr<MMAgentTask>& task) {
  if(!task) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  runtime_tasks_[task->get_task_id()] = task;
}

std::shared_ptr<MMAgentTask> MMAgentSession::get_task(const std::string& task_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(const auto runtime_it = runtime_tasks_.find(task_id); runtime_it != runtime_tasks_.end()) {
    return runtime_it->second;
  }
  if(const auto template_it = template_tasks_.find(task_id); template_it != template_tasks_.end()) {
    return template_it->second;
  }
  return nullptr;
}

std::vector<std::shared_ptr<MMAgentTask>> MMAgentSession::get_tasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<MMAgentTask>> result;
  result.reserve(template_tasks_.size() + runtime_tasks_.size());
  for(const auto& kv : template_tasks_) {
    result.push_back(kv.second);
  }
  for(const auto& kv : runtime_tasks_) {
    result.push_back(kv.second);
  }
  return result;
}

int MMAgentSession::get_template_task_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(template_tasks_.size());
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

void MMAgentSession::set_dispatcher_task(const std::shared_ptr<MMAgentTask>& task) {
  std::lock_guard<std::mutex> lock(mutex_);
  dispatcher_task_ = task;
}

void MMAgentSession::set_coordinator_task(const std::shared_ptr<MMAgentTask>& task) {
  std::lock_guard<std::mutex> lock(mutex_);
  coordinator_task_ = task;
}

std::shared_ptr<MMAgentTask> MMAgentSession::get_dispatcher_task() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dispatcher_task_;
}

std::shared_ptr<MMAgentTask> MMAgentSession::get_coordinator_task() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return coordinator_task_;
}

void MMAgentSession::clear_running_skills() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(dispatcher_task_) {
    dispatcher_task_->clear_running_skills();
  }
  if(coordinator_task_) {
    coordinator_task_->clear_running_skills();
  }
}

void MMAgentSession::clear_all_tasks() {
  std::lock_guard<std::mutex> lock(mutex_);
  runtime_tasks_.clear();
  template_tasks_.clear();
  dispatcher_task_.reset();
  coordinator_task_.reset();
}

// ── Dialogue serialization lock ─────────────────────────────────────

std::unique_ptr<DialogueLockGuard> MMAgentSession::acquire_dialogue_lock(
    const std::string& dialogue_id) {
  std::unique_lock<std::mutex> lk(dialogue_serialize_mutex_);
  dialogue_serialize_cv_.wait(lk, [this]() {
    return active_dialogue_id_.empty() || session_cancelled_.load();
  });
  if (session_cancelled_.load()) {
    // Session being destroyed — caller must NOT proceed.
    return nullptr;
  }
  active_dialogue_id_ = dialogue_id;
  return std::unique_ptr<DialogueLockGuard>(
      new DialogueLockGuard(*this, dialogue_id));
}

void MMAgentSession::cancel_all_dialogues() {
  // Drain all held locks so each guard's destructor releases the slot.
  std::vector<std::unique_ptr<DialogueLockGuard>> victims;
  {
    std::lock_guard<std::mutex> lk(dialogue_serialize_mutex_);
    session_cancelled_.store(true);
    victims.reserve(held_locks_.size());
    for (auto& [did, guard] : held_locks_) {
      victims.push_back(std::move(guard));
    }
    held_locks_.clear();
  }
  // Destroying victims outside the lock clears active_dialogue_id_ (only
  // the first matching one wins; others are no-ops) and notifies waiters.
  victims.clear();
  // Wake all waiters so they observe session_cancelled_ and return nullptr.
  dialogue_serialize_cv_.notify_all();
}

std::string MMAgentSession::get_active_dialogue_id() const {
  std::lock_guard<std::mutex> lk(dialogue_serialize_mutex_);
  return active_dialogue_id_;
}

void MMAgentSession::hold_dialogue_lock(
    const std::string& dialogue_id,
    std::unique_ptr<DialogueLockGuard> guard) {
  if (!guard) {
    // Caller aborted before start — nothing to hold.
    return;
  }
  std::lock_guard<std::mutex> lk(dialogue_serialize_mutex_);
  // If a lock is already held for this dialogue_id, the prior one is
  // overwritten and its destructor releases the slot.  This should not
  // happen in normal flow (dialogue_id is unique per request), but the
  // defensive erase keeps the map bounded.
  held_locks_[dialogue_id] = std::move(guard);
}

void MMAgentSession::release_dialogue_lock(const std::string& dialogue_id) {
  std::unique_ptr<DialogueLockGuard> victim;
  {
    std::lock_guard<std::mutex> lk(dialogue_serialize_mutex_);
    auto it = held_locks_.find(dialogue_id);
    if (it == held_locks_.end()) {
      return;  // Nothing held — idempotent.
    }
    victim = std::move(it->second);
    held_locks_.erase(it);
  }
  // Destroying victim outside the lock runs ~DialogueLockGuard which
  // clears active_dialogue_id_ and notifies the next waiter.
}

void MMAgentSession::release_dialogue_slot(const std::string& dialogue_id) {
  {
    std::lock_guard<std::mutex> lk(dialogue_serialize_mutex_);
    // Only clear if we still own the slot (defensive against double-release).
    if (active_dialogue_id_ == dialogue_id) {
      active_dialogue_id_.clear();
    }
  }
  dialogue_serialize_cv_.notify_one();
}

// ── DialogueLockGuard ───────────────────────────────────────────────

DialogueLockGuard::DialogueLockGuard(MMAgentSession& session,
                                     std::string dialogue_id)
    : session_(session), dialogue_id_(std::move(dialogue_id)),
      cancelled_(session.session_cancelled_.load()) {}

DialogueLockGuard::~DialogueLockGuard() {
  session_.release_dialogue_slot(dialogue_id_);
}

}  // namespace framework
}  // namespace agentflow
