# AgentFlow DataBus UDS Protocol Specification

This document provides a complete interface description for implementing DataBus clients that communicate with the AgentFlow Unix Domain Socket (UDS) server.

## Overview

The UDS server provides real-time DataBus access for external processes (Python, Go, Java, Shell, etc.) using JSON-RPC 2.0 protocol over Unix Domain Sockets.

## Connection

### Socket Path

The socket path is provided via the `AF_DATABUS_UDS_FILE` environment variable:

```
AF_DATABUS_UDS_FILE=/tmp/agentflow_{session_id}.sock
```

### Connection Protocol

1. Create a Unix Domain Socket (`AF_UNIX`, `SOCK_STREAM`)
2. Connect to the path specified in `AF_DATABUS_UDS_FILE`
3. Send JSON-RPC requests terminated by newline (`\n`)
4. Receive JSON-RPC responses terminated by newline (`\n`)

### Socket Lifecycle

- The server creates the socket when a node execution begins
- The socket is removed when the node execution completes
- Clients should handle `ECONNREFUSED` gracefully (server may not be ready)

## JSON-RPC 2.0 Protocol

All messages follow the JSON-RPC 2.0 specification.

### Request Format

```json
{
  "jsonrpc": "2.0",
  "method": "<method_name>",
  "params": { ... },
  "id": <integer>
}
```

### Response Format (Success)

```json
{
  "jsonrpc": "2.0",
  "result": { ... },
  "id": <integer>
}
```

### Response Format (Error)

```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": <integer>,
    "message": "<error_message>"
  },
  "id": <integer>
}
```

### Notification Format (Server-initiated)

```json
{
  "jsonrpc": "2.0",
  "method": "<notification_type>",
  "params": { ... }
}
```

## Methods

### `put` - Store a Value

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "put",
  "params": {
    "session_id": "<session_id>",
    "namespace": "<namespace>",
    "key": "<key>",
    "value": <value>
  },
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true
  },
  "id": 1
}
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `session_id` | string | Yes | Session identifier (from environment or extracted from socket path) |
| `namespace` | string | Yes | Namespace: `sys`, `ctx`, `llm`, `tool`, or `user` |
| `key` | string | Yes | Key name within the namespace |
| `value` | any | Yes | Value to store (JSON-serializable) |

**Error Codes:**

| Code | Message |
|------|---------|
| -32602 | Invalid params: missing session_id, namespace, or key |
| -1 | Failed to put value |

---

### `get` - Retrieve a Value

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "get",
  "params": {
    "session_id": "<session_id>",
    "namespace": "<namespace>",
    "key": "<key>"
  },
  "id": 2
}
```

**Response (exists):**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "exists": true,
    "value": <value>
  },
  "id": 2
}
```

**Response (not found):**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "exists": false,
    "value": null
  },
  "id": 2
}
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `session_id` | string | Yes | Session identifier |
| `namespace` | string | Yes | Namespace |
| `key` | string | Yes | Key name |

---

### `remove` - Delete a Key

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "remove",
  "params": {
    "session_id": "<session_id>",
    "namespace": "<namespace>",
    "key": "<key>"
  },
  "id": 3
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true
  },
  "id": 3
}
```

---

### `contains` - Check Key Existence

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "contains",
  "params": {
    "session_id": "<session_id>",
    "namespace": "<namespace>",
    "key": "<key>"
  },
  "id": 4
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "exists": true
  },
  "id": 4
}
```

---

### `subscribe` - Subscribe to Key Changes

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "subscribe",
  "params": {
    "session_id": "<session_id>",
    "namespace": "<namespace>",
    "key": "<key>"
  },
  "id": 5
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "subscription_id": 1
  },
  "id": 5
}
```

**Notification (when value changes):**
```json
{
  "jsonrpc": "2.0",
  "method": "data_changed",
  "params": {
    "subscription_id": 1,
    "key": "/session/<session_id>/<namespace>/<key>",
    "value": <new_value>
  }
}
```

**Notes:**
- Notifications are sent asynchronously when the subscribed key's value changes
- The connection must remain open to receive notifications
- Multiple subscriptions can be active on a single connection

---

### `unsubscribe` - Cancel Subscription

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "unsubscribe",
  "params": {
    "subscription_id": 1
  },
  "id": 6
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true
  },
  "id": 6
}
```

---

### `publish` - Publish an Event

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "publish",
  "params": {
    "session_id": "<session_id>",
    "namespace": "<namespace>",
    "topic": "<topic>",
    "value": <value>
  },
  "id": 7
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true
  },
  "id": 7
}
```

---

### `list_keys` - List Keys in Namespace

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "list_keys",
  "params": {
    "session_id": "<session_id>",
    "namespace": "<namespace>",
    "prefix": "<optional_prefix>"
  },
  "id": 8
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "keys": ["key1", "key2", "key3"]
  },
  "id": 8
}
```

