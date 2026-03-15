# Multi-Language DataBus Access via Unix Domain Socket

## Overview

This document describes the design and implementation of multi-language DataBus access via Unix Domain Socket (UDS). This feature enables external processes (Python, Go, Shell SDKs) to access the AgentFlow DataBus in real-time with session/dialogue isolation.

## Motivation

The existing file-based DataBus access (via `AF_DATABUS_DIR`) works but has limitations:

1. **File I/O overhead** - Each operation requires file system access
2. **No real-time subscriptions** - Polling is required for change detection
3. **No session isolation** - External processes must manually construct key paths
4. **Different API** - File-based API differs from C++ node's chain-call pattern

The UDS-based approach addresses these limitations by providing:

1. **Real-time access** - Direct socket connection for immediate read/write
2. **Subscription support** - Push notifications when data changes
3. **Session isolation** - Automatic session/dialogue namespacing
4. **Chain-call API** - Matches C++ INodeDataBusClient interface

## Architecture

```
Worker Process
├── MMWorkerEngine
│   ├── MMDataBus* (existing)
│   └── UDS Server Thread (new)
│       └── Unix Domain Socket: /tmp/agentflow_{session_id}.sock
│           └── JSON-RPC 2.0 Protocol
│
External Process (Python/Go/Shell/Other)
└── Client SDK
    └── Connect to UDS via AF_DATABUS_UDS_FILE env var
```

### Key Components

1. **UDSServer** (`include/internal/runtime/databus/uds_server.h`)
   - Runs in a separate thread within the worker
   - Listens on a Unix Domain Socket
   - Implements JSON-RPC 2.0 protocol
   - Forwards requests to MMDataBus

2. **Worker Integration** (`src/runtime/worker/worker.cpp`)
   - Creates UDS server when `databus` and `session_id` are provided
   - Injects `AF_DATABUS_UDS_FILE` environment variable
   - Cleans up server after execution

3. **Client SDKs**
   - Python: `clients/python/agentflow_databus.py`
   - Go: `clients/golang/databus/client.go`
   - CLI: `clients/cli/databus_cli.py`

## Protocol

### JSON-RPC 2.0 Format

All requests and responses use JSON-RPC 2.0 format:

```json
// Request
{"jsonrpc": "2.0", "method": "put", "params": {...}, "id": 1}

// Response (success)
{"jsonrpc": "2.0", "result": {"success": true}, "id": 1}

// Response (error)
{"jsonrpc": "2.0", "error": {"code": -1, "message": "Key not found"}, "id": 1}

// Notification (for subscriptions)
{"jsonrpc": "2.0", "method": "data_changed", "params": {"key": "...", "value": ...}}
```

### Methods

| Method | Params | Result | Description |
|--------|--------|--------|-------------|
| `put` | `{session_id, namespace, key, value}` | `{success}` | Store value |
| `get` | `{session_id, namespace, key}` | `{exists, value}` | Retrieve value |
| `remove` | `{session_id, namespace, key}` | `{success}` | Delete key |
| `contains` | `{session_id, namespace, key}` | `{exists}` | Check existence |
| `subscribe` | `{session_id, namespace, key}` | `{subscription_id}` | Subscribe to changes |
| `unsubscribe` | `{subscription_id}` | `{success}` | Cancel subscription |
| `publish` | `{session_id, namespace, topic, value}` | `{success}` | Publish event |
| `list_keys` | `{session_id, namespace, prefix?}` | `{keys}` | List keys |

### Namespace Values

| Namespace | Description |
|-----------|-------------|
| `sys` | Framework communication (response, api_key, config) |
| `ctx` | Context storage (history, state) |
| `llm` | LLM service interaction (last_prompt, last_response) |
| `tool` | Tool call information (call_result, tool_list) |
| `user` | User-defined node data (custom keys) |

### Key Structure

Keys are built as: `/session/{session_id}/{namespace}/{key}`

For example:
- Session: `abc123`
- Namespace: `user`
- Key: `my_data`

Full key: `/session/abc123/user/my_data`

## Implementation Details

### UDSServer Class

