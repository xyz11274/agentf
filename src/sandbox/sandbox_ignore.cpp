// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// IgnoreMatcher 实现——跳过 dotfile（AgentFoundry 沙箱机制移植）

#include "sandbox/sandbox_ignore.h"

#include <filesystem>
#include <string>

namespace agentflow {
namespace sandbox {

namespace fs = std::filesystem;

bool IgnoreMatcher::starts_with_dot(const std::string& name) {
  return !name.empty() && name[0] == '.';
}

bool IgnoreMatcher::should_ignore(const std::string& relative_path) {
  // 跳过 dotfile（文件名以 . 开头）
  auto name = fs::path(relative_path).filename().string();
  if (starts_with_dot(name)) return true;

  // 跳过 .git 目录
  if (relative_path.find("/.git/") != std::string::npos ||
      relative_path == ".git") return true;

  // 跳过 __pycache__ 和 .pyc
  if (relative_path.find("__pycache__") != std::string::npos) return true;
  if (relative_path.size() > 4 &&
      relative_path.substr(relative_path.size() - 4) == ".pyc") return true;

  return false;
}

}  // namespace sandbox
}  // namespace agentflow