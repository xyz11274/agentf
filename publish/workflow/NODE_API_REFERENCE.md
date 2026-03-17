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

### Plugin UDS 访问

SO Library 节点可通过环境变量获取插件 UDS 路径，用于与外部插件服务通信：

```cpp
#include <cstdlib>

extern "C" void my_node_v3(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBusClient* client) {

  // 通过环境变量获取插件 UDS 路径
  const char* plugin_uds = std::getenv("AF_PLUGIN_UDS_FILE");
  if(plugin_uds) {
    // 连接插件服务...
    // int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    // struct sockaddr_un addr;
    // addr.sun_family = AF_UNIX;
    // strncpy(addr.sun_path, plugin_uds, sizeof(addr.sun_path) - 1);
    // connect(fd, (struct sockaddr*)&addr, sizeof(addr));
  }
}
```

**环境变量说明**：

| 变量名 | 说明 |
|--------|------|
| `AF_PLUGIN_UDS_FILE` | 插件 UDS 路径，由 Runtime 配置的 `plugin_uds_path` 注入 |
| `AF_DATABUS_UDS_FILE` | DataBus UDS 路径，用于实时 DataBus 访问 |

**配置方式**：在 Runtime 初始化时设置：

```cpp
runtime::MMAgentRuntime::Config config;
config.plugin_uds_path = "/var/run/my_plugin.sock";
```

详见 [INTEGRATION_GUIDE.md](../INTEGRATION_GUIDE.md#55-plugin-uds-注入机制)。

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
| `available_parameters` | string | 当前可用的参数值，JSON 对象格式，用于 skill 参数校验 |
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

## Skill 参数校验

### 概述

框架支持在 skill 启动前校验其所需的参数是否可用。Skill 在 `agent.json` 中通过 `expected_parameters` 声明所需参数，Workflow 在输出 `next_skills` 时同时输出 `available_parameters` 提供参数值。

### available_parameters 格式

`available_parameters` 为 JSON 对象格式，结构如下：

```json
{
  "skill_name_1": {
    "param_1": "value_1",
    "param_2": "value_2"
  },
  "skill_name_2": {
    "param_a": "value_a"
  }
}
```

**说明**：
- 外层 key 为 skill 名称
- 内层 key-value 为该 skill 的参数名和参数值
- 参数值仅支持 string 类型

### 输出方式

`available_parameters` 支持两种输出方式：

**方式一：消息 metadata（推荐）**

在 workflow 完成消息的 metadata 中设置：

```cpp
// SO Library Node 中设置
outputs["available_parameters"] = SoDataVariant(std::string(R"({
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20"
  }
})"));
```

**方式二：DataBus 存储（向后兼容）**

写入 DataBus 的 sys namespace：

```cpp
extern "C" void my_node_v3(const SoDataMap& inputs, SoDataMap& outputs,
                           INodeDataBusClient* client) {
    std::string dialogue_id = agentflow::so_node::get_string(inputs, "dialogue_id", "");

    // 构建可用参数
    std::string params = R"({
        "travel": {
            "destination": "Beijing",
            "date": "2024-03-20"
        }
    })";

    // 写入 DataBus
    std::string key = dialogue_id.empty() ? "available_parameters" : dialogue_id + "/available_parameters";
    client->sys()->put(key, SoDataVariant(params));

    // 设置 next_skills
    outputs["next_skills"] = SoDataVariant(std::string("[\"travel\"]"));
}
```

### 优先级策略

框架按以下优先级读取 `available_parameters`：

1. **消息 metadata**：优先从 workflow 完成消息的 `metadata["available_parameters"]` 读取
2. **DataBus**：若 metadata 不存在，从 DataBus 的 `{dialogue_id}/available_parameters` 读取

### 校验流程

```
┌─────────────────────────────────────────────────────────────────┐
│                   Skill Parameter Validation                    │
├─────────────────────────────────────────────────────────────────┤
│  1. 检查 skill 的 expected_parameters 配置                      │
│  2. 若为空，跳过校验，直接启动 skill                             │
│  3. 获取 available_parameters（metadata 优先）                  │
│  4. 校验所有 expected_parameters 是否存在                       │
│  5. 校验通过 → 启动 skill task                                  │
│  6. 校验失败 → 跳过 skill，发送失败消息给 coordinator            │
└─────────────────────────────────────────────────────────────────┘
```

### 失败消息格式

参数校验失败时，框架向 coordinator 发送消息：

```json
{
  "content": "skill_launch_failed",
  "metadata": {
    "task_type": "skill_response",
    "message_type": "skill_launch_failed",
    "skill_name": "travel",
    "failure_reason": "parameter_validation_failed",
    "expected_parameters": "[\"destination\", \"date\", \"travelers\"]",
    "available_parameters": "{\"destination\": \"Beijing\", \"date\": \"2024-03-20\"}",
    "missing_parameters": "[\"travelers\"]"
  }
}
```

### 完整示例

Dispatcher workflow 完成时输出 `next_skills` 和 `available_parameters`：

```cpp
extern "C" void dispatcher_output_v3(const SoDataMap& inputs, SoDataMap& outputs,
                                     INodeDataBusClient* client) {
    std::string dialogue_id = agentflow::so_node::get_string(inputs, "dialogue_id", "");

    // 解析用户意图，确定目标 skill 和可用参数
    std::string intent = parse_user_intent(inputs);
    std::string destination = extract_destination(inputs);
    std::string date = extract_date(inputs);

    // 设置后续 skill
    outputs["next_skills"] = SoDataVariant(std::string("[\"travel\"]"));

    // 设置可用参数
    std::string available_params = "{\"travel\": {\"destination\": \"" + destination +
                                   "\", \"date\": \"" + date + "\"}}";
    outputs["available_parameters"] = SoDataVariant(available_params);

    // 同时写入 DataBus（向后兼容）
    std::string params_key = dialogue_id.empty() ? "available_parameters"
                                                  : dialogue_id + "/available_parameters";
    client->sys()->put(params_key, SoDataVariant(available_params));
}
```

### 向后兼容性

- `expected_parameters` 为可选配置，未配置时跳过校验
- `available_parameters` 为可选输出，未提供时视为空对象
- 现有 workflow 无需修改即可继续工作
