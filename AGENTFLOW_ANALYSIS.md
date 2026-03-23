# AgentFlow C++ Repository Analysis

## 1. BUILD, TEST, AND LINT COMMANDS

### Build Commands
- **Basic build**: `./build.sh build`
- **Release build**: `./build.sh build Release`
- **RelWithDebInfo build**: `./build.sh build RelWithDebInfo`
- **Clean build**: `./build.sh clean`
- **Rebuild (clean + build)**: `./build.sh rebuild`

### Test Commands
- **All tests via ctest**: `./build.sh test`
- **Direct ctest command**: `ctest --test-dir build --output-on-failure`
- **Specific CTest target**: `ctest --test-dir build -R NodeTests --output-on-failure`
- **Single GoogleTest case**: `./build/test_hot_reload --gtest_filter=HotReloadTest.SoLibrary_*`
- **Parallel testing**: `ctest --test-dir build -j $(nproc) --output-on-failure`

### Example Execution
- **Run example**: `./build.sh example <name>` (default: databus)
- **Available examples**: databus, basic_agent, multi_session_chat, etc.

### Code Quality (No dedicated lint target)
```bash
# Formatting check
clang-format --dry-run --Werror src/your_file.cpp

# Formatting fix (in-place)
clang-format -i -style=file src/your_file.cpp

# Static analysis
clang-tidy -p build src/your_file.cpp
cppcheck --enable=all src/
cpplint --linelength=100 src/your_file.cpp
```

### Build Configuration
- **Platform detection**: Automatic (macOS Darwin / Linux)
- **Generator preference**: Ninja (if available), fallback to Unix Makefiles
- **C++ Standard**: C++17 (required)
- **Compiler**: GCC 7+ or Clang 5+
- **CMake minimum**: 3.10+
- **CPU detection**: Automatic parallelization ($(detect_cores))

### Environment Support
- **macOS**: Homebrew support for DYLD_LIBRARY_PATH and DYLD_FALLBACK_LIBRARY_PATH
- **Linux**: LD_LIBRARY_PATH and multiarch library paths
- **Conda**: Automatic conda environment detection and path configuration

## 2. HIGH-LEVEL ARCHITECTURE

### Core Concept
AgentFlow is a **C++ Actor-based reactive programming framework** implementing MMAgent/MMWorkflow development orchestration and execution engine. It separates development/orchestration framework from runtime execution.

### "4+1" Architecture Model
1. **Framework Layer**: `include/internal/agent/`, `include/internal/workflow/`, `src/agent/`, `src/workflow/`
   - **MMAgent**: Lifecycle management, session routing, session/task instance management, agent-level context
   - **MMAgentSession**: Session state, message queues, task collection
   - **SessionTaskCoordinator**: Generates dispatcher/coordinator/skill tasks per session template
   - **MMWorkflow**: Graph structure, control flow (if/switch-case), execution state propagation
   - **MMNode**: Action definitions with state machine; supports green/yellow/red execution risk levels
   - **MMEdge**: Dependency definitions with control flow

2. **Runtime Layer**: `include/internal/runtime/`, `src/runtime/`
   - **MMAgentRuntime**: Central runtime with Scheduler + Deployer
   - **Scheduler**: Task → workflow instance mapping
   - **Deployer**: Workflow instance → worker mapping with rebalancing
   - **MMWorkerEngine**: Unified execution engine for all external modes (C++ SO, Python, Shell, Go, Java)

3. **Infrastructure Layer**: `src/runtime/databus/`, `vendor/adapter/`
   - **MMDataBus**: Unified local/remote data storage with DataRef for zero-copy transfers
   - **Adapters**: Protoactor, Redis, OpenSSL, RapidJSON (all with graceful fallback stubs)

### Three-Tier Scheduling Model
```
MMAgent (Master) → MMWorkflow (Master) → MMNode (Worker)
```
- Runtime continuously asks workflows for executable nodes
- Dispatches nodes to worker threads
- Exchanges node inputs/outputs through MMDataBus

### Core Features
- **Dual framework architecture**: Development orchestration + Runtime execution separation
- **Workflow graph execution**: Conditional branches, loops, backjumps, exception handling
- **Three-tier scheduling**: Master-Worker architecture across MMAgent/MMWorkflow/MMNode
- **High-performance communication**: Zero-copy data bus, dual-plane communication
- **Multi-language support**: C++, Python, Shell, Go, Java execution modes (unified via Worker)
- **Hot reload**: Runtime dynamic config and code updates

## 3. KEY CONVENTIONS AND DATA STRUCTURES

### Header Organization
- **External API** (`include/external/`): Consumer-facing public APIs
  - `agent/` - MMAgent public interfaces
  - `workflow/` - MMWorkflow, MMNode, MMEdge public interfaces
  - `runtime/` - DataBus SDK for external clients
