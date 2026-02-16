# AgentFlow 开发指南

## 快速开始

### 前置要求
- CMake 3.10 or higher
- C++11 compatible compiler (GCC, Clang, etc.)
- Linux/Unix environment
- git

### 环境设置

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

## 代码风格和规范

### 强制要求

所有代码必须符合 [CODING_STANDARDS.md](CODING_STANDARDS.md) 中的规范。

主要要点：
- **语言**: C++11 标准（不支持 C++14+）
- **命名**: PascalCase（类），snake_case（函数/变量）
- **缩进**: 2 个空格
- **行长**: 100 字符（硬限制 120）
- **内存**: 使用 `std::shared_ptr`/`std::unique_ptr`，禁止原始指针
- **线程安全**: 所有共享资源必须用 `std::mutex` 保护

### 格式化脚本

项目包含自动化工具配置文件：

#### .clang-format
自动代码格式化配置

```bash
# 格式化单个文件
clang-format -i include/agentflow/framework/agent.h

# 格式化整个目录
clang-format -r -i include/ src/ tests/ examples/

# 查看格式化效果（不修改文件）
clang-format include/agentflow/framework/agent.h
```

#### .clang-tidy
代码质量和静态分析

```bash
# 如需安装 clang-tidy（Ubuntu/Debian）
sudo apt-get install clang-tools

# 检查单个文件
clang-tidy -p build include/agentflow/framework/agent.h

# 检查整个目录
clang-tidy -p build include/agentflow/**/*.h src/*.cpp
```

#### .editorconfig
编辑器配置，支持 VS Code、JetBrains IDE 等

配置会自动应用（需要安装 EditorConfig 插件）

### VS Code 推荐扩展

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

在 `.vscode/extensions.json` 中配置，使用:

```bash
code --install-extension llvm-vs-code-extensions.vscode-clangd
code --install-extension notskm.clang-tidy
code --install-extension xaver.clang-format
code --install-extension EditorConfig.EditorConfig
```

## 开发工作流

### 1. 创建特性分支

```bash
git checkout -b feature/my-feature
# 或
git checkout -b fix/my-bugfix
```

### 2. 编写代码

遵循 [CODING_STANDARDS.md](CODING_STANDARDS.md) 规范

### 3. 格式化代码

```bash
# 自动格式化修改的文件
clang-format -r -i include/ src/ tests/ examples/
```

### 4. 编译和测试

```bash
# 完整构建
./build.sh build

# 运行全部测试
./build.sh test

# 运行示例
./build.sh example
```

### 5. 检查代码质量（可选）

```bash
# 需要安装 clang-tidy
cd build
clang-tidy -p . ../include/**/*.h ../src/*.cpp
```

### 6. 提交代码

```bash
# 检查状态
git status

# 添加文件
git add include/ src/ tests/

# 提交（遵循提交信息规范）
git commit -m "feat: add new feature description

Detailed explanation of what was changed and why.

- Point 1
- Point 2"

# 推送
git push origin feature/my-feature
```

### 7. 创建 Pull Request

在 GitHub 上创建 PR，描述变更内容和测试结果。

## 项目结构

```
AgentFlow/
├── include/agentflow/
│   ├── common.h              # 通用定义和枚举
│   ├── basic/
│   │   └── logger.h          # 日志系统
│   ├── framework/
│   │   ├── agent.h           # Agent 类
│   │   ├── workflow.h        # Workflow DAG
│   │   ├── node.h            # 节点定义
│   │   └── edge.h            # 边和依赖
│   └── runtime/
│       ├── databus.h         # 高速数据总线
│       └── agent_runtime.h   # 运行时引擎
├── src/
│   ├── agent.cpp
│   ├── workflow.cpp
│   ├── node.cpp
│   ├── edge.cpp
│   ├── databus.cpp
│   ├── agent_runtime.cpp
│   └── logger.cpp
├── tests/
│   ├── test_main.cpp
│   ├── test_node.cpp
│   ├── test_workflow.cpp
│   ├── test_agent.cpp
│   └── test_databus.cpp
├── examples/
│   └── basic_example.cpp     # 基础使用示例
├── CMakeLists.txt            # 构建配置
├── build.sh                  # 构建脚本
├── CODING_STANDARDS.md       # 本文件
├── DEVELOPMENT.md            # 本文档
├── IMPLEMENTATION.md         # 实现细节
├── README.md                 # 项目概述
├── .gitignore                # Git 忽略规则
├── .clang-format             # 代码格式化配置
├── .clang-tidy               # 代码质量检查配置
├── .editorconfig             # 编辑器配置
└── .cmake-format             # CMake 格式化配置
```

