// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// RpcHandler implementation - JSON-RPC method handlers for agentflowd

#include "daemon/rpc_handler.h"

#include <chrono>
#include <csignal>
#include <sstream>
#include <unistd.h>

#include "daemon/agent_registry.h"
#include "daemon/event_broker.h"
#include "daemon/response_poller.h"
#include "daemon/rpc_server.h"
#include "agent/agent.h"
#include "runtime/agent_runtime.h"
#include "utils/logger.h"

namespace agentflow {
namespace daemon {

namespace {

std::string extract_json_string_local(const std::string& json, const std::string& key) {
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
  size_t end = pos;
  while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') end++;
  std::string val = json.substr(pos, end - pos);
  while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
  return val;
}

std::string build_success(const std::string& result, int64_t id) {
  return "{\"jsonrpc\":\"2.0\",\"result\":" + result + ",\"id\":" + std::to_string(id) + "}";
}

std::string build_error(int code, const std::string& message, int64_t id) {
  std::string escaped;
  escaped.reserve(message.size() + 16);
  for (char c : message) {
    switch (c) {
      case '"':  escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      default:   escaped += c; break;
    }
  }
  return "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":" + std::to_string(code) +
         ",\"message\":\"" + escaped + "\"},\"id\":" + std::to_string(id) + "}";
}

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

RpcHandler::RpcHandler(runtime::MMAgentRuntime& runtime, AgentRegistry& registry,
                       ResponsePoller& poller, EventBroker& event_broker)
    : runtime_(runtime), registry_(registry), poller_(poller), event_broker_(event_broker),
      start_time_ms_(now_ms()) {}

std::string RpcHandler::json_escape(const std::string& s) {
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

void RpcHandler::register_all(RpcServer& server) {
  server_ = &server;

  // Query methods
  server.register_handler("health",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_health(p, id); });
  server.register_handler("runtime.status",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_runtime_status(p, id); });
  server.register_handler("agent.list",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_agent_list(p, id); });
  server.register_handler("agent.info",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_agent_info(p, id); });

  // Agent management
  server.register_handler("agent.load",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_agent_load(p, id); });
  server.register_handler("agent.unload",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_agent_unload(p, id); });
  server.register_handler("agent.pause",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_agent_pause(p, id); });
  server.register_handler("agent.resume",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_agent_resume(p, id); });

  // Request execution
  server.register_handler("agent.execute",
      [this](const std::string& p, int64_t id, int fd, ChannelType ch) {
        return handle_agent_execute(p, id, fd, ch);
      });
  server.register_handler("agent.poll_response",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_agent_poll_response(p, id); });
  server.register_handler("agent.send_control",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_agent_send_control(p, id); });

  // Session
  server.register_handler("session.list",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_session_list(p, id); });
  server.register_handler("session.destroy",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_session_destroy(p, id); });
  server.register_handler("session.clear_dialogue",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_session_clear_dialogue(p, id); });

  // Workflow
  server.register_handler("workflow.list",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_workflow_list(p, id); });
  server.register_handler("workflow.info",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_workflow_info(p, id); });
  server.register_handler("workflow.list_instances",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_workflow_list_instances(p, id); });

  // Worker
  server.register_handler("worker.list",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_worker_list(p, id); });

  // Runtime management
  server.register_handler("runtime.shutdown",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_runtime_shutdown(p, id); });

  // Events
  server.register_handler("event.subscribe",
      [this](const std::string& p, int64_t id, int fd, ChannelType) {
        return handle_event_subscribe(p, id, fd);
      });
  server.register_handler("event.unsubscribe",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_event_unsubscribe(p, id); });

  // Metrics
  server.register_handler("metrics.get",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_metrics_get(p, id); });
}

// --- Query methods ---

std::string RpcHandler::handle_health(const std::string& /*params*/, int64_t id) {
  int64_t uptime = now_ms() - start_time_ms_;
  return build_success("{\"status\":\"ok\",\"version\":\"0.1.0\",\"uptime_ms\":" +
                       std::to_string(uptime) + "}", id);
}

