// test_standard_trace 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

#include "trajectory_evolution/evolution.h"
#include "agentflow/trajectory/experience.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {
namespace te = trajectory::evolution;
namespace v1 = te::v1;
namespace legacy = agentflow::trajectory;
namespace ep = legacy::evaluation;

// 测试辅助函数：expect。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void expect(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}

template <class Function> void expect_error(Function function, const std::string &message) {
  try {
    function();
  } catch (const legacy::EvaluationError &) {
    return;
  }
  throw std::runtime_error(message);
}

// 解析当前对象的来源，保留来源身份与后续引用所需的数据。
std::filesystem::path source(const char *suffix) {
  return std::filesystem::path(TRACE_TEST_SOURCE) / suffix;
}

// 测试辅助函数：event。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
v1::TraceEvent &event(v1::TraceRun &trace, const std::string &id) {
  for (auto &item : *trace.mutable_events()) {
    if (item.event_id() == id)
      return item;
  }
  throw std::runtime_error("missing fixture event");
}

// 测试辅助函数：has_issue。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
bool has_issue(const te::ValidatedTrace &trace, const std::string &code) {
  return std::any_of(trace.report.issues().begin(), trace.report.issues().end(),
                     [&](const auto &issue) { return issue.code() == code; });
}

// 测试辅助函数：repeated。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
v1::TraceRun repeated(const std::string &run_id = "test-run") {
  auto trace = te::read_trace(source("examples/standard-agent/one.trace.json"));
  trace.mutable_identity()->set_run_id(run_id);
  return trace;
}

// 测试辅助函数：blackbox。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
v1::TraceRun blackbox() {
  auto trace = repeated();
  const auto begin = trace.events(0);
  const auto end = trace.events(trace.events_size() - 1);
  trace.clear_events();
  *trace.add_events() = begin;
  *trace.add_events() = end;
  return trace;
}

// 测试辅助函数：model_options。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
te::EvaluatorOptions model_options() {
  auto options = te::load_runtime_configuration(source("config/standard-runtime.json")).evaluator;
  options.model_selection = te::ModelSelection::all;
  return options;
}

