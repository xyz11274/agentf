# AgentFlow DataBus Java 客户端

通过 Unix 域套接字访问 AgentFlow DataBus 的 Java 客户端库。

## 环境要求

- Java 17 及以上版本
- Maven 3.6+（构建时需要）

## 安装

### Maven 依赖

在 `pom.xml` 中添加：

```xml
<dependency>
    <groupId>com.agentflow</groupId>
    <artifactId>agentflow-databus</artifactId>
    <version>1.0.0</version>
</dependency>
```

### 从源码构建

```bash
cd src/runtime/databus/clients/java
mvn clean install
```

## 快速上手

```java
import com.agentflow.databus.DataBusClient;
import com.agentflow.databus.DataBusException;

// 通过环境变量连接（AF_DATABUS_UDS_FILE）
try (DataBusClient client = DataBusClient.fromEnv()) {
    // 存储值
    client.user().put("my_key", "my_value");

    // 读取值
    String value = (String) client.user().get("my_key");
    System.out.println(value);  // "my_value"

    // 检查键是否存在
    boolean exists = client.user().contains("my_key");

    // 删除键
    client.user().remove("my_key");
} catch (DataBusException | IOException e) {
    e.printStackTrace();
}
```

## 使用说明

### 连接方式

```java
// 通过环境变量
DataBusClient client = DataBusClient.fromEnv();

// 显式指定 socket 路径
DataBusClient client = new DataBusClient("/tmp/agentflow_session_xxx.sock");

// 使用完毕后关闭（或使用 try-with-resources）
client.close();
```

### 命名空间访问

```java
// sys 命名空间 - 框架通信
client.sys().put("response", "Hello World");
client.sys().put("api_key", "sk-xxx");

// ctx 命名空间 - 上下文存储
client.ctx().put("history", historyList);
client.ctx().put("state", stateMap);

// llm 命名空间 - LLM 服务交互
client.llm().put("last_prompt", "What is AI?");
client.llm().put("last_response", llmResponse);

// tool 命名空间 - 工具调用信息
client.tool().put("call_result", result);
client.tool().put("tool_list", toolList);

// user 命名空间 - 用户自定义数据
client.user().put("custom_key", customValue);
```

### 对话隔离

多对话会话中，通过 dialogue ID 实现数据隔离：

```java
// 带对话隔离的访问
client.user("dialogue-123").put("skill_result", result);
Object result = client.user("dialogue-123").get("skill_result");
```

### 链式调用风格

API 与 C++ INodeDataBusClient 接口保持一致：

```java
// 链式调用
client.user().put("key", value);
Object val = client.user().get("key");
```

### JSON 值存储

支持复杂对象（序列化为 JSON）：

```java
Map<String, Object> config = new HashMap<>();
config.put("debug", true);
config.put("level", 5);
client.user().put("config", config);

// 读取
Object retrieved = client.user().get("config");
```

### 原始访问

绕过 session 隔离直接使用原始键路径：

```java
// 直接操作原始键路径
client.raw().put("/session/abc123/user/my_key", "value");
Object value = client.raw().get("/session/abc123/user/my_key");
```

### 错误处理

```java
try {
    client.user().put("key", "value");
} catch (DataBusException e) {
    System.err.println("错误码: " + e.getCode());
    System.err.println("错误信息: " + e.getMessage());
}
```

## API 参考

### DataBusClient

| 方法 | 说明 |
|------|------|
| `fromEnv()` | 通过 `AF_DATABUS_UDS_FILE` 环境变量创建客户端 |
| `new DataBusClient(path)` | 显式指定 socket 路径创建客户端 |
| `close()` | 关闭连接 |
| `getSessionId()` | 获取会话 ID |
| `getSocketPath()` | 获取 socket 路径 |
| `sys()` | 访问 sys 命名空间 |
| `ctx()` | 访问 ctx 命名空间 |
| `llm()` | 访问 llm 命名空间 |
| `tool()` | 访问 tool 命名空间 |
| `user()` | 访问 user 命名空间 |
| `raw()` | 原始 DataBus 访问 |

### NamespaceClient

| 方法 | 说明 |
|------|------|
| `put(key, value)` | 存储值 |
| `get(key)` | 读取值 |
| `get(key, default)` | 读取值（带默认值） |
| `remove(key)` | 删除键 |
| `contains(key)` | 检查键是否存在 |
| `subscribe(key, callback)` | 订阅数据变更 |
| `unsubscribe(subscriptionId)` | 取消订阅 |
| `publish(topic, payload)` | 发布事件 |
| `listKeys()` | 列出所有键 |
| `listKeys(prefix)` | 列出指定前缀的键 |

## 环境变量

| 变量名 | 说明 |
|--------|------|
| `AF_DATABUS_UDS_FILE` | Unix 域套接字路径 |

## 线程安全

`DataBusClient` 是线程安全的，多个线程可共享同一客户端实例。

## 许可证

MIT License
