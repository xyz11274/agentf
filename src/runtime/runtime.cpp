#include "runtime/agent_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <random>
#include <sstream>
#include <thread>

#include "agent/session.h"
#include "agent/session_router.h"
#include "agent/task.h"
#include "agent/context.h"
#include "runtime/async_call.h"
#include "runtime/trace/trace_context.h"
#include "runtime/trace/trace_manager.h"
#include "runtime/flow_trace.h"
#include "runtime/databus/node_databus_adapter.h"
#include "runtime/databus/node_databus_client.h"
#include "utils/logger.h"
#include "workflow/dsl/json_adapter.h"

namespace agentflow {
namespace runtime {

namespace {
std::condition_variable g_worker_cv;
std::mutex              g_worker_mutex;
bool                    g_shutdown = false;
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string make_workflow_uds_socket_path(const std::string& session_id,
                                          const std::string& execution_id) {
  // sockaddr_un::sun_path is only 104 bytes on macOS. Session UUIDs plus
  // agent/workflow/execution names exceeded that limit and silently disabled
  // node DataBus access. A hash keeps the path short while retaining
  // per-execution uniqueness within the process.
  const auto hash = std::hash<std::string>{}(session_id + "\n" + execution_id);
  std::ostringstream path;
  path << "/tmp/afdb_" << std::hex << hash << ".sock";
  return path.str();
}

// =============================================================================
// Edge Data Transfer Helper Functions (Scenario 1 & Scenario 2 Fix)
// =============================================================================

// Check if a node has any downstream edge
static bool has_downstream_edge(const std::shared_ptr<framework::MMWorkflow>& workflow,
                                const std::shared_ptr<framework::MMNode>& node) {
  if(!workflow || !node) return false;
  for(const auto& edge : workflow->get_edges()) {
    if(edge && edge->get_source() == node->get_id()) {
      return true;
    }
  }
  return false;
}

// Get all outgoing edges from a node
static std::vector<std::shared_ptr<framework::MMEdge>> get_outgoing_edges(
    const std::shared_ptr<framework::MMWorkflow>& workflow,
    const std::shared_ptr<framework::MMNode>& node) {
  std::vector<std::shared_ptr<framework::MMEdge>> result;
  if(!workflow || !node) return result;
  for(const auto& edge : workflow->get_edges()) {
    if(edge && edge->get_source() == node->get_id()) {
      result.push_back(edge);
    }
  }
  return result;
}

// Count incoming edges to a target node (Scenario 2 helper)
static int count_incoming_edges(const std::shared_ptr<framework::MMWorkflow>& workflow,
                                const std::string& target_node_id) {
  if(!workflow) return 0;
  int count = 0;
  for(const auto& edge : workflow->get_edges()) {
    if(edge && edge->get_target() == target_node_id) {
      ++count;
    }
  }
  return count;
}

// Get all incoming edges to a target node (Scenario 2 helper)
static std::vector<std::shared_ptr<framework::MMEdge>> get_incoming_edges(
    const std::shared_ptr<framework::MMWorkflow>& workflow,
    const std::string& target_node_id) {
  std::vector<std::shared_ptr<framework::MMEdge>> result;
  if(!workflow) return result;
  for(const auto& edge : workflow->get_edges()) {
    if(edge && edge->get_target() == target_node_id) {
      result.push_back(edge);
    }
  }
  return result;
}

// Build session-isolated DataBus key for framework data (no downstream scenario)
// Note: workflow_id is NOT included in the key to support cross-workflow communication
// (e.g., skill-selection-workflow outputs next_skills, coordinator reads it)
static std::string build_framework_key(const std::string& session_id,
                                       const std::string& dialogue_id,
                                       const std::string& workflow_id,
                                       const std::string& key) {
  (void)workflow_id;  // Explicitly unused - workflow_id excluded for cross-workflow access
  std::string bus_key = "/session/" + session_id + "/sys/";
  if(!dialogue_id.empty()) {
    bus_key += dialogue_id + "/";
  }
  bus_key += key;
  return bus_key;
}

std::vector<std::shared_ptr<framework::MMAgentTask>> collect_pool_template_tasks(const AgentPtr& agent) {
  std::vector<std::shared_ptr<framework::MMAgentTask>> tasks;
  if(!agent) {
    return tasks;
  }
  std::set<std::string> seen_task_ids;
  if(auto context = agent->get_context(); context) {
    if(auto updater = context->get_context_updater_task(); updater) {
      tasks.push_back(updater);
      seen_task_ids.insert(updater->get_task_id());
    }
  }
  for(const auto& session : agent->get_sessions()) {
    if(!session) {
      continue;
    }
    for(const auto& task : session->get_tasks()) {
      if(!task || task->is_skill_instance() || seen_task_ids.count(task->get_task_id()) > 0) {
        continue;
      }
      if(task->get_task_id().find("_runtime_") != std::string::npos) {
        continue;
      }
      tasks.push_back(task);
      seen_task_ids.insert(task->get_task_id());
    }
  }
  return tasks;
}
}  // namespace

MMAgentRuntime::MMAgentRuntime() {
  config_.worker_threads = num_worker_threads_;
  role_ = config_.role;
  shared_state_ = std::make_shared<InMemoryRuntimeSharedState>();
  deployer_ = std::unique_ptr<master::Deployer>(new master::Deployer());
  scheduler_ = std::unique_ptr<master::Scheduler>(new master::Scheduler());
  scheduler_->set_workflow_snapshot_provider(
      [this]() { return shared_state_ ? shared_state_->list_workflow_instances() : std::vector<WorkflowInstanceInfo>{}; });
  local_worker_ = std::make_shared<MMWorkerEngine>(
      MMWorkerEngine::Config{config_.local_worker_id, true, config_.worker_default_workflow_threads,
                           config_.worker_control_threads, config_.worker_business_threads,
                           config_.worker_queue_size, config_.plugin_uds_path});
  local_worker_->set_status(MMWorkerEngine::WorkerStatus::RUNNING);
}

MMAgentRuntime::MMAgentRuntime(const Config& config) : config_(config), role_(config.role) {
  num_worker_threads_ = config.worker_threads > 0 ? config.worker_threads : num_worker_threads_;
  shared_state_ = std::make_shared<InMemoryRuntimeSharedState>();
  deployer_ = std::unique_ptr<master::Deployer>(new master::Deployer());
  scheduler_ = std::unique_ptr<master::Scheduler>(new master::Scheduler());
  scheduler_->set_workflow_snapshot_provider(
      [this]() { return shared_state_ ? shared_state_->list_workflow_instances() : std::vector<WorkflowInstanceInfo>{}; });
  local_worker_ =
      std::make_shared<MMWorkerEngine>(MMWorkerEngine::Config{config.local_worker_id, true,
                                                            config.worker_default_workflow_threads,
                                                            config.worker_control_threads,
                                                            config.worker_business_threads,
                                                            config.worker_queue_size,
                                                            config.plugin_uds_path});
  local_worker_->set_status(MMWorkerEngine::WorkerStatus::RUNNING);
}

MMAgentRuntime::~MMAgentRuntime() {
  stop();
}

void MMAgentRuntime::set_role(RuntimeRole role) {
  role_ = role;
}

bool MMAgentRuntime::register_worker_engine(const std::shared_ptr<MMWorkerEngine>& worker) {
  if(!deployer_) {
    return false;
  }
  return deployer_->register_worker(worker);
}

bool MMAgentRuntime::unregister_worker_engine(const std::string& worker_id) {
  if(!deployer_) {
    return false;
  }
  return deployer_->unregister_worker(worker_id);
}

bool MMAgentRuntime::heartbeat_worker(const std::string& worker_id) {
  if(!deployer_) {
    return false;
  }
  return deployer_->update_worker_heartbeat(worker_id);
}

bool MMAgentRuntime::update_worker_engine_status(const std::string& worker_id,
                                               MMWorkerEngine::WorkerStatus status) {
  if(!deployer_) {
    return false;
  }
  return deployer_->update_worker_status(worker_id, status);
}

std::vector<master::WorkerSnapshot> MMAgentRuntime::list_worker_engines() const {
  if(!deployer_) {
    return {};
  }
  return deployer_->list_worker_snapshots();
}

bool MMAgentRuntime::deploy_workflow_instance(const WorkflowInstanceInfo& info,
                                             const std::string& preferred_worker_id) {
  if(!deployer_ || !shared_state_) {
    return false;
  }
  if(!deployer_->deploy_workflow_instance(info.workflow_instance_id, info.workflow_id, preferred_worker_id)) {
    return false;
  }
  std::string assigned_worker;
  if(!deployer_->get_workflow_worker(info.workflow_instance_id, assigned_worker)) {
    return false;
  }
  WorkflowInstanceInfo persisted = info;
  persisted.worker_id = assigned_worker;
  shared_state_->add_or_update_workflow_instance(persisted);
  if(auto worker = deployer_->get_worker_engine(assigned_worker); worker) {
    worker->add_supported_workflow(info.workflow_id);
    worker->register_workflow_instance(
        MMWorkerEngine::WorkflowRegistration{info.workflow_id, info.workflow_instance_id,
                                             config_.worker_default_workflow_threads});
  }
  return true;
}

bool MMAgentRuntime::destroy_workflow_instance(const std::string& workflow_instance_id) {
  if(!deployer_ || !shared_state_) {
    return false;
  }
  WorkflowInstanceInfo existing;
  const bool has_existing = shared_state_->get_workflow_instance(workflow_instance_id, existing);
  bool removed_mapping = deployer_->remove_workflow_instance(workflow_instance_id);
  bool removed_state = shared_state_->remove_workflow_instance(workflow_instance_id);
  if(has_existing) {
    if(auto worker = deployer_->get_worker_engine(existing.worker_id); worker) {
      worker->unregister_workflow_instance(workflow_instance_id);
    }
  }
  return removed_mapping || removed_state;
}

master::TaskScheduleResult MMAgentRuntime::schedule_task(const master::TaskScheduleRequest& request) {
  if(!scheduler_ || !shared_state_) {
    return {};
  }
  // Only use synchronous scheduling - the caller needs the result immediately
  auto result = scheduler_->schedule_task(request, shared_state_->list_workflow_instances());
  if(result.scheduled) {
    shared_state_->increment_workflow_ref(result.workflow_instance_id);
    TaskInstanceInfo task;
    task.task_id = request.task_id;
    task.agent_object_id = request.agent_object_id;
    task.agent_instance_id = request.agent_instance_id;
    task.workflow_id = request.workflow_id;
    task.workflow_file = request.workflow_file;
    task.workflow_instance_id = result.workflow_instance_id;
    task.worker_id = result.worker_id;
    task.status = "scheduled";
    shared_state_->add_or_update_task_instance(task);
  }
  return result;
}

bool MMAgentRuntime::report_agent_heartbeat(const AgentId& agent_id) {
  std::lock_guard<std::mutex> lock(agents_mutex_);
  if(agents_.find(agent_id) == agents_.end()) {
    return false;
  }
  agent_heartbeats_[agent_id] = now_ms();
  return true;
}

bool MMAgentRuntime::update_task_status(const std::string& task_id, const std::string& status,
                                       int progress) {
  if(!shared_state_ || !scheduler_) {
    return false;
  }
  TaskInstanceInfo info;
  if(!shared_state_->get_task_instance(task_id, info)) {
    return false;
  }
  info.status = status;
  info.progress = progress;
  shared_state_->add_or_update_task_instance(info);
  return scheduler_->update_task_status(task_id, status, progress);
}

std::vector<WorkflowInstanceInfo> MMAgentRuntime::list_workflow_instances() const {
  return shared_state_ ? shared_state_->list_workflow_instances() : std::vector<WorkflowInstanceInfo>{};
}

std::map<std::string, master::TaskScheduleResult> MMAgentRuntime::list_task_assignments() const {
  return scheduler_ ? scheduler_->list_task_assignments()
                    : std::map<std::string, master::TaskScheduleResult>{};
}

bool MMAgentRuntime::register_agent(AgentPtr agent) {
  if(!agent) {
    return false;
  }
  if(!is_running_) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agents_[agent->get_id()] = agent;
    size_t databus_capacity = static_cast<size_t>(agent->get_config().data_bus_capacity_mb) * 1024 * 1024;
    databuses_[agent->get_id()] = std::unique_ptr<MMDataBus>(new MMDataBus(databus_capacity));
    Logger::instance().info_f("DataBus initialized for agent_id=%s, capacity=%zu bytes (%d MB)",
                              agent->get_id().c_str(), databus_capacity, agent->get_config().data_bus_capacity_mb);
    agent_heartbeats_[agent->get_id()] = now_ms();

    // Set the agent's runtime_databus pointer for unified data storage
    agent->set_runtime_databus(databuses_[agent->get_id()].get());

    if(local_worker_) {
      for(const auto& workflow : agent->get_workflows()) {
        if(workflow) {
          local_worker_->add_supported_workflow(workflow->get_id());
        }
      }
      // Inject this agent's DataBus to the local worker
      local_worker_->set_databus(agent->get_id(), databuses_[agent->get_id()].get());
    }
    Logger::instance().info_f("MMAgent registered: %s (id: %s)", agent->get_name().c_str(),
                              agent->get_id().c_str());

    // Emit AGENT_REGISTERED event
    auto workflows_info = build_workflow_infos(agent);
    AgentEvent event = EventBuilder::build_agent_registered_event(
        agent->get_id(),
        agent->get_name(),
        agent->get_config().version,
        agent->get_config().description,
        workflows_info);
    emit_agent_event(event);

    // Set up session event callback to emit SESSION_CREATED/SESSION_DESTROYED events
    agent->set_session_event_callback(
        [this, agent_id = agent->get_id()](
            const std::string& session_id,
            const std::string& user_id,
            const std::string& client_id,
            framework::SessionEventType event_type) {
          if(event_type == framework::SessionEventType::CREATED) {
            SessionEvent event = EventBuilder::build_session_created_event(
                agent_id, user_id, client_id, session_id);
            emit_session_event(event);
          } else if(event_type == framework::SessionEventType::DESTROYED) {
            SessionEvent event = EventBuilder::build_session_destroyed_event(
                agent_id, session_id);
            emit_session_event(event);
          }
        });

    // Set up dialogue event callback to emit DIALOGUE_CREATED/DESTROYED events
    agent->set_dialogue_event_callback(
        [this](
            const std::string& agent_id,
            const std::string& session_id,
            const std::string& dialogue_id,
            const std::string& user_input,
            framework::DialogueEventType event_type) {
          if(event_type == framework::DialogueEventType::CREATED) {
            auto agent = get_agent(agent_id);
            std::string user_id = agent && agent->get_session(session_id)
                                        ? agent->get_session(session_id)->get_user_id() : "";
            std::string client_id = agent && agent->get_session(session_id)
                                        ? agent->get_session(session_id)->get_client_id() : "";
            DialogueEvent event = EventBuilder::build_dialogue_created_event(
                agent_id, user_id, client_id, session_id, dialogue_id, user_input);
            emit_dialogue_event(event);
          } else if(event_type == framework::DialogueEventType::DESTROYED) {
            DialogueEvent event = EventBuilder::build_dialogue_destroyed_event(
                agent_id, session_id, dialogue_id);
            emit_dialogue_event(event);
          }
        });

    // Set up task event callback to emit TASK_CREATED events
    agent->set_task_event_callback(
        [this](
            const std::string& agent_id,
            const std::string& session_id,
            const std::string& dialogue_id,
            const std::string& task_id,
            const std::string& workflow_id,
            const std::string& task_type,
            const std::string& skill_name,
            framework::TaskEventType event_type) {
          if(event_type == framework::TaskEventType::CREATED) {
            auto agent = get_agent(agent_id);
            std::string user_id = agent && agent->get_session(session_id)
                                        ? agent->get_session(session_id)->get_user_id() : "";
            std::string client_id = agent && agent->get_session(session_id)
                                        ? agent->get_session(session_id)->get_client_id() : "";
            TaskEvent event = EventBuilder::build_task_created_event(
                agent_id, user_id, client_id, session_id, dialogue_id,
                task_id, workflow_id, task_type, skill_name);
            emit_task_event(event);
          }
        });

    return true;
  }

