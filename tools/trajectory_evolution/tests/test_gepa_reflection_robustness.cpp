// Native reflection regression: real saved requests, zero external model/Agent calls.
#define run_task_evaluation run_task_evaluation_robustness_boundary
#include "../src/gepa/gepa_search.cpp"
#undef run_task_evaluation
#include <iostream>
#include "agentflow/trajectory/experience.h"

namespace trajectory::evolution {
dataset::v1::TaskEvaluationReport run_task_evaluation_robustness_boundary(
    const std::filesystem::path &, const std::filesystem::path &,
    const std::filesystem::path &, const TaskEvaluationOptions &) {
  throw std::runtime_error("unexpected Agent evaluation in reflection-only regression");
}
}
using namespace trajectory::evolution::integration;

void expect_robust(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}

namespace trajectory::evolution {
bool initial_review_retry_context_matches(const integration::Json &, const integration::Json &,
                                          const std::string &, const std::string &);
}

void initial_review_retry_context_regressions() {
  using trajectory::evolution::initial_review_retry_context_matches;
  for (const auto &code : {"gepa_baseline_evaluation_inconclusive", "gepa_seed_evaluation_inconclusive"}) {
    Json tree{{"status", "error"}, {"error", code}, {"evaluation_context", "grader"},
              {"candidate_evaluation_context", "execution"}};
    auto job = tree;
    expect_robust(initial_review_retry_context_matches(tree, job, "grader", "execution"),
                  "same initial review contexts were rejected");
    for (const auto &field : {"evaluation_context", "candidate_evaluation_context"}) {
      auto changed = job;
      changed[field] = "foreign";
      expect_robust(!initial_review_retry_context_matches(tree, changed, "grader", "execution"),
                    "initial review accepted a mismatched stored context");
    }
    expect_robust(!initial_review_retry_context_matches(tree, job, "new-grader", "execution") &&
                    !initial_review_retry_context_matches(tree, job, "grader", "new-execution") &&
                    !initial_review_retry_context_matches(tree, job, "", "execution"),
                  "initial review migrated the real grader or Agent execution");
  }
  std::cout << "initial_review_context_guard=PASS cases=10\n";
}

namespace trajectory::evolution {
bool gepa_final_holdout_review_pending(const integration::Json &);
bool gepa_final_holdout_review_retryable(const GepaResult &);
}

void gepa_final_holdout_review_regressions() {
  using namespace trajectory::evolution;
  GepaResult result;
  result.summary = Json{{"status", "inconclusive"}, {"final_holdout", Json{{"status", "inconclusive"},
      {"candidates", Json::Array{Json{{"status", "complete"}}, Json{{"status", "inconclusive"}}}}}}};
  result.artifact = Json{{"selected", "formal-passing-candidate"}};
  result.finalists.resize(2);
  for (auto &finalist : result.finalists) {
    finalist.holdout.set_complete(true);
    auto *trial = finalist.holdout.add_trials();
    trial->set_execution_status("completed"); trial->set_verdict("passed");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{
        {"output", Json{{"framework_capture", Json{{"status", "complete"}}}}},
        {"trace", Json{{"events", Json::Array{Json{{"kind", "run.start"}}, Json{{"kind", "run.end"}}}}}}});
  }
  auto *trial = result.finalists[1].holdout.mutable_trials(0);
  trial->set_verdict("error"); trial->set_error_code("evaluation_judge_failed");
  auto *check = trial->add_checks(); check->set_check_id("model"); check->set_status("error");
  unsigned cases = 0;
  for (const auto &reason : {"evaluation_judge_http_408", "evaluation_judge_http_429",
      "evaluation_judge_http_500", "evaluation_judge_http_502", "evaluation_judge_http_503",
      "evaluation_judge_http_504", "evaluation_judge_transport_18", "evaluation_judge_transport_28",
      "evaluation_judge_reason_conflict"}) {
    check->set_reason(reason);
    const auto original = result.finalists[1].holdout.SerializeAsString();
    expect_robust(gepa_final_holdout_review_pending(result.summary) &&
                  gepa_final_holdout_review_retryable(result), "closed temporary partial Top-K was not retryable");
    expect_robust(result.finalists[1].holdout.SerializeAsString() == original &&
                  result.artifact.str("selected") == "formal-passing-candidate", "terminal check changed a formal result");
    ++cases;
  }
  for (const auto &reason : {"evaluation_judge_http_401", "evaluation_judge_http_403",
      "evaluation_judge_schema_invalid", "evaluation_judge_uncertain", "invalid_json"}) {
    check->set_reason(reason);
    expect_robust(!gepa_final_holdout_review_retryable(result), "permanent/unknown review failure was auto-retryable");
    ++cases;
  }
  auto mixed = result;
  auto *first = mixed.finalists[0].holdout.mutable_trials(0);
  first->set_error_code("evaluation_judge_failed"); first->set_verdict("error");
  auto *first_check = first->add_checks(); first_check->set_check_id("model");
  first_check->set_status("error"); first_check->set_reason("evaluation_judge_http_503");
  mixed.finalists[1].holdout.mutable_trials(0)->mutable_checks(0)->set_reason("evaluation_judge_http_401");
  expect_robust(!gepa_final_holdout_review_retryable(mixed), "temporary peer hid permanent finalist failure"); ++cases;
  check->set_reason("evaluation_judge_http_503");
  auto changed = result; changed.finalists[1].holdout.mutable_trials(0)->set_execution_status("error");
  expect_robust(!gepa_final_holdout_review_retryable(changed), "failed execution was treated as judge-only"); ++cases;
  changed = result; changed.finalists[1].holdout.set_complete(false);
  expect_robust(!gepa_final_holdout_review_retryable(changed), "incomplete report granted retry"); ++cases;
  changed = result; changed.finalists[1].holdout.mutable_trials(0)->clear_evidence();
  expect_robust(!gepa_final_holdout_review_retryable(changed), "missing closed capture granted retry"); ++cases;
  changed = result;
  auto *events = &(*changed.finalists[1].holdout.mutable_trials(0)->mutable_evidence()->mutable_fields())["trace"];
  (*events->mutable_struct_value()->mutable_fields())["events"].mutable_list_value()->mutable_values()->RemoveLast();
  expect_robust(!gepa_final_holdout_review_retryable(changed), "unclosed trace granted retry"); ++cases;
  changed = result; changed.finalists[1].holdout.mutable_trials(0)->clear_error_code();
  changed.finalists[1].holdout.mutable_trials(0)->set_verdict("uncertain");
  expect_robust(!gepa_final_holdout_review_retryable(changed), "real uncertainty granted retry"); ++cases;
  changed = result; changed.finalists[1].holdout.mutable_trials(0)->clear_checks();
  expect_robust(!gepa_final_holdout_review_retryable(changed), "missing fixed model failure granted retry"); ++cases;
  changed = result; changed.finalists.pop_back();
  expect_robust(!gepa_final_holdout_review_retryable(changed), "different frozen finalist count granted retry"); ++cases;
  changed = result; changed.summary["status"] = "completed";
  changed.summary["final_holdout"]["status"] = "completed";
  expect_robust(!gepa_final_holdout_review_pending(changed.summary), "complete campaign remained pending"); ++cases;
  changed = result; changed.summary["final_holdout"]["candidates"] = Json::Array{};
  expect_robust(!gepa_final_holdout_review_pending(changed.summary), "empty/no-candidate plan became pending"); ++cases;
  std::cout << "partial_holdout_terminal_review=PASS cases=" << cases << "\n";
}

void global_selection_regressions() {
  // 实际 Node13 的完整 VAL 失败集合：局部两题净 +1，全局 11/12 -> 10/12。
  // 桩保持实际 Q/strict 总数与 loop 总数，避免依赖已清理的实验目录。
  const std::vector<std::string> tasks{
      "0d8a4ee_1", "0d8a4ee_2", "0d8a4ee_3", "530b157_1", "530b157_2", "530b157_3",
      "68ee2c9_1", "68ee2c9_2", "68ee2c9_3", "6c2c621_1", "6c2c621_2", "6c2c621_3"};
  Json parent = Json::Object{}, specialist = Json::Object{};
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    const auto pq = tasks[i] == "530b157_3" ? 0 : 1;
    const auto cq = tasks[i] == "530b157_2" || tasks[i] == "6c2c621_3" ? 0 : 1;
    parent[tasks[i]] = Json{{"quality", pq}, {"strict_pass_rate", pq},
        {"fitness", pq - (i < 8 ? .1 : 0)}, {"invalid_loops", i < 8 ? 1 : 0},
        {"loop_method", "model_review"}, {"critical_failures", 0}};
    specialist[tasks[i]] = Json{{"quality", cq}, {"strict_pass_rate", cq},
        {"fitness", cq}, {"invalid_loops", 0}, {"loop_method", "model_review"},
        {"critical_failures", 0}};
  }
  const Json changes{{"0", Json{{"530b157_1", Json{{"loop", 1}}},
                                 {"530b157_3", Json{{"loop", 1}}}}}};
  const Json coverage{{"complete", true}, {"level_complete", true}};
  Search search;
  search.config = Json{{"holdout_top_k", 3}, {"min_improvement", .000001}};
  search.tree = Json{{"nodes", Json::Array{Json{{"id", 0}, {"status", "accepted"},
      {"version", "seed"}, {"validation", parent}, {"control_coverage", coverage},
      {"optimization", Json{{"feasible", true}, {"passed", false},
                             {"score", .6557377049}, {"robustness", 11.0 / 12}}}}}}};
  for (std::size_t id = 1; id < 13; ++id)
    search.tree["nodes"].push(Json{{"id", id}, {"status", "duplicate"}});
  search.tree["nodes"].push(Json{{"id", 13}, {"parent", 0}, {"kind", "reflection"},
      {"status", "accepted"}, {"version", "specialist"}, {"validation", specialist},
      {"validation_gain_attributed", true}, {"control_coverage", coverage},
      {"screening", Json{{"split", "train"}, {"controls_observed", true}}},
      {"validation_applied_changes", changes},
      {"optimization", Json{{"feasible", true}, {"passed", true},
                             {"score", 1}, {"robustness", 11.0 / 12}}}});
  expect_robust(search.attributed_validation_gain(13), "real Node13 local gain disappeared");
  expect_robust(gepa::exploration_eligibility(search.tree["nodes"], 13, search.config)
                    .flag("eligible", false), "real Node13 lost its exploration eligibility");
  expect_robust(search.winner() == 0 && search.winner(1) == 0,
      "goal-score saturation replaced the globally stronger seed");
  expect_robust(search.final_holdout_candidates() == std::vector<std::size_t>({0, 13}),
      "finalists lost the protected seed or the legitimate exploration branch");
  search.config["holdout_top_k"] = 1;
  expect_robust(search.final_holdout_candidates() == std::vector<std::size_t>{0},
      "top-one finalist ignored the global elite");
  search.final_holdout_top_k_override = 2;
  expect_robust(search.final_holdout_candidates() == std::vector<std::size_t>({0, 13}),
      "explicit finalist budget did not preserve exploration");

  auto stronger = search.tree["nodes"].items()[13];
  stronger["id"] = 14; stronger["version"] = "stronger";
  stronger["validation"] = parent;
  for (auto &task : stronger["validation"].fields()) {
    task.second["quality"] = 1; task.second["strict_pass_rate"] = 1;
    task.second["fitness"] = 1; task.second["invalid_loops"] = 0;
  }
  stronger["optimization"]["passed"] = false;
  stronger["optimization"]["score"] = .1;
  search.tree["nodes"].push(stronger);
  search.final_holdout_top_k_override = 3;
  expect_robust(search.winner() == 14 && search.final_holdout_candidates() ==
      std::vector<std::size_t>({14, 0, 13}), "proxy score still overrode a global improvement");
  // 相同 executable version 的去重必须先保留已选 elite 的完整验证身份。
  search.tree["nodes"].items()[14]["version"] = "seed";
  expect_robust(search.final_holdout_candidates() == std::vector<std::size_t>({14, 13}),
      "version deduplication displaced the qualified elite");
  search.tree["nodes"].items()[14] = stronger;
  search.gate["require_level_coverage"] = true;
  search.tree["nodes"].items()[14]["control_coverage"]["level_complete"] = false;
  expect_robust(search.winner() == 0 && search.final_holdout_candidates() ==
      std::vector<std::size_t>({0, 13}), "strict coverage qualification was bypassed");
  search.gate = Json::Object{};
  auto different_pool = stronger;
  different_pool["validation"].fields().erase("0d8a4ee_1");
  different_pool["validation"]["different-training-task"] = stronger["validation"]["0d8a4ee_1"];
  search.tree["nodes"].items()[14] = different_pool;
  expect_robust(search.winner() == 0 && search.final_holdout_candidates() ==
      std::vector<std::size_t>({0, 13}), "different validation cohorts were ranked together");
  search.tree["nodes"].items()[14] = stronger;
  search.tree["nodes"].items()[14]["validation"]["0d8a4ee_1"].fields().erase("strict_pass_rate");
  expect_robust(search.winner() == 0 && search.final_holdout_candidates() ==
      std::vector<std::size_t>({0, 13}), "incomplete official strict evidence entered finalists");
  search.tree["nodes"].items()[14] = stronger;
  search.tree["nodes"].items()[14]["validation_applied_changes"] = Json::Object{};
  expect_robust(search.winner() == 0 && search.final_holdout_candidates() ==
      std::vector<std::size_t>({0, 13}), "unattributed global drift entered elite selection");
  search.config["final_holdout_include_seed"] = false;
  expect_robust(search.winner() == 0 && search.final_holdout_candidates().empty() &&
      gepa::exploration_eligibility(search.tree["nodes"], 13, search.config).flag("eligible", false),
      "no new globally safe candidate repeated seed holdout or lost its exploration branch");
  search.tree["nodes"].items()[14] = stronger;
  search.tree["nodes"].items()[14]["version"] = "seed";
  expect_robust(search.final_holdout_candidates().empty(),
      "initial seed's executable version was treated as a new final candidate");
  search.tree["nodes"].items()[14] = stronger;
  expect_robust(search.final_holdout_candidates() == std::vector<std::size_t>{14},
      "new globally stronger candidate lost its automatic holdout slot");
  auto &safe_loop = search.tree["nodes"].items()[14];
  safe_loop["validation"] = parent;
  for (auto &task : safe_loop["validation"].fields()) task.second["invalid_loops"] = 0;
  expect_robust(search.final_holdout_candidates() == std::vector<std::size_t>{14},
      "globally neutral quality/strict with real loop gain was overrestricted");
  const Json frozen_plan{{"status", "prepared"}, {"candidates", Json::Array{
      Json{{"node", 0}, {"version", "seed"}, {"rank", 1}}}}};
  search.tree["final_holdout"] = frozen_plan;
  search.final_holdout_replay = true;
  expect_robust(search.final_holdout_candidates() == std::vector<std::size_t>({14, 0, 13}) &&
      search.tree["final_holdout"].dump() == frozen_plan.dump(),
      "explicit replay changed compatibility or rewrote its frozen source plan");
  const auto config_dir = fs::temp_directory_path() /
      ("gepa-final-holdout-config-" + std::to_string(now()));
  fs::create_directories(config_dir);
  const auto config_path = config_dir / "config.json";
  Json settings{{"schema_version", 1}, {"runtime_config", "runtime.json"},
                {"prompt_file", "reflection.txt"}};
  write(config_path, settings.dump());
  expect_robust(!load_gepa_config(config_path).has("final_holdout_include_seed"),
      "legacy config was rewritten with a new default field");
  settings["final_holdout_include_seed"] = false;
  write(config_path, settings.dump());
  expect_robust(!load_gepa_config(config_path).flag("final_holdout_include_seed", true),
      "automatic holdout seed exclusion was rejected by native config loading");
  settings["final_holdout_include_seed"] = "false";
  write(config_path, settings.dump());
  bool invalid_boolean = false;
  try { (void)load_gepa_config(config_path); }
  catch (const std::exception &) { invalid_boolean = true; }
  fs::remove_all(config_dir);
  expect_robust(invalid_boolean, "string final_holdout_include_seed bypassed boolean validation");
  std::cout << "native Search Node13/local+1/global-1/elite/exploration/cohort/coverage regressions passed\n";
  std::cout << "automatic_holdout seed_excluded=1 equivalent_version_excluded=1 core_guard=1"
               " new_safe_candidate=1 loop_only_safe=1 explicit_replay_preserved=1\n";
}

