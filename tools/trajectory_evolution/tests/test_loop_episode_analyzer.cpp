#include "agentflow/trajectory/loop_episode_analyzer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using agentflow::trajectory::AnalysisReport;
using agentflow::trajectory::AnalyzerOptions;
using agentflow::trajectory::InvalidLoopCategory;
using agentflow::trajectory::LoopEpisodeAnalyzer;

void expect(bool condition, const std::string& message) {
  if(!condition) {
    throw std::runtime_error(message);
  }
}

std::string event(
    const std::string& producer,
    int sequence,
    std::uint64_t timestamp_base,
    const std::string& type,
    std::uint64_t round,
    const std::string& node_id,
    const std::string& data) {
  const std::string event_id = producer + ':' + std::to_string(sequence);
  std::string output =
      "{\"schema_version\":2,\"event_id\":\"" + event_id
      + "\",\"idempotency_key\":\"" + event_id
      + "\",\"type\":\"" + type
      + "\",\"producer_id\":\"" + producer
      + "\",\"producer_sequence\":" + std::to_string(sequence)
      + ",\"timestamp_us\":"
      + std::to_string(timestamp_base + static_cast<std::uint64_t>(sequence))
      + ",\"dialogue_id\":\"d1\",\"node_id\":\"" + node_id + "\"";
  if(round > 0) {
    output += ",\"round\":" + std::to_string(round);
  }
  output += ",\"data\":" + data + '}';
  return output;
}

std::filesystem::path write_trace(
    const std::string& name,
    const std::vector<std::string>& lines,
    bool final_newline = true) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path()
      / ("agentflow-loop-" + name + '-' + std::to_string(nonce) + ".jsonl");
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if(!stream) {
    throw std::runtime_error("cannot create test trace");
  }
  for(std::size_t index = 0; index < lines.size(); ++index) {
    stream << lines[index];
    if(index + 1U < lines.size() || final_newline) {
      stream << '\n';
    }
  }
  return path;
}

std::string messages_with(const std::vector<std::string>& tool_call_ids) {
  std::string output = "[";
  for(std::size_t index = 0; index < tool_call_ids.size(); ++index) {
    if(index > 0) {
      output += ',';
    }
    output += "{\"role\":\"tool\",\"tool_call_id\":\""
        + tool_call_ids[index] + "\",\"content\":\"recorded\"}";
  }
  output += ']';
  return output;
}

void append_failed_tool_round(
    std::vector<std::string>& lines,
    int& sequence,
    std::uint64_t round,
    const std::string& model,
    const std::string& call_id,
    const std::string& error,
    const std::vector<std::string>& consumed_ids) {
  constexpr std::uint64_t base = 2000;
  lines.push_back(event(
      "skill", sequence++, base, "llm.request", round, "skill_executor",
      "{\"task_type\":\"skill\",\"skill_name\":\"video-gen\","
      "\"exec_skill_key\":\"exec-video\",\"messages\":"
          + messages_with(consumed_ids) + '}'));
  lines.push_back(event(
      "skill", sequence++, base, "llm.response", round, "skill_executor",
      "{\"skill_name\":\"video-gen\",\"exec_skill_key\":\"exec-video\","
      "\"response\":{\"choices\":[{\"finish_reason\":\"tool_calls\"}]}}"));
  lines.push_back(event(
      "skill", sequence++, base, "loop.control", round, "skill_executor",
      "{\"scope\":\"skill\",\"action\":\"continue\","
      "\"reason\":\"tool_calls_requested\",\"skill_name\":\"video-gen\"}"));
  lines.push_back(event(
      "skill", sequence++, base, "tool.call", round, "skill_executor",
      "{\"skill_name\":\"video-gen\",\"tool_calls\":[{"
      "\"tool_call_id\":\"" + call_id + "\",\"tool_name\":\"generate_video\","
      "\"arguments\":{\"model\":\"" + model
          + "\",\"prompt\":\"same task\"}}]}"));
  lines.push_back(event(
      "skill", sequence++, base, "tool.result", round, "skill_executor",
      "{\"skill_name\":\"video-gen\",\"tool_results\":[{"
      "\"tool_call_id\":\"" + call_id + "\",\"tool_name\":\"generate_video\","
      "\"status\":\"error\",\"result\":{\"status\":\"failed\","
      "\"error\":\"" + error + "\"}}]}"));
}

