// 逐题运行真实宿主、采集轨迹并统一检查；L0 基线不加载进化策略。
#include "trajectory_evolution/task_evaluation.h"
#include "trajectory_evolution/trace.h"
#include "trajectory_evolution/integration.h"
#include "agentflow/trajectory/runtime_config.h"
#include "evaluation_process.h"
#include "evaluation_entry.h"
#include "integration_json.h"
#include "boundary_runtime.h"
#include "local_support.h"
#include "trace_internal.h"
#include "model_json.h"
#include "model_input.h"
#include "evaluation_contracts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <fnmatch.h>
#include <map>
#include <mutex>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <regex>
#include <rapidjson/error/en.h>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <unistd.h>
#ifdef EVOLUTION_HAS_PNG
#include <png.h>
#include <zlib.h>
#endif

namespace trajectory::evolution {
namespace {
namespace fs = std::filesystem;
namespace dv = dataset::v1;
namespace legacy = agentflow::trajectory;
using integration::Json;
using integration::Error;
using integration::require;

std::string hash(const std::string& bytes) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
  std::string out;
  for(auto value : digest) { out += "0123456789abcdef"[value >> 4]; out += "0123456789abcdef"[value & 15]; }
  return out;
}

std::string read_file(const fs::path& path, std::size_t limit) {
  require(fs::is_regular_file(path) && !fs::is_symlink(path), "evaluation_file_not_regular");
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(stream.good() && stream.tellg() >= 0
      && static_cast<std::uint64_t>(stream.tellg()) <= limit, "evaluation_file_size_invalid");
  std::string bytes(static_cast<std::size_t>(stream.tellg()), '\0');
  stream.seekg(0);
  require(bytes.empty() || bool(stream.read(bytes.data(), bytes.size())), "evaluation_read_failed");
  return bytes;
}

void write_file(const fs::path& path, const std::string& bytes) {
  require(!fs::exists(fs::symlink_status(path)), "evaluation_file_exists");
  std::ofstream stream(path, std::ios::binary);
  require(bool(stream.write(bytes.data(), bytes.size()).flush()), "evaluation_write_failed");
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
}

void make_directory(const fs::path& path) {
  require(fs::create_directory(path), "evaluation_directory_exists");
  fs::permissions(path, fs::perms::owner_all);
}

void safe(const Json& value) { integration::validate_local_data(value); }

void label(const std::string& value) {
  require(integration::identifier(value) && value.find('\0') == std::string::npos,
          "evaluation_identifier_invalid");
  safe(Json(value));
}

std::size_t bounded(const Json& value, const std::string& name, std::size_t fallback,
                    std::size_t minimum, std::size_t maximum) {
  const auto result = value.num(name, fallback);
  require(result >= minimum && result <= maximum, "evaluation_limit_invalid");
  return result;
}

// 支持 RFC 6901 指针（含转义名称与数组）。字段缺失不同于显式 JSON null；
// 此处不执行表达式，也不进行正则求值。
const Json* pointer(const Json& value, const std::string& path) {
  if(path.empty()) return &value;
  require(path.front() == '/' && path.size() <= 2048, "evaluation_pointer_invalid");
  const Json* current = &value;
  std::size_t begin = 1;
  while(true) {
    const auto end = path.find('/', begin);
    const auto raw = path.substr(begin, end == std::string::npos ? end : end - begin);
    std::string key;
    for(std::size_t i = 0; i < raw.size(); ++i) {
      if(raw[i] != '~') key += raw[i];
      else {
        require(++i < raw.size() && (raw[i] == '0' || raw[i] == '1'), "evaluation_pointer_invalid");
        key += raw[i] == '0' ? '~' : '/';
      }
    }
    if(current) {
      if(current->object()) current = current->has(key) ? &(*current)[key] : nullptr;
      else if(current->array()) {
        std::size_t index = 0;
        bool valid = !key.empty() && (key == "0" || key.front() != '0') && key.size() <= 8;
        for(unsigned char c : key) { valid &= c >= '0' && c <= '9'; if(valid) index = index * 10 + c - '0'; }
        current = valid && index < current->items().size() ? &current->items()[index] : nullptr;
      } else current = nullptr;
    }
    if(end == std::string::npos) return current;
    begin = end + 1;
  }
}

double number(const Json& value) {
  require(std::holds_alternative<double>(value.value), "evaluation_number_required");
  return std::get<double>(value.value);
}

void validate_check(const Json& check) {
  if(check.str("op") == "action_count") {
    integration::keys(check, {"id", "op", "operation", "min", "max", "status"});
    label(check.str("id"));
    require(!check.str("operation").empty() && check.str("operation").size() <= 1024, "evaluation_check_operation_name_invalid");
    require(check.has("min") || check.has("max"), "evaluation_check_count_bounds_required");
    const auto minimum = bounded(check, "min", 0, 0, 1000000), maximum = bounded(check, "max", 1000000, 0, 1000000);
    require(minimum <= maximum, "evaluation_check_count_bounds_invalid");
    const auto status = check.str("status");
    require(status.empty() || status == "completed" || status == "failed" || status == "cancelled", "evaluation_check_status_invalid");
    safe(check); return;
  }
  integration::keys(check, {"id", "path", "op", "value"});
  label(check.str("id"));
  pointer(Json(), check.str("path"));
  const auto op = check.str("op");
  require(op == "exists" || op == "equals" || op == "contains" || op == "at_most" || op == "at_least",
          "evaluation_check_operation_invalid");
  require(check.has("value") == (op != "exists"), "evaluation_check_value_invalid");
  if(op == "at_most" || op == "at_least") number(check["value"]);
  safe(check);
}

std::size_t action_count(const Json& check, const Json& evidence) {
  require(!(evidence.has("trace") && evidence.has("traces")), "evaluation_action_evidence_invalid");
  const auto traces = evidence.has("traces") ? evidence["traces"]
      : evidence.has("trace") ? Json(Json::Array{evidence["trace"]}) : Json(Json::Array{});
  require(traces.array() && !traces.items().empty(), "evaluation_action_evidence_unavailable");
  std::set<std::string> runs;
  std::size_t count = 0;
  for(const auto& raw : traces.items()) {
    const auto validated = validate_trace(parse_trace_json(raw.dump()));
    require(validated.report.trace_valid() && runs.insert(validated.trace.identity().run_id()).second,
        "evaluation_action_evidence_invalid");
    using Key = std::pair<std::string, std::string>;
    std::map<Key, const v1::TraceEvent*> starts;
    std::set<Key> ended;
    for(const auto& event : validated.trace.events()) if(event.kind() == "action.start") {
      require(!event.action_id().empty() && !event.operation().empty()
          && starts.emplace(Key{event.execution_id(), event.action_id()}, &event).second, "evaluation_action_evidence_invalid");
    }
    for(const auto& event : validated.trace.events()) if(event.kind() == "action.end") {
      const Key key{event.execution_id(), event.action_id()};
      const auto found = starts.find(key);
      require(found != starts.end() && ended.insert(key).second, "evaluation_action_evidence_invalid");
      require(event.operation().empty() || event.operation() == found->second->operation(), "evaluation_action_evidence_invalid");
      if(found->second->operation() != check.str("operation")) continue;
      const auto attributes = integration::json(event.attributes());
      require(attributes.has("executed"), "evaluation_action_evidence_unavailable");
      // 统计真实观测，而不是建议调用、起止配对或成功产物。执行身份只在各 run
      // 与并发作用域内有效，不能跨作用域合并。
      if(attributes.flag("executed", false) && (check.str("status").empty() || event.status() == check.str("status"))) ++count;
    }
    require(ended.size() == starts.size(), "evaluation_action_evidence_unavailable");
  }
  return count;
}

dv::TrialCheck grade_check(const Json& check, const Json& evidence) {
  dv::TrialCheck result;
  result.set_check_id(check.str("id"));
  if(check.str("op") == "action_count") {
    try {
      const auto count = action_count(check, evidence);
      const auto minimum = check.num("min", 0), maximum = check.num("max", 1000000);
      result.set_status(count >= minimum && count <= maximum ? "passed" : "failed");
      result.set_reason("实际执行 " + check.str("operation") + " " + std::to_string(count) + " 次；"
          + (check.has("min") ? "至少 " + std::to_string(minimum) + " 次；" : "")
          + (check.has("max") ? "最多 " + std::to_string(maximum) + " 次。" : ""));
      result.add_evidence_paths(evidence.has("traces") ? "/traces" : "/trace");
    } catch(...) {
      result.set_status("error"); result.set_reason("执行证据不完整或缺少实际执行标记，无法核验调用次数。");
    }
    return result;
  }
  const auto path = check.str("path");
  const auto* actual = pointer(evidence, path);
  const auto op = check.str("op");
  bool passed = actual != nullptr;
  if(passed && op == "equals") passed = actual->dump() == check["value"].dump();
  else if(passed && op == "contains") {
    if(actual->string() && check["value"].string())
      passed = actual->text().find(check["value"].text()) != std::string::npos;
    else if(actual->array()) passed = std::any_of(actual->items().begin(), actual->items().end(),
        [&](const Json& item) { return item.dump() == check["value"].dump(); });
    else passed = false;
  } else if(passed && (op == "at_most" || op == "at_least")) {
    passed = std::holds_alternative<double>(actual->value);
    if(passed) passed = op == "at_most" ? number(*actual) <= number(check["value"])
        : number(*actual) >= number(check["value"]);
  }
  result.set_status(passed ? "passed" : "failed");
  result.set_reason(passed ? "确定性检查通过。" : actual ? "实际证据不满足预设条件。" : "缺少必需的结果证据。");
  if(actual) result.add_evidence_paths(path);
  return result;
}

std::vector<std::string> command(const Json& config, const fs::path& config_dir) {
  std::vector<std::string> result;
  for(const auto& entry : config["command"].items()) {
    auto item = entry.text();
    require(!item.empty() && item.size() <= 8192 && item.find('\0') == std::string::npos,
            "evaluation_command_invalid");
    const std::string marker = "{config_dir}";
    for(std::size_t pos = 0; (pos = item.find(marker, pos)) != std::string::npos;) {
      item.replace(pos, marker.size(), config_dir.string()); pos += config_dir.string().size();
    }
    // 命令行参数不是传递凭据的可接受通道。
    safe(Json(item));
    result.push_back(std::move(item));
  }
  require(!result.empty() && result.size() <= 64, "evaluation_command_invalid");
  return result;
}

std::vector<std::string> environment(const Json& config) {
  std::set<std::string> names{"PATH", "LANG", "LC_ALL", "TZ"};
  if(config.has("pass_env")) for(const auto& entry : config["pass_env"].items()) {
    const auto name = entry.text();
    require(!name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    }), "evaluation_environment_name_invalid");
    names.insert(name);
  }
  require(names.size() <= 68, "evaluation_environment_limit");
  std::vector<std::string> out;
  for(const auto& name : names) if(const auto* value = std::getenv(name.c_str())) out.push_back(name + "=" + value);
  return out;
}

struct Variant {
  std::string id;
  std::vector<std::string> command, environment;
  std::string version;
  Json artifact;
  Json instrumentation_config;
  std::string instrumentation_transport = "native";
  std::string run_layout = "single";
  Json entry = nullptr;
  bool published_policy = false;
};

// 评测器负责隔离、版本与轨迹组装；宿主只负责发出打点。
std::vector<std::string> instrumented_environment(const Variant& variant, const fs::path& workspace, const std::string& trial, const Json& request) {
  auto config = variant.instrumentation_config;
  config["data_dir"] = (workspace / "capture").string();
  config["evaluation_version_file"] = (workspace / "version.json").string();
  config["auto_evaluate"] = false; config["sharing"] = Json{{"enabled", false}};
  config["port"] = 0;
  config.fields().erase("policy_evaluation_file");
  config.fields().erase("policy_version_file");
  config["instrumentation"]["level"] = variant.artifact["level"];
  config["principals"] = Json::Array{Json{{"user_id", variant.artifact["user_id"]}, {"token_env", "TRAJECTORY_EVOLUTION_TRIAL_TOKEN"}}};
  Json version{{"version", variant.version}, {"artifact", variant.artifact}, {"trial_id", trial}};
  if(variant.published_policy) version["published_policy"] = true;
  if(!variant.entry.null()) {
    const auto& state = request["task"]["initial_state"];
    require(state.null() || (state.object() && state.fields().empty())
        || integration::EvaluationEntry::uses(variant.entry["input"], "initial_state"), "evaluation_entry_initial_state_unbound");
    version["entry"] = Json{{"binding", variant.entry}, {"task", request["task"]},
        {"trial_id", trial}, {"user_id", variant.artifact["user_id"]}};
    require(version.dump().size() <= 2097152, "evaluation_entry_input_limit");
  }
  write_file(workspace / "version.json", version.dump());
  write_file(workspace / "instrumentation.json", config.dump());
  unsigned char salt[32]; require(RAND_bytes(salt, sizeof(salt)) == 1, "evaluation_secret_unavailable");
  const auto token = hash(std::string(reinterpret_cast<const char*>(salt), sizeof(salt)));
  auto env = variant.environment;
  // 环境变量值只留在进程内存中，绝不写入配置或报告。
  const std::string config_key = "TRAJECTORY_EVOLUTION_TRIAL_CONFIG=", token_key = "TRAJECTORY_EVOLUTION_TRIAL_TOKEN=";
  env.erase(std::remove_if(env.begin(), env.end(), [&](const auto& entry) {
    return entry.rfind("TRAJECTORY_EVOLUTION_TRIAL_", 0) == 0
        || entry.rfind("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY=", 0) == 0;
  }), env.end());
  env.push_back(config_key + (workspace / "instrumentation.json").string()); env.push_back(token_key + token);
  if(!variant.entry.null()) env.push_back("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY=1");
  return env;
}

