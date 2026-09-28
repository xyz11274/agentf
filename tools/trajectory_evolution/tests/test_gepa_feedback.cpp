// 验证 GEPA 的可归因反馈进入唯一记忆库，而不是复制库或直接发布未验收策略。
// 全部使用确定性配对证据，不访问远程模型，不把测试分数当作业务效果。
#include "gepa_feedback.h"
#include "gepa_search.h"
#include "trajectory_evolution/policy_runtime.h"
#include "trajectory_evolution/shadow_runtime.h"
#include "agentflow/trajectory/evolution_pipeline.h"
#include <filesystem>
#include <future>
#include <iostream>
#include <unistd.h>

using namespace trajectory::evolution;
using namespace trajectory::evolution::integration;
namespace ep = agentflow::trajectory::evaluation;
namespace fs = std::filesystem;

void expect(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

// 用真正的记忆与策略编译器建立来源修订，避免人工填写不合法的快照哈希。
struct Fixture {
  ep::EvolutionMemoryStore memory;
  v1::ShadowPolicySnapshot snapshot;
  Json artifact;
  Json before{{"experience:memory-one", "缺少素材时先索取素材，继续独立计算。"}};
  Json after{{"experience:memory-one", "确认素材可用后再生成，独立计算只做一次。"}};
  Json parent_scores = Json::Object{}, child_scores = Json::Object{}, applied = Json::Object{};
  dataset::v1::TaskEvaluationReport parent, child;

  Fixture() {
    memory.set_schema_version(1);
    memory.set_store_kind("trajectory_evolution_memory");
    memory.set_store_version("evolution-memory-store-v1");
    memory.set_revision(1);
    auto *entry = memory.add_entries();
    entry->set_memory_id("memory-one");
    entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
    auto *candidate = entry->mutable_candidate();
    candidate->set_schema_version(1);
    candidate->set_candidate_id("candidate-one");
    candidate->set_generator_version("scoped-experience-generator-v1");
    candidate->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
    candidate->set_target_scope("assistant");
    candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
    candidate->set_trigger_pattern("素材缺失");
    candidate->set_observed_problem("缺少素材时重复生成");
    candidate->set_proposed_change(before["experience:memory-one"].text());
    candidate->set_support_user_count(1);
    auto *evidence = candidate->add_support_evidence();
    evidence->set_evidence_id("evidence-one");
    evidence->mutable_source()->set_user_id("user");
    evidence->mutable_source()->set_dialogue_id("dialogue-one");
    const auto identity = proto<v1::Identity>(Json{{"agent_id", "test-agent"}, {"user_id", "user"},
        {"conversation_id", "conversation"}, {"run_id", "run"}});
    snapshot = compile_shadow_policy(memory, identity,
        proto<v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
          {"max_policy_entries", 64}, {"max_advice_items", 3}, {"max_advice_bytes", 4096},
          {"max_query_bytes", 16384}, {"max_memory_bytes", 1048576},
          {"max_snapshot_bytes", 262144}, {"max_match_terms", 128}, {"min_matched_terms", 2}}));
    expect(snapshot.experiences_size() == 1, "fixture experience was not eligible");
    auto options = proto<v1::RuntimeOptions>(Json{{"requested_level", "L1"},
        {"fallback", "strict"}, {"host_capabilities", Json::Array{"context.request"}},
        {"allowed_users", Json::Array{"user"}}, {"allowed_conversations", Json::Array{"conversation"}},
        {"experience_template", "{{experience}}"}, {"max_context_bytes", 4096}});
    auto *ref = options.add_approved_experiences();
    ref->set_memory_id("memory-one");
    ref->set_content_revision(snapshot.experiences(0).content_revision());
    artifact = Json{{"schema_version", 1}, {"agent", Json{{"revision", "agent-v1"}}},
                    {"user_id", "user"}, {"level", "L1"},
                    {"policy", json(compile_runtime_policy(snapshot, options))}};
    parent.set_complete(true);
    parent.set_run_id("parent-report");
    child.set_complete(true);
    child.set_run_id("child-report");
    for (const auto *id : {"train-a", "train-b"}) {
      for (auto *report : {&parent, &child}) {
        auto *trial = report->add_trials();
        trial->set_task_id(id);
        trial->set_task_version("task-v1");
        trial->set_variant_id("candidate");
        trial->set_split("train");
        trial->set_verdict("passed");
        trial->set_execution_status("completed");
      }
      parent_scores[id] = Json{{"quality", 1}, {"strict_pass_rate", 1},
                              {"fitness", 0.5}, {"invalid_loops", 2}};
      child_scores[id] = Json{{"quality", 1}, {"strict_pass_rate", 1},
                             {"fitness", 0.8}, {"invalid_loops", 1}};
      applied[id] = Json{{"llm", 1}};
    }
  }

  Json packet(bool screened = true) const {
    return make_gepa_memory_feedback(before, after, snapshot, parent, child,
        parent_scores, child_scores, applied, "dataset-v1", "evaluation-v1", screened);
  }

  void store(const Json &packet) {
    *memory.mutable_gepa_feedback() = proto<google::protobuf::Struct>(
        merge_gepa_memory_feedback(json(memory.gepa_feedback()), packet, snapshot));
    update_gepa_memory_states(memory, snapshot);
  }
};

