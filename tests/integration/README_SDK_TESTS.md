# SDK UDS Protocol Integration Tests

## 测试目标

验证多语言 SDK（Python/Go/Java/CLI）发送的 JSON-RPC 消息格式符合 UDS 接口设计规范。

## 测试方法

```
┌─────────────────┐      JSON-RPC      ┌─────────────────┐
│   SDK Client    │ ─────────────────► │  Mock UDS Server│
│  (Python/Go/Java)│                    │  (Captures msgs)│
└─────────────────┘                    └─────────────────┘
                                                │
                                                ▼
                                        ┌─────────────────┐
                                        │ Request Capture │
                                        │   & Validation  │
                                        └─────────────────┘
```

### 测试流程

1. **启动 Mock UDS Server**
   - 监听 Unix Domain Socket
   - 捕获所有收到的 JSON-RPC 请求
   - 返回标准响应

2. **调用 SDK 接口**
   - Python SDK: `client.user().put("key", "value")`
   - Go SDK: `client.User().Put("key", "value")`
   - Java SDK: `client.user().put("key", "value")`
   - CLI: `databus-cli -ns user put "key" "value"`

3. **验证捕获的消息**
   - JSON-RPC 2.0 格式正确性
   - 方法名正确性
   - 参数完整性和正确性

## 验证项

### JSON-RPC 2.0 格式验证

| 字段 | 要求 | 示例 |
|------|------|------|
| `jsonrpc` | 必须为 "2.0" | `"jsonrpc": "2.0"` |
| `method` | 必须存在 | `"method": "put"` |
| `params` | 必须存在，为对象 | `"params": {...}` |
| `id` | 必须存在（请求） | `"id": 1` |

### UDS 方法参数验证

#### put 方法

```json
{
  "jsonrpc": "2.0",
  "method": "put",
  "params": {
    "session_id": "session-123",
    "namespace": "user",
    "key": "my_key",
    "value": "my_value"
  },
  "id": 1
}
```

**必需参数**: `session_id`, `namespace`, `key`, `value`

#### get 方法

```json
{
  "jsonrpc": "2.0",
  "method": "get",
  "params": {
    "session_id": "session-123",
    "namespace": "user",
    "key": "my_key"
  },
  "id": 2
}
```

**必需参数**: `session_id`, `namespace`, `key`
**注意**: 不应包含 `value` 参数

#### remove 方法

```json
{
  "jsonrpc": "2.0",
  "method": "remove",
  "params": {
    "session_id": "session-123",
    "namespace": "user",
    "key": "my_key"
  },
  "id": 3
}
```

**必需参数**: `session_id`, `namespace`, `key`

#### contains 方法

```json
{
  "jsonrpc": "2.0",
  "method": "contains",
  "params": {
    "session_id": "session-123",
    "namespace": "user",
    "key": "my_key"
  },
  "id": 4
}
```

**必需参数**: `session_id`, `namespace`, `key`

#### subscribe 方法

```json
{
  "jsonrpc": "2.0",
  "method": "subscribe",
  "params": {
    "session_id": "session-123",
    "namespace": "user",
    "key": "my_key"
  },
  "id": 5
}
```

**必需参数**: `session_id`, `namespace`, `key`

#### unsubscribe 方法

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

**必需参数**: `subscription_id`

#### publish 方法

```json
{
  "jsonrpc": "2.0",
  "method": "publish",
  "params": {
    "session_id": "session-123",
    "namespace": "user",
    "topic": "my_topic",
    "value": "payload"
  },
  "id": 7
}
```

**必需参数**: `session_id`, `namespace`, `topic`, `value`

#### async_callback 方法

```json
{
  "jsonrpc": "2.0",
  "method": "async_callback",
  "params": {
    "session_id": "session-123",
    "dialogue_id": "dialogue-456",
    "task_id": "task-789",
    "node_id": "node-101",
    "call_key": "callback_event",
    "content": "{\"status\": \"ok\"}"
  },
  "id": 8
}
```

**必需参数**: `session_id`, `dialogue_id`, `task_id`, `node_id`, `call_key`, `content`

### Namespace 验证

有效命名空间值：

| Namespace | 用途 |
|-----------|------|
| `sys` | 框架通信 |
| `ctx` | 上下文存储 |
| `llm` | LLM 交互 |
| `tool` | 工具调用 |
| `user` | 用户数据 |

### 环境变量验证

async_callback 方法依赖以下环境变量：

