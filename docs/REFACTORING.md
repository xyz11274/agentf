# 项目目录结构优化说明

本文档说明了 AgentFlow 项目目录结构的优化工作。

## 优化内容

### 1. 脚本目录重组

**变更前：**
```
scripts/
├── vendor.sh
├── install_dependencies.sh
├── run_performance_tests.sh
├── verify_cicd_setup.sh
├── docker/
├── helm/
└── README.md
```

**变更后：**
```
vendor/
├── vendor.sh           # 三方依赖下载脚本
├── README.md          # 使用文档
└── adapter/           # 三方依赖适配器目录

build/
├── cmake/             # CMake 配置文件（从项目根目录移入）
├── install_dependencies.sh  # 依赖安装脚本
└── build.sh           # 构建脚本

tests/
├── functional/
│   ├── run_performance_tests.sh  # 性能测试脚本
│   └── test_performance.cpp
└── unit/
    └── cpp/          # 单元测试代码
```

**说明：**
- 三方依赖相关脚本移动到 `vendor/` 目录
- 构建相关脚本移动到 `build/` 目录
- 测试相关脚本移动到 `tests/` 目录
- 删除了 `docker/`、`helm/` 等部署相关目录和 `verify_cicd_setup.sh` 等脚本
- 删除了 `scripts/` 目录

### 2. 头文件目录重组

**变更前：**
```
include/agentflow/
├── core/
├── network/
├── runtime/
├── actor/
├── ...
└── testing/
```

**变更后：**
```
include/
├── agentflow/         # 保留原有结构（向后兼容）
├── external/agentflow/ # 公共 API 头文件
│   ├── core/
│   ├── network/
│   ├── runtime/
│   ├── actor/
│   ├── serialization/
│   ├── event/
│   ├── dsl/
│   ├── storage/
│   └── utils/
└── internal/agentflow/ # 内部实现头文件
    ├── actor/adapter/
    ├── network/websocket_adapter/
    ├── network/ssl_adapter/
    ├── serialization/proto_adapter/
    ├── event/eventloop_adapter/
    ├── dsl/json_adapter/
    ├── network/
    └── testing/
```

**说明：**
- `external/agentflow/` - 公共 API 头文件，供外部使用
- `internal/agentflow/` - 内部实现头文件，包括适配器实现等
- 原 `agentflow/` 目录保留，用于向后兼容

### 3. 测试目录重组

**变更前：**
```
tests/
├── unit/
│   └── legacy_src/
├── test_performance.cpp
├── run_performance_tests.sh
└── ...
```

**变更后：**
```
tests/
├── unit/
│   └── cpp/           # 单元测试代码
│       ├── test_*.cpp
│       └── test_main.cpp
└── functional/        # 功能测试
    ├── test_performance.cpp
    └── run_performance_tests.sh
```

**说明：**
- `unit/cpp/` - 单元测试，针对单个组件的测试
- `functional/` - 功能测试，包括性能测试等

### 4. 三方依赖适配器目录

新增 `vendor/adapter/` 目录，结构如下：
```
vendor/adapter/
├── network/          # 网络适配器实现
├── serialization/    # 序列化适配器实现
├── eventloop/        # 事件循环适配器实现
├── dsl/              # DSL 解析器适配器实现
└── actor/            # Actor 框架适配器实现
```

### 5. CMake 配置文件移动

**变更前：**
```
cmake/
├── CompilerOptions.cmake
└── Dependencies.cmake
```

**变更后：**
```
build/cmake/
├── CompilerOptions.cmake
└── Dependencies.cmake
```

## 使用说明

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

### 配置三方依赖

编辑项目根目录的 `vendor.conf` 文件，格式如下：
```
name|url|branch|target_dir
```

示例：
```
nlohmann_json|https://github.com/nlohmann/json.git|develop|json
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

# 运行性能测试
./tests/functional/run_performance_tests.sh
```

## 迁移指南

### 更新代码中的 include 路径

如果代码中使用了 `include/agentflow/` 路径，可以考虑迁移到新的目录结构：

**公共 API：**
```cpp
// 旧方式
#include "agentflow/core/agent.h"

// 新方式（推荐，用于公共 API）
#include "agentflow/core/agent.h"  // 仍然有效
```

**内部实现：**
```cpp
// 新方式（用于内部实现）
#include "internal/agentflow/network/websocket_adapter/libwebsockets_impl.h"
```

### CMake 配置更新

现有的 CMake 配置无需修改，因为：
1. `include/agentflow/` 目录仍存在
2. `include_directories(${AGENTFLOW_INCLUDE_DIR})` 会自动包含所有子目录

如需使用新的目录结构，可以在 `CMakeLists.txt` 中添加：
```cmake
# 公共 API
include_directories(${CMAKE_SOURCE_DIR}/include/external)

# 内部实现（仅用于源代码）
include_directories(${CMAKE_SOURCE_DIR}/include/internal)
```

## 注意事项

1. **向后兼容性**：保留了原有的目录结构，现有代码无需立即修改
2. **Git 忽略**：`vendor/` 目录已添加到 `.gitignore`，不会提交到仓库
3. **文档更新**：相关文档（如 contributing.md）已更新路径引用
4. **构建系统**：现有的 CMakeLists.txt 无需修改即可正常工作

## 后续工作建议

1. 逐步将源代码中的 include 路径更新为使用新的目录结构
2. 为 vendor/adapter 目录下的适配器添加更多实现
3. 更新 CI/CD 配置以适应新的目录结构
4. 更新 README 文档以反映新的项目结构

---

**更新日期**：2026 年 2 月 18 日
