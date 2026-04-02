#include "agent/builtin_dispatcher.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>

#include "agent/agent.h"
#include "agent/session.h"
#include "agent/task.h"
#include "workflow/workflow.h"
#include "utils/logger.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#endif

namespace agentflow {
namespace framework {

namespace {
/**
 * @brief Validate that a skill is in the candidate_skills list.
 *
 * @param skill_name The skill name to validate
 * @param candidate_skills The list of allowed skills (empty means all skills allowed)
 * @return true if skill is allowed, false if explicitly restricted
 */
bool validate_skill_against_candidates(const std::string& skill_name,
                                       const std::vector<std::string>& candidate_skills) {
  // Empty candidate_skills means no restrictions (all skills allowed)
  if(candidate_skills.empty()) {
    return true;
  }

  // Check if skill_name is in the candidate_skills list
  return std::find(candidate_skills.begin(), candidate_skills.end(), skill_name) != candidate_skills.end();
}

/**
 * @brief Parse candidate_skills from a DataVariant value.
 *
 * Supports JSON array format: ["skill1", "skill2"]
 * or comma-separated string: "skill1,skill2"
 *
 * @param value The DataVariant containing candidate_skills
 * @return Vector of candidate skill names
 */
std::vector<std::string> parse_candidate_skills(const DataVariant& value) {
  std::vector<std::string> result;

  if(const auto* text = std::get_if<std::string>(&value)) {
    if(text->empty()) {
      return result;
    }

#ifdef AGENTFLOW_WITH_RAPIDJSON
    // Try JSON array format first
    rapidjson::Document doc;
    doc.Parse(text->c_str());
    if(!doc.HasParseError() && doc.IsArray()) {
      for(rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
        if(doc[i].IsString()) {
          result.push_back(doc[i].GetString());
        }
      }
      return result;
    }
#endif

    // Fall back to comma-separated format
    std::string text_copy = *text;

    // Handle JSON array format: strip outer brackets and quotes
    if(text_copy.size() >= 2 && text_copy.front() == '[' && text_copy.back() == ']') {
      text_copy = text_copy.substr(1, text_copy.size() - 2);
    }

    std::istringstream iss(text_copy);
    std::string token;
    while(std::getline(iss, token, ',')) {
      // Trim whitespace
      size_t start = token.find_first_not_of(" \t\n\r");
      size_t end = token.find_last_not_of(" \t\n\r");
      if(start != std::string::npos && end != std::string::npos) {
        std::string skill = token.substr(start, end - start + 1);
        // Strip surrounding quotes if present (JSON string elements)
        if(skill.size() >= 2 && skill.front() == '"' && skill.back() == '"') {
          skill = skill.substr(1, skill.size() - 2);
        }
        if(!skill.empty()) {
          result.push_back(skill);
        }
      }
    }
  }

  return result;
}
}  // namespace

BuiltinDispatcher::BuiltinDispatcher(const Config& config) : config_(config) {}

void BuiltinDispatcher::process_request(MMAgent& agent,
                                        const std::shared_ptr<MMAgentSession>& session,
                                        const std::shared_ptr<MMAgentTask>& task,
                                        const MMAgentMsgQue::Message& message,
                                        const std::vector<std::shared_ptr<MMWorkflow>>& workflows) {
  if(!session || !task) {
    Logger::instance().warn_f("[BuiltinDispatcher] Invalid session or task");
    return;
  }

  Logger::instance().info_f("[BuiltinDispatcher] Processing request for task=%s, next_skills count=%d",
                           task->get_task_id().c_str(),
                           static_cast<int>(config_.next_skills.size()));

  // Check if we can launch more skills based on concurrency limit
  if(!task->can_launch_more_skills()) {
    Logger::instance().warn_f("[BuiltinDispatcher] Max concurrent skills reached (%d), skipping launch",
                             config_.max_concurrent_skills);
    return;
  }

  // Use pre-configured next_skills from task definition
  const auto& next_skills = config_.next_skills;
  if(next_skills.empty()) {
    Logger::instance().info_f("[BuiltinDispatcher] No next_skills configured, forwarding to coordinator");
    // No skills configured, forward directly to coordinator
    if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
      MMAgentMsgQue::Message coordinator_message = message;
      coordinator_message.metadata["task_type"] = "coordinator";
      coordinator_task->enqueue_intent_request_message(coordinator_message);
    }
    return;
  }

