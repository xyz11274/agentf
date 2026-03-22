// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// UDSServer implementation - Unix Domain Socket server for multi-language DataBus access

#include "runtime/databus/uds_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>

#include "utils/logger.h"

// JSON parsing helper (minimal implementation without external dependencies)
// For production, this should use RapidJSON when available
namespace {

using agentflow::DataVariant;

// Simple JSON value extraction (handles basic types)
std::string extract_json_string(const std::string& json, const std::string& key) {
  // Look for "key": "value" or "key": value pattern
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) {
    return "";
  }
  pos = json.find(':', pos);
  if (pos == std::string::npos) {
    return "";
  }
  // Skip whitespace
  while (pos < json.size() && std::isspace(json[pos])) {
    pos++;
  }
  if (pos >= json.size()) {
    return "";
  }
  // Check if string value
  if (json[pos] == '"') {
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) {
      return "";
    }
    return json.substr(pos, end - pos);
  }
  // Check if number or other value
  size_t end = pos;
  while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') {
    end++;
  }
  std::string value = json.substr(pos, end - pos);
  // Trim trailing whitespace
  while (!value.empty() && std::isspace(value.back())) {
    value.pop_back();
  }
  return value;
}

// Extract nested JSON object
std::string extract_json_object(const std::string& json, const std::string& key) {
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) {
    return "";
  }
  pos = json.find(':', pos);
  if (pos == std::string::npos) {
    return "";
  }
  // Skip whitespace
  while (pos < json.size() && std::isspace(json[pos])) {
    pos++;
  }
  if (pos >= json.size() || json[pos] != '{') {
    return "";
  }
  // Find matching brace
  int depth = 0;
  size_t start = pos;
  for (; pos < json.size(); pos++) {
    if (json[pos] == '{') depth++;
    else if (json[pos] == '}') {
      depth--;
      if (depth == 0) {
        return json.substr(start, pos - start + 1);
      }
    }
  }
  return "";
}

// Extract nested JSON array
std::string extract_json_array(const std::string& json, const std::string& key) {
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) {
    return "";
  }
  pos = json.find(':', pos);
  if (pos == std::string::npos) {
    return "";
  }
  // Skip whitespace
  while (pos < json.size() && std::isspace(json[pos])) {
    pos++;
  }
  if (pos >= json.size() || json[pos] != '[') {
    return "";
  }
  // Find matching bracket
  int depth = 0;
  size_t start = pos;
  for (; pos < json.size(); pos++) {
    if (json[pos] == '[') depth++;
    else if (json[pos] == ']') {
      depth--;
      if (depth == 0) {
        return json.substr(start, pos - start + 1);
      }
    }
  }
  return "";
}

// Extract id from JSON-RPC request
int64_t extract_json_id(const std::string& json) {
  std::string id_str = extract_json_string(json, "id");
  if (id_str.empty()) {
    return -1;
  }
  try {
    return std::stoll(id_str);
  } catch (...) {
    return -1;
  }
}

// Extract method from JSON-RPC request
std::string extract_json_method(const std::string& json) {
  return extract_json_string(json, "method");
}

// Extract params from JSON-RPC request
std::string extract_json_params(const std::string& json) {
  // Try object first
  std::string params = extract_json_object(json, "params");
  if (!params.empty()) {
    return params;
  }
  // Try array
  return extract_json_array(json, "params");
}

