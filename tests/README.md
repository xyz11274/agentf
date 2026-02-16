# AgentFlow Test Suite

本文档描述了AgentFlow项目的测试套件结构、组织方式和运行说明。

---

## 目录结构

```
tests/
├── README.md                          # 本文档
├── CMakeLists.txt                      # 测试套件构建配置
├── src/                                 # C++测试源文件
│   ├── test_hot_reload.cpp          # 热更新功能测试
│   ├── test_agent_gtest.cpp       # Agent测试
│   ├── test_workflow_gtest.cpp    # Workflow测试
│   ├── test_node_gtest.cpp        # Node测试
│   ├── test_databus_gtest.cpp     # DataBus测试
│   ├── test_event_gtest.cpp       # Event测试
│   └── test_eventloop_gtest.cpp  # EventLoop测试
├── lib/                                 # 测试库源文件
│   ├── hot_reload_test.cpp       # SO库测试源代码
│   └── CMakeLists.txt             # SO库构建配置
├── scripts/                             # 测试脚本文件
│   ├── shell/
│   │   └── test_script.sh      # Shell测试脚本
│   ├── python/
│   │   └── test_python.py       # Python测试脚本
│   ├── nodejs/
│   │   └── test_nodejs.js        # Node.js测试脚本
│   ├── lua/
│   │   └── test_lua.lua           # Lua测试脚本
│   └── ruby/
│       └── test_ruby.rb           # Ruby测试脚本
├── jars/                                # JAR测试文件
│   ├── HotReloadTest.java       # JAR测试主类
│   ├── MANIFEST.MF               # JAR清单文件
│   └── build.sh                   # JAR构建脚本
└── data/                                # 测试数据和预期输出
    ├── expected/                  # 预期输出文件
    └── temporary/                 # 临时测试数据
```

---

## 测试套件

### 1. 热更新测试 (Hot Reload Tests)

**测试文件**: `src/test_hot_reload.cpp`

**测试内容**:
- 文件监控功能测试
- 版本管理功能测试
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

**测试用例数**: 76个

**运行**:
```bash
./bin/test_hot_reload
```

---

### 2. Agent测试 (Agent Tests)

**测试文件**: `src/test_agent_gtest.cpp`

**测试内容**:
- Agent创建和销毁
- Agent生命周期管理
- Agent状态转换
- Agent配置管理

**运行**:
```bash
./bin/test_agent_gtest
```

---

### 3. Workflow测试 (Workflow Tests)

**测试文件**: `src/test_workflow_gtest.cpp`

**测试内容**:
- Workflow创建和加载
- Workflow DAG管理
- Workflow执行
- Workflow状态管理
- Workflow DSL解析

**运行**:
```bash
./bin/test_workflow_gtest
```

---

### 4. Node测试 (Node Tests)

**测试文件**: `src/test_node_gtest.cpp`

**测试内容**:
- Node创建和配置
- Node状态管理
- Node执行（各种模式）
- Node超时控制
- Node重试机制

**运行**:
```bash
./bin/test_node_gtest
```

---

### 5. 其他测试 (Other Tests)

- `test_databus_gtest.cpp` - DataBus数据总线测试
- `test_event_gtest.cpp` - Event事件系统测试
- `test_eventloop_gtest.cpp` - EventLoop事件循环测试

---

## 可执行代码类型支持

AgentFlow热更新功能支持以下可执行代码类型：

| 类型 | 扩展名 | 运行器 | 测试覆盖 |
|-----|---------|--------|----------|
| Shell脚本 | .sh, .bash | bash | 完整 |
| Python脚本 | .py | python3 | 完整 |
| Node.js脚本 | .js | node | 完整 |
| Lua脚本 | .lua | lua | 完整 |
| Ruby脚本 | .rb | ruby | 完整 |
| JAR包 | .jar | java | 完整 |
| SO库 | .so, .dylib, .dll | dlopen() | 完整 |
| 可执行文件 | * | 直接执行 | 完整 |

---

## 构建和运行

### 1. 构建所有测试

```bash
cd build
cmake ..
make
```

### 2. 构建特定测试套件

```bash
cd build
cmake ..
make test_hot_reload      # 只构建热更新测试
make test_agent_gtest    # 只构建Agent测试
```

### 3. 运行所有测试

```bash
cd build
./bin/test_hot_reload
./bin/test_agent_gtest
./bin/test_workflow_gtest
# ... 其他测试
```

### 4. 运行特定测试

```bash
cd build
./bin/test_hot_reload --gtest_filter=HotReloadTest.ShellScript_*
./bin/test_hot_reload --gtest_filter=HotReloadTest.JarFile_*
```

