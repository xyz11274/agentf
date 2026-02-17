# AgentFlow 项目规划

本项目的完整规划和功能实现状态已整合到 `docs/` 目录。

**查看详细规划**：[docs/PLANNING.md](docs/PLANNING.md)

---

## 📊 快速概览

| 功能模块 | 完成度 | 说明 |
|---------|--------|------|
| 开发编排框架 | 85% | Agent、Workflow、Node、Edge定义基本完成，存储功能已实现，Edge条件支持已添加。DSL load_from_dsl为占位实现 |
| 运行框架 | 75% | 核心调度和执行功能完成，控制流集成完成，本地数据通道已实现。零拷贝跨进程不可用，C++函数执行为stub，调度闭环需补全 |
| 基础设施 | 48% | 适配器接口已定义，Protoactor-cpp适配已完成，libevent完整集成已完成。跨进程序列化为stub |
| 监控和日志 | 58% | 基础监控完成，分布式监控和第三方对接待实现。performance_metrics.cpp未加入编译 |

> ⚠️ 详细的代码完备性评估见 [docs/CODE_REVIEW.md](docs/CODE_REVIEW.md)

---

## 📖 文档导航

### 项目规划与功能
- [PLANNING.md](docs/PLANNING.md) - **完整的项目规划文档**
  - 功能实现状态（P0、P1、P2、P3、P4）
  - 各模块详细规划
  - 优先级排序
  - 更新日志

---

## 一、开发编排框架 - 待实现

### 1.1 Agent编排
- [x] Agent对象定义 - 已实现
- [x] Agent生命周期策略配置 - 已支持PER_REQUEST、FACTORY、SINGLETON
- [ ] Agent资源访问权限和管理策略配置
- [ ] 资源与workflow之间的共享关系定义
- [x] Agent版本管理基础字段 - 已定义version字段
- [ ] Agent版本管理逻辑（版本比较、兼容性检查）
- [ ] Agent灰度发布配置逻辑（除存储外的实际执行逻辑）

### 1.2 Workflow编排
- [x] 完整的DSL解析实现 - 已实现WorkflowParser，支持JSON解析和验证
- [x] DSL验证功能 - 已实现验证逻辑
- [x] Workflow版本管理基础字段 - 已定义version字段
- [ ] Workflow版本管理逻辑（版本比较、兼容性检查）
- [x] 灰度发布支持 - 已实现StorageManager中的灰度发布接口
- [x] 版本回滚机制 - 已实现存储层的版本回滚

### 1.3 Node实现规范
- [x] Node控制面结构 - 已定义基本属性
- [x] Node输入输出接口定义 - 已实现input_params、output_params
- [x] Node错误处理策略 - 已支持重试、超时配置
- [x] Node超时配置和资源限制 - 已实现timeout_ms、memory_limit_mb、cpu_limit_mb
- [x] 控制面/运行面分离架构 - 已定义ExecutionMode
- [x] 支持非C++代码实现的Node - 已支持SHELL_SCRIPT、PYTHON_SCRIPT、SO_LIBRARY等
  - ⚠️ **注意**: C++函数执行(execute_cpp_function)为stub，仅sleep+假输出
- [x] 运行面DSL配置指定运行模式 - 已支持execution_mode配置
- [ ] Node控制面Actor实际实现（暂时不需要实现）
- [ ] Node间Actor协作机制实现（暂时不需要实现）

### 1.4 Edge定义
- [x] 依赖关系定义 - 已实现source_node、target_node
- [x] 数据类型定义 - 已实现data_formats
- [x] 数据传递方式 - 已定义DataTransferMode
- [x] 控制流事件定义 - 已实现supported_events
- [x] Edge条件表达式支持 - 已实现condition_expression字段和解析逻辑
- [x] Edge条件评估实现 - 已集成到ControlFlowManager

### 1.5 持久化
- [x] 本地存储Agent定义实现 - 已实现AgentStorage
- [x] 本地存储Workflow定义实现 - 已实现WorkflowStorage
- [x] 版本管理和变更历史 - 已实现IVersionStorage
- [x] 灰度发布配置存储 - 已在StorageManager中实现
- [ ] CI/CD流程集成接口
- [ ] 远程存储支持（如S3、HDFS等）

---

## 二、运行框架 - 待实现

### 2.1 Agent运行管理
- [x] Agent实例定义 - 已实现
- [x] Agent生命周期和状态管理 - 已实现AgentState状态机
- [x] Agent内部资源管理 - 已实现数据总线、事件队列
- [x] Agent实例策略 - 已支持PER_REQUEST、FACTORY、SINGLETON
- [x] 微服务层接口封装（内部函数） - 已实现Request/Response封装器
  - ⚠️ **注意**: from_json()/from_binary()反序列化为stub
