// 编排执行证据：从标准事件还原真实派发、依赖关系与分支结果。
// 只统计实际执行，不用策略中的计划条目代替运行时回执。
// 回放与在线查询共用归因语义，防止异步失败在离线评测中丢失。

#pragma once

#include "local_support.h"
#include "runtime_policy.pb.h"
#include <map>
#include <set>

namespace trajectory::evolution::integration {

inline constexpr const char *kScheduleResource = "__evolution_resource";

// 只提取原文中的显式文件路径，不判断自然语言中的生产/消费关系。
// 关系仍由版本内的编排规则指定；有多个共享资源时不猜测配对。
inline std::set<std::string> schedule_resources(const Json &arguments) {
  std::set<std::string> result;
  if (!arguments.object() || arguments.fields().size() > 64)
    return result;
  auto path_char = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.' || c == '/';
  };
  auto separator = [](unsigned char c) {
    return c < 128 && (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\'' || c == '"' ||
                       c == '`' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
                       c == '}' || c == ',' || c == ';' || c == '=' || c == '<' || c == '>');
  };
  auto boundary = [&](const std::string &text, std::size_t offset, bool before) {
    if (before ? offset == 0 : offset == text.size())
      return true;
    if (separator(text[before ? offset - 1 : offset]))
      return true;
    // 中文标点可以分隔路径；中文目录名或文件名不能被截成 ASCII 尾段。
    for (const std::string mark :
         {"，", "。", "：", "；", "、", "（", "）", "【", "】", "“", "”", "‘", "’"}) {
      if (before && offset < mark.size())
        continue;
      if (text.compare(before ? offset - mark.size() : offset, mark.size(), mark) == 0)
        return true;
    }
    return false;
  };
  std::size_t bytes = 0;
  for (const auto &field : arguments.fields()) {
    if (field.first == kScheduleResource || !field.second.string())
      continue;
    const auto &text = field.second.text();
    if (text.size() > 8192 || bytes + text.size() > 32768)
      continue;
    bytes += text.size();
    if (text.find("://") != std::string::npos || text.find("<redacted") != std::string::npos ||
        text.find("[REDACTED") != std::string::npos || text.find("[redacted") != std::string::npos)
      continue;
    for (std::size_t start = 0; start < text.size() && result.size() < 16;) {
      if (!path_char(text[start])) {
        ++start;
        continue;
      }
      auto end = start + 1;
      while (end < text.size() && path_char(text[end]))
        ++end;
      auto token = text.substr(start, end - start);
      while (!token.empty() && token.back() == '.')
        token.pop_back();
      const auto slash = token.rfind('/'), dot = token.rfind('.');
      // 不截取 URL、邮件或单词内部的局部路径；仅接受带扩展名的文件路径。
      if (token.size() <= 512 && slash != std::string::npos && dot != std::string::npos &&
          dot > slash + 1 && dot + 1 < token.size() && token.size() - dot <= 13 &&
          token.rfind("//", 0) != 0 && token.find("/../") == std::string::npos &&
          boundary(text, start, true) && boundary(text, end, false))
        result.insert(std::move(token));
      start = end;
    }
  }
  return result;
}

// 运行时与历史回放共用同一投影。只保留同批动作共享的短字符串，
// 不把整段提示词/请求历史放进调度查询，也不凭语言猜测资源依赖。
inline void project_schedule_evidence(v1::RuntimeQuery &query, const Json &input) {
  if (query.boundary() != "schedule" || !input["actions"].array())
    return;
  std::map<std::string, std::set<std::string>> owners;
  std::map<std::string, std::set<std::string>> resource_owners, resources;
  std::map<std::string, const Json *> actions;
  for (const auto &action : input["actions"].items()) {
    if (!action["arguments"].object() || action["arguments"].fields().size() > 64)
      continue;
    actions[action.str("action_id")] = &action;
    auto &paths = resources[action.str("action_id")];
    paths = schedule_resources(action["arguments"]);
    for (const auto &path : paths)
      resource_owners[path].insert(action.str("action_id"));
    for (const auto &field : action["arguments"].fields())
      if (field.second.string() && !field.second.text().empty() &&
          field.second.text().size() <= 512)
        owners[field.second.text()].insert(action.str("action_id"));
  }
  for (auto &action : *query.mutable_actions()) {
    const auto found = actions.find(action.action_id());
    if (found == actions.end() || found->second->str("operation") != action.operation())
      continue;
    action.clear_arguments();
    for (const auto &field : (*found->second)["arguments"].fields()) {
      if (action.arguments().fields_size() >= 8)
        break;
      if (field.first == kScheduleResource || !field.second.string() ||
          field.second.text().empty() || field.second.text().size() > 512 ||
          field.second.text().find("<redacted") != std::string::npos ||
          field.second.text().find("[REDACTED") != std::string::npos ||
          field.second.text().find("[redacted") != std::string::npos ||
          owners[field.second.text()].size() < 2)
        continue;
      (*action.mutable_arguments()->mutable_fields())[field.first].set_string_value(
          field.second.text());
    }
    std::string shared;
    std::size_t matches = 0;
    for (const auto &path : resources[action.action_id()])
      if (resource_owners[path].size() > 1) {
        shared = path;
        ++matches;
      }
    if (matches == 1)
      (*action.mutable_arguments()->mutable_fields())[kScheduleResource].set_string_value(shared);
  }
}

} // 命名空间结束：trajectory::evolution::integration
