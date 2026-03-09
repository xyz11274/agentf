# MMNode 开发指南

本文档详细介绍如何在 AgentFlow 框架中开发 Node，包括框架与 Node 间的通信接口、输出返回方式、控制流操作以及各执行模式的具体实现方法。

## 1. 概述

### 1.1 Node 在框架中的角色

Node 是 AgentFlow 工作流中的基本计算单元。每个 Node 接收输入数据，执行特定逻辑，然后产生输出。框架负责：

- 调度 Node 执行
- 管理 Node 间的数据流转
- 处理重试和超时
- 提供数据总线（DataBus）服务

### 1.2 执行流程

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Runtime   │────▶│    Node     │────▶│   Runtime   │
│ (准备输入)   │     │  (执行逻辑)  │     │ (解析输出)   │
└─────────────┘     └─────────────┘     └─────────────┘
       │                   │                   │
       ▼                   ▼                   ▼
  环境变量/文件       用户代码执行         stdout 解析
```

## 2. 框架与 Node 的通信接口

### 2.1 输入传递方式

框架通过以下方式向 Node 传递输入数据：

#### 环境变量约定

所有输入参数会被转换为环境变量，格式为 `AF_<参数名>`：

```
AF_DATABUS_DIR      - DataBus 根目录
AF_DATABUS_KV_DIR   - DataBus KV 存储目录
AF_DATABUS_EVENTS_DIR - DataBus 事件目录
AF_LLM_BASE_URL     - LLM API 基础 URL（如已配置）
AF_LLM_API_KEY      - LLM API 密钥（如已配置）
AF_LLM_MODEL        - LLM 模型名称（如已配置）
AF_<参数名>          - 用户定义的输入参数
```

**参数名转换规则**：
- 字母转换为大写
- 非字母数字字符转换为下划线

例如，输入参数 `user_query` 对应环境变量 `AF_USER_QUERY`。

**代码位置**：`src/runtime/worker/worker.cpp:62-81`

```cpp
for(const auto& pair : request.inputs) {
  std::string env_key = "AF_" + to_env_suffix(pair.first);
  exec_config.env_vars.push_back(env_key + "=" + variant_to_string(pair.second));
}
```

#### DataBus SDK 读取

对于需要读取其他 Node 产生的数据或历史数据，可以使用 DataBus SDK：

| 语言 | SDK 文件 |
|------|---------|
| Shell | `publish/sdk/shell/agentflow_databus.sh` |
| Python | `publish/sdk/python/agentflow_databus.py` |
| Go | `publish/sdk/go/agentflow_databus.go` |
| Java | `publish/sdk/java/AgentFlowDataBus.java` |

### 2.2 输出返回方式

#### 退出码约定

- `0` - 执行成功
- 非 `0` - 执行失败

#### stdout 输出格式

框架根据 `json_output` 配置项决定如何解析 Node 的 stdout 输出：

**默认模式（json_output = false）**：
- stdout 内容存储到 `outputs["stdout"]`
- stderr 内容存储到 `outputs["stderr"]`

**JSON 模式（json_output = true）**：
- stdout 被解析为 JSON 对象
- 每个 key-value 对存入 outputs

```json
{"status": "success", "result": "processed data", "count": 42}
```

解析后：
- `outputs["status"]` = "success"
- `outputs["result"]` = "processed data"
- `outputs["count"]` = 42

**代码位置**：`src/workflow/node/node.cpp:642-649`

```cpp
if(config_.json_output && !worker_response.stdout_content.empty()) {
  DataMap parsed;
  if(parse_flat_json_object(worker_response.stdout_content, parsed)) {
    for(auto& kv : parsed) {
      outputs_[kv.first] = std::move(kv.second);
    }
  }
}
```

#### SO_LIBRARY 特殊格式

对于 `so_library` 执行模式，框架额外解析 stdout 中的 `key=value` 格式行：

```
result=processed_data
count=42
status=success
```

解析后：
- `outputs["result"]` = "processed_data"
- `outputs["count"]` = "42"
- `outputs["status"]` = "success"

**代码位置**：`src/runtime/worker/worker.cpp:217-227`

### 2.3 控制流接口

#### selected_skills（技能选择路由）

`selected_skills` 是框架约定的控制流接口，用于将任务路由到特定的 skill task-workflow。

**使用场景**：
- Agent 中的协调器（Coordinator）根据用户意图选择技能
- 动态路由到不同的处理流程

**数据格式**：每行一个 JSON 对象，包含 `skill` 和可选的 `session_id`：

```
{"skill": "search", "session_id": "session_123"}
{"skill": "summary", "session_id": "session_456"}
```

**设置方式**：

1. 在 Node 输出中设置（推荐使用 JSON 模式）：

```json
{"selected_skills": "{\"skill\": \"search\"}\n{\"skill\": \"summary\"}"}
```

2. 通过 DataBus 写入：

```bash
af_databus_set "selected_skills" '{"skill": "search"}'
```

**框架处理逻辑**：`src/agent/agent.cpp:719-757`

```cpp
std::string selected_skills;
if(const auto it = message.metadata.find("selected_skills"); it != message.metadata.end()) {
  selected_skills = it->second;
} else {
  DataVariant selected_value;
  if(get_data("selected_skills", selected_value)) {
    selected_skills = *std::get_if<std::string>(&selected_value);
  }
}
// 按行解析，路由到对应 skill task
```

#### DataBus 控制消息

Node 可以通过 DataBus 发布事件消息，实现异步通信和状态通知：

```bash
# 发布事件
af_databus_publish "agentflow.node.progress" '{"node_id": "extract", "progress": 50}'
```

## 3. DataBus SDK 使用指南

### 3.1 Shell SDK

**引入方式**：

```bash
source /path/to/agentflow_databus.sh
```

**核心 API**：

| 函数 | 说明 |
|------|------|
| `af_databus_set <key> <value> [type]` | 写入 KV 数据 |
| `af_databus_get <key>` | 读取 KV 数据 |
| `af_databus_publish <topic> <value> [type] [kind]` | 发布事件 |
| `af_databus_next_event <subscriber> <topic> [timeout_ms]` | 订阅事件 |
| `af_databus_request_llm <request_id> <prompt> [channel]` | 请求 LLM 调用 |

**示例**：

```bash
#!/bin/sh
source /path/to/agentflow_databus.sh

