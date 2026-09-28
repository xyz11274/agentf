// 失败证据账本：保留动作或分支失败的归属，供重规划和候选回放使用。
// 按执行身份关联结果，避免某个并发分支的失败错误扩散成全局失败。
// 成功恢复不删除历史失败；两种事实在不同阶段分别参与判断。

#pragma once

// 重试身份由显式操作参数决定，不从技能名称或自然语言提示推断。

#include "local_support.h"
#include <algorithm>
#include <openssl/sha.h>

namespace trajectory::evolution::integration {

// 只读取结构化错误码；兼容工具将后端 JSON 嵌入 error 字符串的返回形式。
// 普通描述、被截断的 JSON 及未知错误均不能扩大失败作用域。
inline std::string structured_error_code(Json output, unsigned depth = 0) {
  if (depth > 4)
    return {};
  if (output.string()) {
    const auto text = output.text();
    if (text.size() > 65536)
      return {};
    try {
      return structured_error_code(Json::parse(text), depth + 1);
    } catch (...) {
    }
    const auto first = text.find('{'), last = text.rfind('}');
    if (first != std::string::npos && last != std::string::npos && last > first)
      try {
        return structured_error_code(Json::parse(text.substr(first, last - first + 1)), depth + 1);
      } catch (...) {
      }
    return {};
  }
  if (!output.object())
    return {};
  if (output["code"].string() && output["code"].text().size() <= 128)
    return output["code"].text();
  return structured_error_code(output["error"], depth + 1);
}

// 显式目标参数用于识别跨执行实例的重试，
// 不能从技能名、自然语言提示词或另一次运行推断身份。
inline Json target_failure_snapshot(const Json &options, const std::string &scope,
                                    const std::string &operation, const Json &arguments,
                                    const Json &ledger, const std::string &execution_id = {}) {
  Json result = Json::Object{};
  if (!arguments.object() || !options["action_rules"].array())
    return result;
  for (const auto &rule : options["action_rules"].items()) {
    const bool terminal =
        rule["terminal_error_codes"].array() && !rule["terminal_error_codes"].items().empty();
    const bool budget = rule.num("max_attempts", 0) > 0;
    // 终态错误只能约束已经观察到该错误的执行分支；缺少归属时不扩大为全 run 断路。
    if (terminal && !identifier(execution_id))
      continue;
    if (rule.str("target_scope") != scope || rule.str("operation") != operation ||
        (!terminal && !budget &&
         (!rule["failure_key_arguments"].array() || rule["failure_key_arguments"].items().empty())))
      continue;
    Json selected = Json::Object{};
    bool complete = true;
    const auto key_field = terminal ? "error_scope_arguments"
                           : budget ? "attempt_key_arguments"
                                    : "failure_key_arguments";
    const auto keys = rule.has(key_field) ? rule[key_field] : Json(Json::Array{});
    if (terminal && keys.items().empty()) {
      // 未显式声明配置作用域时，错误只约束完整原请求；换 action、地址或嵌套参数仍可尝试。
      // 只有明确指定字段（例如 model）的规则才沿用配置级身份，普通总次数预算不变。
      const auto bytes = arguments.dump();
      if (bytes.find("<redacted") != std::string::npos ||
          bytes.find("[REDACTED") != std::string::npos ||
          bytes.find("[redacted") != std::string::npos)
        continue;
      // 完整请求先取摘要，既不保存原文，也不让合法长参数触发下方显式字段的大小保护。
      unsigned char argument_digest[SHA256_DIGEST_LENGTH];
      SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), argument_digest);
      std::string argument_key;
      const char *hex = "0123456789abcdef";
      for (auto byte : argument_digest) {
        argument_key += hex[byte >> 4];
        argument_key += hex[byte & 15];
      }
      selected = argument_key;
    }
    for (const auto &field : keys.items()) {
      const auto &value = arguments[field.text()];
      // 省略可选配置表示同一默认配置；显式换模型/服务会获得不同身份。
      if ((!terminal && value.null()) || value.object() || value.array() ||
          (value.string() && value.text().empty())) {
        complete = false;
        break;
      }
      // 脱敏占位符不再具有身份区分能力，不能合并不同目标的失败链。
      if (value.string() && (value.text().find("<redacted") != std::string::npos ||
                             value.text().find("[REDACTED") != std::string::npos ||
                             value.text().find("[redacted") != std::string::npos)) {
        complete = false;
        break;
      }
      selected[field.text()] = value;
    }
    if (!complete)
      continue;
    Json identity{{"scope", scope},
                  {"operation", operation},
                  {"rule", rule["rule_id"]},
                  {"arguments", selected}};
    if (terminal)
      identity["execution_id"] = execution_id;
    const auto material = identity.dump();
    if (material.size() > 8192)
      continue;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest);
    std::string key;
    const char *hex = "0123456789abcdef";
    for (auto byte : digest) {
      key += hex[byte >> 4];
      key += hex[byte & 15];
    }
    const auto &previous = ledger["entries"][key];
    result[rule.str("rule_id")] = Json{{"key", key},
                                       {"count", previous.num("count", 0)},
                                       {"epoch", previous.num("epoch", 0)},
                                       {"execution_id", previous.str("execution_id")}};
    if (terminal) {
      result[rule.str("rule_id")]["terminal_error_codes"] = rule["terminal_error_codes"];
      result[rule.str("rule_id")]["limit"] = rule.num("max_failed_attempts", 1);
    }
    if (budget)
      result[rule.str("rule_id")]["attempt_budget"] = true;
  }
  return result;
}

