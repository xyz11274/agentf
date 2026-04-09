# AgentFlow Enhancements

本目录包含 AgentFlow 项目的各类功能增强、缺陷修复和重构记录。

## Enhancement 创建规范

**格式**：`{编号}-{类型}-{名称}-{YYMMDD}-{状态}.md`

**示例**：`020-feature-multi-response-260319-implemented.md`

### 命名规则

| 组成部分 | 说明 | 可选值 |
|---------|------|--------|
| 编号 | 三位数字，每文档递增 | 001, 002, ... |
| 类型 | 变更类型 | `feature`, `bugfix`, `refactor` |
| 名称 | 小写英文，连字符分隔 | `session-isolation`, `multi-response` |
| 日期 | YYMMDD 格式 | 260323 表示 2026-03-23 |
| 状态 | 实施状态 | `tbd`, `wip`, `implemented` |

### 新建 Enhancement 流程

**创建前准备**：
1. 分析已有 enhancement 方案，避免重复设计
2. 分析整体架构，理解变更影响范围
3. 分析关联代码，识别依赖关系

**创建流程**：
1. 创建新文件，编号取当前最大编号 +1
2. 状态初始为 `tbd`（待实施）
3. **按照文档模板编写内容**（参见下方"Enhancement 文档模板"）
4. **更新 README.md 索引**
5. 讨论方案、刷新文档内容
6. 实施开始时改为 `wip`（进行中），更新 README.md 索引
7. 完成并验证后改为 `implemented`（已完成），更新 README.md 索引

**实施要求**：
- `tbd` 文档必须经过人工确认后才能改为 `wip` 状态
- 禁止直接基于 `tbd` 文档实现代码

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

