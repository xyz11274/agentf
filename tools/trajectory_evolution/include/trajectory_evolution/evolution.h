#pragma once

#include <utility>

#include "trajectory_evolution/trace.h"
#include "trajectory_evolution/shadow_runtime.h"
#include "trajectory_evolution/policy_runtime.h"
#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/runtime_config.h"

// Preserve the established Evaluation/Memory wire format and existing C++ API.
// This facade provides neutral entry names without requiring AgentFlow runtime.
namespace trajectory::evolution {
using Evaluator = agentflow::trajectory::HybridTrajectoryEvaluator;
using EvaluatorOptions = agentflow::trajectory::EvaluatorOptions;
using Pipeline = agentflow::trajectory::TrajectoryEvolutionPipeline;
using EvolutionOptions = agentflow::trajectory::EvolutionOptions;
using ModelAdapter = agentflow::trajectory::ModelAdapter;
using ModelSelection = agentflow::trajectory::ModelSelection;
using EvaluationOutput = agentflow::trajectory::EvaluationOutput;
using BatchOutput = agentflow::trajectory::EvolutionBatchOutput;
using MemoryStore = agentflow::trajectory::evaluation::EvolutionMemoryStore;
using ModelRequest = agentflow::trajectory::ModelRequest;
using RawModelResponse = agentflow::trajectory::RawModelResponse;
using OpenAiCompatibleOptions = agentflow::trajectory::OpenAiCompatibleOptions;
using agentflow::trajectory::make_openai_compatible_model_adapter;
using agentflow::trajectory::make_jsonl_model_adapter;
using agentflow::trajectory::load_runtime_configuration;

inline EvaluationOutput evaluate_trace(const v1::TraceRun& trace,
                                       EvaluatorOptions options = {},
                                       std::shared_ptr<ModelAdapter> model = nullptr) {
  return Evaluator(std::move(options), std::move(model)).evaluate_trace_json(
      to_json(trace, false));
}
}  // namespace trajectory::evolution
