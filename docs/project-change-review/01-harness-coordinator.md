# Harness Coordinator 模块改动

## 1. 模块职责

Coordinator 不直接执行具体 Skill。它负责读取对话上下文，让 LLM 决定是直接回答还是调用 run_skills，并把每个 Skill 请求转换成框架可执行的派发清单。

主文件：

- packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
- packages/agentflow-runtime/src/agentflow_runtime/coordinator_common.py
- packages/agentflow-runtime/src/agentflow_runtime/coordinator_agent_loop.py
- packages/agentflow-runtime/src/agentflow_runtime/prompts/coordinator.py
- packages/agentflow-runtime/src/agentflow_runtime/databus_client.py

## 2. 从原型派发改成结构化 Batch

| 原来的行为 | 当前行为 |
|---|---|
| run_skills 的多个 Skill 主要靠名称区分 | 每次 run_skills 生成唯一 batch_id，每个分支生成唯一 exec_skill_key |
| 所有业务 Skill 都表现为 skill-runner，真实身份容易丢失 | skill-runner 只表示框架执行器；真实 name、prompt、load_skills、force 写入独立 manifest |
| next_skills 是松散列表 | next_skills 中每项携带 exec_skill_key 和 batch_id，可由 C++ 注册为一个 Batch |
| 同名 Skill 并行或跨轮执行时无法可靠区分 | exec_skill_key 使用 dialogue、派发轮次和请求序号构成，成为端到端分支主键 |

当前派发身份大致是：

- batch_id：一次 run_skills 调用。
- exec_skill_key：该 Batch 中的一个具体 Skill 请求。
- request_index：该请求在模型输出中的原始顺序。
- skill_name：业务 Skill 名。
- skill-runner：统一框架执行入口，不是业务 Skill。

## 3. 派发提交改为部分成功

原实现分别写 manifest、last_prompt 和 next_skills，却不检查 DataBus 写入结果。当前逻辑改为：

1. 先逐个校验和写入 manifest。
2. 某一个 manifest 写失败，只将该分支放入 rejected，不影响其他分支。
3. 至少一个 manifest 成功后才提交 next_skills。
4. next_skills 提交失败时，整批返回 dispatch_failed，不假装已进入 C++。
5. last_prompt 只在 next_skills 成功后更新，避免未执行请求污染重复判定。
6. 返回状态区分 dispatched、partially_dispatched、dispatch_failed、blocked_by_hook 和 duplicate_blocked。

这使“派发请求”“实际进入框架的分支”和“被拒绝的分支”成为三组可分别记录的事实。

## 4. 参数和边界校验

当前在写 DataBus 之前校验：

- skills 必须是列表，且不能为空。
- 每项必须是对象并具有合法 name。
- prompt 必须是字符串。
- load_skills 必须是合法 Skill 名列表。
- force 必须是显式布尔值。
- 不允许路径穿越式 Skill ID。
- 单次 LLM 回复中出现多个 run_skills 调用会被拒绝并要求模型纠正，防止多个调用覆盖同一个 next_skills。

Hook 阻断、参数非法和派发失败不再提前写入“已派发”账本。

## 5. 异步 Skill 结果重新接回 Tool Call

最初的 Coordinator 在派发时只得到一句“已调用若干 Skill”，之后完成结果可能作为新的 user 消息进入下一轮。当前逻辑保留原 run_skills Tool Call 的身份：

- 派发成功时保存带 batch_id 的占位 Tool Result。
- C++ ALL Join 完成后写 completion_batch:<batch_id>。
- Coordinator 再激活时读取结构化 Batch 结果。
- 完成结果替换与 batch_id 匹配的派发占位结果，使消息历史保持 assistant tool_call → tool result 的协议关系。
- 每个分支结果仍保留 exec_skill_key、业务 Skill 名、Workflow 身份、业务状态和 Artifact。

因此，建树时可以从 Coordinator 决策连接到 Batch，再连接到各 Skill 分支，最后回到同一次 Coordinator 激活。

## 6. 重复派发控制

重复判定从“只看最近完成的 Skill”改为 dialogue 范围账本：

- 使用 Skill 名和 prompt 的完成指纹识别完全相同的已完成委派。
- Batch 完成后逐分支记录完成标记。
- pending、completed 和本轮 requested 分开处理。
- force 是 Coordinator 显式选择的逃生口，不再由框架默认写死。
- 同名 Skill 但 prompt 不同可以再次执行。

这不是禁止循环，而是阻止“同一 Dialogue 中同名且同 Prompt 的无信息重复”。

## 7. Coordinator Loop 与终态

当前显式区分：

- activation：外部输入或一个 Batch 完成后，Coordinator 被重新激活。
- decision：一次 LLM 决策。
- correction：Tool 参数或协议错误后的内部修正。
- completion：给出最终候选回答。
- loop.control：继续、恢复或停止的原因和预算。

所有最终业务出口都通过幂等结束逻辑生成一次 dialogue.end。成功派发后等待 Skill 的出口不会提前写 dialogue.end。未处理异常也会形成结构化终态，而不是只留下断开的 Span。

## 8. Batch 结果语义

Coordinator 接收的 Batch 结果同时包含：

- 每个分支的 workflow_success。
- 每个分支的 skill_success、skill_status 和 skill_result。
- Batch 业务聚合状态 true、partial 或 false。
- Batch Workflow 聚合状态 completed、partial 或 failed。

部分成功使用专门提示：保留成功结果，只在确有必要时处理失败分支，不因一个分支失败重跑全部成功分支。

## 9. 形成的不变量

- 每个已接受派发必须具有 batch_id 和 exec_skill_key。
- 每个 exec_skill_key 至多属于一个 Batch 分支。
- 单个 manifest 失败不能阻止兄弟分支执行。
- C++ 未确认 Batch 终态前，Coordinator 不能越过 pending 屏障给出最终结果。
- 完成结果必须回接原 run_skills Tool Call，不能伪装成用户输入。
- 业务失败与 Workflow 失败不能互相替代。

对应变更主要见 CHG-005、007、010、016、029、030、033、034、047、052、056、057、061、062、117、120。

