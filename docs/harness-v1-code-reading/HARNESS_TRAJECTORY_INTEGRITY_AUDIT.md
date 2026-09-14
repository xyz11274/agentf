# Harness v1 流程与轨迹完整性审计

> 审计基线：分支 harness_agent_poc_evo，提交 118c4be
>
> 审计日期：2026-08-25
>
> 目标：在轨迹采集、轨迹树、Skill 编排进化、Prompt 自进化和 Loop 结束条件优化开始前，确认 Harness v1 产生的执行事实是否正确、完整、可关联、可回放。

## 0. 结论先行

当前 Harness v1 可以作为功能实验原型，但还不能直接作为“无损轨迹真相源”。

最需要先处理的不是字段格式，而是九类会改变事实或丢失因果关系的问题：

1. Skill 的语义恢复分支缩进错误，正常情况下根本不会重试。
2. Skill 的逻辑失败会以进程退出码 0 结束，C++ 随后把它标为 skill_success=true。
3. dispatch key 是唯一的，但完成回传丢掉它；同批 skill-runner 的 task_id 还可能相同。
4. 并行 Skill 的终端输出写入同一 dialogue 共享 key，结果、业务 Skill 名和产物可能串线。
5. 处理一个 Skill 完成事件时会清空同一 Coordinator task 的其余 DataExchange 消息，可能误删兄弟 Skill 完成事件。
6. LLM trace index 使用无 CAS 的“读列表—追加—覆盖”，并发写时会丢索引项。
7. emit_event 文档声称事件进入 NDJSON，实际 span_event 只写 stderr；工具调用无法从 NDJSON 完整回放。
8. LLM response 先被 Recorder 保存，after_llm hook 后执行逻辑才使用可能已被原地改写的对象，记录值与真实执行值会分叉。
9. C++ 只在 found_next_skills 分支检查 pending_skills_count；next_skills 被清理后，最终响应路径可以绕过并行 Skill 屏障。

因此，现阶段不能仅凭 Coordinator messages、AF_SKILL_SUCCESS 或 Python span NDJSON 构建轨迹树。至少要先修复 P0 项，或者在采集层明确标注“不可判定/可能串线”，禁止把不可靠记录当作训练标签。

## 1. 审计范围和证据等级

本次只审计当前 Harness v1 主链：

- packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
- packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py
- DataBus、Recorder、Trace、Hook、Skill loader、Tool registry 和 Artifact pipeline
- Coordinator/Skill 交界处必需的 C++ 调度、工作流完成和响应屏障代码
- 对应 workflow JSON 和现有测试

不对无关的 C++ Runtime 模块做泛化问题扫描。下文出现 C++ 问题，都是因为它直接影响当前 Harness 的结果正确性或轨迹关联。

证据等级：

| 标记 | 含义 |
|---|---|
| 已确认 | 从当前控制流可直接推出，不依赖时序猜测 |
| 竞态风险 | 共享状态和合法交错已经存在，需要并发压力测试确认发生频率 |
| 协议债 | 当前功能可能能跑，但消息语义不适合作为回放或训练事实 |
| 观测缺口 | 执行可能正确，但轨迹无法完整证明或重建 |

说明：仓库提供的 AGENTS.md 要求用 GitNexus 做代码查询，但当前工作区没有 .gitnexus 索引、对应 Skill 文件或可调用的 GitNexus MCP 工具。本审计使用本地源码、调用链和测试进行只读交叉验证。此次只新增文档，没有修改函数、类或方法。

## 2. 轨迹树必须满足的最小不变量

后续 Protobuf 轨迹如果要支持回放、收敛分析和自进化，至少需要以下不变量：

| 编号 | 不变量 |
|---|---|
| I-1 | 每一次 Coordinator 激活、Skill dispatch、Skill execution、LLM call、Tool call 都有稳定且唯一的实例 ID |
| I-2 | DispatchRequested、DispatchAccepted、SkillStarted、SkillCompleted 必须一一可关联 |
| I-3 | 传输成功、工作流成功、业务逻辑成功是三个字段，不能互相替代 |
| I-4 | 每个事件必须有真实 source；用户消息、内部完成事件、工具结果不能共用 user 语义 |
| I-5 | 事件存储必须追加式或具备 CAS，不能通过共享 key 覆盖另一个并发执行 |
| I-6 | before/after hook 改写前后的值都可审计，回放使用“生效后的值” |
| I-7 | 最终响应事件必须对应真正交付给客户端的内容，而不是 Coordinator 的候选答案 |
| I-8 | 每次 dispatch、LLM、Tool、Skill 都必须有明确开始、结束、状态和 stop reason |
| I-9 | 工作流声明 schema 与实际输出完全一致，并带 schema_version |
| I-10 | 任意一个索引丢失、重复事件、未知父节点、终态冲突都能被校验器发现 |

当前代码对 I-1、I-3、I-4、I-5、I-6、I-7、I-8、I-9 均存在确定问题或高风险缺口。

---

## 3. P0：轨迹采集前应优先修复

### P0-01　Skill 语义恢复正常情况下不会重试

- 状态：已确认
- 位置：
  - packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py:753-794
  - 现有测试只验证 validate_tool_calls 和 remove_invalid_tool_turns：
    examples/agent/outfit-assistant/tests/test_skill_executor_recovery.py:12-65
- 触发：LLM 返回 finish_reason=length、截断的 tool arguments 或非法 JSON。

当前逻辑把恢复 prompt 追加到 messages，并扩大 completion_budget；但 continue 位于 response_push 的 except 内：

    try:
        databus.response_push(...)
    except Exception:
        continue

当 progress push 成功时，代码直接落入 tool_call_error 输出并 return。只有 progress push 自身抛异常时才 continue 重试。这与注释和配置 max_recovery_attempts 的语义完全相反。

