// 进程内低延迟运行时：读取固定策略并将采集事件交给后台处理。
// 主 Agent 的同步路径不能等待模型评测或全库管理；队列和负载保持有界。
// 关闭、排空和失败降级分别处理，不能为了采集阻断业务执行。

#include "trajectory_evolution/local_runtime.h"
#include "trajectory_evolution/evolution.h"
#include "boundary_runtime.h"
#include "instrumentation.h"
#include "local_support.h"
#include "agent_evaluation_support.h"
#include "../../clients/c/evolution.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace trajectory::evolution {
namespace {
using namespace integration;
using PrivateMemory = agentflow::trajectory::evaluation::EvolutionMemoryStore;

struct Memory {
  PrivateMemory private_memory;
  v1::SharedExperienceStore shared;
  Json publications;

  // 初始化 Memory 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit Memory(const Json &env)
      : private_memory(proto<PrivateMemory>(env["memory"])),
        shared(env.has("shared_memory") ? proto<v1::SharedExperienceStore>(env["shared_memory"])
                                        : v1::SharedExperienceStore{}),
        publications(env["published_versions"]) {}
};

// 计算稳定内容摘要，供请求标识、缓存或版本比较使用。
std::string hash(const std::string &value) {
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), bytes),
          "hash_failed", 500);
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : bytes) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

// 校验或编码单段身份字段，供当前模块构造隔离路径或稳定标识。
std::string component(const std::string &value) {
  std::string result;
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.')
      result += c;
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}

// 生成当前事件的时间戳，保持协议要求的时间单位。
std::string timestamp() {
  return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}

// 读取单调递增时钟，用于计算耗时与截止期限，避免系统时钟调整影响超时判断。
std::size_t monotonic_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 取得共享后台服务，避免每个用户各启一份写入线程。
std::shared_ptr<IntegrationService> backend(const std::filesystem::path &path) {
  // 该注册表只在启动时使用，多用户共享同一个后台写入器。
  static std::mutex gate;
  static std::map<std::string, std::weak_ptr<IntegrationService>> services;
  std::lock_guard<std::mutex> lock(gate);
  auto &weak = services[std::filesystem::absolute(path).lexically_normal().string()];
  auto service = weak.lock();
  if (!service) {
    service = std::make_shared<IntegrationService>(path);
    weak = service;
  }
  return service;
}

// 判断是否处于隔离评测环境，禁止评测数据误发到生产采集端。
bool isolated_trial() {
  return std::getenv("TRAJECTORY_EVOLUTION_TRIAL_CONFIG") &&
         std::getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN");
}

// 读取当前评测试验的入口上下文，绑定其身份与生命周期。
bool entry_trial() {
  const auto *mode = std::getenv("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY");
  return mode && std::string(mode) == "1" && isolated_trial() &&
         std::getenv("TRAJECTORY_EVOLUTION_TRIAL_URL");
}

enum Phase { free_slot, active, closed, writing, persisted };

struct CaptureLosses {
  const std::string run_id;
  std::atomic<std::size_t> count{0};

  // 初始化 CaptureLosses 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit CaptureLosses(std::string id) : run_id(std::move(id)) {}
};

struct Slot {
  std::timed_mutex gate;
  std::atomic<int> phase{free_slot};
  std::atomic<std::size_t> inflight{0};
  std::shared_ptr<CaptureLosses> losses;
  std::atomic<std::size_t> last_activity_ms{0};
  std::string id, begin_id, begin_fingerprint, end_fingerprint, reason;
  std::size_t epoch = 0, reserved = 0;
  Json state, initial, start_response, end_response;
  std::unique_ptr<PolicyRuntime> policy;
  bool initial_saved = false;
  std::atomic<bool> capture_complete{false};
  std::atomic<unsigned> evaluation{0};
};

struct InFlight {
  Slot &slot;

  // 初始化 InFlight 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit InFlight(Slot &s) : slot(s) {
    ++slot.inflight;
  }

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~InFlight() {
    --slot.inflight;
  }
};
}

struct LocalRuntime::Impl {
  LocalRuntimeOptions options;
  std::shared_ptr<IntegrationService> service;
  std::string token, user, instance;
  Json config;
  std::shared_ptr<const Memory> memory;
  std::vector<std::shared_ptr<const Memory>> retired_memories;
  std::unique_ptr<Slot[]> slots;
  std::timed_mutex begins;
  std::mutex maintenance, learning;
  std::atomic<bool> stopping{false};
  std::atomic<bool> enabled{true};
  std::atomic<std::size_t> epoch{0};
  const pid_t owner_pid = ::getpid();
  std::atomic<std::size_t> calls{0}, degraded{0}, buffer_bytes{0}, published{0},
      persistence_errors{0}, learning_errors{0};
  std::thread writer, learner;
  std::size_t checkpointed = 0, generation = 0;
  std::uint64_t memory_version = 0, status_version = 0;
  std::atomic<std::size_t> memory_refreshes{0};

