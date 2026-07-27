// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxSnapshots——沙箱快照管理（AgentFoundry 沙箱机制移植）
// 对齐 data-model.md Snapshot 实体 + lld.md T054：snapshot/restore_from_snapshot/gc_recycle
// F6 单一职责：SandboxSnapshots 管 snapshot/restore/gc_recycle；与 SandboxLifecycle 平级独立

#ifndef AGENTFLOW_DAEMON_SANDBOX_SNAPSHOTS_H
#define AGENTFLOW_DAEMON_SANDBOX_SNAPSHOTS_H

#include <memory>
#include <string>
#include <vector>

#include "daemon/config.h"
#include "daemon/sandbox_lifecycle.h"
#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"

namespace agentflow {
namespace daemon {

// Snapshot 实体（对齐 data-model.md Snapshot + lld.md T054）
struct Snapshot {
  std::string  snapshot_id;    // UUID v4
  std::string  sandbox_id;     // 来源沙箱 ID（对齐 lld.md T054：按 sandbox_id 查询）
  int64_t      created_at = 0; // ms 时间戳
  std::string  store_path;     // 宿主机持久化绝对路径（E2B 模式为空）
  int64_t      size_bytes = 0;  // ≥0，运维指标
  std::string  sandbox_mode;   // 沙箱模式：""（Local/Docker）/ "E2B" / "none"
};

// SandboxSnapshots：快照生命周期管理
// 持有 SandboxLifecycle（引用）+ config（snapshot_root/gc_recycle_snapshot_ttl_ms）
// daemon initialize 时注入（src/daemon/daemon.cpp:137）
// 对齐 lld.md T054 接口契约：snapshot/restore_from_snapshot/gc_recycle/list_snapshots
class SandboxSnapshots {
 public:
  SandboxSnapshots(SandboxLifecycle& lifecycle, const daemon::DaemonConfig& config);
  ~SandboxSnapshots() = default;

  // snapshot：拍快照——沙箱 workdir 拷贝到 snapshot_root/<sandbox_id>/<snapshot_id>/
  // 前置：sandbox_id 在 SessionManager 活沙箱列表中
  // 后置：返回 Snapshot 实体（store_path 指向宿主机持久化目录，E2B 模式为空）
  // 跳过 dotfile（.agentfoundry/.venv/__pycache__/node_modules/.git），只保留交付物（B9）
  // 不变量 I-1：销毁时快照兜底，异常时 try/finally 触发
  // sandbox_mode 区分 Local/Docker（""） vs E2B（"E2B"），影响 restore 路径
  Snapshot snapshot(const std::string& sandbox_id, const std::string& sandbox_mode = "");

  // restore_from_snapshot：从快照 fork 新沙箱
  // 前置：snapshot_id 在已拍快照列表中
  // 后置：新沙箱 workdir = 快照内容；返回新 Sandbox 实例
  // 异常：快照不存在抛 std::runtime_error 含 snapshot_id
  std::unique_ptr<sandbox::Sandbox> restore_from_snapshot(const std::string& snapshot_id);

  // gc_recycle：清理过期快照——created_at 超 gc_recycle_snapshot_ttl_ms 删除 store_path
  // 阈值从 config.sandbox_config.gc_recycle_snapshot_ttl_ms 读取（FR-009，默认 24h）
  void gc_recycle();

  // 列出所有现存快照（运维/调试用）
  std::vector<Snapshot> list_snapshots() const;

  // 跳过的目录名（对齐 lld.md B9 + AgentFoundry store.ts:44-50）
  static constexpr const char* SKIP_DIRS[] = {".agentfoundry", ".venv", "__pycache__", "node_modules", ".git"};

 private:
  SandboxLifecycle&           lifecycle_;
  const daemon::DaemonConfig& config_;
  std::vector<Snapshot>       snapshots_;  // 简化：内存持有；生产实现用 ArtifactStore 持久化
  std::string                 persist_path_;  // 持久化文件路径（snapshot_root/registry.jsonc）

  // 生成 UUID v4（简化版）
  static std::string generate_uuid();
  // 递归拷贝 workdir → store_path，跳过 dotfile 和 SKIP_DIRS（B9）
  static void copy_dir_skip_dotfiles(const std::string& src, const std::string& dst);
  // 持久化 snapshots_ 到 persist_path_（JSON）
  void _save_to_disk() const;
  // 从 persist_path_ 加载 snapshots_（构造时调用），文件不存在时静默跳过
  void _load_from_disk();
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_SANDBOX_SNAPSHOTS_H