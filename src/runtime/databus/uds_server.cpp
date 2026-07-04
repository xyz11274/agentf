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

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

// JSON parsing — uses RapidJSON (header-only, vendored at vendor/rapidjson/).
// Replaced the hand-written parser to eliminate edge-case bugs in manual
// brace matching and string scanning.
namespace {

using agentflow::DataVariant;

// ── Helpers ──────────────────────────────────────────────────────────

static std::string rj_stringify(const rapidjson::Value& val) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  val.Accept(w);
  return buf.GetString();
}

// ── Extraction ───────────────────────────────────────────────────────

std::string extract_json_string(const std::string& json, const std::string& key) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember(key.c_str())) return "";
  const auto& v = d[key.c_str()];
  if (v.IsString()) return v.GetString();
  return rj_stringify(v);
}

std::string extract_json_object(const std::string& json, const std::string& key) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember(key.c_str()) || !d[key.c_str()].IsObject()) return "";
  return rj_stringify(d[key.c_str()]);
}

std::string extract_json_array(const std::string& json, const std::string& key) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember(key.c_str()) || !d[key.c_str()].IsArray()) return "";
  return rj_stringify(d[key.c_str()]);
}

int64_t extract_json_id(const std::string& json) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember("id")) return -1;
  const auto& v = d["id"];
  if (v.IsInt64())  return v.GetInt64();
  if (v.IsInt())    return v.GetInt();
  if (v.IsUint64()) return static_cast<int64_t>(v.GetUint64());
  return -1;
}

std::string extract_json_method(const std::string& json) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember("method") || !d["method"].IsString()) return "";
  return d["method"].GetString();
}

std::string extract_json_params(const std::string& json) {
  return extract_json_object(json, "params");
}

// ── DataVariant conversion ───────────────────────────────────────────

std::string json_escape(const std::string& s) {
  std::string r;
  r.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '"':  r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\b': r += "\\b";  break;
      case '\f': r += "\\f";  break;
      case '\n': r += "\\n";  break;
      case '\r': r += "\\r";  break;
      case '\t': r += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          r += buf;
        } else { r += c; }
    }
  }
  return r;
}

std::string variant_to_json(const DataVariant& v) {
  if (auto* s = std::get_if<std::string>(&v)) {
    size_t p = s->find_first_not_of(" \t\n\r");
    if (p != std::string::npos && ((*s)[p] == '{' || (*s)[p] == '[')) return *s;
    return "\"" + json_escape(*s) + "\"";
  }
  if (auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
  if (auto* d = std::get_if<double>(&v))  return std::to_string(*d);
  if (auto* b = std::get_if<bool>(&v))    return *b ? "true" : "false";
  return "null";
}

DataVariant json_to_variant(const std::string& s) {
  rapidjson::Document d;
  d.Parse(s.c_str());
  if (d.HasParseError() || d.IsNull()) return DataVariant{};
  if (d.IsString()) return DataVariant{d.GetString()};
  if (d.IsTrue())   return DataVariant{true};
  if (d.IsFalse())  return DataVariant{false};
  if (d.IsInt64())  return DataVariant{d.GetInt64()};
  if (d.IsDouble()) return DataVariant{d.GetDouble()};
  if (d.IsInt())    return DataVariant{static_cast<int64_t>(d.GetInt())};
  if (d.IsUint64()) return DataVariant{static_cast<int64_t>(d.GetUint64())};
  return DataVariant{};
}

DataVariant extract_json_value(const std::string& params) {
  std::string s = extract_json_string(params, "value");
  return s.empty() ? DataVariant{} : DataVariant{s};
}

}  // anonymous namespace

namespace agentflow {
namespace runtime {

UDSServer::UDSServer(MMDataBus* databus, const std::string& socket_path)
    : UDSServer(databus, socket_path, Config{}) {
}

UDSServer::UDSServer(MMDataBus* databus, const std::string& socket_path, AsyncCallback async_callback)
    : UDSServer(databus, socket_path, Config{}) {
  async_callback_ = std::move(async_callback);
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
  } else if (method == "async_callback") {
    return handle_async_callback(params, id);
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

  // DEBUG: log extracted params to diagnose -32602 / extraction issues
  Logger::instance().info_f(
      "[UDS_PUT_DEBUG] session_id=%s namespace=%s key=%s value_len=%zu "
      "sid_empty=%d ns_empty=%d key_empty=%d",
      session_id.c_str(), namespace_str.c_str(), key.c_str(), value_str.size(),
      session_id.empty(), namespace_str.empty(), key.empty());

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

std::string UDSServer::handle_async_callback(const std::string& params, int64_t id) {
  // Check if async callback is configured
  if (!async_callback_) {
    return build_error_response(-1, "Async callback not configured", id);
  }

  // Extract parameters
  std::string session_id = extract_json_string(params, "session_id");
  std::string dialogue_id = extract_json_string(params, "dialogue_id");
  std::string task_id = extract_json_string(params, "task_id");
  std::string node_id = extract_json_string(params, "node_id");
  std::string call_key = extract_json_string(params, "call_key");
  std::string content = extract_json_string(params, "content");

  // Validate required parameters
  if (session_id.empty() || task_id.empty() || node_id.empty() || call_key.empty()) {
    return build_error_response(-32602,
        "Invalid params: missing session_id, task_id, node_id, or call_key", id);
  }

  Logger::instance().info_f("[UDS_SERVER] async_callback: session=%s, task=%s, node=%s, key=%s",
                            session_id.c_str(), task_id.c_str(), node_id.c_str(), call_key.c_str());

  // Call the registered callback
  bool success = async_callback_(session_id, dialogue_id, task_id, node_id, call_key, content);

  if (success) {
    return build_success_response("{\"success\":true,\"queued\":true}", id);
  }
  return build_success_response("{\"success\":false}", id);
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