std::vector<std::string> base_non_retryable_trace() {
  std::vector<std::string> lines = {
      "{\"type\":\"dialogue.header\",\"version\":2,\"dialogue_id\":\"d1\","
      "\"conversation_id\":\"conv1\",\"user_id\":\"user1\"}",
      event("coord1", 1, 1000, "dialogue.start", 0, "coordinator_decide", "{}"),
      event(
          "coord1", 2, 1000, "coordinator.activation", 0,
          "coordinator_decide", "{\"activation_index\":1,\"trigger_type\":\"first\"}"),
      event(
          "coord1", 3, 1000, "llm.request", 1, "coordinator_decide",
          "{\"task_type\":\"coordinator\",\"skill_name\":\"coordinator\","
          "\"activation_index\":1,\"messages\":[]}"),
      event(
          "coord1", 4, 1000, "llm.response", 1, "coordinator_decide",
          "{\"skill_name\":\"coordinator\",\"response\":{\"choices\":[{"
          "\"finish_reason\":\"tool_calls\"}]}}"),
      event(
          "coord1", 5, 1000, "loop.control", 1, "coordinator_decide",
          "{\"scope\":\"coordinator\",\"action\":\"continue\","
          "\"reason\":\"tool_calls_requested\"}"),
      event(
          "coord1", 6, 1000, "skill.dispatch", 1, "coordinator_decide",
          "{\"status\":\"dispatched\",\"batch_id\":\"batch1\","
          "\"skill_names\":[\"video-gen\"],\"dispatches\":[{"
          "\"skill_name\":\"video-gen\",\"prompt\":\"generate a video\","
          "\"force\":false,\"exec_skill_key\":\"exec-video\"}]}"),
      event(
          "skill", 1, 2000, "skill.start", 0, "skill_executor",
          "{\"skill_name\":\"video-gen\",\"exec_skill_key\":\"exec-video\","
          "\"batch_id\":\"batch1\",\"max_rounds\":10}"),
  };

  int sequence = 2;
  append_failed_tool_round(
      lines, sequence, 1, "model-a", "call-1",
      "UnsupportedModel does not support the agent plan request id abcdef1234567890",
      {});
  append_failed_tool_round(
      lines, sequence, 2, "model-b", "call-2",
      "UnsupportedModel does not support the agent plan request id abcdef1234567891",
      {"call-1"});
  append_failed_tool_round(
      lines, sequence, 3, "model-c", "call-3",
      "UnsupportedModel does not support the agent plan request id abcdef1234567892",
      {"call-1", "call-2"});
  append_failed_tool_round(
      lines, sequence, 4, "model-d", "call-4",
      "UnsupportedModel does not support the agent plan request id abcdef1234567893",
      {"call-1", "call-2", "call-3"});
  lines.push_back(event(
      "skill", sequence++, 2000, "llm.request", 5, "skill_executor",
      "{\"task_type\":\"skill\",\"skill_name\":\"video-gen\","
      "\"exec_skill_key\":\"exec-video\",\"messages\":"
          + messages_with({"call-1", "call-2", "call-3", "call-4"}) + '}'));
  lines.push_back(event(
      "skill", sequence++, 2000, "llm.response", 5, "skill_executor",
      "{\"skill_name\":\"video-gen\",\"response\":{\"choices\":[{"
      "\"finish_reason\":\"stop\"}]}}"));
  lines.push_back(event(
      "skill", sequence++, 2000, "loop.control", 5, "skill_executor",
      "{\"scope\":\"skill\",\"action\":\"stop\","
      "\"reason\":\"final_response\",\"skill_name\":\"video-gen\"}"));
  lines.push_back(event(
      "skill", sequence++, 2000, "skill.end", 5, "skill_executor",
      "{\"skill_name\":\"video-gen\",\"exec_skill_key\":\"exec-video\","
      "\"batch_id\":\"batch1\",\"status\":\"tool_error\"}"));

  lines.push_back(event(
      "coord2", 1, 3000, "coordinator.activation", 0,
      "coordinator_decide", "{\"activation_index\":2,\"trigger_type\":"
      "\"skill_completion\",\"batch_id\":\"batch1\"}"));
  lines.push_back(event(
      "coord2", 2, 3000, "batch.result", 0, "coordinator_decide",
      "{\"batch_id\":\"batch1\",\"status\":\"false\","
      "\"workflow_status\":\"completed\",\"successful_count\":0,"
      "\"total_count\":1,\"results\":[{\"exec_skill_key\":\"exec-video\","
      "\"skill_status\":\"tool_error\"}]}"));
  lines.push_back(event(
      "coord2", 3, 3000, "llm.request", 2, "coordinator_decide",
      "{\"task_type\":\"coordinator\",\"skill_name\":\"coordinator\","
      "\"activation_index\":2,\"messages\":[]}"));
  lines.push_back(event(
      "coord2", 4, 3000, "llm.response", 2, "coordinator_decide",
      "{\"skill_name\":\"coordinator\",\"response\":{\"choices\":[{"
      "\"finish_reason\":\"stop\"}]}}"));
  lines.push_back(event(
      "coord2", 5, 3000, "loop.control", 2, "coordinator_decide",
      "{\"scope\":\"coordinator\",\"action\":\"stop\","
      "\"reason\":\"final_response\"}"));
  lines.push_back(event(
      "coord2", 6, 3000, "dialogue.end", 0, "coordinator_decide",
      "{\"status\":\"normal\"}"));
  return lines;
}