Json official_fixture() {
  Json packet{{"selected_components", Json::Array{"experience:approved"}},
              {"components", Json{{"experience:approved", "Approved original body."}}},
              {"experiences", Json::Array{Json{{"memory_id", "approved"},
                    {"scope", "assistant"}, {"content", "Approved original body."},
                    {"trigger_pattern", "observed_machine_symbol"}}}},
              {"training_batch_summary", Json::Array{}}, {"feedback", Json::Array{}}};
  for (std::size_t i = 0; i < 32; ++i) {
    const auto id = std::string("train-") + std::to_string(i);
    Json metrics{{"quality", i % 2 ? 0 : 1}, {"strict_pass_rate", i % 2 ? 0 : 1},
                 {"strict_passed", i % 2 == 0}, {"invalid_loops", i == 7 ? 2 : 0},
                 {"invalid_loop_count", i == 7 ? 2 : 0}};
    Json summary = metrics;
    summary["task_id"] = id;
    packet["training_batch_summary"].push(summary);
    packet["feedback"].push(Json{{"task_id", id}, {"trial", Json{
        {"verdict", i % 2 ? "failed" : "passed"}, {"execution_status", "completed"},
        {"metrics", metrics}}}});
  }
  return packet;
}

void transient_transport_regressions() {
  struct TransportModel final : legacy::ModelAdapter {
    std::string code, first_prompt, first_packet;
    bool persistent = false, cancel_after_first = false;
    bool *cancelled = nullptr;
    std::size_t calls = 0;
    std::string name() const override { return "local-transport-fault"; }
    std::string model_id() const override { return "local"; }
    legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
      ++calls;
      const auto packet = Json::parse(request.user_prompt);
      expect_robust(reflection_packet_metrics_complete(packet) &&
          packet["training_batch_summary"].items().size() == 32,
          "transport retry dropped formal TRAIN facts");
      if (calls == 1) { first_prompt = request.user_prompt; first_packet = request.packet_id; }
      else expect_robust(request.user_prompt == first_prompt && request.packet_id == first_packet,
          "transport retry changed its stage, cohort or request");
      legacy::RawModelResponse response;
      if (calls == 1 || persistent) response.error_code = code;
      else {
        response.transport_ok = true;
        response.body = Json{{"reason", "Retain the approved parent"},
                             {"experiences", Json::Array{}}}.dump();
      }
      if (cancel_after_first) *cancelled = true;
      return response;
    }
  };
  for (const auto &code : {"http_500", "http_502", "http_503", "http_504", "transport_28",
                           "http_401", "http_429"}) {
    for (const unsigned mode : {0u, 1u, 2u, 3u}) {
      char directory[] = "/tmp/gepa-transport-regression-XXXXXX";
      expect_robust(::mkdtemp(directory), "cannot create transport fixture");
      Search search; search.root = directory;
      search.config = Json{{"max_input_bytes", 65536}, {"max_model_calls", mode == 2 ? 1 : 8},
                           {"timeout_ms", 600000}};
      const auto started = now();
      const Json cached{{"healthy", Json{{"complete", true}, {"trial_id", "retained-trial"}}}};
      search.tree = Json{{"started_ms", started}, {"trials_used", 32}, {"evaluations", cached},
                         {"nodes", Json::Array{Json{{"id", 0}, {"status", "pending"}}}}};
      search.config["prompt_file"] = std::string(directory) + "/prompt.txt";
      write(search.config.str("prompt_file"), "Use all official facts and strict JSON.");
      const auto healthy = fs::path(directory) / "healthy-report.json";
      const auto completed = fs::path(directory) / "reflections/completed-map.draft.json";
      write(healthy, cached.dump());
      write(completed, "{\"reason\":\"retained map\",\"experiences\":[]}");
      const auto healthy_bytes = read(healthy), completed_bytes = read(completed);
      bool cancel = false; search.cancelled = [&] { return cancel; };
      auto model = std::make_shared<TransportModel>();
      model->code = code; model->persistent = mode == 1; model->cancel_after_first = mode == 3;
      model->cancelled = &cancel; search.model = model;
      auto packet = official_fixture(); packet["phase"] = "pool_diagnostic_map";
      packet["pool_task_count"] = 32;
      std::string error;
      try { (void)search.reflect(packet, 0, "-map"); }
      catch (const Error &failure) { error = failure.what(); }
      const bool transient = std::string(code) != "http_401" && std::string(code) != "http_429";
      const auto expected_calls = transient && mode < 2 ? 2u : 1u;
      expect_robust(model->calls == expected_calls &&
          search.tree.num("model_calls", 0) == expected_calls &&
          search.tree["reflection_calls"].fields().size() == expected_calls,
          "transport retry was unbounded, uncounted or ignored its model budget");
      const auto expected_error = mode == 3 ? "gepa_cancelled" :
          std::string(code) == "http_429" ? "gepa_model_rate_limited" :
          !transient || mode > 0 ? "gepa_reflection_transport_failed" : "";
      expect_robust(error == expected_error, "transport failure classification changed");
      if (std::string(code) == "http_429" && mode != 3)
        expect_robust(search.tree["nodes"].items()[0].str("status") == "pending",
            "429 lost its resumable candidate");
      expect_robust(search.tree.num("started_ms", 0) == started &&
          search.tree.num("trials_used", 0) == 32 && search.tree["evaluations"].dump() == cached.dump() &&
          read(healthy) == healthy_bytes && read(completed) == completed_bytes,
          "transport retry reset the total budget or rewrote a healthy checkpoint");
      fs::remove_all(directory);
    }
  }
  std::cout << "transient_transport same_stage=1 bounded_retry=1 budget_counted=1"
               " cancel_and_permanent_no_retry=1 cached_grade_map_unchanged=1\n";
}

void synthetic_regressions() {
  auto original = official_fixture();
  expect_robust(reflection_packet_metrics_complete(original), "full official ledger rejected");
  for (const auto *field : {"quality", "strict_pass_rate", "invalid_loops"}) {
    auto missing = original;
    missing["feedback"].items()[2]["trial"]["metrics"].fields().erase(field);
    expect_robust(!reflection_packet_metrics_complete(missing), "missing official metric accepted");
  }
  auto long_row = original["feedback"].items()[0];
  const std::string long_id(900, 'x');
  long_row["task_id"] = long_id;
  const auto facts = reflection_task_facts(Json::Array{long_row}, original["selected_components"]);
  expect_robust(facts.items()[0].str("task_id") == long_id &&
      facts.items()[0]["quality"].dump() == long_row["trial"]["metrics"]["quality"].dump(),
      "dynamic fact budget truncated task identity or official quality");
  auto oversized = original;
  oversized["prior_search_rejections"] = Json::Array{Json{{"reason", std::string(70000, 'r')}}};
  const auto compact = compact_reflection_metadata(oversized, 65536);
  expect_robust(reflection_input_bytes(compact) <= 65536 &&
      compact["training_batch_summary"].dump() == original["training_batch_summary"].dump() &&
      reflection_packet_metrics_complete(compact), "budget adaptation lost official facts");
  for (const auto *code : {"control_argument_not_authorized", "control_operation_not_authorized",
                          "control_source_not_approved", "control_duplicate_loop_scope", "unknown_field"})
    expect_robust(reflection_candidate_error(code), "schema/source rejection kills entire diagnosis");
  for (const auto *code : {"gepa_cancelled", "gepa_reflection_transport_failed",
                          "gepa_feedback_budget_too_small", "gepa_trial_budget_exhausted"})
    expect_robust(!reflection_candidate_error(code), "infrastructure failure hidden as candidate rejection");
  std::cout << "synthetic official-metric/long-ID/budget/error-isolation regressions passed\n";
}

void effective_counter_regressions() {
  Json query{{"boundary", "loop"}, {"iteration", 4}, {"state_fingerprint", "current"},
             {"failed_actions", 3},
             {"repeated_action_steps", 0}, {"repeated_failure_steps", 3},
             {"previous", Json{{"iteration", 3}, {"fingerprint", "previous"},
                                {"decision", "replan"}, {"no_progress", 0}}}};
  Json progress{{"iteration", 4}, {"fingerprint", "current"}, {"no_progress", 0}};
  expect_robust(effective_loop_no_progress(query, progress, false).integer() == 0,
      "three action failures became nonaggressive no-progress");
  expect_robust(effective_loop_no_progress(query, progress, true).integer() == 3,
      "aggressive failure counter disappeared");
  for (const int failures : {0, 1}) {
    query["repeated_failure_steps"] = failures;
    expect_robust(effective_loop_no_progress(query, progress, false).integer() == 0 &&
        effective_loop_no_progress(query, progress, true).integer() == failures,
        "three failed actions replaced the recorded zero/one failure counter");
  }
  query["repeated_failure_steps"] = 3;
  query["previous"]["fingerprint"] = "current";
  expect_robust(effective_loop_no_progress(query, progress, false).integer() == 1,
      "one repeated state became three failure steps");
  query["repeated_action_steps"] = 3;
  expect_robust(effective_loop_no_progress(query, progress, false).integer() == 3,
      "actual repeated actions lost their native counter");
  query["previous"]["iteration"] = 4;
  progress["no_progress"] = 2;
  expect_robust(effective_loop_no_progress(query, progress, false).integer() == 2,
      "same-iteration replay incremented the counter");
  query["previous"]["iteration"] = 3;
  query["previous"]["decision"] = "stop";
  expect_robust(effective_loop_no_progress(query, progress, false).integer() == 2,
      "stopped progress was recomputed");
  query["pending_actions"] = true;
  expect_robust(effective_loop_no_progress(query, progress, true).null(),
      "pending actions received an invented counter");
  query["pending_actions"] = false;
  progress["fingerprint"] = "different";
  expect_robust(effective_loop_no_progress(query, progress, false).null(),
      "mismatched progress received an invented counter");
  std::cout << "native aggressive/failure/action/idempotent counter regressions passed\n";
}