- [ ] Agent对外统一访问接口定义（依赖微服务框架）
- [ ] 多种协议支持（依赖微服务框架）

### 2.2 Workflow运行和Node调度
- [x] DSL动态加载和解析 - 已通过WorkflowParser实现
- [x] Workflow实例创建 - 已实现Workflow构造函数
- [x] Workflow动态加载、卸载和版本更新 - 接口已定义
- [x] DAG调度算法 - 已实现Kahn算法拓扑排序
- [x] 并行执行互不依赖的Node - 已实现get_executable_nodes
- [x] Node执行状态管理 - 已实现NodeState状态机
- [x] Node输入输出数据处理 - 已实现inputs_/outputs_
- [x] Node生命周期管理 - 已实现initialize/execute/cleanup
- [x] 复杂流程控制（条件、循环、异常处理） - 已实现ControlFlowManager
  - ⚠️ **注意**: FOR_EACH循环未实现（直接返回false）
- [ ] Workflow动态更新时无需重新创建实例的实现
- [x] 复杂流程控制与Workflow调度的集成 - 已实现get_executable_nodes_with_control等接口
  - ⚠️ **注意**: on_node_complete未将后续节点入队，调度闭环断裂

### 2.3 AgentRuntime - 核心分层调度
- [x] Agent层Master实现 - 已实现monitor_agents
- [x] Agent生命周期管理、状态监控、资源分配 - 基础功能已实现
- [x] 故障转移基础 - 已实现健康检查框架
- [x] Workflow层Master实现 - 已实现调度逻辑
- [x] Workflow依赖关系计算 - 已实现get_executable_nodes
- [x] Workflow调度决策和状态管理 - 已实现WorkflowExecutionState
- [x] Workflow动态加载、卸载和版本更新 - 接口已定义
- [x] Node层Worker池 - 已实现worker_threads_和node_task_queue_
- [x] Node多线程并发执行 - 已实现worker_thread_main
- [x] 动态负载均衡 - 已实现worker_load跟踪
- [ ] Worker动态分配和回收
- [ ] 多语言实现Node分配到不同Worker池
- [x] 直接管理Agent内的共享资源 - 已实现databuses_
- [x] 对三层调度单位的执行状态和数据流转进行统一协调 - 已实现workflow_states_
- [x] 支持多线程并发执行和动态负载均衡 - 已实现
- [ ] 多Master实例去中心化架构
- [ ] 分布式协调机制
- [ ] Master实例间调度决策一致性

### 2.4 系统通信机制
- [x] Node间Actor消息接口定义 - 已定义Actor相关接口
- [x] Actor消息机制基础框架 - 已实现ActorSystem
- [x] 业务数据通信面实现（private protocol） - 已实现协议定义、编解码、消息类型
- [x] 本地零拷贝共享具体实现 - 已实现ZeroCopyDataBus
  - ⚠️ **注意**: 仅InProcess(堆内存)可用；POSIX/SystemV共享内存返回nullptr；订阅通知未触发
- [ ] 跨节点高速传输
- [ ] 系统维护通信面完整实现
- [ ] gRPC协议支持
- [ ] RESTful协议支持
- [ ] WebSocket协议完整实现（当前为stub，依赖libwebsockets）
- [ ] Protoactor-cpp完整集成（当前为stub）

### 2.5 监控和日志
- [x] Workflow执行全链路监控 - 已支持workflow状态跟踪
- [x] Node级别详细监控 - 已支持Node执行状态监控
- [ ] mesh式分布式监控架构
- [x] Agent状态监控（生命周期、资源使用等） - 已实现基础监控
- [x] Node执行状态监控 - 已支持NodeState跟踪
- [x] 性能指标采集（延迟、吞吐量等） - 已实现HistogramMetric
- [x] 标准监控接口定义 - 已定义MetricsCollector和导出格式
- [ ] 第三方日志系统对接（如ELK、Loki等）
- [ ] 运维系统gRPC/RESTful接口对接
- [ ] 自定义监控指标导出（如Prometheus、OpenTelemetry等）
- [ ] 监控数据持久化

### 2.6 分布式执行支持
- [ ] 执行代理模式实现
- [ ] 多节点协同工作
- [ ] 远程Worker管理
- [ ] Worker动态分配和回收
- [ ] 多语言实现Node分配到不同Worker池
- [ ] 跨节点任务调度
- [ ] 分布式状态同步