AnalysisReport analyze_lines(
    const std::string& name,
    const std::vector<std::string>& lines) {
  const auto path = write_trace(name, lines);
  try {
    const LoopEpisodeAnalyzer analyzer;
    AnalysisReport report = analyzer.analyze_file(path);
    std::filesystem::remove(path);
    return report;
  } catch(...) {
    std::filesystem::remove(path);
    throw;
  }
}

void test_extracts_non_retryable_loop_episodes() {
  const AnalysisReport report = analyze_lines(
      "non-retryable", base_non_retryable_trace());
  expect(report.trace_integrity_valid, "complete trace should be valid");
  expect(
      report.summary.episode_count == 7,
      "expected two coordinator and five skill episodes");
  expect(report.summary.skill_episode_count == 5, "expected five skill episodes");
  expect(
      report.summary.coordinator_episode_count == 2,
      "expected two coordinator episodes");
  expect(
      report.summary.invalid_episode_count == 2,
      "third and fourth failed variants should be invalid");
  expect(
      report.summary.invalid_loop_lower_bound == 2,
      "invalid lower bound must count unique episodes");
  expect(
      report.summary.category_counts.at("non_retryable_repeat") == 2,
      "expected two repeated non-retryable diagnostics");
  expect(
      report.candidate_memories.size() == 1,
      "equivalent diagnostics should merge into one candidate");
  expect(
      report.candidate_memories.front().support_count == 2,
      "candidate support should aggregate");
  expect(
      report.candidate_memories.front().status == "candidate",
      "memory must not auto-promote");
  expect(
      report.candidate_memories.front().requires_validation,
      "candidate must require validation");
  expect(
      report.candidate_memories.front().generator_version
          == "candidate-memory-template-zh-v1",
      "candidate generator must expose the Chinese template version");
  expect(
      report.candidate_memories.front().strategy.find("停止重试")
          != std::string::npos,
      "candidate strategy must use Chinese natural language");
  expect(
      !report.diagnostics.empty()
          && report.diagnostics.front().summary.find("不可重试")
              != std::string::npos,
      "diagnostic summary must use Chinese natural language");
  expect(
      !report.reflection_requests.empty()
          && report.reflection_requests.front().questions.front().find(
                 "阻断条件") != std::string::npos,
      "reflection questions must use Chinese natural language");
  const auto terminal_skill = std::find_if(
      report.episodes.begin(),
      report.episodes.end(),
      [](const auto& episode) {
        return episode.scope == agentflow::trajectory::LoopScope::skill
            && episode.terminal_status == "tool_error";
      });
  expect(
      terminal_skill != report.episodes.end() && terminal_skill->outcome_failed,
      "normally closed Skill business failure must remain a failed outcome");
}

void test_report_does_not_copy_sensitive_payloads() {
  auto lines = base_non_retryable_trace();
  lines[6] = event(
      "coord1", 6, 1000, "skill.dispatch", 1, "coordinator_decide",
      "{\"status\":\"dispatched\",\"batch_id\":\"batch1\","
      "\"skill_names\":[\"video-gen\"],\"dispatches\":[{"
      "\"skill_name\":\"video-gen\",\"prompt\":\"secret-value-sk-test-123\","
      "\"force\":false,\"exec_skill_key\":\"exec-video\"}]}");
  const AnalysisReport report = analyze_lines("privacy", lines);
  const std::string output = LoopEpisodeAnalyzer::to_json(report, false);
  expect(
      output.find("secret-value-sk-test-123") == std::string::npos,
      "analysis output must not copy raw Prompt content");
  expect(
      output.find("UnsupportedModel does not support") == std::string::npos,
      "analysis output must not copy raw Tool errors");
}

