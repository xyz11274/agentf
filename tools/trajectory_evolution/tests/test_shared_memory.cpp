// test_shared_memory 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

#include "shared_memory.h"
#include "gepa_feedback.h"
#include "trajectory_evolution/evolution.h"
#include "agentflow/trajectory/experience.h"
#include "../src/analysis/learning_evidence.h"
#include <iostream>
#include <future>
#include <atomic>

namespace te = trajectory::evolution;
namespace v1 = te::v1;
namespace ep = agentflow::trajectory::evaluation;
using namespace te::integration;

// 测试辅助函数：check。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void check(bool v, const char *label) {
  if (!v)
    throw std::runtime_error(label);
}

const std::string scope = "agent:test-app/actor:default";
const std::string condition = "生成视频缺少图片素材";
const std::string advice = "缺少图片素材时先请求补充，独立检索继续。";

// 测试辅助函数：memory。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
ep::EvolutionMemoryStore memory(const std::string &user, const std::string &text = advice) {
  ep::EvolutionMemoryStore store;
  store.set_schema_version(1);
  store.set_store_kind("trajectory_evolution_memory");
  store.set_store_version("evolution-memory-store-v1");
  auto *e = store.add_entries();
  e->set_memory_id("private-" + user);
  e->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto *c = e->mutable_candidate();
  c->set_schema_version(1);
  c->set_candidate_id("candidate-" + user);
  c->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  c->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  c->set_target_scope(scope);
  c->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  c->set_trigger_pattern(condition);
  c->set_proposed_change(text);
  c->set_observed_problem("这段私人历史不可进入公共库");
  c->set_support_user_count(1);
  c->add_support_evidence()->mutable_source()->set_user_id(user);
  return store;
}

// 提取协议身份字段，保留调用与运行之间的关联。
v1::Identity identity(const std::string &user = "alice") {
  v1::Identity id;
  id.set_agent_id("test-app");
  id.set_user_id(user);
  id.set_conversation_id("conv");
  id.set_run_id("run");
  return id;
}

// 测试辅助函数：config。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
Json config() {
  return Json{{"agent_id", "test-app"},
              {"actors", Json::Array{"default"}},
              {"sharing",
               shared_memory_options(
                   Json{{"enabled", true}, {"min_interval_ms", 10}, {"min_supporting_users", 2}})},
              {"shadow_options", Json{{"max_policy_entries", 64},
                                      {"max_advice_items", 8},
                                      {"max_advice_bytes", 8192},
                                      {"max_query_bytes", 16384},
                                      {"max_memory_bytes", 1048576},
                                      {"max_snapshot_bytes", 262144},
                                      {"max_match_terms", 128},
                                      {"min_matched_terms", 2},
                                      {"target_scopes", Json::Array{scope}}}},
              {"shared_distill_prompt", "DISTILL"},
              {"shared_review_prompt", "REVIEW"},
              {"shared_relation_prompt", "RELATION"}};
}

struct Model : agentflow::trajectory::ModelAdapter {
  std::atomic<unsigned> calls{0}, batch_calls{0};
  std::string relation = "related", draft_text = advice;
  bool fail = false, safe = true, new_entry = false, fenced = false;
  std::string error_stage;

  // 通过本模块的评测入口处理请求，执行与采集生命周期由对应契约约束。
  agentflow::trajectory::RawModelResponse
  evaluate(const agentflow::trajectory::ModelRequest &request) override {
    ++calls;
    agentflow::trajectory::RawModelResponse r;
    r.transport_ok = !fail;
    r.attempts = 1;
    if (fail) {
      r.body = "raw diagnostic must not persist";
      return r;
    }
    const auto q = Json::parse(request.user_prompt);
    Json out;
    if (request.system_prompt == "DISTILL")
      out = Json{{"shareable", true},
                 {"target_scope", scope},
                 {"trigger_pattern", condition},
                 {"content", draft_text}};
    else if (request.system_prompt == "REVIEW") {
      const auto &existing = q["existing"].items();
      out =
          Json{{"decision", existing.empty() || new_entry ? "new" : "duplicate"},
               {"match_id", existing.empty() || new_entry ? "" : existing.front().str("memory_id")},
               {"privacy_safe", safe},
               {"generalizable", safe},
               {"supported", true},
               {"non_overriding", true},
               {"confidence", 0.99}};
    } else if (q.has("pairs")) {
      ++batch_calls;
      out = Json{{"relations", Json::Array{}}};
      for (const auto &pair : q["pairs"].items())
        out["relations"].push(Json{{"id", pair.str("id")}, {"relation", relation}, {"confidence", 0.99}});
    } else
      out = Json{{"relation", relation}, {"confidence", 0.99}};
    r.body = out.dump();
    if (request.system_prompt == error_stage)
      r.error_code = "output_token_limit_reached";
    if (fenced)
      r.body = "```json\r\n" + r.body + "\r\n```";
    return r;
  }