# 读取输入
user_query="$AF_USER_QUERY"

# 写入结果
result="Processed: $user_query"
af_databus_set "processing_result" "$result"

# 发布进度事件
af_databus_publish "agentflow.node.complete" '{"node": "process", "status": "done"}'

# 输出 JSON 格式结果（用于 json_output 模式）
echo "{\"status\": \"success\", \"result\": \"$result\"}"
```

### 3.2 Python SDK

**引入方式**：

```python
from agentflow_databus import FileDataBus
import os

bus = FileDataBus()  # 自动从 AF_DATABUS_DIR 读取
```

**核心 API**：

| 方法 | 说明 |
|------|------|
| `set(key, value, value_type)` | 写入 KV 数据 |
| `get(key)` | 读取 KV 数据 |
| `publish(topic, value, value_type, kind)` | 发布事件 |
| `next_event(topic, timeout_ms)` | 订阅事件 |
| `request_llm(request_id, prompt, channel)` | 请求 LLM 调用 |

**示例**：

```python
#!/usr/bin/env python3
import os
import json
from agentflow_databus import FileDataBus

# 获取输入参数
user_query = os.environ.get("AF_USER_QUERY", "")

# 使用 DataBus
bus = FileDataBus()
bus.set("processing_status", "in_progress")

# 业务逻辑
result = f"Processed: {user_query}"
bus.set("processing_result", result)

# 输出 JSON 结果（用于 json_output 模式）
output = {
    "status": "success",
    "result": result,
    "count": len(user_query)
}
print(json.dumps(output))
```

### 3.3 Go SDK

**引入方式**：

```go
package main

import "path/to/agentflow"

bus := agentflow.NewFileDataBus("") // 自动从 AF_DATABUS_DIR 读取
```

**核心 API**：

| 方法 | 说明 |
|------|------|
| `Set(key, value, valueType string) error` | 写入 KV 数据 |

**示例**：

```go
package main

import (
    "encoding/json"
    "fmt"
    "os"
    "path/to/agentflow"
)