Json instrumented_run(const Variant& variant, const fs::path& workspace, const std::string& trial, std::size_t max_bytes) {
  const auto folder = workspace / "capture/runs";
  require(fs::is_directory(folder) && !fs::is_symlink(folder), "evaluation_trace_missing");
  Json runs = Json::Array{}; std::size_t count = 0, bytes = 0;
  for(const auto& entry : fs::directory_iterator(folder)) {
    require(++count <= 1024, "evaluation_run_limit");
    if(entry.path().extension() != ".json") continue;
    require(fs::is_regular_file(entry.path()) && !fs::is_symlink(entry.path()), "evaluation_file_not_regular");
    require(fs::file_size(entry.path()) <= max_bytes - bytes, "evaluation_trace_bytes_limit");
    const auto content = read_file(entry.path(), max_bytes - bytes);
    bytes += content.size();
    const auto item = Json::parse(content);
    if(item.str("evaluation_trial_id") != trial) continue;
    require(item.str("agent_version_id") == variant.version && item["identity"].str("user_id") == variant.artifact.str("user_id"),
        "evaluation_loaded_version_mismatch");
    require(item["events"].array() && !item["events"].items().empty(), "evaluation_trace_missing");
    runs.push(item);
  }
  require(!runs.items().empty(), "evaluation_trace_missing");
  // 目录遍历顺序不等于对话顺序。必须保留每个真实 run；多轮试验不能伪造为一条
  // 合成执行轨迹。
  std::sort(runs.items().begin(), runs.items().end(), [](const Json& a, const Json& b) {
    const auto at = a["events"].items().front().str("timestamp_us");
    const auto bt = b["events"].items().front().str("timestamp_us");
    return at == bt ? a["identity"].str("run_id") < b["identity"].str("run_id") : at < bt;
  });
  return runs;
}

Json instrumented_response(const Variant& variant, const fs::path& workspace, const std::string& trial,
                           std::size_t max_bytes, std::size_t expected_runs) {
  const auto runs = instrumented_run(variant, workspace, trial, max_bytes);
  require(runs.items().size() == expected_runs, "evaluation_run_count_mismatch");
  Json result{{"schema_version", 1}, {"trial_id", trial}, {"loaded_version", variant.version},
    {"execution_status", "completed"}};
  Json traces = Json::Array{}, outputs = Json::Array{}, files = Json::Array{};
  for(const auto& run : runs.items()) {
    require(run.flag("closed", false) && !run.flag("incomplete", false), "evaluation_trace_incomplete");
    const auto& terminal = run["events"].items().back();
    require(terminal.str("kind") == "run.end", "evaluation_terminal_missing");
    const auto status = terminal["outcome"].str("execution_status");
    require(status == "completed" || status == "failed" || status == "cancelled", "evaluation_execution_status_invalid");
    if(status == "cancelled" || (status == "failed" && result.str("execution_status") == "completed"))
      result["execution_status"] = status;
    const auto& output = terminal["output"];
    outputs.push(output); traces.push(integration::boundary_trace(run));
    result["state"] = output.has("state") ? output["state"] : Json(Json::Object{});
    if(output.has("artifacts")) for(const auto& file : output["artifacts"].items()) files.push(file);
  }
  if(runs.items().size() == 1) {
    result["trace"] = traces.items().front(); result["output"] = outputs.items().front();
  } else {
    result["traces"] = traces; result["output"] = Json{{"turns", outputs}, {"final", outputs.items().back()}};
  }
  if(!files.items().empty()) result["artifacts"] = files;
  return result;
}

Json trial_trace_analysis(const std::vector<ValidatedTrace>& traces) {
  Json analyses = Json::Array{}, metrics = Json::Object{};
  std::size_t invalid_loops = 0;
  bool first = true, observed = false;
  for(const auto& trace : traces) {
    auto analysis = Json::parse(legacy::LoopEpisodeAnalyzer::to_json(analyze_trace(trace)));
    invalid_loops += analysis["summary"].num("invalid_loop_lower_bound", 0);
    analyses.push(std::move(analysis));
    const auto value = integration::json(trace.trace);
    const bool boundary = value["attributes"].str("protocol") == "evolution.boundary.v1";
    observed |= boundary;
    const auto part = boundary ? integration::boundary_metrics(value["events"]) : Json(Json::Object{});
    if(first) metrics = part;
    else for(auto it = metrics.fields().begin(); it != metrics.fields().end();) {
      // 任一 run 缺失指标都表示未知，不能在试验总计中按零处理。
      if(!part.has(it->first)) it = metrics.fields().erase(it);
      else { it->second = it->second.integer() + part[it->first].integer(); ++it; }
    }
    first = false;
  }
  return Json{{"observed", observed}, {"metrics", metrics}, {"trajectory", traces.size() == 1
      ? analyses.items().front() : Json{{"runs", analyses},
        {"summary", Json{{"invalid_loop_lower_bound", invalid_loops}, {"run_count", traces.size()}}}}}};
}

void compact_judge_evidence(Json& input, bool share_inputs = false) {
  rapidjson::Document compact;
  const auto original = input.dump();
  compact.Parse(original.data(), original.size());
  auto& evidence = compact["evidence"];
  legacy::model_input::Sharing sharing(compact.GetAllocator(), "$evidence_ref", share_inputs);
  auto collect_trace = [&](rapidjson::Value& trace, const std::string& path) {
    if(!trace.HasMember("events") || !trace["events"].IsArray()) return;
    auto& events = trace["events"];
    for(rapidjson::SizeType i = 0; i < events.Size(); ++i) {
      auto& event = events[i];
      if(event.HasMember("input")) sharing.collect(event["input"], path + "/events/" + std::to_string(i) + "/input");
      // 活动／步骤结束事件和应用回执也会重复执行值；仅共享完全相同的负载，
      // 绝不能共享事件身份或事件顺序。
      if(share_inputs) for(const auto* field : {"output", "attributes", "application"})
        if(event.HasMember(field)) sharing.collect(event[field],
            path + "/events/" + std::to_string(i) + "/" + field);
    }
  };
  if(evidence.HasMember("trace")) collect_trace(evidence["trace"], "/trace");
  if(evidence.HasMember("traces"))
    for(rapidjson::SizeType i = 0; i < evidence["traces"].Size(); ++i)
      collect_trace(evidence["traces"][i], "/traces/" + std::to_string(i));
  auto references = sharing.references();
  if(references.Empty()) return;
  compact.AddMember("evidence_text_refs", references, compact.GetAllocator());
  const auto packed = legacy::model_input::bytes(compact);
  if(packed.size() < original.size()) input = Json::parse(packed);
}

Json loop_inventory(const Json& evidence) {
  Json inventory = Json::Object{};
  const auto traces = evidence.has("traces") ? evidence["traces"]
      : evidence.has("trace") ? Json(Json::Array{evidence["trace"]}) : Json(Json::Array{});
  std::set<std::string> executed;
  for(const auto& raw : traces.items()) {
    // Protobuf JSON 把 uint64 表示为字符串；分组前要规范化等价线缆表示，
    // 避免同一轮因数字写法不同被拆分。
    const auto trace = integration::json(integration::proto<v1::TraceRun>(raw));
    for(const auto& event : trace["events"].items()) {
      if(event.str("loop_id").empty() || event.str("execution_id").empty() || event.str("iteration", "0") == "0") continue;
      const auto id = hash(Json(Json::Array{trace["identity"]["run_id"], event["execution_id"],
          event["loop_id"], event["iteration"]}).dump());
      auto& item = inventory[id];
      if(item.null()) item = Json{{"run_id", trace["identity"]["run_id"]}, {"execution_id", event["execution_id"]},
          {"loop_id", event["loop_id"]}, {"iteration", event["iteration"]}, {"event_ids", Json::Array{}}};
      item["event_ids"].push(event["event_id"]);
      if(event.str("kind") == "action.end" && event["attributes"].flag("executed", true)) {
        executed.insert(id);
        // 这里只接受具体定位符，不能接受预先计算的判断或生成式证据。
        if(!item.has("anchor_event_id")) item["anchor_event_id"] = event["event_id"];
      }
    }
  }
  for(auto it = inventory.fields().begin(); it != inventory.fields().end();)
    if(!executed.count(it->first)) it = inventory.fields().erase(it); else ++it;
  return inventory;
}

Json validate_loop_review(const Json& value, const Json& inventory, const Json& evidence,
                          bool chinese_reason, Json& diagnostic) {
  const bool detailed = value.has("iteration_reviews");
  std::size_t index = 0;
  bool in_item = false;
  auto check = [&](bool valid, const char* constraint, const Json& reference = Json()) {
    if(valid) return;
    diagnostic = Json{{"constraint", constraint}};
    if(in_item) diagnostic[detailed ? "iteration_review_index" : "wasted_iteration_index"] = index;
    if(!reference.null()) diagnostic["reference"] = reference;
    throw Error("evaluation_loop_review_invalid");
  };
  integration::keys(value, {"status", "wasted_iterations", "iteration_reviews"});
  check(!(detailed && value.has("wasted_iterations")), "review_forms_exclusive");
  const auto status = value.str("status");
  check(status == "complete" || status == "uncertain", "invalid_status");
  const auto& reviews = value[detailed ? "iteration_reviews" : "wasted_iterations"].items();
  check(reviews.size() <= inventory.fields().size(), detailed ? "too_many_iteration_reviews" : "too_many_wasted_iterations");
  check(!detailed || status != "complete" || reviews.size() == inventory.fields().size(), "missing_iteration_reviews");
  check(detailed || status == "complete" || reviews.empty(), "uncertain_must_not_list_waste");
  check(status != "complete" || !inventory.fields().empty(), "no_observed_iterations");
  std::set<std::string> seen;
  Json wasted = Json::Array{};
  static const std::regex han("[\xE4-\xE9][\x80-\xBF][\x80-\xBF]");
  const auto traces = evidence.has("traces") ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
  std::map<std::size_t, v1::TraceRun> ordered_traces;
  for(const auto& item : reviews) {
    in_item = true;
    if(detailed) integration::keys(item, {"iteration_id", "reason", "decision", "evidence_event_ids"});
    else integration::keys(item, {"iteration_id", "reason", "evidence_event_ids"});
    const auto decision = detailed ? item.str("decision") : "waste";
    check(decision == "keep" || decision == "waste" || decision == "uncertain", "invalid_iteration_decision");
    check(status != "complete" || decision != "uncertain", "uncertain_iteration_in_complete_review");
    const auto id = item.str("iteration_id"), reason = item.str("reason");
    check(inventory.has(id), "unknown_iteration");
    check(seen.insert(id).second, "duplicate_iteration");
    check(!reason.empty() && reason.size() <= 4096, "invalid_reason_length");
    check(!chinese_reason || std::regex_search(reason, han), "chinese_reason_required");
    const auto& cited = item["evidence_event_ids"].items();
    check(!cited.empty() && cited.size() <= 32, "invalid_event_count");
    const auto& own = inventory[id]["event_ids"].items();
    auto in_round = [&](const std::string& event) {
      return std::any_of(own.begin(), own.end(), [&](const Json& entry) { return entry.text() == event; });
    };
    // 保留当前轮锚点。已汇合的子结果可能在本轮结束前到达、却未显式先于开始；
    // 但不能把无关的后续事件并入本轮。
    std::set<std::string> preceding;
    for(std::size_t t = 0; t < traces.items().size(); ++t) {
      const auto& trace = traces.items()[t];
      if(trace["identity"]["run_id"].text() != inventory[id]["run_id"].text()) continue;
      // 只在当前审查内复用已验证顺序，绝不能跨请求复用。
      auto found = ordered_traces.find(t);
      if(found == ordered_traces.end())
        found = ordered_traces.emplace(t, validate_trace(parse_trace_json(trace.dump())).trace).first;
      const auto& ordered = found->second;
      for(const auto& event : ordered.events()) {
        const auto& event_id = event.event_id();
        if(in_round(event_id)) break;
        preceding.insert(event_id);
      }
      std::set<std::string> causes;
      for(auto it = ordered.events().rbegin(); it != ordered.events().rend(); ++it)
        if(in_round(it->event_id()) || causes.count(it->event_id()))
          causes.insert(it->caused_by().begin(), it->caused_by().end());
      preceding.insert(causes.begin(), causes.end());
    }
    std::set<std::string> references;
    bool anchored = false;
    for(std::size_t i = 0; i < cited.size(); ++i) {
      const auto event = cited[i].text();
      const char* constraint = !references.insert(event).second ? "duplicate_event"
          : in_round(event) || preceding.count(event) ? nullptr : "event_not_in_iteration_or_prior_context";
      if(constraint) check(false, constraint, Json{{"iteration_id", id}, {"decision", decision}, {"evidence_event_index", i},
          {"event_id", event.size() <= 256 ? event : "<event ID exceeds 256 bytes>"},
          {"allowed_iteration_event_ids", inventory[id]["event_ids"]}});
    }
    for(const auto& event : cited) anchored |= in_round(event.text());
    check(anchored, "missing_iteration_anchor", Json{{"iteration_id", id}, {"decision", decision},
        {"allowed_iteration_event_ids", inventory[id]["event_ids"]}});
    if(decision == "waste" && status == "complete")
      wasted.push(Json{{"iteration_id", id}, {"reason", reason}, {"evidence_event_ids", item["evidence_event_ids"]}});
    ++index;
  }
  Json result{{"method", "model_review"}, {"status", status}, {"observed_iterations", inventory.fields().size()},
      {"count", status == "complete" ? Json(wasted.items().size()) : Json(nullptr)}, {"wasted_iterations", wasted}};
  // 为审计保留每个判断；计数只能来自完整审查中的显式浪费判断，
  // 同时兼容旧版自定义评分器。
  if(detailed) result["iteration_reviews"] = value["iteration_reviews"];
  return result;
}