std::string RpcHandler::handle_runtime_status(const std::string& /*params*/, int64_t id) {
  int64_t uptime = now_ms() - start_time_ms_;
  bool running = runtime_.is_running();
  int active_agents = runtime_.get_active_agents();
  int active_workflows = runtime_.get_active_workflows();
  int running_nodes = runtime_.get_running_nodes();
  std::string role = (runtime_.get_role() == runtime::MMAgentRuntime::RuntimeRole::MASTER) ? "master" : "worker";

  std::ostringstream oss;
  oss << "{\"role\":\"" << role << "\""
      << ",\"running\":" << (running ? "true" : "false")
      << ",\"active_agents\":" << active_agents
      << ",\"active_workflows\":" << active_workflows
      << ",\"running_nodes\":" << running_nodes
      << ",\"uptime_ms\":" << uptime << "}";
  return build_success(oss.str(), id);
}

std::string RpcHandler::handle_agent_list(const std::string& /*params*/, int64_t id) {
  auto entries = registry_.list_all();
  std::ostringstream oss;
  oss << "{\"agents\":[";
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i > 0) oss << ",";
    auto workflows = entries[i].agent->get_workflows();
    oss << "{\"agent_id\":\"" << json_escape(entries[i].agent_id) << "\""
        << ",\"name\":\"" << json_escape(entries[i].name) << "\""
        << ",\"workflow_count\":" << workflows.size() << "}";
  }
  oss << "]}";
  return build_success(oss.str(), id);
}

std::string RpcHandler::handle_agent_info(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  if (agent_id.empty()) return build_error(-32602, "Invalid params: missing agent_id", id);
  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);

  auto workflows = agent->get_workflows();
  std::ostringstream oss;
  oss << "{\"agent_id\":\"" << json_escape(agent->get_id()) << "\""
      << ",\"name\":\"" << json_escape(agent->get_name()) << "\""
      << ",\"workflows\":[";
  for (size_t i = 0; i < workflows.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "{\"id\":\"" << json_escape(workflows[i]->get_id()) << "\""
        << ",\"name\":\"" << json_escape(workflows[i]->get_name()) << "\""
        << ",\"node_count\":" << workflows[i]->get_nodes().size() << "}";
  }
  oss << "]}";
  return build_success(oss.str(), id);
}

// --- Agent management ---

std::string RpcHandler::handle_agent_load(const std::string& params, int64_t id) {
  std::string package_path = extract_json_string_local(params, "package_path");
  std::string name_override = extract_json_string_local(params, "name_override");
  if (package_path.empty()) return build_error(-32602, "Invalid params: missing package_path", id);

  if (!registry_.load_agent(package_path, name_override, runtime_)) {
    return build_error(-1, "Failed to load agent from: " + package_path, id);
  }

  auto entries = registry_.list_all();
  auto& entry = entries.back();
  // Start the newly loaded agent
  entry.agent->start();

  return build_success("{\"agent_id\":\"" + json_escape(entry.agent_id) + "\""
                     + ",\"name\":\"" + json_escape(entry.name) + "\""
                     + ",\"success\":true}", id);
}

std::string RpcHandler::handle_agent_unload(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  if (agent_id.empty()) return build_error(-32602, "Invalid params: missing agent_id", id);
  if (!registry_.unload_agent(agent_id, runtime_)) {
    return build_error(-1, "Failed to unload agent: " + agent_id, id);
  }
  return build_success("{\"success\":true}", id);
}

std::string RpcHandler::handle_agent_pause(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  if (agent_id.empty()) return build_error(-32602, "Invalid params: missing agent_id", id);
  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);
  agent->stop();
  return build_success("{\"success\":true}", id);
}

std::string RpcHandler::handle_agent_resume(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  if (agent_id.empty()) return build_error(-32602, "Invalid params: missing agent_id", id);
  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);
  agent->start();
  return build_success("{\"success\":true}", id);
}

// --- Request execution ---

