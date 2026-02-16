# AgentFlow

功能：基于C++ Actor反应式编程框架实现的一个Agent workflow开发编排与执行引擎系统。

AgentFlow采用"**开发编排框架**"和"**运行框架**"两套相对独立的体系来实现：
- **开发编排框架**：支持Agent和Workflow的定义、编排、装配、持久化等开发阶段的功能
- **运行框架**：支持Agent和Workflow实例的加载、调度、执行、监控等运行阶段的功能

---

## 第一部分：开发编排框架

开发编排框架负责Agent和Workflow的定义、编排、验证、持久化等设计阶段的工作，为运行框架提供静态的结构定义。整体框架由C++实现，但支持通过DSL（如JSON）进行编排定义。

### 1. Agent编排

Agent是AgentFlow系统中的执行主体。开发编排框架中的Agent定义包括：

1）**Agent结构定义**
   - Agent对象的属性（名称、版本、描述、标签等）
   - Agent生命周期策略配置：支持按需创建（Per-Request模式）、工厂模式、单例模式等

2）**Agent资源定义**
   - Agent内的共享资源结构定义（数据总线、控制流事件队列、共享数据存储等）
   - 资源的访问权限和管理策略配置
   - 资源与workflow之间的共享关系定义

3）**Agent组合**
   - 用户可以定义多个Workflow对象
   - 可以选择不同的Workflow组合成一个Agent对象
   - 支持Workflow之间的依赖和组织关系定义

### 2. Workflow编排

Workflow是典型的DAG图结构，由Node和Edge组成，用于描述业务流程和数据流转。开发编排框架中的Workflow编排包括：

1）**Workflow DSL定义**
   - 支持通过JSON等DSL语言进行编排
   - 每个Workflow对象需要指定对应的DSL定义文件
   - 支持Workflow的版本管理和灰度发布配置
   - **注意**：DSL的定义、解析、验证工作由单独的工具项目实现，不在AgentFlow核心项目中

2）**Node定义**
   
   Node是Workflow中执行具体任务的基本单元。Node的开发定义包括：
   
   - **Node控制面结构**：
     * Node的基本属性（名称、类型、优先级等）
     * Node的输入输出接口定义（参数名、类型、默认值等）
     * Node的错误处理策略（重试、降级、补偿等）
     * Node的超时配置和资源限制
   
   - **Node实现规范**：
     * 定义Node由两部分组成：
       - 控制面：由Actor组成，支持多个Actor协作；控制面负责运行面的生命周期管理、运维对接、监控日志采集等系统管理功能
       - 运行面：如果是C++实现，可以基于Actor，同时也支持非C++代码实现的可执行组件，比如shell脚本、so文件、jar包、pyc文件等外部程序实现
     * 运行面通过DSL配置指定运行模式

3）**Edge定义**
   
   Edge定义了Node之间的依赖关系和数据流转。Edge的定义包括：
   
   - **依赖关系**：Node之间的串并行关系定义（前驱、后继、并行等关系）
   - **数据类型和传递方式**：
     * 支持共享内存、消息队列等多种传递方式
     * 支持不同数据格式（文本、二进制、音频、视频等）的定义
   - **控制流事件**：定义Node之间支持的事件类型（开始、结束、失败、数据ready、同步等待、重入等）

### 3. 开发编排框架的持久化

- 本地或远程存储Agent定义和Workflow定义文件
- 支持版本管理、变更历史、灰度发布等开发运维需求
- 支持与CI/CD流程的集成

---

## 第二部分：运行框架

运行框架负责Agent和Workflow的实例加载、调度、执行、监控等运行阶段的工作。主要由C++实现，支持分布式部署和多线程并发执行。

### 1. Agent运行管理

1）**Agent实例**
   - Agent是AgentFlow系统真正的执行主体
   - 代表一个独立的业务处理实例
   - 每个Agent实例具有独立的生命周期、状态和资源空间
   - Agent是有状态的

2）**Agent外部接口**
   - Agent对外提供统一的访问接口
   - 这些接口由微服务层进行包装，包装之后的接口支持多种协议：gRPC、RestFull、websocket等

3）**Agent内部资源管理**
   - Agent对象内有多个Workflow共享的数据总线、控制流事件队列等资源
   - 运行框架负责这些资源的实际生命周期管理和访问控制
   - 确保Workflow之间的数据传递和事件管理的正确性和效率

4）**Agent实例策略**
   - 支持按照每个请求动态创建Agent实例的模式（Per-Request）
   - 支持长期运行的Agent实例池，多个实例支持多请求处理（Agent工厂模式）
   - 支持一个全局单例Agent实例，单个实例支持多请求处理（Agent单例模式）
   - 开发编排阶段提供配置，部署和运行时按需选择不同的实例策略

### 2. Workflow运行和Node调度

运行框架中的Workflow引擎负责：

1）**动态加载和解析**
   - 加载并解析DSL定义文件
   - 创建Workflow实例
   - 支持Workflow的动态加载、卸载和版本更新
   - 架构上支持Workflow的DSL流程可动态更新（如由大语言模型动态调整），无需重新创建Workflow或Agent实例

