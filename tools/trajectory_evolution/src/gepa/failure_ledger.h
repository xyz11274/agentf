#pragma once

// 重试身份由显式操作参数决定，不从技能名称或自然语言提示推断。

#include "local_support.h"
#include <algorithm>
#include <openssl/sha.h>

namespace trajectory::evolution::integration {

// Explicit target arguments identify a retry across execution instances. Never
// infer identity from a skill name, natural-language prompt or another run.
inline Json target_failure_snapshot(const Json& options, const std::string& scope,
    const std::string& operation, const Json& arguments, const Json& ledger) {
  Json result = Json::Object{};
  if(!arguments.object() || !options["action_rules"].array()) return result;
  for(const auto& rule : options["action_rules"].items()) {
    if(rule.str("target_scope") != scope || rule.str("operation") != operation
        || !rule["failure_key_arguments"].array() || rule["failure_key_arguments"].items().empty()) continue;
    Json selected = Json::Object{}; bool complete = true;
    for(const auto& field : rule["failure_key_arguments"].items()) {
      const auto& value = arguments[field.text()];
      if(value.null() || value.object() || value.array() || (value.string() && value.text().empty())) { complete = false; break; }
      selected[field.text()] = value;
    }
    if(!complete) continue;
    const auto material = Json{{"scope", scope}, {"operation", operation},
        {"rule", rule["rule_id"]}, {"arguments", selected}}.dump();
    if(material.size() > 8192) continue;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest);
    std::string key; const char* hex = "0123456789abcdef";
    for(auto byte : digest) { key += hex[byte >> 4]; key += hex[byte & 15]; }
    const auto& previous = ledger["entries"][key];
    result[rule.str("rule_id")] = Json{{"key", key}, {"count", previous.num("count", 0)}, {"epoch", previous.num("epoch", 0)},
        {"execution_id", previous.str("execution_id")}};
  }
  return result;
}

inline void settle_target_failure(Json& ledger, const Json& attempts, bool failed) {
  if(!attempts.object()) return;
  for(const auto& pair : attempts.fields()) {
    const auto& attempt = pair.second; const auto key = attempt.str("key");
    if(!ledger["entries"].object()) ledger["entries"] = Json::Object{};
    if(!failed) { ledger["entries"].fields().erase(key); continue; }
    if(!ledger["entries"].has(key) && ledger["entries"].fields().size() >= 256) continue;
    auto& current = ledger["entries"][key];
    if(!current.has("epoch")) {
      ledger["epoch"] = ledger.num("epoch", 0) + 1;
      current["epoch"] = ledger["epoch"];
    }
    // Simultaneous first attempts are not retries of each other. A success
    // resets the epoch so a late completion cannot restore an old streak.
    const auto next = current.num("epoch", 0) == attempt.num("epoch", 0) ? attempt.num("count", 0) + 1 : 1;
    current["count"] = std::min<std::size_t>(10000, std::max(current.num("count", 0), next));
  }
}

inline void settle_target_event(Json& ledger, const Json& frame, const Json& options, const Json& event) {
  if(event.str("kind") != "action.end" || !event["attributes"].flag("executed", false)
      || event["attributes"].flag("delegates_schedule", false)
      || (event.str("status") != "completed" && event.str("status") != "failed") || !frame["entries"].object()) return;
  auto attempts = frame["entries"];
  if(event.has("input")) {
    const auto actual = target_failure_snapshot(options, frame.str("scope"), frame.str("operation"), event["input"], ledger);
    for(auto it = attempts.fields().begin(); it != attempts.fields().end();)
      if(actual[it->first].str("key") != it->second.str("key")) it = attempts.fields().erase(it);
      else ++it;
  }
  const bool failed = event["attributes"].flag("observed_failure", false) || event.str("status") == "failed";
  settle_target_failure(ledger, attempts, failed);
  if(failed) for(const auto& entry : attempts.fields()) {
    const auto key = entry.second.str("key");
    if(ledger["entries"].has(key)) ledger["entries"][key]["execution_id"] = event.str("execution_id");
  }
}

}  // namespace trajectory::evolution::integration
