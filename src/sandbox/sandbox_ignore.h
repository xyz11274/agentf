// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// IgnoreMatcher——跳过 dotfile（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T061 + 文件结构：Create src/sandbox/sandbox_ignore.h/.cpp
// 跳过规则：文件名以 . 开头、路径含 /.git/、系统文件（__pycache__/*.pyc）

#ifndef AGENTFLOW_SANDBOX_IGNORE_H
#define AGENTFLOW_SANDBOX_IGNORE_H

#include <string>

namespace agentflow {
namespace sandbox {

// IgnoreMatcher——跳过 dotfile（对齐 AgentFoundry 的 gitignore 类似约定）
// 跳过规则：文件名以 . 开头、路径含 /.git/、系统文件（__pycache__/*.pyc）
class IgnoreMatcher {
 public:
  // 判断路径是否应跳过（true = 跳过，不属于交付物）
  static bool should_ignore(const std::string& relative_path);

 private:
  static bool starts_with_dot(const std::string& name);
};

}  // namespace sandbox
}  // namespace agentflow

#endif  // AGENTFLOW_SANDBOX_IGNORE_H