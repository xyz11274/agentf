#include "agentflow/runtime/agent_runtime.h"
#include "agentflow/basic/logger.h"
#include <chrono>
#include <thread>
#include <condition_variable>

namespace agentflow {
namespace runtime {

namespace {
std::condition_variable g_worker_cv;
std::mutex g_worker_mutex;
bool g_shutdown = false;
}

AgentRuntime::AgentRuntime() {
}

AgentRuntime::~AgentRuntime() {
    stop();
}

bool AgentRuntime::register_agent(AgentPtr agent) {
    if (!agent) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(agents_mutex_);
    
    // Register agent
    agents_[agent->get_id()] = agent;
    
    // Create databus for agent
    databuses_[agent->get_id()] = std::unique_ptr<HighSpeedDataBus>(
        new HighSpeedDataBus(1024 * 1024)  // 1MB databus
    );
    
    Logger::instance().info_f("Agent registered: %s", agent->get_name().c_str());
    return true;
}

bool AgentRuntime::unregister_agent(const AgentId& agent_id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    
    auto it = agents_.find(agent_id);
    if (it != agents_.end()) {
        agents_.erase(it);
        databuses_.erase(agent_id);
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
    for (const auto& pair : agents_) {
        result.push_back(pair.second);
    }
    return result;
}

bool AgentRuntime::load_workflow(const AgentId& agent_id, WorkflowPtr workflow) {
    auto agent = get_agent(agent_id);
    if (!agent || !workflow) {
        return false;
    }
    
    agent->add_workflow(workflow);
    if (!workflow->validate()) {
        agent->remove_workflow(workflow->get_id());
        return false;
    }
    
    Logger::instance().info_f("Workflow loaded: %s", workflow->get_name().c_str());
    return true;
}

bool AgentRuntime::unload_workflow(const AgentId& agent_id, const WorkflowId& workflow_id) {
    auto agent = get_agent(agent_id);
    if (!agent) {
        return false;
    }
    
    agent->remove_workflow(workflow_id);
    return true;
}

bool AgentRuntime::execute_workflow(const AgentId& agent_id, const WorkflowId& workflow_id, const DataMap& inputs) {
    auto agent = get_agent(agent_id);
    auto workflow = agent ? agent->get_workflow(workflow_id) : nullptr;
    
    if (!workflow) {
        return false;
    }
    
    // Set input data
    for (const auto& pair : inputs) {
        agent->publish_data(pair.first, pair.second);
    }
    
    Logger::instance().info_f("Executing workflow: %s", workflow->get_name().c_str());
    return true;
}

void AgentRuntime::process_node(AgentPtr agent, WorkflowPtr workflow, NodePtr node) {
    if (!agent || !workflow || !node) {
        return;
    }
    
    // Initialize node
    if (!node->initialize()) {
        node->set_state(NodeState::FAILED);
        return;
    }
    
    // Execute node
    if (!node->execute()) {
        node->set_state(NodeState::FAILED);
        return;
    }
    
    // Cleanup node
    node->cleanup();
    
    Logger::instance().info_f("Node executed: %s", node->get_name().c_str());
}

int AgentRuntime::get_active_agents() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    return static_cast<int>(agents_.size());
}

int AgentRuntime::get_active_workflows() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    int count = 0;
    for (const auto& pair : agents_) {
        count += pair.second->get_workflows().size();
    }
    return count;
}

int AgentRuntime::get_running_nodes() const {
    // Simplified implementation
    return 0;
}

bool AgentRuntime::start() {
    if (is_running_) {
        return false;
    }
    
    is_running_ = true;
    g_shutdown = false;
    
    // Start worker threads
    for (int i = 0; i < num_worker_threads_; ++i) {
        worker_threads_.emplace_back(&AgentRuntime::worker_thread_main, this);
    }
    
    Logger::instance().info("AgentRuntime started");
    return true;
}

bool AgentRuntime::stop() {
    if (!is_running_) {
        return false;
    }
    
    is_running_ = false;
    g_shutdown = true;
    
    // Notify all worker threads
    g_worker_cv.notify_all();
    
    // Wait for all worker threads to finish
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
    
    Logger::instance().info("AgentRuntime stopped");
    return true;
}

HighSpeedDataBus& AgentRuntime::get_databus(const AgentId& agent_id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    
    if (databuses_.find(agent_id) == databuses_.end()) {
        databuses_[agent_id] = std::unique_ptr<HighSpeedDataBus>(
            new HighSpeedDataBus(1024 * 1024)
        );
    }
    
    return *databuses_[agent_id];
}

void AgentRuntime::worker_thread_main() {
    while (is_running_) {
        std::unique_lock<std::mutex> lock(g_worker_mutex);
        
        // Wait for tasks or shutdown signal
        g_worker_cv.wait_for(lock, std::chrono::milliseconds(100), 
            [this]() { return !node_task_queue_.empty() || !is_running_; });
        
        if (!is_running_) {
            break;
        }
        
        // Process pending tasks
        std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
        while (!node_task_queue_.empty()) {
            auto pair = node_task_queue_.front();
            node_task_queue_.pop();
            
            auto agent = pair.first;
            auto node = pair.second;
            
            process_node(agent, nullptr, node);
        }
    }
}

} // namespace runtime
} // namespace agentflow
