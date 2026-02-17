# AgentFlow 项目路线图

> 本文档整合了 AgentFlow 项目的功能实现状态、开发规划和优先级。

**最后更新**: 2026年2月18日  
**相关文档**:
- [planning.md](planning.md) - 详细功能实现状态
- [code-review.md](code-review.md) - 代码完备性评估（P0/P1 ✅ 已完成）
- [architecture.md](architecture.md) - 系统架构设计

---

## 📊 功能实现状态概览

| 功能模块 | 完成度 | 状态说明 |
|---------|--------|---------|
| 开发编排框架 | 85% | Agent、Workflow、Node、Edge 定义完成，存储功能已实现 |
| 运行框架 | **88%** | 核心调度完成 ✅，控制流完成 ✅，P0/P1 缺陷已修复 ✅ |
| 基础设施 | **58%** | 适配器完成，libevent 集成完成，序列化已实现 ✅ |
| 监控和日志 | **68%** | 基础监控完成 ✅，跨平台实现完成 ✅ |

**进度提升**:
- 运行框架：75% → 88% (+13%)
- 基础设施：48% → 58% (+10%)
- 监控和日志：58% → 68% (+10%)

---

## ✅ 已完成功能

### 第一阶段：开发编排框架（85% 完成）

#### Agent 编排
- ✅ Agent 对象定义
- ✅ Agent 生命周期策略（PER_REQUEST、FACTORY、SINGLETON）
- ✅ Agent 版本管理基础字段
- ⏳ Agent 资源访问权限管理
- ⏳ Agent 版本管理逻辑（版本比较、兼容性检查）
- ⏳ Agent 灰度发布配置逻辑

#### Workflow 编排
- ✅ 完整的 DSL 解析实现（WorkflowParser）
- ✅ DSL 验证功能
- ✅ Workflow 版本管理基础字段
- ✅ 灰度发布支持
- ✅ 版本回滚机制
- ⏳ Workflow 版本管理逻辑（版本比较、兼容性检查）

#### Node 实现
- ✅ Node 控制面结构
- ✅ Node 输入输出接口定义
- ✅ Node 错误处理策略（重试、超时）
- ✅ Node 资源限制
- ✅ 控制面/运行面分离架构
- ✅ **多语言 Node 支持**（Shell、Python、SO、JAR）
- ✅ **C++ 函数执行**（cpp_function 回调 + CodeLoader 回退）
- ✅ 运行面 DSL 配置

#### Edge 定义
- ✅ 依赖关系定义
- ✅ 数据类型定义
- ✅ 数据传递方式（DataTransferMode）
- ✅ 控制流事件定义
- ✅ Edge 条件表达式支持
- ✅ Edge 条件评估实现

#### 持久化
- ✅ 本地存储 Agent 定义
- ✅ 本地存储 Workflow 定义
- ✅ 版本管理和变更历史
- ✅ 灰度发布配置存储
- ⏳ CI/CD 流程集成接口
- ⏳ 远程存储支持（S3、HDFS）

---

### 第二阶段：运行框架（88% 完成）

#### Agent 运行管理
- ✅ Agent 实例定义
- ✅ Agent 生命周期和状态管理
- ✅ Agent 内部资源管理
- ✅ Agent 实例策略
- ✅ 微服务层接口封装（内部函数）
- ⏳ Agent 对外统一访问接口（依赖微服务框架）
- ⏳ 多种协议支持（依赖微服务框架）

#### Workflow 运行和 Node 调度
- ✅ DSL 动态加载和解析
- ✅ Workflow 实例创建
- ✅ Workflow 动态加载、卸载和版本更新接口
- ✅ DAG 调度算法（Kahn 算法拓扑排序）
- ✅ 并行执行互不依赖的 Node
- ✅ Node 执行状态管理
- ✅ Node 输入输出数据处理
- ✅ Node 生命周期管理
- ✅ **复杂流程控制**（条件、循环、异常处理）
  - ✅ FOR_EACH 循环实现
  - ✅ 条件表达式解析缓存
- ✅ **复杂流程控制与 Workflow 调度集成**
  - ✅ on_node_complete 调度闭环修复
- ⏳ Workflow 动态更新无需重新创建实例

#### AgentRuntime - 核心分层调度
- ✅ Agent 层 Master 实现
- ✅ Agent 生命周期管理、状态监控、资源分配
- ✅ 故障转移基础
- ✅ Workflow 层 Master 实现
- ✅ Workflow 依赖关系计算
- ✅ Workflow 调度决策和状态管理
- ✅ Workflow 动态加载、卸载和版本更新
- ✅ Node 层 Worker 池
- ✅ Node 多线程并发执行
- ✅ 动态负载均衡
- ✅ **线程安全**（std::atomic 替换 std::rand()）
- ✅ **优雅关闭**（drain 模式 + 5s 超时）

