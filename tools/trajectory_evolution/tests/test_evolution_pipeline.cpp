#include "agentflow/trajectory/evolution_pipeline.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace trajectory = agentflow::trajectory;
namespace ep = agentflow::trajectory::evaluation;

void expect(bool condition, const std::string& message) {
  if(!condition) {
    throw std::runtime_error(message);
  }
}

ep::HybridEvaluationReport make_rule_report(
    const std::string& user,
    const std::string& conversation,
    const std::string& dialogue,
    const std::string& episode,
    ep::SemanticVerdict verdict,
    bool with_diagnostic = true,
    bool trace_valid = true) {
  ep::HybridEvaluationReport report;
  report.set_schema_version(1);
  report.set_evaluation_kind("test");
  report.set_evaluator_version("test-v1");
  report.set_source_fingerprint("source-" + dialogue);
  report.mutable_source()->set_user_id(user);
  report.mutable_source()->set_conversation_id(conversation);
  report.mutable_source()->set_dialogue_id(dialogue);
  report.set_source_trace_integrity_valid(trace_valid);
  report.mutable_dialogue_assessment()->set_status(
      ep::MODEL_CALL_STATUS_NOT_REQUESTED);

  auto* assessment = report.add_assessments();
  auto* packet = assessment->mutable_evidence();
  packet->set_episode_id(episode);
  packet->set_scope("skill");
  packet->set_owner_id("exec-video");
  packet->set_skill_name("video-gen");
  *packet->add_source_event_ids() = "event-" + episode;
  auto* tool = packet->add_tools();
  tool->set_tool_call_id("call-" + episode);
  tool->set_tool_name("generate_video");
  tool->set_error_class("unsupported_model");
  tool->set_retryability("non_retryable");
  tool->set_call_event_id("event-" + episode);
  tool->set_result_event_id("result-" + episode);
  if(with_diagnostic) {
    auto* diagnostic = packet->add_diagnostics();
    diagnostic->set_diagnostic_id("diagnostic-" + episode);
    diagnostic->set_category("non_retryable_repeat");
    diagnostic->set_counts_as_invalid(true);
    diagnostic->set_confidence(0.96);
    diagnostic->set_summary("不支持的模型被重复调用。" );
    diagnostic->set_observable_cause("同类非重试错误再次出现。" );
    diagnostic->set_tool_name("generate_video");
    diagnostic->set_error_class("unsupported_model");
    *diagnostic->add_evidence_event_ids() = "event-" + episode;
  }
  assessment->mutable_rule()->set_verdict(
      with_diagnostic
          ? ep::SEMANTIC_VERDICT_INVALID
          : ep::SEMANTIC_VERDICT_UNASSESSED);
  assessment->set_final_verdict(verdict);
  assessment->set_final_confidence(0.9);
  return report;
}

ep::HybridEvaluationReport make_reflection_report(
    const std::string& user,
    const std::string& conversation,
    const std::string& dialogue,
    const std::string& episode,
    const std::string& proposal) {
  ep::HybridEvaluationReport report = make_rule_report(
      user,
      conversation,
      dialogue,
      episode,
      ep::SEMANTIC_VERDICT_USEFUL,
      false);
  report.mutable_dialogue_assessment()->set_status(
      ep::MODEL_CALL_STATUS_SUCCEEDED);
  report.mutable_dialogue_assessment()->set_confidence(0.88);
  report.mutable_reflection()->set_summary("当前 Skill Prompt 缺少失败约束。" );
  *report.mutable_reflection()->add_root_causes() = "没有约束重复失败后的行为。";
  *report.mutable_reflection()->add_prompt_changes() = proposal;
  *report.mutable_reflection()->add_wasted_episode_ids() = episode;
  *report.mutable_reflection()->add_evidence_event_ids() = "event-" + episode;
  return report;
}

class MetaAdapter final : public trajectory::ModelAdapter {
 public:
  explicit MetaAdapter(std::string proposal =
      "在提示词中明确非重试错误必须停止并重新规划。")
      : proposal_(proposal) {}

