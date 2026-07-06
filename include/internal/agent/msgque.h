#ifndef AGENTFLOW_INTERNAL_AGENT_MSGQUE_H
#define AGENTFLOW_INTERNAL_AGENT_MSGQUE_H

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace agentflow {
namespace framework {

class MMAgentMsgQue {
public:
  enum class MessageQueueType {
    INTENT_REQUEST = 0,
    DATA_EXCHANGE  = 1,
    CONTROL        = 2,   // For control commands (interrupt, destroy)
  };

  enum class MessageType {
    TASK_REQUEST  = 0,
    TASK_RESPONSE = 1,
    EVENT         = 2,
    NOTIFICATION  = 3,
    ERROR         = 4,
    CONTROL       = 5,   // Control command
  };

  struct Message {
    std::string                        message_id;
    MessageType                        type = MessageType::TASK_REQUEST;
    std::string                        sender_id;
    std::string                        receiver_id;
    std::string                        content;
    std::string                        data_ref;   // Databus reference for payload
    int64_t                            timestamp = 0;
    std::map<std::string, std::string> metadata;
  };

  // Control command content values
  struct ControlCommand {
    static constexpr const char* SESSION_DESTROY    = "SESSION-DESTROY";
    static constexpr const char* SESSION_INTERRUPT  = "SESSION-INTERRUPT";
    static constexpr const char* DIALOGUE_INTERRUPT = "DIALOGUE-INTERRUPT";
  };

  // Metadata keys for control messages
  struct ControlMetadata {
    static constexpr const char* USER_ID    = "user_id";
    static constexpr const char* CLIENT_ID  = "client_id";
    static constexpr const char* SESSION_ID = "session_id";
  };

  struct Config {
     int     max_queue_size          = 1024;
     int64_t message_timeout_ms      = 5000;
     int     intent_request_queues   = 1;
     int     data_exchange_queues    = 1;
  };

  explicit MMAgentMsgQue(const Config& config);
  ~MMAgentMsgQue();

  bool enqueue_message(const Message& message);
  bool dequeue_message(Message& message);
  bool dequeue_message_by_type(MessageQueueType queue_type, Message& message);
  bool peek_message(Message& message) const;

  int  get_queue_size() const;
  int  get_queue_size(MessageQueueType queue_type) const;
  bool is_queue_empty() const;
  bool is_queue_full() const;
  void clear_queue();

  bool                 route_message(const std::string& receiver_id, const Message& message);
  std::vector<Message> get_messages_for_receiver(const std::string& receiver_id);

  const Config& get_config() const { return config_; }
  int64_t       get_total_messages_processed() const { return total_messages_processed_; }

private:
  MessageQueueType resolve_queue_type(const Message& message) const;
  bool enqueue_to_type_queue(const Message& message);
  bool dequeue_from_type_queue(MessageQueueType queue_type, Message& message);
  int  get_queue_size_unlocked() const;

  Config                    config_;
  std::vector<Message>      intent_request_queue_;
  std::vector<Message>      data_exchange_queue_;
  std::vector<Message>      control_queue_;
  std::atomic<int64_t>      total_messages_processed_{0};
  mutable std::mutex        mutex_;
};

using AgentMessages = MMAgentMsgQue;

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_MSGQUE_H