  // Launch skill tasks up to max_concurrent_skills
  int launched = 0;
  for(const auto& skill_name : next_skills) {
    if(!task->can_launch_more_skills()) {
      Logger::instance().info_f("[BuiltinDispatcher] Reached max concurrent skills limit");
      break;
    }

    if(launch_skill_task(agent, session, skill_name, "", message, task)) {
      task->on_skill_started(skill_name);
      ++launched;
      Logger::instance().info_f("[BuiltinDispatcher] Launched skill: %s", skill_name.c_str());
    }
  }

  Logger::instance().info_f("[BuiltinDispatcher] Launched %d skills", launched);
}

void BuiltinDispatcher::process_skill_response(MMAgent& agent,
                                                const std::shared_ptr<MMAgentSession>& session,
                                                const std::shared_ptr<MMAgentTask>& task,
                                                const MMAgentMsgQue::Message& response) {
  if(!session || !task) {
    return;
  }

  Logger::instance().info_f("[BuiltinDispatcher] Processing skill response: skill=%s",
                           response.metadata.count("skill_name") ? response.metadata.at("skill_name").c_str() : "unknown");

  // Decrement running skills count
  if(response.metadata.count("skill_name")) {
    task->on_skill_completed(response.metadata.at("skill_name"));
  }

  // Get context for new format key reading
  std::string session_id = session->get_session_id();
  std::string dialogue_id;
  std::string workflow_id;

  if(response.metadata.count("dialogue_id")) {
    dialogue_id = response.metadata.at("dialogue_id");
  }
  if(response.metadata.count("workflow_id")) {
    workflow_id = response.metadata.at("workflow_id");
  }

  // Check for next_skills in the response and launch more skills
  // Check next_skills first (preferred), then selected_skills for backward compatibility
  DataVariant next_skills_value;
  bool found_next_skills = false;

  // Try reading next_skills with new format and fallback support
  if(get_framework_data(agent, session_id, dialogue_id, workflow_id, "next_skills", next_skills_value)) {
    auto next_skills = parse_next_skills(next_skills_value);
    if(!next_skills.empty()) {
      found_next_skills = true;
    }
  }

  // Backward compatibility: also check selected_skills
  if(!found_next_skills) {
    DataVariant selected_skills_value;
    if(get_framework_data(agent, session_id, dialogue_id, workflow_id, "selected_skills", selected_skills_value)) {
      auto selected_skills = parse_next_skills(selected_skills_value);
      if(!selected_skills.empty()) {
        next_skills_value = selected_skills_value;
        found_next_skills = true;
        Logger::instance().info_f("[BuiltinDispatcher] Using legacy 'selected_skills' (consider migrating to 'next_skills')");
      }
    }
  }

  if(found_next_skills) {
    auto next_skills = parse_next_skills(next_skills_value);
    if(!next_skills.empty()) {
      // Clear both next_skills and selected_skills to prevent re-processing
      agent.publish_data("next_skills", std::string(""));
      agent.publish_data("selected_skills", std::string(""));

      Logger::instance().info_f("[BuiltinDispatcher] Found next_skills in response, launching %d more skills",
                               static_cast<int>(next_skills.size()));

      for(const auto& spec : next_skills) {
        if(!task->can_launch_more_skills()) {
          Logger::instance().info_f("[BuiltinDispatcher] Reached max concurrent skills limit during next_skills processing");
          break;
        }

        if(launch_skill_task(agent, session, spec.name, spec.exec_skill_key, response, task)) {
          task->on_skill_started(spec.name);
          Logger::instance().info_f("[BuiltinDispatcher] Launched next skill: %s%s",
                                   spec.name.c_str(),
                                   spec.exec_skill_key.empty() ? "" : (" with exec_skill_key=" + spec.exec_skill_key).c_str());
        }
      }
    }
  }

  // Forward response to coordinator
  if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
    MMAgentMsgQue::Message coordinator_response = response;
    coordinator_response.metadata["source_task_type"] = "dispatcher";
    coordinator_task->enqueue_response_message(coordinator_response);

    // Trigger coordinator's data exchange to process the response
    MMAgentMsgQue::Message trigger_message;
    trigger_message.message_id = "trigger-" + response.message_id;
    trigger_message.metadata["task_type"] = "coordinator";
    trigger_message.metadata["trigger_type"] = "skill_response";
    coordinator_task->enqueue_data_exchange_message(trigger_message);

    Logger::instance().info_f("[BuiltinDispatcher] Forwarded skill response to coordinator");
  }
}

