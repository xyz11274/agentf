#include "agent/session_router.h"

#include <chrono>

#include "agent/session.h"
#include "utils/logger.h"

namespace agentflow {
namespace framework {

SessionRouter::SessionRouter(const Config& config)
    : config_(config),
      request_queue_(MMAgentMsgQue::Config{config.max_queue_size}),
      response_queue_(MMAgentMsgQue::Config{config.max_queue_size}) {
}

SessionRouter::~SessionRouter() {
  stop();
}

void SessionRouter::set_session_pool(const std::vector<std::shared_ptr<MMAgentSession>>& sessions) {
  std::lock_guard<std::mutex> lock(mutex_);
  session_pool_ = sessions;
  active_routes_.clear();
  physical_sessions_.clear();
  session_to_route_.clear();
  for(const auto& session : session_pool_) {
    if(!session) {
      continue;
    }
    session->reset_for_pool();
    physical_sessions_[session->get_session_id()] = session;
  }
}

void SessionRouter::set_route_handler(RouteHandler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  route_handler_ = std::move(handler);
}

void SessionRouter::set_session_factory(SessionFactory factory) {
  std::lock_guard<std::mutex> lock(mutex_);
  session_factory_ = std::move(factory);
}

void SessionRouter::set_session_cleanup_callback(SessionCleanupCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  session_cleanup_callback_ = std::move(callback);
}

bool SessionRouter::start() {
  bool expected = false;
  if(!running_.compare_exchange_strong(expected, true)) {
    return false;
  }
  router_thread_ = std::thread(&SessionRouter::router_loop, this);
  aging_thread_ = std::thread(&SessionRouter::aging_loop, this);
  return true;
}

void SessionRouter::stop() {
  bool expected = true;
  if(!running_.compare_exchange_strong(expected, false)) {
    return;
  }
  request_cv_.notify_all();
  if(router_thread_.joinable()) {
    router_thread_.join();
  }
  if(aging_thread_.joinable()) {
    aging_thread_.join();
  }
}

bool SessionRouter::route_request_message(const MMAgentMsgQue::Message& message) {
  if(!running_) {
    return handle_route_message(message);
  }
  const bool ok = request_queue_.enqueue_message(message);
  if(ok) {
    request_cv_.notify_all();
  }
  return ok;
}

int SessionRouter::drain_pending_requests() {
  int count = 0;
  MMAgentMsgQue::Message message;
  while(request_queue_.dequeue_message(message)) {
    if(handle_route_message(message)) {
      ++count;
    }
  }
  return count;
}

std::shared_ptr<MMAgentSession> SessionRouter::get_or_create_session(const std::string& user_id,
                                                                     const std::string& client_id) {
  const std::string route_key = user_id + ":" + client_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(const auto it = active_routes_.find(route_key); it != active_routes_.end()) {
      return it->second;
    }
  }
  return allocate_session_for_route(route_key, user_id, client_id, "");
}

std::shared_ptr<MMAgentSession> SessionRouter::get_session_by_route_key(const std::string& route_key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(const auto direct = physical_sessions_.find(route_key); direct != physical_sessions_.end()) {
    return direct->second;
  }
  if(const auto routed = active_routes_.find(route_key); routed != active_routes_.end()) {
    return routed->second;
  }
  return nullptr;
}

std::string SessionRouter::get_route_key_for_session(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if(const auto it = session_to_route_.find(session_id); it != session_to_route_.end()) {
    return it->second;
  }
  return "";
}

std::vector<std::string> SessionRouter::get_active_route_keys() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> routes;
  routes.reserve(active_routes_.size());
  for(const auto& kv : active_routes_) {
    routes.push_back(kv.first);
  }
  return routes;
}

bool SessionRouter::enqueue_response_message(const MMAgentMsgQue::Message& message) {
  return response_queue_.enqueue_message(message);
}

bool SessionRouter::dequeue_response_message(MMAgentMsgQue::Message& message) {
  return response_queue_.dequeue_message(message);
}

int SessionRouter::get_response_queue_size() const {
  return response_queue_.get_queue_size();
}

int SessionRouter::get_request_queue_size() const {
  return request_queue_.get_queue_size();
}