void exact_feedback_cache_regressions() {
  Trial trial;
  trial.set_trial_id("same-trial"); trial.set_task_id("train-a");
  trial.set_variant_id("candidate"); trial.set_split("train"); trial.set_repetition(1);
  trial.set_verdict("passed"); trial.set_execution_status("completed");
  *trial.mutable_metrics() = proto<google::protobuf::Struct>(Json{{"quality", 1}, {"tokens", 10}});
  *trial.mutable_evidence() = proto<google::protobuf::Struct>(Json{
      {"loaded_version", "agent-version-v1:original"}, {"output", "original answer"},
      {"assessment", Json{{"quality", 1}, {"goals", Json::Array{}}}},
      {"loop_review", Json{{"status", "complete"}, {"observed_iterations", 0},
                            {"iteration_reviews", Json::Array{}}}}});
  const Trial copied = trial;
  expect_robust(exact_trial_cache_key(trial) == exact_trial_cache_key(copied),
      "protobuf copying changed the complete deterministic cache identity");
  Search search;
  search.controls = Json{{"enabled", true}};
  const auto *original = &search.reviewed_feedback_trial(trial);
  expect_robust(&search.reviewed_feedback_trial(copied) == original &&
      search.reviewed_feedback_cache.size() == 1, "copied trial missed its exact cache entry");
  const auto check_changed = [&](const Trial &changed, const std::string &label) {
    expect_robust(changed.task_id() == trial.task_id() && changed.trial_id() == trial.trial_id() &&
        exact_trial_cache_key(changed) != exact_trial_cache_key(trial), label + " identity collapsed");
    const auto count = search.reviewed_feedback_cache.size();
    auto expected = feedback_trial(changed, search.controls);
    bind_iteration_assessments(expected, json(changed.evidence()));
    expect_robust(search.reviewed_feedback_trial(changed).dump() == expected.dump() &&
        search.reviewed_feedback_cache.size() == count + 1, label + " reused an old view");
  };
  auto version = trial;
  (*version.mutable_evidence()->mutable_fields())["loaded_version"].set_string_value(
      "agent-version-v1:different");
  check_changed(version, "different version");
  auto evidence = trial;
  (*evidence.mutable_evidence()->mutable_fields())["output"].set_string_value("different evidence");
  check_changed(evidence, "different evidence");
  auto judge = trial;
  (*judge.mutable_metrics()->mutable_fields())["quality"].set_number_value(0.25);
  (*(*judge.mutable_evidence()->mutable_fields())["assessment"].mutable_struct_value()
        ->mutable_fields())["quality"].set_number_value(0.25);
  check_changed(judge, "different judge");
  const Json selected = Json::Array{"loop_rules"};
  auto controls = search.controls;
  controls["aggressive_controls"] = false;
  const auto *control = &search.cached_control_feedback_trial(trial, selected, controls);
  expect_robust(&search.cached_control_feedback_trial(copied, selected, controls) == control &&
      search.control_feedback_cache.size() == 1, "copied control view missed its exact cache entry");
  controls["aggressive_controls"] = true;
  search.cached_control_feedback_trial(trial, selected, controls);
  search.cached_control_feedback_trial(trial, Json::Array{"action_rules"}, controls);
  expect_robust(search.control_feedback_cache.size() == 3,
      "aggressive or selected-component context reused a different control view");
  std::cout << "complete deterministic cache/copy/version/evidence/judge/context regressions passed\n";
}

void selector_directory_regressions() {
  auto packet = official_fixture();
  packet["phase"] = "pool_diagnostic_reduce";
  packet["selected_components"] = Json::Array{"experience:approved", "action_rules", "loop_rules"};
  const Json facts = Json::Array{Json{{"task_id", "train-7"}, {"diagnosis", "observed failure"}}};
  packet["intermediate_proposals"] = Json::Array{
      Json{{"id", "remove"}, {"valid", true}, {"task_facts", facts},
           {"candidate_values", Json{{"experience:approved", nullptr}, {"action_rules", Json::Array{}}}}},
      Json{{"id", "empty"}, {"valid", true}, {"task_facts", facts}, {"reason", "Abstain"},
           {"candidate_values", Json::Object{}}},
      Json{{"id", "invalid"}, {"valid", false}, {"task_facts", facts}, {"error", "source_not_approved"},
           {"candidate_values", Json{{"action_rules", Json::Array{}}}}},
      Json{{"id", "unselected"}, {"valid", true}, {"task_facts", facts},
           {"candidate_values", Json{{"schedule_rules", Json::Array{}}}}}};
  const auto original = packet.dump();
  const Json &source_packet = packet;
  const auto clarified = selection_packet(packet);
  expect_robust(clarified["intermediate_proposals"].items().size() == 1 &&
      clarified["intermediate_proposals"].items()[0].dump() == packet["intermediate_proposals"].items()[0].dump() &&
      clarified["legal_selection_choices"]["experience:approved"].dump() == Json(Json::Array{"remove"}).dump() &&
      clarified["legal_selection_choices"]["action_rules"].dump() == Json(Json::Array{"remove"}).dump() &&
      clarified["legal_selection_choices"]["loop_rules"].dump() == Json(Json::Array{}).dump(),
      "invalid/empty/unselected proposal became legal, or null/empty removal disappeared");
  expect_robust(clarified["diagnostic_proposals"].items().size() == 3 &&
      clarified["training_batch_summary"].dump() == packet["training_batch_summary"].dump() &&
      clarified["feedback"].dump() == packet["feedback"].dump(), "selector lost formal or diagnostic rows");
  for (std::size_t i = 1; i < 4; ++i)
    for (const auto *field : {"id", "valid", "task_facts", "reason", "error"})
      expect_robust(clarified["diagnostic_proposals"].items()[i - 1][field].dump() ==
          source_packet["intermediate_proposals"].items()[i][field].dump(), "selector changed diagnostic evidence");
  expect_robust(!clarified.has("selected_proposals") && selection_packet(clarified).dump() == clarified.dump() &&
      packet.dump() == original, "legal directory auto-selected its sole option or mutated its source");
  std::cout << "selector_directory invalid_empty_excluded=1 null_array_removal=1 diagnostics_exact=1 sole_not_default=1\n";
}

void reduction_budget_regressions() {
  auto packet = official_fixture();
  packet.fields().erase("feedback");
  packet["phase"] = "pool_diagnostic_reduce";
  packet["experiences"] = Json::Array{};
  packet["controls"] = Json{{"source", Json{{"memory_id", "source-1"}}}};
  for (std::size_t i = 0; i < 99; ++i)
    packet["experiences"].push(Json{{"memory_id", i ? "source-" + std::to_string(i) : "approved"},
        {"scope", i % 2 ? "coordinator" : "global"}, {"content", std::string(700, 'b')},
        {"original_content", "Authorized original body"}, {"observed_problem", "Actual source problem"},
        {"condition", std::string(300, 'c')}, {"trigger_pattern", std::string(300, 't')}});
  for (auto &row : packet["training_batch_summary"].items())
    row["metric_coverage"] = std::string(300, 'm');
  Json compact;
  bool reproduced = false;
  for (std::size_t width = 100; width <= 700 && !reproduced; width += 25) {
    packet["intermediate_proposals"] = Json::Array{};
    for (std::size_t segment = 0; segment < 2; ++segment) {
      Json facts = Json::Array{}, ids = Json::Array{};
      for (const auto &row : packet["training_batch_summary"].items()) {
        auto fact = row;
        fact["segment"] = segment;
        fact["diagnosis"] = std::string(width, segment ? 's' : 'f');
        facts.push(fact); ids.push(row["task_id"]);
      }
      packet["intermediate_proposals"].push(Json{{"id", "part-" + std::to_string(segment)},
          {"task_ids", ids}, {"task_facts", facts}, {"candidate_values", Json{
              {"experience:approved", Json{{"content", "Approved revised body"},
                  {"source", Json{{"memory_id", "source-2"}}}}}}}});
    }
    compact = compact_full_pool_reduction(packet);
    auto stale = compact;
    stale["intermediate_proposals"] = packet["intermediate_proposals"];
    reproduced = reflection_input_bytes(compact) <= 65536 && reflection_input_bytes(stale) > 65536;
  }
  expect_robust(reproduced && reflection_input_bytes(packet) > 65536,
      "synthetic old two-record budget stall was not reproduced");
  expect_robust(compact["experiences"].items().size() == 99 &&
      compact["training_batch_summary"].dump() == packet["training_batch_summary"].dump(),
      "reduce changed the source catalogue or official 32-task ledger");
  for (std::size_t i = 0; i < 99; ++i) {
    const auto &before = packet["experiences"].items()[i];
    const auto &after = compact["experiences"].items()[i];
    expect_robust(before["memory_id"].dump() == after["memory_id"].dump() &&
        before["scope"].dump() == after["scope"].dump(), "reduce changed an exact source ID/scope");
    if (i < 3) expect_robust(before.dump() == after.dump(), "reduce stripped associated source text");
  }
  auto chunk = compact;
  chunk["intermediate_proposals"] = Json::Array{};
  const auto &compact_records = compact["intermediate_proposals"];
  std::set<std::string> distinct;
  for (const auto &record : compact_records.items()) {
    chunk["intermediate_proposals"].push(record);
    expect_robust(reflection_input_bytes(chunk) <= 65536, "compacted records cannot share a reduce chunk");
    for (std::size_t i = 0; i < 32; ++i) {
      auto expected = packet["intermediate_proposals"].items()[record.str("id") == "part-0" ? 0 : 1]
          ["task_facts"].items()[i];
      for (const auto *field : {"quality", "strict_pass_rate", "strict_passed", "invalid_loops",
                                "invalid_loop_count", "metric_coverage"}) expected.fields().erase(field);
      expect_robust(record["task_facts"].items()[i].dump() == expected.dump(),
          "reduce lost a distinct same-task segment diagnosis");
      distinct.insert(record["task_facts"].items()[i].dump());
    }
  }
  expect_robust(chunk["intermediate_proposals"].items().size() == 2 && distinct.size() == 64,
      "compact-record chunk regressed to two singleton carries or merged different facts");
  std::cout << "reduction_budget source_ids_scopes=99 associated_text_exact=1 ledger_tasks=32"
               " distinct_segment_facts=64 compact_pair_bytes=" << reflection_input_bytes(chunk) << '\n';
}

Json applicability_seed(unsigned source_count = 1, const std::string &content = "",
                        const std::string &problem = "") {
  namespace te = trajectory::evolution;
  namespace ep = agentflow::trajectory::evaluation;
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1); memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1"); memory.set_revision(1);
  auto *entry = memory.add_entries(); entry->set_memory_id("experience-a");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *source = entry->mutable_candidate(); source->set_schema_version(1);
  source->set_candidate_id("candidate-a");
  source->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  source->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  source->set_target_scope("coordinator"); source->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  source->set_trigger_pattern("调用不熟悉的API时先查询apis.api_docs.show_api_doc确认参数。");
  source->set_observed_problem(problem.empty() ? "API arguments were unknown" : problem);
  source->set_proposed_change(content.empty() ? "Inspect API documentation before calling an unfamiliar API." : content);
  source->set_support_user_count(1);
  auto *support = source->add_support_evidence(); support->set_evidence_id("evidence-a");
  support->mutable_source()->set_user_id("user-a"); support->mutable_source()->set_dialogue_id("source-a");
  const auto prototype = *entry;
  for (unsigned i = 1; i < source_count; ++i) {
    auto *other = memory.add_entries(); *other = prototype;
    const auto suffix = std::string(1, static_cast<char>('a' + i));
    other->set_memory_id("experience-" + suffix);
    other->mutable_candidate()->set_candidate_id("candidate-" + suffix);
    other->mutable_candidate()->set_observed_problem("API arguments were unknown for stage " + suffix);
    other->mutable_candidate()->set_proposed_change("Inspect API documentation for stage " + suffix + ".");
  }
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent-a"}, {"user_id", "user-a"},
      {"conversation_id", "conv-a"}, {"run_id", "run-a"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"coordinator"}},
      {"max_policy_entries", 64}, {"max_advice_items", 3}, {"max_advice_bytes", 4096},
      {"max_query_bytes", 16384}, {"max_memory_bytes", 1048576}, {"max_snapshot_bytes", 262144},
      {"max_match_terms", 128}, {"min_matched_terms", 2}});
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  auto options = proto<te::v1::RuntimeOptions>(Json{{"requested_level", "L1"}, {"fallback", "strict"},
      {"host_capabilities", Json::Array{"context.request"}}, {"allowed_users", Json::Array{"user-a"}},
      {"allowed_conversations", Json::Array{"conv-a"}}, {"experience_template", "{{experience}}"},
      {"max_context_bytes", 4096}});
  for (const auto &approved : snapshot.experiences()) {
    auto *ref = options.add_approved_experiences();
    ref->set_memory_id(approved.memory_id()); ref->set_content_revision(approved.content_revision());
  }
  return Json{{"schema_version", 1}, {"agent", Json{{"revision", "applicability-fixture"}}},
      {"user_id", "user-a"}, {"level", "L1"},
      {"policy", json(te::compile_runtime_policy(snapshot, options))}};
}