// Escape string for JSON
std::string json_escape(const std::string& s) {
  std::string result;
  result.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
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

// Convert DataVariant to JSON string
std::string variant_to_json(const DataVariant& var) {
  if (std::holds_alternative<std::string>(var)) {
    return "\"" + json_escape(std::get<std::string>(var)) + "\"";
  } else if (std::holds_alternative<int64_t>(var)) {
    return std::to_string(std::get<int64_t>(var));
  } else if (std::holds_alternative<double>(var)) {
    return std::to_string(std::get<double>(var));
  } else if (std::holds_alternative<bool>(var)) {
    return std::get<bool>(var) ? "true" : "false";
  }
  return "null";
}

// Parse JSON value to DataVariant
DataVariant json_to_variant(const std::string& json) {
  // Trim whitespace
  std::string trimmed = json;
  size_t start = trimmed.find_first_not_of(" \t\n\r");
  size_t end = trimmed.find_last_not_of(" \t\n\r");
  if (start == std::string::npos || end == std::string::npos) {
    return DataVariant{};
  }
  trimmed = trimmed.substr(start, end - start + 1);

  if (trimmed.empty() || trimmed == "null") {
    return DataVariant{};
  }
  if (trimmed == "true") {
    return DataVariant{true};
  }
  if (trimmed == "false") {
    return DataVariant{false};
  }
  if (trimmed.front() == '"') {
    // String value
    if (trimmed.size() >= 2 && trimmed.back() == '"') {
      std::string value = trimmed.substr(1, trimmed.size() - 2);
      // Unescape basic sequences
      std::string unescaped;
      for (size_t i = 0; i < value.size(); i++) {
        if (value[i] == '\\' && i + 1 < value.size()) {
          switch (value[i + 1]) {
            case '"': unescaped += '"'; i++; break;
            case '\\': unescaped += '\\'; i++; break;
            case 'n': unescaped += '\n'; i++; break;
            case 'r': unescaped += '\r'; i++; break;
            case 't': unescaped += '\t'; i++; break;
            default: unescaped += value[i]; break;
          }
        } else {
          unescaped += value[i];
        }
      }
      return DataVariant{unescaped};
    }
    return DataVariant{};
  }
  // Try number
  try {
    if (trimmed.find('.') != std::string::npos) {
      return DataVariant{std::stod(trimmed)};
    }
    return DataVariant{static_cast<int64_t>(std::stoll(trimmed))};
  } catch (...) {
    return DataVariant{};
  }
}

// Extract value from params JSON
DataVariant extract_json_value(const std::string& params) {
  std::string value_str = extract_json_string(params, "value");
  if (value_str.empty()) {
    // Try to get the raw value (could be number/bool)
    return json_to_variant(extract_json_string(params, "value"));
  }
  return DataVariant{value_str};
}

}  // anonymous namespace

namespace agentflow {
namespace runtime {

UDSServer::UDSServer(MMDataBus* databus, const std::string& socket_path)
    : UDSServer(databus, socket_path, Config{}) {
}

UDSServer::UDSServer(MMDataBus* databus, const std::string& socket_path, const Config& config)
    : databus_(databus), socket_path_(socket_path), config_(config) {
  task_queue_.max_size = config_.max_queue_size;
}

UDSServer::~UDSServer() {
  stop();
}

bool UDSServer::start() {
  if (running_.load()) {
    return true;
  }

  if (!databus_) {
    Logger::instance().error("[UDS_SERVER] Cannot start: databus is null");
    return false;
  }

  // Clean up any existing socket file
  cleanup_socket();

  // Create socket
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    Logger::instance().error_f("[UDS_SERVER] Failed to create socket: %s", strerror(errno));
    return false;
  }

  // Set non-blocking mode
  int flags = fcntl(server_fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    Logger::instance().error_f("[UDS_SERVER] Failed to set non-blocking: %s", strerror(errno));
    close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  // Bind to socket path
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    Logger::instance().error_f("[UDS_SERVER] Failed to bind socket %s: %s",
                              socket_path_.c_str(), strerror(errno));
    close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  // Listen for connections
  if (listen(server_fd_, 10) < 0) {
    Logger::instance().error_f("[UDS_SERVER] Failed to listen: %s", strerror(errno));
    close(server_fd_);
    server_fd_ = -1;
    cleanup_socket();
    return false;
  }

  running_ = true;

  // Start worker threads for client handling
  start_worker_threads();

  // Start accept thread
  accept_thread_ = std::thread(&UDSServer::server_loop, this);

  Logger::instance().info_f("[UDS_SERVER] Started on %s with %zu worker threads",
                            socket_path_.c_str(), config_.thread_pool_size);
  return true;
}

void UDSServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  Logger::instance().info("[UDS_SERVER] Stopping...");

  // 1. Stop task queue to unblock worker threads
  task_queue_.stop();