struct Workspace {
  fs::path path;
  explicit Workspace(fs::path value) : path(std::move(value)) { make_directory(path); }
  ~Workspace() {
    // 该路径必须是评测器新建且拥有的试验目录，绝不能是用户工作区、任务或子进程
    // 提供的未解析路径。
    std::error_code ignored; fs::remove_all(path, ignored);
  }
};

std::string artifact_path(const Json& item, bool allow_pattern) {
  integration::keys(item, allow_pattern ? std::initializer_list<const char*>{"path", "pattern", "media_type"}
                                      : std::initializer_list<const char*>{"path", "media_type"});
  require(item.has("path") != item.has("pattern"), "evaluation_artifact_path_invalid");
  const auto name = item.str(item.has("pattern") ? "pattern" : "path");
  const fs::path relative(name);
  require(!name.empty() && name.size() <= 1024 && name.find('\0') == std::string::npos
      && relative.is_relative() && !relative.has_root_path(), "evaluation_artifact_path_invalid");
  std::size_t depth = 0;
  for(const auto& part : relative) {
    require(part != ".." && part != "." && !part.empty(), "evaluation_artifact_path_invalid");
    ++depth;
  }
  if(item.has("pattern")) require(depth <= 16 && name.find("**") == std::string::npos
      && name.find_first_of("?[]\\") == std::string::npos, "evaluation_artifact_pattern_invalid");
  safe(Json(name));
  return name;
}

void collect_artifacts(Json& response, const Json& files, const fs::path& workspace, std::size_t& visited) {
  if(!response.has("artifacts")) response["artifacts"] = Json::Array{};
  auto add = [&](const std::string& path, const Json& file) {
    for(const auto& existing : response["artifacts"].items()) if(existing.str("path") == path) {
      require(existing.str("media_type", "text/plain") == file.str("media_type", "text/plain"),
          "evaluation_artifact_type_conflict");
      return;
    }
    require(response["artifacts"].items().size() < 32, "evaluation_artifact_count_limit");
    response["artifacts"].push(Json{{"path", path}, {"media_type", file.str("media_type", "text/plain")}});
  };
  for(const auto& file : files.items()) {
    const auto name = artifact_path(file, true);
    if(!file.has("pattern")) { add(name, file); continue; }
    // 配置的逐组件模式不递归，也不跟随符号链接；可选分支零匹配是否可接受
    // 由任务验收条件决定。
    std::vector<fs::path> candidates{workspace};
    for(const auto& part : fs::path(name)) {
      std::vector<fs::path> next;
      const auto pattern = part.string();
      for(const auto& directory : candidates) {
        std::error_code ec;
        if(!fs::is_directory(directory, ec)) { require(!ec, "evaluation_artifact_scan_failed"); continue; }
        auto accept = [&](const fs::path& path) {
          require(!fs::is_symlink(path), "evaluation_artifact_symlink");
          require(next.size() < 8192, "evaluation_artifact_scan_limit");
          next.push_back(path);
        };
        if(pattern.find('*') == std::string::npos) {
          const auto path = directory / part;
          if(fs::exists(fs::symlink_status(path))) accept(path);
        } else {
          fs::directory_iterator it(directory, ec), end;
          require(!ec, "evaluation_artifact_scan_failed");
          while(it != end) {
            require(++visited <= 8192, "evaluation_artifact_scan_limit");
            if(::fnmatch(pattern.c_str(), it->path().filename().c_str(), FNM_PERIOD) == 0) accept(it->path());
            it.increment(ec); require(!ec, "evaluation_artifact_scan_failed");
          }
        }
      }
      candidates = std::move(next);
    }
    std::sort(candidates.begin(), candidates.end());
    for(const auto& path : candidates) add(path.lexically_relative(workspace).generic_string(), file);
  }
}

Json artifacts(const Json& response, const fs::path& workspace, const fs::path& output,
               std::size_t max_bytes) {
  Json result = Json::Object{};
  if(!response.has("artifacts")) return result;
  const auto& entries = response["artifacts"].items();
  require(entries.size() <= 32, "evaluation_artifact_count_limit");
  std::size_t total = 0;
  // 先校验全部产物再保留任一产物，避免被拒试验把早先未检查的文件或带签名下载 URL
  // 留在报告目录。
  std::vector<std::pair<std::string, std::string>> accepted;
  for(const auto& item : entries) {
    const auto name = artifact_path(item, false);
    const fs::path relative(name);
    auto path = workspace;
    for(const auto& part : relative) {
      require(part != ".." && part != "." && !part.empty(), "evaluation_artifact_path_invalid");
      path /= part;
      require(!fs::is_symlink(path), "evaluation_artifact_symlink");
    }
    require(!result.has(name), "evaluation_artifact_duplicate");
    const auto bytes = read_file(path, max_bytes);
    total += bytes.size(); require(total <= max_bytes, "evaluation_artifact_bytes_limit");
    const auto media = item.str("media_type", "text/plain");
    Json facts{{"size_bytes", bytes.size()}, {"sha256", hash(bytes)}, {"media_type", media}};
    if(media == "application/json") {
      facts["json"] = Json::parse(bytes); safe(facts["json"]);
    } else if(media.rfind("text/", 0) == 0) {
      // JSON 编码会验证 UTF-8；全部文本产物字节都必须通过隐私检查。
      safe(Json::parse(Json(bytes).dump()));
      require(bytes.find('\0') == std::string::npos, "evaluation_text_artifact_invalid");
      facts["text"] = bytes;
    } else if(media == "image/png") {
      require(bytes.size() >= 33 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0
          && bytes.compare(12, 4, "IHDR") == 0, "evaluation_image_header_invalid");
      auto u32 = [&](std::size_t offset) {
        std::size_t n = 0; for(int i = 0; i < 4; ++i) n = (n << 8) | static_cast<unsigned char>(bytes[offset + i]);
        return n;
      };
      facts["width"] = u32(16); facts["height"] = u32(20);
      require(u32(16) > 0 && u32(20) > 0, "evaluation_image_dimensions_invalid");
      facts["validation"] = "header_only";
#if defined(EVOLUTION_HAS_PNG) && defined(PNG_SIMPLIFIED_READ_SUPPORTED)
      // 简化像素读取器可能在 IEND 前结束；还要验证完整容器，包括分块边界与校验和。
      std::size_t offset = 8;
      bool ended = false;
      while(offset < bytes.size()) {
        require(bytes.size() - offset >= 12, "evaluation_image_decode_failed");
        const auto length = u32(offset);
        require(length <= bytes.size() - offset - 12, "evaluation_image_decode_failed");
        require(crc32(0, reinterpret_cast<const Bytef*>(bytes.data() + offset + 4),
            static_cast<uInt>(length + 4)) == u32(offset + 8 + length), "evaluation_image_decode_failed");
        ended = bytes.compare(offset + 4, 4, "IEND") == 0;
        offset += length + 12;
        if(ended) { require(length == 0 && offset == bytes.size(), "evaluation_image_decode_failed"); break; }
      }
      require(ended, "evaluation_image_decode_failed");
      png_image image{}; image.version = PNG_IMAGE_VERSION;
      struct ImageCleanup { png_image& image; ~ImageCleanup() { png_image_free(&image); } } cleanup{image};
      require(png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()) != 0,
          "evaluation_image_decode_failed");
      // 分配像素前限制解压规模；保留 alpha 并使用最小行跨度。解码器消息不能作为
      // 持久化证据。
      image.format = PNG_FORMAT_RGBA;
      const std::uint64_t pixel_bytes = std::uint64_t(image.width) * image.height * 4;
      require(pixel_bytes > 0 && pixel_bytes <= std::min<std::size_t>(max_bytes, 64 * 1024 * 1024),
          "evaluation_image_decoded_bytes_limit");
      std::vector<unsigned char> pixels(static_cast<std::size_t>(pixel_bytes));
      require(png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) != 0,
          "evaluation_image_decode_failed");
      facts["validation"] = "decoded";
#endif
    } else {
      throw Error("evaluation_artifact_media_unsupported");
    }
    safe(facts);
    const auto stored = hash(name).substr(0, 24) + (media == "image/png" ? ".png" : media == "application/json" ? ".json" : ".txt");
    facts["stored_file"] = stored;
    accepted.emplace_back(stored, bytes); result[name] = facts;
  }
  if(!accepted.empty()) {
    make_directory(output);
    for(const auto& item : accepted) write_file(output / item.first, item.second);
  }
  return result;
}

struct Evaluator {
  Json config;
  fs::path config_dir, output;
  TaskEvaluationOptions options;
  std::vector<Variant> variants;
  std::shared_ptr<legacy::ModelAdapter> adapter;
  std::string judge_prompt;
  std::mutex judge_mutex;
  std::size_t model_calls = 0, max_model_calls = 0;
  std::size_t timeout = 0, max_bytes = 0, max_trace_bytes = 0, max_artifact_bytes = 0;
  EvaluationProcessLimits resources;
  bool model_enabled = false, require_trace = false;
  bool require_chinese_reason = true;
  std::string model_config_fingerprint;

  fs::path resolve(const std::string& path) const {
    require(!path.empty(), "evaluation_configuration_path_missing");
    return fs::path(path).is_absolute() ? fs::path(path) : config_dir / path;
  }

