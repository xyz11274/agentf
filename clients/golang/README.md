# AgentFlow DataBus Go SDK

Go module for real-time DataBus access via Unix Domain Socket.

## Installation

```bash
go get github.com/agentflow/clients/golang/databus
```

Or copy the `databus/` directory to your project.

## Quick Start

```go
package main

import (
    "fmt"
    "log"

    "github.com/agentflow/clients/golang/databus"
)

func main() {
    // Connect via environment variable (recommended)
    client, err := databus.NewClientFromEnv()
    if err != nil {
        log.Fatal(err)
    }
    defer client.Close()

    // Chain-call style
    client.User().Put("my_key", "value")
    value, _ := client.User().Get("my_key")
    fmt.Printf("Got value: %v\n", value)
}
```

## Chain-Call API

The SDK provides a chain-call API matching the C++ INodeDataBusClient interface:

```go
// Access namespaces
client.Sys().Put("response", "Hello World")      // Framework communication
client.Ctx().Put("history", []string{...})       // Context storage
client.LLM().Put("prompt", "...")                // LLM interaction
client.Tool().Put("call_result", map[string]any{...})  // Tool calls
client.User().Put("my_data", map[string]any{...})      // User-defined data

// Get values
value, err := client.User().Get("my_key")
strValue, err := client.User().GetString("my_key")
intValue, err := client.User().GetInt("my_key")
floatValue, err := client.User().GetFloat("my_key")
boolValue, err := client.User().GetBool("my_key")

// Other operations
exists, err := client.User().Contains("my_key")
err = client.User().Remove("my_key")
```

## Dialogue Isolation

For multi-dialogue sessions, use the `*WithDialogue` methods:

```go
// Store data for specific dialogue
client.UserWithDialogue("dialogue-123").Put("skill_result", map[string]any{
    "status": "ok",
})

// Retrieve dialogue-specific data
result, err := client.UserWithDialogue("dialogue-123").Get("skill_result")
```

## Subscriptions

Subscribe to real-time data changes:

```go
// Subscribe
subID, err := client.User().Subscribe("my_key", func(key string, value any) {
    fmt.Printf("Key %s changed to %v\n", key, value)
})

// Later, unsubscribe
client.User().Unsubscribe(subID)
```

## Raw Access

For global keys that don't use session isolation:

```go
// Raw access bypasses session prefix
client.Raw().Put("/global/config", "value")
value, err := client.Raw().Get("/global/config")
```

## Error Handling

```go
import (
    "errors"
    "fmt"

    "github.com/agentflow/clients/golang/databus"
)

func main() {
    client, err := databus.NewClientFromEnv()
    if err != nil {
        if errors.Is(err, databus.ErrNotConnected) {
            fmt.Println("Not connected to DataBus")
        }
        log.Fatal(err)
    }
    defer client.Close()

    value, err := client.User().Get("missing_key")
    if err != nil {
        var rpcErr *databus.Error
        if errors.As(err, &rpcErr) {
            fmt.Printf("JSON-RPC Error %d: %s\n", rpcErr.Code, rpcErr.Message)
        }
        return
    }
}
```

## Environment Variables

| Variable | Description |
|----------|-------------|
| `AF_DATABUS_UDS_FILE` | Path to the Unix Domain Socket |

## API Reference

### Client

#### Functions

- `NewClient(socketPath string) (*Client, error)` - Create client with explicit socket path
- `NewClientFromEnv() (*Client, error)` - Create client using `AF_DATABUS_UDS_FILE`

#### Methods

- `Sys() *NamespaceClient` - Access sys namespace
- `Ctx() *NamespaceClient` - Access ctx namespace
- `LLM() *NamespaceClient` - Access llm namespace
- `Tool() *NamespaceClient` - Access tool namespace
- `User() *NamespaceClient` - Access user namespace
- `SysWithDialogue(dialogueID string) *NamespaceClient` - Sys with dialogue ID
- `CtxWithDialogue(dialogueID string) *NamespaceClient` - Ctx with dialogue ID
- `LLMWithDialogue(dialogueID string) *NamespaceClient` - LLM with dialogue ID
- `ToolWithDialogue(dialogueID string) *NamespaceClient` - Tool with dialogue ID
- `UserWithDialogue(dialogueID string) *NamespaceClient` - User with dialogue ID
- `Raw() *RawClient` - Access raw DataBus (no prefix)
- `Close() error` - Close the connection

#### Properties

- `SessionID() string` - Current session ID
- `SocketPath() string` - Path to the Unix Domain Socket

### NamespaceClient

#### Methods

- `Put(key string, value any) error` - Store a value
- `Get(key string) (any, error)` - Retrieve a value
- `GetString(key string) (string, error)` - Get as string
- `GetInt(key string) (int64, error)` - Get as int64
- `GetFloat(key string) (float64, error)` - Get as float64
- `GetBool(key string) (bool, error)` - Get as bool
- `Remove(key string) error` - Delete a key
- `Contains(key string) (bool, error)` - Check if key exists
- `Subscribe(key string, callback func(string, any)) (int64, error)` - Subscribe to changes
- `Unsubscribe(subscriptionID int64) error` - Cancel subscription
- `Publish(topic string, value any) error` - Publish an event

### RawClient

#### Methods

- `Put(key string, value any) error` - Store a value (no prefix)
- `Get(key string) (any, error)` - Retrieve a value (no prefix)
- `Contains(key string) (bool, error)` - Check existence (no prefix)
- `Remove(key string) error` - Delete a key (no prefix)

## Examples

### Simple Workflow

```go
package main

import (
    "fmt"
    "log"

    "github.com/agentflow/clients/golang/databus"
)

func main() {
    client, err := databus.NewClientFromEnv()
    if err != nil {
        log.Fatal(err)
    }
    defer client.Close()

    // Store input
    client.User().Put("input", "process this data")

    // Read config from framework
    config, _ := client.Sys().Get("config")
    fmt.Printf("Config: %v\n", config)

    // Store result
    result := map[string]any{
        "status": "completed",
        "data":   "processed",
    }
    client.User().Put("output", result)

    // Signal completion
    client.Sys().Put("response", "Done")
}
```

## License

MIT License