| 编号 | 日期 | 标题 | 类型 | 状态 | 背景 |
|------|------|------|------|------|------|
| 001 | 2026-02-21 | [MMAgent 包管理与加载重构](001-refactor-mmagent-包管理与加载重构-260221-implemented.md) | refactor | implemented | Agent实例生命周期管理需要统一的包管理机制 |
| 002 | 2026-02-22 | [文档治理与架构/API 对齐](002-feature-文档治理与架构api-对齐-260222-implemented.md) | feature | implemented | 文档与代码实现存在不一致，需要系统性治理 |
| 003 | 2026-02-22 | [MMDataBus 全局共享重构](003-refactor-mmdatabus-全局共享重构-260222-implemented.md) | refactor | implemented | DataBus实例分散在各处，缺乏统一管理 |
| 004 | 2026-02-23 | [Runtime Master 自主调度重构](004-refactor-runtime-master-自主调度重构-260223-implemented.md) | refactor | implemented | Master调度逻辑分散，难以维护和扩展 |
| 005 | 2026-02-23 | [Visualizer GOTO 环路可视化修复](005-bugfix-visualizer-goto-环路可视化修复-260223-implemented.md) | bugfix | implemented | GOTO节点形成环路时可视化显示异常 |
| 006 | 2026-02-23 | [Worker/MMWorkflow/MMNode/MMEdge 自主执行重构](006-refactor-workermmworkflowmmnodemmedge-自主执行重构-260223-implemented.md) | refactor | implemented | 节点执行逻辑耦合度高，难以独立测试 |
| 007 | 2026-02-23 | [Vendor 依赖配置与文档刷新](007-feature-vendor-依赖配置与文档刷新-260223-implemented.md) | feature | implemented | 第三方依赖管理混乱，文档缺失 |
| 008 | 2026-02-23 | [示例与对外接口收敛重构](008-refactor-示例与对外接口收敛重构-260223-implemented.md) | refactor | implemented | 对外接口过于分散，示例代码不够清晰 |
| 009 | 2026-02-28 | [MMNode 执行引擎下沉 + Coordinator Task0 默认化](009-feature-mmnode-执行引擎下沉-coordinator-task0-默认化-260228-implemented.md) | feature | implemented | 执行引擎层级过高，Coordinator初始化复杂 |
| 010 | 2026-02-28 | [MMNode 热更新能力下沉到 Worker](010-feature-mmnode-热更新能力下沉到-worker-260228-implemented.md) | feature | implemented | 热更新能力仅在Runtime层，Worker无法独立更新 |
| 011 | 2026-03-01 | [SessionTaskCoordinator & Context 分层重构](011-refactor-sessiontaskcoordinator-context-分层重构-260301-implemented.md) | refactor | implemented | Session/Task/Coordinator职责边界模糊 |
| 012 | 2026-03-06 | [multi_session_chat 多语言节点重构](012-refactor-multi-session-chat-多语言节点重构-260306-implemented.md) | refactor | implemented | 多语言节点实现不统一，维护困难 |
| 013 | 2026-03-07 | [src/agent session/task 配置模型重构](013-refactor-srcagent-sessiontask-配置模型重构-260307-implemented.md) | refactor | implemented | Session/Task配置模型分散，扩展性差 |
| 014 | 2026-03-07 | [agent/runtime 双队列与执行风险等级重构](014-refactor-agentruntime-双队列与执行风险等级重构-260307-implemented.md) | refactor | implemented | 消息队列单一，无法区分优先级和风险 |
| 015 | 2026-03-07 | [examples 目录结构整理](015-feature-examples-目录结构整理-260307-implemented.md) | feature | implemented | examples目录结构混乱，示例难以理解 |
| 016 | 2026-03-07 | [全链路跟踪系统（TraceSystem）](016-feature-全链路跟踪系统tracesystem-260307-implemented.md) | feature | implemented | 缺乏全链路跟踪能力，问题定位困难 |
| 017 | 2026-03-17 | [agent.json 配置结构冗余分析](017-feature-agentjson-配置结构冗余分析-260317-implemented.md) | feature | implemented | agent.json配置存在冗余字段 |
| 018 | 2026-03-17 | [Skill 参数槽位校验功能](018-feature-skill-参数槽位校验功能-260317-implemented.md) | feature | implemented | Skill调用时参数校验不足，容易出错 |
| 019 | 2026-03-17 | [Plugin UDS 注入机制设计](019-feature-plugin-uds-注入机制设计-260317-implemented.md) | feature | implemented | Plugin无法访问DataBus，限制了扩展能力 |
| 020 | 2026-03-18 | [Node 输出数据冲突问题修复](020-bugfix-node-输出数据冲突问题修复-260318-implemented.md) | bugfix | implemented | 多节点并行执行时输出数据可能冲突 |
| 021 | 2026-03-18 | [Workflow Input Dialogue 级别隔离](021-feature-workflow-input-dialogue-级别隔离-260318-implemented.md) | feature | implemented | 多对话场景下输入数据未隔离，可能串话 |
| 022 | 2026-03-19 | [Multi-Response 机制设计](022-feature-multi-response-机制设计-260319-implemented.md) | feature | implemented | 流式响应场景需要多次返回结果 |
| 023 | 2026-03-19 | [next_skills 格式扩展](023-feature-next-skills-格式扩展-260319-implemented.md) | feature | implemented | next_skills无法携带技能特定数据 |
| 024 | 2026-03-19 | [消息定向路由机制设计](024-feature-消息定向路由机制设计-260319-implemented.md) | feature | implemented | 消息路由经过多层转发，延迟高 |
| 025 | 2026-03-20 | [Session 老化 DataBus 清理问题](025-feature-session-老化-databus-清理问题-260320-implemented.md) | feature | implemented | Session老化后DataBus数据未及时清理 |
| 026 | 2026-03-20 | [Master-Worker 架构边界问题](026-feature-master-worker-架构边界问题-260320-tbd.md) | feature | tbd | Master和Worker职责边界不清晰 |
| 027 | 2026-03-22 | [UDSServer 线程池改造](027-feature-udsserver-线程池改造-260322-implemented.md) | feature | implemented | UDSServer单线程处理，高并发性能不足 |
| 028 | 2026-03-23 | [Node 模型推理服务流式访问设计](028-feature-node-模型推理服务流式访问设计-260323-tbd.md) | feature | tbd | LLM流式调用阻塞Worker线程 |
| 029 | 2026-03-24 | [上下文回调机制优化](029-feature-上下文回调机制优化-260324-implemented.md) | feature | implemented | 用户上下文注入时机和方式不够灵活 |
| 030 | 2026-03-27 | [Node 异步请求机制设计](030-feature-node-异步请求机制设计-260326-implemented.md) | feature | implemented | Node同步阻塞调用外部资源，并发能力受限 |
| 031 | 2026-04-05 | [Coordinator 协同机制设计分析](031-feature-coordinator-协同机制设计分析-260405-tbd.md) | feature | tbd | Coordinator协同判定与多轮规划机制需增强 |
| 032 | 2026-04-05 | [Dispatcher Coordinator Skill 与 DataBus 可观测性分析](032-feature-dispatcher-coordinator-databus-observability-260405-tbd.md) | feature | tbd | 多任务协同与DataBus并发场景下日志与运维定界能力不足 |
| 033 | 2026-04-08 | [虚拟 Skill（意图分类）编排能力](033-feature-virtual-skill-intent-classification-260408-tbd.md) | feature | tbd | 意图分类不落重工具链路，结构化结果交给 coordinator 驱动下一阶段 loop |