Report applicability_report(Search &search) {
  const auto policy = proto<trajectory::evolution::v1::RuntimePolicy>(search.seed["policy"]);
  Report report; report.set_complete(true);
  for (unsigned i = 0; i < 32; ++i) {
    const auto id = "applicability-" + std::to_string(i);
    auto *task = search.dataset.add_tasks(); task->set_task_id(id); task->set_split("train");
    task->set_task_input("Inspect API documentation and complete the requested operation.");
    task->set_expected_behavior("Complete the operation");
    auto *trial = report.add_trials(); trial->set_trial_id(id); trial->set_task_id(id);
    trial->set_variant_id("candidate"); trial->set_split("train");
    trial->set_execution_status("completed"); trial->set_verdict(i == 0 ? "failed" : "passed");
    auto *strict = trial->add_checks(); strict->set_check_id("strict");
    strict->set_status(i == 0 ? "failed" : "passed");
    Json evidence{{"assessment", Json{{"version", "goal-assessment-v1"}, {"quality", 1},
        {"quality_upper", 1}, {"coverage", 1}, {"critical_failures", 0}, {"goals", Json::Array{}}}},
        {"loop_review", Json{{"status", "complete"}, {"count", i == 0 ? 2 : 0}}}};
    if (i < 2) {
      auto identity = policy.snapshot().identity(); identity.set_run_id("applicability-run-" + std::to_string(i));
      trajectory::evolution::v1::RuntimeQuery query;
      *query.mutable_advice_query()->mutable_identity() = identity;
      query.mutable_advice_query()->set_query_id("query-a");
      query.mutable_advice_query()->set_decision_id("decision-a");
      query.mutable_advice_query()->set_target_scope("coordinator");
      query.mutable_advice_query()->set_task("Inspect apis.api_docs.show_api_doc for API documentation.");
      query.mutable_advice_query()->set_state(
          "apis.api_docs.show_api_doc(app_name='phone', api_name='login'); value='private_secret_value'");
      query.set_execution_id("coordinator"); query.set_boundary("llm");
      const Json event{{"attributes", Json{{"type", "policy.query"},
          {"decision", Json{{"policy_id", policy.policy_id()}, {"query", json(query)}}}}}};
      evidence["trace"] = Json{{"identity", json(identity)}, {"events", Json::Array{event}}};
    }
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(evidence);
  }
  return report;
}

Json experience_draft(const std::string &trigger) {
  return Json{{"reason", "Revise only the approved experience"}, {"experiences", Json::Array{Json{
      {"memory_id", "experience-a"}, {"content", "Inspect the actual API documentation and validate parameters."},
      {"trigger_pattern", trigger}}}}};
}

void applicability_regressions() {
  Search search; search.seed = applicability_seed();
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 4}, {"max_model_calls", 64},
      {"reflection_strategy", "combee"}, {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const Json selected = Json::Array{"experience:experience-a"};
  auto report = applicability_report(search); const auto original = report.SerializeAsString();
  const auto recorded = search.experience_query_evidence(report, selected);
  const auto bad_draft = experience_draft("调用 login 或认证类API时先查询文档并核对凭据来源。");
  const auto bad = search.mutate(search.seed, bad_draft, selected);
  const auto failed = search.experience_applicability_preflight(search.seed, bad, report, selected, &recorded);
  expect_robust(!failed.flag("passed", true) && failed.str("status") == "complete" &&
      failed["sources"]["experience-a"].num("queries", 0) == 2 &&
      failed["missing"].items().size() == 1, "unmatchable ordinary Chinese trigger admitted");
  expect_robust(failed.dump().find("show_api_doc") != std::string::npos &&
      failed.dump().find("private_secret_value") == std::string::npos,
      "repair omitted actual API anchor or exposed a credential-like state value");
  for (const auto *trigger : {"需要查询apis.api_docs.show_api_doc确认参数时先验证来源。",
                            "Inspect apis.api_docs.show_api_doc before unfamiliar API calls"}) {
    const auto candidate = search.mutate(search.seed, experience_draft(trigger), selected);
    const auto checked = search.experience_applicability_preflight(search.seed, candidate, report, selected, &recorded);
    expect_robust(checked.flag("passed", false) && checked.str("status") == "complete" &&
        !checked["sources"]["experience-a"]["matched_task_ids"].items().empty(),
        "actual native match rejected by language or host-profile assumptions");
    expect_robust(candidate["policy"]["options"]["approved_experiences"].dump() ==
        search.seed["policy"]["options"]["approved_experiences"].dump(), "repair widened source approval");
  }
  Report unavailable; unavailable.set_complete(true);
  expect_robust(search.experience_applicability_preflight(search.seed, bad, unavailable, selected).flag("passed", false),
      "missing query evidence invented a negative applicability result");
  const auto disabled = search.mutate(search.seed,
      Json{{"reason", "Disable this approved source"}, {"experiences", Json::Array{
          Json{{"memory_id", "experience-a"}, {"content", nullptr}}}}}, selected);
  expect_robust(search.experience_applicability_preflight(search.seed, disabled, report, selected).flag("passed", false),
      "intentional disable demanded positive injection");
  auto restricted = recorded;
  for (auto &trace : restricted)
    for (auto &query : trace.queries)
      query.mutable_advice_query()->set_state(Json{{"runtime_experience_allowlist", Json::Array{
          Json{{"memory_id", "experience-a"}, {"content_revision", "wrong-revision"}}}}}.dump());
  const auto wrong_ref = search.experience_applicability_preflight(search.seed, bad, report, selected, &restricted);
  expect_robust(wrong_ref.flag("passed", false) && wrong_ref.str("status") == "unavailable",
      "source replay ignored the recorded exact revision allowlist");
  std::cout << "native_applicability bad_chinese_rejected=1 actual_chinese_match=1 english_match=1"
               " exact_approval=1 unavailable_preserved=1 credential_values_absent=1\n";

  struct RepairModel final : legacy::ModelAdapter {
    bool repeat_bad = false;
    bool abstain = false;
    bool saw_sole_legal = false;
    std::size_t maps = 0, repairs = 0, reduces = 0, reviews = 0;
    std::set<std::string> covered;
    std::string name() const override { return "local-applicability-repair"; }
    std::string model_id() const override { return "local"; }
    legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
      const auto packet = Json::parse(request.user_prompt);
      expect_robust(reflection_packet_metrics_complete(packet), "applicability repair lost official facts");
      legacy::RawModelResponse response; response.transport_ok = true;
      if (packet.str("phase") == "pool_diagnostic_map") {
        ++maps;
        for (const auto &row : packet["feedback"].items()) covered.insert(row.str("task_id"));
        response.body = maps == 1 ? experience_draft("调用 login 或认证类API时先查询文档并核对凭据来源。").dump()
            : Json{{"reason", "No further supported revision"}, {"experiences", Json::Array{}}}.dump();
      } else if (packet.str("phase") == "pool_map_repair") {
        ++repairs;
        expect_robust(packet.str("error") == "gepa_experience_trigger_unmatchable" &&
            packet["experience_preflight"].dump().find("show_api_doc") != std::string::npos &&
            packet["selected_components"].dump() == Json(Json::Array{"experience:experience-a"}).dump(),
            "bounded repair lost the runtime failure or widened authorized axes");
        response.body = experience_draft(repeat_bad ? "调用 login 或认证类API时先查询文档。"
            : "需要查询apis.api_docs.show_api_doc确认参数时先验证来源。").dump();
      } else {
        if (packet.str("phase") == "pool_counterexample_review") ++reviews; else ++reduces;
        if (abstain && packet.str("phase") != "pool_counterexample_review") {
          const auto &choices = packet["legal_selection_choices"]["experience:experience-a"];
          expect_robust(choices.array() && choices.items().size() == 1 &&
              packet["intermediate_proposals"].items().size() == 1 &&
              packet["intermediate_proposals"].items()[0].str("id") == choices.items()[0].text() &&
              packet["intermediate_proposals"].items()[0]["candidate_values"].has("experience:experience-a"),
              "abstention fixture did not expose its sole actually legal candidate");
          saw_sole_legal = true;
        }
        const auto &ledger = packet.str("phase") == "pool_counterexample_review"
            ? packet["case_outcomes"] : packet["training_batch_summary"];
        expect_robust(ledger.items().size() == 32, "repair/reduce/review dropped a formal TRAIN task");
        Json selection = Json::Object{};
        for (const auto &proposal : packet["intermediate_proposals"].items())
          if (!abstain && proposal["candidate_values"].has("experience:experience-a")) {
            selection["experience:experience-a"] = proposal["id"]; break;
          }
        response.body = Json{{"reason", "Keep only the genuinely matchable approved revision"},
            {"selected_proposals", selection}}.dump();
      }
      return response;
    }
  };
  for (const unsigned mode : {0u, 1u, 2u}) {
    const bool repeat = mode == 1, abstain = mode == 2;
    char directory[] = "/tmp/gepa-applicability-repair-XXXXXX";
    expect_robust(::mkdtemp(directory), "cannot create local reflection fixture");
    search.root = directory;
    search.tree = Json{{"started_ms", now()}, {"nodes", Json::Array{Json::Object{}}}};
    search.config["prompt_file"] = std::string(directory) + "/prompt.txt";
    search.config["aggregation_prompt_file"] = search.config["prompt_file"];
    write(search.config.str("prompt_file"), "Use official TRAIN facts; return strict JSON.");
    auto model = std::make_shared<RepairModel>(); model->repeat_bad = repeat; model->abstain = abstain; search.model = model;
    Json pending;
    const auto draft = search.propose_full_pool(search.seed, search.packet(search.seed, report, selected), report, 0, pending);
    expect_robust(model->repairs == 1 && model->covered.size() == 32 && report.SerializeAsString() == original,
        "repair exceeded one retry, abandoned TRAIN facts or changed the original report");
    expect_robust(!abstain || (model->saw_sole_legal && model->reduces > 0 && model->reviews > 0),
        "sole-candidate abstention bypassed reduce/review");
    const auto child = search.mutate(search.seed, draft, selected);
    expect_robust(repeat || abstain ? child["policy"].dump() == search.seed["policy"].dump()
        : search.experience_applicability_preflight(search.seed, child, report, selected).flag("passed", false),
        "repeated unmatchable trigger survived or corrected native trigger was lost");
    std::cout << "bounded_applicability_repair repeated_failure=" << repeat << " abstain=" << abstain << " repairs=" << model->repairs
              << " mapped_tasks=" << model->covered.size() << " reduces=" << model->reduces
              << " reviews=" << model->reviews << " formal_report_unchanged=1\n";
    fs::remove_all(directory);
  }
}


Report prevention_timing_report(Search &search, std::uint64_t query_time,
                               const std::string &operation = "apis.amazon.clear_cart",
                               bool paired = true, bool reactive = false,
                               const std::string &code = "") {
  auto report = applicability_report(search);
  for (auto &trial : *report.mutable_trials()) {
    auto evidence = json(trial.evidence());
    if (!evidence["trace"].object()) continue;
    auto &trace = evidence["trace"];
    auto query = trace["events"].items().front();
    query["event_id"] = "source-query"; query["timestamp_us"] = std::to_string(query_time);
    auto wire = proto<trajectory::evolution::v1::RuntimeQuery>(query["attributes"]["decision"]["query"]);
    wire.mutable_advice_query()->set_task("Inspect the requested cart isolation state.");
    wire.mutable_advice_query()->set_state("apis.amazon.show_cart(); isolation");
    if (reactive) wire.add_error_codes("appworld_auth_401");
    query["attributes"]["decision"]["query"] = json(wire);
    Json start{{"event_id", "source-write"}, {"timestamp_us", "100"}, {"kind", "action.start"},
        {"action_id", "world-write"}, {"actor_id", "world"}, {"execution_id", "main"},
        {"step_id", "world-step"}, {"operation", operation}, {"input", Json{{"code", code}}},
        {"attributes", Json{{"semantic", "action"}}}};
    Json end{{"event_id", "source-write-end"}, {"timestamp_us", "120"}, {"kind", "action.end"},
        {"action_id", "world-write"}, {"actor_id", "world"}, {"execution_id", "main"},
        {"step_id", "world-step"}, {"status", "completed"},
        {"attributes", Json{{"semantic", "action"}, {"executed", true}}}};
    trace["events"] = Json::Array{};
    if (query_time < 100) trace["events"].push(query);
    trace["events"].push(start); if (paired) trace["events"].push(end);
    if (query_time >= 100) trace["events"].push(query);
    *trial.mutable_evidence() = proto<google::protobuf::Struct>(evidence);
  }
  return report;
}

