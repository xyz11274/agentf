# AgentFlow DataBus Python SDK

Pure Python SDK for real-time DataBus access via Unix Domain Socket.

## Installation

Copy `agentflow_databus.py` to your project or Python path.

```bash
cp agentflow_databus.py /path/to/your/project/
```

## Quick Start

```python
from agentflow_databus import DataBusClient

# Connect via environment variable (recommended)
client = DataBusClient.from_env()

# Or explicit socket path
client = DataBusClient("/tmp/agentflow_session_xxx.sock")
```

## Chain-Call API

The SDK provides a chain-call API matching the C++ INodeDataBusClient interface:

```python
# Access namespaces
client.sys().put("response", "Hello World")      # Framework communication
client.ctx().put("history", [...])               # Context storage
client.llm().put("prompt", "...")                # LLM interaction
client.tool().put("call_result", {...})          # Tool calls
client.user().put("my_data", {...})              # User-defined data

# Get values
value = client.user().get("my_key", default=None)
exists = client.user().contains("my_key")
client.user().remove("my_key")
```

## Dialogue Isolation

For multi-dialogue sessions, pass the dialogue_id:

```python
# Store data for specific dialogue
client.user("dialogue-123").put("skill_result", {"status": "ok"})

# Retrieve dialogue-specific data
result = client.user("dialogue-123").get("skill_result")
```

## Subscriptions

Subscribe to real-time data changes:

```python
def on_change(key, value):
    print(f"Key {key} changed to {value}")

# Subscribe
sub_id = client.user().subscribe("my_key", on_change)

# Later, unsubscribe
client.user().unsubscribe(sub_id)
```

## Raw Access

For global keys that don't use session isolation:

```python
# Raw access bypasses session prefix
client.raw().put("/global/config", "value")
value = client.raw().get("/global/config")
```

## Context Manager

Use with `with` for automatic cleanup:

```python
with DataBusClient.from_env() as client:
    client.user().put("key", "value")
    value = client.user().get("key")
# Connection automatically closed
```

## Error Handling

```python
from agentflow_databus import DataBusClient, DataBusError, JSONRPCError

try:
    client = DataBusClient.from_env()
    value = client.user().get("missing_key")
except DataBusError as e:
    print(f"Connection error: {e}")
except JSONRPCError as e:
    print(f"Protocol error {e.code}: {e.message}")
```

## Environment Variables

| Variable | Description |
|----------|-------------|
| `AF_DATABUS_UDS_FILE` | Path to the Unix Domain Socket |

## API Reference

### DataBusClient

#### Class Methods

- `from_env()` - Create client using `AF_DATABUS_UDS_FILE` environment variable

#### Methods

- `sys(dialogue_id=None)` - Access sys namespace
- `ctx(dialogue_id=None)` - Access ctx namespace
- `llm(dialogue_id=None)` - Access llm namespace
- `tool(dialogue_id=None)` - Access tool namespace
- `user(dialogue_id=None)` - Access user namespace
- `raw()` - Access raw DataBus (no prefix)
- `close()` - Close the connection

#### Properties

- `session_id` - Current session ID
- `socket_path` - Path to the Unix Domain Socket

### NamespaceClient

#### Methods

- `put(key, value)` - Store a value
- `get(key, default=None)` - Retrieve a value
- `remove(key)` - Delete a key
- `contains(key)` - Check if key exists
- `subscribe(key, callback)` - Subscribe to changes
- `unsubscribe(subscription_id)` - Cancel subscription
- `publish(topic, value)` - Publish an event

## Examples

### Simple Workflow

```python
from agentflow_databus import DataBusClient

def main():
    with DataBusClient.from_env() as client:
        # Store input
        client.user().put("input", "process this data")

        # Read config from framework
        config = client.sys().get("config", {})

        # Store result
        result = {"status": "completed", "data": "processed"}
        client.user().put("output", result)

        # Signal completion
        client.sys().put("response", "Done")

if __name__ == "__main__":
    main()
```

### Reactive Pattern with Subscriptions

```python
import time
from agentflow_databus import DataBusClient

def main():
    with DataBusClient.from_env() as client:

        def on_input(key, value):
            print(f"Received input: {value}")
            # Process and store result
            result = f"Processed: {value}"
            client.user().put("output", result)

        # Subscribe to input changes
        sub_id = client.user().subscribe("input", on_input)

        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            client.user().unsubscribe(sub_id)

if __name__ == "__main__":
    main()
```

## License

MIT License