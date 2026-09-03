#include "agentflow/trajectory/hybrid_evaluator.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

namespace trajectory = agentflow::trajectory;
namespace ep = agentflow::trajectory::evaluation;

void expect(bool condition, const std::string& message) {
  if(!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path write_temporary(
    const std::string& name,
    const std::string& content) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path()
      / ("agentflow-hybrid-" + name + '-' + std::to_string(nonce));
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if(!output) {
    throw std::runtime_error("cannot create temporary test file");
  }
  output << content;
  return path;
}

std::string analysis_json(bool trace_valid = true) {
  return std::string(R"JSON({
    "schema_version": 1,
    "analysis_kind": "loop_episode_analysis",
    "analyzer_version": "loop-episode-analyzer-v1.1",
    "rule_set_version": "invalid-loop-rules-v1",
    "source": {
      "path": "/must/not/appear/ark-secret-value-1234567890.jsonl",
      "dialogue_id": "d1",
      "conversation_id": "conv1",
      "user_id": "user1",
      "fingerprint_algorithm": "sha256-v1"
    },
    "trace_integrity": {"valid": )JSON")
      + (trace_valid ? "true" : "false")
      + R"JSON(, "issues": []},
    "summary": {"episode_count": 3},
    "episodes": [
      {
        "episode_id": "episode-coordinator-1",
        "scope": "coordinator",
        "owner_id": "coordinator:d1",
        "skill_name": "",
        "activation_index": 1,
        "round": 1,
        "attempt_index": 1,
        "step_index": 1,
        "action": "continue",
        "reason": "tool_calls_requested",
        "continued": true,
        "terminal": false,
        "terminal_status": "",
        "outcome_failed": false,
        "all_tool_results_failed": false,
        "has_non_retryable_failure": false,
        "input_fingerprint": "input-a",
        "action_fingerprint": "action-a",
        "outcome_fingerprint": "outcome-a",
        "outcome_class_fingerprint": "class-a",
        "dispatch": {
          "batch_id": "batch-1",
          "skill_names": ["image", "video"],
          "prompt_fingerprints": ["prompt-a", "prompt-b"],
          "force": [false, false],
          "batch_status": "partial",
          "workflow_status": "completed",
          "successful_count": 1,
          "total_count": 2
        },
        "tool_actions": [],
        "source_event_ids": ["event-c1"],
        "diagnostic_ids": []
      },
      {
        "episode_id": "episode-skill-1",
        "scope": "skill",
        "owner_id": "dispatch:d1:1",
        "skill_name": "video",
        "activation_index": 0,
        "round": 1,
        "attempt_index": 1,
        "step_index": 1,
        "action": "continue",
        "reason": "tool_calls_requested",
        "continued": true,
        "terminal": false,
        "terminal_status": "",
        "outcome_failed": true,
        "all_tool_results_failed": true,
        "has_non_retryable_failure": true,
        "input_fingerprint": "input-b",
        "action_fingerprint": "action-b",
        "outcome_fingerprint": "outcome-b",
        "outcome_class_fingerprint": "class-b",
        "dispatch": {},
        "tool_actions": [{
          "tool_call_id": "call-1",
          "tool_name": "generate_video",
          "status": "error",
          "error_class": "unsupported_model",
          "retryability": "non_retryable",
          "arguments_fingerprint": "args-b",
          "result_fingerprint": "result-b",
          "call_event_id": "event-t1",
          "result_event_id": "event-t2"
        }],
        "evaluations": [{
          "event_id": "event-e1",
          "evaluator": "task-grader",
          "evaluator_version": "v1",
          "metric": "task_success",
          "score": 0.9,
          "passed": true
        }],
        "source_event_ids": ["event-s1", "event-t1", "event-t2", "event-e1"],
        "diagnostic_ids": ["diagnostic-1"]
      },
      {
        "episode_id": "episode-skill-2",
        "scope": "skill",
        "owner_id": "dispatch:d1:1",
        "skill_name": "video",
        "activation_index": 0,
        "round": 2,
        "attempt_index": 1,
        "step_index": 2,
        "action": "stop",
        "reason": "final_response",
        "continued": false,
        "terminal": true,
        "terminal_status": "completed",
        "outcome_failed": false,
        "all_tool_results_failed": false,
        "has_non_retryable_failure": false,
        "input_fingerprint": "input-c",
        "action_fingerprint": "action-c",
        "outcome_fingerprint": "outcome-c",
        "outcome_class_fingerprint": "class-c",
        "dispatch": {},
        "tool_actions": [],
        "evaluations": [
          {
            "event_id": "event-e1",
            "evaluator": "task-grader",
            "evaluator_version": "v1",
            "metric": "task_success",
            "score": 0.9,
            "passed": true
          },
          {
            "event_id": "event-e2",
            "evaluator": "human-review",
            "evaluator_version": "v1",
            "metric": "qualitative_review",
            "score": null,
            "passed": null
          }
        ],
        "source_event_ids": ["event-s2", "event-e1", "event-e2"],
        "diagnostic_ids": []
      }
    ],
    "diagnostics": [{
      "diagnostic_id": "diagnostic-1",
      "episode_id": "episode-skill-1",
      "category": "non_retryable_repeat",
      "counts_as_invalid": true,
      "confidence": 0.92,
      "severity": "error",
      "summary": "同一不可重试错误类别被重复观察到。",
      "observable_cause": "失败类别仍为 unsupported_model。",
      "tool_name": "generate_video",
      "error_class": "unsupported_model",
      "evidence_event_ids": ["event-t1", "event-t2"]
    }],
    "raw_secret": "sk-test-should-never-be-copied-1234567890"
  })JSON";
}

