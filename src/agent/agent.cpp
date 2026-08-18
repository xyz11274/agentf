#include "agent/agent.h"

#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

#include "utils/file_storage.h"
#include "utils/logger.h"
#include "workflow/dsl/workflow_parser.h"
#include "workflow/dsl/json_adapter.h"

#include "agent/context.h"
#include "agent/coordinator.h"
#include "agent/dataref_bridge.h"
#include "agent/package.h"
#include "agent/session.h"
#include "agent/session_router.h"
#include "agent/task.h"
#include "agent/builtin_dispatcher.h"

#include "runtime/trace/trace_context.h"
#include "runtime/trace/trace_manager.h"
#include "runtime/flow_trace.h"
#include "runtime/databus/databus.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#endif

namespace agentflow {
namespace framework {

namespace storage = agentflow::utils::storage;

namespace {
constexpr const char* kFallbackSessionId = "0";

int64_t get_current_timestamp_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

/// DataBus namespaces for dialogue-level data isolation
constexpr const char* DIALOGUE_NAMESPACES[] = {"sys", "user", "ctx", "llm", "tool"};

std::map<std::string, std::string> build_task_workflow_map(
    const std::vector<AgentPackage::TaskDefinition>& session_tasks) {
  std::map<std::string, std::string> task_workflows;
  for(const auto& task_definition : session_tasks) {
    if(task_definition.workflow_file.empty()) {
      continue;
    }
    if(task_workflows.count(task_definition.task_type) == 0) {
      task_workflows[task_definition.task_type] = task_definition.workflow_file;
    }
    if(task_definition.task_type == "skill" && !task_definition.skill.skill_name.empty()) {
      task_workflows["skill:" + task_definition.skill.skill_name] = task_definition.workflow_file;
    }
  }
  return task_workflows;
}

const AgentPackage::TaskDefinition* find_task_definition(
    const std::vector<AgentPackage::TaskDefinition>& session_tasks, const std::string& task_type) {
  for(const auto& task_definition : session_tasks) {
    if(task_definition.task_type == task_type) {
      return &task_definition;
    }
  }
  return nullptr;
}

std::shared_ptr<MMAgentTask> find_skill_template_task(const std::shared_ptr<MMAgentSession>& session,
                                                      const std::string& skill_name) {
  if(!session || skill_name.empty()) {
    return nullptr;
  }
  for(const auto& task : session->get_tasks()) {
    if(!task || task->get_task_type() != "skill" || task->get_skill_name() != skill_name
       || task->is_skill_instance()) {
      continue;
    }
    return task;
  }
  return nullptr;
}

std::string extract_json_field(const std::string& json_line, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  auto              pos    = json_line.find(needle);
  if(pos == std::string::npos) {
    return "";
  }
  pos = json_line.find(':', pos + needle.size());
  if(pos == std::string::npos) {
    return "";
  }
  ++pos;
  while(pos < json_line.size() && std::isspace(static_cast<unsigned char>(json_line[pos]))) {
    ++pos;
  }
  if(pos >= json_line.size() || json_line[pos] != '"') {
    return "";
  }
  ++pos;
  std::string value;
  while(pos < json_line.size() && json_line[pos] != '"') {
    if(json_line[pos] == '\\' && pos + 1 < json_line.size()) {
      ++pos;
    }
    value.push_back(json_line[pos]);
    ++pos;
  }
  return value;
}

/// Parse available_parameters from JSON string for a specific skill.
/// @param json_str JSON string in format: {"skill_name": {"param": "value"}}
/// @param skill_name The skill name to extract parameters for
/// @return Map of parameter name to value for the specified skill
std::map<std::string, std::string> parse_available_parameters_for_skill(
    const std::string& json_str, const std::string& skill_name) {
  std::map<std::string, std::string> result;
  if(json_str.empty() || skill_name.empty()) {
    return result;
  }
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());
  if(doc.HasParseError() || !doc.IsObject()) {
    return result;
  }
  if(!doc.HasMember(skill_name.c_str()) || !doc[skill_name.c_str()].IsObject()) {
    return result;
  }
  const auto& skill_params = doc[skill_name.c_str()];
  for(auto it = skill_params.MemberBegin(); it != skill_params.MemberEnd(); ++it) {
    if(it->value.IsString()) {
      result[it->name.GetString()] = it->value.GetString();
    }
  }
#else
  // Fallback: simple string parsing without full JSON parser
  // This is a basic implementation for stub mode
  const std::string skill_key = "\"" + skill_name + "\"";
  auto pos = json_str.find(skill_key);
  if(pos == std::string::npos) {
    return result;
  }
  pos = json_str.find('{', pos + skill_key.size());
  if(pos == std::string::npos) {
    return result;
  }
  // Find matching closing brace
  int brace_count = 1;
  size_t end_pos = pos + 1;
  while(end_pos < json_str.size() && brace_count > 0) {
    if(json_str[end_pos] == '{') ++brace_count;
    else if(json_str[end_pos] == '}') --brace_count;
    ++end_pos;
  }
  // Parse key-value pairs within the skill object
  std::string params_str = json_str.substr(pos, end_pos - pos);
  size_t key_start = 0;
  while((key_start = params_str.find('"', key_start)) != std::string::npos) {
    size_t key_end = params_str.find('"', key_start + 1);
    if(key_end == std::string::npos) break;
    std::string key = params_str.substr(key_start + 1, key_end - key_start - 1);
    size_t colon = params_str.find(':', key_end);
    if(colon == std::string::npos) break;
    size_t val_start = params_str.find('"', colon);
    if(val_start == std::string::npos) break;
    size_t val_end = params_str.find('"', val_start + 1);
    if(val_end == std::string::npos) break;
    result[key] = params_str.substr(val_start + 1, val_end - val_start - 1);
    key_start = val_end + 1;
  }
#endif
  return result;
}

std::string agent_config_to_json(const MMAgent::Config& config,
                                 const std::vector<std::shared_ptr<MMWorkflow>>& workflows) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

  rapidjson::Value agent_obj(rapidjson::kObjectType);
  agent_obj.AddMember("id", rapidjson::StringRef(config.id.c_str()), allocator);
  agent_obj.AddMember("name", rapidjson::StringRef(config.name.c_str()), allocator);
  agent_obj.AddMember("version", rapidjson::StringRef(config.version.c_str()), allocator);
  agent_obj.AddMember("description", rapidjson::StringRef(config.description.c_str()), allocator);
  agent_obj.AddMember("policy", static_cast<int>(config.policy), allocator);
  agent_obj.AddMember("max_workflows", config.max_workflows, allocator);
  agent_obj.AddMember("max_concurrent_workflows", config.max_concurrent_workflows, allocator);
  agent_obj.AddMember("data_bus_capacity_mb", config.data_bus_capacity_mb, allocator);
  doc.AddMember("agent", agent_obj, allocator);

  rapidjson::Value workflows_array(rapidjson::kArrayType);
  for(const auto& workflow : workflows) {
    rapidjson::Value workflow_obj(rapidjson::kObjectType);
    workflow_obj.AddMember("id", rapidjson::StringRef(workflow->get_id().c_str()), allocator);
    workflow_obj.AddMember("name", rapidjson::StringRef(workflow->get_name().c_str()), allocator);
    workflows_array.PushBack(workflow_obj, allocator);
  }
  doc.AddMember("workflows", workflows_array, allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  return buffer.GetString();
#else
  std::ostringstream oss;
  oss << "{" << "\"id\":\"" << config.id << "\"," << "\"name\":\"" << config.name
      << "\"," << "\"version\":\"" << config.version << "\"" << "}";
  return oss.str();
#endif
}

bool agent_config_from_json(const std::string& json_str, MMAgent::Config& config,
                            std::vector<MMWorkflow::Config>& workflow_configs) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());
  if(doc.HasParseError() || !doc.HasMember("agent")) {
    return false;
  }

  const rapidjson::Value& agent_obj = doc["agent"];
  if(agent_obj.HasMember("id"))
    config.id = agent_obj["id"].GetString();
  if(agent_obj.HasMember("name"))
    config.name = agent_obj["name"].GetString();
  if(agent_obj.HasMember("version"))
    config.version = agent_obj["version"].GetString();
  if(agent_obj.HasMember("description"))
    config.description = agent_obj["description"].GetString();
  if(agent_obj.HasMember("policy"))
    config.policy = static_cast<MMAgent::LifecyclePolicy>(agent_obj["policy"].GetInt());
  if(agent_obj.HasMember("max_workflows"))
    config.max_workflows = agent_obj["max_workflows"].GetInt();
  if(agent_obj.HasMember("max_concurrent_workflows"))
    config.max_concurrent_workflows = agent_obj["max_concurrent_workflows"].GetInt();
  if(agent_obj.HasMember("data_bus_capacity_mb"))
    config.data_bus_capacity_mb = agent_obj["data_bus_capacity_mb"].GetInt();

  if(doc.HasMember("workflows") && doc["workflows"].IsArray()) {
    const rapidjson::Value& workflows_array = doc["workflows"];
    for(rapidjson::SizeType i = 0; i < workflows_array.Size(); ++i) {
      const rapidjson::Value& workflow_obj = workflows_array[i];
      MMWorkflow::Config wf_config;
      if(workflow_obj.HasMember("id"))
        wf_config.id = workflow_obj["id"].GetString();
      if(workflow_obj.HasMember("name"))
        wf_config.name = workflow_obj["name"].GetString();
      if(workflow_obj.HasMember("version"))
        wf_config.version = workflow_obj["version"].GetString();
      if(workflow_obj.HasMember("dsl_file"))
        wf_config.dsl_file = workflow_obj["dsl_file"].GetString();
      if(workflow_obj.HasMember("description"))
        wf_config.description = workflow_obj["description"].GetString();
      workflow_configs.push_back(wf_config);
    }
  }
  return true;
#else
  size_t pos = json_str.find("\"id\":\"");
  if(pos == std::string::npos)
    return false;
  size_t start = pos + 6;
  size_t end = json_str.find("\"", start);
  config.id = json_str.substr(start, end - start);
  return true;
#endif
}

/// Helper struct for multi-response consumption result
struct MultiResponseResult {
  bool has_response = false;         // Whether a response was found
  std::string content;               // Response content
  bool is_done = false;              // Whether session is done
  bool all_consumed = false;         // Whether all responses have been consumed
  int consumed_index = -1;           // Index of consumed response
};

/// Consume a single multi-response from the responses queue.
/// Uses consumed_count for O(1) positioning instead of iteration.
/// @param agent The agent to read data from
/// @param session_id Session ID
/// @param dialogue_id Dialogue ID
/// @return MultiResponseResult with consumption details
MultiResponseResult consume_multi_response(MMAgent& agent,
                                            const std::string& session_id,
                                            const std::string& dialogue_id) {
  MultiResponseResult result;

  // Check if responses/done exists (indicates multi-response mode)
  DataVariant done_val;
  if(!BuiltinDispatcher::get_framework_data(agent, session_id, dialogue_id, "", "responses/done", done_val)) {
    return result;  // Not in multi-response mode
  }

  // Read consumed_count (next to consume)
  std::string consumed_count_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses/consumed_count";
  DataVariant consumed_count_val;
  int consumed_count = 0;
  if(agent.get_data(consumed_count_key, consumed_count_val)) {
    if(const auto* count_str = std::get_if<std::string>(&consumed_count_val)) {
      try {
        consumed_count = std::stoi(*count_str);
      } catch(...) {
        consumed_count = 0;
      }
    }
  }

  // Read total_count
  std::string total_count_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses/total_count";
  DataVariant total_count_val;
  int total_count = 0;
  if(agent.get_data(total_count_key, total_count_val)) {
    if(const auto* count_str = std::get_if<std::string>(&total_count_val)) {
      try {
        total_count = std::stoi(*count_str);
      } catch(...) {
        total_count = 0;
      }
    }
  }

  // Check if there are still unconsumed responses
  if(consumed_count >= total_count) {
    result.all_consumed = true;
    // Check done flag
    if(const auto* done_str = std::get_if<std::string>(&done_val); done_str && *done_str == "true") {
      result.is_done = true;
    }
    return result;
  }

  // Read the content at consumed_count
  std::string content_key = "/session/" + session_id + "/sys/" + dialogue_id +
                            "/responses/" + std::to_string(consumed_count) + "/content";
  DataVariant content_val;
  if(agent.get_data(content_key, content_val)) {
    if(const auto* text = std::get_if<std::string>(&content_val)) {
      result.has_response = true;
      result.content = *text;
      result.consumed_index = consumed_count;
    }
  }

  // Check done flag
  if(const auto* done_str = std::get_if<std::string>(&done_val); done_str && *done_str == "true") {
    result.is_done = true;
  }

  return result;
}

/// Mark a multi-response as consumed and increment consumed_count.
/// @param agent The agent to update
/// @param session_id Session ID
/// @param dialogue_id Dialogue ID
/// @param index The index that was consumed
void mark_response_consumed(MMAgent& agent,
                             const std::string& session_id,
                             const std::string& dialogue_id,
                             int index) {
  // Mark the specific response as consumed
  std::string consumed_key = "/session/" + session_id + "/sys/" + dialogue_id +
                             "/responses/" + std::to_string(index) + "/consumed";
  agent.publish_data(consumed_key, std::string("true"));

  // Increment consumed_count
  std::string consumed_count_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses/consumed_count";
  agent.publish_data(consumed_count_key, std::to_string(index + 1));

  Logger::instance().info_f("[MULTI_RESPONSE_CONSUME] Marked index=%d as consumed, consumed_count=%d",
                            index, index + 1);
}

/// Clean up all multi-response data for a dialogue.
/// @param agent The agent to clean up
/// @param session_id Session ID
/// @param dialogue_id Dialogue ID
void cleanup_multi_responses(MMAgent& agent,
                              const std::string& session_id,
                              const std::string& dialogue_id) {
  // Read total_count to know how many responses to clean
  std::string total_count_key = "/session/" + session_id + "/sys/" + dialogue_id + "/responses/total_count";
  DataVariant total_count_val;
  int total_count = 0;
  if(agent.get_data(total_count_key, total_count_val)) {
    if(const auto* count_str = std::get_if<std::string>(&total_count_val)) {
      try {
        total_count = std::stoi(*count_str);
      } catch(...) {
        total_count = 0;
      }
    }
  }

  // Clean each response entry
  for(int i = 0; i < total_count; ++i) {
    agent.publish_data("/session/" + session_id + "/sys/" + dialogue_id +
                       "/responses/" + std::to_string(i) + "/content", DataVariant());
    agent.publish_data("/session/" + session_id + "/sys/" + dialogue_id +
                       "/responses/" + std::to_string(i) + "/consumed", DataVariant());
  }

  // Clean control keys
  agent.publish_data(total_count_key, DataVariant());
  agent.publish_data("/session/" + session_id + "/sys/" + dialogue_id + "/responses/consumed_count", DataVariant());
  agent.publish_data("/session/" + session_id + "/sys/" + dialogue_id + "/responses/done", DataVariant());

  Logger::instance().info_f("[MULTI_RESPONSE_CLEANUP] Cleaned %d responses for dialogue_id=%s",
                            total_count, dialogue_id.c_str());
}

}  // namespace

MMAgent::MMAgent(const Config& config) : config_(config), state_(AgentState::CREATED) {
  initialize_components();
}

std::shared_ptr<MMAgent> MMAgent::create_from_package(const std::string& package_path) {
  auto package = AgentPackage::load_from_directory(package_path);
  if(!package) {
    Logger::instance().error_f("Failed to load agent package from: %s", package_path.c_str());
    return nullptr;
  }

  MMAgent::Config config;
  config.id = package->get_package_id();
  config.name = package->get_package_name();
  config.version = package->get_version();
  config.description = package->get_config().description;
  config.policy = MMAgent::LifecyclePolicy::PER_REQUEST;
  config.max_workflows = package->get_config().max_workflows;
  config.max_concurrent_workflows = package->get_config().max_concurrent_workflows;
  config.data_bus_capacity_mb = package->get_config().data_bus_capacity_mb;

  auto agent = std::make_shared<MMAgent>(config);
  namespace fs = std::filesystem;
  auto parser = dsl::WorkflowParserFactory::create();
  std::vector<std::shared_ptr<MMWorkflow>> loaded_workflows;

  for(const auto& wf_file : package->get_config().workflow_files) {
    fs::path wf_path = fs::path(wf_file);
    if(!wf_path.is_absolute()) {
      wf_path = fs::path(package_path) / wf_path;
    }
    if(fs::exists(wf_path)) {
      auto result = parser->parse_workflow_file(wf_path.string());
      if(result.is_valid() && result.workflow) {
        loaded_workflows.push_back(result.workflow);
      } else {
        Logger::instance().error_f("Failed to load workflow from: %s", wf_path.string().c_str());
        if(!result.errors.empty()) {
          Logger::instance().error_f("Parser error: %s", result.errors[0].message.c_str());
        }
      }
    } else {
      Logger::instance().error_f("Workflow file not found: %s", wf_path.string().c_str());
    }
  }

  for(const auto& workflow : loaded_workflows) {
    agent->add_workflow(workflow);
  }
  agent->initialize_components(package);
  return agent;
}

MMAgent::~MMAgent() {
  bool expected = true;
  if(message_threads_running_.compare_exchange_strong(expected, false)) {
    if(intent_request_thread_.joinable()) {
      intent_request_thread_.join();
    }
    if(data_exchange_thread_.joinable()) {
      data_exchange_thread_.join();
    }
  }
  for(const auto& session : get_sessions()) {
    if(session) {
      if(auto dispatcher_task = session->get_dispatcher_task(); dispatcher_task) {
        dispatcher_task->stop_message_threads();
      }
      if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
        coordinator_task->stop_message_threads();
      }
    }
  }
  if(session_router_) {
    session_router_->stop();
  }
  sessions_.clear();
  current_task_.reset();
  context_.reset();
  package_.reset();
  session_router_.reset();
  // P0-4：不重复调 cleanup_resources()——stop() 已做过，析构时 runtime_databus_ 可能已释放
  // （databuses_.erase 在 ~MMAgent 后执行，但析构链里 smart ptr reset 顺序可能让 runtime_databus_ 野指针）。
  // 析构只需 smart ptr reset，数据清理由 stop() 负责。
}