轨迹污染：

- 一次本应为 recoverable 的模型输出被记录为终态 tool_call_error。
- recovery_attempts、扩大后的 token budget 和 recovery prompt 看似发生，实际上没有第二次 LLM call。
- 用该数据优化 Loop 结束条件时，会错误地把“控制流 bug”学习成“模型无法恢复”。

修复建议：

- 把 continue 放到 try/except 之后、recovery_attempts 分支内部。
- 在重试前调用 remove_invalid_tool_turns，明确保存 recovery_requested 事件。
- 恢复耗尽时再输出 tool_call_error。
- 所有出口统一通过一个 finalize_skill_result 函数关闭 span、触发 hook、写结果。

必须新增测试：

1. progress push 成功：第一次非法、第二次合法，断言 call_llm 调用两次。
2. progress push 失败：仍然调用第二次 LLM。
3. 达到恢复上限后才输出 tool_call_error。

### P0-02　逻辑失败被 C++ 标成 Skill 成功

- 状态：已确认
- 位置：
  - Skill 错误出口：
    skill_executor.py:476-507、516-543、668-698、714-739、753-794、840-904、1082-1103
  - Shell wrapper 直接 exec Python：
    frontend/bridge/templates/agent_template/harness_agent/nodes/skill_executor.sh:21-26
  - Worker 以退出码判断节点成功：
    src/runtime/worker/worker.cpp:363-379
  - Workflow 完成只依据 failed_nodes：
    src/runtime/runtime.cpp:1540-1566、1701
  - Coordinator trigger 再把 workflow_completed 映射成 skill_success=true：
    src/agent/agent.cpp:1863-1876
- 触发：node_timeout、interrupted、llm_error、tool_call_error、tool_call_loop_detected、max_rounds_reached 等。

这些分支打印带失败 status 的 JSON 后正常 return，Python 退出码仍是 0。因此：

    output.status = llm_error
    process exit = 0
    workflow = workflow_completed
    AF_SKILL_SUCCESS = true

Coordinator 的 build_skill_completion_message 又依据 AF_SKILL_SUCCESS 选择 success/failure decision prompt，而不是解析嵌套 output.status。

轨迹污染：

- 同一次执行同时拥有“逻辑失败”和“成功”两个冲突标签。
- 成功率、收敛率、无效 Loop 数都会被系统性高估。
- Prompt/Skill 编排优化会把失败结果当正样本。

修复建议：

- 不建议简单让所有逻辑失败 exit 1，因为当前失败节点路径可能跳过 stdout JSON 解析。
- 定义单一、显式的 SkillOutcome：
  - transport_status
  - workflow_status
  - logical_status
  - stop_reason
  - retryable
- Runtime 完成回传必须携带 logical_status；Coordinator 的 success 判断以它为准。
- AF_SKILL_SUCCESS 只保留兼容字段，并由 logical_status 派生。

### P0-03　唯一 dispatch 身份在完成回传时丢失，task_id 也不能区分并行实例

- 状态：已确认
- 位置：
  - 唯一 dispatch key 生成：
    coordinator_decide.py:217-247
  - exec_skill_key 被传入 Skill：
    src/agent/agent.cpp:2840-2851、2883-2886
    src/runtime/runtime.cpp:2091-2095
  - Runtime task_id 构造未包含 exec_skill_key：
    src/agent/agent.cpp:1230-1256、2840-2843
  - Runtime 持有 complete_skill_task_id 但完成消息未写入：
    src/runtime/runtime.cpp:1497-1505、1558-1565、1627-1702
  - Coordinator trigger 只转发业务名、结果和产物：
    src/agent/agent.cpp:1880-1904
  - Recorder meta 不保存 exec_skill_key、workflow execution id、worker id：
    recorder.py:97-117、379-396

同一个 run_skills 调用中的所有 C++ 规格名都是 skill-runner，传入 launch_skill_task 的 context.message_id 也相同。task_id 由模板 task、message_id 和 skill_name 组成，却没有 exec_skill_key，因此同批多个 skill-runner 实例可能得到相同 self_task_id。

与此同时：

- dispatch key 唯一，但 Python 输出不带它。
- Runtime 有 unique execution_id，但没有传给 Python，也没有放入完成 metadata。
- workflow_instance_id 是调度池实例，不等同于每次 workflow execution。
- complete_skill_task_id 被保存却未用于完成关联。

轨迹污染：

- 同名 Skill 多次执行、同一批不同业务 Skill、同一 Skill 不同 prompt 都无法可靠 join。
- 轨迹树只能按“最近的业务名/时间”猜父节点，遇到并发会产生错误边。
- Protobuf 即使能序列化，也是在稳定地序列化歧义。

修复建议：

- 以 dispatch_id 或 exec_skill_key 作为跨层主关联键。
- task_id 构造包含 dispatch_id 的安全短哈希。
- 完成消息必须携带：
  - dispatch_id
  - skill_task_id
  - workflow_execution_id
  - business_skill_name
  - framework_skill_name
- Recorder 每次 LLM call 保存上述身份和 coordinator_activation_id。

### P0-04　并行 Skill 共享终端输出 key，存在结果串线

- 状态：竞态风险，源码已证明共享命名空间；需要压力测试量化
- 位置：
  - build_framework_key 明确忽略 workflow_id：
    src/runtime/runtime.cpp:106-119
  - 所有无下游节点输出发布到该共享 key：
    src/runtime/runtime.cpp:1193-1293
  - Skill 完成时重新从共享 skill_result、skill_name、artifacts_json 读取：
    src/runtime/runtime.cpp:1643-1691
  - Python 还额外写固定的 skill_result:skill-runner：
    skill_executor.py:1065-1067 及各错误出口

