# AgentFlow 项目规划

> 本文档整合了 AgentFlow 项目的功能实现状态、开发规划、优先级和代码完备性评估。

**最后更新**: 2026年2月18日

---

## 目录

1. [功能实现状态概览](#功能实现状态概览)
2. [已完成功能详情](#已完成功能详情)
3. [待实现功能（按优先级）](#待实现功能按优先级)
4. [近期开发重点](#近期开发重点)
5. [版本规划](#版本规划)
6. [代码完备性评估](#代码完备性评估)
7. [代码质量目标](#代码质量目标)
8. [文档目标](#文档目标)
9. [构建和部署目标](#构建和部署目标)
10. [更新日志](#更新日志)

---

## 功能实现状态概览

| 功能模块 | 完成度 | 状态说明 |
|---------|--------|---------|
| 开发编排框架 | 85% | Agent、Workflow、Node、Edge 定义完成，存储功能已实现 |
| 运行框架 | **88%** | 核心调度完成，控制流完成，P0/P1 缺陷已修复 |
| 基础设施 | **58%** | 适配器完成，libevent 集成完成，序列化已实现 |
| 监控和日志 | **68%** | 基础监控完成，跨平台实现完成 |

**进度提升**:
- 运行框架：75% -> 88% (+13%)
- 基础设施：48% -> 58% (+10%)
- 监控和日志：58% -> 68% (+10%)

---

## 已完成功能详情

### 第一阶段：开发编排框架（85% 完成）

#### Agent 编排
- 已完成 Agent 对象定义
- 已完成 Agent 生命周期策略（PER_REQUEST、FACTORY、SINGLETON）
- 已完成 Agent 版本管理基础字段
- 待完成 Agent 资源访问权限管理
- 待完成 Agent 版本管理逻辑（版本比较、兼容性检查）
- 待完成 Agent 灰度发布配置逻辑

#### Workflow 编排
- 已完成 完整的 DSL 解析实现（WorkflowParser）
- 已完成 DSL 验证功能
- 已完成 Workflow 版本管理基础字段
- 已完成 灰度发布支持
- 已完成 版本回滚机制
- 待完成 Workflow 版本管理逻辑（版本比较、兼容性检查）

#### Node 实现
- 已完成 Node 控制面结构
- 已完成 Node 输入输出接口定义
- 已完成 Node 错误处理策略（重试、超时）
- 已完成 Node 资源限制
- 已完成 控制面/运行面分离架构
- 已完成 多语言 Node 支持（Shell、Python、SO、JAR）
- 已完成 C++ 函数执行（cpp_function 回调 + CodeLoader 回退）
- 已完成 运行面 DSL 配置

#### Edge 定义
- 已完成 依赖关系定义
- 已完成 数据类型定义
- 已完成 数据传递方式（DataTransferMode）
- 已完成 控制流事件定义
- 已完成 Edge 条件表达式支持
- 已完成 Edge 条件评估实现

#### 持久化
- 已完成 本地存储 Agent 定义
- 已完成 本地存储 Workflow 定义
- 已完成 版本管理和变更历史
- 已完成 灰度发布配置存储
- 待完成 CI/CD 流程集成接口
- 待完成 远程存储支持（S3、HDFS）

---

### 第二阶段：运行框架（88% 完成）

#### Agent 运行管理
- 已完成 Agent 实例定义
- 已完成 Agent 生命周期和状态管理
- 已完成 Agent 内部资源管理
- 已完成 Agent 实例策略
- 已完成 微服务层接口封装（内部函数）
- 待完成 Agent 对外统一访问接口（依赖微服务框架）
- 待完成 多种协议支持（依赖微服务框架）

#### Workflow 运行和 Node 调度
- 已完成 DSL 动态加载和解析
- 已完成 Workflow 实例创建
- 已完成 Workflow 动态加载、卸载和版本更新接口
- 已完成 DAG 调度算法（Kahn 算法拓扑排序）
- 已完成 并行执行互不依赖的 Node
- 已完成 Node 执行状态管理
- 已完成 Node 输入输出数据处理
- 已完成 Node 生命周期管理
- 已完成 复杂流程控制（条件、循环、异常处理）
  - FOR_EACH 循环实现
  - 条件表达式解析缓存
- 已完成 复杂流程控制与 Workflow 调度集成
  - on_node_complete 调度闭环修复
- 待完成 Workflow 动态更新无需重新创建实例

#### AgentRuntime - 核心分层调度
- 已完成 Agent 层 Master 实现
- 已完成 Agent 生命周期管理、状态监控、资源分配
- 已完成 故障转移基础
- 已完成 Workflow 层 Master 实现
- 已完成 Workflow 依赖关系计算
- 已完成 Workflow 调度决策和状态管理
- 已完成 Workflow 动态加载、卸载和版本更新
- 已完成 Node 层 Worker 池
- 已完成 Node 多线程并发执行
- 已完成 动态负载均衡
- 已完成 线程安全（std::atomic 替换 std::rand()）
- 已完成 优雅关闭（drain 模式 + 5s 超时）

#### 系统通信机制
- 已完成 Agent 间通信基础
- 已完成 微服务层接口封装
- 已完成 Request/Response 序列化
  - to_json/from_json 实现
  - to_binary/from_binary 实现
- 已完成 高速数据总线（InProcess）
- 已完成 ZeroCopyDataBus 订阅通知
- 待完成 零拷贝跨进程（POSIX/SystemV，P2 规划）
- 待完成 分布式 Agent 间通信

#### 监控和日志
- 已完成 基础监控接口
- 已完成 性能指标收集
  - 跨平台实现（Linux + macOS）
  - Mach API 实现（macOS）
- 已完成 日志系统
- 已完成 调试信息输出
- 待完成 分布式监控
- 待完成 第三方监控系统对接（Prometheus、Grafana）

#### 分布式执行支持
- 待完成 多 Agent 实例协同
- 待完成 负载均衡策略
- 待完成 容错和故障转移
- 待完成 分布式事务支持

---

### 第三阶段：基础设施（58% 完成）

#### Actor 机制
- 已完成 Actor 系统基础框架
- 已完成 Protoactor-cpp 适配实现
- 已完成 消息序列化和反序列化
- 已完成 Ask 模式实现
- 已完成 本地 Actor 通信
- 待完成 远程 Actor 通信
- 待完成 Actor 监督策略

#### 网络适配器
- 已完成 网络接口定义
- 已完成 私有协议实现
- 已完成 SSL 适配器接口
- 已完成 WebSocket 适配器接口
- 待完成 服务器/客户端完整实现
- 待完成 负载均衡支持
- 待完成 连接池管理

#### 事件循环适配器
- 已完成 EventLoop 接口定义
- 已完成 libevent 完整集成
- 已完成 定时器支持
- 已完成 文件描述符事件
- 待完成 其他事件库适配（libuv、libev）

#### 序列化适配器
- 已完成 序列化接口定义
- 已完成 Protobuf 适配器接口
- 待完成 Protobuf 完整实现
- 待完成 JSON 序列化优化
- 待完成 其他序列化格式支持

#### JSON 适配器
- 已完成 JSON 适配器接口定义
- 已完成 RapidJSON 适配器实现
- 已完成 Stub 实现（无依赖回退）
- 已完成 WorkflowParser JSON 支持

---

### 第四阶段：可选独立组件

#### DSL 工具项目
- 待完成 DSL 编辑器
- 待完成 DSL 可视化工具
- 待完成 DSL 语法高亮支持
- 待完成 DSL 实时验证

> 注意：DSL 解析和验证功能已在 AgentFlow 中实现（WorkflowParser）

#### 监控日志收集器
- 待完成 监控数据采集增强
- 待完成 日志数据汇聚
- 待完成 第三方运维系统对接
- 待完成 监控仪表板（Dashboard）
- 待完成 告警规则配置
- 待完成 告警通知机制

#### 后端服务
- 待完成 文件存储服务集成
- 待完成 大语言模型推理服务集成
- 待完成 其他第三方 Agent 或工具服务集成

---

## 待实现功能（按优先级）

### P0 - 核心功能（已全部完成）

所有 P0 缺陷已在 2026-02-18 修复：
- AgentRuntime 调度闭环
- 线程安全（atomic worker ID）
- C++ 函数执行
- FOR_EACH 循环
- 性能指标跨平台
- Request/Response 序列化

### P1 - 高优先级（已全部完成）

所有 P1 功能已在 2026-02-18 实现：
- ZeroCopyDataBus 订阅通知
- 条件表达式解析缓存
- 优雅关闭机制
- 本地数据通道

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

## 近期开发重点

### 当前阶段（已完成）
- P0 缺陷修复（6/6）
- P1 功能补全（6/6）
- 跨平台构建支持
- 文档重组和刷新
- 代码结构重组

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

## 版本规划

### v0.1.0（当前版本）
- 核心框架实现
- 基础功能完成
- P0/P1 缺陷修复
- 跨平台支持

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

## 代码完备性评估

> 版本: 1.0
> 评估日期: 2026-02-17
> 评估方法: 对照功能清单，逐一审查源码实现

### 构建状态

| 项目 | 状态 | 说明 |
|------|------|------|
| 编译 | 通过 | 38个源文件全部编译成功 |
| 链接 | 通过 | agentflow 库和 8 个示例程序链接成功 |
| 测试运行 | 失败 | macOS dyld 找不到 libgtest_main.1.11.0.dylib（RPATH 问题） |
| 遗漏文件 | 警告 | `src/performance_metrics.cpp` 未加入 CMakeLists.txt |

### 文档与代码一致性审查

以下功能在文档中标记为已完成，但代码实际为 stub 或不完备：

#### 严重不一致（核心功能不可用）

**ZeroCopyDataBus POSIX/SystemV 共享内存**
- **文档声称**: "本地零拷贝共享具体实现 - 已实现 ZeroCopyDataBus，支持共享内存和读写锁"
- **代码现状**: `src/runtime/zero_copy_databus.cpp` 中：
  - `SharedMemoryFactory::create_posix_shared()` -> 返回 nullptr
  - `SharedMemoryFactory::create_systemv_shared()` -> 返回 nullptr
  - 仅 `create_in_process()` (堆内存模拟) 可用，不是真正的共享内存
- **影响**: 跨进程零拷贝数据共享完全不可用
- **修复建议**: 使用 `shm_open()/mmap()` 实现 POSIX 共享内存块，或明确文档标注为 "仅进程内可用"

#### 中等不一致（功能部分缺失）

**Request/Response 反序列化**
- **文件**: `src/network/request.cpp`, `src/network/response.cpp`
- **问题**: `from_json()` 和 `from_binary()` 方法为空 stub
- **影响**: 无法从 JSON/Binary 格式恢复 Request/Response 对象
- **状态**: 已修复

**AgentRuntime 节点调度闭环断裂**
- **文件**: `src/agent_runtime.cpp` 中 `on_node_complete()`
- **问题**: 方法获取了后续可执行节点列表，但没有将其入队到 worker 线程
- **影响**: DAG 执行在第一批节点完成后停止
- **状态**: 已修复

**ZeroCopyDataBus 订阅通知未触发**
- **文件**: `src/runtime/zero_copy_databus.cpp`
- **问题**: `subscribe()` 注册了回调，但 `put()` 方法中从未调用 `notify_subscribers()`
- **影响**: 数据变更订阅机制完全无效
- **状态**: 已修复

**ControlFlow FOR_EACH 循环**
- **文件**: `src/runtime/control_flow.cpp`
- **问题**: `FOR_EACH_LOOP` 分支直接 `return false`
- **影响**: FOR_EACH 循环类型完全不可用
- **状态**: 已修复

**跨进程消息序列化**
- **文件**: `src/actor/message_serializer.cpp`
- **问题**: 仅 RAW 格式可用，跨进程序列化路径为 stub
- **影响**: Actor 消息无法跨进程传递
- **状态**: 待实现（P2）

**StorageManager 工厂函数未定义**
- **文件**: `src/storage/storage_manager.cpp`
- **问题**: `create_agent_storage()` 和 `create_workflow_storage()` 工厂函数体未实现
- **影响**: StorageManager 初始化时无法创建实际的存储实例
- **状态**: 待实现

### 代码 Bug 清单

#### BUG-001: std::rand() 线程安全问题
- **文件**: `src/agent_runtime.cpp`
- **严重度**: 中
- **描述**: `get_least_loaded_worker()` 中使用 `std::rand()`，该函数非线程安全
- **状态**: 已修复 - 使用 std::atomic<int> 替换

#### BUG-002: execute_with_timeout lambda 引用悬空
- **文件**: `src/node.cpp` 中 `execute_with_timeout()`
- **严重度**: 中
- **描述**: lambda 捕获 `executor` 引用，如果超时后线程仍在运行，executor 生命周期结束后引用悬空
- **状态**: 已修复

#### BUG-003: on_node_complete 调度闭环断裂
- **文件**: `src/agent_runtime.cpp` 中 `on_node_complete()`
- **严重度**: 中（功能性 Bug）
- **描述**: 获取后续可执行节点后未入队调度
- **状态**: 已修复

### 测试覆盖度评估

#### 当前测试统计

| 测试文件 | 用例数 | 覆盖组件 | 质量 |
|---------|--------|---------|------|
| test_eventloop_gtest.cpp | 36 | EventLoopAdapter 完整生命周期 | 完善 |
| test_request_response_gtest.cpp | 56 | Request/Response 构建、序列化、验证 | 完善 |
| test_local_channel_gtest.cpp | 43 | LocalChannel 完整功能 | 完善 |
| test_hot_reload.cpp | 44 | 多语言热重载、版本管理 | 完善 |
| test_workflow_json_gtest.cpp | 29 | Workflow JSON 解析 | 完善 |
| test_agent_json_gtest.cpp | 27 | Agent JSON 解析 | 完善 |
| test_adapter_isolation.cpp | 6 | Actor 适配器隔离 | 基本 |
| test_node_gtest.cpp | 5 | Node 基本创建和状态 | 基础 |
| test_agent_gtest.cpp | 3 | Agent 创建 | stub |
| test_workflow_gtest.cpp | 3 | Workflow 创建 | stub |
| **合计** | **258** | | |

#### 缺失测试（按优先级排序）

**必须补充（P0）**

| 组件 | 需要测试的内容 |
|------|--------------|
| **AgentRuntime** | 三层调度、workflow 执行、node 并发调度、负载均衡 |
| **Workflow 执行** | DAG 拓扑排序、get_executable_nodes、mark_node_executed、循环/条件 |
| **Agent 生命周期** | 状态机转换（CREATED->INITIALIZED->RUNNING->PAUSED->STOPPED） |
| **ControlFlowManager** | 条件评估、循环控制、错误处理策略 |
| **DataBus** | put/get/remove、订阅回调、线程安全、容量限制 |

### 代码优化建议

#### 架构层面优化

**OPT-A1: Worker 池按语言分组**
- **当前问题**: 所有 Node（C++/Python/Shell）共享同一个 worker 线程池
- **建议**: 按 ExecutionMode 分组，C++ 节点使用高优先级池，脚本节点使用隔离池
- **优先级**: P2

**OPT-A2: 优雅关闭机制**
- **当前问题**: `AgentRuntime::stop()` 直接标记 `running_ = false`，不等待进行中的 workflow 完成
- **建议**: 实现 drain 模式：停止接受新任务 -> 等待进行中任务完成 -> 关闭 worker 线程
- **优先级**: P1
- **状态**: 已完成

**OPT-A3: Workflow 热替换**
- **当前问题**: `Workflow::update_dynamic()` 仅调用 `load_from_dsl()`，需要重建实例
- **建议**: 支持原地更新 DAG 定义，保持执行状态
- **优先级**: P2

#### 性能层面优化

**OPT-P1: 线程安全随机数**
- **当前问题**: `std::rand()` 非线程安全
- **建议**: 使用 `thread_local std::mt19937`
- **优先级**: P0
- **状态**: 已完成

**OPT-P2: 条件表达式解析缓存**
- **当前问题**: `Workflow::parse_condition_expression()` 每次评估都重新解析条件字符串
- **建议**: 首次解析后缓存 `ConditionExpression` 对象
- **优先级**: P1
- **状态**: 已完成

**OPT-P3: ZeroCopyDataBus 分段锁**
- **当前问题**: InProcessSharedMemoryBlock 使用单一 `shared_mutex`，高并发下写操作会阻塞所有读操作
- **建议**: 按 key 的 hash 分段，每段独立锁，降低锁竞争
- **优先级**: P2

**OPT-P4: DataBus 容量预分配**
- **当前问题**: `HighSpeedDataBus` 的 `std::unordered_map` 在数据量增长时频繁 rehash
- **建议**: 构造时根据 `max_capacity` 预留 bucket 数量 (`reserve()`)
- **优先级**: P3

### 优化实施路线

#### 第一阶段：P0 缺陷修复（已全部完成）

| 编号 | 任务 | 涉及文件 | 状态 |
|------|------|---------|------|
| OPT-B1 | performance_metrics.cpp 加入编译 | CMakeLists.txt | 已完成 |
| OPT-B2 | 修复 gtest RPATH | CMakeLists.txt | 已完成 |
| OPT-P1 | std::rand() -> std::atomic<int> | agent_runtime.cpp | 已完成 |
| OPT-R1 | on_node_complete 调度闭环 | agent_runtime.cpp | 已完成 |
| BUG-002 | lambda 引用悬空修复 | node.cpp | 已完成 |

#### 第二阶段：P1 功能补全（已全部完成）

| 编号 | 任务 | 涉及文件 | 状态 |
|------|------|---------|------|
| OPT-R5 | execute_cpp_function 真实实现 | node.cpp, node.h | 已完成 (cpp_function 回调 + CodeLoader 回退) |
| OPT-R3 | ZeroCopyDataBus 订阅通知 | zero_copy_databus.cpp | 已完成 (put()后调用notify_subscribers) |
| OPT-R4 | Request/Response 反序列化 | request.cpp, response.cpp | 已完成 (from_json + from_binary) |
| OPT-R2 | FOR_EACH 循环实现 | control_flow.cpp | 已完成 (逗号分隔集合迭代) |
| OPT-P2 | 条件表达式解析缓存 | workflow.cpp, workflow.h | 已完成 (unordered_map缓存) |
| OPT-A2 | 优雅关闭机制 | agent_runtime.cpp, agent_runtime.h | 已完成 (drain模式+超时) |
| 测试 | 修复 NodeLifecycle 测试 | test_node_gtest.cpp | 已完成 |

#### 第三阶段：P2 性能与架构优化（建议中期规划）

| 编号 | 任务 | 涉及文件 | 工作量 |
|------|------|---------|--------|
| 2.1.1 | POSIX 共享内存真实实现 | zero_copy_databus.cpp | 大 |
| OPT-A1 | Worker 池按语言分组 | agent_runtime.cpp | 大 |
| OPT-A3 | Workflow 热替换 | workflow.cpp | 大 |
| OPT-P3 | ZeroCopyDataBus 分段锁 | zero_copy_databus.cpp | 中 |
| 2.2.5 | 跨进程消息序列化 | message_serializer.cpp | 大 |
| 2.2.6 | StorageManager 工厂函数 | storage_manager.cpp | 中 |

---

## 代码质量目标

### 测试覆盖
- 已完成 单元测试框架（GTest）
- 已完成 集成测试（6/10 通过）
- 已完成 性能测试框架
- 待完成 代码覆盖率 > 80%
- 待完成 所有核心模块单元测试
- 待完成 端到端测试套件

### 代码规范
- 已完成 C++17 标准
- 已完成 Google C++ 风格指南
- 已完成 clang-format 配置
- 已完成 clang-tidy 静态分析
- 已完成 Cppcheck 配置
- 待完成 代码审查流程
- 待完成 贡献者指南完善

### CI/CD
- 已完成 GitHub Actions 工作流
- 已完成 多平台构建（Linux/macOS AMD64/ARM64）
- 已完成 自动化测试
- 已完成 代码质量检查
- 已完成 自动化发布
- 待完成 Docker 镜像构建
- 待完成 性能基准测试自动化

---

## 文档目标

### 用户文档
- 已完成 README.md（项目概览）
- 已完成 快速入门指南
- 已完成 API 参考手册
- 已完成 架构设计文档
- 待完成 用户手册（完整版）
- 待完成 最佳实践指南
- 待完成 故障排查手册

### 开发者文档
- 已完成 开发指南
- 已完成 贡献指南
- 已完成 测试指南
- 已完成 CI/CD 指南
- 待完成 设计模式文档
- 待完成 性能优化指南
- 待完成 插件开发指南

### 示例和教程
- 已完成 基础示例（hello_world 等）
- 待完成 进阶示例
- 待完成 分布式部署示例
- 待完成 视频教程
- 待完成 在线演示环境

---

## 构建和部署目标

### 构建系统
- 已完成 CMake 构建系统
- 已完成 跨平台支持（Linux/macOS）
- 已完成 依赖管理
- 已完成 构建脚本（build.sh）
- 待完成 Windows 支持
- 待完成 交叉编译支持
- 待完成 构建缓存优化

### 部署方式
- 已完成 本地部署
- 待完成 Docker 容器化
- 待完成 Kubernetes 部署
- 待完成 云平台支持（AWS、Azure、GCP）
- 待完成 一键部署脚本

### 发布管理
- 已完成 版本号管理
- 已完成 GitHub Release
- 已完成 变更日志
- 待完成 软件包管理（apt、yum、brew）
- 待完成 二进制分发优化

---

## 更新日志

| 日期 | 变更 |
|------|------|
| 2026-02-18 | 合并 roadmap.md 和 code-review.md 到 planning.md，整合项目规划文档 |
| 2026-02-17 | 代码完备性评估：修正功能状态标注，标记 stub/不完备功能 |
| 2026-02-17 | 整理功能规划文档，更新至 docs 目录 |
| 2026-02-17 | 完成 P1 功能：Protoactor-cpp 适配、本地数据通道、微服务接口封装、libevent 完整集成 |
| 2026-02-16 | 重新梳理功能实现状态，更新优先级排序 |

---

## 相关文档

- [架构设计](architecture.md) - 系统5视图架构
- [开发指南](development.md) - 开发和构建说明
- [测试指南](testing.md) - 测试方法
- [贡献指南](contributing.md) - 贡献流程

---

**项目维护者**: @DanielKernel