  // 初始化 Impl 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  Impl(const std::filesystem::path &path, std::string bearer, LocalRuntimeOptions limits)
      : options(limits),
        service(entry_trial() ? nullptr
                              : backend(isolated_trial() ? std::filesystem::path(std::getenv(
                                                               "TRAJECTORY_EVOLUTION_TRIAL_CONFIG"))
                                                         : path)),
        token(isolated_trial() ? std::getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN")
                               : std::move(bearer)) {
    require(options.max_request_bytes >= 1024 && options.max_request_bytes <= 1048576 &&
                options.max_buffer_bytes >= 1048576 && options.max_buffer_bytes <= 1073741824 &&
                options.max_pending_runs > 0 && options.max_pending_runs <= 1024 &&
                options.poll_interval_ms >= 1 && options.poll_interval_ms <= 1000 &&
                options.contention_budget_us <= 10000,
            "local_limits_invalid");
    if (!service) {
      // 该传输仅用于隔离试验，生产仍保持进程内调用。
      config["trial_url"] = std::getenv("TRAJECTORY_EVOLUTION_TRIAL_URL");
      static std::once_flag initialized;
      std::call_once(initialized,
                     [] { require(te_initialize() == 0, "transport_initialization_failed"); });
      return;
    }
    const auto env = Json::parse(service->local_environment(token));
    config = env["config"];
    user = env.str("user_id");
    options.max_request_bytes =
        std::min(options.max_request_bytes, config.num("max_request_bytes", 1048576));
    options.max_pending_runs =
        std::min(options.max_pending_runs, config.num("max_active_runs", 128));
    memory = std::make_shared<const Memory>(env);
    memory_version = env.num("memory_version", 0);
    ++memory_refreshes;
    unsigned char salt[32];
    require(RAND_bytes(salt, sizeof(salt)) == 1, "random_unavailable");
    instance = hash(std::string(reinterpret_cast<char *>(salt), sizeof(salt))).substr(0, 32);
    slots = std::make_unique<Slot[]>(options.max_pending_runs);
  }

  // 在有界容量内预留运行槽位，不能回收仍活动或仍在写入的槽位。
  bool reserve(std::size_t bytes) {
    auto current = buffer_bytes.load();
    for (unsigned i = 0; i < 4 && current + bytes <= options.max_buffer_bytes; ++i)
      if (buffer_bytes.compare_exchange_weak(current, current + bytes))
        return true;
    return false;
  }

  // 处理隔离评测环境中的本地打点，保持试验身份和采集归属。
  IntegrationResponse trial_invoke(const std::string &request) {
    require(::getpid() == owner_pid && request.size() <= options.max_request_bytes,
            "local_request_invalid");
    require(enabled, "local_disabled");
    const auto url = config.str("trial_url");
    te_client client{url.c_str(), token.c_str(), 5000, 2097152, 0};
    char *bytes = nullptr;
    long status = 0;
    const char *code = nullptr;
    const auto error = te_invoke(&client, request.c_str(), &bytes, &status, &code);
    std::unique_ptr<char, decltype(&te_free)> owned(bytes, &te_free);
    if (error || !bytes)
      return {503, R"({"ok":false,"error":"evaluation_capture_unavailable"})"};
    return {static_cast<int>(status), bytes};
  }

  // 根据认证身份计算当前数据和策略的隔离作用域。
  std::string scope(const std::string &name) const {
    return "agent:" + component(config.str("agent_id")) + "/actor:" + component(name);
  }

  // 构造保持原业务继续执行的降级响应，并记录采集不完整。
  IntegrationResponse fallback(const Json &request, const std::string &reason,
                               Slot *slot = nullptr) {
    ++degraded;
    if (slot) {
      ++slot->losses->count;
      slot->capture_complete = false;
      if (slot->reason.empty())
        slot->reason = reason;
    }
    const auto op = request.str("operation"), id = hash(instance + request.str("request_id"));
    Json result{{"degraded", true},
                {"reason", reason},
                {"capture_complete", false},
                {"policy_applied", false}};
    if (op == "run.begin")
      result["run_id"] = id.substr(0, 40);
    if (op == "activity.begin") {
      result["activity_id"] = "unobserved-" + id.substr(0, 28);
      result["decision"] = "allow";
      result["input"] = request["input"];
      result["event_id"] = "";
      result["context"] = Json{{"run_id", request["run_id"]},
                               {"activity_id", result["activity_id"]},
                               {"degraded", true}};
    }
    if (op == "loop.check") {
      result["control_id"] = "unobserved-" + id.substr(0, 28);
      result["decision"] = "continue";
      result["context"] = "";
      result["event_id"] = "";
    }
    if (op == "run.end" || op == "run.abort") {
      result["closed"] = false;
      result["evaluation_status"] = "quarantined";
    }
    if (op == "activity.end" || op == "activity.abort" || op == "loop.commit")
      result["ended"] = false;
    return {200, Json{{"ok", true}, {"result", result}}.dump_for_transport()};
  }

