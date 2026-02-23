#ifndef AGENTFLOW_FRAMEWORK_EDGE_H
#define AGENTFLOW_FRAMEWORK_EDGE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent/common.h"
#include "utils/thread_pool.h"

namespace agentflow {
namespace framework {

/**
 * Edge defines the dependency relationship between nodes
 * Supports data passing and control flow events.
 * Workflow process control is edge-centric, including branch/loop/fork/join/async/goto.
 */
class Edge {
public:
  enum class ControlType {
    NONE  = 0,
    IF    = 1,
    LOOP  = 2,
    FORK  = 3,
    JOIN  = 4,
    ASYNC = 5,
    GOTO  = 6,
  };

  enum class DataTransferMode {
    SHARED_MEMORY   = 0,
    MESSAGE_QUEUE   = 1,
    REMOTE_TRANSFER = 2,
  };

  struct Config {
    std::string id;
    std::string source_node;
    std::string target_node;

    // Dependency relationships
    std::vector<std::string> source_outputs;
    std::vector<std::string> target_inputs;

    // Data transfer configuration
    DataTransferMode         transfer_mode = DataTransferMode::SHARED_MEMORY;
    std::vector<std::string> data_formats;  // text, binary, audio, video, etc.

    // Control flow events
    std::vector<std::string> supported_events;  // start, end, fail, data_ready, etc.

    // Conditional execution support (as string, parsed by Workflow)
    std::string condition_expression;

    // Edge priority (for multiple outgoing edges)
    int priority = 0;

    struct ControlConfig {
      ControlType  type = ControlType::NONE;
      std::string  expression;
      std::string  goto_target;
      std::string  loop_id;
      int          max_iterations = 0;
      std::string  fork_group;
      std::string  join_group;
      std::string  callback_node;
    } control;
  };

  enum class MessagePriority {
    NORMAL = 0,
    HIGH   = 1,
  };

  struct Message {
    std::string     message_id;
    DataMap         source_data;
    MessagePriority priority = MessagePriority::NORMAL;
  };

  struct EdgeEvent {
    std::string                        type;
    std::map<std::string, std::string> data;
  };

  struct RuntimeConfig {
    int    message_threads = 1;
    int    event_threads = 1;
    int    processor_threads = 2;
    size_t max_queue_size = 1024;
  };

  using EdgeEventHandler = std::function<void(const EdgeEvent&)>;

  explicit Edge(const Config& config);
  ~Edge();

  // Getters
  const std::string& get_id() const { return config_.id; }
  const std::string& get_source() const { return config_.source_node; }
  const std::string& get_target() const { return config_.target_node; }
  DataTransferMode   get_transfer_mode() const { return config_.transfer_mode; }

  // Config getters for storage
  const std::vector<std::string>& get_source_outputs() const { return config_.source_outputs; }
  const std::vector<std::string>& get_target_inputs() const { return config_.target_inputs; }

  // Conditional execution support
  bool               has_condition() const { return !config_.condition_expression.empty(); }
  const std::string& get_condition_expression() const { return config_.condition_expression; }
  bool               has_control() const { return config_.control.type != ControlType::NONE; }
  ControlType        get_control_type() const { return config_.control.type; }
  const Config::ControlConfig& get_control_config() const { return config_.control; }
  int                get_priority() const { return config_.priority; }
  const Config&      get_config() const { return config_; }

  // Data passing
  bool transfer_data(const DataMap& source_data, DataMap& target_data);

  // Event management
  bool trigger_event(const std::string& event_type);

  // Edge runtime queue + event loop
  bool start_runtime();
  bool start_runtime(const RuntimeConfig& config);
  void stop_runtime();
  bool is_runtime_running() const { return runtime_running_; }

  bool enqueue_message(const Message& message);
  bool enqueue_event(const EdgeEvent& event);
  void register_event_handler(const std::string& event_type, EdgeEventHandler handler);

private:
  void message_dispatch_loop();
  void event_dispatch_loop();
  void process_message(const Message& message);

  Config config_;
  RuntimeConfig runtime_config_;
  std::atomic<bool> runtime_running_{false};
  std::deque<Message> high_priority_messages_;
  std::deque<Message> normal_priority_messages_;
  std::queue<EdgeEvent> event_queue_;
  std::mutex             queue_mutex_;
  std::condition_variable message_cv_;
  std::condition_variable event_cv_;
  std::thread             message_thread_;
  std::thread             event_thread_;
  std::unique_ptr<basic::ThreadPool> processor_pool_;
  std::unique_ptr<basic::ThreadPool> event_pool_;
  std::unordered_map<std::string, std::vector<EdgeEventHandler>> event_handlers_;
  std::mutex event_handler_mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_FRAMEWORK_EDGE_H
