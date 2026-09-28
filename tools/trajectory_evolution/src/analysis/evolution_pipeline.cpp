// 进化流水线：把分析与评测证据整理为候选经验，再进行去重、关系判定和入库。
// 阅读顺序：输入验证 → 候选生成 → 元反思 → 记忆更新；来源证据与处理结果分开保存。
// 失败或未知结论不自动变成可靠经验；已有经验的状态变化必须能追溯到证据。

#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/experience.h"
#include "trajectory_evolution/trace.h"
#include "model_json.h"
#include "integration_json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <functional>
#include <iterator>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>
#include <map>
#include <limits>
#include <numeric>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <tuple>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
extern char **environ;
#endif

namespace agentflow::trajectory {
namespace {

namespace ep = evaluation_proto;
using JsonValue = rapidjson::Value;
using Allocator = rapidjson::Document::AllocatorType;

constexpr char kPipelineVersion[] = "trajectory-evolution-pipeline-v1";
constexpr char kCandidateGeneratorVersion[] = "evolution-candidate-generator-v2";
constexpr char kMemoryStoreVersion[] = "evolution-memory-store-v1";
constexpr char kMemoryCuratorVersion[] = "evolution-memory-curator-v4";
constexpr std::size_t kMaxCandidateTextBytes = 2048;
constexpr std::size_t kMaxMetaGroups = 1024;
constexpr std::size_t kMaxMetaSeedsPerGroup = 256;

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
  options.preserve_proto_field_names = true;
  options.add_whitespace = pretty;
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

// 查找 JSON 对象成员；缺失时返回空指针，由调用方决定是否允许缺省。
const JsonValue *find_member(const JsonValue &object, const char *name) {
  if (!object.IsObject()) {
    return nullptr;
  }
  const auto member = object.FindMember(name);
  return member == object.MemberEnd() ? nullptr : &member->value;
}

// 检查文本中是否包含中文字符，用于验证说明语言要求，而非评估业务正确性。
bool has_cjk(const std::string &value) {
  for (std::size_t index = 0; index + 2U < value.size(); ++index) {
    const unsigned char first = static_cast<unsigned char>(value[index]);
    const unsigned char second = static_cast<unsigned char>(value[index + 1U]);
    const unsigned char third = static_cast<unsigned char>(value[index + 2U]);
    if (first >= 0xe3U && first <= 0xefU && (second & 0xc0U) == 0x80U && (third & 0xc0U) == 0x80U) {
      return true;
    }
  }
  return false;
}

// 规范化用于比较的文本；原始执行内容由证据层保存，不能用规范化结果替代回放。
std::string normalize_text(const std::string &value) {
  std::string output;
  output.reserve(value.size());
  bool pending_space = false;
  for (const unsigned char ch : value) {
    if (std::isspace(ch)) {
      pending_space = !output.empty();
      continue;
    }
    if (pending_space) {
      output.push_back(' ');
      pending_space = false;
    }
    output.push_back(ch < 0x80U ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch));
  }
  return output;
}

// 将展示文本限制在约定长度内；该预览不替代完整原始证据。
std::string bounded_text(std::string value) {
  if (value.size() <= kMaxCandidateTextBytes) {
    return value;
  }
  std::size_t boundary = kMaxCandidateTextBytes;
  while (boundary > 0U && (static_cast<unsigned char>(value[boundary]) & 0xc0U) == 0x80U) {
    --boundary;
  }
  return value.substr(0, boundary) + "<TRUNCATED sha256=" + fingerprint(value) + ">";
}

// 排序并去除重复值，保证序列化和摘要不受插入顺序影响。
void sort_unique(std::vector<std::string> *values) {
  std::sort(values->begin(), values->end());
  values->erase(std::unique(values->begin(), values->end()), values->end());
}

// 保持已有顺序追加尚未出现的元素，避免重复证据或标签影响统计。
void append_unique(std::vector<std::string> *values, const std::string &value) {
  if (!value.empty() && std::find(values->begin(), values->end(), value) == values->end()) {
    values->push_back(value);
  }
}

// 保持已有顺序追加尚未出现的元素，避免重复证据或标签影响统计。
void append_unique(google::protobuf::RepeatedPtrField<std::string> *values,
                   const std::string &value) {
  if (value.empty()) {
    return;
  }
  for (const auto &existing : *values) {
    if (existing == value) {
      return;
    }
  }
  *values->Add() = value;
}

template <typename Range> std::vector<std::string> strings_from(const Range &values) {
  return std::vector<std::string>(values.begin(), values.end());
}

// 将字符串集合写回协议字段，保留当前函数规定的顺序与去重语义。
void set_strings(google::protobuf::RepeatedPtrField<std::string> *destination,
                 std::vector<std::string> values) {
  sort_unique(&values);
  destination->Clear();
  for (const auto &value : values) {
    *destination->Add() = value;
  }
}

// 按固定分隔方式拼接内容，使摘要和展示的输入保持确定性。
std::string joined(const google::protobuf::RepeatedPtrField<std::string> &values,
                   const std::string &separator) {
  std::string output;
  for (const auto &value : values) {
    if (!output.empty()) {
      output.append(separator);
    }
    output.append(value);
  }
  return output;
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

// 收集当前进程已知的凭据值供脱敏匹配；这些原文只留在内存中。
std::vector<std::string> collected_secrets(const std::vector<std::string> &configured) {
  std::vector<std::string> output;
  for (const auto &value : configured) {
    if (value.size() >= 8U) {
      append_unique(&output, value);
    }
  }
#if defined(__unix__) || defined(__APPLE__)
  if (environ != nullptr) {
    for (char **item = environ; *item != nullptr; ++item) {
      const std::string entry(*item);
      const std::size_t separator = entry.find('=');
      if (separator != std::string::npos && secret_environment_name(entry.substr(0, separator))) {
        const std::string value = entry.substr(separator + 1U);
        if (value.size() >= 8U) {
          append_unique(&output, value);
        }
      }
    }
  }
#endif
  std::sort(output.begin(), output.end(),
            [](const auto &left, const auto &right) { return left.size() > right.size(); });
  return output;
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

// 检查文本是否包含已知凭据或敏感模式；命中后拒绝进入模型输入或持久化结果。
bool contains_secret(const std::string &value, const std::vector<std::string> &secrets) {
  // 分别检查解码后的 JSON 字符串；脱敏 URL 后的转义换行
  // 不能把签名内容与后续正文错误拼接。
  std::vector<std::pair<std::string, unsigned>> pending{{value, 0}};
  const auto visit_limit = std::max<std::size_t>(
      1024, std::min(value.size(), std::numeric_limits<std::size_t>::max() / 2) * 2);
  std::size_t visited = 0;
  while (!pending.empty()) {
    auto current = std::move(pending.back());
    pending.pop_back();
    if (++visited > visit_limit || current.second > 16)
      return true;
    const auto &text = current.first;
    for (const auto &secret : secrets) {
      if (!secret.empty() && text.find(secret) != std::string::npos)
        return true;
    }
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first != std::string::npos &&
        (text[first] == '{' || text[first] == '[' || text[first] == '"')) {
      rapidjson::Document document;
      document.Parse<rapidjson::kParseIterativeFlag>(text.data(), text.size());
      if (!document.HasParseError()) {
        std::vector<const JsonValue *> fields{&document};
        while (!fields.empty()) {
          const auto *item = fields.back();
          fields.pop_back();
          if (++visited > visit_limit)
            return true;
          if (item->IsObject())
            for (auto it = item->MemberBegin(); it != item->MemberEnd(); ++it) {
              fields.push_back(&it->name);
              fields.push_back(&it->value);
            }
          else if (item->IsArray())
            for (const auto &child : item->GetArray())
              fields.push_back(&child);
          else if (item->IsString())
            pending.emplace_back(std::string(item->GetString(), item->GetStringLength()),
                                 current.second + 1);
        }
        continue;
      }
    }
    // 只有精确匹配的宿主占位符可豁免，相邻凭据文本仍需检查。
    // 已知敏感值，包括 JSON 转义形式，仍须拒绝。
    auto inspected = text;
    const std::string marker = "<redacted:credential_field>";
    for (auto position = inspected.find(marker); position != std::string::npos;
         position = inspected.find(marker, position))
      inspected.erase(position, marker.size());
    if (std::any_of(credential_patterns().begin(), credential_patterns().end(),
                    [&](const auto &pattern) { return std::regex_search(inspected, pattern); }))
      return true;
  }
  return false;
}

// 只返回经过限制的诊断，避免把模型响应、路径或凭据作为错误消息回显。
std::string safe_error_message(std::string value, const std::vector<std::string> &secrets) {
  for (const auto &secret : secrets) {
    std::size_t position = 0;
    while (!secret.empty() && (position = value.find(secret, position)) != std::string::npos) {
      value.replace(position, secret.size(), "<REDACTED:SECRET>");
      position += 17U;
    }
  }
  for (const auto &pattern : credential_patterns()) {
    value = std::regex_replace(value, pattern, "<REDACTED:CREDENTIAL>");
  }
  static const std::regex local_path(R"((^|[\s"'=])/(root|home|tmp|var|etc|mnt)/[^\s"']+)",
                                     std::regex::icase);
  value = std::regex_replace(value, local_path, "$1<REDACTED:LOCAL_PATH>");
  return bounded_text(std::move(value));
}

// 在消息进入模型或输出前检查敏感内容，异常不能回显被拒绝的原文。
void ensure_safe_message(const google::protobuf::Message &message,
                         const std::vector<std::string> &secrets) {
  const std::string json = protobuf_json(message, false);
  if (contains_secret(json, secrets)) {
    throw EvaluationError("credential-like value found in persisted evolution data");
  }
}

// 根据执行身份确定经验作用域，避免把用户专属事实作为公共经验。
std::string scope_for(const ep::EpisodeEvidencePacket &packet) {
  if (packet.scope().rfind("agent:", 0) == 0)
    return packet.scope();
  if (packet.scope() == "skill") {
    return packet.skill_name().empty() ? "skill:unknown" : "skill:" + packet.skill_name();
  }
  return "coordinator";
}

// 把分析类别映射到经验、编排或循环控制目标，保持下游字段一致。
ep::CandidateTargetType target_for_category(const std::string &category) {
  if (category == "invalid_action_repeat") {
    return ep::CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION;
  }
  if (category == "result_not_consumed") {
    return ep::CANDIDATE_TARGET_TYPE_TOOL_RESULT_CONSUMPTION;
  }
  if (category == "non_retryable_repeat") {
    return ep::CANDIDATE_TARGET_TYPE_RETRY_POLICY;
  }
  if (category == "exact_repeat" || category == "no_state_progress" ||
      category == "state_oscillation") {
    return ep::CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION;
  }
  return ep::CANDIDATE_TARGET_TYPE_UNSPECIFIED;
}

// 为规则诊断构造候选改进方向，不将诊断直接升级为可执行策略。
std::string proposal_for_category(const std::string &category) {
  if (category == "invalid_action_repeat") {
    return "在已有终态 Batch 后，禁止以相同 Skill 和相同 Prompt 无条件再次派发；"
           "只有输入发生变化或 Coordinator 显式给出 force 理由时才允许重排。";
  }
  if (category == "result_not_consumed") {
    return "下一轮决策前必须把上一轮 Tool Result 按 tool_call_id 写入模型上下文；"
           "结果缺失时停止继续调用并记录轨迹完整性错误。";
  }
  if (category == "non_retryable_repeat") {
    return "Tool 返回非重试错误后立即终止同类重试，转为重新规划、切换能力或向用户"
           "说明阻塞原因。";
  }
  if (category == "exact_repeat") {
    return "当动作、参数和失败结果类别与上一轮完全相同时，不再重复执行；根据错误的"
           "retryability 选择停止或重新规划。";
  }
  if (category == "no_state_progress") {
    return "连续执行必须证明产生新的可观察状态；达到无进展阈值后停止当前 Loop 并"
           "触发重新规划。";
  }
  if (category == "state_oscillation") {
    return "检测到 A-B-A 状态回摆且结果仍失败时停止循环，保留冲突状态并交由"
           "Coordinator 重新规划。";
  }
  return {};
}

// 按改进目标描述预期收益，后续仍需真实评测确认。
std::string benefit_for_target(ep::CandidateTargetType target) {
  switch (target) {
  case ep::CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION:
    return "减少重复派发和并发分支重放，同时保持其他 Skill 分支独立完成。";
  case ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION:
    return "把可复用经验注入后续决策上下文，减少同类错误和无效决策。";
  case ep::CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION:
    return "降低无状态进展的 Loop 次数和 Token 消耗，同时保留有依据的重试。";
  case ep::CANDIDATE_TARGET_TYPE_RETRY_POLICY:
    return "避免对确定性失败反复重试，并把可恢复错误与不可恢复错误分开处理。";
  case ep::CANDIDATE_TARGET_TYPE_TOOL_RESULT_CONSUMPTION:
    return "保证每次 Tool 结果进入下一轮状态，避免因结果丢失而重复调用。";
  default:
    return "减少可验证的无效执行。";
  }
}

// 根据错误类别决定可重试性，区分暂时故障与重复执行也无法恢复的问题。
std::string retryability_for(const ep::EpisodeEvidencePacket &packet,
                             const ep::DiagnosticEvidence &diagnostic) {
  std::set<std::string> values;
  for (const auto &tool : packet.tools()) {
    const bool tool_matches =
        diagnostic.tool_name().empty() || tool.tool_name() == diagnostic.tool_name();
    const bool error_matches =
        diagnostic.error_class().empty() || tool.error_class() == diagnostic.error_class();
    if (tool_matches && error_matches && !tool.retryability().empty()) {
      values.insert(tool.retryability());
    }
  }
  if (values.empty()) {
    return "unknown";
  }
  if (values.size() > 1U) {
    return "mixed";
  }
  return *values.begin();
}

// 组合候选的目标、作用域和内容特征，供同批次去重使用。
std::string candidate_key(const ep::EvolutionCandidate &candidate) {
  return std::to_string(static_cast<int>(candidate.target_type())) + '|' +
         candidate.generator_version() + '|' + normalize_text(candidate.target_scope()) + '|' +
         normalize_text(candidate.trigger_pattern()) + '|' + candidate.proposal_fingerprint();
}

// 生成候选的稳定身份，连接反思、入库决定与策略来源。
std::string candidate_id(const ep::EvolutionCandidate &candidate) {
  return "candidate:" + fingerprint(candidate_key(candidate));
}

// 为来源证据生成稳定身份，不能只用不同运行都可能重复的局部步骤名。
std::string evidence_id(const ep::CandidateEvidence &evidence) {
  std::vector<std::string> episodes = strings_from(evidence.episode_ids());
  std::vector<std::string> diagnostics = strings_from(evidence.diagnostic_ids());
  std::vector<std::string> events = strings_from(evidence.evidence_event_ids());
  sort_unique(&episodes);
  sort_unique(&diagnostics);
  sort_unique(&events);
  std::string material = std::to_string(static_cast<int>(evidence.kind())) + '|' +
                         evidence.source().user_id() + '|' + evidence.source().conversation_id() +
                         '|' + evidence.source().dialogue_id() + '|' +
                         evidence.source_fingerprint() + '|';
  for (const auto &value : episodes) {
    material.append("episode=").append(value).push_back('|');
  }
  for (const auto &value : diagnostics) {
    material.append("diagnostic=").append(value).push_back('|');
  }
  for (const auto &value : events) {
    material.append("event=").append(value).push_back('|');
  }
  return "evidence:" + fingerprint(material);
}

// 从真实分析结果构造证据记录，保留运行和事件定位信息。
ep::CandidateEvidence make_evidence(ep::CandidateEvidenceKind kind,
                                    const ep::HybridEvaluationReport &report,
                                    const std::vector<std::string> &episode_ids,
                                    const std::vector<std::string> &diagnostic_ids,
                                    const std::vector<std::string> &event_ids,
                                    ep::SemanticVerdict verdict, double confidence) {
  ep::CandidateEvidence evidence;
  evidence.set_kind(kind);
  evidence.mutable_source()->CopyFrom(report.source());
  evidence.set_source_fingerprint(report.source_fingerprint());
  set_strings(evidence.mutable_episode_ids(), episode_ids);
  set_strings(evidence.mutable_diagnostic_ids(), diagnostic_ids);
  set_strings(evidence.mutable_evidence_event_ids(), event_ids);
  evidence.set_verdict(verdict);
  evidence.set_confidence(std::clamp(confidence, 0.0, 1.0));
  evidence.set_evidence_id(evidence_id(evidence));
  return evidence;
}

// 合入经过归属校验的证据，避免重复引用扩大支持数量。
void add_evidence(google::protobuf::RepeatedPtrField<ep::CandidateEvidence> *destination,
                  const ep::CandidateEvidence &evidence) {
  for (const auto &existing : *destination) {
    if (existing.evidence_id() == evidence.evidence_id()) {
      return;
    }
  }
  destination->Add()->CopyFrom(evidence);
}

// 对证据按稳定顺序整理，保证摘要与输出可重复。
void sort_evidence(google::protobuf::RepeatedPtrField<ep::CandidateEvidence> *evidence) {
  std::vector<ep::CandidateEvidence> values(evidence->begin(), evidence->end());
  std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) {
    return left.evidence_id() < right.evidence_id();
  });
  evidence->Clear();
  for (const auto &value : values) {
    evidence->Add()->CopyFrom(value);
  }
}

// 在等价候选文本中选择稳定代表，避免输入顺序造成内容抖动。
void choose_stable_text(std::string *destination, const std::string &value) {
  if (value.empty()) {
    return;
  }
  if (destination->empty() || normalize_text(value) < normalize_text(*destination)) {
    *destination = bounded_text(value);
  }
}

// 合并同类候选的内容和来源，不丢弃相互矛盾的证据。
void merge_candidate_payload(ep::EvolutionCandidate *destination,
                             const ep::EvolutionCandidate &source) {
  if (destination->schema_version() == 0U) {
    destination->CopyFrom(source);
    return;
  }
  std::string observed = destination->observed_problem();
  choose_stable_text(&observed, source.observed_problem());
  destination->set_observed_problem(observed);
  std::string benefit = destination->expected_benefit();
  choose_stable_text(&benefit, source.expected_benefit());
  destination->set_expected_benefit(benefit);
  destination->set_meta_reflection_applied(destination->meta_reflection_applied() ||
                                           source.meta_reflection_applied());
  destination->set_required_supporting_dialogues(std::max(
      destination->required_supporting_dialogues(), source.required_supporting_dialogues()));
  destination->set_required_supporting_episodes(
      std::max(destination->required_supporting_episodes(), source.required_supporting_episodes()));
  for (const auto &evidence : source.support_evidence()) {
    add_evidence(destination->mutable_support_evidence(), evidence);
  }
  for (const auto &evidence : source.contradiction_evidence()) {
    add_evidence(destination->mutable_contradiction_evidence(), evidence);
  }
  for (const auto &seed : source.source_seed_ids()) {
    append_unique(destination->mutable_source_seed_ids(), seed);
  }
}

// 完成候选字段、身份和证据整理，生成可供审查的条目。
void finalize_candidate(ep::EvolutionCandidate *candidate) {
  sort_evidence(candidate->mutable_support_evidence());
  sort_evidence(candidate->mutable_contradiction_evidence());
  std::vector<std::string> seeds = strings_from(candidate->source_seed_ids());
  set_strings(candidate->mutable_source_seed_ids(), std::move(seeds));

  std::set<std::string> users;
  std::set<std::string> conversations;
  std::set<std::string> dialogues;
  std::set<std::string> episodes;
  std::set<std::string> invalid_episodes;
  double confidence_sum = 0.0;
  for (const auto &evidence : candidate->support_evidence()) {
    if (!evidence.source().user_id().empty()) {
      users.insert(evidence.source().user_id());
    }
    if (!evidence.source().conversation_id().empty()) {
      conversations.insert(evidence.source().user_id() + '\0' +
                           evidence.source().conversation_id());
    }
    if (!evidence.source().dialogue_id().empty()) {
      dialogues.insert(evidence.source().user_id() + '\0' + evidence.source().conversation_id() +
                       '\0' + evidence.source().dialogue_id());
    }
    for (const auto &episode : evidence.episode_ids()) {
      // 片段编号只在单份轨迹内有效；独立运行可能都使用 step-1。
      const auto episode_key = evidence.source().user_id() + '\0' +
                               evidence.source().conversation_id() + '\0' +
                               evidence.source().dialogue_id() + '\0' + episode;
      episodes.insert(episode_key);
      if (evidence.verdict() == ep::SEMANTIC_VERDICT_INVALID) {
        invalid_episodes.insert(episode_key);
      }
    }
    confidence_sum += evidence.confidence();
  }
  candidate->set_support_user_count(users.size());
  candidate->set_support_conversation_count(conversations.size());
  candidate->set_support_dialogue_count(dialogues.size());
  candidate->set_support_episode_count(episodes.size());
  candidate->set_invalid_episode_count(invalid_episodes.size());
  candidate->set_contradiction_count(candidate->contradiction_evidence_size());

  const double evidence_mean =
      candidate->support_evidence_size() == 0
          ? 0.0
          : confidence_sum / static_cast<double>(candidate->support_evidence_size());
  const std::uint64_t support_units =
      episodes.empty() ? static_cast<std::uint64_t>(candidate->support_evidence_size())
                       : static_cast<std::uint64_t>(episodes.size());
  const std::uint64_t required_dialogues =
      std::max<std::uint64_t>(1U, candidate->required_supporting_dialogues());
  const std::uint64_t required_episodes =
      std::max<std::uint64_t>(1U, candidate->required_supporting_episodes());
  const double dialogue_factor = std::min(1.0, static_cast<double>(dialogues.size()) /
                                                   static_cast<double>(required_dialogues));
  const double episode_factor =
      std::min(1.0, static_cast<double>(support_units) / static_cast<double>(required_episodes));
  double confidence = evidence_mean * 0.7 + std::min(dialogue_factor, episode_factor) * 0.3;
  if (candidate->contradiction_count() > 0U) {
    confidence *= 0.6;
  }
  candidate->set_confidence(std::clamp(confidence, 0.0, 1.0));

  if (is_scoped_experience(*candidate) &&
      !experience_fields_valid(candidate->target_scope(), candidate->trigger_pattern(),
                               candidate->proposed_change())) {
    candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_REJECTED_BY_GATE);
    candidate->set_gate_reason("经验缺少明确的执行者、适用条件或行为正文。");
  } else if (candidate->support_evidence_size() == 0) {
    candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_REJECTED_BY_GATE);
    candidate->set_gate_reason("候选没有可回链的支持证据。");
  } else if (candidate->contradiction_count() > 0U) {
    candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_CONFLICTED);
    candidate->set_gate_reason("相同触发条件下存在有效步骤或必要重试的反证。");
  } else if (dialogues.size() >= required_dialogues && support_units >= required_episodes) {
    candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
    candidate->set_gate_reason("支持轨迹数量达到候选进入记忆库的质量门槛。");
  } else {
    candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_INSUFFICIENT_SUPPORT);
    candidate->set_gate_reason("支持的 Dialogue 或 Episode 数量尚未达到质量门槛。");
  }
  candidate->set_proposal_fingerprint(fingerprint(normalize_text(candidate->proposed_change())));
  candidate->set_candidate_id(candidate_id(*candidate));
}

