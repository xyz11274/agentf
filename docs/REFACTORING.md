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
└── proto/              # Protobuf 定义
```

## 优化内容

### 1. 头文件目录重组

**目录结构：**
```
include/
├── external/           # 公共 API 头文件（供外部调用方使用）
│   ├── agent/          # agent实例化：提供agent实例生命周期管理接口
│   │   ├── agent.h
│   │   └── common.h
│   └── node/           # node定制开发：基于node控制面/运行面接口开发定制node
│       ├── node.h
│       ├── hot_reload.h
│       └── node/
│           └── code_loader.h
└── internal/           # 内部实现头文件（仅供内部系统使用）
    ├── agent/          # agent实现，单agent实例、多会话、多消息、多workflow任务
    ├── workflow/       # workflow实现，包含node和edge
    ├── runtime/        # master/worker部署，多agent、多workflow、多worker调度
    ├── vendor/         # 三方依赖适配
    │   ├── json_adapter/
    │   ├── websocket_adapter/
    │   └── private_protocol.h
    ├── serialization/
    │   └── proto_adapter/  # Protobuf 适配器实现
    ├── actor_adapter/  # Actor 适配器实现
    └── testing/        # 测试工具
```

**说明：**
- `external/` 仅暴露 `agent/` 与 `node/` 最小外部接口
- `internal/` 承载 `runtime`、`storage`、`network`、`workflow` 等全部内部实现能力

### 2. 测试目录重组

**目录结构：**
```
tests/
├── unit/               # 单元测试
│   ├── agent/          # C++ 单元测试代码（当前预留空目录）
│   ├── workflow/       # 测试数据/脚本/测试库与单元测试代码（当前承载既有 unit 内容）
│   │   ├── cpp/
│   │   ├── data/
│   │   ├── jars/
│   │   ├── lib/
│   │   └── scripts/
│   └── runtime/        # 测试库（如 hot_reload_test，当前预留空目录）
└── functional/         # 功能测试
    ├── cpp/            # 功能测试代码（test_performance.cpp 等）
    └── scripts/        # 功能测试脚本
```

**说明：**
- `unit/agent` 与 `unit/runtime` 当前为预留目录
- 现有 unit 测试内容按约定统一落在 `unit/workflow/`（含 cpp/data/lib/scripts/jars）
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
├── agent/              # Agent 实例管理实现（agent.cpp）
├── workflow/           # Workflow 管理实现
│   ├── dsl/            # DSL 解析实现
│   ├── edge/           # Edge 子模块实现
│   ├── node/           # Node 子模块实现（code_loader, node_actor 等）
│   ├── workflow.cpp    # Workflow 核心实现
│   ├── node.cpp        # Node 核心实现
│   └── edge.cpp        # Edge 核心实现
├── runtime/            # 运行时实现
│   ├── scheduler/      # 调度器（agent_runtime, control_flow）
│   ├── communication/  # 通信（databus, hot_reload, local_channel, zero_copy_databus）
│   ├── storage/        # 存储（agent_storage, storage_manager）
│   ├── actor/          # Actor 系统运行时（actor_system, node_actor 等）
│   └── network/        # 运行时网络（private_protocol, request, response）
├── vendor/             # 三方适配实现（actor/dsl/event/network/serialization/storage）
└── utils/              # 工具实现（logger, thread_pool）
```

**说明：**
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
    ${AGENTFLOW_SOURCE_DIR}/agent/...
    ${AGENTFLOW_SOURCE_DIR}/workflow/...
    ${AGENTFLOW_SOURCE_DIR}/runtime/...
    ${AGENTFLOW_SOURCE_DIR}/vendor/...
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
   - `include/external/agent` + `include/external/workflow` - 公共 API，面向外部用户
   - `include/internal/` - 内部实现，面向开发者
   - `src/` - 核心业务逻辑
   - `vendor/` - 三方依赖适配

2. **模块化**：
   - 适配器与核心业务分离
   - 单元测试与功能测试分离

3. **清晰的边界**：
   - 外部用户只能使用 `external/agent` 与 `external/workflow` 接口
   - 内部实现细节对用户透明

---

**更新日期**：2026 年 2 月 19 日
