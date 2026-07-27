// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxLifecycle 实现——沙箱生命周期管理（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T052 + T055（GC 线程）+ T056（并发软上限）

#include "daemon/sandbox_lifecycle.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#include "daemon/sandbox_snapshots.h"  // SandboxSnapshots::snapshot / gc_recycle
#include "sandbox/sandbox_factory.h"
#include "daemon/artifact_store.h"  // ArtifactStore snapshot
#include "utils/logger.h"

namespace agentflow {
namespace daemon {

namespace {
// 当前时间戳（毫秒）
inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

SandboxLifecycle::SandboxLifecycle(SessionManager& session_manager, const daemon::DaemonConfig& config)
    : session_manager_(session_manager), config_(config) {}

SandboxLifecycle::~SandboxLifecycle() {
  if (gc_running_.load()) {
    gc_running_ = false;
    if (gc_thread_.joinable()) {
      gc_thread_.join();
  }
 }
}

std::shared_ptr<sandbox::Sandbox> SandboxLifecycle::create(const sandbox::SandboxKey& owner) {
  // 先按 key 查找——命中则复用（FR-007，workdir 一致）
  auto existing = session_manager_.get(owner);
  if (existing) {
    return existing;  // 复用已有沙箱
 }

  // 并发软上限（T056/FR-021）：活沙箱数检查
  auto active = session_manager_.list_active();
  if (static_cast<int>(active.size()) >= config_.sandbox_config.sandbox_max_active) {
    throw ConcurrentLimitException(static_cast<int>(active.size()),
                                   config_.sandbox_config.sandbox_max_active);
 }

  // 未命中走 SandboxFactory::create——拉起失败透传 SandboxLaunchException（不 fallback，B22）
  // sandbox_http_server.py 路径：相对 daemon 启动目录（项目根），如 "docker/sandbox/http_server/sandbox_http_server.py"
  // 注意：不能用 workdir_root 做前缀——sandbox_http_server.py 在项目根目录下，不在 workdir_root 下。
  // sandbox_local.cpp:97 chdir→workdir_ 后，相对路径无法正确解析，必须 fs::absolute 提前固定为绝对路径。
  std::string http_server_path = fs::absolute("docker/sandbox/http_server/sandbox_http_server.py").string();
  // 若 daemon 用 sandbox_image_dir 配置，则 http_server_path = sandbox_image_dir + "/http_server/sandbox_http_server.py"

  // 推导 skills_src_dir：先取配置值，若为空则从 agent package 路径推导
  std::string skills_src_dir = config_.sandbox_config.skills_src_dir;
  if (skills_src_dir.empty() && !config_.agents.empty()) {
    // package_path 可能为相对路径（如 "examples/agent/outfit-assistant"），
    skills_src_dir = fs::absolute(config_.agents[0].package_path).string() + "/harness/skills";
  }

  auto sandbox_ptr = sandbox::SandboxFactory::create(
      config_.sandbox_config.mode,
      owner,
      config_.sandbox_config.workdir_root,
      http_server_path,
      config_.sandbox_config.sandbox_http_port,
      config_.sandbox_config.output_max_bytes,
      skills_src_dir,
      config_.sandbox_config.e2b_remote_workspace_dir,
      config_.sandbox_config.image_name,
      config_.sandbox_config.e2b_api_url,
      config_.sandbox_config.e2b_api_key,
      config_.sandbox_config.e2b_default_template,
      config_.sandbox_config.e2b_network_allow_out);

  // mode=none 时 SandboxFactory 返回 nullptr（向后兼容，FR-006）
  if (!sandbox_ptr) {
    return nullptr;
 }

  auto shared = std::shared_ptr<sandbox::Sandbox>(sandbox_ptr.release());
  session_manager_.put(owner, shared);
  return shared;
}

void SandboxLifecycle::destroy(const std::string& sandbox_id) {
  // 幂等：遍历 list_active 找 sandbox_id 匹配的沙箱（B7）
  auto active = session_manager_.list_active();
  for (const auto& sb : active) {
    if (sb && sb->id() == sandbox_id) {
      // T062：销毁前触发快照（try/finally 兜底，B11）
      // 即使 snapshot 抛异常也确保 sandbox->destroy() 执行
      //
      // 快照分为两层（FR-012）：
      // 1. Provider 级 snapshot：各 Provider 自行实现，如 E2B 调 SDK snapshot API
      // 2. SandboxSnapshots 级 snapshot：ArtifactStore 拷贝 workdir（仅 Local/Docker）
      // 两层各自独立——Provider 级负责"平台侧快照"，SandboxSnapshots 负责"本地快照"

      // 第一层：Provider 级快照（E2B 调平台 API；Local/Docker 为 no-op）
      try {
        sb->snapshot();
      } catch (const std::exception& e) {
        Logger::instance().warn_f("[SANDBOX] Provider snapshot failed for sandbox=%s: %s",
                                   sandbox_id.c_str(), e.what());
      }

      // 第二层：ArtifactStore 快照（仅 Local/Docker 模式有效，E2B 无本地 workdir）
      if (snapshots_) {
        try {
          snapshots_->snapshot(sandbox_id, sb->sandbox_mode());
        } catch (const std::exception& e) {
          Logger::instance().warn_f("[SANDBOX] ArtifactStore snapshot failed for sandbox=%s: %s",
                                     sandbox_id.c_str(), e.what());
        }
      } else {
        // 回退：直接 ArtifactStore（向后兼容，sandbox_snapshots 未注入时）
        ArtifactStore store(config_);
        try {
          store.snapshot(sandbox_id, sb->workdir());
        } catch (const std::exception& e) {
          Logger::instance().warn_f("[SANDBOX] Fallback ArtifactStore snapshot failed: %s", e.what());
        }
      }
      sb->destroy();  // 幂等（B7）
      // 从 SessionManager 移除
      session_manager_.destroy(sb->owner());
      return;
    }
  }
  // 未命中：幂等无操作
}

int SandboxLifecycle::destroy_by_agent(const std::string& agent_id) {
  // 按 agent_id 批量摧毁沙箱（幂等）
  // 避免调用方遍历 list_active 后再调 destroy(id) 触发二次遍历
  auto active = session_manager_.list_active();
  int removed = 0;
  for (const auto& sb : active) {
    if (!sb) continue;
    if (sb->owner().agent_id != agent_id) continue;
    // 销毁前快照（与 destroy 路径一致的兜底逻辑，B11）
    if (snapshots_) {
      try {
        snapshots_->snapshot(sb->id());
      } catch (const std::exception& e) {
        Logger::instance().warn_f("[SANDBOX] Snapshot failed for sandbox=%s: %s",
                                   sb->id().c_str(), e.what());
      }
    }
    sb->destroy();  // 幂等（B7）
    session_manager_.destroy(sb->owner());
    Logger::instance().info_f(
        "[SANDBOX] Destroyed sandbox id=%s for agent=%s",
        sb->id().c_str(), agent_id.c_str());
    ++removed;
  }
  if (removed > 0) {
    Logger::instance().info_f(
        "[SANDBOX] destroy_by_agent: removed %d sandbox(es) for agent=%s",
        removed, agent_id.c_str());
  }
  return removed;
}

std::vector<std::shared_ptr<sandbox::Sandbox>> SandboxLifecycle::list_active() const {
  return session_manager_.list_active();
}

void SandboxLifecycle::recycle(const std::string& sandbox_id) {
  // GC 触发回收——转 RECYCLING 终态（不可逆，Q3 澄清）
  // 快照兜底由 SandboxSnapshots 负责（此处不重复）
  destroy(sandbox_id);  // destroy 内部调 sandbox->destroy() + 移除映射
}

void SandboxLifecycle::start_gc_thread() {
  if (gc_running_.load()) {
    return;  // 幂等：已运行则无操作
  }
  gc_running_ = true;
  gc_thread_ = std::thread([this] { gc_loop(); });
}

void SandboxLifecycle::stop_gc_thread() {
  if (!gc_running_.load()) {
    return;  // 幂等：未运行则无操作
  }
  gc_running_ = false;
  if (gc_thread_.joinable()) {
    gc_thread_.join();
  }
}

void SandboxLifecycle::gc_loop() {
  // GC 周期：gc_destroy_ttl_ms（默认 10 min）
  // 每次：list_active → 检查 last_active_at 超时 → recycle
  int64_t ttl_ms = config_.sandbox_config.gc_destroy_ttl_ms;
  int64_t poll_interval_ms = std::min<int64_t>(ttl_ms / 4, 60000);  // 轮询间隔 = TTL/4，上限 60s

  while (gc_running_.load()) {
    auto active = session_manager_.list_active();
    int64_t now = now_ms();
    for (const auto& sb : active) {
      if (!sb) continue;
      // FR-009：闲置超时判定——用 Sandbox::last_active_at() 对比 TTL
      int64_t idle_ms = now - sb->last_active_at();
      if (idle_ms >= ttl_ms) {
        Logger::instance().info_f(
            "[SANDBOX] GC recycling sandbox id=%s owner=%s (idle=%lldms >= ttl=%lldms)",
            sb->id().c_str(), sb->owner().to_string().c_str(),
            static_cast<long long>(idle_ms), static_cast<long long>(ttl_ms));
        recycle(sb->id());
      }
    }

    // 调用 SandboxSnapshots::gc_recycle 清理过期快照（已注入时）
    if (snapshots_) {
      try {
        snapshots_->gc_recycle();
      } catch (const std::exception& e) {
        Logger::instance().warn_f("[SANDBOX] GC recycle snapshots failed: %s", e.what());
      }
    }

    // 等待下一轮询周期
    if (gc_running_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }
  }
}

sandbox::SandboxKey SandboxLifecycle::find_owner_by_sandbox_id(const std::string& sandbox_id) const {
  auto active = session_manager_.list_active();
  for (const auto& sb : active) {
    if (sb && sb->id() == sandbox_id) {
      return sb->owner();
    }
  }
  return sandbox::SandboxKey{};
}

}  // namespace daemon
}  // namespace agentflow