### 5. 重复测试

```bash
cd build
./bin/test_hot_reload --gtest_repeat=10 --gtest_filter=HotReloadTest.ShellScript_HotReload
```

---

## 构建测试库

### SO库

```bash
cd tests/lib
cmake .
make
```

### JAR包

```bash
cd tests/jars
./build.sh
```

---

## 测试脚本

### Shell脚本

```bash
cd tests/scripts/shell
chmod +x *.sh
./test_script.sh
```

### Python脚本

```bash
cd tests/scripts/python
python3 test_python.py
```

### Node.js脚本

```bash
cd tests/scripts/nodejs
node test_nodejs.js
```

### Lua脚本

```bash
cd tests/scripts/lua
lua test_lua.lua
```

### Ruby脚本

```bash
cd tests/scripts/ruby
ruby test_ruby.rb
```

---

## 测试数据

测试数据和预期输出文件位于 `tests/data/` 目录：

- `expected/` - 预期的测试输出文件
- `temporary/` - 临时测试数据文件

---

## CI/CD集成

### GitHub Actions示例

```yaml
name: Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        test:
          - hot_reload
          - agent
          - workflow
          - node
    steps:
      - name: Checkout
        uses: actions/checkout@v2

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake g++ libgtest-dev

      - name: Build
        run: |
          mkdir build && cd build
          cmake ..
          make test_${{ matrix.test }}

      - name: Run tests
        run: |
          cd build
          ./bin/test_${{ matrix.test }}
```

---

## 测试覆盖

### 覆盖的测试用例类别

1. **功能测试**: 验证各种功能的正确性
2. **热更新测试**: 验证代码热更新机制
3. **版本管理测试**: 验证版本创建、切换、回滚
4. **并发测试**: 验证多线程场景
5. **错误处理测试**: 验证各种错误情况的处理
6. **性能测试**: 验证执行性能和资源使用
7. **集成测试**: 验证各组件间的集成

---

## 添加新测试

### 添加新的C++测试

1. 在 `src/` 下创建新的测试文件
2. 在 `tests/CMakeLists.txt` 中添加构建规则
3. 运行 `cmake .. && make` 构建新测试
4. 更新本README.md文档

### 添加新的脚本测试

1. 在 `tests/scripts/<type>/` 下创建新脚本
2. 在 `scripts/README.md` 中添加说明
3. 测试脚本功能
4. 更新本README.md文档

### 添加新的库测试

1. 在 `tests/lib/` 下创建新的库源文件
2. 在 `lib/CMakeLists.txt` 中添加构建规则
3. 运行构建脚本生成测试库
4. 更新本README.md文档

---

## 调试和日志

### 设置调试日志

```bash
# 设置DEBUG级别
export AGENTFLOW_LOG_LEVEL=DEBUG

# 运行测试
./bin/test_hot_reload
```

### 查看测试日志

测试日志输出包含：
- `[TEST]` - 测试框架日志
- `[HOT_RELOAD]` - 热更新模块日志
- `[FILE_WATCHER]` - 文件监控日志
- `[CODE_LOADER]` - 代码加载器日志
- `[NODE_*]` - Node执行日志

---

## 已知问题和限制

### 已知问题

1. **JAR测试依赖**: JAR测试需要Java环境，没有Java时会跳过相关测试
2. **脚本运行器**: Python、Node.js、Lua、Ruby需要相应的运行器

### 限制

1. **平台差异**: 某些测试在不同平台表现不同
2. **文件系统**: 文件监控在不同文件系统上可能行为不同
3. **权限要求**: 测试运行可能需要特定的文件权限

---

## 维护指南

### 代码规范

- 遵循项目现有的代码风格
- 使用统一的命名约定
- 添加适当的注释和文档

### 测试规范

- 测试名称清晰描述测试内容
- 使用GTEST的宏和断言
- 每个测试独立，不依赖其他测试
- 适当的SetUp/TearDown

### 文档规范

- 新增测试时更新相关文档
- 保持文档与代码同步
- 提供清晰的使用示例

---

## 参考资源

- [Google Test文档](https://google.github.io/googletest/)
- [AgentFlow主README](../README.md)
- [AgentFlow开发指南](../docs/DEVELOPMENT.md)

---

## 更新历史

### v1.0 (2024-02-17)
- 初始版本
- 整合所有测试到统一目录结构
- 实现完整的热更新测试套件（76个测试用例）
- 支持多种可执行代码类型
- 提供完整的测试文档
