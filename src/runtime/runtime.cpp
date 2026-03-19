#include "runtime/agent_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <random>
#include <thread>

#include "agent/session.h"
#include "agent/task.h"
#include "agent/context.h"
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
  scheduler_->enqueue_task(request);
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
    databuses_[agent->get_id()] = std::unique_ptr<MMDataBus>(new MMDataBus(1024 * 1024));
    agent_heartbeats_[agent->get_id()] = now_ms();
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
      agents_.erase(it);
      databuses_.erase(agent_id);
      agent_heartbeats_.erase(agent_id);
      // Remove DataBus from local worker
      if(local_worker_) {
        local_worker_->remove_databus(agent_id);
      }
      Logger::instance().info_f("MMAgent unregistered: %s", agent_id.c_str());
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
                                    const std::string& task_type, const std::string& skill_task_id) {
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

  // Set input data to agent with dialogue_id prefix for concurrency isolation
  // When multiple dialogues execute in parallel, each dialogue's inputs are isolated
  std::string input_prefix = dialogue_id.empty() ? "" : dialogue_id + "/";
  for(const auto& pair : inputs) {
    agent->publish_data(input_prefix + pair.first, pair.second);
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
  state.task_type       = task_type;    // Store for correct task_type in completion message
  state.skill_task_id   = skill_task_id;  // Store actual skill task id for completion tracking
  state.total_nodes     = static_cast<int>(workflow->get_nodes().size());
  state.completed_count = 0;
  state.in_progress     = true;

  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    workflow_states_[execution_id] = state;
  }

  Logger::instance().info_f("Starting workflow execution: %s (execution_id: %s)",
                            workflow->get_name().c_str(), execution_id.c_str());

  // Flow trace: workflow started
  FLOW_TRACE_WORKFLOW(session_id, dialogue_id, source_task_id, workflow_id,
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
        update_workflow_state(execution_id, node->get_id(), false);
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
    state.queued_nodes.erase(node->get_id());
    state.running_nodes.insert(node->get_id());
  }

  // [DEBUG] Log node processing start
  std::thread::id thread_id = std::this_thread::get_id();

  Logger::instance().info_f(
      "[PROCESS_NODE_START] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
      "State=%d",
      std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
      workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
      node->get_name().c_str(), static_cast<int>(node->get_state()));

  // Hydrate node inputs from DataBus using declared input_params keys.
  // This enables genuine DataBus-driven data flow between nodes/workflows.
  {
    auto& bus = get_databus(agent->get_id());
    DataMap hydrated;
    Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Node=%s has %zu input_params",
                              node->get_id().c_str(), node->get_input_params().size());

    // Get dialogue_id from workflow state for multi-dialogue support
    // This avoids the concurrency issue with agent databus
    std::string workflow_dialogue_id;
    {
      std::lock_guard<std::mutex> lock(workflow_state_mutex_);
      auto state_it = workflow_states_.find(execution_id);
      if(state_it != workflow_states_.end() && !state_it->second.dialogue_id.empty()) {
        workflow_dialogue_id = state_it->second.dialogue_id;
        // Add dialogue_id to hydrated inputs if node needs it
        for(const auto& key : node->get_input_params()) {
          if(key == "dialogue_id") {
            hydrated["dialogue_id"] = workflow_dialogue_id;
            Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Got input 'dialogue_id'='%s' from workflow state",
                                     workflow_dialogue_id.c_str());
            break;
          }
        }
      }
    }

    // Build input prefix for dialogue isolation
    std::string input_prefix = workflow_dialogue_id.empty() ? "" : workflow_dialogue_id + "/";

    for(const auto& key : node->get_input_params()) {
      // Skip if already hydrated from workflow state
      if(hydrated.find(key) != hydrated.end()) {
        continue;
      }
      DataVariant val;
      // First try agent's internal databus with dialogue_id prefix (where execute_workflow stores inputs)
      if(agent->get_data(input_prefix + key, val)) {
        hydrated[key] = val;
        if(const auto* text = std::get_if<std::string>(&val)) {
          Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Got input '%s'='%s' from agent databus (prefix: %s)",
                                   key.c_str(), text->c_str(), input_prefix.c_str());
        }
      } else if(agent->get_data(key, val)) {
        // Fallback: try without prefix for backward compatibility
        hydrated[key] = val;
        if(const auto* text = std::get_if<std::string>(&val)) {
          Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Got input '%s'='%s' from agent databus (no prefix)", key.c_str(), text->c_str());
        }
      } else if(bus.get(key, val)) {
        // Then try runtime's databus
        hydrated[key] = val;
        if(const auto* text = std::get_if<std::string>(&val)) {
          Logger::instance().info_f("[PROCESS_NODE_HYDRATE] Got input '%s'='%s' from runtime databus", key.c_str(), text->c_str());
        }
      } else {
        Logger::instance().warn_f("[PROCESS_NODE_HYDRATE] Key '%s' not found in any databus", key.c_str());
      }
    }
    // Merge with any existing inputs (e.g. set via set_inputs() or message payload)
    const DataMap& existing = node->get_inputs();
    for(const auto& kv : existing) {
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

  // Trace: open a NODE span (RAII — records to TraceManager on scope exit)
  trace::ScopedSpan node_span(
      trace::SpanKind::NODE,
      node->get_name(),
      {{"agent_id",    agent->get_id()},
       {"workflow_id", workflow ? workflow->get_id() : ""},
       {"node_id",     node->get_id()},
       {"node_name",   node->get_name()}});

  // Get session_id and dialogue_id for flow trace
  std::string flow_session_id;
  std::string flow_dialogue_id;
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    auto state_it = workflow_states_.find(execution_id);
    if(state_it != workflow_states_.end()) {
      flow_session_id = state_it->second.session_id;
      flow_dialogue_id = state_it->second.dialogue_id;
    }
  }

  // Flow trace: node started
  FLOW_TRACE_NODE(flow_session_id, flow_dialogue_id,
                   workflow ? workflow->get_id() : "", node->get_name(),
                   FlowEventType::NODE_STARTED);

  // Execute node
  auto start_time       = std::chrono::steady_clock::now();
  bool execution_result = node->execute();
  auto end_time         = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  // Flow trace: node completed
  FLOW_TRACE_NODE_WITH_DATA(flow_session_id, flow_dialogue_id,
                              workflow ? workflow->get_id() : "", node->get_name(),
                              FlowEventType::NODE_COMPLETED,
                              "", "", duration * 1000);  // duration in microseconds

  // [DEBUG] Log node processing result
  if(!execution_result) {
    node_span.set_status(trace::SpanStatus::ERROR, "node execute() returned false");
    node_span.add_event("node_failed", {{"duration_ms", std::to_string(duration)}});
    Logger::instance().error_f(
        "[PROCESS_NODE_FAILED] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
        "Duration=%ldms, RetryCount=%d",
        std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
        workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
        node->get_name().c_str(), duration, node->get_retry_count());
  } else {
    node_span.set_status(trace::SpanStatus::OK);
    node_span.add_event("node_success", {{"duration_ms", std::to_string(duration)}});
    Logger::instance().info_f(
        "[PROCESS_NODE_SUCCESS] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
        "Duration=%ldms",
        std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
        workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
        node->get_name().c_str(), duration);
  }

  // =========================================================================
  // Edge-based Output Routing (Scenario 1 & Scenario 2 Fix)
  // =========================================================================
  // Core rules:
  // - Has downstream Node → Edge point-to-point transfer to downstream inputs
  // - No downstream Node → DataBus broadcast (with session isolation format)
  // - Multi-incoming edges (Scenario 2): auto-prefix keys with source node ID
  // This naturally isolates parallel node outputs without node code changes.
  const DataMap& node_outputs = node->get_outputs();
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
  node->cleanup();

  // [DEBUG] Log node cleanup
  Logger::instance().debug_f("[PROCESS_NODE_CLEANUP] ThreadId=%zu, AgentId=%s, NodeId=%s",
                             std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
                             node->get_id().c_str());

  // Find execution state and update it
  update_workflow_state(execution_id, node->get_id(), node_success);
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
                                         bool success) {
  bool        invoke_complete = false;
  bool        complete_ok     = false;
  std::string complete_agent;
  std::string complete_workflow;
  std::string complete_source_task_id;
  std::string complete_session_id;
  std::string complete_dialogue_id;
  std::string complete_task_type;
  std::string complete_skill_task_id;

  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);

    auto it = workflow_states_.find(execution_id);
    if(it == workflow_states_.end()) {
      return;
    }

    auto& state = it->second;
    state.queued_nodes.erase(node_id);
    state.running_nodes.erase(node_id);

    if(success) {
      state.completed_nodes[node_id] = true;
      state.completed_count++;
    } else {
      state.failed_nodes[node_id] = true;
    }

    Logger::instance().info_f("MMWorkflow progress: %s - %d/%d completed, %zu failed",
                              execution_id.c_str(), state.completed_count, state.total_nodes,
                              state.failed_nodes.size());

    if(state.completed_count + static_cast<int>(state.failed_nodes.size()) >= state.total_nodes) {
      state.in_progress   = false;
      invoke_complete     = true;
      complete_ok         = state.failed_nodes.empty();
      complete_agent      = state.agent_id;
      complete_workflow   = state.workflow_id;
      complete_source_task_id = state.source_task_id;
      complete_session_id     = state.session_id;
      complete_dialogue_id    = state.dialogue_id;
      complete_task_type      = state.task_type;
      complete_skill_task_id  = state.skill_task_id;
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
    FLOW_TRACE_WORKFLOW(complete_session_id, complete_dialogue_id,
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
            // Use skill_task_id if available, otherwise fall back to source_task_id
            response.metadata["skill_name"] = complete_skill_task_id.empty() ? complete_source_task_id : complete_skill_task_id;
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

  // Clean up the execution state
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    workflow_states_.erase(execution_id);
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
          agents_.erase(it);
          databuses_.erase(agent_id);
          agent_heartbeats_.erase(agent_id);
          // Remove DataBus from local worker
          if(local_worker_) {
            local_worker_->remove_databus(agent_id);
          }
          ok = true;
        }
      } else {
        agents_[agent_id] = request.agent;
        databuses_[agent_id] = std::unique_ptr<MMDataBus>(new MMDataBus(1024 * 1024));
        agent_heartbeats_[agent_id] = now_ms();
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
          Logger::instance().info_f("[TASK_POLLER] Set inputs[input]='%s' from message_content", msg_content.c_str());
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

        // Get skill task id for completion tracking (the actual task id of this skill)
        std::string skill_task_id = task->get_config().task_id;

        const bool scheduled = execute_workflow(agent->get_id(), workflow_id, inputs,
                                                task_source_id, task_session_id, task_dialogue_id,
                                                task_type, skill_task_id);
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
    // If databus doesn't exist, create one (this shouldn't happen for registered agents)
    // But for safety, we create a new one
    databuses_[agent_id] = std::unique_ptr<MMDataBus>(new MMDataBus(1024 * 1024));
    return *databuses_[agent_id];
  }

  return *it->second;
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

}  // namespace runtime
}  // namespace agentflow