两个并行 skill-runner 都会写：

    /session/{sid}/sys/{dialogue}/skill_result
    /session/{sid}/sys/{dialogue}/skill_name
    /session/{sid}/sys/{dialogue}/artifacts_json
    /session/{sid}/sys/{dialogue}/skill_result:skill-runner

Runtime 完成时不是从该 execution 的不可变输出对象取值，而是再次读取这些共享 key。合法交错如下：

    A 发布 result=A
    B 发布 result=B
    A 进入 workflow completion，读取共享 result
    A 得到 B

轨迹污染：

- 完成事件可能带着另一个 Skill 的结果、业务名或产物。
- Coordinator 会基于错误 Skill 结果决策。
- 这是执行正确性问题，不只是 trace 问题。

修复建议：

- 不要全局修改 build_framework_key，因为 next_skills 等控制面确实需要跨工作流共享。
- 将数据面结果改成 execution-scoped：

    executions/{dispatch_id}/result
    executions/{dispatch_id}/skill_name
    executions/{dispatch_id}/artifacts

- Runtime 直接从已完成 node 的 output snapshot 构造 completion，避免二次共享读取。
- 共享控制 key 与执行结果 key 使用不同 API/类型，禁止混用。

### P0-05　清空 DataExchange 队列可能删除兄弟 Skill 完成事件

- 状态：竞态风险，清空行为已确认；需要双 Skill 同时完成测试
- 位置：
  - Skill 完成处理后调用 clear_data_exchange_messages：
    src/agent/agent.cpp:1863-1923，重点为 1913-1919
  - clear 实现是无条件 deque.clear：
    src/agent/task.cpp:183-207
  - 队列一次只 pop 一个再调用 handler：
    src/agent/task.cpp:293-307
  - 现有测试只覆盖“清理旧 Coordinator WORKFLOW_COMPLETE”，未覆盖兄弟 Skill：
    tests/unit/src/test_agent_gtest.cpp:700-755
    tests/e2e/system_agent_mock/test_stale_workflow_completion_e2e.cpp:1-32

这段清理是为修复旧 Coordinator completion 消费新 response 的历史 bug，但它没有按 workflow_execution_id、消息类型或 generation 过滤，而是清空全部待处理 DataExchange 消息。

若 Skill A 和 B 几乎同时完成：

1. A、B completion 都进入 Coordinator task 的 data_exchange_messages_。
2. 处理 A 时 pending 从 2 降到 1。
3. clear_data_exchange_messages 删除尚未处理的 B。
4. B 不会再触发 Coordinator，pending 也可能永远保持 1。

结果可能是挂起、丢结果，或后续某个无关事件才打破等待。

修复建议：

- 删除“全清空”策略。
- completion message 必须带 generation/coordinator_activation_id/workflow_execution_id。
- 只丢弃被证明属于旧 Coordinator generation 的消息。
- 对 SkillCompleted 采用幂等事件 ledger；先落事件再推进 pending。
- pending 不应是可漂移计数器，改成 dispatch_id 集合：

    pending = accepted_dispatch_ids - terminal_dispatch_ids

### P0-06　LLM trace index 并发更新会丢项

- 状态：已确认存在非原子读改写；丢项是竞态风险
- 位置：packages/agentflow-runtime/src/agentflow_runtime/recorder.py:427-438

每个进程执行：

    index = get(...)
    index.append(call_id)
    put(index)

并行 Skill 进程可能都读取同一旧列表，后写者覆盖先写者。traces/{call_id}/request、response、meta 可能仍存在，但枚举入口丢失，Trace Puller 和轨迹构建器看不到它。

轨迹污染：

- LLM call 数少于真实值。
- 某些 Tool Loop 看似跳轮或没有决策来源。
- 平均无效 Loop 次数会被低估。

修复建议：

- 最优：每个 call_id 自包含存储，通过 prefix scan 枚举，不维护共享 JSON 数组。
- 或 DataBus 提供 atomic append/CAS。
- 若必须有顺序索引，使用 dialogue-local 单调 sequence，由一个 writer 分配。
- 轨迹校验器比较 prefix scan 集合与 index 集合，发现缺口立即报警。

### P0-07　emit_event 没有进入 NDJSON，工具轨迹不可完整回放

- 状态：已确认
- 位置：
  - observability.py 声称 NDJSON 是真相源：
    packages/agentflow-runtime/src/agentflow_runtime/observability.py:3-16、49-79
  - span_event 只构造事件并输出 stderr：
    packages/agentflow-runtime/src/agentflow_runtime/trace/emit.py:87-118
  - 代码注释明确要求调用方自己 sp.add_event，但 emit_event 调用点没有 span 引用。

span_event 没有保存活动 TraceSpan 对象，也没有 append 到其 events。它无论是否存在活动 span 都只执行 stderr preview。

影响：

- coordinator.trigger、skill_executor.tool_call、错误、超时、重试等大量 emit_event 事件不在 NDJSON。
- Tool call/result 主要只存在 messages 或 stderr；若工具执行后进程崩溃、尚未进入下一轮 LLM，请求记录也无法补回。
- stderr 没有稳定 schema、事件 ID、parent ID 和完整结构，不适合成为可回放真相源。

修复建议：

- SpanContext 栈保存 span 对象或由全局 event writer 直接写 append-only event。
- Tool 必须有独立 ToolCall span/event，至少保存：
  - tool_call_id
  - tool_name
  - original_ordinal
  - normalized_args
  - args_hash
  - started_at/ended_at
  - executor/sandbox
  - result/status/error/artifacts
- 测试 emit_event 后读取 NDJSON，必须能找到该事件，而不是只检查 stderr。

### P0-08　Recorder 保存的模型响应与 Hook 后真正执行的响应可能不同

