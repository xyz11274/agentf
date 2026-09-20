#include "trajectory_evolution/evolution.h"
#include "agentflow/trajectory/experience.h"

#include <algorithm>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace te = trajectory::evolution;
namespace v1 = te::v1;
namespace ep = agentflow::trajectory::evaluation;

void expect(bool value, const char* message) {
  if(!value) throw std::runtime_error(message);
}
template<class F> void rejects(F fn) {
  try { fn(); } catch(const agentflow::trajectory::EvaluationError&) { return; }
  throw std::runtime_error("expected rejection");
}

v1::Identity identity(const std::string& user = "user-a") {
  v1::Identity id;
  id.set_agent_id("test-agent"); id.set_user_id(user);
  id.set_conversation_id("conversation-a"); id.set_run_id("run-a");
  return id;
}
v1::ShadowOptions options() {
  v1::ShadowOptions opt;
  opt.add_target_scopes("coordinator");
  opt.set_max_policy_entries(64); opt.set_max_advice_items(3);
  opt.set_max_advice_bytes(4096); opt.set_max_query_bytes(16384);
  opt.set_max_memory_bytes(1048576); opt.set_max_snapshot_bytes(262144);
  opt.set_max_match_terms(128); opt.set_min_matched_terms(2);
  opt.add_stop_terms("任务");
  return opt;
}
ep::EvolutionMemoryStore memory() {
  ep::EvolutionMemoryStore store;
  store.set_schema_version(1); store.set_store_kind("trajectory_evolution_memory");
  store.set_store_version("evolution-memory-store-v1"); store.set_revision(7);
  return store;
}
ep::EvolutionMemoryEntry* add(ep::EvolutionMemoryStore& store,
                              const std::string& id, const std::string& user = "user-a") {
  auto* entry = store.add_entries();
  entry->set_memory_id(id); entry->set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
  auto* c = entry->mutable_candidate();
  c->set_schema_version(1); c->set_candidate_id("candidate-" + id);
  c->set_generator_version(agentflow::trajectory::kScopedExperienceGeneratorVersion);
  c->set_target_type(ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION);
  c->set_target_scope("coordinator"); c->set_status(ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED);
  c->set_trigger_pattern("生成视频时缺少图片素材");
  c->set_observed_problem("缺少必需图片仍反复调用工具");
  c->set_proposed_change("缺少图片素材时，先请求补充；独立网络检索任务继续。");
  c->set_support_user_count(1);
  auto* evidence = c->add_support_evidence();
  evidence->set_evidence_id("evidence-" + id);
  evidence->mutable_source()->set_user_id(user);
  evidence->mutable_source()->set_dialogue_id("source-" + id);
  return entry;
}
v1::AdviceQuery query(const std::string& text = "生成视频但是没有图片素材") {
  v1::AdviceQuery q; *q.mutable_identity() = identity();
  q.set_query_id("query-1"); q.set_decision_id("decision-1");
  q.set_target_scope("coordinator"); q.set_task(text);
  return q;
}

