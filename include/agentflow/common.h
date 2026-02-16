#ifndef AGENTFLOW_COMMON_H
#define AGENTFLOW_COMMON_H

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <functional>
#include <stdint.h>

namespace agentflow {

// Version info
constexpr const char* AGENTFLOW_VERSION = "0.1.0";

// Type definitions
using AgentId = std::string;
using WorkflowId = std::string;
using NodeId = std::string;
using EdgeId = std::string;

// Status codes
enum class Status {
    SUCCESS = 0,
    PENDING = 1,
    RUNNING = 2,
    FAILED = 3,
    TIMEOUT = 4,
    CANCELLED = 5,
};

enum class NodeState {
    WAITING = 0,
    READY = 1,
    EXECUTING = 2,
    SUCCESS = 3,
    FAILED = 4,
    RETRYING = 5,
};

enum class AgentState {
    CREATED = 0,
    INITIALIZED = 1,
    RUNNING = 2,
    PAUSED = 3,
    STOPPED = 4,
};

enum class WorkflowState {
    CREATED = 0,
    LOADED = 1,
    RUNNING = 2,
    PAUSED = 3,
    COMPLETED = 4,
    FAILED = 5,
};

// Shared data types
using DataVariant = std::string;  // Simplified for now, can be extended
using DataMap = std::map<std::string, DataVariant>;

} // namespace agentflow

#endif // AGENTFLOW_COMMON_H