func main() {
    // 获取输入参数
    userQuery := os.Getenv("AF_USER_QUERY")

    // 使用 DataBus
    bus := agentflow.NewFileDataBus("")
    bus.Set("processing_status", "in_progress", "string")

    // 业务逻辑
    result := fmt.Sprintf("Processed: %s", userQuery)
    bus.Set("processing_result", result, "string")

    // 输出 JSON 结果
    output := map[string]interface{}{
        "status": "success",
        "result": result,
    }
    data, _ := json.Marshal(output)
    fmt.Println(string(data))
}
```

### 3.4 Java SDK

**引入方式**：

```java
import agentflow.sdk.AgentFlowDataBus;

AgentFlowDataBus bus = new AgentFlowDataBus(null); // 自动从 AF_DATABUS_DIR 读取
```

**核心 API**：

| 方法 | 说明 |
|------|------|
| `set(String key, String value)` | 写入 KV 数据 |

**示例**：

```java
import agentflow.sdk.AgentFlowDataBus;
import com.google.gson.Gson;
import java.util.HashMap;
import java.util.Map;

public class ProcessNode {
    public static void main(String[] args) throws Exception {
        // 获取输入参数
        String userQuery = System.getenv("AF_USER_QUERY");

        // 使用 DataBus
        AgentFlowDataBus bus = new AgentFlowDataBus(null);
        bus.set("processing_status", "in_progress");

        // 业务逻辑
        String result = "Processed: " + userQuery;
        bus.set("processing_result", result);

        // 输出 JSON 结果
        Map<String, Object> output = new HashMap<>();
        output.put("status", "success");
        output.put("result", result);
        System.out.println(new Gson().toJson(output));
    }
}
```

## 4. 按执行模式开发指南

### 4.1 SO_LIBRARY 开发指南

#### 函数签名

SO Library 必须导出符合以下签名的函数：

```cpp
// include/internal/workflow/node/exe_loader.h:139
using SoFunction = void (*)(const DataMap& inputs, DataMap& outputs);

// DataMap 定义
using DataVariant = std::variant<std::string, int64_t, double, bool>;
using DataMap = std::map<std::string, DataVariant>;
```

#### 实现示例

```cpp
// my_node.cpp
#include <string>
#include <map>
#include <variant>

// DataVariant 和 DataMap 定义（与框架保持一致）
using DataVariant = std::variant<std::string, int64_t, double, bool>;
using DataMap = std::map<std::string, DataVariant>;

extern "C" {

// 默认入口函数（可自定义，在 DSL 中通过 function_name 指定）
void execute(const DataMap& inputs, DataMap& outputs) {
    // 1. 读取输入参数
    std::string user_query;
    if (auto it = inputs.find("user_query"); it != inputs.end()) {
        if (auto* str = std::get_if<std::string>(&it->second)) {
            user_query = *str;
        }
    }

    // 2. 执行业务逻辑
    std::string result = "Processed: " + user_query;

    // 3. 填充输出（方式一：直接写入 outputs）
    outputs["status"] = std::string("success");
    outputs["result"] = result;
    outputs["count"] = static_cast<int64_t>(user_query.length());

    // 4. 填充输出（方式二：stdout 输出 key=value 格式）
    // 注意：此方式在 so_library 模式下会被额外解析
    // printf("status=success\n");
    // printf("result=%s\n", result.c_str());
}

} // extern "C"
```

#### 编译要求

```bash
# Linux
g++ -shared -fPIC -o libmy_node.so my_node.cpp

# macOS
clang++ -shared -fPIC -o libmy_node.dylib my_node.cpp
```

#### DSL 配置

```json
{
  "id": "my_node",
  "execution_mode": "so_library",
  "command": "lib/libmy_node.so",
  "function_name": "execute",
  "inputs": ["user_query"],
  "outputs": ["status", "result", "count"]
}
```

### 4.2 SHELL_SCRIPT 开发指南

#### 脚本结构

```bash
#!/bin/sh
# Node: 数据处理节点
# 输入: AF_USER_QUERY
# 输出: JSON 对象到 stdout

# 1. 获取输入参数
user_query="${AF_USER_QUERY:-}"

# 2. 引入 DataBus SDK（可选）
# source /path/to/agentflow_databus.sh

# 3. 执行业务逻辑
if [ -z "$user_query" ]; then
    echo '{"status": "error", "error": "user_query is required"}'
    exit 1
fi

