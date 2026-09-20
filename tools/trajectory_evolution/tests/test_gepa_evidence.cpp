// 直接回归搜索内部的证据处理，不为测试扩大交付 API，也不调用宿主或远程模型。
#include "../src/gepa/gepa_search.cpp"
#include "agentflow/trajectory/experience.h"
#include "trajectory_evolution/orchestration.h"
#include <iostream>

using namespace trajectory::evolution::integration;

void expect(bool value, const char* message) {
  if(!value) throw std::runtime_error(message);
}

Json public_alias_fixture() {
  namespace te = trajectory::evolution;
  namespace ep = agentflow::trajectory::evaluation;
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1); memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1"); memory.set_revision(1);
  auto* entry = memory.add_entries(); entry->set_memory_id("private");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto* candidate = entry->mutable_candidate(); candidate->set_schema_version(1); candidate->set_candidate_id("private-source");
  candidate->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  candidate->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION); candidate->set_target_scope("assistant");
  candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED); candidate->set_trigger_pattern("Missing image argument");
  candidate->set_observed_problem("Repeated work without the required image");
  candidate->set_proposed_change("Request the required image before rendering."); candidate->set_support_user_count(1);
  auto* evidence = candidate->add_support_evidence(); evidence->set_evidence_id("private-evidence");
  evidence->mutable_source()->set_user_id("user"); evidence->mutable_source()->set_dialogue_id("source");
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"}, {"user_id", "user"},
      {"conversation_id", "conversation"}, {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
      {"max_policy_entries", 64}, {"max_advice_items", 3}, {"max_advice_bytes", 4096},
      {"max_query_bytes", 16384}, {"max_memory_bytes", 1048576}, {"max_snapshot_bytes", 262144},
      {"max_match_terms", 128}, {"min_matched_terms", 2}});
  const auto own = te::compile_shadow_policy(memory, identity, limits);
  expect(own.experiences_size() == 1, "private alias fixture was not compiled");
  te::v1::SharedExperienceStore shared; shared.set_application_id("agent"); shared.set_user_id("user"); shared.set_revision(1);
  for(const bool duplicate : {true, false}) {
    auto* item = shared.add_experiences(); item->set_memory_id(duplicate ? "shared-copy" : "shared-independent");
    item->set_target_scope("assistant"); item->set_trigger_pattern(duplicate ? candidate->trigger_pattern() : "Final delivery verification");
    item->set_content(duplicate ? candidate->proposed_change() : "Verify delivery before reporting completion.");
    auto* relation = shared.add_relations(); relation->set_private_memory_id("private");
    relation->set_private_content_revision(own.experiences(0).content_revision());
    relation->set_shared_memory_id(item->memory_id()); relation->set_shared_content_revision(shared_content_revision(*item));
    relation->set_relation(duplicate ? "duplicate" : "independent");
  }
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits, shared);
  expect(snapshot.experiences_size() == 3 && snapshot.cross_relations_size() == 1,
      "shared alias fixture did not use the real compiler");
  auto options = proto<te::v1::RuntimeOptions>(Json{{"requested_level", "L2"}, {"fallback", "strict"},
      {"host_capabilities", Json::Array{"context.request", "action.filter"}},
      {"allowed_users", Json::Array{"user"}}, {"allowed_conversations", Json::Array{"conversation"}},
      {"experience_template", "{{experience}}"}, {"max_context_bytes", 4096}});
  for(const auto& source : snapshot.experiences()) {
    auto* ref = options.add_approved_experiences(); ref->set_memory_id(source.memory_id()); ref->set_content_revision(source.content_revision());
  }
  return Json{{"schema_version", 1}, {"agent", Json{{"revision", "alias-fixture"}}}, {"user_id", "user"},
      {"level", "L2"}, {"policy", json(te::compile_runtime_policy(snapshot, options))}};
}

void public_private_alias_revisions() {
  namespace te = trajectory::evolution;
  auto seed = public_alias_fixture();
  const auto policy = proto<te::v1::RuntimePolicy>(seed["policy"]);
  const auto aliases = canonical_experience_aliases(policy);
  expect(aliases.size() == 1 && aliases.begin()->second == "private"
      && aliases.begin()->first.second != policy.snapshot().cross_relations(0).shared_content_revision(),
      "fixture failed to distinguish publication and shadow hashes");
  expect(all_source_experiences(seed).items().size() == 3 && source_experiences(seed).items().size() == 2,
      "a revision-pinned duplicate created an inert second L1 search axis");
  for(int mode = 0; mode < 7; ++mode) {
    auto changed = policy;
    if(mode == 0) changed.mutable_snapshot()->mutable_cross_relations(0)->set_shared_content_revision("stale");
    if(mode == 1) changed.mutable_snapshot()->mutable_cross_relations(0)->set_private_content_revision("stale");
    if(mode == 2) changed.mutable_snapshot()->mutable_cross_relations(0)->set_relation("related");
    if(mode == 3) for(auto& item : *changed.mutable_snapshot()->mutable_experiences())
      if(item.memory_id() == "shared-copy") item.set_content("Changed public guidance.");
    if(mode == 4) for(auto& item : *changed.mutable_snapshot()->mutable_experiences())
      if(item.memory_id() == "shared-copy") item.set_target_scope("other");
    if(mode >= 5) for(auto& ref : *changed.mutable_options()->mutable_approved_experiences())
      if(ref.memory_id() == (mode == 5 ? "shared-copy" : "private")) ref.set_content_revision("unapproved-revision");
    expect(canonical_experience_aliases(changed).empty(), "stale, different or unapproved sources were merged");
  }
  Search search; search.seed = seed;
  const auto receipt = [&](const std::string& revision) {
    Report report; auto* trial = report.add_trials(); trial->set_variant_id("candidate");
    const Json refs = Json::Array{Json{{"memory_id", "shared-copy"}, {"content_revision", revision}}};
    const Json event{{"kind", "advice.applied"}, {"application", Json{{"status", "applied"}}},
        {"attributes", Json{{"receipt", Json{{"boundary", "llm"}, {"experiences", refs}}}}}};
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", Json::Array{event}}}}});
    return report;
  };
  expect(search.applied_experiences(receipt(aliases.begin()->first.second)) == std::set<std::string>{"experience:private"},
      "real public receipt was not attributed to its canonical search component");
  expect(!search.applied_experiences(receipt("stale")).count("experience:private"), "stale receipt borrowed a private application");
  search.controls = Json{{"enabled", true}, {"prompt_file", "unused-in-unit-test"},
      {"actions", Json{{"render", Json::Array{"image"}}}}};
  const Json draft{{"action_rules", Json::Array{Json{{"memory_id", "shared-copy"}, {"operation", "render"},
      {"required_arguments", Json::Array{"image"}}, {"match_mode", "predicate"}, {"reason", "Require an image"}}}},
      {"loop_rules", Json::Array{}}};
  const auto controlled = apply_control_draft(policy.snapshot(), policy.options(), search.controls, draft);
  search.seed["policy"] = json(te::compile_runtime_policy(policy.snapshot(), controlled));
  const Json selected = Json::Array{"experience:private"};
  const auto ablated = search.mutate(search.seed, Json{{"reason", "Test only the injection"},
      {"experience", Json{{"memory_id", "private"}, {"content", nullptr}}}}, selected);
  const auto ablated_policy = proto<te::v1::RuntimePolicy>(ablated["policy"]);
  expect(ablated_policy.options().approved_experiences_size() == 3
      && ablated_policy.options().disabled_injections_size() == 2,
      "canonical L1 ablation lost control approval or left its public copy active");
  expect(search.components(ablated)["experience:private"].null()
      && !search.components(ablated).has("experience:shared-copy")
      && search.components(ablated).has("experience:shared-independent"), "ablation altered unrelated public search axes");
  auto query = proto<te::v1::RuntimeQuery>(Json{{"boundary", "actions"}, {"execution_id", "execution"},
      {"advice_query", Json{{"identity", json(policy.snapshot().identity())}, {"target_scope", "assistant"},
          {"query_id", "query"}, {"decision_id", "decision"}, {"task", "Render the image"}}},
      {"actions", Json::Array{Json{{"action_id", "render-one"}, {"operation", "render"}}}}});
  expect(te::PolicyRuntime(ablated_policy).decide(query).actions(0).decision() == "reject",
      "disabling duplicate L1 text disabled a public-sourced action guard");
  const auto content = source_experiences(search.seed).items().front().str("content");
  auto llm = query; llm.set_boundary("llm"); llm.clear_actions();
  llm.mutable_advice_query()->set_task("Missing image argument");
  expect(!te::PolicyRuntime(proto<te::v1::RuntimePolicy>(search.seed["policy"])).decide(llm).context().empty()
      && te::PolicyRuntime(ablated_policy).decide(llm).context().empty(),
      "canonical ablation did not remove an actually injectable private/public pair");
  const auto narrowed = search.mutate(search.seed, Json{{"reason", "Only after the observed operation"},
      {"experience", Json{{"memory_id", "private"}, {"content", content},
          {"observed_operations", Json::Array{"render"}}}}}, selected);
  expect(te::PolicyRuntime(proto<te::v1::RuntimePolicy>(narrowed["policy"])).decide(llm).context().empty(),
      "shadowed public advice bypassed the canonical operation condition");
  llm.add_observed_operations("render");
  expect(!te::PolicyRuntime(proto<te::v1::RuntimePolicy>(narrowed["policy"])).decide(llm).context().empty(),
      "canonical operation condition disabled valid injection");
  const auto restored = search.mutate(ablated, Json{{"reason", "Restore canonical injection"},
      {"experience", Json{{"memory_id", "private"}, {"content", content}}}}, selected);
  expect(proto<te::v1::RuntimePolicy>(restored["policy"]).options().disabled_injections_size() == 0
      && search.components(restored).dump() == search.components(search.seed).dump(),
      "canonical injection could not be restored with public control approval intact");
}

void selected_experience_receipt_attribution() {
  const auto seed = public_alias_fixture();
  const auto policy = proto<trajectory::evolution::v1::RuntimePolicy>(seed["policy"]);
  std::map<std::string, std::string> revisions;
  for(const auto& source : policy.snapshot().experiences()) revisions[source.memory_id()] = source.content_revision();
  const Json selected = Json::Array{"experience:private"};
  Json events = Json::Array{};
  for(std::size_t i = 0; i < 12; ++i) events.push(Json{{"kind", "advice.applied"},
      {"event_id", "receipt-" + std::to_string(i)}, {"actor_id", "worker"}, {"execution_id", "branch"},
      {"application", Json{{"status", "applied"}}}, {"attributes", Json{{"receipt", Json{{"boundary", "llm"},
          {"experiences", Json::Array{Json{{"memory_id", "shared-copy"},
              {"content_revision", revisions.at("shared-copy")}}}}}}}}});
  trajectory::evolution::dataset::v1::EvaluationTrial trial;
  const auto evidence = [&](const Json& rows) {
    return proto<google::protobuf::Struct>(Json{{"trace", Json{{"identity", Json{{"run_id", "run"}}}, {"events", rows}}}});
  };
  *trial.mutable_evidence() = evidence(events);
  const auto observed = selected_experience_applications(trial, selected, seed);
  expect(observed.str("status") == "observed" && observed.num("count", 0) == 12
      && observed["points"].items().size() == 8 && observed.flag("truncated", false),
      "receipt attribution lost its total count or exceeded the bounded point list");
  const auto& point = observed["points"].items().front();
  expect(point.str("run_id") == "run" && point.str("receipt_event_id") == "receipt-0"
      && point.str("memory_id") == "private" && point.str("content_revision") == revisions.at("private")
      && point.str("receipt_memory_id") == "shared-copy"
      && point.str("receipt_content_revision") == revisions.at("shared-copy"),
      "public receipt lost its actual or canonical source revision");
  for(int mode = 0; mode < 5; ++mode) {
    auto row = events.items().front();
    if(mode == 0) row["application"]["status"] = "rejected";
    if(mode == 1) row["kind"] = "custom";  // A query is not an application.
    if(mode == 2) row["attributes"]["receipt"]["experiences"].items()[0]["content_revision"] = "stale";
    if(mode == 3) row["attributes"]["receipt"]["experiences"].items()[0] = Json{
        {"memory_id", "shared-independent"}, {"content_revision", revisions.at("shared-independent")}};
    if(mode == 4) row.fields().erase("event_id");
    *trial.mutable_evidence() = evidence(Json::Array{row});
    const auto unused = selected_experience_applications(trial, selected, seed);
    expect(unused.str("status") == "not_observed" && unused.num("count", 1) == 0 && unused["points"].items().empty(),
        "unapplied, stale, unrelated or unlocatable source became selected experience evidence");
  }
  auto stale_alias = seed;
  stale_alias["policy"]["snapshot"]["cross_relations"].items().front()["shared_content_revision"] = "stale";
  *trial.mutable_evidence() = evidence(events);
  expect(selected_experience_applications(trial, selected, stale_alias).str("status") == "not_observed",
      "stale duplicate relation borrowed another source's application");
}

void experience_packet_attribution_isolation() {
  Search search; search.seed = public_alias_fixture();
  search.config = Json{{"max_input_bytes", 65536}, {"max_feedback_trials", 4},
      {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const auto policy = proto<trajectory::evolution::v1::RuntimePolicy>(search.seed["policy"]);
  std::map<std::string, std::string> revisions;
  for(const auto& source : policy.snapshot().experiences()) revisions[source.memory_id()] = source.content_revision();
  Report report;
  for(const auto* id : {"used", "unrelated"}) {
    auto* task = search.dataset.add_tasks(); task->set_task_id(id); task->set_split("train");
    task->set_task_input("Complete the requested work and verify its artifact.");
    task->set_expected_behavior("Produce the requested file; report failures honestly.");
    auto* trial = report.add_trials(); trial->set_task_id(id); trial->set_trial_id(id);
    trial->set_variant_id("candidate"); trial->set_execution_status("completed");
    trial->set_verdict(std::string(id) == "used" ? "passed" : "failed");
    const std::string memory = std::string(id) == "used" ? "private" : "shared-independent";
    const Json refs = Json::Array{Json{{"memory_id", memory}, {"content_revision", revisions.at(memory)}}};
    Json events = Json::Array{Json{{"kind", "custom"}, {"step_id", "model"}, {"event_id", "query"},
        {"attributes", Json{{"type", "policy.query"}, {"decision", Json{{"query", Json{{"boundary", "llm"}}},
            {"experiences", refs}}}}}},
        Json{{"kind", "advice.applied"}, {"step_id", "model"}, {"event_id", "receipt"},
            {"application", Json{{"status", "applied"}}}, {"attributes", Json{{"receipt", Json{
                {"boundary", "llm"}, {"experiences", refs}}}}}}};
    for(std::size_t i = 0; i < 8; ++i) events.push(Json{{"kind", "action.end"}, {"event_id", "action-" + std::to_string(i)},
        {"step_id", "model"}, {"operation", "model_call"}, {"attributes", Json{{"semantic", "model"}}},
        {"output", "original-evidence-" + std::string(id) + std::string(2000, 'x')}});
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{
        {"identity", Json{{"run_id", std::string("run-") + id}}}, {"events", events}}}});
  }
  const Json selected = Json::Array{"experience:private"};
  const auto packet = search.packet(search.seed, report, selected);
  expect(packet["feedback"].items().size() == 2, "counterexample or applied training evidence was dropped");
  const auto& used = packet["feedback"].items()[0]; const auto& unused = packet["feedback"].items()[1];
  expect(used.str("task_id") == "used" && used["selected_experience_applications"].num("count", 0) == 1
      && used["trial"]["evidence"]["runs"].array(), "actual selected application lost its execution feedback");
  expect(unused.str("task_id") == "unrelated" && unused["selected_experience_applications"].str("status") == "not_observed"
      && unused.str("evidence_role") == "unapplied_counterexample" && unused["trial"]["evidence"].null()
      && unused.dump().size() < 4096 && unused["trial"].str("verdict") == "failed",
      "an unrelated failed trajectory was presented as selected experience evidence");

  // 全部未命中和停用后恢复都需要原反馈；不能因缺少应用回执关闭这两个搜索方向。
  Report unobserved; *unobserved.add_trials() = report.trials(1);
  auto ablated = search.mutate(search.seed, Json{{"reason", "Test recovery"},
      {"experience", Json{{"memory_id", "private"}, {"content", nullptr}}}}, selected);
  for(const auto& parent : {search.seed, ablated}) {
    const auto retry = search.packet(parent, unobserved, selected);
    const auto& row = retry["feedback"].items().front();
    expect(row["selected_experience_applications"].str("status") == "not_observed"
        && !row.has("evidence_role") && row["trial"]["evidence"]["runs"].array()
        && row.dump().find("original-evidence-unrelated") != std::string::npos,
        "missing or disabled experience lost feedback needed to improve retrieval or restore injection");
  }
}

void empty_experience_receipts() {
  Search search;
  search.seed = Json{{"policy", Json{{"snapshot", Json::Object{}}, {"options", Json::Object{}}}}};
  Json receipt{{"kind", "advice.applied"}, {"application", Json{{"status", "applied"}}},
      {"attributes", Json{{"receipt", Json{{"boundary", "llm"}}}}}};
  for(int mode = 0; mode < 3; ++mode) {
    auto empty = receipt;
    if(mode == 1) empty["attributes"]["receipt"]["experiences"] = Json();
    if(mode == 2) empty["attributes"]["receipt"]["experiences"] = Json::Array{};
    auto used = receipt;
    used["attributes"]["receipt"]["experiences"] = Json::Array{Json{{"memory_id", "used"}}};
    auto unused = used; unused["application"]["status"] = "not_applied";
    unused["attributes"]["receipt"]["experiences"] = Json::Array{Json{{"memory_id", "unused"}}};
    Report report;
    auto* trial = report.add_trials(); trial->set_variant_id("candidate");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"traces", Json::Array{
        Json{{"events", Json::Array{empty, unused}}}, Json{{"events", Json::Array{used, empty}}}}}});
    expect(search.applied_experiences(report) == std::set<std::string>{"experience:used"},
        "empty receipt interrupted a later dialogue or counted unused experience");
  }
  // 空值可省略；错误类型仍需被拒绝，不能伪造为一次有效应用。
  receipt["attributes"]["receipt"]["experiences"] = "invalid";
  Report malformed; auto* trial = malformed.add_trials(); trial->set_variant_id("candidate");
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", Json::Array{receipt}}}}});
  bool rejected = false;
  try { search.applied_experiences(malformed); } catch(const Error& error) { rejected = std::string(error.what()) == "array_required"; }
  expect(rejected, "malformed experience receipt silently accepted");
}