bool MMAgent::initialize_components(std::shared_ptr<AgentPackage> package) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
  }

  if(package) {
    package_ = package;
  } else {
    AgentPackage::Config package_config;
    package_config.package_id = config_.id + "_package";
    package_config.package_name = config_.name;
    package_config.version = config_.version;
    package_config.session_pool_size = 1;
    package_config.max_workflows = config_.max_workflows;
    package_config.max_concurrent_workflows = config_.max_concurrent_workflows;
    package_ = std::make_shared<AgentPackage>(package_config);
  }

  AgentContext::Config context_config;
  context_config.context_id = config_.id + "_context";
  context_config.agent_id = config_.id;
  if(package_) {
    if(package_->get_config().has_contextupdater) {
      context_config.contextupdater_workflow = package_->get_config().contextupdater_task.workflow_file;
      context_config.contextupdater_task_id = config_.id + "_contextupdater";
      context_config.contextupdater_description =
          package_->get_config().contextupdater_task.description;
      context_config.contextupdater_workflow_pool_size =
          package_->get_config().contextupdater_task.workflow_pool_size;
    }
  }
  context_ = std::make_shared<AgentContext>(context_config);
  current_task_ = context_ ? context_->get_context_updater_task() : nullptr;

  SessionRouter::Config router_config;
  router_config.router_id = config_.id + "_session_router";
  session_router_ = std::make_shared<SessionRouter>(router_config);
  session_router_->set_route_handler([this](const std::shared_ptr<MMAgentSession>& session,
                                            const MMAgentMsgQue::Message& message) {
    handle_routed_request_message(session, message);
  });

  // Set up session factory for on-demand session creation with UUID
  // When a new route (user_id:client_id) is encountered, this factory creates
  // a new session with a UUID as session_id
  session_router_->set_session_factory([this](const std::string& user_id,
                                               const std::string& client_id) {
    // Determine which session tasks to use
    std::vector<AgentPackage::TaskDefinition> session_tasks;
    bool has_session_template = package_ && (!package_->get_config().session_tasks.empty()
                                              || !package_->get_config().sessions.empty());
    if(has_session_template) {
      if(!package_->get_config().session_tasks.empty()) {
        session_tasks = package_->get_config().session_tasks;
      } else if(!package_->get_config().sessions.empty()) {
        session_tasks = package_->get_config().sessions.front().tasks;
      }
    } else {
      session_tasks = build_default_session_tasks();
    }

    // Create session with empty session_id to trigger UUID generation
    // This ensures each user_id:client_id gets a unique UUID-based session
    auto session = create_session_from_tasks("", session_tasks);
    if(session) {
      Logger::instance().info_f("[AGENT_SESSION_FACTORY] Created session for user_id=%s, client_id=%s, session_id=%s",
                               user_id.c_str(), client_id.c_str(), session->get_session_id().c_str());
    }
    return session;
  });

  // Inject cleanup callback: when a session is released back to the pool
  // (aging timeout or explicit release), clear its DataBus data under
  // /session/{session_id}/. Without this, aged-out sessions leak their
  // DataBus keys forever → shared memory full.
  // P0-1 方案 D：callback 收 seq 校验——若归还后已被新用户复用（seq 变化），跳过清理避免误删。
  session_router_->set_session_cleanup_callback([this](const std::string& session_id, uint64_t seq) {
    int cleared = clear_session_data_if_unused(session_id, seq);
    Logger::instance().info_f("[AGENT_SESSION_AGING] Cleared %d DataBus keys for aged-out session=%s seq=%llu",
                              cleared, session_id.c_str(),
                              static_cast<unsigned long long>(seq));
  });

  // Create session pool based on configuration
  std::vector<std::shared_ptr<MMAgentSession>> session_pool;
  bool has_session_template = package_ && (!package_->get_config().session_tasks.empty()
                                            || !package_->get_config().sessions.empty());
  Logger::instance().info_f("[AGENT_INIT] has_session_template=%s, session_tasks_count=%zu, sessions_count=%zu",
                            has_session_template ? "true" : "false",
                            package_ ? package_->get_config().session_tasks.size() : 0,
                            package_ ? package_->get_config().sessions.size() : 0);

  if(has_session_template) {
    // Get session tasks from new flattened format or legacy sessions format
    std::vector<AgentPackage::TaskDefinition> session_tasks;
    if(!package_->get_config().session_tasks.empty()) {
      // New flattened format
      session_tasks = package_->get_config().session_tasks;
    } else if(!package_->get_config().sessions.empty()) {
      // Legacy format: extract tasks from sessions[0]
      session_tasks = package_->get_config().sessions.front().tasks;
    }

    const int session_pool_size = package_->get_config().session_pool_size;
    for(int session_index = 0; session_index < session_pool_size; ++session_index) {
      // Create session with numeric ID for configured sessions
      auto session = create_session_from_tasks(std::to_string(session_index), session_tasks);
      if(session) {
        session_pool.push_back(session);
      }
    }
    Logger::instance().info_f("[AGENT_INIT] Pre-created %zu sessions from session template", session_pool.size());
  } else {
    // No explicit session configuration - sessions will be created on-demand via the session factory
    // when a new route (user_id:client_id) is encountered. This ensures each route gets
    // a unique UUID-based session_id.
    Logger::instance().info_f("[AGENT_INIT] Sessions will be created on-demand with UUID for each user_id:client_id route");
  }

  session_router_->set_session_pool(session_pool);
  return true;
}

std::vector<AgentPackage::TaskDefinition> MMAgent::build_default_session_tasks() const {
  std::vector<AgentPackage::TaskDefinition> session_tasks;

  AgentPackage::TaskDefinition dispatcher_task;
  dispatcher_task.task_index = 0;
  dispatcher_task.task_type = "dispatcher";
  dispatcher_task.scope = "session";
  dispatcher_task.description = "Session dispatcher";

  AgentPackage::TaskDefinition coordinator_task;
  coordinator_task.task_index = 1;
  coordinator_task.task_type = "coordinator";
  coordinator_task.scope = "session";
  coordinator_task.description = "Session coordinator";

  session_tasks = {dispatcher_task, coordinator_task};
  return session_tasks;
}

std::shared_ptr<MMAgentSession> MMAgent::create_session_from_tasks(
    const std::string& session_id, const std::vector<AgentPackage::TaskDefinition>& session_tasks) {
  MMAgentSession::Config session_config;
  session_config.session_index = 0;  // Will be set by pool index
  session_config.session_id = session_id;
  session_config.description = "Pooled session";
  session_config.task_workflows = build_task_workflow_map(session_tasks);

  if(const auto* dispatcher_definition = find_task_definition(session_tasks, "dispatcher");
     dispatcher_definition != nullptr) {
    session_config.dispatcher_workflow =
        resolve_workflow_reference(dispatcher_definition->workflow_file);
    if(!session_config.dispatcher_workflow.empty()) {
      session_config.task_workflows["dispatcher"] = session_config.dispatcher_workflow;
    }
  }
  if(const auto* coordinator_definition = find_task_definition(session_tasks, "coordinator");
     coordinator_definition != nullptr) {
    session_config.coordinator_workflow =
        resolve_workflow_reference(coordinator_definition->workflow_file);
    if(!session_config.coordinator_workflow.empty()) {
      session_config.task_workflows["coordinator"] = session_config.coordinator_workflow;
    }
  }

  auto session = std::make_shared<MMAgentSession>(session_config);
  std::shared_ptr<MMAgentTask> dispatcher_task;
  std::shared_ptr<MMAgentTask> coordinator_task;

  // Get the actual session_id (may have been auto-generated as UUID)
  const std::string& actual_session_id = session->get_session_id();

  for(const auto& task_definition : session_tasks) {
    MMAgentTask::Config task_config;
    task_config.task_index = task_definition.task_index;
    // Use agreed naming for dispatcher/coordinator, enabling direct routing
    if(task_definition.task_type == "dispatcher") {
      task_config.task_id = actual_session_id + "_dispatcher";
    } else if(task_definition.task_type == "coordinator") {
      task_config.task_id = actual_session_id + "_coordinator";
    } else {
      task_config.task_id = actual_session_id + "_task_" + std::to_string(task_definition.task_index);
    }
    task_config.task_type = task_definition.task_type;
    task_config.description = task_definition.description;
    task_config.session_id = actual_session_id;
    task_config.workflow_id = resolve_workflow_reference(task_definition.workflow_file);
    task_config.workflow_file = resolve_workflow_file(task_definition.workflow_file);
    task_config.workflow_pool_size = task_definition.workflow_pool_size;
    if(task_config.workflow_file.empty()) {
      task_config.workflow_file = task_definition.workflow_file;
    }
    // Copy dispatcher-specific fields
    if(task_definition.task_type == "dispatcher") {
      task_config.next_skills = task_definition.next_skills;
      task_config.max_concurrent_skills = task_definition.max_concurrent_skills;
    }
    if(task_definition.task_type == "skill") {
      task_config.skill_name = task_definition.skill.skill_name;
      task_config.instance_pool_size = task_definition.skill.instance_pool_size;
      task_config.expected_parameters = task_definition.skill.expected_parameters;
    }

    auto task = std::make_shared<MMAgentTask>(task_config);
    session->add_template_task(task);

    if(task_definition.task_type == "dispatcher") {
      dispatcher_task = task;
      session->set_dispatcher_task(task);

      // Create and store BuiltinDispatcher instance
      BuiltinDispatcher::Config dispatcher_config;
      dispatcher_config.dispatcher_id = config_.id + "_builtin_dispatcher_" + actual_session_id;
      dispatcher_config.next_skills = task_definition.next_skills;
      dispatcher_config.max_concurrent_skills = task_definition.max_concurrent_skills;
      {
        std::lock_guard<std::mutex> lock(builtin_dispatchers_mutex_);
        builtin_dispatchers_[actual_session_id] = std::make_shared<BuiltinDispatcher>(dispatcher_config);
      }
      Logger::instance().info_f("[CREATE_SESSION] Created BuiltinDispatcher for session=%s, next_skills count=%d",
                               actual_session_id.c_str(), static_cast<int>(dispatcher_config.next_skills.size()));
    } else if(task_definition.task_type == "coordinator") {
      coordinator_task = task;
      session->set_coordinator_task(task);
    }

    if(task_definition.task_type == "skill") {
      for(int instance_index = 0; instance_index < task_definition.skill.instance_pool_size;
          ++instance_index) {
        MMAgentTask::Config pooled_task_config = task_config;
        pooled_task_config.task_id += "_pool_" + std::to_string(instance_index);
        pooled_task_config.is_skill_instance = true;
        pooled_task_config.skill_instance_index = instance_index;
        session->add_template_task(std::make_shared<MMAgentTask>(pooled_task_config));
      }
    }
  }

  if(dispatcher_task) {
    SessionTaskCoordinator::Config dispatcher_config;
    dispatcher_config.coordinator_id = config_.id + "_dispatcher_" + actual_session_id;
    dispatcher_config.default_task_type = "dispatcher";
    dispatcher_config.input_queue_type = SessionTaskCoordinator::InputQueueType::INTENT_REQUEST;
    dispatcher_config.dispatcher_workflow = session_config.dispatcher_workflow;
    dispatcher_config.coordinator_workflow = session_config.coordinator_workflow;
    dispatcher_config.task_workflows = session_config.task_workflows;
    session->set_task_dispatcher(
        std::make_shared<SessionTaskCoordinator>(dispatcher_config, dispatcher_task));
    dispatcher_task->configure_message_handlers(
        [this, session, dispatcher_task](const MMAgentMsgQue::Message& message) {
          handle_task_intent_request(session, dispatcher_task, message);
        },
        [this, session, dispatcher_task](const MMAgentMsgQue::Message& message) {
          handle_task_data_exchange(session, dispatcher_task, message);
        });
  }

  if(coordinator_task) {
    SessionTaskCoordinator::Config coordinator_config;
    coordinator_config.coordinator_id = config_.id + "_coordinator_" + actual_session_id;
    coordinator_config.default_task_type = "coordinator";
    coordinator_config.input_queue_type = SessionTaskCoordinator::InputQueueType::DATA_EXCHANGE;
    coordinator_config.dispatcher_workflow = session_config.dispatcher_workflow;
    coordinator_config.coordinator_workflow = session_config.coordinator_workflow;
    coordinator_config.task_workflows = session_config.task_workflows;
    session->set_task_coordinator(
        std::make_shared<SessionTaskCoordinator>(coordinator_config, coordinator_task));
    coordinator_task->configure_message_handlers(
        [this, session, coordinator_task](const MMAgentMsgQue::Message& message) {
          handle_task_intent_request(session, coordinator_task, message);
        },
        [this, session, coordinator_task](const MMAgentMsgQue::Message& message) {
          handle_task_data_exchange(session, coordinator_task, message);
        });
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[actual_session_id] = session;
  }

  // Set session event callback if configured
  if(session_event_callback_) {
    session->set_session_event_callback(session_event_callback_);
  }

  return session;
}

std::shared_ptr<MMAgentSession> MMAgent::create_session_with_defaults(const std::string& session_id) {
  return create_session_from_tasks(session_id, build_default_session_tasks());
}

std::string MMAgent::resolve_workflow_reference(const std::string& workflow_ref) const {
  if(workflow_ref.empty()) {
    return "";
  }
  namespace fs = std::filesystem;
  const fs::path workflow_path(workflow_ref);
  const std::string workflow_filename = workflow_path.filename().string();
  for(const auto& workflow : workflows_) {
    if(!workflow) {
      continue;
    }
    const fs::path dsl_path(workflow->get_config().dsl_file);
    if(workflow->get_id() == workflow_ref || workflow->get_name() == workflow_ref
       || dsl_path == workflow_path || dsl_path.filename() == workflow_filename) {
      return workflow->get_id();
    }
  }
  return workflow_ref;
}

std::string MMAgent::resolve_workflow_file(const std::string& workflow_ref) const {
  if(workflow_ref.empty()) {
    return "";
  }
  namespace fs = std::filesystem;
  const fs::path workflow_path(workflow_ref);
  const std::string workflow_filename = workflow_path.filename().string();
  for(const auto& workflow : workflows_) {
    if(workflow && (workflow->get_id() == workflow_ref || workflow->get_name() == workflow_ref
                    || fs::path(workflow->get_config().dsl_file) == workflow_path
                    || fs::path(workflow->get_config().dsl_file).filename() == workflow_filename)) {
      return workflow->get_config().dsl_file;
    }
  }
  return workflow_ref;
}

std::shared_ptr<MMAgentSession> MMAgent::get_or_create_session(const std::string& user_id,
                                                               const std::string& client_id) {
  return session_router_ ? session_router_->get_or_create_session(user_id, client_id) : nullptr;
}

std::shared_ptr<MMAgentSession> MMAgent::get_session_by_key(const std::string& session_key) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if(const auto it = sessions_.find(session_key); it != sessions_.end()) {
      return it->second;
    }
  }
  return session_router_ ? session_router_->get_session_by_route_key(session_key) : nullptr;
}

std::shared_ptr<MMAgentSession> MMAgent::get_session(const std::string& session_key) const {
  return get_session_by_key(session_key);
}

std::vector<std::shared_ptr<MMAgentSession>> MMAgent::get_sessions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<MMAgentSession>> out;
  out.reserve(sessions_.size());
  for(const auto& kv : sessions_) {
    out.push_back(kv.second);
  }
  return out;
}

bool MMAgent::route_request_message(const MMAgentMsgQue::Message& message) {
  return session_router_ ? session_router_->route_request_message(message) : false;
}

bool MMAgent::dequeue_response_message(MMAgentMsgQue::Message& message) {
  return session_router_ ? session_router_->dequeue_response_message(message) : false;
}

bool MMAgent::enqueue_response_message(const MMAgentMsgQue::Message& message) {
  return session_router_ ? session_router_->enqueue_response_message(message) : false;
}

int MMAgent::get_response_queue_size() const {
  return session_router_ ? session_router_->get_response_queue_size() : 0;
}

void MMAgent::handle_routed_request_message(const std::shared_ptr<MMAgentSession>& session,
                                            const MMAgentMsgQue::Message& message) {
  if(!session) {
    return;
  }

  // Check for CONTROL message type
  if(message.type == MMAgentMsgQue::MessageType::CONTROL) {
    handle_control_message(session, message);
    return;
  }

  // === Direct Routing: Check if message carries dialogue_id and task_id ===
  const auto dialogue_it = message.metadata.find("dialogue_id");
  const auto task_id_it = message.metadata.find("task_id");

  if(dialogue_it != message.metadata.end() && task_id_it != message.metadata.end()) {
    const std::string& dialogue_id = dialogue_it->second;
    const std::string& task_id = task_id_it->second;

    if(!task_id.empty()) {
      // Look up the target task
      auto target_task = session->get_task(task_id);
      if(target_task && target_task->are_message_threads_running()) {
        // Determine queue based on MessageType
        // TASK_RESPONSE, EVENT, NOTIFICATION, ERROR → data_exchange queue
        // TASK_REQUEST, CONTROL → intent_request queue
        bool use_data_exchange = (message.type == MMAgentMsgQue::MessageType::TASK_RESPONSE ||
                                  message.type == MMAgentMsgQue::MessageType::EVENT ||
                                  message.type == MMAgentMsgQue::MessageType::NOTIFICATION ||
                                  message.type == MMAgentMsgQue::MessageType::ERROR);

        bool routed = false;
        if(use_data_exchange) {
          routed = target_task->enqueue_data_exchange_message(message);
        } else {
          routed = target_task->enqueue_intent_request_message(message);
        }

        if(routed) {
          Logger::instance().info_f("[DIRECT_ROUTE] Message routed directly to task=%s, dialogue_id=%s",
                                   task_id.c_str(), dialogue_id.c_str());
          return;  // Skip default routing
        }

        Logger::instance().warn_f("[DIRECT_ROUTE] Failed to enqueue to task=%s, falling back to default routing",
                                 task_id.c_str());
      } else {
        Logger::instance().info_f("[DIRECT_ROUTE] Task not found or not running: %s, falling back to default routing",
                                 task_id.c_str());
      }
    }
  }
  // === End Direct Routing ===

  if(context_) {
    context_->reserve_session_payload(session->get_session_id(), message);
  }

  const auto user_it = message.metadata.find("user_id");
  const auto client_it = message.metadata.find("client_id");
  if(user_it != message.metadata.end()) {
    session->set_metadata("user_id", user_it->second);
  }
  if(client_it != message.metadata.end()) {
    session->set_metadata("client_id", client_it->second);
  }
  const auto sandbox_ep_it = message.metadata.find("sandbox_endpoint");
  if(sandbox_ep_it != message.metadata.end()) {
    session->set_metadata("sandbox_endpoint", sandbox_ep_it->second);
  }
  // 修复(2026-08-14):sandbox_mode 与 sandbox_endpoint 同源持久化——否则
  // build_runtime_task 的 session metadata 回退分支(见下)永不触发,
  // coordinator 子进程缺 AF_SANDBOX_MODE 会落到 HostExecutor(工厂选错执行器)。
  const auto sandbox_mode_it = message.metadata.find("sandbox_mode");
  if(sandbox_mode_it != message.metadata.end()) {
    session->set_metadata("sandbox_mode", sandbox_mode_it->second);
  }
  // 2026-08-17 Step 1（path-translation-convergence.md）：sandbox_workspace 与
  // sandbox_endpoint/mode 同源持久化——build_runtime_task 的 session metadata 回退
  // 分支据此给 coordinator 子进程注入 AF_SANDBOX_WORKSPACE（当前执行环境工作目录契约）。
  const auto sandbox_ws_it = message.metadata.find("sandbox_workspace");
  if(sandbox_ws_it != message.metadata.end()) {
    session->set_metadata("sandbox_workspace", sandbox_ws_it->second);
  }
  session->set_metadata("session_id", session->get_session_id());
  session->enqueue_received_message(message);

  runtime::trace::SamplingContext sampling_ctx;
  sampling_ctx.agent_id = get_id();
  sampling_ctx.session_id = session->get_session_id();
  sampling_ctx.message_id = message.message_id;
  sampling_ctx.user_id = user_it != message.metadata.end() ? user_it->second : "";
  sampling_ctx.client_id = client_it != message.metadata.end() ? client_it->second : "";

  std::string trace_task_id = runtime::trace::TraceManager::instance().find_task(sampling_ctx);
  if(!trace_task_id.empty()) {
    std::string trace_id = runtime::trace::TraceManager::instance().begin_session(
        trace_task_id, message.message_id, sampling_ctx.user_id, sampling_ctx.client_id,
        sampling_ctx.session_id);
    runtime::trace::TraceContext::current().clear();
    runtime::trace::TraceManager::instance().end_session(trace_id);
  }
}

