#pragma once

#include <string>
#include <set>
#include <vector>
#include <rapidjson/document.h>

namespace agentflow::trajectory {

// Validate object keys without recursive descent; model responses can contain
// nested arrays. Reject ambiguity before any consumer selects a field value.
inline bool unique_object_fields(const rapidjson::Value& value) {
  if(!value.IsObject()) return false;
  std::vector<const rapidjson::Value*> pending{&value};
  while(!pending.empty()) {
    const auto* current = pending.back(); pending.pop_back();
    if(current->IsArray()) {
      for(const auto& item : current->GetArray())
        if(item.IsObject() || item.IsArray()) pending.push_back(&item);
    } else if(current->IsObject()) {
      std::set<std::string> names;
      for(const auto& field : current->GetObject()) {
        if(!names.emplace(field.name.GetString(), field.name.GetStringLength()).second) return false;
        if(field.value.IsObject() || field.value.IsArray()) pending.push_back(&field.value);
      }
    }
  }
  return true;
}

// Only unwrap a single enclosing JSON fence. Callers retain the raw response
// for privacy checks and fingerprints, then validate the decoded JSON contract.
inline std::string unwrap_model_json(const std::string& response, bool* unwrapped = nullptr) {
  if(unwrapped) *unwrapped = false;
  std::string body = response;
  const auto first = body.find_first_not_of(" \r\n\t");
  const auto last = body.find_last_not_of(" \r\n\t");
  if(first != std::string::npos) body = body.substr(first, last - first + 1U);
  const auto line_end = body.find('\n');
  const auto header = body.substr(0, line_end);
  if((header == "```json" || header == "```" || header == "```json\r" || header == "```\r")
     && line_end != std::string::npos && body.size() >= line_end + 5U
     && body.compare(body.size() - 4U, 4U, "\n```") == 0) {
    body = body.substr(line_end + 1U, body.size() - line_end - 5U);
    if(unwrapped) *unwrapped = true;
  }
  return body;
}

}  // namespace agentflow::trajectory
