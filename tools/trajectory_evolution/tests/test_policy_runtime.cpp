// test_policy_runtime 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

#include "trajectory_evolution/evolution.h"
#include "trajectory_evolution/agent_evaluation.h"
#include "trajectory_evolution/orchestration.h"
#include "agentflow/trajectory/experience.h"
#include "../src/gepa/control_generation.h"
#include "../src/evaluation/evaluation_contracts.h"
#include "../src/evaluation/agent_evaluation_support.h"
#include "../src/runtime/boundary_runtime.h"
#include "../src/gepa/failure_ledger.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <google/protobuf/util/json_util.h>

namespace te = trajectory::evolution;
namespace v1 = te::v1;
namespace ep = agentflow::trajectory::evaluation;

// 测试辅助函数：check。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void check(bool value) {
  if (!value)
    throw std::runtime_error("policy assertion failed");
}

template <class F> void rejects(F fn) {
  try {
    fn();
  } catch (const agentflow::trajectory::EvaluationError &) {
    return;
  }
  throw std::runtime_error("expected policy rejection");
}

// 测试辅助函数：policy。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
v1::RuntimePolicy policy(const char *experience_content = nullptr) {
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1");
  memory.set_revision(1);
  auto *entry = memory.add_entries();
  entry->set_memory_id("experience-a");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *candidate = entry->mutable_candidate();
  candidate->set_schema_version(1);
  candidate->set_candidate_id("candidate-a");
  candidate->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  candidate->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  candidate->set_target_scope("coordinator");
  candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  candidate->set_trigger_pattern("视频生成缺少素材");
  candidate->set_observed_problem("缺少图片仍重试");
  candidate->set_proposed_change(experience_content ? experience_content
                                                   : "请先补充图片素材，独立检索任务继续。");
  candidate->set_support_user_count(1);
  auto *evidence = candidate->add_support_evidence();
  evidence->set_evidence_id("evidence-a");
  evidence->mutable_source()->set_user_id("user-a");
  evidence->mutable_source()->set_dialogue_id("source-a");
  auto *control_entry = memory.add_entries();
  control_entry->set_memory_id("orchestration-z");
  control_entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *control = control_entry->mutable_candidate();
  control->set_schema_version(1);
  control->set_candidate_id("candidate-orchestration-z");
  control->set_generator_version("evolution-candidate-generator-v2");
  control->set_target_type(ep::CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION);
  control->set_target_scope("coordinator");
  control->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  control->set_trigger_pattern("同批图片生成依赖计划完成");
  control->set_observed_problem("图片技能在规划完成前被并行派发");
  control->set_proposed_change("同一批次先完成 planner，再执行 image。");
  control->set_support_user_count(1);
  auto *control_evidence = control->add_support_evidence();
  control_evidence->set_evidence_id("evidence-orchestration-z");
  control_evidence->mutable_source()->set_user_id("user-a");
  control_evidence->mutable_source()->set_dialogue_id("source-orchestration-z");
  v1::Identity identity;
  identity.set_agent_id("agent-a");
  identity.set_user_id("user-a");
  identity.set_conversation_id("conv-a");
  identity.set_run_id("run-a");
  v1::ShadowOptions limits;
  limits.add_target_scopes("coordinator");
  limits.add_target_scopes("skill:worker");
  limits.set_max_policy_entries(64);
  limits.set_max_advice_items(3);
  limits.set_max_advice_bytes(4096);
  limits.set_max_query_bytes(16384);
  limits.set_max_memory_bytes(1048576);
  limits.set_max_snapshot_bytes(262144);
  limits.set_max_match_terms(128);
  limits.set_min_matched_terms(2);
  auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  check(snapshot.experiences_size() == 2);
  v1::RuntimeOptions options;
  options.set_requested_level(v1::L3);
  options.set_fallback("strict");
  for (auto cap : {"context.request", "action.filter", "loop.decide"})
    options.add_host_capabilities(cap);
  options.add_allowed_users("user-a");
  options.add_allowed_conversations("conv-a");
  options.set_experience_template("授权经验：\n{{experience}}");
  options.set_max_context_bytes(4096);
  for (const auto &experience : snapshot.experiences()) {
    auto *approval = options.add_approved_experiences();
    approval->set_memory_id(experience.memory_id());
    approval->set_content_revision(experience.content_revision());
  }
  auto *action = options.add_action_rules();
  action->set_rule_id("image-required");
  action->set_target_scope("coordinator");
  action->set_operation("video");
  action->add_required_arguments("image");
  action->set_reason("缺少图片素材");
  auto *loop = options.add_loop_rules();
  loop->set_rule_id("bounded-loop");
  loop->set_target_scope("coordinator");
  loop->set_max_iterations(6);
  loop->set_max_no_progress(1);
  loop->set_no_progress_decision("replan");
  loop->set_max_replans(1);
  loop->set_replan_context("停止重复操作，选择其他可行方法。");
  loop->set_reason("无进展或达到授权轮数");
  return te::compile_runtime_policy(snapshot, options);
}

// 测试辅助函数：query。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
v1::RuntimeQuery query(const v1::RuntimePolicy &policy, const char *boundary = "llm") {
  v1::RuntimeQuery query;
  auto *advice = query.mutable_advice_query();
  *advice->mutable_identity() = policy.snapshot().identity();
  advice->set_query_id("query-a");
  advice->set_decision_id("decision-a");
  advice->set_target_scope("coordinator");
  advice->set_task("视频生成缺少图片素材");
  query.set_execution_id("coordinator");
  query.set_boundary(boundary);
  return query;
}

// Two outstanding model calls keep the prepared decision with its own query.
// Earlier advice in a later request is not a second L1 application.
void interleaved_injection_receipts() {
  const auto base = policy();
  for (const auto level : {v1::L1, v1::L2, v1::L3}) {
    auto options = base.options();
    options.set_requested_level(level);
    const auto p = te::compile_runtime_policy(base.snapshot(), options);
    te::PolicyRuntime runtime(p);
    auto first = query(p);
    first.mutable_advice_query()->set_query_id("model-first");
    first.mutable_advice_query()->set_decision_id("decision-first");
    const auto prepared = runtime.decide(first);
    check(prepared.status() == "ready" && prepared.experiences_size() == 1 &&
          !prepared.context().empty());

    v1::RuntimeObservation actual;
    actual.set_call_id("actual-model-first");
    actual.set_transport_allowed(true);
    auto *message = (*actual.mutable_request()->mutable_fields())["messages"]
                        .mutable_list_value()
                        ->add_values()
                        ->mutable_struct_value();
    (*message->mutable_fields())["role"].set_string_value("system");
    (*message->mutable_fields())["content"].set_string_value(prepared.context());

    auto second = first;
    second.mutable_advice_query()->set_query_id("model-second");
    second.mutable_advice_query()->set_decision_id("decision-second");
    const auto unchanged = runtime.decide(second);
    check(unchanged.context().empty() && unchanged.experiences().empty());
    check(runtime.verify(first, actual).application().status() == "applied");
    check(runtime.verify(first, actual).application().status() == "applied");
    check(runtime.decide(first).SerializeAsString() == prepared.SerializeAsString());

    // The second request can carry earlier advice in its history without
    // pretending that the native boundary injected it again.
    actual.set_call_id("actual-model-second");
    const auto no_second_application = runtime.verify(second, actual);
    check(no_second_application.application().status() == "rejected" &&
          no_second_application.experiences().empty());
    check(runtime.decide(second).context().empty());

    // Reusing identifiers cannot borrow a prepared decision for a different
    // request, scope or execution. The original remains verifiable afterward.
    auto changed = first;
    changed.mutable_advice_query()->set_state("different observed request state");
    check(runtime.decide(changed).context().empty());
    check(runtime.verify(changed, actual).application().status() != "applied");
    changed = first;
    changed.set_execution_id("another-execution");
    check(runtime.verify(changed, actual).application().status() != "applied");
    changed = first;
    changed.mutable_advice_query()->mutable_identity()->set_user_id("another-user");
    rejects([&] { runtime.verify(changed, actual); });
    actual.set_call_id("actual-model-first");
    check(runtime.verify(first, actual).application().status() == "applied");
    actual.set_transport_allowed(false);
    check(runtime.verify(first, actual).application().status() == "failed");
  }
}

// 测试辅助函数：injection_and_evidence。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void injection_and_evidence() {
  auto p = policy();
  te::PolicyRuntime runtime(p);
  auto q = query(p);
  auto result = runtime.decide(q);
  check(result.status() == "ready" && result.experiences_size() == 1);
  for (int i = 0; i < 32; ++i) {
    if (runtime.decide(q).context() != result.context()) {
      throw std::runtime_error("experience context serialization is not deterministic");
    }
  }
  check(result.context().find("图片素材") != std::string::npos);
  check(result.context().find(p.policy_id()) == std::string::npos);
  check(result.policy_id() == p.policy_id());
  auto another_identity = p.snapshot().identity();
  another_identity.set_run_id("another-run");
  const auto rebound = te::bind_runtime_policy(p, another_identity);
  check(rebound.policy_id() != p.policy_id());
  check(te::PolicyRuntime(rebound).decide(query(rebound)).context() == result.context());
  auto other_controls = p.options();
  other_controls.mutable_action_rules(0)->set_reason("changed control metadata");
  const auto control_variant = te::compile_runtime_policy(p.snapshot(), other_controls);
  check(control_variant.policy_id() != p.policy_id());
  check(te::PolicyRuntime(control_variant).decide(query(control_variant)).context() ==
        result.context());
  auto replan = query(p, "loop");
  replan.set_iteration(2);
  replan.set_repeated_action_steps(1);
  auto other_replan = query(control_variant, "loop");
  other_replan.set_iteration(2);
  other_replan.set_repeated_action_steps(1);
  const auto recovery = runtime.decide(replan);
  check(recovery.progress().decision() == "replan" && !recovery.context().empty());
  check(recovery.context().find(p.policy_id()) == std::string::npos);
  check(te::PolicyRuntime(control_variant).decide(other_replan).context() == recovery.context());
  check(result.context().find("\"trigger_pattern\":\"视频生成缺少素材\"") != std::string::npos);
  check(result.context().find("\"target_scope\":\"coordinator\"") != std::string::npos);
  check(result.context().find("缺少图片仍重试") == std::string::npos);
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1");
  memory.set_revision(1);
  for (const auto *id : {"advice-one", "advice-two"}) {
    auto *entry = memory.add_entries();
    entry->set_memory_id(id);
    entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
    auto *candidate = entry->mutable_candidate();
    candidate->set_schema_version(1);
    candidate->set_candidate_id(id);
    candidate->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
    candidate->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
    candidate->set_target_scope("coordinator");
    candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
    candidate->set_trigger_pattern("视频生成缺少素材");
    candidate->set_observed_problem("缺少图片仍重试");
    candidate->set_proposed_change("请先补充图片素材，独立检索任务继续。");
    candidate->set_support_user_count(1);
    auto *evidence = candidate->add_support_evidence();
    evidence->set_evidence_id(id);
    evidence->mutable_source()->set_user_id("user-a");
    evidence->mutable_source()->set_dialogue_id(id);
  }
  auto duplicate_snapshot =
      te::compile_shadow_policy(memory, p.snapshot().identity(), p.snapshot().options());
  auto duplicate_options = p.options();
  duplicate_options.clear_approved_experiences();
  for (const auto &entry : duplicate_snapshot.experiences()) {
    auto *approval = duplicate_options.add_approved_experiences();
    approval->set_memory_id(entry.memory_id());
    approval->set_content_revision(entry.content_revision());
  }
  const auto deduplicated =
      te::PolicyRuntime(te::compile_runtime_policy(duplicate_snapshot, duplicate_options))
          .decide(q);
  check(deduplicated.experiences_size() == 1);
  std::vector<std::future<void>> injections;
  for (int i = 0; i < 24; ++i)
    injections.push_back(std::async(
        std::launch::async, [&] { check(runtime.decide(q).context() == result.context()); }));
  for (auto &injection : injections)
    injection.get();
  auto bounded_options = p.options();
  bounded_options.set_experience_template(std::string(300, 'x') + "{{experience}}");
  bounded_options.set_max_context_bytes(512);
  const auto bounded =
      te::PolicyRuntime(te::compile_runtime_policy(p.snapshot(), bounded_options)).decide(q);
  check(bounded.status() == "no_change" && bounded.context().empty() &&
        bounded.experiences().empty());
  auto legacy = p;
  legacy.set_policy_id("runtime-v1:" + p.policy_id().substr(std::string("runtime-v2:").size()));
  rejects([&] { te::PolicyRuntime incompatible(legacy); });
  auto old_snapshot = p.snapshot();
  old_snapshot.set_schema_version(1);
  try {
    te::compile_runtime_policy(old_snapshot, p.options());
    throw std::runtime_error("legacy snapshot was promoted into an active policy");
  } catch (const agentflow::trajectory::EvaluationError &exc) {
    check(std::string(exc.what()) == "runtime_experience_contract_missing");
  }
  v1::RuntimeObservation observed;
  observed.set_transport_allowed(true);
  observed.set_call_id("actual-call");
  auto *message = (*observed.mutable_request()->mutable_fields())["messages"]
                      .mutable_list_value()
                      ->add_values()
                      ->mutable_struct_value();
  (*message->mutable_fields())["role"].set_string_value("system");
  (*message->mutable_fields())["content"].set_string_value("host system\n" + result.context());
  check(runtime.verify(q, observed).application().status() == "applied");
  observed.set_transport_allowed(false);
  check(runtime.verify(q, observed).application().status() == "failed");
  observed.set_transport_allowed(true);
  (*message->mutable_fields())["content"].set_string_value(result.context() + result.context());
  check(runtime.verify(q, observed).application().status() == "failed");
  (*message->mutable_fields())["content"].set_string_value("truncated");
  check(runtime.verify(q, observed).application().status() == "failed");
  auto condition_removed = result.context();
  condition_removed.erase(condition_removed.find("视频生成缺少素材"),
                          std::string("视频生成缺少素材").size());
  (*message->mutable_fields())["content"].set_string_value(condition_removed);
  check(runtime.verify(q, observed).application().status() == "failed");
  auto options = p.options();
  options.mutable_approved_experiences(0)->set_content_revision("wrong-version");
  check(te::PolicyRuntime(te::compile_runtime_policy(p.snapshot(), options)).decide(q).status() ==
        "no_change");
  q.mutable_advice_query()->mutable_identity()->set_user_id("another-user");
  rejects([&] { runtime.decide(q); });
  options = p.options();
  options.clear_host_capabilities();
  options.add_host_capabilities("context.request");
  rejects([&] { te::compile_runtime_policy(p.snapshot(), options); });
  options.set_fallback("downgrade");
  check(te::compile_runtime_policy(p.snapshot(), options).effective_level() == v1::L1);
  p.mutable_options()->set_max_context_bytes(1024);
  rejects([&] { te::PolicyRuntime altered(p); });
}

