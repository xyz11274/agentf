# AgentFlow 开发指南

> 本文档整合了开发环境配置、编程规范、实现指南和工作流程。

---

## 目录

1. [快速开始](#1-快速开始)
2. [项目结构](#2-项目结构)
3. [编程规范](#3-编程规范)
4. [开发工作流](#4-开发工作流)
5. [组件概述](#5-组件概述)
6. [代码示例](#6-代码示例)
7. [工具配置](#7-工具配置)

---

## 1. 快速开始

### 1.1 前置要求

- CMake 3.10+
- C++17 兼容编译器（GCC 7+ / Clang 5+ / MSVC 2017+）
- Linux/Unix 环境
- Git

### 1.2 环境设置

```bash
# 克隆仓库
git clone https://github.com/DanielKernel/AgentFlow.git
cd AgentFlow

# 构建项目
./build.sh build

# 运行测试
./build.sh test

# 运行示例
./build.sh example
```

### 1.3 手动构建

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# 运行测试
./test_agentflow

# 运行示例
./example_basic
```

---

## 2. 项目结构

```
AgentFlow/
├── include/agentflow/          # 头文件
│   ├── common.h               # 通用类型定义
│   ├── basic/                 # 基础工具
│   │   └── logger.h           # 日志系统
│   ├── framework/             # 框架核心
│   │   ├── agent.h            # Agent 定义
│   │   ├── workflow.h         # Workflow DAG
│   │   ├── node.h             # 节点定义
│   │   └── edge.h             # 边和依赖
│   ├── runtime/               # 运行时
│   │   ├── agent_runtime.h    # 运行时引擎
│   │   ├── databus.h          # 数据总线
│   │   ├── zero_copy_databus.h# 零拷贝总线
│   │   └── control_flow.h     # 控制流
│   ├── actor/                 # Actor 系统
│   ├── dsl/                   # DSL 解析
│   ├── network/               # 网络通信
│   ├── storage/               # 存储管理
│   └── serialization/         # 序列化
├── src/                       # 源代码实现
├── tests/                     # 测试代码
├── examples/                  # 示例程序
│   └── dsl/                   # DSL 示例
├── docs/                      # 文档
├── configs/                   # 配置文件
├── proto/                     # Protobuf 定义
├── scripts/                   # 构建脚本
│   ├── docker/                # Docker 配置
│   └── helm/                  # Kubernetes 部署
├── CMakeLists.txt             # 构建配置
└── build.sh                   # 构建脚本
```

---

## 3. 编程规范

### 3.1 命名规范

| 类型 | 规范 | 示例 |
|------|------|------|
| 类/结构体 | PascalCase | `class AgentRuntime` |
| 枚举 | PascalCase (类型)，UPPER_CASE (值) | `enum class NodeState { WAITING }` |
| 函数/方法 | snake_case | `void execute_node()` |
| 变量 | snake_case | `int task_id` |
| 私有成员 | snake_case_ 后缀 | `std::mutex mutex_` |
| 常量/宏 | UPPER_CASE | `MAX_WORKERS` |

### 3.2 代码风格

- **缩进**: 2 空格（不使用 TAB）
- **行长**: 80 字符（建议），120 字符（硬限制）
- **括号**: K&R 风格

```cpp
// 正确示例
if (condition) {
  // code
} else {
  // code
}

for (int i = 0; i < 10; ++i) {
  // code
}

class Example {
 public:
  void method();

 private:
  int member_;
};
```

### 3.3 C++17 特性使用

**允许使用**：
- ✅ `std::shared_ptr` / `std::unique_ptr`
- ✅ `std::thread` 和线程原语
- ✅ `auto` 关键字
- ✅ 基于范围的 for 循环
- ✅ `std::optional`, `std::variant`, `std::string_view`
- ✅ 结构化绑定
- ✅ `if constexpr`
- ✅ `[[nodiscard]]`, `[[maybe_unused]]`

**禁止使用**：
- ❌ C++20/23 特性（协程、modules、concepts）
- ❌ 原始指针管理内存
- ❌ C 风格数组
- ❌ `goto` 语句

### 3.4 内存管理

```cpp
// ✅ 推荐
auto wf = std::make_shared<Workflow>("wf1");
std::unique_ptr<Logger> logger = std::make_unique<Logger>();

// ❌ 避免
Workflow* wf = new Workflow();
delete wf;
```

### 3.5 线程安全

所有共享资源必须使用 `std::mutex` 保护：

```cpp
class Agent {
 private:
  std::map<WorkflowId, std::shared_ptr<Workflow>> workflows_;
  mutable std::mutex workflows_mutex_;

 public:
  std::shared_ptr<Workflow> get_workflow(const WorkflowId& id) const {
    std::lock_guard<std::mutex> lock(workflows_mutex_);
    auto it = workflows_.find(id);
    return it != workflows_.end() ? it->second : nullptr;
  }
};
```

### 3.6 头文件组织

```cpp
#ifndef AGENTFLOW_AGENT_H
#define AGENTFLOW_AGENT_H

// 1. 标准库
#include <string>
#include <memory>
#include <mutex>

// 2. 项目内部头文件
#include "agentflow/common.h"

namespace agentflow {

// 类定义...

}  // namespace agentflow

#endif  // AGENTFLOW_AGENT_H
```

### 3.7 注释规范

```cpp
/// @brief 创建并返回新的工作流实例
/// @param workflow_name 工作流唯一标识符
/// @return 创建的工作流的共享指针
/// @throws std::runtime_error 如果工作流已存在
std::shared_ptr<Workflow> create_workflow(const std::string& workflow_name);
```

---

## 4. 开发工作流

### 4.1 创建特性分支

```bash
git checkout -b feature/my-feature
# 或
git checkout -b fix/my-bugfix
```

### 4.2 编码和格式化

```bash
# 格式化代码
clang-format -r -i include/ src/ tests/ examples/

# 检查代码质量（可选）
clang-tidy -p build include/**/*.h src/*.cpp
```

### 4.3 编译和测试

```bash
# 完整构建
./build.sh build

# 运行测试
./build.sh test

# 运行示例
./build.sh example
```

### 4.4 提交规范

**提交信息格式**：

```
<type>: <subject>

<body>

<footer>
```

**类型**：
- `feat`: 新功能
- `fix`: 缺陷修复
- `test`: 测试相关
- `refactor`: 代码重构
- `style`: 代码风格
- `doc`: 文档更新
- `chore`: 构建或工具

**示例**：

```
feat: implement workflow DAG scheduling

Add complete directed acyclic graph support for workflow
scheduling with topological ordering and validation.

- Implement Workflow class with add_node/add_edge methods
- Add topological sort algorithm
- Add validation for cycle detection

Implements #42
```

### 4.5 提交前检查清单

- [ ] 代码编译无错误和警告
- [ ] 所有测试通过
- [ ] 代码格式化正确
- [ ] 命名规范统一
- [ ] 没有原始指针
- [ ] 共享资源已同步
- [ ] const 正确性已应用
- [ ] 新增功能有对应测试

---

## 5. 组件概述

### 5.1 开发编排框架 (`framework/`)

#### Agent
- 管理多个 Workflow 和共享资源
- 支持三种生命周期策略：Per-Request、Factory、Singleton
- 提供统一的对外接口

#### Workflow
- DAG（有向无环图）结构
- 由 Node 和 Edge 组成
- 支持动态 DSL 加载和更新
- 拓扑验证

#### Node
- 基本执行单元
- 支持多种执行模式（C++、Shell、Python、SO）
- 可配置错误处理（重试、超时）
- 状态管理

#### Edge
- 定义 Node 之间的依赖关系
- 支持条件表达式
- 支持多种数据传递方式

### 5.2 运行框架 (`runtime/`)

#### AgentRuntime
- 三层 Master-Worker 调度引擎
- Agent 层：管理多 Agent 实例
- Workflow 层：DAG 调度和状态管理
- Node 层：Worker 池执行任务

#### HighSpeedDataBus
- Node 之间的高速数据传递
- 订阅/发布机制
- 线程安全操作

#### ZeroCopyDataBus
- 零拷贝数据共享
- 共享内存实现
- 读写锁并发控制

#### ControlFlowManager
- 条件分支执行
- 循环迭代控制
- 错误处理策略

---

## 6. 代码示例

### 6.1 创建 Agent 和 Workflow

```cpp
#include "agentflow/framework/agent.h"
#include "agentflow/framework/workflow.h"

using namespace agentflow;

// 创建 Agent
framework::Agent::Config agent_config;
agent_config.id = "my_agent";
agent_config.name = "MyAgent";
agent_config.policy = framework::Agent::LifecyclePolicy::FACTORY;
auto agent = std::make_shared<framework::Agent>(agent_config);

// 创建 Workflow
framework::Workflow::Config workflow_config;
workflow_config.id = "my_workflow";
workflow_config.name = "MyWorkflow";
auto workflow = std::make_shared<framework::Workflow>(workflow_config);

// 创建并添加节点
framework::Node::Config node_config;
node_config.id = "task_1";
node_config.name = "TaskNode";
node_config.execution_mode = framework::Node::ExecutionMode::CPP_FUNCTION;
auto node = std::make_shared<framework::Node>(node_config);
workflow->add_node(node);

// 添加 Workflow 到 Agent
agent->add_workflow(workflow);

// 初始化并启动
agent->initialize();
agent->start();

// ... 使用 Agent ...

agent->stop();
```

### 6.2 使用数据总线

```cpp
#include "agentflow/runtime/databus.h"

using namespace agentflow::runtime;

HighSpeedDataBus databus(1024 * 1024);  // 1MB 容量

// 发布数据
databus.put("key1", std::string("value1"));
databus.put("key2", 12345);

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

### 6.3 使用控制流

```cpp
#include "agentflow/runtime/control_flow.h"

using namespace agentflow::runtime;

ControlFlowManager manager;
ControlContext context;

// 设置变量
context.set_variable("score", "85");

// 条件判断
ConditionExpression expr("score", ConditionOperator::GREATER_THAN, "80");
ConditionEvaluator evaluator;
if (evaluator.evaluate(expr, context, DataMap())) {
  // score > 80
}

// 循环控制
LoopConfig loop_config;
loop_config.type = LoopType::FOR_LOOP;
loop_config.start_value = 0;
loop_config.end_value = 10;
loop_config.step = 1;
manager.enter_loop("my_loop", loop_config);

// 错误处理
ErrorHandlerConfig handler;
handler.strategy = ErrorStrategy::RETRY;
handler.max_retries = 3;
handler.retry_delay_ms = 1000;
manager.set_error_handler("node_1", handler);
```

### 6.4 从 DSL 加载

```cpp
#include "agentflow/dsl/workflow_parser.h"

// 从 JSON 文件加载
std::string json_content = R"({
  "workflow": {
    "id": "wf_001",
    "name": "Sample Workflow",
    "nodes": [...],
    "edges": [...]
  }
})";

framework::Workflow::Config config;
config.id = "wf_001";
config.name = "Sample Workflow";

auto workflow = std::make_shared<framework::Workflow>(config);
if (workflow->load_from_dsl(json_content)) {
  // DSL 加载成功
}
```

---

## 7. 工具配置

### 7.1 VS Code 推荐扩展

```json
{
  "recommendations": [
    "llvm-vs-code-extensions.vscode-clangd",
    "notskm.clang-tidy",
    "xaver.clang-format",
    "EditorConfig.EditorConfig",
    "ms-vscode.cpptools"
  ]
}
```

### 7.2 项目配置文件

| 文件 | 用途 |
|------|------|
| `.clang-format` | 代码格式化配置（Google 风格） |
| `.clang-tidy` | 静态分析和代码质量检查 |
| `.editorconfig` | 跨编辑器的一致性配置 |
| `.cmake-format` | CMake 文件格式化配置 |
| `.gitignore` | Git 忽略规则 |

### 7.3 常用命令

```bash
# 完整构建
./build.sh build

# 运行测试
./build.sh test

# 代码格式化
clang-format -r -i include/ src/ tests/ examples/

# 代码质量检查
clang-tidy -p build include/**/*.h src/*.cpp

# 清理构建
./build.sh clean

# 完整清理重建
rm -rf build && ./build.sh build && ./build.sh test

# 代码统计
find . -name "*.cpp" -o -name "*.h" | xargs wc -l
```

---

*最后更新: 2026-02-17*