bool BuiltinDispatcher::launch_skill_task(MMAgent& agent,
                                          const std::shared_ptr<MMAgentSession>& session,
                                          const std::string& skill_name,
                                          const std::string& exec_skill_key,
                                          const MMAgentMsgQue::Message& context,
                                          const std::shared_ptr<MMAgentTask>& source_task) {
  if(!session || skill_name.empty()) {
    return false;
  }

  // Validate skill against candidate_skills if present in context metadata
  if(context.metadata.count("candidate_skills")) {
    DataVariant candidate_skills_value;
    candidate_skills_value = context.metadata.at("candidate_skills");

    // Try to parse as JSON array or comma-separated string
    std::vector<std::string> candidate_skills = parse_candidate_skills(candidate_skills_value);

    if(!validate_skill_against_candidates(skill_name, candidate_skills)) {
      Logger::instance().warn_f("[BuiltinDispatcher] Skill '%s' not in candidate_skills list. "
                               "Allowed skills: [%s]. Skill launch blocked.",
                               skill_name.c_str(),
                               context.metadata.at("candidate_skills").c_str());
      return false;
    }
  }

  // Also check candidate_skills from DataBus (new format key)
  std::string session_id = session->get_session_id();
  std::string dialogue_id;
  std::string workflow_id;

  if(context.metadata.count("dialogue_id")) {
    dialogue_id = context.metadata.at("dialogue_id");
  }
  if(context.metadata.count("workflow_id")) {
    workflow_id = context.metadata.at("workflow_id");
  }

  DataVariant databus_candidate_skills;
  if(get_framework_data(agent, session_id, dialogue_id, workflow_id,
                        "candidate_skills", databus_candidate_skills)) {
    std::vector<std::string> candidate_skills = parse_candidate_skills(databus_candidate_skills);

    if(!validate_skill_against_candidates(skill_name, candidate_skills)) {
      Logger::instance().warn_f("[BuiltinDispatcher] Skill '%s' not in candidate_skills from DataBus. "
                               "Skill launch blocked.", skill_name.c_str());
      return false;
    }
  }

  // Find skill template task
  std::shared_ptr<MMAgentTask> skill_template;
  for(const auto& task : session->get_tasks()) {
    if(task && task->get_task_type() == "skill"
       && task->get_skill_name() == skill_name
       && !task->is_skill_instance()) {
      skill_template = task;
      break;
    }
  }

  if(!skill_template) {
    Logger::instance().warn_f("[BuiltinDispatcher] Skill template not found: %s", skill_name.c_str());
    return false;
  }

  // Create skill runtime task message
  MMAgentMsgQue::Message skill_message;
  skill_message.message_id = "skill-" + skill_name + "-" + context.message_id;
  skill_message.metadata["task_type"] = "skill";
  skill_message.metadata["skill_name"] = skill_name;
  skill_message.metadata["session_id"] = session->get_session_id();
  skill_message.metadata["source_task_id"] = source_task->get_task_id();

  // Add exec_skill_key if provided
  if(!exec_skill_key.empty()) {
    skill_message.metadata["exec_skill_key"] = exec_skill_key;
  }

  skill_message.content = context.content;
  skill_message.data_ref = context.data_ref;

  // Copy relevant metadata from context (exclude keys that are set specifically for this skill)
  for(const auto& [key, value] : context.metadata) {
    if(key != "task_type" && key != "skill_name" && key != "exec_skill_key") {
      skill_message.metadata[key] = value;
    }
  }

  // Get prompt from agent databus if available
  DataVariant prompt_value;
  if(agent.get_data("prompt", prompt_value)) {
    if(const auto* text = std::get_if<std::string>(&prompt_value)) {
      skill_message.content = *text;
      skill_message.metadata["prompt"] = *text;
    }
  }

  // Build runtime task using agent's build_runtime_task method
  // Note: We need to call through agent - this is handled in agent.cpp
  // For now, we'll create the task directly here

  MMAgentTask::Config task_config = skill_template->get_config();
  task_config.task_index = static_cast<int>(session->get_tasks().size());
  task_config.task_id = skill_template->get_task_id() + "_runtime_" + skill_message.message_id;
  task_config.is_skill_instance = true;

  auto skill_task = std::make_shared<MMAgentTask>(task_config);
  skill_task->set_parameter("message_id", skill_message.message_id);
  skill_task->set_parameter("source_task_id", source_task->get_task_id());
  skill_task->set_parameter("session_id", session->get_session_id());

  // Add exec_skill_key if provided
  if(!exec_skill_key.empty()) {
    skill_task->set_parameter("exec_skill_key", exec_skill_key);
  }

  if(!skill_message.content.empty()) {
    skill_task->set_parameter("message_content", skill_message.content);
  }

  session->add_task(skill_task);

  Logger::instance().info_f("[BuiltinDispatcher] Created skill task: %s for skill: %s",
                           skill_task->get_task_id().c_str(), skill_name.c_str());

  return true;
}

