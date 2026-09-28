// test_integration_service 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

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

// 测试辅助函数：check。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void check(bool condition, const std::string &label) {
  if (!condition)
    throw std::runtime_error(label);
}

// 按调用方给出的大小上限读取文件；读取失败向上层报告，不能用空数据冒充成功。
Json read(const fs::path &path) {
  std::ifstream input(path);
  check(bool(input), "fixture read unavailable: " + path.string());
  return Json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

// 按照本模块的输出约定写入内容；调用方负责提供已验证的目标和数据。
void write(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path);
  output << bytes;
  check(bool(output), "fixture write");
}

// 测试辅助函数：config。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
Json config(const fs::path &folder) {
  auto c = read(fs::path(INTEGRATION_TEST_SOURCE) / "config/service.json");
  c["data_dir"] = (folder / "data").string();
  c["runtime_config"] =
      (fs::path(INTEGRATION_TEST_SOURCE) / "config/standard-runtime.json").string();
  c["experience_template_file"] =
      (fs::path(INTEGRATION_TEST_SOURCE) / "config/prompts/runtime-experience.txt").string();
  c["principals"] = Json::Array{Json{{"user_id", "user-a"}, {"token_env", "INTEGRATION_TEST_A"}},
                                Json{{"user_id", "user-b"}, {"token_env", "INTEGRATION_TEST_B"}}};
  c["actors"] = Json::Array{"assistant", "worker"};
  c["auto_approve_experiences"] = true;
  c["learning"]["min_new_runs"] = 1;
  c["learning"]["min_interval_ms"] = 0;
  c["learning"]["retry_backoff_ms"] = 10;
  c["runtime_options"]["request_context_pointer"] = "/system";
  c["runtime_options"]["request_context_mode"] = "text";
  c["runtime_options"]["loop_rules"].items()[0]["max_no_progress"] = 1;
  c["runtime_options"]["action_rules"] =
      Json::Array{Json{{"rule_id", "video-needs-image"},
                       {"target_scope", "agent:my-agent/actor:assistant"},
                       {"operation", "video"},
                       {"required_arguments", Json::Array{"image"}},
                       {"reason", "缺少图片，不执行视频生成"}}};
  return c;
}

// 测试辅助函数：seed。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void seed(const fs::path &root) {
  ep::HybridEvaluationReport report;
  report.set_schema_version(1);
  report.set_evaluation_kind("fixture");
  report.set_evaluator_version("fixture-v1");
  report.set_source_fingerprint("fixture-source");
  report.set_source_trace_integrity_valid(true);
  report.mutable_source()->set_user_id("user-a");
  report.mutable_source()->set_conversation_id("source-conv");
  report.mutable_source()->set_dialogue_id("source-fixture");
  report.mutable_dialogue_assessment()->set_status(ep::MODEL_CALL_STATUS_SUCCEEDED);
  report.mutable_dialogue_assessment()->set_confidence(0.9);
  auto *assessment = report.add_assessments();
  assessment->set_final_verdict(ep::SEMANTIC_VERDICT_USEFUL);
  assessment->mutable_evidence()->set_episode_id("episode-fixture");
  assessment->mutable_evidence()->set_scope("agent:my-agent/actor:assistant");
  assessment->mutable_evidence()->add_source_event_ids("event-fixture");
  auto *reflection = report.mutable_reflection();
  reflection->set_summary("缺少图片仍重试");
  reflection->add_root_causes("缺少前置素材检查");
  reflection->add_evidence_event_ids("event-fixture");
  reflection->add_wasted_episode_ids("episode-fixture");
  reflection->add_experience_injections("先补充图片素材，独立检索任务继续。");
  auto *experience = reflection->add_experience_details();
  experience->set_target_scope("agent:my-agent/actor:assistant");
  experience->set_trigger_pattern("视频生成缺少素材");
  experience->set_proposed_change("先补充图片素材，独立检索任务继续。");
  experience->add_evidence_event_ids("event-fixture");
  te::EvolutionOptions options;
  options.min_supporting_dialogues = 1;
  options.min_supporting_episodes = 1;
  auto batch = te::Pipeline({}, options).synthesize_reports({report}).report;
  ep::EvolutionMemoryStore memory;
  te::Pipeline::merge_memory(batch, &memory);
  check(memory.entries_size() == 1, "fixture must pass production admission");
  write(root / "by-user/user-a/memory/evolution-memory.pb", memory.SerializeAsString());
}

struct Client {
  te::IntegrationService &service;
  std::string token = token_a, run;
  unsigned sequence = 0;

  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  Json invoke(const std::string &op, Json fields = Json::Object{}, int status = 200) {
    fields["operation"] = op;
    if (!fields.has("request_id"))
      fields["request_id"] = "r-" + std::to_string(++sequence);
    if (op != "task.begin" && op != "run.begin" && op != "contract.describe")
      fields["run_id"] = run;
    const auto response = service.call(token, fields.dump());
    check(response.status == status,
          op + " HTTP " + std::to_string(response.status) + " " + response.body);
    return Json::parse(response.body)[status == 200 ? "result" : "error"];
  }

  // 开始一个新的规范运行上下文，绑定应用、用户与会话身份。
  void start(const std::string &level = "L3", const std::string &conv = "conv-a") {
    auto result =
        invoke("task.begin", Json{{"conversation_id", conv},
                                  {"level", level},
                                  {"input", "视频生成缺少图片素材"},
                                  {"capabilities", Json::Array{"model", "actions", "loop"}}});
    run = result.str("run_id");
  }

  // 开始当前步骤并取得规范上下文，后续观测和结束复用该身份。
  Json step(const std::string &execution = "main", const std::string &actor = "assistant") {
    return invoke("step.begin", Json{{"execution_id", execution},
                                     {"actor_id", actor},
                                     {"state", Json{{"result", "缺少素材"}}}});
  }

  // 模拟模型准备和观测，mutate 用于验证宿主丢弃注入内容时不会谎报采用策略。
  Json model(const Json &step, bool mutate = false) {
    auto prepared = invoke("model.prepare", Json{{"step_id", step["step_id"]},
                                                 {"request", Json{{"system", "原有系统指令。"},
                                                                  {"input", "视频生成缺少素材"}}}});
    auto actual = prepared["request"];
    if (mutate)
      actual["system"] = "host removed injected context";
    invoke("model.observe", Json{{"step_id", step["step_id"]},
                                 {"call_id", prepared["call_id"]},
                                 {"request", actual},
                                 {"transport_allowed", true},
                                 {"status", "completed"},
                                 {"output", Json{{"content", "先检查素材"}}}});
    return prepared;
  }
};

// 测试辅助函数：run_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void run_tests(const fs::path &folder) {
  auto c = config(folder);
  write(folder / "service.json", c.dump());
  seed(folder / "data");
  std::string completed;
  {
    te::IntegrationService service(folder / "service.json");
    Client client{service};
    client.invoke("task.begin",
                  Json{{"conversation_id", "missing-cap"},
                       {"input", "test"},
                       {"level", "L3"},
                       {"capabilities", Json::Array{"model"}}},
                  409);
    client.start();
    completed = client.run;
    Client other{service, token_b};
    other.run = client.run;
    other.invoke("task.status", {}, 404);
    auto first = client.step();
    auto prepared = client.model(first);
    check(prepared["request"].str("system").find("先补充图片") != std::string::npos,
          "native text binding must inject actual content");
    const Json action_list =
        Json::Array{Json{{"id", "slow"}, {"operation", "lookup"}, {"arguments", Json::Object{}}},
                    Json{{"id", "video"}, {"operation", "video"}, {"arguments", Json::Object{}}},
                    Json{{"id", "fast"}, {"operation", "lookup"}, {"arguments", Json::Object{}}}};
    const auto actions = client.invoke(
        "actions.prepare", Json{{"step_id", first["step_id"]}, {"actions", action_list}});
    check(actions["actions"].items()[1].str("decision") == "reject", "L2 rejects missing argument");
    client.invoke("step.begin", Json{{"state", "next"}}, 409);
    for (int i : {2, 1, 0}) {
      const auto &action = actions["actions"].items()[i];
      auto fields = Json{{"step_id", first["step_id"]},
                         {"call_id", action["call_id"]},
                         {"executed", i != 1},
                         {"status", i == 2 ? "failed" : (i == 1 ? "cancelled" : "completed")},
                         {"output", "分支自己的结果"}};
      if (i == 2)
        fields["error"] = Json{{"code", "lookup_failed"}, {"retryability", "retryable"}};
      client.invoke("action.observe", fields);
    }
    auto branch = client.step("branch", "worker");
    client.model(branch);
    client.invoke("step.end", Json{{"step_id", branch["step_id"]},
                                   {"output", "分支完成"},
                                   {"decision", branch["decision"]}});
    auto second = client.step();
    check(second.str("decision") == "replan", "L3 should replan unchanged state");
    client.model(second);
    auto third = client.step();
    check(third.str("decision") == "stop", "L3 should stop exhausted replan");
    client.invoke("model.prepare", Json{{"step_id", third["step_id"]}, {"request", Json::Object{}}},
                  409);
    client.invoke("step.begin", Json{{"state", "new-state"}}, 409);
    client.invoke("step.end",
                  Json{{"step_id", third["step_id"]}, {"output", "已停止"}, {"decision", "stop"}});
    client.invoke(
        "task.end",
        Json{{"output", "部分完成"}, {"status", "completed"}, {"business_status", "partial"}});
    auto status = client.invoke("task.status");
    check(status["receipts"].num("applied", 0) >= 4, "real L1 L2 L3 receipts");
    service.process_evaluations();
    status = client.invoke("task.status");
    check(status.str("evaluation_status") == "completed",
          "automatic evaluation and memory: " + status.dump());
    const auto trace =
        read(folder / "data/by-user/user-a/traces/conv-a" / (completed + ".trace.json"));
    check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
          "concurrent trace DAG valid");
    check(fs::exists(folder / "data/by-user/user-a/memory/evolution-memory.json"),
          "automatic memory written");
    // 新任务看到真实篡改，不使用伪造的注入回执。
    client.start("L1", "conv-modified");
    first = client.step();
    client.model(first, true);
    check(client.invoke("task.status")["receipts"].num("failed", 0) >= 1,
          "changed actual request rejected");
    client.invoke("task.end", Json{{"output", "done"}});
    // 打断未决动作时隔离它，不能记为完成。
    client.start("L3", "conv-aborted");
    first = client.step();
    client.invoke("model.prepare",
                  Json{{"step_id", first["step_id"]}, {"request", Json{{"system", "安全"}}}});
    client.invoke("task.abort", Json{{"status", "cancelled"}, {"output", "host crashed"}});
    check(client.invoke("task.status").str("evaluation_status") == "quarantined",
          "incomplete trace quarantine");
    service.process_evaluations();
    const auto incomplete =
        read(folder / "data/by-user/user-a/quarantine/conv-aborted" / (client.run + ".trace.json"));
    check(!te::validate_trace(proto<v1::TraceRun>(incomplete)).report.trace_valid(),
          "offline evaluator rejects incomplete capture");
    // 隐私拒绝不能持久化原始请求，也不能产生假结果。
    client.start("L0", "conv-privacy");
    first = client.step();
    client.invoke(
        "model.prepare",
        Json{{"step_id", first["step_id"]}, {"request", Json{{"authorization", "fake-secret"}}}},
        400);
    client.invoke("model.prepare",
                  Json{{"step_id", first["step_id"]}, {"request", Json{{"input", token_b}}}}, 400);
    client.invoke("task.end", Json{{"output", "sanitized"}, {"capture_complete", false}});
    check(service.call(token_a, "{\"operation\":1,\"operation\":2}").status == 400,
          "duplicate JSON rejected");
    check(service.call("invalid", "{}").status == 401, "auth required");
    // 原生幂等性跨重试保持，拒绝相同身份复用不同内容。
    client.start("L0", "conv-idempotent");
    auto request = Json{{"operation", "step.begin"},
                        {"request_id", "idempotent"},
                        {"run_id", client.run},
                        {"state", "state"}};
    const auto once = service.call(token_a, request.dump());
    check(once.status == 200 && once.body == service.call(token_a, request.dump()).body,
          "idempotent response");
    request["state"] = "changed";
    check(service.call(token_a, request.dump()).status == 409, "conflicting retry");
    client.invoke("task.end", Json{{"output", "done"}});
  }
  te::IntegrationService restarted(folder / "service.json");
  Client client{restarted};
  client.run = completed;
  check(client.invoke("task.status").flag("closed", false), "state restored after process restart");
  std::cout << "Integration service: L0-L3, isolation, callbacks, concurrency, privacy, receipts, "
               "restart, automatic evaluation passed\n";
}

// 测试辅助函数：delegation_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void delegation_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  write(folder / "service.json", c.dump());
  std::string resumed_run;
  Json saved_context;
  {
    te::IntegrationService service(folder / "service.json");
    Client client{service};
    auto prepare = [&](const Json &step, const std::string &execution,
                       const std::string &operation = "delegate",
                       const std::string &completion = "execution") {
      return client
          .invoke(
              "actions.prepare",
              Json{{"step_id", step["step_id"]},
                   {"actions",
                    Json::Array{Json{{"id", execution},
                                     {"operation", operation},
                                     {"arguments", Json::Object{}},
                                     {"delegate", Json{{"execution_id", execution},
                                                       {"actor_id", "worker"},
                                                       {"completion", completion}}}}}}})["actions"]
          .items()[0];
    };
    auto begin = [&](const Json &plan) {
      auto fields = plan["context"];
      fields.fields().erase("run_id");
      fields["state"] = "真实接收任务";
      return client.invoke("step.begin", fields);
    };
    client.start("L0", "delegation-complete");
    auto root = client.step();
    auto a = prepare(root, "worker-a"), b = prepare(root, "worker-b");
    check(client.invoke("execution.status", Json{{"execution_id", "worker-a"}}).str("status") ==
              "queued",
          "reserved before enqueue");
    client.invoke("task.end", Json{{"output", "premature"}}, 409);
    client.invoke("action.observe",
                  Json{{"step_id", root["step_id"]},
                       {"call_id", a["call_id"]},
                       {"executed", true},
                       {"output", "queue accepted"}},
                  409);
    client.invoke("execution.end", Json{{"execution_id", "worker-a"}}, 409);
    Client stranger{service, token_b};
    stranger.run = client.run;
    stranger.invoke("execution.status", Json{{"execution_id", "worker-a"}}, 404);
    auto bs = begin(b);
    client.model(bs);
    const auto b_end = client.invoke("execution.end", Json{{"execution_id", "worker-b"},
                                                           {"status", "failed"},
                                                           {"output", "独立分支的已知失败"},
                                                           {"request_id", "b-end"}});
    check(client.invoke("execution.end", Json{{"execution_id", "worker-b"},
                                              {"status", "failed"},
                                              {"output", "独立分支的已知失败"},
                                              {"request_id", "b-end"}})
                  .dump() == b_end.dump(),
          "completion is idempotent");
    check(!client.invoke("task.status").flag("closed", false), "failure does not close root");
    auto as = begin(a);
    auto nested = prepare(as, "nested-worker");
    client.invoke("execution.end", Json{{"execution_id", "worker-a"}}, 409);
    auto ns = begin(nested);
    client.model(ns);
    client.invoke("execution.end",
                  Json{{"execution_id", "nested-worker"}, {"output", "nested-result"}});
    auto next =
        client.invoke("step.begin", Json{{"execution_id", "worker-a"}, {"state", "子任务已完成"}});
    client.model(next);
    client.invoke("execution.end", Json{{"execution_id", "worker-a"}, {"output", "a-result"}});
    client.invoke("step.begin", Json{{"execution_id", "worker-a"}, {"state", "late delivery"}},
                  409);
    client.invoke("task.end", Json{{"output", "partial"}, {"business_status", "partial"}});
    service.process_evaluations();
    auto trace = read(folder / "data/by-user/user-a/traces/delegation-complete" /
                      (client.run + ".trace.json"));
    auto validation = te::validate_trace(proto<v1::TraceRun>(trace)).report;
    check(validation.trace_valid(), "delegated complete trace: " + json(validation).dump());
    const auto find_event = [&](const std::string &id) -> Json {
      for (const auto &event : trace["events"].items())
        if (event.str("event_id") == id)
          return event;
      throw std::runtime_error("event missing");
    };
    check(find_event(as.str("event_id"))["caused_by"].items()[0].text() == a.str("event_id"),
          "dispatch causes receiving step");
    bool linked = false;
    for (const auto &event : trace["events"].items())
      if (event.str("action_id") == b.str("call_id") && event.str("kind") == "action.end")
        for (const auto &cause : event["caused_by"].items())
          linked |= cause.text() == b_end.str("event_id");
    check(linked, "child completion causes parent action completion");

    client.start("L3", "delegation-rejected");
    root = client.step();
    // 策略拒绝依据发送方执行者，接收方身份不能绕过它。
    const auto rejected = prepare(root, "not-dispatched", "video");
    check(rejected.str("decision") == "reject", "delegated action still obeys L2");
    client.invoke("step.begin", Json{{"execution_id", "not-dispatched"}, {"state", "must not run"}},
                  409);
    client.invoke("action.observe", Json{{"step_id", root["step_id"]},
                                         {"call_id", rejected["call_id"]},
                                         {"executed", false},
                                         {"status", "cancelled"}});
    client.invoke("task.end", Json{{"output", "拒绝派发"}});

    client.start("L0", "delegation-abort");
    root = client.step();
    a = prepare(root, "lost-parent");
    b = prepare(root, "healthy-sibling");
    as = begin(a);
    nested = prepare(as, "lost-child");
    ns = begin(nested);
    client.invoke("model.prepare",
                  Json{{"step_id", ns["step_id"]}, {"request", Json{{"system", "pending"}}}});
    client.invoke(
        "execution.abort",
        Json{{"execution_id", "lost-parent"}, {"status", "unknown"}, {"output", "lease expired"}});
    check(client.invoke("execution.status", Json{{"execution_id", "lost-child"}})
              .flag("ended", false),
          "abort closes descendants");
    check(!client.invoke("execution.status", Json{{"execution_id", "healthy-sibling"}})
               .flag("ended", false),
          "abort preserves sibling");
    check(!client.invoke("task.status").flag("capture_complete", true),
          "unknown branch quarantines learning, not business");
    bs = begin(b);
    client.model(bs);
    client.invoke("execution.end",
                  Json{{"execution_id", "healthy-sibling"}, {"output", "usable result"}});
    client.invoke("task.end",
                  Json{{"output", "usable partial answer"}, {"business_status", "partial"}});
    check(client.invoke("task.status").str("evaluation_status") == "quarantined",
          "incomplete trace excluded from memory");

    client.start("L0", "delegation-rollback");
    root = client.step();
    const auto count = client.invoke("task.status").num("event_count", 0);
    const Json duplicate{{"id", "one"},
                         {"operation", "delegate"},
                         {"arguments", Json::Object{}},
                         {"delegate", Json{{"execution_id", "duplicate"}, {"actor_id", "worker"}}}};
    auto duplicate2 = duplicate;
    duplicate2["id"] = "two";
    client.invoke(
        "actions.prepare",
        Json{{"step_id", root["step_id"]}, {"actions", Json::Array{duplicate, duplicate2}}}, 409);
    check(count == client.invoke("task.status").num("event_count", 0),
          "invalid batch leaves no partial reservations");
    a = prepare(root, "duplicate");
    client.invoke("action.observe", Json{{"step_id", root["step_id"]},
                                         {"call_id", a["call_id"]},
                                         {"executed", false},
                                         {"status", "failed"},
                                         {"output", "enqueue failed"}});
    client.invoke("step.begin", Json{{"execution_id", "duplicate"}, {"state", "late message"}},
                  409);
    client.invoke("task.end", Json{{"output", "enqueue failure was fully observed"}});

    client.start("L0", "delegation-background");
    root = client.step();
    a = prepare(root, "background-worker", "send_message", "delivery");
    client.invoke("action.observe", Json{{"step_id", root["step_id"]},
                                         {"call_id", a["call_id"]},
                                         {"executed", true},
                                         {"output", "broker ack"}});
    next = client.step();
    client.model(next);
    client.invoke("task.end", Json{{"output", "receiver not finished"}}, 409);
    as = begin(a);
    client.model(as);
    const auto background_child = prepare(as, "background-grandchild", "send_message", "delivery");
    client.invoke("action.observe", Json{{"step_id", as["step_id"]},
                                         {"call_id", background_child["call_id"]},
                                         {"executed", true},
                                         {"output", "delivered"}});
    client.invoke("execution.end", Json{{"execution_id", "background-worker"}}, 409);
    auto bg_child_step = begin(background_child);
    client.model(bg_child_step);
    client.invoke("execution.end", Json{{"execution_id", "background-grandchild"},
                                        {"output", "background child done"}});
    const auto bg_end = client.invoke("execution.end", Json{{"execution_id", "background-worker"},
                                                            {"output", "receiver completed"}});
    next = client.invoke("step.begin", Json{{"state", "received result"},
                                            {"caused_by", Json::Array{bg_end["event_id"]}}});
    client.model(next);
    client.invoke("task.end", Json{{"output", "all done"}});
    service.process_evaluations();
    trace = read(folder / "data/by-user/user-a/traces/delegation-background" /
                 (client.run + ".trace.json"));
    check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
          "background send continues independently and joins explicitly");

    client.start("L0", "delegation-fast-receiver");
    root = client.step();
    a = prepare(root, "very-fast", "send_message", "delivery");
    as = begin(a);
    client.invoke("execution.end",
                  Json{{"execution_id", "very-fast"}, {"output", "finished before ack"}});
    client.invoke("task.end", Json{{"output", "ack missing"}}, 409);
    client.invoke("action.observe", Json{{"step_id", root["step_id"]},
                                         {"call_id", a["call_id"]},
                                         {"executed", true},
                                         {"output", "late ack"}});
    client.invoke("task.end", Json{{"output", "done"}});

    client.start("L0", "causal-limit");
    root = client.step();
    Json causes = Json::Array{};
    for (int i = 0; i < 64; ++i)
      causes.push(client.invoke(
          "state.record",
          Json{{"step_id", root["step_id"]}, {"before", i}, {"after", i + 1}})["event_id"]);
    client.invoke("step.begin", Json{{"state", "join all"}, {"caused_by", causes}});
    client.invoke("task.end", Json{{"output", "full user cause budget preserved"}});

    client.start("L0", "delegation-restart");
    root = client.step();
    saved_context = prepare(root, "durable-worker")["context"];
    resumed_run = client.run;
  }
  te::IntegrationService restarted(folder / "service.json");
  Client client{restarted};
  client.run = resumed_run;
  client.sequence = 10000;
  check(client.invoke("execution.status", Json{{"execution_id", "durable-worker"}}).str("status") ==
            "queued",
        "queued reservation survives restart");
  saved_context.fields().erase("run_id");
  saved_context["state"] = "resumed in another process";
  auto step = client.invoke("step.begin", saved_context);
  client.model(step);
  client.invoke("execution.end", Json{{"execution_id", "durable-worker"}, {"output", "recovered"}});
  client.invoke("task.end", Json{{"output", "complete"}});
  restarted.process_evaluations();
  auto trace = read(folder / "data/by-user/user-a/traces/delegation-restart" /
                    (resumed_run + ".trace.json"));
  check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
        "restarted delegation trace valid");
  std::cout << "Delegation: causal contexts, pending barriers, reverse results, nested abort "
               "isolation, rollback, restart passed\n";
}

