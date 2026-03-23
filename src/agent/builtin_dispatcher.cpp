#include "agent/builtin_dispatcher.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "agent/agent.h"
#include "agent/session.h"
#include "agent/task.h"
#include "workflow/workflow.h"
#include "utils/logger.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#endif

namespace agentflow {
namespace framework {

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

  // Copy relevant metadata from context
  for(const auto& [key, value] : context.metadata) {
    if(key != "task_type" && key != "skill_name") {
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

        // New format: object with name and optional exec_skill_key
        if(doc[i].IsObject()) {
          if(doc[i].HasMember("name") && doc[i]["name"].IsString()) {
            spec.name = doc[i]["name"].GetString();
          }
          if(doc[i].HasMember("exec_skill_key") && doc[i]["exec_skill_key"].IsString()) {
            spec.exec_skill_key = doc[i]["exec_skill_key"].GetString();
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

}  // namespace framework
}  // namespace agentflow