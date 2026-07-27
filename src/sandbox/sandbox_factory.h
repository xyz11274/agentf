// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxFactory + Registry（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T013 接口契约

#ifndef AGENTFLOW_SANDBOX_FACTORY_H
#define AGENTFLOW_SANDBOX_FACTORY_H

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"

namespace agentflow {
namespace sandbox {

// 按 sandbox-mode 字符串构造 Provider
// v1 支持 none/local，docker/e2b 在 Phase 7 注册
class SandboxFactory {
 public:
  // 构造沙箱
  // 前置：mode ∈ {none, local, docker, e2b}
  // 后置：返回的 Sandbox 已拉起（endpoint 可达）或抛含失败原因的异常
  // 异常：mode=none 返回 nullptr（向后兼容，FR-006）
  //       拉起失败抛 SandboxLaunchException(reason) —— 不静默 fallback（B22）
  // config 由调用方传入 DaemonConfig，工厂按需提取 sandbox_config 子段
  // skills_src_dir: 宿主机上 agent package 的 harness/skills 路径（DockerSandbox/E2B 需要）
  // e2b_remote_workspace_dir: E2B 远端沙箱内 workspace 挂载点（mode=e2b 时生效）
  // image_name: Docker/E2B 沙箱镜像标签（默认 "agentflow-sandbox:v1"）
  // e2b_api_url: E2B API 服务端点 URL（mode=e2b 时生效，可选）
  // e2b_api_key: E2B API 鉴权密钥（mode=e2b 时生效，可选，未设时回退到环境变量 E2B_API_KEY）
  // e2b_default_template: E2B 沙箱模板 ID（mode=e2b 时生效，默认 "base"）
  // e2b_network_allow_out: E2B 沙箱网络出站 CIDR 列表（mode=e2b 时生效，可选）
  // e2b_ca_cert_path: E2B CA 证书路径（mode=e2b 时生效，设为 SSL_CERT_FILE 传给子进程，可选）
  static std::unique_ptr<Sandbox> create(const std::string& mode,
                                        const SandboxKey& owner,
                                        const std::string& workdir_root,
                                        const std::string& sandbox_http_server_path,
                                        int sandbox_http_port,
                                        int64_t output_max_bytes,
                                        const std::string& skills_src_dir = "",
                                        const std::string& e2b_remote_workspace_dir = "/home/user/workspace",
                                        const std::string& image_name = "agentflow-sandbox:v1",
                                        const std::string& e2b_api_url = "",
                                        const std::string& e2b_api_key = "",
                                        const std::string& e2b_default_template = "base",
                                        const std::string& e2b_network_allow_out = "",
                                        const std::string& e2b_ca_cert_path = "");

  // 注册 Provider 构造函数（供 Phase 7 docker/e2b 注册）
  using FactoryFn = std::function<std::unique_ptr<Sandbox>(
      const SandboxKey&, const std::string&, const std::string&, int, int64_t,
      const std::string&, const std::string&)>;
  static void register_provider(const std::string& mode, FactoryFn fn);

  // SandboxLaunchException：拉起失败含可定位原因（M3：错误含可定位信息）
  class SandboxLaunchException : public std::runtime_error {
   public:
    explicit SandboxLaunchException(const std::string& reason)
        : std::runtime_error("Sandbox launch failed: " + reason), reason_(reason) {}
    const std::string& reason() const { return reason_; }

   private:
    std::string reason_;
  };

 private:
  static std::unordered_map<std::string, FactoryFn>& registry();
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_FACTORY_H
