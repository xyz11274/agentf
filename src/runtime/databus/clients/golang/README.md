# AgentFlow DataBus Go SDK

通过 Unix 域套接字实时访问 DataBus 的 Go 模块。

## 安装

```bash
go get github.com/agentflow/databus/clients/golang/databus
```

或将 `databus/` 目录复制到项目中直接使用。

## 快速上手

```go
package main

import (
    "fmt"
    "log"

    "github.com/agentflow/databus/clients/golang/databus"
)

func main() {
    // 通过环境变量连接（推荐）
    client, err := databus.NewClientFromEnv()
    if err != nil {
        log.Fatal(err)
    }
    defer client.Close()

    // 链式调用风格
    client.User().Put("my_key", "value")
    value, _ := client.User().Get("my_key")
    fmt.Printf("读取值: %v\n", value)
}
```

## 链式调用 API

SDK 提供与 C++ INodeDataBusClient 接口一致的链式调用 API：

```go
// 访问各命名空间
client.Sys().Put("response", "Hello World")      // 框架通信
client.Ctx().Put("history", []string{...})       // 上下文存储
client.LLM().Put("prompt", "...")                // LLM 交互
client.Tool().Put("call_result", map[string]any{...})  // 工具调用
client.User().Put("my_data", map[string]any{...})      // 用户自定义数据

// 读取值（支持类型转换）
value, err := client.User().Get("my_key")
strValue, err := client.User().GetString("my_key")
intValue, err := client.User().GetInt("my_key")
floatValue, err := client.User().GetFloat("my_key")
boolValue, err := client.User().GetBool("my_key")

// 其他操作
exists, err := client.User().Contains("my_key")
err = client.User().Remove("my_key")
```

## 对话隔离

多对话会话中，通过 `*WithDialogue` 方法实现数据隔离：

```go
// 存储特定对话的数据
client.UserWithDialogue("dialogue-123").Put("skill_result", map[string]any{
    "status": "ok",
})

// 读取特定对话的数据
result, err := client.UserWithDialogue("dialogue-123").Get("skill_result")
```

## 订阅

订阅实时数据变更：

```go
// 订阅
subID, err := client.User().Subscribe("my_key", func(key string, value any) {
    fmt.Printf("键 %s 变更为 %v\n", key, value)
})

// 取消订阅
client.User().Unsubscribe(subID)
```

## 原始访问

绕过 session 前缀直接访问全局键：

```go
// 原始访问，不添加 session 前缀
client.Raw().Put("/global/config", "value")
value, err := client.Raw().Get("/global/config")
```

## 错误处理

```go
import (
    "errors"
    "fmt"

    "github.com/agentflow/databus/clients/golang/databus"
)

func main() {
    client, err := databus.NewClientFromEnv()
    if err != nil {
        if errors.Is(err, databus.ErrNotConnected) {
            fmt.Println("未连接到 DataBus")
        }
        log.Fatal(err)
    }
    defer client.Close()

    value, err := client.User().Get("missing_key")
    if err != nil {
        var rpcErr *databus.Error
        if errors.As(err, &rpcErr) {
            fmt.Printf("JSON-RPC 错误 %d: %s\n", rpcErr.Code, rpcErr.Message)
        }
        return
    }
}
```

## 环境变量

| 变量名 | 说明 |
|--------|------|
| `AF_DATABUS_UDS_FILE` | Unix 域套接字路径 |

## API 参考

### Client

#### 函数

- `NewClient(socketPath string) (*Client, error)` - 显式指定 socket 路径创建客户端
- `NewClientFromEnv() (*Client, error)` - 通过 `AF_DATABUS_UDS_FILE` 创建客户端

#### 方法

- `Sys() *NamespaceClient` - 访问 sys 命名空间
- `Ctx() *NamespaceClient` - 访问 ctx 命名空间
- `LLM() *NamespaceClient` - 访问 llm 命名空间
- `Tool() *NamespaceClient` - 访问 tool 命名空间
- `User() *NamespaceClient` - 访问 user 命名空间
- `SysWithDialogue(dialogueID string) *NamespaceClient` - 带 dialogue ID 的 sys
- `CtxWithDialogue(dialogueID string) *NamespaceClient` - 带 dialogue ID 的 ctx
- `LLMWithDialogue(dialogueID string) *NamespaceClient` - 带 dialogue ID 的 llm
- `ToolWithDialogue(dialogueID string) *NamespaceClient` - 带 dialogue ID 的 tool
- `UserWithDialogue(dialogueID string) *NamespaceClient` - 带 dialogue ID 的 user
- `Raw() *RawClient` - 原始访问（无前缀）
- `Close() error` - 关闭连接

#### 属性

- `SessionID() string` - 当前会话 ID
- `SocketPath() string` - Unix 域套接字路径

### NamespaceClient

#### 方法

- `Put(key string, value any) error` - 存储值
- `Get(key string) (any, error)` - 读取值
- `GetString(key string) (string, error)` - 读取字符串
- `GetInt(key string) (int64, error)` - 读取整数
- `GetFloat(key string) (float64, error)` - 读取浮点数
- `GetBool(key string) (bool, error)` - 读取布尔值
- `Remove(key string) error` - 删除键
- `Contains(key string) (bool, error)` - 检查键是否存在
- `Subscribe(key string, callback func(string, any)) (int64, error)` - 订阅变更
- `Unsubscribe(subscriptionID int64) error` - 取消订阅
- `Publish(topic string, value any) error` - 发布事件

### RawClient

#### 方法

- `Put(key string, value any) error` - 存储值（无前缀）
- `Get(key string) (any, error)` - 读取值（无前缀）
- `Contains(key string) (bool, error)` - 检查存在（无前缀）
- `Remove(key string) error` - 删除键（无前缀）

## 使用示例

### 简单工作流

```go
package main

import (
    "fmt"
    "log"

    "github.com/agentflow/databus/clients/golang/databus"
)

func main() {
    client, err := databus.NewClientFromEnv()
    if err != nil {
        log.Fatal(err)
    }
    defer client.Close()

    // 存储输入
    client.User().Put("input", "process this data")

    // 读取框架配置
    config, _ := client.Sys().Get("config")
    fmt.Printf("配置: %v\n", config)

    // 存储结果
    result := map[string]any{
        "status": "completed",
        "data":   "processed",
    }
    client.User().Put("output", result)

    // 发送响应信号
    client.Sys().Put("response", "Done")
}
```

## 许可证

MIT License