  // 测试辅助函数：name。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  std::string name() const override {
    return "fixture";
  }

  // 测试辅助函数：model_id。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  std::string model_id() const override {
    return "deterministic-test-model";
  }
};

struct Fixture {
  Json cfg = config(), state = new_shared_memory("test-app"), checkpoint;
  std::map<std::string, ep::EvolutionMemoryStore> memories;
  std::set<std::string> unverified;
  std::shared_ptr<Model> model = std::make_shared<Model>();
  unsigned saves = 0;

  // 测试辅助函数：add。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  void add(const std::string &user, const std::string &text = advice) {
    memories[user] = memory(user, text);
    enqueue_shared_memory(state, user);
  }

  // 测试辅助函数：cycle。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  void cycle(bool available = true) {
    state["next_due_ms"] = 0; // 这里只推进单元测试时钟，不触发生产强制刷新。
    process_shared_memory(
        state, cfg, [&](const auto &user) { return memories.at(user); },
        available ? model : nullptr,
        [&](const auto &value) {
          checkpoint = value;
          ++saves;
        },
        [&](const auto &user, const auto &entry) {
          return unverified.count(user) ? "" : "fixture-verified-" + user + entry.last_batch_id();
        });
  }

  // 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
  void drain() {
    for (int i = 0; i < 30 && !state["pending_users"].items().empty(); ++i)
      cycle();
  }

  // 测试辅助函数：snapshot。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  v1::ShadowPolicySnapshot snapshot(const std::string &user) {
    return te::compile_shadow_policy(memories.at(user), identity(user),
                                     proto<v1::ShadowOptions>(cfg["shadow_options"]),
                                     shared_memory_view(state, user));
  }
};

// 测试辅助函数：policy。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
v1::RuntimePolicy policy(v1::ShadowPolicySnapshot snapshot, bool private_approved = true) {
  v1::RuntimeOptions opts;
  opts.set_requested_level(v1::L1);
  opts.set_fallback("strict");
  opts.add_host_capabilities("context.request");
  opts.add_allowed_users(snapshot.identity().user_id());
  opts.set_max_context_bytes(8192);
  opts.set_experience_template("条件成立时参考：{{experience}}");
  for (const auto &e : snapshot.experiences())
    if (private_approved || e.application_shared()) {
      auto *a = opts.add_approved_experiences();
      a->set_memory_id(e.memory_id());
      a->set_content_revision(e.content_revision());
    }
  return te::compile_runtime_policy(std::move(snapshot), opts);
}

// 测试辅助函数：query。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
v1::RuntimeQuery query(const v1::RuntimePolicy &policy) {
  v1::RuntimeQuery q;
  *q.mutable_advice_query()->mutable_identity() = policy.snapshot().identity();
  q.mutable_advice_query()->set_query_id("q");
  q.mutable_advice_query()->set_decision_id("d");
  q.mutable_advice_query()->set_target_scope(scope);
  q.mutable_advice_query()->set_task(condition);
  q.set_execution_id("exec");
  q.set_boundary("llm");
  return q;
}