  trajectory::RawModelResponse evaluate(
      const trajectory::ModelRequest& request) override {
    ++calls;
    if(request.user_prompt.find("\"compatibility_group\":")
       == std::string::npos) {
      throw std::runtime_error("meta fixture did not receive compatibility groups");
    }
    if(request.user_prompt.find("\"allowed_seed_groups\":")
       == std::string::npos) {
      throw std::runtime_error("meta fixture did not receive the seed whitelist");
    }
    std::vector<std::string> seed_ids;
    const std::regex pattern(R"REGEX("seed_id":"([^"]+)")REGEX");
    for(std::sregex_iterator iterator(
            request.user_prompt.begin(), request.user_prompt.end(), pattern),
        end;
        iterator != end;
        ++iterator) {
      seed_ids.push_back((*iterator)[1].str());
    }
    if(seed_ids.size() < 2U || request.allowed_event_ids.empty()) {
      throw std::runtime_error("meta fixture did not receive enough evidence");
    }
    std::string seeds;
    for(std::size_t index = 0; index < seed_ids.size(); ++index) {
      if(index > 0U) {
        seeds.push_back(',');
      }
      seeds.append("\"").append(seed_ids[index]).append("\"");
    }
    trajectory::RawModelResponse response;
    response.transport_ok = true;
    response.adapter = name();
    response.model_id = model_id();
    response.attempts = 1;
    response.body =
        "{\"candidate_groups\":[{\"seed_ids\":[" + seeds
        + "],\"trigger_pattern\":\"同一技能反复遇到确定性失败时\","
          "\"observed_problem\":\"失败后缺少统一的停止约束。\","
          "\"proposed_change\":\"" + proposal_ + "\","
          "\"expected_benefit\":\"减少无效循环并保留可恢复错误的正常重试。\","
          "\"confidence\":0.93,\"evidence_event_ids\":[\""
        + request.allowed_event_ids.front() + "\"]}]}";
    return response;
  }

  std::string name() const override {
    return "meta-test";
  }

  std::string model_id() const override {
    return "meta-model";
  }

  std::string proposal_;
  int calls = 0;
};

class TempDirectory {
 public:
  TempDirectory() {
    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path()
        / ("agentflow-evolution-test-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

std::string event(
    int sequence,
    const std::string& type,
    std::uint64_t round,
    const std::string& data) {
  const std::string id = "clean:" + std::to_string(sequence);
  std::string output =
      "{\"schema_version\":2,\"event_id\":\"" + id
      + "\",\"idempotency_key\":\"" + id
      + "\",\"type\":\"" + type
      + "\",\"producer_id\":\"clean\",\"producer_sequence\":"
      + std::to_string(sequence)
      + ",\"timestamp_us\":" + std::to_string(1000 + sequence)
      + ",\"dialogue_id\":\"d-clean\","
        "\"node_id\":\"coordinator_decide\"";
  if(round > 0U) {
    output += ",\"round\":" + std::to_string(round);
  }
  return output + ",\"data\":" + data + '}';
}

void write_lines(
    const std::filesystem::path& path,
    const std::vector<std::string>& lines) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if(!output) {
    throw std::runtime_error("cannot create SessionLog fixture");
  }
  for(const auto& line : lines) {
    output << line << '\n';
  }
}

void test_cross_dialogue_gate_and_contradiction() {
  trajectory::EvolutionOptions options;
  options.min_supporting_dialogues = 2;
  options.min_supporting_episodes = 2;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, options);
  const auto first = make_rule_report(
      "user-a", "conv-a", "dialogue-a", "episode-a",
      ep::SEMANTIC_VERDICT_INVALID);
  const auto second = make_rule_report(
      "user-b", "conv-b", "dialogue-b", "episode-b",
      ep::SEMANTIC_VERDICT_INVALID);
  auto output = pipeline.synthesize_reports({first, second});
  expect(output.report.candidates_size() == 1, "same rule must aggregate");
  const auto& candidate = output.report.candidates(0);
  expect(
      candidate.status() == ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED,
      "two-dialogue evidence should pass the gate");
  expect(candidate.support_dialogue_count() == 2, "dialogue support is wrong");
  expect(candidate.invalid_episode_count() == 2, "invalid support is wrong");

  const auto contradiction = make_rule_report(
      "user-c", "conv-c", "dialogue-c", "episode-c",
      ep::SEMANTIC_VERDICT_NECESSARY_RETRY,
      false);
  output = pipeline.synthesize_reports({first, second, contradiction});
  expect(
      output.report.candidates(0).status()
          == ep::EVOLUTION_CANDIDATE_STATUS_CONFLICTED,
      "necessary retry with the same error must become contradiction evidence");
  expect(
      output.report.candidates(0).contradiction_count() == 1,
      "contradiction count is wrong");
}

void test_composite_source_identity_has_boundaries() {
  trajectory::EvolutionOptions options;
  options.min_supporting_dialogues = 2;
  options.min_supporting_episodes = 2;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, options);
  const auto first = make_rule_report(
      "ab", "c", "d", "episode-a",
      ep::SEMANTIC_VERDICT_INVALID);
  const auto second = make_rule_report(
      "a", "bc", "d", "episode-b",
      ep::SEMANTIC_VERDICT_INVALID);
  const auto output = pipeline.synthesize_reports({first, second});
  expect(output.report.candidates_size() == 1, "candidate aggregation failed");
  expect(
      output.report.candidates(0).support_conversation_count() == 2,
      "composite conversation identities collided");
  expect(
      output.report.candidates(0).support_dialogue_count() == 2,
      "composite dialogue identities collided");
  expect(
      output.report.candidates(0).status()
          == ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED,
      "identity collision incorrectly blocked the support gate");
}

void test_invalid_trace_is_quarantined() {
  trajectory::TrajectoryEvolutionPipeline pipeline;
  const auto invalid = make_rule_report(
      "user-a", "conv-a", "dialogue-invalid", "episode-invalid",
      ep::SEMANTIC_VERDICT_INVALID,
      true,
      false);
  const auto output = pipeline.synthesize_reports({invalid});
  expect(output.report.candidates_size() == 0, "invalid trace leaked into candidates");
  expect(
      output.report.synthesis().quarantined_report_count() == 1,
      "invalid trace was not counted as quarantined");
}

void test_batch_identity_includes_evaluation_result() {
  trajectory::TrajectoryEvolutionPipeline pipeline;
  const auto original = make_rule_report(
      "user-a", "conv-a", "dialogue-a", "episode-a",
      ep::SEMANTIC_VERDICT_INVALID);
  auto revised = original;
  revised.mutable_reflection()->set_summary(
      "模型复核后给出了不同的跨轮结论。" );

  const auto original_batch = pipeline.synthesize_reports({original}).report;
  const auto revised_batch = pipeline.synthesize_reports({revised}).report;
  expect(
      !original_batch.runs(0).evaluation_fingerprint().empty(),
      "evaluation result fingerprint is missing");
  expect(
      original_batch.runs(0).input_fingerprint()
          == revised_batch.runs(0).input_fingerprint(),
      "fixture must represent the same input identity");
  expect(
      original_batch.batch_id() != revised_batch.batch_id(),
      "changed evaluation result must produce a new batch identity");
}

void test_meta_reflection_merges_validated_seeds() {
  auto adapter = std::make_shared<MetaAdapter>();
  trajectory::EvolutionOptions options;
  options.enable_meta_reflection = true;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, options, adapter);
  const auto first = make_reflection_report(
      "user-a", "conv-a", "dialogue-a", "episode-a",
      "在提示词中写明遇到不支持模型时立即停止。" );
  const auto second = make_reflection_report(
      "user-b", "conv-b", "dialogue-b", "episode-b",
      "提示词应要求确定性失败后重新规划，不能继续调用。" );
  const auto output = pipeline.synthesize_reports({first, second});
  expect(adapter->calls == 1, "meta reflection must use one model call");
  expect(
      output.report.meta_reflection().status()
          == ep::MODEL_CALL_STATUS_SUCCEEDED,
      "valid meta reflection was rejected");
  expect(output.report.candidates_size() == 1, "meta seeds were not merged");
  expect(
      output.report.candidates(0).meta_reflection_applied(),
      "meta provenance is missing");
  expect(
      output.report.candidates(0).source_seed_ids_size() == 2,
      "meta source seed IDs are incomplete");
}

void test_batch_identity_includes_meta_reflection_result() {
  trajectory::EvolutionOptions options;
  options.enable_meta_reflection = true;
  const std::vector<ep::HybridEvaluationReport> reports = {
      make_reflection_report(
          "user-a", "conv-a", "dialogue-a", "episode-a",
          "在提示词中写明遇到不支持模型时立即停止。"),
      make_reflection_report(
          "user-b", "conv-b", "dialogue-b", "episode-b",
          "提示词应要求确定性失败后重新规划，不能继续调用。"),
  };
  auto first_adapter = std::make_shared<MetaAdapter>(
      "在提示词中明确非重试错误必须停止并重新规划。");
  auto second_adapter = std::make_shared<MetaAdapter>(
      "在提示词中明确非重试错误必须停止并切换能力。");
  const auto first = trajectory::TrajectoryEvolutionPipeline(
      {}, options, first_adapter).synthesize_reports(reports).report;
  const auto second = trajectory::TrajectoryEvolutionPipeline(
      {}, options, second_adapter).synthesize_reports(reports).report;
  expect(
      first.meta_reflection().status() == ep::MODEL_CALL_STATUS_SUCCEEDED,
      "first meta reflection failed");
  expect(
      first.meta_reflection().response_fingerprint()
          != second.meta_reflection().response_fingerprint(),
      "fixture meta responses must differ");
  expect(
      first.runs(0).evaluation_fingerprint()
          == second.runs(0).evaluation_fingerprint(),
      "fixture dialogue evaluations must be identical");
  expect(
      first.batch_id() != second.batch_id(),
      "changed meta-reflection result must produce a new batch identity");
}

void test_memory_merge_is_idempotent_and_versioned() {
  trajectory::EvolutionOptions options;
  options.min_supporting_dialogues = 2;
  options.min_supporting_episodes = 2;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, options);
  const auto first = make_rule_report(
      "user-a", "conv-a", "dialogue-a", "episode-a",
      ep::SEMANTIC_VERDICT_INVALID);
  const auto second = make_rule_report(
      "user-b", "conv-b", "dialogue-b", "episode-b",
      ep::SEMANTIC_VERDICT_INVALID);
  const auto batch = pipeline.synthesize_reports({first, second}).report;

