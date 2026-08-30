# AgentFlow 插件 UDS 接口规范

## 1. 概述

本文档定义了 AgentFlow 插件系统的 Unix Domain Socket (UDS) 通用接口规范。通过 UDS 接口，外部程序可以跨进程调用插件能力，实现与 LLMPlugin、RAGPlugin、ToolPlugin 的通信。

## 2. 接口设计原则

### 2.1 通信模型

- **传输协议**: Unix Domain Socket
- **数据格式**: JSON (UTF-8 编码)
- **通信模式**: 请求-响应模式 + 事件推送模式
- **消息边界**: 4字节长度前缀 + 消息体

### 2.2 消息格式

#### 请求消息
```json
{
  "request_id": "req_1234567890_abc",
  "plugin_type": "LLM|RAG|Tool",
  "plugin_name": "PluginName",
  "method": "method_name",
  "params": {
    "key": "value"
  },
  "timestamp": 1234567890
}
```

#### 响应消息
```json
{
  "request_id": "req_1234567890_abc",
  "status": "success|error",
  "error_code": 0,
  "error_message": "",
  "data": {
    "key": "value"
  },
  "timestamp": 1234567890
}
```

#### 事件消息 (流式/推送)
```json
{
  "event_id": "evt_1234567890_xyz",
  "event_type": "stream|notification",
  "channel": "llm.stream.task_id",
  "data": {
    "token": "<stream-content>"
  },
  "done": false,
  "timestamp": 1234567890
}
```

### 2.3 错误码定义

| 错误码 | 常量名 | 描述 |
|--------|--------|------|
| 0 | SUCCESS | 成功 |
| 1 | ERROR | 通用错误 |
| 2 | TIMEOUT | 请求超时 |
| 3 | INVALID_PARAMS | 参数无效 |
| 4 | PLUGIN_NOT_FOUND | 插件未找到 |
| 5 | METHOD_NOT_FOUND | 方法未找到 |
| 1001 | MM_AT_REQUEST_ERR | 请求AgentTool失败 |
| 1002 | MM_AT_RESPONSE_ERR | AgentTool响应错误 |
| 1003 | MM_AT_RESPONSE_BODY_ERR | AgentTool响应体解析失败 |
| 0x1000001 | EMBEDDING_TIMEOUT | Embedding超时 |
| 0x1000002 | EMBEDDING_FAIL | Embedding失败 |

---

## 3. 通用接口

### 3.1 插件管理接口

#### 3.1.1 获取插件列表

**方法**: `plugin.list`

**请求参数**:
```json
{
  "plugin_type": "LLM|RAG|Tool"  // 可选，不传则返回所有
}
```

**响应数据**:
```json
{
  "plugins": [
    {
      "name": "ExampleLLMPlugin",
      "type": "LLM",
      "tags": ["example", "mock"],
      "version": "1.0.0",
      "description": "Example LLM Plugin"
    }
  ]
}
```

#### 3.1.2 获取插件信息

**方法**: `plugin.info`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin"
}
```

**响应数据**:
```json
{
  "name": "ExampleLLMPlugin",
  "type": "LLM",
  "tags": ["example", "mock"],
  "version": "1.0.0",
  "description": "Example LLM Plugin",
  "capabilities": ["complete", "stream", "context"]
}
```

#### 3.1.3 初始化插件

**方法**: `plugin.init`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "config_path": "/path/to/config",
  "log_config": {
    "model_id": "llm_001",
    "level_debug": 1,
    "level_info": 1,
    "level_warn": 1,
    "level_error": 1,
    "level_fatal": 1
  }
}
```

**响应数据**:
```json
{
  "initialized": true
}
```

---

## 4. LLMPlugin 接口

### 4.1 非流式推理

#### 4.1.1 单次推理请求

**方法**: `llm.complete`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "model": {
    "service_name": "llm-service",
    "model_name": "gpt-3.5-turbo",
    "temperature": 0.7,
    "top_p": 0.9,
    "context_max_length": 8192,
    "use_third": false
  },
  "request": {
    "intentions": [
      {
        "query": "什么是人工智能?",
        "modal_type": "text"
      }
    ],
    "prompt": [],
    "tools": []
  },
  "timeout_ms": 60000
}
```

**响应数据**:
```json
{
  "response": "人工智能是...",
  "model": "gpt-3.5-turbo",
  "usage": {
    "prompt_tokens": 10,
    "completion_tokens": 100,
    "total_tokens": 110
  },
  "finish_reason": "stop"
}
```

### 4.2 流式推理

#### 4.2.1 创建流式任务

**方法**: `llm.task.create`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "model": {
    "model_name": "gpt-3.5-turbo",
    "temperature": 0.7,
    "top_p": 0.9
  },
  "callback_channel": "llm.stream.callback"
}
```