  bool result = false;
  bool done = false;
  std::mutex done_mutex;
  std::condition_variable done_cv;
  {
    std::lock_guard<std::mutex> queue_lock(agent_registration_mutex_);
    agent_registration_queue_.push({agent, false, &done_cv, &done_mutex, &done, &result});
  }
  agent_registration_cv_.notify_all();
  std::unique_lock<std::mutex> done_lock(done_mutex);
  done_cv.wait(done_lock, [&done]() { return done; });
  return result;
}

bool MMAgentRuntime::unregister_agent(const AgentId& agent_id) {
  auto agent = get_agent(agent_id);
  if(!agent) {
    return false;
  }
  if(!is_running_) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto                        it = agents_.find(agent_id);
    if(it != agents_.end()) {
      // Cascade: clear all session data before destroying the DataBus
      auto session_ids = it->second->get_all_session_ids();
      Logger::instance().info_f("[Runtime] unregister_agent %s: cleaning %zu sessions before DataBus teardown",
                                agent_id.c_str(), session_ids.size());
      for(const auto& sid : session_ids) {
        int cleared = it->second->clear_session_data(sid);
        Logger::instance().debug_f("[Runtime] unregister_agent %s: cleared session=%s keys=%d",
                                   agent_id.c_str(), sid.c_str(), cleared);
      }

      // P0-4 修复方案 A：析构顺序必须为 worker 解引用 -> stop agent -> erase agent -> erase databus
      // 否则 agent 析构时 cleanup_resources 访问已释放 databus -> SIGSEGV
      // agent 先析构（cleanup_resources 访问 databus），再释放 databus
      if(local_worker_) {
        local_worker_->remove_databus(agent_id);
      }
      it->second->stop();
      agents_.erase(it);
      databuses_.erase(agent_id);
      agent_heartbeats_.erase(agent_id);
      Logger::instance().info_f("MMAgent unregistered: %s", agent_id.c_str());

      // Emit AGENT_UNREGISTERED event
      AgentEvent event = EventBuilder::build_agent_unregistered_event(agent_id);
      emit_agent_event(event);

      return true;
    }
    return false;
  }

  bool result = false;
  bool done = false;
  std::mutex done_mutex;
  std::condition_variable done_cv;
  {
    std::lock_guard<std::mutex> queue_lock(agent_registration_mutex_);
    agent_registration_queue_.push({agent, true, &done_cv, &done_mutex, &done, &result});
  }
  agent_registration_cv_.notify_all();
  std::unique_lock<std::mutex> done_lock(done_mutex);
  done_cv.wait(done_lock, [&done]() { return done; });
  return result;
}

AgentPtr MMAgentRuntime::get_agent(const AgentId& agent_id) {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  auto it = agents_.find(agent_id);
  return it != agents_.end() ? it->second : nullptr;
}

std::vector<AgentPtr> MMAgentRuntime::list_agents() {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  std::vector<AgentPtr> result;
  for(const auto& pair : agents_) {
    result.push_back(pair.second);
  }
  return result;
}

bool MMAgentRuntime::load_workflow(const AgentId& agent_id, WorkflowPtr workflow) {
  auto agent = get_agent(agent_id);
  if(!agent || !workflow) {
    return false;
  }

  if(local_worker_) {
    local_worker_->add_supported_workflow(workflow->get_id());
  }
  agent->add_workflow(workflow);
  if(!workflow->validate()) {
    agent->remove_workflow(workflow->get_id());
    Logger::instance().error_f("MMWorkflow validation failed: %s", workflow->get_name().c_str());
    return false;
  }

  Logger::instance().info_f("MMWorkflow loaded into agent %s: %s (id: %s, %zu nodes)",
                            agent_id.c_str(), workflow->get_name().c_str(),
                            workflow->get_id().c_str(), workflow->get_nodes().size());
  return true;
}

bool MMAgentRuntime::unload_workflow(const AgentId& agent_id, const WorkflowId& workflow_id) {
  auto agent = get_agent(agent_id);
  if(!agent) {
    return false;
  }

  agent->remove_workflow(workflow_id);
  Logger::instance().info_f("MMWorkflow unloaded from agent %s: %s", agent_id.c_str(),
                            workflow_id.c_str());
  return true;
}

bool MMAgentRuntime::execute_workflow(const AgentId& agent_id, const WorkflowId& workflow_id,
                                    const DataMap& inputs, const std::string& source_task_id,
                                    const std::string& session_id, const std::string& dialogue_id,
                                    const std::string& trace_labels, const std::string& trace_id,
                                    const std::string& task_type, const std::string& skill_task_id,
                                    const std::string& skill_name) {
  if(draining_) {
    Logger::instance().warn("Rejecting new workflow — runtime is draining");
    return false;
  }

  auto agent    = get_agent(agent_id);
  auto workflow_template = agent ? agent->get_workflow(workflow_id) : nullptr;

  if(!agent || !workflow_template) {
    Logger::instance().error_f("Cannot execute workflow: agent=%s, workflow=%s", agent_id.c_str(),
                               workflow_id.c_str());
    return false;
  }

  // Clone the workflow for this execution to avoid state conflicts with concurrent executions
  auto workflow = workflow_template->clone();
  if(!workflow) {
    Logger::instance().error_f("Failed to clone workflow: %s", workflow_id.c_str());
    return false;
  }

  // Create execution state for this workflow
  std::string execution_id =
      agent_id + ":" + workflow_id + ":"
      + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

  // Store the cloned workflow indexed by execution_id
  {
    std::lock_guard<std::mutex> lock(cloned_workflows_mutex_);
    cloned_workflows_[execution_id] = workflow;
  }

  WorkflowExecutionState state;
  state.workflow_id     = workflow_id;
  state.agent_id        = agent_id;
  state.source_task_id  = source_task_id;
  state.session_id      = session_id;
  state.dialogue_id     = dialogue_id;  // Store for multi-dialogue support
  state.trace_labels    = trace_labels; // Store for flow trace correlation
  state.trace_id        = trace_id;     // Store for end-to-end tracing
  state.task_type       = task_type;    // Store for correct task_type in completion message
  state.skill_task_id   = skill_task_id;  // Store actual skill task id for completion tracking
  state.skill_name      = skill_name;      // Store skill name for completion notification
  state.inputs          = inputs;          // Store inputs in state for node hydration (concurrency safe)
  state.total_nodes     = static_cast<int>(workflow->get_nodes().size());
  state.completed_count = 0;
  state.in_progress     = true;

  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    // Update existing entry or create new one
    auto it = workflow_states_.find(execution_id);
    if (it != workflow_states_.end()) {
      // Update existing entry field by field
      it->second.workflow_id = std::move(state.workflow_id);
      it->second.agent_id = std::move(state.agent_id);
      it->second.source_task_id = std::move(state.source_task_id);
      it->second.session_id = std::move(state.session_id);
      it->second.dialogue_id = std::move(state.dialogue_id);
      it->second.trace_labels = std::move(state.trace_labels);
      it->second.trace_id     = std::move(state.trace_id);
      it->second.task_type = std::move(state.task_type);
      it->second.skill_task_id = std::move(state.skill_task_id);
      it->second.skill_name = std::move(state.skill_name);
      it->second.inputs = std::move(state.inputs);
      it->second.total_nodes = state.total_nodes;
      it->second.completed_count = state.completed_count;
      it->second.in_progress = state.in_progress;
    } else {
      // Create new entry - need to construct in place
      workflow_states_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(execution_id),
          std::forward_as_tuple(
              WorkflowExecutionState{
                  std::move(state.workflow_id),
                  std::move(state.agent_id),
                  std::move(state.source_task_id),
                  std::move(state.session_id),
                  std::move(state.dialogue_id),
                  std::move(state.trace_labels),
                  std::move(state.trace_id),
                  std::move(state.task_type),
                  std::move(state.skill_task_id),
                  std::move(state.skill_name),
                  std::move(state.inputs),
                  {},
                  {},
                  {},
                  {},
                  state.total_nodes,
                  state.completed_count,
                  state.in_progress,
                  {}
              }
          )
      );
    }
  }

  // Create UDS server for this workflow execution (lifecycle tied to execution)
  // This enables Python/Go/Shell SDKs to access DataBus via Unix Domain Socket
  if(!session_id.empty()) {
    std::string uds_socket_path = make_workflow_uds_socket_path(session_id, execution_id);

    // Create async callback for multi-language nodes
    UDSServer::AsyncCallback async_cb;

    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    auto state_it = workflow_states_.find(execution_id);
    if(state_it != workflow_states_.end()) {
      MMDataBus& databus = get_databus(agent_id);

      // Create UDS server with async callback + response_push support.
      // response_push bypasses the coordinator round-trip: streaming chunks
      // are enqueued directly to session_router's response queue, so the
      // ResponsePoller picks them up in near real-time (≤50ms).
      // Design ref: streaming-response.md §3.1 (core idea) + §3.7 S-2
      //   (response_push does NOT enter DataBus, does NOT trigger coordinator).
      UDSServer::ResponsePushCallback push_cb =
          [this, agent_id](const std::string& sid,
                            const std::string& did,
                            const std::string& content,
                            const std::string& type,
                            const std::string& source,
                            const std::string& skill,
                            const std::string& round_str,
                            const std::string& status) -> bool {
        // Empty-content policy: skip llm_chunk with empty content (no token)
        if(type == "llm_chunk" && content.empty()) return true;

        auto agent = get_agent(agent_id);
        if(!agent) {
          Logger::instance().warn_f("[RESPONSE_PUSH] Agent not found: %s",
                                    agent_id.c_str());
          return false;
        }
        auto router = agent->get_session_router();
        if(!router) {
          Logger::instance().warn_f(
              "[RESPONSE_PUSH] SessionRouter not available for agent: %s",
              agent_id.c_str());
          return false;
        }

        framework::MMAgentMsgQue::Message msg;
        msg.content = content;
        msg.metadata["final"] = "false";
        msg.metadata["dialogue_id"] = did;
        msg.metadata["session_id"] = sid;
        if(!type.empty())      msg.metadata["type"] = type;
        if(!source.empty())    msg.metadata["source"] = source;
        if(!skill.empty())     msg.metadata["skill"] = skill;
        if(!round_str.empty()) msg.metadata["round"] = round_str;
        if(!status.empty())    msg.metadata["status"] = status;

        bool ok = router->enqueue_response_message(msg);
        if(ok) {
        } else {
          Logger::instance().warn_f(
              "[RESPONSE_PUSH] enqueue_response_message failed: type=%s, "
              "dialogue=%s",
              type.c_str(), did.c_str());
        }
        return ok;
      };
      auto uds_server = std::make_shared<UDSServer>(&databus, uds_socket_path, async_cb, push_cb);
      if(uds_server->start()) {
        state_it->second.uds_socket_path = uds_socket_path;
        state_it->second.uds_server = uds_server;
        Logger::instance().info_f("[WORKFLOW] UDS server started for execution_id=%s at %s",
                                  execution_id.c_str(), uds_socket_path.c_str());
      } else {
        Logger::instance().warn_f("[WORKFLOW] Failed to start UDS server for execution_id=%s",
                                  execution_id.c_str());
      }
    }
  }

  Logger::instance().info_f("Starting workflow execution: %s (execution_id: %s)",
                            workflow->get_name().c_str(), execution_id.c_str());

  // Flow trace: workflow started
  FLOW_TRACE_WORKFLOW_L(session_id, dialogue_id, trace_labels, source_task_id, workflow_id,
                       FlowEventType::WORKFLOW_STARTED);

  // Trace: open a WORKFLOW span (no-op if no active trace on this thread)
  {
    using namespace trace;
    ScopedSpan wf_span(SpanKind::WORKFLOW, workflow_id,
                       {{"agent_id", agent_id},
                        {"workflow_name", workflow->get_name()},
                        {"execution_id", execution_id}});
    (void)wf_span;  // span closes immediately; node spans will be children
  }

  // Initialize all nodes to READY state (these are the cloned nodes, not shared)
  for(const auto& node : workflow->get_nodes()) {
    if(node->get_state() != NodeState::READY) {
      node->set_state(NodeState::READY);
      if(!node->initialize()) {
        Logger::instance().error_f("Failed to initialize node: %s", node->get_name().c_str());
        node->set_state(NodeState::FAILED);
        update_workflow_state(execution_id, node->get_id(), false,
                              "Failed to initialize node: " + node->get_name());
      }
    }
  }

  // Schedule executable nodes
  schedule_workflow_executable_nodes(execution_id);

  return true;
}

