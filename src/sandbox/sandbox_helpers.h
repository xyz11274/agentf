// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxHelpers——工具类 helper（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T042 接口契约：为每个工具构造 tool_params，统一调 Sandbox::exec

#ifndef AGENTFLOW_SANDBOX_HELPERS_H
#define AGENTFLOW_SANDBOX_HELPERS_H

#include <string>

#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"

namespace agentflow {
namespace sandbox {

// SandboxHelpers：静态工具方法集——构造 tool_params 调 Sandbox::exec
// 对齐 contracts/sandbox-http-server.md 的 tool 分发表
class SandboxHelpers {
 public:
  // bash：执行 shell 命令
  static ExecResult bash(Sandbox& sandbox, const std::string& command, const ExecOptions& opts);

  // 其余工具方法（read_file/write_file/edit_file/list_skills/load_skill/grep/glob）
  // 已迁移到 Python 侧 skill_executor → HTTPExecutor → POST /exec 路径，
  // C++ SandboxHelpers 不再提供，详见 contracts/sandbox-http-server.md。

  // parse_exec_result_json：解析沙箱 HTTP server 返回的 ExecResult JSON 响应
  // 输入：resp — 沙箱内 HTTP server POST /exec 返回的 JSON 字符串
  //       格式：{"exit_code":N,"stdout":"...","stderr":"...","truncated":bool,"duration_ms":N}
  // 输出：解析后的 ExecResult；若 resp 为空或无法解析，exit_code=1 且 stderr 含错误信息
  // 用途：被 LocalSandbox/DockerSandbox/E2BSandbox::exec 共用（F5 单一真相来源）
  static ExecResult parse_exec_result_json(const std::string& resp, const std::string& endpoint);

  // validate_upload_path：校验上传路径是否为合法的会话级路径
  // 接受格式：workspace/{user_id}_{client_id}/upload/{filename}
  // 返回空字符串表示合法，否则返回错误描述
  // 被 rpc_handler.cpp handle_sandbox_upload_file 调用（F5 单一真相来源）
  static std::string validate_upload_path(const std::string& path);

 private:
  // 构造 tool_params JSON 字符串并调 sandbox.exec（统一入口，B14 统一 MCP-like）
  static ExecResult dispatch(Sandbox& sandbox, const std::string& tool,
                             const std::string& tool_params_json, const ExecOptions& opts);
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_HELPERS_H