std::string RpcHandler::handle_agent_execute(const std::string& params, int64_t id,
                                              int client_fd, ChannelType channel) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string user_id = extract_json_string_local(params, "user_id");
  std::string client_id = extract_json_string_local(params, "client_id");
  std::string content = extract_json_string_local(params, "content");
  std::string trace_labels = extract_json_string_local(params, "trace_labels");
  std::string dialogue_id = extract_json_string_local(params, "dialogue_id");
  std::string skill_name = extract_json_string_local(params, "skill_name");

  if (agent_id.empty() || user_id.empty() || client_id.empty() || content.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id, user_id, client_id, or content", id);
  }

  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);

  // Generate message_id
  std::string message_id = "msg_" + std::to_string(++message_counter_) + "_" +
                           std::to_string(now_ms());

  // Construct intent_request message
  framework::MMAgentMsgQue::Message message;
  message.message_id = message_id;
  message.type = framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
  message.sender_id = user_id;
  message.receiver_id = agent_id;
  message.content = content;
  message.timestamp = now_ms();
  message.metadata["message_type"] = "intent_request";
  message.metadata["user_id"] = user_id;
  message.metadata["client_id"] = client_id;
  if (!trace_labels.empty()) message.metadata["trace_labels"] = trace_labels;
  if (!dialogue_id.empty()) message.metadata["dialogue_id"] = dialogue_id;
  if (!skill_name.empty()) message.metadata["skill_name"] = skill_name;

  if (!agent->route_request_message(message)) {
    return build_error(-1, "Failed to route request message", id);
  }

  // Register with ResponsePoller for async notification routing
  poller_.register_request(message_id, client_fd, agent_id, dialogue_id);

  return build_success("{\"message_id\":\"" + json_escape(message_id) + "\""
                     + ",\"status\":\"submitted\"}", id);
}

std::string RpcHandler::handle_agent_poll_response(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  int timeout_ms = 5000;
  std::string timeout_str = extract_json_string_local(params, "timeout_ms");
  if (!timeout_str.empty()) {
    try { timeout_ms = std::stoi(timeout_str); } catch (...) {}
  }

  if (agent_id.empty()) return build_error(-32602, "Invalid params: missing agent_id", id);
  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);

  // Poll response messages
  std::ostringstream result_oss;
  result_oss << "{\"messages\":[";
  int msg_count = 0;
  bool has_final = false;

  auto start = now_ms();
  while (true) {
    framework::MMAgentMsgQue::Message response;
    while (agent->dequeue_response_message(response)) {
      if (msg_count > 0) result_oss << ",";
      result_oss << "{\"content\":\"" << json_escape(response.content) << "\"";

      // Check for final flag
      auto final_it = response.metadata.find("final");
      if (final_it != response.metadata.end() && final_it->second == "true") {
        has_final = true;
        result_oss << ",\"final\":true";
      }

      result_oss << "}";
      msg_count++;
    }

    bool runtime_quiet = runtime_.get_running_nodes() == 0;
    bool queue_empty = agent->get_response_queue_size() == 0;
    bool complete = runtime_quiet && queue_empty;

    if ((complete && msg_count > 0) || has_final) break;

    auto elapsed = now_ms() - start;
    if (elapsed >= timeout_ms) break;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  result_oss << "],\"complete\":" << (has_final || (msg_count > 0 && runtime_.get_running_nodes() == 0) ? "true" : "false") << "}";
  return build_success(result_oss.str(), id);
}

std::string RpcHandler::handle_agent_send_control(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string command = extract_json_string_local(params, "command");
  std::string session_id = extract_json_string_local(params, "session_id");
  std::string user_id = extract_json_string_local(params, "user_id");

  if (agent_id.empty() || command.empty() || session_id.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id, command, or session_id", id);
  }

  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);

  framework::MMAgentMsgQue::Message ctrl_msg;
  ctrl_msg.message_id = "ctrl_" + std::to_string(++message_counter_) + "_" + std::to_string(now_ms());
  ctrl_msg.type = framework::MMAgentMsgQue::MessageType::CONTROL;
  ctrl_msg.sender_id = user_id.empty() ? "daemon" : user_id;
  ctrl_msg.receiver_id = agent_id;
  ctrl_msg.content = command;
  ctrl_msg.timestamp = now_ms();
  ctrl_msg.metadata["message_type"] = "control";
  ctrl_msg.metadata["session_id"] = session_id;
  if (!user_id.empty()) ctrl_msg.metadata["user_id"] = user_id;

  if (!agent->route_request_message(ctrl_msg)) {
    return build_error(-1, "Failed to route control message", id);
  }

  return build_success("{\"success\":true}", id);
}

// --- Session ---

