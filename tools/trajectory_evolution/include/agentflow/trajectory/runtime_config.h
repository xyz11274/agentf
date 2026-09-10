#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "agentflow/trajectory/evolution_pipeline.h"

namespace agentflow::trajectory {

struct RuntimeConfiguration {
  std::filesystem::path config_path;
  std::vector<std::filesystem::path> source_files;
  EvaluatorOptions evaluator;
  EvolutionOptions evolution;
  OpenAiCompatibleOptions model;
  std::string api_key_env;
  std::uint32_t sessionlog_max_output_tokens = 4096;
  bool no_api_key = false;
  bool model_selection_explicit = false;
  bool max_output_tokens_explicit = false;
};

std::filesystem::path discover_runtime_configuration();

RuntimeConfiguration load_runtime_configuration(
    const std::filesystem::path& path);

}  // namespace agentflow::trajectory
