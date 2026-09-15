#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "agentflow/trajectory/hybrid_evaluator.h"

namespace agentflow::trajectory {

// Reuse the existing persisted-data credential gate at additional output boundaries.
void ensure_safe_evolution_data(const google::protobuf::Message& message);

struct EvolutionOptions {
  std::size_t max_input_files = 100000;
  std::size_t max_candidates = 10000;
  std::size_t min_supporting_dialogues = 2;
  std::size_t min_supporting_episodes = 2;
  std::size_t max_memory_entries = 512;
  std::size_t max_applied_batch_ids = 4096;
  std::size_t max_meta_prompt_bytes = 2U * 1024U * 1024U;
  std::size_t max_memory_bytes = 256U * 1024U * 1024U;
  std::size_t max_memory_match_candidates = 8;
  std::size_t max_memory_match_prompt_bytes = 256U * 1024U;
  std::size_t max_memory_update_retries = 3;
  std::size_t full_curation_new_entry_threshold = 20;
  double min_memory_duplicate_confidence = 0.9;
  double min_memory_relation_confidence = 0.75;
  bool enable_meta_reflection = false;
  bool enable_memory_semantic_matching = true;
  bool require_chinese_reason = true;
  std::shared_ptr<const PromptTemplates> prompts;
};

struct DialogueEvolutionResult {
  std::filesystem::path input_path;
  bool evaluation_available = false;
  EvaluationOutput evaluation;
  std::string error_code;
  std::string error_message;
};

struct EvolutionBatchOutput {
  evaluation_proto::HybridEvaluationBatchReport report;
  std::vector<DialogueEvolutionResult> dialogues;
  std::vector<ModelRequest> model_requests;
};

struct MemoryUpdateOutput {
  evaluation_proto::MemoryUpdateResult result;
  std::vector<ModelRequest> model_requests;
};

class TrajectoryEvolutionPipeline {
 public:
  explicit TrajectoryEvolutionPipeline(
      EvaluatorOptions evaluator_options = EvaluatorOptions(),
      EvolutionOptions evolution_options = EvolutionOptions(),
      std::shared_ptr<ModelAdapter> model_adapter = nullptr);

  EvolutionBatchOutput evaluate_directory(
      const std::filesystem::path& root) const;

  EvolutionBatchOutput evaluate_files(
      const std::vector<std::filesystem::path>& paths) const;

  EvolutionBatchOutput synthesize_reports(
      const std::vector<evaluation_proto::HybridEvaluationReport>& reports) const;

  EvolutionBatchOutput synthesize_reports(
      const std::vector<evaluation_proto::HybridEvaluationReport>& reports,
      const evaluation_proto::EvolutionMemoryStore& memory) const;

  static evaluation_proto::EvolutionMemoryStore load_memory(
      const std::filesystem::path& path,
      std::size_t max_file_bytes = 256U * 1024U * 1024U);

  static evaluation_proto::MemoryUpdateResult merge_memory(
      const evaluation_proto::HybridEvaluationBatchReport& batch,
      evaluation_proto::EvolutionMemoryStore* memory);

  static evaluation_proto::MemoryUpdateResult update_memory_file(
      const evaluation_proto::HybridEvaluationBatchReport& batch,
      const std::filesystem::path& protobuf_path,
      const std::filesystem::path& json_path = {},
      std::size_t max_file_bytes = 256U * 1024U * 1024U);

  MemoryUpdateOutput curate_memory_file(
      const evaluation_proto::HybridEvaluationBatchReport& batch,
      const std::filesystem::path& protobuf_path,
      const std::filesystem::path& json_path = {},
      std::size_t max_file_bytes = 256U * 1024U * 1024U,
      std::size_t prior_model_calls = 0) const;

  static std::string to_json(
      const evaluation_proto::HybridEvaluationBatchReport& report,
      bool pretty = true);

  static std::string to_json(
      const evaluation_proto::EvolutionMemoryStore& memory,
      bool pretty = true);

  static void write_protobuf(
      const evaluation_proto::HybridEvaluationBatchReport& report,
      const std::filesystem::path& path);

  static void write_protobuf(
      const evaluation_proto::EvolutionMemoryStore& memory,
      const std::filesystem::path& path);

  static std::string meta_reflection_system_prompt();

  static std::string memory_curator_system_prompt();

 private:
  EvaluatorOptions evaluator_options_;
  EvolutionOptions evolution_options_;
  std::shared_ptr<ModelAdapter> model_adapter_;
};

}  // namespace agentflow::trajectory