void experience_ablation_checkpoint_deduplication() {
  const Json selected = Json::Array{"experience:one"};
  Search search;
  search.tree = Json{{"nodes", Json::Array{
      Json{{"version", "same-parent-version"}, {"parent", nullptr}},
      Json{{"parent", 0}, {"components", selected}, {"generation_method", "experience_ablation"}, {"status", "rejected_minibatch"}},
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
  Search resumed; resumed.tree = Json::parse(search.tree.dump());
  expect(resumed.experience_ablation_seen(resumed.tree["nodes"].items()[2].str("version"), selected, 3),
      "checkpoint reload forgot a previous ablation attempt");
  resumed.tree["nodes"].items()[1].fields().erase("generation_method");
  expect(!resumed.experience_ablation_seen("same-parent-version", selected, 3),
      "ordinary model mutation was treated as automatic ablation");
}

void observed_training_screening() {
  Search search;
  const auto score = [](double quality, double fitness, double loops, double duration) {
    return Json{{"quality", quality}, {"fitness", fitness}, {"invalid_loops", loops},
        {"loop_method", "reviewed"}, {"duration_ms", duration}};
  };
  const Json parent{{"changed", score(0, .3, 2, 34)}, {"unchanged-a", score(0, 0, 5, 100)},
      {"unchanged-b", score(0, 0, 5, 100)}};
  const Json candidate{{"changed", score(0, .1, 6, 57)}, {"unchanged-a", score(1, 1, 0, 1)},
      {"unchanged-b", score(1, 1, 0, 1)}};
  const Json changes{{"changed", Json{{"llm", 1}}}};
  expect(gepa::no_worse(candidate, parent), "fixture must reproduce a misleading full-batch improvement");
  for(const auto* acceptance : {"strict_improvement", "improvement_or_equal", "pareto"}) {
    search.config = Json{{"acceptance", acceptance}};
    expect(!search.observed_training_gain(candidate, parent, changes),
        "unchanged tasks hid a regression on the only applied policy change");
    const auto before = candidate.dump();
    auto improved = candidate; improved["changed"] = score(1, .9, 1, 20);
    expect(search.observed_training_gain(improved, parent, changes), "real attributed training improvement was rejected");
    auto neutral = candidate; neutral["changed"] = parent["changed"];
    expect(search.observed_training_gain(neutral, parent, changes) == (std::string(acceptance) != "strict_improvement"),
        "neutral attribution ignored the configured acceptance mode");
    expect(!search.observed_training_gain(candidate, parent, Json{{"changed", Json{{"llm", 0}}}}),
        "zero application count was treated as a changed trial");
    expect(!search.observed_training_gain(candidate, parent, Json()), "missing changes were interpreted as an improvement");
    expect(!search.observed_training_gain(candidate, parent, Json{{"missing", Json{{"actions", 1}}}}),
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

  // 旧 screened 检查点先用原训练评分重验，不加载候选、调用模型或重新跑验证题。
  char directory[] = "/tmp/trajectory-gepa-screening-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create screening fixture");
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{directory};
  search.root = directory;
  search.config["acceptance"] = "pareto";
  const Json screening{{"parent", parent}, {"candidate", candidate}, {"applied_changes", changes}};
  search.tree = Json{{"model_calls", 4}, {"trials_used", 12}, {"nodes", Json::Array{
      Json{{"id", 0}, {"parent", nullptr}, {"status", "accepted"}},
      Json{{"id", 1}, {"parent", 0}, {"kind", "reflection"}, {"status", "screened"}, {"screening", screening}}}}};
  search.validate(1);
  const auto& node = static_cast<const Json&>(search.tree)["nodes"].items()[1];
  expect(node.str("status") == "rejected_minibatch" && node.str("reason") == "no_observed_training_gain",
      "resuming a harmful screened node continued into validation");
  expect(!node.has("validation") && !node.has("validation_probe") && !node.has("error"),
      "rejected training attribution loaded an artifact or evaluated validation");
  expect(node["screening"].dump() == screening.dump(), "resume rewrote the measured training scores");
  expect(search.tree.num("model_calls", 0) == 4 && search.tree.num("trials_used", 0) == 12,
      "resume attribution check spent model or execution budget");
  const auto stopped = search.tree.dump(); search.validate(1);
  expect(search.tree.dump() == stopped, "resume attribution rejection is not idempotent");
}

void compact_experience_feedback_budget() {
  Json events = Json::Array{};
  auto attributes = [](const std::string& semantic, const std::string& parent = "main") {
    return Json{{"semantic", semantic}, {"executed", true},
        {"extensions", Json{{"point_reference", Json{{"parent", parent}}}}}};
  };
  auto tool = [&](const std::string& id, const std::string& parent = "main") {
    events.push(Json{{"event_id", id}, {"kind", "action.end"}, {"step_id", id}, {"actor_id", "worker"},
        {"execution_id", "branch"}, {"operation", "fetch"}, {"status", "completed"},
        {"output", "tool-evidence-" + id}, {"attributes", attributes("action", parent)}});
  };
  auto model = [&](const std::string& id, const std::string& memory, bool concurrent = false) {
    Json refs = Json::Array{Json{{"memory_id", memory}, {"content_revision", "revision-" + memory}}};
    Json query{{"boundary", "llm"}, {"observed_operations", Json::Array{"fetch"}},
        {"advice_query", Json{{"target_scope", "assistant"}, {"state", "query-state-" + id + std::string(1200, 's')}}}};
    events.push(Json{{"event_id", id + "-query"}, {"kind", "custom"}, {"step_id", id},
        {"attributes", Json{{"type", "policy.query"}, {"decision", Json{{"query", query}, {"experiences", refs}}}}}});
    if(concurrent) tool("concurrent-result");
    events.push(Json{{"event_id", id}, {"kind", "action.end"}, {"step_id", id}, {"actor_id", "worker"},
        {"execution_id", "branch"}, {"operation", "model_call"}, {"status", "completed"},
        {"output", "model-output-" + id + std::string(2048, 'x')}, {"attributes", attributes("model")}});
    events.push(Json{{"event_id", id + "-receipt"}, {"kind", "advice.applied"}, {"step_id", id},
        {"application", Json{{"status", "applied"}}},
        {"attributes", Json{{"receipt", Json{{"boundary", "llm"}, {"experiences", refs}}}}}});
  };
  for(unsigned i = 0; i < 12; ++i) model("early-" + std::to_string(i), "private");
  tool("observed-before"); tool("sibling-before", "sibling");
  model("late-application", "private", true);
  tool("sibling-after", "sibling"); tool("following-action");
  Json traces = Json::Array{Json{{"identity", Json{{"run_id", "first"}}}, {"events", events}}};
  events = Json::Array{}; model("shared-application", "shared");
  traces.push(Json{{"identity", Json{{"run_id", "second"}}}, {"events", events}});
  trajectory::evolution::dataset::v1::EvaluationTrial trial;
  trial.set_verdict("passed"); trial.set_execution_status("completed");
  *trial.mutable_evidence() = proto<google::protobuf::Struct>(Json{{"traces", traces}, {"output", "final"}});
  const Json selected = Json::Array{"experience:private", "experience:shared"};
  const auto full = feedback_trial(trial, Json::Object{});
  expect(full.dump().size() > 8192, "L1 fixture did not exceed the feedback budget");
  for(const std::size_t budget : {std::size_t(4096), std::size_t(8192)}) {
    const auto compact = compact_experience_feedback_trial(trial, full, selected, budget);
    expect(compact.dump().size() <= budget, "compact L1 feedback exceeded its remaining byte budget");
    expect(compact["evidence"]["runs"].items().size() == 2, "private/public applications lost a run");
    bool late = false, shared = false;
    for(const auto& run : compact["evidence"]["runs"].items()) for(const auto& action : run["actions"].items()) {
      const auto id = action.str("event_id");
      expect(id != "sibling-before" && id != "sibling-after" && id != "concurrent-result",
          "unobserved or independent result was attributed to an L1 request");
      if(id == "late-application" || id == "shared-application") {
        late |= id == "late-application"; shared |= id == "shared-application";
        expect(action.str("actor_id") == "worker" && action.str("execution_id") == "branch",
            "L1 application lost its execution identity");
        const auto& q = action["experience_query"];
        expect(q.str("application_status") == "applied" && q.str("receipt_event_id") == id + "-receipt"
            && q.str("state_excerpt").find("query-state-" + id) == 0
            && q["observed_operations"].items().front().text() == "fetch", "L1 applicability evidence disappeared");
      }
      if(action.str("semantic") == "action")
        expect(action.flag("temporal_context_only", false), "nearby action was presented as causal evidence");
    }
    expect(late && shared, "early normal applications crowded out late selected evidence");
  }
  const auto tiny = compact_experience_feedback_trial(trial, full, selected, 256);
  expect(tiny.dump().size() <= 256 && tiny.str("verdict") == "passed", "tiny budget lost the legacy summary fallback");
  auto alias = public_alias_fixture();
  const auto refs = proto<trajectory::evolution::v1::RuntimePolicy>(alias["policy"]).options().approved_experiences();
  for(auto& trace : traces.items()) for(auto& event : trace["events"].items()) {
    auto& attrs = event["attributes"];
    for(auto* sources : {&attrs["decision"]["experiences"], &attrs["receipt"]["experiences"]}) {
      if(!sources->array()) continue;
      for(auto& ref : sources->items()) for(const auto& approved : refs)
        if(approved.memory_id() == (ref.str("memory_id") == "shared" ? "shared-copy" : ref.str("memory_id")))
          ref = json(approved);
    }
  }
  *trial.mutable_evidence() = proto<google::protobuf::Struct>(Json{{"traces", traces}, {"output", "final"}});
  const auto aliased_full = feedback_trial(trial, Json::Object{});
  const auto canonical = compact_experience_feedback_trial(trial, aliased_full, Json::Array{"experience:private"}, 8192, alias);
  expect(canonical["evidence"]["runs"].items().size() == 2, "approved public receipt lost its private component");
  alias["policy"]["snapshot"]["cross_relations"].items().front()["shared_content_revision"] = "stale-revision";
  const auto stale = compact_experience_feedback_trial(trial, aliased_full, Json::Array{"experience:private"}, 8192, alias);
  expect(stale["evidence"]["runs"].items().size() == 1, "stale public revision borrowed a private application");
  for(auto& trace : traces.items()) {
    auto& rows = trace["events"].items();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& event) {
      return event.str("kind") == "advice.applied";
    }), rows.end());
  }
  *trial.mutable_evidence() = proto<google::protobuf::Struct>(Json{{"traces", traces}});
  auto compact = compact_experience_feedback_trial(trial, feedback_trial(trial, Json::Object{}), selected, 4096);
  expect(compact["evidence"]["runs"].null(), "a proposed query was promoted to actual application");
  trial.clear_evidence();
  compact = compact_experience_feedback_trial(trial, feedback_trial(trial, Json::Object{}), selected, 4096);
  expect(compact.str("verdict") == "passed" && compact["evidence"]["runs"].null(), "missing trace fabricated evidence");

  // 异步 ACK 不含最终结果；真实结果只可从本次批次的显式因果链取回。
  events = Json::Array{};
  auto scheduled = [&](const std::string& id, const std::string& step = "batch-step", bool executed = true) {
    return Json{{"event_id", id}, {"kind", "action.end"}, {"step_id", step}, {"actor_id", "worker"},
        {"execution_id", "branch"}, {"operation", "render"}, {"status", "completed"},
        {"output", Json{{"status", "succeeded"}, {"files", Json::Array{"output.bin"}},
            {"report", std::string(360, 'x') + "ACTUAL_RESULT"}}},
        {"attributes", Json{{"type", "schedule.action"}, {"executed", executed}}}};
  };
  events.push(scheduled("settled-result"));
  auto failure = scheduled("failed-result"); failure["status"] = "failed";
  failure["output"] = Json{{"status", "failed"}, {"error", "STRUCTURED_FAILURE"}};
  events.push(failure);
  events.push(scheduled("other-batch-result", "other-step"));
  events.push(scheduled("unexecuted-result", "batch-step", false));
  auto cancelled = scheduled("cancelled-result"); cancelled["status"] = "cancelled"; events.push(cancelled);
  auto batch = Json{{"event_id", "batch-end"}, {"kind", "step.end"}, {"step_id", "batch-step"},
      {"actor_id", "worker"}, {"execution_id", "branch"}, {"status", "completed"},
      {"caused_by", Json::Array{"settled-result", "failed-result", "other-batch-result", "unexecuted-result", "cancelled-result", "future-result"}},
      {"attributes", Json{{"extensions", Json{{"point_reference", Json{{"key", "batch"}, {"parent", "dispatch"}, {"run", "run"}}}}}}}};
  events.push(batch);
  tool("dispatch-ack");
  auto& ack = events.items().back(); ack["output"] = "accepted";
  ack["caused_by"] = Json::Array{"batch-end"}; ack["attributes"]["delegates_schedule"] = true;
  ack["attributes"]["extensions"]["point_reference"]["key"] = "dispatch";
  ack["attributes"]["extensions"]["point_reference"]["run"] = "run";
  model("async-model", "private");
  events.push(scheduled("future-result"));
  auto async_trial = [&](const Json& rows) {
    auto observed = trial;
    *observed.mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"identity", Json{{"run_id", "async"}}}, {"events", rows}}}});
    return compact_experience_feedback_trial(observed, feedback_trial(observed, Json::Object{}), selected, 8192);
  };
  compact = async_trial(events);
  std::set<std::string> results;
  for(const auto& action : compact["evidence"]["runs"].items().front()["actions"].items()) {
    if(!action.has("dispatch_result_of")) continue;
    results.insert(action.str("event_id"));
    expect(action.str("dispatch_result_of") == "dispatch-ack" && action.str("schedule_end_event_id") == "batch-end",
        "scheduled result lost its explicit dispatch provenance");
    if(action.str("event_id") == "settled-result")
      expect(action["output"]["report"].text().find("ACTUAL_RESULT") != std::string::npos
          && action["output"]["files"].items().size() == 1, "typed async result was flattened into an unhelpful prefix");
  }
  expect(results == std::set<std::string>{"settled-result", "failed-result"}, "async evidence included unrelated or unsettled results");
  expect(compact.dump().size() <= 8192, "async results expanded the L1 input budget");
  for(auto& event : events.items()) if(event.str("event_id") == "batch-end")
    event["attributes"]["extensions"]["point_reference"]["parent"] = "other-dispatch";
  compact = async_trial(events);
  for(const auto& action : compact["evidence"]["runs"].items().front()["actions"].items())
    expect(!action.has("dispatch_result_of"), "a sibling batch was attributed through an unrelated causal reference");
}

void unknown_loop_counter() {
  Report report; auto* trial = report.add_trials();
  trial->add_checks()->set_status("passed");
  Json query{{"boundary", "loop"}, {"iteration", "1"}, {"pending_actions", true}};
  Json decision{{"query", query}, {"progress", Json{{"decision", "continue"}}}};
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", Json::Array{
      Json{{"kind", "custom"}, {"step_id", "step"}, {"attributes", Json{{"type", "policy.query"}, {"decision", decision}}}},
      Json{{"kind", "loop.decision"}, {"step_id", "step"}}}}}}});
  const auto feedback = control_feedback_trial(*trial, Json(Json::Array{"loop_rules"}), Json::Object{});
  const auto& control = feedback["evidence"]["runs"].items().front()["actions"].items().front()["loop_control"];
  expect(control["observed_no_progress"].null(), "unknown loop counter was converted into measured zero");
  expect(control.flag("pending_actions", false), "pending actions disappeared from reflection");
}

void append_reviewed_group(Json& trial, Json& raw, const std::string& run_id,
    const std::string& execution_id, const std::string& loop_id,
    const std::vector<std::string>& decisions, bool numeric_iteration = false,
    bool omit_last_loop = false) {
  Json* run = nullptr;
  for(auto& item : trial["evidence"]["runs"].items()) if(item.str("run_id") == run_id) run = &item;
  if(!run) {
    trial["evidence"]["runs"].push(Json{{"run_id", run_id}, {"actions", Json::Array{}}});
    run = &trial["evidence"]["runs"].items().back();
  }
  for(std::size_t index = 0; index < decisions.size(); ++index) {
    const auto iteration = index + 1;
    const Json wire_iteration = numeric_iteration ? Json(iteration) : Json(std::to_string(iteration));
    Json action{{"kind", "action.end"}, {"actor_id", "worker"}, {"execution_id", execution_id},
        {"loop_id", loop_id}, {"iteration", wire_iteration}, {"semantic", "action"}, {"executed", true}};
    const auto id = iteration_identity(*run, action);
    raw["loop_review"]["iteration_reviews"].push(Json{{"iteration_id", id},
        {"decision", decisions[index]}, {"reason", "reviewed iteration " + std::to_string(iteration)}});
    if(!omit_last_loop || index + 1 != decisions.size())
      (*run)["actions"].push(Json{{"kind", "loop.decision"}, {"actor_id", "worker"},
          {"execution_id", execution_id}, {"loop_id", loop_id}, {"iteration", wire_iteration},
          {"loop_control", Json{{"target_scope", "assistant"}, {"iteration", iteration},
              {"pending_actions", false}, {"proposed_decision", "continue"}}}});
    (*run)["actions"].push(std::move(action));
  }
  raw["loop_review"]["observed_iterations"] = raw["loop_review"]["iteration_reviews"].items().size();
}

void reviewed_finalization_cap_evidence() {
  Json trial{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(trial, raw, "run", "execution", "loop", {"keep", "waste", "waste", "keep"});
  for(auto& action : trial["evidence"]["runs"].items().front()["actions"].items())
    if(action["loop_control"].object()) action["loop_control"]["can_finalize"] = true;
  const Json context{{"execution_id", "execution"}, {"loop_id", "loop"}, {"iteration", "4"}, {"action_id", "answer"}};
  auto start = context; start["kind"] = "action.start";
  start["attributes"] = Json{{"semantic", "model"}};
  auto end = context; end["kind"] = "action.end"; end["status"] = "completed";
  end["attributes"] = Json{{"semantic", "model"}, {"executed", true}};
  end["output"] = Json{{"choices", Json::Array{Json{{"finish_reason", "stop"},
      {"message", Json{{"content", "Final report from the completed results."}}}}}}};
  raw["trace"] = Json{{"identity", Json{{"run_id", "run"}}},
      {"events", Json::Array{start, end, Json{{"kind", "run.end"}, {"status", "completed"}}}}};
  auto marked = trial; bind_iteration_assessments(marked, raw);
  bool offered = false;
  for(const auto& action : marked["evidence"]["runs"].items().front()["actions"].items()) {
    const auto& control = action["loop_control"];
    if(!control.object()) continue;
    if(control.num("iteration", 0) == 3)
      offered = control.num("reviewed_waste_after", 99) == 1 && control.flag("require_finalization", false);
    if(control.num("iteration", 0) == 4)
      expect(!control.has("reviewed_waste_after"), "final delivery itself was offered as removable waste");
  }
  expect(offered, "a verified answer-only suffix lost its conditional finalization opportunity");
  auto canonical = raw;
  canonical["trace"]["events"].items().back().fields().erase("status");
  canonical["trace"]["events"].items().back()["outcome"] = Json{{"execution_status", "completed"}};
  auto canonical_view = trial; bind_iteration_assessments(canonical_view, canonical);
  expect(canonical_view["evidence"]["runs"].items().front()["actions"].items()[4]["loop_control"]
      .flag("require_finalization", false), "canonical run.end outcome lost real finalization evidence");
  const auto denied = [&](Json evidence, Json view) {
    bind_iteration_assessments(view, evidence);
    for(const auto& action : view["evidence"]["runs"].items().front()["actions"].items())
      if(action["loop_control"].has("reviewed_waste_after")) return false;
    return true;
  };
  auto unsupported = trial;
  for(auto& action : unsupported["evidence"]["runs"].items().front()["actions"].items())
    if(action["loop_control"].object()) action["loop_control"].fields().erase("can_finalize");
  expect(denied(raw, unsupported), "an unknown host capability authorized a finalization-dependent cap");
  auto invalid = raw;
  invalid["trace"]["events"].items()[1]["output"]["choices"].items()[0]["message"]["tool_calls"] = Json::Array{Json::Object{}};
  expect(denied(invalid, trial), "a tool request was mistaken for final text");
  invalid = raw; invalid["trace"]["events"].items()[1]["attributes"]["semantic"] = "action";
  expect(denied(invalid, trial), "required tool work was removed with finalization");
  invalid = raw; invalid["trace"]["events"].items().erase(invalid["trace"]["events"].items().begin() + 1);
  expect(denied(invalid, trial), "an unfinished model call authorized truncation");
  invalid = raw; invalid["trace"]["events"].items().pop_back();
  expect(denied(invalid, trial), "an unfinished run authorized truncation");
  invalid = raw; invalid["trace"]["identity"]["run_id"] = "another-run";
  expect(denied(invalid, trial), "final-answer evidence crossed run boundaries");
  invalid = raw; auto tool = start; tool["action_id"] = "required-tool";
  tool["attributes"]["semantic"] = "action"; invalid["trace"]["events"].push(tool);
  expect(denied(invalid, trial), "an extra required action in the final iteration was ignored");
}

void required_iteration_floor_tests() {
  auto packet = [](Json view) {
    return Json{{"selected_components", Json::Array{"loop_rules"}},
        {"components", Json{{"loop_rules", Json::Array{}}}},
        {"controls", Json{{"aggressive_controls", true}, {"limits", Json{{"min_iterations", 1}}},
            {"loop_primitives", Json::Array{Json{{"primitive_id", "bounded_iteration_cap"}}}},
            {"base_loop_rules", Json::Array{Json{{"target_scope", "assistant"}, {"max_iterations", 12}}}}}},
        {"feedback", Json::Array{Json{{"trial", std::move(view)}}}}};
  };
  auto has_cap = [](const Json& input) {
    const auto prepared = observed_operation_bindings(input, 65536);
    const auto& bindings = prepared["controls"]["editable_bindings"];
    if(bindings.array()) for(const auto& binding : bindings.items())
      if(binding.str("primitive_id") == "bounded_iteration_cap") return true;
    return false;
  };
  Json view{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(view, raw, "run", "required", "needed", {"keep", "keep", "keep", "keep"});
  append_reviewed_group(view, raw, "run", "redundant", "repeat", {"waste", "waste", "waste", "waste"});
  bind_iteration_assessments(view, raw);
  auto input = packet(view);
  expect(!has_cap(input), "a fully redundant execution authorized truncating the necessary same-actor instance");
  auto hidden = view;
  auto& hidden_rows = hidden["evidence"]["runs"].items()[0]["actions"].items();
  hidden_rows.erase(std::remove_if(hidden_rows.begin(), hidden_rows.end(), [](const auto& action) {
    return action.str("execution_id") == "required" && action.str("kind") == "loop.decision"
        && action["loop_control"].num("iteration", 0) == 4;
  }), hidden_rows.end());
  bind_iteration_assessments(hidden, raw);
  expect(!has_cap(packet(hidden)), "a necessary action without its loop query lost the same-actor keep floor");
  auto& rows = input["feedback"].items()[0]["trial"]["evidence"]["runs"].items()[0]["actions"];
  rows.items().erase(rows.items().begin(), rows.items().begin() + 8);
  expect(rows.items()[0]["loop_control"].num("required_iteration_floor", 0) == 4 && !has_cap(input),
      "display clipping erased the complete same-actor keep floor");

  Json other{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json other_raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(other, other_raw, "another-run", "repeat", "repeat", {"waste", "waste", "waste", "waste"});
  bind_iteration_assessments(other, other_raw);
  input = packet(other);
  merge_loop_requirements(input["controls"]["loop_requirements"], view["evidence"]["loop_requirements"]);
  expect(!has_cap(input), "a dropped training feedback row erased another execution's required work");

  Json finish_view{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json finish_raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(finish_view, finish_raw, "run", "required", "needed", {"keep", "keep", "keep"});
  append_reviewed_group(finish_view, finish_raw, "run", "redundant", "repeat", {"keep", "waste", "waste", "keep"});
  for(auto& action : finish_view["evidence"]["runs"].items()[0]["actions"].items())
    if(action["loop_control"].object()) action["loop_control"]["can_finalize"] = true;
  Json events = Json::Array{};
  for(const auto& ending : {std::make_pair("required", 3), std::make_pair("redundant", 4)}) {
    const std::string execution = ending.first, loop = execution == "required" ? "needed" : "repeat";
    Json context{{"execution_id", execution}, {"loop_id", loop}, {"iteration", std::to_string(ending.second)},
        {"action_id", execution + "-answer"}};
    auto start = context; start["kind"] = "action.start"; start["attributes"] = Json{{"semantic", "model"}};
    auto end = context; end["kind"] = "action.end"; end["status"] = "completed";
    end["attributes"] = Json{{"semantic", "model"}, {"executed", true}};
    end["output"] = Json{{"choices", Json::Array{Json{{"finish_reason", "stop"},
        {"message", Json{{"content", "Observed results."}}}}}}};
    events.push(start); events.push(end);
  }
  events.push(Json{{"kind", "run.end"}, {"outcome", Json{{"execution_status", "completed"}}}});
  finish_raw["trace"] = Json{{"identity", Json{{"run_id", "run"}}}, {"events", events}};
  auto marked = finish_view; bind_iteration_assessments(marked, finish_raw);
  auto prepared = observed_operation_bindings(packet(marked), 65536);
  const auto& binding = prepared["controls"]["editable_bindings"].items().front();
  expect(binding.flag("require_finalization", false) && binding.num("required_iteration_floor", 0) == 2
      && binding["budget"].num("min", 0) == 2 && binding["budget"].num("max", 0) == 2,
      "safe answer-only replacement was blocked by another execution's final answer");
  auto tool = events.items()[0]; tool["action_id"] = "required-tool";
  tool["attributes"]["semantic"] = "action"; finish_raw["trace"]["events"].push(tool);
  marked = finish_view; bind_iteration_assessments(marked, finish_raw);
  expect(!has_cap(packet(marked)), "finalization exemption removed necessary tool work from the floor");
}

Json reviewed_input(Json trial, bool aggressive = true) {
  return Json{{"controls", Json{{"aggressive_controls", aggressive}, {"base_loop_rules", Json::Array{}}}},
      {"feedback", Json::Array{Json{{"trial", std::move(trial)}}}}};
}

void reviewed_iteration_cap_evidence() {
  Search search; search.controls = Json{{"min_iterations", 1}, {"min_no_progress", 1}};
  auto fixture = [&](const std::vector<std::string>& decisions, bool numeric = false, bool omit_last_loop = false) {
    Json trial{{"evidence", Json{{"runs", Json::Array{}}}}};
    Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
    append_reviewed_group(trial, raw, "run", "execution", "loop", decisions, numeric, omit_last_loop);
    bind_iteration_assessments(trial, raw); return trial;
  };

  auto interrupted = fixture({"keep", "waste", "keep"});
  expect(!search.control_evidence(reviewed_input(interrupted), "loop_rules"),
      "a middle waste iteration authorized a global iteration cap");
  for(const auto& action : interrupted["evidence"]["runs"].items().front()["actions"].items())
    expect(action.has("iteration_assessment") && !action["loop_control"].has("reviewed_waste_after"),
        "complete review was lost or a non-suffix waste was promoted");

  auto suffix = fixture({"keep", "waste", "waste"}, true);
  expect(search.control_evidence(reviewed_input(suffix), "loop_rules"),
      "a complete reviewed waste suffix did not authorize a cap opportunity");
  bool cap_one = false;
  for(const auto& action : suffix["evidence"]["runs"].items().front()["actions"].items())
    cap_one |= action["loop_control"].has("reviewed_waste_after")
        && action["loop_control"].num("reviewed_waste_after", 99) == 1;
  expect(cap_one, "reviewed suffix boundary was not preserved");

  auto all_waste = fixture({"waste", "waste", "waste", "waste", "waste", "waste", "waste"});
  expect(search.control_evidence(reviewed_input(all_waste), "loop_rules"),
      "all-waste execution lost its minimum-safe cap opportunity");
  bool cap_zero = false;
  for(const auto& action : all_waste["evidence"]["runs"].items().front()["actions"].items())
    cap_zero |= action["loop_control"].has("reviewed_waste_after")
        && action["loop_control"].num("reviewed_waste_after", 99) == 0;
  expect(cap_zero, "zero reviewed prefix was confused with missing evidence");
  expect(!search.control_evidence(reviewed_input(fixture({"waste"})), "loop_rules"),
      "one reviewed iteration bypassed the configured minimum cap");

  auto hidden_keep = fixture({"keep", "waste", "keep"}, false, true);
  expect(!search.control_evidence(reviewed_input(hidden_keep), "loop_rules"),
      "an executed keep without a loop query was ignored by suffix review");

  Json isolated{{"evidence", Json{{"runs", Json::Array{}}}}};
  Json isolated_raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
  append_reviewed_group(isolated, isolated_raw, "run-a", "execution-a", "loop", {"keep", "waste", "waste"});
  append_reviewed_group(isolated, isolated_raw, "run-a", "execution-b", "loop", {"keep", "waste", "keep"});
  append_reviewed_group(isolated, isolated_raw, "run-b", "execution-a", "loop", {"waste"});
  bind_iteration_assessments(isolated, isolated_raw);
  for(const auto& run : isolated["evidence"]["runs"].items()) for(const auto& action : run["actions"].items()) {
    if(action.str("kind") != "loop.decision") continue;
    const bool suffix = run.str("run_id") == "run-a" && action.str("execution_id") == "execution-a";
    const bool single = run.str("run_id") == "run-b";
    expect(action["loop_control"].has("reviewed_waste_after") == (suffix || single)
        && (!suffix || action["loop_control"].num("reviewed_waste_after", 99) == 1)
        && (!single || action["loop_control"].num("reviewed_waste_after", 99) == 0),
        "reviewed suffix crossed a run or execution boundary");
  }
  Json run_b{{"evidence", Json{{"runs", Json::Array{isolated["evidence"]["runs"].items().back()}}}}};
  expect(!search.control_evidence(reviewed_input(run_b), "loop_rules"),
      "a single all-waste iteration bypassed the configured minimum in another run");

  auto mismatch = fixture({"keep", "waste", "waste"});
  Json stale_raw{{"loop_review", Json{{"status", "complete"}, {"observed_iterations", 1},
      {"iteration_reviews", Json::Array{Json{{"iteration_id", "unknown"}, {"decision", "waste"}, {"reason", "stale"}}}}}}};
  Json clean{{"evidence", mismatch["evidence"]}};
  for(auto& action : clean["evidence"]["runs"].items().front()["actions"].items()) {
    action.fields().erase("iteration_assessment");
    if(action["loop_control"].object()) action["loop_control"].fields().erase("reviewed_waste_after");
  }
  bind_iteration_assessments(clean, stale_raw);
  expect(!search.control_evidence(reviewed_input(clean), "loop_rules"),
      "unknown review hash manufactured an iteration-cap opportunity");
  auto uncertain_raw = stale_raw; uncertain_raw["loop_review"]["status"] = "uncertain";
  bind_iteration_assessments(clean, uncertain_raw);
  expect(!search.control_evidence(reviewed_input(clean), "loop_rules"),
      "uncertain review manufactured an iteration-cap opportunity");

  const Json run{{"run_id", "canonical"}};
  Json numeric{{"execution_id", "execution"}, {"loop_id", "loop"}, {"iteration", 2}};
  Json textual = numeric; textual["iteration"] = "2";
  expect(iteration_identity(run, numeric) == iteration_identity(run, textual),
      "numeric and decimal-string iterations produced different canonical identities");

  const Json structural{{"controls", Json{{"aggressive_controls", false}, {"base_loop_rules", Json::Array{}}}},
      {"feedback", Json::Array{Json{{"trial", Json{{"evidence", Json{{"runs", Json::Array{
          Json{{"run_id", "run"}, {"actions", Json::Array{Json{{"kind", "loop.decision"},
              {"loop_control", Json{{"pending_actions", false}, {"proposed_decision", "continue"},
                  {"observed_no_progress", 1}}}}}}}}}}}}}}}}};
  expect(search.control_evidence(structural, "loop_rules"),
      "structural no-progress evidence incorrectly required a model review");
}

void action_feedback_keeps_iteration_assessment() {
  Json events = Json::Array{}, reviews = Json::Array{};
  const Json run{{"run_id", "run"}};
  for(std::size_t iteration = 1; iteration <= 3; ++iteration) {
    const auto id = "action-" + std::to_string(iteration);
    Json query{{"boundary", "actions"}, {"actions", Json::Array{Json{{"action_id", id},
        {"operation", "tool"}, {"arguments", Json::Object{}}}}},
        {"execution_id", "execution"}, {"advice_query", Json{{"target_scope", "assistant"}}}};
    events.push(Json{{"kind", "custom"}, {"step_id", id},
        {"attributes", Json{{"type", "policy.query"}, {"decision", Json{{"query", query},
            {"actions", Json::Array{Json{{"action_id", id}, {"decision", "allow"}}}}}}}}});
    Json action{{"event_id", "end-" + id}, {"kind", "action.end"}, {"step_id", id},
        {"action_id", id}, {"actor_id", "worker"}, {"execution_id", "execution"},
        {"loop_id", "loop"}, {"iteration", std::to_string(iteration)}, {"operation", "tool"},
        {"status", "completed"}, {"attributes", Json{{"semantic", "action"}, {"executed", true}}}};
    events.push(action);
    reviews.push(Json{{"iteration_id", iteration_identity(run, action)}, {"decision", iteration == 1 ? "keep" : "waste"},
        {"reason", std::string(4096, 'r')}});
  }
  trajectory::evolution::dataset::v1::EvaluationTrial trial;
  trial.set_verdict("passed"); trial.set_execution_status("completed");
  *trial.mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"identity", run}, {"events", events}}},
      {"loop_review", Json{{"status", "complete"}, {"observed_iterations", 3}, {"iteration_reviews", reviews}}}});
  const auto compact = control_feedback_trial(trial, Json::Array{"action_rules"}, Json::Object{});
  expect(!compact["evidence"].has("loop_review") && compact.dump().size() <= 8192,
      "action-only feedback retained the full loop review or exceeded its compact budget");
  const auto& actions = compact["evidence"]["runs"].items().front()["actions"].items();
  expect(actions.size() == 3 && std::all_of(actions.begin(), actions.end(), [](const auto& action) {
    return action["iteration_assessment"].object()
        && action["iteration_assessment"].str("reason").size() <= 524;
  }), "action-only feedback lost the compact semantic iteration assessment");
}

