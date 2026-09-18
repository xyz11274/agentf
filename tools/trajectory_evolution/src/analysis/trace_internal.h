#pragma once

#include "trajectory_evolution/trace.h"
#include "agentflow/trajectory/loop_episode_analyzer.h"

namespace trajectory::evolution {

// Both input formats produce the same existing analysis representation; legacy
// SessionLog continues to use its mature parser and framework-specific rules.
agentflow::trajectory::AnalysisReport analyze_trace(const ValidatedTrace& trace);
std::string trace_content_jsonl(const v1::TraceRun& trace);

}  // namespace trajectory::evolution
