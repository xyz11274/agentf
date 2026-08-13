// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxHelpers——工具类 helper（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T042 接口契约：为每个工具构造 tool_params，统一调 Sandbox::exec

#ifndef AGENTFLOW_SANDBOX_HELPERS_H
#define AGENTFLOW_SANDBOX_HELPERS_H

#include <string>
#include <vector>

#include "sandbox/sandbox.h"
#include "sandbox/sandbox_types.h"

namespace agentflow {
namespace sandbox {

// 子进程执行结果（与 sandbox_types.h 的 ExecResult 撞名，此为进程执行专用）
struct SubprocessResult {
  std::string output;
  int exit_code;  // -1 = pipe/fork 失败（父进程侧资源错）；127 = execvp 失败
};

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

  // rewrite_upload_path_to_sandbox：把 user message 中的图片相对路径改写为沙箱内绝对路径
  //
  // 背景（agent-package-structure.md §3.6 I-7 路径注入语义，2026-08-12 修复）：
  //   Bridge content_serializer 注入的是相对路径 workspace/{user_id}_{client_id}/upload/{file}，
  //   LLM 拿它拼沙箱绝对路径时把 "workspace" 前缀误当沙箱根
  //   （/home/user/workspace 最后一节也叫 workspace），拼成 {prefix}/{session}/upload/...
  //   （少拼一层 workspace/）→ NO_IMAGE。正确沙箱路径是 {prefix}/workspace/{session}/upload/...。
  //   daemon handle_agent_execute 下发 user message 前调用本函数，
  //   e2b 模式把相对路径改写为沙箱内绝对路径，让 LLM 拿到的路径无歧义。
  //
  // 参数：
  //   content        — user message 文本（含 Bridge 注入的图片相对路径）
  //   session_marker — "workspace/{user_id}_{client_id}/upload/"（相对路径前缀）
  //   ws_prefix      — 沙箱内 workspace 根（e2b=/home/user/workspace；空串=不改写，Local/Docker）
  //
  // 返回：改写后的文本；ws_prefix 为空或 content 无匹配时原样返回。
  static std::string rewrite_upload_path_to_sandbox(const std::string& content,
                                                    const std::string& session_marker,
                                                    const std::string& ws_prefix);

  // ── 子进程执行（F5 单一真相来源：E2BSandbox / TemplateRegistry / e2b 子进程调用共用）──

  // resolve_python3：探测 python3 绝对路径（lazy + 缓存）
  // 探测序：PATH which python3 → 常见绝对路径候选 → 裸 "python3" 兜底
  static const std::string& resolve_python3();

  // exec_cmd_argv：execvp 形式执行 argv（不经 shell，防 CWE-78 注入 + CWE-522 凭据暴露）
  // stderr 合并到 stdout；返回 {output, exit_code}。上层 MUST 检 exit_code != 0 → 失败
  // timeout_ms：子进程执行超时（毫秒），<=0 表示不限时（默认，向后兼容）。
  // 超时后 kill 子进程并返回 exit_code=124（与 timeout 命令约定一致），
  // 防止 daemon 启动路径被挂起的子进程（如 E2B snapshot 重建）无限阻塞。
  static SubprocessResult exec_cmd_argv(const std::vector<std::string>& argv_strs,
                                        int timeout_ms = 0);

 private:
  // 构造 tool_params JSON 字符串并调 sandbox.exec（统一入口，B14 统一 MCP-like）
  static ExecResult dispatch(Sandbox& sandbox, const std::string& tool,
                             const std::string& tool_params_json, const ExecOptions& opts);
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_HELPERS_H