// 真实采用、有效配对、来源版本与训练划分缺一不可；评测异常不生成反证。
void eligibility_and_idempotence() {
  Fixture f;
  const auto packet = f.packet();
  const auto merged = merge_gepa_memory_feedback(Json(), packet, f.snapshot);
  const auto &record = merged["records"].fields().begin()->second;
  expect(record.num("positive_task_count", 0) == 2 && record.str("status") == "supported",
         "paired training improvement was not recorded");
  expect(merge_gepa_memory_feedback(merged, packet, f.snapshot).dump() == merged.dump(),
         "replayed feedback changed support or revision timestamp");
  const auto new_dataset = make_gepa_memory_feedback(f.before, f.after, f.snapshot,
      f.parent, f.child, f.parent_scores, f.child_scores, f.applied,
      "dataset-v2", "evaluation-v1", true);
  expect(merge_gepa_memory_feedback(merged, new_dataset, f.snapshot)["records"].fields().begin()
             ->second.num("positive_task_count", 0) == 2,
         "re-exporting a dataset duplicated support from unchanged tasks");
  const auto strict = merge_gepa_memory_feedback(Json(), packet, f.snapshot, 3);
  expect(merge_gepa_memory_feedback(Json(), strict, f.snapshot)["records"].fields().begin()
             ->second.str("status") == "insufficient_support", "delivery lowered support gate");
  auto stale = f.snapshot;
  stale.mutable_experiences(0)->set_content_revision("new-revision");
  expect(merge_gepa_memory_feedback(Json(), packet, stale)["records"].fields().empty(),
         "stale source feedback was applied");
  auto other = f.snapshot;
  other.mutable_identity()->set_user_id("other");
  bool rejected = false;
  try { merge_gepa_memory_feedback(Json(), packet, other); }
  catch (const Error &) { rejected = true; }
  expect(rejected, "feedback crossed user ownership");
  f.applied = Json::Object{};
  expect(f.packet().null(), "unapplied policy gained support");
  f = Fixture();
  for (auto &trial : *f.child.mutable_trials()) trial.set_split("holdout");
  expect(f.packet().null(), "holdout feedback entered memory");
  for (auto &trial : *f.child.mutable_trials()) trial.set_split("val");
  expect(f.packet().null(), "selection validation entered memory");
  f = Fixture();
  for (auto &trial : *f.child.mutable_trials()) trial.set_error_code("judge_invalid_json");
  expect(f.packet().null(), "judge failure became experience evidence");
}

// 修改版本有支持时下一次编译确实采用，但不把新正文的收益记在旧正文上。
void revision_reuse_and_lifecycle() {
  Fixture f;
  f.store(f.packet());
  const auto before = f.memory.entries(0).SerializeAsString();
  expect(f.memory.entries(0).status() == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE,
         "rewrite incorrectly validated its original source");
  const auto learned = apply_gepa_memory_feedback(f.artifact, json(f.memory.gepa_feedback()), Json());
  const auto policy = proto<v1::RuntimePolicy>(learned["policy"]);
  expect(policy.options().experience_overrides_size() == 1 &&
         policy.options().experience_overrides(0).content() == f.after["experience:memory-one"].text(),
         "next seed did not use the learned revision");
  expect(f.memory.entries(0).SerializeAsString() == before, "compile changed source memory");
  f.child_scores = f.parent_scores;
  for (auto &row : f.child_scores.fields()) row.second["quality"] = 0;
  f.store(f.packet(false));
  expect(apply_gepa_memory_feedback(f.artifact, json(f.memory.gepa_feedback()), Json()).dump() ==
         f.artifact.dump(), "conflicting revision was still adopted");
  expect(f.memory.entries(0).status() == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE,
         "failed rewrite demoted the unchanged original");

  Fixture removed;
  removed.after["experience:memory-one"] = nullptr;
  removed.store(removed.packet());
  expect(removed.memory.entries(0).status() == ep::EVOLUTION_MEMORY_STATUS_ARCHIVED &&
         removed.memory.entries(0).evaluation_feedback().negative_trial_count() == 2,
         "beneficial isolated removal did not archive its harmful source");

  Fixture useful;
  useful.after["experience:memory-one"] = nullptr;
  useful.child_scores = useful.parent_scores;
  for (auto &row : useful.child_scores.fields()) row.second["quality"] = 0;
  useful.store(useful.packet(false));
  expect(useful.memory.entries(0).status() == ep::EVOLUTION_MEMORY_STATUS_VALIDATED &&
         useful.memory.entries(0).evaluation_feedback().positive_trial_count() == 2,
         "harmful isolated removal did not validate the useful original");
  useful.child_scores = Fixture().child_scores;
  useful.store(useful.packet());
  expect(useful.memory.entries(0).status() == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE &&
         useful.memory.entries(0).evaluation_feedback().status() == "conflicting_trial_results",
         "contradictory trials left an experience validated");
}

