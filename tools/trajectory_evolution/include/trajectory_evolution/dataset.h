#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "agentflow/trajectory/hybrid_evaluator.h"
#include "evaluation_dataset.pb.h"

namespace trajectory::evolution {

struct DatasetBuildOptions {
  // Reuse the evaluator's provider configuration; empty uses the dataset config.
  std::filesystem::path model_config;
  std::shared_ptr<agentflow::trajectory::ModelAdapter> model_adapter;
};

// Offline only: never changes online memories, policies, or Agent execution.
dataset::v1::EvaluationDataset build_evaluation_dataset(
    const std::filesystem::path& config_path,
    const DatasetBuildOptions& options = {});

// New directories only. manifest.json is the completion marker; incomplete
// writes remain inspectable and cannot be mistaken for a valid frozen dataset.
void write_evaluation_dataset(const dataset::v1::EvaluationDataset& dataset,
                              const std::filesystem::path& directory);
dataset::v1::EvaluationDataset read_evaluation_dataset(
    const std::filesystem::path& directory);
std::string evaluation_dataset_summary(
    const dataset::v1::EvaluationDataset& dataset);

}  // namespace trajectory::evolution