## Key Structure

Keys are automatically built from the parameters:

```
/session/{session_id}/{namespace}/{key}
```

### Example Key Transformations

| session_id | namespace | key | Full Key |
|------------|-----------|-----|----------|
| `sess-123` | `user` | `result` | `/session/sess-123/user/result` |
| `sess-123` | `sys` | `response` | `/session/sess-123/sys/response` |
| `sess-123` | `user` | `dialogue-456/skill_result` | `/session/sess-123/user/dialogue-456/skill_result` |

### Dialogue-Scoped Keys

For multi-dialogue support, include `dialogue_id` in the key:

```
{dialogue_id}/{key}
```

Example: `dialogue-123/travel_result`

## Namespaces

| Namespace | Purpose | Typical Usage |
|-----------|---------|---------------|
| `sys` | Framework communication | `response`, `next_skills`, `pending_skills_count` |
| `ctx` | Context storage | `history`, `state`, `config` |
| `llm` | LLM service | `prompt`, `response`, `tokens` |
| `tool` | Tool calls | `tool_name`, `tool_args`, `tool_result` |
| `user` | User-defined data | Skill results, custom data |

## Error Codes

| Code | Message | Description |
|------|---------|-------------|
| -32700 | Parse error | Invalid JSON |
| -32600 | Invalid Request | Missing `jsonrpc`, `method`, or `id` |
| -32601 | Method not found | Unknown method |
| -32602 | Invalid params | Missing or invalid parameters |
| -32603 | Internal error | Server-side error |
| -1 | Operation failed | General failure |
| -2 | Subscription not found | Invalid subscription_id |

## Implementation Examples

### Minimal Python Client

```python
import json
import socket
import os

class MinimalDataBusClient:
    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(os.environ["AF_DATABUS_UDS_FILE"])
        self.req_id = 0

    def call(self, method, params):
        self.req_id += 1
        request = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params,
            "id": self.req_id
        }
        self.sock.sendall((json.dumps(request) + "\n").encode())
        response = json.loads(self.sock.recv(4096).decode().strip())
        if "error" in response:
            raise Exception(response["error"]["message"])
        return response["result"]

    def get(self, namespace, key):
        result = self.call("get", {
            "session_id": "",  # Extracted from socket path
            "namespace": namespace,
            "key": key
        })
        return result.get("value") if result.get("exists") else None

    def put(self, namespace, key, value):
        return self.call("put", {
            "session_id": "",
            "namespace": namespace,
            "key": key,
            "value": value
        })

    def close(self):
        self.sock.close()
```

### Minimal Go Client

```go
package main

import (
    "encoding/json"
    "net"
    "os"
)

type Request struct {
    JSONRPC string         `json:"jsonrpc"`
    Method  string         `json:"method"`
    Params  map[string]any `json:"params"`
    ID      int            `json:"id"`
}

type Response struct {
    JSONRPC string `json:"jsonrpc"`
    Result  any    `json:"result"`
    Error   *struct {
        Code    int    `json:"code"`
        Message string `json:"message"`
    } `json:"error"`
    ID int `json:"id"`
}

type Client struct {
    conn   net.Conn
    reqID  int
}

func NewClient() (*Client, error) {
    conn, err := net.Dial("unix", os.Getenv("AF_DATABUS_UDS_FILE"))
    if err != nil {
        return nil, err
    }
    return &Client{conn: conn}, nil
}

func (c *Client) Call(method string, params map[string]any) (any, error) {
    c.reqID++
    req := Request{
        JSONRPC: "2.0",
        Method:  method,
        Params:  params,
        ID:      c.reqID,
    }
    data, _ := json.Marshal(req)
    c.conn.Write(append(data, '\n'))

    buf := make([]byte, 4096)
    n, _ := c.conn.Read(buf)

    var resp Response
    json.Unmarshal(buf[:n], &resp)

    if resp.Error != nil {
        return nil, fmt.Errorf("error %d: %s", resp.Error.Code, resp.Error.Message)
    }
    return resp.Result, nil
}

func (c *Client) Close() {
    c.conn.Close()
}
```

## Best Practices

1. **Connection Reuse**: Keep the connection open for multiple operations
2. **Error Handling**: Always check for `error` in responses
3. **Session ID**: Extract from socket path or store from first operation
4. **Subscription Cleanup**: Unsubscribe before closing connection
5. **Timeout**: Set socket read/write timeouts for robustness
6. **Reconnection**: Handle connection drops gracefully

## Security Considerations

1. **File Permissions**: Socket file is created with user-only permissions
2. **Local Access Only**: Unix Domain Sockets are local to the machine
3. **Session Isolation**: Keys are automatically prefixed with session_id
4. **Cleanup**: Socket file is removed when the worker process exits

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial protocol specification |