// 测试辅助函数：serialize。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
std::string serialize(const rapidjson::Value &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

class CapturingModel final : public te::ModelAdapter {
public:
  bool counterfeit = false;
  bool include_experience = false;
  std::vector<legacy::ModelRequest> requests;

  // 通过本模块的评测入口处理请求，执行与采集生命周期由对应契约约束。
  legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
    requests.push_back(request);
    const std::string marker = "输入证据：\n";
    const auto begin = request.user_prompt.find(marker);
    const auto end = request.user_prompt.find("\n只返回一个 JSON 对象", begin);
    expect(begin != std::string::npos && end != std::string::npos, "missing evidence");
    const auto json =
        request.user_prompt.substr(begin + marker.size(), end - begin - marker.size());
    rapidjson::Document evidence;
    evidence.Parse(json.data(), json.size());
    expect(!evidence.HasParseError(), "invalid evidence JSON");
    rapidjson::Document response;
    response.Parse(R"({"task_outcome":{"status":"failed","confidence":0.9,
      "reason":"执行正常结束，但缺少权限而没有完成业务任务。","evidence_event_ids":[]},
      "episode_assessments":[],"reflection":{"summary":"遇到权限错误应先取得授权。",
      "root_causes":[],"skill_orchestration_changes":[],"experience_injections":[],
      "stop_condition_changes":[],"wasted_episode_ids":[],"evidence_event_ids":[]}})");
    auto &allocator = response.GetAllocator();
    const auto cited = counterfeit ? "not-a-real-event" : request.allowed_event_ids.front();
    auto add_citation = [&](rapidjson::Value &array) {
      array.PushBack(rapidjson::Value(cited.c_str(), allocator), allocator);
    };
    add_citation(response["task_outcome"]["evidence_event_ids"]);
    add_citation(response["reflection"]["evidence_event_ids"]);
    if (include_experience) {
      expect(evidence["allowed_experience_scopes"].Size() == 1,
             "neutral experience scope must identify the real actor");
      const auto &scope = evidence["allowed_experience_scopes"][0];
      expect(std::string(scope.GetString()) == "agent:invoice-agent/actor:assistant",
             "neutral experience inferred a coordinator or skill");
      rapidjson::Value detail(rapidjson::kObjectType);
      detail.AddMember("target_scope", rapidjson::Value(scope, allocator), allocator);
      detail.AddMember("trigger_pattern", "发票查询返回权限不足且尚未取得授权时", allocator);
      detail.AddMember("proposed_change", "说明权限前置条件，确认授权后再继续查询。", allocator);
      rapidjson::Value ids(rapidjson::kArrayType);
      add_citation(ids);
      detail.AddMember("evidence_event_ids", ids, allocator);
      response["reflection"]["experience_injections"].PushBack(detail, allocator);
    }
    for (const auto &id : evidence["selected_episode_ids"].GetArray()) {
      rapidjson::Document assessment;
      assessment.Parse(R"({"episode_id":"","verdict":"useful","confidence":0.8,
        "expected_state_change":"获取业务结果","observed_state_change":"记录了执行结果",
        "concise_reason":"本步骤提供了可核实的执行证据。","evidence_event_ids":[]})");
      rapidjson::Value copied;
      copied.CopyFrom(assessment, allocator);
      copied["episode_id"].SetString(id.GetString(), id.GetStringLength(), allocator);
      add_citation(copied["evidence_event_ids"]);
      response["episode_assessments"].PushBack(copied, allocator);
    }
    legacy::RawModelResponse result;
    result.transport_ok = true;
    result.adapter = name();
    result.model_id = model_id();
    result.body = serialize(response);
    result.attempts = 1;
    return result;
  }

  // 测试辅助函数：name。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  std::string name() const override {
    return "standard-trace-fixture";
  }

  // 测试辅助函数：model_id。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  std::string model_id() const override {
    return "offline-capturing-model";
  }
};

// 回归用例：test_blackbox_and_roundtrip。
// 关键断言：expect(te::validate_trace(trace).report.trace_valid(), "blackbox must be valid");；此处只验证测试构造的场景。
void test_blackbox_and_roundtrip() {
  auto trace = blackbox();
  expect(te::validate_trace(trace).report.trace_valid(), "blackbox must be valid");
  const auto output = te::evaluate_trace(trace);
  expect(output.report.assessments_size() == 1, "blackbox needs one assessable episode");
  const auto &packet = output.report.assessments(0).evidence();
  expect(packet.scope() == "agent:invoice-agent/actor:assistant", "neutral scope is not isolated");
  expect(packet.skill_name().empty(), "must not invent a skill");
  expect(output.report.metrics().workflow_status() == ep::AGGREGATE_STATUS_SUCCEEDED,
         "completed execution must remain completed");
  expect(output.report.metrics().business_status() == ep::AGGREGATE_STATUS_FAILED,
         "business failure must be separate");
  expect(te::evaluate_trace(te::parse_trace_json(te::to_json(trace))).report.source_fingerprint() ==
             output.report.source_fingerprint(),
         "JSON roundtrip changed source identity");
  v1::TraceRun binary;
  expect(binary.ParseFromString(trace.SerializeAsString()), "Protobuf roundtrip failed");
  expect(te::evaluate_trace(binary).report.source_fingerprint() ==
             output.report.source_fingerprint(),
         "Protobuf roundtrip changed source identity");
  event(trace, "end").mutable_outcome()->set_execution_status("cancelled");
  event(trace, "end").mutable_outcome()->set_business_status("unknown");
  const auto cancelled = te::evaluate_trace(trace);
  expect(cancelled.report.metrics().workflow_status() == ep::AGGREGATE_STATUS_UNKNOWN,
         "legacy wire format must not turn cancellation into success");
}

