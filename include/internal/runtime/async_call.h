// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// AsyncCall SDK Header for non-blocking external resource access

#ifndef AGENTFLOW_ASYNC_CALL_H
#define AGENTFLOW_ASYNC_CALL_H

#include <atomic>
#include <memory>
#include <string>

// Forward declarations
namespace agentflow {
namespace runtime {
class MMAgentRuntime;
}
}

// Include the interface definition
#include "agentflow/so_node.h"

namespace agentflow {
namespace runtime {

/// Factory function to create an AsyncCall SDK instance.
/// This is called by the runtime when injecting the SDK into a node.
///
/// \param runtime Pointer to the MMAgentRuntime
/// \param execution_id Workflow execution ID
/// \param session_id Session ID
/// \param dialogue_id Dialogue ID
/// \param task_id Task ID
/// \param node_id Node ID
/// \param async_pending_count Shared pointer to the async pending counter
/// \return Shared pointer to the IAsyncCallSDK instance
std::shared_ptr<IAsyncCallSDK> create_async_call_sdk(
    MMAgentRuntime* runtime,
    const std::string& execution_id,
    const std::string& session_id,
    const std::string& dialogue_id,
    const std::string& task_id,
    const std::string& node_id,
    std::shared_ptr<std::atomic<int>> async_pending_count);

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_ASYNC_CALL_H