// 测试辅助函数：relevant_public_experience_is_not_starved。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void relevant_public_experience_is_not_starved() {
  auto own = memory("alice");
  own.mutable_entries(0)->mutable_candidate()->set_trigger_pattern("network timeout");
  own.mutable_entries(0)->mutable_candidate()->set_proposed_change(
      "On a network timeout, inspect connectivity before a bounded retry; independent operations "
      "can continue.");
  auto opts = proto<v1::ShadowOptions>(config()["shadow_options"]);
  opts.set_max_advice_items(1);
  const auto private_snapshot = te::compile_shadow_policy(own, identity(), opts);
  v1::SharedExperienceStore shared;
  shared.set_application_id("test-app");
  shared.set_user_id("alice");
  shared.set_revision(1);
  auto *published = shared.add_experiences();
  published->set_memory_id("shared-network");
  published->set_target_scope(scope);
  published->set_trigger_pattern("network timeout certificate handshake");
  published->set_content(
      "When the TLS handshake times out, inspect the certificate chain before retrying.");
  auto *relation = shared.add_relations();
  relation->set_relation("related");
  relation->set_private_memory_id(private_snapshot.experiences(0).memory_id());
  relation->set_private_content_revision(private_snapshot.experiences(0).content_revision());
  relation->set_shared_memory_id(published->memory_id());
  relation->set_shared_content_revision(shared_content_revision(*published));
  auto decide = [&](bool approve_private = true) {
    const auto runtime_policy =
        policy(te::compile_shadow_policy(own, identity(), opts, shared), approve_private);
    auto q = query(runtime_policy);
    q.mutable_advice_query()->set_task("network timeout certificate handshake");
    return te::PolicyRuntime(runtime_policy).decide(q);
  };
  auto result = decide();
  check(result.experiences_size() == 1 && result.experiences(0).memory_id() == "shared-network",
        "a less relevant private memory displaced specific public advice");
  relation->set_relation("duplicate");
  result = decide();
  check(result.experiences_size() == 1 && result.experiences(0).memory_id() == "private-alice",
        "private duplicate precedence was lost");
  result = decide(false);
  check(result.experiences_size() == 1 && result.experiences(0).memory_id() == "shared-network",
        "an unapproved private memory suppressed usable public advice");
}

// 测试辅助函数：automatic_and_duplicate。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void automatic_and_duplicate(bool fenced = false) {
  Fixture f;
  f.model->fenced = fenced;
  f.add("alice");
  f.drain();
  check(shared_memory_view(f.state).experiences_size() == 0,
        "one user cannot satisfy independent-user gate");
  f.add("bob");
  f.drain();
  const auto public_view = shared_memory_view(f.state);
  check(public_view.experiences_size() == 1, "automatic publication and semantic deduplication");
  const auto bytes = json(public_view).dump();
  for (const auto &forbidden : {"alice", "bob", "私人历史", "private-", "candidate-"})
    check(bytes.find(forbidden) == std::string::npos, "private provenance in public pool");
  auto p = policy(f.snapshot("alice"));
  check(te::PolicyRuntime(p).decide(query(p)).experiences_size() == 1,
        "cross-pool duplicate injected twice");
  auto narrowed_options = p.options();
  for (const auto &experience : p.snapshot().experiences())
    if (!experience.application_shared()) {
      auto *rewrite = narrowed_options.add_experience_overrides();
      rewrite->mutable_source_experience()->set_memory_id(experience.memory_id());
      rewrite->mutable_source_experience()->set_content_revision(experience.content_revision());
      rewrite->set_content(experience.content());
      rewrite->add_observed_operations("video");
    }
  const auto narrowed = te::compile_runtime_policy(p.snapshot(), narrowed_options);
  auto observation = query(narrowed);
  check(te::PolicyRuntime(narrowed).decide(observation).experiences().empty(),
        "public duplicate bypassed private applicability");
  observation.add_observed_operations("video");
  check(te::PolicyRuntime(narrowed).decide(observation).experiences_size() == 1,
        "valid private advice lost after observation guard");
  auto public_only = policy(f.snapshot("alice"), false);
  const auto result = te::PolicyRuntime(public_only).decide(query(public_only));
  check(result.experiences_size() == 1 && result.experiences(0).memory_id().find("shared-") == 0,
        "unapproved private duplicate blocks public");
  f.cfg["shadow_options"]["max_advice_items"] = 1;
  public_only = policy(f.snapshot("alice"), false);
  check(te::PolicyRuntime(public_only).decide(query(public_only)).experiences_size() == 1,
        "unapproved match exhausted top-K");
  const auto calls = f.model->calls.load();
  enqueue_shared_memory(f.state, "alice");
  f.drain();
  check(f.model->calls == calls, "unchanged content re-evaluated");
  check(f.checkpoint.dump() == f.state.dump(), "terminal checkpoint missing");
  auto foreign = public_view;
  foreign.set_application_id("another-app");
  bool rejected = false;
  try {
    te::compile_shadow_policy(f.memories["alice"], identity(),
                              proto<v1::ShadowOptions>(f.cfg["shadow_options"]), foreign);
  } catch (...) {
    rejected = true;
  }
  check(rejected, "cross-application shared data accepted");
  auto old = te::PolicyRuntime(p);
  f.memories["bob"].mutable_entries(0)->set_status(ep::EVOLUTION_MEMORY_STATUS_ARCHIVED);
  enqueue_shared_memory(f.state, "bob");
  f.drain();
  check(shared_memory_view(f.state).experiences().empty(),
        "withdrawn private support did not withdraw public");
  check(old.decide(query(p)).policy_id() == p.policy_id(), "in-flight snapshot changed");
}

