// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxFactory 实现（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T013：按 mode 构造 Provider，v1 支持 none/local

#include "sandbox/sandbox_factory.h"

#include "sandbox/sandbox_docker.h"
#include "sandbox/sandbox_e2b.h"
#include "sandbox/sandbox_local.h"

namespace agentflow {
namespace sandbox {

// 注册表实例
std::unordered_map<std::string, SandboxFactory::FactoryFn>& SandboxFactory::registry() {
  static std::unordered_map<std::string, FactoryFn> reg;
  return reg;
}

void SandboxFactory::register_provider(const std::string& mode, FactoryFn fn) {
  registry()[mode] = std::move(fn);
}

std::unique_ptr<Sandbox> SandboxFactory::create(const std::string& mode,
                                                  const SandboxKey& owner,
                                                  const std::string& workdir_root,
                                                  const std::string& sandbox_http_server_path,
                                                  int sandbox_http_port,
                                                  int64_t output_max_bytes,
                                                  const std::string& skills_src_dir,
                                                  const std::string& e2b_remote_workspace_dir,
                                                  const std::string& image_name,
                                                  const std::string& e2b_api_url,
                                                  const std::string& e2b_api_key,
                                                  const std::string& e2b_default_template,
                                                  const std::string& e2b_network_allow_out,
                                                  const std::string& e2b_ca_cert_path) {
  if (mode == "none") {
    return nullptr;  // 向后兼容（FR-006）
  }

  // 检查注册表
  auto& reg = registry();
  auto it = reg.find(mode);
  if (it != reg.end()) {
    return it->second(owner, workdir_root, sandbox_http_server_path,
                      sandbox_http_port, output_max_bytes, skills_src_dir,
                      e2b_remote_workspace_dir);
  }

  // 内建 Provider
  if (mode == "local") {
    return std::make_unique<LocalSandbox>(owner, workdir_root,
                                          sandbox_http_server_path,
                                          sandbox_http_port, output_max_bytes);
  }
  if (mode == "docker") {
    return std::make_unique<DockerSandbox>(owner, workdir_root,
                                           sandbox_http_server_path,
                                           sandbox_http_port, output_max_bytes,
                                           skills_src_dir, image_name);
  }
  if (mode == "e2b") {
    return std::make_unique<E2BSandbox>(owner, workdir_root,
                                        sandbox_http_server_path,
                                        sandbox_http_port, output_max_bytes,
                                        skills_src_dir,
                                        e2b_remote_workspace_dir,
                                        e2b_api_url,
                                        e2b_api_key,
                                        e2b_default_template,
                                        e2b_network_allow_out,
                                        e2b_ca_cert_path);
  }

  throw SandboxLaunchException("Unknown sandbox mode: " + mode);
}

}  // namespace sandbox
}  // namespace agentflow