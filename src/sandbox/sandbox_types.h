// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Sandbox 类型定义（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T010 接口契约 + data-model.md ExecResult/SandboxOptions 实体

#ifndef AGENTFLOW_SANDBOX_TYPES_H
#define AGENTFLOW_SANDBOX_TYPES_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace agentflow {
namespace sandbox {

// 沙箱状态枚举（对齐 data-model.md Sandbox.state）
// RUNNING↔STOPPED 可逆（暂停/恢复），RECYCLING 终态不可逆（Q3 澄清）
enum class SandboxState {
  RUNNING,    // 活跃，可 exec
  STOPPED,    // 暂停，可恢复到 RUNNING
  RECYCLING,  // 已回收终态，不可逆，需新建沙箱从快照恢复
};

// 命令执行结果（对齐 data-model.md ExecResult + contracts/sandbox-http-server.md 响应体）
// 跨 C++/Python/HTTP 边界契约：JSON 序列化时字段名必须一致
struct ExecResult {
  int32_t     exit_code;      // 0-255，0=成功
  std::string stdout;         // 命令 stdout（截断后）
  std::string stderr;         // 命令 stderr（截断后）
  bool        truncated;      // 默认 false；true 当且仅当 stdout/stderr 发生截断（I-6）
  int64_t     duration_ms;    // ≥0，命令耗时

  ExecResult() : exit_code(0), truncated(false), duration_ms(0) {}
};

// 命令执行选项（对齐 contracts/sandbox-http-server.md opts）
struct ExecOptions {
  int64_t                         timeout_ms;   // >0，可选（默认 30s 由 LocalSandbox 补）
  std::map<std::string, std::string> env_vars;  // 可选，沙箱内 env 覆盖
  std::string                     working_dir;  // 可选，沙箱内工作目录（默认 workdir）

  ExecOptions() : timeout_ms(0) {}
};

// grep 工具返回的匹配项（对齐 data-model.md GrepMatch）
struct GrepMatch {
  std::string file;          // 相对 workdir 的文件路径
  int32_t     line_number;   // 1-based
  std::string line_content;  // 匹配行内容

  GrepMatch() : line_number(0) {}
};

// SandboxOptions：exe_loader 注入的安全层（对齐 data-model.md SandboxOptions）
// nullptr（std::optional 或裸指针）时保持原逻辑不变（I-5/FR-006 向后兼容）
struct SandboxOptions {
  std::vector<std::string> env_whitelist;   // 默认含 PATH/HOME，拒绝 *_API_KEY（B3）
  std::string              path_prefix;     // 沙箱 workdir 绝对路径，双检边界（B4）
  int64_t                  output_max_bytes;  // 默认 0=不截断（向后兼容）

  SandboxOptions() : output_max_bytes(0) {
    // 默认白名单（对齐 FR-013：env 等于宿主侧过滤后白名单）
    env_whitelist = {"PATH", "HOME", "USER", "LANG", "LC_ALL", "TMPDIR", "TMP", "TEMP"};
  }
};

// SandboxKey：按用户身份查找/创建沙箱的唯一复用键（对齐 data-model.md SandboxKey）
// 三元组匹配——遗漏任一字段会导致串台（B6/I-5）
struct SandboxKey {
  std::string user_id;     // 用户身份
  std::string client_id;   // 客户端身份
  std::string agent_id;    // agent 身份

  bool operator==(const SandboxKey& other) const {
    return user_id == other.user_id && client_id == other.client_id && agent_id == other.agent_id;
  }

  // 三元组合序列化，用于 ArtifactStore 快照目录命名（对齐 08-实现方案.md §3.4.3）
  std::string to_string() const {
    return user_id + ":" + client_id + ":" + agent_id;
  }
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_TYPES_H