std::string RpcHandler::handle_session_list(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  if (agent_id.empty()) return build_error(-32602, "Invalid params: missing agent_id", id);
  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);
  return build_success("{\"sessions\":[]}", id);
}

std::string RpcHandler::handle_session_destroy(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string session_id = extract_json_string_local(params, "session_id");
  if (agent_id.empty() || session_id.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id or session_id", id);
  }
  int removed = runtime_.clear_session_data(agent_id, session_id);
  return build_success("{\"success\":true,\"keys_removed\":" + std::to_string(removed) + "}", id);
}

std::string RpcHandler::handle_session_clear_dialogue(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string session_id = extract_json_string_local(params, "session_id");
  std::string dialogue_id = extract_json_string_local(params, "dialogue_id");
  if (agent_id.empty() || session_id.empty() || dialogue_id.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id, session_id, or dialogue_id", id);
  }
  int removed = runtime_.clear_dialogue_data(agent_id, session_id, dialogue_id);
  return build_success("{\"success\":true,\"keys_removed\":" + std::to_string(removed) + "}", id);
}

// --- Workflow ---

std::string RpcHandler::handle_workflow_list(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  if (agent_id.empty()) return build_error(-32602, "Invalid params: missing agent_id", id);
  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);

  auto workflows = agent->get_workflows();
  std::ostringstream oss;
  oss << "{\"workflows\":[";
  for (size_t i = 0; i < workflows.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "{\"id\":\"" << json_escape(workflows[i]->get_id()) << "\""
        << ",\"name\":\"" << json_escape(workflows[i]->get_name()) << "\""
        << ",\"node_count\":" << workflows[i]->get_nodes().size() << "}";
  }
  oss << "]}";
  return build_success(oss.str(), id);
}

std::string RpcHandler::handle_workflow_info(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string workflow_id = extract_json_string_local(params, "workflow_id");
  if (agent_id.empty() || workflow_id.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id or workflow_id", id);
  }
  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);
  auto wf = agent->get_workflow(workflow_id);
  if (!wf) return build_error(-3, "Workflow not found: " + workflow_id, id);

  auto nodes = wf->get_nodes();
  auto edges = wf->get_edges();
  std::ostringstream oss;
  oss << "{\"id\":\"" << json_escape(wf->get_id()) << "\""
      << ",\"name\":\"" << json_escape(wf->get_name()) << "\""
      << ",\"nodes\":[";
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "\"" << json_escape(nodes[i]->get_id()) << "\"";
  }
  oss << "],\"edges\":[";
  for (size_t i = 0; i < edges.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "\"" << json_escape(edges[i]->get_id()) << "\"";
  }
  oss << "]}";
  return build_success(oss.str(), id);
}

std::string RpcHandler::handle_workflow_list_instances(const std::string& /*params*/, int64_t id) {
  auto instances = runtime_.list_workflow_instances();
  std::ostringstream oss;
  oss << "{\"instances\":[";
  for (size_t i = 0; i < instances.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "{\"instance_id\":\"" << json_escape(instances[i].workflow_instance_id) << "\""
        << ",\"workflow_id\":\"" << json_escape(instances[i].workflow_id) << "\"}";
  }
  oss << "]}";
  return build_success(oss.str(), id);
}

// --- Worker ---

std::string RpcHandler::handle_worker_list(const std::string& /*params*/, int64_t id) {
  auto workers = runtime_.list_worker_engines();
  std::ostringstream oss;
  oss << "{\"workers\":[";
  for (size_t i = 0; i < workers.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "{\"worker_id\":\"" << json_escape(workers[i].worker_id) << "\""
        << ",\"status\":" << static_cast<int>(workers[i].status)
        << ",\"load\":" << workers[i].load
        << ",\"last_heartbeat_ms\":" << workers[i].last_heartbeat_ms << "}";
  }
  oss << "]}";
  return build_success(oss.str(), id);
}

// --- Runtime management ---

std::string RpcHandler::handle_runtime_shutdown(const std::string& /*params*/, int64_t id) {
  Logger::instance().info("[RPC_HANDLER] runtime.shutdown requested via RPC");
  kill(getpid(), SIGTERM);
  return build_success("{\"success\":true}", id);
}

// --- Events ---

