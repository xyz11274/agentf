// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxSnapshots 实现——沙箱快照管理（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T054：snapshot/restore_from_snapshot/gc_recycle
// B9 跳过 dotfile，B11 异常兜底（不可访诚实记录）

#include "daemon/sandbox_snapshots.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "sandbox/sandbox_e2b.h"  // E2BSandbox::ForkFromSnapshot (B3 restore 通道)
#include "utils/logger.h"

namespace agentflow {
namespace daemon {

namespace fs = std::filesystem;

namespace {
inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 检查文件名是否应跳过（对齐 lld.md B9 + AgentFoundry store.ts:44-50）
inline bool should_skip(const std::string& name) {
  if (name.size() > 0 && name[0] == '.') return true;
  for (const char* skip : SandboxSnapshots::SKIP_DIRS) {
    if (name == skip) return true;
  }
  return false;
}
}  // namespace

SandboxSnapshots::SandboxSnapshots(SandboxLifecycle& lifecycle, const daemon::DaemonConfig& config)
    : lifecycle_(lifecycle), config_(config) {
  persist_path_ = config_.sandbox_config.snapshot_root + "/registry.jsonc";
  _load_from_disk();
}

std::string SandboxSnapshots::generate_uuid() {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device rd;
  std::stringstream ss;
  ss << std::hex << now << "-" << rd() << "-" << rd() << "-" << rd();
  return ss.str();
}

void SandboxSnapshots::copy_dir_skip_dotfiles(const std::string& src, const std::string& dst) {
  std::error_code ec;
  fs::create_directories(dst, ec);
  if (ec) {
    throw std::runtime_error("Failed to create destination: " + dst + " (" + ec.message() + ")");
  }
  for (const auto& entry : fs::directory_iterator(src, ec)) {
    if (ec) break;
    const std::string name = entry.path().filename().string();
    if (should_skip(name)) continue;
    const auto& src_path = entry.path();
    const auto dst_path = fs::path(dst) / name;
    try {
      if (entry.is_directory()) {
        copy_dir_skip_dotfiles(src_path.string(), dst_path.string());
      } else {
        fs::copy(src_path, dst_path, fs::copy_options::overwrite_existing, ec);
      }
    } catch (...) {
      // best-effort per entry (B11)
      continue;
    }
  }
}

Snapshot SandboxSnapshots::snapshot(const std::string& sandbox_id, const std::string& sandbox_mode) {
  // 前置：sandbox_id 在活沙箱列表中——查其 workdir
  auto active = lifecycle_.list_active();
  std::string workdir;
  for (const auto& sb : active) {
    if (sb && sb->id() == sandbox_id) {
      workdir = sb->workdir();
      break;
    }
  }
  if (workdir.empty()) {
    throw std::runtime_error("Snapshot failed: sandbox '" + sandbox_id + "' not active");
  }

  // 构造快照存储路径：snapshot_root/<sandbox_id>/<snapshot_id>/
  std::string snap_id = generate_uuid();
  std::string store_path = config_.sandbox_config.snapshot_root + "/" +
                           sandbox_id + "/" + snap_id;
  std::error_code ec;
  fs::create_directories(store_path, ec);
  if (ec) {
    throw std::runtime_error("Snapshot failed: cannot create store_path '" +
                            store_path + "' (" + ec.message() + ")");
  }

  // Local/Docker 模式：拷贝 workdir 到 store_path；E2B 模式只在内存记录
  //（平台侧快照由 E2BSandbox::snapshot 调用 E2B SDK 完成）
  if (sandbox_mode != "E2B") {
    copy_dir_skip_dotfiles(workdir, store_path);
  }

  // 统计快照大小（运维指标）
  int64_t size = 0;
  if (sandbox_mode != "E2B") {
    for (const auto& entry : fs::recursive_directory_iterator(store_path, ec)) {
      if (entry.is_regular_file()) {
        size += static_cast<int64_t>(entry.file_size());
      }
    }
  }

  Snapshot snap;
  snap.snapshot_id = snap_id;
  snap.sandbox_id = sandbox_id;
  snap.created_at = now_ms();
  snap.store_path = sandbox_mode == "E2B" ? "" : store_path;
  snap.size_bytes = size;
  snap.sandbox_mode = sandbox_mode;
  snapshots_.push_back(snap);
  _save_to_disk();  // 持久化 snapshot_id → sandbox_id 映射（B3 daemon 重启后 restore）
  return snap;
}

std::unique_ptr<sandbox::Sandbox> SandboxSnapshots::restore_from_snapshot(const std::string& snapshot_id) {
  // 找快照
  Snapshot found;
  bool hit = false;
  for (const auto& s : snapshots_) {
    if (s.snapshot_id == snapshot_id) {
      found = s;
      hit = true;
      break;
    }
  }
  if (!hit) {
    throw std::runtime_error("Restore failed: snapshot '" + snapshot_id + "' not found");
  }

  // 按 sandbox_mode 分支：E2B → 平台侧 fork；Local/Docker → 本地 workdir 拷贝
  if (found.sandbox_mode == "E2B") {
    // E2B 分支：从已快照的源沙箱 fork 派生新沙箱
    sandbox::SandboxKey owner = lifecycle_.find_owner_by_sandbox_id(found.sandbox_id);
    sandbox::E2BSandbox::ForkParams fp;
    fp.owner = owner;
    fp.workdir_root = config_.sandbox_config.workdir_root;
    fp.sandbox_http_server_path =
        fs::absolute("docker/sandbox/http_server/sandbox_http_server.py").string();
    fp.sandbox_http_port = config_.sandbox_config.sandbox_http_port;
    fp.output_max_bytes = config_.sandbox_config.output_max_bytes;
    fp.source_sandbox_id = found.sandbox_id;
    fp.remote_workspace_dir = config_.sandbox_config.e2b_remote_workspace_dir;
    fp.e2b_api_url = config_.sandbox_config.e2b_api_url;
    fp.e2b_api_key = config_.sandbox_config.e2b_api_key;
    fp.e2b_default_template = config_.sandbox_config.e2b_default_template;
    fp.e2b_network_allow_out = config_.sandbox_config.e2b_network_allow_out;
    fp.e2b_ca_cert_path = config_.sandbox_config.e2b_ca_cert_path;
    try {
      return sandbox::E2BSandbox::ForkFromSnapshot(fp);
    } catch (const std::exception& e) {
      // fork 失败是服务端能力问题（如 E2B 不支持 fork 端点），不阻塞流程
      // 日志含完整上下文供定位，调用方收到 nullptr 后全新建沙箱
      Logger::instance().warn_f(
          "[SANDBOX] E2B fork failed for snapshot=%s source_sandbox=%s: %s "
          "(falling back to new sandbox)",
          snapshot_id.c_str(), found.sandbox_id.c_str(), e.what());
      return nullptr;
    }
  }

  // Local/Docker 分支：从 store_path 拷贝恢复（待实现，当前仍为骨架）
  (void)found;
  throw std::runtime_error("Restore from snapshot (Local/Docker) not yet implemented");
}

void SandboxSnapshots::gc_recycle() {
  int64_t ttl_ms = config_.sandbox_config.gc_recycle_snapshot_ttl_ms;
  int64_t threshold = now_ms() - ttl_ms;
  std::error_code ec;
  std::vector<Snapshot> surviving;
  for (const auto& s : snapshots_) {
    if (s.created_at < threshold) {
      fs::remove_all(s.store_path, ec);
      if (ec) {
        surviving.push_back(s);
      }
    } else {
      surviving.push_back(s);
    }
  }
  snapshots_ = surviving;
  _save_to_disk();
}

void SandboxSnapshots::_save_to_disk() const {
  // 持久化 snapshots_ 列表到 registry.jsonc（简化 JSON 格式，非 JSONC，存为 .jsonc 兼容编辑器）
  // 失败不抛异常：持久化是优化，非关键路径（真相源在内存，重启后 _load_from_disk 静默跳过）
  std::error_code ec;
  fs::create_directories(fs::path(persist_path_).parent_path(), ec);
  if (ec) return;

  rapidjson::Document doc;
  doc.SetArray();
  rapidjson::Document::AllocatorType& alloc = doc.GetAllocator();
  for (const auto& s : snapshots_) {
    rapidjson::Value entry(rapidjson::kObjectType);
    entry.AddMember("snapshot_id", rapidjson::Value(s.snapshot_id.c_str(), alloc).Move(), alloc);
    entry.AddMember("sandbox_id", rapidjson::Value(s.sandbox_id.c_str(), alloc).Move(), alloc);
    entry.AddMember("created_at", s.created_at, alloc);
    entry.AddMember("store_path", rapidjson::Value(s.store_path.c_str(), alloc).Move(), alloc);
    entry.AddMember("size_bytes", s.size_bytes, alloc);
    entry.AddMember("sandbox_mode", rapidjson::Value(s.sandbox_mode.c_str(), alloc).Move(), alloc);
    doc.PushBack(entry.Move(), alloc);
  }
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
  doc.Accept(writer);
  std::ofstream ofs(persist_path_);
  if (ofs) {
    ofs << buf.GetString() << std::endl;
  }
}

void SandboxSnapshots::_load_from_disk() {
  // 从 registry.jsonc 加载持久化的 snapshots_ 列表
  std::error_code ec;
  if (!fs::exists(persist_path_, ec)) return;  // 首次运行，文件不存在，静默跳过
  std::ifstream ifs(persist_path_);
  if (!ifs) return;
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  if (content.empty()) return;

  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if (doc.HasParseError() || !doc.IsArray()) {
    Logger::instance().warn_f("[SANDBOX] Failed to parse snapshot registry: %s", persist_path_.c_str());
    return;
  }
  for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
    const auto& entry = doc[i];
    if (!entry.IsObject()) continue;
    Snapshot snap;
    if (entry.HasMember("snapshot_id") && entry["snapshot_id"].IsString())
      snap.snapshot_id = entry["snapshot_id"].GetString();
    if (entry.HasMember("sandbox_id") && entry["sandbox_id"].IsString())
      snap.sandbox_id = entry["sandbox_id"].GetString();
    if (entry.HasMember("created_at") && entry["created_at"].IsInt64())
      snap.created_at = entry["created_at"].GetInt64();
    if (entry.HasMember("store_path") && entry["store_path"].IsString())
      snap.store_path = entry["store_path"].GetString();
    if (entry.HasMember("size_bytes") && entry["size_bytes"].IsInt64())
      snap.size_bytes = entry["size_bytes"].GetInt64();
    if (entry.HasMember("sandbox_mode") && entry["sandbox_mode"].IsString())
      snap.sandbox_mode = entry["sandbox_mode"].GetString();
    snapshots_.push_back(snap);
  }
  Logger::instance().info_f("[SANDBOX] Loaded %zu snapshots from %s",
                             snapshots_.size(), persist_path_.c_str());
}

std::vector<Snapshot> SandboxSnapshots::list_snapshots() const {
  return snapshots_;
}

}  // namespace daemon
}  // namespace agentflow