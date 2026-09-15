#include "trajectory_evolution/local_runtime.h"
#include "trajectory_evolution/evolution.h"
#include "integration_json.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

using namespace trajectory::evolution;
using namespace trajectory::evolution::integration;
namespace fs = std::filesystem;
const char* local_token = "local-regression-principal-00000001";
void check(bool ok, const char* text) { if(!ok) throw std::runtime_error(text); }
Json read(const fs::path& path) {
  std::ifstream f(path); return Json::parse(std::string(std::istreambuf_iterator<char>(f), {}));
}
void write(const fs::path& path, const Json& value) {
  fs::create_directories(path.parent_path()); std::ofstream f(path); f << value.dump(); check(bool(f), "fixture write");
}
Json fixture(const fs::path& root) {
  auto config = read(fs::path(LOCAL_TEST_SOURCE) / "config/boundary-service.json");
  config["data_dir"] = (root / "data").string();
  config["principals"] = Json::Array{Json{{"user_id", "native-user"}, {"token_env", "LOCAL_TEST_TOKEN"}}};
  config["runtime_config"] = (fs::path(LOCAL_TEST_SOURCE) / "config/standard-runtime.json").string();
  config["experience_template_file"] = (fs::path(LOCAL_TEST_SOURCE) / "config/prompts/runtime-experience.txt").string();
  config["runtime_options"]["loop_rules"] = Json::Array{};
  config["runtime_options"]["action_rules"] = Json::Array{Json{{"rule_id", "image-first"}, {"scope_id", "default"},
    {"operation", "video"}, {"required_arguments", Json::Array{"image"}}, {"reason", "需要图片"}}};
  write(root / "service.json", config); return config;
}
struct Client {
  LocalRuntime& runtime;
  std::string run;
  unsigned sequence = 0;
  std::vector<double> microseconds;
  Json call(const char* operation, Json fields = Json::Object{}, bool good = true) {
    fields["operation"] = operation; fields["request_id"] = "native-" + std::to_string(++sequence);
    if(std::string(operation) != "run.begin") fields["run_id"] = run;
    const auto start = std::chrono::steady_clock::now();
    const auto response = runtime.invoke(fields.dump());
    const auto result = Json::parse(response.body)["result"];
    microseconds.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    if(good && (response.status != 200 || result.flag("degraded", true)))
      throw std::runtime_error(std::string(operation) + ": " + response.body);
    return result;
  }
  void start() {
    run = call("run.begin", Json{{"input", "检查图片和天气，准备旅行视频"}, {"conversation_id", "会话一"},
      {"level", "L3"}, {"capabilities", Json::Array{"model", "actions", "loop"}}}).str("run_id");
  }
  void finish(bool good = true) { call("run.end", Json{{"status", "completed"}, {"business_status", "partial"}, {"output", "测试完成"}}, good); }
};

void deny_business_io() {
#ifdef __linux__
  std::vector<sock_filter> code{{BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(seccomp_data, nr)}};
  for(int syscall : {SYS_open, SYS_openat, SYS_creat, SYS_read, SYS_write, SYS_pwrite64, SYS_fsync,
                    SYS_fdatasync, SYS_socket, SYS_connect, SYS_sendto, SYS_recvfrom, SYS_flock,
                    SYS_rename, SYS_renameat, SYS_mkdir, SYS_nanosleep, SYS_clock_nanosleep}) {
    code.push_back({BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<unsigned>(syscall)});
    code.push_back({BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ERRNO | EPERM});
  }
  code.push_back({BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW});
  sock_fprog program{static_cast<unsigned short>(code.size()), code.data()};
  check(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0,
        "cannot install business-thread I/O guard");
#endif
}

