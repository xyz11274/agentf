// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// RpcHandler implementation - JSON-RPC method handlers for agentflowd

#include "daemon/rpc_handler.h"

#include <chrono>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <sstream>
#include <unistd.h>

#include "daemon/agent_registry.h"
#include "daemon/event_broker.h"
#include "daemon/response_poller.h"
#include "daemon/rpc_server.h"
#include "daemon/sandbox_lifecycle.h"  // SandboxLifecycle::create
#include "daemon/sandbox_snapshots.h"  // SandboxSnapshots::snapshot/delete_snapshot/list_snapshots（快照 RPC）
#include "daemon/session_manager.h"
#include "sandbox/sandbox_factory.h"   // SandboxFactory::SandboxLaunchException
#include "sandbox/sandbox_helpers.h"   // SandboxHelpers::validate_upload_path
#include "agent/agent.h"
#include "agent/session_router.h"
#include "runtime/agent_runtime.h"
#include "agent/data_variant_utils.h"
#include "utils/logger.h"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace agentflow {
namespace daemon {

namespace {

// RapidJSON-based JSON helpers (replaced hand-written parser)
static std::string rj_stringify(const rapidjson::Value& v) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  v.Accept(w);
  return buf.GetString();
}

std::string extract_json_string_local(const std::string& json, const std::string& key) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember(key.c_str())) return "";
  const auto& v = d[key.c_str()];
  if (v.IsString()) return v.GetString();
  return rj_stringify(v);
}

std::string extract_json_object_local(const std::string& json, const std::string& key) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.HasMember(key.c_str()) || !d[key.c_str()].IsObject()) return "";
  return rj_stringify(d[key.c_str()]);
}

// DataVariant → JSON string (mirrors uds_server.cpp variant_to_json).
// Strings that are already JSON values (start with '{' or '[') pass through
// verbatim so consumers keep receiving objects/arrays; plain strings (incl.
// empty ones) are quoted + fully escaped so the frame stays valid JSON.
// Non-string variants are serialized by data_variant_to_string(), which
// emits native JSON for bool/int64/double/FileDescriptor/FileDescriptorList.
std::string variant_to_json_local(const DataVariant& var) {
  if (std::holds_alternative<std::string>(var)) {
    const std::string& str = std::get<std::string>(var);
    size_t first_char_pos = str.find_first_not_of(" \t\n\r");
    if (first_char_pos != std::string::npos &&
        (str[first_char_pos] == '{' || str[first_char_pos] == '[')) {
      return str;  // Already JSON, pass through
    }
    return "\"" + escape_json_string(str) + "\"";
  }
  return data_variant_to_string(var);
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

bool is_safe_identity_component(const std::string& value, bool allow_underscore,
                                size_t max_size = 128) {
  if(value.empty() || value.size() > max_size) return false;
  for(unsigned char ch : value) {
    if(std::isalnum(ch) || ch == '.' || ch == '-' ||
       (allow_underscore && ch == '_')) {
      continue;
    }
    return false;
  }
  return true;
}

std::string artifact_routing_id(const std::string& user_id,
                                const std::string& client_id) {
  if (user_id.empty() || client_id.empty()) return "";
  if (user_id.find('_') == std::string::npos &&
      client_id.find('_') == std::string::npos) {
    return user_id + "_" + client_id;
  }
  return "v2__u" + std::to_string(user_id.size()) + "-" + user_id +
         "-c" + std::to_string(client_id.size()) + "-" + client_id;
}

// Resolve session_id: prefer (user_id, client_id) via SessionRouter,
// fallback to session_id param (for internal tools/testing).
// Returns the resolved session_id, or empty string if neither works.
std::string resolve_session_id(
    const std::string& user_id,
    const std::string& client_id,
    const std::string& session_id,
    const std::shared_ptr<framework::MMAgent>& agent)
{
  if (!user_id.empty() && !client_id.empty()) {
    auto router = agent->get_session_router();
    if (router) {
      auto session = router->get_session_by_route_key(user_id + ":" + client_id);
      if (session) return session->get_session_id();
    }
  }
  return session_id;
}

}  // namespace