void MMAgentRuntime::process_node(AgentPtr agent, WorkflowPtr workflow, NodePtr node,
                                  const std::string& execution_id) {
  if(!agent || !node) {
    Logger::instance().error_f("[PROCESS_NODE_ERROR] Invalid agent or node pointer");
    return;
  }

  std::string dialogue_id;
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    auto                        state_it = workflow_states_.find(execution_id);
    if(state_it == workflow_states_.end() || !state_it->second.in_progress) {
      return;
    }
    auto& state = state_it->second;
    if(state.completed_nodes.count(node->get_id()) || state.failed_nodes.count(node->get_id())
       || state.running_nodes.count(node->get_id()) == 1) {
      return;
    }
    dialogue_id = state_it->second.dialogue_id;
    state.queued_nodes.erase(node->get_id());
    state.running_nodes.insert(node->get_id());
  }

  // [DEBUG] Log node processing start
  std::thread::id thread_id = std::this_thread::get_id();

  Logger::instance().info_f(
      "[PROCESS_NODE_START] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
      "State=%d dialogue=%s",
      std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
      workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
      node->get_name().c_str(), static_cast<int>(node->get_state()),
      dialogue_id.empty() ? "(none)" : dialogue_id.c_str());

  // Hydrate node inputs from DataBus using declared input_params keys.
  // This enables genuine DataBus-driven data flow between nodes/workflows.
  {
    auto& bus = get_databus(agent->get_id());
    DataMap hydrated;
    Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Node=%s has %zu input_params",
                              node->get_id().c_str(), node->get_input_params().size());

    // Get dialogue_id and inputs from workflow state for concurrency-safe isolation
    // Each workflow execution has its own state with isolated inputs
    std::string workflow_dialogue_id;
    std::string workflow_session_id;
    DataMap workflow_inputs;
    {
      std::lock_guard<std::mutex> lock(workflow_state_mutex_);
      auto state_it = workflow_states_.find(execution_id);
      if(state_it != workflow_states_.end()) {
        workflow_dialogue_id = state_it->second.dialogue_id;
        workflow_session_id = state_it->second.session_id;
        workflow_inputs = state_it->second.inputs;  // Copy inputs from state

        // Add session_id to hydrated inputs (always available)
        if(!workflow_session_id.empty()) {
          hydrated["session_id"] = workflow_session_id;
        }

        // Add dialogue_id to hydrated inputs if available
        if(!workflow_dialogue_id.empty()) {
          hydrated["dialogue_id"] = workflow_dialogue_id;
          Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Got input 'dialogue_id'='%s' from workflow state",
                                   workflow_dialogue_id.c_str());
        }

        // Add node_id for multi-language async SDK support
        hydrated["node_id"] = node->get_id();
      }
    }

    for(const auto& key : node->get_input_params()) {
      // Skip if already hydrated from workflow state
      if(hydrated.find(key) != hydrated.end()) {
        continue;
      }

      // First try workflow state inputs (concurrency-safe, each execution has its own copy)
      auto input_it = workflow_inputs.find(key);
      if(input_it != workflow_inputs.end()) {
        hydrated[key] = input_it->second;
        if(const auto* text = std::get_if<std::string>(&input_it->second)) {
          Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Got input '%s'='%s' from workflow state inputs",
                                   key.c_str(), text->c_str());
        }
        continue;
      }

      // Fallback to databus for keys not in workflow inputs
      DataVariant val;
      if(agent->get_data(key, val)) {
        hydrated[key] = val;
        if(const auto* text = std::get_if<std::string>(&val)) {
          Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Got input '%s'='%s' from agent databus", key.c_str(), text->c_str());
        }
      } else if(bus.get(key, val)) {
        hydrated[key] = val;
        if(const auto* text = std::get_if<std::string>(&val)) {
          Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Got input '%s'='%s' from runtime databus", key.c_str(), text->c_str());
        }
      } else {
        Logger::instance().warn_f("[PROCESS_NODE_HYDRATE] Key '%s' not found in workflow inputs or databus", key.c_str());
      }
    }
    // Merge with any existing inputs (e.g. set via set_inputs() or message payload)
    const DataMap& existing = node->get_inputs();
    for(const auto& kv : existing) {
      if(hydrated.find(kv.first) == hydrated.end()) {
        hydrated[kv.first] = kv.second;
      }
    }
    // Carry over all workflow-level inputs not yet in hydrated (e.g. sandbox_endpoint
    // that was propagated from message.metadata via BUILD_RUNTIME_TASK → TASK_POLLER
    // → execute_workflow state.inputs but is not declared in the node's input_params).
    for(const auto& kv : workflow_inputs) {
      if(hydrated.find(kv.first) == hydrated.end()) {
        hydrated[kv.first] = kv.second;
      }
    }
    if(!hydrated.empty()) {
      node->set_inputs(hydrated);
    }
  }

  // Inject DataBus for SO library V2 signature support
  node->set_databus(&get_databus(agent->get_id()));

  // Inject session-isolated DataBus client for V3 signature support
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    auto state_it = workflow_states_.find(execution_id);
    if(state_it != workflow_states_.end()) {
      Logger::instance().info_f("[PROCESS_NODE] Found workflow state for execution_id=%s, session_id='%s'",
                                execution_id.c_str(), state_it->second.session_id.c_str());
      if(!state_it->second.session_id.empty()) {
        // Create client with internal adapter - lifetime managed by shared_ptr
        auto client = std::make_shared<NodeDataBusClientImpl>(
            &get_databus(agent->get_id()), state_it->second.session_id);
        node->set_databus_client(client);
        Logger::instance().info_f("[PROCESS_NODE] Set databus_client for node=%s, session_id='%s'",
                                  node->get_id().c_str(), state_it->second.session_id.c_str());

        // Inject AsyncCall SDK for async operation support
        if(!state_it->second.dialogue_id.empty()) {
          // Get task_id from workflow state (skill_task_id for skill tasks)
          std::string task_id = state_it->second.skill_task_id;

          auto async_sdk = create_async_call_sdk(
              this,
              execution_id,
              state_it->second.session_id,
              state_it->second.dialogue_id,
              task_id,
              node->get_id(),
              state_it->second.async_pending_count);

          client->set_async_sdk(async_sdk);
          Logger::instance().info_f("[PROCESS_NODE] Set async_sdk for node=%s, task_id='%s'",
                                    node->get_id().c_str(), task_id.c_str());
        }

        // Check for async callback data (regardless of state - might have been reset to READY for re-execution)
        {
          auto& databus = get_databus(agent->get_id());
          std::string callback_key = "/async_callback/" + node->get_id();

          DataVariant msg_val, key_val;
          std::string async_msg, async_key;

          if (databus.get(callback_key + "/msg", msg_val)) {
            if (const auto* msg_str = std::get_if<std::string>(&msg_val)) {
              async_msg = *msg_str;
            }
          }
          if (databus.get(callback_key + "/key", key_val)) {
            if (const auto* key_str = std::get_if<std::string>(&key_val)) {
              async_key = *key_str;
            }
          }

          if (!async_msg.empty() || !async_key.empty()) {
            // Inject callback info into node inputs
            DataMap inputs = node->get_inputs();
            inputs["__async_callback__"] = true;
            inputs["__async_key__"] = async_key;
            inputs["__async_msg__"] = async_msg;
            // Also inject session_id, dialogue_id, node_id for SendMsg
            inputs["session_id"] = state_it->second.session_id;
            inputs["dialogue_id"] = state_it->second.dialogue_id;
            inputs["node_id"] = node->get_id();
            node->set_inputs(inputs);

            // Decrement pending callback count
            auto pending_it = state_it->second.pending_callbacks.find(node->get_id());
            if (pending_it != state_it->second.pending_callbacks.end() && pending_it->second > 0) {
              pending_it->second--;
            }

            Logger::instance().info_f("[PROCESS_NODE] Injected async callback: node=%s, key=%s, msg_len=%zu",
                                      node->get_id().c_str(), async_key.c_str(), async_msg.size());

            // Clean up callback data from DataBus
            databus.remove(callback_key + "/msg");
            databus.remove(callback_key + "/key");
          }
        }
      } else {
        Logger::instance().warn_f("[PROCESS_NODE] Session ID is empty for execution_id=%s", execution_id.c_str());
      }
    } else {
      Logger::instance().warn_f("[PROCESS_NODE] No workflow state found for execution_id=%s", execution_id.c_str());
    }
  }

  // Inject plugin UDS path if configured
  if(!config_.plugin_uds_path.empty()) {
    node->set_plugin_uds_path(config_.plugin_uds_path);
  }

  // Inject DataBus UDS path from workflow execution state
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    auto state_it = workflow_states_.find(execution_id);
    if(state_it != workflow_states_.end() && !state_it->second.uds_socket_path.empty()) {
      node->set_databus_uds_path(state_it->second.uds_socket_path);
      Logger::instance().debug_f("[PROCESS_NODE] Injected DataBus UDS path for node=%s: %s",
                                 node->get_id().c_str(), state_it->second.uds_socket_path.c_str());
    }
  }

  // Trace: open a NODE span (RAII — records to TraceManager on scope exit)
  trace::ScopedSpan node_span(
      trace::SpanKind::NODE,
      node->get_name(),
      {{"agent_id",    agent->get_id()},
       {"workflow_id", workflow ? workflow->get_id() : ""},
       {"node_id",     node->get_id()},
       {"node_name",   node->get_name()}});

  // Get session_id, dialogue_id, and trace_labels for flow trace
  std::string flow_session_id;
  std::string flow_dialogue_id;
  std::string flow_trace_labels;
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    auto state_it = workflow_states_.find(execution_id);
    if(state_it != workflow_states_.end()) {
      flow_session_id = state_it->second.session_id;
      flow_dialogue_id = state_it->second.dialogue_id;
      flow_trace_labels = state_it->second.trace_labels;
    }
  }

  // Flow trace: node started
  FLOW_TRACE_NODE_L(flow_session_id, flow_dialogue_id, flow_trace_labels,
                   workflow ? workflow->get_id() : "", node->get_name(),
                   FlowEventType::NODE_STARTED);

  // Execute node
  // 异常收尾：node->execute() 抛异常时若裸奔，NODE_COMPLETED 与 WORKFLOW_COMPLETED 都会缺失
  // （trajectory 出现"只有 start 无 end"死节点，且 workflow 永远达不到 can_complete）。
  // 此处捕获异常：记录日志（M3 三要素 ①）+ 以失败结果继续传导（execution_result=false →
  // 下游 update_workflow_state 失败分支 → WORKFLOW_COMPLETED 正常发出）。
  auto start_time       = std::chrono::steady_clock::now();
  bool execution_result = false;
  std::string node_exception;
  try {
    execution_result = node->execute();
  } catch(const std::exception& e) {
    execution_result = false;
    node_exception = e.what();
    Logger::instance().error_f("[PROCESS_NODE_EXCEPTION] Node=%s threw: %s",
                               node->get_name().c_str(), e.what());
  } catch(...) {
    execution_result = false;
    node_exception = "unknown exception";
    Logger::instance().error_f("[PROCESS_NODE_EXCEPTION] Node=%s threw unknown exception",
                               node->get_name().c_str());
  }
  auto end_time         = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  // Flow trace: node completed
  // Get trace inputs/outputs from node outputs if specified
  const auto& node_outputs = node->get_outputs();
  std::string trace_input;
  std::string trace_output;

  // Keys for node to specify trace data (case-insensitive, lowercase priority)
  static const std::string kTraceInputKeyLower  = "af_trace_node_input";
  static const std::string kTraceInputKeyUpper  = "AF_TRACE_NODE_INPUT";
  static const std::string kTraceOutputKeyLower = "af_trace_node_output";
  static const std::string kTraceOutputKeyUpper = "AF_TRACE_NODE_OUTPUT";

  // Extract trace input: lowercase key has priority over uppercase
  auto input_it = node_outputs.find(kTraceInputKeyLower);
  if(input_it == node_outputs.end()) {
    input_it = node_outputs.find(kTraceInputKeyUpper);
  }
  if(input_it != node_outputs.end()) {
    if(const std::string* str_val = std::get_if<std::string>(&input_it->second)) {
      trace_input = escape_json_string(*str_val);
    }
  }

  // Extract trace output: lowercase key has priority over uppercase
  auto output_it = node_outputs.find(kTraceOutputKeyLower);
  if(output_it == node_outputs.end()) {
    output_it = node_outputs.find(kTraceOutputKeyUpper);
  }
  if(output_it != node_outputs.end()) {
    if(const std::string* str_val = std::get_if<std::string>(&output_it->second)) {
      trace_output = escape_json_string(*str_val);
    }
  }

  FLOW_TRACE_NODE_WITH_DATA_L(flow_session_id, flow_dialogue_id, flow_trace_labels,
                              workflow ? workflow->get_id() : "", node->get_name(),
                              FlowEventType::NODE_COMPLETED,
                              trace_input,
                              node_exception.empty()
                                  ? trace_output
                                  : ("{\"exception\":\"" + escape_json_string(node_exception) + "\"}"),
                              duration * 1000);  // duration in microseconds

  // [DEBUG] Log node processing result
  if(!execution_result) {
    node_span.set_status(trace::SpanStatus::ERROR, "node execute() returned false");
    node_span.add_event("node_failed", {{"duration_ms", std::to_string(duration)}});
    Logger::instance().error_f(
        "[PROCESS_NODE_FAILED] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
        "Duration=%ldms, RetryCount=%d dialogue=%s",
        std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
        workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
        node->get_name().c_str(), duration, node->get_retry_count(),
        dialogue_id.empty() ? "(none)" : dialogue_id.c_str());
  } else {
    node_span.set_status(trace::SpanStatus::OK);
    node_span.add_event("node_success", {{"duration_ms", std::to_string(duration)}});
    Logger::instance().info_f(
        "[PROCESS_NODE_SUCCESS] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
        "Duration=%ldms dialogue=%s",
        std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
        workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
        node->get_name().c_str(), duration,
        dialogue_id.empty() ? "(none)" : dialogue_id.c_str());
  }

  // =========================================================================
  // Edge-based Output Routing (Scenario 1 & Scenario 2 Fix)
  // =========================================================================
  // Core rules:
  // - Has downstream Node → Edge point-to-point transfer to downstream inputs
  // - No downstream Node → DataBus broadcast (with session isolation format)
  // - Multi-incoming edges (Scenario 2): auto-prefix keys with source node ID
  // This naturally isolates parallel node outputs without node code changes.
  // node_outputs already declared above for trace logging
  if(!node_outputs.empty()) {
    auto& bus = get_databus(agent->get_id());
    Logger::instance().info_f("[PROCESS_NODE_OUTPUTS] Node=%s has %zu outputs, dialogue_id=%s",
                              node->get_id().c_str(), node_outputs.size(),
                              flow_dialogue_id.c_str());

    // Check if this node has downstream edges
    bool has_downstream = has_downstream_edge(workflow, node);

    if(has_downstream) {
      // Has downstream → Edge point-to-point transfer
      auto outgoing_edges = get_outgoing_edges(workflow, node);

      for(const auto& edge : outgoing_edges) {
        // Get target node
        auto target_node = workflow->get_node(edge->get_target());
        if(!target_node) {
          Logger::instance().warn_f("[EDGE_TRANSFER] Target node not found: %s",
                                    edge->get_target().c_str());
          continue;
        }

        // Calculate data to transfer
        DataMap transferred;
        if(!edge->get_source_outputs().empty()) {
          // Has config: use edge mapping (explicit renaming)
          edge->transfer_data(node_outputs, transferred);
        } else {
          // Zero-config: check if target has multiple incoming edges
          int incoming_count = count_incoming_edges(workflow, edge->get_target());

          if(incoming_count > 1) {
            // Scenario 2: Multi-incoming edges without config
            // Auto-prefix keys with source node ID to avoid conflicts
            // e.g., node_a.result → "node_a.result", node_b.result → "node_b.result"
            for(const auto& out : node_outputs) {
              std::string prefixed_key = node->get_id() + "." + out.first;
              transferred[prefixed_key] = out.second;
              Logger::instance().info_f("[EDGE_TRANSFER] Multi-incoming: prefixed key '%s' -> '%s'",
                                        out.first.c_str(), prefixed_key.c_str());
            }
          } else {
            // Single incoming edge: pass all outputs directly without prefix
            for(const auto& out : node_outputs) {
              transferred[out.first] = out.second;
            }
          }
        }

        // Merge transferred data into target node inputs
        DataMap target_inputs = target_node->get_inputs();
        for(const auto& [key, value] : transferred) {
          target_inputs[key] = value;
        }
        target_node->set_inputs(target_inputs);

        Logger::instance().info_f("[EDGE_TRANSFER] %s -> %s: %zu keys transferred",
                                  node->get_id().c_str(),
                                  edge->get_target().c_str(),
                                  transferred.size());
      }
    } else {
      // No downstream → DataBus publish (session isolation format)
      for(const auto& out : node_outputs) {
        // Special handling for multi-response key
        if(out.first == "responses") {
          // Parse response content
          std::string response_content;
          bool is_done = false;

          if(const auto* text = std::get_if<std::string>(&out.second)) {
            // Try to parse as JSON for content and done flag
            auto json_doc = dsl::JsonDocumentFactory::create();
            if(json_doc->parse(*text) && json_doc->is_valid()) {
              auto root = json_doc->root();
              if(root && root->is_object()) {
                // Extract content
                response_content = dsl::JsonHelper::get_string(root, "content", *text);
                // Extract done flag
                is_done = dsl::JsonHelper::get_bool(root, "done", false);
              } else {
                // Not an object, use raw text as content
                response_content = *text;
              }
            } else {
              // Not valid JSON, use raw text as content
              response_content = *text;
            }
          }

          // Read total_count as next index (default 0)
          std::string count_key = build_framework_key(
              flow_session_id, flow_dialogue_id,
              workflow ? workflow->get_id() : "unknown",
              "responses/total_count");
          int next_index = 0;
          DataVariant count_val;
          if(bus.get(count_key, count_val)) {
            if(const auto* count_str = std::get_if<std::string>(&count_val)) {
              try {
                next_index = std::stoi(*count_str);
              } catch(...) {
                next_index = 0;
              }
            }
          }

          // Write response content
          std::string content_key = build_framework_key(
              flow_session_id, flow_dialogue_id,
              workflow ? workflow->get_id() : "unknown",
              "responses/" + std::to_string(next_index) + "/content");
          bus.put(content_key, response_content);
          agent->publish_data(content_key, response_content);

          // Set consumed = false
          std::string consumed_key = build_framework_key(
              flow_session_id, flow_dialogue_id,
              workflow ? workflow->get_id() : "unknown",
              "responses/" + std::to_string(next_index) + "/consumed");
          bus.put(consumed_key, std::string("false"));
          agent->publish_data(consumed_key, std::string("false"));

          // Increment and write total_count
          bus.put(count_key, std::to_string(next_index + 1));
          agent->publish_data(count_key, std::to_string(next_index + 1));

          // Initialize consumed_count if not exists
          std::string consumed_count_key = build_framework_key(
              flow_session_id, flow_dialogue_id,
              workflow ? workflow->get_id() : "unknown",
              "responses/consumed_count");
          DataVariant consumed_count_val;
          if(!bus.get(consumed_count_key, consumed_count_val)) {
            bus.put(consumed_count_key, std::string("0"));
            agent->publish_data(consumed_count_key, std::string("0"));
          }

          // If done = true, set completion flag
          if(is_done) {
            std::string done_key = build_framework_key(
                flow_session_id, flow_dialogue_id,
                workflow ? workflow->get_id() : "unknown",
                "responses/done");
            bus.put(done_key, std::string("true"));
            agent->publish_data(done_key, std::string("true"));
          }

          Logger::instance().info_f(
              "[MULTI_RESPONSE_PUBLISH] Published response index=%d, done=%d, dialogue_id=%s",
              next_index, is_done ? 1 : 0, flow_dialogue_id.c_str());
        } else {
          // Standard key: publish directly
          std::string bus_key = build_framework_key(
              flow_session_id, flow_dialogue_id,
              workflow ? workflow->get_id() : "unknown",
              out.first);

          agent->publish_data(bus_key, out.second);
          bus.put(bus_key, out.second);

          Logger::instance().info_f("[DATABUS_PUBLISH] Key=%s", bus_key.c_str());
        }
      }
    }
  }

  bool node_success = (node->get_state() == NodeState::SUCCESS);

  // Check for async pending state
  const DataMap& outputs = node->get_outputs();
  bool is_async_pending = false;
  std::string async_call_key;

  // Debug: log all outputs for async node
  {
    std::string keys;
    for (const auto& [key, val] : outputs) {
      if (!keys.empty()) keys += ", ";
      keys += key;
    }
    Logger::instance().info_f("[ASYNC_CHECK] Node=%s has %zu outputs: [%s]",
                              node->get_id().c_str(), outputs.size(), keys.c_str());
  }

  if (outputs.count("__async_pending__")) {
    const auto& pending_val = outputs.at("__async_pending__");
    Logger::instance().info_f("[ASYNC_CHECK] Found __async_pending__, type_index=%zu", pending_val.index());
    if (const auto* pending_bool = std::get_if<bool>(&pending_val)) {
      is_async_pending = *pending_bool;
      Logger::instance().info_f("[ASYNC_CHECK] Bool value=%s", *pending_bool ? "true" : "false");
    } else if (const auto* pending_str = std::get_if<std::string>(&pending_val)) {
      // Handle string "true" from SO library serialization
      is_async_pending = (*pending_str == "true" || *pending_str == "1");
      Logger::instance().info_f("[ASYNC_CHECK] String value='%s', is_async_pending=%s",
                                  pending_str->c_str(), is_async_pending ? "true" : "false");
    }
  } else {
    Logger::instance().info_f("[ASYNC_CHECK] __async_pending__ not found in outputs");
  }

  if (is_async_pending) {
    // Node is waiting for async callback
    node->set_state(NodeState::ASYNC_PENDING);

    // Get the call_key for callback matching
    if (outputs.count("call_key")) {
      const auto& key_val = outputs.at("call_key");
      if (const auto* key_str = std::get_if<std::string>(&key_val)) {
        async_call_key = *key_str;
        node->set_async_call_key(async_call_key);
      }
    }

    // Update workflow state to track async pending (but don't mark as completed)
    {
      std::lock_guard<std::mutex> lock(workflow_state_mutex_);
      auto state_it = workflow_states_.find(execution_id);
      if (state_it != workflow_states_.end()) {
        state_it->second.running_nodes.erase(node->get_id());
        // Node stays in async pending, not marked as completed
        Logger::instance().info_f("[PROCESS_NODE_ASYNC] Node=%s set to ASYNC_PENDING, call_key=%s",
                                  node->get_id().c_str(), async_call_key.c_str());

        // Check for buffered callbacks (arrived before we transitioned to ASYNC_PENDING)
        auto pending_it = state_it->second.pending_callbacks.find(node->get_id());
        if (pending_it != state_it->second.pending_callbacks.end() && pending_it->second > 0) {
          Logger::instance().info_f("[PROCESS_NODE_ASYNC] Found %d buffered callbacks for node=%s, scheduling re-execution",
                                    pending_it->second, node->get_id().c_str());

          // Reset node state to READY and schedule re-execution
          node->set_state(NodeState::READY);

          // Enqueue the node for re-execution
          {
            std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
            state_it->second.queued_nodes.insert(node->get_id());
            node_task_queue_.push({agent, workflow, node, execution_id});
          }
          g_worker_cv.notify_all();
        }
      }
    }

    // Don't call cleanup() for async pending nodes - preserve state and data for callbacks
    return;  // Don't call update_workflow_state
  }

  std::string failure_details;
  if(!node_success) {
    failure_details = node->get_last_error();
    for(const auto* key : {"stderr", "stdout"}) {
      const auto output_it = outputs.find(key);
      if(output_it == outputs.end()) continue;
      if(const auto* text = std::get_if<std::string>(&output_it->second);
         text && !text->empty()) {
        if(!failure_details.empty()) failure_details += "\n";
        failure_details += std::string(key) + ":\n" + *text;
      }
    }
    constexpr std::size_t kMaxFailureDetails = 12000;
    if(failure_details.size() > kMaxFailureDetails) {
      failure_details = "...[truncated]\n" +
          failure_details.substr(failure_details.size() - kMaxFailureDetails);
    }
  }

  node->cleanup();

  // [DEBUG] Log node cleanup
  Logger::instance().debug_f("[PROCESS_NODE_CLEANUP] ThreadId=%zu, AgentId=%s, NodeId=%s",
                             std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
                             node->get_id().c_str());

  // If this node was previously in ASYNC_PENDING state, decrement the async_pending_count
  // This happens when a node completes after processing all async callbacks
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    auto state_it = workflow_states_.find(execution_id);
    if(state_it != workflow_states_.end()) {
      // Check if this node was in ASYNC_PENDING (by checking if it had async callbacks)
      if(state_it->second.pending_callbacks.count(node->get_id()) > 0 ||
         !node->get_async_call_key().empty()) {
        state_it->second.decrement_async_pending();
        Logger::instance().info_f("[PROCESS_NODE] Decremented async_pending_count for node=%s, new count=%d",
                                  node->get_id().c_str(), state_it->second.async_pending_count->load());
      }
    }
  }

  // Find execution state and update it
  update_workflow_state(execution_id, node->get_id(), node_success, failure_details);
}

