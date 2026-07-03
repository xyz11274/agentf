# LLM Query Agent

An AgentFlow agent that queries Large Language Models (LLM) using the llm-query library.

## Overview

This agent demonstrates the complete dispatcher/coordinator/skill architecture, and now also bundles a local echo workflow for no-dependency smoke coverage:

- **Dispatcher**: Receives user requests, routes to coordinator
- **Coordinator**: Coordinates processing, forwards to skill
- **Skill (LLM Query)**: Executes the actual LLM query
- **Local Echo Workflow**: Provides a shell-only fallback path for simple routing and response tests

The skill workflow uses a two-node pipeline:
1. **Config Node** (Shell) - Reads configuration from environment variables
2. **LLM Node** (C++ SO Library) - Calls llm-query library

## Prerequisites

### Dependencies

1. **llm-query library** - Must be built and available at `/root/projects/llm-query`
   ```bash
   cd /root/projects/llm-query
   mkdir -p build && cd build
   cmake ..
   make
   ```

2. **libcurl** - HTTP client library
3. **nlohmann_json** - JSON library (fetched automatically by llm-query)

## Configuration

Set the following environment variables before running:

```bash
# Required: Your LLM API key
export LLM_API_KEY="your-api-key-here"

# Optional: Model name (default: qwen3.5-plus)
export LLM_MODEL="qwen3.5-plus"

# Optional: API base URL (default: Alibaba Cloud DashScope)
export LLM_BASE_URL="https://dashscope.aliyuncs.com/compatible-mode/v1"
```

### Supported LLM Providers

This agent works with any OpenAI-compatible API:

- **Alibaba Cloud DashScope** (default): Uses Qwen models
- **OpenAI**: Set `LLM_BASE_URL="https://api.openai.com/v1"`
- **Azure OpenAI**: Set appropriate base URL
- **Local LLM servers**: Any OpenAI-compatible server

## Building

```bash
# Build the AgentFlow project (includes llm_query_node.so)
./build.sh rebuild
```

After building, verify the SO library was created:
```bash
ls -la build/lib/libllm_query_node.so
```

## Running

```bash
# Set API key
export LLM_API_KEY="your-api-key"

# Start the dedicated TUI
LD_LIBRARY_PATH=/root/projects/llm-query/build/src/lib:$LD_LIBRARY_PATH \
  ./build/example_llm_query_agent_tui examples/agent/llm_query_agent
```

TUI 会将运行时日志写入 `examples/agent/llm_query_agent/runtime/logs/llm_query_agent_tui.log`，
交互窗口只保留 `you>` / `assistant>` / `status>` 输出。

## Architecture

```
User Input
    │
    ▼
┌─────────────────┐
│   Dispatcher    │  Routes to coordinator
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   Coordinator   │  Prepares prompt
└────────┬────────┘
         │
         ▼
┌─────────────────────────────────────┐
│     LLM Query Skill Workflow        │
│  ┌──────────────┐   ┌────────────┐  │
│  │  Config Node │──▶│  LLM Node  │  │
│  │   (Shell)    │   │  (C++ SO)  │  │
│  └──────────────┘   └────────────┘  │
└─────────────────────────────────────┘
         │
         ▼
    LLM Response
```

## Files

| File | Description |
|------|-------------|
| `agent.json` | Agent definition with dispatcher/coordinator/skill |
| `package.json` | Package metadata |
| `dispatcher_workflow.json` | Dispatcher workflow definition |
| `coordinator_workflow.json` | Coordinator workflow definition |
| `llm_query_workflow.json` | Main skill workflow (config → llm) |
| `echo_workflow.json` | Local shell-only echo workflow for smoke coverage |
| `dispatcher_node.sh` | Shell node for dispatcher |
| `coordinator_node.sh` | Shell node for coordinator |
| `config_node.py` | Python node for reading config |
| `nodes/llm_node.cpp` | C++ SO library node for LLM query |

## Node Development

### Config Node (Python)

Reads configuration from environment variables and outputs JSON:
```json
{
  "api_key": "...",
  "model": "qwen3.5-plus",
  "base_url": "https://..."
}
```

### LLM Node (C++ SO Library)

Entry point signature:
```cpp
extern "C" void llm_query_node(const DataMap& inputs, DataMap& outputs);
```

Where:
- `DataMap = std::map<std::string, std::variant<std::string, int64_t, double, bool>>`
- Inputs: `api_key`, `model`, `base_url`, `prompt`
- Outputs: `status`, `response`, `error`

## Error Handling

The agent handles errors gracefully:

- **Missing API Key**: Returns error message asking to set `LLM_API_KEY`
- **API Errors**: Catches `llm_query::ApiError` and returns detailed error
- **Timeout**: LLM node has 120-second timeout configured

## Testing

```bash
# Test config node
python3 ./config_node.py

# Smoke test the TUI manually
printf 'What is 2+2?\n/quit\n' | \
  LD_LIBRARY_PATH=/root/projects/llm-query/build/src/lib:$LD_LIBRARY_PATH \
  ./build/example_llm_query_agent_tui examples/agent/llm_query_agent
```

## License

Part of the AgentFlow project.

## Local Echo Smoke Path

For no-dependency smoke validation, this package also contains `echo_workflow.json`. Tests can route requests with `workflow_id=echo-workflow` to verify dispatcher response delivery without requiring the `llm-query` dynamic library.