// 测试辅助函数：evolvable_applicability。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void explicit_experience_allowlist() {
  using te::integration::Json;
  const auto p = policy();
  const auto original = query(p);
  const auto legacy = te::PolicyRuntime(p).decide(original);
  check(!legacy.context().empty());
  auto q = original;
  const auto ref = te::integration::json(p.options().approved_experiences(0));
  q.mutable_advice_query()->set_state(Json{{"retrieval_state", ""},
      {"runtime_experience_allowlist", Json::Array{ref}}}.dump());
  te::PolicyRuntime restricted(p);
  const auto selected = restricted.decide(q);
  check(selected.context() == legacy.context());
  check(restricted.decide(q).SerializeAsString() == selected.SerializeAsString());
  q.mutable_advice_query()->set_state(Json{{"retrieval_state", ""},
      {"runtime_experience_allowlist", Json::Array{}}}.dump());
  check(te::PolicyRuntime(p).decide(q).context().empty());
  auto invalid = ref;
  invalid["content_revision"] = "wrong-revision";
  q.mutable_advice_query()->set_state(Json{{"retrieval_state", ""},
      {"runtime_experience_allowlist", Json::Array{invalid}}}.dump());
  rejects([&] { te::PolicyRuntime(p).decide(q); });
  check(te::PolicyRuntime(p).decide(original).SerializeAsString() == legacy.SerializeAsString());
}

void explicit_allowlist_reaches_real_model_boundary() {
  namespace in = te::integration;
  using in::Json;
  const auto old = policy();
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1); memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1"); memory.set_revision(1);
  auto *entry = memory.add_entries(); entry->set_memory_id("experience-a");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *source = entry->mutable_candidate(); source->set_schema_version(1);
  source->set_candidate_id("candidate-a");
  source->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  source->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  source->set_target_scope("agent:agent-a/actor:coordinator");
  source->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  source->set_trigger_pattern(old.snapshot().experiences(0).trigger_pattern());
  source->set_observed_problem(old.snapshot().experiences(0).observed_problem());
  source->set_proposed_change(old.snapshot().experiences(0).content()); source->set_support_user_count(1);
  auto *support = source->add_support_evidence(); support->set_evidence_id("local-real-boundary");
  support->mutable_source()->set_user_id("user-a"); support->mutable_source()->set_dialogue_id("source-a");
  auto limits = old.snapshot().options(); limits.clear_target_scopes();
  limits.add_target_scopes("agent:agent-a/actor:coordinator");
  const auto snapshot = te::compile_shadow_policy(memory, old.snapshot().identity(), limits);
  auto options = old.options(); options.clear_action_rules(); options.clear_loop_rules();
  options.clear_approved_experiences();
  for (const auto &card : snapshot.experiences()) {
    auto *ref = options.add_approved_experiences(); ref->set_memory_id(card.memory_id());
    ref->set_content_revision(card.content_revision());
  }
  const auto compiled = te::compile_runtime_policy(snapshot, options);
  const Json config{{"agent_id", "agent-a"}, {"actors", Json::Array{"coordinator"}}};
  auto call = [&](bool restrict) {
    Json run{{"identity", in::json(snapshot.identity())}, {"level", 3}, {"input", "视频生成缺少图片素材"},
      {"capabilities", Json::Array{"model"}}, {"events", Json::Array{Json{{"event_id", "begin"}}}},
      {"policy", in::json(compiled)}};
    in::initialize_boundary_run(run, Json::Object{});
    Json request{{"operation", "activity.begin"}, {"request_id", "actual-model"}, {"kind", "model"},
      {"name", "llm"}, {"actor_id", "coordinator"}, {"scope_id", "coordinator"},
      {"input", "NEXT"}, {"binding", Json{{"pointer", ""}, {"mode", "text"}}}};
    if (restrict) request["extensions"] = Json{{"runtime_experience_allowlist", Json::Array{}}};
    te::PolicyRuntime runtime(compiled);
    return in::dispatch_boundary(run, request, config, &runtime);
  };
  check(call(false)["input"].text() != "NEXT");
  check(call(true)["input"].text() == "NEXT");
}

void evolvable_applicability() {
  const auto original = policy();
  auto options = original.options();
  auto *rewrite = options.add_experience_overrides();
  *rewrite->mutable_source_experience() = original.options().approved_experiences(0);
  rewrite->set_content(original.snapshot().experiences(0).content());
  rewrite->set_trigger_pattern("network timeout");
  auto changed = te::compile_runtime_policy(original.snapshot(), options);
  check(changed.policy_id() != original.policy_id());
  check(changed.snapshot().SerializeAsString() == original.snapshot().SerializeAsString());
  auto q = query(changed);
  check(te::PolicyRuntime(changed).decide(q).status() == "no_change");
  q.mutable_advice_query()->set_task("network timeouts");
  check(te::PolicyRuntime(changed).decide(q).context().find("network timeout") !=
        std::string::npos);
  check(te::PolicyRuntime(changed).decide(q).context().find(
            original.snapshot().experiences(0).trigger_pattern()) == std::string::npos);
  // 收窄条件中的显式执行者必须来自结构化 actor_id；自然语言里的 query/weather
  // 以及泛化“返回”词不能让天气经验注入非天气任务。
  rewrite->set_trigger_pattern(
      "web-search 执行者使用 query 检索天气衍生指标（如风力、体感温度），且 weather "
      "工具未直接返回该字段时");
  changed = te::compile_runtime_policy(original.snapshot(), options);
  q = query(changed);
  q.set_actor_id("office-work");
  q.mutable_advice_query()->set_task("规划北京到上海的跨城搬家清单");
  q.mutable_advice_query()->set_state(
      "office-work 使用 query 返回行政事务清单；weather 只是接口文档中的示例。");
  q.add_observed_operations("sim_office_work");
  check(te::PolicyRuntime(changed).decide(q).status() == "no_change");
  q.set_actor_id("web-search");
  q.clear_observed_operations();
  q.add_observed_operations("sim_web_search");
  check(te::PolicyRuntime(changed).decide(q).status() == "no_change");
  q.mutable_advice_query()->set_task("查询上海天气和体感温度");
  q.mutable_advice_query()->set_state(
      "web-search 使用 query 检索上海天气；weather 工具返回气温，但需要估算体感温度。");
  check(te::PolicyRuntime(changed).decide(q).status() == "ready");
  rewrite->set_trigger_pattern("network timeout");
  rewrite->add_observed_operations("download");
  changed = te::compile_runtime_policy(original.snapshot(), options);
  check(te::PolicyRuntime(changed).decide(q).status() == "no_change");
  q.mutable_advice_query()->set_state("We may call download later; network timeout");
  check(te::PolicyRuntime(changed).decide(q).status() == "no_change");
  q.add_observed_operations("read_file");
  check(te::PolicyRuntime(changed).decide(q).status() == "no_change");
  q.add_observed_operations("download");
  check(te::PolicyRuntime(changed).decide(q).status() == "ready");
  v1::RuntimePolicy restored;
  check(restored.ParseFromString(changed.SerializeAsString()));
  check(te::PolicyRuntime(restored).decide(q).SerializeAsString() ==
        te::PolicyRuntime(changed).decide(q).SerializeAsString());
  // L1 适用性不能禁用旧版由来源约束的动作保护。
  *options.mutable_action_rules(0)->mutable_source_experience() =
      original.options().approved_experiences(0);
  const auto guarded = te::compile_runtime_policy(original.snapshot(), options);
  auto action_query = query(guarded, "actions");
  auto *action = action_query.add_actions();
  action->set_action_id("a");
  action->set_operation("video");
  check(te::PolicyRuntime(guarded).decide(action_query).actions(0).decision() == "reject");
  options.mutable_experience_overrides(0)->set_trigger_pattern(" \n ");
  rejects([&] { te::compile_runtime_policy(original.snapshot(), options); });
}

// 测试辅助函数：disabled_injection_keeps_control_approval。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void disabled_injection_keeps_control_approval() {
  const auto original = policy();
  auto options = original.options();
  const auto source = options.approved_experiences(0);
  *options.mutable_action_rules(0)->mutable_source_experience() = source;
  *options.mutable_loop_rules(0)->mutable_source_experience() = source;
  // 旧版由检索触发的控制也要保留，不只保留条件谓词规则。
  const auto parent = te::compile_runtime_policy(original.snapshot(), options);
  *options.add_disabled_injections() = source;
  auto disabled = te::compile_runtime_policy(original.snapshot(), options);
  check(disabled.policy_id() != parent.policy_id());
  check(disabled.snapshot().SerializeAsString() == parent.snapshot().SerializeAsString());
  check(te::PolicyRuntime(parent).decide(query(parent)).experiences_size() == 1);
  check(te::PolicyRuntime(disabled).decide(query(disabled)).context().empty());
  auto actions = query(disabled, "actions");
  auto *action = actions.add_actions();
  action->set_action_id("missing-image");
  action->set_operation("video");
  check(te::PolicyRuntime(disabled).decide(actions).actions(0).decision() == "reject");
  auto loop = query(disabled, "loop");
  loop.set_iteration(7);
  loop.set_state_fingerprint("state");
  check(te::PolicyRuntime(disabled).decide(loop).progress().decision() == "stop");
  v1::RuntimePolicy reloaded;
  check(reloaded.ParseFromString(disabled.SerializeAsString()));
  check(te::PolicyRuntime(reloaded).decide(query(reloaded)).context().empty());
  check(te::PolicyRuntime(reloaded).decide(actions).actions(0).decision() == "reject");
  auto restored = disabled.options();
  restored.clear_disabled_injections();
  check(te::compile_runtime_policy(disabled.snapshot(), restored).policy_id() ==
        parent.policy_id());
  auto invalid = options;
  invalid.mutable_disabled_injections(0)->set_content_revision("not-approved");
  rejects([&] { te::compile_runtime_policy(original.snapshot(), invalid); });
  invalid = options;
  *invalid.add_disabled_injections() = source;
  rejects([&] { te::compile_runtime_policy(original.snapshot(), invalid); });
  invalid = options;
  invalid.clear_approved_experiences();
  rejects([&] { te::compile_runtime_policy(original.snapshot(), invalid); });
  invalid = options;
  *invalid.mutable_disabled_injections(0) = options.approved_experiences(1);
  rejects([&] { te::compile_runtime_policy(original.snapshot(), invalid); });
}