void MMAgentRuntime::schedule_workflow_executable_nodes(const std::string& execution_id) {
  std::lock_guard<std::mutex> state_lock(workflow_state_mutex_);

  auto it = workflow_states_.find(execution_id);
  if(it == workflow_states_.end()) {
    return;
  }

  auto& state = it->second;
  if(!state.in_progress) {
    return;
  }

  // Find the agent
  auto agent = get_agent(state.agent_id);
  if(!agent) {
    return;
  }

  // Get the cloned workflow for this execution
  WorkflowPtr workflow;
  {
    std::lock_guard<std::mutex> wf_lock(cloned_workflows_mutex_);
    auto wf_it = cloned_workflows_.find(execution_id);
    if(wf_it != cloned_workflows_.end()) {
      workflow = wf_it->second;
    }
  }

  if(!workflow) {
    Logger::instance().error_f("No cloned workflow found for execution_id: %s", execution_id.c_str());
    return;
  }

  // Flow trace: workflow scheduled（调度可执行节点前发出，枚举已定义但此前零打点）
  FLOW_TRACE_WORKFLOW_L(state.session_id, state.dialogue_id, state.trace_labels,
                        state.source_task_id, workflow->get_id(),
                        FlowEventType::WORKFLOW_SCHEDULED);

  // Get executable nodes
  auto executable_nodes = workflow->get_executable_nodes();

  // Submit executable nodes to task queue
  {
    std::lock_guard<std::mutex> lock(task_queue_mutex_);
    for(const auto& node_id : executable_nodes) {
      auto node = workflow->get_node(node_id);
      if(!node || node->get_state() != NodeState::READY) {
        continue;
      }
      if(state.completed_nodes.count(node_id) || state.failed_nodes.count(node_id)
         || state.queued_nodes.count(node_id) || state.running_nodes.count(node_id)) {
        continue;
      }
      state.queued_nodes.insert(node_id);
      node_task_queue_.push({agent, workflow, node, execution_id});
    }
  }

  // Notify worker threads
  g_worker_cv.notify_all();

  Logger::instance().info_f("Scheduled %zu executable nodes for execution",
                            executable_nodes.size());
}

