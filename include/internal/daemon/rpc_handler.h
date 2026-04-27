// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// RpcHandler - JSON-RPC method routing and handler registration for agentflowd

#ifndef AGENTFLOW_DAEMON_RPC_HANDLER_H
#define AGENTFLOW_DAEMON_RPC_HANDLER_H

#include <atomic>
#include <memory>
#include <string>

#include "daemon/rpc_server.h"

namespace agentflow {
namespace runtime {
class MMAgentRuntime;
}

namespace daemon {

class RpcServer;
class AgentRegistry;
class ResponsePoller;
class EventBroker;

namespace agentflow {

namespace runtime {
class MMAgentRuntime;
}

namespace daemon {

class RpcServer;
class AgentRegistry;
class ResponsePoller;

class RpcHandler {
public:
  RpcHandler(runtime::MMAgentRuntime& runtime, AgentRegistry& registry,
             ResponsePoller& poller, EventBroker& event_broker);
  ~RpcHandler() = default;

  void register_all(RpcServer& server);

private:
  runtime::MMAgentRuntime& runtime_;
  AgentRegistry& registry_;
  ResponsePoller& poller_;
  EventBroker& event_broker_;
  RpcServer* server_ = nullptr;
  int64_t start_time_ms_;
  std::atomic<int64_t> message_counter_{0};

  // Helper: escape string for JSON
  static std::string json_escape(const std::string& s);

  // Query methods
  std::string handle_health(const std::string& params, int64_t id);
  std::string handle_runtime_status(const std::string& params, int64_t id);
  std::string handle_agent_list(const std::string& params, int64_t id);
  std::string handle_agent_info(const std::string& params, int64_t id);

  // Agent management methods
  std::string handle_agent_load(const std::string& params, int64_t id);
  std::string handle_agent_unload(const std::string& params, int64_t id);
  std::string handle_agent_pause(const std::string& params, int64_t id);
  std::string handle_agent_resume(const std::string& params, int64_t id);

  // Session methods
  std::string handle_session_list(const std::string& params, int64_t id);
  std::string handle_session_destroy(const std::string& params, int64_t id);
  std::string handle_session_clear_dialogue(const std::string& params, int64_t id);

  // Workflow methods
  std::string handle_workflow_list(const std::string& params, int64_t id);
  std::string handle_workflow_info(const std::string& params, int64_t id);
  std::string handle_workflow_list_instances(const std::string& params, int64_t id);

  // Worker methods
  std::string handle_worker_list(const std::string& params, int64_t id);

  // Runtime management
  std::string handle_runtime_shutdown(const std::string& params, int64_t id);

  // Request execution
  std::string handle_agent_execute(const std::string& params, int64_t id, int client_fd, ChannelType channel);
  std::string handle_agent_poll_response(const std::string& params, int64_t id);
  std::string handle_agent_send_control(const std::string& params, int64_t id);

  // Event methods
  std::string handle_event_subscribe(const std::string& params, int64_t id, int client_fd);
  std::string handle_event_unsubscribe(const std::string& params, int64_t id);

  // Metrics
  std::string handle_metrics_get(const std::string& params, int64_t id);
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_RPC_HANDLER_H
