# AgentFlow 编程规范

## 1. 代码风格

### 1.1 命名规范

#### 类型命名
- **类/结构体**：使用 PascalCase
  ```cpp
  class AgentRuntime { };
  struct NodeConfig { };
  ```

- **枚举**：使用 PascalCase，枚举值使用 UPPER_CASE
  ```cpp
  enum class NodeState {
    WAITING,
    READY,
    EXECUTING,
    SUCCESS,
    FAILED
  };
  ```

- **全局常量/宏**：使用 UPPER_CASE_WITH_UNDERSCORES
  ```cpp
  #define AGENTFLOW_VERSION "1.0.0"
  constexpr int DEFAULT_WORKER_COUNT = 4;
  ```

#### 变量和函数命名
- **局部变量/成员变量**：使用 snake_case
  ```cpp
  int task_id = 0;
  std::mutex workflow_mutex_;
  ```

- **函数/方法**：使用 snake_case
  ```cpp
  void execute_node();
  std::string get_node_id() const;
  bool is_workflow_valid();
  ```

- **私有成员变量**：以 `_` 后缀结尾
  ```cpp
  class Agent {
  private:
    std::map<WorkflowId, std::shared_ptr<Workflow> > workflows_;
    std::mutex agent_mutex_;
  };
  ```

### 1.2 缩进和空格

- **缩进**：使用 2 个空格（不使用 TAB）
- **每行最大长度**：80 字符（建议）、120 字符（硬汇编）
- **括号风格**：K&R 风格（Brace 开括号在同一行）

```cpp
if (condition) {
  // code
} else {
  // code
}

for (int i = 0; i < 10; ++i) {
  // code
}

class MyClass {
public:
  void method();
};
```

### 1.3 空行

- 类定义中，访问修饰符（public/private/protected）前后各一空行
- 函数定义之间一空行
- 逻辑段落之间一空行

```cpp
class Example {
public:
  Example();
  ~Example();

  void public_method();

private:
  int private_member_;

  void private_method();
};
```

### 1.4 注释

- **文件头注释**：包含文件描述（C++11 标准）
  ```cpp
  // Copyright (c) 2026 AgentFlow Authors
  // Licensed under MIT License
  //
  // This file defines the core Agent class for workflow orchestration.
  ```

- **类/函数注释**：使用 Doxygen 风格
  ```cpp
  /// @brief Creates and returns a new workflow instance
  /// @param workflow_name The unique identifier for the workflow
  /// @return Shared pointer to the created workflow
  /// @throws std::runtime_error if workflow already exists
  std::shared_ptr<Workflow> create_workflow(const std::string& workflow_name);
  ```

- **行内注释**：解释复杂逻辑
  ```cpp
  // Iterate through all nodes and mark as ready if dependencies are satisfied
  for (const auto& node : nodes_) {
    // ...
  }
  ```

### 1.5 代码格式化

使用提供的 `.clang-format` 配置进行代码格式化：

```bash
clang-format -i <file>
clang-format -r -i include/ src/ tests/ examples/
```

## 2. C++11 标准要求

### 2.1 现代 C++ 特性（允许）

- ✅ `std::shared_ptr` / `std::unique_ptr`
- ✅ `std::thread` 和线程原语
- ✅ `std::function` 和 lambda 表达式
- ✅ `std::mutex`, `std::lock_guard`
- ✅ `auto` 关键字用于类型推导
- ✅ 初始化列表
- ✅ 基于范围的 for 循环

### 2.2 禁止使用

- ❌ C++14/17/20 特性（如 `std::Optional`, `std::variant`）
- ❌ 外部第三方库（仅使用标准库）
- ❌ 原始指针管理内存
- ❌ C 风格数组（使用 `std::vector` 代替）
- ❌ `goto` 语句

### 2.3 内存管理

```cpp
// ✅ 推荐
std::shared_ptr<Workflow> wf = std::make_shared<Workflow>("wf1");
std::unique_ptr<Logger> logger(new Logger());

// ❌ 避免
Workflow* wf = new Workflow();  // 手动管理
delete wf;

// ✅ 容器管理
std::vector<int> data;
data.push_back(1);
```

### 2.4 线程安全

所有共享资源必须使用 `std::mutex` 保护：

```cpp
class Agent {
private:
  std::map<WorkflowId, std::shared_ptr<Workflow> > workflows_;
  mutable std::mutex workflows_mutex_;

public:
  std::shared_ptr<Workflow> get_workflow(const WorkflowId& id) const {
    std::lock_guard<std::mutex> lock(workflows_mutex_);
    auto it = workflows_.find(id);
    return it != workflows_.end() ? it->second : nullptr;
  }
};
```

## 3. 头文件和组织

### 3.1 包含顺序

```cpp
#ifndef AGENTFLOW_AGENT_H
#define AGENTFLOW_AGENT_H

// 1. 标准库
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// 2. 项目内部头文件
#include "agentflow/common.h"
#include "agentflow/framework/workflow.h"

namespace agentflow {

// 类定义...

}  // namespace agentflow

#endif  // AGENTFLOW_AGENT_H
```

