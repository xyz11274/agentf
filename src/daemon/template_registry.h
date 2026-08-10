// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// TemplateRegistry——按 agent_id 寻址已预建的 e2b template（v2 段1 预热）
// 对齐 docs/design/features/sandbox/sandbox-v2.md §2.2 组件契约：
//   - 寻址键：agent_id（单键，agent 级）——load agent 时无 user/client（部署期动作）
//   - 时机：部署期（load agent 触发 preload），与 SessionManager（会话期）并行
//   - 仅 e2b 模式有效；Local/Docker/none 模式 preload/resolve 均 no-op（无 template 概念）
//   - 不拆 SandboxKey：template 预热用 agent_id 单键，不侵入 SessionManager 三元组寻址
//
// CubeSandbox 适配（2026-08-09，用户拍板方向 A）：当前部署是 CubeSandbox，
// 模板 ID 由平台自动生成（tpl-xxx），无 alias 概念，SDK `Template.build`/`alias_exists`
// 端点不实现（404）。preload 语义从"自动 build 模板"调整为"确认平台已有 READY 模板
// 并复用"——REST 幂等检查 `GET /templates/{templateID}`（200=存在且 READY）。
//
// agent 级快照关联（2026-08-09，设计 sandbox-warm-template-oci.md §2.6）：
//   快照 = 基础模板 + agent package（manifest includes 全量）。每 agent 一个快照，
//   TemplateRegistry 按 agent_id 解析快照；agent load 时快照不存在则自动构建
//   （调 template_snapshot_create.py --manifest）；version/hash 变化 → 重建新快照
//   → 回收旧快照（DELETE /templates/{旧 snapshotID}）。

#ifndef AGENTFLOW_DAEMON_TEMPLATE_REGISTRY_H
#define AGENTFLOW_DAEMON_TEMPLATE_REGISTRY_H

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "daemon/config.h"

namespace agentflow {
namespace daemon {

// TemplateRegistry：按 agent_id 寻址已预建的 e2b template
// 与 SessionManager 并行：SessionManager 按三元组找沙箱实例，TemplateRegistry 按 agent_id 找 template
// v2 改动 #8 简化：内存映射 + preload 时 REST 幂等检查，持久化留后续（用户 2026-08-08 拍板）
class TemplateRegistry {
 public:
  explicit TemplateRegistry(const daemon::DaemonConfig& config);

  // preload：按 agent_id 确认/构建 agent 级快照（幂等——内存映射命中则复用）
  // agent 级快照（2026-08-09）：快照 = 基础模板 + agent package（manifest includes 全量）。
  //   1. 内存映射命中 → 复用（幂等）
  //   2. 未命中 → 计算 agent 快照 key（version + manifest hash8）→ 平台 REST 检查
  //   3. 平台无该 agent 快照 → 自动构建（调 template_snapshot_create.py --manifest）
  //   4. 构建成功 → 记录映射；回收旧快照（DELETE /templates/{旧 snapshotID}，防膨胀）
  //   5. 构建失败 → 降级基础模板（不阻塞首问，execute 走 v1 兜底）
  // 参数：package_path = agent package 根目录（读 package.json version + manifest includes hash）
  // 仅 e2b 模式有效；Local/Docker/none 模式 no-op 返回 true
  // 返回 false = preload 失败（load RPC 降级 warning，execute 首问走 v1 懒加载兜底）
  // 失败信息含可定位要素：agent_id、snapshotID、构建/回收输出（M3）
  bool preload(const std::string& agent_id, const std::string& package_path);

  // resolve：解析 agent_id 对应的 template ID（未确认返回空串）
  // 段2 execute 首次 Sandbox.create(template=<resolve 结果>) 用
  std::string resolve(const std::string& agent_id) const;

 private:
  const daemon::DaemonConfig& config_;

  // agent_id -> templateID 内存映射（v2 改动 #8 简化：不持久化，daemon 重启后首次 load 重建）
  // 幂等检查兜底用 REST（平台侧是真相源，本映射只是缓存）
  std::unordered_map<std::string, std::string> alias_map_;

  // 调 e2b_template_build.py check --name <templateID>：exit 0=存在 1=不存在 2=SDK/REST 错误
  int check_alias_exists(const std::string& template_id);

  // 设置 e2b 子进程环境变量（E2B_API_KEY/E2B_API_URL/SSL_CERT_FILE），与 E2BSandbox 一致
  void set_e2b_env();

  // 调 e2b_template_build.py delete --template-id <templateID>：exit 0=删除成功/已不存在
  int delete_template(const std::string& template_id);

  // 调 template_snapshot_create.py --hash-manifest <manifest> 计算 includes 内容 hash8
  // 返回空串 = 计算失败（无 manifest / 解析错误）
  std::string compute_manifest_hash(const std::string& package_path);

  // 调 template_snapshot_create.py --base-template <base> --manifest <manifest> --name <snap_name>
  // 构建 agent 增量快照；成功返回平台 snapshotID（stdout 末行），失败返回空串
  std::string build_agent_snapshot(const std::string& agent_id, const std::string& package_path,
                                   const std::string& base_template);

  // 解析 agent package version（package.json version 字段）；无则返回 "0"
  std::string read_agent_version(const std::string& package_path);

  // 映射持久化文件路径（config.e2b_template_map_path，默认 data/template_registry.json）
  std::filesystem::path registry_persist_path() const;

  // 持久化映射加载/保存（防 daemon 重启后 agent 快照映射丢失 → 无法回收旧快照，AC-5）
  void load_persist_map();
  void save_persist_map();

  // agent 快照持久化记录（含 version/hash 用于 AC-4 版本变化检测）
  struct SnapshotRecord {
    std::string snapshot;  // 平台 snapshotID
    std::string version;   // agent package.json version（读不到 "0"）
    std::string hash;      // manifest includes 内容 hash8（无 manifest 空串）
  };

  // agent_id -> SnapshotRecord 持久化映射（data/template_registry.json，跨 daemon 重启）
  // 加载后同步进 alias_map_（复用）与 persist_map_（回收旧快照定位）
  std::unordered_map<std::string, SnapshotRecord> persist_map_;
  bool persist_loaded_ = false;

  // 并发保护（RPC server 每客户端一线程，agent.load 并发会进 preload）：
  // persist_map_/alias_map_/persist_loaded_ 读写 + set_e2b_env 的 setenv 需串行（review P3-5）
  mutable std::mutex mutex_;
};

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_TEMPLATE_REGISTRY_H