2）**DAG调度**
   - 根据Edge定义的依赖关系，计算Node的执行顺序
   - 确保Workflow按照预期的逻辑流程执行
   - 支持并行执行互不依赖的Node

3）**Node执行状态管理**
   - 管理每个Node的执行状态（待执行、执行中、成功、失败、重试等）
   - 处理Node的输入输出数据
   - 管理Node的生命周期

4）**高速数据总线**
   - 实现Node之间数据传递的高速总线
   - 支持不同格式数据的免拷贝共享（文本、二进制、音频、视频等）
   - 支持订阅发布机制实现Node间数据更新通知

5）**控制流事件管理**
   - 支持Node之间的控制流事件（开始、结束、失败、数据ready、同步等待、重入等）
   - 根据这些事件来调整Node的执行状态和流程走向
   - 支持复杂的流程控制（条件、循环、异常处理等）

### 3. AgentRuntime - 核心分层调度执行引擎

AgentRuntime是运行框架的核心，负责Agent、Workflow、Node三个层级的运行时管理、调度和执行。

**三层Master-Worker架构**：

- **Agent层Master**
  - 负责多Agent实例的生命周期管理、状态监控、资源分配和故障转移
  - 支持多Agent并发运行和动态创建销毁
  - 每个Agent实例有独立的资源空间和调度上下文

- **Workflow层Master**
  - 负责Workflow的DAG图解析和依赖关系计算
  - 负责多Workflow实例的调度决策和状态管理
  - 支持Workflow的动态加载、卸载和版本更新

- **Node层Worker池**
  - 负责workflow具体Node任务的执行，如果workflow中的Node都是基于一种语言实现，则可以在本地执行整个workflow或者子workflow，如果workflow中的Node有多种语言实现，则需要将不同语言实现的Node分配到不同的Worker池中执行，Worker池可以部署在本地或者远程的执行代理上
  - 支持Node多线程并发执行
  - 每个Node由一个Worker承载，架构上支持Worker按照微服务预部署，也可以支持动态分配和回收（比如不同的worker实例来自于不同的Pod和容器镜像）

**核心能力**：
- 直接管理Agent内的共享资源（高速总线、事件队列等）
- 对三层调度单位的执行状态和数据流转进行统一协调
- 支持多线程并发执行和动态负载均衡
- 保证线程安全和高性能
- 针对分布式部署，Master可扩展为多实例去中心化架构，多个Master实例之间可通过分布式协调机制实现调度决策的一致性
- AgentRuntime整体仍是AgentFlow项目不可分离的核心部分

### 4. 系统通信机制

运行框架实现的通信分为两个通信平面：

1）**业务数据通信面**
   - 用于Agent/Workflow/Node之间的业务数据流转
   - 基于用户态高速总线实现
   - 采用private protocol以获得高性能和高可靠性
   - 支持本地零拷贝共享和跨节点高速传输

2）**系统维护通信面**
   - 用于Node内部Actor、Node外部之间的消息交互
   - 基于Protoactor-cpp提供的Actor消息机制实现
   - 支持gRPC、Restful等标准协议
   - 用于所有Actor间通信，以及基于Actor实现的AgentFlow部件、Node控制面之间的通信

双平面通信机制在不同应用场景中相互协作，各司其职。

### 5. 监控和日志

运行框架提供了丰富的监控和日志功能：

- **监控**
  - Workflow执行的全链路监控，支持Node级别的详细监控，支持mesh式的分布式监控架构
  - Agent状态监控（生命周期、资源使用等）
  - Node执行状态监控
  - workflow实例执行过程中的性能指标采集（延迟、吞吐量等）

- **日志**
  - 错误日志记录
  - 执行跟踪日志
  - 状态变化日志
  - 可通过标准接口导出日志数据，支持与第三方日志系统对接

- **对接运维系统**
  - 通过标准gRPC、Restful接口与第三方运维系统对接
  - 支持自定义监控指标导出

### 6. 分布式执行支持

对于分布式部署场景，运行框架支持"**执行代理**"（Execution Agent）模式，作为独立的Worker服务部署：

- **执行代理概念**：轻量级的独立服务，与AgentRuntime Master分离部署，作为Node任务的执行单元
- **职责**：接收来自Master的Node任务指令，执行具体任务，并将结果回报给Workflow实例
- **架构支持**：支持多节点协同工作，形成分布式执行架构
- **部署方式**：Master服务部署在一个节点，多个执行代理分散部署在其他节点上

---

## 第三部分：基础设施

### 1. Actor机制

Actor是系统的最小执行单元，其实现、管理和消息交互等完全独立实现在Protoactor-cpp项目中。

- AgentFlow通过调用Protoactor-cpp提供的接口来创建和管理Node的控制面Actor对象
- Node内部的Actor之间通过消息通信和协作完成复杂任务
- Node可以由Actor实现（C++实现），也可以由外部程序实现（如shell脚本、jar包等）

### 2. 项目依赖关系

