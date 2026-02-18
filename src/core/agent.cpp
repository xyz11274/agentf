#include "core/agent.h"

#include <algorithm>

namespace agentflow {
namespace framework {

Agent::Agent(const Config& config) : config_(config), state_(AgentState::CREATED) {
}

void Agent::add_workflow(std::shared_ptr<Workflow> workflow) {
  if(workflow && workflow_map_.size() < static_cast<size_t>(config_.max_workflows)) {
    workflow_map_[workflow->get_id()] = workflow;
    workflows_.push_back(workflow);
  }
}

void Agent::remove_workflow(const WorkflowId& workflow_id) {
  auto it = workflow_map_.find(workflow_id);
  if(it != workflow_map_.end()) {
    workflow_map_.erase(it);
    workflows_.erase(std::remove_if(workflows_.begin(), workflows_.end(),
                                    [&workflow_id](const std::shared_ptr<Workflow>& w) {
                                      return w->get_id() == workflow_id;
                                    }),
                     workflows_.end());
  }
}

std::shared_ptr<Workflow> Agent::get_workflow(const WorkflowId& workflow_id) {
  auto it = workflow_map_.find(workflow_id);
  return it != workflow_map_.end() ? it->second : nullptr;
}

bool Agent::initialize_resources() {
  // Initialize shared resources
  state_ = AgentState::INITIALIZED;
  return true;
}

void Agent::cleanup_resources() {
  // Clean up shared resources
  data_bus_.clear();
  while(!event_queue_.empty()) {
    event_queue_.pop();
  }
}

bool Agent::publish_data(const std::string& key, const DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  data_bus_[key] = value;
  return true;
}

bool Agent::get_data(const std::string& key, DataVariant& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto                        it = data_bus_.find(key);
  if(it != data_bus_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

void Agent::post_event(const std::string& event) {
  std::lock_guard<std::mutex> lock(event_mutex_);
  event_queue_.push(event);
}

std::string Agent::get_event() {
  std::lock_guard<std::mutex> lock(event_mutex_);
  if(!event_queue_.empty()) {
    std::string event = event_queue_.front();
    event_queue_.pop();
    return event;
  }
  return "";
}

bool Agent::initialize() {
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

bool Agent::start() {
  if(state_ != AgentState::INITIALIZED) {
    return false;
  }
  state_ = AgentState::RUNNING;
  return true;
}

bool Agent::pause() {
  if(state_ != AgentState::RUNNING) {
    return false;
  }
  state_ = AgentState::PAUSED;
  return true;
}

bool Agent::resume() {
  if(state_ != AgentState::PAUSED) {
    return false;
  }
  state_ = AgentState::RUNNING;
  return true;
}

bool Agent::stop() {
  if(state_ != AgentState::RUNNING && state_ != AgentState::PAUSED) {
    return false;
  }
  cleanup_resources();
  state_ = AgentState::STOPPED;
  return true;
}

}  // namespace framework
}  // namespace agentflow
