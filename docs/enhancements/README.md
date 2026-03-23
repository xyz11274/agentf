# AgentFlow Enhancements

本目录包含 AgentFlow 项目的各类功能增强、缺陷修复和重构记录。

## 状态说明

| 状态 | 说明 |
|------|------|
| `tbd` | 待实施，尚未开始 |
| `wip` | 进行中，正在开发 |
| `implemented` | 已完成并验证 |

## 类型说明

| 类型 | 说明 |
|------|------|
| `feature` | 新功能 |
| `bugfix` | 缺陷修复 |
| `refactor` | 代码重构 |

## 增强列表

按日期升序排列：

| 编号 | 日期 | 标题 | 类型 | 状态 |
|------|------|------|------|------|
| 001 | 2026-02-21 | [MMAgent 包管理与加载重构 (2026-02-21)](001-refactor-mmagent-包管理与加载重构-260221-implemented.md) | refactor | implemented |
| 002 | 2026-02-22 | [文档治理与架构/API 对齐（2026-02-22）](002-feature-文档治理与架构api-对齐-260222-implemented.md) | feature | implemented |
| 003 | 2026-02-22 | [MMDataBus 全局共享重构（2026-02-22）](003-refactor-mmdatabus-全局共享重构-260222-implemented.md) | refactor | implemented |
| 004 | 2026-02-23 | [Runtime Master 自主调度重构（2026-02-23）](004-refactor-runtime-master-自主调度重构-260223-implemented.md) | refactor | implemented |
| 005 | 2026-02-23 | [Visualizer GOTO 环路可视化修复（2026-02-23）](005-bugfix-visualizer-goto-环路可视化修复-260223-implemented.md) | bugfix | implemented |
| 006 | 2026-02-23 | [Worker/MMWorkflow/MMNode/MMEdge 自主执行重构（2026-02-23）](006-refactor-workermmworkflowmmnodemmedge-自主执行重构-260223-implemented.md) | refactor | implemented |
| 007 | 2026-02-23 | [Vendor 依赖配置与文档刷新（2026-02-23）](007-feature-vendor-依赖配置与文档刷新-260223-implemented.md) | feature | implemented |
| 008 | 2026-02-23 | [示例与对外接口收敛重构（2026-02-23）](008-refactor-示例与对外接口收敛重构-260223-implemented.md) | refactor | implemented |
| 009 | 2026-02-28 | [MMNode 执行引擎下沉 + Coordinator Task0 默认化（2026-02-28）](009-feature-mmnode-执行引擎下沉-coordinator-task0-默认化-260228-implemented.md) | feature | implemented |
| 010 | 2026-02-28 | [MMNode 热更新能力下沉到 Worker（2026-02-28）](010-feature-mmnode-热更新能力下沉到-worker-260228-implemented.md) | feature | implemented |
| 011 | 2026-03-01 | [2026-03-01 SessionTaskCoordinator & Context 分层重构](011-refactor-sessiontaskcoordinator-context-分层重构-260301-implemented.md) | refactor | implemented |
| 012 | 2026-03-06 | [2026-03-06 multi_session_chat 多语言节点重构](012-refactor-multi-session-chat-多语言节点重构-260306-implemented.md) | refactor | implemented |
| 013 | 2026-03-07 | [2026-03-07 src/agent session/task 配置模型重构](013-refactor-srcagent-sessiontask-配置模型重构-260307-implemented.md) | refactor | implemented |
| 014 | 2026-03-07 | [2026-03-07 agent/runtime 双队列与执行风险等级重构](014-refactor-agentruntime-双队列与执行风险等级重构-260307-implemented.md) | refactor | implemented |
| 015 | 2026-03-07 | [2026-03-07 examples 目录结构整理](015-feature-examples-目录结构整理-260307-implemented.md) | feature | implemented |
| 016 | 2026-03-07 | [2026-03-07 全链路跟踪系统（TraceSystem）](016-feature-全链路跟踪系统tracesystem-260307-implemented.md) | feature | implemented |
| 017 | 2026-03-17 | [2026-03-17 agent.json 配置结构冗余分析](017-feature-agentjson-配置结构冗余分析-260317-implemented.md) | feature | implemented |
| 018 | 2026-03-17 | [2026-03-17 Skill 参数槽位校验功能](018-feature-skill-参数槽位校验功能-260317-implemented.md) | feature | implemented |
| 019 | 2026-03-17 | [2026-03-17 Plugin UDS 注入机制设计](019-feature-plugin-uds-注入机制设计-260317-implemented.md) | feature | implemented |
| 020 | 2026-03-18 | [2026-03-18 Node 输出数据冲突问题修复](020-bugfix-node-输出数据冲突问题修复-260318-tbd.md) | bugfix | tbd |
| 021 | 2026-03-18 | [2026-03-18 Workflow Input Dialogue 级别隔离](021-feature-workflow-input-dialogue-级别隔离-260318-implemented.md) | feature | implemented |
| 022 | 2026-03-19 | [2026-03-19 Multi-Response 机制设计](022-feature-multi-response-机制设计-260319-implemented.md) | feature | implemented |
| 023 | 2026-03-19 | [2026-03-19 next_skills 格式扩展](023-feature-next-skills-格式扩展-260319-implemented.md) | feature | implemented |
| 024 | 2026-03-19 | [2026-03-19 消息定向路由机制设计](024-feature-消息定向路由机制设计-260319-implemented.md) | feature | implemented |
| 025 | 2026-03-20 | [2026-03-20 Session 老化 DataBus 清理问题](025-feature-session-老化-databus-清理问题-260320-tbd.md) | feature | tbd |
| 026 | 2026-03-20 | [2026-03-20 Master-Worker 架构边界问题](026-feature-master-worker-架构边界问题-260320-tbd.md) | feature | tbd |
| 027 | 2026-03-22 | [2026-03-22 UDSServer 线程池改造](027-feature-udsserver-线程池改造-260322-tbd.md) | feature | tbd |