RpcHandler::RpcHandler(runtime::MMAgentRuntime& runtime, AgentRegistry& registry,
                       ResponsePoller& poller, EventBroker& event_broker,
                       SessionManager& session_manager,
                       SandboxLifecycle* sandbox_lifecycle,
                       SandboxSnapshots* sandbox_snapshots,
                       bool sandbox_allow_fallback)
    : runtime_(runtime), registry_(registry), poller_(poller), event_broker_(event_broker),
      session_manager_(session_manager), sandbox_lifecycle_(sandbox_lifecycle),
      sandbox_snapshots_(sandbox_snapshots),
      sandbox_allow_fallback_(sandbox_allow_fallback),
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

  // DataBus proxy (LLM Trace & general DataBus queries)
  server.register_handler("databus.get",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_databus_get(p, id); });
  server.register_handler("databus.list_keys",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_databus_list_keys(p, id); });
  server.register_handler("databus.remove_by_prefix",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_databus_remove_by_prefix(p, id); });

  // Sandbox
  // 外层文件通道（保留）：sandbox.read_file/upload_file 是 bridge↔daemon 外层语义
  //（用户文件上传 / 产物拉回），非 agent 内部工具执行（后者由 Python executor 直连
  // 承载）。Local/Docker 经 HTTPExecutor 直连沙箱内 HTTP server 可用；e2b 的 cubeproxy
  // 502 是基础设施缺口（2026-08-09），应修复通道而非删除（2026-08-17 用户确认保留）。
  server.register_handler("sandbox.read_file",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_read_file(p, id); });
  server.register_handler("sandbox.upload_file",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_upload_file(p, id); });
  server.register_handler("sandbox.list",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_list(p, id); });
  server.register_handler("sandbox.destroy",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_destroy(p, id); });
  // 暂停/恢复（B21：RUNNING↔STOPPED 可逆）
  server.register_handler("sandbox.pause",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_pause(p, id); });
  server.register_handler("sandbox.resume",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_resume(p, id); });
  // 快照管理（生成 / 单条删除 / 列表）
  server.register_handler("sandbox.snapshot",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_snapshot(p, id); });
  server.register_handler("sandbox.snapshot_delete",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_snapshot_delete(p, id); });
  server.register_handler("sandbox.snapshot_list",
      [this](const std::string& p, int64_t id, int, ChannelType) { return handle_sandbox_snapshot_list(p, id); });
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

  // v2 段1 预热：load agent 后触发 e2b template preload（仅 e2b 模式内部有效）
  // 用户 2026-08-08 拍板：preload 失败不拒绝 load——load RPC 返回 warning 语义，
  // agent 仍载入，execute 首问走 v1 懒加载兜底。模板 build 阻塞等完成（部署期可接受）。
  std::string preload_status = "skipped";
  if (sandbox_lifecycle_) {
    try {
      bool ok = sandbox_lifecycle_->preload_template(entry.agent_id, entry.package_path);
      preload_status = ok ? "ready" : "degraded";
      if (!ok) {
        Logger::instance().warn_f(
            "[TEMPLATE] preload degraded for agent=%s (load success, execute 首问走 v1 兜底)",
            entry.agent_id.c_str());
      }
    } catch (const std::exception& e) {
      preload_status = "degraded";
      Logger::instance().warn_f(
          "[TEMPLATE] preload threw for agent=%s: %s (load success, execute 首问走 v1 兜底)",
          entry.agent_id.c_str(), e.what());
    }
  }

  return build_success("{\"agent_id\":\"" + json_escape(entry.agent_id) + "\""
                     + ",\"name\":\"" + json_escape(entry.name) + "\""
                     + ",\"template_preload\":\"" + preload_status + "\""
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
  std::string request_id = extract_json_string_local(params, "request_id");
  std::string platform_context = extract_json_object_local(params, "platform_context");
  std::string stream_str = extract_json_string_local(params, "stream");

  if (agent_id.empty() || user_id.empty() || client_id.empty() || content.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id, user_id, client_id, or content", id);
  }
  if(!is_safe_identity_component(user_id, true, 120) ||
     !is_safe_identity_component(client_id, true, 120)) {
    return build_error(-32602, "Invalid params: user_id/client_id contain unsupported characters", id);
  }
  if(!dialogue_id.empty() && !is_safe_identity_component(dialogue_id, true)) {
    return build_error(-32602, "Invalid params: dialogue_id contains unsupported characters", id);
  }

  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);

  // Generate trace_id — single correlation id for the full "input→exec→response" chain
  // Reuse request_id if provided by caller, otherwise generate one
  std::string trace_id = request_id.empty()
      ? "trc_" + std::to_string(now_ms()) + "_" + std::to_string(++message_counter_)
      : request_id;

  // [USER_INPUT] — entry point for all user messages into the system
  {
    Logger::instance().info_f(
        "[USER_INPUT] trace=%s agent=%s user=%s client=%s dialogue=%s "
        "content_len=%zu stream=%s",
        trace_id.c_str(), agent_id.c_str(), user_id.c_str(), client_id.c_str(),
        dialogue_id.empty() ? "(none)" : dialogue_id.c_str(),
        content.size(), stream_str.c_str());
  }

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
  message.metadata["trace_id"] = trace_id;
  if (!trace_labels.empty()) message.metadata["trace_labels"] = trace_labels;
  if (!dialogue_id.empty()) message.metadata["dialogue_id"] = dialogue_id;
  if (!skill_name.empty()) message.metadata["skill_name"] = skill_name;
  if (!request_id.empty()) message.metadata["request_id"] = request_id;
  if (!platform_context.empty()) message.metadata["platform_context"] = platform_context;
  if (stream_str == "true") message.metadata["stream"] = "true";

  // ── Sandbox 创建（按用户身份，懒加载）────────────────────────────
  // 在 route_request_message 前创建沙箱，确保 worker 执行时沙箱已就绪
  // sandbox_lifecycle_ 为 nullptr 时跳过（sandbox-mode=none 向后兼容）
  std::shared_ptr<sandbox::Sandbox> sandbox_sb;
  if (sandbox_lifecycle_) {
    sandbox::SandboxKey key;
    key.user_id = user_id;
    key.client_id = client_id;
    key.agent_id = agent_id;
    try {
      auto sb_t0 = std::chrono::steady_clock::now();
      sandbox_sb = sandbox_lifecycle_->create(key);
      auto sb_t1 = std::chrono::steady_clock::now();
      auto sb_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sb_t1 - sb_t0).count();
      if (sandbox_sb) {
        Logger::instance().info_f(
            "[SANDBOX] Created/reused sandbox id=%s for user=%s client=%s agent=%s (create_latency=%lldms)",
            sandbox_sb->id().c_str(), user_id.c_str(), client_id.c_str(), agent_id.c_str(), sb_ms);
      }
    } catch (const SandboxLifecycle::ConcurrentLimitException& e) {
      Logger::instance().warn_f(
          "[SANDBOX] Concurrent limit reached (%d/%d) for user=%s client=%s agent=%s, "
          "continuing without sandbox isolation",
          e.current(), e.limit(), user_id.c_str(), client_id.c_str(), agent_id.c_str());
      // 不拒绝请求——沙箱隔离是增强能力，请求本身仍可处理（降级到无沙箱）
    } catch (const sandbox::SandboxFactory::SandboxLaunchException& e) {
      if (sandbox_allow_fallback_) {
        Logger::instance().error_f(
            "[SANDBOX] Failed to launch sandbox for user=%s client=%s agent=%s: %s, "
            "continuing without sandbox isolation",
            user_id.c_str(), client_id.c_str(), agent_id.c_str(), e.what());
        // 不拒绝请求——沙箱拉起失败（如 Docker 不可用/镜像未构建）降级到无沙箱
      } else {
        // allow_fallback=false：沙箱拉起失败直接拒绝请求，返回明确错误
        auto resp = build_error(-32000,
            "Sandbox launch failed: " + std::string(e.what()) +
            " [allow_fallback=false]", id);
        Logger::instance().error_f(
            "[SANDBOX] Sandbox launch failed, rejecting request (allow_fallback=false): %s",
            e.what());
        return resp;
      }
    }
  }

  // 沙箱创建成功后将 endpoint + mode 注入 message metadata（flow 到 node inputs）
  // 并 setenv 到 daemon 进程环境，确保所有子进程（含 skill_executor）继承
  // AF_SANDBOX_ENDPOINT + AF_SANDBOX_MODE——factory.py 同时读这两个 env 才能选对
  // executor（mode=e2b→E2BExecutor，mode=local/docker→HTTPExecutor，mode=none/缺→HostExecutor）。
  // 漏注 AF_SANDBOX_MODE 会让 factory 落到 HostExecutor，即使 endpoint 已注入也走本地 subprocess。
  // sandbox_sb->sandbox_mode() 已是小写 "e2b"（sandbox_e2b.h:71），与 factory.py 的 mode 比较
  // 直接对齐；Local/Docker 基类返回 ""，映射到 "local"（factory 的 "local"/"docker" 共享 HTTPExecutor 分支）。
  if (sandbox_sb) {
    const std::string& ep = sandbox_sb->endpoint();
    message.metadata["sandbox_endpoint"] = ep;
    setenv("AF_SANDBOX_ENDPOINT", ep.c_str(), 1);
    std::string raw_mode = sandbox_sb->sandbox_mode();  // "e2b" / "" (Local/Docker)
    std::string af_mode = raw_mode.empty() ? "local" : raw_mode;
    message.metadata["sandbox_mode"] = af_mode;
    setenv("AF_SANDBOX_MODE", af_mode.c_str(), 1);
    Logger::instance().info_f(
        "[SANDBOX] Injected AF_SANDBOX_ENDPOINT=%s AF_SANDBOX_MODE=%s for user=%s client=%s",
        ep.c_str(), af_mode.c_str(), user_id.c_str(), client_id.c_str());

    // 同步会话级用户上传目录到沙箱（agent-package-structure.md §2.2/§3.2）
    // E2B 沙箱远端，需主动同步；Local/Docker 共享 workdir，基类 no-op 跳过
    // 失败不拒绝请求（沙箱隔离是增强能力），记 warning 继续处理
    try {
      std::string session_upload_dir = _resolve_session_upload_dir(agent_id, user_id, client_id);
      if (!session_upload_dir.empty()) {
        sandbox_sb->sync_session_uploads(session_upload_dir);
      }
    } catch (const std::exception& e) {
      Logger::instance().warn_f(
          "[SANDBOX] sync_session_uploads failed for user=%s client=%s agent=%s: %s "
          "(continuing without user uploads in sandbox)",
          user_id.c_str(), client_id.c_str(), agent_id.c_str(), e.what());
    }

    // B: 注入路径语义——把 user message 中的图片相对路径改写为沙箱内绝对路径
    // （agent-package-structure.md §3.6 I-7 路径注入语义，2026-08-12 修复）
    //
    // 问题：Bridge content_serializer 注入的是相对路径
    //   workspace/{user_id}_{client_id}/upload/{filename}
    // LLM 拿它拼沙箱绝对路径时，把 "workspace" 前缀误当沙箱根
    // （/home/user/workspace 最后一节也叫 workspace），拼成
    //   /home/user/workspace/{user_id}_{client_id}/upload/...（少拼一层 workspace/）
    // 而正确沙箱路径是
    //   /home/user/workspace/workspace/{user_id}_{client_id}/upload/...
    // 实测（2026-08-12 scene-uc1 日志）：agent 第一次 ls 上述少一层路径 → NO_IMAGE，
    // 然后 find / 兜底 + e2b 输出翻译成宿主机路径才侥幸成功。
    //
    // 修复：route_request_message 下发前，e2b 模式把相对路径改写为沙箱内绝对路径，
    // coordinator/skill_executor 透传给 LLM 的路径无歧义。
    // Local/Docker 与宿主机共享 workdir（sandbox_workspace_prefix() 空），不改写。
    // 实现收敛在 SandboxHelpers::rewrite_upload_path_to_sandbox（F5 单一真相来源）。
    if (sandbox_sb) {
      const std::string ws_prefix = sandbox_sb->sandbox_workspace_prefix();
      if (!ws_prefix.empty()) {
        const std::string session_marker =
            "workspace/" + artifact_routing_id(user_id, client_id) + "/upload/";
        std::string rewritten = sandbox::SandboxHelpers::rewrite_upload_path_to_sandbox(
            message.content, session_marker, ws_prefix);
        if (rewritten != message.content) {
          Logger::instance().info_f(
              "[SANDBOX] rewrite upload path rel=workspace/.../upload/ -> %s/workspace/... "
              "(user=%s client=%s)",
              ws_prefix.c_str(), user_id.c_str(), client_id.c_str());
          message.content = rewritten;
        }
      }
    }
  }

  // 2026-08-17 Step 1（path-translation-convergence.md）：统一注入当前执行环境工作目录契约
  // sandbox_workspace → session metadata → workflow inputs → worker.cpp 自动注入 AF_SANDBOX_WORKSPACE：
  //   e2b = 沙箱目录（sandbox_sb->sandbox_workspace_prefix()，/home/user/workspace）
  //   host = agent 包根（config.agents[i].package_path 绝对化，对齐 _resolve_session_upload_dir）
  //   Local/Docker 共享 workdir → sandbox_workspace_prefix() 返回 "" → 回落到 agent 包根
  {
    std::string sandbox_workspace_val;
    if (sandbox_sb) {
      sandbox_workspace_val = sandbox_sb->sandbox_workspace_prefix();
    }
    if (sandbox_workspace_val.empty() && sandbox_lifecycle_) {
      const auto& sandbox_config = sandbox_lifecycle_->config();
      namespace fs = std::filesystem;
      for (const auto& entry : sandbox_config.agents) {
        std::string pkg_path = fs::absolute(entry.package_path).string();
        std::string name = entry.name_override;
        if (name.empty()) {
          auto pos = pkg_path.find_last_of('/');
          name = (pos != std::string::npos) ? pkg_path.substr(pos + 1) : pkg_path;
        }
        if (name == agent_id) {
          sandbox_workspace_val = pkg_path;
          break;
        }
      }
    }
    // 2026-08-17 review P1 修复：host 模式（sandbox_lifecycle_ 为 nullptr，mode=none 是
    // config 默认值 config.h:62）下上述 config.agents 分支不执行 → AF_SANDBOX_WORKSPACE
    // 不注入，违背 Step 1 验收"节点进程 env 恒含（host=agent 包根）"。回退从 registry_
    // 反查 agent 包根（load_agent 时注册的 package_path，handle_agent_load 同源）。
    if (sandbox_workspace_val.empty()) {
      namespace fs = std::filesystem;
      for (const auto& entry : registry_.list_all()) {
        if (entry.agent_id == agent_id && !entry.package_path.empty()) {
          sandbox_workspace_val = fs::absolute(entry.package_path).string();
          break;
        }
      }
    }
    if (!sandbox_workspace_val.empty()) {
      message.metadata["sandbox_workspace"] = sandbox_workspace_val;
      Logger::instance().info_f(
          "[SANDBOX] Injected sandbox_workspace=%s for user=%s client=%s agent=%s",
          sandbox_workspace_val.c_str(), user_id.c_str(), client_id.c_str(), agent_id.c_str());
    }
  }

  auto route_t0 = std::chrono::steady_clock::now();
  if (!agent->route_request_message(message)) {
    return build_error(-1, "Failed to route request message", id);
  }
  auto route_t1 = std::chrono::steady_clock::now();
  auto route_ms = std::chrono::duration_cast<std::chrono::milliseconds>(route_t1 - route_t0).count();
  // TIMING 修正（2026-08-10）：此指标只测 route_request_message 段，不含沙箱创建。
  // 旧文案 "sandbox_create+route latency (from USER_INPUT to route complete)" 有误导性——
  // 真实沙箱创建耗时在 [SANDBOX] Created/reused sandbox ... (create_latency=...) 单独测量。
  // 首问总耗时 = create_latency + route latency + LLM/工具链路（见耗时分布分析）。
  Logger::instance().info_f(
      "[TIMING] trace=%s route latency=%lldms (route_request_message only; "
      "sandbox create measured separately as create_latency)",
      trace_id.c_str(), route_ms);

  // Register with ResponsePoller for async notification routing
  poller_.register_request(message_id, client_fd, agent_id, dialogue_id);

  return build_success("{\"message_id\":\"" + json_escape(message_id) + "\""
                     + ",\"status\":\"submitted\"}", id);
}

