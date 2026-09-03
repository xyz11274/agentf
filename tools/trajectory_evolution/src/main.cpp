#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/hybrid_evaluator.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace trajectory = agentflow::trajectory;

struct CommandLine {
  std::filesystem::path input;
  std::filesystem::path json_output;
  std::filesystem::path protobuf_output;
  std::filesystem::path requests_output;
  std::filesystem::path output_root;
  std::filesystem::path batch_json_output;
  std::filesystem::path batch_protobuf_output;
  std::filesystem::path memory_output;
  std::filesystem::path memory_json_output;
  std::filesystem::path model_responses;
  std::string model_endpoint;
  std::string model_id;
  std::string api_key_env;
  bool no_api_key = false;
  bool no_memory = false;
  bool pretty = true;
  bool selection_explicit = false;
  bool max_output_tokens_explicit = false;
  trajectory::EvaluatorOptions evaluator;
  trajectory::EvolutionOptions evolution;
  trajectory::OpenAiCompatibleOptions http;
};

void print_usage(std::ostream& output) {
  output
      << "Usage:\n"
      << "  trajectory_evolution evaluate [options] <dialogue.jsonl|analysis.json>\n"
      << "  trajectory_evolution evaluate [options] <sessionlog-directory>\n"
      << "\n"
      << "Single-dialogue outputs:\n"
      << "  -o, --output PATH             Write Protobuf-JSON Evaluation\n"
      << "  --protobuf-out PATH           Write binary Evaluation Protobuf\n"
      << "\n"
      << "Directory pipeline outputs:\n"
      << "  --output-root PATH            Required root for per-dialogue Evaluations\n"
      << "  --batch-json-out PATH         Override batch report JSON path\n"
      << "  --batch-protobuf-out PATH     Override batch report Protobuf path\n"
      << "  --memory-out PATH             Override evolution memory Protobuf path\n"
      << "  --memory-json-out PATH        Override readable memory JSON path\n"
      << "  --no-memory                   Evaluate and build candidates without merging memory\n"
      << "  --min-supporting-dialogues N  Candidate gate (default: 2)\n"
      << "  --min-supporting-episodes N   Candidate gate (default: 2)\n"
      << "  --meta-reflection             One optional cross-dialogue model synthesis call\n"
      << "\n"
      << "Common outputs:\n"
      << "  --requests-out PATH           Write redacted model requests as JSONL\n"
      << "  --compact                     Emit compact JSON\n"
      << "\n"
      << "Model evaluation (offline by default):\n"
      << "  --model-selection MODE        none, suspicious, or all\n"
      << "  --model-responses PATH        Replay prerecorded JSONL judgments\n"
      << "  --model-endpoint URL          Exact OpenAI-compatible chat-completions URL\n"
      << "  --model-id ID                 Live model ID\n"
      << "  --thinking-mode MODE          enabled, disabled, or auto\n"
      << "  --api-key-env NAME            Read API key from this environment variable\n"
      << "  --no-api-key                  Use a keyless loopback/provider endpoint\n"
      << "  --timeout-ms N                Overall HTTP timeout (default: 30000)\n"
      << "  --connect-timeout-ms N        Connect timeout (default: 5000)\n"
      << "  --model-retries N             Transport retries (default: 1)\n"
      << "  --max-model-calls N           Batch-wide model-call budget (default: 512)\n"
      << "  --max-output-tokens N         Model output budget\n"
      << "  --strict-model                Single-file mode fails on model errors\n"
      << "\n"
      << "Input controls:\n"
      << "  --allow-invalid-trace         Quarantine invalid traces; never model them\n"
      << "  --max-input-files N           Directory input limit (default: 100000)\n"
      << "  -h, --help                    Show this help\n";
}

std::uint32_t parse_uint32(const std::string& value, const std::string& name) {
  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed);
    if(consumed != value.size() || parsed > 0xffffffffUL) {
      throw std::out_of_range("range");
    }
    return static_cast<std::uint32_t>(parsed);
  } catch(const std::exception&) {
    throw trajectory::EvaluationError(name + " requires an unsigned integer");
  }
}

std::size_t parse_size(const std::string& value, const std::string& name) {
  try {
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if(consumed != value.size()
       || parsed > static_cast<unsigned long long>(SIZE_MAX)) {
      throw std::out_of_range("range");
    }
    return static_cast<std::size_t>(parsed);
  } catch(const std::exception&) {
    throw trajectory::EvaluationError(name + " requires an unsigned integer");
  }
}