  // 创建本地运行上下文并绑定冻结策略，后续请求不重复编译策略。
  Json make_run(const Json &request, const std::string &rid, Slot &slot) {
    const auto start = boundary_begin_request(request, rid);
    const auto level_name = start.str("level");
    require(level_name.size() == 2 && level_name[0] == 'L' && level_name[1] >= '0' &&
                level_name[1] <= '3',
            "level_not_supported");
    const int level = level_name[1] - '0';
    require(level <= config.str("max_level")[1] - '0' && identifier(start.str("conversation_id")),
            "level_or_identity_invalid");
    const std::vector<std::string> caps{"model", "actions", "loop", "schedule"};
    std::set<std::string> declared;
    for (const auto &cap : start["capabilities"].items()) {
      require(std::find(caps.begin(), caps.end(), cap.text()) != caps.end(),
              "capability_not_supported");
      declared.insert(cap.text());
    }
    if (level > 0)
      require(declared.count(caps[level - 1]) || (level == 2 && declared.count("schedule")),
              "required_boundary_missing");
    Json run{{"kind", "integration-run-v1"},
             {"local_capture", true},
             {"identity", Json{{"agent_id", config["agent_id"]},
                               {"user_id", user},
                               {"run_id", rid},
                               {"conversation_id", start["conversation_id"]}}},
             {"input", start["input"]},
             {"level", level},
             {"capabilities", start["capabilities"]},
             {"closed", false},
             {"sequences", Json{{"task", 1}}},
             {"requests", Json::Object{}},
             {"receipt_counts", Json::Object{}},
             {"observed_boundaries", Json::Object{}},
             {"events", Json::Array{Json{{"event_id", "event-1"},
                                         {"producer_id", "task"},
                                         {"actor_id", "task"},
                                         {"sequence", "1"},
                                         {"timestamp_us", timestamp()},
                                         {"kind", "run.start"},
                                         {"input", start["input"]}}}}};
    initialize_boundary_run(run, request);
    if (config.has("offline_version")) {
      std::vector<std::string> scopes;
      for (const auto &name : config["actors"].items())
        scopes.push_back(scope(name.text()));
      apply_agent_version(run, config, std::atomic_load(&memory)->publications, nullptr, scopes);
      if (run.num("level", 0) > 0)
        slot.policy = std::make_unique<PolicyRuntime>(proto<v1::RuntimePolicy>(run["policy"]));
    } else if (level > 0) {
      auto shadow = proto<v1::ShadowOptions>(config["shadow_options"]);
      shadow.clear_target_scopes();
      for (const auto &name : config["actors"].items())
        shadow.add_target_scopes(scope(name.text()));
      auto cached = std::atomic_load(&memory);
      auto snapshot = compile_shadow_policy(
          cached->private_memory, proto<v1::Identity>(run["identity"]), shadow, cached->shared);
      auto opts = proto<v1::RuntimeOptions>(config["runtime_options"]);
      opts.set_requested_level(static_cast<v1::Level>(static_cast<int>(v1::L0) + level));
      opts.set_fallback("strict");
      opts.clear_allowed_users();
      opts.add_allowed_users(user);
      opts.clear_allowed_conversations();
      opts.add_allowed_conversations(start.str("conversation_id"));
      opts.clear_host_capabilities();
      for (std::size_t i = 0; i < caps.size(); ++i)
        if (declared.count(caps[i]))
          opts.add_host_capabilities(std::vector<std::string>{
              "context.request", "action.filter", "loop.decide", "schedule.dispatch"}[i]);
      if (config.flag("auto_approve_experiences", false)) {
        opts.clear_approved_experiences();
        for (const auto &experience : snapshot.experiences()) {
          auto *a = opts.add_approved_experiences();
          a->set_memory_id(experience.memory_id());
          a->set_content_revision(experience.content_revision());
        }
      }
      auto policy = compile_runtime_policy(std::move(snapshot), opts);
      apply_agent_version(run, config, cached->publications, &policy);
      if (run.num("level", 0) > 0)
        slot.policy = std::make_unique<PolicyRuntime>(proto<v1::RuntimePolicy>(run["policy"]));
    } else
      apply_agent_version(run, config, std::atomic_load(&memory)->publications);
    return run;
  }

  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  IntegrationResponse invoke(const std::string &bytes, const Json *parsed = nullptr,
                             bool point_call = false) {
    ++calls;
    Json request, reference;
    std::string operation, fingerprint_bytes = bytes;
    try {
      require(::getpid() == owner_pid, "local_process_changed");
      require(bytes.size() <= options.max_request_bytes, "local_request_too_large");
      request = parsed ? *parsed : Json::parse(bytes);
      require(request.object() && identifier(request.str("request_id")), "request_id_required");
      auto op = request.str("operation");
      operation = op;
      const bool referenced = op == "point" && request.has("ref");
      if (referenced) {
        request["request_id"] = point_request_key(request);
        reference = request;
        fingerprint_bytes = request.dump();
        if (request.str("kind") == "run" && request.str("phase") == "before") {
          Json empty;
          request = point_request(resolve_point_reference(reference, empty, ""), config);
          op = request.str("operation");
        } else {
          std::unique_lock<std::timed_mutex> begin(begins, std::defer_lock);
          require(begin.try_lock_for(std::chrono::microseconds(options.contention_budget_us)),
                  "local_busy");
          const auto begin_id = point_run_key(reference);
          for (std::size_t i = 0; i < options.max_pending_runs; ++i)
            if (slots[i].begin_id == begin_id && slots[i].phase != free_slot) {
              request["run_id"] = slots[i].id;
              break;
            }
          require(request.has("run_id"), "point_run_not_found");
        }
      }
      if (op == "contract.describe") {
        auto contract = describe_boundary_contract(config);
        auto &operations = contract["operations"].items();
        operations.erase(std::remove_if(operations.begin(), operations.end(),
                                        [](const auto &value) {
                                          return value.text() == "evaluation.retry" ||
                                                 value.text() == "memory.status" ||
                                                 value.text().rfind("agent_evaluation.", 0) == 0;
                                        }),
                         operations.end());
        contract["transport"] = "local.native.v1";
        contract["limits"]["max_request_bytes"] = options.max_request_bytes;
        return {200, Json{{"ok", true}, {"result", contract}}.dump()};
      }
      if (op == "run.begin") {
        if (!enabled)
          return fallback(request, "local_evolution_paused");
        std::unique_lock<std::timed_mutex> begin(begins, std::defer_lock);
        if (!begin.try_lock_for(std::chrono::microseconds(options.contention_budget_us)))
          return fallback(request, "local_busy");
        validate_local_data(request);
        require(bytes.find(token) == std::string::npos, "credential_value_rejected");
        for (std::size_t i = 0; i < options.max_pending_runs; ++i) {
          auto &slot = slots[i];
          // 只有 run.begin 在持有开始锁时写入 begin_id；
          // 不锁住无关活动运行，也不把繁忙的重复请求变成第二个运行。
          if (slot.begin_id != request.str("request_id") || slot.phase == free_slot)
            continue;
          std::unique_lock<std::timed_mutex> guard(slot.gate, std::defer_lock);
          if (!guard.try_lock_for(std::chrono::microseconds(options.contention_budget_us))) {
            ++std::atomic_load(&slot.losses)->count;
            return fallback(request, "local_busy");
          }
          if (slot.begin_fingerprint != hash(fingerprint_bytes))
            return fallback(request, "request_id_conflict", slot.phase == active ? &slot : nullptr);
          return {200, Json{{"ok", true}, {"result", slot.start_response}}.dump_for_transport()};
        }
        for (std::size_t i = 0; i < options.max_pending_runs; ++i) {
          auto &slot = slots[i];
          if (slot.phase != free_slot && slot.phase != persisted)
            continue;
          std::unique_lock<std::timed_mutex> guard(slot.gate, std::try_to_lock);
          if (!guard.owns_lock() || (slot.phase != free_slot && slot.phase != persisted))
            continue;
          // 预留新运行前回收已完成槽位的有界回放缓存，
          // 活动中或仍在写入的槽位不能在此回收。
          if (slot.phase == persisted) {
            buffer_bytes -= slot.reserved;
            slot.reserved = 0;
            slot.state = Json();
          }
          const auto cost = bytes.size() * 16 + 1048576;
          if (!reserve(cost))
            return fallback(request, "local_buffer_full");
          char prefix[9];
          std::snprintf(prefix, sizeof(prefix), "%08x", static_cast<unsigned>(i));
          const auto rid = std::string(prefix) + hash(instance + request.str("request_id") + ":" +
                                                      std::to_string(++generation))
                                                     .substr(0, 32);
          try {
            slot.policy.reset();
            auto state = make_run(request, rid, slot);
            slot.id = rid;
            slot.begin_id = request.str("request_id");
            slot.reason.clear();
            slot.last_activity_ms = monotonic_ms();
            slot.begin_fingerprint = hash(fingerprint_bytes);
            slot.end_fingerprint.clear();
            slot.epoch = epoch.load();
            std::atomic_store(&slot.losses, std::make_shared<CaptureLosses>(rid));
            slot.reserved = cost;
            slot.initial = state;
            slot.initial.fields().erase("policy");
            slot.initial_saved = false;
            slot.capture_complete = true;
            slot.end_response = nullptr;
            slot.evaluation = 0;
            slot.state = std::move(state);
            slot.start_response =
                Json{{"run_id", rid},
                     {"level", "L" + std::to_string(slot.state.num("level", 0))},
                     {"protocol", "evolution.boundary.v1"},
                     {"capabilities",
                      request.has("capabilities") ? request["capabilities"] : Json(Json::Array{})},
                     {"degraded", false}};
            if (slot.state.has("policy_gate"))
              slot.start_response["policy_gate"] = slot.state["policy_gate"];
            if (slot.state.has("agent_version_id"))
              slot.start_response["agent_version_id"] = slot.state["agent_version_id"];
            if (referenced) {
              slot.start_response = point_result(reference, request, slot.start_response);
              record_point_reference(slot.state, reference, slot.start_response);
              slot.initial["point_bindings"] = slot.state["point_bindings"];
            }
            slot.phase = active;
            return {200, Json{{"ok", true}, {"result", slot.start_response}}.dump_for_transport()};
          } catch (...) {
            buffer_bytes -= cost;
            throw;
          }
        }
        return fallback(request, "local_capacity_reached");
      }
      const auto rid = request.str("run_id");
      require(rid.size() == 40 && rid.find_first_not_of("0123456789abcdef") == std::string::npos,
              "run_not_local");
      const auto index = std::stoul(rid.substr(0, 8), nullptr, 16);
      require(index < options.max_pending_runs, "run_not_local");
      auto &slot = slots[index];
      InFlight flight(slot);
      // 槽位可以复用，但锁竞争造成的缺口属于不可变运行身份，
      // 不能归给后来碰巧占用该槽位的另一运行。
      const auto losses = std::atomic_load(&slot.losses);
      require(losses && losses->run_id == rid, "run_not_local");
      std::unique_lock<std::timed_mutex> guard(slot.gate, std::defer_lock);
      if (!guard.try_lock_for(std::chrono::microseconds(options.contention_budget_us))) {
        if (slot.phase == active && op != "run.status" && op != "activity.status")
          ++losses->count;
        return fallback(request, "local_busy");
      }
      require(slot.phase != free_slot && slot.id == rid, "run_not_local");
      if ((referenced || slot.phase != active) &&
          slot.state["requests"].has(request.str("request_id"))) {
        const auto &previous = slot.state["requests"][request.str("request_id")];
        if (previous.str("fingerprint") != hash(fingerprint_bytes))
          return fallback(referenced ? reference : request, "request_id_conflict",
                          slot.phase == active ? &slot : nullptr);
        return {200, Json{{"ok", true}, {"result", previous["response"]}}.dump_for_transport()};
      }
      if (op == "run.status") {
        static const char *statuses[] = {"not_scheduled", "pending",  "running",    "completed",
                                         "failed",        "disabled", "quarantined"};
        Json result{{"run_id", rid},
                    {"closed", slot.phase != active},
                    {"persisted", slot.phase == persisted},
                    {"capture_complete", slot.capture_complete.load() && slot.losses->count == 0 &&
                                             slot.epoch == epoch.load()},
                    {"degraded", slot.losses->count != 0 || slot.epoch != epoch.load()},
                    {"evaluation_status", statuses[slot.evaluation.load()]}};
        return {200, Json{{"ok", true}, {"result", result}}.dump()};
      }
      if (slot.phase != active) {
        if ((op == "run.end" || op == "run.abort" ||
             (referenced && reference.str("kind") == "run")) &&
            !slot.end_response.null() && slot.end_fingerprint == hash(fingerprint_bytes))
          return {200, Json{{"ok", true}, {"result", slot.end_response}}.dump_for_transport()};
        return fallback(request, "local_run_closed");
      }
      try {
        require(enabled.load(), "local_evolution_paused");
        validate_local_data(request);
        require(bytes.find(token) == std::string::npos, "credential_value_rejected");
        const auto key = request.str("request_id"), fingerprint = hash(fingerprint_bytes);
        if (slot.state["requests"].has(key)) {
          const auto &previous = slot.state["requests"][key];
          require(previous.str("fingerprint") == fingerprint, "request_id_conflict");
          return {200, Json{{"ok", true}, {"result", previous["response"]}}.dump_for_transport()};
        }
        require((referenced || slot.losses->count == 0) && slot.epoch == epoch.load(),
                "local_capture_degraded");
        if (referenced && slot.losses->count != 0)
          slot.state["incomplete"] = true;
        const auto cost = bytes.size() * 16 + 4096;
        require(slot.reserved + cost <= config.num("max_run_bytes", 16777216) && reserve(cost),
                "local_buffer_full");
        slot.reserved += cost;
        if (op == "capture.gap")
          throw Error("capture_gap_reported");
        auto result =
            referenced ? dispatch_referenced_point(slot.state, reference, config, slot.policy.get())
                       : dispatch_boundary(slot.state, request, config, slot.policy.get());
        slot.last_activity_ms = monotonic_ms();
        result["degraded"] = false;
        slot.state["requests"][key] = Json{{"fingerprint", fingerprint}, {"response", result}};
        if (slot.state.flag("closed", false)) {
          slot.capture_complete = !slot.state.flag("incomplete", false);
          slot.evaluation =
              !slot.capture_complete ? 6 : (config.flag("auto_evaluate", true) ? 1 : 5);
          slot.end_fingerprint = fingerprint;
          result["persisted"] = false;
          slot.end_response = result;
          slot.phase = closed;
          if (referenced) {
            auto terminal = reference;
            terminal["phase"] = "after";
            terminal["kind"] = "run";
            const auto end_key = point_request_key(terminal);
            if (slot.state["requests"].has(end_key)) {
              slot.end_response = slot.state["requests"][end_key]["response"];
              slot.end_fingerprint = slot.state["requests"][end_key].str("fingerprint");
            }
          }
        }
        return {200, Json{{"ok", true}, {"result", result}}.dump_for_transport()};
      } catch (const Error &error) {
        auto reply = fallback(request, error.what(), &slot);
        if (referenced) {
          abort_point_reference(slot.state, reference, config, slot.policy.get());
          if (slot.state.flag("closed", false)) {
            slot.phase = closed;
            slot.evaluation = 6;
          }
        }
        if (op == "run.end" || op == "run.abort" ||
            (referenced && reference.str("kind") == "run" &&
             (reference.str("phase") == "after" || reference.str("phase") == "abort"))) {
          slot.end_fingerprint = hash(fingerprint_bytes);
          slot.end_response = Json::parse(reply.body)["result"];
          slot.phase = closed;
          slot.evaluation = 6;
        }
        return reply;
      } catch (...) {
        auto reply = fallback(request, "local_boundary_failed", &slot);
        if (referenced) {
          abort_point_reference(slot.state, reference, config, slot.policy.get());
          if (slot.state.flag("closed", false)) {
            slot.phase = closed;
            slot.evaluation = 6;
          }
        }
        if (op == "run.end" || op == "run.abort" ||
            (referenced && reference.str("kind") == "run" &&
             (reference.str("phase") == "after" || reference.str("phase") == "abort"))) {
          slot.end_fingerprint = hash(fingerprint_bytes);
          slot.end_response = Json::parse(reply.body)["result"];
          slot.phase = closed;
          slot.evaluation = 6;
        }
        return reply;
      }
    } catch (const Error &error) {
      if (!point_call && operation != "run.begin" && operation != "contract.describe" &&
          operation != "capture.gap")
        ++epoch;
      return fallback(request, error.what());
    } catch (...) {
      if (!point_call && operation != "run.begin" && operation != "contract.describe" &&
          operation != "capture.gap")
        ++epoch;
      return fallback(request, "local_unavailable");
    }
  }