// 把满足条件的诊断加入反思种子集合，保留其来源与作用目标。
void add_seed(std::map<std::string, ep::EvolutionCandidate> *candidates,
              ep::EvolutionCandidate seed) {
  seed.set_proposal_fingerprint(fingerprint(normalize_text(seed.proposed_change())));
  seed.set_candidate_id(candidate_id(seed));
  const std::string key = candidate_key(seed);
  auto found = candidates->find(key);
  if (found == candidates->end()) {
    candidates->emplace(key, std::move(seed));
  } else {
    merge_candidate_payload(&found->second, seed);
  }
}

// 用确定性分析建立初始候选，不依赖模型先生成完整经验。
ep::EvolutionCandidate deterministic_seed(const ep::HybridEvaluationReport &report,
                                          const ep::EpisodeAssessment &assessment,
                                          const ep::DiagnosticEvidence &diagnostic,
                                          const EvolutionOptions &options) {
  ep::EvolutionCandidate candidate;
  candidate.set_schema_version(1);
  candidate.set_generator_version(kCandidateGeneratorVersion);
  candidate.set_target_type(target_for_category(diagnostic.category()));
  candidate.set_target_scope(scope_for(assessment.evidence()));
  const std::string retryability = retryability_for(assessment.evidence(), diagnostic);
  candidate.set_retryability(retryability);
  candidate.set_error_class(diagnostic.error_class());
  candidate.set_trigger_pattern(
      "category=" + diagnostic.category() +
      ";error_class=" + (diagnostic.error_class().empty() ? "none" : diagnostic.error_class()) +
      ";retryability=" + retryability);
  candidate.set_observed_problem(bounded_text(
      diagnostic.summary().empty() ? diagnostic.observable_cause() : diagnostic.summary()));
  candidate.set_proposed_change(proposal_for_category(diagnostic.category()));
  candidate.set_expected_benefit(benefit_for_target(candidate.target_type()));
  candidate.set_required_supporting_dialogues(options.min_supporting_dialogues);
  candidate.set_required_supporting_episodes(options.min_supporting_episodes);
  const ep::CandidateEvidence evidence = make_evidence(
      ep::CANDIDATE_EVIDENCE_KIND_DETERMINISTIC_RULE, report, {assessment.evidence().episode_id()},
      {diagnostic.diagnostic_id()}, strings_from(diagnostic.evidence_event_ids()),
      ep::SEMANTIC_VERDICT_INVALID, diagnostic.confidence());
  candidate.mutable_support_evidence()->Add()->CopyFrom(evidence);
  return candidate;
}

// 把具备明确作用域的经验加入候选，旧的无作用域文本不能直接在线注入。
void add_scoped_experiences(const ep::HybridEvaluationReport &report,
                            const EvolutionOptions &options,
                            std::map<std::string, ep::EvolutionCandidate> *candidates,
                            std::uint64_t *seed_count) {
  const auto scopes = experience_scopes(report);
  const std::set<std::string> allowed_events(report.reflection().evidence_event_ids().begin(),
                                             report.reflection().evidence_event_ids().end());
  for (const auto &advice : report.reflection().experience_details()) {
    const auto target = advice.target_type() == ep::CANDIDATE_TARGET_TYPE_UNSPECIFIED
                            ? ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION
                            : advice.target_type();
    if (target != ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION &&
        target != ep::CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION &&
        target != ep::CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION)
      continue;
    if (!scopes.count(advice.target_scope()) || advice.evidence_event_ids().empty() ||
        advice.evidence_event_ids_size() > 256 ||
        !experience_fields_valid(advice.target_scope(), advice.trigger_pattern(),
                                 advice.proposed_change()) ||
        !std::all_of(advice.evidence_event_ids().begin(), advice.evidence_event_ids().end(),
                     [&](const auto &id) { return !id.empty() && allowed_events.count(id); }))
      continue;
    const std::set<std::string> cited(advice.evidence_event_ids().begin(),
                                      advice.evidence_event_ids().end());
    std::vector<std::string> episodes;
    for (const auto &assessment : report.assessments()) {
      const auto &packet = assessment.evidence();
      if (std::any_of(packet.source_event_ids().begin(), packet.source_event_ids().end(),
                      [&](const auto &id) { return cited.count(id); })) {
        episodes.push_back(packet.episode_id());
      }
    }
    ep::EvolutionCandidate candidate;
    candidate.set_schema_version(1);
    candidate.set_generator_version(kScopedExperienceGeneratorVersion);
    candidate.set_target_type(target);
    candidate.set_target_scope(advice.target_scope());
    candidate.set_trigger_pattern(advice.trigger_pattern());
    candidate.set_observed_problem(bounded_text(report.reflection().summary()));
    candidate.set_proposed_change(advice.proposed_change());
    candidate.set_expected_benefit(benefit_for_target(candidate.target_type()));
    candidate.set_required_supporting_dialogues(options.min_supporting_dialogues);
    candidate.set_required_supporting_episodes(options.min_supporting_episodes);
    // 只引用本经验自己的事件，不能引用整段对话所有浪费片段。
    *candidate.add_support_evidence() =
        make_evidence(ep::CANDIDATE_EVIDENCE_KIND_DIALOGUE_REFLECTION, report, episodes, {},
                      strings_from(advice.evidence_event_ids()), ep::SEMANTIC_VERDICT_UNASSESSED,
                      report.dialogue_assessment().confidence());
    add_seed(candidates, std::move(candidate));
    ++(*seed_count);
  }
}

// 选择当前反思能够引用的运行与事件范围，拒绝跨范围归因。
std::string reflection_scope(const ep::HybridEvaluationReport &report,
                             ep::CandidateTargetType target) {
  if (report.evaluation_kind() == "content_aware_trace_evaluation") {
    std::set<std::string> actors;
    for (const auto &item : report.assessments())
      actors.insert(scope_for(item.evidence()));
    if (actors.size() == 1U)
      return *actors.begin();
    if (!actors.empty()) {
      const auto &first = *actors.begin();
      const auto boundary = first.find("/actor:");
      if (boundary != std::string::npos)
        return first.substr(0, boundary) + "/shared";
    }
    return "agent:unknown";
  }
  if (target == ep::CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION) {
    return "coordinator";
  }
  std::set<std::string> skills;
  for (const auto &assessment : report.assessments()) {
    if (!assessment.evidence().skill_name().empty()) {
      skills.insert(assessment.evidence().skill_name());
    }
  }
  if (skills.size() == 1U) {
    return "skill:" + *skills.begin();
  }
  return "coordinator";
}

// 将反思提出的改动与已验证证据关联后加入候选集合。
void add_reflection_changes(const ep::HybridEvaluationReport &report,
                            const google::protobuf::RepeatedPtrField<std::string> &changes,
                            ep::CandidateTargetType target, const EvolutionOptions &options,
                            std::map<std::string, ep::EvolutionCandidate> *candidates,
                            std::uint64_t *seed_count) {
  // 标准轨迹的模型建议必须具备独立条件与证据；旧文本仍保留在报告供回放，
  // 不再用来源占位符生成可累计支持的经验。确定性种子不经过此入口。
  if (report.evaluation_kind() == "content_aware_trace_evaluation")
    return;
  if (std::any_of(report.reflection().experience_details().begin(),
                   report.reflection().experience_details().end(), [&](const auto &advice) {
                     return (advice.target_type() == ep::CANDIDATE_TARGET_TYPE_UNSPECIFIED
                                 ? ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION
                                 : advice.target_type()) == target;
                   }))
    return;
  std::vector<std::string> episode_ids = strings_from(report.reflection().wasted_episode_ids());
  std::vector<std::string> event_ids = strings_from(report.reflection().evidence_event_ids());
  std::string observed = report.reflection().summary();
  const std::string root_causes = joined(report.reflection().root_causes(), "；");
  if (!root_causes.empty()) {
    if (!observed.empty()) {
      observed.append("；");
    }
    observed.append(root_causes);
  }
  for (const auto &change : changes) {
    if (normalize_text(change).empty()) {
      continue;
    }
    ep::EvolutionCandidate candidate;
    candidate.set_schema_version(1);
    candidate.set_generator_version(kCandidateGeneratorVersion);
    candidate.set_target_type(target);
    candidate.set_target_scope(reflection_scope(report, target));
    candidate.set_trigger_pattern("validated_dialogue_reflection");
    candidate.set_observed_problem(bounded_text(observed));
    candidate.set_proposed_change(bounded_text(change));
    candidate.set_expected_benefit(benefit_for_target(target));
    candidate.set_required_supporting_dialogues(options.min_supporting_dialogues);
    candidate.set_required_supporting_episodes(options.min_supporting_episodes);
    const ep::CandidateEvidence evidence = make_evidence(
        ep::CANDIDATE_EVIDENCE_KIND_DIALOGUE_REFLECTION, report, episode_ids, {}, event_ids,
        episode_ids.empty() ? ep::SEMANTIC_VERDICT_UNASSESSED : ep::SEMANTIC_VERDICT_INVALID,
        report.dialogue_assessment().confidence());
    candidate.mutable_support_evidence()->Add()->CopyFrom(evidence);
    add_seed(candidates, std::move(candidate));
    ++(*seed_count);
  }
}

// 检查候选与当前运行的应用、用户和执行目标是否匹配。
bool candidate_scope_matches(const ep::EvolutionCandidate &candidate,
                             const ep::EpisodeEvidencePacket &packet) {
  return candidate.target_scope() == scope_for(packet);
}

// 将新的反证与既有经验关联，不能仅增加支持而忽略冲突。
void add_contradictions(const std::vector<ep::HybridEvaluationReport> &reports,
                        ep::EvolutionCandidate *candidate) {
  if (candidate->error_class().empty()) {
    return;
  }
  for (const auto &report : reports) {
    if (!report.source_trace_integrity_valid()) {
      continue;
    }
    for (const auto &assessment : report.assessments()) {
      if (!candidate_scope_matches(*candidate, assessment.evidence())) {
        continue;
      }
      if (assessment.final_verdict() != ep::SEMANTIC_VERDICT_USEFUL &&
          assessment.final_verdict() != ep::SEMANTIC_VERDICT_NECESSARY_RETRY) {
        continue;
      }
      bool same_error = false;
      for (const auto &tool : assessment.evidence().tools()) {
        same_error = same_error || tool.error_class() == candidate->error_class();
      }
      if (!same_error) {
        continue;
      }
      const ep::CandidateEvidence evidence = make_evidence(
          ep::CANDIDATE_EVIDENCE_KIND_CONTRADICTION, report, {assessment.evidence().episode_id()},
          {}, strings_from(assessment.evidence().source_event_ids()), assessment.final_verdict(),
          assessment.final_confidence());
      add_evidence(candidate->mutable_contradiction_evidence(), evidence);
    }
  }
}

// 绑定影响结果的配置内容，防止配置变更后误用旧缓存。
std::string config_fingerprint(const EvaluatorOptions &evaluator, const EvolutionOptions &evolution,
                               const std::shared_ptr<ModelAdapter> &adapter) {
  std::string material = std::string(kPipelineVersion) + '|' + kCandidateGeneratorVersion;
  const auto append = [&](const auto &value) {
    material.push_back('|');
    material.append(std::to_string(value));
  };
  material.append("|").append(to_string(evaluator.model_selection));
  append(evaluator.max_file_bytes);
  append(evaluator.max_model_calls);
  append(evaluator.strict_model);
  append(evaluator.allow_invalid_trace);
  append(evaluator.require_chinese_reason);
  append(evolution.max_input_files);
  append(evolution.max_candidates);
  append(evolution.min_supporting_dialogues);
  append(evolution.min_supporting_episodes);
  append(evolution.max_memory_entries);
  append(evolution.max_applied_batch_ids);
  append(evolution.max_meta_prompt_bytes);
  append(evolution.max_memory_bytes);
  append(evolution.max_memory_match_candidates);
  append(evolution.max_memory_match_prompt_bytes);
  append(evolution.max_memory_update_retries);
  append(evolution.full_curation_new_entry_threshold);
  append(evolution.min_memory_duplicate_confidence);
  append(evolution.min_memory_relation_confidence);
  append(evolution.enable_meta_reflection);
  append(evolution.enable_memory_semantic_matching);
  append(evolution.require_chinese_reason);
  const std::shared_ptr<const PromptTemplates> prompts =
      evolution.prompts != nullptr ? evolution.prompts : evaluator.prompts;
  if (prompts != nullptr) {
    for (const std::string *prompt :
         {&prompts->episode_system, &prompts->episode_user, &prompts->dialogue_system,
          &prompts->dialogue_user, &prompts->meta_reflection_system, &prompts->meta_reflection_user,
          &prompts->memory_curator_system, &prompts->memory_curator_user}) {
      material.append("|").append(fingerprint(*prompt));
    }
  } else {
    material.append("|prompts:not-loaded");
  }
  if (adapter != nullptr &&
      (evaluator.model_selection != ModelSelection::none || evolution.enable_meta_reflection)) {
    material.append("|").append(adapter->name()).append("|").append(adapter->model_id());
  }
  return fingerprint(material);
}

// 固定批次版本和来源摘要，供重复分析与重复入库检测。
void finalize_batch_identity(ep::HybridEvaluationBatchReport *report) {
  std::vector<std::string> sources;
  for (const auto &run : report->runs()) {
    sources.push_back(run.evaluation_fingerprint().empty()
                          ? run.input_fingerprint() + ':' + run.error_code() + ':' +
                                fingerprint(run.error_message())
                          : run.source_fingerprint() + ':' + run.evaluation_fingerprint());
  }
  sort_unique(&sources);

  std::vector<std::string> candidates;
  for (const auto &candidate : report->candidates()) {
    candidates.push_back(fingerprint(candidate.SerializeAsString()));
  }
  sort_unique(&candidates);

  std::string material = report->pipeline_version() + '|' + report->candidate_generator_version() +
                         '|' + report->evaluator_config_fingerprint() + '|';
  for (const auto &source : sources) {
    material.append("source=").append(source).push_back('|');
  }
  for (const auto &candidate : candidates) {
    material.append("candidate=").append(candidate).push_back('|');
  }
  material.append("meta=").append(fingerprint(report->meta_reflection().SerializeAsString()));
  report->set_batch_id("batch:" + fingerprint(material));
}

// 根据输入内容生成摘要；输入改变后必须失效旧分析或旧评测缓存。
std::string input_fingerprint(const std::filesystem::path &path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  const std::string material = error ? path.lexically_normal().generic_string()
                                     : absolute.lexically_normal().generic_string();
  return fingerprint(material);
}

// 为成功分析的运行写入状态与关联结果，和异常运行分开统计。
void fill_success_run(const ep::HybridEvaluationReport &report, ep::EvaluationRunSummary *run) {
  run->mutable_source()->CopyFrom(report.source());
  run->set_source_fingerprint(report.source_fingerprint());
  run->set_evaluation_fingerprint(fingerprint(report.SerializeAsString()));
  run->set_evaluation_available(true);
  run->set_trace_integrity_valid(report.source_trace_integrity_valid());
  run->set_model_status(report.dialogue_assessment().status());
  run->set_model_error_code(report.dialogue_assessment().error_code());
  run->set_episode_count(report.assessments_size());
  std::uint64_t invalid = 0;
  for (const auto &assessment : report.assessments()) {
    if (assessment.final_verdict() == ep::SEMANTIC_VERDICT_INVALID) {
      ++invalid;
    }
  }
  run->set_invalid_episode_count(invalid);
}

// 提取当前候选自己的证据事件，不引用整段对话所有失败事件。
std::vector<std::string> candidate_event_ids(const ep::EvolutionCandidate &candidate) {
  std::vector<std::string> output;
  for (const auto &evidence : candidate.support_evidence()) {
    for (const auto &event_id : evidence.evidence_event_ids()) {
      append_unique(&output, event_id);
    }
  }
  for (const auto &evidence : candidate.contradiction_evidence()) {
    for (const auto &event_id : evidence.evidence_event_ids()) {
      append_unique(&output, event_id);
    }
  }
  sort_unique(&output);
  return output;
}

// 构造元反思的最小输入，发送相关种子与结构信息，完整轨迹保留在本地。
ModelRequest
make_meta_request(const google::protobuf::RepeatedPtrField<ep::EvolutionCandidate> &candidates,
                  const PromptTemplates &prompts) {
  rapidjson::Document payload;
  payload.SetObject();
  auto &allocator = payload.GetAllocator();
  JsonValue seeds(rapidjson::kArrayType);
  std::vector<std::string> allowed_events;
  std::map<std::string, std::vector<std::string>> allowed_seed_groups;
  const bool common_target =
      !candidates.empty() &&
      std::all_of(candidates.begin(), candidates.end(), [&](const auto &candidate) {
        return candidate.target_type() == candidates.Get(0).target_type() &&
               candidate.target_scope() == candidates.Get(0).target_scope();
      });
  // 正常合成已按目标分组，上下文只传一次。
  if (common_target) {
    payload.AddMember(
        "target_type",
        JsonValue(ep::CandidateTargetType_Name(candidates.Get(0).target_type()).c_str(), allocator),
        allocator);
    payload.AddMember("target_scope",
                      JsonValue(candidates.Get(0).target_scope().c_str(), allocator), allocator);
  }
  for (const auto &candidate : candidates) {
    JsonValue seed(rapidjson::kObjectType);
    seed.AddMember("seed_id", JsonValue(candidate.candidate_id().c_str(), allocator), allocator);
    const std::string target = ep::CandidateTargetType_Name(candidate.target_type());
    if (!common_target) {
      seed.AddMember("target_type", JsonValue(target.c_str(), allocator), allocator);
      seed.AddMember("target_scope", JsonValue(candidate.target_scope().c_str(), allocator),
                     allocator);
    }
    const std::string compatibility_group =
        (is_scoped_experience(candidate) ? "scoped-experience-v1:" : "") + target + ":" +
        candidate.target_scope();
    allowed_seed_groups[compatibility_group].push_back(candidate.candidate_id());
    seed.AddMember("trigger_pattern", JsonValue(candidate.trigger_pattern().c_str(), allocator),
                   allocator);
    seed.AddMember("observed_problem", JsonValue(candidate.observed_problem().c_str(), allocator),
                   allocator);
    seed.AddMember("proposed_change", JsonValue(candidate.proposed_change().c_str(), allocator),
                   allocator);
    // 支持门槛和来源在本地管理，模型只负责内容归并。
    seed.AddMember("contradictions", candidate.contradiction_count(), allocator);
    for (const auto &event_id : candidate_event_ids(candidate)) {
      append_unique(&allowed_events, event_id);
    }
    seeds.PushBack(seed, allocator);
  }
  payload.AddMember("candidate_seeds", seeds, allocator);
  JsonValue compatibility_groups(rapidjson::kArrayType);
  for (const auto &[group_id, seed_ids] : allowed_seed_groups) {
    JsonValue group(rapidjson::kObjectType);
    group.AddMember("compatibility_group", JsonValue(group_id.c_str(), allocator), allocator);
    JsonValue ids(rapidjson::kArrayType);
    for (const auto &seed_id : seed_ids) {
      ids.PushBack(JsonValue(seed_id.c_str(), allocator), allocator);
    }
    group.AddMember("seed_ids", ids, allocator);
    compatibility_groups.PushBack(group, allocator);
  }
  payload.AddMember("allowed_seed_groups", compatibility_groups, allocator);

  ModelRequest request;
  request.system_prompt = prompts.meta_reflection_system;
  request.user_prompt = render_prompt_template("meta_reflection_user", prompts.meta_reflection_user,
                                               {{"candidate_json", serialize_json(payload)}},
                                               std::numeric_limits<std::size_t>::max());
  sort_unique(&allowed_events);
  request.allowed_event_ids = std::move(allowed_events);
  std::string material = request.system_prompt;
  material.push_back('\0');
  material.append(request.user_prompt);
  request.packet_id = "meta:" + fingerprint(material);
  return request;
}

// 验证元反思文本的类型和长度，拒绝超出契约的内容。
bool bounded_meta_string(const JsonValue &object, const char *name, bool require_chinese,
                         std::string *output) {
  const JsonValue *value = find_member(object, name);
  if (value == nullptr || !value->IsString() || value->GetStringLength() == 0U ||
      value->GetStringLength() > kMaxCandidateTextBytes) {
    return false;
  }
  *output = std::string(value->GetString(), value->GetStringLength());
  return !require_chinese || has_cjk(*output);
}

struct MetaValidation {
  bool valid = false;
  std::string error_code;
  std::vector<ep::EvolutionCandidate> candidates;
  std::unordered_set<std::string> used_seed_ids;
};

