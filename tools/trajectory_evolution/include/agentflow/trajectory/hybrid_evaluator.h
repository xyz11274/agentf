#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "hybrid_evaluation.pb.h"

namespace agentflow::trajectory {

namespace evaluation_proto = ::agentflow::trajectory::evaluation;

enum class ModelSelection {
  none,
  suspicious,
  all,
};

struct EvaluatorOptions {
  std::size_t max_file_bytes = 256U * 1024U * 1024U;
  std::size_t max_model_calls = 512;
  ModelSelection model_selection = ModelSelection::none;
  bool strict_model = false;
  bool allow_invalid_trace = false;
  bool require_chinese_reason = true;
  std::vector<std::string> redaction_secrets;
};

struct ModelRequest {
  std::string packet_id;
  std::string system_prompt;
  std::string user_prompt;
  std::vector<std::string> allowed_event_ids;
};

struct RawModelResponse {
  bool transport_ok = false;
  std::string adapter;
  std::string model_id;
  std::string body;
  std::string error_code;
  std::uint32_t attempts = 0;
};

class ModelAdapter {
 public:
  virtual ~ModelAdapter() = default;
  virtual RawModelResponse evaluate(const ModelRequest& request) = 0;
  virtual std::string name() const = 0;
  virtual std::string model_id() const = 0;
};

struct OpenAiCompatibleOptions {
  std::string endpoint;
  std::string model_id;
  std::string api_key;
  std::string thinking_type;
  std::uint32_t timeout_ms = 30000;
  std::uint32_t connect_timeout_ms = 5000;
  std::uint32_t max_retries = 1;
  std::size_t max_response_bytes = 256U * 1024U;
  std::size_t max_request_bytes = 2U * 1024U * 1024U;
  std::uint32_t max_output_tokens = 800;
  bool disable_proxy = true;
};

std::shared_ptr<ModelAdapter> make_jsonl_model_adapter(
    const std::filesystem::path& path,
    std::size_t max_file_bytes = 64U * 1024U * 1024U);

std::shared_ptr<ModelAdapter> make_openai_compatible_model_adapter(
    const OpenAiCompatibleOptions& options);

struct EvaluationOutput {
  evaluation_proto::HybridEvaluationReport report;
  std::vector<ModelRequest> model_requests;
};

class EvaluationError : public std::runtime_error {
 public:
  explicit EvaluationError(const std::string& message)
      : std::runtime_error(message) {}
};

class HybridTrajectoryEvaluator {
 public:
  explicit HybridTrajectoryEvaluator(
      EvaluatorOptions options = EvaluatorOptions(),
      std::shared_ptr<ModelAdapter> model_adapter = nullptr);

  EvaluationOutput evaluate_file(const std::filesystem::path& path) const;

  EvaluationOutput evaluate_json(const std::string& analysis_json) const;

  EvaluationOutput evaluate_sessionlog(
      const std::filesystem::path& path) const;

  static std::string to_json(
      const evaluation_proto::HybridEvaluationReport& report,
      bool pretty = true);

  static void write_protobuf(
      const evaluation_proto::HybridEvaluationReport& report,
      const std::filesystem::path& path);

  static void write_model_requests_jsonl(
      const std::vector<ModelRequest>& requests,
      const std::filesystem::path& path);

  static std::string system_prompt();

  static std::string dialogue_system_prompt();

 private:
  EvaluatorOptions options_;
  std::shared_ptr<ModelAdapter> model_adapter_;
};

std::string to_string(ModelSelection selection);
ModelSelection parse_model_selection(const std::string& value);

}  // namespace agentflow::trajectory
