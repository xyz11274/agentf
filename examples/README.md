# AgentFlow 示例程序

## 概述

本目录包含 AgentFlow 框架的完整示例程序，覆盖从基础用法到高级特性的各个方面。所有示例随项目自动构建，无需额外配置。

## 构建与运行

```bash
# 构建整个项目（包括所有示例）
./build.sh build

# 运行指定示例
./build.sh example basic            # 基础入门
./build.sh example adapter          # Actor 适配器
./build.sh example control_flow     # 控制流
./build.sh example dsl              # DSL 工作流定义
./build.sh example metrics          # 指标收集
./build.sh example node_execution   # 节点执行模式
./build.sh example p1_features      # 高级特性
./build.sh example storage          # 持久化存储
```

## 示例列表

---

### 1. basic_example — 基础入门

- **难度**: ⭐ 入门
- **演示**: Agent / Workflow / Node / Edge 的创建、组装与生命周期管理
- **涉及 API**: `framework::Agent`, `framework::Workflow`, `framework::Node`, `framework::Edge`, `Logger`

**关键代码片段 — 创建 Agent 并添加 Workflow：**

```cpp
framework::Agent::Config agent_config;
agent_config.id          = "agent_001";
agent_config.name        = "BasicAgent";
agent_config.description = "A basic example agent";
agent_config.policy      = framework::Agent::LifecyclePolicy::SINGLETON;

auto agent = std::make_shared<framework::Agent>(agent_config);
```

**关键代码片段 — 通过 Edge 连接节点：**

```cpp
framework::Edge::Config edge_config;
edge_config.id            = "edge_001";
edge_config.source_node   = "node_001";
edge_config.target_node   = "node_002";
edge_config.transfer_mode = framework::Edge::DataTransferMode::SHARED_MEMORY;

auto edge = std::make_shared<framework::Edge>(edge_config);
workflow->add_edge(edge);
```

**预期输出**: 依次打印 Agent 创建、Workflow 添加、数据总线读写、Agent 启停等日志信息。

---

### 2. node_execution_example — 节点执行模式

- **难度**: ⭐⭐ 基础
- **演示**: C++ 函数执行模式与 Shell 脚本执行模式的区别与用法
- **涉及 API**: `framework::Node`, `Node::ExecutionMode`, `Node::Config`

**关键代码片段 — C++ 函数节点：**

```cpp
Node::Config cpp_config;
cpp_config.id             = "node_cpp_1";
cpp_config.name           = "Data Processor";
cpp_config.execution_mode = Node::ExecutionMode::CPP_FUNCTION;
cpp_config.timeout_ms     = 5000;
cpp_config.max_retries    = 2;
```

**关键代码片段 — Shell 命令节点：**

```cpp
Node::Config shell_config;
shell_config.id             = "node_shell_1";
shell_config.name           = "List Files";
shell_config.execution_mode = Node::ExecutionMode::SHELL_SCRIPT;
shell_config.command        = "ls -la";
```

**预期输出**: 分别执行 C++ 函数节点、Shell 命令节点和 Echo 命令节点，打印执行状态与 stdout 输出。

---

### 3. dsl_example — DSL 工作流定义

- **难度**: ⭐⭐ 基础
- **演示**: 通过 JSON DSL 定义工作流并解析、验证、拓扑排序
- **涉及 API**: `dsl::WorkflowParserFactory`, `ParseResult`, `Workflow::get_topological_order`

**关键代码片段 — 解析 JSON 工作流：**

```cpp
auto parser = WorkflowParserFactory::create();
auto result = parser->parse_workflow(workflow_dsl);

if (result.is_valid()) {
    Logger::instance().info_f("Workflow ID: %s", result.workflow->get_id().c_str());
    Logger::instance().info_f("Number of nodes: %zu", result.workflow->get_nodes().size());
}
```

**DSL 文件（`examples/dsl/` 目录）：**

