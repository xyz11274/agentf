// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// RpcHandler - JSON-RPC method routing and handler registration for agentflowd

#ifndef AGENTFLOW_DAEMON_RPC_HANDLER_H
#define AGENTFLOW_DAEMON_RPC_HANDLER_H

#include <atomic>
#include <string>

#include "daemon/rpc_server.h"

namespace agentflow {

namespace runtime {
class MMAgentRuntime;
}

namespace daemon {

class AgentRegistry;
class ResponsePoller;
class EventBroker;
class SessionManager;  // AgentFoundry 沙箱机制移植
class SandboxLifecycle;  // AgentFoundry 沙箱生命周期管理

class RpcHandler {
public:
  RpcHandler(runtime::MMAgentRuntime& runtime, AgentRegistry& registry,
             ResponsePoller& poller, EventBroker& event_broker,
             SessionManager& session_manager,
             SandboxLifecycle* sandbox_lifecycle = nullptr,
             bool sandbox_allow_fallback = true);
  ~RpcHandler() = default;

  void register_all(RpcServer& server);

private:
  runtime::MMAgentRuntime& runtime_;
  AgentRegistry& registry_;
  ResponsePoller& poller_;
  EventBroker& event_broker_;
  SessionManager& session_manager_;  // AgentFoundry 沙箱机制移植
  SandboxLifecycle* sandbox_lifecycle_;  // nullable, mode=none 时为 nullptr
  bool sandbox_allow_fallback_;  // false=沙箱失败拒绝请求，true=降级到无沙箱
  RpcServer* server_ = nullptr;
  int64_t start_time_ms_;
  std::atomic<int64_t> message_counter_{0};

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

  // DataBus proxy
  std::string handle_databus_get(const std::string& params, int64_t id);
  std::string handle_databus_list_keys(const std::string& params, int64_t id);
  std::string handle_databus_remove_by_prefix(const std::string& params, int64_t id);

  // Sandbox methods (AgentFoundry 沙箱机制移植)
  std::string handle_sandbox_read_file(const std::string& params, int64_t id);
  // Upload binary asset into sandbox (symmetric to read_file; POST /upload, see contracts/bridge-sandbox-files.md)
  std::string handle_sandbox_upload_file(const std::string& params, int64_t id);
  // 沙箱管理：list_active 枚举 + destroy 销毁（local/docker/e2b 三模式统一，E1/E2）
  // 透传 SandboxLifecycle::list_active() / destroy()——本地开发可观测性管理能力
  std::string handle_sandbox_list(const std::string& params, int64_t id);
  std::string handle_sandbox_destroy(const std::string& params, int64_t id);

  // 推算会话级用户上传目录宿主机绝对路径（与 bridge content_serializer._compute_upload_dir 同构）
  // 返回：{agent_packages_path}/{agent_id}/workspace/{user_id}_{client_id}/upload/
  // agent package 路径取自 sandbox_lifecycle_->config().agents[i].package_path；找不到对应 agent 则返回空
  std::string _resolve_session_upload_dir(const std::string& agent_id,
                                          const std::string& user_id,
                                          const std::string& client_id);
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_RPC_HANDLER_H
