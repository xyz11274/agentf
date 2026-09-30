// 混合评测：确定性规则提供可复核指标，模型补充业务语义判断。
// 阅读时区分原始执行事实、规则结论和模型结论，三者不能互相替代。
// 模型请求通过适配器发出；协议校验和隐私处理失败时，不生成伪造的成功评分。

#include "agentflow/trajectory/hybrid_evaluator.h"
#include "model_input.h"
#include "trajectory_evolution/trace.h"
#include "model_json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <google/protobuf/util/json_util.h>
#include <limits>
#include <map>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace agentflow::trajectory {
namespace {

using JsonValue = rapidjson::Value;
namespace ep = evaluation_proto;

constexpr std::size_t kMaxEpisodes = 100000;
constexpr std::size_t kMaxDiagnostics = 100000;
constexpr std::size_t kMaxItemsPerEpisode = 10000;
constexpr std::size_t kMaxEvidenceEventIds = 10000;
constexpr std::size_t kMaxStructuralStringBytes = 1024U * 1024U;
constexpr std::size_t kMaxModelStateBytes = 512;
constexpr std::size_t kMaxModelReasonBytes = 800;
constexpr std::size_t kMaxModelEvidenceIds = 128;
constexpr std::size_t kMaxModelRequestBytes = 2U * 1024U * 1024U;

// 查找 JSON 对象成员；缺失时返回空指针，由调用方决定是否允许缺省。
const JsonValue *find_member(const JsonValue &object, const char *name) {
  if (!object.IsObject()) {
    return nullptr;
  }
  const auto member = object.FindMember(name);
  return member == object.MemberEnd() ? nullptr : &member->value;
}

// 读取必须存在且类型正确的成员，失败时报告对应契约错误。
const JsonValue &require_member(const JsonValue &object, const char *name, rapidjson::Type type,
                                std::string_view context) {
  const JsonValue *value = find_member(object, name);
  if (value == nullptr || value->GetType() != type) {
    throw EvaluationError(std::string(context) + "." + name + " is missing or has an invalid type");
  }
  return *value;
}

// 读取有长度上限的字符串；超长或类型错误不能被静默截成有效结论。
std::string bounded_string(const JsonValue *value, const std::string &context,
                           std::size_t max_bytes = kMaxStructuralStringBytes) {
  if (value == nullptr) {
    return {};
  }
  if (!value->IsString()) {
    throw EvaluationError(context + " must be a string");
  }
  if (value->GetStringLength() > max_bytes) {
    throw EvaluationError(context + " exceeds the configured byte limit");
  }
  return std::string(value->GetString(), value->GetStringLength());
}

// 读取字符串成员并应用本函数的缺省约定；不把其他 JSON 类型隐式转成字符串。
std::string member_string(const JsonValue &object, const char *name, const std::string &context,
                          std::size_t max_bytes = kMaxStructuralStringBytes) {
  return bounded_string(find_member(object, name), context + "." + name, max_bytes);
}

// 读取布尔成员，明确区分字段缺失与显式 false。
bool member_bool(const JsonValue &object, const char *name, bool fallback,
                 const std::string &context) {
  const JsonValue *value = find_member(object, name);
  if (value == nullptr) {
    return fallback;
  }
  if (!value->IsBool()) {
    throw EvaluationError(context + "." + name + " must be a boolean");
  }
  return value->GetBool();
}

// 读取非负整数成员，并按本模块约定处理类型和范围错误。
std::uint64_t member_uint64(const JsonValue &object, const char *name, std::uint64_t fallback,
                            const std::string &context) {
  const JsonValue *value = find_member(object, name);
  if (value == nullptr) {
    return fallback;
  }
  if (value->IsUint64()) {
    return value->GetUint64();
  }
  if (value->IsInt64() && value->GetInt64() >= 0) {
    return static_cast<std::uint64_t>(value->GetInt64());
  }
  throw EvaluationError(context + "." + name + " must be a non-negative integer");
}

// 读取数值成员，供指标和置信度计算使用；类型校验在此边界完成。
double member_double(const JsonValue &object, const char *name, double fallback,
                     const std::string &context) {
  const JsonValue *value = find_member(object, name);
  if (value == nullptr) {
    return fallback;
  }
  if (!value->IsNumber() || !std::isfinite(value->GetDouble())) {
    throw EvaluationError(context + "." + name + " must be a finite number");
  }
  return value->GetDouble();
}

// 保持已有顺序追加尚未出现的元素，避免重复证据或标签影响统计。
void append_unique(std::vector<std::string> &output, const std::string &value) {
  if (value.empty()) {
    return;
  }
  if (std::find(output.begin(), output.end(), value) == output.end()) {
    output.push_back(value);
  }
}

// 读取字符串数组并验证元素类型，避免混合类型进入标签或引用集合。
std::vector<std::string> member_string_array(const JsonValue &object, const char *name,
                                             const std::string &context,
                                             std::size_t max_items = kMaxItemsPerEpisode) {
  std::vector<std::string> output;
  const JsonValue *value = find_member(object, name);
  if (value == nullptr) {
    return output;
  }
  if (!value->IsArray() || value->Size() > max_items) {
    throw EvaluationError(context + "." + name + " is not a bounded array");
  }
  output.reserve(value->Size());
  for (rapidjson::SizeType index = 0; index < value->Size(); ++index) {
    output.push_back(
        bounded_string(&(*value)[index], context + "." + name + "[" + std::to_string(index) + "]"));
  }
  return output;
}

// 先限制可读取的文件大小，再返回内容；异常由调用方转成固定诊断。
std::string read_bounded_file(const std::filesystem::path &path, std::size_t max_file_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    throw EvaluationError("cannot stat analysis file: " + error.message());
  }
  if (size > max_file_bytes) {
    throw EvaluationError("analysis file exceeds configured byte limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw EvaluationError("cannot open analysis file");
  }
  std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    throw EvaluationError("failed while reading analysis file");
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

// 将协议对象转换为 JSON；字段含义仍由 Protobuf 契约约束。
std::string protobuf_json(const google::protobuf::Message &message, bool pretty) {
  google::protobuf::util::JsonPrintOptions options;
  options.add_whitespace = pretty;
  options.preserve_proto_field_names = true;
  std::string output;
  const auto status = google::protobuf::util::MessageToJsonString(message, &output, options);
  if (!status.ok()) {
    throw EvaluationError("failed to serialize Protobuf JSON: " + status.ToString());
  }
  return output;
}

// 把已校验字符串写入重复字段，避免旧字段残留在新结果中。
void set_repeated_strings(google::protobuf::RepeatedPtrField<std::string> *destination,
                          const std::vector<std::string> &values) {
  for (const auto &value : values) {
    *destination->Add() = value;
  }
}

struct ParsedDiagnostic {
  ep::DiagnosticEvidence evidence;
  std::string episode_id;
};

// 从结构化分析输入读取诊断项，保留类型与证据关联。
std::vector<ParsedDiagnostic> parse_diagnostics(const JsonValue &root) {
  std::vector<ParsedDiagnostic> output;
  const JsonValue *diagnostics = find_member(root, "diagnostics");
  if (diagnostics == nullptr) {
    return output;
  }
  if (!diagnostics->IsArray() || diagnostics->Size() > kMaxDiagnostics) {
    throw EvaluationError("diagnostics is not a bounded array");
  }
  output.reserve(diagnostics->Size());
  std::unordered_set<std::string> seen;
  for (rapidjson::SizeType index = 0; index < diagnostics->Size(); ++index) {
    const JsonValue &item = (*diagnostics)[index];
    const std::string context = "diagnostics[" + std::to_string(index) + "]";
    if (!item.IsObject()) {
      throw EvaluationError(context + " must be an object");
    }
    ParsedDiagnostic parsed;
    parsed.evidence.set_diagnostic_id(member_string(item, "diagnostic_id", context));
    if (parsed.evidence.diagnostic_id().empty() ||
        !seen.insert(parsed.evidence.diagnostic_id()).second) {
      throw EvaluationError(context + " has an empty or duplicate diagnostic_id");
    }
    parsed.episode_id = member_string(item, "episode_id", context);
    parsed.evidence.set_category(member_string(item, "category", context));
    parsed.evidence.set_counts_as_invalid(member_bool(item, "counts_as_invalid", false, context));
    const double confidence = member_double(item, "confidence", 0.0, context);
    if (confidence < 0.0 || confidence > 1.0) {
      throw EvaluationError(context + ".confidence must be between 0 and 1");
    }
    parsed.evidence.set_confidence(confidence);
    parsed.evidence.set_severity(member_string(item, "severity", context));
    parsed.evidence.set_summary(member_string(item, "summary", context));
    parsed.evidence.set_observable_cause(member_string(item, "observable_cause", context));
    parsed.evidence.set_tool_name(member_string(item, "tool_name", context));
    parsed.evidence.set_error_class(member_string(item, "error_class", context));
    set_repeated_strings(
        parsed.evidence.mutable_evidence_event_ids(),
        member_string_array(item, "evidence_event_ids", context, kMaxEvidenceEventIds));
    output.push_back(std::move(parsed));
  }
  return output;
}

// 解析派发记录及目标信息，区分调度动作与任务真正完成。
void parse_dispatch(const JsonValue &episode, const std::string &context,
                    ep::DispatchEvidence *output) {
  const JsonValue *dispatch = find_member(episode, "dispatch");
  if (dispatch == nullptr) {
    return;
  }
  if (!dispatch->IsObject()) {
    throw EvaluationError(context + ".dispatch must be an object");
  }
  const std::string dispatch_context = context + ".dispatch";
  output->set_batch_id(member_string(*dispatch, "batch_id", dispatch_context));
  set_repeated_strings(output->mutable_skill_names(),
                       member_string_array(*dispatch, "skill_names", dispatch_context));
  output->set_batch_status(member_string(*dispatch, "batch_status", dispatch_context));
  output->set_workflow_status(member_string(*dispatch, "workflow_status", dispatch_context));
  output->set_successful_count(static_cast<std::uint32_t>(
      std::min<std::uint64_t>(member_uint64(*dispatch, "successful_count", 0, dispatch_context),
                              std::numeric_limits<std::uint32_t>::max())));
  output->set_total_count(static_cast<std::uint32_t>(
      std::min<std::uint64_t>(member_uint64(*dispatch, "total_count", 0, dispatch_context),
                              std::numeric_limits<std::uint32_t>::max())));

  const JsonValue *force = find_member(*dispatch, "force");
  if (force != nullptr) {
    if (!force->IsArray() || force->Size() > kMaxItemsPerEpisode) {
      throw EvaluationError(dispatch_context + ".force is not a bounded array");
    }
    for (const auto &value : force->GetArray()) {
      if (!value.IsBool()) {
        throw EvaluationError(dispatch_context + ".force must contain booleans");
      }
      output->add_force(value.GetBool());
    }
  }
}

// 解析工具执行记录，保留调用身份、输出与失败信息。
void parse_tools(const JsonValue &episode, const std::string &context,
                 ep::EpisodeEvidencePacket *output) {
  const JsonValue *tools = find_member(episode, "tool_actions");
  if (tools == nullptr) {
    return;
  }
  if (!tools->IsArray() || tools->Size() > kMaxItemsPerEpisode) {
    throw EvaluationError(context + ".tool_actions is not a bounded array");
  }
  for (rapidjson::SizeType index = 0; index < tools->Size(); ++index) {
    const JsonValue &tool = (*tools)[index];
    const std::string tool_context = context + ".tool_actions[" + std::to_string(index) + "]";
    if (!tool.IsObject()) {
      throw EvaluationError(tool_context + " must be an object");
    }
    ep::ToolEvidence *target = output->add_tools();
    target->set_tool_call_id(member_string(tool, "tool_call_id", tool_context));
    target->set_tool_name(member_string(tool, "tool_name", tool_context));
    target->set_status(member_string(tool, "status", tool_context));
    target->set_error_class(member_string(tool, "error_class", tool_context));
    target->set_retryability(member_string(tool, "retryability", tool_context));
    target->set_arguments_fingerprint(member_string(tool, "arguments_fingerprint", tool_context));
    target->set_result_fingerprint(member_string(tool, "result_fingerprint", tool_context));
    target->set_call_event_id(member_string(tool, "call_event_id", tool_context));
    target->set_result_event_id(member_string(tool, "result_event_id", tool_context));
    target->set_action_name(member_string(tool, "action_name", tool_context));
    target->set_action_contract_observed(
        member_bool(tool, "action_contract_observed", false, tool_context));
  }
}

// 解析已有评测记录，明确其属于哪次执行和哪组证据。
void parse_evaluations(const JsonValue &episode, const std::string &context,
                       ep::EpisodeEvidencePacket *output) {
  const JsonValue *evaluations = find_member(episode, "evaluations");
  if (evaluations == nullptr) {
    return;
  }
  if (!evaluations->IsArray() || evaluations->Size() > kMaxItemsPerEpisode) {
    throw EvaluationError(context + ".evaluations is not a bounded array");
  }
  for (rapidjson::SizeType index = 0; index < evaluations->Size(); ++index) {
    const JsonValue &evaluation = (*evaluations)[index];
    const std::string evaluation_context = context + ".evaluations[" + std::to_string(index) + "]";
    if (!evaluation.IsObject()) {
      throw EvaluationError(evaluation_context + " must be an object");
    }
    ep::EvaluationEvidence *target = output->add_evaluations();
    target->set_event_id(member_string(evaluation, "event_id", evaluation_context));
    if (target->event_id().empty()) {
      throw EvaluationError(evaluation_context + ".event_id must not be empty");
    }
    target->set_evaluator(member_string(evaluation, "evaluator", evaluation_context));
    target->set_evaluator_version(
        member_string(evaluation, "evaluator_version", evaluation_context));
    target->set_metric(member_string(evaluation, "metric", evaluation_context));

    const JsonValue *score = find_member(evaluation, "score");
    if (score != nullptr && !score->IsNull()) {
      if (!score->IsNumber() || !std::isfinite(score->GetDouble())) {
        throw EvaluationError(evaluation_context + ".score must be finite or null");
      }
      target->set_score(score->GetDouble());
      target->set_has_score(true);
    }

    const JsonValue *passed = find_member(evaluation, "passed");
    if (passed != nullptr && !passed->IsNull()) {
      if (!passed->IsBool()) {
        throw EvaluationError(evaluation_context + ".passed must be a boolean or null");
      }
      target->set_passed(passed->GetBool());
      target->set_has_passed(true);
    }
  }
}

// 建立相邻执行片段关系，为局部反思提供必要上下文。
void fill_neighbor(const ep::EpisodeEvidencePacket &source, ep::NeighborEvidence *target) {
  target->set_episode_id(source.episode_id());
  target->set_step_index(source.step_index());
  target->set_action(source.action());
  target->set_terminal_status(source.terminal_status());
  target->set_outcome_failed(source.outcome_failed());
  target->set_outcome_class_fingerprint(source.outcome_class_fingerprint());
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

// 检查文本中是否包含中文字符，用于验证说明语言要求，而非评估业务正确性。
bool has_cjk(const std::string &value) {
  for (std::size_t index = 0; index + 2 < value.size(); ++index) {
    const unsigned char first = static_cast<unsigned char>(value[index]);
    const unsigned char second = static_cast<unsigned char>(value[index + 1]);
    const unsigned char third = static_cast<unsigned char>(value[index + 2]);
    if (first >= 0xe4 && first <= 0xe9 && (second & 0xc0U) == 0x80U && (third & 0xc0U) == 0x80U) {
      return true;
    }
  }
  return false;
}

// 检测可能为凭据的值，防止未知厂商密钥绕过名称检查。
bool contains_credential_like_value(const std::string &value) {
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
  };
  return std::any_of(patterns.begin(), patterns.end(),
                     [&](const auto &pattern) { return std::regex_search(value, pattern); });
}

struct ModelValidation {
  bool valid = false;
  std::string error_code;
  ep::SemanticVerdict verdict = ep::SEMANTIC_VERDICT_UNSPECIFIED;
  double confidence = 0.0;
  std::string expected_state_change;
  std::string observed_state_change;
  std::string concise_reason;
  std::vector<std::string> evidence_event_ids;
};

// 验证模型结果的格式、枚举、证据范围与隐私，不直接信任自然语言结论。
ModelValidation validate_model_response(const RawModelResponse &response,
                                        const ModelRequest &request, bool require_chinese_reason) {
  ModelValidation output;
  if (!response.transport_ok) {
    output.error_code = response.error_code.empty() ? "transport_error" : response.error_code;
    return output;
  }
  if (!response.error_code.empty()) {
    output.error_code = response.error_code;
    return output;
  }
  if (response.body.empty() || response.body.size() > 64U * 1024U) {
    output.error_code = "invalid_response_size";
    return output;
  }
  if (contains_credential_like_value(response.body)) {
    output.error_code = "credential_like_output";
    return output;
  }

  const auto body = unwrap_model_json(response.body);
  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
      body.data(), body.size());
  if (document.HasParseError() || !unique_object_fields(document)) {
    output.error_code = "invalid_json";
    return output;
  }

  std::string verdict_text;
  try {
    verdict_text = member_string(document, "verdict", "model_response", 64);
  } catch (const EvaluationError &) {
    output.error_code = "invalid_verdict_field";
    return output;
  }
  output.verdict = parse_verdict(verdict_text);
  if (output.verdict == ep::SEMANTIC_VERDICT_UNSPECIFIED) {
    output.error_code = "invalid_verdict";
    return output;
  }
  const JsonValue *confidence = find_member(document, "confidence");
  if (confidence == nullptr || !confidence->IsNumber() || !std::isfinite(confidence->GetDouble()) ||
      confidence->GetDouble() < 0.0 || confidence->GetDouble() > 1.0) {
    output.error_code = "invalid_confidence";
    return output;
  }
  output.confidence = confidence->GetDouble();
  try {
    if (document.HasMember("expected_state_change"))
      output.expected_state_change =
          member_string(document, "expected_state_change", "model_response", kMaxModelStateBytes);
    if (document.HasMember("observed_state_change"))
      output.observed_state_change =
          member_string(document, "observed_state_change", "model_response", kMaxModelStateBytes);
    output.concise_reason =
        member_string(document, "concise_reason", "model_response", kMaxModelReasonBytes);
  } catch (const EvaluationError &) {
    output.error_code = "invalid_text_field";
    return output;
  }
  if (contains_credential_like_value(output.expected_state_change) ||
      contains_credential_like_value(output.observed_state_change) ||
      contains_credential_like_value(output.concise_reason)) {
    output.error_code = "credential_like_output";
    return output;
  }

  if (output.concise_reason.empty() ||
      (document.HasMember("expected_state_change") && output.expected_state_change.empty()) ||
      (document.HasMember("observed_state_change") && output.observed_state_change.empty())) {
    output.error_code = "missing_text_field";
    return output;
  }
  if (require_chinese_reason && !has_cjk(output.concise_reason)) {
    output.error_code = "reason_must_contain_chinese";
    return output;
  }

  const JsonValue *evidence = find_member(document, "evidence_event_ids");
  if (evidence == nullptr || !evidence->IsArray() || evidence->Size() > kMaxModelEvidenceIds) {
    output.error_code = "invalid_evidence_event_ids";
    return output;
  }
  const std::unordered_set<std::string> allowed(request.allowed_event_ids.begin(),
                                                request.allowed_event_ids.end());
  std::unordered_set<std::string> seen;
  for (const auto &item : evidence->GetArray()) {
    if (!item.IsString() || item.GetStringLength() > 512) {
      output.error_code = "invalid_evidence_event_id";
      return output;
    }
    const std::string event_id(item.GetString(), item.GetStringLength());
    if (allowed.find(event_id) == allowed.end()) {
      output.error_code = "unknown_evidence_event_id";
      return output;
    }
    if (seen.insert(event_id).second) {
      output.evidence_event_ids.push_back(event_id);
    }
  }
  if (output.evidence_event_ids.empty()) {
    output.error_code = "empty_evidence_event_ids";
    return output;
  }
  output.valid = true;
  return output;
}

// 收集本次模型允许引用的事件身份，防止引用其他任务的证据。
std::vector<std::string> allowed_event_ids(const ep::EpisodeEvidencePacket &packet) {
  std::vector<std::string> output;
  for (const auto &value : packet.source_event_ids()) {
    append_unique(output, value);
  }
  for (const auto &tool : packet.tools()) {
    append_unique(output, tool.call_event_id());
    append_unique(output, tool.result_event_id());
  }
  for (const auto &evaluation : packet.evaluations()) {
    append_unique(output, evaluation.event_id());
  }
  for (const auto &diagnostic : packet.diagnostics()) {
    for (const auto &value : diagnostic.evidence_event_ids()) {
      append_unique(output, value);
    }
  }
  return output;
}

// 先用确定性规则评估明确问题，为模型评测提供稳定指标基础。
ep::RuleAssessment assess_rules(const ep::EpisodeEvidencePacket &packet) {
  ep::RuleAssessment assessment;
  assessment.set_verdict(ep::SEMANTIC_VERDICT_UNASSESSED);
  double confidence = 0.0;
  bool has_invalid = false;
  bool has_review = false;
  bool has_failed_evaluation = false;
  std::vector<std::string> event_ids;
  for (const auto &diagnostic : packet.diagnostics()) {
    assessment.add_diagnostic_ids(diagnostic.diagnostic_id());
    confidence = std::max(confidence, diagnostic.confidence());
    if (diagnostic.counts_as_invalid()) {
      has_invalid = true;
    } else {
      has_review = true;
    }
    for (const auto &event_id : diagnostic.evidence_event_ids()) {
      append_unique(event_ids, event_id);
    }
  }
  for (const auto &evaluation : packet.evaluations()) {
    if (evaluation.has_passed() && !evaluation.passed()) {
      has_failed_evaluation = true;
      confidence = std::max(confidence, 0.9);
      append_unique(event_ids, evaluation.event_id());
    }
  }
  set_repeated_strings(assessment.mutable_evidence_event_ids(), event_ids);

  if (has_invalid) {
    assessment.set_verdict(ep::SEMANTIC_VERDICT_INVALID);
    assessment.set_confidence(confidence);
    assessment.set_explanation("确定性规则已命中无效循环诊断；模型不得覆盖该硬结论。");
  } else if (has_failed_evaluation) {
    assessment.set_verdict(ep::SEMANTIC_VERDICT_UNCERTAIN);
    assessment.set_confidence(confidence);
    assessment.set_explanation("已有 Evaluation 明确失败；需要判断问题在当前步骤还是其上下文。");
  } else if (has_review) {
    assessment.set_verdict(ep::SEMANTIC_VERDICT_UNCERTAIN);
    assessment.set_confidence(confidence);
    assessment.set_explanation("确定性规则发现可疑行为，但证据不足以直接判为无效。");
  } else if (packet.outcome_failed()) {
    assessment.set_verdict(ep::SEMANTIC_VERDICT_UNCERTAIN);
    assessment.set_confidence(0.55);
    assessment.set_explanation("该步骤的业务结果失败，需要结合任务语义判断重试是否必要。");
  } else {
    assessment.set_confidence(0.0);
    assessment.set_explanation("未命中确定性异常规则；这不等同于已证明该步骤有效。");
  }
  return assessment;
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

// 构建单次模型请求，只发送本次判定需要的证据和契约。
ModelRequest make_model_request(const ep::EpisodeEvidencePacket &packet,
                                const PromptTemplates &prompts) {
  ModelRequest request;
  request.packet_id = packet.packet_id();
  request.system_prompt = prompts.episode_system;
  request.allowed_event_ids = allowed_event_ids(packet);
  rapidjson::Document evidence;
  const auto raw = protobuf_json(packet, false);
  evidence.Parse(raw.data(), raw.size());
  model_input::analysis_view(evidence);
  request.user_prompt = render_prompt_template("episode_user", prompts.episode_user,
                                               {{"evidence_json", model_input::bytes(evidence)}},
                                               std::numeric_limits<std::size_t>::max());
  return request;
}

// 合并规则与模型结论，保留冲突和不确定性，不用模型覆盖硬性证据。
void fuse_assessment(ep::EpisodeAssessment *assessment) {
  const auto rule_verdict = assessment->rule().verdict();
  const bool model_ok = assessment->model().status() == ep::MODEL_CALL_STATUS_SUCCEEDED;
  const auto model_verdict = assessment->model().verdict();

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
    if (rule_verdict == ep::SEMANTIC_VERDICT_UNASSESSED) {
      assessment->set_decision_source(ep::DECISION_SOURCE_MODEL);
      assessment->set_fusion_reason("确定性规则未下语义结论，采用已校验的模型判断。");
    } else {
      assessment->set_decision_source(ep::DECISION_SOURCE_HYBRID);
      assessment->set_fusion_reason("确定性规则负责筛选可疑步骤，模型补充任务语义判断。");
    }
    return;
  }

  assessment->set_final_verdict(rule_verdict);
  assessment->set_final_confidence(assessment->rule().confidence());
  if (rule_verdict == ep::SEMANTIC_VERDICT_UNASSESSED) {
    assessment->set_decision_source(ep::DECISION_SOURCE_NONE);
    assessment->set_fusion_reason("没有足够的确定性证据，也没有可用的模型判断。");
  } else {
    assessment->set_decision_source(ep::DECISION_SOURCE_RULE);
    assessment->set_fusion_reason("模型未提供有效判断，保留确定性规则结论。");
  }
}

// 根据工具真实返回识别失败；缺少执行证据时不能猜测成功。
bool tool_failed(const ep::ToolEvidence &tool) {
  return tool.status() == "error" || tool.status() == "failed" || tool.status() == "failure" ||
         tool.status() == "timeout" || tool.status() == "cancelled" || !tool.error_class().empty();
}

// 根据已验证执行记录计算指标，统一分母和缺失值处理。
void calculate_metrics(ep::HybridEvaluationReport *report) {
  ep::DeterministicMetrics *metrics = report->mutable_metrics();
  bool workflow_success = false;
  bool workflow_failure = false;
  bool workflow_partial = false;
  bool business_success = false;
  bool business_failure = false;
  std::unordered_set<std::string> counted_evaluation_ids;

  for (const auto &assessment : report->assessments()) {
    const auto &packet = assessment.evidence();
    metrics->set_episode_count(metrics->episode_count() + 1);
    if (packet.scope() == "coordinator") {
      metrics->set_coordinator_episode_count(metrics->coordinator_episode_count() + 1);
    } else if (packet.scope() == "skill") {
      metrics->set_skill_episode_count(metrics->skill_episode_count() + 1);
    }
    if (packet.continued()) {
      metrics->set_continued_episode_count(metrics->continued_episode_count() + 1);
    }
    if (packet.terminal()) {
      metrics->set_terminal_episode_count(metrics->terminal_episode_count() + 1);
      if (packet.outcome_failed()) {
        business_failure = true;
      } else if (packet.scope() == "skill") {
        business_success = true;
      }
    }
    if (packet.outcome_failed()) {
      metrics->set_outcome_failed_episode_count(metrics->outcome_failed_episode_count() + 1);
    }
    if (assessment.rule().verdict() == ep::SEMANTIC_VERDICT_INVALID) {
      metrics->set_invalid_episode_count(metrics->invalid_episode_count() + 1);
    } else if (assessment.rule().verdict() == ep::SEMANTIC_VERDICT_UNCERTAIN) {
      metrics->set_review_episode_count(metrics->review_episode_count() + 1);
    } else {
      metrics->set_unassessed_episode_count(metrics->unassessed_episode_count() + 1);
    }

    for (const auto &tool : packet.tools()) {
      metrics->set_tool_call_count(metrics->tool_call_count() + 1);
      if (tool_failed(tool)) {
        metrics->set_failed_tool_call_count(metrics->failed_tool_call_count() + 1);
        business_failure = true;
        if (tool.retryability() == "non_retryable") {
          metrics->set_non_retryable_failed_tool_call_count(
              metrics->non_retryable_failed_tool_call_count() + 1);
        }
      } else if (tool.status() == "completed" || tool.status() == "success") {
        business_success = true;
      }
    }

    for (const auto &evaluation : packet.evaluations()) {
      if (!counted_evaluation_ids.insert(evaluation.event_id()).second) {
        continue;
      }
      metrics->set_evaluation_count(metrics->evaluation_count() + 1);
      if (evaluation.has_passed()) {
        if (evaluation.passed()) {
          metrics->set_passed_evaluation_count(metrics->passed_evaluation_count() + 1);
          business_success = true;
        } else {
          metrics->set_failed_evaluation_count(metrics->failed_evaluation_count() + 1);
          business_failure = true;
        }
      }
    }

    const auto &dispatch = packet.dispatch();
    if (!dispatch.batch_id().empty()) {
      metrics->set_dispatch_batch_count(metrics->dispatch_batch_count() + 1);
      if (dispatch.batch_status() == "partial") {
        metrics->set_partial_dispatch_batch_count(metrics->partial_dispatch_batch_count() + 1);
        business_failure = true;
      } else if (dispatch.batch_status() == "failed") {
        metrics->set_failed_dispatch_batch_count(metrics->failed_dispatch_batch_count() + 1);
        business_failure = true;
      }
      if (dispatch.successful_count() > 0) {
        business_success = true;
      }
      if (dispatch.workflow_status() == "completed") {
        workflow_success = true;
      } else if (dispatch.workflow_status() == "failed") {
        workflow_failure = true;
      } else if (dispatch.workflow_status() == "partial") {
        workflow_partial = true;
      }
    }
  }

  const double episode_count = static_cast<double>(metrics->episode_count());
  const double tool_count = static_cast<double>(metrics->tool_call_count());
  metrics->set_invalid_loop_rate(
      episode_count == 0.0 ? 0.0
                           : static_cast<double>(metrics->invalid_episode_count()) / episode_count);
  metrics->set_failed_outcome_rate(
      episode_count == 0.0
          ? 0.0
          : static_cast<double>(metrics->outcome_failed_episode_count()) / episode_count);
  metrics->set_failed_tool_call_rate(
      tool_count == 0.0 ? 0.0
                        : static_cast<double>(metrics->failed_tool_call_count()) / tool_count);

  if (workflow_failure) {
    metrics->set_workflow_status(ep::AGGREGATE_STATUS_FAILED);
  } else if (workflow_partial) {
    metrics->set_workflow_status(ep::AGGREGATE_STATUS_PARTIAL);
  } else if (workflow_success) {
    metrics->set_workflow_status(ep::AGGREGATE_STATUS_SUCCEEDED);
  } else {
    metrics->set_workflow_status(ep::AGGREGATE_STATUS_UNKNOWN);
  }

  if (business_failure && business_success) {
    metrics->set_business_status(ep::AGGREGATE_STATUS_PARTIAL);
  } else if (business_failure) {
    metrics->set_business_status(ep::AGGREGATE_STATUS_FAILED);
  } else if (business_success) {
    metrics->set_business_status(ep::AGGREGATE_STATUS_SUCCEEDED);
  } else {
    metrics->set_business_status(ep::AGGREGATE_STATUS_UNKNOWN);
  }
}

// 为原子输出生成同目录临时路径，避免跨文件系统替换失败。
std::filesystem::path temporary_output_path(const std::filesystem::path &path) {
  static std::atomic<std::uint64_t> sequence{0};
  return path.parent_path() / (path.filename().string() + ".tmp." +
                               std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
}

template <typename Writer> void atomic_write(const std::filesystem::path &path, Writer writer) {
  if (path.empty()) {
    throw EvaluationError("output path is empty");
  }
  std::error_code error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      throw EvaluationError("cannot create output directory: " + error.message());
    }
  }
  const std::filesystem::path temporary = temporary_output_path(path);
  try {
    writer(temporary);
    std::filesystem::rename(temporary, path, error);
    if (error) {
      throw EvaluationError("cannot publish output file: " + error.message());
    }
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

} // 结束当前匿名命名空间。

// 初始化 HybridTrajectoryEvaluator 的状态和资源；输入参数的校验与所有权在构造阶段确定。
// 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
HybridTrajectoryEvaluator::HybridTrajectoryEvaluator(EvaluatorOptions options,
                                                     std::shared_ptr<ModelAdapter> model_adapter)
    : options_(std::move(options)), model_adapter_(std::move(model_adapter)) {
  if (options_.max_file_bytes == 0 || options_.max_model_calls > kMaxEpisodes) {
    throw EvaluationError("invalid evaluator resource limits");
  }
  if (options_.model_selection != ModelSelection::none && options_.prompts == nullptr) {
    options_.prompts = load_default_prompt_templates();
  }
}

// 读取并评测单个文件，把解析、分析与模型判定的错误分开处理。
EvaluationOutput HybridTrajectoryEvaluator::evaluate_file(const std::filesystem::path &path) const {
  if (::trajectory::evolution::is_trace_path(path)) {
    ::trajectory::evolution::TraceLimits limits;
    limits.max_bytes = options_.max_file_bytes;
    return evaluate_trace_json(
        ::trajectory::evolution::to_json(::trajectory::evolution::read_trace(path, limits), false));
  }
  const std::string input = read_bounded_file(path, options_.max_file_bytes);
  return evaluate_json(input);
}

// 评测已经读取的结构化输入，复用与文件入口相同的验证流程。
EvaluationOutput HybridTrajectoryEvaluator::evaluate_json(const std::string &input) const {
  rapidjson::Document root;
  root.Parse<rapidjson::kParseValidateEncodingFlag>(input.data(), input.size());
  if (root.HasParseError() || !root.IsObject()) {
    throw EvaluationError("invalid analysis JSON at offset " +
                          std::to_string(root.GetErrorOffset()) + ": " +
                          rapidjson::GetParseError_En(root.GetParseError()));
  }

  if (member_uint64(root, "schema_version", 0, "root") != 1 ||
      member_string(root, "analysis_kind", "root") != "loop_episode_analysis") {
    throw EvaluationError("unsupported analysis schema or analysis_kind");
  }

  EvaluationOutput output;
  auto *report = &output.report;
  report->set_schema_version(1);
  report->set_evaluation_kind("hybrid_loop_evaluation");
  report->set_evaluator_version("hybrid-evaluator-v1");
  report->set_deterministic_rule_set_version("hybrid-fusion-rules-v1");
  report->set_source_analyzer_version(member_string(root, "analyzer_version", "root"));
  report->set_source_rule_set_version(member_string(root, "rule_set_version", "root"));
  report->set_source_fingerprint(fingerprint(input));

  const JsonValue &source = require_member(root, "source", rapidjson::kObjectType, "root");
  report->mutable_source()->set_user_id(member_string(source, "user_id", "source"));
  report->mutable_source()->set_conversation_id(member_string(source, "conversation_id", "source"));
  report->mutable_source()->set_dialogue_id(member_string(source, "dialogue_id", "source"));

  const JsonValue &trace_integrity =
      require_member(root, "trace_integrity", rapidjson::kObjectType, "root");
  const bool trace_valid = member_bool(trace_integrity, "valid", false, "trace_integrity");
  report->set_source_trace_integrity_valid(trace_valid);
  if (!trace_valid && !options_.allow_invalid_trace) {
    throw EvaluationError(
        "source trace integrity is invalid; use --allow-invalid-trace to quarantine it");
  }
  if (!trace_valid) {
    auto *issue = report->add_issues();
    issue->set_severity("error");
    issue->set_code("source_trace_integrity_invalid");
    issue->set_message("源轨迹完整性检查失败；保留确定性结果，但禁止调用模型。");
  }

  const JsonValue *source_issues = find_member(trace_integrity, "issues");
  if (source_issues != nullptr) {
    if (!source_issues->IsArray() || source_issues->Size() > kMaxDiagnostics) {
      throw EvaluationError("trace_integrity.issues is not a bounded array");
    }
    for (rapidjson::SizeType index = 0; index < source_issues->Size(); ++index) {
      const JsonValue &source_issue = (*source_issues)[index];
      if (!source_issue.IsObject()) {
        throw EvaluationError("trace_integrity.issues item must be an object");
      }
      auto *issue = report->add_issues();
      issue->set_severity(member_string(source_issue, "severity", "trace_integrity.issues"));
      issue->set_code("source_" + member_string(source_issue, "code", "trace_integrity.issues"));
      issue->set_message(member_string(source_issue, "message", "trace_integrity.issues"));
    }
  }

  const auto diagnostics = parse_diagnostics(root);
  std::unordered_map<std::string, const ParsedDiagnostic *> diagnostic_by_id;
  std::unordered_multimap<std::string, const ParsedDiagnostic *> diagnostics_by_episode;
  for (const auto &diagnostic : diagnostics) {
    diagnostic_by_id.emplace(diagnostic.evidence.diagnostic_id(), &diagnostic);
    diagnostics_by_episode.emplace(diagnostic.episode_id, &diagnostic);
  }

  const JsonValue &episodes = require_member(root, "episodes", rapidjson::kArrayType, "root");
  if (episodes.Size() > kMaxEpisodes) {
    throw EvaluationError("episodes exceeds the configured item limit");
  }
  report->mutable_assessments()->Reserve(static_cast<int>(std::min<std::size_t>(
      episodes.Size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))));

