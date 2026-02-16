# AgentFlow Implementation Guide

This directory contains the C++11 implementation of AgentFlow based on the architecture described in the README.

## Project Structure

```
AgentFlow/
├── include/
│   └── agentflow/
│       ├── common.h                 # Common types and enums
│       ├── basic/
│       │   └── logger.h            # Logging utility
│       ├── framework/
│       │   ├── agent.h             # Agent definition
│       │   ├── workflow.h          # Workflow definition
│       │   ├── node.h              # Node definition
│       │   └── edge.h              # Edge definition
│       └── runtime/
│           ├── agent_runtime.h     # Core scheduling engine
│           └── databus.h           # High-speed data bus
├── src/
│   ├── agent.cpp
│   ├── workflow.cpp
│   ├── node.cpp
│   ├── edge.cpp
│   ├── agent_runtime.cpp
│   ├── databus.cpp
│   └── logger.cpp
├── tests/
│   ├── test_main.cpp
│   ├── test_agent.cpp
│   ├── test_workflow.cpp
│   ├── test_node.cpp
│   └── test_databus.cpp
├── examples/
│   └── basic_example.cpp
├── CMakeLists.txt
└── build.sh
```

## Building

### Prerequisites
- C++11 compatible compiler (GCC, Clang, LLVM)
- CMake 3.10 or higher
- Linux/Unix environment (tested on Ubuntu 24.04)

### Build Steps

```bash
# Clean previous builds (optional)
./build.sh clean

# Build the project
./build.sh build

# Run tests
./build.sh test

# Run example
./build.sh example
```

Or manually:
```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
./test_agentflow       # Run tests
./example_basic        # Run example
```

## Component Overview

### 1. Development Orchestration Framework (`framework/`)

#### Agent
- Manages multiple workflows with shared resources
- Supports three lifecycle policies: Per-Request, Factory, Singleton
- Provides unified external interfaces
- Manages shared data bus and event queue

#### Workflow
- DAG (Directed Acyclic Graph) structure
- Composed of nodes and edges
- Supports dynamic DSL loading and updating
- Validates topology

#### Node
- Basic execution unit in workflow
- Has control plane and execution interface
- Supports configurable error handling (retry, timeout)
- Represents tasks with inputs/outputs

#### Edge
- Defines dependencies between nodes
- Supports multiple data transfer modes
- Implements control flow event management
- Enables high-speed data passing

### 2. Runtime Framework (`runtime/`)

#### HighSpeedDataBus
- Zero-copy data sharing between nodes
- Subscription/publication mechanism
- Thread-safe operations
- Configurable capacity

#### AgentRuntime
- Core three-level Master-Worker scheduling engine:
  - **Agent Master**: Manages multiple agent instances
  - **Workflow Master**: DAG scheduling and workflow management
  - **Node Worker**: Executes node tasks with thread pool
- Manages shared resources for each agent
- Coordinates workflow execution
- Provides statistics and monitoring

### 3. Basic Infrastructure (`basic/`)

#### Logger
- Simple logging utility with levels (DEBUG, INFO, WARN, ERROR)
- Timestamp support
- Thread-safe operations
- Formatted logging support

## Usage Example

```cpp
#include "agentflow/framework/agent.h"
#include "agentflow/framework/workflow.h"

using namespace agentflow;

// Create an agent
framework::Agent::Config agent_config;
agent_config.id = "my_agent";
agent_config.name = "MyAgent";
auto agent = std::make_shared<framework::Agent>(agent_config);

// Create a workflow
framework::Workflow::Config workflow_config;
workflow_config.id = "my_workflow";
workflow_config.name = "MyWorkflow";
auto workflow = std::make_shared<framework::Workflow>(workflow_config);

// Create and add nodes
framework::Node::Config node_config;
node_config.id = "task_1";
node_config.name = "TaskNode";
auto node = std::make_shared<framework::Node>(node_config);
workflow->add_node(node);

// Add workflow to agent
agent->add_workflow(workflow);

// Initialize and start
agent->initialize();
agent->start();

// ... use agent ...

agent->stop();
```

## Key Features Implemented

✓ Two-framework architecture (Development + Runtime)
✓ Agent with resource management
✓ Workflow DAG structure and validation
✓ Node execution model with state management
✓ Edge dependency and data transfer
✓ High-speed data bus with subscriptions
✓ Three-level AgentRuntime scheduling engine
✓ Thread-safe operations
✓ Basic logging framework
✓ Comprehensive test suite
✓ Build and example scripts

## Future Enhancements

- [ ] DSL parser for JSON workflow definitions
- [ ] Execution agent (remote worker) support
- [ ] Distributed scheduling with consensus
- [ ] Advanced monitoring and metrics
- [ ] Actor-based node implementation (with Protoactor-cpp)
- [ ] Performance optimization for high-speed data bus
- [ ] Workflow persistence and recovery
- [ ] Better error handling and recovery mechanisms

## Testing

The project includes comprehensive test coverage:
- `test_node.cpp`: Node creation, initialization, execution
- `test_workflow.cpp`: Workflow DAG, node/edge management
- `test_agent.cpp`: Agent lifecycle, resource management
- `test_databus.cpp`: Data bus operations and subscriptions

Run tests with: `./build.sh test`

## Notes

- Compiled against C++11 standard for broad compatibility
- Uses standard library only (no external dependencies except CMake)
- Thread-safe implementations using std::mutex
- Simplified data variant type (std::string) - can be extended with more complex types
- Logger uses singleton pattern
- All public APIs documented with Doxygen-style comments
