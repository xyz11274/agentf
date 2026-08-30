// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// ResponsePoller - Background polling of agent responses and routing to RPC clients

#ifndef AGENTFLOW_DAEMON_RESPONSE_POLLER_H
#define AGENTFLOW_DAEMON_RESPONSE_POLLER_H

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace agentflow {
namespace daemon {

class RpcServer;
class AgentRegistry;

struct InFlightRequest {
  std::string message_id;
  std::string agent_id;
  int         client_fd = -1;
  int64_t     submitted_at_ms = 0;
  std::string dialogue_id;
};

class ResponsePoller {
public:
  ResponsePoller();
  ~ResponsePoller();

  void start(AgentRegistry& registry, RpcServer& server);
  void stop();

  void register_request(const std::string& message_id, int client_fd,
                        const std::string& agent_id, const std::string& dialogue_id);
  void remove_request(const std::string& message_id);
  void remove_requests_for_client(int client_fd);

  bool is_running() const { return running_.load(); }

private:
  void poll_loop();
  std::string build_response_notification(const std::string& content,
                                          const std::string& metadata_json,
                                          bool final_flag);

  AgentRegistry* registry_ = nullptr;
  RpcServer* rpc_server_ = nullptr;
  std::atomic<bool> running_{false};
  std::thread poll_thread_;

  std::map<std::string, InFlightRequest> in_flight_;
  mutable std::mutex in_flight_mutex_;
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_RESPONSE_POLLER_H
