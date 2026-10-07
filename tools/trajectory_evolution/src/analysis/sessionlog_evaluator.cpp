// Sessionlog 兼容入口：从已有日志提取会话与执行证据，供通用分析链路消费。
// 该入口用于适配旧日志；新框架仍可直接输入标准轨迹，不必采用 Sessionlog 格式。
// 保留输入来源与告警，避免把解析缺口误当成 Agent 正常完成。

#include "agentflow/trajectory/hybrid_evaluator.h"

#include "agentflow/trajectory/loop_episode_analyzer.h"
#include "agentflow/trajectory/experience.h"
#include "trace_internal.h"
#include "model_json.h"
#include "model_input.h"
#include "code_projection.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <google/protobuf/util/json_util.h>
#include <limits>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
extern char **environ;
#endif

namespace agentflow::trajectory {
namespace {

namespace ep = evaluation_proto;
using JsonValue = rapidjson::Value;
using Allocator = rapidjson::Document::AllocatorType;

constexpr std::size_t kMaxPromptBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxModelResponseBytes = 256U * 1024U;
constexpr std::size_t kMaxContentStringBytes = 32U * 1024U;
constexpr std::size_t kMaxContainerItems = 256;
constexpr std::size_t kMaxResponseTextBytes = 2048;
constexpr std::size_t kMaxResponseListItems = 64;
constexpr std::size_t kMaxEvidenceIds = 256;

// 查找 JSON 对象成员；缺失时返回空指针，由调用方决定是否允许缺省。
const JsonValue *find_member(const JsonValue &object, const char *name) {
  if (!object.IsObject()) {
    return nullptr;
  }
  const auto member = object.FindMember(name);
  return member == object.MemberEnd() ? nullptr : &member->value;
}

// 读取可选字符串，缺失时使用约定默认值，不强制字符串化其他类型。
std::string optional_string(const JsonValue &object, const char *name) {
  const JsonValue *value = find_member(object, name);
  if (value == nullptr || !value->IsString()) {
    return {};
  }
  return std::string(value->GetString(), value->GetStringLength());
}

// 将协议对象转换为 JSON；字段含义仍由 Protobuf 契约约束。
std::string protobuf_json(const google::protobuf::Message &message) {
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  std::string output;
  const auto status = google::protobuf::util::MessageToJsonString(message, &output, options);
  if (!status.ok()) {
    throw EvaluationError("failed to serialize Protobuf JSON: " + status.ToString());
  }
  return output;
}

// 将 JSON 值序列化为紧凑文本，不改变应用字段的实际内容。
std::string serialize_json(const JsonValue &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

// 限制原始会话日志读取大小，避免超大文件耗尽分析内存。
std::string read_bounded_sessionlog(const std::filesystem::path &path, std::size_t max_file_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    throw EvaluationError("cannot stat SessionLog: " + error.message());
  }
  if (size > max_file_bytes) {
    throw EvaluationError("SessionLog exceeds configured byte limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw EvaluationError("cannot open SessionLog");
  }
  std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    throw EvaluationError("failed while reading SessionLog");
  }
  return content;
}

// 对规范化内容计算稳定摘要，用于去重和版本比较。
// 摘要只标识内容是否变化，不替代权限校验，也不包含可还原的凭据原文。
std::string fingerprint(std::string_view value) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  if (SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest.data()) ==
      nullptr) {
    throw EvaluationError("failed to compute SHA-256 fingerprint");
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string output(digest.size() * 2U, '0');
  for (std::size_t index = 0; index < digest.size(); ++index) {
    output[index * 2U] = hex[(digest[index] >> 4U) & 0x0fU];
    output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
  }
  return output;
}

// 只规范化 ASCII 字母大小写，保持中文和其他 UTF-8 字节不变。
std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

// 判断环境变量名是否可能承载凭据；用于脱敏扫描，不记录变量原值。
bool secret_environment_name(const std::string &name) {
  std::string upper = name;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return upper.find("KEY") != std::string::npos || upper.find("TOKEN") != std::string::npos ||
         upper.find("SECRET") != std::string::npos || upper.find("PASSWORD") != std::string::npos ||
         upper.find("CREDENTIAL") != std::string::npos || upper.find("AUTH") != std::string::npos;
}

// 把非空已知凭据加入内存匹配集合，不把其原文写入报告。
void append_secret(std::vector<std::string> *secrets, const std::string &value) {
  if (value.size() >= 8U && std::find(secrets->begin(), secrets->end(), value) == secrets->end()) {
    secrets->push_back(value);
  }
}

// 读取当前环境中的已知敏感值，仅用于当前进程的脱敏检查。
std::vector<std::string> environment_secrets() {
  std::vector<std::string> output;
#if defined(__unix__) || defined(__APPLE__)
  if (environ != nullptr) {
    for (char **item = environ; *item != nullptr; ++item) {
      const std::string entry(*item);
      const std::size_t separator = entry.find('=');
      if (separator != std::string::npos && secret_environment_name(entry.substr(0, separator))) {
        append_secret(&output, entry.substr(separator + 1U));
      }
    }
  }
#endif
  const char *configured_names = std::getenv("AF_SECRET_ENV_NAMES");
  if (configured_names != nullptr) {
    std::istringstream names(configured_names);
    std::string name;
    while (std::getline(names, name, ',')) {
      const char *value = std::getenv(name.c_str());
      if (value != nullptr) {
        append_secret(&output, value);
      }
    }
  }
  std::sort(output.begin(), output.end(),
            [](const auto &left, const auto &right) { return left.size() > right.size(); });
  return output;
}

// 检查内容是否包含已知环境凭据，支持与模式检测互补。
bool contains_environment_secret(const std::string &value,
                                 const std::vector<std::string> &secrets) {
  return std::any_of(secrets.begin(), secrets.end(),
                     [&](const auto &secret) { return value.find(secret) != std::string::npos; });
}

// 返回集中维护的凭据模式，避免各输出路径使用不同的检测规则。
const std::vector<std::regex> &credential_patterns() {
  static const std::vector<std::regex> patterns = {
      std::regex(R"(\bsk-[A-Za-z0-9_-]{16,}\b)"),
      std::regex(R"(\bark-[A-Za-z0-9_-]{16,}\b)"),
      std::regex(R"(\bAKIA[A-Z0-9]{16}\b)"),
      std::regex(R"(\bAIza[A-Za-z0-9_-]{20,}\b)"),
      std::regex(R"(\bgh[pousr]_[A-Za-z0-9_]{20,}\b)"),
      std::regex(R"(\bgithub_pat_[A-Za-z0-9_]{20,}\b)"),
      std::regex(R"(\bxox[a-z]-[A-Za-z0-9-]{16,}\b)"),
      std::regex(R"(\bBearer\s+[A-Za-z0-9._~+/-]{16,}=*\b)", std::regex::icase),
      std::regex(R"((api[_ -]?key|access[_ -]?token|secret|password)\s*[:=]\s*)"
                 R"(["']?[A-Za-z0-9._~+/-]{12,})",
                 std::regex::icase),
      std::regex(R"(([?&](api[_-]?key|access[_-]?token|token|signature|sig|key)=)[^&\s"']{8,})",
                 std::regex::icase),
      std::regex(R"(\beyJ[A-Za-z0-9_-]{16,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b)"),
      std::regex(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----)"),
  };
  return patterns;
}

// 根据集中模式检查未知来源的敏感值。
bool contains_credential_pattern(const std::string &value) {
  return std::any_of(credential_patterns().begin(), credential_patterns().end(),
                     [&](const auto &pattern) { return std::regex_search(value, pattern); });
}

struct SanitizerState {
  struct CachedString {
    std::string value;
    std::uint64_t redactions, truncations;
  };

