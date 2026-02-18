# AgentFlow 开发指南

> 本文档整合了开发环境配置、编程规范、实现指南、工作流程和依赖管理。

**最后更新**: 2026年2月18日

---

## 目录

1. [快速开始](#1-快速开始)
2. [项目结构](#2-项目结构)
3. [编程规范](#3-编程规范)
4. [开发工作流](#4-开发工作流)
5. [组件概述](#5-组件概述)
6. [代码示例](#6-代码示例)
7. [工具配置](#7-工具配置)
8. [依赖管理](#8-依赖管理)

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
- `std::shared_ptr` / `std::unique_ptr`
- `std::thread` 和线程原语
- `auto` 关键字
- 基于范围的 for 循环
- `std::optional`, `std::variant`, `std::string_view`
- 结构化绑定
- `if constexpr`
- `[[nodiscard]]`, `[[maybe_unused]]`

**禁止使用**：
- C++20/23 特性（协程、modules、concepts）
- 原始指针管理内存
- C 风格数组
- `goto` 语句

### 3.4 内存管理

```cpp
// 推荐
auto wf = std::make_shared<Workflow>("wf1");
std::unique_ptr<Logger> logger = std::make_unique<Logger>();

// 避免
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

## 8. 依赖管理

### 8.1 依赖概览

#### 依赖分类

| 类型 | 依赖 | 必需/可选 | 说明 |
|------|------|-----------|------|
| **核心** | C++17 标准库 | 必需 | 基础数据结构和算法 |
| **核心** | CMake >= 3.10 | 必需 | 构建系统 |
| **核心** | Threads | 必需 | 多线程支持 |
| **可选** | Protoactor-cpp | 可选 | Actor 模型实现 |
| **可选** | OpenSSL | 可选 | SSL/TLS 加密 |
| **可选** | libwebsockets | 可选 | WebSocket 支持 |
| **可选** | libevent | 可选 | 事件循环 |
| **可选** | RapidJSON | 可选 | JSON 解析 |
| **可选** | Protobuf | 可选 | 序列化 |
| **测试** | Google Test | 可选 | 单元测试框架 |

#### 依赖检测机制

项目通过 CMake 自动检测这些依赖，如果未找到则使用 stub 实现：

```cmake
# 核心依赖
find_package(Threads REQUIRED)

# 可选依赖（未找到时使用stub）
find_package(protoactor CONFIG QUIET)
find_package(OpenSSL QUIET)
find_package(libwebsockets CONFIG QUIET)
find_package(Protobuf QUIET)
find_package(libevent CONFIG QUIET)

# RapidJSON (header-only)
find_path(RAPIDJSON_INCLUDE_DIR NAMES rapidjson/document.h)
```

### 8.2 可选依赖详解

#### Protoactor-cpp（Actor 框架）

- **GitHub**: https://github.com/DanielKernel/Protoactor-cpp.git
- **用途**: Actor 模型实现，提供分布式 Actor 通信和消息机制
- **C++ 标准**: C++17

**核心依赖**：

| 依赖 | 版本要求 | 用途 |
|------|---------|------|
| grpc | >= 1.30 | gRPC 通信库，用于远程 Actor 通信 |
| protobuf | >= 3.10 | Protocol Buffer，用于消息序列化 |
| openssl | >= 1.1 | SSL/TLS 加密，安全通信支持 |
| zlib | >= 1.2 | 数据压缩，减少网络传输数据量 |

**编译要求**：
- CMake >= 3.10
- GCC 7+ / Clang 5+ / MSVC 2017+

**支持平台**：
- Linux (x86_64, ARM64)
- macOS (x86_64, Apple Silicon ARM64)
- Windows (x86_64, CMake 支持)

#### OpenSSL（SSL/TLS 加密）

- **GitHub**: https://github.com/openssl/openssl.git
- **官网**: https://www.openssl.org/
- **用途**: SSL/TLS 安全通信
- **功能**: SSL/TLS 协议支持、证书管理、加密通信

#### libwebsockets（WebSocket 支持）

- **GitHub**: https://github.com/warmcat/libwebsockets.git
- **官网**: https://libwebsockets.org/
- **用途**: WebSocket 协议实现
- **功能**: WebSocket 服务器和客户端、实时双向通信

#### libevent（事件循环）

- **GitHub**: https://github.com/libevent/libevent.git
- **官网**: https://libevent.org/
- **用途**: 高性能事件循环
- **功能**: 跨平台事件循环、高性能 I/O 多路复用

#### RapidJSON（JSON 解析）

- **GitHub**: https://github.com/Tencent/rapidjson.git
- **用途**: JSON 序列化和反序列化
- **特点**: Header-only 库，高性能 JSON 解析

#### Protobuf（序列化）

- **GitHub**: https://github.com/protocolbuffers/protobuf.git
- **官网**: https://developers.google.com/protocol-buffers
- **用途**: 二进制序列化协议
- **特点**: 高效的二进制序列化，跨语言支持

### 8.2.1 第三方库GitHub仓库地址

以下为AgentFlow依赖的所有第三方库的GitHub仓库地址：

| 依赖库 | GitHub地址 |
|--------|-----------|
| Protoactor-cpp | https://github.com/DanielKernel/Protoactor-cpp.git |
| OpenSSL | https://github.com/openssl/openssl.git |
| libwebsockets | https://github.com/warmcat/libwebsockets.git |
| libevent | https://github.com/libevent/libevent.git |
| RapidJSON | https://github.com/Tencent/rapidjson.git |
| protobuf | https://github.com/protocolbuffers/protobuf.git |

这些库的源码也可以通过vendor脚本下载：
```bash
./vendor/vendor.sh
```

### 8.3 依赖安装指南

#### Ubuntu/Debian

```bash
# 核心编译工具
sudo apt-get install -y build-essential cmake git pkg-config

# Protoactor-cpp 依赖
sudo apt-get install -y \
    libgrpc++-dev \
    libgrpc-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libssl-dev \
    zlib1g-dev

# AgentFlow 可选依赖
sudo apt-get install -y \
    libwebsockets-dev \
    libevent-dev \
    rapidjson-dev

# 测试框架
sudo apt-get install -y libgtest-dev

# 编译安装 Protoactor-cpp
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

#### macOS（Homebrew）

```bash
# 核心编译工具
xcode-select --install
brew install cmake git

# Protoactor-cpp 依赖
brew install grpc protobuf openssl zlib

# AgentFlow 可选依赖
brew install libwebsockets libevent rapidjson

# 测试框架
brew install googletest

# 设置 OpenSSL 路径（macOS 可能需要）
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)

# 编译安装 Protoactor-cpp
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)
sudo make install
```

#### CentOS/RHEL/Fedora

```bash
# Fedora
sudo dnf install -y \
    gcc-c++ cmake git \
    grpc-devel protobuf-devel \
    openssl-devel zlib-devel \
    libevent-devel rapidjson-devel \
    libwebsockets-devel \
    gtest-devel

# CentOS/RHEL（需要 EPEL）
sudo yum install -y epel-release
sudo yum install -y \
    gcc-c++ cmake git \
    grpc-devel protobuf-devel \
    openssl-devel zlib-devel \
    libevent-devel
```

#### Arch Linux

```bash
sudo pacman -S --noconfirm \
    base-devel cmake git \
    grpc protobuf openssl zlib \
    libevent rapidjson libwebsockets \
    gtest
```

### 8.4 依赖隔离策略

#### 核心原则

1. **适配器模式**：所有对外部依赖的调用都通过适配器层进行
2. **接口隔离**：AgentFlow 内部只依赖抽象接口，不依赖具体实现
3. **版本兼容**：适配器层负责处理上游版本差异
4. **可测试性**：提供 Mock 实现用于单元测试，避免外部依赖
5. **条件编译**：在没有外部依赖的情况下，使用存根实现保证编译

#### 隔离层架构

```
+-----------------------------------+
|  AgentFlow 核心 (common.h...)     |
|  - Agent, Workflow, Node          |
|  - DataBus, EventManager          |
+----------------+------------------+
                 | (仅依赖)
+----------------v------------------+
| 适配器接口层 (adapter.h)          |
|  - IActor                         |
|  - IContext                       |
|  - IActorSystem                   |
+----------------+------------------+
                 | (隐藏实现)
+----------------v------------------+
| 实现选择 (条件编译)                |
|  WITH_PROTOACTOR ? 完整实现       |
|  : stub 实现                      |
+-----------------------------------+
```

#### 接口定义示例

```cpp
namespace agentflow::actor::adapter {

/// 抽象 Actor 接口 - AgentFlow 内部使用
class IActor {
public:
  virtual ~IActor() = default;
  virtual void on_receive(IContext* ctx) = 0;
  virtual std::string get_actor_type() const = 0;
};

/// 抽象 ActorSystem 接口
class IActorSystem {
public:
  virtual ~IActorSystem() = default;
  virtual std::shared_ptr<IActorRef> spawn(ActorFactory factory) = 0;
  virtual void shutdown() = 0;
};

}
```

#### 条件编译

```cpp
#ifdef AGENTFLOW_WITH_PROTOACTOR
  #include "adapter/protoactor_impl.h"
#else
  #include "adapter/stub_impl.h"
#endif
```

#### 工厂模式

```cpp
class ActorSystemFactory {
public:
  static std::shared_ptr<IActorSystem> create() {
#ifdef AGENTFLOW_WITH_PROTOACTOR
    return std::make_shared<ProtoactorSystemImpl>();
#else
    return std::make_shared<StubActorSystemImpl>();
#endif
  }
};
```

### 8.5 Stub 实现说明

#### 概述

对于每个可选依赖，AgentFlow 都提供了 stub 实现：

- **作用**: 在依赖库未安装时提供基本功能
- **限制**: 功能受限，仅用于测试和基本场景
- **使用**: CMake 自动检测依赖，未找到时使用 stub

#### Stub 实现列表

| 依赖 | Stub 文件 | 说明 |
|------|-----------|------|
| Actor | `src/actor/stub_impl.cpp` | 基本Actor模拟 |
| SSL | `include/.../openssl_impl.h` | 无加密通信 |
| WebSocket | `include/.../libwebsockets_impl.h` | 无 WebSocket 支持 |
| Event Loop | `include/.../libevent_impl.h` | 简单事件处理 |
| JSON | `src/dsl/json_adapter_stub.cpp` | 基础 JSON 解析 |
| Protobuf | `include/.../protobuf_impl.h` | 无序列化 |

#### 适配器模式示例

```cpp
// 接口定义（项目内部）
class ISslAdapter { ... };

// 具体实现（第三方库）
class SslOpensslAdapter : public ISslAdapter { ... };

// Stub 实现（降级方案）
class SslStubAdapter : public ISslAdapter { ... };

// 工厂方法（根据依赖选择实现）
std::shared_ptr<ISslAdapter> create_ssl_adapter() {
#ifdef AGENTFLOW_WITH_OPENSSL
  return std::make_shared<SslOpensslAdapter>();
#else
  return std::make_shared<SslStubAdapter>();
#endif
}
```

### 8.6 故障排除

#### 找不到 gRPC 库

```bash
# Ubuntu/Debian
sudo apt-get install libgrpc++-dev libgrpc-dev

# macOS
brew install grpc
```

#### Protobuf 版本冲突

```bash
# 检查版本
protoc --version

# 重新安装
sudo apt-get remove libprotobuf-dev protobuf-compiler
sudo apt-get install libprotobuf-dev protobuf-compiler

# macOS
brew upgrade protobuf
```

#### OpenSSL 找不到（macOS）

```bash
brew install openssl
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)

# 或在 cmake 时指定
cmake .. -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

#### 运行时找不到共享库

```bash
# Linux
sudo ldconfig /usr/local/lib
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

# macOS
export DYLD_LIBRARY_PATH=/usr/local/lib:$DYLD_LIBRARY_PATH
```

#### CMake 找不到 Protoactor

```bash
# 验证安装
ls -l /usr/local/lib/libprotoactor*
ls -l /usr/local/include/protoactor/

# 设置 CMAKE_PREFIX_PATH
cmake .. -DCMAKE_PREFIX_PATH=/usr/local
```

#### 自定义安装路径

```bash
# 安装到自定义路径
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make install

# 配置环境
export PATH=$HOME/.local/bin:$PATH
export LD_LIBRARY_PATH=$HOME/.local/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig:$PKG_CONFIG_PATH
```

### 8.7 注意事项

1. **不要重新实现第三方库**: 这些库都有成熟实现，AgentFlow 仅提供适配和集成
2. **依赖冲突**: 确保所有依赖的版本兼容
3. **编译顺序**: Protoactor-cpp 需要先编译，其他库可通过包管理器安装
4. **生产环境**: 建议启用所有依赖，使用完整功能而非 stub
5. **许可证**: 第三方库有自己的开源许可证，使用时请遵守相应协议

### 8.8 核心隔离文件

| 文件 | 说明 |
|------|------|
| `include/agentflow/actor/adapter.h` | 适配器接口定义 |
| `include/agentflow/actor/adapter/protoactor_impl.h` | Protoactor 实现 |
| `include/agentflow/actor/adapter/stub_impl.h` | Stub 实现 |
| `src/actor/adapter/*.cpp` | 适配器实现 |

---

## 相关文档

- [架构设计](architecture.md) - 系统5视图架构
- [项目规划](planning.md) - 功能实现状态和路线图
- [测试指南](testing.md) - 测试方法
- [贡献指南](contributing.md) - 贡献流程

---

**最后更新**: 2026年2月18日
