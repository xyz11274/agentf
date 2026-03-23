## 2026-03-17 Plugin UDS 注入机制设计

### 背景

当前框架在 `AF_DATABUS_UDS_FILE` 环境变量中注入 DataBus UDS 路径，使 Python/Go/Shell 等外部节点进程能够通过 Unix Domain Socket 访问 DataBus。但框架缺少通用的 Plugin UDS 注入机制，集成者无法为自定义插件配置 UDS 路径并在节点运行时获取该路径。

本设计引入 `AF_PLUGIN_UDS_FILE` 环境变量注入机制，参考现有 `AF_DATABUS_UDS_FILE` 实现模式，使集成者能够配置插件 UDS 路径，框架在节点运行时自动注入。

---

### 现有机制分析

#### AF_DATABUS_UDS_FILE 注入流程

位置：`src/runtime/worker/worker.cpp:68-84`

```cpp
// Create UDS server for real-time DataBus access when databus and session_id are provided
std::unique_ptr<UDSServer> uds_server;
if(request.databus && !request.session_id.empty()) {
  // Generate unique socket path based on session_id
  std::string uds_socket_path = "/tmp/agentflow_" + request.session_id + ".sock";
  uds_server = std::make_unique<UDSServer>(request.databus, uds_socket_path);
  if(uds_server->start()) {
    exec_config.env_vars.push_back("AF_DATABUS_UDS_FILE=" + uds_socket_path);
  }
}
```

**关键点**：
1. UDSServer 在节点执行时按需创建
2. UDS 路径基于 session_id 动态生成
3. 通过 `exec_config.env_vars` 注入环境变量
4. 外部进程通过该环境变量获取 UDS 路径

#### 环境变量注入点

所有环境变量通过 `ExecutionConfig.env_vars` 传递：

```cpp
exec_config.env_vars.push_back("AF_DATABUS_DIR=" + file_databus.root_dir().string());
exec_config.env_vars.push_back("AF_DATABUS_KV_DIR=" + file_databus.kv_dir().string());
exec_config.env_vars.push_back("AF_DATABUS_EVENTS_DIR=" + file_databus.events_dir().string());
exec_config.env_vars.push_back("AF_LLM_BASE_URL=" + request.llm_config.base_url);
exec_config.env_vars.push_back("AF_LLM_API_KEY=" + request.llm_config.api_key);
exec_config.env_vars.push_back("AF_LLM_MODEL=" + request.llm_config.model);
```

---

### 接口设计

#### 1. 运行时配置扩展

**MMAgentRuntime::Config 新增字段**：

```cpp
struct Config {
  RuntimeRole role = RuntimeRole::MASTER;
  int         worker_threads = 4;
  std::string local_worker_id = "runtime-local-worker";
  int         worker_default_workflow_threads = 1;
  int         worker_control_threads = 1;
  int         worker_business_threads = 1;
  size_t      worker_queue_size = 1024;
  // 新增
  std::string plugin_uds_path;  // 集成者配置的插件 UDS 路径
};
```

**ExecutionRequest 新增字段**：

```cpp
struct ExecutionRequest {
  // ... 现有字段 ...
  std::string plugin_uds_path;  // 新增：传递给节点的插件 UDS 路径
};
```

#### 2. 环境变量注入

在 `worker.cpp` 的 `execute_code()` 函数中添加注入逻辑：

```cpp
// Inject plugin UDS path if configured
if(!request.plugin_uds_path.empty()) {
  exec_config.env_vars.push_back("AF_PLUGIN_UDS_FILE=" + request.plugin_uds_path);
}
```

#### 3. 配置传递链路

```
MMAgentRuntime::Config.plugin_uds_path
         ↓
MMWorkerEngine (存储配置)
         ↓
ExecutionRequest.plugin_uds_path (传递到 execute_code)
         ↓
AF_PLUGIN_UDS_FILE 环境变量 (注入到节点进程)
```

---

### 配置方式

#### 方式一：运行时配置（推荐）

集成者在初始化 Runtime 时配置：

```cpp
runtime::MMAgentRuntime::Config config;
config.plugin_uds_path = "/var/run/my_plugin.sock";

auto runtime = std::make_shared<runtime::MMAgentRuntime>(config);
```

#### 方式二：环境变量覆盖

框架启动时检查 `AF_PLUGIN_UDS_FILE` 环境变量：

```cpp
// 在 Runtime 初始化时
const char* env_plugin_uds = std::getenv("AF_PLUGIN_UDS_FILE");
if(env_plugin_uds && config.plugin_uds_path.empty()) {
  config.plugin_uds_path = env_plugin_uds;
}
```

---

### SO Library 注入

SO Library 节点通过 V3 签名接收 `INodeDataBusClient*` 参数，已支持 DataBus 访问。插件 UDS 路径的注入方式：

#### 方案：通过环境变量

SO Library 节点在加载执行时，同样通过环境变量获取插件路径：

```cpp
// worker.cpp SO_LIBRARY 分支
case ExecutionMode::SO_LIBRARY: {
  // 环境变量已注入，SO 代码可通过 getenv("AF_PLUGIN_UDS_FILE") 获取
  if(request.databus && !request.session_id.empty()) {
    NodeDataBusAdapter adapter(request.databus);
    NodeDataBusClientImpl client(&adapter, request.session_id);
    result = code_loader.load_and_execute_so_v3(
        request.command, request.function_name.empty() ? "execute" : request.function_name,
        exec_config, &client);
  }
  // ...
}
```

**SO Library 节点获取插件路径示例**：

