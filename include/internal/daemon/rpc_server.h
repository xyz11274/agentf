// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// RpcServer - TCP + dual UDS JSON-RPC 2.0 server for agentflowd

#ifndef AGENTFLOW_DAEMON_RPC_SERVER_H
#define AGENTFLOW_DAEMON_RPC_SERVER_H

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agentflow {
namespace daemon {

enum class ChannelType {
  TCP,       // All methods
  UDS_BIZ,   // Business: agent.execute/poll_response/send_control, databus.*, event.*
  UDS_CTRL,  // Control: agent.load/unload/list/info/pause/resume, session.*, workflow.*, worker.*, runtime.*, metrics.*, health
};

using RpcMethodHandler = std::function<std::string(const std::string& params, int64_t id,
                                                   int client_fd, ChannelType channel)>;

class RpcServer {
public:
  struct Config {
    std::string listen = "0.0.0.0:7890";
    std::string uds_biz_path = "/tmp/agentflowd_biz.sock";
    std::string uds_ctrl_path = "/tmp/agentflowd_ctrl.sock";
    int max_connections = 1024;
  };

  RpcServer();
  ~RpcServer();

  bool start(const Config& config);
  void stop();

  void register_handler(const std::string& method, RpcMethodHandler handler);

  // Send a JSON-RPC notification to a specific client
  void send_notification(int client_fd, const std::string& method, const std::string& params_json);

  bool is_running() const { return running_.load(); }

  // Check if a method is allowed on a given channel
  static bool is_method_allowed(ChannelType channel, const std::string& method);

private:
  void tcp_accept_loop();
  void uds_accept_loop(const std::string& socket_path, ChannelType channel);
  void client_handler(int fd, ChannelType channel);
  std::string handle_request(const std::string& request_json, int client_fd, ChannelType channel);

  // JSON-RPC response builders
  static std::string build_success_response(const std::string& result, int64_t id);
  static std::string build_error_response(int code, const std::string& message, int64_t id);
  static std::string build_notification(const std::string& method, const std::string& params);

  void cleanup_uds(const std::string& path);

  Config config_;
  std::atomic<bool> running_{false};

  // Server file descriptors
  int tcp_fd_ = -1;
  int uds_biz_fd_ = -1;
  int uds_ctrl_fd_ = -1;

  // Accept threads
  std::thread tcp_accept_thread_;
  std::thread uds_biz_accept_thread_;
  std::thread uds_ctrl_accept_thread_;

  // Client handling threads
  std::vector<std::thread> client_threads_;
  std::mutex client_threads_mutex_;

  // Method handlers
  std::map<std::string, RpcMethodHandler> handlers_;
  std::mutex handlers_mutex_;

  // Active client tracking
  std::map<int, ChannelType> active_clients_;
  std::mutex clients_mutex_;
  std::atomic<int> connection_count_{0};

  // WebSocket client tracking: fd → ws_mode
  // send_notification() needs this to wrap push notifications in WebSocket frames
  // for clients connected via WebSocket. BUG FIX: send_notification() previously
  // always sent raw bytes + "\n", which broke WebSocket framing for async pushes
  // like "agent.response" and "event.notification".
  std::map<int, bool> ws_clients_;
  std::mutex ws_clients_mutex_;
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_RPC_SERVER_H
