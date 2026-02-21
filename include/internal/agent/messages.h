#ifndef AGENTFLOW_INTERNAL_AGENT_MESSAGES_H
#define AGENTFLOW_INTERNAL_AGENT_MESSAGES_H

#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <mutex>

namespace agentflow {
namespace framework {

/**
 * AgentMessages handles message communication within and between agents
 * Manages message queues, routing, and delivery
 */
class AgentMessages {
public:
  enum class MessageType {
    TASK_REQUEST   = 0,
    TASK_RESPONSE  = 1,
    EVENT          = 2,
    NOTIFICATION   = 3,
    ERROR          = 4,
  };

  enum class MessagePriority {
    LOW    = 0,
    NORMAL = 1,
    HIGH   = 2,
  };

  struct Message {
    std::string                          message_id;
    MessageType                          type;
    MessagePriority                      priority = MessagePriority::NORMAL;
    std::string                          sender_id;
    std::string                          receiver_id;
    std::string                          content;
    int64_t                              timestamp = 0;
    std::map<std::string, std::string>   metadata;
  };

  struct Config {
    int max_queue_size = 1024;
    int64_t message_timeout_ms = 5000;
  };

  explicit AgentMessages(const Config& config);
  ~AgentMessages();

  // Message operations
  bool enqueue_message(const Message& message);
  bool dequeue_message(Message& message);
  bool peek_message(Message& message) const;
  
  // Queue management
  int get_queue_size() const;
  bool is_queue_empty() const;
  bool is_queue_full() const;
  void clear_queue();

  // Message routing
  bool route_message(const std::string& receiver_id, const Message& message);
  std::vector<Message> get_messages_for_receiver(const std::string& receiver_id);

  // Configuration
  const Config& get_config() const { return config_; }
  int64_t get_total_messages_processed() const { return total_messages_processed_; }

private:
  Config                                      config_;
  std::vector<Message>                        message_queue_;
  int64_t                                     total_messages_processed_ = 0;
  mutable std::mutex                          mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_MESSAGES_H