std::string RpcHandler::handle_event_subscribe(const std::string& params, int64_t id,
                                                int client_fd) {
  // Extract event_types from params JSON array
  std::set<std::string> event_types;

  // Simple array extraction: find "event_types" and parse [...]
  std::string search = "\"event_types\"";
  size_t pos = params.find(search);
  if (pos == std::string::npos) {
    return build_error(-32602, "Invalid params: missing event_types", id);
  }
  pos = params.find('[', pos);
  if (pos == std::string::npos) {
    return build_error(-32602, "Invalid params: event_types must be an array", id);
  }

  int depth = 0;
  size_t start = pos;
  for (; pos < params.size(); pos++) {
    if (params[pos] == '[') depth++;
    else if (params[pos] == ']') {
      depth--;
      if (depth == 0) break;
    }
  }

  std::string arr = params.substr(start, pos - start + 1);
  // Parse comma-separated quoted strings
  size_t i = 1;  // skip '['
  while (i < arr.size() - 1) {  // skip ']'
    while (i < arr.size() && (arr[i] == ' ' || arr[i] == ',' || arr[i] == '\t')) i++;
    if (i >= arr.size() || arr[i] != '"') break;
    i++;  // skip opening quote
    size_t end = arr.find('"', i);
    if (end == std::string::npos) break;
    event_types.insert(arr.substr(i, end - i));
    i = end + 1;
  }

  if (event_types.empty()) {
    return build_error(-32602, "Invalid params: event_types array is empty", id);
  }

  int sub_id = event_broker_.subscribe(client_fd, event_types);
  return build_success("{\"subscription_id\":" + std::to_string(sub_id) + "}", id);
}

std::string RpcHandler::handle_event_unsubscribe(const std::string& params, int64_t id) {
  std::string sub_id_str = extract_json_string_local(params, "subscription_id");
  if (sub_id_str.empty()) {
    return build_error(-32602, "Invalid params: missing subscription_id", id);
  }
  int sub_id = 0;
  try { sub_id = std::stoi(sub_id_str); } catch (...) {
    return build_error(-32602, "Invalid params: subscription_id must be a number", id);
  }

  if (!event_broker_.unsubscribe(sub_id)) {
    return build_error(-2, "Subscription not found: " + sub_id_str, id);
  }
  return build_success("{\"success\":true}", id);
}

// --- Metrics ---

std::string RpcHandler::handle_metrics_get(const std::string& params, int64_t id) {
  std::string format = extract_json_string_local(params, "format");
  if (format.empty()) format = "json";

  int64_t uptime = now_ms() - start_time_ms_;
  int64_t msg_count = message_counter_.load();
  int active_agents = runtime_.get_active_agents();
  int running_nodes = runtime_.get_running_nodes();
  auto entries = registry_.list_all();

  if (format == "prometheus") {
    std::ostringstream oss;
    oss << "# HELP agentflow_uptime_ms Daemon uptime in milliseconds\n"
        << "# TYPE agentflow_uptime_ms gauge\n"
        << "agentflow_uptime_ms " << uptime << "\n"
        << "# HELP agentflow_messages_total Total messages processed\n"
        << "# TYPE agentflow_messages_total counter\n"
        << "agentflow_messages_total " << msg_count << "\n"
        << "# HELP agentflow_active_agents Number of loaded agents\n"
        << "# TYPE agentflow_active_agents gauge\n"
        << "agentflow_active_agents " << active_agents << "\n"
        << "# HELP agentflow_running_nodes Number of running workflow nodes\n"
        << "# TYPE agentflow_running_nodes gauge\n"
        << "agentflow_running_nodes " << running_nodes << "\n";
    return build_success("\"" + json_escape(oss.str()) + "\"", id);
  }

  // JSON format (default)
  std::ostringstream oss;
  oss << "{\"uptime_ms\":" << uptime
      << ",\"messages_total\":" << msg_count
      << ",\"active_agents\":" << active_agents
      << ",\"running_nodes\":" << running_nodes
      << ",\"agents\":[";
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "{\"agent_id\":\"" << json_escape(entries[i].agent_id) << "\"}";
  }
  oss << "]}";
  return build_success(oss.str(), id);
}

}  // namespace daemon
}  // namespace agentflow