void incremental_control_coverage() {
  const Json inherited{{"rules", Json{{"old-schedule", Json{{"queries", 8}, {"applied", 0}}}}}};
  const Json training{{"rules", Json{{"old-schedule", Json{{"queries", 3}, {"applied", 0}}},
      {"new-loop", Json{{"queries", 2}, {"triggered", 1}, {"applied", 1}}}}}};
  auto coverage = merge_control_coverage(training, inherited);
  expect(control_changes_observed(coverage, inherited), "unchanged unobserved rule blocks a real new control");
  expect(!coverage.flag("complete", true), "incomplete whole-policy coverage was hidden");
  auto missed = coverage; missed["rules"]["new-loop"]["applied"] = 0;
  expect(!control_changes_observed(missed, inherited), "new unobserved rule was accepted");
  // 改写后的规则 ID 不同，旧规则的证据不能替它证明生效。
  auto previous = inherited; previous["rules"]["old-schedule"]["applied"] = 9;
  Json changed{{"rules", Json{{"changed-schedule", Json{{"queries", 3}, {"applied", 0}}}}}};
  expect(!control_changes_observed(merge_control_coverage(changed, previous), previous),
      "changed rule borrowed an old rule's receipt");
  Json probe{{"rules", Json{{"old-schedule", Json{{"queries", 4}, {"applied", 0}}},
      {"new-loop", Json{{"queries", 0}, {"applied", 0}}}, {"missed-rule", Json{{"queries", 2}, {"applied", 0}}}}}};
  coverage = merge_control_coverage(probe, coverage);
  expect(coverage["rules"]["new-loop"].num("applied", 0) == 1, "validation erased training receipts");
  expect(!control_changes_observed(coverage, inherited), "another missed new control was ignored");
  // 默认效果优先：旧规则未命中不能压过已验证的收益；严格覆盖仍可显式启用。
  Search search;
  search.tree = Json{{"nodes", Json::Array{
      Json{{"status", "accepted"}, {"optimization", Json{{"passed", false}, {"score", 0}}}},
      Json{{"status", "accepted"}, {"optimization", Json{{"passed", true}, {"score", 1}}},
          {"control_coverage", coverage}}}}};
  search.tree["nodes"].items()[0]["validation"] = Json{{"task", Json{{"quality", 1}, {"fitness", 0.5}}}};
  search.tree["nodes"].items()[1]["parents"] = Json::Array{0};
  search.tree["nodes"].items()[1]["validation"] = Json{{"task", Json{{"quality", 1}, {"fitness", 0.8}}}};
  search.tree["nodes"].items()[1]["validation_applied_changes"] = Json{{"0", Json{{"task", Json{{"loop", 1}}}}}};
  expect(search.winner() == 1, "unused inherited rule suppressed a better candidate");
  search.gate = Json{{"require_level_coverage", false}};
  expect(search.winner() == 1, "explicit effect mode still requires coverage");
  search.gate["require_level_coverage"] = true;
  expect(search.winner() == 0, "explicit strict coverage was bypassed");
  for(auto& rule : coverage["rules"].fields()) rule.second["applied"] = 1;
  search.tree["nodes"].items()[1]["control_coverage"] = merge_control_coverage(coverage, Json{});
  expect(search.winner() == 0, "strict level coverage was bypassed by rule coverage");
  search.tree["nodes"].items()[1]["control_coverage"]["level_complete"] = true;
  expect(search.winner() == 1, "fully observed better candidate cannot win");
  search.gate = Json::Object{};
  auto& nodes = search.tree["nodes"].items();
  nodes[0]["control_coverage"] = Json{{"complete", true}, {"level_complete", true}};
  nodes[1]["control_coverage"] = Json{{"complete", false}, {"level_complete", false}};
  nodes[0]["optimization"] = Json{{"feasible", false}, {"passed", false}, {"score", 0.45}, {"robustness", 0.2}};
  nodes[1]["optimization"] = Json{{"feasible", true}, {"passed", true}, {"score", 1}, {"robustness", 0.4}};
  expect(search.winner() == 1, "covered regression beat a feasible improvement");
  auto best = nodes[1]; best["optimization"]["robustness"] = 0.67;
  nodes.push_back(best);
  expect(search.winner() == 2, "better measured robustness was ignored");
  nodes[2]["status"] = "unobserved_changes";
  expect(search.winner() == 1, "candidate without a real behavioral change won");
  nodes[1]["status"] = "inconclusive";
  expect(search.winner() == 0, "unscored candidate won on apparent performance");
  // 可选地只读回放已有实验的选优，不请求模型、不修改原始树与评测报告。
  if(const auto* path = std::getenv("GEPA_SELECTION_REPLAY_TREE")) {
    Search replay;
    replay.tree = Json::parse(read(path, 16777216));
    replay.gate = replay.tree["optimization_contract"];
    std::cout << "Recorded tree winner=" << replay.tree.num("winner", 0)
              << " effect-first winner=" << replay.winner() << '\n';
  }
}

void cancelled_search_clock() {
  Search search;
  search.config = Json{{"timeout_ms", 100}};
  search.tree = Json{{"started_ms", 1000}, {"finished_ms", 1040},
      {"status", "cancelled"}, {"error", "gepa_cancelled"}};
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
  search.tree["finished_ms"] = 5080; search.tree["status"] = "error";
  search.resume_clock(9000);
  expect(search.deadline() == 5100, "ordinary error silently extended the budget");
  search.tree["status"] = "cancelled"; search.resume_clock(5070);
  expect(search.deadline() == 5100, "backward clock produced a negative pause");
  search.tree = Json{{"started_ms", now() - 5000}, {"paused_ms", 5000}};
  search.config = Json{{"timeout_ms", 1000}};
  search.check();
  search.tree["paused_ms"] = 0;
  bool expired = false;
  try { search.check(); } catch(const Error& error) { expired = std::string(error.what()) == "gepa_time_budget_exhausted"; }
  expect(expired, "active execution timeout was disabled");
}

void context_version_is_not_behavior() {
  namespace te = trajectory::evolution;
  agentflow::trajectory::evaluation::EvolutionMemoryStore memory;
  memory.set_schema_version(1); memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1"); memory.set_revision(1);
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"}, {"user_id", "user"},
      {"conversation_id", "conversation"}, {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
      {"max_policy_entries", 64}, {"max_advice_items", 3}, {"max_advice_bytes", 4096},
      {"max_query_bytes", 16384}, {"max_memory_bytes", 1048576}, {"max_snapshot_bytes", 262144},
      {"max_match_terms", 128}, {"min_matched_terms", 2}});
  auto options = proto<te::v1::RuntimeOptions>(Json{{"requested_level", "L2"}, {"fallback", "strict"},
      {"host_capabilities", Json::Array{"context.request", "action.filter", "schedule.dispatch"}},
      {"allowed_users", Json::Array{"user"}}, {"allowed_conversations", Json::Array{"conversation"}},
      {"experience_template", "{{experience}}"}, {"max_context_bytes", 4096},
      {"schedule_rules", Json::Array{Json{{"rule_id", "ordered"}, {"target_scope", "assistant"},
          {"before_operation", "prepare"}, {"after_operation", "render"}, {"require_success", true},
          {"reason", "wait for preparation"}, {"task_terms", Json::Array{"prepare"}}}}}});
  const auto parent = te::compile_runtime_policy(te::compile_shadow_policy(memory, identity, limits), options);
  const auto query = proto<te::v1::RuntimeQuery>(Json{{"boundary", "llm"}, {"execution_id", "assistant"},
      {"advice_query", Json{{"identity", json(identity)}, {"target_scope", "assistant"},
          {"query_id", "query"}, {"decision_id", "decision"}, {"task", "prepare then render"}}}});
  const auto original = json(te::PolicyRuntime(parent).decide(query));
  expect(!original.str("context").empty(), "fixture did not generate scheduling context");
  options.set_max_context_bytes(8192);
  const auto child = te::compile_runtime_policy(parent.snapshot(), options);
  auto actual = json(te::PolicyRuntime(child).decide(query));
  expect(actual.str("policy_id") != original.str("policy_id"), "fixture must have different policy IDs");
  expect(actual.str("context") == original.str("context"), "metadata changed the model-visible context");
  Search search;
  Report observed;
  auto changes = [&](const Json& decision, bool executed = true) {
    Report report; auto* trial = report.add_trials(); trial->set_variant_id("candidate"); trial->set_task_id("task");
    Json events = Json::Array{
        Json{{"kind", "custom"}, {"step_id", "step"}, {"attributes", Json{{"type", "policy.query"}, {"decision", decision}}}},
        Json{{"kind", "advice.applied"}, {"application", Json{{"query_id", "query"},
            {"status", decision.str("status") == "no_change" ? "rejected" : "applied"}}}}};
    if(executed) events.push(Json{{"kind", "action.end"}, {"step_id", "step"}, {"attributes", Json{{"executed", true}}}});
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}}});
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
  actual["context"] = ""; actual["status"] = "no_change";
  expect(changes(actual)["task"].num("llm", 0) == 1, "executed context removal was discarded");
  expect(changes(actual, false).fields().empty(), "unexecuted context removal was counted");

  // 旧检查点只更新派生归因；报告缺失或哈希不符时不能猜测结果，更不能偷偷重新跑题。
  changes(json(te::PolicyRuntime(child).decide(query)));
  char directory[] = "/tmp/trajectory-gepa-evidence-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create evidence fixture");
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{directory};
  search.root = directory; search.val = {"task"};
  const Json artifact{{"schema_version", 1}, {"agent", Json{{"revision", "fixture"}}},
      {"user_id", "user"}, {"level", "L2"}, {"policy", json(parent)}};
  write(search.root / "versions/0.json", artifact.dump());
  const auto report = json(observed).dump();
  const auto report_path = search.root / "evaluations/cached/attempt-1/report.json";
  write(report_path, report);
  const Json values{{"task", Json{{"fitness", 1}, {"quality", 1}}}};
  search.tree = Json{{"active", nullptr}, {"batch", nullptr}, {"model_calls", 4}, {"trials_used", 12},
      {"evaluations", Json{{"cached", Json{{"version", "child"}, {"split", "train"}, {"attempt", 1},
          {"task_ids", Json::Array{"task"}}, {"report_hash", hash(report)}}}}},
      {"nodes", Json::Array{Json{{"id", 0}, {"parent", nullptr}, {"parents", Json::Array{}},
          {"depth", 0}, {"status", "accepted"}, {"version", te::agent_version_id(artifact.dump())}, {"validation", values}},
        Json{{"id", 1}, {"parent", 0}, {"parents", Json::Array{0}}, {"depth", 1}, {"status", "accepted"},
          {"version", "child"}, {"tasks", Json::Array{"task"}}, {"validation", values},
          {"screening", Json{{"applied_changes", Json{{"task", Json{{"llm", 1}}}}}}}}}}};
  search.tree["batch"] = Json::Object{}; search.refresh_change_evidence();
  expect(!search.tree.has("change_evidence_revision"), "migration changed an active batch");
  search.tree["batch"] = nullptr;
  const auto before = search.tree.dump();
  write(report_path, "{}");
  bool rejected = false;
  try { search.refresh_change_evidence(); } catch(const Error& error) { rejected = std::string(error.what()) == "gepa_report_changed"; }
  expect(rejected && search.tree.dump() == before, "altered report caused a partial migration");
  write(report_path, report); search.refresh_change_evidence();
  expect(search.tree["nodes"].items()[1].str("status") == "unobserved_changes", "stale acceptance survived migration");
  expect(search.tree["nodes"].items()[1]["validation"].dump() == values.dump(), "migration changed measured scores");
  expect(search.tree.num("model_calls", 0) == 4 && search.tree.num("trials_used", 0) == 12, "migration spent evaluation budget");
  expect(read(report_path) == report, "migration rewrote raw report");
  const auto migrated = search.tree.dump(); search.refresh_change_evidence();
  expect(search.tree.dump() == migrated, "migration is not idempotent");
  gepa::validate_nodes(search.tree["nodes"]);
}

void asynchronous_target_replay() {
  namespace te = trajectory::evolution;
  namespace ep = agentflow::trajectory::evaluation;
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1); memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1"); memory.set_revision(1);
  auto* entry = memory.add_entries(); entry->set_memory_id("async-experience");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto* candidate = entry->mutable_candidate();
  candidate->set_schema_version(1); candidate->set_candidate_id("async-candidate");
  candidate->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  candidate->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  candidate->set_target_scope("assistant"); candidate->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  candidate->set_trigger_pattern("重复生成失败"); candidate->set_observed_problem("同一目标重复失败");
  candidate->set_proposed_change("停止重复生成，独立分支继续。"); candidate->set_support_user_count(1);
  auto* support = candidate->add_support_evidence(); support->set_evidence_id("support");
  support->mutable_source()->set_user_id("user"); support->mutable_source()->set_dialogue_id("source");
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"}, {"user_id", "user"},
      {"conversation_id", "conversation"}, {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
      {"max_policy_entries", 64}, {"max_advice_items", 3}, {"max_advice_bytes", 4096},
      {"max_query_bytes", 16384}, {"max_memory_bytes", 1048576}, {"max_snapshot_bytes", 262144},
      {"max_match_terms", 128}, {"min_matched_terms", 2}});
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  expect(snapshot.experiences_size() == 1, "async experience fixture not eligible");
  auto options = proto<te::v1::RuntimeOptions>(Json{{"requested_level", "L2"}, {"fallback", "strict"},
      {"host_capabilities", Json::Array{"context.request", "action.filter", "schedule.dispatch"}},
      {"allowed_users", Json::Array{"user"}}, {"allowed_conversations", Json::Array{"conversation"}},
      {"experience_template", "{{experience}}"}, {"max_context_bytes", 4096}});
  auto* approval = options.add_approved_experiences(); approval->set_memory_id(snapshot.experiences(0).memory_id());
  approval->set_content_revision(snapshot.experiences(0).content_revision());
  const auto base = te::compile_runtime_policy(snapshot, options);
  auto* rule = options.add_action_rules(); rule->set_rule_id("target-budget"); rule->set_target_scope("assistant");
  rule->set_operation("generate"); rule->set_max_failed_attempts(2); rule->add_failure_key_arguments("output");
  rule->set_reason("同一目标已失败两次"); rule->set_match_mode("predicate"); *rule->mutable_source_experience() = *approval;
  const auto guarded = te::compile_runtime_policy(snapshot, options);
  auto query = proto<te::v1::RuntimeQuery>(Json{{"boundary", "actions"}, {"execution_id", "coordinator"},
      {"advice_query", Json{{"identity", json(identity)}, {"target_scope", "assistant"}, {"task", "generate A"},
          {"query_id", "guard-query"}, {"decision_id", "guard-decision"}}},
      {"actions", Json::Array{Json{{"action_id", "guard"}, {"operation", "generate"}, {"arguments", Json{{"output", "A"}}}}}}});
  const auto allowed = json(te::PolicyRuntime(base).decide(query));
  (*query.mutable_actions(0)->mutable_target_failures())["target-budget"] = 2;
  const auto rejected = json(te::PolicyRuntime(guarded).decide(query));
  expect(rejected["actions"].items()[0].str("decision") == "reject", "async fixture did not reject");
  auto start = [](const std::string& id) {
    return Json{{"kind", "action.start"}, {"action_id", id}, {"operation", "generate"}, {"input", Json{{"output", "A"}}},
        {"attributes", Json{{"type", "schedule.action"}, {"target_scope", "assistant"}}}};
  };
  auto end = [](const std::string& id) {
    return Json{{"kind", "action.end"}, {"action_id", id}, {"execution_id", "worker-" + id}, {"status", "failed"},
        {"attributes", Json{{"type", "schedule.action"}, {"executed", true}, {"observed_failure", true}}}};
  };
  auto report = [&](Json events, const Json& actual) {
    events.push(Json{{"kind", "custom"}, {"step_id", "guard-step"},
        {"attributes", Json{{"type", "policy.query"}, {"decision", actual}}}});
    events.push(Json{{"kind", "advice.applied"}, {"application", Json{{"query_id", "guard-query"},
        {"status", actual.str("status") == "no_change" ? "rejected" : "applied"}}}});
    if(actual.str("status") == "no_change") events.push(Json{{"kind", "action.end"}, {"step_id", "guard-step"},
        {"attributes", Json{{"executed", true}}}});
    Report result; auto* trial = result.add_trials(); trial->set_variant_id("candidate"); trial->set_task_id("task");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}}});
    return result;
  };
  Search search;
  const Json parent{{"policy", json(base)}}, child{{"policy", json(guarded)}};
  const Json sequential = Json::Array{start("one"), end("one"), start("two"), end("two")};
  auto feedback_report = report(sequential, allowed);
  feedback_report.mutable_trials(0)->add_checks()->set_status("passed");
  auto feedback_trace = json(feedback_report.trials(0).evidence());
  auto& last = feedback_trace["trace"]["events"].items().back();
  last["action_id"] = query.actions(0).action_id(); last["operation"] = "generate";
  *feedback_report.mutable_trials(0)->mutable_evidence() = proto<google::protobuf::Struct>(feedback_trace);
  const auto feedback = control_feedback_trial(feedback_report.trials(0), Json(Json::Array{"action_rules"}), Json::Object{});
  const auto& actions = feedback["evidence"]["runs"].items().front()["actions"].items();
  expect(actions.back()["action_control"].num("prior_operation_rounds", 0) == 2,
      "reflection lost settled asynchronous attempts");
  const auto preflight = search.control_preflight(parent, child, report(sequential, allowed));
  expect(preflight.flag("passed", false) && preflight["rules"].num("target-budget", 0) == 1,
      "async failures missing from GEPA preflight");
  expect(search.applied_changes(child, report(sequential, rejected)).fields().empty(),
      "identical parent rejection was counted as new effect");
  expect(search.applied_changes(child, report(sequential, allowed))["task"].num("actions", 0) == 1,
      "real removal of the parent restriction was lost");
  const auto removal = search.control_preflight(child, parent, report(sequential, rejected));
  expect(removal.flag("passed", false) && removal["removal_changes"]["task"].num("actions", 0) == 1,
      "deleting an applied asynchronous failure budget was blocked");
  std::vector<Json> incomplete{
      Json::Array{start("one"), start("two"), end("one"), end("two")},
      Json::Array{start("one"), end("one"), end("one")}};
  auto success = sequential; success.items()[3]["status"] = "completed";
  success.items()[3]["attributes"]["observed_failure"] = false; incomplete.push_back(success);
  auto unexecuted = sequential; unexecuted.items()[3]["attributes"]["executed"] = false; incomplete.push_back(unexecuted);
  auto other = sequential; other.items()[2]["input"]["output"] = "B"; incomplete.push_back(other);
  auto sibling = sequential; sibling.items()[2]["attributes"]["target_scope"] = "other-branch"; incomplete.push_back(sibling);
  auto old = sequential; old.items()[0]["attributes"].fields().erase("target_scope");
  old.items()[2]["attributes"].fields().erase("target_scope"); incomplete.push_back(old);
  for(const auto& events : incomplete)
    expect(!search.control_preflight(parent, child, report(events, allowed)).flag("passed", false),
        "concurrent, duplicate, successful, unexecuted, unrelated or old events invented retry evidence");
  auto separate_runs = report(sequential, allowed);
  auto evidence = json(separate_runs.trials(0).evidence()); auto trace = evidence["trace"];
  auto earlier = trace; earlier["identity"]["run_id"] = "previous-run"; earlier["events"] = sequential;
  trace["events"].items().erase(trace["events"].items().begin(), trace["events"].items().begin() + 4);
  *separate_runs.mutable_trials(0)->mutable_evidence() = proto<google::protobuf::Struct>(
      Json{{"traces", Json::Array{earlier, trace}}});
  expect(!search.control_preflight(parent, child, separate_runs).flag("passed", false), "another run granted retry evidence");

  // 终态错误只约束发生错误的 execution；同模型的独立分支仍有首次执行机会。
  auto terminal_options = base.options();
  auto* terminal = terminal_options.add_action_rules(); terminal->set_rule_id("terminal-budget");
  terminal->set_target_scope("assistant"); terminal->set_operation("generate"); terminal->set_max_failed_attempts(1);
  terminal->add_terminal_error_codes("UnsupportedModel"); terminal->add_error_scope_arguments("model");
  terminal->set_match_mode("predicate"); terminal->set_reason("Stop this execution after a terminal backend error");
  *terminal->mutable_source_experience() = base.options().approved_experiences(0);
  const auto terminal_policy = te::compile_runtime_policy(snapshot, terminal_options);
  const Json terminal_child{{"policy", json(terminal_policy)}};
  auto terminal_query = query; terminal_query.set_execution_id("branch-a");
  terminal_query.mutable_advice_query()->set_query_id("first-a-query");
  terminal_query.mutable_advice_query()->set_decision_id("first-a-decision");
  terminal_query.mutable_actions(0)->clear_target_failures(); terminal_query.mutable_actions(0)->set_action_id("first-a");
  *terminal_query.mutable_actions(0)->mutable_arguments() = proto<google::protobuf::Struct>(Json{{"model", "one"}, {"output", "a.png"}});
  const auto first_allowed = json(te::PolicyRuntime(base).decide(terminal_query));
  const Json failure_events = Json::Array{
      Json{{"kind", "custom"}, {"step_id", "first-a-step"}, {"execution_id", "branch-a"},
          {"attributes", Json{{"type", "policy.query"}, {"decision", first_allowed}}}},
      Json{{"kind", "action.end"}, {"step_id", "first-a-step"}, {"action_id", "first-a"}, {"execution_id", "branch-a"},
          {"operation", "generate"}, {"status", "failed"}, {"input", json(terminal_query.actions(0).arguments())},
          {"attributes", Json{{"executed", true}, {"observed_failure", true}, {"observed_error_code", "UnsupportedModel"}}}}};
  auto after_failure = [&](const std::string& branch, bool reject) {
    auto current = terminal_query; current.set_execution_id(branch); current.mutable_actions(0)->set_action_id("guard");
    current.mutable_advice_query()->set_query_id("guard-query"); current.mutable_advice_query()->set_decision_id("guard-decision");
    *current.mutable_actions(0)->mutable_arguments() = proto<google::protobuf::Struct>(Json{{"model", "one"}, {"output", "b.png"}});
    if(reject) (*current.mutable_actions(0)->mutable_target_failures())["terminal-budget"] = 1;
    return json(te::PolicyRuntime(reject ? terminal_policy : base).decide(current));
  };
  const auto same_allowed = after_failure("branch-a", false), same_rejected = after_failure("branch-a", true);
  const auto other_allowed = after_failure("branch-b", false);
  expect(same_rejected["actions"].items().front().str("decision") == "reject", "terminal replay fixture did not reject its own retry");
  const auto same_report = report(failure_events, same_allowed), other_report = report(failure_events, other_allowed);
  const auto terminal_preflight = search.control_preflight(parent, terminal_child, same_report);
  expect(terminal_preflight.flag("passed", false) && terminal_preflight["rules"].num("terminal-budget", 0) == 1,
      "same-execution terminal failure was lost from control preflight");
  expect(!search.control_preflight(parent, terminal_child, other_report).flag("passed", false),
      "a sibling's first attempt became a terminal control opportunity");
  expect(search.applied_changes(terminal_child, other_report).fields().empty(),
      "allowing a sibling's first attempt was misattributed as removing the parent's restriction");
  expect(search.applied_changes(terminal_child, report(failure_events, same_rejected)).fields().empty(),
      "an identical same-execution parent rejection became a new policy effect");
  expect(search.applied_changes(parent, report(failure_events, same_rejected))["task"].num("actions", 0) == 1,
      "an actually applied same-execution terminal guard lost its attribution");
  expect(search.applied_changes(terminal_child, same_report)["task"].num("actions", 0) == 1,
      "removing an actually applicable same-execution terminal guard lost its attribution");

  // 新预算既要能拦浪费，也不能拦已评为必要的第二次任务；只使用真实动作归属。
  auto budget_options = options; budget_options.clear_action_rules();
  auto* budget = budget_options.add_action_rules(); budget->set_rule_id("operation-cap");
  budget->set_target_scope("assistant"); budget->set_operation("generate"); budget->set_max_attempts(1);
  budget->set_reason("Stop redundant attempts"); budget->set_match_mode("predicate");
  *budget->mutable_source_experience() = *approval;
  const Json budget_child{{"policy", json(te::compile_runtime_policy(snapshot, budget_options))}};
  const auto reviewed_budget_report = [&](const char* decision, bool other_action = false,
      bool mixed_work = false, bool final_answer = false) {
    auto result = report(sequential, allowed);
    auto evidence = json(result.trials(0).evidence());
    for(auto& event : evidence["trace"]["events"].items()) {
      if(event.str("step_id") != "guard-step") continue;
      event["execution_id"] = "coordinator"; event["loop_id"] = "coordinator"; event["iteration"] = "3";
      event["actor_id"] = "coordinator";
      if(event.str("kind") == "custom") event["event_id"] = "guard-query-event";
      if(event.str("kind") == "action.end") {
        event["event_id"] = "guard-end"; event["action_id"] = other_action ? "unrelated" : "guard";
        event["operation"] = "generate";
      }
    }
    if(mixed_work || final_answer) {
      auto extra = evidence["trace"]["events"].items().back();
      extra["event_id"] = "independent-end"; extra["action_id"] = "independent";
      extra["operation"] = final_answer ? "chat" : "write";
      if(final_answer) {
        extra["attributes"]["semantic"] = "model";
        extra["output"] = Json{{"choices", Json::Array{Json{{"finish_reason", "stop"},
            {"message", Json{{"content", "Necessary final answer"}}}}}}};
      }
      evidence["trace"]["events"].push(extra);
    }
    const Json context{{"execution_id", "coordinator"}, {"loop_id", "coordinator"}, {"iteration", "3"}};
    evidence["loop_review"] = Json{{"status", "complete"}, {"observed_iterations", 1},
        {"iteration_reviews", Json::Array{Json{{"iteration_id", iteration_identity(Json{{"run_id", "run"}}, context)},
            {"decision", decision}, {"reason", "Necessary or redundant second task"}}}}};
    result.mutable_trials(0)->set_split("train");
    *result.mutable_trials(0)->mutable_evidence() = proto<google::protobuf::Struct>(evidence);
    return result;
  };
  auto budget_check = search.control_preflight(parent, budget_child, reviewed_budget_report("keep"));
  expect(!budget_check.flag("passed", true) && budget_check.has("known_keep_truncation")
      && budget_check["rules"].num("operation-cap", 0) == 1,
      "a real differential budget hit authorized rejection of known necessary work");
  for(const auto* decision : {"waste", "unknown"}) {
    budget_check = search.control_preflight(parent, budget_child, reviewed_budget_report(decision));
    expect(budget_check.flag("passed", false) && !budget_check.has("known_keep_truncation"),
        "waste or incomplete review was falsely marked as necessary work");
  }
  budget_check = search.control_preflight(parent, budget_child, reviewed_budget_report("keep", true));
  expect(!budget_check.has("known_keep_truncation"), "another action's keep assessment contaminated the budget hit");
  for(const bool final_answer : {false, true}) {
    budget_check = search.control_preflight(parent, budget_child, reviewed_budget_report("keep", false, !final_answer, final_answer));
    expect(budget_check.flag("passed", false) && !budget_check.has("known_keep_truncation"),
        "whole-iteration keep was incorrectly attributed to a tool alongside independent work or delivery");
  }
  for(const auto& content : Json::Array{Json(), Json(Json::Array{Json{{"type", "text"}, {"text", "Final answer"}}})}) {
    auto observed = reviewed_budget_report("keep", false, false, true);
    auto evidence = json(observed.trials(0).evidence());
    evidence["trace"]["events"].items().back()["output"]["choices"].items()[0]["message"]["content"] = content;
    *observed.mutable_trials(0)->mutable_evidence() = proto<google::protobuf::Struct>(evidence);
    budget_check = search.control_preflight(parent, budget_child, observed);
    expect(budget_check.flag("passed", false) && !budget_check.has("known_keep_truncation"),
        "null or structured final content crashed or falsely attributed keep to a tool");
  }
  expect(search.control_preflight(budget_child, budget_child, reviewed_budget_report("keep")).flag("passed", false),
      "loading an unchanged budget was rejected as a new proposal");
}