  // 2. Close server socket to wake up accept()
  if (server_fd_ >= 0) {
    close(server_fd_);
    server_fd_ = -1;
  }

  // 3. Wait for accept thread to finish
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  // 4. Wait for worker threads to finish
  for (auto& thread : worker_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  worker_threads_.clear();

  // 5. Close all client connections
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& [fd, _] : active_clients_) {
      close(fd);
    }
    active_clients_.clear();
  }

  // 6. Unsubscribe all subscriptions
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.clear();
    key_to_subscriptions_.clear();
  }

  // 7. Clean up socket file
  cleanup_socket();

  Logger::instance().info("[UDS_SERVER] Stopped");
}

void UDSServer::start_worker_threads() {
  for (size_t i = 0; i < config_.thread_pool_size; ++i) {
    worker_threads_.emplace_back(&UDSServer::worker_thread_func, this, i);
  }
}

void UDSServer::worker_thread_func(size_t thread_id) {
  Logger::instance().debug_f("[UDS_SERVER] Worker %zu started", thread_id);

  while (running_.load()) {
    int client_fd;
    if (task_queue_.pop(client_fd, 100)) {  // 100ms timeout
      // Record active client
      {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        active_clients_[client_fd] = true;
      }

      Logger::instance().debug_f("[UDS_SERVER] Worker %zu handling client fd=%d", thread_id, client_fd);

      handle_client(client_fd);

      // Client cleanup is done in handle_client
    }
  }

  Logger::instance().debug_f("[UDS_SERVER] Worker %zu exiting", thread_id);
}

void UDSServer::server_loop() {
  while (running_.load()) {
    // Use poll to wait for connections with timeout
    struct pollfd pfd;
    pfd.fd = server_fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 100);  // 100ms timeout
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ret == 0) continue;  // timeout, check running_

    if (!(pfd.revents & POLLIN)) continue;

    // Accept new connection
    int client_fd = accept(server_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        Logger::instance().error_f("[UDS_SERVER] Accept failed: %s", strerror(errno));
      }
      continue;
    }

    // Push to task queue for worker thread handling
    if (!task_queue_.push(client_fd)) {
      // Queue full, send error and close
      send_error_and_close(client_fd, -32000, "Server busy: task queue full");
      Logger::instance().warn("[UDS_SERVER] Task queue full, rejected connection");
      continue;
    }

    Logger::instance().debug_f("[UDS_SERVER] Client queued: fd=%d, queue_size=%zu",
                               client_fd, task_queue_.size());
  }
}

void UDSServer::send_error_and_close(int client_fd, int code, const std::string& message) {
  std::string error_response = build_error_response(code, message, -1);
  error_response += "\n";
  send(client_fd, error_response.c_str(), error_response.size(), MSG_NOSIGNAL);
  usleep(1000);  // Brief delay to ensure data is sent
  close(client_fd);
}

void UDSServer::handle_client(int client_fd) {
  // Set non-blocking
  int flags = fcntl(client_fd, F_GETFL, 0);
  fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

  std::string recv_buffer;
  char buf[4096];

  while (running_.load()) {
    // Poll for data
    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 50);  // 50ms timeout
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ret == 0) continue;  // timeout

    if (pfd.revents & (POLLHUP | POLLERR)) {
      break;  // Client disconnected
    }

    if (!(pfd.revents & POLLIN)) continue;

    // Receive data
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
      break;  // Connection closed or error
    }
    buf[n] = '\0';
    recv_buffer += buf;

    // Process complete JSON messages (newline-delimited)
    size_t pos;
    while ((pos = recv_buffer.find('\n')) != std::string::npos) {
      std::string msg = recv_buffer.substr(0, pos);
      recv_buffer = recv_buffer.substr(pos + 1);

      if (msg.empty()) continue;

      // Handle request
      std::string response = handle_request(msg);

      // Send response (if not a notification-only request)
      if (!response.empty()) {
        response += "\n";
        ssize_t sent = send(client_fd, response.c_str(), response.size(), 0);
        if (sent < 0) {
          Logger::instance().error_f("[UDS_SERVER] Send failed: %s", strerror(errno));
        }
      }
    }
  }

  // Clean up client
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    active_clients_.erase(client_fd);
  }

  // Remove subscriptions for this client
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    std::vector<uint64_t> to_remove;
    for (auto& [id, info] : subscriptions_) {
      if (info.client_fd == client_fd) {
        to_remove.push_back(id);
      }
    }
    for (uint64_t id : to_remove) {
      auto it = subscriptions_.find(id);
      if (it != subscriptions_.end()) {
        // Remove from key_to_subscriptions
        auto& subs = key_to_subscriptions_[it->second.full_key];
        subs.erase(std::remove(subs.begin(), subs.end(), id), subs.end());
        if (subs.empty()) {
          key_to_subscriptions_.erase(it->second.full_key);
        }
        subscriptions_.erase(it);
      }
    }
  }

  close(client_fd);
  Logger::instance().debug_f("[UDS_SERVER] Client disconnected: fd=%d", client_fd);
}