std::vector<SkillSpec> BuiltinDispatcher::parse_next_skills(const DataVariant& value) {
  std::vector<SkillSpec> result;

  if(const auto* text = std::get_if<std::string>(&value)) {
    if(text->empty()) {
      return result;
    }

#ifdef AGENTFLOW_WITH_RAPIDJSON
    // Try JSON array format first
    rapidjson::Document doc;
    doc.Parse(text->c_str());
    if(!doc.HasParseError() && doc.IsArray()) {
      for(rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
        SkillSpec spec;

        // New format: object with name and optional exec_skill_key, force
        if(doc[i].IsObject()) {
          if(doc[i].HasMember("name") && doc[i]["name"].IsString()) {
            spec.name = doc[i]["name"].GetString();
          }
          if(doc[i].HasMember("exec_skill_key") && doc[i]["exec_skill_key"].IsString()) {
            spec.exec_skill_key = doc[i]["exec_skill_key"].GetString();
          }
          if(doc[i].HasMember("force") && doc[i]["force"].IsBool()) {
            spec.force = doc[i]["force"].GetBool();
          }
        }
        // Old format: string (backward compatibility)
        else if(doc[i].IsString()) {
          spec.name = doc[i].GetString();
          // exec_skill_key remains empty for old format
        }

        if(!spec.name.empty()) {
          result.push_back(spec);
        }
      }
      return result;
    }
#endif

    // Fall back to comma-separated format
    std::string text_copy = *text;

    // Handle JSON array format: strip outer brackets and quotes
    if(text_copy.size() >= 2 && text_copy.front() == '[' && text_copy.back() == ']') {
      text_copy = text_copy.substr(1, text_copy.size() - 2);
    }

    std::istringstream iss(text_copy);
    std::string token;
    while(std::getline(iss, token, ',')) {
      // Trim whitespace
      size_t start = token.find_first_not_of(" \t\n\r");
      size_t end = token.find_last_not_of(" \t\n\r");
      if(start != std::string::npos && end != std::string::npos) {
        std::string skill = token.substr(start, end - start + 1);
        // Strip surrounding quotes if present (JSON string elements)
        if(skill.size() >= 2 && skill.front() == '"' && skill.back() == '"') {
          skill = skill.substr(1, skill.size() - 2);
        }
        if(!skill.empty()) {
          result.push_back(SkillSpec(skill));
        }
      }
    }
  }

  return result;
}