// 测试辅助函数：independent_actions。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void independent_actions() {
  auto p = policy();
  te::PolicyRuntime runtime(p);
  auto q = query(p, "actions");
  for (auto id : {"video-bad", "video-ok", "independent"}) {
    auto *action = q.add_actions();
    action->set_action_id(id);
    action->set_operation(std::string(id) == "independent" ? "search" : "video");
    if (std::string(id) == "video-ok")
      (*action->mutable_arguments()->mutable_fields())["image"].set_string_value("image.png");
  }
  auto result = runtime.decide(q);
  check(result.actions_size() == 3 && result.actions(0).decision() == "reject" &&
        result.actions(1).action_id() == "video-ok" && result.actions(1).decision() == "allow" &&
        result.actions(2).decision() == "allow");
  v1::RuntimeObservation observed;
  observed.add_skipped_action_ids("video-bad");
  observed.add_executed_action_ids("independent");
  observed.add_executed_action_ids("video-ok");
  check(runtime.verify(q, observed).application().status() == "applied");
  observed.add_executed_action_ids("video-bad");
  check(runtime.verify(q, observed).application().status() == "failed");
  *q.add_actions() = q.actions(0);
  rejects([&] { runtime.decide(q); });
}

// 测试辅助函数：loop_controls。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void loop_controls() {
  auto p = policy();
  te::PolicyRuntime runtime(p);
  auto q = query(p, "loop");
  q.set_iteration(1);
  q.set_state_fingerprint("unchanged-state");
  auto first = runtime.decide(q);
  check(first.progress().decision() == "continue");
  *q.mutable_previous() = first.progress();
  q.mutable_previous()->set_policy_id("different-run-policy");
  rejects([&] { runtime.decide(q); });
  *q.mutable_previous() = first.progress();
  q.set_iteration(2);
  auto replan = runtime.decide(q);
  check(replan.progress().decision() == "replan");
  check(!replan.context().empty() && replan.progress().replans() == 1);
  v1::RuntimeObservation observed;
  observed.set_loop_decision("replan");
  check(runtime.verify(q, observed).application().status() == "failed"); // 排队不等于已应用。
  *q.mutable_previous() = replan.progress();
  check(runtime.decide(q).progress().replans() == 1); // 重复迭代处理必须幂等。
  q.set_iteration(3);
  auto stopped = runtime.decide(q);
  check(stopped.progress().decision() == "stop");
  observed.set_loop_decision("stop");
  check(runtime.verify(q, observed).application().status() == "applied");
  q.set_pending_actions(true);
  check(runtime.decide(q).progress().decision() == "continue");
  check(runtime.verify(q, observed).application().status() != "applied");
  q.set_pending_actions(false);
  q.set_execution_id("parallel-other");
  rejects([&] { runtime.decide(q); });
  q.clear_previous();
  q.set_state_fingerprint("");
  q.set_iteration(6);
  check(runtime.decide(q).progress().decision() == "continue");
  q.set_iteration(7);
  check(runtime.decide(q).progress().decision() == "stop");
  std::vector<std::future<void>> tasks;
  for (int i = 0; i < 24; ++i)
    tasks.push_back(std::async(std::launch::async, [&, i] {
      auto independent = query(p, "loop");
      independent.set_iteration(1);
      independent.set_execution_id("branch-" + std::to_string(i));
      check(runtime.decide(independent).progress().decision() == "continue");
    }));
  for (auto &task : tasks)
    task.get();
}

// 测试辅助函数：generated_controls。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void generated_controls() {
  namespace in = te::integration;
  const auto p = policy();
  auto base = p.options();
  base.clear_action_rules();
  base.mutable_loop_rules(0)->set_max_no_progress(3);
  const auto settings =
      in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt","actions":{"video":["image"]}})");
  const auto generation_input = in::control_generation_input(p.snapshot(), base, settings);
  check(generation_input["memories"].items().size() == 1);
  check(generation_input["control_sources"].items().size() == 1 &&
        generation_input["control_sources"].items().front().str("memory_id") == "orchestration-z");
  auto redundant_replan = base;
  redundant_replan.set_aggressive_controls(true);
  redundant_replan.mutable_loop_rules(0)->set_max_no_progress(1);
  const auto redundant_input =
      in::control_generation_input(p.snapshot(), redundant_replan, settings);
  check(std::none_of(redundant_input["loop_primitives"].items().begin(),
                     redundant_input["loop_primitives"].items().end(), [](const auto &primitive) {
                       return primitive.str("primitive_id") == "replan_on_failure";
                     }));
  const auto redundant_draft = in::Json::parse(R"({"action_rules":[],"loop_rules":[
    {"memory_id":"experience-a","replan_on_failure":true,"reason":"与基础阈值相同"}]})");
  bool redundant_rejected = false;
  try {
    in::apply_control_draft(p.snapshot(), redundant_replan, settings, redundant_draft);
  } catch (const in::Error &error) {
    redundant_rejected = true;
    check(std::string(error.what()) == "control_failure_replan_not_authorized");
  }
  check(redundant_rejected);
  const auto draft = in::Json::parse(
      R"({"action_rules":[{"memory_id":"experience-a","operation":"video","required_arguments":["image"],"reason":"先确认素材"}],"loop_rules":[{"memory_id":"experience-a","max_iterations":6,"max_no_progress":2,"reason":"限制无进展重复"}]})");
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, te::compile_runtime_policy(p.snapshot(), base),
                                          settings));
  auto q = query(learned, "actions");
  auto *action = q.add_actions();
  action->set_action_id("bad");
  action->set_operation("video");
  auto *sibling = q.add_actions();
  sibling->set_action_id("good");
  sibling->set_operation("search");
  const auto matched = te::PolicyRuntime(learned).decide(q);
  check(matched.actions(0).decision() == "reject" && matched.actions(1).decision() == "allow");
  check(matched.experiences_size() == 1);
  q.mutable_advice_query()->set_task("完全无关的算术问题");
  check(te::PolicyRuntime(learned).decide(q).actions(0).decision() == "reject");
  auto invalid = draft;
  invalid["loop_rules"].items()[0]["max_iterations"] = 100;
  bool rejected = false;
  try {
    in::apply_control_draft(p.snapshot(), base, settings, invalid);
  } catch (...) {
    rejected = true;
  }
  check(rejected);
  invalid = draft;
  invalid["action_rules"].items()[0]["operation"] = "shell";
  rejected = false;
  try {
    in::apply_control_draft(p.snapshot(), base, settings, invalid);
  } catch (...) {
    rejected = true;
  }
  check(rejected);
  auto altered = learned;
  altered.mutable_options()->mutable_loop_rules(1)->set_max_iterations(100);
  check(!in::generated_controls_authorized(altered, p, settings));
  const auto automatic_settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt"})");
  const auto automatic_draft = in::Json::parse(
      R"({"action_rules":[{"memory_id":"experience-a","primitive_id":"schema_required_nonempty","reason":"必填参数不能为空"}],"loop_rules":[]})");
  const auto automatic = te::compile_runtime_policy(
      p.snapshot(),
      in::apply_control_draft(p.snapshot(), base, automatic_settings, automatic_draft));
  check(in::generated_controls_authorized(automatic, te::compile_runtime_policy(p.snapshot(), base),
                                          automatic_settings));
  auto automatic_query = query(automatic, "actions");
  auto *schema_action = automatic_query.add_actions();
  schema_action->set_action_id("schema-action");
  schema_action->set_operation("any-tool");
  schema_action->add_schema_required_arguments("items");
  (*schema_action->mutable_arguments()->mutable_fields())["items"].mutable_list_value();
  check(te::PolicyRuntime(automatic).decide(automatic_query).actions(0).decision() == "reject");
  (*schema_action->mutable_arguments()->mutable_fields())["items"]
      .mutable_list_value()
      ->add_values()
      ->set_string_value("one");
  check(te::PolicyRuntime(automatic).decide(automatic_query).actions(0).decision() == "allow");
  schema_action->add_schema_required_arguments("items");
  rejects([&] { te::PolicyRuntime(automatic).decide(automatic_query); });
  const auto retry_draft = in::Json::parse(
      R"({"action_rules":[{"memory_id":"experience-a","primitive_id":"repeat_failed_operation","reason":"同一技能连续两次失败后停止重复派发"}],"loop_rules":[]})");
  const auto retry_policy = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, automatic_settings, retry_draft));
  check(in::generated_controls_authorized(
      retry_policy, te::compile_runtime_policy(p.snapshot(), base), automatic_settings));
  auto retry_query = query(retry_policy, "actions");
  auto *retry_action = retry_query.add_actions();
  retry_action->set_action_id("failed-skill");
  retry_action->set_operation("forge-image-gen");
  retry_action->set_prior_failures(1);
  auto *independent_action = retry_query.add_actions();
  independent_action->set_action_id("independent-skill");
  independent_action->set_operation("outfit-stylist");
  check(te::PolicyRuntime(retry_policy).decide(retry_query).actions(0).decision() == "allow");
  retry_action->set_prior_failures(2);
  const auto guarded = te::PolicyRuntime(retry_policy).decide(retry_query);
  check(guarded.actions(0).decision() == "reject" && guarded.actions(1).decision() == "allow");
  // 指定操作的失败护栏不得扩散到其他工具，即使它们也有自己的失败历史。
  auto scoped_retry_draft = retry_draft;
  scoped_retry_draft["action_rules"].items()[0]["operation"] = "forge-image-gen";
  const auto scoped_retry = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, automatic_settings,
                                          scoped_retry_draft));
  check(in::generated_controls_authorized(
      scoped_retry, te::compile_runtime_policy(p.snapshot(), base), automatic_settings));
  auto scoped_query = query(scoped_retry, "actions");
  *scoped_query.add_actions() = *retry_action;
  *scoped_query.add_actions() = *independent_action;
  scoped_query.mutable_actions(1)->set_prior_failures(3);
  const auto scoped_decision = te::PolicyRuntime(scoped_retry).decide(scoped_query);
  check(scoped_decision.actions(0).decision() == "reject" &&
        scoped_decision.actions(1).decision() == "allow");
  scoped_query.mutable_actions(0)->set_prior_failures(1);
  check(te::PolicyRuntime(scoped_retry).decide(scoped_query).actions(0).decision() == "allow");
  for (const auto *invalid_operation : {"", "*"}) {
    auto bad = scoped_retry_draft;
    bad["action_rules"].items()[0]["operation"] = invalid_operation;
    bool invalid_rejected = false;
    try {
      in::apply_control_draft(p.snapshot(), base, automatic_settings, bad);
    } catch (const in::Error &error) {
      invalid_rejected = std::string(error.what()) == "control_operation_not_authorized";
    }
    check(invalid_rejected);
  }
  auto invalid_automatic = automatic;
  invalid_automatic.mutable_options()->mutable_action_rules(0)->set_deny(true);
  rejects([&] {
    te::compile_runtime_policy(invalid_automatic.snapshot(), invalid_automatic.options());
  });
}

// 测试辅助函数：failure_replanning。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void failure_replanning() {
  namespace in = te::integration;
  const auto p = policy();
  auto base = p.options();
  base.mutable_loop_rules(0)->set_max_no_progress(3);
  const auto settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt"})");
  const auto draft = in::Json::parse(
      R"({"action_rules":[],"loop_rules":[{"memory_id":"experience-a","replan_on_failure":true,"reason":"失败后检查恢复路径"}]})");
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, te::compile_runtime_policy(p.snapshot(), base),
                                          settings));
  te::PolicyRuntime runtime(learned);
  auto q = query(learned, "loop");
  q.set_iteration(1);
  q.set_state_fingerprint("initial");
  *q.mutable_previous() = runtime.decide(q).progress();
  q.set_iteration(2);
  q.set_state_fingerprint("changed-error-id");
  q.set_failed_actions(1);
  q.set_pending_actions(true);
  check(runtime.decide(q).progress().decision() == "continue");
  q.set_pending_actions(false);
  auto first = runtime.decide(q);
  check(first.progress().decision() == "continue");
  *q.mutable_previous() = first.progress();
  q.set_iteration(3);
  q.set_state_fingerprint("changed-error-again");
  q.set_repeated_failure_steps(1);
  const auto decision = runtime.decide(q);
  check(decision.progress().decision() == "replan" && decision.progress().no_progress() == 0);
  *q.mutable_previous() = decision.progress();
  q.set_iteration(4);
  q.set_state_fingerprint("different-error");
  q.set_repeated_failure_steps(0);
  check(runtime.decide(q).progress().decision() == "continue");
  q.clear_previous();
  q.set_execution_id("independent");
  q.set_iteration(1);
  q.set_failed_actions(0);
  check(runtime.decide(q).progress().decision() == "continue");
}

