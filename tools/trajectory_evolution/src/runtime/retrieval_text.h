// 轻量文本检索辅助：规范化查询和经验文本，生成可用于匹配的特征。
// 文本匹配只是选择候选的证据之一，不等同于完整语义理解。
// 边界条件与作用域仍需独立检查，避免高频词造成跨任务过度注入。

#pragma once

#include "integration_json.h"

namespace trajectory::evolution::integration {

// 检索使用实际观测，不使用传输字段名或嵌入指导文字。
// 该投影不修改持久轨迹或实际模型输入。
inline void append_retrieval_text(const Json &value, std::string &result, unsigned depth = 0) {
  if (depth > 48 || value.null())
    return;
  if (value.string()) {
    auto text = value.text();
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
      return;
    if (text[begin] == '{' || text[begin] == '[' || text[begin] == '"') {
      try {
        const auto parsed = Json::parse(text);
        append_retrieval_text(parsed, result, depth + 1);
        return;
      } catch (const Error &) {
        // 结构化输入截断后不能退回匹配其字段名。
        const auto next = text.find_first_not_of(" \t\r\n", begin + 1);
        if (next != std::string::npos &&
            ((text[begin] == '{' && text[next] == '"') ||
             (text[begin] == '[' && (text[next] == '{' || text[next] == '['))))
          return;
      }
    }
    for (const auto *tag : {"system-reminder", "trajectory-experience", "trajectory-control"}) {
      const std::string open = "<" + std::string(tag), close = "</" + std::string(tag) + ">";
      std::size_t pos = 0;
      while ((pos = text.find(open, pos)) != std::string::npos) {
        const auto after = pos + open.size();
        if (after < text.size() && text[after] != '>' &&
            !std::isspace(static_cast<unsigned char>(text[after]))) {
          pos = after;
          continue;
        }
        const auto end = text.find(close, after);
        text.replace(pos, end == std::string::npos ? text.size() - pos : end + close.size() - pos,
                     " ");
      }
    }
    if (!text.empty()) {
      result += text;
      result += '\n';
    }
    return;
  }
  if (value.array()) {
    for (const auto &item : value.items())
      append_retrieval_text(item, result, depth + 1);
    return;
  }
  if (value.object()) {
    if (value["messages"].array()) {
      append_retrieval_text(value["messages"], result, depth + 1);
      return;
    }
    if (value["role"].string()) {
      const auto role = value["role"].text();
      if (role == "system" || role == "developer")
        return;
      if (role == "user" || role == "assistant" || role == "tool" || role == "function") {
        if (role == "tool" || role == "function")
          append_retrieval_text(value["name"], result, depth + 1);
        append_retrieval_text(value["content"], result, depth + 1);
        // 保留操作名和参数，不把调用编号或模式声明当作语义内容。
        if (value["tool_calls"].array())
          for (const auto &call : value["tool_calls"].items())
            append_retrieval_text(call["function"], result, depth + 1);
        append_retrieval_text(value["function_call"], result, depth + 1);
        return;
      }
    }
    for (const auto &item : value.fields())
      append_retrieval_text(item.second, result, depth + 1);
    return;
  }
  if (std::holds_alternative<double>(value.value)) {
    result += value.dump();
    result += '\n';
  }
}

// 从当前查询中提取供检索使用的文本视图，避免把无关历史重复拼入。
inline std::string retrieval_text(const Json &value) {
  std::string result;
  append_retrieval_text(value, result);
  return result;
}

// 任务检索只使用当前轮消息，避免上一轮助手的完成状态触发新请求的收尾经验。
// 仅识别已有 messages/role 契约；未知结构保持原投影，模型输入和轨迹不变。
inline std::string current_task_retrieval_text(const Json &value, unsigned depth = 0) {
  if (depth > 48)
    return {};
  if (value.string()) {
    const auto text = value.text();
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin != std::string::npos &&
        (text[begin] == '{' || text[begin] == '[' || text[begin] == '"')) {
      try {
        return current_task_retrieval_text(Json::parse(text), depth + 1);
      } catch (const Error &) { /* 非完整消息封装沿用原投影，不猜测回合边界。 */
      }
    }
    return retrieval_text(value);
  }
  const Json *messages = value.object() && value["messages"].array() ? &value["messages"]
                         : value.array()                             ? &value
                                                                     : nullptr;
  if (!messages)
    return retrieval_text(value);
  auto latest_user = messages->items().size();
  for (std::size_t i = 0; i < messages->items().size(); ++i) {
    const auto &message = messages->items()[i];
    if (!message.object() || !message["role"].string())
      return retrieval_text(value);
    const auto role = message.str("role");
    if (role != "user" && role != "assistant" && role != "tool" && role != "function" &&
        role != "system" && role != "developer")
      return retrieval_text(value);
    if (role == "user" && retrieval_text(message).find_first_not_of(" \t\r\n") != std::string::npos)
      latest_user = i;
  }
  if (latest_user == messages->items().size())
    return retrieval_text(value);
  Json current = Json::Array{};
  for (std::size_t i = latest_user; i < messages->items().size(); ++i)
    current.push(messages->items()[i]);
  // 不再对当前用户内容做轮次切分；引用、代码和后续未消费工具结果均保留。
  return retrieval_text(current);
}

} // 命名空间结束：trajectory::evolution::integration
