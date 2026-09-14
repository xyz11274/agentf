# Skill 与 Tool 执行模块改动

## 1. 模块职责

Skill Executor 是每个 Skill 分支内部的循环执行器。它读取派发 manifest，装载 Skill 文档和 Tool，构造 LLM Prompt，执行 Tool Call，再将结果回灌给下一轮 LLM。

主文件：

- packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py
- packages/agentflow-runtime/src/agentflow_runtime/skills_loader.py
- packages/agentflow-runtime/src/agentflow_runtime/tool_registry.py
- packages/agentflow-runtime/src/agentflow_runtime/tool_policies.py
- packages/agentflow-runtime/src/agentflow_runtime/hooks/engine.py
- packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py

## 2. Skill 身份不再依赖进程名

所有分支仍运行 skill-runner，但 Skill Executor 必须先通过 exec_skill_key 读取 manifest，恢复：

- skill_name。
- 本次专属 prompt。
- load_skills。
- force。
- batch_id。
- request_index。

manifest 缺失、不可读或 JSON 非法时，现在直接生成 dispatch_manifest_error 终态；不会再以 unknown-skill 和原始用户输入继续执行一个无关任务。

Skill 名和 load_skills 经过格式及路径边界校验，防止通过 Skill ID 读取 Harness 目录之外的文件。

## 3. Prompt 和 Tool 装载

当前 Prompt 由以下部分组成：

- 当前业务 Skill 的 SKILL.md。
- manifest 明确要求加载的伴随 Skill 文档。
- 网络失败、网络资源、Artifact 自动收集、提问降级、执行报告和 Tool 优先级等通用规则。
- manifest 中的 prompt 作为本分支实际任务；只有 prompt 为空时才回退原用户输入。

模型或厂商配置的具体替换不属于本次改动总结。这里真正的项目变化是：每个分支拥有独立任务指令和独立 Skill/Tool 视图。

Tool Registry 的同名覆盖改为同时替换 schema 和实现，避免模型看到旧参数定义却执行新函数。

## 4. LLM 错误和恢复流程

原来语义恢复分支把 continue 放在进度上报异常中，进度上报成功时反而直接结束。当前改为：

- Tool Call 参数截断或 JSON 非法时，追加恢复提示、提高完成预算并真正进入下一轮。
- 进度上报失败只记录 warning，不控制恢复是否继续。
- 语义恢复与 HTTP 传输重试分开计数。
- 明确的瞬态 HTTP 错误可以重试；非瞬态错误直接终止。
- 同一次 LLM 调用的所有传输重试共享总超时预算，不会每次重新获得完整超时。
- 每轮 LLM 还必须服从 Skill 节点剩余时间，给结束与落盘保留预算。

空 content、畸形 Provider 响应和无法恢复的 Tool Call 不再标记 completed。

## 5. Tool Call 并行与顺序

同一轮 LLM 返回多个 Tool Call 时，当前执行器可以并行执行它们，但消息回灌仍遵守模型原始顺序：

1. 给每个 Tool Call 保存 tool_call_id 和 ordinal。
2. 并行执行实际 Tool。
3. Hook 跳过的 Tool 也保留原 ordinal。
4. after_tool Hook 改写结果时校验数量、结构和 tool_call_id。
5. 非法改写被忽略，恢复原结果。
6. 最终 role=tool 消息按 ordinal 排序后写回 messages。

这样既获得 Tool 并行，又不会产生“B 先完成就被写到 A 的 tool_call_id”之类的协议错配。

## 6. Tool 业务失败上卷

Tool 进程能正常返回不等于业务成功。当前会从结构化结果中识别：

- error 字段。
- ok=false 或 success=false。
- 非零 exit_code。
- failed、timeout、cancelled、security_policy_blocked 等业务状态。

未被后续成功重试解决的 Tool 失败会进入 unresolved_tool_failures，并影响最终 Skill 业务终态。一个 Tool 失败不会直接杀死同轮的兄弟 Tool，但也不会再被最终文本掩盖成 Skill 成功。

## 7. 终止路径统一

原代码中大量 return 会绕过 skill.end。当前通过局部幂等结束函数和外层 try/finally 保证：

- completed。
- llm_error。
- tool_call_error。
- node_timeout。
- interrupted。
- max_rounds_reached。
- empty_response。
- dispatch_manifest_error。
- 未处理异常。

只要已经形成结构化 output，就尝试生成一次且仅一次 skill.end。Hook 或 Span 写盘失败被隔离，不反过来改变业务结果。

Timeout 和 Max Rounds 不再把最后一条 Tool JSON 或恢复提示冒充最终答案，而是返回带轮次、原因和可归因中间信息的结构化结果。

## 8. Artifact 收敛

Tool 的 Artifact 只从结构化字段提取，不再从任意 stdout、网页正文、错误文本或最终 LLM 文本中猜路径。当前还会：

- 只处理本轮新观测的路径。
- Hook 原地追加的 Artifact 不丢失。
- 同名外部 Artifact 不静默覆盖。
- 结构化目标 Artifact 已成功生成时允许立即收敛，减少无效后续轮次。

## 9. 形成的不变量

- Skill Executor 必须由有效 manifest 驱动。
- Tool Call 和 Tool Result 必须通过 tool_call_id 一一配对。
- 并行完成顺序不能改变消息协议顺序。
- 每次结构化 Skill 退出必须有唯一业务终态。
- Python 进程退出成功只代表 Workflow 可完成，不能自动推导 Skill 业务成功。
- Tool、LLM、Hook 和轨迹故障必须各自保留所属错误域。

对应变更主要见 CHG-002、003、011、012、013、017、022、024、025、032、035、036、037、038、039、040、041、042、045、048、049、051、054、060、090、091、093、097、098、099、100、103、119。