// 单变量在组合中的局部贡献不能变成全局判决；背景成员也绑定修订和条件。
void conditional_bundle() {
  Fixture f;
  auto *other = f.snapshot.add_experiences();
  other->set_memory_id("other");
  other->set_content_revision("other-v1");
  other->set_content("另一条经验");
  f.before["experience:other"] = f.after["experience:other"] = other->content();
  f.after["experience:memory-one"] = nullptr;
  f.store(f.packet());
  expect(f.memory.entries(0).status() == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE,
         "conditional removal archived one member of a combination");
  const auto feedback = json(f.memory.gepa_feedback());
  const auto &record = feedback["records"].fields().begin()->second;
  expect(record.str("attribution") == "conditional_component" &&
         gepa_feedback_context_matches(record, f.before), "combination context was not retained");
  f.before["experience:other"] = "改变另一条经验";
  expect(!gepa_feedback_context_matches(record, f.before), "changed combination reused old evidence");
}

// 反思只获得仍适用的训练反证摘要，不获得任务正文、全库副本或过期的判决。
void reflection_rejection_summary() {
  Fixture f;
  f.child_scores["train-a"]["quality"] = 0;
  f.store(f.packet(false));
  const auto feedback = json(f.memory.gepa_feedback());
  const auto summary = gepa_reflection_rejections(f.artifact, feedback, Json(), "evaluation-v1");
  expect(summary["records"].items().size() == 1, "matching rejection was not made available");
  const auto &record = summary["records"].items().front();
  expect(record.str("status") == "conflicting" && record.num("negative_tasks", 0) == 1,
         "mixed outcomes became a global failure");
  expect(summary.dump().find("train-a") == std::string::npos &&
         !record.has("tasks") && !record.has("report_ids") && !record.has("sources"),
         "reflection summary copied raw memory evidence");
  auto audit_only = feedback;
  for (auto &item : audit_only["records"].fields()) {
    item.second["updated_ms"] = 1234;
    item.second["report_ids"]["replayed-report"] = true;
  }
  expect(gepa_reflection_rejections(f.artifact, audit_only, Json(), "evaluation-v1").dump() ==
         summary.dump(), "audit-only update created a new reflection context");
  expect(gepa_reflection_rejections(f.artifact, feedback, Json(), "evaluation-v2").null(),
         "different assessment context reused old counterevidence");
  auto obsolete = feedback;
  obsolete["records"].fields().begin()->second["sources"].items().front()["content_revision"] = "old";
  expect(gepa_reflection_rejections(f.artifact, obsolete, Json(), "evaluation-v1").null(),
         "changed source reused old counterevidence");
  auto changed_background = feedback;
  changed_background["records"].fields().begin()->second["background"]["experience:other"] = "old";
  expect(gepa_reflection_rejections(f.artifact, changed_background, Json(), "evaluation-v1").null(),
         "conditional counterevidence escaped its full background");
  auto holdout = feedback;
  holdout["records"].fields().begin()->second["split"] = "holdout";
  expect(gepa_reflection_rejections(f.artifact, holdout, Json(), "evaluation-v1").null(),
         "holdout evidence entered reflection history");
  auto other_user = feedback;
  other_user["user_id"] = "someone-else";
  bool rejected = false;
  try { gepa_reflection_rejections(f.artifact, other_user, Json(), "evaluation-v1"); }
  catch (const Error &) { rejected = true; }
  expect(rejected, "another user's private counterevidence was admitted");
  Fixture good;
  good.store(good.packet());
  expect(gepa_reflection_rejections(good.artifact, json(good.memory.gepa_feedback()), Json(),
                                   "evaluation-v1").null(),
         "supported changes were presented as rejected");
  for (int i = 0; i < 8; ++i) {
    f.after["experience:memory-one"] = "失败的改写候选" + std::to_string(i);
    f.store(f.packet(false));
  }
  const auto bounded = gepa_reflection_rejections(f.artifact, json(f.memory.gepa_feedback()),
                                                Json(), "evaluation-v1");
  expect(bounded["records"].items().size() == 4 && bounded.dump().size() <= 16384,
         "reflection history grew without a bound");
}

