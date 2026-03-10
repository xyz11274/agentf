# LLM Query Agent

An AgentFlow agent that queries Large Language Models (LLM) using the llm-query library.

## Overview

This agent demonstrates the complete dispatcher/coordinator/skill architecture:

- **Dispatcher**: Receives user requests, routes to coordinator
- **Coordinator**: Coordinates processing, forwards to skill
- **Skill (LLM Query)**: Executes the actual LLM query

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

# Run the agent runner with LLM query agent
cd examples/agent/llm_query_agent
LD_LIBRARY_PATH=/root/projects/llm-query/build/src/lib:$LD_LIBRARY_PATH \
  ../../../build/bin/agent_runner . "What is the capital of France?"
```

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
| `dispatcher_node.sh` | Shell node for dispatcher |
| `coordinator_node.sh` | Shell node for coordinator |
| `config_node.sh` | Shell node for reading config |
| `nodes/llm_node.cpp` | C++ SO library node for LLM query |

## Node Development

### Config Node (Shell)

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
./config_node.sh

# Test with curl (requires agent running)
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d '{"input": "Hello, AI!"}'
```

## License

Part of the AgentFlow project.