result="Processed: $user_query"
count=${#user_query}

# 4. 使用 DataBus（可选）
# af_databus_set "processing_result" "$result"

# 5. 输出结果
# json_output=true 时，框架解析 JSON 对象
echo "{\"status\": \"success\", \"result\": \"$result\", \"count\": $count}"

exit 0
```

#### DSL 配置

```json
{
  "id": "process_node",
  "execution_mode": "shell_script",
  "command": "scripts/process.sh",
  "working_dir": "/opt/agentflow",
  "inputs": ["user_query"],
  "outputs": ["status", "result", "count"]
}
```

#### json_output 模式

当 Node 配置 `"json_output": true` 时：

```json
{
  "id": "process_node",
  "execution_mode": "shell_script",
  "command": "scripts/process.sh",
  "json_output": true,
  "inputs": ["user_query"],
  "outputs": ["status", "result", "count"]
}
```

框架会将脚本的 stdout 解析为 JSON 对象，每个字段映射到 outputs。

### 4.3 PYTHON_SCRIPT 开发指南

#### 基础模板

```python
#!/usr/bin/env python3
"""
Node: 数据处理节点
输入: AF_USER_QUERY
输出: JSON 对象到 stdout
"""

import os
import json
import sys

def main():
    # 1. 获取输入参数
    user_query = os.environ.get("AF_USER_QUERY", "")

    # 2. 可选：使用 DataBus SDK
    try:
        from agentflow_databus import FileDataBus
        bus = FileDataBus()
        bus.set("processing_status", "in_progress")
    except ImportError:
        pass

    # 3. 执行业务逻辑
    if not user_query:
        output = {"status": "error", "error": "user_query is required"}
        print(json.dumps(output))
        sys.exit(1)

    result = f"Processed: {user_query}"

    # 4. 可选：写入 DataBus
    # bus.set("processing_result", result)

    # 5. 输出 JSON 结果
    output = {
        "status": "success",
        "result": result,
        "count": len(user_query)
    }
    print(json.dumps(output))
    sys.exit(0)

if __name__ == "__main__":
    main()
```

#### 使用 selected_skills 控制路由

```python
#!/usr/bin/env python3
"""
Node: 技能选择节点
根据用户意图选择要执行的技能
"""

import os
import json

def main():
    user_query = os.environ.get("AF_USER_QUERY", "").lower()

    # 根据查询内容选择技能
    selected_skills = []

    if "search" in user_query or "find" in user_query:
        selected_skills.append({
            "skill": "search",
            "session_id": os.environ.get("AF_SESSION_ID", "")
        })

    if "summarize" in user_query or "summary" in user_query:
        selected_skills.append({
            "skill": "summary",
            "session_id": os.environ.get("AF_SESSION_ID", "")
        })

    # 输出 selected_skills
    skills_json = "\n".join(json.dumps(s) for s in selected_skills)

    output = {
        "status": "success",
        "selected_skills": skills_json
    }
    print(json.dumps(output))

if __name__ == "__main__":
    main()
```

#### DSL 配置

```json
{
  "id": "skill_selector",
  "execution_mode": "python_script",
  "command": "nodes/skill_selector.py",
  "json_output": true,
  "inputs": ["user_query", "session_id"],
  "outputs": ["status", "selected_skills"]
}
```

### 4.4 EXECUTABLE 开发指南

EXECUTABLE 模式用于执行编译后的二进制文件。

#### Go 示例

```go
// main.go
package main

import (
    "encoding/json"
    "fmt"
    "os"
    "strings"
)

func main() {
    // 获取输入参数
    userQuery := os.Getenv("AF_USER_QUERY")

    // 执行业务逻辑
    result := fmt.Sprintf("Processed: %s", userQuery)

    // 输出 JSON 结果
    output := map[string]interface{}{
        "status": "success",
        "result": result,
        "count":  len(userQuery),
    }

    // 检查是否要求 JSON 输出格式
    if os.Getenv("AF_OUTPUT_FORMAT") == "json" {
        data, _ := json.Marshal(output)
        fmt.Println(string(data))
    } else {
        fmt.Println(result)
    }
}
```

编译：

```bash
go build -o process_node main.go
```

#### DSL 配置

```json
{
  "id": "process_node",
  "execution_mode": "executable",
  "command": "bin/process_node",
  "inputs": ["user_query"],
  "outputs": ["status", "result", "count"]
}
```

### 4.5 JAVA_JAR 开发指南

#### 实现示例

```java
// ProcessNode.java
package com.example.agentflow;

import com.google.gson.Gson;
import java.util.HashMap;
import java.util.Map;

public class ProcessNode {
    public static void main(String[] args) {
        // 获取输入参数
        String userQuery = System.getenv("AF_USER_QUERY");

        // 执行业务逻辑
        String result = "Processed: " + userQuery;

        // 输出 JSON 结果
        Map<String, Object> output = new HashMap<>();
        output.put("status", "success");
        output.put("result", result);
        output.put("count", userQuery.length());

        System.out.println(new Gson().toJson(output));
    }
}
```

#### 打包

```bash
# 编译
javac -cp gson.jar ProcessNode.java

# 打包 JAR
jar cvfe process_node.jar com.example.agentflow.ProcessNode *.class
```

#### DSL 配置

```json
{
  "id": "process_node",
  "execution_mode": "java_jar",
  "command": "lib/process_node.jar",
  "main_class": "com.example.agentflow.ProcessNode",
  "inputs": ["user_query"],
  "outputs": ["status", "result", "count"]
}
```

## 5. 场景化示例

### 5.1 简单数据处理 Node

**场景**：接收用户输入，处理后返回结果

```bash
#!/bin/sh
# nodes/text_process.sh

# 获取输入
text="${AF_INPUT_TEXT:-}"

# 处理逻辑
word_count=$(echo "$text" | wc -w)
char_count=${#text}

# 输出结果（JSON 格式）
cat <<EOF
{"status": "success", "word_count": $word_count, "char_count": $char_count}
EOF
```

**DSL**：

```json
{
  "id": "text_process",
  "execution_mode": "shell_script",
  "command": "nodes/text_process.sh",
  "json_output": true,
  "inputs": ["input_text"],
  "outputs": ["status", "word_count", "char_count"]
}
```

### 5.2 技能选择 Node（使用 selected_skills）

**场景**：根据用户意图选择要执行的技能

```python
#!/usr/bin/env python3
"""nodes/intent_classifier.py - 意图分类节点"""

import os
import json

def classify_intent(query):
    """简单的关键词匹配分类"""
    query = query.lower()

    if any(kw in query for kw in ["search", "find", "look for"]):
        return "search"
    elif any(kw in query for kw in ["summarize", "summary", "brief"]):
        return "summary"
    elif any(kw in query for kw in ["translate", "translation"]):
        return "translate"
    else:
        return "chat"

def main():
    user_query = os.environ.get("AF_USER_QUERY", "")
    session_id = os.environ.get("AF_SESSION_ID", "")

    if not user_query:
        print(json.dumps({"status": "error", "error": "empty query"}))
        exit(1)

    # 分类意图
    skill = classify_intent(user_query)

    # 构造 selected_skills
    selected_skill = {
        "skill": skill,
        "session_id": session_id
    }

    output = {
        "status": "success",
        "intent": skill,
        "selected_skills": json.dumps(selected_skill)
    }

    print(json.dumps(output))

if __name__ == "__main__":
    main()
```

**DSL**：

```json
{
  "id": "intent_classifier",
  "execution_mode": "python_script",
  "command": "nodes/intent_classifier.py",
  "json_output": true,
  "inputs": ["user_query", "session_id"],
  "outputs": ["status", "intent", "selected_skills"]
}
```

### 5.3 LLM 调用 Node

**场景**：调用 LLM 进行文本生成

```python
#!/usr/bin/env python3
"""nodes/llm_generate.py - LLM 生成节点"""

import os
import json
import requests

def call_llm(prompt, base_url=None, api_key=None, model=None):
    """调用 LLM API"""
    base_url = base_url or os.environ.get("AF_LLM_BASE_URL", "https://api.openai.com/v1")
    api_key = api_key or os.environ.get("AF_LLM_API_KEY", "")
    model = model or os.environ.get("AF_LLM_MODEL", "gpt-3.5-turbo")

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json"
    }

    payload = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": 1000
    }

    response = requests.post(
        f"{base_url}/chat/completions",
        headers=headers,
        json=payload,
        timeout=60
    )

    response.raise_for_status()
    return response.json()["choices"][0]["message"]["content"]