void test_single_transient_retry_requires_review_but_is_not_invalid() {
  auto lines = base_non_retryable_trace();
  for(std::string& line : lines) {
    const std::string unsupported =
        "UnsupportedModel does not support the agent plan request id abcdef123456789";
    const std::size_t position = line.find(unsupported);
    if(position != std::string::npos) {
      line.replace(
          position,
          unsupported.size() + 1,
          "network timeout while contacting provider");
    }
  }
  const AnalysisReport report = analyze_lines("transient", lines);
  expect(
      report.summary.invalid_episode_count >= 1,
      "four identical transient outcomes should eventually be marked as no progress");

  auto short_lines = base_non_retryable_trace();
  short_lines.erase(
      std::remove_if(
          short_lines.begin(),
          short_lines.end(),
          [](const std::string& line) {
            return line.find("\"producer_id\":\"skill\"") != std::string::npos
                && (line.find("\"round\":3") != std::string::npos
                    || line.find("\"round\":4") != std::string::npos);
          }),
      short_lines.end());
  for(std::string& line : short_lines) {
    const std::string alternate_model = "\"model\":\"model-b\"";
    const std::size_t model_position = line.find(alternate_model);
    if(model_position != std::string::npos) {
      line.replace(
          model_position,
          alternate_model.size(),
          "\"model\":\"model-a\"");
    }
    const std::string marker = "UnsupportedModel does not support the agent plan";
    const std::size_t position = line.find(marker);
    if(position != std::string::npos) {
      const std::size_t end = line.find("request id", position);
      line.replace(
          position,
          end == std::string::npos ? marker.size() : end - position,
          "network timeout ");
    }
  }
  // Removing rounds from a pre-numbered trace intentionally creates sequence
  // gaps (warnings only); the remaining exact transient retry is not invalid.
  const AnalysisReport short_report = analyze_lines("one-transient-retry", short_lines);
  expect(
      short_report.summary.invalid_episode_count == 0,
      "one bounded transient retry must not count as an invalid loop");
  expect(
      short_report.summary.review_episode_count >= 1,
      "one transient retry should remain visible for review");
}

void test_missing_tool_result_fails_strict_integrity() {
  auto lines = base_non_retryable_trace();
  lines.erase(
      std::remove_if(
          lines.begin(),
          lines.end(),
          [](const std::string& line) {
            return line.find("\"event_id\":\"skill:6\"") != std::string::npos;
          }),
      lines.end());
  const auto path = write_trace("missing-result", lines);
  const LoopEpisodeAnalyzer analyzer;
  const AnalysisReport report = analyzer.analyze_file(path);
  expect(!report.trace_integrity_valid, "missing Tool result must invalidate trace integrity");
  expect(
      std::any_of(
          report.trace_issues.begin(),
          report.trace_issues.end(),
          [](const auto& issue) {
            return issue.code == "tool_call_without_result";
          }),
      "missing Tool result issue was not reported");
  expect(
      report.reflection_requests.empty()
          && report.candidate_memories.empty(),
      "invalid traces must be quarantined from evolution inputs");

  AnalyzerOptions options;
  options.strict_trace_integrity = true;
  bool threw = false;
  try {
    (void)analyzer.analyze_file(path, options);
  } catch(const agentflow::trajectory::AnalysisError&) {
    threw = true;
  }
  std::filesystem::remove(path);
  expect(threw, "strict mode must reject an incomplete trace");
}

