#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/experience.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
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
  report.mutable_reflection()->set_summary("当前决策缺少可复用的失败处理经验。" );
  *report.mutable_reflection()->add_root_causes() = "没有约束重复失败后的行为。";
  *report.mutable_reflection()->add_experience_injections() = proposal;
  *report.mutable_reflection()->add_wasted_episode_ids() = episode;
  *report.mutable_reflection()->add_evidence_event_ids() = "event-" + episode;
  return report;
}

class MetaAdapter final : public trajectory::ModelAdapter {
 public:
  explicit MetaAdapter(std::string proposal =
      "注入经验：非重试错误必须停止并重新规划。")
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

class MemoryMatchAdapter final : public trajectory::ModelAdapter {
 public:
  explicit MemoryMatchAdapter(
      std::string relation,
      double confidence = 0.95,
      bool valid_json = true,
      std::function<void()> on_first_call = {})
      : relation_(std::move(relation)),
        on_first_call_(std::move(on_first_call)),
        confidence_(confidence),
        valid_json_(valid_json) {}

  trajectory::RawModelResponse evaluate(
      const trajectory::ModelRequest& request) override {
    ++calls;
    if(request.packet_id.find("memory-curation:") != 0U
       || request.user_prompt.find("\"incoming_candidate\":")
           == std::string::npos
       || request.user_prompt.find("\"memory_options\":[")
           == std::string::npos) {
      throw std::runtime_error("memory fixture received an invalid request");
    }
    if(calls == 1 && on_first_call_) {
      on_first_call_();
    }
    trajectory::RawModelResponse response;
    response.transport_ok = true;
    response.adapter = name();
    response.model_id = model_id();
    response.attempts = 1;
    if(!valid_json_) {
      response.body = "not-json";
      return response;
    }
    std::string memory_id;
    if(relation_ != "DISTINCT") {
      const std::regex pattern(R"REGEX("memory_id":"([^"]+)")REGEX");
      std::smatch match;
      if(!std::regex_search(request.user_prompt, match, pattern)) {
        throw std::runtime_error("memory fixture received no match options");
      }
      memory_id = match[1].str();
    }
    response.body =
        "{\"relation\":\"" + relation_ + "\",\"memory_id\":\""
        + memory_id + "\",\"confidence\":" + std::to_string(confidence_)
        + ",\"reason\":\"两个候选的适用范围相同，按测试关系处理。\"}";
    return response;
  }

  std::string name() const override {
    return "memory-match-test";
  }

  std::string model_id() const override {
    return "memory-match-model";
  }

  std::string relation_;
  std::function<void()> on_first_call_;
  double confidence_ = 0.0;
  bool valid_json_ = true;
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

ep::HybridEvaluationBatchReport make_prompt_batch(
    const std::string& suffix,
    const std::string& proposal) {
  trajectory::EvolutionOptions options;
  options.min_supporting_dialogues = 1;
  options.min_supporting_episodes = 1;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, options);
  return pipeline.synthesize_reports({make_reflection_report(
      "user-" + suffix,
      "conv-" + suffix,
      "dialogue-" + suffix,
      "episode-" + suffix,
      proposal)}).report;
}

const ep::EvolutionMemoryEntry* find_memory_entry(
    const ep::EvolutionMemoryStore& memory,
    const std::string& memory_id) {
  for(const auto& entry : memory.entries()) {
    if(entry.memory_id() == memory_id) {
      return &entry;
    }
  }
  return nullptr;
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
  ep::EvolutionMemoryStore memory;
  trajectory::TrajectoryEvolutionPipeline::merge_memory(output.report, &memory);
  const auto incremental = pipeline.synthesize_reports({contradiction}, memory);
  expect(incremental.report.runs_size() == 1 && incremental.report.candidates_size() == 1,
      "incremental synthesis must not replay old reports or lose counterevidence");
  expect(incremental.report.candidates(0).contradiction_count() == 1,
      "new evidence must contradict memory even without a new invalid seed");
  expect(incremental.report.candidates(0).status() == ep::EVOLUTION_CANDIDATE_STATUS_CONFLICTED,
      "incremental counterevidence must retain the conflict gate");
  expect(pipeline.synthesize_reports({}, memory).report.candidates_size() == 0,
      "an empty batch must not regenerate old candidates");
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
      "注入经验：遇到不支持模型时立即停止。" );
  const auto second = make_reflection_report(
      "user-b", "conv-b", "dialogue-b", "episode-b",
      "注入经验：确定性失败后应重新规划，不能继续调用。" );
  const auto output = pipeline.synthesize_reports({first, second});
  expect(adapter->calls == 1, "meta reflection must use one model call");
  expect(
      output.report.meta_reflection().status()
          == ep::MODEL_CALL_STATUS_SUCCEEDED,
      "valid meta reflection was rejected");
  expect(output.report.candidates_size() == 1, "meta seeds were not merged");
  expect(
      output.report.candidates(0).target_type()
          == ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION,
      "reflection must produce an experience-injection candidate");
  expect(
      output.report.candidates(0).meta_reflection_applied(),
      "meta provenance is missing");
  expect(
      output.report.candidates(0).source_seed_ids_size() == 2,
      "meta source seed IDs are incomplete");
}

void test_meta_reflection_isolates_compatibility_groups() {
  auto adapter = std::make_shared<MetaAdapter>();
  trajectory::EvolutionOptions options;
  options.enable_meta_reflection = true;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, options, adapter);
  auto first = make_reflection_report(
      "user-a", "conv-a", "dialogue-a", "episode-a",
      "注入经验：视频遇到确定性失败时立即停止。" );
  auto second = make_reflection_report(
      "user-b", "conv-b", "dialogue-b", "episode-b",
      "注入经验：视频遇到确定性失败后重新规划。" );
  auto third = make_reflection_report(
      "user-c", "conv-c", "dialogue-c", "episode-c",
      "注入经验：穿搭技能失败时保留已有结果。" );
  auto fourth = make_reflection_report(
      "user-d", "conv-d", "dialogue-d", "episode-d",
      "注入经验：穿搭技能失败后使用已有证据。" );
  third.mutable_assessments(0)->mutable_evidence()->set_skill_name(
      "outfit-stylist");
  fourth.mutable_assessments(0)->mutable_evidence()->set_skill_name(
      "outfit-stylist");

