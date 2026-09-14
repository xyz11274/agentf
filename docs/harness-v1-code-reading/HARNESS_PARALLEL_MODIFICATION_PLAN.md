# Harness v1 并行执行修改方案

> 日期：2026-08-26
>
> 状态：已实施并通过定向回归与真实 Harness v1 hchat 验证
>
> 唯一目标：同一轮 run_skills 中的多个 Skill 可以正确并行，结果互不覆盖，所有 Skill 进入终态后只触发一次 Coordinator，且 final 不会提前发送。

实施说明：代码仍严格限定在 Harness v1 并行主链。通用 C++ Workflow Scheduler
没有改变；旧请求不携带 `batch_id` 时继续走原有路径，携带 `batch_id` 时使用
BatchState、execution-scoped 输出快照、ALL Join 和 Final Gate。

## 0. 修改边界

本方案只处理 Harness v1 的并行执行链：

    Coordinator dispatch
      -> C++ 启动多个 skill-runner
      -> 多个 Skill Workflow 并行
      -> C++ 收集每个 completion
      -> Batch 全部终止
      -> Coordinator 只重入一次
      -> 继续调度或 final

本轮不处理：

- Prompt 自进化、Skill 编排优化和 Loop 结束条件学习。
- 通用轨迹树、Protobuf 全量设计和 Rust 模块设计。
- 与并行无关的 Hook、Tool schema、LLM retry 和消息角色问题。
- ANY、QUORUM 或流式 Join。当前只实现 ALL Join。
- 同一 Batch 内的 Skill 依赖。B 依赖 A 时必须拆成两个 Coordinator Batch。

轨迹相关只保留并行关联必需的三个字段：

- batch_id：一次 run_skills 调用。
- exec_skill_key：Batch 中一次具体 Skill 执行。
- workflow_execution_id：C++ 中该 Skill Workflow 的真实执行实例。

## 1. 修改完成后的确定语义

假设 Coordinator 一次派发 A、B、C，最大并发数为 2：

1. Coordinator 创建一个 batch_id，并为 A、B、C 生成不同的 exec_skill_key。
2. C++ 先登记整个 Batch 的 expected 集合，再启动 A、B。
3. B 先完成：
   - 只保存 B 自己的输出；
   - B 从 active 进入 terminal；
   - 空出的槽位立即启动 C；
   - 不启动 Coordinator。
4. A 完成：
   - 只保存 A 自己的输出；
   - 仍有 C active；
   - 不启动 Coordinator。
5. C 完成：
   - Batch 的 expected 全部 terminal；
   - C++ 生成包含 A、B、C 的 Batch 结果；
   - 只启动一次 Coordinator。
6. Coordinator 一次性看到三个结果，决定下一批 Skill 或 final。
7. 重复到达的 B completion 只写诊断日志，不再次释放槽位、不再次触发 Coordinator。
8. 任一 Skill 失败、超时、取消或启动失败都算 terminal，不允许 Batch 永久等待。

最终约束：

    queued ∪ active ∪ terminal = expected
    三个集合两两不相交
    active.size <= max_concurrent_skills
    Coordinator 触发条件 = terminal.size == expected.size
    每个 Batch 的 Coordinator 触发次数 <= 1
    final 条件包含：不存在 incomplete Batch

## 2. 本方案修复的并行问题

