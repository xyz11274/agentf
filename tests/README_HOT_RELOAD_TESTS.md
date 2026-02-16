# AgentFlow Hot Reload Test Documentation

本文档描述了AgentFlow热更新功能的测试用例和运行说明。

## 目录

- [测试文件结构](#测试文件结构)
- [环境要求](#环境要求)
- [构建测试代码](#构建测试代码)
- [运行测试](#运行测试)
- [测试用例清单](#测试用例清单)

---

## 测试文件结构

```
tests/
├── test_hot_reload.cpp          # 主测试文件（Google Test）
├── test_jar/                   # JAR包测试源文件
│   ├── HotReloadTest.java   # Java源代码
│   ├── ANIFEST.MF           # JAR清单文件
│   └── build.sh             # JAR构建脚本
├── test_lib/                   # SO库测试源文件
│   ├── hot_reload_test.cpp   # SO库源代码
│   └── CMakeLists.txt        # SO库构建文件
└── scripts/                    # 测试脚本文件
    ├── test_script.sh         # Shell测试脚本
    ├── test_python.py          # Python测试脚本
    ├── test_nodejs.js         # Node.js测试脚本
    ├── test_lua.lua           # Lua测试脚本
    └── test_ruby.rb           # Ruby测试脚本
```

---

## 环境要求

### 必需环境

- CMake >= 3.10
- C++17兼容编译器（GCC、Clang、MSVC）
- Google Test >= 1.10

### 可选环境

以下环境用于特定测试：

| 测试类型 | 所需环境 | 命令检查 |
|---------|---------|----------|
| Shell脚本 | Bash | `which bash` |
| Python脚本 | Python 3.x | `which python3` |
| Node.js脚本 | Node.js | `which node` |
| Lua脚本 | Lua | `which lua` |
| Ruby脚本 | Ruby | `which ruby` |
| JAR包 | JDK 8+ | `javac -version` |
| SO库 | GCC/Clang | 自动检测 |

---

## 构建测试代码

### 1. 构建AgentFlow库

```bash
cd build
cmake ..
make agentflow
```

### 2. 构建热更新测试

```bash
cd build
cmake ..
make test_hot_reload
```

### 3. 构建测试SO库（可选）

```bash
cd tests/test_lib
cmake .
make
```

### 4. 构建测试JAR包（可选）

```bash
cd tests/test_jar
./build.sh
```

---

## 运行测试

### 运行所有热更新测试

```bash
cd build
./test_hot_reload
```

### 运行特定测试

```bash
# 运行特定测试组
./test_hot_reload --gtest_filter=HotReloadTest.ShellScript_*

# 运行特定测试
./test_hot_reload --gtest_filter=HotReloadTest.ShellScript_BasicExecution

# 运行重复测试
./test_hot_reload --gtest_repeat=3 --gtest_filter=HotReloadTest.ShellScript_HotReload
```

### 测试脚本直接执行

```bash
# Shell脚本
chmod +x tests/scripts/*.sh
tests/scripts/test_script.sh

# Python脚本
chmod +x tests/scripts/*.py
python3 tests/scripts/test_python.py

# Node.js脚本
node tests/scripts/test_nodejs.js

# Lua脚本
lua tests/scripts/test_lua.lua

# Ruby脚本
ruby tests/scripts/test_ruby.rb

# JAR包（需要先构建）
cd tests/test_jar
./build.sh
java -jar HotReloadTest.jar version
```

---

## 测试用例清单

### 1. Shell脚本测试 (6个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `ShellScript_BasicExecution` | 基本Shell脚本执行 | CodeLoader |
| `ShellScript_HotReload` | Shell脚本热更新 | HotReload + VersionManager |
| `ShellScript_WithArguments` | 带参数的Shell脚本执行 | CodeLoader |
| `ShellScript_ErrorHandling` | Shell脚本错误处理 | CodeLoader |
| `ShellScript_WithWorkingDir` | 指定工作目录执行 | CodeLoader |
| `ShellScript_Timeout` | 超时控制 | CodeLoader |

### 2. Python脚本测试 (4个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `PythonScript_BasicExecution` | 基本Python脚本执行 | CodeLoader |
| `PythonScript_HotReload` | Python脚本热更新 | HotReload + VersionManager |
| `PythonScript_WithArguments` | 带参数的Python脚本执行 | CodeLoader |
| `PythonScript_WithInputs` | 输入数据传递 | CodeLoader |
| `PythonScript_ErrorHandling` | Python脚本错误处理 | CodeLoader |

### 3. Node.js脚本测试 (2个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `NodeJSScript_BasicExecution` | 基本Node.js脚本执行 | CodeLoader |
| `NodeJSScript_HotReload` | Node.js脚本热更新 | HotReload + VersionManager |

### 4. Lua脚本测试 (2个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `LuaScript_BasicExecution` | 基本Lua脚本执行 | CodeLoader |
| `LuaScript_HotReload` | Lua脚本热更新 | HotReload + VersionManager |

### 5. Ruby脚本测试 (2个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `RubyScript_BasicExecution` | 基本Ruby脚本执行 | CodeLoader |
| `RubyScript_HotReload` | Ruby脚本热更新 | HotReload + VersionManager |

### 6. JAR包测试 (23个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `JarFile_TypeDetection` | JAR文件类型检测 | HotReload |
| `JarFile_BasicExecution` | JAR基本执行 | CodeLoader |
| `JarFile_WithMainClass` | 指定主类执行 | CodeLoader |
| `JarFile_WithArguments` | 带参数的JAR执行 | CodeLoader |
| `JarFile_HotReload` | JAR热更新 | HotReload + VersionManager |
| `JarFile_WithWorkingDir` | 指定工作目录 | CodeLoader |
| `JarFile_WithInputs` | 输入数据传递 | CodeLoader |
| `JarFile_WithTimeout` | JAR执行超时 | CodeLoader |
| `JarFile_Error_InvalidFile` | 无效JAR文件 | CodeLoader |
| `JarFile_Error_MalformedJAR` | 格式错误的JAR | CodeLoader |
| `JarFile_MultipleMainClasses` | 多主类处理 | CodeLoader |
| `JarFile_VersionTracking` | 版本跟踪 | HotReload + VersionManager |
| `JarFile_VersionRollback` | 版本回滚 | HotReload + VersionManager |
| `JarFile_MaxVersionsLimit` | 最大版本限制 | HotReload |
| `JarFile_AutoReload` | 自动重载 | HotReload |
| `JarFile_NodeIntegration` | Node集成测试 | Node |
| `JarFile_EnvironmentVariables` | 环境变量传递 | CodeLoader |
| `JarFile_LargeJAR` | 大JAR文件处理 | HotReload |
| `JarFile_BackupBeforeReload` | 重载前备份 | HotReload |
| `JarFile_ConcurrentExecution` | 并发执行 | CodeLoader |
| `JarFile_ValidateBeforeReload` | 重载前验证 | HotReload |
| `JarFile_ChecksumVerification` | 校验和验证 | HotReload |
| `JarFile_MultipleWatches` | 多文件监控 | HotReload + FileWatcher |
| `JarFile_ReloadTimeout` | 重载超时 | HotReload |
| `JarFile_InvalidExtension` | 无效扩展名 | HotReload |

### 7. 版本管理测试 (5个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `VersionManager_CreateMultipleVersions` | 创建多版本 | VersionManager |
| `VersionManager_MaxVersionsLimit` | 最大版本限制 | VersionManager |
| `VersionManager_Rollback` | 版本回滚 | VersionManager |
| `VersionManager_RollbackToInvalidVersion` | 回滚到无效版本 | VersionManager |

### 8. 文件监控测试 (3个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `FileWatcher_StartStop` | 启动/停止文件监控 | FileWatcher |
| `FileWatcher_NonExistentFile` | 不存在的文件 | FileWatcher |
| `FileWatcher_MultipleFiles` | 多文件监控 | FileWatcher |

### 9. 自动重载测试 (2个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `AutoReload_Enabled` | 自动重载启用 | HotReload + FileWatcher |
| `AutoReload_Disabled` | 自动重载禁用 | HotReload |

### 10. 备份测试 (1个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `Backup_BeforeReload` | 重载前备份 | HotReload |

### 11. 可执行类型检测测试 (7个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `TypeDetection_ShellScript` | Shell脚本类型检测 | HotReload |
| `TypeDetection_PythonScript` | Python脚本类型检测 | HotReload |
| `TypeDetection_NodeJSScript` | Node.js脚本类型检测 | HotReload |
| `TypeDetection_LuaScript` | Lua脚本类型检测 | HotReload |
| `TypeDetection_RubyScript` | Ruby脚本类型检测 | HotReload |
| `TypeDetection_Unknown` | 未知类型检测 | HotReload |
| `TypeDetection_JarFile` | JAR文件类型检测 | HotReload |

### 12. Node集成测试 (4个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `Node_ShellScriptNode` | Shell脚本Node | Node + HotReload |
| `Node_PythonScriptNode` | Python脚本Node | Node + HotReload |
| `Node_HotReloadIntegration` | 热更新集成 | Node + HotReload + VersionManager |
| `Node_RollbackIntegration` | 回滚集成 | Node + HotReload + VersionManager |
| `Node_DisableHotReload` | 禁用热重载 | Node |

### 13. 超时和错误测试 (3个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `Timeout_ExecutionTimeout` | 执行超时 | CodeLoader |
| `Error_InvalidFile` | 无效文件 | CodeLoader |
| `Error_MalformedScript` | 格式错误脚本 | CodeLoader |

### 14. 配置测试 (2个测试)

| 测试名称 | 测试内容 | 涉及功能 |
|---------|---------|----------|
| `Config_DefaultValues` | 默认配置值 | HotReload |
| `ExecutableTypeToString` | 类型字符串转换 | HotReload |

---

## 测试覆盖率

### 可执行文件类型覆盖

| 类型 | 扩展名 | 测试覆盖 |
|-----|---------|----------|
| Shell脚本 | `.sh`, `.bash` | 完全覆盖 |
| Python脚本 | `.py` | 完全覆盖 |
| Node.js脚本 | `.js` | 完全覆盖 |
| Lua脚本 | `.lua` | 完全覆盖 |
| Ruby脚本 | `.rb` | 完全覆盖 |
| JAR包 | `.jar` | 完全覆盖 |
| SO库 | `.so`, `.dylib`, `.dll` | 通过CodeLoader覆盖 |
| 可执行文件 | 无扩展名 | 通过CodeLoader覆盖 |

### 功能模块覆盖

| 模块 | 测试数量 | 覆盖状态 |
|-----|---------|----------|
| 文件监控 | 3 | 完全覆盖 |
| 版本管理 | 5 | 完全覆盖 |
| 热更新重载 | 23 | 完全覆盖 |
| 版本回滚 | 2 | 完全覆盖 |
| 自动重载 | 2 | 完全覆盖 |
| 备份功能 | 1 | 完全覆盖 |
| 超时控制 | 2 | 完全覆盖 |
| 类型检测 | 7 | 完全覆盖 |
| Node集成 | 4 | 完全覆盖 |
| 错误处理 | 3 | 完全覆盖 |
| 配置管理 | 2 | 完全覆盖 |
| 输入数据传递 | 2 | 完全覆盖 |
| 环境变量 | 1 | 完全覆盖 |
| 并发执行 | 1 | 完全覆盖 |

---

## 总计

- **测试用例总数**: 76个
- **可执行文件类型覆盖**: 8种
- **测试文件**: 8个
- **总代码行数**: 约3500行

---

## 调试技巧

### 设置详细日志

```bash
# 设置DEBUG级别日志
export AGENTFLOW_LOG_LEVEL=DEBUG

# 运行测试
./test_hot_reload
```

### 单独运行测试

```bash
# 列出所有测试
./test_hot_reload --gtest_list_tests

# 运行带输出的测试
./test_hot_reload --gtest_print_time=1
```

### 测试覆盖率报告

```bash
# 使用gcov生成覆盖率报告（需要编译时启用）
cmake -DENABLE_COVERAGE=ON ..
make test_hot_reload
./test_hot_reload
gcov *.gcda
```

---

## 已知限制

1. **JAR测试需要Java环境**: 如果系统未安装Java，JAR相关测试会被跳过
2. **脚本运行器需安装**: Python、Node.js、Lua、Ruby需要相应的运行器
3. **文件系统权限**: 测试创建文件需要适当的写入权限
4. **平台差异**: 某些测试在不同平台表现不同（如路径分隔符）

---

## 常见问题

### Q: 测试编译失败
A: 确保已安装所有依赖（CMake、GTest），使用C++17兼容编译器

### Q: 某些测试被跳过
A: 检查相应的运行环境是否安装（Java、Python等），查看日志中的GTEST_SKIP信息

### Q: JAR测试失败
A: 使用 `tests/test_jar/build.sh` 构建有效的JAR包，或者跳过JAR测试

### Q: SO库测试失败
A: 运行 `cd tests/test_lib && cmake . && make` 构建SO库

---

## 持续集成

### GitHub Actions示例

```yaml
name: Hot Reload Tests
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Install dependencies
        run: sudo apt-get update && sudo apt-get install -y cmake g++ libgtest-dev
      - name: Build
        run: |
          mkdir build && cd build
          cmake ..
          make agentflow
          make test_hot_reload
      - name: Test
        run: cd build && ./test_hot_reload
```

---

## 更新历史

- **v1.0** - 初始版本，实现所有可执行文件类型的热更新测试
- **v1.1** - 补充完整的JAR包测试用例（23个测试）

---

## 联系方式

如有问题或建议，请联系：
- Issue Tracker: https://github.com/DanielKernel/AgentFlow/issues
- 邮件: [项目维护者邮箱]
