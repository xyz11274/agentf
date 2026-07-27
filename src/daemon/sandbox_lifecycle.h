// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxLifecycle——沙箱生命周期管理（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T052 + research.md Decision 2：create/destroy/list_active 基础生命周期能力
// F6 单一职责：SandboxLifecycle 管 create/destroy/list_active；SandboxSnapshots 管 snapshot/restore/gc_recycle

#ifndef AGENTFLOW_DAEMON_SANDBOX_LIFECYCLE_H
#define AGENTFLOW_DAEMON_SANDBOX_LIFECYCLE_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "daemon/config.h"
#include "daemon/session_manager.h"
#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"

// SandboxSnapshots 前向声明（避免循环 include）
namespace agentflow { namespace daemon { class SandboxSnapshots; } }

namespace agentflow {
namespace daemon {

// SandboxLifecycle：按用户身份查找/创建沙箱，同用户复用，闲置 GC 回收
// 持有 SessionManager（user→sandbox 映射）+ SandboxFactory（构造沙箱）+ config
// daemon initialize 时注入（src/daemon/daemon.cpp:137）
class SandboxLifecycle {
 public:
  SandboxLifecycle(SessionManager& session_manager, const daemon::DaemonConfig& config);
  ~SandboxLifecycle();

  // create：按 owner 查找或创建沙箱
  // 前置：owner 三元组非空
  // 后置：命中返回已有沙箱（复用，workdir 一致）；未命中走 SandboxFactory::create
  // 异常：超并发上限抛 ConcurrentLimitException(当前数, 上限) —— 拒绝新用户不中断已有（FR-021）
  //       拉起失败透传 SandboxFactory::SandboxLaunchException —— 不 fallback（B22）
  std::shared_ptr<sandbox::Sandbox> create(const sandbox::SandboxKey& owner);

  // destroy：摧毁指定 sandbox_id 的沙箱（幂等，B7）
  // 后置：sandbox->destroy() + 从 SessionManager 移除
  void destroy(const std::string& sandbox_id);

  // destroy_by_agent：摧毁指定 agent_id 关联的所有沙箱（幂等）
  // 用途：session destroy 时按 agent 批量清理，避免调用方遍历 list_active 二次匹配
  // 后置：所有匹配 agent_id 的沙箱执行 destroy() + 从 SessionManager 移除
  // 返回：实际摧毁的沙箱数量
  int destroy_by_agent(const std::string& agent_id);

  // list_active：活沙箱列表（含 RUNNING/STOPPED）
  // 用途：并发上限检查 + GC 周期轮询
  std::vector<std::shared_ptr<sandbox::Sandbox>> list_active() const;

  // recycle：GC 触发回收——sandbox 转 RECYCLING 终态（不可逆，Q3 澄清）
  // 后置：sandbox->destroy() + 从 SessionManager 移除（快照兜底，由 SandboxSnapshots 负责）
  void recycle(const std::string& sandbox_id);

  // find_owner：按 sandbox_id 查找 owner（F6 单一职责：遍历 list_active 匹配）
  // 用途：SandboxSnapshots::restore_from_snapshot 需要 owner 来 fork 新沙箱
  // 返回：找到的 SandboxKey，未命中返回空键
  sandbox::SandboxKey find_owner_by_sandbox_id(const std::string& sandbox_id) const;

  // 启动 GC 线程（非阻塞，daemon initialize 时调用）
  // 后置：gc_running_ = true，gc_thread_ 在后台跑 gc_loop()
  void start_gc_thread();

  // 注入 SandboxSnapshots 引用（daemon::initialize 中创建 SandboxSnapshots 后调用）
  // 由于 SandboxSnapshots 构造时依赖 SandboxLifecycle，无法在构造函数中注入
  void set_snapshots(SandboxSnapshots& snapshots) { snapshots_ = &snapshots; }

  // 停止 GC 线程（daemon shutdown 时调用，幂等）
  void stop_gc_thread();

  // 并发上限异常（含当前数与上限，M3 错误可定位）
  class ConcurrentLimitException : public std::runtime_error {
   public:
    ConcurrentLimitException(int current, int limit)
        : std::runtime_error("Concurrent sandbox limit exceeded: " +
                            std::to_string(current) + "/" + std::to_string(limit)),
      current_(current), limit_(limit) {}
    int current() const { return current_; }
    int limit() const { return limit_; }

   private:
    int current_;
    int limit_;
  };

  // 暴露 config（供 RpcHandler 通过 sandbox_lifecycle_ 访问 agent package 路径等）
  const daemon::DaemonConfig& config() const { return config_; }

 private:
  SessionManager&              session_manager_;
  const daemon::DaemonConfig&  config_;
  SandboxSnapshots*            snapshots_ = nullptr;  // 后注入，可为空（向后兼容）
  std::atomic<bool>            gc_running_{false};
  std::thread                  gc_thread_;

  // GC 线程主循环：周期轮询 list_active，闲置超 gc_destroy_ttl_ms 调 recycle
  void gc_loop();
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_SANDBOX_LIFECYCLE_H
