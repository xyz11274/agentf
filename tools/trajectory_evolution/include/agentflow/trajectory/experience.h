// 公开接口：定义宿主接入所需的类型、配置和调用约定。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

#include "hybrid_evaluation.pb.h"

namespace agentflow::trajectory {

// 这里只是生成契约，不表示语义已批准，也不是任务收益证据。
inline constexpr char kScopedExperienceGeneratorVersion[] = "scoped-experience-generator-v1";

// 检查经验文本的长度和非空约束，拒绝不可用注入内容。
inline bool experience_text_valid(const std::string &value, std::size_t limit) {
  return !value.empty() && value.size() <= limit &&
         std::any_of(value.begin(), value.end(),
                     [](unsigned char c) { return !std::isspace(c); }) &&
         std::none_of(value.begin(), value.end(), [](unsigned char c) {
           return (c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127;
         });
}

// 检查经验目标、内容、证据及作用域字段是否符合生成契约。
inline bool experience_fields_valid(const std::string &scope, const std::string &trigger,
                                    const std::string &change) {
  if (!experience_text_valid(scope, 512) || !experience_text_valid(trigger, 2048) ||
      !experience_text_valid(change, 2048))
    return false;
  std::string normalized;
  for (unsigned char c : trigger) {
    if (!std::isspace(c))
      normalized += static_cast<char>(std::tolower(c));
  }
  // 来源标签和诊断编码不能直接作为适用条件。
  return normalized != "validated_dialogue_reflection" && normalized != "unknown" &&
         normalized != "none" && normalized != "n/a" && normalized.rfind("category=", 0) != 0;
}

// 提取经验声明的作用范围，用于编译与权限过滤。
inline std::set<std::string> experience_scopes(const evaluation::HybridEvaluationReport &report) {
  std::set<std::string> scopes;
  for (const auto &assessment : report.assessments()) {
    const auto &evidence = assessment.evidence();
    if (evidence.scope().rfind("agent:", 0) == 0 && evidence.scope() != "agent:unknown") {
      scopes.insert(evidence.scope());
    } else if (evidence.scope() == "skill" && !evidence.skill_name().empty()) {
      scopes.insert("skill:" + evidence.skill_name());
    } else if (evidence.scope() == "coordinator") {
      scopes.insert("coordinator");
    }
  }
  return scopes;
}

// 判断经验是否具备可执行的明确接收对象和适用范围。
inline bool is_scoped_experience(const evaluation::EvolutionCandidate &candidate) {
  return candidate.target_type() == evaluation::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION &&
         candidate.generator_version() == kScopedExperienceGeneratorVersion;
}

// 比较两条经验的结构契约，不能只因文字相近就跨作用域合并。
inline bool same_experience_contract(const evaluation::EvolutionCandidate &left,
                                     const evaluation::EvolutionCandidate &right) {
  return is_scoped_experience(left) == is_scoped_experience(right) &&
         (left.generator_version() == kScopedExperienceGeneratorVersion) ==
             (right.generator_version() == kScopedExperienceGeneratorVersion);
}

} // 命名空间结束：agentflow::trajectory
