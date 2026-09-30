// 循环分析：按执行身份、循环编号与迭代号组织轨迹，识别重复和收敛情况。
// 并行分支的迭代分别归属，不能仅按日志先后顺序拼成同一个循环。
// 规则能够确定的浪费是下界；缺少观测不代表无效循环为零。

#include "agentflow/trajectory/loop_episode_analyzer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace agentflow::trajectory {
namespace {

using Allocator = rapidjson::Document::AllocatorType;
using JsonValue = rapidjson::Value;

struct EventRecord {
  rapidjson::Document document;
  std::size_t line_number = 0;
  std::string type;
  std::string event_id;
  std::string idempotency_key;
  std::string dialogue_id;
  std::string producer_id;
  std::uint64_t producer_sequence = 0;
  std::uint64_t timestamp_us = 0;
  std::uint64_t round = 0;
  std::string node_id;
};

struct ParsedInput {
  std::vector<EventRecord> events;
  std::string dialogue_id;
  std::string conversation_id;
  std::string user_id;
  std::size_t duplicate_event_count = 0;
  std::vector<TraceIssue> issues;
};

struct SkillIdentity {
  std::string skill_name;
  std::string exec_skill_key;
  std::string batch_id;
  std::uint64_t max_rounds = 0;
};

struct ToolResultRecord {
  const EventRecord *event = nullptr;
  std::string tool_call_id;
  std::string tool_name;
  std::string status;
  std::string result_fingerprint;
  std::string error_class;
  std::string retryability;
};

struct EpisodeWork {
  LoopEpisode episode;
  const EventRecord *request = nullptr;
};

// 复制文本后规范化大小写，避免修改调用方持有的原始值。
std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

// 判断集合或文本是否包含指定值，供当前规则的精确匹配使用。
bool contains(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

// 查找 JSON 对象成员；缺失时返回空指针，由调用方决定是否允许缺省。
const JsonValue *find_member(const JsonValue &object, const char *name) {
  if (!object.IsObject()) {
    return nullptr;
  }
  const auto member = object.FindMember(name);
  return member == object.MemberEnd() ? nullptr : &member->value;
}

// 提取事件载荷，兼容入口差异而不修改原始事件身份。
const JsonValue *event_data(const EventRecord &event) {
  return find_member(event.document, "data");
}

// 读取 JSON 字符串值，保持缺失与空字符串的约定。
std::string json_string(const JsonValue *value) {
  if (value == nullptr || !value->IsString()) {
    return {};
  }
  return std::string(value->GetString(), value->GetStringLength());
}

// 读取字符串成员并应用本函数的缺省约定；不把其他 JSON 类型隐式转成字符串。
std::string member_string(const JsonValue &object, const char *name) {
  return json_string(find_member(object, name));
}

// 读取非负整数，兼容协议允许的表示并校验范围。
std::uint64_t json_uint64(const JsonValue *value) {
  if (value == nullptr) {
    return 0;
  }
  if (value->IsUint64()) {
    return value->GetUint64();
  }
  if (value->IsInt64() && value->GetInt64() >= 0) {
    return static_cast<std::uint64_t>(value->GetInt64());
  }
  if (value->IsString()) {
    try {
      return std::stoull(value->GetString());
    } catch (const std::exception &) {
      return 0;
    }
  }
  return 0;
}

// 读取非负整数成员，并按本模块约定处理类型和范围错误。
std::uint64_t member_uint64(const JsonValue &object, const char *name) {
  return json_uint64(find_member(object, name));
}

// 读取布尔成员，明确区分字段缺失与显式 false。
bool member_bool(const JsonValue &object, const char *name, bool fallback = false) {
  const JsonValue *value = find_member(object, name);
  return value != nullptr && value->IsBool() ? value->GetBool() : fallback;
}

// 读取数值成员，供指标和置信度计算使用；类型校验在此边界完成。
double member_double(const JsonValue &object, const char *name, bool *found) {
  const JsonValue *value = find_member(object, name);
  if (value != nullptr && value->IsNumber()) {
    if (found != nullptr) {
      *found = true;
    }
    return value->GetDouble();
  }
  if (found != nullptr) {
    *found = false;
  }
  return 0.0;
}

// 将已验证数组转换为字符串集合，供后续索引和比较使用。
std::vector<std::string> string_array(const JsonValue *value) {
  std::vector<std::string> output;
  if (value == nullptr || !value->IsArray()) {
    return output;
  }
  output.reserve(value->Size());
  for (const auto &item : value->GetArray()) {
    if (item.IsString()) {
      output.emplace_back(item.GetString(), item.GetStringLength());
    }
  }
  return output;
}

// 保持已有顺序追加尚未出现的元素，避免重复证据或标签影响统计。
void append_unique(std::vector<std::string> &values, const std::string &value) {
  if (value.empty()) {
    return;
  }
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

// 将 JSON 值序列化为紧凑文本，不改变应用字段的实际内容。
std::string serialize_json(const JsonValue &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

std::string canonical_json(const JsonValue &value);

// 规范化对象成员顺序，避免序列化顺序改变内容身份。
std::string canonical_object(const JsonValue &value) {
  std::vector<std::pair<std::string, const JsonValue *>> members;
  members.reserve(value.MemberCount());
  for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
    members.emplace_back(std::string(member->name.GetString(), member->name.GetStringLength()),
                         &member->value);
  }
  std::stable_sort(members.begin(), members.end(),
                   [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  std::string output = "{";
  for (std::size_t index = 0; index < members.size(); ++index) {
    if (index > 0) {
      output += ',';
    }
    rapidjson::Document key_document;
    key_document.SetString(members[index].first.c_str(),
                           static_cast<rapidjson::SizeType>(members[index].first.size()),
                           key_document.GetAllocator());
    output += serialize_json(key_document);
    output += ':';
    output += canonical_json(*members[index].second);
  }
  output += '}';
  return output;
}

// 生成稳定 JSON 表示，供重复检测和证据摘要使用。
std::string canonical_json(const JsonValue &value) {
  if (value.IsObject()) {
    return canonical_object(value);
  }
  if (value.IsArray()) {
    std::string output = "[";
    for (rapidjson::SizeType index = 0; index < value.Size(); ++index) {
      if (index > 0) {
        output += ',';
      }
      output += canonical_json(value[index]);
    }
    output += ']';
    return output;
  }
  return serialize_json(value);
}

// 对规范化内容计算稳定摘要，用于去重和版本比较。
// 摘要只标识内容是否变化，不替代权限校验，也不包含可还原的凭据原文。
std::string fingerprint(std::string_view value) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  if (SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest.data()) ==
      nullptr) {
    throw AnalysisError("failed to compute SHA-256 fingerprint");
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.resize(digest.size() * 2U);
  for (std::size_t index = 0; index < digest.size(); ++index) {
    output[index * 2U] = hex[(digest[index] >> 4U) & 0x0fU];
    output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
  }
  return output;
}

// 规范化用于重复结果比较的文本，原始工具返回仍保留。
std::string normalized_result_text(std::string value) {
  value = lower_copy(std::move(value));
  value = std::regex_replace(value, std::regex(R"((request[ _-]?id\s*[:=]?\s*)[a-z0-9-]{12,})"),
                             "$1<id>");
  value = std::regex_replace(value, std::regex(R"([0-9a-f]{20,})"), "<hex>");
  value = std::regex_replace(value, std::regex(R"(\b[0-9]{13,}\b)"), "<number>");
  value = std::regex_replace(value, std::regex(R"(\s+)"), " ");
  if (value.size() > 8192) {
    value.resize(8192);
  }
  return value;
}

// 将已观测错误归入稳定类别，供重试诊断与循环分析使用。
std::string classify_error(const std::string &raw_text) {
  const std::string text = lower_copy(raw_text);
  if (contains(text, "unsupportedmodel") || contains(text, "unsupported model") ||
      contains(text, "does not support the agent plan")) {
    return "unsupported_model";
  }
  if (contains(text, "invalid api key") || contains(text, "authentication") ||
      contains(text, "unauthorized") || contains(text, "status 401") ||
      contains(text, "http 401")) {
    return "authentication_failed";
  }
  if (contains(text, "permission denied") || contains(text, "forbidden") ||
      contains(text, "status 403") || contains(text, "http 403")) {
    return "permission_denied";
  }
  if (contains(text, "safety") || contains(text, "policy blocked") ||
      contains(text, "content policy")) {
    return "policy_blocked";
  }
  if (contains(text, "invalidparameter") || contains(text, "invalid parameter") ||
      contains(text, "invalid argument") || contains(text, "bad request") ||
      contains(text, "status 400") || contains(text, "http 400")) {
    return "invalid_argument";
  }
  if (contains(text, "rate limit") || contains(text, "too many requests") ||
      contains(text, "status 429") || contains(text, "http 429")) {
    return "rate_limited";
  }
  if (contains(text, "timeout") || contains(text, "timed out")) {
    return "timeout";
  }
  if (contains(text, "connection reset") || contains(text, "connection refused") ||
      contains(text, "network error") || contains(text, "temporary failure") ||
      contains(text, "name resolution")) {
    return "transient_network";
  }
  if (contains(text, "status 500") || contains(text, "status 502") ||
      contains(text, "status 503") || contains(text, "status 504") || contains(text, "http 500") ||
      contains(text, "http 502") || contains(text, "http 503") || contains(text, "http 504")) {
    return "provider_5xx";
  }
  if (contains(text, "not found") || contains(text, "status 404") || contains(text, "http 404")) {
    return "not_found";
  }
  return "unknown_tool_error";
}

// 根据错误类别决定可重试性，区分暂时故障与重复执行也无法恢复的问题。
std::string retryability_for(const std::string &error_class) {
  if (error_class == "rate_limited" || error_class == "timeout" ||
      error_class == "transient_network" || error_class == "provider_5xx") {
    return "retryable";
  }
  if (error_class == "unsupported_model" || error_class == "authentication_failed" ||
      error_class == "permission_denied" || error_class == "policy_blocked" ||
      error_class == "invalid_argument" || error_class == "not_found") {
    return "non_retryable";
  }
  return "unknown";
}

// 依据明确状态识别失败，不将未知或缺失状态直接当成业务失败。
bool status_is_failure(const std::string &status, const std::string &result_text) {
  const std::string normalized_status = lower_copy(status);
  if (normalized_status == "error" || normalized_status == "failed" ||
      normalized_status == "failure" || normalized_status == "timeout" ||
      normalized_status == "cancelled") {
    return true;
  }
  const std::string normalized_result = lower_copy(result_text);
  return contains(normalized_result, "\"status\": \"failed\"") ||
         contains(normalized_result, "\"status\":\"failed\"") ||
         contains(normalized_result, "\"status\": \"error\"") ||
         contains(normalized_result, "\"status\":\"error\"");
}

// 判断终态是否表示业务失败，避免把中间进度当作最终结果。
bool terminal_status_is_failure(const std::string &status) {
  static const std::unordered_set<std::string> failure_statuses = {
      "cancelled",
      "dispatch_manifest_error",
      "failed",
      "failure",
      "interrupted",
      "llm_error",
      "max_rounds_reached",
      "node_timeout",
      "security_policy_blocked",
      "skipped",
      "timeout",
      "tool_call_error",
      "tool_call_loop_detected",
      "tool_error",
      "unhandled_exception",
  };
  return failure_statuses.count(lower_copy(status)) != 0;
}

// 把当前值转换为分析用文本表示，不据此改变其原始类型。
std::string value_as_text(const JsonValue *value) {
  if (value == nullptr) {
    return {};
  }
  if (value->IsString()) {
    return std::string(value->GetString(), value->GetStringLength());
  }
  return canonical_json(*value);
}

// 提取事件生产者身份，避免不同进程的局部序号混淆。
std::string event_producer_id(const rapidjson::Document &document) {
  std::string producer = member_string(document, "producer_id");
  if (!producer.empty()) {
    return producer;
  }
  producer = member_string(document, "trace_id");
  const JsonValue *pid = find_member(document, "pid");
  if (pid != nullptr && pid->IsInt64()) {
    producer += ":pid" + std::to_string(pid->GetInt64());
  }
  return producer;
}

// 结合问题类别与证据生成稳定诊断标识，便于去重与回溯。
std::string make_issue_id(const std::string &code, const std::string &message,
                          const std::vector<std::string> &evidence) {
  std::string material = code + '\n' + message;
  for (const auto &event_id : evidence) {
    material += '\n' + event_id;
  }
  return "issue:" + code + ':' + fingerprint(material);
}

// 记录结构或完整性问题，并保留对应事件定位。
void add_issue(std::vector<TraceIssue> &issues, const std::string &severity,
               const std::string &code, const std::string &message,
               std::vector<std::string> evidence = {}) {
  TraceIssue issue;
  issue.severity = severity;
  issue.code = code;
  issue.message = message;
  issue.evidence_event_ids = std::move(evidence);
  issue.issue_id = make_issue_id(issue.code, issue.message, issue.evidence_event_ids);
  issues.push_back(std::move(issue));
}

// 读取事件序列并检查格式，为循环片段构建准备原始事实。
ParsedInput read_events(const std::filesystem::path &path, const AnalyzerOptions &options) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw AnalysisError("cannot open input file: " + path.string());
  }

  stream.seekg(0, std::ios::end);
  const std::streamoff end = stream.tellg();
  if (end < 0) {
    throw AnalysisError("cannot determine input size: " + path.string());
  }
  if (static_cast<std::uint64_t>(end) > options.max_file_bytes) {
    throw AnalysisError("input exceeds max_file_bytes: " + path.string());
  }
  stream.seekg(0, std::ios::beg);
  std::string content(static_cast<std::size_t>(end), '\0');
  if (!content.empty()) {
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) {
      throw AnalysisError("failed to read input file: " + path.string());
    }
  }

  ParsedInput parsed;
  std::unordered_map<std::string, std::string> seen_idempotency;
  std::unordered_map<std::string, std::uint64_t> last_sequence;
  std::size_t offset = 0;
  std::size_t line_number = 0;

  while (offset < content.size()) {
    ++line_number;
    const std::size_t newline = content.find('\n', offset);
    const bool is_unterminated_tail = newline == std::string::npos;
    const std::size_t line_end = is_unterminated_tail ? content.size() : newline;
    std::string line = content.substr(offset, line_end - offset);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    offset = is_unterminated_tail ? content.size() : newline + 1;
    if (line.empty()) {
      continue;
    }
    if (line.size() > options.max_line_bytes) {
      throw AnalysisError("line " + std::to_string(line_number) + " exceeds max_line_bytes");
    }

    rapidjson::Document document;
    document.Parse(line.data(), line.size());
    if (document.HasParseError() || !document.IsObject()) {
      if (is_unterminated_tail) {
        add_issue(parsed.issues, "warning", "torn_tail_record",
                  "ignored one unterminated invalid JSON tail record at line " +
                      std::to_string(line_number));
        continue;
      }
      const std::string parse_message = document.HasParseError()
                                            ? rapidjson::GetParseError_En(document.GetParseError())
                                            : "root is not an object";
      throw AnalysisError("invalid JSON record at line " + std::to_string(line_number) + ": " +
                          parse_message);
    }

    const std::string type = member_string(document, "type");
    if (type == "dialogue.header") {
      const std::string header_dialogue = member_string(document, "dialogue_id");
      if (!header_dialogue.empty()) {
        if (!parsed.dialogue_id.empty() && parsed.dialogue_id != header_dialogue) {
          add_issue(parsed.issues, "error", "multiple_dialogues",
                    "header dialogue_id conflicts with an earlier record");
        } else {
          parsed.dialogue_id = header_dialogue;
        }
      }
      const std::string header_conversation = member_string(document, "conversation_id");
      if (!header_conversation.empty()) {
        if (!parsed.conversation_id.empty() && parsed.conversation_id != header_conversation) {
          add_issue(parsed.issues, "error", "conflicting_conversation_id",
                    "header conversation_id conflicts with an earlier record");
        } else {
          parsed.conversation_id = header_conversation;
        }
      }
      const std::string header_user = member_string(document, "user_id");
      if (!header_user.empty()) {
        if (!parsed.user_id.empty() && parsed.user_id != header_user) {
          add_issue(parsed.issues, "error", "conflicting_user_id",
                    "header user_id conflicts with an earlier record");
        } else {
          parsed.user_id = header_user;
        }
      }
      continue;
    }

    EventRecord event;
    event.line_number = line_number;
    event.type = type;
    event.event_id = member_string(document, "event_id");
    event.idempotency_key = member_string(document, "idempotency_key");
    event.dialogue_id = member_string(document, "dialogue_id");
    event.producer_id = event_producer_id(document);
    event.producer_sequence = member_uint64(document, "producer_sequence");
    if (event.producer_sequence == 0) {
      event.producer_sequence = member_uint64(document, "seq");
    }
    event.timestamp_us = member_uint64(document, "timestamp_us");
    if (event.timestamp_us == 0) {
      const std::uint64_t timestamp_ms = member_uint64(document, "ts");
      if (timestamp_ms <= std::numeric_limits<std::uint64_t>::max() / 1000U) {
        event.timestamp_us = timestamp_ms * 1000U;
      }
    }
    event.round = member_uint64(document, "round");
    event.node_id = member_string(document, "node_id");

    if (event.event_id.empty()) {
      event.event_id = "synthetic:line:" + std::to_string(line_number);
      add_issue(parsed.issues, "error", "missing_event_id",
                "event at line " + std::to_string(line_number) + " has no event_id",
                {event.event_id});
    }
    if (event.type.empty()) {
      add_issue(parsed.issues, "error", "missing_event_type", "event has no type",
                {event.event_id});
    }
    if (event.producer_id.empty()) {
      event.producer_id = "synthetic:producer:line:" + std::to_string(line_number);
      add_issue(parsed.issues, "error", "missing_producer_id",
                "event has no producer_id or legacy producer identity", {event.event_id});
    }
    if (event.producer_sequence == 0) {
      event.producer_sequence = line_number;
      add_issue(parsed.issues, "error", "missing_producer_sequence",
                "event has no positive producer_sequence or legacy seq", {event.event_id});
    }
    if (event.timestamp_us == 0) {
      add_issue(parsed.issues, "error", "missing_timestamp",
                "event has no positive timestamp_us or legacy ts", {event.event_id});
    }
    if (event.idempotency_key.empty()) {
      event.idempotency_key = event.event_id;
      add_issue(parsed.issues, "warning", "missing_idempotency_key",
                "event uses event_id as an idempotency fallback", {event.event_id});
    }

    const std::string record_fingerprint = fingerprint(canonical_json(document));
    const auto seen = seen_idempotency.find(event.idempotency_key);
    if (seen != seen_idempotency.end()) {
      ++parsed.duplicate_event_count;
      add_issue(parsed.issues, seen->second == record_fingerprint ? "warning" : "error",
                seen->second == record_fingerprint ? "duplicate_event"
                                                   : "conflicting_duplicate_event",
                seen->second == record_fingerprint
                    ? "ignored an exact duplicate event"
                    : "ignored a conflicting event with the same idempotency key",
                {event.event_id});
      continue;
    }
    seen_idempotency.emplace(event.idempotency_key, record_fingerprint);

    if (!event.dialogue_id.empty()) {
      if (parsed.dialogue_id.empty()) {
        parsed.dialogue_id = event.dialogue_id;
      } else if (parsed.dialogue_id != event.dialogue_id) {
        add_issue(parsed.issues, "error", "multiple_dialogues",
                  "input contains events from multiple dialogue_id values", {event.event_id});
      }
    }

    if (!event.producer_id.empty() && event.producer_sequence > 0) {
      const auto previous = last_sequence.find(event.producer_id);
      if (previous != last_sequence.end()) {
        if (event.producer_sequence <= previous->second) {
          add_issue(parsed.issues, "error", "producer_sequence_not_monotonic",
                    "producer sequence did not increase in file order", {event.event_id});
        } else if (event.producer_sequence > previous->second + 1) {
          add_issue(parsed.issues, "warning", "producer_sequence_gap",
                    "producer sequence contains a gap before this event", {event.event_id});
        }
      }
      last_sequence[event.producer_id] = event.producer_sequence;
    }

    event.document.Swap(document);
    parsed.events.push_back(std::move(event));
  }

  if (parsed.dialogue_id.empty()) {
    add_issue(parsed.issues, "error", "missing_dialogue_id",
              "input does not contain a dialogue_id");
  }
  return parsed;
}

// 从事件载荷读取指定字符串字段，遵循缺失值约定。
std::string event_data_string(const EventRecord &event, const char *name) {
  const JsonValue *data = event_data(event);
  return data == nullptr ? std::string() : member_string(*data, name);
}

// 从事件载荷读取非负数值，拒绝不符合契约的表示。
std::uint64_t event_data_uint64(const EventRecord &event, const char *name) {
  const JsonValue *data = event_data(event);
  return data == nullptr ? 0 : member_uint64(*data, name);
}

// 读取事件所属轮次，不能仅用接收顺序推断执行轮号。
std::uint64_t event_round(const EventRecord &event) {
  return event.round != 0 ? event.round : event_data_uint64(event, "round");
}

// 从现有身份字段推导分析作用域，缺失信息不伪造为完整归属。
LoopScope infer_scope(const EventRecord &request, const SkillIdentity *identity) {
  const std::string task_type = event_data_string(request, "task_type");
  const std::string skill_name = event_data_string(request, "skill_name");
  if (task_type == "coordinator" || skill_name == "coordinator" ||
      contains(request.node_id, "coordinator")) {
    return LoopScope::coordinator;
  }
  if (task_type == "skill" || (identity != nullptr && !identity->exec_skill_key.empty()) ||
      contains(request.node_id, "skill")) {
    return LoopScope::skill;
  }
  return LoopScope::unknown;
}

// 收集真实工具调用身份，供请求与返回配对。
std::vector<std::string> collect_tool_call_ids(const JsonValue *value) {
  std::vector<std::string> output;
  if (value == nullptr || !value->IsArray()) {
    return output;
  }
  for (const auto &message : value->GetArray()) {
    if (!message.IsObject() || member_string(message, "role") != "tool") {
      continue;
    }
    append_unique(output, member_string(message, "tool_call_id"));
  }
  return output;
}

// 按调用身份索引工具返回，避免并行返回顺序改变归属。
std::unordered_map<std::string, ToolResultRecord>
index_tool_results(const std::vector<EventRecord> &events, std::vector<TraceIssue> &issues) {
  std::unordered_map<std::string, ToolResultRecord> output;
  for (const auto &event : events) {
    if (event.type != "tool.result") {
      continue;
    }
    const JsonValue *data = event_data(event);
    const JsonValue *results = data == nullptr ? nullptr : find_member(*data, "tool_results");
    if (results == nullptr || !results->IsArray()) {
      add_issue(issues, "error", "tool_result_missing_items",
                "tool.result has no tool_results array", {event.event_id});
      continue;
    }
    for (const auto &result : results->GetArray()) {
      if (!result.IsObject()) {
        continue;
      }
      ToolResultRecord record;
      record.event = &event;
      record.tool_call_id = member_string(result, "tool_call_id");
      record.tool_name = member_string(result, "tool_name");
      record.status = member_string(result, "status");
      const std::string result_text = value_as_text(find_member(result, "result"));
      const bool failed = status_is_failure(record.status, result_text);
      if (failed) {
        record.error_class = classify_error(result_text);
        record.retryability = retryability_for(record.error_class);
      } else {
        record.retryability = "not_applicable";
      }
      record.result_fingerprint =
          fingerprint(normalized_result_text(record.status + '\n' + result_text));
      if (record.tool_call_id.empty()) {
        add_issue(issues, "error", "tool_result_missing_call_id",
                  "tool result item has no tool_call_id", {event.event_id});
        continue;
      }
      const auto inserted = output.emplace(record.tool_call_id, record);
      if (!inserted.second) {
        add_issue(issues, "error", "duplicate_tool_result",
                  "multiple tool results use the same tool_call_id",
                  {inserted.first->second.event->event_id, event.event_id});
      }
    }
  }
  return output;
}

// 解析派发记录及目标信息，区分调度动作与任务真正完成。
DispatchSummary
parse_dispatch(const EventRecord &dispatch_event,
               const std::unordered_map<std::string, const EventRecord *> &batch_results) {
  DispatchSummary output;
  const JsonValue *data = event_data(dispatch_event);
  if (data == nullptr || !data->IsObject()) {
    return output;
  }
  output.batch_id = member_string(*data, "batch_id");
  output.skill_names = string_array(find_member(*data, "skill_names"));
  const JsonValue *dispatches = find_member(*data, "dispatches");
  if (dispatches != nullptr && dispatches->IsArray()) {
    output.skill_names.clear();
    for (const auto &item : dispatches->GetArray()) {
      if (!item.IsObject()) {
        continue;
      }
      output.skill_names.push_back(member_string(item, "skill_name"));
      const std::string prompt = member_string(item, "prompt");
      output.prompt_fingerprints.push_back(prompt.empty() ? std::string()
                                                          : fingerprint("prompt-v1\n" + prompt));
      output.force.push_back(member_bool(item, "force"));
    }
  }
  while (output.prompt_fingerprints.size() < output.skill_names.size()) {
    output.prompt_fingerprints.emplace_back();
  }
  while (output.force.size() < output.skill_names.size()) {
    output.force.push_back(false);
  }

  const auto batch = batch_results.find(output.batch_id);
  if (batch != batch_results.end()) {
    const JsonValue *batch_data = event_data(*batch->second);
    if (batch_data != nullptr) {
      output.batch_status = member_string(*batch_data, "status");
      output.workflow_status = member_string(*batch_data, "workflow_status");
      output.successful_count =
          static_cast<std::size_t>(member_uint64(*batch_data, "successful_count"));
      output.total_count = static_cast<std::size_t>(member_uint64(*batch_data, "total_count"));
    }
  }
  return output;
}

// 提取派发动作中用于比较的有效内容，剥离传输噪声。
std::string dispatch_action_material(const DispatchSummary &dispatch) {
  std::string material = "dispatch-v1";
  for (std::size_t index = 0; index < dispatch.skill_names.size(); ++index) {
    material += "\n" + dispatch.skill_names[index];
    material += "\n" + dispatch.prompt_fingerprints[index];
    material += dispatch.force[index] ? "\nforce" : "\nnormal";
  }
  return material;
}

// 规范化工具名称集合，作为重复行为比较的一部分。
std::string tool_names_key(const LoopEpisode &episode) {
  std::string output;
  for (const auto &action : episode.tool_actions) {
    output += action.tool_name + ';';
  }
  return output;
}

// 读取当前片段的首个错误类别，为失败归因提供起点。
std::string first_error_class(const LoopEpisode &episode) {
  for (const auto &action : episode.tool_actions) {
    if (!action.error_class.empty()) {
      return action.error_class;
    }
  }
  return {};
}

// 定位首个失败工具，避免将之后的连带错误当作最初原因。
std::string first_error_tool(const LoopEpisode &episode) {
  for (const auto &action : episode.tool_actions) {
    if (!action.error_class.empty()) {
      return action.tool_name;
    }
  }
  return {};
}

// 依据事件身份和因果关系构造执行片段，不能把并行相邻事件误当串行重试。
std::vector<EpisodeWork>
build_episodes(ParsedInput &parsed,
               const std::unordered_map<std::string, ToolResultRecord> &tool_results) {
  std::unordered_map<std::string, std::vector<const EventRecord *>> by_producer;
  std::unordered_map<std::string, SkillIdentity> producer_skills;
  std::unordered_map<std::string, std::uint64_t> producer_activations;
  std::unordered_map<std::string, const EventRecord *> batch_results;

  for (const auto &event : parsed.events) {
    by_producer[event.producer_id].push_back(&event);
    if (event.type == "skill.start") {
      SkillIdentity identity;
      identity.skill_name = event_data_string(event, "skill_name");
      identity.exec_skill_key = event_data_string(event, "exec_skill_key");
      identity.batch_id = event_data_string(event, "batch_id");
      identity.max_rounds = event_data_uint64(event, "max_rounds");
      producer_skills[event.producer_id] = std::move(identity);
    } else if (event.type == "coordinator.activation") {
      producer_activations[event.producer_id] = event_data_uint64(event, "activation_index");
    } else if (event.type == "batch.result") {
      const std::string batch_id = event_data_string(event, "batch_id");
      if (!batch_id.empty()) {
        batch_results[batch_id] = &event;
      }
    }
  }

  std::vector<EpisodeWork> episodes;
  for (auto &[producer_id, producer_events] : by_producer) {
    (void)producer_id;
    std::sort(producer_events.begin(), producer_events.end(),
              [](const EventRecord *lhs, const EventRecord *rhs) {
                if (lhs->producer_sequence != rhs->producer_sequence) {
                  return lhs->producer_sequence < rhs->producer_sequence;
                }
                if (lhs->timestamp_us != rhs->timestamp_us) {
                  return lhs->timestamp_us < rhs->timestamp_us;
                }
                return lhs->event_id < rhs->event_id;
              });

    std::vector<std::size_t> request_indices;
    for (std::size_t index = 0; index < producer_events.size(); ++index) {
      if (producer_events[index]->type == "llm.request") {
        request_indices.push_back(index);
      }
    }
    if (request_indices.empty()) {
      continue;
    }

    const auto skill_identity_it = producer_skills.find(producer_id);
    const SkillIdentity *skill_identity =
        skill_identity_it == producer_skills.end() ? nullptr : &skill_identity_it->second;

    for (std::size_t request_ordinal = 0; request_ordinal < request_indices.size();
         ++request_ordinal) {
      const std::size_t begin = request_indices[request_ordinal];
      const std::size_t end = request_ordinal + 1 < request_indices.size()
                                  ? request_indices[request_ordinal + 1]
                                  : producer_events.size();
      const EventRecord &request = *producer_events[begin];

      EpisodeWork work;
      work.request = &request;
      LoopEpisode &episode = work.episode;
      episode.dialogue_id = parsed.dialogue_id;
      episode.scope = infer_scope(request, skill_identity);
      episode.request_event_id = request.event_id;
      episode.started_at_us = request.timestamp_us;
      episode.ended_at_us = request.timestamp_us;
      episode.round = event_round(request);
      episode.skill_name = event_data_string(request, "skill_name");
      episode.exec_skill_key = event_data_string(request, "exec_skill_key");
      episode.activation_index = event_data_uint64(request, "activation_index");

      if (episode.scope == LoopScope::skill && skill_identity != nullptr) {
        if (episode.skill_name.empty()) {
          episode.skill_name = skill_identity->skill_name;
        }
        if (episode.exec_skill_key.empty()) {
          episode.exec_skill_key = skill_identity->exec_skill_key;
        }
        episode.owner_id = !episode.exec_skill_key.empty() ? episode.exec_skill_key : producer_id;
      } else if (episode.scope == LoopScope::coordinator) {
        if (episode.activation_index == 0) {
          const auto activation = producer_activations.find(producer_id);
          if (activation != producer_activations.end()) {
            episode.activation_index = activation->second;
          }
        }
        episode.skill_name.clear();
        episode.owner_id = "coordinator:" + parsed.dialogue_id;
      } else {
        episode.owner_id = producer_id;
      }

      const JsonValue *request_data = event_data(request);
      const JsonValue *messages =
          request_data == nullptr ? nullptr : find_member(*request_data, "messages");
      if (messages != nullptr) {
        episode.input_fingerprint = fingerprint(canonical_json(*messages));
        episode.input_tool_call_ids = collect_tool_call_ids(messages);
      }

      const EventRecord *response = nullptr;
      const EventRecord *control = nullptr;
      const EventRecord *dispatch = nullptr;
      const EventRecord *terminal = nullptr;
      std::vector<const EventRecord *> tool_call_events;

      for (std::size_t index = begin; index < end; ++index) {
        const EventRecord &event = *producer_events[index];
        append_unique(episode.source_event_ids, event.event_id);
        episode.ended_at_us = std::max(episode.ended_at_us, event.timestamp_us);
        if (event.type == "llm.response" && response == nullptr) {
          response = &event;
        } else if (event.type == "loop.control") {
          control = &event;
        } else if (event.type == "skill.dispatch") {
          dispatch = &event;
        } else if (event.type == "tool.call") {
          tool_call_events.push_back(&event);
        } else if (event.type == "skill.end" || event.type == "dialogue.end") {
          terminal = &event;
        }
      }

      if (response != nullptr) {
        episode.response_event_id = response->event_id;
      } else {
        add_issue(parsed.issues, "error", "llm_request_without_response",
                  "LLM request has no response in the same producer segment", {request.event_id});
      }

      if (control != nullptr) {
        episode.control_event_id = control->event_id;
        episode.action = event_data_string(*control, "action");
        episode.reason = event_data_string(*control, "reason");
        episode.continued = episode.action == "continue" || episode.action == "retry";
        episode.terminal = episode.action == "stop" || episode.action == "abort";
      } else if (response != nullptr) {
        add_issue(parsed.issues, "warning", "llm_response_without_loop_control",
                  "LLM response has no explicit loop.control decision", {response->event_id});
      }

      if (dispatch != nullptr) {
        episode.dispatch = parse_dispatch(*dispatch, batch_results);
        if (episode.action.empty()) {
          episode.action = "dispatch_skills";
          episode.reason = "skill_dispatch";
          episode.continued = true;
        }
      }

      if (terminal != nullptr) {
        episode.terminal = true;
        episode.terminal_status = event_data_string(*terminal, "status");
        episode.outcome_failed = terminal_status_is_failure(episode.terminal_status);
        if (episode.action.empty()) {
          episode.action = "stop";
          episode.reason = "terminal_event";
        }
      }

      std::string action_material;
      std::string outcome_material;
      std::string outcome_class_material;
      std::size_t failed_tool_count = 0;
      for (const EventRecord *call_event : tool_call_events) {
        const JsonValue *data = event_data(*call_event);
        const JsonValue *calls = data == nullptr ? nullptr : find_member(*data, "tool_calls");
        if (calls == nullptr || !calls->IsArray()) {
          add_issue(parsed.issues, "error", "tool_call_missing_items",
                    "tool.call has no tool_calls array", {call_event->event_id});
          continue;
        }
        for (const auto &call : calls->GetArray()) {
          if (!call.IsObject()) {
            continue;
          }
          ToolActionSummary action;
          action.tool_call_id = member_string(call, "tool_call_id");
          action.tool_name = member_string(call, "tool_name");
          action.call_event_id = call_event->event_id;
          const JsonValue *arguments = find_member(call, "arguments");
          std::string argument_material;
          if (arguments != nullptr) {
            argument_material = canonical_json(*arguments);
          } else {
            const std::string arguments_json = member_string(call, "arguments_json");
            rapidjson::Document parsed_arguments;
            parsed_arguments.Parse(arguments_json.data(), arguments_json.size());
            argument_material = !arguments_json.empty() && !parsed_arguments.HasParseError()
                                    ? canonical_json(parsed_arguments)
                                    : arguments_json;
          }
          action.arguments_fingerprint = fingerprint(argument_material);

          const auto result = tool_results.find(action.tool_call_id);
          if (result != tool_results.end()) {
            action.status = result->second.status;
            action.result_fingerprint = result->second.result_fingerprint;
            action.error_class = result->second.error_class;
            action.retryability = result->second.retryability;
            action.result_event_id = result->second.event->event_id;
            append_unique(episode.source_event_ids, action.result_event_id);
            episode.ended_at_us = std::max(episode.ended_at_us, result->second.event->timestamp_us);
          } else {
            action.status = "missing";
            action.retryability = "unknown";
            add_issue(parsed.issues, "error", "tool_call_without_result",
                      "tool call has no result with the same tool_call_id", {call_event->event_id});
          }

          const bool failed = action.status == "missing" || !action.error_class.empty();
          if (failed) {
            ++failed_tool_count;
          }
          if (action.retryability == "non_retryable") {
            episode.has_non_retryable_failure = true;
          }
          action_material += action.tool_name + ':' + action.arguments_fingerprint + ';';
          outcome_material += action.tool_name + ':' + action.status + ':' + action.error_class +
                              ':' + action.result_fingerprint + ';';
          outcome_class_material +=
              action.tool_name + ':' + action.status + ':' + action.error_class + ';';
          episode.tool_actions.push_back(std::move(action));
        }
      }

      if (!episode.tool_actions.empty()) {
        episode.all_tool_results_failed = failed_tool_count == episode.tool_actions.size();
        episode.outcome_failed = episode.outcome_failed || failed_tool_count > 0;
      }

      if (dispatch != nullptr) {
        action_material += dispatch_action_material(episode.dispatch);
        outcome_material += "batch:" + episode.dispatch.batch_status + ':' +
                            episode.dispatch.workflow_status + ':' +
                            std::to_string(episode.dispatch.successful_count) + '/' +
                            std::to_string(episode.dispatch.total_count);
        outcome_class_material +=
            "batch:" + episode.dispatch.batch_status + ':' + episode.dispatch.workflow_status;
        if (episode.dispatch.batch_status == "false") {
          episode.outcome_failed = true;
        }
      }

      if (action_material.empty()) {
        action_material = episode.action + ':' + episode.reason;
      }
      if (outcome_material.empty()) {
        outcome_material = episode.terminal_status + ':' + episode.action;
      }
      if (outcome_class_material.empty()) {
        outcome_class_material = outcome_material;
      }
      episode.action_fingerprint = fingerprint(action_material);
      episode.outcome_fingerprint = fingerprint(outcome_material);
      episode.outcome_class_fingerprint = fingerprint(outcome_class_material);
      episodes.push_back(std::move(work));
    }
  }

  std::map<std::string, std::vector<std::size_t>> by_owner;
  for (std::size_t index = 0; index < episodes.size(); ++index) {
    by_owner[episodes[index].episode.owner_id].push_back(index);
  }
  for (auto &[owner, indices] : by_owner) {
    (void)owner;
    std::sort(indices.begin(), indices.end(), [&episodes](std::size_t lhs, std::size_t rhs) {
      const LoopEpisode &left = episodes[lhs].episode;
      const LoopEpisode &right = episodes[rhs].episode;
      if (left.scope == LoopScope::coordinator && left.activation_index != right.activation_index) {
        return left.activation_index < right.activation_index;
      }
      if (left.started_at_us != right.started_at_us) {
        return left.started_at_us < right.started_at_us;
      }
      return left.request_event_id < right.request_event_id;
    });
    std::map<std::uint64_t, std::uint64_t> attempts;
    for (std::size_t position = 0; position < indices.size(); ++position) {
      LoopEpisode &episode = episodes[indices[position]].episode;
      episode.step_index = position + 1;
      episode.attempt_index = ++attempts[episode.round];
      episode.episode_id = "loop:" + to_string(episode.scope) + ':' +
                           fingerprint(episode.owner_id) + ':' + std::to_string(episode.step_index);
    }
  }

  std::sort(episodes.begin(), episodes.end(), [](const EpisodeWork &lhs, const EpisodeWork &rhs) {
    if (lhs.episode.started_at_us != rhs.episode.started_at_us) {
      return lhs.episode.started_at_us < rhs.episode.started_at_us;
    }
    return lhs.episode.episode_id < rhs.episode.episode_id;
  });
  return episodes;
}

// 检查当前片段是否已有指定诊断，避免重复添加同一问题。
bool has_diagnostic(const AnalysisReport &report, const LoopEpisode &episode,
                    InvalidLoopCategory category) {
  const std::string category_name = to_string(category);
  for (const auto &diagnostic_id : episode.diagnostic_ids) {
    const auto diagnostic = std::find_if(report.diagnostics.begin(), report.diagnostics.end(),
                                         [&diagnostic_id](const LoopDiagnostic &candidate) {
                                           return candidate.diagnostic_id == diagnostic_id;
                                         });
    if (diagnostic != report.diagnostics.end() &&
        to_string(diagnostic->category) == category_name) {
      return true;
    }
  }
  return false;
}

// 把诊断与片段证据绑定后加入结果，保持可回放定位。
void add_diagnostic(AnalysisReport &report, LoopEpisode &episode, InvalidLoopCategory category,
                    bool counts_as_invalid, double confidence, const std::string &severity,
                    const std::string &summary, const std::string &observable_cause,
                    const std::string &tool_name, const std::string &error_class,
                    std::vector<std::string> evidence, const std::string &strategy_key) {
  if (has_diagnostic(report, episode, category)) {
    return;
  }
  LoopDiagnostic diagnostic;
  diagnostic.episode_id = episode.episode_id;
  diagnostic.category = category;
  diagnostic.counts_as_invalid = counts_as_invalid;
  diagnostic.confidence = confidence;
  diagnostic.severity = severity;
  diagnostic.summary = summary;
  diagnostic.observable_cause = observable_cause;
  diagnostic.tool_name = tool_name;
  diagnostic.error_class = error_class;
  diagnostic.evidence_event_ids = std::move(evidence);
  diagnostic.strategy_key = strategy_key;
  diagnostic.diagnostic_id =
      "diagnostic:" + fingerprint(diagnostic.episode_id + ':' + to_string(category) + ':' +
                                  tool_name + ':' + error_class);
  episode.diagnostic_ids.push_back(diagnostic.diagnostic_id);
  report.diagnostics.push_back(std::move(diagnostic));
}

// 提取片段对应的真实事件引用，供评测和反思使用。
std::vector<std::string> episode_evidence(const LoopEpisode &previous, const LoopEpisode &current) {
  std::vector<std::string> output;
  append_unique(output, previous.control_event_id);
  append_unique(output, previous.response_event_id);
  for (const auto &tool : previous.tool_actions) {
    append_unique(output, tool.call_event_id);
    append_unique(output, tool.result_event_id);
  }
  append_unique(output, current.control_event_id);
  append_unique(output, current.response_event_id);
  for (const auto &tool : current.tool_actions) {
    append_unique(output, tool.call_event_id);
    append_unique(output, tool.result_event_id);
  }
  return output;
}

// 检查派发是否都未显式强制重排，区分正常调度与特殊逃生路径。
bool all_force_false(const DispatchSummary &dispatch) {
  return std::none_of(dispatch.force.begin(), dispatch.force.end(),
                      [](bool force) { return force; });
}

// 识别具备确定性证据的重复或无进展循环，保留不确定片段而不强判浪费。
void detect_invalid_loops(AnalysisReport &report) {
  std::map<std::string, std::vector<std::size_t>> by_owner;
  for (std::size_t index = 0; index < report.episodes.size(); ++index) {
    by_owner[report.episodes[index].owner_id].push_back(index);
  }

  for (auto &[owner, indices] : by_owner) {
    (void)owner;
    std::sort(indices.begin(), indices.end(), [&report](std::size_t lhs, std::size_t rhs) {
      return report.episodes[lhs].step_index < report.episodes[rhs].step_index;
    });
    std::map<std::pair<std::string, std::string>, std::size_t> non_retryable_counts;
    std::unordered_map<std::string, std::size_t> prior_dispatch;
    std::size_t same_outcome_streak = 1;

    for (std::size_t position = 0; position < indices.size(); ++position) {
      LoopEpisode &current = report.episodes[indices[position]];

      if (contains(lower_copy(current.terminal_status), "max_round")) {
        add_diagnostic(report, current, InvalidLoopCategory::max_rounds_exhausted, false, 0.65,
                       "warning", "Skill 因耗尽轮次预算而终止。",
                       "可以观察到轮次预算已经耗尽，但仅凭当前轨迹无法证明此前哪些轮次"
                       "可以避免。",
                       {}, {}, current.source_event_ids, {});
      }

      if (!current.dispatch.skill_names.empty()) {
        const std::string dispatch_key = current.action_fingerprint;
        const auto prior = prior_dispatch.find(dispatch_key);
        if (prior != prior_dispatch.end() && all_force_false(current.dispatch)) {
          const LoopEpisode &previous = report.episodes[prior->second];
          const bool prior_terminal = !previous.dispatch.batch_status.empty();
          add_diagnostic(
              report, current,
              prior_terminal ? InvalidLoopCategory::invalid_action_repeat
                             : InvalidLoopCategory::review_required,
              prior_terminal, prior_terminal ? 0.99 : 0.60, prior_terminal ? "error" : "warning",
              prior_terminal ? "前一个 Batch 已产生终态结果后，Coordinator 仍使用相同 Prompt "
                               "重复派发相同 Skill。"
                             : "在前一个 Batch 的终态结果尚不可观察时，Coordinator 重复派发了"
                               "相同 Skill。",
              "Skill 名称、Prompt 指纹及 force=false 标记均未发生变化。", {}, {},
              episode_evidence(previous, current),
              prior_terminal ? "coordinator:duplicate_dispatch" : std::string());
        }
        prior_dispatch[dispatch_key] = indices[position];
      }

      std::set<std::pair<std::string, std::string>> episode_failures;
      std::set<std::string> successful_tools;
      for (const auto &tool : current.tool_actions) {
        if (tool.retryability == "non_retryable") {
          episode_failures.emplace(tool.tool_name, tool.error_class);
        } else if (tool.retryability == "not_applicable") {
          successful_tools.insert(tool.tool_name);
        }
      }
      for (auto count = non_retryable_counts.begin(); count != non_retryable_counts.end();) {
        if (episode_failures.count(count->first) == 0 ||
            successful_tools.count(count->first.first) != 0) {
          count = non_retryable_counts.erase(count);
        } else {
          ++count;
        }
      }

      std::set<std::pair<std::string, std::string>> counted_failures;
      for (const auto &tool : current.tool_actions) {
        if (tool.retryability != "non_retryable" || successful_tools.count(tool.tool_name) != 0) {
          continue;
        }
        const auto failure = std::make_pair(tool.tool_name, tool.error_class);
        if (!counted_failures.insert(failure).second) {
          continue;
        }
        const std::size_t count = ++non_retryable_counts[failure];
        const bool strict_class = tool.error_class == "authentication_failed" ||
                                  tool.error_class == "permission_denied" ||
                                  tool.error_class == "policy_blocked";
        const std::size_t threshold = strict_class ? 2 : 3;
        if (count >= threshold) {
          add_diagnostic(report, current, InvalidLoopCategory::non_retryable_repeat, true,
                         strict_class ? 0.98 : 0.92, "error", "同一不可重试错误类别被重复观察到。",
                         "改变参数后，可观察到的失败类别仍为 " + tool.error_class + "。",
                         tool.tool_name, tool.error_class,
                         {tool.call_event_id, tool.result_event_id},
                         to_string(current.scope) + ':' + current.skill_name +
                             ":non_retryable:" + tool.tool_name + ':' + tool.error_class);
        }
      }

      if (position == 0) {
        continue;
      }
      LoopEpisode &previous = report.episodes[indices[position - 1]];

      if (!previous.tool_actions.empty() && previous.continued) {
        std::vector<std::string> missing_ids;
        for (const auto &tool : previous.tool_actions) {
          if (tool.tool_call_id.empty()) {
            continue;
          }
          if (std::find(current.input_tool_call_ids.begin(), current.input_tool_call_ids.end(),
                        tool.tool_call_id) == current.input_tool_call_ids.end()) {
            missing_ids.push_back(tool.tool_call_id);
          }
        }
        if (!missing_ids.empty()) {
          std::vector<std::string> evidence = {
              previous.request_event_id,
              current.request_event_id,
          };
          for (const auto &tool : previous.tool_actions) {
            append_unique(evidence, tool.result_event_id);
          }
          add_diagnostic(
              report, current, InvalidLoopCategory::result_not_consumed, true, 0.97, "error",
              "一个或多个 Tool 结果未进入下一次 LLM 请求。",
              "下一次记录的 messages 未包含此前所有 tool_call_id。", {}, {}, std::move(evidence),
              to_string(current.scope) + ':' + current.skill_name + ":consume_tool_results");
        }
      }

      const bool same_outcome =
          current.outcome_class_fingerprint == previous.outcome_class_fingerprint;
      const bool same_tools =
          tool_names_key(current) == tool_names_key(previous) && !current.tool_actions.empty();
      if (same_outcome && same_tools && current.outcome_failed) {
        ++same_outcome_streak;
      } else {
        same_outcome_streak = 1;
      }

      const bool exact_repeat = current.action_fingerprint == previous.action_fingerprint &&
                                same_outcome && current.outcome_failed &&
                                !current.tool_actions.empty();
      if (exact_repeat) {
        const std::string error_class = first_error_class(current);
        const std::string tool_name = first_error_tool(current);
        const bool non_retryable = current.has_non_retryable_failure;
        const bool repeated_beyond_one_retry = same_outcome_streak >= 3;
        add_diagnostic(report, current,
                       non_retryable || repeated_beyond_one_retry
                           ? InvalidLoopCategory::exact_repeat
                           : InvalidLoopCategory::review_required,
                       non_retryable || repeated_beyond_one_retry,
                       non_retryable ? 0.99 : (repeated_beyond_one_retry ? 0.88 : 0.55),
                       non_retryable || repeated_beyond_one_retry ? "error" : "warning",
                       non_retryable || repeated_beyond_one_retry
                           ? "相同 Tool 动作再次产生了相同失败结果。"
                           : "对可重试或未知失败执行了一次相同动作的重试。",
                       "Tool 名称、规范化参数指纹和结果类别均未发生变化。", tool_name, error_class,
                       episode_evidence(previous, current),
                       non_retryable || repeated_beyond_one_retry
                           ? to_string(current.scope) + ':' + current.skill_name +
                                 ":exact_repeat:" + tool_name + ':' + error_class
                           : std::string());
      } else if (same_outcome_streak >= 3 && current.outcome_failed && same_tools &&
                 !has_diagnostic(report, current, InvalidLoopCategory::non_retryable_repeat)) {
        add_diagnostic(report, current, InvalidLoopCategory::no_state_progress, true, 0.82, "error",
                       "至少连续三次尝试产生了相同的失败结果类别。",
                       "Tool 集合与规范化结果类别没有发生足以表明取得进展的变化。",
                       first_error_tool(current), first_error_class(current),
                       episode_evidence(previous, current),
                       to_string(current.scope) + ':' + current.skill_name +
                           ":no_progress:" + first_error_tool(current));
      }

      if (position >= 2) {
        const LoopEpisode &two_back = report.episodes[indices[position - 2]];
        if (current.action_fingerprint == two_back.action_fingerprint &&
            current.action_fingerprint != previous.action_fingerprint &&
            current.outcome_class_fingerprint == two_back.outcome_class_fingerprint &&
            current.outcome_failed) {
          auto evidence = episode_evidence(two_back, current);
          append_unique(evidence, previous.request_event_id);
          add_diagnostic(report, current, InvalidLoopCategory::state_oscillation, true, 0.84,
                         "error", "动作/结果模式回到了两个步骤之前的状态。",
                         "A-B-A 指纹模式表明执行过程发生了振荡。", first_error_tool(current),
                         first_error_class(current), std::move(evidence),
                         to_string(current.scope) + ':' + current.skill_name +
                             ":oscillation:" + first_error_tool(current));
        }
      }
    }
  }
}

// 把评测结果关联到对应执行片段，避免跨轮次附着。
void attach_evaluations(AnalysisReport &report, const std::vector<EventRecord> &events) {
  std::unordered_map<std::string, std::size_t> event_to_episode;
  for (std::size_t episode_index = 0; episode_index < report.episodes.size(); ++episode_index) {
    for (const auto &event_id : report.episodes[episode_index].source_event_ids) {
      event_to_episode[event_id] = episode_index;
    }
  }

  for (const auto &event : events) {
    if (event.type != "evaluation.result") {
      continue;
    }
    const JsonValue *data = event_data(event);
    if (data == nullptr) {
      continue;
    }
    EvaluationSummary evaluation;
    evaluation.event_id = event.event_id;
    evaluation.evaluator = member_string(*data, "evaluator");
    evaluation.evaluator_version = member_string(*data, "evaluator_version");
    evaluation.metric = member_string(*data, "metric");
    evaluation.score = member_double(*data, "score", &evaluation.has_score);
    const JsonValue *passed = find_member(*data, "passed");
    if (passed != nullptr && passed->IsBool()) {
      evaluation.has_passed = true;
      evaluation.passed = passed->GetBool();
    }
    const auto targets = string_array(find_member(*data, "target_event_ids"));
    std::set<std::size_t> target_episodes;
    for (const auto &target : targets) {
      const auto episode = event_to_episode.find(target);
      if (episode == event_to_episode.end()) {
        continue;
      }
      target_episodes.insert(episode->second);
    }
    for (const std::size_t episode_index : target_episodes) {
      report.episodes[episode_index].evaluations.push_back(evaluation);
      append_unique(report.episodes[episode_index].source_event_ids, event.event_id);
    }
    if (target_episodes.empty()) {
      add_issue(report.trace_issues, "warning", "orphan_evaluation",
                targets.empty() ? "evaluation.result has no target_event_ids"
                                : "evaluation.result targets no extracted Loop Episode",
                {event.event_id});
    }
  }
}

// 根据诊断构造局部反思问题，限制模型只解释相关执行事实。
std::string reflection_question(InvalidLoopCategory category) {
  switch (category) {
  case InvalidLoopCategory::exact_repeat:
    return "重复动作是否由新的可观察状态所支持，否则是否应该停止执行？";
  case InvalidLoopCategory::no_state_progress:
    return "原本期望发生什么具体状态变化，为什么连续尝试没有产生该变化？";
  case InvalidLoopCategory::non_retryable_repeat:
    return "哪一个稳定阻断条件本应阻止再次尝试？";
  case InvalidLoopCategory::result_not_consumed:
    return "为什么已经记录的 Tool 结果没有进入下一次模型输入？";
  case InvalidLoopCategory::invalid_action_repeat:
    return "是否存在新的用户需求，足以支持使用相同 Prompt 再次派发相同 Skill？";
  case InvalidLoopCategory::state_oscillation:
    return "哪条停止或升级规则可以阻止这种 A-B-A 振荡？";
  case InvalidLoopCategory::max_rounds_exhausted:
    return "此前哪些轮次有效，最早可以在哪一轮观察到没有进展？";
  case InvalidLoopCategory::review_required:
    return "根据记录的可重试性和预算策略，本次重试是否必要？";
  }
  return "哪些可观察证据支持修改循环策略？";
}

// 将需要分析的片段整理成有界反思请求，避免重复复制整段历史。
void build_reflection_requests(AnalysisReport &report) {
  for (const auto &diagnostic : report.diagnostics) {
    const auto episode = std::find_if(report.episodes.begin(), report.episodes.end(),
                                      [&diagnostic](const LoopEpisode &candidate) {
                                        return candidate.episode_id == diagnostic.episode_id;
                                      });
    if (episode == report.episodes.end()) {
      continue;
    }
    ReflectionRequest request;
    request.diagnostic_id = diagnostic.diagnostic_id;
    request.episode_id = diagnostic.episode_id;
    request.scope = to_string(episode->scope);
    request.target = episode->scope == LoopScope::coordinator ? "coordinator" : episode->skill_name;
    request.observation = diagnostic.summary + " " + diagnostic.observable_cause;
    request.evidence_event_ids = diagnostic.evidence_event_ids;
    request.questions = {
        reflection_question(diagnostic.category),
        "针对这一条件，最小的策略或 Prompt 修改是什么？",
        "建议的提前停止或恢复规则可能引入什么回归？",
    };
    request.constraints = {
        "只能引用提供的 evidence_event_ids。",
        "必须区分 Workflow 完成与业务成功。",
        "不得复现密钥、凭据、原始用户数据或完整 Tool Payload。",
        "只能返回候选策略，不得将其标记为 promoted 或 validated。",
    };
    request.reflection_id = "reflection:" + fingerprint(diagnostic.diagnostic_id);
    report.reflection_requests.push_back(std::move(request));
  }
}

// 按诊断目标选择候选经验模板，后续仍需证据和作用域审查。
void memory_template(InvalidLoopCategory category, const LoopDiagnostic &diagnostic,
                     CandidateMemory &memory) {
  switch (category) {
  case InvalidLoopCategory::exact_repeat:
    memory.applicable_condition = "在没有新的可观察状态时，相同 Tool 和规范化参数已经产生过相同"
                                  "失败结果。";
    memory.strategy = "不要再次发出完全相同的 Tool 调用；应停止、升级处理，或请求缺失的"
                      "外部状态变化。";
    memory.risk = "瞬态 Provider 故障可能需要有限重试；必须保留显式重试预算。";
    break;
  case InvalidLoopCategory::no_state_progress:
    memory.applicable_condition = "至少连续三次尝试使用相同 Tool 集合，并产生相同失败结果类别。";
    memory.strategy = "除非存在能够改变状态的新动作，否则应停止猜测参数并报告稳定阻断"
                      "原因。";
    memory.risk = "不同参数偶尔可能成功；需要验证结果类别是否确实稳定。";
    break;
  case InvalidLoopCategory::non_retryable_repeat:
    memory.applicable_condition = "Tool 重复返回不可重试错误类别 " + diagnostic.error_class + "。";
    memory.strategy = "达到有限确认阈值后，停止重试该 Tool，并明确暴露阻断配置或授权"
                      "条件。";
    memory.risk = "对于某些 Provider，错误分类器可能过于宽泛；必须按 Tool 和 Provider "
                  "版本验证。";
    break;
  case InvalidLoopCategory::result_not_consumed:
    memory.applicable_condition =
        "Tool 结果已经存在，但紧随其后的 LLM 请求遗漏了对应 tool_call_id。";
    memory.strategy = "作出下一次决策前，必须将每个 Tool 结果追加到模型历史；否则以轨迹"
                      "完整性错误停止。";
    memory.risk = "旧版 Provider 可能使用其他关联字段；应保留兼容性白名单。";
    break;
  case InvalidLoopCategory::invalid_action_repeat:
    memory.applicable_condition =
        "相同 Skill 名称与 Prompt 指纹的终态 Batch 已经存在，且 force=false。";
    memory.strategy = "复用已完成结果或解释其失败；只有新输入改变 Prompt，或明确说明 "
                      "force 理由时才可重新派发。";
    memory.risk = "终态结果过期或损坏时，可能需要受控的强制重跑。";
    break;
  case InvalidLoopCategory::state_oscillation:
    memory.applicable_condition =
        "相同动作/结果指纹在一个中间替代动作之后再次出现，形成 A-B-A 模式。";
    memory.strategy = "停止振荡并携带两个失败方案升级处理，不要返回未改变的旧动作。";
    memory.risk = "指纹相等只表示结构相同，可能掩盖有意义的外部状态变化。";
    break;
  case InvalidLoopCategory::max_rounds_exhausted:
  case InvalidLoopCategory::review_required:
    break;
  }
}

// 从可解释的分析结果生成候选经验，尚未审核的内容不直接注入。
void build_candidate_memories(AnalysisReport &report) {
  std::unordered_map<std::string, std::size_t> by_strategy;
  for (const auto &diagnostic : report.diagnostics) {
    if (!diagnostic.counts_as_invalid || diagnostic.strategy_key.empty()) {
      continue;
    }
    const auto episode = std::find_if(report.episodes.begin(), report.episodes.end(),
                                      [&diagnostic](const LoopEpisode &candidate) {
                                        return candidate.episode_id == diagnostic.episode_id;
                                      });
    if (episode == report.episodes.end()) {
      continue;
    }

    const auto existing = by_strategy.find(diagnostic.strategy_key);
    if (existing == by_strategy.end()) {
      CandidateMemory memory;
      memory.memory_id = "memory:candidate:" + fingerprint(diagnostic.strategy_key);
      memory.scope = to_string(episode->scope);
      memory.target =
          episode->scope == LoopScope::coordinator ? "coordinator" : episode->skill_name;
      memory.category = to_string(diagnostic.category);
      memory.confidence = diagnostic.confidence;
      memory.support_count = 1;
      memory_template(diagnostic.category, diagnostic, memory);
      memory.source_diagnostic_ids.push_back(diagnostic.diagnostic_id);
      memory.source_episode_ids.push_back(diagnostic.episode_id);
      memory.evidence_event_ids = diagnostic.evidence_event_ids;
      by_strategy.emplace(diagnostic.strategy_key, report.candidate_memories.size());
      report.candidate_memories.push_back(std::move(memory));
    } else {
      CandidateMemory &memory = report.candidate_memories[existing->second];
      ++memory.support_count;
      memory.confidence = std::max(memory.confidence, diagnostic.confidence);
      append_unique(memory.source_diagnostic_ids, diagnostic.diagnostic_id);
      append_unique(memory.source_episode_ids, diagnostic.episode_id);
      for (const auto &event_id : diagnostic.evidence_event_ids) {
        append_unique(memory.evidence_event_ids, event_id);
      }
    }
  }
}

// 汇总当前分析对象的计数与状态，保持缺失证据的可见性。
void summarize(AnalysisReport &report) {
  report.summary.episode_count = report.episodes.size();
  std::set<std::string> invalid_episodes;
  std::set<std::string> review_episodes;
  for (const auto &episode : report.episodes) {
    if (episode.scope == LoopScope::coordinator) {
      ++report.summary.coordinator_episode_count;
    } else if (episode.scope == LoopScope::skill) {
      ++report.summary.skill_episode_count;
    }
  }
  for (const auto &diagnostic : report.diagnostics) {
    ++report.summary.category_counts[to_string(diagnostic.category)];
    if (diagnostic.counts_as_invalid) {
      invalid_episodes.insert(diagnostic.episode_id);
    } else {
      review_episodes.insert(diagnostic.episode_id);
    }
  }
  report.summary.invalid_episode_count = invalid_episodes.size();
  report.summary.review_episode_count = review_episodes.size();
  report.summary.invalid_loop_lower_bound = invalid_episodes.size();
}

// 构造供 JSON 输出使用的字符串值，遵循所用分配器生命周期。
JsonValue make_string(const std::string &value, Allocator &allocator) {
  return JsonValue(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
}

// 构造字符串数组输出，保持输入顺序和所有权安全。
JsonValue make_strings(const std::vector<std::string> &values, Allocator &allocator) {
  JsonValue array(rapidjson::kArrayType);
  for (const auto &value : values) {
    array.PushBack(make_string(value, allocator), allocator);
  }
  return array;
}

// 序列化轨迹完整性问题，保留类型与对应位置。
JsonValue trace_issue_json(const TraceIssue &issue, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  output.AddMember("issue_id", make_string(issue.issue_id, allocator), allocator);
  output.AddMember("severity", make_string(issue.severity, allocator), allocator);
  output.AddMember("code", make_string(issue.code, allocator), allocator);
  output.AddMember("message", make_string(issue.message, allocator), allocator);
  output.AddMember("evidence_event_ids", make_strings(issue.evidence_event_ids, allocator),
                   allocator);
  return output;
}

// 序列化工具执行事实，保留身份、状态和结果归属。
JsonValue tool_action_json(const ToolActionSummary &tool, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  output.AddMember("tool_call_id", make_string(tool.tool_call_id, allocator), allocator);
  output.AddMember("tool_name", make_string(tool.tool_name, allocator), allocator);
  output.AddMember("arguments_fingerprint", make_string(tool.arguments_fingerprint, allocator),
                   allocator);
  output.AddMember("status", make_string(tool.status, allocator), allocator);
  output.AddMember("result_fingerprint", make_string(tool.result_fingerprint, allocator),
                   allocator);
  output.AddMember("error_class", make_string(tool.error_class, allocator), allocator);
  output.AddMember("retryability", make_string(tool.retryability, allocator), allocator);
  output.AddMember("call_event_id", make_string(tool.call_event_id, allocator), allocator);
  output.AddMember("result_event_id", make_string(tool.result_event_id, allocator), allocator);
  output.AddMember("action_name", make_string(tool.action_name, allocator), allocator);
  output.AddMember("action_contract_observed", tool.action_contract_observed, allocator);
  return output;
}

// 序列化派发信息，区分提交和完成状态。
JsonValue dispatch_json(const DispatchSummary &dispatch, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  output.AddMember("batch_id", make_string(dispatch.batch_id, allocator), allocator);
  output.AddMember("skill_names", make_strings(dispatch.skill_names, allocator), allocator);
  output.AddMember("prompt_fingerprints", make_strings(dispatch.prompt_fingerprints, allocator),
                   allocator);
  JsonValue force(rapidjson::kArrayType);
  for (const bool value : dispatch.force) {
    force.PushBack(value, allocator);
  }
  output.AddMember("force", force, allocator);
  output.AddMember("batch_status", make_string(dispatch.batch_status, allocator), allocator);
  output.AddMember("workflow_status", make_string(dispatch.workflow_status, allocator), allocator);
  output.AddMember("successful_count", static_cast<std::uint64_t>(dispatch.successful_count),
                   allocator);
  output.AddMember("total_count", static_cast<std::uint64_t>(dispatch.total_count), allocator);
  return output;
}

// 序列化评测结论及依据，保留未知与错误状态。
JsonValue evaluation_json(const EvaluationSummary &evaluation, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  output.AddMember("event_id", make_string(evaluation.event_id, allocator), allocator);
  output.AddMember("evaluator", make_string(evaluation.evaluator, allocator), allocator);
  output.AddMember("evaluator_version", make_string(evaluation.evaluator_version, allocator),
                   allocator);
  output.AddMember("metric", make_string(evaluation.metric, allocator), allocator);
  if (evaluation.has_score) {
    output.AddMember("score", evaluation.score, allocator);
  } else {
    output.AddMember("score", JsonValue(rapidjson::kNullType), allocator);
  }
  if (evaluation.has_passed) {
    output.AddMember("passed", evaluation.passed, allocator);
  } else {
    output.AddMember("passed", JsonValue(rapidjson::kNullType), allocator);
  }
  return output;
}

// 序列化执行片段及其关系，供分析报告和回放引用。
JsonValue episode_json(const LoopEpisode &episode, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  output.AddMember("episode_id", make_string(episode.episode_id, allocator), allocator);
  output.AddMember("scope",
                   make_string(episode.scope == LoopScope::agent ? "agent:" + episode.actor_id
                                                                 : to_string(episode.scope),
                               allocator),
                   allocator);
  output.AddMember("owner_id", make_string(episode.owner_id, allocator), allocator);
  output.AddMember("skill_name", make_string(episode.skill_name, allocator), allocator);
  output.AddMember("exec_skill_key", make_string(episode.exec_skill_key, allocator), allocator);
  output.AddMember("activation_index", episode.activation_index, allocator);
  output.AddMember("round", episode.round, allocator);
  output.AddMember("attempt_index", episode.attempt_index, allocator);
  output.AddMember("step_index", episode.step_index, allocator);
  output.AddMember("started_at_us", episode.started_at_us, allocator);
  output.AddMember("ended_at_us", episode.ended_at_us, allocator);
  output.AddMember("request_event_id", make_string(episode.request_event_id, allocator), allocator);
  output.AddMember("response_event_id", make_string(episode.response_event_id, allocator),
                   allocator);
  output.AddMember("control_event_id", make_string(episode.control_event_id, allocator), allocator);
  output.AddMember("action", make_string(episode.action, allocator), allocator);
  output.AddMember("reason", make_string(episode.reason, allocator), allocator);
  output.AddMember("continued", episode.continued, allocator);
  output.AddMember("terminal", episode.terminal, allocator);
  output.AddMember("terminal_status", make_string(episode.terminal_status, allocator), allocator);
  output.AddMember("input_fingerprint", make_string(episode.input_fingerprint, allocator),
                   allocator);
  output.AddMember("action_fingerprint", make_string(episode.action_fingerprint, allocator),
                   allocator);
  output.AddMember("outcome_fingerprint", make_string(episode.outcome_fingerprint, allocator),
                   allocator);
  output.AddMember("outcome_class_fingerprint",
                   make_string(episode.outcome_class_fingerprint, allocator), allocator);
  output.AddMember("outcome_failed", episode.outcome_failed, allocator);
  output.AddMember("all_tool_results_failed", episode.all_tool_results_failed, allocator);
  output.AddMember("has_non_retryable_failure", episode.has_non_retryable_failure, allocator);
  output.AddMember("dispatch", dispatch_json(episode.dispatch, allocator), allocator);
  JsonValue tools(rapidjson::kArrayType);
  for (const auto &tool : episode.tool_actions) {
    tools.PushBack(tool_action_json(tool, allocator), allocator);
  }
  output.AddMember("tool_actions", tools, allocator);
  output.AddMember("input_tool_call_ids", make_strings(episode.input_tool_call_ids, allocator),
                   allocator);
  output.AddMember("source_event_ids", make_strings(episode.source_event_ids, allocator),
                   allocator);
  JsonValue evaluations(rapidjson::kArrayType);
  for (const auto &evaluation : episode.evaluations) {
    evaluations.PushBack(evaluation_json(evaluation, allocator), allocator);
  }
  output.AddMember("evaluations", evaluations, allocator);
  output.AddMember("diagnostic_ids", make_strings(episode.diagnostic_ids, allocator), allocator);
  return output;
}

// 序列化诊断项，不添加未经证据支持的业务结论。
JsonValue diagnostic_json(const LoopDiagnostic &diagnostic, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  output.AddMember("diagnostic_id", make_string(diagnostic.diagnostic_id, allocator), allocator);
  output.AddMember("episode_id", make_string(diagnostic.episode_id, allocator), allocator);
  output.AddMember("category", make_string(to_string(diagnostic.category), allocator), allocator);
  output.AddMember("counts_as_invalid", diagnostic.counts_as_invalid, allocator);
  output.AddMember("confidence", diagnostic.confidence, allocator);
  output.AddMember("severity", make_string(diagnostic.severity, allocator), allocator);
  output.AddMember("summary", make_string(diagnostic.summary, allocator), allocator);
  output.AddMember("observable_cause", make_string(diagnostic.observable_cause, allocator),
                   allocator);
  output.AddMember("tool_name", make_string(diagnostic.tool_name, allocator), allocator);
  output.AddMember("error_class", make_string(diagnostic.error_class, allocator), allocator);
  output.AddMember("evidence_event_ids", make_strings(diagnostic.evidence_event_ids, allocator),
                   allocator);
  return output;
}

// 序列化反思请求或结果，保留来源片段关系。
JsonValue reflection_json(const ReflectionRequest &request, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  output.AddMember("reflection_id", make_string(request.reflection_id, allocator), allocator);
  output.AddMember("diagnostic_id", make_string(request.diagnostic_id, allocator), allocator);
  output.AddMember("episode_id", make_string(request.episode_id, allocator), allocator);
  output.AddMember("scope", make_string(request.scope, allocator), allocator);
  output.AddMember("target", make_string(request.target, allocator), allocator);
  output.AddMember("observation", make_string(request.observation, allocator), allocator);
  output.AddMember("evidence_event_ids", make_strings(request.evidence_event_ids, allocator),
                   allocator);
  output.AddMember("questions", make_strings(request.questions, allocator), allocator);
  output.AddMember("constraints", make_strings(request.constraints, allocator), allocator);
  return output;
}

// 序列化候选经验及来源，管理状态仍由记忆流程决定。
JsonValue memory_json(const CandidateMemory &memory, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  output.AddMember("memory_id", make_string(memory.memory_id, allocator), allocator);
  output.AddMember("status", make_string(memory.status, allocator), allocator);
  output.AddMember("generator_version", make_string(memory.generator_version, allocator),
                   allocator);
  output.AddMember("scope", make_string(memory.scope, allocator), allocator);
  output.AddMember("target", make_string(memory.target, allocator), allocator);
  output.AddMember("category", make_string(memory.category, allocator), allocator);
  output.AddMember("applicable_condition", make_string(memory.applicable_condition, allocator),
                   allocator);
  output.AddMember("strategy", make_string(memory.strategy, allocator), allocator);
  output.AddMember("risk", make_string(memory.risk, allocator), allocator);
  output.AddMember("confidence", memory.confidence, allocator);
  output.AddMember("support_count", static_cast<std::uint64_t>(memory.support_count), allocator);
  output.AddMember("requires_validation", memory.requires_validation, allocator);
  output.AddMember("source_diagnostic_ids", make_strings(memory.source_diagnostic_ids, allocator),
                   allocator);
  output.AddMember("source_episode_ids", make_strings(memory.source_episode_ids, allocator),
                   allocator);
  output.AddMember("evidence_event_ids", make_strings(memory.evidence_event_ids, allocator),
                   allocator);
  return output;
}

} // 结束当前匿名命名空间。

// 将内部枚举映射为稳定的协议名称，保证日志和序列化使用统一表示。
std::string to_string(LoopScope scope) {
  switch (scope) {
  case LoopScope::coordinator:
    return "coordinator";
  case LoopScope::skill:
    return "skill";
  case LoopScope::agent:
    return "agent";
  case LoopScope::unknown:
    return "unknown";
  }
  return "unknown";
}

// 将内部枚举映射为稳定的协议名称，保证日志和序列化使用统一表示。
std::string to_string(InvalidLoopCategory category) {
  switch (category) {
  case InvalidLoopCategory::exact_repeat:
    return "exact_repeat";
  case InvalidLoopCategory::no_state_progress:
    return "no_state_progress";
  case InvalidLoopCategory::non_retryable_repeat:
    return "non_retryable_repeat";
  case InvalidLoopCategory::result_not_consumed:
    return "result_not_consumed";
  case InvalidLoopCategory::invalid_action_repeat:
    return "invalid_action_repeat";
  case InvalidLoopCategory::state_oscillation:
    return "state_oscillation";
  case InvalidLoopCategory::max_rounds_exhausted:
    return "max_rounds_exhausted";
  case InvalidLoopCategory::review_required:
    return "review_required";
  }
  return "review_required";
}

// 从文件读取轨迹并执行规则分析，保留解析和完整性问题。
AnalysisReport LoopEpisodeAnalyzer::analyze_file(const std::filesystem::path &path,
                                                 const AnalyzerOptions &options) const {
  ParsedInput parsed = read_events(path, options);
  const auto tool_results = index_tool_results(parsed.events, parsed.issues);
  std::vector<EpisodeWork> work = build_episodes(parsed, tool_results);

  AnalysisReport report;
  report.source_path = path.string();
  report.dialogue_id = parsed.dialogue_id;
  report.conversation_id = parsed.conversation_id;
  report.user_id = parsed.user_id;
  report.summary.event_count = parsed.events.size();
  report.summary.duplicate_event_count = parsed.duplicate_event_count;
  report.trace_issues = std::move(parsed.issues);
  report.episodes.reserve(work.size());
  for (auto &episode : work) {
    report.episodes.push_back(std::move(episode.episode));
  }

  const bool has_start =
      std::any_of(parsed.events.begin(), parsed.events.end(), [](const EventRecord &event) {
        return event.type == "dialogue.start" || event.type == "session.start";
      });
  const bool has_end =
      std::any_of(parsed.events.begin(), parsed.events.end(), [](const EventRecord &event) {
        return event.type == "dialogue.end" || event.type == "session.end";
      });
  if (!has_start) {
    add_issue(report.trace_issues, "error", "missing_dialogue_start",
              "trajectory has no dialogue.start event");
  }
  if (!has_end) {
    add_issue(report.trace_issues, "error", "missing_dialogue_end",
              "trajectory has no dialogue.end event");
  }

  attach_evaluations(report, parsed.events);
  report.trace_integrity_valid =
      std::none_of(report.trace_issues.begin(), report.trace_issues.end(),
                   [](const TraceIssue &issue) { return issue.severity == "error"; });
  if (options.strict_trace_integrity && !report.trace_integrity_valid) {
    throw AnalysisError("trace integrity validation failed with " +
                        std::to_string(report.trace_issues.size()) + " issue(s)");
  }

  detect_invalid_loops(report);
  if (report.trace_integrity_valid) {
    build_reflection_requests(report);
    build_candidate_memories(report);
  }
  summarize(report);
  return report;
}

// 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
std::string LoopEpisodeAnalyzer::to_json(const AnalysisReport &report, bool pretty) {
  rapidjson::Document document(rapidjson::kObjectType);
  Allocator &allocator = document.GetAllocator();
  document.AddMember("schema_version", report.schema_version, allocator);
  document.AddMember("analysis_kind", make_string(report.analysis_kind, allocator), allocator);
  document.AddMember("analyzer_version", make_string(report.analyzer_version, allocator),
                     allocator);
  document.AddMember("rule_set_version", make_string(report.rule_set_version, allocator),
                     allocator);

  JsonValue source(rapidjson::kObjectType);
  source.AddMember("path", make_string(report.source_path, allocator), allocator);
  source.AddMember("dialogue_id", make_string(report.dialogue_id, allocator), allocator);
  source.AddMember("conversation_id", make_string(report.conversation_id, allocator), allocator);
  source.AddMember("user_id", make_string(report.user_id, allocator), allocator);
  source.AddMember("fingerprint_algorithm", make_string(report.fingerprint_algorithm, allocator),
                   allocator);
  document.AddMember("source", source, allocator);

  JsonValue integrity(rapidjson::kObjectType);
  integrity.AddMember("valid", report.trace_integrity_valid, allocator);
  JsonValue issues(rapidjson::kArrayType);
  for (const auto &issue : report.trace_issues) {
    issues.PushBack(trace_issue_json(issue, allocator), allocator);
  }
  integrity.AddMember("issues", issues, allocator);
  document.AddMember("trace_integrity", integrity, allocator);

  JsonValue summary(rapidjson::kObjectType);
  summary.AddMember("event_count", static_cast<std::uint64_t>(report.summary.event_count),
                    allocator);
  summary.AddMember("duplicate_event_count",
                    static_cast<std::uint64_t>(report.summary.duplicate_event_count), allocator);
  summary.AddMember("episode_count", static_cast<std::uint64_t>(report.summary.episode_count),
                    allocator);
  summary.AddMember("coordinator_episode_count",
                    static_cast<std::uint64_t>(report.summary.coordinator_episode_count),
                    allocator);
  summary.AddMember("skill_episode_count",
                    static_cast<std::uint64_t>(report.summary.skill_episode_count), allocator);
  summary.AddMember("invalid_episode_count",
                    static_cast<std::uint64_t>(report.summary.invalid_episode_count), allocator);
  summary.AddMember("review_episode_count",
                    static_cast<std::uint64_t>(report.summary.review_episode_count), allocator);
  summary.AddMember("invalid_loop_lower_bound",
                    static_cast<std::uint64_t>(report.summary.invalid_loop_lower_bound), allocator);
  JsonValue categories(rapidjson::kObjectType);
  for (const auto &[name, count] : report.summary.category_counts) {
    JsonValue key = make_string(name, allocator);
    JsonValue value;
    value.SetUint64(static_cast<std::uint64_t>(count));
    categories.AddMember(key, value, allocator);
  }
  summary.AddMember("category_counts", categories, allocator);
  document.AddMember("summary", summary, allocator);

  JsonValue episodes(rapidjson::kArrayType);
  for (const auto &episode : report.episodes) {
    episodes.PushBack(episode_json(episode, allocator), allocator);
  }
  document.AddMember("episodes", episodes, allocator);

  JsonValue diagnostics(rapidjson::kArrayType);
  for (const auto &diagnostic : report.diagnostics) {
    diagnostics.PushBack(diagnostic_json(diagnostic, allocator), allocator);
  }
  document.AddMember("diagnostics", diagnostics, allocator);

  JsonValue reflections(rapidjson::kArrayType);
  for (const auto &request : report.reflection_requests) {
    reflections.PushBack(reflection_json(request, allocator), allocator);
  }
  document.AddMember("reflection_requests", reflections, allocator);

  JsonValue memories(rapidjson::kArrayType);
  for (const auto &memory : report.candidate_memories) {
    memories.PushBack(memory_json(memory, allocator), allocator);
  }
  document.AddMember("candidate_memories", memories, allocator);

  rapidjson::StringBuffer buffer;
  if (pretty) {
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
  } else {
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
  }
  return std::string(buffer.GetString(), buffer.GetSize());
}

} // 命名空间结束：agentflow::trajectory