  void initialize_model() {
    const auto& judge = config["judge"];
    if(judge.null()) return;
    integration::keys(judge, {"enabled", "runtime_config", "prompt", "loop_review_prompt", "include_model_requests", "share_repeated_text", "share_repeated_inputs", "max_model_calls", "max_input_bytes", "max_repair_attempts"});
    judge.flag("include_model_requests", true);
    judge.flag("share_repeated_text", false);
    judge.flag("share_repeated_inputs", false);
    model_enabled = judge.flag("enabled", false);
    require(model_enabled || !judge.has("loop_review_prompt"), "evaluation_loop_review_requires_model");
    if(!model_enabled) return;
    max_model_calls = bounded(judge, "max_model_calls", 100, 1, 10000);
    bounded(judge, "max_input_bytes", 262144, 1024, 2097152);
    bounded(judge, "max_repair_attempts", 1, 0, 3);
    judge_prompt = read_file(resolve(judge.str("prompt")), 262144);
    if(judge.has("loop_review_prompt")) {
      const auto loop_prompt = read_file(resolve(judge.str("loop_review_prompt")), 65536);
      require(!loop_prompt.empty(), "evaluation_loop_prompt_empty");
      judge_prompt += '\n' + loop_prompt;
    }
    require(!judge_prompt.empty(), "evaluation_judge_prompt_empty"); safe(Json(judge_prompt));
    const auto path = options.model_config.empty() ? resolve(judge.str("runtime_config")) : options.model_config;
    legacy::RuntimeConfiguration runtime;
    try { runtime = legacy::load_runtime_configuration(path); }
    catch(...) { throw Error("evaluation_model_configuration_invalid"); }
    require_chinese_reason = runtime.evaluator.require_chinese_reason;
    std::string model_sources;
    for(const auto& source : runtime.source_files) model_sources += read_file(source, 1048576);
    model_config_fingerprint = hash(model_sources);
    auto http = runtime.model;
    if(http.endpoint.empty()) if(const auto* base = std::getenv("OPENAI_BASE_URL")) {
      http.endpoint = base;
      while(!http.endpoint.empty() && http.endpoint.back() == '/') http.endpoint.pop_back();
      http.endpoint += "/chat/completions";
    }
    if(http.model_id.empty()) if(const auto* model = std::getenv("OPENAI_MODEL")) http.model_id = model;
    model_config_fingerprint = hash(model_config_fingerprint + http.endpoint + "\n" + http.model_id);
    adapter = options.model_adapter;
    if(adapter) return;
    require(!http.endpoint.empty() && !http.model_id.empty(), "evaluation_model_not_configured");
    if(!runtime.no_api_key) {
      const auto env = runtime.api_key_env.empty() ? "OPENAI_API_KEY" : runtime.api_key_env;
      const auto* key = std::getenv(env.c_str());
      require(key && *key, "evaluation_model_key_missing"); http.api_key = key;
    }
    http.cancelled = options.cancelled;
    adapter = legacy::make_openai_compatible_model_adapter(http);
  }