// 回归用例：test_validation_and_order。
// 重点核验字段或诊断：conflicting_duplicate_event、pair_context_mismatch、missing_step_reference；异常路径不能被当成成功。
void test_validation_and_order() {
  const auto baseline = repeated();
  const auto reference = te::evaluate_trace(baseline);
  auto reordered = baseline;
  std::reverse(reordered.mutable_events()->begin(), reordered.mutable_events()->end());
  expect(te::evaluate_trace(reordered).report.SerializeAsString() ==
             reference.report.SerializeAsString(),
         "physical event order must not change evaluation");
  auto duplicate = baseline;
  *duplicate.add_events() = baseline.events(0);
  const auto validated = te::validate_trace(duplicate);
  expect(validated.report.trace_valid() && validated.report.duplicate_event_count() == 1,
         "exact event replay must be deduplicated");
  duplicate.mutable_events(duplicate.events_size() - 1)
      ->mutable_input()
      ->set_string_value("changed");
  expect(has_issue(te::validate_trace(duplicate), "conflicting_duplicate_event"),
         "conflicting duplicate must be rejected");
  auto broken = baseline;
  event(broken, "e2").set_step_id("missing");
  expect(!te::validate_trace(broken).report.trace_valid(), "unmatched pair must be rejected");
  broken = baseline;
  event(broken, "a2").set_execution_id("other-execution");
  expect(has_issue(te::validate_trace(broken), "pair_context_mismatch"),
         "cross execution pair accepted");
  broken = baseline;
  event(broken, "a2").clear_step_id();
  expect(has_issue(te::validate_trace(broken), "missing_step_reference"),
         "unscoped action accepted");
  broken = baseline;
  event(broken, "a2").add_caused_by("missing");
  expect(has_issue(te::validate_trace(broken), "missing_causal_event"),
         "dangling causality accepted");
  broken = baseline;
  event(broken, "begin").add_caused_by("end");
  expect(has_issue(te::validate_trace(broken), "causal_cycle_or_invalid_order"),
         "causal cycle accepted");
  broken = baseline;
  event(broken, "a2").set_sequence(event(broken, "b2").sequence());
  expect(has_issue(te::validate_trace(broken), "duplicate_producer_sequence"),
         "sequence collision accepted");
  broken = baseline;
  event(broken, "end").clear_outcome();
  expect(!te::validate_trace(broken).report.trace_valid(), "implicit task status accepted");
  te::TraceLimits limits;
  limits.max_events = 1;
  expect_error([&] { te::validate_trace(baseline, limits); }, "event limit not enforced");
  limits.max_bytes = 5;
  expect_error([&] { te::parse_trace_json(te::to_json(baseline), limits); },
               "byte limit not enforced");
  expect_error([&] { te::parse_trace_json(R"({"schema_version":1,"secret_unknown":"test"})"); },
               "unknown top-level fields accepted");
  auto changed = baseline;
  (*changed.mutable_attributes()->mutable_fields())["custom"].set_string_value("新业务上下文");
  expect(te::evaluate_trace(changed).report.source_fingerprint() !=
             reference.report.source_fingerprint(),
         "run custom fields omitted from evidence fingerprint");
  changed = baseline;
  changed.mutable_identity()->set_agent_id("another-agent");
  expect(te::evaluate_trace(changed).report.assessments(0).evidence().scope() !=
             reference.report.assessments(0).evidence().scope(),
         "different agents share scope");
}

