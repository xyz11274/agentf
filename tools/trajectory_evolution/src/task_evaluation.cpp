#include "trajectory_evolution/task_evaluation.h"
#include "trajectory_evolution/trace.h"
#include "trajectory_evolution/integration.h"
#include "agentflow/trajectory/runtime_config.h"
#include "evaluation_process.h"
#include "integration_json.h"
#include "boundary_runtime.h"
#include "local_support.h"
#include "trace_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

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

// RFC 6901 pointers, including escaped names and arrays. Missing is different
// from an explicit JSON null; no executable expressions or regex evaluation.
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

dv::TrialCheck grade_check(const Json& check, const Json& evidence) {
  dv::TrialCheck result;
  result.set_check_id(check.str("id"));
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
    // A command-line argument is not an acceptable channel for credentials.
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
};

// The evaluator owns isolation/version/trace assembly; the host only emits points.
std::vector<std::string> instrumented_environment(const Variant& variant, const fs::path& workspace, const std::string& trial) {
  auto config = variant.instrumentation_config;
  config["data_dir"] = (workspace / "capture").string();
  config["evaluation_version_file"] = (workspace / "version.json").string();
  config["auto_evaluate"] = false; config["sharing"] = Json{{"enabled", false}};
  config["port"] = 0;
  config.fields().erase("policy_evaluation_file");
  config.fields().erase("policy_version_file");
  config["instrumentation"]["level"] = variant.artifact["level"];
  config["principals"] = Json::Array{Json{{"user_id", variant.artifact["user_id"]}, {"token_env", "TRAJECTORY_EVOLUTION_TRIAL_TOKEN"}}};
  write_file(workspace / "version.json", Json{{"version", variant.version}, {"artifact", variant.artifact}, {"trial_id", trial}}.dump());
  write_file(workspace / "instrumentation.json", config.dump());
  unsigned char salt[32]; require(RAND_bytes(salt, sizeof(salt)) == 1, "evaluation_secret_unavailable");
  const auto token = hash(std::string(reinterpret_cast<const char*>(salt), sizeof(salt)));
  auto env = variant.environment;
  // Environment values stay in process memory, not in the config or reports.
  const std::string config_key = "TRAJECTORY_EVOLUTION_TRIAL_CONFIG=", token_key = "TRAJECTORY_EVOLUTION_TRIAL_TOKEN=";
  env.erase(std::remove_if(env.begin(), env.end(), [&](const auto& entry) {
    return entry.rfind("TRAJECTORY_EVOLUTION_TRIAL_", 0) == 0;
  }), env.end());
  env.push_back(config_key + (workspace / "instrumentation.json").string()); env.push_back(token_key + token);
  return env;
}

Json instrumented_run(const Variant& variant, const fs::path& workspace, const std::string& trial, std::size_t max_bytes) {
  const auto folder = workspace / "capture/runs";
  require(fs::is_directory(folder) && !fs::is_symlink(folder), "evaluation_trace_missing");
  Json run; std::size_t count = 0;
  for(const auto& entry : fs::directory_iterator(folder)) {
    require(++count <= 1024, "evaluation_run_limit");
    if(entry.path().extension() != ".json") continue;
    const auto item = Json::parse(read_file(entry.path(), max_bytes));
    if(item.str("evaluation_trial_id") != trial) continue;
    require(run.null(), "evaluation_requires_single_logical_run"); run = item;
  }
  require(!run.null(), "evaluation_trace_missing");
  require(run.str("agent_version_id") == variant.version && run["identity"].str("user_id") == variant.artifact.str("user_id"),
      "evaluation_loaded_version_mismatch");
  return run;
}

Json instrumented_response(const Variant& variant, const fs::path& workspace, const std::string& trial, std::size_t max_bytes) {
  const auto run = instrumented_run(variant, workspace, trial, max_bytes);
  require(run.flag("closed", false) && !run.flag("incomplete", false), "evaluation_trace_incomplete");
  const auto& terminal = run["events"].items().back();
  require(terminal.str("kind") == "run.end", "evaluation_terminal_missing");
  const auto& output = terminal["output"];
  Json result{{"schema_version", 1}, {"trial_id", trial}, {"loaded_version", variant.version},
    {"execution_status", terminal["outcome"]["execution_status"]}, {"output", output},
    {"state", output.has("state") ? output["state"] : Json(Json::Object{})}, {"trace", integration::boundary_trace(run)}};
  if(output.has("artifacts")) result["artifacts"] = output["artifacts"];
  return result;
}