void matching_and_privacy() {
  auto store = memory(); add(store, "mine"); add(store, "other", "user-b");
  auto* mixed = add(store, "mixed")->mutable_candidate();
  mixed->set_support_user_count(2);
  mixed->add_support_evidence()->mutable_source()->set_user_id("user-b");
  auto* unknown = add(store, "unknown")->mutable_candidate();
  unknown->clear_support_evidence();
  add(store, "wrong-scope")->mutable_candidate()->set_target_scope("skill:video-gen");
  add(store, "wrong-type")->mutable_candidate()->set_target_type(
      ep::CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION);
  auto snapshot = te::compile_shadow_policy(store, identity(), options());
  expect(snapshot.experiences_size() == 2, "compiler crossed ownership/scope/type");
  const auto control = std::find_if(snapshot.experiences().begin(), snapshot.experiences().end(),
      [](const auto& item) { return item.memory_id() == "wrong-type"; });
  expect(control != snapshot.experiences().end() && !control->control_target_type().empty()
      && control->match_terms().empty(), "control source was not isolated from L1 matching");
  expect(te::to_json(snapshot).find("user-b") == std::string::npos, "foreign owner leaked");
  te::ShadowRuntime runtime(snapshot);
  auto result = runtime.query_advice(query());
  expect(result.status() == "matched" && result.advice_size() == 1, "Chinese matching failed");
  expect(result.application_status() == "not_applied" && result.mode() == "shadow",
         "Shadow pretended to apply");
  expect(result.advice(0).experience().memory_id() == "mine", "wrong memory selected");
  expect(!result.advice(0).matched_terms().empty(), "missing match explanation");
  auto repeated = query();
  repeated.set_query_id("query-2"); repeated.set_decision_id("decision-2");
  expect(runtime.query_advice(repeated).input_fingerprint() == result.input_fingerprint(),
         "query IDs changed identical input fingerprint");
  expect(runtime.query_advice(query("明天巴黎气温如何")).status() == "no_match",
         "unrelated task matched");
  expect(runtime.query_advice(query("请生成一张图片")).status() == "no_match",
         "a generic Chinese bigram caused an unrelated match");

  for(int field = 0; field < 5; ++field) {
    auto bad = query();
    if(field == 0) bad.mutable_identity()->set_user_id("user-b");
    if(field == 1) bad.mutable_identity()->set_agent_id("another-agent");
    if(field == 2) bad.mutable_identity()->set_conversation_id("another-conversation");
    if(field == 3) bad.mutable_identity()->set_run_id("another-run");
    if(field == 4) bad.set_target_scope("skill:video-gen");
    const auto failed = runtime.query_advice(bad);
    expect(failed.status() == "error" && failed.advice_size() == 0
               && failed.snapshot_id().empty(), "scope mismatch disclosed advice");
  }
  auto big = query(); big.set_task(std::string(20000, 'x'));
  expect(runtime.query_advice(big).status() == "error", "query limit ignored");
}

void freeze_conflict_and_budget() {
  auto store = memory(); add(store, "a"); add(store, "b"); add(store, "c");
  auto* edge = store.mutable_entries(1)->add_relations();
  edge->set_target_memory_id("c"); edge->set_relation(ep::MEMORY_MATCH_RELATION_CONTRADICTS);
  auto first = te::compile_shadow_policy(store, identity(), options());
  expect(first.experiences_size() == 1 && first.experiences(0).memory_id() == "a",
         "one-sided conflict did not exclude both ends");
  auto reversed = store;
  std::reverse(reversed.mutable_entries()->begin(), reversed.mutable_entries()->end());
  expect(te::compile_shadow_policy(reversed, identity(), options()).snapshot_id()
             == first.snapshot_id(), "memory ordering changed snapshot");
  te::ShadowRuntime old_runtime(first);
  store.mutable_entries(0)->mutable_candidate()->set_proposed_change("新经验：图片素材缺失时请求上传。");
  store.set_revision(8);
  auto next = te::compile_shadow_policy(store, identity(), options());
  expect(first.snapshot_id() != next.snapshot_id(), "revision not bound to content");
  expect(old_runtime.query_advice(query()).snapshot_id() == first.snapshot_id(),
         "existing run adopted latest memory");
  auto tampered = first;
  tampered.mutable_experiences(0)->set_content("silently changed");
  rejects([&] { te::ShadowRuntime invalid(tampered); });
  v1::ShadowPolicySnapshot restored;
  expect(restored.ParseFromString(first.SerializeAsString()), "snapshot PB parse");
  expect(te::to_json(te::ShadowRuntime(restored).query_advice(query()))
             == te::to_json(old_runtime.query_advice(query())), "PB replay drift");

  auto limits = options(); limits.set_max_advice_bytes(512);
  auto limited = te::ShadowRuntime(te::compile_shadow_policy(store, identity(), limits));
  expect(limited.query_advice(query()).advice_size() == 0, "wire advice budget ignored");
  limits.set_max_advice_bytes(4096); limits.set_max_advice_items(1);
  add(store, "d");
  expect(te::ShadowRuntime(te::compile_shadow_policy(store, identity(), limits))
             .query_advice(query()).advice_size() == 1, "top-K ignored");
  limits.set_max_query_bytes(0);
  rejects([&] { te::compile_shadow_policy(store, identity(), limits); });
  *store.add_entries() = store.entries(0);
  rejects([&] { te::compile_shadow_policy(store, identity(), options()); });
}