// 回归用例：test_parallel_and_repeat_rules。
// 关键断言：expect(repeat.report.metrics().invalid_episode_count() == 1,；此处只验证测试构造的场景。
void test_parallel_and_repeat_rules() {
  auto trace = repeated();
  const auto repeat = te::evaluate_trace(trace);
  expect(repeat.report.metrics().invalid_episode_count() == 1,
         "explicit non-retryable repeat should be diagnosed");
  // 执行者与参数相同仍不够，执行实例也必须匹配。
  for (auto &item : *trace.mutable_events()) {
    if (item.iteration() == 2) {
      item.set_execution_id("parallel-exec");
      item.set_producer_id("parallel-producer");
      item.set_sequence(item.sequence() - 5);
    }
  }
  event(trace, "end").mutable_outcome()->set_business_status("succeeded");
  const auto parallel = te::evaluate_trace(trace);
  expect(parallel.report.source_trace_integrity_valid(), "valid parallel branches rejected");
  expect(parallel.report.metrics().invalid_episode_count() == 0,
         "parallel branch mistaken for retry");
  expect(parallel.report.metrics().business_status() == ep::AGGREGATE_STATUS_SUCCEEDED,
         "branch failure overwrote explicit run success");
  expect(parallel.report.assessments_size() == 2, "parallel step lost");
  for (const auto &assessment : parallel.report.assessments()) {
    const auto &packet = assessment.evidence();
    expect(packet.tools_size() == 1, "action was attached to wrong step");
    expect(packet.tools(0).call_event_id().back() == packet.episode_id().back(),
           "parallel result attribution was reordered");
  }
  trace = repeated();
  event(trace, "a2").mutable_input()->set_string_value("different order");
  expect(te::evaluate_trace(trace).report.metrics().invalid_episode_count() == 0,
         "changed input mistaken for retry");
  trace = repeated();
  event(trace, "b1").mutable_error()->set_retryability("unknown");
  expect(te::evaluate_trace(trace).report.metrics().invalid_episode_count() == 0,
         "unknown retryability overclaimed");
  trace = repeated();
  for (auto &item : *trace.mutable_events()) {
    if (item.iteration() == 2)
      item.set_producer_id("different-producer");
  }
  expect(te::evaluate_trace(trace).report.metrics().invalid_episode_count() == 0,
         "arbitrary topological order mistaken for causal retry");
  event(trace, "a2").add_caused_by("b1");
  expect(te::evaluate_trace(trace).report.metrics().invalid_episode_count() == 1,
         "explicit cross-producer cause lost");
}

// 回归用例：test_capabilities。
// 关键断言：expect(report.effective_level() == v1::L0 && report.verified_level() == v1::L0,；此处只验证测试构造的场景。
void test_capabilities() {
  v1::CapabilityManifest manifest;
  manifest.set_schema_version(1);
  manifest.set_requested_level(v1::L4);
  manifest.set_fallback("downgrade");
  for (const char *capability : {"run_observation", "experience_injection", "action_control",
                                 "loop_control", "controlled_experiment"}) {
    manifest.add_declared_capabilities(capability);
  }
  const auto validated = te::validate_trace(repeated());
  auto report = te::inspect_capabilities(validated, manifest);
  expect(report.effective_level() == v1::L0 && report.verified_level() == v1::L0,
         "declarations must not grant online capabilities");
  expect(!report.requested_level_satisfied(), "L4 falsely certified");
  for (const auto &check : report.checks()) {
    if (check.required_level() > v1::L0) {
      expect(check.runtime_status() == v1::NOT_VERIFIED, "unimplemented probe passed");
    }
  }
  manifest.set_fallback("strict");
  expect(te::inspect_capabilities(validated, manifest).effective_level() == v1::LEVEL_NONE,
         "strict unsupported level silently downgraded");
  manifest.set_requested_level(v1::L0);
  expect(te::inspect_capabilities(validated, manifest).requested_level_satisfied(), "L0 rejected");
  manifest.clear_declared_capabilities();
  manifest.add_not_applicable("loop_control");
  report = te::inspect_capabilities(validated, manifest);
  expect(report.checks(3).data_status() == v1::NOT_APPLICABLE, "N/A was not preserved");
  expect(report.verified_level() == v1::L0, "N/A elevated verified level");
  manifest.add_not_applicable("run_observation");
  expect_error([&] { te::inspect_capabilities(validated, manifest); },
               "required L0 accepted as N/A");
}