| 编号 | 当前问题 | 直接结果 | 本方案处理 |
|---|---|---|---|
| P-1 | 同批 skill-runner 的 task_id 可能相同，completion 又不带 exec_skill_key | 不知道结果属于哪个 dispatch | 传播 batch_id、exec_skill_key、workflow_execution_id，并让 task_id 包含 exec_skill_key |
| P-2 | skill_result、status、skill_name、artifacts_json 使用 dialogue 共享 key | A completion 可能读到 B 的结果 | Runtime 保存 execution-scoped terminal output snapshot |
| P-3 | 处理一个 completion 后 clear_data_exchange_messages 清空整个队列 | 已排队的兄弟 completion 被删除 | 每次只消费已出队消息，不再清空兄弟消息 |
| P-4 | running/pending 使用标量计数，重复 completion 可重复扣减 | 负数、提前归零或永久不归零 | 使用 exec_skill_key 集合做幂等状态迁移，计数仅作兼容观测 |
| P-5 | 每个 Skill completion 都触发一次 Coordinator | 并发度被误算成 Loop，产生多次无效决策 | 部分完成只回填槽位；Batch 全终止后触发一次 |
| P-6 | no-next_skills final 分支没有独立检查未完成 Skill | final 后还有 Skill 和 Coordinator 活动 | 所有 final 出口统一检查 incomplete Batch |
| P-7 | 最后完成的 Skill metadata 只能携带自己的 Artifact | 同批早完成 Skill 的 Artifact 丢失 | Batch 结果保存每个 completion 的完整 artifacts_json |

不在本次并行修改中处理的问题：

- Skill 结果目前作为 role=user 注入。
- run_skills 的 role=tool 只是 dispatch ACK。
- LLM trace index 的并发追加。
- SpanWriter 的跨进程文件锁。
- Prompt、Hook、Tool、Artifact 的其他语义问题。

这些问题继续留在原问题清单中，不能借并行修改顺手扩展。

## 3. 最小协议变更

### 3.1 Coordinator 输出

coordinator_decide.py 在每次 run_skills 中生成一次 batch_id：

    batch_id = batch:{dialogue_id}:{dispatch_round}

next_skills 的每个元素增加同一个 batch_id：

    {
      "name": "skill-runner",
      "batch_id": "batch:D:R",
      "exec_skill_key": "dispatch:D:R:1",
      "force": false
    }

现有 exec_skill_key 继续作为单次 Skill 执行的权威 ID，不再引入 dispatch_id、attempt_id 等额外身份层。

### 3.2 Skill completion

Runtime 发给 source Coordinator 的每个 completion 必须携带：

    {
      "batch_id": "batch:D:R",
      "exec_skill_key": "dispatch:D:R:1",
      "skill_task_id": "...",
      "workflow_execution_id": "...",
      "framework_skill_name": "skill-runner",
      "business_skill_name": "outfit-stylist",
      "workflow_success": true,
      "skill_success": true,
      "skill_status": "completed",
      "skill_result": "...",
      "artifacts_json": "[]"
    }

skill_result、skill_status、business_skill_name 和 artifacts_json 必须来自该 workflow execution 的输出快照，禁止在完成时重新读取 dialogue 共享 key。

### 3.3 Batch completion

Batch 全部 terminal 后，C++ 生成一次 batch result。为避免把多个大结果塞进环境变量：

1. C++ 将 Batch 结果写入唯一 DataBus key：
   completion_batch:{session_id}:{dialogue_id}:{batch_id}
2. Coordinator trigger 只携带 completed_batch_key。
3. Python Coordinator 通过该 key 读取数组，并一次性构造当前激活的完成输入。
4. Batch 数据在 Coordinator 成功读取后标记 consumed；对话结束时清理。

该 key 只承担当前 Batch 的结果传递，不是通用轨迹存储。

## 4. C++ 为什么必须修改

下面四类信息只有 C++ 掌握，Python 无法可靠补救：

- workflow_execution_id。
- 一个 Skill Task 是否真正启动成功。
- 某个 Workflow 的终端节点实际输出。
- completion 队列、并发槽位和最终响应发送时机。

因此并行修复需要修改 C++，但只改 Harness 交界，不修改 Workflow Scheduler 的通用调度算法。

## 5. 具体修改点

### 5.1 Python Coordinator

文件：

- packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
- packages/agentflow-runtime/src/agentflow_runtime/common/runtime_context.py
- examples/agent/outfit-assistant/coordinator_workflow.json

修改：

