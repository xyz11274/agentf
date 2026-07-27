// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SessionManager 实现——用户身份→沙箱映射（AgentFoundry 沙箱机制移植）
// 对齐 tasks.md T050：线程安全的 SandboxKey → Sandbox 映射

#include "daemon/session_manager.h"

#include <algorithm>
#include <utility>

namespace agentflow {
namespace daemon {

void SessionManager::put(const sandbox::SandboxKey& key, std::shared_ptr<sandbox::Sandbox> sandbox) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_[key] = std::move(sandbox);
}

std::shared_ptr<sandbox::Sandbox> SessionManager::get(const sandbox::SandboxKey& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(key);
  if (it == sessions_.end()) {
    return nullptr;
  }
  return it->second;
}

std::vector<std::shared_ptr<sandbox::Sandbox>> SessionManager::list_active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<sandbox::Sandbox>> result;
  result.reserve(sessions_.size());
  for (const auto& [key, sb] : sessions_) {
    if (sb && sb->state() != sandbox::SandboxState::RECYCLING) {
      result.push_back(sb);
    }
  }
  return result;
}

void SessionManager::destroy(const sandbox::SandboxKey& key) {
  std::shared_ptr<sandbox::Sandbox> sb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(key);
    if (it == sessions_.end()) {
      return;  // 幂等：未命中无操作（B7）
    }
    sb = it->second;
    sessions_.erase(it);
  }
  if (sb) {
    sb->destroy();  // 幂等：Sandbox::destroy 内部处理已销毁情况
  }
}

void SessionManager::destroy_all() {
  std::vector<std::shared_ptr<sandbox::Sandbox>> to_destroy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    to_destroy.reserve(sessions_.size());
    for (auto& [key, sb] : sessions_) {
      if (sb) to_destroy.push_back(std::move(sb));
    }
    sessions_.clear();
  }
  // 在锁外摧毁避免持锁回调死锁
  for (auto& sb : to_destroy) {
    if (sb) sb->destroy();
  }
}

}  // namespace daemon
}  // namespace agentflow