  ep::EvolutionMemoryStore memory;
  auto update = trajectory::TrajectoryEvolutionPipeline::merge_memory(
      batch, &memory);
  expect(update.applied() && update.inserted_entry_count() == 1, "insert failed");
  expect(memory.revision() == 1 && memory.entries_size() == 1, "memory is wrong");
  expect(memory.entries(0).revisions_size() == 1, "initial revision is missing");

  update = trajectory::TrajectoryEvolutionPipeline::merge_memory(batch, &memory);
  expect(update.idempotent_replay(), "same batch must be idempotent");
  expect(memory.revision() == 1, "idempotent replay changed revision");

  const auto third = make_rule_report(
      "user-c", "conv-c", "dialogue-c", "episode-c",
      ep::SEMANTIC_VERDICT_INVALID);
  const auto second_batch = pipeline.synthesize_reports(
      {first, second, third}).report;
  update = trajectory::TrajectoryEvolutionPipeline::merge_memory(
      second_batch, &memory);
  expect(update.applied() && update.updated_entry_count() == 1, "update failed");
  expect(memory.revision() == 2, "memory revision did not advance");
  expect(
      memory.entries(0).candidate().support_dialogue_count() == 3,
      "cross-batch evidence was not merged");
  expect(memory.entries(0).revisions_size() == 2, "history is incomplete");