std::string UDSServer::handle_request(const std::string& request_json) {
  std::string method = extract_json_method(request_json);
  int64_t id = extract_json_id(request_json);
  std::string params = extract_json_params(request_json);

  if (method.empty()) {
    return build_error_response(-32600, "Invalid Request: missing method", id);
  }

  Logger::instance().debug_f("[UDS_SERVER] Request: method=%s id=%lld", method.c_str(), id);

  if (method == "put") {
    return handle_put(method, params, id);
  } else if (method == "get") {
    return handle_get(method, params, id);
  } else if (method == "remove") {
    return handle_remove(method, params, id);
  } else if (method == "contains") {
    return handle_contains(method, params, id);
  } else if (method == "subscribe") {
    // Note: subscribe needs client_fd, but for now we handle it differently
    // This is a limitation of the current design
    return build_error_response(-32601, "Method not supported in this context", id);
  } else if (method == "unsubscribe") {
    return handle_unsubscribe(params, id);
  } else if (method == "publish") {
    return handle_publish(method, params, id);
  } else if (method == "list_keys") {
    return handle_list_keys(params, id);
  }

  return build_error_response(-32601, "Method not found: " + method, id);
}

std::string UDSServer::build_key(const std::string& session_id,
                                 const std::string& namespace_str,
                                 const std::string& key) const {
  return "/session/" + session_id + "/" + namespace_str + "/" + key;
}

std::string UDSServer::handle_put(const std::string& method, const std::string& params, int64_t id) {
  (void)method;
  std::string session_id = extract_json_string(params, "session_id");
  std::string namespace_str = extract_json_string(params, "namespace");
  std::string key = extract_json_string(params, "key");
  std::string value_str = extract_json_string(params, "value");

  if (session_id.empty() || namespace_str.empty() || key.empty()) {
    return build_error_response(-32602, "Invalid params: missing session_id, namespace, or key", id);
  }

  std::string full_key = build_key(session_id, namespace_str, key);
  DataVariant value(value_str);

  bool success = databus_->put(full_key, value);

  if (success) {
    // Notify subscribers
    notify_subscribers(full_key, value);
    return build_success_response("{\"success\":true}", id);
  }
  return build_error_response(-1, "Failed to put value", id);
}

std::string UDSServer::handle_get(const std::string& method, const std::string& params, int64_t id) {
  (void)method;
  std::string session_id = extract_json_string(params, "session_id");
  std::string namespace_str = extract_json_string(params, "namespace");
  std::string key = extract_json_string(params, "key");

  if (session_id.empty() || namespace_str.empty() || key.empty()) {
    return build_error_response(-32602, "Invalid params: missing session_id, namespace, or key", id);
  }

  std::string full_key = build_key(session_id, namespace_str, key);
  DataVariant value;

  bool exists = databus_->get(full_key, value);

  std::string result;
  if (exists) {
    result = "{\"exists\":true,\"value\":" + variant_to_json(value) + "}";
  } else {
    result = "{\"exists\":false,\"value\":null}";
  }
  return build_success_response(result, id);
}