void source_bound_auth_replanning() {
  namespace in = te::integration;
  const auto p = policy("Read the public login contract and matching supervisor credentials. "
                        "After a successful login, pass its actual access_token to every "
                        "protected API call. Do not repeat unchanged failed credentials.");
  auto base = p.options();
  base.set_max_context_bytes(2048);
  base.mutable_loop_rules(0)->set_max_no_progress(3);
  const auto original = te::compile_runtime_policy(p.snapshot(), base);
  const auto &source = p.snapshot().experiences(0);
  check(source.memory_id() == "experience-a");
  const auto settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt"})");
  const auto draft = in::Json{
      {"action_rules", in::Json::Array{}},
      {"loop_rules", in::Json::Array{in::Json{
          {"memory_id", source.memory_id()}, {"replan_on_failure", true},
          {"error_codes", in::Json::Array{"appworld_auth_401"}},
          {"replan_context", source.content()}, {"reason", "Recover using the cited auth experience"}}}}};
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(learned.options().loop_rules_size() == 2);
  check(learned.options().loop_rules(1).replan_context() == source.content());
  check(learned.options().loop_rules(1).source_experience().content_revision() ==
        source.content_revision());
  check(learned.options().max_context_bytes() == 2048 &&
        learned.options().loop_rules(1).max_replans() == 1);
  check(learned.snapshot().SerializeAsString() == p.snapshot().SerializeAsString());
  check(in::generated_controls_authorized(learned, original, settings));
  auto legacy = draft;
  legacy["loop_rules"].items()[0].fields().erase("replan_context");
  const auto old = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, legacy));
  check(old.options().loop_rules(1).replan_context() == base.loop_rules(0).replan_context());
  check(in::generated_controls_authorized(old, original, settings));

  auto rejected = [&](const in::Json &value, const v1::RuntimeOptions &limits,
                      const v1::ShadowPolicySnapshot &snapshot) {
    bool failed = false;
    try { in::apply_control_draft(snapshot, limits, settings, value); }
    catch (const in::Error &) { failed = true; }
    check(failed);
  };
  for (const auto &text : {std::string(), source.content() + " unapproved instruction"}) {
    auto bad = draft; bad["loop_rules"].items()[0]["replan_context"] = text;
    rejected(bad, base, p.snapshot());
  }
  auto bad_type = draft; bad_type["loop_rules"].items()[0]["replan_context"] = true;
  rejected(bad_type, base, p.snapshot());
  auto unknown = draft; unknown["loop_rules"].items()[0]["memory_id"] = "not-approved";
  rejected(unknown, base, p.snapshot());
  auto unapproved = base; unapproved.clear_approved_experiences();
  rejected(draft, unapproved, p.snapshot());
  auto wrong_revision = base;
  wrong_revision.mutable_approved_experiences(0)->set_content_revision("wrong-revision");
  rejected(draft, wrong_revision, p.snapshot());
  auto wrong_scope = p.snapshot(); wrong_scope.mutable_experiences(0)->set_target_scope("other");
  rejected(draft, base, wrong_scope);
  for (const auto &codes : {in::Json::Array{}, in::Json::Array{"appworld_api_422"},
                            in::Json::Array{"appworld_auth_401", "appworld_missing_access_token"}}) {
    auto bad = draft; bad["loop_rules"].items()[0]["error_codes"] = codes;
    rejected(bad, base, p.snapshot());
  }
  auto without_condition = draft; without_condition["loop_rules"].items()[0].fields().erase("error_codes");
  rejected(without_condition, base, p.snapshot());
  auto no_failure = draft; no_failure["loop_rules"].items()[0]["replan_on_failure"] = false;
  rejected(no_failure, base, p.snapshot());
  auto no_replan = draft; no_replan["loop_rules"].items()[0]["max_replans"] = 0;
  rejected(no_replan, base, p.snapshot());
  auto over_budget = draft; over_budget["loop_rules"].items()[0]["max_replans"] = 2;
  rejected(over_budget, base, p.snapshot());
  auto two_base = base; two_base.mutable_loop_rules(0)->set_max_replans(2);
  rejected(over_budget, two_base, p.snapshot());
  auto small = base; small.set_max_context_bytes(source.content().size() + 127);
  rejected(draft, small, p.snapshot());
  auto altered = learned;
  altered.mutable_options()->mutable_loop_rules(1)->set_replan_context("unapproved recovery");
  check(!in::generated_controls_authorized(altered, original, settings));
  altered = learned;
  altered.mutable_options()->mutable_loop_rules(1)->mutable_source_experience()->set_content_revision("wrong");
  check(!in::generated_controls_authorized(altered, original, settings));
  altered = learned; altered.mutable_options()->mutable_loop_rules(1)->set_target_scope("other");
  check(!in::generated_controls_authorized(altered, original, settings));

  // A context change is an actual mutation even when the base already replans.
  auto failure_base = base; failure_base.mutable_loop_rules(0)->set_replan_on_failure(true);
  const auto context_only = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), failure_base, settings, draft));
  check(context_only.options().loop_rules_size() == 2 &&
        context_only.options().loop_rules(1).replan_context() == source.content());
  check(in::generated_controls_authorized(
      context_only, te::compile_runtime_policy(p.snapshot(), failure_base), settings));

  for (const auto &code : {"appworld_auth_401", "appworld_missing_access_token"}) {
    auto exact = draft; exact["loop_rules"].items()[0]["error_codes"] = in::Json::Array{code};
    const auto compiled = te::compile_runtime_policy(
        p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, exact));
    check(in::generated_controls_authorized(compiled, original, settings));
    te::PolicyRuntime runtime(compiled);
    auto q = query(compiled, "loop");
    q.set_iteration(1); q.set_state_fingerprint("initial");
    *q.mutable_previous() = runtime.decide(q).progress();
    q.set_iteration(2); q.set_state_fingerprint("first-auth-error");
    q.add_error_codes(code); q.set_failed_actions(1);
    check(runtime.decide(q).progress().decision() == "continue");
    *q.mutable_previous() = runtime.decide(q).progress();
    q.set_iteration(3); q.set_state_fingerprint("repeated-auth-error");
    q.set_repeated_failure_steps(1); q.set_pending_actions(true);
    check(runtime.decide(q).progress().decision() == "continue");
    q.set_pending_actions(false);
    const auto replan = runtime.decide(q);
    check(replan.progress().decision() == "replan" && replan.progress().replans() == 1);
    check(replan.context().find(source.content()) != std::string::npos);
    check(replan.experiences_size() == 1 && replan.experiences(0).memory_id() == source.memory_id());
    v1::RuntimeObservation observed;
    observed.set_loop_decision("replan");
    check(runtime.verify(q, observed).application().status() == "failed");
    observed.set_transport_allowed(true); observed.set_call_id("next-model-request");
    auto *system = (*observed.mutable_request()->mutable_fields())["messages"]
                       .mutable_list_value()->add_values()->mutable_struct_value();
    (*system->mutable_fields())["role"].set_string_value("system");
    (*system->mutable_fields())["content"].set_string_value("Original task\n" + replan.context());
    check(runtime.verify(q, observed).application().status() == "applied");
    observed.mutable_request()->Clear();
    check(runtime.verify(q, observed).application().status() == "failed");
    *q.mutable_previous() = replan.progress();
    q.set_iteration(4); q.set_state_fingerprint("still-failed");
    check(runtime.decide(q).progress().decision() == "continue"); // No second replan.
    q.set_execution_id("independent");
    rejects([&] { runtime.decide(q); }); // Cannot import another execution's progress.
    q.clear_previous(); q.set_iteration(1); q.set_repeated_failure_steps(0);
    q.set_failed_actions(0); q.clear_error_codes();
    check(runtime.decide(q).progress().decision() == "continue");
    q.set_iteration(2); q.set_repeated_failure_steps(1); q.add_error_codes("appworld_api_422");
    check(runtime.decide(q).progress().decision() == "continue");
  }
}

