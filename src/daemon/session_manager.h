// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SessionManager——用户身份→沙箱映射（AgentFoundry 沙箱机制移植）
// 对齐 data-model.md SandboxKey 实体 + tasks.md T050
// 三元组（user_id+client_id+agent_id）完全相同 → 复用同一沙箱；任一字段不同 → 不同沙箱（隔离）

#ifndef AGENTFLOW_DAEMON_SESSION_MANAGER_H
#define AGENTFLOW_DAEMON_SESSION_MANAGER_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"  // SandboxKey

namespace agentflow {
namespace daemon {

// SandboxKey 哈希函数（供 unordered_map 键用）
struct SandboxKeyHash {
  size_t operator()(const sandbox::SandboxKey& k) const {
    return std::hash<std::string>{}(k.user_id) ^
           (std::hash<std::string>{}(k.client_id) << 1) ^
           (std::hash<std::string>{}(k.agent_id) << 2);
  }
};

// SessionManager：SandboxKey → Sandbox 映射，线程安全
// 职责：get_or_create（按 key 查找，命中则复用，未命中则创建）/ destroy / list_active
// 对齐 lld.md T052 SandboxLifecycle 持有 session_manager_
class SessionManager {
 public:
  SessionManager() = default;
  ~SessionManager() = default;

  // 持有沙箱（key→sandbox 映射存入）
  // 前置：key 三元组非空
  // 后置：后续 get(key) 返回该 sandbox
  void put(const sandbox::SandboxKey& key, std::shared_ptr<sandbox::Sandbox> sandbox);

  // 查找沙箱
  // 前置：key 三元组非空
  // 后置：命中返回 sandbox，未命中返回 nullptr
  std::shared_ptr<sandbox::Sandbox> get(const sandbox::SandboxKey& key) const;

  // 持有映射的沙箱列表（活沙箱，含 RUNNING/STOPPED）
  // 用途：并发上限检查 + GC 周期轮询
  std::vector<std::shared_ptr<sandbox::Sandbox>> list_active() const;

  // 摧毁指定 key 的沙箱（从映射移除 + 调 sandbox->destroy()）
  // 幂等：key 未命中时无操作（B7）
  void destroy(const sandbox::SandboxKey& key);

  // 摧毁所有沙箱（shutdown 时用）
  void destroy_all();

 private:
  mutable std::mutex mutex_;
  std::unordered_map<sandbox::SandboxKey, std::shared_ptr<sandbox::Sandbox>, SandboxKeyHash> sessions_;
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_SESSION_MANAGER_H