void MMAgentRuntime::update_workflow_state(const std::string& execution_id, const NodeId& node_id,
                                         bool success, const std::string& failure_details) {
  bool        invoke_complete = false;
  bool        complete_ok     = false;
  std::string complete_agent;
  std::string complete_workflow;
  std::string complete_source_task_id;
  std::string complete_session_id;
  std::string complete_dialogue_id;
  std::string complete_trace_labels;
  std::string complete_trace_id;
  std::string complete_task_type;
  std::string complete_skill_task_id;
  std::string complete_skill_name;
  std::string complete_failure_details;

  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);

    auto it = workflow_states_.find(execution_id);
    if(it == workflow_states_.end()) {
      return;
    }

    auto& state = it->second;
    state.queued_nodes.erase(node_id);
    state.running_nodes.erase(node_id);

    // Only count node as completed once (not on callback re-executions)
    bool already_completed = state.completed_nodes.count(node_id) > 0;

    if(success) {
      if(!already_completed) {
        state.completed_nodes[node_id] = true;
        state.completed_count++;
      }
    } else {
      if(!failure_details.empty()) {
        state.failure_details = failure_details;
      }
      if(state.failed_nodes.count(node_id) == 0) {
        state.failed_nodes[node_id] = true;
      }
    }

    Logger::instance().info_f("MMWorkflow progress: %s - %d/%d completed, %zu failed",
                              execution_id.c_str(), state.completed_count, state.total_nodes,
                              state.failed_nodes.size());

    // Check if workflow can complete (all nodes done AND no async pending)
    bool all_nodes_done = state.completed_count + static_cast<int>(state.failed_nodes.size()) >= state.total_nodes;
    bool can_complete = all_nodes_done && !state.has_async_pending();

    Logger::instance().info_f("WORKFLOW_PROGRESS: %s - all_nodes_done=%s, has_async_pending=%s, can_complete=%s",
                              execution_id.c_str(),
                              all_nodes_done ? "true" : "false",
                              state.has_async_pending() ? "true" : "false",
                              can_complete ? "true" : "false");

    if(can_complete) {
      Logger::instance().info_f("[WORKFLOW_COMPLETE] Workflow %s completing successfully dialogue=%s", execution_id.c_str(),
                                state.dialogue_id.empty() ? "(none)" : state.dialogue_id.c_str());
      state.in_progress   = false;
      invoke_complete     = true;
      complete_ok         = state.failed_nodes.empty();
      complete_agent      = state.agent_id;
      complete_workflow   = state.workflow_id;
      complete_source_task_id = state.source_task_id;
      complete_session_id     = state.session_id;
      complete_dialogue_id    = state.dialogue_id;
      complete_trace_labels   = state.trace_labels;
      complete_trace_id        = state.trace_id;
      complete_task_type      = state.task_type;
      complete_skill_task_id  = state.skill_task_id;
      complete_skill_name     = state.skill_name;
      complete_failure_details = state.failure_details;
    } else if(success) {
      // Get the cloned workflow for this execution
      WorkflowPtr workflow;
      {
        std::lock_guard<std::mutex> wf_lock(cloned_workflows_mutex_);
        auto wf_it = cloned_workflows_.find(execution_id);
        if(wf_it != cloned_workflows_.end()) {
          workflow = wf_it->second;
        }
      }
      if(workflow) {
        workflow->mark_node_executed(node_id, true);
        auto next_nodes = workflow->get_executable_nodes();
        if(!next_nodes.empty()) {
          auto agent = get_agent(state.agent_id);
          std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
          for(const auto& next_id : next_nodes) {
            auto next_node = workflow->get_node(next_id);
            if(!next_node || next_node->get_state() != NodeState::READY) {
              continue;
            }
            if(state.completed_nodes.count(next_id) || state.failed_nodes.count(next_id)
               || state.queued_nodes.count(next_id) || state.running_nodes.count(next_id)) {
              continue;
            }
            state.queued_nodes.insert(next_id);
            node_task_queue_.push({agent, workflow, next_node, execution_id});
          }
          g_worker_cv.notify_all();
        }
      }
    }
  }

  if(invoke_complete) {
    // Flow trace: workflow completed
    FLOW_TRACE_WORKFLOW_L(complete_session_id, complete_dialogue_id, complete_trace_labels,
                         complete_source_task_id, complete_workflow,
                         FlowEventType::WORKFLOW_COMPLETED);

    if(complete_ok) {
      Logger::instance().info_f("MMWorkflow execution completed successfully: %s",
                                complete_workflow.c_str());
    } else {
      Logger::instance().error_f("MMWorkflow execution failed: %s", complete_workflow.c_str());
    }

    // Send data_exchange message after workflow completes
    // This triggers the next step in the agent's task flow
    auto agent = get_agent(complete_agent);
    if(agent && !complete_source_task_id.empty()) {
      Logger::instance().info_f(
          "[WORKFLOW_COMPLETE] Sending data_exchange for agent=%s, workflow=%s, source_task=%s, session=%s, success=%d",
          complete_agent.c_str(), complete_workflow.c_str(), complete_source_task_id.c_str(),
          complete_session_id.c_str(), complete_ok ? 1 : 0);

      // Find the source task and send data_exchange message
      if(!complete_session_id.empty()) {
        if(auto session = agent->get_session_by_key(complete_session_id); session) {
          if(auto source_task = session->get_task(complete_source_task_id); source_task) {
            framework::MMAgentMsgQue::Message response;
            response.message_id = complete_source_task_id;
            response.type = framework::MMAgentMsgQue::MessageType::TASK_RESPONSE;
            response.sender_id = "runtime-worker";
            response.receiver_id = complete_source_task_id;
            // Use the stored task_type from the workflow execution state
            // This ensures correct task_type for skill tasks launched by dispatcher
            response.metadata["task_type"] = complete_task_type.empty() ? source_task->get_task_type() : complete_task_type;
            response.metadata["workflow_id"] = complete_workflow;
            response.metadata["message_type"] = "data_exchange";
            response.metadata["dialogue_id"] = complete_dialogue_id;
            // Pass skill_name for skill completion tracking
            response.metadata["skill_name"] = complete_skill_name;
            // Terminal node outputs are stored under the session-isolated framework
            // key. Carry the skill result on the completion event so it survives the
            // workflow boundary and does not depend on legacy, unqualified lookups.
            if(complete_task_type == "skill") {
              DataVariant skill_result;
              const std::string skill_result_key = build_framework_key(
                  complete_session_id, complete_dialogue_id, complete_workflow,
                  "skill_result");
              if(get_databus(complete_agent).get(skill_result_key, skill_result)) {
                if(const auto* text = std::get_if<std::string>(&skill_result);
                   text && !text->empty()) {
                  response.metadata["skill_result"] = *text;
                  Logger::instance().info_f(
                      "[WORKFLOW_COMPLETE] Attached skill_result (%zu bytes)",
                      text->size());
                }
              }
              if(!complete_ok && !response.metadata.count("skill_result")) {
                response.metadata["skill_result"] =
                    "{\"status\":\"failed\",\"error\":\"" +
                    escape_json_string(complete_failure_details.empty()
                                           ? "workflow failed"
                                           : complete_failure_details) +
                    "\"}";
                Logger::instance().info_f(
                    "[WORKFLOW_COMPLETE] Attached synthesized failure result (%zu bytes)",
                    response.metadata["skill_result"].size());
              }
              DataVariant business_skill_name;
              const std::string business_skill_name_key = build_framework_key(
                  complete_session_id, complete_dialogue_id, complete_workflow,
                  "skill_name");
              if(get_databus(complete_agent).get(business_skill_name_key,
                                                 business_skill_name)) {
                if(const auto* text = std::get_if<std::string>(&business_skill_name);
                   text && !text->empty()) {
                  response.metadata["business_skill_name"] = *text;
                  Logger::instance().info_f(
                      "[WORKFLOW_COMPLETE] Attached business_skill_name=%s",
                      text->c_str());
                }
              }
              DataVariant artifacts_json;
              const std::string artifacts_key = build_framework_key(
                  complete_session_id, complete_dialogue_id, complete_workflow,
                  "artifacts_json");
              if(get_databus(complete_agent).get(artifacts_key, artifacts_json)) {
                if(const auto* text = std::get_if<std::string>(&artifacts_json);
                   text && !text->empty() && *text != "[]") {
                  response.metadata["artifacts_json"] = *text;
                }
              }
            }
            // Pass trace_labels for flow trace correlation across workflow boundaries
            if(!complete_trace_labels.empty()) {
              response.metadata["trace_labels"] = complete_trace_labels;
            }
            // Pass trace_id for end-to-end correlation across workflow boundaries
            if(!complete_trace_id.empty()) {
              response.metadata["trace_id"] = complete_trace_id;
            }
            response.content = complete_ok ? "workflow_completed" : "workflow_failed";
            source_task->enqueue_data_exchange_message(response);
            Logger::instance().info_f(
                "[WORKFLOW_COMPLETE] Enqueued data_exchange to task=%s, task_type=%s, dialogue_id=%s",
                complete_source_task_id.c_str(), complete_task_type.c_str(), complete_dialogue_id.c_str());
          }
        }
      }
    }

    on_workflow_complete(complete_agent, complete_workflow, execution_id, complete_ok);
  }
}

void MMAgentRuntime::on_node_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                                    const NodeId& node_id, bool success) {
  Logger::instance().info_f("MMNode complete event: agent=%s, workflow=%s, node=%s, success=%d",
                            agent_id.c_str(), workflow_id.c_str(), node_id.c_str(),
                            success ? 1 : 0);
}

void MMAgentRuntime::on_workflow_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                                        const std::string& execution_id, bool success) {
  Logger::instance().info_f("MMWorkflow complete event: agent=%s, workflow=%s, execution_id=%s, success=%d",
                            agent_id.c_str(), workflow_id.c_str(), execution_id.c_str(), success ? 1 : 0);

  // Clean up the specific cloned workflow
  {
    std::lock_guard<std::mutex> lock(cloned_workflows_mutex_);
    auto it = cloned_workflows_.find(execution_id);
    if(it != cloned_workflows_.end()) {
      cloned_workflows_.erase(it);
      Logger::instance().debug_f("Cleaned up cloned workflow for execution_id: %s", execution_id.c_str());
    }
  }

  // Emit TASK_DESTROYED event before cleaning up execution state
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    auto it = workflow_states_.find(execution_id);
    if(it != workflow_states_.end()) {
      const auto& state = it->second;

      // Stop and clean up UDS server (lifecycle tied to workflow execution)
      if(state.uds_server) {
        state.uds_server->stop();
        Logger::instance().info_f("[WORKFLOW] UDS server stopped for execution_id=%s, socket=%s",
                                  execution_id.c_str(), state.uds_socket_path.c_str());
      }

      TaskEvent event = EventBuilder::build_task_destroyed_event(
          agent_id,
          state.session_id,
          state.dialogue_id,
          state.source_task_id);
      emit_task_event(event);
      workflow_states_.erase(it);
    }
  }
}

void MMAgentRuntime::update_worker_load(int worker_id, int load_change) {
  std::lock_guard<std::mutex> lock(worker_load_mutex_);

  if(worker_id >= 0 && worker_id < num_worker_threads_) {
    if(worker_loads_.size() <= static_cast<size_t>(worker_id)) {
      worker_loads_.resize(num_worker_threads_, 0);
    }
    worker_loads_[worker_id] += load_change;
  }
}

int MMAgentRuntime::get_least_loaded_worker() {
  std::lock_guard<std::mutex> lock(worker_load_mutex_);

  if(worker_loads_.empty()) {
    return 0;
  }

  int min_load   = worker_loads_[0];
  int min_worker = 0;

  for(size_t i = 1; i < worker_loads_.size(); ++i) {
    if(worker_loads_[i] < min_load) {
      min_load   = worker_loads_[i];
      min_worker = static_cast<int>(i);
    }
  }

  return min_worker;
}