  std::vector<std::string> secrets;
  std::uint64_t redaction_count = 0;
  std::uint64_t truncation_count = 0;
  std::unordered_map<std::string, CachedString> strings;
  std::size_t cached_bytes = 0;
};

// 替换文本中的全部指定片段，推进位置避免在替换结果上无限循环。
void replace_all(std::string *value, const std::string &needle, const std::string &replacement,
                 std::uint64_t *count) {
  std::size_t position = 0;
  while ((position = value->find(needle, position)) != std::string::npos) {
    value->replace(position, needle.size(), replacement);
    position += replacement.size();
    ++(*count);
  }
}

// 在完整 UTF-8 字符边界截断预览，避免中文产生无效编码。
std::string truncate_utf8(const std::string &value, SanitizerState *state) {
  if (value.size() <= kMaxContentStringBytes) {
    return value;
  }
  std::size_t boundary = kMaxContentStringBytes;
  // 完整不透明凭据可能横跨截断点，应将截断位置移到凭据之前。
  // 如果只检查截取后的前缀，就无法识别完整凭据。
  std::size_t previous;
  do {
    previous = boundary;
    for (const auto &secret : state->secrets) {
      if (secret.empty() || !boundary)
        continue;
      const auto start = value.rfind(secret, boundary - 1);
      if (start != std::string::npos && secret.size() > boundary - start) {
        boundary = start;
        ++state->redaction_count;
      }
    }
  } while (previous != boundary);
  while (boundary > 0U && (static_cast<unsigned char>(value[boundary]) & 0xc0U) == 0x80U) {
    --boundary;
  }
  ++state->truncation_count;
  return value.substr(0, boundary) + "<TRUNCATED sha256=" + fingerprint(value) + ">";
}

// 先识别并替换敏感内容，再限制展示长度，避免截断后只泄露半段凭据。
std::string redact_string(const std::string &input, SanitizerState *state) {
  const auto cached = state->strings.find(input);
  if (cached != state->strings.end()) {
    state->redaction_count += cached->second.redactions;
    state->truncation_count += cached->second.truncations;
    return cached->second.value;
  }
  const auto redactions = state->redaction_count, truncations = state->truncation_count;
  std::string output = truncate_utf8(input, state);
  for (const auto &secret : state->secrets) {
    replace_all(&output, secret, "<REDACTED:ENV_SECRET>", &state->redaction_count);
  }
  for (const auto &pattern : credential_patterns()) {
    if (std::regex_search(output, pattern)) {
      output = std::regex_replace(output, pattern, "<REDACTED:CREDENTIAL>");
      ++state->redaction_count;
    }
  }
  static const std::vector<std::regex> private_paths = {
      std::regex(R"((^|[\s"'=])/(root|home|tmp|var|etc|mnt)/[^\s"']+)", std::regex::icase),
      std::regex(R"(\b[A-Za-z]:\\Users\\[^\s"']+)", std::regex::icase),
  };
  for (const auto &pattern : private_paths) {
    if (std::regex_search(output, pattern)) {
      output = std::regex_replace(output, pattern, "<REDACTED:LOCAL_PATH>");
      ++state->redaction_count;
    }
  }
  // 缓存仅在当前请求内复用相同脱敏结果，不跨请求共享敏感值，
  // 也不改变每次出现的审计计数；同时限制原文和脱敏文本的保留字节数。
  const auto size = input.size() + output.size();
  if (input.size() >= 512 && size <= 262144 && state->strings.size() < 512 &&
      state->cached_bytes + size <= 4194304) {
    state->strings.emplace(input,
                           SanitizerState::CachedString{output, state->redaction_count - redactions,
                                                        state->truncation_count - truncations});
    state->cached_bytes += size;
  }
  return output;
}

// 识别可能承载凭据的字段名，结合值扫描决定脱敏处理。
bool sensitive_field_name(const std::string &name) {
  const std::string lower = lower_ascii(name);
  return lower == "api_key" || lower == "apikey" || lower == "access_key" ||
         lower == "access_token" || lower == "refresh_token" || lower == "authorization" ||
         lower == "cookie" || lower == "set-cookie" || lower == "password" || lower == "secret" ||
         lower == "credential" || lower == "private_key" || lower == "source_path" ||
         lower == "storage_path" || lower == "local_path" || lower == "filesystem_path";
}

// 递归生成脱敏后的数据副本，原始业务值不因分析而被改写。
JsonValue sanitized_value(const JsonValue &input, Allocator &allocator, SanitizerState *state,
                          const std::string &field_name, std::size_t depth) {
  if (sensitive_field_name(field_name)) {
    ++state->redaction_count;
    return JsonValue("<REDACTED:FIELD>", allocator);
  }
  if (depth > 32U) {
    ++state->truncation_count;
    return JsonValue("<TRUNCATED:DEPTH>", allocator);
  }
  if (input.IsObject()) {
    JsonValue output(rapidjson::kObjectType);
    std::size_t count = 0;
    for (auto item = input.MemberBegin(); item != input.MemberEnd(); ++item) {
      if (count >= kMaxContainerItems) {
        ++state->truncation_count;
        break;
      }
      const std::string name(item->name.GetString(), item->name.GetStringLength());
      JsonValue key(name.c_str(), static_cast<rapidjson::SizeType>(name.size()), allocator);
      JsonValue value = sanitized_value(item->value, allocator, state, name, depth + 1U);
      output.AddMember(key, value, allocator);
      ++count;
    }
    return output;
  }
  if (input.IsArray()) {
    JsonValue output(rapidjson::kArrayType);
    const rapidjson::SizeType limit = std::min<rapidjson::SizeType>(
        input.Size(), static_cast<rapidjson::SizeType>(kMaxContainerItems));
    for (rapidjson::SizeType index = 0; index < limit; ++index) {
      JsonValue value = sanitized_value(input[index], allocator, state, field_name, depth + 1U);
      output.PushBack(value, allocator);
    }
    if (input.Size() > limit) {
      ++state->truncation_count;
    }
    return output;
  }
  if (input.IsString()) {
    const std::string value =
        redact_string(std::string(input.GetString(), input.GetStringLength()), state);
    return JsonValue(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
  }
  JsonValue output;
  output.CopyFrom(input, allocator);
  return output;
}

// 筛选分析所需事件类型，保留与执行和结果有关的事实。
bool relevant_event_type(const std::string &type) {
  static const std::unordered_set<std::string> types = {
      "dialogue.start",    "coordinator.activation",
      "llm.request",       "llm.response",
      "loop.control",      "skill.dispatch",
      "batch.result",      "skill.start",
      "tool.call",         "tool.result",
      "artifact.persist",  "skill.end",
      "evaluation.result", "dialogue.end",
  };
  return types.find(type) != types.end();
}

// 判断仅用于内部传输而无需发送给模型的字段。
bool excluded_data_field(const std::string &type, const std::string &name) {
  const std::string lower = lower_ascii(name);
  if (type == "llm.response" &&
      (lower == "request" || lower == "provider_response" || lower == "meta")) {
    return true;
  }
  return (type == "artifact.persist" && lower == "entries") ||
         (type == "skill.end" && lower == "artifacts");
}

// 保持已有顺序追加尚未出现的元素，避免重复证据或标签影响统计。
void append_unique(std::vector<std::string> *values, const std::string &value) {
  if (!value.empty() && std::find(values->begin(), values->end(), value) == values->end()) {
    values->push_back(value);
  }
}

// 建立本次可引用的事件白名单，约束模型证据范围。
std::vector<std::string> collect_allowed_event_ids(const ep::HybridEvaluationReport &report) {
  std::vector<std::string> output;
  for (const auto &assessment : report.assessments()) {
    const auto &packet = assessment.evidence();
    for (const auto &event_id : packet.source_event_ids()) {
      append_unique(&output, event_id);
    }
    for (const auto &tool : packet.tools()) {
      append_unique(&output, tool.call_event_id());
      append_unique(&output, tool.result_event_id());
    }
    for (const auto &evaluation : packet.evaluations()) {
      append_unique(&output, evaluation.event_id());
    }
    for (const auto &diagnostic : packet.diagnostics()) {
      for (const auto &event_id : diagnostic.evidence_event_ids()) {
        append_unique(&output, event_id);
      }
    }
  }
  return output;
}

// 提取并脱敏事件内容，同时保持事件身份与因果信息。
JsonValue sanitized_event_data(const std::string &type, const JsonValue *data, Allocator &allocator,
                               SanitizerState *state) {
  if (data == nullptr) {
    return JsonValue(rapidjson::kObjectType);
  }
  if (!data->IsObject()) {
    return sanitized_value(*data, allocator, state, "data", 0);
  }
  JsonValue output(rapidjson::kObjectType);
  for (auto item = data->MemberBegin(); item != data->MemberEnd(); ++item) {
    const std::string name(item->name.GetString(), item->name.GetStringLength());
    if (excluded_data_field(type, name)) {
      continue;
    }
    JsonValue key(name.c_str(), static_cast<rapidjson::SizeType>(name.size()), allocator);
    JsonValue value = sanitized_value(item->value, allocator, state, name, 0);
    output.AddMember(key, value, allocator);
  }
  return output;
}

struct ContentEvents {
  JsonValue values{rapidjson::kArrayType};
  std::vector<std::string> event_ids;
  std::uint64_t count = 0;
};

// 收集有助于理解实际行为的内容事件，避免只给模型无法解释的指纹。
ContentEvents collect_content_events(const std::string &sessionlog,
                                     const std::vector<std::string> &structural_event_ids,
                                     Allocator &allocator, SanitizerState *state,
                                     bool standard_trace = false) {
  ContentEvents output;
  std::unordered_set<std::string> allowed(structural_event_ids.begin(), structural_event_ids.end());
  std::unordered_set<std::string> seen;
  std::istringstream input(sessionlog);
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty()) {
      continue;
    }
    rapidjson::Document event;
    event.Parse<rapidjson::kParseValidateEncodingFlag>(line.data(), line.size());
    if (event.HasParseError() || !event.IsObject()) {
      throw EvaluationError("valid SessionLog could not be re-read at line " +
                            std::to_string(line_number) + ": " +
                            rapidjson::GetParseError_En(event.GetParseError()));
    }
    const std::string type = optional_string(event, "type");
    const std::string event_id = optional_string(event, "event_id");
    if (event_id.empty() || (!standard_trace && !relevant_event_type(type))) {
      continue;
    }
    const bool contextual = standard_trace || type == "dialogue.start" || type == "dialogue.end" ||
                            type == "coordinator.activation" || type == "skill.dispatch" ||
                            type == "batch.result" || type == "skill.start" ||
                            type == "skill.end" || type == "artifact.persist" ||
                            type == "evaluation.result";
    if (!contextual && allowed.find(event_id) == allowed.end()) {
      continue;
    }
    if (!seen.insert(event_id).second) {
      continue;
    }
    JsonValue item(rapidjson::kObjectType);
    item.AddMember("event_id", JsonValue(event_id.c_str(), allocator), allocator);
    item.AddMember("type", JsonValue(type.c_str(), allocator), allocator);
    const JsonValue *timestamp = find_member(event, "timestamp_us");
    if (timestamp != nullptr && timestamp->IsUint64()) {
      item.AddMember("timestamp_us", timestamp->GetUint64(), allocator);
    }
    const JsonValue *round = find_member(event, "round");
    if (round != nullptr && round->IsUint64()) {
      item.AddMember("round", round->GetUint64(), allocator);
    }
    for (const char *name : {"trace_id", "node_id"}) {
      const std::string value = optional_string(event, name);
      if (!value.empty()) {
        item.AddMember(JsonValue(name, allocator), JsonValue(value.c_str(), allocator), allocator);
      }
    }
    item.AddMember("data", sanitized_event_data(type, find_member(event, "data"), allocator, state),
                   allocator);
    output.values.PushBack(item, allocator);
    output.event_ids.push_back(event_id);
    ++output.count;
  }
  return output;
}

