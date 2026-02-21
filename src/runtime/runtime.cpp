#include "runtime/agent_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <random>
#include <thread>

#include "utils/logger.h"

namespace agentflow {
namespace runtime {

namespace {
std::condition_variable g_worker_cv;
std::mutex              g_worker_mutex;
bool                    g_shutdown = false;
}  // namespace

AgentRuntime::AgentRuntime() {
  config_.worker_threads = num_worker_threads_;
  role_ = config_.role;
  shared_state_ = std::make_shared<InMemoryRuntimeSharedState>();
  deployer_ = std::unique_ptr<master::Deployer>(new master::Deployer());
  scheduler_ = std::unique_ptr<master::Scheduler>(new master::Scheduler());
  local_worker_ = std::make_shared<WorkerEngine>(WorkerEngine::Config{config_.local_worker_id, true});
  local_worker_->set_status(WorkerEngine::WorkerStatus::RUNNING);
}

AgentRuntime::AgentRuntime(const Config& config) : config_(config), role_(config.role) {
  num_worker_threads_ = config.worker_threads > 0 ? config.worker_threads : num_worker_threads_;
  shared_state_ = std::make_shared<InMemoryRuntimeSharedState>();
  deployer_ = std::unique_ptr<master::Deployer>(new master::Deployer());
  scheduler_ = std::unique_ptr<master::Scheduler>(new master::Scheduler());
  local_worker_ = std::make_shared<WorkerEngine>(WorkerEngine::Config{config.local_worker_id, true});
  local_worker_->set_status(WorkerEngine::WorkerStatus::RUNNING);
}

AgentRuntime::~AgentRuntime() {
  stop();
}

void AgentRuntime::set_role(RuntimeRole role) {
  role_ = role;
}

bool AgentRuntime::register_worker_engine(const std::shared_ptr<WorkerEngine>& worker) {
  if(!deployer_) {
    return false;
  }
  return deployer_->register_worker(worker);
}

bool AgentRuntime::unregister_worker_engine(const std::string& worker_id) {
  if(!deployer_) {
    return false;
  }
  return deployer_->unregister_worker(worker_id);
}

bool AgentRuntime::heartbeat_worker(const std::string& worker_id) {
  if(!deployer_) {
    return false;
  }
  return deployer_->update_worker_heartbeat(worker_id);
}

bool AgentRuntime::update_worker_engine_status(const std::string& worker_id,
                                               WorkerEngine::WorkerStatus status) {
  if(!deployer_) {
    return false;
  }
  return deployer_->update_worker_status(worker_id, status);
}

std::vector<master::WorkerSnapshot> AgentRuntime::list_worker_engines() const {
  if(!deployer_) {
    return {};
  }
  return deployer_->list_worker_snapshots();
}

bool AgentRuntime::deploy_workflow_instance(const WorkflowInstanceInfo& info,
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

bool AgentRuntime::destroy_workflow_instance(const std::string& workflow_instance_id) {
  if(!deployer_ || !shared_state_) {
    return false;
  }
  bool removed_mapping = deployer_->remove_workflow_instance(workflow_instance_id);
  bool removed_state = shared_state_->remove_workflow_instance(workflow_instance_id);
  return removed_mapping || removed_state;
}

master::TaskScheduleResult AgentRuntime::schedule_task(const master::TaskScheduleRequest& request) {
  if(!scheduler_ || !shared_state_) {
    return {};
  }
  auto result = scheduler_->schedule_task(request, shared_state_->list_workflow_instances());
  if(result.scheduled) {
    shared_state_->increment_workflow_ref(result.workflow_instance_id);
  }
  return result;
}

bool AgentRuntime::register_agent(AgentPtr agent) {
  if(!agent) {
    return false;
  }

  std::lock_guard<std::mutex> lock(agents_mutex_);

  // Register agent
  agents_[agent->get_id()] = agent;

  // Create databus for agent
  databuses_[agent->get_id()] =
      std::unique_ptr<DataBus>(new DataBus(1024 * 1024)  // 1MB databus
      );

  Logger::instance().info_f("Agent registered: %s (id: %s)", agent->get_name().c_str(),
                            agent->get_id().c_str());
  return true;
}

bool AgentRuntime::unregister_agent(const AgentId& agent_id) {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  auto it = agents_.find(agent_id);
  if(it != agents_.end()) {
    agents_.erase(it);
    databuses_.erase(agent_id);
    Logger::instance().info_f("Agent unregistered: %s", agent_id.c_str());
    return true;
  }
  return false;
}

AgentPtr AgentRuntime::get_agent(const AgentId& agent_id) {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  auto it = agents_.find(agent_id);
  return it != agents_.end() ? it->second : nullptr;
}

std::vector<AgentPtr> AgentRuntime::list_agents() {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  std::vector<AgentPtr> result;
  for(const auto& pair : agents_) {
    result.push_back(pair.second);
  }
  return result;
}

bool AgentRuntime::load_workflow(const AgentId& agent_id, WorkflowPtr workflow) {
  auto agent = get_agent(agent_id);
  if(!agent || !workflow) {
    return false;
  }

  agent->add_workflow(workflow);
  if(!workflow->validate()) {
    agent->remove_workflow(workflow->get_id());
    Logger::instance().error_f("Workflow validation failed: %s", workflow->get_name().c_str());
    return false;
  }

  Logger::instance().info_f("Workflow loaded into agent %s: %s (id: %s, %zu nodes)",
                            agent_id.c_str(), workflow->get_name().c_str(),
                            workflow->get_id().c_str(), workflow->get_nodes().size());
  return true;
}

bool AgentRuntime::unload_workflow(const AgentId& agent_id, const WorkflowId& workflow_id) {
  auto agent = get_agent(agent_id);
  if(!agent) {
    return false;
  }

  agent->remove_workflow(workflow_id);
  Logger::instance().info_f("Workflow unloaded from agent %s: %s", agent_id.c_str(),
                            workflow_id.c_str());
  return true;
}

bool AgentRuntime::execute_workflow(const AgentId& agent_id, const WorkflowId& workflow_id,
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

  // Initialize all nodes to READY state
  for(const auto& node : workflow->get_nodes()) {
    if(node->get_state() != NodeState::WAITING && node->get_state() != NodeState::READY) {
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

void AgentRuntime::process_node(AgentPtr agent, WorkflowPtr workflow, NodePtr node) {
  if(!agent || !node) {
    Logger::instance().error_f("[PROCESS_NODE_ERROR] Invalid agent or node pointer");
    return;
  }

  // [DEBUG] Log node processing start
  std::thread::id thread_id = std::this_thread::get_id();

  Logger::instance().info_f(
      "[PROCESS_NODE_START] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
      "State=%d",
      std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
      workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
      node->get_name().c_str(), static_cast<int>(node->get_state()));

  // Execute node
  auto start_time       = std::chrono::steady_clock::now();
  bool execution_result = node->execute();
  auto end_time         = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  // [DEBUG] Log node processing result
  if(!execution_result) {
    Logger::instance().error_f(
        "[PROCESS_NODE_FAILED] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
        "Duration=%ldms, RetryCount=%d",
        std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
        workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
        node->get_name().c_str(), duration, node->get_retry_count());
  } else {
    Logger::instance().info_f(
        "[PROCESS_NODE_SUCCESS] ThreadId=%zu, AgentId=%s, WorkflowId=%s, NodeId=%s, NodeName=%s, "
        "Duration=%ldms",
        std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
        workflow ? workflow->get_id().c_str() : "unknown", node->get_id().c_str(),
        node->get_name().c_str(), duration);
  }

  node->cleanup();

  // [DEBUG] Log node cleanup
  Logger::instance().debug_f("[PROCESS_NODE_CLEANUP] ThreadId=%zu, AgentId=%s, NodeId=%s",
                             std::hash<std::thread::id>{}(thread_id), agent->get_id().c_str(),
                             node->get_id().c_str());

  // Find execution state and update it
  {
    std::lock_guard<std::mutex> lock(workflow_state_mutex_);
    for(auto& pair : workflow_states_) {
      if(pair.second.agent_id == agent->get_id() && pair.second.in_progress
         && (workflow ? pair.second.workflow_id == workflow->get_id() : true)) {
        update_workflow_state(pair.first, node->get_id(), node->get_state() == NodeState::SUCCESS);
        break;
      }
    }
  }
}

void AgentRuntime::schedule_workflow_executable_nodes(const std::string& execution_id) {
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
      if(node && node->get_state() == NodeState::READY) {
        node_task_queue_.push({ agent, node });
      }
    }
  }

  // Notify worker threads
  g_worker_cv.notify_all();

  Logger::instance().info_f("Scheduled %zu executable nodes for execution",
                            executable_nodes.size());
}

void AgentRuntime::update_workflow_state(const std::string& execution_id, const NodeId& node_id,
                                         bool success) {
  std::lock_guard<std::mutex> lock(workflow_state_mutex_);

  auto it = workflow_states_.find(execution_id);
  if(it == workflow_states_.end()) {
    return;
  }

  auto& state = it->second;

  if(success) {
    state.completed_nodes[node_id] = true;
    state.completed_count++;
  } else {
    state.failed_nodes[node_id] = true;
  }

  Logger::instance().info_f("Workflow progress: %s - %d/%d completed, %zu failed",
                            execution_id.c_str(), state.completed_count, state.total_nodes,
                            state.failed_nodes.size());

  // Check if workflow is complete
  if(state.completed_count + static_cast<int>(state.failed_nodes.size()) >= state.total_nodes) {
    state.in_progress = false;

    if(state.failed_nodes.empty()) {
      Logger::instance().info_f("Workflow execution completed successfully: %s",
                                state.workflow_id.c_str());
      on_workflow_complete(state.agent_id, state.workflow_id, true);
    } else {
      Logger::instance().error_f("Workflow execution failed: %s (%zu nodes failed)",
                                 state.workflow_id.c_str(), state.failed_nodes.size());
      on_workflow_complete(state.agent_id, state.workflow_id, false);
    }
  } else if(success) {
    // Schedule next batch of executable nodes
    auto agent = get_agent(state.agent_id);
    if(agent) {
      auto workflow = agent->get_workflow(state.workflow_id);
      if(workflow) {
        // Mark node as executed in workflow DAG
        workflow->mark_node_executed(node_id, true);

        // Get next executable nodes and enqueue them
        auto next_nodes = workflow->get_executable_nodes();
        if(!next_nodes.empty()) {
          std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
          for(const auto& next_id : next_nodes) {
            auto next_node = workflow->get_node(next_id);
            if(next_node && next_node->get_state() == NodeState::READY) {
              node_task_queue_.push({ agent, next_node });
            }
          }
          g_worker_cv.notify_all();
        }
      }
    }
  }
}

void AgentRuntime::on_node_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                                    const NodeId& node_id, bool success) {
  Logger::instance().info_f("Node complete event: agent=%s, workflow=%s, node=%s, success=%d",
                            agent_id.c_str(), workflow_id.c_str(), node_id.c_str(),
                            success ? 1 : 0);

  // Trigger next nodes
  auto agent = get_agent(agent_id);
  if(!agent) {
    return;
  }

  auto workflow = agent->get_workflow(workflow_id);
  if(!workflow) {
    return;
  }

  // Find next executable nodes
  auto executable = workflow->get_executable_nodes();

  if(!executable.empty()) {
    std::lock_guard<std::mutex> lock(task_queue_mutex_);
    for(const auto& node_id : executable) {
      auto node = workflow->get_node(node_id);
      if(node && node->get_state() == NodeState::READY) {
        node_task_queue_.push({ agent, node });
      }
    }
    g_worker_cv.notify_all();
  }
}

void AgentRuntime::on_workflow_complete(const AgentId& agent_id, const WorkflowId& workflow_id,
                                        bool success) {
  Logger::instance().info_f("Workflow complete event: agent=%s, workflow=%s, success=%d",
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

void AgentRuntime::update_worker_load(int worker_id, int load_change) {
  std::lock_guard<std::mutex> lock(worker_load_mutex_);

  if(worker_id >= 0 && worker_id < num_worker_threads_) {
    if(worker_loads_.size() <= static_cast<size_t>(worker_id)) {
      worker_loads_.resize(num_worker_threads_, 0);
    }
    worker_loads_[worker_id] += load_change;
  }
}

int AgentRuntime::get_least_loaded_worker() {
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

void AgentRuntime::monitor_agents() {
  // Agent Level Master: monitor agent health
  std::lock_guard<std::mutex> lock(agents_mutex_);

  for(const auto& pair : agents_) {
    check_agent_health(pair.first);
  }
}

void AgentRuntime::check_agent_health(const AgentId& agent_id) {
  // Check agent health (simplified implementation)
  auto agent = get_agent(agent_id);
  if(agent) {
    auto state = agent->get_state();
    // In production, check agent health metrics
    // For now, just log the state
    Logger::instance().info_f("Agent health check: %s (state: %d)", agent_id.c_str(),
                              static_cast<int>(state));
  }
}

int AgentRuntime::get_active_agents() const {
  std::lock_guard<std::mutex> lock(agents_mutex_);
  return static_cast<int>(agents_.size());
}

int AgentRuntime::get_active_workflows() const {
  std::lock_guard<std::mutex> lock(agents_mutex_);
  int                         count = 0;
  for(const auto& pair : agents_) {
    count += static_cast<int>(pair.second->get_workflows().size());
  }
  return count;
}

int AgentRuntime::get_running_nodes() const {
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

bool AgentRuntime::start() {
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
    worker_threads_.emplace_back(&AgentRuntime::worker_thread_main, this);
  }

  if(local_worker_) {
    local_worker_->set_status(WorkerEngine::WorkerStatus::RUNNING);
  }
  if(deployer_ && local_worker_ && (role_ == RuntimeRole::MASTER || role_ == RuntimeRole::WORKER)) {
    deployer_->register_worker(local_worker_);
  }

  Logger::instance().info_f("AgentRuntime started with %d worker threads", num_worker_threads_);
  return true;
}

bool AgentRuntime::stop() {
  if(!is_running_) {
    return false;
  }

  // Phase 1: Enter draining mode — stop accepting new tasks
  draining_ = true;
  Logger::instance().info("AgentRuntime entering drain mode...");

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
    Logger::instance().warn_f("AgentRuntime drain timeout after %dms, forcing shutdown", waited_ms);
  }

  // Phase 3: Shutdown workers
  is_running_ = false;
  g_shutdown  = true;

  // Notify all worker threads
  g_worker_cv.notify_all();

  // Wait for all worker threads to finish
  for(auto& thread : worker_threads_) {
    if(thread.joinable()) {
      thread.join();
    }
  }
  worker_threads_.clear();

  draining_ = false;
  if(local_worker_) {
    local_worker_->set_status(WorkerEngine::WorkerStatus::STOPPED);
  }
  Logger::instance().info("AgentRuntime stopped");
  return true;
}

DataBus& AgentRuntime::get_databus(const AgentId& agent_id) {
  std::lock_guard<std::mutex> lock(agents_mutex_);

  auto it = databuses_.find(agent_id);
  if(it == databuses_.end()) {
    // If databus doesn't exist, create one (this shouldn't happen for registered agents)
    // But for safety, we create a new one
    databuses_[agent_id] = std::unique_ptr<DataBus>(new DataBus(1024 * 1024));
    return *databuses_[agent_id];
  }

  return *it->second;
}

void AgentRuntime::worker_thread_main() {
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
    std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
    while(!node_task_queue_.empty()) {
      auto task = node_task_queue_.front();
      node_task_queue_.pop();

      auto agent = task.first;
      auto node  = task.second;

      // Update worker load
      update_worker_load(worker_id, 1);

      // Process node
      process_node(agent, nullptr, node);

      // Update worker load
      update_worker_load(worker_id, -1);
    }
  }

  Logger::instance().info_f("Worker thread %d exiting", worker_id);
}

}  // namespace runtime
}  // namespace agentflow