  // 读取组件自身的采集和排队指标，不把观测指标当作业务任务得分。
  std::string metrics() const {
    return Json{{"transport", "local.native.v1"},
                {"enabled", enabled.load()},
                {"calls", calls.load()},
                {"degraded_calls", degraded.load()},
                {"buffer_bytes", buffer_bytes.load()},
                {"published_runs", published.load()},
                {"persistence_errors", persistence_errors.load()},
                {"learning_errors", learning_errors.load()},
                {"memory_refreshes", memory_refreshes.load()}}
        .dump();
  }

  // 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
  void drain() {
    std::unique_lock<std::mutex> single(maintenance, std::try_to_lock);
    if (!single.owns_lock())
      return;
    for (std::size_t i = 0; i < options.max_pending_runs; ++i) {
      auto &slot = slots[i];
      Json data;
      bool final = false;
      if (slot.phase == active &&
          monotonic_ms() - slot.last_activity_ms.load() >= config.num("run_lease_ms", 86400000)) {
        std::unique_lock<std::timed_mutex> guard(slot.gate, std::try_to_lock);
        if (guard.owns_lock() && slot.phase == active &&
            monotonic_ms() - slot.last_activity_ms.load() >= config.num("run_lease_ms", 86400000)) {
          ++slot.losses->count;
          slot.reason = "run_lease_expired";
          slot.capture_complete = false;
          slot.phase = closed;
        }
      }
      if (slot.phase == active) {
        // 活动状态发布后，初始状态保持不可变。
        // 只有维护写入器能持久化或回收它，不能与业务打点竞争。
        if (slot.initial_saved)
          continue;
        data = slot.initial;
      } else {
        if (slot.phase != closed)
          continue;
        std::unique_lock<std::timed_mutex> guard(slot.gate, std::try_to_lock);
        if (!guard.owns_lock() || slot.inflight != 0 || slot.phase != closed)
          continue;
        auto receipts = slot.state["requests"];
        final = true;
        data = std::move(slot.state);
        slot.state = Json::Object{};
        slot.state["requests"] = std::move(receipts);
        slot.phase = writing;
      }
      try {
        if (final && (slot.losses->count != 0 || slot.epoch != epoch.load())) {
          data["incomplete"] = true;
          data["local_degradation_reason"] = slot.reason.empty() ? "local_busy" : slot.reason;
          if (!data.flag("closed", false)) {
            auto unlimited = config;
            unlimited["max_events"] = config.num("max_events", 10000) * 8 + 32;
            dispatch_boundary(
                data, Json{{"operation", "run.abort"}, {"status", "unknown"}, {"output", nullptr}},
                unlimited, slot.policy.get());
          }
          data["evaluation_status"] = "quarantined";
          slot.evaluation = 6;
        }
        const auto response = service->persist_local(token, data.dump());
        require(response.status == 200, "local_persistence_failed");
        if (final) {
          const bool complete = !data.flag("incomplete", false);
          const auto replay_bytes = data["requests"].dump().size() * 4 + 4096;
          Json retired;
          std::unique_ptr<PolicyRuntime> old_policy;
          {
            std::lock_guard<std::timed_mutex> guard(slot.gate);
            const auto retained = std::min(slot.reserved, replay_bytes);
            slot.capture_complete = complete;
            buffer_bytes -= slot.reserved - retained;
            slot.reserved = retained;
            retired = std::move(slot.initial);
            old_policy = std::move(slot.policy);
            slot.phase = persisted;
            ++published;
          }
          service->wake_background();
        } else
          slot.initial_saved = true;
      } catch (...) {
        ++persistence_errors;
        if (final) {
          std::lock_guard<std::timed_mutex> guard(slot.gate);
          slot.state = std::move(data);
          slot.phase = closed;
        }
      }
    }
    const auto lost = degraded.load();
    if (lost != checkpointed) {
      try {
        service->checkpoint_local(token, instance, metrics());
        checkpointed = lost;
      } catch (...) {
        ++persistence_errors;
      }
    }
  }

