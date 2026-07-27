// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DockerSandbox——容器级沙箱实现（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T073 接口契约：docker run -p 0:8080 启动镜像 + 端口映射拿 endpoint
// 与 E2BSandbox 共用同一 Dockerfile（B18 镜像共用）

#ifndef AGENTFLOW_SANDBOX_DOCKER_H
#define AGENTFLOW_SANDBOX_DOCKER_H

#include <memory>
#include <string>

#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"

namespace agentflow {
namespace sandbox {

// DockerSandbox：docker run 拉起容器 + 端口映射拿 endpoint
// exec 通过 HTTP POST 到 endpoint /exec（同 LocalSandbox 的 curl 方案）
// destroy 时 docker stop + docker rm（幂等，B7）
// 前置：Docker daemon 可用 + 镜像已构建（B18）
class DockerSandbox : public Sandbox {
 public:
  // 构造——docker run 拉起容器，端口映射 0:SANDBOX_HTTP_PORT
  // 前置：Docker daemon 可用
  // 异常：SandboxLaunchException 含可定位原因（Docker daemon 不可用/镜像未构建/拉取失败）
  //       B22：拉起失败不 fallback
  // skills_src_dir: 宿主机上 agent package 的 harness/skills 路径，构造时复制到 workdir
  DockerSandbox(const SandboxKey& owner,
               const std::string& workdir_root,
               const std::string& sandbox_http_server_path,
               int sandbox_http_port,
               int64_t output_max_bytes,
               const std::string& skills_src_dir = "",
               const std::string& image_name = "agentflow-sandbox:v1");

  ~DockerSandbox() override;

  ExecResult exec(const std::string& tool,
                  const std::string& tool_params_json,
                  const ExecOptions& opts) override;

  std::string id() const override { return sandbox_id_; }
  void destroy() override;  // docker stop + docker rm（幂等，B7）
  std::string endpoint() const override { return endpoint_; }
  SandboxState state() const override { return state_; }
  const SandboxKey& owner() const override { return owner_; }
  const std::string& workdir() const override { return workdir_; }
  int64_t last_active_at() const override { return last_active_at_; }
  void mark_active() override {
    last_active_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void snapshot() override {}  // FR-012：no-op，由 SandboxLifecycle 通过 ArtifactStore 处理

 private:
  SandboxKey    owner_;
  std::string   sandbox_id_;
  std::string   workdir_;
  std::string   endpoint_;
  std::string   container_id_;  // docker 容器 ID
  std::string   image_name_;
  std::string   skills_src_dir_;  // 宿主机 harness/skills 路径，构造时复制到 workdir
  int64_t       output_max_bytes_;
  int64_t       last_active_at_ = 0;  // FR-009：最近活跃时间戳（ms），构造时初始化
  SandboxState  state_;

  // docker run 拉起容器，返回 container_id + 映射端口
  // 用 popen("docker run -d -p 0:<port> ...") 解析 stdout 拿 container_id
  // 异常：SandboxLaunchException
  struct DockerRunResult {
    std::string container_id;
    int         host_port;
  };
  DockerRunResult run_container(int sandbox_http_port);
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_DOCKER_H