| 文件 | 说明 |
|------|------|
| `sample_workflow.json` | 基础数据处理管线（Start → Validate → Transform → Enrich → Filter → Output） |
| `conditional_workflow.json` | 条件分支示例，根据 `request_type` 路由到不同处理节点 |
| `loop_workflow.json` | 循环迭代示例，使用 `FOR_EACH` 遍历数据集合 |

**预期输出**: 打印解析后的工作流信息、节点列表、拓扑排序结果和可执行节点集合。

---

### 4. adapter_example — Actor 适配器模式

- **难度**: ⭐⭐⭐ 中级
- **演示**: 基于 `IActor` 接口实现自定义 Actor，使用 `ActorSystemFactory` 创建 Actor 系统
- **涉及 API**: `actor::adapter::IActor`, `IContext`, `ActorSystemFactory`, `MessageBuilder`

**关键代码片段 — 自定义 EchoActor：**

```cpp
class EchoActor : public IActor {
public:
  void on_message(std::shared_ptr<IContext> ctx) override {
    auto msg = ctx->current_message();
    auto sender = ctx->sender();
    if (sender) {
      ctx->send(sender.get(), msg);
    }
  }
  void on_start() override { /* ... */ }
  void on_stop() override { /* ... */ }
  std::string actor_type() const override { return "EchoActor"; }
};
```

**关键代码片段 — 创建 Actor 系统并生成 Actor：**

```cpp
auto system = ActorSystemFactory::create("example-system");
auto echo_ref = system->spawn(echo_factory, "echo-actor-1");
```

**预期输出**: 创建 Actor 系统，生成 EchoActor 和 CounterActor，发送消息后关闭系统。

---

### 5. control_flow_example — 控制流

- **难度**: ⭐⭐⭐ 中级
- **演示**: 条件评估、循环控制（FOR/WHILE/REPEAT）、错误处理策略、控制动作、数据总线集成
- **涉及 API**: `runtime::ControlFlowManager`, `runtime::ConditionEvaluator`, `runtime::ConditionExpression`, `runtime::ZeroCopyDataBus`

本示例包含 6 个子示例：

1. **条件运算符** — `EQUAL`, `NOT_EQUAL`, `GREATER_THAN`, `LESS_THAN`, `CONTAINS`, `IS_NULL`, `AND`, `OR`
2. **循环控制** — FOR 循环、WHILE 循环、REPEAT 循环
3. **错误处理** — `RETRY`（重试）、`SKIP`（跳过）、`FAIL`（中止）策略
4. **控制动作** — `PROCEED`, `SKIP`, `GOTO`, `ABORT`, `RETRY`, `RETURN`, `BREAK`, `CONTINUE`
5. **下一节点决策** — 基于节点执行状态决定后续动作
6. **数据总线集成** — 订阅/取消订阅、读写锁、容量监控

**关键代码片段 — 条件评估：**

```cpp
runtime::ConditionExpression expr_gt("score", runtime::ConditionOperator::GREATER_THAN, "80");
bool result = evaluator.evaluate(expr_gt, context, DataMap());
```

**关键代码片段 — 错误处理策略：**

```cpp
runtime::ErrorHandlerConfig retry_handler;
retry_handler.strategy       = runtime::ErrorStrategy::RETRY;
retry_handler.max_retries    = 3;
retry_handler.retry_delay_ms = 500;
manager.set_error_handler("node_retry", retry_handler);
```

**预期输出**: 依次输出各运算符评估结果、循环迭代日志、错误重试过程和数据总线操作日志。

---

### 6. metrics_example — 指标收集

- **难度**: ⭐⭐ 基础
- **演示**: Counter / Gauge / Histogram 三种指标类型的创建与使用，支持 Text / JSON / Prometheus 格式导出
- **涉及 API**: `runtime::MetricsCollectorFactory`, `Counter`, `Gauge`, `Histogram`