### 3.2 头文件守卫

使用 `#ifndef` / `#define` / `#endif` 模式：

```cpp
#ifndef AGENTFLOW_FRAMEWORK_AGENT_H
#define AGENTFLOW_FRAMEWORK_AGENT_H

// 内容

#endif  // AGENTFLOW_FRAMEWORK_AGENT_H
```

## 4. 函数和类设计

### 4.1 函数参数

```cpp
// ✅ 推荐
void process(const std::string& name);              // 字符串用 const& 传递
void update(int value);                              // 简单类型用值传递
std::shared_ptr<Workflow> create_workflow();        // 返回 shared_ptr

// ❌ 避免
void process(std::string name);                      // 不必要的拷贝
void update(int& value);                             // 简单类型不需要引用
Workflow* create_workflow();                         // 使用原始指针
```

### 4.2 const 正确性

```cpp
class DataBus {
public:
  // const 成员函数
  bool contains(const std::string& key) const;
  std::string get(const std::string& key) const;

  // 非 const 成员函数
  void put(const std::string& key, const std::string& value);
  void remove(const std::string& key);

private:
  mutable std::map<std::string, std::string> data_;
  mutable std::mutex data_mutex_;
};
```

### 4.3 异常处理

```cpp
// ✅ 推荐：使用异常处理错误
Status initialize() {
  try {
    // 初始化代码
    validate_config();
  } catch (const std::runtime_error& e) {
    Logger::get_instance().log(LogLevel::ERROR, e.what());
    return Status::ERROR;
  }
  return Status::OK;
}

// ❌ 避免在其他情况下使用异常
if (condition) {
  throw std::logic_error("This should not happen");  // 不应该用于控制流
}
```

## 5. 测试规范

### 5.1 测试文件命名

```
tests/test_<component>.cpp
```

### 5.2 测试结构

```cpp
bool test_node_creation() {
  // Arrange
  NodeId node_id = "node_1";
  std::string node_name = "Process Node";

  // Act
  Node node(node_id, node_name);

  // Assert
  return node.get_id() == node_id && 
         node.get_name() == node_name &&
         node.get_state() == NodeState::WAITING;
}
```

### 5.3 断言和验证

- 使用简单的 `assert` 或返回 `true/false`
- 日志记录测试进度
- 一个函数一个功能点

## 6. 文档要求

### 6.1 README.md

- 项目概述
- 架构设计
- 快速开始
- 构建步骤
- 测试运行

### 6.2 IMPLEMENTATION.md

- 各组件详细说明
- 核心数据结构
- 使用示例
- 扩展指导

### 6.3 代码注释

- 复杂算法必须有详细注释
- 公开 API 使用 Doxygen 风格
- 不要过度注释显而易见的代码

## 7. 代码审查检查清单

提交代码前请检查：

- [ ] 命名规范一致（类/函数/变量）
- [ ] 没有使用 C++14+ 特性
- [ ] 没有原始指针
- [ ] 所有共享资源都有互斥量保护
- [ ] 头文件包含顺序正确
- [ ] const 正确性已应用
- [ ] 代码可以用 C++11 编译
- [ ] 构建/测试通过（`./build.sh build && ./build.sh test`）
- [ ] 代码格式化已应用（`clang-format -r -i`）
- [ ] 没有编译警告
- [ ] 新功能有对应的测试

## 8. 构建和检查

```bash
# 构建项目
./build.sh build

# 运行测试
./build.sh test

# 运行示例
./build.sh example

# 代码格式化（需安装 clang-format）
clang-format -r -i include/ src/ tests/ examples/

# 检查代码风格（需安装 clang-tidy）
clang-tidy -p build include/**/*.h src/*.cpp
```

## 9. 版本控制

### 9.1 提交信息规范

```
<type>: <subject>

<body>

<footer>
```

类型：
- `feat`: 新功能
- `fix`: 缺陷修复
- `refactor`: 代码重构
- `test`: 测试相关
- `doc`: 文档更新
- `style`: 代码风格调整
- `chore`: 构建或工具链更新

示例：
```
feat: implement workflow state machine

Add complete state machine for workflow execution with
transitions: IDLE -> RUNNING -> COMPLETED/FAILED

- Add WorkflowState enum
- Implement state validation
- Add unit tests

Closes #123
```

### 9.2 .gitignore 规则

所有编译产物、构建目录、IDE 配置都在 `.gitignore` 中排除。

## 10. 性能考量

- 使用 `std::shared_ptr` 而非频繁拷贝
- 字符串传递使用 `const std::string&`
- 容器操作时考虑 `reserve()` 预先分配
- 避免在热路径中进行内存分配（使用对象池）

---

**最后更新**: 2026-02-16  
**标准版本**: C++11  
**维护人**: AgentFlow Team
