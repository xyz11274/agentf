#include "trajectory_evolution/evolution.h"
#include "agentflow/trajectory/experience.h"
#include "../src/control_generation.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <google/protobuf/util/json_util.h>

namespace te = trajectory::evolution;
namespace v1 = te::v1;
namespace ep = agentflow::trajectory::evaluation;

void check(bool value) { if(!value) throw std::runtime_error("policy assertion failed"); }
template <class F> void rejects(F fn) {
  try { fn(); } catch(const agentflow::trajectory::EvaluationError&) { return; }
  throw std::runtime_error("expected policy rejection");
}

v1::RuntimePolicy policy() {
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1); memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1"); memory.set_revision(1);
  auto* entry = memory.add_entries();
  entry->set_memory_id("experience-a"); entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto* candidate = entry->mutable_candidate();
  candidate->set_schema_version(1); candidate->set_candidate_id("candidate-a");
  candidate->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  candidate->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  candidate->set_target_scope("coordinator"); candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  candidate->set_trigger_pattern("视频生成缺少素材"); candidate->set_observed_problem("缺少图片仍重试");
  candidate->set_proposed_change("请先补充图片素材，独立检索任务继续。");
  candidate->set_support_user_count(1);
  auto* evidence = candidate->add_support_evidence(); evidence->set_evidence_id("evidence-a");
  evidence->mutable_source()->set_user_id("user-a");
  evidence->mutable_source()->set_dialogue_id("source-a");
  v1::Identity identity;
  identity.set_agent_id("agent-a"); identity.set_user_id("user-a");
  identity.set_conversation_id("conv-a"); identity.set_run_id("run-a");
  v1::ShadowOptions limits;
  limits.add_target_scopes("coordinator"); limits.add_target_scopes("skill:worker");
  limits.set_max_policy_entries(64); limits.set_max_advice_items(3); limits.set_max_advice_bytes(4096);
  limits.set_max_query_bytes(16384); limits.set_max_memory_bytes(1048576); limits.set_max_snapshot_bytes(262144);
  limits.set_max_match_terms(128); limits.set_min_matched_terms(2);
  auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  check(snapshot.experiences_size() == 1);
  v1::RuntimeOptions options;
  options.set_requested_level(v1::L3); options.set_fallback("strict");
  for(auto cap : {"context.request", "action.filter", "loop.decide"}) options.add_host_capabilities(cap);
  options.add_allowed_users("user-a"); options.add_allowed_conversations("conv-a");
  options.set_experience_template("授权经验：\n{{experience}}"); options.set_max_context_bytes(4096);
  auto* approval = options.add_approved_experiences();
  approval->set_memory_id(snapshot.experiences(0).memory_id());
  approval->set_content_revision(snapshot.experiences(0).content_revision());
  auto* action = options.add_action_rules();
  action->set_rule_id("image-required"); action->set_target_scope("coordinator");
  action->set_operation("video"); action->add_required_arguments("image"); action->set_reason("缺少图片素材");
  auto* loop = options.add_loop_rules(); loop->set_rule_id("bounded-loop");
  loop->set_target_scope("coordinator"); loop->set_max_iterations(6); loop->set_max_no_progress(1);
  loop->set_no_progress_decision("replan"); loop->set_max_replans(1);
  loop->set_replan_context("停止重复操作，选择其他可行方法。"); loop->set_reason("无进展或达到授权轮数");
  return te::compile_runtime_policy(snapshot, options);
}

v1::RuntimeQuery query(const v1::RuntimePolicy& policy, const char* boundary = "llm") {
  v1::RuntimeQuery query;
  auto* advice = query.mutable_advice_query(); *advice->mutable_identity() = policy.snapshot().identity();
  advice->set_query_id("query-a"); advice->set_decision_id("decision-a");
  advice->set_target_scope("coordinator"); advice->set_task("视频生成缺少图片素材");
  query.set_execution_id("coordinator"); query.set_boundary(boundary);
  return query;
}