// 权威 Protobuf 与 JSON 视图同步；重放不会增加修订，失败回调不产生部分提交。
void file_transaction(const fs::path &root) {
  Fixture f;
  fs::create_directories(root);
  const auto pb = root / "evolution-memory.pb", view = root / "evolution-memory.json";
  using Pipeline = agentflow::trajectory::TrajectoryEvolutionPipeline;
  const auto packet = f.packet();
  expect(Pipeline::transform_memory_file(pb, view, [&](auto &memory) { memory = f.memory; }),
         "initial memory write failed");
  auto update = [&](auto &memory) {
    *memory.mutable_gepa_feedback() = proto<google::protobuf::Struct>(
        merge_gepa_memory_feedback(json(memory.gepa_feedback()), packet, f.snapshot));
    update_gepa_memory_states(memory, f.snapshot);
  };
  expect(Pipeline::transform_memory_file(pb, view, update), "feedback was not written");
  const auto saved = Pipeline::load_memory(pb);
  expect(!Pipeline::transform_memory_file(pb, view, update), "retry created another revision");
  expect(json(saved).dump() == json(Pipeline::load_memory(pb)).dump(),
         "duplicate feedback changed authoritative memory");
  expect(fs::file_size(view) > 0, "readable memory view was not written");
  bool failed = false;
  try { Pipeline::transform_memory_file(pb, view, [](auto &memory) {
      memory.clear_entries();
      throw std::runtime_error("simulated failure");
    }); }
  catch (...) { failed = true; }
  expect(failed && json(saved).dump() == json(Pipeline::load_memory(pb)).dump(),
         "failed transaction partially changed memory");
  // 模拟权威 PB 已提交，但 JSON 视图落盘失败；重试只修复视图，不再增加库修订。
  const auto blocked_view = root / "blocked-view";
  fs::create_directory(blocked_view);
  auto change = [](auto &memory) {
    (*memory.mutable_gepa_feedback()->mutable_fields())["view_recovery_test"].set_bool_value(true);
  };
  failed = false;
  try { Pipeline::transform_memory_file(pb, blocked_view, change); }
  catch (...) { failed = true; }
  const auto committed_revision = Pipeline::load_memory(pb).revision();
  expect(failed && committed_revision == saved.revision() + 1,
         "view failure did not leave a recoverable authoritative commit");
  fs::remove(blocked_view);
  expect(!Pipeline::transform_memory_file(pb, blocked_view, change) &&
         Pipeline::load_memory(pb).revision() == committed_revision && fs::is_regular_file(blocked_view),
         "view recovery repeated the memory update");
  // 并发训练和反馈各自修改一项，所有变更都必须保留，不能出现最后一个快照覆盖全库。
  std::vector<std::future<void>> writers;
  for (int i = 0; i < 8; ++i)
    writers.push_back(std::async(std::launch::async, [&, i] {
      Pipeline::transform_memory_file(pb, view, [&](auto &latest) {
        (*latest.mutable_gepa_feedback()->mutable_fields())["writer_" + std::to_string(i)]
            .set_bool_value(true);
      });
    }));
  for (auto &writer : writers) writer.get();
  const auto concurrent = Pipeline::load_memory(pb);
  expect(concurrent.revision() == committed_revision + 8, "concurrent commits lost revisions");
  for (int i = 0; i < 8; ++i)
    expect(concurrent.gepa_feedback().fields().count("writer_" + std::to_string(i)) == 1,
           "one writer overwrote another writer's update");
}

int main(int argc, char **argv) {
  try {
    expect(argc == 2, "build directory required");
    eligibility_and_idempotence();
    revision_reuse_and_lifecycle();
    conditional_bundle();
    reflection_rejection_summary();
    const auto root = fs::path(argv[1]) / ("gepa-feedback-test-" + std::to_string(getpid()));
    file_transaction(root);
    fs::remove_all(root);
    std::cout << "GEPA feedback tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