void MMAgent::handle_control_message(const std::shared_ptr<MMAgentSession>& session,
                                     const MMAgentMsgQue::Message& message) {
  if(!session) {
    Logger::instance().warn_f("[CONTROL] No session for control message");
    return;
  }

  const std::string& command = message.content;

  if(command == MMAgentMsgQue::ControlCommand::SESSION_INTERRUPT) {
    handle_session_interrupt(session, message);
  } else if(command == MMAgentMsgQue::ControlCommand::SESSION_DESTROY) {
    handle_session_destroy(session, message);
  } else if(command == MMAgentMsgQue::ControlCommand::DIALOGUE_INTERRUPT) {
    handle_dialogue_interrupt(session, message);
  } else {
    Logger::instance().warn_f("[CONTROL] Unknown command: %s", command.c_str());
  }
}

void MMAgent::handle_session_interrupt(const std::shared_ptr<MMAgentSession>& session,
                                       const MMAgentMsgQue::Message& message) {
  Logger::instance().info_f("[CONTROL] SESSION-INTERRUPT for session=%s",
                            session->get_session_id().c_str());

  // Get all running skill tasks
  auto tasks = session->get_tasks();
  for(auto& task : tasks) {
    if(task->get_state() == MMAgentTask::TaskState::RUNNING &&
       task->get_task_type() == "skill") {
      // Interrupt the skill task
      task->interrupt();
      task->stop_message_threads();

      // Get coordinator task to send interrupted status
      auto coordinator_task = session->get_coordinator_task();
      if(coordinator_task) {
        // Send interrupted status message to coordinator
        MMAgentMsgQue::Message interrupt_msg;
        interrupt_msg.message_id = "interrupt-" + task->get_task_id();
        interrupt_msg.type = MMAgentMsgQue::MessageType::TASK_RESPONSE;
        interrupt_msg.content = "INTERRUPTED";
        interrupt_msg.metadata["task_id"] = task->get_task_id();
        interrupt_msg.metadata["skill_name"] = task->get_skill_name();
        interrupt_msg.metadata["status"] = "interrupted";

        std::string dialogue_id;
        if(task->get_parameter("dialogue_id", dialogue_id)) {
          interrupt_msg.metadata["dialogue_id"] = dialogue_id;
        }
        interrupt_msg.metadata["session_id"] = session->get_session_id();

        // Enqueue to coordinator's data exchange queue
        coordinator_task->enqueue_data_exchange_message(interrupt_msg);
      }

      Logger::instance().info_f("[CONTROL] Interrupted skill task: %s",
                                task->get_task_id().c_str());
    }
  }
}

void MMAgent::handle_session_destroy(const std::shared_ptr<MMAgentSession>& session,
                                     const MMAgentMsgQue::Message& message) {
  Logger::instance().info_f("[CONTROL] SESSION-DESTROY for session=%s",
                            session->get_session_id().c_str());

  // 1. Stop all running tasks
  auto tasks = session->get_tasks();
  for(auto& task : tasks) {
    auto state = task->get_state();
    if(state == MMAgentTask::TaskState::RUNNING ||
       state == MMAgentTask::TaskState::PENDING) {
      task->interrupt();
      task->stop_message_threads();
    }
  }

  // 2. Stop dispatcher and coordinator tasks
  auto dispatcher_task = session->get_dispatcher_task();
  auto coordinator_task = session->get_coordinator_task();
  if(dispatcher_task) {
    dispatcher_task->interrupt();
    dispatcher_task->stop_message_threads();
  }
  if(coordinator_task) {
    coordinator_task->interrupt();
    coordinator_task->stop_message_threads();
  }

  // 3. Clear all DataBus data under /session/{session-id}
  //    P0-2：先清 tasks/dialogues 再清 databus 再发事件，避免订阅方收到 DESTROYED 时状态未完全销毁。
  //    clear_session_data_internal(emit_event=false) 只清 databus 不发事件，事件由本函数末尾发。
  int cleared_count = clear_session_data_internal(session->get_session_id(), /*emit_event=*/false);

  // 4. Clear session task maps（在发事件前清，保证订阅方收到事件时 tasks 已空）
  session->clear_all_tasks();

  // 5. Cancel all dialogue locks（在发事件前释放，保证订阅方收到事件时 dialogues 已解锁）
  session->cancel_all_dialogues();

  // 6. Emit SESSION_DESTROYED event（此时 tasks/dialogues 已清空，订阅方见一致状态）
  if(session_event_callback_) {
    session_event_callback_(session->get_session_id(), session->get_user_id(),
                            session->get_client_id(), SessionEventType::DESTROYED);
  }
  Logger::instance().info_f("[CONTROL] Cleared %d DataBus keys for session=%s",
                            cleared_count, session->get_session_id().c_str());
  Logger::instance().info_f("[CONTROL] Session destroyed: %s",
                            session->get_session_id().c_str());
}

void MMAgent::handle_dialogue_interrupt(const std::shared_ptr<MMAgentSession>& session,
                                        const MMAgentMsgQue::Message& message) {
  // Resolve target dialogue_id from metadata.
  auto it = message.metadata.find("dialogue_id");
  const std::string dialogue_id = (it != message.metadata.end()) ? it->second : "";
  if(dialogue_id.empty()) {
    Logger::instance().warn_f("[CONTROL] DIALOGUE-INTERRUPT missing dialogue_id, session=%s",
                              session->get_session_id().c_str());
    return;
  }
  Logger::instance().info_f("[CONTROL] DIALOGUE-INTERRUPT session=%s dialogue_id=%s",
                            session->get_session_id().c_str(), dialogue_id.c_str());

  // 1. Interrupt skill tasks whose dialogue_id matches.
  auto tasks = session->get_tasks();
  int interrupted = 0;
  for(auto& task : tasks) {
    std::string task_did;
    if(!task->get_parameter("dialogue_id", task_did)) {
      continue;
    }
    if(task_did != dialogue_id) {
      continue;
    }
    auto state = task->get_state();
    if(state == MMAgentTask::TaskState::RUNNING ||
       state == MMAgentTask::TaskState::PENDING) {
      task->interrupt();
      task->stop_message_threads();
      ++interrupted;

      // Notify coordinator this skill was interrupted so it can finalize.
      auto coordinator_task = session->get_coordinator_task();
      if(coordinator_task) {
        MMAgentMsgQue::Message interrupt_msg;
        interrupt_msg.message_id = "interrupt-" + task->get_task_id();
        interrupt_msg.type = MMAgentMsgQue::MessageType::TASK_RESPONSE;
        interrupt_msg.content = "INTERRUPTED";
        interrupt_msg.metadata["task_id"]        = task->get_task_id();
        interrupt_msg.metadata["skill_name"]     = task->get_skill_name();
        interrupt_msg.metadata["status"]         = "interrupted";
        interrupt_msg.metadata["dialogue_id"]    = dialogue_id;
        interrupt_msg.metadata["session_id"]     = session->get_session_id();
        coordinator_task->enqueue_data_exchange_message(interrupt_msg);
      }
    }
  }
  Logger::instance().info_f("[CONTROL] DIALOGUE-INTERRUPT interrupted %d skill tasks for dialogue_id=%s",
                            interrupted, dialogue_id.c_str());

  // 2. Push a cancellation notification so the Bridge's SSE stream can
  //    terminate gracefully (instead of hanging until wait_for_response
  //    times out).  The ResponsePoller picks this up and sends an
  //    agent.response notification with final=true, which the Bridge
  //    DaemonClient routes to _response_queues[dialogue_id].
  {
    MMAgentMsgQue::Message cancel_msg;
    cancel_msg.message_id = "cancel-" + dialogue_id;
    cancel_msg.content = "对话已被取消";
    cancel_msg.metadata["message_type"] = "agent_response";
    cancel_msg.metadata["dialogue_id"]  = dialogue_id;
    cancel_msg.metadata["session_id"]   = session->get_session_id();
    cancel_msg.metadata["final"]        = "true";
    if(session_router_) {
      session_router_->enqueue_response_message(cancel_msg);
      Logger::instance().info_f(
          "[CONTROL] DIALOGUE-INTERRUPT enqueued cancel notification for dialogue_id=%s",
          dialogue_id.c_str());
    }
  }

  // 3. Publish interrupt flag to global sys namespace BEFORE cleanup.
  //    Subprocess pollers (skill_executor / coordinator_decide) read this
  //    via databus.get_global("interrupt:{dialogue_id}") at the top of each
  //    LLM round and exit when set. MUST be written before cleanup_dialogue_data
  //    so the flag survives the dialogue-scoped subtree wipe (flag is in
  //    global sys namespace, not dialogue-scoped, but the ordering invariant
  //    I-INT-1 protects against the window where flag is unset AND data is
  //    already cleared — a subprocess polling in that window would see both
  //    empty and wrongly conclude "normally completed".
  //    Flag lifecycle: NOT cleared by dialogue-level cleanup (different prefix);
  //    cleared by session aging (remove_by_prefix "/session/{sid}/") or agent stop.
  //    See docs/design/modules/dialogue-interrupt-propagation.md §3.2.
  {
    std::string interrupt_key = "/session/" + session->get_session_id() +
                                "/sys/interrupt:" + dialogue_id;
    publish_data(interrupt_key, std::string("true"));
    Logger::instance().info_f("[CONTROL] DIALOGUE-INTERRUPT set flag key=%s", interrupt_key.c_str());
  }

  // 4. Clean up DataBus keys for this dialogue (forces coordinator path
  //    to finalize and trigger cleanup_dialogue_data, which releases the
  //    serialization lock via release_dialogue_lock).
  BuiltinDispatcher::cleanup_dialogue_data(*this, session->get_session_id(), dialogue_id);
  cleanup_multi_responses(*this, session->get_session_id(), dialogue_id);

  // 5. Belt-and-suspenders: explicitly release the lock in case
  //    cleanup_dialogue_data's normal path doesn't fire (e.g. no
  //    coordinator round was triggered yet).  Idempotent.
  session->release_dialogue_lock(dialogue_id);
}

std::shared_ptr<MMAgentTask> MMAgent::build_runtime_task(
    const std::shared_ptr<MMAgentSession>& session, const std::shared_ptr<MMAgentTask>& source_task,
    const MMAgentMsgQue::Message& message) {
  if(!session || !source_task) {
    return nullptr;
  }

  Logger::instance().info_f("[BUILD_RUNTIME_TASK] source_task_id=%s, source_workflow_id=%s, source_workflow_file=%s",
                            source_task->get_task_id().c_str(),
                            source_task->get_workflow_id().c_str(),
                            source_task->get_workflow_file().c_str());

  MMAgentTask::Config task_config = source_task->get_config();
  task_config.task_index = static_cast<int>(session->get_tasks().size());
  task_config.task_id = source_task->get_task_id() + "_runtime_" + message.message_id;
  task_config.workflow_id =
      resolve_workflow_reference(source_task->get_workflow_id().empty()
                                     ? source_task->get_workflow_file()
                                     : source_task->get_workflow_id());
  task_config.workflow_file = resolve_workflow_file(source_task->get_workflow_file());

  Logger::instance().info_f("[BUILD_RUNTIME_TASK] resolved workflow_id=%s, workflow_file=%s",
                            task_config.workflow_id.c_str(), task_config.workflow_file.c_str());

  if(const auto task_type_it = message.metadata.find("task_type");
     task_type_it != message.metadata.end() && !task_type_it->second.empty()) {
    task_config.task_type = task_type_it->second;
  }
  if(const auto workflow_it = message.metadata.find("workflow_id");
     workflow_it != message.metadata.end() && !workflow_it->second.empty()) {
    task_config.workflow_id = resolve_workflow_reference(workflow_it->second);
    task_config.workflow_file = resolve_workflow_file(workflow_it->second);
  }
  if(task_config.task_type == "skill") {
    if(const auto skill_name_it = message.metadata.find("skill_name");
       skill_name_it != message.metadata.end()) {
      task_config.skill_name = skill_name_it->second;
      task_config.task_id += "_" + skill_name_it->second;
      if(const auto workflow_it =
             session->get_config().task_workflows.find("skill:" + skill_name_it->second);
          workflow_it != session->get_config().task_workflows.end()) {
        task_config.workflow_id = resolve_workflow_reference(workflow_it->second);
        task_config.workflow_file = resolve_workflow_file(workflow_it->second);
      }
    }
    if(const auto target_session_it = message.metadata.find("target_session_id");
       target_session_it != message.metadata.end() && !target_session_it->second.empty()) {
      task_config.task_id += "_" + target_session_it->second;
    }
  }
  if(const auto so_it = message.metadata.find("collaboration_so");
     so_it != message.metadata.end() && !so_it->second.empty()) {
    task_config.collaboration_so_path = so_it->second;
  }

  auto task = std::make_shared<MMAgentTask>(task_config);
  task->set_parameter("message_id", message.message_id);
  task->set_parameter("source_task_id", source_task->get_task_id());
  task->set_parameter("session_id", session->get_session_id());

  std::string payload = message.content;
  std::string payload_ref = message.data_ref;
  {
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] message.content='%s' (full_len=%zu), message.data_ref='%s'",
                              message.content.c_str(), message.content.size(), message.data_ref.c_str());
  }
  if(payload_ref.empty() && !payload.empty()) {
    std::ostringstream logical_key;
    logical_key << "msg:" << message.message_id;
    payload_ref = dataref_bridge::store_payload(message.content, message.metadata, logical_key.str());
  } else if(payload.empty() && !payload_ref.empty()) {
    dataref_bridge::resolve_payload(payload_ref, payload);
  }
  task->set_parameter("message_data_ref", payload_ref);
  if(!payload.empty() && payload_ref.empty()) {
    std::ostringstream logical_key;
    logical_key << "msg:" << message.message_id;
    payload_ref = dataref_bridge::store_payload(payload, message.metadata, logical_key.str());
    task->set_parameter("message_data_ref", payload_ref);
  }
  if(!payload.empty()) {
    task->set_parameter("message_content", payload);
  }
  // Pass sandbox_endpoint + sandbox_mode from message metadata to task parameters
  // for worker env var injection (AF_SANDBOX_ENDPOINT + AF_SANDBOX_MODE).
  // factory.py 同时读这两个 env 才能选对 executor；漏 sandbox_mode 会让 factory 落到 HostExecutor。
  // 修复(2026-08-14)：coordinator 等 runtime 内部任务的消息 metadata 不含 sandbox_endpoint
  // （handle_intent 的注入只落在 skill 消息上），而 session metadata 已由 L966-968 保存
  // intent 请求的 sandbox_endpoint → 缺省回退 session metadata，否则 coordinator 子进程
  // 无 AF_SANDBOX_ENDPOINT，e2b 产物（write_file 生成的 html 等）无法从沙箱拉回宿主机。
  std::string sandbox_endpoint_val;
  const auto sandbox_it = message.metadata.find("sandbox_endpoint");
  if(sandbox_it != message.metadata.end() && !sandbox_it->second.empty()) {
    sandbox_endpoint_val = sandbox_it->second;
  } else if(session->get_metadata("sandbox_endpoint", sandbox_endpoint_val) && !sandbox_endpoint_val.empty()) {
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] sandbox_endpoint from session metadata for task=%s",
                              task_config.task_id.c_str());
  }
  if(!sandbox_endpoint_val.empty()) {
    task->set_parameter("sandbox_endpoint", sandbox_endpoint_val);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set sandbox_endpoint=%s for task=%s",
                              sandbox_endpoint_val.c_str(), task_config.task_id.c_str());
  }
  std::string sandbox_mode_val;
  const auto mode_it = message.metadata.find("sandbox_mode");
  if(mode_it != message.metadata.end() && !mode_it->second.empty()) {
    sandbox_mode_val = mode_it->second;
  } else if(session->get_metadata("sandbox_mode", sandbox_mode_val) && !sandbox_mode_val.empty()) {
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] sandbox_mode from session metadata for task=%s",
                              task_config.task_id.c_str());
  }
  if(!sandbox_mode_val.empty()) {
    task->set_parameter("sandbox_mode", sandbox_mode_val);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set sandbox_mode=%s for task=%s",
                              sandbox_mode_val.c_str(), task_config.task_id.c_str());
  }
  // 2026-08-17 Step 1（path-translation-convergence.md）：sandbox_workspace 回退注入——
  // 与 sandbox_endpoint/mode 同机制，缺省从 session metadata 回退（coordinator 等
  // runtime 内部任务的消息 metadata 不含它），worker 端经 inputs 自动注入 AF_SANDBOX_WORKSPACE。
  std::string sandbox_workspace_val;
  const auto ws_it = message.metadata.find("sandbox_workspace");
  if(ws_it != message.metadata.end() && !ws_it->second.empty()) {
    sandbox_workspace_val = ws_it->second;
  } else if(session->get_metadata("sandbox_workspace", sandbox_workspace_val) && !sandbox_workspace_val.empty()) {
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] sandbox_workspace from session metadata for task=%s",
                              task_config.task_id.c_str());
  }
  if(!sandbox_workspace_val.empty()) {
    task->set_parameter("sandbox_workspace", sandbox_workspace_val);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set sandbox_workspace=%s for task=%s",
                              sandbox_workspace_val.c_str(), task_config.task_id.c_str());
  }
  if(const auto selected_skills_it = message.metadata.find("selected_skills");
     selected_skills_it != message.metadata.end()) {
    task->set_parameter("selected_skills", selected_skills_it->second);
  }

  // Pass dialogue_id from message metadata to task parameters for workflow propagation
  if(const auto dialogue_id_it = message.metadata.find("dialogue_id");
     dialogue_id_it != message.metadata.end() && !dialogue_id_it->second.empty()) {
    task->set_parameter("dialogue_id", dialogue_id_it->second);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set dialogue_id=%s for task=%s",
                              dialogue_id_it->second.c_str(), task_config.task_id.c_str());
  }

  // Pass user_id / client_id to task parameters for artifact routing.
  // rpc_handler::handle_agent_execute 入口把 user_id/client_id 塞进 message.metadata；
  // worker 端把 inputs 按 AF_{KEY} 注入子进程 env（AF_USER_ID/AF_CLIENT_ID），
  // skill_executor 用 {user_id}_{client_id} 作产物路由键（artifact-storage.md §2.2）。
  // metadata 优先，session 兜底（session 也持有 active user/client）。
  const std::string* user_id_val = nullptr;
  const std::string* client_id_val = nullptr;
  if(const auto user_id_it = message.metadata.find("user_id");
     user_id_it != message.metadata.end() && !user_id_it->second.empty()) {
    user_id_val = &user_id_it->second;
  } else if(session && !session->get_user_id().empty()) {
    user_id_val = &session->get_user_id();
  }
  if(const auto client_id_it = message.metadata.find("client_id");
     client_id_it != message.metadata.end() && !client_id_it->second.empty()) {
    client_id_val = &client_id_it->second;
  } else if(session && !session->get_client_id().empty()) {
    client_id_val = &session->get_client_id();
  }
  if(user_id_val) {
    task->set_parameter("user_id", *user_id_val);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set user_id=%s for task=%s",
                              user_id_val->c_str(), task_config.task_id.c_str());
  }
  if(client_id_val) {
    task->set_parameter("client_id", *client_id_val);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set client_id=%s for task=%s",
                              client_id_val->c_str(), task_config.task_id.c_str());
  }

  // Pass trace_labels from message metadata to task parameters for flow trace correlation
  if(const auto trace_labels_it = message.metadata.find("trace_labels");
     trace_labels_it != message.metadata.end() && !trace_labels_it->second.empty()) {
    task->set_parameter("trace_labels", trace_labels_it->second);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set trace_labels=%s for task=%s",
                              trace_labels_it->second.c_str(), task_config.task_id.c_str());
  }

  // Pass trace_id from message metadata to task parameters for end-to-end correlation
  {
    bool found_trace_id = false;
    for(const auto& [k, v] : message.metadata) {
      if(k == "trace_id" && !v.empty()) found_trace_id = true;
    }
    Logger::instance().info_f("[BUILD_RUNTIME_TASK_DEBUG] metadata_keys=%zu trace_id_found=%s",
                              message.metadata.size(), found_trace_id ? "true" : "false");
    if(const auto trace_id_it = message.metadata.find("trace_id");
       trace_id_it != message.metadata.end() && !trace_id_it->second.empty()) {
      task->set_parameter("trace_id", trace_id_it->second);
      Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set trace_id=%s for task=%s",
                                 trace_id_it->second.c_str(), task_config.task_id.c_str());
    }
  }

  if(const auto platform_context_it = message.metadata.find("platform_context");
     platform_context_it != message.metadata.end() && !platform_context_it->second.empty()) {
    task->set_parameter("platform_context", platform_context_it->second);
  }
  if(const auto request_id_it = message.metadata.find("request_id");
     request_id_it != message.metadata.end() && !request_id_it->second.empty()) {
    task->set_parameter("request_id", request_id_it->second);
  }

  // Pass streaming flag from message metadata to task parameters,
  // so the node can set AF_STREAM_ENABLED via ExecutionRequest.stream.
  if(const auto stream_it = message.metadata.find("stream");
     stream_it != message.metadata.end() && stream_it->second == "true") {
    task->set_parameter("stream", "true");
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set stream=true for task=%s",
                              task_config.task_id.c_str());
  }

  // Pass completed_skill_name from message metadata for coordinator tracking
  if(const auto skill_name_it = message.metadata.find("completed_skill_name");
     skill_name_it != message.metadata.end() && !skill_name_it->second.empty()) {
    task->set_parameter("completed_skill_name", skill_name_it->second);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set completed_skill_name=%s for task=%s",
                              skill_name_it->second.c_str(), task_config.task_id.c_str());
  }

  // Pass skill_success from message metadata for coordinator workflow
  if(const auto skill_success_it = message.metadata.find("skill_success");
     skill_success_it != message.metadata.end() && !skill_success_it->second.empty()) {
    task->set_parameter("skill_success", skill_success_it->second);
    Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set skill_success=%s for task=%s",
                              skill_success_it->second.c_str(), task_config.task_id.c_str());
  }

  // Carry the completed skill payload across the workflow boundary. The UDS
  // server that produced it is destroyed with the skill workflow, so the
  // coordinator must not try to read it from that old workflow afterwards.
  if(const auto skill_result_it = message.metadata.find("skill_result");
     skill_result_it != message.metadata.end() && !skill_result_it->second.empty()) {
    task->set_parameter("skill_result", skill_result_it->second);
  }
  if(const auto artifacts_it = message.metadata.find("artifacts_json");
     artifacts_it != message.metadata.end() && !artifacts_it->second.empty()) {
    task->set_parameter("skill_artifacts", artifacts_it->second);
  }

  return task;
}