- **AgentFlow项目**：依赖Protoactor-cpp项目（提供Actor机制，代码仓：https://github.com/DanielKernel/Protoactor-cpp.git）
- **DSL工具项目**：独立实现，为开发编排框架提供DSL编辑、解析、验证等功能
- **监控日志收集**：通过标准协议与第三方运维系统对接

---

## 第四部分：核心组件实现说明

### 必须内置于AgentFlow项目中的核心组件

**开发编排框架部分**：
- Agent对象定义和组合框架
- Workflow对象定义框架（包含DAG图、Node和Edge的定义和组合）
- Workflow版本管理、持久化接口

**运行框架部分**：
- AgentRuntime三层Master-Worker调度执行引擎
- 跨Agent、跨Workflow、跨Node的高速数据总线和事件管理机制
- 数据总线支持不同格式数据的免拷贝共享和高效传输
- Workflow DAG执行引擎（DAG解析、调度、监控）
- Actor生命周期管理接口（基于Protoactor-cpp）
- 本地监控和日志采集接口

**实现说明**：
- 编程语言：使用C++17标准实现，编译器支持LLVM、GCC等主流编译器
- 操作系统：支持Linux、Unix等类Unix系统，不需要支持Windows
- 硬件平台：支持ARM64和x86_64架构
- 依赖库：Actor框架依赖Protoactor-cpp项目，其他也可以选择一些轻量级的主流第三方库（如JSON解析库、日志库等）

### 可选独立实现的组件

- **DSL编排工具**：用JSON格式描述Agent和Workflow流程文件，支持编辑、解析、验证、可视化等功能
- **监控日志收集器**：独立采集和汇聚运行框架的监控日志数据，对接第三方运维系统
- **Node运行面访问的后端服务**：如文件存储服务、大语言模型推理服务、其他第三方Agent或工具服务等

---

## 总结

AgentFlow通过分离"开发编排框架"和"运行框架"，实现了职责清晰、耦合度低的系统设计：
- **开发编排框架**关注Agent和Workflow的定义和编排
- **运行框架**关注Agent和Workflow实例的加载和执行
- 支持灵活的部署模式和扩展能力

---

## 编程规范和贡献

为了保持代码质量和团队协作的一致性，AgentFlow项目遵循严格的编程规范。

### 📋 必读文档

1. **[CODING_STANDARDS.md](CODING_STANDARDS.md)** - 编程规范指南
   - C++17标准代码风格（命名、缩进、注释等）
   - 内存管理和线程安全最佳实践
   - 测试规范和文档要求
   - 代码审查检查清单

2. **[DEVELOPMENT.md](DEVELOPMENT.md)** - 开发工作流指南
   - 快速开始和环境设置
   - 代码格式化和质量检查工具使用
   - 完整的开发流程（分支、编码、测试、提交、PR）
   - 常见问题解决

### 🔧 项目配置

项目包含以下工具配置文件用于保证代码质量：

- **.gitignore** - Git忽略规则，排除build、IDE配置等
- **.clang-format** - 代码格式化配置（Google风格）
- **.clang-tidy** - 静态分析和代码质量检查配置
- **.editorconfig** - 跨编辑器的一致性配置
- **.cmake-format** - CMake文件格式化配置

### ⚡ 快速检查清单

提交代码前，请确认：

```bash
# 构建和测试
./build.sh build && ./build.sh test

# 代码格式化
clang-format -r -i include/ src/ tests/ examples/

# 代码质量检查（可选）
clang-tidy -p build include/**/*.h src/*.cpp

# Git提交
git add <files>
git commit -m "type: descriptive message"
git push
```

### 📝 规范要点（快速参考）

| 规范项 | 要求 | 示例 |
|--------|------|------|
| **类名** | PascalCase | `class AgentRuntime` |
| **函数名** | snake_case | `void execute_node()` |
| **变量名** | snake_case | `int task_id` |
| **常量** | UPPER_CASE | `const int MAX_WORKERS = 4` |
| **缩进** | 2空格 | 无TAB |
| **行长** | 100字符（推荐）| 120字符（硬限制） |
| **内存** | shared_ptr/unique_ptr | 禁止原始指针 |
| **线程** | std::mutex保护共享资源 | lock_guard |
| **C++版本** | C++11标准 | 禁用C++14+ |

### 🚀 贡献流程

1. **Fork并克隆**
   ```bash
   git clone https://github.com/YourUsername/AgentFlow.git
   cd AgentFlow
   ```

2. **创建特性分支**
   ```bash
   git checkout -b feature/your-feature
   ```

3. **编码、测试、提交**
   - 遵循[CODING_STANDARDS.md](CODING_STANDARDS.md)
   - 编写对应的测试
   - 运行`./build.sh test`确保通过
   - 提交遵循规范的commit信息

4. **推送并创建Pull Request**
   ```bash
   git push origin feature/your-feature
   ```

5. **等待Code Review**
   - 项目维护者会检查代码规范和功能正确性
   - 根据反馈进行修改

### 📧 联系方式

- **问题报告**: GitHub Issues
- **贡献讨论**: GitHub Discussions
- **维护团队**: AgentFlow Team