1. handle_run_skills_call 每次调用生成 batch_id。
2. next_skills 每个 spec 写入相同 batch_id。
3. 新增 completed_batch_key 输入读取。
4. 一次性读取该 Batch 的全部结果；不能只使用最近完成的 Skill。
5. 一个 Batch 只增加一次 Coordinator round。
6. 保留当前消息角色和 Prompt 格式，避免把消息协议重构混入并行修改。
7. 读取失败时返回明确内部错误，不能把空 Batch 当作全部成功。
8. consumed 标记只有在结果成功解析并加入 messages 后写入。

Skill Executor 主循环不需要改并行控制，只需保证最终输出仍包含现有结构化字段；exec_skill_key 由 Runtime completion metadata 补齐。

### 5.2 C++ Runtime：输出隔离和身份回传

文件：

- include/internal/runtime/agent_runtime.h
- src/runtime/runtime.cpp

修改：

1. WorkflowExecutionState 增加 batch_id、exec_skill_key 和 terminal_outputs。
2. 创建 workflow execution 时，从 task 参数保存 batch_id 与 exec_skill_key。
3. 终端节点结束时，把该 execution 的输出复制到 terminal_outputs。
4. update_workflow_state 完成回传时：
   - skill_result/status/skill_name/artifacts_json 从 terminal_outputs 读取；
   - 不再从 build_framework_key 生成的 dialogue 共享 key重新读取；
   - metadata 增加 batch_id、exec_skill_key、skill_task_id 和 execution_id。
5. 旧 DataBus 输出可在兼容阶段继续双写，但旧共享 key 不再作为 completion 的权威读取源。
6. 不全局修改 build_framework_key，避免影响 Coordinator 跨 Workflow 通信和普通 Workflow。

这是 C++ 中最重要、也最不能由 Python 代替的一项改动。

### 5.3 C++ Agent：Batch 状态和 Join

文件：

- include/internal/agent/agent.h
- include/internal/agent/builtin_dispatcher.h
- src/agent/agent.cpp
- src/agent/builtin_dispatcher.cpp

新增 Harness 专用 BatchState，建议放在 MMAgent 内部并按 session_id、dialogue_id、batch_id 索引，避免把业务 Join 逻辑放进通用 MMAgentTask：

    BatchState
      expected_exec_keys
      queued_exec_keys
      active_exec_keys
      terminal_results
      coordinator_triggered

修改：

1. SkillSpec 解析并保留 batch_id。
2. C++ 读取 next_skills 后，必须先一次性登记 expected 集合，再启动第一个 Skill。
3. 启动前进行带锁槽位预留：
   - 未超 max_concurrent_skills；
   - exec_skill_key 尚未 queued、active 或 terminal。
4. build_runtime_task 的 task_id 增加 exec_skill_key 的安全后缀。
5. 启动成功：queued -> active。
6. 参数校验失败、Task 构建失败或 launch 失败：直接生成该 exec_skill_key 的失败终态。
7. completion 到达时执行幂等迁移：
   - 未知 exec_skill_key：记录并忽略；
   - 已 terminal：视为重复，记录并忽略；
   - active -> terminal：保存完整 completion，并释放一个槽位。
8. 释放槽位后直接从同一 Batch 的 queued 集合补启动，不调用 LLM。
9. expected 尚未全部 terminal：return，不能触发 Coordinator。
10. expected 全部 terminal 且 coordinator_triggered=false：
    - 写 completion_batch；
    - 原子设置 coordinator_triggered=true；
    - enqueue 一次 Coordinator trigger。
11. 删除两个 completion 分支中的 clear_data_exchange_messages 调用。
12. 旧 pending_skills_count 和 running_skills_count 迁移期继续更新日志，但不再参与 Join、去重或 final 判断。
13. 对话 cleanup 时删除对应 BatchState；迟到 completion 只能记录，不能重建已关闭 Batch。

### 5.4 Final Gate

位置：

- src/agent/agent.cpp 的 Coordinator found_next_skills 和 no-next_skills 两类响应出口。