**响应数据**:
```json
{
  "task_id": "task_1234567890_abc",
  "created": true
}
```

#### 4.2.2 流式请求

**方法**: `llm.stream`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "task_id": "task_1234567890_abc",
  "request": {
    "intentions": [
      {
        "query": "请写一首关于春天的诗",
        "modal_type": "text"
      }
    ]
  }
}
```

**响应数据** (立即返回):
```json
{
  "task_id": "task_1234567890_abc",
  "status": "streaming"
}
```

**事件推送** (通过订阅的 channel):
```json
// OnLLMStart
{
  "event_type": "llm_start",
  "task_id": "task_1234567890_abc",
  "request_id": "req_xxx"
}

// OnLLMNewToken
{
  "event_type": "llm_token",
  "task_id": "task_1234567890_abc",
  "token": "<stream-chunk>"
}

// OnLLMFinalAnswer
{
  "event_type": "llm_final",
  "task_id": "task_1234567890_abc",
  "answer": "春风吹绿柳树枝..."
}

// OnLLMEnd
{
  "event_type": "llm_end",
  "task_id": "task_1234567890_abc"
}

// OnLLMError
{
  "event_type": "llm_error",
  "task_id": "task_1234567890_abc",
  "error_code": 1001,
  "error_message": "Connection timeout"
}
```

#### 4.2.3 销毁流式任务

**方法**: `llm.task.destroy`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "task_id": "task_1234567890_abc"
}
```

**响应数据**:
```json
{
  "task_id": "task_1234567890_abc",
  "destroyed": true
}
```

### 4.3 上下文管理

#### 4.3.1 中断流式请求

**方法**: `llm.interrupt`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "task_id": "task_1234567890_abc"
}
```

**响应数据**:
```json
{
  "interrupted": true
}
```

#### 4.3.2 获取用户上下文

**方法**: `llm.context.get`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "task_id": "task_1234567890_abc"
}
```

**响应数据**:
```json
{
  "context": [
    {
      "role": "user",
      "content": "你好"
    },
    {
      "role": "assistant",
      "content": "你好！有什么可以帮助你的？"
    }
  ]
}
```

#### 4.3.3 更新用户上下文

