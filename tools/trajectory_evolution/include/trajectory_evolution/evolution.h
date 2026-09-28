// 公开接口：定义宿主接入所需的类型、配置和调用约定。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <utility>

#include "trajectory_evolution/trace.h"
#include "trajectory_evolution/shadow_runtime.h"
#include "trajectory_evolution/policy_runtime.h"
#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/runtime_config.h"

// 保留现有评测和记忆的线缆格式及 C++ 接口。
// 该便捷入口提供中性命名，不要求 AgentFlow 运行时。
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
using agentflow::trajectory::load_runtime_configuration;
using agentflow::trajectory::make_jsonl_model_adapter;
using agentflow::trajectory::make_openai_compatible_model_adapter;

// 通过框架无关入口分析标准轨迹，复用统一评测实现。
inline EvaluationOutput evaluate_trace(const v1::TraceRun &trace, EvaluatorOptions options = {},
                                       std::shared_ptr<ModelAdapter> model = nullptr) {
  return Evaluator(std::move(options), std::move(model)).evaluate_trace_json(to_json(trace, false));
}
} // 命名空间结束：trajectory::evolution
