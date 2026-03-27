# AgentFlow DataBus Python SDK

通过 Unix 域套接字实时访问 DataBus 的纯 Python SDK。

## 安装

将 `agentflow_databus.py` 复制到项目目录或 Python 路径中。

```bash
cp agentflow_databus.py /path/to/your/project/
```

## 快速上手

```python
from agentflow_databus import DataBusClient

# 通过环境变量连接（推荐）
client = DataBusClient.from_env()

# 或显式指定 socket 路径
client = DataBusClient("/tmp/agentflow_session_xxx.sock")
```

## 链式调用 API

SDK 提供与 C++ INodeDataBusClient 接口一致的链式调用 API：

```python
# 访问各命名空间
client.sys().put("response", "Hello World")      # 框架通信
client.ctx().put("history", [...])               # 上下文存储
client.llm().put("prompt", "...")                # LLM 交互
client.tool().put("call_result", {...})          # 工具调用
client.user().put("my_data", {...})              # 用户自定义数据

# 读取值
value = client.user().get("my_key", default=None)
exists = client.user().contains("my_key")
client.user().remove("my_key")
```

## 对话隔离

多对话会话中，通过 dialogue_id 实现数据隔离：

```python
# 存储特定对话的数据
client.user("dialogue-123").put("skill_result", {"status": "ok"})

# 读取特定对话的数据
result = client.user("dialogue-123").get("skill_result")
```

## 订阅

订阅实时数据变更：

```python
def on_change(key, value):
    print(f"Key {key} changed to {value}")

# 订阅
sub_id = client.user().subscribe("my_key", on_change)

# 取消订阅
client.user().unsubscribe(sub_id)
```

## 原始访问

绕过 session 前缀直接访问全局键：

```python
# 原始访问，不添加 session 前缀
client.raw().put("/global/config", "value")
value = client.raw().get("/global/config")
```

## 上下文管理器

使用 `with` 语句自动清理资源：

```python
with DataBusClient.from_env() as client:
    client.user().put("key", "value")
    value = client.user().get("key")
# 连接自动关闭
```

## 错误处理

```python
from agentflow_databus import DataBusClient, DataBusError, JSONRPCError

try:
    client = DataBusClient.from_env()
    value = client.user().get("missing_key")
except DataBusError as e:
    print(f"连接错误: {e}")
except JSONRPCError as e:
    print(f"协议错误 {e.code}: {e.message}")
```

## 环境变量

| 变量名 | 说明 |
|--------|------|
| `AF_DATABUS_UDS_FILE` | Unix 域套接字路径 |
| `AF_SESSION_ID` | 当前会话 ID（异步 SDK 使用） |
| `AF_DIALOGUE_ID` | 当前对话 ID（异步 SDK 使用） |
| `AF_SELF_TASK_ID` | 当前任务 ID（异步 SDK 使用） |
| `AF_NODE_ID` | 当前节点 ID（异步 SDK 使用） |

## API 参考

### DataBusClient

#### 类方法

- `from_env()` - 通过 `AF_DATABUS_UDS_FILE` 环境变量创建客户端

#### 实例方法

- `sys(dialogue_id=None)` - 访问 sys 命名空间
- `ctx(dialogue_id=None)` - 访问 ctx 命名空间
- `llm(dialogue_id=None)` - 访问 llm 命名空间
- `tool(dialogue_id=None)` - 访问 tool 命名空间
- `user(dialogue_id=None)` - 访问 user 命名空间
- `raw()` - 原始访问（无前缀）
- `async_sdk()` - 获取异步回调 SDK
- `close()` - 关闭连接

#### 属性

- `session_id` - 当前会话 ID
- `socket_path` - Unix 域套接字路径

### NamespaceClient

#### 方法

- `put(key, value)` - 存储值
- `get(key, default=None)` - 读取值
- `remove(key)` - 删除键
- `contains(key)` - 检查键是否存在
- `subscribe(key, callback)` - 订阅数据变更
- `unsubscribe(subscription_id)` - 取消订阅
- `publish(topic, value)` - 发布事件

### AsyncCallSDK

用于多语言节点（Python/Shell/Go）发送异步回调消息。

#### 获取实例

```python
async_sdk = client.async_sdk()
```

#### 方法

- `send_to_self(call_key, content)` - 发送异步消息到当前任务（短路路由，低延迟）
- `send_msg(target_task_id, target_node_id, call_key, content)` - 发送异步消息到指定任务

#### 属性

- `session_id` - 当前会话 ID
- `dialogue_id` - 当前对话 ID
- `task_id` - 当前任务 ID
- `node_id` - 当前节点 ID

#### 使用示例

```python
from agentflow_databus import DataBusClient

def main():
    with DataBusClient.from_env() as client:
        async_sdk = client.async_sdk()

        # 发送消息到当前任务
        success = async_sdk.send_to_self(
            call_key="callback_event",
            content='{"status": "completed", "result": "ok"}'
        )

        # 发送消息到其他任务
        success = async_sdk.send_msg(
            target_task_id="task-456",
            target_node_id="node-789",
            call_key="cross_task_event",
            content='{"action": "notify"}'
        )

if __name__ == "__main__":
    main()
```

## 使用示例

### 简单工作流

```python
from agentflow_databus import DataBusClient

def main():
    with DataBusClient.from_env() as client:
        # 存储输入
        client.user().put("input", "process this data")

        # 读取框架配置
        config = client.sys().get("config", {})

        # 存储结果
        result = {"status": "completed", "data": "processed"}
        client.user().put("output", result)

        # 发送响应信号
        client.sys().put("response", "Done")

if __name__ == "__main__":
    main()
```

### 响应式模式（订阅）

```python
import time
from agentflow_databus import DataBusClient

def main():
    with DataBusClient.from_env() as client:

        def on_input(key, value):
            print(f"收到输入: {value}")
            result = f"Processed: {value}"
            client.user().put("output", result)

        sub_id = client.user().subscribe("input", on_input)

        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            client.user().unsubscribe(sub_id)

if __name__ == "__main__":
    main()
```

## 许可证

MIT License