// 测试辅助函数：outcome_progress_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void outcome_progress_tests(const fs::path &folder) {
  auto c = config(folder);
  write(folder / "service.json", c.dump());
  te::IntegrationService service(folder / "service.json");
  Client client{service};
  client.run =
      client
          .invoke("run.begin", Json{{"input", "inspect a repeated result"},
                                    {"level", "L3"},
                                    {"conversation_id", "outcomes"},
                                    {"capabilities", Json::Array{"model", "actions", "loop"}}})
          .str("run_id");
  auto control = [&](const std::string &branch, int transport) {
    auto check = client.invoke(
        "loop.check", Json{{"loop_id", branch}, {"state", Json{{"transport_id", transport}}}});
    client.invoke("loop.commit",
                  Json{{"control_id", check["control_id"]}, {"decision", check["decision"]}});
    return check;
  };
  auto action = [&](const Json &step, const std::string &name, int business_id) {
    auto call = client.invoke("activity.begin", Json{{"kind", "action"},
                                                     {"name", name},
                                                     {"input", Json{{"path", "report.json"}}},
                                                     {"control_id", step["control_id"]}});
    client.invoke("activity.end",
                  Json{{"activity_id", call["activity_id"]},
                       {"status", "completed"},
                       {"executed", true},
                       {"output", Json{{"business_id", business_id}, {"ok", true}}}});
  };
  auto first = control("main", 1);
  action(first, "read", 1);
  auto second = control("main", 2);
  check(second.str("decision") == "continue", "first action is progress");
  action(second, "read", 2);
  auto third = control("main", 3);
  check(third.str("decision") == "continue", "changed business result is progress");
  action(third, "read", 1);
  auto recovery = control("main", 4);
  check(recovery.str("decision") == "replan",
        "repeated action outcome was hidden by transport state");
  auto model = client.invoke("activity.begin",
                             Json{{"kind", "model"},
                                  {"name", "recovery"},
                                  {"input", Json{{"system", "continue"}}},
                                  {"binding", Json{{"pointer", "/system"}, {"mode", "text"}}},
                                  {"control_id", recovery["control_id"]}});
  check(model["input"].str("system").find(recovery.str("context")) != std::string::npos,
        "replan context not injected");
  client.invoke("activity.end", Json{{"activity_id", model["activity_id"]},
                                     {"status", "completed"},
                                     {"executed", true},
                                     {"actual_input", model["input"]},
                                     {"output", "use another approach"}});
  auto sibling = control("sibling", 1);
  action(sibling, "read", 1);
  check(control("sibling", 2).str("decision") == "continue",
        "main branch progress leaked into sibling");
  action(recovery, "read", 1);
  check(control("main", 5).str("decision") == "stop", "repeat after bounded recovery did not stop");
  auto pending_first = control("pending-outcomes", 1);
  action(pending_first, "read", 1);
  auto pending_second = control("pending-outcomes", 2);
  action(pending_second, "read", 1);
  auto pending =
      client.invoke("activity.begin", Json{{"kind", "action"},
                                           {"name", "read"},
                                           {"input", Json{{"path", "report.json"}}},
                                           {"control_id", pending_second["control_id"]}});
  check(control("pending-outcomes", 3).str("decision") == "continue",
        "repeated outcome must wait for in-flight work");
  client.invoke("activity.end", Json{{"activity_id", pending["activity_id"]},
                                     {"status", "completed"},
                                     {"executed", true},
                                     {"output", Json{{"business_id", 1}, {"ok", true}}}});
  check(control("pending-outcomes", 4).str("decision") == "replan",
        "pending step must not consume repeated outcomes");
  auto overflow = control("bounded-outcomes", 1);
  for (int i = 0; i <= 256; ++i)
    action(overflow, "read", i);
  auto after_overflow = control("bounded-outcomes", 2);
  check(after_overflow.str("decision") == "continue", "new outcomes are progress");
  action(after_overflow, "read", 1);
  check(control("bounded-outcomes", 3).str("decision") == "continue",
        "outcome capacity must fall back to the host state");
  auto material = [&](const Json &step, const char *operation, const char *prompt,
                      const Json &artifacts) {
    auto call = client.invoke("activity.begin", Json{{"kind", "action"},
                                                     {"name", operation},
                                                     {"input", Json{{"prompt", prompt}}},
                                                     {"control_id", step["control_id"]}});
    client.invoke("activity.end", Json{{"activity_id", call["activity_id"]},
                                       {"status", "completed"},
                                       {"executed", true},
                                       {"output", Json{{"artifacts_json", artifacts.dump()},
                                                       {"summary", prompt}}}});
  };
  auto produced = control("material-repeat", 1);
  material(produced, "make-image", "create", Json::Array{Json{{"path", "image.png"}}});
  auto followup = control("material-repeat", 2);
  check(followup.str("decision") == "continue", "new artifact was not counted as progress");
  material(followup, "make-image", "read image and verify contrast", Json::Array{});
  auto verified = control("material-repeat", 3);
  check(verified.str("decision") == "continue",
        "verification without a new file was mistaken for no progress");
  material(verified, "make-image", "read image and verify contrast", Json::Array{});
  check(control("material-repeat", 4).str("decision") == "replan",
        "identical verification results must still trigger bounded recovery");
  auto new_asset = control("material-new", 1);
  material(new_asset, "make-image", "first", Json::Array{Json{{"path", "first.png"}}});
  new_asset = control("material-new", 2);
  material(new_asset, "make-image", "second", Json::Array{Json{{"path", "second.png"}}});
  check(control("material-new", 3).str("decision") == "continue",
        "new artifact was mistaken for no progress");
  auto ambiguous = [&](const char *branch, const Json &output) {
    material(control(branch, 1), "make-image", "first", Json::Array{Json{{"path", "first.png"}}});
    auto step = control(branch, 2);
    auto call = client.invoke("activity.begin", Json{{"kind", "action"},
                                                     {"name", "make-image"},
                                                     {"input", Json{{"prompt", "follow-up"}}},
                                                     {"control_id", step["control_id"]}});
    client.invoke("activity.end", Json{{"activity_id", call["activity_id"]},
                                       {"status", "completed"},
                                       {"executed", true},
                                       {"output", output}});
    check(control(branch, 3).str("decision") == "continue",
          "ambiguous artifact field suppressed real progress");
  };
  ambiguous("material-malformed", Json{{"artifacts_json", "not-json"}, {"summary", "new answer"}});
  ambiguous("material-input-files",
            Json{{"files", Json::Array{"input.txt"}}, {"summary", "new answer"}});
  client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
  service.process_evaluations();
  auto trace = read(folder / "data/by-user/user-a/traces/outcomes" / (client.run + ".trace.json"));
  check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
        "outcome control trace must remain valid");
}

// 测试辅助函数：aggressive_failure_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void aggressive_failure_tests(const fs::path &folder) {
  for (const bool aggressive : {false, true}) {
    const auto data = folder / (aggressive ? "aggressive" : "standard");
    auto c = config(data);
    c["runtime_options"]["aggressive_controls"] = aggressive;
    auto &rule = c["runtime_options"]["loop_rules"].items()[0];
    rule["no_progress_decision"] = "stop";
    rule["max_no_progress"] = 1;
    rule["max_iterations"] = 64;
    write(data / "service.json", c.dump());
    te::IntegrationService service(data / "service.json");
    Client client{service};
    client.run =
        client
            .invoke("run.begin", Json{{"input", "retry with changing arguments"},
                                      {"level", "L3"},
                                      {"conversation_id", "failure-progress"},
                                      {"capabilities", Json::Array{"model", "actions", "loop"}}})
            .str("run_id");
    int revision = 0;
    auto control = [&](const std::string &branch) {
      auto value = client.invoke(
          "loop.check", Json{{"loop_id", branch}, {"state", Json{{"revision", ++revision}}}});
      client.invoke("loop.commit",
                    Json{{"control_id", value["control_id"]}, {"decision", value["decision"]}});
      return value;
    };
    auto begin = [&](const Json &step, const std::string &operation) {
      return client.invoke("activity.begin", Json{{"kind", "action"},
                                                  {"name", operation},
                                                  {"input", Json{{"attempt", ++revision}}},
                                                  {"control_id", step["control_id"]}});
    };
    auto finish = [&](const Json &call, bool success) {
      client.invoke("activity.end",
                    Json{{"activity_id", call["activity_id"]},
                         {"status", "completed"},
                         {"executed", true},
                         {"output", Json{{"success", success}, {"request_id", ++revision}}}});
    };
    auto first = control("failed");
    finish(begin(first, "generate"), false);
    auto second = control("failed");
    check(second.str("decision") == "continue", "one failure is not a repeated failure");
    finish(begin(second, "generate"), false);
    auto pending = begin(second, "lookup");
    check(control("failed").str("decision") == "continue", "pending sibling cannot be interrupted");
    finish(pending, false);
    check(control("failed").str("decision") == (aggressive ? "stop" : "continue"),
          "changing parameters masked repeated failure");
    auto sibling = control("sibling");
    finish(begin(sibling, "generate"), false);
    check(control("sibling").str("decision") == "continue", "failure counters crossed executions");
    auto parallel = control("parallel-first-attempts");
    const auto left = begin(parallel, "generate"), right = begin(parallel, "generate");
    finish(left, false);
    finish(right, false);
    parallel = control("parallel-first-attempts");
    check(parallel.str("decision") == "continue",
          "two independent first failures became a repeated retry");
    finish(begin(parallel, "generate"), false);
    check(control("parallel-first-attempts").str("decision") == (aggressive ? "stop" : "continue"),
          "a later retry must still count after the parallel batch settles");
    auto mixed = control("mixed");
    finish(begin(mixed, "generate"), false);
    mixed = control("mixed");
    finish(begin(mixed, "generate"), false);
    finish(begin(mixed, "lookup"), true);
    mixed = control("mixed");
    check(mixed.str("decision") == "continue", "successful fallback must remain progress");
    finish(begin(mixed, "generate"), true);
    mixed = control("mixed");
    finish(begin(mixed, "generate"), false);
    check(control("mixed").str("decision") == "continue",
          "success must reset the operation failure streak");
    client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
    service.process_evaluations();
    const auto trace =
        read(data / "data/by-user/user-a/traces/failure-progress" / (client.run + ".trace.json"));
    check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
          "aggressive trace is invalid");
  }
}