void secrets_english_and_concurrency() {
  auto store = memory(); add(store, "valid");
  const std::string secret = "fake-vendor-" + std::string(28, 'q');
#if defined(__unix__) || defined(__APPLE__)
  setenv("TEST_SHADOW_VENDOR_KEY", secret.c_str(), 1);
#endif
  add(store, "unsafe")->mutable_candidate()->set_proposed_change("凭据：" + secret);
  auto* english = add(store, "english")->mutable_candidate();
  english->set_trigger_pattern("network timeout");
  english->set_observed_problem("network request timed out");
  english->set_proposed_change("Retry transient network timeout with a bounded budget.");
  const auto snapshot = te::compile_shadow_policy(store, identity(), options());
  expect(snapshot.experiences_size() == 2, "unsafe entry blocked independent memories");
  expect(te::to_json(snapshot).find(secret) == std::string::npos, "snapshot leaked credential");
  const te::ShadowRuntime runtime(snapshot);
  auto response = runtime.query_advice(query("NETWORK TIMEOUT " + secret));
  expect(response.status() == "matched", "English matching failed");
  expect(runtime.query_advice(query("network timeouts")).status() == "matched",
         "English word normalization failed");
  expect(te::to_json(response).find(secret) == std::string::npos, "query echoed secret");
  std::vector<std::future<v1::AdviceResult>> pending;
  for(int i = 0; i < 24; ++i) {
    pending.emplace_back(std::async(std::launch::async, [&runtime, i] {
      auto q = query(); q.set_query_id("parallel-" + std::to_string(i));
      if(i % 3 == 0) q.mutable_identity()->set_user_id("user-b");
      return runtime.query_advice(q);
    }));
  }
  for(int i = 0; i < 24; ++i) {
    auto result = pending[i].get();
    expect(result.query_id() == "parallel-" + std::to_string(i), "parallel attribution changed");
    expect(result.status() == (i % 3 == 0 ? "error" : "matched"), "branch failure leaked");
  }
#if defined(__unix__) || defined(__APPLE__)
  unsetenv("TEST_SHADOW_VENDOR_KEY");
#endif
}

void experience_contract_and_condition_matching() {
  auto store = memory();
  auto* valid = add(store, "valid")->mutable_candidate();
  valid->set_observed_problem("irrelevant quantum quasar incident");
  valid->set_proposed_change("先补充图片素材；保留 quantum quasar 历史引用。");
  add(store, "legacy")->mutable_candidate()->set_generator_version("evolution-candidate-generator-v2");
  add(store, "placeholder")->mutable_candidate()->set_trigger_pattern("validated_dialogue_reflection");
  add(store, "empty")->mutable_candidate()->set_trigger_pattern("   \n");
  add(store, "diagnostic")->mutable_candidate()->set_trigger_pattern("category=exact_repeat");
  const auto snapshot = te::compile_shadow_policy(store, identity(), options());
  expect(snapshot.experiences_size() == 1, "legacy/malformed experience became injectable");
  expect(snapshot.excluded_counts().at("experience_contract") == 4, "contract exclusions not recorded");
  const te::ShadowRuntime runtime(snapshot);
  expect(runtime.query_advice(query()).status() == "matched", "condition did not match");
  expect(runtime.query_advice(query("quantum quasar")).status() == "no_match",
         "historical problem/advice text was used as applicability evidence");
}

