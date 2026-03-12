# DataBus 文件类型支持

本文档说明 AgentFlow 当前新增的 `FILE` / `FILE_LIST` 一等类型，以及它们在 Workflow DSL、DataBus 和节点 JSON I/O 中的使用方式。

**相关文档**: [DataBus Session 隔离机制](databus-session-isolation.md) - 详细说明 session 级别的数据隔离设计。

## 概述

AgentFlow 现在支持以下文件相关数据类型：

- `file`：单个文件描述符
- `file_list`：文件描述符列表

这里传递的是**文件描述符**，不是音频、图片、视频的原始二进制字节。推荐将文件内容保存在文件系统或对象存储中，通过 DataBus 传递引用和元数据。

## Session 隔离机制

DataBus 提供 session 级别的数据隔离，确保不同会话（对话/交互）的数据完全分离。

### Key 结构

所有 session 隔离的数据遵循以下 key 结构：

```
/session/{session-id}/{namespace}/{key}
```

### 命名空间

| Namespace | 用途 | 示例 Key |
|-----------|------|----------|
| `sys` | 框架消息通信（session 隔离） | `response`, `api_key` |
| `ctx` | 上下文存储（预留） | `history`, `state` |
| `llm` | LLM 服务交互（预留） | `last_prompt` |
| `tool` | 工具调用信息（预留） | `call_result` |
| `user` | 用户自定义 node 数据 | 任意 key |

### 使用方式

Node 代码通过 `INodeDataBusClient` 访问 session 隔离的 DataBus：

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs, INodeDataBusClient* client) {
  // 存储用户数据到 user namespace
  client->user()->put("my_result", SoDataVariant(std::string("hello")));

  // 从 sys namespace 读取框架配置
  SoDataVariant api_key;
  if (client->sys()->get("api_key", api_key)) {
    // 使用 API key...
  }
}
```

详细信息请参考 [databus-session-isolation.md](databus-session-isolation.md)。

## 文件描述符结构

`FILE` 的标准字段如下：

```json
{
  "file_id": "file-123",
  "uri": "file:///tmp/audio.wav",
  "storage_backend": "filesystem",
  "media_type": "audio/wav",
  "size_bytes": 4096,
  "checksum": "sha256:abc",
  "created_at": 1700000000,
  "ttl_seconds": 3600,
  "owner": "session-1",
  "metadata": {
    "lang": "zh"
  }
}
```

## Workflow DSL

节点输入输出现在可以显式声明文件类型：

```json
{
  "id": "file_node",
  "name": "File Node",
  "type": "TASK",
  "inputs": [
    {"name": "source", "type": "file", "required": true},
    {"name": "attachments", "type": "file_list", "transfer_mode": "ref"}
  ],
  "outputs": [
    {"name": "artifact", "type": "file"},
    {"name": "artifacts", "type": "file_list"}
  ]
}
```

## DataBus 行为

- `set/get/pub/sub` 现在可直接承载 `FILE` / `FILE_LIST`
- `store_with_ref/resolve_ref` 支持文件描述符引用
- `FileDataBusBridge` 支持文件描述符与文件描述符列表的落盘和事件轮询

## 节点 JSON I/O

当节点开启 `json_output`，或通过 `node_adapter` 读取/写出 JSON 文件时：

- 顶层 JSON 对象会自动识别 `FILE`
- 文件描述符数组会自动识别 `FILE_LIST`
- 其他不匹配文件描述符结构的对象/数组，仍按原始 JSON 字符串处理

## 设计边界

- 当前新增的是**文件引用类型**，不是大文件字节流直传
- 适合 Workflow 中多节点共享文件、媒体处理产物和外部对象存储引用
- 如果需要真正的上传、下载、分片、物化与生命周期管理，建议后续在文件后端层继续扩展

## 验证

本次实现已通过：

- `./build.sh build`
- `./build.sh test`