void test_duplicate_coordinator_dispatch_is_invalid() {
  std::vector<std::string> lines = {
      "{\"type\":\"dialogue.header\",\"version\":2,\"dialogue_id\":\"d1\"}",
      event("c1", 1, 1000, "dialogue.start", 0, "coordinator_decide", "{}"),
      event(
          "c1", 2, 1000, "coordinator.activation", 0,
          "coordinator_decide", "{\"activation_index\":1}"),
      event(
          "c1", 3, 1000, "llm.request", 1, "coordinator_decide",
          "{\"task_type\":\"coordinator\",\"activation_index\":1,"
          "\"messages\":[]}"),
      event("c1", 4, 1000, "llm.response", 1, "coordinator_decide", "{}"),
      event(
          "c1", 5, 1000, "loop.control", 1, "coordinator_decide",
          "{\"action\":\"continue\",\"reason\":\"tool_calls_requested\"}"),
      event(
          "c1", 6, 1000, "skill.dispatch", 1, "coordinator_decide",
          "{\"batch_id\":\"b1\",\"dispatches\":[{\"skill_name\":\"echo\","
          "\"prompt\":\"same\",\"force\":false}]}"),
      event(
          "c2", 1, 2000, "coordinator.activation", 0,
          "coordinator_decide", "{\"activation_index\":2,\"batch_id\":\"b1\"}"),
      event(
          "c2", 2, 2000, "batch.result", 0, "coordinator_decide",
          "{\"batch_id\":\"b1\",\"status\":\"true\","
          "\"workflow_status\":\"completed\",\"successful_count\":1,"
          "\"total_count\":1}"),
      event(
          "c2", 3, 2000, "llm.request", 2, "coordinator_decide",
          "{\"task_type\":\"coordinator\",\"activation_index\":2,"
          "\"messages\":[]}"),
      event("c2", 4, 2000, "llm.response", 2, "coordinator_decide", "{}"),
      event(
          "c2", 5, 2000, "loop.control", 2, "coordinator_decide",
          "{\"action\":\"continue\",\"reason\":\"tool_calls_requested\"}"),
      event(
          "c2", 6, 2000, "skill.dispatch", 2, "coordinator_decide",
          "{\"batch_id\":\"b2\",\"dispatches\":[{\"skill_name\":\"echo\","
          "\"prompt\":\"same\",\"force\":false}]}"),
      event(
          "c3", 1, 3000, "coordinator.activation", 0,
          "coordinator_decide", "{\"activation_index\":3,\"batch_id\":\"b2\"}"),
      event(
          "c3", 2, 3000, "batch.result", 0, "coordinator_decide",
          "{\"batch_id\":\"b2\",\"status\":\"true\","
          "\"workflow_status\":\"completed\",\"successful_count\":1,"
          "\"total_count\":1}"),
      event(
          "c3", 3, 3000, "llm.request", 3, "coordinator_decide",
          "{\"task_type\":\"coordinator\",\"activation_index\":3,"
          "\"messages\":[]}"),
      event("c3", 4, 3000, "llm.response", 3, "coordinator_decide", "{}"),
      event(
          "c3", 5, 3000, "loop.control", 3, "coordinator_decide",
          "{\"action\":\"stop\",\"reason\":\"final_response\"}"),
      event(
          "c3", 6, 3000, "dialogue.end", 0,
          "coordinator_decide", "{\"status\":\"normal\"}"),
  };
  const AnalysisReport report = analyze_lines("duplicate-dispatch", lines);
  expect(
      report.summary.category_counts.at("invalid_action_repeat") == 1,
      "same Skill and Prompt should produce one duplicate dispatch diagnostic");
  expect(report.summary.invalid_episode_count == 1, "duplicate dispatch should count once");
}

void test_semantic_duplicates_and_conflicts_are_distinguished() {
  auto lines = base_non_retryable_trace();
  const std::string original = lines[1];
  lines.insert(lines.begin() + 2, "  " + original + "  ");

  std::string conflicting = original;
  const std::string empty_data = "\"data\":{}";
  const std::size_t data_position = conflicting.find(empty_data);
  expect(data_position != std::string::npos, "fixture must contain empty data");
  conflicting.replace(
      data_position,
      empty_data.size(),
      "\"data\":{\"changed\":true}");
  lines.insert(lines.begin() + 3, conflicting);

  const AnalysisReport report = analyze_lines("duplicates", lines);
  expect(report.summary.duplicate_event_count == 2, "both duplicate records must be counted");
  expect(!report.trace_integrity_valid, "conflicting duplicate must invalidate integrity");
  expect(
      std::any_of(
          report.trace_issues.begin(),
          report.trace_issues.end(),
          [](const auto& issue) {
            return issue.code == "duplicate_event"
                && issue.severity == "warning";
          }),
      "whitespace-only duplicate must be treated as semantically exact");
  expect(
      std::any_of(
          report.trace_issues.begin(),
          report.trace_issues.end(),
          [](const auto& issue) {
            return issue.code == "conflicting_duplicate_event"
                && issue.severity == "error";
          }),
      "same idempotency key with changed data must be a conflict");
}

