#include "trajectory_evolution/dataset.h"
#include "trajectory_evolution/trace.h"
#include "agentflow/trajectory/runtime_config.h"
#include "integration_json.h"
#include "local_support.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <openssl/sha.h>
#include <regex>
#include <set>
#include <sstream>

namespace trajectory::evolution {
namespace {
namespace fs = std::filesystem;
namespace legacy = agentflow::trajectory;
namespace dv = dataset::v1;
using integration::Json;
using integration::Error;
using integration::require;

constexpr std::size_t kMaxDatasetBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxTaskBytes = 128U * 1024U;
constexpr const char* kSplits[] = {"train", "val", "holdout"};

std::string digest(const std::string& text) {
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), bytes);
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for(auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}

std::string read(const fs::path& path, std::size_t limit) {
  require(fs::is_regular_file(path), "dataset_input_not_regular");
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(stream.good() && stream.tellg() >= 0
      && static_cast<std::uint64_t>(stream.tellg()) <= limit, "dataset_input_size_invalid");
  std::string bytes(static_cast<std::size_t>(stream.tellg()), '\0');
  stream.seekg(0);
  require(bytes.empty() || bool(stream.read(bytes.data(), bytes.size())), "dataset_read_failed");
  return bytes;
}

void write(const fs::path& path, const std::string& bytes) {
  require(!fs::exists(fs::symlink_status(path)), "dataset_file_exists");
  std::ofstream stream(path, std::ios::binary);
  require(bool(stream.write(bytes.data(), bytes.size()).flush()), "dataset_write_failed");
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
}

std::string canonical(const google::protobuf::Message& value) {
  return integration::json(value).dump();
}

void safe_paths(const Json& value) {
  // Absolute private locations are not portable fixtures or suitable exports.
  static const std::regex private_path(
      R"((^|[\s"'=])/(root|home|tmp|var|etc|mnt)/[^\s"']+|\b[A-Za-z]:\\Users\\)",
      std::regex::icase);
  if(value.string()) require(!std::regex_search(value.text(), private_path), "dataset_private_path_rejected");
  if(value.object()) for(const auto& field : value.fields()) {
    require(field.first != "source_path" && field.first != "storage_path"
        && field.first != "local_path", "dataset_private_field_rejected");
    safe_paths(field.second);
  }
  if(value.array()) for(const auto& item : value.items()) safe_paths(item);
}

void safe(const Json& value) {
  integration::validate_local_data(value);
  safe_paths(value);
}

void remove_masked_fields(Json& value) {
  if(value.object()) {
    for(auto it = value.fields().begin(); it != value.fields().end();) {
      const auto mask = it->second.string() ? it->second.text() : std::string();
      if(mask == "<REDACTED:FIELD>" || mask == "<REDACTED:ENV_SECRET>"
          || mask == "<REDACTED:CREDENTIAL>") {
        it = value.fields().erase(it);
      } else {
        remove_masked_fields(it->second); ++it;
      }
    }
  }
  if(value.array()) for(auto& item : value.items()) remove_masked_fields(item);
}

void text_field(const std::string& text, std::size_t limit) {
  require(!text.empty() && text.size() <= limit && text.find('\0') == std::string::npos
      && text.find_first_not_of(" \r\n\t") != std::string::npos, "dataset_text_invalid");
}

std::string normalized(std::string text) {
  std::string out;
  bool space = false;
  for(unsigned char c : text) {
    if(c == ' ' || c == '\n' || c == '\r' || c == '\t') { space = !out.empty(); continue; }
    if(space) out += ' ';
    space = false;
    out += static_cast<char>(c);
  }
  return out;
}

std::string input_fingerprint(const dv::EvaluationTask& task) {
  Json turns = Json::Array{normalized(task.task_input())};
  for(const auto& turn : task.followup_inputs()) turns.push(normalized(turn));
  return digest(Json{{"turns", turns}, {"initial_state", integration::json(task.initial_state())}}.dump());
}

std::string task_version(dv::EvaluationTask task) {
  task.clear_task_version();
  return "task-v1:" + digest(canonical(task));
}

std::string dataset_version(dv::EvaluationDataset dataset) {
  dataset.clear_dataset_version();
  return "dataset-v1:" + digest(canonical(dataset));
}