统一规则：

    can_send_final =
      coordinator produced final
      and current dialogue has no incomplete Batch
      and current Batch completion has been consumed

实现一个共享判断函数，所有 final 出口调用同一函数。不能再通过 next_skills 是否存在推断是否仍有 Skill 在运行。

如果 final 被阻挡：

- 不发送 final。
- 不清理 dialogue。
- 记录 batch_id、queued 和 active 数量。
- 等 Batch 终态后由唯一 Coordinator trigger 继续流程。

## 6. 不采用的方案

### 6.1 只给 DataBus key 加 workflow_id

不采用。build_framework_key 同时承担跨 Workflow 控制数据，直接全局加 workflow_id 会破坏 next_skills、response 等现有通信。

### 6.2 继续使用 pending_skills_count 作为 Join

不采用。标量不能识别重复、迟到和未知 completion，也不能说明具体缺哪个 Skill。

### 6.3 每个 completion 都启动 Coordinator，让 Python 判断是否等待

不采用。这会把框架唤醒次数计入 Coordinator Loop，也会增加 LLM 调用和竞态窗口。

### 6.4 把 max_concurrent_skills 固定为 1

只可作为回退开关，不是并行修复。

### 6.5 顺便重构轨迹、Prompt 或消息角色

不采用。这些不属于并行执行正确性的最小闭环。

## 7. 实施顺序

### 阶段 A：身份和输出隔离（已完成）

1. 添加 batch_id 协议。
2. task_id 包含 exec_skill_key。
3. Runtime completion 回传三个身份。
4. 保存 terminal_outputs snapshot。
5. 先写双 Skill 乱序完成测试，证明 A 永远读 A、B 永远读 B。

完成 A 前不修改 Join，否则 BatchState 收到的仍可能是串线结果。

### 阶段 B：BatchState 和幂等 completion（已完成）

1. 注册 expected、queued、active、terminal。
2. 启动/终止状态迁移带锁。
3. 重复 completion 不产生副作用。
4. launch failure、workflow failure、timeout 都进入 terminal。
5. pending/running 标量降级为兼容指标。

### 阶段 C：槽位回填和单次 Join（已完成）

1. Batch 大于并发上限时只启动容量内 Skill。
2. completion 后立即回填 queued Skill。
3. 全部 terminal 后生成 batch result。
4. coordinator_triggered 保证只触发一次。
5. Python 一次性读取 Batch 结果，并只增加一次 round。

### 阶段 D：Final Gate 和清理（已完成）

1. 移除 completion 路径的全队列 clear。
2. 两类 final 出口统一检查 BatchState。
3. 对话结束清理 BatchState 和 completion_batch key。
4. 添加迟到 completion 测试。
5. 保留串行回退开关，进行 hchat 实测。

每个阶段独立提交和验证，不把四个阶段合成一次大改。

## 8. 必须新增的测试

### 8.1 Runtime 输出隔离

- A 写 result=A，B 写 result=B。
- 顺序覆盖：
  - A 输出、B 输出、A completion、B completion。
  - B 输出、A 输出、B completion、A completion。
- 两种顺序都必须返回各自结果、状态、业务名和 Artifact。

### 8.2 Batch 状态机

- Batch 2，max concurrency 2，倒序完成。
- Batch 5，max concurrency 2，验证槽位始终不超过 2 且会自动回填。
- 同一个 completion 到达两次，只 terminal 一次。
- 未知和迟到 completion 不改变 Batch。
- 参数校验失败、Task 构建失败、workflow_failed、timeout 都能完成 Join。
- 同名 Skill、不同 prompt、不同 exec_skill_key 不会合并。

### 8.3 Coordinator 触发

- 前 N-1 个 completion 不产生 Coordinator intent。
- 最后一个 completion 只产生一个 intent。
- 最后两个 completion 几乎同时到达仍只触发一次。
- 一个 Batch 只增加一次 Coordinator round。
- Batch 结果包含所有 Skill，顺序按原 dispatch 顺序，而不是完成顺序。