void test_conflicting_header_identity_is_not_overwritten() {
  auto lines = base_non_retryable_trace();
  lines.insert(
      lines.begin() + 1,
      "{\"type\":\"dialogue.header\",\"version\":2,"
      "\"dialogue_id\":\"other-dialogue\","
      "\"conversation_id\":\"other-conversation\","
      "\"user_id\":\"other-user\"}");
  const AnalysisReport report = analyze_lines("header-conflict", lines);
  expect(!report.trace_integrity_valid, "conflicting header identity must be invalid");
  expect(report.dialogue_id == "d1", "first Dialogue identity must be retained");
  expect(report.conversation_id == "conv1", "first Conversation identity must be retained");
  expect(report.user_id == "user1", "first User identity must be retained");
  expect(
      std::any_of(
          report.trace_issues.begin(),
          report.trace_issues.end(),
          [](const auto& issue) {
            return issue.code == "conflicting_conversation_id";
          }),
      "Conversation conflict must be explicit");
  expect(
      std::any_of(
          report.trace_issues.begin(),
          report.trace_issues.end(),
          [](const auto& issue) {
            return issue.code == "conflicting_user_id";
          }),
      "User conflict must be explicit");
}

void test_missing_required_event_metadata_is_explicit() {
  auto lines = base_non_retryable_trace();
  std::string& start = lines[1];
  for(const std::string& field : {
          std::string("\"type\":\"dialogue.start\","),
          std::string("\"producer_id\":\"coord1\","),
          std::string("\"producer_sequence\":1,"),
          std::string("\"timestamp_us\":1001,"),
      }) {
    const std::size_t position = start.find(field);
    expect(position != std::string::npos, "metadata fixture field must exist");
    start.erase(position, field.size());
  }
  const AnalysisReport report = analyze_lines("missing-metadata", lines);
  expect(!report.trace_integrity_valid, "missing required metadata must be invalid");
  for(const std::string& code : {
          std::string("missing_event_type"),
          std::string("missing_producer_id"),
          std::string("missing_producer_sequence"),
          std::string("missing_timestamp"),
      }) {
    expect(
        std::any_of(
            report.trace_issues.begin(),
            report.trace_issues.end(),
            [&code](const auto& issue) {
              return issue.code == code;
            }),
        "required metadata issue must be explicit: " + code);
  }
  expect(
      report.candidate_memories.empty(),
      "metadata-invalid trace must not produce candidate memory");
}

void test_torn_tail_is_tolerated_but_complete_bad_record_is_rejected() {
  auto lines = base_non_retryable_trace();
  lines.push_back("{\"type\":");
  const auto torn_path = write_trace("torn-tail", lines, false);
  const LoopEpisodeAnalyzer analyzer;
  const AnalysisReport report = analyzer.analyze_file(torn_path);
  std::filesystem::remove(torn_path);
  expect(report.trace_integrity_valid, "one torn tail record should be warning-only");
  expect(
      std::any_of(
          report.trace_issues.begin(),
          report.trace_issues.end(),
          [](const auto& issue) {
            return issue.code == "torn_tail_record";
          }),
      "torn tail warning must remain observable");

  const auto bad_path = write_trace("bad-record", lines);
  bool threw = false;
  try {
    (void)analyzer.analyze_file(bad_path);
  } catch(const agentflow::trajectory::AnalysisError&) {
    threw = true;
  }
  std::filesystem::remove(bad_path);
  expect(threw, "newline-terminated malformed JSON must not be ignored as a torn tail");
}

void test_only_tool_role_messages_count_as_consumed_results() {
  auto lines = base_non_retryable_trace();
  for(std::string& line : lines) {
    if(line.find("\"event_id\":\"skill:7\"") == std::string::npos) {
      continue;
    }
    const std::string tool_role = "\"role\":\"tool\"";
    const std::size_t role_position = line.find(tool_role);
    expect(role_position != std::string::npos, "round two request must contain a Tool message");
    line.replace(role_position, tool_role.size(), "\"role\":\"assistant\"");
  }
  const AnalysisReport report = analyze_lines("false-consumption", lines);
  expect(
      std::any_of(
          report.diagnostics.begin(),
          report.diagnostics.end(),
          [](const auto& diagnostic) {
            return diagnostic.category
                == InvalidLoopCategory::result_not_consumed;
          }),
      "a correlation id outside a Tool-role message must not hide a missing result");
}

