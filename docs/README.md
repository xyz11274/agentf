# 文档指南

欢迎来到 AgentFlow 文档！所有项目文档已整合到此 `docs/` 目录，方便查阅和维护。

## 根目录文档

项目根目录保留最关键的三个文件：
- **[README.md](../README.md)** - 项目主导航和快速入门
- **[CONTRIBUTING.md](../CONTRIBUTING.md)** - 贡献指南（GitHub标准文件）
- **[REFACTORING.md](REFACTORING.md)** - 项目规划和功能状态

**建议首先阅读 [根目录 README.md](../README.md)，它包含了所有文档导航。**

---

## docs/ 目录文档列表

### 核心文档

| 文档 | 用途 |
|------|------|
| **[ARCHITECTURE.md](ARCHITECTURE.md)** | Framework/Runtime/MMDataBus 架构设计 |
| **[REFACTORING.md](REFACTORING.md)** | 功能规划、路线图和代码完备性评估 |
| **[api-reference.md](api-reference.md)** | API 参考手册 |
| **[../CONTRIBUTING.md](../CONTRIBUTING.md)** | 贡献流程、开发规范与提交流程 |
| **[README.md](README.md)** | 测试指南和方法 |

### 贡献和 CI/CD

| 文档 | 用途 |
|------|------|
| **[../CONTRIBUTING.md](../CONTRIBUTING.md)** | 贡献指南 |
| **[README.md](README.md)** | CI/CD 自动化管线 |

### 重构文档

| 文档 | 用途 |
|------|------|
| **[REFACTORING.md](REFACTORING.md)** | 代码重构记录和说明 |

---

## 快速导航

### 新用户入门

1. 查看 [../README.md](../README.md) - 项目概览和导航
2. 阅读 [ARCHITECTURE.md](ARCHITECTURE.md) - 了解系统架构
3. 查看 [api-reference.md](api-reference.md) - API 使用指南
4. 查看 [../CONTRIBUTING.md](../CONTRIBUTING.md) - 设置开发环境

### 贡献代码前

1. 阅读 [../CONTRIBUTING.md](../CONTRIBUTING.md) - 完整贡献指南
2. 查看 [README.md](README.md) - 测试要求
3. 查看 [README.md](README.md) - 本地质量检查

### 了解项目规划

1. 查看 [REFACTORING.md](REFACTORING.md) - 功能实现状态、路线图和代码评估
2. 查看 [REFACTORING.md](REFACTORING.md) - 快速规划导航

### CI/CD 和部署

1. 查看 [README.md](README.md) - CI/CD 完整指南

### 运维和系统

1. 查看 [../CONTRIBUTING.md](../CONTRIBUTING.md) - 开发与贡献规则
2. 查看 [REFACTORING.md](REFACTORING.md) - 构建和部署目标

---

## 按用途查找

| 我想... | 查看文档 |
|--------|--------|
| 学习系统架构 | [ARCHITECTURE.md](ARCHITECTURE.md) |
| 了解代码现状和优化点 | [REFACTORING.md](REFACTORING.md) - 代码完备性评估章节 |
| 查看 API 使用方法 | [api-reference.md](api-reference.md) |
| 开始开发 | [../CONTRIBUTING.md](../CONTRIBUTING.md) |
| 了解依赖库 | [../CONTRIBUTING.md](../CONTRIBUTING.md) |
| 贡献代码 | [../CONTRIBUTING.md](../CONTRIBUTING.md) |
| 了解项目规划 | [REFACTORING.md](REFACTORING.md) |
| 查询 CI/CD | [README.md](README.md) |
| 运行测试 | [README.md](README.md) |
| 了解重构进展 | [REFACTORING.md](REFACTORING.md) |

---

## 文档内容梗概

### architecture.md
- 5视图架构（场景、逻辑、开发、运行、部署）
- 模块设计详解
- 设计模式说明