void check_task(const dv::EvaluationTask& task, bool frozen) {
  require(integration::identifier(task.task_id()) && integration::identifier(task.group_id()),
          "dataset_identity_invalid");
  text_field(task.task_input(), 32768);
  text_field(task.expected_behavior(), 32768);
  text_field(task.category(), 128);
  require(task.difficulty() == "easy" || task.difficulty() == "medium" || task.difficulty() == "hard",
          "dataset_difficulty_invalid");
  require(task.review_status() == "pending" || task.review_status() == "reviewed", "dataset_review_invalid");
  require(task.followup_inputs_size() <= 32 && task.constraints_size() <= 64, "dataset_task_limit");
  for(const auto& turn : task.followup_inputs()) text_field(turn, 32768);
  for(const auto& constraint : task.constraints()) text_field(constraint, 8192);
  require(task.budget().max_steps() > 0 && task.budget().max_steps() <= 10000
      && task.budget().max_tool_calls() > 0 && task.budget().max_tool_calls() <= 100000
      && task.budget().timeout_ms() > 0 && task.budget().timeout_ms() <= 86400000,
      "dataset_budget_invalid");
  const auto& origin = task.provenance();
  require(origin.kind() == "golden" || origin.kind() == "synthetic" || origin.kind() == "traces",
          "dataset_source_kind_invalid");
  require(origin.source_fingerprints_size() > 0 && origin.source_fingerprints_size() <= 1024,
          "dataset_provenance_missing");
  static const std::regex hash("[a-f0-9]{64}");
  for(const auto& ref : origin.source_fingerprints())
    require(std::regex_match(ref, hash), "dataset_source_fingerprint_invalid");
  const auto bytes = canonical(task);
  require(bytes.size() <= kMaxTaskBytes, "dataset_task_too_large");
  safe(Json::parse(bytes));
  if(frozen) {
    require(task.split() == "train" || task.split() == "val" || task.split() == "holdout",
            "dataset_split_invalid");
    require(!origin.learning_exposed() || task.split() == "train", "dataset_learning_leakage");
    require(task.task_version() == task_version(task), "dataset_task_version_mismatch");
  }
}

void issue(dv::EvaluationDataset& result, const char* severity, const char* code,
           std::size_t source, std::size_t item) {
  require(result.issues_size() < 10000, "dataset_issue_limit");
  auto* value = result.add_issues();
  value->set_severity(severity); value->set_code(code);
  value->set_source_index(source); value->set_item_index(item);
}

std::string split_for(const dv::EvaluationDataset& dataset, const std::string& group) {
  const auto bucket = std::stoul(digest(dataset.split_seed() + "\n" + group).substr(0, 8), nullptr, 16) % 100;
  if(bucket < dataset.split_percent(0)) return "train";
  if(bucket < dataset.split_percent(0) + dataset.split_percent(1)) return "val";
  return "holdout";
}