def main():
    prompt = os.environ.get("AF_PROMPT", "")

    if not prompt:
        print(json.dumps({"status": "error", "error": "prompt is required"}))
        exit(1)

    try:
        result = call_llm(prompt)

        output = {
            "status": "success",
            "response": result,
            "model": os.environ.get("AF_LLM_MODEL", "gpt-3.5-turbo")
        }
        print(json.dumps(output))

    except Exception as e:
        print(json.dumps({"status": "error", "error": str(e)}))
        exit(1)

if __name__ == "__main__":
    main()
```

**DSL**：

```json
{
  "id": "llm_generate",
  "execution_mode": "python_script",
  "command": "nodes/llm_generate.py",
  "json_output": true,
  "timeout_ms": 120000,
  "inputs": ["prompt"],
  "outputs": ["status", "response", "model"]
}
```

### 5.4 外部响应 Node

**场景**：处理完成后向外部系统发送响应

```python
#!/usr/bin/env python3
"""nodes/response_handler.py - 响应处理节点"""

import os
import json
from agentflow_databus import FileDataBus

def main():
    session_id = os.environ.get("AF_SESSION_ID", "")
    response = os.environ.get("AF_RESPONSE", "")

    # 使用 DataBus 写入响应
    bus = FileDataBus()

    # 写入会话响应
    response_key = f"session.{session_id}.response"
    bus.set(response_key, response)

    # 发布响应事件
    bus.publish(
        f"agentflow.session.{session_id}.response",
        json.dumps({
            "session_id": session_id,
            "response": response,
            "timestamp": int(time.time() * 1000)
        })
    )

    output = {
        "status": "success",
        "response_key": response_key
    }
    print(json.dumps(output))