std::string BuiltinDispatcher::reset_force_flags(const std::string& next_skills_json,
                                                  const std::vector<std::string>& skill_names) {
  if(next_skills_json.empty() || skill_names.empty()) {
    return next_skills_json;
  }

  // Build a set for quick lookup
  std::set<std::string> skills_to_reset(skill_names.begin(), skill_names.end());

#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.Parse(next_skills_json.c_str());

  if(!doc.HasParseError() && doc.IsArray()) {
    bool modified = false;
    for(rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
      if(doc[i].IsObject() && doc[i].HasMember("name") && doc[i]["name"].IsString()) {
        std::string name = doc[i]["name"].GetString();
        if(skills_to_reset.count(name) && doc[i].HasMember("force") && doc[i]["force"].IsBool()) {
          if(doc[i]["force"].GetBool()) {
            doc[i]["force"].SetBool(false);
            modified = true;
            Logger::instance().info_f("[RESET_FORCE] Reset force=false for skill=%s", name.c_str());
          }
        }
      }
    }

    if(modified) {
      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
      doc.Accept(writer);
      return buffer.GetString();
    }
  }
#endif

  // Return unchanged if no modification or no RapidJSON
  return next_skills_json;
}

bool BuiltinDispatcher::get_framework_data(MMAgent& agent,
                                            const std::string& session_id,
                                            const std::string& dialogue_id,
                                            const std::string& workflow_id,
                                            const std::string& key,
                                            DataVariant& value) {
  // Note: workflow_id is not used in key building to support cross-workflow communication
  (void)workflow_id;  // Explicitly unused

  // 1. Try new format (with session isolation)
  if(!session_id.empty()) {
    std::string new_key = "/session/" + session_id + "/sys/";
    if(!dialogue_id.empty()) {
      new_key += dialogue_id + "/";
    }
    new_key += key;

    if(agent.get_data(new_key, value)) {
      Logger::instance().info_f("[GET_FRAMEWORK_DATA] Found key=%s (new format)", new_key.c_str());
      return true;
    }
  }

  // 2. Fallback: legacy format (dialogue_id/key)
  if(!dialogue_id.empty()) {
    std::string legacy_key = dialogue_id + "/" + key;
    if(agent.get_data(legacy_key, value)) {
      Logger::instance().info_f("[GET_FRAMEWORK_DATA] Found key=%s (legacy format)", legacy_key.c_str());
      return true;
    }
  }

  // 3. Fallback: no prefix
  if(agent.get_data(key, value)) {
    Logger::instance().info_f("[GET_FRAMEWORK_DATA] Found key=%s (no prefix)", key.c_str());
    return true;
  }

  return false;
}

bool BuiltinDispatcher::publish_framework_data(MMAgent& agent,
                                               const std::string& session_id,
                                               const std::string& dialogue_id,
                                               const std::string& key,
                                               const DataVariant& value) {
  // Build key in new format: /session/{session_id}/sys/{dialogue_id}/{key}
  std::string new_key;
  if(!session_id.empty()) {
    new_key = "/session/" + session_id + "/sys/";
    if(!dialogue_id.empty()) {
      new_key += dialogue_id + "/";
    }
  } else if(!dialogue_id.empty()) {
    new_key = dialogue_id + "/";
  }
  new_key += key;

  if(agent.publish_data(new_key, value)) {
    Logger::instance().info_f("[PUBLISH_FRAMEWORK_DATA] Published key=%s", new_key.c_str());
    return true;
  }
  return false;
}