// 根据配置和规则结果决定是否需要模型评测，减少不必要的远程调用。
bool should_select_model(ModelSelection selection, const ep::EpisodeEvidencePacket &packet,
                         const ep::RuleAssessment &rule) {
  if (selection == ModelSelection::none) {
    return false;
  }
  if (selection == ModelSelection::all) {
    return true;
  }
  if (rule.verdict() != ep::SEMANTIC_VERDICT_UNASSESSED || packet.outcome_failed() ||
      packet.all_tool_results_failed() || packet.has_non_retryable_failure()) {
    return true;
  }
  const std::string batch_status = packet.dispatch().batch_status();
  return batch_status == "partial" || batch_status == "failed" ||
         packet.terminal_status() == "tool_error" ||
         packet.terminal_status() == "max_rounds_reached";
}

// 合并规则与模型结论，保留冲突和不确定性，不用模型覆盖硬性证据。
void fuse_assessment(ep::EpisodeAssessment *assessment) {
  const auto rule_verdict = assessment->rule().verdict();
  const bool model_ok = assessment->model().status() == ep::MODEL_CALL_STATUS_SUCCEEDED;
  const auto model_verdict = assessment->model().verdict();
  assessment->set_rule_model_conflict(false);

  if (rule_verdict == ep::SEMANTIC_VERDICT_INVALID) {
    assessment->set_final_verdict(ep::SEMANTIC_VERDICT_INVALID);
    assessment->set_final_confidence(assessment->rule().confidence());
    assessment->set_decision_source(ep::DECISION_SOURCE_RULE);
    assessment->set_fusion_reason("确定性无效规则是不可被模型推翻的硬约束。");
    if (model_ok && model_verdict == ep::SEMANTIC_VERDICT_INVALID) {
      assessment->set_decision_source(ep::DECISION_SOURCE_HYBRID);
      assessment->set_final_confidence(
          std::max(assessment->rule().confidence(), assessment->model().confidence()));
      assessment->set_fusion_reason("模型判断与确定性无效规则一致。");
    } else if (model_ok && model_verdict != ep::SEMANTIC_VERDICT_UNCERTAIN) {
      assessment->set_rule_model_conflict(true);
    }
    return;
  }

  if (model_ok) {
    assessment->set_final_verdict(model_verdict);
    assessment->set_final_confidence(assessment->model().confidence());
    assessment->set_decision_source(rule_verdict == ep::SEMANTIC_VERDICT_UNASSESSED
                                        ? ep::DECISION_SOURCE_MODEL
                                        : ep::DECISION_SOURCE_HYBRID);
    assessment->set_fusion_reason(rule_verdict == ep::SEMANTIC_VERDICT_UNASSESSED
                                      ? "确定性规则未下语义结论，采用已校验的模型判断。"
                                      : "确定性规则负责筛选可疑步骤，模型补充任务语义判断。");
    return;
  }

  assessment->set_final_verdict(rule_verdict);
  assessment->set_final_confidence(assessment->rule().confidence());
  assessment->set_decision_source(rule_verdict == ep::SEMANTIC_VERDICT_UNASSESSED
                                      ? ep::DECISION_SOURCE_NONE
                                      : ep::DECISION_SOURCE_RULE);
  assessment->set_fusion_reason(rule_verdict == ep::SEMANTIC_VERDICT_UNASSESSED
                                    ? "没有足够的确定性证据，也没有可用的模型判断。"
                                    : "模型未提供有效判断，保留确定性规则结论。");
}