void MMAgent::handle_task_intent_request(const std::shared_ptr<MMAgentSession>& session,
                                         const std::shared_ptr<MMAgentTask>& task,
                                         const MMAgentMsgQue::Message& message) {
  if(!session || !task) {
    return;
  }

  Logger::instance().info_f("[HANDLE_INTENT_REQUEST] task_id=%s, task_type=%s",
                            task->get_task_id().c_str(), task->get_task_type().c_str());

  task->register_dispatch_record(message);
  std::vector<std::shared_ptr<MMAgentTask>> generated;

  // ============================================================
  // DISPATCHER WITH PRE-CONFIGURED next_skills (no workflow)
  // ============================================================
  // If dispatcher has pre-configured next_skills and no workflow_file,
  // directly launch skill tasks without creating a runtime task
  if(task->get_task_type() == "dispatcher") {
    const auto& configured_next_skills = task->get_next_skills();
    const auto& workflow_file = task->get_workflow_file();

    if(!configured_next_skills.empty() && workflow_file.empty()) {
      Logger::instance().info_f("[HANDLE_INTENT_REQUEST] Dispatcher with pre-configured next_skills, launching %d skills",
                                static_cast<int>(configured_next_skills.size()));

      // Preserve a caller-provided dialogue_id so daemon notifications can be
      // correlated with the originating Bridge turn. Generate one only for
      // legacy callers that omitted it.
      std::string user_text = message.content;
      std::string dialogue_id;
      const auto dialogue_it = message.metadata.find("dialogue_id");
      if(dialogue_it != message.metadata.end() && !dialogue_it->second.empty()) {
        dialogue_id = dialogue_it->second;
      } else {
        dialogue_id = std::to_string(
            std::hash<std::string>{}(user_text + message.message_id));
      }

      Logger::instance().info_f("[HANDLE_INTENT_REQUEST] Using dialogue_id=%s for message_id=%s",
                               dialogue_id.c_str(), message.message_id.c_str());

      // Acquire session-level dialogue serialization lock.  This blocks
      // until the previous dialogue in this session completes (its
      // cleanup_dialogue_data releases the lock).  The guard is
      // transferred to the session via hold_dialogue_lock() and released
      // when cleanup_dialogue_data runs for this dialogue_id.  Returns
      // nullptr if the session was cancelled (destroyed) while waiting.
      auto dlock = session->acquire_dialogue_lock(dialogue_id);
      if (!dlock) {
        Logger::instance().info_f(
            "[HANDLE_INTENT_REQUEST] session cancelled while waiting for lock, dialogue_id=%s",
            dialogue_id.c_str());
        return;
      }
      session->hold_dialogue_lock(dialogue_id, std::move(dlock));
      Logger::instance().info_f(
          "[HANDLE_INTENT_REQUEST] Acquired dialogue lock, dialogue_id=%s",
          dialogue_id.c_str());

      // Store dialogue_id and user_prompt in sys namespace with dialogue_id prefix
      // This enables multi-dialogue parallel processing in the same session
      publish_data(dialogue_id + "/dialogue_id", dialogue_id);
      publish_data(dialogue_id + "/user_prompt", user_text);

      // Emit DIALOGUE_CREATED event
      if(has_dialogue_event_callback()) {
        dialogue_event_callback_(config_.id, session->get_session_id(),
                                  dialogue_id, user_text, DialogueEventType::CREATED);
      }

      // ============================================================
      // CONTEXT CALLBACK INVOCATION
      // ============================================================
      // Invoke context callback to inject user context into the dialogue
      if(has_context_callback()) {
        ContextCallbackConfig config;
        {
          std::lock_guard<std::mutex> lock(context_callback_mutex_);
          config = context_callback_config_;
        }

        if(config.callback) {
          // Get user_id and agent_id for callback
          std::string user_id = session->get_user_id();
          std::string agent_id = config_.id;

          // Get previous context from ctx namespace (if exists)
          std::string previous_context;
          {
            DataVariant prev_ctx;
            std::string prev_ctx_key = "/session/" + session->get_session_id() +
                                       "/ctx/" + dialogue_id + "/content";
            if(get_data(prev_ctx_key, prev_ctx)) {
              if(std::holds_alternative<std::string>(prev_ctx)) {
                previous_context = std::get<std::string>(prev_ctx);
              }
            }
          }

          // Build inputs for flow trace: user_id|agent_id|user_input|previous_context
          std::string trace_inputs = user_id + "|" + agent_id + "|" +
                                     user_text + "|" + previous_context;

          if(config.async) {
            // Asynchronous invocation
            // Capture all needed data by value for the detached thread
            std::string session_id_val = session->get_session_id();
            ContextCallback callback_fn = config.callback;
            std::string user_text_val = user_text;
            std::string prev_ctx_val = previous_context;
            std::string dialogue_id_val = dialogue_id;
            std::string agent_id_val = agent_id;
            std::string user_id_val = user_id;

            // Flow trace: enter callback
            FLOW_TRACE_EXTERNAL_CALL(session_id_val, dialogue_id_val,
                                     agent_id_val, "ctx-callback",
                                     "enter-callback", trace_inputs, "");

            // Note: For async callbacks, the context is not stored in DataBus
            // because we can't safely access the agent from a detached thread.
            // The callback should either:
            // 1. Store context externally (recommended)
            // 2. Use sync mode (async=false) for automatic DataBus storage
            std::thread([callback_fn, user_id_val, agent_id_val, user_text_val,
                         prev_ctx_val, dialogue_id_val, session_id_val,
                         trace_inputs]() {
              try {
                std::string new_context = callback_fn(user_id_val, agent_id_val,
                                                      user_text_val, prev_ctx_val);

                // Flow trace: exit callback
                FLOW_TRACE_EXTERNAL_CALL(session_id_val, dialogue_id_val,
                                         agent_id_val, "ctx-callback",
                                         "exit-callback", trace_inputs, new_context);

                Logger::instance().info_f("[CONTEXT_CALLBACK] Async context generated for dialogue=%s, length=%zu. "
                                          "Note: Context not stored in DataBus - callback should handle storage.",
                                          dialogue_id_val.c_str(), new_context.length());
              } catch(const std::exception& e) {
                std::string error_msg = std::string("error: ") + e.what();
                Logger::instance().error_f("[CONTEXT_CALLBACK] Async callback failed: %s", e.what());
                // Flow trace: exit callback with error
                FLOW_TRACE_EXTERNAL_CALL(session_id_val, dialogue_id_val,
                                         agent_id_val, "ctx-callback",
                                         "exit-callback", trace_inputs, error_msg);
              }
            }).detach();
          } else {
            // Synchronous invocation
            std::string new_context;
            bool success = false;

            // Flow trace: enter callback
            FLOW_TRACE_EXTERNAL_CALL(session->get_session_id(), dialogue_id,
                                     agent_id, "ctx-callback",
                                     "enter-callback", trace_inputs, "");

            try {
              new_context = config.callback(user_id, agent_id, user_text, previous_context);
              success = true;
            } catch(const std::exception& e) {
              Logger::instance().error_f("[CONTEXT_CALLBACK] Sync callback failed: %s", e.what());
            }

            // Flow trace: exit callback
            std::string trace_outputs = success ? new_context : std::string("error");
            FLOW_TRACE_EXTERNAL_CALL(session->get_session_id(), dialogue_id,
                                     agent_id, "ctx-callback",
                                     "exit-callback", trace_inputs, trace_outputs);

            if(success && !new_context.empty()) {
              // Store result in ctx namespace with timestamp
              auto now = std::chrono::system_clock::now();
              auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count();

              std::string ctx_key = "/session/" + session->get_session_id() +
                                   "/ctx/" + dialogue_id + "/content";
              std::string time_key = "/session/" + session->get_session_id() +
                                    "/ctx/" + dialogue_id + "/time-created";

              publish_data(ctx_key, new_context);
              publish_data(time_key, std::to_string(timestamp));

              Logger::instance().info_f("[CONTEXT_CALLBACK] Sync context stored for dialogue=%s, key=%s",
                                        dialogue_id.c_str(), ctx_key.c_str());
            }
          }
        }
      }

      // Flow trace: task created
      std::string trace_labels;
      if(message.metadata.count("trace_labels")) {
        trace_labels = message.metadata.at("trace_labels");
      }
      FLOW_TRACE_TASK_L(session->get_session_id(), dialogue_id, trace_labels,
                      "external", "dispatcher",
                      runtime::FlowEventType::TASK_CREATED);

      // Create a message copy with dialogue_id in metadata for skill tasks
      MMAgentMsgQue::Message message_with_dialogue = message;
      message_with_dialogue.metadata["dialogue_id"] = dialogue_id;
      if(!trace_labels.empty()) {
        message_with_dialogue.metadata["trace_labels"] = trace_labels;
      }

      // Launch skills based on pre-configured next_skills
      for(const auto& skill_name : configured_next_skills) {
        if(!task->can_launch_more_skills()) {
          Logger::instance().info_f("[HANDLE_INTENT_REQUEST] Reached max concurrent skills limit");
          break;
        }
        if(launch_skill_task(session, skill_name, "", message_with_dialogue, task)) {
          Logger::instance().info_f("[HANDLE_INTENT_REQUEST] Launched skill: %s with dialogue_id=%s",
                                   skill_name.c_str(), dialogue_id.c_str());
        }
      }
      // Acknowledge the message to prevent monitor_loop from resending it
      // Dispatcher has completed its job (launched skills), no need to wait for response
      task->acknowledge_dispatch_record(message.message_id);
      return;  // Skip runtime task creation
    }
  }

  // Note: skill tasks are NOT created here anymore.
  // They will be created when coordinator workflow completes (in handle_task_data_exchange).
  // This ensures proper skill -> coordinator response flow.

  if(auto runtime_task = build_runtime_task(session, task, message); runtime_task) {
    Logger::instance().info_f("[HANDLE_INTENT_REQUEST] Created runtime_task: task_id=%s, workflow_id=%s",
                              runtime_task->get_task_id().c_str(), runtime_task->get_workflow_id().c_str());
    session->add_task(runtime_task);
    generated.push_back(runtime_task);

    // Emit TASK_CREATED event
    if(has_task_event_callback()) {
      std::string dialogue_id_for_task;
      // Try to get dialogue_id from message metadata
      auto it = message.metadata.find("dialogue_id");
      if(it != message.metadata.end()) {
        dialogue_id_for_task = it->second;
      }
      task_event_callback_(config_.id, session->get_session_id(),
                            dialogue_id_for_task, runtime_task->get_task_id(),
                            runtime_task->get_workflow_id(), runtime_task->get_task_type(),
                            "", TaskEventType::CREATED);
    }

    // If the runtime_task has no valid workflow, acknowledge the message now
    // since there will be no workflow execution to trigger data_exchange
    if(runtime_task->get_workflow_id().empty()) {
      Logger::instance().info_f("[HANDLE_INTENT_REQUEST] Runtime task has no workflow, acknowledging message_id=%s",
                                message.message_id.c_str());
      task->acknowledge_dispatch_record(message.message_id);
    }
  } else {
    Logger::instance().warn_f("[HANDLE_INTENT_REQUEST] build_runtime_task returned null for task=%s", task->get_task_id().c_str());
    // Acknowledge the message since no runtime task was created
    task->acknowledge_dispatch_record(message.message_id);
  }

  if(context_) {
    for(const auto& generated_task : generated) {
      if(!generated_task) {
        continue;
      }
      context_->reserve_task_payload(generated_task->get_task_id());
      if(!generated_task->get_workflow_id().empty()) {
        context_->reserve_workflow_payload(generated_task->get_workflow_id());
      }
    }
  }
  if(!generated.empty()) {
    std::lock_guard<std::mutex> lock(generated_tasks_mutex_);
    for(const auto& generated_task : generated) {
      generated_tasks_.push(generated_task);
    }
  }
}

// ---------------------------------------------------------------------------
// Scope guard for handle_task_data_exchange duration logging
// ---------------------------------------------------------------------------
namespace {

struct DataExchangeDurationGuard {
  explicit DataExchangeDurationGuard(std::string trace)
      : trace_id(std::move(trace)), start_ms(get_current_timestamp_ms()) {}
  ~DataExchangeDurationGuard() {
    int64_t dur = get_current_timestamp_ms() - start_ms;
    Logger::instance().debug_f("[DATA_EXCHANGE_DURATION] duration=%ldms trace=%s",
                               dur, trace_id.empty() ? "(none)" : trace_id.c_str());
  }
  std::string trace_id;
  int64_t start_ms;
};

} // anonymous namespace

