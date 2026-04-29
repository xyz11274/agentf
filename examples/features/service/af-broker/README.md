# af-broker

Go client adapter for agentflowd JSON-RPC 2.0 daemon.

## Usage

```bash
# Build
go build -o af-broker ./cmd/af-broker/

# Health check
./af-broker localhost:7890 health

# Load an agent
./af-broker localhost:7890 load /path/to/agent/package

# Execute a request
./af-broker localhost:7890 execute basic-agent "hello world"

# Poll for responses
./af-broker localhost:7890 poll basic-agent 5000

# Subscribe to events
./af-broker localhost:7890 subscribe task,workflow

# Get metrics
./af-broker localhost:7890 metrics json
./af-broker localhost:7890 metrics prometheus

# Shutdown daemon
./af-broker localhost:7890 shutdown
```

## As a Library

```go
import "github.com/agentflow/af-broker"

client := afbroker.NewClient("localhost:7890", afbroker.ChannelTCP)
client.Connect(context.Background())
defer client.Close()

// Subscribe to notifications
client.SetNotificationHandler(func(n *afbroker.Notification) {
    fmt.Printf("Event: %s\n", n.Method)
})

// Execute request
msgID, err := client.AgentExecute(ctx, "my-agent", "user1", "client1", "hello")

// Poll response
resp, err := client.AgentPollResponse(ctx, "my-agent", 5000)
```