## 测试规范

### 编写测试

遵循以下模式：

```cpp
bool test_feature_description() {
  // Arrange: 准备测试数据
  Node node("node_id", "node_name");

  // Act: 执行操作
  node.initialize(Status::OK);

  // Assert: 验证结果
  if (node.get_state() != NodeState::WAITING) {
    Logger::get_instance().log(LogLevel::ERROR, "Expected WAITING state");
    return false;
  }

  return true;
}
```

### 运行测试

```bash
# 构建项目（包括测试）
./build.sh build

# 运行所有测试
./build.sh test

# 具体测试输出
cd build && ./test_agentflow
```

### 测试覆盖

- 单元测试: 每个组件的功能测试
- 集成测试: 组件之间的交互
  
新功能必须包含相应的测试。

## 提交规范

### 提交信息格式

```
<type>: <subject>

<body>

<footer>
```

**类型（Type）**:
- `feat`: 新功能
- `fix`: 缺陷修复
- `test`: 测试相关
- `refactor`: 代码重构
- `style`: 代码风格调整
- `doc`: 文档更新
- `chore`: 构建或其他杂项

**主题（Subject）**:
- 使用命令式现在时（"add" 而非 "added"）
- 不大写字母开头
- 末尾不加句号

**正文（Body）**:
- 可选，说明更改的原因和内容
- 每行不超过 80 字符

**页脚（Footer）**:
- 可选，引用 issue（如 "Closes #123"）

### 示例

```
feat: implement workflow DAG scheduling

Add complete directed acyclic graph support for workflow
scheduling with topological ordering and validation.

- Implement Workflow class with add_node/add_edge methods
- Add topological sort algorithm
- Add validation for cycle detection

Implements #42
```

## 常见问题

### Q: 如何更新依赖项？
A: AgentFlow 仅使用 C++ 标准库，无外部依赖。

### Q: 如何添加新的编译选项？
A: 修改 [CMakeLists.txt](CMakeLists.txt)，然后 `./build.sh build`

### Q: 代码格式化失败怎么办？
A: 确保安装了 clang-format：
```bash
sudo apt-get install clang-format
```

### Q: 如何运行单个测试？
A: 需要修改 `tests/test_main.cpp` 的测试选择逻辑，或直接链接测试文件

### Q: 支持什么操作系统？
A: 目前仅支持 Linux/Unix。Windows 可能需要调整（Path、线程库等）

## 有用的命令

```bash
# 构建所有目标
./build.sh build

# 只构建库
cd build && cmake .. && make agentflow

# 只构建测试
cd build && make test_agentflow

# 只构建示例
cd build && make example_basic

# 清理构建目录
./build.sh clean

# 完整清理和构建
rm -rf build && ./build.sh build && ./build.sh test

# 代码统计
find . -name "*.cpp" -o -name "*.h" | xargs wc -l

# 查看 git log
git log --oneline --graph --all
```

## 性能优化指南

1. **避免不必要的拷贝**
   ```cpp
   // ❌ 避免
   std::string get_name() { return name_; }  // 不必要的拷贝
   
   // ✅ 推荐
   const std::string& get_name() const { return name_; }  // 返回引用
   ```

2. **使用移动语义**
   ```cpp
   // ✅ C++11 支持
   std::unique_ptr<Node> create_node() {
     return std::make_unique<Node>();  // 移动返回
   }
   ```

3. **对象复用**
   ```cpp
   // 重用承载线程，避免频繁创建销毁
   std::thread worker = std::thread(&AgentRuntime::worker_loop, this);
   ```

## 安全检查清单

提交前检查：

- [ ] 代码编译无错误
- [ ] 代码编译无警告
- [ ] 所有测试通过
- [ ] 代码格式化正确
- [ ] 命名规范统一
- [ ] 没有原始指针
- [ ] 共享资源已同步
- [ ] 没有泄漏内存
- [ ] const 正确性已应用
- [ ] 新增功能有对应测试
- [ ] 提交信息清晰明确

---

**维护**: AgentFlow Team  
**最后更新**: 2026-02-16