  const auto output = pipeline.synthesize_reports(
      {first, second, third, fourth});
  expect(adapter->calls == 2, "each compatibility group needs its own call");
  expect(
      output.model_requests.size() == 2U,
      "meta request audit must preserve both compatibility groups");
  expect(
      output.report.meta_reflection().status()
          == ep::MODEL_CALL_STATUS_SUCCEEDED,
      "isolated compatibility groups were rejected");
  expect(output.report.candidates_size() == 2, "meta groups were not isolated");
  for(const auto& candidate : output.report.candidates()) {
    expect(
        candidate.meta_reflection_applied(),
        "isolated meta candidate is missing provenance");
    expect(
        candidate.source_seed_ids_size() == 2,
        "isolated meta candidate has incomplete seed provenance");
  }
}

void test_batch_identity_includes_meta_reflection_result() {
  trajectory::EvolutionOptions options;
  options.enable_meta_reflection = true;
  const std::vector<ep::HybridEvaluationReport> reports = {
      make_reflection_report(
          "user-a", "conv-a", "dialogue-a", "episode-a",
          "注入经验：遇到不支持模型时立即停止。"),
      make_reflection_report(
          "user-b", "conv-b", "dialogue-b", "episode-b",
          "注入经验：确定性失败后应重新规划，不能继续调用。"),
  };
  auto first_adapter = std::make_shared<MetaAdapter>(
      "注入经验：非重试错误必须停止并重新规划。");
  auto second_adapter = std::make_shared<MetaAdapter>(
      "注入经验：非重试错误必须停止并切换能力。");
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

void test_scoped_and_legacy_experiences_remain_separate() {
  auto first = make_reflection_report(
      "user-a", "conv-a", "legacy-a", "episode-a", "确认失败原因后再决定是否重试。" );
  auto second = make_reflection_report(
      "user-a", "conv-b", "legacy-b", "episode-b", "先核对恢复条件，保留独立分支。" );
  auto third = make_reflection_report(
      "user-a", "conv-c", "scoped-a", "episode-c", "确认失败原因后再决定是否重试。" );
  auto fourth = make_reflection_report(
      "user-a", "conv-d", "scoped-b", "episode-d", "先核对恢复条件，保留独立分支。" );
  for(auto* report : {&third, &fourth}) {
    auto* detail = report->mutable_reflection()->add_experience_details();
    detail->set_target_scope("skill:video-gen");
    detail->set_trigger_pattern("同一技能反复遇到确定性失败时");
    detail->set_proposed_change(report->reflection().experience_injections(0));
    detail->add_evidence_event_ids(report->reflection().evidence_event_ids(0));
  }
  auto adapter = std::make_shared<MetaAdapter>("确认恢复条件和剩余预算后再决定是否重试。");
  trajectory::EvolutionOptions options;
  options.enable_meta_reflection = true;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, options, adapter);
  const auto mixed = pipeline.synthesize_reports({first, second, third, fourth});
  expect(adapter->calls == 2 && mixed.report.candidates_size() == 2,
         "legacy and scoped seeds must use independent meta requests");
  int scoped_count = 0;
  for(const auto& candidate : mixed.report.candidates()) {
    scoped_count += trajectory::is_scoped_experience(candidate) ? 1 : 0;
    expect(candidate.meta_reflection_applied() && candidate.source_seed_ids_size() == 2,
           "meta reflection lost contract-specific provenance");
  }
  expect(scoped_count == 1, "meta reflection silently upgraded legacy experience");
  const auto legacy = pipeline.synthesize_reports({first, second}).report;
  const auto scoped = pipeline.synthesize_reports({third, fourth}).report;
  expect(legacy.candidates(0).target_scope() == scoped.candidates(0).target_scope()
             && legacy.candidates(0).trigger_pattern() == scoped.candidates(0).trigger_pattern()
             && legacy.candidates(0).proposed_change() == scoped.candidates(0).proposed_change(),
         "fixture must differ only in generation contract and provenance");
  ep::EvolutionMemoryStore memory;
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(legacy, &memory);
  const auto merged = trajectory::TrajectoryEvolutionPipeline::merge_memory(scoped, &memory);
  expect(memory.entries_size() == 2 && merged.inserted_entry_count() == 1,
         "exact text matching merged legacy and scoped experience");

  TempDirectory temporary;
  const auto path = temporary.path() / "contract-memory.pb";
  trajectory::TrajectoryEvolutionPipeline::update_memory_file(legacy, path);
  auto matcher = std::make_shared<MemoryMatchAdapter>("DUPLICATE");
  trajectory::EvolutionOptions curation;
  curation.full_curation_new_entry_threshold = 1;
  trajectory::TrajectoryEvolutionPipeline curator({}, curation, matcher);
  const auto updated = curator.curate_memory_file(scoped, path);
  expect(matcher->calls == 0 && updated.result.inserted_entry_count() == 1,
         "semantic matching considered incompatible generation contracts");
  expect(updated.result.full_curation_triggered() && updated.result.full_curation_completed(),
         "contract isolation fixture did not complete periodic full curation");
  expect(trajectory::TrajectoryEvolutionPipeline::load_memory(path).entries_size() == 2,
         "full curation promoted legacy evidence into scoped memory");
  expect(curator.curate_memory_file(scoped, path).result.idempotent_replay(),
         "contract isolation broke idempotent batch replay");

  third.mutable_reflection()->mutable_experience_details(0)->set_target_scope("coordinator");
  trajectory::TrajectoryEvolutionPipeline no_model({}, {});
  expect(no_model.synthesize_reports({third}).report.candidates_size() == 0,
         "invalid structured advice fell back to unscoped display text");
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
  expect(update.decisions_size() == 1, "candidate mapping decision is missing");
  expect(memory.revision() == 1 && memory.entries_size() == 1, "memory is wrong");
  expect(memory.entries(0).revisions_size() == 1, "initial revision is missing");
  expect(
      update.decisions(0).candidate_id() == batch.candidates(0).candidate_id()
          && update.decisions(0).memory_id() == memory.entries(0).memory_id()
          && update.decisions(0).action()
              == ep::MEMORY_UPDATE_ACTION_INSERTED,
      "insert decision does not map candidate to memory");
  expect(
      memory.entries(0).source_candidate_ids_size() == 1
          && memory.entries(0).source_candidate_ids(0)
              == batch.candidates(0).candidate_id(),
      "memory source candidate mapping is missing");
  expect(
      !update.decisions(0).decision_id().empty(),
      "memory decision does not have a stable audit ID");

  update = trajectory::TrajectoryEvolutionPipeline::merge_memory(batch, &memory);
  expect(update.idempotent_replay(), "same batch must be idempotent");
  expect(memory.revision() == 1, "idempotent replay changed revision");
  expect(
      update.decisions_size() == 1
          && update.decisions(0).action()
              == ep::MEMORY_UPDATE_ACTION_UNCHANGED,
      "idempotent replay did not recover the candidate mapping");

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

  auto legacy = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  legacy.mutable_entries(0)->clear_source_candidate_ids();
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      legacy, protobuf_path);
  std::filesystem::remove(json_path);
  update = trajectory::TrajectoryEvolutionPipeline::update_memory_file(
      batch, protobuf_path, json_path);
  expect(update.idempotent_replay(), "replayed batch was not idempotent");
  expect(
      std::filesystem::exists(json_path)
          && std::filesystem::file_size(json_path) > 0U,
      "idempotent replay did not recover the memory JSON view");
  const auto migrated = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  expect(
      migrated.entries(0).source_candidate_ids_size() == 1,
      "idempotent replay did not migrate legacy memory metadata");
}

