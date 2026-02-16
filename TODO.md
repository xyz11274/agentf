# AgentFlow TODO - 待实现功能列表

本文档列出了 README 中描述但尚未实现的核心功能。

## 一、开发编排框架 - 待实现

### 1.1 Agent编排
- [ ] Agent资源访问权限和管理策略配置
- [ ] 资源与workflow之间的共享关系定义
- [ ] Agent版本管理逻辑（当前只有字段）
- [ ] Agent灰度发布配置

### 1.2 Workflow编排
- [x] 完整的DSL解析实现（当前为stub） - 已实现WorkflowParser
- [x] DSL验证功能 - 已实现验证逻辑
- [ ] 灰度发布支持
- [ ] 版本回滚机制

### 1.3 Node实现规范
- [x] 完善控制面/运行面分离架构 - 已定义ExecutionMode
- [x] 支持非C++代码实现的Node（shell脚本、so文件、jar包、pyc文件等） - 已支持SHELL_SCRIPT执行
- [x] 运行面DSL配置指定运行模式 - 已支持

### 1.4 持久化
- [ ] 本地存储Agent定义实现
- [ ] 本地存储Workflow定义实现
- [ ] 版本管理和变更历史
- [ ] 灰度发布配置存储
- [ ] CI/CD流程集成接口

## 二、运行框架 - 待实现

### 2.1 Agent运行管理
- [ ] 微服务层接口封装（gRPC、RESTful、WebSocket）
- [ ] Agent对外统一访问接口
- [ ] 多种协议支持（gRPC、RestFull、websocket）

### 2.2 Workflow运行和Node调度
- [x] 完整的DSL动态加载和解析 - 已通过WorkflowParser实现
- [x] 完整的DAG调度算法 - 已实现Kahn算法拓扑排序
- [x] 并行执行互不依赖的Node（当前只定义了接口）- 已实现get_executable_nodes
- [x] Node执行状态机完整实现 - 已实现状态转换
- [ ] 复杂流程控制（条件、循环、异常处理）

### 2.3 AgentRuntime - 核心分层调度
- [x] 完整的Agent层Master实现（状态监控、资源分配、故障转移）- 已实现monitor_agents
- [x] 完整的Workflow层Master实现（依赖关系计算、调度决策）- 已实现调度逻辑
- [x] 动态负载均衡 - 已实现worker负载跟踪
- [ ] 多Master实例去中心化架构
- [ ] 分布式协调机制

### 2.4 系统通信机制
- [ ] 业务数据通信面实现（private protocol）
- [ ] 本地零拷贝共享实现
- [ ] 跨节点高速传输
- [ ] 系统维护通信面完整实现
- [ ] gRPC协议支持
- [ ] Restful协议支持
- [ ] WebSocket协议完整实现

### 2.5 监控和日志
- [x] Workflow执行全链路监控 - 已支持workflow状态跟踪
- [x] Node级别详细监控 - 已支持Node执行状态监控
- [ ] mesh式分布式监控架构
- [x] Agent状态监控（生命周期、资源使用等）- 已实现基础监控
- [x] Node执行状态监控 - 已支持Node状态跟踪
- [x] 性能指标采集（延迟、吞吐量等）- 已实现Histogram指标
- [x] 标准监控接口定义 - 已定义导出格式
- [ ] 第三方日志系统对接
- [ ] 运维系统gRPC/Restful接口对接
- [ ] 自定义监控指标导出

### 2.6 分布式执行支持
- [ ] 执行代理模式实现
- [ ] 多节点协同工作
- [ ] 远程Worker管理
- [ ] Worker动态分配和回收
- [ ] 多语言实现Node分配到不同Worker池

## 三、基础设施 - 待实现

### 3.1 Actor机制
- [ ] Protoactor-cpp完整集成（当前为stub）
- [ ] Node控制面Actor实现
- [ ] Node间Actor协作机制

### 3.2 网络适配器
- [ ] WebSocket完整实现（当前为stub）
- [ ] SSL/TLS完整实现（当前为stub）
- [ ] libwebsockets集成
- [ ] OpenSSL集成

### 3.3 事件循环适配器
- [ ] libevent完整集成（当前为stub）
- [ ] 事件循环完整实现

### 3.4 序列化适配器
- [ ] Protobuf完整实现
- [ ] 其他序列化格式支持（JSON等）

## 四、可选独立组件

### 4.1 DSL工具项目
- [ ] DSL编辑器
- [ ] DSL解析器
- [ ] DSL验证器
- [ ] DSL可视化工具

### 4.2 监控日志收集器
- [ ] 监控数据采集
- [ ] 日志数据汇聚
- [ ] 第三方运维系统对接
- [ ] 监控仪表板

### 4.3 后端服务
- [ ] 文件存储服务
- [ ] 大语言模型推理服务
- [ ] 其他第三方Agent或工具服务

## 五、其他

### 5.1 代码质量
- [ ] 完善单元测试覆盖率
- [ ] 集成测试
- [ ] 性能测试
- [ ] 压力测试

### 5.2 文档
- [ ] API文档生成（Doxygen）
- [ ] 架构设计文档
- [ ] 部署指南
- [ ] 运维手册

### 5.3 构建和部署
- [ ] Docker镜像构建
- [ ] Kubernetes部署文件
- [ ] CI/CD Pipeline配置
- [ ] 跨平台编译支持（Linux、macOS）

## 优先级建议

### P0 - 核心功能（必须） - 已完成
- [x] 1. 完整的DSL解析和验证 - 已实现WorkflowParser，支持JSON解析和验证
- [x] 2. AgentRuntime三层调度完整实现 - 已实现完整的三层调度架构
- [x] 3. Node执行完整实现 - 已支持C++函数和Shell命令执行
- [x] 4. 基础监控接口 - 已实现MetricsCollector，支持Counter/Gauge/Histogram

### P1 - 重要功能（高优先级）
1. 网络协议完整实现（gRPC、RESTful、WebSocket）
2. 持久化存储
3. 版本管理
4. Actor机制完整集成

### P2 - 扩展功能（中优先级）
1. 分布式执行
2. 复杂流程控制
3. 高级监控
4. 性能优化

### P3 - 可选功能（低优先级）
1. DSL工具项目
2. 可视化工具
3. 运维对接集成
