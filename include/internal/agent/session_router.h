#ifndef AGENTFLOW_INTERNAL_AGENT_SESSION_ROUTER_H
#define AGENTFLOW_INTERNAL_AGENT_SESSION_ROUTER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "agent/msgque.h"

namespace agentflow {
namespace framework {

class MMAgentSession;

class SessionRouter {
 public:
  struct Config {
    std::string router_id;
    int         max_queue_size = 1024;
    int64_t     session_timeout_ms = 30000;
    int64_t     aging_interval_ms = 250;
  };

  using RouteHandler =
      std::function<void(const std::shared_ptr<MMAgentSession>&, const MMAgentMsgQue::Message&)>;

  explicit SessionRouter(const Config& config);
  ~SessionRouter();

  void set_session_pool(const std::vector<std::shared_ptr<MMAgentSession>>& sessions);
  void set_route_handler(RouteHandler handler);

  bool start();
  void stop();
  bool is_running() const { return running_; }

  bool route_request_message(const MMAgentMsgQue::Message& message);
  int  drain_pending_requests();

  std::shared_ptr<MMAgentSession> get_or_create_session(const std::string& user_id,
                                                        const std::string& client_id);
  std::shared_ptr<MMAgentSession> get_session_by_route_key(const std::string& route_key) const;
  std::string                     get_route_key_for_session(const std::string& session_id) const;
  std::vector<std::string>        get_active_route_keys() const;

  bool enqueue_response_message(const MMAgentMsgQue::Message& message);
  bool dequeue_response_message(MMAgentMsgQue::Message& message);
  int  get_response_queue_size() const;

 private:
  bool handle_route_message(const MMAgentMsgQue::Message& message);
  std::shared_ptr<MMAgentSession> allocate_session_for_route(const std::string& route_key,
                                                             const std::string& user_id,
                                                             const std::string& client_id,
                                                             const std::string& session_hint);
  void release_expired_sessions();
  void release_route_locked(const std::string& route_key);
  void router_loop();
  void aging_loop();

  Config config_;
  MMAgentMsgQue request_queue_;
  MMAgentMsgQue response_queue_;
  RouteHandler route_handler_;

  std::vector<std::shared_ptr<MMAgentSession>> session_pool_;
  std::map<std::string, std::shared_ptr<MMAgentSession>> active_routes_;
  std::map<std::string, std::shared_ptr<MMAgentSession>> physical_sessions_;
  std::map<std::string, std::string> session_to_route_;

  mutable std::mutex mutex_;
  std::condition_variable request_cv_;
  std::thread router_thread_;
  std::thread aging_thread_;
  std::atomic<bool> running_{false};
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_SESSION_ROUTER_H