- 状态：已确认协议分叉
- 位置：
  - Recorder 在 call_llm 返回前保存 response：
    recorder.py:337-396
  - Coordinator after-LLM hook：
    coordinator_decide.py:626-630
  - Skill Executor after-LLM hook：
    skill_executor.py:661-666
  - Hook payload 持有 raw choice 的同一 dict 引用：
    hooks/engine.py:27-64

after_llm hook 的返回 payload 在两个调用点都被忽略，但 handler 可以原地修改 raw_choices 指向的对象。后续执行读取已被修改的 raw，而 Recorder 已经保存修改前的 HTTP response。

轨迹污染：

- 回放 Recorder response 得到的分支与线上真实分支不同。
- 无法判断决策来自模型还是 Hook。
- Prompt 进化可能把 Hook 的效果归功给 Prompt。

修复建议：

- 保存 llm_response_raw 和 llm_response_effective 两个版本。
- HookEvent 记录 handler、输入哈希、patch、输出哈希。
- 调用点必须使用 trigger 的显式返回值，禁止依赖引用别名的隐式原地修改。
- replay 默认读取 effective 版本，同时允许审计 raw 版本。

### P0-09　最终响应屏障依赖 next_skills 存在，可能绕过 pending

- 状态：竞态风险
- 位置：
  - 只有 found_next_skills 分支检查 pending：
    src/agent/agent.cpp:2326-2381
  - no next_skills 分支直接进入最终交付：
    src/agent/agent.cpp:2473-2545
  - Coordinator 内部 correction loop 会删除 next_skills：
    coordinator_decide.py:920-940

正常情况下，旧 next_skills 留在 DataBus 能让 C++ 等待兄弟 Skill。但这个屏障依赖“控制 key 尚未被删”。如果某次完成触发后，模型先产生重复/非法 run_skills，Python correction 分支删除 next_skills，随后在同一激活中输出 final，C++ 走 no next_skills 分支，不再检查 pending_skills_count。

轨迹污染：

- 客户端收到的 final 可能早于兄弟 Skill 完成。
- Python 已触发 AGENT_DIALOGUE_END，但后台仍有 Skill。
- 后到的 completion 会形成“final 后又出现节点”的非法轨迹。

修复建议：

- finalization gate 独立于 next_skills：

    can_deliver_final =
        pending_dispatch_ids is empty
        and coordinator_output_is_final
        and no_unconsumed_completion_events

- next_skills 只表示新调度请求，不能兼任 pending ledger。
- 增加“final candidate”和“final delivered”两个事件。

---

## 4. P1：会显著降低轨迹语义质量或实验可信度

### P1-01　Skill 完成结果被伪装成 role=user

- 状态：协议债，用户观察正确
- 位置：coordinator_decide.py:462-474

完成结果通过：

    messages.append({"role": "user", "content": build_skill_completion_message(...)})

重新送给 Coordinator。

它不是普通用户消息。采用 user 的现实原因是当前 run_skills 是异步调度：

- 原 assistant tool_call 需要立刻收到一个 tool result 才能闭合消息协议。
- 当前 tool result 只是 dispatch ACK。
- 真正 Skill 结果跨进程、晚到，而且一次 run_skills 可启动多个 Skill，难以直接映射为传统的一问一答 tool result。

但把内部结果提升为 user 权限会造成：

- 轨迹把系统生成内容误判成用户输入。
- Skill/网页/工具返回的提示注入获得 user 级语义。
- 对话轮数、用户追问数和训练 role 标签失真。

建议：

- 内部轨迹使用 SkillCompleted 事件，不先压成 ChatCompletion role。
- 若模型 API 只接受标准 role，可在适配层生成 role=tool，并使用 dispatch tool_call_id；多 Skill 应拆为多个独立 tool call。
- 至少增加 source=skill_completion、trust=untrusted_tool_output，采集时禁止计入 user_turn。

### P1-02　run_skills 的 role=tool 内容只是调度 ACK，不是执行结果

- 状态：协议债，不是简单的“写错一句话”
- 位置：
  - handle_run_skills_call 返回 dispatched：
    coordinator_decide.py:207-258
  - 该字符串立即作为 role=tool：
    coordinator_decide.py:855-859

这句“已调度 N 个 skill”是异步协议的 DispatchAccepted，不是 SkillCompleted。当前消息历史却没有类型系统区分二者。

问题：

- ACK 没有返回 dispatch key，无法与后续结果关联。
- DataBus put 失败也仍返回 status=dispatched。
- 一个 ACK 对应多个 Skill，后续又逐个完成。
- 传统 tool-call 轨迹解析器会误把 ACK 当工具最终结果。

建议：

- 轨迹事件显式分为 DispatchRequested、DispatchAccepted、SkillStarted、SkillCompleted。
- ACK 返回每个 dispatch_id 和持久化状态。
- Chat 消息只是事件投影，不作为底层真相源。

### P1-03　Dialogue 生命周期 Hook 实际是 Coordinator 激活生命周期

- 状态：已确认
- 位置：
  - 每次 Python Coordinator 进程启动都触发 AGENT_DIALOGUE_START：
    coordinator_decide.py:354-361
  - 每次产生候选 final 就触发 AGENT_DIALOGUE_END：
    coordinator_decide.py:728-758

Skill 每完成一次都会重启 Coordinator，所以同一 dialogue 可以出现多个 start。并行 Skill 尚未完成时，某次 Coordinator 也可能先触发 end，之后再出现另一个 start。

建议：

- 改名为 CoordinatorActivationStarted/Ended。
- 真正 DialogueStarted 位于 Bridge/C++ 接收请求边界。
- 真正 DialogueEnded 位于 final delivered/interrupt delivered 边界。
- CoordinatorFinalResponse 改为 CoordinatorFinalCandidate。

### P1-04　Coordinator round 统计的是完成触发次数，不是编排决策轮