### REFACTORING.md（合并版）
- 功能实现状态概览
- 待实现功能（按优先级）
- 版本规划
- **代码完备性评估**（原 code-review.md 内容）
  - 文档与代码一致性审查
  - 代码 Bug 清单
  - 测试覆盖度评估
  - 代码优化建议
  - 优化实施路线
- 代码质量目标
- 文档目标
- 构建和部署目标

### api-reference.md
- 核心 API 使用指南
- 代码示例
- 最佳实践

### CONTRIBUTING.md
- 贡献流程
- 提交与 PR 规范
- 基础本地验证要求

### README.md（测试合并）
- 测试方法
- 覆盖率要求
- 测试工具说明

### README.md（CI/CD 合并）
- 构建/测试/发布工作流
- 代码质量检查
- 本地开发指南
- 快速参考

---

## 维护指南

### 添加新文档
1. 在 `docs/` 目录创建 Markdown 文件（使用 lowercase-hyphen 命名）
2. 编写清晰的标题和目录
3. 在本文件中更新文档列表
4. 在 [../README.md](../README.md) 中添加导航

### 更新现有文档
1. 保持与代码同步
2. 更新相关链接
3. 在文档尾部记录更新日期

### 文档命名规范
- 使用小写字母和连字符（lowercase-hyphen）
- 例如：`api-reference.md`, `README.md`
- 例外：`README.md`（GitHub 标准）, `REFACTORING.md`（历史保留）

### 文档标准
- 使用 Markdown 格式
- 清晰的目录结构
- 包含代码示例
- 相关文档间的链接

---

## 获取帮助

**找不到需要的信息？**
- 查看 [../README.md](../README.md) 的完整导航

**文档有错误或不清晰？**
- 提交 Issue 或 Pull Request
- 参考 [../CONTRIBUTING.md](../CONTRIBUTING.md)

**需要快速参考？**
- 查看 [README.md](README.md) 的快速参考部分

---

**最后更新**：2026 年 2 月 28 日
**维护者**：@DanielKernel
**变更记录**：
- 2026-02-28: 同步刷新 MMNode 执行引擎下沉到 Worker、Coordinator task0 默认处理与 `coordinator_workflow` 配置说明
- 2026-02-21: 刷新 ARCHITECTURE.md 与 REFACTORING.md，补充 MMAgent/Runtime/MMWorkflow 当日优化落地与下一步优化方向
- 2026-02-18: 合并 roadmap.md 和 code-review.md 到 planning.md，合并 dependencies.md 到 development.md
- 2026-02-18: 合并 CI/CD 文档，统一命名规范为 lowercase-hyphen
- 2026-02-17: 添加 API 参考和代码审查文档


---

## 合并内容：原 ci-cd.md

# CI/CD 自动化管线

## 概述

AgentFlow 项目采用 GitHub Actions 实现完整的 CI/CD 自动化基础设施，支持 AMD64/ARM64 硬件平台和 Linux/macOS 操作系统。

## 支持平台

### 硬件架构
- **AMD64 (x86_64)**: Intel/AMD 64位处理器
- **ARM64 (aarch64)**: ARM 64位处理器 (Apple Silicon, AWS Graviton 等)

### 操作系统
- **Linux**: Ubuntu 24.04 LTS
  - AMD64: 原生 Ubuntu runners
  - ARM64: QEMU 用户空间模拟的交叉编译
- **macOS**: 多版本支持
  - AMD64: macOS 12+ (Intel 平台)
  - ARM64: macOS 14 (Apple Silicon - M1/M2/M3)

## 工作流文件

### 已创建的 MMWorkflow

- ✅ `.github/workflows/build.yml` - 多平台构建管线
- ✅ `.github/workflows/code-quality.yml` - 代码质量与风格检查
- ✅ `.github/workflows/tests.yml` - 综合测试框架
- ✅ `.github/workflows/release.yml` - 自动化发布
- ✅ `.github/workflows/manual_build.yml` - 手动构建触发
- ✅ `.github/workflows/badge_status.yml` - 状态徽章生成

### 配置文件