void MMAgentRuntime::monitor_agents() {
  // MMAgent Level Master: monitor agent health
  std::vector<AgentId> agent_ids;
  {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agent_ids.reserve(agents_.size());
    for(const auto& pair : agents_) {
      agent_ids.push_back(pair.first);
    }
  }

  for(const auto& agent_id : agent_ids) {
    check_agent_health(agent_id);
  }
}

void MMAgentRuntime::agent_registration_loop() {
  while(is_running_) {
    AgentRegistrationRequest request;
    bool has_request = false;
    {
      std::unique_lock<std::mutex> lock(agent_registration_mutex_);
      agent_registration_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                      [this]() { return !agent_registration_queue_.empty() || !is_running_; });
      if(!is_running_) {
        break;
      }
      if(!agent_registration_queue_.empty()) {
        request = agent_registration_queue_.front();
        agent_registration_queue_.pop();
        has_request = true;
      }
    }
    if(!has_request || !request.agent) {
      continue;
    }

    bool ok = false;
    {
      std::lock_guard<std::mutex> lock(agents_mutex_);
      const auto&                 agent_id = request.agent->get_id();
      if(request.unregister) {
        auto it = agents_.find(agent_id);
        if(it != agents_.end()) {
          // Cascade: clear all session data before destroying the DataBus
          auto session_ids = it->second->get_all_session_ids();
          Logger::instance().info_f("[Runtime] worker unregister %s: cleaning %zu sessions before DataBus teardown",
                                    agent_id.c_str(), session_ids.size());
          for(const auto& sid : session_ids) {
            int cleared = it->second->clear_session_data(sid);
            Logger::instance().debug_f("[Runtime] worker unregister %s: cleared session=%s keys=%d",
                                       agent_id.c_str(), sid.c_str(), cleared);
          }

          // P0-4 修复方案 A：析构顺序与 unregister_agent 一致（worker 解引用 -> stop -> erase databus -> erase agent）
          if(local_worker_) {
            local_worker_->remove_databus(agent_id);
          }
          it->second->stop();
          databuses_.erase(agent_id);
          agent_heartbeats_.erase(agent_id);
          agents_.erase(it);
          ok = true;
        }
      } else {
        agents_[agent_id] = request.agent;
        size_t databus_capacity = static_cast<size_t>(request.agent->get_config().data_bus_capacity_mb) * 1024 * 1024;
        databuses_[agent_id] = std::unique_ptr<MMDataBus>(new MMDataBus(databus_capacity));
        Logger::instance().info_f("DataBus initialized for agent_id=%s, capacity=%zu bytes (%d MB)",
                                  agent_id.c_str(), databus_capacity, request.agent->get_config().data_bus_capacity_mb);
        agent_heartbeats_[agent_id] = now_ms();
        // Set the agent's runtime_databus pointer for unified data storage
        request.agent->set_runtime_databus(databuses_[agent_id].get());
        // Inject DataBus to local worker
        if(local_worker_) {
          local_worker_->set_databus(agent_id, databuses_[agent_id].get());
        }
        ok = true;
      }
    }
    if(ok && !request.unregister && request.agent) {
      if(local_worker_) {
        for(const auto& workflow : request.agent->get_workflows()) {
          if(workflow) {
            local_worker_->add_supported_workflow(workflow->get_id());
          }
        }
      }
      initialize_agent_workflow_pools(request.agent);

      // Emit AGENT_REGISTERED event
      auto workflows_info = build_workflow_infos(request.agent);
      AgentEvent event = EventBuilder::build_agent_registered_event(
          request.agent->get_id(),
          request.agent->get_name(),
          request.agent->get_config().version,
          request.agent->get_config().description,
          workflows_info);
      emit_agent_event(event);

      // Set up session event callback to emit SESSION_CREATED/SESSION_DESTROYED events
      request.agent->set_session_event_callback(
          [this, agent_id = request.agent->get_id()](
              const std::string& session_id,
              const std::string& user_id,
              const std::string& client_id,
              framework::SessionEventType event_type) {
            if(event_type == framework::SessionEventType::CREATED) {
              SessionEvent event = EventBuilder::build_session_created_event(
                  agent_id, user_id, client_id, session_id);
              emit_session_event(event);
            } else if(event_type == framework::SessionEventType::DESTROYED) {
              SessionEvent event = EventBuilder::build_session_destroyed_event(
                  agent_id, session_id);
              emit_session_event(event);
            }
          });

      // Set up dialogue event callback to emit DIALOGUE_CREATED/DESTROYED events
      request.agent->set_dialogue_event_callback(
          [this](
              const std::string& agent_id,
              const std::string& session_id,
              const std::string& dialogue_id,
              const std::string& user_input,
              framework::DialogueEventType event_type) {
            if(event_type == framework::DialogueEventType::CREATED) {
              auto agent = get_agent(agent_id);
              std::string user_id = agent && agent->get_session(session_id)
                                          ? agent->get_session(session_id)->get_user_id() : "";
              std::string client_id = agent && agent->get_session(session_id)
                                          ? agent->get_session(session_id)->get_client_id() : "";
              DialogueEvent event = EventBuilder::build_dialogue_created_event(
                  agent_id, user_id, client_id, session_id, dialogue_id, user_input);
              emit_dialogue_event(event);
            } else if(event_type == framework::DialogueEventType::DESTROYED) {
              DialogueEvent event = EventBuilder::build_dialogue_destroyed_event(
                  agent_id, session_id, dialogue_id);
              emit_dialogue_event(event);
            }
          });

      // Set up task event callback to emit TASK_CREATED events
      request.agent->set_task_event_callback(
          [this](
              const std::string& agent_id,
              const std::string& session_id,
              const std::string& dialogue_id,
              const std::string& task_id,
              const std::string& workflow_id,
              const std::string& task_type,
              const std::string& skill_name,
              framework::TaskEventType event_type) {
            if(event_type == framework::TaskEventType::CREATED) {
              auto agent = get_agent(agent_id);
              std::string user_id = agent && agent->get_session(session_id)
                                          ? agent->get_session(session_id)->get_user_id() : "";
              std::string client_id = agent && agent->get_session(session_id)
                                          ? agent->get_session(session_id)->get_client_id() : "";
              TaskEvent event = EventBuilder::build_task_created_event(
                  agent_id, user_id, client_id, session_id, dialogue_id,
                  task_id, workflow_id, task_type, skill_name);
              emit_task_event(event);
            }
          });
    }
    if(ok && request.unregister && request.agent) {
      // Emit AGENT_UNREGISTERED event
      AgentEvent event = EventBuilder::build_agent_unregistered_event(request.agent->get_id());
      emit_agent_event(event);
    }
    if(request.done && request.result && request.done_cv && request.done_mutex) {
      {
        std::lock_guard<std::mutex> done_lock(*request.done_mutex);
        *request.result = ok;
        *request.done = true;
      }
      request.done_cv->notify_all();
    }
  }
}

void MMAgentRuntime::process_agent_state_change(const AgentId& agent_id, bool is_healthy) {
  if(is_healthy || !shared_state_ || !deployer_ || !scheduler_) {
    return;
  }
  auto workflows = shared_state_->list_workflow_instances_by_agent_instance(agent_id);
  for(const auto& workflow : workflows) {
    deployer_->remove_workflow_instance(workflow.workflow_instance_id);
    shared_state_->remove_workflow_instance(workflow.workflow_instance_id);
  }
  deployer_->handle_agent_state_change(agent_id, false);
  scheduler_->handle_agent_state_change(agent_id, false);
}

void MMAgentRuntime::agent_monitor_loop() {
  constexpr int64_t kHeartbeatTimeoutMs = 10000;
  while(is_running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<AgentId> unhealthy_agents;
    {
      std::lock_guard<std::mutex> lock(agents_mutex_);
      const int64_t               now = now_ms();
      for(const auto& kv : agent_heartbeats_) {
        if(now - kv.second > kHeartbeatTimeoutMs) {
          unhealthy_agents.push_back(kv.first);
        }
      }
    }
    for(const auto& agent_id : unhealthy_agents) {
      process_agent_state_change(agent_id, false);
    }
  }
}

void MMAgentRuntime::agent_task_poller_loop() {
  // Consumes tasks generated via the msgque path (route_request_message →
  // process_all_sessions_until_idle → generated_tasks_) and dispatches them
  // to execute_workflow() so worker threads can execute the workflow nodes.
  while(is_running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<AgentPtr> agents_snap;
    {
      std::lock_guard<std::mutex> lock(agents_mutex_);
      agents_snap.reserve(agents_.size());
      for(auto& [id, agent] : agents_) {
        agents_snap.push_back(agent);
      }
    }
    for(auto& agent : agents_snap) {
      if(!agent) continue;
      std::shared_ptr<framework::MMAgentTask> task;
      while(agent->pop_generated_task(task) && task) {
        const auto& workflow_id = task->get_config().workflow_id;
        if(workflow_id.empty()) continue;
        DataMap inputs;
        // Propagate task parameters (message_content → user_text, session_id, etc.)
        std::string msg_content;
        if(task->get_parameter("message_content", msg_content) && !msg_content.empty()) {
          inputs["input"]     = msg_content;
          inputs["user_text"] = msg_content;
          // Payload truncation: log preview only, full content in debug
          std::string preview = msg_content.substr(0, 120);
          if(msg_content.size() > 120) preview += "...";
          Logger::instance().info_f("[TASK_POLLER] Set inputs[input] preview='%s' (full_len=%zu) from message_content",
                                   preview.c_str(), msg_content.size());
        } else {
          Logger::instance().warn_f("[TASK_POLLER] message_content not found or empty for task=%s", task->get_task_id().c_str());
        }
        if(!task->get_config().session_id.empty()) {
          inputs["session_id"] = task->get_config().session_id;
        }
        // Propagate dialogue_id for multi-dialogue support
        std::string dialogue_id;
        if(task->get_parameter("dialogue_id", dialogue_id) && !dialogue_id.empty()) {
          inputs["dialogue_id"] = dialogue_id;
          Logger::instance().info_f("[TASK_POLLER] Set inputs[dialogue_id]='%s'", dialogue_id.c_str());
        }
        // Propagate user_id / client_id for artifact routing. AF_SESSION_ID 是执行槽位
        // (session slot, 0/1/2)，不是用户会话标识；产物目录与 artifact_id 需用
        // {user_id}_{client_id} 作为 session_id 段（见 docs/design/features/sandbox/artifact-storage.md §2.2）。
        std::string user_id;
        if(task->get_parameter("user_id", user_id) && !user_id.empty()) {
          inputs["user_id"] = user_id;
        }
        std::string client_id;
        if(task->get_parameter("client_id", client_id) && !client_id.empty()) {
          inputs["client_id"] = client_id;
        }
        // Propagate stream flag from task parameter to node inputs
        std::string stream_str;
        if(task->get_parameter("stream", stream_str) && stream_str == "true") {
          inputs["stream"] = "true";
          Logger::instance().info_f("[TASK_POLLER] Set inputs[stream]=true from task parameter");
        }
        // Propagate completed_skill_name for coordinator to track skill completion
        std::string completed_skill_name;
        if(task->get_parameter("completed_skill_name", completed_skill_name) && !completed_skill_name.empty()) {
          inputs["completed_skill_name"] = completed_skill_name;
          Logger::instance().info_f("[TASK_POLLER] Set inputs[completed_skill_name]='%s'", completed_skill_name.c_str());
        }
        // Propagate skill_success for coordinator workflow to know skill execution result
        std::string skill_success;
        if(task->get_parameter("skill_success", skill_success) && !skill_success.empty()) {
          inputs["skill_success"] = skill_success;
          Logger::instance().info_f("[TASK_POLLER] Set inputs[skill_success]='%s'", skill_success.c_str());
        }
        std::string skill_result;
        if(task->get_parameter("skill_result", skill_result) && !skill_result.empty()) {
          inputs["skill_result"] = skill_result;
          Logger::instance().info_f("[TASK_POLLER] Propagated skill_result (%zu bytes)",
                                    skill_result.size());
        }
        std::string skill_artifacts;
        if(task->get_parameter("skill_artifacts", skill_artifacts) &&
           !skill_artifacts.empty()) {
          inputs["skill_artifacts"] = skill_artifacts;
        }
        // Propagate exec_skill_key for skill-specific data routing
        std::string exec_skill_key;
        if(task->get_parameter("exec_skill_key", exec_skill_key) && !exec_skill_key.empty()) {
          inputs["exec_skill_key"] = exec_skill_key;
          Logger::instance().info_f("[TASK_POLLER] Set inputs[exec_skill_key]='%s'", exec_skill_key.c_str());
        }
        // Propagate sandbox_endpoint + sandbox_mode from task parameters to node inputs
        // for worker env var injection (AF_SANDBOX_ENDPOINT + AF_SANDBOX_MODE).
        // factory.py 同时读这两个 env 才能选对 executor；漏 sandbox_mode 会让 factory 落到 HostExecutor。
        std::string sandbox_endpoint;
        if(task->get_parameter("sandbox_endpoint", sandbox_endpoint) && !sandbox_endpoint.empty()) {
          inputs["sandbox_endpoint"] = sandbox_endpoint;
          Logger::instance().info_f("[TASK_POLLER] Set inputs[sandbox_endpoint]='%s'", sandbox_endpoint.c_str());
        }
        std::string sandbox_mode;
        if(task->get_parameter("sandbox_mode", sandbox_mode) && !sandbox_mode.empty()) {
          inputs["sandbox_mode"] = sandbox_mode;
          Logger::instance().info_f("[TASK_POLLER] Set inputs[sandbox_mode]='%s'", sandbox_mode.c_str());
        }
        std::string platform_context;
        if(task->get_parameter("platform_context", platform_context) &&
           !platform_context.empty()) {
          inputs["platform_context"] = platform_context;
        }
        std::string request_id;
        if(task->get_parameter("request_id", request_id) && !request_id.empty()) {
          inputs["request_id"] = request_id;
        }
        // Inject self_task_id for node self-identification
        inputs["self_task_id"] = task->get_config().task_id;
        std::string task_owner_id = task->get_config().task_id;
        task->get_parameter("source_task_id", task_owner_id);
        if(task_owner_id.empty()) {
          task_owner_id = task->get_config().task_id;
        }
        ensure_workflow_pool(agent->get_id(), task_owner_id, workflow_id, task->get_workflow_file(),
                             task->get_workflow_pool_size());
        master::TaskScheduleRequest schedule_request;
        schedule_request.task_id = task->get_config().task_id;
        schedule_request.agent_object_id = task_owner_id;
        schedule_request.agent_instance_id = task_owner_id;
        schedule_request.workflow_id = workflow_id;
        schedule_request.workflow_file = task->get_workflow_file();
        auto schedule_result = schedule_task(schedule_request);
        if(schedule_result.scheduled) {
          inputs["workflow_instance_id"] = schedule_result.workflow_instance_id;
          inputs["worker_id"] = schedule_result.worker_id;
          if(local_worker_ && schedule_result.worker_id == local_worker_->get_worker_id()) {
            MMWorkerEngine::TaskItem task_item;
            task_item.task_id = task->get_config().task_id;
            task_item.workflow_id = workflow_id;
            task_item.workflow_instance_id = schedule_result.workflow_instance_id;
            local_worker_->enqueue_task(task_item);
          }
        }
        Logger::instance().info_f(
            "[TASK_POLLER] Dispatching task=%s workflow=%s agent=%s",
            task->get_config().task_id.c_str(), workflow_id.c_str(),
            agent->get_id().c_str());

        // Get source task info for workflow completion callback
        // source_task_id is the task that launched this workflow (for tracing)
        // task_type is the type of the task executing this workflow
        std::string task_source_id;
        std::string task_session_id;
        task->get_parameter("source_task_id", task_source_id);
        task->get_parameter("session_id", task_session_id);
        if(task_source_id.empty()) {
          task_source_id = task->get_config().task_id;
        }
        std::string task_type = task->get_task_type();

        // Get dialogue_id from task parameters for multi-dialogue support
        std::string task_dialogue_id;
        task->get_parameter("dialogue_id", task_dialogue_id);

        // Get trace_labels from task parameters for flow trace correlation
        std::string task_trace_labels;
        task->get_parameter("trace_labels", task_trace_labels);

        // Get trace_id from task parameters for end-to-end correlation
        std::string task_trace_id;
        task->get_parameter("trace_id", task_trace_id);

        // Get skill task id for completion tracking (the actual task id of this skill)
        std::string skill_task_id = task->get_config().task_id;

        // Get skill name for skill completion notification
        std::string skill_name;
        if(task_type == "skill") {
          skill_name = task->get_skill_name();
        }

        const bool scheduled = execute_workflow(agent->get_id(), workflow_id, inputs,
                                                task_source_id, task_session_id, task_dialogue_id,
                                                task_trace_labels, task_trace_id,
                                                task_type, skill_task_id, skill_name);
        framework::MMAgentMsgQue::Message response;
        std::string response_message_id;
        if(!task->get_parameter("message_id", response_message_id) || response_message_id.empty()) {
          response_message_id = task->get_config().task_id;
        }
        response.message_id = response_message_id;
        response.type = framework::MMAgentMsgQue::MessageType::TASK_RESPONSE;
        response.sender_id = "runtime-worker";
        response.receiver_id = task->get_config().task_id;
        response.metadata["task_type"] = task->get_task_type();
        response.metadata["workflow_id"] = workflow_id;
        response.metadata["workflow_instance_id"] = schedule_result.workflow_instance_id;
        response.metadata["worker_id"] = schedule_result.worker_id;
        response.metadata["message_type"] = "data_exchange";
        response.metadata["dialogue_id"] = task_dialogue_id;
        // Propagate trace_id for end-to-end correlation in early data_exchange
        if(!task_trace_id.empty()) {
          response.metadata["trace_id"] = task_trace_id;
        }
        response.content = scheduled ? "workflow_scheduled" : "workflow_schedule_failed";
        std::shared_ptr<framework::MMAgentTask> response_target = task;
        std::string source_task_id;
        std::string session_id;
        if(task->get_parameter("source_task_id", source_task_id)
           && task->get_parameter("session_id", session_id) && !source_task_id.empty()
           && !session_id.empty()) {
          if(auto session = agent->get_session_by_key(session_id); session) {
            if(auto source_task = session->get_task(source_task_id); source_task) {
              response_target = source_task;
            }
          }
        }
        response.receiver_id = response_target->get_config().task_id;
        if(scheduled) {
          response_target->acknowledge_dispatch_record(response.message_id);
        }
        response_target->enqueue_data_exchange_message(response);
      }
    }
  }
}