void prevention_timing_regressions() {
  Search search;
  search.seed = applicability_seed(1, "Use apis.amazon.clear_cart before adding only intended cart items.");
  search.config = Json{{"max_experience_bytes", 2048}};
  const Json selected = Json::Array{"experience:experience-a"};
  const auto candidate = search.mutate(search.seed, experience_draft("show_cart isolation"), selected);
  const auto &marked = candidate["policy"]["options"]["l1_prevention_sources"];
  expect_robust(marked.array() && marked.items().size() == 1 && marked.items().front().text() == "experience-a",
      "compose dropped the authorized prevention marker");
  for (const auto &entry : std::vector<std::tuple<unsigned, std::string, bool, const char *>>{
      {50, "apis.amazon.clear_cart", true, "pre_action"},
      {200, "apis.amazon.clear_cart", true, "late"},
      {200, "apis.gmail.clear_cart", true, "unknown"},
      {200, "apis.gmail.delete_email", true, "unknown"},
      {200, "apis.amazon.clear_cart", false, "unknown"},
      {0, "apis.amazon.clear_cart", true, "unknown"}}) {
    auto report = prevention_timing_report(search, std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
    const auto original = report.SerializeAsString();
    const auto checked = search.experience_applicability_preflight(search.seed, candidate, report, selected);
    const auto state = checked["sources"]["experience-a"].str("prevention_status");
    expect_robust(state == std::get<3>(entry) && checked.flag("passed", false) == (state != "late"),
        "source-specific prevention timing or unknown evidence changed");
    expect_robust(report.SerializeAsString() == original, "prevention proof rewrote a formal parent trial");
  }
  for (const auto &code : {"apis.amazon.clear_cart()", "print('apis.amazon.clear_cart()')",
                          "def later():\n    apis.amazon.clear_cart()", "if False:\n    apis.amazon.clear_cart()",
                          "apis.amazon.login(); apis.amazon.clear_cart()"}) {
    auto report = prevention_timing_report(search, 200, "appworld.execute", true, false, code);
    if (std::string(code).find("login") != std::string::npos)
      for (auto &trial : *report.mutable_trials()) {
        auto evidence = json(trial.evidence()); if (!evidence["trace"].object()) continue;
        auto &end = evidence["trace"]["events"].items()[1]; end["status"] = "failed";
        end["error"] = Json{{"code", "appworld_auth_401"}};
        *trial.mutable_evidence() = proto<google::protobuf::Struct>(evidence);
      }
    const auto original = report.SerializeAsString();
    const auto checked = search.experience_applicability_preflight(search.seed, candidate, report, selected);
    expect_robust(checked.flag("passed", false) &&
        checked["sources"]["experience-a"].str("prevention_status") == "unknown" &&
        report.SerializeAsString() == original,
        "submitted code or an early failure manufactured an actual source write");
  }
  auto before = prevention_timing_report(search, 50);
  for (auto &trial : *before.mutable_trials()) {
    auto evidence = json(trial.evidence()); if (!evidence["trace"].object()) continue;
    auto unrelated = evidence["trace"]["events"].items()[1];
    unrelated["timestamp_us"] = "20"; unrelated["action_id"] = "unrelated-write";
    unrelated["operation"] = "apis.gmail.delete_email";
    auto end = evidence["trace"]["events"].items()[2]; end["timestamp_us"] = "30";
    end["action_id"] = "unrelated-write";
    auto events = evidence["trace"]["events"]; evidence["trace"]["events"] = Json::Array{unrelated, end};
    for (const auto &event : events.items()) evidence["trace"]["events"].push(event);
    *trial.mutable_evidence() = proto<google::protobuf::Struct>(evidence);
  }
  expect_robust(search.experience_applicability_preflight(search.seed, candidate, before, selected).flag("passed", false),
      "an unrelated earlier World write eliminated legitimate prevention");
  auto opportunities = prevention_timing_report(search, 50);
  for (auto &trial : *opportunities.mutable_trials()) {
    auto evidence = json(trial.evidence()); if (!evidence["trace"].object()) continue;
    auto late_query = evidence["trace"]["events"].items().front();
    late_query["event_id"] = "after-source-write-query"; late_query["timestamp_us"] = "200";
    auto wire = proto<trajectory::evolution::v1::RuntimeQuery>(late_query["attributes"]["decision"]["query"]);
    wire.mutable_advice_query()->set_state("apis.amazon.clear_cart(); isolation");
    late_query["attributes"]["decision"]["query"] = json(wire);
    evidence["trace"]["events"].push(late_query);
    *trial.mutable_evidence() = proto<google::protobuf::Struct>(evidence);
  }
  const auto circular = search.mutate(search.seed, experience_draft("clear_cart isolation"), selected);
  const auto rejected = search.experience_applicability_preflight(search.seed, circular, opportunities, selected);
  expect_robust(!rejected.flag("passed", true) && rejected["sources"]["experience-a"].str("prevention_status") == "late" &&
      search.experience_applicability_preflight(search.seed, candidate, opportunities, selected).flag("passed", false),
      "a source-bound corrected native trigger failed to replace its circular late trigger");
  const auto retained = search.compose(candidate, search.components(candidate));
  expect_robust(retained["policy"]["options"]["l1_prevention_sources"].dump() == marked.dump(),
      "normal composition lost the seed prevention marker");
  auto policy = proto<trajectory::evolution::v1::RuntimePolicy>(candidate["policy"]);
  auto options = policy.options(); options.clear_l1_prevention_sources();
  auto legacy = candidate; legacy["policy"] = json(trajectory::evolution::compile_runtime_policy(policy.snapshot(), options));
  const auto late = prevention_timing_report(search, 200);
  expect_robust(search.experience_applicability_preflight(search.seed, legacy, late, selected).flag("passed", false),
      "unmarked legacy recovery was globally forced into prevention");
  Search recovery;
  recovery.seed = applicability_seed(1, "Use apis.amazon.clear_cart for the approved recovery.",
      "The observed request failed with appworld_auth_401.");
  recovery.config = search.config;
  const auto reactive = recovery.mutate(recovery.seed, experience_draft("show_cart isolation"), selected);
  const auto result = recovery.experience_applicability_preflight(recovery.seed, reactive,
      prevention_timing_report(recovery, 200, "apis.amazon.clear_cart", true, true), selected);
  expect_robust(result.flag("passed", false) && result["sources"]["experience-a"].str("prevention_status") == "reactive",
      "source-bound explicit error recovery became a late-prevention rejection");
  std::cout << "native_prevention_timing early=1 late_rejected=1 source_namespace=1 unrelated_write=1"
               " unknown_preserved=1 code_is_not_execution=1 unreachable_unknown=1 early_failure_unknown=1"
               " reactive_preserved=1 corrected_native_trigger=1 compose_marker_retained=1 formal_report_unchanged=1\n";
}

void bundle_regressions() {
  Search search; search.seed = applicability_seed(5);
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 4}, {"max_model_calls", 64},
      {"reflection_strategy", "combee"}, {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  auto report = applicability_report(search);
  const auto policy = proto<trajectory::evolution::v1::RuntimePolicy>(search.seed["policy"]);
  const auto action = [](const std::string &id, const std::string &actor,
                         const std::string &operation, bool failed) {
    auto result = Json{{"event_id", id}, {"kind", "action.end"}, {"actor_id", actor},
        {"execution_id", "coordinator"}, {"operation", operation}, {"status", failed ? "failed" : "completed"},
        {"attributes", Json{{"executed", true}, {"semantic", "action"}}},
        {"input", "unsafe_fixture_payload"}, {"output", "unsafe_fixture_payload"}};
    if (failed) result["error"] = Json{{"code", "stock_unavailable"}};
    return result;
  };
  for (unsigned i = 0; i < 4; ++i) {
    auto evidence = json(report.trials(i).evidence());
    auto identity = policy.snapshot().identity(); identity.set_run_id("applicability-run-" + std::to_string(i));
    if (i == 2) {
      evidence["trace"] = Json{{"identity", json(identity)}, {"events", Json::Array{
          action("search", "inventory-manager", "catalog.search_inventory", false),
          action("reserve", "order-manager", "orders.reserve_stock", true),
          action("reply", "customer-manager", "customer.reply", false)}}};
      report.mutable_trials(i)->set_verdict("failed"); report.mutable_trials(i)->mutable_checks(0)->set_status("failed");
    } else if (i == 3) {
      evidence["trace"] = Json{{"identity", json(identity)}, {"events", Json::Array{
          action("plan", "device-gui", "gui.plan", false),
          action("reply", "customer-manager", "customer.reply", false)}}};
      evidence["assessment"]["quality"] = 0;
      evidence["assessment"]["quality_upper"] = 0;
      evidence["assessment"]["goals"] = Json::Array{Json{{"id", "deliver_plan_steps"},
          {"turn", 1}, {"due_turn", 1}, {"status", "failed"}, {"critical_failure", false},
          {"reason", "The tool completed the plan, but the final reply did not deliver its steps. " +
              std::string(700, 'd')}}};
      report.mutable_trials(i)->set_verdict("failed");
      report.mutable_trials(i)->mutable_checks(0)->set_status("failed");
    } else {
      Json refs = Json::Array{};
      for (const auto &source : policy.snapshot().experiences())
        if (source.memory_id() != "experience-e")
          refs.push(Json{{"memory_id", source.memory_id()}, {"content_revision",
              source.memory_id() == "experience-d" ? "wrong-revision" : source.content_revision()}});
      evidence["trace"]["events"].push(Json{{"event_id", "receipt-" + std::to_string(i)},
          {"kind", "advice.applied"}, {"application", Json{{"status", "applied"}}},
          {"attributes", Json{{"receipt", Json{{"boundary", "llm"}, {"experiences", refs}}}}}});
      evidence["trace"]["events"].push(action("auth-" + std::to_string(i), "auth-manager", "phone.login", false));
      const auto other = std::find_if(policy.snapshot().experiences().begin(), policy.snapshot().experiences().end(),
          [](const auto &source) { return source.memory_id() == "experience-e"; });
      expect_robust(other != policy.snapshot().experiences().end(), "five-source seed collapsed an approved source");
      auto sibling_identity = identity; sibling_identity.set_run_id("unrelated-run-" + std::to_string(i));
      evidence["traces"] = Json::Array{evidence["trace"], Json{{"identity", json(sibling_identity)},
          {"events", Json::Array{Json{{"event_id", "sibling-receipt"}, {"kind", "advice.applied"},
              {"application", Json{{"status", "applied"}}}, {"attributes", Json{{"receipt", Json{
                  {"boundary", "llm"}, {"experiences", Json::Array{Json{{"memory_id", other->memory_id()},
                  {"content_revision", other->content_revision()}}}}}}}}},
              action("sibling-action", "catalog-manager", "catalog.lookup", false)}}}};
    }
    *report.mutable_trials(i)->mutable_evidence() = proto<google::protobuf::Struct>(evidence);
  }
  const auto original = report.SerializeAsString();
  const auto formal = search.scores(report), available = search.components(search.seed);
  const Json selected = Json::Array{"experience:experience-a"};
  const auto observations = search.experience_bundle_observations(search.seed, report);
  const auto bundle = related_experience_bundle(selected, available, formal, observations, Json::Object{});
  expect_robust(bundle.dump() == Json(Json::Array{"experience:experience-a", "experience:experience-b", "experience:experience-c"}).dump(),
      "bundle lost related approved sources or admitted unrelated/wrong-revision evidence");
  auto no_operations = observations;
  for (auto &row : no_operations.items()) row["executed_operations"] = Json::Array{};
  expect_robust(related_experience_bundle(selected, available, formal, no_operations, Json::Object{}).dump() == selected.dump(),
      "receipt without actual machine operation authorized a bundle");
  auto no_receipts = report;
  for (auto &trial : *no_receipts.mutable_trials()) {
    auto evidence = json(trial.evidence());
    const auto remove = [](Json &trace) {
      if (trace["events"].array()) for (auto &event : trace["events"].items())
        if (event.str("kind") == "advice.applied") event["application"]["status"] = "not_applied";
    };
    if (evidence.has("traces") && evidence["traces"].array())
      for (auto &trace : evidence["traces"].items()) remove(trace);
    else if (evidence.has("trace") && evidence["trace"].object()) remove(evidence["trace"]);
    *trial.mutable_evidence() = proto<google::protobuf::Struct>(evidence);
  }
  expect_robust(related_experience_bundle(selected, available, formal,
      search.experience_bundle_observations(search.seed, no_receipts), Json::Object{}).dump() == selected.dump(),
      "query/non-applied receipt authorized a bundle");
  auto disabled = available; disabled["experience:experience-c"] = nullptr;
  expect_robust(related_experience_bundle(selected, disabled, formal, observations, Json::Object{}).items().size() == 2,
      "disabled null source was added to a bundle");
  auto sibling_report = report;
  for (unsigned i = 0; i < 2; ++i) {
    auto evidence = json(sibling_report.trials(i).evidence());
    evidence["traces"].items()[0]["events"].push(evidence["traces"].items()[1]["events"].items()[0]);
    *sibling_report.mutable_trials(i)->mutable_evidence() = proto<google::protobuf::Struct>(evidence);
  }
  auto unrelated_only = available;
  unrelated_only["experience:experience-b"] = nullptr; unrelated_only["experience:experience-c"] = nullptr;
  expect_robust(related_experience_bundle(selected, unrelated_only, formal,
      search.experience_bundle_observations(search.seed, sibling_report), Json::Object{}).dump() == selected.dump(),
      "same run's separate application receipt was mistaken for source coapplication");
  auto unknown = formal; unknown["applicability-0"] = Json::Object{};
  expect_robust(related_experience_bundle(selected, available, unknown, observations, Json::Object{}).dump() == selected.dump(),
      "incomplete formal facts authorized a failure bundle");
  const auto input = search.packet(search.seed, report, bundle);
  expect_robust(input["training_batch_summary"].items().size() == 32 && reflection_input_bytes(input) <= 65536,
      "bundle packet exceeded budget or lost formal pool facts");
  const auto failed_case = std::find_if(input["feedback"].items().begin(), input["feedback"].items().end(),
      [](const Json &row) { return row.str("task_id") == "applicability-2"; });
  expect_robust(failed_case != input["feedback"].items().end() &&
      failed_case->str("evidence_role") == "unapplied_counterexample" &&
      (*failed_case)["trial"].dump().find("orders.reserve_stock") != std::string::npos &&
      (*failed_case)["trial"].dump().find("unsafe_fixture_payload") == std::string::npos,
      "actual map packet hid a different skill's unapplied failure or added its raw payload");
  Report rest; rest.set_complete(true); *rest.add_trials() = report.trials(2);
  const auto diagnostic = search.packet(search.seed, rest, bundle, false, &formal,
      &input["experiences"], &input["training_batch_summary"]);
  expect_robust(diagnostic["training_batch_summary"].dump() == input["training_batch_summary"].dump(),
      "later map did not retain the exact complete formal pool ledger");
  const auto projected = unapplied_failure_context(search.reviewed_feedback_trial(report.trials(2)));
  expect_robust(projected.dump().find("orders.reserve_stock") != std::string::npos &&
      projected.dump().find("inventory-manager") != std::string::npos &&
      projected.dump().find("customer.reply") != std::string::npos &&
      projected.dump().find("stock_unavailable") != std::string::npos &&
      projected.dump().find("unsafe_fixture_payload") == std::string::npos,
      "unapplied diagnostic lost first failure/adjacent actors or added raw input/output");
  const auto delivery_full = search.reviewed_feedback_trial(report.trials(3));
  const auto delivery_projection = unapplied_failure_context(delivery_full);
  expect_robust(delivery_projection["goal_completion"].dump() == delivery_full["goal_completion"].dump() &&
      delivery_projection["goal_completion"]["issues"].items().size() == 1 &&
      delivery_projection["goal_completion"]["issues"].items()[0].str("id") == "deliver_plan_steps" &&
      delivery_projection["goal_completion"]["issues"].items()[0].num("turn", 0) == 1 &&
      delivery_projection["goal_completion"]["issues"].items()[0].num("due_turn", 0) == 1 &&
      delivery_projection["goal_completion"]["issues"].items()[0].str("status") == "failed" &&
      delivery_projection["goal_completion"]["issues"].items()[0].str("reason").size() <= 512 &&
      delivery_projection.dump().find("unsafe_fixture_payload") == std::string::npos,
      "completed tools lost the existing bounded undelivered-goal diagnostic");
  for (const auto &run : delivery_projection["evidence"]["runs"].items())
    for (const auto &fact : run["actions"].items())
      expect_robust(fact.str("status") == "completed" && !fact.flag("observed_failure", false),
          "undelivered goal was falsely reclassified as an action failure");
  const auto revised = search.mutate(search.seed, Json{{"reason", "Coordinate the approved stages"},
      {"experiences", Json::Array{Json{{"memory_id", "experience-b"},
          {"content", "Validate documented parameters before the next actual operation."}},
          Json{{"memory_id", "experience-c"}, {"content", "Check the requested result before finishing."}}}}}, bundle);
  expect_robust(revised["policy"]["options"]["approved_experiences"].dump() ==
      search.seed["policy"]["options"]["approved_experiences"].dump(), "bundle compilation widened approved refs");
  expect_robust(revised["policy"]["options"]["l1_prevention_sources"].items().size() == 2,
      "real bundle changes did not carry their two approved host activation markers");
  const auto removed = search.mutate(search.seed, Json{{"reason", "Remove only a harmful approved stage"},
      {"experiences", Json::Array{Json{{"memory_id", "experience-b"}, {"content", nullptr}}}}}, bundle);
  expect_robust(search.components(removed)["experience:experience-b"].null(), "bundle broke null removal semantics");
  expect_robust(!noop_experience_repair_packet(input, Json{{"reason", "attempted revision"},
      {"experiences", Json::Array{Json{{"memory_id", "experience-b"}, {"content", "same"}}}}}, 65536).null(),
      "multi-source no-op lost its one bounded correction opportunity");
  expect_robust(report.SerializeAsString() == original, "bundle reflection altered the original report");
  struct BundleMapModel final : legacy::ModelAdapter {
    std::size_t maps = 0, peak = 0; std::set<std::string> covered;
    bool saw_undelivered_goal = false;
    std::string name() const override { return "local-bundle-map"; }
    std::string model_id() const override { return "local"; }
    legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
      const auto packet = Json::parse(request.user_prompt);
      peak = std::max(peak, reflection_input_bytes(packet));
      expect_robust(peak <= 65536, "bundle map exceeded the fixed reflection budget");
      const auto &ledger = packet.str("phase") == "pool_counterexample_review"
          ? packet["case_outcomes"] : packet["training_batch_summary"];
      expect_robust(ledger.items().size() == 32 && reflection_packet_metrics_complete(packet),
          "bundle map/reduce did not expose complete formal pool facts");
      if (packet.str("phase") == "pool_diagnostic_map") {
        ++maps;
        expect_robust(packet["selected_components"].items().size() == 3,
            "map silently reverted the related bundle to one source");
        for (const auto &row : packet["feedback"].items()) {
          covered.insert(row.str("task_id"));
          if (row.str("task_id") == "applicability-3") {
            const auto &issues = row["trial"]["goal_completion"]["issues"];
            expect_robust(row.str("evidence_role") == "unapplied_counterexample" &&
                row["selected_experience_applications"].num("count", 0) == 0 && issues.array() &&
                issues.items().size() == 1 && issues.items()[0].str("id") == "deliver_plan_steps" &&
                issues.items()[0].str("reason").find("did not deliver its steps") != std::string::npos &&
                row["trial"].dump().find("unsafe_fixture_payload") == std::string::npos,
                "actual bounded map hid an undelivered goal after completed tools");
            saw_undelivered_goal = true;
          }
        }
      }
      legacy::RawModelResponse response; response.transport_ok = true;
      response.body = packet.str("phase") == "pool_diagnostic_map"
          ? Json{{"reason", "Retain all approved stages"}, {"experiences", Json::Array{}}}.dump()
          : Json{{"reason", "Abstain without choosing a default"}, {"selected_proposals", Json::Object{}}}.dump();
      return response;
    }
  };
  char directory[] = "/tmp/gepa-bundle-map-XXXXXX";
  expect_robust(::mkdtemp(directory), "cannot create local bundle fixture");
  search.root = directory; search.tree = Json{{"started_ms", now()}, {"nodes", Json::Array{Json::Object{}}}};
  search.config["prompt_file"] = std::string(directory) + "/prompt.txt";
  search.config["aggregation_prompt_file"] = search.config["prompt_file"];
  write(search.config.str("prompt_file"), "Use complete official TRAIN facts; return strict JSON.");
  for (auto &task : *search.dataset.mutable_tasks()) {
    task.set_task_input(std::string(4090, 'x'));
    for (unsigned c = 0; c < 4; ++c) task.add_constraints(std::string(1024, 'c'));
  }
  search.config["max_feedback_trials"] = 32;
  auto model = std::make_shared<BundleMapModel>(); search.model = model;
  const auto large_input = search.packet(search.seed, report, bundle);
  Json pending;
  const auto abstain = search.propose_full_pool(search.seed, large_input, report, 0, pending);
  expect_robust(model->maps > 1 && model->covered.size() == 32 && model->saw_undelivered_goal &&
      model->peak > 60000 &&
      search.mutate(search.seed, abstain, bundle)["policy"].dump() == search.seed["policy"].dump(),
      "bounded bundle maps lost coverage, failed to exercise the budget or invented an update");
  expect_robust(report.SerializeAsString() == original, "full-pool bundle changed formal parent evidence");
  fs::remove_all(directory);
  std::cout << "related_bundle sources=3 exact_approval=1 current_failed_run=1 unrelated_run_rejected=1"
               " query_only_rejected=1 no_actual_operation_rejected=1 null_preserved=1 formal_tasks=32"
               " raw_payload_absent=1 completed_tool_undelivered_goal=1 packet_bytes=" << reflection_input_bytes(input)
            << " maps=" << model->maps << " peak_bytes=" << model->peak << '\n';
}

