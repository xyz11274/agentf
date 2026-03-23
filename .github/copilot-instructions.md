# AgentFlow Copilot Instructions

## Build, test, and quality commands

Use the repository script unless you need custom CMake flags.

```bash
# Build (Debug by default; supports Release/RelWithDebInfo)
./build.sh build
./build.sh build Release

# Clean and rebuild
./build.sh rebuild

# Run all tests
./build.sh test
# equivalent direct command:
ctest --test-dir build --output-on-failure

# Run a single registered CTest target (use -R with any name from the list below)
ctest --test-dir build -R NodeTests --output-on-failure
ctest --test-dir build -R DSLAdapterTests --output-on-failure

# Run a single GoogleTest case
./build/<test_binary> --gtest_filter=SuiteName.TestName_*

# Run an example
./build.sh example <name>
```

Registered CTest targets: `AgentFlowGoogleTests`, `AdapterIsolationTests`, `AgentLoadingTests`, `AgentTests`, `WorkflowTests`, `NodeTests`, `DataBusTests`, `DSLAdapterTests`, `ControlMessageTests`, `E2EMessageRouting`, `E2EBasicAgent`, `E2ENestedWorkflow`, `E2EInteractiveRunner`, `E2EDataBusClients`, `E2EMultiSessionChatTUI`, `E2EOpenJiuwenMultiSessionChat`, `E2EClawAgentTUI`, `E2EOpenJiuwenStudioAgentTUI`.

There is no dedicated lint target in `build.sh`; quality checks are run directly:

```bash
clang-format --dry-run --Werror src/your_file.cpp
clang-format -i -style=file src/your_file.cpp
clang-tidy -p build src/your_file.cpp
cppcheck --enable=all src/
cpplint --linelength=100 src/your_file.cpp
```

## High-level architecture

AgentFlow uses a "4+1" architecture: Framework orchestration + Runtime scheduling + MMDataBus shared data.

**Framework Layer** (`include/internal/agent/`, `include/internal/workflow/`, `src/agent/`, `src/workflow/`):
- `MMAgent`: Lifecycle management, session routing, session/task instance management
- `MMAgentSession`: Session state, message queues, task collection
- `SessionTaskCoordinator`: Generates dispatcher/coordinator/skill tasks per session template
- `MMWorkflow`: Graph structure, control flow (if/switch-case), execution state propagation
- `MMNode`: Action definitions with state machine; supports `green/yellow/red` execution risk levels
- `MMEdge`: Dependency definitions with control flow

**Runtime Layer** (`include/internal/runtime/`, `src/runtime/`):
- `MMAgentRuntime`: Central runtime with Scheduler + Deployer
- `Scheduler`: Task → workflow instance mapping (same agent instance > same agent object > min load)
- `Deployer`: Workflow instance → worker mapping with rebalancing
- `MMWorkerEngine`: Unified execution engine for all external execution modes (C++ SO, Python, Shell, Go, Java)

**Infrastructure Layer** (`src/runtime/databus/`, `vendor/adapter/`):
- `MMDataBus`: Unified local/remote data storage with `DataRef` for zero-copy transfers
- Adapters: Protoactor, Redis, OpenSSL, RapidJSON (all with graceful fallback stubs)

### Three-tier scheduling model

```
MMAgent (Master) → MMWorkflow (Master) → MMNode (Worker)
```

Runtime continuously asks workflows for executable nodes, dispatches them to worker threads, and exchanges inputs/outputs through MMDataBus.

### Session/Task configuration model (`agent.json`)

- `session_pool_size`: Number of homogeneous session instances to replicate
- `tasks[]`: Agent-level tasks (e.g., `contextupdater`) at top level
- `sessions[]`: Single session template with 1 `dispatcher`, 1 `coordinator`, N `skill` tasks
- `task.workflow_pool_size`: Workflow instance pool size per task
- `skill.instance_pool_size`: Skill task instance pool size (defaults to 2)

### Key design patterns