void finalize(dv::EvaluationDataset& dataset) {
  std::map<std::string, std::string> parent;
  auto root = [&](std::string group) {
    while(parent.count(group) && parent.at(group) != group) group = parent.at(group);
    return group;
  };
  std::map<std::string, dv::EvaluationTask> unique;
  std::map<std::string, std::string> ids;
  for(const auto& task : dataset.tasks()) {
    const auto fp = input_fingerprint(task);
    require(!ids.count(task.task_id()) || ids.at(task.task_id()) == fp, "dataset_task_id_conflict");
    ids[task.task_id()] = fp;
    parent.emplace(task.group_id(), task.group_id());
    auto found = unique.find(fp);
    if(found == unique.end()) { unique.emplace(fp, task); continue; }
    auto& previous = found->second;
    // Conflicting rubrics are not silently resolved by whichever file arrived first.
    require(normalized(previous.expected_behavior()) == normalized(task.expected_behavior())
        && canonical(previous.budget()) == canonical(task.budget())
        && previous.constraints_size() == task.constraints_size()
        && std::equal(previous.constraints().begin(), previous.constraints().end(), task.constraints().begin()),
        "dataset_duplicate_rubric_conflict");
    require(previous.split().empty() || task.split().empty() || previous.split() == task.split(),
            "dataset_duplicate_split_conflict");
    const auto left = root(previous.group_id()), right = root(task.group_id());
    parent[std::max(left, right)] = std::min(left, right);
    if(previous.split().empty()) previous.set_split(task.split());
    if(task.review_status() == "pending") previous.set_review_status("pending");
    previous.mutable_provenance()->set_learning_exposed(
        previous.provenance().learning_exposed() || task.provenance().learning_exposed());
    for(const auto& ref : task.provenance().source_fingerprints())
      previous.mutable_provenance()->add_source_fingerprints(ref);
    issue(dataset, "info", "duplicate_task_merged", 0, 0);
  }
  struct Group { bool exposed = false; std::string requested; };
  std::map<std::string, Group> groups;
  for(auto& item : unique) {
    auto& task = item.second;
    task.set_group_id(root(task.group_id()));
    auto& group = groups[task.group_id()];
    group.exposed |= task.provenance().learning_exposed();
    if(!task.split().empty()) {
      require(group.requested.empty() || group.requested == task.split(), "dataset_group_split_conflict");
      group.requested = task.split();
    }
  }
  dataset.clear_tasks();
  for(auto& item : unique) {
    auto& task = item.second;
    const auto& group = groups.at(task.group_id());
    require(!group.exposed || group.requested.empty() || group.requested == "train", "dataset_learning_leakage");
    task.set_split(group.exposed ? "train" : group.requested.empty()
        ? split_for(dataset, task.group_id()) : group.requested);
    // Conservatively propagate exposure to the whole scenario family.
    task.mutable_provenance()->set_learning_exposed(group.exposed);
    auto* refs = task.mutable_provenance()->mutable_source_fingerprints();
    std::sort(refs->begin(), refs->end());
    refs->erase(std::unique(refs->begin(), refs->end()), refs->end());
    task.set_task_version(task_version(task));
    check_task(task, true);
    *dataset.add_tasks() = std::move(task);
  }
  std::sort(dataset.mutable_tasks()->begin(), dataset.mutable_tasks()->end(),
      [](const auto& a, const auto& b) { return a.task_id() < b.task_id(); });
  for(auto split : kSplits) {
    const bool present = std::any_of(dataset.tasks().begin(), dataset.tasks().end(),
        [&](const auto& task) { return task.split() == split; });
    if(!present) issue(dataset, "warning", split == std::string("train") ? "empty_train"
        : split == std::string("val") ? "empty_val" : "empty_holdout", 0, 0);
  }
  dataset.set_dataset_version(dataset_version(dataset));
  require(dataset.ByteSizeLong() <= kMaxDatasetBytes, "dataset_too_large");
}

void check_dataset(const dv::EvaluationDataset& dataset) {
  require(dataset.schema_version() == 1 && dataset.dataset_kind() == "evaluation_dataset.v1"
      && dataset.generator_version() == "dataset-builder-v1", "dataset_schema_invalid");
  require(dataset.split_percent_size() == 3 && dataset.split_percent(0) > 0
      && dataset.split_percent(1) > 0 && dataset.split_percent(2) > 0
      && std::uint64_t(dataset.split_percent(0)) + dataset.split_percent(1) + dataset.split_percent(2) == 100,
      "dataset_split_percent_invalid");
  text_field(dataset.name(), 128); text_field(dataset.split_seed(), 128);
  std::set<std::string> ids, inputs;
  std::map<std::string, std::string> splits;
  require(dataset.tasks_size() <= 10000, "dataset_task_limit");
  for(const auto& task : dataset.tasks()) {
    check_task(task, true);
    require(ids.insert(task.task_id()).second && inputs.insert(input_fingerprint(task)).second,
            "dataset_duplicate_task");
    require(!splits.count(task.group_id()) || splits[task.group_id()] == task.split(), "dataset_group_split_conflict");
    splits[task.group_id()] = task.split();
  }
  require(dataset.dataset_version() == dataset_version(dataset), "dataset_version_mismatch");
  safe(integration::json(dataset));
}

std::map<std::string, std::string> artifacts(const dv::EvaluationDataset& dataset) {
  std::map<std::string, std::string> files;
  files["dataset.pb"] = dataset.SerializeAsString();
  for(auto split : kSplits) files[std::string(split) + ".jsonl"] = "";
  for(const auto& task : dataset.tasks()) files[task.split() + ".jsonl"] += canonical(task) + "\n";
  return files;
}

struct Builder {
  Json config;
  fs::path directory;
  DatasetBuildOptions options;
  dv::EvaluationDataset result;
  dv::TaskBudget budget;
  std::size_t max_tasks = 0, max_bytes = 0, max_calls = 0;
  std::size_t task_bytes = 0;
  std::shared_ptr<legacy::ModelAdapter> adapter;
  std::unique_ptr<legacy::RuntimeConfiguration> runtime;
  std::string prompt;