// 测试辅助函数：freshness_capacity_and_recovery。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void freshness_capacity_and_recovery() {
  Fixture f;
  f.add("alice");
  f.add("bob");
  f.drain();
  for (auto &p : f.state["sources"].fields()) {
    if (p.second.str("user_key") == f.state["users"].fields().begin()->first)
      p.second["last_evidence_ms"] = 1;
  }
  f.cycle();
  check(shared_memory_view(f.state).experiences_size() == 0,
        "one active supporter refreshed another user's expired vote");
  check(f.state.num("next_due_ms", 0) > 1000, "expired vote caused idle spin");

  Fixture damaged;
  damaged.add("broken");
  damaged.memories.erase("broken");
  damaged.add("alice");
  damaged.add("bob");
  damaged.drain();
  check(shared_memory_view(damaged.state).experiences_size() == 1,
        "damaged user blocked unrelated users");
  check(damaged.state["user_errors"].fields().size() == 1, "damaged-user diagnostic missing");
  damaged.memories["broken"] = memory("broken");
  enqueue_shared_memory(damaged.state, "broken");
  damaged.drain();
  check(damaged.state["user_errors"].fields().empty(), "recovered user stayed quarantined");

  Fixture capacity;
  capacity.cfg["sharing"]["max_entries"] = 1;
  capacity.add("alice");
  capacity.add("bob");
  capacity.drain();
  const auto old_id = shared_memory_view(capacity.state).experiences(0).memory_id();
  capacity.model->new_entry = true;
  capacity.model->draft_text = "缺少图片素材时先产出文本分镜，独立检索继续。";
  capacity.add("carol", capacity.model->draft_text);
  capacity.add("dave", capacity.model->draft_text);
  capacity.drain();
  const auto updated = shared_memory_view(capacity.state);
  check(updated.experiences_size() == 1 && updated.experiences(0).memory_id() != old_id,
        "full public pool prevented newly-supported replacement");
  check(capacity.state["entries"].fields().size() <= 2, "candidate reservoir unbounded");

  Fixture stale;
  stale.add("alice");
  stale.add("bob");
  stale.drain();
  auto view = shared_memory_view(stale.state, "alice");
  for (auto &r : *view.mutable_relations())
    r.set_private_content_revision("outdated");
  auto snapshot =
      te::compile_shadow_policy(stale.memories["alice"], identity(),
                                proto<v1::ShadowOptions>(stale.cfg["shadow_options"]), view);
  check(snapshot.experiences_size() == 1 &&
            snapshot.excluded_counts().at("cross_memory_unreviewed") == 1,
        "stale relation authorized public injection");
  stale.cfg["shadow_options"]["max_snapshot_bytes"] = 4096;
  check(stale.snapshot("alice").ByteSizeLong() <= 4096,
        "cross-relation bytes exceeded snapshot budget");
}

// 测试辅助函数：conflict_and_recovery。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void conflict_and_recovery() {
  Fixture f;
  f.add("alice");
  f.add("bob");
  f.drain();
  f.unverified.insert("carol");
  f.model->relation = "conflict";
  f.add("carol", "缺少图片素材时，按当前用户要求使用纯文本分镜，不要求补充图片。");
  f.drain();
  check(shared_memory_view(f.state).experiences().empty(),
        "a verified cross-pool conflict must distrust public globally");
  check(f.state["entries"].fields().begin()->second.str("state") == "disputed",
        "public dispute not recorded");
  check(f.snapshot("carol").experiences_size() == 1,
        "valid private experience must survive public conflict");
  f.memories["carol"].mutable_entries(0)->set_status(ep::EVOLUTION_MEMORY_STATUS_ARCHIVED);
  enqueue_shared_memory(f.state, "carol");
  f.drain();
  check(shared_memory_view(f.state).experiences_size() == 1,
        "stale counterexample relation prevented recovery");
  for (auto &p : f.state["entries"].fields())
    p.second["last_evidence_ms"] = 1;
  f.cycle();
  check(shared_memory_view(f.state).experiences().empty() && f.state.num("retired_count", 0) == 1,
        "automatic public expiry");
  check(f.state.num("next_due_ms", 0) == 0, "empty manager kept polling");
}