  std::unordered_set<std::string> episode_ids;
  std::unordered_map<std::string, std::vector<std::size_t>> owner_indices;
  for (rapidjson::SizeType index = 0; index < episodes.Size(); ++index) {
    const JsonValue &episode = episodes[index];
    const std::string context = "episodes[" + std::to_string(index) + "]";
    if (!episode.IsObject()) {
      throw EvaluationError(context + " must be an object");
    }
    ep::EpisodeEvidencePacket *packet = report->add_assessments()->mutable_evidence();
    packet->set_episode_id(member_string(episode, "episode_id", context));
    if (packet->episode_id().empty() || !episode_ids.insert(packet->episode_id()).second) {
      throw EvaluationError(context + " has an empty or duplicate episode_id");
    }
    packet->set_scope(member_string(episode, "scope", context));
    packet->set_owner_id(member_string(episode, "owner_id", context));
    packet->set_skill_name(member_string(episode, "skill_name", context));
    packet->set_activation_index(member_uint64(episode, "activation_index", 0, context));
    packet->set_round(member_uint64(episode, "round", 0, context));
    packet->set_attempt_index(member_uint64(episode, "attempt_index", 0, context));
    packet->set_step_index(member_uint64(episode, "step_index", 0, context));
    packet->set_action(member_string(episode, "action", context));
    packet->set_reason(member_string(episode, "reason", context));
    packet->set_continued(member_bool(episode, "continued", false, context));
    packet->set_terminal(member_bool(episode, "terminal", false, context));
    packet->set_terminal_status(member_string(episode, "terminal_status", context));
    packet->set_outcome_failed(member_bool(episode, "outcome_failed", false, context));
    packet->set_all_tool_results_failed(
        member_bool(episode, "all_tool_results_failed", false, context));
    packet->set_has_non_retryable_failure(
        member_bool(episode, "has_non_retryable_failure", false, context));
    packet->set_input_fingerprint(member_string(episode, "input_fingerprint", context));
    packet->set_action_fingerprint(member_string(episode, "action_fingerprint", context));
    packet->set_outcome_fingerprint(member_string(episode, "outcome_fingerprint", context));
    packet->set_outcome_class_fingerprint(
        member_string(episode, "outcome_class_fingerprint", context));
    parse_dispatch(episode, context, packet->mutable_dispatch());
    parse_tools(episode, context, packet);
    parse_evaluations(episode, context, packet);
    set_repeated_strings(
        packet->mutable_source_event_ids(),
        member_string_array(episode, "source_event_ids", context, kMaxEvidenceEventIds));

    std::unordered_set<std::string> attached_diagnostics;
    const auto diagnostic_ids =
        member_string_array(episode, "diagnostic_ids", context, kMaxItemsPerEpisode);
    for (const auto &diagnostic_id : diagnostic_ids) {
      const auto item = diagnostic_by_id.find(diagnostic_id);
      if (item == diagnostic_by_id.end()) {
        auto *issue = report->add_issues();
        issue->set_severity("warning");
        issue->set_code("missing_diagnostic_reference");
        issue->set_message("Episode 引用了不存在的 diagnostic_id。");
        issue->set_episode_id(packet->episode_id());
        continue;
      }
      *packet->add_diagnostics() = item->second->evidence;
      attached_diagnostics.insert(diagnostic_id);
    }
    const auto range = diagnostics_by_episode.equal_range(packet->episode_id());
    for (auto item = range.first; item != range.second; ++item) {
      if (attached_diagnostics.insert(item->second->evidence.diagnostic_id()).second) {
        *packet->add_diagnostics() = item->second->evidence;
      }
    }

    owner_indices[packet->owner_id()].push_back(index);
  }