  dv::TrialCheck model_grade(const dv::EvaluationTask& task, Json& evidence) {
    dv::TrialCheck result; result.set_check_id("model"); result.set_status("error");
    rapidjson::Document view;
    const auto original = evidence.dump();
    view.Parse(original.data(), original.size());
    const bool include_model_requests = config["judge"].flag("include_model_requests", true);
    legacy::model_input::task_evidence_view(view, include_model_requests);
    const auto model_evidence = Json::parse(legacy::model_input::bytes(view));
    Json allowed = Json::Array{};
    for(const auto& field : model_evidence.fields()) allowed.push("/" + field.first);
    const auto repairs = config["judge"].num("max_repair_attempts", 1);
    const bool review_loops = config["judge"].has("loop_review_prompt");
    const auto inventory = review_loops ? loop_inventory(model_evidence) : Json(Json::Object{});
    Json observed_action_counts = Json::Object{};
    const auto traces = evidence.has("traces") ? evidence["traces"]
        : evidence.has("trace") ? Json(Json::Array{evidence["trace"]}) : Json(Json::Array{});
    std::set<std::string> operations;
    for(const auto& trace : traces.items()) for(const auto& event : trace["events"].items())
      if(event.str("kind") == "action.start" && !event.str("operation").empty())
        operations.insert(event.str("operation"));
    // 共享负载前先统计原始观测。缺失执行标记仍是未知；重复结果不能折叠不同调用。
    for(const auto& operation : operations) {
      try { observed_action_counts[operation] = action_count(Json{{"operation", operation}}, evidence); }
      catch(const Error&) { observed_action_counts[operation] = nullptr; }
    }
    Json model_inventory = Json::Object{}, iteration_ids = Json::Object{};
    std::map<std::pair<std::string, std::string>, std::size_t> event_order;
    for(const auto& trace : traces.items()) for(const auto& event : trace["events"].items())
      event_order.emplace(std::make_pair(trace["identity"].str("run_id"), event.str("event_id")), event_order.size());
    std::vector<std::pair<std::string, Json>> ordered(inventory.fields().begin(), inventory.fields().end());
    auto rank = [&](const Json& item) {
      const auto found = event_order.find({item.str("run_id"), item.str("anchor_event_id")});
      return found == event_order.end() ? event_order.size() : found->second;
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) { return rank(a.second) < rank(b.second); });
    // 短别名按观测到的执行顺序而非哈希顺序生成；补零保证 JSON 对象里 i10 排在 i09
    // 之后，规范 ID 与因果链接仍留在本地。
    const auto width = std::to_string(ordered.size()).size();
    for(const auto& item : ordered) {
      const auto number = std::to_string(model_inventory.fields().size() + 1);
      const auto id = "i" + std::string(width - number.size(), '0') + number;
      model_inventory[id] = item.second; iteration_ids[id] = item.first;
    }
    Json required_checks = Json::Array{};
    for(auto check : checks(task.task_id())) {
      check.fields().erase("id"); required_checks.push(std::move(check));
    }
    for(const auto& limit : std::map<std::string, std::size_t>{{"steps", task.budget().max_steps()},
          {"tool_calls", task.budget().max_tool_calls()}})
      if(static_cast<const Json&>(evidence)["metrics"].has(limit.first)) required_checks.push(Json{{"path", "/metrics/" + limit.first},
          {"op", "at_most"}, {"value", limit.second}});
    std::string validation_error;
    Json invalid_paths = Json::Array{};
    Json previous_response;
    Json loop_diagnostic;
    Json json_diagnostic;
    for(std::size_t attempt = 0; attempt <= repairs; ++attempt) {
      result.clear_evidence_paths();
      try {
        Json turns = Json::Array{}; for(const auto& turn : task.followup_inputs()) turns.push(turn);
        Json constraints = Json::Array{}; for(const auto& item : task.constraints()) constraints.push(item);
        Json input{{"task_input", task.task_input()}, {"followup_inputs", turns},
            {"constraints", constraints},
            {"expected_behavior", task.expected_behavior()}, {"evidence", model_evidence},
            {"allowed_evidence_paths", allowed},
            {"contract", Json{{"verdict", "passed|failed|uncertain"},
                {"reason", "先用简体中文分别核对expected_behavior和constraints中的强制动作、适用条件分支、每条禁止项，再决定verdict；必需项未完成或违反禁止项均不能passed"},
                {"evidence_paths", Json::Array{evidence.has("traces") ? "/traces" : evidence.has("trace") ? "/trace" : "/output"}}}}};
        // 部署检查是验收约束，不是 Agent 自己声称成功的依据。
        if(!required_checks.items().empty()) input["required_checks"] = required_checks;
        if(!traces.items().empty()) input["observed_action_counts"] = observed_action_counts;
        if(model_evidence.has("artifacts")) input["artifact_provenance"] = "evaluator_after_execution_not_agent_actions";
        // 尚未计算出的评估不能当作执行证据。
        input["evidence"].fields().erase("loop_review");
        if(!include_model_requests) input["evidence_view"] = "model_requests_omitted";
        if(review_loops) {
          input["loop_inventory"] = model_inventory;
          input["contract"]["loop_review"] = Json{{"status", "complete|uncertain"},
              {"iteration_reviews", Json::Array{Json{{"iteration_id", "loop_inventory key; cover every iteration exactly once"},
                  {"reason", "先说明保留或省去整轮的依据，再填写decision"}, {"decision", "keep|waste|uncertain"},
                  {"evidence_event_ids", Json::Array{"该轮的真实事件 ID"}}}}}};
        }
        if(attempt) input["validation_feedback"] = Json{{"code", validation_error},
            {"instruction", "上次判分未通过协议校验。重新返回一个严格JSON对象，evidence_paths只能逐字选择allowed_evidence_paths中的根级路径，不构造子路径。不要改变成功标准。"}};
        if(attempt && !invalid_paths.items().empty())
          input["validation_feedback"]["invalid_evidence_paths"] = invalid_paths;
        if(attempt && !previous_response.null())
          input["validation_feedback"]["previous_response"] = previous_response;
        if(attempt && !loop_diagnostic.null())
          input["validation_feedback"]["loop_review_error"] = loop_diagnostic;
        if(attempt && !json_diagnostic.null())
          input["validation_feedback"]["json_error"] = json_diagnostic;
        invalid_paths = Json::Array{};
        previous_response = Json();
        loop_diagnostic = Json();
        json_diagnostic = Json();
        safe(input);
        const bool share_inputs = config["judge"].flag("share_repeated_inputs", !config["judge"].has("share_repeated_text"));
        if(config["judge"].flag("share_repeated_text", false) || share_inputs) {
          compact_judge_evidence(input, share_inputs);
          safe(input);
        }
        const auto bytes = input.dump();
        require(bytes.size() <= config["judge"].num("max_input_bytes", 262144), "evaluation_judge_input_limit");
        legacy::ModelRequest request;
        request.packet_id = "task-eval:" + hash(task.task_version() + judge_prompt + bytes);
        request.system_prompt = judge_prompt; request.user_prompt = bytes;
        legacy::RawModelResponse response;
        {
          std::unique_lock<std::mutex> lock(judge_mutex);
          require(model_calls < max_model_calls, "evaluation_judge_call_limit");
          if(options.cancelled && options.cancelled()) throw Error("evaluation_cancelled");
          ++model_calls;
          result.set_attempts(attempt + 1);
          // 内置 HTTP 适配器每次调用使用新的 CURL 句柄。预算预留必须原子化，
          // 但不能因此串行化工作线程 I/O；注入式适配器仍遵守原有的串行调用契约。
          if(!options.model_adapter) lock.unlock();
          response = adapter->evaluate(request);
        }
        if(!response.transport_ok || !response.error_code.empty()) {
          static const std::regex known_code(R"(http_[0-9]{3}|transport_[0-9]+|output_token_limit_reached|invalid_api_response|response_too_large|request_too_large)");
          if(std::regex_match(response.error_code, known_code))
            throw Error(("evaluation_judge_" + response.error_code).c_str());
          throw Error("evaluation_judge_transport_failed");
        }
        const auto body = legacy::unwrap_model_json(response.body);
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(body.data(), body.size());
        if(document.HasParseError()) {
          // 只保留静态解析诊断，绝不保留未验证的响应体。
          json_diagnostic = Json{{"parser_code", static_cast<int>(document.GetParseError())},
              {"description", rapidjson::GetParseError_En(document.GetParseError())},
              {"offset", static_cast<std::size_t>(document.GetErrorOffset())}, {"response_bytes", body.size()}};
          throw Error("invalid_json");
        }
        const auto value = Json::convert(document);
        if(review_loops) integration::keys(value, {"verdict", "reason", "evidence_paths", "loop_review"});
        else integration::keys(value, {"verdict", "reason", "evidence_paths"});
        safe(value);
        previous_response = value;
        const auto verdict = value.str("verdict"), reason = value.str("reason");
        require(verdict == "passed" || verdict == "failed" || verdict == "uncertain", "evaluation_judge_verdict_invalid");
        require(!reason.empty() && reason.size() <= 8192, "evaluation_judge_reason_invalid");
        if(require_chinese_reason) {
          static const std::regex han("[\xE4-\xE9][\x80-\xBF][\x80-\xBF]");
          require(std::regex_search(reason, han), "evaluation_judge_chinese_reason_required");
        }
        const auto& paths = value["evidence_paths"].items();
        require(paths.size() <= 32 && (verdict == "uncertain" || !paths.empty()), "evaluation_judge_evidence_missing");
        for(const auto& item : paths) {
          const auto path = item.text();
          bool valid = std::any_of(allowed.items().begin(), allowed.items().end(), [&](const Json& p) {
                const auto root = p.text();
                return path == root || path.compare(0, root.size() + 1, root + '/') == 0;
              });
          try { valid = valid && pointer(evidence, path); }
          catch(const Error&) { valid = false; }
          if(!valid) {
            // 只有已通过隐私检查且受长度限制的路径才能进入恢复流程。
            if(path.size() <= 2048) invalid_paths.push(path);
            else invalid_paths.push("<path exceeds 2048 bytes>");
            continue;
          }
          result.add_evidence_paths(path);
        }
        require(invalid_paths.items().empty(), "evaluation_judge_evidence_invalid");
        if(review_loops) {
          auto review = validate_loop_review(value["loop_review"], model_inventory,
              evidence, require_chinese_reason, loop_diagnostic);
          // 短引用只在模型输入中局部有效；原始身份只有在覆盖、唯一性与因果证据
          // 通过验证后才能持久化。
          for(const auto* key : {"iteration_reviews", "wasted_iterations"})
            if(review.has(key)) for(auto& item : review[key].items())
              item["iteration_id"] = iteration_ids[item.str("iteration_id")];
          evidence["loop_review"] = std::move(review);
        }
        result.set_status(verdict); result.set_reason(reason);
        return result;
      } catch(const Error& error) {
        validation_error = error.what(); result.set_reason(validation_error);
        if(!loop_diagnostic.null()) result.set_reason(validation_error + ":" + loop_diagnostic.str("constraint"));
        const std::set<std::string> repairable{"invalid_json", "duplicate_json_key", "unknown_field", "string_required",
            "array_required", "object_required", "evaluation_judge_verdict_invalid", "evaluation_judge_reason_invalid",
            "evaluation_judge_chinese_reason_required", "evaluation_judge_evidence_missing", "evaluation_judge_evidence_invalid",
            "evaluation_loop_review_invalid"};
        if(!repairable.count(validation_error)) break;
      } catch(...) { result.set_reason("evaluation_judge_failed"); break; }
    }
    // 只保留有界的修复诊断，不能保留未经验证的模型判断。
    if(!invalid_paths.items().empty()) evidence["judge_validation_error"] = Json{
        {"code", validation_error}, {"invalid_evidence_paths", invalid_paths}};
    if(!json_diagnostic.null()) evidence["judge_validation_error"] = Json{
        {"code", validation_error}, {"json_error", json_diagnostic}};
    if(review_loops && !loop_diagnostic.null()) evidence["loop_review"]["validation_error"] = loop_diagnostic;
    return result;
  }

  std::vector<Json> checks(const std::string& task_id) const {
    std::vector<Json> out;
    for(const auto& key : {std::string("*"), task_id})
      if(config["checks"].has(key)) for(const auto& check : config["checks"][key].items()) out.push_back(check);
    return out;
  }

  dv::EvaluationTrial regrade(const dv::EvaluationTask& task, dv::EvaluationTrial result) {
    auto evidence = integration::json(result.evidence());
    evidence.fields().erase("judge_validation_error");
    evidence.fields().erase("loop_review");
    auto previous = result.checks(); result.clear_checks();
    bool failed = false, unavailable = false;
    std::set<std::string> rechecked;
    if(options.recheck) for(const auto& spec : checks(task.task_id())) rechecked.insert(spec.str("id"));
    for(const auto& check : previous) if(check.check_id() != "model") {
      if(check.check_id() == "contract.schedule_success") continue;
      if(rechecked.count(check.check_id())) continue;
      failed |= check.status() == "failed";
      unavailable |= check.status() == "error";
      *result.add_checks() = check;
    }
    if(options.recheck) for(const auto& spec : checks(task.task_id())) {
      auto check = grade_check(spec, evidence);
      failed |= check.status() == "failed"; unavailable |= check.status() == "error";
      *result.add_checks() = std::move(check);
    }
    result.clear_error_code();
    const auto contract = integration::schedule_contract_check(integration::json(task.attributes()), evidence);
    if(!contract.null()) {
      failed |= contract.str("status") == "failed";
      *result.add_checks() = integration::proto<dv::TrialCheck>(contract);
    }
    if(evidence.has("artifact_error")) result.set_error_code(evidence.str("artifact_error"));
    auto judgment = model_grade(task, evidence);
    result.set_verdict(unavailable ? "error" : failed ? "failed" : judgment.status());
    if(unavailable) result.set_error_code("evaluation_check_evidence_invalid");
    if(judgment.status() == "error") result.set_error_code("evaluation_judge_failed");
    *result.add_checks() = std::move(judgment);
    *result.mutable_evidence() = integration::proto<google::protobuf::Struct>(evidence);
    return result;
  }

  dv::EvaluationTrial trial(const dv::EvaluationTask& task, std::size_t variant_index,
                           std::size_t repetition, const std::string& id) {
    const auto& variant = variants[variant_index];
    dv::EvaluationTrial result;
    result.set_trial_id(id); result.set_task_id(task.task_id()); result.set_task_version(task.task_version());
    result.set_group_id(task.group_id()); result.set_category(task.category()); result.set_split(task.split());
    result.set_variant_id(variant.id); result.set_repetition(repetition);
    result.set_execution_status("unknown"); result.set_verdict("error");
    try {
      if(options.cancelled && options.cancelled()) throw Error("evaluation_cancelled");
      Workspace workspace(output / ".work" / id);
      Json followups = Json::Array{}, constraints = Json::Array{};
      for(const auto& item : task.followup_inputs()) followups.push(item);
      const auto expected_runs = variant.run_layout == "per_input" ? followups.items().size() + 1 : 1;
      for(const auto& item : task.constraints()) constraints.push(item);
      Json request{{"schema_version", 1}, {"trial_id", id}, {"variant_id", variant.id}, {"repetition", repetition},
          {"task", Json{{"task_id", task.task_id()}, {"task_input", task.task_input()},
              {"followup_inputs", followups}, {"initial_state", integration::json(task.initial_state())},
              {"constraints", constraints}, {"budget", integration::json(task.budget())}}}};
      if(!variant.artifact.null() && variant.instrumentation_config.null()) {
        request["version"] = variant.version;
        request["artifact"] = variant.artifact;
        request["evaluation_mode"] = true;
      }
      // 评分量表、数据切分、审查元数据与评分器断言都不能进入宿主进程。
      auto trial_env = variant.instrumentation_config.null() ? variant.environment
          : instrumented_environment(variant, workspace.path, id, request);
      std::unique_ptr<IntegrationHttpServer> observer;
      if(!variant.instrumentation_config.null() && variant.instrumentation_transport == "http") {
        std::map<std::string, std::string> credentials;
        for(const auto& entry : trial_env) if(entry.rfind("TRAJECTORY_EVOLUTION_TRIAL_TOKEN=", 0) == 0)
          credentials["TRAJECTORY_EVOLUTION_TRIAL_TOKEN"] = entry.substr(entry.find('=') + 1);
        observer = std::make_unique<IntegrationHttpServer>(workspace.path / "instrumentation.json", credentials);
        trial_env.push_back("TRAJECTORY_EVOLUTION_TRIAL_URL=http://127.0.0.1:" + std::to_string(observer->port()));
      }
      const auto process = run_evaluation_process(variant.command, trial_env, workspace.path,
          variant.entry.null() ? request.dump() + "\n" : "", std::min(timeout, static_cast<std::size_t>(task.budget().timeout_ms())),
          max_bytes, options.cancelled, resources, observer ? std::function<bool()>([&] {
            return observer->capture_finished(expected_runs);
          }) : std::function<bool()>{}, variant.entry.null() ? std::function<std::string()>{}
              : std::function<std::string()>([&] { return observer->evaluation_entry_status(); }));
      result.set_duration_ms(process.duration_ms);
      if(!process.error_code.empty()) {
        result.set_error_code(process.error_code);
        result.set_execution_status(process.error_code == "runner_timeout" ? "timeout"
            : process.error_code == "runner_cancelled" ? "cancelled" : "error");
        if(process.error_code == "runner_timeout") result.set_verdict("failed");
        // 保留 runner 自己的结论。采集与退出码独立；只有有界、已校验归属／版本且
        // 隐私安全的数据可以保留下来。
        if(!variant.instrumentation_config.null()) {
          try {
            if(observer) require(observer->flush_capture(), "evaluation_capture_not_persisted");
            const auto runs = instrumented_run(variant, workspace.path, id, max_trace_bytes);
            Json traces = Json::Array{}, captures = Json::Array{};
            std::vector<ValidatedTrace> validations;
            bool complete = runs.items().size() == expected_runs;
            for(const auto& run : runs.items()) {
              const auto trace = integration::boundary_trace(run);
              safe(trace);
              auto validated = validate_trace(parse_trace_json(trace.dump()));
              bool bound = false;
              for(const auto& event : validated.trace.events()) if(event.kind() == "run.start") {
                const auto attributes = integration::json(event.attributes());
                bound = attributes.str("agent_version_id") == variant.version && attributes.str("evaluation_trial_id") == id;
              }
              require(bound && validated.trace.identity().user_id() == variant.artifact.str("user_id"),
                  "evaluation_trace_version_mismatch");
              const bool ended = validated.report.trace_valid() && run.flag("closed", false) && !run.flag("incomplete", false);
              complete &= ended;
              traces.push(trace);
              captures.push(Json{{"trace", trace}, {"complete", ended}, {"validation", integration::json(validated.report)}});
              validations.push_back(std::move(validated));
            }
            Json evidence{{"loaded_version", variant.version}, {"runner_error", process.error_code},
                {"exit_code", process.exit_code}};
            if(complete) {
              const auto analysis = trial_trace_analysis(validations);
              if(validations.size() == 1) evidence["trace"] = traces.items().front();
              else evidence["traces"] = traces;
              evidence["metrics"] = analysis["metrics"]; evidence["trajectory"] = analysis["trajectory"];
              *result.mutable_metrics() = integration::proto<google::protobuf::Struct>(analysis["metrics"]);
            } else {
              evidence["failure_capture"] = captures.items().size() == 1 && expected_runs == 1 ? captures.items().front()
                  : Json{{"runs", captures}, {"complete", false}, {"expected_runs", expected_runs}, {"observed_runs", runs.items().size()}};
              result.add_warnings("failure_capture_incomplete");
            }
            safe(evidence);
            *result.mutable_evidence() = integration::proto<google::protobuf::Struct>(evidence);
          } catch(const Error& error) {
            result.clear_evidence(); result.clear_metrics();
            result.add_warnings(std::string("failure_evidence_unavailable:") + error.what());
          } catch(...) {
            result.clear_evidence(); result.clear_metrics();
            result.add_warnings("failure_evidence_unavailable");
          }
        }
        return result;
      }
      if(observer) require(observer->flush_capture(), "evaluation_capture_not_persisted");
      const auto response = variant.instrumentation_config.null() ? Json::parse(process.output)
          : instrumented_response(variant, workspace.path, id, max_trace_bytes, expected_runs);
      integration::keys(response, {"schema_version", "trial_id", "execution_status", "output", "state",
          "metrics", "trace", "traces", "artifacts", "loaded_version"});
      require(response.num("schema_version", 0) == 1 && response.str("trial_id") == id,
              "evaluation_response_identity_invalid");
      if(!variant.artifact.null())
        require(response.str("loaded_version") == variant.version, "evaluation_loaded_version_mismatch");
      const auto status = response.str("execution_status");
      require(status == "completed" || status == "failed" || status == "cancelled" || status == "environment_error",
              "evaluation_execution_status_invalid");
      result.set_execution_status(status);
      try { safe(response); }
      catch(...) {
        result.set_verdict("failed"); result.set_error_code("evaluation_sensitive_output_rejected");
        auto* check = result.add_checks(); check->set_check_id("privacy"); check->set_status("failed");
        check->set_reason("响应含敏感内容，已拒绝保存和发送评测模型。"); return result;
      }
      require(response.has("output"), "evaluation_output_missing");
      Json metrics = response.has("metrics") ? response["metrics"] : Json(Json::Object{});
      integration::keys(metrics, {"steps", "tool_calls", "loop_count", "tokens", "llm_calls", "input_tokens", "output_tokens"});
      for(const auto& item : metrics.fields()) item.second.integer();
      Json evidence{{"execution_status", status}, {"output", response["output"]}, {"state", response.has("state") ? response["state"] : Json(Json::Object{})},
          {"metrics", metrics}};
      if(model_enabled && config["judge"].has("loop_review_prompt"))
        evidence["loop_review"] = Json{{"method", "model_review"}, {"status", "not_reviewed"}, {"count", nullptr}};
      if(!variant.artifact.null()) evidence["loaded_version"] = variant.version;
      require(evidence["state"].object(), "evaluation_state_invalid");
      if(response.has("trace")) evidence["trace"] = response["trace"];
      *result.mutable_evidence() = integration::proto<google::protobuf::Struct>(evidence);
      if(response.has("trace") || response.has("traces")) {
        require(!(response.has("trace") && response.has("traces")), "evaluation_trace_forms_exclusive");
        const bool multiple = response.has("traces");
        const auto traces = multiple ? response["traces"] : Json(Json::Array{response["trace"]});
        require(traces.array() && !traces.items().empty() && traces.items().size() <= 1024, "evaluation_run_limit");
        std::vector<ValidatedTrace> validations;
        std::set<std::string> execution_ids;
        Json outputs = Json::Array{}, run_ids = Json::Array{};
        std::string execution_status = "completed";
        for(const auto& trace : traces.items()) {
          auto validated = validate_trace(parse_trace_json(trace.dump()));
          require(validated.report.trace_valid(), "evaluation_trace_invalid");
          require(execution_ids.insert(validated.trace.identity().run_id()).second, "evaluation_duplicate_run");
          if(variant.artifact.null()) require(validated.trace.identity().run_id() == id, "evaluation_trace_identity_invalid");
          if(!variant.artifact.null()) {
            require(validated.trace.identity().user_id() == variant.artifact.str("user_id"), "evaluation_trace_owner_mismatch");
            bool bound = false;
            for(const auto& event : validated.trace.events()) if(event.kind() == "run.start") {
              const auto attributes = integration::json(event.attributes());
              bound = attributes.str("agent_version_id") == variant.version && attributes.str("evaluation_trial_id") == id;
            }
            require(bound, "evaluation_trace_version_mismatch");
            run_ids.push(validated.trace.identity().run_id());
            if(!multiple) evidence["execution_run_id"] = validated.trace.identity().run_id();
          }
          for(const auto& event : validated.trace.events()) if(event.kind() == "run.end") {
            const auto state = event.outcome().execution_status();
            if(!multiple) {
              require(state == status, "evaluation_trace_outcome_mismatch");
              require(integration::json(event.output()).dump() == response["output"].dump(), "evaluation_trace_output_mismatch");
            }
            if(state == "cancelled" || (state == "failed" && execution_status == "completed")) execution_status = state;
            outputs.push(integration::json(event.output()));
          }
          validations.push_back(std::move(validated));
        }
        if(multiple) {
          require(execution_status == status, "evaluation_trace_outcome_mismatch");
          require(response["output"].dump() == Json{{"turns", outputs}, {"final", outputs.items().back()}}.dump(),
              "evaluation_trace_output_mismatch");
          evidence["traces"] = traces; evidence["execution_run_ids"] = run_ids;
        } else evidence["trace"] = response["trace"];
        const auto analysis = trial_trace_analysis(validations);
        if(analysis.flag("observed", false)) {
          const auto& observed = analysis["metrics"];
          for(const auto* key : {"steps", "tool_calls", "loop_count", "llm_calls", "tokens", "input_tokens", "output_tokens"}) {
            if(observed.has(key)) {
              if(metrics.has(key) && metrics[key].integer() != observed[key].integer())
                result.add_warnings(std::string("host_metric_mismatch:") + key);
              metrics[key] = observed[key];
            } else metrics.fields().erase(key);
          }
        }
        evidence["trajectory"] = analysis["trajectory"];
      } else {
        require(!require_trace, "evaluation_trace_required");
        result.add_warnings("trajectory_not_observed");
      }
      *result.mutable_metrics() = integration::proto<google::protobuf::Struct>(metrics);
      evidence["metrics"] = metrics;
      // 交付物缺失或无效不能丢弃已经验证过的执行证据。
      *result.mutable_evidence() = integration::proto<google::protobuf::Struct>(evidence);
      if(!metrics.has("steps") || !metrics.has("tool_calls")) result.add_warnings("step_or_tool_budget_unobserved");
      bool failed = false, unavailable = false;
      try {
        auto artifact_source = response;
        std::size_t visited = 0;
        for(const auto& key : {std::string("*"), task.task_id()}) if(config["artifacts"].has(key)) {
          collect_artifacts(artifact_source, config["artifacts"][key], workspace.path, visited);
        }
        evidence["artifacts"] = artifacts(artifact_source, workspace.path, output / "artifacts" / id, max_artifact_bytes);
      } catch(const Error& error) {
        const std::string code = error.what();
        if(code == "evaluation_write_failed" || code == "evaluation_directory_exists" || code == "evaluation_file_exists") throw;
        failed = true; result.set_error_code(code); evidence["artifact_error"] = code;
        auto* check = result.add_checks(); check->set_check_id("artifact"); check->set_status("failed");
        check->set_reason("声明的产物无法通过文件、格式或隐私验收。");
      }
      safe(evidence);
      *result.mutable_evidence() = integration::proto<google::protobuf::Struct>(evidence);
      if(status == "environment_error" || status == "cancelled") {
        result.set_error_code(status == "environment_error" ? "runner_environment_error" : "runner_cancelled");
        return result;
      }
      // 宿主执行结果与业务验收必须分离；后处理失败不能抹掉已可验证完成的产物。
      if(status == "failed") result.add_warnings("host_execution_failed");
      const std::map<std::string, std::size_t> budgets{{"steps", task.budget().max_steps()},
                                                     {"tool_calls", task.budget().max_tool_calls()}};
      for(const auto& pair : budgets) {
        if(metrics.has(pair.first)) {
          Json budget_check{{"id", std::string("budget.") + pair.first}, {"path", std::string("/metrics/") + pair.first},
              {"op", "at_most"}, {"value", pair.second}};
          auto check = grade_check(budget_check, evidence); failed |= check.status() == "failed";
          *result.add_checks() = std::move(check);
        }
      }
      for(const auto& spec : checks(task.task_id())) {
        auto check = grade_check(spec, evidence); failed |= check.status() == "failed";
        unavailable |= check.status() == "error";
        *result.add_checks() = std::move(check);
      }
      result.set_verdict(unavailable ? "error" : failed ? "failed" : "passed");
      const auto contract = integration::schedule_contract_check(integration::json(task.attributes()), evidence);
      if(!contract.null()) {
        failed |= contract.str("status") == "failed";
        *result.add_checks() = integration::proto<dv::TrialCheck>(contract);
        if(failed && !unavailable) result.set_verdict("failed");
      }
      if(unavailable) result.set_error_code("evaluation_check_evidence_invalid");
      // 硬失败仍是失败；显式循环审查仍要检查这些轨迹，使失败业务任务也保留效率证据。
      if(model_enabled && ((!failed && !unavailable) || config["judge"].has("loop_review_prompt"))) {
        auto judgment = model_grade(task, evidence);
        *result.mutable_evidence() = integration::proto<google::protobuf::Struct>(evidence);
        if(!failed && !unavailable) result.set_verdict(judgment.status());
        if(judgment.status() == "error") result.set_error_code("evaluation_judge_failed");
        *result.add_checks() = std::move(judgment);
      }
    } catch(const Error& error) { result.set_error_code(error.what()); result.set_verdict("error"); }
    catch(...) { result.set_error_code("evaluation_trial_failed"); result.set_verdict("error"); }
    return result;
  }
};

