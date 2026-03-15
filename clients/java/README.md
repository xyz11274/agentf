# AgentFlow DataBus Java Client

Java client for AgentFlow DataBus via Unix Domain Socket.

## Requirements

- Java 17 or higher
- Maven 3.6+ (for building)

## Installation

### Maven

Add to your `pom.xml`:

```xml
<dependency>
    <groupId>com.agentflow</groupId>
    <artifactId>agentflow-databus</artifactId>
    <version>1.0.0</version>
</dependency>
```

### Build from Source

```bash
cd clients/java
mvn clean install
```

## Quick Start

```java
import com.agentflow.databus.DataBusClient;
import com.agentflow.databus.DataBusException;

// Connect via environment variable (AF_DATABUS_UDS_FILE)
try (DataBusClient client = DataBusClient.fromEnv()) {
    // Store a value
    client.user().put("my_key", "my_value");

    // Retrieve a value
    String value = (String) client.user().get("my_key");
    System.out.println(value);  // "my_value"

    // Check existence
    boolean exists = client.user().contains("my_key");

    // Remove a key
    client.user().remove("my_key");
} catch (DataBusException | IOException e) {
    e.printStackTrace();
}
```

## Usage

### Connecting

```java
// Via environment variable
DataBusClient client = DataBusClient.fromEnv();

// Or with explicit socket path
DataBusClient client = new DataBusClient("/tmp/agentflow_session_xxx.sock");

// Always close when done (or use try-with-resources)
client.close();
```

### Namespaces

```java
// sys namespace - Framework communication
client.sys().put("response", "Hello World");
client.sys().put("api_key", "sk-xxx");

// ctx namespace - Context storage
client.ctx().put("history", historyList);
client.ctx().put("state", stateMap);

// llm namespace - LLM service interaction
client.llm().put("last_prompt", "What is AI?");
client.llm().put("last_response", llmResponse);

// tool namespace - Tool call information
client.tool().put("call_result", result);
client.tool().put("tool_list", toolList);

// user namespace - User-defined data
client.user().put("custom_key", customValue);
```

### With Dialogue ID

For multi-dialogue sessions, specify a dialogue ID:

```java
// Access with dialogue isolation
client.user("dialogue-123").put("skill_result", result);
Object result = client.user("dialogue-123").get("skill_result");
```

### Chain-Call Style

The API matches the C++ INodeDataBusClient interface:

```java
// Chain-call style
client.user().put("key", value);
Object val = client.user().get("key");
```

### JSON Values

Store and retrieve complex objects (serialized as JSON):

```java
Map<String, Object> config = new HashMap<>();
config.put("debug", true);
config.put("level", 5);
client.user().put("config", config);

// Retrieve
Object retrieved = client.user().get("config");
```

### Raw Access

Bypass session isolation with raw key access:

```java
// Direct key path manipulation
client.raw().put("/session/abc123/user/my_key", "value");
Object value = client.raw().get("/session/abc123/user/my_key");
```

### Error Handling

```java
try {
    client.user().put("key", "value");
} catch (DataBusException e) {
    System.err.println("Error code: " + e.getCode());
    System.err.println("Message: " + e.getMessage());
}
```

## API Reference

### DataBusClient

| Method | Description |
|--------|-------------|
| `fromEnv()` | Create client from `AF_DATABUS_UDS_FILE` env var |
| `new DataBusClient(path)` | Create client with explicit socket path |
| `close()` | Close the connection |
| `getSessionId()` | Get the session ID |
| `getSocketPath()` | Get the socket path |
| `sys()` | Access sys namespace |
| `ctx()` | Access ctx namespace |
| `llm()` | Access llm namespace |
| `tool()` | Access tool namespace |
| `user()` | Access user namespace |
| `raw()` | Get raw DataBus access |

### NamespaceClient

| Method | Description |
|--------|-------------|
| `put(key, value)` | Store a value |
| `get(key)` | Retrieve a value |
| `get(key, default)` | Retrieve with default |
| `remove(key)` | Remove a key |
| `contains(key)` | Check existence |
| `subscribe(key, callback)` | Subscribe to changes |
| `unsubscribe(subscriptionId)` | Cancel subscription |
| `publish(topic, payload)` | Publish an event |
| `listKeys()` | List all keys |
| `listKeys(prefix)` | List keys with prefix |

## Environment Variables

| Variable | Description |
|----------|-------------|
| `AF_DATABUS_UDS_FILE` | Path to the Unix Domain Socket |

## Thread Safety

`DataBusClient` is thread-safe. Multiple threads can share a single client instance.

## License

MIT License