// 测试辅助函数：schedule_outcome_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void schedule_outcome_tests(const fs::path &folder) {
  for (const bool aggressive : {false, true}) {
    const auto data = folder / (aggressive ? "aggressive" : "standard");
    auto c = config(data);
    c["runtime_options"]["aggressive_controls"] = aggressive;
    auto &rule = c["runtime_options"]["loop_rules"].items()[0];
    rule["no_progress_decision"] = "stop";
    rule["max_no_progress"] = 1;
    rule["max_iterations"] = 64;
    write(data / "service.json", c.dump());
    te::IntegrationService service(data / "service.json");
    Client client{service};
    client.run =
        client
            .invoke("run.begin", Json{{"input", "observe asynchronous results"},
                                      {"level", "L3"},
                                      {"conversation_id", "schedule-progress"},
                                      {"capabilities", Json::Array{"actions", "schedule", "loop"}}})
            .str("run_id");
    int revision = 0;
    auto control = [&](const std::string &branch) {
      auto value = client.invoke(
          "loop.check", Json{{"loop_id", branch}, {"state", Json{{"revision", ++revision}}}});
      client.invoke("loop.commit",
                    Json{{"control_id", value["control_id"]}, {"decision", value["decision"]}});
      return value;
    };
    auto batch = [&](const Json &step, bool parallel, int stable_input = -1) {
      auto wrapper = client.invoke("activity.begin", Json{{"kind", "action"},
                                                          {"name", "dispatch"},
                                                          {"input", Json{{"attempt", revision}}},
                                                          {"control_id", step["control_id"]}});
      Json actions = Json::Array{
          Json{{"action_id", "a"},
               {"operation", "generate"},
               {"arguments", Json{{"attempt", stable_input < 0 ? revision : stable_input}}}}};
      if (parallel)
        actions.push(Json{{"action_id", "b"},
                          {"operation", "lookup"},
                          {"arguments", Json{{"attempt", revision}}}});
      auto plan =
          client.invoke("activity.begin", Json{{"kind", "schedule"},
                                               {"name", "batch"},
                                               {"input", Json{{"actions", actions}}},
                                               {"control_id", step["control_id"]},
                                               {"caused_by", Json::Array{wrapper["event_id"]}}});
      client.invoke("activity.end",
                    Json{{"activity_id", wrapper["activity_id"]},
                         {"status", "completed"},
                         {"executed", true},
                         {"output", Json{{"accepted", true}, {"receipt", ++revision}}}});
      for (const auto &action : actions.items())
        client.invoke("schedule.observe", Json{{"activity_id", plan["activity_id"]},
                                               {"input", Json{{"action_id", action["action_id"]},
                                                              {"status", "running"}}}});
      return plan;
    };
    auto finish = [&](const Json &plan, const std::string &action, bool success) {
      return client.invoke("schedule.observe",
                           Json{{"activity_id", plan["activity_id"]},
                                {"input", Json{{"action_id", action},
                                               {"status", success ? "succeeded" : "failed"},
                                               {"output", Json{{"result", revision}}}}}});
    };
    auto close = [&](const Json &plan) {
      client.invoke("activity.end", Json{{"activity_id", plan["activity_id"]},
                                         {"status", "completed"},
                                         {"output", "settled"}});
    };
    auto first = batch(control("retries"), false);
    finish(first, "a", false);
    check(!finish(first, "a", false).flag("changed", true),
          "duplicate result must not be counted twice");
    close(first);
    auto next = control("retries");
    check(next.str("decision") == "continue", "one asynchronous failure is not a streak");
    auto second = batch(next, false);
    finish(second, "a", false);
    close(second);
    check(control("retries").str("decision") == (aggressive ? "stop" : "continue"),
          "dispatch ACK hid repeated asynchronous failure");
    check(control("independent").str("decision") == "continue",
          "asynchronous failure crossed loops");
    auto parallel_step = control("parallel-first-failures");
    const auto parallel_plan = client.invoke(
        "activity.begin",
        Json{{"kind", "schedule"},
             {"name", "same-operation-batch"},
             {"control_id", parallel_step["control_id"]},
             {"input",
              Json{{"actions", Json::Array{Json{{"action_id", "left"},
                                                {"operation", "generate"},
                                                {"arguments", Json{{"item", "left"}}}},
                                           Json{{"action_id", "right"},
                                                {"operation", "generate"},
                                                {"arguments", Json{{"item", "right"}}}}}}}}});
    for (const auto *action : {"left", "right"})
      client.invoke("schedule.observe",
                    Json{{"activity_id", parallel_plan["activity_id"]},
                         {"input", Json{{"action_id", action}, {"status", "running"}}}});
    finish(parallel_plan, "right", false);
    finish(parallel_plan, "left", false);
    close(parallel_plan);
    check(control("parallel-first-failures").str("decision") == "continue",
          "parallel child failures stopped the whole loop");
    for (const bool reverse : {false, true}) {
      const std::string branch = reverse ? "mixed-reverse" : "mixed";
      for (int i = 0; i < 2; ++i) {
        auto step = control(branch);
        check(step.str("decision") == "continue", "successful branch must remain progress");
        auto plan = batch(step, true);
        finish(plan, reverse ? "b" : "a", reverse);
        check(control(branch).str("decision") == "continue", "in-flight branch was interrupted");
        finish(plan, reverse ? "a" : "b", !reverse);
        close(plan);
      }
      check(control(branch).str("decision") == "continue",
            "completion order changed mixed-batch progress");
    }
    auto reset = batch(control("reset"), false);
    finish(reset, "a", false);
    close(reset);
    reset = batch(control("reset"), false);
    finish(reset, "a", true);
    close(reset);
    reset = batch(control("reset"), false);
    finish(reset, "a", false);
    close(reset);
    check(control("reset").str("decision") == "continue",
          "successful scheduled action did not clear its failure streak");
    auto asset = [&](const Json &plan, const Json &artifacts, const char *summary) {
      client.invoke("schedule.observe",
                    Json{{"activity_id", plan["activity_id"]},
                         {"input", Json{{"action_id", "a"},
                                        {"status", "succeeded"},
                                        {"output", Json{{"artifacts_json", artifacts.dump()},
                                                        {"summary", summary}}}}}});
      close(plan);
    };
    asset(batch(control("material-repeat"), false), Json::Array{Json{{"path", "first.png"}}},
          "created");
    check(control("material-repeat").str("decision") == "continue",
          "scheduled artifact was not progress");
    asset(batch(control("material-repeat"), false, 7), Json::Array{}, "verified image contrast");
    auto verified = control("material-repeat");
    check(verified.str("decision") == "continue",
          "scheduled verification without a file was mistaken for no progress");
    asset(batch(verified, false, 7), Json::Array{}, "verified image contrast");
    check(control("material-repeat").str("decision") == "stop",
          "identical scheduled results must still stop");
    asset(batch(control("material-new"), false), Json::Array{Json{{"path", "first.png"}}},
          "created");
    asset(batch(control("material-new"), false), Json::Array{Json{{"path", "second.png"}}},
          "created another");
    check(control("material-new").str("decision") == "continue",
          "new scheduled artifact was mistaken for repeated work");
    auto transport = control("dispatch-error");
    auto wrapper = client.invoke("activity.begin", Json{{"kind", "action"},
                                                        {"name", "dispatch"},
                                                        {"input", Json::Object{}},
                                                        {"control_id", transport["control_id"]}});
    auto uncertain = client.invoke(
        "activity.begin",
        Json{{"kind", "schedule"},
             {"name", "batch"},
             {"input",
              Json{{"actions", Json::Array{Json{{"action_id", "a"}, {"operation", "generate"}}}}}},
             {"control_id", transport["control_id"]},
             {"caused_by", Json::Array{wrapper["event_id"]}}});
    for (const auto *status : {"running", "unknown"})
      client.invoke(
          "schedule.observe",
          Json{{"activity_id", uncertain["activity_id"]},
               {"input", Json{{"action_id", "a"}, {"status", status}, {"output", nullptr}}}});
    close(uncertain);
    client.invoke("activity.end", Json{{"activity_id", wrapper["activity_id"]},
                                       {"status", "failed"},
                                       {"executed", true},
                                       {"output", "dispatch transport failed"}});
    check(control("dispatch-error").str("decision") == "continue",
          "unknown child became a repeated business failure");
    client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
    service.process_evaluations();
    const auto trace =
        read(data / "data/by-user/user-a/traces/schedule-progress" / (client.run + ".trace.json"));
    check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
          "schedule outcome trace is invalid");
    bool transport_seen = false;
    for (const auto &event : trace["events"].items()) {
      const auto &query = event["attributes"]["decision"]["query"];
      if (query.str("boundary") == "loop" && query.str("execution_id") == "dispatch-error" &&
          query.str("iteration") == "2")
        transport_seen =
            query.num("failed_actions", 0) == 1 && query.num("repeated_failure_steps", 0) == 0;
    }
    check(transport_seen,
          "delegation hid the dispatch transport failure or counted its unknown child");
  }
}

// 测试辅助函数：failure_replan_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void failure_replan_tests(const fs::path &folder) {
  auto c = config(folder);
  auto &rule = c["runtime_options"]["loop_rules"].items()[0];
  rule["max_no_progress"] = 3;
  rule["replan_on_failure"] = true;
  write(folder / "service.json", c.dump());
  te::IntegrationService service(folder / "service.json");
  Client client{service};
  client.run =
      client
          .invoke("run.begin", Json{{"input", "failure recovery"},
                                    {"level", "L3"},
                                    {"capabilities", Json::Array{"model", "actions", "loop"}}})
          .str("run_id");
  auto check_loop = [&](const char *id, int state) {
    return client.invoke("loop.check", Json{{"loop_id", id}, {"state", state}});
  };
  auto commit = [&](const Json &control) {
    client.invoke("loop.commit",
                  Json{{"control_id", control["control_id"]}, {"decision", control["decision"]}});
  };
  auto first = check_loop("work", 1);
  commit(first);
  auto action = client.invoke("activity.begin", Json{{"kind", "action"},
                                                     {"name", "lookup"},
                                                     {"input", Json::Object{}},
                                                     {"control_id", first["control_id"]}});
  auto pending = client.invoke("activity.begin", Json{{"kind", "action"},
                                                      {"name", "lookup"},
                                                      {"input", Json::Object{}},
                                                      {"control_id", first["control_id"]}});
  client.invoke("activity.end",
                Json{{"activity_id", action["activity_id"]},
                     {"status", "completed"},
                     {"executed", true},
                     {"output", "{\"status\":\"failed\",\"request_id\":\"changing\"}"}});
  auto waiting = check_loop("work", 2);
  check(waiting.str("decision") == "continue", "failure must wait for pending sibling");
  commit(waiting);
  client.invoke("activity.end", Json{{"activity_id", pending["activity_id"]},
                                     {"status", "completed"},
                                     {"executed", true},
                                     {"output", Json{{"success", true}}}});
  auto independent = check_loop("other", 1);
  check(independent.str("decision") == "continue", "failure crossed execution boundary");
  commit(independent);
  auto recovery = check_loop("work", 3);
  check(recovery.str("decision") == "replan",
        "failure opportunity was lost while sibling was pending");
  commit(recovery);
  auto model = client.invoke("activity.begin",
                             Json{{"kind", "model"},
                                  {"name", "recover"},
                                  {"input", Json{{"system", "continue task"}}},
                                  {"binding", Json{{"pointer", "/system"}, {"mode", "text"}}},
                                  {"control_id", recovery["control_id"]}});
  check(model["input"].str("system").find(recovery.str("context")) != std::string::npos,
        "replan did not reach model");
  client.invoke("activity.end", Json{{"activity_id", model["activity_id"]},
                                     {"status", "completed"},
                                     {"executed", true},
                                     {"actual_input", model["input"]},
                                     {"output", "recovered"}});
  auto next = check_loop("work", 4);
  check(next.str("decision") == "continue", "already consumed failure triggered again");
  commit(next);
  client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});

  client.run =
      client
          .invoke("run.begin", Json{{"input", "host declines recovery"},
                                    {"level", "L3"},
                                    {"conversation_id", "declined-failure-replan"},
                                    {"capabilities", Json::Array{"model", "actions", "loop"}}})
          .str("run_id");
  auto fail = [&](const Json &control, int attempt) {
    auto call = client.invoke("activity.begin", Json{{"kind", "action"},
                                                     {"name", "lookup"},
                                                     {"input", Json{{"attempt", attempt}}},
                                                     {"control_id", control["control_id"]}});
    client.invoke("activity.end", Json{{"activity_id", call["activity_id"]},
                                       {"status", "completed"},
                                       {"executed", true},
                                       {"output", Json{{"success", false}, {"attempt", attempt}}}});
  };
  first = check_loop("declined", 1);
  commit(first);
  fail(first, 1);
  auto declined = check_loop("declined", 2);
  check(declined.str("decision") == "replan", "first failure should offer recovery");
  client.invoke("loop.commit",
                Json{{"control_id", declined["control_id"]}, {"decision", "continue"}});
  fail(declined, 2);
  declined = check_loop("declined", 3);
  check(declined.str("decision") == "replan", "repeated failure should offer recovery");
  client.invoke("loop.commit",
                Json{{"control_id", declined["control_id"]}, {"decision", "continue"}});
  auto unchanged = check_loop("declined", 3);
  check(unchanged.str("decision") == "continue",
        "one unchanged state is below the ordinary threshold");
  commit(unchanged);
  unchanged = check_loop("declined", 3);
  check(unchanged.str("decision") == "continue",
        "declined failure must not inflate ordinary no-progress");
  commit(unchanged);
  client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
}