void injection_and_evidence() {
  auto p = policy(); te::PolicyRuntime runtime(p); auto q = query(p);
  auto result = runtime.decide(q); check(result.status() == "ready" && result.experiences_size() == 1);
  for(int i = 0; i < 32; ++i) {
    if(runtime.decide(q).context() != result.context()) {
      throw std::runtime_error("experience context serialization is not deterministic");
    }
  }
  check(result.context().find("图片素材") != std::string::npos);
  check(result.context().find("\"trigger_pattern\":\"视频生成缺少素材\"") != std::string::npos);
  check(result.context().find("\"target_scope\":\"coordinator\"") != std::string::npos);
  check(result.context().find("缺少图片仍重试") == std::string::npos);
  std::vector<std::future<void>> injections;
  for(int i = 0; i < 24; ++i) injections.push_back(std::async(std::launch::async, [&] {
    check(runtime.decide(q).context() == result.context());
  }));
  for(auto& injection : injections) injection.get();
  auto bounded_options = p.options();
  bounded_options.set_experience_template(std::string(300, 'x') + "{{experience}}");
  bounded_options.set_max_context_bytes(512);
  const auto bounded = te::PolicyRuntime(te::compile_runtime_policy(p.snapshot(), bounded_options)).decide(q);
  check(bounded.status() == "no_change" && bounded.context().empty() && bounded.experiences().empty());
  auto legacy = p;
  legacy.set_policy_id("runtime-v1:" + p.policy_id().substr(std::string("runtime-v2:").size()));
  rejects([&] { te::PolicyRuntime incompatible(legacy); });
  auto old_snapshot = p.snapshot();
  old_snapshot.set_schema_version(1);
  try {
    te::compile_runtime_policy(old_snapshot, p.options());
    throw std::runtime_error("legacy snapshot was promoted into an active policy");
  } catch(const agentflow::trajectory::EvaluationError& exc) {
    check(std::string(exc.what()) == "runtime_experience_contract_missing");
  }
  v1::RuntimeObservation observed;
  observed.set_transport_allowed(true); observed.set_call_id("actual-call");
  auto* message = (*observed.mutable_request()->mutable_fields())["messages"].mutable_list_value()->add_values()->mutable_struct_value();
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
  condition_removed.erase(condition_removed.find("视频生成缺少素材"), std::string("视频生成缺少素材").size());
  (*message->mutable_fields())["content"].set_string_value(condition_removed);
  check(runtime.verify(q, observed).application().status() == "failed");
  auto options = p.options(); options.mutable_approved_experiences(0)->set_content_revision("wrong-version");
  check(te::PolicyRuntime(te::compile_runtime_policy(p.snapshot(), options)).decide(q).status() == "no_change");
  q.mutable_advice_query()->mutable_identity()->set_user_id("another-user");
  rejects([&] { runtime.decide(q); });
  options = p.options(); options.clear_host_capabilities(); options.add_host_capabilities("context.request");
  rejects([&] { te::compile_runtime_policy(p.snapshot(), options); });
  options.set_fallback("downgrade");
  check(te::compile_runtime_policy(p.snapshot(), options).effective_level() == v1::L1);
  p.mutable_options()->set_max_context_bytes(1024);
  rejects([&] { te::PolicyRuntime altered(p); });
}

void independent_actions() {
  auto p = policy(); te::PolicyRuntime runtime(p); auto q = query(p, "actions");
  for(auto id : {"video-bad", "video-ok", "independent"}) {
    auto* action = q.add_actions(); action->set_action_id(id);
    action->set_operation(std::string(id) == "independent" ? "search" : "video");
    if(std::string(id) == "video-ok") (*action->mutable_arguments()->mutable_fields())["image"].set_string_value("image.png");
  }
  auto result = runtime.decide(q);
  check(result.actions_size() == 3 && result.actions(0).decision() == "reject"
        && result.actions(1).action_id() == "video-ok" && result.actions(1).decision() == "allow"
        && result.actions(2).decision() == "allow");
  v1::RuntimeObservation observed;
  observed.add_skipped_action_ids("video-bad"); observed.add_executed_action_ids("independent");
  observed.add_executed_action_ids("video-ok");
  check(runtime.verify(q, observed).application().status() == "applied");
  observed.add_executed_action_ids("video-bad");
  check(runtime.verify(q, observed).application().status() == "failed");
  *q.add_actions() = q.actions(0);
  rejects([&] { runtime.decide(q); });
}

