# AgentFlow Test Suite Refactoring Summary

本文档描述了AgentFlow测试套件的重构工作，将散列在不同地方的测试代码和文档归集到统一的位置。

---

## 重构目标

1. **统一管理**: 所有测试相关代码、脚本、文档集中到一个位置
2. **结构化组织**: 按照功能类型和文件类型进行清晰分类
3. **便于维护**: 减少跨目录查找，提高代码可维护性
4. **独立构建**: 测试套件可以独立构建和运行
5. **完整文档**: 提供清晰的使用和维护文档

---

## 新的目录结构

```
tests/
├── README.md                          # 主测试文档（入口点）
├── QUICK_REFERENCE.md                # 快速参考文档
├── CMakeLists.txt                      # 测试套件构建配置
├── src/                                 # C++测试源文件（18个文件）
├── lib/                                 # 测试库源文件（SO库等）
│   ├── hot_reload_test.cpp
│   └── CMakeLists.txt
├── scripts/                             # 测试脚本（按类型分类）
│   ├── shell/
│   │   ├── test_script.sh
│   │   └── CMakeLists.txt
│   ├── python/
│   │   ├── test_python.py
│   │   └── CMakeLists.txt
│   ├── nodejs/
│   │   ├── test_nodejs.js
│   │   └── CMakeLists.txt
│   ├── lua/
│   │   ├── test_lua.lua
│   │   └── CMakeLists.txt
│   └── ruby/
│       ├── test_ruby.rb
│       └── CMakeLists.txt
├── jars/                                # JAR测试文件
│   ├── HotReloadTest.java
│   ├── MANIFEST.MF
│   └── build.sh
└── data/                                # 测试数据
    ├── README.md
    ├── expected/
    └── temporary/
```

---

## 文件迁移

### 移动的文件

#### C++测试源文件 (tests/src/)
从 `tests/` 根目录移动到 `src/`：
- `test_hot_reload.cpp` - 热更新测试（76个测试用例）
- `test_adapter_isolation.cpp` - 适配器隔离测试
- `test_agent_gtest.cpp` - Agent测试
- `test_agent_json_gtest.cpp` - Agent JSON测试
- `test_agent.cpp` - Agent源码测试
- `test_workflow_gtest.cpp` - Workflow测试
- `test_workflow_json_gtest.cpp` - Workflow JSON测试
- `test_node_gtest.cpp` - Node测试
- `test_node.cpp` - Node源码测试
- `test_databus_gtest.cpp` - DataBus测试
- `test_databus.cpp` - DataBus源码测试
- `test_event_gtest.cpp` - Event测试
- `test_eventloop_gtest.cpp` - EventLoop测试
- `test_local_channel_gtest.cpp` - 本地通道测试
- `test_request_response_gtest.cpp` - 请求响应测试
- `test_main.cpp` - 主函数测试
- `test_workflow.cpp` - Workflow源码测试

#### 测试库 (tests/lib/)
从 `tests/test_lib/` 移动到 `lib/`：
- `hot_reload_test.cpp` - SO库测试源码
- `CMakeLists.txt` - SO库构建配置

#### JAR测试文件 (tests/jars/)
从 `tests/test_jar/` 移动到 `jars/`：
- `HotReloadTest.java` - JAR主类
- `MANIFEST.MF` - JAR清单
- `build.sh` - JAR构建脚本

#### 测试脚本 (tests/scripts/)
按类型重组到子目录：
- `shell/test_script.sh` → `scripts/shell/test_script.sh`
- `python/test_python.py` → `scripts/python/test_python.py`
- `nodejs/test_nodejs.js` → `scripts/nodejs/test_nodejs.js`
- `lua/test_lua.lua` → `scripts/lua/test_lua.lua`
- `ruby/test_ruby.rb` → `scripts/ruby/test_ruby.rb`

#### 测试数据 (tests/data/)
创建新的数据目录结构：
- `expected/` - 预期输出目录
- `temporary/` - 临时数据目录

---

## 测试套件统计

### 测试文件分类统计

| 类别 | 文件数量 | 测试用例数 |
|------|---------|-----------|
| 热更新测试 | 1 | 76 |
| Agent测试 | 3 | 若干 |
| Workflow测试 | 3 | 若干 |
| Node测试 | 2 | 若干 |
| 其他测试 | 3 | 若干 |
| **总计** | **18** | **76+** |

### 可执行代码支持

| 类型 | 扩展名 | 测试用例 |
|-----|---------|----------|
| Shell脚本 | .sh, .bash | 6 |
| Python脚本 | .py | 4 |
| Node.js脚本 | .js | 2 |
| Lua脚本 | .lua | 2 |
| Ruby脚本 | .rb | 2 |
| JAR包 | .jar | 23 |
| SO库 | .so, .dylib, .dll | 完整覆盖 |
| 可执行文件 | * | 完整覆盖 |

---

## 新建文件清单

### 主文档
1. **`tests/README.md`** - 主测试文档，包含：
   - 目录结构说明
   - 测试套件介绍
   - 构建和运行指南
   - 维护指南
   - 参考资源

2. **`tests/QUICK_REFERENCE.md`** - 快速参考文档，包含：
   - 文件位置快速索引
   - 常用命令速查
   - 测试文件移动记录

3. **`tests/CMakeLists.txt`** - 测试构建配置，包含：
   - 所有测试可执行文件构建规则
   - 测试库构建规则
   - 脚本文件安装规则
   - JAR文件安装规则

### 测试数据文档
4. **`tests/data/README.md`** - 测试数据文档，包含：
   - 预期输出文件使用说明
   - 临时文件使用规则
   - 添加新数据的指南

