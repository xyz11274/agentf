#include "agent/context.h"

#include <mutex>

namespace agentflow {
namespace framework {

AgentContext::AgentContext(const Config& config)
    : config_(config), is_valid_(true) {
}

AgentContext::~AgentContext() {
  std::lock_guard<std::mutex> lock(mutex_);
  context_variables_.clear();
  env_variables_.clear();
  properties_.clear();
}

void AgentContext::set_variable(const std::string& key, const std::any& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(static_cast<int>(context_variables_.size()) < config_.max_context_variables) {
    context_variables_[key] = value;
  }
}

bool AgentContext::get_variable(const std::string& key, std::any& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = context_variables_.find(key);
  if(it != context_variables_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

bool AgentContext::has_variable(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return context_variables_.count(key) > 0;
}

void AgentContext::remove_variable(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  context_variables_.erase(key);
}

void AgentContext::clear_all_variables() {
  std::lock_guard<std::mutex> lock(mutex_);
  context_variables_.clear();
}

void AgentContext::set_env_variable(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  env_variables_[key] = value;
}

bool AgentContext::get_env_variable(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = env_variables_.find(key);
  if(it != env_variables_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

void AgentContext::clear_env_variables() {
  std::lock_guard<std::mutex> lock(mutex_);
  env_variables_.clear();
}

void AgentContext::set_property(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  properties_[key] = value;
}

bool AgentContext::get_property(const std::string& key, std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = properties_.find(key);
  if(it != properties_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

int AgentContext::get_variable_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return context_variables_.size();
}

bool AgentContext::is_variables_full() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(context_variables_.size()) >= config_.max_context_variables;
}

void AgentContext::invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  is_valid_ = false;
}

}  // namespace framework
}  // namespace agentflow