- ✅ `.github/workflows/build.yml + tests.yml` - 代码所有权和审查分配
- ✅ `.github/dependabot.yml` - 自动化依赖更新
- ✅ `.github/pull_request_template.md` - PR 模板
- ✅ `.github/ISSUE_TEMPLATE/` - Issue 模板（bug、feature、performance）

---

## 1. Build MMWorkflow (构建工作流)

**触发事件:**
- 推送到 `main` 或 `develop` 分支
- Pull Request 到 `main` 或 `develop` 分支

**矩阵配置:**
- Linux AMD64 (Ubuntu Latest)
- Linux ARM64 (Ubuntu Latest + QEMU)
- macOS AMD64 (macOS 12)
- macOS ARM64 (macOS 14)

**构建步骤:**
1. 检出源码
3. 配置 CMake (Release 模式)
4. 并行构建 (`-j` 自动检测核心数)
5. 上传构建产物（7天保留）

**构建配置:**
```cmake
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_CXX_STANDARD=17
-DAGENTFLOW_WITH_OPENSSL=ON
```

**特性:**
- 并行构建，自动 CPU 检测
- 编译器版本报告
- 构建产物存储（7天保留）

---

## 2. Code Quality MMWorkflow (代码质量工作流)

**触发事件:**
- 推送到 `main` 或 `develop` 分支
- Pull Request 到 `main` 或 `develop` 分支
- 手动触发（workflow_dispatch）

### 质量检查项

#### a. Clang Format 检查
- 验证代码格式一致性
- 使用 `.clang-format` 配置
- 报告需要格式化的文件
- 100% 通过要求

#### b. Clang Tidy 静态分析
- 代码质量问题检测
- 性能反模式识别
- 现代化建议
- 生成 `compile_commands.json` 用于分析

#### c. Cppcheck 静态分析
- 综合静态分析
- 内存泄漏检测
- 逻辑错误识别
- 可配置的严重性级别

#### d. Cpplint 风格检查
- Google C++ 风格指南强制执行
- 可自定义行长度（100字符）
- 版权和头文件验证

#### e. Sanitizer 编译检查
- AddressSanitizer (ASAN) 验证
- 提前检测内存安全问题

---

## 3. Test MMWorkflow (测试工作流)

**触发事件:**
- 推送到 `main` 或 `develop` 分支
- Pull Request 到 `main` 或 `develop` 分支

### 测试类型

#### a. 单元测试
- 多平台测试执行
- GTest 框架集成
- 并行测试执行
- 60秒超时限制
- 详细失败报告

#### b. 代码覆盖率分析
- LCOV 覆盖率收集
- Gcovr 报告生成
- Codecov 集成
- HTML 覆盖率报告（30天保留）
- 自动 PR 注释

#### c. 集成测试
- 示例应用测试
- MMWorkflow 验证
- 端到端功能检查

#### d. 内存泄漏检测
- Valgrind 工具集成
- 堆和栈泄漏检测
- 可配置的抑制规则

---

## 4. Release MMWorkflow (发布工作流)

**触发事件:**
- Git tag 推送 (v* 模式，如 v1.0.0)

**自动化发布流程:**
1. 多平台二进制构建
2. 产物打包和压缩
3. SHA256 校验和生成
4. GitHub Release 页面创建
5. 资源发布

**分发包内容:**
- 头文件、库文件、CMake 文件
- 平台特定命名约定
- 完整文档

---

## 5. Manual Build MMWorkflow (手动构建工作流)

**用户可配置参数:**
- 构建类型选择 (Debug/Release/RelWithDebInfo)
- 代码覆盖率启用/禁用
- Valgrind 内存检查
- Slack 通知

**使用方法:**
1. 进入 Actions 标签 → Manual Build & Test
2. 点击 "Run workflow"
3. 选择选项并运行

---

## 6. Status Badge MMWorkflow (状态徽章工作流)

自动生成项目状态徽章，显示：
- 构建状态
- 测试状态
- 代码覆盖率
- 代码质量

---

## 本地开发快速参考

### 克隆与构建