// 检查元反思的目标、来源和结构，模型不得引用未提供的种子。
MetaValidation
validate_meta_response(const RawModelResponse &response,
                       const google::protobuf::RepeatedPtrField<ep::EvolutionCandidate> &seeds,
                       const ModelRequest &request, const std::vector<std::string> &secrets,
                       bool require_chinese) {
  MetaValidation output;
  if (!response.transport_ok || !response.error_code.empty()) {
    output.error_code = response.error_code.empty() ? "transport_error" : response.error_code;
    return output;
  }
  if (response.body.size() > 256U * 1024U) {
    output.error_code = "response_too_large";
    return output;
  }
  if (contains_secret(response.body, secrets)) {
    output.error_code = "credential_in_response";
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
  const JsonValue *groups = find_member(document, "candidate_groups");
  if (groups == nullptr || !groups->IsArray() || groups->Size() > kMaxMetaGroups) {
    output.error_code = "invalid_candidate_groups";
    return output;
  }
  std::unordered_map<std::string, const ep::EvolutionCandidate *> seed_by_id;
  for (const auto &seed : seeds) {
    seed_by_id.emplace(seed.candidate_id(), &seed);
  }
  const std::unordered_set<std::string> allowed_events(request.allowed_event_ids.begin(),
                                                       request.allowed_event_ids.end());

  for (const auto &group : groups->GetArray()) {
    const JsonValue *seed_ids = find_member(group, "seed_ids");
    const JsonValue *evidence_ids = find_member(group, "evidence_event_ids");
    const JsonValue *confidence = find_member(group, "confidence");
    // 分别报告字段错误，避免把类型、空数组和置信度越界混成一个无法排查的错误码。
    if (!group.IsObject()) {
      output.error_code = "invalid_candidate_group_object";
      return output;
    }
    if (seed_ids == nullptr || !seed_ids->IsArray() || seed_ids->Empty() ||
        seed_ids->Size() > kMaxMetaSeedsPerGroup) {
      output.error_code = "invalid_candidate_seed_ids";
      return output;
    }
    if (evidence_ids != nullptr &&
        (!evidence_ids->IsArray() || evidence_ids->Empty() || evidence_ids->Size() > 256U)) {
      output.error_code = "invalid_candidate_evidence_event_ids";
      return output;
    }
    if (confidence == nullptr || !confidence->IsNumber() ||
        !std::isfinite(confidence->GetDouble()) || confidence->GetDouble() < 0.0 ||
        confidence->GetDouble() > 1.0) {
      output.error_code = "invalid_candidate_confidence";
      return output;
    }

    std::vector<const ep::EvolutionCandidate *> selected;
    for (const auto &value : seed_ids->GetArray()) {
      if (!value.IsString()) {
        output.error_code = "invalid_seed_id";
        return output;
      }
      const std::string id(value.GetString(), value.GetStringLength());
      const auto found = seed_by_id.find(id);
      if (found == seed_by_id.end() || !output.used_seed_ids.insert(id).second) {
        output.error_code = "unknown_or_reused_seed_id";
        return output;
      }
      selected.push_back(found->second);
    }
    const auto target = selected.front()->target_type();
    const std::string scope = selected.front()->target_scope();
    if (!std::all_of(selected.begin(), selected.end(), [&](const auto *seed) {
          return seed->target_type() == target && seed->target_scope() == scope &&
                 same_experience_contract(*seed, *selected.front());
        })) {
      output.error_code = "incompatible_seed_group";
      return output;
    }

    std::string trigger;
    std::string observed;
    std::string proposal;
    std::string benefit;
    for (const auto &[field, value] :
         std::vector<std::pair<const char *, std::string *>>{{"trigger_pattern", &trigger},
                                                           {"observed_problem", &observed},
                                                           {"proposed_change", &proposal},
                                                           {"expected_benefit", &benefit}}) {
      if (!bounded_meta_string(group, field, require_chinese, value)) {
        output.error_code = std::string("invalid_candidate_text_") + field;
        return output;
      }
    }
    if (is_scoped_experience(*selected.front()) &&
        !experience_fields_valid(scope, trigger, proposal)) {
      output.error_code = "invalid_experience_contract";
      return output;
    }

    std::unordered_set<std::string> selected_events;
    for (const auto *seed : selected) {
      for (const auto &event_id : candidate_event_ids(*seed)) {
        selected_events.insert(event_id);
      }
    }
    // 证据继承自下方选中的种子；旧版模型响应允许引用子集，
    // 但这些可选引用同样必须严格校验。
    if (evidence_ids != nullptr)
      for (const auto &value : evidence_ids->GetArray()) {
        if (!value.IsString()) {
          output.error_code = "invalid_evidence_event_id";
          return output;
        }
        const std::string id(value.GetString(), value.GetStringLength());
        if (allowed_events.find(id) == allowed_events.end() ||
            selected_events.find(id) == selected_events.end()) {
          output.error_code = "unknown_evidence_event_id";
          return output;
        }
      }

    ep::EvolutionCandidate candidate;
    candidate.CopyFrom(*selected.front());
    candidate.clear_support_evidence();
    candidate.clear_contradiction_evidence();
    candidate.clear_source_seed_ids();
    for (const auto *seed : selected) {
      merge_candidate_payload(&candidate, *seed);
      append_unique(candidate.mutable_source_seed_ids(), seed->candidate_id());
    }
    // 元反思选择代表经验、归并支持，不扩大已核验的适用条件或改写动作。
    // 原始轨迹分析负责生成经验；合并阶段不能把两种业务决策拼成一条新规则。
    const auto *canonical = selected.front();
    const auto *canonical_id = find_member(group, "canonical_seed_id");
    if (canonical_id != nullptr) {
      if (!canonical_id->IsString()) {
        output.error_code = "invalid_canonical_seed_id";
        return output;
      }
      const std::string id(canonical_id->GetString(), canonical_id->GetStringLength());
      const auto found = std::find_if(selected.begin(), selected.end(), [&](const auto *seed) {
        return seed->candidate_id() == id;
      });
      if (found == selected.end()) {
        output.error_code = "unknown_canonical_seed_id";
        return output;
      }
      canonical = *found;
    }
    candidate.set_trigger_pattern(canonical->trigger_pattern());
    candidate.set_observed_problem(canonical->observed_problem());
    candidate.set_proposed_change(canonical->proposed_change());
    candidate.set_expected_benefit(canonical->expected_benefit());
    candidate.set_meta_reflection_applied(true);
    candidate.set_confidence(confidence->GetDouble());
    finalize_candidate(&candidate);
    candidate.set_confidence(std::max(candidate.confidence(), confidence->GetDouble()));
    output.candidates.push_back(std::move(candidate));
  }
  output.valid = true;
  return output;
}

// 按稳定候选身份合并反思结果，避免一条经验重复进入下游。
std::vector<ep::EvolutionCandidate>
merge_by_candidate_id(std::vector<ep::EvolutionCandidate> candidates) {
  std::map<std::string, ep::EvolutionCandidate> merged;
  for (auto &candidate : candidates) {
    finalize_candidate(&candidate);
    const std::string id = candidate.candidate_id();
    auto found = merged.find(id);
    if (found == merged.end()) {
      merged.emplace(id, std::move(candidate));
    } else {
      merge_candidate_payload(&found->second, candidate);
      finalize_candidate(&found->second);
    }
  }
  std::vector<ep::EvolutionCandidate> output;
  output.reserve(merged.size());
  for (auto &item : merged) {
    output.push_back(std::move(item.second));
  }
  return output;
}

// 根据实际合成结果刷新统计，区分生成、合并和拒绝的候选。
void refresh_synthesis_metrics(ep::HybridEvaluationBatchReport *batch) {
  auto *metrics = batch->mutable_synthesis();
  metrics->set_candidate_count(batch->candidates_size());
  metrics->set_proposed_count(0);
  metrics->set_conflicted_count(0);
  metrics->set_insufficient_support_count(0);
  metrics->set_rejected_count(0);
  for (const auto &candidate : batch->candidates()) {
    switch (candidate.status()) {
    case ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED:
      metrics->set_proposed_count(metrics->proposed_count() + 1U);
      break;
    case ep::EVOLUTION_CANDIDATE_STATUS_CONFLICTED:
      metrics->set_conflicted_count(metrics->conflicted_count() + 1U);
      break;
    case ep::EVOLUTION_CANDIDATE_STATUS_INSUFFICIENT_SUPPORT:
      metrics->set_insufficient_support_count(metrics->insufficient_support_count() + 1U);
      break;
    case ep::EVOLUTION_CANDIDATE_STATUS_REJECTED_BY_GATE:
      metrics->set_rejected_count(metrics->rejected_count() + 1U);
      break;
    default:
      break;
    }
  }
}

// 先写临时文件再替换目标，避免进程退出后留下半份有效格式的结果。
void write_bytes_atomic(const std::filesystem::path &path, const std::string &content) {
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
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path temporary = path.string() + ".tmp." + std::to_string(nonce);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw EvaluationError("cannot open temporary output file");
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output.flush()) {
      std::filesystem::remove(temporary, error);
      throw EvaluationError("failed to write temporary output file");
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    throw EvaluationError("cannot atomically replace output file");
  }
}

class MemoryFileLock {
public:
  // 初始化 MemoryFileLock 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit MemoryFileLock(const std::filesystem::path &memory_path) {
#if defined(__unix__) || defined(__APPLE__)
    const std::filesystem::path lock_path = memory_path.string() + ".lock";
    std::error_code error;
    if (!lock_path.parent_path().empty()) {
      std::filesystem::create_directories(lock_path.parent_path(), error);
      if (error) {
        throw EvaluationError("cannot create memory lock directory");
      }
    }
    descriptor_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX) != 0) {
      if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
      }
      throw EvaluationError("cannot acquire evolution memory lock");
    }
#else
    (void)memory_path;
#endif
  }

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~MemoryFileLock() {
#if defined(__unix__) || defined(__APPLE__)
    if (descriptor_ >= 0) {
      (void)::flock(descriptor_, LOCK_UN);
      (void)::close(descriptor_);
    }
#endif
  }

  MemoryFileLock(const MemoryFileLock &) = delete;
  MemoryFileLock &operator=(const MemoryFileLock &) = delete;

private:
#if defined(__unix__) || defined(__APPLE__)
  int descriptor_ = -1;
#endif
};

struct MemoryShortlistItem {
  int entry_index = -1;
  double lexical_score = 0.0;
  std::string content_fingerprint;
};

struct ParsedMemoryMatch {
  bool valid = false;
  ep::MemoryMatchRelation relation = ep::MEMORY_MATCH_RELATION_UNSPECIFIED;
  std::string memory_id;
  double confidence = 0.0;
  std::string reason;
  std::string error_code;
};

struct MemoryCurationContext {
  const EvolutionOptions *options = nullptr;
  std::shared_ptr<ModelAdapter> adapter;
  std::size_t remaining_model_calls = 0;
  std::vector<ModelRequest> *model_requests = nullptr;
  std::unordered_map<std::string, RawModelResponse> *response_cache = nullptr;
  std::vector<std::string> secrets;
  std::size_t max_memory_bytes = 0;
  bool model_rate_limited = false;
  // 用户库旁的独立检查点，只保存经过验证的判断，不保存模型原文或源轨迹。
  ::trajectory::evolution::integration::Json *journal = nullptr;
  std::function<void()> checkpoint;
};

using ManagementJson = ::trajectory::evolution::integration::Json;

// 墙上时间供重启后退避续跑；耗时统计另外使用单调时钟。
std::size_t management_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

// 缓存归属当前库目录，并绑定模型、提示词和判定门槛；支持次数和来源别名不在键中。
std::string memory_review_key(const ModelRequest &request, const MemoryCurationContext &context) {
  return fingerprint(request.packet_id + '\0' + context.adapter->name() + '\0' +
      context.adapter->model_id() + '\0' + kMemoryCuratorVersion + '\0' +
      std::to_string(context.options->require_chinese_reason) + ':' +
      std::to_string(context.options->min_memory_duplicate_confidence) + ':' +
      std::to_string(context.options->min_memory_relation_confidence));
}

// 检查点是派生状态，但损坏时也不能静默忽略后重花模型预算。
ManagementJson load_curation_journal(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path))
    return ManagementJson{{"schema_version", 1}, {"records", ManagementJson::Object{}},
                          {"subjects", ManagementJson::Array{}}, {"active", false}};
  if (std::filesystem::file_size(path) > 32U * 1024U * 1024U)
    throw EvaluationError("memory curation journal exceeds limit");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw EvaluationError("cannot read memory curation journal");
  auto state = ManagementJson::parse(std::string(std::istreambuf_iterator<char>(input), {}));
  if (state.num("schema_version", 0) != 1 || !state["records"].object() || !state["subjects"].array())
    throw EvaluationError("invalid memory curation journal");
  return state;
}

// 计算候选内容摘要，供变更检测与幂等入库使用。
std::string candidate_content_fingerprint(const ep::EvolutionCandidate &candidate) {
  return fingerprint(candidate.SerializeAsString());
}

// 组合经验作用域、目标与规范内容，形成跨批次比较的语义身份。
std::string semantic_identity_key(const ep::EvolutionCandidate &candidate) {
  return std::string(is_scoped_experience(candidate) ? "scoped-experience-v1|" : "") +
         std::to_string(static_cast<int>(candidate.target_type())) + '|' +
         normalize_text(candidate.target_scope()) + '|' +
         normalize_text(candidate.trigger_pattern()) + '|' +
         normalize_text(candidate.proposed_change());
}

// 构造轻量字节片段集合，供词面召回使用，不当作完整语义判断。
std::set<std::string> byte_ngrams(const std::string &value) {
  const std::string normalized = normalize_text(value);
  std::set<std::string> output;
  if (normalized.empty()) {
    return output;
  }
  constexpr std::size_t width = 3;
  if (normalized.size() <= width) {
    output.insert(normalized);
    return output;
  }
  for (std::size_t index = 0; index + width <= normalized.size(); ++index) {
    output.insert(normalized.substr(index, width));
  }
  return output;
}

// 计算轻量词面相似度用于候选召回，不将其等同于完整语义判定。
double lexical_similarity(const ep::EvolutionCandidate &left, const ep::EvolutionCandidate &right) {
  const std::string left_text =
      left.trigger_pattern() + '\n' + left.observed_problem() + '\n' + left.proposed_change();
  const std::string right_text =
      right.trigger_pattern() + '\n' + right.observed_problem() + '\n' + right.proposed_change();
  const auto left_grams = byte_ngrams(left_text);
  const auto right_grams = byte_ngrams(right_text);
  if (left_grams.empty() || right_grams.empty()) {
    return 0.0;
  }
  std::size_t intersection = 0;
  auto left_it = left_grams.begin();
  auto right_it = right_grams.begin();
  while (left_it != left_grams.end() && right_it != right_grams.end()) {
    if (*left_it < *right_it) {
      ++left_it;
    } else if (*right_it < *left_it) {
      ++right_it;
    } else {
      ++intersection;
      ++left_it;
      ++right_it;
    }
  }
  return 2.0 * static_cast<double>(intersection) /
         static_cast<double>(left_grams.size() + right_grams.size());
}

// 检查记忆的目标和作用条件是否兼容，防止相似文字跨用途合并。
bool structurally_compatible(const ep::EvolutionCandidate &incoming,
                             const ep::EvolutionCandidate &existing) {
  return incoming.target_type() == existing.target_type() &&
         same_experience_contract(incoming, existing) &&
         normalize_text(incoming.target_scope()) == normalize_text(existing.target_scope());
}

// 校验候选身份和作用域，避免将来源不完整的经验写入正式记忆。
bool candidate_identity_valid(const ep::EvolutionCandidate &candidate) {
  if (candidate.candidate_id().empty()) {
    return false;
  }
  ep::EvolutionCandidate normalized = candidate;
  normalized.set_proposal_fingerprint(fingerprint(normalize_text(normalized.proposed_change())));
  return candidate.proposal_fingerprint() == normalized.proposal_fingerprint() &&
         candidate.candidate_id() == candidate_id(normalized);
}

// 将记忆关系按稳定顺序排列，便于比较、序列化和增量管理。
void sort_relations(google::protobuf::RepeatedPtrField<ep::EvolutionMemoryRelation> *relations) {
  std::vector<ep::EvolutionMemoryRelation> values(relations->begin(), relations->end());
  std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) {
    if (left.target_memory_id() != right.target_memory_id()) {
      return left.target_memory_id() < right.target_memory_id();
    }
    return left.relation() < right.relation();
  });
  relations->Clear();
  for (const auto &value : values) {
    relations->Add()->CopyFrom(value);
  }
}

// 统一记忆条目的字段与状态表示，兼容可迁移的旧格式并保留来源证据。
void normalize_memory_entry(ep::EvolutionMemoryEntry *entry) {
  append_unique(entry->mutable_source_candidate_ids(), entry->candidate().candidate_id());
  std::vector<std::string> sources = strings_from(entry->source_candidate_ids());
  set_strings(entry->mutable_source_candidate_ids(), std::move(sources));
  std::vector<std::string> batches = strings_from(entry->observed_batch_ids());
  set_strings(entry->mutable_observed_batch_ids(), std::move(batches));
  sort_relations(entry->mutable_relations());
}

// 建立记忆库的版本与管理状态，已有内容不能被无条件清空。
void initialize_memory_store(ep::EvolutionMemoryStore *memory) {
  if (memory->schema_version() == 0U) {
    memory->set_schema_version(1);
    memory->set_store_kind("trajectory_evolution_memory");
    memory->set_store_version(kMemoryStoreVersion);
  }
  if (memory->schema_version() != 1U || memory->store_kind() != "trajectory_evolution_memory" ||
      memory->store_version() != kMemoryStoreVersion) {
    throw EvaluationError("unsupported evolution memory schema or version");
  }
  std::unordered_set<std::string> memory_ids;
  std::unordered_map<std::string, std::string> source_owners;
  for (auto &entry : *memory->mutable_entries()) {
    if (entry.memory_id().empty() || !candidate_identity_valid(entry.candidate()) ||
        entry.memory_id() != "memory:" + entry.candidate().candidate_id() ||
        !memory_ids.insert(entry.memory_id()).second) {
      throw EvaluationError("invalid or duplicate evolution memory entry");
    }
    normalize_memory_entry(&entry);
    for (const auto &source_id : entry.source_candidate_ids()) {
      const auto inserted = source_owners.emplace(source_id, entry.memory_id());
      if (!inserted.second && inserted.first->second != entry.memory_id()) {
        throw EvaluationError("candidate is mapped to multiple memory entries");
      }
    }
  }
  for (const auto &entry : memory->entries()) {
    for (const auto &relation : entry.relations()) {
      if (relation.target_memory_id() == entry.memory_id() ||
          memory_ids.find(relation.target_memory_id()) == memory_ids.end() ||
          (relation.relation() != ep::MEMORY_MATCH_RELATION_RELATED &&
           relation.relation() != ep::MEMORY_MATCH_RELATION_CONTRADICTS)) {
        throw EvaluationError("invalid evolution memory relation");
      }
    }
  }
}

// 按记忆身份建立查询索引，供重复匹配、关联与冲突检查复用。
void build_memory_indices(const ep::EvolutionMemoryStore &memory,
                          std::unordered_map<std::string, int> *entry_by_id,
                          std::unordered_map<std::string, int> *entry_by_candidate_id) {
  entry_by_id->clear();
  entry_by_candidate_id->clear();
  for (int index = 0; index < memory.entries_size(); ++index) {
    const auto &entry = memory.entries(index);
    entry_by_id->emplace(entry.memory_id(), index);
    for (const auto &source_id : entry.source_candidate_ids()) {
      entry_by_candidate_id->emplace(source_id, index);
    }
  }
}

// 保存一次修订对应的内容与状态快照，为回溯和关系失效提供依据。
void fill_revision_snapshot(ep::EvolutionMemoryRevision *snapshot,
                            const ep::EvolutionMemoryEntry &entry, std::uint64_t revision,
                            const std::string &batch_id) {
  snapshot->Clear();
  snapshot->set_store_revision(revision);
  snapshot->set_batch_id(batch_id);
  snapshot->mutable_candidate_snapshot()->CopyFrom(entry.candidate());
}

// 记录记忆内容或状态变化前后的修订信息，保留可追溯的演化历史。
void record_revision(ep::EvolutionMemoryEntry *entry, std::uint64_t revision,
                     const std::string &batch_id) {
  ep::EvolutionMemoryRevision *snapshot = nullptr;
  if (entry->revisions_size() > 0 &&
      entry->revisions(entry->revisions_size() - 1).store_revision() == revision &&
      entry->revisions(entry->revisions_size() - 1).batch_id() == batch_id) {
    snapshot = entry->mutable_revisions(entry->revisions_size() - 1);
  } else {
    snapshot = entry->add_revisions();
  }
  fill_revision_snapshot(snapshot, *entry, revision, batch_id);
}

// 新增或更新一对记忆的关系，避免重复边和互相矛盾的关系记录。
bool upsert_relation(ep::EvolutionMemoryEntry *entry, const std::string &target_memory_id,
                     ep::MemoryMatchRelation relation, double confidence,
                     const std::string &decision_id) {
  ep::EvolutionMemoryRelation *destination = nullptr;
  for (auto &existing : *entry->mutable_relations()) {
    if (existing.target_memory_id() == target_memory_id) {
      destination = &existing;
      break;
    }
  }
  const std::string before =
      destination == nullptr ? std::string() : destination->SerializeAsString();
  if (destination == nullptr) {
    destination = entry->add_relations();
    destination->set_target_memory_id(target_memory_id);
  }
  const bool preserves_conflict =
      destination->relation() == ep::MEMORY_MATCH_RELATION_CONTRADICTS &&
      relation != ep::MEMORY_MATCH_RELATION_CONTRADICTS;
  const bool promotes_conflict = relation == ep::MEMORY_MATCH_RELATION_CONTRADICTS &&
                                 destination->relation() != ep::MEMORY_MATCH_RELATION_CONTRADICTS;
  if (destination->relation() == ep::MEMORY_MATCH_RELATION_UNSPECIFIED || promotes_conflict ||
      (!preserves_conflict && confidence >= destination->confidence())) {
    destination->set_relation(relation);
    destination->set_confidence(std::clamp(confidence, 0.0, 1.0));
    destination->set_decision_id(decision_id);
  }
  sort_relations(entry->mutable_relations());
  for (const auto &existing : entry->relations()) {
    if (existing.target_memory_id() == target_memory_id) {
      return before != existing.SerializeAsString();
    }
  }
  return true;
}

