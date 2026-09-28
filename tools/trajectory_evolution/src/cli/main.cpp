// 命令行入口：识别子命令并调用包内稳定接口，不在此实现业务 Agent。
// 参数错误与执行失败使用明确退出码；敏感配置不得拼入诊断输出。

// 命令行入口只做命令分发，评测、进化与运行时逻辑留在各自模块。
#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/hybrid_evaluator.h"
#include "agentflow/trajectory/runtime_config.h"
#include "trace_cli.h"
#include "shadow_cli.h"
#include "policy_cli.h"
#include "dataset_cli.h"
#include "learn_cli.h"
#include "evaluation_process.h"
#include "trajectory_evolution/integration.h"
#include "trajectory_evolution/task_evaluation.h"

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
#include <utility>
#include <vector>

namespace {

namespace trajectory = agentflow::trajectory;

struct CommandLine {
  std::filesystem::path input;
  std::vector<std::filesystem::path> config_sources;
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
  std::uint32_t sessionlog_max_output_tokens = 0;
  bool no_memory = false;
  bool pretty = true;
  bool selection_explicit = false;
  bool max_output_tokens_explicit = false;
  trajectory::EvaluatorOptions evaluator;
  trajectory::EvolutionOptions evolution;
  trajectory::OpenAiCompatibleOptions http;
};

// 打印命令行参数与用法，帮助用户在执行前确定输入和输出位置。
void print_usage(std::ostream &output) {
  output << "Usage:\n"
         << "  trajectory_evolution serve --config <service.json> [--no-auto-evaluate]\n"
         << "  trajectory_evolution learn [LOG_OR_DIRECTORY] --config <service.json> "
            "[--enqueue-only]\n"
         << "  trajectory_evolution evaluate [options] <dialogue.jsonl|analysis.json>\n"
         << "  trajectory_evolution evaluate [options] <sessionlog-directory>\n"
         << "  trajectory_evolution evaluate [options] <run.trace.json|run.trace.pb>\n"
         << "  trajectory_evolution doctor <trace|sessionlog> [--level L0..L4]\n"
         << "      [--manifest PATH] [--fallback strict|downgrade]\n"
         << "  trajectory_evolution shadow compile|query < request.json\n"
         << "  trajectory_evolution dataset build|check|run [options]\n"
         << "\n"
         << "Runtime configuration:\n"
         << "  --config PATH                 JSON config; overrides discovery/env\n"
         << "  AGENTFLOW_TRAJECTORY_CONFIG  Optional config path environment variable\n"
         << "\n"
         << "Single-dialogue outputs:\n"
         << "  -o, --output PATH             Write Protobuf-JSON Evaluation\n"
         << "  --protobuf-out PATH           Write binary Evaluation Protobuf\n"
         << "\n"
         << "Directory pipeline outputs:\n"
         << "  --output-root PATH            Required root for per-dialogue Evaluations\n"
         << "  --batch-json-out PATH         Override batch report JSON path\n"
         << "  --batch-protobuf-out PATH     Override batch report Protobuf path\n"
         << "  --no-memory                   Required for standalone directory analysis; use learn "
            "for memory\n"
         << "  --min-supporting-dialogues N  Override configured Candidate gate\n"
         << "  --min-supporting-episodes N   Override configured Candidate gate\n"
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
         << "  --timeout-ms N                Override configured overall HTTP timeout\n"
         << "  --connect-timeout-ms N        Override configured connect timeout\n"
         << "  --model-retries N             Override configured transport retries\n"
         << "  --max-model-calls N           Override configured model-call budget\n"
         << "  --max-output-tokens N         Model output budget\n"
         << "  --strict-model                Single-file mode fails on model errors\n"
         << "\n"
         << "Input controls:\n"
         << "  --allow-invalid-trace         Quarantine invalid traces; never model them\n"
         << "  --max-input-files N           Override configured directory input limit\n"
         << "  -h, --help                    Show this help\n";
}

// 解析受范围限制的非负整数，拒绝溢出或附带无关字符的输入。
std::uint32_t parse_uint32(const std::string &value, const std::string &name) {
  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed > 0xffffffffUL) {
      throw std::out_of_range("range");
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (const std::exception &) {
    throw trajectory::EvaluationError(name + " requires an unsigned integer");
  }
}

// 解析大小限制参数，保持字节单位与范围检查一致。
std::size_t parse_size(const std::string &value, const std::string &name) {
  try {
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed > static_cast<unsigned long long>(SIZE_MAX)) {
      throw std::out_of_range("range");
    }
    return static_cast<std::size_t>(parsed);
  } catch (const std::exception &) {
    throw trajectory::EvaluationError(name + " requires an unsigned integer");
  }
}

