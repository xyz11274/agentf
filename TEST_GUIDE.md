# AgentFlow 测试指南

## 概述

本文档描述 AgentFlow 项目的测试覆盖情况以及如何使用 AgentFlow 框架进行开发和测试。

---

## 测试覆盖情况

### 已构建并测试的组件

| 组件 | 文件 | 构建状态 | 运行状态 | 测试覆盖 |
|------|------|----------|----------|----------|
| **核心库** | `libagentflow.a` | ✅ 成功 | - | 核心功能库 |
| **Agent** | `src/agent.cpp` | ✅ 成功 | - | Agent 基础功能 |
| **Workflow** | `src/workflow.cpp` | ✅ 成功 | - | DAG 调度、验证 |
| **Node** | `src/node.cpp` | ✅ 成功 | - | 节点执行、状态管理 |
| **Edge** | `src/edge.cpp` | ✅ 成功 | - | 依赖关系定义 |
| **DataBus** | `src/databus.cpp` | ✅ 成功 | - | 数据总线功能 |
| **Actor System** | `src/actor_system.cpp` | ✅ 成功 | - | Actor 框架适配 |
| **NodeActor** | `src/node_actor.cpp` | ✅ 成功 | - | 节点 Actor 实现 |
| **WorkflowActor** | `src/workflow_actor.cpp` | ✅ 成功 | - | 工作流 Actor 实现 |
| **Event** | `src/event.cpp` | ✅ 成功 | - | 事件系统 |
| **AgentRuntime** | `src/agent_runtime.cpp` | ✅ 成功 | - | 运行时引擎 |
| **Metrics** | `src/metrics.cpp` | ✅ 成功 | - | 指标收集 |
| **Logger** | `src/logger.cpp` | ✅ 成功 | - | 日志系统 |
| **DSL Parser** | `src/dsl/workflow_parser.cpp` | ✅ 成功 | - | JSON 解析 |
| **Network** | `src/network/*.cpp` | ✅ 成功 | - | 网络通信 |
| **Serialization** | `src/serialization/*.cpp` | ✅ 成功 | - | 序列化适配 |
| **EventLoop** | `src/event/eventloop_adapter/libevent_impl.cpp` | ✅ 成功 | - | 事件循环 |
| **Storage** | `src/storage/*.cpp` | ✅ 成功 | - | 存储管理 |
| **ControlFlow** | `src/runtime/control_flow.cpp` | ✅ 成功 | - | 控制流管理 |
| **ZeroCopyDataBus** | `src/runtime/zero_copy_databus.cpp` | ✅ 成功 | - | 零拷贝数据总线 |
| **LocalChannel** | `src/runtime/local_channel.cpp` | ✅ 成功 | - | 本地通道 |

### 示例程序

| 示例 | 文件 | 构建状态 | 运行状态 |
|------|------|----------|----------|
| `example_basic` | `examples/basic_example.cpp` | ✅ 成功 | ✅ 运行成功 |
| `example_adapter` | `examples/adapter_example.cpp` | ✅ 成功 | - |
| `example_dsl` | `examples/dsl_example.cpp` | ✅ 成功 | - |
| `example_metrics` | `examples/metrics_example.cpp` | ✅ 成功 | ✅ 运行成功 |
| `example_p1_features` | `examples/p1_features_example.cpp` | ✅ 成功 | ✅ 运行成功 |
| `example_storage` | `examples/storage_example.cpp` | ✅ 成功 | - |
| `example_control_flow` | `examples/control_flow_example.cpp` | ✅ 成功 | ✅ 运行成功 |
| `example_node_execution` | `examples/node_execution_example.cpp` | ✅ 成功 | ✅ 运行成功 |

### 测试文件

| 测试文件 | 构建状态 | 说明 |
|----------|----------|------|
| `test_adapter_isolation` | ✅ 成功 | 适配器隔离测试 |
| `test_agentflow_gtest` | ❌ 编译错误 | 需要修复 API 不匹配问题 |