Json aggregate(const std::vector<const dv::EvaluationTrial*>& trials) {
  std::size_t passed = 0, failed = 0, uncertain = 0, errors = 0;
  double duration = 0;
  std::vector<double> durations;
  std::size_t trace_observations = 0;
  double invalid_loop_sum = 0;
  const bool reviewed_loops = std::any_of(trials.begin(), trials.end(), [](const auto* trial) {
    return integration::json(trial->evidence()).has("loop_review");
  });
  double reviewed_sum = 0; std::size_t reviewed_observations = 0;
  Json metric_sum = Json::Object{}, metric_count = Json::Object{};
  for(const auto* trial : trials) {
    passed += trial->verdict() == "passed"; failed += trial->verdict() == "failed";
    uncertain += trial->verdict() == "uncertain"; errors += trial->verdict() == "error";
    duration += trial->duration_ms();
    durations.push_back(trial->duration_ms());
    const auto metrics = integration::json(trial->metrics());
    for(const auto& item : metrics.fields()) {
      if(!metric_sum.has(item.first)) { metric_sum[item.first] = 0; metric_count[item.first] = 0; }
      metric_sum[item.first] = number(metric_sum[item.first]) + number(item.second);
      metric_count[item.first] = metric_count[item.first].integer() + 1;
    }
    const auto evidence = integration::json(trial->evidence());
    const auto* lower_bound = pointer(evidence, "/trajectory/summary/invalid_loop_lower_bound");
    if(lower_bound) { invalid_loop_sum += number(*lower_bound); ++trace_observations; }
    if(evidence["loop_review"].str("status") == "complete" && !evidence["loop_review"]["count"].null()) {
      reviewed_sum += number(evidence["loop_review"]["count"]); ++reviewed_observations;
    }
  }
  Json means = Json::Object{};
  for(const auto& item : metric_sum.fields()) means[item.first] = Json{{"mean", number(item.second) / metric_count[item.first].integer()},
      {"observations", metric_count[item.first]}};
  const auto scored = passed + failed;
  std::sort(durations.begin(), durations.end());
  return Json{{"trials", trials.size()}, {"passed", passed}, {"failed", failed}, {"uncertain", uncertain}, {"errors", errors},
      {"scored", scored}, {"coverage", trials.empty() ? Json(nullptr) : Json(double(scored) / trials.size())},
      {"pass_rate", scored ? Json(double(passed) / scored) : Json(nullptr)},
      {"pass_rate_all_trials", trials.empty() ? Json(nullptr) : Json(double(passed) / trials.size())},
      {"mean_agent_duration_ms", trials.empty() ? Json(nullptr) : Json(duration / trials.size())},
      {"p95_agent_duration_ms", durations.empty() ? Json(nullptr) : Json(durations[static_cast<std::size_t>(std::ceil(durations.size() * 0.95)) - 1])},
      {"invalid_loop_lower_bound", Json{{"mean", trace_observations ? Json(invalid_loop_sum / trace_observations) : Json(nullptr)},
          {"observations", trace_observations}}},
      {"invalid_loop_assessment", Json{{"method", reviewed_loops ? "model_review" : "rule_lower_bound"},
          {"mean", reviewed_loops ? (reviewed_observations ? Json(reviewed_sum / reviewed_observations) : Json(nullptr))
              : (trace_observations ? Json(invalid_loop_sum / trace_observations) : Json(nullptr))},
          {"observations", reviewed_loops ? reviewed_observations : trace_observations}}},
      {"host_reported_metrics", means}};
}

std::string markdown_cell(const std::string& input) {
  std::string out;
  for(char c : input) {
    if(c == '\n' || c == '\r') out += ' ';
    else if(c == '|') out += "\\|";
    else if(c == '<') out += "&lt;";
    else if(c == '>') out += "&gt;";
    else if(c == '&') out += "&amp;";
    else if(c == '`' || c == '[' || c == ']' || c == '\\' || c == '*' || c == '_') { out += '\\'; out += c; }
    else out += c;
  }
  return out;
}

std::string markdown_report(const dv::TaskEvaluationReport& report) {
  const auto summary = integration::json(report.summary());
  std::ostringstream text;
  text << "# Agent 任务评测报告\n\n"
       << "数据集版本：`" << report.dataset_version() << "`\n\n"
       << "试验 " << report.trials_size() << " 次；通过 " << summary.num("passed", 0)
       << "，失败 " << summary.num("failed", 0) << "，不确定 " << summary.num("uncertain", 0)
       << "，执行/评测错误 " << summary.num("errors", 0) << "。\n\n"
       << "本次仅执行和验收，不生成进化记忆、不修改策略、不自动晋升。"
          "通过率仅描述本任务集；未知指标不视作零，规则无效 loop 数是下界。\n\n"
       << "| 任务 | 版本 | 重复 | 执行状态 | 验收 | 说明 | 详情 |\n"
          "| --- | --- | --- | --- | --- | --- | --- |\n";
  for(const auto& trial : report.trials()) {
    std::string reason = trial.error_code();
    if(reason.empty()) for(const auto& check : trial.checks()) if(check.status() != "passed") { reason = check.reason(); break; }
    if(reason.empty()) reason = "已通过配置的验收条件。";
    text << "| " << markdown_cell(trial.task_id()) << " | " << markdown_cell(trial.variant_id())
         << " | " << trial.repetition() << " | " << trial.execution_status() << " | " << trial.verdict()
         << " | " << markdown_cell(reason) << " | [JSON](trials/" << trial.trial_id() << ".json) |\n";
  }
  text << "\n## 结果文件\n\n"
          "- [完整 JSON 报告](report.json)：逐次证据、规则/模型判断、分类统计和配对比较。\n"
          "- [Protobuf 报告](report.pb)：用于程序接入。\n"
          "- [逐次结果 JSONL](results.jsonl)：每行一次试验，不是原始 stdout。\n\n"
          "产物在 artifacts/<trial_id>/ 下；原始文件名与保留文件的对应关系见试验 evidence.artifacts。"
          "PNG 验证范围见 validation：decoded 为像素解码成功，header_only 为文件头和尺寸；画面语义另行验收。\n";
  return text.str();
}