void observation_is_not_injection() {
  auto store = memory(); add(store, "m");
  auto result = te::ShadowRuntime(te::compile_shadow_policy(store, identity(), options()))
                    .query_advice(query());
  v1::TraceRun trace;
  trace.set_schema_version(1); trace.set_trace_kind("trajectory.v1");
  *trace.mutable_identity() = identity();
  auto* start = trace.add_events();
  start->set_event_id("begin"); start->set_kind("run.start");
  start->set_producer_id("host"); start->set_sequence(1); start->set_actor_id("assistant");
  start->mutable_input()->set_string_value("生成视频但没有图片");
  *trace.add_events() = te::shadow_trace_event(result, "shadow", 1);
  auto* end = trace.add_events();
  end->set_event_id("end"); end->set_kind("run.end"); end->set_producer_id("host");
  end->set_sequence(2); end->set_actor_id("assistant");
  end->mutable_output()->set_string_value("请提供素材");
  end->mutable_outcome()->set_execution_status("completed");
  end->mutable_outcome()->set_business_status("partial");
  auto valid = te::validate_trace(trace);
  expect(valid.report.trace_valid(), "shadow observation broke neutral trace");
  auto manifest = te::parse_manifest_json(
      R"({"schema_version":1,"requested_level":"L1","declared_capabilities":["run_observation","experience_injection"],"fallback":"downgrade"})");
  auto report = te::inspect_capabilities(valid, manifest);
  expect(report.data_level() == v1::L0 && report.verified_level() == v1::L0,
         "Shadow was counted as injection");
}