// 回归用例：test_model_actions_are_not_tools。
// 关键断言：expect(report.metrics().tool_call_count() == 0 && report.metrics().failed_tool_call_count() == 0,；此处只验证测试构造的场景。
void test_model_actions_are_not_tools() {
  auto trace = repeated();
  for (const auto *id : {"a1", "a2"})
    (*event(trace, id).mutable_attributes()->mutable_fields())["semantic"].set_string_value(
        "model");
  auto report = te::evaluate_trace(trace).report;
  expect(report.metrics().tool_call_count() == 0 && report.metrics().failed_tool_call_count() == 0,
         "model activities polluted tool metrics");
  expect(report.assessments_size() == 2 && report.metrics().outcome_failed_episode_count() == 2,
         "model failure evidence disappeared");
  expect(report.metrics().invalid_episode_count() == 1, "model retry diagnostic disappeared");
  for (const auto &assessment : report.assessments())
    expect(assessment.evidence().tools_size() == 0 &&
               assessment.evidence().source_event_ids_size() >= 4,
           "model must retain evidence without claiming a tool call");
  (*event(trace, "a2").mutable_attributes()->mutable_fields())["semantic"].set_string_value(
      "action");
  report = te::evaluate_trace(trace).report;
  expect(report.metrics().tool_call_count() == 1, "mixed model/tool metrics are incorrect");
  expect(report.metrics().invalid_episode_count() == 0, "model/tool name collision became a retry");
  (*event(trace, "b2").mutable_attributes()->mutable_fields())["executed"].set_bool_value(false);
  expect(te::evaluate_trace(trace).report.metrics().tool_call_count() == 0,
         "unexecuted action counted as a tool call");
  (*event(trace, "a2").mutable_attributes()->mutable_fields())["semantic"].set_string_value(
      "custom");
  expect(te::evaluate_trace(trace).report.metrics().tool_call_count() == 0,
         "custom activity counted as a business tool");
  trace = repeated();
  (*event(trace, "b1").mutable_attributes()->mutable_fields())["executed"].set_bool_value(false);
  expect(te::evaluate_trace(trace).report.metrics().invalid_episode_count() == 0,
         "an unexecuted action became a previous failed attempt");
}

// 回归用例：test_real_content_and_sanitization。
// 关键断言：expect(output.report.model_succeeded_count() == 1, "neutral content model contract failed");；此处只验证测试构造的场景。
void test_real_content_and_sanitization() {
  const std::string secret = "fake-vendor-secret-1234567890";
  auto trace = blackbox();
  event(trace, "begin").mutable_input()->set_string_value("查询中文发票，凭据 " + secret);
  (*trace.mutable_attributes()->mutable_fields())["business_context"].set_string_value(
      "需要增值税发票");
  auto *training =
      (*trace.mutable_attributes()->mutable_fields())["training_evaluation"].mutable_struct_value();
  (*training->mutable_fields())["expected_behavior"].set_string_value(
      "需要真实解析失败，写入读回不能代替解析");
  (*training->mutable_fields())["verdict"].set_string_value("failed");
  (*event(trace, "begin").mutable_attributes()->mutable_fields())["api_key"].set_string_value(
      secret);
  auto options = model_options();
  options.redaction_secrets.push_back(secret);
  auto adapter = std::make_shared<CapturingModel>();
  const auto output = te::evaluate_trace(trace, options, adapter);
  expect(output.report.model_succeeded_count() == 1, "neutral content model contract failed");
  expect(adapter->requests.size() == 1, "one run must call model once");
  const auto &request = adapter->requests.front();
  expect(request.system_prompt.find("框架中立") != std::string::npos, "neutral prompt not loaded");
  expect(request.user_prompt.find("查询中文发票") != std::string::npos,
         "model sees only fingerprints");
  expect(request.user_prompt.find("增值税发票") != std::string::npos, "run custom content omitted");
  expect(request.user_prompt.find("training_evaluation") != std::string::npos &&
             request.user_prompt.find("需要真实解析失败，写入读回不能代替解析") !=
                 std::string::npos,
         "training acceptance omitted from analysis request");
  expect(request.user_prompt.find(secret) == std::string::npos, "request contains secret");
  expect(te::Evaluator::to_json(output.report).find(secret) == std::string::npos,
         "persisted report contains secret");
  adapter->counterfeit = true;
  const auto rejected = te::evaluate_trace(trace, options, adapter);
  expect(rejected.report.model_succeeded_count() == 0, "counterfeit evidence accepted");
  const auto previous_calls = adapter->requests.size();
  event(trace, "begin").add_caused_by("missing");
  options.allow_invalid_trace = true;
  const auto invalid = te::evaluate_trace(trace, options, adapter);
  expect(!invalid.report.source_trace_integrity_valid(), "invalid trace lost integrity flag");
  expect(adapter->requests.size() == previous_calls, "invalid evidence sent to model");
}