Json comparisons(const std::vector<Variant>& variants, const dv::TaskEvaluationReport& report) {
  Json out = Json::Object{};
  if(variants.size() < 2) return out;
  std::map<std::pair<std::string, std::size_t>, const dv::EvaluationTrial*> baseline;
  for(const auto& trial : report.trials()) if(trial.variant_id() == variants.front().id)
    baseline[{trial.task_id(), trial.repetition()}] = &trial;
  for(std::size_t v = 1; v < variants.size(); ++v) {
    std::size_t pairs = 0, excluded = 0, wins = 0, losses = 0;
    for(const auto& trial : report.trials()) if(trial.variant_id() == variants[v].id) {
      const auto* parent = baseline.at({trial.task_id(), trial.repetition()});
      auto scored = [](const dv::EvaluationTrial& t) { return t.verdict() == "passed" || t.verdict() == "failed"; };
      if(!scored(*parent) || !scored(trial)) { ++excluded; continue; }
      ++pairs;
      wins += trial.verdict() == "passed" && parent->verdict() == "failed";
      losses += trial.verdict() == "failed" && parent->verdict() == "passed";
    }
    out[variants[v].id] = Json{{"baseline", variants.front().id}, {"paired_trials", pairs}, {"excluded_pairs", excluded},
        {"wins", wins}, {"losses", losses}, {"ties", pairs - wins - losses},
        {"pass_rate_delta", pairs ? Json((double(wins) - double(losses)) / pairs) : Json(nullptr)},
        {"automatic_promotion", false}};
  }
  return out;
}

}  // namespace