// 读取必须跟随选项的参数值，缺失时立即报错。
std::string require_value(int argc, char **argv, int *index, const char *option) {
  if (*index + 1 >= argc) {
    throw trajectory::EvaluationError(std::string(option) + " requires a value");
  }
  ++(*index);
  return argv[*index];
}

// 解析命令参数并检查组合约束，不在解析阶段发起模型调用。
CommandLine parse_arguments(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      print_usage(std::cout);
      std::exit(0);
    }
  }

  std::filesystem::path requested_config;
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) != "--config") {
      continue;
    }
    if (!requested_config.empty()) {
      throw trajectory::EvaluationError("--config may only be specified once");
    }
    requested_config = require_value(argc, argv, &index, "--config");
  }
  const trajectory::RuntimeConfiguration runtime = trajectory::load_runtime_configuration(
      requested_config.empty() ? trajectory::discover_runtime_configuration() : requested_config);

  CommandLine command;
  command.config_sources = runtime.source_files;
  command.evaluator = runtime.evaluator;
  command.evolution = runtime.evolution;
  command.http = runtime.model;
  command.model_endpoint = runtime.model.endpoint;
  command.model_id = runtime.model.model_id;
  command.api_key_env = runtime.api_key_env;
  command.no_api_key = runtime.no_api_key;
  command.selection_explicit = runtime.model_selection_explicit;
  command.max_output_tokens_explicit = runtime.max_output_tokens_explicit;
  command.sessionlog_max_output_tokens = runtime.sessionlog_max_output_tokens;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (index == 1 && argument == "evaluate") {
      continue;
    }
    if (argument == "-h" || argument == "--help") {
      print_usage(std::cout);
      std::exit(0);
    } else if (argument == "--config") {
      (void)require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "-o" || argument == "--output") {
      command.json_output = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--protobuf-out") {
      command.protobuf_output = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--requests-out") {
      command.requests_output = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--output-root") {
      command.output_root = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--batch-json-out") {
      command.batch_json_output = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--batch-protobuf-out") {
      command.batch_protobuf_output = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--memory-out") {
      command.memory_output = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--memory-json-out") {
      command.memory_json_output = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--no-memory") {
      command.no_memory = true;
    } else if (argument == "--meta-reflection") {
      command.evolution.enable_meta_reflection = true;
    } else if (argument == "--memory-full-curation-threshold") {
      command.evolution.full_curation_new_entry_threshold =
          parse_size(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--memory-max-entries") {
      command.evolution.max_memory_entries =
          parse_size(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--memory-replay-window") {
      command.evolution.max_applied_batch_ids =
          parse_size(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--min-supporting-dialogues") {
      command.evolution.min_supporting_dialogues =
          parse_size(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--min-supporting-episodes") {
      command.evolution.min_supporting_episodes =
          parse_size(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--max-input-files") {
      command.evolution.max_input_files =
          parse_size(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--model-responses") {
      command.model_responses = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--model-endpoint") {
      command.model_endpoint = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--model-id") {
      command.model_id = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--thinking-mode") {
      command.http.thinking_type = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--api-key-env") {
      command.api_key_env = require_value(argc, argv, &index, argument.c_str());
    } else if (argument == "--no-api-key") {
      command.no_api_key = true;
    } else if (argument == "--model-selection") {
      command.evaluator.model_selection =
          trajectory::parse_model_selection(require_value(argc, argv, &index, argument.c_str()));
      command.selection_explicit = true;
    } else if (argument == "--timeout-ms") {
      command.http.timeout_ms =
          parse_uint32(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--connect-timeout-ms") {
      command.http.connect_timeout_ms =
          parse_uint32(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--model-retries") {
      command.http.max_retries =
          parse_uint32(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--max-model-calls") {
      command.evaluator.max_model_calls =
          parse_size(require_value(argc, argv, &index, argument.c_str()), argument);
    } else if (argument == "--max-output-tokens") {
      command.http.max_output_tokens =
          parse_uint32(require_value(argc, argv, &index, argument.c_str()), argument);
      command.max_output_tokens_explicit = true;
    } else if (argument == "--strict-model") {
      command.evaluator.strict_model = true;
    } else if (argument == "--allow-invalid-trace") {
      command.evaluator.allow_invalid_trace = true;
    } else if (argument == "--compact") {
      command.pretty = false;
    } else if (!argument.empty() && argument[0] == '-') {
      throw trajectory::EvaluationError("unknown option: " + argument);
    } else if (command.input.empty()) {
      command.input = argument;
    } else {
      throw trajectory::EvaluationError("only one input path is supported");
    }
  }

  if (command.input.empty()) {
    throw trajectory::EvaluationError("input path is required");
  }
  std::error_code input_error;
  const bool directory = std::filesystem::is_directory(command.input, input_error);
  if (input_error) {
    throw trajectory::EvaluationError("cannot inspect input path");
  }
  if ((directory || command.input.extension() == ".jsonl" ||
       trajectory::is_standard_trace_input(command.input)) &&
      !command.max_output_tokens_explicit) {
    command.http.max_output_tokens = command.sessionlog_max_output_tokens;
  }
  if (directory && command.output_root.empty()) {
    throw trajectory::EvaluationError("directory mode requires --output-root");
  }
  if (directory && (!command.json_output.empty() || !command.protobuf_output.empty())) {
    throw trajectory::EvaluationError("directory mode uses --output-root and batch output options");
  }
  if (!directory && (!command.output_root.empty() || !command.batch_json_output.empty() ||
                     !command.batch_protobuf_output.empty() || !command.memory_output.empty() ||
                     !command.memory_json_output.empty() || command.no_memory ||
                     command.evolution.enable_meta_reflection)) {
    throw trajectory::EvaluationError("batch and memory options require a directory input");
  }
  if (command.no_memory &&
      (!command.memory_output.empty() || !command.memory_json_output.empty())) {
    throw trajectory::EvaluationError(
        "--no-memory cannot be combined with memory curation options");
  }
  if (command.evolution.min_supporting_dialogues == 0U ||
      command.evolution.min_supporting_episodes == 0U || command.evolution.max_input_files == 0U ||
      command.evolution.max_memory_entries == 0U || command.evolution.max_applied_batch_ids == 0U ||
      command.evolution.full_curation_new_entry_threshold == 0U) {
    throw trajectory::EvaluationError("evolution limits must be greater than zero");
  }

  const bool replay = !command.model_responses.empty();
  const bool live = !command.model_endpoint.empty() || !command.model_id.empty() ||
                    !command.http.thinking_type.empty() || !command.api_key_env.empty() ||
                    command.no_api_key;
  if (replay && live) {
    throw trajectory::EvaluationError(
        "--model-responses cannot be combined with live model options");
  }
  if (live) {
    if (command.model_endpoint.empty() || command.model_id.empty()) {
      throw trajectory::EvaluationError("live evaluation requires --model-endpoint and --model-id");
    }
    if (!command.api_key_env.empty() == command.no_api_key) {
      throw trajectory::EvaluationError("choose exactly one of --api-key-env or --no-api-key");
    }
  }
  if ((replay || live) && !command.selection_explicit) {
    command.evaluator.model_selection = trajectory::ModelSelection::suspicious;
  }
  return command;
}

// 规范化路径表示，为输入输出冲突检查提供一致比较形式。
std::filesystem::path normalized_path(const std::filesystem::path &path) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  if (!error) {
    return canonical.lexically_normal();
  }
  error.clear();
  const auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    throw trajectory::EvaluationError("cannot normalize path: " + error.message());
  }
  return absolute.lexically_normal();
}

// 检查两条路径是否冲突，防止输出覆盖原始输入。
bool paths_conflict(const std::filesystem::path &lhs, const std::filesystem::path &rhs) {
  if (lhs.empty() || rhs.empty()) {
    return false;
  }
  std::error_code error;
  if (std::filesystem::exists(lhs, error) && !error && std::filesystem::exists(rhs, error) &&
      !error) {
    const bool equivalent = std::filesystem::equivalent(lhs, rhs, error);
    if (!error && equivalent) {
      return true;
    }
  }
  return normalized_path(lhs) == normalized_path(rhs);
}

// 判断目标是否落在指定目录内，使用路径语义而非简单字符串前缀。
bool path_is_within(const std::filesystem::path &child, const std::filesystem::path &parent) {
  if (child.empty() || parent.empty()) {
    return false;
  }
  const auto relative = normalized_path(child).lexically_relative(normalized_path(parent));
  if (relative.empty()) {
    return false;
  }
  const auto first = relative.begin();
  return first != relative.end() && *first != "..";
}

// 验证多个输出位置互不覆盖，避免后一阶段破坏前一阶段结果。
void ensure_distinct_outputs(const CommandLine &command) {
  std::vector<std::filesystem::path> inputs = {
      command.input,
      command.model_responses,
  };
  inputs.insert(inputs.end(), command.config_sources.begin(), command.config_sources.end());
  const std::vector<std::filesystem::path> outputs = {
      command.output_root,     command.json_output,        command.protobuf_output,
      command.requests_output, command.batch_json_output,  command.batch_protobuf_output,
      command.memory_output,   command.memory_json_output,
  };
  std::error_code directory_error;
  if (std::filesystem::is_directory(command.input, directory_error) && !directory_error) {
    for (const auto &output : outputs) {
      if (path_is_within(output, command.input)) {
        throw trajectory::EvaluationError(
            "directory outputs must be outside the SessionLog input root");
      }
    }
    if (path_is_within(command.model_responses, command.input)) {
      throw trajectory::EvaluationError(
          "model replay input must be outside the SessionLog input root");
    }
  }

  for (const auto &output : outputs) {
    for (const auto &input : inputs) {
      if (paths_conflict(output, input)) {
        throw trajectory::EvaluationError("an output path must not overwrite an input file");
      }
    }
  }
  for (std::size_t left = 0; left < outputs.size(); ++left) {
    for (std::size_t right = left + 1; right < outputs.size(); ++right) {
      if (paths_conflict(outputs[left], outputs[right])) {
        throw trajectory::EvaluationError("output paths must be distinct");
      }
    }
  }
}

// 确认对话分析输出不覆盖原始轨迹或其他对话产物。
void ensure_dialogue_output_distinct(const CommandLine &command,
                                     const std::filesystem::path &path) {
  std::vector<std::filesystem::path> reserved = {
      command.input,
      command.model_responses,
      command.requests_output,
      command.batch_json_output,
      command.batch_protobuf_output,
      command.memory_output,
      command.memory_json_output,
  };
  reserved.insert(reserved.end(), command.config_sources.begin(), command.config_sources.end());
  for (const auto &other : reserved) {
    if (paths_conflict(path, other)) {
      throw trajectory::EvaluationError(
          "a dialogue output path conflicts with another input or output");
    }
  }
}

// 写出已经生成的文本结果；调用前完成路径与隐私检查。
void write_text(const std::filesystem::path &path, const std::string &content) {
  std::error_code error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      throw trajectory::EvaluationError("cannot create output directory: " + error.message());
    }
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw trajectory::EvaluationError("cannot open JSON output");
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!output.flush()) {
    throw trajectory::EvaluationError("failed to write JSON output");
  }
}

// 供发布器在复制最终策略前复用内核的规范化版本算法。输入和解析错误只返回
// 固定诊断，避免把可能含敏感内容的 artifact 回显到日志。
int run_artifact_version(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "ERROR: artifact_version_usage\n";
    return 2;
  }
  try {
    std::string input;
    std::array<char, 4096> buffer{};
    while (std::cin) {
      std::cin.read(buffer.data(), buffer.size());
      input.append(buffer.data(), static_cast<std::size_t>(std::cin.gcount()));
      if (input.size() > 2U * 1024U * 1024U)
        throw std::runtime_error("artifact_version_too_large");
    }
    std::cout << ::trajectory::evolution::agent_version_id(input) << '\n';
    return 0;
  } catch (...) {
    std::cerr << "ERROR: artifact_version_invalid\n";
    return 2;
  }
}

// 根据运行配置选择模型适配器，在线与离线方式共享上层评测流程。
std::shared_ptr<trajectory::ModelAdapter> make_adapter(CommandLine *command) {
  if (!command->model_responses.empty()) {
    return trajectory::make_jsonl_model_adapter(command->model_responses);
  }
  if (command->model_endpoint.empty()) {
    return nullptr;
  }
  command->http.endpoint = command->model_endpoint;
  command->http.model_id = command->model_id;
  if (!command->no_api_key) {
    static const std::regex environment_name(R"([A-Za-z_][A-Za-z0-9_]*)");
    if (!std::regex_match(command->api_key_env, environment_name)) {
      throw trajectory::EvaluationError("invalid API key environment variable name");
    }
    const char *value = std::getenv(command->api_key_env.c_str());
    if (value == nullptr || *value == '\0') {
      // 与后台 learn 保持一致：规则入库不依赖保留下来的模型凭据配置。
      // 需要模型的分析或反思仍报告缺少密钥，不能静默伪装成模型成功。
      if (command->evaluator.model_selection == trajectory::ModelSelection::none &&
          !command->evolution.enable_meta_reflection)
        return nullptr;
      throw trajectory::EvaluationError(
          "configured API key environment variable is missing or empty");
    }
    command->http.api_key = value;
    command->evaluator.redaction_secrets.emplace_back(value);
  }
  return trajectory::make_openai_compatible_model_adapter(command->http);
}

// 将身份限制为安全的单段目录名，避免路径穿越和不同用户目录混写。
std::string safe_component(const std::string &value, const std::string &fallback) {
  if (value.empty() || value == "." || value == ".." || value.size() > 160U) {
    return fallback;
  }
  std::string output;
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char ch : value) {
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
      output.push_back(static_cast<char>(ch));
    } else {
      output.push_back('_');
      output.push_back(hex[(ch >> 4U) & 0x0fU]);
      output.push_back(hex[ch & 0x0fU]);
    }
  }
  return output.empty() ? fallback : output;
}