void terminal_failure_identity() {
  const Json options{{"action_rules", Json::Array{Json{{"rule_id", "terminal"}, {"target_scope", "scope"},
      {"operation", "render"}, {"max_failed_attempts", 1},
      {"terminal_error_codes", Json::Array{"UnsupportedModel"}}, {"error_scope_arguments", Json::Array{"model"}}}}}};
  Json ledger;
  auto snapshot = [&](const char* output, const char* model = "one", const char* scope = "scope", const char* execution = "branch-a") {
    return target_failure_snapshot(options, scope, "render", Json{{"output", output}, {"model", model}}, ledger, execution);
  };
  const Json failure{{"status", "failed"}, {"error", "HTTP 404: {\"error\":{\"code\":\"UnsupportedModel\"}}"}};
  auto first = snapshot("a.png");
  settle_target_failure(ledger, first, true, Json{{"error", Json{{"code", "Timeout"}}}});
  expect(snapshot("a.png")["terminal"].num("count", 0) == 0, "transient failure became terminal");
  settle_target_failure(ledger, first, true, failure);
  expect(snapshot("b.png")["terminal"].num("count", 0) == 1, "changed output evaded terminal configuration failure");
  expect(snapshot("b.png", "one", "scope", "branch-b")["terminal"].num("count", 0) == 0,
      "an independent first attempt inherited a sibling's terminal model error");
  expect(snapshot("b.png", "one", "scope", "").fields().empty()
      && target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}}, ledger).fields().empty(),
      "missing execution identity became a run-wide terminal circuit breaker");
  expect(snapshot("b.png", "two")["terminal"].num("count", 0) == 0, "different model inherited terminal failure");
  expect(snapshot("b.png", "one", "other").fields().empty(), "unrelated scope inherited terminal failure");
  expect(snapshot("b.png", "<redacted:model>").fields().empty(), "redacted configurations were merged as one identity");
  auto late = snapshot("a.png"); settle_target_failure(ledger, late, false);
  settle_target_failure(ledger, late, true, failure);
  expect(snapshot("a.png")["terminal"].num("count", 0) == 0, "late failure resurrected cleared terminal state");
  auto retry = snapshot("a.png"); settle_target_failure(ledger, retry, true, failure);
  expect(snapshot("a.png")["terminal"].num("count", 0) == 1, "new failed attempt after success was ignored");
  expect(structured_error_code(Json("UnsupportedModel mentioned in prose")).empty(), "free text inferred a terminal code");
  Json replay_ledger;
  Json frame{{"entries", target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}}, replay_ledger, "branch-a")},
      {"scope", "scope"}, {"operation", "render"}, {"execution_id", "branch-a"}};
  settle_target_event(replay_ledger, frame, options, Json{{"kind", "action.end"}, {"status", "failed"},
      {"input", Json{{"model", "one"}}}, {"execution_id", "branch-a"},
      {"output", Json{{"envelope", Json::Object{}}}},
      {"attributes", Json{{"executed", true}, {"observed_error_code", "UnsupportedModel"}}}});
  expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}}, replay_ledger, "branch-a")["terminal"].num("count", 0) == 1,
      "projected asynchronous error code was not replayable");
  expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}}, replay_ledger, "branch-b")["terminal"].num("count", 0) == 0,
      "replayed terminal error escaped its execution identity");
  Json scheduled, schedule_ledger;
  const auto start = [](const std::string& id) {
    return Json{{"kind", "action.start"}, {"action_id", id}, {"execution_id", "same-dispatch-parent"},
        {"operation", "render"}, {"input", Json{{"model", "one"}}},
        {"attributes", Json{{"type", "schedule.action"}, {"target_scope", "scope"}}}};
  };
  snapshot_scheduled_target(scheduled, schedule_ledger, options, start("child-a"));
  settle_target_event(schedule_ledger, static_cast<const Json&>(scheduled)["child-a"], options,
      Json{{"kind", "action.end"}, {"action_id", "child-a"}, {"execution_id", "same-dispatch-parent"},
          {"status", "failed"}, {"input", Json{{"model", "one"}}},
          {"attributes", Json{{"executed", true}, {"observed_error_code", "UnsupportedModel"}}}});
  snapshot_scheduled_target(scheduled, schedule_ledger, options, start("child-b"));
  expect(scheduled["child-a"].str("execution_id") == "child-a"
      && scheduled["child-b"].str("execution_id") == "child-b"
      && scheduled["child-b"]["entries"]["terminal"].num("count", 0) == 0,
      "scheduled siblings with one dispatch parent shared a terminal failure identity");
  expect(target_failure_snapshot(options, "scope", "render", Json{{"model", "one"}}, schedule_ledger, "child-a")["terminal"].num("count", 0) == 1,
      "scheduled terminal failure was not settled under the guard's child identity");
}

void attributed_candidate_selection() {
  Search search;
  const Json base{{"quality", 1}, {"fitness", 0.5}, {"invalid_loops", 2},
      {"loop_method", "model_review"}, {"duration_ms", 100}};
  auto better = base; better["fitness"] = 0.8; better["invalid_loops"] = 1; better["duration_ms"] = 90;
  search.gate = Json{{"max_latency_ratio", 1}};
  search.tree = Json{{"nodes", Json::Array{
      Json{{"id", 0}, {"status", "accepted"}, {"validation", Json{{"task", base}}}},
      Json{{"id", 1}, {"status", "accepted"}, {"parents", Json::Array{0}},
          {"validation", Json{{"task", better}}}, {"validation_applied_changes", Json{{"0", Json::Object{}}}}}}}};
  expect(search.winner() == 0, "unapplied validation gains selected a new winner");
  auto& node = search.tree["nodes"].items()[1];
  node["validation_applied_changes"]["0"]["task"] = Json{{"actions", 1}};
  expect(search.winner() == 1, "observed nonregressing improvement was not selectable");
  node["validation"]["task"]["duration_ms"] = 200;
  expect(search.winner() == 0, "latency regression in affected cases was hidden by other metrics");
  node["validation"]["task"] = better; node["validation"]["task"]["quality"] = 0;
  expect(search.winner() == 0, "failing faster was counted as attributed improvement");
  node["validation"]["task"] = better;
  node["validation_applied_changes"]["0"] = Json{{"unrelated-training-task", Json{{"loop", 1}}}};
  expect(search.winner() == 0, "training-only evidence substituted for validation effects");
}

void operation_budget_identity() {
  Json action{{"kind", "action.end"}, {"executed", true},
      {"action_control", Json{{"decision", "allow"}, {"prior_operation_rounds", 20}}}};
  expect(!action_control_evidence(action), "ordinary repeated work crowded out real loop/argument evidence");
  action["guard_only"] = true; action["executed"] = false;
  expect(action_control_evidence(action), "redispatch opportunity was lost with execution guards");
  const Json options{{"action_rules", Json::Array{Json{{"rule_id", "budget"}, {"target_scope", "scope"},
      {"operation", "verify"}, {"max_attempts", 2}}}}};
  Json ledger;
  auto snapshot = [&](const char* prompt) {
    return target_failure_snapshot(options, "scope", "verify", Json{{"prompt", prompt}}, ledger);
  };
  const auto first = snapshot("verify a"), parallel = snapshot("verify b");
  settle_target_failure(ledger, first, false);
  settle_target_failure(ledger, parallel, false);
  expect(snapshot("changed wording")["budget"].num("count", 0) == 1, "parallel attempts counted as sequential retries");
  settle_target_failure(ledger, snapshot("already finished"), false);
  expect(snapshot("new wording")["budget"].num("count", 0) == 2, "successful no-op reset explicit budget");
  expect(target_failure_snapshot(options, "scope", "search", Json::Object{}, ledger).fields().empty(),
      "operation budget leaked to unrelated branch");
  expect(target_failure_snapshot(options, "scope", "verify", Json::Object{}, Json{})["budget"].num("count", 0) == 0,
      "operation budget leaked to a new run");

  // 长子循环不能挤掉上层重复派发；反思仍只保留 16 项，并按原始顺序呈现。
  Json events = Json::Array{};
  for(int i = 1; i <= 24; ++i) {
    const auto step = "loop-" + std::to_string(i);
    Json event{{"kind", "custom"}, {"step_id", step}};
    event["attributes"]["type"] = "policy.query";
    event["attributes"]["decision"]["query"] = Json{{"boundary", "loop"}, {"iteration", i},
        {"repeated_action_steps", 2}, {"advice_query", Json{{"target_scope", "scope"}}}};
    event["attributes"]["decision"]["progress"]["decision"] = "continue";
    events.push(event);
    events.push(Json{{"kind", "loop.decision"}, {"step_id", step}, {"actor_id", "worker"}});
  }
  events.push(Json{{"kind", "action.start"}, {"action_id", "first"}, {"operation", "delegate"},
      {"input", Json::Object{}}, {"attributes", Json{{"type", "schedule.action"}, {"target_scope", "scope"}}}});
  events.push(Json{{"kind", "action.end"}, {"action_id", "first"}, {"status", "completed"},
      {"attributes", Json{{"executed", true}}}});
  const Json planned{{"action_id", "guard"}, {"operation", "delegate"}, {"arguments", Json::Object{}}};
  Json guard{{"kind", "custom"}, {"step_id", "guard-step"}};
  guard["attributes"]["type"] = "policy.query";
  guard["attributes"]["decision"]["query"] = Json{{"boundary", "actions"}, {"execution_id", "dispatcher"},
      {"advice_query", Json{{"target_scope", "scope"}}}, {"actions", Json::Array{planned}}};
  guard["attributes"]["decision"]["actions"] = Json::Array{Json{{"action_id", "guard"}, {"decision", "allow"}}};
  events.push(guard);
  events.push(Json{{"kind", "action.start"}, {"action_id", "guard"}, {"operation", "delegate"},
      {"actor_id", "dispatcher"}, {"input", Json::Object{}},
      {"attributes", Json{{"extensions", Json{{"guard_only", true}}}}}});
  events.push(Json{{"kind", "action.end"}, {"action_id", "guard"}, {"step_id", "guard-step"},
      {"attributes", Json{{"guard_only", true}, {"executed", false}}}});
  Report report; auto* trial = report.add_trials(); trial->set_variant_id("candidate");
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", events}}}});
  const auto compact = control_feedback_trial(*trial, Json::Array{"action_rules", "loop_rules"}, Json::Object{});
  const auto& rows = compact["evidence"]["runs"].items().front()["actions"].items();
  expect(rows.size() == 16, "control feedback window changed");
  expect(rows.back().flag("guard_only", false)
      && rows.back()["action_control"].num("prior_operation_rounds", 0) == 1,
      "long child loop crowded out a repeated parent dispatch");
}

void schedule_resource_projection() {
  auto query = proto<trajectory::evolution::v1::RuntimeQuery>(Json{{"boundary", "schedule"}, {"execution_id", "batch"},
      {"actions", Json::Array{Json{{"action_id", "a"}, {"operation", "prepare"}},
          Json{{"action_id", "b"}, {"operation", "render"}}, Json{{"action_id", "c"}, {"operation", "render"}}}}});
  const Json input{{"actions", Json::Array{
      Json{{"action_id", "a"}, {"operation", "prepare"}, {"arguments", Json{{"output", "asset.png"}, {"prompt", std::string(2000, 'x')}}}},
      Json{{"action_id", "b"}, {"operation", "render"}, {"arguments", Json{{"input", "asset.png"}}}},
      Json{{"action_id", "c"}, {"operation", "render"}, {"arguments", Json{{"input", "independent.png"}}}}}}};
  project_schedule_evidence(query, input);
  const auto projected = json(query);
  expect(projected["actions"].items()[0]["arguments"].str("output") == "asset.png", "producer evidence missing");
  expect(!projected["actions"].items()[0]["arguments"].has("prompt"), "full prompt copied into schedule evidence");
  expect(projected["actions"].items()[1]["arguments"].str("input") == "asset.png", "consumer evidence missing");
  expect(!projected["actions"].items()[2].has("arguments"), "unshared resource leaked into scheduling context");
  auto replay = query; project_schedule_evidence(replay, input);
  expect(replay.SerializeAsString() == query.SerializeAsString(), "schedule evidence projection not idempotent");
}

void recovery_component_pairing() {
  const Json available{{"action_rules", Json::Array{}}, {"loop_rules", Json::Array{}}};
  const Json selected = Json::Array{"action_rules"};
  const Json failure{{"kind", "action.end"}, {"actor_id", "worker"}, {"execution_id", "branch-a"},
      {"executed", true}, {"terminal_error_code_candidate", "UnsupportedModel"},
      {"action_control", Json{{"decision", "allow"}, {"target_scope", "scope"}}}};
  const Json loop{{"kind", "loop.decision"}, {"actor_id", "worker"}, {"execution_id", "branch-a"},
      {"loop_control", Json{{"target_scope", "scope"}, {"proposed_decision", "replan"}, {"pending_actions", false}}}};
  auto packet = [&](Json f, Json l) { return Json{{"feedback", Json::Array{
      Json{{"trial", Json{{"evidence", Json{{"runs", Json::Array{
          Json{{"actions", Json::Array{f, l}}}}}}}}}}}}}; };
  expect(recovery_components(selected, packet(failure, loop), available).items().size() == 2,
      "same-branch guard and recovery were optimized separately");
  auto sibling = loop; sibling["execution_id"] = "branch-b";
  expect(recovery_components(selected, packet(failure, sibling), available).items().size() == 1,
      "independent concurrent branches were coupled");
  sibling = loop; sibling["loop_control"]["pending_actions"] = true;
  expect(recovery_components(selected, packet(failure, sibling), available).items().size() == 1,
      "pending work was treated as a recovery opportunity");
  sibling = loop; sibling["loop_control"]["target_scope"] = "other";
  expect(recovery_components(selected, packet(failure, sibling), available).items().size() == 1,
      "cross-scope recovery was coupled");
  const Json l1 = Json::Array{"experience:one"};
  expect(recovery_components(l1, packet(failure, loop), available).dump() == l1.dump(),
      "experience selection silently expanded control authority");
}

void prompt_resource_workflows() {
  namespace te = trajectory::evolution;
  Json input{{"actions", Json::Array{}}};
  for(const auto& item : std::vector<std::pair<std::string, std::string>>{
      {"prepare", "保存图片到 assets/a.png"}, {"render", "Use assets/a.png as the first frame"},
      {"prepare", "Generate assets/b.png"}, {"render", "使用 assets/b.png 生成视频"},
      {"lookup", "独立查询，不依赖上述文件"}})
    input["actions"].push(Json{{"action_id", "action-" + std::to_string(input["actions"].items().size())},
        {"operation", item.first}, {"arguments", Json{{"prompt", item.second}}}});
  auto q = proto<te::v1::RuntimeQuery>(input); q.set_boundary("schedule"); q.set_execution_id("batch");
  q.mutable_advice_query()->set_target_scope("scope");
  project_schedule_evidence(q, input);
  expect(json(q)["actions"].items()[0]["arguments"].str(kScheduleResource) == "assets/a.png",
      "shared file hidden in prompt was lost");
  const auto options = proto<te::v1::RuntimeOptions>(Json{{"requested_level", "L2"},
      {"host_capabilities", Json::Array{"schedule.dispatch"}}, {"schedule_rules", Json::Array{
        Json{{"rule_id", "paired"}, {"target_scope", "scope"}, {"before_operation", "prepare"},
          {"after_operation", "render"}, {"before_argument", kScheduleResource}, {"after_argument", kScheduleResource},
          {"require_success", true}, {"reason", "explicit resource dependency"}}}}});
  const auto plan = te::plan_schedule(options, q);
  expect(plan.status() == "scheduled" && plan.actions(1).after_size() == 1 && plan.actions(3).after_size() == 1,
      "resource-based rule did not create two separate workflow pairs");
  auto state = te::begin_schedule(plan);
  state = te::advance_schedule(state, "action-0", "running"); state = te::advance_schedule(state, "action-2", "running");
  state = te::advance_schedule(state, "action-0", "failed"); state = te::advance_schedule(state, "action-2", "succeeded");
  expect(state.actions(1).status() == "skipped"
      && te::ready_schedule(state) == std::vector<std::string>({"action-3", "action-4"}),
      "one failed producer cancelled an independent workflow");
  auto ambiguous = input; ambiguous["actions"].items()[1]["arguments"]["prompt"] = "assets/a.png assets/b.png";
  project_schedule_evidence(q, ambiguous);
  expect(!json(q)["actions"].items()[1]["arguments"].has(kScheduleResource), "ambiguous resource was guessed");
  expect(schedule_resources(Json{{"prompt", "https://example.com/assets/a.png?token=hidden"}}).empty(),
      "URL data was used as a local file identity");
  expect(schedule_resources(Json{{kScheduleResource, "assets/forged.png"}}).empty(),
      "caller supplied synthetic resource was trusted");
  for(const auto* path : {"甲/a.png", "乙/a.png", "资料assets/a.png", "assets/a.png中文", "C:\\assets/a.png"})
    expect(schedule_resources(Json{{"prompt", path}}).empty(), "unsupported path was partially projected");
  expect(schedule_resources(Json{{"prompt", "保存为 assets/a.png。请确认；随后使用“assets/a.png”作为首帧"}})
      == std::set<std::string>{"assets/a.png"}, "Chinese punctuation hid a complete ASCII resource");
}