**注意**: `test_agentflow_gtest` 包含以下测试文件，由于 API 不匹配目前无法编译：
- `test_workflow_json_gtest.cpp`
- `test_agent_json_gtest.cpp`
- `test_request_response_gtest.cpp`
- `test_eventloop_gtest.cpp`
- `test_local_channel_gtest.cpp`
- 其他基础测试文件

---

## 如何使用 AgentFlow 框架

### 1. 快速开始

#### 构建项目

```bash
# 使用构建脚本
./build.sh build

# 或手动构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(sysctl -n hw.ncpu)
```

#### 运行示例

```bash
cd build

# 基础示例
./example_basic

# 指标收集示例
./example_metrics

# 控制流示例
./example_control_flow

# P1 功能示例
./example_p1_features
```

### 2. AgentFlow 核心概念

#### 2.1 Agent

Agent 是 AgentFlow 系统中的执行主体，包含工作流定义和资源管理。

```cpp
#include "agentflow/framework/agent.h"

using namespace agentflow;

// 创建 Agent
Agent agent("agent_001", "My Agent");

// 设置 Agent 配置
Agent::Config config;
config.policy = Agent::LifecyclePolicy::FACTORY;
agent.set_config(config);
```

#### 2.2 Workflow

Workflow 是典型的 DAG 图结构，由 Node 和 Edge 组成。

```cpp
#include "agentflow/framework/workflow.h"

// 创建 Workflow
Workflow workflow("workflow_001", "My Workflow");

// 添加节点
Node node("node_001", "node_name");
workflow.add_node(node);

// 添加边
Edge edge("edge_001", "source_node", "target_node");
workflow.add_edge(edge);

// 验证 DAG
if (workflow.validate_dag()) {
    Logger::instance().info("Workflow DAG validation passed");
}
```

#### 2.3 Node

Node 是 Workflow 中执行具体任务的基本单元。

```cpp
#include "agentflow/framework/node.h"

// 创建节点配置
Node::Config config;
config.id = "node_001";
config.name = "My Node";
config.type = "DataProcessor";

// 创建节点
Node node(config);

// 设置输入
DataMap input;
input["key1"] = "value1";
node.set_inputs(input);

// 执行节点
if (node.execute()) {
    const DataMap& outputs = node.get_outputs();
    // 处理输出
}

// 清理节点
node.cleanup();
```

#### 2.4 数据总线 (DataBus)

用于 Workflow 内部的数据传递。

```cpp
#include "agentflow/runtime/databus.h"

using namespace agentflow::runtime;

DataBus databus;

// 发布数据
databus.put("key1", "value1");

// 获取数据
DataVariant value;
if (databus.get("key1", value)) {
    std::visit([](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            Logger::instance().info(arg);
        }
    }, value);
}

// 订阅数据变更
databus.subscribe("key1", [](const std::string& key, const DataVariant& value) {
    Logger::instance().info_f("Data updated: %s", key.c_str());
});
```

#### 2.5 零拷贝数据总线 (ZeroCopyDataBus)

高性能数据总线，支持零拷贝访问。

```cpp
#include "agentflow/runtime/zero_copy_databus.h"

using namespace agentflow::runtime;

// 创建数据总线 (1MB 容量)
ZeroCopyDataBus databus(1024 * 1024);

// 写入数据
databus.put("key1", "value1");

// 获取数据引用（零拷贝）
DataVariant value_ref;
if (databus.get_data_ref("key1", value_ref)) {
    // 直接访问数据，无拷贝
}

// 订阅数据变更
databus.subscribe("key1", [](const std::string& key, const DataVariant& value) {
    Logger::instance().info_f("Data changed: %s", key.c_str());
});
```

#### 2.6 控制流 (ControlFlow)

支持条件判断、循环、异常处理等控制流功能。