std::string RpcHandler::handle_agent_poll_response(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string user_id = extract_json_string_local(params, "user_id");
  std::string client_id = extract_json_string_local(params, "client_id");
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

      // Check for error flag
      auto error_it = response.metadata.find("error");
      if (error_it != response.metadata.end() && error_it->second == "true") {
        result_oss << ",\"error\":true";
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

  // complete 判定修正（2026-08-10）：返回式不再要求 msg_count > 0。
  // 原实现 final 消息被 ResponsePoller 消费后返回 complete=false，
  // 导致 poll 端（e2e_helpers.poll_agent_response）永远等不到完成信号。
  // 与循环内 break 使用的 complete（runtime_quiet && queue_empty）保持一致，
  // 只要无运行节点且响应队列空即视为完成，即使消息已被异步消费。
  bool runtime_quiet_final = runtime_.get_running_nodes() == 0;
  bool queue_empty_final = agent->get_response_queue_size() == 0;
  result_oss << "],\"complete\":"
             << (has_final || (runtime_quiet_final && queue_empty_final) ? "true" : "false");

  result_oss << "}";
  return build_success(result_oss.str(), id);
}

std::string RpcHandler::handle_agent_send_control(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string command = extract_json_string_local(params, "command");
  std::string session_id = extract_json_string_local(params, "session_id");
  std::string user_id = extract_json_string_local(params, "user_id");
  std::string dialogue_id = extract_json_string_local(params, "dialogue_id");

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
  if (!dialogue_id.empty()) ctrl_msg.metadata["dialogue_id"] = dialogue_id;

  if (!agent->route_request_message(ctrl_msg)) {
    return build_error(-1, "Failed to route control message", id);
  }

  return build_success("{\"success\":true}", id);
}

// --- Session ---

std::string RpcHandler::handle_session_list(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string filter_user_id = extract_json_string_local(params, "user_id");
  if (agent_id.empty()) return build_error(-32602, "Invalid params: missing agent_id", id);
  auto agent = registry_.find(agent_id);
  if (!agent) return build_error(-2, "Agent not found: " + agent_id, id);

  auto router = agent->get_session_router();
  if (!router) return build_success("{\"sessions\":[]}", id);

  auto route_keys = router->get_active_route_keys();
  int64_t now = now_ms();

  std::ostringstream oss;
  bool first_entry = true;
  oss << "{\"sessions\":[";
  for (const auto& route_key : route_keys) {
    auto session = router->get_session_by_route_key(route_key);
    if (!session) continue;

    // Filter by user_id if requested.
    if (!filter_user_id.empty() && session->get_user_id() != filter_user_id) continue;

    int64_t age_ms = now - session->get_created_time();
    std::string status;
    switch (session->get_state()) {
      case framework::MMAgentSession::SessionState::CREATED:    status = "created"; break;
      case framework::MMAgentSession::SessionState::ACTIVE:     status = "active"; break;
      case framework::MMAgentSession::SessionState::PAUSED:     status = "paused"; break;
      case framework::MMAgentSession::SessionState::COMPLETED:  status = "completed"; break;
      case framework::MMAgentSession::SessionState::TERMINATED: status = "terminated"; break;
      default: status = "unknown"; break;
    }

    if (!first_entry) oss << ",";
    first_entry = false;
    oss << "{\"session_id\":\"" << json_escape(session->get_session_id())
        << "\",\"user_id\":\"" << json_escape(session->get_user_id())
        << "\",\"client_id\":\"" << json_escape(session->get_client_id())
        << "\",\"age_ms\":" << age_ms
        << ",\"status\":\"" << json_escape(status)
        << "\"}";
  }
  oss << "]}";

  return build_success(oss.str(), id);
}

std::string RpcHandler::handle_session_destroy(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string session_id = extract_json_string_local(params, "session_id");
  if (agent_id.empty() || session_id.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id or session_id", id);
  }
  int removed = runtime_.clear_session_data(agent_id, session_id);

  // 清理该 agent 关联的沙箱（AgentFoundry 沙箱机制移植）
  // 委托给 SandboxLifecycle::destroy_by_agent 一次性销毁，避免调用方遍历 list_active 后再调 destroy 触发二次遍历
  if (sandbox_lifecycle_) {
    sandbox_lifecycle_->destroy_by_agent(agent_id);
  }

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

// ---------------------------------------------------------------------------
// DataBus proxy
// ---------------------------------------------------------------------------

std::string RpcHandler::handle_databus_get(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string user_id = extract_json_string_local(params, "user_id");
  std::string client_id = extract_json_string_local(params, "client_id");
  std::string session_id = extract_json_string_local(params, "session_id");
  std::string key = extract_json_string_local(params, "key");

  if (agent_id.empty() || key.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id or key", id);
  }

  auto agent = runtime_.get_agent(agent_id);
  if (!agent) {
    return build_error(-32001, "Agent not found: " + agent_id, id);
  }

  session_id = resolve_session_id(user_id, client_id, session_id, agent);
  if (session_id.empty()) {
    return build_error(-32602, "Invalid params: provide (user_id+client_id) or session_id", id);
  }

  std::string full_key = "/session/" + session_id + "/" + key;
  auto& databus = runtime_.get_databus(agent_id);
  DataVariant value;
  bool exists = databus.get(full_key, value);

  std::ostringstream oss;
  if (exists) {
    // variant_to_json_local() emits a self-contained JSON *value*: strings
    // that are already JSON (e.g. LLM trace payloads) pass through verbatim,
    // while plain/empty strings are quoted + escaped — otherwise non-JSON
    // string values yield malformed frames like {"value":} that the client
    // drops as non-JSON, hanging the request until timeout.
    oss << "{\"exists\":true,\"value\":" << variant_to_json_local(value) << "}";
  } else {
    oss << "{\"exists\":false,\"value\":null}";
  }
  return build_success(oss.str(), id);
}

std::string RpcHandler::handle_databus_list_keys(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string user_id = extract_json_string_local(params, "user_id");
  std::string client_id = extract_json_string_local(params, "client_id");
  std::string session_id = extract_json_string_local(params, "session_id");
  std::string prefix = extract_json_string_local(params, "prefix");

  if (agent_id.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id", id);
  }

  auto agent = runtime_.get_agent(agent_id);
  if (!agent) {
    return build_error(-32001, "Agent not found: " + agent_id, id);
  }

  session_id = resolve_session_id(user_id, client_id, session_id, agent);
  if (session_id.empty()) {
    return build_error(-32602, "Invalid params: provide (user_id+client_id) or session_id", id);
  }

  std::string full_prefix = "/session/" + session_id + "/" + prefix;
  auto& databus = runtime_.get_databus(agent_id);
  auto keys = databus.list_keys_by_prefix(full_prefix);

  // Strip the "/session/{session_id}/" prefix from each key before returning
  std::string strip_prefix = "/session/" + session_id + "/";
  std::ostringstream oss;
  oss << "{\"keys\":[";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i > 0) oss << ",";
    std::string rel = keys[i];
    if (rel.compare(0, strip_prefix.size(), strip_prefix) == 0) {
      rel = rel.substr(strip_prefix.size());
    }
    oss << "\"" << json_escape(rel) << "\"";
  }
  oss << "]}";
  return build_success(oss.str(), id);
}