std::string UDSServer::handle_remove(const std::string& method, const std::string& params, int64_t id) {
  (void)method;
  std::string session_id = extract_json_string(params, "session_id");
  std::string namespace_str = extract_json_string(params, "namespace");
  std::string key = extract_json_string(params, "key");

  if (session_id.empty() || namespace_str.empty() || key.empty()) {
    return build_error_response(-32602, "Invalid params: missing session_id, namespace, or key", id);
  }

  std::string full_key = build_key(session_id, namespace_str, key);
  bool success = databus_->remove(full_key);

  return build_success_response(success ? "{\"success\":true}" : "{\"success\":false}", id);
}

std::string UDSServer::handle_contains(const std::string& method, const std::string& params, int64_t id) {
  (void)method;
  std::string session_id = extract_json_string(params, "session_id");
  std::string namespace_str = extract_json_string(params, "namespace");
  std::string key = extract_json_string(params, "key");

  if (session_id.empty() || namespace_str.empty() || key.empty()) {
    return build_error_response(-32602, "Invalid params: missing session_id, namespace, or key", id);
  }

  std::string full_key = build_key(session_id, namespace_str, key);
  bool exists = databus_->contains(full_key);

  return build_success_response(exists ? "{\"exists\":true}" : "{\"exists\":false}", id);
}

std::string UDSServer::handle_subscribe(int client_fd, const std::string& params, int64_t id) {
  std::string session_id = extract_json_string(params, "session_id");
  std::string namespace_str = extract_json_string(params, "namespace");
  std::string key = extract_json_string(params, "key");

  if (session_id.empty() || namespace_str.empty() || key.empty()) {
    return build_error_response(-32602, "Invalid params: missing session_id, namespace, or key", id);
  }

  std::string full_key = build_key(session_id, namespace_str, key);

  std::lock_guard<std::mutex> lock(subscription_mutex_);

  uint64_t sub_id = next_subscription_id_++;

  SubscriptionInfo info;
  info.session_id = session_id;
  info.namespace_str = namespace_str;
  info.key = key;
  info.full_key = full_key;
  info.client_fd = client_fd;
  info.subscription_id = sub_id;

  subscriptions_[sub_id] = info;
  key_to_subscriptions_[full_key].push_back(sub_id);

  // Subscribe to the DataBus
  databus_->subscribe(full_key, [this, sub_id](const std::string& k, const DataVariant& v) {
    // Find subscription and send notification
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    auto it = subscriptions_.find(sub_id);
    if (it != subscriptions_.end()) {
      std::string notification = build_notification("data_changed",
          "{\"subscription_id\":" + std::to_string(sub_id) +
          ",\"key\":\"" + json_escape(k) + "\"" +
          ",\"value\":" + variant_to_json(v) + "}");

      // Send to client
      int fd = it->second.client_fd;
      notification += "\n";
      ssize_t sent = send(fd, notification.c_str(), notification.size(), MSG_NOSIGNAL);
      if (sent < 0) {
        Logger::instance().error_f("[UDS_SERVER] Notification send failed: %s", strerror(errno));
      }
    }
  });

  return build_success_response("{\"subscription_id\":" + std::to_string(sub_id) + "}", id);
}

std::string UDSServer::handle_unsubscribe(const std::string& params, int64_t id) {
  std::string sub_id_str = extract_json_string(params, "subscription_id");
  if (sub_id_str.empty()) {
    return build_error_response(-32602, "Invalid params: missing subscription_id", id);
  }

  uint64_t sub_id = std::stoull(sub_id_str);

  std::lock_guard<std::mutex> lock(subscription_mutex_);

  auto it = subscriptions_.find(sub_id);
  if (it == subscriptions_.end()) {
    return build_error_response(-2, "Subscription not found", id);
  }

  // Unsubscribe from DataBus
  databus_->unsubscribe(it->second.full_key);

  // Remove from key_to_subscriptions
  auto& subs = key_to_subscriptions_[it->second.full_key];
  subs.erase(std::remove(subs.begin(), subs.end(), sub_id), subs.end());
  if (subs.empty()) {
    key_to_subscriptions_.erase(it->second.full_key);
  }

  subscriptions_.erase(it);

  return build_success_response("{\"success\":true}", id);
}