bool SessionRouter::handle_route_message(const MMAgentMsgQue::Message& message) {
  const auto user_it = message.metadata.find("user_id");
  const auto client_it = message.metadata.find("client_id");
  const auto session_it = message.metadata.find("session_id");
  const std::string user_id = user_it != message.metadata.end() ? user_it->second : std::string();
  const std::string client_id = client_it != message.metadata.end() ? client_it->second : std::string();
  const std::string route_key = (!user_id.empty() || !client_id.empty()) ? user_id + ":" + client_id : std::string();
  const std::string session_hint = session_it != message.metadata.end() ? session_it->second : std::string();

  std::shared_ptr<MMAgentSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!route_key.empty()) {
      if(const auto routed = active_routes_.find(route_key); routed != active_routes_.end()) {
        session = routed->second;
      }
    }
    if(!session && !session_hint.empty()) {
      if(const auto physical = physical_sessions_.find(session_hint); physical != physical_sessions_.end()) {
        session = physical->second;
      }
    }
  }
  if(!session) {
    session = allocate_session_for_route(route_key, user_id, client_id, session_hint);
  } else if(!route_key.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_routes_[route_key] = session;
    session_to_route_[session->get_session_id()] = route_key;
    session->bind_route(user_id, client_id);
  }

  RouteHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handler = route_handler_;
  }
  if(!session || !handler) {
    return false;
  }
  handler(session, message);
  return true;
}

std::shared_ptr<MMAgentSession> SessionRouter::allocate_session_for_route(
    const std::string& route_key, const std::string& user_id, const std::string& client_id,
    const std::string& session_hint) {
  // First, try to find a session without holding the lock (for factory call)
  SessionFactory factory_copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    factory_copy = session_factory_;
  }

  std::shared_ptr<MMAgentSession> session;
  if(!session_hint.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(const auto it = physical_sessions_.find(session_hint); it != physical_sessions_.end()) {
      session = it->second;
    }
  }
  if(!session) {
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& candidate : session_pool_) {
      if(candidate && candidate->is_available()) {
        session = candidate;
        break;
      }
    }
  }

  // If no pool session available, use factory to create a new session
  if(!session && factory_copy) {
    // Create session outside of lock to avoid potential deadlock
    session = factory_copy(user_id, client_id);
    if(session) {
      std::lock_guard<std::mutex> lock(mutex_);
      // Register the new session
      physical_sessions_[session->get_session_id()] = session;
      Logger::instance().info_f("[SessionRouter] Created new session via factory: session_id=%s for route=%s",
                               session->get_session_id().c_str(), route_key.c_str());
    }
  }

  if(!session) {
    return nullptr;
  }

  session->reset_for_pool();
  session->activate();
  if(!route_key.empty()) {
    session->bind_route(user_id, client_id);
    std::lock_guard<std::mutex> lock(mutex_);
    active_routes_[route_key] = session;
    session_to_route_[session->get_session_id()] = route_key;
  }
  return session;
}

void SessionRouter::release_expired_sessions() {
  // 三元组：route_key, session_id, active_seq（抓 seq 副本在锁内，callback 在锁外用 seq 校验复用竞态）
  std::vector<std::tuple<std::string, std::string, uint64_t>> expired;
  std::vector<SessionCleanupCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& kv : active_routes_) {
      if(kv.second && kv.second->is_timeout_expired()) {
        expired.emplace_back(kv.first, kv.second->get_session_id(), kv.second->get_active_seq());
        callbacks.push_back(session_cleanup_callback_);
      }
    }
    for(const auto& [route_key, session_id, seq] : expired) {
      release_route_locked(route_key);
    }
  }
  // Run cleanup callbacks outside the lock to avoid deadlock with
  // MMAgent::clear_session_data which takes its own mutex.
  // seq 校验：若归还后 session 已被新用户复用（activate 自增了 seq），callback 跳过清理。
  for(size_t i = 0; i < callbacks.size(); ++i) {
    if(callbacks[i]) {
      callbacks[i](std::get<1>(expired[i]), std::get<2>(expired[i]));
    }
  }
}

void SessionRouter::release_route_locked(const std::string& route_key) {
  const auto it = active_routes_.find(route_key);
  if(it == active_routes_.end()) {
    return;
  }
  if(it->second) {
    session_to_route_.erase(it->second->get_session_id());
    it->second->reset_for_pool();
  }
  active_routes_.erase(it);
}

void SessionRouter::router_loop() {
  while(running_) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      request_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return !running_ || request_queue_.get_queue_size() > 0;
      });
    }
    if(!running_) {
      break;
    }
    drain_pending_requests();
  }
}

void SessionRouter::aging_loop() {
  while(running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.aging_interval_ms));
    if(!running_) {
      break;
    }
    release_expired_sessions();
  }
}

}  // namespace framework
}  // namespace agentflow