std::string require_value(int argc, char** argv, int* index, const char* option) {
  if(*index + 1 >= argc) {
    throw trajectory::EvaluationError(std::string(option) + " requires a value");
  }
  ++(*index);
  return argv[*index];
}

CommandLine parse_arguments(int argc, char** argv) {
  CommandLine command;
  for(int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if(index == 1 && argument == "evaluate") {
      continue;
    }
    if(argument == "-h" || argument == "--help") {
      print_usage(std::cout);
      std::exit(0);
    } else if(argument == "-o" || argument == "--output") {
      command.json_output = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--protobuf-out") {
      command.protobuf_output = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--requests-out") {
      command.requests_output = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--output-root") {
      command.output_root = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--batch-json-out") {
      command.batch_json_output = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--batch-protobuf-out") {
      command.batch_protobuf_output = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--memory-out") {
      command.memory_output = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--memory-json-out") {
      command.memory_json_output = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--no-memory") {
      command.no_memory = true;
    } else if(argument == "--meta-reflection") {
      command.evolution.enable_meta_reflection = true;
    } else if(argument == "--min-supporting-dialogues") {
      command.evolution.min_supporting_dialogues = parse_size(
          require_value(argc, argv, &index, argument.c_str()), argument);
    } else if(argument == "--min-supporting-episodes") {
      command.evolution.min_supporting_episodes = parse_size(
          require_value(argc, argv, &index, argument.c_str()), argument);
    } else if(argument == "--max-input-files") {
      command.evolution.max_input_files = parse_size(
          require_value(argc, argv, &index, argument.c_str()), argument);
    } else if(argument == "--model-responses") {
      command.model_responses = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--model-endpoint") {
      command.model_endpoint = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--model-id") {
      command.model_id = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--thinking-mode") {
      command.http.thinking_type = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--api-key-env") {
      command.api_key_env = require_value(argc, argv, &index, argument.c_str());
    } else if(argument == "--no-api-key") {
      command.no_api_key = true;
    } else if(argument == "--model-selection") {
      command.evaluator.model_selection = trajectory::parse_model_selection(
          require_value(argc, argv, &index, argument.c_str()));
      command.selection_explicit = true;
    } else if(argument == "--timeout-ms") {
      command.http.timeout_ms = parse_uint32(
          require_value(argc, argv, &index, argument.c_str()), argument);
    } else if(argument == "--connect-timeout-ms") {
      command.http.connect_timeout_ms = parse_uint32(
          require_value(argc, argv, &index, argument.c_str()), argument);
    } else if(argument == "--model-retries") {
      command.http.max_retries = parse_uint32(
          require_value(argc, argv, &index, argument.c_str()), argument);
    } else if(argument == "--max-model-calls") {
      command.evaluator.max_model_calls = parse_size(
          require_value(argc, argv, &index, argument.c_str()), argument);
    } else if(argument == "--max-output-tokens") {
      command.http.max_output_tokens = parse_uint32(
          require_value(argc, argv, &index, argument.c_str()), argument);
      command.max_output_tokens_explicit = true;
    } else if(argument == "--strict-model") {
      command.evaluator.strict_model = true;
    } else if(argument == "--allow-invalid-trace") {
      command.evaluator.allow_invalid_trace = true;
    } else if(argument == "--compact") {
      command.pretty = false;
    } else if(!argument.empty() && argument[0] == '-') {
      throw trajectory::EvaluationError("unknown option: " + argument);
    } else if(command.input.empty()) {
      command.input = argument;
    } else {
      throw trajectory::EvaluationError("only one input path is supported");
    }
  }

  if(command.input.empty()) {
    throw trajectory::EvaluationError("input path is required");
  }
  std::error_code input_error;
  const bool directory = std::filesystem::is_directory(command.input, input_error);
  if(input_error) {
    throw trajectory::EvaluationError("cannot inspect input path");
  }
  if((directory || command.input.extension() == ".jsonl")
     && !command.max_output_tokens_explicit) {
    command.http.max_output_tokens = 4096;
  }
  if(directory && command.output_root.empty()) {
    throw trajectory::EvaluationError(
        "directory mode requires --output-root");
  }
  if(directory && (!command.json_output.empty()
                    || !command.protobuf_output.empty())) {
    throw trajectory::EvaluationError(
        "directory mode uses --output-root and batch output options");
  }
  if(!directory && (!command.output_root.empty()
                     || !command.batch_json_output.empty()
                     || !command.batch_protobuf_output.empty()
                     || !command.memory_output.empty()
                     || !command.memory_json_output.empty()
                     || command.no_memory
                     || command.evolution.enable_meta_reflection)) {
    throw trajectory::EvaluationError(
        "batch and memory options require a directory input");
  }
  if(command.no_memory
     && (!command.memory_output.empty() || !command.memory_json_output.empty())) {
    throw trajectory::EvaluationError(
        "--no-memory cannot be combined with memory output options");
  }
  if(command.evolution.min_supporting_dialogues == 0U
     || command.evolution.min_supporting_episodes == 0U
     || command.evolution.max_input_files == 0U) {
    throw trajectory::EvaluationError("evolution limits must be greater than zero");
  }

  const bool replay = !command.model_responses.empty();
  const bool live = !command.model_endpoint.empty() || !command.model_id.empty()
      || !command.http.thinking_type.empty() || !command.api_key_env.empty()
      || command.no_api_key;
  if(replay && live) {
    throw trajectory::EvaluationError(
        "--model-responses cannot be combined with live model options");
  }
  if(live) {
    if(command.model_endpoint.empty() || command.model_id.empty()) {
      throw trajectory::EvaluationError(
          "live evaluation requires --model-endpoint and --model-id");
    }
    if(!command.api_key_env.empty() == command.no_api_key) {
      throw trajectory::EvaluationError(
          "choose exactly one of --api-key-env or --no-api-key");
    }
  }
  if((replay || live) && !command.selection_explicit) {
    command.evaluator.model_selection = trajectory::ModelSelection::suspicious;
  }
  return command;
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  if(!error) {
    return canonical.lexically_normal();
  }
  error.clear();
  const auto absolute = std::filesystem::absolute(path, error);
  if(error) {
    throw trajectory::EvaluationError("cannot normalize path: " + error.message());
  }
  return absolute.lexically_normal();
}

