// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// TemplateRegistry 实现——按 agent_id 寻址已预建的 e2b template（v2 段1 预热）
// 对齐 docs/design/features/sandbox/sandbox-v2.md §3 段1 流程：
//   handle_agent_load → SandboxLifecycle::preload_template → TemplateRegistry::preload
//     → 幂等检查（内存映射优先 + e2b_template_build.py check 兜底，REST 方式）
//     → 确认 config e2b_default_template 存在且 READY → 记录 agent_id ↔ templateID 映射
// 仅 e2b 模式有效；Local/Docker/none 模式 no-op（无 template 概念）
// v2 改动 #8 简化：内存映射 + REST 幂等检查，持久化留后续（用户 2026-08-08 拍板）
// CubeSandbox 适配（2026-08-09，用户拍板方向 A）：模板 ID 平台自动生成（tpl-xxx），
// 无 alias 概念，SDK build/alias_exists 端点不实现（404）→ preload 不复用 build，
// 改为 REST 确认已有 READY 模板复用。
// agent 级快照关联（2026-08-09，设计 sandbox-warm-template-oci.md §2.6）：
//   preload 按 agent 解析/构建快照（快照 = 基础模板 + agent manifest includes 全量），
//   未命中自动构建（调 template_admin.py snapshot --manifest），构建成功回收旧快照。

#include "daemon/template_registry.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "sandbox/sandbox_helpers.h"  // sandbox::SandboxHelpers::exec_cmd_argv / resolve_python3
#include "utils/logger.h"
#include "workflow/dsl/json_adapter.h"  // dsl::JsonHelper（读 package.json version）