  // 后台尝试处理待评测任务，再按版本变化刷新只读记忆快照和运行状态。
  // learning 使用非阻塞加锁，已有学习任务时直接返回；热路径不会等待整轮自进化。
  void evaluate(bool flush = false) {
    std::unique_lock<std::mutex> single(learning, std::try_to_lock);
    if (!single.owns_lock())
      return;
    try {
      service->process_evaluations(flush);
      const auto env = Json::parse(service->local_environment(token, memory_version));
      if (env.has("memory")) {
        auto next = std::make_shared<const Memory>(env);
        retired_memories.push_back(std::atomic_exchange(&memory, std::move(next)));
        memory_version = env.num("memory_version", 0);
        ++memory_refreshes;
      }
      retired_memories.erase(
          std::remove_if(retired_memories.begin(), retired_memories.end(),
                         [](const auto &value) { return value.use_count() == 1; }),
          retired_memories.end());
      if (status_version == env.num("work_version", 0))
        return;
      bool retry_status = false;
      for (std::size_t i = 0; i < options.max_pending_runs; ++i) {
        auto &slot = slots[i];
        std::string rid;
        if (slot.phase != persisted || (slot.evaluation != 1 && slot.evaluation != 2))
          continue;
        {
          std::unique_lock<std::timed_mutex> guard(slot.gate, std::try_to_lock);
          if (!guard.owns_lock()) {
            retry_status = true;
            continue;
          }
          if (slot.phase != persisted || (slot.evaluation != 1 && slot.evaluation != 2))
            continue;
          rid = slot.id;
        }
        const auto response = service->call(token, Json{{"operation", "run.status"},
                                                        {"request_id", "local-background-status"},
                                                        {"run_id", rid}}
                                                       .dump());
        if (response.status != 200) {
          retry_status = true;
          continue;
        }
        const auto status = Json::parse(response.body)["result"].str("evaluation_status");
        const std::vector<std::string> names{"not_scheduled", "pending",  "running",    "completed",
                                             "failed",        "disabled", "quarantined"};
        const auto found = std::find(names.begin(), names.end(), status);
        if (slot.phase != persisted)
          continue;
        std::unique_lock<std::timed_mutex> guard(slot.gate, std::try_to_lock);
        if (!guard.owns_lock())
          retry_status = true;
        else if (slot.id == rid && found != names.end())
          slot.evaluation = static_cast<unsigned>(found - names.begin());
      }
      if (!retry_status)
        status_version = env.num("work_version", 0);
    } catch (...) {
      ++learning_errors;
    }
  }
};

