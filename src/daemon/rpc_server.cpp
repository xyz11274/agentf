// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// RpcServer implementation - TCP + dual UDS JSON-RPC 2.0 server

#include "daemon/rpc_server.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

#include <cctype>
#include <cstring>
#include <iostream>

#include "utils/logger.h"
#include "daemon/ws_upgrade.h"

namespace agentflow {
namespace daemon {

namespace {

// Minimal JSON helpers (same pattern as UDSServer)
std::string extract_json_string(const std::string& json, const std::string& key) {
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos);
  if (pos == std::string::npos) return "";
  pos++;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
  if (pos >= json.size()) return "";
  if (json[pos] == '"') {
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
  }
  // Simple unquoted value
  size_t end = pos;
  while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') end++;
  std::string val = json.substr(pos, end - pos);
  while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
  return val;
}

std::string extract_json_method(const std::string& json) {
  return extract_json_string(json, "method");
}

int64_t extract_json_id(const std::string& json) {
  std::string id_str = extract_json_string(json, "id");
  if (id_str.empty()) {
    // Try unquoted number
    std::string search = "\"id\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return -1;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return -1;
    pos++;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    size_t end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}') end++;
    try { return std::stoll(json.substr(pos, end - pos)); }
    catch (...) { return -1; }
  }
  try { return std::stoll(id_str); }
  catch (...) { return -1; }
}

std::string extract_json_params(const std::string& json) {
  std::string search = "\"params\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos);
  if (pos == std::string::npos) return "";
  pos++;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
  if (pos >= json.size()) return "";

  if (json[pos] == '{') {
    int depth = 0;
    size_t start = pos;
    for (; pos < json.size(); pos++) {
      if (json[pos] == '{') depth++;
      else if (json[pos] == '}') {
        depth--;
        if (depth == 0) return json.substr(start, pos - start + 1);
      }
    }
  } else if (json[pos] == '[') {
    int depth = 0;
    size_t start = pos;
    for (; pos < json.size(); pos++) {
      if (json[pos] == '[') depth++;
      else if (json[pos] == ']') {
        depth--;
        if (depth == 0) return json.substr(start, pos - start + 1);
      }
    }
  }
  return "";
}

std::string json_escape(const std::string& s) {
  std::string result;
  result.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// Parse "host:port" and create TCP server socket
int create_tcp_socket(const std::string& listen_addr, int max_conn) {
  auto pos = listen_addr.rfind(':');
  if (pos == std::string::npos) return -1;
  std::string host = listen_addr.substr(0, pos);
  int port = std::stoi(listen_addr.substr(pos + 1));

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, max_conn > 0 ? max_conn : 128) < 0) {
    close(fd);
    return -1;
  }

  set_nonblocking(fd);
  return fd;
}

int create_uds_socket(const std::string& path) {
  unlink(path.c_str());  // Clean up old socket

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, 64) < 0) {
    close(fd);
    unlink(path.c_str());
    return -1;
  }

  set_nonblocking(fd);
  return fd;
}

// Method access control sets
static const char* kUdsBizMethods[] = {
    "agent.execute", "agent.poll_response", "agent.send_control",
    "databus.put", "databus.get", "databus.remove", "databus.contains",
    "databus.subscribe", "databus.unsubscribe", "databus.publish", "databus.list_keys",
    "event.subscribe", "event.unsubscribe",
    nullptr
};

static const char* kUdsCtrlMethods[] = {
    "agent.load", "agent.unload", "agent.list", "agent.info", "agent.pause", "agent.resume",
    "session.list", "session.destroy", "session.clear_dialogue",
    "workflow.list", "workflow.info", "workflow.list_instances",
    "worker.list",
    "runtime.status", "runtime.shutdown",
    "health", "metrics.get",
    nullptr
};

bool in_list(const char* list[], const std::string& method) {
  for (int i = 0; list[i] != nullptr; ++i) {
    if (method == list[i]) return true;
  }
  return false;
}

}  // namespace

RpcServer::RpcServer() = default;

RpcServer::~RpcServer() {
  stop();
}

