// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// ResponsePoller implementation

#include "daemon/response_poller.h"

#include <chrono>
#include <sstream>

#include "agent/agent.h"
#include "daemon/agent_registry.h"
#include "daemon/rpc_server.h"
#include "utils/logger.h"

namespace agentflow {
namespace daemon {

namespace {

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
      default:   result += c; break;
    }
  }
  return result;
}

}  // namespace

ResponsePoller::ResponsePoller() = default;

ResponsePoller::~ResponsePoller() {
  stop();
}

void ResponsePoller::start(AgentRegistry& registry, RpcServer& server) {
  if (running_.load()) return;
  registry_ = &registry;
  rpc_server_ = &server;
  running_ = true;
  poll_thread_ = std::thread(&ResponsePoller::poll_loop, this);
  Logger::instance().info("[RESPONSE_POLLER] Started");
}

void ResponsePoller::stop() {
  if (!running_.exchange(false)) return;
  if (poll_thread_.joinable()) poll_thread_.join();
  Logger::instance().info("[RESPONSE_POLLER] Stopped");
}

void ResponsePoller::register_request(const std::string& message_id, int client_fd,
                                       const std::string& agent_id,
                                       const std::string& dialogue_id) {
  std::lock_guard<std::mutex> lock(in_flight_mutex_);
  InFlightRequest req;
  req.message_id = message_id;
  req.agent_id = agent_id;
  req.client_fd = client_fd;
  req.submitted_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  req.dialogue_id = dialogue_id;
  in_flight_[message_id] = req;
}

void ResponsePoller::remove_request(const std::string& message_id) {
  std::lock_guard<std::mutex> lock(in_flight_mutex_);
  in_flight_.erase(message_id);
}

void ResponsePoller::poll_loop() {
  while (running_.load()) {
    auto entries = registry_->list_all();

    for (auto& entry : entries) {
      framework::MMAgentMsgQue::Message response;
      while (entry.agent->dequeue_response_message(response)) {
        Logger::instance().info_f("[RESPONSE_POLLER] Dequeued response: msg_id=%s, content_len=%zu, agent=%s",
                                 response.message_id.c_str(), response.content.size(),
                                 entry.agent_id.c_str());
        // Find matching InFlightRequest by message_id or agent_id
        std::lock_guard<std::mutex> lock(in_flight_mutex_);

        // Try exact message_id match first
        auto it = in_flight_.find(response.message_id);
        if (it == in_flight_.end()) {
          // Workflow responses use task ids rather than the original RPC
          // message id. Match the stable per-turn dialogue id before falling
          // back to agent id, otherwise concurrent turns can cross routes.
          const auto dialogue_it = response.metadata.find("dialogue_id");
          if (dialogue_it != response.metadata.end() && !dialogue_it->second.empty()) {
            for (auto request_it = in_flight_.begin(); request_it != in_flight_.end();
                 ++request_it) {
              if (request_it->second.agent_id == entry.agent_id &&
                  request_it->second.dialogue_id == dialogue_it->second) {
                it = request_it;
                break;
              }
            }
          }
        }
        if (it == in_flight_.end()) {
          // Legacy fallback for responses that do not carry a dialogue id.
          for (auto& [mid, req] : in_flight_) {
            if (req.agent_id == entry.agent_id) {
              it = in_flight_.find(mid);
              break;
            }
          }
        }

        if (it != in_flight_.end()) {
          // Build metadata JSON
          std::ostringstream meta_oss;
          meta_oss << "{";
          bool first = true;
          for (const auto& [k, v] : response.metadata) {
            if (!first) meta_oss << ",";
            meta_oss << "\"" << json_escape(k) << "\":\"" << json_escape(v) << "\"";
            first = false;
          }
          meta_oss << "}";

          bool is_final = false;
          auto final_it = response.metadata.find("final");
          if (final_it != response.metadata.end() && final_it->second == "true") {
            is_final = true;
          }

          std::string params = build_response_notification(
              response.content, meta_oss.str(), is_final);

          rpc_server_->send_notification(it->second.client_fd, "agent.response", params);

          if (is_final) {
            in_flight_.erase(it);
          }
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

std::string ResponsePoller::build_response_notification(const std::string& content,
                                                         const std::string& metadata_json,
                                                         bool final_flag) {
  std::ostringstream oss;
  oss << "{\"content\":\"" << json_escape(content) << "\""
      << ",\"metadata\":" << metadata_json
      << ",\"final\":" << (final_flag ? "true" : "false")
      << "}";
  return oss.str();
}

}  // namespace daemon
}  // namespace agentflow