#### 系统通信机制
- ✅ Agent 间通信基础
- ✅ 微服务层接口封装
- ✅ **Request/Response 序列化**
  - ✅ to_json/from_json 实现
  - ✅ to_binary/from_binary 实现
- ✅ 高速数据总线（InProcess）
- ✅ **ZeroCopyDataBus 订阅通知**
- ⏳ 零拷贝跨进程（POSIX/SystemV，P2 规划）
- ⏳ 分布式 Agent 间通信

#### 监控和日志
- ✅ 基础监控接口
- ✅ **性能指标收集**
  - ✅ 跨平台实现（Linux + macOS）
  - ✅ Mach API 实现（macOS）
- ✅ 日志系统
- ✅ 调试信息输出
- ⏳ 分布式监控
- ⏳ 第三方监控系统对接（Prometheus、Grafana）

#### 分布式执行支持
- ⏳ 多 Agent 实例协同
- ⏳ 负载均衡策略
- ⏳ 容错和故障转移
- ⏳ 分布式事务支持

---

### 第三阶段：基础设施（58% 完成）

#### Actor 机制
- ✅ Actor 系统基础框架
- ✅ Protoactor-cpp 适配实现
- ✅ 消息序列化和反序列化
- ✅ Ask 模式实现
- ✅ 本地 Actor 通信
- ⏳ 远程 Actor 通信
- ⏳ Actor 监督策略

#### 网络适配器
- ✅ 网络接口定义
- ✅ 私有协议实现
- ✅ SSL 适配器接口
- ✅ WebSocket 适配器接口
- ⏳ 服务器/客户端完整实现
- ⏳ 负载均衡支持
- ⏳ 连接池管理

#### 事件循环适配器
- ✅ EventLoop 接口定义
- ✅ **libevent 完整集成**
- ✅ 定时器支持
- ✅ 文件描述符事件
- ⏳ 其他事件库适配（libuv、libev）

#### 序列化适配器
- ✅ 序列化接口定义
- ✅ Protobuf 适配器接口
- ⏳ Protobuf 完整实现
- ⏳ JSON 序列化优化
- ⏳ 其他序列化格式支持

#### JSON 适配器
- ✅ JSON 适配器接口定义
- ✅ RapidJSON 适配器实现
- ✅ Stub 实现（无依赖回退）
- ✅ WorkflowParser JSON 支持

---

## 📋 待实现功能（按优先级）

### P0 - 核心功能 ✅ 已全部完成

所有 P0 缺陷已在 2026-02-18 修复：
- ✅ AgentRuntime 调度闭环
- ✅ 线程安全（atomic worker ID）
- ✅ C++ 函数执行
- ✅ FOR_EACH 循环
- ✅ 性能指标跨平台
- ✅ Request/Response 序列化

### P1 - 高优先级 ✅ 已全部完成

所有 P1 功能已在 2026-02-18 实现：
- ✅ ZeroCopyDataBus 订阅通知
- ✅ 条件表达式解析缓存
- ✅ 优雅关闭机制
- ✅ 本地数据通道

### P2 - 中优先级（下一步规划）

#### 性能优化
1. **零拷贝共享内存**
   - POSIX 共享内存真实实现
   - SystemV 共享内存支持
   - 跨进程数据总线

2. **调度器优化**
   - Worker 池按语言分组
   - 动态 Worker 池扩缩容
   - 智能负载均衡

3. **Workflow 优化**
   - Workflow 热替换
   - 动态更新无需重启
   - ZeroCopyDataBus 分段锁

#### 分布式支持
4. **跨进程通信**
   - 消息序列化完整实现
   - 远程 Actor 通信
   - 分布式调度

5. **高可用**
   - 故障检测和自动恢复
   - 状态复制和同步
   - 分布式协调（Raft/Paxos）

### P3 - 低优先级（长期规划）

#### 开发工具
1. **DSL 工具链**
   - 可视化编排工具
   - DSL 语法检查器
   - DSL 到 JSON 转换器

2. **调试工具**
   - Workflow 执行追踪
   - 可视化监控面板
   - 性能分析工具

#### 生态集成
3. **云原生支持**
   - Kubernetes 集成
   - Docker 容器化
   - Helm Chart

4. **第三方集成**
   - Prometheus 监控
   - Grafana 可视化
   - ELK 日志收集

### P4 - 外部依赖相关

1. **可选依赖完整实现**
   - Protoactor-cpp 远程通信
   - Protobuf 完整序列化
   - OpenSSL 完整 SSL/TLS
   - WebSocket 完整实现

