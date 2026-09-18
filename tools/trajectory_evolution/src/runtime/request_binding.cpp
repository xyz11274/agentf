#include "trajectory_evolution/request_binding.h"
#include "agentflow/trajectory/hybrid_evaluator.h"

#include <algorithm>
#include <vector>

namespace trajectory::evolution {
namespace {
using Value = google::protobuf::Value;
void require(bool value) {
  if(!value) throw agentflow::trajectory::EvaluationError("request_binding_invalid");
}
std::vector<std::string> parts(const v1::RuntimeOptions& options) {
  const auto path = options.request_context_pointer().empty() ? "/messages" : options.request_context_pointer();
  require(path.size() <= 256 && path.front() == '/');
  std::vector<std::string> result;
  for(std::size_t start = 1; start <= path.size();) {
    auto end = path.find('/', start); if(end == std::string::npos) end = path.size();
    std::string key;
    for(auto i = start; i < end; ++i) {
      if(path[i] == '~') {
        require(++i < end && (path[i] == '0' || path[i] == '1'));
        key += path[i] == '0' ? '~' : '/';
      } else { require(static_cast<unsigned char>(path[i]) >= 32); key += path[i]; }
    }
    result.push_back(key); require(result.size() <= 16);
    if(end == path.size()) break;
    start = end + 1;
  }
  return result;
}
Value* locate(Value& root, const std::vector<std::string>& path, bool create) {
  Value* current = &root;
  for(std::size_t i = 0; i < path.size(); ++i) {
    const auto& key = path[i];
    if(current->kind_case() == Value::kStructValue) {
      auto* fields = current->mutable_struct_value()->mutable_fields();
      auto it = fields->find(key);
      if(it == fields->end()) {
        if(!create || i + 1 != path.size()) return nullptr;
        current = &(*fields)[key];
      } else current = &it->second;
    } else if(current->kind_case() == Value::kListValue) {
      if(key.empty() || key.size() > 6 || (key.size() > 1 && key.front() == '0')
          || !std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; })) return nullptr;
      const auto n = static_cast<unsigned>(std::stoul(key));
      auto* values = current->mutable_list_value()->mutable_values();
      if(n >= static_cast<unsigned>(values->size())) return nullptr;
      current = values->Mutable(n);
    } else return nullptr;
  }
  return current;
}
std::string mode(const v1::RuntimeOptions& options) {
  return options.request_context_mode().empty() ? "system_message" : options.request_context_mode();
}
unsigned occurrences(const std::string& text, const std::string& part) {
  unsigned count = 0;
  for(std::size_t pos = 0; (pos = text.find(part, pos)) != std::string::npos; pos += part.size()) ++count;
  return count;
}
}  // namespace

void validate_request_binding(const v1::RuntimeOptions& options) {
  require(options.request_context_pointer().empty() == options.request_context_mode().empty());
  require(mode(options) == "text" || mode(options) == "system_message");
  parts(options);
}

google::protobuf::Struct inject_request_context(const google::protobuf::Struct& request,
                                                const std::string& context,
                                                const v1::RuntimeOptions& options) {
  validate_request_binding(options);
  if(context.empty()) return request;
  Value root; *root.mutable_struct_value() = request;
  auto* value = locate(root, parts(options), true); require(value != nullptr);
  if(mode(options) == "text") {
    require(value->kind_case() == Value::KIND_NOT_SET || value->kind_case() == Value::kStringValue);
    value->set_string_value(value->string_value() + context);
  } else {
    require(value->kind_case() == Value::KIND_NOT_SET || value->kind_case() == Value::kListValue);
    auto* values = value->mutable_list_value()->mutable_values();
    for(auto& item : *values) {
      if(item.kind_case() != Value::kStructValue) continue;
      auto* fields = item.mutable_struct_value()->mutable_fields();
      auto role = fields->find("role"), content = fields->find("content");
      if(role != fields->end() && role->second.string_value() == "system"
          && content != fields->end() && content->second.kind_case() == Value::kStringValue) {
        content->second.set_string_value(content->second.string_value() + context);
        return root.struct_value();
      }
    }
    auto* message = values->Add()->mutable_struct_value();
    (*message->mutable_fields())["role"].set_string_value("system");
    (*message->mutable_fields())["content"].set_string_value(context);
    for(int i = values->size() - 1; i > 0; --i) values->SwapElements(i, i - 1);
  }
  return root.struct_value();
}

bool bound_request_contains(const google::protobuf::Struct& request, const std::string& context,
                            const v1::RuntimeOptions& options) {
  if(context.empty()) return false;
  validate_request_binding(options);
  Value root; *root.mutable_struct_value() = request;
  auto* value = locate(root, parts(options), false);
  if(!value) return false;
  if(mode(options) == "text") return value->kind_case() == Value::kStringValue
      && occurrences(value->string_value(), context) == 1;
  if(value->kind_case() != Value::kListValue) return false;
  unsigned count = 0;
  for(const auto& item : value->list_value().values()) {
    if(item.kind_case() != Value::kStructValue) continue;
    const auto& fields = item.struct_value().fields();
    auto role = fields.find("role"), content = fields.find("content");
    if(role != fields.end() && role->second.string_value() == "system"
        && content != fields.end() && content->second.kind_case() == Value::kStringValue)
      count += occurrences(content->second.string_value(), context);
  }
  return count == 1;
}
}  // namespace trajectory::evolution