// 将异步分支的失败归入对应目标和执行时期，避免一个失败分支污染其他目标。
inline void settle_target_failure(Json &ledger, const Json &attempts, bool failed,
                                  const Json &output = Json{}) {
  if (!attempts.object())
    return;
  for (const auto &pair : attempts.fields()) {
    const auto &attempt = pair.second;
    const auto key = attempt.str("key");
    const bool terminal = attempt["terminal_error_codes"].array();
    if (failed && terminal) {
      const auto code = structured_error_code(output);
      if (code.empty() || std::none_of(attempt["terminal_error_codes"].items().begin(),
                                       attempt["terminal_error_codes"].items().end(),
                                       [&](const auto &value) { return value.text() == code; }))
        continue;
    }
    if (!ledger["entries"].object())
      ledger["entries"] = Json::Object{};
    if (!ledger["entries"].has(key) && ledger["entries"].fields().size() >= 256)
      continue;
    auto &current = ledger["entries"][key];
    if (!failed && !attempt.flag("attempt_budget", false)) {
      // 保留成功代次，避免并发迟到失败把已经清零的旧失败链复活。
      ledger["epoch"] = ledger.num("epoch", 0) + 1;
      current = Json{{"count", 0}, {"epoch", ledger["epoch"]}};
      continue;
    }
    if (current.has("epoch") && current.num("count", 0) == 0 &&
        current.num("epoch", 0) != attempt.num("epoch", 0))
      continue;
    if (!current.has("epoch")) {
      ledger["epoch"] = ledger.num("epoch", 0) + 1;
      current["epoch"] = ledger["epoch"];
    }
    // 同时发生的首次尝试不是彼此的重试；一次成功
    // 会重置失败时期，迟到回执不能恢复旧的连续失败计数。
    const auto next =
        current.num("epoch", 0) == attempt.num("epoch", 0) ? attempt.num("count", 0) + 1 : 1;
    current["count"] = terminal
                           ? attempt.num("limit", 1)
                           : std::min<std::size_t>(10000, std::max(current.num("count", 0), next));
  }
}

// guard 只做检查；异步目标预算从真正开始的子动作取快照，再按终态结算。
inline void snapshot_scheduled_target(Json &attempts, const Json &ledger, const Json &options,
                                      const Json &event) {
  if (event.str("kind") != "action.start" || event["attributes"].str("type") != "schedule.action" ||
      event.str("action_id").empty() || event["attributes"].str("target_scope").empty())
    return;
  const auto scope = event["attributes"].str("target_scope"), operation = event.str("operation");
  // 调度子动作共享父 execution，必须使用其唯一 action_id 区分兄弟执行域。
  const auto execution = event.str("action_id");
  attempts[event.str("action_id")] =
      Json{{"entries",
            target_failure_snapshot(options, scope, operation, event["input"], ledger, execution)},
           {"scope", scope},
           {"operation", operation},
           {"execution_id", execution}};
}

// 按事件身份更新失败台账，处理重复和迟到回执，保持状态转换幂等。
inline void settle_target_event(Json &ledger, const Json &frame, const Json &options,
                                const Json &event) {
  if (event.str("kind") != "action.end" || !event["attributes"].flag("executed", false) ||
      event["attributes"].flag("delegates_schedule", false) ||
      (event.str("status") != "completed" && event.str("status") != "failed") ||
      !frame["entries"].object())
    return;
  auto attempts = frame["entries"];
  if (event.has("input")) {
    const auto actual = target_failure_snapshot(options, frame.str("scope"), frame.str("operation"),
                                                event["input"], ledger, frame.str("execution_id"));
    for (auto it = attempts.fields().begin(); it != attempts.fields().end();)
      if (actual[it->first].str("key") != it->second.str("key"))
        it = attempts.fields().erase(it);
      else
        ++it;
  }
  const bool failed =
      event["attributes"].flag("observed_failure", false) || event.str("status") == "failed";
  auto code = event["attributes"].str("observed_error_code");
  // 回放和在线保持同一优先级：已记录的观测码、结果结构码、显式 ErrorInfo。
  // 自由文本不参与推断；沿用当前 frame 的执行身份，不扩大终态阻断范围。
  if (code.empty())
    code = structured_error_code(event["output"]);
  if (code.empty() && event.has("error"))
    code = structured_error_code(event["error"]);
  settle_target_failure(ledger, attempts, failed,
                        code.empty() ? event["output"] : Json{{"code", code}});
  if (failed)
    for (const auto &entry : attempts.fields()) {
      const auto key = entry.second.str("key");
      if (ledger["entries"].has(key))
        ledger["entries"][key]["execution_id"] = event.str("execution_id");
    }
}

} // 命名空间结束：trajectory::evolution::integration