// 初始化 LocalRuntime 的状态和资源；输入参数的校验与所有权在构造阶段确定。
// 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
LocalRuntime::LocalRuntime(const std::filesystem::path &path, const std::string &token,
                           LocalRuntimeOptions options)
    : impl_(std::make_unique<Impl>(path, token, options)) {
  if (!impl_->service)
    return;
  if (options.background) {
    impl_->writer = std::thread([this] {
      while (!impl_->stopping) {
        try {
          impl_->drain();
        } catch (...) {
          ++impl_->persistence_errors;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(impl_->options.poll_interval_ms));
      }
    });
    try {
      impl_->learner = std::thread([this] {
        while (!impl_->stopping) {
          const auto version = impl_->service->work_version();
          impl_->evaluate();
          if (!impl_->stopping)
            impl_->service->wait_for_work(version, 30000);
        }
      });
    } catch (...) {
      impl_->stopping = true;
      impl_->writer.join();
      throw;
    }
  }
}

// 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
LocalRuntime::~LocalRuntime() {
  if (!impl_->service)
    return;
  impl_->stopping = true;
  impl_->service->wake_background();
  if (impl_->writer.joinable())
    impl_->writer.join();
  if (impl_->learner.joinable())
    impl_->learner.join();
  // 关闭操作不在业务边界执行，未完成工作不能进入学习。
  for (std::size_t i = 0; i < impl_->options.max_pending_runs; ++i) {
    auto &slot = impl_->slots[i];
    if (slot.phase == active) {
      ++slot.losses->count;
      slot.reason = "producer_shutdown";
      slot.phase = closed;
    }
  }
  try {
    impl_->drain();
  } catch (...) {
  }
}