  fs::path resolve(const std::string& configured) const {
    require(!configured.empty(), "dataset_path_required");
    return fs::weakly_canonical(directory / fs::path(configured));
  }

  void initialize_model() {
    if(runtime) {
      require(adapter != nullptr, "dataset_model_initialization_failed");
      return;
    }
    const auto path = options.model_config.empty()
        ? resolve(config.str("runtime_config")) : options.model_config;
    runtime = std::make_unique<legacy::RuntimeConfiguration>(legacy::load_runtime_configuration(path));
    prompt = read(resolve(config.str("generation_prompt")), 262144);
    text_field(prompt, 262144);
    safe(Json(prompt));
    adapter = options.model_adapter;
    if(adapter) return;
    auto http = runtime->model;
    // Explicit runtime values win. Environment fallback does not read .env files.
    if(http.endpoint.empty()) {
      const auto* base = std::getenv("OPENAI_BASE_URL");
      if(base) {
        http.endpoint = base;
        while(!http.endpoint.empty() && http.endpoint.back() == '/') http.endpoint.pop_back();
        http.endpoint += "/chat/completions";
      }
    }
    if(http.model_id.empty()) {
      const auto* model = std::getenv("OPENAI_MODEL");
      if(model) http.model_id = model;
    }
    require(!http.endpoint.empty() && !http.model_id.empty(), "dataset_model_not_configured");
    if(!runtime->no_api_key) {
      const std::string env = runtime->api_key_env.empty() ? "OPENAI_API_KEY" : runtime->api_key_env;
      const auto* key = std::getenv(env.c_str());
      require(key && *key, "dataset_model_key_missing");
      http.api_key = key;
      runtime->evaluator.redaction_secrets.push_back(key);
    }
    http.max_output_tokens = config.num("generation_max_output_tokens", 8192);
    adapter = legacy::make_openai_compatible_model_adapter(http);
  }

  void add(Json value, const std::string& kind, const std::string& source_hash,
           const std::string& group, const std::string& model_hash, std::size_t si, std::size_t row) {
    try {
      require(static_cast<std::size_t>(result.tasks_size()) < max_tasks, "dataset_task_limit");
      if(kind != "golden") {
        integration::keys(value, {"task_input", "followup_inputs", "expected_behavior", "category",
            "difficulty", "initial_state", "constraints", "attributes"});
      }
      safe(value);
      auto task = integration::proto<dv::EvaluationTask>(value);
      if(task.task_id().empty()) task.set_task_id("task:" + input_fingerprint(task));
      if(task.review_status().empty()) task.set_review_status("pending");
      if(!task.has_budget()) *task.mutable_budget() = budget;
      auto* origin = task.mutable_provenance();
      if(kind != "golden") {
        task.set_group_id(group);
        task.set_review_status("pending");
        task.clear_split();
        origin->set_kind(kind);
        // Existing trajectories may already have entered memory. Synthetic
        // generations are search material, not independent proof of improvement.
        origin->set_learning_exposed(true);
        origin->set_model_id(adapter->model_id());
        origin->set_prompt_fingerprint(model_hash);
      } else if(origin->kind().empty()) {
        origin->set_kind("golden");
      }
      if(config.has("learning_groups")) for(const auto& exposed : config["learning_groups"].items())
        if(exposed.text() == task.group_id()) origin->set_learning_exposed(true);
      origin->add_source_fingerprints(source_hash);
      task.clear_task_version();
      check_task(task, false);
      require(task_bytes + task.ByteSizeLong() <= kMaxDatasetBytes / 2, "dataset_total_task_bytes_limit");
      task_bytes += task.ByteSizeLong();
      *result.add_tasks() = std::move(task);
    } catch(const Error& error) {
      issue(result, "error", error.what(), si, row);
    } catch(...) {
      issue(result, "error", "dataset_case_rejected", si, row);
    }
  }

