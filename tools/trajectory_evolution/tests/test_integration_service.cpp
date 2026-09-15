#include "trajectory_evolution/integration.h"
#include "trajectory_evolution/evolution.h"
#include "trajectory_evolution/request_binding.h"
#include "agentflow/trajectory/experience.h"
#include "integration_json.h"
#include "instrumentation.h"
#include "trace_internal.h"

#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <chrono>
#include <unistd.h>

namespace te = trajectory::evolution;
namespace v1 = te::v1;
namespace ep = agentflow::trajectory::evaluation;
namespace fs = std::filesystem;
using namespace te::integration;
const std::string token_a = "fixture-principal-alpha-00000001";
const std::string token_b = "fixture-principal-bravo-00000002";
void check(bool condition, const std::string& label) { if(!condition) throw std::runtime_error(label); }
Json read(const fs::path& path) {
  std::ifstream input(path); return Json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}
void write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path()); std::ofstream output(path); output << bytes;
  check(bool(output), "fixture write");
}
Json config(const fs::path& folder) {
  auto c = read(fs::path(INTEGRATION_TEST_SOURCE) / "config/service.json");
  c["data_dir"] = (folder / "data").string();
  c["runtime_config"] = (fs::path(INTEGRATION_TEST_SOURCE) / "config/standard-runtime.json").string();
  c["experience_template_file"] = (fs::path(INTEGRATION_TEST_SOURCE) / "config/prompts/runtime-experience.txt").string();
  c["principals"] = Json::Array{Json{{"user_id", "user-a"}, {"token_env", "INTEGRATION_TEST_A"}},
    Json{{"user_id", "user-b"}, {"token_env", "INTEGRATION_TEST_B"}}};
  c["actors"] = Json::Array{"assistant", "worker"}; c["auto_approve_experiences"] = true;
  c["learning"]["min_new_runs"] = 1; c["learning"]["min_interval_ms"] = 0;
  c["learning"]["retry_backoff_ms"] = 10;
  c["runtime_options"]["request_context_pointer"] = "/system";
  c["runtime_options"]["request_context_mode"] = "text";
  c["runtime_options"]["loop_rules"].items()[0]["max_no_progress"] = 1;
  c["runtime_options"]["action_rules"] = Json::Array{Json{{"rule_id", "video-needs-image"},
    {"target_scope", "agent:my-agent/actor:assistant"}, {"operation", "video"},
    {"required_arguments", Json::Array{"image"}}, {"reason", "缺少图片，不执行视频生成"}}};
  return c;
}
void seed(const fs::path& root) {
  ep::HybridEvaluationReport report;
  report.set_schema_version(1); report.set_evaluation_kind("fixture"); report.set_evaluator_version("fixture-v1");
  report.set_source_fingerprint("fixture-source"); report.set_source_trace_integrity_valid(true);
  report.mutable_source()->set_user_id("user-a"); report.mutable_source()->set_conversation_id("source-conv");
  report.mutable_source()->set_dialogue_id("source-fixture");
  report.mutable_dialogue_assessment()->set_status(ep::MODEL_CALL_STATUS_SUCCEEDED);
  report.mutable_dialogue_assessment()->set_confidence(0.9);
  auto* assessment = report.add_assessments(); assessment->set_final_verdict(ep::SEMANTIC_VERDICT_USEFUL);
  assessment->mutable_evidence()->set_episode_id("episode-fixture");
  assessment->mutable_evidence()->set_scope("agent:my-agent/actor:assistant");
  assessment->mutable_evidence()->add_source_event_ids("event-fixture");
  auto* reflection = report.mutable_reflection(); reflection->set_summary("缺少图片仍重试");
  reflection->add_root_causes("缺少前置素材检查"); reflection->add_evidence_event_ids("event-fixture");
  reflection->add_wasted_episode_ids("episode-fixture"); reflection->add_experience_injections("先补充图片素材，独立检索任务继续。");
  auto* experience = reflection->add_experience_details(); experience->set_target_scope("agent:my-agent/actor:assistant");
  experience->set_trigger_pattern("视频生成缺少素材"); experience->set_proposed_change("先补充图片素材，独立检索任务继续。");
  experience->add_evidence_event_ids("event-fixture");
  te::EvolutionOptions options; options.min_supporting_dialogues = 1; options.min_supporting_episodes = 1;
  auto batch = te::Pipeline({}, options).synthesize_reports({report}).report;
  ep::EvolutionMemoryStore memory; te::Pipeline::merge_memory(batch, &memory);
  check(memory.entries_size() == 1, "fixture must pass production admission");
  write(root / "by-user/user-a/memory/evolution-memory.pb", memory.SerializeAsString());
}
struct Client {
  te::IntegrationService& service;
  std::string token = token_a, run; unsigned sequence = 0;
  Json invoke(const std::string& op, Json fields = Json::Object{}, int status = 200) {
    fields["operation"] = op; if(!fields.has("request_id")) fields["request_id"] = "r-" + std::to_string(++sequence);
    if(op != "task.begin" && op != "run.begin" && op != "contract.describe") fields["run_id"] = run;
    const auto response = service.call(token, fields.dump());
    check(response.status == status, op + " HTTP " + std::to_string(response.status) + " " + response.body);
    return Json::parse(response.body)[status == 200 ? "result" : "error"];
  }
  void start(const std::string& level = "L3", const std::string& conv = "conv-a") {
    auto result = invoke("task.begin", Json{{"conversation_id", conv}, {"level", level},
      {"input", "视频生成缺少图片素材"}, {"capabilities", Json::Array{"model", "actions", "loop"}}}); run = result.str("run_id");
  }
  Json step(const std::string& execution = "main", const std::string& actor = "assistant") {
    return invoke("step.begin", Json{{"execution_id", execution}, {"actor_id", actor}, {"state", Json{{"result", "缺少素材"}}}});
  }
  Json model(const Json& step, bool mutate = false) {
    auto prepared = invoke("model.prepare", Json{{"step_id", step["step_id"]}, {"request", Json{{"system", "原有系统指令。"}, {"input", "视频生成缺少素材"}}}});
    auto actual = prepared["request"];
    if(mutate) actual["system"] = "host removed injected context";
    invoke("model.observe", Json{{"step_id", step["step_id"]}, {"call_id", prepared["call_id"]},
      {"request", actual}, {"transport_allowed", true}, {"status", "completed"}, {"output", Json{{"content", "先检查素材"}}}});
    return prepared;
  }
};