## 按状态分类

### 待实施 (tbd)

| 编号 | 日期 | 标题 | 背景 |
|------|------|------|------|
| 026 | 2026-03-20 | [Master-Worker 架构边界问题](026-feature-master-worker-架构边界问题-260320-tbd.md) | Master和Worker职责边界不清晰 |
| 028 | 2026-03-23 | [Node 模型推理服务流式访问设计](028-feature-node-模型推理服务流式访问设计-260323-tbd.md) | LLM流式调用阻塞Worker线程 |
| 031 | 2026-04-05 | [Coordinator 协同机制设计分析](031-feature-coordinator-协同机制设计分析-260405-tbd.md) | Coordinator协同判定与多轮规划机制需增强 |
| 032 | 2026-04-05 | [Dispatcher Coordinator Skill 与 DataBus 可观测性分析](032-feature-dispatcher-coordinator-databus-observability-260405-tbd.md) | 多任务协同与DataBus并发场景下日志与运维定界能力不足 |
| 033 | 2026-04-08 | [虚拟 Skill（意图分类）编排能力](033-feature-virtual-skill-intent-classification-260408-tbd.md) | 意图分类不落重工具链路，结构化结果交给 coordinator 驱动下一阶段 loop |

### 进行中 (wip)

（暂无）

### 已完成 (implemented)