class FixedModelAdapter final : public trajectory::ModelAdapter {
 public:
  enum class Mode {
    valid_useful,
    valid_necessary_retry,
    unknown_evidence,
    credential,
    credential_escaped,
    transport_error,
    throw_exception,
  };

  explicit FixedModelAdapter(Mode mode) : mode_(mode) {}

  trajectory::RawModelResponse evaluate(
      const trajectory::ModelRequest& request) override {
    ++calls;
    if(mode_ == Mode::throw_exception) {
      throw std::runtime_error("sensitive adapter detail must not escape");
    }
    trajectory::RawModelResponse response;
    response.adapter = name();
    response.model_id = model_id();
    response.attempts = 1;
    if(mode_ == Mode::transport_error) {
      response.error_code = "timeout";
      return response;
    }
    response.transport_ok = true;
    const std::string event_id = mode_ == Mode::unknown_evidence
        ? "fabricated-event"
        : request.allowed_event_ids.front();
    const std::string verdict = mode_ == Mode::valid_necessary_retry
        ? "necessary_retry"
        : "useful";
    const std::string reason = mode_ == Mode::credential
        ? "该步骤有效，key=ark-secret-value-12345678901234567890"
        : mode_ == Mode::credential_escaped
            ? "该步骤有效，key=\\u0061rk-secret-value-12345678901234567890"
            : "该步骤产生了可观察的必要状态变化。";
    response.body =
        "{\"verdict\":\"" + verdict
        + "\",\"confidence\":0.81,"
          "\"expected_state_change\":\"形成新的可验证状态\","
          "\"observed_state_change\":\"证据显示状态已变化\","
          "\"concise_reason\":\"" + reason
        + "\",\"evidence_event_ids\":[\"" + event_id + "\"]}";
    return response;
  }

  std::string name() const override {
    return "fixed-test";
  }

  std::string model_id() const override {
    return "fixed-test-model";
  }

  int calls = 0;

 private:
  Mode mode_;
};