bool RpcServer::start(const Config& config) {
  if (running_.load()) return true;
  config_ = config;

  // Create TCP socket
  tcp_fd_ = create_tcp_socket(config_.listen, config_.max_connections);
  if (tcp_fd_ < 0) {
    Logger::instance().error_f("[RPC_SERVER] Failed to create TCP socket: %s", config_.listen.c_str());
    return false;
  }

  // Create UDS business socket
  uds_biz_fd_ = create_uds_socket(config_.uds_biz_path);
  if (uds_biz_fd_ < 0) {
    Logger::instance().error_f("[RPC_SERVER] Failed to create UDS biz socket: %s", config_.uds_biz_path.c_str());
    close(tcp_fd_);
    tcp_fd_ = -1;
    return false;
  }

  // Create UDS control socket
  uds_ctrl_fd_ = create_uds_socket(config_.uds_ctrl_path);
  if (uds_ctrl_fd_ < 0) {
    Logger::instance().error_f("[RPC_SERVER] Failed to create UDS ctrl socket: %s", config_.uds_ctrl_path.c_str());
    close(tcp_fd_);
    close(uds_biz_fd_);
    tcp_fd_ = -1;
    uds_biz_fd_ = -1;
    return false;
  }

  running_ = true;

  // Start accept threads
  tcp_accept_thread_ = std::thread(&RpcServer::tcp_accept_loop, this);
  uds_biz_accept_thread_ = std::thread(&RpcServer::uds_accept_loop, this, config_.uds_biz_path, ChannelType::UDS_BIZ);
  uds_ctrl_accept_thread_ = std::thread(&RpcServer::uds_accept_loop, this, config_.uds_ctrl_path, ChannelType::UDS_CTRL);

  Logger::instance().info_f("[RPC_SERVER] Started: TCP=%s, UDS-biz=%s, UDS-ctrl=%s",
                            config_.listen.c_str(), config_.uds_biz_path.c_str(), config_.uds_ctrl_path.c_str());
  return true;
}

void RpcServer::stop() {
  if (!running_.exchange(false)) return;

  Logger::instance().info("[RPC_SERVER] Stopping...");

  // Close server sockets to wake up accept threads
  if (tcp_fd_ >= 0) { close(tcp_fd_); tcp_fd_ = -1; }
  if (uds_biz_fd_ >= 0) { close(uds_biz_fd_); uds_biz_fd_ = -1; }
  if (uds_ctrl_fd_ >= 0) { close(uds_ctrl_fd_); uds_ctrl_fd_ = -1; }

  // Wait for accept threads
  if (tcp_accept_thread_.joinable()) tcp_accept_thread_.join();
  if (uds_biz_accept_thread_.joinable()) uds_biz_accept_thread_.join();
  if (uds_ctrl_accept_thread_.joinable()) uds_ctrl_accept_thread_.join();

  // Close all client connections
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& [fd, _] : active_clients_) {
      close(fd);
    }
    active_clients_.clear();
    connection_count_ = 0;
  }

  // Wait for client handler threads
  {
    std::lock_guard<std::mutex> lock(client_threads_mutex_);
    for (auto& t : client_threads_) {
      if (t.joinable()) t.join();
    }
    client_threads_.clear();
  }

  // Cleanup UDS socket files
  cleanup_uds(config_.uds_biz_path);
  cleanup_uds(config_.uds_ctrl_path);

  Logger::instance().info("[RPC_SERVER] Stopped");
}

void RpcServer::register_handler(const std::string& method, RpcMethodHandler handler) {
  std::lock_guard<std::mutex> lock(handlers_mutex_);
  handlers_[method] = std::move(handler);
}

void RpcServer::send_notification(int client_fd, const std::string& method, const std::string& params_json) {
  std::string notification = build_notification(method, params_json);
  notification += "\n";
  ssize_t sent = send(client_fd, notification.c_str(), notification.size(), MSG_NOSIGNAL);
  if (sent < 0) {
    Logger::instance().error_f("[RPC_SERVER] Notification send failed: %s", strerror(errno));
  }
}

bool RpcServer::is_method_allowed(ChannelType channel, const std::string& method) {
  switch (channel) {
    case ChannelType::TCP:
      return true;  // TCP allows all methods
    case ChannelType::UDS_BIZ:
      return in_list(kUdsBizMethods, method);
    case ChannelType::UDS_CTRL:
      return in_list(kUdsCtrlMethods, method);
  }
  return false;
}

void RpcServer::tcp_accept_loop() {
  while (running_.load()) {
    struct pollfd pfd;
    pfd.fd = tcp_fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 100);
    if (ret < 0) { if (errno == EINTR) continue; break; }
    if (ret == 0 || !(pfd.revents & POLLIN)) continue;

    int client_fd = accept(tcp_fd_, nullptr, nullptr);
    if (client_fd < 0) continue;

    if (connection_count_.load() >= config_.max_connections) {
      std::string err = build_error_response(-32000, "Server busy: max connections reached", -1);
      err += "\n";
      send(client_fd, err.c_str(), err.size(), MSG_NOSIGNAL);
      close(client_fd);
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      active_clients_[client_fd] = ChannelType::TCP;
    }
    connection_count_++;

    std::lock_guard<std::mutex> lock(client_threads_mutex_);
    client_threads_.emplace_back(&RpcServer::client_handler, this, client_fd, ChannelType::TCP);
  }
}

void RpcServer::uds_accept_loop(const std::string& socket_path, ChannelType channel) {
  int server_fd = (channel == ChannelType::UDS_BIZ) ? uds_biz_fd_ : uds_ctrl_fd_;

  while (running_.load()) {
    struct pollfd pfd;
    pfd.fd = server_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 100);
    if (ret < 0) { if (errno == EINTR) continue; break; }
    if (ret == 0 || !(pfd.revents & POLLIN)) continue;

    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) continue;

    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      active_clients_[client_fd] = channel;
    }
    connection_count_++;

    std::lock_guard<std::mutex> lock(client_threads_mutex_);
    client_threads_.emplace_back(&RpcServer::client_handler, this, client_fd, channel);
  }
}