// 把模型返回的判定名称转换为内部枚举；未知名称不能默认算作成功。
ep::SemanticVerdict parse_verdict(const std::string &value) {
  if (value == "useful") {
    return ep::SEMANTIC_VERDICT_USEFUL;
  }
  if (value == "invalid") {
    return ep::SEMANTIC_VERDICT_INVALID;
  }
  if (value == "necessary_retry") {
    return ep::SEMANTIC_VERDICT_NECESSARY_RETRY;
  }
  if (value == "premature_stop") {
    return ep::SEMANTIC_VERDICT_PREMATURE_STOP;
  }
  if (value == "uncertain") {
    return ep::SEMANTIC_VERDICT_UNCERTAIN;
  }
  return ep::SEMANTIC_VERDICT_UNSPECIFIED;
}

// 解析任务结果状态，区分业务失败、未知和执行异常。
ep::AggregateStatus parse_task_status(const std::string &value) {
  if (value == "succeeded") {
    return ep::AGGREGATE_STATUS_SUCCEEDED;
  }
  if (value == "failed") {
    return ep::AGGREGATE_STATUS_FAILED;
  }
  if (value == "partial") {
    return ep::AGGREGATE_STATUS_PARTIAL;
  }
  if (value == "unknown") {
    return ep::AGGREGATE_STATUS_UNKNOWN;
  }
  return ep::AGGREGATE_STATUS_UNSPECIFIED;
}

// 检查文本中是否包含中文字符，用于验证说明语言要求，而非评估业务正确性。
bool has_cjk(const std::string &value) {
  for (std::size_t index = 0; index + 2U < value.size(); ++index) {
    const unsigned char first = static_cast<unsigned char>(value[index]);
    const unsigned char second = static_cast<unsigned char>(value[index + 1U]);
    const unsigned char third = static_cast<unsigned char>(value[index + 2U]);
    if (first >= 0xe4U && first <= 0xe9U && (second & 0xc0U) == 0x80U && (third & 0xc0U) == 0x80U) {
      return true;
    }
  }
  return false;
}

// 验证模型说明文本的长度和类型，拒绝未经约束的响应正文。
bool bounded_response_string(const JsonValue &object, const char *name, std::string *output,
                             std::size_t max_bytes = kMaxResponseTextBytes) {
  const JsonValue *value = find_member(object, name);
  if (value == nullptr || !value->IsString() || value->GetStringLength() == 0U ||
      value->GetStringLength() > max_bytes) {
    return false;
  }
  output->assign(value->GetString(), value->GetStringLength());
  return true;
}

// 读取并检查置信度范围，不把缺失值自动当作高可信。
bool response_confidence(const JsonValue &object, double *output) {
  const JsonValue *value = find_member(object, "confidence");
  if (value == nullptr || !value->IsNumber() || !std::isfinite(value->GetDouble()) ||
      value->GetDouble() < 0.0 || value->GetDouble() > 1.0) {
    return false;
  }
  *output = value->GetDouble();
  return true;
}

// 读取模型返回的字符串列表，并检查数量及元素限制。
bool response_string_array(const JsonValue &object, const char *name,
                           std::vector<std::string> *output,
                           std::size_t max_items = kMaxResponseListItems) {
  const JsonValue *values = find_member(object, name);
  if (values == nullptr || !values->IsArray() || values->Size() > max_items) {
    return false;
  }
  std::unordered_set<std::string> seen;
  for (const auto &value : values->GetArray()) {
    if (!value.IsString() || value.GetStringLength() > kMaxResponseTextBytes) {
      return false;
    }
    std::string item(value.GetString(), value.GetStringLength());
    if (seen.insert(item).second) {
      output->push_back(std::move(item));
    }
  }
  return true;
}

// 验证响应证据都属于本次允许范围，拒绝幻造或跨任务引用。
bool response_evidence_ids(const JsonValue &object, const std::unordered_set<std::string> &allowed,
                           std::vector<std::string> *output) {
  if (!response_string_array(object, "evidence_event_ids", output, kMaxEvidenceIds) ||
      output->empty()) {
    return false;
  }
  return std::all_of(output->begin(), output->end(),
                     [&](const auto &event_id) { return allowed.find(event_id) != allowed.end(); });
}

struct EpisodeJudgment {
  ep::SemanticVerdict verdict = ep::SEMANTIC_VERDICT_UNSPECIFIED;
  double confidence = 0.0;
  std::string expected_state_change;
  std::string observed_state_change;
  std::string concise_reason;
  std::vector<std::string> evidence_event_ids;
};

struct DialogueValidation {
  bool valid = false;
  bool json_fence_unwrapped = false;
  std::string error_code;
  ep::AggregateStatus task_outcome = ep::AGGREGATE_STATUS_UNSPECIFIED;
  double task_confidence = 0.0;
  std::string task_reason;
  std::vector<std::string> task_evidence_event_ids;
  std::unordered_map<std::string, EpisodeJudgment> episodes;
  std::string reflection_summary;
  std::vector<std::string> root_causes;
  std::vector<std::string> skill_orchestration_changes;
  std::vector<std::string> experience_injections;
  std::vector<ep::ExperienceAdvice> experience_details;
  std::vector<std::pair<std::string, std::size_t>> experience_issues;
  std::vector<std::string> stop_condition_changes;
  std::vector<std::string> wasted_episode_ids;
  std::vector<std::string> reflection_evidence_event_ids;
};