void loop_feedback_runtime_budget() {
  namespace te = trajectory::evolution;
  agentflow::trajectory::evaluation::EvolutionMemoryStore memory;
  memory.set_schema_version(1); memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1"); memory.set_revision(1);
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"}, {"user_id", "user"},
      {"conversation_id", "conversation"}, {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
      {"max_policy_entries", 64}, {"max_advice_items", 3}, {"max_advice_bytes", 4096},
      {"max_query_bytes", 16384}, {"max_memory_bytes", 1048576}, {"max_snapshot_bytes", 262144},
      {"max_match_terms", 128}, {"min_matched_terms", 2}});
  auto options = proto<te::v1::RuntimeOptions>(Json{{"requested_level", "L3"}, {"fallback", "strict"},
      {"host_capabilities", Json::Array{"context.request", "action.filter", "loop.decide"}},
      {"allowed_users", Json::Array{"user"}}, {"allowed_conversations", Json::Array{"conversation"}},
      {"experience_template", "{{experience}}"}, {"max_context_bytes", 4096},
      {"loop_rules", Json::Array{Json{{"rule_id", "base-loop"}, {"target_scope", "assistant"},
          {"max_iterations", 12}, {"max_no_progress", 3}, {"no_progress_decision", "stop"},
          {"reason", "bounded execution"}}}}});
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  Search search;
  search.controls = Json{{"enabled", true}, {"prompt_file", "test.txt"}, {"min_iterations", 1}};
  validate_control_learning(search.controls);
  search.config = Json{{"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  auto* task = search.dataset.add_tasks(); task->set_task_id("train-loop"); task->set_split("train");
  task->set_task_input("Complete the delegated work and report its result.");
  Json events = Json::Array{};
  for(unsigned index = 0; index <= 24; ++index) {
    const bool coordinator = index == 24;
    const auto step = "step-" + std::to_string(index);
    const auto iteration = coordinator ? "3" : "1";
    const auto actor = coordinator ? "coordinator" : "worker";
    Json query{{"boundary", "loop"}, {"iteration", iteration}, {"state_fingerprint", step},
        {"actor_id", actor}, {"execution_id", step}, {"advice_query", Json{{"target_scope", "assistant"}}}};
    Json progress{{"decision", "continue"}, {"iteration", iteration}, {"fingerprint", step}};
    events.push(Json{{"kind", "custom"}, {"step_id", step}, {"actor_id", actor},
        {"attributes", Json{{"type", "policy.query"}, {"decision", Json{{"query", query}, {"progress", progress}}}}}});
    events.push(Json{{"kind", "loop.decision"}, {"step_id", step}, {"actor_id", actor}});
  }
  Report report; auto* trial = report.add_trials();
  trial->set_trial_id("trial"); trial->set_task_id(task->task_id()); trial->set_variant_id("candidate");
  trial->set_verdict("passed");
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"events", events}}}});
  for(bool aggressive : {false, true}) {
    options.set_aggressive_controls(aggressive);
    search.seed = Json{{"schema_version", 1}, {"agent", Json{{"revision", "test"}}}, {"user_id", "user"},
        {"level", "L3"}, {"policy", json(te::compile_runtime_policy(snapshot, options))}};
    const auto packet = search.packet(search.seed, report, Json::Array{"loop_rules"});
    const auto& rows = packet["feedback"].items().front()["trial"]["evidence"]["runs"].items().front()["actions"].items();
    expect(rows.size() == 16, "iteration evidence expanded the feedback budget");
    const bool included = std::any_of(rows.begin(), rows.end(), [](const auto& row) {
      return row.str("actor_id") == "coordinator" && row["loop_control"].num("iteration", 0) == 3
          && row["loop_control"].num("observed_no_progress", 99) == 0;
    });
    expect(!included, "unreviewed high iteration was treated as an iteration-cap opportunity");
  }
  expect(!search.controls.has("aggressive_controls"), "runtime feedback flag changed control-learning configuration");
  // COMBEE 直接调用 mutate；空增量必须保持同一版本，不多出策略编号。
  const Json abstain{{"reason", "No justified change"}, {"control_updates", Json::Array{}}};
  expect(search.mutate(search.seed, abstain, Json::Array{"loop_rules"}).dump() == search.seed.dump(),
      "empty delta did not preserve the original agent version");

  // 用真实编译链复现：新增 coordinator 限制不能丢失已有 video-gen 规则。
  namespace ep = agentflow::trajectory::evaluation;
  auto* entry = memory.add_entries(); entry->set_memory_id("loop-memory");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto* source = entry->mutable_candidate(); source->set_schema_version(1); source->set_candidate_id("loop-candidate");
  source->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  source->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION); source->set_target_scope("assistant");
  source->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED); source->set_trigger_pattern("Repeated failed work");
  source->set_observed_problem("Repeated video calls"); source->set_proposed_change("Stop retrying completed work.");
  source->set_support_user_count(1);
  auto* evidence = source->add_support_evidence(); evidence->set_evidence_id("loop-evidence");
  evidence->mutable_source()->set_user_id("user"); evidence->mutable_source()->set_dialogue_id("source");
  const auto learned_snapshot = te::compile_shadow_policy(memory, identity, limits);
  for(const auto& experience : learned_snapshot.experiences()) {
    auto* approval = options.add_approved_experiences(); approval->set_memory_id(experience.memory_id());
    approval->set_content_revision(experience.content_revision());
  }
  const Json video{{"memory_id", "loop-memory"}, {"actor_id", "video-gen"}, {"max_iterations", 12},
      {"max_no_progress", 1}, {"reason", "Stop repeated video work"}};
  const auto learned_options = apply_control_draft(learned_snapshot, options, search.controls,
      Json{{"action_rules", Json::Array{}}, {"loop_rules", Json::Array{video}}});
  search.seed["policy"] = json(te::compile_runtime_policy(learned_snapshot, learned_options));
  const auto parent_components = search.components(search.seed);
  const Json coordinator{{"memory_id", "loop-memory"}, {"actor_id", "coordinator"}, {"max_iterations", 2},
      {"reason", "Bound unnecessary coordinator iterations"}};
  const Json delta{{"reason", "Change coordinator only"}, {"control_updates", Json::Array{
      Json{{"component", "loop_rules"}, {"op", "add"}, {"value", coordinator}}}}};
  const auto child = search.mutate(search.seed, delta, Json::Array{"loop_rules"});
  const auto child_components = search.components(child);
  expect(child_components["loop_rules"].items().size() == 2
      && child_components["loop_rules"].items()[0].dump() == parent_components["loop_rules"].items()[0].dump(),
      "compiled coordinator delta dropped or rewrote its video sibling");
  const Json drop{{"reason", "Explicitly remove coordinator"}, {"control_updates", Json::Array{
      Json{{"component", "loop_rules"}, {"op", "remove"}, {"index", 1}}}}};
  expect(search.components(search.mutate(child, drop, Json::Array{"loop_rules"})).dump() == parent_components.dump(),
      "explicit deletion cannot restore the parent controls");
  const Json same{{"reason", "Keep existing rule"}, {"control_updates", Json::Array{
      Json{{"component", "loop_rules"}, {"op", "set"}, {"index", 0}, {"value", parent_components["loop_rules"].items()[0]}}}}};
  expect(search.mutate(search.seed, same, Json::Array{"loop_rules"}).dump() == search.seed.dump(),
      "setting an identical rule produced a new version");
  const Json replace{{"reason", "Legacy full replacement"}, {"controls", Json{{"loop_rules", Json::Array{coordinator}}}}};
  expect(search.components(search.mutate(search.seed, replace, Json::Array{"loop_rules"}))["loop_rules"].items().size() == 1,
      "legacy explicit replacement no longer removes omitted rules");
}

void incremental_control_drafts() {
  const Json video{{"memory_id", "memory"}, {"actor_id", "video-gen"}, {"max_iterations", 12}};
  const Json coordinator{{"memory_id", "memory"}, {"actor_id", "coordinator"}, {"max_iterations", 2}};
  const Json before{{"loop_rules", Json::Array{video, coordinator}}, {"action_rules", Json::Array{}},
      {"schedule_rules", Json::Array{}}};
  const auto original = before.dump();
  const Json selected = Json::Array{"loop_rules", "action_rules", "schedule_rules"};
  auto proposal = [](Json updates) { return Json{{"reason", "Only change the selected rule."}, {"control_updates", updates}}; };
  const auto add = Json{{"component", "loop_rules"}, {"op", "add"}, {"value", Json{{"actor_id", "writer"}}}};
  const auto remove = Json{{"component", "loop_rules"}, {"op", "remove"}, {"index", 0}};
  auto replacement = coordinator; replacement["max_iterations"] = 3;
  const auto set = Json{{"component", "loop_rules"}, {"op", "set"}, {"index", 1}, {"value", replacement}};
  auto expanded = expand_control_updates(proposal(Json::Array{add}), before, selected);
  expect(expanded["controls"]["loop_rules"].items().size() == 3
      && expanded["controls"]["loop_rules"].items()[0].dump() == video.dump(), "adding a coordinator rule erased independent video control");
  expanded = expand_control_updates(proposal(Json::Array{remove, add, set}), before, selected);
  expect(expanded["controls"]["loop_rules"].items().size() == 2
      && expanded["controls"]["loop_rules"].items()[0].dump() == replacement.dump(), "remove shifted a later parent-array index");
  expect(before.dump() == original, "draft expansion mutated its parent");
  const auto legacy = Json{{"reason", "Explicit replacement"}, {"controls", Json{{"loop_rules", Json::Array{coordinator}}}}};
  expect(expand_control_updates(legacy, before, selected).dump() == legacy.dump(), "legacy full replacement changed meaning");
  expect(expand_control_updates(expanded, before, selected).dump() == expanded.dump(), "draft expansion is not idempotent");
  auto reject = [&](const Json& draft, const Json& allowed = Json(Json::Array{"loop_rules", "action_rules", "schedule_rules"})) {
    bool failed = false;
    try { expand_control_updates(draft, before, allowed); } catch(const Error&) { failed = true; }
    expect(failed, "invalid or unauthorized control delta was accepted");
    expect(before.dump() == original, "rejected delta mutated its parent");
  };
  reject(proposal(Json::Array{set, set})); reject(proposal(Json::Array{remove, remove}));
  auto remove_same = remove; remove_same["index"] = 1;
  reject(proposal(Json::Array{set, remove_same})); reject(proposal(Json::Array{add, add}));
  for(const auto& index : Json::Array{Json(-1), Json(0.5), Json(2), Json("0"), Json(true), Json()}) {
    auto invalid = set; invalid["index"] = index; reject(proposal(Json::Array{invalid}));
  }
  auto missing_index = set; missing_index.fields().erase("index"); reject(proposal(Json::Array{missing_index}));
  auto indexed_add = add; indexed_add["index"] = 0; reject(proposal(Json::Array{indexed_add}));
  auto valued_remove = remove; valued_remove["value"] = video; reject(proposal(Json::Array{valued_remove}));
  for(const auto& value : Json::Array{Json(), Json("rule"), Json(Json::Array{})}) {
    auto invalid = set; invalid["value"] = value; reject(proposal(Json::Array{invalid}));
  }
  auto unknown_field = add; unknown_field["extra"] = true; reject(proposal(Json::Array{unknown_field}));
  auto unknown_op = add; unknown_op["op"] = "merge"; reject(proposal(Json::Array{unknown_op}));
  auto unknown_component = add; unknown_component["component"] = "experience:memory"; reject(proposal(Json::Array{unknown_component}));
  reject(proposal(Json::Array{add}), Json::Array{"action_rules"});
  auto mixed = proposal(Json::Array{add}); mixed["controls"] = Json::Object{}; reject(mixed);
  reject(proposal(Json::Object{})); reject(proposal(Json::Array{Json("add")}));
  auto scheduled = add; scheduled["component"] = "schedule_rules";
  auto prepared = expand_control_updates(proposal(Json::Array{scheduled}), before, selected);
  expect(prepared["controls"]["schedule_rules"].items().size() == 1, "schedule repair cannot see expanded control delta");
}