| 编号 | 日期 | 标题 | 背景 |
|------|------|------|------|
| 001 | 2026-02-21 | [MMAgent 包管理与加载重构](001-refactor-mmagent-包管理与加载重构-260221-implemented.md) | Agent实例生命周期管理需要统一的包管理机制 |
| 002 | 2026-02-22 | [文档治理与架构/API 对齐](002-feature-文档治理与架构api-对齐-260222-implemented.md) | 文档与代码实现存在不一致 |
| 003 | 2026-02-22 | [MMDataBus 全局共享重构](003-refactor-mmdatabus-全局共享重构-260222-implemented.md) | DataBus实例分散在各处，缺乏统一管理 |
| 004 | 2026-02-23 | [Runtime Master 自主调度重构](004-refactor-runtime-master-自主调度重构-260223-implemented.md) | Master调度逻辑分散，难以维护和扩展 |
| 005 | 2026-02-23 | [Visualizer GOTO 环路可视化修复](005-bugfix-visualizer-goto-环路可视化修复-260223-implemented.md) | GOTO节点形成环路时可视化显示异常 |
| 006 | 2026-02-23 | [Worker/MMWorkflow/MMNode/MMEdge 自主执行重构](006-refactor-workermmworkflowmmnodemmedge-自主执行重构-260223-implemented.md) | 节点执行逻辑耦合度高，难以独立测试 |
| 007 | 2026-02-23 | [Vendor 依赖配置与文档刷新](007-feature-vendor-依赖配置与文档刷新-260223-implemented.md) | 第三方依赖管理混乱，文档缺失 |
| 008 | 2026-02-23 | [示例与对外接口收敛重构](008-refactor-示例与对外接口收敛重构-260223-implemented.md) | 对外接口过于分散，示例代码不够清晰 |
| 009 | 2026-02-28 | [MMNode 执行引擎下沉 + Coordinator Task0 默认化](009-feature-mmnode-执行引擎下沉-coordinator-task0-默认化-260228-implemented.md) | 执行引擎层级过高，Coordinator初始化复杂 |
| 010 | 2026-02-28 | [MMNode 热更新能力下沉到 Worker](010-feature-mmnode-热更新能力下沉到-worker-260228-implemented.md) | 热更新能力仅在Runtime层，Worker无法独立更新 |
| 011 | 2026-03-01 | [SessionTaskCoordinator & Context 分层重构](011-refactor-sessiontaskcoordinator-context-分层重构-260301-implemented.md) | Session/Task/Coordinator职责边界模糊 |
| 012 | 2026-03-06 | [multi_session_chat 多语言节点重构](012-refactor-multi-session-chat-多语言节点重构-260306-implemented.md) | 多语言节点实现不统一，维护困难 |
| 013 | 2026-03-07 | [src/agent session/task 配置模型重构](013-refactor-srcagent-sessiontask-配置模型重构-260307-implemented.md) | Session/Task配置模型分散，扩展性差 |
| 014 | 2026-03-07 | [agent/runtime 双队列与执行风险等级重构](014-refactor-agentruntime-双队列与执行风险等级重构-260307-implemented.md) | 消息队列单一，无法区分优先级和风险 |
| 015 | 2026-03-07 | [examples 目录结构整理](015-feature-examples-目录结构整理-260307-implemented.md) | examples目录结构混乱，示例难以理解 |
| 016 | 2026-03-07 | [全链路跟踪系统（TraceSystem）](016-feature-全链路跟踪系统tracesystem-260307-implemented.md) | 缺乏全链路跟踪能力，问题定位困难 |
| 017 | 2026-03-17 | [agent.json 配置结构冗余分析](017-feature-agentjson-配置结构冗余分析-260317-implemented.md) | agent.json配置存在冗余字段 |
| 018 | 2026-03-17 | [Skill 参数槽位校验功能](018-feature-skill-参数槽位校验功能-260317-implemented.md) | Skill调用时参数校验不足，容易出错 |
| 019 | 2026-03-17 | [Plugin UDS 注入机制设计](019-feature-plugin-uds-注入机制设计-260317-implemented.md) | Plugin无法访问DataBus，限制了扩展能力 |
| 020 | 2026-03-18 | [Node 输出数据冲突问题修复](020-bugfix-node-输出数据冲突问题修复-260318-implemented.md) | 多节点并行执行时输出数据可能冲突 |
| 021 | 2026-03-18 | [Workflow Input Dialogue 级别隔离](021-feature-workflow-input-dialogue-级别隔离-260318-implemented.md) | 多对话场景下输入数据未隔离，可能串话 |
| 022 | 2026-03-19 | [Multi-Response 机制设计](022-feature-multi-response-机制设计-260319-implemented.md) | 流式响应场景需要多次返回结果 |
| 023 | 2026-03-19 | [next_skills 格式扩展](023-feature-next-skills-格式扩展-260319-implemented.md) | next_skills无法携带技能特定数据 |
| 024 | 2026-03-19 | [消息定向路由机制设计](024-feature-消息定向路由机制设计-260319-implemented.md) | 消息路由经过多层转发，延迟高 |
| 025 | 2026-03-20 | [Session 老化 DataBus 清理问题](025-feature-session-老化-databus-清理问题-260320-implemented.md) | Session老化后DataBus数据未及时清理 |
| 027 | 2026-03-22 | [UDSServer 线程池改造](027-feature-udsserver-线程池改造-260322-implemented.md) | UDSServer单线程处理，高并发性能不足 |
| 029 | 2026-03-24 | [上下文回调机制优化](029-feature-上下文回调机制优化-260324-implemented.md) | 用户上下文注入时机和方式不够灵活 |
| 030 | 2026-03-27 | [Node 异步请求机制设计](030-feature-node-异步请求机制设计-260326-implemented.md) | Node同步阻塞调用外部资源，并发能力受限 |

## Enhancement 文档模板

新增 Enhancement 必须包含以下章节：

### 1. 背景

说明为什么要做这个优化，优化希望达到的效果。

### 2. 方案简述

整体的思路和对方案要解决问题的拆解。

### 3. 约束与假设

方案的生效场景/条件，是否对其他功能有依赖，是否要考虑兼容性。

### 4. 当前实现分析

**代码分析**：现有架构图、类图和相关的核心代码片段及解释，识别关键变更点。

**Examples 分析**：分析 examples 目录下相关示例的实现，识别需要同步修改的内容。

