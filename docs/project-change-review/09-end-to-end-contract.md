# 当前端到端执行契约

## 1. 一轮 Dialogue 的完整路径

当前主链如下：

1. 前端向 Bridge 发送 user_id、client_id、消息和可选输入文件。
2. Bridge 创建 dialogue_id，加载该 User/Conversation 的历史，并注册活动 Dialogue。
3. Daemon 将请求交给 C++ Agent 的 Coordinator Task。
4. Python Coordinator 调用 LLM，选择直接回答或 run_skills。
5. run_skills 为每个请求写 manifest，并提交带 batch_id 的 next_skills。
6. C++ Agent 注册 Harness Batch，按并发上限启动 skill-runner Task。
7. 每个 Skill Executor 用 exec_skill_key 读取自己的 manifest，恢复业务 Skill 和 prompt。
8. Skill 内部循环执行 LLM；同一轮多个 Tool 可并行，并按原 Tool Call 顺序回灌。
9. Tool 可在 Host、Local Sandbox 或 E2B 执行，结构化产物被回收和持久化。
10. 每个 Skill 产生独立 Workflow 终态和业务终态。
11. C++ 等待所有 exec_skill_key 有终态，按派发顺序形成 completion_batch。
12. Coordinator 被重新激活，完成结果接回原 run_skills Tool Call，再决定继续派发或回答。
13. Coordinator 给出候选终态，Bridge 完成内容和 Artifact 转换。
14. Bridge 持久化本轮并发送 final/error/done，SessionLog 追加 client.delivery。

## 2. 身份映射

| 身份 | 唯一范围 | 用途 |
|---|---|---|
| user_id | 用户 | 隔离用户数据和轨迹目录 |
| client_id / conversation_id | 用户内会话 | 多轮历史和 Conversation 工作区 |
| dialogue_id | 一轮输入到交付 | 本轮请求、取消、轨迹文件 |
| batch_id | 一次 run_skills | 并行分支共同父节点 |
| exec_skill_key | Batch 内一个请求 | Dispatch、Skill Task、completion 的主关联键 |
| skill_task_id | C++ Task 实例 | 框架 Task 生命周期 |
| workflow_execution_id | Workflow 一次执行 | Runtime 结果隔离 |
| call_id | 一次 LLM 调用 | request、response、Hook 和 usage 配对 |
| tool_call_id | 一次 Tool Call | Tool 请求与结果配对 |
| trace_id/span_id | 一次执行链及节点 | 时序、父子关系和耗时 |
| artifact routing/content id | 一个产物 | 存储、下载和来源追踪 |

其中 session_id/session_slot_id 是底座内部路由身份，不应成为业务轨迹树的 User/Conversation 层级。

## 3. 五类状态不能混用

| 状态轴 | 回答的问题 | 示例 |
|---|---|---|
| Tool 状态 | 单次工具业务是否成功 | completed、timeout、security_policy_blocked |
| Skill 业务状态 | 委派任务是否完成 | completed、llm_error、max_rounds_reached |
| Workflow 状态 | 执行器是否按框架协议收尾 | completed、failed |
| Batch 状态 | 多分支整体怎样 | true、partial、false；另有 workflow_status |
| Delivery 状态 | 用户实际是否收到什么 | final、error、done、disconnect |

典型合法组合：

- Workflow completed + Skill failed：程序正常返回了明确业务失败。
- Batch partial：部分 Skill 成功，部分失败。
- Dialogue 候选 completed + Delivery disconnect：答案已生成，但用户连接断开。
- Tool failed + Skill completed：Tool 首次失败，后续重试或替代方案成功。

轨迹树不能用一个 success 布尔值覆盖这些状态。

## 4. 建树所需的主要边

| 父节点 | 子节点 | 连接依据 |
|---|---|---|
| Dialogue | Coordinator activation | dialogue_id、activation_index |
| Coordinator decision | Dispatch Batch | LLM Tool Call、batch_id |
| Batch | Skill branch | exec_skill_key、request_index |
| Skill | LLM round | skill_name、execution identity、Span |
| LLM round | Tool Call | tool_call_id、round、Span |
| Tool Call | Tool Result | tool_call_id |
| Tool/Skill | Artifact | source event、路径、内容身份 |
| Skill branches | Batch result | exec_skill_key、batch_id |
| Batch result | Next activation | completed_batch_key |
| Dialogue candidate | Client delivery | dialogue_id、source terminal |

source_event_seqs 用于同一进程内直接因果，跨进程连接优先使用上述稳定业务键，不能只依赖时间戳。

## 5. 并发下的排序规则

- dispatch_order 表示模型请求顺序。
- Tool ordinal 表示单次 LLM 回复中的 Tool Call 顺序。
- timestamp 表示实际发生时间。
- seq 只保证同一 trace_id + pid 内单调。
- 并发分支的全局顺序不能由多个进程的 seq 推导。
- Batch 聚合按 dispatch_order 输出，但树上保留每个分支自己的真实时间线。

## 6. 当前已建立的关键不变量

- 每个接受的 Dispatch 有唯一 exec_skill_key。
- 每个 Tool Call 有唯一 Tool Result，或明确的 skip/timeout 终态。
- 每个结构化 Skill 出口有唯一 skill.end。
- 每个 Batch 在所有分支终止后只触发一次 Coordinator。
- 每个 Dialogue 有 header、start 和唯一业务终态。
- Bridge 实际交付单独记录。
- Artifact 来源必须结构化且可验证。
- 轨迹写入失败不能反过来改变业务结果。
- 凭据不能以原值进入 Tool、Sandbox、日志、轨迹或数据库。

## 7. 没有在本轮总结中声称完成的内容

- SessionLog Reader 不是正式轨迹树数据库。
- 当前没有在本目录中实现 Protobuf 序列化或树结构存储。
- 没有实现基于轨迹的 Skill 编排学习、Prompt 自进化或 Loop 结束条件优化。
- 没有评价具体 Provider、模型或多媒体 API 的优劣。
- 没有把当前巨大工作区声明为可直接合并；仍需按模块拆分、审查和提交。