// 测试辅助函数：gates_and_failures。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void gates_and_failures() {
  check(shared_memory_options(Json{}).flag("enabled", false),
        "omitted sharing must default to enabled");
  check(shared_memory_options(Json::Object{}).flag("enabled", false),
        "empty sharing must default to enabled");
  Fixture defaults;
  defaults.cfg["sharing"] = shared_memory_options(Json{{"min_interval_ms", 10}});
  defaults.add("alice");
  defaults.add("bob");
  defaults.drain();
  check(shared_memory_view(defaults.state).experiences_size() == 1,
        "default sharing did not publish verified experience");
  Fixture disabled;
  disabled.cfg["sharing"] = shared_memory_options(Json{{"enabled", false}});
  disabled.add("alice");
  const auto before_disabled = disabled.state.dump();
  disabled.cycle();
  check(disabled.model->calls == 0 && disabled.state.dump() == before_disabled,
        "explicit sharing opt-out ignored");
  Fixture missing;
  missing.unverified.insert("alice");
  missing.add("alice");
  missing.drain();
  check(missing.model->calls == 0 && missing.state["entries"].fields().empty(),
        "unverified/echo evidence reached promotion model");
  Fixture unsafe;
  unsafe.model->draft_text = "请联系 private.person@example.test 后继续视频生成。";
  unsafe.add("alice");
  unsafe.drain();
  check(unsafe.state["entries"].fields().empty(), "private contact published");
  check(unsafe.state.dump().find("private.person") == std::string::npos,
        "rejected model text persisted");
  Fixture rejected;
  rejected.model->safe = false;
  rejected.add("alice");
  rejected.drain();
  check(rejected.state["entries"].fields().empty(), "review veto ignored");
  Fixture failed;
  failed.model->fail = true;
  failed.add("alice");
  failed.drain();
  check(failed.model->calls <= 2 && failed.state["entries"].fields().empty(),
        "unbounded retry or failure published");
  check(failed.state.dump().find("raw diagnostic") == std::string::npos,
        "provider diagnostics leaked");
  for (const auto *stage : {"DISTILL", "REVIEW"}) {
    Fixture incomplete;
    incomplete.model->error_stage = stage;
    incomplete.add("alice");
    incomplete.drain();
    check(incomplete.state["entries"].fields().empty(),
          "truncated model response entered public memory");
    check(incomplete.model->calls <= 4, "truncated response caused unbounded retries");
  }
  Fixture unavailable;
  unavailable.add("alice");
  unavailable.cycle(false);
  check(unavailable.state.str("last_status") == "waiting_for_model" &&
            unavailable.state.num("next_due_ms", 0) == 0,
        "unconfigured model caused empty polling");
  check(unavailable.snapshot("alice").experiences_size() == 1, "model failure broke private path");
}

// 测试辅助函数：training_publication。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void training_publication() {
  Fixture f;
  f.add("alice");
  f.add("bob");
  const std::string a(64, 'a'), b(64, 'b');
  std::map<std::string, Json> groups{{"alice", Json::Array{a}}, {"bob", Json::Array{a}}};
  auto drain = [&] {
    for (int i = 0; i < 30 && !f.state["pending_users"].items().empty(); ++i) {
      f.state["next_due_ms"] = 0;
      process_shared_memory(
          f.state, f.cfg, [&](const auto &user) { return f.memories.at(user); }, f.model,
          [](const auto &) {},
          [&](const auto &user, const auto &) -> Json {
            if (f.unverified.count(user))
              return Json::Object{};
            return Json{{"fingerprint", "training-" + user + groups[user].dump()},
                        {"online_verified", false},
                        {"training_groups", groups[user]}};
          });
    }
  };
  drain();
  check(shared_memory_view(f.state).experiences_size() == 0,
        "duplicate training group counted as independent users");
  groups["bob"] = Json::Array{b};
  enqueue_shared_memory(f.state, "bob");
  drain();
  check(shared_memory_view(f.state).experiences_size() == 1,
        "independent training groups did not publish");
  const auto id = shared_memory_view(f.state).experiences(0).memory_id();
  check(f.state["entries"][id].num("supporting_users", 99) == 0 &&
            f.state["entries"][id].num("supporting_training_groups", 0) == 2,
        "training provenance mislabeled as users");
  f.cfg["sharing"]["allow_training_sources"] = false;
  reconcile_shared_memory(f.state, f.cfg["sharing"]);
  check(shared_memory_view(f.state).experiences_size() == 0,
        "training opt-out did not withdraw public advice");
  f.cfg["sharing"]["allow_training_sources"] = true;
  reconcile_shared_memory(f.state, f.cfg["sharing"]);
  f.state["relations"]["conflict"] =
      Json{{"state", "accepted"},
           {"user_key", "alice"},
           {"relation", Json{{"relation", "conflict"}, {"shared_memory_id", id}}}};
  reconcile_shared_memory(f.state, f.cfg["sharing"]);
  check(shared_memory_view(f.state).experiences_size() == 0,
        "private conflict did not suspend training-backed public advice");
  f.state["relations"].fields().erase("conflict");
  f.unverified.insert("bob");
  enqueue_shared_memory(f.state, "bob");
  drain();
  check(shared_memory_view(f.state).experiences_size() == 0,
        "withdrawn training evidence remained published");
}