void pure_control_removal_preflight() {
  namespace te = trajectory::evolution;
  namespace ep = agentflow::trajectory::evaluation;
  ep::EvolutionMemoryStore memory;
  memory.set_schema_version(1); memory.set_store_kind("trajectory_evolution_memory");
  memory.set_store_version("evolution-memory-store-v1"); memory.set_revision(1);
  auto* entry = memory.add_entries(); entry->set_memory_id("removal-memory");
  entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto* source = entry->mutable_candidate(); source->set_schema_version(1); source->set_candidate_id("removal-candidate");
  source->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  source->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION); source->set_target_scope("assistant");
  source->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED); source->set_trigger_pattern("Repeated failed work");
  source->set_observed_problem("Unnecessary repeated calls"); source->set_proposed_change("Keep independent work running.");
  source->set_support_user_count(1);
  auto* support = source->add_support_evidence(); support->set_evidence_id("removal-evidence");
  support->mutable_source()->set_user_id("user"); support->mutable_source()->set_dialogue_id("source");
  const auto identity = proto<te::v1::Identity>(Json{{"agent_id", "agent"}, {"user_id", "user"},
      {"conversation_id", "conversation"}, {"run_id", "run"}});
  const auto limits = proto<te::v1::ShadowOptions>(Json{{"target_scopes", Json::Array{"assistant"}},
      {"max_policy_entries", 64}, {"max_advice_items", 3}, {"max_advice_bytes", 4096},
      {"max_query_bytes", 16384}, {"max_memory_bytes", 1048576}, {"max_snapshot_bytes", 262144},
      {"max_match_terms", 128}, {"min_matched_terms", 2}});
  const auto snapshot = te::compile_shadow_policy(memory, identity, limits);
  expect(snapshot.experiences_size() == 1, "removal experience fixture not eligible");
  auto options = proto<te::v1::RuntimeOptions>(Json{{"requested_level", "L3"}, {"fallback", "strict"},
      {"host_capabilities", Json::Array{"context.request", "action.filter", "schedule.dispatch", "loop.decide"}},
      {"allowed_users", Json::Array{"user"}}, {"allowed_conversations", Json::Array{"conversation"}},
      {"experience_template", "{{experience}}"}, {"max_context_bytes", 4096}});
  auto* approval = options.add_approved_experiences(); approval->set_memory_id(snapshot.experiences(0).memory_id());
  approval->set_content_revision(snapshot.experiences(0).content_revision());
  const auto ref = *approval;
  auto artifact = [&](const te::v1::RuntimeOptions& value) {
    return Json{{"policy", json(te::compile_runtime_policy(snapshot, value))}};
  };
  const auto child = artifact(options);
  auto query = [&](const char* boundary) {
    return proto<te::v1::RuntimeQuery>(Json{{"boundary", boundary}, {"execution_id", "execution"}, {"actor_id", "worker"},
        {"advice_query", Json{{"identity", json(identity)}, {"target_scope", "assistant"}, {"task", "Complete the task"},
            {"query_id", "removal-query"}, {"decision_id", "removal-decision"}}}});
  };
  auto report = [&](const Json& parent, const te::v1::RuntimeQuery& input, const char* status = "") {
    const auto actual = json(te::PolicyRuntime(proto<te::v1::RuntimePolicy>(parent["policy"])).decide(input));
    Json events = Json::Array{Json{{"kind", "custom"}, {"step_id", "step"}, {"actor_id", input.actor_id()},
        {"attributes", Json{{"type", "policy.query"}, {"decision", actual}}}},
        Json{{"kind", "advice.applied"}, {"application", Json{{"query_id", "removal-query"},
            {"status", *status ? status : actual.str("status") == "no_change" ? "rejected" : "applied"}}}}};
    if(input.boundary() == "schedule") events.push(Json{{"kind", "step.end"}, {"step_id", "step"}, {"status", "completed"}});
    if(input.boundary() == "loop") events.push(Json{{"kind", "loop.decision"}, {"step_id", "step"}});
    Report result; auto* trial = result.add_trials(); trial->set_variant_id("candidate"); trial->set_task_id("train-task");
    trial->set_split("train");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}}});
    return result;
  };
  Search search;
  auto scheduled_options = options;
  auto* schedule = scheduled_options.add_schedule_rules(); schedule->set_rule_id("learned-order");
  schedule->set_target_scope("assistant"); schedule->set_before_operation("prepare"); schedule->set_after_operation("render");
  schedule->set_before_argument("resource"); schedule->set_after_argument("resource"); schedule->set_require_success(true);
  schedule->set_reason("Wait for the matching resource"); *schedule->mutable_source_experience() = ref;
  const auto scheduled = artifact(scheduled_options);
  auto schedule_query = query("schedule");
  *schedule_query.add_actions() = proto<te::v1::RuntimeAction>(Json{{"action_id", "producer"}, {"operation", "prepare"},
      {"arguments", Json{{"resource", "a.png"}}}});
  *schedule_query.add_actions() = proto<te::v1::RuntimeAction>(Json{{"action_id", "consumer"}, {"operation", "render"},
      {"arguments", Json{{"resource", "b.png"}}}});
  const auto original_parent = scheduled.dump();
  auto check = search.control_preflight(scheduled, child, report(scheduled, schedule_query));
  expect(!check.flag("passed", true) && check.flag("pure_removal_no_effect", false)
      && check["removed_rule_ids"].items().size() == 1 && check["removal_changes"].fields().empty(),
      "deleting a schedule rule that never matched was accepted");
  (*schedule_query.mutable_actions(1)->mutable_arguments()->mutable_fields())["resource"].set_string_value("a.png");
  check = search.control_preflight(scheduled, child, report(scheduled, schedule_query));
  expect(check.flag("passed", false) && check["removal_changes"]["train-task"].num("schedule", 0) == 1,
      "removing a genuinely applied dependency was blocked");
  expect(!search.control_preflight(scheduled, child, report(scheduled, schedule_query, "failed")).flag("passed", true),
      "a failed schedule receipt authorized removal");
  auto* host_edge = schedule_query.mutable_actions(1)->add_after(); host_edge->set_action_id("producer");
  host_edge->set_require_success(true);
  expect(!search.control_preflight(scheduled, child, report(scheduled, schedule_query)).flag("passed", true),
      "retained host dependency or changed policy metadata counted as an execution difference");
  expect(scheduled.dump() == original_parent, "deletion preflight modified its parent");

  // 同一查询中的独立分支仍继续；不把 rule_id 变化当作拒绝效果变化。
  auto guarded_options = options;
  auto* guard = guarded_options.add_action_rules(); guard->set_rule_id("learned-guard"); guard->set_target_scope("assistant");
  guard->set_operation("render"); guard->add_required_arguments("image"); guard->set_reason("Image is required");
  guard->set_match_mode("predicate"); *guard->mutable_source_experience() = ref;
  const auto guarded = artifact(guarded_options);
  auto action_query = query("actions");
  *action_query.add_actions() = proto<te::v1::RuntimeAction>(Json{{"action_id", "render"}, {"operation", "render"}});
  *action_query.add_actions() = proto<te::v1::RuntimeAction>(Json{{"action_id", "independent"}, {"operation", "summarize"}});
  const auto decisions = te::PolicyRuntime(proto<te::v1::RuntimePolicy>(guarded["policy"])).decide(action_query);
  expect(decisions.actions(0).decision() == "reject" && decisions.actions(1).decision() == "allow", "action fixture lost branch isolation");
  check = search.control_preflight(guarded, child, report(guarded, action_query));
  expect(check.flag("passed", false) && check["removal_changes"]["train-task"].num("actions", 0) == 1,
      "removal of an applied action rejection was blocked");
  action_query.mutable_actions(0)->set_operation("summarize");
  expect(!search.control_preflight(guarded, child, report(guarded, action_query)).flag("passed", true),
      "unrelated action siblings justified removal of a nonmatching guard");
  action_query.mutable_actions(0)->set_operation("render");
  auto equivalent_options = guarded_options;
  auto* equivalent = equivalent_options.add_action_rules(); *equivalent = guarded_options.action_rules(0);
  equivalent->set_rule_id("host-guard"); equivalent->clear_source_experience();
  auto equivalent_child = options; *equivalent_child.add_action_rules() = *equivalent;
  const auto equivalent_parent = artifact(equivalent_options);
  expect(!search.control_preflight(equivalent_parent, artifact(equivalent_child), report(equivalent_parent, action_query)).flag("passed", true),
      "same rejection with a different rule_id was counted as behavior change");

  auto bounded_options = options;
  auto* loop = bounded_options.add_loop_rules(); loop->set_rule_id("learned-loop"); loop->set_target_scope("assistant");
  loop->set_actor_id("worker"); loop->set_max_iterations(2); loop->set_no_progress_decision("stop");
  loop->set_reason("Bound repeated work"); loop->set_match_mode("predicate"); *loop->mutable_source_experience() = ref;
  const auto bounded = artifact(bounded_options);
  auto loop_query = query("loop"); loop_query.set_iteration(3); loop_query.set_state_fingerprint("state");
  check = search.control_preflight(bounded, child, report(bounded, loop_query));
  expect(check.flag("passed", false) && check["removal_changes"]["train-task"].num("loop", 0) == 1,
      "removing a committed stop was blocked");
  auto recovery_options = bounded_options;
  auto* recovery = recovery_options.mutable_loop_rules(0); recovery->set_max_iterations(12); recovery->set_max_no_progress(1);
  recovery->set_no_progress_decision("replan"); recovery->set_max_replans(1); recovery->set_replan_context("Use another source.");
  const auto recovery_parent = artifact(recovery_options);
  auto recovery_query = loop_query; recovery_query.set_repeated_action_steps(1);
  expect(te::PolicyRuntime(proto<te::v1::RuntimePolicy>(recovery_parent["policy"])).decide(recovery_query).progress().decision() == "replan",
      "recovery fixture did not request a replan");
  expect(search.control_preflight(recovery_parent, child, report(recovery_parent, recovery_query)).flag("passed", false),
      "removal of a genuinely applied replan was blocked");
  auto narrowed_options = recovery_options;
  auto* narrowed = narrowed_options.mutable_loop_rules(0);
  narrowed->set_rule_id("conditional-stop"); narrowed->set_max_replans(0);
  narrowed->set_no_progress_decision("stop"); narrowed->clear_replan_context();
  check = search.control_preflight(recovery_parent, artifact(narrowed_options), report(recovery_parent, recovery_query));
  expect(!check.flag("passed", true) && check.has("unconditioned_recovery"),
      "new reduced recovery budget used prose instead of an executable condition");
  narrowed->set_after_action_rejection(true);
  check = search.control_preflight(recovery_parent, artifact(narrowed_options), report(recovery_parent, recovery_query));
  expect(!check.flag("passed", true) && !check.has("unconditioned_recovery"),
      "unobserved conditional recovery was authorized");
  recovery_query.set_rejected_actions(1);
  check = search.control_preflight(recovery_parent, artifact(narrowed_options), report(recovery_parent, recovery_query));
  expect(check.flag("passed", false), "observed branch rejection could not authorize a conditional stop");
  const auto unchanged = artifact(narrowed_options);
  expect(search.control_preflight(unchanged, unchanged, report(unchanged, recovery_query)).flag("passed", false),
      "unchanged recovery policy was forced through a new-candidate condition check");
  loop_query.set_actor_id("coordinator");
  expect(!search.control_preflight(bounded, child, report(bounded, loop_query)).flag("passed", true),
      "another actor justified removal of a worker-only loop cap");
  loop_query.set_actor_id("worker"); loop_query.set_pending_actions(true);
  expect(!search.control_preflight(bounded, child, report(bounded, loop_query)).flag("passed", true),
      "pending work was treated as an applied loop stop");
  loop_query.set_pending_actions(false);
  expect(!search.control_preflight(bounded, child, report(bounded, loop_query, "failed")).flag("passed", true),
      "an uncommitted loop stop justified deletion");

  // 重复实例全浪费，也不能用 actor 级新 cap 截断同一 actor 的必要实例。
  auto reviewed_report = [&](bool third_required, bool final_tool = false, bool omit_final_query = false) {
    Json events = Json::Array{}, reviews = Json::Array{};
    for(const std::string execution : {"required", "redundant"}) for(std::size_t i = 1; i <= 4; ++i) {
      auto input = query("loop"); input.set_execution_id(execution); input.set_iteration(i);
      input.set_can_finalize(true); input.set_state_fingerprint(execution + std::to_string(i));
      Json context{{"actor_id", "worker"}, {"execution_id", execution}, {"loop_id", execution},
          {"iteration", std::to_string(i)}, {"step_id", execution + std::to_string(i)}};
      const auto actual = json(te::PolicyRuntime(proto<te::v1::RuntimePolicy>(child["policy"])).decide(input));
      auto event = context; event["event_id"] = context.str("step_id") + "-query"; event["kind"] = "custom";
      event["attributes"] = Json{{"type", "policy.query"}, {"decision", actual}}; events.push(event);
      event = context; event["event_id"] = context.str("step_id") + "-loop"; event["kind"] = "loop.decision";
      event["decision"] = "continue"; events.push(event);
      Json review{{"iteration_id", iteration_identity(Json{{"run_id", "run"}}, context)},
          {"reason", "Verified necessary or redundant work"}};
      review["decision"] = execution == "required" && (i != 3 || third_required) ? "keep" : "waste";
      reviews.push(review);
      if(i == 4) {
        event = context; event["kind"] = "action.start"; event["action_id"] = execution + "-answer";
        event["attributes"] = Json{{"semantic", "model"}}; events.push(event);
        event["kind"] = "action.end"; event["status"] = "completed";
        event["attributes"]["executed"] = true;
        event["output"] = Json{{"choices", Json::Array{Json{{"finish_reason", "stop"},
            {"message", Json{{"content", "Results from completed work."}}}}}}}; events.push(event);
        if(final_tool && execution == "required") {
          event = context; event["kind"] = "action.start"; event["action_id"] = "required-write";
          event["attributes"] = Json{{"semantic", "action"}}; events.push(event);
        }
      }
    }
    if(omit_final_query) events.items().erase(std::remove_if(events.items().begin(), events.items().end(), [](const auto& event) {
      return event.str("execution_id") == "required" && event.str("iteration") == "4"
          && (event.str("kind") == "custom" || event.str("kind") == "loop.decision");
    }), events.items().end());
    events.push(Json{{"kind", "run.end"}, {"outcome", Json{{"execution_status", "completed"}}}});
    Report result; auto* trial = result.add_trials(); trial->set_variant_id("candidate"); trial->set_task_id("train-task");
    trial->set_split("train");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{
        {"trace", Json{{"identity", json(identity)}, {"events", events}}},
        {"loop_review", Json{{"status", "complete"}, {"observed_iterations", 8}, {"iteration_reviews", reviews}}}});
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
  auto untriggered = schedule_query; untriggered.mutable_actions(1)->clear_after();
  (*untriggered.mutable_actions(1)->mutable_arguments()->mutable_fields())["resource"].set_string_value("b.png");
  auto rewritten = options; auto* rewrite = rewritten.add_experience_overrides(); *rewrite->mutable_source_experience() = ref;
  rewrite->set_content("Use available evidence before requesting another attempt.");
  check = search.control_preflight(scheduled, artifact(rewritten), report(scheduled, untriggered));
  expect(check.flag("passed", false) && !check.has("pure_removal_no_effect"), "mixed L1 content rewrite was blocked as pure deletion");
  rewrite->set_content(snapshot.experiences(0).content()); rewrite->set_trigger_pattern("Missing image argument");
  check = search.control_preflight(scheduled, artifact(rewritten), report(scheduled, untriggered));
  expect(check.flag("passed", false) && !check.has("pure_removal_no_effect"), "mixed L1 trigger rewrite was blocked as pure deletion");
  auto disabled = options; disabled.clear_approved_experiences();
  check = search.control_preflight(scheduled, artifact(disabled), report(scheduled, untriggered));
  expect(check.flag("passed", false) && !check.has("pure_removal_no_effect"), "mixed L1 disable was blocked as pure deletion");

  // L1 消融不撤销控制来源：同一经验的文字可停用，已编译动作护栏继续工作。
  search.controls = Json{{"enabled", true}, {"prompt_file", "unused-in-unit-test"},
      {"actions", Json{{"render", Json::Array{"image"}}}}};
  const Json control_draft{{"action_rules", Json::Array{Json{{"memory_id", "removal-memory"},
      {"operation", "render"}, {"required_arguments", Json::Array{"image"}},
      {"match_mode", "predicate"}, {"reason", "Require the input image"}}}},
      {"loop_rules", Json::Array{}}};
  const auto control_options = apply_control_draft(snapshot, options, search.controls, control_draft);
  search.seed = Json{{"schema_version", 1}, {"agent", Json{{"revision", "ablation-fixture"}}},
      {"user_id", "user"}, {"level", "L3"}, {"policy", json(te::compile_runtime_policy(snapshot, control_options))}};
  const Json selected = Json::Array{"experience:removal-memory"};
  const Json remove{{"reason", "Compare the actual injection with no injection"},
      {"experience", Json{{"memory_id", "removal-memory"}, {"content", nullptr}}}};
  const auto ablated = search.mutate(search.seed, remove, selected);
  const auto ablated_policy = proto<te::v1::RuntimePolicy>(ablated["policy"]);
  expect(search.components(ablated)["experience:removal-memory"].null(), "disabled experience lost its null search axis");
  expect(ablated_policy.options().approved_experiences_size() == 1
      && ablated_policy.options().disabled_injections_size() == 1, "ablation revoked control source approval");
  expect(search.components(ablated)["action_rules"].dump() == search.components(search.seed)["action_rules"].dump(),
      "L1-only ablation mutated a sibling control component");
  expect(te::PolicyRuntime(ablated_policy).decide(action_query).actions(0).decision() == "reject",
      "L1 ablation disabled the generated action guard");
  expect(te::published_policy_compatible(ablated_policy, proto<te::v1::RuntimePolicy>(search.seed["policy"]), search.controls.dump()),
      "disabled injection was rejected on compatible bundle reload");
  auto llm = query("llm"); llm.mutable_advice_query()->set_task("Repeated failed work");
  expect(!te::PolicyRuntime(proto<te::v1::RuntimePolicy>(search.seed["policy"])).decide(llm).context().empty(),
      "ablation fixture never injected its source");
  expect(te::PolicyRuntime(ablated_policy).decide(llm).context().empty(), "ablated text was still injected");
  auto observed_ablation = report(ablated, llm);
  auto* ablation_trial = observed_ablation.mutable_trials(0);
  auto ablation_evidence = json(ablation_trial->evidence());
  ablation_evidence["trace"]["events"].push(Json{{"kind", "action.end"}, {"step_id", "step"},
      {"attributes", Json{{"executed", true}}}});
  *ablation_trial->mutable_evidence() = proto<google::protobuf::Struct>(ablation_evidence);
  expect(search.applied_changes(search.seed, observed_ablation)["train-task"].num("llm", 0) == 1,
      "zero new application receipts incorrectly rejected a real L1 removal");
  const Json restore{{"reason", "Restore the same authorized source"}, {"experience", Json{
      {"memory_id", "removal-memory"}, {"content", snapshot.experiences(0).content()}}}};
  const auto restored = search.mutate(ablated, restore, selected);
  expect(!restored["policy"]["options"].has("disabled_injections")
      && search.components(restored).dump() == search.components(search.seed).dump(), "disabled experience could not be restored");

  // 第二候选只在本经验实际应用且同试次有失败/已评审浪费时安排消融；不是因果结论。
  auto applied_report = report(search.seed, llm);
  auto* applied_trial = applied_report.mutable_trials(0);
  applied_trial->set_execution_status("completed"); applied_trial->set_verdict("failed");
  auto applied_evidence = json(applied_trial->evidence());
  applied_evidence["trace"]["events"].items()[1]["attributes"]["receipt"] = Json{{"boundary", "llm"},
      {"experiences", Json::Array{json(ref)}}};
  *applied_trial->mutable_evidence() = proto<google::protobuf::Struct>(applied_evidence);
  expect(!search.experience_ablation(search.seed, selected, applied_report).null(), "actual failed injection got no ablation opportunity");
  expect(search.experience_ablation(ablated, selected, applied_report).null(), "disabled experience was ablated again");
  expect(search.experience_ablation(search.seed, Json::Array{"action_rules"}, applied_report).null(), "control slot was forced into L1 ablation");
  applied_trial->set_verdict("passed");
  expect(search.experience_ablation(search.seed, selected, applied_report).null(), "successful unwasteful injection forced ablation");
  applied_evidence["loop_review"] = Json{{"status", "uncertain"}, {"count", 1}};
  *applied_trial->mutable_evidence() = proto<google::protobuf::Struct>(applied_evidence);
  expect(search.experience_ablation(search.seed, selected, applied_report).null(), "unknown loop review justified ablation");
  applied_evidence["loop_review"]["status"] = "complete";
  *applied_trial->mutable_evidence() = proto<google::protobuf::Struct>(applied_evidence);
  expect(!search.experience_ablation(search.seed, selected, applied_report).null(), "reviewed waste with injection got no ablation comparison");
  applied_evidence["trace"]["events"].items()[1]["application"]["status"] = "rejected";
  *applied_trial->mutable_evidence() = proto<google::protobuf::Struct>(applied_evidence);
  expect(search.experience_ablation(search.seed, selected, applied_report).null(), "unapplied experience was treated as a harmful injection");

  // 弃权后只试一个已评审控制假设；来源关联、编译、评测门禁仍使用现有契约。
  const Json probe_guard{{"kind", "action.end"}, {"event_id", "guard"}, {"actor_id", "worker"},
      {"execution_id", "execution"}, {"loop_id", "loop"}, {"iteration", 2}, {"semantic", "action"},
      {"guard_only", true}, {"executed", false}, {"operation", "render"},
      {"action_control", Json{{"decision", "allow"}, {"query_event_id", "guard-query"},
          {"target_scope", "assistant"}, {"prior_operation_rounds", 1}, {"reviewed_budget_prior", 1}}}};
  Json probe_input{{"phase", "reflection"}, {"selected_components", Json::Array{"action_rules"}},
      {"components", search.components(search.seed)},
      {"experiences", Json::Array{Json{{"memory_id", "removal-memory"}, {"scope", "assistant"}}}},
      {"controls", Json{{"action_primitives", Json::Array{Json{{"primitive_id", "operation_budget"}}}},
          {"limits", Json{{"min_iterations", 1}}}}}};
  Json probe_trial;
  probe_trial["evidence"]["runs"] = Json::Array{Json{{"run_id", "run"}, {"actions", Json::Array{probe_guard}}}};
  probe_input["feedback"] = Json::Array{Json{{"task_id", "train-task"}, {"trial", probe_trial}}};
  const auto seed_before_probe = search.seed.dump(), input_before_probe = probe_input.dump();
  auto probe = search.reviewed_control_probe(search.seed, search.seed, probe_input, Report{}, 0);
  expect(!probe.null() && probe["detail"]["source"].str("association") == "existing_control"
      && probe["detail"].str("hypothesis_origin") == "training_review", "reviewed guard with exact control source got no probe");
  const auto probe_draft = expand_control_updates(probe["draft"], search.components(search.seed), Json::Array{"action_rules"});
  const auto probe_artifact = search.mutate(search.seed, probe_draft, Json::Array{"action_rules"});
  const auto probe_rules = search.components(probe_artifact)["action_rules"];
  expect(probe_rules.items().size() == 2 && probe_rules.items().back().str("operation") == "render"
      && probe_rules.items().back().num("max_attempts", 0) == 1,
      "probe did not compile the reviewed operation or lost the independent existing guard");
  expect(search.reviewed_control_probe(search.seed, probe_artifact, probe_input, Report{}, 0).null()
      && search.reviewed_control_probe(search.seed, ablated, probe_input, Report{}, 0).null(),
      "probe overwrote a substantive control or L1 ablation proposal");
  auto reason_only = search.components(search.seed); reason_only["action_rules"].items()[0]["reason"] = "Another explanation";
  const auto reworded = search.compose(search.seed, reason_only);
  expect(!search.reviewed_control_probe(search.seed, reworded, probe_input, Report{}, 0).null(),
      "reason-only metadata prevented exploration of a real reviewed opportunity");
  search.tree["nodes"] = Json::Array{Json{{"control_probe", probe["detail"]}}};
  auto new_request = probe_input; new_request["phase"] = "repair_control_binding";
  search.tree = Json::parse(search.tree.dump());
  expect(search.reviewed_control_probe(search.seed, search.seed, new_request, Report{}, 1).null(),
      "checkpoint recovery or new request-local binding ID repeated the same control hypothesis");
  search.tree = Json::Object{};
  auto ordinary = probe_input;
  ordinary["feedback"].items()[0]["trial"]["evidence"]["runs"].items()[0]["actions"].items()[0]
      ["action_control"].fields().erase("reviewed_budget_prior");
  expect(search.reviewed_control_probe(search.seed, search.seed, ordinary, Report{}, 0).null(),
      "ordinary repeated work without full reviewed redundancy triggered a deterministic budget");
  auto unselected = probe_input; unselected["selected_components"] = selected;
  expect(search.reviewed_control_probe(search.seed, search.seed, unselected, Report{}, 0).null(),
      "control probe stole an L1-only component selection");

  // 无现有同操作来源时，只接受同执行同轮且确实交给模型的经验回执。
  auto injection_input = probe_input; injection_input["components"]["action_rules"] = Json::Array{};
  const auto prepared_probe = observed_operation_bindings(injection_input, 65536);
  const auto binding = prepared_probe["controls"]["editable_bindings"].items().front();
  auto event = [](const char* kind, const char* id) {
    return Json{{"kind", kind}, {"event_id", id}, {"actor_id", "worker"}, {"execution_id", "execution"},
        {"loop_id", "loop"}, {"iteration", 2}, {"step_id", "model-step"}};
  };
  auto raw_anchor = event("action.end", "guard");
  auto model_event = event("action.end", "model"); model_event["attributes"] = Json{{"semantic", "model"}, {"executed", true}};
  auto query_event = event("custom", "query"); query_event["attributes"] = Json{{"type", "policy.query"},
      {"decision", Json{{"query", Json{{"boundary", "llm"}, {"advice_query", Json{
          {"target_scope", "assistant"}, {"query_id", "llm-query"}}}}}}}};
  auto receipt_event = event("advice.applied", "receipt");
  receipt_event["application"] = Json{{"status", "applied"}, {"query_id", "llm-query"}};
  receipt_event["attributes"] = Json{{"receipt", Json{{"boundary", "llm"}, {"experiences", Json::Array{json(ref)}}}}};
  Json source_events = Json::Array{raw_anchor, query_event, model_event, receipt_event};
  auto source_report = [&](const Json& events) {
    Report result; auto* trial = result.add_trials(); trial->set_variant_id("candidate");
    trial->set_task_id("train-task"); trial->set_execution_status("completed");
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"trace", Json{{"identity", json(identity)}, {"events", events}}}});
    return result;
  };
  const auto actual_source = control_probe_source(search.seed, prepared_probe, binding, source_report(source_events));
  expect(actual_source.str("association") == "same_iteration_application"
      && actual_source.str("application_event_id") == "receipt" && actual_source.str("model_event_id") == "model",
      "same-iteration actual L1 adoption was not associated with the reviewed guard");
  expect(!search.reviewed_control_probe(search.seed, search.seed, injection_input, source_report(source_events), 0).null(),
      "actual L1 adoption could not supply an explicitly associated control hypothesis");
  expect(control_probe_source(search.seed, prepared_probe, binding, Report{}).null(),
      "arbitrary approved same-scope memory was treated as a control source");
  for(int mode = 0; mode < 9; ++mode) {
    auto wrong = source_events;
    if(mode == 0) wrong.items()[3]["execution_id"] = "sibling";
    if(mode == 1) wrong.items()[3]["actor_id"] = "other-worker";
    if(mode == 2) wrong.items()[3]["iteration"] = 3;
    if(mode == 3) wrong.items()[3]["application"]["status"] = "rejected";
    if(mode == 4) wrong.items()[2]["attributes"]["executed"] = false;
    if(mode == 5) wrong.items()[1]["attributes"]["decision"]["query"]["advice_query"]["target_scope"] = "other-scope";
    if(mode == 6) wrong.items()[3]["attributes"]["receipt"]["experiences"].items()[0]["content_revision"] = "stale";
    if(mode == 7) { wrong.items()[1]["attributes"]["decision"]["query"]["advice_query"]["query_id"] = "";
      wrong.items()[3]["application"]["query_id"] = ""; }
    if(mode == 8) for(auto& value : wrong.items()) value["step_id"] = "";
    expect(control_probe_source(search.seed, prepared_probe, binding, source_report(wrong)).null(),
        "missing adoption or sibling/stale L1 receipt supplied a control source");
  }
  auto unauthorized_parent = search.seed; unauthorized_parent["policy"]["options"]["approved_experiences"] = Json::Array{};
  expect(control_probe_source(unauthorized_parent, prepared_probe, binding, source_report(source_events)).null(),
      "historical application bypassed current source approval");

  // L3 的自动假设保留必要文本收尾；已评审普通尾部仍须真实来源、必要轮下界和去重。
  auto finalization_options = control_options;
  auto* host_loop = finalization_options.add_loop_rules();
  host_loop->set_rule_id("host-loop"); host_loop->set_target_scope("assistant"); host_loop->set_actor_id("worker");
  host_loop->set_max_iterations(8); host_loop->set_max_no_progress(2); host_loop->set_max_replans(1);
  host_loop->set_no_progress_decision("stop"); host_loop->set_reason("Host loop safety budget");
  host_loop->set_match_mode("predicate");
  auto finalization_parent = search.seed;
  finalization_parent["policy"] = json(te::compile_runtime_policy(snapshot, finalization_options));
  auto finalization_input = injection_input;
  finalization_input["components"] = search.components(finalization_parent);
  finalization_input["selected_components"] = Json::Array{"loop_rules"};
  finalization_input["controls"]["aggressive_controls"] = true;
  finalization_input["controls"]["loop_primitives"] = Json::Array{Json{{"primitive_id", "bounded_iteration_cap"}}};
  finalization_input["controls"]["base_loop_rules"] = Json::Array{Json{{"target_scope", "assistant"},
      {"actor_id", "worker"}, {"max_iterations", 8}, {"max_no_progress", 2}, {"max_replans", 1}}};
  auto& loop_anchor = finalization_input["feedback"].items()[0]["trial"]["evidence"]["runs"].items()[0]["actions"].items()[0];
  loop_anchor["kind"] = "loop.decision";
  loop_anchor["loop_control"] = Json{{"target_scope", "assistant"}, {"iteration", 2},
      {"reviewed_waste_after", 1}, {"require_finalization", true}, {"proposed_decision", "continue"}};
  auto finalization_events = source_events; finalization_events.items()[0]["kind"] = "loop.decision";
  const auto loop_probe = search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
      source_report(finalization_events), 0);
  expect(!loop_probe.null() && loop_probe["draft"]["control_updates"].items().front()["value"].flag("require_finalization", false)
      && loop_probe["draft"]["control_updates"].items().front()["value"].str("actor_id") == "worker"
      && loop_probe["draft"]["control_updates"].items().front()["value"].num("max_iterations", 0) == 1,
      "reviewed finalization hypothesis dropped its actor, safe bound or mandatory final response");
  loop_anchor["loop_control"].fields().erase("require_finalization");
  const auto ordinary_loop_probe = search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
      source_report(finalization_events), 0);
  expect(!ordinary_loop_probe.null()
      && !ordinary_loop_probe["draft"]["control_updates"].items().front()["value"].flag("require_finalization", false)
      && ordinary_loop_probe["detail"].str("key") != loop_probe["detail"].str("key"),
      "reviewed ordinary loop cap was skipped or shared the finalization hypothesis key");
  search.tree["nodes"] = Json::Array{Json{{"control_probe", loop_probe["detail"]}}};
  expect(!search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
      source_report(finalization_events), 1).null(), "finalization hypothesis suppressed an independent ordinary cap");
  search.tree["nodes"].push(Json{{"control_probe", ordinary_loop_probe["detail"]}});
  expect(search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
      source_report(finalization_events), 2).null(), "ordinary loop cap was probed again after checkpoint recovery");
  search.tree = Json::Object{};
  expect(search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input, Report{}, 0).null(),
      "ordinary loop cap bypassed actual source attribution");
  loop_anchor["loop_control"]["required_iteration_floor"] = 2;
  expect(search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
      source_report(finalization_events), 0).null(), "ordinary loop cap truncated a known necessary iteration");
  loop_anchor["loop_control"].fields().erase("required_iteration_floor");
  loop_anchor["loop_control"].fields().erase("reviewed_waste_after");
  expect(search.reviewed_control_probe(finalization_parent, finalization_parent, finalization_input,
      source_report(finalization_events), 0).null(), "unreviewed high iteration count justified an ordinary loop probe");
  expect(search.seed.dump() == seed_before_probe && probe_input.dump() == input_before_probe,
      "control probing mutated the parent policy or source request");
}

void reviewed_operation_budget_bindings() {
  auto fixture = [](const std::vector<std::string>& decisions, const std::vector<std::size_t>& priors,
      int rejected = -1, int unidentified = -1, int mixed = -1) {
    Json trial{{"evidence", Json{{"runs", Json::Array{}}}}};
    Json raw{{"loop_review", Json{{"status", "complete"}, {"iteration_reviews", Json::Array{}}}}};
    append_reviewed_group(trial, raw, "run-a", "execution", "loop", decisions);
    auto& rows = trial["evidence"]["runs"].items()[0]["actions"].items();
    for(std::size_t i = 0; i < decisions.size(); ++i) {
      auto& action = rows[i * 2 + 1];
      action["operation"] = "render"; action["guard_only"] = true; action["executed"] = false;
      action["action_control"] = Json{{"decision", int(i) == rejected ? "reject" : "allow"},
          {"query_event_id", "q-" + std::to_string(i)}, {"target_scope", "assistant"},
          {"prior_operation_rounds", priors[i]}};
      if(int(i) == unidentified) action.fields().erase("iteration");
    }
    if(mixed >= 0) {
      auto other = rows[mixed * 2 + 1]; other["operation"] = "independent";
      other["action_control"]["query_event_id"] = "independent-query"; rows.push_back(other);
    }
    bind_iteration_assessments(trial, raw);
    return trial;
  };
  auto prepared = [](const Json& trial) {
    return observed_operation_bindings(Json{{"selected_components", Json::Array{"action_rules"}},
        {"controls", Json{{"action_primitives", Json::Array{Json{{"primitive_id", "operation_budget"}}}}}},
        {"feedback", Json::Array{Json{{"trial", trial}}}}}, 65536);
  };
  auto early = fixture({"keep", "waste", "waste"}, {0, 1, 2});
  const auto input = prepared(early);
  const auto& binding = input["controls"]["editable_bindings"].items().front();
  expect(binding.num("prior_operation_rounds", 0) == 1 && binding.flag("reviewed_redundant_suffix", false),
      "typed binding did not prefer the earliest reviewed safe budget");
  expect(input["controls"]["action_primitives"].items()[0]["observed_bindings"].items()[0]
      .num("prior_operation_rounds", 0) == 2, "legacy maximum binding changed");
  expect(observed_operation_bindings(input, 65536).dump() == input.dump(), "early binding changed on reprojection");
  for(const auto& trial : {fixture({"keep", "waste", "keep"}, {0, 1, 2}),
      fixture({"keep", "waste"}, {1, 1}), fixture({"keep", "waste", "unknown"}, {0, 1, 2}),
      fixture({"keep", "waste", "waste"}, {0, 1, 2}, -1, 2)}) {
    const auto result = prepared(trial);
    if(result["controls"]["editable_bindings"].array())
      for(const auto& item : result["controls"]["editable_bindings"].items())
        expect(!item.flag("reviewed_redundant_suffix", false), "unsafe or incomplete suffix authorized an early cap");
  }
  const auto rejected = prepared(fixture({"keep", "waste", "waste"}, {0, 1, 2}, 1));
  expect(rejected["controls"]["editable_bindings"].items()[0].num("prior_operation_rounds", 0) == 2,
      "an already rejected guard was treated as a new opportunity");
  auto hidden_keep = fixture({"keep", "waste", "keep"}, {0, 1, 2});
  auto& hidden_rows = hidden_keep["evidence"]["runs"].items()[0]["actions"].items();
  hidden_rows.resize(4);
  expect(!prepared(hidden_keep)["controls"].has("editable_bindings"),
      "display clipping erased the necessary third-attempt floor");
  auto separate = fixture({"keep", "waste"}, {0, 1});
  separate["evidence"]["runs"].push(fixture({"keep", "keep"}, {0, 1})["evidence"]["runs"].items()[0]);
  separate["evidence"]["runs"].items()[1]["run_id"] = "run-b";
  expect(!prepared(separate)["controls"].has("editable_bindings"),
      "one run's redundant second attempt overrode another run's necessary second attempt");
  auto later = fixture({"keep", "waste"}, {0, 3});
  later["evidence"]["runs"].push(separate["evidence"]["runs"].items()[1]);
  const auto later_budget = prepared(later)["controls"]["editable_bindings"].items()[0]["budget"];
  expect(later_budget.num("min", 0) == 2 && later_budget.num("max", 0) == 3,
      "independent runs had their required attempt counters added instead of taking the maximum");
  auto clipped = prepared(early);
  merge_action_requirements(clipped["controls"]["action_requirements"],
      fixture({"keep", "keep"}, {0, 1})["evidence"]["action_requirements"]);
  expect(observed_operation_bindings(clipped, 65536)["controls"]["editable_bindings"].items()[0]["budget"].num("min", 0) == 2,
      "omitting a feedback row erased its complete necessary-attempt summary");
  clipped["controls"]["action_requirements"].items()[0]["operation"] = "another-operation";
  expect(observed_operation_bindings(clipped, 65536)["controls"].has("editable_bindings"),
      "another operation's requirement blocked the reviewed budget");
  const auto mixed = prepared(fixture({"keep", "keep", "waste"}, {0, 1, 2}, -1, -1, 1));
  expect(mixed["controls"]["editable_bindings"].items()[0]["budget"].num("min", 0) == 1,
      "mixed-iteration keep was falsely assigned to every operation");
  const auto zero = prepared(fixture({"waste"}, {0}));
  expect(!zero["controls"].has("editable_bindings"), "a zero attempt budget was offered");
}

