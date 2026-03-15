# AgentFlow Multi-Language DataBus Clients

This directory contains client SDKs for accessing the AgentFlow DataBus from multiple programming languages via Unix Domain Socket (UDS).

## Overview

The UDS-based DataBus access provides:

- **Real-time access** - Direct socket connection for immediate read/write operations
- **Subscription support** - Real-time notifications when data changes
- **Session isolation** - Automatic session/dialogue namespacing
- **Chain-call API** - Matches C++ INodeDataBusClient interface

## Available SDKs

| SDK | Directory | Description |
|-----|-----------|-------------|
| Python | `python/` | Pure Python SDK with chain-call API |
| Go | `golang/` | Go module with chain-call API |
| CLI | `cli/` | Command-line tool for shell access |

## Quick Start

### Environment Variable

All SDKs support the `AF_DATABUS_UDS_FILE` environment variable, which is automatically set by the AgentFlow worker when executing external processes.

```bash
export AF_DATABUS_UDS_FILE=/tmp/agentflow_session_abc123.sock
```

### Python

```python
from agentflow_databus import DataBusClient

# Connect via environment variable
client = DataBusClient.from_env()

# Chain-call style
client.user().put("my_key", "value")
value = client.user().get("my_key")

# With dialogue_id
client.user("dialogue-123").put("result", {"status": "ok"})
```

### Go

```go
import "github.com/agentflow/clients/golang/databus"

// Connect via environment variable
client, err := databus.NewClientFromEnv()

// Chain-call style
client.User().Put("my_key", "value")
value, _ := client.User().Get("my_key")

// With dialogue_id
client.UserWithDialogue("dialogue-123").Put("result", map[string]any{"status": "ok"})
```

### CLI

```bash
# Set value
databus-cli -s {session-id} -ns user put "my_key" "my_value"

# Get value
databus-cli -s {session-id} -ns user get "my_key"

# Subscribe to changes
databus-cli -s {session-id} -ns user subscribe "my_key"
```

## Protocol

The UDS server uses JSON-RPC 2.0 protocol:

```json
// Request
{"jsonrpc": "2.0", "method": "put", "params": {...}, "id": 1}

// Response
{"jsonrpc": "2.0", "result": {"success": true}, "id": 1}
```

### Methods

| Method | Description |
|--------|-------------|
| `put` | Store a value |
| `get` | Retrieve a value |
| `remove` | Delete a key |
| `contains` | Check existence |
| `subscribe` | Subscribe to changes |
| `unsubscribe` | Cancel subscription |
| `publish` | Publish event |
| `list_keys` | List keys |

### Namespaces

| Namespace | Purpose |
|-----------|---------|
| `sys` | Framework communication (response, config) |
| `ctx` | Context storage (history, state) |
| `llm` | LLM service interaction |
| `tool` | Tool call information |
| `user` | User-defined data |

## Key Structure

Keys are automatically prefixed with session and namespace:

```
/session/{session_id}/{namespace}/{key}
```

For dialogue-scoped access:

```
/session/{session_id}/{namespace}/{dialogue_id}/{key}
```

## Architecture

```
Worker Process
├── MMWorkerEngine
│   ├── MMDataBus* (existing)
│   └── UDS Server Thread
│       └── Unix Domain Socket: /tmp/agentflow_worker_{worker_id}.sock
│           └── JSON-RPC 2.0 Protocol
│
External Process (Python/Go/Shell)
└── Client SDK
    └── Connect to UDS via AF_DATABUS_UDS_FILE env var
```

## Comparison with File-based Access

| Feature | File-based (Old) | UDS-based (New) |
|---------|-----------------|-----------------|
| Latency | File I/O overhead | Direct socket |
| Real-time | Polling required | Push notifications |
| Subscriptions | Not supported | Fully supported |
| Session isolation | Manual | Automatic |
| API style | File operations | Chain-call |

## License

MIT License - See LICENSE file in the project root.