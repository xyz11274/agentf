// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Sandbox 抽象接口（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T012 接口契约 + data-model.md Sandbox 实体

#ifndef AGENTFLOW_SANDBOX_H
#define AGENTFLOW_SANDBOX_H

#include <chrono>
#include <string>

#include "sandbox/sandbox_types.h"  // NOLINT：src/sandbox/ 头文件引用约定

namespace agentflow {
namespace sandbox {

// Sandbox 抽象接口：3 方法 MCP-like exec + id + destroy
// 三个实现（LocalSandbox/DockerSandbox/E2BSandbox）各自持有拉起资源
// 对齐 contracts/sandbox-http-server.md POST /exec 契约
class Sandbox {
 public:
  virtual ~Sandbox() = default;

  // 命令执行：通过沙箱内 HTTP server POST /exec
  // 前置：state() == RUNNING
  // 后置：返回 ExecResult；超时从 opts.timeout_ms 读取（默认 30s）
  virtual ExecResult exec(const std::string& tool,
                          const std::string& tool_params_json,
                          const ExecOptions& opts) = 0;

  // 沙箱唯一标识（I-5：sandbox_id 唯一寻址）
  virtual std::string id() const = 0;

  // 销毁沙箱（B7：幂等，多次调用一致）
  // 后置：state() == RECYCLING（终态不可逆）或已清理；进程/socket 已清理
  virtual void destroy() = 0;

  // 沙箱内 HTTP server 的可达 endpoint（B16：daemon 拉起后持有，注入给 workflow node）
  virtual std::string endpoint() const = 0;

  // 当前状态（B21：RUNNING↔STOPPED 可逆，RECYCLING 终态不可逆）
  virtual SandboxState state() const = 0;

  // 最近活跃时间戳（ms，epoch steady_clock，FR-009 GC 判定依据）
  // GC 线程检查 now_ms - last_active_at() > gc_destroy_ttl_ms 决定回收
  virtual int64_t last_active_at() const = 0;

  // 标记活跃（FR-009）：每次 exec() 调用后更新 last_active_at
  // GC 根据 last_active_at 判断闲置超时，只回收长时间无 exec 的沙箱
  virtual void mark_active() = 0;

  // 创建快照（FR-012）：各 Provider 自行实现快照逻辑
  // LocalSandbox/DockerSandbox：由 SandboxLifecycle 通过 ArtifactStore 处理，此处为 no-op
  // E2BSandbox：调用 E2B SDK sb.snapshot(filesystem_only=True) 在平台侧保存磁盘状态
  // 前置：state() == RUNNING
  // 后置：快照已保存，具体存储介质由 Provider 决定
  virtual void snapshot() = 0;

  // 持有者身份（B6：按 SandboxKey 复用）
  virtual const SandboxKey& owner() const = 0;

  // 工作区绝对路径（B4：路径双检的 allowed_prefix）
  //
  // 语义说明（三种 mode 共用同一 workspace 契约）：
  //   - LocalSandbox：宿主机侧 workdir，进程 chdir 到这里直接访问
  //   - DockerSandbox：宿主机侧 workdir，docker -v 挂载到容器内 /workdir
  //   - E2BSandbox：宿主机侧 workdir（用于待上传/已下载文件暂存），
  //                 远端 E2B 沙箱内另有独立 workspace（通过 e2b_sync.py 同步）
  //
  // 返回的是**宿主机侧绝对路径**，沙箱内文件访问通过 endpoint HTTP 调用
  // （HTTP POST /exec 走 MCP-like 入口，FR-015）
  //
  // 跨机器只是一个 mode 切换：none → local → docker → e2b
  // 隔离强度递增，工具调用方式完全一致
  virtual const std::string& workdir() const = 0;

  // 同步会话级用户上传目录到沙箱（agent-package-structure.md §2.2/§3.2）
  //
  // session_upload_dir 是宿主机侧绝对路径：
  //   {agent_packages_path}/{agent_id}/workspace/{user_id}_{client_id}/upload/
  //
  // 默认 no-op：LocalSandbox/DockerSandbox 与宿主机共享 workdir，无需同步。
  // E2BSandbox override：调 e2b_sync.py upload 把该目录增量同步到远端沙箱。
  //
  // 调用时机：daemon handle_agent_execute 创建/复用沙箱后、route_request_message 前
  // 失败处理：抛 std::runtime_error 含可定位信息，daemon 捕获后记 warning，不拒绝请求
  virtual void sync_session_uploads(const std::string& session_upload_dir) { (void)session_upload_dir; }

  // 沙箱模式标识（用于 SandboxSnapshots::restore 分支判断）
  // 默认返回空字符串（Local/Docker）；E2BSandbox override 返回 "e2b"
  virtual std::string sandbox_mode() const { return ""; }
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_H