void MMAgent::handle_task_data_exchange(const std::shared_ptr<MMAgentSession>& session,
                                        const std::shared_ptr<MMAgentTask>& task,
                                        const MMAgentMsgQue::Message& message) {
  if(!session || !task) {
    return;
  }

  // Extract trace_id from message metadata for correlation
  std::string trace_id;
  if(message.metadata.count("trace_id")) {
    trace_id = message.metadata.at("trace_id");
  }

  // Scope guard: logs DATA_EXCHANGE duration on function exit (all return paths)
  DataExchangeDurationGuard duration_guard(trace_id);

  Logger::instance().info_f("[HANDLE_DATA_EXCHANGE] task_id=%s, task_type=%s, message_id=%s trace=%s",
                            task->get_task_id().c_str(), task->get_task_type().c_str(),
                            message.message_id.c_str(),
                            trace_id.empty() ? "(none)" : trace_id.c_str());
  task->acknowledge_dispatch_record(message.message_id);

  // Get dialogue_id from message metadata for multi-dialogue support
  std::string dialogue_id;
  if(message.metadata.count("dialogue_id")) {
    dialogue_id = message.metadata.at("dialogue_id");
    Logger::instance().info_f("[HANDLE_DATA_EXCHANGE] Using dialogue_id=%s", dialogue_id.c_str());
  }

  // ============================================================
  // SKILL COMPLETION DETECTION (MUST RUN BEFORE next_skills HANDLING)
  // ============================================================
  // Detect skill completion and decrement counter before we try to launch new skills
  // NOTE: Concurrency control only applies to skill-type tasks (task_type == "skill").
  // Dispatcher and coordinator tasks do NOT use the counter.
  // IMPORTANT: Both success and failure should trigger completion handling
  bool is_skill_completion = false;
  std::string skill_name_for_counter;

  // Check task_type from message metadata to determine if this is a skill task
  // Handle both workflow_completed and workflow_failed
  if((message.content == "workflow_completed" || message.content == "workflow_failed")
     && message.metadata.count("task_type")) {
    const std::string& source_task_type = message.metadata.at("task_type");
    if(source_task_type == "skill") {
      // This is a skill task completion - decrement the counter
      is_skill_completion = true;
      // Get skill name from metadata
      if(message.metadata.count("skill_name")) {
        skill_name_for_counter = message.metadata.at("skill_name");
      }
      Logger::instance().info_f("[SKILL_COMPLETION] Detected skill task completion (success=%s), skill=%s, decrementing counter",
                               message.content == "workflow_completed" ? "true" : "false",
                               skill_name_for_counter.empty() ? "unknown" : skill_name_for_counter.c_str());
    }
  }

  // Decrement counter if this is a skill completion
  if(is_skill_completion) {
    if(!skill_name_for_counter.empty()) {
      task->on_skill_completed(skill_name_for_counter);
    } else {
      task->on_skill_completed("unknown-skill");  // fallback
    }

    // Flow trace: skill completed（与 agent.cpp:2850 SKILL_LAUNCHED 配对，from/to 同构）
    {
      std::string skill_complete_labels;
      if(message.metadata.count("trace_labels")) {
        skill_complete_labels = message.metadata.at("trace_labels");
      }
      FLOW_TRACE_SKILL_L(session->get_session_id(), dialogue_id, skill_complete_labels,
                         task->get_task_type(),
                         skill_name_for_counter.empty() ? "unknown-skill" : skill_name_for_counter,
                         runtime::FlowEventType::SKILL_COMPLETED);
    }

    // Decrement pending_skills_count in DataBus for coordinator to track
    std::string pending_count_key = dialogue_id.empty() ? "pending_skills_count" : dialogue_id + "/pending_skills_count";
    DataVariant pending_count_value;
    long pending_count = 0;
    if(get_data(pending_count_key, pending_count_value)) {
      if(const auto* count_ptr = std::get_if<int64_t>(&pending_count_value)) {
        pending_count = *count_ptr;
      } else if(const auto* count_str = std::get_if<std::string>(&pending_count_value)) {
        pending_count = std::stol(*count_str);
      }
      if(pending_count > 0) {
        pending_count--;
        publish_data(pending_count_key, DataVariant(pending_count));
      }
    }
    Logger::instance().info_f("[SKILL_COMPLETION] Decremented pending_skills_count to %ld for dialogue_id=%s",
                              pending_count, dialogue_id.c_str());

    // If task is coordinator, trigger coordinator workflow to check results
    if(task->get_task_type() == "coordinator") {
      MMAgentMsgQue::Message coordinator_trigger;
      coordinator_trigger.message_id = "coordinator-trigger-" + message.message_id;
      coordinator_trigger.metadata["task_type"] = "coordinator";
      coordinator_trigger.metadata["trigger_type"] = "skill_completion";
      const bool skill_ok = (message.content == "workflow_completed");
      coordinator_trigger.metadata["skill_success"] = skill_ok ? "true" : "false";
      // Track skill failure in DataBus so the final response can carry error status
      if(!skill_ok) {
        publish_data("/session/" + session->get_session_id() + "/sys/"
                         + (dialogue_id.empty() ? "" : dialogue_id + "/")
                         + "skill_failed",
                     std::string("true"));
        Logger::instance().warn_f("[SKILL_COMPLETION] Skill failed, setting skill_failed flag, skill=%s",
                                  skill_name_for_counter.c_str());
      }
      if(!dialogue_id.empty()) {
        coordinator_trigger.metadata["dialogue_id"] = dialogue_id;
      }
      if(const auto business_name_it = message.metadata.find("business_skill_name");
         business_name_it != message.metadata.end() && !business_name_it->second.empty()) {
        coordinator_trigger.metadata["completed_skill_name"] = business_name_it->second;
      } else if(!skill_name_for_counter.empty()) {
        coordinator_trigger.metadata["completed_skill_name"] = skill_name_for_counter;
      }
      if(const auto result_it = message.metadata.find("skill_result");
         result_it != message.metadata.end() && !result_it->second.empty()) {
        coordinator_trigger.metadata["skill_result"] = result_it->second;
      }
      if(const auto artifacts_it = message.metadata.find("artifacts_json");
         artifacts_it != message.metadata.end() && !artifacts_it->second.empty()) {
        coordinator_trigger.metadata["artifacts_json"] = artifacts_it->second;
      }
      // Pass trace_labels for flow trace correlation
      if(message.metadata.count("trace_labels")) {
        coordinator_trigger.metadata["trace_labels"] = message.metadata.at("trace_labels");
      }
      // Propagate trace_id for end-to-end correlation
      if(!trace_id.empty()) {
        coordinator_trigger.metadata["trace_id"] = trace_id;
      }
      // Get the original user prompt
      std::string user_prompt_key = dialogue_id.empty() ? "user_prompt" : dialogue_id + "/user_prompt";
      DataVariant user_prompt_value;
      if(get_data(user_prompt_key, user_prompt_value)) {
        if(const auto* text = std::get_if<std::string>(&user_prompt_value)) {
          coordinator_trigger.content = *text;
        }
      }
      // Clear stale data exchange messages from previous rounds.
      // When a new coordinator workflow is triggered, any WORKFLOW_COMPLETE
      // messages still in the queue from an older round are now stale and
      // would cause the handler to prematurely read the current DataBus state
      // (which may already have a "response" key from a completed skill).
      task->clear_data_exchange_messages();
      task->enqueue_intent_request_message(coordinator_trigger);
      Logger::instance().info_f("[SKILL_COMPLETION] Triggered coordinator workflow for skill completion, skill=%s, success=%s",
                               skill_name_for_counter.c_str(),
                               message.content == "workflow_completed" ? "true" : "false");
      return;
    }
  }

  // ============================================================
  // UNIFIED next_skills HANDLING (applies to all task types)
  // ============================================================
  // Check for next_skills in databus - this can be output by any task
  // Use new format with session isolation and fallback to legacy formats
  process_next_skills_launch(session, task, dialogue_id, message, "DATA_EXCHANGE");

  // ============================================================
  // DISPATCHER HANDLING
  // ============================================================
  if(task->get_task_type() == "dispatcher") {
    // Use the skill completion detection from the top section
    // Counter was already decremented there
    if(is_skill_completion) {
      // Skill completed - forward response to coordinator
      Logger::instance().info_f("[DISPATCHERDataExchange] Skill completion detected, skill=%s, forwarding to coordinator",
                               skill_name_for_counter.empty() ? "unknown" : skill_name_for_counter.c_str());

      // Forward response to coordinator
      if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
        MMAgentMsgQue::Message response_message = message;
        response_message.metadata["task_type"] = "coordinator";
        response_message.metadata["source_task_type"] = "dispatcher";
        // Include skill success/failure status for coordinator workflow
        bool skill_success = (message.content == "workflow_completed");
        response_message.metadata["skill_success"] = skill_success ? "true" : "false";
        if(!skill_name_for_counter.empty()) {
          response_message.metadata["skill_name"] = skill_name_for_counter;
        }
        // Preserve dialogue_id
        if(!dialogue_id.empty()) {
          response_message.metadata["dialogue_id"] = dialogue_id;
        }
        // Pass trace_labels for flow trace correlation
        if(message.metadata.count("trace_labels")) {
          response_message.metadata["trace_labels"] = message.metadata.at("trace_labels");
        }
        // Propagate trace_id for end-to-end correlation
        if(!trace_id.empty()) {
          response_message.metadata["trace_id"] = trace_id;
        }

        // Get response from databus (with dialogue_id prefix for multi-dialogue support)
        std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
        DataVariant response_value;
        if(get_data(response_key, response_value)) {
          if(const auto* text = std::get_if<std::string>(&response_value)) {
            response_message.content = *text;
            Logger::instance().info_f("[DISPATCHERDataExchange] Got skill response from databus: %s",
                                     text->c_str());
          }
        }

        coordinator_task->enqueue_response_message(response_message);

        // Trigger coordinator's data exchange
        MMAgentMsgQue::Message trigger_message;
        trigger_message.message_id = "trigger-" + message.message_id;
        trigger_message.metadata["task_type"] = "coordinator";
        trigger_message.metadata["trigger_type"] = "skill_response";
        if(!dialogue_id.empty()) {
          trigger_message.metadata["dialogue_id"] = dialogue_id;
        }
        // Pass trace_labels for flow trace correlation
        if(message.metadata.count("trace_labels")) {
          trigger_message.metadata["trace_labels"] = message.metadata.at("trace_labels");
        }
        // Propagate trace_id for end-to-end correlation
        if(!trace_id.empty()) {
          trigger_message.metadata["trace_id"] = trace_id;
        }
        coordinator_task->enqueue_data_exchange_message(trigger_message);

        Logger::instance().info_f("[DISPATCHERDataExchange] Forwarded skill response to coordinator");
      } else {
        Logger::instance().warn_f("[DISPATCHERDataExchange] No coordinator task found");
      }
      return;
    }

    if(message.content != "workflow_completed") {
      // Dispatcher workflow failed — forward error to coordinator so caller gets error response
      if(message.content == "workflow_failed") {
        Logger::instance().error_f(
            "[DISPATCHERDataExchange] Dispatcher workflow failed, forwarding error to coordinator");
        publish_data("/session/" + session->get_session_id() + "/sys/"
                         + (dialogue_id.empty() ? "" : dialogue_id + "/")
                         + "skill_failed",
                     std::string("true"));
        if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
          MMAgentMsgQue::Message error_msg = message;
          error_msg.metadata["task_type"] = "coordinator";
          error_msg.metadata["error"] = "true";
          coordinator_task->enqueue_intent_request_message(error_msg);
        }
      }
      return;
    }

    // Check if dispatcher has pre-configured next_skills (and has its own workflow)
    // This is for dispatcher WITH workflow - launches skills after workflow completes
    const auto& configured_next_skills = task->get_next_skills();
    const auto& workflow_file = task->get_workflow_file();
    if(!configured_next_skills.empty() && !workflow_file.empty()) {
      Logger::instance().info_f("[DISPATCHERDataExchange] Using pre-configured next_skills after workflow, count=%d",
                               static_cast<int>(configured_next_skills.size()));

      // Launch skills based on pre-configured next_skills
      for(const auto& skill_name : configured_next_skills) {
        if(!task->can_launch_more_skills()) {
          Logger::instance().info_f("[DISPATCHERDataExchange] Reached max concurrent skills limit");
          break;
        }
        launch_skill_task(session, skill_name, "", message, task);
      }
      return;
    }

    // No pre-configured next_skills - check for response or forward to coordinator
    // Use dialogue_id-scoped key to avoid cross-dialogue contamination (a stale
    // "response" written by a previous dialogue must NOT be read as this
    // dialogue's response).
    std::string dispatcher_response_key = dialogue_id.empty()
                                              ? "response"
                                              : dialogue_id + "/response";
    DataVariant dispatcher_response_value;
    if(get_data(dispatcher_response_key, dispatcher_response_value)) {
      if(const auto* text = std::get_if<std::string>(&dispatcher_response_value); text && !text->empty()) {
        MMAgentMsgQue::Message response_message = message;
        response_message.content = *text;
        response_message.metadata["task_type"] = "dispatcher";
        response_message.metadata["message_type"] = "dispatcher_response";
        if(session_router_) {
          Logger::instance().info_f(
              "[DISPATCHERDataExchange] Enqueueing direct dispatcher response, content='%s'",
              response_message.content.c_str());
          // Flow trace: response sent（dispatcher 直接响应出口）
          {
            std::string dispatcher_trace_labels;
            if(message.metadata.count("trace_labels")) {
              dispatcher_trace_labels = message.metadata.at("trace_labels");
            }
            FLOW_TRACE_EVENT_L("task-exec", session->get_session_id(), dialogue_id,
                               "dispatcher", "bridge", runtime::FlowEventType::RESPONSE_SENT,
                               {}, dispatcher_trace_labels);
          }
          session_router_->enqueue_response_message(response_message);
        }
        return;
      }
    }

    // Check coord_forwarded flag to prevent duplicate coordinator forwards
    // (multiple skill completions can each trigger this path)
    std::string coord_forwarded_key = dialogue_id.empty() ? "coord_forwarded" : dialogue_id + "/coord_forwarded";
    DataVariant coord_forwarded_value;
    bool already_forwarded = false;
    if(get_data(coord_forwarded_key, coord_forwarded_value)) {
      if(const auto* text = std::get_if<std::string>(&coord_forwarded_value)) {
        already_forwarded = (*text == "true");
      }
    }

    if(already_forwarded) {
      Logger::instance().info_f("[DISPATCHERDataExchange] Coordinator already forwarded for dialogue_id=%s, skipping",
                               dialogue_id.c_str());
      return;
    }

    // Set coord_forwarded flag before forwarding to prevent race condition
    publish_data(coord_forwarded_key, std::string("true"));

    // Forward to coordinator
    MMAgentMsgQue::Message coordinator_message = message;
    coordinator_message.metadata["task_type"] = "coordinator";
    coordinator_message.metadata.erase("workflow_id");
    coordinator_message.metadata.erase("workflow_instance_id");

    DataVariant original_input;
    if(get_data("input", original_input)) {
      if(const auto* text = std::get_if<std::string>(&original_input)) {
        coordinator_message.content = *text;
        Logger::instance().info_f("[DISPATCHERDataExchange] Preserved original input: %s", text->c_str());
      }
    }

    if(const auto user_id = session->get_config().session_id; !user_id.empty()) {
      coordinator_message.metadata["session_id"] = user_id;
    }

    if(context_) {
      auto reserved = context_->get_reserved_payloads();
      if(const auto reserved_it =
             reserved.session_payload_refs.find(session->get_session_id());
         reserved_it != reserved.session_payload_refs.end()) {
        coordinator_message.metadata["session_context_payload_ref"] = reserved_it->second;
      }
    }
    if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
      Logger::instance().info_f("[DISPATCHERDataExchange] Forwarding to coordinator, task_id=%s", coordinator_task->get_task_id().c_str());
      coordinator_task->enqueue_intent_request_message(coordinator_message);
    } else {
      Logger::instance().warn_f("[DISPATCHERDataExchange] No coordinator task found in session");
    }
    return;
  }

  // ============================================================
  // COORDINATOR HANDLING
  // ============================================================
  if(task->get_task_type() == "coordinator") {
    // Check if this is a response from skill (in coordinator's response_queue)
    MMAgentMsgQue::Message skill_response;
    if(task->dequeue_response_message(skill_response)) {
      // Skill completed - get result from databus and send to client
      Logger::instance().info_f("[COORDINATORDataExchange] Received skill response, message_id=%s",
                                skill_response.message_id.c_str());

      // Get dialogue_id from skill response
      std::string coord_dialogue_id;
      if(skill_response.metadata.count("dialogue_id")) {
        coord_dialogue_id = skill_response.metadata.at("dialogue_id");
      }

      // Mark skill as completed (for running count tracking)
      if(skill_response.metadata.count("skill_name")) {
        task->on_skill_completed(skill_response.metadata["skill_name"]);
      }

      // Decrement pending skills count for reference
      std::string pending_count_key = coord_dialogue_id.empty() ? "pending_skills_count" : coord_dialogue_id + "/pending_skills_count";
      DataVariant pending_count_value;
      long pending_count = 0;
      if(get_data(pending_count_key, pending_count_value)) {
        if(const auto* count_ptr = std::get_if<int64_t>(&pending_count_value)) {
          pending_count = *count_ptr;
        } else if(const auto* count_str = std::get_if<std::string>(&pending_count_value)) {
          pending_count = std::stol(*count_str);
        }
      }

      if(pending_count > 0) {
        pending_count--;
        publish_data(pending_count_key, DataVariant(pending_count));
        Logger::instance().info_f("[COORDINATORDataExchange] Decremented pending_skills_count to %ld for dialogue_id=%s",
                                  pending_count, coord_dialogue_id.c_str());
      }

      // Trigger coordinator workflow - coordinator will decide based on available results
      MMAgentMsgQue::Message coordinator_trigger;
      coordinator_trigger.message_id = "coordinator-trigger-" + skill_response.message_id;
      coordinator_trigger.metadata["task_type"] = "coordinator";
      coordinator_trigger.metadata["trigger_type"] = "skill_completion";
      if(!coord_dialogue_id.empty()) {
        coordinator_trigger.metadata["dialogue_id"] = coord_dialogue_id;
      }
      if(skill_response.metadata.count("skill_name")) {
        coordinator_trigger.metadata["completed_skill_name"] = skill_response.metadata["skill_name"];
      }
      // Include skill success/failure status from skill response
      if(skill_response.metadata.count("skill_success")) {
        coordinator_trigger.metadata["skill_success"] = skill_response.metadata["skill_success"];
      }
      // Pass trace_labels for flow trace correlation
      if(skill_response.metadata.count("trace_labels")) {
        coordinator_trigger.metadata["trace_labels"] = skill_response.metadata["trace_labels"];
      }
      // Propagate trace_id for end-to-end correlation
      if(skill_response.metadata.count("trace_id")) {
        coordinator_trigger.metadata["trace_id"] = skill_response.metadata.at("trace_id");
      }
      // Get the original user prompt
      std::string user_prompt_key = coord_dialogue_id.empty() ? "user_prompt" : coord_dialogue_id + "/user_prompt";
      DataVariant user_prompt_value;
      if(get_data(user_prompt_key, user_prompt_value)) {
        if(const auto* text = std::get_if<std::string>(&user_prompt_value)) {
          coordinator_trigger.content = *text;
        }
      }
      // Clear stale data exchange messages from previous rounds.
      // When a new coordinator workflow is triggered, any WORKFLOW_COMPLETE
      // messages still in the queue from an older round are now stale and
      // would cause the handler to prematurely read the current DataBus state
      // (which may already have a "response" key from a completed skill).
      task->clear_data_exchange_messages();
      task->enqueue_intent_request_message(coordinator_trigger);
      return;
    }

    // Coordinator workflow completed - check if need to start skill tasks
    // Check next_skills first (preferred), then selected_skills for backward compatibility
    // Use new format with session isolation and fallback to legacy formats
    bool has_skill_error = false;
    {
      DataVariant skill_error_val;
      std::string skill_error_key = "/session/" + session->get_session_id() + "/sys/"
          + (dialogue_id.empty() ? "" : dialogue_id + "/")
          + "skill_failed";
      if(get_data(skill_error_key, skill_error_val)) {
        if(const auto* text = std::get_if<std::string>(&skill_error_val)) {
          has_skill_error = (*text == "true");
        }
      }
    }
    auto result = process_next_skills_launch(session, task, dialogue_id, message, "COORDINATORDataExchange");

    // Lambda: drain any pending stream chunks from response_push.
    // Defined once, called from both found_next_skills and completed branches.
    auto drain_lambda = [&](const std::string& label) {
      if(!runtime_databus_ || !session_router_) return;
      std::stringstream stream_ss;
      stream_ss << "/session/" << session->get_session_id() << "/stream/" << dialogue_id << "/";
      std::string stream_prefix = stream_ss.str();
      auto stream_keys = runtime_databus_->list_keys_by_prefix(stream_prefix);
      for(const auto& key : stream_keys) {
        DataVariant stream_val;
        if(!runtime_databus_->get(key, stream_val)) continue;
        const auto* text = std::get_if<std::string>(&stream_val);
        if(!text || text->empty()) continue;
        // Parse the wrapper JSON to extract inner fields
        auto extract_json_field = [](const std::string& json, const std::string& field) -> std::string {
          std::string search = "\"" + field + "\":\"";
          auto pos = json.find(search);
          if(pos == std::string::npos) return "";
          pos += search.size();
          std::string result;
          for(; pos < json.size(); ++pos) {
            if(json[pos] == '\\' && pos + 1 < json.size()) {
              char next = json[pos + 1];
              switch(next) {
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case '\\': result += '\\'; break;
                case '"':  result += '"';  break;
                case '/':  result += '/';  break;
                case 'u': {
                  // \uXXXX — read 4 hex digits
                  if(pos + 5 < json.size()) {
                    std::string hex = json.substr(pos + 2, 4);
                    char* end = nullptr;
                    long cp = std::strtol(hex.c_str(), &end, 16);
                    if(end == hex.c_str() + 4 && cp > 0) {
                      if(cp <= 0x7F) {
                        result += static_cast<char>(cp);
                      } else if(cp <= 0x7FF) {
                        result += static_cast<char>(0xC0 | (cp >> 6));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                      } else {
                        result += static_cast<char>(0xE0 | (cp >> 12));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                      }
                      pos += 4; // skip the 4 hex digits (plus the initial ++pos below)
                    }
                  }
                  break;
                }
                default: result += next; break;
              }
              ++pos;
            } else if(json[pos] == '"') {
              break;
            } else {
              result += json[pos];
            }
          }
          return result;
        };
        std::string inner_content = extract_json_field(*text, "content");
        std::string inner_type = extract_json_field(*text, "type");
        // Empty content policy:
        //   llm_chunk with empty content → skip (no meaningful token)
        //   progress with empty content   → forward (clear progress indicator)
        if(inner_type == "llm_chunk" && inner_content.empty()) {
          runtime_databus_->remove(key);
          continue;
        }
        if(inner_content.empty()) continue;
        // Extract optional skill progress fields
        std::string inner_skill = extract_json_field(*text, "skill");
        std::string inner_round = extract_json_field(*text, "round");
        std::string inner_status = extract_json_field(*text, "status");
        MMAgentMsgQue::Message stream_msg = message;
        stream_msg.content = inner_content;
        stream_msg.metadata["final"] = "false";
        stream_msg.metadata["dialogue_id"] = dialogue_id;
        if(!inner_type.empty())   stream_msg.metadata["type"] = inner_type;
        if(!inner_skill.empty())  stream_msg.metadata["skill"] = inner_skill;
        if(!inner_round.empty())  stream_msg.metadata["round"] = inner_round;
        if(!inner_status.empty()) stream_msg.metadata["status"] = inner_status;
        session_router_->enqueue_response_message(stream_msg);
        runtime_databus_->remove(key);
        Logger::instance().info_f(
            "[COORDINATORDataExchange][%s] Drained stream chunk: key=%s, type=%s, content_len=%zu",
            label.c_str(), key.c_str(), inner_type.c_str(), inner_content.size());
      }
    };

    if(result.found_next_skills) {
      // ── Progress push (non-final) ───────────────────────────────────
      // When the coordinator has dispatched all skills AND a "response" key
      // exists in the databus, push a non-final notification to the frontend
      // so the user sees progress immediately.  The response key is preserved
      // so the final response path can still send it when skills complete.
      // This MUST NOT call cleanup_dialogue_data or emit DIALOGUE_DESTROYED.
      if(result.all_launched) {
        DataVariant progress_value;
        if(BuiltinDispatcher::get_framework_data(*this, session->get_session_id(),
                                                  dialogue_id, "", "response", progress_value)) {
          if(const auto* text = std::get_if<std::string>(&progress_value)) {
            MMAgentMsgQue::Message progress_msg = message;
            progress_msg.content = *text;
            // Non-final — bridge keeps polling, response_poller keeps in_flight
            progress_msg.metadata["final"] = "false";
            // Unified envelope: mark type=progress so Bridge can style it
            progress_msg.metadata["type"] = "progress";
            if(session_router_) {
              session_router_->enqueue_response_message(progress_msg);
              Logger::instance().info_f(
                  "[COORDINATORDataExchange] Pushed progress notification (non-final), "
                  "content_len=%zu",
                  progress_msg.content.size());
            }
          }
        }
      }

      // Drain stream chunks on this round (found_next_skills path)
      drain_lambda("found_next_skills");

      // If all skills are already launched AND no new skills were just launched
      // in THIS round, check for response. If we just launched new skills
      // (launched_count > 0), wait for them to complete — the pending_skills_count
      // may be stale from a previous round.
      if(result.all_launched && result.launched_count == 0) {
        Logger::instance().info_f("[COORDINATORDataExchange] All skills already launched, checking if response is ready");

        // Check pending_skills_count to ensure all skills have completed
        std::string pending_count_key = dialogue_id.empty() ? "pending_skills_count" : dialogue_id + "/pending_skills_count";
        DataVariant pending_count_value;
        long pending_count = 0;
        if(get_data(pending_count_key, pending_count_value)) {
          if(const auto* count_ptr = std::get_if<int64_t>(&pending_count_value)) {
            pending_count = *count_ptr;
          } else if(const auto* count_str = std::get_if<std::string>(&pending_count_value)) {
            pending_count = std::stol(*count_str);
          }
        }

        if(pending_count > 0) {
          Logger::instance().info_f("[COORDINATORDataExchange] %ld skills still pending, waiting for completion",
                                    pending_count);
          return;
        }

        // All skills completed - check for response
        auto multi_result = consume_multi_response(*this, session->get_session_id(), dialogue_id);
        if(multi_result.has_response) {
          Logger::instance().info_f("[COORDINATORDataExchange] Multi-response index=%d ready, sending to client",
                                    multi_result.consumed_index);
          MMAgentMsgQue::Message response_message = message;
          response_message.content = multi_result.content;
          if(multi_result.is_done) {
            response_message.metadata["final"] = "true";
          }
          if(has_skill_error) {
            response_message.metadata["error"] = "true";
          }

          if(session_router_) {
            // Flow trace: response sent（coordinator multi-response 出口）
            {
              std::string multi_trace_labels;
              if(message.metadata.count("trace_labels")) {
                multi_trace_labels = message.metadata.at("trace_labels");
              }
              FLOW_TRACE_EVENT_L("task-exec", session->get_session_id(), dialogue_id,
                                 "coordinator", "bridge", runtime::FlowEventType::RESPONSE_SENT,
                                 {}, multi_trace_labels);
            }
            session_router_->enqueue_response_message(response_message);
            mark_response_consumed(*this, session->get_session_id(), dialogue_id, multi_result.consumed_index);

            if(multi_result.is_done && multi_result.consumed_index + 1 >= 0) {
              auto check_result = consume_multi_response(*this, session->get_session_id(), dialogue_id);
              if(check_result.all_consumed && check_result.is_done) {
                Logger::instance().info_f("[COORDINATORDataExchange] All multi-responses consumed, cleaning up dialogue_id=%s",
                                          dialogue_id.c_str());
                if(has_dialogue_event_callback()) {
                  dialogue_event_callback_(config_.id, session->get_session_id(),
                                            dialogue_id, "", DialogueEventType::DESTROYED);
                }
                BuiltinDispatcher::cleanup_dialogue_data(*this, session->get_session_id(), dialogue_id);
            session->release_dialogue_lock(dialogue_id);
                cleanup_multi_responses(*this, session->get_session_id(), dialogue_id);
              }
            }
          }
          return;
        }

        // Check for single response
        DataVariant response_value;
        if(BuiltinDispatcher::get_framework_data(*this, session->get_session_id(), dialogue_id, "", "response", response_value)) {
          Logger::instance().info_f("[COORDINATORDataExchange] Response ready, sending to client");
          MMAgentMsgQue::Message response_message = message;
          if(const auto* text = std::get_if<std::string>(&response_value)) {
            response_message.content = *text;
          }
          response_message.metadata["final"] = "true";
          if(has_skill_error) {
            response_message.metadata["error"] = "true";
          }

          if(session_router_) {
            // Flow trace: response sent（coordinator single-response 出口）
            {
              std::string single_trace_labels;
              if(message.metadata.count("trace_labels")) {
                single_trace_labels = message.metadata.at("trace_labels");
              }
              FLOW_TRACE_EVENT_L("task-exec", session->get_session_id(), dialogue_id,
                                 "coordinator", "bridge", runtime::FlowEventType::RESPONSE_SENT,
                                 {}, single_trace_labels);
            }
            session_router_->enqueue_response_message(response_message);

            if(has_dialogue_event_callback()) {
              dialogue_event_callback_(config_.id, session->get_session_id(),
                                        dialogue_id, "", DialogueEventType::DESTROYED);
            }
            BuiltinDispatcher::cleanup_dialogue_data(*this, session->get_session_id(), dialogue_id);
            session->release_dialogue_lock(dialogue_id);
          }
          return;
        } else {
          Logger::instance().info_f("[COORDINATORDataExchange] Response not ready yet, more skills pending");
          return;
        }
      }

      // Skills were launched - wait for completion
      return;
    }

    // No next_skills - coordinator is done, send response to client
    Logger::instance().info_f("[COORDINATORDataExchange] Coordinator completed without skill, sending response");

    // Final drain: flush any remaining stream chunks before final response.
    // This catches chunks pushed between the last found_next_skills round
    // and the coordinator wrapping up.
    drain_lambda("completed");

    // First check for multi-responses (streaming mode)
    auto multi_result = consume_multi_response(*this, session->get_session_id(), dialogue_id);
    if(multi_result.has_response) {
      // Multi-response found - send it to client
      Logger::instance().info_f("[COORDINATORDataExchange] Multi-response index=%d ready, sending to client",
                                multi_result.consumed_index);
      MMAgentMsgQue::Message response_message = message;
      response_message.content = multi_result.content;
      if(multi_result.is_done) {
        response_message.metadata["final"] = "true";
      }
      if(has_skill_error) {
        response_message.metadata["error"] = "true";
      }

      if(session_router_) {
        session_router_->enqueue_response_message(response_message);

        // Mark as consumed
        mark_response_consumed(*this, session->get_session_id(), dialogue_id, multi_result.consumed_index);

        // If all consumed and done, clean up dialogue data
        if(multi_result.is_done) {
          // Re-check if we've consumed all responses after incrementing consumed_count
          auto check_result = consume_multi_response(*this, session->get_session_id(), dialogue_id);
          if(check_result.all_consumed && check_result.is_done) {
            Logger::instance().info_f("[COORDINATORDataExchange] All multi-responses consumed, cleaning up dialogue_id=%s",
                                      dialogue_id.c_str());
            // Emit DIALOGUE_DESTROYED event before cleanup
            if(has_dialogue_event_callback()) {
              dialogue_event_callback_(config_.id, session->get_session_id(),
                                        dialogue_id, "", DialogueEventType::DESTROYED);
            }
            // Clean up dialogue data
            BuiltinDispatcher::cleanup_dialogue_data(*this, session->get_session_id(), dialogue_id);
            session->release_dialogue_lock(dialogue_id);
            cleanup_multi_responses(*this, session->get_session_id(), dialogue_id);
          }
        }
      }
      return;
    }

    // Single response (backward compatibility)
    MMAgentMsgQue::Message response_message = message;
    // Use get_framework_data for new format with fallback support
    DataVariant response_value;
    bool has_response = false;
    if(BuiltinDispatcher::get_framework_data(*this, session->get_session_id(), dialogue_id, "", "response", response_value)) {
      if(const auto* text = std::get_if<std::string>(&response_value)) {
        response_message.content = *text;
        has_response = true;
      }
    }

    if(has_skill_error) {
      response_message.metadata["error"] = "true";
    }

    // Mark as final so the bridge knows this is the complete response
    response_message.metadata["final"] = "true";
    if(session_router_ && (has_response || has_skill_error)) {
      if(!has_response) {
        response_message.content = "error: skill execution failed";
      }
      Logger::instance().info_f("[COORDINATOR_SEND] Enqueueing response via session_router, content_len=%zu, msg_id=%s, error=%s",
                               response_message.content.size(), response_message.message_id.c_str(),
                               has_skill_error ? "true" : "false");

      // E2E summary: correlation landmarks for this dialogue
      {
        std::string summary_trace_id;
        if(response_message.metadata.count("trace_id")) {
          summary_trace_id = response_message.metadata.at("trace_id");
        }
        Logger::instance().info_f(
            "[E2E_SUMMARY] dialogue=%s trace=%s response_len=%zu has_error=%s",
            dialogue_id.empty() ? "(none)" : dialogue_id.c_str(),
            summary_trace_id.empty() ? "(none)" : summary_trace_id.c_str(),
            response_message.content.size(),
            has_skill_error ? "true" : "false");
      }
      // Flow trace: response sent（coordinator 单响应最终出口，与前端回传配对）
      {
        std::string response_trace_labels;
        if(response_message.metadata.count("trace_labels")) {
          response_trace_labels = response_message.metadata.at("trace_labels");
        } else if(message.metadata.count("trace_labels")) {
          response_trace_labels = message.metadata.at("trace_labels");
        }
        FLOW_TRACE_EVENT_L("task-exec", session->get_session_id(), dialogue_id,
                           "coordinator", "bridge", runtime::FlowEventType::RESPONSE_SENT,
                           {}, response_trace_labels);
      }
      session_router_->enqueue_response_message(response_message);

      // Clean up dialogue data after response is sent
      if(!dialogue_id.empty()) {
        Logger::instance().info_f("[COORDINATORDataExchange] Cleaning up dialogue_id=%s", dialogue_id.c_str());
        // Emit DIALOGUE_DESTROYED event before cleanup
        if(has_dialogue_event_callback()) {
          dialogue_event_callback_(config_.id, session->get_session_id(),
                                    dialogue_id, "", DialogueEventType::DESTROYED);
        }
        BuiltinDispatcher::cleanup_dialogue_data(*this, session->get_session_id(), dialogue_id);
            session->release_dialogue_lock(dialogue_id);
      }
    }
    return;
  }

  // ============================================================
  // SKILL HANDLING
  // ============================================================
  if(task->get_task_type() == "skill") {
    Logger::instance().info_f("[SKILLDataExchange] Skill workflow completed: %s", task->get_task_id().c_str());

    // Mark skill as completed on source task
    std::string source_task_id;
    if(task->get_parameter("source_task_id", source_task_id) && !source_task_id.empty()) {
      if(auto source_task = session->get_task(source_task_id)) {
        source_task->on_skill_completed(task->get_skill_name());
      }
    }

    // Get coordinator task ID from skill task parameters
    std::string coordinator_task_id;
    if(task->get_parameter("coordinator_task_id", coordinator_task_id) && !coordinator_task_id.empty()) {
      auto coordinator_task = session->get_task(coordinator_task_id);
      if(coordinator_task) {
        // Send response to coordinator's response_queue
        MMAgentMsgQue::Message coordinator_response;
        coordinator_response.message_id = task->get_task_id();
        coordinator_response.sender_id = task->get_task_id();
        coordinator_response.receiver_id = coordinator_task_id;
        coordinator_response.metadata["task_type"] = "skill_response";
        coordinator_response.metadata["skill_name"] = task->get_skill_name();
        coordinator_response.metadata["message_type"] = "skill_completed";
        // Include skill success/failure status from the original workflow completion message
        bool skill_success = (message.content == "workflow_completed");
        coordinator_response.metadata["skill_success"] = skill_success ? "true" : "false";
        coordinator_response.content = skill_success ? "skill_completed" : "skill_failed";
        // Propagate trace_id for end-to-end correlation
        if(!trace_id.empty()) {
          coordinator_response.metadata["trace_id"] = trace_id;
        }

        coordinator_task->enqueue_response_message(coordinator_response);

        // Trigger coordinator's data exchange to process the response
        MMAgentMsgQue::Message trigger_message;
        trigger_message.message_id = "trigger-" + task->get_task_id();
        trigger_message.metadata["task_type"] = "coordinator";
        trigger_message.metadata["trigger_type"] = "skill_response";
        // Propagate trace_id for end-to-end correlation
        if(!trace_id.empty()) {
          trigger_message.metadata["trace_id"] = trace_id;
        }
        coordinator_task->enqueue_data_exchange_message(trigger_message);

        Logger::instance().info_f("[SKILLDataExchange] Notified coordinator %s of skill completion",
                                  coordinator_task_id.c_str());
      } else {
        Logger::instance().warn_f("[SKILLDataExchange] Coordinator task not found: %s", coordinator_task_id.c_str());
      }
    } else {
      Logger::instance().warn_f("[SKILLDataExchange] No coordinator_task_id set for skill task");
    }
    return;
  }
}