namespace agentflow {
namespace daemon {

namespace fs = std::filesystem;

// 映射持久化文件路径：优先 config.e2b_template_map_path（默认 data/template_registry.json）
// 防 daemon 重启后 agent 快照映射丢失 → 无法回收旧快照（AC-5）
// 运行时状态归 data/（与 sessions.db 同目录惯例），不落 logs/（日志目录可能轮转清理）
fs::path TemplateRegistry::registry_persist_path() const {
  std::string p = config_.sandbox_config.e2b_template_map_path;
  if (p.empty()) {
    p = "data/template_registry.json";
  }
  return fs::absolute(p);
}

TemplateRegistry::TemplateRegistry(const daemon::DaemonConfig& config) : config_(config) {}

std::string TemplateRegistry::resolve(const std::string& agent_id) const {
  // 并发保护（P3-5）：RPC 并发线程读 alias_map_，与 preload 的写入互斥
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = alias_map_.find(agent_id);
  if (it != alias_map_.end()) {
    return it->second;
  }
  return "";  // 未确认返回空串（段2 fallback 到 config e2b_default_template）
}

void TemplateRegistry::load_persist_map() {
  // 并发保护（P3-5）：persist_loaded_ check-then-set + persist_map_ 写入需与 preload 互斥
  std::lock_guard<std::mutex> lock(mutex_);
  // 启动/首次 preload 时读持久化映射（agent_id → snapshotID + version/hash），
  // 恢复跨重启关联（AC-5 回收前提）+ 版本检测（AC-4）
  if (!persist_loaded_) {
    persist_loaded_ = true;
    fs::path p = registry_persist_path();
    if (!fs::is_regular_file(p)) {
      return;
    }
    std::ifstream ifs(p);
    std::stringstream ss;
    ss << ifs.rdbuf();
    auto doc = dsl::JsonDocumentFactory::create();
    if (!doc || !doc->parse(ss.str())) {
      Logger::instance().warn_f("[TEMPLATE] persist map unparseable: %s", p.string().c_str());
      return;
    }
    auto root = doc->root();
    if (!root) {
      return;
    }
    auto arr = dsl::JsonHelper::get_array(root, "entries");
    if (!arr) {
      return;
    }
    // 逐项读 {"agent_id": "...", "snapshot": "...", "version": "...", "hash": "..."}
    for (size_t i = 0; i < arr->array_size(); ++i) {
      auto item = arr->array_at(i);
      auto id = dsl::JsonHelper::get_string(item, "agent_id");
      auto snap = dsl::JsonHelper::get_string(item, "snapshot");
      auto ver = dsl::JsonHelper::get_string(item, "version", "0");
      auto h8 = dsl::JsonHelper::get_string(item, "hash");
      if (!id.empty() && !snap.empty()) {
        persist_map_[id] = {snap, ver, h8};
      }
    }
    Logger::instance().info_f("[TEMPLATE] loaded %zu persisted snapshot mappings from %s",
                              persist_map_.size(), p.string().c_str());
  }
}

// JSON 字符串转义（save_persist_map 手工拼接用）——agent_id/version 等字段来自
// package.json（开发者可控），可能含 `"`/`\`/控制字符，不转义会生成非法 JSON，
// 导致下次 load_persist_map 解析失败 → 复用/回收静默丢失（review P3-4）
static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

void TemplateRegistry::save_persist_map() {
  // 并发保护（P3-5）：persist_map_ 读取 + 写文件需与 preload/resolve 互斥。
  // 注意：本方法内部加锁，调用方（preload）不得在外层持锁调用（std::mutex 不可重入）
  std::lock_guard<std::mutex> lock(mutex_);
  // 写回持久化映射（构建/复用成功后更新）
  std::ostringstream oss;
  oss << "{\"entries\":[";
  bool first = true;
  for (const auto& [id, rec] : persist_map_) {
    if (!first) oss << ",";
    first = false;
    oss << "{\"agent_id\":\"" << json_escape(id)
        << "\",\"snapshot\":\"" << json_escape(rec.snapshot)
        << "\",\"version\":\"" << json_escape(rec.version)
        << "\",\"hash\":\"" << json_escape(rec.hash) << "\"}";
  }
  oss << "]}";
  fs::path p = registry_persist_path();
  try {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p);
    ofs << oss.str();
    Logger::instance().info_f("[TEMPLATE] persisted %zu snapshot mappings -> %s",
                              persist_map_.size(), p.string().c_str());
  } catch (const std::exception& e) {
    Logger::instance().warn_f("[TEMPLATE] persist map failed: %s", e.what());
  }
}

void TemplateRegistry::set_e2b_env() {
  // 与 E2BSandbox 构造一致：子进程（e2b_template_build.py）继承的环境变量
  // E2B_API_KEY：配置 e2b_api_key > 环境变量 E2B_API_KEY
  std::string api_key = config_.sandbox_config.e2b_api_key;
  if (!api_key.empty()) {
    setenv("E2B_API_KEY", api_key.c_str(), 1);
  }
  // E2B_API_URL（可选）
  if (!config_.sandbox_config.e2b_api_url.empty()) {
    setenv("E2B_API_URL", config_.sandbox_config.e2b_api_url.c_str(), 1);
  }
  // SSL_CERT_FILE（自建部署自签名证书，相对路径转绝对）
  if (!config_.sandbox_config.e2b_ca_cert_path.empty()) {
    std::string abs_cert = fs::absolute(config_.sandbox_config.e2b_ca_cert_path).string();
    setenv("SSL_CERT_FILE", abs_cert.c_str(), 1);
  }
}

int TemplateRegistry::check_alias_exists(const std::string& template_id) {
  // 幂等检查：e2b_template_build.py check --name <templateID>（REST GET /templates/{id}）
  // exit 0=存在 1=不存在 2=REST/网络错误（平台不可达/API key 错）
  fs::path script = fs::absolute("docker/cubesandbox/e2b_template_build.py");
  if (!fs::exists(script)) {
    Logger::instance().error_f("[TEMPLATE] e2b_template_build.py not found: %s",
                               script.string().c_str());
    return 2;
  }
  std::vector<std::string> argv = {
      sandbox::SandboxHelpers::resolve_python3(), script.string(),
      "check", "--name", template_id,
  };
  auto res = sandbox::SandboxHelpers::exec_cmd_argv(argv);
  Logger::instance().info_f(
      "[TEMPLATE] check template=%s exit=%d output=%s",
      template_id.c_str(), res.exit_code,
      res.output.empty() ? "(empty)" : res.output.c_str());
  return res.exit_code;
}

int TemplateRegistry::delete_template(const std::string& template_id) {
  // 回收旧快照：e2b_template_build.py delete --template-id <templateID>
  // （REST DELETE /templates/{id}，2026-08-09 实测 204 成功；404 视为已不存在幂等）
  // exit 0=删除成功/已不存在 2=REST/网络错误
  fs::path script = fs::absolute("docker/cubesandbox/e2b_template_build.py");
  if (!fs::exists(script)) {
    Logger::instance().error_f("[TEMPLATE] e2b_template_build.py not found: %s",
                               script.string().c_str());
    return 2;
  }
  std::vector<std::string> argv = {
      sandbox::SandboxHelpers::resolve_python3(), script.string(),
      "delete", "--template-id", template_id,
  };
  auto res = sandbox::SandboxHelpers::exec_cmd_argv(argv);
  Logger::instance().info_f(
      "[TEMPLATE] delete template=%s exit=%d output=%s",
      template_id.c_str(), res.exit_code,
      res.output.empty() ? "(empty)" : res.output.c_str());
  return res.exit_code;
}

std::string TemplateRegistry::read_agent_version(const std::string& package_path) {
  // 读 agent package.json version 字段；无则返回 "0"
  fs::path pkg_json = fs::path(package_path) / "package.json";
  if (!fs::is_regular_file(pkg_json)) {
    return "0";
  }
  std::ifstream ifs(pkg_json);
  std::stringstream ss;
  ss << ifs.rdbuf();
  auto doc = dsl::JsonDocumentFactory::create();
  if (!doc || !doc->parse(ss.str())) {
    return "0";
  }
  auto root = doc->root();
  if (!root) {
    return "0";
  }
  return dsl::JsonHelper::get_string(root, "version", "0");
}

std::string TemplateRegistry::compute_manifest_hash(const std::string& package_path) {
  // 调 template_admin.py snapshot --hash-manifest <pkg>/manifest.jsonc
  // 计算 manifest includes 内容 hash8（复用 e2b_sync 权威解析）；失败返回空串
  fs::path script = fs::absolute("scripts/e2b/template_admin.py");
  if (!fs::exists(script)) {
    Logger::instance().error_f("[TEMPLATE] template_admin.py not found: %s",
                               script.string().c_str());
    return "";
  }
  fs::path manifest = fs::path(package_path) / "manifest.jsonc";
  if (!fs::is_regular_file(manifest)) {
    return "";  // 无 manifest → 无资产可 hash（构建时仅基础模板）
  }
  std::vector<std::string> argv = {
      sandbox::SandboxHelpers::resolve_python3(), script.string(),
      "snapshot", "--hash-manifest", manifest.string(),
  };
  auto res = sandbox::SandboxHelpers::exec_cmd_argv(argv);
  Logger::instance().info_f(
      "[TEMPLATE] manifest hash agent_pkg=%s exit=%d output=%s",
      package_path.c_str(), res.exit_code,
      res.output.empty() ? "(empty)" : res.output.c_str());
  if (res.exit_code != 0) {
    return "";
  }
  // 解析 hash8：exec_cmd_argv 把 stderr 合并进 stdout（dup2），不能取"最后一行"——
  // 脚本的 _log 走 stderr、print(h8) 走 stdout，合并流顺序依赖缓冲不可靠（review P2-2）。
  // 改为模式匹配：逐行找 `^[0-9a-f]{8}$`（脚本 print(h8) 的唯一纯 hash 行）。
  std::string h8;
  {
    std::istringstream iss(res.output);
    std::string line;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      // 8 位十六进制（[snapshot_create] 日志行含前缀/汉字，不匹配纯 hash 行）
      bool is_hex8 = line.size() == 8;
      for (char c : line) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          is_hex8 = false;
          break;
        }
      }
      if (is_hex8) {
        h8 = line;
        break;
      }
    }
  }
  return h8;
}

