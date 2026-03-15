# DataBus CLI

Command-line tool for DataBus access via Unix Domain Socket.

## Installation

Copy `databus_cli.py` to your PATH or use directly:

```bash
chmod +x databus_cli.py
./databus_cli.py --help
```

## Quick Start

```bash
# Set value (uses AF_DATABUS_UDS_FILE environment variable)
databus-cli -ns user put "my_key" "my_value"

# Get value
databus-cli -ns user get "my_key"

# With explicit socket and session
databus-cli -S /tmp/agentflow_session_xxx.sock -ns user get "my_key"
```

## Commands

### put

Store a value in the DataBus.

```bash
databus-cli -ns user put "key" "value"
databus-cli -ns user put "config" '{"setting": "value"}'
```

### get

Retrieve a value from the DataBus.

```bash
databus-cli -ns user get "key"
databus-cli -ns user --raw get "key"  # Raw output (no JSON formatting)
```

### remove

Remove a key from the DataBus.

```bash
databus-cli -ns user remove "key"
```

### contains

Check if a key exists.

```bash
databus-cli -ns user contains "key"
# Exit code: 0 if exists, 1 if not found
```

### list

List keys in a namespace (limited functionality).

```bash
databus-cli -ns user list
```

### publish

Publish an event to a topic.

```bash
databus-cli -ns sys publish "topic.event" "payload"
```

### subscribe

Subscribe to key changes (blocking).

```bash
databus-cli -ns user subscribe "key"
# Press Ctrl+C to stop
```

## Options

### Global Options

| Option | Description |
|--------|-------------|
| `-S, --socket` | Unix Domain Socket path (or set `AF_DATABUS_UDS_FILE`) |
| `-s, --session` | Session ID |
| `-d, --dialogue` | Dialogue ID for dialogue-scoped access |
| `-ns, --namespace` | Namespace: sys, ctx, llm, tool, user (default: user) |
| `-q, --quiet` | Suppress non-essential output |
| `--raw` | Output raw values without formatting |

## Examples

### Basic Usage

```bash
# Set a simple value
databus-cli -ns user put "name" "Alice"

# Get the value
databus-cli -ns user get "name"
# Output: "Alice"

# Check existence
databus-cli -ns user contains "name"
# Output: exists: name

# Remove the key
databus-cli -ns user remove "name"
```

### JSON Values

```bash
# Store JSON value
databus-cli -ns user put "config" '{"debug": true, "level": 5}'

# Get JSON (formatted)
databus-cli -ns user get "config"
# Output:
# {
#   "debug": true,
#   "level": 5
# }

# Get raw output
databus-cli -ns user --raw get "config"
# Output: map[debug:true level:5]
```

### Session and Dialogue

```bash
# With explicit session ID
databus-cli -s "session-abc123" -ns user put "data" "value"

# With dialogue ID for multi-dialogue sessions
databus-cli -s "session-abc123" -d "dialogue-456" -ns user put "result" "done"
```

### Namespace Access

```bash
# System namespace (framework communication)
databus-cli -ns sys put "response" "Hello World"

# LLM namespace
databus-cli -ns llm put "prompt" "What is AI?"

# Tool namespace
databus-cli -ns tool put "call_result" '{"status": "success"}'
```

### Subscriptions

```bash
# Subscribe to key changes
databus-cli -ns user subscribe "status"
# Output:
# Subscribed to status (subscription_id: 1)
# Press Ctrl+C to stop...
# [2024-01-15 10:30:00] status: "processing"
# [2024-01-15 10:30:05] status: "completed"
```

### Scripting

Use in shell scripts:

```bash
#!/bin/bash

# Store value
databus-cli -ns user put "input" "process this"

# Wait for result
while true; do
    status=$(databus-cli -ns user --raw get "status" 2>/dev/null)
    if [ "$status" = "completed" ]; then
        break
    fi
    sleep 1
done

# Get result
result=$(databus-cli -ns user --raw get "output")
echo "Result: $result"
```

### Error Handling

Check exit codes in scripts:

```bash
# Check if key exists
if databus-cli -ns user contains "config" 2>/dev/null; then
    echo "Config exists"
else
    echo "Config not found"
fi

# Handle get failure
if ! databus-cli -ns user get "missing_key" 2>/dev/null; then
    echo "Key not found, using default"
fi
```

## Environment Variables

| Variable | Description |
|----------|-------------|
| `AF_DATABUS_UDS_FILE` | Path to the Unix Domain Socket |

## Exit Codes

| Code | Description |
|------|-------------|
| 0 | Success |
| 1 | Error (key not found, operation failed) |

## License

MIT License