trajectory::EvaluationOutput evaluate(
    const std::string& name,
    trajectory::EvaluatorOptions options,
    std::shared_ptr<trajectory::ModelAdapter> adapter = nullptr,
    bool trace_valid = true) {
  const auto path = write_temporary(name + ".json", analysis_json(trace_valid));
  try {
    trajectory::HybridTrajectoryEvaluator evaluator(options, std::move(adapter));
    auto output = evaluator.evaluate_file(path);
    std::filesystem::remove(path);
    return output;
  } catch(...) {
    std::filesystem::remove(path);
    throw;
  }
}

void test_deterministic_metrics_and_privacy() {
  trajectory::EvaluatorOptions options;
  const auto output = evaluate("deterministic", options);
  const auto& report = output.report;
  expect(report.assessments_size() == 3, "all episodes must be preserved");
  expect(report.metrics().episode_count() == 3, "episode metric mismatch");
  expect(report.metrics().invalid_episode_count() == 1, "invalid metric mismatch");
  expect(report.metrics().tool_call_count() == 1, "tool metric mismatch");
  expect(report.metrics().failed_tool_call_count() == 1, "failed tool mismatch");
  expect(report.metrics().evaluation_count() == 2, "evaluation metric mismatch");
  expect(
      report.metrics().passed_evaluation_count() == 1,
      "passed evaluation metric mismatch");
  expect(
      report.metrics().failed_evaluation_count() == 0,
      "null pass value must not count as failed");
  expect(
      report.metrics().workflow_status() == ep::AGGREGATE_STATUS_SUCCEEDED,
      "workflow completion must remain separate from business failure");
  expect(
      report.metrics().business_status() == ep::AGGREGATE_STATUS_PARTIAL,
      "mixed branch results should produce partial business status");
  expect(report.model_requested_count() == 0, "offline must be the default");
  expect(
      report.assessments(1).final_verdict() == ep::SEMANTIC_VERDICT_INVALID,
      "hard invalid rule must become final verdict");
  expect(
      report.assessments(2).evidence().has_previous_same_owner(),
      "same-owner predecessor must be represented");
  expect(
      report.assessments(2).evidence().evaluations_size() == 2,
      "evaluation evidence must remain attached to its episode");
  expect(
      report.assessments(1).evidence().evaluations_size() == 1,
      "shared evaluation must remain attached to every target episode");
  expect(report.metrics().evaluation_count() == 2, "shared event must count once");

  const std::string json = trajectory::HybridTrajectoryEvaluator::to_json(
      report, false);
  expect(
      json.find("should-never-be-copied") == std::string::npos,
      "unknown raw fields must not be copied");
  expect(
      json.find("/must/not/appear") == std::string::npos,
      "source filesystem path must not be copied");

  std::string binary;
  expect(report.SerializeToString(&binary), "Protobuf serialization failed");
  ep::HybridEvaluationReport round_trip;
  expect(round_trip.ParseFromString(binary), "Protobuf round trip failed");
  expect(
      round_trip.source().dialogue_id() == "d1",
      "Protobuf round trip changed identity");
}

void test_failed_evaluation_enters_semantic_review() {
  std::string input = analysis_json();
  const std::string passing = "\"passed\": true";
  const std::string failing = "\"passed\": false";
  std::size_t position = 0;
  std::size_t replacement_count = 0;
  while((position = input.find(passing, position)) != std::string::npos) {
    input.replace(position, passing.size(), failing);
    position += failing.size();
    ++replacement_count;
  }
  expect(replacement_count == 2, "shared passing evaluation copies were not found");
  const auto path = write_temporary("failed-evaluation.json", input);
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::suspicious;
  trajectory::EvaluationOutput output;
  try {
    trajectory::HybridTrajectoryEvaluator evaluator(options);
    output = evaluator.evaluate_file(path);
    std::filesystem::remove(path);
  } catch(...) {
    std::filesystem::remove(path);
    throw;
  }

  expect(
      output.report.metrics().failed_evaluation_count() == 1,
      "failed evaluation metric mismatch");
  const auto& assessment = output.report.assessments(2);
  expect(
      assessment.rule().verdict() == ep::SEMANTIC_VERDICT_UNCERTAIN,
      "failed evaluation must enter semantic review");
  expect(
      assessment.rule().confidence() == 0.9,
      "failed evaluation review confidence mismatch");
  expect(
      assessment.rule().evidence_event_ids_size() == 1
          && assessment.rule().evidence_event_ids(0) == "event-e1",
      "failed evaluation event must be retained as rule evidence");
  expect(
      output.model_requests.size() == 3
          && assessment.model().error_code() == "adapter_not_configured",
      "failed evaluation must be selected for pluggable model review");
}