// 判断条目是否属于当前可管理范围，隔离无效或不可用的记忆。
bool is_manageable_memory(const ep::EvolutionMemoryEntry &entry) {
  return entry.status() == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE ||
         entry.status() == ep::EVOLUTION_MEMORY_STATUS_VALIDATED;
}

// 先筛选结构与内容可能相关的记忆，避免每个新候选都请求模型扫描全库。
std::vector<MemoryShortlistItem> memory_shortlist(const ep::EvolutionCandidate &incoming,
                                                  const ep::EvolutionMemoryStore &memory,
                                                  std::size_t limit,
                                                  std::string_view excluded_memory_id = {}) {
  std::vector<MemoryShortlistItem> output;
  for (int index = 0; index < memory.entries_size(); ++index) {
    const auto &entry = memory.entries(index);
    if (entry.memory_id() == excluded_memory_id || !is_manageable_memory(entry) ||
        (incoming.target_type() == entry.candidate().target_type() &&
         !same_experience_contract(incoming, entry.candidate())) ||
        normalize_text(incoming.target_scope()) != normalize_text(entry.candidate().target_scope())) {
      continue;
    }
    output.push_back({
        index,
        lexical_similarity(incoming, entry.candidate()),
        candidate_content_fingerprint(entry.candidate()),
    });
  }
  std::sort(output.begin(), output.end(), [&](const auto &left, const auto &right) {
    if (left.lexical_score != right.lexical_score) {
      return left.lexical_score > right.lexical_score;
    }
    return memory.entries(left.entry_index).memory_id() <
           memory.entries(right.entry_index).memory_id();
  });
  if (output.size() > limit) {
    output.resize(limit);
  }
  return output;
}

// 记录本次匹配检查过的候选范围，使入库决定可解释和复查。
void add_shortlist_to_decision(const std::vector<MemoryShortlistItem> &shortlist,
                               const ep::EvolutionMemoryStore &memory,
                               ep::MemoryUpdateDecision *decision) {
  for (const auto &item : shortlist) {
    const auto &entry = memory.entries(item.entry_index);
    auto *option = decision->add_considered_matches();
    option->set_memory_id(entry.memory_id());
    option->set_lexical_score(item.lexical_score);
    option->set_content_fingerprint(item.content_fingerprint);
    option->set_memory_status(entry.status());
  }
}

// 构建候选经验的最小比较摘要，不重复发送来源轨迹全文。
JsonValue candidate_summary_json(const ep::EvolutionCandidate &candidate, Allocator &allocator) {
  JsonValue output(rapidjson::kObjectType);
  const std::string target = ep::CandidateTargetType_Name(candidate.target_type());
  output.AddMember("target_type", JsonValue(target.c_str(), allocator), allocator);
  output.AddMember("target_scope", JsonValue(candidate.target_scope().c_str(), allocator),
                   allocator);
  output.AddMember("trigger_pattern", JsonValue(candidate.trigger_pattern().c_str(), allocator),
                   allocator);
  output.AddMember("observed_problem", JsonValue(candidate.observed_problem().c_str(), allocator),
                   allocator);
  output.AddMember("proposed_change", JsonValue(candidate.proposed_change().c_str(), allocator),
                   allocator);
  output.AddMember("scoped_contract", candidate.generator_version() == kScopedExperienceGeneratorVersion, allocator);
  if (!candidate.error_class().empty())
    output.AddMember("error_class", JsonValue(candidate.error_class().c_str(), allocator),
                     allocator);
  if (!candidate.retryability().empty())
    output.AddMember("retryability", JsonValue(candidate.retryability().c_str(), allocator),
                     allocator);
  return output;
}

// 工作项是否完成只绑定该条目自己的语义，不因别的条目合并导致短名单换位而清零。
std::string memory_subject_fingerprint(const ep::EvolutionCandidate &candidate) {
  rapidjson::Document document(rapidjson::kObjectType);
  return fingerprint(serialize_json(candidate_summary_json(candidate, document.GetAllocator())));
}

// 把新候选和候选匹配集交给记忆裁判，模型只判断内容关系。
ModelRequest make_memory_match_request(const ep::EvolutionCandidate &incoming,
                                       const ep::EvolutionMemoryStore &memory,
                                       const std::vector<MemoryShortlistItem> &shortlist,
                                       const PromptTemplates &prompts) {
  rapidjson::Document payload(rapidjson::kObjectType);
  auto &allocator = payload.GetAllocator();
  payload.AddMember("incoming_candidate", candidate_summary_json(incoming, allocator), allocator);
  JsonValue options(rapidjson::kArrayType);
  for (const auto &item : shortlist) {
    const auto &entry = memory.entries(item.entry_index);
    JsonValue option(rapidjson::kObjectType);
    option.AddMember("memory_id", JsonValue(entry.memory_id().c_str(), allocator), allocator);
    option.AddMember("candidate", candidate_summary_json(entry.candidate(), allocator), allocator);
    // 候选中仍保留跨类型项以检查冲突，但明确逐项声明允许的关系，避免模型误合并。
    JsonValue allowed(rapidjson::kArrayType);
    if (structurally_compatible(incoming, entry.candidate()))
      allowed.PushBack(JsonValue("DUPLICATE", allocator), allocator);
    for (const auto *relation : {"RELATED", "CONTRADICTS"})
      allowed.PushBack(JsonValue(relation, allocator), allocator);
    option.AddMember("allowed_relations", allowed, allocator);
    options.PushBack(option, allocator);
  }
  payload.AddMember("memory_options", options, allocator);

  ModelRequest request;
  request.system_prompt = prompts.memory_curator_system;
  request.user_prompt = render_prompt_template("memory_curator_user", prompts.memory_curator_user,
                                               {{"memory_json", serialize_json(payload)}},
                                               std::numeric_limits<std::size_t>::max());
  std::string material = request.system_prompt;
  material.push_back('\0');
  material.append(request.user_prompt);
  request.packet_id = "memory-curation:" + fingerprint(material);
  return request;
}

// 校验重复、相关、冲突或独立的模型结论及引用范围，拒绝越界记忆标识。
ParsedMemoryMatch validate_memory_match_response(const RawModelResponse &response,
                                                 const std::vector<MemoryShortlistItem> &shortlist,
                                                 const ep::EvolutionMemoryStore &memory,
                                                 const std::vector<std::string> &secrets,
                                                 bool require_chinese) {
  ParsedMemoryMatch output;
  if (!response.transport_ok || !response.error_code.empty()) {
    output.error_code = response.error_code.empty() ? "transport_error" : response.error_code;
    return output;
  }
  if (response.body.size() > 256U * 1024U) {
    output.error_code = "response_too_large";
    return output;
  }
  if (contains_secret(response.body, secrets)) {
    output.error_code = "credential_in_response";
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
  const JsonValue *relation = find_member(document, "relation");
  const JsonValue *memory_id = find_member(document, "memory_id");
  const JsonValue *confidence = find_member(document, "confidence");
  const JsonValue *reason = find_member(document, "reason");
  if (relation == nullptr || !relation->IsString() || memory_id == nullptr ||
      !memory_id->IsString() || confidence == nullptr || !confidence->IsNumber() ||
      !std::isfinite(confidence->GetDouble()) || confidence->GetDouble() < 0.0 ||
      confidence->GetDouble() > 1.0 || reason == nullptr || !reason->IsString() ||
      reason->GetStringLength() == 0U || reason->GetStringLength() > kMaxCandidateTextBytes) {
    output.error_code = "invalid_memory_match_fields";
    return output;
  }
  const std::string relation_name(relation->GetString(), relation->GetStringLength());
  if (relation_name == "DUPLICATE") {
    output.relation = ep::MEMORY_MATCH_RELATION_DUPLICATE;
  } else if (relation_name == "RELATED") {
    output.relation = ep::MEMORY_MATCH_RELATION_RELATED;
  } else if (relation_name == "CONTRADICTS") {
    output.relation = ep::MEMORY_MATCH_RELATION_CONTRADICTS;
  } else if (relation_name == "DISTINCT") {
    output.relation = ep::MEMORY_MATCH_RELATION_DISTINCT;
  } else if (relation_name == "UNRESOLVED") {
    output.relation = ep::MEMORY_MATCH_RELATION_UNRESOLVED;
  } else {
    output.error_code = "invalid_memory_match_relation";
    return output;
  }
  output.memory_id.assign(memory_id->GetString(), memory_id->GetStringLength());
  std::unordered_set<std::string> allowed_ids;
  for (const auto &item : shortlist) {
    allowed_ids.insert(memory.entries(item.entry_index).memory_id());
  }
  const bool without_match = output.relation == ep::MEMORY_MATCH_RELATION_DISTINCT ||
                             output.relation == ep::MEMORY_MATCH_RELATION_UNRESOLVED;
  if ((without_match && !output.memory_id.empty()) ||
      (!without_match &&
       allowed_ids.find(output.memory_id) == allowed_ids.end())) {
    output.error_code = "unknown_or_incompatible_memory_id";
    return output;
  }
  output.confidence = confidence->GetDouble();
  output.reason.assign(reason->GetString(), reason->GetStringLength());
  if (require_chinese && !has_cjk(output.reason)) {
    output.error_code = "memory_match_reason_not_chinese";
    return output;
  }
  output.valid = true;
  return output;
}

} // 结束当前匿名命名空间。

// 初始化 TrajectoryEvolutionPipeline 的状态和资源；输入参数的校验与所有权在构造阶段确定。
// 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
TrajectoryEvolutionPipeline::TrajectoryEvolutionPipeline(
    EvaluatorOptions evaluator_options, EvolutionOptions evolution_options,
    std::shared_ptr<ModelAdapter> model_adapter)
    : evaluator_options_(std::move(evaluator_options)),
      evolution_options_(std::move(evolution_options)), model_adapter_(std::move(model_adapter)) {
  if (model_adapter_ != nullptr) {
    if (evaluator_options_.prompts == nullptr && evolution_options_.prompts == nullptr) {
      evaluator_options_.prompts = load_default_prompt_templates();
      evolution_options_.prompts = evaluator_options_.prompts;
    } else if (evaluator_options_.prompts == nullptr) {
      evaluator_options_.prompts = evolution_options_.prompts;
    } else if (evolution_options_.prompts == nullptr) {
      evolution_options_.prompts = evaluator_options_.prompts;
    }
  }
  if (evolution_options_.max_input_files == 0U || evolution_options_.max_candidates == 0U ||
      evolution_options_.min_supporting_dialogues == 0U ||
      evolution_options_.min_supporting_episodes == 0U ||
      evolution_options_.max_memory_bytes == 0U || evolution_options_.max_memory_entries == 0U ||
      evolution_options_.max_applied_batch_ids == 0U ||
      evolution_options_.max_memory_match_candidates == 0U ||
      evolution_options_.max_memory_update_retries == 0U ||
      evolution_options_.max_memory_match_prompt_bytes == 0U ||
      evolution_options_.full_curation_new_entry_threshold == 0U) {
    throw EvaluationError("evolution limits must be greater than zero");
  }
  if (!std::isfinite(evolution_options_.min_memory_duplicate_confidence) ||
      !std::isfinite(evolution_options_.min_memory_relation_confidence) ||
      evolution_options_.min_memory_duplicate_confidence < 0.0 ||
      evolution_options_.min_memory_duplicate_confidence > 1.0 ||
      evolution_options_.min_memory_relation_confidence < 0.0 ||
      evolution_options_.min_memory_relation_confidence > 1.0) {
    throw EvaluationError("memory curation confidence must be in [0, 1]");
  }
}

// 遍历指定目录内有效输入，逐份评测并保留独立失败状态。
EvolutionBatchOutput
TrajectoryEvolutionPipeline::evaluate_directory(const std::filesystem::path &root) const {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    throw EvaluationError("SessionLog input root is not a readable directory");
  }
  std::vector<std::filesystem::path> paths;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    throw EvaluationError("cannot traverse SessionLog input root");
  }
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      throw EvaluationError("failed while traversing SessionLog input root");
    }
    if (iterator->is_symlink(error)) {
      error.clear();
      continue;
    }
    if (iterator->is_regular_file(error) && !error &&
        (iterator->path().extension() == ".jsonl" ||
         ::trajectory::evolution::is_trace_path(iterator->path()))) {
      paths.push_back(iterator->path());
      if (paths.size() > evolution_options_.max_input_files) {
        throw EvaluationError("SessionLog input count exceeds configured limit");
      }
    }
    error.clear();
  }
  if (paths.empty()) {
    throw EvaluationError("SessionLog input root contains no .jsonl files");
  }
  return evaluate_files(paths);
}

// 评测调用方明确选择的文件，避免隐式扫描其他历史数据。
EvolutionBatchOutput TrajectoryEvolutionPipeline::evaluate_files(
    const std::vector<std::filesystem::path> &input_paths) const {
  if (input_paths.size() > evolution_options_.max_input_files) {
    throw EvaluationError("SessionLog input count exceeds configured limit");
  }
  std::vector<std::filesystem::path> paths = input_paths;
  std::sort(paths.begin(), paths.end(), [](const auto &left, const auto &right) {
    return left.generic_string() < right.generic_string();
  });
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

  EvolutionBatchOutput output;
  std::vector<ep::HybridEvaluationReport> reports;
  const std::vector<std::string> secrets = collected_secrets(evaluator_options_.redaction_secrets);
  std::size_t model_requests_used = 0;
  for (const auto &path : paths) {
    DialogueEvolutionResult dialogue;
    dialogue.input_path = path;
    try {
      EvaluatorOptions options = evaluator_options_;
      options.strict_model = false;
      options.max_model_calls = model_requests_used < evaluator_options_.max_model_calls ? 1U : 0U;
      HybridTrajectoryEvaluator evaluator(options, model_adapter_);
      dialogue.evaluation = ::trajectory::evolution::is_trace_path(path)
                                ? evaluator.evaluate_file(path)
                                : evaluator.evaluate_sessionlog(path);
      dialogue.evaluation_available = true;
      model_requests_used += dialogue.evaluation.report.model_requested_count();
      for (const auto &request : dialogue.evaluation.model_requests) {
        output.model_requests.push_back(request);
      }
      reports.push_back(dialogue.evaluation.report);
    } catch (const EvaluationError &error) {
      dialogue.error_code = "evaluation_error";
      dialogue.error_message = safe_error_message(error.what(), secrets);
    } catch (const std::exception &error) {
      dialogue.error_code = "unexpected_evaluation_error";
      dialogue.error_message = safe_error_message(error.what(), secrets);
    }
    output.dialogues.push_back(std::move(dialogue));
  }

  EvolutionBatchOutput synthesized = synthesize_reports(reports);
  for (const auto &request : synthesized.model_requests) {
    output.model_requests.push_back(request);
  }
  output.report.CopyFrom(synthesized.report);
  output.report.clear_runs();
  output.report.set_discovered_input_count(output.dialogues.size());
  output.report.set_evaluation_succeeded_count(0);
  output.report.set_evaluation_failed_count(0);
  for (const auto &dialogue : output.dialogues) {
    auto *run = output.report.add_runs();
    run->set_input_fingerprint(input_fingerprint(dialogue.input_path));
    if (dialogue.evaluation_available) {
      fill_success_run(dialogue.evaluation.report, run);
      output.report.set_evaluation_succeeded_count(output.report.evaluation_succeeded_count() + 1U);
    } else {
      run->set_error_code(dialogue.error_code);
      run->set_error_message(dialogue.error_message);
      output.report.set_evaluation_failed_count(output.report.evaluation_failed_count() + 1U);
    }
  }
  finalize_batch_identity(&output.report);
  ensure_safe_message(output.report, secrets);
  return output;
}

// 将分析报告归并为反思候选与批次结果；来源证据和作用域随候选保留。
EvolutionBatchOutput TrajectoryEvolutionPipeline::synthesize_reports(
    const std::vector<ep::HybridEvaluationReport> &reports) const {
  return synthesize_reports(reports, ep::EvolutionMemoryStore{});
}