// 校验对话评测响应的结构、目标、证据和隐私，失败时保留固定诊断。
DialogueValidation
validate_dialogue_response(const RawModelResponse &response, const ModelRequest &request,
                           const std::unordered_set<std::string> &selected_episode_ids,
                           const std::unordered_set<std::string> &all_episode_ids,
                           const std::set<std::string> &allowed_experience_scopes,
                           const std::vector<std::string> &secrets, bool require_chinese_reason) {
  DialogueValidation output;
  if (!response.transport_ok || !response.error_code.empty()) {
    output.error_code = response.error_code.empty() ? "transport_error" : response.error_code;
    return output;
  }
  if (response.body.empty() || response.body.size() > kMaxModelResponseBytes) {
    output.error_code = "invalid_response_size";
    return output;
  }
  if (contains_credential_pattern(response.body) ||
      contains_environment_secret(response.body, secrets)) {
    output.error_code = "credential_like_output";
    return output;
  }

  const auto body = unwrap_model_json(response.body, &output.json_fence_unwrapped);
  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
      body.data(), body.size());
  if (document.HasParseError() || !unique_object_fields(document)) {
    output.error_code = "invalid_json";
    return output;
  }
  const JsonValue *task = find_member(document, "task_outcome");
  const JsonValue *episodes = find_member(document, "episode_assessments");
  const JsonValue *reflection = find_member(document, "reflection");
  if (task == nullptr || !task->IsObject() || episodes == nullptr || !episodes->IsArray() ||
      reflection == nullptr || !reflection->IsObject()) {
    output.error_code = "missing_dialogue_section";
    return output;
  }

  output.task_outcome = parse_task_status(optional_string(*task, "status"));
  const std::unordered_set<std::string> allowed_events(request.allowed_event_ids.begin(),
                                                       request.allowed_event_ids.end());
  if (output.task_outcome == ep::AGGREGATE_STATUS_UNSPECIFIED ||
      !response_confidence(*task, &output.task_confidence) ||
      !bounded_response_string(*task, "reason", &output.task_reason) ||
      (require_chinese_reason && !has_cjk(output.task_reason)) ||
      !response_evidence_ids(*task, allowed_events, &output.task_evidence_event_ids)) {
    output.error_code = "invalid_task_outcome";
    return output;
  }

  if (episodes->Size() != selected_episode_ids.size()) {
    output.error_code = "episode_assessment_count_mismatch";
    return output;
  }
  for (const auto &value : episodes->GetArray()) {
    if (!value.IsObject()) {
      output.error_code = "invalid_episode_assessment";
      return output;
    }
    const std::string episode_id = optional_string(value, "episode_id");
    if (selected_episode_ids.find(episode_id) == selected_episode_ids.end() ||
        output.episodes.find(episode_id) != output.episodes.end()) {
      output.error_code = "unknown_or_duplicate_episode_id";
      return output;
    }
    EpisodeJudgment judgment;
    judgment.verdict = parse_verdict(optional_string(value, "verdict"));
    if (judgment.verdict == ep::SEMANTIC_VERDICT_UNSPECIFIED) {
      output.error_code = "invalid_episode_verdict";
    } else if (!response_confidence(value, &judgment.confidence)) {
      output.error_code = "invalid_episode_confidence";
    } else if ((value.HasMember("expected_state_change") &&
                !bounded_response_string(value, "expected_state_change", &judgment.expected_state_change)) ||
               (value.HasMember("observed_state_change") &&
                !bounded_response_string(value, "observed_state_change", &judgment.observed_state_change))) {
      output.error_code = "invalid_episode_state_change";
    } else if (!bounded_response_string(value, "concise_reason", &judgment.concise_reason) ||
               (require_chinese_reason && !has_cjk(judgment.concise_reason))) {
      output.error_code = "invalid_episode_concise_reason";
    } else if (!response_evidence_ids(value, allowed_events, &judgment.evidence_event_ids)) {
      output.error_code = "invalid_episode_evidence_event_ids";
    }
    if (!output.error_code.empty()) {
      return output;
    }
    output.episodes.emplace(episode_id, std::move(judgment));
  }

  // 诊断只公开固定字段名，不记录模型原文；字段错误不能让整批仅剩一个模糊原因。
  if (!bounded_response_string(*reflection, "summary", &output.reflection_summary) ||
      (require_chinese_reason && !has_cjk(output.reflection_summary))) {
    output.error_code = "invalid_reflection_summary";
    return output;
  }
  for (const auto &field : std::vector<std::pair<const char *, std::vector<std::string> *>>{
           {"root_causes", &output.root_causes},
           {"wasted_episode_ids", &output.wasted_episode_ids}}) {
    if (!response_string_array(*reflection, field.first, field.second)) {
      output.error_code = std::string("invalid_reflection_") + field.first;
      return output;
    }
  }
  if (!response_evidence_ids(*reflection, allowed_events, &output.reflection_evidence_event_ids)) {
    output.error_code = "invalid_reflection_evidence_event_ids";
    return output;
  }
  if (!std::all_of(output.wasted_episode_ids.begin(), output.wasted_episode_ids.end(),
                   [&](const auto &episode_id) {
                     return all_episode_ids.find(episode_id) != all_episode_ids.end();
                   })) {
    output.error_code = "unknown_wasted_episode_id";
    return output;
  }
  // 三类建议共用条目级契约。单条建议无效只隔离该条，不丢失整轮分析。
  const auto read_advice = [&](const char *field, ep::CandidateTargetType target,
                               std::vector<std::string> *display) {
    const auto *experiences = find_member(*reflection, field);
    if (experiences == nullptr || !experiences->IsArray() ||
        experiences->Size() > kMaxResponseListItems) {
      output.experience_issues.emplace_back("invalid_experience_list", 0);
    } else {
      std::unordered_set<std::string> seen;
      for (rapidjson::SizeType index = 0; index < experiences->Size(); ++index) {
        const auto &value = (*experiences)[index];
        auto reject = [&](const char *reason) {
          output.experience_issues.emplace_back(reason, index);
        };
        if (value.IsString()) {
          // 旧版文本仍可检查，但没有明确的接收对象与适用条件契约。
          const std::string text(value.GetString(), value.GetStringLength());
          if (experience_text_valid(text, kMaxResponseTextBytes)) {
            if (seen.insert(text).second)
              display->push_back(text);
            reject("legacy_experience_unscoped");
          } else
            reject("invalid_experience_contract");
          continue;
        }
        std::string scope, trigger, change;
        std::vector<std::string> evidence;
        if (!value.IsObject() || value.MemberCount() != 4 ||
            !bounded_response_string(value, "target_scope", &scope, 512) ||
            !bounded_response_string(value, "trigger_pattern", &trigger) ||
            !bounded_response_string(value, "proposed_change", &change) ||
            !experience_fields_valid(scope, trigger, change) ||
            (require_chinese_reason && (!has_cjk(trigger) || !has_cjk(change)))) {
          reject("invalid_experience_contract");
          continue;
        }
        if (!allowed_experience_scopes.count(scope)) {
          reject("invalid_experience_scope");
          continue;
        }
        if (!response_evidence_ids(value, allowed_events, &evidence)) {
          reject("invalid_experience_evidence");
          continue;
        }
        std::sort(evidence.begin(), evidence.end());
        ep::ExperienceAdvice advice;
        if (target != ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION)
          advice.set_target_type(target);
        advice.set_target_scope(scope);
        advice.set_trigger_pattern(trigger);
        advice.set_proposed_change(change);
        for (const auto &id : evidence)
          advice.add_evidence_event_ids(id);
        if (!seen.insert(advice.SerializeAsString()).second)
          continue;
        output.experience_details.push_back(std::move(advice));
        display->push_back(change);
        for (const auto &id : evidence)
          append_unique(&output.reflection_evidence_event_ids, id);
      }
    }
  };
  read_advice("experience_injections", ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION,
              &output.experience_injections);
  read_advice("skill_orchestration_changes", ep::CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION,
              &output.skill_orchestration_changes);
  read_advice("stop_condition_changes", ep::CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION,
              &output.stop_condition_changes);
  output.valid = true;
  return output;
}

