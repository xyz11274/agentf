// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Ask pattern manager - lightweight wrapper for Protoactor's built-in ask

#include "actor/ask_pattern.h"
#include "utils/logger.h"

#ifdef AGENTFLOW_WITH_PROTOACTOR

#include <mutex>

namespace agentflow {
namespace actor {

// Global ask pattern manager instance
AskPatternManager& ask_pattern_manager() {
  static AskPatternManager instance;
  return instance;
}

}  // namespace actor
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_PROTOACTOR