void test_conflicting_shared_evaluation_is_rejected() {
  std::string input = analysis_json();
  const std::string passing = "\"passed\": true";
  const std::size_t position = input.find(passing);
  expect(position != std::string::npos, "shared evaluation was not found");
  input.replace(position, passing.size(), "\"passed\": false");
  const auto path = write_temporary("conflicting-evaluation.json", input);
  bool threw = false;
  try {
    trajectory::HybridTrajectoryEvaluator evaluator;
    evaluator.evaluate_file(path);
  } catch(const trajectory::EvaluationError&) {
    threw = true;
  }
  std::filesystem::remove(path);
  expect(
      threw,
      "conflicting payloads with one Evaluation event_id must be rejected");
}

void test_hard_rule_cannot_be_overridden() {
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  auto adapter = std::make_shared<FixedModelAdapter>(
      FixedModelAdapter::Mode::valid_useful);
  const auto output = evaluate("conflict", options, adapter);
  const auto& invalid = output.report.assessments(1);
  expect(adapter->calls == 3, "all selection must invoke all episodes");
  expect(
      invalid.model().status() == ep::MODEL_CALL_STATUS_SUCCEEDED,
      "valid model response was not accepted");
  expect(
      invalid.final_verdict() == ep::SEMANTIC_VERDICT_INVALID,
      "model must not override deterministic invalid rule");
  expect(invalid.rule_model_conflict(), "rule/model conflict must be visible");
  expect(
      invalid.decision_source() == ep::DECISION_SOURCE_RULE,
      "conflicting hard rule must remain the decision source");
}

void test_model_can_resolve_uncertain_business_failure() {
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::suspicious;
  auto adapter = std::make_shared<FixedModelAdapter>(
      FixedModelAdapter::Mode::valid_necessary_retry);
  const auto output = evaluate("resolve", options, adapter);
  expect(adapter->calls == 2, "suspicious selection should evaluate two episodes");
  const auto& coordinator = output.report.assessments(0);
  expect(
      coordinator.final_verdict() == ep::SEMANTIC_VERDICT_NECESSARY_RETRY,
      "validated model verdict should resolve suspicious episode");
  expect(
      coordinator.decision_source() == ep::DECISION_SOURCE_MODEL,
      "unassessed rule plus model should use model source");
}

void test_invalid_model_evidence_degrades_without_corrupting_rules() {
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  auto adapter = std::make_shared<FixedModelAdapter>(
      FixedModelAdapter::Mode::unknown_evidence);
  const auto output = evaluate("unknown-evidence", options, adapter);
  expect(output.report.model_failed_count() == 3, "all invalid responses must count");
  expect(output.report.model_failures_degraded(), "non-strict failure must degrade");
  expect(
      output.report.assessments(1).final_verdict()
          == ep::SEMANTIC_VERDICT_INVALID,
      "model validation failure must preserve rule verdict");
  expect(
      output.report.assessments(1).model().error_code()
          == "unknown_evidence_event_id",
      "specific validation error must be recorded");
}

