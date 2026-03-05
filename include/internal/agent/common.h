#ifndef AGENTFLOW_COMMON_H
#define AGENTFLOW_COMMON_H

#include <functional>
#include <map>
#include <memory>
#include <stdint.h>
#include <string>
#include <variant>
#include <vector>

namespace agentflow {

// Version info
constexpr const char* AGENTFLOW_VERSION = "0.1.0";

// Type definitions
using AgentId    = std::string;
using WorkflowId = std::string;
using NodeId     = std::string;
using EdgeId     = std::string;

// Status codes
enum class Status {
  SUCCESS   = 0,
  PENDING   = 1,
  RUNNING   = 2,
  FAILED    = 3,
  TIMEOUT   = 4,
  CANCELLED = 5,
};

enum class NodeState {
  WAITING   = 0,
  READY     = 1,
  EXECUTING = 2,
  SUCCESS   = 3,
  FAILED    = 4,
  RETRYING  = 5,
};

enum class AgentState {
  CREATED     = 0,
  INITIALIZED = 1,
  RUNNING     = 2,
  PAUSED      = 3,
  STOPPED     = 4,
};

enum class WorkflowState {
  CREATED   = 0,
  LOADED    = 1,
  RUNNING   = 2,
  PAUSED    = 3,
  COMPLETED = 4,
  FAILED    = 5,
};

// Shared data types
using DataVariant = std::variant<int64_t, double, std::string, bool>;
using DataValue   = DataVariant;  // Alias for consistency
using DataMap     = std::map<std::string, DataVariant>;
// Event types for control flow
enum class EventType {
  NODE_START       = 0,
  NODE_COMPLETE    = 1,
  NODE_FAILURE     = 2,
  NODE_RETRY       = 3,
  DATA_READY       = 4,
  SYNC_WAIT        = 5,
  DEPENDENCY_READY = 6,
};

// Error handling strategies
enum class ErrorStrategy {
  FAIL_FAST    = 0,  // Stop immediately on error
  RETRY        = 1,  // Retry on error
  FALLBACK     = 2,  // Use fallback handler
  COMPENSATION = 3,  // Execute compensation logic
  IGNORE       = 4,  // Ignore and continue
};

// MMAgent lifecycle policy
enum class AgentLifecyclePolicy {
  PER_REQUEST = 0,  // Create new instance per request
  FACTORY     = 1,  // Use factory to manage instances
  SINGLETON   = 2,  // Single global instance
};

// Data transfer mode
enum class DataTransferMode {
  SHARED_MEMORY   = 0,  // Zero-copy shared memory
  MESSAGE_QUEUE   = 1,  // Message queue copy
  REMOTE_TRANSFER = 2,  // Remote transfer
};

// MMNode configuration
struct NodeConfig {
  std::string                        name;
  std::string                        type;
  int                                priority       = 0;
  int                                timeout_ms     = 30000;  // Default 30 seconds
  int                                max_retries    = 0;      // No retry by default
  ErrorStrategy                      error_strategy = ErrorStrategy::FAIL_FAST;
  std::map<std::string, std::string> metadata;  // For extension

  NodeConfig() = default;
  NodeConfig(const std::string& n, const std::string& t) : name(n), type(t) {}
};

// MMWorkflow configuration
struct WorkflowConfig {
  std::string                        name;
  std::string                        version = "1.0.0";
  std::string                        description;
  int                                timeout_ms = 300000;  // Default 5 minutes
  std::map<std::string, std::string> metadata;

  WorkflowConfig() = default;
  explicit WorkflowConfig(const std::string& n) : name(n) {}
};

// MMAgent configuration
struct AgentConfig {
  std::string                        name;
  std::string                        version = "1.0.0";
  std::string                        description;
  AgentLifecyclePolicy               lifecycle = AgentLifecyclePolicy::FACTORY;
  std::vector<std::string>           tags;
  std::map<std::string, std::string> metadata;

  AgentConfig() = default;
  explicit AgentConfig(const std::string& n) : name(n) {}
};
}  // namespace agentflow

#endif  // AGENTFLOW_COMMON_H