// 把已校验字符串写入重复字段，避免旧字段残留在新结果中。
void set_repeated_strings(google::protobuf::RepeatedPtrField<std::string> *destination,
                          const std::vector<std::string> &values) {
  destination->Clear();
  for (const auto &value : values) {
    *destination->Add() = value;
  }
}

// 记录模型评测是否执行及结果状态，便于区分规则结论与模型结论。
void set_selected_model_status(ep::HybridEvaluationReport *report,
                               const std::vector<int> &selected_indices, ep::ModelCallStatus status,
                               const std::string &error_code, const std::string &adapter,
                               const std::string &model_id, const std::string &request_fingerprint,
                               const std::string &response_fingerprint, std::uint32_t attempts) {
  for (const int index : selected_indices) {
    auto *model = report->mutable_assessments(index)->mutable_model();
    model->set_status(status);
    model->set_error_code(error_code);
    model->set_adapter(adapter);
    model->set_model_id(model_id);
    model->set_request_fingerprint(request_fingerprint);
    model->set_response_fingerprint(response_fingerprint);
    model->set_attempts(attempts);
    fuse_assessment(report->mutable_assessments(index));
  }
}

// 对最终报告再次检查已知敏感内容，作为落盘前的防线。
void ensure_report_has_no_secret(const ep::HybridEvaluationReport &report,
                                 const std::vector<std::string> &secrets) {
  const std::string json = protobuf_json(report);
  if (contains_credential_pattern(json) || contains_environment_secret(json, secrets)) {
    throw EvaluationError("credential-like value found in persisted evaluation report");
  }
}

// 构造包含实际内容而非仅指纹的对话评测输入，同时控制字段和长度。
ModelRequest make_dialogue_request(const ep::HybridEvaluationReport &report,
                                   const std::vector<int> &selected_indices,
                                   const std::string &sessionlog, const PromptTemplates &prompts,
                                   SanitizerState *sanitizer, std::uint64_t *content_event_count,
                                   bool standard_trace = false) {
  if (standard_trace && prompts.standard_dialogue_system.empty()) {
    throw EvaluationError("standard trace model evaluation requires standard_dialogue_system; "
                          "use config/standard-runtime.json or provide that prompt in your config");
  }
  rapidjson::Document payload;
  payload.SetObject();
  auto &allocator = payload.GetAllocator();
  payload.AddMember("schema_version", 1, allocator);
  payload.AddMember("evaluation_scope",
                    JsonValue(standard_trace ? "offline_run" : "offline_dialogue", allocator),
                    allocator);

  JsonValue source(rapidjson::kObjectType);
  source.AddMember("conversation_id",
                   JsonValue(report.source().conversation_id().c_str(), allocator), allocator);
  source.AddMember("dialogue_id", JsonValue(report.source().dialogue_id().c_str(), allocator),
                   allocator);
  payload.AddMember("source", source, allocator);

  std::unordered_set<std::string> selected_ids;
  JsonValue selected(rapidjson::kArrayType);
  for (const int index : selected_indices) {
    const std::string &episode_id = report.assessments(index).evidence().episode_id();
    selected_ids.insert(episode_id);
    selected.PushBack(JsonValue(episode_id.c_str(), allocator), allocator);
  }
  payload.AddMember("selected_episode_ids", selected, allocator);

  JsonValue episodes(rapidjson::kArrayType);
  for (const auto &assessment : report.assessments()) {
    JsonValue item(rapidjson::kObjectType);
    const std::string &episode_id = assessment.evidence().episode_id();
    item.AddMember("episode_id", JsonValue(episode_id.c_str(), allocator), allocator);
    item.AddMember("selected", selected_ids.find(episode_id) != selected_ids.end(), allocator);
    const std::vector<std::pair<const char *, std::string>> members = {
        {"evidence", protobuf_json(assessment.evidence())},
        {"rule_assessment", protobuf_json(assessment.rule())},
    };
    for (const auto &member : members) {
      rapidjson::Document parsed;
      parsed.Parse(member.second.data(), member.second.size());
      if (parsed.HasParseError() || !parsed.IsObject()) {
        throw EvaluationError("failed to build dialogue evidence payload");
      }
      JsonValue value;
      value.CopyFrom(parsed, allocator);
      item.AddMember(JsonValue(member.first, allocator), value, allocator);
    }
    episodes.PushBack(item, allocator);
  }
  payload.AddMember("episodes", episodes, allocator);
  JsonValue scopes(rapidjson::kArrayType);
  for (const auto &scope : experience_scopes(report)) {
    scopes.PushBack(JsonValue(scope.c_str(), allocator), allocator);
  }
  payload.AddMember("allowed_experience_scopes", scopes, allocator);

  std::vector<std::string> allowed_ids = collect_allowed_event_ids(report);
  ContentEvents content =
      collect_content_events(sessionlog, allowed_ids, allocator, sanitizer, standard_trace);
  for (const auto &event_id : content.event_ids) {
    append_unique(&allowed_ids, event_id);
  }
  *content_event_count = content.count;
  payload.AddMember("content_events", content.values, allocator);
  payload.AddMember("redaction_count", sanitizer->redaction_count, allocator);
  payload.AddMember("truncation_count", sanitizer->truncation_count, allocator);
  payload.AddMember("content_policy", "sanitized_untrusted_sessionlog_evidence", allocator);

  model_input::dialogue_view(payload, standard_trace);
  ModelRequest request;
  request.system_prompt = standard_trace && !prompts.standard_dialogue_system.empty()
                              ? prompts.standard_dialogue_system
                              : prompts.dialogue_system;
  request.user_prompt =
      render_prompt_template("dialogue_user", prompts.dialogue_user,
                             {{"selected_episode_count", std::to_string(selected_indices.size())},
                              {"evidence_json", serialize_json(payload)}},
                             std::numeric_limits<std::size_t>::max());
  request.allowed_event_ids = std::move(allowed_ids);
  std::string request_material = request.system_prompt;
  request_material.push_back('\0');
  request_material.append(request.user_prompt);
  request.packet_id = "dialogue:" + fingerprint(request_material);
  return request;
}

} // 结束当前匿名命名空间。

