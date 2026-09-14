# 轨迹、Span 与 SessionLog 模块改动

## 1. 模块目标

该模块不是轨迹树存储实现，而是建树前的事实采集层。目标是让每个 Dialogue 中的 Coordinator、Batch、Skill、LLM、Tool、Artifact、状态迁移和最终交付都能被唯一识别、关联、校验和回放。

主文件：

- packages/agentflow-runtime/src/agentflow_runtime/recorder.py
- packages/agentflow-runtime/src/agentflow_runtime/trace/emit.py
- packages/agentflow-runtime/src/agentflow_runtime/trace/span_writer.py
- packages/agentflow-runtime/src/agentflow_runtime/events.py
- docs/sessionlog/sessionlog_hook.py
- docs/sessionlog/sessionlog_reader.py
- examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py
- resource/agent-packages/outfit-assistant/harness/hooks/sessionlog_hook.py

## 2. Span 生命周期修复

原实现中 Skill、LLM 和 Tool 的部分 return、continue、异常出口不会关闭 Span。当前：

- Skill 外层使用 try/finally 兜住所有结构化出口。
- 每轮 LLM Span 在成功、Provider 错误、恢复、超时和异常时都关闭。
- Tool 单次调用和 Tool Batch 都具有独立 Span。
- emit_event 会进入当前活动 Span，而不只是打印调试信息。
- SpanWriter 写盘失败被隔离为观测故障，不中断业务。
- 跨进程写 Span 使用锁和唯一身份，避免 JSONL 行交叉或覆盖。

## 3. LLM 事实采集

每次 LLM 调用现在具有独立 call_id，并保存：

- 调用点和执行身份。
- Coordinator 或业务 skill_name。
- 实际发送前的 HTTP 请求。
- Provider 原始响应。
- Hook 改写后真正被业务消费的有效响应。
- 成功或错误状态。
- Transport Retry 次数。
- token usage 及其来源。
- Prompt、Skill 文档和 Tool 定义的内容指纹。
- model、code revision、dirty 状态等运行版本信息。

Recorder 将 call_id 明确返回调用点，SessionLog 不再通过“索引最后一项”猜测当前调用。并发写入使用每个 call_id 的唯一索引项，旧列表只为兼容保留。

## 4. SessionLog 事件模型

当前事件使用稳定信封，核心字段包括：

- type、event_id、idempotency_key。
- trace_id、span_id、parent_span_id。
- pid、seq、timestamp。
- dialogue_id。
- source_event_seqs。
- data。

主要业务事件包括：

| 层级 | 事件 |
|---|---|
| Dialogue | dialogue.header、dialogue.start、dialogue.end、client.delivery |
| Coordinator | coordinator.activation、coordinator.decision、messages.snapshot、loop.control |
| Dispatch/Batch | skill.dispatch、batch.result |
| Skill | skill.start、skill.end |
| LLM | llm.request、llm.response |
| Tool | tool.call、tool.result |
| Artifact | artifact.persist |

未处理的 Skill 或 Coordinator 异常也会生成终态，不再只留下开始节点。

## 5. User/Conversation/Dialogue 目录

SessionLog 不再把内部 session_slot_id 当成业务会话。当前投影是：

    sessionlog-root/
      by-user/
        <user_id>/
          <client_id，也就是 conversation_id>/
            <dialogue_id>.jsonl

每个 Dialogue 独立一个 JSONL 文件。第一行是 dialogue.header，保存 User、Conversation、Dialogue、Agent 和代码版本身份；后续均为该 Dialogue 的事件。

Bridge 精确使用 user_id、client_id、dialogue_id 定位文件，不会因不同用户碰巧使用相同 dialogue_id 而写错文件。仍保留对旧平铺、sessions 和 conversations 布局的只读兼容。

## 6. 并行建树关联

并行树的关键连接键已经显式保存：

- coordinator.decision → skill.dispatch：Tool Call 和 batch_id。
- skill.dispatch → skill.start：exec_skill_key。
- skill.start → LLM/Tool：Skill Span 和 execution identity。
- Tool Call → Tool Result：tool_call_id。
- 各 skill.end → batch.result：exec_skill_key 和 batch_id。
- batch.result → 下一次 coordinator.activation：completed_batch_key。
- coordinator completion → client.delivery：dialogue_id 和候选终态。
- Artifact → 产生它的 Tool/Skill：内容身份、路径、存储终态和源事件。

Batch 结果按派发顺序保存分支，但各分支事件仍保留真实并发时间线。

## 7. 状态采集修正

轨迹中明确区分：

- Skill 业务 status。
- 单分支 workflow_success。
- Batch 业务 status。
- Batch workflow_status。
- Dialogue 候选 status。
- Bridge 实际 delivery status。

尤其是 batch.result 的 workflow_status 从 results[].workflow_success 计算，不能被某个 Skill 的业务失败覆盖。原始 status 继续表达业务聚合。

## 8. Artifact 与消息事实

当前采集：

- 完整 Tool Call 参数和最终 Tool Result。
- after_tool Hook 是否改写及改写结果。
- 跨 Coordinator 轮次累计的 Artifact。
- 每个 Artifact 的内容哈希、来源和持久化终态。
- dialogue-scoped 的 append-only messages.snapshot。
- Coordinator 候选答案和 Bridge 最终交付分别记录。

输入文件引用不会被误记成新生成 Artifact，最终 LLM 文本中的任意路径也不再作为 Artifact 来源。

## 9. Reader 能力与边界

SessionLog Reader 用于演示、校验和回放采集结果，不是正式轨迹树数据库。它可以检查：

- Header 数量和 Dialogue 一致性。
- 同一进程内 seq 单调且不重复。
- source_event_seqs 是否存在。
- LLM request/response 是否配对。
- Tool Call/Result 是否配对。
- 已派发 Skill 是否有终态或 Batch 兜底。
- Dialogue start/end 是否成对。
- 并行 Batch 图是否能正确汇合。

后续 Protobuf 轨迹树应消费这些事实，而不是把 Reader 本身扩展成生产建树存储。

对应变更主要见 CHG-002、008、009、011、012、013、014、018、019、023、024、025、026、027、028、030、032、043、044、046、048、050、052、053、056、057、058、059、060、061、062、063、064、065、066、069、070、073、074、075、076、079、109、117、119、120。