// 测试辅助函数：skill_orchestration。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void skill_orchestration() {
  auto p = policy();
  auto options = p.options();
  options.add_host_capabilities("schedule.dispatch");
  auto *rule = options.add_schedule_rules();
  rule->set_rule_id("plan-before-image");
  rule->set_target_scope("coordinator");
  rule->set_before_operation("planner");
  rule->set_after_operation("image");
  rule->set_require_success(true);
  rule->set_reason("先确认计划，再生成图片");
  for (const auto &source : options.approved_experiences())
    if (source.memory_id() == "orchestration-z")
      *rule->mutable_source_experience() = source;
  p = te::compile_runtime_policy(p.snapshot(), options);
  te::PolicyRuntime runtime(p);
  auto q = query(p, "schedule");
  for (const auto &pair : {std::make_pair("b", "image"), {"a", "planner"}, {"c", "report"}}) {
    auto *action = q.add_actions();
    action->set_action_id(pair.first);
    action->set_operation(pair.second);
  }
  const auto decision = runtime.decide(q);
  check(decision.status() == "ready" && decision.schedule().rule_ids_size() == 1);
  check(decision.schedule().actions(0).action_id() == "b"); // 输入顺序不等于执行顺序。
  auto state = te::begin_schedule(decision.schedule());
  check(te::ready_schedule(state) == std::vector<std::string>({"a", "c"}));
  rejects([&] { te::advance_schedule(state, "b", "running"); });
  rejects([&] { te::advance_schedule(state, "a", "succeeded"); }); // 提交确认不等于任务完成。
  state = te::advance_schedule(state, "a", "running");
  check(te::ready_schedule(state) == std::vector<std::string>({"c"}));
  v1::ScheduleState restored;
  check(restored.ParseFromString(state.SerializeAsString()));
  check(te::ready_schedule(restored) == std::vector<std::string>({"c"}));
  state = te::advance_schedule(restored, "a", "succeeded");
  check(te::ready_schedule(state) == std::vector<std::string>({"b", "c"}));
  check(te::advance_schedule(state, "a", "succeeded").SerializeAsString() ==
        state.SerializeAsString());
  rejects([&] { te::advance_schedule(state, "a", "failed"); });
  v1::RuntimeObservation actual;
  auto observe = [&](const char *id, const char *status) {
    auto *event = actual.add_schedule_events();
    event->set_action_id(id);
    event->set_status(status);
  };
  observe("a", "running");
  observe("c", "running");
  observe("c", "succeeded");
  observe("a", "succeeded");
  observe("b", "running");
  observe("b", "succeeded");
  for (const auto *id : {"a", "b", "c"})
    actual.add_executed_action_ids(id);
  const auto receipt = runtime.verify(q, actual);
  check(receipt.application().status() == "applied" && receipt.rule_ids_size() == 1);
  auto reordered = actual;
  reordered.mutable_schedule_events()->SwapElements(3, 4);
  check(runtime.verify(q, reordered).application().status() == "failed");
  actual.clear_schedule_events();
  check(runtime.verify(q, actual).application().status() ==
        "failed"); // 仅有计划不足以证明实际执行。

  auto failed = te::begin_schedule(decision.schedule());
  failed = te::advance_schedule(failed, "a", "running");
  failed = te::advance_schedule(failed, "a", "failed");
  check(failed.actions(0).status() == "skipped" &&
        te::ready_schedule(failed) == std::vector<std::string>({"c"}));
  failed = te::advance_schedule(failed, "c", "running");
  failed = te::advance_schedule(failed, "c", "succeeded");
  check(te::schedule_finished(failed));
  observe("a", "running");
  observe("a", "failed");
  observe("c", "running");
  observe("c", "succeeded");
  actual.clear_executed_action_ids();
  actual.add_executed_action_ids("a");
  actual.add_executed_action_ids("c");
  check(runtime.verify(q, actual).application().status() == "failed"); // 跳过工作也需要对应观测。
  actual.add_skipped_action_ids("b");
  check(runtime.verify(q, actual).application().status() == "applied");

  auto settled_options = options;
  settled_options.mutable_schedule_rules(0)->set_require_success(false);
  auto settled = te::begin_schedule(te::plan_schedule(settled_options, q));
  settled = te::advance_schedule(settled, "a", "running");
  settled = te::advance_schedule(settled, "a", "failed");
  check(te::ready_schedule(settled) == std::vector<std::string>({"b", "c"}));

  auto plain = options;
  plain.clear_schedule_rules();
  auto parallel = te::begin_schedule(te::plan_schedule(plain, q));
  check(te::ready_schedule(parallel) == std::vector<std::string>({"b", "a", "c"}));
  auto l1 = options;
  l1.set_requested_level(v1::L1);
  check(te::plan_schedule(l1, q).status() == "unchanged");
  auto foreign = q;
  foreign.mutable_advice_query()->set_target_scope("skill:worker");
  check(te::plan_schedule(options, foreign).status() == "unchanged");
  auto duplicate = q;
  auto *a2 = duplicate.add_actions();
  a2->set_action_id("a2");
  a2->set_operation("planner");
  check(te::plan_schedule(options, duplicate).status() == "unchanged");
  auto explicit_join = duplicate;
  for (const auto *predecessor : {"a", "a2"})
    explicit_join.mutable_actions(0)->add_after()->set_action_id(predecessor);
  const auto joined = te::plan_schedule(options, explicit_join);
  check(joined.actions(0).after_size() == 2 && joined.actions(0).after(0).require_success() &&
        joined.actions(0).after(1).require_success());
  auto manual = options;
  manual.mutable_schedule_rules(0)->clear_source_experience();
  check(te::plan_schedule(manual, duplicate).actions(0).after_size() == 2);

  auto multilingual = options;
  multilingual.mutable_schedule_rules(0)->add_task_terms("先完成");
  auto *english = multilingual.add_schedule_rules();
  *english = multilingual.schedule_rules(0);
  english->set_rule_id("plan-before-image-en");
  english->clear_task_terms();
  english->add_task_terms("first complete");
  auto chinese_task = q;
  chinese_task.mutable_advice_query()->set_task("先完成计划，再生成图片");
  auto english_task = q;
  english_task.mutable_advice_query()->set_task("first complete the plan, then generate the image");
  check(te::plan_schedule(multilingual, chinese_task).rule_ids(0) == "plan-before-image");
  check(te::plan_schedule(multilingual, english_task).rule_ids(0) == "plan-before-image-en");
  const auto scheduled_policy = te::compile_runtime_policy(p.snapshot(), multilingual);
  te::PolicyRuntime scheduled(scheduled_policy);
  auto parent_options = multilingual;
  parent_options.clear_schedule_rules();
  const auto parent_policy = te::compile_runtime_policy(p.snapshot(), parent_options);
  te::PolicyRuntime parent(parent_policy);
  // 只改编排规则时，模型实际可见的上下文必须逐字相同；命中 L1 的任务仍保留原经验。
  for (const auto *task : {"先完成计划，再生成图片", "独立生成报告",
                           "先完成计划，再生成图片，视频生成缺少图片素材"}) {
    auto first_model = query(scheduled_policy, "llm");
    first_model.mutable_advice_query()->set_task(task);
    const auto before = parent.decide(first_model);
    const auto after = scheduled.decide(first_model);
    check(before.context() == after.context() &&
          before.experiences_size() == after.experiences_size());
    if (std::string(task).find("视频生成缺少") != std::string::npos)
      check(!after.context().empty() && after.experiences_size() == 1);
    else
      check(after.context().empty());
    first_model.add_observed_operations("planner");
    check(parent.decide(first_model).context() == scheduled.decide(first_model).context());
  }
  // 同批真实执行仍由上面的依赖及回执测试覆盖；拆为两个批次时不可宣称新增了依赖。
  for (const auto *operation : {"planner", "image"}) {
    auto single_batch = chinese_task;
    single_batch.clear_actions();
    auto *action = single_batch.add_actions();
    action->set_action_id(operation);
    action->set_operation(operation);
    const auto unchanged = scheduled.decide(single_batch);
    check(unchanged.status() == "no_change" && unchanged.schedule().status() == "unchanged" &&
          unchanged.schedule().rule_ids_size() == 0);
    v1::RuntimeObservation single_observation;
    single_observation.add_executed_action_ids(operation);
    for (const auto *status : {"running", "succeeded"}) {
      auto *event = single_observation.add_schedule_events();
      event->set_action_id(operation);
      event->set_status(status);
    }
    const auto no_application = scheduled.verify(single_batch, single_observation);
    check(no_application.application().status() == "rejected" &&
          no_application.rule_ids_size() == 0);
  }
  auto same_guard = multilingual;
  same_guard.mutable_schedule_rules(1)->set_task_terms(0, "先完成");
  rejects([&] { te::validate_schedule_rules(same_guard); });

  // 学习依赖形成循环时，宿主已有前置依赖优先。
  auto host_order = q;
  host_order.mutable_actions(1)->add_after()->set_action_id("b");
  const auto conflict = te::plan_schedule(options, host_order);
  check(conflict.status() == "conflict" && conflict.rule_ids_size() == 0);
  check(te::ready_schedule(te::begin_schedule(conflict)) == std::vector<std::string>({"b", "c"}));
  auto cyclic = options;
  auto *reverse = cyclic.add_schedule_rules();
  *reverse = cyclic.schedule_rules(0);
  reverse->set_rule_id("reverse");
  reverse->set_before_operation("image");
  reverse->set_after_operation("planner");
  rejects([&] { te::compile_runtime_policy(p.snapshot(), cyclic); });
  auto alternative = cyclic;
  alternative.mutable_schedule_rules(0)->add_task_terms("先完成计划");
  alternative.mutable_schedule_rules(0)->add_exclude_terms("先生成图片");
  alternative.mutable_schedule_rules(1)->add_task_terms("先生成图片");
  alternative.mutable_schedule_rules(1)->add_exclude_terms("先完成计划");
  const auto alternative_policy = te::compile_runtime_policy(p.snapshot(), alternative);
  auto forward_task = q;
  forward_task.mutable_advice_query()->set_task("先完成计划，然后生成图片");
  auto reverse_task = q;
  reverse_task.mutable_advice_query()->set_task("先生成图片，然后完成计划");
  check(te::PolicyRuntime(alternative_policy).decide(forward_task).schedule().rule_ids(0) ==
        "plan-before-image");
  check(te::PolicyRuntime(alternative_policy).decide(reverse_task).schedule().rule_ids(0) ==
        "reverse");
  alternative.mutable_schedule_rules(0)->clear_exclude_terms();
  alternative.mutable_schedule_rules(1)->clear_exclude_terms();
  rejects([&] { te::compile_runtime_policy(p.snapshot(), alternative); });
  alternative.mutable_schedule_rules(0)->add_exclude_terms("计划");
  rejects([&] { te::compile_runtime_policy(p.snapshot(), alternative); });
  auto invalid = q;
  invalid.mutable_actions(0)->add_after()->set_action_id("not-in-batch");
  rejects([&] { te::plan_schedule(options, invalid); });
  invalid = q;
  invalid.mutable_actions(0)->add_after()->set_action_id("b");
  rejects([&] { te::plan_schedule(options, invalid); });
  auto compatible = p.options();
  compatible.clear_schedule_rules();
  compatible.clear_host_capabilities();
  compatible.set_requested_level(v1::L2);
  compatible.add_host_capabilities("schedule.dispatch");
  check(te::compile_runtime_policy(p.snapshot(), compatible).effective_level() == v1::L2);

  namespace in = te::integration;
  const auto settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt"})");
  const auto draft = in::Json::parse(
      R"({"action_rules":[],"loop_rules":[],"schedule_rules":[{"memory_id":"experience-a","before_operation":"planner","after_operation":"image","require_success":true,"task_terms":["先完成","生成视频"],"exclude_terms":["互不依赖"],"reason":"先确认计划再生成"}]})");
  auto base = options;
  base.clear_schedule_rules();
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, te::compile_runtime_policy(p.snapshot(), base),
                                          settings));
  check(in::without_generated_controls(learned.options()).schedule_rules_size() == 0);
  auto dependent = q;
  dependent.mutable_advice_query()->set_task("请先完成 planner，再生成视频");
  check(te::PolicyRuntime(learned).decide(dependent).schedule().status() == "scheduled");
  const auto variant_draft = in::Json::parse(
      R"({"action_rules":[],"loop_rules":[],"schedule_rules":[{"memory_id":"experience-a","before_operation":"planner","after_operation":"image","require_success":true,"task_terms":["先完成"],"reason":"先完成计划"},{"memory_id":"experience-a","before_operation":"planner","after_operation":"image","require_success":true,"task_terms":["first complete"],"reason":"Complete the plan first"}]})");
  const auto variant_policy = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, variant_draft));
  check(in::generated_controls_authorized(
      variant_policy, te::compile_runtime_policy(p.snapshot(), base), settings));
  auto english_dependent = q;
  english_dependent.mutable_advice_query()->set_task("first complete planner, then generate image");
  check(te::PolicyRuntime(variant_policy).decide(english_dependent).schedule().status() ==
        "scheduled");
  auto independent = q;
  independent.mutable_advice_query()->set_task("请分别完成 planner 和生成视频，两个任务互不依赖");
  check(te::PolicyRuntime(learned).decide(independent).schedule().status() == "unchanged");
  independent.mutable_advice_query()->set_task("请先完成 planner，再生成视频；但两项互不依赖");
  check(te::PolicyRuntime(learned).decide(independent).schedule().status() == "unchanged");
  auto changed = learned;
  changed.mutable_options()->mutable_schedule_rules(0)->set_rule_id("unverified-rule");
  check(!in::generated_controls_authorized(changed, te::compile_runtime_policy(p.snapshot(), base),
                                           settings));
  check(in::control_generation_input(p.snapshot(), base, settings).has("schedule_primitives"));
  base.clear_host_capabilities();
  base.add_host_capabilities("action.filter");
  check(!in::control_generation_input(p.snapshot(), base, settings).has("schedule_primitives"));
  try {
    in::apply_control_draft(p.snapshot(), base, settings, draft);
    throw std::runtime_error("schedule capability missing");
  } catch (const in::Error &error) {
    check(std::string(error.what()) == "control_schedule_not_authorized");
  }
}

// 测试辅助函数：aggressive_controls。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void aggressive_controls() {
  namespace in = te::integration;
  const auto p = policy();
  auto opts = p.options();
  *opts.mutable_action_rules(0)->mutable_source_experience() = opts.approved_experiences(0);
  *opts.mutable_loop_rules(0)->mutable_source_experience() = opts.approved_experiences(0);
  opts.mutable_loop_rules(0)->set_no_progress_decision("stop");
  const auto strict = te::compile_runtime_policy(p.snapshot(), opts);
  auto q = query(strict, "actions");
  q.mutable_advice_query()->set_task("unrelated language");
  auto *action = q.add_actions();
  action->set_action_id("video-a");
  action->set_operation("video");
  check(te::PolicyRuntime(strict).decide(q).actions(0).decision() == "allow");
  opts.set_aggressive_controls(true);
  const auto aggressive = te::compile_runtime_policy(p.snapshot(), opts);
  check(aggressive.policy_id() != strict.policy_id());
  check(te::PolicyRuntime(aggressive).decide(q).actions(0).decision() == "reject");
  q.clear_actions();
  q.set_boundary("loop");
  q.set_iteration(1);
  q.set_state_fingerprint("new-state");
  q.set_repeated_failure_steps(1);
  check(te::PolicyRuntime(strict).decide(q).progress().decision() == "continue");
  check(te::PolicyRuntime(aggressive).decide(q).progress().decision() == "stop");
  q.set_pending_actions(true);
  check(te::PolicyRuntime(aggressive).decide(q).progress().decision() == "continue");
  q.set_pending_actions(false);
  q.mutable_advice_query()->set_target_scope("skill:worker");
  check(te::PolicyRuntime(aggressive).decide(q).progress().decision() == "continue");
  auto base = p.options();
  base.set_aggressive_controls(true);
  base.clear_action_rules();
  const auto settings =
      in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt","min_iterations":1})");
  const auto draft = in::Json::parse(
      R"({"action_rules":[],"loop_rules":[{"memory_id":"experience-a","max_iterations":2,"reason":"按实际试验缩短分支预算"}]})");
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, te::compile_runtime_policy(p.snapshot(), base),
                                          settings));
  const auto actor_draft = in::Json::parse(
      R"({"action_rules":[],"loop_rules":[{"memory_id":"experience-a","actor_id":"coordinator","max_iterations":2,"reason":"只限制协调分支"}]})");
  const auto actor_policy = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, actor_draft));
  auto actor_query = query(actor_policy, "loop");
  actor_query.set_iteration(3);
  actor_query.set_state_fingerprint("new-state");
  actor_query.set_actor_id("coordinator");
  check(te::PolicyRuntime(actor_policy).decide(actor_query).progress().decision() == "stop");
  actor_query.set_actor_id("worker");
  check(te::PolicyRuntime(actor_policy).decide(actor_query).progress().decision() == "continue");
  const auto aggressive_input = in::control_generation_input(p.snapshot(), base, settings);
  check(aggressive_input["loop_primitives"].items().size() == 3);
  for (const auto &primitive : aggressive_input["loop_primitives"].items())
    check(primitive.str("primitive_id") != "replan_on_failure");
  auto stopped = base;
  auto *stopped_loop = stopped.mutable_loop_rules(0);
  stopped_loop->set_no_progress_decision("stop");
  stopped_loop->set_max_replans(0);
  const auto stopped_input = in::control_generation_input(p.snapshot(), stopped, settings);
  check(stopped_input["loop_primitives"].items().size() == 2);
  for (const auto &primitive : stopped_input["loop_primitives"].items())
    check(primitive.str("primitive_id") != "replan_on_failure");
  base.set_aggressive_controls(false);
  try {
    in::apply_control_draft(p.snapshot(), base, settings, draft);
    throw std::runtime_error("unapproved aggressive limit");
  } catch (const in::Error &error) {
    check(std::string(error.what()) == "control_loop_semantic_scope_required");
  }
}