// 回归用例：main。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
// 历史完成观察只辅助本轮归因；未来、其他用户和模型自称完成都不能混入事实。
void learning_context_boundaries() {
  const Json identity{{"run_id", "old"}, {"agent_id", "agent"},
                      {"user_id", "user"}, {"conversation_id", "conversation"}};
  const Json action{{"kind", "action.end"}, {"event_id", "created"},
                    {"actor_id", "worker"}, {"operation", "create"},
                    {"status", "completed"}, {"input", Json{{"target", "reservation"}}},
                    {"output", Json{{"status", "succeeded"}, {"id", "record"}}}};
  Json earlier{{"identity", identity}, {"events", Json::Array{action}}};
  auto declaration = action;
  declaration["event_id"] = "model-claim";
  declaration["attributes"]["semantic"] = "model";
  earlier["events"].push(declaration);
  auto current = earlier;
  current["identity"]["run_id"] = "current";
  current["events"] = Json::Array{Json{{"kind", "run.start"},
                                      {"input", Json{{"messages", Json::Array{
                                         Json{{"role", "user"}, {"content", "旧请求"}},
                                         Json{{"role", "user"}, {"content", "只细化行程"}}}}}}}};
  Json traces = Json::Array{earlier, current, earlier};
  const auto context = learning_history(traces, 1);
  check(context["observations"].items().size() == 1, "history admitted future or model claims");
  check(context["observations"].items()[0].str("run_id") == "old", "history lost original run");
  check(learning_user_input(current).text() == "只细化行程", "history replaced current input");
  check(learning_history(traces, 0)["observations"].items().empty(), "first turn read future facts");
  check(learning_history(traces, 1, 1).num("omitted_observations", 0) == 1,
        "truncated history was not marked incomplete");
  traces.items()[0]["identity"]["user_id"] = "another-user";
  check(learning_history(traces, 1)["observations"].items().empty(), "cross-user history leaked");
  const auto review = learning_review_context(earlier, {"created"});
  check(review["evidence"].items().size() == 1 &&
            review["evidence"].items()[0].str("evidence_kind") == "execution_observation",
        "public review replaced execution evidence with a model summary");

  // 检索回执再多，也应给另一执行者留出证据；数组错序时优先实际较新的预约。
  earlier["events"] = Json::Array{};
  auto reserved = action;
  reserved["timestamp_us"] = "20";
  reserved["output"] = Json{{"body", std::string(2048, 'x')}, {"status", "succeeded"}}.dump();
  earlier["events"].push(reserved);
  auto obsolete = action;
  obsolete["event_id"] = "obsolete";
  obsolete["timestamp_us"] = 10;
  earlier["events"].push(obsolete);
  auto dispatch = action;
  dispatch["event_id"] = "dispatch";
  dispatch["attributes"]["delegates_schedule"] = true;
  earlier["events"].push(dispatch);
  for (int i = 0; i < 40; ++i) {
    auto search = action;
    search["actor_id"] = "search";
    search["event_id"] = "search-" + std::to_string(i);
    search["timestamp_us"] = 100 - i;
    earlier["events"].push(search);
  }
  traces = Json::Array{earlier, current};
  const auto balanced = learning_history(traces, 1, 1800);
  bool kept = false;
  std::size_t observation_bytes = 0;
  for (const auto &fact : balanced["observations"].items()) {
    observation_bytes += fact.dump().size();
    check(fact.str("event_id") != "dispatch", "dispatch was promoted to execution fact");
    if (fact.str("actor") == "worker" && !kept) {
      check(fact.str("event_id") == "created", "history treated array position as event time");
      check(fact["output"]["fields"].str("status") == "succeeded",
            "JSON string preview lost completion evidence");
      kept = true;
    }
  }
  check(kept && observation_bytes <= 1800 && balanced.num("omitted_observations", 0) > 0,
        "search observations exhausted another actor's history budget");
  Json crowded{{"status", "succeeded"}, {"body", std::string(2048, 'x')}};
  for (int i = 0; i < 10; ++i)
    crowded["a" + std::to_string(i)] = std::string(70, 'a');
  check(learning_preview(crowded.dump(), 768)["fields"].str("status") == "succeeded",
        "early metadata fields crowded out the short status fact");
}

