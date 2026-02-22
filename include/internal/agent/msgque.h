#ifndef AGENTFLOW_INTERNAL_AGENT_MSGQUE_H
#define AGENTFLOW_INTERNAL_AGENT_MSGQUE_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace agentflow {
namespace framework {

class AgentMsgQue {
public:
  enum class MessageType {
    TASK_REQUEST  = 0,
    TASK_RESPONSE = 1,
    EVENT         = 2,
    NOTIFICATION  = 3,
    ERROR         = 4,
  };

  enum class MessagePriority {
    LOW    = 0,
    NORMAL = 1,
    HIGH   = 2,
  };

  struct Message {
    std::string                        message_id;
    MessageType                        type;
    MessagePriority                    priority = MessagePriority::NORMAL;
    std::string                        sender_id;
    std::string                        receiver_id;
    std::string                        content;
    std::string                        data_ref;   // Databus reference for payload
    int64_t                            timestamp = 0;
    std::map<std::string, std::string> metadata;
  };

  struct Config {
    int     max_queue_size      = 1024;
    int64_t message_timeout_ms  = 5000;
    int     high_priority_queues = 1;
    int     normal_priority_queues = 1;
    int     low_priority_queues = 1;
  };

  explicit AgentMsgQue(const Config& config);
  ~AgentMsgQue();

  bool enqueue_message(const Message& message);
  bool dequeue_message(Message& message);
  bool peek_message(Message& message) const;

  int  get_queue_size() const;
  int  get_queue_size(MessagePriority priority) const;
  bool is_queue_empty() const;
  bool is_queue_full() const;
  void clear_queue();

  bool                 route_message(const std::string& receiver_id, const Message& message);
  std::vector<Message> get_messages_for_receiver(const std::string& receiver_id);

  const Config& get_config() const { return config_; }
  int64_t       get_total_messages_processed() const { return total_messages_processed_; }

private:
  bool enqueue_to_priority_queue(const Message& message);
  bool dequeue_from_priority_queues(Message& message);

  Config                    config_;
  std::vector<Message>      high_priority_queue_;
  std::vector<Message>      normal_priority_queue_;
  std::vector<Message>      low_priority_queue_;
  int64_t                   total_messages_processed_ = 0;
  mutable std::mutex        mutex_;
};

using AgentMessages = AgentMsgQue;

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_MSGQUE_H