### 各类别的CMakeLists.txt
5. **`tests/lib/CMakeLists.txt`** - SO库构建
6. **`tests/scripts/shell/CMakeLists.txt`** - Shell脚本
7. **`tests/scripts/python/CMakeLists.txt`** - Python脚本
8. **`tests/scripts/nodejs/CMakeLists.txt`** - Node.js脚本
9. **`tests/scripts/lua/CMakeLists.txt`** - Lua脚本
10. **`tests/scripts/ruby/CMakeLists.txt`** - Ruby脚本

---

## 热更新测试覆盖

热更新测试（76个测试用例）覆盖以下方面：

### 文件监控 (5个测试)
- 启动/停止文件监控
- 不存在的文件处理
- 多文件监控
- 文件变化检测

### 版本管理 (5个测试)
- 创建多版本
- 最大版本限制
- 版本回滚
- 回滚到无效版本

### Shell脚本测试 (6个测试)
- 基本执行
- 热更新
- 带参数执行
- 错误处理
- 工作目录
- 超时控制

### Python脚本测试 (4个测试)
- 基本执行
- 热更新
- 带参数执行
- 输入数据传递
- 错误处理

### Node.js脚本测试 (2个测试)
- 基本执行
- 热更新

### Lua脚本测试 (2个测试)
- 基本执行
- 热更新

### Ruby脚本测试 (2个测试)
- 基本执行
- 热更新

### JAR包测试 (23个测试)
- 类型检测
- 基本执行
- 主类执行
- 带参数执行
- 热更新
- 工作目录
- 输入数据传递
- 超时控制
- 错误处理
- 版本跟踪
- 版本回滚
- 最大版本限制
- 自动重载
- Node集成
- 环境变量传递
- 并发执行
- 备份功能
- 重载前验证
- 校验和验证
- 多文件监控
- 无效扩展名
- 空JAR文件

### 版本管理测试 (5个测试)
- 创建多版本
- 最大版本限制
- 版本回滚

### 自动重载测试 (2个测试)
- 自动重载启用
- 自动重载禁用

### 备份测试 (1个测试)
- 重载前备份

### 可执行类型检测测试 (7个测试)
- Shell脚本类型检测
- Python脚本类型检测
- Node.js脚本类型检测
- Lua脚本类型检测
- Ruby脚本类型检测
- JAR文件类型检测
- 未知类型检测

### Node集成测试 (4个测试)
- Shell脚本Node
- Python脚本Node
- 热更新集成
- 版本回滚集成
- 禁用热重载

### 超时和错误测试 (3个测试)
- 执行超时
- 无效文件
- 格式错误脚本

### 配置测试 (2个测试)
- 默认配置值
- 类型字符串转换

---

## 构建和运行

### 构建所有测试
```bash
cd build
cmake ..
make
```

### 运行热更新测试
```bash
cd build
./bin/test_hot_reload
```

### 运行特定测试组
```bash
# 只运行Shell脚本热更新测试
./bin/test_hot_reload --gtest_filter=HotReloadTest.ShellScript_*

# 只运行JAR包测试
./bin/test_hot_reload --gtest_filter=HotReloadTest.JarFile_*
```

### 构建测试库
```bash
cd tests/lib
cmake .
make
```

### 构建JAR包
```bash
cd tests/jars
./build.sh
```

---

## 使用示例

### 运行所有热更新测试
```bash
# 设置DEBUG日志
export AGENTFLOW_LOG_LEVEL=DEBUG

# 运行测试
cd build
./bin/test_hot_reload

# 查看日志
cat logs/agentflow.log
```

### 测试特定功能
```bash
# 测试文件监控
./bin/test_hot_reload --gtest_filter=HotReloadTest.FileWatcher*

# 测试版本回滚
./bin/test_hot_reload --gtest_filter=HotReloadTest.VersionManager_Rollback

# 测试JAR执行
cd tests/jars
./build.sh
java -jar HotReloadTest.jar version
```

---

## 维护指南

### 添加新的测试

1. **C++测试**: 在 `tests/src/` 中创建新的测试源文件
2. **测试脚本**: 在 `tests/scripts/<type>/` 中创建脚本
3. **JAR测试**: 在 `tests/jars/` 中添加测试
4. **更新文档**: 更新相应的README和CMakeLists.txt
5. **保持一致**: 遵循现有代码风格和测试规范

### 修改现有测试

1. 测试文件位置: `tests/src/`
2. 测试脚本位置: `tests/scripts/<type>/`
3. JAR测试位置: `tests/jars/`
4. 更新相关文档

---

## 与主项目集成

### CMakeLists.txt集成
主CMakeLists.txt中的测试部分已更新，添加：
```cmake
# Add tests subdirectory
add_subdirectory(tests)
```

### 测试输出目录
测试可执行文件安装到 `build/bin/`：
- `test_hot_reload` - 热更新测试
- `test_agent_gtest` - Agent测试
- `test_workflow_gtest` - Workflow测试
- 等其他测试

### 测试脚本安装
测试脚本安装到 `build/scripts/<type>/`：
- `scripts/shell/test_script.sh`
- `scripts/python/test_python.py`
- 等其他脚本

---

## 变更日志

### v1.0 (2024-02-17)
- 创建统一的测试目录结构
- 整合所有测试相关代码和文档
- 添加76个热更新测试用例
- 支持SO、Shell、Python、JAR、Node.js、Lua、Ruby等多种可执行代码
- 创建完整的测试文档和使用指南

---

## 相关文档

- [AgentFlow README](../README.md) - 项目主文档
- [Hot Reload Tests Documentation](README_HOT_RELOAD_TESTS.md) - 详细测试文档
- [Test Suite README](README.md) - 测试套件文档
- [Quick Reference](QUICK_REFERENCE.md) - 快速参考文档