bool paths_conflict(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs) {
  if(lhs.empty() || rhs.empty()) {
    return false;
  }
  std::error_code error;
  if(std::filesystem::exists(lhs, error) && !error
     && std::filesystem::exists(rhs, error) && !error) {
    const bool equivalent = std::filesystem::equivalent(lhs, rhs, error);
    if(!error && equivalent) {
      return true;
    }
  }
  return normalized_path(lhs) == normalized_path(rhs);
}

bool path_is_within(
    const std::filesystem::path& child,
    const std::filesystem::path& parent) {
  if(child.empty() || parent.empty()) {
    return false;
  }
  const auto relative = normalized_path(child).lexically_relative(
      normalized_path(parent));
  if(relative.empty()) {
    return false;
  }
  const auto first = relative.begin();
  return first != relative.end() && *first != "..";
}

void ensure_distinct_outputs(const CommandLine& command) {
  const std::vector<std::filesystem::path> inputs = {
      command.input,
      command.model_responses,
  };
  const std::vector<std::filesystem::path> outputs = {
      command.output_root,
      command.json_output,
      command.protobuf_output,
      command.requests_output,
      command.batch_json_output,
      command.batch_protobuf_output,
      command.memory_output,
      command.memory_json_output,
  };
  std::error_code directory_error;
  if(std::filesystem::is_directory(command.input, directory_error)
     && !directory_error) {
    for(const auto& output : outputs) {
      if(path_is_within(output, command.input)) {
        throw trajectory::EvaluationError(
            "directory outputs must be outside the SessionLog input root");
      }
    }
    if(path_is_within(command.model_responses, command.input)) {
      throw trajectory::EvaluationError(
          "model replay input must be outside the SessionLog input root");
    }
  }

  for(const auto& output : outputs) {
    for(const auto& input : inputs) {
      if(paths_conflict(output, input)) {
        throw trajectory::EvaluationError(
            "an output path must not overwrite an input file");
      }
    }
  }
  for(std::size_t left = 0; left < outputs.size(); ++left) {
    for(std::size_t right = left + 1; right < outputs.size(); ++right) {
      if(paths_conflict(outputs[left], outputs[right])) {
        throw trajectory::EvaluationError("output paths must be distinct");
      }
    }
  }
}