// 回归用例：test_pipeline_and_memory。
// 关键断言：expect(batch.report.evaluation_succeeded_count() == 2, "standard batch failed");；此处只验证测试构造的场景。
void test_pipeline_and_memory() {
  te::Pipeline pipeline;
  const auto batch = pipeline.evaluate_files({source("examples/standard-agent/one.trace.json"),
                                              source("examples/standard-agent/two.trace.json")});
  expect(batch.report.evaluation_succeeded_count() == 2, "standard batch failed");
  expect(batch.report.evaluation_failed_count() == 0, "standard file routed to legacy reader");
  expect(batch.report.candidates_size() >= 1, "deterministic candidates not generated");
  ep::EvolutionMemoryStore memory;
  te::Pipeline::merge_memory(batch.report, &memory);
  expect(memory.entries_size() >= 1, "supported candidate was not stored");
  const auto once = memory.SerializeAsString();
  te::Pipeline::merge_memory(batch.report, &memory);
  expect(memory.SerializeAsString() == once, "same batch was stored twice");
  expect(memory.entries(0).candidate().target_scope() == "agent:invoice-agent/actor:assistant",
         "memory scope lost framework-neutral identity");
}

// 回归用例：test_neutral_structured_experience。
// 关键断言：expect(evaluated.report.model_succeeded_count() == 1 &&；此处只验证测试构造的场景。
void test_neutral_structured_experience() {
  auto model = std::make_shared<CapturingModel>();
  model->include_experience = true;
  const auto evaluated = te::evaluate_trace(blackbox(), model_options(), model);
  expect(model->requests.front().user_prompt.find("content_events 顶层 event_id") != std::string::npos &&
             model->requests.front().user_prompt.find("承载该 history 的本轮上下文事件") != std::string::npos,
         "model contract does not distinguish historical references from current citations");
  expect(evaluated.report.model_succeeded_count() == 1 &&
             evaluated.report.reflection().experience_details_size() == 1,
         "neutral model experience was not validated");
  const auto &detail = evaluated.report.reflection().experience_details(0);
  te::EvolutionOptions options;
  options.min_supporting_dialogues = 1;
  options.min_supporting_episodes = 1;
  te::Pipeline pipeline({}, options);
  const auto batch = pipeline.synthesize_reports({evaluated.report});
  expect(batch.report.candidates_size() == 1, "neutral experience did not become a candidate");
  const auto &candidate = batch.report.candidates(0);
  expect(legacy::is_scoped_experience(candidate) &&
             candidate.target_scope() == detail.target_scope() &&
             candidate.trigger_pattern() == detail.trigger_pattern() &&
             candidate.support_evidence(0).evidence_event_ids_size() == 1,
         "neutral candidate lost its condition, actor or individual evidence");
}