**方法**: `llm.context.update`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "task_id": "task_1234567890_abc",
  "context": [
    {
      "role": "user",
      "content": "新问题"
    }
  ]
}
```

**响应数据**:
```json
{
  "updated": true
}
```

#### 4.3.4 追加用户上下文

**方法**: `llm.context.add`

**请求参数**:
```json
{
  "plugin_name": "ExampleLLMPlugin",
  "task_id": "task_1234567890_abc",
  "context": [
    {
      "role": "user",
      "content": "追加的消息"
    }
  ]
}
```

**响应数据**:
```json
{
  "added": true
}
```

---

## 5. ToolPlugin 接口

### 5.1 工具管理

#### 5.1.1 列出工具

**方法**: `tool.list`

**请求参数**:
```json
{
  "plugin_name": "MyToolPlugin"
}
```

**响应数据**:
```json
{
  "tools": [
    {
      "name": "weather",
      "description": "查询天气信息",
      "input_schema": {
        "type": "object",
        "properties": {
          "city": {
            "type": "string",
            "description": "城市名称"
          }
        },
        "required": ["city"]
      },
      "output_schema": {
        "type": "object",
        "properties": {
          "temperature": {"type": "number"},
          "condition": {"type": "string"},
          "humidity": {"type": "number"},
          "city": {"type": "string"}
        }
      }
    }
  ]
}
```

#### 5.1.2 调用工具

**方法**: `tool.call`

**请求参数**:
```json
{
  "plugin_name": "MyToolPlugin",
  "tool_name": "weather",
  "params": {
    "city": "beijing"
  }
}
```

**响应数据**:
```json
{
  "content": [
    {
      "type": "text",
      "text": {
        "temperature": 25,
        "condition": "晴",
        "humidity": 60,
        "city": "beijing"
      }
    }
  ],
  "error_code": 0
}
```

---

## 6. RAGPlugin 接口

### 6.1 Embedding 接口

#### 6.1.1 创建 Embedding 模型

**方法**: `rag.embedding.create`

**请求参数**:
```json
{
  "plugin_name": "MyRAGPlugin",
  "model_option": {
    "model_path": "/model/embedding/gte-small-zh/model.mindir",
    "model_name": "gte-small-zh",
    "dimension": 512
  },
  "vocab_option": {
    "vocab_path": "/model/embedding/gte-small-zh/vocab.txt",
    "max_seq_len": 512
  },
  "inference_option": {
    "xpu_type": "CPU",
    "backend_type": "MINDSPORECLOUD",
    "config_path": "/config/product.json",
    "thread_num": 1,
    "instance_num": 1
  },
  "pattern": "LOCAL"
}
```

**响应数据**:
```json
{
  "embedding_id": "emb_1234567890_abc",
  "dimension": 512,
  "created": true
}
```

#### 6.1.2 单文本向量化

**方法**: `rag.embedding.vectorize`

**请求参数**:
```json
{
  "plugin_name": "MyRAGPlugin",
  "embedding_id": "emb_1234567890_abc",
  "text": "这是一段需要向量化的文本",
  "option": {
    "timeout_ms": 10000,
    "padding_type": "NONE"
  }
}
```

**响应数据**:
```json
{
  "embedding": [0.1, 0.2, 0.3, "..."],
  "token_num": 15
}
```

#### 6.1.3 批量文本向量化

**方法**: `rag.embedding.batch_vectorize`

**请求参数**:
```json
{
  "plugin_name": "MyRAGPlugin",
  "embedding_id": "emb_1234567890_abc",
  "texts": [
    "第一段文本",
    "第二段文本",
    "第三段文本"
  ],
  "option": {
    "timeout_ms": 30000,
    "padding_type": "BATCH_MAX"
  }
}
```

**响应数据**:
```json
{
  "results": [
    {
      "embedding": [0.1, 0.2, "..."],
      "token_num": 10
    },
    {
      "embedding": [0.3, 0.4, "..."],
      "token_num": 12
    },
    {
      "embedding": [0.5, 0.6, "..."],
      "token_num": 8
    }
  ]
}
```

### 6.2 数据源接口

#### 6.2.1 创建数据源

**方法**: `rag.datasource.create`

**请求参数**:
```json
{
  "plugin_name": "MyRAGPlugin",
  "app_id": 1001,
  "app_name": "MyApplication",
  "db_name": "knowledge_base",
  "config": {
    "max_conn_num": 40,
    "init_conn_num": 5,
    "idle_conn_num": 3,
    "sweep_interval_sec": 30,
    "max_idle_timeout_sec": 90,
    "test_on_borrow": true
  }
}
```

**响应数据**:
```json
{
  "datasource_id": "ds_1234567890_abc",
  "connected": true
}
```

### 6.3 重排器接口

#### 6.3.1 创建重排器

**方法**: `rag.rerank.create`

**请求参数**:
```json
{
  "plugin_name": "MyRAGPlugin",
  "model_name": "bge-reranker",
  "pattern": "CROSSED_ENCODER"
}
```

**响应数据**:
```json
{
  "rerank_id": "rerank_1234567890_abc",
  "created": true
}
```

### 6.4 检索器接口

#### 6.4.1 创建检索器

**方法**: `rag.retriever.create`

**请求参数**:
```json
{
  "plugin_name": "MyRAGPlugin",
  "datasource_id": "ds_1234567890_abc",
  "embedding_id": "emb_1234567890_abc",
  "rerank_id": "rerank_1234567890_abc"  // 可选
}
```

**响应数据**:
```json
{
  "retriever_id": "ret_1234567890_abc",
  "created": true
}
```

#### 6.4.2 执行检索

**方法**: `rag.retrieve`

**请求参数**:
```json
{
  "plugin_name": "MyRAGPlugin",
  "retriever_id": "ret_1234567890_abc",
  "query": "什么是机器学习?",
  "top_k": 5
}
```

**响应数据**:
```json
{
  "results": [
    {
      "text": "机器学习是人工智能的一个分支...",
      "score": 0.95,
      "metadata": {
        "source": "wiki",
        "doc_id": "doc_001"
      }
    },
    {
      "text": "机器学习算法可以分为监督学习...",
      "score": 0.89,
      "metadata": {
        "source": "textbook",
        "doc_id": "doc_002"
      }
    }
  ]
}
```

---

## 7. 订阅与事件

### 7.1 订阅事件

**方法**: `event.subscribe`

**请求参数**:
```json
{
  "channels": [
    "llm.stream.task_123",
    "tool.notification"
  ]
}
```

**响应数据**:
```json
{
  "subscribed": true,
  "subscription_id": "sub_1234567890"
}
```

### 7.2 取消订阅

**方法**: `event.unsubscribe`

**请求参数**:
```json
{
  "subscription_id": "sub_1234567890"
}
```

**响应数据**:
```json
{
  "unsubscribed": true
}
```

### 7.3 支持的事件类型

| 事件类型 | Channel 模式 | 描述 |
|----------|-------------|------|
| llm_start | `llm.stream.{task_id}` | LLM 流式开始 |
| llm_token | `llm.stream.{task_id}` | LLM 流式 Token |
| llm_final | `llm.stream.{task_id}` | LLM 最终答案 |
| llm_end | `llm.stream.{task_id}` | LLM 流式结束 |
| llm_error | `llm.stream.{task_id}` | LLM 错误 |
| tool_notification | `tool.notification` | 工具通知 |

---

## 8. UDS 连接管理

### 8.1 Socket 路径

默认路径: `/var/run/agentflow/plugin.sock`

可通过环境变量配置: `AGENTFLOW_UDS_PATH`

### 8.2 连接流程

```
客户端                          服务端
   |                              |
   |---- connect() -------------->|
   |                              |
   |---- 请求消息 --------------->|
   |<--- 响应消息 ----------------|
   |                              |
   |---- subscribe() ------------>|
   |<--- 事件推送 ----------------|
   |                              |
   |---- close() ---------------->|
