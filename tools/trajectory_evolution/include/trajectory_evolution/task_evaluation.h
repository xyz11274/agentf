#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "trajectory_evolution/dataset.h"

namespace trajectory::evolution {

struct TaskEvaluationOptions {
  std::filesystem::path model_config;
  std::shared_ptr<agentflow::trajectory::ModelAdapter> model_adapter;
  bool allow_unreviewed = false;
  // Called between trials and while supervising the host process.
  std::function<bool()> cancelled;
  // Appended for aggregate-initializer compatibility with existing C++ callers.
  // Frozen configuration supplied by a background caller; CLI paths stay valid.
  std::string config_json;
};

// Canonical behavior fingerprint, independent of run IDs and memory counters.
std::string agent_version_id(const std::string& artifact_json);

// Offline task execution and grading only: never learns, mutates memory, or
// publishes policies. Host commands are trusted programs, not a security sandbox.
dataset::v1::TaskEvaluationReport run_task_evaluation(
    const std::filesystem::path& dataset_directory,
    const std::filesystem::path& config_path,
    const std::filesystem::path& output_directory,
    const TaskEvaluationOptions& options = {});

}  // namespace trajectory::evolution
