#include "agentflow/trajectory/runtime_config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

namespace agentflow::trajectory {
namespace {

using JsonValue = rapidjson::Value;

constexpr std::size_t kMaxConfigurationBytes = 1024U * 1024U;
constexpr std::size_t kMaxPromptTemplateBytes = 256U * 1024U;
constexpr std::size_t kMaxPromptTemplateCount = 8U;

std::filesystem::path normalized_existing_file(
    const std::filesystem::path& path,
    const std::string& description) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  if(error) {
    throw EvaluationError("cannot resolve " + description + ": " + error.message());
  }
  const auto canonical = std::filesystem::weakly_canonical(absolute, error);
  if(error || !std::filesystem::is_regular_file(canonical, error) || error) {
    throw EvaluationError(description + " is not a readable regular file");
  }
  return canonical.lexically_normal();
}

std::string read_bounded_text(
    const std::filesystem::path& path,
    std::size_t max_bytes,
    const std::string& description) {
  std::ifstream input(path, std::ios::binary);
  if(!input) {
    throw EvaluationError("cannot open " + description);
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if(size < 0 || static_cast<unsigned long long>(size) > max_bytes) {
    throw EvaluationError(description + " exceeds its byte limit");
  }
  input.seekg(0, std::ios::beg);
  std::string output(static_cast<std::size_t>(size), '\0');
  if(size > 0) {
    input.read(output.data(), size);
  }
  if(!input && size > 0) {
    throw EvaluationError("failed to read " + description);
  }
  if(output.find('\0') != std::string::npos) {
    throw EvaluationError(description + " contains a NUL byte");
  }
  return output;
}

const JsonValue& required_member(
    const JsonValue& object,
    const char* name,
    const std::string& context) {
  if(!object.IsObject() || !object.HasMember(name)) {
    throw EvaluationError(context + "." + name + " is required");
  }
  return object[name];
}

const JsonValue& required_object(
    const JsonValue& object,
    const char* name,
    const std::string& context) {
  const auto& value = required_member(object, name, context);
  if(!value.IsObject()) {
    throw EvaluationError(context + "." + name + " must be an object");
  }
  return value;
}

void require_known_members(
    const JsonValue& object,
    const std::unordered_set<std::string>& allowed,
    const std::string& context) {
  std::unordered_set<std::string> observed;
  for(auto member = object.MemberBegin(); member != object.MemberEnd(); ++member) {
    const std::string name(member->name.GetString(), member->name.GetStringLength());
    if(!observed.insert(name).second) {
      throw EvaluationError(context + " contains duplicate key: " + name);
    }
    if(allowed.find(name) == allowed.end()) {
      throw EvaluationError(context + " contains unknown key: " + name);
    }
  }
}

std::string required_string(
    const JsonValue& object,
    const char* name,
    const std::string& context,
    bool allow_empty = false) {
  const auto& value = required_member(object, name, context);
  if(!value.IsString() || (!allow_empty && value.GetStringLength() == 0U)) {
    throw EvaluationError(context + "." + name + " must be a string");
  }
  return std::string(value.GetString(), value.GetStringLength());
}

bool required_bool(
    const JsonValue& object,
    const char* name,
    const std::string& context) {
  const auto& value = required_member(object, name, context);
  if(!value.IsBool()) {
    throw EvaluationError(context + "." + name + " must be a boolean");
  }
  return value.GetBool();
}

std::uint64_t required_uint64(
    const JsonValue& object,
    const char* name,
    const std::string& context) {
  const auto& value = required_member(object, name, context);
  if(!value.IsUint64()) {
    throw EvaluationError(context + "." + name + " must be an unsigned integer");
  }
  return value.GetUint64();
}

std::size_t required_size(
    const JsonValue& object,
    const char* name,
    const std::string& context) {
  const std::uint64_t value = required_uint64(object, name, context);
  if(value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw EvaluationError(context + "." + name + " is outside size_t range");
  }
  return static_cast<std::size_t>(value);
}

std::uint32_t required_uint32(
    const JsonValue& object,
    const char* name,
    const std::string& context) {
  const std::uint64_t value = required_uint64(object, name, context);
  if(value > std::numeric_limits<std::uint32_t>::max()) {
    throw EvaluationError(context + "." + name + " is outside uint32 range");
  }
  return static_cast<std::uint32_t>(value);
}

double required_probability(
    const JsonValue& object,
    const char* name,
    const std::string& context) {
  const auto& value = required_member(object, name, context);
  if(!value.IsNumber() || !std::isfinite(value.GetDouble())
     || value.GetDouble() < 0.0 || value.GetDouble() > 1.0) {
    throw EvaluationError(context + "." + name + " must be in [0, 1]");
  }
  return value.GetDouble();
}

std::filesystem::path resolve_prompt_path(
    const std::filesystem::path& config_path,
    const std::string& relative_path,
    const std::string& name) {
  const std::filesystem::path configured(relative_path);
  if(configured.is_absolute()) {
    throw EvaluationError("prompts." + name + " must use a relative path");
  }
  const auto normalized = configured.lexically_normal();
  if(normalized.empty() || *normalized.begin() == "..") {
    throw EvaluationError("prompts." + name + " escapes the config directory");
  }
  return normalized_existing_file(
      config_path.parent_path() / normalized,
      "prompt template " + name);
}

std::string load_prompt(
    const JsonValue& prompts,
    const std::filesystem::path& config_path,
    const char* name,
    const std::vector<std::string>& placeholders,
    std::vector<std::filesystem::path>* sources) {
  const std::string configured = required_string(prompts, name, "prompts");
  const auto path = resolve_prompt_path(config_path, configured, name);
  const std::string value = read_bounded_text(
      path, kMaxPromptTemplateBytes, "prompt template " + std::string(name));
  std::vector<std::pair<std::string, std::string>> validation;
  for(const auto& placeholder : placeholders) {
    validation.emplace_back(placeholder, "x");
  }
  (void)render_prompt_template(
      name, value, validation, kMaxPromptTemplateBytes);
  sources->push_back(path);
  return value;
}

void validate_positive(std::size_t value, const std::string& name) {
  if(value == 0U) {
    throw EvaluationError(name + " must be greater than zero");
  }
}

}  // namespace

std::string render_prompt_template(
    const std::string& template_name,
    const std::string& prompt_template,
    const std::vector<std::pair<std::string, std::string>>& variables,
    std::size_t max_output_bytes) {
  if(prompt_template.empty()) {
    throw EvaluationError("prompt template " + template_name + " is empty");
  }
  if(max_output_bytes == 0U) {
    throw EvaluationError("prompt output limit must be greater than zero");
  }

  std::unordered_map<std::string, std::string> replacements;
  std::unordered_map<std::string, std::size_t> counts;
  for(const auto& variable : variables) {
    if(variable.first.empty()
       || !replacements.emplace(variable.first, variable.second).second) {
      throw EvaluationError(
          "prompt template " + template_name + " has invalid variables");
    }
    counts.emplace(variable.first, 0U);
  }

  std::string output;
  output.reserve(std::min(max_output_bytes, prompt_template.size() + 1024U));
  std::size_t cursor = 0U;
  while(cursor < prompt_template.size()) {
    const std::size_t opening = prompt_template.find("{{", cursor);
    if(opening == std::string::npos) {
      output.append(prompt_template, cursor, std::string::npos);
      break;
    }
    output.append(prompt_template, cursor, opening - cursor);
    const std::size_t closing = prompt_template.find("}}", opening + 2U);
    if(closing == std::string::npos) {
      throw EvaluationError(
          "prompt template " + template_name + " has an unclosed placeholder");
    }
    const std::string key = prompt_template.substr(
        opening + 2U, closing - opening - 2U);
    const auto replacement = replacements.find(key);
    if(replacement == replacements.end()) {
      throw EvaluationError(
          "prompt template " + template_name
          + " contains unknown placeholder: " + key);
    }
    output.append(replacement->second);
    ++counts[key];
    cursor = closing + 2U;
  }
  for(const auto& variable : variables) {
    if(counts[variable.first] != 1U) {
      throw EvaluationError(
          "prompt template " + template_name + " must contain exactly one {{"
          + variable.first + "}} placeholder");
    }
  }
  if(output.size() > max_output_bytes) {
    throw EvaluationError("rendered prompt " + template_name + " is too large");
  }
  return output;
}

std::filesystem::path discover_runtime_configuration() {
  const char* neutral = std::getenv("TRAJECTORY_EVOLUTION_CONFIG");
  if(neutral != nullptr && *neutral != '\0') {
    return normalized_existing_file(neutral, "runtime configuration");
  }
  const char* configured = std::getenv("AGENTFLOW_TRAJECTORY_CONFIG");
  if(configured != nullptr && *configured != '\0') {
    return normalized_existing_file(configured, "runtime configuration");
  }

  std::error_code error;
  std::filesystem::path current = std::filesystem::current_path(error);
  if(error) {
    throw EvaluationError("cannot inspect the current directory");
  }
  current = current.lexically_normal();
  while(!current.empty()) {
    const std::vector<std::filesystem::path> candidates = {
        current / "tools/trajectory_evolution/config/runtime.json",
        current.filename() == "trajectory_evolution"
            ? current / "config/runtime.json" : std::filesystem::path(),
    };
    for(const auto& candidate : candidates) {
      if(candidate.empty()) {
        continue;
      }
      error.clear();
      if(std::filesystem::is_regular_file(candidate, error) && !error) {
        return normalized_existing_file(candidate, "runtime configuration");
      }
    }
    const auto parent = current.parent_path();
    if(parent == current) {
      break;
    }
    current = parent;
  }
  // Installed CLI layouts are relocatable; explicit --config remains portable.
#if defined(__linux__)
  error.clear();
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  if(!error) {
    const auto installed = executable.parent_path().parent_path()
        / "share/trajectory_evolution/config/runtime.json";
    if(std::filesystem::is_regular_file(installed, error) && !error) {
      return normalized_existing_file(installed, "runtime configuration");
    }
  }
#endif
  throw EvaluationError(
      "runtime configuration was not found; use --config or set "
      "TRAJECTORY_EVOLUTION_CONFIG or AGENTFLOW_TRAJECTORY_CONFIG");
}

RuntimeConfiguration load_runtime_configuration(
    const std::filesystem::path& path) {
  RuntimeConfiguration output;
  output.config_path = normalized_existing_file(path, "runtime configuration");
  output.source_files.push_back(output.config_path);
  const std::string input = read_bounded_text(
      output.config_path, kMaxConfigurationBytes, "runtime configuration");

  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag>(input.data(), input.size());
  if(document.HasParseError() || !document.IsObject()) {
    const std::string detail = document.HasParseError()
        ? rapidjson::GetParseError_En(document.GetParseError())
        : "root is not an object";
    throw EvaluationError("invalid runtime configuration JSON: " + detail);
  }
  require_known_members(
      document,
      {"schema_version", "prompts", "evaluation", "evolution", "model"},
      "configuration");
  const std::string configuration_context = "configuration";
  const auto& schema = required_member(
      document, "schema_version", configuration_context);
  if(!schema.IsUint() || schema.GetUint() != 1U) {
    throw EvaluationError("configuration.schema_version must be 1");
  }

  const auto& prompts = required_object(
      document, "prompts", configuration_context);
  require_known_members(
      prompts,
      {"episode_system", "episode_user", "dialogue_system", "dialogue_user",
       "meta_reflection_system", "meta_reflection_user",
       "memory_curator_system", "memory_curator_user", "standard_dialogue_system"},
      "prompts");
  if(prompts.MemberCount() != kMaxPromptTemplateCount
     && prompts.MemberCount() != kMaxPromptTemplateCount + 1U) {
    throw EvaluationError("prompts must define all eight templates");
  }
  auto loaded_prompts = std::make_shared<PromptTemplates>();
  loaded_prompts->episode_system = load_prompt(
      prompts, output.config_path, "episode_system", {}, &output.source_files);
  loaded_prompts->episode_user = load_prompt(
      prompts, output.config_path, "episode_user", {"evidence_json"},
      &output.source_files);
  loaded_prompts->dialogue_system = load_prompt(
      prompts, output.config_path, "dialogue_system", {}, &output.source_files);
  if(prompts.HasMember("standard_dialogue_system")) {
    loaded_prompts->standard_dialogue_system = load_prompt(
        prompts, output.config_path, "standard_dialogue_system", {}, &output.source_files);
  }
  loaded_prompts->dialogue_user = load_prompt(
      prompts, output.config_path, "dialogue_user",
      {"selected_episode_count", "evidence_json"},
      &output.source_files);
  loaded_prompts->meta_reflection_system = load_prompt(
      prompts, output.config_path, "meta_reflection_system", {},
      &output.source_files);
  loaded_prompts->meta_reflection_user = load_prompt(
      prompts, output.config_path, "meta_reflection_user", {"candidate_json"},
      &output.source_files);
  loaded_prompts->memory_curator_system = load_prompt(
      prompts, output.config_path, "memory_curator_system", {},
      &output.source_files);
  loaded_prompts->memory_curator_user = load_prompt(
      prompts, output.config_path, "memory_curator_user", {"memory_json"},
      &output.source_files);

  const auto& evaluation = required_object(
      document, "evaluation", configuration_context);
  require_known_members(
      evaluation,
      {"max_file_bytes", "max_model_calls", "model_selection", "strict_model",
       "allow_invalid_trace", "require_chinese_reason"},
      "evaluation");
  output.evaluator.max_file_bytes = required_size(
      evaluation, "max_file_bytes", "evaluation");
  output.evaluator.max_model_calls = required_size(
      evaluation, "max_model_calls", "evaluation");
  const std::string selection = required_string(
      evaluation, "model_selection", "evaluation");
  if(selection == "auto") {
    output.evaluator.model_selection = ModelSelection::none;
  } else if(selection == "none" || selection == "suspicious"
            || selection == "all") {
    output.evaluator.model_selection = parse_model_selection(selection);
    output.model_selection_explicit = true;
  } else {
    throw EvaluationError(
        "evaluation.model_selection must be auto, none, suspicious, or all");
  }
  output.evaluator.strict_model = required_bool(
      evaluation, "strict_model", "evaluation");
  output.evaluator.allow_invalid_trace = required_bool(
      evaluation, "allow_invalid_trace", "evaluation");
  output.evaluator.require_chinese_reason = required_bool(
      evaluation, "require_chinese_reason", "evaluation");
  output.evaluator.prompts = loaded_prompts;

  const auto& evolution = required_object(
      document, "evolution", configuration_context);
  require_known_members(
      evolution,
      {"max_input_files", "max_candidates", "min_supporting_dialogues",
       "min_supporting_episodes", "max_memory_entries",
       "max_applied_batch_ids", "max_meta_prompt_bytes", "max_memory_bytes",
       "max_memory_match_candidates", "max_memory_match_prompt_bytes",
       "max_memory_update_retries", "full_curation_new_entry_threshold",
       "min_memory_duplicate_confidence", "min_memory_relation_confidence",
       "enable_meta_reflection", "enable_memory_semantic_matching",
       "require_chinese_reason"},
      "evolution");
  output.evolution.max_input_files = required_size(
      evolution, "max_input_files", "evolution");
  output.evolution.max_candidates = required_size(
      evolution, "max_candidates", "evolution");
  output.evolution.min_supporting_dialogues = required_size(
      evolution, "min_supporting_dialogues", "evolution");
  output.evolution.min_supporting_episodes = required_size(
      evolution, "min_supporting_episodes", "evolution");
  output.evolution.max_memory_entries = required_size(
      evolution, "max_memory_entries", "evolution");
  output.evolution.max_applied_batch_ids = required_size(
      evolution, "max_applied_batch_ids", "evolution");
  output.evolution.max_meta_prompt_bytes = required_size(
      evolution, "max_meta_prompt_bytes", "evolution");
  output.evolution.max_memory_bytes = required_size(
      evolution, "max_memory_bytes", "evolution");
  output.evolution.max_memory_match_candidates = required_size(
      evolution, "max_memory_match_candidates", "evolution");
  output.evolution.max_memory_match_prompt_bytes = required_size(
      evolution, "max_memory_match_prompt_bytes", "evolution");
  output.evolution.max_memory_update_retries = required_size(
      evolution, "max_memory_update_retries", "evolution");
  output.evolution.full_curation_new_entry_threshold = required_size(
      evolution, "full_curation_new_entry_threshold", "evolution");
  output.evolution.min_memory_duplicate_confidence = required_probability(
      evolution, "min_memory_duplicate_confidence", "evolution");
  output.evolution.min_memory_relation_confidence = required_probability(
      evolution, "min_memory_relation_confidence", "evolution");
  output.evolution.enable_meta_reflection = required_bool(
      evolution, "enable_meta_reflection", "evolution");
  output.evolution.enable_memory_semantic_matching = required_bool(
      evolution, "enable_memory_semantic_matching", "evolution");
  output.evolution.require_chinese_reason = required_bool(
      evolution, "require_chinese_reason", "evolution");
  output.evolution.prompts = loaded_prompts;

  validate_positive(output.evaluator.max_file_bytes, "evaluation.max_file_bytes");
  validate_positive(output.evolution.max_input_files, "evolution.max_input_files");
  validate_positive(output.evolution.max_candidates, "evolution.max_candidates");
  validate_positive(
      output.evolution.min_supporting_dialogues,
      "evolution.min_supporting_dialogues");
  validate_positive(
      output.evolution.min_supporting_episodes,
      "evolution.min_supporting_episodes");
  validate_positive(
      output.evolution.max_memory_entries, "evolution.max_memory_entries");
  validate_positive(
      output.evolution.max_applied_batch_ids,
      "evolution.max_applied_batch_ids");
  validate_positive(
      output.evolution.max_meta_prompt_bytes,
      "evolution.max_meta_prompt_bytes");
  validate_positive(
      output.evolution.max_memory_bytes, "evolution.max_memory_bytes");
  validate_positive(
      output.evolution.max_memory_match_candidates,
      "evolution.max_memory_match_candidates");
  validate_positive(
      output.evolution.max_memory_match_prompt_bytes,
      "evolution.max_memory_match_prompt_bytes");
  validate_positive(
      output.evolution.max_memory_update_retries,
      "evolution.max_memory_update_retries");
  validate_positive(
      output.evolution.full_curation_new_entry_threshold,
      "evolution.full_curation_new_entry_threshold");

  const auto& model = required_object(
      document, "model", configuration_context);
  require_known_members(
      model,
      {"endpoint", "model_id", "api_key_env", "no_api_key", "thinking_mode",
       "timeout_ms", "connect_timeout_ms", "max_retries",
       "max_response_bytes", "max_request_bytes", "max_output_tokens",
       "sessionlog_max_output_tokens", "disable_proxy"},
      "model");
  output.model.endpoint = required_string(
      model, "endpoint", "model", true);
  output.model.model_id = required_string(
      model, "model_id", "model", true);
  output.api_key_env = required_string(
      model, "api_key_env", "model", true);
  output.no_api_key = required_bool(model, "no_api_key", "model");
  output.model.thinking_type = required_string(
      model, "thinking_mode", "model", true);
  output.model.timeout_ms = required_uint32(model, "timeout_ms", "model");
  output.model.connect_timeout_ms = required_uint32(
      model, "connect_timeout_ms", "model");
  output.model.max_retries = required_uint32(model, "max_retries", "model");
  output.model.max_response_bytes = required_size(
      model, "max_response_bytes", "model");
  output.model.max_request_bytes = required_size(
      model, "max_request_bytes", "model");
  output.model.max_output_tokens = required_uint32(
      model, "max_output_tokens", "model");
  output.sessionlog_max_output_tokens = required_uint32(
      model, "sessionlog_max_output_tokens", "model");
  output.model.disable_proxy = required_bool(model, "disable_proxy", "model");
  if(output.model.timeout_ms == 0U || output.model.connect_timeout_ms == 0U
     || output.model.max_retries > 10U
     || output.model.max_response_bytes == 0U
     || output.model.max_response_bytes > 8U * 1024U * 1024U
     || output.model.max_request_bytes == 0U
     || output.model.max_request_bytes > 8U * 1024U * 1024U
     || output.model.max_output_tokens == 0U
     || output.model.max_output_tokens > 32768U
     || output.sessionlog_max_output_tokens == 0U
     || output.sessionlog_max_output_tokens > 32768U) {
    throw EvaluationError("model resource limits are invalid");
  }
  if(!output.model.thinking_type.empty()
     && output.model.thinking_type != "enabled"
     && output.model.thinking_type != "disabled"
     && output.model.thinking_type != "auto") {
    throw EvaluationError("model.thinking_mode is invalid");
  }
  std::sort(output.source_files.begin(), output.source_files.end());
  output.source_files.erase(
      std::unique(output.source_files.begin(), output.source_files.end()),
      output.source_files.end());
  return output;
}

std::shared_ptr<const PromptTemplates> load_default_prompt_templates() {
  return load_runtime_configuration(discover_runtime_configuration())
      .evaluator.prompts;
}

}  // namespace agentflow::trajectory