```

### 8.3 心跳机制

客户端可定期发送心跳请求保持连接:

**方法**: `heartbeat`

**请求参数**:
```json
{}
```

**响应数据**:
```json
{
  "pong": true,
  "server_time": 1234567890
}
```

建议心跳间隔: 30秒

---

## 9. 安全性

### 9.1 权限控制

Socket 文件权限应设置为 0660，仅允许特定用户组访问。

### 9.2 请求验证

服务端可配置请求验证机制:
- 基于进程 PID 的访问控制
- 基于 Token 的认证（可选）

---

## 10. 性能考虑

### 10.1 连接复用

建议客户端复用 UDS 连接，避免频繁创建/销毁连接。

### 10.2 批量操作

对于 Embedding 等支持批量的操作，应使用批量接口提高效率。

### 10.3 异步处理

流式接口应使用事件订阅模式，避免同步阻塞。

---

## 附录 A: 数据类型定义

### A.1 MMReasonParam (LLM 推理参数)

```json
{
  "model_service_name": "string",    // 模型服务名称
  "model_instance_id": "string",     // 模型实例ID (已废弃)
  "model_name": "string",            // 模型名称
  "temperature": 0.7,                // 温度参数
  "top_p": 0.9,                      // Top-P 参数
  "lora_id": "string",               // 微调模型ID (可选)
  "context_max_length": 8192,        // 上下文最大长度
  "sched_group": 0,                  // 调度组标识
  "use_third": false                 // 是否使用第三方模型
}
```

### A.2 MMLLMRequest (LLM 请求)

```json
{
  "intentions": [
    {
      "query": "string",
      "modal_type": "text|audio|image"
    }
  ],
  "prompt": [
    {
      "key": {
        "name": "string",
        "owner": "string",
        "language": "zh",
        "version": "",
        "namespace": "common"
      },
      "content": "string"
    }
  ],
  "tools": [
    {
      "tools_type": "function",
      "tools_function": {
        "function_name": "string",
        "strict": false,
        "function_description": "string",
        "parameters": {
          "parameters_type": "object",
          "properties": {},
          "required_list": [],
          "additional_properties": false
        }
      }
    }
  ]
}
```

### A.3 DataSource::Config (数据源配置)

```json
{
  "max_conn_num": 40,           // 最大连接数 (0-800]
  "init_conn_num": 5,           // 初始连接数 (0-800]
  "idle_conn_num": 3,           // 空闲连接数 (0-800]
  "sweep_interval_sec": 30,     // 回收间隔 (0-300]
  "max_idle_timeout_sec": 90,   // 最大空闲时间 (0-300]
  "test_on_borrow": true        // 获取时检查
}
```

### A.4 EmbeddingOption (Embedding 选项)

```json
{
  "timeout_ms": 10000,
  "padding_type": "NONE|MAX|BATCH_MAX"
}
```

---

## 附录 B: 错误处理示例

### B.1 插件未找到

```json
{
  "request_id": "req_xxx",
  "status": "error",
  "error_code": 4,
  "error_message": "Plugin 'UnknownPlugin' not found"
}
```

### B.2 参数无效

```json
{
  "request_id": "req_xxx",
  "status": "error",
  "error_code": 3,
  "error_message": "Invalid parameter: 'model_name' is required"
}
```

### B.3 超时

```json
{
  "request_id": "req_xxx",
  "status": "error",
  "error_code": 2,
  "error_message": "Request timeout after 60000ms"
}
```