```bash
# 克隆仓库
git clone https://github.com/DanielKernel/AgentFlow.git
cd AgentFlow

# 构建
./build.sh build

# 测试
./build.sh test

# 运行示例
./build.sh example hello_world
```

### 提交规范

```bash
# 格式: <type>(<scope>): <subject>
git commit -m "feat(actor): add message priority queue"

# 类型: feat, fix, docs, style, refactor, perf, test, chore
# 范围: framework, actor, dsl, event, network, storage, serialization, runtime, build, ci
```

### 推送前代码质量检查

```bash
# 格式检查和修复
clang-format --dry-run --Werror src/*.cpp          # 检查
clang-format -i -style=file src/*.cpp               # 修复

# 静态分析
clang-tidy -p build src/your_file.cpp

# 代码分析
cppcheck --enable=all src/

# 风格检查
cpplint src/your_file.cpp
```

### Git 工作流

```bash
# 创建功能分支
git checkout -b feature/my-feature

# 提交更改
git add .
git commit -m "feat(scope): description"

# 推送并创建 PR
git push origin feature/my-feature
```

---

## 状态徽章

在 README.md 中添加以下徽章：

```markdown
[![Build](https://github.com/DanielKernel/AgentFlow/workflows/Build/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/build.yml)
[![Tests](https://github.com/DanielKernel/AgentFlow/workflows/Tests/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/tests.yml)
[![Code Quality](https://github.com/DanielKernel/AgentFlow/workflows/Code%20Quality/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/code-quality.yml)
[![codecov](https://codecov.io/gh/DanielKernel/AgentFlow/branch/main/graph/badge.svg)](https://codecov.io/gh/DanielKernel/AgentFlow)
```

---

## 分支保护规则

建议对 `main` 和 `develop` 分支设置以下保护规则：

1. **必须通过状态检查**
   - Build 工作流必须成功
   - Tests 工作流必须成功

2. **必须有审查批准**
   - 至少需要1个审查批准
   - 建议关键模块至少 1 位维护者审查

3. **强制线性历史**
   - 要求线性提交历史
   - 禁止合并提交

4. **禁止强制推送**
   - 保护历史记录完整性

---

## 查看构建状态

访问 Actions 页面：https://github.com/DanielKernel/AgentFlow/actions

### 平台支持状态

| 平台 | 架构 | 状态 |
|------|------|------|
| Ubuntu | x86_64 (AMD64) | ✅ |
| Ubuntu | ARM64 | ✅ |
| macOS 12 | x86_64 (AMD64) | ✅ |
| macOS 14 | ARM64 (Apple Silicon) | ✅ |

---

## 自动化检查摘要

每次推送和 PR 触发：

1. **构建检查** - 所有平台编译成功
2. **测试执行** - 单元测试、集成测试、内存泄漏检测

---

## 实施完成清单

✅ **工作流文件**: 2个 GitHub Actions workflows（build/tests）
✅ **平台支持**: Linux AMD64/ARM64, macOS AMD64/ARM64
✅ **测试覆盖**: 单元测试, 集成测试, 内存检测, 覆盖率
✅ **文档**: 构建与测试流程已合并到当前文档

---

## 故障排查

### 构建失败
- 检查依赖是否正确安装
- 验证 CMake 配置选项
- 查看详细构建日志

### 测试失败
- 查看失败测试的详细输出
- 检查平台特定问题
- 本地复现测试失败

### 代码质量问题
- 运行 `clang-format` 修复格式
- 使用 `clang-tidy` 修复静态分析问题
- 遵循 Google C++ 风格指南



---