- 状态：已确认
- 位置：coordinator_decide.py:391-406、521-525

current_round 在每个 Skill completion trigger 上加一。一次并行调度 N 个 Skill，可能消耗 N 个 Coordinator round；但这并不等于 N 次新编排。文件内部还有独立的 tool_loop_round。

如果直接用 current_round 计算“无效 Loop”：

- 并发度越高，表面 Loop 越多。
- completion arrival 次序会改变 round 数。
- max_iterations 可能被完成事件而非决策消耗。

建议独立记录：

- coordinator_activation_index
- coordinator_decision_index
- dispatch_batch_id
- skill_completion_index
- internal_tool_correction_index

### P1-05　只自动附加最近一次完成 Skill 的 artifacts

- 状态：已确认
- 位置：
  - build_skill_completion_message 不单独聚合 artifacts：
    coordinator_decide.py:261-284
  - final append 使用本次 AF_SKILL_ARTIFACTS：
    coordinator_decide.py:725-727、782

较早 Skill 的 result JSON 里可能仍嵌有 artifacts_json，因此并非完全不可见；但最终自动附件逻辑只处理触发本次 Coordinator 的那一个 completion。最后完成的 Skill 决定哪些产物被稳定附加给用户。

建议：

- 维护 executions/{dispatch_id}/artifacts。
- final 时按所有 terminal dispatch 聚合、去重和排序。
- Artifact 节点必须属于对应 Skill execution，而不是挂在 dialogue 的“最后结果”上。

### P1-06　Dispatch manifest 和 next_skills 写入非事务，返回值被忽略

- 状态：已确认
- 位置：
  - coordinator_decide.py:217-258
  - DataBusClient 失败返回 False：
    databus_client.py:98-109、146-162

可能状态：

- manifest 已写、next_skills 未写：孤儿 dispatch。
- next_skills 已写、部分 manifest 未写：Skill 启动后找不到输入。
- last_prompt 写失败：判重状态漂移。
- 所有写失败：仍向模型返回 dispatched。

建议：

- 先写一条完整 DispatchBatch 记录，状态 PREPARED。
- 校验每个 manifest 成功后原子切换 ACCEPTED。
- 调度器只消费 ACCEPTED。
- ACK 只在全部必要写成功后返回；失败返回结构化、可重试错误。

### P1-07　run_skills 参数缺少运行时验证

- 状态：已确认
- 位置：coordinator_decide.py:794-845、handle_run_skills_call:214-227

JSON 解析失败被降级为 {}，但后续没有验证：

- skills 是否为非空 list
- spec 是否为 dict
- name 是否存在且属于可用 Skill
- prompt 是否为 string
- load_skills 是否为合法列表
- force 是否为 bool

JSON-valid 但 schema-invalid 的模型输出可在 spec["name"] 处抛异常，使整个 Coordinator 节点失败。

建议：

- 使用一份版本化 DispatchRequest schema 做强校验。
- 校验失败作为 role=tool correction result 回灌，不崩进程。
- 轨迹保存 validation_error 和原始 arguments。

### P1-08　Hook 阻断的 Skill 仍被统计为已调度

- 状态：已确认
- 位置：coordinator_decide.py:823-845、867-919

Skill 名先加入 _dispatched_skills，随后 before_skill_dispatch hook 才可能 blocked。阻断后只构造 skipped_by_hook 的 tool result，却没有从 _dispatched_skills 移除。

因此代码会：

- 发出 coordinator.skills_dispatched。
- 保存“已调度”进度。
- return 等待 Skill。
- 实际没有写 next_skills，也没有启动 Skill。

建议：

- requested_skills 与 accepted_skills 分开。
- 只有 handle_run_skills_call 成功返回并持久化后才加入 accepted。
- hook block 走内部 correction loop，不能 yield 等待不存在的 Skill。

### P1-09　重复调度守卫只记住最近完成的 Skill

- 状态：已确认
- 位置：coordinator_decide.py:180-204、381-389、804-817

守卫只比较当前 completed_skill 和每个业务名的 last_prompt。它没有：

- 全 dialogue completed dispatch 集合
- 当前 pending dispatch 集合
- 每个业务 Skill 的 prompt 历史
- 同名并发执行的独立状态

完成 A、再完成 B 后，A 可以用同 prompt 被再次调度，因为当前 completed_skill 已变成 B。last_prompt 还是单值覆盖。

建议：用 dispatch ledger 判定，不使用“最近完成名称 + 最近 prompt”模拟状态机。

### P1-10　缺失 manifest 时静默退化为 unknown-skill 并继续执行

- 状态：已确认
- 位置：skill_executor.py:330-370

exec_skill_key 缺失、DataBus 不通、manifest 不存在或 JSON 解析失败时，执行器会回退到 unknown-skill，并可能使用原 user input 继续跑通用工具循环。

这会把“调度关联断裂”伪装成一次正常 Skill 执行，甚至输出 completed。

建议：

- 对 Harness v1 的 skill-runner 路径 fail closed。
- 输出 dispatch_manifest_missing/invalid，携带 exec_skill_key。
- unknown-skill 只能用于明确的兼容模式，不能自动启用。

### P1-11　Workflow JSON 声明与实际 Python 输出不一致

- 状态：已确认
- 位置：
  - skill workflow 声明 response/status/artifacts_json：
    frontend/bridge/templates/agent_template/harness_agent/skill_workflow.json:15-24
  - Python 实际输出 skill_result/status/skill_name/rounds_used/artifacts_json：
    skill_executor.py:1019-1025 等
  - coordinator workflow 声明 coordinator_status：
    coordinator_workflow.json:52-64
  - Runtime 会发布实际解析出的动态 key：
    src/runtime/runtime.cpp:1193-1293

当前能工作是因为 json_output 解析和 Runtime 动态发布了 Python 实际字段，并非声明契约正确。

