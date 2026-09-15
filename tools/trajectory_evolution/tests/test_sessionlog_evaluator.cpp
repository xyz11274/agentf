#include "agentflow/trajectory/hybrid_evaluator.h"
#include "trajectory_evolution/evolution.h"
#include "agentflow/trajectory/experience.h"
#include "../src/model_input.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <set>
#include <sstream>
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

void test_model_input_preserves_application_data() {
  rapidjson::Document event;
  event.Parse(R"({"producer_id":"writer","sequence":1,"actor_id":"worker",
    "action_id":"","step_id":"s1","status":"","caused_by":["e0"],
    "state_before":{},"input":{"producer_id":"business","sequence":0,"status":"","token_fingerprint":"data"},
    "output":null,"attributes":{"protocol":"wire","agent_version_id":"v1","evaluation_trial_id":"t1",
      "policy_scope":"worker","extensions":{"producer_id":"custom","status":""}}})");
  const auto input = trajectory::model_input::bytes(event["input"]);
  const auto custom = trajectory::model_input::bytes(event["attributes"]["extensions"]);
  trajectory::model_input::event_view(event);
  expect(!event.HasMember("producer_id") && !event.HasMember("sequence")
      && !event.HasMember("action_id") && !event.HasMember("status"), "protocol defaults retained");
  expect(event.HasMember("step_id") && event.HasMember("caused_by") && event.HasMember("state_before")
      && event.HasMember("output") && event["output"].IsNull(), "execution evidence omitted");
  expect(trajectory::model_input::bytes(event["input"]) == input
      && trajectory::model_input::bytes(event["attributes"]["extensions"]) == custom,
      "model projection rewrote application fields");
  expect(!event["attributes"].HasMember("protocol") && event["attributes"].HasMember("policy_scope"),
      "routing metadata removed the authorization scope");
  rapidjson::Document runtime;
  runtime.Parse(R"({"event_id":"e1","execution_id":"x1","caused_by":["e0"],
    "attributes":{"extensions":{"point_reference":{"run":"r","key":"k"},"custom":"keep"},"policy_scope":"worker"},
    "run_attributes":{"training_evaluation":{"job_id":"job"},"capture_complete":true},
    "input":{"training_evaluation":"business data","point_reference":"business data"}})");
  const auto original_input = trajectory::model_input::bytes(runtime["input"]);
  trajectory::model_input::event_view(runtime);
  expect(!runtime["attributes"]["extensions"].HasMember("point_reference")
      && runtime["attributes"]["extensions"].HasMember("custom")
      && runtime["run_attributes"].HasMember("training_evaluation")
      && runtime["run_attributes"].HasMember("capture_complete"), "runtime provenance not projected correctly");
  expect(trajectory::model_input::bytes(runtime["input"]) == original_input
      && runtime.HasMember("caused_by") && runtime.HasMember("execution_id"), "causal or business input lost");

  rapidjson::Document dialogue;
  dialogue.Parse(R"({"content_events":[],"episodes":[
    {"episode_id":"a","rule_assessment":{"verdict":"SEMANTIC_VERDICT_UNASSESSED","explanation":"未命中确定性异常规则；这不等同于已证明该步骤有效。"},
     "evidence":{"step_index":"1","terminal_status":"completed","next_same_owner":{"episode_id":"b","step_index":"2","terminal_status":"failed"}}},
    {"episode_id":"b","rule_assessment":{"verdict":"SEMANTIC_VERDICT_INVALID","explanation":"保留硬约束。"},
     "evidence":{"step_index":"2","terminal_status":"failed","previous_same_owner":{"episode_id":"a","step_index":"1","terminal_status":"unknown"}}},
    {"episode_id":"c","rule_assessment":{"verdict":"SEMANTIC_VERDICT_UNASSESSED","explanation":"保留不同的具体说明。"},
     "evidence":{"next_same_owner":{"episode_id":"missing","step_index":"4"}}}
  ]})");
  trajectory::model_input::dialogue_view(dialogue, false);
  const auto& episodes = dialogue["episodes"];
  expect(!episodes[0].HasMember("rule_assessment") && episodes[1].HasMember("rule_assessment")
      && episodes[2].HasMember("rule_assessment"), "default-rule projection removed a meaningful assessment");
  expect(episodes[0]["evidence"]["next_same_owner"].MemberCount() == 1
      && episodes[0]["evidence"]["next_same_owner"]["episode_id"] == "b", "neighbor facts were not shared by episode identity");
  expect(!episodes[1]["evidence"]["previous_same_owner"].HasMember("step_index")
      && episodes[1]["evidence"]["previous_same_owner"]["terminal_status"] == "unknown"
      && episodes[2]["evidence"]["next_same_owner"].HasMember("step_index"), "projection hid inconsistent or unavailable evidence");
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
      + ",\"dialogue_id\":\"d-content\",\"node_id\":\"" + node_id + "\"";
  if(round > 0) {
    output += ",\"round\":" + std::to_string(round);
  }
  output += ",\"data\":" + data + '}';
  return output;
}

std::string messages_with(const std::vector<std::string>& tool_call_ids) {
  std::string output = "[";
  for(std::size_t index = 0; index < tool_call_ids.size(); ++index) {
    if(index > 0) {
      output += ',';
    }
    output += "{\"role\":\"tool\",\"tool_call_id\":\""
        + tool_call_ids[index] + "\",\"content\":\"network timeout\"}";
  }
  output += ']';
  return output;
}