// 通过兼容解析器处理原会话日志，复用统一评测和脱敏流程。
EvaluationOutput
HybridTrajectoryEvaluator::evaluate_sessionlog(const std::filesystem::path &path) const {
  const std::string sessionlog = read_bounded_sessionlog(path, options_.max_file_bytes);

  AnalysisReport analysis;
  try {
    AnalyzerOptions analyzer_options;
    analyzer_options.max_file_bytes = options_.max_file_bytes;
    analysis = LoopEpisodeAnalyzer().analyze_file(path, analyzer_options);
  } catch (const AnalysisError &error) {
    throw EvaluationError(std::string("SessionLog analysis failed: ") + error.what());
  }

  return evaluate_content(analysis, sessionlog);
}

// 通过标准协议处理框架无关轨迹，避免依赖 Coordinator 或技能名称。
EvaluationOutput
HybridTrajectoryEvaluator::evaluate_trace_json(const std::string &trace_json) const {
  namespace neutral = ::trajectory::evolution;
  neutral::TraceLimits limits;
  limits.max_bytes = options_.max_file_bytes;
  const auto validated =
      neutral::validate_trace(neutral::parse_trace_json(trace_json, limits), limits);
  const auto analysis = neutral::analyze_trace(validated);
  const neutral::v1::RunOutcome *outcome = nullptr;
  for (const auto &event : validated.trace.events()) {
    if (event.kind() == "run.end")
      outcome = &event.outcome();
  }
  // Source identity and deterministic analysis use the original trace. The
  // model receives only the validated local code view attached at import.
  std::string model_content;
  const auto attributes = ::trajectory::evolution::integration::json(validated.trace.attributes());
  const auto &projection = attributes["training_evaluation"]["evidence_projection"];
  if (projection.has("code_patches")) {
    namespace view = ::trajectory::evolution::integration;
    view::Json evidence{{"trace", view::json(validated.trace)}};
    view::apply_code_projection(evidence, projection);
    evidence["trace"]["attributes"]["training_evaluation"].fields().erase("evidence_projection");
    model_content = neutral::trace_content_jsonl(view::proto<neutral::v1::TraceRun>(evidence["trace"]));
  }
  return evaluate_content(analysis, neutral::trace_content_jsonl(validated.trace), true,
                          outcome == nullptr ? "unknown" : outcome->execution_status(),
                          outcome == nullptr ? "unknown" : outcome->business_status(), model_content);
}