// 按对话身份计算输出位置，保持同一会话下多轮可区分。
std::filesystem::path
dialogue_output_base(const std::filesystem::path &root,
                     const trajectory::evaluation_proto::HybridEvaluationReport &report) {
  const std::string source_suffix =
      report.source_fingerprint().empty() ? "unknown" : report.source_fingerprint().substr(0, 16U);
  const std::string user = safe_component(report.source().user_id(), "unknown-user");
  const std::string conversation =
      safe_component(report.source().conversation_id(), "unknown-conversation");
  const std::string dialogue =
      safe_component(report.source().dialogue_id(), "unknown-dialogue-" + source_suffix);
  return root / "by-user" / user / conversation / dialogue;
}

// 处理单个输入，依次完成分析与结果落盘，错误通过统一出口报告。
int run_single(const CommandLine &command,
               const std::shared_ptr<trajectory::ModelAdapter> &adapter) {
  trajectory::HybridTrajectoryEvaluator evaluator(command.evaluator, adapter);
  const trajectory::EvaluationOutput result = command.input.extension() == ".jsonl"
                                                  ? evaluator.evaluate_sessionlog(command.input)
                                                  : evaluator.evaluate_file(command.input);
  const std::string json =
      trajectory::HybridTrajectoryEvaluator::to_json(result.report, command.pretty);
  if (command.json_output.empty()) {
    std::cout << json;
    if (json.empty() || json.back() != '\n') {
      std::cout << '\n';
    }
  } else {
    write_text(command.json_output, json);
  }
  if (!command.protobuf_output.empty()) {
    trajectory::HybridTrajectoryEvaluator::write_protobuf(result.report, command.protobuf_output);
  }
  if (!command.requests_output.empty()) {
    trajectory::HybridTrajectoryEvaluator::write_model_requests_jsonl(result.model_requests,
                                                                      command.requests_output);
  }
  return 0;
}