std::string RpcHandler::handle_databus_remove_by_prefix(const std::string& params, int64_t id) {
  std::string agent_id = extract_json_string_local(params, "agent_id");
  std::string user_id = extract_json_string_local(params, "user_id");
  std::string client_id = extract_json_string_local(params, "client_id");
  std::string session_id = extract_json_string_local(params, "session_id");
  std::string prefix = extract_json_string_local(params, "prefix");

  if (agent_id.empty()) {
    return build_error(-32602, "Invalid params: missing agent_id", id);
  }

  auto agent = runtime_.get_agent(agent_id);
  if (!agent) {
    return build_error(-32001, "Agent not found: " + agent_id, id);
  }

  session_id = resolve_session_id(user_id, client_id, session_id, agent);
  if (session_id.empty()) {
    return build_error(-32602, "Invalid params: provide (user_id+client_id) or session_id", id);
  }

  std::string full_prefix = "/session/" + session_id + "/" + prefix;
  auto& databus = runtime_.get_databus(agent_id);
  int removed = databus.remove_by_prefix(full_prefix);

  std::ostringstream oss;
  oss << "{\"removed\":" << removed << ",\"prefix\":\"" << json_escape(prefix) << "\"}";
  Logger::instance().info_f("[DATABUS_CLEANUP] Removed %d keys for agent=%s session=%s prefix=%s",
                            removed, agent_id.c_str(), session_id.c_str(), prefix.c_str());
  return build_success(oss.str(), id);
}

