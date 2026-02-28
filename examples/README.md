# AgentFlow 示例说明

## 构建与运行

```bash
./build.sh build

./build.sh example databus
./build.sh example adapter
./build.sh example metrics
./build.sh example storage
```

## 示例结构

- `databus_example.cpp`：仅演示 DataBus（零拷贝读写、订阅、指针访问）。
- `adapter_example.cpp`：Actor 适配器调用示例。
- `metrics_example.cpp`：指标采集与导出示例。
- `storage_example.cpp`：Agent/Workflow 存储读写示例。
- `agent/`：仅保留 Agent 包示例资产（`basic_agent`、`calculator_agent` 及 workflow json）。

## Agent 包创建与运行（推荐）

外部业务不再通过代码直接创建 `Agent/Workflow/Node/Edge`，而是提供 Agent 包并由 `AgentActor` 自动加载。

### 1) 准备 AgentService.config

```json
{
  "package_root": "examples/agent",
  "manifests": [
    "basic_agent",
    "calculator_agent"
  ],
  "scan_interval_ms": 1000,
  "heartbeat_interval_ms": 1000
}
```

### 2) 启动后自动加载

`AgentActor` 启动后会读取上述配置，自动创建 Agent 实例并向 `RuntimeActor` 上报注册/心跳/状态。

### 3) 业务请求处理

业务侧通过 Actor 消息投递 `request.message`，`Agent` 内部事件队列触发 `AgentCoordinator` 会话处理，默认生成 `task0` 类型 task，并由 `AgentActor` 统一维护生命周期并继续上报 Runtime。

### 4) coordinator(task0) 固定流程定制

可在 Agent 包 `package.json` 中配置：

```json
{
  "coordinator_workflow": "basic-coordinator-workflow"
}
```

当请求消息未显式指定 `workflow_id` 时，系统会优先使用 `coordinator_workflow`。示例中的 `coordinator_workflow.json` 展示了 session 创建节点与 task 创建节点的编排方式。