void test_success_resets_non_retryable_failure_streak() {
  auto lines = base_non_retryable_trace();
  for(std::string& line : lines) {
    if(line.find("\"event_id\":\"skill:16\"") == std::string::npos) {
      continue;
    }
    line = event(
        "skill", 16, 2000, "tool.result", 3, "skill_executor",
        "{\"skill_name\":\"video-gen\",\"tool_results\":[{"
        "\"tool_call_id\":\"call-3\",\"tool_name\":\"generate_video\","
        "\"status\":\"ok\",\"result\":{\"status\":\"succeeded\"}}]}");
  }
  const AnalysisReport report = analyze_lines("success-reset", lines);
  expect(
      std::none_of(
          report.diagnostics.begin(),
          report.diagnostics.end(),
          [](const auto& diagnostic) {
            return diagnostic.category
                == InvalidLoopCategory::non_retryable_repeat;
          }),
      "a successful Tool outcome must reset the stable failure streak");
}

void test_evaluation_is_attached_without_copying_details() {
  auto lines = base_non_retryable_trace();
  lines.push_back(event(
      "evaluator", 1, 4000, "evaluation.result", 4, "offline_evaluator",
      "{\"evaluator\":\"trajectory-quality\",\"evaluator_version\":\"v3\","
      "\"metric\":\"task_success\","
      "\"score\":0.75,\"passed\":false,"
      "\"target_event_ids\":[\"skill:20\",\"skill:21\"],"
      "\"details\":{\"reason\":\"private-evaluation-detail\"}}"));
  const AnalysisReport report = analyze_lines("evaluation", lines);
  const auto episode = std::find_if(
      report.episodes.begin(),
      report.episodes.end(),
      [](const auto& candidate) {
        return candidate.round == 4
            && candidate.scope == agentflow::trajectory::LoopScope::skill;
      });
  expect(episode != report.episodes.end(), "round four Skill episode must exist");
  expect(
      episode->evaluations.size() == 1,
      "one evaluation targeting two events in one Episode must attach once");
  expect(
      episode->evaluations.front().evaluator_version == "v3",
      "evaluator version must be preserved for reproducibility");
  expect(episode->evaluations.front().has_score, "evaluation score presence must be preserved");
  expect(episode->evaluations.front().score == 0.75, "evaluation score must be preserved");
  expect(episode->evaluations.front().has_passed, "evaluation verdict presence must be preserved");
  expect(!episode->evaluations.front().passed, "failed evaluation verdict must be preserved");
  const std::string output = LoopEpisodeAnalyzer::to_json(report, false);
  expect(
      output.find("private-evaluation-detail") == std::string::npos,
      "free-form evaluation details must not be copied into analysis output");
  expect(report.fingerprint_algorithm == "sha256-v1", "fingerprint contract must be versioned");
  expect(
      report.analyzer_version == "loop-episode-analyzer-v1.1",
      "analyzer implementation version must be explicit");
  expect(
      report.rule_set_version == "invalid-loop-rules-v1",
      "invalid-loop rule-set version must be explicit");
  expect(
      episode->action_fingerprint.size() == 64,
      "SHA-256 fingerprints must contain 64 lowercase hexadecimal characters");
}

}  // namespace

int main() {
  try {
    test_extracts_non_retryable_loop_episodes();
    test_report_does_not_copy_sensitive_payloads();
    test_single_transient_retry_requires_review_but_is_not_invalid();
    test_missing_tool_result_fails_strict_integrity();
    test_duplicate_coordinator_dispatch_is_invalid();
    test_semantic_duplicates_and_conflicts_are_distinguished();
    test_conflicting_header_identity_is_not_overwritten();
    test_missing_required_event_metadata_is_explicit();
    test_torn_tail_is_tolerated_but_complete_bad_record_is_rejected();
    test_only_tool_role_messages_count_as_consumed_results();
    test_success_resets_non_retryable_failure_streak();
    test_evaluation_is_attached_without_copying_details();
    std::cout << "All trajectory loop analyzer tests passed.\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
