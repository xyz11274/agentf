// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// LocalSandbox——进程级沙箱实现（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T022 接口契约：subprocess.Popen 启 sandbox_http_server.py，popen(curl) 调 POST /exec

#ifndef AGENTFLOW_SANDBOX_LOCAL_H
#define AGENTFLOW_SANDBOX_LOCAL_H

#include <memory>
#include <string>

#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"

namespace agentflow {
namespace sandbox {

// LocalSandbox：宿主机侧 subprocess.Popen 启 sandbox_http_server.py
// HTTP client 方案：popen("curl ...")（与 exe_loader.cpp 既有 popen 模式一致，F5 单一真相来源）
// 生命周期：构造时拉起 server，destroy() 时 terminate 进程（幂等，B7）
class LocalSandbox : public Sandbox {
 public:
  // 构造——拉起 sandbox_http_server.py，bind 0 拿 endpoint
  // 前置：sandbox_http_server_path 指向宿主机侧 sandbox_http_server.py（plan.md F5：LocalSandbox 用宿主机侧绝对路径）
  //       workdir_root 可写
  // 异常：SandboxLaunchException 含可定位原因（Popen 失败/server 启动超时 5s/endpoint 不可达）—— 不静默 fallback（B22）
  LocalSandbox(const SandboxKey& owner,
               const std::string& workdir_root,
               const std::string& sandbox_http_server_path,
               int sandbox_http_port,
               int64_t output_max_bytes);

  ~LocalSandbox() override;

  // exec：popen("curl -s -X POST .../exec") 调沙箱内 server，解析 ExecResult JSON
  // 前置：state() == RUNNING
  // 异常：endpoint 不可达 → ExecResult{exit_code=1, stderr="Sandbox endpoint unreachable"}
  ExecResult exec(const std::string& tool,
                  const std::string& tool_params_json,
                  const ExecOptions& opts) override;

  std::string id() const override { return sandbox_id_; }
  void destroy() override;  // 幂等，B7
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
  SandboxState  state_;
  int64_t       output_max_bytes_;
  int64_t       last_active_at_ = 0;  // FR-009：最近活跃时间戳（ms），构造时初始化
  pid_t         server_pid_;  // sandbox_http_server.py 子进程 PID，<0 表示未启动/已销毁

  // 拉起 sandbox_http_server.py 子进程，返回分配的端口号
  // 异常：SandboxLaunchException
  int launch_http_server(const std::string& sandbox_http_server_path, int port);

  // 等待 server 启动并探测 endpoint 可达（超时 5s）
  // 异常：SandboxLaunchException("timeout") / SandboxLaunchException("endpoint unreachable")
  void wait_for_server_ready(int port, int timeout_ms = 5000);

  // popen(curl) 调沙箱内 server POST /exec，返回原始 JSON 字符串
  std::string curl_post(const std::string& path, const std::string& body_json, int64_t timeout_ms);
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_LOCAL_H