void MMAgentRuntime::check_agent_health(const AgentId& agent_id) {
  // Check agent health (simplified implementation)
  auto agent = get_agent(agent_id);
  if(agent) {
    auto state = agent->get_state();
    // In production, check agent health metrics
    // For now, just log the state
    Logger::instance().info_f("MMAgent health check: %s (state: %d)", agent_id.c_str(),
                              static_cast<int>(state));
  }
}

int MMAgentRuntime::get_active_agents() const {
  std::lock_guard<std::mutex> lock(agents_mutex_);
  return static_cast<int>(agents_.size());
}

int MMAgentRuntime::get_active_workflows() const {
  std::lock_guard<std::mutex> lock(agents_mutex_);
  int                         count = 0;
  for(const auto& pair : agents_) {
    count += static_cast<int>(pair.second->get_workflows().size());
  }
  return count;
}

int MMAgentRuntime::get_running_nodes() const {
  // Count nodes in EXECUTING state across all workflows
  int running_count = 0;

  std::lock_guard<std::mutex> lock(agents_mutex_);
  for(const auto& pair : agents_) {
    const auto& workflows = pair.second->get_workflows();
    for(const auto& workflow : workflows) {
      const auto& nodes = workflow->get_nodes();
      for(const auto& node : nodes) {
        if(node->get_state() == NodeState::EXECUTING) {
          running_count++;
        }
      }
    }
  }

  return running_count;
}

bool MMAgentRuntime::start() {
  if(is_running_) {
    return false;
  }

  is_running_ = true;
  g_shutdown  = false;

  // Initialize worker load tracking
  {
    std::lock_guard<std::mutex> lock(worker_load_mutex_);
    worker_loads_.resize(num_worker_threads_, 0);
  }

  // Start callback thread pool for async event handling
  start_callback_threads();

  // Start worker threads
  for(int i = 0; i < num_worker_threads_; ++i) {
    worker_threads_.emplace_back(&MMAgentRuntime::worker_thread_main, this);
  }
  agent_registration_thread_ = std::thread(&MMAgentRuntime::agent_registration_loop, this);
  agent_monitor_thread_ = std::thread(&MMAgentRuntime::agent_monitor_loop, this);
  agent_task_poller_thread_ = std::thread(&MMAgentRuntime::agent_task_poller_loop, this);

  if(local_worker_) {
    local_worker_->set_status(MMWorkerEngine::WorkerStatus::RUNNING);
    local_worker_->start();
  }
  if(deployer_ && local_worker_ && (role_ == RuntimeRole::MASTER || role_ == RuntimeRole::WORKER)) {
    deployer_->register_worker(local_worker_);
  }
  {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    for(const auto& [agent_id, agent] : agents_) {
      (void)agent_id;
      initialize_agent_workflow_pools(agent);
    }
  }

  Logger::instance().info_f("MMAgentRuntime started with %d worker threads", num_worker_threads_);
  return true;
}

bool MMAgentRuntime::stop() {
  if(!is_running_) {
    return false;
  }

  // Phase 1: Enter draining mode — stop accepting new tasks
  draining_ = true;
  Logger::instance().info("MMAgentRuntime entering drain mode...");

  // Phase 2: Wait for in-progress workflows to complete (with timeout)
  const int  max_drain_wait_ms = 5000;
  const int  poll_interval_ms  = 50;
  int        waited_ms         = 0;
  bool       drained           = false;

  while(waited_ms < max_drain_wait_ms) {
    {
      std::lock_guard<std::mutex> lock(workflow_state_mutex_);
      bool has_active = false;
      for(const auto& pair : workflow_states_) {
        if(pair.second.in_progress) {
          has_active = true;
          break;
        }
      }
      if(!has_active) {
        drained = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    waited_ms += poll_interval_ms;
  }

  if(!drained) {
    Logger::instance().warn_f("MMAgentRuntime drain timeout after %dms, forcing shutdown", waited_ms);
  }

  // Phase 3: Shutdown workers
  is_running_ = false;
  g_shutdown  = true;

  // Stop callback thread pool
  stop_callback_threads();

  // Notify all worker threads
  g_worker_cv.notify_all();
  agent_registration_cv_.notify_all();

  // Wait for all worker threads to finish
  for(auto& thread : worker_threads_) {
    if(thread.joinable()) {
      thread.join();
    }
  }
  worker_threads_.clear();
  if(agent_registration_thread_.joinable()) {
    agent_registration_thread_.join();
  }
  if(agent_monitor_thread_.joinable()) {
    agent_monitor_thread_.join();
  }
  if(agent_task_poller_thread_.joinable()) {
    agent_task_poller_thread_.join();
  }

  draining_ = false;
  if(local_worker_) {
    local_worker_->stop();
    local_worker_->set_status(MMWorkerEngine::WorkerStatus::STOPPED);
  }
  Logger::instance().info("MMAgentRuntime stopped");
  return true;
}

MMDataBus& MMAgentRuntime::get_databus(const AgentId& agent_id) {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  auto it = databuses_.find(agent_id);
  if(it == databuses_.end()) {
    // If databus doesn't exist, create one with agent's configured capacity
    // or default 1MB if agent not found
    size_t capacity = 1024 * 1024;  // Default 1MB
    int capacity_mb = 1;
    auto agent_it = agents_.find(agent_id);
    if(agent_it != agents_.end() && agent_it->second) {
      capacity_mb = agent_it->second->get_config().data_bus_capacity_mb;
      capacity = static_cast<size_t>(capacity_mb) * 1024 * 1024;
    }
    databuses_[agent_id] = std::unique_ptr<MMDataBus>(new MMDataBus(capacity));
    Logger::instance().info_f("DataBus initialized for agent_id=%s, capacity=%zu bytes (%d MB)",
                              agent_id.c_str(), capacity, capacity_mb);
    return *databuses_[agent_id];
  }

  return *it->second;
}

int MMAgentRuntime::clear_session_data(const AgentId& agent_id, const std::string& session_id) {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  auto agent_it = agents_.find(agent_id);
  if(agent_it == agents_.end() || !agent_it->second) {
    Logger::instance().warn_f("[Runtime] Agent not found for session cleanup: %s", agent_id.c_str());
    return -1;
  }

  return agent_it->second->clear_session_data(session_id);
}

int MMAgentRuntime::clear_dialogue_data(const AgentId& agent_id,
                                          const std::string& session_id,
                                          const std::string& dialogue_id) {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  auto agent_it = agents_.find(agent_id);
  if(agent_it == agents_.end() || !agent_it->second) {
    Logger::instance().warn_f("[Runtime] Agent not found for dialogue cleanup: %s", agent_id.c_str());
    return -1;
  }

  return agent_it->second->clear_dialogue_data(session_id, dialogue_id);
}

// =============================================================================
// Event Handler Management
// =============================================================================

void MMAgentRuntime::register_event_handler(std::shared_ptr<IEventHandler> handler) {
  if(!handler) {
    return;
  }
  std::lock_guard<std::mutex> lock(event_handlers_mutex_);
  event_handlers_.push_back(handler);
}

void MMAgentRuntime::unregister_event_handler(std::shared_ptr<IEventHandler> handler) {
  if(!handler) {
    return;
  }
  std::lock_guard<std::mutex> lock(event_handlers_mutex_);
  event_handlers_.erase(
      std::remove(event_handlers_.begin(), event_handlers_.end(), handler),
      event_handlers_.end());
}

void MMAgentRuntime::clear_event_handlers() {
  std::lock_guard<std::mutex> lock(event_handlers_mutex_);
  event_handlers_.clear();
}

size_t MMAgentRuntime::get_event_handler_count() const {
  std::lock_guard<std::mutex> lock(event_handlers_mutex_);
  return event_handlers_.size();
}

void MMAgentRuntime::set_callback_thread_pool_size(size_t size) {
  callback_thread_pool_size_ = size > 0 ? size : 5;
}

size_t MMAgentRuntime::get_callback_thread_pool_size() const {
  return callback_thread_pool_size_;
}

// =============================================================================
// Event Emission
// =============================================================================

void MMAgentRuntime::start_callback_threads() {
  if(callback_threads_running_) {
    return;
  }
  callback_threads_running_ = true;
  size_t num_threads = callback_thread_pool_size_ > 0 ? callback_thread_pool_size_ : 5;
  for(size_t i = 0; i < num_threads; ++i) {
    callback_threads_.emplace_back(&MMAgentRuntime::callback_thread_main, this);
  }
  Logger::instance().info_f("Callback thread pool started with %zu threads", num_threads);
}

void MMAgentRuntime::stop_callback_threads() {
  if(!callback_threads_running_) {
    return;
  }
  callback_threads_running_ = false;
  callback_queue_cv_.notify_all();

  for(auto& thread : callback_threads_) {
    if(thread.joinable()) {
      thread.join();
    }
  }
  callback_threads_.clear();
  Logger::instance().info("Callback thread pool stopped");
}

void MMAgentRuntime::callback_thread_main() {
  while(callback_threads_running_) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(callback_queue_mutex_);
      callback_queue_cv_.wait(lock, [this]() {
        return !callback_queue_.empty() || !callback_threads_running_;
      });
      if(!callback_threads_running_ && callback_queue_.empty()) {
        break;
      }
      if(!callback_queue_.empty()) {
        task = std::move(callback_queue_.front());
        callback_queue_.pop();
      }
    }
    if(task) {
      try {
        task();
      } catch(const std::exception& e) {
        Logger::instance().error_f("Exception in callback thread: %s", e.what());
      } catch(...) {
        Logger::instance().error("Unknown exception in callback thread");
      }
    }
  }
}