void transport_checkpoint_regressions() {
  struct CheckpointModel final : legacy::ModelAdapter {
    std::size_t calls = 0;
    std::map<std::string, std::size_t> attempts;
    std::set<std::string> covered;
    std::string name() const override { return "local-checkpoint-fault"; }
    std::string model_id() const override { return "local"; }
    legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
      ++calls; ++attempts[request.packet_id];
      const auto packet = Json::parse(request.user_prompt);
      expect_robust(packet["training_batch_summary"].items().size() == 32 &&
          reflection_packet_metrics_complete(packet), "checkpoint recovery lost formal TRAIN facts");
      legacy::RawModelResponse response;
      if (packet.str("phase") == "pool_diagnostic_map" && calls == 2) {
        response.error_code = "http_429"; return response;
      }
      for (const auto &row : packet["feedback"].items()) covered.insert(row.str("task_id"));
      response.transport_ok = true;
      response.body = packet.str("phase") == "pool_diagnostic_map"
          ? Json{{"reason", "Keep the approved parent"}, {"experiences", Json::Array{}}}.dump()
          : Json{{"reason", "Abstain"}, {"selected_proposals", Json::Object{}}}.dump();
      return response;
    }
  };
  char directory[] = "/tmp/gepa-stage-checkpoint-XXXXXX";
  expect_robust(::mkdtemp(directory), "cannot create checkpoint fixture");
  Search search; search.root = directory; search.seed = applicability_seed(2);
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 4},
      {"max_model_calls", 64}, {"reflection_strategy", "combee"},
      {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const auto started = now();
  search.tree = Json{{"started_ms", started}, {"trials_used", 32},
      {"nodes", Json::Array{Json{{"id", 0}, {"status", "pending"}}}}};
  search.config["prompt_file"] = std::string(directory) + "/prompt.txt";
  search.config["aggregation_prompt_file"] = search.config["prompt_file"];
  write(search.config.str("prompt_file"), "Use official TRAIN facts; return strict JSON.");
  const auto report = applicability_report(search);
  const auto original = report.SerializeAsString();
  const Json selected = Json::Array{"experience:experience-a"};
  search.tree["nodes"].items()[0]["components"] = selected;
  auto priority = search.components(search.seed);
  priority["__gepa_selection_evidence"] = Json{{"components", Json::Object{}}};
  for (const auto *source : {"experience:experience-a", "experience:experience-b"})
    priority["__gepa_selection_evidence"]["components"][source] = Json{
        {"failed_task_ids", Json::Array{"applicability-0", "applicability-1"}},
        {"applied_task_ids", Json::Array{"applicability-0", "applicability-1"}}, {"attempts", 0}};
  expect_robust(search.pending_reflection_components(0, priority, false).dump() == selected.dump(),
      "fresh node did not follow current failure priority");
  // Paid generation reserves source a. The same formal gaps now favour b by attempts.
  search.tree["nodes"].items()[0]["component_reserved"] = true;
  priority["__gepa_selection_evidence"]["components"]["experience:experience-a"]["attempts"] = 1;
  const Json alternate = Json::Array{"experience:experience-b"};
  expect_robust(!search.reflection_components_checkpoint(0) &&
      search.pending_reflection_components(0, priority, false).dump() == alternate.dump(),
      "reservation without a real reflection request froze an unstarted node");
  const auto input = search.packet(search.seed, report, selected);
  auto model = std::make_shared<CheckpointModel>(); search.model = model;
  Json pending; std::string error;
  try { (void)search.propose_full_pool(search.seed, input, report, 0, pending); }
  catch (const std::exception &failure) { error = failure.what(); }
  expect_robust(error == "gepa_model_rate_limited" && model->calls == 2,
      "map rate limit did not preserve its resumable stage");
  const auto completed = fs::path(directory) / "reflections/0.pool-map-0.draft.json";
  const auto request = fs::path(directory) / "reflections/0.pool-map-0.request.json";
  const auto completed_bytes = read(completed), request_bytes = read(request);
  // Same-job restart reloads the durable budget/node checkpoint, retaining completed maps.
  search.tree = Json::parse(read(fs::path(directory) / "tree.json"));
  expect_robust(refine_current_pool_selection(selected, priority, 0, false, "pending").dump() ==
      alternate.dump(), "fixture did not reproduce attempts-based a-to-b rerouting");
  expect_robust(search.reflection_components_checkpoint(0) &&
      search.pending_reflection_components(0, priority, false).dump() == selected.dump(),
      "429 recovery changed its recorded source because attempt priority changed");
  auto &node = search.tree["nodes"].items()[0];
  node["component_reserved"] = false;
  expect_robust(!search.reflection_components_checkpoint(0), "unreserved node borrowed a request");
  node["component_reserved"] = true;
  node["components"] = alternate;
  expect_robust(!search.reflection_components_checkpoint(0), "mismatched request froze another source");
  node["components"] = Json::Array{"experience:experience-a", "loop_rules"};
  node["component_coupling"] = "same_run_applied_experience_control";
  const auto coupled = node["components"].dump();
  expect_robust(search.pending_reflection_components(0, priority, false).dump() == coupled &&
      node.str("component_coupling") == "same_run_applied_experience_control",
      "recorded reflection lost its original coupled axes");
  node["components"] = selected;
  const auto resumed = search.pending_reflection_components(0, priority, false);
  const auto draft = search.propose_full_pool(search.seed, search.packet(search.seed, report, resumed),
                                             report, 0, pending);
  const auto first = std::find_if(model->attempts.begin(), model->attempts.end(),
      [](const auto &entry) { return entry.first.find(".pool-map-0") != std::string::npos; });
  const auto second = std::find_if(model->attempts.begin(), model->attempts.end(),
      [](const auto &entry) { return entry.first.find(".pool-map-1") != std::string::npos; });
  expect_robust(first != model->attempts.end() && first->second == 1 &&
      second != model->attempts.end() && second->second == 2 && model->covered.size() == 32,
      "resume replayed a completed map or skipped the interrupted map/cohort");
  expect_robust(search.tree.num("model_calls", 0) == model->calls &&
      search.tree.num("trials_used", 0) == 32 && search.tree.num("started_ms", 0) == started &&
      report.SerializeAsString() == original && read(completed) == completed_bytes && read(request) == request_bytes &&
      search.mutate(search.seed, draft, selected)["policy"].dump() == search.seed["policy"].dump(),
      "same-job recovery reset its budget, rewrote healthy facts or changed an abstaining parent");
  fs::remove_all(directory);
  std::cout << "native_429_checkpoint attempt_priority_reroute_reproduced=1 axis_and_coupling_frozen=1"
               " completed_map_reused=1 interrupted_map_resumed=1"
               " formal_tasks=32 same_budget=1 parent_report_unchanged=1\n";
}