void observed_operation_binding_views() {
  auto guard = [](const std::string& operation, const std::string& scope, std::size_t rounds) {
    return Json{{"kind", "action.end"}, {"semantic", "action"}, {"guard_only", true}, {"executed", false},
        {"operation", operation}, {"action_control", Json{{"decision", "allow"}, {"query_event_id", "query-1"},
            {"target_scope", scope}, {"prior_operation_rounds", rounds}}}};
  };
  auto row = [](const Json& actions) {
    return Json{{"trial", Json{{"evidence", Json{{"runs", Json::Array{Json{{"actions", actions}}}}}}}}};
  };
  const Json input{{"selected_components", Json::Array{"action_rules"}},
      {"controls", Json{{"action_primitives", Json::Array{Json{{"primitive_id", "terminal_failure"}},
          Json{{"primitive_id", "operation_budget"}}}}}},
      {"feedback", Json::Array{row(Json::Array{guard("fetch", "scope-a", 1), guard("render", "scope-a", 2)}),
          row(Json::Array{guard("fetch", "scope-a", 3), guard("fetch", "scope-b", 2)})}}};
  auto bindings = [](const Json& packet) -> const Json& {
    return packet["controls"]["action_primitives"].items()[1]["observed_bindings"];
  };
  const auto original = input.dump();
  const auto prepared = observed_operation_bindings(input, 65536);
  expect(bindings(prepared).items().size() == 3 && input.dump() == original
      && prepared["feedback"].dump() == input["feedback"].dump(), "binding projection changed input or feedback");
  expect(bindings(prepared).items()[0].str("operation") == "fetch"
      && bindings(prepared).items()[0].str("target_scope") == "scope-a"
      && bindings(prepared).items()[0].num("prior_operation_rounds", 0) == 3
      && bindings(prepared).items()[0].str("evidence_ref") == "/feedback/1/trial/evidence/runs/0/actions/0",
      "binding summed independent runs or lost the exact maximum observation");
  expect(observed_operation_bindings(prepared, 65536).dump() == prepared.dump(), "binding projection was not idempotent");

  // repair 和 COMBEE map 只保留当前子组；reduce/source repair 无反馈时不能继承旧绑定。
  auto subset = prepared; subset["feedback"] = Json::Array{input["feedback"].items()[0]};
  const auto projected_subset = observed_operation_bindings(subset, 65536);
  expect(bindings(projected_subset).items().size() == 2
      && bindings(projected_subset).items()[0].str("operation") == "render"
      && bindings(projected_subset).items()[1].num("prior_operation_rounds", 0) == 1
      && bindings(projected_subset).items()[1].str("evidence_ref") == "/feedback/0/trial/evidence/runs/0/actions/0",
      "child reflection retained another feedback group's operation binding");
  auto no_feedback = prepared; no_feedback.fields().erase("feedback");
  expect(bindings(observed_operation_bindings(no_feedback, 65536)).null(), "feedback-free reflection retained old bindings");
  auto no_actions = prepared; no_actions["selected_components"] = Json::Array{"loop_rules"};
  expect(bindings(observed_operation_bindings(no_actions, 65536)).null(), "unselected action component gained operation bindings");
  auto unavailable = prepared;
  unavailable["controls"]["action_primitives"].items()[1]["primitive_id"] = "repeat_failed_operation";
  expect(bindings(observed_operation_bindings(unavailable, 65536)).null(), "missing primitive retained operation budget bindings");
  auto no_controls = input; no_controls.fields().erase("controls");
  expect(observed_operation_bindings(no_controls, 65536).dump() == no_controls.dump(), "binding helper manufactured controls");

  const auto valid = guard("arbitrary-operation", "scope-a", 1);
  std::vector<Json> invalid;
  auto wrong = valid; wrong["executed"] = true; invalid.push_back(wrong);
  wrong = valid; wrong.fields().erase("executed"); invalid.push_back(wrong);
  wrong = valid; wrong["guard_only"] = false; invalid.push_back(wrong);
  wrong = valid; wrong["semantic"] = "model"; invalid.push_back(wrong);
  wrong = valid; wrong["kind"] = "action.start"; invalid.push_back(wrong);
  wrong = valid; wrong["action_control"]["decision"] = "reject"; invalid.push_back(wrong);
  wrong = valid; wrong["action_control"]["prior_operation_rounds"] = 0; invalid.push_back(wrong);
  wrong = valid; wrong["action_control"].fields().erase("query_event_id"); invalid.push_back(wrong);
  wrong = valid; wrong["action_control"]["target_scope"] = ""; invalid.push_back(wrong);
  wrong = valid; wrong["operation"] = ""; invalid.push_back(wrong);
  wrong = valid; wrong["action_control"] = nullptr; invalid.push_back(wrong);
  for(const auto& action : invalid) {
    auto packet = prepared; packet["feedback"] = Json::Array{row(Json::Array{action})};
    const auto result = observed_operation_bindings(packet, 65536);
    expect(bindings(result).null() && result["feedback"].dump() == packet["feedback"].dump(),
        "non-executable or absent evidence manufactured an operation binding");
  }
  auto crowded = input;
  for(std::size_t i = 0; i < 5; ++i)
    crowded["feedback"].push(row(Json::Array{guard("operation-" + std::to_string(i), "scope-a", i + 4)}));
  const auto capped = observed_operation_bindings(crowded, 65536);
  expect(bindings(capped).items().size() == 4 && bindings(capped).items()[0].num("prior_operation_rounds", 0) == 8,
      "operation binding cap or strongest-observation ordering changed");

  // 精确满额仍发送原反馈；只够一条时不加入第二条，也不以旧绑定挤占空间。
  expect(observed_operation_bindings(input, original.size()).dump() == original,
      "full input budget was exceeded or original feedback was discarded");
  auto one = prepared; one["controls"]["action_primitives"].items()[1]["observed_bindings"].items().resize(1);
  const auto limited = observed_operation_bindings(prepared, one.dump().size());
  expect(bindings(limited).items().size() == 1 && limited.dump().size() <= one.dump().size()
      && limited["feedback"].dump() == input["feedback"].dump(), "binding projection exceeded the remaining byte budget");
  expect(observed_operation_bindings(prepared, original.size()).dump() == original,
      "stale bindings prevented a full-budget request from remaining usable");
}

void typed_control_binding_updates() {
  const auto binding = [](const Json& packet, const std::string& component) {
    for(const auto& item : packet["controls"]["editable_bindings"].items())
      if(item.str("component") == component) return item;
    throw std::runtime_error("missing typed control binding");
  };
  const auto update = [](const Json& item, const std::string& memory, const char* field, std::size_t value) {
    return Json{{"reason", "Use the observed control point."}, {"control_binding_updates", Json::Array{
        Json{{"binding_id", item["binding_id"]}, {"memory_id", memory}, {field, value}, {"reason", "Bound observed waste."}}}}};
  };
  const auto invalid = [](const Json& draft, const Json& packet) {
    try { resolve_control_binding_updates(draft, packet); }
    catch(const Error& error) { return std::string(error.what()) == "control_binding_invalid"; }
    return false;
  };
  const auto diagnose = [](const Json& draft, const Json& packet) {
    Json diagnostic;
    try { resolve_control_binding_updates(draft, packet, &diagnostic); }
    catch(const Error& error) {
      expect(std::string(error.what()) == "control_binding_invalid", "binding diagnostics changed the public error code");
      expect(diagnostic.object() && diagnostic.has("reason") && diagnostic.has("update_index"),
          "binding error lacked bounded repair diagnostics");
      expect(diagnostic.dump().find("private-marker") == std::string::npos,
          "binding diagnostics leaked a model ID, field name or reason");
      return diagnostic;
    }
    throw std::runtime_error("diagnostic fixture did not fail");
  };
  const auto row = [](Json actions) {
    return Json{{"trial", Json{{"evidence", Json{{"runs", Json::Array{Json{{"actions", std::move(actions)}}}}}}}}};
  };
  const Json guard{{"kind", "action.end"}, {"semantic", "action"}, {"guard_only", true}, {"executed", false},
      {"operation", "forge-image-gen"}, {"action_control", Json{{"decision", "allow"}, {"query_event_id", "query-1"},
          {"target_scope", "scope-a"}, {"prior_operation_rounds", 2}}}};
  Json action_input{{"phase", "reflection"}, {"selected_components", Json::Array{"action_rules"}},
      {"components", Json{{"action_rules", Json::Array{}}}},
      {"experiences", Json::Array{Json{{"memory_id", "wrong-scope"}, {"scope", "scope-b"}}}},
      {"controls", Json{{"action_primitives", Json::Array{Json{{"primitive_id", "operation_budget"}}}},
          {"control_sources", Json::Array{Json{{"memory_id", "control-memory"}, {"scope", "scope-a"}}}},
          {"limits", Json{{"min_iterations", 1}}}}},
      {"feedback", Json::Array{row(Json::Array{guard})}}};
  auto prepared = observed_operation_bindings(action_input, 65536);
  auto action = binding(prepared, "action_rules");
  expect(action.str("operation") == "forge-image-gen" && action.num("prior_operation_rounds", 0) == 2
      && action.str("binding_id").find("control:") == 0, "action binding lost its trusted operation identity");
  expect(action["budget"].dump() == Json({{"field", "max_attempts"}, {"min", 1}, {"max", 2}}).dump(),
      "request did not expose the exact legal operation budget");
  auto draft = update(action, "control-memory", "max_attempts", 2);
  auto resolved = resolve_control_binding_updates(draft, prepared);
  const auto& action_change = resolved["control_updates"].items().front();
  expect(!resolved.has("control_binding_updates") && action_change.str("component") == "action_rules"
      && action_change.str("op") == "add" && action_change["value"].str("operation") == "forge-image-gen"
      && action_change["value"].num("max_attempts", 0) == 2
      && action_change["value"]["attempt_key_arguments"].items().empty(),
      "typed action binding did not derive the concrete operation budget");

  // 纠正请求只拿到固定码、下标和可信范围，不回显模型输出中的任意字段。
  auto diagnostic_draft = draft;
  diagnostic_draft["control_binding_updates"].items()[0]["binding_id"] = "private-marker-id";
  auto diagnostic = diagnose(diagnostic_draft, prepared);
  expect(diagnostic.str("reason") == "unknown_binding_id" && diagnostic.num("update_index", 99) == 0
      && diagnostic.fields().size() == 2, "unknown binding diagnostic included unrelated or raw fields");
  auto stale_packet = prepared; stale_packet["phase"] = "changed";
  expect(diagnose(draft, stale_packet).str("reason") == "stale_binding_id", "stale binding context was not distinguished");
  diagnostic_draft = draft; diagnostic_draft["control_binding_updates"].items()[0]["private-marker-field"] = "private-marker-value";
  expect(diagnose(diagnostic_draft, prepared).str("reason") == "unknown_update_field", "unknown model field was not diagnosed safely");
  diagnostic_draft = draft; diagnostic_draft["control_updates"] = Json::Array{};
  expect(diagnose(diagnostic_draft, prepared).str("reason") == "mixed_protocol", "mixed protocols were not diagnosed");
  diagnostic_draft = draft; diagnostic_draft["control_binding_updates"].items()[0]["memory_id"] = "private-marker-source";
  expect(diagnose(diagnostic_draft, prepared).str("reason") == "source_missing_or_ambiguous", "missing source was not distinguished");
  expect(diagnose(update(action, "wrong-scope", "max_attempts", 2), prepared).str("reason") == "source_scope_mismatch",
      "source scope mismatch was hidden behind a generic failure");
  diagnostic_draft = draft; diagnostic_draft["control_binding_updates"].push(diagnostic_draft["control_binding_updates"].items().front());
  diagnostic = diagnose(diagnostic_draft, prepared);
  expect(diagnostic.str("reason") == "duplicate_binding_update" && diagnostic.num("update_index", 99) == 1,
      "diagnostic did not identify the failing update index");
  for(const auto& bad : {Json(3), Json(-1), Json(10001)}) {
    diagnostic_draft = draft; diagnostic_draft["control_binding_updates"].items()[0]["max_attempts"] = bad;
    diagnostic_draft["control_binding_updates"].items()[0]["reason"] = "private-marker-reason";
    diagnostic = diagnose(diagnostic_draft, prepared);
    expect(diagnostic.str("reason") == "budget_out_of_range" && diagnostic["budget"].dump() == action["budget"].dump(),
        "range repair did not receive the same trusted limits as the original binding");
  }
  for(const auto& bad : {Json(1.5), Json("private-marker-number")}) {
    diagnostic_draft = draft; diagnostic_draft["control_binding_updates"].items()[0]["max_attempts"] = bad;
    expect(diagnose(diagnostic_draft, prepared).str("reason") == "budget_not_integer", "noninteger budget was not diagnosed");
  }
  diagnostic = Json{{"stale", "private-marker-old-diagnostic"}};
  resolve_control_binding_updates(draft, prepared, &diagnostic);
  expect(diagnostic.null(), "a valid retry retained the prior error diagnostic");

  auto wrong = draft; wrong["control_binding_updates"].items()[0]["operation"] = "run_skills";
  expect(invalid(wrong, prepared), "model-supplied operation bypassed typed action identity");
  wrong = update(action, "control-memory", "max_attempts", 3);
  expect(invalid(wrong, prepared), "an action budget that cannot trigger in the observed guard was accepted");
  wrong = update(action, "wrong-scope", "max_attempts", 2);
  expect(invalid(wrong, prepared), "a binding was moved to another approved source scope");
  auto repair_input = action_input; repair_input["phase"] = "repair_control_binding";
  auto repaired = observed_operation_bindings(repair_input, 65536);
  expect(binding(repaired, "action_rules").str("binding_id") != action.str("binding_id")
      && invalid(draft, repaired), "a request-local binding survived a repair phase change");

  auto existing_action = action_input;
  existing_action["components"]["action_rules"] = Json::Array{Json{{"memory_id", "control-memory"},
      {"operation", "forge-image-gen"}, {"max_attempts", 2}, {"attempt_key_arguments", Json::Array{"target"}},
      {"required_arguments", Json::Array{}}, {"max_failed_attempts", 0}, {"reason", "old"}, {"match_mode", "predicate"}}};
  auto existing_prepared = observed_operation_bindings(existing_action, 65536);
  auto tightened = resolve_control_binding_updates(
      update(binding(existing_prepared, "action_rules"), "control-memory", "max_attempts", 1), existing_prepared);
  const auto& tightened_action = tightened["control_updates"].items().front();
  expect(tightened_action.str("op") == "set" && tightened_action.num("index", 99) == 0
      && tightened_action["value"]["attempt_key_arguments"].items().front().text() == "target",
      "action set discarded the existing target identity");
  existing_action["components"]["action_rules"].items()[0]["max_attempts"] = 1;
  existing_prepared = observed_operation_bindings(existing_action, 65536);
  expect(binding(existing_prepared, "action_rules")["budget"].num("max", 0) == 1,
      "published budget ignored the stricter parent rule");
  expect(invalid(update(binding(existing_prepared, "action_rules"), "control-memory", "max_attempts", 2),
      existing_prepared), "typed action update relaxed an existing parent budget");

  const Json keep{{"kind", "loop.decision"}, {"actor_id", "outfit-stylist"}, {"execution_id", "required"},
      {"iteration_assessment", Json{{"decision", "keep"}}},
      {"loop_control", Json{{"target_scope", "scope-a"}, {"pending_actions", false},
          {"proposed_decision", "continue"}, {"iteration", 2}}}};
  const Json waste{{"kind", "loop.decision"}, {"actor_id", "outfit-stylist"}, {"execution_id", "redundant"},
      {"iteration_assessment", Json{{"decision", "waste"}}},
      {"loop_control", Json{{"target_scope", "scope-a"}, {"pending_actions", false},
          {"proposed_decision", "continue"}, {"iteration", 7}, {"reviewed_waste_after", 0}}}};
  Json loop_input{{"phase", "reflection"}, {"selected_components", Json::Array{"loop_rules"}},
      {"components", Json{{"loop_rules", Json::Array{}}}}, {"experiences", Json::Array{}},
      {"controls", Json{{"aggressive_controls", true},
          {"loop_primitives", Json::Array{Json{{"primitive_id", "bounded_iteration_cap"}}}},
          {"control_sources", Json::Array{Json{{"memory_id", "control-memory"}, {"scope", "scope-a"}}}},
          {"base_loop_rules", Json::Array{Json{{"target_scope", "scope-a"}, {"actor_id", ""},
              {"max_iterations", 12}, {"max_no_progress", 1}, {"max_replans", 1},
              {"replan_on_failure", false}, {"no_progress_decision", "replan"}}}},
          {"limits", Json{{"min_iterations", 1}}}}},
      {"feedback", Json::Array{row(Json::Array{keep, waste})}}};
  prepared = observed_operation_bindings(loop_input, 65536);
  auto loop = binding(prepared, "loop_rules");
  expect(loop["budget"].str("field") == "max_iterations" && loop["budget"].num("max", 0) == 6,
      "loop binding failed to exclude the first nontriggering budget");
  expect(loop["budget"].num("current", 0) == 12, "loop binding hid the effective parent budget");
  auto narrow_loop = loop_input;
  auto& narrow_actions = narrow_loop["feedback"].items()[0]["trial"]["evidence"]["runs"].items()[0]["actions"].items();
  narrow_actions[0]["loop_control"]["iteration"] = 3;
  narrow_actions[1]["loop_control"]["iteration"] = 4;
  auto narrow_prepared = observed_operation_bindings(narrow_loop, 65536);
  const auto narrow_budget = binding(narrow_prepared, "loop_rules")["budget"];
  expect(narrow_budget.num("current", 0) == 12 && narrow_budget.num("min", 0) == 3 && narrow_budget.num("max", 0) == 3,
      "singleton candidate range was confused with the current parent budget");
  narrow_loop["components"]["loop_rules"] = Json::Array{Json{{"memory_id", "control-memory"},
      {"actor_id", "other-worker"}, {"max_iterations", 9}, {"match_mode", "predicate"}}};
  expect(binding(observed_operation_bindings(narrow_loop, 65536), "loop_rules")["budget"].num("current", 0) == 12,
      "another actor's loop rule replaced the inherited current budget");
  auto& override = narrow_loop["components"]["loop_rules"].items()[0];
  override["actor_id"] = "outfit-stylist";
  expect(binding(observed_operation_bindings(narrow_loop, 65536), "loop_rules")["budget"].num("current", 0) == 9,
      "same-actor unconditional override was omitted from the current budget");
  override["error_codes"] = Json::Array{"TransientFailure"};
  expect(!binding(observed_operation_bindings(narrow_loop, 65536), "loop_rules")["budget"].has("current"),
      "conditional override was presented as an unconditional current budget");
  narrow_loop["components"]["loop_rules"] = Json::Array{};
  narrow_loop["controls"]["base_loop_rules"].items()[0]["after_action_rejection"] = true;
  expect(!binding(observed_operation_bindings(narrow_loop, 65536), "loop_rules")["budget"].has("current"),
      "conditional base rule was presented as an unconditional current budget");
  auto safe_floor = loop; safe_floor["required_iteration_floor"] = 4;
  expect(control_binding_budget(prepared, safe_floor).num("min", 0) == 4,
      "trusted necessary-work floor was omitted from the advertised and validated budget");
  safe_floor["require_finalization"] = true;
  expect(control_binding_budget(prepared, safe_floor).num("min", 0) == 4,
      "finalization flag alone erased a verified tool-work floor");
  expect(loop.str("actor_id") == "outfit-stylist" && loop.num("observed_iteration", 0) == 7
      && loop.has("reviewed_waste_after") && loop.num("reviewed_waste_after", 99) == 0
      && loop.num("highest_reviewed_keep_iteration", 0) == 2,
      "loop binding lost actor, reviewed suffix or quality context");
  resolved = resolve_control_binding_updates(update(loop, "control-memory", "max_iterations", 2), prepared);
  const auto& loop_change = resolved["control_updates"].items().front();
  expect(loop_change.str("component") == "loop_rules" && loop_change.str("op") == "add"
      && loop_change["value"].str("actor_id") == "outfit-stylist"
      && loop_change["value"].num("max_iterations", 0) == 2
      && loop_change["value"].num("max_no_progress", 0) == 1
      && loop_change["value"].num("max_replans", 0) == 1,
      "typed loop binding did not inherit the effective base rule");
  expect(invalid(update(loop, "control-memory", "max_iterations", 1), prepared),
      "typed cap ignored necessary work in another same-actor execution");
  auto finalization_input = loop_input;
  finalization_input["feedback"].items()[0]["trial"]["evidence"]["runs"].items()[0]["actions"].items()[1]
      ["loop_control"]["require_finalization"] = true;
  auto finalization_prepared = observed_operation_bindings(finalization_input, 65536);
  auto finalization_binding = binding(finalization_prepared, "loop_rules");
  expect(finalization_binding.flag("require_finalization", false), "typed binding lost its finalization requirement");
  auto finalization_resolved = resolve_control_binding_updates(
      update(finalization_binding, "control-memory", "max_iterations", 2), finalization_prepared);
  expect(finalization_resolved["control_updates"].items()[0]["value"].flag("require_finalization", false),
      "typed resolver dropped the host finalization capability gate");
  expect(invalid(update(loop, "control-memory", "max_iterations", 0), prepared)
      && invalid(update(loop, "control-memory", "max_iterations", 7), prepared),
      "typed loop binding accepted an out-of-range or non-triggering cap");

  auto existing_loop = loop_input;
  const Json old_loop{{"memory_id", "control-memory"}, {"actor_id", "outfit-stylist"},
      {"max_iterations", 5}, {"max_no_progress", 1}, {"max_replans", 1},
      {"replan_on_failure", false}, {"reason", "old"}, {"match_mode", "predicate"}};
  existing_loop["components"]["loop_rules"] = Json::Array{old_loop};
  existing_prepared = observed_operation_bindings(existing_loop, 65536);
  expect(binding(existing_prepared, "loop_rules")["budget"].num("max", 0) == 5,
      "loop binding ignored the existing parent cap");
  tightened = resolve_control_binding_updates(
      update(binding(existing_prepared, "loop_rules"), "control-memory", "max_iterations", 2), existing_prepared);
  expect(tightened["control_updates"].items().front().str("op") == "set"
      && tightened["control_updates"].items().front()["value"].num("max_no_progress", 0) == 1,
      "loop set did not preserve the existing rule");
  auto impossible_loop = existing_loop;
  impossible_loop["components"]["loop_rules"].items()[0]["max_iterations"] = 1;
  expect(!observed_operation_bindings(impossible_loop, 65536)["controls"].has("editable_bindings"),
      "a parent cap below the necessary-work floor exposed an unsatisfiable binding");
  existing_loop["components"]["loop_rules"].push(old_loop);
  existing_prepared = observed_operation_bindings(existing_loop, 65536);
  expect(invalid(update(binding(existing_prepared, "loop_rules"), "control-memory", "max_iterations", 2),
      existing_prepared), "ambiguous same-actor loop rules were silently overwritten");

  const Json concrete{{"reason", "legacy concrete"}, {"control_updates", Json::Array{Json{
      {"component", "action_rules"}, {"op", "remove"}, {"index", 0}}}}};
  expect(resolve_control_binding_updates(concrete, prepared).dump() == concrete.dump(),
      "legacy concrete and remove drafts lost compatibility");
  diagnostic = Json{{"reason", "private-marker-old-error"}};
  expect(resolve_control_binding_updates(concrete, prepared, &diagnostic).dump() == concrete.dump() && diagnostic.null(),
      "optional diagnostics changed legacy concrete drafts or retained stale state");
}

void reflection_component_views() {
  const Json original{{"selected_components", Json::Array{"action_rules", "loop_rules", "schedule_rules", "experience:a", "experience:b"}},
      {"components", Json{{"action_rules", Json::Array{}}, {"loop_rules", Json::Array{}}, {"schedule_rules", Json::Array{}},
          {"experience:a", "original a"}, {"experience:b", "original b"}}},
      {"experiences", Json::Array{Json{{"memory_id", "a"}, {"content", "original a"}}, Json{{"memory_id", "b"}, {"content", "original b"}}}},
      {"controls", Json{{"action_primitives", Json::Array{"operation_budget"}}, {"loop_primitives", Json::Array{"progress_stop"}},
          {"schedule_primitives", Json::Array{"resource_order"}}, {"base_action_rules", Json::Array{"action"}},
          {"base_loop_rules", Json::Array{"loop"}}, {"base_schedule_rules", Json::Array{"schedule"}}, {"limits", Json{{"max_rules", 8}}}}},
      {"constraints", Json::Array{"stay in scope"}}, {"feedback", Json::Array{"training evidence"}}};
  auto all = original; restrict_reflection_components(all, original["selected_components"]);
  expect(all.dump() == original.dump(), "selecting all components changed their request view");
  auto schedule = original; restrict_reflection_components(schedule, Json::Array{"schedule_rules"});
  expect(schedule["components"].fields().size() == 1 && schedule["controls"].has("schedule_primitives")
      && schedule["controls"].has("base_schedule_rules") && !schedule["controls"].has("action_primitives")
      && !schedule["controls"].has("loop_primitives") && !schedule["controls"].has("base_action_rules")
      && !schedule["controls"].has("base_loop_rules") && schedule["controls"]["limits"].dump() == original["controls"]["limits"].dump()
      && schedule["experiences"].dump() == original["experiences"].dump(), "schedule-only view exposed unauthorized components or lost source/limits");
  auto experience = original; restrict_reflection_components(experience, Json::Array{"experience:b"});
  expect(!experience.has("controls") && experience["components"].fields().size() == 1
      && experience["experiences"].items().size() == 1 && experience["experiences"].items()[0].str("content") == "original b"
      && experience["constraints"].dump() == original["constraints"].dump() && experience["feedback"].dump() == original["feedback"].dump(),
      "L1-only view lost selected original content or changed feedback/constraints");

  // 实际混合 packet 的正文只在 components 中；COMBEE 控制子请求必须搬移当前值。
  auto compact = original;
  compact["components"]["experience:a"] = "current a";
  compact["components"]["experience:b"] = Json{{"content", "current b"}, {"trigger_pattern", "current trigger"},
      {"observed_operations", Json::Array{"operation_b"}}};
  compact["components"]["experience:disabled"] = nullptr;
  compact["experiences"] = Json::Array{Json{{"memory_id", "a"}, {"original_content", "historical a"}},
      Json{{"memory_id", "b"}, {"original_content", "historical b"}},
      Json{{"memory_id", "disabled"}, {"content", "stale disabled body"}, {"original_content", "historical disabled body"}}};
  const auto compact_before = compact.dump();
  auto control_map = compact; restrict_reflection_components(control_map, Json::Array{"schedule_rules"});
  expect(control_map["components"].fields().size() == 1
      && control_map["experiences"].items()[0].str("content") == "current a"
      && control_map["experiences"].items()[1].str("content") == "current b"
      && control_map["experiences"].items()[1].str("original_content") == "historical b"
      && !control_map["experiences"].items()[2].has("content")
      && control_map["experiences"].items()[2].str("original_content") == "historical disabled body",
      "control map lost current source content or restored disabled/historical content");
  const auto mapped = control_map.dump();
  restrict_reflection_components(control_map, Json::Array{"schedule_rules"});
  expect(control_map.dump() == mapped, "repeated control projection changed recovered source content");
  auto experience_map = compact; restrict_reflection_components(experience_map, Json::Array{"experience:b"});
  expect(!experience_map.has("controls") && experience_map["components"].fields().size() == 1
      && experience_map["components"]["experience:b"].dump() == compact["components"]["experience:b"].dump()
      && experience_map["experiences"].items().size() == 1
      && experience_map["experiences"].items()[0].str("memory_id") == "b"
      && !experience_map["experiences"].items()[0].has("content")
      && experience_map["experiences"].items()[0].str("original_content") == "historical b"
      && compact.dump() == compact_before, "L1 map changed component/history or mutated its parent packet");
}