void MMAgentRuntime::emit_agent_event(const AgentEvent& event) {
  if(role_ != RuntimeRole::MASTER) {
    return;
  }
  std::vector<std::shared_ptr<IEventHandler>> handlers_copy;
  {
    std::lock_guard<std::mutex> lock(event_handlers_mutex_);
    handlers_copy = event_handlers_;
  }
  if(handlers_copy.empty()) {
    return;
  }
  // Dispatch to callback thread pool
  {
    std::lock_guard<std::mutex> lock(callback_queue_mutex_);
    callback_queue_.emplace([handlers_copy = std::move(handlers_copy), event]() {
      for(const auto& handler : handlers_copy) {
        if(handler) {
          handler->handle_agent_event(event);
        }
      }
    });
  }
  callback_queue_cv_.notify_one();
}

void MMAgentRuntime::emit_session_event(const SessionEvent& event) {
  if(role_ != RuntimeRole::MASTER) {
    return;
  }
  std::vector<std::shared_ptr<IEventHandler>> handlers_copy;
  {
    std::lock_guard<std::mutex> lock(event_handlers_mutex_);
    handlers_copy = event_handlers_;
  }
  if(handlers_copy.empty()) {
    return;
  }
  // Dispatch to callback thread pool
  {
    std::lock_guard<std::mutex> lock(callback_queue_mutex_);
    callback_queue_.emplace([handlers_copy = std::move(handlers_copy), event]() {
      for(const auto& handler : handlers_copy) {
        if(handler) {
          handler->handle_session_event(event);
        }
      }
    });
  }
  callback_queue_cv_.notify_one();
}

void MMAgentRuntime::emit_dialogue_event(const DialogueEvent& event) {
  if(role_ != RuntimeRole::MASTER) {
    return;
  }
  std::vector<std::shared_ptr<IEventHandler>> handlers_copy;
  {
    std::lock_guard<std::mutex> lock(event_handlers_mutex_);
    handlers_copy = event_handlers_;
  }
  if(handlers_copy.empty()) {
    return;
  }
  // Dispatch to callback thread pool
  {
    std::lock_guard<std::mutex> lock(callback_queue_mutex_);
    callback_queue_.emplace([handlers_copy = std::move(handlers_copy), event]() {
      for(const auto& handler : handlers_copy) {
        if(handler) {
          handler->handle_dialogue_event(event);
        }
      }
    });
  }
  callback_queue_cv_.notify_one();
}

void MMAgentRuntime::emit_task_event(const TaskEvent& event) {
  if(role_ != RuntimeRole::MASTER) {
    return;
  }
  std::vector<std::shared_ptr<IEventHandler>> handlers_copy;
  {
    std::lock_guard<std::mutex> lock(event_handlers_mutex_);
    handlers_copy = event_handlers_;
  }
  if(handlers_copy.empty()) {
    return;
  }
  // Dispatch to callback thread pool
  {
    std::lock_guard<std::mutex> lock(callback_queue_mutex_);
    callback_queue_.emplace([handlers_copy = std::move(handlers_copy), event]() {
      for(const auto& handler : handlers_copy) {
        if(handler) {
          handler->handle_task_event(event);
        }
      }
    });
  }
  callback_queue_cv_.notify_one();
}

std::vector<WorkflowInfo> MMAgentRuntime::build_workflow_infos(const AgentPtr& agent) {
  std::vector<WorkflowInfo> result;
  if(!agent) {
    return result;
  }
  for(const auto& workflow : agent->get_workflows()) {
    if(!workflow) {
      continue;
    }
    WorkflowInfo wf_info;
    wf_info.id          = workflow->get_id();
    wf_info.name        = workflow->get_name();
    wf_info.version     = workflow->get_config().version;
    wf_info.description = workflow->get_config().description;

    // Build node infos
    for(const auto& node : workflow->get_nodes()) {
      if(!node) {
        continue;
      }
      NodeInfo node_info;
      node_info.id       = node->get_id();
      node_info.name     = node->get_name();
      node_info.type     = node->get_type();
      node_info.priority = node->get_priority();
      // Note: input_params and output_params would need to be extracted from node config
      // This is a placeholder for future expansion
      wf_info.nodes.push_back(node_info);
    }

    // Build edge infos
    for(const auto& edge : workflow->get_edges()) {
      if(!edge) {
        continue;
      }
      EdgeInfo edge_info;
      edge_info.id             = edge->get_id();
      edge_info.source_node_id = edge->get_source();
      edge_info.target_node_id = edge->get_target();
      edge_info.condition      = edge->get_condition_expression();
      wf_info.edges.push_back(edge_info);
    }

    result.push_back(wf_info);
  }
  return result;
}

void MMAgentRuntime::initialize_agent_workflow_pools(const AgentPtr& agent) {
  if(!agent) {
    return;
  }
  for(const auto& task : collect_pool_template_tasks(agent)) {
    if(!task) {
      continue;
    }
    const std::string workflow_id =
        !task->get_workflow_id().empty() ? task->get_workflow_id() : task->get_workflow_file();
    if(workflow_id.empty()) {
      continue;
    }
    ensure_workflow_pool(agent->get_id(), task->get_task_id(), workflow_id, task->get_workflow_file(),
                         task->get_workflow_pool_size());
  }
}

bool MMAgentRuntime::ensure_workflow_pool(const std::string& agent_id,
                                          const std::string& task_owner_id,
                                          const std::string& workflow_id,
                                          const std::string& workflow_file,
                                          int workflow_pool_size) {
  (void)agent_id;
  if(!shared_state_ || !deployer_ || task_owner_id.empty() || workflow_id.empty()) {
    return false;
  }
  if(local_worker_) {
    local_worker_->add_supported_workflow(workflow_id);
  }
  const int desired_size = std::max(1, workflow_pool_size);
  for(int index = 0; index < desired_size; ++index) {
    const std::string workflow_instance_id =
        task_owner_id + ":workflow_instance:" + std::to_string(index);
    WorkflowInstanceInfo existing;
    if(shared_state_->get_workflow_instance(workflow_instance_id, existing)) {
      continue;
    }
    WorkflowInstanceInfo info;
    info.workflow_instance_id = workflow_instance_id;
    info.workflow_id = workflow_id;
    info.workflow_file = workflow_file;
    info.agent_object_id = task_owner_id;
    info.agent_instance_id = task_owner_id;
    if(!deploy_workflow_instance(info, config_.local_worker_id)) {
      Logger::instance().warn_f("Failed to deploy pooled workflow instance %s for workflow %s",
                                workflow_instance_id.c_str(), workflow_id.c_str());
      return false;
    }
  }
  return true;
}

void MMAgentRuntime::worker_thread_main() {
  // Thread-safe worker ID assignment
  static std::atomic<int> next_worker_id{0};
  int worker_id = next_worker_id.fetch_add(1) % num_worker_threads_;

  while(is_running_) {
    std::unique_lock<std::mutex> wake_lock(g_worker_mutex);

    // Wait for tasks or shutdown signal
    g_worker_cv.wait_for(wake_lock, std::chrono::milliseconds(100), [this]() {
      if(!is_running_) {
        return true;
      }
      std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
      return !node_task_queue_.empty();
    });
    wake_lock.unlock();

    if(!is_running_) {
      break;
    }

    // Process pending tasks
    while(true) {
      QueuedNodeTask task;
      {
        std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
        if(node_task_queue_.empty()) {
          break;
        }
        task = node_task_queue_.front();
        node_task_queue_.pop();
      }

      // Update worker load
      update_worker_load(worker_id, 1);

      // Process node without holding task_queue_mutex_ so downstream scheduling
      // can enqueue follow-up nodes from the same workflow.
      process_node(task.agent, task.workflow, task.node, task.execution_id);

      // Update worker load
      update_worker_load(worker_id, -1);
    }
  }

  Logger::instance().info_f("Worker thread %d exiting", worker_id);
}

// ============================================================================
// AsyncCall SDK Support Methods
// ============================================================================

bool MMAgentRuntime::send_async_message(const framework::MMAgentMsgQue::Message& message) {
  // Use agent's message routing for cross-task communication
  // Find the agent by session_id
  auto session_id_it = message.metadata.find("session_id");
  if (session_id_it == message.metadata.end()) {
    Logger::instance().warn_f("[send_async_message] Missing session_id in message metadata");
    return false;
  }

  // Find the agent that owns this session
  AgentPtr agent;
  {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    for (const auto& [id, ag] : agents_) {
      if (ag && ag->get_session(session_id_it->second)) {
        agent = ag;
        break;
      }
    }
  }

  if (!agent) {
    Logger::instance().warn_f("[send_async_message] Agent not found for session %s",
                              session_id_it->second.c_str());
    return false;
  }

  // Route the message through the agent's message handling
  agent->route_request_message(message);
  return true;
}

bool MMAgentRuntime::enqueue_async_callback(const framework::MMAgentMsgQue::Message& message) {
  // Short-circuit routing: directly enqueue to the target node's workflow execution

  auto execution_id_it = message.metadata.find("execution_id");
  auto node_id_it = message.metadata.find("node_id");

  if (execution_id_it == message.metadata.end() || node_id_it == message.metadata.end()) {
    Logger::instance().warn_f("[enqueue_async_callback] Missing execution_id or node_id in message metadata");
    return false;
  }

  const std::string& execution_id = execution_id_it->second;
  const std::string& node_id = node_id_it->second;

  Logger::instance().info_f("[enqueue_async_callback] Received callback for execution=%s, node=%s",
                            execution_id.c_str(), node_id.c_str());

  std::lock_guard<std::mutex> lock(workflow_state_mutex_);

  // Find workflow execution state
  auto state_it = workflow_states_.find(execution_id);
  if (state_it == workflow_states_.end()) {
    Logger::instance().warn_f("[enqueue_async_callback] Workflow state not found for execution_id=%s",
                              execution_id.c_str());
    return false;
  }

  auto& state = state_it->second;

  // Find the cloned workflow
  WorkflowPtr workflow;
  {
    std::lock_guard<std::mutex> wf_lock(cloned_workflows_mutex_);
    auto wf_it = cloned_workflows_.find(execution_id);
    if (wf_it != cloned_workflows_.end()) {
      workflow = wf_it->second;
    }
  }

  if (!workflow) {
    Logger::instance().warn_f("[enqueue_async_callback] Cloned workflow not found for execution_id=%s",
                              execution_id.c_str());
    return false;
  }

  // Find the target node
  auto node = workflow->get_node(node_id);
  if (!node) {
    Logger::instance().warn_f("[enqueue_async_callback] Node not found: %s", node_id.c_str());
    return false;
  }

  // Store callback data in DataBus for node to read (regardless of current state)
  // This handles the race condition where callback arrives before node transitions to ASYNC_PENDING
  auto& databus = get_databus(state.agent_id);
  std::string callback_key = "/async_callback/" + node_id;
  std::string async_call_key = message.metadata.count("async_call_key") ? message.metadata.at("async_call_key") : "";

  // Store in databus
  databus.put(callback_key + "/msg", DataVariant(message.content));
  databus.put(callback_key + "/key", DataVariant(async_call_key));

  Logger::instance().info_f("[enqueue_async_callback] Stored callback data at %s, key=%s, msg_len=%zu, node_state=%d",
                            callback_key.c_str(), async_call_key.c_str(), message.content.size(),
                            static_cast<int>(node->get_state()));

  // Check if node is in ASYNC_PENDING state - if so, schedule re-execution
  // If not, the callback data is buffered in DataBus and will be processed when node becomes ASYNC_PENDING
  if (node->get_state() != NodeState::ASYNC_PENDING) {
    Logger::instance().info_f("[enqueue_async_callback] Node %s not yet ASYNC_PENDING (state=%d), callback buffered in DataBus",
                              node_id.c_str(), static_cast<int>(node->get_state()));
    // Increment a pending callback count so we know there's buffered data
    state.pending_callbacks[node_id]++;
    return true;  // Callback buffered, will be processed when node is ready
  }

  Logger::instance().info_f("[enqueue_async_callback] Node %s is ASYNC_PENDING, scheduling re-execution", node_id.c_str());

  // Reset node state to READY so it can be executed again
  node->set_state(NodeState::READY);

  // Get agent for task execution
  auto agent = get_agent(state.agent_id);
  if (!agent) {
    Logger::instance().warn_f("[enqueue_async_callback] Agent not found: %s", state.agent_id.c_str());
    return false;
  }

  // Enqueue the node for re-execution
  {
    std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
    state.queued_nodes.insert(node_id);
    state.running_nodes.erase(node_id);
    node_task_queue_.push({agent, workflow, node, execution_id});
  }

  g_worker_cv.notify_all();

  Logger::instance().info_f("[enqueue_async_callback] Node %s enqueued for re-execution", node_id.c_str());

  return true;
}

}  // namespace runtime
}  // namespace agentflow
