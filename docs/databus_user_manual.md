# DataBus 用户手册

> AgentFlow 数据总线完整指南
> 版本：v1.1 | 更新日期：2026-03-16

---

## 目录

1. [概述](#1-概述)
2. [架构设计](#2-架构设计)
3. [多 Session 与多对话隔离机制](#3-多-session-与多对话隔离机制)
4. [数据结构说明](#4-数据结构说明)
5. [C++ 接口](#5-c-接口)
6. [Python 接口](#6-python-接口)
7. [Java 接口](#7-java-接口)
8. [Go 接口](#8-go-接口)
9. [CLI 命令行工具](#9-cli-命令行工具)
10. [UDS 协议接口](#10-uds-协议接口)
11. [最佳实践](#11-最佳实践)
12. [故障排查](#12-故障排查)

---

## 1. 概述

### 1.1 什么是 DataBus

DataBus 是 AgentFlow 框架的核心数据共享组件，提供高性能的键值存储和发布订阅功能。它支持：

- **零拷贝数据传输** - 大型数据通过引用传递
- **实时订阅** - 数据变更时自动推送通知
- **会话隔离** - 多用户、多对话数据完全隔离
- **多语言支持** - C++、Python、Java、Go、Shell 均可访问

### 1.2 核心特性

| 特性 | 说明 |
|------|------|
| 数据操作 | `put`、`get`、`remove`、`contains` |
| 批量操作 | `remove_by_prefix`、`list_keys_by_prefix` |
| 订阅机制 | `subscribe`、`unsubscribe` |
| 命名空间 | 自动隔离不同类型的数据 |
| 会话隔离 | 多租户场景下的数据安全 |
| 对话隔离 | 同一会话内多对话并行处理 |

---

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        AgentFlow Agent                           │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                      MMDataBus                              │  │
│  │  ┌─────────────────┐    ┌─────────────────────────────┐   │  │
│  │  │  LocalDataBus   │    │      RemoteDataBus          │   │  │
│  │  │  (内存存储)      │    │      (Redis 分布式)          │   │  │
│  │  └─────────────────┘    └─────────────────────────────┘   │  │
│  │                              │                              │  │
│  │                              ▼                              │  │
│  │  ┌─────────────────────────────────────────────────────┐   │  │
│  │  │                    UDSServer                         │   │  │
│  │  │        /tmp/agentflow_{session_id}.sock             │   │  │
│  │  │                 JSON-RPC 2.0                         │   │  │
│  │  └─────────────────────────────────────────────────────┘   │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                               │
              ┌────────────────┼────────────────┐
              ▼                ▼                ▼
        ┌──────────┐    ┌──────────┐    ┌──────────┐
        │  Python  │    │    Go    │    │   Java   │
        │  Client  │    │  Client  │    │  Client  │
        └──────────┘    └──────────┘    └──────────┘
```

### 2.2 核心组件

| 组件 | 文件位置 | 说明 |
|------|----------|------|
| MMDataBus | `include/internal/runtime/databus/databus.h` | 主数据总线类 |
| LocalDataBus | `include/internal/runtime/databus/local_databus.h` | 本地内存存储 |
| RemoteDataBus | `include/internal/runtime/databus/remote_databus.h` | Redis 分布式存储 |
| UDSServer | `include/internal/runtime/databus/uds_server.h` | Unix Domain Socket 服务端 |
| NodeDataBusClient | `include/internal/runtime/databus/node_databus_client.h` | 会话隔离客户端 |

---

## 3. 多 Session 与多对话隔离机制

### 3.1 隔离层级

DataBus 提供三级数据隔离：

| 层级 | 范围 | 实现方式 |
|------|------|----------|
| Agent | 每个 Agent 实例 | 独立的 MMDataBus 实例 |
| Session | 每个会话/对话 | Key 前缀 `/session/{session-id}/` |
| Namespace | 每个数据类别 | Key 前缀 `/session/{session-id}/{namespace}/` |

### 3.2 Session ID

Session ID 是会话的唯一标识符：

- **格式**：UUID 格式，如 `550e8400-e29b-41d4-a716-446655440000`
- **生成**：由框架在会话创建时自动生成
- **独立性**：与 `user_id` 和 `client_id` 独立，这些作为会话元数据存储

### 3.3 Dialogue ID

Dialogue ID 用于同一会话内的多对话隔离：

- **格式**：数值哈希字符串，如 `13155239170500615310`
- **生成**：`hash(user_text + message_id)` 在分发器中生成
- **用途**：支持单会话多对话并行处理

### 3.4 Key 结构

所有会话隔离数据遵循以下 Key 结构：

```
/session/{session-id}/{namespace}/{key}
```

**多对话场景的 Key 结构**：

```
/session/{session-id}/{namespace}/{dialogue-id}/{key}
```

**示例**：

```
/session/550e8400-e29b-41d4-a716-446655440000/user/my_data
/session/550e8400-e29b-41d4-a716-446655440000/sys/response
/session/550e8400-e29b-41d4-a716-446655440000/ctx/13155239170500615310/content
```

### 3.5 数据流向图

```
用户请求 ──────────────────────────────────────────────────────────┐
    │                                                              │
    ▼                                                              │
┌─────────────────┐                                                │
│   Dispatcher    │ ◀── 生成 dialogue_id = hash(text + msg_id)    │
│                 │                                                │
└────────┬────────┘                                                │
         │                                                         │
         ▼                                                         │
┌─────────────────────────────────────────────────────────────┐   │
│                       DataBus                                 │   │
│  ┌───────────────────────────────────────────────────────┐  │   │
│  │  /session/{session-id}/                               │  │   │
│  │  ├── sys/                                             │  │   │
│  │  │   ├── {dialogue-id}/dialogue_id                    │  │   │
│  │  │   ├── {dialogue-id}/user_prompt                    │  │   │
│  │  │   ├── {dialogue-id}/next_skills                    │  │   │
│  │  │   └── {dialogue-id}/response                       │  │   │
│  │  ├── ctx/                                             │  │   │
│  │  │   └── {dialogue-id}/content    ← Context Callback  │──┘   │
│  │  ├── user/                                            │      │
│  │  │   └── {dialogue-id}/skill_result                   │      │
│  │  └── llm/                                             │      │
│  │       └── {dialogue-id}/last_response                 │      │
│  └───────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. 数据结构说明

### 4.1 命名空间

| 命名空间 | 用途 | 示例 Key |
|----------|------|----------|
| `sys` | 框架消息和配置 | `response`、`api_key`、`config` |
| `ctx` | 上下文存储（保留） | `history`、`state` |
| `llm` | LLM 服务交互（保留） | `last_prompt`、`last_response` |
| `tool` | 工具调用信息（保留） | `call_result`、`tool_list` |
| `user` | 用户自定义节点数据 | 任意自定义 Key |

### 4.2 DataVariant 类型

DataBus 支持的数据类型：

```cpp
using DataVariant = std::variant<
    std::string,
    int64_t,
    double,
    bool,
    std::vector<uint8_t>,  // 二进制数据
    std::map<std::string, DataVariant>  // 嵌套对象
>;
```

### 4.3 OwnerContext 结构

用于 DataRef 的所有者索引：

```cpp
struct OwnerContext {
    std::string request_id;
    std::string user_id;
    std::string client_id;
    std::string agent_id;
    std::string session_id;
    std::string task_id;
    std::string workflow_id;
    std::string node_id;
    std::string master_id;
    std::string worker_id;
};
```

### 4.4 DataRef 结构

用于大数据的引用传递：

```cpp
struct DataRef {
    std::string data_id;    // 数据唯一标识
    std::string address;    // 存储地址
    std::string checksum;   // 校验和
};
```

---

## 5. C++ 接口

### 5.1 INodeDataBusClient 接口

```cpp
class INodeDataBusClient {
public:
    virtual ~INodeDataBusClient() = default;

    // 命名空间访问
    virtual INamespaceClient* sys() = 0;    // 框架通信
    virtual INamespaceClient* ctx() = 0;    // 上下文存储
    virtual INamespaceClient* llm() = 0;    // LLM 交互
    virtual INamespaceClient* tool() = 0;   // 工具调用
    virtual INamespaceClient* user() = 0;   // 用户数据

    // Session ID 获取
    virtual const std::string& session_id() const = 0;

    // 原始 DataBus 访问（绕过隔离）
    virtual INodeDataBus* raw() = 0;
};
```

### 5.2 INamespaceClient 接口

```cpp
class INamespaceClient {
public:
    virtual ~INamespaceClient() = default;

    // 数据操作
    virtual bool put(const std::string& key, const SoDataVariant& value) = 0;
    virtual bool get(const std::string& key, SoDataVariant& value) const = 0;
    virtual bool contains(const std::string& key) const = 0;
    virtual bool remove(const std::string& key) = 0;

    // 订阅机制
    virtual SubscriptionHandle subscribe(const std::string& key,
                                         SoDataBusCallback callback) = 0;
    virtual void unsubscribe(const SubscriptionHandle& handle) = 0;
};
```

### 5.3 V3 节点签名示例

```cpp
#include "agentflow/so_node.h"

extern "C" void my_node_v3(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBusClient* client) {

    // 存储用户数据
    client->user()->put("result", SoDataVariant(std::string("success")));

    // 读取框架配置
    SoDataVariant api_key;
    if (client->sys()->get("api_key", api_key)) {
        std::string key = api_key.as_string();
        // 使用 API key...
    }

    // 读取上一轮响应
    SoDataVariant response;
    if (client->sys()->get("response", response)) {
        // 处理响应...
    }

    // 订阅数据变更
    auto handle = client->user()->subscribe("watch_key",
        [](const std::string& key, const SoDataVariant& value) {
            // 处理变更
        });

    // 记得取消订阅
    client->user()->unsubscribe(handle);
}
```

### 5.4 直接使用 MMDataBus

```cpp
#include "runtime/databus/databus.h"

// 创建 DataBus
agentflow::runtime::MMDataBus databus(1024 * 1024);  // 1MB 容量

// 基本操作
databus.put("/session/abc123/user/my_key",
            agentflow::DataVariant(std::string("value")));

agentflow::DataVariant value;
if (databus.get("/session/abc123/user/my_key", value)) {
    std::cout << std::get<std::string>(value) << std::endl;
}

// 订阅
databus.subscribe("/session/abc123/user/my_key",
    [](const std::string& key, const agentflow::DataVariant& val) {
        std::cout << "Key " << key << " changed" << std::endl;
    });

// 连接远程 DataBus（Redis）
databus.connect_remote("redis://127.0.0.1:6379");

// 按前缀删除数据（用于会话清理）
int removed = databus.remove_by_prefix("/session/abc123");
std::cout << "Removed " << removed << " keys" << std::endl;

// 按前缀列出键
auto keys = databus.list_keys_by_prefix("/session/abc123/user");
for (const auto& key : keys) {
    std::cout << "Found key: " << key << std::endl;
}
```

### 5.5 按前缀操作方法

DataBus 提供两个按前缀操作的方法，主要用于会话生命周期管理：

| 方法 | 说明 | 返回值 |
|------|------|--------|
| `remove_by_prefix(prefix)` | 删除所有匹配前缀的键 | 删除的键数量 |
| `list_keys_by_prefix(prefix)` | 列出所有匹配前缀的键 | 键名列表 |

**使用场景**：

```cpp
// 场景1：SESSION-DESTROY 时清理会话数据
void destroy_session(const std::string& session_id) {
    std::string prefix = "/session/" + session_id;
    int removed = databus.remove_by_prefix(prefix);
    std::cout << "Cleaned up " << removed << " keys for session " << session_id << std::endl;
}

// 场景2：查看会话中的用户数据
void list_user_data(const std::string& session_id) {
    std::string prefix = "/session/" + session_id + "/user";
    auto keys = databus.list_keys_by_prefix(prefix);
    std::cout << "User data keys:" << std::endl;
    for (const auto& key : keys) {
        std::cout << "  " << key << std::endl;
    }
}
```

---

## 6. Python 接口

### 6.1 安装与导入

```python
# SDK 位于 clients/python/agentflow_databus.py
from agentflow_databus import DataBusClient, DataBusError
```

### 6.2 连接 DataBus

```python
# 方式一：使用环境变量 AF_DATABUS_UDS_FILE
client = DataBusClient.from_env()

# 方式二：指定 socket 路径
client = DataBusClient("/tmp/agentflow_session_xxx.sock")

# 方式三：指定 socket 路径和 session_id
client = DataBusClient("/tmp/agentflow_session_xxx.sock", session_id="xxx")

# 使用上下文管理器自动关闭
with DataBusClient.from_env() as client:
    client.user().put("key", "value")
```

### 6.3 基本操作

```python
# 存储数据
client.user().put("my_key", "my_value")
client.user().put("json_data", {"status": "ok", "count": 42})

# 读取数据
value = client.user().get("my_key")
json_data = client.user().get("json_data")
print(value)  # "my_value"

# 检查存在
if client.user().contains("my_key"):
    print("Key exists")

# 删除数据
client.user().remove("my_key")
```

### 6.4 命名空间访问

```python
# sys 命名空间 - 框架通信
client.sys().put("response", "Hello World")
response = client.sys().get("response")

# ctx 命名空间 - 上下文存储
client.ctx().put("history", "Previous conversation...")

# user 命名空间 - 用户数据
client.user().put("custom_data", {"key": "value"})

# llm 命名空间 - LLM 交互
client.llm().put("last_prompt", "What is AI?")

# tool 命名空间 - 工具调用
client.tool().put("call_result", {"result": "success"})
```

### 6.5 多对话隔离

```python
# 指定 dialogue_id 进行对话级隔离
dialogue_id = "13155239170500615310"

# 存储对话特定数据
client.user(dialogue_id).put("skill_result", {"status": "ok"})
client.ctx(dialogue_id).put("content", "User context for this dialogue")

# 读取对话数据
result = client.user(dialogue_id).get("skill_result")
```

### 6.6 订阅机制

```python
# 定义回调函数
def on_data_change(key, value):
    print(f"Key {key} changed to {value}")

# 订阅数据变更
sub_id = client.user().subscribe("my_key", on_data_change)
print(f"Subscribed with ID: {sub_id}")

# 取消订阅
client.user().unsubscribe(sub_id)
```

### 6.7 原始访问

```python
# 绕过会话隔离，访问全局数据（谨慎使用）
client.raw().put("/global/config", "value")
value = client.raw().get("/global/config")
```

### 6.8 错误处理

```python
from agentflow_databus import DataBusError, JSONRPCError

try:
    client.user().put("key", "value")
except DataBusError as e:
    print(f"DataBus error: {e}")
except JSONRPCError as e:
    print(f"JSON-RPC error {e.code}: {e.message}")
```

---

## 7. Java 接口

### 7.1 依赖配置

```xml
<!-- pom.xml -->
<dependency>
    <groupId>com.agentflow</groupId>
    <artifactId>databus-client</artifactId>
    <version>1.0.0</version>
</dependency>
```

### 7.2 连接 DataBus

```java
import com.agentflow.databus.DataBusClient;
import com.agentflow.databus.NamespaceClient;
import com.agentflow.databus.DataBusException;

// 方式一：使用环境变量
DataBusClient client = DataBusClient.fromEnv();

// 方式二：指定 socket 路径
DataBusClient client = new DataBusClient("/tmp/agentflow_session_xxx.sock");

// 使用 try-with-resources 自动关闭
try (DataBusClient client = DataBusClient.fromEnv()) {
    client.user().put("key", "value");
}
```

### 7.3 基本操作

```java
// 存储数据
client.user().put("my_key", "my_value");
client.user().put("json_data", Map.of("status", "ok", "count", 42));

// 读取数据
String value = (String) client.user().get("my_key");
Map<?, ?> jsonData = (Map<?, ?>) client.user().get("json_data");

// 检查存在
if (client.user().contains("my_key")) {
    System.out.println("Key exists");
}

// 删除数据
client.user().remove("my_key");
```

### 7.4 命名空间访问

```java
// sys 命名空间
client.sys().put("response", "Hello World");

// ctx 命名空间
client.ctx().put("history", "Previous conversation...");

// user 命名空间
client.user().put("custom_data", "value");

// llm 命名空间
client.llm().put("last_prompt", "What is AI?");

// tool 命名空间
client.tool().put("call_result", Map.of("result", "success"));
```

### 7.5 多对话隔离

```java
String dialogueId = "13155239170500615310";

// 存储对话特定数据
client.user(dialogueId).put("skill_result", Map.of("status", "ok"));
client.ctx(dialogueId).put("content", "User context");

// 读取对话数据
Object result = client.user(dialogueId).get("skill_result");
```

### 7.6 订阅机制

```java
// 订阅数据变更
BiConsumer<String, Object> callback = (key, value) -> {
    System.out.println("Key " + key + " changed to " + value);
};

long subId = client.user().subscribe("my_key", callback);

// 取消订阅
client.user().unsubscribe(subId);
```

### 7.7 错误处理

```java
try {
    client.user().put("key", "value");
} catch (DataBusException e) {
    System.err.println("DataBus error: " + e.getMessage());
    if (e.getCode() != -1) {
        System.err.println("Error code: " + e.getCode());
    }
}
```

---

## 8. Go 接口

### 8.1 包导入

```go
import "agentflow/clients/golang/databus"
```

### 8.2 连接 DataBus

```go
// 方式一：使用环境变量
client, err := databus.NewClientFromEnv()
if err != nil {
    log.Fatal(err)
}
defer client.Close()

// 方式二：指定 socket 路径
client, err := databus.NewClient("/tmp/agentflow_session_xxx.sock")
if err != nil {
    log.Fatal(err)
}
defer client.Close()
```

### 8.3 基本操作

```go
// 存储数据
err := client.User().Put("my_key", "my_value")
if err != nil {
    log.Fatal(err)
}

// 存储复杂数据
err = client.User().Put("json_data", map[string]any{
    "status": "ok",
    "count":  42,
})

// 读取数据
value, err := client.User().Get("my_key")
if err != nil {
    log.Fatal(err)
}
fmt.Println(value)

// 类型便捷方法
str, _ := client.User().GetString("my_key")
num, _ := client.User().GetInt("count")
flag, _ := client.User().GetBool("enabled")

// 检查存在
exists, _ := client.User().Contains("my_key")
if exists {
    fmt.Println("Key exists")
}

// 删除数据
client.User().Remove("my_key")
```

### 8.4 命名空间访问

```go
// sys 命名空间
client.Sys().Put("response", "Hello World")

// ctx 命名空间
client.Ctx().Put("history", "Previous conversation...")

// user 命名空间
client.User().Put("custom_data", "value")

// llm 命名空间
client.LLM().Put("last_prompt", "What is AI?")

// tool 命名空间
client.Tool().Put("call_result", map[string]any{"result": "success"})
```

### 8.5 多对话隔离

```go
dialogueID := "13155239170500615310"

// 存储对话特定数据
client.UserWithDialogue(dialogueID).Put("skill_result",
    map[string]any{"status": "ok"})
client.CtxWithDialogue(dialogueID).Put("content", "User context")

// 读取对话数据
result, _ := client.UserWithDialogue(dialogueID).Get("skill_result")
```

### 8.6 订阅机制

```go
// 订阅数据变更
subID, err := client.User().Subscribe("my_key", func(key string, value any) {
    fmt.Printf("Key %s changed to %v\n", key, value)
})
if err != nil {
    log.Fatal(err)
}

// 取消订阅
client.User().Unsubscribe(subID)
```

### 8.7 错误处理

```go
import "errors"

value, err := client.User().Get("key")
if err != nil {
    if errors.Is(err, databus.ErrNotConnected) {
        log.Println("Not connected to DataBus")
    } else if rpcErr, ok := err.(*databus.Error); ok {
        log.Printf("RPC error %d: %s\n", rpcErr.Code, rpcErr.Message)
    } else {
        log.Fatal(err)
    }
}
```

---

## 9. CLI 命令行工具

### 9.1 工具位置

```bash
# CLI 工具位于
clients/cli/databus_cli.py

# 添加执行权限
chmod +x clients/cli/databus_cli.py
```

### 9.2 基本使用

```bash
# 设置环境变量
export AF_DATABUS_UDS_FILE=/tmp/agentflow_session_xxx.sock

# 存储数据
databus-cli -s {session-id} -ns user put "my_key" "my_value"
databus-cli -s {session-id} -ns user put "json_data" '{"status":"ok"}'

# 读取数据
databus-cli -s {session-id} -ns user get "my_key"

# 检查存在
databus-cli -s {session-id} -ns user contains "my_key"

# 删除数据
databus-cli -s {session-id} -ns user remove "my_key"
```

### 9.3 多对话访问

```bash
# 指定 dialogue_id
databus-cli -s {session-id} -d {dialogue-id} -ns user put "result" '{"status":"ok"}'
databus-cli -s {session-id} -d {dialogue-id} -ns user get "result"
```

### 9.4 命名空间选择

```bash
# -ns/--namespace 参数可选值：sys, ctx, llm, tool, user

# sys 命名空间
databus-cli -s {session-id} -ns sys put "response" "Hello"

# ctx 命名空间
databus-cli -s {session-id} -ns ctx put "history" "conversation..."

# user 命名空间（默认）
databus-cli -s {session-id} -ns user put "data" "value"
```

### 9.5 订阅数据变更

```bash
# 订阅（阻塞运行）
databus-cli -s {session-id} -ns user subscribe "my_key"

# 按 Ctrl+C 停止订阅
```

### 9.6 发布事件

```bash
# 发布事件
databus-cli -s {session-id} -ns sys publish "topic.event" "payload"
```

### 9.7 输出格式

```bash
# 默认 JSON 格式化输出
databus-cli -s {session-id} -ns user get "json_data"
# {
#   "status": "ok",
#   "count": 42
# }

# 原始输出
databus-cli -s {session-id} -ns user --raw get "my_key"
# my_value

# 静默模式（不输出错误信息）
databus-cli -s {session-id} -ns user -q get "nonexistent"
```

---

## 10. UDS 协议接口

### 10.1 协议概述

UDS Server 实现 JSON-RPC 2.0 协议，供外部进程访问 DataBus。

**Socket 路径格式**：`/tmp/agentflow_{session_id}.sock`

### 10.2 JSON-RPC 格式

#### 请求格式

```json
{
    "jsonrpc": "2.0",
    "method": "put",
    "params": {
        "session_id": "abc123",
        "namespace": "user",
        "key": "my_key",
        "value": "my_value"
    },
    "id": 1
}
```

#### 成功响应

```json
{
    "jsonrpc": "2.0",
    "result": {
        "success": true
    },
    "id": 1
}
```

#### 错误响应

```json
{
    "jsonrpc": "2.0",
    "error": {
        "code": -1,
        "message": "Key not found"
    },
    "id": 1
}
```

#### 通知（订阅）

```json
{
    "jsonrpc": "2.0",
    "method": "data_changed",
    "params": {
        "key": "/session/abc123/user/my_key",
        "value": "new_value"
    }
}
```

### 10.3 方法列表

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `put` | `{session_id, namespace, key, value}` | `{success}` | 存储值 |
| `get` | `{session_id, namespace, key}` | `{exists, value}` | 获取值 |
| `remove` | `{session_id, namespace, key}` | `{success}` | 删除键 |
| `contains` | `{session_id, namespace, key}` | `{exists}` | 检查存在 |
| `subscribe` | `{session_id, namespace, key}` | `{subscription_id}` | 订阅变更 |
| `unsubscribe` | `{subscription_id}` | `{success}` | 取消订阅 |
| `publish` | `{session_id, namespace, topic, value}` | `{success}` | 发布事件 |
| `list_keys` | `{session_id, namespace, prefix?}` | `{keys}` | 列出键 |

### 10.4 命名空间值

| 值 | 说明 |
|----|------|
| `sys` | 框架通信（response、api_key、config） |
| `ctx` | 上下文存储（history、state） |
| `llm` | LLM 服务交互（last_prompt、last_response） |
| `tool` | 工具调用信息（call_result、tool_list） |
| `user` | 用户自定义节点数据（自定义 Key） |

### 10.5 Key 构建规则

Key 自动构建为：`/session/{session_id}/{namespace}/{key}`

**示例**：
- `session_id`: `abc123`
- `namespace`: `user`
- `key`: `my_data`

完整 Key: `/session/abc123/user/my_data`

### 10.6 Python Socket 连接示例

```python
import socket
import json

# 连接 UDS
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect("/tmp/agentflow_abc123.sock")

# 发送请求
request = {
    "jsonrpc": "2.0",
    "method": "put",
    "params": {
        "session_id": "abc123",
        "namespace": "user",
        "key": "test_key",
        "value": "test_value"
    },
    "id": 1
}
sock.sendall((json.dumps(request) + "\n").encode())

# 接收响应
response = json.loads(sock.recv(4096).decode())
print(response)

sock.close()
```

### 10.7 Shell Netcat 示例

```bash
# 使用 nc 连接 UDS（需要 socat）
socat - UNIX-CONNECT:/tmp/agentflow_abc123.sock

# 发送请求（手动输入）
{"jsonrpc":"2.0","method":"get","params":{"session_id":"abc123","namespace":"user","key":"test"},"id":1}
```

---

## 11. 最佳实践

### 11.1 命名空间使用建议

| 场景 | 推荐命名空间 | 说明 |
|------|--------------|------|
| 节点间传递结果 | `user` | 自定义数据存储 |
| 读取 API Key | `sys` | 框架配置 |
| 存储对话历史 | `ctx` | 上下文存储 |
| LLM 调用记录 | `llm` | LLM 相关数据 |
| 工具调用结果 | `tool` | 工具相关数据 |

### 11.2 隔离层级选择

```
┌─────────────────────────────────────────────────────────────┐
│  场景                          │  推荐隔离层级              │
├─────────────────────────────────────────────────────────────┤
│  全局配置（所有会话共享）       │  Agent 级（raw 访问）      │
│  用户级数据（跨会话）           │  需自行实现外部存储         │
│  会话级数据                     │  Session 级（默认）        │
│  对话级数据                     │  Dialogue 级（带 dialogue_id）│
└─────────────────────────────────────────────────────────────┘
```

### 11.3 性能优化建议

1. **大数据使用 DataRef**
   ```cpp
   // 大数据通过引用传递，避免拷贝
   DataRef ref;
   databus.store_with_ref("large_data", large_value, owner, ref);
   ```

2. **合理使用订阅**
   ```python
   # 订阅比轮询更高效
   client.user().subscribe("key", callback)
   # 而非
   while True:
       value = client.user().get("key")
       time.sleep(0.1)
   ```

3. **批量操作**
   ```python
   # 减少连接开销，复用客户端
   with DataBusClient.from_env() as client:
       for key, value in data.items():
           client.user().put(key, value)
   ```

### 11.4 错误处理模式

```python
from agentflow_databus import DataBusClient, DataBusError, JSONRPCError

def safe_put(client, key, value, max_retries=3):
    for attempt in range(max_retries):
        try:
            return client.user().put(key, value)
        except JSONRPCError as e:
            if e.code == -1:  # Key not found
                return False
            if attempt < max_retries - 1:
                time.sleep(0.1 * (attempt + 1))
        except DataBusError as e:
            if attempt < max_retries - 1:
                time.sleep(0.5)
    return False
```

### 11.5 安全注意事项

1. **谨慎使用 raw() 访问**
   ```python
   # 仅用于读取全局配置，避免写入
   config = client.raw().get("/global/config")
   ```

2. **会话 ID 保密**
   - Session ID 是会话的唯一标识
   - 不要在日志中暴露

3. **敏感数据加密**
   - DataBus 不自动加密数据
   - 敏感数据应先加密再存储

---

## 12. 故障排查

### 12.1 常见错误

| 错误 | 原因 | 解决方案 |
|------|------|----------|
| `AF_DATABUS_UDS_FILE not set` | 环境变量未设置 | 设置环境变量或显式指定路径 |
| `Connection refused` | Socket 文件不存在 | 检查 Agent 是否启动 UDS Server |
| `Key not found` | Key 不存在 | 检查 Key 路径是否正确 |
| `Invalid namespace` | 无效的命名空间 | 使用 sys/ctx/llm/tool/user |

### 12.2 调试技巧

```python
# 启用详细日志
import logging
logging.basicConfig(level=logging.DEBUG)

# 检查连接状态
client = DataBusClient.from_env()
print(f"Session ID: {client.session_id}")
print(f"Socket Path: {client.socket_path}")

# 验证 Key 存在
if client.user().contains("key"):
    print("Key exists")
else:
    print("Key not found")
```

### 12.3 连接问题排查

```bash
# 检查 socket 文件是否存在
ls -la /tmp/agentflow_*.sock

# 检查 socket 是否可连接
socat - UNIX-CONNECT:/tmp/agentflow_session_xxx.sock

# 检查环境变量
echo $AF_DATABUS_UDS_FILE
```

### 12.4 性能问题排查

```python
import time

# 测量操作延迟
start = time.time()
for i in range(100):
    client.user().put(f"key_{i}", f"value_{i}")
elapsed = time.time() - start
print(f"100 puts took {elapsed:.3f}s ({elapsed/100*1000:.1f}ms per op)")
```

---

## 附录

### A. 环境变量

| 变量名 | 说明 | 示例 |
|--------|------|------|
| `AF_DATABUS_UDS_FILE` | UDS Socket 路径 | `/tmp/agentflow_session_xxx.sock` |
| `AF_DATABUS_DIR` | 文件模式目录（旧版） | `/tmp/agentflow_databus` |

### B. 版本兼容性

| 版本 | V2 签名 | V3 签名 | UDS Server |
|------|---------|---------|------------|
| 1.0 | ✅ | ✅ | ✅ |

### C. 相关文档

- [DataBus Session Isolation](./databus-session-isolation.md)
- [Multi-Language DataBus Access](./multi_language_databus.md)
- [Context Callback Mechanism](../publish/INTEGRATION_GUIDE.md#74-上下文回调机制)

---

*文档版本: v1.0 | 最后更新: 2026-03-16*