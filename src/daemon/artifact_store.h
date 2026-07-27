// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// ArtifactStore——沙箱产物快照存储（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T061 接口契约：snapshot/read_snapshot，跳过 dotfile，幂等，异常兜底
// F6 单一职责：ArtifactStore 管 workdir 快照/恢复；与 SandboxLifecycle/SandboxSnapshots 平级

#ifndef AGENTFLOW_DAEMON_ARTIFACT_STORE_H
#define AGENTFLOW_DAEMON_ARTIFACT_STORE_H

#include <string>
#include <vector>

#include "daemon/config.h"
#include "sandbox/sandbox_ignore.h"  // IgnoreMatcher
#include "sandbox/sandbox_types.h"   // ExecResult

namespace agentflow {
namespace daemon {

// Snapshot 元数据（复用于 sandbox_snapshots.h 的 Snapshot 结构体）
// data-model.md Snapshot 实体：snapshot_id/sandbox_id/created_at/store_path/size_bytes
struct ArtifactSnapshot {
  std::string snapshot_id;    // UUID v4
  std::string sandbox_id;     // 来源沙箱
  int64_t     created_at = 0; // ms 时间戳
  std::string store_path;     // 宿主机持久化绝对路径
  int64_t     size_bytes = 0; // ≥0，运维指标
};

// IgnoreMatcher——跳过 dotfile，定义在 src/sandbox/sandbox_ignore.h
// 使用 sandbox::IgnoreMatcher 而非内联定义（对齐 lld.md 文件结构）

// ArtifactStore：沙箱产物快照持久化
// 持有 snapshot_root（从 config.sandbox_config.snapshot_root 读取）
// daemon initialize 时注入（与 SandboxSnapshots 同级）
class ArtifactStore {
 public:
  explicit ArtifactStore(const daemon::DaemonConfig& config);
  ~ArtifactStore() = default;

  // snapshot：对 workdir 做一次性快照
  // 前置：workdir 是宿主机可达目录；snapshot_root 可写
  // 后置：幂等（先清旧再重做）；跳过 dotfile 只保留交付物；返回 ArtifactSnapshot
  // 异常：workdir 完全不可读 → 记日志，返回空 ArtifactSnapshot（诚实记录丢失，B11）
  //       try/finally 兜底——异常时确保不抛，只记日志（B11）
  ArtifactSnapshot snapshot(const std::string& sandbox_id, const std::string& workdir);

  // read_snapshot：从快照读文件
  // 前置：snapshot_id 对应的快照存在
  // 后置：返回文件内容（多行 vector）；路径不在快照中返回空 vector
  // 异常：快照存储路径损坏 → 记日志，返回空 vector（不抛，B11 诚实记录）
  std::vector<std::string> read_snapshot(const std::string& sandbox_id,
                                          const std::string& relative_path);

  // 快照是否存在（供 Bridge 端点判断用）
  bool has_snapshot(const std::string& sandbox_id) const;

 private:
  const daemon::DaemonConfig& config_;

  // 快照存储根目录 = config.sandbox_config.snapshot_root + "/" + sandbox_id
  std::string snapshot_path(const std::string& sandbox_id) const;

  // 统计快照大小（遍历目录累加文件大小）
  static int64_t calculate_size(const std::string& path);

  // 生成 UUID v4（简化版）
  static std::string generate_uuid();

  // 拷贝文件（跳过 dotfile）到快照目录
  static void copy_with_ignore(const std::string& src_dir, const std::string& dst_dir);
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_ARTIFACT_STORE_H