// 按识别出的输入类型选择正确解析入口，统一返回评测报告。
EvaluationOutput HybridTrajectoryEvaluator::evaluate_content(
    const AnalysisReport &analysis, const std::string &sessionlog, bool standard_trace,
    const std::string &execution_status, const std::string &business_status,
    const std::string &model_content) const {

  EvaluatorOptions deterministic_options = options_;
  deterministic_options.model_selection = ModelSelection::none;
  deterministic_options.strict_model = false;
  EvaluationOutput output = HybridTrajectoryEvaluator(deterministic_options, nullptr)
                                .evaluate_json(LoopEpisodeAnalyzer::to_json(analysis, false));

  auto *report = &output.report;
  report->set_evaluation_kind("content_aware_dialogue_evaluation");
  report->set_evaluator_version("hybrid-evaluator-v2");
  report->set_source_fingerprint(fingerprint(sessionlog));
  report->set_content_aware_model(true);
  report->set_model_scope("dialogue");
  report->set_model_prompt_version("content-aware-dialogue-v3");
  report->mutable_dialogue_assessment()->set_status(ep::MODEL_CALL_STATUS_NOT_REQUESTED);

  if (standard_trace) {
    report->set_evaluation_kind("content_aware_trace_evaluation");
    report->set_model_scope("run");
    report->set_model_prompt_version("content-aware-trace-v2");
    // 运行结果必须显式提供，单个分支失败不能覆盖整个运行状态。
    auto aggregate = [](const std::string &status) {
      if (status == "completed" || status == "succeeded")
        return ep::AGGREGATE_STATUS_SUCCEEDED;
      if (status == "failed")
        return ep::AGGREGATE_STATUS_FAILED;
      if (status == "partial")
        return ep::AGGREGATE_STATUS_PARTIAL;
      return ep::AGGREGATE_STATUS_UNKNOWN;
    };
    report->mutable_metrics()->set_workflow_status(aggregate(execution_status));
    report->mutable_metrics()->set_business_status(aggregate(business_status));
  }

  std::vector<int> selected_indices;
  std::unordered_set<std::string> selected_episode_ids;
  std::unordered_set<std::string> all_episode_ids;
  for (int index = 0; index < report->assessments_size(); ++index) {
    const auto &assessment = report->assessments(index);
    all_episode_ids.insert(assessment.evidence().episode_id());
    if (should_select_model(options_.model_selection, assessment.evidence(), assessment.rule())) {
      selected_indices.push_back(index);
      selected_episode_ids.insert(assessment.evidence().episode_id());
    }
  }

  std::vector<std::string> secrets = environment_secrets();
  for (const auto &secret : options_.redaction_secrets) {
    if (!secret.empty() && std::find(secrets.begin(), secrets.end(), secret) == secrets.end()) {
      secrets.push_back(secret);
    }
  }
  std::sort(secrets.begin(), secrets.end(),
            [](const auto &left, const auto &right) { return left.size() > right.size(); });
  if (options_.model_selection == ModelSelection::none) {
    ensure_report_has_no_secret(*report, secrets);
    return output;
  }

  if (!report->source_trace_integrity_valid()) {
    set_selected_model_status(report, selected_indices, ep::MODEL_CALL_STATUS_SKIPPED,
                              "invalid_source_trace", {}, {}, {}, {}, 0);
    auto *dialogue = report->mutable_dialogue_assessment();
    dialogue->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
    dialogue->set_error_code("invalid_source_trace");
    ensure_report_has_no_secret(*report, secrets);
    return output;
  }

  SanitizerState sanitizer;
  sanitizer.secrets = secrets;
  std::uint64_t content_event_count = 0;
  if (options_.prompts == nullptr) {
    throw EvaluationError("dialogue evaluation requires prompt templates");
  }
  ModelRequest request =
      make_dialogue_request(*report, selected_indices, model_content.empty() ? sessionlog : model_content,
                            *options_.prompts, &sanitizer,
                            &content_event_count, standard_trace);
  report->set_content_event_count(content_event_count);
  report->set_redaction_count(sanitizer.redaction_count);

  std::string request_material = request.system_prompt;
  request_material.push_back('\0');
  request_material.append(request.user_prompt);
  const std::string request_fingerprint = fingerprint(request_material);
  const bool request_too_large =
      request.system_prompt.size() > kMaxPromptBytes ||
      request.user_prompt.size() > kMaxPromptBytes - request.system_prompt.size();
  if (request_too_large || options_.max_model_calls == 0U) {
    const std::string error_code =
        request_too_large ? "model_request_too_large" : "model_call_limit_reached";
    set_selected_model_status(report, selected_indices, ep::MODEL_CALL_STATUS_SKIPPED, error_code,
                              {}, {}, request_fingerprint, {}, 0);
    auto *dialogue = report->mutable_dialogue_assessment();
    dialogue->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
    dialogue->set_error_code(error_code);
    dialogue->set_request_fingerprint(request_fingerprint);
    if (options_.strict_model) {
      throw EvaluationError("strict dialogue model evaluation could not run: " + error_code);
    }
    ensure_report_has_no_secret(*report, secrets);
    return output;
  }

  output.model_requests.push_back(request);
  report->set_model_requested_count(1);
  if (!model_adapter_) {
    set_selected_model_status(report, selected_indices, ep::MODEL_CALL_STATUS_SKIPPED,
                              "adapter_not_configured", {}, {}, request_fingerprint, {}, 0);
    auto *dialogue = report->mutable_dialogue_assessment();
    dialogue->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
    dialogue->set_error_code("adapter_not_configured");
    dialogue->set_request_fingerprint(request_fingerprint);
    if (options_.strict_model) {
      throw EvaluationError(
          "strict dialogue model evaluation could not run: adapter_not_configured");
    }
    ensure_report_has_no_secret(*report, secrets);
    return output;
  }

  RawModelResponse response;
  try {
    response = model_adapter_->evaluate(request);
  } catch (...) {
    response.error_code = "adapter_exception";
  }
  const std::string adapter = model_adapter_->name();
  const std::string model_id = model_adapter_->model_id();
  const std::string response_fingerprint =
      response.body.empty() ? std::string() : fingerprint(response.body);
  const DialogueValidation validation = validate_dialogue_response(
      response, request, selected_episode_ids, all_episode_ids, experience_scopes(*report), secrets,
      options_.require_chinese_reason);
  if (validation.json_fence_unwrapped) {
    auto *issue = report->add_issues();
    issue->set_severity("info");
    issue->set_code("dialogue_model_json_fence_unwrapped");
    issue->set_message("已移除响应最外层的单个 JSON 代码块；内容仍经过完整协议与证据校验。");
  }
  if (!validation.valid) {
    const ep::ModelCallStatus status = response.transport_ok
                                           ? ep::MODEL_CALL_STATUS_INVALID_RESPONSE
                                           : ep::MODEL_CALL_STATUS_TRANSPORT_ERROR;
    set_selected_model_status(report, selected_indices, status, validation.error_code, adapter,
                              model_id, request_fingerprint, response_fingerprint,
                              response.attempts);
    auto *dialogue = report->mutable_dialogue_assessment();
    dialogue->set_status(status);
    dialogue->set_adapter(adapter);
    dialogue->set_model_id(model_id);
    dialogue->set_request_fingerprint(request_fingerprint);
    dialogue->set_response_fingerprint(response_fingerprint);
    dialogue->set_error_code(validation.error_code);
    dialogue->set_attempts(response.attempts);
    report->set_model_failed_count(1);
    report->set_model_failures_degraded(!options_.strict_model);
    auto *issue = report->add_issues();
    issue->set_severity(options_.strict_model ? "error" : "warning");
    issue->set_code("dialogue_model_" + validation.error_code);
    issue->set_message("对话级模型评测失败；全部模型语义结果被丢弃，确定性规则结果保留。");
    if (options_.strict_model) {
      throw EvaluationError("strict dialogue model evaluation failed: " + validation.error_code);
    }
    ensure_report_has_no_secret(*report, secrets);
    return output;
  }

  auto *dialogue = report->mutable_dialogue_assessment();
  dialogue->set_status(ep::MODEL_CALL_STATUS_SUCCEEDED);
  dialogue->set_adapter(adapter);
  dialogue->set_model_id(model_id);
  dialogue->set_request_fingerprint(request_fingerprint);
  dialogue->set_response_fingerprint(response_fingerprint);
  dialogue->set_task_outcome(validation.task_outcome);
  dialogue->set_confidence(validation.task_confidence);
  dialogue->set_reason(validation.task_reason);
  dialogue->set_attempts(response.attempts);
  set_repeated_strings(dialogue->mutable_evidence_event_ids(), validation.task_evidence_event_ids);

  auto *reflection = report->mutable_reflection();
  reflection->set_summary(validation.reflection_summary);
  set_repeated_strings(reflection->mutable_root_causes(), validation.root_causes);
  set_repeated_strings(reflection->mutable_skill_orchestration_changes(),
                       validation.skill_orchestration_changes);
  set_repeated_strings(reflection->mutable_experience_injections(),
                       validation.experience_injections);
  set_repeated_strings(reflection->mutable_stop_condition_changes(),
                       validation.stop_condition_changes);
  set_repeated_strings(reflection->mutable_wasted_episode_ids(), validation.wasted_episode_ids);
  set_repeated_strings(reflection->mutable_evidence_event_ids(),
                       validation.reflection_evidence_event_ids);
  for (const auto &advice : validation.experience_details) {
    *reflection->add_experience_details() = advice;
  }
  for (const auto &[code, index] : validation.experience_issues) {
    auto *issue = report->add_issues();
    issue->set_severity("warning");
    issue->set_code(code);
    issue->set_message("经验条目未通过结构化注入契约，已隔离；不影响其他评测结论。索引=" +
                       std::to_string(index));
  }

  for (const int index : selected_indices) {
    auto *assessment = report->mutable_assessments(index);
    const auto judgment = validation.episodes.find(assessment->evidence().episode_id());
    if (judgment == validation.episodes.end()) {
      throw EvaluationError("validated dialogue response lost an episode judgment");
    }
    auto *model = assessment->mutable_model();
    model->set_status(ep::MODEL_CALL_STATUS_SUCCEEDED);
    model->set_adapter(adapter);
    model->set_model_id(model_id);
    model->set_request_fingerprint(request_fingerprint);
    model->set_response_fingerprint(response_fingerprint);
    model->set_verdict(judgment->second.verdict);
    model->set_confidence(judgment->second.confidence);
    model->set_expected_state_change(judgment->second.expected_state_change);
    model->set_observed_state_change(judgment->second.observed_state_change);
    model->set_concise_reason(judgment->second.concise_reason);
    model->set_attempts(response.attempts);
    set_repeated_strings(model->mutable_evidence_event_ids(), judgment->second.evidence_event_ids);
    fuse_assessment(assessment);
  }
  report->set_model_succeeded_count(1);
  ensure_report_has_no_secret(*report, secrets);
  return output;
}

// 返回对话评测系统提示词，内容由外部模板提供。
std::string HybridTrajectoryEvaluator::dialogue_system_prompt() {
  return load_default_prompt_templates()->dialogue_system;
}

} // 命名空间结束：agentflow::trajectory
