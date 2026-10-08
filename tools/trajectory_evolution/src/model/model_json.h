// 模型 JSON 辅助处理：规范化响应封装，保留严格的 JSON 解析边界。
// 只处理允许的外围格式；不猜测或补造缺失字段，避免错误文本被当成有效结果。

#pragma once

#include <string>
#include <set>
#include <vector>
#include <rapidjson/document.h>

namespace agentflow::trajectory {

// 以非递归方式校验对象键；模型响应可能包含嵌套数组。
// 在任何读取方选择字段值前，先拒绝重复键造成的歧义。
inline bool unique_object_fields(const rapidjson::Value &value) {
  if (!value.IsObject())
    return false;
  std::vector<const rapidjson::Value *> pending{&value};
  while (!pending.empty()) {
    const auto *current = pending.back();
    pending.pop_back();
    if (current->IsArray()) {
      for (const auto &item : current->GetArray())
        if (item.IsObject() || item.IsArray())
          pending.push_back(&item);
    } else if (current->IsObject()) {
      std::set<std::string> names;
      for (const auto &field : current->GetObject()) {
        if (!names.emplace(field.name.GetString(), field.name.GetStringLength()).second)
          return false;
        if (field.value.IsObject() || field.value.IsArray())
          pending.push_back(&field.value);
      }
    }
  }
  return true;
}

// 只移除完整包围响应的一层 JSON 代码围栏。
// 调用方仍保留原始响应做隐私和摘要检查，再验证解码后的契约。
inline std::string unwrap_model_json(const std::string &response, bool *unwrapped = nullptr) {
  if (unwrapped)
    *unwrapped = false;
  std::string body = response;
  const auto first = body.find_first_not_of(" \r\n\t");
  const auto last = body.find_last_not_of(" \r\n\t");
  if (first != std::string::npos)
    body = body.substr(first, last - first + 1U);
  const auto line_end = body.find('\n');
  auto header = body.substr(0, line_end);
  const auto header_last = header.find_last_not_of(" \t\r");
  if (header_last != std::string::npos)
    header.resize(header_last + 1U);
  // Markdown language labels are case-insensitive. Only a complete JSON fence
  // is normalized; prose, other languages and multiple blocks stay invalid.
  if (header.compare(0, 3, "```") == 0) {
    auto label = header.substr(3);
    const auto label_first = label.find_first_not_of(" \t");
    label = label_first == std::string::npos ? "" : label.substr(label_first);
    for (auto &character : label)
      if (character >= 'A' && character <= 'Z')
        character = static_cast<char>(character - 'A' + 'a');
    header = "```" + label;
  }
  if ((header == "```json" || header == "```") &&
      line_end != std::string::npos && body.size() >= line_end + 5U &&
      body.compare(body.size() - 4U, 4U, "\n```") == 0) {
    body = body.substr(line_end + 1U, body.size() - line_end - 5U);
    if (unwrapped)
      *unwrapped = true;
  }
  return body;
}

} // 命名空间结束：agentflow::trajectory