dv::TaskEvaluationReport run_task_evaluation(const fs::path& dataset_directory,
    const fs::path& config_path, const fs::path& output_directory, const TaskEvaluationOptions& options) {
  const bool regrade_only = !options.regrade_report.empty();
  require(!options.recheck || regrade_only, "evaluation_recheck_requires_regrade");
  require(!regrade_only || options.resume_report.empty(), "evaluation_checkpoint_modes_conflict");
  const auto& checkpoint_path = regrade_only ? options.regrade_report : options.resume_report;
  require(!fs::exists(fs::symlink_status(output_directory)), "evaluation_output_exists");
  const auto dataset = read_evaluation_dataset(dataset_directory);
  for(const auto& issue : dataset.issues()) require(issue.severity() != "error", "evaluation_dataset_has_errors");
  Evaluator evaluator;
  evaluator.config = Json::parse(options.config_json.empty() ? read_file(config_path, 1048576) : options.config_json);
  require(evaluator.config.dump().size() <= 4194304, "evaluation_config_size_limit");
  evaluator.config_dir = fs::absolute(config_path).parent_path();
  evaluator.output = fs::absolute(output_directory); evaluator.options = options;
  const auto& config = evaluator.config;
  integration::keys(config, {"schema_version", "splits", "repetitions", "parallelism", "max_trials", "timeout_ms",
      "max_output_bytes", "max_trace_bytes", "max_artifact_bytes", "max_report_bytes", "require_trace", "variants", "checks", "judge", "resources", "task_ids", "artifacts"});
  require(config.num("schema_version", 0) == 1, "evaluation_config_version_invalid");
  const auto repetitions = bounded(config, "repetitions", 1, 1, 100);
  const auto parallelism = bounded(config, "parallelism", 1, 1, 16);
  const auto max_trials = bounded(config, "max_trials", 1000, 1, 10000);
  const auto max_report_bytes = bounded(config, "max_report_bytes", 33554432, 1024, 134217728);
  evaluator.timeout = bounded(config, "timeout_ms", 1800000, 1, 86400000);
  evaluator.max_bytes = bounded(config, "max_output_bytes", 1048576, 128, 8388608);
  // 标准输出响应与多轮轨迹独立限额；不扩大模型评分输入或产物预算。
  evaluator.max_trace_bytes = bounded(config, "max_trace_bytes", 33554432, 128, 67108864);
  evaluator.max_artifact_bytes = bounded(config, "max_artifact_bytes", 16777216, 1, 67108864);
  evaluator.require_trace = config.flag("require_trace", false);
  if(config.has("resources")) {
    integration::keys(config["resources"], {"nice", "address_space_bytes", "cpu_seconds"});
    evaluator.resources.nice = bounded(config["resources"], "nice", 0, 0, 19);
    evaluator.resources.address_space_bytes = bounded(config["resources"], "address_space_bytes", 0, 0, 1099511627776ULL);
    evaluator.resources.cpu_seconds = bounded(config["resources"], "cpu_seconds", 0, 0, 86400);
  }
  std::set<std::string> splits;
  for(const auto& entry : config["splits"].items()) {
    const auto split = entry.text();
    require((split == "train" || split == "val" || split == "holdout") && splits.insert(split).second,
            "evaluation_split_invalid");
  }
  require(!splits.empty(), "evaluation_split_required");
  std::set<std::string> variant_ids, task_ids;
  auto& variants = evaluator.config["variants"].items();
  require(!variants.empty() && variants.size() <= 8, "evaluation_variant_limit");
  for(auto& value : variants) {
    integration::keys(value, {"id", "version", "command", "pass_env", "artifact", "policy_version_file", "instrumentation_config", "instrumentation_transport", "run_layout", "entry"});
    if(value.has("policy_version_file")) {
      require(!value.has("artifact") && value.has("instrumentation_config"), "evaluation_policy_source_conflict");
      require(!value.str("policy_version_file").empty(), "evaluation_policy_file_required");
      const auto bundle = Json::parse(read_file(evaluator.resolve(value.str("policy_version_file")), 4194304));
      integration::keys(bundle, {"schema_version", "version", "artifact", "evaluation"});
      require(bundle.num("schema_version", 0) == 1 && bundle["evaluation"]["gate"].flag("passed", false)
          && bundle.str("version") == agent_version_id(bundle["artifact"].dump()), "offline_policy_bundle_invalid");
      require(!value.has("version") || value.str("version") == bundle.str("version"), "evaluation_version_invalid");
      // 执行前只解析一次；文件内容参与来源与检查点身份，而不仅是可变的文件路径。
      value["artifact"] = bundle["artifact"]; value["version"] = bundle["version"];
    }
    label(value.str("id")); label(value.str("version"));
    require(variant_ids.insert(value.str("id")).second, "evaluation_variant_duplicate");
    if(value.has("artifact"))
      require(value.str("version") == agent_version_id(value["artifact"].dump()), "evaluation_version_invalid");
    Variant variant{value.str("id"), command(value, evaluator.config_dir), environment(value), value.str("version"), value["artifact"], nullptr};
    variant.published_policy = value.has("policy_version_file");
    variant.instrumentation_transport = value.str("instrumentation_transport", "native");
    variant.run_layout = value.str("run_layout", "single");
    if(value.has("entry")) {
      require(value.has("instrumentation_config"), "evaluation_instrumentation_config_required");
      require(!value.has("run_layout") || variant.run_layout == "per_input", "evaluation_entry_run_layout_invalid");
      require(!value.has("instrumentation_transport") || variant.instrumentation_transport == "http", "evaluation_entry_transport_invalid");
      integration::EvaluationEntry::validate(value["entry"]);
      safe(value["entry"]);
      variant.entry = value["entry"];
      variant.run_layout = "per_input"; variant.instrumentation_transport = "http";
    }
    require(variant.run_layout == "single" || variant.run_layout == "per_input", "evaluation_run_layout_invalid");
    require(!value.has("run_layout") || value.has("instrumentation_config"), "evaluation_instrumentation_config_required");
    require(variant.instrumentation_transport == "native" || variant.instrumentation_transport == "http", "evaluation_transport_invalid");
    require(!value.has("instrumentation_transport") || value.has("instrumentation_config"), "evaluation_instrumentation_config_required");
    if(value.has("instrumentation_config")) {
      const auto path = fs::absolute(evaluator.resolve(value.str("instrumentation_config")));
      variant.instrumentation_config = Json::parse(read_file(path, 262144));
      auto& settings = variant.instrumentation_config;
      for(const auto* key : {"runtime_config", "experience_template_file"}) {
        if(variant.published_policy) continue;
        const fs::path resource(settings.str(key));
        require(!resource.empty(), "evaluation_instrumentation_config_invalid");
        if(resource.is_relative()) settings[key] = (path.parent_path() / resource).string();
      }
      if(variant.artifact.null()) {
        require(settings["principals"].array() && !settings["principals"].items().empty(), "evaluation_user_required");
        variant.artifact = Json{{"schema_version", 1}, {"agent", Json{{"revision", variant.version},
          {"name", settings.str("application_id", settings.str("agent_id"))}}},
          {"user_id", settings["principals"].items().front()["user_id"]}, {"level", "L0"}, {"policy", nullptr}};
        variant.version = agent_version_id(variant.artifact.dump());
      }
    }
    evaluator.variants.push_back(std::move(variant));
  }
  require(config.dump().size() <= 4194304, "evaluation_config_size_limit");
  std::vector<const dv::EvaluationTask*> tasks;
  std::set<std::string> selected_ids;
  if(config.has("task_ids")) {
    require(config["task_ids"].array() && !config["task_ids"].items().empty(), "evaluation_task_ids_required");
    for(const auto& id : config["task_ids"].items())
      require(selected_ids.insert(id.text()).second, "evaluation_task_id_duplicate");
  }
  for(const auto& task : dataset.tasks()) {
    task_ids.insert(task.task_id());
    if(splits.count(task.split()) && (selected_ids.empty() || selected_ids.count(task.task_id()))) {
      require(options.allow_unreviewed || task.review_status() == "reviewed", "evaluation_review_required");
      tasks.push_back(&task);
    }
  }
  require(selected_ids.empty() || tasks.size() == selected_ids.size(), "evaluation_task_id_or_split_mismatch");
  require(!tasks.empty(), "evaluation_no_selected_tasks");
  if(config.has("checks")) for(const auto& item : config["checks"].fields()) {
    require(item.first == "*" || task_ids.count(item.first), "evaluation_check_task_unknown");
    require(item.second.items().size() <= 64, "evaluation_check_limit");
    for(const auto& check : item.second.items()) validate_check(check);
  }
  if(config.has("artifacts")) for(const auto& item : config["artifacts"].fields()) {
    require(item.first == "*" || task_ids.count(item.first), "evaluation_artifact_task_unknown");
    require(item.second.items().size() <= 32, "evaluation_artifact_count_limit");
    std::set<std::string> paths;
    for(const auto& file : item.second.items()) {
      safe(file); const auto name = artifact_path(file, true);
      require(paths.insert((file.has("pattern") ? "pattern:" : "path:") + name).second,
          "evaluation_artifact_duplicate");
      const auto media = file.str("media_type", "text/plain");
      require(media == "application/json" || media == "image/png" || media.rfind("text/", 0) == 0,
          "evaluation_artifact_media_unsupported");
    }
  }
  for(const auto* task : tasks) {
    std::set<std::string> ids{"model", "privacy", "execution", "artifact", "budget.steps", "budget.tool_calls",
        "contract.schedule_success"};
    for(const auto& check : evaluator.checks(task->task_id()))
      require(ids.insert(check.str("id")).second, "evaluation_check_duplicate");
    require(!evaluator.checks(task->task_id()).empty() || config["judge"].flag("enabled", false),
            "evaluation_task_has_no_grader");
  }
  const auto total = tasks.size() * repetitions * variants.size();
  require(total <= max_trials, "evaluation_trial_limit");
  evaluator.initialize_model();
  require(!regrade_only || evaluator.model_enabled, "evaluation_regrade_requires_model");
  dv::TaskEvaluationReport report;
  report.set_schema_version(1); report.set_report_kind("task_evaluation.v1");
  report.set_dataset_version(dataset.dataset_version()); report.set_evaluator_version("task-evaluator-v1");
  report.set_config_fingerprint(hash(config.dump() + evaluator.judge_prompt + evaluator.model_config_fingerprint));
  report.set_run_id("evaluation:" + hash(dataset.dataset_version() + config.dump()
      + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + std::to_string(getpid())));
  // 配置是来源信息而不是凭据；绝不能持久化已解析的环境变量值或 runner／runtime 的
  // 绝对路径，只保留显式版本标签。
  Json metadata{{"splits", config["splits"]}, {"repetitions", repetitions}, {"parallelism", parallelism},
      {"model_enabled", evaluator.model_enabled}, {"allow_unreviewed", options.allow_unreviewed},
      {"variants", Json::Array{}}};
  for(const auto& variant : variants) metadata["variants"].push(Json{{"id", variant["id"]}, {"version", variant["version"]}});
  if(evaluator.adapter) { metadata["judge_model"] = evaluator.adapter->model_id(); safe(metadata); }
  *report.mutable_configuration() = integration::proto<google::protobuf::Struct>(metadata);
  if(!evaluator.output.parent_path().empty()) fs::create_directories(evaluator.output.parent_path());
  make_directory(evaluator.output); make_directory(evaluator.output / ".work");
  make_directory(evaluator.output / "trials"); make_directory(evaluator.output / "artifacts");
  write_file(evaluator.output / "started.json", Json{{"run_id", report.run_id()}, {"dataset_version", dataset.dataset_version()},
      {"planned_trials", total}, {"complete", false}}.dump() + "\n");
  struct Job { const dv::EvaluationTask* task; std::size_t variant, repetition; };
  std::vector<Job> jobs;
  for(const auto* task : tasks) for(std::size_t repetition = 1; repetition <= repetitions; ++repetition)
    for(std::size_t variant = 0; variant < variants.size(); ++variant) jobs.push_back({task, variant, repetition});
  std::vector<dv::EvaluationTrial> results(total);
  std::map<std::size_t, dv::EvaluationTrial> checkpoint;
  Json resume = nullptr;
  if(!checkpoint_path.empty()) {
    const auto bytes = read_file(checkpoint_path, 134217728);
    const auto manifest = Json::parse(read_file(checkpoint_path.parent_path() / "manifest.json", 16777216));
    require(manifest["files"].str("report.json") == hash(bytes), "evaluation_checkpoint_changed");
    const auto raw = Json::parse(bytes); safe(raw);
    const auto previous = integration::proto<dv::TaskEvaluationReport>(raw);
    require(previous.schema_version() == report.schema_version() && previous.report_kind() == report.report_kind()
        && previous.dataset_version() == report.dataset_version() && previous.evaluator_version() == report.evaluator_version(),
        "evaluation_checkpoint_context_mismatch");
    require(manifest.str("run_id") == previous.run_id(), "evaluation_checkpoint_changed");
    if(regrade_only) {
      auto before = integration::json(previous.configuration()), after = metadata;
      for(const auto* key : {"parallelism", "model_enabled", "judge_model", "model_only_regrade", "rechecked_configured_checks", "execution_source"}) {
        before.fields().erase(key); after.fields().erase(key);
      }
      require(before.dump() == after.dump(), "evaluation_checkpoint_context_mismatch");
      require(previous.complete() == manifest.flag("complete", false)
          && (previous.complete() || !selected_ids.empty()), "evaluation_regrade_source_incomplete");
      // 更换评分器不能重新执行 runner 或确定性检查。该独立报告要同时绑定已采集执行
      // 与新评分器；普通同配置恢复不能把它当作一次新的执行。
      metadata["model_only_regrade"] = !options.recheck;
      if(options.recheck) metadata["rechecked_configured_checks"] = true;
      metadata["execution_source"] = Json{{"run_id", previous.run_id()},
          {"config_fingerprint", previous.config_fingerprint()}, {"report_sha256", hash(bytes)}};
      report.set_config_fingerprint(hash(previous.config_fingerprint() + report.config_fingerprint()
          + (options.recheck ? "evidence-recheck-v1" : "model-only-regrade-v1")));
      *report.mutable_configuration() = integration::proto<google::protobuf::Struct>(metadata);
    } else {
      require(previous.config_fingerprint() == report.config_fingerprint()
          && integration::json(previous.configuration()).dump() == metadata.dump(), "evaluation_checkpoint_context_mismatch");
    }
    std::map<std::tuple<std::string, std::string, std::size_t>, std::size_t> indexes;
    for(std::size_t i = 0; i < jobs.size(); ++i)
      indexes.emplace(std::make_tuple(jobs[i].task->task_id(), evaluator.variants[jobs[i].variant].id, jobs[i].repetition), i);
    std::set<std::size_t> seen; std::set<std::string> trial_ids;
    for(const auto& trial : previous.trials()) {
      // 显式子集审查可以复用暂停批次里的已完成任务；但每个被选执行仍必须存在且完整。
      if(regrade_only && !selected_ids.empty() && !selected_ids.count(trial.task_id())) continue;
      const auto found = indexes.find(std::make_tuple(trial.task_id(), trial.variant_id(), trial.repetition()));
      require(found != indexes.end() && seen.insert(found->second).second
          && trial_ids.insert(trial.trial_id()).second, "evaluation_checkpoint_trial_mismatch");
      label(trial.trial_id());
      const auto& job = jobs[found->second]; const auto& variant = evaluator.variants[job.variant];
      const auto evidence = integration::json(trial.evidence());
      require(trial.task_version() == job.task->task_version(), "evaluation_checkpoint_trial_mismatch");
      if(regrade_only) require((trial.execution_status() == "completed" || trial.execution_status() == "failed")
          && evidence.object() && (trial.error_code().empty() || trial.error_code() == "evaluation_judge_failed"
              || evidence.has("artifact_error")), "evaluation_regrade_execution_unavailable");
      if(trial.execution_status() != "completed" && trial.execution_status() != "failed") continue;
      const bool reviewed = !config["judge"].has("loop_review_prompt") || evidence["loop_review"].str("status") == "complete";
      const bool judge_error = evaluator.model_enabled && trial.error_code() == "evaluation_judge_failed"
          && std::any_of(trial.checks().begin(), trial.checks().end(), [](const auto& c) { return c.check_id() == "model" && c.status() == "error"; });
      if(regrade_only || judge_error || (reviewed && trial.error_code() != "evaluation_judge_failed"
          && (trial.verdict() == "passed" || trial.verdict() == "failed"))) {
        require(variant.artifact.null() || evidence.str("loaded_version") == variant.version, "evaluation_checkpoint_trial_mismatch");
        checkpoint.emplace(found->second, trial);
      }
    }
    require(!regrade_only || checkpoint.size() == total, "evaluation_regrade_source_incomplete");
    resume = Json{{"run_id", previous.run_id()}, {"report_sha256", hash(bytes)}};
  }
  std::vector<int> restored(total, 0);
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> retained_bytes{0};
  std::exception_ptr failure;
  std::mutex failure_mutex;
  auto work = [&] {
    while(true) {
      const auto index = next.fetch_add(1); if(index >= total) return;
      try {
        // 取消会停止调度，而不仅是终止子进程；保留真实试验结果，不能为未启动工作
        // 人为制造错误。
        const auto& job = jobs[index];
        const bool cancelled = options.cancelled && options.cancelled();
        // 未由新评分器审查的旧分数，不是新评分器下的结果。
        if(regrade_only && cancelled) continue;
        const auto saved = checkpoint.find(index);
        if(saved != checkpoint.end()) {
          results[index] = saved->second; restored[index] = 1;
          // 保留原始执行 ID 与时长；复制到新的独立可读报告前必须验证交付物字节。
          const auto evidence = integration::json(saved->second.evidence());
          std::vector<std::pair<std::string, std::string>> files; std::size_t bytes = 0;
          if(evidence["artifacts"].object()) for(const auto& entry : evidence["artifacts"].fields()) {
            const auto name = entry.second.str("stored_file"); label(name);
            require(fs::path(name).filename() == name && name != "." && name != "..", "evaluation_artifact_path_invalid");
            auto path = checkpoint_path.parent_path() / "artifacts" / saved->second.trial_id() / name;
            fs::path prefix;
            for(const auto& part : fs::absolute(path)) { prefix /= part; require(!fs::is_symlink(prefix), "evaluation_artifact_symlink"); }
            const auto content = read_file(path, evaluator.max_artifact_bytes);
            bytes += content.size(); require(bytes <= evaluator.max_artifact_bytes, "evaluation_artifact_bytes_limit");
            require(hash(content) == entry.second.str("sha256") && content.size() == entry.second.num("size_bytes", 0),
                "evaluation_checkpoint_artifact_changed");
            files.emplace_back(name, content);
          }
          if(!files.empty()) {
            const auto directory = evaluator.output / "artifacts" / saved->second.trial_id(); make_directory(directory);
            for(const auto& file : files) write_file(directory / file.first, file.second);
          }
          if(!cancelled && (regrade_only || saved->second.error_code() == "evaluation_judge_failed")) {
            results[index] = evaluator.regrade(*job.task, saved->second); restored[index] = 2;
          }
        } else {
          require(!regrade_only, "evaluation_regrade_execution_unavailable");
          if(cancelled) continue;
          const auto id = hash(report.run_id() + ":" + std::to_string(index)).substr(0, 32);
          results[index] = evaluator.trial(*job.task, job.variant, job.repetition, id);
        }
        require(retained_bytes.fetch_add(results[index].ByteSizeLong()) + results[index].ByteSizeLong()
            <= max_report_bytes, "evaluation_report_size_limit");
        // 持久化每个完成试验。崩溃会留下可用检查点，但没有最终清单就绝不是完整评测批次。
        write_file(evaluator.output / "trials" / (results[index].trial_id() + ".json"), integration::json(results[index]).dump() + "\n");
      } catch(...) {
        std::lock_guard<std::mutex> lock(failure_mutex); if(!failure) failure = std::current_exception();
        next.store(total); return;
      }
    }
  };
  std::vector<std::thread> workers;
  try { for(std::size_t i = 0; i < std::min(total, parallelism); ++i) workers.emplace_back(work); }
  catch(...) { next.store(total); for(auto& worker : workers) worker.join(); throw; }
  for(auto& worker : workers) worker.join();
  if(failure) std::rethrow_exception(failure);
  std::string jsonl;
  for(auto& trial : results) {
    if(trial.trial_id().empty()) continue;
    jsonl += integration::json(trial).dump() + "\n";
    *report.add_trials() = std::move(trial);
  }
  std::vector<const dv::EvaluationTrial*> all;
  std::map<std::string, std::vector<const dv::EvaluationTrial*>> by_variant, by_category, by_split;
  for(const auto& trial : report.trials()) {
    all.push_back(&trial); by_variant[trial.variant_id()].push_back(&trial);
    by_category[trial.category()].push_back(&trial); by_split[trial.split()].push_back(&trial);
  }
  Json summary = aggregate(all);
  for(const auto& group : by_variant) summary["by_variant"][group.first] = aggregate(group.second);
  for(const auto& group : by_category) summary["by_category"][group.first] = aggregate(group.second);
  for(const auto& group : by_split) summary["by_split"][group.first] = aggregate(group.second);
  summary["model_calls"] = evaluator.model_calls;
  summary["planned_trials"] = total;
  summary["not_started_trials"] = total - report.trials_size();
  if(regrade_only) summary["agent_reexecuted"] = false;
  if(!resume.null()) {
    summary["resume_source"] = resume;
    summary["reused_trials"] = std::size_t(std::count(restored.begin(), restored.end(), 1));
    summary["regraded_trials"] = std::size_t(std::count(restored.begin(), restored.end(), 2));
    summary["regraded_trial_ids"] = Json::Array{};
    for(std::size_t i = 0; i < restored.size(); ++i) if(restored[i] == 2)
      summary["regraded_trial_ids"].push(checkpoint.at(i).trial_id());
  }
  *report.mutable_summary() = integration::proto<google::protobuf::Struct>(summary);
  *report.mutable_comparisons() = integration::proto<google::protobuf::Struct>(comparisons(evaluator.variants, report));
  report.set_complete(report.trials_size() == static_cast<int>(total)
      && !(options.cancelled && options.cancelled()));
  auto report_json = integration::json(report);
  report_json["complete"] = report.complete();
  const auto json = report_json.dump() + "\n";
  require(json.size() <= max_report_bytes && report.ByteSizeLong() <= max_report_bytes,
      "evaluation_report_size_limit");
  const auto pb = report.SerializeAsString();
  const auto markdown = markdown_report(report);
  write_file(evaluator.output / "report.json", json);
  write_file(evaluator.output / "report.pb", pb);
  write_file(evaluator.output / "report.md", markdown);
  write_file(evaluator.output / "results.jsonl", jsonl);
  write_file(evaluator.output / "manifest.json", Json{{"complete", report.complete()}, {"run_id", report.run_id()},
      {"dataset_version", dataset.dataset_version()}, {"summary", summary},
      {"files", Json{{"report.json", hash(json)}, {"report.pb", hash(pb)}, {"report.md", hash(markdown)}, {"results.jsonl", hash(jsonl)}}}}.dump() + "\n");
  fs::remove(evaluator.output / ".work");
  return report;
}

}  // namespace trajectory::evolution