std::string TemplateRegistry::build_agent_snapshot(const std::string& agent_id,
                                                   const std::string& package_path,
                                                   const std::string& base_template) {
  // 调 template_admin.py snapshot 构建 agent 增量快照：
  //   snapshot --base-template <基础模板> --manifest <pkg>/manifest.jsonc
  //   --name snap-<agent_id>-<version>-<hash8>（设计 §2.6 命名规范）
  // 成功返回平台 snapshotID（stdout 末行），失败返回空串
  fs::path script = fs::absolute("scripts/e2b/template_admin.py");
  if (!fs::exists(script)) {
    Logger::instance().error_f("[TEMPLATE] template_admin.py not found: %s",
                               script.string().c_str());
    return "";
  }
  std::string version = read_agent_version(package_path);
  std::string hash8 = compute_manifest_hash(package_path);
  std::string snap_name = "snap-" + agent_id + "-" + version;
  if (!hash8.empty()) {
    snap_name += "-" + hash8;
  }
  fs::path manifest = fs::path(package_path) / "manifest.jsonc";
  std::vector<std::string> argv = {
      sandbox::SandboxHelpers::resolve_python3(), script.string(),
      "snapshot",
      "--base-template", base_template,
      "--name", snap_name,
  };
  if (fs::is_regular_file(manifest)) {
    argv.push_back("--manifest");
    argv.push_back(manifest.string());
  }
  // 2026-08-11：agentflow-runtime 由宿主随 agent package 一起上传到沙箱
  //（--upload-dir 与 --manifest 并存，见 template_admin.py snapshot 步骤 3）
  // 目标路径 $HOME/workspace/packages/agentflow-runtime——与沙箱版 setup.sh 的
  // AF_RUNTIME_PKG 默认值（$SCRIPT_DIR/packages/agentflow-runtime）对齐，
  // setup.sh 沙箱预热时 pip install -e 该路径即可，无需 pip 源/--find-links。
  fs::path runtime_pkg = fs::absolute("packages/agentflow-runtime");
  if (fs::is_directory(runtime_pkg)) {
    argv.push_back("--upload-dir");
    argv.push_back(runtime_pkg.string() + ":/home/user/workspace/packages/agentflow-runtime");
  }
  Logger::instance().info_f(
      "[TEMPLATE] building snapshot agent=%s name=%s base=%s manifest=%s runtime_upload=%s",
      agent_id.c_str(), snap_name.c_str(), base_template.c_str(),
      fs::is_regular_file(manifest) ? manifest.string().c_str() : "(none)",
      fs::is_directory(runtime_pkg) ? "yes" : "no");
  // 超时 180s：快照构建含 E2B 沙箱创建 + 沙箱内 setup.sh，网络不可达时可能挂起；
  // 超时返回 124 → 上层走"降级 base + execute v1 兜底"路径，不阻塞 daemon 启动。
  auto res = sandbox::SandboxHelpers::exec_cmd_argv(argv, /*timeout_ms=*/180000);
  Logger::instance().info_f(
      "[TEMPLATE] snapshot build agent=%s exit=%d output=%s",
      agent_id.c_str(), res.exit_code,
      res.output.empty() ? "(empty)" : res.output.c_str());
  if (res.exit_code != 0) {
    return "";
  }
  // 解析 snapshotID：同 hash 解析（P2-2），exec_cmd_argv 合并 stderr 到 stdout，
  // 不能取"最后一行"。改为模式匹配：逐行找以 `snap-` 开头且非日志前缀的行
  // （脚本 print(snap_id) 的唯一纯 snapshotID 行；`[snapshot_create]` 日志行以 [ 开头）。
  std::string snap_id;
  {
    std::istringstream iss(res.output);
    std::string line;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      // snap- 前缀 + 非日志行（日志行形如 "[snapshot_create] ..."）
      if (line.rfind("snap-", 0) == 0) {
        snap_id = line;
        break;
      }
    }
  }
  return snap_id;
}