void ensure_dialogue_output_distinct(
    const CommandLine& command,
    const std::filesystem::path& path) {
  const std::vector<std::filesystem::path> reserved = {
      command.input,
      command.model_responses,
      command.requests_output,
      command.batch_json_output,
      command.batch_protobuf_output,
      command.memory_output,
      command.memory_json_output,
  };
  for(const auto& other : reserved) {
    if(paths_conflict(path, other)) {
      throw trajectory::EvaluationError(
          "a dialogue output path conflicts with another input or output");
    }
  }
}

void write_text(const std::filesystem::path& path, const std::string& content) {
  std::error_code error;
  if(!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if(error) {
      throw trajectory::EvaluationError(
          "cannot create output directory: " + error.message());
    }
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if(!output) {
    throw trajectory::EvaluationError("cannot open JSON output");
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if(!output.flush()) {
    throw trajectory::EvaluationError("failed to write JSON output");
  }
}

std::shared_ptr<trajectory::ModelAdapter> make_adapter(CommandLine* command) {
  if(!command->model_responses.empty()) {
    return trajectory::make_jsonl_model_adapter(command->model_responses);
  }
  if(command->model_endpoint.empty()) {
    return nullptr;
  }
  command->http.endpoint = command->model_endpoint;
  command->http.model_id = command->model_id;
  if(!command->no_api_key) {
    static const std::regex environment_name(R"([A-Za-z_][A-Za-z0-9_]*)");
    if(!std::regex_match(command->api_key_env, environment_name)) {
      throw trajectory::EvaluationError("invalid API key environment variable name");
    }
    const char* value = std::getenv(command->api_key_env.c_str());
    if(value == nullptr || *value == '\0') {
      throw trajectory::EvaluationError(
          "configured API key environment variable is missing or empty");
    }
    command->http.api_key = value;
    command->evaluator.redaction_secrets.emplace_back(value);
  }
  return trajectory::make_openai_compatible_model_adapter(command->http);
}

std::string safe_component(const std::string& value, const std::string& fallback) {
  if(value.empty() || value == "." || value == ".." || value.size() > 160U) {
    return fallback;
  }
  std::string output;
  constexpr char hex[] = "0123456789abcdef";
  for(const unsigned char ch : value) {
    if(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
      output.push_back(static_cast<char>(ch));
    } else {
      output.push_back('_');
      output.push_back(hex[(ch >> 4U) & 0x0fU]);
      output.push_back(hex[ch & 0x0fU]);
    }
  }
  return output.empty() ? fallback : output;
}

std::filesystem::path dialogue_output_base(
    const std::filesystem::path& root,
    const trajectory::evaluation_proto::HybridEvaluationReport& report) {
  const std::string source_suffix = report.source_fingerprint().empty()
      ? "unknown" : report.source_fingerprint().substr(0, 16U);
  const std::string user = safe_component(
      report.source().user_id(), "unknown-user");
  const std::string conversation = safe_component(
      report.source().conversation_id(), "unknown-conversation");
  const std::string dialogue = safe_component(
      report.source().dialogue_id(), "unknown-dialogue-" + source_suffix);
  return root / "by-user" / user / conversation / dialogue;
}

int run_single(
    const CommandLine& command,
    const std::shared_ptr<trajectory::ModelAdapter>& adapter) {
  trajectory::HybridTrajectoryEvaluator evaluator(command.evaluator, adapter);
  const trajectory::EvaluationOutput result =
      command.input.extension() == ".jsonl"
          ? evaluator.evaluate_sessionlog(command.input)
          : evaluator.evaluate_file(command.input);
  const std::string json = trajectory::HybridTrajectoryEvaluator::to_json(
      result.report, command.pretty);
  if(command.json_output.empty()) {
    std::cout << json;
    if(json.empty() || json.back() != '\n') {
      std::cout << '\n';
    }
  } else {
    write_text(command.json_output, json);
  }
  if(!command.protobuf_output.empty()) {
    trajectory::HybridTrajectoryEvaluator::write_protobuf(
        result.report, command.protobuf_output);
  }
  if(!command.requests_output.empty()) {
    trajectory::HybridTrajectoryEvaluator::write_model_requests_jsonl(
        result.model_requests, command.requests_output);
  }
  return 0;
}

int run_directory(
    CommandLine* command,
    const std::shared_ptr<trajectory::ModelAdapter>& adapter) {
  trajectory::TrajectoryEvolutionPipeline pipeline(
      command->evaluator, command->evolution, adapter);
  trajectory::EvolutionBatchOutput output = pipeline.evaluate_directory(
      command->input);

  const std::string batch_component = safe_component(
      output.report.batch_id(), "unknown-batch");
  const std::filesystem::path run_root =
      command->output_root / "runs" / batch_component;
  if(command->batch_json_output.empty()) {
    command->batch_json_output = run_root / "batch.evaluation.json";
  }
  if(command->batch_protobuf_output.empty()) {
    command->batch_protobuf_output = run_root / "batch.evaluation.pb";
  }
  if(!command->no_memory) {
    if(command->memory_output.empty()) {
      command->memory_output =
          command->output_root / "memory" / "evolution-memory.pb";
    }
    if(command->memory_json_output.empty()) {
      command->memory_json_output =
          command->output_root / "memory" / "evolution-memory.json";
    }
  }
  ensure_distinct_outputs(*command);

  std::set<std::filesystem::path> written_bases;
  for(const auto& dialogue : output.dialogues) {
    if(!dialogue.evaluation_available) {
      continue;
    }
    std::filesystem::path base = dialogue_output_base(
        command->output_root, dialogue.evaluation.report);
    if(!written_bases.insert(base).second) {
      base = base.string() + '-'
          + dialogue.evaluation.report.source_fingerprint().substr(0, 16U);
      if(!written_bases.insert(base).second) {
        throw trajectory::EvaluationError(
            "duplicate dialogue identity and source fingerprint");
      }
    }
    const std::filesystem::path json_path =
        base.string() + ".evaluation.json";
    const std::filesystem::path protobuf_path =
        base.string() + ".evaluation.pb";
    ensure_dialogue_output_distinct(*command, json_path);
    ensure_dialogue_output_distinct(*command, protobuf_path);
    write_text(
        json_path,
        trajectory::HybridTrajectoryEvaluator::to_json(
            dialogue.evaluation.report, command->pretty));
    trajectory::HybridTrajectoryEvaluator::write_protobuf(
        dialogue.evaluation.report,
        protobuf_path);
  }

  if(!command->no_memory) {
    const auto update = trajectory::TrajectoryEvolutionPipeline::update_memory_file(
        output.report,
        command->memory_output,
        command->memory_json_output,
        command->evolution.max_memory_bytes);
    output.report.mutable_memory_update()->CopyFrom(update);
  }

  write_text(
      command->batch_json_output,
      trajectory::TrajectoryEvolutionPipeline::to_json(
          output.report, command->pretty));
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(
      output.report, command->batch_protobuf_output);
  if(!command->requests_output.empty()) {
    trajectory::HybridTrajectoryEvaluator::write_model_requests_jsonl(
        output.model_requests, command->requests_output);
  }

  std::cout
      << "batch_id=" << output.report.batch_id() << '\n'
      << "evaluated=" << output.report.evaluation_succeeded_count() << '\n'
      << "failed=" << output.report.evaluation_failed_count() << '\n'
      << "candidates=" << output.report.candidates_size() << '\n'
      << "batch_json=" << command->batch_json_output << '\n';
  if(!command->no_memory) {
    std::cout << "memory=" << command->memory_output << '\n';
  }
  return output.report.evaluation_succeeded_count() == 0U ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  CommandLine command;
  std::shared_ptr<trajectory::ModelAdapter> adapter;
  try {
    command = parse_arguments(argc, argv);
    ensure_distinct_outputs(command);
    adapter = make_adapter(&command);
  } catch(const trajectory::EvaluationError& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 2;
  } catch(const std::exception& error) {
    std::cerr << "ERROR: unexpected argument failure: " << error.what() << '\n';
    return 4;
  }

  try {
    std::error_code error;
    const bool directory = std::filesystem::is_directory(command.input, error);
    if(error) {
      throw trajectory::EvaluationError("cannot inspect input path");
    }
    return directory
        ? run_directory(&command, adapter)
        : run_single(command, adapter);
  } catch(const trajectory::EvaluationError& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 3;
  } catch(const std::exception& error) {
    std::cerr << "ERROR: unexpected failure: " << error.what() << '\n';
    return 4;
  }
}