void run_tests(const fs::path& folder) {
  auto c = config(folder); write(folder / "service.json", c.dump()); seed(folder / "data");
  std::string completed;
  {
    te::IntegrationService service(folder / "service.json"); Client client{service};
    client.invoke("task.begin", Json{{"conversation_id", "missing-cap"}, {"input", "test"}, {"level", "L3"}, {"capabilities", Json::Array{"model"}}}, 409);
    client.start(); completed = client.run;
    Client other{service, token_b}; other.run = client.run; other.invoke("task.status", {}, 404);
    auto first = client.step();
    auto prepared = client.model(first);
    check(prepared["request"].str("system").find("先补充图片") != std::string::npos, "native text binding must inject actual content");
    const Json action_list = Json::Array{
      Json{{"id", "slow"}, {"operation", "lookup"}, {"arguments", Json::Object{}}},
      Json{{"id", "video"}, {"operation", "video"}, {"arguments", Json::Object{}}},
      Json{{"id", "fast"}, {"operation", "lookup"}, {"arguments", Json::Object{}}}};
    const auto actions = client.invoke("actions.prepare", Json{{"step_id", first["step_id"]}, {"actions", action_list}});
    check(actions["actions"].items()[1].str("decision") == "reject", "L2 rejects missing argument");
    client.invoke("step.begin", Json{{"state", "next"}}, 409);
    for(int i : {2, 1, 0}) {
      const auto& action = actions["actions"].items()[i];
      auto fields = Json{{"step_id", first["step_id"]}, {"call_id", action["call_id"]}, {"executed", i != 1},
        {"status", i == 2 ? "failed" : (i == 1 ? "cancelled" : "completed")}, {"output", "分支自己的结果"}};
      if(i == 2) fields["error"] = Json{{"code", "lookup_failed"}, {"retryability", "retryable"}};
      client.invoke("action.observe", fields);
    }
    auto branch = client.step("branch", "worker"); client.model(branch);
    client.invoke("step.end", Json{{"step_id", branch["step_id"]}, {"output", "分支完成"}, {"decision", branch["decision"]}});
    auto second = client.step(); check(second.str("decision") == "replan", "L3 should replan unchanged state");
    client.model(second);
    auto third = client.step(); check(third.str("decision") == "stop", "L3 should stop exhausted replan");
    client.invoke("model.prepare", Json{{"step_id", third["step_id"]}, {"request", Json::Object{}}}, 409);
    client.invoke("step.begin", Json{{"state", "new-state"}}, 409);
    client.invoke("step.end", Json{{"step_id", third["step_id"]}, {"output", "已停止"}, {"decision", "stop"}});
    client.invoke("task.end", Json{{"output", "部分完成"}, {"status", "completed"}, {"business_status", "partial"}});
    auto status = client.invoke("task.status");
    check(status["receipts"].num("applied", 0) >= 4, "real L1 L2 L3 receipts");
    service.process_evaluations(); status = client.invoke("task.status");
    check(status.str("evaluation_status") == "completed", "automatic evaluation and memory: " + status.dump());
    const auto trace = read(folder / "data/by-user/user-a/traces/conv-a" / (completed + ".trace.json"));
    check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(), "concurrent trace DAG valid");
    check(fs::exists(folder / "data/by-user/user-a/memory/evolution-memory.json"), "automatic memory written");
    // New task observes actual tampering, never a fabricated injection receipt.
    client.start("L1", "conv-modified"); first = client.step(); client.model(first, true);
    check(client.invoke("task.status")["receipts"].num("failed", 0) >= 1, "changed actual request rejected");
    client.invoke("task.end", Json{{"output", "done"}});
    // Interrupt an unresolved action; quarantine it and do not call it completed.
    client.start("L3", "conv-aborted"); first = client.step();
    client.invoke("model.prepare", Json{{"step_id", first["step_id"]}, {"request", Json{{"system", "安全"}}}});
    client.invoke("task.abort", Json{{"status", "cancelled"}, {"output", "host crashed"}});
    check(client.invoke("task.status").str("evaluation_status") == "quarantined", "incomplete trace quarantine");
    service.process_evaluations();
    const auto incomplete = read(folder / "data/by-user/user-a/quarantine/conv-aborted" / (client.run + ".trace.json"));
    check(!te::validate_trace(proto<v1::TraceRun>(incomplete)).report.trace_valid(), "offline evaluator rejects incomplete capture");
    // Privacy rejection has no persistent original request or fake result.
    client.start("L0", "conv-privacy"); first = client.step();
    client.invoke("model.prepare", Json{{"step_id", first["step_id"]}, {"request", Json{{"authorization", "fake-secret"}}}}, 400);
    client.invoke("model.prepare", Json{{"step_id", first["step_id"]}, {"request", Json{{"input", token_b}}}}, 400);
    client.invoke("task.end", Json{{"output", "sanitized"}, {"capture_complete", false}});
    check(service.call(token_a, "{\"operation\":1,\"operation\":2}").status == 400, "duplicate JSON rejected");
    check(service.call("invalid", "{}").status == 401, "auth required");
    // Native idempotency survives retries and rejects reuse with different content.
    client.start("L0", "conv-idempotent");
    auto request = Json{{"operation", "step.begin"}, {"request_id", "idempotent"}, {"run_id", client.run}, {"state", "state"}};
    const auto once = service.call(token_a, request.dump());
    check(once.status == 200 && once.body == service.call(token_a, request.dump()).body, "idempotent response");
    request["state"] = "changed"; check(service.call(token_a, request.dump()).status == 409, "conflicting retry");
    client.invoke("task.end", Json{{"output", "done"}});
  }
  te::IntegrationService restarted(folder / "service.json"); Client client{restarted}; client.run = completed;
  check(client.invoke("task.status").flag("closed", false), "state restored after process restart");
  std::cout << "Integration service: L0-L3, isolation, callbacks, concurrency, privacy, receipts, restart, automatic evaluation passed\n";
}
void delegation_tests(const fs::path& folder) {
  auto c = config(folder); c["auto_evaluate"] = false;
  write(folder / "service.json", c.dump());
  std::string resumed_run;
  Json saved_context;
  {
    te::IntegrationService service(folder / "service.json"); Client client{service};
    auto prepare = [&](const Json& step, const std::string& execution, const std::string& operation = "delegate", const std::string& completion = "execution") {
      return client.invoke("actions.prepare", Json{{"step_id", step["step_id"]}, {"actions", Json::Array{
        Json{{"id", execution}, {"operation", operation}, {"arguments", Json::Object{}},
          {"delegate", Json{{"execution_id", execution}, {"actor_id", "worker"}, {"completion", completion}}}}}}})["actions"].items()[0];
    };
    auto begin = [&](const Json& plan) {
      auto fields = plan["context"]; fields.fields().erase("run_id"); fields["state"] = "真实接收任务";
      return client.invoke("step.begin", fields);
    };
    client.start("L0", "delegation-complete"); auto root = client.step();
    auto a = prepare(root, "worker-a"), b = prepare(root, "worker-b");
    check(client.invoke("execution.status", Json{{"execution_id", "worker-a"}}).str("status") == "queued", "reserved before enqueue");
    client.invoke("task.end", Json{{"output", "premature"}}, 409);
    client.invoke("action.observe", Json{{"step_id", root["step_id"]}, {"call_id", a["call_id"]},
      {"executed", true}, {"output", "queue accepted"}}, 409);
    client.invoke("execution.end", Json{{"execution_id", "worker-a"}}, 409);
    Client stranger{service, token_b}; stranger.run = client.run;
    stranger.invoke("execution.status", Json{{"execution_id", "worker-a"}}, 404);
    auto bs = begin(b); client.model(bs);
    const auto b_end = client.invoke("execution.end", Json{{"execution_id", "worker-b"}, {"status", "failed"}, {"output", "独立分支的已知失败"}, {"request_id", "b-end"}});
    check(client.invoke("execution.end", Json{{"execution_id", "worker-b"}, {"status", "failed"}, {"output", "独立分支的已知失败"}, {"request_id", "b-end"}}).dump() == b_end.dump(), "completion is idempotent");
    check(!client.invoke("task.status").flag("closed", false), "failure does not close root");
    auto as = begin(a); auto nested = prepare(as, "nested-worker");
    client.invoke("execution.end", Json{{"execution_id", "worker-a"}}, 409);
    auto ns = begin(nested); client.model(ns);
    client.invoke("execution.end", Json{{"execution_id", "nested-worker"}, {"output", "nested-result"}});
    auto next = client.invoke("step.begin", Json{{"execution_id", "worker-a"}, {"state", "子任务已完成"}});
    client.model(next);
    client.invoke("execution.end", Json{{"execution_id", "worker-a"}, {"output", "a-result"}});
    client.invoke("step.begin", Json{{"execution_id", "worker-a"}, {"state", "late delivery"}}, 409);
    client.invoke("task.end", Json{{"output", "partial"}, {"business_status", "partial"}});
    service.process_evaluations();
    auto trace = read(folder / "data/by-user/user-a/traces/delegation-complete" / (client.run + ".trace.json"));
    auto validation = te::validate_trace(proto<v1::TraceRun>(trace)).report;
    check(validation.trace_valid(), "delegated complete trace: " + json(validation).dump());
    const auto find_event = [&](const std::string& id) -> Json {
      for(const auto& event : trace["events"].items()) if(event.str("event_id") == id) return event;
      throw std::runtime_error("event missing");
    };
    check(find_event(as.str("event_id"))["caused_by"].items()[0].text() == a.str("event_id"), "dispatch causes receiving step");
    bool linked = false;
    for(const auto& event : trace["events"].items()) if(event.str("action_id") == b.str("call_id") && event.str("kind") == "action.end")
      for(const auto& cause : event["caused_by"].items()) linked |= cause.text() == b_end.str("event_id");
    check(linked, "child completion causes parent action completion");

    client.start("L3", "delegation-rejected"); root = client.step();
    // Policy rejection uses the sender's actor; receiver identity cannot bypass it.
    const auto rejected = prepare(root, "not-dispatched", "video");
    check(rejected.str("decision") == "reject", "delegated action still obeys L2");
    client.invoke("step.begin", Json{{"execution_id", "not-dispatched"}, {"state", "must not run"}}, 409);
    client.invoke("action.observe", Json{{"step_id", root["step_id"]}, {"call_id", rejected["call_id"]}, {"executed", false}, {"status", "cancelled"}});
    client.invoke("task.end", Json{{"output", "拒绝派发"}});

    client.start("L0", "delegation-abort"); root = client.step();
    a = prepare(root, "lost-parent"); b = prepare(root, "healthy-sibling");
    as = begin(a); nested = prepare(as, "lost-child"); ns = begin(nested);
    client.invoke("model.prepare", Json{{"step_id", ns["step_id"]}, {"request", Json{{"system", "pending"}}}});
    client.invoke("execution.abort", Json{{"execution_id", "lost-parent"}, {"status", "unknown"}, {"output", "lease expired"}});
    check(client.invoke("execution.status", Json{{"execution_id", "lost-child"}}).flag("ended", false), "abort closes descendants");
    check(!client.invoke("execution.status", Json{{"execution_id", "healthy-sibling"}}).flag("ended", false), "abort preserves sibling");
    check(!client.invoke("task.status").flag("capture_complete", true), "unknown branch quarantines learning, not business");
    bs = begin(b); client.model(bs);
    client.invoke("execution.end", Json{{"execution_id", "healthy-sibling"}, {"output", "usable result"}});
    client.invoke("task.end", Json{{"output", "usable partial answer"}, {"business_status", "partial"}});
    check(client.invoke("task.status").str("evaluation_status") == "quarantined", "incomplete trace excluded from memory");

    client.start("L0", "delegation-rollback"); root = client.step();
    const auto count = client.invoke("task.status").num("event_count", 0);
    const Json duplicate{{"id", "one"}, {"operation", "delegate"}, {"arguments", Json::Object{}},
      {"delegate", Json{{"execution_id", "duplicate"}, {"actor_id", "worker"}}}};
    auto duplicate2 = duplicate; duplicate2["id"] = "two";
    client.invoke("actions.prepare", Json{{"step_id", root["step_id"]}, {"actions", Json::Array{duplicate, duplicate2}}}, 409);
    check(count == client.invoke("task.status").num("event_count", 0), "invalid batch leaves no partial reservations");
    a = prepare(root, "duplicate");
    client.invoke("action.observe", Json{{"step_id", root["step_id"]}, {"call_id", a["call_id"]}, {"executed", false}, {"status", "failed"}, {"output", "enqueue failed"}});
    client.invoke("step.begin", Json{{"execution_id", "duplicate"}, {"state", "late message"}}, 409);
    client.invoke("task.end", Json{{"output", "enqueue failure was fully observed"}});

    client.start("L0", "delegation-background"); root = client.step();
    a = prepare(root, "background-worker", "send_message", "delivery");
    client.invoke("action.observe", Json{{"step_id", root["step_id"]}, {"call_id", a["call_id"]}, {"executed", true}, {"output", "broker ack"}});
    next = client.step(); client.model(next);
    client.invoke("task.end", Json{{"output", "receiver not finished"}}, 409);
    as = begin(a); client.model(as);
    const auto background_child = prepare(as, "background-grandchild", "send_message", "delivery");
    client.invoke("action.observe", Json{{"step_id", as["step_id"]}, {"call_id", background_child["call_id"]}, {"executed", true}, {"output", "delivered"}});
    client.invoke("execution.end", Json{{"execution_id", "background-worker"}}, 409);
    auto bg_child_step = begin(background_child); client.model(bg_child_step);
    client.invoke("execution.end", Json{{"execution_id", "background-grandchild"}, {"output", "background child done"}});
    const auto bg_end = client.invoke("execution.end", Json{{"execution_id", "background-worker"}, {"output", "receiver completed"}});
    next = client.invoke("step.begin", Json{{"state", "received result"}, {"caused_by", Json::Array{bg_end["event_id"]}}});
    client.model(next); client.invoke("task.end", Json{{"output", "all done"}}); service.process_evaluations();
    trace = read(folder / "data/by-user/user-a/traces/delegation-background" / (client.run + ".trace.json"));
    check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(), "background send continues independently and joins explicitly");

    client.start("L0", "delegation-fast-receiver"); root = client.step();
    a = prepare(root, "very-fast", "send_message", "delivery"); as = begin(a);
    client.invoke("execution.end", Json{{"execution_id", "very-fast"}, {"output", "finished before ack"}});
    client.invoke("task.end", Json{{"output", "ack missing"}}, 409);
    client.invoke("action.observe", Json{{"step_id", root["step_id"]}, {"call_id", a["call_id"]}, {"executed", true}, {"output", "late ack"}});
    client.invoke("task.end", Json{{"output", "done"}});

    client.start("L0", "causal-limit"); root = client.step(); Json causes = Json::Array{};
    for(int i = 0; i < 64; ++i) causes.push(client.invoke("state.record", Json{{"step_id", root["step_id"]}, {"before", i}, {"after", i + 1}})["event_id"]);
    client.invoke("step.begin", Json{{"state", "join all"}, {"caused_by", causes}});
    client.invoke("task.end", Json{{"output", "full user cause budget preserved"}});

    client.start("L0", "delegation-restart"); root = client.step();
    saved_context = prepare(root, "durable-worker")["context"]; resumed_run = client.run;
  }
  te::IntegrationService restarted(folder / "service.json"); Client client{restarted};
  client.run = resumed_run; client.sequence = 10000;
  check(client.invoke("execution.status", Json{{"execution_id", "durable-worker"}}).str("status") == "queued", "queued reservation survives restart");
  saved_context.fields().erase("run_id"); saved_context["state"] = "resumed in another process";
  auto step = client.invoke("step.begin", saved_context); client.model(step);
  client.invoke("execution.end", Json{{"execution_id", "durable-worker"}, {"output", "recovered"}});
  client.invoke("task.end", Json{{"output", "complete"}}); restarted.process_evaluations();
  auto trace = read(folder / "data/by-user/user-a/traces/delegation-restart" / (resumed_run + ".trace.json"));
  check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(), "restarted delegation trace valid");
  std::cout << "Delegation: causal contexts, pending barriers, reverse results, nested abort isolation, rollback, restart passed\n";
}