// 遍历选定目录中的有效输入，保持每份轨迹的独立身份与失败诊断。
int run_directory(CommandLine *command, const std::shared_ptr<trajectory::ModelAdapter> &adapter) {
  if (!command->no_memory)
    throw trajectory::EvaluationError(
        "directory learning now uses: learn LOG_DIR --config boundary-service.json; "
        "use evaluate ... --no-memory only for standalone analysis (no memory writes)");
  if (!command->memory_output.empty() || !command->memory_json_output.empty())
    throw trajectory::EvaluationError("memory output is managed by learn --config SERVICE_JSON");
  trajectory::TrajectoryEvolutionPipeline pipeline(command->evaluator, command->evolution, adapter);
  trajectory::EvolutionBatchOutput output = pipeline.evaluate_directory(command->input);

  const std::string batch_component = safe_component(output.report.batch_id(), "unknown-batch");
  const std::filesystem::path run_root = command->output_root / "runs" / batch_component;
  if (command->batch_json_output.empty()) {
    command->batch_json_output = run_root / "batch.evaluation.json";
  }
  if (command->batch_protobuf_output.empty()) {
    command->batch_protobuf_output = run_root / "batch.evaluation.pb";
  }
  ensure_distinct_outputs(*command);

  std::set<std::filesystem::path> written_bases;
  for (const auto &dialogue : output.dialogues) {
    if (!dialogue.evaluation_available) {
      continue;
    }
    std::filesystem::path base =
        dialogue_output_base(command->output_root, dialogue.evaluation.report);
    if (!written_bases.insert(base).second) {
      base = base.string() + '-' + dialogue.evaluation.report.source_fingerprint().substr(0, 16U);
      if (!written_bases.insert(base).second) {
        throw trajectory::EvaluationError("duplicate dialogue identity and source fingerprint");
      }
    }
    const std::filesystem::path json_path = base.string() + ".evaluation.json";
    const std::filesystem::path protobuf_path = base.string() + ".evaluation.pb";
    ensure_dialogue_output_distinct(*command, json_path);
    ensure_dialogue_output_distinct(*command, protobuf_path);
    write_text(json_path, trajectory::HybridTrajectoryEvaluator::to_json(dialogue.evaluation.report,
                                                                         command->pretty));
    trajectory::HybridTrajectoryEvaluator::write_protobuf(dialogue.evaluation.report,
                                                          protobuf_path);
  }

  write_text(command->batch_json_output,
             trajectory::TrajectoryEvolutionPipeline::to_json(output.report, command->pretty));
  trajectory::TrajectoryEvolutionPipeline::write_protobuf(output.report,
                                                          command->batch_protobuf_output);
  if (!command->requests_output.empty()) {
    trajectory::HybridTrajectoryEvaluator::write_model_requests_jsonl(output.model_requests,
                                                                      command->requests_output);
  }

  std::cout << "batch_id=" << output.report.batch_id() << '\n'
            << "evaluated=" << output.report.evaluation_succeeded_count() << '\n'
            << "failed=" << output.report.evaluation_failed_count() << '\n'
            << "candidates=" << output.report.candidates_size() << '\n'
            << "batch_json=" << command->batch_json_output << '\n';
  return output.report.evaluation_succeeded_count() == 0U ? 3 : 0;
}

} // 结束当前匿名命名空间。