风险：

- Rust 重写若按 workflow schema 实现，会漏 skill_result、skill_name、rounds_used。
- Protobuf adapter 不知道该相信声明还是运行值。
- 静态校验无法发现 ABI 漂移。

建议：定义 HarnessResult v1 schema，声明和输出完全一致，并在边界拒绝未知/缺失必填字段。

### P1-12　after_tool hook 改写结果不校验数量和类型

- 状态：已确认
- 位置：skill_executor.py:943-959

注释说“长度需与 tool_calls 一致”，代码只执行 list(_rewritten_results)，随后 zip。少结果会静默截断，多结果会被丢弃，元素不是三元组还会解包崩溃。

少一个 Tool result 会让下一轮 messages 出现 assistant 声明了多个 tool_calls、实际缺少对应 tool message 的非法协议。

建议：

- 强校验 cardinality。
- 按 tool_call_id 映射，不按列表位置。
- 对每个 result 校验 name、args、result、status。
- Hook patch 非法时保留原结果并记录 hook_error。

### P1-13　Hook 原地追加 Artifact 不会回写 artifacts_json

- 状态：已确认
- 位置：
  - AgentContext 文档明确支持 artifacts.append：
    context.py:17-32
  - 测试也只验证 list 已被 append：
    tests/test_hooks.py:337-349
  - Skill 输出只在 list 对象身份变化时重序列化：
    skill_executor.py:872-888、1048-1064

ctx.artifacts 和 generated_artifacts 默认是同一个 list。Hook 调用 append 后对象身份不变，于是：

    if _completed_ctx.artifacts is not generated_artifacts:

为 False，output["artifacts_json"] 仍是 hook 前序列化的字符串。Hook 注入的产物丢失。

建议：

- Hook 后无条件从 ctx.artifacts 重新规范化、去重并序列化。
- 校验 blocked 语义；当前 before_skill_complete 的 ctx.blocked 也未被处理。
- 增加完整 execute 集成测试，而不是只测 Hook engine。

### P1-14　空最终文本仍标记 completed

- 状态：已确认
- 位置：skill_executor.py:1020-1032

output.status 先固定为 completed。response_text 为空时只发 WARN，不改变状态。再叠加 P0-02，Coordinator 最终得到 success=true。

建议：空文本且无结构化成果时使用 empty_result/incomplete；若有 artifacts，使用 completed_with_artifacts，并显式记录没有自然语言 summary。

### P1-15　超时和最大轮次的“汇总”不是汇总

- 状态：已确认
- 位置：
  - node timeout 直接取 messages[-1].content：
    skill_executor.py:476-497
  - max rounds 同样取最后一条消息：
    skill_executor.py:1082-1094

最后一条可能是 Tool JSON、Hook 跳过提示或 recovery prompt。UI 文案说“正在汇总”，但没有任何总结过程。

轨迹树若把 skill_result 当最终回答，会混入内部协议消息。

建议：

- 保存 last_assistant_text、tool_observations、artifacts 和 stop_reason 的结构化快照。
- 超时不再伪装总结；由 Coordinator 根据结构化 partial outcome 决策。

### P1-16　Session 消息历史不是 dialogue-scoped，也不是长期轨迹存储

- 状态：已确认
- 位置：recorder.py:153-176

llm/session_messages 在一个 session 内只有一个值。C++ 当前通过 session 级串行锁避免不同 dialogue 同时运行，但每个新 dialogue 会覆盖上一个 dialogue 的内部历史；cleanup 后无法把它当历史轨迹库。

建议：

- 运行态消息快照使用 conversations/{dialogue_id}/effective_messages。
- 轨迹使用 append-only MessageAdded/MessageRewritten 事件。
- session_messages 只做兼容缓存，不纳入训练真相源。

### P1-17　SpanWriter 不是 fail-open，且文件锁只在单进程内有效

- 状态：已确认前半，后半为可靠性风险
- 位置：
  - emit_span finally 无异常隔离地 write_span：
    trace/emit.py:70-82
  - SpanWriter 使用实例内 threading.Lock：
    trace/span_writer.py:43-48、68-89

如果 mkdir/open/write 失败，异常会从 context manager 的 finally 向上传播，观测路径可以反过来打断业务流程。多个 Skill 是多个 Python 进程，每个都有自己的 threading.Lock，它们不会互斥。

建议：

- 业务路径默认 fail-open，但同步上报 trace_write_failed 计数。
- 使用独立 trace writer 服务、每 execution 独立文件，或可靠的跨进程 append。
- 每条 event/span 带 checksum 和 schema_version，读取时检测半行与损坏。

### P1-18　Recorder 身份注释已过时，且没有利用现有输入

- 状态：已确认
- 位置：
  - recorder.py:56-117
  - Runtime 已注入 self_task_id、workflow_instance_id、worker_id：
    src/runtime/runtime.cpp:2119-2143
  - Worker 会把所有 inputs 转为 AF_*：
    src/runtime/worker/worker.cpp:176-180

Recorder 仍注释 AF_SELF_TASK_ID 永远为空，但当前代码已经注入。这说明身份契约缺少自动测试，也容易误导 Rust 迁移。

同时 Recorder 没有保存：

- AF_EXEC_SKILL_KEY
- AF_WORKFLOW_INSTANCE_ID
- AF_WORKER_ID
- call_llm 的 source、skill、round_i

注意：workflow_instance_id 是调度池实例，不是 Runtime 的 execution_id，仍不足以唯一标识一次执行。

建议：建立单一 RuntimeIdentity 对象，由所有 Recorder、Span、Hook 和结果输出复用。

---

## 5. P2：建议在 Rust 基线固化前清理

### P2-01　Tool registry 覆盖实现时保留重复 schema