// --- Sandbox ---

std::string RpcHandler::handle_sandbox_read_file(const std::string& params, int64_t id) {
  // 参数：sandbox_id（沙箱唯一标识），path（文件相对路径）
  std::string sandbox_id = extract_json_string_local(params, "sandbox_id");
  std::string file_path = extract_json_string_local(params, "path");
  if (sandbox_id.empty() || file_path.empty()) {
    return build_error(-32602, "Invalid params: need sandbox_id and path", id);
  }

  // 1. 运行时通道：查活沙箱 → POST /exec (tool=read_file) 转发
  // ⚠️ 当前 e2b 场景不通（2026-08-09 确认）：endpoint 是沙箱内 HTTP server 的
  // cubeproxy 外部路径（http://host:80/sandbox/{id}/8080），而 D-001 实测
  // cubeproxy 不转发 8080 端口（HTTP 502）。e2b 下此通道实际失败，调用方
  // （bridge sandbox_files.py）会回退 ArtifactStore 快照通道。仅 Local/Docker
  // 模式（HTTPExecutor 直连沙箱内 HTTP server）此通道可用。
  auto active = session_manager_.list_active();
  for (auto& sb : active) {
    if (sb->id() == sandbox_id) {
      std::string ep = sb->endpoint().rfind('/') == sb->endpoint().size() - 1
          ? sb->endpoint().substr(0, sb->endpoint().size() - 1)
          : sb->endpoint();
      // 通过 POST /exec 调用沙箱内 HTTP server 的 read_file 工具
      // 对齐 contracts/sandbox-http-server.md POST /exec 契约
      // 用 rapidjson 构造 body，转义由库自动处理（避免手写转义漏覆盖控制字符）
      rapidjson::Document req_doc;
      req_doc.SetObject();
      rapidjson::Document::AllocatorType& alloc = req_doc.GetAllocator();
      // rapidjson AddMember 需要 Value 右值，不能直接传 const char* / std::string
      rapidjson::Value tool_val;
      tool_val.SetString("read_file", alloc);
      req_doc.AddMember("tool", tool_val.Move(), alloc);
      rapidjson::Value tool_params(rapidjson::kObjectType);
      rapidjson::Value path_val;
      path_val.SetString(file_path.c_str(), static_cast<rapidjson::SizeType>(file_path.size()), alloc);
      tool_params.AddMember("path", path_val.Move(), alloc);
      req_doc.AddMember("tool_params", tool_params.Move(), alloc);
      req_doc.AddMember("opts", rapidjson::Value(rapidjson::kObjectType).Move(), alloc);
      std::string body = rj_stringify(req_doc);
      std::string curl_cmd = "curl -s -m 10 -X POST -H 'Content-Type: application/json' -d '" + body + "' '" + ep + "/exec' 2>/dev/null";
      FILE* pipe = popen(curl_cmd.c_str(), "r");
      if (!pipe) {
        return build_success("{\"found\":false,\"source\":\"runtime\",\"error\":\"curl failed\"}", id);
      }
      char buf[4096];
      std::string result;
      while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result += buf;
      }
      pclose(pipe);

      // 解析 ExecResult JSON
      std::string exit_code_str = extract_json_string_local(result, "exit_code");
      std::string content = extract_json_string_local(result, "stdout");
      std::string err = extract_json_string_local(result, "stderr");
      if (!exit_code_str.empty() && exit_code_str != "0") {
        return build_success("{\"found\":false,\"source\":\"runtime\",\"error\":\"" + json_escape(err) + "\"}", id);
      }
      if (!content.empty()) {
        // 对内容做 JSON 转义（包含在 success JSON 的 content 字段中）
        std::string escaped;
        for (char c : content) {
          if (c == '"' || c == '\\') { escaped += '\\'; escaped += c; }
          else if (c == '\n') { escaped += "\\n"; }
          else if (c == '\r') { escaped += "\\r"; }
          else if (c == '\t') { escaped += "\\t"; }
          else { escaped += c; }
        }
        return build_success("{\"found\":true,\"source\":\"runtime\",\"content\":\"" + escaped + "\"}", id);
      }
      // 运行时通道无内容，回退到快照
      break;
    }
  }

  // 2. 快照通道（回退，仅 Local/Docker 模式）
  // E2B 模式不经过此路径（直接返回未找到）
  return build_success("{\"found\":false,\"source\":\"snapshot\",\"error\":\"not found\"}", id);
}