if __name__ == "__main__":
    main()
```

## 6. 最佳实践

### 6.1 错误处理

- 始终检查必需的输入参数
- 使用结构化的错误输出格式
- 将详细信息写入 stderr，仅输出结果到 stdout

```python
if not required_param:
    print(json.dumps({"status": "error", "error": "required_param is missing"}))
    print("Detailed error message for debugging", file=sys.stderr)
    exit(1)
```

### 6.2 日志记录

- 使用 stderr 输出调试信息
- 关键步骤记录到 DataBus
- 遵循 JSON 输出格式确保 stdout 干净

### 6.3 超时处理

- 在 DSL 中合理设置 `timeout_ms`
- 对于长时间操作，考虑分批处理或异步模式

### 6.4 资源清理

- 在 Node 结束前清理临时文件
- 使用 DataBus 的事件机制通知完成状态

### 6.5 可测试性

- 将核心逻辑与框架依赖分离
- 支持通过命令行参数和环境变量双重输入
- 提供独立的测试入口

## 7. 附录

### 7.1 环境变量完整列表

| 环境变量 | 说明 | 来源 |
|---------|------|------|
| `AF_DATABUS_DIR` | DataBus 根目录 | 框架自动设置 |
| `AF_DATABUS_KV_DIR` | KV 存储目录 | 框架自动设置 |
| `AF_DATABUS_EVENTS_DIR` | 事件存储目录 | 框架自动设置 |
| `AF_LLM_BASE_URL` | LLM API 基础 URL | 配置传递 |
| `AF_LLM_API_KEY` | LLM API 密钥 | 配置传递 |
| `AF_LLM_MODEL` | LLM 模型名称 | 配置传递 |
| `AF_OUTPUT_FORMAT` | 输出格式（json） | 框架设置 |
| `AF_<INPUT_NAME>` | 用户输入参数 | 框架转换 |

### 7.2 输出字段约定

| 字段 | 说明 |
|------|------|
| `status` | 执行状态（success/error） |
| `error` | 错误信息（失败时） |
| `selected_skills` | 技能选择（控制流） |
| `stdout` | 标准输出（默认模式） |
| `stderr` | 标准错误输出 |
| `exit_code` | 退出码 |

### 7.3 相关文档

- [Node 开发规范](NODE_DEVELOPMENT_SPEC.md) - 基础入口契约
- [Node API 参考](NODE_API_REFERENCE.md) - 执行模式映射
- [Workflow DSL 参考](workflow_sample.jsonc) - DSL 配置详解
- [DataBus SDK 参考](../sdk/) - 各语言 SDK 实现