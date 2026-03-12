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

std::map<std::string, std::string> build_task_workflow_map(
    const AgentPackage::SessionDefinition& session_definition) {
  std::map<std::string, std::string> task_workflows;
  for(const auto& task_definition : session_definition.tasks) {
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
    const AgentPackage::SessionDefinition& session_definition, const std::string& task_type) {
  for(const auto& task_definition : session_definition.tasks) {
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
  agent_obj.AddMember("data_bus_capacity", config.data_bus_capacity, allocator);
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
  if(agent_obj.HasMember("data_bus_capacity"))
    config.data_bus_capacity = agent_obj["data_bus_capacity"].GetInt();

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
}  // namespace

MMAgent::MMAgent(const Config& config) : config_(config), state_(AgentState::CREATED) {
  initialize_components();
}

std::shared_ptr<MMAgent> MMAgent::create_from_package(const std::string& package_path) {
  auto package = AgentPackage::load_from_directory(package_path);
  if(!package) {
    std::cerr << "Failed to load agent package from: " << package_path << std::endl;
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
        const auto& node_context_workflow = package->get_config().node_context_workflow;
        if(!node_context_workflow.empty()) {
          for(const auto& node : result.workflow->get_nodes()) {
            if(node && node->get_config().node_context_workflow.empty()) {
              node->bind_context_workflow(node_context_workflow);
            }
          }
        }
        loaded_workflows.push_back(result.workflow);
      } else {
        std::cerr << "Failed to load workflow from: " << wf_path.string() << std::endl;
        if(!result.errors.empty()) {
          std::cerr << "Parser error: " << result.errors[0].message << std::endl;
        }
      }
    } else {
      std::cerr << "Workflow file not found: " << wf_path.string() << std::endl;
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
  cleanup_resources();
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
    context_config.context_workflow = package_->get_config().agent_context_workflow;
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

  std::vector<std::shared_ptr<MMAgentSession>> session_pool;
  bool has_session_template = package_ && !package_->get_config().sessions.empty();
  Logger::instance().info_f("[AGENT_INIT] has_session_template=%s, sessions_count=%zu",
                            has_session_template ? "true" : "false",
                            package_ ? package_->get_config().sessions.size() : 0);
  if(has_session_template) {
    const auto& session_template = package_->get_config().sessions.front();
    const int session_pool_size =
        std::max(1, package_->get_config().session_pool_size);
    for(int session_index = 0; session_index < session_pool_size; ++session_index) {
      auto session_definition = session_template;
      session_definition.session_index = session_index;
      // Use empty session_id to trigger auto UUID generation
      auto session = create_session_from_definition("", session_definition);
      if(session) {
        session_pool.push_back(session);
      }
    }
  } else {
    // Create session with empty session_id to trigger auto UUID generation
    auto session = create_session_with_defaults("");  // Empty string triggers UUID generation
    if(session) {
      session_pool.push_back(session);
    }
  }
  session_router_->set_session_pool(session_pool);
  return true;
}

AgentPackage::SessionDefinition MMAgent::build_default_session_definition() const {
  AgentPackage::SessionDefinition session_definition;
  session_definition.session_index = 0;
  session_definition.description = "Default pooled session";
  session_definition.task_count = 2;

  AgentPackage::TaskDefinition dispatcher_task;
  dispatcher_task.task_index = 0;
  dispatcher_task.task_type = "dispatcher";
  dispatcher_task.description = "Session dispatcher";
  if(package_) {
    dispatcher_task.workflow_file = package_->get_config().dispatcher_workflow.empty()
                                        ? package_->get_config().coordinator_workflow
                                        : package_->get_config().dispatcher_workflow;
  }

  AgentPackage::TaskDefinition coordinator_task;
  coordinator_task.task_index = 1;
  coordinator_task.task_type = "coordinator";
  coordinator_task.description = "Session coordinator";
  coordinator_task.workflow_file =
      package_ ? package_->get_config().coordinator_workflow : std::string();

  session_definition.tasks = {dispatcher_task, coordinator_task};
  return session_definition;
}

std::shared_ptr<MMAgentSession> MMAgent::create_session_from_definition(
    const std::string& session_id, const AgentPackage::SessionDefinition& session_definition) {
  MMAgentSession::Config session_config;
  session_config.session_index = session_definition.session_index;
  session_config.session_id = session_id;
  session_config.description = session_definition.description;
  session_config.task_workflows = build_task_workflow_map(session_definition);
  if(package_) {
    session_config.task_context_workflow = package_->get_config().task_context_workflow;
  }
  if(const auto* dispatcher_definition = find_task_definition(session_definition, "dispatcher");
     dispatcher_definition != nullptr) {
    session_config.dispatcher_workflow =
        resolve_workflow_reference(dispatcher_definition->workflow_file);
    if(!session_config.dispatcher_workflow.empty()) {
      session_config.task_workflows["dispatcher"] = session_config.dispatcher_workflow;
    }
  } else if(package_) {
    session_config.dispatcher_workflow =
        resolve_workflow_reference(package_->get_config().dispatcher_workflow);
  }
  if(const auto* coordinator_definition = find_task_definition(session_definition, "coordinator");
     coordinator_definition != nullptr) {
    session_config.coordinator_workflow =
        resolve_workflow_reference(coordinator_definition->workflow_file);
    if(!session_config.coordinator_workflow.empty()) {
      session_config.task_workflows["coordinator"] = session_config.coordinator_workflow;
    }
  } else if(package_) {
    session_config.coordinator_workflow =
        resolve_workflow_reference(package_->get_config().coordinator_workflow);
  }

  auto session = std::make_shared<MMAgentSession>(session_config);
  std::shared_ptr<MMAgentTask> dispatcher_task;
  std::shared_ptr<MMAgentTask> coordinator_task;

  for(const auto& task_definition : session_definition.tasks) {
    MMAgentTask::Config task_config;
    task_config.task_index = task_definition.task_index;
    task_config.task_id = session_id + "_task_" + std::to_string(task_definition.task_index);
    task_config.task_type = task_definition.task_type;
    task_config.description = task_definition.description;
    task_config.session_id = session_id;
    task_config.task_context_workflow = session_config.task_context_workflow;
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
      task_config.skill_config_dir = task_definition.skill.skill_config_dir;
      task_config.instance_pool_size = task_definition.skill.instance_pool_size;
    }

    auto task = std::make_shared<MMAgentTask>(task_config);
    session->add_template_task(task);

    if(task_definition.task_type == "dispatcher") {
      dispatcher_task = task;
      session->set_dispatcher_task(task);

      // Create and store BuiltinDispatcher instance
      BuiltinDispatcher::Config dispatcher_config;
      dispatcher_config.dispatcher_id = config_.id + "_builtin_dispatcher_" + session_id;
      dispatcher_config.next_skills = task_definition.next_skills;
      dispatcher_config.max_concurrent_skills = task_definition.max_concurrent_skills;
      {
        std::lock_guard<std::mutex> lock(builtin_dispatchers_mutex_);
        builtin_dispatchers_[session_id] = std::make_shared<BuiltinDispatcher>(dispatcher_config);
      }
      Logger::instance().info_f("[CREATE_SESSION] Created BuiltinDispatcher for session=%s, next_skills count=%d",
                               session_id.c_str(), static_cast<int>(dispatcher_config.next_skills.size()));
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
    dispatcher_config.coordinator_id = config_.id + "_dispatcher_" + session_id;
    dispatcher_config.default_task_type = "dispatcher";
    dispatcher_config.input_queue_type = SessionTaskCoordinator::InputQueueType::INTENT_REQUEST;
    dispatcher_config.dispatcher_workflow = session_config.dispatcher_workflow;
    dispatcher_config.coordinator_workflow = session_config.coordinator_workflow;
    dispatcher_config.task_workflows = session_config.task_workflows;
    dispatcher_config.task_context_workflow = session_config.task_context_workflow;
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
    coordinator_config.coordinator_id = config_.id + "_coordinator_" + session_id;
    coordinator_config.default_task_type = "coordinator";
    coordinator_config.input_queue_type = SessionTaskCoordinator::InputQueueType::DATA_EXCHANGE;
    coordinator_config.dispatcher_workflow = session_config.dispatcher_workflow;
    coordinator_config.coordinator_workflow = session_config.coordinator_workflow;
    coordinator_config.task_workflows = session_config.task_workflows;
    coordinator_config.task_context_workflow = session_config.task_context_workflow;
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
    sessions_[session_id] = session;
  }
  return session;
}

std::shared_ptr<MMAgentSession> MMAgent::create_session_with_defaults(const std::string& session_id) {
  return create_session_from_definition(session_id, build_default_session_definition());
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

int MMAgent::get_response_queue_size() const {
  return session_router_ ? session_router_->get_response_queue_size() : 0;
}

void MMAgent::handle_routed_request_message(const std::shared_ptr<MMAgentSession>& session,
                                            const MMAgentMsgQue::Message& message) {
  if(!session) {
    return;
  }

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
  Logger::instance().info_f("[BUILD_RUNTIME_TASK] message.content='%s', message.data_ref='%s'",
                            message.content.c_str(), message.data_ref.c_str());
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
  if(const auto selected_skills_it = message.metadata.find("selected_skills");
     selected_skills_it != message.metadata.end()) {
    task->set_parameter("selected_skills", selected_skills_it->second);
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

      // Launch skills based on pre-configured next_skills
      for(const auto& skill_name : configured_next_skills) {
        if(!task->can_launch_more_skills()) {
          Logger::instance().info_f("[HANDLE_INTENT_REQUEST] Reached max concurrent skills limit");
          break;
        }
        if(launch_skill_task(session, skill_name, message, task)) {
          Logger::instance().info_f("[HANDLE_INTENT_REQUEST] Launched skill: %s", skill_name.c_str());
        }
      }
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
  } else {
    Logger::instance().warn_f("[HANDLE_INTENT_REQUEST] build_runtime_task returned null for task=%s", task->get_task_id().c_str());
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

void MMAgent::handle_task_data_exchange(const std::shared_ptr<MMAgentSession>& session,
                                        const std::shared_ptr<MMAgentTask>& task,
                                        const MMAgentMsgQue::Message& message) {
  if(!session || !task) {
    return;
  }
  Logger::instance().info_f("[HANDLE_DATA_EXCHANGE] task_id=%s, task_type=%s, message_id=%s",
                            task->get_task_id().c_str(), task->get_task_type().c_str(),
                            message.message_id.c_str());
  task->acknowledge_dispatch_record(message.message_id);

  // ============================================================
  // UNIFIED next_skills HANDLING (applies to all task types)
  // ============================================================
  // Check for next_skills in databus - this can be output by any task
  DataVariant next_skills_value;
  if(get_data("next_skills", next_skills_value)) {
    auto next_skills = BuiltinDispatcher::parse_next_skills(next_skills_value);
    if(!next_skills.empty()) {
      // Clear the processed next_skills to prevent re-processing
      publish_data("next_skills", std::string(""));

      Logger::instance().info_f("[DATA_EXCHANGE] Found next_skills, launching %d skills",
                               static_cast<int>(next_skills.size()));

      // Get source task for skill launching (could be dispatcher, coordinator, or skill)
      std::string source_task_id;
      if(message.metadata.count("source_task_id")) {
        source_task_id = message.metadata.at("source_task_id");
      }
      auto source_task = source_task_id.empty() ? task : session->get_task(source_task_id);
      if(!source_task) {
        source_task = task;
      }

      for(const auto& skill_name : next_skills) {
        if(!source_task->can_launch_more_skills()) {
          Logger::instance().info_f("[DATA_EXCHANGE] Reached max concurrent skills limit");
          break;
        }
        launch_skill_task(session, skill_name, message, source_task);
      }
    }
  }

  // ============================================================
  // DISPATCHER HANDLING
  // ============================================================
  if(task->get_task_type() == "dispatcher") {
    // Check if this is a skill completion message
    // Skill completions have source_task_id pointing to this dispatcher
    // but workflow_id from the skill workflow
    bool is_skill_completion = false;
    std::string skill_name;
    std::string source_workflow_id;

    if(message.metadata.count("workflow_id")) {
      source_workflow_id = message.metadata.at("workflow_id");
    }
    if(message.metadata.count("skill_name")) {
      skill_name = message.metadata.at("skill_name");
      is_skill_completion = true;
    }

    // Also check if source_task_id indicates this came from a skill task
    if(message.metadata.count("source_task_id")) {
      const auto& source_id = message.metadata.at("source_task_id");
      // If source_task_id contains "skill" marker, it's a skill completion
      if(source_id.find("_skill-") != std::string::npos ||
         source_id.find("skill-") != std::string::npos) {
        is_skill_completion = true;
      }
    }

    if(is_skill_completion) {
      // Skill completed - forward response to coordinator and decrement skill count
      Logger::instance().info_f("[DISPATCHERDataExchange] Skill completion detected, skill=%s",
                               skill_name.empty() ? "unknown" : skill_name.c_str());

      // Decrement skill count
      if(!skill_name.empty()) {
        task->on_skill_completed(skill_name);
      } else {
        // Fallback: try to extract skill name from workflow_id
        task->on_skill_completed("echo-skill");  // default for echo agent
      }

      // Forward response to coordinator
      if(auto coordinator_task = session->get_coordinator_task(); coordinator_task) {
        MMAgentMsgQue::Message response_message = message;
        response_message.metadata["task_type"] = "coordinator";
        response_message.metadata["source_task_type"] = "dispatcher";
        if(!skill_name.empty()) {
          response_message.metadata["skill_name"] = skill_name;
        }

        // Get response from databus
        DataVariant response_value;
        if(get_data("response", response_value)) {
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
        coordinator_task->enqueue_data_exchange_message(trigger_message);

        Logger::instance().info_f("[DISPATCHERDataExchange] Forwarded skill response to coordinator");
      } else {
        Logger::instance().warn_f("[DISPATCHERDataExchange] No coordinator task found");
      }
      return;
    }

    if(message.content != "workflow_completed") {
      Logger::instance().info_f(
          "[DISPATCHERDataExchange] Ignoring non-complete event, content='%s'",
          message.content.c_str());
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
        launch_skill_task(session, skill_name, message, task);
      }
      return;
    }

    // No pre-configured next_skills - check for response or forward to coordinator
    DataVariant dispatcher_response_value;
    if(get_data("response", dispatcher_response_value)) {
      if(const auto* text = std::get_if<std::string>(&dispatcher_response_value); text && !text->empty()) {
        MMAgentMsgQue::Message response_message = message;
        response_message.content = *text;
        response_message.metadata["task_type"] = "dispatcher";
        response_message.metadata["message_type"] = "dispatcher_response";
        if(session_router_) {
          Logger::instance().info_f(
              "[DISPATCHERDataExchange] Enqueueing direct dispatcher response, content='%s'",
              response_message.content.c_str());
          session_router_->enqueue_response_message(response_message);
        }
        return;
      }
    }

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

      // Mark skill as completed
      if(skill_response.metadata.count("skill_name")) {
        task->on_skill_completed(skill_response.metadata["skill_name"]);
      }

      MMAgentMsgQue::Message response_message = skill_response;

      // Get response from databus (published by skill workflow)
      DataVariant response_value;
      if(get_data("response", response_value)) {
        if(const auto* text = std::get_if<std::string>(&response_value)) {
          response_message.content = *text;
          Logger::instance().info_f("[COORDINATORDataExchange] Got skill response from databus: %s",
                                    text->c_str());
        }
      }

      if(session_router_) {
        Logger::instance().info_f("[COORDINATORDataExchange] Enqueueing final response to client, content='%s'",
                                  response_message.content.c_str());
        session_router_->enqueue_response_message(response_message);
      }
      return;
    }

    // Coordinator workflow completed - check if need to start skill tasks
    DataVariant selected_skills_value;
    std::string selected_skills;
    if(get_data("selected_skills", selected_skills_value)) {
      if(const auto* text = std::get_if<std::string>(&selected_skills_value)) {
        selected_skills = *text;
      }
    }

    if(!selected_skills.empty()) {
      Logger::instance().info_f("[COORDINATORDataExchange] Coordinator workflow completed, starting skill tasks: %s",
                                selected_skills.c_str());

      // Clear selected_skills to prevent re-creating skill tasks
      publish_data("selected_skills", std::string(""));

      // Parse selected_skills using BuiltinDispatcher's parser
      auto skill_names = BuiltinDispatcher::parse_next_skills(selected_skills_value);

      for(const auto& skill_name : skill_names) {
        if(!task->can_launch_more_skills()) {
          Logger::instance().info_f("[COORDINATORDataExchange] Reached max concurrent skills limit");
          break;
        }
        launch_skill_task(session, skill_name, message, task);
      }
      return;  // Don't send response to client yet, wait for skill
    }

    // No selected_skills - coordinator is done, send response to client
    Logger::instance().info_f("[COORDINATORDataExchange] Coordinator completed without skill, sending response");

    MMAgentMsgQue::Message response_message = message;
    DataVariant response_value;
    if(get_data("response", response_value)) {
      if(const auto* text = std::get_if<std::string>(&response_value)) {
        response_message.content = *text;
      }
    }

    if(session_router_) {
      session_router_->enqueue_response_message(response_message);
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
        coordinator_response.content = "skill_completed";

        coordinator_task->enqueue_response_message(coordinator_response);

        // Trigger coordinator's data exchange to process the response
        MMAgentMsgQue::Message trigger_message;
        trigger_message.message_id = "trigger-" + task->get_task_id();
        trigger_message.metadata["task_type"] = "coordinator";
        trigger_message.metadata["trigger_type"] = "skill_response";
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

  // Create skill runtime task message
  MMAgentMsgQue::Message skill_message;
  skill_message.message_id = "skill-" + skill_name + "-" + context.message_id;
  skill_message.metadata["task_type"] = "skill";
  skill_message.metadata["skill_name"] = skill_name;
  skill_message.metadata["session_id"] = session->get_session_id();
  skill_message.metadata["source_task_id"] = source_task->get_task_id();
  skill_message.content = context.content;
  skill_message.data_ref = context.data_ref;

  // Copy relevant metadata from context
  for(const auto& [key, value] : context.metadata) {
    if(key != "task_type" && key != "skill_name") {
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

  // Set coordinator_task_id if source is coordinator
  if(source_task->get_task_type() == "coordinator") {
    skill_task->set_parameter("coordinator_task_id", source_task->get_task_id());
  }

  session->add_task(skill_task);

  // Mark skill as started on source task
  source_task->on_skill_started(skill_name);

  // Add to generated tasks queue for runtime scheduling
  {
    std::lock_guard<std::mutex> lock(generated_tasks_mutex_);
    generated_tasks_.push(skill_task);
  }

  Logger::instance().info_f("[LAUNCH_SKILL] Created skill task: %s for skill: %s",
                           skill_task->get_task_id().c_str(), skill_name.c_str());

  return true;
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
  data_bus_.clear();
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
  data_bus_[key] = value;
  return true;
}

bool MMAgent::get_data(const std::string& key, DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);
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
  while(message_threads_running_) {
    int processed = process_data_exchange_messages();
    if(processed == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

}  // namespace framework
}  // namespace agentflow