bool MMAgent::launch_skill_task(const std::shared_ptr<MMAgentSession>& session,
                                const std::string& skill_name,
                                const std::string& exec_skill_key,
                                const MMAgentMsgQue::Message& context,
                                const std::shared_ptr<MMAgentTask>& source_task) {
  if(!session || skill_name.empty() || !source_task) {
    Logger::instance().warn_f("[LAUNCH_SKILL] Invalid parameters: session=%d, skill_name='%s', source_task=%d",
                             session ? 1 : 0, skill_name.c_str(), source_task ? 1 : 0);
    return false;
  }

  // Check concurrency limit on source task (dispatcher or coordinator)
  if(!source_task->can_launch_more_skills()) {
    Logger::instance().warn_f("[LAUNCH_SKILL] Max concurrent skills reached for task %s",
                             source_task->get_task_id().c_str());
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
    Logger::instance().warn_f("[LAUNCH_SKILL] Skill template not found: %s", skill_name.c_str());
    return false;
  }

  // ========================================
  // PARAMETER VALIDATION
  // ========================================
  const auto& expected_params = skill_template->get_expected_parameters();

  if(!expected_params.empty()) {
    // Get dialogue_id for DataBus key prefix
    std::string dialogue_id;
    if(context.metadata.count("dialogue_id")) {
      dialogue_id = context.metadata.at("dialogue_id");
    }

    // Get available_parameters (priority: metadata > DataBus)
    std::map<std::string, std::string> available_params;

    // Try metadata first (recommended path)
    if(context.metadata.count("available_parameters")) {
      available_params = parse_available_parameters_for_skill(
          context.metadata.at("available_parameters"), skill_name);
    } else {
      // Fallback to DataBus
      std::string params_key = dialogue_id.empty()
          ? "available_parameters"
          : dialogue_id + "/available_parameters";

      DataVariant params_value;
      if(get_data(params_key, params_value)) {
        if(const auto* text = std::get_if<std::string>(&params_value)) {
          available_params = parse_available_parameters_for_skill(*text, skill_name);
        }
      }
    }

    // Validate: check if all expected parameters are available
    std::vector<std::string> missing_params;
    for(const auto& expected : expected_params) {
      if(available_params.find(expected) == available_params.end()) {
        missing_params.push_back(expected);
      }
    }

    // If validation fails, notify coordinator and return
    if(!missing_params.empty()) {
      Logger::instance().info_f("[LAUNCH_SKILL] Parameter validation failed for skill '%s': %zu missing parameters",
                               skill_name.c_str(), missing_params.size());

      // Send failure message to coordinator
      MMAgentMsgQue::Message failure_message;
      failure_message.message_id = "skill-failure-" + skill_name + "-" + context.message_id;
      failure_message.sender_id = "framework";
      failure_message.receiver_id = source_task->get_task_id();  // coordinator or dispatcher
      failure_message.content = "skill_launch_failed";

      failure_message.metadata["task_type"] = "skill_response";
      failure_message.metadata["message_type"] = "skill_launch_failed";
      failure_message.metadata["skill_name"] = skill_name;
      failure_message.metadata["failure_reason"] = "parameter_validation_failed";
      failure_message.metadata["dialogue_id"] = dialogue_id;

      // Serialize expected_parameters as JSON array
#ifdef AGENTFLOW_WITH_RAPIDJSON
      {
        rapidjson::Document doc;
        doc.SetArray();
        rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
        for(const auto& param : expected_params) {
          rapidjson::Value v(param.c_str(), allocator);
          doc.PushBack(v, allocator);
        }
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);
        failure_message.metadata["expected_parameters"] = buffer.GetString();
      }
      // Serialize available_parameters as JSON object
      {
        rapidjson::Document doc;
        doc.SetObject();
        rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
        for(const auto& [k, v] : available_params) {
          rapidjson::Value key(k.c_str(), allocator);
          rapidjson::Value val(v.c_str(), allocator);
          doc.AddMember(key, val, allocator);
        }
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);
        failure_message.metadata["available_parameters"] = buffer.GetString();
      }
      // Serialize missing_parameters as JSON array
      {
        rapidjson::Document doc;
        doc.SetArray();
        rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
        for(const auto& param : missing_params) {
          rapidjson::Value v(param.c_str(), allocator);
          doc.PushBack(v, allocator);
        }
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);
        failure_message.metadata["missing_parameters"] = buffer.GetString();
      }