void replay_maps(const fs::path &dir, std::size_t node) {
  std::set<std::string> ids;
  std::size_t maps = 0, largest = 0;
  for (std::size_t index = 0; index < 64; ++index) {
    const auto path = dir / (std::to_string(node) + ".pool-map-" + std::to_string(index) + ".request.json");
    if (!fs::exists(path)) break;
    const auto original = Json::parse(read(path, 262144))["input"];
    const auto source = original.dump();
    if (node == 21) {
      expect_robust(!reflection_packet_metrics_complete(original), "real Node21 missing official facts concealed");
    } else {
      expect_robust(reflection_packet_metrics_complete(original), "real scored map lost an official fact");
      const auto facts = reflection_task_facts(original["feedback"], original["selected_components"]);
      expect_robust(facts.items().size() == original["feedback"].items().size(), "real map fact coverage lost");
      for (std::size_t i = 0; i < facts.items().size(); ++i)
        expect_robust(facts.items()[i]["task_id"].dump() == original["feedback"].items()[i]["task_id"].dump(),
                      "real long task ID changed");
      auto pressure = original;
      pressure["prior_search_rejections"] = Json::Array{Json{{"reason", std::string(12000, 'd')}}};
      const auto bounded = compact_reflection_metadata(pressure, 65536);
      expect_robust(reflection_input_bytes(bounded) <= 65536 &&
          bounded["training_batch_summary"].dump() == original["training_batch_summary"].dump() &&
          reflection_packet_metrics_complete(bounded), "real metadata pressure broke formal ledger");
      largest = std::max(largest, reflection_input_bytes(bounded));
    }
    for (const auto &row : original["feedback"].items()) ids.insert(row.str("task_id"));
    expect_robust(original.dump() == source, "saved source request mutated");
    ++maps;
  }
  expect_robust(maps > 0, "requested real fixture missing");
  std::cout << "real_node=" << node << " maps=" << maps << " available_tasks=" << ids.size()
            << " largest_bounded=" << largest << " source_unchanged=1\n";
}

void replay_review(const fs::path &dir, std::size_t node) {
  const auto prefix = std::to_string(node);
  auto review = Json::parse(read(dir / (prefix + ".pool-map-0.request.json"), 262144))["input"];
  Json selection;
  bool reduced = false;
  for (std::size_t level = 0; level < 32; ++level) {
    const auto stem = prefix + ".pool-reduce-" + std::to_string(level);
    if (!fs::exists(dir / (stem + ".request.json")) || !fs::exists(dir / (stem + ".draft.json"))) continue;
    const auto packet = Json::parse(read(dir / (stem + ".request.json"), 262144))["input"];
    review["intermediate_proposals"] = packet["intermediate_proposals"];
    selection = Json::parse(read(dir / (stem + ".draft.json"), 262144));
    reduced = true;
  }
  expect_robust(reduced, "real final reduce fixture unavailable");
  review["phase"] = "pool_counterexample_review";
  review["proposed_strategy"] = selection;
  review["proposed_selection"] = selection["selected_proposals"];
  review["case_outcomes"] = Json::Array{};
  review["feedback"] = Json::Array{};
  Json all = Json::Array{};
  std::set<std::string> seen;
  for (std::size_t index = 0; index < 64; ++index) {
    const auto path = dir / (prefix + ".pool-map-" + std::to_string(index) + ".request.json");
    if (!fs::exists(path)) break;
    const auto packet = Json::parse(read(path, 262144))["input"];
    for (const auto &row : packet["feedback"].items()) {
      if (!seen.insert(row.str("task_id")).second) continue;
      const auto &trial = row["trial"];
      Json outcome{{"task_id", row["task_id"]}, {"verdict", trial["verdict"]},
                   {"execution_status", trial["execution_status"]}, {"metrics", trial["metrics"]}};
      for (const auto *field : {"quality", "strict_pass_rate", "strict_passed", "invalid_loops",
                                "invalid_loop_count", "metric_coverage"})
        if (trial["metrics"].has(field)) outcome[field] = trial["metrics"][field];
      if (trial["goal_completion"].object()) outcome["goal_completion"] = trial["goal_completion"];
      if (row["selected_experience_applications"].object())
        outcome["selected_experience_applications"] = row["selected_experience_applications"];
      review["case_outcomes"].push(outcome);
      all.push(row);
    }
  }
  const auto pair = select_full_pool_review_cases(all, Json::Object{});
  for (const auto *name : {"failure", "success"})
    if (pair[name].object()) review["feedback"].push(
        compact_full_pool_review_case(pair[name], review["selected_components"], Json::Object{}));
  const auto before = reflection_input_bytes(review);
  const auto compact = compact_full_pool_review(review, selection, 65536);
  expect_robust(compact["case_outcomes"].items().size() == 32 &&
      compact["feedback"].items().size() == 2 && reflection_packet_metrics_complete(compact),
      "real review lost full 32 official facts or real contrast pair");
  for (std::size_t i = 0; i < 32; ++i)
    for (const auto *field : {"task_id", "quality", "strict_pass_rate", "invalid_loops"})
      expect_robust(compact["case_outcomes"].items()[i][field].dump() ==
                    review["case_outcomes"].items()[i][field].dump(), "real review changed mandatory fact");
  std::cout << "real_node=" << node << " review_before=" << before << " review_after="
            << reflection_input_bytes(compact) << " formal_tasks=32 contrast_cases=2\n";
}

void multi_loop_budget_regressions() {
  namespace te = trajectory::evolution;
  auto artifact = applicability_seed(2);
  const auto l1 = proto<te::v1::RuntimePolicy>(artifact["policy"]);
  auto options = l1.options();
  options.set_requested_level(te::v1::L3);
  options.add_host_capabilities("action.filter"); options.add_host_capabilities("loop.decide");
  auto *base = options.add_loop_rules();
  base->set_rule_id("local-bounded-loop"); base->set_target_scope("coordinator"); base->set_reason("local bounded recovery");
  base->set_max_iterations(6); base->set_max_no_progress(3); base->set_max_replans(1);
  base->set_no_progress_decision("replan"); base->set_replan_context("Choose a bounded correction.");
  base->add_error_codes("appworld_auth_401"); base->add_error_codes("appworld_validation_422");
  const auto original = te::compile_runtime_policy(l1.snapshot(), options);
  const Json settings{{"enabled", true}, {"prompt_file", "offline-fixture.txt"}, {"max_rules", 4}};
  Json loops = Json::Array{};
  for (const auto &entry : std::vector<std::pair<const char *, const char *>>{
      {"experience-a", "appworld_auth_401"}, {"experience-b", "appworld_validation_422"}})
    loops.push(Json{{"memory_id", entry.first}, {"actor_id", "worker"}, {"match_mode", "predicate"},
        {"error_codes", Json::Array{entry.second}}, {"replan_on_failure", true}, {"max_replans", 1},
        {"max_iterations", 6}, {"max_no_progress", 3}, {"reason", "source-bound repeated failure"}});
  const auto parent_policy = te::compile_runtime_policy(original.snapshot(), apply_control_draft(
      original.snapshot(), original.options(), settings, Json{{"action_rules", Json::Array{}},
      {"loop_rules", loops}, {"schedule_rules", Json::Array{}}}));
  artifact["level"] = "L3"; artifact["policy"] = json(parent_policy);
  Search search; search.seed = artifact; search.controls = settings;
  search.config = Json{{"max_experience_bytes", 2048}};
  Json packet{{"components", search.components(artifact)}, {"experiences", source_experiences(artifact)},
      {"controls", control_generation_input(parent_policy.snapshot(),
                                            without_generated_controls(parent_policy.options()), settings)}};
  packet["controls"]["editable_bindings"] = Json::Array{};
  Json binding{{"component", "loop_rules"}, {"primitive_id", "bounded_iteration_cap"},
      {"target_scope", "coordinator"}, {"actor_id", "worker"}, {"observed_iteration", 7},
      {"reviewed_waste_after", 2}, {"required_iteration_floor", 2}, {"require_finalization", true},
      {"evidence_ref", "/feedback/0/trial/evidence/runs/0/actions/0"}};
  binding["budget"] = control_binding_budget(packet, binding);
  binding["binding_id"] = control_binding_id(packet, binding);
  packet["controls"]["editable_bindings"].push(binding);
  const auto update = [&](const Json &value, const char *mid, int cap) {
    return Json{{"binding_id", value["binding_id"]}, {"memory_id", mid}, {"max_iterations", cap},
                {"reason", "retain necessary work and bound the reviewed waste suffix"}};
  };
  Json body{{"reason", "typed group budget"}, {"control_binding_updates",
      Json::Array{update(binding, "experience-a", 5)}}};
  // The real production order guards the raw typed protocol before resolving;
  // mutate then expands parent indices and composes under exact native approval.
  require_typed_budget_updates(body, packet);
  const auto resolved = resolve_control_binding_updates(body, packet);
  const auto child = search.mutate(artifact, resolved, Json::Array{"loop_rules"});
  const auto child_policy = proto<te::v1::RuntimePolicy>(child["policy"]);
  expect_robust(resolved["control_updates"].items().size() == 2 &&
      generated_controls_authorized(child_policy, parent_policy, settings), "multi cap lost native authority");
  for (int index = 1; index <= 2; ++index) {
    const auto &rule = child_policy.options().loop_rules(index);
    expect_robust(rule.max_iterations() == 5 && rule.max_no_progress() == 3 &&
        rule.max_replans() == 1 && rule.require_finalization() &&
        rule.source_experience().SerializeAsString() ==
            parent_policy.options().loop_rules(index).source_experience().SerializeAsString() &&
        rule.error_codes(0) == parent_policy.options().loop_rules(index).error_codes(0),
        "multi cap replaced a source, predicate or shared recovery budget");
    te::v1::RuntimeQuery query;
    *query.mutable_advice_query()->mutable_identity() = child_policy.snapshot().identity();
    query.mutable_advice_query()->set_target_scope("coordinator");
    query.mutable_advice_query()->set_query_id("local-query");
    query.mutable_advice_query()->set_decision_id("local-decision");
    query.set_boundary("loop"); query.set_actor_id("worker"); query.set_execution_id("local-execution");
    query.set_iteration(6); query.set_state_fingerprint("local-state"); query.set_can_finalize(true);
    query.add_error_codes(rule.error_codes(0));
    expect_robust(te::PolicyRuntime(parent_policy).decide(query).progress().decision() == "continue" &&
        te::PolicyRuntime(child_policy).decide(query).progress().decision() == "stop",
        "native multi cap did not apply independently to each cited family");
  }
  // Concrete updates cannot reuse the native resolver's expansion as a bypass.
  Json diagnostic; bool bypass_rejected = false;
  try { require_typed_budget_updates(resolved, packet, &diagnostic); }
  catch (const Error &) { bypass_rejected = diagnostic.str("reason") == "typed_binding_required"; }
  expect_robust(bypass_rejected, "multi cap weakened the non-typed budget guard");
  auto other = binding; other["evidence_ref"] = "/feedback/1/trial/evidence/runs/0/actions/0";
  other["binding_id"] = control_binding_id(packet, other);
  auto twice = packet; twice["controls"]["editable_bindings"].push(other);
  body["control_binding_updates"].push(update(other, "experience-b", 4));
  require_typed_budget_updates(body, twice);
  bool duplicate_rejected = false;
  try {
    const auto duplicate = resolve_control_binding_updates(body, twice, &diagnostic);
    (void)search.mutate(artifact, duplicate, Json::Array{"loop_rules"});
  } catch (const Error &error) {
    duplicate_rejected = std::string(error.what()) == "control_binding_invalid" &&
                         diagnostic.str("reason") == "duplicate_loop_group_update";
  }
  expect_robust(duplicate_rejected && artifact["policy"].dump() == json(parent_policy).dump(),
      "two typed bindings silently replaced the same group's first budget");
  auto single_options = apply_control_draft(original.snapshot(), original.options(), settings,
      Json{{"action_rules", Json::Array{}}, {"loop_rules", Json::Array{loops.items().front()}},
           {"schedule_rules", Json::Array{}}});
  auto single_artifact = artifact;
  single_artifact["policy"] = json(te::compile_runtime_policy(original.snapshot(), single_options));
  auto single = packet; single["components"] = search.components(single_artifact);
  auto single_binding = binding; single_binding["binding_id"] = control_binding_id(single, single_binding);
  single["controls"]["editable_bindings"] = Json::Array{single_binding};
  auto single_body = Json{{"reason", "single budget"}, {"control_binding_updates",
      Json::Array{update(single_binding, "experience-a", 5)}}};
  const auto single_resolved = resolve_control_binding_updates(single_body, single);
  expect_robust(single_resolved["control_updates"].items().size() == 1 &&
      single_resolved["control_updates"].items().front().str("op") == "set" &&
      single_resolved["control_updates"].items().front().num("index", 99) == 0,
      "existing single-rule typed update changed");
  std::cout << "multi_loop_budget native_whole_group=1 source_preserved=1 typed_guard=1"
               " duplicate_group_rejected=1 single_compatible=1\n";
}