| 变量名 | 用途 |
|--------|------|
| `AF_SESSION_ID` | 当前会话 ID |
| `AF_DIALOGUE_ID` | 当前对话 ID |
| `AF_SELF_TASK_ID` | 当前任务 ID |
| `AF_NODE_ID` | 当前节点 ID |

## 测试用例清单

### Python SDK

| 测试项 | 验证内容 |
|--------|---------|
| `test_put` | put 方法发送正确的 JSON-RPC 请求 |
| `test_get` | get 方法不包含 value 参数 |
| `test_remove` | remove 方法参数正确 |
| `test_contains` | contains 方法参数正确 |
| `test_subscribe` | subscribe 方法参数正确 |
| `test_unsubscribe` | unsubscribe 包含 subscription_id |
| `test_publish` | publish 包含 topic 和 value |
| `test_async_callback` | async_callback 所有参数正确 |
| `test_namespaces` | 各命名空间参数正确 |
| `test_json_values` | JSON 值正确序列化 |

### Go SDK

| 测试项 | 验证内容 |
|--------|---------|
| `test_put` | Put 方法参数正确 |
| `test_get` | Get 方法参数正确 |
| `test_async_callback` | SendToSelf/SendMsg 参数正确 |

### CLI Tool

| 测试项 | 验证内容 |
|--------|---------|
| `test_put` | put 命令参数正确 |
| `test_get` | get 命令参数正确 |
| `test_async_callback` | async-callback 命令参数正确 |

### Java SDK

| 测试项 | 验证内容 |
|--------|---------|
| `test_put` | put 方法参数正确 |
| `test_async_callback` | sendToSelf/sendMsg 参数正确 |

## 运行测试

```bash
# 运行所有测试
python3 tests/integration/test_sdk_uds_protocol.py

# 运行结果示例
============================================================
DataBus SDK UDS Protocol Integration Tests
============================================================
Socket: /tmp/test_sdk_uds_1158126.sock
Mock UDS Server started

=== Python SDK Tests ===
  [PASS] put: sends valid JSON-RPC request
  [PASS] get: sends valid JSON-RPC request
  [PASS] contains: sends valid JSON-RPC request
  [PASS] remove: sends valid JSON-RPC request
  [PASS] subscribe: sends valid JSON-RPC request
  [PASS] publish: sends valid JSON-RPC request
  [PASS] async_callback: sends valid JSON-RPC request with all parameters
  [PASS] namespace sys: correct namespace parameter
  [PASS] namespace ctx: correct namespace parameter
  [PASS] namespace llm: correct namespace parameter
  [PASS] namespace tool: correct namespace parameter
  [PASS] namespace user: correct namespace parameter
  [PASS] JSON values: correctly serialized

=== CLI Tool Tests ===
  [PASS] put: sends valid JSON-RPC request
  [PASS] get: sends valid JSON-RPC request
  [PASS] contains: sends valid JSON-RPC request
  [PASS] remove: sends valid JSON-RPC request
  [PASS] publish: sends valid JSON-RPC request
  [PASS] async-callback: sends valid JSON-RPC request

=== Go SDK Tests ===
  [PASS] put: sends correct parameters
  [PASS] get: sends correct parameters
  [PASS] contains: sends correct parameters
  [PASS] remove: sends correct parameters
  [PASS] async_callback: sends correct parameters

=== Java SDK Tests ===
  [SKIP] Java SDK tests require JVM and Maven/Gradle setup

============================================================
Results: PASSED=24 FAILED=0
============================================================
```

## 测试注意事项

### Go SDK 测试

Go SDK 的 `AsyncSDK()` 在创建时读取环境变量。测试框架必须在启动 Go 测试程序**之前**设置环境变量：

```python
env["AF_SESSION_ID"] = "go-session"
env["AF_DIALOGUE_ID"] = "go-dialogue"
env["AF_SELF_TASK_ID"] = "go-task"
env["AF_NODE_ID"] = "go-node"
```

### Java SDK 测试

Java SDK 测试需要：
- JVM 17+
- Maven 或 Gradle
- 编译 Java 测试类

当前测试框架跳过 Java SDK 测试，需要手动验证。

## 扩展测试

### 压力测试

- 并发请求：多线程同时发送请求
- 大消息：测试大 JSON 值序列化
- 长连接：长时间连接稳定性

### 错误场景

- 无效 JSON 值
- 空参数
- 超长键名
- 特殊字符处理