void RpcServer::client_handler(int fd, ChannelType channel) {
  set_nonblocking(fd);

  std::string recv_buffer;
  char buf[4096];
  bool ws_mode = false;

  while (running_.load()) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 50);
    if (ret < 0) { if (errno == EINTR) continue; break; }
    if (ret == 0) continue;
    if (pfd.revents & (POLLHUP | POLLERR)) break;
    if (!(pfd.revents & POLLIN)) continue;

    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) break;
    buf[n] = '\0';
    recv_buffer += buf;

    // On first data, check for WebSocket upgrade (TCP only)
    if (!ws_mode && channel == ChannelType::TCP) {
      WsUpgradeResult ws_result = try_ws_upgrade(fd, recv_buffer);
      if (ws_result == WsUpgradeResult::UPGRADE_OK) {
        ws_mode = true;
        recv_buffer.clear();
        continue;
      } else if (ws_result == WsUpgradeResult::UPGRADE_FAIL) {
        break;  // Close on failed upgrade
      }
      // NOT_WS: proceed with normal JSON-RPC parsing
    }

    if (ws_mode) {
      // WebSocket frame mode
      uint8_t opcode = 0;
      std::string payload;
      while (ws_decode_frame(recv_buffer, payload, opcode)) {
        if (opcode == WS_OPCODE_CLOSE) {
          // Send close frame and exit
          std::string close_frame;
          close_frame += static_cast<char>(0x80 | WS_OPCODE_CLOSE);
          close_frame += static_cast<char>(0);
          send(fd, close_frame.c_str(), close_frame.size(), MSG_NOSIGNAL);
          recv_buffer.clear();
          goto cleanup;
        }
        if (opcode == WS_OPCODE_PING) {
          // Respond with pong
          std::string pong;
          pong += static_cast<char>(0x80 | WS_OPCODE_PONG);
          if (payload.size() <= 125) {
            pong += static_cast<char>(payload.size());
          }
          pong += payload;
          send(fd, pong.c_str(), pong.size(), MSG_NOSIGNAL);
          continue;
        }
        if (opcode == WS_OPCODE_TEXT) {
          // Remove trailing newline if present (JSON-RPC compatibility)
          if (!payload.empty() && payload.back() == '\n') payload.pop_back();
          std::string response = handle_request(payload, fd, channel);
          if (!response.empty()) {
            std::string frame = ws_encode_text_frame(response);
            send(fd, frame.c_str(), frame.size(), MSG_NOSIGNAL);
          }
        }
      }
    } else {
      // Process newline-delimited JSON messages
      size_t pos;
      while ((pos = recv_buffer.find('\n')) != std::string::npos) {
        std::string msg = recv_buffer.substr(0, pos);
        recv_buffer = recv_buffer.substr(pos + 1);
        if (msg.empty()) continue;

        std::string response = handle_request(msg, fd, channel);
        if (!response.empty()) {
          response += "\n";
          ssize_t sent = send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
          if (sent < 0) {
            Logger::instance().error_f("[RPC_SERVER] Send failed: %s", strerror(errno));
          }
        }
      }
    }
  }

cleanup:

  // Cleanup
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    active_clients_.erase(fd);
  }
  connection_count_--;
  close(fd);
}

std::string RpcServer::handle_request(const std::string& request_json, int client_fd, ChannelType channel) {
  std::string method = extract_json_method(request_json);
  int64_t id = extract_json_id(request_json);

  if (method.empty()) {
    return build_error_response(-32600, "Invalid Request: missing method", id);
  }

  // Check channel access control
  if (!is_method_allowed(channel, method)) {
    return build_error_response(-32601, "Method not allowed on this channel: " + method, id);
  }

  // Look up handler
  RpcMethodHandler handler;
  {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
      return build_error_response(-32601, "Method not found: " + method, id);
    }
    handler = it->second;
  }

  std::string params = extract_json_params(request_json);
  return handler(params, id, client_fd, channel);
}

std::string RpcServer::build_success_response(const std::string& result, int64_t id) {
  return "{\"jsonrpc\":\"2.0\",\"result\":" + result + ",\"id\":" + std::to_string(id) + "}";
}

std::string RpcServer::build_error_response(int code, const std::string& message, int64_t id) {
  return "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":" + std::to_string(code) +
         ",\"message\":\"" + json_escape(message) + "\"},\"id\":" + std::to_string(id) + "}";
}

std::string RpcServer::build_notification(const std::string& method, const std::string& params) {
  return "{\"jsonrpc\":\"2.0\",\"method\":\"" + method + "\",\"params\":" + params + "}";
}

void RpcServer::cleanup_uds(const std::string& path) {
  if (!path.empty()) {
    unlink(path.c_str());
  }
}

}  // namespace daemon
}  // namespace agentflow