// 程序入口：读取参数后路由到对应实现，统一处理退出状态和资源收尾。
int main(int argc, char **argv) {
  namespace trajectory = agentflow::trajectory;
  if (argc > 1 && std::string(argv[1]) == "--internal-evaluation-supervisor") {
    return ::trajectory::evolution::run_evaluation_supervisor(argc, argv);
  }
  if (argc > 1 && std::string(argv[1]) == "learn") {
    return ::trajectory::evolution::run_learning_import(argc, argv);
  }
  if (argc > 1 && std::string(argv[1]) == "dataset") {
    return ::trajectory::evolution::run_dataset(argc, argv);
  }
  if (argc > 1 && std::string(argv[1]) == "serve") {
    return ::trajectory::evolution::run_integration_server(argc, argv);
  }
  if (argc > 1 && std::string(argv[1]) == "runtime") {
    return trajectory::run_policy_runtime(argc, argv);
  }
  if (argc > 1 && std::string(argv[1]) == "shadow") {
    return trajectory::run_shadow(argc, argv);
  }
  if (argc > 1 && std::string(argv[1]) == "doctor") {
    return trajectory::run_doctor(argc, argv);
  }
  if (argc > 1 && std::string(argv[1]) == "artifact-version") {
    return run_artifact_version(argc, argv);
  }
  CommandLine command;
  std::shared_ptr<trajectory::ModelAdapter> adapter;
  try {
    command = parse_arguments(argc, argv);
    ensure_distinct_outputs(command);
    adapter = make_adapter(&command);
  } catch (const trajectory::EvaluationError &error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "ERROR: unexpected argument failure: " << error.what() << '\n';
    return 4;
  }

  try {
    std::error_code error;
    const bool directory = std::filesystem::is_directory(command.input, error);
    if (error) {
      throw trajectory::EvaluationError("cannot inspect input path");
    }
    return directory ? run_directory(&command, adapter) : run_single(command, adapter);
  } catch (const trajectory::EvaluationError &error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 3;
  } catch (const std::exception &error) {
    std::cerr << "ERROR: unexpected failure: " << error.what() << '\n';
    return 4;
  }
}
