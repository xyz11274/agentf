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

### AsyncCall SDK（异步回调）

V3 签名的 `INodeDataBusClient` 提供 `async()` 方法获取异步调用 SDK，用于发起异步操作并通过回调接收结果。

#### 接口定义

```cpp
class IAsyncCallSDK {
public:
  virtual ~IAsyncCallSDK() = default;

  // 发起异步调用，async_func 在独立线程执行
  virtual bool AsyncCall(const std::string& key,
                         std::function<void()> async_func) = 0;

  // 发送消息给自己（短路路由，直接入队到当前 task）
  virtual bool SendToSelf(const std::string& key,
                          const std::string& msg) = 0;

  // 发送消息给指定 node（跨 task）
  virtual bool SendMsg(const std::string& session_id,
                       const std::string& dialogue_id,
                       const std::string& target_task_id,
                       const std::string& target_node_id,
                       const std::string& key,
                       const std::string& msg) = 0;
};
```

#### 使用示例：流式 LLM 调用

```cpp
#include "agentflow/so_node.h"
#include <thread>

extern "C" void llm_stream_node_v3(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBusClient* client) {

  std::string session_id = so_node::get_string(inputs, "session_id", "");
  std::string dialogue_id = so_node::get_string(inputs, "dialogue_id", "");
  std::string prompt = so_node::get_string(inputs, "prompt", "");

  // 获取 AsyncCall SDK
  auto async_sdk = client->async();

  // 发起异步 LLM 调用
  async_sdk->AsyncCall("llm-stream", [&]() {
    // 在独立线程中执行流式请求
    StreamingLLMClient llm;
    llm.stream(prompt, [&](const std::string& chunk, bool done) {
      // 每个 chunk 发送给自己
      nlohmann::json msg;
      msg["chunk"] = chunk;
      msg["done"] = done;
      async_sdk->SendToSelf("llm-stream", msg.dump());

      // 如果需要转发给 coordinator（流式输出场景）
      if (!session_id.empty()) {
        std::string coord_task = session_id + "_coordinator";
        async_sdk->SendMsg(session_id, dialogue_id, coord_task,
                          "coordinator", "llm-stream", msg.dump());
      }
    });
  });

  // 设置异步 pending 状态
  outputs["__async_pending__"] = SoDataVariant(std::string("true"));
  outputs["__async_key__"] = SoDataVariant(std::string("llm-stream"));
}
```

#### 异步回调处理流程

```
node.execute()
  │
  ├── async()->AsyncCall("key", [&]() {
  │     // 独立线程执行耗时操作
  │     streaming_request(...);
  │     async()->SendToSelf("key", chunk);  // 每个 chunk 回调
  │   })
  │
  ├── outputs["__async_pending__"] = "true"
  └── return  // 函数返回，但 workflow 等待异步完成

异步回调到达:
  │
  ├── 消息入队到 task.data_exchange_queue
  │
  ├── node 收到回调消息，inputs 包含：
  │   - "__async_callback__": "true"
  │   - "__async_key__": "llm-stream"
  │   - "__async_msg__": chunk 内容
  │
  └── 处理 chunk，累积或转发
```

#### 框架注入字段

异步回调时，框架会向 node inputs 注入以下字段：

| 字段名 | 类型 | 说明 |
|--------|------|------|
| `__async_callback__` | string | "true" 表示这是一次异步回调 |
| `__async_key__` | string | 对应 AsyncCall 时指定的 key |
| `__async_msg__` | string | 回调消息内容 |

**判断是否为回调**：

```cpp
bool is_callback = false;
if (inputs.count("__async_callback__")) {
  auto val = std::get_if<std::string>(&inputs.at("__async_callback__"));
  is_callback = val && *val == "true";
}
```

#### 线程安全说明

- `SendToSelf` 和 `SendMsg` 是线程安全的，可在异步线程中调用
- 回调消息直接入队到 task 消息队列，跳过 agent 路由层
- `session_id` 和 `dialogue_id` 从 node inputs 获取，不从 SDK 获取

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

## 框架注入字段

除了用户定义的输入参数，框架还会自动注入以下字段到 node inputs：

| 字段名 | 类型 | 说明 |
|--------|------|------|
| `self_task_id` | string | 当前 task 的唯一标识 |
| `session_id` | string | 当前 session 的唯一标识 |
| `dialogue_id` | string | 当前 dialogue 的唯一标识（如适用） |
| `exec_skill_key` | string | skill 专属数据源 key（如配置） |

**访问方式**：