void retrieval_evidence_and_independent_matches() {
  auto check = [](const std::string& pattern, const std::string& text, bool matched,
                  const char* message) {
    auto store = memory();
    add(store, "m")->mutable_candidate()->set_trigger_pattern(pattern);
    te::ShadowRuntime runtime(te::compile_shadow_policy(store, identity(), options()));
    auto q = query("库存核算"); q.set_state(text);
    const auto result = runtime.query_advice(q);
    expect(result.status() == (matched ? "matched" : "no_match"), message);
    return result;
  };
  const std::string image = "当 generate_image 返回 status=succeeded 且 files 非空时";
  check(image, R"([{"role":"tool","content":"{\"status\":\"success\",\"weather\":\"晴\"}"}])", false,
        "JSON field names were treated as applicability evidence");
  check(image, R"([{"role":"tool","content":"{\"status\":\"succeeded\"}"}])", false,
        "a status value and its word variants passed the match threshold");
  check("当结构化生成工具（如 generate_image）返回 status=succeeded 且 files 非空，或返回 tool_error 但执行报告显示生成已成功且 files 非空时",
        "tool_error 执行报告：CSV 已成功保存，用户未要求图片，未调用生成工具", false,
        "generic report wording bypassed the named operation in the condition");
  check("当 generate_video 工具返回 HTTP 404 UnsupportedModel，模型不支持 Agent Plan 特性时",
        "agent 正在核算预算，bash 返回 HTTP 200，但目录不存在", false,
        "generic protocol terms triggered advice for another operation");
  check(image, R"([{"role":"assistant","tool_calls":[{"id":"call-a","type":"function","function":{"name":"generate_image","arguments":"{}"}}]},{"role":"tool","content":"{\"status\":\"succeeded\",\"files\":[\"result.png\"]}"}])", true,
        "real operation and result evidence was lost");
  check(image, R"({"role":"function","name":"generate_image","content":"{\"status\":\"succeeded\"}"})", true,
        "a named function result lost its actual operation");
  check(image, R"({"role":"user","name":"generate_image","content":"succeeded"})", false,
        "a participant name was treated as an executed operation");
  check("generate_image failed", "generate_video failed", false,
        "splitting operation identifiers matched an unrelated operation");
  const std::string completion = "当 coordinator 已获得所有委派 skill 的成功结果时";
  check(completion, R"([{"role":"user","content":"核算库存"},{"role":"user","content":"<system-reminder>直接给出 skill_result 文本结果交还 coordinator</system-reminder>"}])", false,
        "a user-role system reminder triggered completion advice");
  check(completion, R"({"messages":[{"role":"system","content":"coordinator skill"},{"role":"developer","content":"coordinator skill"},{"role":"user","content":"核算库存"}],"tools":[{"description":"coordinator skill"}]})", false,
        "instructions or tool schemas entered retrieval");
  check(completion, "<trajectory-experience policy=\"old\">coordinator skill</trajectory-experience>", false,
        "an old injection triggered itself");
  check(completion, "<system-reminder>coordinator skill", false,
        "an incomplete reminder became retrieval evidence");
  check(completion, "coordinator 已获得所有 skill 的成功结果", true,
        "actual completion text was filtered with the reminders");
  check("当 coordinator 已获得所有委派 skill 的成功结果且结果覆盖用户需求时",
        "报告需要覆盖用户需求中的天气信息", false,
        "a partial Han phrase triggered unrelated completion advice");
  const std::string write = "当需要写入包含大量引号或特殊字符的文件，或使用 bash 重定向写入失败且错误为 'No such file or directory' 且目标目录不存在时";
  check(write, "没有上传照片，仅说明需要补充首帧，不要多次尝试不存在的路径或其他用户文件", false,
        "overlapping Han substrings triggered unrelated file-writing advice");
  check(write, "请确认用户上传的衣物照片素材是否存在。upload 目录为空或不存在，没有可用的上传图片。"
        "如果确认没有素材，请明确返回素材缺失，不要生成新图片，也不要引用服务器上其他用户的文件。", false,
        "function-word fragments triggered file-writing advice for a missing image");
  check(write, "上传素材目录不存在，不要引用其他用户的文件。", false,
        "a directory observation without a write failure triggered write recovery");
  check("当结构化生成工具（如 generate_image）返回 status=succeeded 且 files 非空，或返回 tool_error 但执行报告显示生成已成功且 files 非空时",
        "执行报告：预算已成功保存，全部任务完成。\\n\\n下一步建议\\n\\n"
        "若用户需要，可进一步生成效果图（generate_image），但需用户明确意图。", false,
        "a hypothetical tool mention borrowed success evidence from another paragraph");
  check(image, std::string(4000, ' ') + "generate_image generate_image succeeded", true,
        "repeated anchors must not change valid applicability");
  check(write, "bash 写入失败: No such file or directory", true,
        "real file-writing failure no longer matched");
  check(write, "bash: file report.csv exists; read completed", false,
        "words inside an error literal independently triggered failure advice");
  check(write, "bash: no such file or directory", true,
        "case normalization lost an actual error literal");
  check(image, "{ \n \"role\":\"tool\", \"content\":\"generate_image succeeded", false,
        "truncated JSON with whitespace fell back to its raw keys and values");
  check("生成视频时缺少图片素材", "图片素材", false,
        "one Han phrase was counted as multiple independent matches");
  check("生成视频时缺少图片素材", "生成视频但是没有图片素材", true,
        "related Chinese evidence no longer matched");
  check("视频生成缺少素材", "视频生成缺少图片素材", true,
        "a short fragment could not extend a specific Chinese phrase");
  check("network timeout", "timeouts", false,
        "one English word and its stem passed the match threshold");
  const auto normalized = check("network timeout", "network timeouts", true,
        "English plural normalization was lost");
  expect(normalized.advice(0).matched_terms_size() == 2, "English variants were counted repeatedly");
  check("network timeout", R"({"role":"user","content":"network timeouts"})", true,
        "structured user content was lost");
  check("network timeout", "<system-reminder>库存核算</system-reminder>network timeouts", true,
        "real text outside an instruction block was lost");
}

