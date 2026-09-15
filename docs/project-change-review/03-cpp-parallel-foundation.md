# C++ AgentFlow 并行底座改动

## 1. 为什么这一部分必须改 C++

Python Coordinator 只能把 next_skills 写入 DataBus。真正创建 Task、控制并发槽位、消费 Workflow completion、阻止提前 final 和重新激活 Coordinator 的逻辑都在 C++ AgentFlow 底座中。因此并行闭环无法只靠 Python 正确实现。

主文件：

- include/internal/agent/agent.h
- src/agent/agent.cpp
- src/agent/builtin_dispatcher.cpp
- src/runtime/runtime.cpp
- src/runtime/worker/exe_loader.cpp
- src/runtime/worker/worker.cpp

## 2. 新增 Harness Batch 状态机

底座新增 HarnessBatchState，保存：

- session_id 和 dialogue_id。
- batch_id。
- 发起派发的 source_task_id。
- 按请求顺序保存的 dispatch_order。
- expected、queued、active 三组 exec_skill_key。
- 按 exec_skill_key 保存的 terminal_results。
- max_concurrent_skills。
- coordinator_triggered 幂等标记。

Batch 注册后会删除一次性的 next_skills/selected_skills，后续状态由 BatchState 持有，防止下一轮 Coordinator 重复注册同一个 Batch。

## 3. 有界并行

pump_harness_batch 按 max_concurrent_skills 启动分支：

- active 未满时按 dispatch_order 从 queued 取下一个分支。
- 一个分支终止后释放槽位并继续启动队列中的分支。
- 同一 Batch 中所有分支都可以独立成功或失败。
- 对话取消时，已排队但未启动的分支也会进入可解释的取消路径。

这不是一次性无上限并发，而是可配置的有界并行。

## 4. 每个分支使用独立身份

原始共享键会让并行 Skill 互相覆盖。当前跨层传递：

- exec_skill_key。
- batch_id。
- skill_task_id。
- workflow_execution_id。
- framework skill name。
- business skill name。

Runtime 结果键和 completion metadata 按执行身份区分，不再只依赖 dialogue 下的 skill_result、skill_name、artifacts_json 等共享名称。

## 5. Completion 消费和 ALL Join

原逻辑处理一个 completion 后可能清空兄弟 completion。当前 Batch 以 exec_skill_key 精确接收分支终态：

- 非预期 exec_skill_key 不计入当前 Batch。
- 同一分支重复 completion 不会重复扣减 pending。
- 一个 completion 只消费对应分支，不清空整个消息队列。
- terminal_results 数量等于 expected_exec_keys 数量时才执行 ALL Join。
- ALL Join 只触发一次 Coordinator。
- 聚合 payload 按原始 dispatch_order 输出，而不是按完成先后输出。

这同时解决结果丢失、返回顺序漂移和 pending 永不归零的问题。

## 6. Pending 屏障

最终回答路径现在必须服从 Batch pending 屏障：

- 只要仍有 expected 分支没有终态，Coordinator 的暂时 no-next-skills 不能结束整个 Dialogue。
- 内部 correction 删除 next_skills 也不能绕过屏障。
- 全部分支完成后才发布 completion_batch:<batch_id> 并重新激活 Coordinator。

因此不会再出现前端已收到 final，后台 Skill 随后继续返回新 chunk 的情况。

## 7. Workflow 与业务状态分离

当前每个分支同时保存：

- workflow_success：Python 节点是否按框架协议结束。
- skill_success：业务任务是否成功。
- skill_status：completed、llm_error、max_rounds_reached 等业务状态。
- skill_result：业务结果或业务错误说明。

Batch 还形成两个聚合轴：

- 业务聚合：true、partial、false。
- Workflow 聚合：completed、partial、failed。

Python 退出码为 0 可以让 Workflow 正常收尾，但不会把业务失败自动改写成成功。只有全部业务分支失败时才设置 dialogue 级 skill_failed；部分失败不会抹掉成功兄弟分支。

## 8. Trace 上下文传播

WorkflowExecutionState 中的 trace_id 会在 Runtime 工作线程打开 NODE span 前重新注入。Worker 再把 trace_id 和 NODE span_id 注入 Python 子进程，最后通过 RAII 清理线程局部上下文。

这样 C++ NODE、Python Coordinator、Skill、LLM 和 Tool Span 可以属于同一 Trace，线程复用时也不会把上一个请求的上下文带到下一个请求。

## 9. ABI 和清理

底座还对齐了以下协议：

- Workflow JSON 结果与 Python stdout 的结构化 ABI。
- Framework completion 与业务字段的 metadata。
- Dialogue interrupt 对活动及排队 Batch 的清理。
- 分支结果消费后的 DataBus 清理时机。
- 只有真实终态才能让 Batch 消费屏障归零。

## 10. 风险边界

这是当前改动中风险最高的模块，因为它改变 Agent Task 调度和完成消费。修改目标被限制在 Harness Batch、结果关联、Trace 传播和必要生命周期逻辑，没有把通用 Workflow 引擎重写成另一套调度器。

对应变更主要见 CHG-005、007、014、016、019、021、029、031、048、056、057、067、084、089、107、117、120。

