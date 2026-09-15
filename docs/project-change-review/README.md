# AgentFlow Harness 分支实质改动总览

## 1. 对比口径

本目录回答的是：从最初开始阅读和修改 Harness 到当前工作区，项目的实际运行逻辑发生了什么变化。

- 起始基线：提交 118c4be，2026-08-24 首次切入 harness_agent_poc_evo 分支时的状态。
- 当前快照：提交 4e56714 加上 2026-08-30 尚未提交的工作区改动。
- 对比对象是当前工作区，而不只是 HEAD。原因是大量近期修复仍在工作区中。
- 创建本目录前，Git 统计到相对基线有 205 个已跟踪文件变化，约 21047 行新增、2993 行删除；该数字包含测试和文档，不能直接当作生产代码规模。

## 2. 明确排除的内容

下列内容没有纳入模块改动正文，因为它们不代表项目通用能力的变化：

- API Base URL、模型名称、套餐和密钥的本地替换。
- hchat、SSH、端口转发等个人使用方式。
- 单次或多次测试运行、测试输入、超时加长、测试结果数字。
- 生成的图片、视频、上传文件、工作区产物、SessionLog 实例和 SQLite 测试数据。
- 只用于个人理解的旧解读文档、示意图和轨迹树样例。
- 仅为测试代码补 include、调整 mock 或增加开发环境依赖的改动。

但以下内容虽然源于测试发现，实际改变了运行逻辑，因此仍然纳入：并行调度、超时预算、取消、业务状态、轨迹采集、Sandbox 协议、Artifact、安全隔离等。

## 3. 总体结论

最初的 Harness 更接近“Coordinator 派发一个通用 skill-runner，依靠共享 DataBus 键和进程退出状态继续运行”的原型。当前工作区已经形成一套可并行、可关联、可回放、可区分业务失败和框架失败的执行链：

1. Bridge 用 user_id、client_id 和 dialogue_id 标识一次真实交互。
2. Coordinator 将一次 run_skills 拆成带 batch_id 和 exec_skill_key 的独立分支清单。
3. C++ 底座按并发上限启动分支，等待所有分支进入终态后只触发一次 Coordinator。
4. Skill Executor 根据清单恢复真实 Skill 身份，执行自己的 LLM 与 Tool 循环。
5. 每个分支独立保存 Workflow 终态、Skill 业务终态、结果和 Artifact。
6. SessionLog 用稳定事件、显式关联键和完整终态记录整个 Dialogue。
7. Bridge 最终交付与 Coordinator 候选答案分开记录，形成可回放的真实输出事实。
8. 凭据在进程、Tool、Sandbox、日志、轨迹、SQLite 和下载链路上有统一保护。

## 4. 模块导航

| 文件 | 模块 | 重点 |
|---|---|---|
| [01-harness-coordinator.md](01-harness-coordinator.md) | Harness Coordinator | 派发清单、重复判定、部分派发、异步结果回接、Loop 控制 |
| [02-skill-tool-execution.md](02-skill-tool-execution.md) | Skill 与 Tool 执行 | Skill 身份、LLM/Tool 循环、恢复、终态、Tool 并行与结果顺序 |
| [03-cpp-parallel-foundation.md](03-cpp-parallel-foundation.md) | C++ AgentFlow 底座 | Batch 状态机、并发槽位、ALL Join、结果隔离、pending 屏障 |
| [04-trajectory-sessionlog.md](04-trajectory-sessionlog.md) | 轨迹与 SessionLog | Span、LLM/Tool 事实、事件信封、目录身份、校验和回放 |
| [05-bridge-conversation-delivery.md](05-bridge-conversation-delivery.md) | Bridge | 用户与会话、多轮持久化、SSE 终态、取消、最终交付 |
| [06-sandbox-artifact.md](06-sandbox-artifact.md) | Sandbox 与 Artifact | 本地/E2B 执行、HTTP 工具协议、路径边界、产物回收 |
| [07-security-privacy.md](07-security-privacy.md) | 安全与隐私 | 厂商无关凭据识别、进程隔离、出站阻断、全链路脱敏 |
| [08-runtime-lifecycle.md](08-runtime-lifecycle.md) | 运行生命周期 | 超时、取消、TTL、GC、Daemon/Bridge 生命周期 |
| [09-end-to-end-contract.md](09-end-to-end-contract.md) | 跨模块契约 | 完整执行链、身份映射、五类状态、建树连接规则 |
| [10-change-index.md](10-change-index.md) | 变更编号索引 | 将 HARNESS_CHANGES.md 中的实质变更映射到上述模块 |

## 5. 当前状态说明

这不是一份“已经可直接合并的 PR 清单”，而是当前工作区的语义快照：

- 当前工作区仍有大量未提交文件，同一个模块可能同时包含已提交和未提交内容。
- HARNESS_CHANGES.md 在 HEAD 中只记录到 CHG-20260826-003，后续记录本身仍在工作区；不能仅凭该文件在 HEAD 中的位置判断对应代码是否已经提交。
- GitNexus 的 HEAD 索引可用，但刷新工作区索引时因 256 MiB 图数据库缓冲池耗尽而失败。本文对未提交内容使用源代码搜索、Git diff 和变更记录交叉确认，不把图查询的空结果当作“无调用者”。
- GitNexus 对现有整体改动给出的风险是 CRITICAL，主要原因是改动横跨 Coordinator、C++ Agent、Runtime、Bridge、Sandbox 和轨迹链。本文新增内容本身只有 Markdown，不改变运行行为。

## 6. 推荐阅读顺序

如果目标是后续做轨迹树和自进化，建议依次阅读：

1. 09-end-to-end-contract.md，先建立全局身份和状态模型。
2. 01-harness-coordinator.md 与 03-cpp-parallel-foundation.md，理解派发和并发闭环。
3. 02-skill-tool-execution.md，理解每个树分支内部发生什么。
4. 04-trajectory-sessionlog.md，理解现有数据能怎样连接成树。
5. 05、06、07、08，补全交付、Sandbox、安全和生命周期边界。

