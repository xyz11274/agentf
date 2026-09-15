#pragma once

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

#include "hybrid_evaluation.pb.h"

namespace agentflow::trajectory {

// A generation contract, not semantic approval or evidence of task benefit.
inline constexpr char kScopedExperienceGeneratorVersion[] = "scoped-experience-generator-v1";

inline bool experience_text_valid(const std::string& value, std::size_t limit) {
  return !value.empty() && value.size() <= limit
      && std::any_of(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); })
      && std::none_of(value.begin(), value.end(), [](unsigned char c) {
           return (c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127;
         });
}

inline bool experience_fields_valid(const std::string& scope, const std::string& trigger,
                                    const std::string& change) {
  if(!experience_text_valid(scope, 512) || !experience_text_valid(trigger, 2048)
     || !experience_text_valid(change, 2048)) return false;
  std::string normalized;
  for(unsigned char c : trigger) {
    if(!std::isspace(c)) normalized += static_cast<char>(std::tolower(c));
  }
  // Source labels and diagnostic encodings are not applicability conditions.
  return normalized != "validated_dialogue_reflection" && normalized != "unknown"
      && normalized != "none" && normalized != "n/a" && normalized.rfind("category=", 0) != 0;
}

inline std::set<std::string> experience_scopes(const evaluation::HybridEvaluationReport& report) {
  std::set<std::string> scopes;
  for(const auto& assessment : report.assessments()) {
    const auto& evidence = assessment.evidence();
    if(evidence.scope().rfind("agent:", 0) == 0 && evidence.scope() != "agent:unknown") {
      scopes.insert(evidence.scope());
    } else if(evidence.scope() == "skill" && !evidence.skill_name().empty()) {
      scopes.insert("skill:" + evidence.skill_name());
    } else if(evidence.scope() == "coordinator") {
      scopes.insert("coordinator");
    }
  }
  return scopes;
}

inline bool is_scoped_experience(const evaluation::EvolutionCandidate& candidate) {
  return candidate.target_type() == evaluation::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION
      && candidate.generator_version() == kScopedExperienceGeneratorVersion;
}

inline bool same_experience_contract(const evaluation::EvolutionCandidate& left,
                                     const evaluation::EvolutionCandidate& right) {
  return is_scoped_experience(left) == is_scoped_experience(right);
}

}  // namespace agentflow::trajectory