// 分组请求仍逐项判断；合法未决不重试，证据别名变化只重绑版本。
void batched_relation_management() {
  Fixture f;
  f.cfg["sharing"]["min_supporting_users"] = 1;
  f.cfg["sharing"]["relation_batch_size"] = 8;
  f.cfg["shared_relation_batch_prompt"] = "BATCH";
  f.model->new_entry = true;
  for (const auto &user : {"alice", "bob", "carol"}) {
    f.model->draft_text = advice + std::string("补充处理方式") + std::to_string(f.memories.size());
    f.add(user, f.model->draft_text);
    f.drain();
  }
  check(f.model->batch_calls > 0 && f.state["pending_users"].items().empty(),
        "public relations were not batched or queue failed to drain");
  const auto before = f.model->calls.load();
  f.memories["alice"].mutable_entries(0)->add_source_candidate_ids("additional-evidence-alias");
  enqueue_shared_memory(f.state, "alice");
  f.drain();
  check(f.model->calls == before, "provenance-only update invalidated semantic relation cache");
  f.unverified.insert("dave");
  f.model->relation = "uncertain";
  f.add("dave", "缺少素材时先分析用户提供的说明。");
  f.drain();
  std::size_t unresolved = 0;
  for (const auto &pair : f.state["relations"].fields())
    if (pair.second.str("state") == "unresolved") {
      ++unresolved;
      check(pair.second.num("attempts", 0) == 1, "valid uncertain judgement retried as protocol error");
    }
  check(unresolved == 3 && f.snapshot("dave").experiences_size() == 1,
        "unresolved public relationships were silently treated as independent");
  const auto done = f.model->calls.load();
  enqueue_shared_memory(f.state, "dave");
  f.drain();
  check(f.model->calls == done, "unchanged unresolved pairs were repeatedly judged");
}

// GEPA 反馈不建立第二套公共库：撤回失效来源、独立反证降信、组合效果不拆票。
void gepa_public_feedback() {
  Fixture f;
  for (const auto *user : {"alice", "bob", "carol"}) f.add(user);
  f.drain();
  check(shared_memory_view(f.state).experiences_size() == 1, "public fixture not published");
  f.memories["alice"].mutable_entries(0)->set_status(ep::EVOLUTION_MEMORY_STATUS_ARCHIVED);
  reconcile_shared_feedback(f.state, f.cfg, "alice", f.memories["alice"]);
  check(f.state["entries"].fields().begin()->second.num("supporting_users", 0) == 2 &&
        shared_memory_view(f.state).experiences_size() == 1,
        "withdrawing one private source removed other users' support");
  const auto reconciled = f.state.dump();
  reconcile_shared_feedback(f.state, f.cfg, "alice", f.memories["alice"]);
  check(f.state.dump() == reconciled, "replaying private withdrawal changed the ledger");

  Fixture public_test;
  public_test.add("alice"); public_test.add("bob"); public_test.drain();
  const auto original = public_test.state;
  auto empty = memory("alice");
  empty.clear_entries();
  const auto snapshot = te::compile_shadow_policy(empty, identity(),
      proto<v1::ShadowOptions>(public_test.cfg["shadow_options"]),
      shared_memory_view(public_test.state));
  check(snapshot.experiences_size() == 1, "public-only source missing");
  const auto key = "experience:" + snapshot.experiences(0).memory_id();
  Json before{{key, snapshot.experiences(0).content()}}, after{{key, nullptr}};
  te::dataset::v1::TaskEvaluationReport parent, child;
  parent.set_complete(true); child.set_complete(true);
  parent.set_run_id("public-parent"); child.set_run_id("public-child");
  Json p = Json::Object{}, c = Json::Object{}, applied = Json::Object{};
  for (const auto *task : {"public-task-a", "public-task-b"}) {
    for (auto *report : {&parent, &child}) {
      auto *trial = report->add_trials();
      trial->set_task_id(task); trial->set_task_version("v1");
      trial->set_variant_id("candidate"); trial->set_split("train");
      trial->set_verdict("passed"); trial->set_execution_status("completed");
    }
    p[task] = Json{{"quality", 1}, {"fitness", .5}};
    c[task] = Json{{"quality", 1}, {"fitness", .8}};
    applied[task] = Json{{"llm", 1}};
  }
  const auto packet = make_gepa_memory_feedback(before, after, snapshot, parent, child,
      p, c, applied, "dataset", "contract", true);
  auto &own = public_test.memories["alice"];
  *own.mutable_gepa_feedback() = proto<google::protobuf::Struct>(
      merge_gepa_memory_feedback(Json(), packet, snapshot));
  const auto original_private = own.entries(0).SerializeAsString();
  reconcile_shared_feedback(public_test.state, public_test.cfg, "alice", own);
  check(shared_memory_view(public_test.state).experiences_size() == 0 &&
        public_test.state["entries"].fields().begin()->second.str("state") == "disputed",
        "isolated public counterevidence did not lower public trust");
  check(own.entries(0).SerializeAsString() == original_private &&
        public_test.state["entries"].fields().size() == 1,
        "public counterevidence deleted an experience or changed private members");
  const auto saved = public_test.state.dump();
  reconcile_shared_feedback(public_test.state, public_test.cfg, "alice", own);
  check(public_test.state.dump() == saved, "replaying public evidence changed support");

  auto conditional = snapshot;
  auto *background = conditional.add_experiences();
  background->set_memory_id("background"); background->set_content_revision("background-v1");
  before["experience:background"] = after["experience:background"] = "保留的背景经验";
  *own.mutable_gepa_feedback() = proto<google::protobuf::Struct>(merge_gepa_memory_feedback(
      Json(), make_gepa_memory_feedback(before, after, conditional, parent, child, p, c,
        applied, "dataset", "contract", true), conditional));
  public_test.state = original;
  reconcile_shared_feedback(public_test.state, public_test.cfg, "alice", own);
  check(shared_memory_view(public_test.state).experiences_size() == 1 &&
        !public_test.state["evaluation_feedback"].object(),
        "combination result was incorrectly attributed to a public member");
}