  void generate(const Json& evidence, const std::string& kind, const std::string& source_hash,
                const std::string& group, std::size_t count, std::size_t si, std::size_t index) {
    require(result.model_calls() < max_calls, "dataset_model_call_limit");
    Json request{{"source_kind", kind}, {"requested_count", count}, {"untrusted_evidence", evidence}};
    safe(request);
    const auto body = request.dump();
    require(body.size() <= runtime->model.max_request_bytes, "dataset_model_input_too_large");
    legacy::ModelRequest query;
    query.system_prompt = prompt;
    query.user_prompt = body;
    query.packet_id = "dataset:" + digest(prompt + "\n" + body);
    result.set_model_calls(result.model_calls() + 1);
    legacy::RawModelResponse response;
    try { response = adapter->evaluate(query); }
    catch(...) { throw Error("dataset_model_exception"); }
    if(!response.transport_ok || !response.error_code.empty()) {
      // Only known adapter codes may cross the diagnostics boundary.
      static const std::regex code(R"(http_[0-9]{3}|transport_[0-9]+|output_token_limit_reached|invalid_api_response|response_too_large|request_too_large)");
      if(std::regex_match(response.error_code, code))
        throw Error(("dataset_model_" + response.error_code).c_str());
      throw Error("dataset_model_failed");
    }
    require(response.body.size() <= runtime->model.max_response_bytes, "dataset_model_output_too_large");
    const auto generated = Json::parse(response.body);
    safe(generated);
    integration::keys(generated, {"cases"});
    require(generated["cases"].array() && generated["cases"].items().size() <= count,
            "dataset_model_contract_invalid");
    if(generated["cases"].items().size() != count)
      issue(result, "error", "dataset_generated_count_short", si, index);
    for(const auto& item : generated["cases"].items())
      add(item, kind, source_hash, group, digest(prompt), si, ++index);
  }

  void source(const Json& spec, std::size_t si) {
    integration::keys(spec, {"kind", "path", "count", "batch_size", "max_files"});
    const auto kind = spec.str("kind");
    require(kind == "golden" || kind == "synthetic" || kind == "traces", "dataset_source_kind_invalid");
    const auto path = resolve(spec.str("path"));
    if(kind == "golden") {
      const auto bytes = read(path, max_bytes);
      const auto fp = digest(bytes);
      std::istringstream lines(bytes);
      std::string line;
      std::size_t row = 0;
      while(std::getline(lines, line)) {
        ++row;
        require(row <= 100000, "dataset_line_limit");
        if(normalized(line).empty()) continue;
        if(static_cast<std::size_t>(result.tasks_size()) >= max_tasks)
          throw Error("dataset_task_limit");
        try { add(Json::parse(line), kind, fp, {}, {}, si, row); }
        catch(...) { issue(result, "error", "dataset_jsonl_invalid", si, row); }
      }
      return;
    }
    initialize_model();
    if(kind == "synthetic") {
      const auto bytes = read(path, max_bytes);
      safe(Json(bytes));
      const auto count = spec.num("count", 20), batch = spec.num("batch_size", 5);
      require(count > 0 && count <= max_tasks && batch > 0 && batch <= 20,
              "dataset_generation_count_invalid");
      const auto fp = digest(bytes);
      for(std::size_t offset = 0; offset < count; offset += batch) {
        Json previous = Json::Array{};
        for(const auto& task : result.tasks()) if(task.group_id() == "synthetic:" + fp)
          previous.push(task.task_input());
        generate(Json{{"specification", bytes}, {"already_generated_inputs", previous}, {"offset", offset}},
            kind, fp, "synthetic:" + fp, std::min(batch, count - offset), si, offset);
      }
      return;
    }
    const auto max_files = spec.num("max_files", 20);
    require(max_files > 0 && max_files <= 1000, "dataset_file_limit_invalid");
    std::vector<fs::path> paths;
    if(fs::is_regular_file(path)) paths.push_back(path);
    else {
      require(fs::is_directory(path), "dataset_trace_directory_invalid");
      for(const auto& entry : fs::recursive_directory_iterator(path)) {
        if(entry.is_symlink() || !entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if(name.size() >= 6 && (name.substr(name.size() - 6) == ".jsonl" || is_trace_path(entry.path())))
          paths.push_back(entry.path());
        require(paths.size() <= max_files, "dataset_trace_file_limit");
      }
    }
    require(!paths.empty(), "dataset_no_trace_files");
    std::sort(paths.begin(), paths.end());
    std::size_t index = 0;
    for(const auto& file : paths) {
      ++index;
      try {
        const auto bytes = read(file, max_bytes);
        auto evaluator_options = runtime->evaluator;
        evaluator_options.model_selection = legacy::ModelSelection::all;
        evaluator_options.strict_model = false;
        evaluator_options.allow_invalid_trace = false;
        evaluator_options.max_model_calls = 1;
        evaluator_options.max_file_bytes = max_bytes;
        auto templates = std::make_shared<legacy::PromptTemplates>(*evaluator_options.prompts);
        // Reuse the existing sanitizer, without sending an evaluation request.
        templates->dialogue_user = R"({"selected_episode_count":{{selected_episode_count}},"evidence":{{evidence_json}}})";
        if(templates->standard_dialogue_system.empty()) templates->standard_dialogue_system = templates->dialogue_system;
        evaluator_options.prompts = templates;
        legacy::HybridTrajectoryEvaluator evaluator(evaluator_options, nullptr);
        const auto evaluated = is_trace_path(file) ? evaluator.evaluate_file(file)
            : evaluator.evaluate_sessionlog(file);
        require(evaluated.report.source_trace_integrity_valid() && evaluated.model_requests.size() == 1,
                "dataset_trace_not_usable");
        const auto packet = Json::parse(evaluated.model_requests.front().user_prompt);
        const auto& identity = evaluated.report.source();
        // Hash structured components to avoid delimiter collisions and raw identities.
        const auto group = identity.conversation_id().empty() ? digest(bytes)
            : digest(Json(Json::Array{identity.user_id(), identity.conversation_id()}).dump());
        Json evidence = packet["evidence"];
        // Identifiers are for local lineage, not needed by the generator model.
        evidence.fields().erase("source");
        // Already-masked credential fields are omitted, not sent as credentials.
        // Any remaining sensitive field still fails the shared privacy gate.
        remove_masked_fields(evidence);
        generate(evidence, kind, digest(bytes), "conversation:" + group, 1, si, index);
      } catch(const Error& error) { issue(result, "error", error.what(), si, index); }
      catch(...) { issue(result, "error", "dataset_trace_rejected", si, index); }
      if(result.model_calls() >= max_calls && index < paths.size()) {
        issue(result, "error", "dataset_model_call_limit", si, index + 1); break;
      }
    }
  }
};

}  // namespace