**缺失功能分析**：
- 相对目标效果缺失的功能列表
- 行业实践研究：
  - 各类实现方案及其优劣分析
  - 对于当前方案的参考意义
  - 具体参考点总结

### 5. 数据结构设计

如果涉及变更，给出已有实现和新的方案，并给出设计思路和兼容性分析。

### 6. 流程设计

包括时序图；如果涉及变更，给出已有实现和新的方案，并给出设计思路和兼容性分析，要用调用流程和时序图呈现设计内容。

### 7. 接口设计

函数接口、消息接口、配置（DSL）接口设计，解释接口和参数的作用，如果涉及变更，给出已有实现和新的方案，并给出设计思路和兼容性分析。

### 8. 实现方案

根据对待解决问题的拆解，分小节描述实现方案，包括关键代码片段和实现思路（如何保持性能、可维护性、安全相好，保证软件架构优雅、影响最小），方案的优点，如果有潜在的问题要给出问题消减思路。如果有多个备选方案要给出方案对比的总结。

**必须包含待修改文件清单**：

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `path/to/file.h` | 新增/修改/删除 | 变更说明 |

**功能点实现优先级排序**：

按以下原则排序，优先实现无副作用影响的功能：
1. **无副作用**：不破坏已有功能，无兼容性影响，可独立验证
2. **有依赖但无影响**：依赖其他功能但不影响已有功能
3. **有影响需兼容**：有兼容性影响，需做兼容处理
4. **破坏性变更**：破坏已有功能，需慎重评估

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | 功能名称 | 无副作用 | 小范围验证 |
| P2 | 功能名称 | 有兼容影响 | 集成验证 |

### 9. 测试方案

**Examples 黑盒测试覆盖分析**：分析当前 examples 是否能满足功能覆盖，不能则给出更新建议。

| 功能点 | 现有 Examples 覆盖 | 更新建议 |
|--------|-------------------|---------|
| 功能名称 | ✅覆盖 / ❌未覆盖 | 新增/修改建议 |

**测试用例设计**：针对变更范围制定测试方案和测试用例。

**独立性要求**：
- 测试方案必须具备独立性
- 禁止因测试便捷性修改功能代码（日志除外）
- 测试数据、Mock、Stub 等应通过测试框架或配置实现

### 10. 文档

**需要变更的文档**：分析 docs 下和 publish 下的架构文档和手册，描述需要变更的文档和内容。

| 文档 | 变更说明 |
|------|---------|
| [文档名称](相对路径/xxx.md) | 具体到章节的变更内容描述 |

**关联方案**：整理本方案参考或依赖的其他 enhancement，给出引用。

| 关联方案 | 关系说明 |
|---------|---------|
| [方案名称](xxx.md) | 具体到章节的参考/依赖说明 |

### 11. 进展跟踪

给出任务 checklist，包含代码实现、验证结果、文档更新的进展，任务都完成后 enhancement 才能标注 implemented。

```markdown
### 进展跟踪

- [ ] 代码实现
- [ ] 单元测试
- [ ] 集成测试
- [ ] 文档更新
- [ ] 验证通过
```

**详细进度**：如果按功能点分步骤实现，记录已就绪和待实施的功能/用例/文档。

| 功能点 | 功能 | 用例 | 文档 | 状态 |
|--------|------|------|------|------|
| 功能模块 | ✅已就绪 / ⏳待实施 | ✅已就绪 / ⏳待实施 | ✅已就绪 / ⏳待实施 | 进行中/已完成 |

### 12. 后续计划（可选）

描述满足当前需求的情况下，还有哪些内容可以优化，给出效果和方案的简述。

| 优化项 | 预期效果 | 方案简述 |
|--------|---------|---------|
| 优化名称 | 效果描述 | 方案概述 |

### 13. 修订记录

记录文档的版本变更历史。

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | YYYY-MM-DD | 作者 | 初始版本/变更内容描述 |

**刷新要求**：
- 修订记录用于记录**实现方案**的变更（设计决策、架构调整、接口变更等）
- `wip` 文档更新实现进度（如勾选 checklist）**不刷新**修订记录
- 修改非 `tbd` 状态文档的实现方案：**必选**刷新修订记录
- 修改 `tbd` 状态文档：可选刷新修订记录（按修订者要求）