int main(int argc, char **argv) {
  try {
    learning_context_boundaries();
    // 仅供固定测试的编解码；生产命令行没有私有记忆强行植入接口。
    if (argc == 2 && std::string(argv[1]) == "--encode-memory") {
      const auto input = Json::parse(std::string(std::istreambuf_iterator<char>(std::cin), {}));
      std::cout << proto<ep::EvolutionMemoryStore>(input).SerializeAsString();
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--encode-trace") {
      const auto input = Json::parse(std::string(std::istreambuf_iterator<char>(std::cin), {}));
      std::cout << proto<v1::TraceRun>(input).SerializeAsString();
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--seed-reports") {
      const auto input = Json::parse(std::string(std::istreambuf_iterator<char>(std::cin), {}));
      std::vector<ep::HybridEvaluationReport> reports;
      for (const auto &item : input["reports"].items()) {
        auto report = proto<ep::HybridEvaluationReport>(item);
        auto *reflection = report.mutable_reflection();
        reflection->Clear();
        check(report.assessments_size() > 0 &&
                  report.assessments(0).evidence().source_event_ids_size() > 0,
              "fixture source episode missing");
        report.mutable_dialogue_assessment()->set_status(ep::MODEL_CALL_STATUS_SUCCEEDED);
        report.mutable_dialogue_assessment()->set_confidence(0.99);
        const auto event = report.assessments(0).evidence().source_event_ids(0);
        reflection->set_summary("缺少素材时应请求补充");
        reflection->add_root_causes("缺少前置素材检查");
        reflection->add_evidence_event_ids(event);
        reflection->add_wasted_episode_ids(report.assessments(0).evidence().episode_id());
        reflection->add_experience_injections(advice);
        auto *experience = reflection->add_experience_details();
        experience->set_target_scope(input.str("scope"));
        experience->set_trigger_pattern(input.str("condition"));
        experience->set_proposed_change(advice);
        experience->add_evidence_event_ids(event);
        reports.push_back(std::move(report));
      }
      te::EvolutionOptions options;
      options.min_supporting_dialogues = 2;
      options.min_supporting_episodes = 2;
      auto batch = te::Pipeline({}, options).synthesize_reports(reports).report;
      ep::EvolutionMemoryStore store;
      te::Pipeline::merge_memory(batch, &store);
      check(store.entries_size() > 0, "fixture memory was not admitted");
      std::cout << store.SerializeAsString();
      return 0;
    }
    automatic_and_duplicate();
    batched_relation_management();
    automatic_and_duplicate(true);
    relevant_public_experience_is_not_starved();
    conflict_and_recovery();
    gates_and_failures();
    freshness_capacity_and_recovery();
    training_publication();
    gepa_public_feedback();
    std::cout << "Shared/private memory: promotion, deduplication, disputes, evidence withdrawal, "
                 "expiry, snapshots, privacy and failure gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