// 测试辅助函数：boundary_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void boundary_tests(const fs::path &folder) {
  auto c = config(folder);
  c["application_id"] = c["agent_id"];
  c.fields().erase("agent_id");
  c["policy_scopes"] = c["actors"];
  c.fields().erase("actors");
  c["activity_types"] =
      Json{{"vendor.generate",
            Json{{"semantic", "model"}, {"required_input_paths", Json::Array{"/messages"}}}},
           {"warehouse.lookup",
            Json{{"semantic", "action"}, {"required_input_paths", Json::Array{"/query"}}}}};
  c["instrumentation"]["action_outcomes"] = Json::Array{
      Json{{"pointer", "/status"},
           {"equals", "blocked"},
           {"status", "failed"},
           {"error", Json{{"code", "policy_blocked"}, {"retryability", "non_retryable"}}}},
      Json{{"pointer", "/status"}, {"equals", "failed"}, {"status", "failed"}},
      Json{{"pointer", "/error"},
           {"nonempty", true},
           {"status", "failed"},
           {"error", Json{{"code", "tool_error"}, {"retryability", "unknown"}}}}};
  validate_instrumentation(c);
  const Json observation{
      {"request_id", "mapped-outcome"},
      {"phase", "after"},
      {"kind", "action"},
      {"executed", true},
      {"context", Json{{"run_id", "mapping-fixture"}, {"activity_id", "activity-fixture"}}}};
  for (const auto &output : std::vector<Json>{Json{{"status", "failed"}}, "{\"status\":\"failed\"}",
                                              Json{{"error", "failed to read"}},
                                              Json{{"error", Json{{"code", "missing"}}}}}) {
    auto request = observation;
    request["output"] = output;
    const auto packet = point_request(request, c);
    check(packet.str("status") == "failed" && packet["output"].dump() == output.dump(),
          "mapped failure preserves original result");
    check(point_request(request, Json::Object{}).str("status") == "completed",
          "unconfigured result mapping stays compatible");
    request["status"] = "cancelled";
    check(point_request(request, c).str("status") == "cancelled",
          "explicit host status takes priority");
    request.fields().erase("status");
    request["executed"] = false;
    check(point_request(request, c).str("status") == "cancelled",
          "unexecuted operation is not a business failure");
    request["executed"] = true;
    request["phase"] = "abort";
    check(point_request(request, c).str("status") == "unknown",
          "aborted capture is not classified from partial output");
  }
  for (const auto &empty :
       std::vector<Json>{nullptr, false, 0, "", Json::Array{}, Json::Object{}}) {
    auto request = observation;
    request["output"] = Json{{"error", empty}};
    check(point_request(request, c).str("status") == "completed",
          "empty error must not invent failure");
  }
  for (const auto &output :
       std::vector<Json>{"plain text", "{invalid", Json{{"status", "succeeded"}}, Json::Array{}}) {
    auto request = observation;
    request["output"] = output;
    check(point_request(request, c).str("status") == "completed",
          "unmatched result keeps original outcome");
  }
  for (const auto *kind : {"model", "custom", "run"}) {
    auto request = observation;
    request["kind"] = kind;
    request["actual_input"] = "original";
    request["output"] = Json{{"status", "failed"}};
    check(point_request(request, c).str("status") == "completed",
          "action mapping never changes models or whole run status");
  }
  c["instrumentation"]["model_outcomes"] =
      Json::Array{Json{{"pointer", "/error"},
                       {"nonempty", true},
                       {"status", "failed"},
                       {"error", Json{{"code", "model_error"}, {"retryability", "unknown"}}}}};
  validate_instrumentation(c);
  for (const auto *kind : {"model", "vendor.generate"}) {
    auto request = observation;
    request["kind"] = kind;
    request["actual_input"] = "original";
    request["output"] = Json{{"error", "provider timeout"}};
    auto packet = point_request(request, c);
    check(packet.str("status") == "failed" && packet["error"].str("code") == "model_error" &&
              packet["output"].dump() == request["output"].dump() && !packet.has("metrics"),
          "mapped model error preserves output and unknown usage");
    request["output"] = "{\"error\":\"provider timeout\"}";
    check(point_request(request, c).str("status") == "failed",
          "model output accepts one JSON string layer");
    request["status"] = "unknown";
    check(point_request(request, c).str("status") == "unknown", "host model status wins");
    request.fields().erase("status");
    request["error"] = Json{{"code", "host_error"}, {"retryability", "retryable"}};
    check(point_request(request, c)["error"].str("code") == "host_error", "host model error wins");
    request.fields().erase("error");
    request["executed"] = false;
    check(point_request(request, c).str("status") == "cancelled",
          "unexecuted model stays cancelled");
    request["executed"] = true;
    request["phase"] = "abort";
    check(point_request(request, c).str("status") == "unknown",
          "aborted model capture stays unknown");
    request["phase"] = "after";
    request.fields().erase("actual_input");
    check(point_request(request, c).str("operation") == "activity.abort",
          "mapping does not repair missing execution evidence");
    request["actual_input"] = "original";
    for (const auto &output :
         std::vector<Json>{nullptr, "plain text", "{invalid", Json{{"error", ""}},
                           Json{{"choices", Json::Array{}}}}) {
      request["output"] = output;
      check(point_request(request, c).str("status") == "completed",
            "model mapping requires positive match");
    }
  }
  for (const auto &rule :
       std::vector<Json>{Json{{"pointer", "bad"}, {"equals", "failed"}, {"status", "failed"}},
                         Json{{"pointer", "/status"},
                              {"equals", "failed"},
                              {"nonempty", true},
                              {"status", "failed"}},
                         Json{{"pointer", "/error"}, {"nonempty", false}, {"status", "failed"}},
                         Json{{"pointer", "/error"},
                              {"nonempty", true},
                              {"status", "completed"},
                              {"error", Json{{"code", "error"}, {"retryability", "unknown"}}}}}) {
    for (const auto *mapping : {"action_outcomes", "model_outcomes"}) {
      auto invalid = c;
      invalid["instrumentation"][mapping] = Json::Array{rule};
      bool rejected = false;
      try {
        validate_instrumentation(invalid);
      } catch (const Error &) {
        rejected = true;
      }
      check(rejected, "invalid outcome mapping rejected at configuration load");
    }
  }
  write(folder / "service.json", c.dump());
  seed(folder / "data");
  std::string resumed_run;
  Json reserved;
  {
    te::IntegrationService service(folder / "service.json");
    Client client{service};
    check(client.invoke("contract.describe").str("protocol") == "evolution.boundary.v1",
          "discover neutral contract");
    auto start = [&](const std::string &level, const std::string &conv) {
      const auto result =
          client.invoke("run.begin", Json{{"input", "视频生成缺少素材"},
                                          {"conversation_id", conv},
                                          {"level", level},
                                          {"capabilities", Json::Array{"model", "actions", "loop"}},
                                          {"extensions", Json{{"domain", "testing"}}}});
      client.run = result.str("run_id");
    };
    auto begin = [&](const std::string &kind, const std::string &name, Json input,
                     Json extra = Json::Object{}) {
      extra["kind"] = kind;
      extra["name"] = name;
      extra["input"] = input;
      return client.invoke("activity.begin", extra);
    };
    auto finish = [&](const Json &activity, const std::string &status = "completed",
                      bool executed = true) {
      return client.invoke("activity.end", Json{{"activity_id", activity["activity_id"]},
                                                {"status", status},
                                                {"output", "真实结果"},
                                                {"executed", executed},
                                                {"actual_input", activity["input"]}});
    };
    auto close = [&]() {
      return client.invoke(
          "run.end",
          Json{{"status", "completed"}, {"business_status", "partial"}, {"output", "汇总"}});
    };
    auto trace = [&](const std::string &conv) {
      service.process_evaluations();
      const auto value =
          read(folder / "data/by-user/user-a/traces" / conv / (client.run + ".trace.json"));
      const auto validation = te::validate_trace(proto<v1::TraceRun>(value));
      check(validation.report.trace_valid(),
            "boundary trace valid: " + json(validation.report).dump());
      return value;
    };
    client.invoke("run.begin", Json{{"input", "x"}, {"contract_version", 2}}, 400);
    start("L0", "neutral-flat");
    auto a =
        begin("model", "vendor-alpha", Json::Array{"a", "b"}, Json{{"actor_id", "unregistered-a"}});
    auto b = begin("model", "vendor-beta", "a bare prompt", Json{{"actor_id", "unregistered-b"}});
    auto d = begin("action", "lookup", Json{{"q", "network"}});
    auto e = begin("planning.private", "planner", Json::Array{1, 2, 3});
    client.invoke("run.end", Json{{"status", "completed"}, {"output", "too early"}}, 409);
    const auto e_end = finish(e), b_end = finish(b, "failed"), d_end = finish(d);
    check(!client.invoke("run.status").flag("closed", false),
          "one failed branch must not end siblings");
    finish(a);
    auto joined = begin(
        "merge", "fan-in", "join",
        Json{{"caused_by", Json::Array{e_end["event_id"], b_end["event_id"], d_end["event_id"]}},
             {"extensions", Json{{"business", Json{{"visible", "保留具体自定义内容"}}}}}});
    client.invoke("event.record", Json{{"type", "state.transition"},
                                       {"data", Json{{"before", "waiting"}, {"after", "merged"}}},
                                       {"activity_id", joined["activity_id"]},
                                       {"extensions", Json{{"own_field", 42}}}});
    finish(joined);
    close();
    auto captured = trace("neutral-flat");
    check(captured.dump().find("保留具体自定义内容") != std::string::npos, "extensions preserved");
    check(te::trace_content_jsonl(proto<v1::TraceRun>(captured)).find("保留具体自定义内容") !=
              std::string::npos,
          "extensions reach model evidence input");
    for (const auto &event : captured["events"].items())
      check(!event.has("loop_id"), "activity must not imply a loop iteration");
    check(client.invoke("run.status").str("evaluation_status") == "completed",
          "neutral data reaches evaluation and memory");

    {
      start("L0", "mapped-failure-isolation");
      auto failed = begin("action", "read", Json{{"path", "missing.txt"}});
      auto sibling = begin("action", "lookup", Json{{"q", "independent"}});
      auto observed = observation;
      observed["context"] = Json{{"run_id", client.run}, {"activity_id", failed["activity_id"]}};
      observed["output"] = "{\"status\":\"blocked\",\"error\":\"fixture restriction\"}";
      client.invoke("activity.end", point_request(observed, c));
      check(!client.invoke("run.status").flag("closed", false),
            "mapped failure does not end concurrent sibling");
      finish(sibling);
      close();
      captured = trace("mapped-failure-isolation");
      bool mapped_failure = false, healthy_sibling = false;
      for (const auto &event : captured["events"].items()) {
        if (event.str("kind") != "action.end")
          continue;
        mapped_failure |= event.str("status") == "failed" &&
                          event["error"].str("code") == "policy_blocked" &&
                          event["error"].str("retryability") == "non_retryable" &&
                          event["output"].dump() == observed["output"].dump();
        healthy_sibling |= event.str("status") == "completed";
      }
      check(mapped_failure && healthy_sibling &&
                captured["events"].items().back()["outcome"].str("execution_status") == "completed",
            "configured failure is captured without changing run execution status");
    }

    {
      start("L0", "mapped-model-failure-isolation");
      auto failed = begin("model", "provider", "question");
      auto sibling = begin("action", "lookup", Json{{"q", "independent"}});
      auto observed = observation;
      observed["kind"] = "model";
      observed["actual_input"] = "question";
      observed["context"] = Json{{"run_id", client.run}, {"activity_id", failed["activity_id"]}};
      observed["output"] = Json{{"error", "provider timeout"}};
      client.invoke("activity.end", point_request(observed, c));
      check(!client.invoke("run.status").flag("closed", false),
            "mapped model failure leaves sibling running");
      finish(sibling);
      close();
      captured = trace("mapped-model-failure-isolation");
      bool mapped_failure = false;
      for (const auto &event : captured["events"].items())
        mapped_failure |= event.str("kind") == "action.end" && event.str("status") == "failed" &&
                          event["error"].str("code") == "model_error" &&
                          event["output"].dump() == observed["output"].dump();
      check(mapped_failure &&
                te::validate_trace(proto<v1::TraceRun>(captured)).report.trace_valid() &&
                captured["events"].items().back()["outcome"].str("execution_status") == "completed",
            "model failure is captured without changing workflow completion");
    }

    start("L1", "neutral-bindings");
    auto messages =
        begin("vendor.generate", "chat",
              Json{{"messages", Json::Array{Json{{"role", "user"}, {"content", "素材"}}}}},
              Json{{"binding", Json{{"pointer", "/messages"}, {"mode", "system_message"}}},
                   {"actor_id", "dynamic-worker-17"}});
    auto text =
        begin("model", "different-api", Json{{"native", Json{{"instructions", "原有指令"}}}},
              Json{{"binding", Json{{"pointer", "/native/instructions"}, {"mode", "text"}}}});
    auto bare = begin("model", "string-api", "原有指令",
                      Json{{"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    check(messages["input"].dump().find("先补充图片") != std::string::npos &&
              text["input"].dump().find("先补充图片") != std::string::npos &&
              bare["input"].text().find("先补充图片") != std::string::npos,
          "per-call binding injects at arbitrary JSON location");
    finish(text);
    finish(messages);
    finish(bare);
    auto tampered = begin("model", "string-api", "original",
                          Json{{"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    tampered["input"] = "host discarded injection";
    finish(tampered);
    auto receipt_status = client.invoke("run.status");
    check(receipt_status["receipts"].num("applied", 0) == 3 &&
              receipt_status["receipts"].num("failed", 0) == 1,
          "verify actual native request without altering policy hash");
    client.invoke("activity.begin",
                  Json{{"kind", "vendor.generate"}, {"name", "chat"}, {"input", Json::Object{}}},
                  400);
    client.invoke(
        "activity.begin",
        Json{{"kind", "model"}, {"name", "chat"}, {"input", Json{{"unmapped", Json::Array{1}}}}},
        400);
    close();
    captured = trace("neutral-bindings");
    auto report = te::analyze_trace(te::validate_trace(proto<v1::TraceRun>(captured)));
    check(!report.episodes.empty() && report.episodes[0].actor_id == "my-agent/actor:assistant",
          "dynamic actor metadata does not change memory scope");

    // 多轮聊天保留完整原始输入；检索不能把上一轮助手的事实借给本轮。
    for (const bool serialized : {false, true}) {
      const std::string conversation = serialized ? "current-task-string" : "current-task-object";
      const Json messages =
          Json::Array{Json{{"role", "user"}, {"content", "历史请求"}},
                      Json{{"role", "assistant"}, {"content", "HISTORY_ONLY 视频生成缺少素材"}},
                      Json{{"role", "user"}, {"content", "CURRENT_ONLY 查询库存"}}};
      const Json envelope{{"messages", messages}, {"stream", false}};
      const Json raw = serialized ? Json(envelope.dump()) : envelope;
      const auto opened = client.invoke("run.begin", Json{{"input", raw},
                                                          {"level", "L1"},
                                                          {"capabilities", Json::Array{"model"}},
                                                          {"conversation_id", conversation}});
      client.run = opened.str("run_id");
      const Json binding{{"pointer", "/messages"}, {"mode", "system_message"}};
      const Json original{{"messages", messages}};
      const auto model = begin("model", "chat", original, Json{{"binding", binding}});
      check(model["input"].dump() == original.dump(),
            "historical task text triggered advice for a fresh request");
      finish(model);
      const auto observed = begin("model", "chat", original,
                                  Json{{"binding", binding}, {"state", "视频生成缺少素材"}});
      check(observed["input"].dump().find("先补充图片") != std::string::npos,
            "current state lost its independent retrieval evidence");
      finish(observed);
      close();
      captured = trace(conversation);
      check(captured["events"].items().front()["input"].dump() == raw.dump(),
            "current task projection changed the complete recorded run input");
      bool original_preserved = false, fresh_state = false;
      unsigned queries = 0;
      for (const auto &event : captured["events"].items()) {
        if (event.str("kind") == "step.start" &&
            event.str("step_id") == "step-" + model.str("activity_id"))
          original_preserved = event["input"].dump() == original.dump();
        if (event["attributes"].str("type") != "policy.query")
          continue;
        const auto &query = event["attributes"]["decision"]["query"]["advice_query"];
        ++queries;
        check(query.str("task").find("CURRENT_ONLY") != std::string::npos &&
                  query.str("task").find("HISTORY_ONLY") == std::string::npos,
              "boundary query.task kept previous-turn history or lost the current user input");
        if (event.str("step_id") == "step-" + observed.str("activity_id"))
          fresh_state = query.str("state").find("视频生成缺少素材") != std::string::npos;
      }
      check(queries == 2 && original_preserved && fresh_state,
            "current task filtering changed trace provenance or the current observation");
    }

    {
      const auto opened =
          client.invoke("run.begin", Json{{"input", "库存查询"},
                                          {"level", "L1"},
                                          {"capabilities", Json::Array{"model", "actions"}},
                                          {"conversation_id", "current-request-state"}});
      client.run = opened.str("run_id");
      const Json binding{{"pointer", "/body/m~1n~0/0/messages"}, {"mode", "system_message"}};
      const Json instructions = Json::Array{
          Json{{"role", "system"},
               {"content", std::string(20000, 's') + "STATIC_ONLY 视频生成缺少素材"}},
          Json{{"role", "developer"}, {"content", "DEVELOPER_ONLY 视频生成缺少素材"}},
          Json{{"role", "user"},
               {"content", "<system-reminder>REMINDER_ONLY 视频生成缺少素材</system-reminder>"}},
          Json{{"role", 7}, {"content", "普通说明"}},
          Json{{"role", "user"}, {"content", "库存查询"}}};
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
      // 第一个活动尚未关闭时，未失败兄弟分支也不应继承其失败。
      auto sibling = begin("model", "chat", payload(instructions), Json{{"binding", binding}});
      check(sibling["input"].dump() == payload(instructions).dump(),
            "system/developer/schema text and a sibling state must not trigger advice");
      finish(sibling);
      finish(prepared);
      for (const auto &state : std::vector<Json>{"", Json::Object{}, false}) {
        auto explicit_state =
            begin("model", "chat", original, Json{{"binding", binding}, {"state", state}});
        check(explicit_state["input"].dump() == original.dump(),
              "explicit empty state remains authoritative");
        finish(explicit_state);
      }
      auto implicit_state =
          begin("model", "chat", original, Json{{"binding", binding}, {"state", nullptr}});
      check(implicit_state["input"].dump().find("先补充图片") != std::string::npos,
            "null state uses the current request");
      finish(implicit_state);
      auto observed_only =
          begin("model", "chat", original, Json{{"binding", binding}, {"apply_policy", false}});
      check(observed_only["input"].dump() == original.dump(),
            "observation-only input remains untouched");
      finish(observed_only);
      auto missing = payload(Json::Array{});
      missing["body"]["m/n~"].items()[0].fields().erase("messages");
      auto absent_messages = begin("model", "chat", missing, Json{{"binding", binding}});
      check(absent_messages["input"].dump() == missing.dump(),
            "missing messages do not select advice from tool schemas");
      finish(absent_messages);
      auto alias = begin(
          "vendor.generate", "chat",
          Json{{"messages", Json::Array{Json{{"role", "tool"}, {"content", "视频生成缺少素材"}}}}},
          Json{{"binding", Json{{"pointer", "/messages"}, {"mode", "system_message"}}}});
      check(alias["input"].dump().find("先补充图片") != std::string::npos,
            "configured model aliases use the same default state");
      finish(alias);
      auto explicit_match = begin("model", "chat", payload(instructions),
                                  Json{{"binding", binding}, {"state", "视频生成缺少素材"}});
      check(explicit_match["input"].dump().find("先补充图片") != std::string::npos,
            "explicit state overrides the neutral request");
      finish(explicit_match);
      auto native = begin("model", "text-api", "视频生成缺少素材",
                          Json{{"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
      check(native["input"].text().find("先补充图片") != std::string::npos,
            "bare text request supplies retrieval context");
      finish(native);
      auto action = begin("action", "lookup", Json{{"current", "available"}});
      finish(action);
      close();
      captured = trace("current-request-state");
      unsigned queries = 0;
      bool original_preserved = false, action_state = false;
      for (const auto &event : captured["events"].items()) {
        if (event.str("kind") == "step.start" &&
            event.str("step_id") == "step-" + prepared.str("activity_id"))
          original_preserved = event["input"].dump() == original.dump();
        if (event["attributes"].str("type") != "policy.query")
          continue;
        const auto &query = event["attributes"]["decision"]["query"];
        const auto state = query["advice_query"].str("state");
        ++queries;
        if (event.str("step_id") == "step-" + prepared.str("activity_id")) {
          check(state.size() <= 8192 && state.find("视频生成缺少素材") != std::string::npos,
                "bounded retrieval keeps the latest tool result");
          check(state.find("STATIC_ONLY") == std::string::npos &&
                    state.find("DEVELOPER_ONLY") == std::string::npos &&
                    state.find("SCHEMA_ONLY") == std::string::npos &&
                    state.find("REMINDER_ONLY") == std::string::npos,
                "retrieval excludes static instructions, reminders and schemas");
        }
        if (query.str("boundary") == "actions")
          action_state = state.find("available") != std::string::npos;
      }
      check(queries == 11 && original_preserved && action_state,
            "default state retains trace and per-call query semantics");
      check(client.invoke("run.status")["receipts"].num("failed", 0) == 0,
            "retrieval default preserves injection receipts");
    }

    {
      const auto opened =
          client.invoke("run.begin", Json{{"input", "库存查询"},
                                          {"level", "L1"},
                                          {"capabilities", Json::Array{"model"}},
                                          {"conversation_id", "current-observation-window"}});
      client.run = opened.str("run_id");
      const Json binding{{"pointer", "/messages"}, {"mode", "system_message"}};
      Json history = Json::Array{Json{{"role", "user"}, {"content", "库存查询"}},
                                 Json{{"role", "assistant"}, {"content", "检查请求"}},
                                 Json{{"role", "tool"}, {"content", "视频生成缺少素材"}}};
      auto ask = [&](const Json &messages, bool expected) {
        const Json original{{"messages", messages}};
        auto model = begin("model", "chat", original, Json{{"binding", binding}});
        check((model["input"].dump().find("先补充图片") != std::string::npos) == expected,
              "advice must follow current observations, not consumed history");
        if (!expected)
          check(model["input"].dump() == original.dump(),
                "history must remain intact for the model");
        finish(model);
        return model;
      };
      ask(history, true);
      const Json reminder{
          {"role", "user"},
          {"content", "<system-reminder>REMINDER_ONLY 视频生成缺少素材</system-reminder>"}};
      Json current = Json::Array{
          Json{{"role", "user"}, {"content", "库存查询"}},
          Json{{"role", "assistant"},
               {"content", nullptr},
               {"tool_calls", Json::Array{Json{{"id", "widget-call"},
                                               {"type", "function"},
                                               {"function", Json{{"name", "render_widget"},
                                                                 {"arguments", "{}"}}}}}}},
          Json{{"role", "tool"},
               {"tool_call_id", "widget-call"},
               {"content", Json{{"status", "failed"}, {"error", "视频生成缺少素材"}}.dump()}},
          reminder};
      const auto after_reminder = ask(current, true);
      auto mixed_user = current;
      mixed_user.items().back()["content"] =
          "新任务：计算价格 <system-reminder>REMINDER_ONLY 视频生成缺少素材</system-reminder>";
      ask(mixed_user, false);
      auto next_action = current;
      next_action.push(Json{
          {"role", "assistant"},
          {"content", nullptr},
          {"tool_calls",
           Json::Array{Json{{"id", "stock-call"},
                            {"type", "function"},
                            {"function", Json{{"name", "lookup_stock"}, {"arguments", "{}"}}}}}}});
      next_action.push(Json{{"role", "tool"},
                            {"tool_call_id", "stock-call"},
                            {"content", Json{{"status", "succeeded"}, {"count", 20}}.dump()}});
      next_action.push(reminder);
      ask(next_action, false);
      ask(
          Json::Array{
              Json{{"role", "system"}, {"content", "render_widget failed 视频生成缺少素材"}},
              Json{{"role", "developer"}, {"content", "render_widget failed 视频生成缺少素材"}},
              reminder},
          false);
      current.push(Json{{"role", "assistant"}, {"content", nullptr}});
      ask(current, true);
      history.push(Json{{"role", "assistant"}, {"content", "转为查询库存"}});
      history.push(Json{{"role", "tool"}, {"content", "库存查询成功，20 件"}});
      ask(history, false);
      history.push(Json{{"role", "user"}, {"content", "新任务：计算价格"}});
      ask(history, false);
      // 保留最近并行批次的全部结果，不只保留最后返回的一个。
      // 真正再次出现的失败仍可再次触发控制。
      history.push(Json{{"role", "assistant"}, {"content", "同时查询库存和视频"}});
      history.push(Json{{"role", "tool"}, {"content", "视频生成缺少素材"}});
      history.push(Json{{"role", "tool"}, {"content", "库存查询成功"}});
      ask(history, true);
      close();
      captured = trace("current-observation-window");
      bool complete_history = false, reminder_observation = false, reminder_applied = false;
      for (const auto &event : captured["events"].items()) {
        if (event.str("kind") == "step.start" && event["input"]["messages"].array())
          complete_history |= event["input"]["messages"].dump() == history.dump();
        if (event.str("step_id") != "step-" + after_reminder.str("activity_id"))
          continue;
        if (event["attributes"].str("type") == "policy.query") {
          const auto state = event["attributes"]["decision"]["query"]["advice_query"].str("state");
          reminder_observation = state.find("render_widget") != std::string::npos &&
                                 state.find("failed") != std::string::npos &&
                                 state.find("视频生成缺少素材") != std::string::npos &&
                                 state.find("REMINDER_ONLY") == std::string::npos;
        }
        if (event.str("kind") == "advice.applied")
          reminder_applied |= event["application"].str("status") == "applied";
      }
      check(complete_history, "retrieval window must not truncate the recorded request");
      check(reminder_observation && reminder_applied,
            "reminder-only user messages preserve the current operation/result and its applied "
            "advice");
    }

    {
      start("L1", "observed-operation-isolation");
      const Json a{{"stream_id", "branch-a"}}, b{{"stream_id", "branch-b"}};
      auto first = begin("action", "generate_image", Json::Object{}, a);
      finish(first);
      auto rejected = begin("action", "not_executed", Json::Object{}, a);
      finish(rejected, "cancelled", false);
      auto sibling = begin("model", "chat", Json{{"system", "network timeout"}}, b);
      finish(sibling);
      auto consumer = begin("model", "chat", Json{{"system", "network timeout"}}, a);
      auto concurrent = begin("action", "read_file", Json::Object{}, a);
      finish(concurrent);
      finish(consumer);
      auto next = begin("model", "chat", Json{{"system", "network timeout"}}, a);
      finish(next);
      auto consumed = begin("model", "chat", Json{{"system", "network timeout"}}, a);
      finish(consumed);
      close();
      captured = trace("observed-operation-isolation");
      std::map<std::string, std::set<std::string>> seen;
      for (const auto &event : captured["events"].items()) {
        const auto &query = event["attributes"]["decision"]["query"];
        if (query.str("boundary") != "llm")
          continue;
        auto &observed = seen[event.str("step_id")];
        if (query["observed_operations"].array())
          for (const auto &operation : query["observed_operations"].items())
            observed.insert(operation.text());
      }
      check(seen["step-" + sibling.str("activity_id")].empty(),
            "sibling did not inherit operation evidence");
      check(seen["step-" + consumer.str("activity_id")] == std::set<std::string>{"generate_image"},
            "only executed tools count");
      check(seen["step-" + next.str("activity_id")] == std::set<std::string>{"read_file"},
            "concurrent completion survives consumption");
      check(seen["step-" + consumed.str("activity_id")].empty(),
            "old operations cannot trigger every later request");
    }

    start("L3", "neutral-loop");
    auto loop = [&](const std::string &id = "react-main") {
      return client.invoke("loop.check", Json{{"loop_id", id}, {"state", "unchanged"}});
    };
    auto commit = [&](const Json &control) {
      client.invoke("loop.commit",
                    Json{{"control_id", control["control_id"]}, {"decision", control["decision"]}});
    };
    auto first = loop();
    check(first.str("decision") == "continue", "first explicit loop boundary");
    commit(first);
    auto slow =
        begin("action", "lookup", Json{{"q", "slow"}}, Json{{"control_id", first["control_id"]}});
    auto pending = loop();
    check(pending.str("decision") == "continue", "do not stop in-flight work in this loop");
    commit(pending);
    auto independent = loop("other-loop");
    check(independent.num("iteration", 0) == 1, "loop counters independent");
    commit(independent);
    finish(slow);
    auto second = loop();
    check(second.str("decision") == "replan", "explicit no-progress replan");
    commit(second);
    auto model = begin("model", "chat", "instructions",
                       Json{{"control_id", second["control_id"]},
                            {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    check(model["input"].text().find("停止重复") != std::string::npos,
          "replan supplied to actual next model request");
    finish(model);
    auto stop = loop();
    check(stop.str("decision") == "stop", "loop budget independent of number of activities");
    commit(stop);
    client.invoke("activity.begin",
                  Json{{"kind", "action"},
                       {"name", "late"},
                       {"input", nullptr},
                       {"control_id", stop["control_id"]}},
                  409);
    client.invoke("activity.begin",
                  Json{{"kind", "action"},
                       {"name", "stale-control"},
                       {"input", nullptr},
                       {"control_id", first["control_id"]}},
                  409);
    auto blocked = begin("action", "video", Json::Object{}),
         allowed = begin("action", "lookup", Json::Object{});
    check(blocked.str("decision") == "reject" && allowed.str("decision") == "allow",
          "L2 rejects individual action only");
    finish(allowed);
    finish(blocked, "cancelled", false);
    close();
    trace("neutral-loop");
    check(client.invoke("run.status")["receipts"].num("failed", 0) == 0,
          "native model/action/loop receipt consistent");

    {
      start("L0", "neutral-event-loop");
      const auto left = loop("left"), right = loop("right");
      commit(left);
      commit(right);
      const auto action =
          begin("action", "lookup", nullptr, Json{{"control_id", right["control_id"]}});
      Json event{{"request_id", "point-event-context"},
                 {"phase", "event"},
                 {"kind", "custom"},
                 {"name", "dispatch.result"},
                 {"input", Json{{"status", "invalid_arguments"}, {"error", "empty dispatch"}}},
                 {"context", Json{{"run_id", client.run}, {"control_id", left["control_id"]}}}};
      auto packet = point_request(event, c);
      check(packet["control_id"].dump() == left["control_id"].dump(),
            "event point retains explicit loop context");
      packet["request_id"] = "same-loop-event";
      const auto observed = client.invoke("event.record", packet);
      check(client.invoke("event.record", packet).dump() == observed.dump(),
            "event retry is idempotent");
      event["context"]["activity_id"] = action["activity_id"];
      packet = point_request(event, c);
      check(packet.has("activity_id") && !packet.has("control_id"),
            "activity context takes priority for event points");
      client.invoke("event.record", packet);
      client.invoke("event.record",
                    Json{{"type", "bad"}, {"data", nullptr}, {"control_id", "missing"}}, 404);
      client.invoke("event.record",
                    Json{{"type", "bad"},
                         {"data", nullptr},
                         {"control_id", left["control_id"]},
                         {"activity_id", action["activity_id"]}},
                    400);
      client.invoke("event.record", Json{{"type", "run.note"}, {"data", "unscoped observation"}});
      finish(action);
      close();
      captured = trace("neutral-event-loop");
      unsigned left_events = 0, right_events = 0, unscoped = 0;
      for (const auto &recorded : captured["events"].items()) {
        if (recorded.str("kind") != "custom")
          continue;
        if (recorded["attributes"].str("event_type") == "run.note") {
          check(!recorded.has("loop_id"), "unscoped event does not invent loop context");
          ++unscoped;
        } else {
          check(recorded.str("iteration") == "1", "custom result stays in its original iteration");
          left_events += recorded.str("loop_id") == "left";
          right_events += recorded.str("loop_id") == "right";
        }
      }
      check(left_events == 1 && right_events == 1 && unscoped == 1,
            "parallel loop event contexts stay distinct");
      start("L0", "neutral-event-cross-run");
      client.invoke("event.record",
                    Json{{"type", "bad"}, {"data", nullptr}, {"control_id", left["control_id"]}},
                    404);
      close();
      trace("neutral-event-cross-run");
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
      const auto opened = point(Json{{"phase", "before"},
                                     {"kind", "run"},
                                     {"ref", ref},
                                     {"input", "task"},
                                     {"level", "L0"},
                                     {"conversation_id", "neutral-reference-event"}});
      client.run = opened["context"].str("run_id");
      auto current = [&](const std::string &key) {
        auto value = ref;
        value["key"] = key;
        return value;
      };
      point(Json{{"phase", "guard"},
                 {"kind", "loop"},
                 {"ref", current("round-1")},
                 {"loop_id", "dispatch"},
                 {"actor_id", "planner"},
                 {"state", "ready"}});
      point(Json{{"phase", "before"},
                 {"kind", "model"},
                 {"ref", current("model")},
                 {"input", "original"}});
      point(Json{{"phase", "after"},
                 {"kind", "model"},
                 {"ref", current("model")},
                 {"actual_input", "original"},
                 {"executed", true},
                 {"output", "dispatch requested"}});
      point(Json{{"phase", "event"},
                 {"kind", "custom"},
                 {"name", "dispatch.result"},
                 {"ref", current("result")},
                 {"input", Json{{"status", "invalid_arguments"}, {"error", "empty dispatch"}}}});
      point(Json{{"phase", "guard"},
                 {"kind", "loop"},
                 {"ref", current("round-2")},
                 {"loop_id", "dispatch"},
                 {"actor_id", "planner"},
                 {"state", "error received"}});
      point(Json{{"phase", "after"}, {"kind", "run"}, {"ref", ref}, {"output", "done"}});
      captured = trace("neutral-reference-event");
      unsigned results = 0;
      for (const auto &event : captured["events"].items())
        if (event.str("kind") == "custom") {
          check(event.str("loop_id") == "dispatch" && event.str("iteration") == "1" &&
                    event.str("actor_id") == "planner",
                "one result point inherits its loop without host-managed control IDs");
          check(event["attributes"]["data"].str("status") == "invalid_arguments",
                "original result data retained");
          ++results;
        }
      check(results == 1, "one logical result produces one custom event");
    }

    start("L3", "neutral-replan-observations");
    first = loop();
    commit(first);
    second = loop();
    commit(second);
    auto not_sent = begin("model", "chat", "original",
                          Json{{"control_id", second["control_id"]},
                               {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    auto sent = begin("model", "chat", "original",
                      Json{{"control_id", second["control_id"]},
                           {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    finish(not_sent, "failed", false);
    finish(sent);
    check(client.invoke("run.status")["receipts"].num("applied", 0) == 2,
          "one failed model must not suppress sibling replan receipt");
    close();
    trace("neutral-replan-observations");

    start("L3", "neutral-replan-mismatch");
    first = loop();
    commit(first);
    second = loop();
    client.invoke("loop.commit",
                  Json{{"control_id", second["control_id"]}, {"decision", "continue"}});
    model = begin("model", "chat", "original",
                  Json{{"control_id", second["control_id"]},
                       {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    check(model["input"].text().find("停止重复") == std::string::npos,
          "declined replan must not inject its context; independent L1 experience remains allowed");
    finish(model);
    check(client.invoke("run.status")["receipts"].num("failed", 0) == 1,
          "mismatched host loop decision not credited as applied");
    auto retry_replan = loop();
    check(retry_replan.str("decision") == "replan", "declined replan does not consume its budget");
    commit(retry_replan);
    auto declined_stop = loop();
    check(declined_stop.str("decision") == "stop", "exhausted accepted replan proposes stop");
    client.invoke("loop.commit",
                  Json{{"control_id", declined_stop["control_id"]}, {"decision", "continue"}});
    model = begin("model", "chat", "original",
                  Json{{"control_id", declined_stop["control_id"]},
                       {"binding", Json{{"pointer", ""}, {"mode", "text"}}}});
    finish(model);
    auto check_again = loop();
    commit(check_again);
    close();
    trace("neutral-replan-mismatch");

    auto loop_only = client.invoke("run.begin", Json{{"input", "no tools"},
                                                     {"level", "L3"},
                                                     {"capabilities", Json::Array{"model", "loop"}},
                                                     {"conversation_id", "model-loop-only"}});
    client.run = loop_only.str("run_id");
    first = loop();
    commit(first);
    close();
    trace("model-loop-only");

    std::string long_input;
    for (int i = 0; i < 600; ++i)
      long_input += "普通任务背景。";
    auto long_run = client.invoke("run.begin", Json{{"input", long_input},
                                                    {"level", "L1"},
                                                    {"capabilities", Json::Array{"model"}},
                                                    {"conversation_id", "long-input"}});
    client.run = long_run.str("run_id");
    model = begin(
        "model", "chat", Json{{"system", "original"}, {"content", long_input}},
        Json{{"state", long_input}, {"binding", Json{{"pointer", "/system"}, {"mode", "text"}}}});
    check(model["input"].str("content") == long_input,
          "retrieval bound must not shorten the actual model input");
    finish(model);
    close();
    captured = trace("long-input");
    check(captured["events"].items().front()["input"].text() == long_input,
          "trace retains complete long task input");

    start("L0", "neutral-causal-retry");
    Json result_causes = Json::Array{};
    for (int round = 0; round < 2; ++round) {
      const auto control = client.invoke("loop.check", Json{{"loop_id", "retry"},
                                                            {"state", "missing permission"},
                                                            {"caused_by", result_causes}});
      commit(control);
      result_causes = Json::Array{};
      for (const auto *operation : {"lookup", "download"}) {
        const auto action = begin("action", operation, Json{{"q", "same"}},
                                  Json{{"control_id", control["control_id"]}});
        const auto result =
            client.invoke("activity.end", Json{{"activity_id", action["activity_id"]},
                                               {"executed", true},
                                               {"status", "failed"},
                                               {"output", "permission missing"},
                                               {"error", Json{{"code", "permission_denied"},
                                                              {"retryability", "non_retryable"}}}});
        result_causes.push(result["event_id"]);
      }
    }
    close();
    captured = trace("neutral-causal-retry");
    report = te::analyze_trace(te::validate_trace(proto<v1::TraceRun>(captured)));
    check(report.diagnostics.size() == 2,
          "transitive explicit causes preserve deterministic retry detection");
    check(report.summary.invalid_loop_lower_bound == 1,
          "multiple invalid activities in one iteration are not multiple loops");

    start("L0", "neutral-quarantine");
    auto interrupted = begin("model", "pending", "not sent"),
         sibling = begin("action", "survivor", nullptr);
    client.invoke("activity.abort", Json{{"activity_id", interrupted["activity_id"]},
                                         {"status", "unknown"},
                                         {"output", nullptr}});
    check(!client.invoke("activity.status", Json{{"activity_id", sibling["activity_id"]}})
               .flag("ended", true),
          "abort does not cascade");
    check(!client.invoke("run.status")["observed_boundaries"].flag("model", false),
          "abort does not fabricate observed model boundary");
    finish(sibling);
    close();
    check(client.invoke("run.status").str("evaluation_status") == "quarantined",
          "unknown observations excluded from learning");

    start("L0", "neutral-privacy");
    client.invoke("event.record", Json{{"type", "debug"}, {"data", Json{{"api_key", "secret"}}}},
                  400);
    client.invoke(
        "event.record",
        Json{{"type", "debug"}, {"data", nullptr}, {"extensions", Json{{"text", token_a}}}}, 400);
    client.invoke("event.record",
                  Json{{"type", "advice.applied"}, {"data", Json{{"status", "applied"}}}});
    check(client.invoke("run.status")["receipts"].fields().empty(),
          "extension cannot forge core receipt");
    client.invoke("capture.gap", Json{{"reason", "privacy_rejected"}});
    close();

    auto diagnostic = Json::parse(service
                                      .call(token_a, Json{{"operation", "point"},
                                                          {"request_id", "diagnostic-open"},
                                                          {"phase", "before"},
                                                          {"kind", "run"},
                                                          {"level", "L0"},
                                                          {"conversation_id", "gap-diagnostic"},
                                                          {"input", "task"}}
                                                         .dump())
                                      .body)["result"];
    auto failed_point = Json::parse(
        service
            .call(token_a, Json{{"operation", "point"},
                                {"request_id", "diagnostic-fail"},
                                {"phase", "before"},
                                {"kind", "custom"},
                                {"context", diagnostic["context"]},
                                {"input", Json{{"api_key", "never-persist-diagnostic-value"}}}}
                               .dump())
            .body)["result"];
    check(failed_point.flag("degraded", false) &&
              failed_point.str("reason") == "sensitive_field_rejected",
          "point failure preserves a safe diagnostic code");
    client.run = diagnostic.str("run_id");
    client.invoke("capture.gap",
                  Json{{"reason", "missing_boundary"}, {"error_code", "unsafe code\ntext"}}, 400);
    client.invoke("capture.gap", Json{{"reason", "missing_boundary"}, {"error_code", token_a}},
                  400);
    close();
    service.process_evaluations();
    const auto diagnostic_trace = read(folder / "data/by-user/user-a/quarantine/gap-diagnostic" /
                                       (client.run + ".trace.json"));
    bool has_code = false;
    for (const auto &event : diagnostic_trace["events"].items())
      has_code |= event["attributes"].str("error_code") == "sensitive_field_rejected";
    check(has_code &&
              diagnostic_trace.dump().find("never-persist-diagnostic-value") == std::string::npos,
          "capture gap retains the rejection cause but no private payload");

    auto minimal = client.invoke("run.begin", Json{{"input", "non-chat task"}});
    client.run = minimal.str("run_id");
    auto request = Json{{"kind", "action"},
                        {"name", "queue-consumer"},
                        {"input", nullptr},
                        {"request_id", "durable-reservation"}};
    reserved = client.invoke("activity.begin", request);
    resumed_run = client.run;
    check(reserved.dump() == client.invoke("activity.begin", request).dump(),
          "neutral operation idempotent");
    request["input"] = "changed";
    client.invoke("activity.begin", request, 409);
    Client stranger{service, token_b};
    stranger.run = client.run;
    stranger.invoke("run.status", {}, 404);
  }
  te::IntegrationService service(folder / "service.json");
  Client receiver{service};
  receiver.run = resumed_run;
  receiver.sequence = 10000;
  check(!receiver.invoke("activity.status", Json{{"activity_id", reserved["activity_id"]}})
             .flag("ended", true),
        "reservation survives producer exit and service restart");
  receiver.invoke("activity.end", Json{{"activity_id", reserved["activity_id"]},
                                       {"status", "completed"},
                                       {"executed", true},
                                       {"output", "remote result"}});
  receiver.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
  service.process_evaluations();
  check(receiver.invoke("run.status").str("evaluation_status") == "completed",
        "minimal no-conversation task evaluates after restart");
  std::cout << "Neutral boundaries: arbitrary actors, flat concurrency, JSON bindings, independent "
               "loops, extensions, privacy, restart passed\n";
}

// 测试辅助函数：lifecycle_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void lifecycle_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  c["max_stored_runs"] = 2;
  c["run_lease_ms"] = 300;
  write(folder / "service.json", c.dump());
  seed(folder / "data");
  te::IntegrationService service(folder / "service.json");
  Client client{service};
  Json original;
  for (int i = 0; i < 8; ++i) {
    auto request = Json{{"input", "retained"}, {"request_id", "retained-" + std::to_string(i)}};
    auto run = client.invoke("run.begin", request);
    client.run = run.str("run_id");
    if (i == 0)
      original = run;
    client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
    service.process_evaluations();
  }
  check(fs::exists(folder / "data/archive/runs"),
        "terminal runs archive automatically at capacity");
  check(client.invoke("run.begin", Json{{"input", "retained"}, {"request_id", "retained-0"}})
                .dump() == original.dump(),
        "archived begin remains idempotent");
  auto run = client.invoke("run.begin", Json{{"input", "long external task"}});
  client.run = run.str("run_id");
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  check(client.invoke("run.heartbeat").flag("alive", false),
        "heartbeat refreshes a long task lease");
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  service.process_evaluations();
  check(!client.invoke("run.status").flag("closed", true), "heartbeat prevents premature expiry");
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  service.process_evaluations();
  const auto expired = client.invoke("run.status");
  check(expired.flag("closed", false) && !expired.flag("capture_complete", true) &&
            expired.str("evaluation_status") == "quarantined",
        "expired observer state is unknown, never business failure or learnable success");
  std::cout << "Lifecycle: automatic archive, durable idempotency, heartbeat and lease quarantine "
               "passed\n";
}

// 测试辅助函数：schedule_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void schedule_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  c["runtime_options"]["schedule_rules"] =
      Json::Array{Json{{"rule_id", "prepare-before-image"},
                       {"target_scope", "agent:my-agent/actor:assistant"},
                       {"before_operation", "prepare"},
                       {"after_operation", "image"},
                       {"require_success", true},
                       {"reason", "先准备，再生成"}}};
  write(folder / "service.json", c.dump());
  seed(folder / "data");
  const Json actions = Json::Array{
      Json{{"action_id", "b"}, {"operation", "image"}, {"arguments", Json{{"prompt", "海报"}}}},
      Json{{"action_id", "a"}, {"operation", "prepare"}, {"arguments", Json::Object{}}},
      Json{{"action_id", "c"}, {"operation", "report"}, {"arguments", Json::Object{}}}};
  std::string run, activity;
  {
    te::IntegrationService service(folder / "service.json");
    Client client{service};
    client.run = client
                     .invoke("run.begin", Json{{"conversation_id", "schedule"},
                                               {"level", "L2"},
                                               {"input", "准备海报并独立写报告"},
                                               {"capabilities", Json::Array{"schedule"}}})
                     .str("run_id");
    run = client.run;
    const auto prepared = client.invoke(
        "activity.begin",
        Json{{"kind", "schedule"}, {"name", "skills"}, {"input", Json{{"actions", actions}}}});
    activity = prepared.str("activity_id");
    check(prepared["ready"].items().size() == 2 &&
              prepared["ready"].items()[0].str("action_id") == "a",
          "dependent skill not ready");
    auto observe = [&](const char *id, const char *state, int status = 200) {
      return client.invoke(
          "schedule.observe",
          Json{{"activity_id", activity},
               {"input", Json{{"action_id", id}, {"status", state}, {"output", "result"}}}},
          status);
    };
    observe("b", "running", 400);
    observe("a", "running");
    observe("c", "running");
    observe("c", "succeeded");
    const auto repeated = observe("c", "succeeded");
    check(!repeated.flag("changed", true), "duplicate completion is idempotent");
    client.invoke("activity.end",
                  Json{{"activity_id", activity}, {"status", "completed"}, {"output", "ack"}}, 409);
    check(service.flush_capture(), "persist unfinished batch");
  }
  {
    te::IntegrationService service(folder / "service.json");
    Client client{service};
    client.run = run;
    client.sequence = 100;
    auto observe = [&](const char *id, const char *state) {
      return client.invoke(
          "schedule.observe",
          Json{{"activity_id", activity},
               {"input", Json{{"action_id", id}, {"status", state}, {"output", "result"}}}});
    };
    const auto next = observe("a", "succeeded");
    check(next["ready"].items().size() == 1 && next["ready"].items()[0].str("action_id") == "b",
          "restart releases only successor");
    observe("b", "running");
    check(observe("b", "succeeded").flag("finished", false), "all branches settled");
    client.invoke("activity.end",
                  Json{{"activity_id", activity}, {"status", "completed"}, {"output", "done"}});
    check(client.invoke("run.status")["receipts"].num("applied", 0) == 1,
          "one observed schedule receipt");
    client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
    check(service.flush_capture(), "persist schedule trace");
    service.process_evaluations();
    const auto trace = read(folder / "data/by-user/user-a/traces/schedule" / (run + ".trace.json"));
    check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
          "schedule trace causal structure");
    std::string a_end;
    bool causally_ordered = false;
    for (const auto &event : trace["events"].items()) {
      if (event["attributes"].str("logical_action_id") == "a" && event.str("kind") == "action.end")
        a_end = event.str("event_id");
      if (event["attributes"].str("logical_action_id") == "b" &&
          event.str("kind") == "action.start")
        for (const auto &cause : event["caused_by"].items())
          causally_ordered |= !a_end.empty() && cause.text() == a_end;
    }
    check(causally_ordered, "B starts causally after A actually ended");

    unsigned point_sequence = 0;
    auto point = [&](Json request) {
      request["operation"] = "point";
      request["request_id"] = "schedule-point-" + std::to_string(++point_sequence);
      const auto response = service.call(token_a, request.dump());
      check(response.status == 200, "schedule point response");
      const auto result = Json::parse(response.body)["result"];
      check(!result.flag("degraded", false), "schedule point degraded: " + result.dump());
      return result;
    };
    const Json ref{{"run", "schedule-point-run"}};
    const Json plan_ref{{"run", "schedule-point-run"}, {"key", "plan"}};
    point(Json{{"phase", "before"},
               {"kind", "run"},
               {"ref", ref},
               {"input", "先准备再生成，独立报告继续"},
               {"level", "L2"},
               {"capabilities", Json::Array{"schedule"}}});
    const auto prepared = point(Json{{"phase", "before"},
                                     {"kind", "schedule"},
                                     {"ref", plan_ref},
                                     {"input", Json{{"actions", actions}}}});
    check(prepared["ready"].items().size() == 2, "point supplies ready actions");
    auto settled = [&](const char *id, const char *status) {
      return point(
          Json{{"phase", "event"},
               {"kind", "schedule"},
               {"ref", Json{{"run", "schedule-point-run"},
                            {"parent", "plan"},
                            {"key", std::string(id) + ":" + status}}},
               {"input", Json{{"action_id", id}, {"status", status}, {"output", "result"}}}});
    };
    settled("c", "running");
    settled("a", "running");
    const auto failure = settled("a", "failed");
    check(failure["skipped"].items().size() == 1 && failure["skipped"].items()[0].text() == "b",
          "point skips only failed dependency");
    settled("b", "skipped");
    settled("c", "succeeded");
    point(Json{{"phase", "after"}, {"kind", "schedule"}, {"ref", plan_ref}, {"output", "partial"}});
    const auto closed =
        point(Json{{"phase", "after"}, {"kind", "run"}, {"ref", ref}, {"output", "partial"}});
    check(closed.flag("closed", false) && closed.flag("capture_complete", false),
          "point schedule closes complete trace");
  }
  std::cout << "L2 scheduling: dependency, independent parallel branch, restart and causal receipt "
               "passed\n";
}

// 测试辅助函数：dispatch_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void dispatch_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  c["runtime_options"]["schedule_rules"] =
      Json::Array{Json{{"rule_id", "a-before-b"},
                       {"target_scope", "agent:my-agent/actor:assistant"},
                       {"before_operation", "prepare"},
                       {"after_operation", "image"},
                       {"require_success", true},
                       {"reason", "Prepare input first"}}};
  write(folder / "service.json", c.dump());
  seed(folder / "data");
  te::IntegrationService service(folder / "service.json");
  unsigned sequence = 0;
  auto point = [&](Json request) {
    request["operation"] = "point";
    request["request_id"] = "dispatch-" + std::to_string(++sequence);
    const auto response = service.call(token_a, request.dump());
    check(response.status == 200, "dispatch HTTP response");
    const auto result = Json::parse(response.body)["result"];
    check(!result.flag("degraded", false), "dispatch degraded: " + result.dump());
    return result;
  };
  const Json binding{{"items", "/jobs"},           {"operation", "/name"},
                     {"receipt", "/ticket"},       {"accepted", "/accepted"},
                     {"accepted_index", "/index"}, {"accepted_id", "/id"},
                     {"results", "/results"},      {"result_id", "/id"},
                     {"result_success", "/ok"},    {"result_operation", "/name"},
                     {"result_status", "/status"}, {"result_output", "/output"}};
  for (const auto failure : {false, true}) {
    const std::string run = failure ? "failed-dispatch" : "ordered-dispatch";
    const Json ref{{"run", run}}, plan{{"run", run}, {"key", "batch"}};
    point(Json{{"phase", "before"},
               {"kind", "run"},
               {"ref", ref},
               {"input", "prepare then image"},
               {"level", "L2"},
               {"capabilities", Json::Array{"schedule"}}});
    auto next = point(
        Json{{"phase", "before"},
             {"kind", "dispatch"},
             {"name", "dispatch"},
             {"ref", plan},
             {"binding", binding},
             {"input", Json{{"jobs", Json::Array{Json{{"name", "image"}}, Json{{"name", "prepare"}},
                                                 Json{{"name", "report"}}}},
                            {"reason", "keep me"}}}});
    check(next["input"]["jobs"].items().size() == 2 &&
              next["input"]["jobs"].items()[0].str("name") == "prepare" &&
              next["input"].str("reason") == "keep me",
          "first wave preserves envelope and independent parallel branch");
    auto command = [&](const std::string &name, Json input) {
      input["wave_id"] = next["wave_id"];
      return point(
          Json{{"phase", "event"},
               {"kind", "dispatch"},
               {"name", name},
               {"ref",
                Json{{"run", run}, {"parent", "batch"}, {"key", name + std::to_string(sequence)}}},
               {"input", input}});
    };
    check(command("claim", Json::Object{}).flag("execute", false), "claim actual host callback");
    check(!command("claim", Json::Object{}).flag("execute", true),
          "second process must not dispatch twice");
    const Json first_ack{{"ticket", "first"},
                         {"accepted", Json::Array{Json{{"index", 1}, {"id", "host-a"}},
                                                  Json{{"index", 2}, {"id", "host-c"}}}}};
    const auto ack = command("accept", Json{{"output", first_ack}});
    check(ack.flag("pending", false), "ACK waits for real completion");
    const auto logical = ack["output"].str("ticket");
    auto complete = [&](const Json &payload) {
      return point(
          Json{{"phase", "event"},
               {"kind", "dispatch"},
               {"name", "complete"},
               {"ref", Json{{"run", run}, {"key", "completion-" + std::to_string(sequence)}}},
               {"input", payload}});
    };
    const Json first{
        {"ticket", "first"},
        {"results", Json::Array{Json{{"id", "host-c"}, {"ok", true}, {"name", "report"}},
                                Json{{"id", "host-a"}, {"ok", !failure}, {"name", "prepare"}}}}};
    next = complete(first);
    if (!failure) {
      check(next["input"]["jobs"].items().size() == 1 &&
                next["input"]["jobs"].items()[0].str("name") == "image",
            "second wave only after completion");
      check(command("claim", Json::Object{}).flag("execute", false), "claim successor wave");
      next = complete(first);
      check(!command("claim", Json::Object{}).flag("execute", true),
            "duplicate completion cannot repeat successor");
      const Json second_ack{{"ticket", "second"},
                            {"accepted", Json::Array{Json{{"index", 1}, {"id", "host-b"}}}}};
      command("accept", Json{{"output", second_ack}});
      next = complete(
          Json{{"ticket", "second"},
               {"results", Json::Array{Json{{"id", "host-b"}, {"ok", true}, {"name", "image"}}}}});
    }
    const auto &result = next["completion"];
    check(!next.flag("pending", true) && result.str("ticket") == logical &&
              result["results"].items().size() == 3,
          "physical batches become one logical completion");
    check(result["results"].items()[0].str("name") == "image" &&
              result["results"].items()[1].str("name") == "prepare" &&
              result["results"].items()[2].str("name") == "report",
          "result order follows request, not completion order");
    if (failure)
      check(result["results"].items()[0].str("status") == "skipped" &&
                result["results"].items()[2].flag("ok", false),
            "failed prerequisite does not cancel report");
    check(point(Json{{"phase", "after"}, {"kind", "run"}, {"ref", ref}, {"output", "done"}})
              .flag("capture_complete", false),
          "asynchronous dispatch run fully captured");
  }
  std::cout << "Async dispatch: ordered waves, duplicate delivery, stable results and failure "
               "isolation passed\n";
}

// 测试辅助函数：dispatch_guard_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void dispatch_guard_tests(const fs::path &folder) {
  auto c = config(folder);
  c["runtime_options"]["action_rules"].push(Json{{"rule_id", "stop-third-failed-skill"},
                                                 {"target_scope", "agent:my-agent/actor:assistant"},
                                                 {"operation", "*"},
                                                 {"max_failed_attempts", 2},
                                                 {"reason", "same skill already failed twice"}});
  write(folder / "service.json", c.dump());
  seed(folder / "data");
  te::IntegrationService service(folder / "service.json");
  unsigned sequence = 0;
  auto point = [&](Json request) {
    request["operation"] = "point";
    request["request_id"] = "dispatch-guard-" + std::to_string(++sequence);
    const auto reply = service.call(token_a, request.dump());
    check(reply.status == 200, "dispatch guard point: " + reply.body);
    const auto result = Json::parse(reply.body)["result"];
    check(!result.flag("degraded", false), "dispatch guard degraded: " + result.str("reason"));
    return result;
  };
  const Json run{{"run", "dispatch-guard-run"}};
  point(Json{{"phase", "before"},
             {"kind", "run"},
             {"ref", run},
             {"input", "test"},
             {"level", "L3"},
             {"capabilities", Json::Array{"model", "actions", "loop"}}});
  auto ref = [&](const char *key) {
    auto value = run;
    value["key"] = key;
    return value;
  };
  point(Json{{"phase", "guard"},
             {"kind", "loop"},
             {"ref", ref("first-round")},
             {"loop_id", "coordinator"},
             {"actor_id", "assistant"},
             {"state", "start"}});
  for (const auto *key : {"failure-one", "failure-two"}) {
    const auto before = point(Json{{"phase", "before"},
                                   {"kind", "action"},
                                   {"name", "forge-image-gen"},
                                   {"actor_id", "assistant"},
                                   {"ref", ref(key)},
                                   {"input", Json::Object{}}});
    check(before.str("decision") == "allow", "first two attempts stay available");
    point(Json{{"phase", "after"},
               {"kind", "action"},
               {"ref", ref(key)},
               {"status", "failed"},
               {"executed", true},
               {"output", "fixture failure"}});
  }
  const auto blocked = point(Json{{"phase", "guard"},
                                  {"kind", "action"},
                                  {"name", "forge-image-gen"},
                                  {"actor_id", "assistant"},
                                  {"ref", ref("repeat-guard")},
                                  {"input", Json::Object{}}});
  const auto sibling = point(Json{{"phase", "guard"},
                                  {"kind", "action"},
                                  {"name", "outfit-stylist"},
                                  {"actor_id", "assistant"},
                                  {"ref", ref("sibling-guard")},
                                  {"input", Json::Object{}}});
  check(blocked.str("decision") == "reject" && sibling.str("decision") == "allow",
        "L2 guard blocks only a settled repeated failure");
  point(Json{{"phase", "after"}, {"kind", "run"}, {"ref", run}, {"output", "done"}});
  std::cout << "L2 dispatch guard: repeat failed skill blocked, sibling allowed\n";
}

// 测试辅助函数：target_retry_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void target_retry_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  c["runtime_options"]["action_rules"] =
      Json::Array{Json{{"rule_id", "same-target-budget"},
                       {"target_scope", "agent:my-agent/actor:assistant"},
                       {"operation", "generate"},
                       {"max_failed_attempts", 2},
                       {"failure_key_arguments", Json::Array{"output"}},
                       {"reason", "同一目标重试预算已耗尽"}}};
  c["runtime_options"]["action_rules"].push(
      Json{{"rule_id", "explicit-terminal"},
           {"target_scope", "agent:my-agent/actor:assistant"}, {"operation", "render"},
           {"max_failed_attempts", 1}, {"terminal_error_codes", Json::Array{"UnsupportedModel"}},
           {"error_scope_arguments", Json::Array{"model"}}, {"reason", "显式终态错误不重复执行"}});
  write(folder / "service.json", c.dump());
  te::IntegrationService service(folder / "service.json");
  Client client{service};
  auto start = [&] {
    client.run = client
                     .invoke("run.begin", Json{{"input", "independent targets"},
                                               {"level", "L2"},
                                               {"conversation_id", "target-retry"},
                                               {"capabilities", Json::Array{"actions"}}})
                     .str("run_id");
  };
  int sequence = 0;
  auto begin = [&](const std::string &target, const std::string &operation = "generate") {
    Json input = Json::Object{};
    if (!target.empty())
      input["output"] = target;
    return client.invoke("activity.begin",
                         Json{{"kind", "action"},
                              {"name", operation},
                              {"input", input},
                              {"actor_id", "assistant"},
                              {"stream_id", "execution-" + std::to_string(++sequence)}});
  };
  auto finish = [&](const Json &call, bool success, const std::string &actual = "") {
    const bool executed = call.str("decision") == "allow";
    Json request{{"activity_id", call["activity_id"]},
                 {"executed", executed},
                 {"status", executed ? "completed" : "cancelled"},
                 {"output", Json{{"success", success}}}};
    if (!actual.empty())
      request["actual_input"] = Json{{"output", actual}};
    client.invoke("activity.end", request);
  };
  start();
  for (int n = 0; n < 2; ++n) {
    const auto call = begin("A");
    check(call.str("decision") == "allow", "initial/retry allowed");
    finish(call, false);
  }
  const auto blocked = begin("A");
  check(blocked.str("decision") == "reject", "redispatch cannot reset target budget");
  finish(blocked, false);
  const auto left = begin("B"), right = begin("B");
  check(left.str("decision") == "allow" && right.str("decision") == "allow",
        "independent concurrent initial attempts");
  finish(left, false);
  finish(right, false);
  const auto retry = begin("B");
  check(retry.str("decision") == "allow", "parallel failures count once");
  finish(retry, false);
  const auto exhausted = begin("B");
  check(exhausted.str("decision") == "reject", "settled retry exhausts target budget");
  finish(exhausted, false);
  finish(begin("C"), false);
  finish(begin("C"), true);
  const auto reset = begin("C");
  check(reset.str("decision") == "allow", "success resets streak");
  finish(reset, false);
  finish(begin("D"), false, "changed-target");
  finish(begin("D"), false);
  const auto unchanged = begin("D");
  check(unchanged.str("decision") == "allow",
        "changed actual input not counted against planned target");
  finish(unchanged, true);
  const auto other = begin("A", "independent");
  check(other.str("decision") == "allow", "another operation continues");
  finish(other, true);
  for (int n = 0; n < 3; ++n) {
    const auto absent = begin("");
    check(absent.str("decision") == "allow", "missing identity is not shared failure");
    finish(absent, false);
  }
  client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
  start();
  const auto fresh = begin("A");
  check(fresh.str("decision") == "allow", "new run does not inherit failures");
  finish(fresh, true);
  client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});

  // 模拟宿主通过 ErrorInfo 报告终态错误：只阻断同执行实例、同配置的重试。
  start();
  auto render = [&](const char *execution, const char *model = "one") {
    return client.invoke("activity.begin",
                         Json{{"kind", "action"}, {"name", "render"},
                              {"input", Json{{"model", model}}}, {"actor_id", "assistant"},
                              {"stream_id", execution}});
  };
  const auto explicit_failure = render("explicit-error");
  client.invoke("activity.end",
                Json{{"activity_id", explicit_failure["activity_id"]}, {"status", "failed"},
                     {"executed", true}, {"actual_input", explicit_failure["input"]},
                     {"output", Json{{"status", "unsupported"}, {"error", "fixture_miss"}}},
                     {"error", Json{{"code", "UnsupportedModel"},
                                    {"retryability", "non_retryable"}}}});
  const auto denied = render("explicit-error"), sibling = render("independent-error");
  check(denied.str("decision") == "reject" && sibling.str("decision") == "allow",
        "explicit ErrorInfo must block only its original execution");
  finish(denied, false);
  finish(sibling, true);
  const auto changed_configuration = render("explicit-error", "two");
  check(changed_configuration.str("decision") == "allow",
        "explicit ErrorInfo must not block a different error-scope configuration");
  finish(changed_configuration, true);
  const auto structured = render("structured-output");
  client.invoke("activity.end",
                Json{{"activity_id", structured["activity_id"]}, {"status", "failed"},
                     {"executed", true}, {"actual_input", structured["input"]},
                     {"output", Json{{"error", Json{{"code", "Timeout"}}}}},
                     {"error", Json{{"code", "UnsupportedModel"},
                                    {"retryability", "non_retryable"}}}});
  const auto retry_allowed = render("structured-output");
  check(retry_allowed.str("decision") == "allow", "output structured code remains authoritative");
  finish(retry_allowed, true);
  client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
  // 本夹具关闭自动分析；显式刷新归档队列后再核验落盘轨迹，不启动模型评测。
  service.process_evaluations();
  const auto captured =
      read(folder / "data/by-user/user-a/traces/target-retry" / (client.run + ".trace.json"));
  bool explicit_recorded = false, structured_recorded = false;
  for (const auto &event : captured["events"].items()) {
    if (event.str("kind") != "action.end")
      continue;
    if (event.str("action_id") == explicit_failure.str("activity_id"))
      explicit_recorded = event["attributes"].str("observed_error_code") == "UnsupportedModel" &&
                          event["error"].str("retryability") == "non_retryable";
    if (event.str("action_id") == structured.str("activity_id"))
      structured_recorded = event["attributes"].str("observed_error_code") == "Timeout";
  }
  check(explicit_recorded && structured_recorded,
        "trace must retain explicit ErrorInfo and output-code precedence for GEPA replay");
  std::cout << "Target retry: redispatch, concurrency, success reset and run isolation passed\n";
}

// 测试辅助函数：l123_boundary_regression_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void l123_boundary_regression_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  c["runtime_options"]["loop_rules"].items()[0]["no_progress_decision"] = "stop";
  c["runtime_options"]["action_rules"] =
      Json::Array{Json{{"rule_id", "settled-target"},
                       {"target_scope", "agent:my-agent/actor:assistant"},
                       {"operation", "generate"},
                       {"max_failed_attempts", 1},
                       {"failure_key_arguments", Json::Array{"output"}},
                       {"reason", "同一目标已失败"}}};
  write(folder / "service.json", c.dump());
  te::IntegrationService service(folder / "service.json");
  unsigned sequence = 0;
  auto point = [&](Json request) {
    request["operation"] = "point";
    request["request_id"] = "boundary-fix-" + std::to_string(++sequence);
    const auto reply = service.call(token_a, request.dump());
    check(reply.status == 200, "boundary regression HTTP: " + reply.body);
    auto result = Json::parse(reply.body)["result"];
    check(!result.flag("degraded", false), "boundary regression degraded: " + result.dump());
    return result;
  };
  auto ref = [](const std::string &run, const std::string &key = "root") {
    return Json{{"run", run}, {"key", key}};
  };
  auto start = [&](const std::string &run, const std::string &level) {
    point(Json{{"phase", "before"},
               {"kind", "run"},
               {"ref", ref(run)},
               {"input", "Answer in English"},
               {"level", level},
               {"capabilities", Json::Array{"model", "actions", "schedule", "loop"}}});
  };
  auto guard = [&](const std::string &run, const std::string &key, const Json &state,
                   bool finalize = false) {
    return point(Json{{"phase", "guard"},
                      {"kind", "loop"},
                      {"ref", ref(run, key)},
                      {"loop_id", "main"},
                      {"actor_id", "assistant"},
                      {"state", state},
                      {"extensions", Json{{"finalize_on_stop", finalize}}}});
  };
  auto finish = [&](const std::string &run) {
    check(point(Json{{"phase", "after"}, {"kind", "run"}, {"ref", ref(run)}, {"output", "done"}})
              .flag("capture_complete", false),
          "boundary regression trace complete");
  };
  const Json request{
      {"messages", Json::Array{Json{{"role", "user"}, {"content", "Answer in English"}}}},
      {"tools", Json::Array{}},
      {"tool_choice", "auto"},
      {"parallel_tool_calls", true}};
  for (const std::string level : {"L0", "L3"}) {
    const auto run = "finalize-" + level;
    start(run, level);
    check(guard(run, "loop-1", "unchanged", true).str("decision") == "continue",
          "first loop continues");
    const auto stopped = guard(run, "loop-2", "unchanged", true);
    check(stopped.str("decision") == (level == "L3" ? "finalize" : "continue"),
          "finalization only after L3 stop");
    auto prepared = point(Json{{"phase", "before"},
                               {"kind", "model"},
                               {"ref", ref(run, "answer")},
                               {"actor_id", "assistant"},
                               {"input", request}});
    check(prepared.str("decision") == "allow", "one final model is permitted");
    if (level == "L0")
      check(prepared["input"].dump() == request.dump(), "L0 request is unchanged");
    else
      check(!prepared["input"].has("tools") && !prepared["input"].has("tool_choice") &&
                !prepared["input"].has("parallel_tool_calls") &&
                prepared["input"].dump().find("Report failures and unfinished work honestly") !=
                    std::string::npos,
            "final response uses external prompt and no tools");
    point(Json{{"phase", "after"},
               {"kind", "model"},
               {"ref", ref(run, "answer")},
               {"executed", true},
               {"input", prepared["input"]},
               {"output", Json{{"content", "The operation failed."}}}});
    if (level == "L3") {
      auto denied = point(Json{{"phase", "guard"},
                               {"kind", "action"},
                               {"name", "generate"},
                               {"actor_id", "assistant"},
                               {"ref", ref(run, "late-action")},
                               {"input", Json{{"output", "B"}}}});
      check(denied.str("decision") == "reject", "finalization cannot launch new work");
      auto extra = point(Json{{"phase", "before"},
                              {"kind", "model"},
                              {"ref", ref(run, "second-answer")},
                              {"actor_id", "assistant"},
                              {"input", request}});
      check(extra.str("decision") == "reject", "finalization has exactly one model budget");
      point(Json{{"phase", "after"},
                 {"kind", "model"},
                 {"ref", ref(run, "second-answer")},
                 {"executed", false},
                 {"input", extra["input"]},
                 {"output", nullptr}});
      auto schedule = point(Json{
          {"phase", "before"},
          {"kind", "schedule"},
          {"ref", ref(run, "late-schedule")},
          {"input",
           Json{{"actions", Json::Array{Json{{"action_id", "one"}, {"operation", "generate"}}}}}}});
      check(schedule.str("decision") == "reject" && schedule["ready"].items().empty() &&
                schedule.flag("finished", false),
            "late schedule is skipped, not degraded to allow");
      point(Json{{"phase", "after"},
                 {"kind", "schedule"},
                 {"ref", ref(run, "late-schedule")},
                 {"output", nullptr}});
      check(guard(run, "loop-3", "changed", true).str("decision") == "stop",
            "finalization cannot reopen loop");
    }
    finish(run);
  }
  const Json binding{{"items", "/jobs"},           {"operation", "/name"},
                     {"receipt", "/ticket"},       {"accepted", "/accepted"},
                     {"accepted_index", "/index"}, {"accepted_id", "/id"},
                     {"results", "/results"},      {"result_id", "/id"},
                     {"result_success", "/ok"},    {"result_operation", "/name"},
                     {"result_status", "/status"}, {"result_output", "/output"}};
  auto batch = [&](const std::string &run, const std::string &key, bool success,
                   const std::string &result) {
    auto wave = point(
        Json{{"phase", "before"},
             {"kind", "dispatch"},
             {"actor_id", "assistant"},
             {"ref", ref(run, key)},
             {"binding", binding},
             {"input", Json{{"jobs", Json::Array{Json{{"name", "generate"}, {"output", "A"}}}}}}});
    auto command = [&](const std::string &name, Json input) {
      input["wave_id"] = wave["wave_id"];
      return point(Json{{"phase", "event"},
                        {"kind", "dispatch"},
                        {"name", name},
                        {"ref", Json{{"run", run}, {"parent", key}, {"key", key + name}}},
                        {"input", input}});
    };
    command("claim", Json::Object{});
    const auto id = "host-" + key;
    command("accept",
            Json{{"output", Json{{"ticket", key},
                                 {"accepted", Json::Array{Json{{"index", 1}, {"id", id}}}}}}});
    const auto settled = command(
        "complete",
        Json{{"ticket", key},
             {"results",
              Json::Array{
                  Json{{"id", id}, {"name", "generate"}, {"ok", success}, {"output", result}}}}});
    check(settled.flag("managed", false) && !settled.flag("pending", true), "batch really settled");
  };
  start("async-target", "L2");
  batch("async-target", "first", false, "failed");
  for (const auto target : {"A", "B"}) {
    auto result = point(Json{{"phase", "guard"},
                             {"kind", "action"},
                             {"name", "generate"},
                             {"actor_id", "assistant"},
                             {"ref", ref("async-target", target)},
                             {"input", Json{{"output", target}}}});
    check(result.str("decision") == (std::string(target) == "A" ? "reject" : "allow"),
          "async settlement limits only the failed target without requiring a loop");
  }
  finish("async-target");
  const std::vector<std::pair<Json, bool>> outcomes{
      {Json{{"status", "security_policy_blocked"}}, true},
      {Json{{"error", Json{{"code", "network"}}}}, true},
      {Json{{"error", "network"}}, true},
      {Json{{"ok", false}}, true},
      {Json{{"success", true}, {"error", "previous attempt failed"}}, false},
      {Json{{"status", "completed"}, {"error", "previous attempt failed"}}, false},
      {Json{{"error", false}}, false},
      {"free text mentions error", false}};
  for (std::size_t i = 0; i < outcomes.size(); ++i) {
    const auto run = "structured-outcome-" + std::to_string(i);
    start(run, "L2");
    const Json input{{"output", "A"}};
    point(Json{{"phase", "before"},
               {"kind", "action"},
               {"name", "generate"},
               {"actor_id", "assistant"},
               {"ref", ref(run, "first")},
               {"input", input}});
    point(Json{{"phase", "after"},
               {"kind", "action"},
               {"ref", ref(run, "first")},
               {"executed", true},
               {"output", outcomes[i].first}});
    const auto again = point(Json{{"phase", "guard"},
                                  {"kind", "action"},
                                  {"name", "generate"},
                                  {"actor_id", "assistant"},
                                  {"ref", ref(run, "second")},
                                  {"input", input}});
    check(again.str("decision") == (outcomes[i].second ? "reject" : "allow"),
          "structured failure classification");
    finish(run);
  }
  for (const bool changed : {false, true}) {
    const std::string run = changed ? "business-progress" : "transport-only";
    start(run, "L3");
    guard(run, "round-1", "one");
    batch(run, "first", true, "artifact-A");
    check(guard(run, "round-2", "two").str("decision") == "continue", "first result is progress");
    batch(run, "second", true, changed ? "artifact-B" : "artifact-A");
    check(guard(run, "round-3", "three").str("decision") == (changed ? "continue" : "stop"),
          "business output, not a fresh transport ID, defines progress");
    finish(run);
  }
  std::cout << "L1-L3 regressions: bounded finalization, async target isolation and business "
               "progress passed\n";
}

// 测试辅助函数：settled_budget_progress_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void settled_budget_progress_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  c["runtime_options"]["action_rules"] =
      Json::Array{Json{{"rule_id", "verify-budget"},
                       {"target_scope", "agent:my-agent/actor:assistant"},
                       {"operation", "verify"},
                       {"max_attempts", 1},
                       {"reason", "已落定检查预算"}}};
  auto &rule = c["runtime_options"]["loop_rules"].items()[0];
  rule["no_progress_decision"] = "stop";
  rule["max_no_progress"] = 1;
  rule["max_iterations"] = 64;
  write(folder / "service.json", c.dump());
  te::IntegrationService service(folder / "service.json");
  Client client{service};
  client.run =
      client
          .invoke("run.begin", Json{{"input", "verify and independent work"},
                                    {"level", "L3"},
                                    {"conversation_id", "budget-progress"},
                                    {"capabilities", Json::Array{"model", "actions", "loop"}}})
          .str("run_id");
  int n = 0;
  auto control = [&](const char *branch) {
    auto step = client.invoke("loop.check",
                              Json{{"loop_id", branch}, {"state", Json{{"new_prompt", ++n}}}});
    client.invoke("loop.commit",
                  Json{{"control_id", step["control_id"]}, {"decision", step["decision"]}});
    return step;
  };
  auto begin = [&](const Json &step, const char *operation) {
    return client.invoke("activity.begin", Json{{"kind", "action"},
                                                {"name", operation},
                                                {"input", Json{{"changed_prompt", ++n}}},
                                                {"control_id", step["control_id"]}});
  };
  auto finish = [&](const Json &call) {
    const bool executed = call.str("decision") == "allow";
    client.invoke("activity.end", Json{{"activity_id", call["activity_id"]},
                                       {"executed", executed},
                                       {"status", executed ? "completed" : "cancelled"},
                                       {"output", Json{{"success", true}, {"result", ++n}}}});
  };
  auto step = control("coordinator");
  auto first = begin(step, "verify");
  finish(first);
  step = control("coordinator");
  check(step.str("decision") == "continue", "first verification remains progress");
  auto blocked = begin(step, "verify");
  check(blocked.str("decision") == "reject", "new prompt evaded settled budget");
  finish(blocked);
  auto useful = begin(step, "search");
  finish(useful);
  step = control("coordinator");
  check(step.str("decision") == "continue", "blocked branch stopped useful sibling");
  auto pending = begin(step, "search");
  finish(begin(step, "verify"));
  check(control("coordinator").str("decision") == "continue",
        "pending sibling interrupted by rejection");
  finish(pending);
  step = control("coordinator");
  check(step.str("decision") == "continue", "settled new sibling result was ignored");
  finish(begin(step, "verify"));
  check(control("coordinator").str("decision") == "stop",
        "rejected redispatch with changed prompt did not converge");
  check(control("independent").str("decision") == "continue",
        "another execution inherited stopped loop");
  client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
}

// 宿主在执行前拒绝参数时，记录真实失败但不增加工具执行次数或伪造策略采用。
// 每轮状态故意变化，保证收敛来自动作账本，而不是碰巧相同的消息指纹。
void host_rejection_progress_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  auto &rule = c["runtime_options"]["loop_rules"].items()[0];
  rule["no_progress_decision"] = "stop";
  rule["max_no_progress"] = 2;
  rule["max_iterations"] = 64;
  rule["replan_on_failure"] = false;
  write(folder / "service.json", c.dump());
  te::IntegrationService service(folder / "service.json");
  Client client{service};
  client.run = client.invoke("run.begin", Json{{"input", "校验拒绝与并发分支"},
      {"level", "L3"}, {"conversation_id", "host-rejection"},
      {"capabilities", Json::Array{"actions", "loop"}}}).str("run_id");
  int sequence = 0;
  auto control = [&](const std::string &branch) {
    auto step = client.invoke("loop.check", Json{{"loop_id", branch},
        {"state", Json{{"changed_message", ++sequence}}}});
    client.invoke("loop.commit", Json{{"control_id", step["control_id"]},
        {"decision", step["decision"]}});
    return step;
  };
  auto begin = [&](const Json &step) {
    return client.invoke("activity.begin", Json{{"kind", "action"}, {"name", "dispatch"},
        {"control_id", step["control_id"]}, {"input", Json::Object{}},
        {"apply_policy", false}});
  };
  auto finish = [&](const Json &action, const std::string &status) {
    Json request{{"activity_id", action["activity_id"]}, {"status", status},
        {"executed", status == "completed"}, {"actual_input", Json::Object{}},
        {"output", Json{{"result", ++sequence}}}};
    if (status == "failed")
      request["error"] = Json{{"code", "invalid_arguments"}, {"retryability", "unknown"}};
    client.invoke("activity.end", request);
  };
  auto reject = [&](const Json &step) { finish(begin(step), "failed"); };

  auto step = control("repeated");
  reject(step);
  step = control("repeated");
  check(step.str("decision") == "continue", "first host rejection allows argument repair");
  reject(step);
  check(control("repeated").str("decision") == "stop",
        "consecutive host rejection must converge despite changed messages");
  check(control("independent").str("decision") == "continue",
        "host rejection cannot stop another loop");

  // 成功分支不受失败返回顺序影响；在途分支未完成前也不能终止循环。
  step = control("siblings");
  reject(step);
  step = control("siblings");
  reject(step);
  finish(begin(step), "completed");
  step = control("siblings");
  check(step.str("decision") == "continue", "successful sibling resets rejected-only progress");
  auto pending = begin(step);
  reject(step);
  step = control("siblings");
  reject(step);
  check(control("siblings").str("decision") == "continue", "pending sibling is not interrupted");
  finish(pending, "completed");
  step = control("siblings");
  check(step.str("decision") == "continue", "settled sibling remains useful progress");
  finish(begin(step), "completed");
  reject(step);
  check(control("siblings").str("decision") == "continue", "late rejection preserves sibling success");

  for (int i = 0; i < 3; ++i) {
    step = control("cancelled");
    check(step.str("decision") == "continue", "ordinary cancellation is not host failure");
    finish(begin(step), "cancelled");
  }
  check(control("cancelled").str("decision") == "continue", "cancellations do not accumulate rejection budget");
  client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
  service.process_evaluations();
  const auto trace = read(folder / "data/by-user/user-a/traces/host-rejection" /
                          (client.run + ".trace.json"));
  bool counted = false;
  for (const auto &event : trace["events"].items()) {
    const auto &query = event["attributes"]["decision"]["query"];
    if (query.str("boundary") == "loop" && query.num("repeated_action_steps", 0) >= 2) {
      counted = query.num("failed_actions", 0) == 1 &&
                query.num("repeated_failure_steps", 0) == 0;
    }
    if (event.str("kind") == "action.end" && event.str("status") == "failed")
      check(!event["attributes"].flag("executed", true) &&
                event["error"].str("code") == "invalid_arguments",
            "host rejection must retain false execution and explicit error");
  }
  check(counted, "host rejections count without fabricating actual operation failures");
}

// 测试辅助函数：terminal_recovery_chain_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void terminal_recovery_chain_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  c["runtime_options"]["action_rules"] =
      Json::Array{Json{{"rule_id", "terminal-video"},
                       {"target_scope", "agent:my-agent/actor:assistant"},
                       {"operation", "video"},
                       {"terminal_error_codes", Json::Array{"UnsupportedModel"}},
                       {"error_scope_arguments", Json::Array{"model"}},
                       {"max_failed_attempts", 1},
                       {"reason", "本配置已明确不支持，不重复提交"}}};
  auto &loop_rule = c["runtime_options"]["loop_rules"].items()[0];
  loop_rule["no_progress_decision"] = "stop";
  loop_rule["max_replans"] = 0;
  loop_rule["max_no_progress"] = 1;
  loop_rule["max_iterations"] = 64;
  loop_rule["error_codes"] = Json::Array{"UnsupportedModel"};
  loop_rule["after_action_rejection"] = true;
  write(folder / "service.json", c.dump());
  te::IntegrationService service(folder / "service.json");
  Client client{service};
  client.run =
      client
          .invoke("run.begin", Json{{"input", "video plus independent report"},
                                    {"level", "L3"},
                                    {"conversation_id", "terminal-chain"},
                                    {"capabilities", Json::Array{"model", "actions", "loop"}}})
          .str("run_id");
  int n = 0;
  auto control = [&](const char *branch) {
    auto step =
        client.invoke("loop.check", Json{{"loop_id", branch}, {"state", Json{{"message", ++n}}}});
    client.invoke("loop.commit",
                  Json{{"control_id", step["control_id"]}, {"decision", step["decision"]}});
    return step;
  };
  auto begin = [&](const Json &step, const char *operation, const char *model) {
    return client.invoke(
        "activity.begin",
        Json{{"kind", "action"},
             {"name", operation},
             {"input", Json{{"model", model}, {"output", "output-" + std::to_string(++n)}}},
             {"control_id", step["control_id"]}});
  };
  auto finish = [&](const Json &call, bool fail) {
    const bool executed = call.str("decision") == "allow";
    client.invoke("activity.end",
                  Json{{"activity_id", call["activity_id"]},
                       {"executed", executed},
                       {"status", executed ? "completed" : "cancelled"},
                       {"output", fail ? Json{{"status", "failed"},
                                              {"error", Json{{"code", "UnsupportedModel"}}}}
                                       : Json{{"success", true}}}});
  };
  const auto report_step = control("report");
  auto report = begin(report_step, "write", "original");
  auto step = control("video");
  finish(begin(step, "video", "original"), true);
  step = control("video");
  check(step.str("decision") == "continue", "one failure stopped the whole task");
  auto rejected = begin(step, "video", "original");
  check(rejected.str("decision") == "reject", "changed output path evaded terminal guard");
  finish(rejected, false);
  check(control("video").str("decision") == "stop",
        "terminal rejection was followed by another retry budget");
  finish(report, false);
  check(control("report").str("decision") == "continue",
        "failed video stopped an independent report");
  auto changed = begin(control("other-config"), "video", "fixed");
  check(changed.str("decision") == "allow", "terminal failure poisoned another configuration");
  finish(changed, false);
  // 失败之后的成功、同轮成功分支和待完成分支不能携带旧拒绝/错误条件。
  step = control("recovered");
  finish(begin(step, "video", "recoverable"), true);
  step = control("recovered");
  auto denied = begin(step, "video", "recoverable");
  finish(denied, false);
  auto useful = begin(step, "write", "report");
  check(control("recovered").str("decision") == "continue",
        "conditional rule interrupted pending useful work");
  finish(useful, false);
  step = control("recovered");
  check(step.str("decision") == "continue", "successful recovery kept stale terminal condition");
  check(control("recovered").str("decision") == "continue",
        "consumed rejection leaked to a later step");
  step = control("success-first");
  finish(begin(step, "write", "report"), false);
  finish(begin(step, "video", "success-first"), true);
  step = control("success-first");
  check(step.str("decision") == "continue", "completion order suppressed useful work");
  finish(begin(step, "video", "success-first"), false);
  check(control("success-first").str("decision") == "continue",
        "retired mixed-batch error resurfaced after commit");
  auto old_step = control("late-after-check");
  finish(begin(old_step, "write", "report"), false);
  auto checkpoint = client.invoke(
      "loop.check", Json{{"loop_id", "late-after-check"}, {"state", Json{{"message", ++n}}}});
  finish(begin(old_step, "video", "late-after-check"), true);
  client.invoke("loop.commit", Json{{"control_id", checkpoint["control_id"]},
                                    {"decision", checkpoint["decision"]}});
  check(control("late-after-check").str("decision") == "continue",
        "late failure stopped without a repeated rejection");
  step = control("unrelated-rejection");
  auto missing = begin(step, "video", "original");
  check(missing.str("decision") == "allow",
        "another execution inherited the terminal action rejection");
  finish(missing, false);
  check(control("unrelated-rejection").str("decision") == "continue",
        "another execution inherited terminal error codes");
  client.invoke("run.end",
                Json{{"status", "completed"}, {"output", "report succeeded, video failed"}});
  service.process_evaluations();
  const auto trace =
      read(folder / "data/by-user/user-a/traces/terminal-chain" / (client.run + ".trace.json"));
  check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
        "conditional recovery trace invalid");
  bool matched = false, recovered = false, sibling = false, retired = false, late = false;
  for (const auto &event : trace["events"].items()) {
    const auto &q = event["attributes"]["decision"]["query"];
    if (q.str("boundary") != "loop")
      continue;
    if (q.str("execution_id") == "video" && q.str("iteration") == "3")
      matched = q.num("rejected_actions", 0) == 1 && q["error_codes"].array() &&
                q["error_codes"].items().front().text() == "UnsupportedModel";
    if (q.str("execution_id") == "recovered" && q.str("iteration") == "4")
      recovered = q.num("rejected_actions", 0) == 0 &&
                  (!q["error_codes"].array() || q["error_codes"].items().empty());
    if (q.str("execution_id") == "unrelated-rejection" && q.str("iteration") == "2")
      sibling = q.num("rejected_actions", 0) == 0 &&
                (!q["error_codes"].array() || q["error_codes"].items().empty());
    if (q.str("execution_id") == "success-first" && q.str("iteration") == "3")
      retired = q.num("rejected_actions", 0) == 1 &&
                (!q["error_codes"].array() || q["error_codes"].items().empty());
    if (q.str("execution_id") == "late-after-check" && q.str("iteration") == "3")
      late = q["error_codes"].array() && q["error_codes"].items().size() == 1 &&
             q["error_codes"].items().front().text() == "UnsupportedModel";
  }
  check(matched && recovered && sibling && retired && late,
        "conditional loop queries lost error/rejection causality");
}

// 测试辅助函数：terminal_branch_isolation_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void terminal_branch_isolation_tests(const fs::path &folder) {
  auto c = config(folder);
  c["auto_evaluate"] = false;
  c["runtime_options"]["action_rules"] =
      Json::Array{Json{{"rule_id", "terminal-video"},
                       {"target_scope", "agent:my-agent/actor:assistant"},
                       {"operation", "video"},
                       {"terminal_error_codes", Json::Array{"UnsupportedModel"}},
                       {"error_scope_arguments", Json::Array{"model"}},
                       {"max_failed_attempts", 1},
                       {"reason", "同一分支的配置错误不重复提交"}}};
  write(folder / "service.json", c.dump());
  te::IntegrationService service(folder / "service.json");
  Client client{service};
  for (const bool explicit_model : {false, true})
    for (const bool use_loop : {false, true}) {
      const auto conversation =
          std::string(explicit_model ? "explicit" : "default") + (use_loop ? "-loop" : "-stream");
      auto start = [&] {
        client.run =
            client
                .invoke("run.begin", Json{{"input", "two independent video tasks"},
                                          {"level", "L2"},
                                          {"conversation_id", conversation},
                                          {"capabilities", Json::Array{"actions", "loop"}}})
                .str("run_id");
      };
      start();
      auto begin = [&](const char *branch, bool apply = true, const char *model = "same-model") {
        Json input{{"output", std::string(branch) + ".mp4"}};
        if (explicit_model)
          input["model"] = model;
        Json request{{"kind", "action"},
                     {"name", "video"},
                     {"stream_id", branch},
                     {"input", input},
                     {"apply_policy", apply}};
        if (use_loop) {
          auto control =
              client.invoke("loop.check", Json{{"loop_id", branch}, {"state", Json::Object{}}});
          client.invoke("loop.commit", Json{{"control_id", control["control_id"]},
                                            {"decision", control["decision"]}});
          request["control_id"] = control["control_id"];
        }
        return client.invoke("activity.begin", request);
      };
      auto finish = [&](const Json &call, bool failed) {
        const bool executed = call.str("decision") == "allow";
        client.invoke("activity.end",
                      Json{{"activity_id", call["activity_id"]},
                           {"executed", executed},
                           {"actual_input", call["input"]},
                           {"status", executed ? "completed" : "cancelled"},
                           {"output", failed ? Json{{"error", Json{{"code", "UnsupportedModel"}}}}
                                             : Json{{"success", true}}}});
      };
      auto first = begin("left");
      check(first.str("decision") == "allow", "first branch must execute");
      finish(first, true);
      // 同工具、同默认/显式模型；右分支首次取快照晚于左分支失败，仍必须允许执行。
      auto sibling = begin("right");
      check(sibling.str("decision") == "allow", "sibling first attempt inherited terminal failure");
      finish(sibling, true);
      for (const auto *branch : {"left", "right"}) {
        auto retry = begin(branch);
        check(retry.str("decision") == "reject", "own terminal retry was not rejected");
        finish(retry, false);
      }
      // 宿主明确观测到左分支恢复成功，只清理该分支，不能解除右分支的错误记录。
      finish(begin("left", false), false);
      auto recovered = begin("left");
      check(recovered.str("decision") == "allow", "successful branch retained terminal failure");
      finish(recovered, false);
      auto independent = begin("right");
      check(independent.str("decision") == "reject",
            "sibling success cleared another branch failure");
      finish(independent, false);
      if (explicit_model) {
        auto changed = begin("right", true, "fixed-model");
        check(changed.str("decision") == "allow",
              "changed model inherited old configuration failure");
        finish(changed, false);
      }
      client.invoke("run.end",
                    Json{{"status", "completed"}, {"output", "isolated terminal observations"}});
      start();
      auto fresh = begin("right");
      check(fresh.str("decision") == "allow", "new run inherited terminal failure");
      finish(fresh, false);
      client.invoke("run.end", Json{{"status", "completed"}, {"output", "fresh run"}});
    }
}

// 测试辅助函数：finalization_capability_tests。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void finalization_capability_tests(const fs::path &folder) {
  for (const auto &flags :
       {std::pair{true, true}, std::pair{true, false}, std::pair{false, true}}) {
    const bool available = flags.first && flags.second;
    const auto root =
        folder / (flags.first ? (flags.second ? "supported" : "no-opt-in") : "no-template");
    auto c = config(root);
    c["auto_evaluate"] = false;
    auto &rule = c["runtime_options"]["loop_rules"].items()[0];
    rule["max_iterations"] = 2;
    rule["max_no_progress"] = 0;
    rule["require_finalization"] = true;
    if (!flags.first) {
      write(root / "empty-finalization.txt", "");
      c["finalization_template_file"] = (root / "empty-finalization.txt").string();
    }
    write(root / "service.json", c.dump());
    te::IntegrationService service(root / "service.json");
    Client client{service};
    client.run =
        client
            .invoke("run.begin", Json{{"input", "finish settled work"},
                                      {"level", "L3"},
                                      {"conversation_id", "finalization-capability"},
                                      {"capabilities", Json::Array{"model", "actions", "loop"}}})
            .str("run_id");
    std::size_t iteration = 0;
    auto control = [&] {
      Json request{
          {"loop_id", "main"}, {"actor_id", "assistant"}, {"state", Json{{"step", ++iteration}}}};
      if (flags.second)
        request["extensions"] = Json{{"finalize_on_stop", true}};
      auto step = client.invoke("loop.check", request);
      client.invoke("loop.commit",
                    Json{{"control_id", step["control_id"]}, {"decision", step["decision"]}});
      return step;
    };
    auto step = control();
    check(step.str("decision") == "continue", "finalization must retain minimum work");
    const auto pending = client.invoke("activity.begin", Json{{"kind", "action"},
                                                              {"name", "lookup"},
                                                              {"input", Json::Object{}},
                                                              {"control_id", step["control_id"]}});
    check(control().str("decision") == "continue", "pending branch continues before cap");
    check(control().str("decision") == "continue", "finalization cap cannot truncate pending work");
    client.invoke("activity.end", Json{{"activity_id", pending["activity_id"]},
                                       {"executed", true},
                                       {"status", "completed"},
                                       {"output", Json{{"success", true}}}});
    step = control();
    check(step.str("decision") == (available ? "stop" : "continue"),
          "finalization requires both local template and current boundary opt-in");
    if (available) {
      auto answer = client.invoke("activity.begin",
                                  Json{{"kind", "model"},
                                       {"name", "answer"},
                                       {"input", Json{{"system", "Answer from observed results"},
                                                      {"tools", Json::Array{}}}},
                                       {"control_id", step["control_id"]}});
      check(answer.str("decision") == "allow" && !answer["input"].has("tools"),
            "conditional cap retains answer-only finalization");
      client.invoke("activity.end", Json{{"activity_id", answer["activity_id"]},
                                         {"executed", true},
                                         {"actual_input", answer["input"]},
                                         {"status", "completed"},
                                         {"output", Json{{"content", "Lookup completed."}}}});
    }
    client.invoke("run.end", Json{{"status", "completed"}, {"output", "done"}});
    check(service.flush_capture(), "persist finalization capability trace");
    service.process_evaluations();
    const auto trace = read(root / "data/by-user/user-a/traces/finalization-capability" /
                            (client.run + ".trace.json"));
    check(te::validate_trace(proto<v1::TraceRun>(trace)).report.trace_valid(),
          "finalization capability trace valid");
    unsigned queries = 0;
    for (const auto &event : trace["events"].items()) {
      const auto &query = event["attributes"]["decision"]["query"];
      if (query.str("boundary") != "loop")
        continue;
      ++queries;
      check(query.flag("can_finalize", false) == available,
            "finalization query must reflect the local boundary, not a different host");
    }
    check(queries == 4, "all finalization checks are captured");
  }
}

// 回归用例：main。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
int main(int argc, char **argv) {
  try {
    ::setenv("INTEGRATION_TEST_A", token_a.c_str(), 1);
    ::setenv("INTEGRATION_TEST_B", token_b.c_str(), 1);
    if (argc == 3 && std::string(argv[1]) == "--fixture") {
      const auto folder = fs::absolute(argv[2]);
      check(!fs::exists(folder / "service.json"), "fixture output already exists");
      auto c = config(folder);
      c["port"] = 0;
      write(folder / "service.json", c.dump());
      seed(folder / "data");
      std::cout << "Fixture ready\n";
      return 0;
    }
    std::string path =
        (fs::temp_directory_path() / "agentflow-integration-service.XXXXXX").string();
    check(::mkdtemp(path.data()), "temporary test directory");

    struct Cleanup {
      fs::path path;

      // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
      ~Cleanup() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
      }
    } cleanup{path};

    run_tests(path);
    delegation_tests(fs::path(path) / "delegation");
    boundary_tests(fs::path(path) / "boundary");
    failure_replan_tests(fs::path(path) / "failure-replan");
    outcome_progress_tests(fs::path(path) / "outcome-progress");
    aggressive_failure_tests(fs::path(path) / "aggressive-failures");
    schedule_outcome_tests(fs::path(path) / "schedule-outcomes");
    lifecycle_tests(fs::path(path) / "lifecycle");
    schedule_tests(fs::path(path) / "schedule");
    dispatch_tests(fs::path(path) / "dispatch");
    dispatch_guard_tests(fs::path(path) / "dispatch-guard");
    target_retry_tests(fs::path(path) / "target-retry");
    settled_budget_progress_tests(fs::path(path) / "budget-progress");
    host_rejection_progress_tests(fs::path(path) / "host-rejection-progress");
    terminal_recovery_chain_tests(fs::path(path) / "terminal-chain");
    terminal_branch_isolation_tests(fs::path(path) / "terminal-branches");
    finalization_capability_tests(fs::path(path) / "finalization-capability");
    l123_boundary_regression_tests(fs::path(path) / "l123-regression");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