#else
      // Stub: simple format without JSON
      std::ostringstream oss;
      oss << "[";
      for(size_t i = 0; i < expected_params.size(); ++i) {
        if(i > 0) oss << ",";
        oss << "\"" << expected_params[i] << "\"";
      }
      oss << "]";
      failure_message.metadata["expected_parameters"] = oss.str();

      oss.str("");
      oss << "{";
      size_t i = 0;
      for(const auto& [k, v] : available_params) {
        if(i > 0) oss << ",";
        oss << "\"" << k << "\":\"" << v << "\"";
        ++i;
      }
      oss << "}";
      failure_message.metadata["available_parameters"] = oss.str();

      oss.str("");
      oss << "[";
      for(size_t j = 0; j < missing_params.size(); ++j) {
        if(j > 0) oss << ",";
        oss << "\"" << missing_params[j] << "\"";
      }
      oss << "]";
      failure_message.metadata["missing_parameters"] = oss.str();
#endif

      // Enqueue failure message to source task's data_exchange queue
      if(source_task->get_task_type() == "coordinator") {
        source_task->enqueue_data_exchange_message(failure_message);
        Logger::instance().info_f("[LAUNCH_SKILL] Sent skill_launch_failed message to coordinator for skill '%s'",
                                 skill_name.c_str());
      } else {
        source_task->enqueue_data_exchange_message(failure_message);
        Logger::instance().info_f("[LAUNCH_SKILL] Sent skill_launch_failed message to task '%s' for skill '%s'",
                                 source_task->get_task_id().c_str(), skill_name.c_str());
      }

      return false;
    }

    Logger::instance().info_f("[LAUNCH_SKILL] Parameter validation passed for skill '%s'",
                             skill_name.c_str());
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

  // Get prompt from databus if available
  DataVariant prompt_value;
  if(get_data("prompt", prompt_value)) {
    if(const auto* text = std::get_if<std::string>(&prompt_value)) {
      skill_message.content = *text;
      skill_message.metadata["prompt"] = *text;
    }
  }

  // Build runtime task
  auto skill_task = build_runtime_task(session, skill_template, skill_message);
  if(!skill_task) {
    Logger::instance().warn_f("[LAUNCH_SKILL] Failed to build runtime task for skill: %s", skill_name.c_str());
    return false;
  }

  // Store coordinator task ID for skill to notify when done
  skill_task->set_parameter("source_task_id", source_task->get_task_id());
  skill_task->set_parameter("session_id", session->get_session_id());

  // Pass exec_skill_key if provided
  if(!exec_skill_key.empty()) {
    skill_task->set_parameter("exec_skill_key", exec_skill_key);
  }

  // Pass dialogue_id as task parameter for workflow input propagation
  if(context.metadata.count("dialogue_id")) {
    skill_task->set_parameter("dialogue_id", context.metadata.at("dialogue_id"));
  }

  // Pass trace_labels as task parameter for flow trace correlation
  if(context.metadata.count("trace_labels")) {
    skill_task->set_parameter("trace_labels", context.metadata.at("trace_labels"));
  }

  // Set coordinator_task_id if source is coordinator
  if(source_task->get_task_type() == "coordinator") {
    skill_task->set_parameter("coordinator_task_id", source_task->get_task_id());
  }

  session->add_task(skill_task);

  // Emit TASK_CREATED event for skill task
  if(has_task_event_callback()) {
    std::string dialogue_id_for_task;
    if(context.metadata.count("dialogue_id")) {
      dialogue_id_for_task = context.metadata.at("dialogue_id");
    }
    task_event_callback_(config_.id, session->get_session_id(),
                          dialogue_id_for_task, skill_task->get_task_id(),
                          skill_task->get_workflow_id(), skill_task->get_task_type(),
                          skill_name, TaskEventType::CREATED);
  }

  // Mark skill as started on source task
  source_task->on_skill_started(skill_name);

  // Increment pending skills count in databus for coordinator to track
  {
    std::string dialogue_id;
    if(context.metadata.count("dialogue_id")) {
      dialogue_id = context.metadata.at("dialogue_id");
    }
    std::string pending_count_key = dialogue_id.empty() ? "pending_skills_count" : dialogue_id + "/pending_skills_count";
    DataVariant pending_count_value;
    long pending_count = 0;
    if(get_data(pending_count_key, pending_count_value)) {
      if(const auto* count_ptr = std::get_if<int64_t>(&pending_count_value)) {
        pending_count = *count_ptr;
      } else if(const auto* count_str = std::get_if<std::string>(&pending_count_value)) {
        pending_count = std::stol(*count_str);
      }
    }
    pending_count++;
    publish_data(pending_count_key, DataVariant(pending_count));
    Logger::instance().info_f("[LAUNCH_SKILL] Incremented pending_skills_count to %ld for dialogue_id=%s",
                              pending_count, dialogue_id.c_str());
  }

  // Flow trace: skill launched
  {
    std::string dialogue_id;
    if(context.metadata.count("dialogue_id")) {
      dialogue_id = context.metadata.at("dialogue_id");
    }
    std::string trace_labels;
    if(context.metadata.count("trace_labels")) {
      trace_labels = context.metadata.at("trace_labels");
    }
    FLOW_TRACE_SKILL_L(session->get_session_id(), dialogue_id, trace_labels,
                     source_task->get_task_type(), skill_name,
                     runtime::FlowEventType::SKILL_LAUNCHED);
  }

  // Add to generated tasks queue for runtime scheduling
  {
    std::lock_guard<std::mutex> lock(generated_tasks_mutex_);
    generated_tasks_.push(skill_task);
  }

  Logger::instance().info_f("[LAUNCH_SKILL] Created skill task: %s for skill: %s",
                           skill_task->get_task_id().c_str(), skill_name.c_str());

  return true;
}

MMAgent::NextSkillsLaunchResult MMAgent::process_next_skills_launch(
    const std::shared_ptr<MMAgentSession>& session,
    const std::shared_ptr<MMAgentTask>& task,
    const std::string& dialogue_id,
    const MMAgentMsgQue::Message& message,
    const std::string& log_prefix) {

  NextSkillsLaunchResult result;

  // ============================================================
  // Fetch next_skills from databus (with selected_skills backward compat)
  // ============================================================
  DataVariant next_skills_value;
  result.found_next_skills = BuiltinDispatcher::get_framework_data(
      *this, session->get_session_id(), dialogue_id, "", "next_skills", next_skills_value);

  // Backward compatibility: also check selected_skills
  if(!result.found_next_skills) {
    DataVariant selected_skills_value;
    if(BuiltinDispatcher::get_framework_data(*this, session->get_session_id(), dialogue_id, "", "selected_skills", selected_skills_value)) {
      if(const auto* text = std::get_if<std::string>(&selected_skills_value)) {
        if(!text->empty()) {
          next_skills_value = selected_skills_value;
          result.found_next_skills = true;
          Logger::instance().info_f("[%s] Using legacy 'selected_skills' (consider migrating to 'next_skills')",
                                   log_prefix.c_str());
        }
      }
    }
  }

  if(!result.found_next_skills) {
    return result;
  }

  // Parse next_skills
  auto next_skills = BuiltinDispatcher::parse_next_skills(next_skills_value);
  if(next_skills.empty()) {
    return result;
  }

  // Build keys with dialogue_id prefix
  std::string launched_key = dialogue_id.empty() ? "launched_skills" : dialogue_id + "/launched_skills";
  std::string scheduled_key = dialogue_id.empty() ? "skills_scheduled" : dialogue_id + "/skills_scheduled";

  // Load launched_skills from databus
  // Key 维度 = "name:exec_skill_key"——区分同一 skill-runner task 下不同 exec_skill_key
  // 的调度实例（实测 2026-08-11 死链：维度只看 name 导致第二轮 outfit-stylist 调度被
  // 误判为"重排 forge-image-gen"。F4 铐律：所有消费点已同步迁移到复合 key 维度）。
  DataVariant launched_value;
  if(get_data(launched_key, launched_value)) {
    if(const auto* text = std::get_if<std::string>(&launched_value)) {
      std::istringstream iss(*text);
      std::string launched_key_entry;
      while(std::getline(iss, launched_key_entry, ',')) {
        if(!launched_key_entry.empty()) {
          result.launched_skills.insert(launched_key_entry);
        }
      }
    }
  }

  // ============================================================
  // FORCE RE-LAUNCH HANDLING
  // ============================================================
  // Use set to avoid duplicates when collecting force skills
  // launched_skills 维度 = "name:exec_skill_key"，force 判断必须用复合 key，
  // 否则同一 skill-runner task 下不同 exec_skill_key 的调度会被误判为"已 launch"。
  std::set<std::string> force_skill_names_set;
  bool has_force_skills = false;

  for(const auto& spec : next_skills) {
    std::string launched_key_for_spec = spec.exec_skill_key.empty()
        ? spec.name
        : spec.name + ":" + spec.exec_skill_key;
    if(spec.force && result.launched_skills.count(launched_key_for_spec)) {
      has_force_skills = true;
      force_skill_names_set.insert(spec.name);

      // Remove from launched_skills set
      result.launched_skills.erase(launched_key_for_spec);

      // Check if skill has result (completed) or is still pending
      std::string result_key = dialogue_id.empty()
          ? spec.name + "_result"
          : dialogue_id + "/" + spec.name + "_result";
      DataVariant result_value;
      bool has_result = get_data(result_key, result_value);

      // Clear skill result data
      BuiltinDispatcher::remove_user_data(*this, session->get_session_id(),
                                           dialogue_id, spec.name + "_result");
      BuiltinDispatcher::remove_user_data(*this, session->get_session_id(),
                                           dialogue_id, spec.name + "_confidence");

      // Decrement pending_skills_count if skill was still executing
      if(!has_result) {
        std::string pending_key = dialogue_id.empty()
            ? "pending_skills_count"
            : dialogue_id + "/pending_skills_count";
        DataVariant pending_value;
        long pending_count = 0;
        if(get_data(pending_key, pending_value)) {
          if(const auto* count_ptr = std::get_if<int64_t>(&pending_value)) {
            pending_count = *count_ptr;
          } else if(const auto* count_str = std::get_if<std::string>(&pending_value)) {
            pending_count = std::stol(*count_str);
          }
          if(pending_count > 0) {
            pending_count--;
            publish_data(pending_key, DataVariant(pending_count));
          }
        }
        Logger::instance().info_f("[%s] Force re-launch for executing skill=%s, decremented pending_count=%ld",
                                 log_prefix.c_str(), spec.name.c_str(), pending_count);
      } else {
        Logger::instance().info_f("[%s] Force re-launch for completed skill=%s, pending_count unchanged",
                                 log_prefix.c_str(), spec.name.c_str());
      }
    }
  }

  // Reset skills_scheduled flag if any force skills
  if(has_force_skills) {
    publish_data(scheduled_key, std::string("false"));
    Logger::instance().info_f("[%s] Force skills detected, reset skills_scheduled for dialogue_id=%s",
                             log_prefix.c_str(), dialogue_id.c_str());
  }

  // Filter out already launched skills
  // launched_skills 维度 = "name:exec_skill_key"，复合 key 判重。
  std::vector<SkillSpec> skills_to_launch;
  for(const auto& spec : next_skills) {
    std::string launched_key_for_spec = spec.exec_skill_key.empty()
        ? spec.name
        : spec.name + ":" + spec.exec_skill_key;
    if(result.launched_skills.find(launched_key_for_spec) == result.launched_skills.end()) {
      skills_to_launch.push_back(spec);
    }
  }

  if(skills_to_launch.empty()) {
    Logger::instance().info_f("[%s] All skills already launched for dialogue_id=%s",
                             log_prefix.c_str(), dialogue_id.c_str());
    result.all_launched = true;
    return result;
  }

  // Build skill names list for logging
  std::string skills_to_launch_names;
  for(const auto& spec : skills_to_launch) {
    if(!skills_to_launch_names.empty()) skills_to_launch_names += ",";
    skills_to_launch_names += spec.name;
  }
  Logger::instance().info_f("[%s] Found %d skills to launch for dialogue_id=%s: [%s] (%d already launched)",
                           log_prefix.c_str(),
                           static_cast<int>(skills_to_launch.size()),
                           dialogue_id.c_str(),
                           skills_to_launch_names.c_str(),
                           static_cast<int>(result.launched_skills.size()));

  // Create message copy with dialogue_id for skill tasks
  MMAgentMsgQue::Message message_with_dialogue = message;
  if(dialogue_id.empty() && message.metadata.count("dialogue_id")) {
    message_with_dialogue.metadata["dialogue_id"] = message.metadata.at("dialogue_id");
  } else if(!dialogue_id.empty()) {
    message_with_dialogue.metadata["dialogue_id"] = dialogue_id;
  }
  // Preserve trace_labels for flow trace correlation across skill boundaries
  if(message.metadata.count("trace_labels")) {
    message_with_dialogue.metadata["trace_labels"] = message.metadata.at("trace_labels");
  }

  // Launch skills
  for(const auto& spec : skills_to_launch) {
    if(!task->can_launch_more_skills()) {
      // Build remaining skill names for logging
      std::string remaining_names;
      for(size_t i = result.launched_count; i < skills_to_launch.size(); ++i) {
        if(!remaining_names.empty()) remaining_names += ",";
        remaining_names += skills_to_launch[i].name;
      }
      Logger::instance().info_f("[%s] Reached max concurrent skills limit, %d skills still pending: [%s]",
                               log_prefix.c_str(),
                               static_cast<int>(skills_to_launch.size()) - result.launched_count,
                               remaining_names.c_str());
      break;
    }
    if(launch_skill_task(session, spec.name, spec.exec_skill_key, message_with_dialogue, task)) {
      // launched_skills 维度 = "name:exec_skill_key"，记录复合 key 避免不同
      // exec_skill_key 的 skill-runner 调度实例被误判为同一已 launch skill。
      std::string launched_key_for_spec = spec.exec_skill_key.empty()
          ? spec.name
          : spec.name + ":" + spec.exec_skill_key;
      result.launched_skills.insert(launched_key_for_spec);
      result.launched_count++;
      result.launched_any = true;
      // Collect force skills to reset their flags (set avoids duplicates)
      if(spec.force) {
        force_skill_names_set.insert(spec.name);
      }
    }
  }

  // Update launched_skills list in databus
  std::string launched_list;
  for(const auto& name : result.launched_skills) {
    if(!launched_list.empty()) launched_list += ",";
    launched_list += name;
  }
  publish_data(launched_key, launched_list);

  // Reset force flags immediately after launch
  if(!force_skill_names_set.empty()) {
    if(const auto* json_str = std::get_if<std::string>(&next_skills_value)) {
      std::vector<std::string> force_skill_names(force_skill_names_set.begin(), force_skill_names_set.end());
      std::string updated_next_skills = BuiltinDispatcher::reset_force_flags(*json_str, force_skill_names);
      if(updated_next_skills != *json_str) {
        BuiltinDispatcher::publish_framework_data(*this, session->get_session_id(),
                                                   dialogue_id, "next_skills",
                                                   DataVariant(updated_next_skills));
        Logger::instance().info_f("[%s] Reset force flags for %zu skills",
                                  log_prefix.c_str(), force_skill_names.size());
      }
    }
  }

  // Check if all skills have been launched
  // launched_skills 维度 = "name:exec_skill_key"，复合 key 判全 launch 完成。
  result.all_launched = true;
  for(const auto& spec : next_skills) {
    std::string launched_key_for_spec = spec.exec_skill_key.empty()
        ? spec.name
        : spec.name + ":" + spec.exec_skill_key;
    if(result.launched_skills.find(launched_key_for_spec) == result.launched_skills.end()) {
      result.all_launched = false;
      break;
    }
  }

  if(result.all_launched) {
    publish_data(scheduled_key, std::string("true"));
    Logger::instance().info_f("[%s] All skills launched for dialogue_id=%s",
                             log_prefix.c_str(), dialogue_id.c_str());
  } else {
    Logger::instance().info_f("[%s] %d/%d skills launched for dialogue_id=%s, will retry remaining on next trigger",
                             log_prefix.c_str(),
                             static_cast<int>(result.launched_skills.size()),
                             static_cast<int>(next_skills.size()),
                             dialogue_id.c_str());
  }

  return result;
}

int MMAgent::process_session_messages(const std::string& session_key) {
  auto session = get_session_by_key(session_key);
  if(!session) {
    return 0;
  }
  int total_tasks = 0;
  if(auto dispatcher = session->get_dispatcher_task();
     dispatcher && !dispatcher->are_message_threads_running()) {
    total_tasks += dispatcher->process_pending_messages_once();
  }
  if(auto coordinator = session->get_coordinator_task();
     coordinator && !coordinator->are_message_threads_running()) {
    total_tasks += coordinator->process_pending_messages_once();
  }
  return total_tasks;
}

int MMAgent::process_all_sessions_until_idle() {
  if(session_router_) {
    session_router_->drain_pending_requests();
  }

  int total_tasks = 0;
  int rounds = 0;
  while(rounds++ < 64) {
    const int round_tasks = process_intent_request_messages() + process_data_exchange_messages();
    auto snapshot = get_sessions();
    total_tasks += round_tasks;

    bool queues_empty = true;
    for(const auto& session : snapshot) {
      if(session && (session->get_receive_queue_size() > 0 || session->get_send_queue_size() > 0)) {
        queues_empty = false;
        break;
      }
    }
    if(queues_empty && round_tasks == 0) {
      break;
    }
  }
  return total_tasks;
}

bool MMAgent::pop_generated_task(std::shared_ptr<MMAgentTask>& task) {
  std::lock_guard<std::mutex> lock(generated_tasks_mutex_);
  if(generated_tasks_.empty()) {
    return false;
  }
  task = generated_tasks_.front();
  generated_tasks_.pop();
  return static_cast<bool>(task);
}

void MMAgent::add_workflow(std::shared_ptr<MMWorkflow> workflow) {
  if(workflow && workflow_map_.size() < static_cast<size_t>(config_.max_workflows)) {
    workflow_map_[workflow->get_id()] = workflow;
    workflows_.push_back(workflow);
  }
}

void MMAgent::remove_workflow(const WorkflowId& workflow_id) {
  const auto it = workflow_map_.find(workflow_id);
  if(it != workflow_map_.end()) {
    workflow_map_.erase(it);
    workflows_.erase(std::remove_if(workflows_.begin(), workflows_.end(),
                                    [&workflow_id](const std::shared_ptr<MMWorkflow>& workflow) {
                                      return workflow->get_id() == workflow_id;
                                    }),
                     workflows_.end());
  }
}

std::shared_ptr<MMWorkflow> MMAgent::get_workflow(const WorkflowId& workflow_id) {
  const auto it = workflow_map_.find(workflow_id);
  return it != workflow_map_.end() ? it->second : nullptr;
}