- 状态：已确认
- 位置：tool_registry.py:205-226

同名 agent/skill tool 会覆盖 _modules[name]，但 _tools 继续 append 旧 schema。LLM 可能看到多个同名 function 定义，实际 execute 使用最后一个 module。

建议：schema 也按 tool name 建 map，最终一次性生成列表；轨迹记录 tool_definition_hash。

### P2-02　Skill 名和 load_skills 缺少路径/白名单约束

- 状态：已确认
- 位置：
  - skills_loader.py:62-106
  - tool_registry.py:88-92

Coordinator 的 enum 只是给模型的 schema 提示，不等于运行时安全校验。传入带路径分隔符的名字会参与 os.path.join。

建议：

- 只接受已注册 Skill ID。
- realpath 后验证仍位于 harness/skills 根目录。
- load_skills 同样验证。
- 轨迹中的 Skill ID 使用 registry canonical name，不使用未经验证的模型字符串。

### P2-03　LLM retry 统计永远为 0，HTTP 429/5xx 被统一视为不可重试

- 状态：已确认
- 位置：
  - recorder.py:260、349-363、379-396
  - openai_compatible_client.py:121-158、238-267

实际 retry loop 位于 OpenAiCompatibleClient 内部，但没有把 attempts 返回 Recorder；Recorder 的 attempt 永远是 0。所有 _ApiError，包括 429 和 5xx，都在第一次终止。

这会直接影响后续“无效 Loop/重试优化”的数据解释。

建议：

- client 返回 attempts、retry_reasons、http_status、data_received。
- 429/408/部分 5xx 按 provider 策略重试。
- 区分 transport retry、provider retry、semantic recovery、agent loop。

### P2-04　流式 usage 是估算值但没有 estimated 标志

- 状态：已确认
- 位置：openai_compatible_client.py:217-235

provider 不返回 usage 时，以字符数除 2 估算 completion token，prompt token 固定为 0。Recorder 随后把它当普通 usage 保存。

建议增加 usage_source=provider|estimated、estimator_version，成本/效率评测不得混用两者。

### P2-05　多 Skill progress 的 skill metadata 被拼成一个名字

- 状态：已确认
- 位置：coordinator_decide.py:867-897

skill_labels 用中文顿号连接，之后用 split() 按空白拆分。多个 Skill 会作为一个组合字符串写入 skill 字段。

建议直接遍历 _dispatched_skills 的 key，不先拼接再拆分。

### P2-06　Tool skip 后的消息顺序可能偏离原始 tool_calls 顺序

- 状态：已确认
- 位置：skill_executor.py:921-987

被跳过的 Tool response 先全部 append，保留的 Tool 执行结果随后 append。混合 skip/execute 时，response 顺序可能不再等于原 assistant tool_calls 顺序。

建议按原 ordinal 构造完整结果 map，最后一次性按原顺序回灌。

---

## 6. 用户指出的两个“奇怪点”到底是什么

### 6.1 为什么 Skill 结果回 Coordinator 时是 user

它是当前异步调度协议的折中，不是用户真的又说了一句话。

真实事件顺序是：

~~~mermaid
sequenceDiagram
    participant C as Coordinator LLM
    participant P as Coordinator Python
    participant R as C++ Runtime
    participant S as Skill Executor

    C->>P: assistant tool_call run_skills
    P-->>C: role=tool: DispatchAccepted
    P->>R: next_skills + manifests
    R->>S: launch skill-runner
    S-->>R: SkillCompleted
    R->>P: re-enter Coordinator
    P-->>C: role=user: internal Skill completion
~~~

也就是说：

- role=tool 是调度确认。
- role=user 是晚到的真正执行结果。

它在功能上解释得通，但在语义、权限和轨迹上不正确。轨迹层必须恢复为四类独立事件，不能照抄 Chat role。

### 6.2 为什么 Coordinator 的 tool result 只有“调用了几个 Skill”

因为 run_skills 在 v1 中不是“执行工具”，而是“提交异步工作”。该 tool result 相当于消息队列的 accepted ACK。

问题不在于 ACK 文案短，而在于它没有：

- dispatch_id 列表
- 持久化成功证明
- 接受/拒绝明细
- 与后续 completion 的关联

因此建议保留 ACK 概念，但不要称其为 Skill result。

---

## 7. 轨迹采集在修复前的临时解析规则

如果轨迹采集必须先于修复启动，采集器至少应做以下规范化，防止把现有协议表象当真：

| 当前信号 | 临时解释 |
|---|---|
| assistant.run_skills tool_call | DispatchRequested |
| role=tool 且 status=dispatched | DispatchAccepted，不是 Skill result |
| Coordinator 中 completion template 的 role=user | InternalSkillCompletion，不计 user turn |
| AF_SKILL_SUCCESS | 只表示 workflow/process success，不能表示 logical success |
| skill_result JSON 的 status | 暂时作为 logical status，但需保留与 AF_SKILL_SUCCESS 的冲突 |
| COORDINATOR_FINAL_RESPONSE | FinalCandidate，不是 FinalDelivered |
| C++ RESPONSE_SENT 且 final=true | FinalDelivered |
| emit_event stderr | preview only，不视为可靠持久事件 |
| traces/index | 非完备索引；必须额外 prefix scan |
| skill_result/skill_name 共享 key | 并发时标记 correlation_confidence=low |

无法通过采集器补救的部分：

- 丢失的兄弟 completion 消息。
- 已被共享 key 串线且没有 dispatch_id 的结果。
- Hook 改写后未记录的 effective response。
- 工具完成后、下一轮 LLM 前进程崩溃导致的结果缺失。

这些必须修执行链，不能靠离线推断恢复。

## 8. 建议的 Protobuf 事件骨架

