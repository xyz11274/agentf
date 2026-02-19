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
├── external/           # 公共 API 头文件（供外部调用方使用）
│   ├── agent/          # agent实例化：提供agent实例生命周期管理接口
│   ├── node/           # node定制开发：基于node的控制面对接接口、运行面接口开发定制node
└── internal/           # 内部实现头文件（仅供内部系统使用）
    ├── agent/          # agent实现，单agent实例、多会话、多消息、多workflow任务，可以再增加子目录
    ├── workflow/       # workflow实现，包含node和edge，可以再增加子目录
    ├── runtime/        # master、worker部署，多agent实例、多workflow任务、多worker综合调度
    ├── vendor/         # 三方依赖适配
    │   ├── json_adapter/
    │   ├── websocket_adapter/
    │   ├── proto_adapter/
    │   └── actor_adapter/
    └── testing/        # 测试工具
```

**说明：**
- `external/` - 公共 API 头文件，随 agentflow 库发布，外部调用方使用
- `internal/` - 内部实现头文件，外部无法直接调用

### 2. 测试目录重组

**目录结构：**
```
tests/
├── unit/               # 单元测试
│   ├── agent/          # C++ 单元测试代码
│   ├── workflow/       # 测试数据
│   ├── runtime/        # 测试库（如 hot_reload_test）
├── functional/         # 功能测试
    ├── cpp/            # 功能测试代码
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
│   ├── eventloop/      # 事件循环适配器
│   │   └── libevent_impl.cpp
│   ├── communication/  # 通信适配器（预留）
│   └── serialization/  # 序列化适配器（预留）
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
├── agent/              # 提供agent对象实现，对agent进行生命周期管理，并由agent对外提供接口
├── workflow/           # 提供workflow对象实现，包含node和edge实现，对workflow进行多实例管理，提供node等开发接口
├── runtime/            # 运行时实现
├── vendor/             # 三方依赖适配代码
└── utils/              # 工具类
```

**说明：**
- agent：实现以下模块：1、agent实例管理模块：支持agent生命周期及状态管理，比如创建（参考agent包里面包含的workflow流程文件和node二进制实现进行实例化）->CREATED，agent实例运行->RUNNING、暂停->PAUSED、停止->STOPPED；2、Agent实例对应会话管理，创建、销毁会话等；3、Agent收发消息管理：agent实例接收的消息缓存、映射到会话及任务；4、Agent任务管理：创建执行任务实例，任务实例与workflow实例映射，Agent只做任务的状态管理，任务到workflow实例需要等待调度
- workflow：实现以下模块：1、worflow管理模块：比如创建worflow实例（低码化DSL、高码化C++接口）
- runtime：实现调度和执行：1、master将workflow到worker调度，所有agent实例生成的workflow统一调度到worker上；2、所有agent实例对应任务统一调度到不同worker上的workflow实例执行；3、所有workflow实例上的Node可以调度到不同的worker上

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
   - `vendor/` - 三方依赖适配

2. **模块化**：
   - 适配器与核心业务分离
   - 单元测试与功能测试分离

3. **清晰的边界**：
   - 外部用户只能使用 `external/` 中的接口
   - 内部实现细节对用户透明

---

**更新日期**：2026 年 2 月 18 日