void test_credential_like_model_output_is_rejected() {
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  auto adapter = std::make_shared<FixedModelAdapter>(
      FixedModelAdapter::Mode::credential);
  const auto output = evaluate("credential", options, adapter);
  const std::string json = trajectory::HybridTrajectoryEvaluator::to_json(
      output.report, false);
  expect(
      output.report.assessments(0).model().error_code()
          == "credential_like_output",
      "credential-like output must be rejected");
  expect(
      json.find("ark-secret-value") == std::string::npos,
      "rejected model body must never be persisted");

  auto escaped_adapter = std::make_shared<FixedModelAdapter>(
      FixedModelAdapter::Mode::credential_escaped);
  const auto escaped_output = evaluate(
      "credential-escaped", options, escaped_adapter);
  expect(
      escaped_output.report.assessments(0).model().error_code()
          == "credential_like_output",
      "decoded credential-like output must be rejected");
  const std::string escaped_json =
      trajectory::HybridTrajectoryEvaluator::to_json(
          escaped_output.report, false);
  expect(
      escaped_json.find("ark-secret-value") == std::string::npos,
      "decoded credential-like model text must never be persisted");
}

void test_strict_model_failure_throws() {
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  options.strict_model = true;
  auto adapter = std::make_shared<FixedModelAdapter>(
      FixedModelAdapter::Mode::transport_error);
  bool threw = false;
  try {
    evaluate("strict", options, adapter);
  } catch(const trajectory::EvaluationError&) {
    threw = true;
  }
  expect(threw, "strict model mode must fail on transport error");
}

void test_adapter_exception_degrades_without_leaking_detail() {
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  auto adapter = std::make_shared<FixedModelAdapter>(
      FixedModelAdapter::Mode::throw_exception);
  const auto output = evaluate("adapter-exception", options, adapter);
  expect(
      output.report.model_failed_count() == 3,
      "adapter exceptions must become per-episode model failures");
  expect(
      output.report.assessments(0).model().error_code() == "adapter_exception",
      "adapter exception must use a stable generic error code");
  const std::string json = trajectory::HybridTrajectoryEvaluator::to_json(
      output.report, false);
  expect(
      json.find("sensitive adapter detail") == std::string::npos,
      "adapter exception detail must not be persisted");
}

void test_credential_like_structural_evidence_is_rejected() {
  std::string input = analysis_json();
  const std::string original = "同一不可重试错误类别被重复观察到。";
  const std::size_t position = input.find(original);
  expect(position != std::string::npos, "test diagnostic was not found");
  input.replace(
      position,
      original.size(),
      "api_key=ark-secret-structural-evidence-1234567890");
  const auto path = write_temporary("structural-secret.json", input);
  bool threw = false;
  try {
    trajectory::HybridTrajectoryEvaluator evaluator;
    evaluator.evaluate_file(path);
  } catch(const trajectory::EvaluationError&) {
    threw = true;
  }
  std::filesystem::remove(path);
  expect(threw, "credential-like structural evidence must be rejected");
}

void test_http_adapter_rejects_unsafe_configuration_without_network() {
  trajectory::OpenAiCompatibleOptions options;
  options.model_id = "test-model";
  bool external_http_rejected = false;
  options.endpoint = "http://example.com/v1/chat/completions";
  try {
    trajectory::make_openai_compatible_model_adapter(options);
  } catch(const trajectory::EvaluationError&) {
    external_http_rejected = true;
  }
  expect(external_http_rejected, "non-loopback HTTP must be rejected");

  bool query_rejected = false;
  options.endpoint = "http://127.0.0.1:18080/v1/chat/completions?key=secret";
  try {
    trajectory::make_openai_compatible_model_adapter(options);
  } catch(const trajectory::EvaluationError&) {
    query_rejected = true;
  }
  expect(query_rejected, "endpoint query must be rejected");

  bool resource_limit_rejected = false;
  options.endpoint = "http://127.0.0.1:18080/v1/chat/completions";
  options.max_retries = 11;
  try {
    trajectory::make_openai_compatible_model_adapter(options);
  } catch(const trajectory::EvaluationError&) {
    resource_limit_rejected = true;
  }
  expect(resource_limit_rejected, "excessive retry count must be rejected");

  options.max_retries = 0;
  bool thinking_mode_rejected = false;
  options.thinking_type = "sometimes";
  try {
    trajectory::make_openai_compatible_model_adapter(options);
  } catch(const trajectory::EvaluationError&) {
    thinking_mode_rejected = true;
  }
  expect(thinking_mode_rejected, "unknown thinking mode must be rejected");

  options.thinking_type = "disabled";
  options.max_request_bytes = 8;
  const auto adapter = trajectory::make_openai_compatible_model_adapter(options);
  trajectory::ModelRequest request;
  request.packet_id = "oversized";
  request.user_prompt = "larger than eight bytes";
  const auto response = adapter->evaluate(request);
  expect(
      !response.transport_ok && response.error_code == "request_too_large",
      "oversized adapter request must be rejected before network access");
}

