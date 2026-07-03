// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#ifndef AGENTFLOW_EXAMPLES_CHAT_BRIDGE_SUPPORT_H
#define AGENTFLOW_EXAMPLES_CHAT_BRIDGE_SUPPORT_H

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "agent/msgque.h"
#include "runtime/agent_runtime.h"
#include "runtime/databus/databus.h"

namespace agentflow::examples {

inline std::string json_escape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for(char c : input) {
    switch(c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

inline std::string utc_iso8601_now() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

inline std::string sanitize_session_id(const std::string& session_id) {
  std::string safe = session_id;
  for(char& c : safe) {
    if(c == '/' || c == '\\' || c == ':') {
      c = '_';
    }
  }
  return safe;
}

inline std::string trim_copy(const std::string& input) {
  const auto begin = std::find_if_not(input.begin(), input.end(), [](unsigned char ch) {
    return std::isspace(ch);
  });
  const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
    return std::isspace(ch);
  }).base();
  if(begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

inline std::string ascii_lower_copy(const std::string& input) {
  std::string lowered = input;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
}

inline bool is_greeting_text(const std::string& text) {
  const std::string trimmed = trim_copy(text);
  if(trimmed.empty()) {
    return false;
  }
  const std::string lowered = ascii_lower_copy(trimmed);
  static const std::vector<std::string> greetings = {
      "hi", "hello", "hey", "你好", "您好", "嗨", "哈喽", "在吗", "在么"};
  return std::find(greetings.begin(), greetings.end(), trimmed) != greetings.end() ||
         std::find(greetings.begin(), greetings.end(), lowered) != greetings.end();
}

inline bool is_summary_request(const std::string& text) {
  static const std::vector<std::string> tokens = {
      "总结", "概括", "摘要", "提炼", "复盘", "整理", "上条消息", "上一条"};
  for(const auto& token : tokens) {
    if(text.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

inline std::string summarize_previous_message(const std::string& history_json,
                                              const std::string& user_text) {
  if(!is_summary_request(user_text) || history_json.empty()) {
    return "";
  }
  std::vector<std::string> contents;
  size_t pos = 0;
  while(true) {
    const size_t role_key = history_json.find("\"role\":\"", pos);
    if(role_key == std::string::npos) {
      break;
    }
    pos = role_key + 8;
    std::string role;
    while(pos < history_json.size()) {
      const char c = history_json[pos++];
      if(c == '"') {
        break;
      }
      role += c;
    }
    const size_t content_key = history_json.find("\"content\":\"", pos);
    if(content_key == std::string::npos) {
      break;
    }
    pos = content_key + 11;
    std::string content;
    bool escaped = false;
    while(pos < history_json.size()) {
      char c = history_json[pos++];
      if(escaped) {
        content += c;
        escaped = false;
        continue;
      }
      if(c == '\\') {
        escaped = true;
        continue;
      }
      if(c == '"') {
        break;
      }
      content += c;
    }
    if(role == "user" && !content.empty() && content != user_text) {
      contents.push_back(content);
    }
  }
  if(contents.empty()) {
    return "";
  }
  return "上条消息主要内容是：" + contents.back();
}

inline std::string build_conversation_response(const std::string& user_text,
                                               const std::string& history_json) {
  if(is_greeting_text(user_text)) {
    return "你好！我可以帮你做总结、检索、日期时间计算和任务规划。";
  }
  return summarize_previous_message(history_json, user_text);
}

inline std::string build_direct_response(const std::string& user_text,
                                         const std::string& history_json) {
  std::string conversation_response = build_conversation_response(user_text, history_json);
  if(!conversation_response.empty()) {
    return conversation_response;
  }
  return "我暂时没有选到合适的技能，请换一种说法，或明确告诉我要总结、检索、算日期，还是做规划。";
}

class ConversationStore {
 public:
  explicit ConversationStore(const std::string& root) : root_(root) {
    std::filesystem::create_directories(root_);
  }

  void append(const std::string& session_id, const std::string& role, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream out(file_for(session_id), std::ios::app);
    out << "{\"ts\":\"" << utc_iso8601_now() << "\",\"session_id\":\""
        << json_escape(session_id) << "\",\"role\":\"" << json_escape(role)
        << "\",\"content\":\"" << json_escape(content) << "\"}\n";
  }

  std::string recent_as_json(const std::string& session_id, size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream in(file_for(session_id));
    if(!in.is_open()) {
      return "[]";
    }
    std::vector<std::string> lines;
    std::string line;
    while(std::getline(in, line)) {
      if(!line.empty()) {
        lines.push_back(line);
      }
    }
    if(lines.empty()) {
      return "[]";
    }
    const size_t start = lines.size() > limit ? (lines.size() - limit) : 0;
    std::ostringstream oss;
    oss << "[";
    for(size_t i = start; i < lines.size(); ++i) {
      if(i > start) {
        oss << ",";
      }
      oss << lines[i];
    }
    oss << "]";
    return oss.str();
  }

  std::filesystem::path session_file(const std::string& session_id) const {
    return file_for(session_id);
  }

 private:
  std::filesystem::path file_for(const std::string& session_id) const {
    return root_ / (sanitize_session_id(session_id) + ".jsonl");
  }

  std::filesystem::path root_;
  std::mutex mutex_;
};

class ChatAgentBridge {
 public:
  explicit ChatAgentBridge(const std::string& package_path) : package_path_(package_path) {}

  ~ChatAgentBridge() {
    if(runtime_) {
      runtime_->stop();
    }
    if(agent_) {
      agent_->stop();
    }
  }

  bool initialize() {
    agent_ = framework::MMAgent::create_from_package(package_path_);
    if(!agent_ || !agent_->initialize()) {
      return false;
    }
    runtime_ = std::make_unique<runtime::MMAgentRuntime>();
    if(!runtime_->register_agent(agent_) || !runtime_->start() || !agent_->start()) {
      return false;
    }
    return true;
  }

  std::string execute(const std::string& workflow_id, const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& bus = runtime_->get_databus(agent_->get_id());
    const std::string agent_id = agent_->get_id();

    std::string user_text;
    std::string client_session_id = "default";
    std::string history_json = "[]";
    {
      std::istringstream ss(payload);
      std::string line;
      while(std::getline(ss, line)) {
        if(line.rfind("user=", 0) == 0) {
          user_text = line.substr(5);
        } else if(line.rfind("session=", 0) == 0) {
          client_session_id = line.substr(8);
          if(client_session_id.empty()) {
            client_session_id = "default";
          }
        } else if(line.rfind("history=", 0) == 0) {
          history_json = line.substr(8);
        }
      }
      if(user_text.empty()) {
        user_text = payload;
      }
    }

    for(const std::string& key : {"final_response", "response", "output", "result", "dispatcher_done",
                                  "coordinator_done", "orchestration_plan", "selected_skills",
                                  "partial_skill_results",
                                  "session-dispatcher-workflow.merge_candidates.selected_skills",
                                  "session-coordinator-workflow.merge_plan.orchestration_plan",
                                  "session-coordinator-workflow.merge_plan.final_response"}) {
      bus.remove(key);
    }
    bus.put("user_text", user_text);
    bus.put("input", payload);
    bus.put("session:" + client_session_id + ":history_json", history_json);
    std::ostringstream context_json;
    context_json << "{\"session_id\":\"" << json_escape(client_session_id) << "\"," 
                 << "\"user_text\":\"" << json_escape(user_text) << "\"," 
                 << "\"history_json\":" << history_json << "}";
    bus.put("session:" + client_session_id + ":context", context_json.str());

    framework::MMAgentMsgQue::Message msg;
    msg.message_id = "bridge-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    msg.type = framework::MMAgentMsgQue::MessageType::TASK_REQUEST;
    msg.sender_id = "chat_bridge";
    msg.content = user_text;
    msg.metadata["user_id"] = "chat_user";
    msg.metadata["client_id"] = client_session_id;
    msg.metadata["session_id"] = "0";
    msg.metadata["task_type"] = "dispatcher";
    if(!workflow_id.empty()) {
      msg.metadata["workflow_id"] = workflow_id;
    }
    agent_->route_request_message(msg);
    agent_->process_all_sessions_until_idle();

    int max_wait_sec = 45;
    if(const char* env = std::getenv("CHAT_WORKFLOW_TIMEOUT_SEC"); env && *env) {
      try {
        max_wait_sec = std::max(5, std::stoi(env));
      } catch(...) {
      }
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(max_wait_sec);
    bool dispatcher_done = false;
    bool dispatcher_done_seen = false;
    std::string selected_skills;
    std::chrono::steady_clock::time_point dispatcher_done_at;
    while(std::chrono::steady_clock::now() < deadline) {
      DataVariant v;
      if(bus.get("dispatcher_done", v)) {
        if(!dispatcher_done_seen) {
          dispatcher_done_at = std::chrono::steady_clock::now();
          dispatcher_done_seen = true;
        }
        dispatcher_done = true;
      }
      if(bus.get("selected_skills", v) && std::holds_alternative<std::string>(v)) {
        selected_skills = std::get<std::string>(v);
      }
      if(dispatcher_done && !selected_skills.empty()) {
        break;
      }
      if(dispatcher_done_seen &&
         (std::chrono::steady_clock::now() - dispatcher_done_at) >= std::chrono::milliseconds(250)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if(!dispatcher_done) {
      return "[error] dispatcher workflow timeout";
    }
    if(selected_skills.empty()) {
      std::string direct_response = build_direct_response(user_text, history_json);
      bus.put("final_response", direct_response);
      return direct_response;
    }

    bus.remove("orchestration_plan");
    runtime_->execute_workflow(agent_id, "session-coordinator-workflow",
                               {{"session_id", client_session_id}, {"user_text", user_text}});

    auto coord_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::string orchestration_plan;
    while(std::chrono::steady_clock::now() < coord_deadline) {
      DataVariant v;
      if(bus.get("orchestration_plan", v) && std::holds_alternative<std::string>(v)) {
        orchestration_plan = std::get<std::string>(v);
        if(!orchestration_plan.empty()) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    static const std::map<std::string, std::string> kSkillWorkflows = {
        {"skill_search", "skill-search-workflow"},
        {"skill_summary", "skill-summary-workflow"},
        {"skill_date", "skill-date-workflow"},
        {"skill_delegate", "skill-delegate-workflow"},
    };

    auto parse_str = [](const std::string& json, const std::string& key) -> std::string {
      const std::string needle = "\"" + key + "\":\"";
      auto pos = json.find(needle);
      if(pos == std::string::npos) return "";
      pos += needle.size();
      std::string val;
      while(pos < json.size() && json[pos] != '"') {
        if(json[pos] == '\\' && pos + 1 < json.size()) {
          ++pos;
        }
        val += json[pos++];
      }
      return val;
    };

    struct SkillExecution {
      std::string session_id;
      std::string skill;
      std::string workflow_id;
      std::vector<std::string> answer_keys;
      std::vector<std::string> status_keys;
      std::vector<std::string> can_handle_keys;
      std::vector<std::string> grounded_keys;
      std::vector<std::string> confidence_keys;
      std::vector<std::string> reason_keys;
    };

    std::vector<SkillExecution> started_skills;
    if(!orchestration_plan.empty()) {
      size_t pos = 0;
      while(pos < orchestration_plan.size()) {
        const size_t start_brace = orchestration_plan.find('{', pos);
        if(start_brace == std::string::npos) break;
        const size_t end_brace = orchestration_plan.find('}', start_brace);
        if(end_brace == std::string::npos) break;
        const std::string entry = orchestration_plan.substr(start_brace, end_brace - start_brace + 1);
        pos = end_brace + 1;

        const std::string action = parse_str(entry, "action");
        const std::string skill = parse_str(entry, "skill");
        const std::string session_id = parse_str(entry, "session_id");
        if(action != "start") continue;
        auto wf_it = kSkillWorkflows.find(skill);
        if(wf_it == kSkillWorkflows.end()) continue;

        SkillExecution started_skill;
        started_skill.session_id = session_id;
        started_skill.skill = skill;
        started_skill.workflow_id = wf_it->second;
        started_skill.answer_keys = {wf_it->second + ".n1.answer_text", "answer_text"};
        started_skill.status_keys = {wf_it->second + ".n1.status", "status"};
        started_skill.can_handle_keys = {wf_it->second + ".n1.can_handle", "can_handle"};
        started_skill.grounded_keys = {wf_it->second + ".n1.grounded", "grounded"};
        started_skill.confidence_keys = {wf_it->second + ".n1.confidence", "confidence"};
        started_skill.reason_keys = {wf_it->second + ".n1.reason", "reason"};
        if(skill == "skill_search" || skill == "skill_summary") {
          const std::string output_key = skill == "skill_search" ? "search_result" : "summary_result";
          started_skill.answer_keys.push_back(wf_it->second + ".n1." + output_key);
          started_skill.answer_keys.push_back(output_key);
        } else if(skill == "skill_date") {
          started_skill.answer_keys.push_back(wf_it->second + ".n1.date_result");
          started_skill.answer_keys.push_back("date_result");
          started_skill.answer_keys.push_back(wf_it->second + ".n1.stdout");
        } else if(skill == "skill_delegate") {
          started_skill.answer_keys.push_back(wf_it->second + ".n1.delegate_result");
          started_skill.answer_keys.push_back("delegate_result");
          started_skill.answer_keys.push_back(wf_it->second + ".n1.stdout");
        }

        for(const auto& key : started_skill.answer_keys) bus.remove(key);
        for(const auto& key : started_skill.status_keys) bus.remove(key);
        for(const auto& key : started_skill.can_handle_keys) bus.remove(key);
        for(const auto& key : started_skill.grounded_keys) bus.remove(key);
        for(const auto& key : started_skill.confidence_keys) bus.remove(key);
        for(const auto& key : started_skill.reason_keys) bus.remove(key);
        bus.put("session_id", session_id);
        bus.put("session:" + session_id + ":history_json", history_json);

        DataMap skill_inputs;
        skill_inputs["user_text"] = user_text;
        skill_inputs["session_id"] = session_id;
        skill_inputs["history_json"] = history_json;
        skill_inputs["input"] = user_text;
        runtime_->execute_workflow(agent_id, wf_it->second, skill_inputs);
        started_skills.push_back(std::move(started_skill));
      }
    }

    auto skills_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::vector<std::string> skill_result_jsonl;
    for(const auto& started_skill : started_skills) {
      std::string answer_text;
      std::string status;
      std::string reason;
      bool can_handle = false;
      bool grounded = false;
      bool can_handle_seen = false;
      bool grounded_seen = false;
      double confidence = 0.0;

      auto read_string_from_keys = [&bus](const std::vector<std::string>& keys, std::string& value) {
        for(const auto& key : keys) {
          DataVariant v;
          if(bus.get(key, v) && std::holds_alternative<std::string>(v)) {
            const std::string candidate = std::get<std::string>(v);
            if(!candidate.empty()) {
              value = candidate;
              return true;
            }
          }
        }
        return false;
      };
      auto read_bool_from_keys = [&bus](const std::vector<std::string>& keys, bool& value) {
        for(const auto& key : keys) {
          DataVariant v;
          if(bus.get(key, v)) {
            if(std::holds_alternative<bool>(v)) {
              value = std::get<bool>(v);
              return true;
            }
            if(std::holds_alternative<std::string>(v)) {
              const std::string candidate = std::get<std::string>(v);
              if(candidate == "true" || candidate == "1") { value = true; return true; }
              if(candidate == "false" || candidate == "0") { value = false; return true; }
            }
          }
        }
        return false;
      };
      auto read_double_from_keys = [&bus](const std::vector<std::string>& keys, double& value) {
        for(const auto& key : keys) {
          DataVariant v;
          if(bus.get(key, v)) {
            if(std::holds_alternative<double>(v)) { value = std::get<double>(v); return true; }
            if(std::holds_alternative<int64_t>(v)) { value = static_cast<double>(std::get<int64_t>(v)); return true; }
            if(std::holds_alternative<std::string>(v)) {
              try { value = std::stod(std::get<std::string>(v)); return true; } catch(...) {}
            }
          }
        }
        return false;
      };

      while(std::chrono::steady_clock::now() < skills_deadline) {
        read_string_from_keys(started_skill.answer_keys, answer_text);
        read_string_from_keys(started_skill.status_keys, status);
        read_string_from_keys(started_skill.reason_keys, reason);
        can_handle_seen = read_bool_from_keys(started_skill.can_handle_keys, can_handle) || can_handle_seen;
        grounded_seen = read_bool_from_keys(started_skill.grounded_keys, grounded) || grounded_seen;
        read_double_from_keys(started_skill.confidence_keys, confidence);
        if(!answer_text.empty() || can_handle_seen || !status.empty() || !reason.empty()) {
          if(started_skill.skill == "skill_delegate" && !can_handle_seen && !answer_text.empty()) {
            can_handle = true;
            can_handle_seen = true;
            status = status.empty() ? "ok" : status;
            reason = reason.empty() ? "规划技能返回了执行建议。" : reason;
          }
          std::ostringstream item;
          item << "{\"session_id\":\"" << json_escape(started_skill.session_id)
               << "\",\"skill\":\"" << json_escape(started_skill.skill)
               << "\",\"result\":\"" << json_escape(answer_text)
               << "\",\"status\":\"" << json_escape(status.empty() ? "ok" : status)
               << "\",\"reason\":\"" << json_escape(reason) << "\",\"can_handle\":"
               << ((can_handle_seen ? can_handle : !answer_text.empty()) ? "true" : "false")
               << ",\"grounded\":" << ((grounded_seen ? grounded : false) ? "true" : "false")
               << ",\"confidence\":" << confidence << "}";
          if(!answer_text.empty()) {
            bus.put("session:" + started_skill.session_id + ":skill_result", answer_text);
          }
          skill_result_jsonl.push_back(item.str());
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
      }
    }

    if(started_skills.empty()) {
      DataVariant v;
      for(const std::string& key : {"final_response", "response", "output", "result"}) {
        if(bus.get(key, v) && std::holds_alternative<std::string>(v)) {
          const std::string text = std::get<std::string>(v);
          if(!text.empty()) return text;
        }
      }
      std::string direct_response = build_direct_response(user_text, history_json);
      bus.put("final_response", direct_response);
      return direct_response;
    }

    std::ostringstream jsonl;
    for(size_t i = 0; i < skill_result_jsonl.size(); ++i) {
      if(i > 0) jsonl << "\n";
      jsonl << skill_result_jsonl[i];
    }
    bus.put("partial_skill_results", jsonl.str());

    bus.remove("final_response");
    runtime_->execute_workflow(agent_id, "session-coordinator-workflow",
                               {{"session_id", client_session_id},
                                {"user_text", user_text},
                                {"selected_skills", selected_skills},
                                {"partial_skill_results", jsonl.str()}});

    auto final_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::string result;
    while(std::chrono::steady_clock::now() < final_deadline) {
      DataVariant v;
      if(bus.get("final_response", v) && std::holds_alternative<std::string>(v)) {
        result = std::get<std::string>(v);
        if(!result.empty()) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if(result.rfind("当前可用技能暂不支持直接回答该问题：", 0) == 0) {
      const std::string direct_response = build_direct_response(user_text, history_json);
      if(!direct_response.empty() && direct_response != result) {
        result = direct_response;
      }
    }
    if(result.empty()) {
      result = build_direct_response(user_text, history_json);
    }
    bus.put("final_response", result);
    return result;
  }

 private:
  std::string package_path_;
  std::shared_ptr<framework::MMAgent> agent_;
  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
  std::mutex mutex_;
};

}  // namespace agentflow::examples

#endif  // AGENTFLOW_EXAMPLES_CHAT_BRIDGE_SUPPORT_H