void repair_feedback_keeps_redispatch_opportunity() {
  Search search;
  const Json selected = Json::Array{"action_rules", "loop_rules"};
  auto row = [](const char* task, const char* verdict, const Json& actions) {
    return Json{{"task_id", task}, {"task_input", "Retain the original task contract."},
        {"trial", Json{{"verdict", verdict}, {"evidence", Json{{"runs", Json::Array{
            Json{{"run_id", task}, {"actions", actions}}}}}}}}};
  };
  const Json controlled = row("earlier-failed", "failed", Json::Array{
      Json{{"kind", "action.end"}, {"semantic", "action"}, {"actor_id", "other-worker"},
          {"execution_id", "other-branch"}, {"operation", "already-blocked"}, {"guard_only", true}, {"executed", false},
          {"action_control", Json{{"decision", "reject"}, {"prior_operation_rounds", 2}}}},
      Json{{"kind", "loop.decision"}, {"actor_id", "other-worker"}, {"execution_id", "other-branch"},
          {"loop_control", Json{{"pending_actions", false}, {"proposed_decision", "replan"}, {"observed_no_progress", 1}}}}});
  const Json guard{{"kind", "action.end"}, {"semantic", "action"}, {"actor_id", "dispatcher"},
      {"execution_id", "dispatch-loop"}, {"operation", "arbitrary-operation"}, {"guard_only", true}, {"executed", false},
      {"action_control", Json{{"decision", "allow"}, {"prior_operation_rounds", 1}, {"prior_failures", 0}}}};
  auto packet = [&](const Json& action) {
    return Json{{"feedback", Json::Array{controlled, row("later-opportunity", "passed", Json::Array{action})}}};
  };
  const auto input = packet(guard); const auto original = input.dump();
  const auto* chosen = search.repair_feedback_row(input, selected);
  expect(chosen == &input["feedback"].items()[1] && chosen->str("task_id") == "later-opportunity",
      "earlier controlled failure displaced the only allow redispatch opportunity");
  expect(input.dump() == original, "repair selection changed the task contract or evidence");
  expect(search.repair_feedback_row(input, Json::Array{"action_rules"}) == chosen,
      "dispatch opportunity required an unrelated loop component");
  expect(search.repair_feedback_row(input, Json::Array{"loop_rules"}) == &input["feedback"].items()[0],
      "action-only evidence changed loop-only repair selection");
  expect(search.repair_feedback_row(input, Json::Array{"schedule_rules"}) == &input["feedback"].items()[0],
      "action-only evidence changed schedule repair fallback");
  for(const auto* operation : {"fetch", "verification-worker", "render-document"}) {
    auto other_operation = guard; other_operation["operation"] = operation;
    const auto candidate = packet(other_operation);
    expect(search.repair_feedback_row(candidate, selected) == &candidate["feedback"].items()[1],
        "redispatch priority was coupled to a business operation name");
  }
  std::vector<Json> nonopportunities;
  auto rejected = guard; rejected["action_control"]["decision"] = "reject"; nonopportunities.push_back(rejected);
  auto absent = guard; absent["action_control"].fields().erase("decision"); nonopportunities.push_back(absent);
  auto initial = guard; initial["action_control"]["prior_operation_rounds"] = 0; nonopportunities.push_back(initial);
  auto plain = guard; plain["guard_only"] = false; plain["executed"] = true; nonopportunities.push_back(plain);
  auto inconsistent = guard; inconsistent["executed"] = true; nonopportunities.push_back(inconsistent);
  auto proposed = guard; proposed["semantic"] = "model";
  proposed["output"] = Json{{"tool_calls", Json::Array{Json{{"name", "arbitrary-operation"}}}}};
  nonopportunities.push_back(proposed);
  auto sibling = guard; sibling["guard_only"] = false; sibling["executed"] = true;
  sibling["status"] = "failed"; sibling["action_control"]["prior_failures"] = 3; nonopportunities.push_back(sibling);
  for(const auto& action : nonopportunities) {
    const auto candidate = packet(action);
    expect(search.repair_feedback_row(candidate, selected) == &candidate["feedback"].items()[0],
        "rejected, initial, nonguard, model-plan or sibling-failure evidence gained redispatch priority");
  }
  // 没有重复派发时，仍选择第一条相关证据；都不相关时保留第一条，空输入不造证据。
  const Json fallback{{"feedback", Json::Array{row("unrelated", "failed", Json::Array{}), controlled}}};
  expect(search.repair_feedback_row(fallback, Json::Array{"loop_rules"}) == &fallback["feedback"].items()[1],
      "original first-relevant-row fallback changed");
  expect(search.repair_feedback_row(fallback, Json::Array{"experience:memory"}) == &fallback["feedback"].items()[0],
      "original first-row fallback changed");
  const Json suffix = row("reviewed-suffix", "passed", Json::Array{Json{{"kind", "loop.decision"},
      {"actor_id", "worker"}, {"execution_id", "loop"}, {"iteration", "3"},
      {"loop_control", Json{{"target_scope", "assistant"}, {"pending_actions", false},
          {"proposed_decision", "continue"}, {"iteration", 3}, {"reviewed_waste_after", 2}}}}});
  Json suffix_input{{"controls", Json{{"aggressive_controls", true}, {"base_loop_rules", Json::Array{}}}},
      {"feedback", Json::Array{row("unrelated", "failed", Json::Array{}), suffix}}};
  expect(search.repair_feedback_row(suffix_input, Json::Array{"loop_rules"}) == &suffix_input["feedback"].items()[1],
      "repair row selection lost a reviewed iteration-cap opportunity");
  suffix_input.fields().erase("controls");
  expect(search.repair_feedback_row(suffix_input, Json::Array{"loop_rules"}) == &suffix_input["feedback"].items()[0],
      "missing parent controls authorized a reviewed iteration cap");

  const Json replan = row("replan", "passed", Json::Array{Json{{"kind", "loop.decision"},
      {"actor_id", "worker"}, {"execution_id", "loop"}, {"loop_control", Json{{"target_scope", "assistant"},
          {"pending_actions", false}, {"proposed_decision", "continue"}, {"repeated_failure_steps", 1}}}}});
  Json replan_input{{"controls", Json{{"aggressive_controls", false}, {"base_loop_rules", Json::Array{
      Json{{"target_scope", "assistant"}, {"actor_id", "worker"}, {"no_progress_decision", "replan"}, {"max_replans", 1}}}}}},
      {"feedback", Json::Array{row("unrelated", "failed", Json::Array{}), replan}}};
  expect(search.repair_feedback_row(replan_input, Json::Array{"loop_rules"}) == &replan_input["feedback"].items()[1],
      "repair row selection lost an authorized repeated-failure replan opportunity");
  replan_input.fields().erase("controls");
  expect(search.repair_feedback_row(replan_input, Json::Array{"loop_rules"}) == &replan_input["feedback"].items()[0],
      "missing base loop rules authorized a repeated-failure replan");
  expect(search.repair_feedback_row(Json{{"feedback", Json::Array{}}}, selected) == nullptr,
      "empty repair input manufactured evidence");
}

void missing_trial_control_evidence() {
  Report report; auto& trial = *report.add_trials();
  trial.set_verdict("error"); trial.set_execution_status("failed");
  trial.set_error_code("evaluation_trace_bytes_limit");
  const auto result = control_feedback_trial(trial, Json(Json::Array{"action_rules", "loop_rules"}), Json::Object{});
  expect(result.str("verdict") == "error" && result.str("error_code") == "evaluation_trace_bytes_limit",
      "capture failure was converted to a business result");
  expect(result["checks"].items().empty() && result["evidence"]["runs"].items().empty()
      && !result.flag("control_evidence_preserved", true), "missing capture manufactured control evidence");
  Search search;
  const Json packet{{"feedback", Json::Array{Json{{"trial", result}}}}};
  expect(!search.control_evidence(packet, "action_rules") && !search.control_evidence(packet, "loop_rules"),
      "capture failure authorized runtime controls");
}

void reflection_transport_diagnostics() {
  agentflow::trajectory::RawModelResponse response;
  response.transport_ok = true; response.attempts = 2;
  response.adapter = "private-adapter-marker"; response.model_id = "private-model-marker";
  response.body = "private-response-body-marker";
  for(const auto* code : {"", "missing_record", "request_too_large", "response_too_large",
      "client_initialization_failed", "header_allocation_failed", "output_token_limit_reached",
      "invalid_api_response", "transport_exhausted", "http_100", "http_429", "http_503", "http_599",
      "transport_1", "transport_28", "transport_42", "transport_999"}) {
    response.error_code = code;
    const auto value = reflection_call_diagnostic(response, 123);
    expect(value.fields().size() == 5 && value.flag("transport_ok", false)
        && value.str("error_code") == code && value.num("attempts", 0) == 2
        && value.num("duration_ms", 0) == 123 && value.num("decoded_body_bytes", 0) == response.body.size(),
        "reflection diagnostic lost fixed code or numeric measurements");
    expect(value.dump().find("private-") == std::string::npos,
        "reflection diagnostic retained body, adapter or model text");
  }
  response.transport_ok = false; response.attempts = 0;
  for(const auto* code : {"http_0", "http_99", "http_600", "http_1000", "http_0429", "http_+429", "http_429suffix",
      "transport_0", "transport_028", "transport_1000", "transport_-1", "transport_1.0", "transport_1e2",
      "transport_999999999999999999999999999999", "transport_28\nprivate-secret", "private-secret",
      "https://private-user:private-secret@example.invalid", "Authorization: Bearer private-secret"}) {
    response.error_code = code;
    const auto value = reflection_call_diagnostic(response, 0);
    expect(value.fields().size() == 5 && !value.flag("transport_ok", true)
        && value.str("error_code") == "unknown_adapter_error"
        && value.num("attempts", 1) == 0 && value.num("duration_ms", 1) == 0
        && value.dump().find("private-") == std::string::npos,
        "reflection diagnostic leaked an unknown or out-of-range adapter error");
  }
  response.error_code = std::string("http_429\0private-secret", 23);
  expect(reflection_call_diagnostic(response, 0).str("error_code") == "unknown_adapter_error",
      "reflection diagnostic accepted a numeric code with an embedded null");
  expect(response.body == "private-response-body-marker" && response.adapter == "private-adapter-marker"
      && response.model_id == "private-model-marker", "reflection diagnostic mutated its response");
}

void cross_job_evaluation_cache() {
  char directory[] = "/tmp/trajectory-gepa-cross-job-cache-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create shared evaluation cache fixture");
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{directory};
  Search before; before.root = fs::path(directory) / "old-job/search";
  before.context = "search-v12"; before.candidate_evaluation_context = "runtime-and-evaluator-v1";
  before.dataset.set_dataset_version("dataset-v1");
  before.config = Json{{"cache_evaluations", true}, {"objective", Json{{"invalid_loop_weight", 0}, {"token_weight", 0}}}};
  const Json settings{{"splits", Json::Array{"val"}}, {"task_ids", Json::Array{"task-a"}}, {"repetitions", 1},
      {"variants", Json::Array{Json{{"id", "candidate"}, {"version", "policy-a"}, {"command", Json::Array{"private-marker-host"}}}}}};
  Report report; report.set_schema_version(1); report.set_report_kind("task_evaluation.v1");
  report.set_dataset_version("dataset-v1"); report.set_complete(true); report.set_run_id("observed-run");
  auto* trial = report.add_trials(); trial->set_variant_id("candidate"); trial->set_task_id("task-a"); trial->set_split("val");
  trial->set_repetition(1); trial->set_trial_id("observed-trial"); trial->set_execution_status("completed"); trial->set_verdict("passed");
  *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"loaded_version", "policy-a"},
      {"loop_review", Json{{"status", "complete"}, {"count", 1}}}});
  *trial->mutable_metrics() = proto<google::protobuf::Struct>(Json{{"tokens", 10}});
  const auto path = before.root / "evaluations/exact/attempt-1/report.json";
  const auto bytes = json(report).dump();
  const auto manifest = Json{{"complete", true}, {"run_id", "observed-run"}, {"files", Json{{"report.json", hash(bytes)}}}}.dump();
  write(path, bytes); write(path.parent_path() / "manifest.json", manifest);
  before.remember_shared_evaluation(settings, false, path, report);
  auto after = before; after.root = fs::path(directory) / "new-job/search"; after.context = "search-v13";
  expect(after.shared_evaluation_report(settings, false) == path, "search-only changes invalidated identical execution evidence");
  after.config["objective"]["invalid_loop_weight"] = 0.5;
  after.gate = Json{{"min_pass_rate", 1}};
  expect(after.shared_evaluation_report(settings, false) == path, "new ranking or gate discarded reusable raw measurements");
  auto changed = settings; changed["variants"].items()[0]["version"] = "policy-b";
  expect(after.shared_evaluation_report(changed, false).empty(), "a different policy reused previous measurements");
  changed = settings; changed["parallelism"] = 2;
  expect(after.shared_evaluation_report(changed, false).empty(), "different evaluation conditions reused previous measurements");
  changed = settings; changed["repetitions"] = 2;
  expect(after.shared_evaluation_report(changed, false).empty(), "cache silently filled missing repetitions");
  changed = settings; changed["task_ids"] = Json::Array{"task-b"};
  expect(after.shared_evaluation_report(changed, false).empty(), "cache reused a different task set");
  after.candidate_evaluation_context = "runtime-and-evaluator-v2";
  expect(after.shared_evaluation_report(settings, false).empty(), "runtime or judge changes reused old execution evidence");
  after.candidate_evaluation_context = before.candidate_evaluation_context;
  after.dataset.set_dataset_version("dataset-v2");
  expect(after.shared_evaluation_report(settings, false).empty(), "changed dataset reused old grades");
  after.dataset.set_dataset_version("dataset-v1");
  changed = settings; changed["splits"] = Json::Array{"holdout"};
  expect(after.shared_evaluation_report(changed, false).empty() && after.shared_evaluation_report(settings, true).empty(),
      "holdout or final publication evaluation reused the shared cache");
  changed = settings; changed["splits"] = Json::Array{"train"};
  auto training = report; training.mutable_trials(0)->set_split("train");
  const auto training_path = before.root / "evaluations/train/attempt-1/report.json";
  const auto training_bytes = json(training).dump();
  write(training_path, training_bytes); write(training_path.parent_path() / "manifest.json",
      Json{{"complete", true}, {"run_id", training.run_id()}, {"files", Json{{"report.json", hash(training_bytes)}}}}.dump());
  before.remember_shared_evaluation(changed, false, training_path, training);
  expect(after.shared_evaluation_report(changed, false) == training_path, "training report did not reuse the exact matching cache");
  auto failed = report; failed.mutable_trials(0)->set_error_code("evaluation_judge_failed");
  expect(!after.shared_report_complete(failed, settings), "failed model judging became a completed reusable result");
  failed = report; failed.set_complete(false);
  expect(!after.shared_report_complete(failed, settings), "partial evaluation became a completed reusable result");
  const auto index = fs::path(directory) / "candidate-cache" / (hash(before.shared_evaluation_identity(settings, false).dump()) + ".json");
  expect(read(index).find("private-marker") == std::string::npos, "cache index exposed execution configuration values");
  write(path, bytes + " ");
  expect(after.shared_evaluation_report(settings, false).empty(), "changed report bytes passed the cache hash check");
  write(path, bytes); write(path.parent_path() / "manifest.json", "{}");
  expect(after.shared_evaluation_report(settings, false).empty(), "changed manifest passed the cache hash check");
  write(path.parent_path() / "manifest.json", manifest); fs::remove(path);
  expect(after.shared_evaluation_report(settings, false).empty(), "deleted source report caused an error instead of a cache miss");
}

void fixed_holdout_baseline_cache() {
  char directory[] = "/tmp/trajectory-gepa-holdout-cache-XXXXXX";
  expect(::mkdtemp(directory) != nullptr, "cannot create holdout cache fixture");
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{directory};
  const Json legacy{{"evaluation_context", "execution-v1"}, {"baseline_version", "l0"}, {"dataset_version", "data-v1"}};
  const auto legacy_key = hash(legacy.dump());
  const Json identity{{"schema_version", 1}, {"evaluation_context", "execution-v1"}, {"split", "holdout"},
      {"baseline_version", "l0"}, {"dataset_version", "data-v1"}, {"task_versions", Json{{"h-a", "task-v1"}}},
      {"repetitions", 1}, {"loop_review", true}};
  HoldoutBaselineCache cache{directory, identity, legacy_key, 1024,
      [](const fs::path& path, std::size_t limit) { return read(path, limit); },
      [](const fs::path& path, const std::string& bytes) { write(path, bytes); },
      [](const std::string& bytes) { return hash(bytes); }};
  write(fs::path(directory) / "baseline-cache" / (legacy_key + ".json"), Json{{"identity", legacy}}.dump());
  auto report = [&](const char* variant, const char* version, const char* trial_id, bool passed) {
    Report value; value.set_schema_version(1); value.set_report_kind("task_evaluation.v1");
    value.set_evaluator_version("task-evaluator-v1"); value.set_dataset_version("data-v1");
    value.set_run_id(trial_id); value.set_complete(true); value.set_config_fingerprint("measured-settings");
    auto* trial = value.add_trials(); trial->set_variant_id(variant); trial->set_task_id("h-a"); trial->set_split("holdout");
    trial->set_task_version("task-v1"); trial->set_repetition(1); trial->set_trial_id(trial_id);
    trial->set_execution_status("completed"); trial->set_verdict(passed ? "passed" : "failed"); trial->set_duration_ms(100);
    *trial->mutable_evidence() = proto<google::protobuf::Struct>(Json{{"loaded_version", version},
        {"loop_review", Json{{"status", "complete"}, {"count", 1}}}, {"artifacts", Json{{"proof", Json{
            {"stored_file", "proof.txt"}, {"size_bytes", 5}, {"sha256", hash("proof")}}}}}});
    *value.mutable_configuration() = proto<google::protobuf::Struct>(Json{{"splits", Json::Array{"holdout"}},
        {"repetitions", 1}, {"variants", Json::Array{Json{{"id", variant}, {"version", version}}}}});
    *value.mutable_summary() = proto<google::protobuf::Struct>(Json{{"model_calls", 1}, {"by_variant", Json{{variant,
        Json{{"trials", 1}, {"scored", 1}, {"passed", passed ? 1 : 0}, {"failed", passed ? 0 : 1},
            {"pass_rate", passed ? 1.0 : 0.0}, {"mean_agent_duration_ms", 100}}}}}});
    return value;
  };
  auto save = [&](const fs::path& path, const Report& value) {
    const auto bytes = json(value).dump(), pb = value.SerializeAsString();
    write(path, bytes); write(path.parent_path() / "report.pb", pb);
    write(path.parent_path() / "manifest.json", Json{{"complete", value.complete()}, {"run_id", value.run_id()},
        {"files", Json{{"report.json", hash(bytes)}, {"report.pb", hash(pb)}}}}.dump());
    for(const auto& trial : value.trials()) write(path.parent_path() / "artifacts" / trial.trial_id() / "proof.txt", "proof");
  };
  const auto original = report("baseline", "l0", "old-l0-trial", false);
  const auto path = fs::path(directory) / "old/search/evaluations/final/attempt-1/report.json"; save(path, original);
  const auto tree = Json{{"started_ms", 1}, {"baseline_version", "l0"}, {"dataset_version", "data-v1"},
      {"baseline_cache", Json{{"key", legacy_key}}}, {"evaluations", Json{{"final", Json{{"split", "holdout"},
          {"status", "complete"}, {"baseline_version", "l0"}, {"report", "evaluations/final/attempt-1/report.json"},
          {"report_hash", hash(json(original).dump())}}}}}};
  write(fs::path(directory) / "old/search/tree.json", tree.dump());
  const auto pinned = cache.lookup();
  expect(pinned.str("status") == "hit", "valid historical holdout L0 was not imported");
  expect(cache.load(pinned["source"]).trials(0).trial_id() == "old-l0-trial", "L0 execution ID changed during reuse");
  // 同一 dataset/context 下任务集合、版本、split、重复数不同均不能借用旧基线。
  for(int mode = 0; mode < 5; ++mode) {
    auto changed = cache;
    if(mode == 0) changed.identity["split"] = "val";
    if(mode == 1) changed.identity["task_versions"] = Json{{"v-a", "task-v1"}};
    if(mode == 2) changed.identity["task_versions"]["h-a"] = "task-v2";
    if(mode == 3) changed.identity["repetitions"] = 2;
    if(mode == 4) changed.identity["evaluation_context"] = "execution-v2";
    expect(changed.lookup(pinned).str("status") == "invalid", "incompatible holdout baseline was reused");
  }
  auto invalid = original; invalid.mutable_trials(0)->set_split("val");
  *invalid.mutable_configuration() = proto<google::protobuf::Struct>(Json{{"splits", Json::Array{"val"}},
      {"repetitions", 1}, {"variants", Json::Array{Json{{"id", "baseline"}, {"version", "l0"}}}}});
  save(path, invalid); bool rejected = false;
  try { cache.load(cache.source(path, hash(json(invalid).dump()))); } catch(const Error&) { rejected = true; }
  expect(rejected, "a val report became the holdout reference through its baseline identity"); save(path, original);
  const auto fresh = report("candidate", "policy-new", "new-candidate-trial", true);
  const auto fresh_path = fs::path(directory) / "new/candidate/report.json"; save(fresh_path, fresh);
  const auto output = fs::path(directory) / "new/combined";
  const auto combined = cache.combine(pinned["source"], fresh, fresh_path, output, "pair");
  expect(combined.complete() && combined.trials_size() == 2 && combined.report_kind() == "gepa_cached_pair.v1"
      && json(combined.summary()).num("reused_baseline_trials", 0) == 1
      && json(combined.summary()).str("comparison_timing") == "historical_baseline"
      && json(combined.comparisons())["candidate"].num("paired_trials", 0) == 1
      && json(combined.comparisons())["candidate"].num("wins", 0) == 1
      && json(combined.summary())["by_variant"]["baseline"].num("passed", 1) == 0,
      "paired report altered grades, lost task pairing, or concealed historical timing");
  expect(read(output / "artifacts/old-l0-trial/proof.txt") == "proof"
      && read(output / "artifacts/new-candidate-trial/proof.txt") == "proof", "combined report is not independently readable");
  const auto limited = fs::path(directory) / "new/size-limited"; rejected = false;
  try { cache.combine(pinned["source"], fresh, fresh_path, limited, "small-pair", 1); }
  catch(const Error& error) { rejected = std::string(error.what()) == "evaluation_report_size_limit"; }
  expect(rejected && !fs::exists(limited), "oversized pair wrote partial reports or copied artifacts");
  const auto artifact = path.parent_path() / "artifacts/old-l0-trial/proof.txt";
  write(artifact, "other");
  expect(cache.lookup(pinned).str("status") == "invalid", "changed locked source silently switched or reran L0");
  write(artifact, "proof"); write(path.parent_path() / "report.pb", "changed");
  expect(cache.lookup(pinned).str("status") == "invalid", "manifest-listed protobuf corruption was ignored");
  save(path, original); write(path, json(original).dump() + " ");
  expect(cache.lookup(pinned).str("status") == "invalid", "changed report passed its pinned hash");
}

int main() {
  try {
    public_private_alias_revisions();
    selected_experience_receipt_attribution();
    experience_packet_attribution_isolation();
    compact_experience_feedback_budget();
    experience_ablation_checkpoint_deduplication();
    observed_training_screening();
    empty_experience_receipts(); unknown_loop_counter(); incremental_control_coverage();
    cancelled_search_clock(); context_version_is_not_behavior(); asynchronous_target_replay();
    attributed_candidate_selection(); terminal_failure_identity(); operation_budget_identity(); schedule_resource_projection();
    recovery_component_pairing(); prompt_resource_workflows(); loop_feedback_runtime_budget(); missing_trial_control_evidence();
    reviewed_iteration_cap_evidence(); reviewed_finalization_cap_evidence(); required_iteration_floor_tests(); action_feedback_keeps_iteration_assessment();
    incremental_control_drafts(); pure_control_removal_preflight(); reflection_component_views();
    observed_operation_binding_views();
    reviewed_operation_budget_bindings();
    typed_control_binding_updates();
    repair_feedback_keeps_redispatch_opportunity();
    reflection_transport_diagnostics();
    cross_job_evaluation_cache();
    fixed_holdout_baseline_cache();
    std::cout << "GEPA evidence regressions passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