void test_model_request_limit_bounds_export_and_execution() {
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  options.max_model_calls = 1;
  const auto output = evaluate("request-limit", options);
  expect(
      output.model_requests.size() == 1
          && output.report.model_requested_count() == 1,
      "model request export must honor the configured call limit");
  expect(
      output.report.assessments(1).model().status()
              == ep::MODEL_CALL_STATUS_SKIPPED
          && output.report.assessments(1).model().error_code()
              == "model_call_limit_reached",
      "episodes beyond the model call limit must be explicitly skipped");
}

void test_invalid_trace_is_quarantined() {
  trajectory::EvaluatorOptions options;
  options.model_selection = trajectory::ModelSelection::all;
  options.allow_invalid_trace = true;
  auto adapter = std::make_shared<FixedModelAdapter>(
      FixedModelAdapter::Mode::valid_useful);
  const auto output = evaluate("quarantine", options, adapter, false);
  expect(adapter->calls == 0, "invalid source trace must never reach the model");
  expect(
      output.model_requests.empty()
          && output.report.model_requested_count() == 0,
      "invalid source trace must not produce exportable model requests");
  expect(
      output.report.assessments(0).model().status()
          == ep::MODEL_CALL_STATUS_SKIPPED,
      "quarantined model call must be marked skipped");
  expect(
      output.report.assessments(1).final_verdict()
          == ep::SEMANTIC_VERDICT_INVALID,
      "quarantine must retain deterministic evidence");
}

void test_jsonl_adapter_replays_by_packet_id() {
  const auto path = write_temporary(
      "responses.jsonl",
      "{\"packet_id\":\"p1\",\"judgment\":{\"verdict\":\"uncertain\"}}\n");
  try {
    const auto adapter = trajectory::make_jsonl_model_adapter(path);
    trajectory::ModelRequest request;
    request.packet_id = "p1";
    const auto response = adapter->evaluate(request);
    expect(response.transport_ok, "known JSONL packet must replay");
    expect(
        response.body.find("uncertain") != std::string::npos,
        "JSONL judgment was not returned");
    request.packet_id = "missing";
    const auto missing = adapter->evaluate(request);
    expect(
        !missing.transport_ok && missing.error_code == "missing_record",
        "missing JSONL packet must be explicit");
    std::filesystem::remove(path);
  } catch(...) {
    std::filesystem::remove(path);
    throw;
  }
}

}  // namespace

int main() {
  try {
    test_deterministic_metrics_and_privacy();
    test_failed_evaluation_enters_semantic_review();
    test_conflicting_shared_evaluation_is_rejected();
    test_hard_rule_cannot_be_overridden();
    test_model_can_resolve_uncertain_business_failure();
    test_invalid_model_evidence_degrades_without_corrupting_rules();
    test_credential_like_model_output_is_rejected();
    test_strict_model_failure_throws();
    test_adapter_exception_degrades_without_leaking_detail();
    test_credential_like_structural_evidence_is_rejected();
    test_http_adapter_rejects_unsafe_configuration_without_network();
    test_model_request_limit_bounds_export_and_execution();
    test_invalid_trace_is_quarantined();
    test_jsonl_adapter_replays_by_packet_id();
    std::cout << "trajectory hybrid evaluator tests passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
}