void append_failed_round(
    std::vector<std::string>* lines,
    int* sequence,
    std::uint64_t round,
    const std::string& call_id,
    const std::string& secret,
    const std::vector<std::string>& consumed_ids) {
  constexpr std::uint64_t base = 2000;
  lines->push_back(event(
      "skill", (*sequence)++, base, "llm.request", round, "skill_executor",
      "{\"task_type\":\"skill\",\"skill_name\":\"poster-maker\","
      "\"exec_skill_key\":\"exec-poster\",\"messages\":"
          + messages_with(consumed_ids) + '}'));
  lines->push_back(event(
      "skill", (*sequence)++, base, "llm.response", round, "skill_executor",
      "{\"skill_name\":\"poster-maker\",\"response\":{\"choices\":[{"
      "\"finish_reason\":\"tool_calls\"}]}}"));
  lines->push_back(event(
      "skill", (*sequence)++, base, "loop.control", round, "skill_executor",
      "{\"scope\":\"skill\",\"action\":\"continue\","
      "\"reason\":\"tool_calls_requested\",\"skill_name\":\"poster-maker\"}"));
  lines->push_back(event(
      "skill", (*sequence)++, base, "tool.call", round, "skill_executor",
      "{\"skill_name\":\"poster-maker\",\"tool_calls\":[{"
      "\"tool_call_id\":\"" + call_id
          + "\",\"tool_name\":\"generate_poster\",\"arguments\":{"
            "\"prompt\":\"霓虹海报\",\"vendor_value\":\"" + secret
          + "\"}}]}"));
  lines->push_back(event(
      "skill", (*sequence)++, base, "tool.result", round, "skill_executor",
      "{\"skill_name\":\"poster-maker\",\"tool_results\":[{"
      "\"tool_call_id\":\"" + call_id
          + "\",\"tool_name\":\"generate_poster\",\"status\":\"error\","
            "\"result\":{\"status\":\"failed\","
            "\"error\":\"network timeout " + secret + "\"}}]}"));
}

std::vector<std::string> content_trace(const std::string& secret) {
  std::vector<std::string> lines = {
      "{\"type\":\"dialogue.header\",\"version\":2,"
      "\"dialogue_id\":\"d-content\",\"conversation_id\":\"conv-content\","
      "\"user_id\":\"user-content\"}",
      event(
          "coord1", 1, 1000, "dialogue.start", 0, "coordinator_decide",
          "{\"input\":\"请生成霓虹海报，私密串 " + secret
              + " 绝不能外传\",\"trigger_type\":\"user\"}"),
      event(
          "coord1", 2, 1000, "coordinator.activation", 0,
          "coordinator_decide", "{\"activation_index\":1}"),
      event(
          "coord1", 3, 1000, "llm.request", 1, "coordinator_decide",
          "{\"task_type\":\"coordinator\",\"activation_index\":1,"
          "\"messages\":[{\"role\":\"user\",\"content\":\"请生成霓虹海报\"}]}"),
      event(
          "coord1", 4, 1000, "llm.response", 1, "coordinator_decide",
          "{\"response\":{\"choices\":[{\"finish_reason\":\"tool_calls\"}]}}"),
      event(
          "coord1", 5, 1000, "loop.control", 1, "coordinator_decide",
          "{\"scope\":\"coordinator\",\"action\":\"continue\","
          "\"reason\":\"tool_calls_requested\"}"),
      event(
          "coord1", 6, 1000, "skill.dispatch", 1, "coordinator_decide",
          "{\"status\":\"dispatched\",\"batch_id\":\"batch1\","
          "\"skill_names\":[\"poster-maker\"],\"dispatches\":[{"
          "\"skill_name\":\"poster-maker\",\"prompt\":\"处理霓虹海报 "
              + secret + "\",\"force\":false,"
              "\"exec_skill_key\":\"exec-poster\"}]}"),
      event(
          "skill", 1, 2000, "skill.start", 0, "skill_executor",
          "{\"skill_name\":\"poster-maker\",\"skill_prompt\":\"生成霓虹海报 "
              + secret + "\",\"exec_skill_key\":\"exec-poster\","
              "\"batch_id\":\"batch1\",\"max_rounds\":10}"),
  };

  int sequence = 2;
  std::vector<std::string> consumed;
  for(int round = 1; round <= 4; ++round) {
    const std::string call_id = "call-" + std::to_string(round);
    append_failed_round(
        &lines,
        &sequence,
        static_cast<std::uint64_t>(round),
        call_id,
        secret,
        consumed);
    consumed.push_back(call_id);
  }
  lines.push_back(event(
      "skill", sequence++, 2000, "llm.request", 5, "skill_executor",
      "{\"task_type\":\"skill\",\"skill_name\":\"poster-maker\","
      "\"exec_skill_key\":\"exec-poster\",\"messages\":"
          + messages_with(consumed) + '}'));
  lines.push_back(event(
      "skill", sequence++, 2000, "llm.response", 5, "skill_executor",
      "{\"skill_name\":\"poster-maker\",\"response\":{\"choices\":[{"
      "\"finish_reason\":\"stop\",\"message\":{\"content\":"
      "\"多次网络超时，无法生成\"}}]}}"));
  lines.push_back(event(
      "skill", sequence++, 2000, "loop.control", 5, "skill_executor",
      "{\"scope\":\"skill\",\"action\":\"stop\","
      "\"reason\":\"final_response\",\"skill_name\":\"poster-maker\"}"));
  lines.push_back(event(
      "skill", sequence++, 2000, "skill.end", 5, "skill_executor",
      "{\"skill_name\":\"poster-maker\",\"exec_skill_key\":\"exec-poster\","
      "\"batch_id\":\"batch1\",\"status\":\"tool_error\","
      "\"skill_result\":\"生成失败：network timeout\"}"));
  lines.push_back(event(
      "coord2", 1, 3000, "coordinator.activation", 0,
      "coordinator_decide", "{\"activation_index\":2,\"batch_id\":\"batch1\"}"));
  lines.push_back(event(
      "coord2", 2, 3000, "batch.result", 0, "coordinator_decide",
      "{\"batch_id\":\"batch1\",\"status\":\"false\","
      "\"workflow_status\":\"completed\",\"successful_count\":0,"
      "\"total_count\":1,\"results\":[{\"exec_skill_key\":\"exec-poster\","
      "\"skill_status\":\"tool_error\"}]}"));
  lines.push_back(event(
      "coord2", 3, 3000, "llm.request", 2, "coordinator_decide",
      "{\"task_type\":\"coordinator\",\"activation_index\":2,"
      "\"messages\":[{\"role\":\"user\",\"content\":\"汇总结果\"}]}"));
  lines.push_back(event(
      "coord2", 4, 3000, "llm.response", 2, "coordinator_decide",
      "{\"response\":{\"choices\":[{\"finish_reason\":\"stop\","
      "\"message\":{\"content\":\"生成失败\"}}]}}"));
  lines.push_back(event(
      "coord2", 5, 3000, "loop.control", 2, "coordinator_decide",
      "{\"scope\":\"coordinator\",\"action\":\"stop\","
      "\"reason\":\"final_response\"}"));
  lines.push_back(event(
      "coord2", 6, 3000, "dialogue.end", 0, "coordinator_decide",
      "{\"status\":\"normal\",\"response_text\":\"生成失败\"}"));
  return lines;
}