// 将分析报告归并为反思候选与批次结果；来源证据和作用域随候选保留。
EvolutionBatchOutput TrajectoryEvolutionPipeline::synthesize_reports(
    const std::vector<ep::HybridEvaluationReport> &reports,
    const ep::EvolutionMemoryStore &memory) const {
  EvolutionBatchOutput output;
  auto *batch = &output.report;
  batch->set_schema_version(1);
  batch->set_evaluation_kind("hybrid_evaluation_and_candidate_synthesis");
  batch->set_pipeline_version(kPipelineVersion);
  batch->set_candidate_generator_version(kCandidateGeneratorVersion);
  batch->set_evaluator_config_fingerprint(
      config_fingerprint(evaluator_options_, evolution_options_, model_adapter_));
  if (evaluator_options_.prompts != nullptr &&
      std::any_of(reports.begin(), reports.end(), [](const auto &report) {
        return report.evaluation_kind() == "content_aware_trace_evaluation";
      })) {
    batch->set_evaluator_config_fingerprint(
        fingerprint(batch->evaluator_config_fingerprint() + "|standard-trace-v1|" +
                    fingerprint(evaluator_options_.prompts->standard_dialogue_system)));
  }
  batch->set_discovered_input_count(reports.size());
  batch->set_evaluation_succeeded_count(reports.size());
  batch->mutable_synthesis()->set_input_report_count(reports.size());
  batch->mutable_meta_reflection()->set_status(ep::MODEL_CALL_STATUS_NOT_REQUESTED);

  std::map<std::string, ep::EvolutionCandidate> seeds;
  for (const auto &report : reports) {
    auto *run = batch->add_runs();
    run->set_input_fingerprint(fingerprint(report.source().user_id() + '\0' +
                                           report.source().conversation_id() + '\0' +
                                           report.source().dialogue_id()));
    fill_success_run(report, run);
    if (!report.source_trace_integrity_valid()) {
      batch->mutable_synthesis()->set_quarantined_report_count(
          batch->synthesis().quarantined_report_count() + 1U);
      continue;
    }
    batch->mutable_synthesis()->set_accepted_report_count(
        batch->synthesis().accepted_report_count() + 1U);
    for (const auto &assessment : report.assessments()) {
      for (const auto &diagnostic : assessment.evidence().diagnostics()) {
        if (!diagnostic.counts_as_invalid() ||
            target_for_category(diagnostic.category()) == ep::CANDIDATE_TARGET_TYPE_UNSPECIFIED) {
          continue;
        }
        add_seed(&seeds, deterministic_seed(report, assessment, diagnostic, evolution_options_));
        batch->mutable_synthesis()->set_deterministic_seed_count(
            batch->synthesis().deterministic_seed_count() + 1U);
      }
    }
    if (report.dialogue_assessment().status() == ep::MODEL_CALL_STATUS_SUCCEEDED) {
      std::uint64_t reflection_seeds = 0;
      add_reflection_changes(report, report.reflection().skill_orchestration_changes(),
                             ep::CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION, evolution_options_,
                             &seeds, &reflection_seeds);
      if (!report.reflection().experience_details().empty()) {
        add_scoped_experiences(report, evolution_options_, &seeds, &reflection_seeds);
      } else {
        // 旧的纯文本报告仍可作为离线候选，不是具备明确作用域的在线经验。
        add_reflection_changes(report, report.reflection().experience_injections(),
                               ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION, evolution_options_,
                               &seeds, &reflection_seeds);
      }
      add_reflection_changes(report, report.reflection().stop_condition_changes(),
                             ep::CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION, evolution_options_,
                             &seeds, &reflection_seeds);
      batch->mutable_synthesis()->set_reflection_seed_count(
          batch->synthesis().reflection_seed_count() + reflection_seeds);
    }
  }

  // 增量批次仍检查新反证是否影响现有记忆；
  // 不会重新评测历史报告，也不将其重新生成为新种子。
  for (const auto &entry : memory.entries()) {
    auto candidate = entry.candidate();
    const auto count = candidate.contradiction_evidence_size();
    add_contradictions(reports, &candidate);
    if (candidate.contradiction_evidence_size() > count)
      add_seed(&seeds, std::move(candidate));
  }
  if (seeds.size() > evolution_options_.max_candidates) {
    throw EvaluationError("candidate seed count exceeds configured limit");
  }
  std::vector<ep::EvolutionCandidate> candidates;
  candidates.reserve(seeds.size());
  for (auto &item : seeds) {
    add_contradictions(reports, &item.second);
    finalize_candidate(&item.second);
    candidates.push_back(std::move(item.second));
  }
  candidates = merge_by_candidate_id(std::move(candidates));
  for (const auto &candidate : candidates) {
    batch->add_candidates()->CopyFrom(candidate);
  }

  const std::vector<std::string> secrets = collected_secrets(evaluator_options_.redaction_secrets);
  if (evolution_options_.enable_meta_reflection) {
    auto *meta = batch->mutable_meta_reflection();
    meta->set_input_seed_count(batch->candidates_size());
    if (batch->candidates().empty()) {
      meta->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
      meta->set_error_code("no_candidate_seeds");
    } else if (model_adapter_ == nullptr) {
      meta->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
      meta->set_error_code("adapter_not_configured");
    } else {
      if (evolution_options_.prompts == nullptr) {
        throw EvaluationError("meta reflection requires prompt templates");
      }
      using CandidateGroup = google::protobuf::RepeatedPtrField<ep::EvolutionCandidate>;
      std::map<std::tuple<int, std::string, bool>, CandidateGroup> seed_groups;
      for (const auto &candidate : batch->candidates()) {
        const auto key = std::make_tuple(static_cast<int>(candidate.target_type()),
                                         candidate.target_scope(), is_scoped_experience(candidate));
        seed_groups[key].Add()->CopyFrom(candidate);
      }

      struct PendingMetaRequest {
        const CandidateGroup *seeds = nullptr;
        ModelRequest request;
      };

      std::vector<PendingMetaRequest> pending_requests;
      bool request_too_large = false;
      for (const auto &[key, group] : seed_groups) {
        (void)key;
        if (group.size() < 2) {
          continue;
        }
        ModelRequest request = make_meta_request(group, *evolution_options_.prompts);
        if (request.system_prompt.size() > evolution_options_.max_meta_prompt_bytes ||
            request.user_prompt.size() > evolution_options_.max_meta_prompt_bytes -
                                             std::min(request.system_prompt.size(),
                                                      evolution_options_.max_meta_prompt_bytes)) {
          request_too_large = true;
        }
        pending_requests.push_back({&group, std::move(request)});
      }

      const std::size_t prior_requests =
          std::accumulate(reports.begin(), reports.end(), std::size_t{0},
                          [](std::size_t count, const auto &report) {
                            return count + report.model_requested_count();
                          });
      const bool request_budget_exhausted =
          prior_requests >= evaluator_options_.max_model_calls ||
          pending_requests.size() > evaluator_options_.max_model_calls - prior_requests;
      if (pending_requests.empty()) {
        meta->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
        meta->set_error_code("no_mergeable_candidate_groups");
      } else if (request_too_large || request_budget_exhausted) {
        meta->set_status(ep::MODEL_CALL_STATUS_SKIPPED);
        meta->set_error_code(request_budget_exhausted ? "model_call_limit_reached"
                                                      : "meta_request_too_large");
      } else {
        std::string request_material;
        meta->set_adapter(model_adapter_->name());
        meta->set_model_id(model_adapter_->model_id());

        std::vector<ep::EvolutionCandidate> refined;
        std::unordered_set<std::string> used_seed_ids;
        std::string response_material;
        std::uint64_t total_attempts = 0;
        bool has_response_body = false;
        bool all_valid = true;
        bool format_repair_used = false;
        // 只统计实际发送的请求，包括格式修复；不能把尚未执行的后续分组记入指纹。
        const auto evaluate = [&](const ModelRequest &request) {
          output.model_requests.push_back(request);
          request_material.append(request.system_prompt);
          request_material.push_back('\0');
          request_material.append(request.user_prompt);
          request_material.push_back('\0');
          RawModelResponse response;
          try {
            response = model_adapter_->evaluate(request);
          } catch (...) {
            response.error_code = "adapter_exception";
          }
          total_attempts += response.attempts;
          response_material.append(response.body);
          response_material.push_back('\0');
          has_response_body = has_response_body || !response.body.empty();
          return response;
        };
        for (std::size_t index = 0; index < pending_requests.size(); ++index) {
          const auto &pending = pending_requests[index];
          RawModelResponse response = evaluate(pending.request);

          MetaValidation validation =
              validate_meta_response(response, *pending.seeds, pending.request, secrets,
                                     evolution_options_.require_chinese_reason);
          // 格式错误可以纠正一次，但证据越权、凭据、传输错误不走此路径。
          // 留足后续分组预算；不回传错误响应，只附加程序产生的固定字段错误码。
          const auto &code = validation.error_code;
          const bool repairable =
              code == "invalid_json" || code == "invalid_candidate_groups" ||
              code == "invalid_candidate_group_object" || code == "invalid_candidate_seed_ids" ||
              code == "invalid_candidate_evidence_event_ids" ||
              code == "invalid_candidate_confidence" || code == "invalid_seed_id" ||
              code.rfind("invalid_candidate_text_", 0) == 0;
          const std::size_t remaining_groups = pending_requests.size() - index - 1;
          if (!validation.valid && response.transport_ok && response.error_code.empty() &&
              repairable && !format_repair_used &&
              output.model_requests.size() + remaining_groups <
                  evaluator_options_.max_model_calls - prior_requests) {
            ModelRequest repair = pending.request;
            repair.user_prompt.append("\nvalidation_error: ").append(code);
            if (repair.system_prompt.size() + repair.user_prompt.size() <=
                evolution_options_.max_meta_prompt_bytes) {
              format_repair_used = true;
              repair.packet_id = "meta:" + fingerprint(repair.system_prompt + '\0' +
                                                        repair.user_prompt);
              response = evaluate(repair);
              validation = validate_meta_response(response, *pending.seeds, repair, secrets,
                                                  evolution_options_.require_chinese_reason);
            }
          }
          if (!validation.valid) {
            meta->set_status(response.transport_ok ? ep::MODEL_CALL_STATUS_INVALID_RESPONSE
                                                   : ep::MODEL_CALL_STATUS_TRANSPORT_ERROR);
            meta->set_error_code(validation.error_code);
            all_valid = false;
            break;
          }
          refined.insert(refined.end(), std::make_move_iterator(validation.candidates.begin()),
                         std::make_move_iterator(validation.candidates.end()));
          used_seed_ids.insert(validation.used_seed_ids.begin(), validation.used_seed_ids.end());
        }
        request_material.pop_back();
        meta->set_request_fingerprint(fingerprint(request_material));
        meta->set_attempts(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(total_attempts, std::numeric_limits<std::uint32_t>::max())));
        if (has_response_body) {
          response_material.pop_back();
          meta->set_response_fingerprint(fingerprint(response_material));
        }

        if (all_valid) {
          const std::size_t output_candidate_count = refined.size();
          for (const auto &seed : batch->candidates()) {
            if (used_seed_ids.find(seed.candidate_id()) == used_seed_ids.end()) {
              refined.push_back(seed);
            }
          }
          refined = merge_by_candidate_id(std::move(refined));
          batch->clear_candidates();
          for (const auto &candidate : refined) {
            batch->add_candidates()->CopyFrom(candidate);
          }
          meta->set_status(ep::MODEL_CALL_STATUS_SUCCEEDED);
          meta->set_output_candidate_count(output_candidate_count);
        }
      }
    }
  }

  refresh_synthesis_metrics(batch);
  finalize_batch_identity(batch);
  ensure_safe_message(*batch, secrets);
  return output;
}

// 读取记忆库并验证版本与结构，损坏内容不得作为可注入经验。
ep::EvolutionMemoryStore TrajectoryEvolutionPipeline::load_memory(const std::filesystem::path &path,
                                                                  std::size_t max_file_bytes) {
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version(kMemoryStoreVersion);
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (error) {
      throw EvaluationError("cannot inspect evolution memory file");
    }
    return memory;
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > max_file_bytes) {
    throw EvaluationError(error ? "cannot stat evolution memory file"
                                : "evolution memory exceeds configured byte limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input || !memory.ParseFromIstream(&input)) {
    throw EvaluationError("cannot parse evolution memory Protobuf");
  }
  if (memory.schema_version() != 1U || memory.store_kind() != "trajectory_evolution_memory" ||
      memory.store_version() != kMemoryStoreVersion) {
    throw EvaluationError("unsupported evolution memory schema or version");
  }
  return memory;
}