bool BuiltinDispatcher::remove_framework_data(MMAgent& agent,
                                              const std::string& session_id,
                                              const std::string& dialogue_id,
                                              const std::string& key) {
  // Build key in new format: /session/{session_id}/sys/{dialogue_id}/{key}
  std::string new_key;
  if(!session_id.empty()) {
    new_key = "/session/" + session_id + "/sys/";
    if(!dialogue_id.empty()) {
      new_key += dialogue_id + "/";
    }
  } else if(!dialogue_id.empty()) {
    new_key = dialogue_id + "/";
  }
  new_key += key;

  // Publish empty DataVariant to remove
  if(agent.publish_data(new_key, DataVariant())) {
    Logger::instance().info_f("[REMOVE_FRAMEWORK_DATA] Removed key=%s", new_key.c_str());
    return true;
  }
  return false;
}

bool BuiltinDispatcher::remove_user_data(MMAgent& agent,
                                         const std::string& session_id,
                                         const std::string& dialogue_id,
                                         const std::string& key) {
  // Build key in user namespace: /session/{session_id}/user/{dialogue_id}/{key}
  std::string new_key;
  if(!session_id.empty()) {
    new_key = "/session/" + session_id + "/user/";
    if(!dialogue_id.empty()) {
      new_key += dialogue_id + "/";
    }
  } else if(!dialogue_id.empty()) {
    new_key = dialogue_id + "/";
  }
  new_key += key;

  // Publish empty DataVariant to remove
  if(agent.publish_data(new_key, DataVariant())) {
    Logger::instance().info_f("[REMOVE_USER_DATA] Removed key=%s", new_key.c_str());
    return true;
  }
  return false;
}

void BuiltinDispatcher::cleanup_dialogue_data(MMAgent& agent,
                                               const std::string& session_id,
                                               const std::string& dialogue_id) {
  if(dialogue_id.empty()) {
    return;
  }

  Logger::instance().info_f("[CLEANUP_DIALOGUE] Cleaning up dialogue_id=%s", dialogue_id.c_str());

  // Read launched_skills list to know which skill result keys to clean
  std::string launched_key = "/session/" + session_id + "/sys/" + dialogue_id + "/launched_skills";
  DataVariant launched_value;
  std::set<std::string> launched_skills;

  if(agent.get_data(launched_key, launched_value)) {
    if(const auto* text = std::get_if<std::string>(&launched_value)) {
      std::istringstream iss(*text);
      std::string skill_name;
      while(std::getline(iss, skill_name, ',')) {
        if(!skill_name.empty()) {
          launched_skills.insert(skill_name);
        }
      }
    }
  }

  // Clean up framework-managed keys in sys namespace
  remove_framework_data(agent, session_id, dialogue_id, "dialogue_id");
  remove_framework_data(agent, session_id, dialogue_id, "user_prompt");
  remove_framework_data(agent, session_id, dialogue_id, "next_skills");
  remove_framework_data(agent, session_id, dialogue_id, "skills_scheduled");
  remove_framework_data(agent, session_id, dialogue_id, "launched_skills");
  remove_framework_data(agent, session_id, dialogue_id, "response");
  remove_framework_data(agent, session_id, dialogue_id, "pending_skills_count");

  // Clean up skill result keys in user namespace (dynamic based on launched_skills)
  for(const auto& skill_name : launched_skills) {
    remove_user_data(agent, session_id, dialogue_id, skill_name + "_result");
    remove_user_data(agent, session_id, dialogue_id, skill_name + "_confidence");
  }

  Logger::instance().info_f("[CLEANUP_DIALOGUE] Cleaned up %zu skills for dialogue_id=%s",
                            launched_skills.size(), dialogue_id.c_str());
}

}  // namespace framework
}  // namespace agentflow