void extended_reflection_budget_regressions() {
  const Json original{{"status", "validation_rejected"}, {"stop_reason", "model_budget"},
      {"reflection_exhausted", true}, {"model_calls", 2}, {"rounds", 1}, {"trials_used", 32},
      {"active", nullptr}, {"batch", nullptr}, {"seed_version", "approved-local-parent"},
      {"baseline_version", "fixed-local-L0"}, {"started_ms", now()},
      {"budget_extensions", Json::Array{Json{{"reason", "initial_budget"},
          {"max_model_calls", 3}, {"max_rounds", 2}, {"timeout_ms", 600000}}}},
      {"evaluations", Json{{"healthy", Json{{"status", "complete"}, {"task_ids", Json::Array{"same-task"}}}}}},
      {"nodes", Json::Array{Json{{"id", 0}, {"status", "pending"}, {"parent", nullptr}}}}};
  const Json increased{{"max_model_calls", 6}, {"max_rounds", 4}, {"timeout_ms", 600000}};
  for (unsigned mode = 0; mode < 8; ++mode) {
    auto tree = original, config = increased;
    if (mode == 0) config["max_model_calls"] = 3; // same paid ceiling
    if (mode == 1) { config["max_model_calls"] = 3; tree["stop_reason"] = "max_rounds"; }
    if (mode == 2) tree.fields().erase("budget_extensions"); // legacy checkpoint without proof
    if (mode == 3) tree["holdout_started"] = true;
    if (mode == 4) tree["active"] = 1;
    if (mode == 5) tree["batch"] = Json{{"id", 0}};
    if (mode == 6) tree["budget_extensions"].items().back()["reason"] = "unverified";
    if (mode == 7) tree["model_calls"] = 4; // old recorded ceiling already violated
    const auto before = tree.dump();
    expect_robust(!clear_extended_gepa_reflection_exhaustion(tree, config) && tree.dump() == before,
        "unchanged/round-only/unproved/holdout/active budget reopened or reset a checkpoint");
  }
  // This is the exact production helper called only after retry's existing
  // job/user/source guards; it must mutate only the derived exhaustion bit.
  auto recovered = original;
  expect_robust(clear_extended_gepa_reflection_exhaustion(recovered, increased),
      "proved larger model budget did not reopen reflection");
  auto expected = original; expected.fields().erase("reflection_exhausted");
  expect_robust(recovered.dump() == expected.dump(), "budget extension changed a paid counter or source");
  recovered["budget_extensions"].push(Json{{"reason", "budget_extension"},
      {"max_model_calls", 6}, {"rounds_used", recovered["rounds"]},
      {"model_calls_used", recovered["model_calls"]}});
  auto already_extended = recovered; already_extended["reflection_exhausted"] = true;
  const auto same = already_extended.dump();
  expect_robust(!clear_extended_gepa_reflection_exhaustion(already_extended, increased) &&
      already_extended.dump() == same, "same latest extension revived an exhausted paid budget");
  struct LocalModel final : legacy::ModelAdapter {
    unsigned calls = 0;
    std::string name() const override { return "local-budget-recovery"; }
    std::string model_id() const override { return "local"; }
    legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
      ++calls;
      const auto packet = Json::parse(request.user_prompt);
      expect_robust(reflection_packet_metrics_complete(packet) &&
          packet["training_batch_summary"].items().size() == 32,
          "continued reflection lost the same 32 official facts");
      legacy::RawModelResponse result; result.transport_ok = true;
      result.body = R"({"reason":"Retain the approved parent","experiences":[]})";
      return result;
    }
  };
  char directory[] = "/tmp/gepa-extended-budget-XXXXXX";
  expect_robust(::mkdtemp(directory), "cannot create budget recovery fixture");
  Search search; search.root = directory; search.tree = recovered; search.config = increased;
  search.config["max_input_bytes"] = 65536;
  search.config["prompt_file"] = std::string(directory) + "/prompt.txt";
  write(search.config.str("prompt_file"), "Use all official facts and strict JSON.");
  const auto healthy = fs::path(directory) / "healthy-report.json";
  const auto old_map = fs::path(directory) / "reflections/completed-map.draft.json";
  write(healthy, original["evaluations"].dump());
  write(old_map, R"({"reason":"completed paid map","experiences":[]})");
  const auto report_bytes = read(healthy), map_bytes = read(old_map);
  auto adapter = std::make_shared<LocalModel>(); search.model = adapter;
  auto packet = official_fixture(); packet["phase"] = "pool_diagnostic_map"; packet["pool_task_count"] = 32;
  expect_robust(!search.tree.flag("reflection_exhausted", false), "native scheduler would still stop");
  const auto draft = search.reflect(packet, 0, "-extended-budget");
  expect_robust(adapter->calls == 1 && search.tree.num("model_calls", 0) == 3 &&
      draft["experiences"].items().empty() && search.tree.num("rounds", 0) == 1 &&
      search.tree.num("trials_used", 0) == 32 &&
      search.tree["evaluations"].dump() == original["evaluations"].dump() &&
      search.tree["budget_extensions"].items().size() == 2 &&
      search.tree.str("seed_version") == original.str("seed_version") &&
      search.tree.str("baseline_version") == original.str("baseline_version") &&
      read(healthy) == report_bytes && read(old_map) == map_bytes,
      "continued real Search reflection reset paid work, parent, L0 or a completed map");
  fs::remove_all(directory);
  std::cout << "extended_reflection_budget real_search_continued=1 paid_calls=2_to_3"
               " same_round_only_legacy_holdout_blocked=1 latest_limit_preserved=1 cache_exact=1\n";
}

void rate_limit_budget_completion_regressions() {
  namespace te = trajectory::evolution;
  struct NoModel final : legacy::ModelAdapter {
    unsigned calls = 0;
    std::string name() const override { return "local-no-model-budget"; }
    std::string model_id() const override { return "local"; }
    legacy::RawModelResponse evaluate(const legacy::ModelRequest &) override {
      ++calls; throw std::runtime_error("spent budget issued a model request");
    }
  };
  for (unsigned mode = 0; mode < 4; ++mode) {
    char directory[] = "/tmp/gepa-rate-budget-XXXXXX";
    expect_robust(::mkdtemp(directory), "cannot create rate budget fixture");
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{directory};
    const fs::path root(directory);
    Json rows = Json::Array{};
    for (const auto *split : {"train", "val", "holdout"}) rows.push(Json{
        {"task_id", std::string("local-") + split}, {"group_id", std::string("local-") + split},
        {"task_input", std::string("Complete the bounded local ") + split + " task"},
        {"expected_behavior", "Complete the task"},
        {"initial_state", Json::Object{}}, {"category", "local"}, {"difficulty", "easy"},
        {"split", split}, {"review_status", "reviewed"}});
    std::string lines; for (const auto &row : rows.items()) lines += row.dump() + "\n";
    write(root / "golden.jsonl", lines);
    write(root / "dataset.json", Json{{"schema_version", 1}, {"name", "local-rate-budget"},
        {"split_seed", "local"}, {"split_percent", Json::Array{34, 33, 33}},
        {"budget", Json{{"max_steps", 12}, {"max_tool_calls", 12}, {"timeout_ms", 1000}}},
        {"sources", Json::Array{Json{{"kind", "golden"}, {"path", "golden.jsonl"}}}}}.dump());
    const auto dataset = te::build_evaluation_dataset(root / "dataset.json");
    te::write_evaluation_dataset(dataset, root / "dataset");
    Search search; search.root = root / "search"; search.dataset_path = root / "dataset";
    search.seed = applicability_seed(); search.baseline = search.seed;
    search.context = "local-budget-checkpoint"; search.evaluation_context = "fixed-local-judge";
    search.candidate_evaluation_context = "fixed-local-execution";
    search.config = Json{{"max_rounds", 3}, {"max_model_calls", 2}, {"max_trials", 100},
        {"timeout_ms", mode == 0 ? 600000 : 1000}, {"cache_evaluations", false},
        {"use_merge", false}, {"final_holdout_include_seed", false},
        {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
    search.evaluation = Json{{"repetitions", 1}, {"parallelism", 1}};
    search.gate = Json::Object{};
    const Json values{{"local-val", Json{{"fitness", 1}, {"quality", 1}, {"strict_pass_rate", 1},
        {"invalid_loops", 0}, {"loop_method", "model_review"}, {"duration_ms", 100}, {"repetitions", 1}}}};
    const auto version = te::agent_version_id(search.seed.dump());
    const auto started = mode == 0 ? now() : now() - 10000;
    const Json healthy{{"closed", Json{{"status", "complete"}, {"split", "train"}}}};
    search.tree = Json{{"schema_version", 2}, {"context", search.context},
        {"status", "error"}, {"error", mode == 2 ? "credential_value_rejected" : "gepa_model_rate_limited"},
        {"started_ms", started}, {"rounds", 1}, {"model_calls", mode == 0 ? 2 : 1}, {"trials_used", 32},
        {"seed_version", version}, {"baseline_version", version}, {"active", 1}, {"batch", nullptr},
        {"evaluations", healthy}, {"baseline_validation", values}, {"patience_accounting_revision", 2},
        {"nodes", Json::Array{Json{{"id", 0}, {"kind", "seed"}, {"parent", nullptr},
            {"parents", Json::Array{}}, {"depth", 0}, {"version", version}, {"status", "accepted"},
            {"validation", values}, {"optimization", Json{{"feasible", true}, {"score", 1}}}},
            Json{{"id", 1}, {"parent", 0}, {"parents", Json::Array{0}}, {"kind", "reflection"},
                {"depth", 1}, {"status", "pending"}, {"components", Json::Array{"experience:experience-a"}},
                {"tasks", Json::Array{"local-train"}}, {"reserved", true}}}}};
    write(search.root / "tree.json", search.tree.dump());
    write(search.root / "versions/0.json", search.seed.dump());
    const auto report_path = search.root / "healthy-report.json", map = search.root / "reflections/1.pool-map-0.draft.json";
    write(report_path, healthy.dump()); write(map, R"({"reason":"retained paid map","experiences":[]})");
    const auto report_bytes = read(report_path), map_bytes = read(map), seed_bytes = read(search.root / "versions/0.json");
    auto model = std::make_shared<NoModel>(); search.model = model;
    search.cancelled = [mode] { return mode == 3; };
    std::string error; Json summary;
    try { summary = search.run().summary; }
    catch (const Error &failure) { error = failure.what(); }
    if (mode < 2) {
      expect_robust(error.empty() && summary.str("status") == "validation_rejected" &&
          summary.str("stop_reason") == (mode == 0 ? "model_budget" : "gepa_search_time_reserved") &&
          search.tree["active"].null() && search.tree["nodes"].items()[1].str("status") == "skipped_budget",
          "known paid rate limit did not finish the same search through its normal terminal");
    } else expect_robust(error == (mode == 2 ? "gepa_time_budget_exhausted" : "gepa_cancelled") &&
        search.tree["active"].integer() == 1, "unknown fault or cancellation was reclassified as a budget terminal");
    expect_robust(model->calls == 0 && search.tree.num("model_calls", 0) == (mode == 0 ? 2 : 1) &&
        search.tree.num("trials_used", 0) == 32 && search.tree.num("started_ms", 0) == started &&
        search.tree["baseline_validation"].dump() == values.dump() &&
        search.tree["evaluations"].dump() == healthy.dump() && read(report_path) == report_bytes &&
        read(map) == map_bytes && read(search.root / "versions/0.json") == seed_bytes,
        "normal budget closure reset paid calls/time, seed, L0, report or completed map");
    auto protected_tree = search.tree; protected_tree["holdout_started"] = true;
    search.tree = protected_tree; const auto protected_bytes = search.tree.dump();
    expect_robust(!search.close_rate_limited_search_budget(true) && search.tree.dump() == protected_bytes,
        "budget closure touched a frozen holdout");
  }
  std::cout << "rate_limit_budget_completion real_Search_run=1 calls_cap_and_deadline_terminal=1"
               " unknown_cancel_holdout_guarded=1 paid_cache_seed_L0_exact=1\n";
}

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--partial-holdout-review") {
      gepa_final_holdout_review_regressions();
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--initial-review-retry") {
      initial_review_retry_context_regressions();
      std::cout << "external_model_calls=0 agent_executions=0\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--rate-limit-budget") {
      rate_limit_budget_completion_regressions();
      std::cout << "external_model_calls=0 agent_executions=0\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--extended-budget") {
      extended_reflection_budget_regressions();
      std::cout << "external_model_calls=0 agent_executions=0\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--multi-loop-budget") {
      multi_loop_budget_regressions();
      std::cout << "external_model_calls=0 agent_executions=0\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--prevention-timing") {
      prevention_timing_regressions();
      std::cout << "external_model_calls=0 agent_executions=0\n";
      return 0;
    }
    global_selection_regressions();
    if (argc == 2 && std::string(argv[1]) == "--global-selection") {
      std::cout << "external_model_calls=0 agent_executions=0\n";
      return 0;
    }
    synthetic_regressions();
    transient_transport_regressions();
    extended_reflection_budget_regressions();
    rate_limit_budget_completion_regressions();
    effective_counter_regressions();
    exact_feedback_cache_regressions();
    selector_directory_regressions();
    reduction_budget_regressions();
    applicability_regressions();
    prevention_timing_regressions();
    multi_loop_budget_regressions();
    bundle_regressions();
    transport_checkpoint_regressions();
    if (argc == 2) {
      const fs::path fixtures = argv[1];
      for (const auto node : {21u, 25u, 27u, 29u}) replay_maps(fixtures, node);
      replay_review(fixtures, 27);
    }
    std::cout << "external_model_calls=0 agent_executions=0\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "reflection robustness failed: " << error.what() << '\n';
    return 1;
  }
}