  for (auto &owner : owner_indices) {
    auto &indices = owner.second;
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t lhs, std::size_t rhs) {
      const auto &left = report->assessments(static_cast<int>(lhs)).evidence();
      const auto &right = report->assessments(static_cast<int>(rhs)).evidence();
      if (left.step_index() != right.step_index()) {
        return left.step_index() < right.step_index();
      }
      return lhs < rhs;
    });
    for (std::size_t position = 0; position < indices.size(); ++position) {
      ep::EpisodeEvidencePacket *packet =
          report->mutable_assessments(static_cast<int>(indices[position]))->mutable_evidence();
      if (position > 0) {
        fill_neighbor(report->assessments(static_cast<int>(indices[position - 1])).evidence(),
                      packet->mutable_previous_same_owner());
      }
      if (position + 1 < indices.size()) {
        fill_neighbor(report->assessments(static_cast<int>(indices[position + 1])).evidence(),
                      packet->mutable_next_same_owner());
      }
    }
  }
  std::unordered_map<std::string, std::string> evaluation_payload_by_id;
  for (int index = 0; index < report->assessments_size(); ++index) {
    ep::EpisodeEvidencePacket *packet = report->mutable_assessments(index)->mutable_evidence();
    packet->clear_packet_id();
    for (const auto &evaluation : packet->evaluations()) {
      std::string evaluation_bytes;
      if (!evaluation.SerializeToString(&evaluation_bytes)) {
        throw EvaluationError("failed to serialize Evaluation evidence");
      }
      const auto inserted =
          evaluation_payload_by_id.emplace(evaluation.event_id(), evaluation_bytes);
      if (!inserted.second && inserted.first->second != evaluation_bytes) {
        throw EvaluationError("conflicting Evaluation payloads share the same event_id");
      }
    }

    const std::string evidence_json = protobuf_json(*packet, false);
    if (contains_credential_like_value(evidence_json)) {
      throw EvaluationError("credential-like value found in structural evidence packet");
    }
    std::string packet_bytes;
    if (!packet->SerializeToString(&packet_bytes)) {
      throw EvaluationError("failed to serialize evidence packet");
    }
    packet->set_packet_id("packet:" + fingerprint(packet_bytes));
  }

  std::size_t selected_model_calls = 0;
  for (int index = 0; index < report->assessments_size(); ++index) {
    ep::EpisodeAssessment *assessment = report->mutable_assessments(index);
    *assessment->mutable_rule() = assess_rules(assessment->evidence());
    ep::ModelAssessment *model = assessment->mutable_model();
    model->set_status(ep::MODEL_CALL_STATUS_NOT_REQUESTED);

    const bool selected =
        should_select_model(options_.model_selection, assessment->evidence(), assessment->rule());
    if (selected) {
      if (options_.prompts == nullptr) {
        throw EvaluationError("model evaluation requires prompt templates");
      }
      ModelRequest request = make_model_request(assessment->evidence(), *options_.prompts);
      const bool request_too_large =
          request.system_prompt.size() > kMaxModelRequestBytes ||
          request.user_prompt.size() > kMaxModelRequestBytes - request.system_prompt.size();
      ++selected_model_calls;
      const bool request_eligible =
          trace_valid && !request_too_large && selected_model_calls <= options_.max_model_calls;
      if (request_eligible) {
        output.model_requests.push_back(request);
        report->set_model_requested_count(report->model_requested_count() + 1);
      }

      if (!trace_valid) {
        model->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
        model->set_error_code("invalid_source_trace");
      } else if (request_too_large) {
        model->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
        model->set_error_code("model_request_too_large");
      } else if (selected_model_calls > options_.max_model_calls) {
        model->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
        model->set_error_code("model_call_limit_reached");
      } else if (!model_adapter_) {
        model->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
        model->set_error_code("adapter_not_configured");
      } else {
        std::string request_material = request.system_prompt;
        request_material.push_back('\0');
        request_material.append(request.user_prompt);
        model->set_request_fingerprint(fingerprint(request_material));
        model->set_adapter(model_adapter_->name());
        model->set_model_id(model_adapter_->model_id());

        RawModelResponse response;
        try {
          response = model_adapter_->evaluate(request);
        } catch (...) {
          response.error_code = "adapter_exception";
        }
        model->set_attempts(response.attempts);
        if (!response.body.empty()) {
          model->set_response_fingerprint(fingerprint(response.body));
        }
        const ModelValidation validation =
            validate_model_response(response, request, options_.require_chinese_reason);
        if (validation.valid) {
          model->set_status(ep::MODEL_CALL_STATUS_SUCCEEDED);
          model->set_verdict(validation.verdict);
          model->set_confidence(validation.confidence);
          model->set_expected_state_change(validation.expected_state_change);
          model->set_observed_state_change(validation.observed_state_change);
          model->set_concise_reason(validation.concise_reason);
          set_repeated_strings(model->mutable_evidence_event_ids(), validation.evidence_event_ids);
          report->set_model_succeeded_count(report->model_succeeded_count() + 1);
        } else {
          model->set_status(response.transport_ok ? ep::MODEL_CALL_STATUS_INVALID_RESPONSE
                                                  : ep::MODEL_CALL_STATUS_TRANSPORT_ERROR);
          model->set_error_code(validation.error_code);
          report->set_model_failed_count(report->model_failed_count() + 1);
          report->set_model_failures_degraded(!options_.strict_model);
          auto *issue = report->add_issues();
          issue->set_severity(options_.strict_model ? "error" : "warning");
          issue->set_code("model_" + validation.error_code);
          issue->set_message("模型评测失败；未使用未经校验的输出。");
          issue->set_episode_id(assessment->evidence().episode_id());
          if (options_.strict_model) {
            throw EvaluationError("strict model evaluation failed for episode " +
                                  assessment->evidence().episode_id() + ": " +
                                  validation.error_code);
          }
        }
      }
      if (options_.strict_model && model->status() == ep::MODEL_CALL_STATUS_SKIPPED &&
          model->error_code() != "invalid_source_trace") {
        throw EvaluationError("strict model evaluation could not run: " + model->error_code());
      }
    }
    fuse_assessment(assessment);
  }

  calculate_metrics(report);
  const std::string persisted_report_json = protobuf_json(*report, false);
  if (contains_credential_like_value(persisted_report_json)) {
    throw EvaluationError("credential-like value found in persisted evaluation report");
  }
  return output;
}

