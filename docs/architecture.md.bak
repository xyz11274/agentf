# AgentFlow 软件架构设计方案

> 版本: 1.0
> 日期: 2026-02-17
> 状态: 正式发布

---

## 目录

1. [概述](#1-概述)
2. [场景用例视图](#2-场景用例视图)
3. [逻辑视图](#3-逻辑视图)
4. [开发视图](#4-开发视图)
5. [运行视图](#5-运行视图)
6. [部署视图](#6-部署视图)
7. [附录](#7-附录)

---

## 1. 概述

### 1.1 项目定位

AgentFlow 是一个基于 C++17 Actor 反应式编程框架实现的 Agent Workflow 开发编排与执行引擎系统。系统采用"**开发编排框架**"和"**运行框架**"两套相对独立的体系来实现：

- **开发编排框架**：支持 Agent 和 Workflow 的定义、编排、装配、持久化等开发阶段的功能
- **运行框架**：支持 Agent 和 Workflow 实例的加载、调度、执行、监控等运行阶段的功能

### 1.2 设计目标

| 目标 | 描述 |
|------|------|
| **高性能** | 零拷贝数据传输、并行调度、高效事件处理 |
| **可扩展** | 支持分布式部署、多语言节点执行、插件化架构 |
| **高可用** | 热重载、故障转移、优雅降级 |
| **易用性** | DSL编排、可视化监控、丰富API |
| **标准化** | 遵循行业规范、开放接口协议 |

### 1.3 系统特性

```
┌─────────────────────────────────────────────────────────────────┐
│                      AgentFlow 核心特性                          │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │   DAG编排    │  │  Actor模型   │  │  热重载      │          │
│  │  条件/循环   │  │  消息驱动    │  │  版本管理    │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │  零拷贝总线  │  │  多语言支持  │  │  分布式执行  │          │
│  │  高速传输    │  │  C++/Python  │  │  执行代理    │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. 场景用例视图

### 2.1 系统上下文模型

#### 2.1.1 系统边界

```
                        ┌─────────────────────────────────┐
                        │          外部系统               │
                        │  ┌───────────┐  ┌───────────┐  │
                        │  │  CI/CD    │  │  监控平台  │  │
                        │  │  Jenkins  │  │ Prometheus│  │
                        │  └─────┬─────┘  └─────┬─────┘  │
                        └────────┼──────────────┼────────┘
                                 │              │
                                 ▼              ▼
┌──────────────┐         ┌─────────────────────────────┐         ┌──────────────┐
│   DSL编辑器   │────────▶│                             │◀────────│   日志系统   │
│   VSCode插件 │         │                             │         │   ELK/Loki  │
└──────────────┘         │        AgentFlow            │         └──────────────┘
                         │         Engine              │
┌──────────────┐         │                             │         ┌──────────────┐
│   后端服务    │◀───────▶│                             │◀───────▶│   存储服务   │
│   LLM API    │         │                             │         │   S3/HDFS   │
└──────────────┘         └─────────────────────────────┘         └──────────────┘
                                    │
                                    ▼
                         ┌─────────────────────────────┐
                         │        执行代理集群          │
                         │  ┌───────┐  ┌───────┐       │
                         │  │Worker1│  │Worker2│  ...  │
                         │  └───────┘  └───────┘       │
                         └─────────────────────────────┘
```

#### 2.1.2 外部角色定义

| 角色 | 描述 | 交互方式 |
|------|------|----------|
| **应用开发者** | 使用 AgentFlow 构建工作流应用的开发人员 | DSL定义、API调用 |
| **运维人员** | 负责系统部署、监控和维护的人员 | 运维接口、监控面板 |
| **外部系统** | 与 AgentFlow 集成的第三方系统 | gRPC/REST/WebSocket |
| **执行代理** | 分布式部署的 Node 执行单元 | Private Protocol |

### 2.2 用例模型

#### 2.2.1 核心用例图

```
                        ┌─────────────────────────────────────────┐
                        │              AgentFlow System            │
                        │                                         │
                        │  ┌─────────────┐    ┌─────────────┐     │
     ┌──────────┐       │  │  定义Agent  │    │  定义Workflow│     │
     │          │───────▶  └─────────────┘    └─────────────┘     │
     │ 应用开发者 │       │                                         │
     │          │───────▶  ┌─────────────┐    ┌─────────────┐     │
     └──────────┘       │  │  编排Node   │    │  配置Edge   │     │
                        │  └─────────────┘    └─────────────┘     │
                        │                                         │
     ┌──────────┐       │  ┌─────────────┐    ┌─────────────┐     │
     │          │───────▶  │  执行Workflow│    │  监控执行   │     │
     │ 调用方    │       │  └─────────────┘    └─────────────┘     │
     │          │───────▶  │                                     │
     └──────────┘       │  ┌─────────────┐    ┌─────────────┐     │
                        │  │  热重载配置  │    │  版本管理   │     │
     ┌──────────┐       │  └─────────────┘    └─────────────┘     │
     │          │───────▶  │                                     │
     │ 运维人员  │       │  ┌─────────────┐    ┌─────────────┐     │
     │          │───────▶  │  部署管理   │    │  日志查询   │     │
     └──────────┘       │  └─────────────┘    └─────────────┘     │
                        │                                         │
                        └─────────────────────────────────────────┘
```

#### 2.2.2 核心用例描述

**UC-01: 定义和编排 Workflow**

| 项目 | 内容 |
|------|------|
| **参与者** | 应用开发者 |
| **前置条件** | 已安装 AgentFlow SDK |
| **主成功场景** | 1. 创建 Workflow 定义<br>2. 添加 Node 节点<br>3. 配置 Edge 依赖<br>4. 设置条件表达式<br>5. 验证 DAG 结构 |
| **后置条件** | Workflow 定义持久化存储 |

**UC-02: 执行 Workflow**

| 项目 | 内容 |
|------|------|
| **参与者** | 调用方（外部系统或应用） |
| **前置条件** | Workflow 已定义并验证 |
| **主成功场景** | 1. 加载 Workflow 实例<br>2. 准备输入数据<br>3. DAG 调度执行<br>4. Node 并行处理<br>5. 输出结果返回 |
| **扩展场景** | 3a. 条件分支执行<br>3b. 循环迭代执行<br>4a. 错误重试<br>4b. 降级处理 |

**UC-03: 热重载和版本管理**

| 项目 | 内容 |
|------|------|
| **参与者** | 应用开发者、运维人员 |
| **前置条件** | 系统运行中 |
| **主成功场景** | 1. 上传新版本定义<br>2. 配置灰度策略<br>3. 触发热重载<br>4. 新请求使用新版本<br>5. 监控新版本状态 |
| **扩展场景** | 5a. 回滚到旧版本 |

### 2.3 业务交互模型

#### 2.3.1 Workflow 执行时序

```
┌────────┐     ┌────────┐     ┌────────┐     ┌────────┐     ┌────────┐
│ Client │     │Runtime │     │Workflow│     │ Node A │     │ Node B │
└───┬────┘     └───┬────┘     └───┬────┘     └───┬────┘     └───┬────┘
    │              │              │              │              │
    │ execute()    │              │              │              │
    │─────────────▶│              │              │              │
    │              │ load()       │              │              │
    │              │─────────────▶│              │              │
    │              │              │ validate()   │              │
    │              │              │──────────┐   │              │
    │              │              │◀─────────┘   │              │
    │              │              │              │              │
    │              │              │ get_executable_nodes()       │
    │              │              │──────────┐   │              │
    │              │              │◀─────────┘   │              │
    │              │              │              │              │
    │              │              │ execute()    │              │
    │              │              │─────────────▶│              │
    │              │              │              │──┐           │
    │              │              │              │◀─┘           │
    │              │              │◀─────────────│              │
    │              │              │              │              │
    │              │              │ on_complete()│              │
    │              │              │─────────────▶│              │
    │              │              │              │              │
    │              │              │ execute()    │              │
    │              │              │─────────────────────────────▶│
    │              │              │              │              │──┐
    │              │              │              │              │◀─┘
    │              │              │◀─────────────────────────────│
    │              │              │              │              │
    │◀─────────────│              │              │              │
    │  result      │              │              │              │
    │              │              │              │              │
```

#### 2.3.2 三层 Master-Worker 调度模型

```
                    ┌─────────────────────────────────────┐
                    │           Agent Level Master        │
                    │   ┌─────────────────────────────┐   │
                    │   │  • Agent 实例生命周期管理     │   │
                    │   │  • 资源分配和隔离            │   │
                    │   │  • 健康检查和故障转移        │   │
                    │   └─────────────────────────────┘   │
                    └──────────────┬──────────────────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │         Workflow Level Master       │
                    │   ┌─────────────────────────────┐   │
                    │   │  • DAG 解析和拓扑排序        │   │
                    │   │  • 调度决策和状态管理        │   │
                    │   │  • 条件/循环控制流          │   │
                    │   └─────────────────────────────┘   │
                    └──────────────┬──────────────────────┘
                                   │
         ┌─────────────────────────┼─────────────────────────┐
         │                         │                         │
    ┌────▼────┐               ┌────▼────┐               ┌────▼────┐
    │ Worker 1│               │ Worker 2│               │ Worker N│
    │┌───────┐│               │┌───────┐│               │┌───────┐│
    ││Node A ││               ││Node C ││               ││Node E ││
    │└───────┘│               │└───────┘│               │└───────┘│
    │┌───────┐│               │┌───────┐│               │         │
    ││Node B ││               ││Node D ││               │         │
    │└───────┘│               │└───────┘│               │         │
    └─────────┘               └─────────┘               └─────────┘
                    Node Level Worker Pool
```

---

## 3. 逻辑视图

### 3.1 逻辑划分模型

#### 3.1.1 系统分层架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           API Gateway Layer                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐   │
│  │   gRPC      │  │  REST API   │  │  WebSocket  │  │   CLI       │   │
│  └─────────────┘  └─────────────┘  └─────────────┘  └─────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
┌─────────────────────────────────────────────────────────────────────────┐
│                         Application Layer                               │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    Development Orchestration Framework            │  │
│  │  ┌─────────┐  ┌──────────┐  ┌─────────┐  ┌──────────────────┐   │  │
│  │  │  Agent  │  │ Workflow │  │  Node   │  │      Edge        │   │  │
│  │  │ Definer │  │ Definer  │  │ Definer │  │     Definer      │   │  │
│  │  └─────────┘  └──────────┘  └─────────┘  └──────────────────┘   │  │
│  │  ┌─────────┐  ┌──────────┐  ┌─────────┐  ┌──────────────────┐   │  │
│  │  │   DSL   │  │  Storage │  │ Version │  │   Validation     │   │  │
│  │  │ Parser  │  │  Manager │  │ Control │  │     Engine       │   │  │
│  │  └─────────┘  └──────────┘  └─────────┘  └──────────────────┘   │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                         Runtime Framework                         │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │  │
│  │  │   Agent     │  │  Workflow   │  │      Node               │  │  │
│  │  │  Runtime    │  │  Scheduler  │  │    Executor             │  │  │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘  │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │  │
│  │  │  Control    │  │  Hot Reload │  │     Metrics             │  │  │
│  │  │  Flow Mgr   │  │   Manager   │  │    Collector            │  │  │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
┌─────────────────────────────────────────────────────────────────────────┐
│                          Infrastructure Layer                           │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │ Actor System │  │ Data Bus     │  │ Event Loop   │  │  Network   │ │
│  │ (Protoactor) │  │(Zero-Copy)   │  │ (libevent)   │  │ (gRPC/WS)  │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └────────────┘ │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │ Serialization│  │ Storage      │  │ Thread Pool  │  │   Logger   │ │
│  │ (Protobuf)   │  │ (RocksDB)    │  │              │  │            │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

#### 3.1.2 核心模块职责

| 模块 | 命名空间 | 职责 |
|------|----------|------|
| **framework** | `agentflow::framework` | Agent/Workflow/Node/Edge 定义和组合 |
| **runtime** | `agentflow::runtime` | 运行时调度、执行、状态管理 |
| **actor** | `agentflow::actor` | Actor 系统集成和生命周期管理 |
| **network** | `agentflow::network` | 网络通信协议和适配器 |
| **serialization** | `agentflow::serialization` | 序列化/反序列化适配器 |
| **storage** | `agentflow::storage` | 持久化存储管理 |
| **dsl** | `agentflow::dsl` | DSL 解析和验证 |
| **event** | `agentflow::event` | 事件循环和异步 I/O |
| **basic** | `agentflow::basic` | 基础工具（日志、线程池） |

### 3.2 技术栈模型

#### 3.2.1 技术选型

```
┌────────────────────────────────────────────────────────────────────┐
│                         技术栈全景图                                │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│  编程语言         ┌──────────────────────────────────────────┐     │
│  ──────────       │  C++17 (Core) │ Python (Script) │ Shell │     │
│                   └──────────────────────────────────────────┘     │
│                                                                    │
│  构建工具         ┌──────────────────────────────────────────┐     │
│  ──────────       │  CMake 3.10+ │ Ninja │ Make │ Clang/GCC │     │
│                   └──────────────────────────────────────────┘     │
│                                                                    │
│  核心框架         ┌──────────────────────────────────────────┐     │
│  ──────────       │  Protoactor-cpp (Actor Model)            │     │
│                   │  libevent (Event Loop)                   │     │
│                   │  gRPC (RPC Framework)                    │     │
│                   └──────────────────────────────────────────┘     │
│                                                                    │
│  网络通信         ┌──────────────────────────────────────────┐     │
│  ──────────       │  gRPC │ WebSocket (libwebsockets)        │     │
│                   │  Private Protocol (Custom)               │     │
│                   └──────────────────────────────────────────┘     │
│                                                                    │
│  序列化           ┌──────────────────────────────────────────┐     │
│  ──────────       │  Protocol Buffers │ RapidJSON            │     │
│                   └──────────────────────────────────────────┘     │
│                                                                    │
│  安全加密         ┌──────────────────────────────────────────┐     │
│  ──────────       │  OpenSSL (TLS/SSL)                       │     │
│                   └──────────────────────────────────────────┘     │
│                                                                    │
│  存储引擎         ┌──────────────────────────────────────────┐     │
│  ──────────       │  RocksDB (Local) │ S3 (Remote)           │     │
│                   └──────────────────────────────────────────┘     │
│                                                                    │
│  监控可观测       ┌──────────────────────────────────────────┐     │
│  ──────────       │  Prometheus │ OpenTelemetry │ Grafana    │     │
│                   └──────────────────────────────────────────┘     │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

#### 3.2.2 外部依赖集成策略

| 依赖 | 集成方式 | 可替代方案 | 说明 |
|------|----------|------------|------|
| **Protoactor-cpp** | 直接依赖 | Stub 实现 | Actor 核心框架 |
| **gRPC** | 传递依赖 | 自定义 RPC | 分布式通信 |
| **libevent** | 直接依赖 | Stub 实现 | 事件循环 |
| **libwebsockets** | 可选依赖 | Stub 实现 | WebSocket 支持 |
| **OpenSSL** | 可选依赖 | Stub 实现 | TLS 加密 |
| **RapidJSON** | 可选依赖 | Stub 实现 | JSON 解析 |
| **Protobuf** | 传递依赖 | Stub 实现 | 序列化 |
| **GTest** | 测试依赖 | Catch2 | 单元测试 |

### 3.3 数据模型

#### 3.3.1 核心实体关系

```
                    ┌─────────────────────┐
                    │       Agent         │
                    │─────────────────────│
                    │ + id: AgentId       │
                    │ + name: string      │
                    │ + version: string   │
                    │ + policy: Lifecycle │
                    │ + state: AgentState │
                    └──────────┬──────────┘
                               │ 1:N
                               │
                    ┌──────────▼──────────┐
                    │      Workflow       │
                    │─────────────────────│
                    │ + id: WorkflowId    │
                    │ + name: string      │
                    │ + version: string   │
                    │ + dsl_file: string  │
                    │ + state: WorkflowSt │
                    └──────────┬──────────┘
                               │
              ┌────────────────┼────────────────┐
              │ 1:N            │            1:N │
              │                │                │
    ┌─────────▼─────┐   ┌──────▼──────┐   ┌────▼────────┐
    │     Node      │   │    Edge     │   │ ControlFlow │
    │───────────────│   │─────────────│   │─────────────│
    │ + id: NodeId  │   │ + id: EdgeId│   │ + loops     │
    │ + name: string│   │ + source    │   │ + handlers  │
    │ + type: string│   │ + target    │   │ + context   │
    │ + exec_mode   │   │ + condition │   │             │
    │ + timeout_ms  │   │ + data_fmt  │   │             │
    │ + max_retries │   │             │   │             │
    └───────────────┘   └─────────────┘   └─────────────┘
```

#### 3.3.2 数据类型定义

```cpp
// 基础数据类型
using DataVariant = std::variant<int64_t, double, std::string, bool>;
using DataMap = std::map<std::string, DataVariant>;

// ID 类型定义
using AgentId = std::string;
using WorkflowId = std::string;
using NodeId = std::string;
using EdgeId = std::string;

// 状态枚举
enum class AgentState { CREATED, INITIALIZED, RUNNING, PAUSED, STOPPED };
enum class WorkflowState { CREATED, LOADED, RUNNING, PAUSED, COMPLETED, FAILED };
enum class NodeState { WAITING, READY, EXECUTING, SUCCESS, FAILED, RETRYING };

// 生命周期策略
enum class LifecyclePolicy { PER_REQUEST, FACTORY, SINGLETON };

// 执行模式
enum class ExecutionMode { CPP_FUNCTION, SHELL_SCRIPT, EXECUTABLE, PYTHON_SCRIPT, SO_LIBRARY };
```

#### 3.3.3 DSL 数据模型 (JSON Schema)

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "properties": {
    "workflow": {
      "type": "object",
      "properties": {
        "id": { "type": "string" },
        "name": { "type": "string" },
        "version": { "type": "string" },
        "nodes": {
          "type": "array",
          "items": { "$ref": "#/definitions/node" }
        },
        "edges": {
          "type": "array",
          "items": { "$ref": "#/definitions/edge" }
        }
      }
    }
  },
  "definitions": {
    "node": {
      "type": "object",
      "properties": {
        "id": { "type": "string" },
        "name": { "type": "string" },
        "type": { "type": "string" },
        "execution_mode": { "type": "string" },
        "command": { "type": "string" },
        "timeout_ms": { "type": "integer" },
        "max_retries": { "type": "integer" },
        "input_params": { "type": "array" },
        "output_params": { "type": "array" }
      }
    },
    "edge": {
      "type": "object",
      "properties": {
        "id": { "type": "string" },
        "source": { "type": "string" },
        "target": { "type": "string" },
        "condition": { "type": "string" },
        "data_formats": { "type": "array" }
      }
    }
  }
}
```

---

## 4. 开发视图

### 4.1 代码组织模型

#### 4.1.1 推荐目录结构

```
AgentFlow/
├── CMakeLists.txt                    # 主构建配置
├── cmake/                            # CMake 模块
│   ├── Dependencies.cmake            # 依赖管理
│   ├── CompilerOptions.cmake         # 编译选项
│   └── Coverage.cmake                # 代码覆盖率
│
├── include/                          # 公共头文件
│   └── agentflow/                    # 命名空间目录
│       ├── common.h                  # 公共类型定义
│       ├── version.h                 # 版本信息
│       │
│       ├── core/                     # 核心框架
│       │   ├── agent.hpp             # Agent 定义
│       │   ├── workflow.hpp          # Workflow 定义
│       │   ├── node.hpp              # Node 定义
│       │   └── edge.hpp              # Edge 定义
│       │
│       ├── runtime/                  # 运行时引擎
│       │   ├── agent_runtime.hpp     # 运行时引擎
│       │   ├── scheduler.hpp         # 调度器
│       │   ├── executor.hpp          # 执行器
│       │   ├── databus.hpp           # 数据总线
│       │   ├── zero_copy_databus.hpp # 零拷贝总线
│       │   ├── control_flow.hpp      # 控制流管理
│       │   ├── hot_reload.hpp        # 热重载
│       │   └── metrics.hpp           # 监控指标
│       │
│       ├── actor/                    # Actor 系统
│       │   ├── actor_system.hpp      # Actor 系统管理
│       │   ├── adapter.hpp           # 适配器接口
│       │   ├── node_actor.hpp        # Node Actor
│       │   ├── workflow_actor.hpp    # Workflow Actor
│       │   ├── message_serializer.hpp# 消息序列化
│       │   └── ask_pattern.hpp       # Ask 模式
│       │
│       ├── dsl/                      # DSL 支持
│       │   ├── parser.hpp            # DSL 解析器
│       │   ├── validator.hpp         # DSL 验证器
│       │   └── json_adapter.hpp      # JSON 适配器
│       │
│       ├── network/                  # 网络通信
│       │   ├── server.hpp            # 服务器接口
│       │   ├── client.hpp            # 客户端接口
│       │   ├── private_protocol.hpp  # 私有协议
│       │   ├── request.hpp           # 请求封装
│       │   ├── response.hpp          # 响应封装
│       │   ├── ssl_adapter.hpp       # SSL 适配器
│       │   └── websocket_adapter.hpp # WebSocket 适配器
│       │
│       ├── storage/                  # 存储管理
│       │   ├── storage.hpp           # 存储接口
│       │   ├── agent_storage.hpp     # Agent 存储
│       │   ├── workflow_storage.hpp  # Workflow 存储
│       │   └── storage_manager.hpp   # 存储管理器
│       │
│       ├── serialization/            # 序列化
│       │   ├── serializer.hpp        # 序列化接口
│       │   └── proto_adapter.hpp     # Protobuf 适配器
│       │
│       ├── event/                    # 事件系统
│       │   ├── event_loop.hpp        # 事件循环
│       │   └── eventloop_adapter.hpp # 事件循环适配器
│       │
│       └── utils/                    # 工具类
│           ├── logger.hpp            # 日志工具
│           ├── thread_pool.hpp       # 线程池
│           └── string_utils.hpp      # 字符串工具
│
├── src/                              # 源代码实现
│   ├── core/                         # 核心框架实现
│   │   ├── agent.cpp
│   │   ├── workflow.cpp
│   │   ├── node.cpp
│   │   └── edge.cpp
│   │
│   ├── runtime/                      # 运行时实现
│   │   ├── agent_runtime.cpp
│   │   ├── scheduler.cpp
│   │   ├── executor.cpp
│   │   ├── databus.cpp
│   │   ├── zero_copy_databus.cpp
│   │   ├── control_flow.cpp
│   │   ├── hot_reload.cpp
│   │   ├── code_loader.cpp
│   │   ├── local_channel.cpp
│   │   └── metrics.cpp
│   │
│   ├── actor/                        # Actor 实现s
│   │   ├── actor_system.cpp
│   │   ├── adapter_factory.cpp
│   │   ├── stub_impl.cpp
│   │   ├── protoactor_impl.cpp
│   │   ├── node_actor.cpp
│   │   ├── workflow_actor.cpp
│   │   ├── message_serializer.cpp
│   │   └── ask_pattern_manager.cpp
│   │
│   ├── dsl/                          # DSL 实现
│   │   ├── parser.cpp
│   │   ├── validator.cpp
│   │   ├── workflow_parser.cpp
│   │   ├── json_adapter_factory.cpp
│   │   ├── json_adapter_stub.cpp
│   │   └── json_adapter_rapidjson.cpp
│   │
│   ├── network/                      # 网络实现
│   │   ├── server.cpp
│   │   ├── client.cpp
│   │   ├── adapter_factory.cpp
│   │   ├── private_protocol.cpp
│   │   ├── request.cpp
│   │   └── response.cpp
│   │
│   ├── storage/                      # 存储实现
│   │   ├── storage.cpp
│   │   ├── agent_storage.cpp
│   │   ├── workflow_storage.cpp
│   │   └── storage_manager.cpp
│   │
│   ├── serialization/                # 序列化实现
│   │   └── adapter_factory.cpp
│   │
│   ├── event/                        # 事件实现
│   │   ├── adapter_factory.cpp
│   │   └── eventloop_adapter/
│   │       └── libevent_impl.cpp
│   │
│   └── utils/                        # 工具实现
│       ├── logger.cpp
│       └── thread_pool.cpp
│
├── proto/                            # Protobuf 定义
│   ├── agentflow.proto               # 核心消息定义
│   ├── workflow.proto                # Workflow 消息
│   └── node.proto                    # Node 消息
│
├── tests/                            # 测试代码
│   ├── unit/                         # 单元测试
│   │   ├── core/
│   │   ├── runtime/
│   │   ├── actor/
│   │   └── network/
│   │
│   ├── integration/                  # 集成测试
│   │   └── workflow_execution_test.cpp
│   │
│   └── e2e/                          # 端到端测试
│       └── full_pipeline_test.cpp
│
├── examples/                         # 示例代码
│   ├── basic_example.cpp
│   ├── dsl_example.cpp
│   ├── control_flow_example.cpp
│   ├── metrics_example.cpp
│   └── dsl/                          # DSL 示例文件
│       └── sample_workflow.json
│
├── docs/                             # 文档
│   ├── ARCHITECTURE.md               # 架构设计
│   ├── API.md                        # API 文档
│   ├── DEPLOYMENT.md                 # 部署指南
│   └── DEVELOPMENT.md                # 开发指南
│
├── scripts/                          # 脚本
│   ├── build.sh                      # 构建脚本
│   ├── test.sh                       # 测试脚本
│   ├── install_dependencies.sh       # 依赖安装
│   └── docker/                       # Docker 相关
│       ├── Dockerfile
│       └── docker-compose.yml
│
├── configs/                          # 配置文件
│   ├── default.yaml                  # 默认配置
│   └── logging.yaml                  # 日志配置
│
└── third_party/                      # 第三方依赖（可选）
    └── README.md
```

#### 4.1.2 命名规范

| 类型 | 规范 | 示例 |
|------|------|------|
| **命名空间** | 小写下划线 | `agentflow::runtime` |
| **类名** | PascalCase | `AgentRuntime` |
| **函数名** | snake_case | `execute_workflow()` |
| **变量名** | snake_case | `workflow_id` |
| **常量** | UPPER_CASE | `MAX_WORKERS` |
| **文件名** | snake_case | `agent_runtime.cpp` |
| **头文件** | snake_case.hpp | `agent_runtime.hpp` |

### 4.2 代码构建工程

#### 4.2.1 CMake 构建结构

```cmake
# 主 CMakeLists.txt 结构示例
cmake_minimum_required(VERSION 3.16)
project(AgentFlow VERSION 1.0.0 LANGUAGES CXX)

# 编译选项
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 选项
option(AGENTFLOW_BUILD_TESTS "Build tests" ON)
option(AGENTFLOW_BUILD_EXAMPLES "Build examples" ON)
option(AGENTFLOW_WITH_PROTOACTOR "Enable Protoactor" ON)
option(AGENTFLOW_WITH_LIBEVENT "Enable libevent" ON)
option(AGENTFLOW_ENABLE_COVERAGE "Enable coverage" OFF)

# 包含模块
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Dependencies)
include(CompilerOptions)

# 子目录
add_subdirectory(src)
if(AGENTFLOW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
if(AGENTFLOW_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
```

#### 4.2.2 构建目标

```
构建目标结构:

agentflow (静态库)
├── agentflow_core
│   ├── agent.cpp
│   ├── workflow.cpp
│   ├── node.cpp
│   └── edge.cpp
│
├── agentflow_runtime
│   ├── agent_runtime.cpp
│   ├── scheduler.cpp
│   ├── databus.cpp
│   └── control_flow.cpp
│
├── agentflow_actor
│   ├── actor_system.cpp
│   └── [protoactor_impl.cpp] (可选)
│
├── agentflow_dsl
│   ├── workflow_parser.cpp
│   └── [json_adapter_rapidjson.cpp] (可选)
│
├── agentflow_network
│   ├── private_protocol.cpp
│   └── request.cpp, response.cpp
│
├── agentflow_storage
│   └── storage_manager.cpp
│
└── agentflow_utils
    ├── logger.cpp
    └── thread_pool.cpp

可执行目标:
├── example_basic
├── example_dsl
├── example_control_flow
├── test_agentflow (单元测试)
└── benchmark_agentflow (性能测试)
```

#### 4.2.3 构建命令

```bash
# 开发构建
./scripts/build.sh build

# 发布构建
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 运行测试
./scripts/build.sh test
# 或
ctest --test-dir build --output-on-failure

# 安装
cmake --install build --prefix /usr/local

# 代码覆盖率
cmake -B build -DAGENTFLOW_ENABLE_COVERAGE=ON
cmake --build build
ctest --test-dir build
gcovr -r . build
```

---

## 5. 运行视图

### 5.1 并发运行模型

#### 5.1.1 线程模型

```
┌─────────────────────────────────────────────────────────────────────┐
│                        AgentRuntime 进程                             │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────┐     ┌─────────────────────────────────────────┐   │
│  │ Main Thread │     │           Worker Thread Pool            │   │
│  │             │     │  ┌───────┐ ┌───────┐ ┌───────┐         │   │
│  │ • 初始化    │     │  │Worker1│ │Worker2│ │Worker3│ ...     │   │
│  │ • 配置加载  │     │  └───┬───┘ └───┬───┘ └───┬───┘         │   │
│  │ • 信号处理  │     │      │         │         │              │   │
│  └─────────────┘     │      ▼         ▼         ▼              │   │
│                      │  ┌───────────────────────────────┐     │   │
│                      │  │       Task Queue              │     │   │
│                      │  │  ┌─────┐ ┌─────┐ ┌─────┐     │     │   │
│                      │  │  │Task1│ │Task2│ │Task3│ ... │     │   │
│                      │  │  └─────┘ └─────┘ └─────┘     │     │   │
│                      │  └───────────────────────────────┘     │   │
│                      └─────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Event Loop Thread                         │   │
│  │  ┌───────────────┐ ┌───────────────┐ ┌───────────────┐     │   │
│  │  │ Timer Events  │ │ I/O Events    │ │ Signal Events │     │   │
│  │  └───────────────┘ └───────────────┘ └───────────────┘     │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Actor System Threads                      │   │
│  │  ┌───────────────┐ ┌───────────────┐ ┌───────────────┐     │   │
│  │  │ Mailbox Proc1 │ │ Mailbox Proc2 │ │ Mailbox ProcN │     │   │
│  │  └───────────────┘ └───────────────┘ └───────────────┘     │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 5.1.2 并发控制机制

```cpp
// 数据总线并发控制
class ZeroCopyDataBus {
    mutable std::shared_mutex data_mutex_;  // 读写锁

    void lock_read() const { data_mutex_.lock_shared(); }
    void unlock_read() const { data_mutex_.unlock_shared(); }
    void lock_write() { data_mutex_.lock(); }
    void unlock_write() { data_mutex_.unlock(); }
};

// 任务队列并发控制
class TaskQueue {
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    void push(Task task) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        tasks_.push(std::move(task));
        queue_cv_.notify_one();
    }

    Task pop() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !tasks_.empty(); });
        auto task = std::move(tasks_.front());
        tasks_.pop();
        return task;
    }
};

// Agent 状态管理并发控制
class Agent {
    mutable std::mutex mutex_;
    mutable std::mutex event_mutex_;
};
```

#### 5.1.3 负载均衡策略

```
负载均衡算法:

                    ┌─────────────────────────────┐
                    │       Load Balancer         │
                    │                             │
                    │  worker_loads_: [2, 5, 1, 4] │
                    │                             │
                    └──────────────┬──────────────┘
                                   │
                    get_least_loaded_worker()
                                   │
                                   ▼
                    ┌─────────────────────────────┐
                    │   Worker Selection:         │
                    │   Worker 2 (load = 1)       │
                    └─────────────────────────────┘

策略说明:
1. 维护每个 Worker 的负载计数
2. 新任务分配给负载最低的 Worker
3. 任务完成后更新负载计数
4. 支持动态 Worker 增减
```

### 5.2 通信模型

#### 5.2.1 双平面通信机制

```
┌─────────────────────────────────────────────────────────────────────┐
│                        双平面通信架构                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │                   业务数据通信面 (Data Plane)                  │ │
│  │                                                               │ │
│  │  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐ │ │
│  │  │ ZeroCopy      │    │ Private       │    │ Shared        │ │ │
│  │  │ DataBus       │    │ Protocol      │    │ Memory        │ │ │
│  │  │               │    │               │    │               │ │ │
│  │  │ • 本地零拷贝  │    │ • 跨节点传输  │    │ • POSIX SHM   │ │ │
│  │  │ • 订阅发布    │    │ • 高性能编解码│    │ • System V    │ │ │
│  │  └───────────────┘    └───────────────┘    └───────────────┘ │ │
│  │                                                               │ │
│  │  特点: 高性能、低延迟、批量传输                              │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │                  系统维护通信面 (Control Plane)                │ │
│  │                                                               │ │
│  │  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐ │ │
│  │  │ Actor Message │    │ gRPC          │    │ REST API      │ │ │
│  │  │ (Protoactor)  │    │               │    │               │ │ │
│  │  │               │    │               │    │               │ │ │
│  │  │ • 消息传递    │    │ • 远程调用    │    │ • HTTP 接口   │ │ │
│  │  │ • Ask/Tell    │    │ • 流式传输    │    │ • 资源操作    │ │ │
│  │  └───────────────┘    └───────────────┘    └───────────────┘ │ │
│  │                                                               │ │
│  │  特点: 标准化、可观测、易于调试                              │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 5.2.2 Actor 消息流

```
Actor 消息通信流程:

┌────────────┐      ┌────────────┐      ┌────────────┐
│ NodeActor  │      │WorkflowActor│     │ AgentActor │
│   (A)      │      │    (B)     │      │    (C)     │
└─────┬──────┘      └──────┬─────┘      └──────┬─────┘
      │                    │                   │
      │     Tell(Msg)      │                   │
      │───────────────────▶│                   │
      │                    │                   │
      │                    │   Ask(Msg)        │
      │                    │──────────────────▶│
      │                    │                   │
      │                    │◀──────────────────│
      │                    │   Response        │
      │                    │                   │
      │◀───────────────────│                   │
      │     Notify         │                   │
      │                    │                   │

消息类型:
┌─────────────────────────────────────────────────────────────────┐
│ • Tell: 单向消息，不等待响应                                    │
│ • Ask: 双向消息，等待响应 (Future/Promise)                      │
│ • Broadcast: 广播消息，发送给所有订阅者                         │
│ • Forward: 转发消息，保持原始发送者信息                         │
└─────────────────────────────────────────────────────────────────┘
```

### 5.3 状态机模型

#### 5.3.1 Agent 状态机

```
                    AgentState State Machine

                         ┌─────────┐
                         │ CREATED │
                         └────┬────┘
                              │ initialize()
                              ▼
                       ┌──────────────┐
                       │ INITIALIZED  │
                       └──────┬───────┘
                              │ start()
                              ▼
         ┌───────────────────┐───────────────────┐
         │                                       │
    ┌────▼────┐                            ┌─────▼─────┐
    │ RUNNING │◀─────── resume() ──────────│  PAUSED   │
    └────┬────┘                            └───────────┘
         │                                       ▲
         │               pause()                 │
         └───────────────────────────────────────┘
         │
         │ stop()
         ▼
    ┌─────────┐
    │ STOPPED │
    └─────────┘
```

#### 5.3.2 Workflow 状态机

```
                    WorkflowState State Machine

                         ┌─────────┐
                         │ CREATED │
                         └────┬────┘
                              │ load_from_dsl()
                              ▼
                        ┌──────────┐
                        │  LOADED  │
                        └────┬─────┘
                             │ execute()
                             ▼
        ┌───────────────────┐───────────────────┐
        │                                       │
   ┌────▼────┐                            ┌─────▼─────┐
   │ RUNNING │◀─────── resume() ──────────│  PAUSED   │
   └────┬────┘                            └───────────┘
        │                                       ▲
        │               pause()                 │
        └───────────────────────────────────────┘
        │
        ├───────────────────────────────┐
        │                               │
        ▼                               ▼
   ┌──────────┐                   ┌──────────┐
   │COMPLETED │                   │  FAILED  │
   └──────────┘                   └──────────┘
```

#### 5.3.3 Node 状态机

```
                    NodeState State Machine

                         ┌─────────┐
                         │ WAITING │
                         └────┬────┘
                              │ inputs_ready()
                              ▼
                         ┌─────────┐
                         │  READY  │
                         └────┬────┘
                              │ execute()
                              ▼
                       ┌──────────────┐
                       │  EXECUTING   │
                       └──────┬───────┘
                              │
              ┌───────────────┼───────────────┐
              │               │               │
              ▼               ▼               ▼
        ┌──────────┐   ┌──────────┐   ┌──────────┐
        │ SUCCESS  │   │  FAILED  │   │ RETRYING │
        └──────────┘   └────┬─────┘   └────┬─────┘
                            │               │
                            │               │ retry()
                            │               ▼
                            │        ┌──────────────┐
                            │        │  EXECUTING   │
                            │        └──────────────┘
                            │
                            └──────────────────────▶ [max_retries exceeded]
```

---

## 6. 部署视图

### 6.1 软件包交付模型

#### 6.1.1 交付物清单

```
AgentFlow Release Package
│
├── agentflow-1.0.0-linux-x86_64.tar.gz
│   ├── bin/
│   │   ├── agentflow_server      # 主服务程序
│   │   ├── agentflow_cli         # 命令行工具
│   │   └── agentflow_agent       # 执行代理程序
│   │
│   ├── lib/
│   │   ├── libagentflow.so       # 共享库
│   │   ├── libagentflow.a        # 静态库
│   │   └── plugins/              # 插件目录
│   │
│   ├── include/
│   │   └── agentflow/            # 开发头文件
│   │
│   ├── configs/
│   │   ├── server.yaml           # 服务配置
│   │   ├── logging.yaml          # 日志配置
│   │   └── workflows/            # 示例工作流
│   │
│   └── docs/
│       ├── README.md
│       ├── API.md
│       └── CHANGELOG.md
│
├── agentflow-1.0.0-devel.tar.gz  # 开发包
│   ├── include/
│   ├── lib/
│   └── cmake/
│
└── docker/
    ├── agentflow-server:1.0.0
    ├── agentflow-agent:1.0.0
    └── agentflow-all:1.0.0
```

#### 6.1.2 软件包格式

| 格式 | 平台 | 说明 |
|------|------|------|
| `.tar.gz` | Linux (通用) | 预编译二进制包 |
| `.deb` | Debian/Ubuntu | APT 包管理器 |
| `.rpm` | RHEL/CentOS | YUM/DNF 包管理器 |
| Docker Image | 容器环境 | Docker Hub 分发 |
| `.pkg.tar.zst` | Arch Linux | Pacman 包管理器 |

### 6.2 服务部署模型

#### 6.2.1 单机部署模式

```
单机部署架构:

┌─────────────────────────────────────────────────────────────────┐
│                        Physical Server                          │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                   AgentFlow Server                       │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐     │   │
│  │  │   Runtime   │  │  Scheduler  │  │   Storage   │     │   │
│  │  │   Engine    │  │             │  │   Manager   │     │   │
│  │  └─────────────┘  └─────────────┘  └─────────────┘     │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐     │   │
│  │  │ Worker Pool │  │ Data Bus    │  │ Metrics     │     │   │
│  │  │ (4 threads) │  │             │  │ Collector   │     │   │
│  │  └─────────────┘  └─────────────┘  └─────────────┘     │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐            │
│  │  RocksDB    │  │ Prometheus  │  │   Grafana   │            │
│  │  (Storage)  │  │ (Metrics)   │  │ (Dashboard) │            │
│  └─────────────┘  └─────────────┘  └─────────────┘            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

#### 6.2.2 分布式部署模式

```
分布式部署架构:

                    ┌─────────────────────────────────────┐
                    │            Load Balancer            │
                    │          (Nginx/HAProxy)            │
                    └──────────────┬──────────────────────┘
                                   │
         ┌─────────────────────────┼─────────────────────────┐
         │                         │                         │
         ▼                         ▼                         ▼
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  Server Node 1  │     │  Server Node 2  │     │  Server Node 3  │
│                 │     │                 │     │                 │
│ ┌─────────────┐ │     │ ┌─────────────┐ │     │ ┌─────────────┐ │
│ │  Master     │ │     │ │  Master     │ │     │ │  Master     │ │
│ │  Runtime    │ │     │ │  Runtime    │ │     │ │  Runtime    │ │
│ └─────────────┘ │     │ └─────────────┘ │     │ └─────────────┘ │
│                 │     │                 │     │                 │
│ ┌─────────────┐ │     │ ┌─────────────┐ │     │ ┌─────────────┐ │
│ │  Worker     │ │     │ │  Worker     │ │     │ │  Worker     │ │
│ │  Pool       │ │     │ │  Pool       │ │     │ │  Pool       │ │
│ └─────────────┘ │     │ └─────────────┘ │     │ └─────────────┘ │
└─────────────────┘     └─────────────────┘     └─────────────────┘
         │                         │                         │
         └─────────────────────────┼─────────────────────────┘
                                   │
                    ┌──────────────▼──────────────┐
                    │      Coordination Layer      │
                    │  ┌───────────────────────┐  │
                    │  │   etcd / Consul       │  │
                    │  │   (Service Discovery) │  │
                    │  └───────────────────────┘  │
                    │  ┌───────────────────────┐  │
                    │  │   Redis Cluster       │  │
                    │  │   (State/DLock)       │  │
                    │  └───────────────────────┘  │
                    └─────────────────────────────┘
                                   │
                    ┌──────────────▼──────────────┐
                    │      Shared Storage Layer    │
                    │  ┌───────────────────────┐  │
                    │  │   MinIO / S3          │  │
                    │  │   (DSL/Config Store)  │  │
                    │  └───────────────────────┘  │
                    │  ┌───────────────────────┐  │
                    │  │   PostgreSQL          │  │
                    │  │   (Metadata Store)    │  │
                    │  └───────────────────────┘  │
                    └─────────────────────────────┘
```

#### 6.2.3 Kubernetes 部署模式

```yaml
# Kubernetes 部署架构
apiVersion: apps/v1
kind: Deployment
metadata:
  name: agentflow-server
spec:
  replicas: 3
  selector:
    matchLabels:
      app: agentflow-server
  template:
    spec:
      containers:
      - name: server
        image: agentflow/server:1.0.0
        ports:
        - containerPort: 50051  # gRPC
        - containerPort: 8080   # REST
        resources:
          requests:
            cpu: "500m"
            memory: "512Mi"
          limits:
            cpu: "2000m"
            memory: "2Gi"
        env:
        - name: AGENTFLOW_WORKERS
          value: "4"
        volumeMounts:
        - name: config
          mountPath: /etc/agentflow
      volumes:
      - name: config
        configMap:
          name: agentflow-config
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: agentflow-agent
spec:
  replicas: 5
  template:
    spec:
      containers:
      - name: agent
        image: agentflow/agent:1.0.0
        env:
        - name: AGENTFLOW_MASTER
          value: "agentflow-server:50051"
```

```
Kubernetes 部署视图:

┌─────────────────────────────────────────────────────────────────────────┐
│                          Kubernetes Cluster                              │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                        Namespace: agentflow                      │   │
│  │                                                                 │   │
│  │  ┌─────────────────────────────────────────────────────────┐    │   │
│  │  │               Deployment: agentflow-server               │    │   │
│  │  │                                                         │    │   │
│  │  │  ┌─────────┐  ┌─────────┐  ┌─────────┐                 │    │   │
│  │  │  │  Pod 1  │  │  Pod 2  │  │  Pod 3  │                 │    │   │
│  │  │  │ Master  │  │ Master  │  │ Master  │                 │    │   │
│  │  │  └─────────┘  └─────────┘  └─────────┘                 │    │   │
│  │  └─────────────────────────────────────────────────────────┘    │   │
│  │                                                                 │   │
│  │  ┌─────────────────────────────────────────────────────────┐    │   │
│  │  │               Deployment: agentflow-agent                │    │   │
│  │  │                                                         │    │   │
│  │  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐   │    │   │
│  │  │  │ Worker 1│  │ Worker 2│  │ Worker 3│  │ Worker N│   │    │   │
│  │  │  └─────────┘  └─────────┘  └─────────┘  └─────────┘   │    │   │
│  │  └─────────────────────────────────────────────────────────┘    │   │
│  │                                                                 │   │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐   │   │
│  │  │ Service        │  │ ConfigMap      │  │ Secret         │   │   │
│  │  │ (ClusterIP)    │  │ (Configuration)│  │ (Credentials)  │   │   │
│  │  └────────────────┘  └────────────────┘  └────────────────┘   │   │
│  │                                                                 │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                        Shared Resources                          │   │
│  │                                                                 │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐             │   │
│  │  │   etcd      │  │   Redis     │  │   MinIO     │             │   │
│  │  │  (State)    │  │  (Cache)    │  │  (Storage)  │             │   │
│  │  └─────────────┘  └─────────────┘  └─────────────┘             │   │
│  │                                                                 │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 6.3 运维监控模型

#### 6.3.1 监控指标体系

```
┌─────────────────────────────────────────────────────────────────────┐
│                        监控指标层次结构                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  系统层指标                                                         │
│  ├── CPU 使用率                                                     │
│  ├── 内存使用率                                                     │
│  ├── 磁盘 I/O                                                       │
│  ├── 网络 I/O                                                       │
│  └── 文件描述符数量                                                 │
│                                                                     │
│  应用层指标                                                         │
│  ├── 请求 QPS                                                       │
│  ├── 请求延迟 (P50/P95/P99)                                         │
│  ├── 错误率                                                         │
│  ├── 活跃连接数                                                     │
│  └── Goroutine/线程数                                               │
│                                                                     │
│  业务层指标                                                         │
│  ├── Workflow 执行数                                                │
│  ├── Node 执行成功率                                                │
│  ├── Workflow 执行时长                                              │
│  ├── Agent 实例数                                                   │
│  ├── 热重载次数                                                     │
│  └── 版本分布                                                       │
│                                                                     │
│  自定义指标                                                         │
│  ├── Node 执行耗时分布                                              │
│  ├── 数据总线使用率                                                 │
│  ├── Worker 负载分布                                                │
│  └── DAG 深度分布                                                   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 6.3.2 告警规则示例

```yaml
# Prometheus AlertManager Rules
groups:
- name: agentflow-alerts
  rules:
  - alert: HighErrorRate
    expr: |
      sum(rate(agentflow_workflow_errors_total[5m])) /
      sum(rate(agentflow_workflow_total[5m])) > 0.05
    for: 5m
    labels:
      severity: critical
    annotations:
      summary: "High workflow error rate"
      description: "Error rate is {{ $value | humanizePercentage }}"

  - alert: HighLatency
    expr: |
      histogram_quantile(0.95,
        sum(rate(agentflow_workflow_duration_seconds_bucket[5m])) by (le)
      ) > 30
    for: 5m
    labels:
      severity: warning
    annotations:
      summary: "High workflow latency"
      description: "P95 latency is {{ $value }}s"

  - alert: AgentDown
    expr: up{job="agentflow-server"} == 0
    for: 1m
    labels:
      severity: critical
    annotations:
      summary: "AgentFlow server is down"
```

---

## 7. 附录

### 7.1 术语表

| 术语 | 英文 | 定义 |
|------|------|------|
| **Agent** | Agent | 系统执行主体，管理多个 Workflow 和共享资源 |
| **Workflow** | Workflow | DAG 结构的业务流程定义，由 Node 和 Edge 组成 |
| **Node** | Node | Workflow 中的执行单元，包含控制面和运行面 |
| **Edge** | Edge | Node 之间的依赖关系和数据流转定义 |
| **Actor** | Actor | 最小执行单元，基于消息驱动的并发模型 |
| **DSL** | Domain Specific Language | 领域特定语言，用于编排 Workflow |
| **DataBus** | Data Bus | 高速数据传输通道，支持零拷贝共享 |
| **Hot Reload** | Hot Reload | 运行时动态更新配置和代码 |
| **DAG** | Directed Acyclic Graph | 有向无环图，Workflow 的结构模型 |

### 7.2 参考资料

1. **Protoactor-cpp**: https://github.com/DanielKernel/Protoactor-cpp
2. **gRPC Documentation**: https://grpc.io/docs/
3. **libevent**: https://libevent.org/
4. **Protocol Buffers**: https://developers.google.com/protocol-buffers
5. **Kubernetes Documentation**: https://kubernetes.io/docs/
6. **Prometheus**: https://prometheus.io/docs/

### 7.3 版本历史

| 版本 | 日期 | 作者 | 变更说明 |
|------|------|------|----------|
| 1.0 | 2026-02-17 | AgentFlow Team | 初始版本 |

---

*本文档由 AgentFlow 团队维护，如有问题请提交 Issue 或 Pull Request。*