dv::EvaluationDataset build_evaluation_dataset(const fs::path& config_path,
                                              const DatasetBuildOptions& options) {
  Builder builder;
  builder.config = Json::parse(read(config_path, 1048576));
  builder.directory = fs::absolute(config_path).parent_path();
  builder.options = options;
  const auto& config = builder.config;
  integration::keys(config, {"schema_version", "name", "split_seed", "split_percent", "max_tasks",
      "max_input_bytes", "max_model_calls", "generation_max_output_tokens", "runtime_config",
      "generation_prompt", "budget", "sources", "learning_groups"});
  require(config.num("schema_version", 0) == 1, "dataset_config_version_invalid");
  auto& result = builder.result;
  result.set_schema_version(1); result.set_dataset_kind("evaluation_dataset.v1");
  result.set_generator_version("dataset-builder-v1");
  result.set_name(config.str("name")); result.set_split_seed(config.str("split_seed"));
  text_field(result.name(), 128); text_field(result.split_seed(), 128);
  const auto& percents = config["split_percent"].items();
  require(percents.size() == 3, "dataset_split_percent_invalid");
  std::size_t total = 0;
  for(const auto& item : percents) {
    const auto n = item.integer(); require(n > 0 && n < 100, "dataset_split_percent_invalid");
    result.add_split_percent(n); total += n;
  }
  require(total == 100, "dataset_split_percent_invalid");
  builder.budget = integration::proto<dv::TaskBudget>(config["budget"]);
  require(builder.budget.max_steps() > 0 && builder.budget.max_steps() <= 10000
      && builder.budget.max_tool_calls() > 0 && builder.budget.max_tool_calls() <= 100000
      && builder.budget.timeout_ms() > 0 && builder.budget.timeout_ms() <= 86400000,
      "dataset_budget_invalid");
  if(config.has("learning_groups")) {
    require(config["learning_groups"].items().size() <= 10000, "dataset_learning_group_limit");
    for(const auto& group : config["learning_groups"].items()) text_field(group.text(), 128);
  }
  builder.max_tasks = config.num("max_tasks", 1000);
  builder.max_bytes = config.num("max_input_bytes", 8388608);
  builder.max_calls = config.num("max_model_calls", 8);
  const auto output_tokens = config.num("generation_max_output_tokens", 8192);
  require(output_tokens > 0 && output_tokens <= 32768, "dataset_model_token_limit_invalid");
  require(builder.max_tasks > 0 && builder.max_tasks <= 10000 && builder.max_bytes > 0
      && builder.max_bytes <= kMaxDatasetBytes && builder.max_calls <= 1000, "dataset_limits_invalid");
  const auto& sources = config["sources"].items();
  require(!sources.empty() && sources.size() <= 1000, "dataset_sources_invalid");
  for(std::size_t i = 0; i < sources.size(); ++i) {
    try { builder.source(sources[i], i + 1); }
    catch(const Error& error) { issue(result, "error", error.what(), i + 1, 0); }
    catch(...) { issue(result, "error", "dataset_source_rejected", i + 1, 0); }
  }
  finalize(result);
  check_dataset(result);
  return result;
}