## 合并内容：原 testing.md

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
| **MMAgent** | `src/agent/agent.cpp` | ✅ | MMAgent 基础功能 |
| **MMWorkflow** | `src/workflow/workflow.cpp` | ✅ | 工作流图调度、验证 |
| **MMNode** | `src/workflow/node/node.cpp` | ✅ | 节点执行、状态管理 |
| **MMEdge** | `src/workflow/edge/edge.cpp` | ✅ | 依赖关系定义 |
| **MMDataBus** | `src/runtime/communication/databus.cpp` | ✅ | 数据总线功能 |
| **ZeroCopyDataBus** | `src/runtime/communication/zero_copy_databus.cpp` | ✅ | 零拷贝数据总线 |
| **LocalChannel** | `src/runtime/communication/local_channel.cpp` | ✅ | 本地通道 |
| **ControlFlow** | `src/runtime/scheduler/control_flow.cpp` | ✅ | 控制流管理 |
| **Actor System** | `src/runtime/actor/actor_system.cpp` | ✅ | Actor 框架适配 |
| **DSL Parser** | `src/workflow/dsl/workflow_parser.cpp` | ✅ | JSON 解析 |
| **EventLoop** | `vendor/adapter/event/` | ✅ | 事件循环 |
| **Storage** | `src/runtime/storage/` | ✅ | 存储管理 |
| **Network** | `src/runtime/network/` | ✅ | 网络通信 |

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
| `test_agent_gtest.cpp` | MMAgent 测试 |
| `test_workflow_gtest.cpp` | MMWorkflow 测试 |
| `test_node_gtest.cpp` | MMNode 测试 |
| `test_databus_gtest.cpp` | MMDataBus 测试 |
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
    agent_ = std::make_unique<framework::MMAgent>(framework::MMAgent::Config{});
  }

  void TearDown() override {
    // 测试后清理
    agent_.reset();
  }

  std::unique_ptr<framework::MMAgent> agent_;
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
  framework::MMNode::Config config;
  config.id = "node_1";
  config.name = "TestNode";
  framework::MMNode node(config);

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

### 4.1 MMAgent

```cpp
#include "agentflow/framework/agent.h"

using namespace agentflow;

// 创建 MMAgent
framework::MMAgent::Config config;
config.id = "agent_001";
config.name = "My MMAgent";
config.policy = framework::MMAgent::LifecyclePolicy::FACTORY;
auto agent = std::make_shared<framework::MMAgent>(config);

// 生命周期管理
agent->initialize();
agent->start();
agent->stop();
```

### 4.2 MMWorkflow

```cpp
#include "agentflow/framework/workflow.h"

// 创建 MMWorkflow
framework::MMWorkflow::Config config;
config.id = "workflow_001";
config.name = "My MMWorkflow";
auto workflow = std::make_shared<framework::MMWorkflow>(config);

// 添加节点
framework::MMNode::Config node_config;
node_config.id = "node_001";
auto node = std::make_shared<framework::MMNode>(node_config);
workflow->add_node(node);

// 添加边
framework::MMEdge::Config edge_config;
edge_config.id = "edge_001";
edge_config.source_node_id = "node_a";
edge_config.target_node_id = "node_b";
auto edge = std::make_shared<framework::MMEdge>(edge_config);
workflow->add_edge(edge);

// 验证工作流图
if (workflow->validate()) {
  // 工作流图有效
}
```

### 4.3 MMNode

```cpp
#include "agentflow/framework/node.h"

// 创建节点
framework::MMNode::Config config;
config.id = "node_001";
config.name = "My MMNode";
config.type = "DataProcessor";
config.timeout_ms = 30000;
config.max_retries = 3;

framework::MMNode node(config);

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

### 4.4 MMDataBus

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
├── test_agent_gtest.cpp         # MMAgent 测试
├── test_workflow_gtest.cpp      # MMWorkflow 测试
├── test_node_gtest.cpp          # MMNode 测试
├── test_databus_gtest.cpp       # MMDataBus 测试
├── test_workflow_json_gtest.cpp # MMWorkflow JSON 解析测试
├── test_agent_json_gtest.cpp    # MMAgent JSON 解析测试
├── test_local_channel_gtest.cpp # LocalChannel 测试
├── test_request_response_gtest.cpp # Request/Response 测试
├── test_adapter_isolation.cpp   # 适配器隔离测试
└── test_hot_reload.cpp          # 热重载测试
```

---

## 7. 开发建议

1. **遵循编码规范**: 参考 `../CONTRIBUTING.md`
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
