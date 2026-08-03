// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// E2BSandbox——远程沙箱实现（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T075 接口契约：E2B SDK 拉起远程沙箱 + 公共 URL + token 鉴权
// research.md Decision 1：公共 URL + token 鉴权，非监听姿态隔离
// FR-022：allow_public_traffic=false + traffic_access_token 鉴权

#ifndef AGENTFLOW_SANDBOX_E2B_H
#define AGENTFLOW_SANDBOX_E2B_H

#include <memory>
#include <string>

#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"

namespace agentflow {
namespace sandbox {

// E2BSandbox：E2B SDK 拉起远程沙箱
// exec 通过 HTTP POST 到公共 URL /exec，请求头带 Authorization: Bearer {traffic_access_token}（B17）
// destroy 时 sandbox.kill（幂等，B7）
// 前置：E2B_API_KEY（配置文件 e2b_api_key 或环境变量 E2B_API_KEY）
class E2BSandbox : public Sandbox {
 public:
  // 构造——E2B SDK 拉起远程沙箱
  // 前置：E2B_API_KEY（配置文件 e2b_api_key 或环境变量 E2B_API_KEY）
  // 异常：SandboxLaunchException 含可定位原因（E2B SDK 不可用/配额耗尽/网络不可达）
  //       B22：拉起失败不 fallback
  // skills_src_dir: 宿主机上 agent package 的 harness/skills 路径，用于同步到远端沙箱（可为空）
  // e2b_remote_workspace_dir: 远端沙箱内 workspace 挂载点（默认与 E2B 基础镜像一致）
  // e2b_api_url: E2B API 服务端点 URL（如 http://218.245.96.148:30030，可选）
  // e2b_api_key: E2B API 鉴权密钥（可选，未设时回退到环境变量 E2B_API_KEY）
  // e2b_default_template: E2B 沙箱模板 ID（如 tpl-cb6b9dcbf5784a1487a75971，默认 "base"）
  // e2b_network_allow_out: 沙箱出站网络 CIDR 列表（JSON 数组字符串，可选）
  // e2b_ca_cert_path: E2B CA 证书路径（自签名证书，设为 SSL_CERT_FILE 传给子进程，可选）
  E2BSandbox(const SandboxKey& owner,
             const std::string& workdir_root,
             const std::string& sandbox_http_server_path,
             int sandbox_http_port,
             int64_t output_max_bytes,
             const std::string& skills_src_dir = "",
             const std::string& e2b_remote_workspace_dir = "/home/user/workspace",
             const std::string& e2b_api_url = "",
             const std::string& e2b_api_key = "",
             const std::string& e2b_default_template = "base",
             const std::string& e2b_network_allow_out = "",
             const std::string& e2b_ca_cert_path = "");

  ~E2BSandbox() override;

  ExecResult exec(const std::string& tool,
                  const std::string& tool_params_json,
                  const ExecOptions& opts) override;

