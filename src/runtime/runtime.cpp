#include "runtime/agent_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <random>
#include <thread>

#include "agent/session.h"
#include "agent/task.h"
#include "runtime/trace/trace_context.h"
#include "runtime/trace/trace_manager.h"
#include "utils/logger.h"

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
                           config_.worker_queue_size});
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
                                                            config.worker_queue_size});
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
  return true;
}

bool MMAgentRuntime::destroy_workflow_instance(const std::string& workflow_instance_id) {
  if(!deployer_ || !shared_state_) {
    return false;
  }
  bool removed_mapping = deployer_->remove_workflow_instance(workflow_instance_id);
  bool removed_state = shared_state_->remove_workflow_instance(workflow_instance_id);
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

bool MMAgentRuntime::register_agent(AgentPtr agent) {
  if(!agent) {
    return false;
  }
  if(!is_running_) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agents_[agent->get_id()] = agent;
    databuses_[agent->get_id()] = std::unique_ptr<MMDataBus>(new MMDataBus(1024 * 1024));
    agent_heartbeats_[agent->get_id()] = now_ms();
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
                                    const DataMap& inputs) {
  if(draining_) {
    Logger::instance().warn("Rejecting new workflow — runtime is draining");
    return false;
  }

  auto agent    = get_agent(agent_id);
  auto workflow = agent ? agent->get_workflow(workflow_id) : nullptr;

  if(!agent || !workflow) {
    Logger::instance().error_f("Cannot execute workflow: agent=%s, workflow=%s", agent_id.c_str(),
                               workflow_id.c_str());
    return false;
  }

  // Set input data to agent
  for(const auto& pair : inputs) {
    agent->publish_data(pair.first, pair.second);
  }

  // Create execution state for this workflow
  std::string execution_id =
      agent_id + ":" + workflow_id + ":"
      + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

  WorkflowExecutionState state;
  state.workflow_id     = workflow_id;
  state.agent_id        = agent_id;
  state.total_nodes     = static_cast<int>(workflow->get_nodes().size());
  state.completed_count = 0;
  state.in_progress     = true;

  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    workflow_states_[execution_id] = state;
  }

  Logger::instance().info_f("Starting workflow execution: %s (execution_id: %s)",
                            workflow->get_name().c_str(), execution_id.c_str());

  // Trace: open a WORKFLOW span (no-op if no active trace on this thread)
  {
    using namespace trace;
    ScopedSpan wf_span(SpanKind::WORKFLOW, workflow_id,
                       {{"agent_id", agent_id},
                        {"workflow_name", workflow->get_name()},
                        {"execution_id", execution_id}});
    (void)wf_span;  // span closes immediately; node spans will be children
  }

  // Initialize all nodes to READY state
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
    for(const auto& key : node->get_input_params()) {
      DataVariant val;
      if(bus.get(key, val)) {
        hydrated[key] = val;
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

  // Trace: open a NODE span (RAII — records to TraceManager on scope exit)
  trace::ScopedSpan node_span(
      trace::SpanKind::NODE,
      node->get_name(),
      {{"agent_id",    agent->get_id()},
       {"workflow_id", workflow ? workflow->get_id() : ""},
       {"node_id",     node->get_id()},
       {"node_name",   node->get_name()}});

  // Execute node
  auto start_time       = std::chrono::steady_clock::now();
  bool execution_result = node->execute();
  auto end_time         = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

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

  // Publish node outputs for downstream consumers (bridge/UI/orchestration scripts).
  const DataMap& node_outputs = node->get_outputs();
  if(!node_outputs.empty()) {
    auto& bus = get_databus(agent->get_id());
    for(const auto& out : node_outputs) {
      agent->publish_data(out.first, out.second);
      bus.put(out.first, out.second);
      if(workflow) {
        std::string scoped_key = workflow->get_id() + "." + node->get_id() + "." + out.first;
        agent->publish_data(scoped_key, out.second);
        bus.put(scoped_key, out.second);
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

  // Find the agent and workflow
  auto agent = get_agent(state.agent_id);
  if(!agent) {
    return;
  }

  auto workflow = agent->get_workflow(state.workflow_id);
  if(!workflow) {
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
    } else if(success) {
      auto agent = get_agent(state.agent_id);
      if(agent) {
        auto workflow = agent->get_workflow(state.workflow_id);
        if(workflow) {
          workflow->mark_node_executed(node_id, true);
          auto next_nodes = workflow->get_executable_nodes();
          if(!next_nodes.empty()) {
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
  }

  if(invoke_complete) {
    if(complete_ok) {
      Logger::instance().info_f("MMWorkflow execution completed successfully: %s",
                                complete_workflow.c_str());
    } else {
      Logger::instance().error_f("MMWorkflow execution failed: %s", complete_workflow.c_str());
    }
    on_workflow_complete(complete_agent, complete_workflow, complete_ok);
  }
}

void MMAgentRuntime::on_node_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                                    const NodeId& node_id, bool success) {
  Logger::instance().info_f("MMNode complete event: agent=%s, workflow=%s, node=%s, success=%d",
                            agent_id.c_str(), workflow_id.c_str(), node_id.c_str(),
                            success ? 1 : 0);
}

void MMAgentRuntime::on_workflow_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                                        bool success) {
  Logger::instance().info_f("MMWorkflow complete event: agent=%s, workflow=%s, success=%d",
                            agent_id.c_str(), workflow_id.c_str(), success ? 1 : 0);

  // Clean up execution states matching this agent+workflow
  std::lock_guard<std::mutex> lock(workflow_state_mutex_);
  for(auto it = workflow_states_.begin(); it != workflow_states_.end();) {
    if(it->second.agent_id == agent_id && it->second.workflow_id == workflow_id) {
      it = workflow_states_.erase(it);
    } else {
      ++it;
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
  std::lock_guard<std::mutex> lock(agents_mutex_);

  for(const auto& pair : agents_) {
    check_agent_health(pair.first);
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
          ok = true;
        }
      } else {
        agents_[agent_id] = request.agent;
        databuses_[agent_id] = std::unique_ptr<MMDataBus>(new MMDataBus(1024 * 1024));
        agent_heartbeats_[agent_id] = now_ms();
        ok = true;
      }
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
        }
        if(!task->get_config().session_id.empty()) {
          inputs["session_id"] = task->get_config().session_id;
        }
        Logger::instance().info_f(
            "[TASK_POLLER] Dispatching task=%s workflow=%s agent=%s",
            task->get_config().task_id.c_str(), workflow_id.c_str(),
            agent->get_id().c_str());
        const bool scheduled = execute_workflow(agent->get_id(), workflow_id, inputs);
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
        response.metadata["message_type"] = "data_exchange";
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

void MMAgentRuntime::worker_thread_main() {
  // Thread-safe worker ID assignment
  static std::atomic<int> next_worker_id{0};
  int worker_id = next_worker_id.fetch_add(1) % num_worker_threads_;

  while(is_running_) {
    std::unique_lock<std::mutex> lock(g_worker_mutex);

    // Wait for tasks or shutdown signal
    g_worker_cv.wait_for(lock, std::chrono::milliseconds(100),
                         [this]() { return !node_task_queue_.empty() || !is_running_; });

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
