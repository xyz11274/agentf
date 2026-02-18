# 项目目录结构优化说明

本文档说明了 AgentFlow 项目目录结构的优化工作。

## 目录结构总览

```
AgentFlow/
├── build.sh              # 构建入口脚本
├── build/                # 构建输出目录和CMake配置
│   ├── cmake/           # CMake 配置文件
│   └── install_dependencies.sh
├── include/             # 头文件目录
│   ├── external/        # 公共 API 头文件（随库发布）
│   └── internal/        # 内部实现头文件（仅供内部使用）
├── src/                 # 核心业务源代码
├── vendor/              # 三方依赖和适配器
│   ├── vendor.sh       # 三方依赖下载脚本
│   └── adapter/        # 三方依赖适配器实现
├── tests/               # 测试目录
│   ├── unit/           # 单元测试
│   └── functional/     # 功能测试
├── examples/           # 示例代码
├── docs/               # 项目文档
├── configs/            # 配置文件
└── proto/              # Protobuf 定义
```

## 优化内容

### 1. 头文件目录重组

**目录结构：**
```
include/
├── external/            # 公共 API 头文件（供外部调用方使用）
│   ├── core/           # 核心组件：Agent, Workflow, Node, Edge
│   ├── runtime/        # 运行时：DataBus, HotReload, Metrics 等
│   ├── actor/          # Actor 系统
│   ├── network/        # 网络组件
│   ├── serialization/  # 序列化接口
│   ├── event/          # 事件系统
│   ├── dsl/            # DSL 解析器接口
│   ├── storage/        # 存储接口
│   ├── utils/          # 工具类
│   └── common.h        # 公共定义
└── internal/            # 内部实现头文件（仅供内部系统使用）
    ├── actor/adapter/  # Actor 适配器实现
    ├── dsl/json_adapter/ # JSON 适配器实现
    ├── event/eventloop_adapter/ # 事件循环适配器实现
    ├── network/        # 网络内部实现
    │   ├── ssl_adapter/
    │   ├── websocket_adapter/
    │   └── private_protocol.h
    ├── serialization/proto_adapter/ # Protobuf 适配器实现
    └── testing/        # 测试工具
```

**说明：**
- `external/` - 公共 API 头文件，随 agentflow 库发布，外部调用方使用
- `internal/` - 内部实现头文件，外部无法直接调用
- 不再有 `agentflow` 中间目录，简化 include 路径

**include 方式：**
```cpp
// 公共 API（外部使用）
#include "core/agent.h"
#include "runtime/databus.h"

// 内部实现（仅内部系统使用）
#include "actor/adapter/protoactor_impl.h"
```

### 2. 测试目录重组

**目录结构：**
```
tests/
├── unit/                # 单元测试
│   ├── cpp/            # C++ 单元测试代码
│   ├── data/           # 测试数据
│   ├── lib/            # 测试库（如 hot_reload_test）
│   └── scripts/        # 测试脚本
│       ├── python/
│       └── shell/
├── functional/          # 功能测试
│   ├── cpp/            # 功能测试代码
│   └── scripts/        # 功能测试脚本
└── jars/               # Java 测试
```

**说明：**
- `unit/` - 单元测试，针对单个组件的测试
- `functional/` - 功能测试，包括性能测试、集成测试等

### 3. 三方依赖与适配器目录

**目录结构：**
```
vendor/
├── vendor.conf         # 三方依赖配置文件
├── vendor.sh           # 三方依赖下载脚本
├── README.md           # 使用文档
├── adapter/            # 三方依赖适配器实现
│   ├── actor/          # Actor 框架适配器
│   │   ├── protoactor_impl.cpp
│   │   └── stub_impl.cpp
│   ├── dsl/            # DSL 解析器适配器
│   │   ├── json_adapter_rapidjson.cpp
│   │   └── json_adapter_stub.cpp
│   ├── eventloop/      # 事件循环适配器
│   │   └── libevent_impl.cpp
│   ├── network/        # 网络适配器（预留）
│   └── serialization/  # 序列化适配器（预留）
└── <downloaded>/       # 下载的三方依赖（gitignore）
```

**说明：**
- `vendor/` - 所有三方依赖相关内容统一放置
- `vendor.conf` - 三方依赖下载配置
- `vendor/adapter/` - 与三方依赖相关的适配器代码
- `src/` - 保留核心业务代码，不包含适配器实现

### 4. 源代码目录

**目录结构：**
```
src/
├── core/               # 核心组件实现
├── runtime/            # 运行时实现
├── actor/              # Actor 系统核心（不包含适配器）
├── dsl/                # DSL 解析器核心（不包含适配器）
├── event/              # 事件系统核心（不包含适配器）
├── network/            # 网络组件核心
├── serialization/      # 序列化核心
├── storage/            # 存储实现
└── utils/              # 工具类
```

**说明：**
- 适配器实现代码已移动到 `vendor/adapter/` 目录
- `src/` 只包含核心业务逻辑

### 5. 构建系统

**构建入口：**
- `build.sh` - 项目根目录的构建入口脚本

**构建输出：**
- `build/` - 构建输出目录，包含编译产物和 CMake 配置

## 使用说明

### 构建项目

```bash
# 构建项目
./build.sh build

# 清理构建
./build.sh clean

# 重新构建
./build.sh rebuild

# 运行测试
./build.sh test

# 运行示例
./build.sh example basic
```

### 下载三方依赖

```bash
# 下载所有配置的三方依赖
./vendor/vendor.sh

# 查看已配置的依赖
./vendor/vendor.sh --list

# 更新已下载的依赖
./vendor/vendor.sh --update

# 清理并重新下载
./vendor/vendor.sh --clean
```

### 安装构建依赖

```bash
# 安装系统依赖和开发环境
./build/install_dependencies.sh
```

### 运行测试

```bash
# 运行单元测试
./build.sh test

# 运行功能测试
./tests/functional/scripts/run_performance_tests.sh
```

## CMake 配置说明

### Include 目录配置

```cmake
# 公共 API（用于外部用户）
include_directories(${CMAKE_SOURCE_DIR}/include/external)

# 内部实现（仅用于源代码编译）
include_directories(${CMAKE_SOURCE_DIR}/include/internal)
```

### 源文件组织

```cmake
# 核心业务代码
set(AGENTFLOW_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/core/...
    ${AGENTFLOW_SOURCE_DIR}/runtime/...
    ...
)

# 适配器代码
set(AGENTFLOW_ADAPTER_SOURCES
    ${AGENTFLOW_VENDOR_DIR}/adapter/actor/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/dsl/...
    ...
)
```

## 设计原则

1. **关注点分离**：
   - `include/external/` - 公共 API，面向外部用户
   - `include/internal/` - 内部实现，面向开发者
   - `src/` - 核心业务逻辑
   - `vendor/adapter/` - 三方依赖适配

2. **模块化**：
   - 适配器与核心业务分离
   - 单元测试与功能测试分离

3. **清晰的边界**：
   - 外部用户只能使用 `external/` 中的接口
   - 内部实现细节对用户透明

---

**更新日期**：2026 年 2 月 18 日