  std::string id() const override { return sandbox_id_; }
  void destroy() override;  // sandbox.kill（幂等，B7）
  std::string endpoint() const override { return endpoint_; }
  SandboxState state() const override { return state_; }
  const SandboxKey& owner() const override { return owner_; }
  const std::string& workdir() const override { return workdir_; }
  int64_t last_active_at() const override { return last_active_at_; }
  void mark_active() override {
    last_active_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void snapshot() override;  // FR-012：调用 e2b_lifecycle.py snapshot 保存远端磁盘状态

  // 沙箱模式标识——E2B（用于 SandboxSnapshots restore 分支判断）
  std::string sandbox_mode() const override { return "e2b"; }

  // 同步会话级用户上传目录到远端沙箱（agent-package-structure.md §2.2/§3.2）
  // 调 e2b_sync.py upload 增量同步 session_upload_dir 到 {remote_workspace_dir}/workspace/{session}/upload
  // 失败抛 std::runtime_error 含可定位信息
  void sync_session_uploads(const std::string& session_upload_dir) override;

  // ── 静态工厂：从源沙箱 fork 派生新沙箱（B3 restore 通道） ─────────────────
  //
  // 调 e2b_lifecycle.py fork 在 E2B 平台侧从源沙箱快照派生新沙箱。
  // 返回的沙箱已就绪（HTTP server 已在跑），不需要额外 Skills 同步
  //（filesystem 继承自源沙箱）。
  //
  // params 结构见 ForkParams（表驱动，避免长参数列）。
  struct ForkParams {
    SandboxKey    owner;
    std::string   workdir_root;           // 宿主机 workdir 根目录
    std::string   sandbox_http_server_path;  // sandbox_http_server.py 绝对路径
    int           sandbox_http_port;      // 沙箱内 HTTP server 端口
    int64_t       output_max_bytes;       // 输出截断阈值
    std::string   source_sandbox_id;      // 源沙箱 ID
    std::string   remote_workspace_dir;   // E2B 远端 workspace 挂载点
    std::string   e2b_api_url;            // E2B API 端点（可选）
    std::string   e2b_api_key;            // E2B API 密钥（可选）
    std::string   e2b_default_template;   // E2B 沙箱模板 ID（可选）
    std::string   e2b_network_allow_out;  // 出站网络 CIDR（可选）
    std::string   e2b_ca_cert_path;       // CA 证书路径（可选）
  };

  // 从源沙箱 fork 派生新沙箱。失败抛 SandboxLaunchException。
  // 调用方（SandboxSnapshots::restore_from_snapshot）负责从 config 构造 ForkParams。
  static std::unique_ptr<E2BSandbox> ForkFromSnapshot(const ForkParams& params);

 private:
  SandboxKey    owner_;
  std::string   sandbox_id_;
  std::string   workdir_;                 // 宿主机侧 workspace 根路径
  std::string   endpoint_;
  std::string   traffic_access_token_;    // daemon 生成的 token（FR-022）
  std::string   e2b_sync_script_path_;    // sandbox_http_server.py 路径（用于推导 e2b_sync.py 同级路径）
  std::string   skills_src_dir_;          // 宿主机 harness/skills 路径，用于同步到远端沙箱
  std::string   remote_workspace_dir_;    // E2B 远端 workspace 挂载点
  std::string   e2b_api_url_;             // E2B API 服务端点 URL
  std::string   e2b_api_key_;             // E2B API 鉴权密钥（优先于环境变量）
  std::string   e2b_default_template_;    // E2B 沙箱模板 ID
  std::string   e2b_network_allow_out_;   // E2B 沙箱出站网络 CIDR 列表
  std::string   e2b_ca_cert_path_;        // E2B CA 证书路径（SSL_CERT_FILE）
  int64_t       output_max_bytes_;
  int64_t       last_active_at_ = 0;  // FR-009：最近活跃时间戳（ms），构造时初始化
  SandboxState  state_;

  // 生成 traffic_access_token（简化版：UUID 风格）
  static std::string generate_token();

  // 私有构造器——不调 e2b_lifecycle.py create，直接注入已 fork 的结果（B3 restore 通道）
  // 仅 ForkFromSnapshot 静态工厂使用，绕过全量 create 流程
  E2BSandbox(const SandboxKey& owner,
             const std::string& workdir,              // 宿主机侧 workdir（绝对路径）
             const std::string& forked_sandbox_id,    // E2B 平台侧 sandbox_id（fork 结果）
             const std::string& forked_endpoint,      // host_url（fork 结果）
             const std::string& sandbox_http_server_path,
             int sandbox_http_port,
             int64_t output_max_bytes,
             const std::string& skills_src_dir,
             const std::string& e2b_remote_workspace_dir,
             const std::string& e2b_api_url,
             const std::string& e2b_api_key,
             const std::string& e2b_default_template,
             const std::string& e2b_network_allow_out,
             const std::string& e2b_ca_cert_path);

  // curl 带 token 鉴权 POST 到公共 URL
  std::string curl_post_with_token(const std::string& path, const std::string& body_json, int64_t timeout_ms);
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_E2B_H