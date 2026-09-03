#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace agentflow::trajectory {

enum class LoopScope {
  unknown,
  coordinator,
  skill,
};

enum class InvalidLoopCategory {
  exact_repeat,
  no_state_progress,
  non_retryable_repeat,
  result_not_consumed,
  invalid_action_repeat,
  state_oscillation,
  max_rounds_exhausted,
  review_required,
};

struct AnalyzerOptions {
  std::size_t max_file_bytes = 256U * 1024U * 1024U;
  std::size_t max_line_bytes = 32U * 1024U * 1024U;
  bool strict_trace_integrity = false;
};

struct TraceIssue {
  std::string issue_id;
  std::string severity;
  std::string code;
  std::string message;
  std::vector<std::string> evidence_event_ids;
};

struct ToolActionSummary {
  std::string tool_call_id;
  std::string tool_name;
  std::string arguments_fingerprint;
  std::string status;
  std::string result_fingerprint;
  std::string error_class;
  std::string retryability;
  std::string call_event_id;
  std::string result_event_id;
};

struct DispatchSummary {
  std::string batch_id;
  std::vector<std::string> skill_names;
  std::vector<std::string> prompt_fingerprints;
  std::vector<bool> force;
  std::string batch_status;
  std::string workflow_status;
  std::size_t successful_count = 0;
  std::size_t total_count = 0;
};

struct EvaluationSummary {
  std::string event_id;
  std::string evaluator;
  std::string evaluator_version;
  std::string metric;
  double score = 0.0;
  bool has_score = false;
  bool passed = false;
  bool has_passed = false;
};

struct LoopEpisode {
  std::string episode_id;
  std::string dialogue_id;
  LoopScope scope = LoopScope::unknown;
  std::string owner_id;
  std::string skill_name;
  std::string exec_skill_key;
  std::uint64_t activation_index = 0;
  std::uint64_t round = 0;
  std::uint64_t attempt_index = 0;
  std::uint64_t step_index = 0;
  std::uint64_t started_at_us = 0;
  std::uint64_t ended_at_us = 0;
  std::string request_event_id;
  std::string response_event_id;
  std::string control_event_id;
  std::string action;
  std::string reason;
  bool continued = false;
  bool terminal = false;
  std::string terminal_status;
  std::string input_fingerprint;
  std::string action_fingerprint;
  std::string outcome_fingerprint;
  std::string outcome_class_fingerprint;
  bool outcome_failed = false;
  bool all_tool_results_failed = false;
  bool has_non_retryable_failure = false;
  DispatchSummary dispatch;
  std::vector<ToolActionSummary> tool_actions;
  std::vector<std::string> input_tool_call_ids;
  std::vector<std::string> source_event_ids;
  std::vector<EvaluationSummary> evaluations;
  std::vector<std::string> diagnostic_ids;
};

struct LoopDiagnostic {
  std::string diagnostic_id;
  std::string episode_id;
  InvalidLoopCategory category = InvalidLoopCategory::review_required;
  bool counts_as_invalid = false;
  double confidence = 0.0;
  std::string severity;
  std::string summary;
  std::string observable_cause;
  std::string tool_name;
  std::string error_class;
  std::vector<std::string> evidence_event_ids;
  std::string strategy_key;
};

struct ReflectionRequest {
  std::string reflection_id;
  std::string diagnostic_id;
  std::string episode_id;
  std::string scope;
  std::string target;
  std::string observation;
  std::vector<std::string> evidence_event_ids;
  std::vector<std::string> questions;
  std::vector<std::string> constraints;
};

struct CandidateMemory {
  std::string memory_id;
  std::string status = "candidate";
  std::string generator_version = "candidate-memory-template-zh-v1";
  std::string scope;
  std::string target;
  std::string category;
  std::string applicable_condition;
  std::string strategy;
  std::string risk;
  double confidence = 0.0;
  std::size_t support_count = 0;
  bool requires_validation = true;
  std::vector<std::string> source_diagnostic_ids;
  std::vector<std::string> source_episode_ids;
  std::vector<std::string> evidence_event_ids;
};

struct AnalysisSummary {
  std::size_t event_count = 0;
  std::size_t duplicate_event_count = 0;
  std::size_t episode_count = 0;
  std::size_t coordinator_episode_count = 0;
  std::size_t skill_episode_count = 0;
  std::size_t invalid_episode_count = 0;
  std::size_t review_episode_count = 0;
  std::size_t invalid_loop_lower_bound = 0;
  std::map<std::string, std::size_t> category_counts;
};

struct AnalysisReport {
  std::uint32_t schema_version = 1;
  std::string analysis_kind = "loop_episode_analysis";
  std::string analyzer_version = "loop-episode-analyzer-v1.1";
  std::string rule_set_version = "invalid-loop-rules-v1";
  std::string source_path;
  std::string dialogue_id;
  std::string conversation_id;
  std::string user_id;
  std::string fingerprint_algorithm = "sha256-v1";
  bool trace_integrity_valid = true;
  AnalysisSummary summary;
  std::vector<TraceIssue> trace_issues;
  std::vector<LoopEpisode> episodes;
  std::vector<LoopDiagnostic> diagnostics;
  std::vector<ReflectionRequest> reflection_requests;
  std::vector<CandidateMemory> candidate_memories;
};

class AnalysisError : public std::runtime_error {
 public:
  explicit AnalysisError(const std::string& message)
      : std::runtime_error(message) {}
};

class LoopEpisodeAnalyzer {
 public:
  AnalysisReport analyze_file(
      const std::filesystem::path& path,
      const AnalyzerOptions& options = AnalyzerOptions()) const;

  static std::string to_json(const AnalysisReport& report, bool pretty = true);
};

std::string to_string(LoopScope scope);
std::string to_string(InvalidLoopCategory category);

}  // namespace agentflow::trajectory
