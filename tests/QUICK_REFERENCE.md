# AgentFlow Tests Quick Reference

测试相关文件的快速参考文档。

---

## 快速索引

| 文件 | 位置 | 描述 |
|-----|------|------|
| 主文档 | `tests/README.md` | 完整的测试文档 |
| 测试构建 | `tests/CMakeLists.txt` | 测试套件构建配置 |
| 测试源码 | `tests/src/*.cpp` | C++测试源文件 |
| SO库 | `tests/lib/hot_reload_test.cpp` | SO库测试源码 |
| Shell脚本 | `tests/scripts/shell/test_script.sh` | Shell测试脚本 |
| Python脚本 | `tests/scripts/python/test_python.py` | Python测试脚本 |
| Node.js脚本 | `tests/scripts/nodejs/test_nodejs.js` | Node.js测试脚本 |
| Lua脚本 | `tests/scripts/lua/test_lua.lua` | Lua测试脚本 |
| Ruby脚本 | `tests/scripts/ruby/test_ruby.rb` | Ruby测试脚本 |
| JAR文件 | `tests/jars/HotReloadTest.java` | JAR测试主类 |
| JAR构建 | `tests/jars/build.sh` | JAR构建脚本 |
| 测试数据 | `tests/data/` | 测试数据和预期输出 |

---

## 常用命令

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
cd build
./bin/test_hot_reload --gtest_filter=HotReloadTest.ShellScript_*
./bin/test_hot_reload --gtest_filter=HotReloadTest.JarFile_*
```

### 构建SO测试库
```bash
cd tests/lib
cmake .
make
```

### 构建JAR测试包
```bash
cd tests/jars
./build.sh
```

### 运行测试脚本
```bash
# Shell脚本
chmod +x tests/scripts/shell/test_script.sh
tests/scripts/shell/test_script.sh

# Python脚本
python3 tests/scripts/python/test_python.py

# Node.js脚本
node tests/scripts/nodejs/test_nodejs.js

# Lua脚本
lua tests/scripts/lua/test_lua.lua

# Ruby脚本
ruby tests/scripts/ruby/test_ruby.rb
```

---

## 测试文件移动

以下文件已从旧位置移动到新的统一结构：

### 源文件 (tests/src/)
- `test_hot_reload.cpp` - 热更新测试（76个测试用例）
- `test_agent_gtest.cpp` - Agent测试
- `test_workflow_gtest.cpp` - Workflow测试
- `test_node_gtest.cpp` - Node测试
- 以及其他测试文件

### 测试库 (tests/lib/)
- `hot_reload_test.cpp` - SO库测试源码
- `CMakeLists.txt` - SO库构建配置

### 测试脚本 (tests/scripts/)
- Shell脚本: `scripts/shell/test_script.sh`
- Python脚本: `scripts/python/test_python.py`
- Node.js脚本: `scripts/nodejs/test_nodejs.js`
- Lua脚本: `scripts/lua/test_lua.lua`
- Ruby脚本: `scripts/ruby/test_ruby.rb`

### JAR测试 (tests/jars/)
- `HotReloadTest.java` - JAR测试主类
- `MANIFEST.MF` - JAR清单
- `build.sh` - JAR构建脚本

### 测试数据 (tests/data/)
- `expected/` - 预期输出目录
- `temporary/` - 临时数据目录

### 文档 (tests/)
- `README.md` - 主测试文档
- `README_HOT_RELOAD_TESTS.md` - 详细测试文档
- `CMakeLists.txt` - 测试构建配置

---

## 测试用例分类

### 热更新测试 (76个)
- 文件监控测试
- 版本管理测试
- SO库动态加载和热更新测试
- Shell脚本热更新测试
- Python脚本热更新测试
- JAR包热更新测试
- Node.js脚本热更新测试
- Lua脚本热更新测试
- Ruby脚本热更新测试
- 版本回滚测试
- 自动重载测试
- 备份功能测试

### 其他测试
- Agent测试
- Workflow测试
- Node测试
- DataBus测试
- Event测试
- EventLoop测试

---

## 维护注意事项

### 添加新测试时
1. 在 `src/` 中添加新的测试源文件
2. 在 `tests/CMakeLists.txt` 中添加构建规则
3. 在 `tests/README.md` 中更新文档
4. 保持与现有测试风格一致

### 添加新脚本测试时
1. 在 `tests/scripts/<type>/` 中添加脚本文件
2. 在对应的子目录CMakeLists.txt中添加构建规则
3. 更新文档

### 添加新库测试时
1. 在 `tests/lib/` 中添加库源文件
2. 在 `lib/CMakeLists.txt` 中添加构建规则
3. 更新文档

---

## 问题排查

### 编译失败
```bash
# 清理构建目录
rm -rf build
mkdir build

# 重新配置和构建
cd build
cmake ..
make -j4
```

### 测试跳过
某些测试可能会因缺少依赖而跳过，这是正常的。查看日志中的GTEST_SKIP信息。

### 链接错误
确保已编译agentflow库：
```bash
cd build
make agentflow
```

---

## 更新日志

### v1.1 (2024-02-17)
- 创建统一的测试目录结构
- 整合所有测试相关文件
- 添加JAR包完整测试用例
- 创建详细的测试文档