```cpp
#include "agentflow/runtime/control_flow.h"

using namespace agentflow::runtime;

// 创建控制上下文
ControlContext context;

// 设置变量
context.set_variable("score", "85");
context.set_variable("threshold", "80");

// 条件判断
ConditionEvaluator evaluator;
ConditionExpression expr("score", ConditionOperator::GREATER_THAN, "80");
if (evaluator.evaluate(expr, context, DataMap())) {
    Logger::instance().info("Score is greater than 80");
}

// 循环控制
LoopConfig for_loop;
for_loop.type = LoopType::FOR_LOOP;
for_loop.start_value = 0;
for_loop.end_value = 10;
for_loop.step = 1;

ControlFlowManager manager;
manager.initialize(context);
manager.enter_loop("example_loop", for_loop);

// 错误处理
ErrorHandlerConfig error_handler;
error_handler.strategy = ErrorStrategy::RETRY;
error_handler.max_retries = 3;
error_handler.retry_delay_ms = 500;
manager.set_error_handler("node_001", error_handler);
```

#### 2.7 本地通道 (LocalChannel)

支持发布-订阅模式的数据通道。

```cpp
#include "agentflow/runtime/local_channel.h"

using namespace agentflow::runtime;

// 获取或创建通道
auto channel = LocalChannelRegistry::instance().get_or_create(
    "my_channel", 100  // 容量
);

// 打开通道
channel->open();

// 发送数据
DataPacket packet("key1", "value1", "source_001");
channel->send(packet);

// 接收数据
DataPacket received;
if (channel->receive(received, 5000)) {  // 5秒超时
    Logger::instance().info_f("Received: %s", received.key.c_str());
}

// 订阅事件
size_t sub_id = channel->subscribe([](ChannelEvent event, const DataPacket& packet) {
    if (event == ChannelEvent::DATA_READY) {
        Logger::instance().info("Data is ready");
    }
});

// 取消订阅
channel->unsubscribe(sub_id);

// 关闭通道
channel->close();
```

#### 2.8 指标收集 (Metrics)

用于收集和导出监控指标。

```cpp
#include "agentflow/runtime/metrics.h"

using namespace agentflow::runtime;

// 创建指标收集器
Metrics metrics;

// 计数器指标
metrics.increment_counter("workflow_requests_total");
metrics.increment_counter("workflow_requests_success");
metrics.increment_counter("workflow_requests_failed");

// 直方图指标
metrics.record_histogram("node_execution_latency_ms", 45);
metrics.record_histogram("node_execution_latency_ms", 78);
metrics.record_histogram("node_execution_latency_ms", 23);

// 仪表盘指标
metrics.set_gauge("active_workflows", 5);
metrics.set_gauge("node_queue_size", 10);

// 导出指标（文本格式）
Logger::instance().info(metrics.export_text_format());

// 导出指标（JSON 格式）
Logger::instance().info(metrics.export_json_format());

// 导出指标（Prometheus 格式）
Logger::instance().info(metrics.export_prometheus_format());

// 获取统计信息
auto stats = metrics.get_histogram_stats("node_execution_latency_ms");
Logger::instance().info_f("P50: %.2f, P90: %.2f, P95: %.2f",
                           stats.p50, stats.p90, stats.p95);
```

### 3. DataVariant 使用指南

`DataVariant` 是 `std::variant<int64_t, double, std::string, bool>` 类型，用于表示不同类型的数据。

```cpp
#include "agentflow/common.h"

// 创建 DataVariant
DataVariant value1 = "string_value";
DataVariant value2 = 123;
DataVariant value3 = 45.67;
DataVariant value4 = true;

// 使用 std::visit 访问 variant
std::visit([](auto&& arg) {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, std::string>) {
        Logger::instance().info(arg);
    } else if constexpr (std::is_same_v<T, int64_t>) {
        Logger::instance().info_f("Int: %lld", arg);
    } else if constexpr (std::is_same_v<T, double>) {
        Logger::instance().info_f("Double: %f", arg);
    } else if constexpr (std::is_same_v<T, bool>) {
        Logger::instance().info_f("Bool: %s", arg ? "true" : "false");
    }
}, value);

// 将 DataVariant 转换为字符串
auto variant_to_string = [](const DataVariant& v) -> std::string {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return arg;
        } else {
            return std::to_string(arg);
        }
    }, v);
};
```

### 4. 测试指南

#### 4.1 编写测试