- **Internal Implementation** (`include/internal/`): Internal framework only
  - `agent/` - MMAgent internals (session, task, dispatcher, coordinator)
  - `workflow/` - MMWorkflow/MMNode internals
  - `runtime/` - Runtime internals (master, worker, databus)
  - `vendor/` - Adapter interfaces
  - `utils/` - Utilities (logger, thread pool, file storage)

### Key Data Structures

#### MMAgent
- Lifecycle management for agent instances
- Session routing and session pool management
- Session/task instance management
- Agent-level context maintenance
- Config: agent_id, session_pool_size, max_workflows, max_concurrent_workflows

#### MMWorkflow
- Graph structure representation
- Control flow definitions (if/switch-case)
- Execution state propagation
- Nodes and edges composition
- Config: workflow_id, nodes, edges, control_flow_rules

#### MMNode
- **Field Types**: STRING, INT, FLOAT, BOOL, LIST, MAP, FILE, FILE_LIST
- **Transfer Modes**: VALUE (copy) or REFERENCE (zero-copy)
- **Execution Modes**: 
  - CPP_FUNCTION (local C++ lambda)
  - SHELL_SCRIPT (shell execution)
  - EXECUTABLE (external program)
  - GO_SCRIPT (Go script)
  - PYTHON_SCRIPT (Python script)
  - JAVA_JAR (Java JAR)
  - SO_LIBRARY (shared object)
- **Execution Risk Levels**: GREEN, YELLOW, RED
- Config: node_id, input_definitions, output_definitions, execution_mode, command, working_dir, timeout_ms, max_retries

#### MMEdge
- **Control Types**: NONE, IF, SWITCH_CASE
- Dependency definitions with control flow conditions
- Connects MMNodes in workflow graph

#### MMDataBus
- Unified local/remote data storage
- DataRef for zero-copy transfers
- Namespace isolation
- UDS (Unix Domain Socket) server support for remote access
- Local in-memory storage with optional Redis adapter

### Session/Task Configuration Model
The `agent.json` schema uses:
- **session_pool_size**: Pre-created session pool size (homogeneous template)
- **tasks[]**: Agent-level and session-level tasks
  - Agent-level: `contextupdater`
  - Session-level: `dispatcher`, `coordinator`, `skill` (1 of each per session)
- **task.workflow_pool_size**: Workflow instance pool size per task
- **skill.instance_pool_size**: Skill task instance pool size
- **dispatcher**: Built-in framework workflow with next_skills mechanism
- **coordinator**: Skill coordination and output assembly

### Workflow DSL (JSON/JSONC Format)
Located in: `publish/workflow/workflow_sample.jsonc` and parsed by `src/workflow/dsl/workflow_parser.cpp`

**Top-level structure**:
```json
{
  "version": "1.0",                    // DSL schema version
  "id": "workflow_id",                 // Unique identifier
  "name": "Human-readable name",       // Display name
  "description": "Purpose",            // Description
  "dsl_file": "relative/path",         // Path to this file
  "metadata": { ... },                 // Freeform metadata
  "nodes": [ ... ],                    // Array of node definitions
  "edges": [ ... ]                     // Array of edge definitions
}
```

**Node DSL structure**:
```json
{
  "id": "node_id",                     // Required, unique per workflow
  "name": "Display name",              // Optional, default: id
  "type": "source|processor|sink",     // Optional, default: "default"
  "priority": 10,                      // Optional, default: 0
  "execution_mode": "cpp_function",    // Modes: cpp_function, shell_script, executable, go_script, python_script, java_jar, so_library
  "function_name": "func_name",        // For so_library mode
  "command": "path/to/script",         // Script/executable/SO path
  "working_dir": "/path",              // Working directory
  "timeout_ms": 5000,                  // Execution timeout
  "max_retries": 3,                    // Retry policy
  "inputs": [
    { "name": "input_name", "type": "string", "required": true, "transfer_mode": "value" }
  ],
  "outputs": [
    { "name": "output_name", "type": "string", "transfer_mode": "reference" }
  ],
  "execution_risk_level": "green"      // green|yellow|red
}
```

**Edge DSL structure**:
```json
{
  "id": "edge_id",                     // Optional unique identifier
  "from": "source_node_id",            // Source node
  "to": "target_node_id",              // Target node
  "control_type": "if",                // NONE|IF|SWITCH_CASE
  "condition": "context.field == value" // Condition expression (for IF/SWITCH_CASE)
}
```

### Agent.json Schema
Located in: `publish/agent/agent.json`

**Required fields**:
- `name`: MMAgent name
- `version`: Agent version
- `description`: Functional overview
- `session_pool_size`: >= 1, pre-created session pool
- `max_workflows`: >= 1, max loadable workflows
- `max_concurrent_workflows`: >= 1, max concurrent executions
- `tasks[]`: Task list with id, type, scope, workflow_file, workflow_pool_size