  TempDirectory temporary;
  const auto path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(memory, path);
  const auto loaded = trajectory::TrajectoryEvolutionPipeline::load_memory(path);
  expect(
      loaded.SerializeAsString() == memory.SerializeAsString(),
      "memory Protobuf round trip changed data");
  expect(
      trajectory::TrajectoryEvolutionPipeline::to_json(memory).find("api_key")
          == std::string::npos,
      "memory unexpectedly contains credential fields");
}

void test_memory_file_replay_recovers_json_view() {
  trajectory::EvolutionOptions options;
  options.min_supporting_dialogues = 2;
  options.min_supporting_episodes = 2;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, options);
  const auto batch = pipeline.synthesize_reports({
      make_rule_report(
          "user-a", "conv-a", "dialogue-a", "episode-a",
          ep::SEMANTIC_VERDICT_INVALID),
      make_rule_report(
          "user-b", "conv-b", "dialogue-b", "episode-b",
          ep::SEMANTIC_VERDICT_INVALID),
  }).report;

  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  const auto json_path = temporary.path() / "memory.json";
  auto update = trajectory::TrajectoryEvolutionPipeline::update_memory_file(
      batch, protobuf_path, json_path);
  expect(update.applied(), "initial memory file update failed");
  expect(std::filesystem::exists(json_path), "memory JSON view is missing");

  std::filesystem::remove(json_path);
  update = trajectory::TrajectoryEvolutionPipeline::update_memory_file(
      batch, protobuf_path, json_path);
  expect(update.idempotent_replay(), "replayed batch was not idempotent");
  expect(
      std::filesystem::exists(json_path)
          && std::filesystem::file_size(json_path) > 0U,
      "idempotent replay did not recover the memory JSON view");
}

