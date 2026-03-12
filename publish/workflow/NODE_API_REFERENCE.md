# MMNode API 参考

**详细开发指南**: [NODE_DEVELOPMENT_GUIDE.md](NODE_DEVELOPMENT_GUIDE.md) - 包含框架与 Node 的通信接口、各执行模式开发方法和场景化示例。

## SO Library 函数签名

SO library 节点支持三种函数签名，框架按优先级加载：

### V3 签名（推荐）

Session 隔离的 DataBus 访问，推荐新节点使用：

```cpp
#include "agentflow/so_node.h"

extern "C" void my_node_v3(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBusClient* client) {

  // 存储用户数据到 user namespace
  client->user()->put("my_result", SoDataVariant(std::string("hello")));

  // 从 sys namespace 读取框架配置
  SoDataVariant api_key;
  if (client->sys()->get("api_key", api_key)) {
    // 使用 API key...
  }

  // 获取当前 session ID
  const std::string& session_id = client->session_id();
}
```

**DataBus Namespace 说明**：

| Namespace | 用途 | 示例 |
|-----------|------|------|
| `sys` | 框架消息通信（session 隔离） | `response`, `api_key` |
| `ctx` | 上下文存储（预留） | `history`, `state` |
| `llm` | LLM 服务交互（预留） | `last_prompt` |
| `tool` | 工具调用信息（预留） | `call_result` |
| `user` | 用户自定义 node 数据 | 任意 key |

**跨 Session 数据共享**：使用 `client->raw()` 获取原始 DataBus 访问，用于读取全局配置。

### V2 签名（向后兼容）

原始 DataBus 访问，无 session 隔离：

```cpp
extern "C" void my_node_v2(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBus* databus) {

  if (databus) {
    databus->put("my_key", SoDataVariant(std::string("value")));
  }
}
```

### V1 签名（基础）

仅输入输出，无 DataBus 访问：

```cpp
extern "C" void my_node(
    const SoDataMap& inputs,
    SoDataMap& outputs) {

  // 处理 inputs，填充 outputs
}
```

### 函数加载优先级

框架按以下顺序尝试加载函数符号：
1. `{function_name}_v3` - Session 隔离的 DataBus client
2. `{function_name}_v2` - 原始 DataBus 访问
3. `{function_name}` - 基础输入输出

## 运行时 API 约定

MMNode 实现应遵循以下运行时约定：

1. 调用输入包含基础元信息（如 `trace_id/workflow_id/node_id`）。
2. 输入输出数据通过 JSON `payload` 交互，并与 workflow 的 `inputs/outputs` 对齐。
3. 重试与超时由 workflow node 配置控制（`max_retries`、`retry_delay_ms`、`timeout_ms`）。

## 执行模式映射

- `shell_script`：通过 shell 执行命令
- `executable`：直接执行二进制
- `go_script`：执行 Go 脚本
- `python_script`：执行 Python 脚本
- `java_jar`：执行 Java JAR
- `so_library`：加载动态库并调用指定符号
- `cpp_function`：调用框架注册的 C++ 函数

以上执行模式统一由 Worker 执行引擎分发，MMNode 负责声明执行模式、命令、参数与 I/O 约束。
MMNode 热更新也由 Worker 处理，默认按 `enable_hot_reload/hot_reload_config` 生效，无需手工调用 enable 接口。

## 输出示例

```json
{
  "status": "SUCCESS",
  "payload": {
    "result": "ok"
  }
}
```

## Skill Workflow 输出约定

当 workflow 作为 skill task 执行时，应输出以下字段到 DataBus：

### 必需字段

| 字段名 | 类型 | 说明 |
|--------|------|------|
| `response` | string | skill 执行结果文本，最终返回给客户端 |

### 可选字段

| 字段名 | 类型 | 说明 |
|--------|------|------|
| `next_skills` | string | 后续要拉起的 skill 名称列表，格式为 JSON 数组或逗号分隔字符串 |
| `skill_completed` | bool | 表示 skill 是否完成，默认为 true |

### next_skills 格式

支持两种格式：

1. **JSON 数组格式**（推荐）：
```json
["search", "summary", "delegate"]
```

2. **逗号分隔字符串格式**：
```
search, summary, delegate
```

### 示例

Skill workflow 完成后，将结果写入 DataBus：

```json
{
  "response": "搜索完成，找到 5 个相关结果",
  "next_skills": "[\"summary\", \"delegate\"]",
  "skill_completed": true
}
```

### 处理流程

1. Skill workflow 执行完成
2. 框架检测 DataBus 中的 `next_skills` 字段
3. 解析 skill 名称列表
4. 检查并发限制（`max_concurrent_skills`）
5. 拉起对应的 skill task
6. 清除已处理的 `next_skills` 防止重复处理