```cpp
extern "C" void my_node_v3(
    const SoDataMap& inputs,
    SoDataMap& outputs,
    INodeDataBusClient* client) {

  // 通过环境变量获取插件 UDS 路径
  const char* plugin_uds = std::getenv("AF_PLUGIN_UDS_FILE");
  if(plugin_uds) {
    // 连接插件服务...
  }
}
```

---

### 节点进程访问示例

#### Shell Script

```bash
#!/bin/bash
PLUGIN_UDS="${AF_PLUGIN_UDS_FILE:-}"
if [ -n "$PLUGIN_UDS" ]; then
  echo "Plugin UDS path: $PLUGIN_UDS"
  # 通过 socat 或 nc 与插件服务通信
fi
```

#### Python Script

```python
import os

plugin_uds = os.environ.get('AF_PLUGIN_UDS_FILE')
if plugin_uds:
    import socket
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(plugin_uds)
    # 与插件服务通信...
```

#### Go Script

```go
package main

import (
    "os"
    "net"
)

func main() {
    pluginUDS := os.Getenv("AF_PLUGIN_UDS_FILE")
    if pluginUDS != "" {
        conn, err := net.Dial("unix", pluginUDS)
        if err == nil {
            defer conn.Close()
            // 与插件服务通信...
        }
    }
}
```

---

### 代码变更清单

#### 头文件变更

**`include/internal/runtime/agent_runtime.h`**

```cpp
struct Config {
  // ... 现有字段 ...
  std::string plugin_uds_path;  // 新增
};
```

**`include/internal/runtime/worker.h`**

```cpp
struct ExecutionRequest {
  // ... 现有字段 ...
  std::string plugin_uds_path;  // 新增
};
```

#### 源文件变更

**`src/runtime/worker/worker.cpp`**

- 在 `execute_code()` 函数中添加 `AF_PLUGIN_UDS_FILE` 环境变量注入

**`src/runtime/runtime.cpp`**

- 在构造 MMWorkerEngine 时传递 `plugin_uds_path` 配置

**`src/workflow/node/node.cpp`**

- 在 `execute_with_worker_engine()` 中填充 `request.plugin_uds_path`

---

### 文档更新清单

| 文档 | 更新内容 | 状态 |
|------|----------|------|
| `publish/INTEGRATION_GUIDE.md` | 新增 `AF_PLUGIN_UDS_FILE` 环境变量说明、配置方式和节点访问示例 | ✅ 已完成 |
| `publish/workflow/NODE_API_REFERENCE.md` | 新增 SO Library 节点获取插件 UDS 路径的方法说明 | ✅ 已完成 |
| `docs/REFACTORING.md` | 本设计文档 | ✅ 已完成 |

---

### 验证方案

#### 单元测试用例

已实现 9 个单元测试用例：

**NodeTest (3 个)**：
1. **PluginUdsPathGetterSetter** - 测试节点 plugin_uds_path 的 getter/setter
2. **CloneInheritsPluginUdsPath** - 测试节点克隆时继承 plugin_uds_path
3. **CloneWithEmptyPluginUdsPath** - 测试空 plugin_uds_path 的克隆

**RuntimeMasterWorkerTest (6 个)**：
4. **WorkerEngineConfigPluginUdsPath** - 测试 WorkerEngine 配置中设置 plugin_uds_path
5. **RuntimeConfigPluginUdsPath** - 测试 Runtime 配置中设置 plugin_uds_path
6. **PluginUdsPathInjectedAsEnvVar** - 测试 AF_PLUGIN_UDS_FILE 环境变量正确注入
7. **NoPluginUdsPathWhenNotSet** - 测试未配置时不注入环境变量
8. **PluginUdsPathWithPythonScript** - 测试 Python 脚本可通过 os.environ 获取
9. **PluginUdsPathSpecialCharacters** - 测试特殊字符路径处理

#### 验证结果

- `./build.sh build`：✅ 通过
- `ctest --test-dir build -R AgentFlowGoogleTests --output-on-failure`：✅ 全部通过
- 9 个 Plugin UDS 相关测试全部通过

---

### 向后兼容性

- `plugin_uds_path` 为可选配置，未配置时不注入 `AF_PLUGIN_UDS_FILE` 环境变量
- 现有节点代码无需修改，`AF_PLUGIN_UDS_FILE` 环境变量不存在时节点正常运行
- 完全向后兼容现有 Runtime、Worker 和节点实现

---

### 实施状态

**设计阶段**：✅ 已完成

**代码变更**：✅ 已完成

已完成的代码变更：
- `include/internal/runtime/agent_runtime.h` - 添加 `Config.plugin_uds_path` 字段
- `include/internal/runtime/worker.h` - 添加 `ExecutionRequest.plugin_uds_path` 和 `Config.plugin_uds_path` 字段
- `include/internal/workflow/node.h` - 添加 `plugin_uds_path_` 成员和 getter/setter 方法
- `include/external/workflow/node.h` - 同步更新外部 API
- `src/runtime/worker/worker.cpp` - 添加 `AF_PLUGIN_UDS_FILE` 环境变量注入
- `src/runtime/runtime.cpp` - 传递 `plugin_uds_path` 到 MMWorkerEngine 并在 process_node 中注入
- `src/workflow/node/node.cpp` - 在 execute_with_worker_engine 和 clone 中支持 plugin_uds_path

**验证结果**：
- `./build.sh build`：✅ 通过
- `ctest --test-dir build --output-on-failure`：✅ 18/19 测试通过（E2ELLMQueryAgent 因 API Key 缺失失败，与本次修改无关）

**文档更新**：✅ 已完成
- `publish/INTEGRATION_GUIDE.md` - 新增 Plugin UDS 注入机制说明
- `publish/workflow/NODE_API_REFERENCE.md` - 新增 SO Library 节点获取插件 UDS 路径方法

---