**Task types**:
- `contextupdater` (agent scope): Agent-level context updates
- `dispatcher` (session scope): Built-in request dispatcher
- `coordinator` (session scope): Skill output coordination
- `skill` (session scope): Skill execution with instance pool

**Dispatcher special fields**:
- `next_skills`: Array of skill names to trigger
- `max_concurrent_skills`: Max concurrent skill execution

**Skill special fields**:
- `skill.name`: Skill identifier
- `skill.config_dir`: Configuration directory
- `skill.instance_pool_size`: >= 1, default 2
- `skill.expected_parameters`: List of required parameter names

**Skill output conventions**:
- `response`: Execution result text
- `next_skills`: List of subsequent skill names
- `skill_completed`: Boolean completion flag
- `available_parameters`: { "skill_name": { "param": "value" } }

### Test Organization

#### Unit Tests
Located in: `tests/unit/src/`
- **test_agent_gtest.cpp**: Agent lifecycle and session management
- **test_workflow_gtest.cpp**: Workflow graph and execution
- **test_node_gtest.cpp**: Node execution and data transfer
- **test_databus_gtest.cpp**: Data storage and retrieval
- **test_dsl_adapter_gtest.cpp**: DSL parsing and adapter behavior
- **test_control_message_gtest.cpp**: Control flow and messaging

#### E2E/Blackbox Tests
Located in: `tests/e2e/`
- `workflow_execution/`: Workflow execution scenarios
- `message_routing/`: Session and message routing
- `llm_query_agent/`: LLM integration tests
- `system_agent_mock/`: System mock tests
- `blackbox/`: Behavioral/integration tests

#### Test Execution
- Build: Generated via CMakeLists.txt with GoogleTest linking
- Run: Via CTest framework (registered as ctest targets)
- Targets: `test_agent_gtest`, `test_workflow_gtest`, `test_node_gtest`, `test_databus_gtest`, `test_dsl_adapter_gtest`, `test_control_message_gtest`

### CMake Conditional Compilation Flags

#### Mandatory Flags
```cmake
CMAKE_CXX_STANDARD = 17              # C++ standard
CMAKE_BUILD_TYPE = Debug|Release|RelWithDebInfo
```

#### Optional Feature Flags
```cmake
AGENTFLOW_WITH_PROTOACTOR  # Actor system (ProtoActor-cpp)
AGENTFLOW_WITH_RAPIDJSON   # JSON parsing (RapidJSON)
AGENTFLOW_WITH_OPENSSL     # SSL/TLS support
AGENTFLOW_WITH_REDIS       # Redis adapter for DataBus
```

**Dependency Fallback Strategy**:
- All optional dependencies have graceful stub implementations
- When a dependency is missing, stub adapters are expected behavior
- Do NOT remove fallback code paths
- CMake will emit STATUS messages indicating which features are enabled/disabled

#### Detection Paths
- **Conda environments**: Automatic detection of `CONDA_PREFIX`
- **Homebrew (macOS)**: `/opt/homebrew` or `/usr/local`
- **System paths**: `/usr/local`, `/usr/include`, `/usr/lib`
- **Multiarch (Linux)**: Automatic detection via gcc -dumpmachine

### Code Style Conventions
- **Format**: Google C++ style via `.clang-format` (2-space indent, 100-column limit)
- **Naming**: snake_case for functions, PascalCase for types/classes
- **Headers**: Short project paths: `#include "agentflow/framework/agent.h"`
- **Linting**: `.clang-tidy` and `.clang-format` configuration files present
- **Wall -Wextra**: Strict warning flags enabled

## 4. EXAMPLES STRUCTURE

Located in: `examples/agent/` (also `examples/features/`)

### Agent Examples
Each example is a self-contained MMAgent package demonstrating specific capabilities:

1. **basic_agent**: Minimal agent with shell and Python nodes
   - Covers: Minimal package structure, shell execution, Python execution
   
2. **multi_session_chat**: Multi-round dialogue with multi-language nodes
   - Covers: C++, Go, Python, Java node execution, session management
   - E2E test: `E2EMultiSessionChatTUI`
   
3. **claw_agent**: Dispatcher directly connecting vendored jiuwenclaw chat
   - Covers: Custom dispatcher, TUI integration
   - E2E test: `E2EClawAgentTUI`
   
4. **llm_query_agent**: LLM task orchestration with dependency on llm-query library
   - Covers: External dependency management, workflow dependencies
   - E2E test: `E2ELLMQueryAgent`
   
5. **openjiuwen_studio_agent**: Agent-studio bundle integration
   - Covers: Artifact bundling, external tool integration
   - E2E test: `E2EOpenJiuwenStudioAgentTUI`
   
