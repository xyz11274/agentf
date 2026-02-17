# AgentFlow

基于 C++ Actor 反应式编程框架实现的 Agent Workflow 开发编排与执行引擎。

## 核心特性

- **双框架架构**：开发编排框架 + 运行框架分离
- **DAG 工作流**：支持条件分支、循环迭代、异常处理
- **三层调度**：Agent → Workflow → Node 的 Master-Worker 架构
- **高性能通信**：零拷贝数据总线、双平面通信机制
- **多语言支持**：C++、Python、Shell 等多种执行模式
- **热重载**：运行时动态更新配置和代码

## 快速开始

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

## 文档

| 文档 | 说明 |
|------|------|
| [架构设计](docs/ARCHITECTURE.md) | 5视图架构设计（场景、逻辑、开发、运行、部署） |
| [开发指南](docs/development.md) | 环境配置、编程规范、开发流程 |
| [依赖管理](docs/dependencies.md) | 外部依赖安装和隔离策略 |
| [测试指南](docs/testing.md) | 测试方法和组件使用示例 |
| [路线图](docs/roadmap.md) | 功能实现状态和开发计划 |

## 项目结构

```
AgentFlow/
├── include/agentflow/    # 头文件
│   ├── framework/        # Agent、Workflow、Node、Edge
│   ├── runtime/          # 运行时引擎、数据总线、控制流
│   ├── actor/            # Actor 系统
│   └── ...
├── src/                  # 源代码
├── tests/                # 测试
├── examples/             # 示例
├── docs/                 # 文档
├── configs/              # 配置文件
├── proto/                # Protobuf 定义
└── scripts/              # 构建和部署脚本
```

## 架构概览

```
┌─────────────────────────────────────────┐
│            Agent Level Master           │
│        (Agent 实例生命周期管理)           │
└──────────────────┬──────────────────────┘
                   │
┌──────────────────▼──────────────────────┐
│          Workflow Level Master          │
│        (DAG 解析、调度、状态管理)         │
└──────────────────┬──────────────────────┘
                   │
    ┌──────────────┼──────────────┐
    │              │              │
┌───▼───┐      ┌───▼───┐      ┌───▼───┐
│Worker1│      │Worker2│      │WorkerN│
│(Node) │      │(Node) │      │(Node) │
└───────┘      └───────┘      └───────┘
```

## 贡献

1. Fork 项目
2. 创建特性分支 (`git checkout -b feature/my-feature`)
3. 遵循 [编码规范](docs/development.md)
4. 提交代码 (`git commit -m 'feat: add feature'`)
5. 推送分支 (`git push origin feature/my-feature`)
6. 创建 Pull Request

## 许可证

MIT License

---

*维护: AgentFlow Team*