void loop_controls() {
  auto p = policy(); te::PolicyRuntime runtime(p); auto q = query(p, "loop");
  q.set_iteration(1); q.set_state_fingerprint("unchanged-state");
  auto first = runtime.decide(q); check(first.progress().decision() == "continue");
  *q.mutable_previous() = first.progress();
  q.mutable_previous()->set_policy_id("different-run-policy");
  rejects([&] { runtime.decide(q); });
  *q.mutable_previous() = first.progress(); q.set_iteration(2);
  auto replan = runtime.decide(q); check(replan.progress().decision() == "replan");
  check(!replan.context().empty() && replan.progress().replans() == 1);
  v1::RuntimeObservation observed; observed.set_loop_decision("replan");
  check(runtime.verify(q, observed).application().status() == "failed"); // queued is not applied
  *q.mutable_previous() = replan.progress();
  check(runtime.decide(q).progress().replans() == 1); // duplicate iteration is idempotent
  q.set_iteration(3); auto stopped = runtime.decide(q); check(stopped.progress().decision() == "stop");
  observed.set_loop_decision("stop");
  check(runtime.verify(q, observed).application().status() == "applied");
  q.set_pending_actions(true); check(runtime.decide(q).progress().decision() == "continue");
  check(runtime.verify(q, observed).application().status() != "applied");
  q.set_pending_actions(false); q.set_execution_id("parallel-other");
  rejects([&] { runtime.decide(q); });
  q.clear_previous(); q.set_state_fingerprint(""); q.set_iteration(6);
  check(runtime.decide(q).progress().decision() == "continue");
  q.set_iteration(7); check(runtime.decide(q).progress().decision() == "stop");
  std::vector<std::future<void>> tasks;
  for(int i = 0; i < 24; ++i) tasks.push_back(std::async(std::launch::async, [&, i] {
    auto independent = query(p, "loop"); independent.set_iteration(1);
    independent.set_execution_id("branch-" + std::to_string(i));
    check(runtime.decide(independent).progress().decision() == "continue");
  }));
  for(auto& task : tasks) task.get();
}

void generated_controls() {
  namespace in = te::integration;
  const auto p = policy();
  auto base = p.options(); base.clear_action_rules(); base.mutable_loop_rules(0)->set_max_no_progress(3);
  const auto settings = in::Json::parse(R"({"enabled":true,"prompt_file":"test.txt","actions":{"video":["image"]}})");
  const auto draft = in::Json::parse(R"({"action_rules":[{"memory_id":"experience-a","operation":"video","required_arguments":["image"],"reason":"先确认素材"}],"loop_rules":[{"memory_id":"experience-a","max_iterations":3,"reason":"限制重复"}]})");
  const auto learned = te::compile_runtime_policy(p.snapshot(), in::apply_control_draft(p.snapshot(), base, settings, draft));
  check(in::generated_controls_authorized(learned, te::compile_runtime_policy(p.snapshot(), base), settings));
  auto q = query(learned, "actions");
  auto* action = q.add_actions(); action->set_action_id("bad"); action->set_operation("video");
  auto* sibling = q.add_actions(); sibling->set_action_id("good"); sibling->set_operation("search");
  const auto matched = te::PolicyRuntime(learned).decide(q);
  check(matched.actions(0).decision() == "reject" && matched.actions(1).decision() == "allow");
  check(matched.experiences_size() == 1);
  q.mutable_advice_query()->set_task("完全无关的算术问题");
  check(te::PolicyRuntime(learned).decide(q).actions(0).decision() == "allow");
  auto invalid = draft; invalid["loop_rules"].items()[0]["max_iterations"] = 100;
  bool rejected = false;
  try { in::apply_control_draft(p.snapshot(), base, settings, invalid); } catch(...) { rejected = true; }
  check(rejected);
  invalid = draft; invalid["action_rules"].items()[0]["operation"] = "shell"; rejected = false;
  try { in::apply_control_draft(p.snapshot(), base, settings, invalid); } catch(...) { rejected = true; }
  check(rejected);
  auto altered = learned; altered.mutable_options()->mutable_loop_rules(1)->set_max_iterations(100);
  check(!in::generated_controls_authorized(altered, p, settings));
}

int main() {
  try { injection_and_evidence(); independent_actions(); loop_controls(); generated_controls(); }
  catch(const std::exception& exc) { std::cerr << exc.what() << '\n'; return 1; }
  std::cout << "L1-L3 policy boundaries passed\n";
}