struct Workspace {
  fs::path path;
  explicit Workspace(fs::path value) : path(std::move(value)) { make_directory(path); }
  ~Workspace() {
    // This path is a newly created, evaluator-owned trial directory. Never a
    // user workspace or unresolved path supplied by a task or child process.
    std::error_code ignored; fs::remove_all(path, ignored);
  }
};

Json artifacts(const Json& response, const fs::path& workspace, const fs::path& output,
               std::size_t max_bytes) {
  Json result = Json::Object{};
  if(!response.has("artifacts")) return result;
  const auto& entries = response["artifacts"].items();
  require(entries.size() <= 32, "evaluation_artifact_count_limit");
  std::size_t total = 0;
  // Verify all artifacts before retaining any, so a rejected trial cannot leave
  // earlier unchecked files or signed download URLs in the report directory.
  std::vector<std::pair<std::string, std::string>> accepted;
  for(const auto& item : entries) {
    integration::keys(item, {"path", "media_type"});
    const auto name = item.str("path");
    const fs::path relative(name);
    require(!name.empty() && name.size() <= 1024 && name.find('\0') == std::string::npos
        && relative.is_relative() && !relative.has_root_path(), "evaluation_artifact_path_invalid");
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
      // JSON encoding validates UTF-8; all textual artifact bytes pass privacy.
      safe(Json::parse(Json(bytes).dump()));
      require(bytes.find('\0') == std::string::npos, "evaluation_text_artifact_invalid");
      facts["text"] = bytes;
    } else if(media == "image/png") {
      require(bytes.size() >= 33 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0
          && bytes.compare(12, 4, "IHDR") == 0, "evaluation_image_header_invalid");
      auto u32 = [&](int offset) {
        std::size_t n = 0; for(int i = 0; i < 4; ++i) n = (n << 8) | static_cast<unsigned char>(bytes[offset + i]);
        return n;
      };
      facts["width"] = u32(16); facts["height"] = u32(20);
      require(u32(16) > 0 && u32(20) > 0, "evaluation_image_dimensions_invalid");
      facts["validation"] = "header_only";
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
  std::size_t timeout = 0, max_bytes = 0, max_artifact_bytes = 0;
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
    integration::keys(judge, {"enabled", "runtime_config", "prompt", "max_model_calls", "max_input_bytes", "max_repair_attempts"});
    model_enabled = judge.flag("enabled", false);
    if(!model_enabled) return;
    max_model_calls = bounded(judge, "max_model_calls", 100, 1, 10000);
    bounded(judge, "max_input_bytes", 262144, 1024, 2097152);
    bounded(judge, "max_repair_attempts", 1, 0, 3);
    judge_prompt = read_file(resolve(judge.str("prompt")), 262144);
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

  dv::TrialCheck model_grade(const dv::EvaluationTask& task, const Json& evidence) {
    dv::TrialCheck result; result.set_check_id("model"); result.set_status("error");
    Json allowed = Json::Array{};
    for(const auto& field : evidence.fields()) allowed.push("/" + field.first);
    const auto repairs = config["judge"].num("max_repair_attempts", 1);
    std::string validation_error;
    for(std::size_t attempt = 0; attempt <= repairs; ++attempt) {
      result.clear_evidence_paths();
      try {
        Json turns = Json::Array{}; for(const auto& turn : task.followup_inputs()) turns.push(turn);
        Json input{{"task_input", task.task_input()}, {"followup_inputs", turns},
            {"expected_behavior", task.expected_behavior()}, {"evidence", evidence},
            {"allowed_evidence_paths", allowed},
            {"contract", Json{{"verdict", "passed|failed|uncertain"}, {"reason", "简体中文理由"},
                {"evidence_paths", Json::Array{"/output"}}}}};
        if(attempt) input["validation_feedback"] = Json{{"code", validation_error},
            {"instruction", "上次判分未通过协议校验。重新返回一个严格JSON对象，evidence_paths只能从allowed_evidence_paths选择。不要改变成功标准。"}};
        safe(input);
        const auto bytes = input.dump();
        require(bytes.size() <= config["judge"].num("max_input_bytes", 262144), "evaluation_judge_input_limit");
        legacy::ModelRequest request;
        request.packet_id = "task-eval:" + hash(task.task_version() + judge_prompt + bytes);
        request.system_prompt = judge_prompt; request.user_prompt = bytes;
        legacy::RawModelResponse response;
        {
          std::lock_guard<std::mutex> lock(judge_mutex);
          require(model_calls < max_model_calls, "evaluation_judge_call_limit");
          if(options.cancelled && options.cancelled()) throw Error("evaluation_cancelled");
          ++model_calls;
          result.set_attempts(attempt + 1);
          response = adapter->evaluate(request);
        }
        if(!response.transport_ok || !response.error_code.empty()) {
          static const std::regex known_code(R"(http_[0-9]{3}|transport_[0-9]+|output_token_limit_reached|invalid_api_response|response_too_large|request_too_large)");
          if(std::regex_match(response.error_code, known_code))
            throw Error(("evaluation_judge_" + response.error_code).c_str());
          throw Error("evaluation_judge_transport_failed");
        }
        std::string body = response.body;
        const auto first = body.find_first_not_of(" \r\n\t"), last = body.find_last_not_of(" \r\n\t");
        if(first != std::string::npos) body = body.substr(first, last - first + 1);
        // Accept a single enclosing JSON fence, not arbitrary prose or a greedy
        // search for braces that could silently select an unrelated object.
        const auto fence = body.rfind("```json\n", 0) == 0 ? 8U : body.rfind("```\n", 0) == 0 ? 4U : 0U;
        if(fence && body.size() >= fence + 4 && body.compare(body.size() - 4, 4, "\n```") == 0)
          body = body.substr(fence, body.size() - fence - 4);
        const auto value = Json::parse(body);
        integration::keys(value, {"verdict", "reason", "evidence_paths"}); safe(value);
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
          require(std::any_of(allowed.items().begin(), allowed.items().end(), [&](const Json& p) { return p.text() == path; })
              && pointer(evidence, path), "evaluation_judge_evidence_invalid");
          result.add_evidence_paths(path);
        }
        result.set_status(verdict); result.set_reason(reason);
        return result;
      } catch(const Error& error) {
        validation_error = error.what(); result.set_reason(validation_error);
        const std::set<std::string> repairable{"invalid_json", "duplicate_json_key", "unknown_field", "string_required",
            "array_required", "object_required", "evaluation_judge_verdict_invalid", "evaluation_judge_reason_invalid",
            "evaluation_judge_chinese_reason_required", "evaluation_judge_evidence_missing", "evaluation_judge_evidence_invalid"};
        if(!repairable.count(validation_error)) break;
      } catch(...) { result.set_reason("evaluation_judge_failed"); break; }
    }
    return result;
  }

  std::vector<Json> checks(const std::string& task_id) const {
    std::vector<Json> out;
    for(const auto& key : {std::string("*"), task_id})
      if(config["checks"].has(key)) for(const auto& check : config["checks"][key].items()) out.push_back(check);
    return out;
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
      // No rubric, split, review metadata, or grader assertions enter the host.
      auto trial_env = variant.instrumentation_config.null() ? variant.environment
          : instrumented_environment(variant, workspace.path, id);
      std::unique_ptr<IntegrationHttpServer> observer;
      if(!variant.instrumentation_config.null() && variant.instrumentation_transport == "http") {
        std::map<std::string, std::string> credentials;
        for(const auto& entry : trial_env) if(entry.rfind("TRAJECTORY_EVOLUTION_TRIAL_TOKEN=", 0) == 0)
          credentials["TRAJECTORY_EVOLUTION_TRIAL_TOKEN"] = entry.substr(entry.find('=') + 1);
        observer = std::make_unique<IntegrationHttpServer>(workspace.path / "instrumentation.json", credentials);
        trial_env.push_back("TRAJECTORY_EVOLUTION_TRIAL_URL=http://127.0.0.1:" + std::to_string(observer->port()));
      }
      const auto process = run_evaluation_process(variant.command, trial_env, workspace.path,
          request.dump() + "\n", std::min(timeout, static_cast<std::size_t>(task.budget().timeout_ms())),
          max_bytes, options.cancelled, resources, observer ? std::function<bool()>([&] {
            return observer->capture_finished();
          }) : std::function<bool()>{});
      result.set_duration_ms(process.duration_ms);
      if(!process.error_code.empty()) {
        result.set_error_code(process.error_code);
        result.set_execution_status(process.error_code == "runner_timeout" ? "timeout"
            : process.error_code == "runner_cancelled" ? "cancelled" : "error");
        if(process.error_code == "runner_timeout") result.set_verdict("failed");
        // Keep the runner verdict. Capture is independent of its exit
        // code; only bounded, owner/version-checked and privacy-safe data survive.
        if(!variant.instrumentation_config.null()) {
          try {
            if(observer) require(observer->flush_capture(), "evaluation_capture_not_persisted");
            const auto run = instrumented_run(variant, workspace.path, id, max_bytes);
            const auto trace = integration::boundary_trace(run);
            safe(trace);
            const auto validated = validate_trace(parse_trace_json(trace.dump()));
            bool bound = false;
            for(const auto& event : validated.trace.events()) if(event.kind() == "run.start") {
              const auto attributes = integration::json(event.attributes());
              bound = attributes.str("agent_version_id") == variant.version && attributes.str("evaluation_trial_id") == id;
            }
            require(bound && validated.trace.identity().user_id() == variant.artifact.str("user_id"),
                "evaluation_trace_version_mismatch");
            Json evidence{{"loaded_version", variant.version}, {"runner_error", process.error_code},
                {"exit_code", process.exit_code}};
            const bool complete = validated.report.trace_valid() && run.flag("closed", false) && !run.flag("incomplete", false);
            if(complete) {
              const auto metrics = integration::boundary_metrics(trace["events"]);
              evidence["trace"] = trace; evidence["metrics"] = metrics;
              evidence["trajectory"] = Json::parse(legacy::LoopEpisodeAnalyzer::to_json(analyze_trace(validated)));
              *result.mutable_metrics() = integration::proto<google::protobuf::Struct>(metrics);
            } else {
              evidence["failure_capture"] = Json{{"trace", trace}, {"complete", false},
                  {"validation", integration::json(validated.report)}};
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
          : instrumented_response(variant, workspace.path, id, max_bytes);
      integration::keys(response, {"schema_version", "trial_id", "execution_status", "output", "state",
          "metrics", "trace", "artifacts", "loaded_version"});
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
      if(!variant.artifact.null()) evidence["loaded_version"] = variant.version;
      require(evidence["state"].object(), "evaluation_state_invalid");
      if(response.has("trace")) evidence["trace"] = response["trace"];
      *result.mutable_evidence() = integration::proto<google::protobuf::Struct>(evidence);
      if(response.has("trace")) {
        const auto validated = validate_trace(parse_trace_json(response["trace"].dump()));
        require(validated.report.trace_valid(), "evaluation_trace_invalid");
        if(variant.artifact.null()) require(validated.trace.identity().run_id() == id, "evaluation_trace_identity_invalid");
        if(!variant.artifact.null()) {
          require(validated.trace.identity().user_id() == variant.artifact.str("user_id"), "evaluation_trace_owner_mismatch");
          bool bound = false;
          for(const auto& event : validated.trace.events()) if(event.kind() == "run.start") {
            const auto attributes = integration::json(event.attributes());
            bound = attributes.str("agent_version_id") == variant.version && attributes.str("evaluation_trial_id") == id;
          }
          require(bound, "evaluation_trace_version_mismatch");
          evidence["execution_run_id"] = validated.trace.identity().run_id();
        }
        for(const auto& event : validated.trace.events()) if(event.kind() == "run.end") {
          require(event.outcome().execution_status() == status, "evaluation_trace_outcome_mismatch");
          require(integration::json(event.output()).dump() == response["output"].dump(), "evaluation_trace_output_mismatch");
        }
        const auto analysis = analyze_trace(validated);
        if(response["trace"]["attributes"].str("protocol") == "evolution.boundary.v1") {
          const auto observed = integration::boundary_metrics(response["trace"]["events"]);
          for(const auto* key : {"steps", "tool_calls", "loop_count", "llm_calls", "tokens", "input_tokens", "output_tokens"}) {
            if(observed.has(key)) {
              if(metrics.has(key) && metrics[key].integer() != observed[key].integer())
                result.add_warnings(std::string("host_metric_mismatch:") + key);
              metrics[key] = observed[key];
            } else metrics.fields().erase(key);
          }
        }
        evidence["trace"] = response["trace"];
        evidence["trajectory"] = Json::parse(legacy::LoopEpisodeAnalyzer::to_json(analysis));
      } else {
        require(!require_trace, "evaluation_trace_required");
        result.add_warnings("trajectory_not_observed");
      }
      *result.mutable_metrics() = integration::proto<google::protobuf::Struct>(metrics);
      evidence["metrics"] = metrics;
      if(!metrics.has("steps") || !metrics.has("tool_calls")) result.add_warnings("step_or_tool_budget_unobserved");
      try {
        evidence["artifacts"] = artifacts(response, workspace.path, output / "artifacts" / id, max_artifact_bytes);
      } catch(const Error& error) {
        const std::string code = error.what();
        if(code == "evaluation_write_failed" || code == "evaluation_directory_exists" || code == "evaluation_file_exists") throw;
        result.set_verdict("failed"); result.set_error_code(code);
        auto* check = result.add_checks(); check->set_check_id("artifact"); check->set_status("failed");
        check->set_reason("声明的产物无法通过文件、格式或隐私验收。"); return result;
      }
      safe(evidence);
      *result.mutable_evidence() = integration::proto<google::protobuf::Struct>(evidence);
      if(status == "environment_error" || status == "cancelled") {
        result.set_error_code(status == "environment_error" ? "runner_environment_error" : "runner_cancelled");
        return result;
      }
      // Host execution outcome and business acceptance remain separate. A
      // post-processing failure does not erase a verifiably completed artifact.
      bool failed = false;
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
        *result.add_checks() = std::move(check);
      }
      result.set_verdict(failed ? "failed" : "passed");
      // A semantic grader cannot override a deterministic failure. Skip those
      // paid calls; model outages are unscored errors, never successful trials.
      if(model_enabled && !failed) {
        auto judgment = model_grade(task, evidence);
        result.set_verdict(judgment.status());
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
          "PNG 的 C++ 内置检查为文件头和尺寸；完整解码或画面语义需要宿主提供相应验收证据。\n";
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
      "max_output_bytes", "max_artifact_bytes", "max_report_bytes", "require_trace", "variants", "checks", "judge", "resources", "task_ids"});
  require(config.num("schema_version", 0) == 1, "evaluation_config_version_invalid");
  const auto repetitions = bounded(config, "repetitions", 1, 1, 100);
  const auto parallelism = bounded(config, "parallelism", 1, 1, 16);
  const auto max_trials = bounded(config, "max_trials", 1000, 1, 10000);
  const auto max_report_bytes = bounded(config, "max_report_bytes", 33554432, 1024, 134217728);
  evaluator.timeout = bounded(config, "timeout_ms", 1800000, 1, 86400000);
  evaluator.max_bytes = bounded(config, "max_output_bytes", 1048576, 128, 8388608);
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
  const auto& variants = config["variants"].items();
  require(!variants.empty() && variants.size() <= 8, "evaluation_variant_limit");
  for(const auto& value : variants) {
    integration::keys(value, {"id", "version", "command", "pass_env", "artifact", "instrumentation_config", "instrumentation_transport"});
    label(value.str("id")); label(value.str("version"));
    require(variant_ids.insert(value.str("id")).second, "evaluation_variant_duplicate");
    if(value.has("artifact"))
      require(value.str("version") == agent_version_id(value["artifact"].dump()), "evaluation_version_invalid");
    Variant variant{value.str("id"), command(value, evaluator.config_dir), environment(value), value.str("version"), value["artifact"], nullptr};
    variant.instrumentation_transport = value.str("instrumentation_transport", "native");
    require(variant.instrumentation_transport == "native" || variant.instrumentation_transport == "http", "evaluation_transport_invalid");
    require(!value.has("instrumentation_transport") || value.has("instrumentation_config"), "evaluation_instrumentation_config_required");
    if(value.has("instrumentation_config")) {
      const auto path = fs::absolute(evaluator.resolve(value.str("instrumentation_config")));
      variant.instrumentation_config = Json::parse(read_file(path, 262144));
      auto& settings = variant.instrumentation_config;
      for(const auto* key : {"runtime_config", "experience_template_file"}) {
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
  for(const auto* task : tasks) {
    std::set<std::string> ids{"model", "privacy", "execution", "budget.steps", "budget.tool_calls"};
    for(const auto& check : evaluator.checks(task->task_id()))
      require(ids.insert(check.str("id")).second, "evaluation_check_duplicate");
    require(!evaluator.checks(task->task_id()).empty() || config["judge"].flag("enabled", false),
            "evaluation_task_has_no_grader");
  }
  const auto total = tasks.size() * repetitions * variants.size();
  require(total <= max_trials, "evaluation_trial_limit");
  evaluator.initialize_model();
  dv::TaskEvaluationReport report;
  report.set_schema_version(1); report.set_report_kind("task_evaluation.v1");
  report.set_dataset_version(dataset.dataset_version()); report.set_evaluator_version("task-evaluator-v1");
  report.set_config_fingerprint(hash(config.dump() + evaluator.judge_prompt + evaluator.model_config_fingerprint));
  report.set_run_id("evaluation:" + hash(dataset.dataset_version() + config.dump()
      + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + std::to_string(getpid())));
  // Configuration is provenance, not credentials. Never persist resolved env
  // values or absolute runner/runtime paths; retain explicit version labels.
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
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> retained_bytes{0};
  std::exception_ptr failure;
  std::mutex failure_mutex;
  auto work = [&] {
    while(true) {
      const auto index = next.fetch_add(1); if(index >= total) return;
      try {
        const auto& job = jobs[index];
        const auto id = hash(report.run_id() + ":" + std::to_string(index)).substr(0, 32);
        results[index] = evaluator.trial(*job.task, job.variant, job.repetition, id);
        require(retained_bytes.fetch_add(results[index].ByteSizeLong()) + results[index].ByteSizeLong()
            <= max_report_bytes / 2, "evaluation_report_size_limit");
        // Persist each finished trial. A crash leaves useful checkpoints, but
        // without the final manifest it is never a completed evaluation batch.
        write_file(evaluator.output / "trials" / (id + ".json"), integration::json(results[index]).dump() + "\n");
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
  *report.mutable_summary() = integration::proto<google::protobuf::Struct>(summary);
  *report.mutable_comparisons() = integration::proto<google::protobuf::Struct>(comparisons(evaluator.variants, report));
  report.set_complete(true);
  const auto json = integration::json(report).dump() + "\n";
  require(json.size() <= 134217728, "evaluation_report_size_limit");
  const auto pb = report.SerializeAsString();
  const auto markdown = markdown_report(report);
  write_file(evaluator.output / "report.json", json);
  write_file(evaluator.output / "report.pb", pb);
  write_file(evaluator.output / "report.md", markdown);
  write_file(evaluator.output / "results.jsonl", jsonl);
  write_file(evaluator.output / "manifest.json", Json{{"complete", true}, {"run_id", report.run_id()},
      {"dataset_version", dataset.dataset_version()}, {"summary", summary},
      {"files", Json{{"report.json", hash(json)}, {"report.pb", hash(pb)}, {"report.md", hash(markdown)}, {"results.jsonl", hash(jsonl)}}}}.dump() + "\n");
  fs::remove(evaluator.output / ".work");
  return report;
}

}  // namespace trajectory::evolution
