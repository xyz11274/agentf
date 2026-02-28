# AgentFlow

基于 C++ Actor 反应式编程框架实现的 Agent Workflow 开发编排与执行引擎。

## 核心特性

- **双框架架构**：开发编排框架 + 运行框架分离
- **工作流图执行**：支持条件分支、循环迭代、环回跳转、异常处理
- **三层调度**：Agent → Workflow → Node 的 Master-Worker 架构
- **高性能通信**：零拷贝数据总线、双平面通信机制
- **多语言支持**：C++、Python、Shell、Go、Java 等执行模式（由 Worker 执行引擎统一承载）
- **热重载**：运行时动态更新配置和代码

## 🚀 快速开始

### 环境要求

- CMake 3.10+
- C++17 编译器（GCC 7+ / Clang 5+）
- Linux/macOS

### 构建

```bash
# 克隆仓库
git clone https://github.com/DanielKernel/AgentFlow.git
cd AgentFlow

# 构建
./build.sh build

# 测试
./build.sh test

# 示例
./build.sh example
```

### 代码示例

```cpp
#include "agentflow/framework/agent.h"
#include "agentflow/framework/workflow.h"

using namespace agentflow;

// 创建 Agent
framework::Agent::Config agent_config;
agent_config.id = "my_agent";
auto agent = std::make_shared<framework::Agent>(agent_config);

// 创建 Workflow
framework::Workflow::Config wf_config;
wf_config.id = "my_workflow";
auto workflow = std::make_shared<framework::Workflow>(wf_config);

// 添加节点
framework::Node::Config node_config;
node_config.id = "task_1";
auto node = std::make_shared<framework::Node>(node_config);
workflow->add_node(node);

// 启动
agent->add_workflow(workflow);
agent->initialize();
agent->start();
```

## 📚 完整文档

### 📖 新用户入门
1. **[文档导航](docs/README.md)** - 所有文档的索引和说明
2. **[架构设计](docs/ARCHITECTURE.md)** - Framework/Runtime/DataBus 架构设计
3. **[开发与贡献指南](CONTRIBUTING.md)** - 环境配置、编程规范与贡献流程
4. **[快速参考](docs/README.md)** - 常用命令、测试与 CI/CD 快速指南

### 🏗️ 项目规划与功能状态
- **[项目规划](docs/REFACTORING.md)** - 功能实现状态、优先级和路线图
- **[重构记录](docs/REFACTORING.md)** - 近期重构与兼容策略

### 💻 开发和贡献
- **[贡献指南](CONTRIBUTING.md)** - 如何为项目做贡献
  - 代码规范和提交流程
  - 测试要求
  - Pull Request 指南
- **[发布与打包](publish/README.md)** - Agent 包开发与发布说明
- **[测试指南](docs/README.md)** - 测试方法和覆盖率要求

### ⚙️ CI/CD 和部署
- **[CI/CD 流水线](docs/README.md)** - 自动化测试和部署流程
- **[CI/CD 设置](docs/README.md)** - 配置和常见问题
- **[重构说明](docs/REFACTORING.md)** - 项目目录结构优化说明

## 项目结构

```
AgentFlow/
├── include/             # 头文件
│   ├── external/        # 公共 API（供外部调用方使用）
│   │   ├── agent/       # Agent 外部接口
│   │   └── workflow/    # Workflow/Node/Edge 外部接口
│   └── internal/        # 内部实现（仅供内部系统使用）
├── src/                 # 核心业务源代码（agent/workflow/runtime/vendor/utils）
├── vendor/              # 三方依赖和适配器
│   ├── vendor.sh        # 依赖下载脚本
│   └── adapter/         # 三方适配器实现
├── tests/               # 测试
│   ├── unit/            # 单元测试
│   └── functional/      # 功能测试
├── examples/            # 示例
├── docs/                # 文档
└── build/               # 构建输出和CMake配置
```

## 🔗 系统架构

```
┌─────────────────────────────────────────┐
│            Agent Level Master           │
│        (Agent 实例生命周期管理)           │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│          Workflow Level Master          │
│      (工作流图解析、调度、状态管理)       │
└──────────────────┬──────────────────────┘
                   │
    ┌──────────────┼──────────────┐
    │              │              │
┌───▼───┐      ┌───▼───┐      ┌───▼───┐
│Worker1│      │Worker2│      │WorkerN│
│(Node) │      │(Node) │      │(Node) │
└───────┘      └───────┘      └───────┘
```

## 🤝 贡献

欢迎贡献！请参考 [贡献指南](CONTRIBUTING.md)：

1. Fork 项目
2. 创建特性分支 (`git checkout -b feature/my-feature`)
3. 遵循 [编码规范](CONTRIBUTING.md)
4. 提交代码 (`git commit -m 'feat: add feature'`)
5. 推送分支 (`git push origin feature/my-feature`)
6. 创建 Pull Request

## 📊 项目状态

| 功能模块 | 完成度 | 详情 |
|---------|--------|------|
| 开发编排框架 | 85% | [查看详情](docs/REFACTORING.md) |
| 运行框架 | 75% | [查看详情](docs/REFACTORING.md) |
| 基础设施 | 48% | [查看详情](docs/REFACTORING.md) |
| 监控和日志 | 58% | [查看详情](docs/REFACTORING.md) |

完整的功能规划和实现状态见 [项目规划](docs/REFACTORING.md)。

## 📋 支持和帮助

- 📖 查看 [文档导航](docs/README.md) 了解所有可用文档
- 🐛 报告问题：使用 [问题报告模板](.github/ISSUE_TEMPLATE/bug_report.md)
- 💡 提出建议：使用 [功能请求模板](.github/ISSUE_TEMPLATE/feature_request.md)
- ⚙️ 配置问题：查看 [配置模板](.github/ISSUE_TEMPLATE/config.md)
- 🚀 性能优化：查看 [性能模板](.github/ISSUE_TEMPLATE/performance.md)

---

**维护者**: @DanielKernel
**文档最后更新**: 2026-02-18
**快速设置**: `./build.sh build && ./build.sh test` 验证所有组件是否正确安装
