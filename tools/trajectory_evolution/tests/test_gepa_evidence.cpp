// test_gepa_evidence 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

// 只替换外部评测边界，实际执行 Search 的缓存选择、拆题和自动恢复代码。
// 桩不运行宿主或远程模型；生产库仍链接原有的任务评测函数。
#define run_task_evaluation run_task_evaluation_test_boundary
#include "../src/gepa/gepa_search.cpp"
#undef run_task_evaluation
#include "agentflow/trajectory/experience.h"
#include "trajectory_evolution/orchestration.h"
#include "../src/evaluation/judge_evidence.h"
#include "../src/model/model_input.h"
#include <iostream>

using namespace trajectory::evolution::integration;

namespace trajectory::evolution {
std::function<dataset::v1::TaskEvaluationReport(
    const std::filesystem::path &, const TaskEvaluationOptions &)> evaluation_test_boundary;

dataset::v1::TaskEvaluationReport run_task_evaluation_test_boundary(
    const std::filesystem::path &, const std::filesystem::path &,
    const std::filesystem::path &output, const TaskEvaluationOptions &options) {
  if (!evaluation_test_boundary)
    throw std::runtime_error("评测边界桩尚未配置");
  return evaluation_test_boundary(output, options);
}
} // 测试桩只影响当前翻译单元。

// 测试辅助函数：expect。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void expect(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

// 与生产 evaluate() 使用同一冻结身份，供断点与缓存回归精确定位目录。
std::string evaluation_cache_key(const Search &search, const Json &settings) {
  return hash("gepa-evaluation-entry-v2\n" + search.evaluation_context + '\n' +
              search.candidate_evaluation_context + '\n' + search.context + '\n' +
              settings.dump());
}

// 测试辅助函数：public_alias_fixture。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
Json public_alias_fixture() {
  namespace te = trajectory::evolution;
  namespace ep = agentflow::trajectory::evaluation;
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1");
  memory.set_revision(1);
  auto *entry = memory.add_entries();
  entry->set_memory_id("private");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *candidate = entry->mutable_candidate();
  candidate->set_schema_version(1);
  candidate->set_candidate_id("private-source");
  candidate->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  candidate->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  candidate->set_target_scope("assistant");
  candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  candidate->set_trigger_pattern("Missing image argument");
  candidate->set_observed_problem("Repeated work without the required image");
  candidate->set_proposed_change("Request the required image before rendering.");
  candidate->set_support_user_count(1);
  auto *evidence = candidate->add_support_evidence();
  evidence->set_evidence_id("private-evidence");
  evidence->mutable_source()->set_user_id("user");
  evidence->mutable_source()->set_dialogue_id("source");
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"},
                                                     {"user_id", "user"},
                                                     {"conversation_id", "conversation"},
                                                     {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
                                                        {"max_policy_entries", 64},
                                                        {"max_advice_items", 3},
                                                        {"max_advice_bytes", 4096},
                                                        {"max_query_bytes", 16384},
                                                        {"max_memory_bytes", 1048576},
                                                        {"max_snapshot_bytes", 262144},
                                                        {"max_match_terms", 128},
                                                        {"min_matched_terms", 2}});
  const auto own = te::compile_shadow_policy(memory, identity, limits);
  expect(own.experiences_size() == 1, "private alias fixture was not compiled");
  te::v1::SharedExperienceStore shared;
  shared.set_application_id("agent");
  shared.set_user_id("user");
  shared.set_revision(1);
  for (const bool duplicate : {true, false}) {
    auto *item = shared.add_experiences();
    item->set_memory_id(duplicate ? "shared-copy" : "shared-independent");
    item->set_target_scope("assistant");
    item->set_trigger_pattern(duplicate ? candidate->trigger_pattern()
                                        : "Final delivery verification");
    item->set_content(duplicate ? candidate->proposed_change()
                                : "Verify delivery before reporting completion.");
    auto *relation = shared.add_relations();
    relation->set_private_memory_id("private");
    relation->set_private_content_revision(own.experiences(0).content_revision());
    relation->set_shared_memory_id(item->memory_id());
    relation->set_shared_content_revision(shared_content_revision(*item));
    relation->set_relation(duplicate ? "duplicate" : "independent");
  }
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits, shared);
  expect(snapshot.experiences_size() == 3 && snapshot.cross_relations_size() == 1,
         "shared alias fixture did not use the real compiler");
  auto options = proto<te::v1::RuntimeOptions>(
      Json{{"requested_level", "L2"},
           {"fallback", "strict"},
           {"host_capabilities", Json::Array{"context.request", "action.filter"}},
           {"allowed_users", Json::Array{"user"}},
           {"allowed_conversations", Json::Array{"conversation"}},
           {"experience_template", "{{experience}}"},
           {"max_context_bytes", 4096}});
  for (const auto &source : snapshot.experiences()) {
    auto *ref = options.add_approved_experiences();
    ref->set_memory_id(source.memory_id());
    ref->set_content_revision(source.content_revision());
  }
  return Json{{"schema_version", 1},
              {"agent", Json{{"revision", "alias-fixture"}}},
              {"user_id", "user"},
              {"level", "L2"},
              {"policy", json(te::compile_runtime_policy(snapshot, options))}};
}

// 测试辅助函数：public_private_alias_revisions。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void public_private_alias_revisions() {
  namespace te = trajectory::evolution;
  auto seed = public_alias_fixture();
  const auto policy = proto<te::v1::RuntimePolicy>(seed["policy"]);
  const auto aliases = canonical_experience_aliases(policy);
  expect(aliases.size() == 1 && aliases.begin()->second == "private" &&
             aliases.begin()->first.second !=
                 policy.snapshot().cross_relations(0).shared_content_revision(),
         "fixture failed to distinguish publication and shadow hashes");
  expect(all_source_experiences(seed).items().size() == 3 &&
             source_experiences(seed).items().size() == 2,
         "a revision-pinned duplicate created an inert second L1 search axis");
  for (int mode = 0; mode < 7; ++mode) {
    auto changed = policy;
    if (mode == 0)
      changed.mutable_snapshot()->mutable_cross_relations(0)->set_shared_content_revision("stale");
    if (mode == 1)
      changed.mutable_snapshot()->mutable_cross_relations(0)->set_private_content_revision("stale");
    if (mode == 2)
      changed.mutable_snapshot()->mutable_cross_relations(0)->set_relation("related");
    if (mode == 3)
      for (auto &item : *changed.mutable_snapshot()->mutable_experiences())
        if (item.memory_id() == "shared-copy")
          item.set_content("Changed public guidance.");
    if (mode == 4)
      for (auto &item : *changed.mutable_snapshot()->mutable_experiences())
        if (item.memory_id() == "shared-copy")
          item.set_target_scope("other");
    if (mode >= 5)
      for (auto &ref : *changed.mutable_options()->mutable_approved_experiences())
        if (ref.memory_id() == (mode == 5 ? "shared-copy" : "private"))
          ref.set_content_revision("unapproved-revision");
    expect(canonical_experience_aliases(changed).empty(),
           "stale, different or unapproved sources were merged");
  }
  Search search;
  search.seed = seed;
  const auto receipt = [&](const std::string &revision) {
    Report report;
    auto *trial = report.add_trials();
    trial->set_variant_id("candidate");
    const Json refs =
        Json::Array{Json{{"memory_id", "shared-copy"}, {"content_revision", revision}}};
    const Json event{
        {"kind", "advice.applied"},
        {"application", Json{{"status", "applied"}}},
        {"attributes", Json{{"receipt", Json{{"boundary", "llm"}, {"experiences", refs}}}}}};
    *trial->mutable_evidence() =
        proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", Json::Array{event}}}}});
    return report;
  };
  expect(search.applied_experiences(receipt(aliases.begin()->first.second)) ==
             std::set<std::string>{"experience:private"},
         "real public receipt was not attributed to its canonical search component");
  expect(!search.applied_experiences(receipt("stale")).count("experience:private"),
         "stale receipt borrowed a private application");
  search.controls = Json{{"enabled", true},
                         {"prompt_file", "unused-in-unit-test"},
                         {"actions", Json{{"render", Json::Array{"image"}}}}};
  const Json draft{{"action_rules", Json::Array{Json{{"memory_id", "shared-copy"},
                                                     {"operation", "render"},
                                                     {"required_arguments", Json::Array{"image"}},
                                                     {"match_mode", "predicate"},
                                                     {"reason", "Require an image"}}}},
                   {"loop_rules", Json::Array{}}};
  const auto controlled =
      apply_control_draft(policy.snapshot(), policy.options(), search.controls, draft);
  search.seed["policy"] = json(te::compile_runtime_policy(policy.snapshot(), controlled));
  const Json selected = Json::Array{"experience:private"};
  const auto ablated =
      search.mutate(search.seed,
                    Json{{"reason", "Test only the injection"},
                         {"experience", Json{{"memory_id", "private"}, {"content", nullptr}}}},
                    selected);
  const auto ablated_policy = proto<te::v1::RuntimePolicy>(ablated["policy"]);
  expect(ablated_policy.options().approved_experiences_size() == 3 &&
             ablated_policy.options().disabled_injections_size() == 2,
         "canonical L1 ablation lost control approval or left its public copy active");
  expect(search.components(ablated)["experience:private"].null() &&
             !search.components(ablated).has("experience:shared-copy") &&
             search.components(ablated).has("experience:shared-independent"),
         "ablation altered unrelated public search axes");
  auto query = proto<te::v1::RuntimeQuery>(
      Json{{"boundary", "actions"},
           {"execution_id", "execution"},
           {"advice_query", Json{{"identity", json(policy.snapshot().identity())},
                                 {"target_scope", "assistant"},
                                 {"query_id", "query"},
                                 {"decision_id", "decision"},
                                 {"task", "Render the image"}}},
           {"actions", Json::Array{Json{{"action_id", "render-one"}, {"operation", "render"}}}}});
  expect(te::PolicyRuntime(ablated_policy).decide(query).actions(0).decision() == "reject",
         "disabling duplicate L1 text disabled a public-sourced action guard");
  const auto content = source_experiences(search.seed).items().front().str("content");
  auto llm = query;
  llm.set_boundary("llm");
  llm.clear_actions();
  llm.mutable_advice_query()->set_task("Missing image argument");
  expect(!te::PolicyRuntime(proto<te::v1::RuntimePolicy>(search.seed["policy"]))
                 .decide(llm)
                 .context()
                 .empty() &&
             te::PolicyRuntime(ablated_policy).decide(llm).context().empty(),
         "canonical ablation did not remove an actually injectable private/public pair");
  const auto narrowed =
      search.mutate(search.seed,
                    Json{{"reason", "Only after the observed operation"},
                         {"experience", Json{{"memory_id", "private"},
                                             {"content", content},
                                             {"observed_operations", Json::Array{"render"}}}}},
                    selected);
  expect(te::PolicyRuntime(proto<te::v1::RuntimePolicy>(narrowed["policy"]))
             .decide(llm)
             .context()
             .empty(),
         "shadowed public advice bypassed the canonical operation condition");
  llm.add_observed_operations("render");
  expect(!te::PolicyRuntime(proto<te::v1::RuntimePolicy>(narrowed["policy"]))
              .decide(llm)
              .context()
              .empty(),
         "canonical operation condition disabled valid injection");
  const auto restored =
      search.mutate(ablated,
                    Json{{"reason", "Restore canonical injection"},
                         {"experience", Json{{"memory_id", "private"}, {"content", content}}}},
                    selected);
  expect(proto<te::v1::RuntimePolicy>(restored["policy"]).options().disabled_injections_size() ==
                 0 &&
             search.components(restored).dump() == search.components(search.seed).dump(),
         "canonical injection could not be restored with public control approval intact");
}

// 测试辅助函数：selected_experience_receipt_attribution。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void selected_experience_receipt_attribution() {
  const auto seed = public_alias_fixture();
  const auto policy = proto<trajectory::evolution::v1::RuntimePolicy>(seed["policy"]);
  std::map<std::string, std::string> revisions;
  for (const auto &source : policy.snapshot().experiences())
    revisions[source.memory_id()] = source.content_revision();
  const Json selected = Json::Array{"experience:private"};
  Json events = Json::Array{};
  for (std::size_t i = 0; i < 12; ++i)
    events.push(Json{
        {"kind", "advice.applied"},
        {"event_id", "receipt-" + std::to_string(i)},
        {"actor_id", "worker"},
        {"execution_id", "branch"},
        {"application", Json{{"status", "applied"},
                             {"policy_id", "policy"},
                             {"query_id", "query"},
                             {"decision_id", "decision"}}},
        {"attributes",
         Json{{"receipt",
               Json{{"boundary", "llm"},
                    {"experiences",
                     Json::Array{Json{{"memory_id", "shared-copy"},
                                      {"content_revision", revisions.at("shared-copy")}}}}}}}}});
  events.push(Json{
      {"kind", "custom"},
      {"event_id", "query-event"},
      {"attributes",
       Json{{"type", "policy.query"},
            {"decision",
             Json{{"policy_id", "policy"},
                  {"query",
                   Json{{"boundary", "llm"},
                        {"advice_query",
                         Json{{"query_id", "query"},
                              {"decision_id", "decision"},
                              {"target_scope", "assistant"},
                              {"task", std::string(700, 'T')},
                              {"state", std::string(1300, 'S')}}},
                        {"observed_operations", Json::Array{"sim_web_search"}}}}}}}}});
  trajectory::evolution::dataset::v1::EvaluationTrial trial;
  const auto evidence = [&](const Json &rows) {
    return proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", "run"}}}, {"events", rows}}}});
  };
  *trial.mutable_evidence() = evidence(events);
  const auto observed = selected_experience_applications(trial, selected, seed);
  expect(observed.str("status") == "observed" && observed.num("count", 0) == 12 &&
             observed["points"].items().size() == 8 && observed.flag("truncated", false),
         "receipt attribution lost its total count or exceeded the bounded point list");
  const auto &point = observed["points"].items().front();
  expect(point.str("run_id") == "run" && point.str("receipt_event_id") == "receipt-0" &&
             point.str("memory_id") == "private" &&
             point.str("content_revision") == revisions.at("private") &&
             point.str("receipt_memory_id") == "shared-copy" &&
             point.str("receipt_content_revision") == revisions.at("shared-copy") &&
             point.str("target_scope") == "assistant" &&
             !point.str("task_excerpt").empty() &&
             point.str("task_excerpt").size() <= 512 + std::string(" [truncated]").size() &&
             !point.str("state_excerpt").empty() &&
             point.str("state_excerpt").size() <= 1024 + std::string(" [truncated]").size() &&
             point["observed_operations"].array() &&
             point["observed_operations"].items().front().text() == "sim_web_search",
         "public receipt lost its actual or canonical source revision");
  for (int mode = 0; mode < 5; ++mode) {
    auto row = events.items().front();
    if (mode == 0)
      row["application"]["status"] = "rejected";
    if (mode == 1)
      row["kind"] = "custom"; // 查询不等于应用。
    if (mode == 2)
      row["attributes"]["receipt"]["experiences"].items()[0]["content_revision"] = "stale";
    if (mode == 3)
      row["attributes"]["receipt"]["experiences"].items()[0] =
          Json{{"memory_id", "shared-independent"},
               {"content_revision", revisions.at("shared-independent")}};
    if (mode == 4)
      row.fields().erase("event_id");
    *trial.mutable_evidence() = evidence(Json::Array{row});
    const auto unused = selected_experience_applications(trial, selected, seed);
    expect(unused.str("status") == "not_observed" && unused.num("count", 1) == 0 &&
               unused["points"].items().empty(),
           "unapplied, stale, unrelated or unlocatable source became selected experience evidence");
  }
  auto stale_alias = seed;
  stale_alias["policy"]["snapshot"]["cross_relations"].items().front()["shared_content_revision"] =
      "stale";
  *trial.mutable_evidence() = evidence(events);
  expect(selected_experience_applications(trial, selected, stale_alias).str("status") ==
             "not_observed",
         "stale duplicate relation borrowed another source's application");
}

// 测试辅助函数：experience_packet_attribution_isolation。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void experience_packet_attribution_isolation() {
  Search search;
  search.seed = public_alias_fixture();
  search.config = Json{{"max_input_bytes", 65536},
                       {"max_feedback_trials", 4},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const auto policy = proto<trajectory::evolution::v1::RuntimePolicy>(search.seed["policy"]);
  std::map<std::string, std::string> revisions;
  for (const auto &source : policy.snapshot().experiences())
    revisions[source.memory_id()] = source.content_revision();
  Report report;
  for (const auto *id : {"used", "unrelated"}) {
    auto *task = search.dataset.add_tasks();
    task->set_task_id(id);
    task->set_split("train");
    task->set_task_input("Complete the requested work and verify its artifact.");
    task->set_expected_behavior("Produce the requested file; report failures honestly.");
    auto *trial = report.add_trials();
    trial->set_task_id(id);
    trial->set_trial_id(id);
    trial->set_variant_id("candidate");
    trial->set_execution_status("completed");
    trial->set_verdict(std::string(id) == "used" ? "passed" : "failed");
    const std::string memory = std::string(id) == "used" ? "private" : "shared-independent";
    const Json refs =
        Json::Array{Json{{"memory_id", memory}, {"content_revision", revisions.at(memory)}}};
    Json events = Json::Array{
        Json{{"kind", "custom"},
             {"step_id", "model"},
             {"event_id", "query"},
             {"attributes", Json{{"type", "policy.query"},
                                 {"decision", Json{{"query", Json{{"boundary", "llm"}}},
                                                   {"experiences", refs}}}}}},
        Json{{"kind", "advice.applied"},
             {"step_id", "model"},
             {"event_id", "receipt"},
             {"application", Json{{"status", "applied"}}},
             {"attributes", Json{{"receipt", Json{{"boundary", "llm"}, {"experiences", refs}}}}}}};
    for (std::size_t i = 0; i < 8; ++i)
      events.push(
          Json{{"kind", "action.end"},
               {"event_id", "action-" + std::to_string(i)},
               {"step_id", "model"},
               {"operation", "model_call"},
               {"attributes", Json{{"semantic", "model"}}},
               {"output", "original-evidence-" + std::string(id) + std::string(2000, 'x')}});
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace",
              Json{{"identity", Json{{"run_id", std::string("run-") + id}}}, {"events", events}}}});
  }
  const Json selected = Json::Array{"experience:private"};
  const auto packet = search.packet(search.seed, report, selected);
  expect(packet["feedback"].items().size() == 2,
         "counterexample or applied training evidence was dropped");
  const auto &used = packet["feedback"].items()[0];
  const auto &unused = packet["feedback"].items()[1];
  expect(used.str("task_id") == "used" &&
             used["selected_experience_applications"].num("count", 0) == 1 &&
             used["trial"]["evidence"]["runs"].array(),
         "actual selected application lost its execution feedback");
  expect(unused.str("task_id") == "unrelated" &&
             unused["selected_experience_applications"].str("status") == "not_observed" &&
             unused.str("evidence_role") == "unapplied_counterexample" &&
             unused["trial"]["evidence"].null() && unused.dump().size() < 4096 &&
             unused["trial"].str("verdict") == "failed",
         "an unrelated failed trajectory was presented as selected experience evidence");

  // 全部未命中和停用后恢复都需要原反馈；不能因缺少应用回执关闭这两个搜索方向。
  Report unobserved;
  *unobserved.add_trials() = report.trials(1);
  auto ablated =
      search.mutate(search.seed,
                    Json{{"reason", "Test recovery"},
                         {"experience", Json{{"memory_id", "private"}, {"content", nullptr}}}},
                    selected);
  for (const auto &parent : {search.seed, ablated}) {
    const auto retry = search.packet(parent, unobserved, selected);
    const auto &row = retry["feedback"].items().front();
    expect(row["selected_experience_applications"].str("status") == "not_observed" &&
               !row.has("evidence_role") && row["trial"]["evidence"]["runs"].array() &&
               row.dump().find("original-evidence-unrelated") != std::string::npos,
           "missing or disabled experience lost feedback needed to improve retrieval or restore "
           "injection");
  }

  // 多条失败不能挤掉同一经验真实应用过的成功对照；成功只代表任务完成，不宣称因果收益。
  Report balanced;
  auto copy_trial = [&](const auto &source, const char *id, const char *verdict) {
    auto *task = search.dataset.add_tasks();
    task->set_task_id(id);
    task->set_split("train");
    task->set_task_input("Complete the requested work and verify its artifact.");
    task->set_expected_behavior("Produce the requested file; report failures honestly.");
    auto copy = source;
    copy.set_task_id(id);
    copy.set_trial_id(id);
    copy.set_verdict(verdict);
    return copy;
  };
  for (const auto *id : {"failed-one", "failed-two", "failed-three", "failed-four"})
    *balanced.add_trials() = copy_trial(report.trials(0), id, "failed");
  const auto failures_only = balanced;
  *balanced.add_trials() = copy_trial(report.trials(0), "applied-success", "passed");
  const auto original = balanced.SerializeAsString();
  auto paired = search.packet(search.seed, balanced, selected);
  expect(paired["feedback"].items().size() == 4 &&
             paired["feedback"].items()[0].str("task_id") == "failed-one" &&
             paired["feedback"].items()[1].str("task_id") == "applied-success" &&
             paired["feedback"].items()[1]["selected_experience_applications"].num("count", 0) == 1,
         "same-experience applied success was displaced by multiple failed trials");
  Search mixed = search;
  mixed.controls = Json{{"enabled", true}, {"prompt_file", "fixture.txt"}};
  validate_control_learning(mixed.controls);
  const auto joint = mixed.packet(
      mixed.seed, balanced, Json::Array{"experience:private", "action_rules"});
  std::set<std::string> applied_joint_tasks;
  bool joint_success = false, joint_failure = false;
  for (const auto &row : joint["feedback"].items())
    if (row["selected_experience_applications"].num("count", 0)) {
      applied_joint_tasks.insert(row.str("task_id"));
      joint_success |= row["trial"].str("verdict") == "passed";
      joint_failure |= row["trial"].str("verdict") == "failed";
    }
  expect(applied_joint_tasks.size() >= 2 && joint_success && joint_failure &&
             joint.num("experience_application_task_count", 0) >= 2,
         "joint L1/control packet hid actual application points or its success contrast");
  expect(balanced.SerializeAsString() == original, "balanced sampling rewrote original evidence");
  const Json multiple = Json::Array{"experience:private", "experience:shared-independent"};
  paired = search.packet(search.seed, balanced, multiple);
  expect(paired["feedback"].items()[1].str("task_id") == "applied-success",
         "multi-component selection lost its same-memory positive counterpart");

  // 即使另一经验也获选，它的成功也不能被拿来解释当前经验的失败。
  auto unrelated_success = failures_only;
  *unrelated_success.add_trials() = copy_trial(report.trials(1), "other-success", "passed");
  for (const auto &selection : {selected, multiple}) {
    const auto isolated = search.packet(search.seed, unrelated_success, selection);
    expect(isolated["feedback"].items()[1].str("task_id") == "failed-two",
           "success of a different memory was promoted as the failed memory's counterpart");
  }
  expect(search.packet(search.seed, failures_only, selected)["feedback"].items()[1].str("task_id") ==
             "failed-two", "a missing successful observation changed failure-only sampling");

  // 样本数和字节上限保持原值：只容一题时保留最优先失败，不因补对照而报预算错误。
  search.config["max_feedback_trials"] = 1;
  const auto one = search.packet(search.seed, balanced, selected);
  expect(one["feedback"].items().size() == 1 &&
             one["feedback"].items()[0].str("task_id") == "failed-one",
         "positive counterpart exceeded the configured feedback-count limit");
  search.config["max_feedback_trials"] = 4;
  search.config["max_input_bytes"] = reflection_input_bytes(one) + 64;
  const auto bounded = search.packet(search.seed, balanced, selected);
  expect(bounded["feedback"].items().size() == 1 && bounded.flag("feedback_truncated", false) &&
             reflection_input_bytes(bounded) <= search.config.num("max_input_bytes", 0),
         "positive counterpart overflowed the byte budget or silently disappeared");
}

// 应用次数不是独立题数；反思诊断必须只统计真正保留在请求里的题。
void experience_packet_counts_distinct_retained_tasks() {
  Search search;
  search.seed = public_alias_fixture();
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 1},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const auto policy = proto<trajectory::evolution::v1::RuntimePolicy>(search.seed["policy"]);
  std::string revision;
  for (const auto &source : policy.snapshot().experiences())
    if (source.memory_id() == "private") revision = source.content_revision();
  expect(!revision.empty(), "experience fixture has no selected source revision");
  Report report;
  for (unsigned task_index = 0; task_index < 3; ++task_index) {
    const auto id = "applied-task-" + std::to_string(task_index);
    auto *task = search.dataset.add_tasks();
    task->set_task_id(id);
    task->set_split("train");
    task->set_task_input("Check the required image and report the result.");
    task->set_expected_behavior("Deliver the result without repeated work.");
    auto *trial = report.add_trials();
    trial->set_task_id(id);
    trial->set_trial_id(id);
    trial->set_variant_id("candidate");
    trial->set_execution_status("completed");
    trial->set_verdict("failed");
    Json events = Json::Array{};
    for (unsigned application = 0; application < (task_index == 0 ? 2u : 1u); ++application) {
      const auto suffix = std::to_string(application);
      const Json refs = Json::Array{Json{{"memory_id", "private"},
                                         {"content_revision", revision}}};
      events.push(Json{{"kind", "custom"}, {"step_id", "model-" + suffix},
                       {"event_id", "query-" + suffix},
                       {"attributes", Json{{"type", "policy.query"},
                         {"decision", Json{{"query", Json{{"boundary", "llm"}}},
                                           {"experiences", refs}}}}}});
      events.push(Json{{"kind", "advice.applied"}, {"step_id", "model-" + suffix},
                       {"event_id", "receipt-" + suffix},
                       {"application", Json{{"status", "applied"}}},
                       {"attributes", Json{{"receipt", Json{{"boundary", "llm"},
                                                             {"experiences", refs}}}}}});
    }
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", "run-" + id}}},
                              {"events", events}}}});
  }
  const auto packet = search.packet(search.seed, report, Json::Array{"experience:private"});
  expect(packet["feedback"].items().size() == 1 &&
             packet["feedback"].items()[0].str("task_id") == "applied-task-0" &&
             packet["feedback"].items()[0]["selected_experience_applications"].num("count", 0) == 2,
         "fixture did not retain the two applications of one task");
  expect(packet.num("experience_application_count", 0) == 2 &&
             packet.num("experience_application_task_count", 0) == 1 &&
             !packet.flag("experience_evidence_sufficient", true),
         "repeated applications or discarded tasks were counted as independent evidence");
  search.config["max_feedback_trials"] = 2;
  const auto paired = search.packet(search.seed, report, Json::Array{"experience:private"});
  expect(paired["feedback"].items().size() == 2 &&
             paired.num("experience_application_count", 0) == 3 &&
             paired.num("experience_application_task_count", 0) == 2 &&
             paired.flag("experience_evidence_sufficient", false),
         "two distinct retained tasks did not establish same-memory support");
}

// 测试辅助函数：empty_experience_receipts。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void empty_experience_receipts() {
  Search search;
  search.seed = Json{{"policy", Json{{"snapshot", Json::Object{}}, {"options", Json::Object{}}}}};
  Json receipt{{"kind", "advice.applied"},
               {"application", Json{{"status", "applied"}}},
               {"attributes", Json{{"receipt", Json{{"boundary", "llm"}}}}}};
  for (int mode = 0; mode < 3; ++mode) {
    auto empty = receipt;
    if (mode == 1)
      empty["attributes"]["receipt"]["experiences"] = Json();
    if (mode == 2)
      empty["attributes"]["receipt"]["experiences"] = Json::Array{};
    auto used = receipt;
    used["attributes"]["receipt"]["experiences"] = Json::Array{Json{{"memory_id", "used"}}};
    auto unused = used;
    unused["application"]["status"] = "not_applied";
    unused["attributes"]["receipt"]["experiences"] = Json::Array{Json{{"memory_id", "unused"}}};
    Report report;
    auto *trial = report.add_trials();
    trial->set_variant_id("candidate");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"traces", Json::Array{Json{{"events", Json::Array{empty, unused}}},
                                    Json{{"events", Json::Array{used, empty}}}}}});
    expect(search.applied_experiences(report) == std::set<std::string>{"experience:used"},
           "empty receipt interrupted a later dialogue or counted unused experience");
  }
  // 空值可省略；错误类型仍需被拒绝，不能伪造为一次有效应用。
  receipt["attributes"]["receipt"]["experiences"] = "invalid";
  Report malformed;
  auto *trial = malformed.add_trials();
  trial->set_variant_id("candidate");
  *trial->mutable_evidence() =
      proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", Json::Array{receipt}}}}});
  bool rejected = false;
  try {
    search.applied_experiences(malformed);
  } catch (const Error &error) {
    rejected = std::string(error.what()) == "array_required";
  }
  expect(rejected, "malformed experience receipt silently accepted");
}

// 测试辅助函数：experience_ablation_checkpoint_deduplication。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void experience_ablation_checkpoint_deduplication() {
  const Json selected = Json::Array{"experience:one"};
  Search search;
  search.tree = Json{
      {"nodes", Json::Array{Json{{"version", "same-parent-version"}, {"parent", nullptr}},
                            Json{{"parent", 0},
                                 {"components", selected},
                                 {"generation_method", "experience_ablation"},
                                 {"status", "rejected_minibatch"}},
                            Json{{"version", "same-parent-version"}, {"parent", 0}},
                            Json{{"parent", 2}, {"components", selected}, {"status", "pending"}}}}};
  expect(search.experience_ablation_seen("same-parent-version", selected, 3),
         "duplicate parent nodes allowed repeating the same version's ablation");
  expect(!search.experience_ablation_seen("new-parent-version", selected, 3),
         "an older version blocked a new parent counterfactual");
  expect(!search.experience_ablation_seen("same-parent-version", Json::Array{"experience:two"}, 3),
         "one experience's ablation suppressed a different component");
  expect(!search.experience_ablation_seen("same-parent-version", selected, 1),
         "an in-flight node was mistaken for a prior completed attempt");
  Search resumed;
  resumed.tree = Json::parse(search.tree.dump());
  expect(resumed.experience_ablation_seen(resumed.tree["nodes"].items()[2].str("version"), selected,
                                          3),
         "checkpoint reload forgot a previous ablation attempt");
  resumed.tree["nodes"].items()[1].fields().erase("generation_method");
  expect(!resumed.experience_ablation_seen("same-parent-version", selected, 3),
         "ordinary model mutation was treated as automatic ablation");
}

// 测试辅助函数：observed_training_screening。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void observed_training_screening() {
  Search search;
  const auto score = [](double quality, double fitness, double loops, double duration) {
    return Json{{"quality", quality},
                {"fitness", fitness},
                {"invalid_loops", loops},
                {"loop_method", "reviewed"},
                {"duration_ms", duration}};
  };
  const Json parent{{"changed", score(0, .3, 2, 34)},
                    {"unchanged-a", score(0, 0, 5, 100)},
                    {"unchanged-b", score(0, 0, 5, 100)}};
  const Json candidate{{"changed", score(0, .1, 6, 57)},
                       {"unchanged-a", score(1, 1, 0, 1)},
                       {"unchanged-b", score(1, 1, 0, 1)}};
  const Json changes{{"changed", Json{{"llm", 1}}}};
  expect(gepa::no_worse(candidate, parent),
         "fixture must reproduce a misleading full-batch improvement");
  for (const auto *acceptance : {"strict_improvement", "improvement_or_equal", "pareto"}) {
    search.config = Json{{"acceptance", acceptance}};
    expect(!search.observed_training_gain(candidate, parent, changes),
           "unchanged tasks hid a regression on the only applied policy change");
    const auto before = candidate.dump();
    auto improved = candidate;
    improved["changed"] = score(1, .9, 1, 20);
    expect(search.observed_training_gain(improved, parent, changes),
           "real attributed training improvement was rejected");
    auto neutral = candidate;
    neutral["changed"] = parent["changed"];
    expect(search.observed_training_gain(neutral, parent, changes) ==
               (std::string(acceptance) == "improvement_or_equal"),
           "neutral attribution ignored the configured acceptance mode");
    expect(!search.observed_training_gain(candidate, parent, Json{{"changed", Json{{"llm", 0}}}}),
           "zero application count was treated as a changed trial");
    expect(!search.observed_training_gain(candidate, parent, Json()),
           "missing changes were interpreted as an improvement");
    expect(
        !search.observed_training_gain(candidate, parent, Json{{"missing", Json{{"actions", 1}}}}),
        "unscored applied change authorized training acceptance");
    expect(candidate.dump() == before, "attribution filtering mutated original scores");
  }
  // Pareto 仍可保留真实局部受益的专长分支；不强迫每道受影响题都改善。
  const Json specialist_parent{{"a", score(0, .2, 1, 10)}, {"b", score(1, .9, 1, 10)}};
  const Json specialist{{"a", score(1, .8, 1, 10)}, {"b", score(0, .1, 1, 10)}};
  const Json specialist_changes{{"a", Json{{"actions", 1}}}, {"b", Json{{"loop", 1}}}};
  expect(!gepa::no_worse(specialist, specialist_parent), "fixture must require local Pareto gain");
  search.config = Json{{"acceptance", "pareto"}};
  expect(search.observed_training_gain(specialist, specialist_parent, specialist_changes),
         "an actually improved specialist was removed from Pareto exploration");
  search.config["acceptance"] = "improvement_or_equal";
  expect(!search.observed_training_gain(specialist, specialist_parent, specialist_changes),
         "Pareto exception leaked into nonregression acceptance");

  // 22 题 pool 中少数二值质量回退被其他同题收益抵消时，交给独立 Val 复核；
  // 大范围回退、关键目标新失败和没有真实命中收益仍不得消耗 Val。
  Json tradeoff_parent = Json::Object{}, tradeoff = Json::Object{}, tradeoff_changes = Json::Object{};
  for (int i = 0; i < 22; ++i) {
    const auto id = std::to_string(i);
    const double original = i == 4 || i == 5 ? 1.0 : i == 6 ? .6 :
                            i < 4 ? 0.0 : .6;
    tradeoff_parent[id] = score(original, original, 3, 100);
    tradeoff[id] = score(original, original, 2.7, 90);
    if (i < 14)
      tradeoff_changes[id] = Json{{"llm", 1}};
  }
  tradeoff["0"]["quality"] = 1;
  tradeoff["1"]["quality"] = 1;
  tradeoff["2"]["quality"] = .5;
  tradeoff["3"]["quality"] = .05;
  tradeoff["4"]["quality"] = 0;
  tradeoff["5"]["quality"] = 0;
  tradeoff["6"]["quality"] = 0;
  search.config["acceptance"] = "pareto";
  expect(search.screening_quality_safe(tradeoff, tradeoff_parent, tradeoff_changes) &&
             search.observed_training_gain(tradeoff, tradeoff_parent, tradeoff_changes),
         "small measured pool tradeoff with attributed gains was barred from validation");
  auto widespread = tradeoff;
  widespread["7"]["quality"] = 0;
  widespread["8"]["quality"] = 0;
  expect(!search.screening_quality_safe(widespread, tradeoff_parent, tradeoff_changes),
         "one-third affected-task quality regressions entered validation");
  auto critical = tradeoff;
  critical["7"]["critical_failures"] = 1;
  expect(!search.screening_quality_safe(critical, tradeoff_parent, tradeoff_changes),
         "new critical goal failure entered validation");
  auto severe = tradeoff;
  for (int i = 7; i < 22; ++i)
    severe[std::to_string(i)]["quality"] = 0;
  expect(!search.screening_quality_safe(severe, tradeoff_parent, tradeoff_changes),
         "large mean quality regression entered validation");
  expect(!search.observed_training_gain(tradeoff, tradeoff_parent, Json::Object{}),
         "untriggered change entered validation");
  Json partial_parent = Json::Object{}, partial_candidate = Json::Object{},
       partial_changes = Json::Object{};
  for (int i = 0; i < 22; ++i) {
    const auto id = std::to_string(i);
    partial_parent[id] = score(.6, .6, 3, 100);
    partial_candidate[id] = score(.65, .65, 2.7, 83);
    partial_parent[id]["tokens"] = 100;
    partial_candidate[id]["tokens"] = 88;
    if (i < 6)
      partial_changes[id] = Json{{"loop", 1}};
  }
  const Json partial_coverage{{"rules", Json{{"applied-a", Json{{"applied", 4}}},
                                               {"applied-b", Json{{"applied", 2}}},
                                               {"missed-c", Json{{"applied", 0}}}}}};
  const Json empty_inherited{{"rules", Json::Object{}}};
  expect(new_control_observation(partial_coverage, empty_inherited).flag("observed", false) &&
             search.screening_quality_safe(partial_candidate, partial_parent, partial_changes) &&
             search.observed_training_gain(partial_candidate, partial_parent, partial_changes),
         "partially applied proposal with causal gain was barred before independent validation");
  auto sparse_candidate = partial_candidate;
  Json sparse_changes = Json::Object{};
  for (int i = 0; i < 3; ++i)
    sparse_changes[std::to_string(i)] = Json{{"actions", 1}};
  sparse_candidate["0"]["quality"] = .5;
  sparse_candidate["0"]["fitness"] = .5;
  for (int i = 1; i < 3; ++i) {
    sparse_candidate[std::to_string(i)]["quality"] = .6;
    sparse_candidate[std::to_string(i)]["fitness"] = .6;
  }
  expect(search.screening_quality_safe(sparse_candidate, partial_parent, sparse_changes) &&
             !search.observed_training_gain(sparse_candidate, partial_parent, sparse_changes) &&
             search.screening_gain_for_validation(sparse_candidate, partial_parent,
                                                  sparse_changes),
         "three affected tasks forced a premature attribution verdict before validation");

  // 旧 screened 检查点先用原训练评分重验，不加载候选、调用模型或重新跑验证题。
  char directory[] = "/tmp/trajectory-gepa-screening-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create screening fixture");

  struct Cleanup {
    fs::path path;

    // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
    ~Cleanup() {
      fs::remove_all(path);
    }
  } cleanup{directory};

  search.root = directory;
  search.config["acceptance"] = "pareto";
  const Json screening{{"parent", parent}, {"candidate", candidate}, {"applied_changes", changes}};
  search.tree =
      Json{{"model_calls", 4},
           {"trials_used", 12},
           {"nodes", Json::Array{Json{{"id", 0}, {"parent", nullptr}, {"status", "accepted"}},
                                 Json{{"id", 1},
                                      {"parent", 0},
                                      {"kind", "reflection"},
                                      {"status", "screened"},
                                      {"screening", screening}}}}};
  search.validate(1);
  const auto &node = static_cast<const Json &>(search.tree)["nodes"].items()[1];
  expect(node.str("status") == "rejected_minibatch" &&
             node.str("reason") == "no_observed_training_gain",
         "resuming a harmful screened node continued into validation");
  expect(!node.has("validation") && !node.has("validation_probe") && !node.has("error"),
         "rejected training attribution loaded an artifact or evaluated validation");
  expect(node["screening"].dump() == screening.dump(),
         "resume rewrote the measured training scores");
  expect(search.tree.num("model_calls", 0) == 4 && search.tree.num("trials_used", 0) == 12,
         "resume attribution check spent model or execution budget");
  const auto stopped = search.tree.dump();
  search.validate(1);
  expect(search.tree.dump() == stopped, "resume attribution rejection is not idempotent");
}

// 测试辅助函数：compact_experience_feedback_budget。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void compact_experience_feedback_budget() {
  Json events = Json::Array{};
  auto attributes = [](const std::string &semantic, const std::string &parent = "main") {
    return Json{{"semantic", semantic},
                {"executed", true},
                {"extensions", Json{{"point_reference", Json{{"parent", parent}}}}}};
  };
  auto tool = [&](const std::string &id, const std::string &parent = "main") {
    events.push(Json{{"event_id", id},
                     {"kind", "action.end"},
                     {"step_id", id},
                     {"actor_id", "worker"},
                     {"execution_id", "branch"},
                     {"operation", "fetch"},
                     {"status", "completed"},
                     {"output", "tool-evidence-" + id},
                     {"attributes", attributes("action", parent)}});
  };
  auto model = [&](const std::string &id, const std::string &memory, bool concurrent = false) {
    Json refs =
        Json::Array{Json{{"memory_id", memory}, {"content_revision", "revision-" + memory}}};
    Json query{{"boundary", "llm"},
               {"observed_operations", Json::Array{"fetch"}},
               {"advice_query", Json{{"target_scope", "assistant"},
                                     {"state", "query-state-" + id + std::string(1200, 's')}}}};
    events.push(
        Json{{"event_id", id + "-query"},
             {"kind", "custom"},
             {"step_id", id},
             {"attributes", Json{{"type", "policy.query"},
                                 {"decision", Json{{"query", query}, {"experiences", refs}}}}}});
    if (concurrent)
      tool("concurrent-result");
    events.push(Json{{"event_id", id},
                     {"kind", "action.end"},
                     {"step_id", id},
                     {"actor_id", "worker"},
                     {"execution_id", "branch"},
                     {"operation", "model_call"},
                     {"status", "completed"},
                     {"output", "model-output-" + id + std::string(2048, 'x')},
                     {"attributes", attributes("model")}});
    events.push(
        Json{{"event_id", id + "-receipt"},
             {"kind", "advice.applied"},
             {"step_id", id},
             {"application", Json{{"status", "applied"}}},
             {"attributes", Json{{"receipt", Json{{"boundary", "llm"}, {"experiences", refs}}}}}});
  };
  for (unsigned i = 0; i < 12; ++i)
    model("early-" + std::to_string(i), "private");
  tool("observed-before");
  tool("sibling-before", "sibling");
  model("late-application", "private", true);
  tool("sibling-after", "sibling");
  tool("following-action");
  Json traces = Json::Array{Json{{"identity", Json{{"run_id", "first"}}}, {"events", events}}};
  events = Json::Array{};
  model("shared-application", "shared");
  traces.push(Json{{"identity", Json{{"run_id", "second"}}}, {"events", events}});
  trajectory::evolution::dataset::v1::EvaluationTrial trial;
  trial.set_verdict("passed");
  trial.set_execution_status("completed");
  *trial.mutable_evidence() =
      proto<google::protobuf::Struct>(Json{{"traces", traces}, {"output", "final"}});
  const Json selected = Json::Array{"experience:private", "experience:shared"};
  const auto full = feedback_trial(trial, Json::Object{});
  expect(full.dump().size() > 8192, "L1 fixture did not exceed the feedback budget");
  for (const std::size_t budget : {std::size_t(4096), std::size_t(8192)}) {
    const auto compact = compact_experience_feedback_trial(trial, full, selected, budget);
    expect(compact.dump().size() <= budget,
           "compact L1 feedback exceeded its remaining byte budget");
    expect(compact["evidence"]["runs"].items().size() == 2,
           "private/public applications lost a run");
    bool late = false, shared = false;
    for (const auto &run : compact["evidence"]["runs"].items())
      for (const auto &action : run["actions"].items()) {
        const auto id = action.str("event_id");
        expect(id != "sibling-before" && id != "sibling-after" && id != "concurrent-result",
               "unobserved or independent result was attributed to an L1 request");
        if (id == "late-application" || id == "shared-application") {
          late |= id == "late-application";
          shared |= id == "shared-application";
          expect(action.str("actor_id") == "worker" && action.str("execution_id") == "branch",
                 "L1 application lost its execution identity");
          const auto &q = action["experience_query"];
          expect(q.str("application_status") == "applied" &&
                     q.str("receipt_event_id") == id + "-receipt" &&
                     q.str("state_excerpt").find("query-state-" + id) == 0 &&
                     q["observed_operations"].items().front().text() == "fetch",
                 "L1 applicability evidence disappeared");
        }
        if (action.str("semantic") == "action")
          expect(action.flag("temporal_context_only", false),
                 "nearby action was presented as causal evidence");
      }
    expect(late && shared, "early normal applications crowded out late selected evidence");
  }
  const auto tiny = compact_experience_feedback_trial(trial, full, selected, 256);
  expect(tiny.dump().size() <= 256 && tiny.str("verdict") == "passed",
         "tiny budget lost the legacy summary fallback");
  auto alias = public_alias_fixture();
  const auto refs = proto<trajectory::evolution::v1::RuntimePolicy>(alias["policy"])
                        .options()
                        .approved_experiences();
  for (auto &trace : traces.items())
    for (auto &event : trace["events"].items()) {
      auto &attrs = event["attributes"];
      for (auto *sources : {&attrs["decision"]["experiences"], &attrs["receipt"]["experiences"]}) {
        if (!sources->array())
          continue;
        for (auto &ref : sources->items())
          for (const auto &approved : refs)
            if (approved.memory_id() ==
                (ref.str("memory_id") == "shared" ? "shared-copy" : ref.str("memory_id")))
              ref = json(approved);
      }
    }
  *trial.mutable_evidence() =
      proto<google::protobuf::Struct>(Json{{"traces", traces}, {"output", "final"}});
  const auto aliased_full = feedback_trial(trial, Json::Object{});
  const auto canonical = compact_experience_feedback_trial(
      trial, aliased_full, Json::Array{"experience:private"}, 8192, alias);
  expect(canonical["evidence"]["runs"].items().size() == 2,
         "approved public receipt lost its private component");
  alias["policy"]["snapshot"]["cross_relations"].items().front()["shared_content_revision"] =
      "stale-revision";
  const auto stale = compact_experience_feedback_trial(
      trial, aliased_full, Json::Array{"experience:private"}, 8192, alias);
  expect(stale["evidence"]["runs"].items().size() == 1,
         "stale public revision borrowed a private application");
  for (auto &trace : traces.items()) {
    auto &rows = trace["events"].items();
    rows.erase(
        std::remove_if(rows.begin(), rows.end(),
                       [](const auto &event) { return event.str("kind") == "advice.applied"; }),
        rows.end());
  }
  *trial.mutable_evidence() = proto<google::protobuf::Struct>(Json{{"traces", traces}});
  auto compact = compact_experience_feedback_trial(trial, feedback_trial(trial, Json::Object{}),
                                                   selected, 4096);
  expect(compact["evidence"]["runs"].null(), "a proposed query was promoted to actual application");
  trial.clear_evidence();
  compact = compact_experience_feedback_trial(trial, feedback_trial(trial, Json::Object{}),
                                              selected, 4096);
  expect(compact.str("verdict") == "passed" && compact["evidence"]["runs"].null(),
         "missing trace fabricated evidence");

  // 异步 ACK 不含最终结果；真实结果只可从本次批次的显式因果链取回。
  events = Json::Array{};
  auto scheduled = [&](const std::string &id, const std::string &step = "batch-step",
                       bool executed = true) {
    return Json{{"event_id", id},
                {"kind", "action.end"},
                {"step_id", step},
                {"actor_id", "worker"},
                {"execution_id", "branch"},
                {"operation", "render"},
                {"status", "completed"},
                {"output", Json{{"status", "succeeded"},
                                {"files", Json::Array{"output.bin"}},
                                {"report", std::string(360, 'x') + "ACTUAL_RESULT"}}},
                {"attributes", Json{{"type", "schedule.action"}, {"executed", executed}}}};
  };
  events.push(scheduled("settled-result"));
  auto failure = scheduled("failed-result");
  failure["status"] = "failed";
  failure["output"] = Json{{"status", "failed"}, {"error", "STRUCTURED_FAILURE"}};
  events.push(failure);
  events.push(scheduled("other-batch-result", "other-step"));
  events.push(scheduled("unexecuted-result", "batch-step", false));
  auto cancelled = scheduled("cancelled-result");
  cancelled["status"] = "cancelled";
  events.push(cancelled);
  auto batch =
      Json{{"event_id", "batch-end"},
           {"kind", "step.end"},
           {"step_id", "batch-step"},
           {"actor_id", "worker"},
           {"execution_id", "branch"},
           {"status", "completed"},
           {"caused_by", Json::Array{"settled-result", "failed-result", "other-batch-result",
                                     "unexecuted-result", "cancelled-result", "future-result"}},
           {"attributes", Json{{"extensions", Json{{"point_reference", Json{{"key", "batch"},
                                                                            {"parent", "dispatch"},
                                                                            {"run", "run"}}}}}}}};
  events.push(batch);
  tool("dispatch-ack");
  auto &ack = events.items().back();
  ack["output"] = "accepted";
  ack["caused_by"] = Json::Array{"batch-end"};
  ack["attributes"]["delegates_schedule"] = true;
  ack["attributes"]["extensions"]["point_reference"]["key"] = "dispatch";
  ack["attributes"]["extensions"]["point_reference"]["run"] = "run";
  model("async-model", "private");
  events.push(scheduled("future-result"));
  auto async_trial = [&](const Json &rows) {
    auto observed = trial;
    *observed.mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", "async"}}}, {"events", rows}}}});
    return compact_experience_feedback_trial(observed, feedback_trial(observed, Json::Object{}),
                                             selected, 8192);
  };
  compact = async_trial(events);
  std::set<std::string> results;
  for (const auto &action : compact["evidence"]["runs"].items().front()["actions"].items()) {
    if (!action.has("dispatch_result_of"))
      continue;
    results.insert(action.str("event_id"));
    expect(action.str("dispatch_result_of") == "dispatch-ack" &&
               action.str("schedule_end_event_id") == "batch-end",
           "scheduled result lost its explicit dispatch provenance");
    if (action.str("event_id") == "settled-result")
      expect(action["output"]["report"].text().find("ACTUAL_RESULT") != std::string::npos &&
                 action["output"]["files"].items().size() == 1,
             "typed async result was flattened into an unhelpful prefix");
  }
  expect(results == std::set<std::string>{"settled-result", "failed-result"},
         "async evidence included unrelated or unsettled results");
  expect(compact.dump().size() <= 8192, "async results expanded the L1 input budget");
  for (auto &event : events.items())
    if (event.str("event_id") == "batch-end")
      event["attributes"]["extensions"]["point_reference"]["parent"] = "other-dispatch";
  compact = async_trial(events);
  for (const auto &action : compact["evidence"]["runs"].items().front()["actions"].items())
    expect(!action.has("dispatch_result_of"),
           "a sibling batch was attributed through an unrelated causal reference");
}

// 测试辅助函数：unknown_loop_counter。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void unknown_loop_counter() {
  Report report;
  auto *trial = report.add_trials();
  trial->add_checks()->set_status("passed");
  Json query{{"boundary", "loop"}, {"iteration", "1"}, {"pending_actions", true}};
  Json decision{{"query", query}, {"progress", Json{{"decision", "continue"}}}};
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"trace",
            Json{{"events", Json::Array{Json{{"kind", "custom"},
                                             {"step_id", "step"},
                                             {"attributes", Json{{"type", "policy.query"},
                                                                 {"decision", decision}}}},
                                        Json{{"kind", "loop.decision"}, {"step_id", "step"}}}}}}});
  const auto feedback =
      control_feedback_trial(*trial, Json(Json::Array{"loop_rules"}), Json::Object{});
  const auto &control =
      feedback["evidence"]["runs"].items().front()["actions"].items().front()["loop_control"];
  expect(control["observed_no_progress"].null(),
         "unknown loop counter was converted into measured zero");
  expect(control.flag("pending_actions", false), "pending actions disappeared from reflection");
}

// 测试辅助函数：append_reviewed_group。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void append_reviewed_group(Json &trial, Json &raw, const std::string &run_id,
                           const std::string &execution_id, const std::string &loop_id,
                           const std::vector<std::string> &decisions,
                           bool numeric_iteration = false, bool omit_last_loop = false) {
  Json *run = nullptr;
  for (auto &item : trial["evidence"]["runs"].items())
    if (item.str("run_id") == run_id)
      run = &item;
  if (!run) {
    trial["evidence"]["runs"].push(Json{{"run_id", run_id}, {"actions", Json::Array{}}});
    run = &trial["evidence"]["runs"].items().back();
  }
  for (std::size_t index = 0; index < decisions.size(); ++index) {
    const auto iteration = index + 1;
    const Json wire_iteration =
        numeric_iteration ? Json(iteration) : Json(std::to_string(iteration));
    Json action{
        {"kind", "action.end"}, {"actor_id", "worker"},        {"execution_id", execution_id},
        {"loop_id", loop_id},   {"iteration", wire_iteration}, {"semantic", "action"},
        {"executed", true}};
    const auto id = iteration_identity(*run, action);
    raw["loop_review"]["iteration_reviews"].push(
        Json{{"iteration_id", id},
             {"decision", decisions[index]},
             {"reason", "reviewed iteration " + std::to_string(iteration)}});
    if (!omit_last_loop || index + 1 != decisions.size())
      (*run)["actions"].push(Json{{"kind", "loop.decision"},
                                  {"actor_id", "worker"},
                                  {"execution_id", execution_id},
                                  {"loop_id", loop_id},
                                  {"iteration", wire_iteration},
                                  {"loop_control", Json{{"target_scope", "assistant"},
                                                        {"iteration", iteration},
                                                        {"pending_actions", false},
                                                        {"proposed_decision", "continue"}}}});
    (*run)["actions"].push(std::move(action));
  }
  raw["loop_review"]["observed_iterations"] =
      raw["loop_review"]["iteration_reviews"].items().size();
}

// 测试辅助函数：reviewed_finalization_cap_evidence。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void reviewed_finalization_cap_evidence() {
  Json trial{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(trial, raw, "run", "execution", "loop", {"keep", "waste", "waste", "keep"});
  for (auto &action : trial["evidence"]["runs"].items().front()["actions"].items())
    if (action["loop_control"].object())
      action["loop_control"]["can_finalize"] = true;
  const Json context{{"execution_id", "execution"},
                     {"loop_id", "loop"},
                     {"iteration", "4"},
                     {"action_id", "answer"}};
  auto start = context;
  start["kind"] = "action.start";
  start["attributes"] = Json{{"semantic", "model"}};
  auto end = context;
  end["kind"] = "action.end";
  end["status"] = "completed";
  end["attributes"] = Json{{"semantic", "model"}, {"executed", true}};
  end["output"] =
      Json{{"choices",
            Json::Array{
                Json{{"finish_reason", "stop"},
                     {"message", Json{{"content", "Final report from the completed results."}}}}}}};
  raw["trace"] =
      Json{{"identity", Json{{"run_id", "run"}}},
           {"events", Json::Array{start, end, Json{{"kind", "run.end"}, {"status", "completed"}}}}};
  auto marked = trial;
  bind_iteration_assessments(marked, raw);
  bool offered = false;
  for (const auto &action : marked["evidence"]["runs"].items().front()["actions"].items()) {
    const auto &control = action["loop_control"];
    if (!control.object())
      continue;
    if (control.num("iteration", 0) == 3)
      offered = control.num("reviewed_waste_after", 99) == 1 &&
                control.flag("require_finalization", false);
    if (control.num("iteration", 0) == 4)
      expect(!control.has("reviewed_waste_after"),
             "final delivery itself was offered as removable waste");
  }
  expect(offered, "a verified answer-only suffix lost its conditional finalization opportunity");
  auto canonical = raw;
  canonical["trace"]["events"].items().back().fields().erase("status");
  canonical["trace"]["events"].items().back()["outcome"] = Json{{"execution_status", "completed"}};
  auto canonical_view = trial;
  bind_iteration_assessments(canonical_view, canonical);
  expect(
      canonical_view["evidence"]["runs"].items().front()["actions"].items()[4]["loop_control"].flag(
          "require_finalization", false),
      "canonical run.end outcome lost real finalization evidence");
  const auto denied = [&](Json evidence, Json view) {
    bind_iteration_assessments(view, evidence);
    for (const auto &action : view["evidence"]["runs"].items().front()["actions"].items())
      if (action["loop_control"].has("reviewed_waste_after"))
        return false;
    return true;
  };
  auto unsupported = trial;
  for (auto &action : unsupported["evidence"]["runs"].items().front()["actions"].items())
    if (action["loop_control"].object())
      action["loop_control"].fields().erase("can_finalize");
  expect(denied(raw, unsupported),
         "an unknown host capability authorized a finalization-dependent cap");
  auto invalid = raw;
  invalid["trace"]["events"].items()[1]["output"]["choices"].items()[0]["message"]["tool_calls"] =
      Json::Array{Json::Object{}};
  expect(denied(invalid, trial), "a tool request was mistaken for final text");
  invalid = raw;
  invalid["trace"]["events"].items()[1]["attributes"]["semantic"] = "action";
  expect(denied(invalid, trial), "required tool work was removed with finalization");
  invalid = raw;
  invalid["trace"]["events"].items().erase(invalid["trace"]["events"].items().begin() + 1);
  expect(denied(invalid, trial), "an unfinished model call authorized truncation");
  invalid = raw;
  invalid["trace"]["events"].items().pop_back();
  expect(denied(invalid, trial), "an unfinished run authorized truncation");
  invalid = raw;
  invalid["trace"]["identity"]["run_id"] = "another-run";
  expect(denied(invalid, trial), "final-answer evidence crossed run boundaries");
  invalid = raw;
  auto tool = start;
  tool["action_id"] = "required-tool";
  tool["attributes"]["semantic"] = "action";
  invalid["trace"]["events"].push(tool);
  expect(denied(invalid, trial), "an extra required action in the final iteration was ignored");
}

// 测试辅助函数：required_iteration_floor_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void required_iteration_floor_tests() {
  auto packet = [](Json view) {
    return Json{
        {"selected_components", Json::Array{"loop_rules"}},
        {"components", Json{{"loop_rules", Json::Array{}}}},
        {"controls",
         Json{{"aggressive_controls", true},
              {"limits", Json{{"min_iterations", 1}}},
              {"loop_primitives",
               Json::Array{Json{{"primitive_id", "bounded_no_progress"}},
                           Json{{"primitive_id", "bounded_iteration_cap"}}}},
              {"base_loop_rules",
               Json::Array{Json{{"target_scope", "assistant"}, {"max_iterations", 12}}}}}},
        {"feedback", Json::Array{Json{{"trial", std::move(view)}}}}};
  };
  auto has_cap = [](const Json &input) {
    const auto prepared = observed_operation_bindings(input, 65536);
    const auto &bindings = prepared["controls"]["editable_bindings"];
    if (bindings.array())
      for (const auto &binding : bindings.items())
        if (binding.str("primitive_id") == "bounded_iteration_cap")
          return true;
    return false;
  };
  Json view{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(view, raw, "run", "required", "needed", {"keep", "keep", "keep", "keep"});
  append_reviewed_group(view, raw, "run", "redundant", "repeat",
                        {"waste", "waste", "waste", "waste"});
  bind_iteration_assessments(view, raw);
  auto input = packet(view);
  expect(!has_cap(input),
         "a fully redundant execution authorized truncating the necessary same-actor instance");
  const auto unavailable = observed_operation_bindings(input, 65536);
  expect(!unavailable["controls"].has("editable_bindings") &&
             unavailable["controls"]["loop_primitives"].items().size() == 1 &&
             unavailable["controls"]["loop_primitives"].items()[0].str("primitive_id") ==
                 "bounded_no_progress",
         "an unavailable iteration cap remained advertised or hid another loop primitive");
  auto hidden = view;
  auto &hidden_rows = hidden["evidence"]["runs"].items()[0]["actions"].items();
  hidden_rows.erase(std::remove_if(hidden_rows.begin(), hidden_rows.end(),
                                   [](const auto &action) {
                                     return action.str("execution_id") == "required" &&
                                            action.str("kind") == "loop.decision" &&
                                            action["loop_control"].num("iteration", 0) == 4;
                                   }),
                    hidden_rows.end());
  bind_iteration_assessments(hidden, raw);
  expect(!has_cap(packet(hidden)),
         "a necessary action without its loop query lost the same-actor keep floor");
  auto &rows = input["feedback"].items()[0]["trial"]["evidence"]["runs"].items()[0]["actions"];
  rows.items().erase(rows.items().begin(), rows.items().begin() + 8);
  expect(rows.items()[0]["loop_control"].num("required_iteration_floor", 0) == 4 && !has_cap(input),
         "display clipping erased the complete same-actor keep floor");

  Json other{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json other_raw{
      {"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(other, other_raw, "another-run", "repeat", "repeat",
                        {"waste", "waste", "waste", "waste"});
  bind_iteration_assessments(other, other_raw);
  input = packet(other);
  merge_loop_requirements(input["controls"]["loop_requirements"],
                          view["evidence"]["loop_requirements"]);
  expect(!has_cap(input),
         "a dropped training feedback row erased another execution's required work");

  Json finish_view{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json finish_raw{
      {"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(finish_view, finish_raw, "run", "required", "needed",
                        {"keep", "keep", "keep"});
  append_reviewed_group(finish_view, finish_raw, "run", "redundant", "repeat",
                        {"keep", "waste", "waste", "keep"});
  for (auto &action : finish_view["evidence"]["runs"].items()[0]["actions"].items())
    if (action["loop_control"].object())
      action["loop_control"]["can_finalize"] = true;
  Json events = Json::Array{};
  for (const auto &ending : {std::make_pair("required", 3), std::make_pair("redundant", 4)}) {
    const std::string execution = ending.first,
                      loop = execution == "required" ? "needed" : "repeat";
    Json context{{"execution_id", execution},
                 {"loop_id", loop},
                 {"iteration", std::to_string(ending.second)},
                 {"action_id", execution + "-answer"}};
    auto start = context;
    start["kind"] = "action.start";
    start["attributes"] = Json{{"semantic", "model"}};
    auto end = context;
    end["kind"] = "action.end";
    end["status"] = "completed";
    end["attributes"] = Json{{"semantic", "model"}, {"executed", true}};
    end["output"] =
        Json{{"choices", Json::Array{Json{{"finish_reason", "stop"},
                                          {"message", Json{{"content", "Observed results."}}}}}}};
    events.push(start);
    events.push(end);
  }
  events.push(Json{{"kind", "run.end"}, {"outcome", Json{{"execution_status", "completed"}}}});
  finish_raw["trace"] = Json{{"identity", Json{{"run_id", "run"}}}, {"events", events}};
  auto marked = finish_view;
  bind_iteration_assessments(marked, finish_raw);
  auto prepared = observed_operation_bindings(packet(marked), 65536);
  const auto &binding = prepared["controls"]["editable_bindings"].items().front();
  expect(binding.flag("require_finalization", false) &&
             binding.num("required_iteration_floor", 0) == 2 &&
             binding["budget"].num("min", 0) == 2 && binding["budget"].num("max", 0) == 2,
         "safe answer-only replacement was blocked by another execution's final answer");
  expect(std::any_of(prepared["controls"]["loop_primitives"].items().begin(),
                     prepared["controls"]["loop_primitives"].items().end(),
                     [](const auto &primitive) {
                       return primitive.str("primitive_id") == "bounded_iteration_cap";
                     }),
         "an executable iteration cap was hidden from reflection");
  auto tool = events.items()[0];
  tool["action_id"] = "required-tool";
  tool["attributes"]["semantic"] = "action";
  finish_raw["trace"]["events"].push(tool);
  marked = finish_view;
  bind_iteration_assessments(marked, finish_raw);
  expect(!has_cap(packet(marked)),
         "finalization exemption removed necessary tool work from the floor");
}

// 测试辅助函数：reviewed_input。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
Json reviewed_input(Json trial, bool aggressive = true) {
  return Json{
      {"selected_components", Json::Array{"loop_rules"}},
      {"components", Json{{"loop_rules", Json::Array{}}}},
      {"controls",
       Json{{"aggressive_controls", aggressive},
            {"limits", Json{{"min_iterations", 1}}},
            {"loop_primitives",
             Json::Array{Json{{"primitive_id", "bounded_iteration_cap"}}}},
            {"base_loop_rules", Json::Array{}}}},
      {"feedback", Json::Array{Json{{"trial", std::move(trial)}}}}};
}

// 测试辅助函数：reviewed_iteration_cap_evidence。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void reviewed_iteration_cap_evidence() {
  Search search;
  search.controls = Json{{"min_iterations", 1}, {"min_no_progress", 1}};
  auto fixture = [&](const std::vector<std::string> &decisions, bool numeric = false,
                     bool omit_last_loop = false) {
    Json trial{{"evidence", Json{{"runs", Json::Array{}}}}};
    Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
    append_reviewed_group(trial, raw, "run", "execution", "loop", decisions, numeric,
                          omit_last_loop);
    bind_iteration_assessments(trial, raw);
    return trial;
  };

  auto interrupted = fixture({"keep", "waste", "keep"});
  expect(!search.control_evidence(reviewed_input(interrupted), "loop_rules"),
         "a middle waste iteration authorized a global iteration cap");
  for (const auto &action : interrupted["evidence"]["runs"].items().front()["actions"].items())
    expect(action.has("iteration_assessment") &&
               !action["loop_control"].has("reviewed_waste_after"),
           "complete review was lost or a non-suffix waste was promoted");

  auto suffix = fixture({"keep", "waste", "waste"}, true);
  expect(search.control_evidence(reviewed_input(suffix), "loop_rules"),
         "a complete reviewed waste suffix did not authorize a cap opportunity");
  bool cap_one = false;
  for (const auto &action : suffix["evidence"]["runs"].items().front()["actions"].items())
    cap_one |= action["loop_control"].has("reviewed_waste_after") &&
               action["loop_control"].num("reviewed_waste_after", 99) == 1;
  expect(cap_one, "reviewed suffix boundary was not preserved");

  // 保守模式只能发现有真实收尾能力的条件上限；不能据此放行普通全局截停。
  expect(!search.control_evidence(reviewed_input(suffix, false), "loop_rules"),
         "保守模式误放行无收尾约束的轮数上限");
  auto finalizable_suffix = suffix;
  for (auto &action : finalizable_suffix["evidence"]["runs"].items()[0]["actions"].items())
    if (action["loop_control"].object()) {
      action["loop_control"]["can_finalize"] = true;
      action["loop_control"]["require_finalization"] = true;
    }
  expect(search.control_evidence(reviewed_input(finalizable_suffix, false), "loop_rules"),
         "保守模式漏掉已评审且可收尾的浪费后缀");
  for (const auto *field : {"can_finalize", "require_finalization"}) {
    auto missing = finalizable_suffix;
    for (auto &action : missing["evidence"]["runs"].items()[0]["actions"].items())
      if (action["loop_control"].object())
        action["loop_control"].fields().erase(field);
    expect(!search.control_evidence(reviewed_input(missing, false), "loop_rules"),
           "缺失宿主收尾能力或强制收尾约束仍生成了保守上限机会");
  }

  auto all_waste = fixture({"waste", "waste", "waste", "waste", "waste", "waste", "waste"});
  expect(search.control_evidence(reviewed_input(all_waste), "loop_rules"),
         "all-waste execution lost its minimum-safe cap opportunity");
  bool cap_zero = false;
  for (const auto &action : all_waste["evidence"]["runs"].items().front()["actions"].items())
    cap_zero |= action["loop_control"].has("reviewed_waste_after") &&
                action["loop_control"].num("reviewed_waste_after", 99) == 0;
  expect(cap_zero, "zero reviewed prefix was confused with missing evidence");
  expect(!search.control_evidence(reviewed_input(fixture({"waste"})), "loop_rules"),
         "one reviewed iteration bypassed the configured minimum cap");

  auto hidden_keep = fixture({"keep", "waste", "keep"}, false, true);
  expect(!search.control_evidence(reviewed_input(hidden_keep), "loop_rules"),
         "an executed keep without a loop query was ignored by suffix review");

  Json isolated{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json isolated_raw{
      {"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(isolated, isolated_raw, "run-a", "execution-a", "loop",
                        {"keep", "waste", "waste"});
  append_reviewed_group(isolated, isolated_raw, "run-a", "execution-b", "loop",
                        {"keep", "waste", "keep"});
  append_reviewed_group(isolated, isolated_raw, "run-b", "execution-a", "loop", {"waste"});
  bind_iteration_assessments(isolated, isolated_raw);
  for (const auto &run : isolated["evidence"]["runs"].items())
    for (const auto &action : run["actions"].items()) {
      if (action.str("kind") != "loop.decision")
        continue;
      const bool suffix =
          run.str("run_id") == "run-a" && action.str("execution_id") == "execution-a";
      const bool single = run.str("run_id") == "run-b";
      expect(action["loop_control"].has("reviewed_waste_after") == (suffix || single) &&
                 (!suffix || action["loop_control"].num("reviewed_waste_after", 99) == 1) &&
                 (!single || action["loop_control"].num("reviewed_waste_after", 99) == 0),
             "reviewed suffix crossed a run or execution boundary");
    }
  Json run_b{
      {"evidence", Json{{"runs", Json::Array{isolated["evidence"]["runs"].items().back()}}}}};
  expect(!search.control_evidence(reviewed_input(run_b), "loop_rules"),
         "a single all-waste iteration bypassed the configured minimum in another run");

  auto mismatch = fixture({"keep", "waste", "waste"});
  Json stale_raw{
      {"loop_review", Json{{"status", "complete"},
                           {"observed_iterations", 1},
                           {"iteration_reviews", Json::Array{Json{{"iteration_id", "unknown"},
                                                                  {"decision", "waste"},
                                                                  {"reason", "stale"}}}}}}};
  Json clean{{"evidence", mismatch["evidence"]}};
  for (auto &action : clean["evidence"]["runs"].items().front()["actions"].items()) {
    action.fields().erase("iteration_assessment");
    if (action["loop_control"].object())
      action["loop_control"].fields().erase("reviewed_waste_after");
  }
  bind_iteration_assessments(clean, stale_raw);
  expect(!search.control_evidence(reviewed_input(clean), "loop_rules"),
         "unknown review hash manufactured an iteration-cap opportunity");
  auto uncertain_raw = stale_raw;
  uncertain_raw["loop_review"]["status"] = "uncertain";
  bind_iteration_assessments(clean, uncertain_raw);
  expect(!search.control_evidence(reviewed_input(clean), "loop_rules"),
         "uncertain review manufactured an iteration-cap opportunity");

  const Json run{{"run_id", "canonical"}};
  Json numeric{{"execution_id", "execution"}, {"loop_id", "loop"}, {"iteration", 2}};
  Json textual = numeric;
  textual["iteration"] = "2";
  expect(iteration_identity(run, numeric) == iteration_identity(run, textual),
         "numeric and decimal-string iterations produced different canonical identities");

  const Json structural{
      {"controls", Json{{"aggressive_controls", false}, {"base_loop_rules", Json::Array{}}}},
      {"feedback",
       Json::Array{Json{
           {"trial", Json{{"evidence",
                           Json{{"runs", Json::Array{Json{
                                             {"run_id", "run"},
                                             {"actions",
                                              Json::Array{Json{
                                                  {"kind", "loop.decision"},
                                                  {"loop_control",
                                                   Json{{"pending_actions", false},
                                                        {"proposed_decision", "continue"},
                                                        {"observed_no_progress", 1}}}}}}}}}}}}}}}}};
  expect(search.control_evidence(structural, "loop_rules"),
         "structural no-progress evidence incorrectly required a model review");
}

// 测试辅助函数：action_feedback_keeps_iteration_assessment。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void action_feedback_keeps_iteration_assessment() {
  Json events = Json::Array{}, reviews = Json::Array{};
  const Json run{{"run_id", "run"}};
  for (std::size_t iteration = 1; iteration <= 3; ++iteration) {
    const auto id = "action-" + std::to_string(iteration);
    Json query{{"boundary", "actions"},
               {"actions",
                Json::Array{
                    Json{{"action_id", id}, {"operation", "tool"}, {"arguments", Json::Object{}}}}},
               {"execution_id", "execution"},
               {"advice_query", Json{{"target_scope", "assistant"}}}};
    events.push(Json{
        {"kind", "custom"},
        {"step_id", id},
        {"attributes",
         Json{{"type", "policy.query"},
              {"decision",
               Json{{"query", query},
                    {"actions", Json::Array{Json{{"action_id", id}, {"decision", "allow"}}}}}}}}});
    Json action{{"event_id", "end-" + id},
                {"kind", "action.end"},
                {"step_id", id},
                {"action_id", id},
                {"actor_id", "worker"},
                {"execution_id", "execution"},
                {"loop_id", "loop"},
                {"iteration", std::to_string(iteration)},
                {"operation", "tool"},
                {"status", "completed"},
                {"attributes", Json{{"semantic", "action"}, {"executed", true}}}};
    events.push(action);
    reviews.push(Json{{"iteration_id", iteration_identity(run, action)},
                      {"decision", iteration == 1 ? "keep" : "waste"},
                      {"reason", std::string(4096, 'r')}});
  }
  trajectory::evolution::dataset::v1::EvaluationTrial trial;
  trial.set_verdict("passed");
  trial.set_execution_status("completed");
  *trial.mutable_evidence() = proto<google::protobuf::Struct>(Json{
      {"trace", Json{{"identity", run}, {"events", events}}},
      {"loop_review",
       Json{{"status", "complete"}, {"observed_iterations", 3}, {"iteration_reviews", reviews}}}});
  const auto compact = control_feedback_trial(trial, Json::Array{"action_rules"}, Json::Object{});
  expect(!compact["evidence"].has("loop_review") && compact.dump().size() <= 8192,
         "action-only feedback retained the full loop review or exceeded its compact budget");
  const auto &actions = compact["evidence"]["runs"].items().front()["actions"].items();
  expect(actions.size() == 3 &&
             std::all_of(actions.begin(), actions.end(),
                         [](const auto &action) {
                           return action["iteration_assessment"].object() &&
                                  action["iteration_assessment"].str("reason").size() <= 524;
                         }),
         "action-only feedback lost the compact semantic iteration assessment");
  expect(actions[0].flag("reviewed_required_action", false) &&
             !actions[1].flag("reviewed_required_action", false) &&
             !actions[2].flag("reviewed_required_action", false),
         "control projection dropped the required action or marked waste as required");
}

// 测试辅助函数：incremental_control_coverage。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void incremental_control_coverage() {
  const Json inherited{{"rules", Json{{"old-schedule", Json{{"queries", 8}, {"applied", 0}}}}}};
  const Json training{
      {"rules", Json{{"old-schedule", Json{{"queries", 3}, {"applied", 0}}},
                     {"new-loop", Json{{"queries", 2}, {"triggered", 1}, {"applied", 1}}}}}};
  auto coverage = merge_control_coverage(training, inherited);
  expect(control_changes_observed(coverage, inherited),
         "unchanged unobserved rule blocks a real new control");
  expect(!coverage.flag("complete", true), "incomplete whole-policy coverage was hidden");
  auto missed = coverage;
  missed["rules"]["new-loop"]["applied"] = 0;
  expect(!control_changes_observed(missed, inherited), "new unobserved rule was accepted");
  // 改写后的规则 ID 不同，旧规则的证据不能替它证明生效。
  auto previous = inherited;
  previous["rules"]["old-schedule"]["applied"] = 9;
  Json changed{{"rules", Json{{"changed-schedule", Json{{"queries", 3}, {"applied", 0}}}}}};
  expect(!control_changes_observed(merge_control_coverage(changed, previous), previous),
         "changed rule borrowed an old rule's receipt");
  Json probe{{"rules", Json{{"old-schedule", Json{{"queries", 4}, {"applied", 0}}},
                            {"new-loop", Json{{"queries", 0}, {"applied", 0}}},
                            {"missed-rule", Json{{"queries", 2}, {"applied", 0}}}}}};
  coverage = merge_control_coverage(probe, coverage);
  expect(coverage["rules"]["new-loop"].num("applied", 0) == 1,
         "validation erased training receipts");
  expect(control_changes_observed(coverage, inherited),
         "an applied new control was blocked by another untriggered rule");
  const auto partial = new_control_observation(coverage, inherited);
  expect(partial["applied_new_rules"].items().size() == 1 &&
             partial["unobserved_new_rules"].items().size() == 1 &&
             partial["unobserved_new_rules"].items().front().text() == "missed-rule" &&
             !coverage.flag("complete", true),
         "partially observed controls lost their missing-rule diagnosis");
  // 筛选和反馈必须只看本轮组件对应的控制边界，不能让继承层的随机变化代为记功。
  const Json mixed_changes{{"task-a", Json{{"llm", 4}, {"schedule", 0}}},
                           {"task-b", Json{{"schedule", 2}, {"loop", 1}}}};
  const auto schedule_changes =
      component_applied_changes(Json::Array{"schedule_rules"}, mixed_changes);
  expect(schedule_changes.fields().size() == 1 &&
             schedule_changes["task-b"].num("schedule", 0) == 2 &&
             !schedule_changes["task-b"].has("loop"),
         "inherited L1/L3 effects were attributed to an L2 proposal");
  // 默认效果优先：旧规则未命中不能压过已验证的收益；严格覆盖仍可显式启用。
  Search search;
  search.tree =
      Json{{"nodes", Json::Array{Json{{"status", "accepted"},
                                      {"optimization", Json{{"passed", false}, {"score", 0}}}},
                                 Json{{"status", "accepted"},
                                      {"optimization", Json{{"passed", true}, {"score", 1}}},
                                      {"control_coverage", coverage}}}}};
  search.tree["nodes"].items()[0]["validation"] =
      Json{{"task", Json{{"quality", 1}, {"fitness", 0.5}}}};
  search.tree["nodes"].items()[1]["parents"] = Json::Array{0};
  search.tree["nodes"].items()[1]["validation"] =
      Json{{"task", Json{{"quality", 1}, {"fitness", 0.8}}}};
  search.tree["nodes"].items()[1]["validation_applied_changes"] =
      Json{{"0", Json{{"task", Json{{"loop", 1}}}}}};
  expect(search.winner() == 1, "unused inherited rule suppressed a better candidate");
  search.gate = Json{{"require_level_coverage", false}};
  expect(search.winner() == 1, "explicit effect mode still requires coverage");
  search.gate["require_level_coverage"] = true;
  expect(search.winner() == 0, "explicit strict coverage was bypassed");
  for (auto &rule : coverage["rules"].fields())
    rule.second["applied"] = 1;
  search.tree["nodes"].items()[1]["control_coverage"] = merge_control_coverage(coverage, Json{});
  expect(search.winner() == 0, "strict level coverage was bypassed by rule coverage");
  search.tree["nodes"].items()[1]["control_coverage"]["level_complete"] = true;
  expect(search.winner() == 1, "fully observed better candidate cannot win");
  search.gate = Json::Object{};
  auto &nodes = search.tree["nodes"].items();
  nodes[0]["control_coverage"] = Json{{"complete", true}, {"level_complete", true}};
  nodes[1]["control_coverage"] = Json{{"complete", false}, {"level_complete", false}};
  nodes[0]["optimization"] =
      Json{{"feasible", false}, {"passed", false}, {"score", 0.45}, {"robustness", 0.2}};
  nodes[1]["optimization"] =
      Json{{"feasible", true}, {"passed", true}, {"score", 1}, {"robustness", 0.4}};
  expect(search.winner() == 1, "covered regression beat a feasible improvement");
  auto best = nodes[1];
  best["optimization"]["robustness"] = 0.67;
  nodes.push_back(best);
  expect(search.winner() == 2, "better measured robustness was ignored");
  nodes[2]["status"] = "unobserved_changes";
  expect(search.winner() == 1, "candidate without a real behavioral change won");
  nodes[1]["status"] = "inconclusive";
  expect(search.winner() == 0, "unscored candidate won on apparent performance");
  // 可选地只读回放已有实验的选优，不请求模型、不修改原始树与评测报告。
  if (const auto *path = std::getenv("GEPA_SELECTION_REPLAY_TREE")) {
    Search replay;
    replay.tree = Json::parse(read(path, 16777216));
    replay.gate = replay.tree["optimization_contract"];
    std::cout << "Recorded tree winner=" << replay.tree.num("winner", 0)
              << " effect-first winner=" << replay.winner() << '\n';
  }
}

// 测试辅助函数：cancelled_search_clock。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void cancelled_search_clock() {
  Search search;
  search.config = Json{{"timeout_ms", 100}};
  search.tree = Json{{"started_ms", 1000},
                     {"finished_ms", 1040},
                     {"status", "cancelled"},
                     {"error", "gepa_cancelled"}};
  search.resume_clock(2040);
  expect(search.deadline() == 2100 && search.tree.num("paused_ms", 0) == 1000,
         "explicit pause consumed the active execution budget");
  expect(search.tree.num("started_ms", 0) == 1000, "original start timestamp changed");
  search.resume_clock(2050);
  expect(search.deadline() == 2100, "same cancellation counted twice");
  search.tree["finished_ms"] = 2070;
  search.resume_clock(5070);
  expect(search.deadline() == 5100, "second pause reset previously consumed budget");
  search.tree["finished_ms"] = 5100;
  search.resume_clock(9000);
  expect(search.deadline() == 5100, "exhausted budget revived on retry");
  search.tree["finished_ms"] = 5080;
  search.tree["status"] = "error";
  search.resume_clock(9000);
  expect(search.deadline() == 5100, "ordinary error silently extended the budget");
  search.tree["status"] = "cancelled";
  search.resume_clock(5070);
  expect(search.deadline() == 5100, "backward clock produced a negative pause");
  search.tree = Json{{"started_ms", now() - 5000}, {"paused_ms", 5000}};
  search.config = Json{{"timeout_ms", 1000}};
  search.check();
  search.tree["paused_ms"] = 0;
  bool expired = false;
  try {
    search.check();
  } catch (const Error &error) {
    expired = std::string(error.what()) == "gepa_time_budget_exhausted";
  }
  expect(expired, "active execution timeout was disabled");
}

// 测试辅助函数：context_version_is_not_behavior。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void context_version_is_not_behavior() {
  namespace te = trajectory::evolution;
  namespace ep = agentflow::trajectory::evaluation;
  agentflow::trajectory::evaluation::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1");
  memory.set_revision(1);
  // 非空模型上下文必须来自真实 L1 经验，不能再借用已删除的 L2 规划提示。
  auto *entry = memory.add_entries();
  entry->set_memory_id("context-memory");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *experience = entry->mutable_candidate();
  experience->set_schema_version(1);
  experience->set_candidate_id("context-source");
  experience->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  experience->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  experience->set_target_scope("assistant");
  experience->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  experience->set_trigger_pattern("prepare render");
  experience->set_observed_problem("Repeated preparation without checking required inputs.");
  experience->set_proposed_change("Verify required inputs before preparing the render.");
  experience->set_support_user_count(1);
  auto *support = experience->add_support_evidence();
  support->set_evidence_id("context-evidence");
  support->mutable_source()->set_user_id("user");
  support->mutable_source()->set_dialogue_id("source");
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"},
                                                     {"user_id", "user"},
                                                     {"conversation_id", "conversation"},
                                                     {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
                                                        {"max_policy_entries", 64},
                                                        {"max_advice_items", 3},
                                                        {"max_advice_bytes", 4096},
                                                        {"max_query_bytes", 16384},
                                                        {"max_memory_bytes", 1048576},
                                                        {"max_snapshot_bytes", 262144},
                                                        {"max_match_terms", 128},
                                                        {"min_matched_terms", 2}});
  auto options = proto<te::v1::RuntimeOptions>(Json{
      {"requested_level", "L2"},
      {"fallback", "strict"},
      {"host_capabilities", Json::Array{"context.request", "action.filter", "schedule.dispatch"}},
      {"allowed_users", Json::Array{"user"}},
      {"allowed_conversations", Json::Array{"conversation"}},
      {"experience_template", "{{experience}}"},
      {"max_context_bytes", 4096},
      {"schedule_rules", Json::Array{Json{{"rule_id", "ordered"},
                                          {"target_scope", "assistant"},
                                          {"before_operation", "prepare"},
                                          {"after_operation", "render"},
                                          {"require_success", true},
                                          {"reason", "wait for preparation"},
                                          {"task_terms", Json::Array{"prepare"}}}}}});
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  expect(snapshot.experiences_size() == 1, "真实 L1 上下文夹具没有生成经验");
  auto *approval = options.add_approved_experiences();
  approval->set_memory_id(snapshot.experiences(0).memory_id());
  approval->set_content_revision(snapshot.experiences(0).content_revision());
  const auto parent = te::compile_runtime_policy(snapshot, options);
  const auto query =
      proto<te::v1::RuntimeQuery>(Json{{"boundary", "llm"},
                                       {"execution_id", "assistant"},
                                       {"advice_query", Json{{"identity", json(identity)},
                                                             {"target_scope", "assistant"},
                                                             {"query_id", "query"},
                                                             {"decision_id", "decision"},
                                                             {"task", "prepare then render"}}}});
  const auto original = json(te::PolicyRuntime(parent).decide(query));
  expect(!original.str("context").empty(), "真实 L1 经验没有注入模型上下文");
  auto context_only_options = options;
  context_only_options.clear_schedule_rules();
  const auto context_only = te::compile_runtime_policy(snapshot, context_only_options);
  expect(te::PolicyRuntime(context_only).decide(query).context() == original.str("context"),
         "L2 编排规则仍然改变了模型上下文");
  auto dispatch = query;
  dispatch.set_boundary("schedule");
  for (const auto *operation : {"prepare", "render"}) {
    auto *action = dispatch.add_actions();
    action->set_action_id(operation);
    action->set_operation(operation);
  }
  const auto scheduled = te::PolicyRuntime(parent).decide(dispatch).schedule();
  const auto independent = te::PolicyRuntime(context_only).decide(dispatch).schedule();
  expect(scheduled.actions_size() == 2 && independent.actions_size() == 2 &&
             scheduled.actions(1).after_size() == 1 &&
             scheduled.actions(1).after(0).action_id() == "prepare" &&
             independent.actions(1).after_size() == 0,
         "L2 规则没有在真实 schedule 边界形成依赖差分");
  options.set_max_context_bytes(8192);
  const auto child = te::compile_runtime_policy(parent.snapshot(), options);
  auto actual = json(te::PolicyRuntime(child).decide(query));
  expect(actual.str("policy_id") != original.str("policy_id"),
         "fixture must have different policy IDs");
  expect(actual.str("context") == original.str("context"),
         "metadata changed the model-visible context");
  Search search;
  Report observed;
  auto changes = [&](const Json &decision, bool executed = true) {
    Report report;
    auto *trial = report.add_trials();
    trial->set_variant_id("candidate");
    trial->set_task_id("task");
    Json events = Json::Array{
        Json{{"kind", "custom"},
             {"step_id", "step"},
             {"attributes", Json{{"type", "policy.query"}, {"decision", decision}}}},
        Json{{"kind", "advice.applied"},
             {"application",
              Json{{"query_id", "query"},
                   {"status", decision.str("status") == "no_change" ? "rejected" : "applied"}}}}};
    if (executed)
      events.push(Json{
          {"kind", "action.end"}, {"step_id", "step"}, {"attributes", Json{{"executed", true}}}});
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}}});
    observed = report;
    return search.applied_changes(Json{{"policy", json(parent)}}, report);
  };
  expect(changes(actual).fields().empty(), "policy ID alone was counted as an applied L1 change");
  // 历史轨迹仍可能带旧编号包装；新版回放也不能把它认作业务改动。
  auto legacy = actual;
  auto legacy_context = legacy.str("context");
  legacy_context.replace(0, std::string("\n<trajectory-experience>\n").size(),
                         "\n<trajectory-experience policy=\"" + actual.str("policy_id") + "\">\n");
  legacy["context"] = legacy_context;
  expect(changes(legacy).fields().empty(), "legacy policy ID counted as a behavior change");
  actual["context"] = actual.str("context") + "new scheduling instruction";
  expect(changes(actual)["task"].num("llm", 0) == 1, "real context edit was discarded");
  actual["context"] = "";
  actual["status"] = "no_change";
  expect(changes(actual)["task"].num("llm", 0) == 1, "executed context removal was discarded");
  expect(changes(actual, false).fields().empty(), "unexecuted context removal was counted");

  // 旧检查点只更新派生归因；报告缺失或哈希不符时不能猜测结果，更不能偷偷重新跑题。
  changes(json(te::PolicyRuntime(child).decide(query)));
  char directory[] = "/tmp/trajectory-gepa-evidence-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create evidence fixture");

  struct Cleanup {
    fs::path path;

    // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
    ~Cleanup() {
      fs::remove_all(path);
    }
  } cleanup{directory};

  search.root = directory;
  search.val = {"task"};
  const Json artifact{{"schema_version", 1},
                      {"agent", Json{{"revision", "fixture"}}},
                      {"user_id", "user"},
                      {"level", "L2"},
                      {"policy", json(parent)}};
  write(search.root / "versions/0.json", artifact.dump());
  const auto report = json(observed).dump();
  const auto report_path = search.root / "evaluations/cached/attempt-1/report.json";
  write(report_path, report);
  const Json values{{"task", Json{{"fitness", 1}, {"quality", 1}}}};
  search.tree = Json{
      {"active", nullptr},
      {"batch", nullptr},
      {"model_calls", 4},
      {"trials_used", 12},
      {"evaluations", Json{{"cached", Json{{"version", "child"},
                                           {"split", "train"},
                                           {"attempt", 1},
                                           {"task_ids", Json::Array{"task"}},
                                           {"report_hash", hash(report)}}}}},
      {"nodes", Json::Array{Json{{"id", 0},
                                 {"parent", nullptr},
                                 {"parents", Json::Array{}},
                                 {"depth", 0},
                                 {"status", "accepted"},
                                 {"version", te::agent_version_id(artifact.dump())},
                                 {"validation", values}},
                            Json{{"id", 1},
                                 {"parent", 0},
                                 {"parents", Json::Array{0}},
                                 {"depth", 1},
                                 {"status", "accepted"},
                                 {"version", "child"},
                                 {"tasks", Json::Array{"task"}},
                                 {"validation", values},
                                 {"screening",
                                  Json{{"applied_changes", Json{{"task", Json{{"llm", 1}}}}}}}}}}};
  search.tree["batch"] = Json::Object{};
  search.refresh_change_evidence();
  expect(!search.tree.has("change_evidence_revision"), "migration changed an active batch");
  search.tree["batch"] = nullptr;
  const auto before = search.tree.dump();
  write(report_path, "{}");
  bool rejected = false;
  try {
    search.refresh_change_evidence();
  } catch (const Error &error) {
    rejected = std::string(error.what()) == "gepa_report_changed";
  }
  expect(rejected && search.tree.dump() == before, "altered report caused a partial migration");
  write(report_path, report);
  search.refresh_change_evidence();
  expect(search.tree["nodes"].items()[1].str("status") == "unobserved_changes",
         "stale acceptance survived migration");
  expect(search.tree["nodes"].items()[1]["validation"].dump() == values.dump(),
         "migration changed measured scores");
  expect(search.tree.num("model_calls", 0) == 4 && search.tree.num("trials_used", 0) == 12,
         "migration spent evaluation budget");
  expect(read(report_path) == report, "migration rewrote raw report");
  // 修订 1 的 screened 节点和反馈同样不可信；升级时必须一起撤回，不能在续跑后入库。
  search.tree["change_evidence_revision"] = 1;
  search.tree["nodes"].items()[1]["status"] = "screened";
  search.tree["memory_feedback"] = Json{{"records", Json{{"stale", Json{{"status", "supported"}}}}}};
  search.refresh_change_evidence();
  expect(search.tree.num("change_evidence_revision", 0) == 2 &&
             search.tree["nodes"].items()[1].str("status") == "unobserved_changes" &&
             search.tree["nodes"].items()[1].str("attribution_previous_status") == "screened" &&
             search.tree["memory_feedback"].null(),
         "legacy screened node or stale feedback survived causal migration");
  const auto migrated = search.tree.dump();
  search.refresh_change_evidence();
  expect(search.tree.dump() == migrated, "migration is not idempotent");
  gepa::validate_nodes(search.tree["nodes"]);
}

// 测试辅助函数：asynchronous_target_replay。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void asynchronous_target_replay() {
  namespace te = trajectory::evolution;
  namespace ep = agentflow::trajectory::evaluation;
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1");
  memory.set_revision(1);
  auto *entry = memory.add_entries();
  entry->set_memory_id("async-experience");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *candidate = entry->mutable_candidate();
  candidate->set_schema_version(1);
  candidate->set_candidate_id("async-candidate");
  candidate->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  candidate->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  candidate->set_target_scope("assistant");
  candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  candidate->set_trigger_pattern("重复生成失败");
  candidate->set_observed_problem("同一目标重复失败");
  candidate->set_proposed_change("停止重复生成，独立分支继续。");
  candidate->set_support_user_count(1);
  auto *support = candidate->add_support_evidence();
  support->set_evidence_id("support");
  support->mutable_source()->set_user_id("user");
  support->mutable_source()->set_dialogue_id("source");
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"},
                                                     {"user_id", "user"},
                                                     {"conversation_id", "conversation"},
                                                     {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
                                                        {"max_policy_entries", 64},
                                                        {"max_advice_items", 3},
                                                        {"max_advice_bytes", 4096},
                                                        {"max_query_bytes", 16384},
                                                        {"max_memory_bytes", 1048576},
                                                        {"max_snapshot_bytes", 262144},
                                                        {"max_match_terms", 128},
                                                        {"min_matched_terms", 2}});
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  expect(snapshot.experiences_size() == 1, "async experience fixture not eligible");
  auto options = proto<te::v1::RuntimeOptions>(Json{
      {"requested_level", "L2"},
      {"fallback", "strict"},
      {"host_capabilities", Json::Array{"context.request", "action.filter", "schedule.dispatch"}},
      {"allowed_users", Json::Array{"user"}},
      {"allowed_conversations", Json::Array{"conversation"}},
      {"experience_template", "{{experience}}"},
      {"max_context_bytes", 4096}});
  auto *approval = options.add_approved_experiences();
  approval->set_memory_id(snapshot.experiences(0).memory_id());
  approval->set_content_revision(snapshot.experiences(0).content_revision());
  const auto base = te::compile_runtime_policy(snapshot, options);
  auto *rule = options.add_action_rules();
  rule->set_rule_id("target-budget");
  rule->set_target_scope("assistant");
  rule->set_operation("generate");
  rule->set_max_failed_attempts(2);
  rule->add_failure_key_arguments("output");
  rule->set_reason("同一目标已失败两次");
  rule->set_match_mode("predicate");
  *rule->mutable_source_experience() = *approval;
  const auto guarded = te::compile_runtime_policy(snapshot, options);
  auto query = proto<te::v1::RuntimeQuery>(
      Json{{"boundary", "actions"},
           {"execution_id", "coordinator"},
           {"advice_query", Json{{"identity", json(identity)},
                                 {"target_scope", "assistant"},
                                 {"task", "generate A"},
                                 {"query_id", "guard-query"},
                                 {"decision_id", "guard-decision"}}},
           {"actions", Json::Array{Json{{"action_id", "guard"},
                                        {"operation", "generate"},
                                        {"arguments", Json{{"output", "A"}}}}}}});
  const auto allowed = json(te::PolicyRuntime(base).decide(query));
  (*query.mutable_actions(0)->mutable_target_failures())["target-budget"] = 2;
  const auto rejected = json(te::PolicyRuntime(guarded).decide(query));
  expect(rejected["actions"].items()[0].str("decision") == "reject",
         "async fixture did not reject");
  auto start = [](const std::string &id) {
    return Json{{"kind", "action.start"},
                {"action_id", id},
                {"operation", "generate"},
                {"input", Json{{"output", "A"}}},
                {"attributes", Json{{"type", "schedule.action"}, {"target_scope", "assistant"}}}};
  };
  auto end = [](const std::string &id) {
    return Json{
        {"kind", "action.end"},
        {"action_id", id},
        {"execution_id", "worker-" + id},
        {"status", "failed"},
        {"attributes",
         Json{{"type", "schedule.action"}, {"executed", true}, {"observed_failure", true}}}};
  };
  auto report = [&](Json events, const Json &actual) {
    events.push(Json{{"kind", "custom"},
                     {"step_id", "guard-step"},
                     {"attributes", Json{{"type", "policy.query"}, {"decision", actual}}}});
    events.push(
        Json{{"kind", "advice.applied"},
             {"application",
              Json{{"query_id", "guard-query"},
                   {"status", actual.str("status") == "no_change" ? "rejected" : "applied"}}}});
    if (actual.str("status") == "no_change")
      events.push(Json{{"kind", "action.end"},
                       {"step_id", "guard-step"},
                       {"attributes", Json{{"executed", true}}}});
    Report result;
    auto *trial = result.add_trials();
    trial->set_variant_id("candidate");
    trial->set_task_id("task");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}}});
    return result;
  };
  Search search;
  const Json parent{{"policy", json(base)}}, child{{"policy", json(guarded)}};
  const Json sequential = Json::Array{start("one"), end("one"), start("two"), end("two")};
  auto feedback_report = report(sequential, allowed);
  feedback_report.mutable_trials(0)->add_checks()->set_status("passed");
  auto feedback_trace = json(feedback_report.trials(0).evidence());
  auto &last = feedback_trace["trace"]["events"].items().back();
  last["action_id"] = query.actions(0).action_id();
  last["operation"] = "generate";
  *feedback_report.mutable_trials(0)->mutable_evidence() =
      proto<google::protobuf::Struct>(feedback_trace);
  const auto feedback = control_feedback_trial(feedback_report.trials(0),
                                               Json(Json::Array{"action_rules"}), Json::Object{});
  const auto &actions = feedback["evidence"]["runs"].items().front()["actions"].items();
  expect(actions.back()["action_control"].num("prior_operation_rounds", 0) == 2,
         "reflection lost settled asynchronous attempts");
  const auto preflight = search.control_preflight(parent, child, report(sequential, allowed));
  expect(preflight.flag("passed", false) && preflight["rules"].num("target-budget", 0) == 1,
         "async failures missing from GEPA preflight");

  // 新 repeat_failed_operation 只能用生成时引用的同题、同 operation/action 预检；
  // 其他任务恰好命中同一 rule_id 不能冒充候选证据。
  auto repeat_options = base.options();
  auto *repeat_rule = repeat_options.add_action_rules();
  repeat_rule->set_rule_id("operation-repeat");
  repeat_rule->set_target_scope("assistant");
  repeat_rule->set_operation("generate");
  repeat_rule->set_max_failed_attempts(2);
  repeat_rule->set_reason("Stop the cited repeated operation failure");
  repeat_rule->set_match_mode("predicate");
  *repeat_rule->mutable_source_experience() = *approval;
  const auto repeat_policy = te::compile_runtime_policy(snapshot, repeat_options);
  auto repeat_query = query;
  repeat_query.mutable_actions(0)->clear_target_failures();
  repeat_query.mutable_actions(0)->set_prior_failures(2);
  *repeat_query.mutable_actions(0)->mutable_arguments() =
      proto<google::protobuf::Struct>(Json{{"action", "render"}, {"output", "A"}});
  const auto repeat_allowed = json(te::PolicyRuntime(base).decide(repeat_query));
  const auto repeat_rejected = json(te::PolicyRuntime(repeat_policy).decide(repeat_query));
  expect(repeat_rejected["actions"].items().front().str("decision") == "reject",
         "repeat provenance fixture did not exercise the candidate rule");
  const Json repeat_provenance = Json::Array{Json{
      {"component", "action_rules"},
      {"primitive_id", "repeat_failed_operation"},
      {"memory_id", "async-experience"},
      {"target_scope", "assistant"},
      {"task_id", "task"},
      {"operation", "generate"},
      {"arguments", Json{{"action", "render"}}},
      {"observed_failure", true},
      {"prior_failures", 2},
      {"evidence_ref", "/feedback/0/trial/evidence/runs/0/actions/2"}}};
  const Json repeat_child{{"policy", json(repeat_policy)}};
  const auto bound_preflight =
      search.control_preflight(parent, repeat_child, report(sequential, repeat_allowed),
                               repeat_provenance);
  expect(bound_preflight.flag("passed", false) &&
             bound_preflight["rules"].num("operation-repeat", 0) == 1 &&
             bound_preflight["provenance"]["operation-repeat"].str("task_id") == "task",
         "same-task repeat provenance was not recorded and accepted");
  auto wrong_task = repeat_provenance;
  wrong_task.items().front()["task_id"] = "another-task";
  expect(!search
              .control_preflight(parent, repeat_child, report(sequential, repeat_allowed),
                                 wrong_task)
              .flag("passed", false),
         "another task's repeat hit satisfied the cited candidate provenance");
  auto wrong_action = repeat_provenance;
  wrong_action.items().front()["arguments"]["action"] = "query";
  expect(!search
              .control_preflight(parent, repeat_child, report(sequential, repeat_allowed),
                                 wrong_action)
              .flag("passed", false),
         "another business action satisfied the cited candidate provenance");
  expect(!search
              .control_preflight(parent, repeat_child, report(sequential, repeat_allowed),
                                 Json(Json::Array{}))
              .flag("passed", false),
         "a new repeat rule without typed provenance passed preflight");
  expect(search.applied_changes(child, report(sequential, rejected)).fields().empty(),
         "identical parent rejection was counted as new effect");
  expect(search.applied_changes(child, report(sequential, allowed))["task"].num("actions", 0) == 1,
         "real removal of the parent restriction was lost");
  const auto removal = search.control_preflight(child, parent, report(sequential, rejected));
  expect(removal.flag("passed", false) && removal["removal_changes"]["task"].num("actions", 0) == 1,
         "deleting an applied asynchronous failure budget was blocked");
  std::vector<Json> incomplete{Json::Array{start("one"), start("two"), end("one"), end("two")},
                               Json::Array{start("one"), end("one"), end("one")}};
  auto success = sequential;
  success.items()[3]["status"] = "completed";
  success.items()[3]["attributes"]["observed_failure"] = false;
  incomplete.push_back(success);
  auto unexecuted = sequential;
  unexecuted.items()[3]["attributes"]["executed"] = false;
  incomplete.push_back(unexecuted);
  auto other = sequential;
  other.items()[2]["input"]["output"] = "B";
  incomplete.push_back(other);
  auto sibling = sequential;
  sibling.items()[2]["attributes"]["target_scope"] = "other-branch";
  incomplete.push_back(sibling);
  auto old = sequential;
  old.items()[0]["attributes"].fields().erase("target_scope");
  old.items()[2]["attributes"].fields().erase("target_scope");
  incomplete.push_back(old);
  for (const auto &events : incomplete)
    expect(!search.control_preflight(parent, child, report(events, allowed)).flag("passed", false),
           "concurrent, duplicate, successful, unexecuted, unrelated or old events invented retry "
           "evidence");
  auto separate_runs = report(sequential, allowed);
  auto evidence = json(separate_runs.trials(0).evidence());
  auto trace = evidence["trace"];
  auto earlier = trace;
  earlier["identity"]["run_id"] = "previous-run";
  earlier["events"] = sequential;
  trace["events"].items().erase(trace["events"].items().begin(),
                                trace["events"].items().begin() + 4);
  *separate_runs.mutable_trials(0)->mutable_evidence() =
      proto<google::protobuf::Struct>(Json{{"traces", Json::Array{earlier, trace}}});
  expect(!search.control_preflight(parent, child, separate_runs).flag("passed", false),
         "another run granted retry evidence");

  // 终态错误只约束发生错误的 execution；同模型的独立分支仍有首次执行机会。
  auto terminal_options = base.options();
  auto *terminal = terminal_options.add_action_rules();
  terminal->set_rule_id("terminal-budget");
  terminal->set_target_scope("assistant");
  terminal->set_operation("generate");
  terminal->set_max_failed_attempts(1);
  terminal->add_terminal_error_codes("UnsupportedModel");
  terminal->add_error_scope_arguments("model");
  terminal->set_match_mode("predicate");
  terminal->set_reason("Stop this execution after a terminal backend error");
  *terminal->mutable_source_experience() = base.options().approved_experiences(0);
  const auto terminal_policy = te::compile_runtime_policy(snapshot, terminal_options);
  const Json terminal_child{{"policy", json(terminal_policy)}};
  auto terminal_query = query;
  terminal_query.set_execution_id("branch-a");
  terminal_query.mutable_advice_query()->set_query_id("first-a-query");
  terminal_query.mutable_advice_query()->set_decision_id("first-a-decision");
  terminal_query.mutable_actions(0)->clear_target_failures();
  terminal_query.mutable_actions(0)->set_action_id("first-a");
  *terminal_query.mutable_actions(0)->mutable_arguments() =
      proto<google::protobuf::Struct>(Json{{"model", "one"}, {"output", "a.png"}});
  const auto first_allowed = json(te::PolicyRuntime(base).decide(terminal_query));
  const Json failure_events =
      Json::Array{Json{{"kind", "custom"},
                       {"step_id", "first-a-step"},
                       {"execution_id", "branch-a"},
                       {"attributes", Json{{"type", "policy.query"}, {"decision", first_allowed}}}},
                  Json{{"kind", "action.end"},
                       {"step_id", "first-a-step"},
                       {"action_id", "first-a"},
                       {"execution_id", "branch-a"},
                       {"operation", "generate"},
                       {"status", "failed"},
                       {"input", json(terminal_query.actions(0).arguments())},
                       {"attributes", Json{{"executed", true},
                                           {"observed_failure", true},
                                           {"observed_error_code", "UnsupportedModel"}}}}};
  auto after_failure = [&](const std::string &branch, bool reject) {
    auto current = terminal_query;
    current.set_execution_id(branch);
    current.mutable_actions(0)->set_action_id("guard");
    current.mutable_advice_query()->set_query_id("guard-query");
    current.mutable_advice_query()->set_decision_id("guard-decision");
    *current.mutable_actions(0)->mutable_arguments() =
        proto<google::protobuf::Struct>(Json{{"model", "one"}, {"output", "b.png"}});
    if (reject)
      (*current.mutable_actions(0)->mutable_target_failures())["terminal-budget"] = 1;
    return json(te::PolicyRuntime(reject ? terminal_policy : base).decide(current));
  };
  const auto same_allowed = after_failure("branch-a", false),
             same_rejected = after_failure("branch-a", true);
  const auto other_allowed = after_failure("branch-b", false);
  expect(same_rejected["actions"].items().front().str("decision") == "reject",
         "terminal replay fixture did not reject its own retry");
  const auto same_report = report(failure_events, same_allowed),
             other_report = report(failure_events, other_allowed);
  const auto terminal_preflight = search.control_preflight(parent, terminal_child, same_report);
  expect(terminal_preflight.flag("passed", false) &&
             terminal_preflight["rules"].num("terminal-budget", 0) == 1,
         "same-execution terminal failure was lost from control preflight");
  expect(!search.control_preflight(parent, terminal_child, other_report).flag("passed", false),
         "a sibling's first attempt became a terminal control opportunity");
  expect(
      search.applied_changes(terminal_child, other_report).fields().empty(),
      "allowing a sibling's first attempt was misattributed as removing the parent's restriction");
  expect(search.applied_changes(terminal_child, report(failure_events, same_rejected))
             .fields()
             .empty(),
         "an identical same-execution parent rejection became a new policy effect");
  expect(search.applied_changes(parent, report(failure_events, same_rejected))["task"].num(
             "actions", 0) == 1,
         "an actually applied same-execution terminal guard lost its attribution");
  expect(search.applied_changes(terminal_child, same_report)["task"].num("actions", 0) == 1,
         "removing an actually applicable same-execution terminal guard lost its attribution");

  // 新预算既要能拦浪费，也不能拦已评为必要的第二次任务；只使用真实动作归属。
  auto budget_options = options;
  budget_options.clear_action_rules();
  auto *budget = budget_options.add_action_rules();
  budget->set_rule_id("operation-cap");
  budget->set_target_scope("assistant");
  budget->set_operation("generate");
  budget->set_max_attempts(1);
  budget->set_reason("Stop redundant attempts");
  budget->set_match_mode("predicate");
  *budget->mutable_source_experience() = *approval;
  const Json budget_child{{"policy", json(te::compile_runtime_policy(snapshot, budget_options))}};
  const auto reviewed_budget_report = [&](const char *decision, bool other_action = false,
                                          bool mixed_work = false, bool final_answer = false) {
    auto result = report(sequential, allowed);
    auto evidence = json(result.trials(0).evidence());
    for (auto &event : evidence["trace"]["events"].items()) {
      if (event.str("step_id") != "guard-step")
        continue;
      event["execution_id"] = "coordinator";
      event["loop_id"] = "coordinator";
      event["iteration"] = "3";
      event["actor_id"] = "coordinator";
      if (event.str("kind") == "custom")
        event["event_id"] = "guard-query-event";
      if (event.str("kind") == "action.end") {
        event["event_id"] = "guard-end";
        event["action_id"] = other_action ? "unrelated" : "guard";
        event["operation"] = "generate";
      }
    }
    if (mixed_work || final_answer) {
      auto extra = evidence["trace"]["events"].items().back();
      extra["event_id"] = "independent-end";
      extra["action_id"] = "independent";
      extra["operation"] = final_answer ? "chat" : "write";
      if (final_answer) {
        extra["attributes"]["semantic"] = "model";
        extra["output"] =
            Json{{"choices",
                  Json::Array{Json{{"finish_reason", "stop"},
                                   {"message", Json{{"content", "Necessary final answer"}}}}}}};
      }
      evidence["trace"]["events"].push(extra);
    }
    const Json context{
        {"execution_id", "coordinator"}, {"loop_id", "coordinator"}, {"iteration", "3"}};
    evidence["loop_review"] = Json{
        {"status", "complete"},
        {"observed_iterations", 1},
        {"iteration_reviews",
         Json::Array{Json{{"iteration_id", iteration_identity(Json{{"run_id", "run"}}, context)},
                          {"decision", decision},
                          {"reason", "Necessary or redundant second task"}}}}};
    result.mutable_trials(0)->set_split("train");
    *result.mutable_trials(0)->mutable_evidence() = proto<google::protobuf::Struct>(evidence);
    return result;
  };
  auto budget_check =
      search.control_preflight(parent, budget_child, reviewed_budget_report("keep"));
  expect(!budget_check.flag("passed", true) && budget_check.has("known_keep_truncation") &&
             budget_check["rules"].num("operation-cap", 0) == 1,
         "a real differential budget hit authorized rejection of known necessary work");
  for (const auto *decision : {"waste", "unknown"}) {
    budget_check = search.control_preflight(parent, budget_child, reviewed_budget_report(decision));
    expect(budget_check.flag("passed", false) && !budget_check.has("known_keep_truncation"),
           "waste or incomplete review was falsely marked as necessary work");
  }
  budget_check =
      search.control_preflight(parent, budget_child, reviewed_budget_report("keep", true));
  expect(!budget_check.has("known_keep_truncation"),
         "another action's keep assessment contaminated the budget hit");
  for (const bool final_answer : {false, true}) {
    budget_check = search.control_preflight(
        parent, budget_child, reviewed_budget_report("keep", false, !final_answer, final_answer));
    expect(budget_check.flag("passed", false) && !budget_check.has("known_keep_truncation"),
           "whole-iteration keep was incorrectly attributed to a tool alongside independent work "
           "or delivery");
  }
  for (const auto &content :
       Json::Array{Json(), Json(Json::Array{Json{{"type", "text"}, {"text", "Final answer"}}})}) {
    auto observed = reviewed_budget_report("keep", false, false, true);
    auto evidence = json(observed.trials(0).evidence());
    evidence["trace"]["events"].items().back()["output"]["choices"].items()[0]["message"]
                                                                           ["content"] = content;
    *observed.mutable_trials(0)->mutable_evidence() = proto<google::protobuf::Struct>(evidence);
    budget_check = search.control_preflight(parent, budget_child, observed);
    expect(budget_check.flag("passed", false) && !budget_check.has("known_keep_truncation"),
           "null or structured final content crashed or falsely attributed keep to a tool");
  }
  expect(search.control_preflight(budget_child, budget_child, reviewed_budget_report("keep"))
             .flag("passed", false),
         "loading an unchanged budget was rejected as a new proposal");
}

// 测试辅助函数：terminal_failure_identity。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void terminal_failure_identity() {
  // 终态失败只是事实；只有失败落定后同请求的真实查询才构成可拦截机会。
  const auto opportunity_query = [](const std::string &id, const std::string &execution = "branch-a",
                                     const std::string &scope = "scope",
                                     const std::string &model = "one") {
    return Json{{"kind", "custom"}, {"event_id", "query-" + id},
                 {"attributes", Json{{"type", "policy.query"},
                   {"decision", Json{{"query", Json{{"boundary", "actions"},
                     {"execution_id", execution}, {"advice_query", Json{{"target_scope", scope}}},
                     {"actions", Json::Array{Json{{"action_id", id}, {"operation", "render"},
                       {"arguments", Json{{"model", model}}}}}}}},
                     {"actions", Json::Array{Json{{"action_id", id}, {"decision", "allow"}}}}}}}}};
  };
  const auto opportunity_end = [](const std::string &id, bool failed = true,
                                   const std::string &retryability = "non_retryable") {
    Json event{{"kind", "action.end"}, {"event_id", "end-" + id}, {"action_id", id},
                {"operation", "render"}, {"execution_id", "branch-a"},
                {"status", failed ? "failed" : "completed"},
                {"input", Json{{"model", "one"}}},
                {"attributes", Json{{"executed", true}, {"semantic", "tool"}}}};
    if (failed)
      event["error"] = Json{{"code", "UnsupportedModel"}, {"retryability", retryability}};
    return event;
  };
  const auto opportunity = [&](const Json &events, bool expected) {
    Trial trial;
    *trial.mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", "terminal-opportunity"}}},
                              {"events", events}}}});
    const auto full = feedback_trial(trial, Json::Object{});
    const auto &first = full["evidence"]["runs"].items()[0]["actions"].items()[0];
    expect(first.str("terminal_error_code_candidate") == "UnsupportedModel",
           "首次失败事实因缺少后续调用而丢失");
    expect(action_control_evidence(first) == expected,
           "终态机会未按失败之后的同执行、同请求边界关联");
    Search search;
    const auto input = [&](const Json &view) {
      return Json{{"selected_components", Json::Array{"action_rules"}},
                   {"feedback", Json::Array{Json{{"trial", view}}}}};
    };
    expect(search.control_evidence(input(full), "action_rules") == expected,
           "机会路由仍把首次终态失败本身当作可干预位置");
    const auto compact = control_feedback_trial(trial, Json::Array{"action_rules"}, Json::Object{});
    expect(search.control_evidence(input(compact), "action_rules") == expected,
           "控制反馈裁剪丢失或捏造了真实后续终态查询");
    return first;
  };
  opportunity(Json::Array{opportunity_query("first"), opportunity_end("first")}, false);
  const auto repeated = opportunity(
      Json::Array{opportunity_query("first"), opportunity_end("first"), opportunity_query("retry")},
      true);
  expect(repeated["action_control"].str("terminal_retry_query_event_id") == "query-retry",
         "终态机会没有指向失败之后的真实查询");
  for (const auto &unrelated :
       Json::Array{opportunity_query("other", "branch-b"),
                    opportunity_query("other", "branch-a", "other-scope"),
                    opportunity_query("other", "branch-a", "scope", "two")})
    opportunity(Json::Array{opportunity_query("first"), opportunity_end("first"), unrelated}, false);
  opportunity(Json::Array{opportunity_query("first"), opportunity_query("parallel"),
                           opportunity_end("first")}, false);
  opportunity(Json::Array{opportunity_query("first"), opportunity_end("first", true, "retryable"),
                           opportunity_query("retry")}, false);
  // 并发成功回执先清除失败时期；随后查询不能复活旧的终态机会。
  opportunity(Json::Array{opportunity_query("first"), opportunity_query("parallel"),
                           opportunity_end("first"), opportunity_end("parallel", false),
                           opportunity_query("after-success")}, false);
  // 成功已经结束旧失败时期；其后的显式可重试失败不能抬高终态账本，
  // 更不能让再下一次查询重新关联到成功前的失败 source。
  const auto retryable_after_success = opportunity(
      Json::Array{opportunity_query("first"), opportunity_query("parallel"),
                  opportunity_end("first"), opportunity_end("parallel", false),
                  opportunity_query("transient"),
                  opportunity_end("transient", true, "retryable"),
                  opportunity_query("after-retryable")},
      false);
  expect(retryable_after_success["action_control"]
             .str("terminal_retry_query_event_id")
             .empty(),
         "retryable failure revived a terminal source from before a successful reset");

  const Json options{
      {"action_rules", Json::Array{Json{{"rule_id", "terminal"},
                                        {"target_scope", "scope"},
                                        {"operation", "render"},
                                        {"max_failed_attempts", 1},
                                        {"terminal_error_codes", Json::Array{"UnsupportedModel"}},
                                        {"error_scope_arguments", Json::Array{"model"}}}}}};
  Json ledger;
  auto snapshot = [&](const char *output, const char *model = "one", const char *scope = "scope",
                      const char *execution = "branch-a") {
    return target_failure_snapshot(options, scope, "render",
                                   Json{{"output", output}, {"model", model}}, ledger, execution);
  };
  const Json failure{{"status", "failed"},
                     {"error", "HTTP 404: {\"error\":{\"code\":\"UnsupportedModel\"}}"}};
  auto first = snapshot("a.png");
  settle_target_failure(ledger, first, true, Json{{"error", Json{{"code", "Timeout"}}}});
  expect(snapshot("a.png")["terminal"].num("count", 0) == 0, "transient failure became terminal");
  settle_target_failure(ledger, first, true, failure);
  expect(snapshot("b.png")["terminal"].num("count", 0) == 1,
         "changed output evaded terminal configuration failure");
  expect(snapshot("b.png", "one", "scope", "branch-b")["terminal"].num("count", 0) == 0,
         "an independent first attempt inherited a sibling's terminal model error");
  expect(snapshot("b.png", "one", "scope", "").fields().empty() &&
             target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}}, ledger)
                 .fields()
                 .empty(),
         "missing execution identity became a run-wide terminal circuit breaker");
  expect(snapshot("b.png", "two")["terminal"].num("count", 0) == 0,
         "different model inherited terminal failure");
  expect(snapshot("b.png", "one", "other").fields().empty(),
         "unrelated scope inherited terminal failure");
  expect(snapshot("b.png", "<redacted:model>").fields().empty(),
         "redacted configurations were merged as one identity");
  auto late = snapshot("a.png");
  settle_target_failure(ledger, late, false);
  settle_target_failure(ledger, late, true, failure);
  expect(snapshot("a.png")["terminal"].num("count", 0) == 0,
         "late failure resurrected cleared terminal state");
  auto retry = snapshot("a.png");
  settle_target_failure(ledger, retry, true, failure);
  expect(snapshot("a.png")["terminal"].num("count", 0) == 1,
         "new failed attempt after success was ignored");
  expect(structured_error_code(Json("UnsupportedModel mentioned in prose")).empty(),
         "free text inferred a terminal code");
  Json replay_ledger;
  Json frame{{"entries", target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}},
                                                 replay_ledger, "branch-a")},
             {"scope", "scope"},
             {"operation", "render"},
             {"execution_id", "branch-a"}};
  settle_target_event(
      replay_ledger, frame, options,
      Json{{"kind", "action.end"},
           {"status", "failed"},
           {"input", Json{{"model", "one"}}},
           {"execution_id", "branch-a"},
           {"output", Json{{"envelope", Json::Object{}}}},
           {"attributes", Json{{"executed", true}, {"observed_error_code", "UnsupportedModel"}}}});
  expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}}, replay_ledger,
                                 "branch-a")["terminal"]
                 .num("count", 0) == 1,
         "projected asynchronous error code was not replayable");
  expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}}, replay_ledger,
                                 "branch-b")["terminal"]
                 .num("count", 0) == 0,
         "replayed terminal error escaped its execution identity");
  Json scheduled, schedule_ledger;
  const auto start = [](const std::string &id) {
    return Json{{"kind", "action.start"},
                {"action_id", id},
                {"execution_id", "same-dispatch-parent"},
                {"operation", "render"},
                {"input", Json{{"model", "one"}}},
                {"attributes", Json{{"type", "schedule.action"}, {"target_scope", "scope"}}}};
  };
  snapshot_scheduled_target(scheduled, schedule_ledger, options, start("child-a"));
  settle_target_event(
      schedule_ledger, static_cast<const Json &>(scheduled)["child-a"], options,
      Json{{"kind", "action.end"},
           {"action_id", "child-a"},
           {"execution_id", "same-dispatch-parent"},
           {"status", "failed"},
           {"input", Json{{"model", "one"}}},
           {"attributes", Json{{"executed", true}, {"observed_error_code", "UnsupportedModel"}}}});
  snapshot_scheduled_target(scheduled, schedule_ledger, options, start("child-b"));
  expect(scheduled["child-a"].str("execution_id") == "child-a" &&
             scheduled["child-b"].str("execution_id") == "child-b" &&
             scheduled["child-b"]["entries"]["terminal"].num("count", 0) == 0,
         "scheduled siblings with one dispatch parent shared a terminal failure identity");
  expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}},
                                 schedule_ledger, "child-a")["terminal"]
                 .num("count", 0) == 1,
         "scheduled terminal failure was not settled under the guard's child identity");

  // 显式错误元数据必须贯通预检回放和反思；已有结果码优先，自然语言仍不猜码。
  for (const auto &scenario : Json::Array{
           Json{{"output", Json{{"status", "unsupported"}, {"error", "fixture_miss"}}},
                {"error", Json{{"code", "UnsupportedModel"}, {"retryability", "non_retryable"}}},
                {"expected", "UnsupportedModel"}},
           Json{{"output", Json{{"error", Json{{"code", "Timeout"}}}}},
                {"error", Json{{"code", "UnsupportedModel"}, {"retryability", "non_retryable"}}},
                {"expected", "Timeout"}},
           Json{{"output", Json{{"error", Json{{"code", "Timeout"}}}}},
                {"observed", ""},
                {"error", Json{{"code", "UnsupportedModel"}, {"retryability", "non_retryable"}}},
                {"expected", "Timeout"}},
           Json{{"output", Json{{"error", Json{{"code", "UnsupportedModel"}}}}},
                {"observed", "Timeout"}, {"expected", "Timeout"}},
           Json{{"output", "UnsupportedModel mentioned in prose"}, {"expected", ""}}}) {
    Json explicit_ledger;
    Json explicit_frame{
        {"entries", target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}},
                                              explicit_ledger, "branch-a")},
        {"scope", "scope"}, {"operation", "render"}, {"execution_id", "branch-a"}};
    Json event{{"kind", "action.end"}, {"status", "failed"}, {"action_id", "explicit"},
               {"execution_id", "branch-a"}, {"input", Json{{"model", "one"}}},
               {"output", scenario["output"]}, {"attributes", Json{{"executed", true}}}};
    if (scenario.has("error"))
      event["error"] = scenario["error"];
    if (scenario.has("observed"))
      event["attributes"]["observed_error_code"] = scenario["observed"];
    settle_target_event(explicit_ledger, explicit_frame, options, event);
    const auto expected = scenario.str("expected");
    expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}},
                                   explicit_ledger, "branch-a")["terminal"].num("count", 0) ==
               (expected == "UnsupportedModel" ? 1 : 0),
           "explicit ErrorInfo replay lost structured-code precedence");
    expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}},
                                   explicit_ledger, "branch-b")["terminal"].num("count", 0) == 0,
           "explicit ErrorInfo replay blocked an independent execution");
    expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "two"}},
                                   explicit_ledger, "branch-a")["terminal"].num("count", 0) == 0,
           "explicit ErrorInfo replay blocked a different error-scope configuration");
    trajectory::evolution::dataset::v1::EvaluationTrial trial;
    *trial.mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", "explicit-error"}}},
                            {"events", Json::Array{event}}}}});
    const auto feedback = feedback_trial(trial, Json::Object{});
    const auto &action = feedback["evidence"]["runs"].items()[0]["actions"].items()[0];
    expect(action.str("terminal_error_code_candidate") == expected,
           "reflection lost explicit ErrorInfo or overrode an existing structured code");
  }

  // 没有显式配置作用域时，终态失败仅约束原请求，不能封禁同一工具的其他可行参数。
  // 缺省和空数组语义一致；明确声明 model 的跨输出限制仍由本函数前面的断言覆盖。
  for (const bool empty_field : {false, true}) {
    auto request_scoped = options;
    auto &rule = request_scoped["action_rules"].items()[0];
    if (empty_field)
      rule["error_scope_arguments"] = Json::Array{};
    else
      rule.fields().erase("error_scope_arguments");
    Json request_ledger;
    const Json original{{"action", "browser_open"}, {"url", "fixture:missing"},
                         {"settings", Json{{"mode", "one"}}}};
    const auto before = target_failure_snapshot(request_scoped, "scope", "render", original,
                                                request_ledger, "branch-a");
    settle_target_failure(request_ledger, before, true, failure);
    expect(target_failure_snapshot(request_scoped, "scope", "render", original,
                                   request_ledger, "branch-a")["terminal"].num("count", 0) == 1,
           "same request evaded its terminal failure");
    for (const Json &alternative : Json::Array{
             Json{{"action", "query"}, {"query", "available"}},
             Json{{"action", "browser_open"}, {"url", "fixture:available"},
                  {"settings", Json{{"mode", "one"}}}},
             Json{{"action", "browser_open"}, {"url", "fixture:missing"},
                  {"settings", Json{{"mode", "two"}}}}})
      expect(target_failure_snapshot(request_scoped, "scope", "render", alternative,
                                     request_ledger, "branch-a")["terminal"].num("count", 0) == 0,
             "unscoped terminal failure poisoned different actual arguments");
    expect(target_failure_snapshot(request_scoped, "scope", "render", original,
                                   request_ledger, "branch-b")["terminal"].num("count", 0) == 0,
           "request-local terminal failure poisoned a sibling execution");
    expect(target_failure_snapshot(request_scoped, "scope", "render",
                                   Json{{"nested", Json{{"url", "<redacted:url>"}}}},
                                   request_ledger, "branch-a").fields().empty(),
           "redacted full arguments were used as a shared terminal identity");

    // 长提示词仍有稳定身份；只存摘要，不能因原文超过显式字段上限而静默跳过规则。
    Json long_request{{"prompt", std::string(12000, 'x')}, {"query", "one"}};
    const auto long_before = target_failure_snapshot(request_scoped, "scope", "render",
                                                     long_request, request_ledger, "branch-a");
    expect(long_before["terminal"].object(), "long terminal arguments lost their failure identity");
    settle_target_failure(request_ledger, long_before, true, failure);
    expect(target_failure_snapshot(request_scoped, "scope", "render", long_request,
                                   request_ledger, "branch-a")["terminal"].num("count", 0) == 1,
           "long same arguments evaded their terminal failure");
    long_request["query"] = "two";
    expect(target_failure_snapshot(request_scoped, "scope", "render", long_request,
                                   request_ledger, "branch-a")["terminal"].num("count", 0) == 0,
           "long different arguments shared a terminal failure identity");

    // 本改动不改变无参数总次数预算；该规则原本就需要覆盖同操作的不同请求。
    rule.fields().erase("terminal_error_codes");
    rule.fields().erase("error_scope_arguments");
    rule.fields().erase("max_failed_attempts");
    rule["max_attempts"] = 1;
    Json budget_ledger;
    settle_target_failure(budget_ledger,
                          target_failure_snapshot(request_scoped, "scope", "render", original,
                                                  budget_ledger, "branch-a"), true, failure);
    expect(target_failure_snapshot(request_scoped, "scope", "render", Json{{"other", true}},
                                   budget_ledger, "branch-a")["terminal"].num("count", 0) == 1,
           "request-local terminal change narrowed an ordinary operation budget");
  }
}

// 测试辅助函数：attributed_candidate_selection。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void attributed_candidate_selection() {
  Search search;
  const Json base{{"quality", 1},
                  {"fitness", 0.5},
                  {"invalid_loops", 2},
                  {"loop_method", "model_review"},
                  {"duration_ms", 100}};
  auto better = base;
  better["fitness"] = 0.8;
  better["invalid_loops"] = 1;
  better["duration_ms"] = 90;
  search.gate = Json{{"max_latency_ratio", 1}};
  search.tree = Json{
      {"nodes",
       Json::Array{Json{{"id", 0}, {"status", "accepted"}, {"validation", Json{{"task", base}}}},
                   Json{{"id", 1},
                        {"status", "accepted"},
                        {"parents", Json::Array{0}},
                        {"validation", Json{{"task", better}}},
                        {"validation_applied_changes", Json{{"0", Json::Object{}}}}}}}};
  expect(search.winner() == 0, "unapplied validation gains selected a new winner");
  auto &node = search.tree["nodes"].items()[1];
  node["validation_applied_changes"]["0"]["task"] = Json{{"actions", 1}};
  expect(search.winner() == 1, "observed nonregressing improvement was not selectable");
  node["validation"]["task"]["duration_ms"] = 200;
  expect(search.winner() == 1,
         "changed-task latency incorrectly replaced the independent publication gate");
  node["validation"]["task"] = better;
  node["validation"]["task"]["quality"] = 0;
  expect(search.winner() == 0, "failing faster was counted as attributed improvement");
  node["validation"]["task"] = better;
  node["validation_applied_changes"]["0"] = Json{{"unrelated-training-task", Json{{"loop", 1}}}};
  expect(search.winner() == 0, "training-only evidence substituted for validation effects");

  // 进入留出只要求已有可归因收益；发布门槛必须在冻结留出上单独执行。
  node["validation_applied_changes"]["0"] = Json{{"task", Json{{"actions", 1}}}};
  node["status"] = "accepted";
  node["optimization"] = Json{{"passed", false}};
  expect(search.final_holdout_eligible(1), "publication gate blocked an attributed holdout trial");
  node["status"] = "unobserved_changes";
  node["exploration_eligible"] = true;
  expect(search.final_holdout_eligible(1), "validated exploration branch could not reach holdout");
  node["exploration_eligible"] = false;
  expect(!search.final_holdout_eligible(1), "unqualified unobserved branch reached holdout");
  node["status"] = "accepted";

  // 未配置绝对质量线时只检查相对基线；显式配置仍保持可选能力。
  search.tree["baseline_validation"] = Json{{"task", base}};
  search.evaluation = Json{{"judge", Json{{"assessment_mode", "objectives"}}}};
  search.gate = Json{{"max_quality_drop", 0}};
  const auto relative_only = search.optimization_status(Json{{"task", better}});
  expect(relative_only.flag("passed", false) &&
             relative_only["reasons"].dump().find("quality_below_threshold") ==
                 std::string::npos,
         "missing absolute quality option still defaulted to 0.8");
  search.gate["min_quality"] = 0.9;
  auto below_absolute = better;
  below_absolute["quality"] = 0.8;
  search.tree["baseline_validation"]["task"]["quality"] = 0.8;
  const auto explicitly_bounded = search.optimization_status(Json{{"task", below_absolute}});
  expect(!explicitly_bounded.flag("passed", true) &&
             explicitly_bounded["reasons"].dump().find("quality_below_threshold") !=
                 std::string::npos,
         "explicit absolute quality option was ignored");
  // 旧检查点中“发生变化但收益不可归因”的 accepted 节点必须自动退出前沿；
  // 原始分数与父子关系继续保留，迁移本身不执行新的评测。
  search.val = {"task"};
  node["validation_applied_changes"]["0"] = Json::Object{};
  expect(search.refresh_validation_attribution(), "validation attribution migration did not run");
  expect(node.str("status") == "unobserved_changes" &&
             node.str("attribution_previous_status") == "accepted" &&
             node.str("reason") == "no_attributed_validation_gain" &&
             !node.flag("validation_gain_attributed", true),
         "unattributed accepted node survived migration");
  expect(search.frontier() == std::vector<std::size_t>{0},
         "unattributed node remained in the active frontier");
  const auto migrated = search.tree.dump();
  expect(!search.refresh_validation_attribution() && search.tree.dump() == migrated,
         "validation attribution migration was not idempotent");
}

// 测试辅助函数：operation_budget_identity。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void operation_budget_identity() {
  Json action{{"kind", "action.end"},
              {"executed", true},
              {"action_control", Json{{"decision", "allow"}, {"prior_operation_rounds", 20}}}};
  expect(!action_control_evidence(action),
         "ordinary repeated work crowded out real loop/argument evidence");
  action["guard_only"] = true;
  action["executed"] = false;
  expect(action_control_evidence(action), "redispatch opportunity was lost with execution guards");
  Search search;
  const auto control_input = [](const Json &candidate) {
    const Json run{{"actions", Json::Array{candidate}}};
    const Json evidence{{"runs", Json::Array{run}}};
    const Json trial{{"evidence", evidence}};
    return Json{{"selected_components", Json::Array{"action_rules"}},
                {"feedback", Json::Array{Json{{"trial", trial}}}}};
  };
  action["executed"] = true;
  action["terminal_error_code_candidate"] = "UnsupportedModel";
  expect(!search.control_evidence(control_input(action), "action_rules"),
         "a terminal fact without a later interceptable query bypassed budget binding");
  action["action_control"]["terminal_retry_query_event_id"] = "query-retry";
  expect(search.control_evidence(control_input(action), "action_rules"),
         "a real terminal retry opportunity was hidden by operation-budget gating");
  action.fields().erase("terminal_error_code_candidate");
  action["action_control"].fields().erase("terminal_retry_query_event_id");
  expect(!search.control_evidence(control_input(action), "action_rules"),
         "guard-only operation budget escaped editable-binding gating");
  const Json options{{"action_rules", Json::Array{Json{{"rule_id", "budget"},
                                                       {"target_scope", "scope"},
                                                       {"operation", "verify"},
                                                       {"max_attempts", 2}}}}};
  Json ledger;
  auto snapshot = [&](const char *prompt) {
    return target_failure_snapshot(options, "scope", "verify", Json{{"prompt", prompt}}, ledger);
  };
  const auto first = snapshot("verify a"), parallel = snapshot("verify b");
  settle_target_failure(ledger, first, false);
  settle_target_failure(ledger, parallel, false);
  expect(snapshot("changed wording")["budget"].num("count", 0) == 1,
         "parallel attempts counted as sequential retries");
  settle_target_failure(ledger, snapshot("already finished"), false);
  expect(snapshot("new wording")["budget"].num("count", 0) == 2,
         "successful no-op reset explicit budget");
  expect(
      target_failure_snapshot(options, "scope", "search", Json::Object{}, ledger).fields().empty(),
      "operation budget leaked to unrelated branch");
  expect(target_failure_snapshot(options, "scope", "verify", Json::Object{}, Json{})["budget"].num(
             "count", 0) == 0,
         "operation budget leaked to a new run");

  // 长子循环不能挤掉上层重复派发；反思仍只保留 16 项，并按原始顺序呈现。
  Json events = Json::Array{};
  for (int i = 1; i <= 24; ++i) {
    const auto step = "loop-" + std::to_string(i);
    Json event{{"kind", "custom"}, {"step_id", step}};
    event["attributes"]["type"] = "policy.query";
    event["attributes"]["decision"]["query"] =
        Json{{"boundary", "loop"},
             {"iteration", i},
             {"repeated_action_steps", 2},
             {"advice_query", Json{{"target_scope", "scope"}}}};
    event["attributes"]["decision"]["progress"]["decision"] = "continue";
    events.push(event);
    events.push(Json{{"kind", "loop.decision"}, {"step_id", step}, {"actor_id", "worker"}});
  }
  events.push(Json{{"kind", "action.start"},
                   {"action_id", "first"},
                   {"operation", "delegate"},
                   {"input", Json::Object{}},
                   {"attributes", Json{{"type", "schedule.action"}, {"target_scope", "scope"}}}});
  events.push(Json{{"kind", "action.end"},
                   {"action_id", "first"},
                   {"status", "completed"},
                   {"attributes", Json{{"executed", true}}}});
  const Json planned{
      {"action_id", "guard"}, {"operation", "delegate"}, {"arguments", Json::Object{}}};
  Json guard{{"kind", "custom"}, {"step_id", "guard-step"}};
  guard["attributes"]["type"] = "policy.query";
  guard["attributes"]["decision"]["query"] = Json{{"boundary", "actions"},
                                                  {"execution_id", "dispatcher"},
                                                  {"advice_query", Json{{"target_scope", "scope"}}},
                                                  {"actions", Json::Array{planned}}};
  guard["attributes"]["decision"]["actions"] =
      Json::Array{Json{{"action_id", "guard"}, {"decision", "allow"}}};
  events.push(guard);
  events.push(Json{{"kind", "action.start"},
                   {"action_id", "guard"},
                   {"operation", "delegate"},
                   {"actor_id", "dispatcher"},
                   {"input", Json::Object{}},
                   {"attributes", Json{{"extensions", Json{{"guard_only", true}}}}}});
  events.push(Json{{"kind", "action.end"},
                   {"action_id", "guard"},
                   {"step_id", "guard-step"},
                   {"attributes", Json{{"guard_only", true}, {"executed", false}}}});
  Report report;
  auto *trial = report.add_trials();
  trial->set_variant_id("candidate");
  *trial->mutable_evidence() =
      proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", events}}}});
  const auto compact =
      control_feedback_trial(*trial, Json::Array{"action_rules", "loop_rules"}, Json::Object{});
  const auto &rows = compact["evidence"]["runs"].items().front()["actions"].items();
  expect(rows.size() == 16, "control feedback window changed");
  expect(rows.back().flag("guard_only", false) &&
             rows.back()["action_control"].num("prior_operation_rounds", 0) == 1,
         "long child loop crowded out a repeated parent dispatch");
}

// 测试辅助函数：schedule_resource_projection。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void schedule_resource_projection() {
  auto query = proto<trajectory::evolution::v1::RuntimeQuery>(
      Json{{"boundary", "schedule"},
           {"execution_id", "batch"},
           {"actions", Json::Array{Json{{"action_id", "a"}, {"operation", "prepare"}},
                                   Json{{"action_id", "b"}, {"operation", "render"}},
                                   Json{{"action_id", "c"}, {"operation", "render"}}}}});
  const Json input{
      {"actions", Json::Array{Json{{"action_id", "a"},
                                   {"operation", "prepare"},
                                   {"arguments", Json{{"output", "asset.png"},
                                                      {"prompt", std::string(2000, 'x')}}}},
                              Json{{"action_id", "b"},
                                   {"operation", "render"},
                                   {"arguments", Json{{"input", "asset.png"}}}},
                              Json{{"action_id", "c"},
                                   {"operation", "render"},
                                   {"arguments", Json{{"input", "independent.png"}}}}}}};
  project_schedule_evidence(query, input);
  const auto projected = json(query);
  expect(projected["actions"].items()[0]["arguments"].str("output") == "asset.png",
         "producer evidence missing");
  expect(!projected["actions"].items()[0]["arguments"].has("prompt"),
         "full prompt copied into schedule evidence");
  expect(projected["actions"].items()[1]["arguments"].str("input") == "asset.png",
         "consumer evidence missing");
  expect(!projected["actions"].items()[2].has("arguments"),
         "unshared resource leaked into scheduling context");
  auto replay = query;
  project_schedule_evidence(replay, input);
  expect(replay.SerializeAsString() == query.SerializeAsString(),
         "schedule evidence projection not idempotent");
}

// 测试辅助函数：recovery_component_pairing。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void recovery_component_pairing() {
  const Json available{{"action_rules", Json::Array{}}, {"loop_rules", Json::Array{}}};
  const Json selected = Json::Array{"action_rules"};
  const Json failure{{"kind", "action.end"},
                     {"actor_id", "worker"},
                     {"execution_id", "branch-a"},
                     {"executed", true},
                     {"terminal_error_code_candidate", "UnsupportedModel"},
                     {"action_control", Json{{"decision", "allow"}, {"target_scope", "scope"},
                                              {"terminal_retry_query_event_id", "retry-query"}}}};
  const Json loop{{"kind", "loop.decision"},
                  {"actor_id", "worker"},
                  {"execution_id", "branch-a"},
                  {"loop_control", Json{{"target_scope", "scope"},
                                        {"proposed_decision", "replan"},
                                        {"pending_actions", false}}}};
  auto packet = [&](Json f, Json l) {
    return Json{
        {"feedback",
         Json::Array{
             Json{{"trial",
                   Json{{"evidence",
                         Json{{"runs", Json::Array{Json{{"actions", Json::Array{f, l}}}}}}}}}}}}};
  };
  expect(recovery_components(selected, packet(failure, loop), available).items().size() == 2,
         "same-branch guard and recovery were optimized separately");
  auto sibling = loop;
  sibling["execution_id"] = "branch-b";
  expect(recovery_components(selected, packet(failure, sibling), available).items().size() == 1,
         "independent concurrent branches were coupled");
  sibling = loop;
  sibling["loop_control"]["pending_actions"] = true;
  expect(recovery_components(selected, packet(failure, sibling), available).items().size() == 1,
         "pending work was treated as a recovery opportunity");
  sibling = loop;
  sibling["loop_control"]["target_scope"] = "other";
  expect(recovery_components(selected, packet(failure, sibling), available).items().size() == 1,
         "cross-scope recovery was coupled");
  const Json l1 = Json::Array{"experience:one"};
  expect(recovery_components(l1, packet(failure, loop), available).dump() == l1.dump(),
         "experience selection silently expanded control authority");
}

// 测试辅助函数：prompt_resource_workflows。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void prompt_resource_workflows() {
  namespace te = trajectory::evolution;
  Json input{{"actions", Json::Array{}}};
  for (const auto &item : std::vector<std::pair<std::string, std::string>>{
           {"prepare", "保存图片到 assets/a.png"},
           {"render", "Use assets/a.png as the first frame"},
           {"prepare", "Generate assets/b.png"},
           {"render", "使用 assets/b.png 生成视频"},
           {"lookup", "独立查询，不依赖上述文件"}})
    input["actions"].push(
        Json{{"action_id", "action-" + std::to_string(input["actions"].items().size())},
             {"operation", item.first},
             {"arguments", Json{{"prompt", item.second}}}});
  auto q = proto<te::v1::RuntimeQuery>(input);
  q.set_boundary("schedule");
  q.set_execution_id("batch");
  q.mutable_advice_query()->set_target_scope("scope");
  project_schedule_evidence(q, input);
  expect(json(q)["actions"].items()[0]["arguments"].str(kScheduleResource) == "assets/a.png",
         "shared file hidden in prompt was lost");
  const auto options = proto<te::v1::RuntimeOptions>(
      Json{{"requested_level", "L2"},
           {"host_capabilities", Json::Array{"schedule.dispatch"}},
           {"schedule_rules", Json::Array{Json{{"rule_id", "paired"},
                                               {"target_scope", "scope"},
                                               {"before_operation", "prepare"},
                                               {"after_operation", "render"},
                                               {"before_argument", kScheduleResource},
                                               {"after_argument", kScheduleResource},
                                               {"require_success", true},
                                               {"reason", "explicit resource dependency"}}}}});
  const auto plan = te::plan_schedule(options, q);
  expect(plan.status() == "scheduled" && plan.actions(1).after_size() == 1 &&
             plan.actions(3).after_size() == 1,
         "resource-based rule did not create two separate workflow pairs");
  auto state = te::begin_schedule(plan);
  state = te::advance_schedule(state, "action-0", "running");
  state = te::advance_schedule(state, "action-2", "running");
  state = te::advance_schedule(state, "action-0", "failed");
  state = te::advance_schedule(state, "action-2", "succeeded");
  expect(state.actions(1).status() == "skipped" &&
             te::ready_schedule(state) == std::vector<std::string>({"action-3", "action-4"}),
         "one failed producer cancelled an independent workflow");
  auto ambiguous = input;
  ambiguous["actions"].items()[1]["arguments"]["prompt"] = "assets/a.png assets/b.png";
  project_schedule_evidence(q, ambiguous);
  expect(!json(q)["actions"].items()[1]["arguments"].has(kScheduleResource),
         "ambiguous resource was guessed");
  expect(
      schedule_resources(Json{{"prompt", "https://example.com/assets/a.png?token=hidden"}}).empty(),
      "URL data was used as a local file identity");
  expect(schedule_resources(Json{{kScheduleResource, "assets/forged.png"}}).empty(),
         "caller supplied synthetic resource was trusted");
  for (const auto *path :
       {"甲/a.png", "乙/a.png", "资料assets/a.png", "assets/a.png中文", "C:\\assets/a.png"})
    expect(schedule_resources(Json{{"prompt", path}}).empty(),
           "unsupported path was partially projected");
  expect(schedule_resources(
             Json{{"prompt", "保存为 assets/a.png。请确认；随后使用“assets/a.png”作为首帧"}}) ==
             std::set<std::string>{"assets/a.png"},
         "Chinese punctuation hid a complete ASCII resource");
}

// 测试辅助函数：loop_feedback_runtime_budget。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void loop_feedback_runtime_budget() {
  namespace te = trajectory::evolution;
  agentflow::trajectory::evaluation::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1");
  memory.set_revision(1);
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"},
                                                     {"user_id", "user"},
                                                     {"conversation_id", "conversation"},
                                                     {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
                                                        {"max_policy_entries", 64},
                                                        {"max_advice_items", 3},
                                                        {"max_advice_bytes", 4096},
                                                        {"max_query_bytes", 16384},
                                                        {"max_memory_bytes", 1048576},
                                                        {"max_snapshot_bytes", 262144},
                                                        {"max_match_terms", 128},
                                                        {"min_matched_terms", 2}});
  auto options = proto<te::v1::RuntimeOptions>(
      Json{{"requested_level", "L3"},
           {"fallback", "strict"},
           {"host_capabilities", Json::Array{"context.request", "action.filter", "loop.decide"}},
           {"allowed_users", Json::Array{"user"}},
           {"allowed_conversations", Json::Array{"conversation"}},
           {"experience_template", "{{experience}}"},
           {"max_context_bytes", 4096},
           {"loop_rules", Json::Array{Json{{"rule_id", "base-loop"},
                                           {"target_scope", "assistant"},
                                           {"max_iterations", 12},
                                           {"max_no_progress", 3},
                                           {"no_progress_decision", "stop"},
                                           {"reason", "bounded execution"}}}}});
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  Search search;
  search.controls = Json{{"enabled", true}, {"prompt_file", "test.txt"}, {"min_iterations", 1}};
  validate_control_learning(search.controls);
  search.config = Json{{"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  auto *task = search.dataset.add_tasks();
  task->set_task_id("train-loop");
  task->set_split("train");
  task->set_task_input("Complete the delegated work and report its result.");
  Json events = Json::Array{};
  for (unsigned index = 0; index <= 24; ++index) {
    const bool coordinator = index == 24;
    const auto step = "step-" + std::to_string(index);
    const auto iteration = coordinator ? "3" : "1";
    const auto actor = coordinator ? "coordinator" : "worker";
    Json query{{"boundary", "loop"},        {"iteration", iteration},
               {"state_fingerprint", step}, {"actor_id", actor},
               {"execution_id", step},      {"advice_query", Json{{"target_scope", "assistant"}}}};
    Json progress{{"decision", "continue"}, {"iteration", iteration}, {"fingerprint", step}};
    events.push(
        Json{{"kind", "custom"},
             {"step_id", step},
             {"actor_id", actor},
             {"attributes", Json{{"type", "policy.query"},
                                 {"decision", Json{{"query", query}, {"progress", progress}}}}}});
    events.push(Json{{"kind", "loop.decision"}, {"step_id", step}, {"actor_id", actor}});
  }
  Report report;
  auto *trial = report.add_trials();
  trial->set_trial_id("trial");
  trial->set_task_id(task->task_id());
  trial->set_variant_id("candidate");
  trial->set_verdict("passed");
  *trial->mutable_evidence() =
      proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", events}}}});
  for (bool aggressive : {false, true}) {
    options.set_aggressive_controls(aggressive);
    search.seed = Json{{"schema_version", 1},
                       {"agent", Json{{"revision", "test"}}},
                       {"user_id", "user"},
                       {"level", "L3"},
                       {"policy", json(te::compile_runtime_policy(snapshot, options))}};
    const auto packet = search.packet(search.seed, report, Json::Array{"loop_rules"});
    const auto &runs = packet["feedback"].items().front()["trial"]["evidence"]["runs"];
    expect(runs.array() && !runs.items().empty(),
           "neutral loop context disappeared from the reflection packet");
    const auto &rows = runs.items().front()["actions"].items();
    expect(!rows.empty() && rows.size() <= 8,
           "neutral loop context was lost or expanded beyond the reflection budget");
    const bool included = std::any_of(rows.begin(), rows.end(), [](const auto &row) {
      return row.str("actor_id") == "coordinator" && row["loop_control"].num("iteration", 0) == 3 &&
             row["loop_control"].num("observed_no_progress", 99) == 0;
    });
    expect(!included, "unreviewed high iteration was treated as an iteration-cap opportunity");
  }
  expect(!search.controls.has("aggressive_controls"),
         "runtime feedback flag changed control-learning configuration");
  // COMBEE 直接调用 mutate；空增量必须保持同一版本，不多出策略编号。
  const Json abstain{{"reason", "No justified change"}, {"control_updates", Json::Array{}}};
  expect(search.mutate(search.seed, abstain, Json::Array{"loop_rules"}).dump() ==
             search.seed.dump(),
         "empty delta did not preserve the original agent version");

  // 用真实编译链复现：新增 coordinator 限制不能丢失已有 video-gen 规则。
  namespace ep = agentflow::trajectory::evaluation;
  auto *entry = memory.add_entries();
  entry->set_memory_id("loop-memory");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *source = entry->mutable_candidate();
  source->set_schema_version(1);
  source->set_candidate_id("loop-candidate");
  source->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  source->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  source->set_target_scope("assistant");
  source->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  source->set_trigger_pattern("Repeated failed work");
  source->set_observed_problem("Repeated video calls");
  source->set_proposed_change("Stop retrying completed work.");
  source->set_support_user_count(1);
  auto *evidence = source->add_support_evidence();
  evidence->set_evidence_id("loop-evidence");
  evidence->mutable_source()->set_user_id("user");
  evidence->mutable_source()->set_dialogue_id("source");
  const auto learned_snapshot = te::compile_shadow_policy(memory, identity, limits);
  for (const auto &experience : learned_snapshot.experiences()) {
    auto *approval = options.add_approved_experiences();
    approval->set_memory_id(experience.memory_id());
    approval->set_content_revision(experience.content_revision());
  }
  const Json video{{"memory_id", "loop-memory"},
                   {"actor_id", "video-gen"},
                   {"max_iterations", 12},
                   {"max_no_progress", 1},
                   {"reason", "Stop repeated video work"}};
  const auto learned_options = apply_control_draft(
      learned_snapshot, options, search.controls,
      Json{{"action_rules", Json::Array{}}, {"loop_rules", Json::Array{video}}});
  search.seed["policy"] = json(te::compile_runtime_policy(learned_snapshot, learned_options));
  const auto parent_components = search.components(search.seed);
  const Json coordinator{{"memory_id", "loop-memory"},
                         {"actor_id", "coordinator"},
                         {"max_iterations", 2},
                         {"reason", "Bound unnecessary coordinator iterations"}};
  const Json delta{
      {"reason", "Change coordinator only"},
      {"control_updates",
       Json::Array{Json{{"component", "loop_rules"}, {"op", "add"}, {"value", coordinator}}}}};
  const auto child = search.mutate(search.seed, delta, Json::Array{"loop_rules"});
  const auto child_components = search.components(child);
  expect(child_components["loop_rules"].items().size() == 2 &&
             child_components["loop_rules"].items()[0].dump() ==
                 parent_components["loop_rules"].items()[0].dump(),
         "compiled coordinator delta dropped or rewrote its video sibling");
  const Json drop{{"reason", "Explicitly remove coordinator"},
                  {"control_updates",
                   Json::Array{Json{{"component", "loop_rules"}, {"op", "remove"}, {"index", 1}}}}};
  expect(search.components(search.mutate(child, drop, Json::Array{"loop_rules"})).dump() ==
             parent_components.dump(),
         "explicit deletion cannot restore the parent controls");
  const Json same{{"reason", "Keep existing rule"},
                  {"control_updates",
                   Json::Array{Json{{"component", "loop_rules"},
                                    {"op", "set"},
                                    {"index", 0},
                                    {"value", parent_components["loop_rules"].items()[0]}}}}};
  expect(search.mutate(search.seed, same, Json::Array{"loop_rules"}).dump() == search.seed.dump(),
         "setting an identical rule produced a new version");
  const Json replace{{"reason", "Legacy full replacement"},
                     {"controls", Json{{"loop_rules", Json::Array{coordinator}}}}};
  expect(search.components(
                   search.mutate(search.seed, replace, Json::Array{"loop_rules"}))["loop_rules"]
                 .items()
                 .size() == 1,
         "legacy explicit replacement no longer removes omitted rules");
}

// 测试辅助函数：incremental_control_drafts。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void incremental_control_drafts() {
  const Json video{{"memory_id", "memory"}, {"actor_id", "video-gen"}, {"max_iterations", 12}};
  const Json coordinator{
      {"memory_id", "memory"}, {"actor_id", "coordinator"}, {"max_iterations", 2}};
  const Json before{{"loop_rules", Json::Array{video, coordinator}},
                    {"action_rules", Json::Array{}},
                    {"schedule_rules", Json::Array{}}};
  const auto original = before.dump();
  const Json selected = Json::Array{"loop_rules", "action_rules", "schedule_rules"};
  auto proposal = [](Json updates) {
    return Json{{"reason", "Only change the selected rule."}, {"control_updates", updates}};
  };
  const auto add =
      Json{{"component", "loop_rules"}, {"op", "add"}, {"value", Json{{"actor_id", "writer"}}}};
  const auto remove = Json{{"component", "loop_rules"}, {"op", "remove"}, {"index", 0}};
  auto replacement = coordinator;
  replacement["max_iterations"] = 3;
  const auto set =
      Json{{"component", "loop_rules"}, {"op", "set"}, {"index", 1}, {"value", replacement}};
  auto expanded = expand_control_updates(proposal(Json::Array{add}), before, selected);
  expect(expanded["controls"]["loop_rules"].items().size() == 3 &&
             expanded["controls"]["loop_rules"].items()[0].dump() == video.dump(),
         "adding a coordinator rule erased independent video control");
  expanded = expand_control_updates(proposal(Json::Array{remove, add, set}), before, selected);
  expect(expanded["controls"]["loop_rules"].items().size() == 2 &&
             expanded["controls"]["loop_rules"].items()[0].dump() == replacement.dump(),
         "remove shifted a later parent-array index");
  expect(before.dump() == original, "draft expansion mutated its parent");
  const auto legacy = Json{{"reason", "Explicit replacement"},
                           {"controls", Json{{"loop_rules", Json::Array{coordinator}}}}};
  expect(expand_control_updates(legacy, before, selected).dump() == legacy.dump(),
         "legacy full replacement changed meaning");
  expect(expand_control_updates(expanded, before, selected).dump() == expanded.dump(),
         "draft expansion is not idempotent");
  auto reject = [&](const Json &draft,
                    const Json &allowed =
                        Json(Json::Array{"loop_rules", "action_rules", "schedule_rules"})) {
    bool failed = false;
    try {
      expand_control_updates(draft, before, allowed);
    } catch (const Error &) {
      failed = true;
    }
    expect(failed, "invalid or unauthorized control delta was accepted");
    expect(before.dump() == original, "rejected delta mutated its parent");
  };
  reject(proposal(Json::Array{set, set}));
  reject(proposal(Json::Array{remove, remove}));
  auto remove_same = remove;
  remove_same["index"] = 1;
  reject(proposal(Json::Array{set, remove_same}));
  reject(proposal(Json::Array{add, add}));
  for (const auto &index :
       Json::Array{Json(-1), Json(0.5), Json(2), Json("0"), Json(true), Json()}) {
    auto invalid = set;
    invalid["index"] = index;
    reject(proposal(Json::Array{invalid}));
  }
  auto missing_index = set;
  missing_index.fields().erase("index");

  // editable binding 不是父规则。空 schedule_rules 上的 remove 应在本地成为无改动，
  // 不能触发第二次模型修复；同一草稿里的 add 仍必须正常保留。
  const Json empty_schedule_remove =
      Json{{"component", "schedule_rules"}, {"op", "remove"}, {"index", 0}};
  auto empty_only = expand_control_updates(proposal(Json::Array{empty_schedule_remove}), before,
                                           Json::Array{"schedule_rules"});
  expect(empty_only["experiences"].array() && empty_only["experiences"].items().empty() &&
             !empty_only.has("controls"),
         "empty control removal did not become a local no-op");
  const Json schedule_add =
      Json{{"component", "schedule_rules"},
           {"op", "add"},
           {"value", Json{{"memory_id", "schedule-memory"},
                          {"before_operation", "prepare"},
                          {"after_operation", "render"}}}};
  const auto empty_with_add = expand_control_updates(
      proposal(Json::Array{empty_schedule_remove, schedule_add}), before,
      Json::Array{"schedule_rules"});
  expect(empty_with_add["controls"]["schedule_rules"].items().size() == 1 &&
             empty_with_add["controls"]["schedule_rules"].items()[0].dump() ==
                 schedule_add["value"].dump(),
         "discarding an impossible removal also discarded a valid first add");
  reject(proposal(Json::Array{missing_index}));
  auto indexed_add = add;
  indexed_add["index"] = 0;
  reject(proposal(Json::Array{indexed_add}));
  auto valued_remove = remove;
  valued_remove["value"] = video;
  reject(proposal(Json::Array{valued_remove}));
  for (const auto &value : Json::Array{Json(), Json("rule"), Json(Json::Array{})}) {
    auto invalid = set;
    invalid["value"] = value;
    reject(proposal(Json::Array{invalid}));
  }
  auto unknown_field = add;
  unknown_field["extra"] = true;
  reject(proposal(Json::Array{unknown_field}));
  auto unknown_op = add;
  unknown_op["op"] = "merge";
  reject(proposal(Json::Array{unknown_op}));
  auto unknown_component = add;
  unknown_component["component"] = "experience:memory";
  reject(proposal(Json::Array{unknown_component}));
  reject(proposal(Json::Array{add}), Json::Array{"action_rules"});
  auto mixed = proposal(Json::Array{add});
  mixed["controls"] = Json::Object{};
  reject(mixed);
  reject(proposal(Json::Object{}));
  reject(proposal(Json::Array{Json("add")}));
  auto scheduled = add;
  scheduled["component"] = "schedule_rules";
  auto prepared = expand_control_updates(proposal(Json::Array{scheduled}), before, selected);
  expect(prepared["controls"]["schedule_rules"].items().size() == 1,
         "schedule repair cannot see expanded control delta");

  // 真实 GEPA 不能再把“某一个操作重复失败”编译为作用于所有工具的规则。
  Search scoped;
  scoped.seed = public_alias_fixture();
  scoped.controls = Json{{"enabled", true}, {"prompt_file", "test.txt"}, {"max_rules", 4}};
  const Json scoped_selection = Json::Array{"action_rules"};
  Json retry{{"memory_id", "private"}, {"primitive_id", "repeat_failed_operation"},
             {"reason", "仅限制已有重复失败证据的 render 操作。"}};
  const auto retry_proposal = [&](const Json &rule) {
    return Json{{"reason", "限制重复失败但保留其他操作。"},
                {"control_updates", Json::Array{Json{{"component", "action_rules"},
                                                     {"op", "add"}, {"value", rule}}}}};
  };
  for (const auto &operation : Json::Array{Json(), Json("*")}) {
    auto invalid = retry;
    if (!operation.null())
      invalid["operation"] = operation;
    bool refused = false;
    try {
      scoped.mutate(scoped.seed, retry_proposal(invalid), scoped_selection);
    } catch (const Error &error) {
      refused = std::string(error.what()) == "control_repeat_operation_required";
    }
    expect(refused, "new GEPA repeated-failure control expanded to all operations");
  }
  retry["operation"] = "render";
  const auto narrowed = scoped.mutate(scoped.seed, retry_proposal(retry), scoped_selection);
  const auto reconstructed = scoped.compose(narrowed, scoped.components(narrowed));
  expect(scoped.components(reconstructed).dump() == scoped.components(narrowed).dump(),
         "specific-operation failure guard was lost during component recomposition");

  // 直接加载的旧通配规则可继承和删除，不因新增提案契约而破坏旧版本。
  auto legacy_retry = retry;
  legacy_retry.fields().erase("operation");
  const auto old_policy = proto<trajectory::evolution::v1::RuntimePolicy>(scoped.seed["policy"]);
  auto legacy_seed = scoped.seed;
  legacy_seed["policy"] = json(trajectory::evolution::compile_runtime_policy(
      old_policy.snapshot(), apply_control_draft(old_policy.snapshot(), old_policy.options(),
          scoped.controls, Json{{"action_rules", Json::Array{legacy_retry}},
                                {"loop_rules", Json::Array{}}})));
  const auto inherited = scoped.components(legacy_seed)["action_rules"];
  const Json keep{{"reason", "保留旧版已存在规则。"},
                  {"controls", Json{{"action_rules", inherited}}}};
  expect(scoped.mutate(legacy_seed, keep, scoped_selection).dump() == legacy_seed.dump(),
         "unchanged legacy guard was rejected");
  const Json erase{{"reason", "移除旧版通配规则。"},
                   {"control_updates", Json::Array{Json{{"component", "action_rules"},
                                                        {"op", "remove"}, {"index", 0}}}}};
  expect(scoped.components(scoped.mutate(legacy_seed, erase, scoped_selection))
             ["action_rules"].items().empty(), "legacy guard could not be removed");
  auto broadened = inherited.items()[0];
  broadened["reason"] = "新建通配规则不能伪装成旧规则的继承。";
  bool refused = false;
  try {
    scoped.mutate(scoped.seed, retry_proposal(broadened), scoped_selection);
  } catch (const Error &error) {
    refused = std::string(error.what()) == "control_repeat_operation_required";
  }
  expect(refused, "canonical rule format bypassed the new-operation scope requirement");

  // 编排修复必须仍经过真实编译器：反向 add 是成环，不是把原有串行改成并行。
  auto schedule_policy = proto<trajectory::evolution::v1::RuntimePolicy>(scoped.seed["policy"]);
  auto schedule_options = schedule_policy.options();
  schedule_options.add_host_capabilities("schedule.dispatch");
  scoped.seed["policy"] = json(trajectory::evolution::compile_runtime_policy(
      schedule_policy.snapshot(), schedule_options));
  const Json schedule_selected = Json::Array{"schedule_rules"};
  const Json forward{{"memory_id", "private"}, {"before_operation", "prepare"},
                     {"after_operation", "render"}, {"require_success", false},
                     {"reason", "准备后再渲染。"}, {"task_terms", Json::Array{"render"}}};
  const Json forward_add{{"component", "schedule_rules"}, {"op", "add"}, {"value", forward}};
  const auto schedule_parent = scoped.mutate(scoped.seed, proposal(Json::Array{forward_add}),
                                             schedule_selected);
  const auto schedule_original = schedule_parent.dump();
  auto backward = forward;
  backward["before_operation"] = "render";
  backward["after_operation"] = "prepare";
  const Json backward_add{{"component", "schedule_rules"}, {"op", "add"}, {"value", backward}};
  const auto cyclic = proposal(Json::Array{backward_add});
  bool cycle_rejected = false;
  try {
    scoped.mutate(schedule_parent, cyclic, schedule_selected);
  } catch (const std::exception &error) {
    cycle_rejected = std::string(error.what()) == "schedule_rule_cycle";
  }
  expect(cycle_rejected, "相同守卫下的双向编排没有准确拒绝成环");

  // 模拟模型明确删除父边再提出反向边；程序不能擅自选边，也不能污染父策略。
  const auto repaired = scoped.mutate(schedule_parent,
      proposal(Json::Array{Json{{"component", "schedule_rules"}, {"op", "remove"},
                               {"index", 0}}, backward_add}), schedule_selected);
  const auto repaired_rules = scoped.components(repaired)["schedule_rules"];
  expect(repaired_rules.items().size() == 1 &&
             repaired_rules.items()[0].str("before_operation") == "render" &&
             schedule_parent.dump() == schedule_original,
         "显式修复后方向不符，或修复改写了父策略");

  // 模拟修复额度耗尽时的落盘状态（不执行完整预算流程）：非法草稿仍失败并保留固定错误码。
  char directory[] = "/tmp/trajectory-gepa-cycle-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "无法创建编排成环回归目录");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
  } cleanup{directory};
  scoped.root = directory;
  scoped.tree = Json{{"model_calls", 2}, {"nodes", Json::Array{Json{{"status", "reflecting"},
      {"repair_error", "schedule_rule_cycle"}}}}};
  try {
    scoped.mutate(schedule_parent, cyclic, schedule_selected);
    expect(false, "修复后的成环草稿绕过了无环校验");
  } catch (const std::exception &error) {
    scoped.error(0, error);
  }
  const auto persisted = Json::parse(read(scoped.root / "tree.json"));
  const auto &failed = persisted["nodes"].items()[0];
  expect(failed.str("error") == "schedule_rule_cycle" &&
             failed.str("repair_error") == "schedule_rule_cycle" &&
             failed.str("error_stage") == "reflecting" && failed.str("status") == "error" &&
             persisted.num("model_calls", 0) == 2 && schedule_parent.dump() == schedule_original,
         "修复失败丢失成环错误码、改变调用计数或污染父策略");
}

// 测试辅助函数：pure_control_removal_preflight。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void pure_control_removal_preflight() {
  namespace te = trajectory::evolution;
  namespace ep = agentflow::trajectory::evaluation;
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1");
  memory.set_revision(1);
  auto *entry = memory.add_entries();
  entry->set_memory_id("removal-memory");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *source = entry->mutable_candidate();
  source->set_schema_version(1);
  source->set_candidate_id("removal-candidate");
  source->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  source->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  source->set_target_scope("assistant");
  source->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  source->set_trigger_pattern("Repeated failed work");
  source->set_observed_problem("Unnecessary repeated calls");
  source->set_proposed_change("Keep independent work running.");
  source->set_support_user_count(1);
  auto *support = source->add_support_evidence();
  support->set_evidence_id("removal-evidence");
  support->mutable_source()->set_user_id("user");
  support->mutable_source()->set_dialogue_id("source");
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"},
                                                     {"user_id", "user"},
                                                     {"conversation_id", "conversation"},
                                                     {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
                                                        {"max_policy_entries", 64},
                                                        {"max_advice_items", 3},
                                                        {"max_advice_bytes", 4096},
                                                        {"max_query_bytes", 16384},
                                                        {"max_memory_bytes", 1048576},
                                                        {"max_snapshot_bytes", 262144},
                                                        {"max_match_terms", 128},
                                                        {"min_matched_terms", 2}});
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  expect(snapshot.experiences_size() == 1, "removal experience fixture not eligible");
  auto options = proto<te::v1::RuntimeOptions>(
      Json{{"requested_level", "L3"},
           {"fallback", "strict"},
           {"host_capabilities",
            Json::Array{"context.request", "action.filter", "schedule.dispatch", "loop.decide"}},
           {"allowed_users", Json::Array{"user"}},
           {"allowed_conversations", Json::Array{"conversation"}},
           {"experience_template", "{{experience}}"},
           {"max_context_bytes", 4096}});
  auto *approval = options.add_approved_experiences();
  approval->set_memory_id(snapshot.experiences(0).memory_id());
  approval->set_content_revision(snapshot.experiences(0).content_revision());
  const auto ref = *approval;
  auto artifact = [&](const te::v1::RuntimeOptions &value) {
    return Json{{"policy", json(te::compile_runtime_policy(snapshot, value))}};
  };
  const auto child = artifact(options);
  auto query = [&](const char *boundary) {
    return proto<te::v1::RuntimeQuery>(
        Json{{"boundary", boundary},
             {"execution_id", "execution"},
             {"actor_id", "worker"},
             {"advice_query", Json{{"identity", json(identity)},
                                   {"target_scope", "assistant"},
                                   {"task", "Complete the task"},
                                   {"query_id", "removal-query"},
                                   {"decision_id", "removal-decision"}}}});
  };
  auto report = [&](const Json &parent, const te::v1::RuntimeQuery &input,
                    const char *status = "") {
    const auto actual =
        json(te::PolicyRuntime(proto<te::v1::RuntimePolicy>(parent["policy"])).decide(input));
    Json events = Json::Array{
        Json{{"kind", "custom"},
             {"step_id", "step"},
             {"actor_id", input.actor_id()},
             {"attributes", Json{{"type", "policy.query"}, {"decision", actual}}}},
        Json{{"kind", "advice.applied"},
             {"application", Json{{"query_id", "removal-query"},
                                  {"status", *status                               ? status
                                             : actual.str("status") == "no_change" ? "rejected"
                                                                                   : "applied"}}}}};
    if (input.boundary() == "schedule")
      events.push(Json{{"kind", "step.end"}, {"step_id", "step"}, {"status", "completed"}});
    if (input.boundary() == "loop")
      events.push(Json{{"kind", "loop.decision"}, {"step_id", "step"}});
    Report result;
    auto *trial = result.add_trials();
    trial->set_variant_id("candidate");
    trial->set_task_id("train-task");
    trial->set_split("train");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}}});
    return result;
  };
  Search search;
  auto scheduled_options = options;
  auto *training_task = search.dataset.add_tasks();
  training_task->set_task_id("train-task");
  training_task->set_split("train");
  training_task->set_task_input("Complete the task");
  const Json success_contract{{"dependency", "sequential"}, {"stop_on_failure", true}};
  *training_task->mutable_attributes() = proto<google::protobuf::Struct>(success_contract);
  auto *schedule = scheduled_options.add_schedule_rules();
  schedule->set_rule_id("learned-order");
  schedule->set_target_scope("assistant");
  schedule->set_before_operation("prepare");
  schedule->set_after_operation("render");
  schedule->set_before_argument("resource");
  schedule->set_after_argument("resource");
  schedule->set_require_success(true);
  schedule->set_reason("Wait for the matching resource");
  *schedule->mutable_source_experience() = ref;
  const auto scheduled = artifact(scheduled_options);
  auto schedule_query = query("schedule");
  *schedule_query.add_actions() =
      proto<te::v1::RuntimeAction>(Json{{"action_id", "producer"},
                                        {"operation", "prepare"},
                                        {"arguments", Json{{"resource", "a.png"}}}});
  *schedule_query.add_actions() =
      proto<te::v1::RuntimeAction>(Json{{"action_id", "consumer"},
                                        {"operation", "render"},
                                        {"arguments", Json{{"resource", "b.png"}}}});
  const auto original_parent = scheduled.dump();
  auto check = search.control_preflight(scheduled, child, report(scheduled, schedule_query));
  expect(!check.flag("passed", true) && check.flag("pure_removal_no_effect", false) &&
             check["removed_rule_ids"].items().size() == 1 &&
             check["removal_changes"].fields().empty(),
         "deleting a schedule rule that never matched was accepted");
  (*schedule_query.mutable_actions(1)->mutable_arguments()->mutable_fields())["resource"]
      .set_string_value("a.png");
  // 依赖变化与模型规划提示必须分开计数；后者不能被展示成实际编排。
  auto added = search.control_preflight(child, scheduled, report(child, schedule_query));
  expect(added["rules"].num("learned-order", 0) == 1 &&
             added["execution_changes"].num("learned-order", 0) == 1 &&
             added["planning_hints"].fields().empty(),
         "schedule graph change was not separated from planning hints");
  // 同一对操作的结果条件不是成功条件；机械加边也不能通过预检或被纠正成成功依赖。
  auto conditional_contract = success_contract;
  conditional_contract["turn_expectations"] = Json::Array{Json{
      {"turn", 1}, {"expected_behavior", "按结果条件决定是否执行后续操作"}, {"conditional", true}}};
  *training_task->mutable_attributes() = proto<google::protobuf::Struct>(conditional_contract);
  const auto conditional_check =
      search.control_preflight(child, scheduled, report(child, schedule_query));
  expect(!conditional_check.flag("passed", true) &&
             conditional_check.has("unsupported_schedule_contract") &&
             conditional_check["execution_changes"].num("learned-order", 0) == 0,
         "结果条件被当作普通成功依赖通过了预检");
  const Json conditional_draft{{"controls", Json{{"schedule_rules", Json::Array{Json{
      {"before_operation", "prepare"}, {"after_operation", "render"},
      {"task_terms", Json::Array{"task"}}, {"require_success", false}}}}}}};
  Json repairs;
  const auto unrepaired = search.repair_schedule_contracts(
      conditional_draft, report(child, schedule_query), repairs);
  expect(repairs.items().empty() &&
             !unrepaired["controls"]["schedule_rules"].items()[0].flag("require_success", false),
         "结果条件被旧契约修复路径错误升级为执行成功条件");
  *training_task->mutable_attributes() = proto<google::protobuf::Struct>(success_contract);
  auto hinted_options = scheduled_options;
  auto *hint = hinted_options.mutable_schedule_rules(0);
  hint->clear_before_argument();
  hint->clear_after_argument();
  hint->add_task_terms("task");
  const auto hinted = artifact(hinted_options);
  added = search.control_preflight(child, hinted, report(child, query("llm")));
  expect(!added.flag("passed", true) && added["rules"].num("learned-order", 0) == 0 &&
             added["planning_hints"].num("learned-order", 0) == 0 &&
             added["execution_changes"].num("learned-order", 1) == 0 &&
             added["missing"].items().size() == 1,
         "planning-only preflight was accepted without a schedule graph change");

  check = search.control_preflight(scheduled, child, report(scheduled, schedule_query));
  expect(check.flag("passed", false) &&
             check["removal_changes"]["train-task"].num("schedule", 0) == 1,
         "removing a genuinely applied dependency was blocked");
  expect(!search.control_preflight(scheduled, child, report(scheduled, schedule_query, "failed"))
              .flag("passed", true),
         "a failed schedule receipt authorized removal");
  auto *host_edge = schedule_query.mutable_actions(1)->add_after();
  host_edge->set_action_id("producer");
  host_edge->set_require_success(true);
  expect(!search.control_preflight(scheduled, child, report(scheduled, schedule_query))
              .flag("passed", true),
         "retained host dependency or changed policy metadata counted as an execution difference");
  expect(scheduled.dump() == original_parent, "deletion preflight modified its parent");

  // 同一查询中的独立分支仍继续；不把 rule_id 变化当作拒绝效果变化。
  auto guarded_options = options;
  auto *guard = guarded_options.add_action_rules();
  guard->set_rule_id("learned-guard");
  guard->set_target_scope("assistant");
  guard->set_operation("render");
  guard->add_required_arguments("image");
  guard->set_reason("Image is required");
  guard->set_match_mode("predicate");
  *guard->mutable_source_experience() = ref;
  const auto guarded = artifact(guarded_options);
  auto action_query = query("actions");
  *action_query.add_actions() =
      proto<te::v1::RuntimeAction>(Json{{"action_id", "render"}, {"operation", "render"}});
  *action_query.add_actions() =
      proto<te::v1::RuntimeAction>(Json{{"action_id", "independent"}, {"operation", "summarize"}});
  const auto decisions =
      te::PolicyRuntime(proto<te::v1::RuntimePolicy>(guarded["policy"])).decide(action_query);
  expect(decisions.actions(0).decision() == "reject" && decisions.actions(1).decision() == "allow",
         "action fixture lost branch isolation");
  check = search.control_preflight(guarded, child, report(guarded, action_query));
  expect(check.flag("passed", false) &&
             check["removal_changes"]["train-task"].num("actions", 0) == 1,
         "removal of an applied action rejection was blocked");
  action_query.mutable_actions(0)->set_operation("summarize");
  expect(
      !search.control_preflight(guarded, child, report(guarded, action_query)).flag("passed", true),
      "unrelated action siblings justified removal of a nonmatching guard");
  action_query.mutable_actions(0)->set_operation("render");
  auto equivalent_options = guarded_options;
  auto *equivalent = equivalent_options.add_action_rules();
  *equivalent = guarded_options.action_rules(0);
  equivalent->set_rule_id("host-guard");
  equivalent->clear_source_experience();
  auto equivalent_child = options;
  *equivalent_child.add_action_rules() = *equivalent;
  const auto equivalent_parent = artifact(equivalent_options);
  expect(!search
              .control_preflight(equivalent_parent, artifact(equivalent_child),
                                 report(equivalent_parent, action_query))
              .flag("passed", true),
         "same rejection with a different rule_id was counted as behavior change");

  auto bounded_options = options;
  auto *loop = bounded_options.add_loop_rules();
  loop->set_rule_id("learned-loop");
  loop->set_target_scope("assistant");
  loop->set_actor_id("worker");
  loop->set_max_iterations(2);
  loop->set_no_progress_decision("stop");
  loop->set_reason("Bound repeated work");
  loop->set_match_mode("predicate");
  *loop->mutable_source_experience() = ref;
  const auto bounded = artifact(bounded_options);
  auto loop_query = query("loop");
  loop_query.set_iteration(3);
  loop_query.set_state_fingerprint("state");
  check = search.control_preflight(bounded, child, report(bounded, loop_query));
  expect(check.flag("passed", false) && check["removal_changes"]["train-task"].num("loop", 0) == 1,
         "removing a committed stop was blocked");
  auto recovery_options = bounded_options;
  auto *recovery = recovery_options.mutable_loop_rules(0);
  recovery->set_max_iterations(12);
  recovery->set_max_no_progress(1);
  recovery->set_no_progress_decision("replan");
  recovery->set_max_replans(1);
  recovery->set_replan_context("Use another source.");
  const auto recovery_parent = artifact(recovery_options);
  auto recovery_query = loop_query;
  recovery_query.set_repeated_action_steps(1);
  expect(te::PolicyRuntime(proto<te::v1::RuntimePolicy>(recovery_parent["policy"]))
                 .decide(recovery_query)
                 .progress()
                 .decision() == "replan",
         "recovery fixture did not request a replan");
  expect(search.control_preflight(recovery_parent, child, report(recovery_parent, recovery_query))
             .flag("passed", false),
         "removal of a genuinely applied replan was blocked");
  auto narrowed_options = recovery_options;
  auto *narrowed = narrowed_options.mutable_loop_rules(0);
  narrowed->set_rule_id("conditional-stop");
  narrowed->set_max_replans(0);
  narrowed->set_no_progress_decision("stop");
  narrowed->clear_replan_context();
  check = search.control_preflight(recovery_parent, artifact(narrowed_options),
                                   report(recovery_parent, recovery_query));
  expect(!check.flag("passed", true) && check.has("unconditioned_recovery"),
         "new reduced recovery budget used prose instead of an executable condition");
  narrowed->set_after_action_rejection(true);
  check = search.control_preflight(recovery_parent, artifact(narrowed_options),
                                   report(recovery_parent, recovery_query));
  expect(!check.flag("passed", true) && !check.has("unconditioned_recovery"),
         "unobserved conditional recovery was authorized");
  recovery_query.set_rejected_actions(1);
  check = search.control_preflight(recovery_parent, artifact(narrowed_options),
                                   report(recovery_parent, recovery_query));
  expect(check.flag("passed", false),
         "observed branch rejection could not authorize a conditional stop");
  const auto unchanged = artifact(narrowed_options);
  expect(search.control_preflight(unchanged, unchanged, report(unchanged, recovery_query))
             .flag("passed", false),
         "unchanged recovery policy was forced through a new-candidate condition check");
  loop_query.set_actor_id("coordinator");
  expect(
      !search.control_preflight(bounded, child, report(bounded, loop_query)).flag("passed", true),
      "another actor justified removal of a worker-only loop cap");
  loop_query.set_actor_id("worker");
  loop_query.set_pending_actions(true);
  expect(
      !search.control_preflight(bounded, child, report(bounded, loop_query)).flag("passed", true),
      "pending work was treated as an applied loop stop");
  loop_query.set_pending_actions(false);
  expect(!search.control_preflight(bounded, child, report(bounded, loop_query, "failed"))
              .flag("passed", true),
         "an uncommitted loop stop justified deletion");

  // 重复实例全浪费，也不能用 actor 级新 cap 截断同一 actor 的必要实例。
  auto reviewed_report = [&](bool third_required, bool final_tool = false,
                             bool omit_final_query = false) {
    Json events = Json::Array{}, reviews = Json::Array{};
    for (const std::string execution : {"required", "redundant"})
      for (std::size_t i = 1; i <= 4; ++i) {
        auto input = query("loop");
        input.set_execution_id(execution);
        input.set_iteration(i);
        input.set_can_finalize(true);
        input.set_state_fingerprint(execution + std::to_string(i));
        Json context{{"actor_id", "worker"},
                     {"execution_id", execution},
                     {"loop_id", execution},
                     {"iteration", std::to_string(i)},
                     {"step_id", execution + std::to_string(i)}};
        const auto actual =
            json(te::PolicyRuntime(proto<te::v1::RuntimePolicy>(child["policy"])).decide(input));
        auto event = context;
        event["event_id"] = context.str("step_id") + "-query";
        event["kind"] = "custom";
        event["attributes"] = Json{{"type", "policy.query"}, {"decision", actual}};
        events.push(event);
        event = context;
        event["event_id"] = context.str("step_id") + "-loop";
        event["kind"] = "loop.decision";
        event["decision"] = "continue";
        events.push(event);
        Json review{{"iteration_id", iteration_identity(Json{{"run_id", "run"}}, context)},
                    {"reason", "Verified necessary or redundant work"}};
        review["decision"] =
            execution == "required" && (i != 3 || third_required) ? "keep" : "waste";
        reviews.push(review);
        if (i == 4) {
          event = context;
          event["kind"] = "action.start";
          event["action_id"] = execution + "-answer";
          event["attributes"] = Json{{"semantic", "model"}};
          events.push(event);
          event["kind"] = "action.end";
          event["status"] = "completed";
          event["attributes"]["executed"] = true;
          event["output"] = Json{
              {"choices",
               Json::Array{Json{{"finish_reason", "stop"},
                                {"message", Json{{"content", "Results from completed work."}}}}}}};
          events.push(event);
          if (final_tool && execution == "required") {
            event = context;
            event["kind"] = "action.start";
            event["action_id"] = "required-write";
            event["attributes"] = Json{{"semantic", "action"}};
            events.push(event);
          }
        }
      }
    if (omit_final_query)
      events.items().erase(std::remove_if(events.items().begin(), events.items().end(),
                                          [](const auto &event) {
                                            return event.str("execution_id") == "required" &&
                                                   event.str("iteration") == "4" &&
                                                   (event.str("kind") == "custom" ||
                                                    event.str("kind") == "loop.decision");
                                          }),
                           events.items().end());
    events.push(Json{{"kind", "run.end"}, {"outcome", Json{{"execution_status", "completed"}}}});
    Report result;
    auto *trial = result.add_trials();
    trial->set_variant_id("candidate");
    trial->set_task_id("train-task");
    trial->set_split("train");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}},
             {"loop_review", Json{{"status", "complete"},
                                  {"observed_iterations", 8},
                                  {"iteration_reviews", reviews}}}});
    return result;
  };
  check = search.control_preflight(child, bounded, reviewed_report(true));
  expect(!check.flag("passed", true) && check.has("known_keep_truncation"),
         "a new concrete cap could truncate known necessary work in another same-actor execution");
  auto finalizing_options = bounded_options;
  finalizing_options.mutable_loop_rules(0)->set_require_finalization(true);
  const auto finalizing = artifact(finalizing_options);
  check = search.control_preflight(child, finalizing, reviewed_report(true));
  expect(!check.flag("passed", true) && check.has("known_keep_truncation"),
         "finalization exempted a necessary nonfinal iteration");
  check = search.control_preflight(child, finalizing, reviewed_report(false));
  expect(check.flag("passed", false) && !check.has("known_keep_truncation"),
         "canonical run.end final answer could not be safely replaced by host finalization");
  check = search.control_preflight(child, bounded, reviewed_report(false));
  expect(!check.flag("passed", true) && check.has("known_keep_truncation"),
         "a cap without host finalization skipped a necessary final answer");
  check = search.control_preflight(child, finalizing, reviewed_report(false, true));
  expect(!check.flag("passed", true) && check.has("known_keep_truncation"),
         "a final-iteration tool was mistaken for an answer-only finalization");
  check = search.control_preflight(child, finalizing, reviewed_report(false, true, true));
  expect(!check.flag("passed", true) && check.has("known_keep_truncation"),
         "a missing final loop query hid known necessary action work from cap preflight");

  // 经验正文、触发条件或停用与删除控制并存时，交给既有完整评测，不误判为纯删除。
  auto untriggered = schedule_query;
  untriggered.mutable_actions(1)->clear_after();
  (*untriggered.mutable_actions(1)->mutable_arguments()->mutable_fields())["resource"]
      .set_string_value("b.png");
  auto rewritten = options;
  auto *rewrite = rewritten.add_experience_overrides();
  *rewrite->mutable_source_experience() = ref;
  rewrite->set_content("Use available evidence before requesting another attempt.");
  check = search.control_preflight(scheduled, artifact(rewritten), report(scheduled, untriggered));
  expect(check.flag("passed", false) && !check.has("pure_removal_no_effect"),
         "mixed L1 content rewrite was blocked as pure deletion");
  rewrite->set_content(snapshot.experiences(0).content());
  rewrite->set_trigger_pattern("Missing image argument");
  check = search.control_preflight(scheduled, artifact(rewritten), report(scheduled, untriggered));
  expect(check.flag("passed", false) && !check.has("pure_removal_no_effect"),
         "mixed L1 trigger rewrite was blocked as pure deletion");
  auto disabled = options;
  disabled.clear_approved_experiences();
  check = search.control_preflight(scheduled, artifact(disabled), report(scheduled, untriggered));
  expect(check.flag("passed", false) && !check.has("pure_removal_no_effect"),
         "mixed L1 disable was blocked as pure deletion");

  // L1 消融不撤销控制来源：同一经验的文字可停用，已编译动作护栏继续工作。
  search.controls = Json{{"enabled", true},
                         {"prompt_file", "unused-in-unit-test"},
                         {"actions", Json{{"render", Json::Array{"image"}}}}};
  const Json control_draft{
      {"action_rules", Json::Array{Json{{"memory_id", "removal-memory"},
                                        {"operation", "render"},
                                        {"required_arguments", Json::Array{"image"}},
                                        {"match_mode", "predicate"},
                                        {"reason", "Require the input image"}}}},
      {"loop_rules", Json::Array{}}};
  const auto control_options =
      apply_control_draft(snapshot, options, search.controls, control_draft);
  search.seed = Json{{"schema_version", 1},
                     {"agent", Json{{"revision", "ablation-fixture"}}},
                     {"user_id", "user"},
                     {"level", "L3"},
                     {"policy", json(te::compile_runtime_policy(snapshot, control_options))}};
  const Json selected = Json::Array{"experience:removal-memory"};
  const Json remove{{"reason", "Compare the actual injection with no injection"},
                    {"experience", Json{{"memory_id", "removal-memory"}, {"content", nullptr}}}};
  const auto ablated = search.mutate(search.seed, remove, selected);
  const auto ablated_policy = proto<te::v1::RuntimePolicy>(ablated["policy"]);
  expect(search.components(ablated)["experience:removal-memory"].null(),
         "disabled experience lost its null search axis");
  expect(ablated_policy.options().approved_experiences_size() == 1 &&
             ablated_policy.options().disabled_injections_size() == 1,
         "ablation revoked control source approval");
  expect(search.components(ablated)["action_rules"].dump() ==
             search.components(search.seed)["action_rules"].dump(),
         "L1-only ablation mutated a sibling control component");
  expect(te::PolicyRuntime(ablated_policy).decide(action_query).actions(0).decision() == "reject",
         "L1 ablation disabled the generated action guard");
  expect(te::published_policy_compatible(ablated_policy,
                                         proto<te::v1::RuntimePolicy>(search.seed["policy"]),
                                         search.controls.dump()),
         "disabled injection was rejected on compatible bundle reload");
  auto llm = query("llm");
  llm.mutable_advice_query()->set_task("Repeated failed work");
  expect(!te::PolicyRuntime(proto<te::v1::RuntimePolicy>(search.seed["policy"]))
              .decide(llm)
              .context()
              .empty(),
         "ablation fixture never injected its source");
  expect(te::PolicyRuntime(ablated_policy).decide(llm).context().empty(),
         "ablated text was still injected");
  auto observed_ablation = report(ablated, llm);
  auto *ablation_trial = observed_ablation.mutable_trials(0);
  auto ablation_evidence = json(ablation_trial->evidence());
  ablation_evidence["trace"]["events"].push(
      Json{{"kind", "action.end"}, {"step_id", "step"}, {"attributes", Json{{"executed", true}}}});
  *ablation_trial->mutable_evidence() = proto<google::protobuf::Struct>(ablation_evidence);
  expect(search.applied_changes(search.seed, observed_ablation)["train-task"].num("llm", 0) == 1,
         "zero new application receipts incorrectly rejected a real L1 removal");
  const Json restore{{"reason", "Restore the same authorized source"},
                     {"experience", Json{{"memory_id", "removal-memory"},
                                         {"content", snapshot.experiences(0).content()}}}};
  const auto restored = search.mutate(ablated, restore, selected);
  expect(!restored["policy"]["options"].has("disabled_injections") &&
             search.components(restored).dump() == search.components(search.seed).dump(),
         "disabled experience could not be restored");

  // 第二候选只在本经验实际应用且同试次有失败/已评审浪费时安排消融；不是因果结论。
  auto applied_report = report(search.seed, llm);
  auto *applied_trial = applied_report.mutable_trials(0);
  applied_trial->set_execution_status("completed");
  applied_trial->set_verdict("failed");
  auto applied_evidence = json(applied_trial->evidence());
  applied_evidence["trace"]["events"].items()[1]["attributes"]["receipt"] =
      Json{{"boundary", "llm"}, {"experiences", Json::Array{json(ref)}}};
  *applied_trial->mutable_evidence() = proto<google::protobuf::Struct>(applied_evidence);
  expect(!search.experience_ablation(search.seed, selected, applied_report).null(),
         "actual failed injection got no ablation opportunity");
  expect(search.experience_ablation(ablated, selected, applied_report).null(),
         "disabled experience was ablated again");
  expect(
      search.experience_ablation(search.seed, Json::Array{"action_rules"}, applied_report).null(),
      "control slot was forced into L1 ablation");
  applied_trial->set_verdict("passed");
  expect(search.experience_ablation(search.seed, selected, applied_report).null(),
         "successful unwasteful injection forced ablation");
  applied_evidence["loop_review"] = Json{{"status", "uncertain"}, {"count", 1}};
  *applied_trial->mutable_evidence() = proto<google::protobuf::Struct>(applied_evidence);
  expect(search.experience_ablation(search.seed, selected, applied_report).null(),
         "unknown loop review justified ablation");
  applied_evidence["loop_review"]["status"] = "complete";
  *applied_trial->mutable_evidence() = proto<google::protobuf::Struct>(applied_evidence);
  expect(!search.experience_ablation(search.seed, selected, applied_report).null(),
         "reviewed waste with injection got no ablation comparison");
  applied_evidence["trace"]["events"].items()[1]["application"]["status"] = "rejected";
  *applied_trial->mutable_evidence() = proto<google::protobuf::Struct>(applied_evidence);
  expect(search.experience_ablation(search.seed, selected, applied_report).null(),
         "unapplied experience was treated as a harmful injection");

  // 弃权后只试一个已评审控制假设；来源关联、编译、评测门禁仍使用现有契约。
  const Json probe_guard{{"kind", "action.end"},
                         {"event_id", "guard"},
                         {"actor_id", "worker"},
                         {"execution_id", "execution"},
                         {"loop_id", "loop"},
                         {"iteration", 2},
                         {"semantic", "action"},
                         {"guard_only", true},
                         {"executed", false},
                         {"operation", "render"},
                         {"action_control", Json{{"decision", "allow"},
                                                 {"query_event_id", "guard-query"},
                                                 {"target_scope", "assistant"},
                                                 {"prior_operation_rounds", 1},
                                                 {"reviewed_budget_prior", 1}}}};
  Json probe_input{
      {"phase", "reflection"},
      {"selected_components", Json::Array{"action_rules"}},
      {"components", search.components(search.seed)},
      {"experiences", Json::Array{Json{{"memory_id", "removal-memory"}, {"scope", "assistant"}}}},
      {"controls",
       Json{{"action_primitives", Json::Array{Json{{"primitive_id", "operation_budget"}}}},
            {"limits", Json{{"min_iterations", 1}}}}}};
  Json probe_trial;
  probe_trial["evidence"]["runs"] =
      Json::Array{Json{{"run_id", "run"}, {"actions", Json::Array{probe_guard}}}};
  probe_input["feedback"] = Json::Array{Json{{"task_id", "train-task"}, {"trial", probe_trial}}};
  const auto seed_before_probe = search.seed.dump(), input_before_probe = probe_input.dump();
  auto probe = search.reviewed_control_probe(search.seed, search.seed, probe_input, Report{}, 0);
  expect(!probe.null() && probe["detail"]["source"].str("association") == "existing_control" &&
             probe["detail"].str("hypothesis_origin") == "training_review",
         "reviewed guard with exact control source got no probe");
  const auto probe_draft = expand_control_updates(probe["draft"], search.components(search.seed),
                                                  Json::Array{"action_rules"});
  const auto probe_artifact = search.mutate(search.seed, probe_draft, Json::Array{"action_rules"});
  const auto probe_rules = search.components(probe_artifact)["action_rules"];
  expect(probe_rules.items().size() == 2 &&
             probe_rules.items().back().str("operation") == "render" &&
             probe_rules.items().back().num("max_attempts", 0) == 1,
         "probe did not compile the reviewed operation or lost the independent existing guard");
  expect(
      search.reviewed_control_probe(search.seed, probe_artifact, probe_input, Report{}, 0).null() &&
          search.reviewed_control_probe(search.seed, ablated, probe_input, Report{}, 0).null(),
      "probe overwrote a substantive control or L1 ablation proposal");
  auto reason_only = search.components(search.seed);
  reason_only["action_rules"].items()[0]["reason"] = "Another explanation";
  const auto reworded = search.compose(search.seed, reason_only);
  expect(!search.reviewed_control_probe(search.seed, reworded, probe_input, Report{}, 0).null(),
         "reason-only metadata prevented exploration of a real reviewed opportunity");
  search.tree["nodes"] = Json::Array{Json{{"control_probe", probe["detail"]}}};
  auto new_request = probe_input;
  new_request["phase"] = "repair_control_binding";
  search.tree = Json::parse(search.tree.dump());
  expect(
      search.reviewed_control_probe(search.seed, search.seed, new_request, Report{}, 1).null(),
      "checkpoint recovery or new request-local binding ID repeated the same control hypothesis");
  search.tree = Json::Object{};
  auto ordinary = probe_input;
  ordinary["feedback"]
      .items()[0]["trial"]["evidence"]["runs"]
      .items()[0]["actions"]
      .items()[0]["action_control"]
      .fields()
      .erase("reviewed_budget_prior");
  expect(
      search.reviewed_control_probe(search.seed, search.seed, ordinary, Report{}, 0).null(),
      "ordinary repeated work without full reviewed redundancy triggered a deterministic budget");
  auto unselected = probe_input;
  unselected["selected_components"] = selected;
  expect(search.reviewed_control_probe(search.seed, search.seed, unselected, Report{}, 0).null(),
         "control probe stole an L1-only component selection");

  // 无现有同操作来源时，只接受同执行同轮且确实交给模型的经验回执。
  auto injection_input = probe_input;
  injection_input["components"]["action_rules"] = Json::Array{};
  const auto prepared_probe = observed_operation_bindings(injection_input, 65536);
  const auto binding = prepared_probe["controls"]["editable_bindings"].items().front();
  auto event = [](const char *kind, const char *id) {
    return Json{{"kind", kind},           {"event_id", id},
                {"actor_id", "worker"},   {"execution_id", "execution"},
                {"loop_id", "loop"},      {"iteration", 2},
                {"step_id", "model-step"}};
  };
  auto raw_anchor = event("action.end", "guard");
  auto model_event = event("action.end", "model");
  model_event["attributes"] = Json{{"semantic", "model"}, {"executed", true}};
  auto query_event = event("custom", "query");
  query_event["attributes"] =
      Json{{"type", "policy.query"},
           {"decision", Json{{"query", Json{{"boundary", "llm"},
                                            {"advice_query", Json{{"target_scope", "assistant"},
                                                                  {"query_id", "llm-query"}}}}}}}};
  auto receipt_event = event("advice.applied", "receipt");
  receipt_event["application"] = Json{{"status", "applied"}, {"query_id", "llm-query"}};
  receipt_event["attributes"] =
      Json{{"receipt", Json{{"boundary", "llm"}, {"experiences", Json::Array{json(ref)}}}}};
  Json source_events = Json::Array{raw_anchor, query_event, model_event, receipt_event};
  auto source_report = [&](const Json &events) {
    Report result;
    auto *trial = result.add_trials();
    trial->set_variant_id("candidate");
    trial->set_task_id("train-task");
    trial->set_execution_status("completed");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}}});
    return result;
  };
  const auto actual_source =
      control_probe_source(search.seed, prepared_probe, binding, source_report(source_events));
  expect(actual_source.str("association") == "same_iteration_application" &&
             actual_source.str("application_event_id") == "receipt" &&
             actual_source.str("model_event_id") == "model",
         "same-iteration actual L1 adoption was not associated with the reviewed guard");
  expect(!search
              .reviewed_control_probe(search.seed, search.seed, injection_input,
                                      source_report(source_events), 0)
              .null(),
         "actual L1 adoption could not supply an explicitly associated control hypothesis");
  expect(control_probe_source(search.seed, prepared_probe, binding, Report{}).null(),
         "arbitrary approved same-scope memory was treated as a control source");
  for (int mode = 0; mode < 9; ++mode) {
    auto wrong = source_events;
    if (mode == 0)
      wrong.items()[3]["execution_id"] = "sibling";
    if (mode == 1)
      wrong.items()[3]["actor_id"] = "other-worker";
    if (mode == 2)
      wrong.items()[3]["iteration"] = 3;
    if (mode == 3)
      wrong.items()[3]["application"]["status"] = "rejected";
    if (mode == 4)
      wrong.items()[2]["attributes"]["executed"] = false;
    if (mode == 5)
      wrong.items()[1]["attributes"]["decision"]["query"]["advice_query"]["target_scope"] =
          "other-scope";
    if (mode == 6)
      wrong.items()[3]["attributes"]["receipt"]["experiences"].items()[0]["content_revision"] =
          "stale";
    if (mode == 7) {
      wrong.items()[1]["attributes"]["decision"]["query"]["advice_query"]["query_id"] = "";
      wrong.items()[3]["application"]["query_id"] = "";
    }
    if (mode == 8)
      for (auto &value : wrong.items())
        value["step_id"] = "";
    expect(control_probe_source(search.seed, prepared_probe, binding, source_report(wrong)).null(),
           "missing adoption or sibling/stale L1 receipt supplied a control source");
  }
  auto unauthorized_parent = search.seed;
  unauthorized_parent["policy"]["options"]["approved_experiences"] = Json::Array{};
  expect(control_probe_source(unauthorized_parent, prepared_probe, binding,
                              source_report(source_events))
             .null(),
         "historical application bypassed current source approval");

  // L3 的自动假设保留必要文本收尾；已评审普通尾部仍须真实来源、必要轮下界和去重。
  auto finalization_options = control_options;
  auto *host_loop = finalization_options.add_loop_rules();
  host_loop->set_rule_id("host-loop");
  host_loop->set_target_scope("assistant");
  host_loop->set_actor_id("worker");
  host_loop->set_max_iterations(8);
  host_loop->set_max_no_progress(2);
  host_loop->set_max_replans(1);
  host_loop->set_no_progress_decision("stop");
  host_loop->set_reason("Host loop safety budget");
  host_loop->set_match_mode("predicate");
  auto finalization_parent = search.seed;
  finalization_parent["policy"] = json(te::compile_runtime_policy(snapshot, finalization_options));
  auto finalization_input = injection_input;
  finalization_input["components"] = search.components(finalization_parent);
  finalization_input["selected_components"] = Json::Array{"loop_rules"};
  finalization_input["controls"]["aggressive_controls"] = true;
  finalization_input["controls"]["loop_primitives"] =
      Json::Array{Json{{"primitive_id", "bounded_iteration_cap"}}};
  finalization_input["controls"]["base_loop_rules"] =
      Json::Array{Json{{"target_scope", "assistant"},
                       {"actor_id", "worker"},
                       {"max_iterations", 8},
                       {"max_no_progress", 2},
                       {"max_replans", 1}}};
  auto &loop_anchor = finalization_input["feedback"]
                          .items()[0]["trial"]["evidence"]["runs"]
                          .items()[0]["actions"]
                          .items()[0];
  loop_anchor["kind"] = "loop.decision";
  loop_anchor["loop_control"] = Json{{"target_scope", "assistant"},
                                     {"iteration", 2},
                                     {"reviewed_waste_after", 1},
                                     {"require_finalization", true},
                                     {"proposed_decision", "continue"}};
  auto finalization_events = source_events;
  finalization_events.items()[0]["kind"] = "loop.decision";
  const auto loop_probe =
      search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
                                    source_report(finalization_events), 0);
  expect(
      !loop_probe.null() &&
          loop_probe["draft"]["control_updates"].items().front()["value"].flag(
              "require_finalization", false) &&
          loop_probe["draft"]["control_updates"].items().front()["value"].str("actor_id") ==
              "worker" &&
          loop_probe["draft"]["control_updates"].items().front()["value"].num("max_iterations",
                                                                              0) == 1,
      "reviewed finalization hypothesis dropped its actor, safe bound or mandatory final response");
  loop_anchor["loop_control"].fields().erase("require_finalization");
  const auto ordinary_loop_probe =
      search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
                                    source_report(finalization_events), 0);
  expect(!ordinary_loop_probe.null() &&
             !ordinary_loop_probe["draft"]["control_updates"].items().front()["value"].flag(
                 "require_finalization", false) &&
             ordinary_loop_probe["detail"].str("key") != loop_probe["detail"].str("key"),
         "reviewed ordinary loop cap was skipped or shared the finalization hypothesis key");
  search.tree["nodes"] = Json::Array{Json{{"control_probe", loop_probe["detail"]}}};
  expect(!search
              .reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
                                      source_report(finalization_events), 1)
              .null(),
         "finalization hypothesis suppressed an independent ordinary cap");
  search.tree["nodes"].push(Json{{"control_probe", ordinary_loop_probe["detail"]}});
  expect(search
             .reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
                                     source_report(finalization_events), 2)
             .null(),
         "ordinary loop cap was probed again after checkpoint recovery");
  search.tree = Json::Object{};
  expect(search
             .reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
                                     Report{}, 0)
             .null(),
         "ordinary loop cap bypassed actual source attribution");
  loop_anchor["loop_control"]["required_iteration_floor"] = 2;
  expect(search
             .reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
                                     source_report(finalization_events), 0)
             .null(),
         "ordinary loop cap truncated a known necessary iteration");
  loop_anchor["loop_control"].fields().erase("required_iteration_floor");
  loop_anchor["loop_control"].fields().erase("reviewed_waste_after");
  expect(search
             .reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
                                     source_report(finalization_events), 0)
             .null(),
         "unreviewed high iteration count justified an ordinary loop probe");
  expect(search.seed.dump() == seed_before_probe && probe_input.dump() == input_before_probe,
         "control probing mutated the parent policy or source request");

  // 两个独立执行者的新增规则只有一条可触发时，保留有证据的子集重新评测。
  auto subset_options = options;
  auto *base_loop = subset_options.add_loop_rules();
  base_loop->set_rule_id("base-progress");
  base_loop->set_target_scope("assistant");
  base_loop->set_max_iterations(12);
  base_loop->set_max_no_progress(3);
  base_loop->set_no_progress_decision("stop");
  base_loop->set_reason("基础进展限制");
  search.controls = Json{{"enabled", true}, {"prompt_file", "unused-in-unit-test"}};
  auto subset_parent = search.seed;
  subset_parent["policy"] = json(te::compile_runtime_policy(snapshot, subset_options));
  search.seed = subset_parent;
  const Json loop_selected = Json::Array{"loop_rules"};
  const auto rule_for = [&](const char *actor, std::size_t threshold) {
    return Json{{"memory_id", "removal-memory"}, {"actor_id", actor},
                {"max_iterations", 12}, {"max_no_progress", threshold},
                {"reason", "限制本执行者连续无进展"}};
  };
  const auto subset_draft = [&](const Json &rules) {
    return Json{{"reason", "独立分支应分别验证"}, {"controls", Json{{"loop_rules", rules}}}};
  };
  const auto mixed = search.mutate(subset_parent, subset_draft(Json::Array{
      rule_for("worker", 1), rule_for("unobserved", 1)}), loop_selected);
  auto subset_query = query("loop");
  subset_query.set_iteration(2);
  subset_query.set_repeated_action_steps(2);
  const auto subset_report = report(subset_parent, subset_query);
  const auto rejected = search.control_preflight(subset_parent, mixed, subset_report);
  expect(!rejected.flag("passed", true) && rejected["missing"].items().size() == 1,
         "未复现无差分规则导致整个组合拒绝");
  const auto retained = search.triggerable_loop_subset(subset_parent, mixed, subset_report, rejected);
  expect(!retained.null() && retained["preflight"].flag("passed", false) &&
             retained["removed_rule_ids"].items().size() == 1 &&
             search.components(retained["candidate"])["loop_rules"].items().size() == 1 &&
             search.components(retained["candidate"])["loop_rules"].items()[0].str("actor_id") == "worker",
         "有真实干预机会的独立循环规则未被保留");
  expect(search.seed.dump() == subset_parent.dump(), "筛选子集修改了原父策略");
  const auto idle_only = search.mutate(subset_parent,
      subset_draft(Json::Array{rule_for("unobserved", 1)}), loop_selected);
  expect(search.triggerable_loop_subset(subset_parent, idle_only, subset_report,
      search.control_preflight(subset_parent, idle_only, subset_report)).null(),
      "删除全部无效新增规则后冒记为有效候选");
  const auto replacement = search.mutate(idle_only, subset_draft(Json::Array{
      rule_for("worker", 1), rule_for("unobserved", 2)}), loop_selected);
  expect(search.triggerable_loop_subset(idle_only, replacement, subset_report,
      search.control_preflight(idle_only, replacement, subset_report)).null(),
      "子集修复删除了父策略已有执行者的约束");
  for (const auto *unsafe : {"known_keep_truncation", "unconditioned_recovery"}) {
    auto check = rejected;
    check[unsafe] = Json::Array{"unsafe-rule"};
    expect(search.triggerable_loop_subset(subset_parent, mixed, subset_report, check).null(),
           "子集修复绕过必要工作或恢复条件检查");
  }
}

// 测试辅助函数：reviewed_operation_budget_bindings。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void reviewed_operation_budget_bindings() {
  auto fixture = [](const std::vector<std::string> &decisions,
                    const std::vector<std::size_t> &priors, int rejected = -1,
                    int unidentified = -1, int mixed = -1) {
    Json trial{{"evidence", Json{{"runs", Json::Array{}}}}};
    Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
    append_reviewed_group(trial, raw, "run-a", "execution", "loop", decisions);
    auto &rows = trial["evidence"]["runs"].items()[0]["actions"].items();
    for (std::size_t i = 0; i < decisions.size(); ++i) {
      auto &action = rows[i * 2 + 1];
      action["operation"] = "render";
      action["guard_only"] = true;
      action["executed"] = false;
      action["action_control"] = Json{{"decision", int(i) == rejected ? "reject" : "allow"},
                                      {"query_event_id", "q-" + std::to_string(i)},
                                      {"target_scope", "assistant"},
                                      {"prior_operation_rounds", priors[i]}};
      if (int(i) == unidentified)
        action.fields().erase("iteration");
    }
    if (mixed >= 0) {
      auto other = rows[mixed * 2 + 1];
      other["operation"] = "independent";
      other["action_control"]["query_event_id"] = "independent-query";
      rows.push_back(other);
    }
    bind_iteration_assessments(trial, raw);
    return trial;
  };
  auto prepared = [](const Json &trial) {
    return observed_operation_bindings(
        Json{{"selected_components", Json::Array{"action_rules"}},
             {"controls",
              Json{{"action_primitives", Json::Array{Json{{"primitive_id", "operation_budget"}}}}}},
             {"feedback", Json::Array{Json{{"trial", trial}}}}},
        65536);
  };
  auto early = fixture({"keep", "waste", "waste"}, {0, 1, 2});
  const auto input = prepared(early);
  const auto &binding = input["controls"]["editable_bindings"].items().front();
  expect(binding.num("prior_operation_rounds", 0) == 1 &&
             binding.flag("reviewed_redundant_suffix", false),
         "typed binding did not prefer the earliest reviewed safe budget");
  expect(input["controls"]["action_primitives"].items()[0]["observed_bindings"].items()[0].num(
             "prior_operation_rounds", 0) == 2,
         "legacy maximum binding changed");
  expect(observed_operation_bindings(input, 65536).dump() == input.dump(),
         "early binding changed on reprojection");
  for (const auto &trial :
       {fixture({"keep", "waste", "keep"}, {0, 1, 2}), fixture({"keep", "waste"}, {1, 1}),
        fixture({"keep", "waste", "unknown"}, {0, 1, 2}),
        fixture({"keep", "waste", "waste"}, {0, 1, 2}, -1, 2)}) {
    const auto result = prepared(trial);
    if (result["controls"]["editable_bindings"].array())
      for (const auto &item : result["controls"]["editable_bindings"].items())
        expect(!item.flag("reviewed_redundant_suffix", false),
               "unsafe or incomplete suffix authorized an early cap");
  }
  const auto rejected = prepared(fixture({"keep", "waste", "waste"}, {0, 1, 2}, 1));
  expect(rejected["controls"]["editable_bindings"].items()[0].num("prior_operation_rounds", 0) == 2,
         "an already rejected guard was treated as a new opportunity");
  auto hidden_keep = fixture({"keep", "waste", "keep"}, {0, 1, 2});
  auto &hidden_rows = hidden_keep["evidence"]["runs"].items()[0]["actions"].items();
  hidden_rows.resize(4);
  expect(!prepared(hidden_keep)["controls"].has("editable_bindings"),
         "display clipping erased the necessary third-attempt floor");
  auto separate = fixture({"keep", "waste"}, {0, 1});
  separate["evidence"]["runs"].push(
      fixture({"keep", "keep"}, {0, 1})["evidence"]["runs"].items()[0]);
  separate["evidence"]["runs"].items()[1]["run_id"] = "run-b";
  expect(!prepared(separate)["controls"].has("editable_bindings"),
         "one run's redundant second attempt overrode another run's necessary second attempt");
  auto later = fixture({"keep", "waste"}, {0, 3});
  later["evidence"]["runs"].push(separate["evidence"]["runs"].items()[1]);
  const auto later_budget = prepared(later)["controls"]["editable_bindings"].items()[0]["budget"];
  expect(
      later_budget.num("min", 0) == 2 && later_budget.num("max", 0) == 3,
      "independent runs had their required attempt counters added instead of taking the maximum");
  auto clipped = prepared(early);
  merge_action_requirements(clipped["controls"]["action_requirements"],
                            fixture({"keep", "keep"}, {0, 1})["evidence"]["action_requirements"]);
  expect(observed_operation_bindings(clipped, 65536)["controls"]["editable_bindings"]
                 .items()[0]["budget"]
                 .num("min", 0) == 2,
         "omitting a feedback row erased its complete necessary-attempt summary");
  clipped["controls"]["action_requirements"].items()[0]["operation"] = "another-operation";
  expect(observed_operation_bindings(clipped, 65536)["controls"].has("editable_bindings"),
         "another operation's requirement blocked the reviewed budget");
  const auto mixed = prepared(fixture({"keep", "keep", "waste"}, {0, 1, 2}, -1, -1, 1));
  expect(mixed["controls"]["editable_bindings"].items()[0]["budget"].num("min", 0) == 1,
         "mixed-iteration keep was falsely assigned to every operation");
  const auto zero = prepared(fixture({"waste"}, {0}));
  expect(!zero["controls"].has("editable_bindings"), "a zero attempt budget was offered");

  // 真实异步派发包含 guard、schedule.action 和委派包装三条记录，实际只做一件事。
  // 第二次派发若是必要补救，不能因这三条记录被误判成混合工作而丢掉预算下界。
  Json dispatched{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}},
           {"trace", Json{{"identity", Json{{"run_id", "run-dispatch"}}},
                          {"events", Json::Array{}}}}};
  append_reviewed_group(dispatched, raw, "run-dispatch", "execution", "loop", {"keep", "keep"});
  auto &dispatch_rows = dispatched["evidence"]["runs"].items()[0]["actions"];
  for (std::size_t i = 0; i < 2; ++i) {
    const auto suffix = std::to_string(i + 1);
    auto guard = dispatch_rows.items()[i * 2 + 1];
    guard["event_id"] = "guard-" + suffix;
    guard["action_id"] = "guard-action-" + suffix;
    guard["operation"] = "render";
    guard["guard_only"] = true;
    guard["executed"] = false;
    guard["action_control"] = Json{{"decision", "allow"}, {"query_event_id", "query-" + suffix},
                                    {"target_scope", "assistant"}, {"prior_operation_rounds", i}};
    guard["attributes"] = Json{{"semantic", "action"}, {"executed", false},
        {"extensions", Json{{"point_reference", Json{{"key", "batch-" + suffix + ":guard:1"}}}}}};
    dispatch_rows.items()[i * 2 + 1] = guard;
    raw["trace"]["events"].push(guard);

    auto settled = guard;
    settled.fields().erase("guard_only");
    settled.fields().erase("action_control");
    settled["event_id"] = "settled-" + suffix;
    settled["action_id"] = "scheduled-action-" + suffix;
    settled["step_id"] = "step-batch-" + suffix;
    settled["executed"] = true;
    settled["attributes"] = Json{{"type", "schedule.action"}, {"executed", true},
                                  {"logical_action_id", "1"}, {"target_scope", "assistant"}};
    dispatch_rows.push(settled);
    raw["trace"]["events"].push(settled);

    auto wrapper = settled;
    wrapper["event_id"] = "wrapper-" + suffix;
    wrapper["action_id"] = "wrapper-action-" + suffix;
    wrapper["operation"] = "delegate";
    wrapper["attributes"] = Json{{"delegates_schedule", true}};
    dispatch_rows.push(wrapper);
    raw["trace"]["events"].push(wrapper);
  }
  auto marked = dispatched;
  bind_iteration_assessments(marked, raw);
  expect(!prepared(marked)["controls"].has("editable_bindings"),
         "前置检查与真实派发被重复计数，必要的第二次派发失去了预算保护");

  // 只有包内显式批次身份、实例身份和作用域都一致时才合并，不按名称或先后顺序猜测。
  for (const auto *mismatch : {"batch", "scope", "operation", "logical_id", "run"}) {
    auto changed = raw;
    auto &settled = changed["trace"]["events"].items()[4];
    if (std::string(mismatch) == "batch") settled["step_id"] = "step-other-batch";
    if (std::string(mismatch) == "scope") settled["attributes"]["target_scope"] = "other-scope";
    if (std::string(mismatch) == "operation") settled["operation"] = "other-operation";
    if (std::string(mismatch) == "logical_id") settled["attributes"].fields().erase("logical_action_id");
    if (std::string(mismatch) == "run") changed["trace"]["identity"]["run_id"] = "other-run";
    auto unknown = dispatched;
    bind_iteration_assessments(unknown, changed);
    expect(prepared(unknown)["controls"].has("editable_bindings"),
           "身份不一致的独立动作被合并并错误归因了整轮必要性");
  }
  auto concurrent = dispatched;
  auto concurrent_raw = raw;
  auto peer = raw["trace"]["events"].items()[4];
  peer["event_id"] = "independent-peer";
  peer["action_id"] = "independent-peer-action";
  peer["attributes"]["logical_action_id"] = "2";
  concurrent["evidence"]["runs"].items()[0]["actions"].push(peer);
  concurrent_raw["trace"]["events"].push(peer);
  bind_iteration_assessments(concurrent, concurrent_raw);
  expect(prepared(concurrent)["controls"].has("editable_bindings"),
         "同名并发分支被合并成单一必要动作");
}

// 测试辅助函数：observed_operation_binding_views。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void observed_operation_binding_views() {
  auto guard = [](const std::string &operation, const std::string &scope, std::size_t rounds) {
    return Json{{"kind", "action.end"},
                {"semantic", "action"},
                {"guard_only", true},
                {"executed", false},
                {"operation", operation},
                {"action_control", Json{{"decision", "allow"},
                                        {"query_event_id", "query-1"},
                                        {"target_scope", scope},
                                        {"prior_operation_rounds", rounds}}}};
  };
  auto row = [](const Json &actions) {
    return Json{
        {"trial", Json{{"evidence", Json{{"runs", Json::Array{Json{{"actions", actions}}}}}}}}};
  };
  const Json input{
      {"selected_components", Json::Array{"action_rules"}},
      {"controls",
       Json{{"action_primitives", Json::Array{Json{{"primitive_id", "terminal_failure"}},
                                              Json{{"primitive_id", "operation_budget"}}}}}},
      {"feedback",
       Json::Array{row(Json::Array{guard("fetch", "scope-a", 1), guard("render", "scope-a", 2)}),
                   row(Json::Array{guard("fetch", "scope-a", 3), guard("fetch", "scope-b", 2)})}}};
  auto bindings = [](const Json &packet) -> const Json & {
    // 无可执行预算时原语会被移除；按身份查找，不能假定它始终位于第二项。
    const auto &primitives = packet["controls"]["action_primitives"];
    if (primitives.array())
      for (const auto &primitive : primitives.items())
        if (primitive.str("primitive_id") == "operation_budget")
          return primitive["observed_bindings"];
    static const Json missing;
    return missing;
  };
  const auto original = input.dump();
  const auto prepared = observed_operation_bindings(input, 65536);
  expect(bindings(prepared).items().size() == 3 && input.dump() == original &&
             prepared["feedback"].dump() == input["feedback"].dump(),
         "binding projection changed input or feedback");
  expect(bindings(prepared).items()[0].str("operation") == "fetch" &&
             bindings(prepared).items()[0].str("target_scope") == "scope-a" &&
             bindings(prepared).items()[0].num("prior_operation_rounds", 0) == 3 &&
             bindings(prepared).items()[0].str("evidence_ref") ==
                 "/feedback/1/trial/evidence/runs/0/actions/0",
         "binding summed independent runs or lost the exact maximum observation");
  expect(observed_operation_bindings(prepared, 65536).dump() == prepared.dump(),
         "binding projection was not idempotent");

  // repair 和 COMBEE map 只保留当前子组；reduce/source repair 无反馈时不能继承旧绑定。
  auto subset = prepared;
  subset["feedback"] = Json::Array{input["feedback"].items()[0]};
  const auto projected_subset = observed_operation_bindings(subset, 65536);
  expect(bindings(projected_subset).items().size() == 2 &&
             bindings(projected_subset).items()[0].str("operation") == "render" &&
             bindings(projected_subset).items()[1].num("prior_operation_rounds", 0) == 1 &&
             bindings(projected_subset).items()[1].str("evidence_ref") ==
                 "/feedback/0/trial/evidence/runs/0/actions/0",
         "child reflection retained another feedback group's operation binding");
  auto no_feedback = prepared;
  no_feedback.fields().erase("feedback");
  const auto without_feedback = observed_operation_bindings(no_feedback, 65536);
  expect(bindings(without_feedback).null() &&
             !without_feedback["controls"].has("editable_bindings") &&
             without_feedback["controls"]["action_primitives"].items().size() == 1 &&
             without_feedback["controls"]["action_primitives"].items()[0].str("primitive_id") ==
                 "terminal_failure",
         "无反馈时必须隐藏预算原语和旧绑定，并保留其它控制能力");
  auto no_actions = prepared;
  no_actions["selected_components"] = Json::Array{"loop_rules"};
  expect(bindings(observed_operation_bindings(no_actions, 65536)).null(),
         "unselected action component gained operation bindings");
  auto unavailable = prepared;
  unavailable["controls"]["action_primitives"].items()[1]["primitive_id"] =
      "repeat_failed_operation";
  expect(bindings(observed_operation_bindings(unavailable, 65536)).null(),
         "missing primitive retained operation budget bindings");
  auto no_controls = input;
  no_controls.fields().erase("controls");
  expect(observed_operation_bindings(no_controls, 65536).dump() == no_controls.dump(),
         "binding helper manufactured controls");

  const auto valid = guard("arbitrary-operation", "scope-a", 1);
  std::vector<Json> invalid;
  auto wrong = valid;
  wrong["executed"] = true;
  invalid.push_back(wrong);
  wrong = valid;
  wrong.fields().erase("executed");
  invalid.push_back(wrong);
  wrong = valid;
  wrong["guard_only"] = false;
  invalid.push_back(wrong);
  wrong = valid;
  wrong["semantic"] = "model";
  invalid.push_back(wrong);
  wrong = valid;
  wrong["kind"] = "action.start";
  invalid.push_back(wrong);
  wrong = valid;
  wrong["action_control"]["decision"] = "reject";
  invalid.push_back(wrong);
  wrong = valid;
  wrong["action_control"]["prior_operation_rounds"] = 0;
  invalid.push_back(wrong);
  wrong = valid;
  wrong["action_control"].fields().erase("query_event_id");
  invalid.push_back(wrong);
  wrong = valid;
  wrong["action_control"]["target_scope"] = "";
  invalid.push_back(wrong);
  wrong = valid;
  wrong["operation"] = "";
  invalid.push_back(wrong);
  wrong = valid;
  wrong["action_control"] = nullptr;
  invalid.push_back(wrong);
  for (const auto &action : invalid) {
    auto packet = prepared;
    packet["feedback"] = Json::Array{row(Json::Array{action})};
    const auto result = observed_operation_bindings(packet, 65536);
    expect(bindings(result).null() && result["feedback"].dump() == packet["feedback"].dump(),
           "non-executable or absent evidence manufactured an operation binding");
  }
  auto crowded = input;
  for (std::size_t i = 0; i < 5; ++i)
    crowded["feedback"].push(
        row(Json::Array{guard("operation-" + std::to_string(i), "scope-a", i + 4)}));
  const auto capped = observed_operation_bindings(crowded, 65536);
  expect(bindings(capped).items().size() == 4 &&
             bindings(capped).items()[0].num("prior_operation_rounds", 0) == 8,
         "operation binding cap or strongest-observation ordering changed");

  // 精确满额仍发送原反馈；只够一条时不加入第二条，也不以旧绑定挤占空间。
  expect(observed_operation_bindings(input, original.size()).dump() == original,
         "full input budget was exceeded or original feedback was discarded");
  auto one = prepared;
  one["controls"]["action_primitives"].items()[1]["observed_bindings"].items().resize(1);
  const auto limited = observed_operation_bindings(prepared, one.dump().size());
  expect(bindings(limited).items().size() == 1 && limited.dump().size() <= one.dump().size() &&
             limited["feedback"].dump() == input["feedback"].dump(),
         "binding projection exceeded the remaining byte budget");
  expect(observed_operation_bindings(prepared, original.size()).dump() == original,
         "stale bindings prevented a full-budget request from remaining usable");
}

// 测试辅助函数：typed_control_binding_updates。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void typed_control_binding_updates() {
  // 空更新是弃权，无需存在绑定；仍拒绝混合协议和没有授权对象的非空修改。
  const Json abstain{{"reason", "没有可验证的收益"}, {"control_binding_updates", Json::Array{}}};
  Json empty_diagnostic{{"reason", "stale"}};
  const auto empty = resolve_control_binding_updates(abstain, Json::Object{}, &empty_diagnostic);
  expect(empty["control_updates"].array() && empty["control_updates"].items().empty() &&
             !empty.has("control_binding_updates") && empty_diagnostic.null(),
         "没有绑定的合法空更新被误判成生成协议错误");
  const auto binding = [](const Json &packet, const std::string &component) {
    for (const auto &item : packet["controls"]["editable_bindings"].items())
      if (item.str("component") == component)
        return item;
    throw std::runtime_error("missing typed control binding");
  };
  const auto update = [](const Json &item, const std::string &memory, const char *field,
                         std::size_t value) {
    return Json{
        {"reason", "Use the observed control point."},
        {"control_binding_updates", Json::Array{Json{{"binding_id", item["binding_id"]},
                                                     {"memory_id", memory},
                                                     {field, value},
                                                     {"reason", "Bound observed waste."}}}}};
  };
  const auto invalid = [](const Json &draft, const Json &packet) {
    try {
      resolve_control_binding_updates(draft, packet);
    } catch (const Error &error) {
      return std::string(error.what()) == "control_binding_invalid";
    }
    return false;
  };
  const auto diagnose = [](const Json &draft, const Json &packet) {
    Json diagnostic;
    try {
      resolve_control_binding_updates(draft, packet, &diagnostic);
    } catch (const Error &error) {
      expect(std::string(error.what()) == "control_binding_invalid",
             "binding diagnostics changed the public error code");
      expect(diagnostic.object() && diagnostic.has("reason") && diagnostic.has("update_index"),
             "binding error lacked bounded repair diagnostics");
      expect(diagnostic.dump().find("private-marker") == std::string::npos,
             "binding diagnostics leaked a model ID, field name or reason");
      return diagnostic;
    }
    throw std::runtime_error("diagnostic fixture did not fail");
  };
  const auto row = [](Json actions) {
    return Json{
        {"trial",
         Json{{"evidence", Json{{"runs", Json::Array{Json{{"actions", std::move(actions)}}}}}}}}};
  };
  const Json guard{{"kind", "action.end"},
                   {"semantic", "action"},
                   {"guard_only", true},
                   {"executed", false},
                   {"operation", "forge-image-gen"},
                   {"action_control", Json{{"decision", "allow"},
                                           {"query_event_id", "query-1"},
                                           {"target_scope", "scope-a"},
                                           {"prior_operation_rounds", 2}}}};
  Json action_input{
      {"phase", "reflection"},
      {"selected_components", Json::Array{"action_rules"}},
      {"components", Json{{"action_rules", Json::Array{}}}},
      {"experiences", Json::Array{Json{{"memory_id", "wrong-scope"}, {"scope", "scope-b"}}}},
      {"controls",
       Json{{"action_primitives", Json::Array{Json{{"primitive_id", "operation_budget"}}}},
            {"control_sources",
             Json::Array{Json{{"memory_id", "control-memory"}, {"scope", "scope-a"}}}},
            {"limits", Json{{"min_iterations", 1}}}}},
      {"feedback", Json::Array{row(Json::Array{guard})}}};
  auto prepared = observed_operation_bindings(action_input, 65536);
  auto action = binding(prepared, "action_rules");
  expect(action.str("operation") == "forge-image-gen" &&
             action.num("prior_operation_rounds", 0) == 2 &&
             action.str("binding_id").find("control:") == 0,
         "action binding lost its trusted operation identity");
  expect(action["budget"].dump() ==
             Json({{"field", "max_attempts"}, {"min", 1}, {"max", 2}}).dump(),
         "request did not expose the exact legal operation budget");
  auto draft = update(action, "control-memory", "max_attempts", 2);
  auto resolved = resolve_control_binding_updates(draft, prepared);
  const auto &action_change = resolved["control_updates"].items().front();
  expect(!resolved.has("control_binding_updates") &&
             action_change.str("component") == "action_rules" && action_change.str("op") == "add" &&
             action_change["value"].str("operation") == "forge-image-gen" &&
             action_change["value"].num("max_attempts", 0) == 2 &&
             action_change["value"]["attempt_key_arguments"].items().empty(),
         "typed action binding did not derive the concrete operation budget");

  // 同名失败护栏必须引用一条完整的结构化失败事实；成功空结果不能生成绑定。
  const auto repeated_failure = [](bool failed) {
    return Json{{"kind", "action.end"},
                {"semantic", "action"},
                {"executed", true},
                {"observed_failure", failed},
                {"status", "completed"},
                {"operation", "web-search"},
                {"input", Json{{"action", "query"}, {"city", "Beijing"}}},
                {"action_control", Json{{"decision", "allow"},
                                         {"query_event_id", "query-repeat"},
                                         {"target_scope", "scope-a"},
                                         {"prior_failures", 2}}}};
  };
  auto repeat_input = action_input;
  repeat_input["controls"]["action_primitives"] =
      Json::Array{Json{{"primitive_id", "repeat_failed_operation"}}};
  repeat_input["feedback"] = Json::Array{Json{
      {"task_id", "task-repeat"},
      {"trial", Json{{"evidence", Json{{"runs", Json::Array{Json{
                                         {"actions", Json::Array{repeated_failure(true)}}}}}}}}}}};
  auto repeat_prepared = observed_operation_bindings(repeat_input, 65536);
  const auto repeat = binding(repeat_prepared, "action_rules");
  expect(repeat.str("primitive_id") == "repeat_failed_operation" &&
             repeat.str("task_id") == "task-repeat" &&
             repeat.str("operation") == "web-search" &&
             repeat["arguments"].str("action") == "query" &&
             repeat.flag("observed_failure", false) && repeat.num("prior_failures", 0) == 2 &&
             !repeat.str("evidence_ref").empty(),
         "repeat failure binding lost typed task, operation, action or failure provenance");
  Json repeat_provenance;
  auto repeat_resolved = resolve_control_binding_updates(
      Json{{"reason", "Stop only the cited repeated failure."},
           {"control_binding_updates",
            Json::Array{Json{{"binding_id", repeat["binding_id"]},
                             {"memory_id", "control-memory"},
                             {"reason", "The same action failed after two settled failures."}}}}},
      repeat_prepared, nullptr, &repeat_provenance);
  const auto &repeat_value = repeat_resolved["control_updates"].items().front()["value"];
  expect(repeat_value.str("primitive_id") == "repeat_failed_operation" &&
             repeat_value.str("operation") == "web-search" &&
             repeat_value.num("max_failed_attempts", 0) == 2 &&
             repeat_provenance.items().size() == 1 &&
             repeat_provenance.items().front().str("task_id") == "task-repeat",
         "typed repeat binding did not derive a precise rule and auditable provenance");
  auto successful_input = repeat_input;
  successful_input["feedback"].items().front()["trial"]["evidence"]["runs"].items().front()
      ["actions"] = Json::Array{repeated_failure(false)};
  expect(!observed_operation_bindings(successful_input, 65536)["controls"]
              .has("editable_bindings"),
         "a successful empty/not_found result created repeat-failure evidence");
  Json direct_repeat{{"reason", "unbound"},
                     {"control_updates",
                      Json::Array{Json{{"component", "action_rules"},
                                       {"op", "add"},
                                       {"value", repeat_value}}}}};
  Json direct_diagnostic;
  bool direct_rejected = false;
  try {
    require_typed_budget_updates(direct_repeat, repeat_prepared, &direct_diagnostic);
  } catch (const Error &error) {
    direct_rejected = std::string(error.what()) == "control_binding_invalid";
  }
  expect(direct_rejected && direct_diagnostic.str("reason") == "typed_binding_required",
         "a free-form repeat rule bypassed its typed failure binding");

  // 纠正请求只拿到固定码、下标和可信范围，不回显模型输出中的任意字段。
  auto diagnostic_draft = draft;
  diagnostic_draft["control_binding_updates"].items()[0]["binding_id"] = "private-marker-id";
  auto diagnostic = diagnose(diagnostic_draft, prepared);
  expect(diagnostic.str("reason") == "unknown_binding_id" &&
             diagnostic.num("update_index", 99) == 0 && diagnostic.fields().size() == 2,
         "unknown binding diagnostic included unrelated or raw fields");
  auto stale_packet = prepared;
  stale_packet["phase"] = "changed";
  expect(diagnose(draft, stale_packet).str("reason") == "stale_binding_id",
         "stale binding context was not distinguished");
  diagnostic_draft = draft;
  diagnostic_draft["control_binding_updates"].items()[0]["private-marker-field"] =
      "private-marker-value";
  expect(diagnose(diagnostic_draft, prepared).str("reason") == "unknown_update_field",
         "unknown model field was not diagnosed safely");
  diagnostic_draft = draft;
  diagnostic_draft["control_updates"] = Json::Array{};
  expect(diagnose(diagnostic_draft, prepared).str("reason") == "mixed_protocol",
         "mixed protocols were not diagnosed");
  diagnostic_draft = draft;
  diagnostic_draft["control_binding_updates"].items()[0]["memory_id"] = "private-marker-source";
  expect(diagnose(diagnostic_draft, prepared).str("reason") == "source_missing_or_ambiguous",
         "missing source was not distinguished");
  expect(diagnose(update(action, "wrong-scope", "max_attempts", 2), prepared).str("reason") ==
             "source_scope_mismatch",
         "source scope mismatch was hidden behind a generic failure");
  diagnostic_draft = draft;
  diagnostic_draft["control_binding_updates"].push(
      diagnostic_draft["control_binding_updates"].items().front());
  diagnostic = diagnose(diagnostic_draft, prepared);
  expect(diagnostic.str("reason") == "duplicate_binding_update" &&
             diagnostic.num("update_index", 99) == 1,
         "diagnostic did not identify the failing update index");
  for (const auto &bad : {Json(3), Json(-1), Json(10001)}) {
    diagnostic_draft = draft;
    diagnostic_draft["control_binding_updates"].items()[0]["max_attempts"] = bad;
    diagnostic_draft["control_binding_updates"].items()[0]["reason"] = "private-marker-reason";
    diagnostic = diagnose(diagnostic_draft, prepared);
    expect(diagnostic.str("reason") == "budget_out_of_range" &&
               diagnostic["budget"].dump() == action["budget"].dump(),
           "range repair did not receive the same trusted limits as the original binding");
  }
  for (const auto &bad : {Json(1.5), Json("private-marker-number")}) {
    diagnostic_draft = draft;
    diagnostic_draft["control_binding_updates"].items()[0]["max_attempts"] = bad;
    expect(diagnose(diagnostic_draft, prepared).str("reason") == "budget_not_integer",
           "noninteger budget was not diagnosed");
  }
  diagnostic = Json{{"stale", "private-marker-old-diagnostic"}};
  resolve_control_binding_updates(draft, prepared, &diagnostic);
  expect(diagnostic.null(), "a valid retry retained the prior error diagnostic");

  auto wrong = draft;
  wrong["control_binding_updates"].items()[0]["operation"] = "run_skills";
  expect(invalid(wrong, prepared), "model-supplied operation bypassed typed action identity");
  wrong = update(action, "control-memory", "max_attempts", 3);
  expect(invalid(wrong, prepared),
         "an action budget that cannot trigger in the observed guard was accepted");
  wrong = update(action, "wrong-scope", "max_attempts", 2);
  expect(invalid(wrong, prepared), "a binding was moved to another approved source scope");
  auto repair_input = action_input;
  repair_input["phase"] = "repair_control_binding";
  auto repaired = observed_operation_bindings(repair_input, 65536);
  expect(binding(repaired, "action_rules").str("binding_id") != action.str("binding_id") &&
             invalid(draft, repaired),
         "a request-local binding survived a repair phase change");

  auto existing_action = action_input;
  existing_action["components"]["action_rules"] =
      Json::Array{Json{{"memory_id", "control-memory"},
                       {"operation", "forge-image-gen"},
                       {"max_attempts", 2},
                       {"attempt_key_arguments", Json::Array{"target"}},
                       {"required_arguments", Json::Array{}},
                       {"max_failed_attempts", 0},
                       {"reason", "old"},
                       {"match_mode", "predicate"}}};
  auto existing_prepared = observed_operation_bindings(existing_action, 65536);
  auto tightened = resolve_control_binding_updates(
      update(binding(existing_prepared, "action_rules"), "control-memory", "max_attempts", 1),
      existing_prepared);
  const auto &tightened_action = tightened["control_updates"].items().front();
  expect(tightened_action.str("op") == "set" && tightened_action.num("index", 99) == 0 &&
             tightened_action["value"]["attempt_key_arguments"].items().front().text() == "target",
         "action set discarded the existing target identity");
  existing_action["components"]["action_rules"].items()[0]["max_attempts"] = 1;
  existing_prepared = observed_operation_bindings(existing_action, 65536);
  expect(binding(existing_prepared, "action_rules")["budget"].num("max", 0) == 1,
         "published budget ignored the stricter parent rule");
  expect(invalid(update(binding(existing_prepared, "action_rules"), "control-memory",
                        "max_attempts", 2),
                 existing_prepared),
         "typed action update relaxed an existing parent budget");

  const Json keep{{"kind", "loop.decision"},
                  {"actor_id", "outfit-stylist"},
                  {"execution_id", "required"},
                  {"iteration_assessment", Json{{"decision", "keep"}}},
                  {"loop_control", Json{{"target_scope", "scope-a"},
                                        {"pending_actions", false},
                                        {"proposed_decision", "continue"},
                                        {"iteration", 2}}}};
  const Json waste{{"kind", "loop.decision"},
                   {"actor_id", "outfit-stylist"},
                   {"execution_id", "redundant"},
                   {"iteration_assessment", Json{{"decision", "waste"}}},
                   {"loop_control", Json{{"target_scope", "scope-a"},
                                         {"pending_actions", false},
                                         {"proposed_decision", "continue"},
                                         {"iteration", 7},
                                         {"reviewed_waste_after", 0}}}};
  Json loop_input{
      {"phase", "reflection"},
      {"selected_components", Json::Array{"loop_rules"}},
      {"components", Json{{"loop_rules", Json::Array{}}}},
      {"experiences", Json::Array{}},
      {"controls",
       Json{{"aggressive_controls", true},
            {"loop_primitives", Json::Array{Json{{"primitive_id", "bounded_iteration_cap"}}}},
            {"control_sources",
             Json::Array{Json{{"memory_id", "control-memory"}, {"scope", "scope-a"}}}},
            {"base_loop_rules", Json::Array{Json{{"target_scope", "scope-a"},
                                                 {"actor_id", ""},
                                                 {"max_iterations", 12},
                                                 {"max_no_progress", 1},
                                                 {"max_replans", 1},
                                                 {"replan_on_failure", false},
                                                 {"no_progress_decision", "replan"}}}},
            {"limits", Json{{"min_iterations", 1}}}}},
      {"feedback", Json::Array{row(Json::Array{keep, waste})}}};
  prepared = observed_operation_bindings(loop_input, 65536);
  auto loop = binding(prepared, "loop_rules");
  expect(loop["budget"].str("field") == "max_iterations" && loop["budget"].num("max", 0) == 6,
         "loop binding failed to exclude the first nontriggering budget");
  expect(loop["budget"].num("current", 0) == 12, "loop binding hid the effective parent budget");
  auto narrow_loop = loop_input;
  auto &narrow_actions =
      narrow_loop["feedback"].items()[0]["trial"]["evidence"]["runs"].items()[0]["actions"].items();
  narrow_actions[0]["loop_control"]["iteration"] = 3;
  narrow_actions[1]["loop_control"]["iteration"] = 4;
  auto narrow_prepared = observed_operation_bindings(narrow_loop, 65536);
  const auto narrow_budget = binding(narrow_prepared, "loop_rules")["budget"];
  expect(narrow_budget.num("current", 0) == 12 && narrow_budget.num("min", 0) == 3 &&
             narrow_budget.num("max", 0) == 3,
         "singleton candidate range was confused with the current parent budget");
  narrow_loop["components"]["loop_rules"] = Json::Array{Json{{"memory_id", "control-memory"},
                                                             {"actor_id", "other-worker"},
                                                             {"max_iterations", 9},
                                                             {"match_mode", "predicate"}}};
  expect(binding(observed_operation_bindings(narrow_loop, 65536), "loop_rules")["budget"].num(
             "current", 0) == 12,
         "another actor's loop rule replaced the inherited current budget");
  auto &override = narrow_loop["components"]["loop_rules"].items()[0];
  override["actor_id"] = "outfit-stylist";
  expect(binding(observed_operation_bindings(narrow_loop, 65536), "loop_rules")["budget"].num(
             "current", 0) == 9,
         "same-actor unconditional override was omitted from the current budget");
  override["error_codes"] = Json::Array{"TransientFailure"};
  expect(!binding(observed_operation_bindings(narrow_loop, 65536), "loop_rules")["budget"].has(
             "current"),
         "conditional override was presented as an unconditional current budget");
  narrow_loop["components"]["loop_rules"] = Json::Array{};
  narrow_loop["controls"]["base_loop_rules"].items()[0]["after_action_rejection"] = true;
  expect(!binding(observed_operation_bindings(narrow_loop, 65536), "loop_rules")["budget"].has(
             "current"),
         "conditional base rule was presented as an unconditional current budget");
  auto safe_floor = loop;
  safe_floor["required_iteration_floor"] = 4;
  expect(control_binding_budget(prepared, safe_floor).num("min", 0) == 4,
         "trusted necessary-work floor was omitted from the advertised and validated budget");
  safe_floor["require_finalization"] = true;
  expect(control_binding_budget(prepared, safe_floor).num("min", 0) == 4,
         "finalization flag alone erased a verified tool-work floor");
  expect(loop.str("actor_id") == "outfit-stylist" && loop.num("observed_iteration", 0) == 7 &&
             loop.has("reviewed_waste_after") && loop.num("reviewed_waste_after", 99) == 0 &&
             loop.num("highest_reviewed_keep_iteration", 0) == 2,
         "loop binding lost actor, reviewed suffix or quality context");
  resolved = resolve_control_binding_updates(update(loop, "control-memory", "max_iterations", 2),
                                             prepared);
  const auto &loop_change = resolved["control_updates"].items().front();
  expect(loop_change.str("component") == "loop_rules" && loop_change.str("op") == "add" &&
             loop_change["value"].str("actor_id") == "outfit-stylist" &&
             loop_change["value"].num("max_iterations", 0) == 2 &&
             loop_change["value"].num("max_no_progress", 0) == 1 &&
             loop_change["value"].num("max_replans", 0) == 1,
         "typed loop binding did not inherit the effective base rule");
  expect(invalid(update(loop, "control-memory", "max_iterations", 1), prepared),
         "typed cap ignored necessary work in another same-actor execution");
  auto finalization_input = loop_input;
  finalization_input["controls"]["aggressive_controls"] = false;
  finalization_input["feedback"]
      .items()[0]["trial"]["evidence"]["runs"]
      .items()[0]["actions"]
      .items()[1]["loop_control"]["require_finalization"] = true;
  finalization_input["feedback"]
      .items()[0]["trial"]["evidence"]["runs"]
      .items()[0]["actions"]
      .items()[1]["loop_control"]["can_finalize"] = true;
  auto finalization_prepared = observed_operation_bindings(finalization_input, 65536);
  auto finalization_binding = binding(finalization_prepared, "loop_rules");
  expect(finalization_binding.flag("require_finalization", false),
         "typed binding lost its finalization requirement");
  auto finalization_resolved = resolve_control_binding_updates(
      update(finalization_binding, "control-memory", "max_iterations", 2), finalization_prepared);
  expect(finalization_resolved["control_updates"].items()[0]["value"].flag("require_finalization",
                                                                           false),
         "typed resolver dropped the host finalization capability gate");
  expect(invalid(update(loop, "control-memory", "max_iterations", 0), prepared) &&
             invalid(update(loop, "control-memory", "max_iterations", 7), prepared),
         "typed loop binding accepted an out-of-range or non-triggering cap");

  auto existing_loop = loop_input;
  const Json old_loop{{"memory_id", "control-memory"},
                      {"actor_id", "outfit-stylist"},
                      {"max_iterations", 5},
                      {"max_no_progress", 1},
                      {"max_replans", 1},
                      {"replan_on_failure", false},
                      {"reason", "old"},
                      {"match_mode", "predicate"}};
  existing_loop["components"]["loop_rules"] = Json::Array{old_loop};
  existing_prepared = observed_operation_bindings(existing_loop, 65536);
  expect(binding(existing_prepared, "loop_rules")["budget"].num("max", 0) == 5,
         "loop binding ignored the existing parent cap");
  tightened = resolve_control_binding_updates(
      update(binding(existing_prepared, "loop_rules"), "control-memory", "max_iterations", 2),
      existing_prepared);
  expect(tightened["control_updates"].items().front().str("op") == "set" &&
             tightened["control_updates"].items().front()["value"].num("max_no_progress", 0) == 1,
         "loop set did not preserve the existing rule");
  auto impossible_loop = existing_loop;
  impossible_loop["components"]["loop_rules"].items()[0]["max_iterations"] = 1;
  expect(!observed_operation_bindings(impossible_loop, 65536)["controls"].has("editable_bindings"),
         "a parent cap below the necessary-work floor exposed an unsatisfiable binding");
  // 删除不可用的 loop 原语会改变整个 request-local binding 身份；混合组件中仍可用的
  // schedule binding 必须基于最终发送包重算，不能沿用删原语前的旧 ID。
  auto mixed_loop = impossible_loop;
  mixed_loop["selected_components"] = Json::Array{"loop_rules", "schedule_rules"};
  mixed_loop["components"]["schedule_rules"] = Json::Array{};
  mixed_loop["controls"]["loop_primitives"].push(
      Json{{"primitive_id", "bounded_no_progress"}});
  mixed_loop["feedback"]
      .items()[0]["trial"]["evidence"]["runs"]
      .items()[0]["schedule_plans"] = Json::Array{Json{
          {"target_scope", "scope-a"},
          {"task_text", "prepare then deliver"},
          {"schedule_contract", Json{{"order_editable", true},
                                      {"ordering_required", true},
                                      {"require_success", false}}},
          {"actions", Json::Array{Json{{"action_id", "prepare"},
                                        {"operation", "prepare"},
                                        {"after", Json::Array{}}},
                                  Json{{"action_id", "deliver"},
                                       {"operation", "deliver"},
                                       {"after", Json::Array{}}}}}}};
  const auto mixed_prepared = observed_operation_bindings(mixed_loop, 65536);
  const auto &mixed_primitives = mixed_prepared["controls"]["loop_primitives"];
  const auto &mixed_bindings = mixed_prepared["controls"]["editable_bindings"];
  expect(mixed_primitives.items().size() == 1 &&
             mixed_primitives.items()[0].str("primitive_id") == "bounded_no_progress" &&
             mixed_bindings.array() && !mixed_bindings.items().empty(),
         "an unavailable mixed iteration cap remained visible or removed another binding");
  for (const auto &item : mixed_bindings.items())
    expect(item.str("component") == "schedule_rules" &&
               item.str("binding_id") == control_binding_id(mixed_prepared, item),
           "removing an unavailable loop primitive left a stale mixed-component binding ID");
  existing_loop["components"]["loop_rules"].push(old_loop);
  existing_prepared = observed_operation_bindings(existing_loop, 65536);
  expect(invalid(update(binding(existing_prepared, "loop_rules"), "control-memory",
                        "max_iterations", 2),
                 existing_prepared),
         "ambiguous same-actor loop rules were silently overwritten");
  Search evidence_search;
  evidence_search.controls = Json{{"min_iterations", 1}, {"min_no_progress", 1}};
  expect(evidence_search.control_evidence(loop_input, "loop_rules"),
         "an executable reviewed cap disappeared from control routing");
  auto unavailable_loop = loop_input;
  unavailable_loop["controls"].fields().erase("loop_primitives");
  expect(!evidence_search.control_evidence(unavailable_loop, "loop_rules"),
         "cap-only evidence without a typed binding was routed to reflection");

  const Json direct_cap{
      {"reason", "legacy direct cap"},
      {"control_updates",
       Json::Array{Json{{"component", "loop_rules"},
                        {"op", "add"},
                        {"value",
                         Json{{"memory_id", "control-memory"},
                              {"actor_id", "outfit-stylist"},
                              {"max_iterations", 2},
                              {"max_no_progress", 1},
                              {"max_replans", 1},
                              {"replan_on_failure", false},
                              {"match_mode", "predicate"}}}}}}};
  const auto rejected_budget = [&](const Json &packet) {
    Json reason;
    try {
      require_typed_budget_updates(direct_cap, packet, &reason);
    } catch (const Error &error) {
      expect(std::string(error.what()) == "control_binding_invalid",
             "direct budget bypass changed its public error code");
      return reason;
    }
    throw std::runtime_error("direct budget bypass was accepted");
  };
  expect(rejected_budget(unavailable_loop).str("reason") == "budget_binding_unavailable",
         "direct cap without an executable binding was not rejected precisely");
  expect(rejected_budget(prepared).str("reason") == "typed_binding_required",
         "direct cap bypassed an available typed binding");
  // 无关 binding 不能把带 actor + 收尾保护的普通收敛规则误判成 typed budget
  // 绕过；但同目标一旦存在已评审轮次 binding，仍必须引用该 binding。
  auto unrelated_binding = unavailable_loop;
  unrelated_binding["controls"]["editable_bindings"] = Json::Array{Json{
      {"component", "action_rules"},
      {"primitive_id", "operation_budget"},
      {"target_scope", "scope-a"},
      {"operation", "knowledge-base"}}};
  auto safe_convergence = direct_cap;
  auto &safe_value = safe_convergence["control_updates"].items()[0]["value"];
  safe_value["actor_id"] = "coordinator";
  safe_value["require_finalization"] = true;
  safe_value["max_iterations"] = 4;
  safe_value["max_no_progress"] = 3;
  require_typed_budget_updates(safe_convergence, unrelated_binding);
  expect(rejected_budget(unrelated_binding).str("reason") == "budget_binding_unavailable",
         "an unrelated binding allowed an unscoped cap without finalization protection");

  auto matching_convergence = safe_convergence;
  auto &matching_value = matching_convergence["control_updates"].items()[0]["value"];
  matching_value["actor_id"] = "outfit-stylist";
  Json matching_reason;
  try {
    require_typed_budget_updates(matching_convergence, prepared, &matching_reason);
    throw std::runtime_error("matching typed loop evidence accepted a concrete cap");
  } catch (const Error &error) {
    expect(std::string(error.what()) == "control_binding_invalid" &&
               matching_reason.str("reason") == "typed_binding_required",
           "matching typed loop evidence lost its precise rejection");
  }
  auto preserved_budget = direct_cap;
  preserved_budget["control_updates"].items()[0]["value"]["max_iterations"] = 12;
  require_typed_budget_updates(preserved_budget, unavailable_loop);

  const Json concrete{
      {"reason", "legacy concrete"},
      {"control_updates",
       Json::Array{Json{{"component", "action_rules"}, {"op", "remove"}, {"index", 0}}}}};
  expect(resolve_control_binding_updates(concrete, prepared).dump() == concrete.dump(),
         "legacy concrete and remove drafts lost compatibility");
  diagnostic = Json{{"reason", "private-marker-old-error"}};
  expect(resolve_control_binding_updates(concrete, prepared, &diagnostic).dump() ==
                 concrete.dump() &&
             diagnostic.null(),
         "optional diagnostics changed legacy concrete drafts or retained stale state");
}

// 测试辅助函数：reflection_component_views。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void reflection_component_views() {
  const Json original{
      {"selected_components",
       Json::Array{"action_rules", "loop_rules", "schedule_rules", "experience:a", "experience:b"}},
      {"components", Json{{"action_rules", Json::Array{}},
                          {"loop_rules", Json::Array{}},
                          {"schedule_rules", Json::Array{}},
                          {"experience:a", "original a"},
                          {"experience:b", "original b"}}},
      {"experiences", Json::Array{Json{{"memory_id", "a"}, {"content", "original a"}},
                                  Json{{"memory_id", "b"}, {"content", "original b"}}}},
      {"controls", Json{{"action_primitives", Json::Array{"operation_budget"}},
                        {"loop_primitives", Json::Array{"progress_stop"}},
                        {"loop_conditions", Json{{"error_codes", "exact"}}},
                        {"loop_requirements", Json{{"minimum", 2}}},
                        {"schedule_primitives", Json::Array{"resource_order"}},
                        {"base_action_rules", Json::Array{"action"}},
                        {"base_loop_rules", Json::Array{"loop"}},
                        {"base_schedule_rules", Json::Array{"schedule"}},
                        {"limits", Json{{"max_rules", 8}}}}},
      {"constraints", Json::Array{"stay in scope"}},
      {"feedback", Json::Array{"training evidence"}}};
  auto all = original;
  restrict_reflection_components(all, original["selected_components"]);
  expect(all.dump() == original.dump(), "selecting all components changed their request view");
  auto schedule = original;
  restrict_reflection_components(schedule, Json::Array{"schedule_rules"});
  expect(schedule["components"].fields().size() == 1 &&
             schedule["controls"].has("schedule_primitives") &&
             schedule["controls"].has("base_schedule_rules") &&
             !schedule["controls"].has("action_primitives") &&
             !schedule["controls"].has("loop_primitives") &&
             !schedule["controls"].has("base_action_rules") &&
             !schedule["controls"].has("base_loop_rules") &&
             !schedule["controls"].has("loop_conditions") &&
             !schedule["controls"].has("loop_requirements") &&
             schedule["controls"]["limits"].dump() == original["controls"]["limits"].dump() &&
             schedule["experiences"].dump() == original["experiences"].dump(),
         "schedule-only view exposed unauthorized components or lost source/limits");
  auto experience = original;
  restrict_reflection_components(experience, Json::Array{"experience:b"});
  expect(!experience.has("controls") && experience["components"].fields().size() == 1 &&
             experience["experiences"].items().size() == 1 &&
             experience["experiences"].items()[0].str("content") == "original b" &&
             experience["constraints"].dump() == original["constraints"].dump() &&
             experience["feedback"].dump() == original["feedback"].dump(),
         "L1-only view lost selected original content or changed feedback/constraints");

  // 实际混合 packet 的正文只在 components 中；COMBEE 控制子请求必须搬移当前值。
  auto compact = original;
  compact["components"]["experience:a"] = "current a";
  compact["components"]["experience:b"] = Json{{"content", "current b"},
                                               {"trigger_pattern", "current trigger"},
                                               {"observed_operations", Json::Array{"operation_b"}}};
  compact["components"]["experience:disabled"] = nullptr;
  compact["experiences"] =
      Json::Array{Json{{"memory_id", "a"}, {"original_content", "historical a"}},
                  Json{{"memory_id", "b"}, {"original_content", "historical b"}},
                  Json{{"memory_id", "disabled"},
                       {"content", "stale disabled body"},
                       {"original_content", "historical disabled body"}}};
  const auto compact_before = compact.dump();
  auto control_map = compact;
  restrict_reflection_components(control_map, Json::Array{"schedule_rules"});
  expect(control_map["components"].fields().size() == 1 &&
             control_map["experiences"].items()[0].str("content") == "current a" &&
             control_map["experiences"].items()[1].str("content") == "current b" &&
             control_map["experiences"].items()[1].str("original_content") == "historical b" &&
             !control_map["experiences"].items()[2].has("content") &&
             control_map["experiences"].items()[2].str("original_content") ==
                 "historical disabled body",
         "control map lost current source content or restored disabled/historical content");
  const auto mapped = control_map.dump();
  restrict_reflection_components(control_map, Json::Array{"schedule_rules"});
  expect(control_map.dump() == mapped,
         "repeated control projection changed recovered source content");
  auto experience_map = compact;
  restrict_reflection_components(experience_map, Json::Array{"experience:b"});
  expect(!experience_map.has("controls") && experience_map["components"].fields().size() == 1 &&
             experience_map["components"]["experience:b"].dump() ==
                 compact["components"]["experience:b"].dump() &&
             experience_map["experiences"].items().size() == 1 &&
             experience_map["experiences"].items()[0].str("memory_id") == "b" &&
             !experience_map["experiences"].items()[0].has("content") &&
             experience_map["experiences"].items()[0].str("original_content") == "historical b" &&
             compact.dump() == compact_before,
         "L1 map changed component/history or mutated its parent packet");
}

// 测试辅助函数：repair_feedback_keeps_redispatch_opportunity。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void repair_feedback_keeps_redispatch_opportunity() {
  Search search;
  const Json selected = Json::Array{"action_rules", "loop_rules"};
  auto row = [](const char *task, const char *verdict, const Json &actions) {
    return Json{
        {"task_id", task},
        {"task_input", "Retain the original task contract."},
        {"trial",
         Json{{"verdict", verdict},
              {"evidence",
               Json{{"runs", Json::Array{Json{{"run_id", task}, {"actions", actions}}}}}}}}};
  };
  const Json controlled =
      row("earlier-failed", "failed",
          Json::Array{
              Json{{"kind", "action.end"},
                   {"semantic", "action"},
                   {"actor_id", "other-worker"},
                   {"execution_id", "other-branch"},
                   {"operation", "already-blocked"},
                   {"guard_only", true},
                   {"executed", false},
                   {"action_control", Json{{"decision", "reject"}, {"prior_operation_rounds", 2}}}},
              Json{{"kind", "loop.decision"},
                   {"actor_id", "other-worker"},
                   {"execution_id", "other-branch"},
                   {"loop_control", Json{{"pending_actions", false},
                                         {"proposed_decision", "replan"},
                                         {"observed_no_progress", 1}}}}});
  const Json guard{
      {"kind", "action.end"},
      {"semantic", "action"},
      {"actor_id", "dispatcher"},
      {"execution_id", "dispatch-loop"},
      {"operation", "arbitrary-operation"},
      {"guard_only", true},
      {"executed", false},
      {"action_control",
       Json{{"decision", "allow"}, {"prior_operation_rounds", 1}, {"prior_failures", 0}}}};
  auto packet = [&](const Json &action) {
    return Json{{"feedback",
                 Json::Array{controlled, row("later-opportunity", "passed", Json::Array{action})}}};
  };
  const auto input = packet(guard);
  const auto original = input.dump();
  const auto *chosen = search.repair_feedback_row(input, selected);
  expect(chosen == &input["feedback"].items()[1] && chosen->str("task_id") == "later-opportunity",
         "earlier controlled failure displaced the only allow redispatch opportunity");
  expect(input.dump() == original, "repair selection changed the task contract or evidence");
  expect(search.repair_feedback_row(input, Json::Array{"action_rules"}) == chosen,
         "dispatch opportunity required an unrelated loop component");
  expect(search.repair_feedback_row(input, Json::Array{"loop_rules"}) ==
             &input["feedback"].items()[0],
         "action-only evidence changed loop-only repair selection");
  expect(search.repair_feedback_row(input, Json::Array{"schedule_rules"}) ==
             &input["feedback"].items()[0],
         "action-only evidence changed schedule repair fallback");
  for (const auto *operation : {"fetch", "verification-worker", "render-document"}) {
    auto other_operation = guard;
    other_operation["operation"] = operation;
    const auto candidate = packet(other_operation);
    expect(search.repair_feedback_row(candidate, selected) == &candidate["feedback"].items()[1],
           "redispatch priority was coupled to a business operation name");
  }
  std::vector<Json> nonopportunities;
  auto rejected = guard;
  rejected["action_control"]["decision"] = "reject";
  nonopportunities.push_back(rejected);
  auto absent = guard;
  absent["action_control"].fields().erase("decision");
  nonopportunities.push_back(absent);
  auto initial = guard;
  initial["action_control"]["prior_operation_rounds"] = 0;
  nonopportunities.push_back(initial);
  auto plain = guard;
  plain["guard_only"] = false;
  plain["executed"] = true;
  nonopportunities.push_back(plain);
  auto inconsistent = guard;
  inconsistent["executed"] = true;
  nonopportunities.push_back(inconsistent);
  auto proposed = guard;
  proposed["semantic"] = "model";
  proposed["output"] = Json{{"tool_calls", Json::Array{Json{{"name", "arbitrary-operation"}}}}};
  nonopportunities.push_back(proposed);
  auto sibling = guard;
  sibling["guard_only"] = false;
  sibling["executed"] = true;
  sibling["status"] = "failed";
  sibling["action_control"]["prior_failures"] = 3;
  nonopportunities.push_back(sibling);
  for (const auto &action : nonopportunities) {
    const auto candidate = packet(action);
    expect(search.repair_feedback_row(candidate, selected) == &candidate["feedback"].items()[0],
           "rejected, initial, nonguard, model-plan or sibling-failure evidence gained redispatch "
           "priority");
  }
  // 没有重复派发时，仍选择第一条相关证据；都不相关时保留第一条，空输入不造证据。
  const Json fallback{
      {"feedback", Json::Array{row("unrelated", "failed", Json::Array{}), controlled}}};
  expect(search.repair_feedback_row(fallback, Json::Array{"loop_rules"}) ==
             &fallback["feedback"].items()[1],
         "original first-relevant-row fallback changed");
  expect(search.repair_feedback_row(fallback, Json::Array{"experience:memory"}) ==
             &fallback["feedback"].items()[0],
         "original first-row fallback changed");
  const Json suffix = row("reviewed-suffix", "passed",
                          Json::Array{Json{{"kind", "loop.decision"},
                                           {"actor_id", "worker"},
                                           {"execution_id", "loop"},
                                           {"iteration", "3"},
                                           {"loop_control", Json{{"target_scope", "assistant"},
                                                                 {"pending_actions", false},
                                                                 {"proposed_decision", "continue"},
                                                                 {"iteration", 3},
                                                                 {"reviewed_waste_after", 2}}}}});
  Json suffix_input{
      {"selected_components", Json::Array{"loop_rules"}},
      {"components", Json{{"loop_rules", Json::Array{}}}},
      {"controls",
       Json{{"aggressive_controls", true},
            {"limits", Json{{"min_iterations", 2}}},
            {"loop_primitives",
             Json::Array{Json{{"primitive_id", "bounded_iteration_cap"}}}},
            {"base_loop_rules", Json::Array{}}}},
      {"feedback", Json::Array{row("unrelated", "failed", Json::Array{}), suffix}}};
  expect(search.repair_feedback_row(suffix_input, Json::Array{"loop_rules"}) ==
             &suffix_input["feedback"].items()[1],
         "repair row selection lost a reviewed iteration-cap opportunity");
  suffix_input.fields().erase("controls");
  expect(search.repair_feedback_row(suffix_input, Json::Array{"loop_rules"}) ==
             &suffix_input["feedback"].items()[0],
         "missing parent controls authorized a reviewed iteration cap");

  const Json replan = row("replan", "passed",
                          Json::Array{Json{{"kind", "loop.decision"},
                                           {"actor_id", "worker"},
                                           {"execution_id", "loop"},
                                           {"loop_control", Json{{"target_scope", "assistant"},
                                                                 {"pending_actions", false},
                                                                 {"proposed_decision", "continue"},
                                                                 {"repeated_failure_steps", 1}}}}});
  Json replan_input{
      {"controls", Json{{"aggressive_controls", false},
                        {"base_loop_rules", Json::Array{Json{{"target_scope", "assistant"},
                                                             {"actor_id", "worker"},
                                                             {"no_progress_decision", "replan"},
                                                             {"max_replans", 1}}}}}},
      {"feedback", Json::Array{row("unrelated", "failed", Json::Array{}), replan}}};
  expect(search.repair_feedback_row(replan_input, Json::Array{"loop_rules"}) ==
             &replan_input["feedback"].items()[1],
         "repair row selection lost an authorized repeated-failure replan opportunity");
  replan_input.fields().erase("controls");
  expect(search.repair_feedback_row(replan_input, Json::Array{"loop_rules"}) ==
             &replan_input["feedback"].items()[0],
         "missing base loop rules authorized a repeated-failure replan");
  expect(search.repair_feedback_row(Json{{"feedback", Json::Array{}}}, selected) == nullptr,
         "empty repair input manufactured evidence");
}

// 测试辅助函数：missing_trial_control_evidence。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void missing_trial_control_evidence() {
  Report report;
  auto &trial = *report.add_trials();
  trial.set_verdict("error");
  trial.set_execution_status("failed");
  trial.set_error_code("evaluation_trace_bytes_limit");
  const auto result = control_feedback_trial(trial, Json(Json::Array{"action_rules", "loop_rules"}),
                                             Json::Object{});
  expect(result.str("verdict") == "error" &&
             result.str("error_code") == "evaluation_trace_bytes_limit",
         "capture failure was converted to a business result");
  expect(result["checks"].items().empty() && result["evidence"]["runs"].items().empty() &&
             !result.flag("control_evidence_preserved", true),
         "missing capture manufactured control evidence");
  Search search;
  const Json packet{{"feedback", Json::Array{Json{{"trial", result}}}}};
  expect(!search.control_evidence(packet, "action_rules") &&
             !search.control_evidence(packet, "loop_rules"),
         "capture failure authorized runtime controls");
  // 部分旧评测只留下 trace 容器，没有 events；经验应用诊断应返回未观测，
  // 不能因一个坏 trace 中断整批反思。
  auto missing_events = trial;
  missing_events.set_trial_id("missing-events");
  *missing_events.mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"traces", Json::Array{Json{{"run_id", "empty-trace"}},
                                    Json{{"events", Json::Array{}}}}}});
  const auto applications = selected_experience_applications(
      missing_events, Json::Array{"experience:private"}, public_alias_fixture());
  expect(applications.str("status") == "not_observed" &&
             applications.num("count", 1) == 0 && applications["points"].items().empty(),
         "missing trace events crashed or invented an experience application");
}

// 长轨迹压缩只能删除重复正文，不能删除生成 typed 控制和解释失败所需的字段。
void compact_control_reflection_preserves_typed_evidence() {
  const Json action{{"event_id", "action-event"}, {"kind", "action.end"},
                    {"operation", "run_skills"}, {"observed_failure", "invalid_arguments"},
                    {"input", Json{{"skills", Json::Array{}}}},
                    {"iteration_assessment", Json{{"decision", "waste"}}},
                    {"reviewed_required_action", true},
                    {"terminal_error_code_candidate", "invalid_arguments"},
                    {"output", std::string(12000, 'x')},
                    {"action_control", Json{{"query_event_id", "action-query"},
                                             {"target_scope", "coordinator"},
                                             {"operation", "run_skills"},
                                             {"decision", "reject"},
                                             {"prior_failures", 2},
                                             {"failure_key_candidates", Json::Array{"invalid_arguments"}},
                                             {"missing_required_arguments", Json::Array{"skills"}},
                                             {"terminal_retry_query_event_id", "retry-query"}}}};
  const Json loop{{"event_id", "loop-event"}, {"kind", "loop.decision"},
                  {"actor_id", "coordinator"}, {"execution_id", "branch"},
                  {"loop_control", Json{{"query_event_id", "loop-query"},
                                         {"target_scope", "coordinator"},
                                         {"iteration", 3}, {"pending_actions", false},
                                         {"proposed_decision", "continue"},
                                         {"observed_no_progress", 1},
                                         {"repeated_failure_steps", 2},
                                         {"error_codes", Json::Array{"invalid_arguments"}},
                                         {"reviewed_waste_after", 2},
                                         {"required_iteration_floor", 4},
                                         {"can_finalize", true}}}};
  Json actions = Json::Array{action, loop};
  for (unsigned index = 2; index < 12; ++index) {
    auto next = action;
    next["event_id"] = "action-event-" + std::to_string(index);
    actions.push(std::move(next));
  }
  Json source{{"verdict", "failed"}, {"execution_status", "completed"},
              {"error_code", "final_answer_missing"},
              {"checks", Json::Array{Json{{"criterion", "deliver answer"},
                                           {"passed", false},
                                           {"reason", "missing final answer"}}}},
              {"goal_completion", Json{{"quality", 0.25}, {"critical_failures", 1}}}};
  source["evidence"]["runs"] = Json::Array{Json{{"run_id", "run"}, {"actions", actions}}};
  const auto original = source.dump();
  const auto compact = compact_control_reflection_trial(
      source, Json::Array{"action_rules", "loop_rules"});
  const auto &rows = compact["evidence"]["runs"].items()[0]["actions"].items();
  expect(rows.size() == 12 && compact.flag("evidence_compacted_for_reflection", false) &&
             compact.str("error_code") == "final_answer_missing" &&
             compact["checks"].items()[0].str("reason") == "missing final answer" &&
             compact["goal_completion"].num("critical_failures", 0) == 1,
         "control compression lost evaluation failure context");
  expect(rows[0]["action_control"].str("query_event_id") == "action-query" &&
             rows[0]["action_control"].str("decision") == "reject" &&
             rows[0]["action_control"]["missing_required_arguments"].items()[0].text() ==
                 "skills" &&
             rows[0]["action_control"].str("terminal_retry_query_event_id") == "retry-query" &&
             rows[1]["loop_control"].str("query_event_id") == "loop-query" &&
             rows[1]["loop_control"].num("required_iteration_floor", 0) == 4 &&
             rows[1]["loop_control"].num("reviewed_waste_after", 0) == 2 &&
             rows[1]["loop_control"].flag("can_finalize", false) &&
             rows[0]["input"].has("skills") &&
             rows[0]["iteration_assessment"].str("decision") == "waste" &&
             rows[0].flag("reviewed_required_action", false) &&
             rows[0].str("terminal_error_code_candidate") == "invalid_arguments" &&
             rows[11].str("event_id") == "action-event-11" &&
             rows[0]["output"].dump().size() < action["output"].dump().size() &&
             source.dump() == original,
         "control compression lost typed selector/action/loop evidence or modified source");
}

// 64KB 反思预算应容纳八道不同的控制证据题，并保留成功对照与可编辑绑定。
void control_packet_retains_eight_distinct_tasks() {
  namespace te = trajectory::evolution;
  Search search;
  search.seed = public_alias_fixture();
  auto policy = proto<te::v1::RuntimePolicy>(search.seed["policy"]);
  auto options = policy.options();
  options.set_requested_level(te::v1::L3);
  options.add_host_capabilities("loop.decide");
  auto *base_loop = options.add_loop_rules();
  base_loop->set_rule_id("base-loop");
  base_loop->set_target_scope("assistant");
  base_loop->set_max_iterations(12);
  base_loop->set_max_no_progress(3);
  base_loop->set_no_progress_decision("stop");
  base_loop->set_reason("Preserve the existing execution budget");
  search.seed["level"] = "L3";
  search.seed["policy"] = json(te::compile_runtime_policy(policy.snapshot(), options));
  search.controls = Json{{"enabled", true}, {"prompt_file", "unused"}};
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 8},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  Report report;
  for (unsigned index = 0; index < 8; ++index) {
    const auto id = "control-task-" + std::to_string(index);
    auto *task = search.dataset.add_tasks();
    task->set_task_id(id);
    task->set_split("train");
    task->set_task_input("Execute the necessary action and deliver a verified result: " + id);
    task->set_expected_behavior("Report the completed result and any failure honestly.");
    Json events = Json::Array{};
    for (unsigned prior = 0; prior < 2; ++prior) {
      const auto action_id = "prior-" + id + "-" + std::to_string(prior);
      events.push(Json{{"kind", "action.start"}, {"action_id", action_id},
                       {"operation", "query"}, {"input", Json::Object{}},
                       {"attributes", Json{{"type", "schedule.action"},
                                            {"target_scope", "assistant"}}}});
      events.push(Json{{"kind", "action.end"}, {"event_id", action_id},
                       {"action_id", action_id}, {"operation", "query"},
                       {"status", "completed"},
                       {"output", "prior-result-" + id + std::string(2500, 'x')},
                       {"attributes", Json{{"semantic", "action"}, {"executed", true}}}});
    }
    const Json query{{"boundary", "actions"}, {"execution_id", "branch"},
                     {"advice_query", Json{{"target_scope", "assistant"}}},
                     {"actions", Json::Array{Json{{"action_id", "action-" + id},
                                                    {"operation", "query"},
                                                    {"arguments", Json::Object{}}}}}};
    const Json decision{{"query", query},
                        {"actions", Json::Array{Json{{"action_id", "action-" + id},
                                                        {"decision", "allow"}}}}};
    const Json attributes{{"type", "policy.query"}, {"decision", decision}};
    events.push(Json{{"kind", "custom"}, {"event_id", "query-" + id},
                     {"step_id", "guard"}, {"attributes", attributes}});
    events.push(Json{{"kind", "action.start"}, {"action_id", "action-" + id},
                     {"operation", "query"},
                     {"attributes", Json{{"extensions", Json{{"guard_only", true}}}}}});
    events.push(Json{{"kind", "action.end"}, {"event_id", "action-end-" + id},
                     {"action_id", "action-" + id}, {"operation", "query"},
                     {"status", "completed"},
                     {"output", "observed-result-" + id + std::string(2500, 'x')},
                     {"attributes", Json{{"semantic", "action"}, {"executed", false},
                                          {"guard_only", true}}}});
    auto *trial = report.add_trials();
    trial->set_task_id(id);
    trial->set_trial_id(id);
    trial->set_variant_id("candidate");
    trial->set_execution_status("completed");
    trial->set_verdict(index == 7 ? "passed" : "failed");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", id}}}, {"events", events}}}});
  }
  const auto source = report.SerializeAsString();
  const auto packet = search.packet(search.seed, report, Json::Array{"action_rules"});
  std::set<std::string> ids;
  bool positive = false;
  for (const auto &row : packet["feedback"].items()) {
    ids.insert(row.str("task_id"));
    positive |= row["trial"].str("verdict") == "passed";
    expect(row["trial"]["evidence"]["runs"].array() &&
               !row["trial"]["evidence"]["runs"].items().empty(),
           "control feedback lost its real execution evidence");
  }
  expect(packet["feedback"].items().size() == 8 && ids.size() == 8 && positive &&
             reflection_input_bytes(packet) <= 65536 &&
             report.SerializeAsString() == source,
         "eight distinct control tasks or successful counterpart were lost under 64KB");
  const auto prepared = observed_operation_bindings(packet, 65536);
  if (!prepared["controls"]["editable_bindings"].array() ||
      prepared["controls"]["editable_bindings"].items().empty()) {
    std::cerr << "eight_task_packet bytes=" << reflection_input_bytes(packet)
              << " prepared_bytes=" << prepared.dump().size()
              << " action_primitives=" << packet["controls"]["action_primitives"].dump()
              << " first_actions="
              << packet["feedback"].items().front()["trial"]["evidence"]["runs"]
                     .items().front()["actions"].dump().substr(0, 3000)
              << '\n';
  }
  expect(prepared.dump().size() <= 65536 &&
             prepared["controls"]["editable_bindings"].array() &&
             !prepared["controls"]["editable_bindings"].items().empty(),
         "eight-task control packet lost typed editable bindings");
}

// 回归用例：反思分阶段覆盖只统计实际入模 task_id，reduce 空 feedback 不清零；
// 同一 suffix 重试将 task 取并集，输入字节累计真实请求。
void reflection_stage_coverage_accumulates_unique_tasks() {
  Json node = Json::Object{};
  const Json first{{"phase", "pool_diagnostic_map"},
                   {"feedback", Json::Array{Json{{"task_id", "a"}},
                                             Json{{"task_id", "b"}},
                                             Json{{"task_id", "a"}}}}};
  const auto first_bytes = first.dump().size();
  record_reflection_coverage(node, ".pool-map-0", first);
  expect(node.num("reflection_feedback_count", 0) == 2 &&
             node.num("reflection_input_bytes", 0) == first_bytes &&
             node["reflection_inputs"][".pool-map-0"]["task_ids"].items().size() == 2,
         "reflection coverage did not dedupe tasks in an actual map input");

  const Json second{{"phase", "pool_diagnostic_map"},
                    {"feedback", Json::Array{Json{{"task_id", "b"}},
                                              Json{{"task_id", "c"}}}}};
  record_reflection_coverage(node, ".pool-map-1", second);
  expect(node.num("reflection_feedback_count", 0) == 3 &&
             node.num("reflection_input_bytes", 0) == first_bytes + second.dump().size(),
         "reflection coverage did not union tasks across map groups");

  const Json reduce{{"phase", "pool_diagnostic_reduce"}, {"feedback", Json::Array{}}};
  record_reflection_coverage(node, ".pool-reduce-0", reduce);
  expect(node.num("reflection_feedback_count", 0) == 3 &&
             node.num("reflection_input_bytes", 0) ==
                 first_bytes + second.dump().size() + reduce.dump().size() &&
             node["reflection_inputs"][".pool-reduce-0"]["task_ids"].items().empty(),
         "empty-feedback reduce erased prior coverage or invented task evidence");

  const Json retry{{"phase", "pool_diagnostic_map"},
                   {"feedback", Json::Array{Json{{"task_id", "c"}},
                                             Json{{"task_id", "d"}}}}};
  record_reflection_coverage(node, ".pool-map-1", retry);
  expect(node.num("reflection_feedback_count", 0) == 4 &&
             node.num("reflection_input_bytes", 0) == first_bytes + second.dump().size() +
                 reduce.dump().size() + retry.dump().size() &&
             node["reflection_inputs"][".pool-map-1"].num("attempts", 0) == 2,
         "repeated reflection stage double-counted task coverage or lost request bytes");

  Json single = Json::Object{};
  const Json ordinary{{"feedback", Json::Array{Json{{"task_id", "seed-task"}}}}};
  record_reflection_coverage(single, "", ordinary);
  expect(single.num("reflection_feedback_count", 0) == 1 &&
             single.num("reflection_input_bytes", 0) == ordinary.dump().size() &&
             single["reflection_inputs"].has("single"),
         "single-stage reflection metrics changed shape");
}

// 逐题 map 摘要必须保留明确业务根因、loop loss 与正反例身份，且不超过紧凑预算。
void reflection_task_facts_are_bounded_and_grounded() {
  const std::string long_reason = "Satisfied an unrelated precondition. Missing the requested final image.";
  Json failing_trial{{"verdict", "failed"},
                     {"execution_status", "completed"},
                     {"error_code", "business_failure"},
                     {"metrics", Json{{"quality", 0.25}, {"tokens", 120}}}};
  failing_trial["goal_completion"] = Json{
      {"quality", 0.25}, {"coverage", 0.5}, {"critical_failures", 1},
      {"issues", Json::Array{Json{{"id", "image"}, {"status", "failed"},
                                 {"critical_failure", true}, {"reason", long_reason}}}}};
  Json actions = Json::Array{};
  actions.push(Json{{"executed", true}, {"observed_failure", true},
                    {"action_control", Json{{"decision", "allow"}}}});
  actions.push(Json{{"loop_control", Json{{"rejected_actions", 1}, {"failed_actions", 2},
                                            {"observed_no_progress", 1}}}});
  failing_trial["evidence"] = Json::Object{};
  failing_trial["evidence"]["loop_review"] = Json{{"status", "complete"}, {"count", 3}};
  failing_trial["evidence"]["runs"] = Json::Array{Json{{"actions", actions}}};
  Json failure_row{{"task_id", "task-fail"},
                   {"selected_experience_applications",
                    Json{{"status", "observed"}, {"count", 2},
                         {"points", Json::Array{Json{{"memory_id", "memory-a"}}}}}},
                   {"trial", failing_trial}};
  Json success_row{{"task_id", "task-pass"},
                   {"trial", Json{{"verdict", "passed"},
                                   {"execution_status", "completed"},
                                   {"metrics", Json{{"quality", 1.0}}}}}};
  const Json rows = Json::Array{failure_row, success_row};
  const auto facts = reflection_task_facts(rows, Json::Array{"action_rules", "loop_rules"});
  expect(facts.items().size() == 2 && facts.items()[0].num("invalid_loops", 0) == 3 &&
             facts.items()[0]["control_evidence"].num("action_queries", 0) == 1 &&
             facts.items()[0]["control_evidence"].num("loop_rejected_actions", 0) == 1 &&
             facts.items()[0]["goal"]["issues"].items()[0].str("id") == "image" &&
             facts.items()[0]["experience_applications"].num("count", 0) == 2 &&
             facts.items()[0].dump().size() <= 512 && facts.items()[1].dump().size() <= 512,
         "per-task reflection facts lost a counterexample field or exceeded the bound");
  expect(facts.items()[0].str("verdict") == "failed" &&
             facts.items()[1].str("verdict") == "passed",
         "per-task facts did not preserve a real success/failure contrast");
  const auto experience_only = reflection_task_facts(
      Json::Array{rows.items()[0]}, Json::Array{"experience:memory-a"});
  expect(experience_only.items()[0]["experience_applications"].num("count", 0) == 2 &&
             !experience_only.items()[0].has("control_evidence"),
         "experience-only summary invented control evidence");
}

// 测试辅助函数：reflection_transport_diagnostics。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void reflection_transport_diagnostics() {
  agentflow::trajectory::RawModelResponse response;
  response.transport_ok = true;
  response.attempts = 2;
  response.adapter = "private-adapter-marker";
  response.model_id = "private-model-marker";
  response.body = "private-response-body-marker";
  for (const auto *code :
       {"", "missing_record", "request_too_large", "response_too_large",
        "client_initialization_failed", "header_allocation_failed", "output_token_limit_reached",
        "invalid_api_response", "transport_exhausted", "http_100", "http_429", "http_503",
        "http_599", "transport_1", "transport_28", "transport_42", "transport_999"}) {
    response.error_code = code;
    const auto value = reflection_call_diagnostic(response, 123);
    expect(value.fields().size() == 5 && value.flag("transport_ok", false) &&
               value.str("error_code") == code && value.num("attempts", 0) == 2 &&
               value.num("duration_ms", 0) == 123 &&
               value.num("decoded_body_bytes", 0) == response.body.size(),
           "reflection diagnostic lost fixed code or numeric measurements");
    expect(value.dump().find("private-") == std::string::npos,
           "reflection diagnostic retained body, adapter or model text");
  }
  response.transport_ok = false;
  response.attempts = 0;
  for (const auto *code :
       {"http_0", "http_99", "http_600", "http_1000", "http_0429", "http_+429", "http_429suffix",
        "transport_0", "transport_028", "transport_1000", "transport_-1", "transport_1.0",
        "transport_1e2", "transport_999999999999999999999999999999", "transport_28\nprivate-secret",
        "private-secret", "https://private-user:private-secret@example.invalid",
        "Authorization: Bearer private-secret"}) {
    response.error_code = code;
    const auto value = reflection_call_diagnostic(response, 0);
    expect(value.fields().size() == 5 && !value.flag("transport_ok", true) &&
               value.str("error_code") == "unknown_adapter_error" &&
               value.num("attempts", 1) == 0 && value.num("duration_ms", 1) == 0 &&
               value.dump().find("private-") == std::string::npos,
           "reflection diagnostic leaked an unknown or out-of-range adapter error");
  }
  response.error_code = std::string("http_429\0private-secret", 23);
  expect(reflection_call_diagnostic(response, 0).str("error_code") == "unknown_adapter_error",
         "reflection diagnostic accepted a numeric code with an embedded null");
  expect(response.body == "private-response-body-marker" &&
             response.adapter == "private-adapter-marker" &&
             response.model_id == "private-model-marker",
         "reflection diagnostic mutated its response");
}

// 缺 token 的重试不能从共享报告重新接回失败执行；完整题仍按原身份复用。
void missing_metric_recovery_cache(const fs::path &directory) {
  namespace te = trajectory::evolution;
  Search search;
  search.root = directory / "metric-job/search";
  search.context = "metric-recovery";
  search.candidate_evaluation_context = "metric-runtime";
  search.dataset.set_dataset_version("metric-dataset");
  search.config = Json{{"cache_evaluations", true}, {"max_trials", 20},
      {"timeout_ms", 60000}, {"unknown_token_policy", "reject"},
      {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0.05}}}};
  search.tree = Json{{"started_ms", now()}, {"evaluations", Json::Object{}}};
  const auto candidate = public_alias_fixture();
  search.baseline = candidate;
  const auto version = te::agent_version_id(candidate.dump());
  search.evaluation = Json{{"repetitions", 1},
      {"variants", Json::Array{Json{{"id", "candidate"}, {"version", version},
                                   {"artifact", candidate}}}}};
  auto settings = search.evaluation;
  settings["splits"] = Json::Array{"train"};
  settings["task_ids"] = Json::Array{"complete", "missing"};
  Report old;
  old.set_schema_version(1);
  old.set_report_kind("task_evaluation.v1");
  old.set_dataset_version("metric-dataset");
  old.set_run_id("original-execution");
  old.set_complete(true);
  for (const auto &id : {"complete", "missing"}) {
    auto *trial = old.add_trials();
    trial->set_task_id(id);
    trial->set_trial_id(std::string("original-") + id);
    trial->set_variant_id("candidate");
    trial->set_split("train");
    trial->set_repetition(1);
    trial->set_execution_status("completed");
    trial->set_verdict("passed");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"loaded_version", version}});
    if (std::string(id) == "complete")
      *trial->mutable_metrics() = proto<google::protobuf::Struct>(Json{{"tokens", 100}});
  }
  auto persist = [&](const fs::path &path, const Report &report) {
    const auto bytes = json(report).dump();
    write(path, bytes);
    write(path.parent_path() / "manifest.json", Json{{"complete", true},
        {"run_id", report.run_id()}, {"files", Json{{"report.json", hash(bytes)}}}}.dump());
  };
  const auto shared = directory / "old-metric-job/search/evaluations/old/attempt-1/report.json";
  persist(shared, old);
  search.remember_shared_evaluation(settings, false, shared, old);
  expect(search.shared_evaluation_report(settings, false) == shared,
         "混合指标报告未进入共享恢复场景");
  const auto key = evaluation_cache_key(search, settings);
  const auto relative = fs::path("evaluations") / key / "attempt-1/report.json";
  persist(search.root / relative, old);
  std::vector<std::string> executed;
  std::size_t boundary_calls = 0;
  te::evaluation_test_boundary = [&](const fs::path &output, const te::TaskEvaluationOptions &options) {
    ++boundary_calls;
    const auto request = Json::parse(options.config_json);
    expect((request["task_ids"].items().size() == 2 && options.resume_report == shared &&
                options.resume_rerun_trial_ids == std::set<std::string>{"original-missing"}) ||
               (request["task_ids"].dump() == "[\"missing\"]" &&
                options.resume_report.empty() &&
                options.resume_rerun_trial_ids.empty()),
           "共享检查点未精确重跑缺指标题，或本地子集错误接回旧执行");
    Report fresh = old;
    fresh.clear_trials();
    fresh.set_run_id("fresh-execution");
    for (const auto &id : request["task_ids"].items()) {
      executed.push_back(id.text());
      auto *trial = fresh.add_trials();
      *trial = old.trials(id.text() == "complete" ? 0 : 1);
      trial->set_trial_id("fresh-" + id.text());
      *trial->mutable_metrics() = proto<google::protobuf::Struct>(Json{{"tokens", 123}});
    }
    persist(output / "report.json", fresh);
    return fresh;
  };
  struct ResetBoundary {
    ~ResetBoundary() { te::evaluation_test_boundary = {}; }
  } reset;
  // 已持久化重跑标志且无可评分本地报告时，可复用共享报告的完整题，
  // 但必须把缺 token 的旧执行列入强制重跑集合。
  search.tree["evaluations"][key] = Json{{"status", "running"}, {"attempt", 1},
      {"evaluation_context", search.evaluation_context},
      {"candidate_evaluation_context", search.candidate_evaluation_context},
      {"rerun_incomplete_metrics", true}};
  auto result = search.evaluate(candidate, {"complete", "missing"}, "train");
  expect(boundary_calls == 1 && search.scores(result).fields().size() == 2,
         "缺指标的共享检查点阻止了真实补跑");
  // 已完成本地报告中的完整题可单独复用，仅缺 token 的一题进入执行边界。
  search.tree["evaluations"] = Json::Object{};
  search.tree["evaluations"][key] = Json{{"status", "inconclusive"}, {"attempt", 1},
      {"evaluation_context", search.evaluation_context},
      {"candidate_evaluation_context", search.candidate_evaluation_context},
      {"rerun_incomplete_metrics", true}, {"split", "train"}, {"version", version},
      {"task_ids", settings["task_ids"]}, {"report", relative.string()},
      {"report_hash", hash(json(old).dump())}};
  executed.clear();
  boundary_calls = 0;
  result = search.evaluate(candidate, {"complete", "missing"}, "train");
  expect(boundary_calls == 1 && executed == std::vector<std::string>{"missing"} &&
             result.trials(0).trial_id() == "original-complete" &&
             result.trials(1).trial_id() == "fresh-missing" &&
             search.scores(result).fields().size() == 2,
         "补跑缺失遥测时重跑了完整题，或失败执行未被替换");

  // evaluate 的自动额度已用过：同一父节点仍可额外补一次，不新建节点或更换题目。
  // 两种结局分别验证成功恢复，以及持续缺失达到上限后重启也不再盲目重跑。
  for (const bool eventually_complete : {true, false}) {
    auto bounded = search;
    bounded.root = directory / (eventually_complete ? "parent-recovered/search"
                                                  : "parent-unresolved/search");
    bounded.context = eventually_complete ? "parent-recovered" : "parent-unresolved";
    // 与上方的共享子集隔离，单独验证父节点自身的两次恢复及上限。
    bounded.candidate_evaluation_context = bounded.context + "-runtime";
    const auto full_key = evaluation_cache_key(bounded, settings);
    auto subset_settings = settings;
    subset_settings["task_ids"] = Json::Array{"missing"};
    const auto subset_key = evaluation_cache_key(bounded, subset_settings);
    const auto full_path = fs::path("evaluations") / full_key / "attempt-1/report.json";
    const auto subset_path = fs::path("evaluations") / subset_key / "attempt-1/report.json";
    auto missing = old;
    missing.clear_trials();
    *missing.add_trials() = old.trials(1);
    persist(bounded.root / full_path, old);
    persist(bounded.root / subset_path, missing);
    const auto cache = [&](const Report &report, const fs::path &path, const Json &ids) {
      return Json{{"status", "inconclusive"}, {"attempt", 1},
          {"evaluation_context", bounded.evaluation_context},
          {"candidate_evaluation_context", bounded.candidate_evaluation_context},
          {"automatic_recovery_used", true}, {"rerun_incomplete_metrics", true},
          {"split", "train"}, {"version", version}, {"task_ids", ids},
          {"report", path.string()}, {"report_hash", hash(json(report).dump())}};
    };
    bounded.tree = Json{{"started_ms", now()}, {"rounds", 1}, {"model_calls", 0},
        {"memory_feedback", Json::Array{}}, {"nodes", Json::Array{Json{
            {"id", 0}, {"parent", 0}, {"status", "pending"}, {"tasks", settings["task_ids"]}}}},
        {"evaluations", Json{{full_key, cache(old, full_path, settings["task_ids"])},
                             {subset_key, cache(missing, subset_path, subset_settings["task_ids"])}}}};
    executed.clear();
    boundary_calls = 0;
    te::evaluation_test_boundary = [&](const fs::path &output,
                                        const te::TaskEvaluationOptions &options) {
      ++boundary_calls;
      expect(options.resume_report.empty(), "父节点补跑恢复了缺用量的旧执行");
      const auto request = Json::parse(options.config_json);
      expect(request["task_ids"].dump() == subset_settings["task_ids"].dump(),
             "父节点恢复重复执行完整题，或更换了原始任务集");
      executed.push_back("missing");
      auto fresh = missing;
      fresh.mutable_trials(0)->set_trial_id("parent-retry-" + std::to_string(boundary_calls));
      if (eventually_complete && boundary_calls == 2)
        *fresh.mutable_trials(0)->mutable_metrics() =
            proto<google::protobuf::Struct>(Json{{"tokens", 123}});
      persist(output / "report.json", fresh);
      return fresh;
    };
    bool unresolved = false;
    try {
      const auto repaired = bounded.evaluate_parent(
          bounded.tree["nodes"].items()[0], candidate, {"complete", "missing"});
      expect(eventually_complete && bounded.scores(repaired).fields().size() == 2 &&
                 repaired.trials(0).trial_id() == "original-complete" &&
                 json(repaired.trials(1).metrics()).num("tokens", 0) == 123,
             "恢复未使用真实新用量，或覆盖了原完整题");
    } catch (const Error &error) {
      unresolved = std::string(error.what()) == "gepa_parent_evaluation_inconclusive";
      expect(unresolved, "父评测补跑抛出了非预期错误");
      bounded.error(0, error);
    }
    const auto &node = bounded.tree["nodes"].items()[0];
    expect(unresolved != eventually_complete && boundary_calls == 2 &&
               executed == std::vector<std::string>({"missing", "missing"}) &&
               node.num("parent_recovery_attempts", 0) == 1 &&
               bounded.tree.num("rounds", 0) == 1 && bounded.tree.num("model_calls", 99) == 0 &&
               node["tasks"].dump() == settings["task_ids"].dump() &&
               bounded.tree["memory_feedback"].items().empty(),
           "父节点恢复改变了轮数、题目或记忆，或没有遵循一次额外恢复上限");
    if (!eventually_complete) {
      expect(node.str("status") == "inconclusive" &&
                 node["parent_evaluation"]["missing_task_ids"].dump() == "[\"missing\"]",
             "父评测未完成被误记成策略失败，或没有指出缺失题目");
      auto resumed = bounded;
      resumed.tree = Json::parse(read(bounded.root / "tree.json"));
      bool exhausted = false;
      try {
        resumed.evaluate_parent(resumed.tree["nodes"].items()[0], candidate,
                                {"complete", "missing"});
      } catch (const Error &error) {
        exhausted = std::string(error.what()) == "gepa_parent_evaluation_inconclusive";
      }
      expect(exhausted && boundary_calls == 2 &&
                 resumed.tree["nodes"].items()[0].num("parent_recovery_attempts", 0) == 1 &&
                 json(missing.trials(0).metrics())["tokens"].null(),
             "重启重置恢复额度、重复调用宿主或伪造缺失的 token");
    }
  }

  // 即使报告声称 complete，也不能把完全漏掉的一题当成已有成绩。
  auto omitted = search;
  omitted.root = directory / "parent-omitted/search";
  omitted.context = omitted.candidate_evaluation_context = "parent-omitted";
  omitted.tree = Json{{"started_ms", now()}, {"evaluations", Json::Object{}},
                     {"nodes", Json::Array{Json{{"status", "pending"}}}}};
  te::evaluation_test_boundary = [&](const fs::path &output, const te::TaskEvaluationOptions &) {
    auto partial = old;
    partial.clear_trials();
    *partial.add_trials() = old.trials(0);
    persist(output / "report.json", partial);
    return partial;
  };
  bool missing_task = false;
  try {
    omitted.evaluate_parent(omitted.tree["nodes"].items()[0], candidate, {"complete", "missing"});
  } catch (const Error &error) {
    missing_task = std::string(error.what()) == "gepa_parent_evaluation_inconclusive";
  }
  expect(missing_task && omitted.tree["nodes"].items()[0]["parent_evaluation"]
                            ["missing_task_ids"].dump() == "[\"missing\"]",
         "遗漏整道题的父报告被视为完整，或未列出缺席题号");
}

// 优雅中断会先把新 attempt 写入 tree，但可能尚未发布 manifest。
// retry 必须跳过这个空／损坏 attempt，复用更早检查点，且不再次预留同批预算。
void interrupted_attempt_reuses_prior_checkpoint_without_rebilling() {
  namespace te = trajectory::evolution;
  char directory[] = "/tmp/trajectory-gepa-attempt-resume-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create attempt resume fixture");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { fs::remove_all(path); }
  } cleanup{directory};

  Search search;
  search.root = fs::path(directory) / "job/search";
  search.context = "frozen-evaluation-context";
  search.dataset.set_dataset_version("dataset-v1");
  for (const auto *id : {"task-a", "task-b"}) {
    auto *task = search.dataset.add_tasks();
    task->set_task_id(id);
    task->set_task_version("task-v1");
    task->set_split("train");
  }
  search.config = Json{{"cache_evaluations", true},
                       {"max_trials", 20},
                       {"timeout_ms", 60000},
                       {"objective", Json{{"invalid_loop_weight", 0},
                                           {"token_weight", 0},
                                           {"latency_weight", 0}}}};
  search.tree = Json{{"started_ms", now()}, {"trials_used", 2},
                     {"evaluations", Json::Object{}}};
  const auto candidate = public_alias_fixture();
  search.baseline = candidate;
  const auto version = te::agent_version_id(candidate.dump());
  search.evaluation = Json{{"repetitions", 1},
                           {"variants", Json::Array{Json{{"id", "host"}}}}};
  Json settings = search.evaluation;
  settings["splits"] = Json::Array{"train"};
  settings["task_ids"] = Json::Array{"task-a", "task-b"};
  auto host = search.evaluation["variants"].items().front();
  host["id"] = "candidate";
  host["artifact"] = candidate;
  host["version"] = version;
  settings["variants"] = Json::Array{host};
  const auto key = evaluation_cache_key(search, settings);

  const auto make_trial = [&](const std::string &task, const std::string &trial_id) {
    Trial trial;
    trial.set_task_id(task);
    trial.set_task_version("task-v1");
    trial.set_trial_id(trial_id);
    trial.set_variant_id("candidate");
    trial.set_split("train");
    trial.set_repetition(1);
    trial.set_execution_status("completed");
    trial.set_verdict("passed");
    *trial.mutable_evidence() =
        proto<google::protobuf::Struct>(Json{{"loaded_version", version}});
    return trial;
  };
  const auto persist = [&](const fs::path &path, const Report &report, bool valid = true) {
    const auto bytes = json(report).dump();
    write(path, bytes);
    write(path.parent_path() / "manifest.json",
          Json{{"complete", report.complete()},
               {"run_id", report.run_id()},
               {"files", Json{{"report.json", valid ? hash(bytes) : "invalid"}}}}
              .dump());
  };

  Report partial;
  partial.set_schema_version(1);
  partial.set_report_kind("task_evaluation.v1");
  partial.set_dataset_version("dataset-v1");
  partial.set_run_id("attempt-1-run");
  partial.set_complete(false);
  *partial.add_trials() = make_trial("task-a", "attempt-1-task-a");
  persist(search.root / "evaluations" / key / "attempt-1/report.json", partial);
  auto duplicate = partial;
  *duplicate.add_trials() = duplicate.trials(0);
  persist(search.root / "evaluations" / key / "attempt-2/report.json", duplicate);
  // attempt-3 模拟中断窗口：有目录和报告，但清单未成功发布。
  persist(search.root / "evaluations" / key / "attempt-3/report.json", partial, false);
  search.tree["evaluations"][key] =
      Json{{"status", "running"}, {"attempt", 3}, {"trials_reserved", 2},
           {"version", version}, {"split", "train"}, {"task_ids", settings["task_ids"]}};

  std::size_t calls = 0;
  te::evaluation_test_boundary = [&](const fs::path &output,
                                      const te::TaskEvaluationOptions &options) {
    ++calls;
    expect(options.resume_report ==
               search.root / "evaluations" / key / "attempt-1/report.json",
           "retry did not fall back to the last valid historical attempt");
    expect(search.tree.num("trials_used", 0) == 2 &&
               search.tree["evaluations"][key].num("trials_reserved", 99) == 0 &&
               search.tree["evaluations"][key].num("trials_budget_reserved", 0) == 2,
           "same evaluation retry reserved or charged the frozen batch twice");
    Report complete = partial;
    complete.set_run_id("attempt-4-run");
    complete.set_complete(true);
    *complete.add_trials() = make_trial("task-b", "attempt-4-task-b");
    persist(output / "report.json", complete);
    return complete;
  };
  struct ResetBoundary {
    ~ResetBoundary() { te::evaluation_test_boundary = {}; }
  } reset;

  const auto result = search.evaluate(candidate, {"task-a", "task-b"}, "train");
  const auto &cached = search.tree["evaluations"][key];
  expect(calls == 1 && result.complete() && result.trials_size() == 2,
         "historical attempt resume did not complete the missing trial");
  expect(search.tree.num("trials_used", 0) == 2 && cached.num("attempt", 0) == 4 &&
             cached.num("trials_reserved", 99) == 0 &&
             cached.num("trials_budget_reserved", 0) == 2 &&
             cached.str("status") == "complete",
         "retry accounting or final cache state is incorrect");

  // 首次 attempt 若只指向后来被证明损坏的共享检查点，当时可能没有
  // 预留真实执行预算。这种 retry 必须补足，不能把“同 key”误解为永远免费。
  auto fallback = search;
  fallback.root = fs::path(directory) / "shared-cache-fallback/search";
  fallback.tree = Json{{"started_ms", now()}, {"trials_used", 0},
                       {"evaluations", Json{{key, Json{{"status", "error"},
                                                       {"attempt", 1},
                                                       {"trials_reserved", 0},
                                                       {"version", version},
                                                       {"split", "train"},
                                                       {"task_ids", settings["task_ids"]}}}}}};
  te::evaluation_test_boundary = [&](const fs::path &output,
                                      const te::TaskEvaluationOptions &options) {
    expect(options.resume_report.empty(), "invalid shared checkpoint was resumed again");
    expect(fallback.tree.num("trials_used", 0) == 2,
           "an unreserved shared-cache fallback did not charge trials_used");
    const auto &running =
        static_cast<const Json &>(fallback.tree)["evaluations"][key];
    if (running.str("status") != "running") {
      std::string actual;
      for (const auto &entry : fallback.tree["evaluations"].fields())
        if (entry.second.str("status") == "running")
          actual = entry.first;
      throw std::runtime_error("shared-cache fallback changed evaluation identity: expected=" +
                               key + " actual=" + actual + " expected_settings=" +
                               settings.dump() + " actual_settings=" + options.config_json);
    }
    expect(running.num("attempt", 0) == 2,
           "shared-cache fallback did not advance the existing attempt");
    expect(running.num("trials_reserved", 0) == 2,
           "an unreserved shared-cache fallback did not reserve the current attempt");
    expect(running.num("trials_budget_reserved", 0) == 2,
           "an unreserved shared-cache fallback lost its cumulative reservation");
    Report complete = partial;
    complete.set_run_id("fallback-attempt-2");
    complete.set_complete(true);
    complete.clear_trials();
    *complete.add_trials() = make_trial("task-a", "fallback-task-a");
    *complete.add_trials() = make_trial("task-b", "fallback-task-b");
    persist(output / "report.json", complete);
    return complete;
  };
  const auto fallback_result =
      fallback.evaluate(candidate, {"task-a", "task-b"}, "train", false, false);
  expect(fallback_result.complete() && fallback.tree.num("trials_used", 0) == 2 &&
             fallback.tree["evaluations"][key].num("attempt", 0) == 2,
         "shared-cache fallback did not complete with a newly reserved real execution");

  // 即便损坏或旧迁移把历史 attempt 放到了当前键下，持久化的执行 context
  // 不一致时也必须重新运行宿主，不能把旧二进制的结果当作本轮检查点。
  auto mismatch = search;
  mismatch.root = fs::path(directory) / "runtime-context-mismatch/search";
  mismatch.evaluation_context = "judge-current";
  mismatch.candidate_evaluation_context = "runtime-current";
  mismatch.tree = Json{{"started_ms", now()}, {"trials_used", 0},
                       {"evaluations", Json::Object{}}};
  const auto mismatch_key = evaluation_cache_key(mismatch, settings);
  persist(mismatch.root / "evaluations" / mismatch_key / "attempt-1/report.json", partial);
  mismatch.tree["evaluations"][mismatch_key] =
      Json{{"status", "inconclusive"},
           {"attempt", 1},
           {"evaluation_context", "judge-current"},
           {"candidate_evaluation_context", "runtime-old"},
           {"version", version},
           {"split", "train"},
           {"task_ids", settings["task_ids"]},
           {"report", (fs::path("evaluations") / mismatch_key /
                       "attempt-1/report.json").string()},
           {"report_hash", hash(json(partial).dump())}};
  calls = 0;
  te::evaluation_test_boundary = [&](const fs::path &output,
                                      const te::TaskEvaluationOptions &options) {
    ++calls;
    expect(options.resume_report.empty(),
           "runtime context mismatch resumed an old execution checkpoint");
    Report fresh = partial;
    fresh.set_run_id("runtime-current-execution");
    fresh.set_complete(true);
    fresh.clear_trials();
    *fresh.add_trials() = make_trial("task-a", "runtime-current-task-a");
    *fresh.add_trials() = make_trial("task-b", "runtime-current-task-b");
    persist(output / "report.json", fresh);
    return fresh;
  };
  const auto mismatch_result =
      mismatch.evaluate(candidate, {"task-a", "task-b"}, "train", false, false);
  expect(calls == 1 && mismatch_result.complete() &&
             mismatch_result.trials(0).trial_id() == "runtime-current-task-a" &&
             mismatch.tree["evaluations"][mismatch_key].str("candidate_evaluation_context") ==
                 "runtime-current",
         "runtime context mismatch reused an old tree evaluation");
}

// 测试辅助函数：cross_job_evaluation_cache。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void cross_job_evaluation_cache() {
  char directory[] = "/tmp/trajectory-gepa-cross-job-cache-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create shared evaluation cache fixture");

  struct Cleanup {
    fs::path path;

    // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
    ~Cleanup() {
      fs::remove_all(path);
    }
  } cleanup{directory};

  Search before;
  before.root = fs::path(directory) / "old-job/search";
  before.context = "search-v12";
  before.candidate_evaluation_context = "runtime-and-evaluator-v1";
  before.dataset.set_dataset_version("dataset-v1");
  before.config = Json{{"cache_evaluations", true},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const Json settings{
      {"splits", Json::Array{"val"}},
      {"task_ids", Json::Array{"task-a"}},
      {"repetitions", 1},
      {"variants", Json::Array{Json{{"id", "candidate"},
                                    {"version", "policy-a"},
                                    {"command", Json::Array{"private-marker-host"}}}}}};
  Report report;
  report.set_schema_version(1);
  report.set_report_kind("task_evaluation.v1");
  report.set_dataset_version("dataset-v1");
  report.set_complete(true);
  report.set_run_id("observed-run");
  auto *trial = report.add_trials();
  trial->set_variant_id("candidate");
  trial->set_task_id("task-a");
  trial->set_split("val");
  trial->set_repetition(1);
  trial->set_trial_id("observed-trial");
  trial->set_execution_status("completed");
  trial->set_verdict("passed");
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{
      {"loaded_version", "policy-a"}, {"loop_review", Json{{"status", "complete"}, {"count", 1}}}});
  *trial->mutable_metrics() = proto<google::protobuf::Struct>(Json{{"tokens", 10}});
  const auto path = before.root / "evaluations/exact/attempt-1/report.json";
  const auto bytes = json(report).dump();
  const auto manifest = Json{{"complete", true},
                             {"run_id", "observed-run"},
                             {"files", Json{{"report.json", hash(bytes)}}}}
                            .dump();
  write(path, bytes);
  write(path.parent_path() / "manifest.json", manifest);
  before.remember_shared_evaluation(settings, false, path, report);
  auto after = before;
  after.root = fs::path(directory) / "new-job/search";
  after.context = "search-v13";
  expect(after.shared_evaluation_report(settings, false) == path,
         "search-only changes invalidated identical execution evidence");
  after.config["objective"]["invalid_loop_weight"] = 0.5;
  after.gate = Json{{"min_pass_rate", 1}};
  expect(after.shared_evaluation_report(settings, false) == path,
         "new ranking or gate discarded reusable raw measurements");
  auto changed = settings;
  changed["variants"].items()[0]["version"] = "policy-b";
  expect(after.shared_evaluation_report(changed, false).empty(),
         "a different policy reused previous measurements");
  changed = settings;
  changed["parallelism"] = 2;
  expect(after.shared_evaluation_report(changed, false).empty(),
         "different evaluation conditions reused previous measurements");
  changed = settings;
  changed["repetitions"] = 2;
  expect(after.shared_evaluation_report(changed, false).empty(),
         "cache silently filled missing repetitions");
  changed = settings;
  changed["task_ids"] = Json::Array{"task-b"};
  expect(after.shared_evaluation_report(changed, false).empty(),
         "cache reused a different task set");
  after.candidate_evaluation_context = "runtime-and-evaluator-v2";
  expect(after.shared_evaluation_report(settings, false).empty(),
         "runtime or judge changes reused old execution evidence");
  after.candidate_evaluation_context = before.candidate_evaluation_context;
  after.dataset.set_dataset_version("dataset-v2");
  expect(after.shared_evaluation_report(settings, false).empty(),
         "changed dataset reused old grades");
  after.dataset.set_dataset_version("dataset-v1");
  changed = settings;
  changed["splits"] = Json::Array{"holdout"};
  expect(after.shared_evaluation_report(changed, false).empty() &&
             after.shared_evaluation_report(settings, true).empty(),
         "holdout or final publication evaluation reused the shared cache");
  changed = settings;
  changed["splits"] = Json::Array{"train"};
  auto training = report;
  training.mutable_trials(0)->set_split("train");
  const auto training_path = before.root / "evaluations/train/attempt-1/report.json";
  const auto training_bytes = json(training).dump();
  write(training_path, training_bytes);
  write(training_path.parent_path() / "manifest.json",
        Json{{"complete", true},
             {"run_id", training.run_id()},
             {"files", Json{{"report.json", hash(training_bytes)}}}}
            .dump());
  before.remember_shared_evaluation(changed, false, training_path, training);
  expect(after.shared_evaluation_report(changed, false) == training_path,
         "training report did not reuse the exact matching cache");
  auto failed = report;
  failed.mutable_trials(0)->set_error_code("evaluation_judge_failed");
  expect(!after.shared_report_complete(failed, settings),
         "failed model judging became a completed reusable result");
  failed = report;
  failed.set_complete(false);
  expect(!after.shared_report_complete(failed, settings),
         "partial evaluation became a completed reusable result");
  // 部分批次跨作业恢复：已评分题、已执行但裁判失败的题都可保留。
  // 未知 loop、取消执行和缺少身份的记录不能冒充已完成题。
  auto partial_settings = settings;
  partial_settings["task_ids"] = Json::Array{"task-a", "task-b"};
  partial_settings["judge"] = Json{{"loop_review_prompt", "loop.txt"}};
  auto partial = report;
  partial.set_complete(false);
  const auto partial_path = before.root / "evaluations/partial/attempt-1/report.json";
  auto persist = [&](const fs::path &target, const Report &value) {
    const auto serialized = json(value).dump();
    write(target, serialized);
    write(target.parent_path() / "manifest.json",
          Json{{"complete", value.complete()}, {"run_id", value.run_id()},
               {"files", Json{{"report.json", hash(serialized)}}}}.dump());
    before.remember_shared_evaluation(partial_settings, false, target, value);
  };
  persist(partial_path, partial);
  expect(after.shared_evaluation_report(partial_settings, false) == partial_path &&
             after.shared_report_reusable(partial, partial_settings) == 1 &&
             !after.shared_report_complete(partial, partial_settings),
         "部分检查点无法跨作业恢复，或被误判为完整评测");
  auto uncertain = partial;
  (*uncertain.mutable_trials(0)->mutable_evidence()->mutable_fields())["loop_review"] =
      proto<google::protobuf::Value>(Json{{"status", "uncertain"}});
  expect(after.shared_report_reusable(uncertain, partial_settings) == 0,
         "未确认的 loop 指标被当成可复用评分");
  auto judge_failed = partial;
  judge_failed.mutable_trials(0)->set_error_code("evaluation_judge_failed");
  judge_failed.mutable_trials(0)->set_verdict("error");
  auto *model_check = judge_failed.mutable_trials(0)->add_checks();
  model_check->set_check_id("model");
  model_check->set_status("error");
  expect(after.shared_report_reusable(judge_failed, partial_settings) == 1 &&
             !after.shared_report_complete(judge_failed, partial_settings),
         "裁判失败丢失了已完成执行，或被当成有效评分");
  auto missing_metrics = partial;
  missing_metrics.mutable_trials(0)->clear_metrics();
  auto cost_required = after;
  cost_required.config["objective"]["token_weight"] = 0.05;
  expect(cost_required.shared_report_reusable(missing_metrics, partial_settings) == 0,
         "缺少必需 token 的执行被反复复用，无法通过裁判重评恢复");
  cost_required.config["unknown_token_policy"] = "worst_case";
  expect(cost_required.shared_report_reusable(missing_metrics, partial_settings) == 1,
         "显式允许保守成本的配置丢失了可复用执行");
  auto invalid = partial;
  *invalid.add_trials() = invalid.trials(0);
  expect(after.shared_report_reusable(invalid, partial_settings) == 0,
         "重复试验身份通过了检查点筛选");
  invalid = partial;
  invalid.mutable_trials(0)->clear_trial_id();
  expect(after.shared_report_reusable(invalid, partial_settings) == 0,
         "缺失试验身份通过了检查点筛选");
  invalid = partial;
  invalid.mutable_trials(0)->set_execution_status("cancelled");
  expect(after.shared_report_reusable(invalid, partial_settings) == 0,
         "被取消的执行被当成完整执行");
  invalid = partial;
  *invalid.mutable_trials(0)->mutable_evidence() =
      proto<google::protobuf::Struct>(Json{{"loaded_version", "policy-b"}});
  expect(after.shared_report_reusable(invalid, partial_settings) == 0,
         "错误版本的执行通过了检查点筛选");
  auto finished = partial;
  finished.set_complete(true);
  *finished.add_trials() = report.trials(0);
  finished.mutable_trials(1)->set_task_id("task-b");
  finished.mutable_trials(1)->set_trial_id("observed-trial-b");
  const auto finished_path = before.root / "evaluations/partial/attempt-2/report.json";
  persist(finished_path, finished);
  persist(partial_path, partial);
  expect(after.shared_evaluation_report(partial_settings, false) == finished_path,
         "较新的部分检查点覆盖了已完整评分的缓存");
  const auto index = fs::path(directory) / "candidate-cache" /
                     (hash(before.shared_evaluation_identity(settings, false).dump()) + ".json");
  expect(read(index).find("private-marker") == std::string::npos,
         "cache index exposed execution configuration values");
  write(path, bytes + " ");
  expect(after.shared_evaluation_report(settings, false).empty(),
         "changed report bytes passed the cache hash check");
  write(path, bytes);
  write(path.parent_path() / "manifest.json", "{}");
  expect(after.shared_evaluation_report(settings, false).empty(),
         "changed manifest passed the cache hash check");
  write(path.parent_path() / "manifest.json", manifest);
  fs::remove(path);
  expect(after.shared_evaluation_report(settings, false).empty(),
         "deleted source report caused an error instead of a cache miss");
  missing_metric_recovery_cache(directory);
}

// 测试辅助函数：fixed_holdout_baseline_cache。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void fixed_holdout_baseline_cache() {
  char directory[] = "/tmp/trajectory-gepa-holdout-cache-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create holdout cache fixture");

  struct Cleanup {
    fs::path path;

    // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
    ~Cleanup() {
      fs::remove_all(path);
    }
  } cleanup{directory};

  const Json legacy{{"evaluation_context", "execution-v1"},
                    {"candidate_evaluation_context", "runtime-v1"},
                    {"baseline_version", "l0"},
                    {"dataset_version", "data-v1"}};
  const auto legacy_key = hash(legacy.dump());
  const Json identity{{"schema_version", 1},
                      {"evaluation_context", "execution-v1"},
                      {"candidate_evaluation_context", "runtime-v1"},
                      {"split", "holdout"},
                      {"baseline_version", "l0"},
                      {"dataset_version", "data-v1"},
                      {"task_versions", Json{{"h-a", "task-v1"}}},
                      {"repetitions", 1},
                      {"loop_review", true}};
  HoldoutBaselineCache cache{
      directory,
      identity,
      legacy_key,
      1024,
      [](const fs::path &path, std::size_t limit) { return read(path, limit); },
      [](const fs::path &path, const std::string &bytes) { write(path, bytes); },
      [](const std::string &bytes) { return hash(bytes); }};
  write(fs::path(directory) / "baseline-cache" / (legacy_key + ".json"),
        Json{{"identity", legacy}}.dump());
  auto report = [&](const char *variant, const char *version, const char *trial_id, bool passed) {
    Report value;
    value.set_schema_version(1);
    value.set_report_kind("task_evaluation.v1");
    value.set_evaluator_version("task-evaluator-v1");
    value.set_dataset_version("data-v1");
    value.set_run_id(trial_id);
    value.set_complete(true);
    value.set_config_fingerprint("measured-settings");
    auto *trial = value.add_trials();
    trial->set_variant_id(variant);
    trial->set_task_id("h-a");
    trial->set_split("holdout");
    trial->set_task_version("task-v1");
    trial->set_repetition(1);
    trial->set_trial_id(trial_id);
    trial->set_execution_status("completed");
    trial->set_verdict(passed ? "passed" : "failed");
    trial->set_duration_ms(100);
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"loaded_version", version},
             {"loop_review", Json{{"status", "complete"}, {"count", 1}}},
             {"artifacts", Json{{"proof", Json{{"stored_file", "proof.txt"},
                                               {"size_bytes", 5},
                                               {"sha256", hash("proof")}}}}}});
    *value.mutable_configuration() = proto<google::protobuf::Struct>(
        Json{{"splits", Json::Array{"holdout"}},
             {"repetitions", 1},
             {"variants", Json::Array{Json{{"id", variant}, {"version", version}}}}});
    *value.mutable_summary() = proto<google::protobuf::Struct>(
        Json{{"model_calls", 1},
             {"by_variant", Json{{variant, Json{{"trials", 1},
                                                {"scored", 1},
                                                {"passed", passed ? 1 : 0},
                                                {"failed", passed ? 0 : 1},
                                                {"pass_rate", passed ? 1.0 : 0.0},
                                                {"mean_agent_duration_ms", 100}}}}}});
    return value;
  };
  auto save = [&](const fs::path &path, const Report &value) {
    const auto bytes = json(value).dump(), pb = value.SerializeAsString();
    write(path, bytes);
    write(path.parent_path() / "report.pb", pb);
    write(path.parent_path() / "manifest.json",
          Json{{"complete", value.complete()},
               {"run_id", value.run_id()},
               {"files", Json{{"report.json", hash(bytes)}, {"report.pb", hash(pb)}}}}
              .dump());
    for (const auto &trial : value.trials())
      write(path.parent_path() / "artifacts" / trial.trial_id() / "proof.txt", "proof");
  };
  const auto original = report("baseline", "l0", "old-l0-trial", false);
  const auto path = fs::path(directory) / "old/search/evaluations/final/attempt-1/report.json";
  save(path, original);
  const auto tree = Json{
      {"started_ms", 1},
      {"candidate_evaluation_context", "runtime-v1"},
      {"baseline_version", "l0"},
      {"dataset_version", "data-v1"},
      {"baseline_cache", Json{{"key", legacy_key}}},
      {"evaluations", Json{{"final", Json{{"split", "holdout"},
                                          {"status", "complete"},
                                          {"baseline_version", "l0"},
                                          {"report", "evaluations/final/attempt-1/report.json"},
                                          {"report_hash", hash(json(original).dump())}}}}}};
  write(fs::path(directory) / "old/search/tree.json", tree.dump());
  const auto pinned = cache.lookup();
  expect(pinned.str("status") == "hit", "valid historical holdout L0 was not imported");
  expect(cache.load(pinned["source"]).trials(0).trial_id() == "old-l0-trial",
         "L0 execution ID changed during reuse");
  // 同一 dataset/context 下任务集合、版本、split、重复数不同均不能借用旧基线。
  for (int mode = 0; mode < 6; ++mode) {
    auto changed = cache;
    if (mode == 0)
      changed.identity["split"] = "val";
    if (mode == 1)
      changed.identity["task_versions"] = Json{{"v-a", "task-v1"}};
    if (mode == 2)
      changed.identity["task_versions"]["h-a"] = "task-v2";
    if (mode == 3)
      changed.identity["repetitions"] = 2;
    if (mode == 4)
      changed.identity["evaluation_context"] = "execution-v2";
    if (mode == 5)
      changed.identity["candidate_evaluation_context"] = "runtime-v2";
    expect(changed.lookup(pinned).str("status") == (mode == 5 ? "hit" : "invalid"),
           "holdout L0 identity incorrectly followed candidate context or ignored execution mismatch");
  }
  auto invalid = original;
  invalid.mutable_trials(0)->set_split("val");
  *invalid.mutable_configuration() = proto<google::protobuf::Struct>(
      Json{{"splits", Json::Array{"val"}},
           {"repetitions", 1},
           {"variants", Json::Array{Json{{"id", "baseline"}, {"version", "l0"}}}}});
  save(path, invalid);
  bool rejected = false;
  try {
    cache.load(cache.source(path, hash(json(invalid).dump())));
  } catch (const Error &) {
    rejected = true;
  }
  expect(rejected, "a val report became the holdout reference through its baseline identity");
  save(path, original);
  const auto fresh = report("candidate", "policy-new", "new-candidate-trial", true);
  const auto fresh_path = fs::path(directory) / "new/candidate/report.json";
  save(fresh_path, fresh);
  const auto output = fs::path(directory) / "new/combined";
  const auto combined = cache.combine(pinned["source"], fresh, fresh_path, output, "pair");
  expect(combined.complete() && combined.trials_size() == 2 &&
             combined.report_kind() == "gepa_cached_pair.v1" &&
             json(combined.summary()).num("reused_baseline_trials", 0) == 1 &&
             json(combined.summary()).str("comparison_timing") == "historical_baseline" &&
             json(combined.comparisons())["candidate"].num("paired_trials", 0) == 1 &&
             json(combined.comparisons())["candidate"].num("wins", 0) == 1 &&
             json(combined.summary())["by_variant"]["baseline"].num("passed", 1) == 0,
         "paired report altered grades, lost task pairing, or concealed historical timing");
  expect(read(output / "artifacts/old-l0-trial/proof.txt") == "proof" &&
             read(output / "artifacts/new-candidate-trial/proof.txt") == "proof",
         "combined report is not independently readable");
  const auto limited = fs::path(directory) / "new/size-limited";
  rejected = false;
  try {
    cache.combine(pinned["source"], fresh, fresh_path, limited, "small-pair", 1);
  } catch (const Error &error) {
    rejected = std::string(error.what()) == "evaluation_report_size_limit";
  }
  expect(rejected && !fs::exists(limited),
         "oversized pair wrote partial reports or copied artifacts");
  const auto artifact = path.parent_path() / "artifacts/old-l0-trial/proof.txt";
  write(artifact, "other");
  expect(cache.lookup(pinned).str("status") == "invalid",
         "changed locked source silently switched or reran L0");
  write(artifact, "proof");
  write(path.parent_path() / "report.pb", "changed");
  expect(cache.lookup(pinned).str("status") == "invalid",
         "manifest-listed protobuf corruption was ignored");
  save(path, original);
  write(path, json(original).dump() + " ");
  expect(cache.lookup(pinned).str("status") == "invalid", "changed report passed its pinned hash");
}

// 测试辅助函数：multi_turn_feedback_and_intent_observation。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void multi_turn_feedback_and_intent_observation() {
  trajectory::evolution::dataset::v1::EvaluationTask task;
  task.set_task_input("First request");
  task.set_split("train");
  task.set_task_id("multi");
  Json expectations = Json::Array{}, traces = Json::Array{}, grades = Json::Array{};
  for (std::size_t turn = 1; turn <= 20; ++turn) {
    if (turn > 1)
      task.add_followup_inputs(turn == 20 ? "First prepare A, then execute B"
                                          : "Continue " + std::to_string(turn));
    expectations.push(Json{{"turn", turn},
                           {"expected_behavior", "Requirement " + std::to_string(turn)},
                           {"expected_targets", Json::Array{"alpha", "beta"}}});
    traces.push(Json{{"identity", Json{{"run_id", "run-" + std::to_string(turn)}}},
                     {"events", Json::Array{}}});
    grades.push(Json{{"turn", turn},
                     {"verdict", turn == 12 ? "failed" : "passed"},
                     {"reason", "Observed result"}});
  }
  *task.mutable_attributes() =
      proto<google::protobuf::Struct>(Json{{"turn_expectations", expectations}});
  auto &events = traces.items()[0]["events"];
  for (const char *target : {"alpha", "alpha", "gamma"})
    events.push(Json{{"kind", "action.start"},
                     {"operation", target},
                     {"attributes", Json{{"type", "schedule.action"}, {"executed", true}}}});
  events.push(Json{{"kind", "action.start"},
                   {"operation", "beta"},
                   {"attributes", Json{{"type", "schedule.action"}, {"executed", false}}}});
  traces.items()[14]["events"].push(
      Json{{"kind", "advice.applied"}, {"application", Json{{"status", "applied"}}}});
  Json evidence{{"traces", traces}, {"turn_evaluation", grades}};
  const auto grading = turn_grading_context(task, evidence);
  expect(grading.items().size() == 20 && grading.items()[19].str("evidence_path") == "/traces/19",
         "multi-turn standards lost their real evidence mapping");
  Json diagnostic;
  const auto checked = validate_turn_verdicts(grades, grading, evidence, diagnostic);
  expect(checked.items()[11].str("run_id") == "run-12",
         "turn verdict was attributed to a different run");
  auto incomplete = grades;
  incomplete.items().pop_back();
  bool rejected = false;
  try {
    validate_turn_verdicts(incomplete, grading, evidence, diagnostic);
  } catch (const Error &) {
    rejected = true;
  }
  expect(rejected, "partial turn grading became full acceptance");
  auto rows = observe_turn_intents(task, evidence);
  expect(rows.items()[0]["actual_targets"].items().size() == 2 &&
             rows.items()[0]["missing"].items()[0].text() == "beta" &&
             rows.items()[0]["extra"].items()[0].text() == "gamma",
         "dispatch intents counted retries or skipped actions");
  expect(rows.items()[1].str("status") == "dispatch_unobserved",
         "unobserved dispatch became an empty observed set");
  auto metrics = summarize_intents(rows);
  expect(metrics.num("measured_turns", 0) == 1 && metrics.num("missing_trace_turns", 0) == 0 &&
             metrics.num("dispatch_unobserved_turns", 0) == 19,
         "unobserved turns entered the intent denominator");
  expectations.items()[0]["conditional"] = true;
  *task.mutable_attributes() =
      proto<google::protobuf::Struct>(Json{{"turn_expectations", expectations}});
  metrics = summarize_intents(observe_turn_intents(task, evidence));
  expect(metrics.num("conditional_turns", 0) == 1 && metrics.num("measured_turns", 99) == 0 &&
             metrics["recall"].null(),
         "conditional routing was scored as unconditional intent failure");
  const auto context = feedback_turn_context(task, evidence);
  std::set<std::size_t> turns;
  for (const auto &row : context["turns"].items())
    turns.insert(row.num("turn", 0));
  expect(turns.count(20) && turns.count(12) && turns.count(11) && turns.count(15) &&
             turns.count(14) && turns.size() <= 8 &&
             context.num("omitted_turns", 0) == 20 - turns.size(),
         "bounded reflection lost later failures, applied rules or the latest revision");
  Search search;
  *search.dataset.add_tasks() = task;
  expect(search.schedule_training_priority("multi") > 0,
         "later-turn orchestration could not be sampled");
  search.dataset.mutable_tasks(0)->set_split("holdout");
  expect(search.schedule_training_priority("multi") == 0,
         "held-out dialogue leaked into training sampling");
}

// 参数与结果必须按调用身份对应，不能按并发返回顺序或工具名配对。
// 本用例直接检查裁判视图，不调用模型；原始轨迹、副作用和执行顺序均保持不变。
void judge_action_request_pairing() {
  const auto original = Json::parse(R"({
    "traces":[{"events":[
      {"event_id":"begin","kind":"run.start"},
      {"event_id":"a0","kind":"action.start","action_id":"a","actor_id":"alice","execution_id":"left","operation":"query","input":{"query":"target-A"}},
      {"event_id":"b0","kind":"action.start","action_id":"b","actor_id":"bob","execution_id":"right","operation":"query","input":{"query":"target-B"}},
      {"event_id":"b1","kind":"action.end","action_id":"b","actor_id":"bob","execution_id":"right","output":{"results":[]}},
      {"event_id":"a1","kind":"action.end","action_id":"a","actor_id":"alice","execution_id":"left","input":{"query":"executed-A"},"output":{"results":["found"]}},
      {"event_id":"m0","kind":"action.start","action_id":"m","attributes":{"semantic":"model","metric_tool_call":true},"input":{"messages":["model-only-request"]}},
      {"event_id":"m1","kind":"action.end","action_id":"m","attributes":{"semantic":"model"},"output":{"choices":[]}},
      {"event_id":"orphan","kind":"action.end","action_id":"missing","input":{"query":"only-observed-input"},"output":[]}
    ]}]
  })");
  const auto original_bytes = original.dump();
  auto view = original;
  auto &events = view["traces"].items()[0]["events"].items();
  events.erase(events.begin()); // 模拟裁判视图删去传输事件，引用下标仍须回到原始位置。
  bind_judge_action_requests(view);
  expect(events[2]["request"].str("event_id") == "b0" &&
             events[2]["request"]["input"].str("query") == "target-B" &&
             events[3]["request"].str("event_id") == "a0" &&
             events[3]["request"]["input"].str("query") == "target-A",
         "并发返回与另一个调用的参数串用");
  expect(events[3]["input"].str("query") == "executed-A" &&
             !events[5].has("request") && !events[6].has("request") &&
             events[6]["input"].str("query") == "only-observed-input",
         "参数变化、模型请求或缺失开始事件被错误改写");
  expect(events[2].str("event_id") == "b1" && events[3].str("event_id") == "a1" &&
             original.dump() == original_bytes,
         "参数配对修改了执行顺序或原始轨迹");
  expect(original_judge_path("/traces/0/events/2/request/input/query", view, original) ==
             "/traces/0/events/2/input/query" &&
             original_judge_path("/traces/0/events/3/request", view, original) ==
             "/traces/0/events/1" &&
             original_judge_path("/traces/0/events/3/input/query", view, original) ==
             "/traces/0/events/4/input/query",
         "派生请求或实际结束参数的引用没有映射回原始事件");

  // trace 单对象入口与 traces 多轮入口共用相同绑定与引用规则。
  auto single = Json{{"trace", view["traces"].items()[0]}};
  const auto raw_single = Json{{"trace", original["traces"].items()[0]}};
  expect(original_judge_path("/trace/events/2/request/input", single, raw_single) ==
             "/trace/events/2/input",
         "单 Run 请求引用没有还原");
  bind_judge_action_requests(single);
  expect(!single.has("traces"), "单 Run 投影不应凭空添加 traces 字段");

  // 相同 action_id 在不同 Run 内可以复用，但不能借用前一 Run 的请求。
  auto multi = original;
  auto other = original["traces"].items()[0];
  other["events"].items()[1]["input"]["query"] = "second-run";
  multi["traces"].push(other);
  bind_judge_action_requests(multi);
  expect(multi["traces"].items()[1]["events"].items()[4]["request"]["input"]
             .str("query") == "second-run" &&
             multi["traces"].items()[0]["events"].items()[4]["request"]["input"]
             .str("query") == "target-A",
         "跨 Run 的相同调用 ID 串用了实参");

  // 重复开始事件、矛盾执行身份均不猜测配对；缺少派生 request 不代表未执行。
  auto ambiguous = original;
  auto duplicate = ambiguous["traces"].items()[0]["events"].items()[1];
  duplicate["event_id"] = "duplicate-start";
  ambiguous["traces"].items()[0]["events"].push(duplicate);
  bind_judge_action_requests(ambiguous);
  expect(!ambiguous["traces"].items()[0]["events"].items()[4].has("request") &&
             ambiguous["traces"].items()[0]["events"].items()[3].has("request"),
         "重复 ID 被任意挑选，或牵连了独立分支");
  auto mismatch = original;
  mismatch["traces"].items()[0]["events"].items()[4]["execution_id"] = "right";
  bind_judge_action_requests(mismatch);
  expect(!mismatch["traces"].items()[0]["events"].items()[4].has("request"),
         "矛盾的执行实例身份仍被配对");

  // 原业务已占用同名字段时不覆盖、不重解释，也不将伪造引用变成另一调用的证据。
  auto occupied = original;
  occupied["traces"].items()[0]["events"].items()[4]["request"] = Json{{"custom", "business"}};
  const auto occupied_raw = occupied;
  bind_judge_action_requests(occupied);
  expect(occupied["traces"].items()[0]["events"].items()[4]["request"].str("custom") == "business" &&
             original_judge_path("/traces/0/events/4/request/custom", occupied, occupied_raw) ==
                 "/traces/0/events/4/request/custom",
         "原业务同名字段被当作派生引用");
  auto forged = view;
  forged["traces"].items()[0]["events"].items()[3]["request"]["event_id"] = "b0";
  bool rejected = false;
  try {
    original_judge_path("/traces/0/events/3/request/input", forged, original);
  } catch (const Error &) {
    rejected = true;
  }
  expect(rejected, "派生引用可以跨调用伪造来源");

  // 正式压缩器也必须保留工具结束时的实际参数，只有显式模型请求可以省略。
  rapidjson::Document reduced;
  reduced.Parse(original_bytes.data(), original_bytes.size());
  agentflow::trajectory::model_input::task_evidence_view(reduced, false);
  auto compact = Json::parse(agentflow::trajectory::model_input::bytes(reduced));
  bind_judge_action_requests(compact);
  for (const auto &event : compact["traces"].items()[0]["events"].items()) {
    if (event.str("event_id") == "a1")
      expect(event["input"].str("query") == "executed-A", "压缩丢失了执行时修正的参数");
    if (event.str("event_id") == "orphan")
      expect(event["input"].str("query") == "only-observed-input", "压缩丢失了唯一参数证据");
    if (event.str("event_id") == "m1")
      expect(!event.has("request"), "模型请求被当作工具请求带入裁判");
  }
}

// 对照连续完成度、关键约束和未知状态，验证搜索没有退回“一处失败全题零分”。
// 同时检查分批裁判的证据映射和反思摘要，防止评分有效但引用错轮或反馈缺失。
void objective_search_scores() {
  // 先检查纯目标聚合：不违规本身不能抬高完成度；关键违规保留独立失败。
  const Json fixed_goals =
      Json::Array{Json{{"id", "work"}, {"turn", 1}, {"due_turn", 1}, {"weight", 1}},
                  Json{{"id", "constraint-1"},
                       {"turn", 1},
                       {"due_turn", 1},
                       {"weight", 1},
                       {"constraint", true},
                       {"critical", true}}};
  auto judgments =
      Json{{"work", Json{{"status", "failed"}}}, {"constraint-1", Json{{"status", "passed"}}}};
  expect(number(summarize_goals(fixed_goals, judgments)["quality"]) == 0,
         "未执行任务不能仅靠不违规获得完成分");
  judgments["work"]["status"] = "passed";
  judgments["constraint-1"]["status"] = "failed";
  expect(summarize_goals(fixed_goals, judgments).num("critical_failures", 0) == 1,
         "关键约束失败必须保留");

  // 裁剪视图删除了开始事件后，可见下标 0 仍须映射到原始下标 1。
  const Json raw_trace{{"events", Json::Array{Json{{"event_id", "begin"}, {"kind", "run.start"}},
                                              Json{{"event_id", "finish"},
                                                   {"kind", "run.end"},
                                                   {"output", Json{{"state", Json{{"value", 1}}}}}},
                                              Json{{"event_id", "late"}, {"kind", "custom"}}}}};
  const Json traces = Json::Array{raw_trace, raw_trace};
  const Json raw{{"traces", Json::Array{raw_trace}}};
  const Json view{
      {"traces", Json::Array{Json{{"events", Json::Array{raw_trace["events"].items()[1]}}}}}};
  expect(original_judge_path("/traces/0/events/0/output/state/value", view, raw) ==
             "/traces/0/events/1/output/state/value",
         "裁剪导致证据指向错误事件");
  expect(terminal_event_index(raw_trace) == 1, "不能把尾部自定义事件当作运行结束");
  expect(global_batch_path("/state/value", Json::Object{}, traces, 1, 2) ==
             "/traces/1/events/1/output/state/value",
         "状态引用归到了错误轮次");
  const Json part{{"context_evidence", Json::Array{Json{{"source", "history"},
                                                        {"path", "/traces/0/events/1/output"}}}}};
  expect(global_batch_path("/context_evidence/0/value/state/value", part, traces, 1, 2) ==
             "/traces/0/events/1/output/state/value",
         "历史补查被错误归为当前轮");
  auto large = Json{{"output", Json{{"text", "开头说明" + std::string(9000, 'x') + "祝您面试顺利"}}}};
  bound_judge_payload(large, "", 256);
  expect(large["output"]["text"].flag("preview_only", false) &&
             large["output"]["text"].str("path") == "/output/text",
         "长字段预览缺少原始路径");
  const auto preview = large["output"]["text"].str("preview");
  expect(preview.size() <= 256 && preview.find("开头说明") == 0 &&
             preview.find("祝您面试顺利") != std::string::npos &&
             preview.find("中段省略") != std::string::npos,
         "限长预览必须同时保留回复开头和结尾，且不扩大预算");
  std::string chinese;
  for (int i = 0; i < 300; ++i)
    chinese += "中文";
  for (const auto limit : {0u, 1u, 63u, 64u, 65u, 256u}) {
    auto utf8 = Json{{"output", Json{{"text", chinese}}}};
    bound_judge_payload(utf8, "", limit);
    const auto roundtrip = proto<google::protobuf::Struct>(utf8);
    expect(!roundtrip.fields().empty() && utf8["output"]["text"].str("preview").size() <= limit,
           "中文裁剪必须保持 UTF-8 完整及原字节上限");
  }

  // 答复只保留一份较完整的验收视图；相同文字在工具轨迹中仍遵守小字段预算。
  const auto answer = std::string(700, 'a') + "中段任务交付事实" + std::string(700, 'b');
  auto focused = Json{{"output", Json{{"response", answer}}},
                      {"traces", Json::Array{Json{{"events", Json::Array{
                          Json{{"output", Json{{"response", answer}}}}}}}}}};
  auto bounded_answer = focused;
  bound_judge_payload(focused, "", 256, 4096);
  expect(focused["output"].str("response") == answer,
         "最终答复在独立预算内不应丢失中段交付事实");
  expect(focused["traces"].items()[0]["events"].items()[0]["output"]["response"]
             .flag("preview_only", false),
         "提高答复预算不能扩大所有工具输出");
  bound_judge_payload(bounded_answer, "", 256, 512);
  expect(bounded_answer["output"]["response"].flag("preview_only", false) &&
             bounded_answer["output"]["response"].str("preview").size() <= 512,
         "超出独立答复预算后仍需限长并保留补查路径");

  // 未知答案补查只定位本批对应轮；不带入已通过目标、历史或输入字段。
  auto lookup_view = Json{{"output", Json::Object{}}};
  lookup_view["output"]["turns"] = Json::Array{
      Json{{"answer", std::string(1000, 'a')}},
      Json{{"answer", std::string(1000, 'b')}}};
  bound_judge_payload(lookup_view, "", 256);
  const Json lookup_scope{{"turn_begin", 3}, {"turn_end", 4},
                          {"goals", Json::Array{Json{{"turn", 2}},
                                                Json{{"turn", 3}}, Json{{"turn", 4}}}}};
  const Json lookup_rows = Json::Array{Json{{"status", "uncertain"}},
                                       Json{{"status", "passed"}},
                                       Json{{"status", "uncertain"}}};
  const auto refs = answer_previews_for_unknown_goals(lookup_view, lookup_scope, lookup_rows);
  expect(refs.items().size() == 1 &&
             refs.items()[0].str("path") == "/output/turns/1/answer",
         "自动补查只能读取当前未知目标的答案，不能跨轮扩大输入");
  lookup_view["output"]["turns"].items()[1]["answer"]["path"] = "/output/turns/0/answer";
  expect(answer_previews_for_unknown_goals(lookup_view, lookup_scope, lookup_rows).items().empty(),
         "不能通过裁剪标记伪造跨轮补查路径");
  lookup_view["output"]["turns"].items()[1]["answer"]["preview_only"] = "业务字符串";
  expect(answer_previews_for_unknown_goals(lookup_view, lookup_scope, lookup_rows).items().empty(),
         "业务同名字段不能被误认为程序生成的裁剪标记");
  auto many = Json{{"output", Json::Object{}}};
  for (int i = 0; i < 10; ++i)
    many["output"]["field-" + std::to_string(i)] = std::string(1000, 'x');
  bound_judge_payload(many, "", 256);
  const Json single_scope{{"turn_begin", 1}, {"turn_end", 1},
                          {"goals", Json::Array{Json{{"turn", 1}}, Json{{"turn", 1}}}}};
  const Json duplicate_rows = Json::Array{Json{{"status", "uncertain"}},
                                          Json{{"status", "uncertain"}}};
  expect(answer_previews_for_unknown_goals(many, single_scope, duplicate_rows).items().size() == 4,
         "重复目标不应重复补查，单次补查仍最多四个字段");

  // 两个目标中一个失败仍提供 0.5 的搜索信号；严格通过率继续为零。
  Search search;
  search.config = Json{
      {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}, {"latency_weight", 0}}}};
  Report report;
  report.set_complete(true);
  auto *trial = report.add_trials();
  trial->set_task_id("partial");
  trial->set_variant_id("candidate");
  // 该夹具表示 Agent 已完成执行、但只完成了部分业务目标；执行状态不能省略，
  // 否则会被运行时失败保护正确地排除，测不到连续目标完成度本身。
  trial->set_execution_status("completed");
  trial->set_verdict("failed");
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"assessment", Json{{"coverage", 0.5}}}});
  expect(incomplete_goal_assessment(trial->evidence()),
         "整题失败不能掩盖子目标未知");
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"assessment", Json{{"coverage", 1.0}}}});
  expect(!incomplete_goal_assessment(trial->evidence()),
         "已确定的业务失败不能反复重评");
  Json assessment{{"version", "goal-assessment-v1"},
                  {"quality", 0.5},
                  {"quality_upper", 0.5},
                  {"coverage", 1},
                  {"critical_failures", 0}};
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"assessment", assessment}});
  const auto scores = search.scores(report);
  expect(number(scores["partial"]["quality"]) == 0.5 &&
             number(scores["partial"]["strict_pass_rate"]) == 0,
         "goal completion was collapsed back to binary acceptance");
  assessment["goals"] = Json::Array{
      Json{{"id", "done"}, {"status", "passed"}},
      Json{{"id", "inapplicable"}, {"status", "not_applicable"}},
      Json{{"id", "missing"}, {"turn", 2}, {"status", "failed"}, {"reason", "遗漏第二项任务"}}};
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"assessment", assessment}});
  const auto feedback = feedback_trial(*trial, Json::Object{});
  expect(feedback["goal_completion"]["issues"].items().size() == 1 &&
             feedback["goal_completion"]["issues"].items()[0].str("id") == "missing",
         "反思缺少未完成目标的具体原因");
  expect(
      control_feedback_trial(*trial, Json::Array{"loop_rules"}, Json::Object{})["goal_completion"]
              .dump() == feedback["goal_completion"].dump(),
      "控制策略反思丢失目标摘要");
  assessment["critical_failures"] = 1;
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"assessment", assessment}});
  expect(number(search.scores(report)["partial"]["quality"]) == 0,
         "critical failure was offset by ordinary goals");
  assessment["critical_failures"] = 0;
  assessment["coverage"] = 0.5;
  assessment["quality_upper"] = 1;
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"assessment", assessment}});
  expect(!scored(report) && search.scores(report).fields().empty(),
         "unknown evidence gained search advantage");

  // 一条试验缺少必需成本遥测时，只能让该题未决，不能清空同批其他有效题。
  Search isolated;
  isolated.config =
      Json{{"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0.05},
                               {"latency_weight", 0}, {"token_scale", 10000}}},
           {"unknown_token_policy", "reject"}};
  Report batch;
  batch.set_complete(true);
  auto add = [&](const std::string &id, bool with_tokens) {
    auto *row = batch.add_trials();
    row->set_trial_id("trial-" + id);
    row->set_task_id(id);
    row->set_variant_id("candidate");
    row->set_execution_status("completed");
    row->set_verdict("passed");
    *row->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"assessment", Json{{"version", "goal-assessment-v1"}, {"quality", 1},
                                   {"quality_upper", 1}, {"coverage", 1},
                                   {"critical_failures", 0}}}});
    if (with_tokens)
      *row->mutable_metrics() = proto<google::protobuf::Struct>(Json{{"tokens", 1000}});
  };
  add("complete", true);
  add("missing-token", false);
  expect(isolated.scores(batch).fields().empty(),
         "an incomplete evaluation batch entered parent-child selection");
  Report reusable;
  reusable.set_complete(true);
  *reusable.add_trials() = batch.trials(0);
  const auto complete = isolated.scores(reusable);
  expect(complete.fields().size() == 1 && complete.has("complete"),
         "a valid task could not be reused independently from an incomplete batch");
}

// 自然语言排序提示只能提高优先级，不能删除没有“先/再”字样的真实编排机会。
void schedule_sampling_hint_preserves_other_evidence() {
  namespace te = trajectory::evolution;
  Search search;
  search.seed = public_alias_fixture();
  const auto old_policy = proto<te::v1::RuntimePolicy>(search.seed["policy"]);
  auto options = old_policy.options();
  options.add_host_capabilities("schedule.dispatch");
  search.seed["policy"] = json(te::compile_runtime_policy(old_policy.snapshot(), options));
  search.controls = Json{{"enabled", true}, {"prompt_file", "test.txt"}};
  validate_control_learning(search.controls);
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 4},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  Report feedback;
  for (unsigned i = 0; i < 3; ++i) {
    auto *task = search.dataset.add_tasks();
    task->set_task_id("task-" + std::to_string(i));
    task->set_split("train");
    task->set_task_input(i == 0 ? "先查今日天气，再查明日天气"
                              : i == 1 ? "制作宣传图片及使用该图片的视频" : "查询天气和列出提醒");
    task->set_expected_behavior("完成两项工作并交付结果");
    Json actions = Json::Array{
        Json{{"action_id", "a"}, {"operation", i == 1 ? "make_image" : "query"}},
        Json{{"action_id", "b"}, {"operation", i == 1 ? "render_video" : "list"}}};
    if (i == 1) {
      actions.items()[0]["arguments"] = Json{{"output_path", "assets/poster.png"}};
      actions.items()[1]["arguments"] = Json{{"input_path", "assets/poster.png"}};
    }
    const Json query{{"boundary", "schedule"}, {"execution_id", "batch"},
                     {"advice_query", Json{{"target_scope", "assistant"},
                                            {"task", task->task_input()}}},
                     {"actions", actions}};
    const Json event{{"kind", "custom"}, {"event_id", "query"}, {"step_id", "batch"},
                     {"attributes", Json{{"type", "policy.query"},
                       {"decision", Json{{"query", query},
                                         {"schedule", Json{{"status", "unchanged"}}}}}}}};
    auto *trial = feedback.add_trials();
    trial->set_trial_id(task->task_id());
    trial->set_task_id(task->task_id());
    trial->set_variant_id("candidate");
    trial->set_execution_status("completed");
    trial->set_verdict("failed");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", task->task_id()}}},
                            {"events", Json::Array{event}}}}});
  }
  const auto packet = search.packet(search.seed, feedback, Json::Array{"schedule_rules"});
  expect(packet["feedback"].items().size() == 3,
         "排序提示删掉了没有特定关键词的编排证据");
  expect(packet["components"]["schedule_rules"].items().empty() &&
             packet["controls"]["allowed_update_operations"]["schedule_rules"].dump() ==
                 Json(Json::Array{"add"}).dump(),
         "空编排组件仍向反思开放了 set/remove");
  expect(packet["feedback"].items().front().str("task_id") == "task-0",
         "取消硬过滤时丢失了原有优先级");
  const auto &resource = packet["feedback"].items()[1]["trial"]["evidence"]["runs"]
      .items().front()["schedule_plans"].items().front()["actions"];
  expect(resource.items()[0]["arguments"].str("output_path") == "assets/poster.png" &&
             resource.items()[1]["arguments"].str("input_path") == "assets/poster.png",
         "低优先级样本中的真实资源依赖被丢失或改写");

  // 明确顺序与真实输出→输入依赖可以搜索；独立并行不能凭空串行化。
  // 此基础夹具只有 L1 经验；编排更新必须另有获准的编排来源，不能借用 L1 的 memory_id。
  auto binding_input = packet;
  binding_input["controls"]["control_sources"] = Json::Array{Json{
      {"memory_id", "schedule-source"}, {"scope", "assistant"},
      {"target_type", "CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION"}}};
  const auto prepared = observed_operation_bindings(binding_input, 65536);
  const auto &bindings = prepared["controls"]["editable_bindings"];
  expect(bindings.items().size() == 2 && search.control_evidence(packet, "schedule_rules"),
         "明确顺序或资源依赖没有形成可编辑顺序绑定");
  // Node26 的真实形态是多个独立并行计划，未见任何生产/消费或先后要求。
  // 此时即使轨迹很长，也不应制造两个任意方向的候选边。
  std::set<std::pair<std::string, std::string>> independent_orders;
  for (const auto &entry : bindings.items())
    if (entry.str("task_text") == "查询天气和列出提醒")
      independent_orders.emplace(entry.str("before_operation"), entry.str("after_operation"));
  expect(independent_orders.empty(), "独立同批操作被错误登记为串行候选");
  for (const auto &binding : bindings.items())
    expect(binding.str("primitive_id") == "order_operations" &&
               !binding.flag("require_success", true),
           "普通顺序被错误转换为前置成功要求");
  const auto &binding = bindings.items()[0];
  const auto draft = resolve_control_binding_updates(
      Json{{"reason", "测试同批顺序候选"}, {"control_binding_updates", Json::Array{Json{
          {"binding_id", binding["binding_id"]},
          {"memory_id", "schedule-source"},
          {"task_terms", Json::Array{"天气"}}, {"reason", "比较同批顺序的实际收益"}}}}}, prepared);
  expect(draft["control_updates"].items().size() == 1 &&
             draft["control_updates"].items()[0].str("component") == "schedule_rules",
         "真实顺序绑定不能生成控制提案");

  for (const auto &task : search.dataset.tasks())
    search.train.push_back(task.task_id());
  search.remember_opportunities(search.seed, feedback);
  expect(search.tree["training_opportunities"]["schedule_rules"].fields().size() == 2,
         "机会记录把独立操作误算为可执行编排改动");
  auto independent = search;
  *independent.dataset.mutable_tasks(2)->mutable_attributes() =
      proto<google::protobuf::Struct>(Json{{"dependency", "independent"}});
  expect(!independent.schedule_training_contract("task-2").flag("order_editable", true) &&
             !independent.schedule_training_contract("task-2").flag("require_success", true),
         "显式独立操作仍被开放为串行候选");

  auto node26 = packet;
  node26["feedback"] = Json::Array{packet["feedback"].items()[2]};
  auto &independent_plans = node26["feedback"].items()[0]["trial"]["evidence"]["runs"]
      .items()[0]["schedule_plans"];
  const auto independent_plan = independent_plans.items().front();
  while (independent_plans.items().size() < 19)
    independent_plans.push(independent_plan);
  expect(!search.control_evidence(node26, "schedule_rules") &&
             !observed_operation_bindings(node26, 65536)["controls"]["editable_bindings"].array(),
         "Node26 形态的独立并行计划仍占用编排反思轮次");
  node26["components"]["schedule_rules"] = Json::Array{Json{
      {"memory_id", "schedule-source"}, {"before_operation", "query"},
      {"after_operation", "list"}}};
  independent_plans.items()[0]["effective_plan"]["rule_ids"] = Json::Array{"existing-rule"};
  expect(search.control_evidence(node26, "schedule_rules"),
         "修复独立假机会时丢失了移除已有错误编排边的机会");

  // 没有同批两个操作、已有同方向依赖时，不凭任务文本制造不存在的编辑机会。
  auto single = packet;
  single["feedback"] = Json::Array{packet["feedback"].items()[0]};
  auto &plan = single["feedback"].items()[0]["trial"]["evidence"]["runs"]
      .items()[0]["schedule_plans"].items()[0];
  plan["actions"].items()[1]["after"] = Json::Array{Json{{"action_id", "a"}}};
  expect(!search.control_evidence(single, "schedule_rules"),
         "已有依赖仍被重复登记为新增顺序机会");
  plan["actions"].items().pop_back();
  expect(!search.control_evidence(single, "schedule_rules"),
         "单个操作被登记为编排机会");

  // 已有 B→中间步骤→A 时，再建议 A→B 会形成环，即使没有直接的 B→A 边。
  auto transitive = packet;
  transitive["feedback"] = Json::Array{packet["feedback"].items()[0]};
  auto &chain = transitive["feedback"].items()[0]["trial"]["evidence"]["runs"]
      .items()[0]["schedule_plans"].items()[0]["actions"];
  chain.items()[0]["after"] = Json::Array{Json{{"action_id", "middle"}}};
  chain.push(Json{{"action_id", "middle"}, {"operation", "prepare"},
                  {"after", Json::Array{Json{{"action_id", "b"}}}}});
  expect(!search.control_evidence(transitive, "schedule_rules"),
         "忽略传递依赖而生成成环的编排候选");

  // 当前轮 conditional 不能污染下一轮纯顺序；两者仍保留在训练反馈中。
  auto *multi = search.dataset.mutable_tasks(0);
  multi->add_followup_inputs("先整理记录，再发送报告");
  *multi->mutable_attributes() = proto<google::protobuf::Struct>(Json{
      {"turn_expectations", Json::Array{
          Json{{"turn", 1}, {"expected_behavior", "按结果条件决定后续查询"}, {"conditional", true}},
          Json{{"turn", 2}, {"expected_behavior", "先整理记录，再发送报告"}, {"conditional", false}}}}});
  const auto first = search.schedule_training_contract("task-0", multi->task_input());
  const auto followup = search.schedule_training_contract("task-0", multi->followup_inputs(0));
  expect(!first.flag("order_editable", true) && first.flag("result_conditional", false) &&
             followup.flag("order_editable", false) &&
             followup.flag("ordering_required", false) &&
             !followup.flag("require_success", true),
         "多轮结果条件被错误跨轮传播，或被转换为成功依赖");
  search.tree["training_opportunities"] = Json::Object{};
  search.remember_opportunities(search.seed, feedback);
  const auto conditional_packet = search.packet(search.seed, feedback, Json::Array{"schedule_rules"});
  const auto conditional_bindings = observed_operation_bindings(conditional_packet, 65536);
  expect(conditional_packet["feedback"].items().size() == 3 &&
             conditional_bindings["controls"]["editable_bindings"].items().size() == 1 &&
             !search.tree["training_opportunities"]["schedule_rules"].has("task-0"),
         "conditional 被登记为虚假成功依赖，或通过删题掩盖不支持的结果条件");
}

// 历史失败反馈必须在长轨迹填满请求前获得小额预算；同时保留当前证据与授权隔离。
void reflection_history_budget_reservation() {
  Search search;
  search.seed = public_alias_fixture();
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 4},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const Json selected = Json::Array{"experience:private"};
  const auto available = search.components(search.seed);
  Json background = Json::Object{};
  for (const auto &component : available.fields())
    if (component.first != "experience:private" && !component.second.null() &&
        !(component.second.array() && component.second.items().empty()))
      background[component.first] = hash(component.second.dump());
  Json rejection{{"id", "previous-regression"}, {"status", "regressed"},
                 {"attribution", "conditional_component"},
                 {"before", Json{{"experience:private", available["experience:private"]}}},
                 {"after", Json{{"experience:private", Json()}}},
                 {"background_fingerprint", hash(background.dump())},
                 {"positive_tasks", 0}, {"negative_tasks", 4}};
  auto *task = search.dataset.add_tasks();
  task->set_task_id("training");
  task->set_split("train");
  task->set_task_input("查询并如实报告结果");
  task->set_expected_behavior("正确执行查询并输出结果，不把模拟数据当作实时信息");
  Report feedback;
  auto *trial = feedback.add_trials();
  trial->set_task_id("training");
  trial->set_trial_id("training");
  trial->set_variant_id("candidate");
  trial->set_execution_status("completed");
  trial->set_verdict("failed");
  Json events = Json::Array{};
  for (unsigned i = 0; i < 12; ++i)
    events.push(Json{{"kind", "action.end"}, {"event_id", "event-" + std::to_string(i)},
                     {"step_id", "step-" + std::to_string(i)}, {"operation", "query"},
                     {"actor_id", "worker"}, {"execution_id", "branch"},
                     {"status", "completed"}, {"attributes", Json{{"semantic", "action"}}},
                     {"output", "执行证据-" + std::string(1200, 'x')}});
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"trace", Json{{"identity", Json{{"run_id", "run"}}}, {"events", events}}},
           {"output", "已完成查询，但尚未交付用户要求的文件"}});
  const auto source_before = feedback.SerializeAsString();
  const auto without_history = search.packet(search.seed, feedback, selected);
  // 模拟真实边界：原请求可以放下，追加一条简短反证就超限。
  const auto limit = without_history.dump().size() + 16;
  search.config["max_input_bytes"] = static_cast<double>(limit);
  search.reflection_history = Json{{"records", Json::Array{rejection}}};
  const auto packet = search.packet(search.seed, feedback, selected);
  expect(packet.dump().size() <= limit && packet["feedback"].items().size() == 1,
         "历史反证超出总输入预算或挤掉所有当前训练证据");
  expect(packet["prior_search_rejections"].items().size() == 1 &&
             packet["prior_search_rejections"].items()[0].str("record_id") == "previous-regression" &&
             packet["prior_search_rejections"].items()[0]["attempted_change"]["experience:private"].null(),
         "长轨迹挤掉了匹配的历史失败反馈");
  expect(packet["feedback"].items()[0]["trial"].str("verdict") == "failed" &&
             packet["feedback"].items()[0].str("expected_behavior") == task->expected_behavior(),
         "预算调整改变评分或丢失任务验收目标");

  // 不同组件或不同父策略背景的反证不能混进来，更不能因为预留而改变原请求。
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto unrelated = rejection;
    if (mode == 0)
      unrelated["background_fingerprint"] = "different-background";
    if (mode == 1)
      unrelated["before"]["experience:private"] = "different-parent";
    if (mode == 2) {
      unrelated["before"] = Json{{"experience:shared-independent",
                                  available["experience:shared-independent"]}};
      unrelated["after"] = Json{{"experience:shared-independent", Json()}};
    }
    search.reflection_history["records"] = Json::Array{unrelated};
    const auto unrelated_packet = search.packet(search.seed, feedback, selected);
    if (unrelated_packet.dump() != without_history.dump()) {
      const auto expected = without_history.dump(), actual = unrelated_packet.dump();
      std::size_t first_difference = 0;
      while (first_difference < expected.size() && first_difference < actual.size() &&
             expected[first_difference] == actual[first_difference])
        ++first_difference;
      std::cerr << "unrelated_history mode=" << mode << " expected_bytes=" << expected.size()
                << " actual_bytes=" << actual.size() << " first_difference=" << first_difference
                << " expected_tail=" << expected.substr(first_difference, 250)
                << " actual_tail=" << actual.substr(first_difference, 250) << '\n';
    }
    expect(unrelated_packet.dump() == without_history.dump(),
           "不相关历史占用了预算或污染当前组件反思");
  }
  expect(feedback.SerializeAsString() == source_before, "组装反思请求修改了原始执行证据");

  // 当任务说明已用尽预算，历史是可选加速信息；退还它后应完整恢复原请求，而非留下一份
  // 被前一次失败装填裁剪过的证据。无需再次执行 Agent 或调用模型。
  task->set_task_input(std::string(4096, 'q'));
  task->set_expected_behavior(std::string(4096, 'r'));
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"trace", Json{{"identity", Json{{"run_id", "empty-run"}}},
                           {"events", Json::Array{}}}}});
  search.config["max_input_bytes"] = 65536;
  search.reflection_history = Json();
  const auto minimal = search.packet(search.seed, feedback, selected);
  search.config["max_input_bytes"] = minimal.dump().size();
  Json records = Json::Array{};
  for (unsigned i = 0; i < 4; ++i) {
    auto record = rejection;
    record["id"] = "prior-" + std::to_string(i);
    records.push(record);
  }
  search.reflection_history = Json{{"records", records}};
  expect(search.packet(search.seed, feedback, selected).dump() == minimal.dump(),
         "历史预算回退没有恢复原本可放入的当前证据");
}

// 控制反思不得读回种子中的旧L1正文，尤其不能复活父节点已经停用的注入。
void reflection_sources_follow_current_parent() {
  Search search;
  search.seed = public_alias_fixture();
  search.controls = Json{{"enabled", true}, {"prompt_file", "unused"}};
  search.config = Json{{"max_input_bytes", 65536},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const auto original = search.seed.dump();
  auto *task = search.dataset.add_tasks();
  task->set_task_id("source-consistency");
  task->set_split("train");
  task->set_task_input("缺少图片时先核验输入，独立任务继续");
  Report feedback;
  feedback.set_complete(true);
  auto *trial = feedback.add_trials();
  trial->set_task_id(task->task_id());
  trial->set_trial_id(task->task_id());
  trial->set_variant_id("candidate");
  trial->set_verdict("failed");
  trial->set_execution_status("completed");
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"trace", Json{{"identity", Json{{"run_id", "observed-run"}}},
                          {"events", Json::Array{}}}}});
  const Json selected = Json::Array{"experience:private"};
  const auto rewritten = search.mutate(search.seed,
      Json{{"reason", "仅修改当前父策略"},
           {"experience", Json{{"memory_id", "private"}, {"content", "当前父策略的新正文"},
                                {"trigger_pattern", "当前父策略的新适用条件"}}}}, selected);
  const auto packet = search.packet(rewritten, feedback, Json::Array{"action_rules"});
  bool found = false;
  for (const auto &source : packet["experiences"].items())
    if (source.str("memory_id") == "private") {
      found = source.str("content") == "当前父策略的新正文" &&
              source.str("condition") == "当前父策略的新适用条件";
    }
  expect(found, "控制反思读取了种子旧正文或旧条件，而非当前父策略");
  expect(!packet["components"].has("experience:private"),
         "只读的当前经验意外变成可编辑组件");
  const auto disabled = search.mutate(rewritten,
      Json{{"reason", "停用当前注入"},
           {"experience", Json{{"memory_id", "private"}, {"content", nullptr}}}}, selected);
  const auto disabled_packet = search.packet(disabled, feedback, Json::Array{"action_rules"});
  for (const auto &source : disabled_packet["experiences"].items())
    if (source.str("memory_id") == "private")
      expect(source["content"].null(), "控制反思复活了已停用的L1正文");
  expect(search.seed.dump() == original, "反思投影修改了冻结种子");
}

// 复现真实长请求：正文能放下，但追加操作绑定后超限。必须在选取反馈时一起计费，
// 并给同一请求的绑定纠错留出空间；不能靠发送没有绑定的空动作空间来“满足”字节限制。
void reflection_control_binding_budget_reservation() {
  namespace te = trajectory::evolution;
  Search search;
  search.seed = public_alias_fixture();
  auto policy = proto<te::v1::RuntimePolicy>(search.seed["policy"]);
  auto options = policy.options();
  options.set_requested_level(te::v1::L3);
  options.add_host_capabilities("loop.decide");
  auto *loop = options.add_loop_rules();
  loop->set_rule_id("base-loop");
  loop->set_target_scope("assistant");
  loop->set_max_iterations(12);
  loop->set_max_no_progress(3);
  loop->set_no_progress_decision("stop");
  loop->set_reason("保留原有执行预算");
  search.seed["level"] = "L3";
  search.seed["policy"] = json(te::compile_runtime_policy(policy.snapshot(), options));
  search.controls = Json{{"enabled", true}, {"prompt_file", "unused"}};
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 4},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  Report report;
  for (unsigned index = 0; index < 2; ++index) {
    const auto id = "binding-task-" + std::to_string(index);
    auto *task = search.dataset.add_tasks();
    task->set_task_id(id);
    task->set_split("train");
    task->set_task_input("执行查询并如实报告，其他任务仍须独立完成。");
    task->set_expected_behavior("保留真实结果，不得伪造成功。");
    Json events = Json::Array{};
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      const auto action = "finished-" + std::to_string(attempt);
      events.push(Json{{"kind", "action.start"}, {"action_id", action},
                       {"operation", "query"}, {"input", Json::Object{}},
                       {"attributes", Json{{"type", "schedule.action"},
                                            {"target_scope", "assistant"}}}});
      events.push(Json{{"kind", "action.end"}, {"event_id", action},
                       {"action_id", action}, {"operation", "query"},
                       {"status", "completed"}, {"output", std::string(2000, 'x')},
                       {"attributes", Json{{"semantic", "action"}, {"executed", true}}}});
    }
    const Json query{{"boundary", "actions"}, {"execution_id", "guard"},
                     {"advice_query", Json{{"target_scope", "assistant"}}},
                     {"actions", Json::Array{Json{{"action_id", "guard"},
                                                  {"operation", "query"},
                                                  {"arguments", Json::Object{}}}}}};
    events.push(Json{{"kind", "custom"}, {"event_id", "policy-query"},
                     {"attributes", Json{{"type", "policy.query"},
                       {"decision", Json{{"query", query},
                         {"actions", Json::Array{Json{{"action_id", "guard"},
                                                      {"decision", "allow"}}}}}}}}});
    events.push(Json{{"kind", "action.start"}, {"action_id", "guard"},
                     {"operation", "query"},
                     {"attributes", Json{{"extensions", Json{{"guard_only", true}}}}}});
    events.push(Json{{"kind", "action.end"}, {"event_id", "guard-end"},
                     {"action_id", "guard"}, {"operation", "query"}, {"status", "completed"},
                     {"attributes", Json{{"semantic", "action"}, {"executed", false}}}});
    auto *trial = report.add_trials();
    trial->set_task_id(id);
    trial->set_trial_id(id);
    trial->set_variant_id("candidate");
    trial->set_execution_status("completed");
    trial->set_verdict("failed");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", id}}}, {"events", events}}}});
  }
  const auto source = report.SerializeAsString();
  const Json selected = Json::Array{"action_rules"};
  std::cerr << "binding_budget_stage=packet\n";
  const auto original = search.packet(search.seed, report, selected);
  std::cerr << "binding_budget_stage=original feedback=" << original["feedback"].items().size()
            << " bytes=" << original.dump().size() << '\n';
  const auto full = observed_operation_bindings(original, 65536);
  std::cerr << "binding_budget_stage=full\n";
  expect(full["controls"]["editable_bindings"].array() &&
             !full["controls"]["editable_bindings"].items().empty(),
         "回归样本没有形成真实可编辑操作，不能证明预算修复");
  const auto limit = original.dump().size() + 16;
  expect(full.dump().size() > limit, "回归样本没有覆盖正文填满、绑定被挤出的边界");
  search.config["max_input_bytes"] = limit;
  const auto packet = search.packet(search.seed, report, selected);
  std::cerr << "binding_budget_stage=bounded feedback=" << packet["feedback"].items().size()
            << " bytes=" << packet.dump().size() << '\n';
  auto prepared = observed_operation_bindings(packet, limit);
  expect(prepared.dump().size() <= limit &&
             prepared["controls"]["editable_bindings"].array() &&
             !prepared["controls"]["editable_bindings"].items().empty(),
         "完整组包链仍让轨迹挤掉全部操作绑定");
  const auto original_id = prepared["controls"]["editable_bindings"].items()[0].str("binding_id");
  auto repair = packet;
  repair.fields().erase("parent_version");
  repair["phase"] = "repair_control_binding";
  repair["error"] = "control_binding_invalid";
  repair["binding_error"] = Json{{"reason", "budget_out_of_range"}, {"update_index", 0},
      {"budget", prepared["controls"]["editable_bindings"].items()[0]["budget"]}};
  const auto repaired = observed_operation_bindings(repair, limit);
  std::cerr << "binding_budget_stage=repair\n";
  expect(repaired.dump().size() <= limit &&
             repaired["controls"]["editable_bindings"].array() &&
             !repaired["controls"]["editable_bindings"].items().empty(),
         "首次请求保留了绑定，但一次纠错又把绑定挤掉");
  expect(repaired["controls"]["editable_bindings"].items()[0].str("binding_id") != original_id,
         "纠错请求错误复用了上一请求的绑定身份");
  // 错误字段比数值越界的诊断更长；用解析器实际产生的诊断核验最坏预算，
  // 不让首次可发送的请求在一次纠错时再次超限。
  Json diagnostic;
  try {
    resolve_control_binding_updates(
        Json{{"control_binding_updates", Json::Array{Json{
                 {"binding_id", full["controls"]["editable_bindings"].items()[0]["binding_id"]},
                 {"memory_id", "private"}, {"max_iterations", 1}, {"reason", "错误预算字段"}}}}},
        full, &diagnostic);
  } catch (const Error &) {
  }
  expect(diagnostic.str("reason") == "budget_field_mismatch", "未复现真实绑定字段诊断");
  std::cerr << "binding_budget_stage=diagnostic\n";
  auto field_repair = original;
  field_repair.fields().erase("parent_version");
  field_repair["phase"] = "repair_control_binding";
  field_repair["error"] = "control_binding_invalid";
  field_repair["binding_error"] = diagnostic;
  expect(observed_operation_bindings(field_repair, std::numeric_limits<std::size_t>::max())
                 .dump().size() <= reflection_input_bytes(original),
         "纠错预留只覆盖数值越界，没有覆盖更长的实际诊断");
  std::cerr << "binding_budget_stage=field_repair\n";

  // 分组放大到超过输入预算，验证实际发送给模型的控制证据仍完整，而非只测裁剪函数。
  // 模型桩只返回合法弃权，整个测试不会访问远程接口或生成真实策略。
  struct BoundedControlModel final : legacy::ModelAdapter {
    std::size_t limit = 0, bounded_maps = 0;
    std::string name() const override { return "bounded-control-fixture"; }
    std::string model_id() const override { return "local"; }
    legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
      const auto packet = Json::parse(request.user_prompt);
      expect(request.user_prompt.size() <= limit, "分组反思发送了超限输入");
      if (packet.str("phase") == "combee_map") {
        expect(packet.flag("feedback_truncated", false), "未实际覆盖分组超限裁剪");
        expect(packet["controls"]["editable_bindings"].array() &&
                   !packet["controls"]["editable_bindings"].items().empty(),
               "分组裁剪丢失了真实操作绑定");
        for (const auto &row : packet["feedback"].items())
          expect(row["trial"]["evidence"]["runs"].array() &&
                     !row["trial"]["evidence"]["runs"].items().empty(),
                 "分组裁剪把所有控制轨迹退化成了分数摘要");
        ++bounded_maps;
      }
      legacy::RawModelResponse response;
      response.transport_ok = true;
      response.body = R"({"reason":"保留父策略，验证输入投影不制造改动。","experiences":[]})";
      return response;
    }
  };
  char directory[] = "/tmp/trajectory-gepa-packet-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "无法创建分组回归临时目录");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
  } cleanup{directory};
  auto grouped = original;
  grouped["feedback"].push(original["feedback"].items()[0]);
  grouped["feedback"].push(original["feedback"].items()[1]);
  search.root = directory;
  search.tree = Json{{"started_ms", now()}, {"nodes", Json::Array{Json::Object{}}}};
  search.config["reflection_strategy"] = "combee";
  search.config["duplication_factor"] = 4;
  search.config["max_model_calls"] = 3;
  search.config["max_input_bytes"] = reflection_input_bytes(grouped);
  search.config["prompt_file"] = std::string(directory) + "/prompt.txt";
  search.config["aggregation_prompt_file"] = search.config["prompt_file"];
  write(search.config.str("prompt_file"), "只依据给定控制证据提出改动，不能扩大授权。");
  auto adapter = std::make_shared<BoundedControlModel>();
  adapter->limit = search.config.num("max_input_bytes", 0);
  search.model = adapter;
  Json pending;
  std::cerr << "binding_budget_stage=before_propose\n";
  const auto grouped_draft = search.propose(search.seed, grouped, 0, pending);
  std::cerr << "binding_budget_stage=proposed\n";
  expect(adapter->bounded_maps == 2 && search.tree.num("model_calls", 0) == 3,
         "分组裁剪没有覆盖两次 map 和一次 reduce，或错误增减调用预算");
  expect(search.mutate(search.seed, grouped_draft, selected).dump() == search.seed.dump(),
         "合法分组弃权错误生成了新策略");
  expect(report.SerializeAsString() == source, "预算调整修改了原始执行和评分证据");
}

// Full-pool reflection must visit all 32 distinct train tasks and resume completed
// map/reduce stages after a failed final model call, without replaying their API calls.
void full_pool_reflection_covers_32_tasks_and_resumes() {
  Search search;
  search.seed = public_alias_fixture();
  auto mixed_policy = proto<trajectory::evolution::v1::RuntimePolicy>(search.seed["policy"]);
  auto mixed_options = mixed_policy.options();
  mixed_options.set_requested_level(trajectory::evolution::v1::L3);
  mixed_options.add_host_capabilities("loop.decide");
  auto *base_loop = mixed_options.add_loop_rules();
  base_loop->set_rule_id("base-loop");
  base_loop->set_target_scope("assistant");
  base_loop->set_max_iterations(12);
  base_loop->set_max_no_progress(3);
  base_loop->set_no_progress_decision("stop");
  base_loop->set_reason("Preserve the existing execution budget");
  search.seed["level"] = "L3";
  search.seed["policy"] = json(trajectory::evolution::compile_runtime_policy(
      mixed_policy.snapshot(), mixed_options));
  search.controls = Json{{"enabled", true}, {"prompt_file", "unused-in-unit-test"}};
  validate_control_learning(search.controls);
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 8},
                       {"max_model_calls", 64}, {"reflection_strategy", "combee"},
                       {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  std::string selected_key;
  const auto available = search.components(search.seed);
  for (const auto &entry : available.fields())
    if (entry.first.rfind("experience:", 0) == 0) {
      selected_key = entry.first;
      break;
    }
  expect(!selected_key.empty(), "full-pool fixture has no editable experience");
  const Json selected = Json::Array{selected_key, "loop_rules"};
  Report report;
  for (unsigned i = 0; i < 32; ++i) {
    const auto id = "pool-" + std::to_string(i);
    auto *task = search.dataset.add_tasks();
    task->set_task_id(id);
    task->set_split("train");
    task->set_task_input("完成第 " + std::to_string(i) + " 项任务");
    task->set_expected_behavior("完整交付");
    auto *trial = report.add_trials();
    trial->set_trial_id(id);
    trial->set_task_id(id);
    trial->set_variant_id("candidate");
    trial->set_execution_status("completed");
    trial->set_verdict(i % 7 == 0 ? "passed" : "failed");
  }
  const auto memory_id = selected_key.substr(11);
  const auto policy =
      proto<trajectory::evolution::v1::RuntimePolicy>(search.seed["policy"]);
  std::string revision;
  for (const auto &source : policy.snapshot().experiences())
    if (source.memory_id() == memory_id)
      revision = source.content_revision();
  expect(!revision.empty(), "full-pool fixture memory has no approved revision");
  for (unsigned i = 0; i < 2; ++i) {
    const Json refs = Json::Array{
        Json{{"memory_id", memory_id}, {"content_revision", revision}}};
    const Json events = Json::Array{
        Json{{"kind", "custom"}, {"step_id", "model"}, {"event_id", "query"},
             {"attributes", Json{{"type", "policy.query"},
                 {"decision", Json{{"query", Json{{"boundary", "llm"}}},
                                     {"experiences", refs}}}}}},
        Json{{"kind", "advice.applied"}, {"step_id", "model"}, {"event_id", "receipt"},
             {"application", Json{{"status", "applied"}}},
             {"attributes", Json{{"receipt", Json{{"boundary", "llm"},
                                                    {"experiences", refs}}}}}}};
    *report.mutable_trials(i)->mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", "pool-run-" +
                                                  std::to_string(i)}}},
                             {"events", events}}}});
  }
  const auto original = report.SerializeAsString();
  const auto initial = search.packet(search.seed, report, selected);
  struct FullPoolModel final : legacy::ModelAdapter {
    std::set<std::string> mapped;
    std::size_t map_calls = 0, reduce_calls = 0, review_calls = 0;
    bool fail_review_once = true;
    bool mixed_provenance_preserved = true;
    std::string memory_id;
    std::string trigger_pattern;
    std::string name() const override { return "full-pool-fixture"; }
    std::string model_id() const override { return "local"; }
    legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
      const auto packet = Json::parse(request.user_prompt);
      legacy::RawModelResponse response;
      response.transport_ok = true;
      if (packet.str("phase") == "pool_diagnostic_map") {
        ++map_calls;
        mixed_provenance_preserved &= packet["selected_components"].items().size() == 2 &&
                                      packet.dump().size() <= 65536;
        for (const auto &row : packet["feedback"].items()) {
          mapped.insert(row.str("task_id"));
          mixed_provenance_preserved &= row["trial"]["observed_tool_failures"].object();
        }
        response.body = R"({"reason":"现有证据不足，保留父策略。","experiences":[]})";
      } else if (packet.str("phase") == "pool_counterexample_review") {
        ++review_calls;
        if (fail_review_once) {
          fail_review_once = false;
          response.transport_ok = false;
          response.error_code = "temporary";
        } else
          response.body =
              Json{{"reason", "跨题复核保留对全部交付目标的经验修订。"},
                   {"selected_proposals", Json::Object{}},
                   {"experience_revisions", Json::Array{
                       Json{{"memory_id", memory_id},
                            {"content", "Verify every requested deliverable and state unmet goals."},
                            {"trigger_pattern", trigger_pattern}}}}}
                  .dump();
      } else {
        ++reduce_calls;
        response.body =
            Json{{"reason", "跨32题目标缺失作综合经验修订。"},
                 {"selected_proposals", Json::Object{}},
                 {"experience_revisions", Json::Array{
                     Json{{"memory_id", memory_id},
                          {"content", "Verify every requested deliverable and state unmet goals."},
                          {"trigger_pattern", trigger_pattern}}}}}
                .dump();
      }
      return response;
    }
  };
  char directory[] = "/tmp/trajectory-gepa-full-pool-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create full-pool fixture directory");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
  } cleanup{directory};
  search.root = directory;
  search.tree = Json{{"started_ms", now()}, {"nodes", Json::Array{Json::Object{}}}};
  search.config["prompt_file"] = std::string(directory) + "/prompt.txt";
  search.config["aggregation_prompt_file"] = search.config["prompt_file"];
  write(search.config.str("prompt_file"), "仅依据本次训练证据输出严格 JSON。");
  auto model = std::make_shared<FullPoolModel>();
  model->memory_id = memory_id;
  model->trigger_pattern = available[selected_key].str("trigger_pattern");
  if (model->trigger_pattern.empty())
    model->trigger_pattern = "when delivery verification is needed";
  search.model = model;
  Json pending;
  bool interrupted = false;
  try {
    (void)search.propose_full_pool(search.seed, initial, report, 0, pending);
  } catch (const Error &error) {
    interrupted = std::string(error.what()) == "gepa_reflection_transport_failed";
  }
  expect(interrupted && model->mapped.size() == 32 && model->mixed_provenance_preserved &&
             search.tree["nodes"].items()[0].num("reflection_pool_covered_task_count", 0) == 32,
         "mixed full-pool maps lost provenance, exceeded 64 KiB, or missed a task");
  const auto map_calls = model->map_calls, reduce_calls = model->reduce_calls;
  const auto cached = fs::path(directory) / "reflections/0.pool-map-0.request.json";
  auto stale = Json::parse(read(cached));
  stale["input"]["feedback"].items()[0]["trial"]["observed_tool_failures"]["count"] = 99;
  write(cached, stale.dump());
  const auto draft = search.propose_full_pool(search.seed, initial, report, 0, pending);
  expect(model->map_calls == map_calls + 1 && model->reduce_calls == reduce_calls &&
             model->review_calls == 2,
         "resume failed to refresh a changed feedback stage or repeated unaffected reduce");
  expect(search.mutate(search.seed, draft, selected).dump() != search.seed.dump() &&
             draft["experiences"].items().size() == 1 &&
             report.SerializeAsString() == original,
         "full-pool synthesis lost its applied L1 revision or changed source report");
}

void full_pool_review_preserves_32_cases_with_mixed_controls() {
  const std::string memory = "experience:shared-review-memory";
  Json review{{"phase", "pool_counterexample_review"},
              {"selected_components", Json::Array{memory, "action_rules"}},
              {"components", Json{{memory, Json{{"content", std::string(700, 'm')}}},
                                   {"action_rules", Json::Array{}}}},
              {"controls", Json{{"action_requirements", Json::Array{
                  Json{{"operation", "run_skills"}, {"min_attempts", 2}}}},
                  {"loop_requirements", Json::Array{
                      Json{{"operation", "run_skills"}, {"min_attempts", 2}}}}}},
              {"experiences", Json::Array{}},
              {"training_batch_summary", Json::Array{}},
              {"intermediate_proposals", Json::Array{}},
              {"case_outcomes", Json::Array{}},
              {"feedback", Json::Array{}},
              {"proposed_strategy", Json{{"reason", "综合32题后保留必要步骤"}}},
              {"prior_search_rejections", Json::Array{
                  Json{{"reason", std::string(1000, 'r')}}}}};
  for (unsigned i = 0; i < 24; ++i)
    review["experiences"].push(Json{{"memory_id", i ? "other-" + std::to_string(i)
                                                    : "shared-review-memory"},
                                     {"content", std::string(460, 'e')}});
  for (unsigned i = 0; i < 10; ++i) {
    Json facts = Json::Array{};
    for (unsigned j = 0; j < 6; ++j)
      facts.push(Json{{"task_id", "task-" + std::to_string((i * 6 + j) % 32)},
                      {"reason", std::string(350, 'f')}});
    review["intermediate_proposals"].push(
        Json{{"id", "map-" + std::to_string(i)},
             {"reason", std::string(500, 'd')}, {"task_facts", facts},
             {"candidate_values", i == 5
                  ? Json{{memory, Json{{"content", "reviewed"}}}}
                  : Json::Object{}}});
  }
  for (unsigned i = 0; i < 32; ++i) {
    Json applications{{"status", "observed"}, {"count", 3},
                      {"points", Json::Array{}}};
    for (unsigned j = 0; j < 3; ++j)
      applications["points"].push(
          Json{{"memory_id", "shared-review-memory"},
               {"task_excerpt", std::string(110, 'q')},
               {"state_excerpt", std::string(150, 's')}});
    review["case_outcomes"].push(
        Json{{"task_id", "task-" + std::to_string(i)},
             {"verdict", i % 4 ? "failed" : "passed"},
             {"execution_status", "completed"},
             {"metrics", Json{{"tokens", 1000}, {"llm_calls", 5}}},
             {"goal_completion", Json{{"issues", Json::Array{
                 Json{{"id", "goal"}, {"status", "failed"},
                      {"reason", std::string(320, 'g')}}}}}},
             {"selected_experience_applications", applications}});
    review["training_batch_summary"].push(
        Json{{"task_id", "task-" + std::to_string(i)}, {"verdict", "failed"}});
  }
  const Json failed{{"task_id", "task-1"}, {"trial", Json{{"verdict", "failed"},
                    {"evidence", Json{{"runs", Json::Array{}}}}}},
                    {"task_input", std::string(5600, 'x')}};
  const Json passed{{"task_id", "task-0"}, {"trial", Json{{"verdict", "passed"},
                    {"evidence", Json{{"runs", Json::Array{}}}}}},
                    {"task_input", std::string(5600, 'y')}};
  review["feedback"] = Json::Array{failed, passed};
  const auto original_requirements = review["controls"].dump();
  const auto input_cases = review["case_outcomes"].items().size();
  expect(reflection_input_bytes(review) > 65536,
         "mixed review fixture did not reproduce an over-budget packet");
  const auto compact = compact_full_pool_review(
      review, Json{{"selected_proposals", Json{{memory, "map-5"}}}}, 65536);
  expect(reflection_input_bytes(compact) <= 65536 &&
             compact["case_outcomes"].items().size() == input_cases &&
             compact["case_outcomes"].items().size() == 32 &&
             compact["feedback"].dump() == review["feedback"].dump() &&
             compact["controls"].dump() == original_requirements &&
             compact["intermediate_proposals"].items().size() == 1 &&
             compact["intermediate_proposals"].items().front().str("id") == "map-5" &&
             compact["intermediate_proposals"].items().front()["task_facts"].null() &&
             compact["case_outcomes"].items().front()["goal_completion"].object() &&
             compact["case_outcomes"].items().front()
                 ["selected_experience_applications"].num("count", 0) == 3,
         "mixed full-pool review lost task goals, typed bounds or real contrast");
}

void full_pool_review_replays_saved_stages(const fs::path &reflection_dir,
                                           std::size_t node) {
  const auto prefix = std::to_string(node);
  const auto first = Json::parse(read(reflection_dir / (prefix + ".pool-map-0.request.json")))["input"];
  const auto reduce = Json::parse(read(reflection_dir / (prefix + ".pool-reduce-0.request.json")))["input"];
  const auto selection = Json::parse(read(reflection_dir / (prefix + ".pool-reduce-0.draft.json")));
  auto review = first;
  review["phase"] = "pool_counterexample_review";
  review["intermediate_proposals"] = reduce["intermediate_proposals"];
  review["proposed_strategy"] = selection;
  review["proposed_selection"] = selection["selected_proposals"];
  review["case_outcomes"] = Json::Array{};
  review["feedback"] = Json::Array{};
  Json failed_case, successful_case;
  std::set<std::string> ids;
  std::map<std::string, Json> observed_failures;
  for (std::size_t group_index = 0;; ++group_index) {
    const auto path = reflection_dir /
                      (prefix + ".pool-map-" + std::to_string(group_index) + ".request.json");
    if (!fs::exists(path))
      break;
    const auto group = Json::parse(read(path))["input"];
    for (const auto &row : group["feedback"].items()) {
      const auto &trial = row["trial"];
      const auto failures = observed_tool_failures(trial["evidence"]["runs"]);
      observed_failures[row.str("task_id")] = failures;
      Json outcome{{"task_id", row["task_id"]}, {"verdict", trial["verdict"]},
                   {"execution_status", trial["execution_status"]},
                   {"metrics", trial["metrics"]},
                   {"observed_tool_failures", failures}};
      if (trial["goal_completion"].object()) {
        outcome["goal_completion"] = trial["goal_completion"];
        if (outcome["goal_completion"]["issues"].array()) {
          auto &issues = outcome["goal_completion"]["issues"].items();
          if (issues.size() > 2)
            issues.resize(2);
          for (auto &issue : issues)
            issue["reason"] = diagnostic_excerpt(issue.str("reason"), 256);
        }
      }
      if (row["selected_experience_applications"].object())
        outcome["selected_experience_applications"] =
            row["selected_experience_applications"];
      if (ids.insert(row.str("task_id")).second)
        review["case_outcomes"].push(std::move(outcome));
      auto example = row;
      example["trial"]["observed_tool_failures"] = failures;
      if (trial.str("verdict") == "passed" && successful_case.null())
        successful_case = example;
      if (trial.str("verdict") != "passed" && failed_case.null())
        failed_case = example;
    }
  }
  auto enriched_reduce = reduce;
  for (auto &proposal : enriched_reduce["intermediate_proposals"].items())
    if (proposal["task_facts"].array())
      for (auto &fact : proposal["task_facts"].items()) {
        const auto source = observed_failures.find(fact.str("task_id"));
        if (source == observed_failures.end())
          continue;
        const auto &failures = source->second;
        if (failures.str("status") == "scanned")
          fact["observed_tool_failure_count"] = failures["count"];
        else
          fact["observed_tool_failure_status"] = "unavailable";
      }
  expect(reflection_input_bytes(enriched_reduce) <= 65536,
         "saved Node2 reduce exceeds the model budget after tool provenance is added");
  for (auto example : {failed_case, successful_case})
    if (example.object()) {
      if (example["trial"]["evidence"]["runs"].array())
        example["trial"] = compact_control_reflection_detail(
            example["trial"], review["selected_components"], 2);
      review["feedback"].push(std::move(example));
    }
  review["counterexample_pair"] = Json{{"failure_present", failed_case.object()},
                                          {"success_present", successful_case.object()}};
  const auto before = reflection_input_bytes(review);
  const auto compact = compact_full_pool_review(review, selection, 65536);
  const auto after = reflection_input_bytes(compact);
  expect(ids.size() == 32 && compact["case_outcomes"].items().size() == 32 &&
             compact["feedback"].items().size() == 2 &&
             compact["controls"].dump() == review["controls"].dump() &&
             after <= 65536,
         "saved mixed L1/control review lost full-pool facts or typed bounds");
  std::cout << "saved full-pool review: tasks=" << ids.size()
            << " before_bytes=" << before << " after_bytes=" << after << '\n';
}

// 每次反思只展示获选能力的说明；旧自定义纯文本模板完全兼容，非法标记明确报错。
void reflection_prompt_component_contract() {
  const std::string source =
      "公共约束\n<!-- gepa:experience -->\nL1改写示例\n"
      "<!-- gepa:controls -->\n只读来源与增量规则\n"
      "<!-- gepa:schedule_rules -->\nL2依赖编排\n"
      "<!-- gepa:action_rules,loop_rules -->\n绑定预算\n"
      "<!-- gepa:loop_rules -->\nL3分支收尾\n"
      "<!-- gepa:common -->\n统一安全约束\n";
  const auto schedule = reflection_prompt_for_components(source, Json::Array{"schedule_rules"});
  expect(schedule == "公共约束\n只读来源与增量规则\nL2依赖编排\n统一安全约束\n",
         "L2请求仍包含L1改写或L3示例");
  expect(reflection_prompt_for_components(source, Json::Array{"experience:a"}) ==
             "公共约束\nL1改写示例\n统一安全约束\n",
         "经验改写丢失本身协议或混入控制协议");
  const auto combined = reflection_prompt_for_components(
      source, Json::Array{"experience:a", "loop_rules", "action_rules"});
  expect(combined.find("L1改写示例") != std::string::npos &&
             combined.find("L3分支收尾") != std::string::npos &&
             combined.find("L2依赖编排") == std::string::npos,
         "联合进化没有保留全部获选能力");
  // 同时检查交付的真实模板，防止示例测试通过而分段标记漏掉编排绑定协议。
  const auto prompt_file = fs::path(__FILE__).parent_path().parent_path() /
                           "config/prompts/gepa-reflection.txt";
  const auto actual_schedule =
      reflection_prompt_for_components(read(prompt_file, 65536), Json::Array{"schedule_rules"});
  for (const auto *field : {"controls.editable_bindings", "order_operations", "binding_id",
                             "control_binding_updates"})
    expect(actual_schedule.find(field) != std::string::npos,
           "真实编排反思模板裁剪后丢失绑定格式说明");
  const std::string plain = "未分节的用户模板\r\n  不修改格式\n";
  expect(reflection_prompt_for_components(plain, Json::Array{"schedule_rules"}) == plain,
         "旧模板被重写");
  expect(reflection_prompt_for_components(
             "<!-- gepa:loop_rules -->\r\nloop\r\n",
             Json::Array{"loop_rules"}) == "loop\r\n",
         "Windows换行的外置模板不能使用");
  for (const auto &invalid : {"<!-- gepa:unknown -->", "<!-- gepa:common, -->",
                              "<!-- gepa: -->", "<!-- gepa:common"}) {
    bool rejected = false;
    try {
      reflection_prompt_for_components(std::string(invalid) + "\n内容\n", Json::Array{});
    } catch (const Error &error) {
      rejected = std::string(error.what()) == "gepa_prompt_section_invalid";
    }
    expect(rejected, "错误的分节标记被静默忽略");
  }
}

// 恢复的小批量执行不能被当成串行吞吐；保留总预算、留出余量及真正执行计数。
void parallel_evaluation_time_reservation() {
  Search search;
  constexpr std::size_t budget = 18000000;
  search.config = Json{{"timeout_ms", budget}};
  search.evaluation = Json{{"parallelism", 16}};
  search.reserve = 800;
  search.tree = Json{{"started_ms", now()}, {"trials_used", 33},
                     {"evaluation_wall_ms", 999999999}, {"timed_trials", 1}};
  const auto deadline = search.deadline();
  expect(search.holdout_time() == budget / 5,
         "旧计时没有并发占用信息时不能继续沿用失真的逐题估计");
  search.record_evaluation_time(195462, 1);
  expect(search.tree.num("timed_trials", 0) == 1 && search.tree.num("timed_waves", 0) == 1 &&
             search.tree.num("evaluation_wall_ms", 0) == 195462 &&
             search.tree.num("trials_used", 0) == 33 && search.deadline() == deadline,
         "重建时间估计器改变了真实已用预算，或沿用了旧错误耗时");
  expect(search.estimated_evaluation_ms(0) == 0 &&
             search.estimated_evaluation_ms(1) == 195462 &&
             search.estimated_evaluation_ms(16) == 195462 &&
             search.estimated_evaluation_ms(17) == 390924 &&
             search.estimated_evaluation_ms(32) == 390924,
         "未满并发批次按串行题数外推，或尾波被漏算");
  expect(search.holdout_time() == 195462 * 50 * 3 / 2,
         "留出预算没有按并发波次保留原有安全系数");
  search.search_check(32);
  const auto before = search.tree.dump();
  search.record_evaluation_time(900000, 0);
  expect(search.tree.dump() == before, "纯补评分或缓存命中污染了 Agent 吞吐估计");
  search.record_evaluation_time(500000, 32);
  expect(search.tree.num("timed_trials", 0) == 33 && search.tree.num("timed_waves", 0) == 3 &&
             search.estimated_evaluation_ms(32) == (195462 + 500000) * 2 / 3,
         "多批次的观测墙钟或波次没有一致累计");

  Search full;
  full.config = search.config;
  full.evaluation = search.evaluation;
  full.tree = Json{{"started_ms", now()}};
  full.record_evaluation_time(32000, 32);
  expect(full.estimated_evaluation_ms(32) == 32000 && full.estimated_evaluation_ms(16) == 16000,
         "满并发观测被重复除以并行数，低估后续执行时间");
  full.record_evaluation_time(32000, 17);
  expect(full.tree.num("timed_waves", 0) == 4 && full.estimated_evaluation_ms(17) == 32000,
         "最后不足一批的题目没有保留完整一波");

  Search serial;
  serial.config = search.config;
  serial.evaluation = Json{{"parallelism", 1}};
  serial.tree = Json{{"started_ms", now()}};
  serial.reserve = 800;
  serial.record_evaluation_time(195462, 1);
  expect(serial.estimated_evaluation_ms(32) == 195462 * 32 &&
             serial.holdout_time() == budget - 1,
         "真正串行执行不能被赋予不存在的并行收益");
  bool reserved = false;
  try {
    serial.search_check(32);
  } catch (const Error &error) {
    reserved = std::string(error.what()) == "gepa_search_time_reserved";
  }
  expect(reserved, "真实时间不足时失去了预算保护");
  search.config["holdout_reserve_ms"] = 120000;
  expect(search.holdout_time() == 120000, "显式留出时间配置被自动估计覆盖");
}

// 工具补查必须沿原事件身份定位；同轮可见事实可补，其他轮、模型和输入不得混入。
void unknown_goal_tool_previews() {
  const auto event = [](const std::string &id, const std::string &semantic = "tool") {
    return Json{{"kind", "action.end"}, {"event_id", id},
                {"attributes", Json{{"semantic", semantic}}},
                {"input", Json{{"secret_input", std::string(2000, 'i')}}},
                {"output", Json{{"text", std::string(2000, 'x')}}}};
  };
  Json raw{{"traces", Json::Array{
      Json{{"events", Json::Array{event("other-turn")}}},
      Json{{"events", Json::Array{
          Json{{"kind", "action.start"}, {"event_id", "start"}},
          event("model", "model"), event("result")}}}}}};
  const auto original = raw.dump();
  auto view = raw;
  bound_judge_payload(view, "", 256);
  // 模型视图删除前两个事件后，工具预览仍指向原始下标 2。
  auto &events = view["traces"].items()[1]["events"].items();
  events.erase(events.begin(), events.begin() + 2);
  const Json scope{{"turn_begin", 3}, {"turn_end", 4},
                   {"goals", Json::Array{Json{{"turn", 2}}, Json{{"turn", 3}}, Json{{"turn", 4}}}}};
  const Json rows = Json::Array{Json{{"status", "uncertain"}}, Json{{"status", "passed"}},
                                Json{{"status", "uncertain"}}};
  const auto path = std::string("/traces/1/events/2/output/text");
  auto refs = result_previews_for_unknown_goals(view, raw, scope, rows);
  expect(refs.items().size() == 1 && refs.items()[0].str("path") == path,
         "工具补查跨轮、读取输入或按压缩下标定位");
  expect(raw.dump() == original, "补查改写了原始证据");
  view["context_evidence"] = Json::Array{Json{{"source", "current"}, {"path", path}}};
  expect(result_previews_for_unknown_goals(view, raw, scope, rows).items().empty(),
         "同一工具结果被重复补查");
  view.fields().erase("context_evidence");

  auto invalid = view;
  invalid["traces"].items()[1]["events"].items()[0]["output"]["text"]["path"] =
      "/traces/0/events/0/output/text";
  expect(result_previews_for_unknown_goals(invalid, raw, scope, rows).items().empty(),
         "伪造裁剪标记越过了目标轮次边界");
  invalid = view;
  invalid["traces"].items()[1]["events"].items()[0]["output"]["text"]["preview_only"] = "true";
  expect(result_previews_for_unknown_goals(invalid, raw, scope, rows).items().empty(),
         "业务字符串被误认成补查标记");
  auto ambiguous = raw;
  ambiguous["traces"].items()[1]["events"].push(event("result"));
  expect(result_previews_for_unknown_goals(view, ambiguous, scope, rows).items().empty(),
         "重复 event_id 不能决定原始结果身份");

  const Json one_scope{{"turn_begin", 1}, {"turn_end", 1},
                       {"goals", Json::Array{Json{{"turn", 1}}, Json{{"turn", 1}}}}};
  const Json unknown = Json::Array{Json{{"status", "uncertain"}}, Json{{"status", "uncertain"}}};
  Json many{{"trace", Json{{"events", Json::Array{event("model", "model")}}}},
            {"output", Json::Object{}}};
  for (int i = 0; i < 6; ++i) {
    many["trace"]["events"].push(event("tool-" + std::to_string(i)));
    many["output"]["answer-" + std::to_string(i)] = std::string(2000, 'a');
  }
  auto bounded = many;
  bound_judge_payload(bounded, "", 256);
  // 一次最多四项，先补答案。已读取的四项不应阻止下次补剩余答案和工具结果。
  auto first = result_previews_for_unknown_goals(bounded, many, one_scope, unknown);
  expect(first.items().size() == 4, "重复目标突破四字段上限");
  bounded["context_evidence"] = first;
  const auto second = result_previews_for_unknown_goals(bounded, many, one_scope, unknown);
  expect(second.items().size() == 4 &&
             second.items()[0].str("path") == "/output/answer-4" &&
             second.items()[2].str("path") == "/trace/events/1/output/text",
         "已读取字段仍占补查名额，或模型输出混入工具补查");
}

// 元数据投影与原有全文转换逐字段等价；保留类型、空值和缺失差异，原始轨迹不能被修改。
void evidence_field_projection() {
  Json source{{"loaded_version", "版本一"},
              {"loop_review", Json{{"status", "complete"}, {"count", 2}}},
              {"assessment", Json{{"coverage", 0.5}, {"quality", 0}}},
              {"artifacts", Json{{"输出", Json{{"stored_file", "result.txt"}, {"size_bytes", 0}}}}},
              {"explicit_null", Json()}, {"flag", false}, {"empty", Json::Object{}},
              {"list", Json::Array{0, false, Json(), "中文"}},
              {"trace", Json{{"events", Json::Array{Json{{"output", std::string(1048576, 'x')}}}}}}};
  auto stored = proto<google::protobuf::Struct>(source);
  const auto original = stored.SerializeAsString();
  const auto full = json(stored);
  const auto selected = evidence_fields(
      stored, {"loaded_version", "loop_review", "assessment", "artifacts",
               "explicit_null", "flag", "empty", "list", "missing", "loaded_version"});
  for (const auto *name : {"loaded_version", "loop_review", "assessment", "artifacts",
                           "explicit_null", "flag", "empty", "list"})
    expect(selected[name].dump() == full[name].dump(), "元数据投影改变字段值或类型");
  expect(selected.fields().size() == 8 && !selected.has("missing") && !selected.has("trace"),
         "未请求或缺失字段被意外复制");
  expect(selected.has("explicit_null") && selected["explicit_null"].null(),
         "显式空值被误当成字段缺失");
  expect(evidence_fields(stored, {}).object() && evidence_fields(stored, {}).fields().empty(),
         "空字段集合应返回空对象");
  expect(stored.SerializeAsString() == original, "读取元数据修改了原始完整证据");

  // 错误类型也原样交给既有校验器，不能在性能优化中自动纠正坏数据。
  (*stored.mutable_fields())["loop_review"].set_string_value("invalid");
  expect(evidence_fields(stored, {"loop_review"})["loop_review"].string(),
         "投影把非法评分类型转换成了有效对象");
}

// 回归用例：main。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
void recovery_primitive_evidence_contract() {
  // 同样的语义浪费描述不能凭空产生恢复条件；非恢复原语必须保持可用。
  Json action{{"kind", "loop.decision"}, {"actor_id", "worker"},
              {"loop_control", Json{{"target_scope", "scope"}, {"pending_actions", false},
                                    {"observed_no_progress", 1}}}};
  Json input{{"selected_components", Json::Array{"loop_rules"}},
             {"controls", Json{{"loop_primitives", Json::Array{
                 Json{{"primitive_id", "bounded_no_progress"}},
                 Json{{"primitive_id", "bounded_recovery_budget"}}}}}},
             {"feedback", Json::Array{Json{{"trial", Json{{"evidence", Json{{"runs", Json::Array{
                 Json{{"actions", Json::Array{action}}}}}}}}}}}}};
  auto prepared = observed_operation_bindings(input, 65536);
  expect(prepared["controls"]["loop_primitives"].items().size() == 1,
         "无条件依据时仍向模型开放了削减恢复预算");
  expect(prepared["controls"]["loop_primitives"].items()[0].str("primitive_id") ==
             "bounded_no_progress", "恢复条件缺失不应关闭有真实计数依据的其他原语");
  auto &actions = input["feedback"].items()[0]["trial"]["evidence"]["runs"].items()[0]["actions"];
  actions.items()[0]["loop_control"]["error_codes"] = Json::Array{"FixtureUnavailable"};
  actions.push(actions.items()[0]);
  prepared = observed_operation_bindings(input, 65536);
  const auto &bindings = prepared["controls"]["loop_primitives"].items()[1]["observed_bindings"];
  expect(bindings.items().size() == 1 && bindings.items()[0].str("actor_id") == "worker" &&
             bindings.items()[0]["error_codes"].items()[0].text() == "FixtureUnavailable",
         "恢复条件未与实际执行者绑定或重复观测挤占输入");
  expect(prepared.dump() == observed_operation_bindings(prepared, 65536).dump(),
         "重复组包改变了恢复条件或证据位置");
  for (auto &item : actions.items()) {
    item["loop_control"].fields().erase("error_codes");
    item["loop_control"]["rejected_actions"] = 1;
  }
  prepared = observed_operation_bindings(input, 65536);
  expect(prepared["controls"]["loop_primitives"].items()[1]["observed_bindings"].items()[0]
             .flag("after_action_rejection", false), "真实动作拒绝未作为可用恢复条件提供");
  for (auto &item : actions.items())
    item["loop_control"]["pending_actions"] = true;
  expect(recovery_primitive_bindings(input).items().empty(), "在途工作被用作停止恢复的条件");
}

// 100 轮纯状态回归：未执行候选不消耗 patience，真实筛选/验证才消耗；
// 中途序列化恢复后必须延续同一计数口径，不调用 Agent 或模型。
void patience_assessment_state_machine_100_rounds() {
  const auto node = [](const std::string &status, const bool screened, const bool validated) {
    Json result{{"status", status}};
    if (screened)
      result["screening"] =
          Json{{"candidate", Json{{"task", Json{{"fitness", 0.5}, {"quality", 0.5}}}}}};
    if (validated)
      result["validation"] =
          Json{{"task", Json{{"fitness", 0.5}, {"quality", 0.5}}}};
    return result;
  };
  Search initial, resumed;
  Search *search = &initial;
  Json state{{"rounds", 0}, {"stagnation", 0}};
  constexpr std::size_t patience = 51;
  for (std::size_t round = 0; round < 100; ++round) {
    bool assessed = false;
    if (round % 4 == 0) {
      const Json batch = Json::Array{node("duplicate", false, false),
                                     node("untriggerable_controls", false, false)};
      assessed = std::any_of(batch.items().begin(), batch.items().end(),
                             [&](const auto &item) { return search->assessed_for_patience(item); });
    } else if (round % 4 == 1) {
      assessed = search->assessed_for_patience(node("unobserved_changes", false, true));
    } else if (round % 4 == 2) {
      const Json batch = Json::Array{node("duplicate", false, false),
                                     node("rejected_minibatch", true, false)};
      assessed = std::any_of(batch.items().begin(), batch.items().end(),
                             [&](const auto &item) { return search->assessed_for_patience(item); });
    } else {
      assessed = search->assessed_for_patience(node("untriggerable_controls", false, false));
    }
    state["rounds"] = state.num("rounds", 0) + 1;
    if (assessed)
      state["stagnation"] = state.num("stagnation", 0) + 1;
    expect(state.num("stagnation", 0) < patience,
           "未评测节点错误消耗 patience，导致 100 轮前停止");
    if (round == 56) {
      state = Json::parse(state.dump());
      search = &resumed;
    }
  }
  expect(state.num("rounds", 0) == 100 && state.num("stagnation", 0) == 50,
         "100 轮混合状态没有保持真实 assessed 计数");
  expect(!search->assessed_for_patience(node("duplicate", false, false)) &&
             !search->assessed_for_patience(node("untriggerable_controls", false, false)),
         "未执行候选被计入 patience");
  expect(search->assessed_for_patience(node("unobserved_changes", false, true)) &&
             search->assessed_for_patience(node("rejected_minibatch", true, false)),
         "真实筛选或验证结果没有计入 patience");
  state["stagnation"] = state.num("stagnation", 0) + 1;
  expect(state.num("stagnation", 0) == patience,
         "第 51 个真实 assessed 结果没有触发配置的 patience");

  // 复放旧版错误计数的完整 tree：100 个逻辑轮次中，一半没有执行候选；
  // migration 必须按 batch/merge 边界重算，并让错误的 patience 终态可恢复。
  Search legacy;
  legacy.config = Json{{"frontier_type", "instance"},
                       {"prune_dominated", false},
                       {"patience", patience}};
  legacy.val = {"task"};
  Json nodes = Json::Array{Json{{"id", 0},
                                {"kind", "seed"},
                                {"status", "accepted"},
                                {"validation",
                                 Json{{"task", Json{{"fitness", 0.5}, {"quality", 0.5}}}}}}};
  std::size_t id = 1;
  const auto append = [&](const std::size_t round, const std::string &kind,
                          const std::string &status, const bool screened,
                          const bool validated) {
    auto value = node(status, screened, validated);
    value["id"] = id++;
    value["kind"] = kind;
    value["parent"] = 0;
    if (kind == "reflection")
      value["batch"] = round;
    nodes.push(std::move(value));
  };
  for (std::size_t round = 0; round < 100; ++round) {
    if (round % 4 == 0) {
      append(round, "reflection", "duplicate", false, false);
      append(round, "reflection", "untriggerable_controls", false, false);
    } else if (round % 4 == 1) {
      append(round, "reflection", "unobserved_changes", false, true);
    } else if (round % 4 == 2) {
      append(round, "reflection", "duplicate", false, false);
      append(round, "reflection", "rejected_minibatch", true, false);
    } else {
      append(round, "merge", "untriggerable_controls", false, false);
    }
  }
  legacy.tree = Json{{"nodes", nodes},
                     {"batch", nullptr},
                     {"rounds", 100},
                     {"stagnation", 100},
                     {"stop_reason", "patience"}};
  expect(legacy.refresh_patience_accounting(), "旧 patience 计数没有执行 revision migration");
  expect(legacy.tree.num("stagnation", 0) == 50,
         "旧 checkpoint 没有恢复为 50 次真实 assessed 结果");
  expect(!legacy.tree.has("stop_reason"),
         "迁移后尚未达到 patience，却仍保留了旧停止原因");
  expect(legacy.tree.num("patience_accounting_revision", 0) == 2,
         "旧 checkpoint 没有写入当前 patience 计数版本");
  Search recovered;
  recovered.config = legacy.config;
  recovered.val = legacy.val;
  recovered.tree = Json::parse(legacy.tree.dump());
  expect(!recovered.refresh_patience_accounting() &&
             recovered.tree.num("stagnation", 0) == 50,
         "已迁移 checkpoint 恢复后重复改写 patience 计数");
}

// 恢复已有 L0 验证后，未提供 seed_report_file 也必须先评测 seed，
// 再标注 Node0；accepted 但验证缺失的旧检查点遵循同一路径。
void seed_validation_precedes_node_optimization() {
  namespace te = trajectory::evolution;
  const auto view = fs::path(__FILE__).parent_path().parent_path() /
                    "experiments/business-coverage/datasets/gepa-view.json";
  const auto dataset = te::read_evaluation_dataset(view);
  Json baseline_values = Json::Object{};
  std::vector<std::string> val_ids;
  for (const auto &task : dataset.tasks())
    if (task.split() == "val") {
      val_ids.push_back(task.task_id());
      baseline_values[task.task_id()] =
          Json{{"fitness", 1}, {"quality", 1}, {"strict_pass_rate", 1},
               {"invalid_loops", 0}, {"loop_method", "model_review"},
               {"duration_ms", 100}, {"repetitions", 1}};
    }
  expect(!val_ids.empty(), "startup regression has no validation tasks");
  struct ResetBoundary {
    ~ResetBoundary() { te::evaluation_test_boundary = {}; }
  } reset;
  for (const auto *initial_status : {"seed", "accepted"}) {
    char path[] = "/tmp/trajectory-gepa-seed-start-XXXXXX";
    expect(::mkdtemp(path) != nullptr, "cannot create seed startup fixture");
    struct Cleanup {
      fs::path path;
      ~Cleanup() { fs::remove_all(path); }
    } cleanup{path};
    Search search;
    search.root = fs::path(path) / "search";
    search.dataset_path = view;
    search.seed = public_alias_fixture();
    search.baseline = search.seed;
    search.context = "seed-startup-fixture";
    search.evaluation_context = "seed-evaluation-fixture";
    search.candidate_evaluation_context = "seed-runtime-fixture";
    search.config = Json{{"max_rounds", 1}, {"max_model_calls", 1},
                         {"max_trials", 1000}, {"timeout_ms", 600000},
                         {"cache_evaluations", false}, {"use_merge", false},
                         {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
    search.evaluation = Json{{"repetitions", 1}, {"parallelism", 1},
                             {"variants", Json::Array{Json{{"id", "candidate"}}}},
                             {"judge", Json{{"assessment_mode", "objectives"}}}};
    search.gate = Json::Object{};
    search.tree = Json{{"schema_version", 2}, {"started_ms", now()},
                       {"rounds", 0}, {"model_calls", 0}, {"trials_used", 0},
                       {"evaluations", Json::Object{}},
                       {"baseline_validation", baseline_values},
                       {"nodes", Json::Array{Json{{"id", 0}, {"parent", nullptr},
                                                    {"parents", Json::Array{}},
                                                    {"kind", "seed"}, {"depth", 0},
                                                    {"version", te::agent_version_id(search.seed.dump())},
                                                    {"status", initial_status},
                                                    {"validation", nullptr},
                                                    {"selections", 0}}}}};
    write(search.root / "tree.json", search.tree.dump());
    write(search.root / "versions/0.json", search.seed.dump());
    unsigned calls = 0;
    te::evaluation_test_boundary = [&](const fs::path &output,
                                       const te::TaskEvaluationOptions &options) {
      ++calls;
      const auto settings = Json::parse(options.config_json);
      expect(settings["splits"].dump() == "[\"val\"]" &&
                 settings["task_ids"].items().size() == val_ids.size() &&
                 settings["variants"].items().size() == 1 &&
                 settings["variants"].items()[0].str("version") ==
                     te::agent_version_id(search.seed.dump()),
             "seed startup evaluated L0 again or changed the validation task set");
      Report report;
      report.set_schema_version(1);
      report.set_report_kind("task_evaluation.v1");
      report.set_dataset_version(dataset.dataset_version());
      report.set_evaluator_version("task-evaluator-v2-goals");
      report.set_run_id("seed-startup-fixture");
      report.set_complete(true);
      *report.mutable_configuration() = proto<google::protobuf::Struct>(settings);
      for (const auto &task_id : val_ids) {
        auto *trial = report.add_trials();
        trial->set_trial_id("seed-" + task_id);
        trial->set_task_id(task_id);
        trial->set_variant_id("candidate");
        trial->set_split("val");
        trial->set_repetition(1);
        trial->set_execution_status("completed");
        trial->set_verdict("passed");
        trial->set_duration_ms(100);
        *trial->mutable_evidence() = proto<google::protobuf::Struct>(
            Json{{"loaded_version", te::agent_version_id(search.seed.dump())},
                 {"loop_review", Json{{"status", "complete"}, {"count", 0}}}});
        *trial->mutable_metrics() =
            proto<google::protobuf::Struct>(Json{{"tokens", 100}, {"llm_calls", 1}});
      }
      const auto bytes = json(report).dump();
      write(output / "report.json", bytes);
      write(output / "manifest.json",
            Json{{"complete", true}, {"run_id", report.run_id()},
                 {"files", Json{{"report.json", hash(bytes)}}}}.dump());
      return report;
    };
    search.cancelled = [&] {
      return search.tree["nodes"].items()[0].str("status") == "accepted" &&
             search.tree["nodes"].items()[0]["optimization"].object();
    };
    bool stopped_after_seed = false;
    try {
      search.run();
    } catch (const Error &error) {
      stopped_after_seed = std::string(error.what()) == "gepa_cancelled";
    }
    const auto persisted = Json::parse(read(search.root / "tree.json"));
    expect(stopped_after_seed && calls == 1 &&
               persisted["baseline_validation"].dump() == baseline_values.dump() &&
               persisted["nodes"].items()[0].str("status") == "accepted" &&
               persisted["nodes"].items()[0]["validation"].fields().size() == val_ids.size() &&
               persisted["nodes"].items()[0]["optimization"].object(),
           "cached L0 did not advance through seed validation before Node0 optimization");
  }
}

void unchanged_experience_repair_is_bounded() {
  Json input{{"parent_version", "parent"},
             {"selected_components", Json::Array{"experience:weather"}},
             {"components", Json{{"experience:weather", "Use only actual weather results."}}},
             {"experiences", Json::Array{Json{{"memory_id", "weather"}, {"condition", "weather rain"}}}},
             {"feedback", Json::Array{Json{{"task_id", "failure"}}, Json{{"task_id", "success"}},
                                       Json{{"task_id", "extra"}, {"text", std::string(5000, 'x')}}}}};
  Json draft{{"experiences", Json::Array{Json{{"memory_id", "weather"},
                    {"content", "Use only actual weather results."}, {"trigger_pattern", "weather rain"}}}}};
  const auto original = input.dump();
  const auto repair = noop_experience_repair_packet(input, draft, 2048);
  expect(repair.object() && repair.dump().size() <= 2048 &&
             repair.str("phase") == "repair_experience_no_behavior_change" &&
             repair["feedback"].items().size() == 2 &&
             repair["feedback"].items()[1].str("task_id") == "success" &&
             repair["attempted_experiences"].items().size() == 1,
         "unchanged proposal repair lost its failure/success evidence or exceeded input budget");
  expect(input.dump() == original, "no-op repair mutated the original evidence");
  expect(noop_experience_repair_packet(input, Json{{"experiences", Json::Array{}}}, 2048).null(),
         "a legitimate abstention was forced into a new proposal");
  input["selected_components"] = Json::Array{"schedule_rules"};
  expect(noop_experience_repair_packet(input, draft, 2048).null(),
         "experience repair expanded control authorization");
}

void reflection_goal_diagnostic_keeps_missing_requirement() {
  trajectory::evolution::dataset::v1::EvaluationTrial trial;
  trial.set_verdict("failed");
  trial.set_execution_status("completed");
  const std::string head = "Already completed: hospital appointments. ";
  const std::string tail = "Missing: the explicitly requested train times were never delivered.";
  std::string middle;
  for (unsigned i = 0; i < 300; ++i) middle += "成功项目";
  const auto reason = head + middle + tail;
  *trial.mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"assessment", Json{{"quality", 0}, {"goals", Json::Array{
          Json{{"id", "goal"}, {"status", "failed"}, {"reason", reason}}}}}}});
  const auto view = feedback_trial(trial, Json::Object{});
  const auto text = view["goal_completion"]["issues"].items().front().str("reason");
  expect(text.size() <= 512 && text.find(head) == 0 &&
             text.find(tail) != std::string::npos &&
             text.find("[middle omitted]") != std::string::npos,
         "reflection discarded the missing requirement behind the satisfied prefix");
  expect(Json::parse(Json(text).dump()).text() == text,
         "reflection diagnostic split a UTF-8 character");
  expect(diagnostic_excerpt(tail, 512) == tail, "short diagnostic was changed");
}

void reflection_tool_failure_provenance() {
  using trajectory::evolution::dataset::v1::EvaluationTrial;
  const Json history = Json::Array{
      Json{{"kind", "action.start"}, {"event_id", "model-start"},
           {"action_id", "model"}, {"operation", "model_call"},
           {"input", Json{{"messages", Json::Array{
               Json{{"content", "Earlier run_skills returned invalid_arguments for skills: []"}}}}}},
           {"attributes", Json{{"semantic", "model"}, {"executed", true}}}},
      Json{{"kind", "action.end"}, {"event_id", "model-end"},
           {"action_id", "model"}, {"status", "completed"},
           {"output", Json{{"choices", Json::Array{
               Json{{"message", Json{{"content", "invalid_arguments was in prior context"}}}}}}}},
           {"attributes", Json{{"semantic", "model"}, {"executed", true}}}}};
  const auto view = [&](const Json &events) {
    EvaluationTrial trial;
    trial.set_execution_status("completed");
    trial.set_verdict("failed");
    *trial.mutable_evidence() = proto<google::protobuf::Struct>(
        Json{{"trace", Json{{"identity", Json{{"run_id", "test-run"}}},
                             {"events", events}}}});
    return std::make_pair(trial, feedback_trial(trial, Json::Object{}));
  };
  const auto historical = view(history);
  expect(historical.second["observed_tool_failures"].str("status") == "scanned" &&
             historical.second["observed_tool_failures"].num("count", 99) == 0 &&
             historical.second["observed_tool_failures"]["examples"].items().empty(),
         "model_call history was counted as a current tool failure");
  expect(historical.second["evidence"]["runs"].items()[0]["actions"].items()[0]
                 .str("semantic") == "model",
         "model output evidence was deleted while filtering failure provenance");

  auto real = history;
  real.push(Json{{"kind", "action.start"}, {"event_id", "tool-start"},
                 {"action_id", "tool"}, {"operation", "run_skills"},
                 {"input", Json{{"skills", Json::Array{}}}},
                 {"attributes", Json{{"semantic", "action"}, {"executed", true}}}});
  real.push(Json{{"kind", "action.end"}, {"event_id", "tool-end"},
                 {"action_id", "tool"}, {"status", "failed"},
                 {"error", Json{{"code", "invalid_arguments"}}},
                 {"attributes", Json{{"semantic", "action"}, {"executed", true},
                                      {"observed_failure", true}}}});
  const auto observed = view(real);
  const auto &failure = observed.second["observed_tool_failures"];
  const Json mixed = Json::Array{"experience:unused", "loop_rules"};
  const auto controlled = control_feedback_trial(observed.first, mixed, Json::Object{});
  const auto historical_control =
      control_feedback_trial(historical.first, mixed, Json::Object{});
  expect(controlled["observed_tool_failures"].dump() == failure.dump() &&
             historical_control["observed_tool_failures"].str("status") == "scanned" &&
             historical_control["observed_tool_failures"].num("count", 99) == 0,
         "mixed control feedback lost actual failure provenance or invented historical failures");
  expect(failure.num("count", 0) == 1 && failure.num("executed_count", 0) == 1 &&
             failure.num("omitted", 99) == 0 &&
             failure["examples"].items().size() == 1 &&
             failure["examples"].items()[0].str("run_id") == "test-run" &&
             failure["examples"].items()[0].str("event_id") == "tool-end" &&
             failure["examples"].items()[0].str("operation") == "run_skills" &&
             failure["examples"].items()[0].str("error_code") == "invalid_arguments" &&
             failure["examples"].items()[0].str("skills_argument") == "empty_array",
         "actual executed run_skills error lost its event/source or empty argument evidence");
  const auto compact = compact_experience_feedback_trial(
      observed.first, observed.second, Json::Array{"experience:unused"}, 1024);
  expect(compact["observed_tool_failures"].num("count", 0) == 1,
         "compact experience feedback lost the structured tool failure");
  const Json facts = reflection_task_facts(
      Json::Array{Json{{"task_id", "history"}, {"trial", historical.second}},
                  Json{{"task_id", "actual"}, {"trial", observed.second}}},
      Json::Array{"experience:unused"});
  expect(facts.items().size() == 2 &&
             facts.items()[0].num("observed_tool_failure_count", 99) == 0 &&
             facts.items()[1].num("observed_tool_failure_count", 0) == 1 &&
             compact["observed_tool_failures"]["examples"].items()[0]
                     .str("event_id") == "tool-end",
         "map/reduce task facts confused historical text with an executed tool error");
  EvaluationTrial no_trace;
  no_trace.set_execution_status("error");
  const auto unobserved = feedback_trial(no_trace, Json::Object{});
  const auto unavailable_facts = reflection_task_facts(
      Json::Array{Json{{"task_id", "no-trace"}, {"trial", unobserved}}},
      Json::Array{"experience:unused"});
  expect(unobserved["observed_tool_failures"].str("status") == "unavailable" &&
             unavailable_facts.items()[0].str("observed_tool_failure_status") == "unavailable" &&
             !unavailable_facts.items()[0].has("observed_tool_failure_count"),
         "missing trace was mislabeled as a scanned task with zero tool errors");
}

void reflection_real_tool_provenance(const fs::path &report_path) {
  const auto report = proto<Report>(
      Json::parse(read(report_path, trajectory::evolution::kMaxEvaluationReportBytes)));
  bool saw_history_only = false, saw_real_empty = false, saw_passed_waste = false;
  for (const auto &trial : report.trials()) {
    if (trial.task_id() != "QA_ZS_WYYL_007" &&
        trial.task_id() != "QA_JK_YDYL_008" && trial.task_id() != "SYS_DYT_03_102")
      continue;
    const auto failures = feedback_trial(trial, Json::Object{})["observed_tool_failures"];
    const auto controlled = control_feedback_trial(
        trial, Json::Array{"experience:unused", "loop_rules"}, Json::Object{});
    expect(controlled["observed_tool_failures"].dump() == failures.dump(),
           "real mixed control feedback dropped structured failure provenance");
    expect(failures.str("status") == "scanned", "real parent trial has no scanned trace");
    if (trial.task_id() == "QA_ZS_WYYL_007") {
      bool historical_invalid_counted = false;
      for (const auto &example : failures["examples"].items())
        historical_invalid_counted |= example.str("operation") == "run_skills" ||
                                     example.str("error_code") == "invalid_arguments";
      expect(failures.num("count", 99) == 2 &&
                 failures.num("executed_count", 0) == 2 &&
                 !historical_invalid_counted &&
                 failures["examples"].items()[0].str("operation") == "sim_web_search" &&
                 failures["examples"].items()[0].str("error_code") == "snapshot_miss",
             "real QA_ZS history became a run_skills error or real search errors were lost");
      saw_history_only = true;
    } else if (trial.task_id() == "QA_JK_YDYL_008") {
      bool bound = false;
      for (const auto &example : failures["examples"].items())
        bound |= example.str("event_id") == "event-126" &&
                 example.str("operation") == "run_skills" &&
                 example.str("error_code") == "invalid_arguments" &&
                 example.str("skills_argument") == "empty_array" &&
                 !example.flag("executed", true) &&
                 !example.str("run_id").empty();
      expect(failures.num("count", 0) >= 1 && bound,
             "real QA_JK rejected empty run_skills action lost run/event binding");
      saw_real_empty = true;
    } else {
      expect(trial.verdict() == "passed" && failures.num("count", 0) >= 2,
             "real passed SYS_DYT trial lost its actual tool waste");
      saw_passed_waste = true;
    }
  }
  expect(saw_history_only && saw_real_empty && saw_passed_waste,
         "real parent report did not contain all three audited trials");
}

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "reflection_goal_diagnostic") {
      reflection_goal_diagnostic_keeps_missing_requirement();
      unchanged_experience_repair_is_bounded();
      std::cout << "reflection goal diagnostic passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "reflection_tool_failure_provenance") {
      reflection_tool_failure_provenance();
      action_feedback_keeps_iteration_assessment();
      compact_control_reflection_preserves_typed_evidence();
      std::cout << "reflection tool failure provenance passed\n";
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "reflection_real_tool_provenance") {
      reflection_real_tool_provenance(argv[2]);
      std::cout << "real reflection tool failure provenance passed\n";
      return 0;
    }
    reflection_tool_failure_provenance();
    reflection_goal_diagnostic_keeps_missing_requirement();
    unchanged_experience_repair_is_bounded();
    if (argc == 2 && std::string(argv[1]) == "seed_startup") {
      seed_validation_precedes_node_optimization();
      std::cout << "GEPA seed startup passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "reflection_control_compaction") {
      compact_control_reflection_preserves_typed_evidence();
      std::cout << "reflection control compaction passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "reflection_eight_tasks") {
      control_packet_retains_eight_distinct_tasks();
      std::cout << "reflection eight tasks passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "reflection_experience_support") {
      experience_packet_counts_distinct_retained_tasks();
      std::cout << "reflection experience support passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "reflection_evidence_quality") {
      compact_control_reflection_preserves_typed_evidence();
      control_packet_retains_eight_distinct_tasks();
      experience_packet_counts_distinct_retained_tasks();
      std::cout << "reflection evidence quality regressions passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "schedule_binding") {
      schedule_sampling_hint_preserves_other_evidence();
      std::cout << "schedule binding regression passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "objective_search_scores") {
      objective_search_scores();
      std::cout << "objective search score regression passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "patience_100_rounds") {
      patience_assessment_state_machine_100_rounds();
      std::cout << "100-round patience regression passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "runtime_context_cache") {
      interrupted_attempt_reuses_prior_checkpoint_without_rebilling();
      cross_job_evaluation_cache();
      fixed_holdout_baseline_cache();
      std::cout << "runtime context cache regressions passed\n";
      return 0;
    }
    // 大型证据回归允许精确执行控制绑定用例，避免无关旧用例失败遮蔽
    // L2/L3 生成、预检与运行时绑定的定向验证结果。
    if (argc == 2 && std::string(argv[1]) == "typed_control_binding_updates") {
      typed_control_binding_updates();
      std::cout << "typed control binding regressions passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "full_pool_reflection") {
      full_pool_reflection_covers_32_tasks_and_resumes();
      full_pool_review_preserves_32_cases_with_mixed_controls();
      std::cout << "full-pool reflection and resume regression passed\n";
      return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "full_pool_review_replay") {
      full_pool_review_replays_saved_stages(argv[2], std::stoull(argv[3]));
      return 0;
    }
    // 可选只读复放：使用现有配置、数据集、父版本和完整报告重建所有组件的实际模型输入。
    // 不运行 Agent、不调用模型、不改报告或记忆，输出仅为字节数和结构检查结果。
    if (argc == 6 || (argc == 7 && std::string(argv[6]) == "controls_only") ||
        (argc == 8 && std::string(argv[6]) == "mixed_maps")) {
      Search search;
      search.config = load_gepa_config(argv[1]);
      const auto service = Json::parse(read(argv[2]));
      search.controls = service["control_learning"];
      auto evaluation_path = fs::path(service.str("policy_evaluation_file"));
      if (evaluation_path.is_relative())
        evaluation_path = fs::path(argv[2]).parent_path() / evaluation_path;
      search.gate = Json::parse(read(evaluation_path))["gate"];
      search.dataset = trajectory::evolution::read_evaluation_dataset(argv[3]);
      search.seed = Json::parse(read(argv[4]));
      const auto report = proto<Report>(
          Json::parse(read(argv[5], trajectory::evolution::kMaxEvaluationReportBytes)));
      Json summary = Json::Array{};
      const auto limit = search.config.num("max_input_bytes", 65536);
      const auto available = search.components(search.seed);
      if (argc == 8) {
        const Json selected = Json::Array{argv[7], "loop_rules"};
        expect(available.has(argv[7]), "real mixed replay memory is unavailable");
        struct RealMixedMapModel final : legacy::ModelAdapter {
          std::set<std::string> covered;
          std::size_t calls = 0, largest = 0, limit = 65536;
          bool reached_reduce = false;
          std::string name() const override { return "real-mixed-replay"; }
          std::string model_id() const override { return "local"; }
          legacy::RawModelResponse evaluate(const legacy::ModelRequest &request) override {
            const auto input = Json::parse(request.user_prompt);
            expect(input.dump().size() <= limit, "real mixed request exceeds input budget");
            largest = std::max(largest, input.dump().size());
            legacy::RawModelResponse response;
            if (input.str("phase") != "pool_diagnostic_map" &&
                input.str("phase") != "pool_trajectory_segment") {
              expect(input.str("phase").rfind("pool_diagnostic_reduce", 0) == 0,
                     "real mixed replay stopped before the reduce boundary");
              reached_reduce = true;
              response.transport_ok = false;
              response.error_code = "replay_map_complete";
              return response;
            }
            ++calls;
            expect(input["selected_components"].items().size() == 2,
                   "real replay did not use the mixed component route");
            for (const auto &row : input["feedback"].items()) {
              covered.insert(row.str("task_id"));
              expect(row["trial"]["observed_tool_failures"].object(),
                     "real mixed model request lost structured failure provenance");
            }
            response.transport_ok = true;
            response.body = R"({"reason":"Read-only replay; retain parent.","experiences":[]})";
            return response;
          }
        };
        char directory[] = "/tmp/trajectory-gepa-real-mixed-XXXXXX";
        expect(::mkdtemp(directory), "cannot create real mixed replay directory");
        struct Cleanup {
          fs::path path;
          ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
        } cleanup{directory};
        search.root = directory;
        search.tree = Json{{"started_ms", now()}, {"nodes", Json::Array{Json::Object{}}}};
        auto model = std::make_shared<RealMixedMapModel>();
        model->limit = limit;
        search.model = model;
        const auto original = report.SerializeAsString();
        const auto packet = search.packet(search.seed, report, selected);
        Json pending;
        try {
          (void)search.propose_full_pool(search.seed, packet, report, 0, pending);
        } catch (const Error &error) {
          expect(model->reached_reduce &&
                     std::string(error.what()) == "gepa_reflection_transport_failed",
                 "real mixed replay failed before all map requests completed");
        }
        expect(model->reached_reduce && model->covered.size() == 32 &&
                   report.SerializeAsString() == original,
               "real mixed replay missed tasks or changed source evidence");
        std::cout << Json{{"covered_tasks", model->covered.size()},
                          {"map_calls", model->calls}, {"largest_request_bytes", model->largest}}
                         .dump() << '\n';
        return 0;
      }
      for (const auto &component : available.fields()) {
        if (argc == 7 && component.first != "action_rules" &&
            component.first != "loop_rules" && component.first != "schedule_rules")
          continue;
        const auto packet = search.packet(search.seed, report, Json::Array{component.first});
        const auto prepared =
            observed_operation_bindings(packet, std::numeric_limits<std::size_t>::max());
        expect(prepared.dump().size() <= limit, "真实复放的最终请求超过配置预算");
        expect(prepared.dump() == observed_operation_bindings(packet, limit).dump(),
               "真实复放仍在发送前丢失操作绑定");
        Json row{{"component", component.first}, {"input_bytes", prepared.dump().size()},
                 {"feedback_trials", packet["feedback"].items().size()},
                 {"editable_bindings", prepared["controls"]["editable_bindings"].array()
                                           ? prepared["controls"]["editable_bindings"].items().size()
                                           : 0}};
        if (prepared["controls"]["editable_bindings"].array()) {
          auto repair = packet;
          repair.fields().erase("parent_version");
          repair["phase"] = "repair_control_binding";
          repair["error"] = "control_binding_invalid";
          repair["binding_error"] = Json{{"reason", "budget_out_of_range"}, {"update_index", 0},
              {"budget", prepared["controls"]["editable_bindings"].items()[0]["budget"]}};
          const auto rebuilt =
              observed_operation_bindings(repair, std::numeric_limits<std::size_t>::max());
          expect(rebuilt.dump().size() <= limit &&
                     rebuilt.dump() == observed_operation_bindings(repair, limit).dump(),
                 "真实复放的纠错请求超限或丢失绑定");
          row["repair_bytes"] = rebuilt.dump().size();
        }
        summary.push(row);
        if (argc == 7)
          std::cout << row.dump() << std::endl;
      }
      if (argc == 6)
        std::cout << summary.dump() << '\n';
      return 0;
    }
    expect(argc == 1, "复放参数应为：gepa配置、service配置、数据集、父版本、评测报告");
    // 保留原断言及执行顺序；异常只补用例名，避免通用协议错误无法定位。
    const auto run = [](const char *name, auto test) {
      try {
        std::cerr << "运行用例：" << name << '\n';
        test();
      } catch (...) {
        std::cerr << "失败用例：" << name << '\n';
        throw;
      }
    };
    // 断点预算回归优先运行，避免其他策略测试失败时掩盖重复执行风险。
    run("interrupted_attempt_reuses_prior_checkpoint_without_rebilling",
        interrupted_attempt_reuses_prior_checkpoint_without_rebilling);
    run("patience_assessment_state_machine_100_rounds",
        patience_assessment_state_machine_100_rounds);
    run("recovery_primitive_evidence_contract", recovery_primitive_evidence_contract);
    run("parallel_evaluation_time_reservation", parallel_evaluation_time_reservation);
    run("unknown_goal_tool_previews", unknown_goal_tool_previews);
    run("schedule_sampling_hint_preserves_other_evidence", schedule_sampling_hint_preserves_other_evidence);
    run("reflection_history_budget_reservation", reflection_history_budget_reservation);
    run("reflection_control_binding_budget_reservation", reflection_control_binding_budget_reservation);
    run("evidence_field_projection", evidence_field_projection);
    run("public_private_alias_revisions", public_private_alias_revisions);
    run("multi_turn_feedback_and_intent_observation", multi_turn_feedback_and_intent_observation);
    run("objective_search_scores", objective_search_scores);
    run("judge_action_request_pairing", judge_action_request_pairing);
    run("selected_experience_receipt_attribution", selected_experience_receipt_attribution);
    run("experience_packet_attribution_isolation", experience_packet_attribution_isolation);
    run("experience_packet_counts_distinct_retained_tasks",
        experience_packet_counts_distinct_retained_tasks);
    run("compact_experience_feedback_budget", compact_experience_feedback_budget);
    run("experience_ablation_checkpoint_deduplication", experience_ablation_checkpoint_deduplication);
    run("observed_training_screening", observed_training_screening);
    run("empty_experience_receipts", empty_experience_receipts);
    run("unknown_loop_counter", unknown_loop_counter);
    run("incremental_control_coverage", incremental_control_coverage);
    run("cancelled_search_clock", cancelled_search_clock);
    run("context_version_is_not_behavior", context_version_is_not_behavior);
    run("asynchronous_target_replay", asynchronous_target_replay);
    run("attributed_candidate_selection", attributed_candidate_selection);
    run("terminal_failure_identity", terminal_failure_identity);
    run("operation_budget_identity", operation_budget_identity);
    run("schedule_resource_projection", schedule_resource_projection);
    run("recovery_component_pairing", recovery_component_pairing);
    run("prompt_resource_workflows", prompt_resource_workflows);
    run("loop_feedback_runtime_budget", loop_feedback_runtime_budget);
    run("missing_trial_control_evidence", missing_trial_control_evidence);
    run("compact_control_reflection_preserves_typed_evidence",
        compact_control_reflection_preserves_typed_evidence);
    run("control_packet_retains_eight_distinct_tasks",
        control_packet_retains_eight_distinct_tasks);
    run("reviewed_iteration_cap_evidence", reviewed_iteration_cap_evidence);
    run("reviewed_finalization_cap_evidence", reviewed_finalization_cap_evidence);
    run("required_iteration_floor_tests", required_iteration_floor_tests);
    run("action_feedback_keeps_iteration_assessment", action_feedback_keeps_iteration_assessment);
    run("incremental_control_drafts", incremental_control_drafts);
    run("pure_control_removal_preflight", pure_control_removal_preflight);
    run("reflection_component_views", reflection_component_views);
    run("reflection_prompt_component_contract", reflection_prompt_component_contract);
    run("full_pool_reflection_covers_32_tasks_and_resumes",
        full_pool_reflection_covers_32_tasks_and_resumes);
    run("reflection_sources_follow_current_parent", reflection_sources_follow_current_parent);
    run("observed_operation_binding_views", observed_operation_binding_views);
    run("reviewed_operation_budget_bindings", reviewed_operation_budget_bindings);
    run("typed_control_binding_updates", typed_control_binding_updates);
    run("repair_feedback_keeps_redispatch_opportunity", repair_feedback_keeps_redispatch_opportunity);
    run("reflection_transport_diagnostics", reflection_transport_diagnostics);
    run("reflection_stage_coverage_accumulates_unique_tasks",
        reflection_stage_coverage_accumulates_unique_tasks);
    run("reflection_task_facts_are_bounded_and_grounded",
        reflection_task_facts_are_bounded_and_grounded);
    run("cross_job_evaluation_cache", cross_job_evaluation_cache);
    run("fixed_holdout_baseline_cache", fixed_holdout_baseline_cache);
    std::cout << "GEPA evidence regressions passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
