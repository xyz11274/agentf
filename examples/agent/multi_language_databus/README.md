# Multi-Language DataBus Test Agent

This agent demonstrates multi-language DataBus access via Unix Domain Socket (UDS). It chains nodes written in C++, Python, Go, and Shell to verify cross-language data exchange.

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌────────────────┐     ┌─────────────────┐
│  C++ Producer   │────▶│ Python Transform │────▶│  Go Processor  │────▶│ Shell Verifier  │
│  (SO Library)   │     │    (Script)      │     │   (Script)     │     │    (Script)     │
└─────────────────┘     └──────────────────┘     └────────────────┘     └─────────────────┘
        │                        │                      │                       │
        └────────────────────────┴──────────────────────┴───────────────────────┘
                                        │
                                ┌───────▼───────┐
                                │   DataBus     │
                                │  (via UDS)    │
                                └───────────────┘
```

## Chain Flow

1. **C++ Producer Node** (`chain_cpp_node.cpp`)
   - Generates initial data: `CPP_DATA_<timestamp>`
   - Writes to `user/{dialogue_id}/chain_data`
   - Sets step marker: `chain_data_step = "cpp"`

2. **Python Transformer Node** (`python_transformer.py`)
   - Reads data from C++ node
   - Transforms: `PYTHON_TRANSFORMED(CPP_DATA_xxx)`
   - Updates step marker: `chain_data_step = "python"`

3. **Go Processor Node** (`go_processor.go`)
   - Reads data from Python node
   - Processes: `GO_PROCESSED[PYTHON_TRANSFORMED(...)]`
   - Updates step marker: `chain_data_step = "go"`

4. **Shell Verifier Node** (`shell_verifier.sh`)
   - Reads final data
   - Verifies chain completion
   - Reports verification status

## Files

```
multi_language_databus/
├── agent.json                    # Agent configuration
├── chain_test_workflow.json      # Main workflow definition
├── coordinator_workflow.json     # Coordinator workflow
├── CMakeLists.txt                # Build configuration for SO libraries
├── test.sh                       # Test runner script
├── README.md                     # This file
└── nodes/
    ├── chain_cpp_node.cpp        # C++ producer node
    ├── chain_coordinator_node.cpp # C++ coordinator node
    ├── python_transformer.py     # Python transformer node
    ├── go_processor.go           # Go processor node
    └── shell_verifier.sh         # Shell verifier node
```

## Building

```bash
# Build the project
./build.sh build

# Build the multi-language agent libraries
cd examples/agent/multi_language_databus
./test.sh
```

## Running

```bash
# Run the agent example
cd /path/to/AgentFlow
./build/bin/run_agent_example multi_language_databus
```

## UDS Protocol

All nodes communicate with the DataBus via Unix Domain Socket using JSON-RPC 2.0 protocol.

### Environment Variables

- `AF_DATABUS_UDS_FILE`: Path to the UDS socket
- `AF_SESSION_ID`: Session identifier
- `AF_DIALOGUE_ID`: Dialogue identifier (optional)

### Python SDK Usage

```python
from agentflow_databus import DataBusClient

client = DataBusClient.from_env()
client.user().put("chain_data", "my_value")
value = client.user().get("chain_data")
```

### Go SDK Usage

```go
client, _ := databus.NewClientFromEnv()
client.User().Put("chain_data", "my_value")
value, _ := client.User().Get("chain_data")
```

### Shell/CLI Usage

```bash
databus-cli -s $SESSION_ID -ns user put "chain_data" "my_value"
databus-cli -s $SESSION_ID -ns user get "chain_data"
```

## Expected Output

When the chain completes successfully, the coordinator will output:

```
Multi-Language Chain Test Results:

Session ID: <session_id>
Dialogue ID: <dialogue_id>

Final Data: GO_PROCESSED[PYTHON_TRANSFORMED(CPP_DATA_xxx)]
Last Step: go
Verification: PASS

✓ Chain completed successfully!
  - C++ Producer: ✓
  - Python Transformer: ✓
  - Go Processor: ✓
  - Shell Verifier: ✓
```

## Troubleshooting

### UDS Connection Failed

If nodes cannot connect to the UDS socket:

1. Verify `AF_DATABUS_UDS_FILE` environment variable is set
2. Check that the socket file exists
3. Ensure the worker process is running

### Python Import Error

If `agentflow_databus` cannot be imported:

1. Add the SDK to PYTHONPATH: `export PYTHONPATH=/path/to/AgentFlow/clients/python:$PYTHONPATH`
2. Or use the fallback minimal client included in the script

### Go Execution Failed

If the Go script fails to run:

1. Ensure Go is installed: `go version`
2. The Go node uses environment variables as fallback when UDS is not available

## Related Documentation

- [UDS Protocol Specification](../../../docs/UDS_PROTOCOL.md)
- [Python SDK](../../../clients/python/README.md)
- [Go SDK](../../../clients/golang/README.md)
- [CLI Tool](../../../clients/cli/README.md)