std::string RpcHandler::handle_sandbox_upload_file(const std::string& params, int64_t id) {
  // 对称 handle_sandbox_read_file：Bridge → daemon → 沙箱内 HTTP server POST /upload
  // 契约：contracts/bridge-sandbox-files.md RPC sandbox.upload_file
  // 设计权威源：agent-package-structure.md §3.6 用户上传资产流转
  //
  // 与 read_file 的关键差异：
  //   1. POST /upload 端点（二进制专属）而非 /exec + write_file
  //   2. body 是 {path, content_b64} 而非 {tool, tool_params, opts}
  //   3. 强制 path 以 workspace/upload/ 为前缀（守 artifact 边界 I-6 + 鲛穿越）
  //   4. 不走快照回退通道——快照是只读的，写入仅走运行时通道
  std::string sandbox_id = extract_json_string_local(params, "sandbox_id");
  std::string file_path = extract_json_string_local(params, "path");
  std::string content_b64 = extract_json_string_local(params, "content_b64");
  if (sandbox_id.empty() || file_path.empty() || content_b64.empty()) {
    return build_error(-32602, "Invalid params: need sandbox_id, path and content_b64", id);
  }

  // 守 artifact 边界 I-6 + 会话级路径校验：只接受 workspace/{user_id}_{client_id}/upload/{filename}
  {
    std::string upload_err = sandbox::SandboxHelpers::validate_upload_path(file_path);
    if (!upload_err.empty()) {
      return build_success("{\"written\":false,\"error\":\"PATH_NOT_IN_UPLOAD_DIR\","
                           "\"message\":\"" + json_escape(upload_err) + "\","
                           "\"path\":\"" + json_escape(file_path) + "\"}", id);
    }
  }

  // 1. 运行时通道：查活沙箱 → POST /upload 转发
  // ⚠️ 当前 e2b 场景不通（2026-08-09 确认）：与 read_file 同因——endpoint 是
  // 沙箱内 HTTP server 的 cubeproxy 外部路径（http://host:80/sandbox/{id}/8080），
  // D-001 实测 cubeproxy 不转发 8080（HTTP 502），e2b 下此通道实际失败。
  // 且 upload 不走快照回退（快照只读），e2b 下用户上传资产实际不可达。
  // 仅 Local/Docker 模式（HTTPExecutor 直连沙箱内 HTTP server）此通道可用。
  auto active = session_manager_.list_active();
  for (auto& sb : active) {
    if (sb->id() == sandbox_id) {
      std::string ep = sb->endpoint().rfind('/') == sb->endpoint().size() - 1
          ? sb->endpoint().substr(0, sb->endpoint().size() - 1)
          : sb->endpoint();
      // 对齐 contracts/sandbox-http-server.md POST /upload 契约
      // 用 rapidjson 构造 body，转义由库自动处理（避免手写转义漏覆盖控制字符）
      rapidjson::Document req_doc;
      req_doc.SetObject();
      rapidjson::Document::AllocatorType& alloc = req_doc.GetAllocator();
      rapidjson::Value path_val;
      path_val.SetString(file_path.c_str(), static_cast<rapidjson::SizeType>(file_path.size()), alloc);
      req_doc.AddMember("path", path_val.Move(), alloc);
      rapidjson::Value b64_val;
      b64_val.SetString(content_b64.c_str(), static_cast<rapidjson::SizeType>(content_b64.size()), alloc);
      req_doc.AddMember("content_b64", b64_val.Move(), alloc);
      std::string body = rj_stringify(req_doc);
      // -m 30：二进制图可能几 MB，给足超时（read_file 用 -m 10 是因为文本小）
      std::string curl_cmd = "curl -s -m 30 -X POST -H 'Content-Type: application/json' -d '" + body + "' '" + ep + "/upload' 2>/dev/null";
      FILE* pipe = popen(curl_cmd.c_str(), "r");
      if (!pipe) {
        return build_success("{\"written\":false,\"error\":\"INTERNAL\",\"message\":\"curl failed\"}", id);
      }
      char buf[4096];
      std::string result;
      while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result += buf;
      }
      pclose(pipe);

      // 解析沙箱内 server 响应（{written, bytes_written, path} 或 {error: {...}})
      // 透传给 Bridge，由 Bridge 决定如何处理错误
      std::string written = extract_json_string_local(result, "written");
      if (written == "true") {
        // 成功：透传完整响应（含 bytes_written + path）
        return build_success(result, id);
      }
      // 失败：沙箱内 server 返回错误 JSON，透传给 Bridge
      std::string err_code = extract_json_string_local(result, "code");
      if (!err_code.empty()) {
        return build_success("{\"written\":false,\"error\":\"" + json_escape(err_code) +
                             "\",\"message\":\"" + json_escape(extract_json_string_local(result, "message")) +
                             "\",\"path\":\"" + json_escape(file_path) + "\"}", id);
      }
      // curl 失败或响应不可解析
      return build_success("{\"written\":false,\"error\":\"INTERNAL\","
                           "\"message\":\"sandbox HTTP server unreachable or invalid response: " +
                           json_escape(result) + "\"}", id);
    }
  }

  // 2. 沙箱不存在——upload 不走快照回退通道（快照是只读的）
  return build_success("{\"written\":false,\"error\":\"SANDBOX_NOT_FOUND\","
                       "\"message\":\"sandbox not active: " + json_escape(sandbox_id) + "\"}", id);
}

std::string RpcHandler::handle_sandbox_list(const std::string& params, int64_t id) {
  // 沙箱列表（E1：透传 SandboxLifecycle::list_active()，三模式统一）
  // 参数：可选 state 过滤（RUNNING/STOPPED/RECYCLING，不区分大小写）
  // 响应：{"sandboxes": [{sandbox_id, state, mode, owner:{user_id,client_id,agent_id},
  //                        endpoint, workdir}], "total": N}
  // 设计权威源：docs/design/features/sandbox-management.md 演进方向 E1
  std::string state_filter = extract_json_string_local(params, "state");
  for (char& c : state_filter) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }

  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Document::AllocatorType& alloc = doc.GetAllocator();
  rapidjson::Value arr(rapidjson::kArrayType);

  if (sandbox_lifecycle_) {
    // mode：透传配置（local/docker/e2b），与 bridge 观测侧对齐
    std::string mode = sandbox_lifecycle_->config().sandbox_config.mode;
    auto active = sandbox_lifecycle_->list_active();
    for (const auto& sb : active) {
      if (!sb) continue;
      std::string state_str;
      switch (sb->state()) {
        case sandbox::SandboxState::RUNNING:   state_str = "RUNNING"; break;
        case sandbox::SandboxState::STOPPED:   state_str = "STOPPED"; break;
        case sandbox::SandboxState::RECYCLING: state_str = "RECYCLING"; break;
      }
      if (!state_filter.empty() && state_str != state_filter) continue;

      rapidjson::Value item(rapidjson::kObjectType);
      rapidjson::Value v;
      v.SetString(sb->id().c_str(), static_cast<rapidjson::SizeType>(sb->id().size()), alloc);
      item.AddMember("sandbox_id", v.Move(), alloc);
      v.SetString(state_str.c_str(), static_cast<rapidjson::SizeType>(state_str.size()), alloc);
      item.AddMember("state", v.Move(), alloc);
      v.SetString(mode.c_str(), static_cast<rapidjson::SizeType>(mode.size()), alloc);
      item.AddMember("mode", v.Move(), alloc);

      const sandbox::SandboxKey& owner = sb->owner();
      rapidjson::Value owner_obj(rapidjson::kObjectType);
      v.SetString(owner.user_id.c_str(), static_cast<rapidjson::SizeType>(owner.user_id.size()), alloc);
      owner_obj.AddMember("user_id", v.Move(), alloc);
      v.SetString(owner.client_id.c_str(), static_cast<rapidjson::SizeType>(owner.client_id.size()), alloc);
      owner_obj.AddMember("client_id", v.Move(), alloc);
      v.SetString(owner.agent_id.c_str(), static_cast<rapidjson::SizeType>(owner.agent_id.size()), alloc);
      owner_obj.AddMember("agent_id", v.Move(), alloc);
      item.AddMember("owner", owner_obj.Move(), alloc);

      const std::string& ep = sb->endpoint();
      if (!ep.empty()) {
        v.SetString(ep.c_str(), static_cast<rapidjson::SizeType>(ep.size()), alloc);
        item.AddMember("endpoint", v.Move(), alloc);
      }
      const std::string& wd = sb->workdir();
      if (!wd.empty()) {
        v.SetString(wd.c_str(), static_cast<rapidjson::SizeType>(wd.size()), alloc);
        item.AddMember("workdir", v.Move(), alloc);
      }
      // FR-009：最近活跃时间戳（steady_clock ms，GC 闲置判定依据）。
      // bridge 侧 prune 用它计算闲置时长（与 daemon GC 同一语义）。
      item.AddMember("last_active_at", sb->last_active_at(), alloc);
      arr.PushBack(item.Move(), alloc);
    }
  }

  int total = static_cast<int>(arr.Size());
  doc.AddMember("sandboxes", arr.Move(), alloc);
  doc.AddMember("total", total, alloc);
  // server_now_ms：daemon 当前 steady_clock 时间（与 last_active_at 同基准）。
  // bridge 用它计算闲置时长（idle = server_now_ms - last_active_at），
  // 避免跨进程时钟基准假设（F11 不猜接口，时间基准由权威源提供）。
  doc.AddMember("server_now_ms", now_ms(), alloc);
  return build_success(rj_stringify(doc), id);
}