```cpp
// C++ SO Node
std::string task_id;
if(inputs.count("self_task_id")) {
    task_id = std::get<std::string>(inputs["self_task_id"]);
}
```

```bash
# Shell Node - 通过环境变量访问
echo "Task ID: $AF_SELF_TASK_ID"
echo "Session ID: $AF_SESSION_ID"
```

```python
# Python Node - 通过环境变量访问
import os
task_id = os.environ.get("AF_SELF_TASK_ID", "")
session_id = os.environ.get("AF_SESSION_ID", "")
```

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

支持多种格式：

1. **JSON 字符串数组格式**（旧格式，向后兼容）：
```json
["search", "summary", "delegate"]
```

2. **JSON 对象数组格式**（新格式，支持 exec_skill_key）：
```json
[
  {"name": "travel", "exec_skill_key": "travel_data"},
  {"name": "shopping", "exec_skill_key": "shopping_data"},
  {"name": "foodie"}
]
```

3. **混合格式**：
```json
["skill1", {"name": "skill2", "exec_skill_key": "key2"}, "skill3"]
```

4. **逗号分隔字符串格式**：
```
search, summary, delegate
```

#### exec_skill_key 说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | Skill 名称 |
| `exec_skill_key` | string | 否 | DataBus key，用于读取该 skill 的专属数据 |

当指定 `exec_skill_key` 时，框架会将其注入到 skill node 的 `inputs["exec_skill_key"]`，
node 可据此从 DataBus 读取专属数据，实现 skill 输入的精准分发。

#### 访问 exec_skill_key

```cpp
// C++ SO Node
std::string exec_skill_key;
if(inputs.count("exec_skill_key")) {
  exec_skill_key = std::get<std::string>(inputs["exec_skill_key"]);
  // 从 DataBus 读取 skill 专属数据
}
```

```bash
# Shell Node
EXEC_SKILL_KEY="${inputs[exec_skill_key]}"
if [ -n "$EXEC_SKILL_KEY" ]; then
  SKILL_DATA=$(af_databus_get "$EXEC_SKILL_KEY")
fi
```

```python
# Python Node
exec_skill_key = inputs.get("exec_skill_key", "")
if exec_skill_key:
    skill_data = af_databus_get(exec_skill_key)
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

## Candidate Skills 限制

### 概述

`candidate_skills` 是 node 级别的 skill 访问控制机制，用于限制该节点可以请求调用的 skill 范围。通过声明 `candidate_skills`，可以增强系统的安全性和可控性，防止节点调用未授权的 skill。

### 配置方式

在 workflow JSON 的 node 定义中添加 `candidate_skills` 字段：

```json
{
  "id": "skill_selector",
  "name": "Skill Selection Node",
  "type": "dispatcher",
  "candidate_skills": ["search", "translate", "summarize"]
}
```

### 行为规则

| candidate_skills 状态 | 行为 |
|----------------------|------|
| 未配置或为空 | 无限制，允许调用任何 skill（向后兼容） |
| 配置了 skill 列表 | 仅允许调用列表中的 skill |

当节点尝试调用不在 `candidate_skills` 列表中的 skill 时：
- 框架记录警告日志
- Skill 启动被阻止
- 返回失败消息给 coordinator

### 使用场景

1. **权限隔离**：限制 dispatcher 节点只能调用特定的 skill 集合
2. **安全审计**：明确声明节点的 skill 依赖，便于系统审计
3. **防止误用**：避免节点意外调用未授权的 skill

### 与 next_skills 的关系

`candidate_skills` 用于校验节点输出的 `next_skills`：

```
┌─────────────────────────────────────────────────────────────────┐
│                 Candidate Skills Validation                      │
├─────────────────────────────────────────────────────────────────┤
│  1. 节点输出 next_skills                                         │
│  2. 框架检查节点的 candidate_skills 配置                         │
│  3. 若 candidate_skills 为空，允许所有 skill                     │
│  4. 若 candidate_skills 非空，校验每个 skill 是否在列表中        │
│  5. 校验通过 → 启动 skill task                                  │
│  6. 校验失败 → 阻止 skill 启动，记录警告日志                     │
└─────────────────────────────────────────────────────────────────┘
```

### 完整示例

```json
{
  "id": "smart_dispatcher",
  "name": "Smart Dispatcher",
  "type": "dispatcher",
  "candidate_skills": ["search", "translate", "summarize", "weather"],
  "inputs": ["user_prompt"],
  "outputs": ["next_skills", "response"]
}
```

该 dispatcher 节点只能调用 `search`、`translate`、`summarize`、`weather` 四个 skill，尝试调用其他 skill 将被阻止。