bool MMAgent::initialize_resources() {
  state_ = AgentState::INITIALIZED;
  return true;
}

void MMAgent::cleanup_resources() {
  // Clear internal data_bus_ map
  size_t map_count = data_bus_.size();
  data_bus_.clear();

  // Clear runtime DataBus if available (cover all session data under /session/)
  int bus_count = 0;
  if(runtime_databus_) {
    // Iterate known sessions and clear their prefix data
    for(const auto& sid : get_all_session_ids()) {
      std::string prefix = "/session/" + sid + "/";
      bus_count += runtime_databus_->remove_by_prefix(prefix);
    }
    // Also clean up entries that may belong to already-destroyed sessions
    // (belt-and-suspenders: /session/ prefix sweep)
    bus_count += runtime_databus_->remove_by_prefix("/session/");
  }

  Logger::instance().info_f(
      "[MMAgent] cleanup_resources: cleared %zu data_bus_ keys, %d runtime_databus_ keys for agent=%s",
      map_count, bus_count, config_.id.c_str());
}

std::vector<std::string> MMAgent::get_all_session_ids() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  ids.reserve(sessions_.size());
  for(const auto& kv : sessions_) {
    ids.push_back(kv.first);
  }
  return ids;
}

bool MMAgent::get_session_output(const std::string& session_id, const std::string& key,
                                 DataVariant& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string namespaced_key = "session:" + session_id + ":" + key;
  const auto it = data_bus_.find(namespaced_key);
  if(it == data_bus_.end()) {
    return false;
  }
  value = it->second;
  return true;
}

bool MMAgent::publish_data(const std::string& key, const DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Prefer MMDataBus if available (unified storage with nodes)
  if(runtime_databus_) {
    runtime_databus_->put(key, value);
    return true;
  }

  // Fallback to internal map
  data_bus_[key] = value;
  return true;
}

bool MMAgent::get_data(const std::string& key, DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Prefer MMDataBus if available (unified storage with nodes)
  if(runtime_databus_) {
    return runtime_databus_->get(key, value);
  }

  // Fallback to internal map
  const auto it = data_bus_.find(key);
  if(it == data_bus_.end()) {
    return false;
  }
  value = it->second;
  return true;
}

bool MMAgent::initialize() {
  if(!initialize_resources()) {
    return false;
  }
  for(auto& workflow : workflows_) {
    if(!workflow->validate()) {
      return false;
    }
  }
  state_ = AgentState::INITIALIZED;
  return true;
}

bool MMAgent::start() {
  if(state_ != AgentState::INITIALIZED) {
    return false;
  }
  if(session_router_) {
    session_router_->start();
  }
  for(const auto& session : get_sessions()) {
    if(!session) {
      continue;
    }
    if(auto dispatcher_task = session->get_dispatcher_task(); dispatcher_task) {
      dispatcher_task->start_message_threads(
          [this, session, dispatcher_task](const MMAgentMsgQue::Message& message) {
            handle_task_intent_request(session, dispatcher_task, message);
          },
          [this, session, dispatcher_task](const MMAgentMsgQue::Message& message) {
            handle_task_data_exchange(session, dispatcher_task, message);
          });
    }
    if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
      coordinator_task->start_message_threads(
          [this, session, coordinator_task](const MMAgentMsgQue::Message& message) {
            handle_task_intent_request(session, coordinator_task, message);
          },
          [this, session, coordinator_task](const MMAgentMsgQue::Message& message) {
            handle_task_data_exchange(session, coordinator_task, message);
          });
    }
  }
  bool expected = false;
  if(message_threads_running_.compare_exchange_strong(expected, true)) {
    intent_request_thread_ = std::thread(&MMAgent::process_intent_request_loop, this);
    data_exchange_thread_ = std::thread(&MMAgent::process_data_exchange_loop, this);
  }
  state_ = AgentState::RUNNING;
  return true;
}

bool MMAgent::pause() {
  if(state_ != AgentState::RUNNING) {
    return false;
  }
  state_ = AgentState::PAUSED;
  return true;
}

bool MMAgent::resume() {
  if(state_ != AgentState::PAUSED) {
    return false;
  }
  state_ = AgentState::RUNNING;
  return true;
}

bool MMAgent::stop() {
  if(state_ != AgentState::RUNNING && state_ != AgentState::PAUSED) {
    return false;
  }
  bool expected = true;
  if(message_threads_running_.compare_exchange_strong(expected, false)) {
    if(intent_request_thread_.joinable()) {
      intent_request_thread_.join();
    }
    if(data_exchange_thread_.joinable()) {
      data_exchange_thread_.join();
    }
  }
  for(const auto& session : get_sessions()) {
    if(session) {
      if(auto dispatcher_task = session->get_dispatcher_task(); dispatcher_task) {
        dispatcher_task->stop_message_threads();
      }
      if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
        coordinator_task->stop_message_threads();
      }
    }
  }
  if(session_router_) {
    session_router_->stop();
  }
  cleanup_resources();
  state_ = AgentState::STOPPED;
  return true;
}

int MMAgent::process_intent_request_messages() {
  int total_tasks = 0;
  auto snapshot = get_sessions();
  for(const auto& session : snapshot) {
    if(!session) {
      continue;
    }

    auto dispatcher = session->get_task_dispatcher();
    if(!dispatcher) {
      continue;
    }
    auto dispatcher_task = session->get_dispatcher_task();
    if(dispatcher_task && !dispatcher_task->are_message_threads_running()) {
      total_tasks += dispatcher_task->process_pending_messages_once();
    } else if(dispatcher_task) {
      total_tasks += dispatcher_task->get_intent_request_queue_size();
    }
  }
  return total_tasks;
}

int MMAgent::process_data_exchange_messages() {
  int total_tasks = 0;
  auto snapshot = get_sessions();
  for(const auto& session : snapshot) {
    if(!session) {
      continue;
    }

    auto coordinator = session->get_task_coordinator();
    if(!coordinator) {
      continue;
    }
    auto coordinator_task = session->get_coordinator_task();
    if(coordinator_task && !coordinator_task->are_message_threads_running()) {
      total_tasks += coordinator_task->process_pending_messages_once();
    } else if(coordinator_task) {
      total_tasks += coordinator_task->get_data_exchange_queue_size();
    }
  }
  return total_tasks;
}

void MMAgent::process_intent_request_loop() {
  while(message_threads_running_) {
    int processed = 0;
    if(session_router_) {
      processed += session_router_->drain_pending_requests();
    }
    processed += process_intent_request_messages();
    if(processed == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

void MMAgent::process_data_exchange_loop() {
  int64_t queue_log_counter = 0;
  while(message_threads_running_) {
    int processed = process_data_exchange_messages();
    if(processed == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Periodic queue depth logging (~every 1 second at 10ms idle sleep)
    if(++queue_log_counter % 100 == 0) {
      auto snapshot = get_sessions();
      for(const auto& session : snapshot) {
        if(!session) continue;
        auto router = session_router_;
        int request_depth = router ? router->get_request_queue_size() : -1;
        auto coordinator_task = session->get_coordinator_task();
        int data_ex_depth = coordinator_task ? coordinator_task->get_data_exchange_queue_size() : -1;
        auto dispatcher_task = session->get_dispatcher_task();
        int dispatcher_dex_depth = dispatcher_task ? dispatcher_task->get_data_exchange_queue_size() : -1;
        // Logger::instance().debug_f(
        //     "[QUEUE_DEPTH] session=%s request=%d coord_data_ex=%d dispatcher_data_ex=%d",
        //     session->get_session_id().c_str(),
        //     request_depth, data_ex_depth, dispatcher_dex_depth);
      }
    }
  }
}

storage::StorageResult MMAgent::save(const storage::StorageConfig& config,
                                   const storage::VersionInfo&   version_info) {
  auto version_storage = utils::storage::create_versioned_storage(config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  // Convert to JSON
  std::string json_data = agent_config_to_json(config_, workflows_);

  // Save version
  utils::storage::StorageResult result =
      version_storage->save_version(config_.id, json_data, version_info);
  if(!result.is_success()) {
    return result;
  }

  // Save as current data
  result = version_storage->save(config_.id, json_data);
  if(!result.is_success()) {
    return result;
  }

  // Set as active version
  return version_storage->set_active_version(config_.id, version_info.version);
}

utils::storage::StorageResult MMAgent::load(const std::string&            agent_id,
                                   const utils::storage::StorageConfig& storage_config,
                                   std::shared_ptr<MMAgent>&       agent) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  std::string            json_data;
  utils::storage::StorageResult result = version_storage->load(agent_id, json_data);
  if(!result.is_success()) {
    return result;
  }

  MMAgent::Config                 config;
  std::vector<MMWorkflow::Config> workflow_configs;

  if(!agent_config_from_json(json_data, config, workflow_configs)) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_INVALID_DATA,
                                   "Failed to parse agent config" };
  }

  // Create agent
  agent = std::make_shared<MMAgent>(config);

  // Note: Workflows are not fully restored here as MMWorkflow::Config only has basic info
  // and we don't have the MMWorkflow factory or registry here to recreate them.
  // In a real implementation, we would need to load workflows from their DSL files
  // or store full workflow definitions.
  // For now, we just restore the agent config.

  return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_OK, "" };
}

utils::storage::StorageResult MMAgent::delete_agent(const std::string&            agent_id,
                                           const utils::storage::StorageConfig& storage_config) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  return version_storage->remove(agent_id);
}

utils::storage::StorageResult MMAgent::list_agents(const utils::storage::StorageConfig& storage_config,
                                          std::vector<std::string>&     agent_ids) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  return version_storage->list("", agent_ids);
}

utils::storage::StorageResult MMAgent::get_metadata(const std::string&            agent_id,
                                           const utils::storage::StorageConfig& storage_config,
                                           AgentMetadata&                metadata) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }

  std::string            json_data;
  utils::storage::StorageResult result = version_storage->load(agent_id, json_data);
  if(!result.is_success()) {
    return result;
  }

  MMAgent::Config                 config;
  std::vector<MMWorkflow::Config> workflow_configs;

  if(!agent_config_from_json(json_data, config, workflow_configs)) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_INVALID_DATA,
                                   "Failed to parse agent config" };
  }

  metadata.id          = config.id;
  metadata.name        = config.name;
  metadata.version     = config.version;
  metadata.description = config.description;
  metadata.created_at  = 0;
  metadata.updated_at  = get_current_timestamp_ms();
  metadata.author      = "";

  metadata.workflow_ids.clear();
  for(const auto& wf_config : workflow_configs) {
    metadata.workflow_ids.push_back(wf_config.id);
  }

  return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_OK, "" };
}

utils::storage::StorageResult MMAgent::set_active_version(const AgentId&                agent_id,
                                                 const std::string&            version,
                                                 const utils::storage::StorageConfig& storage_config) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }
  return version_storage->set_active_version(agent_id, version);
}

utils::storage::StorageResult MMAgent::get_active_version(const AgentId&                agent_id,
                                                 const utils::storage::StorageConfig& storage_config,
                                                 std::string&                  version) {
  auto version_storage = utils::storage::create_versioned_storage(storage_config);
  if(!version_storage) {
    return utils::storage::StorageResult{ utils::storage::StorageError::STORAGE_ERROR_IO,
                                   "Version storage not initialized" };
  }
  return version_storage->get_active_version(agent_id, version);
}

// ============================================================================
// Context Callback Configuration
// ============================================================================

void MMAgent::set_context_callback(const ContextCallbackConfig& config) {
  std::lock_guard<std::mutex> lock(context_callback_mutex_);
  context_callback_config_ = config;
  Logger::instance().info_f("[CONTEXT_CALLBACK] Set context callback, async=%s",
                            config.async ? "true" : "false");
}

void MMAgent::set_context_callback(ContextCallback callback, bool async) {
  ContextCallbackConfig config;
  config.callback = std::move(callback);
  config.async = async;
  set_context_callback(config);
}

void MMAgent::clear_context_callback() {
  std::lock_guard<std::mutex> lock(context_callback_mutex_);
  context_callback_config_.callback = nullptr;
  context_callback_config_.async = true;
  Logger::instance().info_f("[CONTEXT_CALLBACK] Cleared context callback");
}

bool MMAgent::has_context_callback() const {
  std::lock_guard<std::mutex> lock(context_callback_mutex_);
  return static_cast<bool>(context_callback_config_.callback);
}

void MMAgent::set_session_event_callback(SessionEventCallback callback) {
  session_event_callback_ = std::move(callback);

  // Set the callback on all existing sessions
  if(session_router_) {
    auto sessions = get_sessions();
    for(auto& session : sessions) {
      if(session) {
        session->set_session_event_callback(session_event_callback_);
      }
    }
  }
}

void MMAgent::clear_session_event_callback() {
  session_event_callback_ = nullptr;

  // Clear the callback on all existing sessions
  if(session_router_) {
    auto sessions = get_sessions();
    for(auto& session : sessions) {
      if(session) {
        session->clear_session_event_callback();
      }
    }
  }
}

void MMAgent::set_dialogue_event_callback(DialogueEventCallback callback) {
  dialogue_event_callback_ = std::move(callback);
}

void MMAgent::clear_dialogue_event_callback() {
  dialogue_event_callback_ = nullptr;
}

bool MMAgent::has_dialogue_event_callback() const {
  return static_cast<bool>(dialogue_event_callback_);
}

void MMAgent::set_task_event_callback(TaskEventCallback callback) {
  task_event_callback_ = std::move(callback);
}

void MMAgent::clear_task_event_callback() {
  task_event_callback_ = nullptr;
}

bool MMAgent::has_task_event_callback() const {
  return static_cast<bool>(task_event_callback_);
}

int MMAgent::clear_session_data_internal(const std::string& session_id, bool emit_event) {
  int total_count = 0;

  // 1. Clear runtime_databus_ data under /session/{session_id}/
  if(runtime_databus_) {
    std::string prefix = "/session/" + session_id + "/";
    total_count += runtime_databus_->remove_by_prefix(prefix);
  } else {
    Logger::instance().warn_f("[MMAgent] No runtime databus for session cleanup: %s",
                              session_id.c_str());
  }

  // 2. Clear data_bus_ entries with matching session prefix
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string session_prefix = "/session/" + session_id;
    int cleared = 0;
    auto it = data_bus_.begin();
    while(it != data_bus_.end()) {
      if(it->first.compare(0, session_prefix.size(), session_prefix) == 0) {
        it = data_bus_.erase(it);
        ++cleared;
      } else {
        ++it;
      }
    }
    total_count += cleared;
  }

  // 3. Emit SESSION_DESTROYED event so external subscribers (EventBroker / Bridge)
  //    are notified.  Look up the session to fill user_id / client_id.
  //    P0-2：emit_event=false 时跳过，让 handle_session_destroy 控制事件时机。
  if(emit_event && session_event_callback_) {
    std::string user_id;
    std::string client_id;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto sit = sessions_.find(session_id);
      if(sit != sessions_.end() && sit->second) {
        user_id   = sit->second->get_user_id();
        client_id = sit->second->get_client_id();
      }
    }
    session_event_callback_(session_id, user_id, client_id, SessionEventType::DESTROYED);
    Logger::instance().info_f("[MMAgent] Emitted SESSION_DESTROYED for session=%s user=%s client=%s",
                              session_id.c_str(), user_id.c_str(), client_id.c_str());
  }

  return total_count;
}

int MMAgent::clear_session_data(const std::string& session_id) {
  // P0-2：公开接口保持发事件（外部主动调时订阅方应收到）
  int total_count = clear_session_data_internal(session_id, /*emit_event=*/true);
  Logger::instance().info_f("[MMAgent] clear_session_data: session=%s total_keys=%d",
                            session_id.c_str(), total_count);
  return total_count;
}

int MMAgent::clear_session_data_if_unused(const std::string& session_id, uint64_t expected_seq) {
  // P0-1 方案 D：aging callback 延迟到锁外执行，归还后 session 可能已被新用户复用。
  // 若复用了（activate() 自增了 active_seq），当前 seq != expected_seq → 跳过清理，
  // 否则会误删新用户刚写入的数据。未复用则 seq 不变，正常清理。
  auto session = get_session_by_key(session_id);
  if(!session) {
    // session 不在 sessions_ map（可能尚未注册或已析构）——视为未复用，正常清
    return clear_session_data(session_id);
  }
  const uint64_t current_seq = session->get_active_seq();
  if(current_seq != expected_seq) {
    Logger::instance().info_f(
        "[MMAgent] clear_session_data_if_unused: SKIP session=%s (seq mismatch: expected=%llu current=%llu, "
        "session已复用给新用户，跳过清理避免误删新数据)",
        session_id.c_str(),
        static_cast<unsigned long long>(expected_seq),
        static_cast<unsigned long long>(current_seq));
    return 0;
  }
  return clear_session_data(session_id);
}

int MMAgent::clear_dialogue_data(const std::string& session_id,
                                  const std::string& dialogue_id) {
  int total_count = 0;

  // 1. Clear runtime_databus_ dialogue namespace keys
  if(runtime_databus_) {
    // Clean all dialogue namespaces: sys, user, ctx, llm, tool
    for(const auto& ns : DIALOGUE_NAMESPACES) {
      std::string prefix = "/session/" + session_id + "/" + ns + "/" + dialogue_id + "/";
      total_count += runtime_databus_->remove_by_prefix(prefix);
    }
    // Also clean stream chunks for this dialogue
    std::string stream_prefix = "/session/" + session_id + "/stream/" + dialogue_id + "/";
    total_count += runtime_databus_->remove_by_prefix(stream_prefix);
  } else {
    Logger::instance().warn_f("[MMAgent] No runtime databus for dialogue cleanup: session=%s dialogue=%s",
                              session_id.c_str(), dialogue_id.c_str());
  }

  // 2. Clear data_bus_ entries for this dialogue's namespaces and stream
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& ns : DIALOGUE_NAMESPACES) {
      std::string prefix = "/session/" + session_id + "/" + ns + "/" + dialogue_id;
      auto it = data_bus_.begin();
      while(it != data_bus_.end()) {
        if(it->first.compare(0, prefix.size(), prefix) == 0) {
          it = data_bus_.erase(it);
          ++total_count;
        } else {
          ++it;
        }
      }
    }
    // Stream prefix
    std::string stream_prefix = "/session/" + session_id + "/stream/" + dialogue_id;
    auto it = data_bus_.begin();
    while(it != data_bus_.end()) {
      if(it->first.compare(0, stream_prefix.size(), stream_prefix) == 0) {
        it = data_bus_.erase(it);
        ++total_count;
      } else {
        ++it;
      }
    }
  }

  // 3. Emit DIALOGUE_DESTROYED event
  if(dialogue_event_callback_) {
    dialogue_event_callback_(config_.id, session_id, dialogue_id, "", DialogueEventType::DESTROYED);
    Logger::instance().info_f("[MMAgent] Emitted DIALOGUE_DESTROYED for session=%s dialogue=%s",
                              session_id.c_str(), dialogue_id.c_str());
  }

  Logger::instance().info_f("[MMAgent] clear_dialogue_data: session=%s dialogue=%s total_keys=%d",
                            session_id.c_str(), dialogue_id.c_str(), total_count);
  return total_count;
}

}  // namespace framework
}  // namespace agentflow
