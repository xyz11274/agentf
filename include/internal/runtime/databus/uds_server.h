// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// UDSServer - Unix Domain Socket server for multi-language DataBus access

#ifndef AGENTFLOW_RUNTIME_UDS_SERVER_H
#define AGENTFLOW_RUNTIME_UDS_SERVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "runtime/databus/databus.h"

namespace agentflow {
namespace runtime {

/**
 * @brief UDSServer provides Unix Domain Socket access to MMDataBus.
 *
 * This server implements a JSON-RPC 2.0 protocol for external processes
 * (Python, Go, Shell, etc.) to access the DataBus in real-time with
 * session/dialogue isolation.
 *
 * The server uses a thread pool pattern for concurrent client handling:
 * - Accept thread: Accepts new connections and queues client FDs
 * - Worker threads: Process client requests from the task queue
 *
 * Protocol:
 * - Request:  {"jsonrpc": "2.0", "method": "...", "params": {...}, "id": N}
 * - Response: {"jsonrpc": "2.0", "result": {...}, "id": N}
 * - Error:    {"jsonrpc": "2.0", "error": {"code": X, "message": "..."}, "id": N}
 * - Notification: {"jsonrpc": "2.0", "method": "...", "params": {...}}
 *
 * Methods:
 * - put: {session_id, namespace, key, value} -> {success}
 * - get: {session_id, namespace, key} -> {value, exists}
 * - remove: {session_id, namespace, key} -> {success}
 * - contains: {session_id, namespace, key} -> {exists}
 * - subscribe: {session_id, namespace, key} -> {subscription_id}
 * - unsubscribe: {subscription_id} -> {success}
 * - publish: {session_id, namespace, topic, value} -> {success}
 * - list_keys: {session_id, namespace, prefix?} -> {keys}
 *
 * Namespace values: "sys", "ctx", "llm", "tool", "user"
 *
 * Key structure: /session/{session_id}/{namespace}/{key}
 *
 * Usage:
 * \code
 * MMDataBus databus;
 * UDSServer::Config config;
 * config.thread_pool_size = 10;
 * config.max_queue_size = 100;
 * UDSServer server(&databus, "/tmp/agentflow_session_123.sock", config);
 * if (server.start()) {
 *     // Server is running, clients can connect
 *     // Pass AF_DATABUS_UDS_FILE env var to external processes
 * }
 * // Later...
 * server.stop();
 * \endcode
 */
class UDSServer {
public:
  /// Callback type for data change notifications to clients
  using ClientNotificationCallback = std::function<void(const std::string& json)>;

  /// Server configuration
  struct Config {
    size_t thread_pool_size = 10;  ///< Number of worker threads for client handling
    size_t max_queue_size = 100;   ///< Maximum number of queued client connections
  };

  /// Construct a UDS server with the given DataBus and socket path.
  /// The databus pointer must remain valid for the lifetime of this server.
  /// Uses default configuration (10 worker threads, 100 max queue size).
  UDSServer(MMDataBus* databus, const std::string& socket_path);

  /// Construct a UDS server with custom configuration.
  UDSServer(MMDataBus* databus, const std::string& socket_path, const Config& config);

  /// Destructor stops the server and cleans up the socket file.
  ~UDSServer();

  // Non-copyable
  UDSServer(const UDSServer&) = delete;
  UDSServer& operator=(const UDSServer&) = delete;

  /// Start the server thread.
  /// Returns true if the server started successfully.
  bool start();

  /// Stop the server thread and clean up.
  void stop();

  /// Check if the server is running.
  bool is_running() const { return running_.load(); }

  /// Get the socket path.
  const std::string& socket_path() const { return socket_path_; }

private:
  /// Task queue for client connections
  struct TaskQueue {
    std::queue<int> queue;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stopped{false};
    size_t max_size = 100;

    bool push(int client_fd);
    bool pop(int& out_client_fd, int timeout_ms);
    void stop();
    size_t size() const;
  };

  /// Main server loop (runs in accept thread)
  void server_loop();

  /// Worker thread function
  void worker_thread_func(size_t thread_id);

  /// Start worker threads
  void start_worker_threads();

  /// Handle a connected client
  void handle_client(int client_fd);

  /// Send error response and close connection
  void send_error_and_close(int client_fd, int code, const std::string& message);

  /// Process a single JSON-RPC request and return the response
  std::string handle_request(const std::string& request_json);

  /// Build full key from session_id, namespace, and key
  /// Returns /session/{session_id}/{namespace}/{key}
  std::string build_key(const std::string& session_id,
                        const std::string& namespace_str,
                        const std::string& key) const;

  /// JSON-RPC method handlers
  std::string handle_put(const std::string& method, const std::string& params, int64_t id);
  std::string handle_get(const std::string& method, const std::string& params, int64_t id);
  std::string handle_remove(const std::string& method, const std::string& params, int64_t id);
  std::string handle_contains(const std::string& method, const std::string& params, int64_t id);
  std::string handle_subscribe(int client_fd, const std::string& params, int64_t id);
  std::string handle_unsubscribe(const std::string& params, int64_t id);
  std::string handle_publish(const std::string& method, const std::string& params, int64_t id);
  std::string handle_list_keys(const std::string& params, int64_t id);

  /// Build JSON-RPC success response
  std::string build_success_response(const std::string& result, int64_t id) const;

  /// Build JSON-RPC error response
  std::string build_error_response(int code, const std::string& message, int64_t id) const;

  /// Build JSON-RPC notification
  std::string build_notification(const std::string& method, const std::string& params) const;

  /// Notify all subscribers for a key
  void notify_subscribers(const std::string& key, const DataVariant& value);

  /// Clean up socket file
  void cleanup_socket();

  /// Subscription information
  struct SubscriptionInfo {
    std::string session_id;
    std::string namespace_str;
    std::string key;
    std::string full_key;
    int client_fd;
    uint64_t subscription_id;
  };

  MMDataBus* databus_;
  std::string socket_path_;
  Config config_;
  std::atomic<bool> running_{false};

  /// Accept thread
  std::thread accept_thread_;

  /// Worker threads
  std::vector<std::thread> worker_threads_;

  /// Task queue for client connections
  TaskQueue task_queue_;

  int server_fd_{-1};

  /// Subscription management
  mutable std::mutex subscription_mutex_;
  uint64_t next_subscription_id_{1};
  std::unordered_map<uint64_t, SubscriptionInfo> subscriptions_;
  std::unordered_map<std::string, std::vector<uint64_t>> key_to_subscriptions_;

  /// Client file descriptors for notifications (protected by mutex)
  mutable std::mutex clients_mutex_;
  std::map<int, bool> active_clients_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_UDS_SERVER_H