---

## 三、基础设施 - 待实现

### 3.1 Actor机制
- [x] Actor接口定义 - 已定义ActorSystem相关接口
- [x] Actor stub实现 - 已实现stub_impl.cpp
- [ ] Protoactor-cpp完整集成（当前为stub，需要第三方库）
- [ ] Node控制面Actor实现
- [ ] Node间Actor协作机制
- [ ] Actor消息路由
- [ ] Actor生命周期管理（创建、启动、停止、销毁）

### 3.2 网络适配器
- [x] WebSocket接口定义 - 已定义IWebSocketAdapter
- [x] WebSocket stub实现 - 已实现
- [ ] WebSocket完整实现（当前为stub，需要libwebsockets）
- [x] SSL/TLS接口定义 - 已定义ISslAdapter
- [x] SSL/TLS stub实现 - 已实现
- [ ] SSL/TLS完整实现（当前为stub，需要OpenSSL）
- [ ] libwebsockets集成
- [ ] OpenSSL集成

### 3.3 事件循环适配器
- [x] 事件循环接口定义 - 已定义IEventLoopAdapter
- [x] 事件循环stub实现 - 已实现
- [x] libevent完整集成 - 已实现EventLoopLibevent完整功能
- [x] 事件循环完整实现 - 已实现dispatch、dispatch_once、exit
- [x] I/O多路复用集成 - 已实现file descriptor事件
- [x] 定时器支持 - 已实现单次和持久定时器
- [x] 异步I/O支持 - 已实现事件回调机制

### 3.4 序列化适配器
- [x] 序列化接口定义 - 已定义IProtoAdapter
- [x] 序列化stub实现 - 已实现
- [ ] Protobuf完整实现（当前为stub，需要Protobuf）
- [ ] 其他序列化格式支持（JSON等）

### 3.5 JSON适配器
- [x] JSON接口定义 - 已定义IJsonAdapter
- [x] JSON stub实现 - 已实现json_adapter_stub.cpp
- [x] JSON RapidJSON实现 - 已实现json_adapter_rapidjson.cpp

---

## 四、可选独立组件

### 4.1 DSL工具项目
- [ ] DSL编辑器
- [ ] DSL可视化工具
- [ ] DSL语法高亮支持
- [ ] DSL实时验证

> 注意：DSL解析和验证功能已在AgentFlow中实现（WorkflowParser）

### 4.2 监控日志收集器
- [ ] 监控数据采集增强
- [ ] 日志数据汇聚
- [ ] 第三方运维系统对接
- [ ] 监控仪表板（Dashboard）
- [ ] 告警规则配置
- [ ] 告警通知机制

### 4.3 后端服务
- [ ] 文件存储服务集成
- [ ] 大语言模型推理服务集成
- [ ] 其他第三方Agent或工具服务集成

---

## 五、其他

### 5.1 代码质量
- [ ] 完善单元测试覆盖率（目标：>80%）
- [ ] 集成测试
- [ ] 端到端测试
- [ ] 性能测试
- [ ] 压力测试
- [ ] 内存泄漏检测
- [ ] 线程安全测试

### 5.2 文档
- [ ] API文档生成（Doxygen）
- [ ] 架构设计文档
- [ ] 部署指南
- [ ] 运维手册
- [ ] 性能调优指南
- [ ] 故障排查手册

### 5.3 构建和部署
- [ ] Docker镜像构建
- [ ] Docker Compose配置
- [ ] Kubernetes部署文件（Helm Charts）
- [ ] CI/CD Pipeline配置
- [ ] 跨平台编译支持（Linux、macOS）
- [ ] 包管理器支持（deb、rpm）

---

## 优先级建议

### P0 - 核心功能（存在已知缺陷需修复）
- [x] 1. 完整的DSL解析和验证
- [x] 2. AgentRuntime三层调度完整实现
  - ⚠️ on_node_complete调度闭环断裂；std::rand()线程不安全
- [x] 3. Node执行完整实现
  - ⚠️ C++函数执行为stub（Shell/Python/SO等已完成）
- [x] 4. 基础监控接口
- [x] 5. 持久化存储基础功能
- [x] 6. 复杂流程控制
  - ⚠️ FOR_EACH循环未实现

