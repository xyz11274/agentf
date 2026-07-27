// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SandboxHelpers 实现——工具类 helper（AgentFoundry 沙箱机制移植）
// 对齐 lld.md T042：为每个工具构造 tool_params JSON，统一调 Sandbox::exec

#include "sandbox/sandbox_helpers.h"

#include <cstdlib>
#include <sstream>
#include <string>

#include <rapidjson/document.h>

namespace agentflow {
namespace sandbox {

// 简化 JSON 字符串转义（生产实现可用 rapidjson，此处为 MVP 范围）
static std::string escape_json(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

ExecResult SandboxHelpers::dispatch(Sandbox& sandbox, const std::string& tool,
                                    const std::string& tool_params_json, const ExecOptions& opts) {
  // 前置：sandbox.state() == RUNNING（由 Sandbox::exec 内部校验）
  return sandbox.exec(tool, tool_params_json, opts);
}

ExecResult SandboxHelpers::bash(Sandbox& sandbox, const std::string& command, const ExecOptions& opts) {
  std::string params = "{\"command\":\"" + escape_json(command) + "\"}";
  return dispatch(sandbox, "bash", params, opts);
}

// 其余工具方法（read_file/write_file/edit_file/list_skills/load_skill/grep/glob）
// 已迁移到 Python 侧 skill_executor → HTTPExecutor → POST /exec 路径，
// C++ SandboxHelpers 不再提供，详见 contracts/sandbox-http-server.md。

std::string SandboxHelpers::validate_upload_path(const std::string& path) {
  // 接受格式：workspace/{user_id}_{client_id}/upload/{filename}
  // 1) 必须以 workspace/ 开头
  if (path.find("workspace/") != 0) {
    return "PATH_NOT_IN_UPLOAD_DIR: path must start with "
           "'workspace/{session}/upload/', got: " + path;
  }
  // 2) 去掉 workspace/ 前缀后，剩余部分: {session}/upload/{filename}
  std::string rest = path.substr(strlen("workspace/"));
  auto upload_pos = rest.find("/upload/");
  if (upload_pos == std::string::npos || upload_pos == 0) {
    return "PATH_NOT_IN_UPLOAD_DIR: path must contain a non-empty session "
           "directory before '/upload/', got: " + path;
  }
  return "";  // 合法
}

ExecResult SandboxHelpers::parse_exec_result_json(const std::string& resp, const std::string& endpoint) {
  ExecResult result;
  result.exit_code = 1;
  result.stdout = "";
  result.stderr = "";
  result.truncated = false;
  result.duration_ms = 0;

  if (resp.empty()) {
    result.stderr = "Sandbox endpoint unreachable: " + endpoint;
    return result;
  }

  // 用 rapidjson 解析 JSON（对齐 ExecResult 结构化契约：sandbox_types.h + contracts/sandbox-http-server.md）
  // 替代手动字符串搜索——旧版依赖字符串长度推算（pos+12/pos+10 等魔数），改字段名就要改 5 处
  rapidjson::Document doc;
  doc.Parse(resp.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    result.stderr = resp;  // 无法解析，返回原始响应
    return result;
  }

  if (doc.HasMember("exit_code") && doc["exit_code"].IsInt()) {
    result.exit_code = doc["exit_code"].GetInt();
  } else {
    result.stderr = resp;  // 缺少 exit_code 字段，无法确认执行结果
    return result;
  }

  if (doc.HasMember("stdout") && doc["stdout"].IsString()) {
    result.stdout = doc["stdout"].GetString();
  }
  if (doc.HasMember("stderr") && doc["stderr"].IsString()) {
    result.stderr = doc["stderr"].GetString();
  }
  if (doc.HasMember("truncated") && doc["truncated"].IsBool()) {
    result.truncated = doc["truncated"].GetBool();
  }
  if (doc.HasMember("duration_ms") && doc["duration_ms"].IsInt64()) {
    result.duration_ms = doc["duration_ms"].GetInt64();
  }

  return result;
}

}  // namespace sandbox
}  // namespace agentflow