void boundary_tests(const fs::path& folder) {
  auto c = config(folder); c["application_id"] = c["agent_id"]; c.fields().erase("agent_id");
  c["policy_scopes"] = c["actors"]; c.fields().erase("actors");
  c["activity_types"] = Json{{"vendor.generate", Json{{"semantic", "model"}, {"required_input_paths", Json::Array{"/messages"}}}},
    {"warehouse.lookup", Json{{"semantic", "action"}, {"required_input_paths", Json::Array{"/query"}}}}};
  c["instrumentation"]["action_outcomes"] = Json::Array{
    Json{{"pointer", "/status"}, {"equals", "blocked"}, {"status", "failed"},
      {"error", Json{{"code", "policy_blocked"}, {"retryability", "non_retryable"}}}},
    Json{{"pointer", "/status"}, {"equals", "failed"}, {"status", "failed"}},
    Json{{"pointer", "/error"}, {"nonempty", true}, {"status", "failed"},
      {"error", Json{{"code", "tool_error"}, {"retryability", "unknown"}}}}};
  validate_instrumentation(c);
  const Json observation{{"request_id", "mapped-outcome"}, {"phase", "after"}, {"kind", "action"}, {"executed", true},
    {"context", Json{{"run_id", "mapping-fixture"}, {"activity_id", "activity-fixture"}}}};
  for(const auto& output : std::vector<Json>{Json{{"status", "failed"}}, "{\"status\":\"failed\"}",
      Json{{"error", "failed to read"}}, Json{{"error", Json{{"code", "missing"}}}}}) {
    auto request = observation; request["output"] = output;
    const auto packet = point_request(request, c);
    check(packet.str("status") == "failed" && packet["output"].dump() == output.dump(), "mapped failure preserves original result");
    check(point_request(request, Json::Object{}).str("status") == "completed", "unconfigured result mapping stays compatible");
    request["status"] = "cancelled";
    check(point_request(request, c).str("status") == "cancelled", "explicit host status takes priority");
    request.fields().erase("status"); request["executed"] = false;
    check(point_request(request, c).str("status") == "cancelled", "unexecuted operation is not a business failure");
    request["executed"] = true; request["phase"] = "abort";
    check(point_request(request, c).str("status") == "unknown", "aborted capture is not classified from partial output");
  }
  for(const auto& empty : std::vector<Json>{nullptr, false, 0, "", Json::Array{}, Json::Object{}}) {
    auto request = observation; request["output"] = Json{{"error", empty}};
    check(point_request(request, c).str("status") == "completed", "empty error must not invent failure");
  }
  for(const auto& output : std::vector<Json>{"plain text", "{invalid", Json{{"status", "succeeded"}}, Json::Array{}}) {
    auto request = observation; request["output"] = output;
    check(point_request(request, c).str("status") == "completed", "unmatched result keeps original outcome");
  }
  for(const auto* kind : {"model", "custom", "run"}) {
    auto request = observation; request["kind"] = kind; request["actual_input"] = "original";
    request["output"] = Json{{"status", "failed"}};
    check(point_request(request, c).str("status") == "completed", "action mapping never changes models or whole run status");
  }
  c["instrumentation"]["model_outcomes"] = Json::Array{
    Json{{"pointer", "/error"}, {"nonempty", true}, {"status", "failed"},
      {"error", Json{{"code", "model_error"}, {"retryability", "unknown"}}}}};
  validate_instrumentation(c);
  for(const auto* kind : {"model", "vendor.generate"}) {
    auto request = observation; request["kind"] = kind; request["actual_input"] = "original";
    request["output"] = Json{{"error", "provider timeout"}};
    auto packet = point_request(request, c);
    check(packet.str("status") == "failed" && packet["error"].str("code") == "model_error"
        && packet["output"].dump() == request["output"].dump() && !packet.has("metrics"),
        "mapped model error preserves output and unknown usage");
    request["output"] = "{\"error\":\"provider timeout\"}";
    check(point_request(request, c).str("status") == "failed", "model output accepts one JSON string layer");
    request["status"] = "unknown";
    check(point_request(request, c).str("status") == "unknown", "host model status wins");
    request.fields().erase("status"); request["error"] = Json{{"code", "host_error"}, {"retryability", "retryable"}};
    check(point_request(request, c)["error"].str("code") == "host_error", "host model error wins");
    request.fields().erase("error"); request["executed"] = false;
    check(point_request(request, c).str("status") == "cancelled", "unexecuted model stays cancelled");
    request["executed"] = true; request["phase"] = "abort";
    check(point_request(request, c).str("status") == "unknown", "aborted model capture stays unknown");
    request["phase"] = "after"; request.fields().erase("actual_input");
    check(point_request(request, c).str("operation") == "activity.abort", "mapping does not repair missing execution evidence");
    request["actual_input"] = "original";
    for(const auto& output : std::vector<Json>{nullptr, "plain text", "{invalid", Json{{"error", ""}}, Json{{"choices", Json::Array{}}}}) {
      request["output"] = output;
      check(point_request(request, c).str("status") == "completed", "model mapping requires positive match");
    }
  }
  for(const auto& rule : std::vector<Json>{
      Json{{"pointer", "bad"}, {"equals", "failed"}, {"status", "failed"}},
      Json{{"pointer", "/status"}, {"equals", "failed"}, {"nonempty", true}, {"status", "failed"}},
      Json{{"pointer", "/error"}, {"nonempty", false}, {"status", "failed"}},
      Json{{"pointer", "/error"}, {"nonempty", true}, {"status", "completed"},
        {"error", Json{{"code", "error"}, {"retryability", "unknown"}}}}}) {
    for(const auto* mapping : {"action_outcomes", "model_outcomes"}) {
      auto invalid = c; invalid["instrumentation"][mapping] = Json::Array{rule};
      bool rejected = false; try { validate_instrumentation(invalid); } catch(const Error&) { rejected = true; }
      check(rejected, "invalid outcome mapping rejected at configuration load");
    }
  }
  write(folder / "service.json", c.dump()); seed(folder / "data");
  std::string resumed_run; Json reserved;
  {
    te::IntegrationService service(folder / "service.json"); Client client{service};
    check(client.invoke("contract.describe").str("protocol") == "evolution.boundary.v1", "discover neutral contract");
    auto start = [&](const std::string& level, const std::string& conv) {
      const auto result = client.invoke("run.begin", Json{{"input", "视频生成缺少素材"}, {"conversation_id", conv},
        {"level", level}, {"capabilities", Json::Array{"model", "actions", "loop"}}, {"extensions", Json{{"domain", "testing"}}}});
      client.run = result.str("run_id");
    };
    auto begin = [&](const std::string& kind, const std::string& name, Json input, Json extra = Json::Object{}) {
      extra["kind"] = kind; extra["name"] = name; extra["input"] = input;
      return client.invoke("activity.begin", extra);
    };
    auto finish = [&](const Json& activity, const std::string& status = "completed", bool executed = true) {
      return client.invoke("activity.end", Json{{"activity_id", activity["activity_id"]}, {"status", status},
        {"output", "真实结果"}, {"executed", executed}, {"actual_input", activity["input"]}});
    };
    auto close = [&]() { return client.invoke("run.end", Json{{"status", "completed"}, {"business_status", "partial"}, {"output", "汇总"}}); };
    auto trace = [&](const std::string& conv) {
      service.process_evaluations();
      const auto value = read(folder / "data/by-user/user-a/traces" / conv / (client.run + ".trace.json"));
      const auto validation = te::validate_trace(proto<v1::TraceRun>(value));
      check(validation.report.trace_valid(), "boundary trace valid: " + json(validation.report).dump());
      return value;
    };
    client.invoke("run.begin", Json{{"input", "x"}, {"contract_version", 2}}, 400);
    start("L0", "neutral-flat");
    auto a = begin("model", "vendor-alpha", Json::Array{"a", "b"}, Json{{"actor_id", "unregistered-a"}});
    auto b = begin("model", "vendor-beta", "a bare prompt", Json{{"actor_id", "unregistered-b"}});
    auto d = begin("action", "lookup", Json{{"q", "network"}});
    auto e = begin("planning.private", "planner", Json::Array{1, 2, 3});
    client.invoke("run.end", Json{{"status", "completed"}, {"output", "too early"}}, 409);
    const auto e_end = finish(e), b_end = finish(b, "failed"), d_end = finish(d);
    check(!client.invoke("run.status").flag("closed", false), "one failed branch must not end siblings");
    finish(a);
    auto joined = begin("merge", "fan-in", "join", Json{{"caused_by", Json::Array{e_end["event_id"], b_end["event_id"], d_end["event_id"]}},
      {"extensions", Json{{"business", Json{{"visible", "保留具体自定义内容"}}}}}});
    client.invoke("event.record", Json{{"type", "state.transition"}, {"data", Json{{"before", "waiting"}, {"after", "merged"}}},
      {"activity_id", joined["activity_id"]}, {"extensions", Json{{"own_field", 42}}}});
    finish(joined); close(); auto captured = trace("neutral-flat");
    check(captured.dump().find("保留具体自定义内容") != std::string::npos, "extensions preserved");
    check(te::trace_content_jsonl(proto<v1::TraceRun>(captured)).find("保留具体自定义内容") != std::string::npos, "extensions reach model evidence input");
    for(const auto& event : captured["events"].items()) check(!event.has("loop_id"), "activity must not imply a loop iteration");
    check(client.invoke("run.status").str("evaluation_status") == "completed", "neutral data reaches evaluation and memory");

    {
      start("L0", "mapped-failure-isolation");
      auto failed = begin("action", "read", Json{{"path", "missing.txt"}});
      auto sibling = begin("action", "lookup", Json{{"q", "independent"}});
      auto observed = observation;
      observed["context"] = Json{{"run_id", client.run}, {"activity_id", failed["activity_id"]}};
      observed["output"] = "{\"status\":\"blocked\",\"error\":\"fixture restriction\"}";
      client.invoke("activity.end", point_request(observed, c));
      check(!client.invoke("run.status").flag("closed", false), "mapped failure does not end concurrent sibling");
      finish(sibling); close(); captured = trace("mapped-failure-isolation");
      bool mapped_failure = false, healthy_sibling = false;
      for(const auto& event : captured["events"].items()) {
        if(event.str("kind") != "action.end") continue;
        mapped_failure |= event.str("status") == "failed" && event["error"].str("code") == "policy_blocked"
            && event["error"].str("retryability") == "non_retryable" && event["output"].dump() == observed["output"].dump();
        healthy_sibling |= event.str("status") == "completed";
      }
      check(mapped_failure && healthy_sibling && captured["events"].items().back()["outcome"].str("execution_status") == "completed",
          "configured failure is captured without changing run execution status");
    }

    {
      start("L0", "mapped-model-failure-isolation");
      auto failed = begin("model", "provider", "question");
      auto sibling = begin("action", "lookup", Json{{"q", "independent"}});
      auto observed = observation; observed["kind"] = "model"; observed["actual_input"] = "question";
      observed["context"] = Json{{"run_id", client.run}, {"activity_id", failed["activity_id"]}};
      observed["output"] = Json{{"error", "provider timeout"}};
      client.invoke("activity.end", point_request(observed, c));
      check(!client.invoke("run.status").flag("closed", false), "mapped model failure leaves sibling running");
      finish(sibling); close(); captured = trace("mapped-model-failure-isolation");
      bool mapped_failure = false;
      for(const auto& event : captured["events"].items())
        mapped_failure |= event.str("kind") == "action.end" && event.str("status") == "failed"
            && event["error"].str("code") == "model_error" && event["output"].dump() == observed["output"].dump();
      check(mapped_failure && te::validate_trace(proto<v1::TraceRun>(captured)).report.trace_valid()
          && captured["events"].items().back()["outcome"].str("execution_status") == "completed",
          "model failure is captured without changing workflow completion");
    }

    start("L1", "neutral-bindings");
    auto messages = begin("vendor.generate", "chat", Json{{"messages", Json::Array{Json{{"role", "user"}, {"content", "素材"}}}}},
      Json{{"binding", Json{{"pointer", "/messages"}, {"mode", "system_message"}}}, {"actor_id", "dynamic-worker-17"}});
    auto text = begin("model", "different-api", Json{{"native", Json{{"instructions", "原有指令"}}}},
      Json{{"binding", Json{{"pointer", "/native/instructions"}, {"mode", "text"}}}});
    auto bare = begin("model", "string-api", "原有指令", Json{{"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    check(messages["input"].dump().find("先补充图片") != std::string::npos && text["input"].dump().find("先补充图片") != std::string::npos
      && bare["input"].text().find("先补充图片") != std::string::npos, "per-call binding injects at arbitrary JSON location");
    finish(text); finish(messages); finish(bare);
    auto tampered = begin("model", "string-api", "original", Json{{"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    tampered["input"] = "host discarded injection"; finish(tampered);
    auto receipt_status = client.invoke("run.status");
    check(receipt_status["receipts"].num("applied", 0) == 3 && receipt_status["receipts"].num("failed", 0) == 1, "verify actual native request without altering policy hash");
    client.invoke("activity.begin", Json{{"kind", "vendor.generate"}, {"name", "chat"}, {"input", Json::Object{}}}, 400);
    client.invoke("activity.begin", Json{{"kind", "model"}, {"name", "chat"}, {"input", Json{{"unmapped", Json::Array{1}}}}}, 400);
    close(); captured = trace("neutral-bindings");
    auto report = te::analyze_trace(te::validate_trace(proto<v1::TraceRun>(captured)));
    check(!report.episodes.empty() && report.episodes[0].actor_id == "my-agent/actor:assistant", "dynamic actor metadata does not change memory scope");

    {
      const auto opened = client.invoke("run.begin", Json{{"input", "库存查询"}, {"level", "L1"},
        {"capabilities", Json::Array{"model", "actions"}}, {"conversation_id", "current-request-state"}});
      client.run = opened.str("run_id");
      const Json binding{{"pointer", "/body/m~1n~0/0/messages"}, {"mode", "system_message"}};
      const Json instructions = Json::Array{
        Json{{"role", "system"}, {"content", std::string(20000, 's') + "STATIC_ONLY 视频生成缺少素材"}},
        Json{{"role", "developer"}, {"content", "DEVELOPER_ONLY 视频生成缺少素材"}},
        Json{{"role", 7}, {"content", "普通说明"}}, Json{{"role", "user"}, {"content", "库存查询"}}};
      auto payload = [&](Json messages) {
        return Json{{"body", Json{{"m/n~", Json::Array{Json{{"messages", messages}}}}}},
          {"tools", std::string(20000, 't') + "SCHEMA_ONLY 视频生成缺少素材"}};
      };
      auto current = instructions;
      current.push(Json{{"role", "assistant"}, {"content", std::string(20000, 'x')}});
      current.push(Json{{"role", "tool"}, {"content", "视频生成缺少素材"}});
      const auto original = payload(current);
      auto prepared = begin("model", "chat", original, Json{{"binding", binding}});
      check(prepared["input"].dump().find("先补充图片") != std::string::npos,
            "current tool result must trigger advice without an extra state field");
      // The sibling has no failure, even while the first activity is still open.
      auto sibling = begin("model", "chat", payload(instructions), Json{{"binding", binding}});
      check(sibling["input"].dump() == payload(instructions).dump(),
            "system/developer/schema text and a sibling state must not trigger advice");
      finish(sibling); finish(prepared);
      for(const auto& state : std::vector<Json>{"", Json::Object{}, false}) {
        auto explicit_state = begin("model", "chat", original, Json{{"binding", binding}, {"state", state}});
        check(explicit_state["input"].dump() == original.dump(), "explicit empty state remains authoritative");
        finish(explicit_state);
      }
      auto implicit_state = begin("model", "chat", original, Json{{"binding", binding}, {"state", nullptr}});
      check(implicit_state["input"].dump().find("先补充图片") != std::string::npos, "null state uses the current request");
      finish(implicit_state);
      auto observed_only = begin("model", "chat", original, Json{{"binding", binding}, {"apply_policy", false}});
      check(observed_only["input"].dump() == original.dump(), "observation-only input remains untouched"); finish(observed_only);
      auto missing = payload(Json::Array{}); missing["body"]["m/n~"].items()[0].fields().erase("messages");
      auto absent_messages = begin("model", "chat", missing, Json{{"binding", binding}});
      check(absent_messages["input"].dump() == missing.dump(), "missing messages do not select advice from tool schemas"); finish(absent_messages);
      auto alias = begin("vendor.generate", "chat", Json{{"messages", Json::Array{Json{{"role", "tool"}, {"content", "视频生成缺少素材"}}}}},
        Json{{"binding", Json{{"pointer", "/messages"}, {"mode", "system_message"}}}});
      check(alias["input"].dump().find("先补充图片") != std::string::npos, "configured model aliases use the same default state"); finish(alias);
      auto explicit_match = begin("model", "chat", payload(instructions), Json{{"binding", binding}, {"state", "视频生成缺少素材"}});
      check(explicit_match["input"].dump().find("先补充图片") != std::string::npos, "explicit state overrides the neutral request"); finish(explicit_match);
      auto native = begin("model", "text-api", "视频生成缺少素材",
        Json{{"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
      check(native["input"].text().find("先补充图片") != std::string::npos, "bare text request supplies retrieval context"); finish(native);
      auto action = begin("action", "lookup", Json{{"current", "available"}}); finish(action);
      close(); captured = trace("current-request-state");
      unsigned queries = 0; bool original_preserved = false, action_state = false;
      for(const auto& event : captured["events"].items()) {
        if(event.str("kind") == "step.start" && event.str("step_id") == "step-" + prepared.str("activity_id"))
          original_preserved = event["input"].dump() == original.dump();
        if(event["attributes"].str("type") != "policy.query") continue;
        const auto& query = event["attributes"]["decision"]["query"];
        const auto state = query["advice_query"].str("state"); ++queries;
        if(event.str("step_id") == "step-" + prepared.str("activity_id")) {
          check(state.size() <= 8192 && state.find("视频生成缺少素材") != std::string::npos,
                "bounded retrieval keeps the latest tool result");
          check(state.find("STATIC_ONLY") == std::string::npos && state.find("DEVELOPER_ONLY") == std::string::npos
              && state.find("SCHEMA_ONLY") == std::string::npos, "retrieval excludes static instructions and schemas");
        }
        if(query.str("boundary") == "actions") action_state = state.find("available") != std::string::npos;
      }
      check(queries == 11 && original_preserved && action_state, "default state retains trace and per-call query semantics");
      check(client.invoke("run.status")["receipts"].num("failed", 0) == 0, "retrieval default preserves injection receipts");
    }

    start("L3", "neutral-loop");
    auto loop = [&](const std::string& id = "react-main") {
      return client.invoke("loop.check", Json{{"loop_id", id}, {"state", "unchanged"}});
    };
    auto commit = [&](const Json& control) { client.invoke("loop.commit", Json{{"control_id", control["control_id"]}, {"decision", control["decision"]}}); };
    auto first = loop(); check(first.str("decision") == "continue", "first explicit loop boundary"); commit(first);
    auto slow = begin("action", "lookup", Json{{"q", "slow"}}, Json{{"control_id", first["control_id"]}});
    auto pending = loop(); check(pending.str("decision") == "continue", "do not stop in-flight work in this loop"); commit(pending);
    auto independent = loop("other-loop"); check(independent.num("iteration", 0) == 1, "loop counters independent"); commit(independent);
    finish(slow);
    auto second = loop(); check(second.str("decision") == "replan", "explicit no-progress replan"); commit(second);
    auto model = begin("model", "chat", "instructions", Json{{"control_id", second["control_id"]},
      {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    check(model["input"].text().find("停止重复") != std::string::npos, "replan supplied to actual next model request"); finish(model);
    auto stop = loop(); check(stop.str("decision") == "stop", "loop budget independent of number of activities"); commit(stop);
    client.invoke("activity.begin", Json{{"kind", "action"}, {"name", "late"}, {"input", nullptr}, {"control_id", stop["control_id"]}}, 409);
    client.invoke("activity.begin", Json{{"kind", "action"}, {"name", "stale-control"}, {"input", nullptr}, {"control_id", first["control_id"]}}, 409);
    auto blocked = begin("action", "video", Json::Object{}), allowed = begin("action", "lookup", Json::Object{});
    check(blocked.str("decision") == "reject" && allowed.str("decision") == "allow", "L2 rejects individual action only");
    finish(allowed); finish(blocked, "cancelled", false); close(); trace("neutral-loop");
    check(client.invoke("run.status")["receipts"].num("failed", 0) == 0, "native model/action/loop receipt consistent");

    {
      start("L0", "neutral-event-loop");
      const auto left = loop("left"), right = loop("right"); commit(left); commit(right);
      const auto action = begin("action", "lookup", nullptr, Json{{"control_id", right["control_id"]}});
      Json event{{"request_id", "point-event-context"}, {"phase", "event"}, {"kind", "custom"}, {"name", "dispatch.result"},
        {"input", Json{{"status", "invalid_arguments"}, {"error", "empty dispatch"}}},
        {"context", Json{{"run_id", client.run}, {"control_id", left["control_id"]}}}};
      auto packet = point_request(event, c);
      check(packet["control_id"].dump() == left["control_id"].dump(), "event point retains explicit loop context");
      packet["request_id"] = "same-loop-event";
      const auto observed = client.invoke("event.record", packet);
      check(client.invoke("event.record", packet).dump() == observed.dump(), "event retry is idempotent");
      event["context"]["activity_id"] = action["activity_id"];
      packet = point_request(event, c);
      check(packet.has("activity_id") && !packet.has("control_id"), "activity context takes priority for event points");
      client.invoke("event.record", packet);
      client.invoke("event.record", Json{{"type", "bad"}, {"data", nullptr}, {"control_id", "missing"}}, 404);
      client.invoke("event.record", Json{{"type", "bad"}, {"data", nullptr}, {"control_id", left["control_id"]},
        {"activity_id", action["activity_id"]}}, 400);
      client.invoke("event.record", Json{{"type", "run.note"}, {"data", "unscoped observation"}});
      finish(action); close(); captured = trace("neutral-event-loop");
      unsigned left_events = 0, right_events = 0, unscoped = 0;
      for(const auto& recorded : captured["events"].items()) {
        if(recorded.str("kind") != "custom") continue;
        if(recorded["attributes"].str("event_type") == "run.note") {
          check(!recorded.has("loop_id"), "unscoped event does not invent loop context"); ++unscoped;
        } else {
          check(recorded.str("iteration") == "1", "custom result stays in its original iteration");
          left_events += recorded.str("loop_id") == "left";
          right_events += recorded.str("loop_id") == "right";
        }
      }
      check(left_events == 1 && right_events == 1 && unscoped == 1, "parallel loop event contexts stay distinct");
      start("L0", "neutral-event-cross-run");
      client.invoke("event.record", Json{{"type", "bad"}, {"data", nullptr}, {"control_id", left["control_id"]}}, 404);
      close(); trace("neutral-event-cross-run");
    }

    {
      auto point = [&](Json fields) {
        fields["operation"] = "point";
        fields["request_id"] = "point-fixture-" + std::to_string(++client.sequence);
        const auto response = service.call(token_a, fields.dump());
        check(response.status == 200, "reference event point: " + response.body);
        const auto result = Json::parse(response.body)["result"];
        check(!result.flag("degraded", false), "reference event capture: " + result.str("reason"));
        return result;
      };
      const Json ref{{"run", "reference-event-run"}};
      const auto opened = point(Json{{"phase", "before"}, {"kind", "run"}, {"ref", ref},
        {"input", "task"}, {"level", "L0"}, {"conversation_id", "neutral-reference-event"}});
      client.run = opened["context"].str("run_id");
      auto current = [&](const std::string& key) { auto value = ref; value["key"] = key; return value; };
      point(Json{{"phase", "guard"}, {"kind", "loop"}, {"ref", current("round-1")},
        {"loop_id", "dispatch"}, {"actor_id", "planner"}, {"state", "ready"}});
      point(Json{{"phase", "before"}, {"kind", "model"}, {"ref", current("model")}, {"input", "original"}});
      point(Json{{"phase", "after"}, {"kind", "model"}, {"ref", current("model")},
        {"actual_input", "original"}, {"executed", true}, {"output", "dispatch requested"}});
      point(Json{{"phase", "event"}, {"kind", "custom"}, {"name", "dispatch.result"}, {"ref", current("result")},
        {"input", Json{{"status", "invalid_arguments"}, {"error", "empty dispatch"}}}});
      point(Json{{"phase", "guard"}, {"kind", "loop"}, {"ref", current("round-2")},
        {"loop_id", "dispatch"}, {"actor_id", "planner"}, {"state", "error received"}});
      point(Json{{"phase", "after"}, {"kind", "run"}, {"ref", ref}, {"output", "done"}});
      captured = trace("neutral-reference-event"); unsigned results = 0;
      for(const auto& event : captured["events"].items()) if(event.str("kind") == "custom") {
        check(event.str("loop_id") == "dispatch" && event.str("iteration") == "1" && event.str("actor_id") == "planner",
            "one result point inherits its loop without host-managed control IDs");
        check(event["attributes"]["data"].str("status") == "invalid_arguments", "original result data retained"); ++results;
      }
      check(results == 1, "one logical result produces one custom event");
    }

    start("L3", "neutral-replan-observations");
    first = loop(); commit(first); second = loop(); commit(second);
    auto not_sent = begin("model", "chat", "original", Json{{"control_id", second["control_id"]},
      {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    auto sent = begin("model", "chat", "original", Json{{"control_id", second["control_id"]},
      {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    finish(not_sent, "failed", false); finish(sent);
    check(client.invoke("run.status")["receipts"].num("applied", 0) == 2, "one failed model must not suppress sibling replan receipt");
    close(); trace("neutral-replan-observations");

    start("L3", "neutral-replan-mismatch"); first = loop(); commit(first); second = loop();
    client.invoke("loop.commit", Json{{"control_id", second["control_id"]}, {"decision", "continue"}});
    model = begin("model", "chat", "original", Json{{"control_id", second["control_id"]},
      {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    check(model["input"].text().find("停止重复") == std::string::npos,
      "declined replan must not inject its context; independent L1 experience remains allowed"); finish(model);
    check(client.invoke("run.status")["receipts"].num("failed", 0) == 1, "mismatched host loop decision not credited as applied");
    auto retry_replan = loop();
    check(retry_replan.str("decision") == "replan", "declined replan does not consume its budget"); commit(retry_replan);
    auto declined_stop = loop(); check(declined_stop.str("decision") == "stop", "exhausted accepted replan proposes stop");
    client.invoke("loop.commit", Json{{"control_id", declined_stop["control_id"]}, {"decision", "continue"}});
    model = begin("model", "chat", "original", Json{{"control_id", declined_stop["control_id"]},
      {"binding", Json{{"pointer", ""}, {"mode", "text"}}}}); finish(model);
    auto check_again = loop(); commit(check_again);
    close(); trace("neutral-replan-mismatch");

    auto loop_only = client.invoke("run.begin", Json{{"input", "no tools"}, {"level", "L3"},
      {"capabilities", Json::Array{"model", "loop"}}, {"conversation_id", "model-loop-only"}});
    client.run = loop_only.str("run_id"); first = loop(); commit(first); close(); trace("model-loop-only");

    std::string long_input; for(int i = 0; i < 600; ++i) long_input += "普通任务背景。";
    auto long_run = client.invoke("run.begin", Json{{"input", long_input}, {"level", "L1"},
      {"capabilities", Json::Array{"model"}}, {"conversation_id", "long-input"}});
    client.run = long_run.str("run_id");
    model = begin("model", "chat", Json{{"system", "original"}, {"content", long_input}},
      Json{{"state", long_input}, {"binding", Json{{"pointer", "/system"}, {"mode", "text"}}}});
    check(model["input"].str("content") == long_input, "retrieval bound must not shorten the actual model input");
    finish(model); close(); captured = trace("long-input");
    check(captured["events"].items().front()["input"].text() == long_input, "trace retains complete long task input");

    start("L0", "neutral-causal-retry"); Json result_causes = Json::Array{};
    for(int round = 0; round < 2; ++round) {
      const auto control = client.invoke("loop.check", Json{{"loop_id", "retry"}, {"state", "missing permission"}, {"caused_by", result_causes}});
      commit(control);
      result_causes = Json::Array{};
      for(const auto* operation : {"lookup", "download"}) {
        const auto action = begin("action", operation, Json{{"q", "same"}}, Json{{"control_id", control["control_id"]}});
        const auto result = client.invoke("activity.end", Json{{"activity_id", action["activity_id"]}, {"executed", true}, {"status", "failed"},
          {"output", "permission missing"}, {"error", Json{{"code", "permission_denied"}, {"retryability", "non_retryable"}}}});
        result_causes.push(result["event_id"]);
      }
    }
    close(); captured = trace("neutral-causal-retry");
    report = te::analyze_trace(te::validate_trace(proto<v1::TraceRun>(captured)));
    check(report.diagnostics.size() == 2, "transitive explicit causes preserve deterministic retry detection");
    check(report.summary.invalid_loop_lower_bound == 1, "multiple invalid activities in one iteration are not multiple loops");

    start("L0", "neutral-quarantine");
    auto interrupted = begin("model", "pending", "not sent"), sibling = begin("action", "survivor", nullptr);
    client.invoke("activity.abort", Json{{"activity_id", interrupted["activity_id"]}, {"status", "unknown"}, {"output", nullptr}});
    check(!client.invoke("activity.status", Json{{"activity_id", sibling["activity_id"]}}).flag("ended", true), "abort does not cascade");
    check(!client.invoke("run.status")["observed_boundaries"].flag("model", false), "abort does not fabricate observed model boundary");
    finish(sibling); close(); check(client.invoke("run.status").str("evaluation_status") == "quarantined", "unknown observations excluded from learning");

    start("L0", "neutral-privacy");
    client.invoke("event.record", Json{{"type", "debug"}, {"data", Json{{"api_key", "secret"}}}}, 400);
    client.invoke("event.record", Json{{"type", "debug"}, {"data", nullptr}, {"extensions", Json{{"text", token_a}}}}, 400);
    client.invoke("event.record", Json{{"type", "advice.applied"}, {"data", Json{{"status", "applied"}}}});
    check(client.invoke("run.status")["receipts"].fields().empty(), "extension cannot forge core receipt");
    client.invoke("capture.gap", Json{{"reason", "privacy_rejected"}}); close();

    auto diagnostic = Json::parse(service.call(token_a, Json{{"operation", "point"}, {"request_id", "diagnostic-open"},
        {"phase", "before"}, {"kind", "run"}, {"level", "L0"}, {"conversation_id", "gap-diagnostic"}, {"input", "task"}}.dump()).body)["result"];
    auto failed_point = Json::parse(service.call(token_a, Json{{"operation", "point"}, {"request_id", "diagnostic-fail"},
        {"phase", "before"}, {"kind", "custom"}, {"context", diagnostic["context"]},
        {"input", Json{{"api_key", "never-persist-diagnostic-value"}}}}.dump()).body)["result"];
    check(failed_point.flag("degraded", false) && failed_point.str("reason") == "sensitive_field_rejected",
        "point failure preserves a safe diagnostic code");
    client.run = diagnostic.str("run_id");
    client.invoke("capture.gap", Json{{"reason", "missing_boundary"}, {"error_code", "unsafe code\ntext"}}, 400);
    client.invoke("capture.gap", Json{{"reason", "missing_boundary"}, {"error_code", token_a}}, 400);
    close();
    service.process_evaluations();
    const auto diagnostic_trace = read(folder / "data/by-user/user-a/quarantine/gap-diagnostic" / (client.run + ".trace.json"));
    bool has_code = false;
    for(const auto& event : diagnostic_trace["events"].items())
      has_code |= event["attributes"].str("error_code") == "sensitive_field_rejected";
    check(has_code && diagnostic_trace.dump().find("never-persist-diagnostic-value") == std::string::npos,
        "capture gap retains the rejection cause but no private payload");

    auto minimal = client.invoke("run.begin", Json{{"input", "non-chat task"}}); client.run = minimal.str("run_id");
    auto request = Json{{"kind", "action"}, {"name", "queue-consumer"}, {"input", nullptr}, {"request_id", "durable-reservation"}};
    reserved = client.invoke("activity.begin", request); resumed_run = client.run;
    check(reserved.dump() == client.invoke("activity.begin", request).dump(), "neutral operation idempotent");
    request["input"] = "changed"; client.invoke("activity.begin", request, 409);
    Client stranger{service, token_b}; stranger.run = client.run; stranger.invoke("run.status", {}, 404);
  }
  te::IntegrationService service(folder / "service.json"); Client receiver{service}; receiver.run = resumed_run; receiver.sequence = 10000;
  check(!receiver.invoke("activity.status", Json{{"activity_id", reserved["activity_id"]}}).flag("ended", true), "reservation survives producer exit and service restart");
  receiver.invoke("activity.end", Json{{"activity_id", reserved["activity_id"]}, {"status", "completed"}, {"executed", true}, {"output", "remote result"}});
  receiver.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}}); service.process_evaluations();
  check(receiver.invoke("run.status").str("evaluation_status") == "completed", "minimal no-conversation task evaluates after restart");
  std::cout << "Neutral boundaries: arbitrary actors, flat concurrency, JSON bindings, independent loops, extensions, privacy, restart passed\n";
}

void lifecycle_tests(const fs::path& folder) {
  auto c = config(folder); c["auto_evaluate"] = false; c["max_stored_runs"] = 2; c["run_lease_ms"] = 300;
  write(folder / "service.json", c.dump()); seed(folder / "data");
  te::IntegrationService service(folder / "service.json"); Client client{service};
  Json original;
  for(int i = 0; i < 8; ++i) {
    auto request = Json{{"input", "retained"}, {"request_id", "retained-" + std::to_string(i)}};
    auto run = client.invoke("run.begin", request); client.run = run.str("run_id");
    if(i == 0) original = run;
    client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}}); service.process_evaluations();
  }
  check(fs::exists(folder / "data/archive/runs"), "terminal runs archive automatically at capacity");
  check(client.invoke("run.begin", Json{{"input", "retained"}, {"request_id", "retained-0"}}).dump() == original.dump(),
      "archived begin remains idempotent");
  auto run = client.invoke("run.begin", Json{{"input", "long external task"}}); client.run = run.str("run_id");
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  check(client.invoke("run.heartbeat").flag("alive", false), "heartbeat refreshes a long task lease");
  std::this_thread::sleep_for(std::chrono::milliseconds(180)); service.process_evaluations();
  check(!client.invoke("run.status").flag("closed", true), "heartbeat prevents premature expiry");
  std::this_thread::sleep_for(std::chrono::milliseconds(180)); service.process_evaluations();
  const auto expired = client.invoke("run.status");
  check(expired.flag("closed", false) && !expired.flag("capture_complete", true)
      && expired.str("evaluation_status") == "quarantined", "expired observer state is unknown, never business failure or learnable success");
  std::cout << "Lifecycle: automatic archive, durable idempotency, heartbeat and lease quarantine passed\n";
}

int main(int argc, char** argv) {
  try {
    ::setenv("INTEGRATION_TEST_A", token_a.c_str(), 1); ::setenv("INTEGRATION_TEST_B", token_b.c_str(), 1);
    if(argc == 3 && std::string(argv[1]) == "--fixture") {
      const auto folder = fs::absolute(argv[2]);
      check(!fs::exists(folder / "service.json"), "fixture output already exists");
      auto c = config(folder); c["port"] = 0;
      write(folder / "service.json", c.dump()); seed(folder / "data");
      std::cout << "Fixture ready\n"; return 0;
    }
    std::string path = (fs::current_path() / "integration-service.XXXXXX").string();
    check(::mkdtemp(path.data()), "temporary test directory"); run_tests(path); delegation_tests(fs::path(path) / "delegation");
    boundary_tests(fs::path(path) / "boundary"); lifecycle_tests(fs::path(path) / "lifecycle"); return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
