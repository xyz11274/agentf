#ifndef AGENTFLOW_INTERNAL_AGENT_BUILTIN_DISPATCHER_H
#define AGENTFLOW_INTERNAL_AGENT_BUILTIN_DISPATCHER_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent/msgque.h"
#include "agent/common.h"

namespace agentflow {
namespace framework {

class MMAgentTask;
class MMAgentSession;
class MMAgent;
class MMWorkflow;

/**
 * @brief Skill specification for next_skills format extension.
 *
 * Supports both old format (string array) and new format (object array with exec_skill_key).
 */
struct SkillSpec {
  std::string name;            ///< Skill name
  std::string exec_skill_key;  ///< Optional: DataBus key for skill-specific data
  bool        force = false;   ///< Force re-launch: clear previous result and re-schedule

  SkillSpec() = default;
  explicit SkillSpec(const std::string& n) : name(n) {}
  SkillSpec(const std::string& n, const std::string& key) : name(n), exec_skill_key(key) {}
  SkillSpec(const std::string& n, const std::string& key, bool f) : name(n), exec_skill_key(key), force(f) {}
};

/**
 * @brief Built-in Dispatcher for task-workflow scheduling.
 *
 * The BuiltinDispatcher is a framework-internal component that handles:
 * 1. Receiving external requests from the request queue
 * 2. Launching skill tasks based on next_skills configuration
 * 3. Supporting max_concurrent_skills concurrency limits
 * 4. Forwarding skill results and next_skills to Coordinator
 */
class BuiltinDispatcher {
 public:
  struct Config {
    std::string dispatcher_id;
    std::vector<std::string> next_skills;  ///< Legacy: skill names only (for backward compat)
    int max_concurrent_skills = 10;
  };

  explicit BuiltinDispatcher(const Config& config);
  ~BuiltinDispatcher() = default;

  /**
   * @brief Process an incoming request message.
   *
   * This method is called when a new request arrives at the dispatcher's request queue.
   * It will launch skill tasks based on the next_skills configuration.
   *
   * @param agent The agent instance
   * @param session The session that owns this dispatcher
   * @param task The dispatcher task
   * @param message The incoming request message
   * @param workflows Available workflows for task creation
   */
  void process_request(MMAgent& agent,
                       const std::shared_ptr<MMAgentSession>& session,
                       const std::shared_ptr<MMAgentTask>& task,
                       const MMAgentMsgQue::Message& message,
                       const std::vector<std::shared_ptr<MMWorkflow>>& workflows);

  /**
   * @brief Process a skill completion response.
   *
   * This method is called when a skill task completes and sends its result
   * to the coordinator. The dispatcher may forward this to the coordinator.
   *
   * @param agent The agent instance
   * @param session The session that owns this dispatcher
   * @param task The dispatcher task
   * @param response The skill response message
   */
  void process_skill_response(MMAgent& agent,
                              const std::shared_ptr<MMAgentSession>& session,
                              const std::shared_ptr<MMAgentTask>& task,
                              const MMAgentMsgQue::Message& response);

  /**
   * @brief Launch a skill task.
   *
   * Creates and enqueues a skill task for execution.
   *
   * @param agent The agent instance
   * @param session The session context
   * @param skill_name The name of the skill to launch
   * @param exec_skill_key Optional DataBus key for skill-specific data
   * @param context The context message for the skill
   * @param source_task The task that initiated the skill launch (dispatcher or coordinator)
   * @return true if skill was launched successfully
   */
  bool launch_skill_task(MMAgent& agent,
                         const std::shared_ptr<MMAgentSession>& session,
                         const std::string& skill_name,
                         const std::string& exec_skill_key,
                         const MMAgentMsgQue::Message& context,
                         const std::shared_ptr<MMAgentTask>& source_task);

  /**
   * @brief Parse next_skills from a DataVariant value.
   *
   * Supports JSON array formats:
   * - Old format: ["skill1", "skill2"]
   * - New format: [{"name": "skill1", "exec_skill_key": "key1"}, {"name": "skill2"}]
   * or comma-separated string: "skill1,skill2"
   *
   * @param value The DataVariant containing next_skills
   * @return Vector of SkillSpec with name and optional exec_skill_key
   */
  static std::vector<SkillSpec> parse_next_skills(const DataVariant& value);

  /**
   * @brief Get framework data with fallback support for multiple key formats.
   *
   * Tries the following key formats in order:
   * 1. New format: /session/{session_id}/sys/{dialogue_id}/{workflow_id}.{key}
   * 2. Legacy format: {dialogue_id}/{key}
   * 3. No prefix: {key}
   *
   * @param agent The agent instance
   * @param session_id The session ID (can be empty)
   * @param dialogue_id The dialogue ID (can be empty)
   * @param workflow_id The source workflow ID (can be empty)
   * @param key The data key to read
   * @param value Output parameter for the retrieved value
   * @return true if data was found
   */
  static bool get_framework_data(MMAgent& agent,
                                 const std::string& session_id,
                                 const std::string& dialogue_id,
                                 const std::string& workflow_id,
                                 const std::string& key,
                                 DataVariant& value);

  /**
   * @brief Publish framework data to MMDataBus with proper key format.
   *
   * Writes to: /session/{session_id}/sys/{dialogue_id}/{key}
   * This ensures data is stored in the same location as node outputs,
   * enabling unified management and cleanup.
   *
   * @param agent The agent instance
   * @param session_id The session ID
   * @param dialogue_id The dialogue ID (can be empty)
   * @param key The data key
   * @param value The value to write
   * @return true if successful
   */
  static bool publish_framework_data(MMAgent& agent,
                                     const std::string& session_id,
                                     const std::string& dialogue_id,
                                     const std::string& key,
                                     const DataVariant& value);

  /**
   * @brief Remove framework data from MMDataBus.
   *
   * @param agent The agent instance
   * @param session_id The session ID
   * @param dialogue_id The dialogue ID (can be empty)
   * @param key The data key to remove
   * @return true if successful
   */
  static bool remove_framework_data(MMAgent& agent,
                                    const std::string& session_id,
                                    const std::string& dialogue_id,
                                    const std::string& key);

  /**
   * @brief Remove user data from MMDataBus (skill results namespace).
   *
   * Key format: /session/{session_id}/user/{dialogue_id}/{key}
   *
   * @param agent The agent instance
   * @param session_id The session ID
   * @param dialogue_id The dialogue ID
   * @param key The data key to remove
   * @return true if successful
   */
  static bool remove_user_data(MMAgent& agent,
                               const std::string& session_id,
                               const std::string& dialogue_id,
                               const std::string& key);

  /**
   * @brief Clean up all dialogue data from MMDataBus.
   *
   * Cleans both sys and user namespaces for a dialogue:
   * - Framework keys: dialogue_id, user_prompt, next_skills, skills_scheduled,
   *   launched_skills, response, pending_skills_count
   * - Skill result keys: dynamically read from launched_skills list
   *   ({skill_name}_result, {skill_name}_confidence)
   *
   * @param agent The agent instance
   * @param session_id The session ID
   * @param dialogue_id The dialogue ID
   */
  static void cleanup_dialogue_data(MMAgent& agent,
                                    const std::string& session_id,
                                    const std::string& dialogue_id);

  const Config& get_config() const { return config_; }
  const std::string& get_dispatcher_id() const { return config_.dispatcher_id; }

 private:
  Config config_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_BUILTIN_DISPATCHER_H