## 按状态分类

### 待实施 (tbd)

- [2026-03-18 - 2026-03-18 Node 输出数据冲突问题修复](020-bugfix-node-输出数据冲突问题修复-260318-tbd.md)
- [2026-03-20 - 2026-03-20 Session 老化 DataBus 清理问题](025-feature-session-老化-databus-清理问题-260320-tbd.md)
- [2026-03-20 - 2026-03-20 Master-Worker 架构边界问题](026-feature-master-worker-架构边界问题-260320-tbd.md)
- [2026-03-22 - 2026-03-22 UDSServer 线程池改造](027-feature-udsserver-线程池改造-260322-tbd.md)

### 进行中 (wip)

*暂无进行中项目*

### 已完成 (implemented)

- [2026-02-21 - MMAgent 包管理与加载重构 (2026-02-21)](001-refactor-mmagent-包管理与加载重构-260221-implemented.md)
- [2026-02-22 - 文档治理与架构/API 对齐（2026-02-22）](002-feature-文档治理与架构api-对齐-260222-implemented.md)
- [2026-02-22 - MMDataBus 全局共享重构（2026-02-22）](003-refactor-mmdatabus-全局共享重构-260222-implemented.md)
- [2026-02-23 - Runtime Master 自主调度重构（2026-02-23）](004-refactor-runtime-master-自主调度重构-260223-implemented.md)
- [2026-02-23 - Visualizer GOTO 环路可视化修复（2026-02-23）](005-bugfix-visualizer-goto-环路可视化修复-260223-implemented.md)
- [2026-02-23 - Worker/MMWorkflow/MMNode/MMEdge 自主执行重构（2026-02-23）](006-refactor-workermmworkflowmmnodemmedge-自主执行重构-260223-implemented.md)
- [2026-02-23 - Vendor 依赖配置与文档刷新（2026-02-23）](007-feature-vendor-依赖配置与文档刷新-260223-implemented.md)
- [2026-02-23 - 示例与对外接口收敛重构（2026-02-23）](008-refactor-示例与对外接口收敛重构-260223-implemented.md)
- [2026-02-28 - MMNode 执行引擎下沉 + Coordinator Task0 默认化（2026-02-28）](009-feature-mmnode-执行引擎下沉-coordinator-task0-默认化-260228-implemented.md)
- [2026-02-28 - MMNode 热更新能力下沉到 Worker（2026-02-28）](010-feature-mmnode-热更新能力下沉到-worker-260228-implemented.md)
- [2026-03-01 - 2026-03-01 SessionTaskCoordinator & Context 分层重构](011-refactor-sessiontaskcoordinator-context-分层重构-260301-implemented.md)
- [2026-03-06 - 2026-03-06 multi_session_chat 多语言节点重构](012-refactor-multi-session-chat-多语言节点重构-260306-implemented.md)
- [2026-03-07 - 2026-03-07 src/agent session/task 配置模型重构](013-refactor-srcagent-sessiontask-配置模型重构-260307-implemented.md)
- [2026-03-07 - 2026-03-07 agent/runtime 双队列与执行风险等级重构](014-refactor-agentruntime-双队列与执行风险等级重构-260307-implemented.md)
- [2026-03-07 - 2026-03-07 examples 目录结构整理](015-feature-examples-目录结构整理-260307-implemented.md)
- [2026-03-07 - 2026-03-07 全链路跟踪系统（TraceSystem）](016-feature-全链路跟踪系统tracesystem-260307-implemented.md)
- [2026-03-17 - 2026-03-17 agent.json 配置结构冗余分析](017-feature-agentjson-配置结构冗余分析-260317-implemented.md)
- [2026-03-17 - 2026-03-17 Skill 参数槽位校验功能](018-feature-skill-参数槽位校验功能-260317-implemented.md)
- [2026-03-17 - 2026-03-17 Plugin UDS 注入机制设计](019-feature-plugin-uds-注入机制设计-260317-implemented.md)
- [2026-03-18 - 2026-03-18 Workflow Input Dialogue 级别隔离](021-feature-workflow-input-dialogue-级别隔离-260318-implemented.md)
- [2026-03-19 - 2026-03-19 Multi-Response 机制设计](022-feature-multi-response-机制设计-260319-implemented.md)
- [2026-03-19 - 2026-03-19 next_skills 格式扩展](023-feature-next-skills-格式扩展-260319-implemented.md)
- [2026-03-19 - 2026-03-19 消息定向路由机制设计](024-feature-消息定向路由机制设计-260319-implemented.md)
