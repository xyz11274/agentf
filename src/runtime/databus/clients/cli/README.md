# DataBus CLI 工具

通过 Unix 域套接字访问 DataBus 的命令行工具。

## 安装

将 `databus_cli.py` 复制到 PATH 路径或直接使用：

```bash
chmod +x databus_cli.py
./databus_cli.py --help
```

## 快速上手

```bash
# 写入值（使用 AF_DATABUS_UDS_FILE 环境变量）
databus-cli -ns user put "my_key" "my_value"

# 读取值
databus-cli -ns user get "my_key"

# 显式指定 socket 和 session
databus-cli -S /tmp/agentflow_session_xxx.sock -ns user get "my_key"
```

## 命令说明

### put

将值存储到 DataBus。

```bash
databus-cli -ns user put "key" "value"
databus-cli -ns user put "config" '{"setting": "value"}'
```

### get

从 DataBus 读取值。

```bash
databus-cli -ns user get "key"
databus-cli -ns user --raw get "key"  # 原始输出（不格式化 JSON）
```

### remove

从 DataBus 删除键。

```bash
databus-cli -ns user remove "key"
```

### contains

检查键是否存在。

```bash
databus-cli -ns user contains "key"
# 退出码: 0 表示存在，1 表示不存在
```

### list

列出命名空间中的键（功能有限）。

```bash
databus-cli -ns user list
```

### publish

向指定 topic 发布事件。

```bash
databus-cli -ns sys publish "topic.event" "payload"
```

### subscribe

订阅键变更（阻塞模式）。

```bash
databus-cli -ns user subscribe "key"
# 按 Ctrl+C 停止
```

## 选项说明

### 全局选项

| 选项 | 说明 |
|------|------|
| `-S, --socket` | Unix 域套接字路径（或设置 `AF_DATABUS_UDS_FILE` 环境变量） |
| `-s, --session` | 会话 ID |
| `-d, --dialogue` | 对话 ID，用于对话隔离访问 |
| `-ns, --namespace` | 命名空间：sys、ctx、llm、tool、user（默认：user） |
| `-q, --quiet` | 抑制非必要输出 |
| `--raw` | 输出原始值，不格式化 |

## 使用示例

### 基本操作

```bash
# 写入简单值
databus-cli -ns user put "name" "Alice"

# 读取值
databus-cli -ns user get "name"
# 输出: "Alice"

# 检查存在
databus-cli -ns user contains "name"
# 输出: exists: name

# 删除键
databus-cli -ns user remove "name"
```

### JSON 值操作

```bash
# 存储 JSON 值
databus-cli -ns user put "config" '{"debug": true, "level": 5}'

# 格式化输出
databus-cli -ns user get "config"
# 输出:
# {
#   "debug": true,
#   "level": 5
# }

# 原始输出
databus-cli -ns user --raw get "config"
# 输出: map[debug:true level:5]
```

### 会话与对话隔离

```bash
# 显式指定 session ID
databus-cli -s "session-abc123" -ns user put "data" "value"

# 指定 dialogue ID 实现多对话隔离
databus-cli -s "session-abc123" -d "dialogue-456" -ns user put "result" "done"
```

### 命名空间使用

```bash
# sys 命名空间（框架通信）
databus-cli -ns sys put "response" "Hello World"

# llm 命名空间
databus-cli -ns llm put "prompt" "What is AI?"

# tool 命名空间
databus-cli -ns tool put "call_result" '{"status": "success"}'
```

### 订阅变更

```bash
# 订阅键变更
databus-cli -ns user subscribe "status"
# 输出:
# Subscribed to status (subscription_id: 1)
# Press Ctrl+C to stop...
# [2024-01-15 10:30:00] status: "processing"
# [2024-01-15 10:30:05] status: "completed"
```

### Shell 脚本集成

```bash
#!/bin/bash

# 写入输入数据
databus-cli -ns user put "input" "process this"

# 等待处理结果
while true; do
    status=$(databus-cli -ns user --raw get "status" 2>/dev/null)
    if [ "$status" = "completed" ]; then
        break
    fi
    sleep 1
done

# 读取结果
result=$(databus-cli -ns user --raw get "output")
echo "Result: $result"
```

### 错误处理

在脚本中使用退出码判断：

```bash
# 检查键是否存在
if databus-cli -ns user contains "config" 2>/dev/null; then
    echo "配置存在"
else
    echo "配置不存在"
fi

# 处理 get 失败
if ! databus-cli -ns user get "missing_key" 2>/dev/null; then
    echo "键不存在，使用默认值"
fi
```

## 环境变量

| 变量名 | 说明 |
|--------|------|
| `AF_DATABUS_UDS_FILE` | Unix 域套接字路径 |

## 退出码

| 退出码 | 说明 |
|--------|------|
| 0 | 成功 |
| 1 | 错误（键不存在、操作失败等） |

## 许可证

MIT License
