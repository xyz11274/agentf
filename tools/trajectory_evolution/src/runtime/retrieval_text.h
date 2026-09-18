#pragma once

#include "integration_json.h"

namespace trajectory::evolution::integration {

// Retrieval uses observations, not transport field names or embedded guidance.
// This projection never changes the recorded trace or the actual model input.
inline void append_retrieval_text(const Json& value, std::string& result, unsigned depth = 0) {
  if(depth > 48 || value.null()) return;
  if(value.string()) {
    auto text = value.text();
    const auto begin = text.find_first_not_of(" \t\r\n");
    if(begin == std::string::npos) return;
    if(text[begin] == '{' || text[begin] == '[' || text[begin] == '"') {
      try {
        const auto parsed = Json::parse(text);
        append_retrieval_text(parsed, result, depth + 1);
        return;
      } catch(const Error&) {
        // Truncated structured input must not fall back to matching its keys.
        const auto next = text.find_first_not_of(" \t\r\n", begin + 1);
        if(next != std::string::npos && ((text[begin] == '{' && text[next] == '"')
            || (text[begin] == '[' && (text[next] == '{' || text[next] == '[')))) return;
      }
    }
    for(const auto* tag : {"system-reminder", "trajectory-experience", "trajectory-control"}) {
      const std::string open = "<" + std::string(tag), close = "</" + std::string(tag) + ">";
      std::size_t pos = 0;
      while((pos = text.find(open, pos)) != std::string::npos) {
        const auto after = pos + open.size();
        if(after < text.size() && text[after] != '>' && !std::isspace(static_cast<unsigned char>(text[after]))) {
          pos = after; continue;
        }
        const auto end = text.find(close, after);
        text.replace(pos, end == std::string::npos ? text.size() - pos : end + close.size() - pos, " ");
      }
    }
    if(!text.empty()) { result += text; result += '\n'; }
    return;
  }
  if(value.array()) {
    for(const auto& item : value.items()) append_retrieval_text(item, result, depth + 1);
    return;
  }
  if(value.object()) {
    if(value["messages"].array()) {
      append_retrieval_text(value["messages"], result, depth + 1);
      return;
    }
    if(value["role"].string()) {
      const auto role = value["role"].text();
      if(role == "system" || role == "developer") return;
      if(role == "user" || role == "assistant" || role == "tool" || role == "function") {
        if(role == "tool" || role == "function") append_retrieval_text(value["name"], result, depth + 1);
        append_retrieval_text(value["content"], result, depth + 1);
        // Preserve operation names and arguments, not call IDs or schema text.
        if(value["tool_calls"].array()) for(const auto& call : value["tool_calls"].items())
          append_retrieval_text(call["function"], result, depth + 1);
        append_retrieval_text(value["function_call"], result, depth + 1);
        return;
      }
    }
    for(const auto& item : value.fields()) append_retrieval_text(item.second, result, depth + 1);
    return;
  }
  if(std::holds_alternative<double>(value.value)) { result += value.dump(); result += '\n'; }
}

inline std::string retrieval_text(const Json& value) {
  std::string result;
  append_retrieval_text(value, result);
  return result;
}

}  // namespace trajectory::evolution::integration