// 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
std::string HybridTrajectoryEvaluator::to_json(const ep::HybridEvaluationReport &report,
                                               bool pretty) {
  return protobuf_json(report, pretty);
}

// 按协议格式写出结果，供跨语言读取和持久化回放。
void HybridTrajectoryEvaluator::write_protobuf(const ep::HybridEvaluationReport &report,
                                               const std::filesystem::path &path) {
  atomic_write(path, [&](const std::filesystem::path &temporary) {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !report.SerializeToOstream(&output) || !output.flush()) {
      throw EvaluationError("failed to write Protobuf output");
    }
  });
}

// 按行写出允许保存的模型请求用于诊断，写入前遵守脱敏约束。
void HybridTrajectoryEvaluator::write_model_requests_jsonl(
    const std::vector<ModelRequest> &requests, const std::filesystem::path &path) {
  atomic_write(path, [&](const std::filesystem::path &temporary) {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw EvaluationError("cannot open model request output");
    }
    for (const auto &request : requests) {
      rapidjson::Document document;
      document.SetObject();
      auto &allocator = document.GetAllocator();
      document.AddMember("packet_id", rapidjson::Value(request.packet_id.c_str(), allocator),
                         allocator);
      document.AddMember("system_prompt",
                         rapidjson::Value(request.system_prompt.c_str(), allocator), allocator);
      document.AddMember("user_prompt", rapidjson::Value(request.user_prompt.c_str(), allocator),
                         allocator);
      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
      document.Accept(writer);
      output.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
      output.put('\n');
    }
    if (!output.flush()) {
      throw EvaluationError("failed to write model request output");
    }
  });
}

// 返回当前评测器使用的系统提示词，来源由运行配置控制。
std::string HybridTrajectoryEvaluator::system_prompt() {
  return load_default_prompt_templates()->episode_system;
}

// 将内部枚举映射为稳定的协议名称，保证日志和序列化使用统一表示。
std::string to_string(ModelSelection selection) {
  switch (selection) {
  case ModelSelection::none:
    return "none";
  case ModelSelection::suspicious:
    return "suspicious";
  case ModelSelection::all:
    return "all";
  }
  return "none";
}

// 解析何时启用模型的选择模式，拒绝未支持的名称。
ModelSelection parse_model_selection(const std::string &value) {
  if (value == "none") {
    return ModelSelection::none;
  }
  if (value == "suspicious") {
    return ModelSelection::suspicious;
  }
  if (value == "all") {
    return ModelSelection::all;
  }
  throw EvaluationError("model selection must be none, suspicious, or all");
}

} // 命名空间结束：agentflow::trajectory