class TempTrace {
 public:
  TempTrace(const std::string& name, const std::vector<std::string>& lines) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path()
        / ("agentflow-session-eval-" + name + '-'
           + std::to_string(nonce) + ".jsonl");
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if(!output) {
      throw std::runtime_error("cannot create SessionLog fixture");
    }
    for(const auto& line : lines) {
      output << line << '\n';
    }
    if(!output.flush()) {
      throw std::runtime_error("cannot write SessionLog fixture");
    }
  }

  ~TempTrace() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

std::string serialize_json(const rapidjson::Value& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

std::vector<std::string> selected_episode_ids(
    const trajectory::ModelRequest& request) {
  const std::string marker = "输入证据：\n";
  const std::size_t begin = request.user_prompt.find(marker);
  const std::size_t end = request.user_prompt.find(
      "\n只返回一个 JSON 对象", begin);
  if(begin == std::string::npos || end == std::string::npos) {
    throw std::runtime_error("model request does not contain evidence JSON");
  }
  const std::size_t json_begin = begin + marker.size();
  const std::string json = request.user_prompt.substr(json_begin, end - json_begin);
  rapidjson::Document document;
  document.Parse(json.data(), json.size());
  if(document.HasParseError() || !document.IsObject()) {
    throw std::runtime_error("model request evidence JSON is invalid");
  }
  const auto member = document.FindMember("selected_episode_ids");
  if(member == document.MemberEnd() || !member->value.IsArray()) {
    throw std::runtime_error("model request has no selected episode IDs");
  }
  std::vector<std::string> output;
  for(const auto& value : member->value.GetArray()) {
    output.emplace_back(value.GetString(), value.GetStringLength());
  }
  return output;
}

class CapturingAdapter final : public trajectory::ModelAdapter {
 public:
  explicit CapturingAdapter(bool invalid_evidence = false)
      : invalid_evidence_(invalid_evidence) {}

  trajectory::RawModelResponse evaluate(
      const trajectory::ModelRequest& request) override {
    requests.push_back(request);
    const auto episode_ids = selected_episode_ids(request);
    const std::string event_id = invalid_evidence_
        ? "event-not-recorded" : request.allowed_event_ids.front();

    rapidjson::Document response;
    response.SetObject();
    auto& allocator = response.GetAllocator();
    rapidjson::Value task(rapidjson::kObjectType);
    task.AddMember("status", "failed", allocator);
    task.AddMember("confidence", 0.91, allocator);
    task.AddMember("reason", "工具持续失败，用户任务没有完成。", allocator);
    rapidjson::Value task_evidence(rapidjson::kArrayType);
    task_evidence.PushBack(
        rapidjson::Value(event_id.c_str(), allocator), allocator);
    task.AddMember("evidence_event_ids", task_evidence, allocator);
    response.AddMember("task_outcome", task, allocator);

    rapidjson::Value assessments(rapidjson::kArrayType);
    for(const auto& episode_id : episode_ids) {
      rapidjson::Value item(rapidjson::kObjectType);
      item.AddMember(
          "episode_id", rapidjson::Value(episode_id.c_str(), allocator), allocator);
      item.AddMember("verdict", "useful", allocator);
      item.AddMember("confidence", 0.73, allocator);
      item.AddMember("expected_state_change", "应推进海报生成", allocator);
      item.AddMember("observed_state_change", "记录了本轮执行结果", allocator);
      if(compact) { item.RemoveMember("expected_state_change"); item.RemoveMember("observed_state_change"); }
      item.AddMember("concise_reason", "该步骤留下了可供后续判断的结果。", allocator);
      rapidjson::Value evidence(rapidjson::kArrayType);
      evidence.PushBack(rapidjson::Value(event_id.c_str(), allocator), allocator);
      item.AddMember("evidence_event_ids", evidence, allocator);
      assessments.PushBack(item, allocator);
    }
    response.AddMember("episode_assessments", assessments, allocator);

    rapidjson::Value reflection(rapidjson::kObjectType);
    reflection.AddMember("summary", "应减少无依据的重复调用。", allocator);
    for(const char* name : {
             "root_causes", "skill_orchestration_changes", "experience_injections",
             "stop_condition_changes", "wasted_episode_ids"}) {
      reflection.AddMember(
          rapidjson::Value(name, allocator),
          rapidjson::Value(rapidjson::kArrayType),
          allocator);
    }
    rapidjson::Value reflection_evidence(rapidjson::kArrayType);
    reflection_evidence.PushBack(
        rapidjson::Value(event_id.c_str(), allocator), allocator);
    reflection.AddMember(
        "evidence_event_ids", reflection_evidence, allocator);
    response.AddMember("reflection", reflection, allocator);

    if(!duplicate_section.empty()) {
      auto& section = duplicate_section == "root" ? static_cast<rapidjson::Value&>(response)
          : duplicate_section == "episode" ? response["episode_assessments"][0] : response["reflection"];
      rapidjson::Value extra, key;
      extra.CopyFrom(section.MemberBegin()->value, allocator);
      key.CopyFrom(section.MemberBegin()->name, allocator);
      section.AddMember(key, extra, allocator);
    }

    trajectory::RawModelResponse output;
    output.transport_ok = true;
    output.adapter = name();
    output.model_id = model_id();
    output.attempts = 1;
    output.body = response_prefix + serialize_json(response) + response_suffix;
    last_response_body = output.body;
    return output;
  }

  std::string name() const override {
    return "capturing-test";
  }

  std::string model_id() const override {
    return "test-model";
  }

  std::vector<trajectory::ModelRequest> requests;
  std::string last_response_body;
  std::string response_prefix;
  std::string response_suffix;
  std::string duplicate_section;
  bool compact = false;

 private:
  bool invalid_evidence_ = false;
};

class ExperienceAdapter final : public trajectory::ModelAdapter {
 public:
  explicit ExperienceAdapter(std::string mode = "mixed") : mode_(std::move(mode)) {}
  trajectory::RawModelResponse evaluate(const trajectory::ModelRequest& request) override {
    CapturingAdapter base;
    auto result = base.evaluate(request);
    expect(request.user_prompt.find("allowed_experience_scopes") != std::string::npos,
           "model was not given allowed recipients");
    rapidjson::Document response;
    response.Parse(result.body.c_str());
    auto& allocator = response.GetAllocator();
    auto& items = response["reflection"]["experience_injections"];
    if(mode_ == "missing") {
      response["reflection"].RemoveMember("experience_injections");
    } else if(mode_ == "legacy") {
      items.PushBack(rapidjson::Value("旧的纯文本建议仍能读取。", allocator), allocator);
    } else {
      auto add = [&](const char* scope, const char* condition, const char* advice, const std::string& id) {
        rapidjson::Value item(rapidjson::kObjectType), evidence(rapidjson::kArrayType);
        item.AddMember("target_scope", rapidjson::Value(scope, allocator), allocator);
        item.AddMember("trigger_pattern", rapidjson::Value(condition, allocator), allocator);
        item.AddMember("proposed_change", rapidjson::Value(advice, allocator), allocator);
        evidence.PushBack(rapidjson::Value(id.c_str(), allocator), allocator);
        item.AddMember("evidence_event_ids", evidence, allocator);
        items.PushBack(item, allocator);
      };
      const auto& first = request.allowed_event_ids.front();
      const auto& last = request.allowed_event_ids.back();
      add("coordinator", "海报生成缺少输入素材时", "先确认素材是否齐备，独立检索分支继续。", first);
      add("skill:poster-maker", "海报工具返回网络超时且暂无恢复依据时", "遵守当前重试预算；有恢复依据再尝试。", last);
      if(mode_ == "mixed") {
        add("coordinator", "海报生成缺少输入素材时", "先确认素材是否齐备，独立检索分支继续。", first);
        add("skill:made-up", "海报缺少输入时", "先确认素材。", first);
        add("coordinator", "validated_dialogue_reflection", "先确认素材。", first);
        add("coordinator", "海报缺少输入时", "先确认素材。", "invented-event");
        add("coordinator", "海报缺少输入时", "   ", first);
        add("coordinator", "海报缺少输入时", "先确认素材。", first);
        items[items.Size() - 1].AddMember("approved", true, allocator);
        items.PushBack(rapidjson::Value("旧文本仅供分析。", allocator), allocator);
      }
    }
    result.body = serialize_json(response);
    return result;
  }
  std::string name() const override { return "experience-contract-test"; }
  std::string model_id() const override { return "controlled-model"; }
 private:
  std::string mode_;
};

void test_scoped_experience_end_to_end_and_item_isolation() {
  namespace te = ::trajectory::evolution;
  TempTrace trace("scoped-experiences", content_trace("safe-placeholder-value"));
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  const auto evaluated = trajectory::HybridTrajectoryEvaluator(options, std::make_shared<ExperienceAdapter>())
                             .evaluate_sessionlog(trace.path());
  const auto& report = evaluated.report;
  expect(report.model_succeeded_count() == 1, "bad experience discarded valid dialogue judgments");
  expect(report.reflection().experience_details_size() == 2, "invalid/duplicate experiences were not isolated");
  expect(report.reflection().experience_injections_size() == 3, "legacy readable view lost compatibility");
  expect(std::any_of(report.issues().begin(), report.issues().end(), [](const auto& issue) {
    return issue.code() == "invalid_experience_scope";
  }), "scope rejection missing from audit");
  expect(std::any_of(report.issues().begin(), report.issues().end(), [](const auto& issue) {
    return issue.code() == "invalid_experience_evidence";
  }), "fabricated evidence rejection missing from audit");

  trajectory::EvolutionOptions evolution;
  evolution.min_supporting_dialogues = 1; evolution.min_supporting_episodes = 1;
  trajectory::TrajectoryEvolutionPipeline pipeline({}, evolution);
  const auto batch = pipeline.synthesize_reports({report});
  ep::EvolutionMemoryStore memory;
  pipeline.merge_memory(batch.report, &memory);
  std::size_t scoped_count = 0;
  for(const auto& entry : memory.entries()) {
    if(!trajectory::is_scoped_experience(entry.candidate())) continue;
    ++scoped_count;
    const auto& candidate = entry.candidate();
    const auto matching = std::find_if(report.reflection().experience_details().begin(),
        report.reflection().experience_details().end(), [&](const auto& advice) {
          return advice.target_scope() == candidate.target_scope();
        });
    expect(matching != report.reflection().experience_details().end(), "recipient was inferred again");
    expect(candidate.trigger_pattern() == matching->trigger_pattern(), "condition was replaced by a source label");
    expect(candidate.support_evidence_size() == 1
               && candidate.support_evidence(0).evidence_event_ids_size() == 1
               && candidate.support_evidence(0).evidence_event_ids(0) == matching->evidence_event_ids(0),
           "experience inherited unrelated dialogue evidence");
  }
  expect(scoped_count == 2, "structured experience did not reach memory or legacy text duplicated it");
  te::v1::Identity identity;
  identity.set_agent_id("test-agent"); identity.set_user_id(report.source().user_id());
  identity.set_conversation_id("new-conversation"); identity.set_run_id("new-run");
  te::v1::ShadowOptions limits;
  limits.add_target_scopes("coordinator"); limits.add_target_scopes("skill:poster-maker");
  limits.set_max_policy_entries(64); limits.set_max_advice_items(3); limits.set_max_advice_bytes(4096);
  limits.set_max_query_bytes(16384); limits.set_max_memory_bytes(1048576); limits.set_max_snapshot_bytes(262144);
  limits.set_max_match_terms(128); limits.set_min_matched_terms(2);
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  expect(snapshot.experiences_size() == 2, "scoped experience was not eligible for a snapshot");
  te::v1::RuntimeOptions runtime_options;
  runtime_options.set_requested_level(te::v1::L1); runtime_options.set_fallback("strict");
  runtime_options.add_host_capabilities("context.request"); runtime_options.add_allowed_users(identity.user_id());
  runtime_options.set_experience_template("参考经验，先确认适用条件：{{experience}}");
  runtime_options.set_max_context_bytes(4096);
  for(const auto& item : snapshot.experiences()) {
    auto* approval = runtime_options.add_approved_experiences();
    approval->set_memory_id(item.memory_id()); approval->set_content_revision(item.content_revision());
  }
  const te::PolicyRuntime runtime(te::compile_runtime_policy(snapshot, runtime_options));
  te::v1::RuntimeQuery query;
  auto* advice_query = query.mutable_advice_query();
  *advice_query->mutable_identity() = identity;
  advice_query->set_query_id("q-new"); advice_query->set_decision_id("decision-new");
  advice_query->set_target_scope("coordinator"); advice_query->set_task("海报生成缺少输入素材");
  query.set_boundary("llm"); query.set_execution_id("coordinator");
  const auto decision = runtime.decide(query);
  expect(decision.status() == "ready" && decision.context().find("海报生成缺少输入素材时") != std::string::npos,
         "condition did not survive evaluation -> memory -> injection");
  expect(decision.experiences_size() == 1, "recipient scope was crossed during injection");

  for(const auto* mode : {"legacy", "missing"}) {
    const auto compatible = trajectory::HybridTrajectoryEvaluator(options, std::make_shared<ExperienceAdapter>(mode))
                                .evaluate_sessionlog(trace.path());
    expect(compatible.report.model_succeeded_count() == 1
               && compatible.report.reflection().experience_details().empty(),
           "legacy/invalid experience section broke the dialogue evaluation");
  }
}

void test_content_aware_single_call_and_redaction() {
  const std::string secret = "vendor-opaque-secret-9f8e7d6c5b4a";
  expect(
      setenv("AF_TEST_TRAJECTORY_VALUE", secret.c_str(), 1) == 0,
      "failed to register test secret");
  TempTrace trace("content", content_trace(secret));
  auto adapter = std::make_shared<CapturingAdapter>();
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  options.redaction_secrets.push_back(secret);
  const trajectory::EvaluationOutput output =
      trajectory::HybridTrajectoryEvaluator(options, adapter)
          .evaluate_sessionlog(trace.path());
  unsetenv("AF_TEST_TRAJECTORY_VALUE");

  expect(adapter->requests.size() == 1, "dialogue must use one model call");
  expect(output.model_requests.size() == 1, "one replayable request is expected");
  expect(output.report.assessments_size() > 2, "fixture must yield multiple episodes");
  expect(output.report.model_requested_count() == 1, "request count is per dialogue");
  expect(output.report.model_succeeded_count() == 1, "model call should succeed");
  expect(
      output.report.model_prompt_version() == "content-aware-dialogue-v3",
      "dialogue response contract version is stale");
  expect(
      output.report.dialogue_assessment().task_outcome()
          == ep::AGGREGATE_STATUS_FAILED,
      "dialogue task result was not stored");
  expect(
      output.report.reflection().summary().find("重复调用") != std::string::npos,
      "reflection was not stored");
  expect(output.report.content_event_count() > 10, "content events are missing");
  expect(output.report.redaction_count() > 0, "secret redaction was not counted");

  const std::string& prompt = adapter->requests.front().user_prompt;
  expect(prompt.find("请生成霓虹海报") != std::string::npos, "user input is absent");
  expect(prompt.find("skill_prompt") != std::string::npos, "Skill Prompt is absent");
  expect(
      prompt.find("\"experience_injections\":[]") != std::string::npos,
      "experience injection response field is absent");
  expect(
      prompt.find("\"prompt_changes\"") == std::string::npos,
      "deprecated prompt evolution field is still requested");
  expect(prompt.find("霓虹海报") != std::string::npos, "Tool argument is absent");
  expect(prompt.find("network timeout") != std::string::npos, "Tool result is absent");
  expect(prompt.find(secret) == std::string::npos, "environment secret leaked");
  expect(
      prompt.find("<REDACTED:ENV_SECRET>") != std::string::npos,
      "redaction marker is absent");
  expect(
      !std::filesystem::exists(trace.path().string() + ".analysis.json"),
      "direct mode must not create an intermediate analysis file");

  bool preserved_hard_rule = false;
  for(const auto& assessment : output.report.assessments()) {
    if(assessment.rule().verdict() == ep::SEMANTIC_VERDICT_INVALID) {
      preserved_hard_rule = assessment.final_verdict()
              == ep::SEMANTIC_VERDICT_INVALID
          && assessment.rule_model_conflict();
      break;
    }
  }
  expect(preserved_hard_rule, "model must not override a hard invalid rule");

  // Repeated histories below each field limit used to exceed the whole prompt
  // limit. Exercise the production sanitizer/packer, not a substitute encoder.
  const std::string repeated = "真实历史内容。" + std::string(4096, 'x') + secret;
  const std::string safe_repeated = "真实历史内容。" + std::string(4096, 'x') + "<REDACTED:ENV_SECRET>";
  auto history_trace = [&](bool unique) {
    std::vector<std::string> result;
    std::size_t ordinal = 0;
    for(const auto& line : content_trace(secret)) {
      rapidjson::Document event;
      event.Parse(line.c_str());
      if(std::string(event["type"].GetString()) == "llm.request") {
        auto& allocator = event.GetAllocator();
        rapidjson::Value history(rapidjson::kArrayType);
        for(std::size_t i = 0; i < 96; ++i) {
          const auto value = repeated + (unique ? std::to_string(ordinal) + ":" + std::to_string(i) : "");
          history.PushBack(rapidjson::Value(value.c_str(), allocator), allocator);
        }
        event["data"].AddMember("history/~", history, allocator);
        rapidjson::Value literal(rapidjson::kObjectType);
        literal.AddMember("$content_ref", "/not-generated", allocator);
        event["data"].AddMember("literal", literal, allocator);
        ++ordinal;
      }
      result.push_back(serialize_json(event));
    }
    return result;
  };
  TempTrace large("shared-content", history_trace(false));
  auto shared_adapter = std::make_shared<CapturingAdapter>();
  const auto shared = trajectory::HybridTrajectoryEvaluator(options, shared_adapter).evaluate_sessionlog(large.path());
  expect(shared.report.model_succeeded_count() == 1, "repeated history still blocks evaluation");
  expect(shared.report.redaction_count() == output.report.redaction_count() + 7 * 96,
      "request-local sanitizer cache lost per-occurrence redaction counts");
  const auto& packed = shared_adapter->requests.front().user_prompt;
  expect(packed.size() < 200000, "repeated histories were not compacted");
  const auto begin = packed.find("输入证据：\n") + std::string("输入证据：\n").size();
  const auto end = packed.find("\n只返回一个 JSON 对象", begin);
  rapidjson::Document payload;
  payload.Parse(packed.substr(begin, end - begin).c_str());
  expect(!payload.HasParseError() && payload.HasMember("content_value_refs"), "sharing registry missing");
  expect(payload["truncation_count"].GetUint64() == 0, "sharing introduced content truncation");
  std::map<std::string, std::string> aliases;
  for(const auto& entry : payload["content_value_refs"].GetArray())
    for(const auto& path : entry["paths"].GetArray())
      expect(aliases.emplace(path.GetString(), entry["source"].GetString()).second, "duplicate reference path");
  rapidjson::Document restored;
  std::set<std::string> active;
  auto restore = [&](auto&& self, const rapidjson::Value& value, const std::string& path) -> rapidjson::Value {
    const auto found = aliases.find(path);
    if(found != aliases.end()) {
      expect(active.insert(path).second, "cyclic content reference");
      expect(value.IsObject() && value.HasMember("$content_ref") && value["$content_ref"] == found->second.c_str(), "reference wrapper changed");
      const auto* original = rapidjson::Pointer(found->second.c_str()).Get(payload);
      expect(original != nullptr, "reference source is missing");
      auto result = self(self, *original, found->second);
      active.erase(path);
      return result;
    }
    rapidjson::Value result;
    auto& allocator = restored.GetAllocator();
    if(value.IsArray()) {
      result.SetArray();
      for(rapidjson::SizeType i = 0; i < value.Size(); ++i) result.PushBack(self(self, value[i], path + '/' + std::to_string(i)), allocator);
    } else if(value.IsObject()) {
      result.SetObject();
      for(auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        std::string token;
        for(char c : std::string(it->name.GetString())) token += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
        rapidjson::Value name;
        name.CopyFrom(it->name, allocator);
        result.AddMember(name, self(self, it->value, path + '/' + token), allocator);
      }
    } else result.CopyFrom(value, allocator);
    return result;
  };
  auto content = restore(restore, payload["content_events"], "/content_events");
  std::size_t histories = 0;
  for(const auto& event : content.GetArray()) if(event["data"].HasMember("history/~")) {
    ++histories;
    expect(event["data"]["history/~"].Size() == 96, "history length changed");
    for(const auto& value : event["data"]["history/~"].GetArray()) expect(value == safe_repeated.c_str(), "history text changed");
    expect(event["data"]["literal"]["$content_ref"] == "/not-generated", "unregistered user object was interpreted as a reference");
  }
  expect(histories == 7 && shared_adapter->requests.front().allowed_event_ids == adapter->requests.front().allowed_event_ids,
      "sharing changed event identity or count");
  expect(packed.find(secret) == std::string::npos, "sharing bypassed redaction");
  for(const auto& episode : payload["episodes"].GetArray())
    expect(!episode["evidence"].HasMember("packet_id") && !episode["evidence"].HasMember("input_fingerprint"), "local analysis metadata leaked into content request");
  TempTrace unique("unique-content", history_trace(true));
  auto no_call = std::make_shared<CapturingAdapter>();
  const auto oversized = trajectory::HybridTrajectoryEvaluator(options, no_call).evaluate_sessionlog(unique.path());
  expect(no_call->requests.empty() && oversized.report.dialogue_assessment().error_code() == "model_request_too_large", "unique oversized evidence bypassed budget");
}

void test_clean_suspicious_mode_still_assesses_dialogue() {
  const std::vector<std::string> lines = {
      "{\"type\":\"dialogue.header\",\"version\":2,"
      "\"dialogue_id\":\"d-content\",\"conversation_id\":\"conv-content\"}",
      event(
          "clean", 1, 5000, "dialogue.start", 0, "coordinator_decide",
          "{\"input\":\"请直接回答你好\"}"),
      event(
          "clean", 2, 5000, "coordinator.activation", 0,
          "coordinator_decide", "{\"activation_index\":1}"),
      event(
          "clean", 3, 5000, "llm.request", 1, "coordinator_decide",
          "{\"task_type\":\"coordinator\",\"activation_index\":1,"
          "\"messages\":[{\"role\":\"user\",\"content\":\"你好\"}]}"),
      event(
          "clean", 4, 5000, "llm.response", 1, "coordinator_decide",
          "{\"response\":{\"choices\":[{\"finish_reason\":\"stop\","
          "\"message\":{\"content\":\"你好\"}}]}}"),
      event(
          "clean", 5, 5000, "loop.control", 1, "coordinator_decide",
          "{\"scope\":\"coordinator\",\"action\":\"stop\","
          "\"reason\":\"final_response\"}"),
      event(
          "clean", 6, 5000, "dialogue.end", 0, "coordinator_decide",
          "{\"status\":\"normal\",\"response_text\":\"你好\"}"),
  };
  TempTrace trace("clean", lines);
  auto adapter = std::make_shared<CapturingAdapter>();
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::suspicious;
  const trajectory::EvaluationOutput output =
      trajectory::HybridTrajectoryEvaluator(options, adapter)
          .evaluate_sessionlog(trace.path());
  expect(adapter->requests.size() == 1, "clean dialogue still needs task assessment");
  expect(
      adapter->requests.front().user_prompt.find(
          "当前为 0 项，必须原样返回空数组 []") != std::string::npos,
      "empty suspicious selection must have an explicit response contract");
  expect(
      adapter->requests.front().user_prompt.find(
          "即使 task_outcome.status 为 unknown 也不能返回空数组")
          != std::string::npos,
      "unknown task outcome must still require concrete evidence");
  expect(
      output.report.model_succeeded_count() == 1,
      "clean dialogue task assessment should succeed");
  expect(
      output.report.assessments(0).model().status()
          == ep::MODEL_CALL_STATUS_NOT_REQUESTED,
      "clean episode must not be selected as suspicious");
}

void test_invalid_dialogue_response_degrades_atomically() {
  TempTrace trace("invalid-response", content_trace("safe-placeholder-value"));
  auto adapter = std::make_shared<CapturingAdapter>(true);
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  const trajectory::EvaluationOutput output =
      trajectory::HybridTrajectoryEvaluator(options, adapter)
          .evaluate_sessionlog(trace.path());

  expect(adapter->requests.size() == 1, "invalid response still uses one call");
  expect(output.report.model_failed_count() == 1, "failure count is per dialogue");
  expect(output.report.model_failures_degraded(), "failure must degrade to rules");
  expect(
      output.report.dialogue_assessment().status()
          == ep::MODEL_CALL_STATUS_INVALID_RESPONSE,
      "dialogue response should be rejected");
  for(const auto& assessment : output.report.assessments()) {
    expect(
        assessment.model().status() == ep::MODEL_CALL_STATUS_INVALID_RESPONSE,
        "all selected episode judgments must be rejected atomically");
    expect(
        assessment.decision_source() != ep::DECISION_SOURCE_MODEL
            && assessment.decision_source() != ep::DECISION_SOURCE_HYBRID,
        "invalid model output must not affect final decisions");
  }
}

void test_compact_assessments_and_credential_slice() {
  const std::string secret = "opaqueSensitiveBoundaryValue987654321";
  auto lines = content_trace("safe-placeholder-value");
  lines[1] = event("coord1", 1, 1000, "dialogue.start", 0, "coordinator_decide",
      "{\"input\":\"" + std::string(32760, ' ') + secret + "\"}");
  TempTrace trace("credential-slice", lines);
  trajectory::EvaluatorOptions options;
  options.redaction_secrets = {secret};
  options.model_selection = trajectory::ModelSelection::all;
  auto adapter = std::make_shared<CapturingAdapter>();
  adapter->compact = true;
  const auto output = trajectory::HybridTrajectoryEvaluator(options, adapter).evaluate_sessionlog(trace.path());
  expect(output.report.model_succeeded_count() == 1 && adapter->requests.size() == 1,
      "compact assessments were rejected");
  expect(adapter->requests.front().user_prompt.find(secret.substr(0, 8)) == std::string::npos,
      "truncation exposed a credential prefix");
  expect(output.report.redaction_count() > 0, "credential boundary redaction was not counted");
  for(const auto& item : output.report.assessments())
    expect(item.model().expected_state_change().empty() && item.model().observed_state_change().empty(),
        "omitted model fields were fabricated locally");
}

void test_single_json_envelope_without_extra_calls() {
  TempTrace trace("json-envelope", content_trace("safe-placeholder-value"));
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  options.strict_model = true;
  for(const std::string prefix : {"```json\n", "```\n", "```json\r\n", "```\r\n"}) {
    auto adapter = std::make_shared<CapturingAdapter>();
    adapter->response_prefix = " \n\t" + prefix;
    adapter->response_suffix = "\r\n``` \r\n";
    const auto output = trajectory::HybridTrajectoryEvaluator(options, adapter).evaluate_sessionlog(trace.path());
    expect(output.report.model_succeeded_count() == 1 && adapter->requests.size() == 1,
           "one JSON envelope must pass without another model call");
    expect(std::any_of(output.report.issues().begin(), output.report.issues().end(), [](const auto& issue) {
      return issue.code() == "dialogue_model_json_fence_unwrapped";
    }), "JSON envelope normalization must remain observable without saving raw content");
  }
}

void test_json_envelope_does_not_extract_arbitrary_prose() {
  TempTrace trace("json-envelope-invalid", content_trace("safe-placeholder-value"));
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  struct Wrapper { const char* prefix; const char* suffix; };
  for(const auto& wrapper : {Wrapper{"explanation\n```json\n", "\n```"},
      Wrapper{"```json\n", "\n```\nexplanation"}, Wrapper{"```python\n", "\n```"},
      Wrapper{"```json\n", "\n"}, Wrapper{"```json\n", "\n```\n```json\n{}\n```"},
      Wrapper{"```json\n[", "]\n```"}}) {
    auto adapter = std::make_shared<CapturingAdapter>();
    adapter->response_prefix = wrapper.prefix;
    adapter->response_suffix = wrapper.suffix;
    const auto output = trajectory::HybridTrajectoryEvaluator(options, adapter).evaluate_sessionlog(trace.path());
    expect(output.report.model_failed_count() == 1
        && output.report.dialogue_assessment().error_code() == "invalid_json",
        "prose, multiple blocks and non-object responses must remain invalid");
  }
  options.strict_model = true;
  auto adapter = std::make_shared<CapturingAdapter>();
  adapter->response_prefix = "```json\n";
  bool rejected = false;
  try { trajectory::HybridTrajectoryEvaluator(options, adapter).evaluate_sessionlog(trace.path()); }
  catch(const trajectory::EvaluationError&) { rejected = true; }
  expect(rejected, "strict mode must still reject malformed JSON envelopes");
  options.strict_model = false;
  for(const auto* section : {"root", "episode", "reflection"}) {
    auto duplicate = std::make_shared<CapturingAdapter>();
    duplicate->duplicate_section = section;
    const auto result = trajectory::HybridTrajectoryEvaluator(options, duplicate).evaluate_sessionlog(trace.path());
    expect(result.report.model_failed_count() == 1
        && result.report.dialogue_assessment().error_code() == "invalid_json",
        "duplicate model fields bypassed content validation");
  }
}

void test_json_envelope_preserves_evidence_and_privacy_checks() {
  TempTrace trace("json-envelope-gates", content_trace("safe-placeholder-value"));
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  auto adapter = std::make_shared<CapturingAdapter>(true);
  adapter->response_prefix = "```json\n";
  adapter->response_suffix = "\n```";
  const auto invalid = trajectory::HybridTrajectoryEvaluator(options, adapter).evaluate_sessionlog(trace.path());
  expect(invalid.report.dialogue_assessment().error_code() == "invalid_task_outcome",
         "unwrapping must not permit fabricated evidence");
  options.redaction_secrets.push_back("fixture-sensitive-envelope-value");
  adapter->response_prefix = "fixture-sensitive-envelope-value\n```json\n";
  const auto sensitive = trajectory::HybridTrajectoryEvaluator(options, adapter).evaluate_sessionlog(trace.path());
  expect(sensitive.report.dialogue_assessment().error_code() == "credential_like_output",
         "privacy inspection must run on the whole original response");
  expect(trajectory::HybridTrajectoryEvaluator::to_json(sensitive.report).find(options.redaction_secrets.front())
      == std::string::npos, "diagnostics must never retain the credential");
}

void test_dialogue_jsonl_replay() {
  TempTrace trace("replay-source", content_trace("safe-placeholder-value"));
  auto capturing = std::make_shared<CapturingAdapter>();
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  const trajectory::EvaluationOutput first =
      trajectory::HybridTrajectoryEvaluator(options, capturing)
          .evaluate_sessionlog(trace.path());
  expect(first.report.model_succeeded_count() == 1, "capture setup failed");
  const std::string response_line =
      "{\"packet_id\":\"" + capturing->requests.front().packet_id
      + "\",\"judgment\":" + capturing->last_response_body + '}';
  TempTrace responses("replay-responses", {response_line});
  const auto replay_adapter = trajectory::make_jsonl_model_adapter(
      responses.path());
  const trajectory::EvaluationOutput replayed =
      trajectory::HybridTrajectoryEvaluator(options, replay_adapter)
          .evaluate_sessionlog(trace.path());
  expect(
      replayed.report.model_succeeded_count() == 1
          && replayed.report.dialogue_assessment().adapter() == "jsonl-replay",
      "dialogue-level JSONL replay failed");
}

void test_invalid_trace_never_reaches_model() {
  auto lines = content_trace("safe-placeholder-value");
  lines.erase(
      std::remove_if(
          lines.begin(),
          lines.end(),
          [](const std::string& line) {
            return line.find("\"event_id\":\"skill:6\"") != std::string::npos;
          }),
      lines.end());
  TempTrace trace("invalid-trace", lines);
  auto adapter = std::make_shared<CapturingAdapter>();
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  options.allow_invalid_trace = true;
  const trajectory::EvaluationOutput output =
      trajectory::HybridTrajectoryEvaluator(options, adapter)
          .evaluate_sessionlog(trace.path());
  expect(!output.report.source_trace_integrity_valid(), "fixture must be invalid");
  expect(adapter->requests.empty(), "invalid trace must never reach the model");
  expect(output.model_requests.empty(), "invalid trace must not emit model requests");
  expect(
      output.report.dialogue_assessment().error_code() == "invalid_source_trace",
      "invalid trace quarantine reason is missing");
}

}  // namespace

int main() {
  try {
    test_model_input_preserves_application_data();
    test_compact_assessments_and_credential_slice();
    test_content_aware_single_call_and_redaction();
    test_clean_suspicious_mode_still_assesses_dialogue();
    test_invalid_dialogue_response_degrades_atomically();
    test_single_json_envelope_without_extra_calls();
    test_json_envelope_does_not_extract_arbitrary_prose();
    test_json_envelope_preserves_evidence_and_privacy_checks();
    test_dialogue_jsonl_replay();
    test_invalid_trace_never_reaches_model();
    test_scoped_experience_end_to_end_and_item_isolation();
    std::cout << "SessionLog hybrid evaluator tests passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
