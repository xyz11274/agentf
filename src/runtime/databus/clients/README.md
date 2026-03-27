# AgentFlow 多语言 DataBus 客户端

本目录包含用于通过 Unix 域套接字（UDS）从多种编程语言访问 AgentFlow DataBus 的客户端 SDK。

## 概述

基于 UDS 的 DataBus 访问提供：

- **实时访问** - 通过直接套接字连接实现即时读写操作
- **订阅支持** - 数据变更时实时推送通知
- **会话隔离** - 自动的 session/dialogue 命名空间管理
- **链式调用 API** - 与 C++ INodeDataBusClient 接口保持一致

## 可用 SDK

| SDK | 目录 | 说明 |
|-----|------|------|
| Python | `python/` | 纯 Python SDK，支持链式调用 |
| Go | `golang/` | Go 模块，支持链式调用 |
| Java | `java/` | Java 17+ SDK，支持链式调用 |
| CLI | `cli/` | 命令行工具，适用于 Shell 脚本 |

## 快速上手

### 环境变量

所有 SDK 均支持 `AF_DATABUS_UDS_FILE` 环境变量，该变量由 AgentFlow Worker 在执行外部进程时自动设置。

```bash
export AF_DATABUS_UDS_FILE=/tmp/agentflow_session_abc123.sock
```

### Python

```python
from agentflow_databus import DataBusClient

# 通过环境变量连接
client = DataBusClient.from_env()

# 链式调用风格
client.user().put("my_key", "value")
value = client.user().get("my_key")

# 指定 dialogue_id
client.user("dialogue-123").put("result", {"status": "ok"})
```

### Go

```go
import "github.com/agentflow/databus/clients/golang/databus"

// 通过环境变量连接
client, err := databus.NewClientFromEnv()

// 链式调用风格
client.User().Put("my_key", "value")
value, _ := client.User().Get("my_key")

// 指定 dialogue_id
client.UserWithDialogue("dialogue-123").Put("result", map[string]any{"status": "ok"})
```

### Java

```java
import com.agentflow.databus.DataBusClient;

// 通过环境变量连接
try (DataBusClient client = DataBusClient.fromEnv()) {
    // 链式调用风格
    client.user().put("my_key", "value");
    String value = (String) client.user().get("my_key");

    // 指定 dialogue_id
    client.user("dialogue-123").put("result", Map.of("status", "ok"));
}
```

### CLI

```bash
# 写入值
databus-cli -s {session-id} -ns user put "my_key" "my_value"

# 读取值
databus-cli -s {session-id} -ns user get "my_key"

# 订阅变更
databus-cli -s {session-id} -ns user subscribe "my_key"
```

## 通信协议

UDS 服务端使用 JSON-RPC 2.0 协议：

```json
// 请求
{"jsonrpc": "2.0", "method": "put", "params": {...}, "id": 1}

// 响应
{"jsonrpc": "2.0", "result": {"success": true}, "id": 1}
```

### 方法列表

| 方法 | 说明 |
|------|------|
| `put` | 存储值 |
| `get` | 读取值 |
| `remove` | 删除键 |
| `contains` | 检查键是否存在 |
| `subscribe` | 订阅数据变更 |
| `unsubscribe` | 取消订阅 |
| `publish` | 发布事件 |
| `list_keys` | 列出键名 |
| `async_callback` | 异步回调（多语言节点） |

### 命名空间

| 命名空间 | 用途 |
|---------|------|
| `sys` | 框架通信（响应、配置） |
| `ctx` | 上下文存储（历史记录、状态） |
| `llm` | LLM 服务交互 |
| `tool` | 工具调用信息 |
| `user` | 用户自定义数据 |

## 键名结构

键名自动添加 session 和命名空间前缀：

```
/session/{session_id}/{namespace}/{key}
```

带 dialogue_id 的访问：

```
/session/{session_id}/{namespace}/{dialogue_id}/{key}
```

## 架构

```
Worker 进程
├── MMWorkerEngine
│   ├── MMDataBus*（现有 C++ DataBus）
│   └── UDS Server 线程
│       └── Unix 域套接字：/tmp/agentflow_worker_{worker_id}.sock
│           └── JSON-RPC 2.0 协议
│
外部进程（Python/Go/Java/Shell）
└── 客户端 SDK
    └── 通过 AF_DATABUS_UDS_FILE 连接 UDS
```

## 与文件模式的对比

| 特性 | 文件模式（旧） | UDS 模式（新） |
|------|--------------|--------------|
| 延迟 | 文件 I/O 开销 | 直接套接字 |
| 实时性 | 需要轮询 | 推送通知 |
| 订阅 | 不支持 | 完整支持 |
| 会话隔离 | 手动管理 | 自动管理 |
| API 风格 | 文件操作 | 链式调用 |

## 许可证

MIT License


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
| Java | `java/` | Java 17+ SDK with chain-call API |
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
import "github.com/agentflow/databus/clients/golang/databus"

// Connect via environment variable
client, err := databus.NewClientFromEnv()

// Chain-call style
client.User().Put("my_key", "value")
value, _ := client.User().Get("my_key")

// With dialogue_id
client.UserWithDialogue("dialogue-123").Put("result", map[string]any{"status": "ok"})
```

### Java

```java
import com.agentflow.databus.DataBusClient;

// Connect via environment variable
try (DataBusClient client = DataBusClient.fromEnv()) {
    // Chain-call style
    client.user().put("my_key", "value");
    String value = (String) client.user().get("my_key");

    // With dialogue_id
    client.user("dialogue-123").put("result", Map.of("status", "ok"));
}
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
| `async_callback` | Async callback for multi-language nodes |

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
External Process (Python/Go/Java/Shell)
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