std::string evaluation_dataset_summary(const dv::EvaluationDataset& dataset) {
  std::size_t rejected = 0, pending = 0;
  Json counts = Json::Object{};
  std::set<std::string> groups;
  for(auto split : kSplits) counts[split] = std::size_t(0);
  for(const auto& task : dataset.tasks()) {
    counts[task.split()] = counts[task.split()].integer() + 1;
    pending += task.review_status() != "reviewed";
    groups.insert(task.group_id());
  }
  for(const auto& item : dataset.issues()) rejected += item.severity() == "error";
  return Json{{"dataset_version", dataset.dataset_version()}, {"task_count", std::size_t(dataset.tasks_size())},
      {"group_count", groups.size()}, {"splits", counts}, {"pending_review_count", pending},
      {"rejected_count", rejected}, {"model_calls", std::size_t(dataset.model_calls())},
      {"review_complete", !dataset.tasks().empty() && pending == 0 && rejected == 0},
      {"l4_ready", false}, {"status", rejected ? "partial" : dataset.tasks().empty() ? "empty" : "built"}}.dump();
}

void write_evaluation_dataset(const dv::EvaluationDataset& dataset, const fs::path& directory) {
  check_dataset(dataset);
  require(!fs::exists(fs::symlink_status(directory)), "dataset_output_exists");
  if(!directory.parent_path().empty()) fs::create_directories(directory.parent_path());
  require(fs::create_directory(directory), "dataset_output_exists");
  fs::permissions(directory, fs::perms::owner_all);
  Json hashes = Json::Object{};
  for(const auto& entry : artifacts(dataset)) {
    write(directory / entry.first, entry.second);
    hashes[entry.first] = digest(entry.second);
  }
  Json metadata = integration::json(dataset);
  metadata.fields().erase("tasks");
  const Json manifest{{"metadata", metadata}, {"files", hashes},
      {"summary", Json::parse(evaluation_dataset_summary(dataset))}};
  write(directory / "manifest.json", manifest.dump() + "\n");
}

dv::EvaluationDataset read_evaluation_dataset(const fs::path& directory) {
  const auto load = [&](const std::string& name) {
    require(!fs::is_symlink(directory / name), "dataset_artifact_symlink");
    return read(directory / name, kMaxDatasetBytes);
  };
  const auto manifest = Json::parse(load("manifest.json"));
  integration::keys(manifest, {"metadata", "files", "summary"});
  integration::keys(manifest["files"], {"dataset.pb", "train.jsonl", "val.jsonl", "holdout.jsonl"});
  dv::EvaluationDataset result;
  const auto bytes = load("dataset.pb");
  require(result.ParseFromString(bytes), "dataset_protobuf_invalid");
  check_dataset(result);
  for(const auto& entry : artifacts(result)) {
    const auto stored = entry.first == "dataset.pb" ? bytes : load(entry.first);
    require(digest(stored) == manifest["files"].str(entry.first), "dataset_artifact_hash_mismatch");
    if(entry.first != "dataset.pb") require(stored == entry.second, "dataset_split_content_mismatch");
  }
  Json metadata = integration::json(result);
  metadata.fields().erase("tasks");
  require(metadata.dump() == manifest["metadata"].dump()
      && evaluation_dataset_summary(result) == manifest["summary"].dump(), "dataset_manifest_mismatch");
  return result;
}

}  // namespace trajectory::evolution