6. **openjiuwen_multi_session_chat**: Python-only multi-round dialogue
   - Covers: Pure Python implementation, openJiuwen compatibility
   - E2E test: `E2EOpenJiuwenMultiSessionChat`
   
7. **nested_workflow_agent**: Nested workflow patterns
   
8. **workflow_patterns**: Reusable workflow pattern templates
   
9. **system_agent_mock**: System mock for testing
   
10. **multi_language_databus**: Multi-language DataBus integration

### Feature Examples
Located in: `examples/features/`
- **interactive_runner_demo.md**: Message-driven execution model with `InteractiveRunner`
- **agent_runner_design.md**: Agent runner design documentation

### Example Build Pattern
- CMakeLists.txt defines example targets
- Examples can depend on core agentflow library
- Each example self-contained with own workflows and configurations
- Multi-node examples show multi-language capability

## 5. EXISTING CONVENTION FILES

### Present
✓ **.github/copilot-instructions.md** (2,972 bytes)
  - Existing Copilot instructions focused on build/test commands
  - High-level architecture summary
  - Repository-specific conventions

✓ **CLAUDE.md** (2,126 bytes)
  - Claude Code guidance
  - Build/test commands
  - Architecture overview
  - Key files reference
  - Examples reference

✓ **CONTRIBUTING.md** (895 bytes, Chinese)
  - Fork and branch creation guidelines
  - Build/test execution
  - Commit message format: type(scope): subject
  - PR requirements
  - Issue feedback guidelines

### Absent (NOT present)
✗ **.cursorrules**
✗ **.clinerules**
✗ **.windsurfrules**
✗ **AGENTS.md**
✗ **CONVENTIONS.md**
✗ **AIDER_CONVENTIONS.md**

## 6. PROJECT STATUS & ROADMAP

From `docs/REFACTORING.md`:
- **Development Orchestration Framework**: 85% complete
- **Runtime Framework**: 75% complete
- **Infrastructure**: 48% complete
- **Monitoring and Logging**: 58% complete

## 7. IMPORTANT DESIGN PATTERNS

### Message-Driven Execution
- `MMAgentMsgQue` with `route_request_message()` and `dequeue_response_message()`
- Session-based message routing with user_id + client_id

### Dual-Queue Workers
- `control` queue: Control messages
- `business` queue: Execution tasks

### Task Dual Queues
- `intent_request`: Submit work to worker
- `data_exchange`: Receive worker responses

### DataBus-First Communication
- Nodes read/write through MMDataBus instead of files
- Support for local and remote (Redis) storage
- DataRef for zero-copy data transfer

### Built-in Dispatcher
- Framework handles `next_skills` mechanism uniformly
- Skill scheduling and coordination transparent to user

## 8. DIRECTORY STRUCTURE SUMMARY

```
AgentFlow/
├── include/
│   ├── external/              # Public API surface
│   │   ├── agent/
│   │   ├── agentflow/
│   │   ├── runtime/
│   │   └── workflow/
│   └── internal/              # Internal implementation
│       ├── agent/             # MMAgent internals
│       ├── runtime/           # Runtime & scheduling
│       ├── utils/
│       ├── vendor/            # Adapter interfaces
│       └── workflow/          # MMWorkflow/MMNode internals
├── src/                       # Implementation
│   ├── agent/
│   ├── runtime/
│   ├── utils/
│   └── workflow/
├── vendor/                    # Third-party adapters
│   ├── adapter/               # Actor, JSON, Event, Network, Serialization
│   └── openjiuwen/
├── tests/
│   ├── unit/
│   ├── e2e/
│   ├── functional/
│   └── perf/
├── examples/
│   ├── agent/
│   └── features/
├── docs/                      # Comprehensive documentation
├── publish/                   # Agent package specs
├── CMakeLists.txt
├── build.sh
├── CONTRIBUTING.md
├── CLAUDE.md
├── README.md
└── .github/
    └── copilot-instructions.md
```

## 9. KEY DOCUMENTATION FILES

| File | Purpose | Key Sections |
|------|---------|------|
| `docs/ARCHITECTURE.md` | Complete 5-view architecture | Use cases, components, data flow, deployment |
| `docs/REFACTORING.md` | Feature roadmap & code assessment | Implementation status, priorities |
| `CONTRIBUTING.md` | Contribution process | Commit format, PR requirements |
| `publish/agent/AGENT_PACKAGE_SPEC.md` | Agent packaging schema | agent.json structure, task types |
| `publish/workflow/workflow_sample.jsonc` | Workflow DSL reference | Complete DSL with all field documentation |
| `src/workflow/dsl/workflow_parser.cpp` | DSL parser implementation | Parsing logic, validation rules |
| `docs/databus_user_manual.md` | DataBus usage guide | Local/remote storage, operations |
| `docs/agent-lifecycle.md` | Agent lifecycle management | Init, start, stop, cleanup |

