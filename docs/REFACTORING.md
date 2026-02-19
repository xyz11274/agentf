# 项目目录结构优化说明

本文档说明了 AgentFlow 项目目录结构的优化工作。

## 目录结构总览

```
AgentFlow/
├── CMakeLists.txt        # 顶层构建入口
├── build.sh              # 构建入口脚本
├── build/                # 构建输出目录和CMake配置
│   ├── cmake/           # CMake 配置文件
│   └── install_dependencies.sh
├── include/             # 头文件目录
│   ├── external/        # 公共 API 头文件（随库发布）
│   └── internal/        # 内部实现头文件（仅供内部使用）
├── src/                 # 核心业务源代码
├── vendor/              # 三方依赖和适配器
│   ├── vendor.conf     # 三方依赖配置文件
│   ├── vendor.sh       # 三方依赖下载脚本
│   ├── README.md       # 使用文档
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
├── external/           # 公共 API 头文件（供外部调用方使用）
│   ├── common.h        # 公共类型定义（DataVariant, AgentId, 枚举等）
│   ├── core/           # 开发编排框架核心接口（agent.h, workflow.h, node.h, edge.h）
│   ├── agent/          # agent实例化：提供agent实例生命周期管理接口
│   ├── workflow/       # workflow/node/edge 定制开发外部接口
│   │   └── node/       # node定制开发子接口
│   ├── runtime/        # 运行框架公开接口
│   │   ├── agent_runtime.h
│   │   ├── databus.h / zero_copy_databus.h / local_channel.h
│   │   ├── control_flow.h / hot_reload.h / code_loader.h
│   │   ├── metrics.h / event.h
│   │   ├── scheduler/  # 调度器子接口
│   │   ├── communication/ # 通信子接口
│   │   ├── storage/    # 存储子接口
│   │   └── actor/      # Actor 运行时子接口
│   ├── actor/          # Actor 系统公开接口（actor_system.h, node_actor.h, workflow_actor.h 等）
│   ├── dsl/            # DSL 解析公开接口（json_adapter.h, workflow_parser.h）
│   ├── event/          # 事件循环公开接口（eventloop_adapter.h, adapter_factory.h）
│   ├── network/        # 网络通信公开接口（websocket_adapter.h, ssl_adapter.h 等）
│   ├── serialization/  # 序列化公开接口（proto_adapter.h, adapter_factory.h）
│   ├── storage/        # 持久化公开接口（storage.h, storage_manager.h 等）
│   └── utils/          # 工具库公开接口（logger.h, thread_pool.h）
└── internal/           # 内部实现头文件（仅供内部系统使用）
    ├── adapter/        # 三方依赖适配实现头文件
    │   ├── actor/      # actor框架适配（protoactor_impl.h, stub_impl.h）
    │   ├── dsl/        # DSL 适配（rapidjson_impl.h, stub_impl.h）
    │   ├── event/      # 事件循环适配（libevent_impl.h）
    │   ├── network/    # 网络适配（libwebsockets_impl.h, openssl_impl.h）
    │   └── serialization/ # 序列化适配（protobuf_impl.h）
    ├── actor/          # Actor 框架内部实现头文件
    ├── dsl/            # DSL 内部实现头文件
    ├── event/          # 事件循环内部实现头文件
    ├── network/        # 网络内部实现头文件（private_protocol.h 等）
    ├── serialization/  # 序列化内部实现头文件
    └── testing/        # 测试工具（performance_metrics.h 等）
```

**说明：**
- `external/` - 公共 API 头文件，随 agentflow 库发布，外部调用方使用
- `internal/` - 内部实现头文件，外部无法直接调用

### 2. 测试目录重组

**目录结构：**
```
tests/
├── unit/               # 单元测试
│   ├── cpp/            # C++ 单元测试代码（test_agent, test_workflow, test_databus 等）
│   ├── data/           # 测试数据（JSON 配置、期望输出等）
│   │   ├── expected/   # 期望输出数据
│   │   └── temporary/  # 临时测试数据
│   ├── jars/           # Java 相关测试资源
│   ├── lib/            # 测试库（hot_reload_test 等）
│   └── scripts/        # 测试脚本
│       ├── python/     # Python 测试脚本
│       └── shell/      # Shell 测试脚本
└── functional/         # 功能测试
    ├── cpp/            # 功能测试代码（test_performance.cpp 等）
    └── scripts/        # 功能测试脚本
```

**说明：**
- `unit/` - 单元测试，针对单个组件的测试，按照和src/相同的子目录组织单元测试代码
- `functional/` - 功能测试，包括性能测试、集成测试等

### 3. 三方依赖与适配器目录

