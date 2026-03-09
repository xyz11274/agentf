# AgentFlow Copilot Instructions

## Build, test, and quality commands

Use the repository script unless you need custom CMake flags.

```bash
# Build (Debug by default; supports Release/RelWithDebInfo)
./build.sh build
./build.sh build Release

# Run all tests
./build.sh test
# equivalent direct command:
ctest --test-dir build --output-on-failure

# Run a single registered CTest target
ctest --test-dir build -R NodeTests --output-on-failure
ctest --test-dir build -R AgentFlowGoogleTests --output-on-failure

# Run a single GoogleTest case (example)
./build/test_hot_reload --gtest_filter=HotReloadTest.SoLibrary_*
```

There is no dedicated lint target in `build.sh`; quality checks are run directly:

```bash
clang-format --dry-run --Werror src/your_file.cpp
clang-format -i -style=file src/your_file.cpp
clang-tidy -p build src/your_file.cpp
cppcheck --enable=all src/
cpplint --linelength=100 src/your_file.cpp
```

## High-level architecture

- AgentFlow is split into two core layers:
  - `agentflow::framework`: authoring/orchestration objects (`MMAgent`, `MMWorkflow`, `MMNode`, `MMEdge`) and workflow-graph/control-flow definitions.
  - `agentflow::runtime`: execution-time scheduling and infrastructure (runtime actor loop, databus, metrics, control flow, hot reload).
- Scheduling model is three-tier: **MMAgent -> MMWorkflow -> MMNode**. Runtime repeatedly asks workflows for executable nodes, dispatches nodes to worker threads, and exchanges node inputs/outputs through the MMDataBus.
- Public API surface is intentionally narrow under `include/external/{agent,workflow}`; most implementation and extension points live under `include/internal/` and `src/`.
- Optional integrations are isolated behind adapters in `vendor/adapter/` (actor/json/event/network/serialization), with compile-time switches enabling full implementations when dependencies are present.

## Repository-specific conventions

- Respect dependency fallback design:
  - `AGENTFLOW_WITH_PROTOACTOR`, `AGENTFLOW_WITH_RAPIDJSON`, `AGENTFLOW_WITH_OPENSSL` drive conditional compilation.
  - If a dependency is missing, stub adapters are expected behavior (do not remove fallback paths).
- Keep internal vs external header boundaries intact:
  - external consumer-facing APIs in `include/external/`
  - framework/runtime internals in `include/internal/`
  - includes in code use short project paths (for example `#include "agent/common.h"`).
- `build.sh test` sets runtime library search paths (`DYLD_LIBRARY_PATH` on macOS, `LD_LIBRARY_PATH` on Linux); if running binaries manually, preserve equivalent library path behavior.
- C++ style in this repository follows `.clang-format` (Google base, 2-space indent, 100-column limit, snake_case function names with PascalCase types).
- MMWorkflow DSL is JSON/JSONC and should align with parser behavior in `src/workflow/dsl/workflow_parser.cpp`; update `publish/workflow/workflow_sample.jsonc`-style fields consistently when DSL keys change.