void current_task_history_isolation() {
  auto store = memory();
  add(store, "completion")->mutable_candidate()->set_trigger_pattern(
      "当 coordinator 已获得所有委派 skill 的成功结果时");
  const te::ShadowRuntime runtime(te::compile_shadow_policy(store, identity(), options()));
  const auto check = [&](const std::string& task, const std::string& state, bool matched, const char* message) {
    auto q = query(task); q.set_state(state); const auto before = q.SerializeAsString();
    expect(runtime.query_advice(q).status() == (matched ? "matched" : "no_match"), message);
    expect(q.SerializeAsString() == before, "current-task projection mutated the caller's query");
  };
  const std::string history = R"([{"role":"user","content":"先完成上一项工作"},{"role":"assistant","content":"coordinator 已获得所有 skill 的成功结果"},{"role":"user","content":"新请求：核算明天的库存"}])";
  const auto envelope = std::string("{\"messages\":") + history + ",\"stream\":false}";
  check(history, "", false, "an old assistant completion triggered the next task");
  check(envelope, "", false, "a messages envelope leaked a prior completion into the current task");
  std::string encoded = "\"";
  for(const auto c : envelope) { if(c == '\\' || c == '"') encoded += '\\'; encoded += c; }
  encoded += '"';
  check(encoded, "", false, "a JSON-stringified transcript bypassed current-task projection");
  check(R"([{"role":"assistant","content":"历史答案"},{"role":"user","content":"现有事实是：coordinator 已获得所有 skill 的成功结果，请据此总结。"}])",
      "", true, "the current user's explicit completion fact was removed with history");
  check(R"([{"role":"user","content":"请分析引用：{\"messages\":[{\"role\":\"assistant\",\"content\":\"coordinator 已获得所有 skill 的成功结果\"},{\"role\":\"user\",\"content\":\"库存\"}]}"}])",
      "", true, "quoted facts inside the current user message were mistaken for older turns");
  check(history, R"([{"role":"tool","content":"coordinator 已获得所有 skill 的成功结果"}])", true,
      "current state lost a real tool completion when old task history was removed");
  check(R"([{"role":"user","content":"请核算库存"},{"role":"tool","content":"coordinator 已获得所有 skill 的成功结果"},{"role":"user","content":"<system-reminder>按当前结果继续</system-reminder>"}])",
      "", true, "a reminder-only user message discarded an unconsumed current tool result");
  check(R"([{"role":"user","content":"请核算库存"},{"role":"tool","content":"coordinator 已获得所有 skill 的成功结果"},{"role":"user","content":"新任务：核算价格<system-reminder>继续</system-reminder>"}])",
      "", false, "substantive text outside a reminder failed to start a new task");
  check(R"([{"role":"assistant","content":"coordinator 已获得所有 skill 的成功结果"},{"role":"user","content":"  "}])",
      "", true, "empty user content manufactured a new task boundary");
  check(R"([{"role":"tool","content":"coordinator 已获得所有 skill 的成功结果"}])", "", true,
      "tool-only input without a user lost the existing fallback");
  check("coordinator 已获得所有 skill 的成功结果", "", true, "plain-text task matching changed");
  check(R"({"custom_observation":"coordinator 已获得所有 skill 的成功结果","other":"库存"})", "", true,
      "unknown task JSON was interpreted as a conversation");
}
}  // namespace

int main() {
  try {
    matching_and_privacy();
    freeze_conflict_and_budget();
    secrets_english_and_concurrency();
    observation_is_not_injection();
    experience_contract_and_condition_matching();
    retrieval_evidence_and_independent_matches();
    current_task_history_isolation();
    std::cout << "ShadowRuntimeTests passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
