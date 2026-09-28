// 模型输出契约：把文本响应解析为结构化评测或反思结果并逐项校验。
// 模型输出是不可信输入，不能绕过字段、枚举、证据引用和长度限制。
// 解析失败返回可诊断状态，不用默认通过或空结果掩盖错误。

#include "model_contract.h"

#include <set>

namespace trajectory::evolution::integration {
namespace {
// 统计当前契约中的有效项，避免重复声明扩大能力数量。
bool count(const Json &value) {
  if (!std::holds_alternative<double>(value.value))
    return false;
  const auto n = std::get<double>(value.value);
  return std::isfinite(n) && n >= 0 && n <= 1000000000 && n == std::floor(n);
}

// 返回当前对象的稳定名称，供协议选择和诊断引用。
bool name(const Json &value) {
  return value.string() && !value.text().empty() && value.text().size() <= 256 &&
         value.text().find('\0') == std::string::npos;
}

// 合并工具声明，出现冲突时保持未知，不按到达顺序覆盖。
void add_contract(Json &result, const Json &tool, const Json &schema) {
  if (!name(tool))
    return;
  const auto key = tool.text();
  Json contract;
  bool valid = schema.object() && (!schema.has("type") ||
                                   (schema["type"].string() && schema["type"].text() == "object"));
  if (schema.has("required"))
    valid &= schema["required"].array() && schema["required"].items().size() <= 32;
  if (valid) {
    std::set<std::string> fields;
    if (schema.has("required"))
      for (const auto &field : schema["required"].items()) {
        if (!name(field) || !fields.insert(field.text()).second) {
          valid = false;
          break;
        }
      }
    if (valid) {
      contract = Json{{"required", Json::Array{}}};
      for (const auto &field : fields)
        contract["required"].push(field);
    }
  }
  // 冲突声明应视为未知，不能由到达顺序选出所谓正确声明。
  if (result.has(key) && result[key].dump() != contract.dump())
    result[key] = nullptr;
  else if (!result.has(key))
    result[key] = contract;
}
}

// 从模型请求的线缆结构识别注入位置，不依赖模型自然语言或特定 Agent 实现。
Json infer_model_binding(const Json &input) {
  auto binding = [](const std::string &path, const char *mode) {
    return Json{{"pointer", path}, {"mode", mode}};
  };
  if (input.string())
    return binding("", "text");
  if (!input.object())
    return nullptr;
  if (input["system"].string())
    return binding("/system", "text");
  if (input["system"].array() && !input["system"].items().empty() &&
      input["system"].items().front()["text"].string())
    return binding("/system/0/text", "text");
  if (input["instructions"].string())
    return binding("/instructions", "text");
  if (input["messages"].array()) {
    // 使用 input_schema 工具定义的消息接口采用独立 system 字段。
    if (input["tools"].array())
      for (const auto &tool : input["tools"].items())
        if (tool.has("input_schema"))
          return input.has("system") ? Json{} : binding("/system", "text");
    if (input.has("system"))
      return nullptr;
    bool chat = input["functions"].array();
    if (input["tools"].array())
      for (const auto &tool : input["tools"].items())
        chat |= tool["function"].object();
    for (const auto &message : input["messages"].items())
      chat |= message.str("role") == "system" || message.str("role") == "developer" ||
              message.str("role") == "tool" || message.has("tool_calls");
    if (chat)
      return binding("/messages", "system_message");
    // 普通 user/assistant 消息无法唯一确定提供方；
    // 优先复用现有文本字段，不添加可能不受支持的 system 角色。
    const auto &messages = input["messages"].items();
    for (std::size_t i = messages.size(); i > 0; --i)
      if (messages[i - 1].str("role") == "user") {
        const auto path = "/messages/" + std::to_string(i - 1) + "/content";
        if (messages[i - 1]["content"].string())
          return binding(path, "text");
        const auto &content = messages[i - 1]["content"];
        if (content.array())
          for (std::size_t j = 0; j < content.items().size(); ++j)
            if (content.items()[j].str("type") == "text" && content.items()[j]["text"].string())
              return binding(path + "/" + std::to_string(j) + "/text", "text");
        return nullptr;
      }
    return nullptr;
  }
  if (input["input"].string() || input["input"].array())
    return input.has("instructions") ? Json{} : binding("/instructions", "text");
  if (input["prompt"].string())
    return binding("/prompt", "text");
  const auto &parts = input["systemInstruction"]["parts"];
  if (parts.array() && !parts.items().empty() && parts.items().front()["text"].string())
    return binding("/systemInstruction/parts/0/text", "text");
  return nullptr;
}

// 从真实返回结构提取 token 使用量，区分新输入和缓存消耗。
Json infer_model_usage(const Json &output) {
  Json result = Json::Object{};
  const auto &usage = output["usage"];
  auto take = [&](const char *field, const Json &a, const Json &b = Json{}) {
    if (count(a) && (b.null() || (count(b) && a.dump() == b.dump())))
      result[field] = a;
    else if (!a.null() && !b.null())
      return;
    else if (a.null() && count(b))
      result[field] = b;
  };
  take("input_tokens", usage["input_tokens"], usage["prompt_tokens"]);
  take("output_tokens", usage["output_tokens"], usage["completion_tokens"]);
  take("tokens", usage["total_tokens"]);
  take("cached_input_tokens", usage["cache_read_input_tokens"],
       usage["prompt_tokens_details"]["cached_tokens"]);
  // Anthropic 将新处理的输入与缓存用量分开报告。
  if (result.has("input_tokens") &&
      (usage.has("cache_read_input_tokens") || usage.has("cache_creation_input_tokens"))) {
    const auto &read = usage["cache_read_input_tokens"];
    const auto &created = usage["cache_creation_input_tokens"];
    if ((read.null() || count(read)) && (created.null() || count(created))) {
      const auto total = result["input_tokens"].integer() + (read.null() ? 0 : read.integer()) +
                         (created.null() ? 0 : created.integer());
      if (total <= 1000000000)
        result["input_tokens"] = total;
      else
        result.fields().erase("input_tokens");
    } else
      result.fields().erase("input_tokens");
  }
  if (usage.null()) {
    const auto &meta = output["usageMetadata"];
    take("input_tokens", meta["promptTokenCount"]);
    take("output_tokens", meta["candidatesTokenCount"]);
    take("tokens", meta["totalTokenCount"]);
    take("cached_input_tokens", meta["cachedContentTokenCount"]);
    if (meta.has("thoughtsTokenCount")) {
      if (result.has("output_tokens") && count(meta["thoughtsTokenCount"])) {
        const auto total = result["output_tokens"].integer() + meta["thoughtsTokenCount"].integer();
        if (total <= 1000000000)
          result["output_tokens"] = total;
        else
          result.fields().erase("output_tokens");
      } else
        result.fields().erase("output_tokens");
    }
  }
  return result;
}

// 从工具声明提取可执行动作契约，不能从历史基线猜测允许修改的动作。
Json model_tool_contracts(const Json &input) {
  Json result = Json::Object{};
  const auto &tools = input["tools"];
  if (tools.array() && tools.items().size() <= 64)
    for (const auto &tool : tools.items()) {
      if (tool["function"].object())
        add_contract(result, tool["function"]["name"], tool["function"]["parameters"]);
      else if (tool.has("input_schema"))
        add_contract(result, tool["name"], tool["input_schema"]);
      else if (tool.has("parameters"))
        add_contract(result, tool["name"], tool["parameters"]);
      else if (tool["functionDeclarations"].array() &&
               tool["functionDeclarations"].items().size() <= 64)
        for (const auto &declaration : tool["functionDeclarations"].items())
          add_contract(result, declaration["name"], declaration["parameters"]);
    }
  const auto &functions = input["functions"];
  if (functions.array() && functions.items().size() <= 64)
    for (const auto &tool : functions.items())
      add_contract(result, tool["name"], tool["parameters"]);
  if (result.fields().size() > 64 || result.dump().size() > 65536)
    return Json::Object{};
  return result;
}

// 核对工具调用是否符合已声明的参数契约，拒绝不支持的名称与结构。
Json check_tool_contract(const Json &contracts, const std::string &operation,
                         const Json &arguments) {
  if (contracts.string())
    return Json{{"status", "unknown"}, {"reason", "tool_definition_capacity"}};
  if (!contracts.has(operation))
    return Json{{"status", "unknown"}, {"reason", "tool_definition_not_observed"}};
  const auto &contract = contracts[operation];
  if (contract.null())
    return Json{{"status", "unknown"}, {"reason", "tool_definition_ambiguous"}};
  Json missing = Json::Array{};
  for (const auto &field : contract["required"].items())
    if (!arguments.has(field.text()))
      missing.push(field);
  return Json{{"status", missing.items().empty() ? "passed" : "failed"},
              {"reason", missing.items().empty() ? "tool_required_arguments_present"
                                                 : "tool_required_arguments_missing"},
              {"missing", missing}};
}

} // 命名空间结束：trajectory::evolution::integration