// 执行一次组件协议调用，检查响应信封并保持请求身份一致。
IntegrationResponse LocalRuntime::invoke(const std::string &request) noexcept {
  try {
    if (!impl_->service)
      return impl_->trial_invoke(request);
    if (request.size() <= impl_->options.max_request_bytes) {
      integration::Json point;
      try {
        point = integration::Json::parse(request);
      } catch (...) {
        return impl_->invoke(request);
      }
      if (point["operation"].string() && point["operation"].text() == "point") {
        try {
          if (point.has("ref")) {
            auto response = impl_->invoke(request, &point, true);
            auto body = integration::Json::parse(response.body);
            if (body["result"].flag("degraded", false)) {
              body["result"] = integration::point_fallback(
                  point, body["result"].str("reason", "point_unavailable"));
              response.body = body.dump_for_transport();
            }
            return response;
          }
          require(!point["context"].flag("degraded", false), "upstream_capture_degraded");
          const auto packet = integration::point_request(point, impl_->config);
          auto response = impl_->invoke(packet.dump(), &packet, true);
          auto body = integration::Json::parse(response.body);
          if (body.flag("ok", false))
            body["result"] = integration::point_result(point, packet, body["result"]);
          response.body = body.dump_for_transport();
          return response;
        } catch (...) {
          const auto &parent = point["context"];
          const auto rid = parent["run_id"].string() ? parent["run_id"].text() : "";
          const auto request_id =
              point["request_id"].string() ? point["request_id"].text() : "invalid";
          if (!rid.empty()) {
            impl_->invoke(Json{{"operation", "capture.gap"},
                               {"request_id", "gap-" + hash(request_id).substr(0, 32)},
                               {"run_id", rid},
                               {"reason", "missing_boundary"}}
                              .dump(),
                          nullptr, true);
            const auto kind = point["kind"].string()    ? point["kind"].text()
                              : parent["kind"].string() ? parent["kind"].text()
                                                        : "custom";
            const auto phase = point["phase"].string() ? point["phase"].text() : "invalid";
            if (kind == "run" && (phase == "after" || phase == "abort"))
              impl_->invoke(Json{{"operation", "run.abort"},
                                 {"request_id", "abort-" + hash(request_id).substr(0, 32)},
                                 {"run_id", rid},
                                 {"status", "unknown"},
                                 {"output", nullptr}}
                                .dump(),
                            nullptr, true);
          }
          return {200,
                  integration::Json{{"ok", true},
                                    {"result", integration::point_fallback(point, "point_invalid")}}
                      .dump_for_transport()};
        }
      }
      return impl_->invoke(request, &point);
    }
    return impl_->invoke(request);
  } catch (...) {
    return {503, R"({"ok":false,"error":"local_unavailable"})"};
  }
}