### P1 - 重要功能（高优先级）
1. [x] **复杂流程控制与Workflow调度集成** - ControlFlowManager已实现 ⚠️ 调度闭环需补全
2. [x] **本地零拷贝共享实现** - 已实现ZeroCopyDataBus ⚠️ 仅InProcess可用，POSIX/SystemV返回nullptr
3. [x] **本地数据通道实现** - 已实现LocalChannel，支持Agent/Workflow/Node间数据共享
4. [x] **微服务层接口封装** - 已实现Request/Response封装器 ⚠️ from_json/from_binary为stub
5. [x] **Protoactor-cpp适配** - 已完成Protoactor-cpp消息序列化和Ask模式适配
6. [x] **libevent完整集成** - 已实现事件循环、定时器、文件描述符事件
7. ⏸ **Node控制面Actor实现** - 暂时不需要实现
8. ⏸ **WorkflowActor协调逻辑** - 暂时不需要实现

### P2 - 扩展功能（中优先级）
1. **分布式执行支持** - 执行代理模式、多节点协同工作
2. **多Master实例去中心化架构** - 分布式协调机制
3. **mesh式分布式监控架构** - 跨实例监控数据汇聚
4. **监控和日志增强** - 第三方系统对接、自定义指标导出
5. **资源访问权限管理** - Agent资源和Workflow资源权限控制

### P3 - 可选功能（低优先级）
1. **远程存储支持** - S3、HDFS等云存储集成
2. **可视化工具** - DSL可视化、监控仪表板
3. **高级运维功能** - 自动扩缩容、故障自愈
4. **多语言Worker池分配** - 不同语言实现Node的智能分配

### P4 - 外部依赖相关（不在此项目实现）
1. **第三方库实现** - Protoactor-cpp、libwebsockets、OpenSSL、libevent、Protobuf等
2. **DSL工具项目** - 独立的DSL编辑器和可视化工具
3. **监控日志收集器** - 独立的监控数据采集和汇聚服务

> 说明：P4类功能由成熟的开源项目提供，AgentFlow仅提供接口适配和集成，不需要在项目中重新实现。详见[DEPENDENCIES.md](DEPENDENCIES.md)。

---

## 实现建议

### 第一阶段（已完成）
1. ✅ 完成ControlFlowManager与Workflow调度的集成
2. ✅ 实现本地零拷贝共享（基于共享内存）
3. ✅ 实现本地数据通道（支持Agent/Workflow/Node间数据共享）

### 第二阶段（已完成）
1. ✅ 实现微服务层接口封装（Request/Response）
2. ✅ 完善第三方库集成（Protoactor-cpp、libevent）

### 第三阶段：缺陷修复与优化（建议优先执行）

> 详细优化指南见 [docs/CODE_REVIEW.md](docs/CODE_REVIEW.md)

#### 立即修复（P0 缺陷）
1. 🔧 CMakeLists.txt 加入 performance_metrics.cpp
2. 🔧 修复 gtest RPATH 链接问题
3. 🔧 std::rand() → thread_local std::mt19937（agent_runtime.cpp）
4. 🔧 on_node_complete 调度闭环补全（agent_runtime.cpp）
5. 🔧 execute_with_timeout lambda 引用悬空修复（node.cpp）

#### 短期补全（P1 功能）
1. 🔧 execute_cpp_function 真实实现（node.cpp）
2. 🔧 ZeroCopyDataBus put()后触发订阅通知（zero_copy_databus.cpp）
3. 🔧 Request/Response from_json/from_binary 反序列化实现
4. 🔧 FOR_EACH 循环实现（control_flow.cpp）
5. 🔧 条件表达式解析缓存（workflow.cpp）
6. 🔧 AgentRuntime 优雅关闭机制
7. 🔧 补充核心模块单元测试（Agent/Workflow/DataBus/ControlFlow/AgentRuntime）

### 第四阶段（暂不实现）
1. ⏸ Node控制面Actor实现
2. ⏸ WorkflowActor协调逻辑实现
3. ⏸ 执行代理模式（分布式）
4. ⏸ 多Master实例去中心化架构
5. ⏸ mesh式分布式监控架构

### 第四阶段（优化和运维）
1. ⏸ 实现监控指标导出到Prometheus
2. ⏸ 完善测试覆盖率
3. ⏸ 编写完整的文档
4. ⏸ 实现Docker和Kubernetes部署支持

---

## 更新日志

| 日期 | 变更 |
|------|------|
| 2026-02-17 | 完成P1功能：Protoactor-cpp适配、本地数据通道、微服务接口封装、libevent完整集成。Node/Workflow Actor控制面功能暂时不需要实现 |
| 2026-02-16 | 重新梳理功能实现状态，更新优先级排序，明确P4外部依赖功能 |
