# AgentFlow 测试指南

> 本文档描述 AgentFlow 项目的测试覆盖情况、测试方法和使用示例。

---

## 目录

1. [测试覆盖情况](#1-测试覆盖情况)
2. [构建和运行测试](#2-构建和运行测试)
3. [编写测试](#3-编写测试)
4. [核心组件使用示例](#4-核心组件使用示例)
5. [DataVariant 使用指南](#5-datavariant-使用指南)

---

## 1. 测试覆盖情况

### 1.1 已构建并测试的组件

| 组件 | 文件 | 状态 | 说明 |
|------|------|------|------|
| **核心库** | `libagentflow.a` | ✅ | 核心功能库 |
| **Agent** | `src/agent.cpp` | ✅ | Agent 基础功能 |
| **Workflow** | `src/workflow.cpp` | ✅ | DAG 调度、验证 |
| **Node** | `src/node.cpp` | ✅ | 节点执行、状态管理 |
| **Edge** | `src/edge.cpp` | ✅ | 依赖关系定义 |
| **DataBus** | `src/databus.cpp` | ✅ | 数据总线功能 |
| **ZeroCopyDataBus** | `src/runtime/zero_copy_databus.cpp` | ✅ | 零拷贝数据总线 |
| **LocalChannel** | `src/runtime/local_channel.cpp` | ✅ | 本地通道 |
| **ControlFlow** | `src/runtime/control_flow.cpp` | ✅ | 控制流管理 |
| **Actor System** | `src/actor_system.cpp` | ✅ | Actor 框架适配 |
| **DSL Parser** | `src/dsl/workflow_parser.cpp` | ✅ | JSON 解析 |
| **EventLoop** | `src/event/eventloop_adapter/` | ✅ | 事件循环 |
| **Storage** | `src/storage/` | ✅ | 存储管理 |
| **Network** | `src/network/` | ✅ | 网络通信 |

### 1.2 示例程序

| 示例 | 文件 | 状态 |
|------|------|------|
| `example_basic` | `examples/basic_example.cpp` | ✅ |
| `example_dsl` | `examples/dsl_example.cpp` | ✅ |
| `example_metrics` | `examples/metrics_example.cpp` | ✅ |
| `example_control_flow` | `examples/control_flow_example.cpp` | ✅ |
| `example_node_execution` | `examples/node_execution_example.cpp` | ✅ |
| `example_p1_features` | `examples/p1_features_example.cpp` | ✅ |
| `example_storage` | `examples/storage_example.cpp` | ✅ |

### 1.3 测试文件

| 测试文件 | 说明 |
|----------|------|
| `test_adapter_isolation.cpp` | 适配器隔离测试 |
| `test_agent_gtest.cpp` | Agent 测试 |
| `test_workflow_gtest.cpp` | Workflow 测试 |
| `test_node_gtest.cpp` | Node 测试 |
| `test_databus_gtest.cpp` | DataBus 测试 |
| `test_event_gtest.cpp` | Event 系统测试 |
| `test_hot_reload.cpp` | 热重载测试 |

---

## 2. 构建和运行测试

### 2.1 使用构建脚本

```bash
# 完整构建
./build.sh build

# 运行测试
./build.sh test

# 运行示例
./build.sh example
```

### 2.2 手动构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

### 2.3 运行测试

```bash
cd build

# 使用 CTest
ctest --output-on-failure

# 直接运行 Google Test
./test_agentflow_gtest

# 运行特定测试
./test_agentflow_gtest --gtest_filter=AgentTest.*

# 运行适配器隔离测试
./test_adapter_isolation
```

### 2.4 运行示例

```bash
cd build

./example_basic
./example_metrics
./example_control_flow
./example_p1_features
./example_node_execution
```

---

## 3. 编写测试

### 3.1 测试文件命名

```
tests/test_<component>.cpp
```

### 3.2 测试结构

```cpp
#include <gtest/gtest.h>
#include "agentflow/framework/agent.h"
#include "agentflow/basic/logger.h"

using namespace agentflow;

class AgentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 测试前置准备
    agent_ = std::make_unique<framework::Agent>(framework::Agent::Config{});
  }

  void TearDown() override {
    // 测试后清理
    agent_.reset();
  }

  std::unique_ptr<framework::Agent> agent_;
};

TEST_F(AgentTest, CreateAgent) {
  ASSERT_NE(agent_, nullptr);
}

TEST_F(AgentTest, AgentLifecycle) {
  agent_->initialize();
  EXPECT_EQ(agent_->get_state(), framework::AgentState::INITIALIZED);

  agent_->start();
  EXPECT_EQ(agent_->get_state(), framework::AgentState::RUNNING);

  agent_->stop();
  EXPECT_EQ(agent_->get_state(), framework::AgentState::STOPPED);
}
```

### 3.3 简单测试模式

```cpp
bool test_feature_description() {
  // Arrange: 准备测试数据
  framework::Node::Config config;
  config.id = "node_1";
  config.name = "TestNode";
  framework::Node node(config);

  // Act: 执行操作
  node.initialize();

  // Assert: 验证结果
  if (node.get_state() != framework::NodeState::WAITING) {
    Logger::get_instance().log(LogLevel::ERROR, "Expected WAITING state");
    return false;
  }

  return true;
}
```

---

## 4. 核心组件使用示例

### 4.1 Agent

```cpp
#include "agentflow/framework/agent.h"

using namespace agentflow;

// 创建 Agent
framework::Agent::Config config;
config.id = "agent_001";
config.name = "My Agent";
config.policy = framework::Agent::LifecyclePolicy::FACTORY;
auto agent = std::make_shared<framework::Agent>(config);

// 生命周期管理
agent->initialize();
agent->start();
agent->stop();
```

### 4.2 Workflow

```cpp
#include "agentflow/framework/workflow.h"

// 创建 Workflow
framework::Workflow::Config config;
config.id = "workflow_001";
config.name = "My Workflow";
auto workflow = std::make_shared<framework::Workflow>(config);

// 添加节点
framework::Node::Config node_config;
node_config.id = "node_001";
auto node = std::make_shared<framework::Node>(node_config);
workflow->add_node(node);

// 添加边
framework::Edge::Config edge_config;
edge_config.id = "edge_001";
edge_config.source_node_id = "node_a";
edge_config.target_node_id = "node_b";
auto edge = std::make_shared<framework::Edge>(edge_config);
workflow->add_edge(edge);

// 验证 DAG
if (workflow->validate()) {
  // DAG 有效
}
```

### 4.3 Node

```cpp
#include "agentflow/framework/node.h"

// 创建节点
framework::Node::Config config;
config.id = "node_001";
config.name = "My Node";
config.type = "DataProcessor";
config.timeout_ms = 30000;
config.max_retries = 3;

framework::Node node(config);

// 设置输入
DataMap input;
input["key1"] = std::string("value1");
node.set_inputs(input);

// 执行节点
if (node.execute()) {
  const DataMap& outputs = node.get_outputs();
  // 处理输出
}

// 清理
node.cleanup();
```

### 4.4 DataBus

```cpp
#include "agentflow/runtime/databus.h"

using namespace agentflow::runtime;

HighSpeedDataBus databus(1024 * 1024);  // 1MB 容量

// 发布数据
databus.put("key1", std::string("value1"));
databus.put("key2", 12345);
databus.put("key3", 3.14);
databus.put("key4", true);

// 获取数据
DataVariant value;
if (databus.get("key1", value)) {
  // 处理数据
}

// 订阅数据变更
databus.subscribe("key1", [](const std::string& key, const DataVariant& value) {
  Logger::get_instance().log(LogLevel::INFO, "Data updated: " + key);
});
```

### 4.5 ZeroCopyDataBus

```cpp
#include "agentflow/runtime/zero_copy_databus.h"

using namespace agentflow::runtime;

// 创建数据总线 (1MB 容量)
ZeroCopyDataBus databus(1024 * 1024);

// 写入数据
databus.put("key1", std::string("value1"));

// 获取数据引用（零拷贝）
DataVariant value_ref;
if (databus.get_data_ref("key1", value_ref)) {
  // 直接访问数据，无拷贝
}

// 获取数据指针
const DataVariant* ptr = databus.get_data_ptr("key1");
if (ptr) {
  // 直接访问
}

// 读写锁控制
databus.lock_read();
// 读取操作...
databus.unlock_read();

databus.lock_write();
// 写入操作...
databus.unlock_write();
```

### 4.6 ControlFlowManager

```cpp
#include "agentflow/runtime/control_flow.h"

using namespace agentflow::runtime;

// 创建控制上下文
ControlContext context;
context.set_variable("score", "85");
context.set_variable("threshold", "80");

// 条件判断
ConditionEvaluator evaluator;
ConditionExpression expr("score", ConditionOperator::GREATER_THAN, "80");
if (evaluator.evaluate(expr, context, DataMap())) {
  // score > 80
}

// 控制流管理器
ControlFlowManager manager;
manager.initialize(context);

// 循环控制
LoopConfig for_loop;
for_loop.type = LoopType::FOR_LOOP;
for_loop.start_value = 0;
for_loop.end_value = 10;
for_loop.step = 1;
manager.enter_loop("example_loop", for_loop);

// 错误处理
ErrorHandlerConfig error_handler;
error_handler.strategy = ErrorStrategy::RETRY;
error_handler.max_retries = 3;
error_handler.retry_delay_ms = 500;
manager.set_error_handler("node_001", error_handler);
```

### 4.7 LocalChannel

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
DataPacket packet("key1", std::string("value1"), "source_001");
channel->send(packet);

// 接收数据
DataPacket received;
if (channel->receive(received, 5000)) {  // 5秒超时
  // 处理接收到的数据
}

// 订阅事件
size_t sub_id = channel->subscribe([](ChannelEvent event, const DataPacket& packet) {
  if (event == ChannelEvent::DATA_READY) {
    // 数据就绪
  }
});

// 取消订阅
channel->unsubscribe(sub_id);

// 关闭通道
channel->close();
```

### 4.8 Metrics

```cpp
#include "agentflow/runtime/metrics.h"

using namespace agentflow::runtime;

Metrics metrics;

// 计数器
metrics.increment_counter("workflow_requests_total");
metrics.increment_counter("workflow_requests_success");

// 直方图
metrics.record_histogram("node_execution_latency_ms", 45);
metrics.record_histogram("node_execution_latency_ms", 78);

// 仪表盘
metrics.set_gauge("active_workflows", 5);

// 获取统计信息
auto stats = metrics.get_histogram_stats("node_execution_latency_ms");
// stats.p50, stats.p90, stats.p95, stats.avg, stats.min, stats.max

// 导出
std::string text = metrics.export_text_format();
std::string json = metrics.export_json_format();
std::string prometheus = metrics.export_prometheus_format();
```

---

## 5. DataVariant 使用指南

`DataVariant` 是 `std::variant<int64_t, double, std::string, bool>` 类型。

### 5.1 创建 DataVariant

```cpp
#include "agentflow/common.h"

DataVariant value1 = std::string("string_value");
DataVariant value2 = 123;
DataVariant value3 = 45.67;
DataVariant value4 = true;
```

### 5.2 访问 DataVariant

```cpp
// 使用 std::visit
std::visit([](auto&& arg) {
  using T = std::decay_t<decltype(arg)>;
  if constexpr (std::is_same_v<T, std::string>) {
    Logger::get_instance().log(LogLevel::INFO, arg);
  } else if constexpr (std::is_same_v<T, int64_t>) {
    Logger::get_instance().log(LogLevel::INFO, std::to_string(arg));
  } else if constexpr (std::is_same_v<T, double>) {
    Logger::get_instance().log(LogLevel::INFO, std::to_string(arg));
  } else if constexpr (std::is_same_v<T, bool>) {
    Logger::get_instance().log(LogLevel::INFO, arg ? "true" : "false");
  }
}, value);
```

### 5.3 类型转换辅助函数

```cpp
auto variant_to_string = [](const DataVariant& v) -> std::string {
  return std::visit([](auto&& arg) -> std::string {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, std::string>) {
      return arg;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? "true" : "false";
    } else {
      return std::to_string(arg);
    }
  }, v);
};
```

---

## 6. 测试组织结构

```
tests/
├── test_agent_gtest.cpp         # Agent 测试
├── test_workflow_gtest.cpp      # Workflow 测试
├── test_node_gtest.cpp          # Node 测试
├── test_databus_gtest.cpp       # DataBus 测试
├── test_event_gtest.cpp         # Event 系统测试
├── test_eventloop_gtest.cpp     # EventLoop 测试
├── test_workflow_json_gtest.cpp # Workflow JSON 解析测试
├── test_agent_json_gtest.cpp    # Agent JSON 解析测试
├── test_local_channel_gtest.cpp # LocalChannel 测试
├── test_request_response_gtest.cpp # Request/Response 测试
├── test_adapter_isolation.cpp   # 适配器隔离测试
└── test_hot_reload.cpp          # 热重载测试
```

---

## 7. 开发建议

1. **遵循编码规范**: 参考 `docs/development.md`
2. **使用智能指针**: 避免原始指针
3. **线程安全**: 所有共享资源用 `std::mutex` 保护
4. **错误处理**: 使用返回值和异常机制
5. **日志记录**: 使用 Logger 记录重要事件

---

## 8. 已知问题和限制

1. **平台支持**: 目前仅支持 Linux/Unix 系统
2. **Protoactor 依赖**: 需要单独编译安装 Protoactor-cpp
3. **测试编译**: 部分测试文件由于 API 变化可能需要调整

---

*最后更新: 2026-02-17*