std::string RpcHandler::handle_sandbox_destroy(const std::string& params, int64_t id) {
  // 沙箱销毁（E2：透传 SandboxLifecycle::destroy()，幂等单删）
  // 参数：sandbox_id（沙箱唯一标识）
  // 响应：{"sandbox_id": ..., "destroyed": true/false, "already_gone": true/false}
  // 幂等语义（B7）：未命中视为 already_gone=true（对齐 E2B kill 语义）
  // 设计权威源：docs/design/features/sandbox-management.md 演进方向 E2
  std::string sandbox_id = extract_json_string_local(params, "sandbox_id");
  if (sandbox_id.empty()) {
    return build_error(-32602, "Invalid params: need sandbox_id", id);
  }
  if (!sandbox_lifecycle_) {
    // mode=none：无沙箱子系统，任何 id 都视为已消失
    return build_success(
        "{\"sandbox_id\":\"" + json_escape(sandbox_id) +
        "\",\"destroyed\":false,\"already_gone\":true}", id);
  }

  // 先确认存在性（list_active 匹配）——未命中则幂等已消失
  bool found = false;
  for (const auto& sb : sandbox_lifecycle_->list_active()) {
    if (sb && sb->id() == sandbox_id) {
      found = true;
      break;
    }
  }
  if (!found) {
    return build_success(
        "{\"sandbox_id\":\"" + json_escape(sandbox_id) +
        "\",\"destroyed\":false,\"already_gone\":true}", id);
  }

  sandbox_lifecycle_->destroy(sandbox_id);  // 幂等：内部已处理已销毁情况
  return build_success(
      "{\"sandbox_id\":\"" + json_escape(sandbox_id) +
      "\",\"destroyed\":true,\"already_gone\":false}", id);
}

std::string RpcHandler::handle_sandbox_pause(const std::string& params, int64_t id) {
  // 沙箱暂停（B21：RUNNING→STOPPED 可逆，非终态）
  // 参数：sandbox_id
  // 响应：{"sandbox_id": ..., "paused": true/false}（false = 已暂停，幂等）
  std::string sandbox_id = extract_json_string_local(params, "sandbox_id");
  if (sandbox_id.empty()) {
    return build_error(-32602, "Invalid params: need sandbox_id", id);
  }
  if (!sandbox_lifecycle_) {
    return build_error(-32603, "sandbox subsystem disabled (mode=none)", id);
  }
  for (const auto& sb : sandbox_lifecycle_->list_active()) {
    if (sb && sb->id() == sandbox_id) {
      if (sb->state() == sandbox::SandboxState::STOPPED) {
        // 已暂停：幂等成功
        return build_success(
            "{\"sandbox_id\":\"" + json_escape(sandbox_id) + "\",\"paused\":false}", id);
      }
      try {
        sb->pause();
      } catch (const std::exception& e) {
        // SDK/平台不支持 pause（如 e2b 平台无 pause 端点）——返回可定位错误（M3）
        Logger::instance().warn_f("[SANDBOX] pause failed sandbox=%s: %s",
                                  sandbox_id.c_str(), e.what());
        return build_error(-32603,
                           std::string("sandbox.pause failed: ") + e.what(), id);
      }
      return build_success(
          "{\"sandbox_id\":\"" + json_escape(sandbox_id) + "\",\"paused\":true}", id);
    }
  }
  return build_error(-32602,
                     "sandbox not found: " + json_escape(sandbox_id), id);
}

std::string RpcHandler::handle_sandbox_resume(const std::string& params, int64_t id) {
  // 沙箱恢复（B21：STOPPED→RUNNING 可逆）
  // 参数：sandbox_id
  // 响应：{"sandbox_id": ..., "resumed": true/false}（false = 已运行，幂等）
  std::string sandbox_id = extract_json_string_local(params, "sandbox_id");
  if (sandbox_id.empty()) {
    return build_error(-32602, "Invalid params: need sandbox_id", id);
  }
  if (!sandbox_lifecycle_) {
    return build_error(-32603, "sandbox subsystem disabled (mode=none)", id);
  }
  for (const auto& sb : sandbox_lifecycle_->list_active()) {
    if (sb && sb->id() == sandbox_id) {
      if (sb->state() == sandbox::SandboxState::RUNNING) {
        // 已运行：幂等成功
        return build_success(
            "{\"sandbox_id\":\"" + json_escape(sandbox_id) + "\",\"resumed\":false}", id);
      }
      if (sb->state() == sandbox::SandboxState::RECYCLING) {
        return build_error(-32602,
                           "sandbox in terminal state RECYCLING: " + json_escape(sandbox_id), id);
      }
      try {
        sb->resume();
      } catch (const std::exception& e) {
        Logger::instance().warn_f("[SANDBOX] resume failed sandbox=%s: %s",
                                  sandbox_id.c_str(), e.what());
        return build_error(-32603,
                           std::string("sandbox.resume failed: ") + e.what(), id);
      }
      return build_success(
          "{\"sandbox_id\":\"" + json_escape(sandbox_id) + "\",\"resumed\":true}", id);
    }
  }
  return build_error(-32602,
                     "sandbox not found: " + json_escape(sandbox_id), id);
}

