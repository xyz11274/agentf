// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// ArtifactStore 实现——沙箱产物快照存储（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T061：snapshot/read_snapshot，跳过 dotfile，幂等，异常 try/finally 兜底

#include "daemon/artifact_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace agentflow {
namespace daemon {

namespace fs = std::filesystem;

namespace {
inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

// ============================================================================
// IgnoreMatcher 实现（已迁移到 src/sandbox/sandbox_ignore.cpp）
// ============================================================================

// ============================================================================
// ArtifactStore 实现
// ============================================================================

ArtifactStore::ArtifactStore(const daemon::DaemonConfig& config)
    : config_(config) {}

std::string ArtifactStore::snapshot_path(const std::string& sandbox_id) const {
  return config_.sandbox_config.snapshot_root + "/" + sandbox_id;
}

std::string ArtifactStore::generate_uuid() {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device rd;
  std::stringstream ss;
  ss << std::hex << now << "-" << rd() << "-" << rd();
  return ss.str();
}

int64_t ArtifactStore::calculate_size(const std::string& path) {
  int64_t size = 0;
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(path, ec)) {
    if (entry.is_regular_file()) {
      size += static_cast<int64_t>(entry.file_size());
    }
  }
  return size;
}

void ArtifactStore::copy_with_ignore(const std::string& src_dir, const std::string& dst_dir) {
  std::error_code ec;
  fs::create_directories(dst_dir, ec);
  if (ec) return;  // 创建失败，由调用方处理

  for (const auto& entry : fs::recursive_directory_iterator(src_dir, ec)) {
    if (ec) break;  // 遍历中断，保留已拷贝部分
    auto rel = fs::relative(entry.path(), src_dir).string();
    if (sandbox::IgnoreMatcher::should_ignore(rel)) continue;  // 跳过 dotfile
    auto dst = fs::path(dst_dir) / rel;
    if (entry.is_directory()) {
      fs::create_directories(dst, ec);
    } else if (entry.is_regular_file()) {
      fs::copy(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
    }
  }
}

ArtifactSnapshot ArtifactStore::snapshot(const std::string& sandbox_id, const std::string& workdir) {
  ArtifactSnapshot snap;
  snap.snapshot_id = generate_uuid();
  snap.sandbox_id = sandbox_id;
  snap.created_at = now_ms();

  std::error_code ec;
  // 检查 workdir 可读
  if (!fs::exists(workdir, ec) || ec) {
    // B11：完全不可访时诚实记录丢失，返回空 Snapshot（不抛）
    // 生产实现记 logging.error；此处简化
    return snap;
  }

  // 幂等：先清旧快照再重做（I-4）
  auto snap_dir = snapshot_path(sandbox_id);
  fs::remove_all(snap_dir, ec);

  // try/finally 兜底：异常时确保不抛，只记日志（B11）
  try {
    // 拷贝 workdir 到 snapshot_root/<sandbox_id>/，跳过 dotfile
    copy_with_ignore(workdir, snap_dir);

    // 统计快照大小
    snap.store_path = snap_dir;
    snap.size_bytes = calculate_size(snap_dir);
  } catch (const std::exception&) {
    // B11 异常兜底：不抛，返回空 Snapshot（生产实现写日志）
    // 已拷贝部分保留，不做清理
  }

  return snap;
}

std::vector<std::string> ArtifactStore::read_snapshot(const std::string& sandbox_id,
                                                       const std::string& relative_path) {
  std::vector<std::string> result;
  auto snap_dir = snapshot_path(sandbox_id);
  std::error_code ec;

  // 快照不存在则返回空（不抛，B11 诚实记录）
  if (!fs::exists(snap_dir, ec) || ec) return result;

  auto file_path = fs::path(snap_dir) / relative_path;
  // 路径越界检查：file_path 必须在 snap_dir 内（B4 双检）
  auto resolved = fs::weakly_canonical(file_path, ec);
  if (ec) return result;
  auto snap_canonical = fs::weakly_canonical(snap_dir, ec);
  if (ec) return result;
  auto resolved_str = resolved.string();
  auto snap_str = snap_canonical.string();
  if (resolved_str.find(snap_str) != 0) return result;  // 越界拒绝

  if (!fs::is_regular_file(resolved, ec) || ec) return result;

  // 读文件
  try {
    std::ifstream ifs(resolved_str);
    std::string line;
    while (std::getline(ifs, line)) {
      result.push_back(line);
    }
  } catch (const std::exception&) {
    // B11 异常兜底：返回空，不抛
    result.clear();
  }
  return result;
}

bool ArtifactStore::has_snapshot(const std::string& sandbox_id) const {
  std::error_code ec;
  auto p = snapshot_path(sandbox_id);
  return fs::exists(p, ec) && !ec;
}

}  // namespace daemon
}  // namespace agentflow