```cpp
#include <gtest/gtest.h>
#include "agentflow/framework/agent.h"
#include "agentflow/basic/logger.h"

using namespace agentflow;

class AgentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 测试前置准备
    agent_ = std::make_unique<Agent>("test_agent", "Test Agent");
  }

  void TearDown() override {
    // 测试后清理
    agent_.reset();
  }

  std::unique_ptr<Agent> agent_;
};

TEST_F(AgentTest, CreateAgent) {
  ASSERT_NE(agent_, nullptr);
  EXPECT_EQ(agent_->get_id(), "test_agent");
  EXPECT_EQ(agent_->get_name(), "Test Agent");
}

TEST_F(AgentTest, AgentLifecycle) {
  // 测试 Agent 生命周期
  agent_->initialize();
  EXPECT_EQ(agent_->get_state(), Agent::State::INITIALIZED);

  agent_->start();
  EXPECT_EQ(agent_->get_state(), Agent::State::RUNNING);

  agent_->stop();
  EXPECT_EQ(agent_->get_state(), Agent::State::STOPPED);
}
```

#### 4.2 运行测试

```bash
cd build

# 运行所有 Google Test 测试
./test_agentflow_gtest

# 运行特定测试
./test_agentflow_gtest --gtest_filter=AgentTest.*

# 运行适配器隔离测试
./test_adapter_isolation
```

#### 4.3 测试组织结构

```
tests/
├── test_agent_gtest.cpp         # Agent 测试
├── test_workflow_gtest.cpp      # Workflow 测试
├── test_node_gtest.cpp         # Node 测试
├── test_databus_gtest.cpp      # DataBus 测试
├── test_event_gtest.cpp        # Event 系统测试
├── test_eventloop_gtest.cpp    # EventLoop 测试
├── test_workflow_json_gtest.cpp # Workflow JSON 解析测试
├── test_agent_json_gtest.cpp   # Agent JSON 解析测试
├── test_local_channel_gtest.cpp # LocalChannel 测试
├── test_request_response_gtest.cpp # Request/Response 测试
└── test_adapter_isolation.cpp  # 适配器隔离测试
```

### 5. 构建和运行命令

```bash
# 完整构建
./build.sh build

# 清理构建目录
./build.sh clean

# 运行测试
./build.sh test

# 运行示例
./build.sh example

# 手动 CMake 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(sysctl -n hw.ncpu)

# 手动运行测试
cd build && ctest

# 手动运行特定示例
cd build && ./example_basic
```

### 6. 依赖说明

AgentFlow 使用以下第三方库（通过 Homebrew 安装，无需重新编译）：

| 依赖 | 用途 | 安装状态 |
|------|------|----------|
| OpenSSL | SSL/TLS 加密 | ✅ 已安装 |
| libwebsockets | WebSocket 支持 | ✅ 已安装 |
| Protobuf | 序列化 | ✅ 已安装 |
| libevent | 事件循环 | ✅ 已安装 |
| RapidJSON | JSON 解析 | ✅ 已安装 |
| Google Test | 测试框架 | ✅ 已安装 |

**注意**: Protoactor-cpp 是核心依赖，需要单独编译安装。

---

## 开发建议

1. **遵循编码规范**: 参考 `CODING_STANDARDS.md`
2. **使用智能指针**: 避免原始指针，使用 `std::shared_ptr`/`std::unique_ptr`
3. **线程安全**: 所有共享资源必须用 `std::mutex` 保护
4. **错误处理**: 使用返回值和异常机制处理错误
5. **日志记录**: 使用 Logger 记录重要事件和错误

---

## 已知问题和限制

1. **测试文件编译错误**: 部分测试文件由于 API 不匹配无法编译，需要修复
2. **平台支持**: 目前仅支持 Linux/Unix 系统
3. **Protoactor 依赖**: 需要单独编译安装 Protoactor-cpp

---

## 贡献指南

1. Fork 项目仓库
2. 创建特性分支 (`git checkout -b feature/my-feature`)
3. 提交更改 (`git commit -m 'feat: add new feature'`)
4. 推送到分支 (`git push origin feature/my-feature`)
5. 创建 Pull Request

更多信息请参考 `README.md` 和 `DEVELOPMENT.md`。

---

**维护**: AgentFlow Team
**最后更新**: 2026-02-17