namespace {

// 将新的支持来源合入记忆条目，按来源身份去重。
bool append_entry_source(ep::EvolutionMemoryEntry *entry, const std::string &candidate_id_value) {
  const int before = entry->source_candidate_ids_size();
  append_unique(entry->mutable_source_candidate_ids(), candidate_id_value);
  std::vector<std::string> sources = strings_from(entry->source_candidate_ids());
  set_strings(entry->mutable_source_candidate_ids(), std::move(sources));
  return entry->source_candidate_ids_size() != before;
}

// 先用确定性规则查找明确重复项，减少不必要的模型关系判断。
int find_rule_duplicate(const ep::EvolutionCandidate &incoming,
                        const ep::EvolutionMemoryStore &memory) {
  const std::string key = semantic_identity_key(incoming);
  int selected = -1;
  for (int index = 0; index < memory.entries_size(); ++index) {
    const auto &entry = memory.entries(index);
    if (!structurally_compatible(incoming, entry.candidate()) ||
        semantic_identity_key(entry.candidate()) != key) {
      continue;
    }
    if (selected < 0 || entry.memory_id() < memory.entries(selected).memory_id()) {
      selected = index;
    }
  }
  return selected;
}

// 从已审核候选建立新条目，保留来源、状态和修订起点。
int create_memory_entry(ep::EvolutionMemoryStore *memory, const ep::EvolutionCandidate &incoming,
                        const std::string &batch_id, std::uint64_t revision) {
  auto *entry = memory->add_entries();
  entry->set_memory_id("memory:" + incoming.candidate_id());
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  entry->mutable_candidate()->CopyFrom(incoming);
  finalize_candidate(entry->mutable_candidate());
  entry->set_first_batch_id(batch_id);
  entry->set_last_batch_id(batch_id);
  *entry->add_observed_batch_ids() = batch_id;
  append_entry_source(entry, incoming.candidate_id());
  normalize_memory_entry(entry);
  record_revision(entry, revision, batch_id);
  return memory->entries_size() - 1;
}

// 把新支持证据合入既有记忆，避免重复入库，同时保留可追溯修订。
bool merge_into_memory_entry(ep::EvolutionMemoryEntry *entry,
                             const ep::EvolutionCandidate &incoming, const std::string &batch_id,
                             std::uint64_t revision) {
  const std::string before_candidate = entry->candidate().SerializeAsString();
  const std::string before_sources = joined(entry->source_candidate_ids(), "\n");
  ep::EvolutionCandidate merged = entry->candidate();
  merge_candidate_payload(&merged, incoming);
  finalize_candidate(&merged);
  entry->mutable_candidate()->CopyFrom(merged);
  append_entry_source(entry, incoming.candidate_id());
  entry->set_last_batch_id(batch_id);
  append_unique(entry->mutable_observed_batch_ids(), batch_id);
  normalize_memory_entry(entry);
  const bool changed = before_candidate != entry->candidate().SerializeAsString() ||
                       before_sources != joined(entry->source_candidate_ids(), "\n");
  if (changed) {
    record_revision(entry, revision, batch_id);
  }
  return changed;
}

// 记录入库决定最终指向的记忆标识，建立候选与正式条目的对应关系。
void set_decision_destination(ep::MemoryUpdateDecision *decision,
                              const ep::EvolutionMemoryEntry &entry,
                              ep::MemoryUpdateAction action) {
  decision->set_memory_id(entry.memory_id());
  decision->set_action(action);
}

// 将已确认的匹配关系应用到现有条目，保留修订和证据。
void apply_existing_match(ep::EvolutionMemoryStore *memory, int entry_index,
                          const ep::EvolutionCandidate &incoming, const std::string &batch_id,
                          std::uint64_t revision, ep::MemoryUpdateDecision *decision,
                          ep::MemoryUpdateResult *result) {
  auto *entry = memory->mutable_entries(entry_index);
  decision->set_matched_memory_id(entry->memory_id());
  const bool changed = merge_into_memory_entry(entry, incoming, batch_id, revision);
  set_decision_destination(decision, *entry,
                           changed ? ep::MEMORY_UPDATE_ACTION_MERGED
                                   : ep::MEMORY_UPDATE_ACTION_UNCHANGED);
  if (changed) {
    result->set_updated_entry_count(result->updated_entry_count() + 1U);
  } else {
    result->set_unchanged_entry_count(result->unchanged_entry_count() + 1U);
  }
}

// 创建经过审查的新记忆，并更新本批次决定与库索引。
int apply_new_entry(ep::EvolutionMemoryStore *memory, const ep::EvolutionCandidate &incoming,
                    const std::string &batch_id, std::uint64_t revision,
                    ep::MemoryUpdateDecision *decision, ep::MemoryUpdateResult *result) {
  const int index = create_memory_entry(memory, incoming, batch_id, revision);
  set_decision_destination(decision, memory->entries(index), ep::MEMORY_UPDATE_ACTION_INSERTED);
  result->set_inserted_entry_count(result->inserted_entry_count() + 1U);
  return index;
}

// 创建新条目并同时登记关系，避免条目已入库但冲突边缺失。
void apply_new_entry_with_relation(ep::EvolutionMemoryStore *memory, int matched_index,
                                   const ep::EvolutionCandidate &incoming,
                                   ep::MemoryMatchRelation relation, double confidence,
                                   const std::string &batch_id, std::uint64_t revision,
                                   ep::MemoryUpdateDecision *decision,
                                   ep::MemoryUpdateResult *result) {
  const std::string matched_id = memory->entries(matched_index).memory_id();
  const int inserted_index =
      apply_new_entry(memory, incoming, batch_id, revision, decision, result);
  const std::string inserted_id = memory->entries(inserted_index).memory_id();
  decision->set_matched_memory_id(matched_id);
  auto *inserted = memory->mutable_entries(inserted_index);
  const bool inserted_changed =
      upsert_relation(inserted, matched_id, relation, confidence, decision->decision_id());
  auto *matched = memory->mutable_entries(matched_index);
  const bool matched_changed =
      upsert_relation(matched, inserted_id, relation, confidence, decision->decision_id());
  (void)inserted_changed;
  if (matched_changed) {
    result->set_updated_entry_count(result->updated_entry_count() + 1U);
  }
}

// 初始化候选入库决定，后续分支必须给出明确处理结果。
void initialize_memory_decision(ep::MemoryUpdateDecision *decision, const std::string &batch_id,
                                const std::string &candidate_id_value, bool full_store_curation) {
  decision->set_candidate_id(candidate_id_value);
  decision->set_full_store_curation(full_store_curation);
  std::string material = kMemoryCuratorVersion;
  material.push_back('\0');
  material.append(batch_id);
  material.push_back('\0');
  material.append(full_store_curation ? "full" : "incremental");
  material.push_back('\0');
  material.append(candidate_id_value);
  decision->set_decision_id("memory-decision:" + fingerprint(material));
}

// 为已处理批次恢复既有决定，不重复计入支持或新增条目。
void fill_idempotent_decisions(const ep::HybridEvaluationBatchReport &batch,
                               const ep::EvolutionMemoryStore &memory,
                               ep::MemoryUpdateResult *result) {
  std::unordered_map<std::string, int> entry_by_id;
  std::unordered_map<std::string, int> entry_by_candidate_id;
  build_memory_indices(memory, &entry_by_id, &entry_by_candidate_id);
  bool incomplete = false;
  for (const auto &incoming : batch.candidates()) {
    auto *decision = result->add_decisions();
    initialize_memory_decision(decision, batch.batch_id(), incoming.candidate_id(), false);
    if (incoming.status() == ep::EVOLUTION_CANDIDATE_STATUS_REJECTED_BY_GATE) {
      decision->set_action(ep::MEMORY_UPDATE_ACTION_SKIPPED);
      decision->set_relation(ep::MEMORY_MATCH_RELATION_UNSPECIFIED);
      decision->set_match_method(ep::MEMORY_MATCH_METHOD_STRUCTURED_RULE);
      decision->set_reason("候选未通过质量门禁，不写入记忆库。");
      continue;
    }
    const auto found = entry_by_candidate_id.find(incoming.candidate_id());
    if (found == entry_by_candidate_id.end()) {
      decision->set_action(ep::MEMORY_UPDATE_ACTION_SKIPPED);
      decision->set_relation(ep::MEMORY_MATCH_RELATION_UNRESOLVED);
      decision->set_match_method(ep::MEMORY_MATCH_METHOD_STORED_ALIAS);
      decision->set_error_code("applied_batch_mapping_missing");
      decision->set_reason("批次已应用，但历史记忆缺少该候选的映射。");
      incomplete = true;
      continue;
    }
    const auto &entry = memory.entries(found->second);
    decision->set_memory_id(entry.memory_id());
    decision->set_matched_memory_id(entry.memory_id());
    decision->set_action(ep::MEMORY_UPDATE_ACTION_UNCHANGED);
    const bool canonical = entry.candidate().candidate_id() == incoming.candidate_id();
    decision->set_relation(canonical ? ep::MEMORY_MATCH_RELATION_EXACT
                                     : ep::MEMORY_MATCH_RELATION_DUPLICATE);
    decision->set_match_method(canonical ? ep::MEMORY_MATCH_METHOD_CANDIDATE_ID
                                         : ep::MEMORY_MATCH_METHOD_STORED_ALIAS);
    decision->set_confidence(1.0);
    decision->set_reason("相同批次已应用，复用记忆库中的稳定候选映射。");
    result->set_unchanged_entry_count(result->unchanged_entry_count() + 1U);
  }
  if (incomplete) {
    result->set_error_code("idempotent_mapping_incomplete");
  }
}

// 执行候选与已有记忆的关系判定，使用规则召回和可选模型审查。
ParsedMemoryMatch
run_memory_match(const ep::EvolutionCandidate &incoming, const ep::EvolutionMemoryStore &memory,
                 const std::vector<MemoryShortlistItem> &shortlist, MemoryCurationContext *context,
                 ep::MemoryUpdateDecision *decision, ep::MemoryUpdateResult *result) {
  ParsedMemoryMatch output;
  const bool semantic_enabled = context != nullptr && context->options != nullptr &&
                                context->options->enable_memory_semantic_matching;
  if (!semantic_enabled || context->adapter == nullptr) {
    decision->set_match_method(ep::MEMORY_MATCH_METHOD_STRUCTURED_RULE);
    output.error_code = !semantic_enabled ? "semantic_matching_disabled" : "adapter_not_configured";
    return output;
  }
  if (context->options->prompts == nullptr) {
    decision->set_match_method(ep::MEMORY_MATCH_METHOD_MODEL);
    output.error_code = "prompt_templates_not_configured";
    return output;
  }
  const ModelRequest request =
      make_memory_match_request(incoming, memory, shortlist, *context->options->prompts);
  const std::size_t max_prompt = context->options->max_memory_match_prompt_bytes;
  const bool secret_present = contains_secret(request.user_prompt, context->secrets);
  if (request.system_prompt.size() > max_prompt ||
      request.user_prompt.size() >
          max_prompt - std::min(request.system_prompt.size(), max_prompt) ||
      secret_present) {
    decision->set_match_method(ep::MEMORY_MATCH_METHOD_MODEL);
    output.error_code =
        secret_present ? "credential_in_memory_request" : "memory_match_request_too_large";
    return output;
  }

  decision->set_match_method(ep::MEMORY_MATCH_METHOD_MODEL);
  decision->set_adapter(context->adapter->name());
  decision->set_model_id(context->adapter->model_id());
  std::string request_material = request.system_prompt;
  request_material.push_back('\0');
  request_material.append(request.user_prompt);
  decision->set_request_fingerprint(fingerprint(request_material));

  RawModelResponse response;
  bool response_cached = false;
  bool model_called = false;
  ManagementJson *record = nullptr;
  if (context->journal) {
    record = &(*context->journal)["records"][memory_review_key(request, *context)];
    if (record->null()) *record = ManagementJson::Object{};
    if ((*record)["result"].object()) {
      response.transport_ok = true;
      response.body = (*record)["result"].dump();
      response_cached = true;
      result->set_review_cache_hit_count(result->review_cache_hit_count() + 1);
    }
  }
  if (!response_cached && context->response_cache != nullptr) {
    const auto cached = context->response_cache->find(request.packet_id);
    if (cached != context->response_cache->end()) {
      response = cached->second;
      response_cached = true;
    }
  }
  if (!response_cached) {
    if (record && record->num("attempts", 0) >= context->options->memory_review_max_attempts) {
      output.error_code = record->str("error", "memory_review_exhausted");
      (*record)["state"] = "unresolved";
      (*record)["next_due_ms"] = 0;
      if (context->checkpoint) context->checkpoint();
      return output;
    }
    if (record && record->num("next_due_ms", 0) > management_now_ms()) {
      output.error_code = "memory_review_backoff";
      return output;
    }
    if (context->model_rate_limited) {
      output.error_code = "http_429";
      return output;
    }
    if (context->remaining_model_calls == 0U) {
      output.error_code = "model_call_limit_reached";
      return output;
    }
    --context->remaining_model_calls;
    result->set_model_requested_count(result->model_requested_count() + 1U);
    auto actual = request;
    if (record) {
      // 只纠正该项协议错误，不用上一轮原文扩张上下文，也不猜测替代语义。
      if (!record->str("error").empty())
        actual.user_prompt += '\n' + ManagementJson{{"repair_error", record->str("error")},
            {"rejected_memory_id", record->str("rejected_memory_id")}}.dump();
      (*record)["attempts"] = record->num("attempts", 0) + 1;
      (*record)["state"] = "running";
      (*context->journal)["total_model_calls"] = context->journal->num("total_model_calls", 0) + 1;
      (*record)["next_due_ms"] = management_now_ms() + context->options->memory_review_retry_backoff_ms;
      if (context->checkpoint) context->checkpoint();
    }
    actual.packet_id = "memory-curation:" + fingerprint(actual.system_prompt + '\0' + actual.user_prompt);
    decision->set_request_fingerprint(fingerprint(actual.system_prompt + '\0' + actual.user_prompt));
    if (context->model_requests) context->model_requests->push_back(actual);
    try {
      response = context->adapter->evaluate(actual);
    } catch (...) {
      response.error_code = "adapter_exception";
    }
    if (context->response_cache != nullptr) {
      (*context->response_cache)[request.packet_id] = response;
    }
    model_called = true;
  }
  if (response.error_code == "http_429") {
    context->model_rate_limited = true;
  }
  if (response_cached && record && !record->str("request_fingerprint").empty())
    decision->set_request_fingerprint(record->str("request_fingerprint"));
  decision->set_attempts(response.attempts);
  if (!response.body.empty()) {
    decision->set_response_fingerprint(fingerprint(response.body));
  }
  output = validate_memory_match_response(response, shortlist, memory, context->secrets,
                                          context->options->require_chinese_reason);
  // L1/L2/L3 在相同权限范围内也可能互相冲突，但跨类型/契约绝不能按重复合并。
  if (output.valid && output.relation == ep::MEMORY_MATCH_RELATION_DUPLICATE) {
    const auto match = std::find_if(memory.entries().begin(), memory.entries().end(),
                                     [&](const auto &entry) { return entry.memory_id() == output.memory_id; });
    if (match == memory.entries().end() || !structurally_compatible(incoming, match->candidate())) {
      output.valid = false;
      output.error_code = "cross_contract_duplicate";
    }
  }
  if (output.valid && output.relation != ep::MEMORY_MATCH_RELATION_UNRESOLVED &&
      output.confidence < (output.relation == ep::MEMORY_MATCH_RELATION_DUPLICATE
                               ? context->options->min_memory_duplicate_confidence
                               : context->options->min_memory_relation_confidence)) {
    // 低置信判断既不是独立证据，也不是网络失败；停留未决，等待内容或审查器改变。
    output.relation = ep::MEMORY_MATCH_RELATION_UNRESOLVED;
    output.memory_id.clear();
  }
  if (record && !(*record)["result"].object()) {
    if (output.valid) {
      std::string relation = ep::MemoryMatchRelation_Name(output.relation);
      relation.erase(0, std::string("MEMORY_MATCH_RELATION_").size());
      (*record)["result"] = ManagementJson{{"relation", relation}, {"memory_id", output.memory_id},
          {"confidence", output.confidence}, {"reason", output.reason}};
      (*record)["state"] = output.relation == ep::MEMORY_MATCH_RELATION_UNRESOLVED ? "unresolved" : "completed";
      (*record)["next_due_ms"] = 0;
      record->fields().erase("error");
    } else {
      (*record)["error"] = output.error_code;
      (*record)["rejected_memory_id"] = output.memory_id;
      // A cross-contract duplicate is rejected by a deterministic local
      // compatibility rule. Retrying the same semantic request cannot make
      // that response valid, so keep the candidate and finish this review in
      // the terminal unresolved state instead of spending model calls on an
      // unproductive repair loop.
      const bool terminal_protocol_error = output.error_code == "cross_contract_duplicate";
      (*record)["state"] = terminal_protocol_error ||
                                     record->num("attempts", 0) >= context->options->memory_review_max_attempts
                                 ? "unresolved" : "pending";
      (*record)["next_due_ms"] = record->str("state") == "pending"
          ? management_now_ms() + context->options->memory_review_retry_backoff_ms : 0;
    }
    (*record)["updated_ms"] = management_now_ms();
    (*record)["request_fingerprint"] = decision->request_fingerprint();
    (*record)["response_fingerprint"] = decision->response_fingerprint();
    if (context->checkpoint) context->checkpoint();
  } else if (record && (*record)["result"].object()) {
    // 缓存回放引用原始模型请求/响应指纹，而不是重新序列化后的 JSON 指纹。
    if (!record->str("request_fingerprint").empty()) decision->set_request_fingerprint(record->str("request_fingerprint"));
    if (!record->str("response_fingerprint").empty()) decision->set_response_fingerprint(record->str("response_fingerprint"));
  }
  if (model_called) {
    if (output.valid) {
      result->set_model_succeeded_count(result->model_succeeded_count() + 1U);
    } else {
      result->set_model_failed_count(result->model_failed_count() + 1U);
    }
  }
  return output;
}

// 冻结当前库后并发计算一小组关系。工作线程只持有副本，绝不写库或共享计数器。
// 调用预算在提交前预留；结果回到当前线程后先存缓存，后按 memory_id 顺序应用。
void prefetch_memory_reviews(const ep::EvolutionMemoryStore &memory,
                             const std::vector<std::string> &ids, MemoryCurationContext *context,
                             ep::MemoryUpdateResult *result,
                             const std::vector<ep::EvolutionCandidate> *incoming = nullptr) {
  if (!context || !context->adapter || !context->options || !context->options->prompts ||
      context->options->memory_review_parallelism <= 1 || !context->response_cache || context->model_rate_limited)
    return;
  struct Completed {
    ep::MemoryUpdateResult result;
    std::unordered_map<std::string, RawModelResponse> responses;
    std::vector<ModelRequest> requests;
    ep::EvolutionCandidate subject;
    std::vector<MemoryShortlistItem> shortlist;
    std::string request_fingerprint;
  };
  std::vector<std::future<Completed>> pending;
  const auto count = incoming ? incoming->size() : ids.size();
  for (std::size_t index = 0; index < count; ++index) {
    const std::string id = incoming ? std::string{} : ids[index];
    ep::EvolutionCandidate subject;
    if (incoming) {
      subject = (*incoming)[index];
    } else {
      const auto found = std::find_if(memory.entries().begin(), memory.entries().end(),
                                      [&](const auto &e) { return e.memory_id() == id; });
      if (found == memory.entries().end()) continue;
      subject = found->candidate();
    }
    if (!incoming && context->journal) {
      const auto &work = (*context->journal)["work"][id];
      if (work.str("subject") == memory_subject_fingerprint(subject) &&
          (work.str("state") == "completed" || work.str("state") == "unresolved")) continue;
    }
    const auto shortlist = memory_shortlist(subject, memory, context->options->max_memory_match_candidates, id);
    if (shortlist.empty() || !context->remaining_model_calls) continue;
    const auto request = make_memory_match_request(subject, memory, shortlist, *context->options->prompts);
    if (context->response_cache->count(request.packet_id)) continue;
    if (request.system_prompt.size() + request.user_prompt.size() > context->options->max_memory_match_prompt_bytes ||
        contains_secret(request.user_prompt, context->secrets)) continue;
    const std::string review_key = memory_review_key(request, *context);
    bool repair = false;
    ManagementJson repair_record;
    if (context->journal) {
      auto &record = (*context->journal)["records"][review_key];
      if (record.null()) record = ManagementJson::Object{};
      if (record["result"].object() || record.num("attempts", 0) >= context->options->memory_review_max_attempts ||
          record.num("next_due_ms", 0) > management_now_ms()) continue;
      // 协议错误项也进入并发队列；在线程本地复制原记录，让修复请求携带
      // repair_error/rejected_memory_id，同时不把共享 journal 指针带入工作线程。
      repair = !record.str("error").empty();
      if (repair) repair_record = record;
      record["attempts"] = record.num("attempts", 0) + 1;
      record["state"] = "running";
      (*context->journal)["total_model_calls"] = context->journal->num("total_model_calls", 0) + 1;
      record["next_due_ms"] = management_now_ms() + context->options->memory_review_retry_backoff_ms;
    }
    --context->remaining_model_calls;
    if (context->checkpoint) context->checkpoint();
    MemoryCurationContext local = *context;
    local.journal = nullptr;
    local.checkpoint = {};
    local.remaining_model_calls = 1;
    pending.push_back(std::async(std::launch::async,
                                 [local, subject, &memory, shortlist, repair, repair_record, review_key]() mutable {
      Completed completed;
      completed.subject = subject;
      completed.shortlist = shortlist;
      local.response_cache = &completed.responses;
      local.model_requests = &completed.requests;
      ManagementJson local_journal;
      if (repair) {
        local_journal = ManagementJson::Object{};
        local_journal["records"][review_key] = repair_record;
        // Outer prefetch reserves this attempt and sets a backoff. The worker
        // must use the copied due record to issue the repair call now.
        local_journal["records"][review_key]["next_due_ms"] = 0;
        local.journal = &local_journal;
      } else {
        local.journal = nullptr;
      }
      ep::MemoryUpdateDecision decision;
      run_memory_match(subject, memory, shortlist, &local, &decision, &completed.result);
      completed.request_fingerprint = decision.request_fingerprint();
      return completed;
    }));
    if (pending.size() >= context->options->memory_review_parallelism) break;
  }
  std::size_t remaining = pending.size();
  while (remaining) {
    bool progressed = false;
    for (auto &future : pending) {
    if (!future.valid() || future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) continue;
    auto completed = future.get();
    --remaining;
    progressed = true;
    result->set_model_requested_count(result->model_requested_count() + completed.result.model_requested_count());
    result->set_model_succeeded_count(result->model_succeeded_count() + completed.result.model_succeeded_count());
    result->set_model_failed_count(result->model_failed_count() + completed.result.model_failed_count());
    for (auto &item : completed.responses) {
      context->model_rate_limited |= item.second.error_code == "http_429";
      (*context->response_cache)[item.first] = std::move(item.second);
    }
    if (context->model_requests)
      context->model_requests->insert(context->model_requests->end(), completed.requests.begin(), completed.requests.end());
    // 先基于原快照验证并保存已返回判断，再应用会改变短名单的合并。
    // 否则合并使输入变化后，这些成功响应会永久遗留为 running。
    if (context->journal) {
      ep::MemoryUpdateDecision decision;
      run_memory_match(completed.subject, memory, completed.shortlist, context, &decision, result);
      // The replay sees the original cache key; preserve the actual repair
      // prompt identity recorded by the worker instead of its base prompt.
      if (!completed.request_fingerprint.empty()) {
        const auto request = make_memory_match_request(completed.subject, memory, completed.shortlist,
                                                       *context->options->prompts);
        auto &record = (*context->journal)["records"][memory_review_key(request, *context)];
        if (record.str("request_fingerprint") != completed.request_fingerprint) {
          record["request_fingerprint"] = completed.request_fingerprint;
          if (context->checkpoint) context->checkpoint();
        }
      }
    }
    }
    if (!progressed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

// 按记忆身份查找当前库内位置，不能依赖历史数组下标。
int find_memory_index(const ep::EvolutionMemoryStore &memory, std::string_view memory_id) {
  for (int index = 0; index < memory.entries_size(); ++index) {
    if (memory.entries(index).memory_id() == memory_id) {
      return index;
    }
  }
  return -1;
}

// 给记忆状态定义管理优先级，供规范条目选择与淘汰使用。
int memory_status_priority(ep::EvolutionMemoryStatus status) {
  return status == ep::EVOLUTION_MEMORY_STATUS_VALIDATED   ? 2
         : status == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE ? 1
                                                           : 0;
}

// 在已有关系之间选择有效且优先的判定，避免低可信关系覆盖确认结果。
bool prefer_relation(ep::MemoryMatchRelation relation, double confidence,
                     const ep::EvolutionMemoryRelation &existing) {
  if (relation != existing.relation()) {
    if (relation == ep::MEMORY_MATCH_RELATION_CONTRADICTS) {
      return true;
    }
    if (existing.relation() == ep::MEMORY_MATCH_RELATION_CONTRADICTS) {
      return false;
    }
  }
  return confidence > existing.confidence();
}

// 整理修订顺序，保持历史读取与最终状态一致。
void sort_memory_revisions(ep::EvolutionMemoryEntry *entry) {
  std::vector<ep::EvolutionMemoryRevision> revisions(entry->revisions().begin(),
                                                     entry->revisions().end());
  std::sort(revisions.begin(), revisions.end(), [](const auto &left, const auto &right) {
    if (left.store_revision() != right.store_revision()) {
      return left.store_revision() < right.store_revision();
    }
    if (left.batch_id() != right.batch_id()) {
      return left.batch_id() < right.batch_id();
    }
    return left.candidate_snapshot().candidate_id() < right.candidate_snapshot().candidate_id();
  });
  revisions.erase(std::unique(revisions.begin(), revisions.end(),
                              [](const auto &left, const auto &right) {
                                return left.SerializeAsString() == right.SerializeAsString();
                              }),
                  revisions.end());
  entry->clear_revisions();
  for (const auto &revision : revisions) {
    entry->add_revisions()->CopyFrom(revision);
  }
}

struct StoredRelationEdge {
  std::string left;
  std::string right;
  ep::MemoryMatchRelation relation = ep::MEMORY_MATCH_RELATION_UNSPECIFIED;
  double confidence = 0.0;
  std::string decision_id;
};

// 重建需要双向一致的记忆关系，避免只在一端记录重复或冲突。
void rebuild_symmetric_relations(ep::EvolutionMemoryStore *memory) {
  std::unordered_set<std::string> valid_ids;
  for (const auto &entry : memory->entries()) {
    valid_ids.insert(entry.memory_id());
  }
  std::map<std::pair<std::string, std::string>, StoredRelationEdge> edges;
  for (const auto &entry : memory->entries()) {
    for (const auto &relation : entry.relations()) {
      if (relation.target_memory_id() == entry.memory_id() ||
          valid_ids.find(relation.target_memory_id()) == valid_ids.end() ||
          (relation.relation() != ep::MEMORY_MATCH_RELATION_RELATED &&
           relation.relation() != ep::MEMORY_MATCH_RELATION_CONTRADICTS)) {
        continue;
      }
      const auto key = std::minmax(entry.memory_id(), relation.target_memory_id());
      const std::pair<std::string, std::string> edge_key{key.first, key.second};
      const auto found = edges.find(edge_key);
      if (found == edges.end()) {
        edges.emplace(edge_key, StoredRelationEdge{
                                    edge_key.first,
                                    edge_key.second,
                                    relation.relation(),
                                    relation.confidence(),
                                    relation.decision_id(),
                                });
      } else {
        ep::EvolutionMemoryRelation current;
        current.set_relation(found->second.relation);
        current.set_confidence(found->second.confidence);
        if (prefer_relation(relation.relation(), relation.confidence(), current)) {
          found->second.relation = relation.relation();
          found->second.confidence = relation.confidence();
          found->second.decision_id = relation.decision_id();
        }
      }
    }
  }

  for (auto &entry : *memory->mutable_entries()) {
    entry.clear_relations();
  }
  for (const auto &item : edges) {
    const auto &edge = item.second;
    const int left = find_memory_index(*memory, edge.left);
    const int right = find_memory_index(*memory, edge.right);
    if (left < 0 || right < 0) {
      continue;
    }
    (void)upsert_relation(memory->mutable_entries(left), edge.right, edge.relation, edge.confidence,
                          edge.decision_id);
    (void)upsert_relation(memory->mutable_entries(right), edge.left, edge.relation, edge.confidence,
                          edge.decision_id);
  }
}

// 按当前记忆的状态和支持信息计算淘汰优先级。
int memory_eviction_priority(ep::EvolutionMemoryStatus status) {
  switch (status) {
  case ep::EVOLUTION_MEMORY_STATUS_VALIDATED:
    return 3;
  case ep::EVOLUTION_MEMORY_STATUS_CANDIDATE:
    return 2;
  case ep::EVOLUTION_MEMORY_STATUS_ARCHIVED:
    return 1;
  case ep::EVOLUTION_MEMORY_STATUS_REJECTED:
  case ep::EVOLUTION_MEMORY_STATUS_UNSPECIFIED:
    return 0;
  default:
    return 0;
  }
}

// 为未成熟候选计算清理优先级，优先保留有充分依据的内容。
int candidate_eviction_priority(ep::EvolutionCandidateStatus status) {
  switch (status) {
  case ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED:
    return 3;
  case ep::EVOLUTION_CANDIDATE_STATUS_CONFLICTED:
    return 2;
  case ep::EVOLUTION_CANDIDATE_STATUS_INSUFFICIENT_SUPPORT:
    return 1;
  case ep::EVOLUTION_CANDIDATE_STATUS_REJECTED_BY_GATE:
  case ep::EVOLUTION_CANDIDATE_STATUS_UNSPECIFIED:
    return 0;
  default:
    return 0;
  }
}

// 读取条目的最新有效修订，供关系版本和去重比较使用。
std::uint64_t latest_memory_revision(const ep::EvolutionMemoryEntry &entry) {
  std::uint64_t latest = 0;
  for (const auto &revision : entry.revisions()) {
    latest = std::max(latest, revision.store_revision());
  }
  return latest;
}

struct MemoryEvictionGroup {
  std::vector<std::string> memory_ids;
  int memory_priority = 0;
  int candidate_priority = 0;
  std::uint64_t support_user_count = 0;
  std::uint64_t support_dialogue_count = 0;
  std::uint64_t net_support_episode_count = 0;
  double confidence = 0.0;
  std::uint64_t latest_revision = 0;
  std::string stable_id;
};

// 把必须协同处理的关联记忆分组，避免淘汰后留下悬空关系。
std::vector<MemoryEvictionGroup>
build_memory_eviction_groups(const ep::EvolutionMemoryStore &memory) {
  std::unordered_map<std::string, int> entry_by_id;
  for (int index = 0; index < memory.entries_size(); ++index) {
    entry_by_id.emplace(memory.entries(index).memory_id(), index);
  }

  std::vector<MemoryEvictionGroup> groups;
  std::vector<bool> visited(static_cast<std::size_t>(memory.entries_size()), false);
  for (int start = 0; start < memory.entries_size(); ++start) {
    if (visited[static_cast<std::size_t>(start)]) {
      continue;
    }
    MemoryEvictionGroup group;
    std::vector<int> pending{start};
    while (!pending.empty()) {
      const int index = pending.back();
      pending.pop_back();
      if (visited[static_cast<std::size_t>(index)]) {
        continue;
      }
      visited[static_cast<std::size_t>(index)] = true;
      const auto &entry = memory.entries(index);
      const auto &candidate = entry.candidate();
      group.memory_ids.push_back(entry.memory_id());
      group.memory_priority =
          std::max(group.memory_priority, memory_eviction_priority(entry.status()));
      group.candidate_priority =
          std::max(group.candidate_priority, candidate_eviction_priority(candidate.status()));
      group.support_user_count = std::max(group.support_user_count, candidate.support_user_count());
      group.support_dialogue_count =
          std::max(group.support_dialogue_count, candidate.support_dialogue_count());
      const std::uint64_t net_support =
          candidate.support_episode_count() > candidate.contradiction_count()
              ? candidate.support_episode_count() - candidate.contradiction_count()
              : 0U;
      group.net_support_episode_count = std::max(group.net_support_episode_count, net_support);
      group.confidence = std::max(group.confidence, candidate.confidence());
      group.latest_revision = std::max(group.latest_revision, latest_memory_revision(entry));
      for (const auto &relation : entry.relations()) {
        if (relation.relation() != ep::MEMORY_MATCH_RELATION_CONTRADICTS) {
          continue;
        }
        const auto found = entry_by_id.find(relation.target_memory_id());
        if (found != entry_by_id.end() && !visited[static_cast<std::size_t>(found->second)]) {
          pending.push_back(found->second);
        }
      }
    }
    std::sort(group.memory_ids.begin(), group.memory_ids.end());
    group.stable_id = group.memory_ids.front();
    groups.push_back(std::move(group));
  }
  std::sort(groups.begin(), groups.end(), [](const auto &left, const auto &right) {
    if (left.memory_priority != right.memory_priority) {
      return left.memory_priority < right.memory_priority;
    }
    if (left.candidate_priority != right.candidate_priority) {
      return left.candidate_priority < right.candidate_priority;
    }
    if (left.support_user_count != right.support_user_count) {
      return left.support_user_count < right.support_user_count;
    }
    if (left.support_dialogue_count != right.support_dialogue_count) {
      return left.support_dialogue_count < right.support_dialogue_count;
    }
    if (left.net_support_episode_count != right.net_support_episode_count) {
      return left.net_support_episode_count < right.net_support_episode_count;
    }
    if (left.confidence != right.confidence) {
      return left.confidence < right.confidence;
    }
    if (left.latest_revision != right.latest_revision) {
      return left.latest_revision < right.latest_revision;
    }
    return left.stable_id < right.stable_id;
  });
  return groups;
}

// 建立可追溯的淘汰记录，明确被移除的身份和原因。
void initialize_eviction_decision(ep::MemoryUpdateDecision *decision, const std::string &batch_id,
                                  const ep::EvolutionMemoryEntry &entry, bool conflict_group) {
  decision->set_candidate_id(entry.candidate().candidate_id());
  decision->set_memory_id(entry.memory_id());
  decision->set_action(ep::MEMORY_UPDATE_ACTION_EVICTED);
  decision->set_match_method(ep::MEMORY_MATCH_METHOD_STRUCTURED_RULE);
  decision->set_reason(conflict_group ? "记忆库达到容量上限；按生命周期、证据强度和最近修订排序后，"
                                        "该冲突连通组整体淘汰，避免只删除冲突一侧。"
                                      : "记忆库达到容量上限；按生命周期、证据强度和最近修订排序后"
                                        "淘汰本条低优先级记忆。");
  std::string material = kMemoryCuratorVersion;
  material.push_back('\0');
  material.append(batch_id);
  material.push_back('\0');
  material.append("capacity-eviction");
  material.push_back('\0');
  material.append(entry.memory_id());
  decision->set_decision_id("memory-decision:" + fingerprint(material));
}

// 限制历史批次索引增长，在容量约束下保留必要的幂等记录。
std::size_t prune_applied_batch_ids(ep::EvolutionMemoryStore *memory, std::size_t keep_count) {
  const std::size_t current = static_cast<std::size_t>(memory->applied_batch_ids_size());
  if (current <= keep_count) {
    return 0;
  }
  const std::size_t removed = current - keep_count;
  memory->mutable_applied_batch_ids()->DeleteSubrange(0, static_cast<int>(removed));
  return removed;
}

// 移除选定记忆组并同步更新相关关系和管理记录。
void evict_memory_group(ep::EvolutionMemoryStore *memory, const MemoryEvictionGroup &group,
                        const std::string &batch_id, ep::MemoryUpdateResult *result) {
  const bool conflict_group = group.memory_ids.size() > 1U;
  std::unordered_set<std::string> removed_ids(group.memory_ids.begin(), group.memory_ids.end());
  for (const auto &memory_id : group.memory_ids) {
    const int index = find_memory_index(*memory, memory_id);
    if (index < 0) {
      continue;
    }
    initialize_eviction_decision(result->add_decisions(), batch_id, memory->entries(index),
                                 conflict_group);
    result->set_evicted_entry_count(result->evicted_entry_count() + 1U);
  }

  std::vector<ep::EvolutionMemoryEntry> retained;
  retained.reserve(static_cast<std::size_t>(memory->entries_size()) -
                   std::min(static_cast<std::size_t>(memory->entries_size()), removed_ids.size()));
  for (const auto &entry : memory->entries()) {
    if (removed_ids.find(entry.memory_id()) == removed_ids.end()) {
      retained.push_back(entry);
    }
  }
  memory->clear_entries();
  for (const auto &entry : retained) {
    memory->add_entries()->CopyFrom(entry);
  }
  rebuild_symmetric_relations(memory);
}

// 按照容量与保留规则淘汰记忆组，不能只删除索引而遗留悬空关系。
void enforce_memory_capacity(ep::EvolutionMemoryStore *memory, const std::string &batch_id,
                             std::size_t max_entries, std::size_t max_applied_batch_ids,
                             std::size_t max_memory_bytes, ep::MemoryUpdateResult *result) {
  const std::size_t initially_pruned = prune_applied_batch_ids(memory, max_applied_batch_ids);
  result->set_pruned_applied_batch_id_count(result->pruned_applied_batch_id_count() +
                                            initially_pruned);

  const auto over_capacity = [&]() {
    return static_cast<std::size_t>(memory->entries_size()) > max_entries ||
           memory->ByteSizeLong() > max_memory_bytes;
  };
  if (over_capacity()) {
    rebuild_symmetric_relations(memory);
    const auto groups = build_memory_eviction_groups(*memory);
    for (const auto &group : groups) {
      if (!over_capacity()) {
        break;
      }
      evict_memory_group(memory, group, batch_id, result);
    }
  }

  while (memory->ByteSizeLong() > max_memory_bytes && memory->applied_batch_ids_size() > 1) {
    const std::size_t removed = prune_applied_batch_ids(
        memory, static_cast<std::size_t>(memory->applied_batch_ids_size() - 1));
    result->set_pruned_applied_batch_id_count(result->pruned_applied_batch_id_count() + removed);
  }
  if (over_capacity()) {
    throw EvaluationError("evolution memory metadata exceeds configured byte limit");
  }
}

// 在可合并条目中选择规范代表，保留支持证据和可追溯身份。
int choose_canonical_memory(const ep::EvolutionMemoryStore &memory, int left, int right) {
  const int left_priority = memory_status_priority(memory.entries(left).status());
  const int right_priority = memory_status_priority(memory.entries(right).status());
  if (left_priority != right_priority) {
    return left_priority > right_priority ? left : right;
  }
  return memory.entries(left).memory_id() < memory.entries(right).memory_id() ? left : right;
}

// 合并已确认重复的库内条目，迁移来源与关系后处理被合并条目。
void merge_existing_memory_entries(ep::EvolutionMemoryStore *memory, int canonical_index,
                                   int duplicate_index, const std::string &batch_id,
                                   std::uint64_t revision, ep::MemoryUpdateDecision *decision,
                                   ep::MemoryUpdateResult *result) {
  const ep::EvolutionMemoryEntry duplicate = memory->entries(duplicate_index);
  const std::string canonical_id = memory->entries(canonical_index).memory_id();
  const std::string duplicate_id = duplicate.memory_id();
  auto *canonical = memory->mutable_entries(canonical_index);
  for (const auto &snapshot : duplicate.revisions()) {
    canonical->add_revisions()->CopyFrom(snapshot);
  }
  (void)merge_into_memory_entry(canonical, duplicate.candidate(), batch_id, revision);
  for (const auto &source_id : duplicate.source_candidate_ids()) {
    append_entry_source(canonical, source_id);
  }
  for (const auto &observed_batch : duplicate.observed_batch_ids()) {
    append_unique(canonical->mutable_observed_batch_ids(), observed_batch);
  }
  for (const auto &relation : duplicate.relations()) {
    canonical->add_relations()->CopyFrom(relation);
  }
  if (memory_status_priority(duplicate.status()) > memory_status_priority(canonical->status())) {
    canonical->set_status(duplicate.status());
  }
  canonical->set_last_batch_id(batch_id);
  normalize_memory_entry(canonical);
  sort_memory_revisions(canonical);

  for (auto &entry : *memory->mutable_entries()) {
    for (auto &relation : *entry.mutable_relations()) {
      if (relation.target_memory_id() == duplicate_id) {
        relation.set_target_memory_id(canonical_id);
      }
    }
  }
  memory->mutable_entries()->DeleteSubrange(duplicate_index, 1);
  rebuild_symmetric_relations(memory);
  const int final_index = find_memory_index(*memory, canonical_id);
  if (final_index < 0) {
    throw EvaluationError("canonical memory disappeared during full curation");
  }
  record_revision(memory->mutable_entries(final_index), revision, batch_id);
  sort_memory_revisions(memory->mutable_entries(final_index));
  decision->set_memory_id(canonical_id);
  decision->set_action(ep::MEMORY_UPDATE_ACTION_MERGED);
  result->set_updated_entry_count(result->updated_entry_count() + 1U);
  result->set_full_curation_merged_count(result->full_curation_merged_count() + 1U);
}

// 在管理周期内检查全库关系与状态，不在主 Agent 的执行路径远程调用模型。
bool run_full_store_curation(ep::EvolutionMemoryStore *memory, const std::string &batch_id,
                             std::uint64_t revision, MemoryCurationContext *context,
                             ep::MemoryUpdateResult *result) {
  while (true) {
    bool merged = false;
    for (int left = 0; left < memory->entries_size() && !merged; ++left) {
      if (!is_manageable_memory(memory->entries(left))) {
        continue;
      }
      for (int right = left + 1; right < memory->entries_size(); ++right) {
        if (!is_manageable_memory(memory->entries(right)) ||
            !structurally_compatible(memory->entries(left).candidate(), memory->entries(right).candidate()) ||
            semantic_identity_key(memory->entries(left).candidate()) !=
                semantic_identity_key(memory->entries(right).candidate())) {
          continue;
        }
        const int canonical = choose_canonical_memory(*memory, left, right);
        const int duplicate = canonical == left ? right : left;
        const std::string canonical_id = memory->entries(canonical).memory_id();
        const std::string duplicate_id = memory->entries(duplicate).memory_id();
        auto *decision = result->add_decisions();
        initialize_memory_decision(decision, batch_id,
                                   memory->entries(duplicate).candidate().candidate_id(), true);
        decision->set_matched_memory_id(canonical_id);
        decision->set_relation(ep::MEMORY_MATCH_RELATION_DUPLICATE);
        decision->set_match_method(ep::MEMORY_MATCH_METHOD_STRUCTURED_RULE);
        decision->set_confidence(1.0);
        decision->set_reason("全库复核发现规范化后完全一致的记忆，合并到稳定主记录。");
        auto *option = decision->add_considered_matches();
        option->set_memory_id(canonical_id);
        option->set_lexical_score(1.0);
        option->set_content_fingerprint(
            candidate_content_fingerprint(memory->entries(canonical).candidate()));
        option->set_memory_status(memory->entries(canonical).status());
        merge_existing_memory_entries(memory, canonical, duplicate, batch_id, revision, decision,
                                      result);
        result->set_full_curation_examined_count(result->full_curation_examined_count() + 1U);
        (void)duplicate_id;
        merged = true;
        break;
      }
    }
    if (!merged) {
      break;
    }
  }

  const bool semantic_requested = context != nullptr && context->options != nullptr &&
                                  context->options->enable_memory_semantic_matching;
  if (!semantic_requested) {
    memory->set_new_entries_since_full_curation(0);
    if (context && context->journal) {
      (*context->journal)["active"] = false;
      (*context->journal)["pending_count"] = 0;
      (*context->journal)["next_due_ms"] = 0;
      (*context->journal)["status"] = "disabled";
    }
    return true;
  }
  std::vector<std::string> memory_ids;
  if (context->journal && (*context->journal).flag("active", false) && !context->journal->flag("refresh_all", false) &&
      memory->new_entries_since_full_curation() < context->options->full_curation_new_entry_threshold) {
    for (const auto &id : (*context->journal)["subjects"].items()) memory_ids.push_back(id.text());
  } else {
    for (const auto &entry : memory->entries())
      if (is_manageable_memory(entry)) memory_ids.push_back(entry.memory_id());
    if (context->journal) {
      auto &journal = *context->journal;
      journal["subjects"] = ManagementJson::Array{};
      journal["work"] = ManagementJson::Object{};
      for (const auto &id : memory_ids) journal["subjects"].push(id);
      journal["active"] = true;
      journal["refresh_all"] = false;
      journal["generation"] = journal.num("generation", 0) + 1;
      // 阈值只创建一代工作，失败不再把这批新增数量反复当作下一代触发条件。
      memory->set_new_entries_since_full_curation(0);
      if (context->checkpoint) context->checkpoint();
    }
  }
  std::sort(memory_ids.begin(), memory_ids.end());
  std::size_t semantic_review_count = 0;
  for (const auto &memory_id : memory_ids) {
    const int subject_index = find_memory_index(*memory, memory_id);
    if (subject_index < 0) {
      continue;
    }
    const auto shortlist =
        memory_shortlist(memory->entries(subject_index).candidate(), *memory,
                         context->options->max_memory_match_candidates, memory_id);
    if (shortlist.empty()) {
      continue;
    }
    ++semantic_review_count;
    if (context->adapter == nullptr) {
      continue;
    }
    if (context->options->prompts == nullptr) {
      result->set_full_curation_error_code("prompt_templates_not_configured");
      return false;
    }
  }
  if (semantic_review_count && context->adapter == nullptr) {
    result->set_full_curation_error_code("adapter_not_configured");
    result->set_review_pending_count(semantic_review_count);
    if (context->journal) {
      (*context->journal)["pending_count"] = semantic_review_count;
      (*context->journal)["status"] = "waiting_for_model";
    }
    return false;
  }

  bool completed = true;
  std::size_t position = 0;
  for (const auto &memory_id : memory_ids) {
    if (position++ % context->options->memory_review_parallelism == 0)
      prefetch_memory_reviews(*memory, std::vector<std::string>(memory_ids.begin() + position - 1, memory_ids.end()), context, result);
    const int subject_index = find_memory_index(*memory, memory_id);
    if (subject_index < 0) {
      continue;
    }
    const ep::EvolutionCandidate subject = memory->entries(subject_index).candidate();
    ManagementJson *work = context->journal ? &(*context->journal)["work"][memory_id] : nullptr;
    const auto subject_fingerprint = memory_subject_fingerprint(subject);
    if (work) {
      if (work->str("subject") == subject_fingerprint &&
          (work->str("state") == "completed" || work->str("state") == "unresolved")) continue;
      *work = ManagementJson{{"subject", subject_fingerprint}, {"state", "pending"}};
    }
    const auto shortlist = memory_shortlist(
        subject, *memory, context->options->max_memory_match_candidates, memory_id);
    if (shortlist.empty()) {
      if (work) (*work)["state"] = "completed";
      continue;
    }

    auto *decision = result->add_decisions();
    initialize_memory_decision(decision, batch_id, subject.candidate_id(), true);
    decision->set_memory_id(memory_id);
    add_shortlist_to_decision(shortlist, *memory, decision);
    result->set_full_curation_examined_count(result->full_curation_examined_count() + 1U);
    const ParsedMemoryMatch parsed =
        run_memory_match(subject, *memory, shortlist, context, decision, result);
    if (!parsed.valid) {
      if (work) {
        const auto request = make_memory_match_request(subject, *memory, shortlist, *context->options->prompts);
        const auto &record = (*context->journal)["records"][memory_review_key(request, *context)];
        (*work)["state"] = record.str("state") == "unresolved" ? "unresolved" : "pending";
        (*work)["next_due_ms"] = record.num("next_due_ms", management_now_ms() + context->options->memory_review_retry_backoff_ms);
      }
      decision->set_action(ep::MEMORY_UPDATE_ACTION_SKIPPED);
      decision->set_relation(ep::MEMORY_MATCH_RELATION_UNRESOLVED);
      decision->set_error_code(parsed.error_code);
      decision->set_reason("本条复核未完成；保留记忆并记录逐项退避或未决状态，不重跑已完成判断。");
      completed = false;
      if (parsed.error_code == "http_429") {
        result->set_full_curation_error_code(parsed.error_code);
        continue;
      }
      continue;
    }

    decision->set_relation(parsed.relation);
    decision->set_confidence(parsed.confidence);
    decision->set_reason(bounded_text(parsed.reason));
    decision->set_matched_memory_id(parsed.memory_id);
    if (work) (*work)["state"] = parsed.relation == ep::MEMORY_MATCH_RELATION_UNRESOLVED ? "unresolved" : "completed";
    if (parsed.relation == ep::MEMORY_MATCH_RELATION_UNRESOLVED) {
      decision->set_action(ep::MEMORY_UPDATE_ACTION_UNCHANGED);
      decision->set_error_code("memory_relation_uncertain");
      completed = false;
      continue;
    }
    if (parsed.relation == ep::MEMORY_MATCH_RELATION_DUPLICATE &&
        parsed.confidence >= context->options->min_memory_duplicate_confidence) {
      const int current_subject = find_memory_index(*memory, memory_id);
      const int matched = find_memory_index(*memory, parsed.memory_id);
      if (current_subject < 0 || matched < 0 || current_subject == matched) {
        decision->set_action(ep::MEMORY_UPDATE_ACTION_SKIPPED);
        decision->set_error_code("full_curation_memory_changed");
        if (work) (*work)["state"] = "pending";
        completed = false;
        continue;
      }
      const int canonical = choose_canonical_memory(*memory, current_subject, matched);
      const int duplicate = canonical == current_subject ? matched : current_subject;
      merge_existing_memory_entries(memory, canonical, duplicate, batch_id, revision, decision,
                                    result);
      continue;
    }

    if ((parsed.relation == ep::MEMORY_MATCH_RELATION_RELATED ||
         parsed.relation == ep::MEMORY_MATCH_RELATION_CONTRADICTS) &&
        parsed.confidence >= context->options->min_memory_relation_confidence) {
      const int current_subject = find_memory_index(*memory, memory_id);
      const int matched = find_memory_index(*memory, parsed.memory_id);
      if (current_subject < 0 || matched < 0 || current_subject == matched) {
        decision->set_action(ep::MEMORY_UPDATE_ACTION_SKIPPED);
        decision->set_error_code("full_curation_memory_changed");
        if (work) (*work)["state"] = "pending";
        completed = false;
        continue;
      }
      const bool subject_changed =
          upsert_relation(memory->mutable_entries(current_subject), parsed.memory_id,
                          parsed.relation, parsed.confidence, decision->decision_id());
      const bool matched_changed =
          upsert_relation(memory->mutable_entries(matched), memory_id, parsed.relation,
                          parsed.confidence, decision->decision_id());
      decision->set_action(subject_changed || matched_changed ? ep::MEMORY_UPDATE_ACTION_LINKED
                                                              : ep::MEMORY_UPDATE_ACTION_UNCHANGED);
      if (subject_changed) {
        result->set_updated_entry_count(result->updated_entry_count() + 1U);
      }
      if (matched_changed) {
        result->set_updated_entry_count(result->updated_entry_count() + 1U);
      }
      continue;
    }

    decision->set_action(ep::MEMORY_UPDATE_ACTION_UNCHANGED);
    if (parsed.relation != ep::MEMORY_MATCH_RELATION_DISTINCT) {
      decision->set_error_code("memory_match_confidence_below_threshold");
    }
    result->set_unchanged_entry_count(result->unchanged_entry_count() + 1U);
  }
  if (!completed && result->full_curation_error_code().empty()) {
    result->set_full_curation_error_code("semantic_review_incomplete");
  }
  if (context->journal) {
    auto &journal = *context->journal;
    std::size_t pending = 0, unresolved = 0, due = 0;
    for (const auto &id : memory_ids) {
      const int index = find_memory_index(*memory, id);
      if (index < 0 || !is_manageable_memory(memory->entries(index))) continue;
      const auto &work = journal["work"][id];
      const bool current = work.str("subject") == memory_subject_fingerprint(memory->entries(index).candidate());
      if (current && work.str("state") == "completed") continue;
      if (current && work.str("state") == "unresolved") { ++unresolved; continue; }
      const auto shortlist = memory_shortlist(memory->entries(index).candidate(), *memory,
                                              context->options->max_memory_match_candidates, id);
      if (shortlist.empty()) continue;
      const auto request = make_memory_match_request(memory->entries(index).candidate(), *memory, shortlist, *context->options->prompts);
      const auto &record = journal["records"][memory_review_key(request, *context)];
      {
        ++pending;
        const auto next = std::max(management_now_ms() + 1, work.num("next_due_ms",
            record.num("next_due_ms", management_now_ms() + context->options->memory_review_retry_backoff_ms)));
        due = !due ? next : std::min(due, next);
      }
    }
    journal["active"] = pending != 0;
    journal["pending_count"] = pending;
    journal["unresolved_count"] = unresolved;
    journal["next_due_ms"] = due;
    journal["status"] = pending ? "pending" : unresolved ? "unresolved" : "completed";
    result->set_review_pending_count(pending);
    result->set_review_unresolved_count(unresolved);
    result->set_review_next_due_ms(due);
    completed = !pending && !unresolved;
    if (completed) result->clear_full_curation_error_code();
    if (context->checkpoint) context->checkpoint();
  }
  return completed;
}

// 单项入库判断未完成时，只加入该条目的后续复核，不重新打开整代已完成工作。
void queue_memory_review(MemoryCurationContext *context, const std::string &memory_id) {
  if (!context || !context->journal) return;
  auto &journal = *context->journal;
  if (!journal.flag("active", false)) {
    // 新增一项失败不能把上代所有已完成条目重新排入队列。
    journal["subjects"] = ManagementJson::Array{};
    if (journal["work"].object())
      for (const auto &item : journal["work"].fields())
        if (item.second.str("state") == "unresolved") journal["subjects"].push(item.first);
  }
  journal["active"] = true;
  auto &subjects = journal["subjects"].items();
  if (std::none_of(subjects.begin(), subjects.end(), [&](const auto &id) { return id.text() == memory_id; }))
    subjects.push_back(memory_id);
}

// 在同一份库状态中应用审核、合并和淘汰结果，保持关系与内容一致。
ep::MemoryUpdateResult curate_memory_in_place(const ep::HybridEvaluationBatchReport &batch,
                                              ep::EvolutionMemoryStore *memory,
                                              MemoryCurationContext *context) {
  if (memory == nullptr) {
    throw EvaluationError("evolution memory destination is null");
  }
  const bool maintenance = batch.batch_id().empty() && batch.candidates().empty();
  if (batch.batch_id().empty() && !maintenance) {
    throw EvaluationError("batch_id is required for idempotent memory merge");
  }
  std::unordered_set<std::string> batch_candidate_ids;
  for (const auto &incoming : batch.candidates()) {
    if (!candidate_identity_valid(incoming) ||
        !batch_candidate_ids.insert(incoming.candidate_id()).second) {
      throw EvaluationError("invalid or duplicate candidate identity for memory curation");
    }
  }
  initialize_memory_store(memory);

  const EvolutionOptions default_options;
  const EvolutionOptions &capacity_options =
      context == nullptr || context->options == nullptr ? default_options : *context->options;
  const std::size_t capacity_bytes = context == nullptr || context->max_memory_bytes == 0U
                                         ? capacity_options.max_memory_bytes
                                         : context->max_memory_bytes;
  ep::MemoryUpdateResult result;
  result.set_requested(true);
  result.set_curator_version(kMemoryCuratorVersion);
  result.set_revision_before(memory->revision());
  for (const auto &applied : memory->applied_batch_ids()) {
    if (applied == batch.batch_id()) {
      result.set_applied(false);
      result.set_idempotent_replay(true);
      fill_idempotent_decisions(batch, *memory, &result);
      enforce_memory_capacity(memory, batch.batch_id(), capacity_options.max_memory_entries,
                              capacity_options.max_applied_batch_ids, capacity_bytes, &result);
      result.set_pending_new_entry_count(memory->new_entries_since_full_curation());
      result.set_revision_after(memory->revision());
      if (!context || !context->journal || !context->journal->flag("active", false)) return result;
      break;
    }
  }

  const std::uint64_t revision = memory->revision() + 1U;
  memory->set_revision(revision);
  if (!maintenance && !result.idempotent_replay())
    append_unique(memory->mutable_applied_batch_ids(), batch.batch_id());

  // Model comparisons use a frozen base; all writes remain in the ordered loop
  // below. Exact/rule duplicates need no request, including duplicates in this
  // batch. Candidates with no base shortlist keep the existing incremental path
  // so the first entries of a new scope can still discover each other.
  ep::EvolutionMemoryStore incremental_snapshot;
  std::unordered_set<std::string> prefetched_candidates;
  if (!result.idempotent_replay() && context && context->adapter && context->options &&
      context->options->enable_memory_semantic_matching && context->options->prompts &&
      context->options->memory_review_parallelism > 1 && context->response_cache &&
      !memory->entries().empty()) {
    incremental_snapshot = *memory;
    std::unordered_map<std::string, int> existing_ids, existing_candidates;
    build_memory_indices(incremental_snapshot, &existing_ids, &existing_candidates);
    std::unordered_set<std::string> seen_rules;
    std::vector<ep::EvolutionCandidate> pending_candidates;
    const auto flush_prefetch = [&] {
      prefetch_memory_reviews(incremental_snapshot, {}, context, &result, &pending_candidates);
      pending_candidates.clear();
    };
    for (const auto &incoming : batch.candidates()) {
      if (incoming.status() == ep::EVOLUTION_CANDIDATE_STATUS_REJECTED_BY_GATE) continue;
      const bool first_rule = seen_rules.insert(semantic_identity_key(incoming)).second;
      if (!first_rule || existing_candidates.count(incoming.candidate_id()) ||
          find_rule_duplicate(incoming, incremental_snapshot) >= 0 ||
          memory_shortlist(incoming, incremental_snapshot,
                           context->options->max_memory_match_candidates).empty()) continue;
      prefetched_candidates.insert(incoming.candidate_id());
      pending_candidates.push_back(incoming);
      if (pending_candidates.size() >= context->options->memory_review_parallelism)
        flush_prefetch();
    }
    if (!pending_candidates.empty()) flush_prefetch();
  }

  for (const auto &incoming : batch.candidates()) {
    if (result.idempotent_replay()) break;
    auto *decision = result.add_decisions();
    initialize_memory_decision(decision, batch.batch_id(), incoming.candidate_id(), false);
    if (incoming.status() == ep::EVOLUTION_CANDIDATE_STATUS_REJECTED_BY_GATE) {
      decision->set_action(ep::MEMORY_UPDATE_ACTION_SKIPPED);
      decision->set_match_method(ep::MEMORY_MATCH_METHOD_STRUCTURED_RULE);
      decision->set_reason("候选未通过质量门禁，不写入记忆库。");
      continue;
    }

    std::unordered_map<std::string, int> entry_by_id;
    std::unordered_map<std::string, int> entry_by_candidate_id;
    build_memory_indices(*memory, &entry_by_id, &entry_by_candidate_id);
    const auto exact = entry_by_candidate_id.find(incoming.candidate_id());
    if (exact != entry_by_candidate_id.end()) {
      const auto &existing = memory->entries(exact->second);
      const bool canonical = existing.candidate().candidate_id() == incoming.candidate_id();
      decision->set_relation(canonical ? ep::MEMORY_MATCH_RELATION_EXACT
                                       : ep::MEMORY_MATCH_RELATION_DUPLICATE);
      decision->set_match_method(canonical ? ep::MEMORY_MATCH_METHOD_CANDIDATE_ID
                                           : ep::MEMORY_MATCH_METHOD_STORED_ALIAS);
      decision->set_confidence(1.0);
      decision->set_reason(canonical ? "候选 ID 与已有记忆完全一致。"
                                     : "候选 ID 已记录为已有记忆的语义别名。");
      apply_existing_match(memory, exact->second, incoming, batch.batch_id(), revision, decision,
                           &result);
      continue;
    }

    const int rule_duplicate = find_rule_duplicate(incoming, *memory);
    if (rule_duplicate >= 0) {
      const auto shortlist = memory_shortlist(incoming, *memory,
                                              context == nullptr || context->options == nullptr
                                                  ? 1U
                                                  : context->options->max_memory_match_candidates);
      add_shortlist_to_decision(shortlist, *memory, decision);
      decision->set_relation(ep::MEMORY_MATCH_RELATION_DUPLICATE);
      decision->set_match_method(ep::MEMORY_MATCH_METHOD_STRUCTURED_RULE);
      decision->set_confidence(1.0);
      decision->set_reason("候选的类型、作用域、触发条件和修改建议规范化后完全一致。");
      apply_existing_match(memory, rule_duplicate, incoming, batch.batch_id(), revision, decision,
                           &result);
      continue;
    }

    const std::size_t shortlist_limit = context == nullptr || context->options == nullptr
                                            ? 8U
                                            : context->options->max_memory_match_candidates;
    const auto &comparison_memory = prefetched_candidates.count(incoming.candidate_id())
                                        ? incremental_snapshot : *memory;
    const auto shortlist = memory_shortlist(incoming, comparison_memory, shortlist_limit);
    add_shortlist_to_decision(shortlist, comparison_memory, decision);
    if (shortlist.empty()) {
      decision->set_relation(ep::MEMORY_MATCH_RELATION_DISTINCT);
      decision->set_match_method(ep::MEMORY_MATCH_METHOD_STRUCTURED_RULE);
      decision->set_confidence(1.0);
      decision->set_reason("记忆库中不存在类型和作用域兼容的候选。");
      apply_new_entry(memory, incoming, batch.batch_id(), revision, decision, &result);
      continue;
    }

    const ParsedMemoryMatch parsed =
        run_memory_match(incoming, comparison_memory, shortlist, context, decision, &result);
    if (!parsed.valid) {
      decision->set_relation(ep::MEMORY_MATCH_RELATION_UNRESOLVED);
      decision->set_error_code(parsed.error_code);
      decision->set_reason(
          decision->match_method() == ep::MEMORY_MATCH_METHOD_MODEL
              ? "模型语义判断失败；按保守策略独立入库，不执行猜测性合并。"
              : "存在结构兼容的历史记忆，但未完成模型语义判断；按保守策略独立入库。");
      apply_new_entry(memory, incoming, batch.batch_id(), revision, decision, &result);
      if (context && context->journal && decision->match_method() == ep::MEMORY_MATCH_METHOD_MODEL) {
        queue_memory_review(context, decision->memory_id());
      }
      continue;
    }

    decision->set_relation(parsed.relation);
    decision->set_confidence(parsed.confidence);
    decision->set_reason(bounded_text(parsed.reason));
    decision->set_matched_memory_id(parsed.memory_id);
    const auto matched = entry_by_id.find(parsed.memory_id);
    // A preceding candidate may merge and remove the target that was present
    // in the frozen comparison snapshot. Treat that stale relation as an
    // unresolved item instead of dereferencing a missing index or guessing a
    // new target; the normal review queue can retry it against the new store.
    if (parsed.relation != ep::MEMORY_MATCH_RELATION_DISTINCT &&
        parsed.relation != ep::MEMORY_MATCH_RELATION_UNRESOLVED &&
        matched == entry_by_id.end()) {
      decision->set_relation(ep::MEMORY_MATCH_RELATION_UNRESOLVED);
      decision->set_error_code("memory_match_target_missing");
      decision->set_reason("模型引用的快照记忆已在本批前序合并中移除；保留候选并等待新库状态复核。");
      apply_new_entry(memory, incoming, batch.batch_id(), revision, decision, &result);
      if (context && context->journal) queue_memory_review(context, decision->memory_id());
      continue;
    }
    if (parsed.relation == ep::MEMORY_MATCH_RELATION_UNRESOLVED) {
      decision->set_error_code("memory_relation_uncertain");
      apply_new_entry(memory, incoming, batch.batch_id(), revision, decision, &result);
      if (context && context->journal) {
        queue_memory_review(context, decision->memory_id());
      }
      continue;
    }
    if (parsed.relation == ep::MEMORY_MATCH_RELATION_DUPLICATE &&
        parsed.confidence >= context->options->min_memory_duplicate_confidence) {
      apply_existing_match(memory, matched->second, incoming, batch.batch_id(), revision, decision,
                           &result);
      continue;
    }
    if ((parsed.relation == ep::MEMORY_MATCH_RELATION_RELATED ||
         parsed.relation == ep::MEMORY_MATCH_RELATION_CONTRADICTS) &&
        parsed.confidence >= context->options->min_memory_relation_confidence) {
      apply_new_entry_with_relation(memory, matched->second, incoming, parsed.relation,
                                    parsed.confidence, batch.batch_id(), revision, decision,
                                    &result);
      continue;
    }
    if (parsed.relation != ep::MEMORY_MATCH_RELATION_DISTINCT) {
      decision->set_error_code("memory_match_confidence_below_threshold");
    }
    apply_new_entry(memory, incoming, batch.batch_id(), revision, decision, &result);
  }
  const std::uint64_t inserted = result.inserted_entry_count();
  const std::uint64_t previous_pending = memory->new_entries_since_full_curation();
  const std::uint64_t pending =
      inserted > std::numeric_limits<std::uint64_t>::max() - previous_pending
          ? std::numeric_limits<std::uint64_t>::max()
          : previous_pending + inserted;
  memory->set_new_entries_since_full_curation(pending);
  const std::size_t full_curation_threshold =
      context == nullptr || context->options == nullptr
          ? EvolutionOptions().full_curation_new_entry_threshold
          : context->options->full_curation_new_entry_threshold;
  if (pending >= full_curation_threshold ||
      (context && context->journal && context->journal->flag("active", false))) {
    result.set_full_curation_triggered(true);
    const bool completed =
        run_full_store_curation(memory, maintenance ? "curation:" + std::to_string(revision) : batch.batch_id(),
                                 revision, context, &result);
    result.set_full_curation_completed(completed);
    if (completed && (!context || !context->journal)) {
      memory->set_new_entries_since_full_curation(0U);
    }
  }
  enforce_memory_capacity(memory, batch.batch_id(), capacity_options.max_memory_entries,
                          capacity_options.max_applied_batch_ids, capacity_bytes, &result);
  result.set_pending_new_entry_count(memory->new_entries_since_full_curation());

  std::sort(
      memory->mutable_entries()->begin(), memory->mutable_entries()->end(),
      [](const auto &left, const auto &right) { return left.memory_id() < right.memory_id(); });
  initialize_memory_store(memory);
  if (maintenance && !result.full_curation_triggered()) memory->set_revision(result.revision_before());
  result.set_applied(true);
  result.set_revision_after(memory->revision());
  return result;
}

} // 结束当前匿名命名空间。

// 把本批候选合入既有库，统一执行重复、关联、冲突和容量管理。
ep::MemoryUpdateResult
TrajectoryEvolutionPipeline::merge_memory(const ep::HybridEvaluationBatchReport &batch,
                                          ep::EvolutionMemoryStore *memory) {
  return curate_memory_in_place(batch, memory, nullptr);
}

// 对进化产物执行最终隐私检查，阻止凭据混入经验和策略。
void ensure_safe_evolution_data(const google::protobuf::Message &message) {
  ensure_safe_message(message, collected_secrets({}));
}

// 加锁读取最新库后应用增量并原子写回，避免并发写入覆盖他人结果。
bool TrajectoryEvolutionPipeline::transform_memory_file(
    const std::filesystem::path &protobuf_path, const std::filesystem::path &json_path,
    const std::function<void(ep::EvolutionMemoryStore &)> &update, std::size_t max_file_bytes) {
  MemoryFileLock lock(protobuf_path);
  auto memory = load_memory(protobuf_path, max_file_bytes);
  const auto before = memory;
  const auto revision = memory.revision();
  update(memory);
  // Struct 内的 map 无固定序列化顺序；按消息内容比较，不能把相同反馈误判为新修订。
  const bool changed = !google::protobuf::util::MessageDifferencer::Equals(before, memory);
  if (changed) {
    memory.set_revision(revision + 1);
    ensure_safe_evolution_data(memory);
    if (memory.ByteSizeLong() > max_file_bytes)
      throw EvaluationError("evolution memory exceeds configured byte limit");
    write_protobuf(memory, protobuf_path);
  }
  // Protobuf 是权威数据。即使上次 JSON 写入中断，重放也会修复阅读视图。
  if (!json_path.empty())
    write_bytes_atomic(json_path, to_json(memory, true));
  return changed;
}

// 加锁读取最新库后应用增量并原子写回，避免并发写入覆盖他人结果。
ep::MemoryUpdateResult TrajectoryEvolutionPipeline::update_memory_file(
    const ep::HybridEvaluationBatchReport &batch, const std::filesystem::path &protobuf_path,
    const std::filesystem::path &json_path, std::size_t max_file_bytes) {
  MemoryFileLock lock(protobuf_path);
  ep::EvolutionMemoryStore memory = load_memory(protobuf_path, max_file_bytes);
  const auto before = memory;
  EvolutionOptions options;
  options.enable_memory_semantic_matching = false;
  MemoryCurationContext context;
  context.options = &options;
  context.max_memory_bytes = max_file_bytes;
  ep::MemoryUpdateResult result = curate_memory_in_place(batch, &memory, &context);
  const std::vector<std::string> secrets = collected_secrets({});
  ensure_safe_message(memory, secrets);
  if (result.idempotent_replay()) {
    if (!google::protobuf::util::MessageDifferencer::Equals(before, memory)) {
      write_protobuf(memory, protobuf_path);
    }
    if (!json_path.empty()) {
      try {
        write_bytes_atomic(json_path, to_json(memory, true));
      } catch (const EvaluationError &) {
        result.set_error_code("memory_json_view_write_failed");
      }
    }
    return result;
  }
  write_protobuf(memory, protobuf_path);
  if (!json_path.empty()) {
    try {
      write_bytes_atomic(json_path, to_json(memory, true));
    } catch (const EvaluationError &) {
      result.set_error_code("memory_json_view_write_failed");
    }
  }
  return result;
}

// 在受保护的文件更新流程中执行全库管理，保留原有来源和修订。
MemoryUpdateOutput TrajectoryEvolutionPipeline::curate_memory_file(
    const ep::HybridEvaluationBatchReport &batch, const std::filesystem::path &protobuf_path,
    const std::filesystem::path &json_path, std::size_t max_file_bytes,
    std::size_t prior_model_calls) const {
  MemoryUpdateOutput output;
  const std::size_t global_remaining = prior_model_calls >= evaluator_options_.max_model_calls
                                           ? 0U
                                           : evaluator_options_.max_model_calls - prior_model_calls;
  std::size_t remaining_calls = global_remaining;
  std::uint64_t requested_total = 0;
  std::uint64_t succeeded_total = 0;
  std::uint64_t failed_total = 0;
  bool model_rate_limited = false;
  const std::vector<std::string> secrets = collected_secrets(evaluator_options_.redaction_secrets);
  std::unordered_map<std::string, RawModelResponse> response_cache;
  if (evolution_options_.memory_review_parallelism < 1 || evolution_options_.memory_review_parallelism > 32 ||
      evolution_options_.memory_review_max_attempts < 1 || evolution_options_.memory_review_max_attempts > 4 ||
      evolution_options_.memory_review_retry_backoff_ms < 1 || evolution_options_.memory_review_retry_backoff_ms > 3600000)
    throw EvaluationError("invalid memory review scheduling limits");
  const auto started = std::chrono::steady_clock::now();
  const std::filesystem::path journal_path = protobuf_path.string() + ".curation.json";
  // 整理锁与记忆提交锁分开：只串行同一用户的后台整理，不占用 Agent 打点锁。
  MemoryFileLock journal_lock(journal_path);
  auto journal = load_curation_journal(journal_path);
  // 已拿到同用户整理锁，旧 running 不可能仍由另一个整理器执行。
  // 没有持久结果就是中断，不捏造成功；只在该输入再次需要时按剩余次数恢复。
  for (auto &item : journal["records"].fields()) {
    auto &record = item.second;
    if (!record["result"].object() && record.str("state") == "running") {
      record["error"] = "memory_review_interrupted";
      record["state"] = record.num("attempts", 0) >= evolution_options_.memory_review_max_attempts ? "unresolved" : "pending";
    }
  }
  if (journal.flag("commit_pending", false)) {
    // 检查点和 Protobuf 是两个原子文件；断在二者之间时，不能确信工作已经应用。
    // 丢弃的是完成标记，不是已验证的模型结果；恢复时从结果缓存重放本地合并。
    journal["work"] = ManagementJson::Object{};
    journal["active"] = true;
  }
  if (model_adapter_ && evolution_options_.prompts) {
    const auto signature = fingerprint(model_adapter_->name() + '\0' + model_adapter_->model_id() + '\0' +
        evolution_options_.prompts->memory_curator_system + '\0' + evolution_options_.prompts->memory_curator_user + '\0' +
        std::to_string(evolution_options_.min_memory_duplicate_confidence) + ':' +
        std::to_string(evolution_options_.min_memory_relation_confidence) + ':' +
        std::to_string(evolution_options_.require_chinese_reason));
    if (!journal.str("reviewer_signature").empty() && journal.str("reviewer_signature") != signature) {
      journal["active"] = true;
      journal["refresh_all"] = true;
    }
    journal["reviewer_signature"] = signature;
  }
  bool planning = false;
  ManagementJson committed_work;
  const auto checkpoint = [&]() {
    auto &records = journal["records"].fields();
    while (records.size() > 8192) {
      auto oldest = records.end();
      for (auto it = records.begin(); it != records.end(); ++it)
        if (it->second.str("state") != "pending" && it->second.str("state") != "running" &&
            (oldest == records.end() || it->second.num("updated_ms", 0) < oldest->second.num("updated_ms", 0))) oldest = it;
      if (oldest == records.end()) throw EvaluationError("memory review worklist capacity");
      records.erase(oldest);
    }
    auto durable = journal;
    if (planning) {
      // 模型返回可立即保存，尚在内存中试算的合并则不能提前标成已提交。
      // 进程中断后保留上一份已提交进度，继续应用缓存，不漏掉待写入的关系。
      durable["work"] = committed_work;
      durable["active"] = true;
      durable["status"] = "pending";
      durable["pending_count"] = std::max<std::size_t>(1, durable.num("pending_count", 0));
      durable["next_due_ms"] = 0;
    }
    const auto bytes = durable.dump();
    if (bytes.size() > 32U * 1024U * 1024U || contains_secret(bytes, secrets))
      throw EvaluationError("unsafe or oversized memory curation journal");
    write_bytes_atomic(journal_path, bytes);
  };

  for (std::size_t attempt = 0; attempt < evolution_options_.max_memory_update_retries; ++attempt) {
    ep::EvolutionMemoryStore base = load_memory(protobuf_path, max_file_bytes);
    ep::EvolutionMemoryStore planned = base;
    MemoryCurationContext context;
    context.options = &evolution_options_;
    context.adapter = model_adapter_;
    context.remaining_model_calls = remaining_calls;
    context.model_requests = &output.model_requests;
    context.response_cache = &response_cache;
    context.secrets = secrets;
    context.max_memory_bytes = max_file_bytes;
    context.journal = &journal;
    context.checkpoint = checkpoint;
    // 升级旧的已收尾检查点时，只有库修订完全一致才恢复逐项完成状态。
    // 中断中的缓存结果不能视为已经应用，仍走正常的缓存校验与提交路径。
    if (!journal["work"].object() && !journal.flag("active", false) && !journal.flag("commit_pending", false) &&
        journal.num("memory_revision", 0) == base.revision() && context.adapter && context.options->prompts) {
      journal["work"] = ManagementJson::Object{};
      for (const auto &entry : base.entries()) {
        if (!is_manageable_memory(entry)) continue;
        const auto shortlist = memory_shortlist(entry.candidate(), base, context.options->max_memory_match_candidates, entry.memory_id());
        std::string state = "completed";
        if (!shortlist.empty()) {
          const auto request = make_memory_match_request(entry.candidate(), base, shortlist, *context.options->prompts);
          const auto &record = journal["records"][memory_review_key(request, context)];
          state = record.str("state");
          if (state != "completed" && state != "unresolved") continue;
        }
        journal["work"][entry.memory_id()] = ManagementJson{{"subject", memory_subject_fingerprint(entry.candidate())}, {"state", state}};
      }
    }
    // 并发写入冲突不能导致重新调用已经受限流约束的提供方。
    context.model_rate_limited = model_rate_limited;
    committed_work = journal["work"];
    planning = true;
    ep::MemoryUpdateResult result = curate_memory_in_place(batch, &planned, &context);
    model_rate_limited = context.model_rate_limited;
    remaining_calls = context.remaining_model_calls;
    requested_total += result.model_requested_count();
    succeeded_total += result.model_succeeded_count();
    failed_total += result.model_failed_count();
    if (planned.ByteSizeLong() > max_file_bytes) {
      throw EvaluationError("evolution memory exceeds configured byte limit");
    }
    ensure_safe_message(planned, secrets);

    bool retry = false;
    {
      MemoryFileLock lock(protobuf_path);
      const ep::EvolutionMemoryStore current = load_memory(protobuf_path, max_file_bytes);
      // GEPA 与训练共用原库；实际内容变化才算并发冲突，map 编码顺序变化不算。
      if (!google::protobuf::util::MessageDifferencer::Equals(current, base)) {
        // 乐观提交失败时，下一次试算不能沿用本次尚未写入的完成标记。
        journal["work"] = committed_work;
        journal["active"] = true;
        retry = true;
      } else {
        result.set_model_requested_count(requested_total);
        result.set_model_succeeded_count(succeeded_total);
        result.set_model_failed_count(failed_total);
        result.set_concurrency_retry_count(attempt);
        result.set_management_duration_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        journal["last_update"] = ManagementJson::parse(protobuf_json(result, false));
        journal["memory_revision"] = std::size_t(planned.revision());
        journal["commit_pending"] = true;
        planning = false;
        checkpoint();
        if (!result.idempotent_replay() ||
            !google::protobuf::util::MessageDifferencer::Equals(planned, base)) {
          write_protobuf(planned, protobuf_path);
        }
        if (!json_path.empty()) {
          try {
            write_bytes_atomic(json_path, to_json(planned, true));
          } catch (const EvaluationError &) {
            result.set_error_code("memory_json_view_write_failed");
          }
        }
        output.result = std::move(result);
        journal["commit_pending"] = false;
        checkpoint();
      }
    }
    if (!retry) {
      return output;
    }
  }
  throw EvaluationError("memory concurrent update retry limit reached");
}

// 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
std::string TrajectoryEvolutionPipeline::to_json(const ep::HybridEvaluationBatchReport &report,
                                                 bool pretty) {
  ensure_safe_message(report, collected_secrets({}));
  return protobuf_json(report, pretty);
}

// 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
std::string TrajectoryEvolutionPipeline::to_json(const ep::EvolutionMemoryStore &memory,
                                                 bool pretty) {
  ensure_safe_message(memory, collected_secrets({}));
  return protobuf_json(memory, pretty);
}

// 按协议格式写出结果，供跨语言读取和持久化回放。
void TrajectoryEvolutionPipeline::write_protobuf(const ep::HybridEvaluationBatchReport &report,
                                                 const std::filesystem::path &path) {
  ensure_safe_message(report, collected_secrets({}));
  std::string bytes;
  if (!report.SerializeToString(&bytes)) {
    throw EvaluationError("failed to serialize batch Protobuf");
  }
  write_bytes_atomic(path, bytes);
}

// 按协议格式写出结果，供跨语言读取和持久化回放。
void TrajectoryEvolutionPipeline::write_protobuf(const ep::EvolutionMemoryStore &memory,
                                                 const std::filesystem::path &path) {
  ensure_safe_message(memory, collected_secrets({}));
  std::string bytes;
  if (!memory.SerializeToString(&bytes)) {
    throw EvaluationError("failed to serialize memory Protobuf");
  }
  write_bytes_atomic(path, bytes);
}

// 返回外部配置载入的元反思提示词，调用处不硬编码业务策略。
std::string TrajectoryEvolutionPipeline::meta_reflection_system_prompt() {
  return load_default_prompt_templates()->meta_reflection_system;
}

// 返回记忆关系审查提示词，供重复与冲突判断复用。
std::string TrajectoryEvolutionPipeline::memory_curator_system_prompt() {
  return load_default_prompt_templates()->memory_curator_system;
}

} // 命名空间结束：agentflow::trajectory