// 测试辅助函数：actor_scoped_loops。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void actor_scoped_loops() {
  namespace in = te::integration;
  const auto original = policy();
  auto base = original.options();
  base.set_aggressive_controls(true);
  const auto settings =
      in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt","min_iterations":1})");
  const auto draft = in::Json::parse(R"({"action_rules":[],"loop_rules":[
    {"memory_id":"experience-a","actor_id":"planner","max_iterations":2,"reason":"规划分支预算"},
    {"memory_id":"experience-a","actor_id":"worker","max_iterations":4,"reason":"执行分支预算"},
    {"memory_id":"experience-a","max_iterations":5,"reason":"其他分支预算"}]})");
  const auto compiled = te::compile_runtime_policy(
      original.snapshot(), in::apply_control_draft(original.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(
      compiled, te::compile_runtime_policy(original.snapshot(), base), settings));
  for (bool reverse : {false, true}) {
    auto options = compiled.options();
    if (reverse)
      options.mutable_loop_rules()->SwapElements(1, 3);
    const auto current = te::compile_runtime_policy(original.snapshot(), options);
    auto q = query(current, "loop");
    q.set_iteration(3);
    q.set_state_fingerprint("progress");
    q.set_actor_id("planner");
    check(te::PolicyRuntime(current).decide(q).progress().decision() == "stop");
    q.set_actor_id("worker");
    check(te::PolicyRuntime(current).decide(q).progress().decision() == "continue");
    q.set_iteration(5);
    check(te::PolicyRuntime(current).decide(q).progress().decision() == "stop");
    q.set_actor_id("other");
    check(te::PolicyRuntime(current).decide(q).progress().decision() == "continue");
    q.set_iteration(6);
    check(te::PolicyRuntime(current).decide(q).progress().decision() == "stop");
  }
  auto duplicate = draft;
  duplicate["loop_rules"].push(draft["loop_rules"].items()[0]);
  try {
    in::apply_control_draft(original.snapshot(), base, settings, duplicate);
    throw std::runtime_error("duplicate actor accepted");
  } catch (const in::Error &error) {
    check(std::string(error.what()) == "control_duplicate_loop_scope");
  }
  auto scoped_base = base;
  scoped_base.mutable_loop_rules(0)->set_max_no_progress(3);
  auto *worker_base = scoped_base.add_loop_rules();
  *worker_base = scoped_base.loop_rules(0);
  worker_base->set_rule_id("worker-base");
  worker_base->set_actor_id("worker");
  worker_base->set_no_progress_decision("stop");
  worker_base->set_max_replans(0);
  worker_base->clear_replan_context();
  const auto unauthorized = in::Json::parse(R"({"action_rules":[],"loop_rules":[
    {"memory_id":"experience-a","actor_id":"worker","replan_on_failure":true,"reason":"另一个分支拥有重规划预算"}]})");
  try {
    in::apply_control_draft(original.snapshot(), scoped_base, settings, unauthorized);
    throw std::runtime_error("cross-actor replan accepted");
  } catch (const in::Error &error) {
    check(std::string(error.what()) == "control_failure_replan_not_authorized");
  }

  in::Json events = in::Json::Array{};
  for (const auto *actor : {"planner", "worker", "other"}) {
    auto q = query(compiled, "loop");
    q.set_actor_id(actor);
    q.set_iteration(3);
    q.set_state_fingerprint("progress");
    q.mutable_advice_query()->set_query_id(actor);
    q.mutable_advice_query()->set_decision_id(actor);
    events.push(in::Json{
        {"kind", "custom"},
        {"attributes", in::Json{{"type", "policy.query"},
                                {"decision", in::json(te::PolicyRuntime(compiled).decide(q))}}}});
  }
  te::dataset::v1::TaskEvaluationReport report;
  *report.mutable_configuration() = in::proto<google::protobuf::Struct>(in::Json{
      {"variants", in::Json::Array{in::Json{{"id", "candidate"}, {"version", "actor-version"}}}}});
  auto *trial = report.add_trials();
  trial->set_variant_id("candidate");
  trial->set_execution_status("completed");
  trial->set_verdict("passed");
  *trial->mutable_evidence() = in::proto<google::protobuf::Struct>(
      in::Json{{"loaded_version", "actor-version"}, {"trace", in::Json{{"events", events}}}});
  const auto coverage =
      in::control_coverage(in::Json{{"level", "L3"}, {"policy", in::json(compiled)}}, report);
  for (const auto &rule : compiled.options().loop_rules())
    if (rule.has_source_experience())
      check(coverage["rules"][rule.rule_id()].num("queries", 0) ==
            (rule.actor_id().empty() ? 3 : 1));
}

// 测试辅助函数：independent_workflow_pairs。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void independent_workflow_pairs() {
  auto p = policy();
  auto options = p.options();
  options.add_host_capabilities("schedule.dispatch");
  auto *rule = options.add_schedule_rules();
  rule->set_rule_id("prepare-image-pair");
  rule->set_target_scope("coordinator");
  rule->set_before_operation("prepare");
  rule->set_after_operation("image");
  rule->set_require_success(true);
  rule->set_reason("Each image requires its own prepared input.");
  *rule->mutable_source_experience() = options.approved_experiences(0);
  p = te::compile_runtime_policy(p.snapshot(), options);
  auto q = query(p, "schedule");
  for (const auto &pair : {std::make_pair("a1", "prepare"),
                           {"b1", "image"},
                           {"a2", "prepare"},
                           {"b2", "image"},
                           {"independent", "lookup"}}) {
    auto *action = q.add_actions();
    action->set_action_id(pair.first);
    action->set_operation(pair.second);
  }
  check(te::plan_schedule(options, q).status() == "unchanged");
  q.mutable_actions(1)->add_after()->set_action_id("a1");
  q.mutable_actions(3)->add_after()->set_action_id("a2");
  const auto plan = te::plan_schedule(options, q);
  check(plan.status() == "scheduled" && plan.actions(1).after_size() == 1 &&
        plan.actions(3).after_size() == 1);
  auto state = te::begin_schedule(plan);
  state = te::advance_schedule(state, "a1", "running");
  state = te::advance_schedule(state, "a2", "running");
  state = te::advance_schedule(state, "a1", "failed");
  state = te::advance_schedule(state, "a2", "succeeded");
  check(state.actions(1).status() == "skipped");
  check(te::ready_schedule(state) == std::vector<std::string>({"b2", "independent"}));
}

// 测试辅助函数：same_operation_orchestration。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void same_operation_orchestration() {
  auto p = policy();
  auto options = p.options();
  options.add_host_capabilities("schedule.dispatch");
  auto *rule = options.add_schedule_rules();
  rule->set_rule_id("one-render-at-a-time");
  rule->set_target_scope("coordinator");
  rule->set_before_operation("render");
  rule->set_after_operation("render");
  rule->set_require_success(false);
  rule->set_reason("同一资源上的调用按提交顺序执行");
  const auto compiled = te::compile_runtime_policy(p.snapshot(), options);
  auto q = query(compiled, "schedule");
  for (const auto &pair :
       {std::make_pair("r1", "render"), {"other", "lookup"}, {"r2", "render"}, {"r3", "render"}}) {
    auto *action = q.add_actions();
    action->set_action_id(pair.first);
    action->set_operation(pair.second);
  }
  auto state = te::begin_schedule(te::PolicyRuntime(compiled).decide(q).schedule());
  check(te::ready_schedule(state) == std::vector<std::string>({"r1", "other"}));
  state = te::advance_schedule(state, "r1", "running");
  state = te::advance_schedule(state, "r1", "failed");
  check(te::ready_schedule(state) == std::vector<std::string>({"other", "r2"}));
  state = te::advance_schedule(state, "r2", "running");
  state = te::advance_schedule(state, "r2", "succeeded");
  check(te::ready_schedule(state) == std::vector<std::string>({"other", "r3"}));
  auto conflict = q;
  conflict.mutable_actions(0)->add_after()->set_action_id("r3");
  check(te::plan_schedule(options, conflict).status() == "conflict");
  options.clear_schedule_rules();
  check(te::ready_schedule(te::begin_schedule(te::plan_schedule(options, q))) ==
        std::vector<std::string>({"r1", "other", "r2", "r3"}));
}

// 测试辅助函数：level_application_coverage。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void level_application_coverage() {
  namespace in = te::integration;
  auto p = policy();
  auto options = p.options();
  options.add_host_capabilities("schedule.dispatch");
  auto *rule = options.add_schedule_rules();
  rule->set_rule_id("order");
  rule->set_target_scope("coordinator");
  rule->set_before_operation("prepare");
  rule->set_after_operation("image");
  rule->set_reason("prepare first");
  p = te::compile_runtime_policy(p.snapshot(), options);
  te::PolicyRuntime runtime(p);
  const in::Json artifact{{"level", "L3"}, {"policy", in::json(p)}};
  te::dataset::v1::TaskEvaluationReport report;
  *report.mutable_configuration() = in::proto<google::protobuf::Struct>(
      in::Json{{"variants",
                in::Json::Array{in::Json{{"id", "candidate"}, {"version", "fixture-version"}}}}});
  auto *trial = report.add_trials();
  trial->set_variant_id("candidate");
  trial->set_execution_status("completed");
  trial->set_verdict("passed");
  in::Json events = in::Json::Array{};
  auto coverage = [&] {
    *trial->mutable_evidence() = in::proto<google::protobuf::Struct>(
        in::Json{{"loaded_version", "fixture-version"}, {"trace", in::Json{{"events", events}}}});
    return in::control_coverage(artifact, report);
  };
  auto result = coverage();
  check(result.flag("complete", false) && !result.flag("level_complete", true));
  auto observe = [&](v1::RuntimeQuery q, const v1::RuntimeObservation &observed) {
    q.mutable_advice_query()->set_query_id(q.boundary());
    q.mutable_advice_query()->set_decision_id(q.boundary());
    const auto decision = runtime.decide(q);
    const auto receipt = runtime.verify(q, observed);
    events.push(in::Json{
        {"kind", "custom"},
        {"attributes", in::Json{{"type", "policy.query"}, {"decision", in::json(decision)}}}});
    events.push(in::Json{{"kind", "advice.applied"},
                         {"application", in::json(receipt.application())},
                         {"attributes", in::Json{{"receipt", in::json(receipt)}}}});
  };
  auto q = query(p);
  q.mutable_advice_query()->set_query_id(q.boundary());
  q.mutable_advice_query()->set_decision_id(q.boundary());
  v1::RuntimeObservation observed;
  observed.set_call_id("model");
  observed.set_transport_allowed(true);
  *observed.mutable_request() = in::proto<google::protobuf::Struct>(in::Json{
      {"messages",
       in::Json::Array{in::Json{{"role", "system"}, {"content", runtime.decide(q).context()}}}}});
  observe(q, observed);
  result = coverage();
  check(result["capabilities"]["llm"].num("applied", 0) == 1 &&
        !result.flag("level_complete", true));
  q = query(p, "schedule");
  for (const auto &name : {"image", "prepare"}) {
    auto *a = q.add_actions();
    a->set_action_id(name);
    a->set_operation(name);
  }
  observed.Clear();
  observe(q, observed); // 未执行计划不能补齐能力证据缺口。
  result = coverage();
  check(result["capabilities"]["schedule"].num("applied", 0) == 0);
  events.items().resize(2);
  for (const auto &pair : {std::make_pair("prepare", "running"),
                           {"prepare", "succeeded"},
                           {"image", "running"},
                           {"image", "succeeded"}}) {
    auto *event = observed.add_schedule_events();
    event->set_action_id(pair.first);
    event->set_status(pair.second);
  }
  observed.add_executed_action_ids("prepare");
  observed.add_executed_action_ids("image");
  observe(q, observed);
  q = query(p, "loop");
  q.set_iteration(7);
  q.set_state_fingerprint("state");
  observed.Clear();
  observed.set_loop_decision("stop");
  observe(q, observed);
  result = coverage();
  check(result.flag("level_complete", false) &&
        result["capabilities"]["schedule"].num("applied", 0) == 1 &&
        result["capabilities"]["loop"].num("applied", 0) == 1);
}

