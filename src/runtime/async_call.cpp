// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// AsyncCall SDK Implementation for non-blocking external resource access

#include "runtime/async_call.h"
#include "runtime/agent_runtime.h"
#include "agent/msgque.h"
#include "runtime/databus/databus.h"
#include "utils/logger.h"
#include <thread>
#include <atomic>

namespace agentflow {
namespace runtime {

// ============================================================================
// NodeAsyncCallSDK Implementation
// ============================================================================

class NodeAsyncCallSDK : public IAsyncCallSDK {
public:
  NodeAsyncCallSDK(MMAgentRuntime* runtime,
                   const std::string& execution_id,
                   const std::string& session_id,
                   const std::string& dialogue_id,
                   const std::string& task_id,
                   const std::string& node_id,
                   std::shared_ptr<std::atomic<int>> async_pending_count)
      : runtime_(runtime)
      , execution_id_(execution_id)
      , session_id_(session_id)
      , dialogue_id_(dialogue_id)
      , task_id_(task_id)
      , node_id_(node_id)
      , async_pending_count_(async_pending_count) {}

  ~NodeAsyncCallSDK() override = default;

  bool AsyncCall(const std::string& key, std::function<void()> async_func) override {
    if (!async_func || !async_pending_count_) {
      Logger::instance().warn_f("[AsyncCallSDK] Invalid async_func or async_pending_count for key=%s", key.c_str());
      return false;
    }

    // Increment pending count before spawning thread
    async_pending_count_->fetch_add(1);
    Logger::instance().info_f("[AsyncCallSDK] AsyncCall started: key=%s, node=%s, pending_count=%d",
                              key.c_str(), node_id_.c_str(), async_pending_count_->load());

    // Spawn thread for async operation
    std::thread([this, key, func = std::move(async_func)]() {
      try {
        func();
      } catch (const std::exception& e) {
        Logger::instance().error_f("[AsyncCallSDK] Async function exception: key=%s, error=%s",
                                   key.c_str(), e.what());
      } catch (...) {
        Logger::instance().error_f("[AsyncCallSDK] Async function unknown exception: key=%s", key.c_str());
      }

      // Note: We don't decrement here because the callback will be processed
      // when the node receives the [DONE] message and returns without __async_pending__
    }).detach();

    return true;
  }

  bool SendMsg(const std::string& target_session_id,
               const std::string& target_dialogue_id,
               const std::string& target_task_id,
               const std::string& target_node_id,
               const std::string& key,
               const std::string& msg) override {
    if (!runtime_) {
      Logger::instance().warn_f("[AsyncCallSDK] Runtime is null, cannot send message");
      return false;
    }

    // Create message with routing metadata
    framework::MMAgentMsgQue::Message message;
    message.message_id = "async-msg-" + key + "-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    message.content = msg;
    message.receiver_id = target_node_id;
    // Use DATA_EXCHANGE type (value 1 from anonymous enum)
    message.type = static_cast<framework::MMAgentMsgQue::MessageType>(1);

    // Set routing metadata for directed delivery
    message.metadata["session_id"] = target_session_id;
    message.metadata["dialogue_id"] = target_dialogue_id;
    message.metadata["task_id"] = target_task_id;
    message.metadata["node_id"] = target_node_id;
    message.metadata["async_call_key"] = key;
    message.metadata["message_type"] = "async_message";

    Logger::instance().debug_f("[AsyncCallSDK] SendMsg: session=%s, task=%s, node=%s, key=%s",
                               target_session_id.c_str(), target_task_id.c_str(),
                               target_node_id.c_str(), key.c_str());

    // Use runtime's message routing
    return runtime_->send_async_message(message);
  }

  bool SendToSelf(const std::string& key, const std::string& msg) override {
    if (!runtime_) {
      Logger::instance().warn_f("[AsyncCallSDK] Runtime is null, cannot send to self");
      return false;
    }

    // Create callback message with short-circuit routing
    framework::MMAgentMsgQue::Message message;
    message.message_id = "async-callback-" + key + "-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    message.content = msg;
    message.receiver_id = node_id_;
    // Use DATA_EXCHANGE type (value 1 from anonymous enum)
    message.type = static_cast<framework::MMAgentMsgQue::MessageType>(1);

    // Set routing metadata for self-delivery
    message.metadata["session_id"] = session_id_;
    message.metadata["dialogue_id"] = dialogue_id_;
    message.metadata["task_id"] = task_id_;
    message.metadata["node_id"] = node_id_;
    message.metadata["execution_id"] = execution_id_;
    message.metadata["async_call_key"] = key;
    message.metadata["message_type"] = "async_callback";

    Logger::instance().debug_f("[AsyncCallSDK] SendToSelf: node=%s, key=%s, msg_len=%zu",
                               node_id_.c_str(), key.c_str(), msg.size());

    // Use short-circuit routing - directly enqueue to task queue
    return runtime_->enqueue_async_callback(message);
  }

  const std::string& session_id() const override { return session_id_; }
  const std::string& dialogue_id() const override { return dialogue_id_; }
  const std::string& task_id() const override { return task_id_; }
  const std::string& node_id() const override { return node_id_; }

private:
  MMAgentRuntime* runtime_;
  std::string execution_id_;
  std::string session_id_;
  std::string dialogue_id_;
  std::string task_id_;
  std::string node_id_;
  std::shared_ptr<std::atomic<int>> async_pending_count_;
};

// ============================================================================
// Factory Function
// ============================================================================

std::shared_ptr<IAsyncCallSDK> create_async_call_sdk(
    MMAgentRuntime* runtime,
    const std::string& execution_id,
    const std::string& session_id,
    const std::string& dialogue_id,
    const std::string& task_id,
    const std::string& node_id,
    std::shared_ptr<std::atomic<int>> async_pending_count) {
  return std::make_shared<NodeAsyncCallSDK>(
      runtime, execution_id, session_id, dialogue_id, task_id, node_id, async_pending_count);
}

}  // namespace runtime
}  // namespace agentflow