### 8.4 队列和 Final

- A、B completion 同时排队，处理 A 后 B 仍存在。
- next_skills 被删除时，只要 Batch 未完成，final 仍被阻挡。
- final 发送后再到迟到 completion，不会再次启动 Coordinator。
- 对话清理后 BatchState 和 completion_batch key 均不存在。

### 8.5 hchat 实测

构造一次至少调用两个 Skill、每个 Skill 至少调用一个 Tool 的请求，重复运行并人为改变完成顺序。验收日志必须显示：

- 不同 exec_skill_key、task_id 和 workflow_execution_id。
- 并发 active 峰值大于 1。
- 每个 exec_skill_key 恰好一个终态。
- Coordinator 在整个 Batch 完成后只重入一次。
- 客户端只收到一次 final。
- final 后没有新的 llm_chunk 或 Skill 启动。

## 9. 回退和兼容

实际实现采用协议级兼容，不增加新的全局开关：

- `next_skills` 携带合法 `batch_id` 时，使用 BatchState、execution-scoped 输出和
  ALL Join；批次 completion 只由新路径消费。
- 旧 `next_skills` 不带 `batch_id` 时继续走旧路径，不注册或猜测 Batch。
- `batch_id` 或批次身份不完整、无法建立可靠 Join 时回退旧路径并记录诊断，避免
  部分进入新协议后永久等待。
- 新 Batch 路径不执行旧 completion 分支的整队列清空；旧路径行为保持不变。
- 两条路径不会同时消费同一个 Batch completion，避免 Coordinator 重复重入。

## 10. 完成标准

并行修改完成必须同时满足：

1. 同批 Skill 的 task_id 和 completion 身份唯一。
2. 任意合法完成顺序都不会串 result、status、skill_name 或 Artifact。
3. completion 队列不会删除未处理的兄弟消息。
4. 重复、未知、迟到 completion 不会重复扣减或重复触发。
5. Batch 大于并发上限时能持续回填，不会饿死 queued Skill。
6. 失败和超时也能使 Batch 收敛。
7. 一个 Batch 只触发一次 Coordinator，只增加一次 Coordinator round。
8. final 一定发生在 Batch 全部 terminal 之后，且只发送一次。
9. 串行旧路径可以回退。
10. 定向 C++、Python 测试和真实 hchat 并行场景全部通过。

## 11. 实施结果

阶段 A 至 D 已形成完整闭环：

1. Python 为每次 `run_skills` 生成一个 `batch_id`，每项 dispatch 保留独立
   `exec_skill_key`。
2. C++ Runtime 将 `batch_id`、`exec_skill_key`、`skill_task_id` 和
   `workflow_execution_id` 贯穿到 completion，并从该 execution 的
   `terminal_outputs` 快照取结果。
3. Harness BatchState 先登记 expected，再按并发上限启动并回填；重复、未知和迟到
   completion 不产生状态副作用。
4. ALL Join 后按原 dispatch 顺序写入唯一 `completion_batch`，并只触发一次
   Coordinator。
5. Python 一次性读取整个 Batch，构造一条沿用现有格式的输入，并写入 `consumed`
   标志；Final Gate 在 Batch 未全终止或未消费时阻止 final。
6. Session interrupt 将 active 与 queued 项都收敛为明确失败终态；对话清理同时清除
   BatchState。

验证结果：Python Coordinator 13 项、C++ Harness Batch 3 项、Runtime 2 项和旧
Session interrupt 1 项定向测试通过。真实 Harness v1 hchat 同批启动
`outfit-stylist` 与 `forge-image-gen`，日志确认 active 峰值为 2、完成顺序与 dispatch
顺序不同、ALL Join 后只触发一次 Coordinator、消费前 Final Gate 保持阻塞，最终仅
发送一次 response。真实测试必须显式设置 `AF_COORDINATOR_VERSION=v1`；默认 v2
Coordinator 不经过本方案的 `run_skills` 路径。