// 回归用例：test_binary_file_and_config。
// 关键断言：expect(std::filesystem::create_directory(directory), "test directory already exists");；此处只验证测试构造的场景。
void test_binary_file_and_config() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory =
      std::filesystem::current_path() / ("standard-trace-test-" + std::to_string(nonce));
  expect(std::filesystem::create_directory(directory), "test directory already exists");

  struct Cleanup {
    std::filesystem::path path;

    // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};

  const auto path = directory / "run.trace.pb";
  const auto trace = repeated();
  {
    std::ofstream output(path, std::ios::binary);
    expect(trace.SerializeToOstream(&output), "cannot write Protobuf fixture");
  }
  expect(te::Evaluator().evaluate_file(path).report.source_trace_integrity_valid(),
         "PB file entry failed");
  expect(te::Pipeline().evaluate_directory(directory).report.evaluation_succeeded_count() == 1,
         "directory did not discover binary traces");
  const auto old = te::load_runtime_configuration(source("config/runtime.json"));
  const auto neutral = te::load_runtime_configuration(source("config/standard-runtime.json"));
  expect(old.source_files.size() == 9, "legacy eight-prompt config changed");
  expect(neutral.source_files.size() == 10, "optional neutral prompt not tracked");
}

// 回归用例：test_capability_evidence_is_not_runtime。
// 关键断言：expect(report.data_level() == v1::L4, "structural capability evidence was not observed");；此处只验证测试构造的场景。
void test_capability_evidence_is_not_runtime() {
  auto trace = repeated();
  auto *receipt = trace.add_events();
  receipt->set_event_id("receipt");
  receipt->set_kind("advice.applied");
  receipt->set_producer_id("observer");
  receipt->set_sequence(1);
  receipt->set_actor_id("assistant");
  auto *application = receipt->mutable_application();
  application->set_query_id("query-1");
  application->set_policy_id("policy-1");
  application->set_policy_revision("revision-1");
  application->set_decision_id("decision-1");
  application->set_status("applied");
  application->set_experiment_id("experiment-1");
  application->set_cohort("treatment");
  auto *evaluation = trace.add_events();
  evaluation->set_event_id("evaluation");
  evaluation->set_kind("evaluation");
  evaluation->set_producer_id("observer");
  evaluation->set_sequence(2);
  evaluation->set_actor_id("judge");
  evaluation->mutable_output()->set_string_value("没有取得发票");
  v1::CapabilityManifest manifest;
  manifest.set_schema_version(1);
  manifest.set_requested_level(v1::L4);
  manifest.set_fallback("downgrade");
  const auto report = te::inspect_capabilities(te::validate_trace(trace), manifest);
  expect(report.data_level() == v1::L4, "structural capability evidence was not observed");
  expect(report.verified_level() == v1::L0 && !report.requested_level_satisfied(),
         "complete sample evidence falsely certified online execution");
}

// 回归用例：test_standard_prompt_required。
// 关键断言：expect(adapter->requests.empty(), "model was called before neutral prompt validation");；此处只验证测试构造的场景。
void test_standard_prompt_required() {
  auto options = te::load_runtime_configuration(source("config/runtime.json")).evaluator;
  options.model_selection = te::ModelSelection::all;
  auto adapter = std::make_shared<CapturingModel>();
  expect_error([&] { te::evaluate_trace(blackbox(), options, adapter); },
               "standard trace silently used the AgentFlow-specific model prompt");
  expect(adapter->requests.empty(), "model was called before neutral prompt validation");
}
} // 结束当前匿名命名空间。

// 回归用例：main。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
int main() {
  try {
    test_blackbox_and_roundtrip();
    test_validation_and_order();
    test_parallel_and_repeat_rules();
    test_model_actions_are_not_tools();
    test_capabilities();
    test_real_content_and_sanitization();
    test_pipeline_and_memory();
    test_neutral_structured_experience();
    test_binary_file_and_config();
    test_capability_evidence_is_not_runtime();
    test_standard_prompt_required();
    std::cout << "Standard trajectory compatibility tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