// 测试辅助函数：bounded_recovery_budget。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void bounded_recovery_budget() {
  namespace in = te::integration;
  const auto p = policy();
  auto base = p.options();
  base.set_aggressive_controls(true);
  base.mutable_loop_rules(0)->set_max_no_progress(1);
  const auto settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt"})");
  const auto draft = in::Json::parse(R"({"action_rules":[],"loop_rules":[
    {"memory_id":"experience-a","actor_id":"worker","max_replans":0,"reason":"重规划后仍无进展"}]})");
  const auto original = te::compile_runtime_policy(p.snapshot(), base);
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, original, settings));
  auto q = query(learned, "loop");
  q.set_actor_id("worker");
  q.set_iteration(3);
  q.set_state_fingerprint("changed");
  q.set_failed_actions(1);
  check(te::PolicyRuntime(learned).decide(q).progress().decision() == "continue");
  q.set_repeated_failure_steps(1);
  check(te::PolicyRuntime(learned).decide(q).progress().decision() == "stop");
  q.set_pending_actions(true);
  check(te::PolicyRuntime(learned).decide(q).progress().decision() == "continue");
  q.set_pending_actions(false);
  q.set_actor_id("independent-worker");
  check(te::PolicyRuntime(learned).decide(q).progress().decision() == "replan");
  auto invalid = draft;
  invalid["loop_rules"].items()[0]["max_replans"] = 2;
  bool rejected = false;
  try {
    in::apply_control_draft(p.snapshot(), base, settings, invalid);
  } catch (const in::Error &error) {
    rejected = std::string(error.what()) == "control_loop_relaxes_limit";
  }
  check(rejected);
}

// 测试辅助函数：conditional_recovery_budget。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void conditional_recovery_budget() {
  namespace in = te::integration;
  const auto p = policy();
  auto base = p.options();
  base.set_aggressive_controls(true);
  const auto settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt"})");
  const auto draft = in::Json::parse(R"({"action_rules":[],"loop_rules":[
    {"memory_id":"experience-a","actor_id":"worker","max_replans":0,
     "error_codes":["UnsupportedModel"],"after_action_rejection":true,"reason":"已拒绝的不可恢复错误不再次重规划"}]})");
  const auto original = te::compile_runtime_policy(p.snapshot(), base);
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, original, settings));
  auto q = query(learned, "loop");
  q.set_actor_id("worker");
  q.set_iteration(3);
  q.set_repeated_action_steps(1);
  q.set_state_fingerprint("changed");
  te::PolicyRuntime runtime(learned);
  check(runtime.decide(q).progress().decision() == "replan"); // 没有观测到条件时使用原始预算。
  q.add_error_codes("Timeout");
  q.set_rejected_actions(1);
  check(runtime.decide(q).progress().decision() == "replan");
  q.clear_error_codes();
  q.add_error_codes("UnsupportedModel");
  q.set_rejected_actions(0);
  check(runtime.decide(q).progress().decision() == "replan"); // 条件之间是逻辑与，不是逻辑或。
  q.set_rejected_actions(1);
  check(runtime.decide(q).progress().decision() == "stop");
  q.set_pending_actions(true);
  check(runtime.decide(q).progress().decision() == "continue");
  q.set_pending_actions(false);
  q.set_actor_id("independent-worker");
  check(runtime.decide(q).progress().decision() == "replan");
  q.set_actor_id("worker");
  q.clear_error_codes();
  q.set_rejected_actions(0);
  check(runtime.decide(q).progress().decision() == "replan"); // 不能从说明或任务正文推断旧错误。
  auto rejection_only = draft;
  rejection_only["loop_rules"].items()[0].fields().erase("error_codes");
  const auto rejected_policy = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, rejection_only));
  check(in::generated_controls_authorized(rejected_policy, original, settings));
  q = query(rejected_policy, "loop");
  q.set_actor_id("worker");
  q.set_iteration(2);
  q.set_repeated_action_steps(1);
  q.set_rejected_actions(1);
  check(te::PolicyRuntime(rejected_policy).decide(q).progress().decision() == "stop");
  auto error_only = draft;
  error_only["loop_rules"].items()[0]["after_action_rejection"] = false;
  const auto error_policy = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, error_only));
  check(in::generated_controls_authorized(error_policy, original, settings));
  q = query(error_policy, "loop");
  q.set_actor_id("worker");
  q.set_iteration(2);
  q.set_repeated_action_steps(1);
  q.add_error_codes("UnsupportedModel");
  check(te::PolicyRuntime(error_policy).decide(q).progress().decision() == "stop");
  auto duplicate = draft;
  duplicate["loop_rules"].items()[0]["error_codes"] =
      in::Json::Array{"UnsupportedModel", "UnsupportedModel"};
  bool bad = false;
  try {
    in::apply_control_draft(p.snapshot(), base, settings, duplicate);
  } catch (const in::Error &) {
    bad = true;
  }
  check(bad);
  auto conditional_base = base;
  conditional_base.mutable_loop_rules(0)->add_error_codes("UnsupportedModel");
  conditional_base.mutable_loop_rules(0)->set_after_action_rejection(true);
  for (auto relaxed : {rejection_only, error_only}) {
    relaxed["loop_rules"].items()[0]["error_codes"] = in::Json::Array{};
    bad = false;
    try {
      in::apply_control_draft(p.snapshot(), conditional_base, settings, relaxed);
    } catch (const in::Error &) {
      bad = true;
    }
    check(bad);
  }
  // 原生查询同样有独立边界；合法重复码不必被强制视为无效。
  auto bounded = query(original);
  bounded.set_rejected_actions(10000);
  for (int i = 0; i < 64; ++i)
    bounded.add_error_codes("UnsupportedModel");
  check(te::PolicyRuntime(original).decide(bounded).status() == "ready");
  bounded.add_error_codes("UnsupportedModel");
  rejects([&] { te::PolicyRuntime(original).decide(bounded); });
  bounded.clear_error_codes();
  bounded.set_rejected_actions(10001);
  rejects([&] { te::PolicyRuntime(original).decide(bounded); });
  bounded.set_rejected_actions(0);
  for (const auto &code :
       {std::string{}, std::string(129, 'x'), std::string("bad\ncode"), std::string("x\0y", 3)}) {
    bounded.clear_error_codes();
    bounded.add_error_codes(code);
    rejects([&] { te::PolicyRuntime(original).decide(bounded); });
  }
}

// 测试辅助函数：finalization_condition。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void finalization_condition() {
  namespace in = te::integration;
  const auto p = policy();
  auto base = p.options();
  base.set_aggressive_controls(true);
  const auto settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt"})");
  const auto draft = in::Json::parse(R"({"action_rules":[],"loop_rules":[
    {"memory_id":"experience-a","actor_id":"worker","max_iterations":2,
     "require_finalization":true,"reason":"跳过重复执行后仍保留最终答复"}]})");
  const auto original = te::compile_runtime_policy(p.snapshot(), base);
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(learned.options().loop_rules(1).require_finalization());
  check(in::generated_controls_authorized(learned, original, settings));
  const auto exported = in::proto<v1::RuntimePolicy>(in::json(learned));
  check(exported.options().loop_rules(1).require_finalization() &&
        exported.policy_id() == learned.policy_id());
  auto q = query(learned, "loop");
  q.set_actor_id("worker");
  q.set_iteration(3);
  q.set_state_fingerprint("new-state");
  te::PolicyRuntime runtime(learned);
  check(runtime.decide(q).progress().decision() == "continue"); // 宿主未提供该能力时保留原预算。
  q.set_can_finalize(true);
  check(runtime.decide(q).progress().decision() == "stop");
  q.set_pending_actions(true);
  check(runtime.decide(q).progress().decision() == "continue");
  q.set_pending_actions(false);
  q.set_actor_id("independent-worker");
  check(runtime.decide(q).progress().decision() == "continue");
  q.set_actor_id("worker");
  q.set_can_finalize(false);
  q.set_iteration(7);
  check(runtime.decide(q).progress().decision() == "stop"); // 原始上限仍然有效。
  // 保守模式保持关闭，但允许限定 actor 且强制收尾的候选；重载后仍可执行。
  auto conservative = base;
  conservative.set_aggressive_controls(false);
  const auto conservative_policy = te::compile_runtime_policy(p.snapshot(), conservative);
  const auto conditional = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), conservative, settings, draft));
  check(!conditional.options().aggressive_controls());
  check(in::generated_controls_authorized(conditional, conservative_policy, settings));
  q = query(conditional, "loop");
  q.set_actor_id("worker");
  q.set_iteration(3);
  q.set_state_fingerprint("new-state");
  check(te::PolicyRuntime(conditional).decide(q).progress().decision() == "continue");
  q.set_can_finalize(true);
  check(te::PolicyRuntime(conditional).decide(q).progress().decision() == "stop");
  q.set_pending_actions(true);
  check(te::PolicyRuntime(conditional).decide(q).progress().decision() == "continue");
  for (const auto *missing : {"actor_id", "require_finalization"}) {
    auto invalid = draft;
    invalid["loop_rules"].items()[0].fields().erase(missing);
    bool rejected = false;
    try {
      in::apply_control_draft(p.snapshot(), conservative, settings, invalid);
    } catch (const in::Error &error) {
      rejected = std::string(error.what()) == "control_loop_semantic_scope_required";
    }
    check(rejected);
  }

  auto legacy = draft;
  legacy["loop_rules"].items()[0].fields().erase("require_finalization");
  const auto old = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, legacy));
  q = query(old, "loop");
  q.set_actor_id("worker");
  q.set_iteration(3);
  check(te::PolicyRuntime(old).decide(q).progress().decision() == "stop");
  check(in::generated_controls_authorized(old, original, settings));
  auto conditional_base = base;
  conditional_base.mutable_loop_rules(0)->set_require_finalization(true);
  const auto inherited = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), conditional_base, settings, legacy));
  check(inherited.options().loop_rules(1).require_finalization());
  check(in::generated_controls_authorized(
      inherited, te::compile_runtime_policy(p.snapshot(), conditional_base), settings));
  auto relaxed = draft;
  relaxed["loop_rules"].items()[0]["require_finalization"] = false;
  bool bad = false;
  try {
    in::apply_control_draft(p.snapshot(), conditional_base, settings, relaxed);
  } catch (const in::Error &error) {
    bad = std::string(error.what()) == "control_loop_relaxes_condition";
  }
  check(bad);
}

// 测试辅助函数：target_retry_rule。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void target_retry_rule() {
  namespace in = te::integration;
  const auto p = policy();
  auto base = p.options();
  base.clear_action_rules();
  const auto settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt"})");
  const auto draft = in::Json::parse(R"({"action_rules":[{"memory_id":"experience-a",
    "primitive_id":"repeat_failed_target","operation":"generate","failure_key_arguments":["output"],
    "max_failed_attempts":2,"reason":"同一目标失败后停止重派发"}],"loop_rules":[]})");
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, te::compile_runtime_policy(p.snapshot(), base),
                                          settings));
  const auto &rule = learned.options().action_rules(0);
  check(rule.failure_key_arguments_size() == 1 && rule.failure_key_arguments(0) == "output");
  auto q = query(learned, "actions");
  auto *action = q.add_actions();
  action->set_action_id("next-execution");
  action->set_operation("generate");
  action->set_prior_failures(50);
  check(te::PolicyRuntime(learned).decide(q).actions(0).decision() == "allow");
  (*action->mutable_target_failures())[rule.rule_id()] = 2;
  check(te::PolicyRuntime(learned).decide(q).actions(0).decision() == "reject");
  action->set_operation("independent");
  check(te::PolicyRuntime(learned).decide(q).actions(0).decision() == "allow");
  auto invalid = draft;
  invalid["action_rules"].items()[0]["failure_key_arguments"] = in::Json::Array{"output", "output"};
  bool rejected = false;
  try {
    in::apply_control_draft(p.snapshot(), base, settings, invalid);
  } catch (const in::Error &) {
    rejected = true;
  }
  check(rejected);
}