// 读取组件自身的采集和排队指标，不把观测指标当作业务任务得分。
std::string LocalRuntime::metrics() const {
  return impl_->service ? impl_->metrics() : R"({"evaluation_remote":true})";
}

// 执行评测等阻塞式管理操作，与主 Agent 热路径分离。
IntegrationResponse LocalRuntime::manage(const std::string &request) noexcept {
  try {
    if (!impl_->service)
      return impl_->trial_invoke(request);
    require(::getpid() == impl_->owner_pid, "local_process_changed");
    require(request.size() <= impl_->options.max_request_bytes, "local_request_too_large", 413);
    const auto message = Json::parse(request);
    require(message.str("operation").rfind("agent_evaluation.", 0) == 0,
            "management_operation_required");
    return impl_->service->call(impl_->token, request);
  } catch (const Error &error) {
    return {error.status, Json{{"ok", false}, {"error", error.what()}}.dump()};
  } catch (...) {
    return {503, R"({"ok":false,"error":"local_management_unavailable"})"};
  }
}

// 切换运行时是否启用采集或注入；关闭时仍按协议保持安全降级。
void LocalRuntime::set_enabled(bool enabled) noexcept {
  if (impl_->enabled.exchange(enabled) != enabled)
    ++impl_->epoch;
}

// 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
void LocalRuntime::drain() {
  if (impl_->service)
    impl_->drain();
}

// 显式处理待评测工作，只用于后台或维护调用，不阻塞主 Agent 边界。
void LocalRuntime::evaluate_pending() {
  if (impl_->service)
    impl_->evaluate(true);
}
} // 命名空间结束：trajectory::evolution