int main() {
  try {
    const auto numbers = Json::parse(R"({"max_tokens":4096,"temperature":0.25,"nested":[0,-3,true,null,1.5]})");
    const auto stored = numbers.dump();
    check(stored.find("4096.0") != std::string::npos, "stored canonical encoding changed");
    check(numbers.dump_for_transport().find("\"max_tokens\":4096,") != std::string::npos, "wire integer missing");
    check(Json::parse(numbers.dump_for_transport()).dump() == stored, "transport changed version/hash material");
    setenv("LOCAL_TEST_TOKEN", local_token, 1);
    std::string pattern = std::string(LOCAL_TEST_BUILD) + "/local-runtime.XXXXXX";
    check(mkdtemp(pattern.data()) != nullptr, "fixture directory"); const fs::path root(pattern);
    fixture(root); LocalRuntimeOptions options; options.background = false;
    LocalRuntime runtime(root / "service.json", local_token, options); Client client{runtime};
    // Blocking I/O is denied only on the business thread. Maintenance runs later.
    auto worker = std::async(std::launch::async, [&] {
      deny_business_io(); client.start();
      for(unsigned round = 0; round < 40; ++round) {
        auto control = client.call("loop.check", Json{{"loop_id", "planner"}, {"state", Json{{"round", static_cast<int>(round)}}}});
        client.call("loop.commit", Json{{"control_id", control["control_id"]}, {"decision", control["decision"]}});
        auto model = client.call("activity.begin", Json{{"kind", "model"}, {"name", "reason"},
          {"input", Json{{"system", "给出下一步操作"}}}, {"binding", Json{{"pointer", "/system"}, {"mode", "text"}}},
          {"control_id", control["control_id"]}});
        client.call("activity.end", Json{{"activity_id", model["activity_id"]}, {"status", "completed"}, {"executed", true},
          {"actual_input", model["input"]}, {"output", "并行查询图片和天气"}});
        auto a = client.call("activity.begin", Json{{"kind", "action"}, {"name", "weather"}, {"input", Json::Object{}}});
        auto b = client.call("activity.begin", Json{{"kind", "action"}, {"name", "video"}, {"input", Json::Object{}}});
        check(b.str("decision") == "reject", "local action gate ignored");
        client.call("activity.end", Json{{"activity_id", b["activity_id"]}, {"status", "cancelled"}, {"executed", false}, {"output", "缺少图片"}});
        client.call("activity.end", Json{{"activity_id", a["activity_id"]}, {"status", "completed"}, {"executed", true}, {"output", "晴天"}});
      }
      client.finish();
    });
    worker.get();
    check(!fs::exists(root / "data/runs" / (client.run + ".json")), "business call persisted synchronously");
    check(Json::parse(runtime.metrics()).num("degraded_calls", 0) == 0, "ordinary execution degraded");
    runtime.drain(); runtime.evaluate_pending();
    fs::path trace;
    for(const auto& item : fs::recursive_directory_iterator(root / "data/by-user/native-user/traces"))
      if(item.path().extension() == ".json") trace = item.path();
    auto data = read(trace); check(data["attributes"].flag("capture_complete", false), "complete trace missing");
    check(validate_trace(proto<v1::TraceRun>(data)).report.trace_valid(), "local trace integrity");
    check(fs::exists(root / "data/by-user/native-user/memory/evolution-memory.pb"), "memory pipeline missing");
    const auto status = client.call("run.status"); check(status.str("evaluation_status") == "completed", "background evaluation failed");

    // Drain deterministically between a completion and its transport retry.
    // Background timing must not decide whether an identical callback succeeds.
    Client replay{runtime}; replay.sequence = 5000; replay.start();
    const auto activity = replay.call("activity.begin", Json{{"kind", "action"}, {"name", "read"}, {"input", "safe"}});
    Json completion{{"operation", "activity.end"}, {"request_id", "replayed-completion"}, {"run_id", replay.run},
        {"activity_id", activity["activity_id"]}, {"status", "completed"}, {"executed", true}, {"output", "done"}};
    const auto receipt = runtime.invoke(completion.dump()).body;
    replay.finish(); runtime.drain();
    auto retried = std::async(std::launch::async, [&] {
      deny_business_io(); return runtime.invoke(completion.dump()).body;
    }).get();
    check(retried == receipt, "persisting a run discarded completion idempotency");
    completion["output"] = "different";
    check(Json::parse(runtime.invoke(completion.dump()).body)["result"].str("reason") == "request_id_conflict",
        "closed-run replay accepted changed content");

    Client paused{runtime}; paused.sequence = 10000; paused.start(); runtime.set_enabled(false);
    auto skipped = paused.call("activity.begin", Json{{"kind", "action"}, {"name", "read"}, {"input", Json::Object{}}}, false);
    check(skipped.flag("degraded", false) && !skipped.flag("policy_applied", true), "pause was not explicit");
    runtime.set_enabled(true); paused.finish(false); runtime.drain(); runtime.evaluate_pending();
    check(!paused.call("run.status", Json::Object{}, false).flag("capture_complete", true), "paused run was learned");

    Client privacy{runtime}; privacy.sequence = 20000; privacy.start();
    auto denied = privacy.call("event.record", Json{{"type", "check"}, {"data", Json{{"api_key", "fixture-only-private-value"}}}}, false);
    check(denied.flag("degraded", false), "sensitive event not rejected"); privacy.finish(false); runtime.drain(); runtime.evaluate_pending();
    for(const auto& item : fs::recursive_directory_iterator(root / "data")) if(item.is_regular_file()) {
      std::ifstream f(item.path(), std::ios::binary); const std::string bytes(std::istreambuf_iterator<char>(f), {});
      check(bytes.find("fixture-only-private-value") == std::string::npos, "private value persisted");
    }
    std::sort(client.microseconds.begin(), client.microseconds.end());
    Json summary{{"output", root.string()}, {"business_thread_io_denied", true}, {"complete_trace_valid", true},
      {"calls", client.microseconds.size()}, {"median_us", client.microseconds[client.microseconds.size()/2]},
      {"p95_us", client.microseconds[client.microseconds.size()*95/100]},
      {"p99_us", client.microseconds[client.microseconds.size()*99/100]}, {"max_us", client.microseconds.back()},
      {"automatic_memory_pipeline", true}, {"pause_quarantined", true}, {"privacy_quarantined", true}};
    write(root / "test-summary.json", summary); std::cout << summary.dump() << '\n';
    return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