bool TemplateRegistry::preload(const std::string& agent_id, const std::string& package_path) {
  // 仅 e2b 模式有效；Local/Docker/none 模式 no-op（v2 改动 #3 约束）
  if (config_.sandbox_config.mode != "e2b") {
    Logger::instance().info_f(
        "[TEMPLATE] preload skipped for agent=%s (mode=%s, no template concept)",
        agent_id.c_str(), config_.sandbox_config.mode.c_str());
    return true;
  }

  // 改动 #6：preload 前配置校验——e2b 关键字段缺失时 warning + 返回 false（降级）
  const auto& sc = config_.sandbox_config;
  if (sc.e2b_api_key.empty() && !getenv("E2B_API_KEY")) {
    Logger::instance().warn_f(
        "[TEMPLATE] preload skipped for agent=%s: e2b_api_key not set "
        "(config sandbox.e2b_api_key or env E2B_API_KEY)", agent_id.c_str());
    return false;
  }

  // 基础模板（快照构建源 + 降级兜底）：config e2b_default_template
  std::string base = sc.e2b_default_template;
  if (base.empty()) {
    Logger::instance().warn_f(
        "[TEMPLATE] preload skipped for agent=%s: e2b_default_template not set",
        agent_id.c_str());
    return false;
  }

  // 幂等：persist_map_ 是跨重启真相源（含 version/hash），alias_map_ 只是进程内缓存。
  // 不在 alias_map_ 命中时无条件短路返回——否则同进程 re-load 时 package 变化不会触发
  // 重建（AC-4/SC-A3 失效），且构建失败降级映射会被永久锁定（review P2-1）。
  // 统一走下方 persist_map_ 检测：version/hash 未变 + 平台存在 → 复用；变化 → 重建。
  set_e2b_env();
  load_persist_map();  // 首次加载持久化映射（恢复跨重启关联，AC-5 回收前提）

  // 当前 agent 的 version/hash（AC-4 版本变化检测：任一变化 → 重建新快照）
  std::string cur_version = read_agent_version(package_path);
  std::string cur_hash = compute_manifest_hash(package_path);

  // 持久化复用：有记录且 version/hash 未变 + 平台快照仍存在 → 复用（跳过构建）
  // 并发保护（P3-5）：persist_map_ 读取加锁；REST 检查（check_alias_exists）耗时，
  // 锁内只拷贝记录，检查在锁外执行
  SnapshotRecord rec;
  bool has_rec = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto pit = persist_map_.find(agent_id);
    if (pit != persist_map_.end()) {
      rec = pit->second;
      has_rec = true;
    }
  }
  if (has_rec) {
    bool version_unchanged = (rec.version == cur_version);
    bool hash_unchanged = (rec.hash == cur_hash);  // 均无 manifest 时（空串==空串）视为未变
    if (version_unchanged && hash_unchanged) {
      int check_rc = check_alias_exists(rec.snapshot);
      if (check_rc == 0) {
        Logger::instance().info_f(
            "[TEMPLATE] preload reuse persisted snapshot for agent=%s snapshot=%s "
            "(version/hash unchanged)",
            agent_id.c_str(), rec.snapshot.c_str());
        {
          std::lock_guard<std::mutex> lock(mutex_);
          alias_map_[agent_id] = rec.snapshot;
        }
        return true;
      }
      // 快照在平台已不存在（被外部删除）→ 落入下方重建
      Logger::instance().warn_f(
          "[TEMPLATE] persisted snapshot=%s gone on platform for agent=%s, rebuild",
          rec.snapshot.c_str(), agent_id.c_str());
    } else {
      Logger::instance().info_f(
          "[TEMPLATE] agent=%s snapshot stale (version %s->%s, hash %s->%s), rebuild",
          agent_id.c_str(), rec.version.c_str(), cur_version.c_str(),
          rec.hash.empty() ? "(none)" : rec.hash.c_str(),
          cur_hash.empty() ? "(none)" : cur_hash.c_str());
    }
  }

  // agent 级快照：未命中 / 版本变化 / 快照丢失 → 自动构建（快照 = 基础模板 + manifest includes 全量）
  // 设计 §2.6.2：agent load 时快照不存在 → 自动构建并注册映射
  std::string snap_id = build_agent_snapshot(agent_id, package_path, base);
  if (snap_id.empty() || snap_id.size() < 4 || snap_id.rfind("snap-", 0) != 0) {
    // 构建失败（脚本报错 / stdout 无合法 snapshotID）→ 降级：确认基础模板可用则复用
    // （不阻塞首问——execute 走 v1 懒加载兜底，见 §2.6.2 步骤 3）
    Logger::instance().warn_f(
        "[TEMPLATE] snapshot build FAILED agent=%s (build output above), degrade to base=%s "
        "(execute 首问走 v1 兜底)", agent_id.c_str(), base.c_str());
    int check_rc = check_alias_exists(base);
    if (check_rc == 0) {
      {  // 并发保护（P3-5）：alias_map_ 写入加锁
        std::lock_guard<std::mutex> lock(mutex_);
        alias_map_[agent_id] = base;  // 降级复用基础模板（有资产缺失风险，但链路可用）
      }
      return true;
    }
    return false;
  }

  Logger::instance().info_f(
      "[TEMPLATE] snapshot ready agent=%s snapshot=%s", agent_id.c_str(), snap_id.c_str());

  // 回收旧快照（防膨胀，设计 §2.6.4 I-A5：每 agent 至多保留 1 个）：先建后删
  // 旧快照 = 持久化记录中的旧 snapshotID（与新建不同才删；构建失败保留旧 → I-A6）
  if (has_rec && !rec.snapshot.empty() && rec.snapshot != snap_id) {
    int rc = delete_template(rec.snapshot);
    if (rc == 0) {
      Logger::instance().info_f(
          "[TEMPLATE] recycled old snapshot agent=%s old=%s -> new=%s",
          agent_id.c_str(), rec.snapshot.c_str(), snap_id.c_str());
    } else {
      Logger::instance().warn_f(
          "[TEMPLATE] recycle old snapshot FAILED agent=%s old=%s rc=%d (保留旧，人工清理)",
          agent_id.c_str(), rec.snapshot.c_str(), rc);
    }
  }

  // 记录映射（内存 + 持久化）——并发保护（P3-5）：写入加锁；
  // save_persist_map 内部自锁，须在锁外调用（std::mutex 不可重入）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    alias_map_[agent_id] = snap_id;
    persist_map_[agent_id] = {snap_id, cur_version, cur_hash};
  }
  save_persist_map();
  return true;
}

}  // namespace daemon
}  // namespace agentflow