void test_file_failure_does_not_abort_batch() {
  TempDirectory temporary;
  const auto good = temporary.path() / "good.jsonl";
  const auto bad = temporary.path() / "bad.jsonl";
  write_lines(good, {
      "{\"type\":\"dialogue.header\",\"version\":2,"
      "\"dialogue_id\":\"d-clean\",\"conversation_id\":\"conv-clean\","
      "\"user_id\":\"user-clean\"}",
      event(1, "dialogue.start", 0, "{\"input\":\"你好\"}"),
      event(2, "coordinator.activation", 0, "{\"activation_index\":1}"),
      event(3, "llm.request", 1,
            "{\"task_type\":\"coordinator\",\"activation_index\":1,"
            "\"messages\":[{\"role\":\"user\",\"content\":\"你好\"}]}"),
      event(4, "llm.response", 1,
            "{\"response\":{\"choices\":[{\"finish_reason\":\"stop\"}]}}"),
      event(5, "loop.control", 1,
            "{\"scope\":\"coordinator\",\"action\":\"stop\","
            "\"reason\":\"final_response\"}"),
      event(6, "dialogue.end", 0,
            "{\"status\":\"normal\",\"response_text\":\"你好\"}"),
  });
  write_lines(bad, {"{not-json"});

  trajectory::TrajectoryEvolutionPipeline pipeline;
  const auto output = pipeline.evaluate_files({bad, good});
  expect(
      output.report.evaluation_succeeded_count() == 1,
      "valid dialogue did not finish");
  expect(
      output.report.evaluation_failed_count() == 1,
      "invalid dialogue failure was not isolated");
  expect(output.dialogues.size() == 2, "batch result count is incomplete");
}

}  // namespace

int main() {
  try {
    test_cross_dialogue_gate_and_contradiction();
    test_composite_source_identity_has_boundaries();
    test_invalid_trace_is_quarantined();
    test_batch_identity_includes_evaluation_result();
    test_meta_reflection_merges_validated_seeds();
    test_batch_identity_includes_meta_reflection_result();
    test_memory_merge_is_idempotent_and_versioned();
    test_memory_file_replay_recovers_json_view();
    test_file_failure_does_not_abort_batch();
    std::cout << "Trajectory evolution tests passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