- **Message-driven execution**: `MMAgentMsgQue` with `route_request_message()` / `dequeue_response_message()`; session routing uses `user_id + client_id`
- **Dual-queue workers**: `control` / `business` queues in `MMWorkerEngine`
- **Task dual queues**: Each task has `intent_request` (submit to worker) and `data_exchange` (receive worker responses)
- **DataBus-first node communication**: Nodes read/write through MMDataBus instead of files; `DataRef` enables zero-copy transfers
- **Built-in Dispatcher**: Framework handles `next_skills` mechanism uniformly for dispatcher/coordinator/skill outputs

### MMNode field vocabulary (workflow DSL)

**Execution modes**: `cpp_function`, `shell_script`, `executable`, `go_script`, `python_script`, `java_jar`, `so_library`

**Field types**: `string`, `int`, `float`, `bool`, `list`, `map`, `file`, `file_list`

**Transfer modes** (per field): `value` (copy) · `ref` (zero-copy DataRef)

**Risk levels**: `green` (safe) · `yellow` (caution) · `red` (critical)

**Edge control types**: `none` (unconditional) · `if` · `switch_case`

**Standard skill output keys**: `response`, `next_skills`, `skill_completed`, `available_parameters`

## Repository-specific conventions

- **Dependency fallback**: `AGENTFLOW_WITH_PROTOACTOR`, `AGENTFLOW_WITH_RAPIDJSON`, `AGENTFLOW_WITH_OPENSSL` drive conditional compilation. Never remove stub fallback paths — they are expected behavior when dependencies are absent.
- **Header boundaries**: External consumer-facing APIs in `include/external/`; framework/runtime internals in `include/internal/`. Code uses short project-relative include paths (e.g., `#include "agent/common.h"`).
- **Library paths**: `build.sh test` sets `DYLD_LIBRARY_PATH` (macOS) / `LD_LIBRARY_PATH` (Linux). Preserve this when running binaries manually.
- **C++ style**: Google base via `.clang-format` — 2-space indent, 100-column limit, `snake_case` function names, `PascalCase` types.
- **Workflow DSL**: JSON/JSONC, parsed by `src/workflow/dsl/workflow_parser.cpp`. When DSL keys change, update `publish/workflow/workflow_sample.jsonc` consistently.
- **Commit format**: `type(scope): subject` where type is one of `feat|fix|docs|refactor|test|chore`; subject in imperative mood.
- **Branch naming**: `feature/<name>` or `fix/<name>`.
- **PR requirement**: If the change touches DSL, package spec, or public API, update corresponding docs in `publish/` or `docs/`.

## Key files

| File | Purpose |
|------|---------|
| `docs/ARCHITECTURE.md` | 5-view architecture documentation |
| `docs/REFACTORING.md` | Feature status, roadmap, code assessment |
| `publish/agent/AGENT_PACKAGE_SPEC.md` | `agent.json` schema reference |
| `publish/INTEGRATION_GUIDE.md` | Service integration guide |
| `src/workflow/dsl/workflow_parser.cpp` | Workflow DSL parser (source of truth for DSL keys) |
| `publish/workflow/workflow_sample.jsonc` | Canonical workflow JSON/JSONC sample |

## Examples

| Example | Location | Description |
|---------|----------|-------------|
| Basic Agent | `examples/agent/basic_agent` | Minimal agent with shell/Python nodes |
| Multi-Session Chat | `examples/agent/multi_session_chat` | Multi-language nodes (C++/Go/Python/Java) |
| Claw Agent | `examples/agent/claw_agent` | Dispatcher with vendored `jiuwenclaw`, TUI entry |
| OpenJiuwen Studio | `examples/agent/openjiuwen_studio_agent` | Adapter for agent-studio bundles, TUI entry |
| OpenJiuwen Multi-Session | `examples/agent/openjiuwen_multi_session_chat` | Python-only multi-session with openJiuwen layer |
| LLM Query Agent | `examples/agent/llm_query_agent` | LLM workflow with dynamic library dependency |
| Nested Workflow | `examples/agent/nested_workflow_agent` | Nested workflow composition patterns |
| Workflow Patterns | `examples/agent/workflow_patterns` | Reusable workflow patterns |
