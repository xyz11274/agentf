# AgentFlow 路线图

> 本文档列出 AgentFlow 项目的功能实现状态和开发计划。

---

## 功能实现状态概览

| 功能模块 | 完成度 | 说明 |
|---------|--------|------|
| 开发编排框架 | 88% | Agent、Workflow、Node、Edge 定义基本完成 |
| 运行框架 | 85% | 核心调度和执行功能完成，零拷贝共享已实现 |
| 基础设施 | 50% | 适配器接口已定义，libevent 完整集成已完成 |
| 监控和日志 | 60% | 基础监控完成，第三方对接待实现 |

---

## 已完成功能 ✅

### 开发编排框架

#### Agent 编排
- [x] Agent 对象定义
- [x] Agent 生命周期策略配置（PER_REQUEST、FACTORY、SINGLETON）
- [x] Agent 版本管理基础字段

#### Workflow 编排
- [x] 完整的 DSL 解析实现（WorkflowParser）
- [x] DSL 验证功能
- [x] Workflow 版本管理基础字段
- [x] 灰度发布支持
- [x] 版本回滚机制

#### Node 实现
- [x] Node 控制面结构
- [x] Node 输入输出接口定义
- [x] Node 错误处理策略（重试、超时）
- [x] Node 资源限制
- [x] 控制面/运行面分离架构
- [x] 支持非 C++ 代码实现的 Node（Shell、Python、SO）
- [x] 运行面 DSL 配置

#### Edge 定义
- [x] 依赖关系定义
- [x] 数据类型定义
- [x] 数据传递方式
- [x] 控制流事件定义
- [x] Edge 条件表达式支持
- [x] Edge 条件评估实现

#### 持久化
- [x] 本地存储 Agent 定义
- [x] 本地存储 Workflow 定义
- [x] 版本管理和变更历史
- [x] 灰度发布配置存储

### 运行框架

#### Agent 运行管理
- [x] Agent 实例定义
- [x] Agent 生命周期和状态管理
- [x] Agent 内部资源管理
- [x] Agent 实例策略
- [x] 微服务层接口封装（内部函数）

#### Workflow 运行和 Node 调度
- [x] DSL 动态加载和解析
- [x] Workflow 实例创建
- [x] Workflow 动态加载、卸载和版本更新接口
- [x] DAG 调度算法（Kahn 算法拓扑排序）
- [x] 并行执行互不依赖的 Node
- [x] Node 执行状态管理
- [x] Node 输入输出数据处理
- [x] Node 生命周期管理
- [x] 复杂流程控制（条件、循环、异常处理）
- [x] 复杂流程控制与 Workflow 调度的集成

#### AgentRuntime - 核心分层调度
- [x] Agent 层 Master 实现
- [x] Agent 生命周期管理、状态监控、资源分配
- [x] 故障转移基础框架
- [x] Workflow 层 Master 实现
- [x] Workflow 依赖关系计算
- [x] Workflow 调度决策和状态管理
- [x] Node 层 Worker 池
- [x] Node 多线程并发执行
- [x] 动态负载均衡
- [x] 直接管理 Agent 内的共享资源
- [x] 统一协调三层调度单位

#### 系统通信机制
- [x] Node 间 Actor 消息接口定义
- [x] Actor 消息机制基础框架
- [x] 业务数据通信面实现（private protocol）
- [x] 本地零拷贝共享实现

#### 监控和日志
- [x] Workflow 执行全链路监控
- [x] Node 级别详细监控
- [x] Agent 状态监控
- [x] Node 执行状态监控
- [x] 性能指标采集（延迟、吞吐量）
- [x] 标准监控接口定义

### 基础设施

- [x] Actor 接口定义
- [x] Actor stub 实现
- [x] WebSocket 接口定义
- [x] WebSocket stub 实现
- [x] SSL/TLS 接口定义
- [x] SSL/TLS stub 实现
- [x] 事件循环接口定义
- [x] 事件循环 stub 实现
- [x] libevent 完整集成
- [x] 序列化接口定义
- [x] 序列化 stub 实现
- [x] JSON 接口定义
- [x] JSON stub 实现
- [x] JSON RapidJSON 实现

---

## 待实现功能 📋

### P1 - 高优先级

| 功能 | 说明 | 状态 |
|------|------|------|
| Node 控制面 Actor 实现 | 基于 Protoactor 实现 Node 控制面 | 暂缓 |
| WorkflowActor 协调逻辑 | Workflow 执行的 Actor 协调 | 暂缓 |

### P2 - 中优先级

| 功能 | 说明 |
|------|------|
| 分布式执行支持 | 执行代理模式、多节点协同工作 |
| 多 Master 实例去中心化架构 | 分布式协调机制 |
| Mesh 式分布式监控架构 | 跨实例监控数据汇聚 |
| 监控和日志增强 | 第三方系统对接、自定义指标导出 |
| 资源访问权限管理 | Agent 资源和 Workflow 资源权限控制 |
| 跨节点高速传输 | 分布式数据传输优化 |

### P3 - 低优先级

| 功能 | 说明 |
|------|------|
| 远程存储支持 | S3、HDFS 等云存储集成 |
| 可视化工具 | DSL 可视化、监控仪表板 |
| 高级运维功能 | 自动扩缩容、故障自愈 |
| 多语言 Worker 池分配 | 不同语言实现 Node 的智能分配 |
| CI/CD 流程集成接口 | 与 Jenkins、GitLab CI 等集成 |

### P4 - 外部依赖相关

| 依赖 | 说明 |
|------|------|
| Protoactor-cpp 完整集成 | 需要安装 Protoactor-cpp |
| WebSocket 完整实现 | 需要安装 libwebsockets |
| SSL/TLS 完整实现 | 需要安装 OpenSSL |
| Protobuf 完整实现 | 需要安装 Protobuf |

---

## 已完成阶段

### 第一阶段 ✅
- ControlFlowManager 与 Workflow 调度的集成
- 本地零拷贝共享（基于共享内存）
- 本地数据通道（支持 Agent/Workflow/Node 间数据共享）

### 第二阶段 ✅
- 微服务层接口封装（Request/Response）
- 完善第三方库集成（Protoactor-cpp、libevent）

---

## 代码质量目标

| 指标 | 目标 | 当前 |
|------|------|------|
| 单元测试覆盖率 | > 80% | ~60% |
| 集成测试 | 完整 | 部分 |
| 端到端测试 | 完整 | 部分 |
| 性能测试 | 完整 | 待实现 |
| 压力测试 | 完整 | 待实现 |
| 内存泄漏检测 | 通过 | 待检测 |
| 线程安全测试 | 通过 | 待测试 |

---

## 文档目标

| 文档 | 状态 |
|------|------|
| API 文档（Doxygen） | 待生成 |
| 架构设计文档 | ✅ 已完成 |
| 部署指南 | ✅ 已完成（架构文档中） |
| 性能调优指南 | 待编写 |
| 故障排查手册 | 待编写 |

---

## 构建和部署目标

| 目标 | 状态 |
|------|------|
| Docker 镜像构建 | ✅ 已完成 |
| Docker Compose 配置 | ✅ 已完成 |
| Kubernetes Helm Charts | ✅ 已完成 |
| CI/CD Pipeline 配置 | 待实现 |
| 跨平台编译支持 | ✅ 已完成 |
| 包管理器支持（deb、rpm） | 待实现 |

---

*最后更新: 2026-02-17*