void test_default_memory_curation_is_conservative() {
  const auto first = make_prompt_batch(
      "default-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "default-b", "遇到网络读取超时时尽快停止无效重试。" );
  expect(
      first.candidates(0).candidate_id() != second.candidates(0).candidate_id(),
      "memory fixture candidates must have different IDs");

  ep::EvolutionMemoryStore memory;
  auto update = trajectory::TrajectoryEvolutionPipeline::merge_memory(
      first, &memory);
  expect(update.inserted_entry_count() == 1, "first candidate was not stored");
  update = trajectory::TrajectoryEvolutionPipeline::merge_memory(
      second, &memory);
  expect(
      memory.entries_size() == 2 && update.inserted_entry_count() == 1,
      "default curation guessed a semantic duplicate");
  expect(
      update.decisions_size() == 1
          && update.decisions(0).relation()
              == ep::MEMORY_MATCH_RELATION_UNRESOLVED
          && update.decisions(0).error_code() == "semantic_matching_disabled"
          && update.decisions(0).considered_matches_size() == 1,
      "default curation did not record its conservative decision");
}

void test_memory_curator_merges_semantic_duplicate() {
  const auto first = make_prompt_batch(
      "duplicate-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "duplicate-b", "遇到网络读取超时时尽快停止无效重试。" );
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  const auto json_path = temporary.path() / "memory.json";
  trajectory::TrajectoryEvolutionPipeline::update_memory_file(
      first, protobuf_path, json_path);
  const auto initial = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  const std::string original_memory_id = initial.entries(0).memory_id();
  const std::string original_candidate_id =
      initial.entries(0).candidate().candidate_id();

  auto adapter = std::make_shared<MemoryMatchAdapter>("DUPLICATE");
  trajectory::EvolutionOptions options;
  trajectory::TrajectoryEvolutionPipeline curator({}, options, adapter);
  auto output = curator.curate_memory_file(
      second, protobuf_path, json_path);
  expect(adapter->calls == 1, "semantic duplicate did not call the model once");
  expect(output.model_requests.size() == 1, "curator request was not retained");
  expect(
      output.result.model_requested_count() == 1
          && output.result.model_succeeded_count() == 1
          && output.result.model_failed_count() == 0,
      "curator model counters are wrong");
  expect(
      output.result.decisions_size() == 1
          && output.result.decisions(0).action()
              == ep::MEMORY_UPDATE_ACTION_MERGED
          && output.result.decisions(0).relation()
              == ep::MEMORY_MATCH_RELATION_DUPLICATE
          && output.result.decisions(0).memory_id() == original_memory_id,
      "semantic duplicate was not mapped to the existing memory");

  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  expect(memory.entries_size() == 1, "semantic duplicate created a new entry");
  const auto& entry = memory.entries(0);
  expect(
      entry.candidate().candidate_id() == original_candidate_id,
      "semantic merge changed the canonical candidate ID");
  expect(
      entry.source_candidate_ids_size() == 2,
      "semantic merge did not preserve both source candidates");
  expect(entry.revisions_size() == 2, "semantic merge revision is missing");

  output = curator.curate_memory_file(second, protobuf_path, json_path);
  expect(output.result.idempotent_replay(), "semantic batch replay is not idempotent");
  expect(adapter->calls == 1, "idempotent replay called the model again");
  expect(
      output.result.decisions(0).memory_id() == original_memory_id,
      "idempotent replay lost the semantic alias mapping");
}

void test_memory_curator_records_related_entries() {
  const auto first = make_prompt_batch(
      "related-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "related-b", "网络超时后切换到备用模型继续完成任务。" );
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::update_memory_file(
      first, protobuf_path);
  const auto initial = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  const std::string original_memory_id = initial.entries(0).memory_id();

  auto adapter = std::make_shared<MemoryMatchAdapter>("RELATED");
  trajectory::EvolutionOptions options;
  options.enable_memory_semantic_matching = true;
  trajectory::TrajectoryEvolutionPipeline curator({}, options, adapter);
  const auto output = curator.curate_memory_file(second, protobuf_path);
  expect(
      output.result.inserted_entry_count() == 1
          && output.result.updated_entry_count() == 1,
      "related memory did not report both changed entries");
  const auto& decision = output.result.decisions(0);
  expect(
      decision.action() == ep::MEMORY_UPDATE_ACTION_INSERTED
          && decision.relation() == ep::MEMORY_MATCH_RELATION_RELATED
          && decision.matched_memory_id() == original_memory_id
          && decision.memory_id() != original_memory_id,
      "related candidate mapping is wrong");

  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  expect(memory.entries_size() == 2, "related candidate was incorrectly merged");
  const auto* original = find_memory_entry(memory, original_memory_id);
  const auto* inserted = find_memory_entry(memory, decision.memory_id());
  expect(original != nullptr && inserted != nullptr, "related entries are missing");
  expect(
      original->relations_size() == 1
          && original->relations(0).target_memory_id() == inserted->memory_id()
          && inserted->relations_size() == 1
          && inserted->relations(0).target_memory_id() == original->memory_id(),
      "related memory link is not symmetric");
  expect(
      original->relations(0).decision_id() == decision.decision_id()
          && inserted->relations(0).decision_id() == decision.decision_id()
          && !decision.decision_id().empty(),
      "compact relations do not link back to the batch decision");
}

void test_memory_curator_model_failure_does_not_merge() {
  const auto first = make_prompt_batch(
      "failure-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "failure-b", "遇到网络读取超时时尽快停止无效重试。" );
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::update_memory_file(
      first, protobuf_path);

  auto adapter = std::make_shared<MemoryMatchAdapter>("DUPLICATE", 0.95, false);
  trajectory::EvolutionOptions options;
  options.enable_memory_semantic_matching = true;
  trajectory::TrajectoryEvolutionPipeline curator({}, options, adapter);
  const auto output = curator.curate_memory_file(second, protobuf_path);
  expect(
      output.result.model_failed_count() == 1
          && output.result.decisions(0).action()
              == ep::MEMORY_UPDATE_ACTION_INSERTED
          && output.result.decisions(0).relation()
              == ep::MEMORY_MATCH_RELATION_UNRESOLVED
          && output.result.decisions(0).error_code() == "invalid_json",
      "invalid model output was not handled conservatively");
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  expect(memory.entries_size() == 2, "model failure caused an unsafe merge");
  expect(
      memory.entries(0).relations_size() == 0
          && memory.entries(1).relations_size() == 0,
      "model failure created an unsupported relation");
}

void test_memory_curator_rejects_low_confidence_and_records_conflict() {
  const auto first = make_prompt_batch(
      "relation-a", "网络超时后减少重复调用并重新规划。" );
  const auto duplicate = make_prompt_batch(
      "relation-b", "遇到网络读取超时时尽快停止无效重试。" );
  const auto conflict = make_prompt_batch(
      "relation-c", "网络超时后必须继续重试原有请求。" );
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::update_memory_file(
      first, protobuf_path);

  trajectory::EvolutionOptions options;
  options.enable_memory_semantic_matching = true;
  auto low_confidence = std::make_shared<MemoryMatchAdapter>(
      "DUPLICATE", 0.6);
  trajectory::TrajectoryEvolutionPipeline conservative(
      {}, options, low_confidence);
  const auto low_output = conservative.curate_memory_file(
      duplicate, protobuf_path);
  expect(
      low_output.result.decisions(0).action()
              == ep::MEMORY_UPDATE_ACTION_INSERTED
          && low_output.result.decisions(0).error_code()
              == "memory_match_confidence_below_threshold",
      "low-confidence duplicate was merged");

  auto contradiction = std::make_shared<MemoryMatchAdapter>("CONTRADICTS");
  trajectory::TrajectoryEvolutionPipeline conflict_curator(
      {}, options, contradiction);
  const auto conflict_output = conflict_curator.curate_memory_file(
      conflict, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  const auto& decision = conflict_output.result.decisions(0);
  const auto* inserted = find_memory_entry(memory, decision.memory_id());
  const auto* matched = find_memory_entry(memory, decision.matched_memory_id());
  expect(
      decision.action() == ep::MEMORY_UPDATE_ACTION_INSERTED
          && decision.relation() == ep::MEMORY_MATCH_RELATION_CONTRADICTS
          && inserted != nullptr && matched != nullptr,
      "contradicting memory decision is incomplete");
  expect(
      inserted->relations_size() == 1
          && inserted->relations(0).relation()
              == ep::MEMORY_MATCH_RELATION_CONTRADICTS
          && inserted->status() == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE
          && matched->relations_size() == 1
          && matched->relations(0).relation()
              == ep::MEMORY_MATCH_RELATION_CONTRADICTS
          && matched->status() == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE,
      "contradicting memory relation is not symmetric");
}

void test_conflict_relation_survives_later_related_review() {
  const auto first = make_prompt_batch(
      "sticky-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "sticky-b", "网络读取超时后停止当前工具并切换备用路径。" );
  auto trigger = first;
  trigger.set_batch_id("batch:sticky-full-review");

  ep::EvolutionMemoryStore seeded;
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(first, &seeded);
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      seeded, protobuf_path);

  trajectory::EvolutionOptions incremental_options;
  incremental_options.full_curation_new_entry_threshold = 100;
  auto conflict_adapter = std::make_shared<MemoryMatchAdapter>(
      "CONTRADICTS", 0.75);
  trajectory::TrajectoryEvolutionPipeline incremental(
      {}, incremental_options, conflict_adapter);
  (void)incremental.curate_memory_file(second, protobuf_path);

  trajectory::EvolutionOptions full_options;
  full_options.full_curation_new_entry_threshold = 2;
  auto related_adapter = std::make_shared<MemoryMatchAdapter>(
      "RELATED", 0.99);
  trajectory::TrajectoryEvolutionPipeline full_curator(
      {}, full_options, related_adapter);
  const auto output = full_curator.curate_memory_file(
      trigger, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);

  expect(
      output.result.full_curation_completed()
          && related_adapter->calls == 2
          && memory.entries_size() == 2,
      "later full-store related review did not complete");
  for(const auto& entry : memory.entries()) {
    expect(
        entry.relations_size() == 1
            && entry.relations(0).relation()
                == ep::MEMORY_MATCH_RELATION_CONTRADICTS,
        "higher-confidence related review overwrote a known conflict");
  }
}

void test_full_curation_runs_after_retained_threshold() {
  const auto first = make_prompt_batch(
      "full-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "full-b", "遇到网络读取超时时尽快停止无效重试。" );
  const auto replay = make_prompt_batch(
      "full-c", "网络超时后减少重复调用并重新规划。" );

  ep::EvolutionMemoryStore seeded;
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(first, &seeded);
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(second, &seeded);
  expect(
      seeded.entries_size() == 2
          && seeded.new_entries_since_full_curation() == 2,
      "seeded memory did not retain the new-entry counter");

  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      seeded, protobuf_path);

  auto adapter = std::make_shared<MemoryMatchAdapter>("DUPLICATE");
  trajectory::EvolutionOptions options;
  options.full_curation_new_entry_threshold = 2;
  trajectory::TrajectoryEvolutionPipeline curator({}, options, adapter);
  const auto output = curator.curate_memory_file(replay, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);

  expect(
      output.result.full_curation_triggered()
          && output.result.full_curation_completed(),
      "retained-entry threshold did not complete a full-store review");
  expect(
      output.result.full_curation_examined_count() == 1
          && output.result.full_curation_merged_count() == 1
          && adapter->calls == 1,
      "full-store review did not semantically consolidate the existing pair");
  expect(
      memory.entries_size() == 1
          && memory.new_entries_since_full_curation() == 0
          && output.result.pending_new_entry_count() == 0,
      "successful full-store review did not reset the persisted counter");
  bool found_full_decision = false;
  for(const auto& decision : output.result.decisions()) {
    if(decision.full_store_curation()) {
      found_full_decision = true;
      expect(
          decision.action() == ep::MEMORY_UPDATE_ACTION_MERGED
              && !decision.decision_id().empty(),
          "full-store merge is not auditable");
    }
  }
  expect(found_full_decision, "full-store decision is missing");

  const auto alias_replay = curator.curate_memory_file(second, protobuf_path);
  const auto replayed_memory =
      trajectory::TrajectoryEvolutionPipeline::load_memory(protobuf_path);
  expect(
      alias_replay.result.idempotent_replay()
          && alias_replay.result.decisions_size() == 1
          && alias_replay.result.decisions(0).memory_id()
              == replayed_memory.entries(0).memory_id()
          && adapter->calls == 1,
      "full-store merge lost an idempotent source alias");
}

void test_failed_full_curation_keeps_pending_work() {
  const auto first = make_prompt_batch(
      "pending-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "pending-b", "遇到网络读取超时时尽快停止无效重试。" );
  const auto replay = make_prompt_batch(
      "pending-c", "网络超时后减少重复调用并重新规划。" );

  ep::EvolutionMemoryStore seeded;
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(first, &seeded);
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(second, &seeded);
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      seeded, protobuf_path);

  auto adapter = std::make_shared<MemoryMatchAdapter>(
      "DUPLICATE", 0.95, false);
  trajectory::EvolutionOptions options;
  options.full_curation_new_entry_threshold = 2;
  trajectory::TrajectoryEvolutionPipeline curator({}, options, adapter);
  const auto output = curator.curate_memory_file(replay, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);

  expect(
      output.result.applied()
          && output.result.full_curation_triggered()
          && !output.result.full_curation_completed()
          && output.result.full_curation_error_code()
              == "semantic_review_incomplete",
      "model failure did not remain isolated to full-store management");
  expect(
      output.result.model_failed_count() == 2
          && memory.entries_size() == 2,
      "failed semantic review changed or lost existing memories");
  expect(
      memory.new_entries_since_full_curation() == 2
          && output.result.pending_new_entry_count() == 2,
      "failed full-store review incorrectly cleared pending work");
}

void test_full_curation_defers_without_required_resources() {
  const auto first = make_prompt_batch(
      "defer-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "defer-b", "遇到网络读取超时时尽快停止无效重试。" );
  const auto replay = make_prompt_batch(
      "defer-c", "网络超时后减少重复调用并重新规划。" );

  ep::EvolutionMemoryStore seeded;
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(first, &seeded);
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(second, &seeded);
  TempDirectory temporary;
  const auto no_adapter_path = temporary.path() / "no-adapter.pb";
  const auto no_budget_path = temporary.path() / "no-budget.pb";
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      seeded, no_adapter_path);
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      seeded, no_budget_path);

  trajectory::EvolutionOptions isolated_options;
  isolated_options.full_curation_new_entry_threshold = 1;
  trajectory::TrajectoryEvolutionPipeline isolated({}, isolated_options);
  const auto isolated_output = isolated.curate_memory_file(
      first, temporary.path() / "isolated.pb");
  const auto isolated_memory =
      trajectory::TrajectoryEvolutionPipeline::load_memory(
          temporary.path() / "isolated.pb");
  expect(
      isolated_output.result.full_curation_triggered()
          && isolated_output.result.full_curation_completed()
          && isolated_output.result.pending_new_entry_count() == 0
          && isolated_memory.entries_size() == 1,
      "isolated memory incorrectly required a semantic model review");

  trajectory::EvolutionOptions options;
  options.full_curation_new_entry_threshold = 2;
  trajectory::TrajectoryEvolutionPipeline no_adapter({}, options);
  const auto no_adapter_output = no_adapter.curate_memory_file(
      replay, no_adapter_path);
  expect(
      no_adapter_output.result.applied()
          && !no_adapter_output.result.full_curation_completed()
          && no_adapter_output.result.full_curation_error_code()
              == "adapter_not_configured"
          && no_adapter_output.result.pending_new_entry_count() == 2,
      "missing adapter silently completed semantic full-store management");

  trajectory::EvaluatorOptions evaluator;
  evaluator.max_model_calls = 1;
  auto adapter = std::make_shared<MemoryMatchAdapter>("DUPLICATE");
  trajectory::TrajectoryEvolutionPipeline no_budget(
      evaluator, options, adapter);
  const auto no_budget_output = no_budget.curate_memory_file(
      replay, no_budget_path);
  expect(
      no_budget_output.result.applied()
          && !no_budget_output.result.full_curation_completed()
          && no_budget_output.result.full_curation_error_code()
              == "model_call_limit_reached"
          && no_budget_output.result.pending_new_entry_count() == 2
          && adapter->calls == 0,
      "insufficient full-store budget consumed a partial review");
}

void test_memory_capacity_bounds_working_set_and_replay_window() {
  const auto first = make_prompt_batch(
      "capacity-a", "网络超时后停止当前工具并重新规划。" );
  const auto second = make_prompt_batch(
      "capacity-b", "图片生成失败后切换到备用模型。" );
  const auto third = make_prompt_batch(
      "capacity-c", "工具返回空结果时先校验产物再继续。" );
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";

  trajectory::EvolutionOptions options;
  options.enable_memory_semantic_matching = false;
  options.full_curation_new_entry_threshold = 100;
  options.max_memory_entries = 2;
  options.max_applied_batch_ids = 2;
  trajectory::TrajectoryEvolutionPipeline curator({}, options);
  (void)curator.curate_memory_file(first, protobuf_path);
  (void)curator.curate_memory_file(second, protobuf_path);
  const auto output = curator.curate_memory_file(third, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);

  const std::string first_memory_id =
      "memory:" + first.candidates(0).candidate_id();
  const std::string second_memory_id =
      "memory:" + second.candidates(0).candidate_id();
  const std::string third_memory_id =
      "memory:" + third.candidates(0).candidate_id();
  expect(
      memory.entries_size() == 2
          && find_memory_entry(memory, first_memory_id) == nullptr
          && find_memory_entry(memory, second_memory_id) != nullptr
          && find_memory_entry(memory, third_memory_id) != nullptr,
      "capacity policy did not evict the oldest equally strong memory");
  expect(
      memory.applied_batch_ids_size() == 2
          && memory.applied_batch_ids(0) == second.batch_id()
          && memory.applied_batch_ids(1) == third.batch_id()
          && output.result.pruned_applied_batch_id_count() == 1,
      "bounded exact replay window was not maintained");
  expect(
      output.result.evicted_entry_count() == 1
          && output.result.curator_version()
              == "evolution-memory-curator-v3",
      "capacity eviction counters or curator version are wrong");
  bool audited = false;
  for(const auto& decision : output.result.decisions()) {
    if(decision.action() == ep::MEMORY_UPDATE_ACTION_EVICTED) {
      audited = decision.memory_id() == first_memory_id
          && !decision.decision_id().empty();
    }
  }
  expect(audited, "capacity eviction is not auditable in the Batch result");

  const auto replay = curator.curate_memory_file(third, protobuf_path);
  expect(
      replay.result.idempotent_replay()
          && replay.result.evicted_entry_count() == 0,
      "recent retained Batch replay was not idempotent");
}

void test_idempotent_replay_applies_capacity_migration() {
  const auto first = make_prompt_batch(
      "migration-a", "网络超时后停止当前工具并重新规划。" );
  const auto second = make_prompt_batch(
      "migration-b", "图片生成失败后切换到备用模型。" );
  const auto third = make_prompt_batch(
      "migration-c", "工具返回空结果时先校验产物再继续。" );
  ep::EvolutionMemoryStore seeded;
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(first, &seeded);
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(second, &seeded);
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(third, &seeded);
  expect(seeded.entries_size() == 3, "capacity migration fixture is invalid");

  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      seeded, protobuf_path);
  trajectory::EvolutionOptions options;
  options.enable_memory_semantic_matching = false;
  options.full_curation_new_entry_threshold = 100;
  options.max_memory_entries = 2;
  options.max_applied_batch_ids = 2;
  trajectory::TrajectoryEvolutionPipeline curator({}, options);
  const auto output = curator.curate_memory_file(third, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);

  expect(
      output.result.idempotent_replay()
          && !output.result.applied()
          && output.result.revision_after() == seeded.revision()
          && output.result.evicted_entry_count() == 1
          && output.result.pruned_applied_batch_id_count() == 1
          && memory.entries_size() == 2,
      "idempotent replay did not apply bounded-store migration");
}

void test_memory_capacity_preserves_validated_memory() {
  const auto validated = make_prompt_batch(
      "validated-capacity", "非重试错误应立即停止并重新规划。" );
  const auto incoming = make_prompt_batch(
      "candidate-capacity", "工具失败后重新生成相同参数。" );
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";

  trajectory::EvolutionOptions options;
  options.enable_memory_semantic_matching = false;
  options.full_curation_new_entry_threshold = 100;
  options.max_memory_entries = 1;
  trajectory::TrajectoryEvolutionPipeline curator({}, options);
  (void)curator.curate_memory_file(validated, protobuf_path);
  auto seeded = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  seeded.mutable_entries(0)->set_status(
      ep::EVOLUTION_MEMORY_STATUS_VALIDATED);
  const std::string validated_memory_id = seeded.entries(0).memory_id();
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      seeded, protobuf_path);

  const auto output = curator.curate_memory_file(incoming, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);
  expect(
      output.result.inserted_entry_count() == 1
          && output.result.evicted_entry_count() == 1
          && memory.entries_size() == 1
          && memory.entries(0).memory_id() == validated_memory_id
          && memory.entries(0).status()
              == ep::EVOLUTION_MEMORY_STATUS_VALIDATED,
      "capacity policy evicted a validated memory before a candidate");
}

void test_memory_capacity_evicts_conflict_group_atomically() {
  const auto first = make_prompt_batch(
      "conflict-capacity-a", "网络超时后立即停止原请求。" );
  const auto second = make_prompt_batch(
      "conflict-capacity-b", "网络超时后必须继续重试原请求。" );
  const auto third = make_prompt_batch(
      "conflict-capacity-c", "空工具结果必须先校验状态再继续。" );
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";

  trajectory::EvolutionOptions conflict_options;
  conflict_options.full_curation_new_entry_threshold = 100;
  conflict_options.max_memory_entries = 10;
  auto adapter = std::make_shared<MemoryMatchAdapter>("CONTRADICTS");
  trajectory::TrajectoryEvolutionPipeline conflict_curator(
      {}, conflict_options, adapter);
  (void)conflict_curator.curate_memory_file(first, protobuf_path);
  (void)conflict_curator.curate_memory_file(second, protobuf_path);

  trajectory::EvolutionOptions capacity_options;
  capacity_options.enable_memory_semantic_matching = false;
  capacity_options.full_curation_new_entry_threshold = 100;
  capacity_options.max_memory_entries = 2;
  trajectory::TrajectoryEvolutionPipeline capacity_curator(
      {}, capacity_options);
  const auto output = capacity_curator.curate_memory_file(
      third, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);

  expect(
      output.result.evicted_entry_count() == 2
          && memory.entries_size() == 1
          && memory.entries(0).candidate().candidate_id()
              == third.candidates(0).candidate_id()
          && memory.entries(0).relations_size() == 0,
      "capacity policy split or orphaned a contradiction group");
}

void test_memory_capacity_enforces_serialized_byte_limit() {
  const auto batch = make_prompt_batch(
      "byte-capacity", "非重试错误后停止当前技能并切换安全路径。" );
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";

  trajectory::EvolutionOptions options;
  options.enable_memory_semantic_matching = false;
  options.full_curation_new_entry_threshold = 100;
  options.max_memory_entries = 10;
  trajectory::TrajectoryEvolutionPipeline curator({}, options);
  constexpr std::size_t byte_limit = 512;
  const auto output = curator.curate_memory_file(
      batch, protobuf_path, {}, byte_limit);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path, byte_limit);

  expect(
      output.result.evicted_entry_count() == 1
          && memory.entries_size() == 0
          && std::filesystem::file_size(protobuf_path) <= byte_limit,
      "serialized Memory byte limit did not evict an oversized entry");
}

void test_memory_retry_reuses_identical_model_response() {
  const auto first = make_prompt_batch(
      "retry-a", "网络超时后减少重复调用并重新规划。" );
  const auto second = make_prompt_batch(
      "retry-b", "网络读取超时后停止当前工具并切换备用路径。" );

  ep::EvolutionMemoryStore seeded;
  (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(first, &seeded);
  TempDirectory temporary;
  const auto protobuf_path = temporary.path() / "memory.pb";
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      seeded, protobuf_path);

  auto adapter = std::make_shared<MemoryMatchAdapter>(
      "RELATED",
      0.95,
      true,
      [protobuf_path]() {
        auto concurrent = trajectory::TrajectoryEvolutionPipeline::load_memory(
            protobuf_path);
        concurrent.set_revision(concurrent.revision() + 1U);
        trajectory::TrajectoryEvolutionPipeline::write_protobuf(
            concurrent, protobuf_path);
      });
  trajectory::EvolutionOptions options;
  options.full_curation_new_entry_threshold = 100;
  trajectory::TrajectoryEvolutionPipeline curator({}, options, adapter);
  const auto output = curator.curate_memory_file(second, protobuf_path);
  const auto memory = trajectory::TrajectoryEvolutionPipeline::load_memory(
      protobuf_path);

  expect(
      output.result.applied()
          && output.result.concurrency_retry_count() == 1
          && output.result.model_requested_count() == 1
          && output.model_requests.size() == 1U
          && adapter->calls == 1,
      "optimistic retry repeated an identical model request");
  expect(
      memory.entries_size() == 2
          && output.result.decisions(0).relation()
              == ep::MEMORY_MATCH_RELATION_RELATED,
      "optimistic retry lost the final memory update");
}

void test_memory_rejects_corrupt_candidate_identity_before_mutation() {
  auto batch = make_prompt_batch(
      "corrupt", "网络超时后减少重复调用并重新规划。" );
  batch.mutable_candidates(0)->set_candidate_id("candidate:corrupt");
  ep::EvolutionMemoryStore memory;
  const std::string before = memory.SerializeAsString();
  bool rejected = false;
  try {
    (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(batch, &memory);
  } catch(const trajectory::EvaluationError&) {
    rejected = true;
  }
  expect(rejected, "corrupt candidate identity was accepted");
  expect(
      memory.SerializeAsString() == before,
      "corrupt candidate changed memory before rejection");

  auto duplicates = make_prompt_batch(
      "duplicate-ids", "网络超时后减少重复调用并重新规划。" );
  const ep::EvolutionCandidate repeated = duplicates.candidates(0);
  duplicates.add_candidates()->CopyFrom(repeated);
  ep::EvolutionMemoryStore duplicate_memory;
  const std::string duplicate_before = duplicate_memory.SerializeAsString();
  bool duplicate_rejected = false;
  try {
    (void)trajectory::TrajectoryEvolutionPipeline::merge_memory(
        duplicates, &duplicate_memory);
  } catch(const trajectory::EvaluationError&) {
    duplicate_rejected = true;
  }
  expect(duplicate_rejected, "duplicate candidate identity was accepted");
  expect(
      duplicate_memory.SerializeAsString() == duplicate_before,
      "duplicate candidate changed memory before rejection");
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
    test_meta_reflection_isolates_compatibility_groups();
    test_scoped_and_legacy_experiences_remain_separate();
    test_batch_identity_includes_meta_reflection_result();
    test_memory_merge_is_idempotent_and_versioned();
    test_memory_file_replay_recovers_json_view();
    test_default_memory_curation_is_conservative();
    test_memory_curator_merges_semantic_duplicate();
    test_memory_curator_records_related_entries();
    test_conflict_relation_survives_later_related_review();
    test_full_curation_runs_after_retained_threshold();
    test_failed_full_curation_keeps_pending_work();
    test_memory_curator_model_failure_does_not_merge();
    test_full_curation_defers_without_required_resources();
    test_memory_capacity_bounds_working_set_and_replay_window();
    test_idempotent_replay_applies_capacity_migration();
    test_memory_capacity_preserves_validated_memory();
    test_memory_capacity_evicts_conflict_group_atomically();
    test_memory_capacity_enforces_serialized_byte_limit();
    test_memory_retry_reuses_identical_model_response();
    test_memory_curator_rejects_low_confidence_and_records_conflict();
    test_memory_rejects_corrupt_candidate_identity_before_mutation();
    test_file_failure_does_not_abort_batch();
    std::cout << "Trajectory evolution tests passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