std::string UDSServer::handle_publish(const std::string& method, const std::string& params, int64_t id) {
  (void)method;
  std::string session_id = extract_json_string(params, "session_id");
  std::string namespace_str = extract_json_string(params, "namespace");
  std::string topic = extract_json_string(params, "topic");
  std::string value_str = extract_json_string(params, "value");

  if (session_id.empty() || namespace_str.empty() || topic.empty()) {
    return build_error_response(-32602, "Invalid params: missing session_id, namespace, or topic", id);
  }

  // Publish by putting to topic key
  std::string full_topic = build_key(session_id, namespace_str, topic);
  DataVariant value(value_str);

  bool success = databus_->put(full_topic, value);

  if (success) {
    // Notify subscribers
    notify_subscribers(full_topic, value);
    return build_success_response("{\"success\":true}", id);
  }
  return build_error_response(-1, "Failed to publish", id);
}

std::string UDSServer::handle_list_keys(const std::string& params, int64_t id) {
  std::string session_id = extract_json_string(params, "session_id");
  std::string namespace_str = extract_json_string(params, "namespace");
  std::string prefix = extract_json_string(params, "prefix");

  if (session_id.empty() || namespace_str.empty()) {
    return build_error_response(-32602, "Invalid params: missing session_id or namespace", id);
  }

  // For now, return empty list (full implementation would iterate keys)
  // The DataBus interface doesn't have a list_keys method yet
  return build_success_response("{\"keys\":[]}", id);
}

std::string UDSServer::build_success_response(const std::string& result, int64_t id) const {
  return "{\"jsonrpc\":\"2.0\",\"result\":" + result + ",\"id\":" + std::to_string(id) + "}";
}

std::string UDSServer::build_error_response(int code, const std::string& message, int64_t id) const {
  return "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":" + std::to_string(code) +
         ",\"message\":\"" + json_escape(message) + "\"},\"id\":" + std::to_string(id) + "}";
}

std::string UDSServer::build_notification(const std::string& method, const std::string& params) const {
  return "{\"jsonrpc\":\"2.0\",\"method\":\"" + method + "\",\"params\":" + params + "}";
}

void UDSServer::notify_subscribers(const std::string& key, const DataVariant& value) {
  std::lock_guard<std::mutex> lock(subscription_mutex_);

  auto it = key_to_subscriptions_.find(key);
  if (it == key_to_subscriptions_.end()) {
    return;
  }

  for (uint64_t sub_id : it->second) {
    auto sub_it = subscriptions_.find(sub_id);
    if (sub_it != subscriptions_.end()) {
      std::string notification = build_notification("data_changed",
          "{\"subscription_id\":" + std::to_string(sub_id) +
          ",\"key\":\"" + json_escape(key) + "\"" +
          ",\"value\":" + variant_to_json(value) + "}");

      int fd = sub_it->second.client_fd;
      notification += "\n";
      send(fd, notification.c_str(), notification.size(), MSG_NOSIGNAL);
    }
  }
}

void UDSServer::cleanup_socket() {
  if (!socket_path_.empty()) {
    unlink(socket_path_.c_str());
  }
}

// TaskQueue implementation
bool UDSServer::TaskQueue::push(int client_fd) {
  std::unique_lock<std::mutex> lock(mutex);
  if (queue.size() >= max_size) {
    return false;
  }
  queue.push(client_fd);
  cv.notify_one();
  return true;
}

bool UDSServer::TaskQueue::pop(int& out_client_fd, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex);
  if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                   [this] { return !queue.empty() || stopped.load(); })) {
    return false;  // timeout
  }
  if (stopped.load() && queue.empty()) {
    return false;
  }
  if (queue.empty()) {
    return false;
  }
  out_client_fd = queue.front();
  queue.pop();
  return true;
}

void UDSServer::TaskQueue::stop() {
  stopped.store(true);
  cv.notify_all();
}

size_t UDSServer::TaskQueue::size() const {
  std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex));
  return queue.size();
}

}  // namespace runtime
}  // namespace agentflow