**目录结构：**
```
src/                    # 业务代码相关实现，和三方实现独立目录
vendor/                 # 三方依赖相关实现
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
│   ├── event/          # 事件循环适配器（adapter_factory.cpp, libevent_impl.cpp）
│   ├── eventloop/      # 事件循环适配器（备用路径，libevent_impl.cpp）
│   ├── network/        # 网络适配器（adapter_factory.cpp）
│   └── serialization/  # 序列化适配器（adapter_factory.cpp）
└── <downloaded>/       # 下载的三方依赖（gitignore）
```

**说明：**
- `src/` - 保留核心业务代码，不包含适配器实现
- `vendor/` - 所有三方依赖相关内容统一放置
- `vendor.conf` - 三方依赖下载配置
- `vendor/adapter/` - 与三方依赖相关的适配器代码

### 4. 源代码目录

**目录结构：**
```
src/
├── core/               # 开发编排框架核心实现（Agent/Workflow/Node/Edge）
├── agent/              # Agent 实例管理实现（生命周期、会话、消息、任务）
├── workflow/           # Workflow 管理实现
│   ├── dsl/            # DSL 解析实现
│   ├── edge/           # Edge 实现
│   └── node/           # Node 实现（code_loader, node_actor 等）
├── runtime/            # 运行时实现
│   ├── scheduler/      # 调度器（agent_runtime, control_flow）
│   ├── communication/  # 通信（databus, hot_reload, local_channel, zero_copy_databus）
│   ├── storage/        # 存储（agent_storage, storage_manager）
│   ├── actor/          # Actor 系统运行时（actor_system, node_actor 等）
│   └── network/        # 运行时网络（private_protocol, request, response）
├── actor/              # Actor 系统实现（ask_pattern, message_serializer 等）
├── dsl/                # DSL 适配器工厂实现
├── event/              # 事件循环适配器工厂实现
├── network/            # 网络层实现（private_protocol, request, response）
├── serialization/      # 序列化实现
├── storage/            # 存储实现
└── utils/              # 工具实现（logger, thread_pool）
    └── log/            # 日志子模块
```

**说明：**
- `core/`：开发编排框架核心实体（Agent、Workflow、Node、Edge）的基础实现
- `agent/`：实现以下模块：1、agent实例管理模块：支持agent生命周期及状态管理，比如创建（参考agent包里面包含的workflow流程文件和node二进制实现进行实例化）->CREATED，agent实例运行->RUNNING、暂停->PAUSED、停止->STOPPED；2、Agent实例对应会话管理，创建、销毁会话等；3、Agent收发消息管理：agent实例接收的消息缓存、映射到会话及任务；4、Agent任务管理：创建执行任务实例，任务实例与workflow实例映射，Agent只做任务的状态管理，任务到workflow实例需要等待调度
- `workflow/`：实现以下模块：1、workflow管理模块：比如创建workflow实例（低码化DSL、高码化C++接口）；2、node实现（code_loader、node_actor等）；3、edge实现；4、DSL解析
- `runtime/`：实现调度和执行：1、scheduler/：master将workflow到worker调度，所有agent实例生成的workflow统一调度到worker上；control_flow控制流；2、communication/：databus高速数据共享、hot_reload热重载、local_channel本地通道；3、storage/：agent和workflow持久化；4、actor/：运行时Actor系统
- `vendor/` 目录已移出 `src/`，三方依赖适配器统一在 `vendor/adapter/` 中管理

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
    ${AGENTFLOW_SOURCE_DIR}/agent/...
    ${AGENTFLOW_SOURCE_DIR}/workflow/...
    ${AGENTFLOW_SOURCE_DIR}/runtime/...
    ${AGENTFLOW_SOURCE_DIR}/actor/...
    ${AGENTFLOW_SOURCE_DIR}/dsl/...
    ${AGENTFLOW_SOURCE_DIR}/event/...
    ${AGENTFLOW_SOURCE_DIR}/network/...
    ${AGENTFLOW_SOURCE_DIR}/serialization/...
    ${AGENTFLOW_SOURCE_DIR}/storage/...
    ${AGENTFLOW_SOURCE_DIR}/utils/...
)

# 适配器代码（三方依赖，条件编译）
set(AGENTFLOW_ADAPTER_SOURCES
    ${AGENTFLOW_VENDOR_DIR}/adapter/actor/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/dsl/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/event/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/network/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/serialization/...
)
```

## 设计原则

1. **关注点分离**：
   - `include/external/` - 公共 API，面向外部用户
   - `include/internal/` - 内部实现，面向开发者
   - `src/` - 核心业务逻辑
   - `vendor/` - 三方依赖适配

2. **模块化**：
   - 适配器与核心业务分离
   - 单元测试与功能测试分离

3. **清晰的边界**：
   - 外部用户只能使用 `external/` 中的接口
   - 内部实现细节对用户透明

---

**更新日期**：2026 年 2 月 19 日