// 测试辅助函数：schedule_execution_contract。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void schedule_execution_contract() {
  namespace in = te::integration;
  using in::Json;
  const auto constraints = Json::parse(R"({"dependency":"A_before_B","stop_on_failure":true})");
  auto evidence = Json::parse(R"({"trace":{"events":[
    {"step_id":"batch","attributes":{"type":"policy.query","decision":{"query":{"boundary":"schedule"},
      "schedule":{"actions":[{"action_id":"A"},{"action_id":"B","after":[{"action_id":"A"}]}]}}}},
    {"step_id":"batch","kind":"action.end","status":"failed","attributes":{"type":"schedule.action","logical_action_id":"A","executed":true}},
    {"step_id":"batch","kind":"action.start","attributes":{"type":"schedule.action","logical_action_id":"B","executed":true}}
  ]}})");
  check(in::schedule_contract_check(constraints, evidence).str("status") == "failed");
  check(in::schedule_contract_check(Json::Object{}, evidence).null());
  check(in::schedule_contract_check(
            Json{{"dependency", Json::Array{"A", "B"}}, {"stop_on_failure", true}}, evidence)
            .null());
  check(in::schedule_contract_check(Json{{"dependency", "A_before_B"}, {"stop_on_failure", "true"}},
                                    evidence)
            .null());
  auto independent = constraints;
  independent["dependency"] = "independent";
  check(in::schedule_contract_check(independent, evidence).null());
  auto &events = evidence["trace"]["events"].items();
  events[2]["attributes"]["logical_action_id"] = "C";
  check(in::schedule_contract_check(constraints, evidence).str("status") == "passed");
  events[2]["attributes"]["logical_action_id"] = "B";
  events[2]["attributes"]["executed"] = false;
  check(in::schedule_contract_check(constraints, evidence).str("status") == "passed");
  events[2]["attributes"]["executed"] = true;
  events[1]["status"] = "completed";
  check(in::schedule_contract_check(constraints, evidence).str("status") == "passed");
  events[1]["attributes"]["observed_failure"] = true;
  events[0]["attributes"]["decision"]["schedule"]["actions"]
      .items()[1]["after"]
      .items()[0]["require_success"] = true;
  check(in::schedule_contract_check(Json::Object{}, evidence).str("status") == "failed");
  std::reverse(events.begin(), events.end());
  check(in::schedule_contract_check(constraints, evidence).str("status") == "failed");
  Json multi{{"traces", Json::Array{evidence["trace"], Json{{"events", Json::Array{}}}}}};
  check(in::schedule_contract_check(constraints, multi).str("status") == "failed");
}

// 测试辅助函数：scoped_control_roundtrip。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void scoped_control_roundtrip() {
  namespace in = te::integration;
  using in::Json;
  const auto p = policy();
  auto base = p.options();
  base.clear_action_rules();
  base.add_host_capabilities("schedule.dispatch");
  const Json settings{{"enabled", true}, {"prompt_file", "test.txt"}};
  const auto draft = Json::parse(R"({"action_rules":[
    {"memory_id":"experience-a","primitive_id":"terminal_failure","operation":"render",
     "terminal_error_codes":["UnsupportedModel"],"error_scope_arguments":["model"],"reason":"配置故障不重试"},
    {"memory_id":"experience-a","primitive_id":"operation_budget","operation":"verify",
     "max_attempts":2,"reason":"重复检查预算"}],"loop_rules":[],"schedule_rules":[
    {"memory_id":"orchestration-z","before_operation":"prepare","after_operation":"render",
     "before_argument":"output","after_argument":"input","require_success":true,"reason":"同一资源先准备再消费"}]})");
  const auto learned = te::compile_runtime_policy(
      p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, te::compile_runtime_policy(p.snapshot(), base),
                                          settings));
  const auto exported = in::proto<v1::RuntimePolicy>(in::Json::parse(in::json(learned).dump()));
  auto q = query(exported, "actions");
  auto *a = q.add_actions();
  a->set_action_id("verification");
  a->set_operation("verify");
  (*a->mutable_target_failures())[exported.options().action_rules(1).rule_id()] = 2;
  auto *b = q.add_actions();
  b->set_action_id("other");
  b->set_operation("search");
  const auto decision = te::PolicyRuntime(exported).decide(q);
  check(decision.actions(0).decision() == "reject" && decision.actions(1).decision() == "allow");
  auto schedule = query(exported, "schedule");
  schedule.set_execution_id("batch");
  schedule.mutable_advice_query()->set_task("Prepare two independent images / 准备两个独立图片");
  for (const auto *resource : {"a.png", "b.png"}) {
    auto *producer = schedule.add_actions();
    producer->set_action_id(std::string("prepare-") + resource);
    producer->set_operation("prepare");
    (*producer->mutable_arguments()->mutable_fields())["output"].set_string_value(resource);
    auto *consumer = schedule.add_actions();
    consumer->set_action_id(std::string("render-") + resource);
    consumer->set_operation("render");
    (*consumer->mutable_arguments()->mutable_fields())["input"].set_string_value(resource);
  }
  const auto plan = te::plan_schedule(exported.options(), schedule);
  check(plan.status() == "scheduled" && plan.actions(1).after_size() == 1 &&
        plan.actions(3).after_size() == 1);
  check(plan.actions(1).after(0).action_id() == "prepare-a.png" &&
        plan.actions(3).after(0).action_id() == "prepare-b.png");
  auto state = te::begin_schedule(plan);
  state = te::advance_schedule(state, "prepare-a.png", "running");
  state = te::advance_schedule(state, "prepare-a.png", "failed");
  const auto ready = te::ready_schedule(state);
  check(ready.size() == 1 && ready[0] == "prepare-b.png");
  schedule.mutable_actions(3)->clear_arguments();
  check(te::plan_schedule(exported.options(), schedule).actions(3).after_size() == 0);
  (*schedule.mutable_actions(0)->mutable_arguments()->mutable_fields())["output"].set_string_value(
      "<redacted:resource>");
  (*schedule.mutable_actions(1)->mutable_arguments()->mutable_fields())["input"].set_string_value(
      "<redacted:resource>");
  check(te::plan_schedule(exported.options(), schedule).actions(1).after_size() == 0);
  auto invalid = draft;
  invalid["action_rules"].items()[0]["required_arguments"] = Json::Array{"secret"};
  bool rejected = false;
  try {
    in::apply_control_draft(p.snapshot(), base, settings, invalid);
  } catch (const in::Error &) {
    rejected = true;
  }
  check(rejected);
}

// 收尾模板属于被评测的版本：独立导入不需要部署模板，不同运行不能互相覆盖。
void portable_finalization_context() {
  namespace in = te::integration;
  using in::Json;
  const auto original = policy();
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1);
  memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1");
  memory.set_revision(1);
  auto limits = original.snapshot().options();
  limits.clear_target_scopes();
  limits.add_target_scopes("agent:agent-a/actor:coordinator");
  auto options = original.options();
  options.clear_approved_experiences();
  options.clear_action_rules();
  options.clear_loop_rules();
  auto *loop = options.add_loop_rules();
  loop->set_rule_id("finalization-only");
  loop->set_target_scope("agent:agent-a/actor:coordinator");
  loop->set_max_iterations(1);
  loop->set_require_finalization(true);
  loop->set_no_progress_decision("stop");
  loop->set_reason("停止重复执行后保留最终答复");
  const auto p = te::compile_runtime_policy(
      te::compile_shadow_policy(memory, original.snapshot().identity(), limits), options);
  const Json legacy{{"schema_version", 1}, {"agent", Json{{"revision", "portable-test"}}},
                    {"user_id", "user-a"}, {"level", "L3"}, {"policy", in::json(p)}};
  auto frozen = legacy;
  frozen["runtime_options"] = Json{{"finalization_context", "仅汇总已确认的执行结果。"}};
  const auto version = te::agent_version_id(frozen.dump());
  check(version != te::agent_version_id(legacy.dump()));
  auto empty = frozen;
  empty["runtime_options"]["finalization_context"] = "";
  check(version != te::agent_version_id(empty.dump()));
  for (const auto &invalid :
       std::vector<Json>{Json(nullptr), Json::Array{}, Json{{"finalization_context", 3}},
        Json{{"finalization_context", std::string(16385, 'x')}},
        Json{{"finalization_context", "ok"}, {"endpoint", "not-exportable"}}, Json::Object{}}) {
    auto bad = frozen;
    bad["runtime_options"] = invalid;
    bool rejected = false;
    try {
      te::agent_version_id(bad.dump());
    } catch (const in::Error &) {
      rejected = true;
    }
    check(rejected);
  }
  // 传输只保留版本本身；没有原始配置文件、记忆或模板文件也能绑定同一身份。
  const auto imported = Json::parse(frozen.dump());
  const Json deployment{{"offline_version", imported}, {"runtime_options", Json::Object{}},
                        {"actors", Json::Array{"coordinator"}}, {"agent_id", "agent-a"},
                        {"finalization_context", "另一部署的模板不应覆盖版本"}};
  auto bind = [&](const Json &artifact, const char *id) {
    auto config = deployment;
    config["offline_version"] = artifact;
    auto identity = in::json(p.snapshot().identity());
    identity["run_id"] = id;
    Json run{{"identity", identity}, {"level", 3}, {"input", "汇总执行结果"},
             {"capabilities", Json::Array{"model", "actions", "loop"}},
             {"events", Json::Array{Json{{"event_id", "begin"}}}}};
    in::initialize_boundary_run(run, Json::Object{});
    in::apply_agent_version(run, config, Json::Object{}, nullptr,
                            {"agent:agent-a/actor:coordinator"});
    return run;
  };
  auto first = std::async(std::launch::async, [&] { return bind(imported, "portable-a"); });
  auto second = std::async(std::launch::async, [&] { return bind(empty, "portable-b"); });
  auto run = first.get(), no_template = second.get(), old_run = bind(legacy, "portable-old");
  check(run.str("agent_version_id") == version &&
        run.str("finalization_context") == imported["runtime_options"].str("finalization_context"));
  check(no_template.has("finalization_context") && no_template.str("finalization_context").empty());
  check(!old_run.has("finalization_context") &&
        deployment.str("finalization_context") == "另一部署的模板不应覆盖版本");
  const Json request{{"operation", "loop.check"}, {"request_id", "portable-check"},
                     {"loop_id", "loop"}, {"state", Json::Object{}},
                     {"extensions", Json{{"finalize_on_stop", true}}}};
  // 实际边界必须把冻结空模板转成 can_finalize=false，不能因部署模板存在而放行。
  const auto ready = in::dispatch_boundary(run, request, deployment);
  const auto absent = in::dispatch_boundary(no_template, request, deployment);
  check(run["controls"][ready.str("control_id")]["query"].flag("can_finalize", false));
  check(!no_template["controls"][absent.str("control_id")]["query"].flag("can_finalize", false));
  auto old_config = deployment;
  old_config.fields().erase("finalization_context");
  const auto old_absent = in::dispatch_boundary(old_run, request, old_config);
  check(!old_run["controls"][old_absent.str("control_id")]["query"].flag("can_finalize", false));
  // 使用真实 activity.begin 收尾路径，确认注入的是版本模板，而非另一部署的文本。
  auto &control = run["controls"][ready.str("control_id")];
  control["committed"] = true;
  control["actual_decision"] = "stop";
  control["commit_event"] = control["start_event"];
  run["loops"]["loop"]["stopped"] = true;
  run["loops"]["loop"]["finalizing"] = true;
  const auto answer = in::dispatch_boundary(
      run, Json{{"operation", "activity.begin"}, {"request_id", "portable-answer"},
                {"kind", "model"}, {"name", "answer"}, {"input", "原问题"},
                {"control_id", ready["control_id"]},
                {"binding", Json{{"pointer", ""}, {"mode", "text"}}}}, deployment);
  check(answer.dump().find(imported["runtime_options"].str("finalization_context")) !=
        std::string::npos);
  check(answer.dump().find(deployment.str("finalization_context")) == std::string::npos);
}

// 回归用例：main。
// 重点核验字段或诊断：scoped_control_roundtrip、injection_and_evidence、disabled_injection_keeps_control_approval；异常路径不能被当成成功。
int main() {
  const std::vector<std::pair<const char *, void (*)()>> cases = {
      {"scoped_control_roundtrip", scoped_control_roundtrip},
      {"injection_and_evidence", injection_and_evidence},
      {"interleaved_injection_receipts", interleaved_injection_receipts},
      {"evolvable_applicability", evolvable_applicability},
      {"explicit_experience_allowlist", explicit_experience_allowlist},
      {"explicit_allowlist_reaches_real_model_boundary", explicit_allowlist_reaches_real_model_boundary},
      {"disabled_injection_keeps_control_approval", disabled_injection_keeps_control_approval},
      {"independent_actions", independent_actions},
      {"loop_controls", loop_controls},
      {"generated_controls", generated_controls},
      {"failure_replanning", failure_replanning},
      {"source_bound_auth_replanning", source_bound_auth_replanning},
      {"bounded_recovery_budget", bounded_recovery_budget},
      {"conditional_recovery_budget", conditional_recovery_budget},
      {"finalization_condition", finalization_condition},
      {"portable_finalization_context", portable_finalization_context},
      {"target_retry_rule", target_retry_rule},
      {"schedule_execution_contract", schedule_execution_contract},
      {"skill_orchestration", skill_orchestration},
      {"independent_workflow_pairs", independent_workflow_pairs},
      {"same_operation_orchestration", same_operation_orchestration},
      {"level_application_coverage", level_application_coverage},
      {"aggressive_controls", aggressive_controls},
      {"actor_scoped_loops", actor_scoped_loops}};
  try {
    for (const auto &test : cases) {
      try {
        test.second();
      } catch (const std::exception &error) {
        throw std::runtime_error(std::string(test.first) + ": " + error.what());
      }
    }
  } catch (const std::exception &exc) {
    std::cerr << exc.what() << '\n';
    return 1;
  }
  std::cout << "L1-L3 policy boundaries passed\n";
}