**关键代码片段 — 创建指标并记录：**

```cpp
auto collector = MetricsCollectorFactory::get_default();
auto requests_total = collector->create_counter(
    "workflow_requests_total", "Total number of workflow execution requests");
auto execution_time = collector->create_histogram(
    "workflow_execution_time_ms", "Workflow execution time in milliseconds",
    {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000});

requests_total->increment();
execution_time->record(exec_time);
```

**预期输出**: 模拟 10 次工作流执行后，以 Text、JSON、Prometheus 三种格式输出指标数据，包括百分位统计（P50/P90/P95/P99）。

---

### 7. storage_example — 持久化存储

- **难度**: ⭐⭐ 基础
- **演示**: Agent 和 Workflow 配置的持久化保存/加载、版本管理、存储统计
- **涉及 API**: `storage::StorageManager`, `storage::IAgentStorage`, `storage::IWorkflowStorage`, `storage::VersionInfo`

**关键代码片段 — 保存与加载 Agent：**

```cpp
storage::StorageConfig config;
config.base_path         = "./storage_data";
config.enable_versioning = true;
config.max_versions      = 10;

auto storage_manager = storage::create_storage_manager(config);
auto result = storage_manager->agent_storage()->save_agent(agent_config, workflows, version_info);
```

**预期输出**: 创建存储管理器，保存并加载 Agent 和 Workflow 配置，列出已有实体和版本信息。

---

### 8. p1_features_example — 高级特性

- **难度**: ⭐⭐⭐ 中级
- **演示**: 零拷贝数据总线、条件评估、控制流与 Workflow 集成、私有协议通信
- **涉及 API**: `runtime::ZeroCopyDataBus`, `runtime::ConditionEvaluator`, `runtime::ControlFlowManager`, `network::ProtocolEncoder`, `network::ProtocolDecoder`

本示例包含 4 个子示例：

1. **零拷贝数据总线** — 数据读写、指针直接访问（`get_data_ptr`）、变更订阅通知
2. **条件评估** — 等于、大于、包含、AND/OR/NOT 复合条件
3. **控制流与 Workflow** — 带条件 Edge 的工作流创建、节点执行后的路由决策
4. **私有协议通信** — `DataRequest` / `ControlStart` / `FlowCondition` 消息的编解码

**关键代码片段 — 零拷贝指针访问：**

```cpp
runtime::ZeroCopyDataBus databus(1024 * 1024);  // 1MB
databus.put("key1", "value1");

const DataVariant* value_ptr = databus.get_data_ptr("key2");
```

**关键代码片段 — 私有协议编解码：**

```cpp
network::DataRequest data_request{
    "sender_agent_001", "receiver_agent_001", "data_key_123", "request_id_456"};
auto encoded = network::ProtocolEncoder::encode_data_request(data_request);

network::DataRequest decoded_request;
network::ProtocolDecoder::decode_message(encoded.data(), encoded.size(), decoded_request);
```

**预期输出**: 依次输出数据总线操作、条件评估结果、工作流路由决策和协议编解码结果。

---

## 学习路径

建议按以下顺序学习示例程序：

```
1. basic_example          ← 了解 Agent/Workflow/Node 基本概念
       ↓
2. node_execution_example ← 掌握节点执行模式（C++ / Shell）
       ↓
3. dsl_example            ← 学习 JSON DSL 定义工作流
       ↓
4. control_flow_example   ← 理解条件、循环、错误处理
       ↓
5. adapter_example        ← 学习 Actor 模式与消息传递
       ↓
6. metrics_example        ← 了解指标收集与监控
       ↓
7. storage_example        ← 掌握持久化存储
       ↓
8. p1_features_example    ← 综合运用高级特性
```

## 相关文档

- [API 参考](../docs/README.md)
- [架构设计](../docs/ARCHITECTURE.md)
- [开发指南](../docs/development.md)