std::string RpcHandler::handle_sandbox_snapshot(const std::string& params, int64_t id) {
  // 快照生成——透传 SandboxSnapshots::snapshot(sandbox_id, sandbox_mode)
  // 参数：sandbox_id
  // 响应：Snapshot 实体 JSON（snapshot_id/sandbox_id/created_at/store_path/size_bytes/sandbox_mode）
  std::string sandbox_id = extract_json_string_local(params, "sandbox_id");
  if (sandbox_id.empty()) {
    return build_error(-32602, "Invalid params: need sandbox_id", id);
  }
  if (!sandbox_snapshots_) {
    return build_error(-32603, "snapshot subsystem disabled (mode=none)", id);
  }
  // 由 SandboxSnapshots::snapshot 前置校验：sandbox_id 必须在活沙箱列表中
  std::string sandbox_mode;
  for (const auto& sb : sandbox_lifecycle_->list_active()) {
    if (sb && sb->id() == sandbox_id) {
      sandbox_mode = sb->sandbox_mode();
      break;
    }
  }
  if (sandbox_mode.empty() && sandbox_lifecycle_) {
    // 未在活沙箱列表命中——让 snapshot 抛"not active"（统一错误语义）
    // 空 mode 会按 Local/Docker 分支处理，但前置会先抛 not active
  }
  try {
    daemon::Snapshot snap = sandbox_snapshots_->snapshot(sandbox_id, sandbox_mode);
    std::string resp =
        "{\"snapshot_id\":\"" + json_escape(snap.snapshot_id) +
        "\",\"sandbox_id\":\"" + json_escape(snap.sandbox_id) +
        "\",\"created_at\":" + std::to_string(snap.created_at) +
        ",\"store_path\":\"" + json_escape(snap.store_path) +
        "\",\"size_bytes\":" + std::to_string(snap.size_bytes) +
        ",\"sandbox_mode\":\"" + json_escape(snap.sandbox_mode) + "\"}";
    return build_success(resp, id);
  } catch (const std::exception& e) {
    Logger::instance().warn_f("[SANDBOX] snapshot failed sandbox=%s: %s",
                              sandbox_id.c_str(), e.what());
    return build_error(-32603, std::string("sandbox.snapshot failed: ") + e.what(), id);
  }
}

std::string RpcHandler::handle_sandbox_snapshot_delete(const std::string& params, int64_t id) {
  // 快照单条删除——透传 SandboxSnapshots::delete_snapshot（幂等）
  // 参数：snapshot_id
  // 响应：{"snapshot_id": ..., "deleted": true/false, "already_gone": true/false}
  std::string snapshot_id = extract_json_string_local(params, "snapshot_id");
  if (snapshot_id.empty()) {
    return build_error(-32602, "Invalid params: need snapshot_id", id);
  }
  if (!sandbox_snapshots_) {
    return build_error(-32603, "snapshot subsystem disabled (mode=none)", id);
  }
  try {
    bool deleted = sandbox_snapshots_->delete_snapshot(snapshot_id);
    return build_success(
        "{\"snapshot_id\":\"" + json_escape(snapshot_id) +
        "\",\"deleted\":" + (deleted ? "true" : "false") +
        ",\"already_gone\":" + (deleted ? "false" : "true") + "}", id);
  } catch (const std::exception& e) {
    Logger::instance().warn_f("[SANDBOX] snapshot_delete failed snapshot=%s: %s",
                              snapshot_id.c_str(), e.what());
    return build_error(-32603, std::string("sandbox.snapshot_delete failed: ") + e.what(), id);
  }
}

std::string RpcHandler::handle_sandbox_snapshot_list(const std::string& params, int64_t id) {
  // 快照列表——透传 SandboxSnapshots::list_snapshots()
  // 参数：可选 sandbox_id 过滤
  // 响应：{"snapshots": [Snapshot...], "total": N}
  if (!sandbox_snapshots_) {
    return build_error(-32603, "snapshot subsystem disabled (mode=none)", id);
  }
  std::string filter_sandbox_id = extract_json_string_local(params, "sandbox_id");
  std::vector<daemon::Snapshot> all = sandbox_snapshots_->list_snapshots();
  std::string items;
  int count = 0;
  for (const auto& s : all) {
    if (!filter_sandbox_id.empty() && s.sandbox_id != filter_sandbox_id) {
      continue;
    }
    if (!items.empty()) items += ",";
    items += "{\"snapshot_id\":\"" + json_escape(s.snapshot_id) +
             "\",\"sandbox_id\":\"" + json_escape(s.sandbox_id) +
             "\",\"created_at\":" + std::to_string(s.created_at) +
             ",\"store_path\":\"" + json_escape(s.store_path) +
             "\",\"size_bytes\":" + std::to_string(s.size_bytes) +
             ",\"sandbox_mode\":\"" + json_escape(s.sandbox_mode) + "\"}";
    ++count;
  }
  return build_success("{\"snapshots\":[" + items + "],\"total\":" + std::to_string(count) + "}", id);
}

std::string RpcHandler::_resolve_session_upload_dir(const std::string& agent_id,
                                                    const std::string& user_id,
                                                    const std::string& client_id) {
  // 推算会话级用户上传目录宿主机绝对路径（与 bridge content_serializer._compute_upload_dir 同构）
  // 真相源：agent package 路径在 config.agents[i].package_path，agent_id 匹配 name_override 或 basename
  // 返回：{abs_package_path}/workspace/{user_id}_{client_id}/upload/
  // 找不到对应 agent 或三元组不完整 → 返回空（调用方按空跳过）
  if (agent_id.empty() || user_id.empty() || client_id.empty()) {
    return "";
  }
  if (!sandbox_lifecycle_) {
    return "";  // mode=none 时 sandbox_lifecycle_ 为 nullptr，无沙箱无上传
  }
  const auto& config = sandbox_lifecycle_->config();
  namespace fs = std::filesystem;
  for (const auto& entry : config.agents) {
    // package_path 可能为相对路径（如 "examples/agent/outfit-assistant"）
    std::string pkg_path = fs::absolute(entry.package_path).string();
    // 匹配 agent_id：name_override 优先，否则用 basename
    std::string name = entry.name_override;
    if (name.empty()) {
      auto pos = pkg_path.find_last_of('/');
      name = (pos != std::string::npos) ? pkg_path.substr(pos + 1) : pkg_path;
    }
    if (name != agent_id) continue;
    // 同构 bridge 的 _compute_upload_dir 路径约定
    return pkg_path + "/workspace/" + artifact_routing_id(user_id, client_id) +
           "/upload";
  }
  return "";
}

}  // namespace daemon
}  // namespace agentflow