2. **新依赖引入评估**
   - gRPC 支持
   - HTTP/2 支持
   - MessagePack 序列化

---

## 🎯 近期开发重点

### 当前阶段（已完成）
- ✅ P0 缺陷修复（6/6）
- ✅ P1 功能补全（6/6）
- ✅ 跨平台构建支持
- ✅ 文档重组和刷新
- ✅ 代码结构重组

### 下一阶段（P2 性能优化）
1. **POSIX 共享内存实现**
   - 工作量：大
   - 优先级：高
   - 预计时间：2-3周

2. **Worker 池按语言分组**
   - 工作量：大
   - 优先级：高
   - 预计时间：1-2周

3. **Workflow 热替换**
   - 工作量：大
   - 优先级：中
   - 预计时间：2-3周

4. **修复预存在测试失败**
   - AgentFlowGoogleTests SEGFAULT
   - HotReloadTests 时序问题
   - EventLoopTests SEGFAULT
   - 工作量：中
   - 优先级：高

---

## 📈 代码质量目标

### 测试覆盖
- ✅ 单元测试框架（GTest）
- ✅ 集成测试（6/10 通过）
- ✅ 性能测试框架
- ⏳ 代码覆盖率 > 80%
- ⏳ 所有核心模块单元测试
- ⏳ 端到端测试套件

### 代码规范
- ✅ C++17 标准
- ✅ Google C++ 风格指南
- ✅ clang-format 配置
- ✅ clang-tidy 静态分析
- ✅ Cppcheck 配置
- ⏳ 代码审查流程
- ⏳ 贡献者指南完善

### CI/CD
- ✅ GitHub Actions 工作流
- ✅ 多平台构建（Linux/macOS AMD64/ARM64）
- ✅ 自动化测试
- ✅ 代码质量检查
- ✅ 自动化发布
- ⏳ Docker 镜像构建
- ⏳ 性能基准测试自动化

---

## 📚 文档目标

### 用户文档
- ✅ README.md（项目概览）
- ✅ 快速入门指南
- ✅ API 参考手册
- ✅ 架构设计文档
- ⏳ 用户手册（完整版）
- ⏳ 最佳实践指南
- ⏳ 故障排查手册

### 开发者文档
- ✅ 开发指南
- ✅ 贡献指南
- ✅ 测试指南
- ✅ CI/CD 指南
- ✅ 代码审查指南
- ⏳ 设计模式文档
- ⏳ 性能优化指南
- ⏳ 插件开发指南

### 示例和教程
- ✅ 基础示例（hello_world 等）
- ⏳ 进阶示例
- ⏳ 分布式部署示例
- ⏳ 视频教程
- ⏳ 在线演示环境

---

## 🚀 构建和部署目标

### 构建系统
- ✅ CMake 构建系统
- ✅ 跨平台支持（Linux/macOS）
- ✅ 依赖管理
- ✅ 构建脚本（build.sh）
- ⏳ Windows 支持
- ⏳ 交叉编译支持
- ⏳ 构建缓存优化

### 部署方式
- ✅ 本地部署
- ⏳ Docker 容器化
- ⏳ Kubernetes 部署
- ⏳ 云平台支持（AWS、Azure、GCP）
- ⏳ 一键部署脚本

### 发布管理
- ✅ 版本号管理
- ✅ GitHub Release
- ✅ 变更日志
- ⏳ 软件包管理（apt、yum、brew）
- ⏳ 二进制分发优化

---

## 📅 版本规划

### v0.1.0（当前版本）
- ✅ 核心框架实现
- ✅ 基础功能完成
- ✅ P0/P1 缺陷修复
- ✅ 跨平台支持

### v0.2.0（计划中）
- 目标：P2 性能优化
- 预计发布：2026 Q2
- 主要特性：
  - POSIX 共享内存
  - Worker 池优化
  - Workflow 热替换
  - 测试失败修复

### v0.3.0（规划中）
- 目标：分布式支持
- 预计发布：2026 Q3
- 主要特性：
  - 远程 Actor 通信
  - 分布式调度
  - 高可用支持

### v1.0.0（远期目标）
- 目标：生产就绪
- 预计发布：2026 Q4
- 主要特性：
  - 完整功能集
  - 生产级稳定性
  - 完整文档和工具链
  - 云原生支持

---

## 🔗 相关链接

- **源码仓库**: https://github.com/DanielKernel/AgentFlow
- **文档**: [docs/](.)
- **问题追踪**: [GitHub Issues](https://github.com/DanielKernel/AgentFlow/issues)
- **贡献指南**: [contributing.md](contributing.md)

---

**维护者**: @DanielKernel  
**最后更新**: 2026年2月18日