至少需要以下身份和状态维度：

    message TrajectoryEvent {
      string schema_version;
      string event_id;
      string parent_event_id;

      string trace_id;
      string session_id;
      string dialogue_id;
      string coordinator_activation_id;
      string dispatch_batch_id;
      string dispatch_id;
      string skill_task_id;
      string workflow_execution_id;
      string node_run_id;
      string llm_call_id;
      string tool_call_id;

      uint64 dialogue_sequence;
      int64 wall_time_us;
      int64 monotonic_time_us;

      EventType event_type;
      EventSource source;
      ExecutionStatus status;
      StopReason stop_reason;
      TrustLevel trust;

      bytes payload;
      string payload_sha256;
    }

建议的核心事件：

1. DialogueStarted
2. CoordinatorActivationStarted
3. LlmCallStarted / LlmCallCompleted
4. HookApplied
5. DispatchRequested
6. DispatchAccepted / DispatchRejected
7. SkillStarted
8. ToolCallStarted / ToolCallCompleted
9. ArtifactProduced
10. SkillCompleted
11. CoordinatorFinalCandidate
12. FinalDelivered
13. DialogueCompleted / DialogueInterrupted

状态必须避免单 bool：

    transport_status
    process_status
    workflow_status
    logical_status
    retryable
    stop_reason

轨迹树的边应由 ID 构建，不应通过时间相邻、业务名相同或消息文本模板猜测。

## 9. 修复顺序

### 阶段 A：先保证执行事实正确

1. 修 P0-01 语义恢复。
2. 统一 P0-02 逻辑状态。
3. 给 dispatch/task/workflow completion 打通唯一身份。
4. 隔离并行 Skill 输出。
5. 用带 generation 的消息过滤替代全队列 clear。
6. final gate 独立检查 pending dispatch set。

### 阶段 B：再保证轨迹不丢

1. 改成 append-only event store。
2. 修 trace index 原子性或取消共享 index。
3. 让 emit_event 真正落盘。
4. 增加 Tool spans 和 Hook patch 记录。
5. 记录 raw/effective 两套 LLM response。

### 阶段 C：最后稳定可进化 schema

1. 对齐 workflow JSON 与实际输出。
2. 建立 Protobuf schema_version。
3. 明确 Chat messages 只是事件投影。
4. 补 Artifact 聚合和所有 stop reason。
5. Rust 版本与 Python 版本共用 golden replay cases。

## 10. 开始轨迹树实验前的验收测试

以下测试不全通过，不建议把数据用于 Skill/Prompt/Loop 自进化：

### 10.1 单 Skill

- 非法 tool JSON 后成功恢复，且事件严格为 request → recovery → request → success。
- llm_error、timeout、max_rounds、empty_result 均不被标成 logical success。
- 每个 terminal result 只有一个终态。

### 10.2 并行 Skill

- 同一 batch 启动 2、5、10 个 skill-runner，每个使用不同 dispatch_id。
- 同一业务 Skill 用不同 prompt 并行执行，结果不串线。
- 所有 completion 恰好一次到达 Coordinator。
- pending set 最终为空，不依赖完成顺序。
- final delivered 一定晚于所有非取消 dispatch 的 terminal event。

### 10.3 Trace 完整性

- 并发 100 次 LLM call，prefix scan、index 和期望集合完全一致。
- 每个 LLM call 有 request、raw response、effective response、meta。
- 每个 Tool call 有 start/end/result，进程在 Tool 后崩溃仍可找到已完成结果。
- 每个 span/event 的 parent 存在，或被明确标为 root/external parent。

### 10.4 Hook

- after_llm 原地改写和返回 patch 两种写法都产生相同 effective response。
- before_skill_complete append artifact 后，最终 artifacts_json 包含它。
- blocked dispatch 不产生 DispatchAccepted/SkillStarted。
- 非法 after_tool 结果不会生成缺失 tool response 的消息序列。

### 10.5 回放

- 用同一条轨迹重建 effective messages，逐轮 hash 与线上保存值一致。
- 重放的 Coordinator 分支与线上一致。
- 对任意 SkillCompleted 都能沿 parent/dispatch ID 回溯到唯一 DispatchRequested。
- FinalDelivered 内容 hash 与客户端收到内容一致。

## 11. 当前测试覆盖的关键空白

现有测试已经覆盖部分基础函数，但不足以证明轨迹完整性：

- recovery 测试只验证“识别出非法 tool call”，没有执行 execute 的第二次 LLM。
- hook 测试验证 AgentContext.artifacts 可以 append，没有验证 output artifacts_json。
- trace_emit 测试验证 span trace_id 和父子链，没有验证 emit_event 真正落入 span events。
- force/dispatch 测试验证 exec_skill_key 被生成，没有验证 completion 把它原样带回。
- Tool 并行测试验证执行时间重叠，没有验证同 dialogue 的 workflow output 隔离。
- stale completion 测试证明全队列 clear 能删除旧消息，没有验证它不会删除合法兄弟 Skill completion。
- 没有测试 logical status 与 AF_SKILL_SUCCESS 的一致性。
- 没有测试 no-next_skills final 分支仍受 pending 屏障约束。

## 12. 最终判断

Harness v1 的主控制思路是清晰的：Coordinator 做委派决策，skill-runner 执行业务 Skill，C++ Runtime 负责异步重入。但当前实现把“异步控制面”“Chat 消息投影”“共享 DataBus 状态”“Trace 真相源”混在了一起。

对后续工作最关键的判断是：

> 先建立唯一执行身份和追加式事件事实，再构建轨迹树；不要先从现有 messages 猜一棵树，再把猜测结果固化进 Protobuf。

修完 P0 后，Harness 才适合作为 Python 实验基线；完成 P1 的角色、Hook、Artifact 和 schema 收敛后，才适合作为 Rust 重写与自进化系统的长期行为基线。