```cpp
class UDSServer {
public:
  UDSServer(MMDataBus* databus, const std::string& socket_path);
  ~UDSServer();

  bool start();
  void stop();
  bool is_running() const;
  const std::string& socket_path() const;

private:
  void server_loop();
  void handle_client(int client_fd);
  std::string handle_request(const std::string& request_json);
  std::string build_key(const std::string& session_id,
                        const std::string& namespace_str,
                        const std::string& key) const;

  MMDataBus* databus_;
  std::string socket_path_;
  std::atomic<bool> running_{false};
  std::thread server_thread_;
  std::map<uint64_t, SubscriptionInfo> subscriptions_;
};
```

### Thread Safety

- UDS server runs in a separate thread
- MMDataBus is already thread-safe
- Subscription map protected by mutex
- Client connections handled sequentially (single-threaded for simplicity)

### Socket Lifecycle

1. Worker creates UDS server with unique socket path
2. Socket path injected as `AF_DATABUS_UDS_FILE` environment variable
3. External process connects via client SDK
4. Worker stops UDS server after execution completes
5. Socket file cleaned up automatically

### Error Handling

- Connection failures: SDK raises exceptions
- JSON-RPC errors: SDK wraps in Error objects
- Server crashes: Socket file cleaned up in destructor
- Client disconnects: Subscriptions automatically cleaned up

## Client SDK Design

### Chain-Call Pattern

The SDKs provide a chain-call API matching C++ INodeDataBusClient:

```python
# Python
client.user().put("key", "value")
client.user("dialogue-123").put("key", "value")
```

```go
// Go
client.User().Put("key", "value")
client.UserWithDialogue("dialogue-123").Put("key", "value")
```

This matches the C++ pattern:

```cpp
// C++
client->user()->put("key", SoDataVariant(std::string("value")));
```

### Namespace Client

Each namespace returns a NamespaceClient that:

1. Stores reference to parent client
2. Stores namespace name
3. Optionally stores dialogue_id
4. Automatically builds full keys on each operation

### Subscription Handling

Subscriptions work by:

1. Client sends `subscribe` request
2. Server registers subscription with unique ID
3. Server subscribes to MMDataBus
4. On data change, server sends notification to client
5. Client invokes callback function

## Performance Considerations

### Latency

- UDS has lower latency than file I/O
- No polling required for subscriptions
- Single syscall for most operations

### Throughput

- Single-threaded client handling limits throughput
- For high throughput, consider thread-per-client model
- Large data transfers should use chunked or file-based approach

### Memory

- Minimal memory overhead
- Subscription data structures are small
- No data copying (references passed through)

## Future Enhancements

### Large Data Transfer

For payloads > 1MB, consider:
1. Chunked transfer over UDS
2. File-based transfer with FD passing
3. Shared memory for zero-copy

### Connection Pooling

For high-frequency operations:
1. Keep connections alive between executions
2. Use connection pool pattern

### Binary Protocol

For performance-critical applications:
1. Binary protocol (e.g., MessagePack, Protocol Buffers)
2. More compact than JSON
3. Faster serialization/deserialization

## Migration Guide

### From File-based to UDS

1. Replace `AF_DATABUS_DIR` with `AF_DATABUS_UDS_FILE`
2. Replace `FileDataBus` class with `DataBusClient`
3. Use chain-call API instead of file operations

```python
# Old (file-based)
from agentflow_databus import FileDataBus
bus = FileDataBus()
bus.set("key", "value")

# New (UDS-based)
from agentflow_databus import DataBusClient
client = DataBusClient.from_env()
client.user().put("key", "value")
```

## Testing

### Unit Tests

Test UDS server request handling:
- JSON-RPC parsing
- Method dispatching
- Error handling

### Integration Tests

Test SDK clients against UDS server:
- Connection establishment
- All operations
- Subscription notifications

### End-to-End Tests

Test cross-language interoperability:
- C++ node writes data
- Python client reads data
- Go client writes data
- Verify all visible

## Conclusion

The UDS-based DataBus access provides a modern, efficient, and language-agnostic way for external processes to interact with AgentFlow's DataBus. The chain-call API matches the C++ interface, making it intuitive for developers familiar with AgentFlow's node development.