# Harness 改动记录

本文件用于持续记录 Harness 开发过程中的实际改动，作为代码审查、问题回溯、
轨迹采集设计和后续 Rust 重写的变更依据。

## 记录规则

- 代码、测试、配置、脚本、开发环境或项目文档发生改动时，必须在同一轮工作中更新本文件。
- 纯只读分析、代码讲解、测试执行和临时诊断文件不单独记为改动；其结论可写入相关改动的验证部分。
- 不覆盖或改写历史条目；需要更正时新增条目并引用原条目编号。
- 每条记录至少包含：问题或目标、改动内容、涉及文件、诱发原因、验证结果和遗留事项。
- 改动记录按编号升序排列，新记录追加到已有记录之后。

## 改动条目

### CHG-20260826-001：修复 Sandbox endpoint 自递归与工具错误丢失

- 日期：2026-08-26
- 目标：修复 local Sandbox 工具调用自身、耗尽 8 个并发槽的问题，并防止工具失败被表示为空成功。
- 诱发原因：
  - `sandbox_http_server` 继承了 worker 的 `AF_SANDBOX_MODE` 和 `AF_SANDBOX_ENDPOINT`。
  - 服务内部委托 runtime tool 时，executor factory 再次选择 `HTTPExecutor`，请求被 POST 回当前服务，形成递归。
  - `HTTPExecutor._post()` 原样返回响应 JSON，没有把 HTTP 503 或非零 `exit_code` 的 `stderr` 转换为 `error`。
- 改动内容：
  - 建立本集中改动日志，后续项目文件变更均在同一轮追加记录。
  - Sandbox HTTP 服务启动后将执行模式切换为 `none`，并清除继承的上游 endpoint；此处的 HostExecutor 运行在已经建立的 local、Docker 或 E2B 隔离环境内部。
  - HTTPExecutor 将 HTTP 4xx/5xx 和非零 `exit_code` 统一转换为明确错误，保留服务端 `stderr`。
  - 增加 HTTP 503、写文件失败以及继承自身 endpoint 的回归测试。
  - 新增注释按所在文件的既有语言风格编写。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `docker/sandbox/http_server/sandbox_http_server.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/executors/http.py`
  - `packages/agentflow-runtime/tests/test_http_executor.py`
  - `packages/agentflow-runtime/tests/integration/test_file_tools_sandbox_integration.py`
- 验证结果：
  - 相关单元与集成测试：44 项通过。
  - 最终核心回归：14 项通过。
  - Python 语法编译与 `git diff --check` 通过。
  - 真实 hchat 中 `bash` 返回 `exit_code=0`，不再出现 `Concurrent tool execution limit reached (8)`。
  - 同一真实 Sandbox 中 `write_file` 成功，`read_file` 读回 `SANDBOX_FIX_OK`。
  - GitNexus 修改前影响分析：`sandbox_http_server.main` 为 LOW，`HTTPExecutor._post` 为 MEDIUM。
  - GitNexus 当前工作树检测为 MEDIUM；对比 `main` 的 CRITICAL 来自 harness 分支已有的大规模差异，并非本条改动单独造成。
- 遗留事项：
  - Coordinator 最终回答可能把 Skill Executor 的工具能力错误概括为“只有 run_skills”；该问题与 Sandbox 自递归独立，本条未修改。
  - Skill 读取宿主绝对路径时可能被 Sandbox 路径边界拒绝，需要作为独立问题分析。

### CHG-20260826-002（补录）：修复 Skill/LLM span 泄漏并补充 Tool span

- 日期：2026-08-26
- 目标：保证 Skill Executor 在正常返回、错误返回和未捕获异常下都能闭合 span，并让工具调用进入可回放的 span 树。
- 诱发原因：
  - `skill_executor.execute` 的外层 Skill span 依赖各个 `return` 出口手工调用 `__exit__()`；新增出口或未捕获异常会绕过关闭逻辑，导致 span 泄漏。
  - 每轮 LLM span 同样在若干显式分支中手工关闭，LLM 调用前后的任意异常可能留下未闭合 span，并污染后续 span 的父子关系。
  - 工具调用只有事件和耗时日志，没有独立的 Tool span，轨迹中无法完整表达“一轮工具批次”的父子关系、状态和工具数量。
- 改动内容：
  - 将 `skill_executor.execute` 的主体放入 `try/finally`，在唯一的 `finally` 中使用当前异常信息关闭 `skill_executor.execute` Skill span。
  - 将每轮 LLM 生命周期放入 `try/finally`，统一关闭 `skill_executor.llm_round` span；正常结果标记 `OK`，LLM 失败和空响应标记 `ERROR`。
  - 保留各终止出口的业务状态设置，包括 `node_timeout`、`interrupted`、`llm_error`、`tool_call_error`、工具死循环和 `max_rounds_reached`。
  - 新增 `skill_executor.tool_batch` span 名并加入全量 span 目录。
  - 使用 `SpanKind.TOOL` 包裹 `execute_in_batches()`；记录 round、tool_names、tool_count 和 dialogue_id。LLM span 在工具执行前已经闭合，因此 Tool span 与 LLM span 均直接挂在 Skill span 下。
  - 增加未捕获异常、语义恢复耗尽、并行工具批次父子关系及 SpanContext 清空回归测试。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `packages/agentflow-runtime/src/agentflow_runtime/events.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_hooks.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_tool_parallel.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py`
- 验证结果：
  - continue、span 目录、并行工具和异常关闭相关测试合计 43 项通过。
  - 未捕获异常测试确认 `__exit__()` 收到真实 exception type、value 和 traceback。
  - 并行工具测试确认只生成一个 `TOOL` batch span，其父 span 为 `skill_executor.execute`，状态为 `OK`，tool_count 为 2。
  - 测试结束后 `SpanContext.current().current_span_id()` 为空，未发现 span 栈残留。
- 遗留事项：
  - 当前 Tool span 以批次为粒度；如果轨迹树后续需要逐工具节点，还需在每个 `_execute_tool_call` 周围增加子 span，并处理并行线程中的上下文传播。

### CHG-20260826-003（补录）：修复语义恢复 continue 位置错误

- 日期：2026-08-26
- 目标：当 LLM 返回截断或非法 JSON 的 tool arguments 时，Skill Executor 应真正进入下一轮语义恢复，而不是立即返回 `tool_call_error`。
- 诱发原因：
  - 恢复分支中的 `continue` 原本缩进在 `databus.response_push()` 的 `except` 内。
  - 当进度推送成功时不会执行 `continue`，控制流继续落入“恢复次数耗尽”输出并 `return`；反而只有进度推送失败时才会重试，行为与设计完全相反。
- 改动内容：
  - 将 `continue` 移到进度推送 `try/except` 之后，使推送成功或失败都进入下一轮外层 LLM 循环。
  - 保留语义恢复原有行为：追加 `SKILL_RECOVERY_PROMPT`、增加 `recovery_attempts`，并将 completion budget 按轮加倍且最高限制为 16384。
  - 恢复次数真正耗尽后才写入 `status=tool_call_error`，同时把外层 Skill span 标记为 `ERROR`。
  - 新增控制流回归测试，覆盖成功推送后的重试、恢复耗尽和相关 span 关闭。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py`
- 验证结果：
  - 回归测试确认第一次非法 tool call 后会再次调用 LLM，总调用次数为 2。
  - 第二轮 messages 包含 `SKILL_RECOVERY_PROMPT`，completion budget 从 4000 增长到 8000。
  - 第二轮正常响应后结果为 `status=completed`，不会误报 `tool_call_error`。
  - continue、span 目录、并行工具和异常关闭相关测试合计 43 项通过。
- 遗留事项：
  - 此恢复仅处理 tool arguments 截断或非法 JSON 等语义错误；SSL、DNS、超时等传输错误仍由 `call_llm()` 内部重试，不进入该外层恢复分支。

### CHG-20260826-005：拆分 Workflow 执行结果与 Skill 业务结果

- 日期：2026-08-26
- 目标：使 workflow 状态只描述基础设施和控制流是否执行成功，Skill 的业务结果
  通过独立字段传递，避免业务失败污染 workflow、Node span 和后续轨迹。
- 诱发原因：
  - `skill_executor.py` 为了保留结构化 `skill_result`，会在业务失败时正常退出
    Python 进程，并通过输出中的 `status` 表达业务结果。
  - Agent 原本仅通过 `workflow_completed`/`workflow_failed` 推导
    `skill_success`，无法区分“节点执行失败”和“节点执行成功但 Skill 业务失败”。
  - Flow trace 将 Skill 的所有终止结果都记录为 `skill-completed`，导致业务失败
    在轨迹中被错误标记为成功完成。
- 改动内容：
  - workflow 和 Node 继续只根据进程、节点及控制流执行结果判定成功或失败；
    Skill 业务状态不会改变 `NodeState`、Node span 或 workflow 结果。
  - Skill workflow 完成时由 Runtime 附加 `skill_status` 与 `skill_success`：
    `completed` 对应成功；`node_timeout`、`interrupted`、`llm_error`、
    `tool_call_error`、`tool_call_loop_detected`、`max_rounds_reached` 对应业务失败。
  - Agent 优先使用显式 `skill_success`，缺失时继续根据 workflow 消息推导，
    保持旧 Runtime 消息兼容；同时把 `skill_status` 传给 Coordinator 和 Skill
    response。
  - Coordinator 的 `skill_failed` 标志和重触发逻辑反映 Skill 业务结果，不再
    依赖 workflow 是否失败。
  - 新增 `SKILL_FAILED` Flow trace 事件；业务失败记录为 `skill-failed`，业务成功
    仍记录为 `skill-completed`。
  - 回归测试分别覆盖业务失败但 workflow 成功、业务成功、节点真实失败、非
    Skill workflow 隔离，以及 Agent 对显式业务失败字段的处理。
- 最终语义：
  - 节点执行成功且 Skill 成功：`workflow_completed`、`skill_success=true`。
  - 节点执行成功但 Skill 失败：`workflow_completed`、`skill_success=false`，并
    保留具体 `skill_status` 和 `skill_result`。
  - 进程或节点执行失败：`workflow_failed`、`skill_success=false`。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `include/internal/runtime/flow_trace.h`
  - `src/runtime/runtime.cpp`
  - `src/agent/agent.cpp`
  - `tests/functional/src/test_agent_loading.cpp`
  - `tests/unit/src/test_agent_gtest.cpp`
- 验证结果：
  - 两个受影响测试目标编译成功；输出仅有项目原有编译警告。
  - 两项定向回归通过；失败日志确认同时出现 `workflow_completed`、
    `skill_success=false` 和 `event=skill-failed`。
  - `test_agent_loading` 全量 10 项通过。
  - `AgentTest.*Skill*` 13 项通过。
  - 相邻 `RuntimeMasterWorkerTest.*` 13 项通过。
  - `skill_executor.py` 状态源核对确认当前 6 个失败终态均已覆盖。
  - 修改前 GitNexus 类级影响分析：`MMAgentRuntime` 为 HIGH，44 个受影响符号、
    21 个直接依赖；`MMAgent::handle_task_data_exchange` 按中央消息分发路径作为
    HIGH 风险处理。本会话未暴露 GitNexus MCP，因此 FlowTrace 枚举采用源码级
    依赖检查；提交前仍需在 GitNexus 可用环境执行 `detect_changes()`。
- 遗留事项：
  - 失败状态使用与 Python Skill Executor 当前契约一致的精确白名单；今后新增
    Skill 终态时，必须同步更新 C++ 映射和测试，或将该契约迁移到共享 schema。
  - `workflow-completed` Flow trace 只描述工作流执行完成；Skill 业务结果由独立
    `skill-completed`/`skill-failed` 事件及 completion metadata 表达。

### CHG-20260826-006：形成 Harness v1 并行执行修改方案

- 日期：2026-08-26
- 目标：只解决同一轮多个 Skill 的并行启动、独立完成、批次汇合和 final 屏障，
  不扩展到通用轨迹、Rust 重写、Prompt/Loop 进化或其他 Harness 重构。
- 诱发原因：
  - 同批 skill-runner 可能共用 task_id，完成回传又缺少 exec_skill_key。
  - Runtime 在 workflow 完成时重新读取 dialogue 共享输出键，可能把 A 的结果
    读成 B 的结果。
  - completion 路径会清空整个 DataExchange 队列，可能删除已排队的兄弟消息。
  - pending/running 标量无法对重复、迟到和未知 completion 做幂等处理。
  - 每个 completion 都会重启 Coordinator；no-next_skills final 路径还可能绕过
    未完成 Skill。
- 改动内容：
  - 新增仅面向并行执行的方案文档，采用固定 ALL Join：部分完成只释放并回填
    并发槽位，整个 Batch 终止后只触发一次 Coordinator。
  - 复用 exec_skill_key 作为单次 Skill 权威身份，只新增 batch_id，并由 C++
    回传 workflow_execution_id。
  - 设计 execution-scoped terminal output snapshot，避免修改通用
    build_framework_key 或继续从共享结果键读取。
  - 设计 Harness 专用 BatchState，以 expected、queued、active、terminal 集合
    替代标量计数作为正确性依据。
  - 明确移除全队列 clear、统一 Final Gate、处理重复 completion、launch failure、
    timeout、乱序完成和并发槽位回填。
  - 给出四阶段实施顺序、回退开关、定向测试和 hchat 验收条件。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `docs/harness-v1-code-reading/README.md`
  - `docs/harness-v1-code-reading/HARNESS_PARALLEL_MODIFICATION_PLAN.md`
- 验证结果：
  - 方案仅覆盖并行主链中的身份、输出隔离、队列、槽位、Join 和 Final Gate。
  - 已核对 Coordinator dispatch、Runtime completion、Agent queue/launch 以及
    pending/running 相关实现；本条没有修改业务代码或 C++ 符号。
- 遗留事项：
  - 开始实际代码改动前，仍须按 `AGENTS.md` 对目标符号运行 GitNexus upstream
    impact；当前会话未暴露 GitNexus MCP。
  - 推荐先实施身份传播和输出隔离，验证双 Skill 乱序完成后再修改 Batch Join。

### CHG-20260826-007：实现 Harness v1 多 Skill 并行闭环

- 日期：2026-08-26
- 目标：使同一次 `run_skills` 中的多个 Skill 能真正并行运行、结果互不串线，
  全部进入终态后只触发一次 Coordinator，并在批次结果被消费前阻止 final。
- 诱发原因：
  - 同批 `skill-runner` 缺少完整批次身份，task_id 还可能冲突。
  - Runtime completion 原本会重新读取 dialogue 共享结果键，并发时可能把 A 的结果
    读成 B 的结果。
  - completion 路径清空整个 DataExchange 队列，可能删除已排队的兄弟消息。
  - `pending_skills_count` 和 `running_skills_count` 是标量，无法对重复、未知、迟到
    completion 做幂等状态迁移。
  - 每个 Skill completion 都可能重启 Coordinator，且 final 路径没有可靠的批次
    完成与消费屏障。
- 改动内容：
  - Coordinator 每次 `run_skills` 生成一个 `batch_id`，每项 dispatch 保留唯一
    `exec_skill_key`；C++ task_id 同时包含该执行身份。
  - Runtime 为每个 workflow execution 保存独立 `terminal_outputs`，completion
    携带 `batch_id`、`exec_skill_key`、`skill_task_id` 和
    `workflow_execution_id`，不再从共享结果键反查终态输出。
  - Harness 新增按 dialogue 和 batch 索引的 BatchState，以 expected、queued、
    active、terminal 集合进行带锁迁移；槽位释放后直接回填，重复、未知和迟到结果
    不产生副作用。
  - 采用固定 ALL Join：只有全部 expected 进入终态后才按原 dispatch 顺序写入唯一
    `completion_batch`，并原子保证 Coordinator 只触发一次。
  - Python 一次读取完整 Batch，生成一条沿用当前角色与 Prompt 格式的结果输入，聚合
    全部 Artifact，并在消息构造成功后立即写入 `:consumed`。
  - Final Gate 同时检查“全部 terminal”和“聚合结果已消费”；消费前不发送 final、
    不清理 dialogue。
  - Session interrupt 会中断 active task，并将 queued 项写为
    `session_interrupted_before_launch`，保证 Batch 收敛。
  - 无 `batch_id` 的旧请求继续走兼容路径；通用 C++ Workflow Scheduler、Tool 执行器
    和消息角色协议均未重构。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `docs/harness-v1-code-reading/README.md`
  - `docs/harness-v1-code-reading/HARNESS_PARALLEL_MODIFICATION_PLAN.md`
  - `examples/agent/outfit-assistant/coordinator_workflow.json`
  - `frontend/bridge/templates/agent_template/harness_agent/coordinator_workflow.json`
  - `frontend/bridge/templates/agent_template/harness_agent/skill_workflow.json`
  - `resource/agent-packages/outfit-assistant/coordinator_workflow.json`
  - `include/internal/agent/agent.h`
  - `include/internal/agent/builtin_dispatcher.h`
  - `include/internal/runtime/agent_runtime.h`
  - `packages/agentflow-runtime/src/agentflow_runtime/common/runtime_context.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
  - `src/agent/agent.cpp`
  - `src/agent/builtin_dispatcher.cpp`
  - `src/runtime/runtime.cpp`
  - `tests/functional/src/test_agent_loading.cpp`
  - `tests/unit/src/test_agent_gtest.cpp`
- 验证结果：
  - Python Coordinator 13 项测试通过。
  - C++ Harness Batch 3 项测试通过，覆盖 batch_id 解析、5 个 Skill 在并发上限 2
    下持续回填、乱序完成、重复/迟到 completion、单次 Join、消费屏障和 Session
    interrupt 收敛。
  - Runtime 2 项测试通过，确认 Skill 业务状态独立于 workflow 成败，并发 execution
    即使共享 dialogue 也各自返回正确 result、status、skill_name 和 Artifact。
  - 旧 `SessionInterruptStopsRunningSkillTasks` 回归通过。
  - 真实 Harness v1 hchat 同一次 `run_skills` 启动 `outfit-stylist` 和
    `forge-image-gen`：active 峰值为 2，后派发的 Skill 先完成；两项都 terminal 后
    才生成 `completion_batch`，Coordinator 只触发一次；`:consumed` 出现后才发送
    唯一 final 并清理 dialogue。
  - 测试日志：`logs/agentflowd-parallel-hchat-v1.log`。
- 遗留事项：
  - 默认 Coordinator 版本为 v2，它不经过本次 v1 `run_skills` 路径；复测本功能需
    显式设置 `AF_COORDINATOR_VERSION=v1`。
  - 当前仍采用既有“Skill 结果作为 user 消息、dispatch ACK 作为 tool 消息”的协议；
    这是原问题清单中的独立问题，不在本次并行修改中处理。
  - 全量 C++ 测试包含依赖外部流式网络的既有用例，本次以所有受影响路径的定向回归
    和真实 hchat 作为验收，未把无关网络失败计为并行修改失败。

### CHG-20260826-008：为本地 Harness v1 挂载 sessionlog 轨迹采集

- 日期：2026-08-26
- 目标：让 outfit-assistant 的 Coordinator、Skill、LLM 和 Tool hook 事件融合落盘，
  为下一次真实 hchat 提供可校验、可重放的单会话 JSONL。
- 诱发原因：
  - sessionlog 实现此前只存在于 `docs/sessionlog`，尚未部署到 agent 包的
    `harness/hooks`。
  - agent 环境未启用 `AF_SESSIONLOG_ENABLED`，所以真实执行只生成 Python span
    和 Bridge SQLite LLM trace，没有生成 `trace_output/sessionlogs/{dialogue_id}.jsonl`。
- 改动内容：
  - 将现有 `docs/sessionlog/sessionlog_hook.py` 原样部署到
    `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`。
  - 在 outfit-assistant 本地 `.env` 中启用 `AF_SESSIONLOG_ENABLED=1`。
  - 不修改 C++ 底座、共享 Hook 框架或 sessionlog 源实现；子进程下一次启动时自动加载，
    无需重启 daemon。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `examples/agent/outfit-assistant/.env`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
- 验证结果：
  - 部署文件与 docs 源文件逐字比较一致。
  - Hook loader 成功注册 `preload_user_context` 和 `sessionlog_recorder_v2`。
  - 临时目录冒烟验证成功生成 `sessionlogs/mount-smoke.jsonl`，包含 header 和
    `session.start` 两行事件。
- 遗留事项：
  - 冒烟验证只证明加载与写盘正常；完整事件配对、并行派发边和回传边需在下一次真实
    hchat 后使用 `sessionlog_reader.py` 校验。
  - LLM trace meta 中既有的空 `skill_name` 不由本次挂载修复；sessionlog 可通过
    `skill.start/tool.call/tool.result/skill.end` 的业务 skill 字段及
    `exec_skill_key` 辅助关联，后续仍应修复 Recorder 的原始归因字段。

### CHG-20260827-009：合并远程 sessionlog 并行派发边修复与测试

- 日期：2026-08-27
- 目标：将已抓取的 `agentflow/harness_agent_poc_evo` 更新合并到当前本地分支，
  同时保留现有 Harness 并行闭环、状态传播及未提交开发改动。
- 远程改动：
  - `sessionlog_reader.py` 在一个 `skill.dispatch` 包含多个
    `exec_skill_key` 时，为全部精确匹配的 `skill.start` 建立 dispatch 边。
  - 新增 sessionlog Hook 与 Reader 的 18 项单元测试及测试说明。
  - `.gitignore` 忽略 sessionlog HTML 测试报告。
- 合并结果：
  - 将远程提交 `54dd90d` 合并为本地提交 `574a5ed`，三方合并无冲突。
  - 合并前已有未提交文件均保留；远程净改动不涉及 Runtime、Coordinator、
    Skill Executor 或 C++ 底座。
- 验证结果：
  - `docs/sessionlog/tests` 共 18 项测试全部通过。
  - 使用真实轨迹 `0826171814-08715b87f3e2.jsonl` 复验，编排图 dispatch 边由
    1 条增加到 2 条，两个并行 Skill 均能连接到派发节点。
  - GitNexus MCP 未在当前工具环境暴露，项目 runner 也未返回查询结果；源码调用点
    检查确认 `build_graph` 仅由离线 Reader 入口直接调用，风险按 LOW 处理。
- 遗留事项：
  - 两条 dispatch 边的显示字段仍统一取 `skill_names[0]`，导致第二条边虽然目标正确，
    但显示名称仍为第一个 Skill。
  - Batch 完成后 `completed_skill=skill-batch`，Reader 仍无法将各 `skill.end` 连接到
    后续 Coordinator 激活，真实图目前仍缺少 return/ALL Join 边。

### CHG-20260827-010：隔离同批 Skill 的 dispatch manifest 写入失败

- 日期：2026-08-27
- 目标：同一次 `run_skills` 调用中，单个 Skill 的 manifest 写入失败时只拒绝
  该项，其余已成功写入 manifest 的 Skill 仍可进入并行 Batch；只有整批均不可提交时
  才让 Coordinator 重新决策；成功、部分成功、全部失败及保护逻辑阻断均形成可直接
  建树的结构化 `skill.dispatch` 轨迹。
- 诱发原因：
  - `handle_run_skills_call` 原本忽略 `put_global(dispatch_key, ...)` 和
    `put(next_skills, ...)` 的布尔返回值，即使写入失败仍把全部请求项报告为
    `status=dispatched`。
  - Coordinator 在调用分发函数前就把所有请求名称放入 `_dispatched_skills`，
    因此 manifest 写入失败或 Hook 阻断也会被记录成成功分发，并直接结束本轮。
  - `last_prompt` 在 `next_skills` 提交前写入，未实际进入框架的 Skill 也可能
    污染下一轮重复调度判定。
  - `coordinator.after_skill_dispatch` 原本只在至少一项成功时触发，整批失败不进入
    sessionlog；部分失败的事件也只有成功项，失败阶段和原因只隐藏在历史 tool message
    中，无法作为可靠的轨迹树节点。
- 改动内容：
  - 逐项检查 manifest 写入结果；失败项写入 `rejected`，成功项继续组装
    `next_skills`，互不连带。
  - 部分成功返回 `partially_dispatched`，并只在 `skills` 和
    `exec_skill_keys` 中列出实际提交项；全部 manifest 失败或
    `next_skills` 提交失败返回 `dispatch_failed`。
  - Coordinator 只根据分发函数返回的成功项更新进度、Span 和分发后 Hook；整批失败
    时将结构化工具结果回灌给模型，继续现有 tool loop，而不是误报成功后退出。
  - `last_prompt` 改为在 `next_skills` 提交成功后写入，且只处理实际提交项；
    该辅助写入失败只形成 warning，不回滚已经进入框架的业务调度。
  - 分发后 Hook 对所有 `run_skills` 结果触发，包括 `dispatched`、
    `partially_dispatched`、`dispatch_failed`、`duplicate_blocked` 和
    `blocked_by_hook`，并携带 requested/accepted 名称、`batch_id`、`exec_skill_keys`、
    `rejected` 和 `warnings`。
  - sessionlog 将上述字段写入同一 `skill.dispatch` 事件；失败状态禁止回读
    DataBus 中可能残留的旧 `next_skills`，避免给失败节点挂上错误 exec key。
  - 未修改 C++ Runtime、Agent BatchState 或调度底座。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/sessionlog_hook.py`
  - `docs/sessionlog/tests/test_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
- 验证结果：
  - 新增三项故障注入回归，覆盖单个 manifest 失败、全部 manifest 失败和
    `next_skills` 提交失败。
  - 新增 Coordinator 到 Hook 的执行级测试，确认全部 manifest 失败仍触发
    `coordinator.after_skill_dispatch`。
  - sessionlog 20 项测试通过，其中两项覆盖部分失败字段、全部失败落盘以及旧
    `next_skills` 隔离；docs 源 Hook 与实际挂载 Hook 逐字一致。
  - Coordinator dispatch 相关定向测试 25 项通过。
  - `packages/agentflow-runtime/tests` 全量测试 365 项通过、4 项跳过。
  - 修改文件 Python 语法编译和 `git diff --check` 通过。
  - GitNexus MCP 在当前会话未暴露，项目 runner 尝试回退到 `npx` 安装但未能
    提供可用查询；源码级上游检查确认生产调用点只有 v1 Coordinator
    `execute`。该入口位于核心调度路径，按 HIGH 风险处理，因此未扩大修改范围。
- 遗留事项：
  - 若 manifest 已写入但 `next_skills` 最终提交失败，会留下本轮不可达的 manifest；
    当前由既有 dialogue 清理逻辑按 `dispatch:{dialogue_id}:` 前缀统一回收，本条未为此
    增加 C++ 或 DataBus API。
  - 本条面向一次 `run_skills` 中包含多个 Skill 的标准调用；同一条 LLM 回复若生成多个
    独立 `run_skills` tool call，仍可能互相覆盖 `next_skills`，应作为单独问题处理。

### CHG-20260827-011：补全 LLM 轨迹的 Skill 归因字段

- 日期：2026-08-27
- 目标：Skill Executor 发起的每次 LLM 调用都在
  `llm.response.data.meta.skill_name` 中保存 manifest 对应的业务 Skill 名；
  Coordinator 调用继续保留空值，避免制造虚假 Skill 归因。
- 诱发原因：
  - Skill Executor 已通过 `call_llm(skill=skill_name)` 传递业务名，但
    `TraceRecorder.trace_call` 写成功和失败 meta 时重新调用 `_task_context()`，
    完全忽略了显式 `skill` 参数。
  - C++ 当前没有向 Skill Executor 注入 `AF_SKILL_NAME`，因此
    `_task_context()` 得到的 `skill_name` 始终为空。
  - hard compaction 会在 Skill 进程内嵌套发起 LLM 调用且不显式传 `skill`，
    仅修 recorder 主调用仍会留下该边界路径。
- 改动内容：
  - Recorder 每次调用只构造一次 task context；显式 `skill` 非空时覆盖其中的
    `skill_name`，成功和错误 meta 共用该 context。
  - Skill Executor 从 dispatch manifest 解析业务名后，在执行 span 生命周期内
    进程内绑定 `AF_SKILL_NAME`，使 hard compaction 等嵌套 LLM 调用也可归因；
    `finally` 中恢复原环境，避免单元测试或复用进程发生上下文泄漏。
  - 未修改 LLM 请求、Prompt、工具循环、Skill 返回、C++ Runtime 或调度底座。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `packages/agentflow-runtime/src/agentflow_runtime/recorder.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_recorder_skill_name.py`
- 验证结果：
  - 新增成功响应和错误响应两项 recorder 回归，均确认未注入
    `AF_SKILL_NAME` 时显式业务名仍能进入 meta。
  - `packages/agentflow-runtime/tests` 全量测试 367 项通过、4 项跳过；
    修改文件语法编译和 `git diff --check` 通过。
  - 使用隔离 user/client 的真实双 Skill `hchat` 生成轨迹
    `0827033955-17855ac33281.jsonl`：共 19 次 LLM 调用，其中
    `outfit-stylist` 10 次、`forge-image-gen` 7 次，Skill Executor 空归因 0 次；
    其余 2 次空值均为 Coordinator，符合预期。
  - GitNexus MCP 未挂载且仓库 runner 无法提供 impact；源码级回退分析确认
    `TraceRecorder.trace_call` 只有 `call_llm` 一个直接调用者，间接覆盖
    Coordinator、Skill Executor、agent loop 和 compaction，风险按 MEDIUM 处理；
    Skill Executor `execute` 属核心入口，按 HIGH 风险处理并将改动限制为进程内
    身份绑定及恢复。
- 后续处理：
  - 本次真实轨迹中 `outfit-stylist` 以 `max_rounds_reached` 结束时未产生
    `skill.end`；该问题已由 CHG-20260827-012 修复。

### CHG-20260827-012：补齐 max-rounds 终止路径的 skill.end

- 日期：2026-08-27
- 目标：Skill Executor 因循环预算耗尽以 `max_rounds_reached` 结束时，也必须
  生成带业务 Skill 名、轮次、状态和 `exec_skill_key` 的 `skill.end` 轨迹事件。
- 诱发原因：
  - sessionlog 只在 `skill_executor.after_skill_complete` Hook 上写入
    `skill.end`。
  - 正常完成和重复工具调用保护路径都会触发该 Hook，但循环自然耗尽路径只写
    DataBus 结果并关闭 Skill span，遗漏了结束 Hook。
- 改动内容：
  - 在 `max_rounds_reached` 结果写入 DataBus 后补发一次
    `SKILL_EXECUTOR_AFTER_SKILL_COMPLETE`，携带 `skill_name`、
    `round=max_rounds`、`status=max_rounds_reached` 和产物数量。
  - 保持原有 DataBus 结果、Skill span ERROR 状态、workflow 状态及 Coordinator
    输入不变；未增加 `BEFORE_SKILL_COMPLETE` 干预语义。
  - 未修改 sessionlog Hook、C++ Runtime、调度或循环判断。
- 涉及文件：
  - `HARNESS_CHANGES.md`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py`
- 验证结果：
  - 新增确定性控制流测试：设置 `max_rounds=1` 且 LLM 继续返回 tool call，
    确认结束 Hook 只触发一次，payload 状态与轮次正确，DataBus 仍保存
    `max_rounds_reached`。
  - Skill Executor 控制流定向测试 5 项通过。
  - `packages/agentflow-runtime/tests` 全量测试 368 项通过、4 项跳过。
  - sessionlog 20 项测试通过，既有 Hook 测试确认
    `after_skill_complete → skill.end` 且保存 `AF_EXEC_SKILL_KEY`。
  - 修改文件语法编译和 `git diff --check` 通过。
  - 按已知环境限制采用源码级影响分析；`skill_executor.execute` 是核心执行入口，
    风险按 HIGH 处理，实际修改仅覆盖循环自然耗尽后的终止尾部。

### CHG-20260827-013：统一 Skill 与 Coordinator 的结构化终止事件

- 日期：2026-08-27
- 目标：解决新增问题 2、3；所有已经形成结构化业务结果的 Skill 终止路径都生成
  一次 `skill.end`，所有 Coordinator 最终返回路径都生成一次 `dialogue.end`。
- 诱发原因：
  - Skill 的结束 Hook 分散在正常完成、工具循环和最大轮次等少数分支；
    `node_timeout`、`interrupted`、`llm_error`、空响应、`tool_call_error` 等直接
    `return` 会绕过 Hook。
  - Coordinator 只在正常最终回答路径触发 `AGENT_DIALOGUE_END`；批次解析失败、
    工具循环耗尽、LLM 错误、空响应、中断和强制轮次上限等出口没有终止事件。
- 改动内容：
  - Skill Executor 增加局部 `_emit_skill_end_once`，仅在 `output` 已形成结构化
    字典时发出结束 Hook；正常路径可主动调用，外层 `finally` 负责兜住所有直接
    `return`，幂等标志保证只尝试一次。
  - Coordinator 增加局部 `_emit_dialogue_end_once`，在所有最终业务出口统一写入
    session、dialogue、status、response 和 elapsed；成功 dispatch 后等待 Skill
    的路径不误报 dialogue 终止。
  - 两个 helper 都隔离 Hook 异常，轨迹插件失败不会改变既定业务结果或 workflow
    结果。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 定向控制流测试 22 项通过；`tool_call_error` 和 `max_rounds_reached` 均确认
    `skill.end` 一次且仅一次，Coordinator `max_iterations` 确认 `dialogue.end`
    一次且字段完整。
  - runtime Python 全量测试 370 项通过、4 项跳过；sessionlog 20 项通过。
- 遗留事项：
  - 未捕获异常且尚未形成结构化 `output` 时不会伪造业务 `skill.end`；该类异常仍
    由 span 异常状态和 workflow failure 表达。

### CHG-20260827-014：把 workflow trace 上下文传播到 Runtime 工作线程

- 日期：2026-08-27
- 目标：解决新增问题 5；让 C++ NODE span 与 Python Coordinator/Skill/LLM/Tool
  span 使用同一个 trace，并建立真实父子关系。
- 诱发原因：
  - `TraceContext` 是 thread-local；请求线程保存了 trace，但 `process_node` 在
    Runtime 工作线程执行，该线程上下文为空。
  - Worker 虽然已经从当前线程读取 `TraceContext` 并注入 `AF_TRACE_ID` 和
    `AF_PARENT_SPAN_ID`，但上游工作线程没有重新注入 execution 自己的 trace，
    Python 只能退化成独立 trace。
- 改动内容：
  - `process_node` 从 `WorkflowExecutionState` 读取 execution-owned `trace_id`，
    在打开 NODE span 前清空并注入当前工作线程。
  - NODE span 存活期间 Worker 读取同一上下文，向子进程传入 trace id 和 NODE
    span id；局部 RAII cleanup 在 NODE span 关闭后清空工作线程，避免复用线程
    串入下一个请求。
  - 未修改 Python trace 协议、调度状态机或通用 Worker 接口。
- 涉及文件：
  - `src/runtime/runtime.cpp`
  - `tests/unit/src/test_runtime_master_worker_gtest.cpp`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 新增完整链路测试：`execute_workflow(trace_id)` 经工作线程和 NODE span 启动
    Shell 子进程，子进程读到原始 trace id 与非空父 span id。
  - C++ core 全量 309 项测试通过。
  - 该入口属于 HIGH 风险核心路径；GitNexus 按已知环境限制不可用，本次采用源码
    调用链审查并把 C++ 修改限制在 trace 上下文注入与清理。

### CHG-20260827-015：修复 Local Sandbox 工件回收链路

- 日期：2026-08-27
- 目标：解决新增问题 6；Skill 在本地 Sandbox 写出的文件必须进入 Artifact 列表，
  并能被下载到 agent package 后交给 Bridge。
- 诱发原因：
  - `HTTPExecutor.write_file` 成功结果没有回传 path，结构化工件提取器没有明确路径。
  - Sandbox 内路径尚未存在于宿主文件系统，提取器在下载步骤之前就以
    `os.path.isfile` 失败为由丢弃。
  - `_download_from_sandbox` 使用 `params`，而 Sandbox `/exec` 的实际契约是
    `tool_params`，导致 `read_file` 请求字段不匹配。
- 改动内容：
  - 成功的 `write_file` 返回原始 path。
  - 存在 `AF_SANDBOX_ENDPOINT` 时，只允许明确结构化字段中的合法后缀路径延迟到
    `_ensure_artifacts_in_package` 校验和下载；没有放宽文本正则候选。
  - Sandbox 下载请求改用 `tool_params` 并携带标准 `opts`；Bridge 测试 mock 同步
    到真实协议。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/executors/http.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/artifact_pipeline.py`
  - `packages/agentflow-runtime/tests/test_http_executor.py`
  - `packages/agentflow-runtime/tests/test_artifact_collect_e2b.py`
  - `frontend/bridge/tests/test_artifact_storage_e2e.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - HTTP executor、Artifact pipeline 和 LLM timeout 相邻定向测试共 19 项通过。
  - Bridge Local Sandbox 文本与二进制下载契约 2 项通过。
- 遗留事项：
  - 新增问题 7 的文本路径误识别本轮未改；本条特意只放行 structured artifact，
    避免扩大误报面。

### CHG-20260827-016：并行 Batch 改为部分成功语义

- 日期：2026-08-27
- 目标：解决新增问题 9；一个并发 Skill 分支失败时保留失败事实，但不把其余成功
  分支和整个对话强制标成失败。
- 诱发原因：
  - C++ ALL Join 原先只要任一 terminal result 失败就聚合成 `skill_success=false`，
    并设置 dialogue 级 `skill_failed`。
  - Python Coordinator 因此选用全失败 Prompt，可能忽略成功结果或重新执行已经
    成功的 Skill。
- 改动内容：
  - C++ 与 Python 聚合状态统一为三值：全部成功 `true`、部分成功 `partial`、
    全部失败 `false`。
  - 单分支失败仍保留分支自己的失败状态和 `skill-failed` FlowTrace；只有全部
    分支失败才设置 dialogue 级 `skill_failed`。
  - 增加 partial decision Prompt，要求使用成功结果、只在确有必要时重试失败分支，
    不因兄弟分支失败而重跑成功 Skill。
  - 未改变 ALL Join、原 dispatch 顺序、并发槽位回填、消费屏障或最终清理时机。
- 涉及文件：
  - `src/agent/agent.cpp`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/prompts/coordinator.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/prompts/__init__.py`
  - `tests/unit/src/test_agent_gtest.cpp`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 5 个并发分支中 4 成功、1 失败的 C++ 回归通过：Batch 只触发一次 Coordinator，
    聚合状态为 `partial`，最终响应不含全局 error，失败分支仍有失败事件。
  - C++ core 全量 309 项、Coordinator/Skill 定向 22 项均通过。

### CHG-20260827-017：让 LLM 重试共享单次调用的总超时预算

- 日期：2026-08-27
- 目标：解决新增问题 10；配置 120 秒的 LLM 调用不能因为传输重试串行叠加到
  约 251 秒。
- 诱发原因：
  - `request_timeout` 原先作为每次 `requests.post` 的 read timeout；首次耗尽后
    transient retry 会重新获得完整 120 秒预算，再叠加退避时间。
- 改动内容：
  - 单次 `chat_completion` 入口创建 monotonic deadline，所有重试共享
    `request_timeout` 总预算。
  - 每次连接、读取、SSE 行检查和重试退避都使用剩余时间；剩余预算不足时直接
    返回明确的 total timeout 错误，不再启动下一次完整超时请求。
  - 未采用进程级 signal 定时器，避免并发请求互相中断。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
  - `packages/agentflow-runtime/tests/test_openai_compatible_client.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 确定性时钟测试模拟首个请求耗尽 120 秒，确认配置两个 retry 时也只发起一次
    HTTP 请求并返回 total timeout。
  - runtime Python 全量测试 370 项通过、4 项跳过。
- 本轮范围：
  - 按用户要求，新增问题 1、4、7、8 暂不修改；本轮只解决 2、3、5、6、9、10。
### CHG-20260827-018：让 LLM Hook 使用精确 call_id，并补充并发安全枚举键

- 日期：2026-08-27
- 目标：并行 Coordinator/Skill 的每次 LLM 调用都能归因到自己的 request、
  response 和 meta，不能通过“索引中最后一项”猜测。
- 诱发原因：
  - Recorder 生成的 call id 原先没有返回调用点；sessionlog Hook 只能读取共享
    index 的最后一项，并发时可能绑定到兄弟调用。
  - 旧 index 是 JSON 数组的读—追加—覆盖，多进程同时更新会丢失 call id。
- 改动内容：
  - Recorder 在成功和错误响应上返回内部字段 `_agentflow_call_id`，三个 LLM
    调用点将其显式传给 after-LLM Hook；Hook 仅在兼容旧调用方时回退到 latest。
  - 每个调用额外写唯一键
    `traces/index_entries/{dialogue_id}/{call_id}`，避免并发写覆盖；保留旧列表供
    现有消费者兼容。
  - provider 原始 response 先落盘，再向运行态对象加入内部 call id，不污染所保存
    的 provider 响应。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/recorder.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_agent_loop.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `packages/agentflow-runtime/tests/test_recorder_skill_name.py`
  - `docs/sessionlog/tests/test_hook.py`
- 验证结果：
  - 覆盖显式 call id 优先于陈旧 index、成功/失败响应归因和唯一索引键。
  - Runtime Python 全量 376 项通过、4 项跳过；sessionlog 23 项通过。

### CHG-20260827-019：修正跨进程共享 trace 的校验边界和孤儿父 span

- 日期：2026-08-27
- 目标：同一个 workflow trace 下的多个 Python 进程可以形成可校验的执行图，
  且所有持久化 parent 都真实存在。
- 诱发原因：
  - sessionlog 的 `event_seq` 是进程内序号，但 Reader 原先只按 trace_id 分组，
    会把不同 pid 的相同序号误判为重复，并把它们合并成一个图节点。
  - `skill.end` 将自身 span id 写成 parent，形成自环。
  - Worker 将 C++ NODE span 写入 `AF_PARENT_SPAN_ID`，但该 NODE span 不进入
    Python sessionlog 文件，Reader 看到的是无法解析的外部父节点。
- 改动内容：
  - 校验器按 `(trace_id, pid)` 验证事件序号和 source_event_seq；图的执行节点
    使用 `trace_id:pid{pid}`，不再合并兄弟进程。
  - `skill.end` 不再把自己的 span id作为 parent。
  - Worker 继续传播 workflow trace id，但暂不注入未持久化的 NODE parent；
    等 C++/Python 共用同一 span 存储后再恢复真实父子边。
- 涉及文件：
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/sessionlog_reader.py`
  - `docs/sessionlog/tests/test_hook.py`
  - `docs/sessionlog/tests/test_reader.py`
  - `src/runtime/worker/worker.cpp`
  - `tests/unit/src/test_runtime_master_worker_gtest.cpp`
- 验证结果：
  - 覆盖共享 trace 的不同 pid、跨进程 source 缺失、图节点隔离和 self-parent。
  - sessionlog 23 项通过；Worker trace 定向测试通过；其余 C++ core 308 项通过。
- 风险说明：
  - Worker 执行入口属于 HIGH 风险；本次只移除无效 parent 环境变量，trace id、
    业务输入和执行逻辑不变。

### CHG-20260827-020：使用受限二进制接口回收 Local Sandbox Artifact

- 日期：2026-08-27
- 目标：图片、PDF 等二进制 Artifact 必须逐字节回收，不能经 UTF-8/Latin-1
  文本转换损坏。
- 诱发原因：
  - 原下载链借用 `/exec read_file`，返回文本后再编码为字节；任意非 UTF-8
    内容都会被替换或无法重新编码。
- 改动内容：
  - Sandbox 增加 `POST /artifact/download`，以 base64 返回文件字节和精确大小。
  - 接口仅允许既有 Artifact 后缀白名单，拒绝 traversal、`/upload(s)/` 输入和
    非 Artifact 文件，并继续使用 workspace realpath 边界检查。
  - Artifact pipeline 严格 base64 解码并核对声明大小后才落盘，不再经过文本编码。
- 涉及文件：
  - `docker/sandbox/http_server/sandbox_http_server.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/artifact_pipeline.py`
  - `packages/agentflow-runtime/tests/integration/test_file_tools_sandbox_integration.py`
  - `frontend/bridge/tests/test_artifact_storage_e2e.py`
- 验证结果：
  - 32 KiB、覆盖全部 0..255 字节的二进制往返逐字节相等。
  - traversal、upload 路径和 `.env` 下载均被拒绝；Artifact 定向 17 项通过。
  - Runtime Python 全量 376 项通过、4 项跳过。

### CHG-20260827-021：让 Batch 消费屏障和失败标记反映真实终态

- 日期：2026-08-27
- 目标：已解析 Batch 不因 DataBus 短暂写失败永久卡住 Final Gate；旧的全失败
  标记也不能污染后续部分成功或全部成功的 Batch。
- 诱发原因：
  - `DataBusClient.put()` 写失败返回 `False`，Coordinator 原先只捕获异常，
    因而会把失败的 `:consumed` 写入当成功，C++ Final Gate 永远等待。
  - C++ 仅在全部失败时写 `skill_failed=true`，但成功 Batch 不清理旧值。
  - `remove_framework_data()` 只删除 Runtime DataBus，没有处理 Agent 尚未挂载
    Runtime DataBus 时使用的内存回退表。
- 改动内容：
  - Coordinator 对 consumed marker 做最多 3 次有界重试并检查布尔结果；耗尽后
    进入既有 `batch_result_error`，同时显式记录 consume_error。
  - ALL Join 仍只有“全部失败”写全局失败；只要至少一个分支成功，就删除旧
    `skill_failed`，不改变单分支失败事实和 `partial` 语义。
  - 现有 framework-data 删除 helper 增加线程安全的内存表回退，不新增 API。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `src/agent/agent.cpp`
  - `src/agent/builtin_dispatcher.cpp`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
  - `tests/unit/src/test_agent_gtest.cpp`
- 验证结果：
  - consumed 前两次写失败、第三次成功的回归通过。
  - 5 个分支乱序完成、4 成功 1 失败时只触发一次 Coordinator，最终为
    `partial`、不带全局 error，重复 completion 保持幂等。
  - Runtime Python 全量 376 项通过、4 项跳过；其余 C++ core 308 项通过。
- 风险说明：
  - Python Batch 消费和 C++ ALL Join 均为 HIGH 风险主链；未改变调度顺序、
    并发槽位、失败分支隔离或 Final Gate 条件。

### CHG-20260827-022：保留 Hook 原地追加的 Skill Artifact

- 日期：2026-08-27
- 目标：`before_skill_complete` Hook 使用
  `ctx.artifacts.append(...)` 注入的产物必须进入 Skill 结果和后续轨迹。
- 诱发原因：
  - `ctx.artifacts` 默认与 `generated_artifacts` 是同一个 list；原代码仅在
    list 对象身份改变时重写 `artifacts_json`，原地 append 因此被遗漏。
- 改动内容：
  - 正常完成与重复 Tool Loop 终止两个出口都在 Hook 后从
    `ctx.artifacts` 重新构造列表并序列化。
  - 未新增路径扫描、文件下载或状态转换，Hook 之外的 Artifact 行为不变。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py`
- 验证结果：
  - 执行级回归先稳定复现 `append` 后 DataBus 仍为 `[]`，修复后保存
    `outputs/hook-result.png`。
  - Skill 控制流 6 项、Runtime Python 全量 376 项通过，4 项跳过。
- 风险说明：
  - `skill_executor.execute` 属于 HIGH 风险入口；生产改动仅为两个完成出口各
    两行重新序列化。


### CHG-20260827-023：让 emit_event 真正进入活动 Span

- 日期：2026-08-27
- 目标：通过 `observability.emit_event()` 产生的瞬时事件必须随当前活动 Span
  写入 NDJSON，支持错误、重试和控制事件回放。
- 诱发原因：
  - `SpanContext` 只保存 span id，`span_event()` 无法取得 `TraceSpan` 对象，
    因此事件只输出 stderr preview。
  - 部分调用点同时保留 `span.add_event()` 与 `emit_event()` 兼容写法，直接启用
    持久化会生成相邻重复事件。
- 改动内容：
  - `emit_span()` 存活期间按 span id 保存活动对象，退出前移除。
  - `span_event()` 将事件追加到当前活动 Span；无活动 Span 时继续只写 stderr。
  - 仅对相邻、同名且来自两个不同 API 的兼容双写做合并；同一 API 的连续同名
    事件仍分别保存。
  - 未修改 Coordinator、Skill、Tool、DataBus 或 NDJSON schema。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/trace/emit.py`
  - `packages/agentflow-runtime/tests/test_trace_emit.py`
- 验证结果：
  - 新增事件落盘和兼容双写去重测试，trace 定向测试 6 项通过。
  - Runtime Python 全量 378 项通过、4 项跳过；语法编译和 `git diff --check`
    通过。
- 风险说明：
  - `span_event` 是 HIGH 影响面的统一观测入口；生产改动仅改变事件持久化，
    不改变任何业务状态和返回值。

### CHG-20260827-024：完整持久化 Tool Call 与最终 Tool Result

- 日期：2026-08-27
- 目标：每次 Tool 调用都能按稳定身份回放，并在下一轮 LLM 请求前持久保存
  完整结果，消除进程中途退出造成的结果空窗。
- 诱发原因：
  - 原 `skill_executor.tool_call` 只记录工具名和数量，没有逐调用的
    `tool_call_id`、顺序与参数。
  - Tool 结果只进入 messages、stderr 和聚合式 after Hook；进程若在
    下一轮 LLM 前退出，无法证明 Tool 的实际终态和完整返回值。
- 改动内容：
  - Tool Call 事件增加 `tool_call_id`、原始序号、`tool_name` 和原始
    `arguments_json`，不在观测路径重复解析参数。
  - Tool Result 在 `after_tool_call` 改写生效后按原始序号重排，记录
    `completed/error/skipped`、实际参数和未截断结果。
  - 新增只读事件 `skill_executor.tool_result`；同时进入活动 Skill Span 的
    NDJSON，并由 sessionlog Hook 同步写入 `tool.result`。
  - 观测事件返回值不参与执行；单个 Tool 的 error 只改变该结果记录的
    status，不改变 Skill、并行批次或 Workflow 终态。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/events.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/mounts.py`
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/tests/README.md`
  - `docs/sessionlog/tests/test_hook.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_tool_parallel.py`
  - `packages/agentflow-runtime/tests/test_hooks.py`
- 验证结果：
  - 两个并行 Tool 中一个成功、一个 error 时，结果仍按 call id/ordinal 对齐，
    status 分别为 completed/error，Skill 继续按原逻辑 completed。
  - 定向 48 项、sessionlog 全量 23 项通过；两个 Hook 副本字节一致。
  - Runtime Python 全量 378 项通过、4 项跳过；语法编译通过。
- 风险说明：
  - `skill_executor.execute` 是 HIGH 风险主循环；改动仅增加结果构造和只读事件，
    不改变 Tool 选择、批次调度、Hook 改写、消息回灌或 C++ 底座。

### CHG-20260827-025：区分 Provider 原始响应与 Hook 后实际响应

- 日期：2026-08-27
- 目标：Recorder、sessionlog 与线上实际使用同一份 Hook 后响应，同时保留
  Provider 原始返回用于审计和对比。
- 诱发原因：
  - Recorder 在 `after_llm` Hook 之前保存 `response`；Hook 可以原地修改
    `choices`，导致持久化分支与实际执行分支不同。
  - sessionlog 原先直接订阅干预挂载点，多个 Hook 的注册顺序会决定它读到
    修改前、中间态还是最终态。
- 改动内容：
  - 首次持久化的模型返回另存为不可覆盖的 `provider_response`；现有
    `response` 在全部 `after_llm` Hook 完成后更新为实际执行值。
  - meta 同时保存 Provider 与实际 `finish_reason`、响应摘要及
    `hook_modified`，内部关联字段不进入响应快照。
  - Coordinator v1/v2 和 Skill Executor 在干预 Hook 完成后触发现有最终
    LLM Result 观察事件；sessionlog 只在该事件写入 `llm.response`，并按精确
    `call_id` 读取 meta，不再依赖最新索引或 Hook 顺序。
  - 未改变 Hook 对业务对象的改写方式、决策解析、Tool 调用、调度或 C++ 底座。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/recorder.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_agent_loop.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/tests/test_hook.py`
  - `packages/agentflow-runtime/tests/test_recorder_skill_name.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_tool_parallel.py`
- 验证结果：
  - 覆盖 Provider 返回 `tool_calls`、Hook 改成最终文本的分支；运行结果、
    canonical response 和 sessionlog 均为 Hook 后文本，Provider 快照保持原值。
  - 定向 14 项、sessionlog 23 项通过；两个 Hook 副本字节一致。
  - Runtime Python 全量 379 项通过、4 项跳过；语法编译和
    `git diff --check` 通过。
- 风险说明：
  - Coordinator v1/v2、Skill Executor 与 Recorder 均属 HIGH 影响面；生产改动
    只补齐最终响应持久化和只读观察事件，不改变 `raw` 的使用及控制流。

### CHG-20260827-026：跨 Coordinator 轮次累计并正确归因 Artifact

- 日期：2026-08-27
- 目标：最终响应必须包含本 dialogue 所有已完成 Skill 的产物，且轨迹中的产物
  仍能关联到实际生成它的 Skill 执行。
- 诱发原因：
  - ALL-join 已能聚合同一批并行 Skill，但 Coordinator 再调度下一批后，
    `completed_skill_artifacts` 会被后一批覆盖；最后一批为空时，前一批附件消失。
  - sessionlog 的 `skill.end` 只保存执行身份和状态，没有读取 Hook context 中
    已确定的产物列表。
- 改动内容：
  - 增加 dialogue 作用域的 `_coordinator_artifacts` 账本；每次 batch 或兼容的
    单 Skill completion 都按首次出现顺序稳定去重，并在后续轮次继续使用。
  - 账本读写异常只记录 WARN；当前业务结果、批次 consumed 屏障、调度和失败
    隔离不受影响。
  - 正常 final、LLM/轮次兜底及工具循环上限出口统一使用累计产物。
  - `skill.end` 增加 `artifacts`，与同事件的 `exec_skill_key` 一起保存，不把
    早期产物错误挂到最后完成的 Skill。
  - 未修改 C++ 底座、Artifact 文件搬运、上传或 URL 生成。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/tests/README.md`
  - `docs/sessionlog/tests/test_hook.py`
- 验证结果：
  - 覆盖第一批生成图片、第二批重复一个附件并新增报告的顺序去重；后一批不会
    覆盖前一批。
  - 覆盖 max-iterations 兜底使用累计产物，以及 `skill.end` 同时保存
    exec_skill_key 和该 Skill 的 artifacts。
  - sessionlog 23 项、Runtime Python 全量 380 项通过，4 项跳过；两个 Hook
    副本字节一致，语法编译和 `git diff --check` 通过。
- 风险说明：
  - `coordinator_decide.execute` 是 HIGH 风险工作流入口；生产控制流只增加一次
    completion 产物合并，账本失败不会改变 Skill、Batch 或 Workflow 终态。

### CHG-20260827-027：补齐每次 LLM 调用的执行身份

- 日期：2026-08-27
- 目标：LLM 记录无需解析 Prompt 或 task_id 尾缀即可直接连接到 Coordinator
  轮次、Skill dispatch 和完成批次。
- 诱发原因：
  - Runtime 已注入唯一 `AF_SELF_TASK_ID`、`AF_EXEC_SKILL_KEY` 和
    `AF_BATCH_ID`，但 Recorder 只读取 task_id/skill_name，遗漏后两者。
  - `source` 与 `round_i` 只用于流式推送，没有写入 meta；Coordinator v1/v2
    也未传 round。
  - Recorder 注释仍把已经注入的 self_task_id 误写成 framework gap。
- 改动内容：
  - runtime context 增加 `batch_id()` accessor，并修正 task/skill 身份注释。
  - Recorder meta 对成功和失败响应统一增加 `exec_skill_key`、`batch_id`、
    `source`、`round`；保留现有 dialogue/session/node/task/skill 字段。
  - Coordinator v1/v2 传入实际轮次；硬压缩调用标记为
    `{node_type}_compaction`，避免与正常决策调用混淆。
  - `workflow_execution_id` 仍在 C++ 工作流终态生成，通过 exec_skill_key 与
    completion batch 连接；本次无需改 C++。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/common/runtime_context.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/recorder.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_agent_loop.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/compaction.py`
  - `packages/agentflow-runtime/tests/test_recorder_skill_name.py`
  - `packages/agentflow-runtime/tests/test_coordinator_skill_question_passthrough.py`
  - `docs/sessionlog/README.md`
- 验证结果：
  - Provider 成功和错误两条路径都保存 task_id、exec_skill_key、batch_id、
    skill_name、source 与 round。
  - Recorder/Coordinator/compaction 定向 26 项通过；Runtime Python 全量
    380 项通过、4 项跳过；语法编译和 `git diff --check` 通过。
- 风险说明：
  - `TraceRecorder.trace_call` 覆盖全部 LLM 调用，属于 HIGH 影响面；改动只扩展
    meta 和调用归因参数，不改变请求体、网络重试、模型响应或控制流。

### CHG-20260827-028：跨进程安全追加 Span 与 sessionlog

- 日期：2026-08-27
- 目标：同一 dialogue 的并行 Skill 进程不能互相破坏 NDJSON/JSONL 记录；单个
  进程中断最多损失自身尚未完成的一条记录。
- 诱发原因：
  - SpanWriter 和 sessionlog 都只使用 `threading.Lock`；每个 Python 进程各有
    一把锁，兄弟 Skill 实际同时写同一文件。
  - 前一进程在一行中途被杀后，后一进程会直接接在半行末尾，导致两条记录合并
    成一条非法 JSON。
  - sessionlog header 的“文件为空”检查也不在跨进程临界区内。
- 改动内容：
  - 保留现有按 dialogue 文件名，不改变 viewer/reader 路径契约。
  - 进程内锁外增加 POSIX `flock`，在锁内使用二进制 O_APPEND 写完整 UTF-8
    记录，并显式处理 short write。
  - 写入前检查文件尾；若不是换行，先补换行隔离 torn fragment，再写当前完整
    记录。碎片保留用于诊断，但不会吞掉后续兄弟记录。
  - sessionlog header 的空文件判定与写入进入同一跨进程临界区，避免重复 header。
  - 未修改 Span/event schema、trace_id、父子关系或业务控制流。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/trace/span_writer.py`
  - `packages/agentflow-runtime/tests/test_trace_emit.py`
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/tests/test_hook.py`
  - `docs/TRACING.md`
  - `docs/harness-rust-rewrite-baseline/README.md`
- 验证结果：
  - 三个并行进程同时写入 36 条各含 64 KiB payload 的 Span，全部独立可解析且
    span_id 无丢失。
  - Span 与 sessionlog 都覆盖“预置半行后继续写”，下一条完整记录可解析。
  - sessionlog 24 项、Runtime Python 全量 382 项通过，4 项跳过；两个 Hook
    副本字节一致，语法编译和 `git diff --check` 通过。
- 风险说明：
  - SpanWriter 是 HIGH 影响面的统一落盘出口；改动仅改变文件临界区。POSIX
    harness 使用 flock；非 POSIX 环境保留原进程内锁降级行为。

### CHG-20260827-029：将异步 Skill 完成结果接回原 Tool Call

- 日期：2026-08-27
- 目标：Coordinator 下一轮接收 Skill 完成结果时，保持 OpenAI Tool Call/Tool
  Result 语义，不再把框架内部事件伪装成用户输入，也不把 dispatch ACK 误认为执行终态。
- 诱发原因：
  - `run_skills` 提交异步任务时已立即保存一条 `role=tool` 的 dispatch ACK；
    真正结果稍后到达时没有可直接追加的未闭合 Tool Call。
  - 原实现因此把真实完成消息追加成 `role=user`，导致内部事件增加 user turn，
    并把 Skill/网页等低信任输出提升到用户权限。
  - 普通 ReAct 解析器又会把先前的 ACK 当成 Tool 已执行成功，形成一条假完成记录。
- 改动内容：
  - Skill 完成后按 `batch_id` 查找原 `run_skills` ACK，保留原
    `tool_call_id` 并原位替换为 `status=execution_completed` 的最终 Tool Result。
  - 最终 Tool Result 同时保留原 dispatch ACK，因此部分 manifest 拒绝、warning、
    accepted Skill 和真实执行结果不会互相覆盖。
  - 兼容的单 Skill 回流使用最近一条尚未完成的 dispatch ACK；历史读取失败时重建
    一组配对的 `assistant(tool_call) -> tool(result)`，不发送孤立 Tool Result，
    也不降级为 user/system 消息。
  - 首轮请求、dispatch、并行 ALL Join、批次 consumed 屏障、工具循环和 C++ 底座
    均未修改。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
  - `packages/agentflow-runtime/tests/test_coordinator_skill_question_passthrough.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 覆盖按 batch 精确替换、原 tool_call_id 不变、部分分发拒绝信息保留、无 user
    完成消息，以及历史缺失时重建合法 Tool 配对。
  - Coordinator 定向 25 项通过；Runtime Python 全量 384 项通过、4 项跳过；
    Python 语法和 `git diff --check` 通过。
- 风险说明：
  - `coordinator_decide.execute` 是 HIGH 风险 v1 入口；改动只替换完成消息的
    历史承载方式，不改变 Skill 是否调度、成功判定、批次状态或最终回复出口。
  - 该协议修复同时关闭问题清单中“Skill 完成结果被放成 role=user”和
    “run_skills 的 role=tool 只是调度确认”两个同根问题。

### CHG-20260827-030：分离 Dialogue 生命周期与 Coordinator 激活

- 日期：2026-08-27
- 目标：一个 dialogue 在轨迹中只能有一个开始和一个终止；Skill 完成导致的
  Coordinator 子进程重入必须表达为决策激活，而不是新的会话。
- 诱发原因：
  - v1 每次执行 `coordinator_decide.execute()` 都触发
    `agent.dialogue.start`，包括并行 ALL Join 后的完成回流。
  - sessionlog 将每次该事件都落为 `session.start`，Reader 又依赖这个错误类型
    建回传边，导致同一 dialogue 多个开始，生命周期统计失真。
  - 并行完成时 Coordinator 的 `completed_skill` 可能是聚合标签，按 Skill 名称
    无法把全部 `skill.end` 连接到唯一的 ALL-join 激活。
- 改动内容：
  - v1 仅在首轮触发 `agent.dialogue.start`；每次首轮或 completion 重入都通过
    现有只读 `coordinator.trigger` Hook 表达一次 Coordinator activation。
  - batch completion 即使没有单一 completed Skill，也根据
    `AF_COMPLETED_BATCH_KEY` 正确识别为 completion trigger；轮次判定复用同一布尔值。
  - sessionlog 新增 `coordinator.activation` 事件；`session.start` 只表示整个
    dialogue 起点，`session.end` 继续由 CHG-013 的统一终态出口保证一次。
  - `skill.end` 保存 `AF_BATCH_ID`；Reader 优先用 batch_id 将同批所有 Skill
    分别回连到唯一 activation，旧单 Skill 轨迹继续按 skill_name + 时序兼容。
  - 未修改 dispatch、BatchState、final gate、LLM/Tool 执行或 C++ 底座。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/sessionlog_reader.py`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/tests/conftest.py`
  - `docs/sessionlog/tests/test_hook.py`
  - `docs/sessionlog/tests/test_reader.py`
  - `docs/sessionlog/tests/README.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 执行级测试确认首轮事件顺序为 dialogue.start → coordinator.trigger；Batch 回流
    只有 coordinator.trigger，不产生第二个 dialogue.start。
  - Reader 确认两个不同 Skill 的同批 `skill.end` 形成两条 return 边，目标均为
    同一次聚合 activation。
  - 生命周期/sessionlog 定向 46 项通过；Runtime Python 全量 384 项通过、4 项
    跳过；sessionlog 全量 26 项通过；两个 Hook 副本字节一致，语法与
    `git diff --check` 通过。
- 风险说明：
  - `coordinator_decide.execute` 是 HIGH 风险入口；本次改变的是 Hook 事件口径，
    不改变业务控制流。依赖旧错误语义、把 `agent.dialogue.start` 当每次激活的
    外部 Hook 应迁移订阅 `coordinator.trigger`。

### CHG-20260827-031：对齐 Workflow JSON 与 Python stdout ABI

- 日期：2026-08-27
- 目标：Workflow 声明必须准确描述 Python 节点实际跨边界输出，为后续 Rust typed
  struct 与 Protobuf adapter 提供唯一、可测试的基线。
- 诱发原因：
  - 三份 Skill workflow 声明 `response/status/artifacts_json`，但 Python
    Skill Executor 实际稳定输出 `skill_result/status/skill_name/rounds_used/
    artifacts_json`，错误出口还可能输出 `error`。
  - Coordinator workflow 声明 `next_skills/coordinator_status`，前者实际是
    dialogue-scoped DataBus 控制键，后者没有稳定 stdout 输出。
  - 示例包、资源包和前端模板的 Skill inputs 也发生漂移，只有前端模板声明了
    并行执行所需的 `batch_id`。
- 改动内容：
  - 三份 Skill workflow 统一声明 `skill_result`、`status`、`skill_name`、
    `rounds_used:int`、`artifacts_json` 和可选 `error`，删除不存在的
    `response`。
  - 三份 Coordinator workflow 只声明实际可能从 stdout 出现的
    `response/status`；删除 DataBus continuation `next_skills` 和未稳定输出的
    `coordinator_status`。
  - 示例包与资源包补齐 `batch_id` 输入，使三份 Skill input contract 相同。
  - 新增参数化契约测试，逐份解析六个 JSON 并精确比较 input/output 名称和类型。
  - 更新 v1 代码解读和 Rust 重写基线，明确 stdout result 与 DataBus continuation
    是两条不同通道。
  - 未修改 Python 主循环、DataBus 写入、Runtime 解析或 C++ 底座。
- 涉及文件：
  - `examples/agent/outfit-assistant/skill_workflow.json`
  - `examples/agent/outfit-assistant/coordinator_workflow.json`
  - `resource/agent-packages/outfit-assistant/skill_workflow.json`
  - `resource/agent-packages/outfit-assistant/coordinator_workflow.json`
  - `frontend/bridge/templates/agent_template/harness_agent/skill_workflow.json`
  - `frontend/bridge/templates/agent_template/harness_agent/coordinator_workflow.json`
  - `packages/agentflow-runtime/tests/test_harness_workflow_contract.py`
  - `docs/harness-v1-code-reading/README.md`
  - `docs/harness-rust-rewrite-baseline/README.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 六份 Workflow JSON 语法解析通过，6 项 ABI 契约测试通过。
  - C++ `AgentLoadingTest` 11 项全部通过，确认 Workflow/Agent 包仍可正常加载。
  - Runtime Python 全量 390 项通过、4 项跳过；`git diff --check` 通过。
- 风险说明：
  - 修改只影响 Workflow 元数据和未来静态/typed 消费者，按 MEDIUM 风险处理；
    当前动态 stdout 解析及 `next_skills` DataBus 调度路径不变。
### CHG-20260827-032：记录真实 LLM Transport Retry 次数

- 日期：2026-08-27
- 目标：使每条 LLM 轨迹准确区分一次逻辑 LLM Call 内的 HTTP 尝试与重试，
  为网络稳定性、成本和无效 Loop 分析提供可信数据。
- 诱发原因：
  - Transport retry loop 位于 `OpenAiCompatibleClient.chat_completion`，
    Recorder 中独立的 `attempt` 从未递增。
  - 因此即使前两次连接失败、第三次成功，成功 meta 的 `retries_used` 和失败
    meta 的 `retries_attempted` 仍固定为 0。
- 改动内容：
  - HTTP client 在每次真正调用 `requests.post` 前增加本次调用的
    `last_attempts`，每次 `chat_completion` 开始时归零。
  - Recorder 在 HTTP 调用结束后读取实际尝试数，并统一记录
    `transport_attempts` 与 `retries_used = max(attempts - 1, 0)`。
  - 失败轨迹保留兼容字段 `retries_attempted`，其值也改为真实 retry 数；
    成功日志中的 `retries=` 同步使用真实值。
  - 未把内部计数塞入 OpenAI-compatible response，未修改最大重试数、可重试
    错误判定、总 deadline、退避、流式去重或任何业务返回分支。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/recorder.py`
  - `packages/agentflow-runtime/tests/test_openai_compatible_client.py`
  - `packages/agentflow-runtime/tests/test_recorder_skill_name.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 回归场景确认两次瞬时连接失败、第三次成功时发起 3 次 HTTP attempt，
    Recorder 成功与失败 meta 均写入 `transport_attempts=3`、
    `retries_used=2`。
  - 定向 6 项通过；Runtime Python 全量 391 项通过、4 项跳过；语法检查和
    `git diff --check` 通过。
- 风险说明：
  - `TraceRecorder.trace_call` 是所有 Harness LLM 调用的 HIGH 风险入口；
    本次只读取逐调用客户端的观测字段，不改变 LLM 控制流或响应协议。

### CHG-20260827-033：在派发前校验 run_skills 运行时参数

- 日期：2026-08-27
- 目标：JSON 语法合法但语义结构错误的 `run_skills` 调用不能使 Coordinator
  崩溃或启动非法 Skill，而应作为 Tool Result 回到既有 correction loop。
- 诱发原因：
  - Tool schema 只能约束模型生成倾向，运行时直接对 `args.get` 和
    `spec["name"]` 解引用。
  - 缭绕的 JSON、非对象 arguments、缺失 name/prompt、错误的 load_skills
    类型及不在 agent.md 中的 Skill 名称此前没有可靠边界校验。
- 改动内容：
  - 新增纯函数 `_validate_run_skills_args`，校验 arguments 对象、非空 skills
    数组、每项对象、name/prompt、load_skills、force、exec_skill_key、reason
    类型，以及业务 Skill 白名单。
  - `handle_run_skills_call` 在任何 DataBus 写入前执行结构校验；非法输入返回
    `status=invalid_arguments`，不创建 manifest、next_skills 或 last_prompt。
  - Coordinator Tool loop 保留 JSON 解析错误，并在重复判定和 Hook 之前执行
    完整校验；错误通过原 tool_call_id 回灌给 LLM，随后沿原 correction loop
    重决策。
  - `load_skills` 只检查“非空字符串数组”，不套业务 Skill 白名单，避免阻断
    合法的额外技能文档；未修改合法派发、并行 Batch、Hook 或 C++ 底座。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 7 类 malformed 参数均返回 `invalid_arguments`，且 DataBus 零写入；
    未知 Skill 被 agent.md 白名单拒绝，合法 Skill 继续通过。
  - Coordinator 定向 36 项通过；Runtime Python 全量 399 项通过、4 项跳过；
    语法检查与 `git diff --check` 通过。
- 风险说明：
  - `coordinator_decide.execute` 是所有 v1 决策的 HIGH 风险入口；新增分支只在
    原本会崩溃或非法派发的输入上生效，合法 Tool Call 的执行路径保持不变。

### CHG-20260827-034：用完成指纹阻止跨 Coordinator 轮次的重复派发

- 日期：2026-08-27
- 目标：相同 dialogue 中已经终止的 `skill_name + prompt` 调度，在其他 Skill
  后续完成后仍能被识别，不能只防住“当前刚完成”的一个 Skill。
- 诱发原因：
  - 原 F2 只比较本次 completion 的 Skill 名和 `last_prompt`；下一批 Skill
    完成后，较早完成项便退出防护集合。
  - 批内任意 spec 的 `force=true` 还会把整个批次的重复防护关闭，权限范围过大。
- 改动内容：
  - Batch completion 根据每个结果的 `exec_skill_key` 回读原 dispatch manifest，
    取得精确 name/prompt，并写入 dialogue-scoped SHA-256 完成指纹。
  - 指纹采用独立幂等 DataBus key，不进行共享 JSON 列表的读改写；失败只告警，
    继续使用原“当前 completion”防护，不改变业务终态或 Batch consumed 屏障。
  - legacy 单 Skill completion 在有可靠 `last_prompt` 时写入同一格式指纹。
  - 新派发在当前 completion 检查之后查询整个 dialogue 的精确完成指纹；相同
    name 但新 prompt、其他 dialogue、顶层 force 或该 spec 自己的 force 均放行。
  - per-spec force 只豁免对应 Skill，不再替同批其他分支绕过重复检查；未修改
    C++ 底座、合法 dispatch manifest 或并行 Batch 协议。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_coordinator_duplicate_dispatch_block.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 回归场景确认 weather-search 完成后，即使随后完成的是 outfit-stylist，
    相同北京天气 prompt 仍被拦截；上海天气 prompt、另一 dialogue 和 force 放行。
  - 确认一个强制重跑的兄弟 Skill 不会解锁未带 force 的重复项。
  - Coordinator 定向 38 项通过；Runtime Python 全量 401 项通过、4 项跳过；
    语法检查与 `git diff --check` 通过。
- 风险说明：
  - `coordinator_decide.execute` 是 HIGH 风险入口；本次只扩展重复拒绝条件，
    marker 读写均为旁路、失败开放，不影响 Skill completion 或正常派发。

### CHG-20260827-035：Dispatch Manifest 断链时禁止执行 unknown-skill

- 日期：2026-08-27
- 目标：Skill Runner 无法取得其 dispatch manifest 时必须明确失败并可回放，
  不能把原始用户输入交给通用 LLM/Tool 以 `unknown-skill` 继续执行。
- 诱发原因：
  - 旧代码将 manifest 缺失、DataBus 不可用、JSON 损坏和缺少 skill_name
    全部降级为 `unknown-skill`，随后仍加载 fallback prompt 和工具。
  - 这会把调度/基础设施断链伪装成业务成功，并使结果无法连接到原 Dispatch。
- 改动内容：
  - 新增 dispatch manifest preflight，校验 exec_skill_key、DataBus 连接、JSON
    对象、非空 skill_name/prompt 和 load_skills 字符串数组。
  - 删除 `unknown-skill + AF_INPUT` fallback；任一 preflight 错误统一输出
    `status=dispatch_manifest_error`、`rounds_used=0`、空 artifacts 和稳定
    error code，并写入 `skill_result:skill-runner`。
  - 失败路径建立并闭合 Skill span，触发同一
    `skill_executor.trigger/after_skill_complete` Hook，因此 sessionlog 得到
    可关联 exec_skill_key 的 skill.start/skill.end；Hook 失败不改业务终态。
  - preflight 在加载 Skill 文档、Tool registry、模型配置及调用 LLM 之前执行；
    合法 manifest 后的原执行路径未修改，C++ 底座未修改。
  - 原执行级测试补充显式合法 manifest，不再依赖错误 fallback。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_dispatch_manifest.py`
  - `packages/agentflow-runtime/tests/test_interrupt_poll.py`
  - `packages/agentflow-runtime/tests/test_coordinator_skill_question_passthrough.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 覆盖 key 缺失、DataBus 不可用、manifest 不存在、非法 JSON、非对象及字段
    错误；执行级测试确认 LLM、Tool registry、Skill docs 和模型配置均未调用。
  - 定向 28 项通过；Runtime Python 全量 411 项通过、4 项跳过；语法检查与
    `git diff --check` 通过。
- 风险说明：
  - `skill_executor.execute` 是所有业务 Skill 的 HIGH 风险入口；新分支只替换
    原本不可信的 unknown fallback，合法 dispatch 行为保持不变。

### CHG-20260827-036：按 tool_call_id 校验 after_tool Hook 结果改写

- 日期：2026-08-27
- 目标：Hook 改写 Tool 结果时必须保持调用身份、数量、实参和返回类型完整；
  畸形改写不能通过 `zip` 静默丢失或错配下一轮 LLM 的 role=tool 消息。
- 诱发原因：
  - 现有局部防护只检查“列表等长、元素是三元组”，没有校验 name、args、result
    类型，更没有让 Hook 携带或核对 tool_call_id。
  - 批内重复 Tool 名、Hook 重排或错误缩短列表时，位置绑定不足以证明结果身份。
- 改动内容：
  - after_tool Hook 默认收到每项包含 `tool_call_id/ordinal/tool_name/arguments/
    result` 的结构化 records；传给 Hook 的 args 使用深拷贝，不能原地污染原始快照。
  - 新增纯校验 helper：结构化 records 必须覆盖且只覆盖原 call_id，拒绝未知/重复
    ID、name/args 不一致和非字符串 result，并按原 tool_calls 顺序归一化。
  - 兼容旧三元组格式，但严格要求逐位置 name/args 与实际执行一致、result 为字符串；
    混合格式或数量不符一律拒绝。
  - 非法改写保留全部原始 Tool 结果；最终 Tool Result 事件新增
    `after_hook_rewrite_status/error`，可区分 unchanged、applied 与
    ignored_invalid。
  - after_tool 挂载点显式标记 `can_intervene=True`；未修改工具调度、并行批次、
    Artifact 收集或下一轮 LLM 协议。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_tool_parallel.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 覆盖按 call_id 逆序返回后正确复位、数量不足、未知 ID、args 错配、result 类型
    错误及 legacy 错序；执行级测试确认畸形空列表后两个 Tool message 均保留。
  - 定向 51 项通过；Runtime Python 全量 417 项通过、4 项跳过；语法检查与
    `git diff --check` 通过。
- 风险说明：
  - 修改位于 `skill_executor.execute` 的 HIGH 风险 Tool 回灌边界；非法 Hook
    输出采用保守回退，未订阅或未改写 Hook 的结果与原执行路径一致。
### CHG-20260827-037：空最终响应不再标记为 Skill 成功

- 日期：2026-08-27
- 目标：Provider 已计费但最终未返回 content/tool_calls 时，不能输出
  `status=completed` 和空 `skill_result`。
- 诱发原因：
  - 原空响应检查只覆盖 usage 全零；usage 非零的空 content 会落入普通完成分支。
  - 普通完成分支固定写 completed、Skill span OK，C++ 随后沿业务成功路径上报。
- 改动内容：
  - 最终分支先将非字符串 content 归一为空串并判定空结果。
  - 空结果输出 `status=llm_error`、稳定的非空错误说明和 `error` 字段，
    复用既有 C++ 失败分类，不修改底座。
  - progress、`skill_executor.empty_result`、`skill.end` 与 Skill span 同步记录
    错误终态；正常非空最终文本路径保持不变。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 新增 nonzero usage + finish=stop + content 为空的执行级回归，确认 DataBus、
    Hook、progress 和 span 均为失败语义。
  - 定向 7 项通过；Runtime Python 全量 418 项通过、4 项跳过；语法检查与
    `git diff --check` 通过。
- 风险说明：
  - `skill_executor.execute` 是 HIGH 风险入口；本次只收紧无 tool_calls 的空
    最终文本终态，不改变正常回答、Tool 调用、并行或 workflow 成败。

### CHG-20260827-038：Timeout 与 Max Rounds 返回可归因的中间结果

- 日期：2026-08-27
- 目标：强制终止时不能把消息列表最后一项（Tool JSON、恢复提示或预算提醒）
  直接作为业务结果交给 Coordinator。
- 诱发原因：
  - node timeout 与 max rounds 都直接读取 `messages[-1].content`，没有检查 role。
  - 最后一项由控制流决定，可能是内部 user 提示，也可能是没有来源标签的 Tool 返回。
- 改动内容：
  - 复用已有 `_summarize_tool_history`，只汇总 assistant/tool 证据，排除 system、
    user、recovery 与 reminder 消息。
  - node timeout 和 max rounds 均使用该确定性汇总；max-rounds 事件的
    `response_length` 同步改为实际返回长度。
  - 无任何 assistant/tool 证据时返回明确的“最终回答前终止”说明；不新增一次
    LLM 汇总调用，不改变终止 status、轮数、Artifact 或 C++ 行为。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 覆盖内部 recovery 提示排除、Tool 结果来源标签及 max-rounds 执行出口。
  - 定向 19 项通过；Runtime Python 全量 419 项通过、4 项跳过；语法检查与
    `git diff --check` 通过。
- 风险说明：
  - `skill_executor.execute` 是 HIGH 风险入口；本次仅替换两个失败终态的
    `skill_result` 取值，不改变正常完成、Tool 执行或调度流程。

### CHG-20260827-039：SpanWriter 写盘失败不再中断业务

- 日期：2026-08-27
- 目标：Trace 目录只读、磁盘写失败或 span 序列化异常不能改变 Harness 业务终态，
  也不能覆盖正在传播的真实业务异常。
- 诱发原因：
  - `emit_span` 的 `finally` 直接调用 `SpanWriter.write_span`，写盘异常会从
    context manager 退出路径向上传播。
  - 若业务块本身已抛错，finally 中的写盘异常还会遮蔽原异常。
- 改动内容：
  - `emit_span` 在 span 栈 pop、活动对象移除后隔离 `write_span` 异常。
  - 写盘失败只输出 best-effort `stage=span_write_failed` stderr preview，
    包含 trace/span/name、异常类型和错误摘要；连 stderr 也失败时静默降级。
  - `SpanWriter.write_span` 本身仍保留抛错语义，直接调用者和写盘测试可继续诊断；
    仅统一业务可观测性入口负责故障隔离。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/trace/emit.py`
  - `packages/agentflow-runtime/tests/test_trace_emit.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 模拟 PermissionError，确认成功业务正常返回、失败业务仍抛原 RuntimeError、
    span 栈清空且两次写盘失败均有告警。
  - Trace 定向 9 项通过；Runtime Python 全量 420 项通过、4 项跳过；语法检查与
    `git diff --check` 通过。
- 风险说明：
  - `emit_span` 是 Coordinator/Skill/LLM/Tool 共用的 HIGH 风险入口；本次只
    隔离其持久化副作用，不修改 span 状态、父子链、业务返回或原异常。

### CHG-20260827-040：同名 Tool 覆盖时同步替换 Schema 与实现

- 日期：2026-08-27
- 目标：agent/skill 层覆盖 builtin 同名 Tool 时，LLM 只能看到与最终执行实现
  一致的一份 function schema。
- 诱发原因：
  - `_modules[name]` 使用字典覆盖，最终执行后加载实现。
  - `_tools` 使用列表无条件 append，旧 schema 仍会连同新 schema 一起发给模型。
- 改动内容：
  - `_load_dir` 先构造同名 Tool 的完整 schema；若名称已注册，按原索引替换，
    否则追加。
  - 实现映射仍由后加载 module 覆盖，因此 schema 与 execute 始终来自同一层。
  - 保持原 Tool 顺序，未修改目录优先级、执行参数、Sandbox 路由或 MCP 工具。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/tool_registry.py`
  - `packages/agentflow-runtime/tests/test_tool_registry_override.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 构造 builtin `command` schema 与 skill `script` schema 的同名 Tool，确认
    最终仅保留 skill schema 且执行 skill 实现。
  - Tool registry/MCP/路径/归属定向 34 项通过；Runtime Python 全量 421 项通过、
    4 项跳过；语法检查与 `git diff --check` 通过。
- 风险说明：
  - `_load_dir` 上游覆盖 Skill Executor 与 v2 Coordinator 的全部本地 Tool，

### CHG-20260827-041：约束 Skill ID、load_skills 与文件路径边界

- 日期：2026-08-27
- 目标：模型参数、旧调用方或受污染 manifest 不能借 Skill 名读取/加载
  `harness/skills` 根目录之外的 SKILL.md 或 Python Tool。
- 诱发原因：
  - 原实现直接把 `skill_name/load_skills` 拼进 `os.path.join`。
  - Coordinator 只对白名单校验主 Skill，未校验 `load_skills`；Executor
    preflight 只检查字符串类型。
- 改动内容：
  - 新增单一 Skill 目录解析边界：拒绝空白变体、`.`、`..`、路径分隔符、
    NUL、绝对/穿越路径及 realpath 后逃逸根目录的 symlink。
  - SKILL.md 文件、skill scripts 目录和单个 Python Tool 文件均再次检查
    realpath 父目录，阻断文件级/目录级 symlink 逃逸。
  - manifest preflight 对主 Skill 与 load_skills 使用同一边界；Coordinator
    同时要求每个 `load_skills` 属于 agent.md 可用 Skill 集合。
  - 未改变合法 Skill 名、Skill 文档替换、Tool 层级优先级或 Sandbox 路径展示。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skills_loader.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/tool_registry.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/tests/test_skill_name_security.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 覆盖 traversal、绝对路径、Skill 目录 symlink、SKILL.md symlink、scripts
    symlink、Tool 文件 symlink和未注册 load_skills。
  - 定向 68 项通过；Runtime Python 全量 427 项通过、4 项跳过；语法检查与
    `git diff --check` 通过。
- 风险说明：
  - Skill loader、Tool registry、Executor 和 Coordinator 均为 HIGH 影响面；
    所有新增条件只拒绝越界/未注册输入，合法调用路径保持不变。

### CHG-20260827-042：区分并重试明确的 Provider HTTP 瞬态错误

- 日期：2026-08-27
- 目标：Provider 返回限流或临时服务故障时可以在既有预算内恢复，同时让轨迹明确
  记录重试原因和最终 HTTP 状态。
- 诱发原因：
  - 原实现把所有非 200 响应包装为同一种 `_ApiError`，随后立即抛出，不进入既有
    transport retry；429 与可恢复 5xx 因而只尝试一次。
  - Recorder 只记录最终异常/响应，无法判断一次 LLM 调用内部是否发生过 HTTP 重试。
- 改动内容：
  - 仅将 408、429、500、502、503、504 标记为可重试；400、401、501 等确定性错误
    仍立即失败。
  - 复用原有最大次数、指数退避和总请求 deadline，不增加新的外层循环；流式响应
    一旦收到首个 SSE 数据仍禁止重试，避免重复输出。
  - Client 保存本次调用的 `last_retry_reasons` 与 `last_http_status`，Recorder 将其写入
    LLM span meta，便于轨迹重放解释尝试次数和终态。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/recorder.py`
  - `packages/agentflow-runtime/tests/test_openai_compatible_client.py`
  - `packages/agentflow-runtime/tests/test_recorder_skill_name.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 覆盖六种可重试状态先失败后成功，以及 400、401、501 不重试；定向 40 项通过。
  - Runtime Python 全量 436 项通过、4 项跳过；语法检查与 `git diff --check` 通过。
- 风险说明：
  - Provider Client 与 Recorder 是 HIGH 影响面；本次只扩展明确瞬态 HTTP 状态的既有
    retry 路径和 span meta，不改变正常响应、确定性错误或流式中途失败语义。

### CHG-20260827-043：标记流式 token usage 的真实来源

- 日期：2026-08-27
- 目标：轨迹明确区分 Provider 返回的 token usage 与本地粗略估算，避免成本、
  loop 效率和收敛分析混用两种口径。
- 诱发原因：
  - Provider 不返回流式 usage 时，Client 使用字符数除以 2 估算 completion token，
    Recorder 却按普通 usage 写盘。
  - Provider 常在 `choices=[]` 的最后一个 SSE chunk 返回 usage；原循环先跳过空
    choices，导致真实 usage 被丢弃并被估算值替代。
- 改动内容：
  - Client 为每次调用维护 `last_usage_source=provider|estimated|missing`；
    估算时同时记录固定版本 `chars_div_2_v1`。
  - 在判断 choices 前提取 SSE usage，使 usage-only 尾块可以覆盖本地估算。
  - Recorder 将 `usage_source` 与 `estimator_version` 写入成功和失败 LLM span
    meta；OpenAI 兼容响应字典及 token 数值字段保持不变。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/recorder.py`
  - `packages/agentflow-runtime/tests/test_openai_compatible_client.py`
  - `packages/agentflow-runtime/tests/test_recorder_skill_name.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 覆盖无 usage 的估算路径、usage-only SSE 尾块和 Recorder meta；定向 37 项通过。
  - Runtime Python 全量 438 项通过、4 项跳过；语法检查与 `git diff --check` 通过。
- 风险说明：
  - `chat_completion` 与 `TraceRecorder.trace_call` 是 HIGH 影响面；本次只增加
    调用级 telemetry 与 span meta，并优先保留 Provider 已返回的真实 usage。

### CHG-20260827-044：多 Skill progress 使用真实业务 Skill 名

- 日期：2026-08-27
- 目标：并行 dispatch 后，每条 progress 的 `skill` 字段只表示一个可关联的业务
  Skill，不能把多个名称拼成伪 Skill。
- 诱发原因：
  - 展示文本先用中文顿号连接 `_dispatched_skills`。
  - progress 随后对该展示文本按空白 `split()`；顿号不会被拆开，两个名称会作为
    一个组合字符串写入 metadata。
- 改动内容：
  - progress 循环直接遍历有序字典 `_dispatched_skills` 的 key。
  - 展示标签、dispatch span、事件、manifest、batch 和并行调度逻辑均保持不变。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `examples/agent/outfit-assistant/tests/test_coordinator_decide_databus.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 模拟同批 outfit-stylist 与 forge-image-gen，确认产生两条独立 progress 且
    `source=coordinator`；Coordinator 定向 39 项通过。
  - 同步固定旧 dispatch 清理用例的 Skill 白名单前置，示例 DataBus 测试文件整体
    4 项通过。
  - Runtime Python 最终全量 440 项通过、4 项跳过；语法检查与
    `git diff --check` 通过。
- 风险说明：
  - `coordinator_decide.execute` 是 HIGH 风险入口；业务改动仅一行迭代源，
    不改变任何 dispatch 接受、失败隔离、控制权转移或返回路径。

### CHG-20260827-045：Tool skip 与执行结果按原调用顺序回灌

- 日期：2026-08-27
- 目标：同一 assistant 消息内混合“Hook 跳过”和“实际执行”的 Tool call 时，
  下一轮 LLM 消息、Tool Result 事件和原始 tool_calls 使用相同顺序。
- 诱发原因：
  - 被跳过的 Tool message 在过滤阶段立即 append。
  - 保留 Tool 的结果在并行/串行批次结束后才 append，因此 mixed 场景会形成
    `skipped... + executed...`，偏离原 ordinal。
- 改动内容：
  - 跳过和执行结果先分别缓存到 `_tool_messages_by_ordinal`。
  - artifact 收集结束后统一按原 ordinal 扩展 messages。
  - 复用既有 `_tool_call_ordinals`；没有修改 Tool policy、批次划分、Hook
    result rewrite、错误状态或 artifact 处理。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_tool_parallel.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 覆盖执行 call_1、跳过 call_2、执行 call_3；下一轮 LLM messages 和结构化
    `tool_results` 均严格保持 1→2→3，且被跳过 Tool 未执行。
  - Skill Executor/Hook 定向 54 项通过；Runtime Python 最终全量 440 项通过、
    4 项跳过；语法检查与 `git diff --check` 通过。
- 风险说明：
  - `skill_executor.execute` 是 HIGH 风险入口；本次只延迟 Tool message append
    到同轮副作用结束，不改变执行结果、失败隔离、并行性或 loop 次数。

### CHG-20260827-046：为有效消息增加 dialogue-scoped 快照

- 日期：2026-08-27
- 目标：新 dialogue 覆盖 session 兼容缓存后，仍能按 dialogue 直接取得其最新有效
  Coordinator messages，避免把 session 级单值误当轨迹历史。
- 诱发原因：
  - Recorder 只写 `llm/session_messages`；同一 session 的每个 dialogue 都覆盖
    同一个 key。
- 改动内容：
  - 每次保存额外写
    `llm/conversations/{dialogue_id}/effective_messages`。
  - 旧 `llm/session_messages` 的读取、写入和返回值语义保持不变，继续用于运行时
    兼容缓存；scoped 快照失败只告警，不影响旧缓存和业务控制流。
  - 未把 snapshot 宣称为 append-only 事件；逐调用事实仍以现有 LLM trace 为准。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/recorder.py`
  - `packages/agentflow-runtime/tests/test_recorder_skill_name.py`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 连续写 dialogue-1 与 dialogue-2，确认各自快照均保留且旧缓存仍指向最新消息；
    Recorder 定向 5 项通过。
  - Runtime Python 最终全量 440 项通过、4 项跳过；语法检查与
    `git diff --check` 通过。
- 风险说明：
  - `write_session_messages` 上游覆盖 v1/v2 Coordinator 的多个 HIGH 风险出口；
    本次仅增加 fail-open 的 dialogue 快照副作用，不改变读取上下文或业务终态。

### CHG-20260827-047：拒绝单次响应中的多个 run_skills 调用

- 日期：2026-08-27
- 目标：一个 LLM 响应不能先记录多批成功 dispatch，实际却只执行最后一次写入
  `next_skills` 的 Batch。
- 诱发原因：
  - Coordinator 逐个处理同一响应中的多个 `run_skills` tool call。
  - 每次调用都写同一个 `next_skills` key，后一次覆盖前一次；Hook 却已把每次调用
    都记录为成功，造成控制事实与轨迹事实分裂。
- 改动内容：
  - 同一 assistant 响应出现多个 `run_skills` 时，在写 manifest 前整体拒绝。
  - 为每个 tool_call 回灌明确的 `invalid_arguments`，继续既有 Coordinator
    内部工具纠错循环；单个 `run_skills.skills[]` 的多 Skill 并行保持不变。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `examples/agent/outfit-assistant/tests/test_coordinator_decide_databus.py`
- 验证结果：
  - 确认非法双调用不进入 `handle_run_skills_call`、不写 `next_skills`，随后第二次
    LLM 决策可正常收敛；Coordinator 定向 36 项通过。

### CHG-20260827-048：补齐 Skill 与框架 Batch 的完整终态

- 日期：2026-08-27
- 目标：Python Skill 和未进入 Python 的底座失败分支都必须有可回放终态。
- 诱发原因：
  - `skill.end` 只有状态和 Artifact，没有最终 `skill_result/error`。
  - launch failure 等框架合成结果只存在 completion Batch，sessionlog 没有权威事件。
- 改动内容：
  - `skill.end` 保存 Hook 后最终 `skill_result`、`error` 和 Artifact。
  - 新增 `coordinator.batch_result → batch.result`，完整保存 Batch results、
    成功数、总数和聚合状态。
  - 事件只做观测；单个分支失败仍使用原部分成功语义，不改变 Workflow 控制状态。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/events.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - 对应 Runtime/sessionlog 测试
- 验证结果：
  - 覆盖 completed、tool_call_error、max_rounds_reached 和 launch_failed；
    Skill/Batch/Hook 定向 86 项通过。

### CHG-20260827-049：空答案与畸形 Provider 响应统一进入 LLM 错误终态

- 日期：2026-08-27
- 目标：已消耗 token 的空答案不能标成正常完成，畸形 `choices/message/usage`
  不能在 Recorder 中索引崩溃。
- 诱发原因：
  - 共享空响应判定额外要求 usage 全零和 `finish_reason=stop`。
  - HTTP Client 未校验非流式响应的最低 OpenAI 兼容结构。
- 改动内容：
  - 无有效 content 且无 tool_calls 即为空业务结果，不再依赖 token 计数。
  - Client 校验 root、非空 choices、`choices[0].message` 和 usage；畸形响应转换为
    既有 `_llm_error` 协议，不新增网络重试。
  - v1/v2 Coordinator 继续使用原 fallback 与终态出口。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_common.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_agent_loop.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
  - 对应 Client/Coordinator 测试
- 验证结果：
  - 覆盖非零 usage 空答案、空 choices、错误 message 和错误 usage；定向 63 项通过。

### CHG-20260827-050：Reader 校验完整终态并正确构建并行图

- 日期：2026-08-27
- 目标：离线校验能识别缺失的 Session/Tool/Skill 终态，并行边不能全部误标为
  第一个 Skill。
- 诱发原因：
  - Reader 原先只校验 seq、LLM 配对和同进程 source seq。
  - 多 exec key 的 dispatch 边固定使用 `skill_names[0]`。
  - 框架失败分支没有 Python `skill.start/end`，旧图无法表达。
- 改动内容：
  - 增加 session 起止、Tool call/result、dispatch/start/terminal 和 orphan end 校验；
    `batch.result` 可作为框架终态兜底。
  - 派发边按 exec key 与同位置 Skill 名精确映射。
  - 对 framework-only terminal 生成合成节点及 dispatch/return 边；已有 Python
    Skill 不重复生成。
  - 跨进程父链继续使用 `exec_skill_key/batch_id`；未恢复未落盘的 C++ NODE parent。
- 涉及文件：
  - `docs/sessionlog/sessionlog_reader.py`
  - `docs/sessionlog/tests/test_reader.py`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/tests/README.md`
- 验证结果：
  - Reader 22 项、sessionlog hook 13 项全部通过。

### CHG-20260827-051：空 content 的正常 dispatch 仍推送 Skill progress

- 日期：2026-08-27
- 目标：标准 tool-call 响应的 `content=""` 不能吞掉每个业务 Skill 的运行进度。
- 诱发原因：
  - progress 发送被 `msg.content.strip()` 包裹，而正常函数调用通常没有文本 content。
- 改动内容：
  - 成功 dispatch 后直接遍历已接受 Skill 推送 progress。
  - 保留 DataBus 连接判断、异常隔离、来源字段和 dispatch 控制逻辑。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - `examples/agent/outfit-assistant/tests/test_coordinator_decide_databus.py`
- 验证结果：
  - 使用空 content 同批派发两个 Skill，确认两条 progress 均存在；定向 36 项通过。

### CHG-20260827-052：拆分 Coordinator 激活、决策、完成与内部纠错计数

- 日期：2026-08-27
- 目标：并行宽度、Skill 回流与一次激活内的 LLM 自纠错不能混成同一个 Loop 指标。
- 改动内容：
  - 保留原 `round/max_iterations` 控制语义，新增 trace-only 字段：
    `activation_index`、`decision_index`、`skill_completion_index`、
    `internal_tool_correction_index`。
  - `_coordinator_decision_index` 在同一 dialogue 内递增；首触发重置。
  - 字段进入 activation、LLM request/response、Span tag 和 sessionlog。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py`
  - 两份 `sessionlog_hook.py`
  - 对应 Coordinator/sessionlog 测试
- 验证结果：
  - 一次激活内先非法 dispatch、再正常 final：decision 为 1→2、内部纠错为 0→1，
    activation 保持 1；定向 70 项通过。

### CHG-20260827-053：将有效消息快照写成 append-only 事件

- 日期：2026-08-27
- 目标：dialogue-scoped 最新快照之外，还要保存每次有效消息状态，供轨迹重放。
- 改动内容：
  - 新增 `coordinator.messages_snapshot → messages.snapshot`，每次
    `save_session_messages` 都追加完整有效 messages 和 message_count。
  - 事件写入与 Recorder 连接解耦；Hook/导入失败只告警。
  - CHG-046 的 `effective_messages` 和旧 `session_messages` 兼容缓存保持不变。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/events.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/coordinator_common.py`
  - 两份 `sessionlog_hook.py`、Reader、README 与对应测试
- 验证结果：
  - 断开 Recorder 时事件仍可产生；连续两次保存得到两条独立快照；定向 84 项通过。

### CHG-20260827-054：输入文件引用不再污染生成 Artifact

- 日期：2026-08-27
- 目标：Prompt、Tool 输出或最终文本中回显的已有输入文件，不能进入本轮生成产物账本。
- 诱发原因：
  - Skill 在任何 Tool 执行前就扫描 `task_instruction`，并把存在的文件直接作为
    `generated_artifacts` 搬运。
- 改动内容：
  - Prompt 路径改为 `referenced_input_artifacts`，不再预填生成产物。
  - Tool/最终文本的正则兜底排除这些输入引用。
  - 结构化 `artifacts/files/outputs` 保持权威，可显式重新纳入同一路径，支持
    write/edit 后产出。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/artifact_pipeline.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - 对应 Artifact/Skill 测试
- 验证结果：
  - 文本回显输入被排除，结构化同路径输出仍保留；新增及双通道 6 项、
    Sandbox/上传边界与 Skill 29 项通过。
  - Runtime Python 全量 450 项通过、4 项跳过；sessionlog + v1 Coordinator
    40 项通过。
- 风险说明：
  - 未修改 C++ 调度、BatchState、Workflow 成败或跨进程 parent span 策略。

### CHG-20260827-055：同批外部 Artifact 重名时禁止静默覆盖

- 日期：2026-08-27
- 目标：同一次 Artifact 收集中两个不同源文件不能被搬到同一个目标后互相覆盖。
- 诱发原因：
  - 外部文件统一使用 `outputs/{session}/{basename}`；同名时第二个直接覆盖第一个，
    但返回列表仍保留两个相同路径，造成轨迹数量与磁盘事实不一致。
- 改动内容：
  - 每次收集维护目标占用表；第一个文件沿用原名，后续不同源依次使用
    `stem_1.ext`、`stem_2.ext`。
  - package 内已存在于 outputs 的同批产物也先占用目标，外部同名文件不能覆盖它。
  - 不同调用/轮次仍保留原同名覆盖语义，避免改变历史 URL 指向最新版的既有约定。
  - 将旧 Artifact 测试中的 URL/版本去重 helper 指向迁移后的真实所有者
    `coordinator_common`，不恢复 v1/v2 重复实现。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/artifact_pipeline.py`
  - `examples/agent/outfit-assistant/tests/test_dispatch_and_artifacts.py`
- 验证结果：
  - 覆盖两个外部同名文件、package output 与外部同名文件；专门回归 2 项通过。
  - 示例 Artifact/dispatch 测试文件全量 75 项通过。
  - E2B/Artifact 路由 11 项通过；Runtime Python 全量 450 项通过、4 项跳过。

### CHG-20260827-056：Dispatch manifest 按分支确认并记录部分失败

- 日期：2026-08-27
- 目标：多 Skill 调度时，一个 manifest 写入失败不能把未落盘的分支伪装成已调度，
  也不能无条件取消其他已成功分支。
- 诱发原因：
  - 旧路径先构造全部 skill-runner spec，再默认写 next_skills；没有逐项核对
    DataBus put_global 的返回值。
  - 轨迹只能看到请求列表，无法区分真正可启动与写 manifest 失败的分支。
- 改动内容：
  - handle_run_skills_call 逐项确认 manifest 写入；只把成功项加入 next_skills。
  - 返回 requested、accepted、rejected、request_index、exec_skill_keys、batch_id，
    状态区分 dispatched、partially_dispatched 与 dispatch_failed。
  - 全部 manifest 失败或 next_skills 写入失败时不启动任何分支，并把失败阶段写入
    rejected；部分失败继续执行成功分支。
  - skill.dispatch 保存请求列表、接受列表和拒绝原因，采集事实与真实控制结果一致。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
  - docs/sessionlog/sessionlog_hook.py
  - examples/agent/outfit-assistant/tests/test_coordinator_decide_databus.py
  - packages/agentflow-runtime/tests/test_coordinator_run_skills_force.py
- 验证结果：
  - 覆盖全部成功、单项 manifest 失败、全部失败及 next_skills 写入失败。
  - 后续 Runtime 全量回归 452 项通过、4 项跳过。
- 风险说明：
  - handle_run_skills_call 是调度入口；改动只收紧“已成功调度”的事实定义，
    保留并发分支独立失败和部分成功语义，没有修改 C++。

### CHG-20260827-057：并行 Batch 使用稳定的分支身份和回传关联键

- 日期：2026-08-27
- 目标：并行分支不能依赖完成顺序或 Skill 名称猜测身份，派发、执行、聚合和回放必须
  使用同源的 batch_id 与 exec_skill_key。
- 诱发原因：
  - 同名 Skill、乱序完成和框架合成失败都可能让“按列表位置/名称”配对失真。
  - completion Batch 缺少足够强的逐分支身份时，Coordinator 输入与轨迹图都可能错接。
- 改动内容：
  - 每次 run_skills 生成稳定 batch_id；每个 manifest 同时保存 request_index、
    batch_id 和唯一 exec_skill_key。
  - skill-runner spec、Skill start/end、completion results 与 batch.result 透传同一组键。
  - Coordinator 读取完成 Batch 时校验 batch_id、results 和每项 exec_skill_key，
    再按 manifest 恢复准确的 Skill 名与原 prompt；完成顺序不再决定身份。
  - Batch 聚合继续允许 partial success，单分支失败不把其他成功分支改写成失败。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
  - packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py
  - packages/agentflow-runtime/src/agentflow_runtime/common/runtime_context.py
  - docs/sessionlog/sessionlog_hook.py
  - 对应 Coordinator、并行 Tool 与 SessionLog 测试
- 验证结果：
  - 覆盖双分支乱序完成、同批部分失败、框架失败结果和精确 prompt 恢复。
  - SessionLog 全量 38 项及 Runtime 全量回归通过。
- 风险说明：
  - 没有修改底座并行调度算法；新增字段是现有 DataBus/环境变量链路上的稳定身份。

### CHG-20260827-058：LLM 内容轨迹补齐 Skill 归因与有效响应事实

- 日期：2026-08-27
- 目标：每次 LLM 调用必须能直接回答“属于哪个 Coordinator/Skill、使用哪个 Provider、
  Hook 后真正进入后续逻辑的响应是什么”。
- 诱发原因：
  - Recorder 的多条 llm/traces 记录中 skill_name 为空，只能从 task_id 尾部猜测。
  - Provider 与有效响应分散在不同层，后续建树无法可靠归因。
- 改动内容：
  - Skill manifest 解析后把 skill_name、exec_skill_key、batch_id 放入当前运行上下文，
    嵌套 LLM Client 与 Recorder 直接读取，不再解析 task_id。
  - Recorder 的精确传输请求、响应和 meta 使用同一 call_id，并在 meta 保存
    skill_name、Provider/model、循环索引与执行身份；SessionLog 的直接字段归一化
    见 CHG-20260827-069。
  - response 继续记录所有 after Hook 完成后的最终值，避免原始 Provider 值与业务实际值混淆。
  - Recorder 的权威 call_id 索引保留兼容旧列表，同时补齐直接可查询的 Skill 归因。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/common/runtime_context.py
  - packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py
  - packages/agentflow-runtime/src/agentflow_runtime/recorder.py
  - packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py
  - docs/sessionlog/sessionlog_hook.py
  - packages/agentflow-runtime/tests/test_recorder_skill_name.py
  - packages/agentflow-runtime/tests/test_openai_compatible_client.py
- 验证结果：
  - 覆盖 Coordinator 与多个 Skill 的独立归因、Hook 改写响应和 Recorder 索引。
  - 后续 Runtime 全量回归 452 项通过、4 项跳过。
- 风险说明：
  - 仅补充上下文和观测字段，不改变模型请求、重试或业务控制流。

### CHG-20260827-059：SessionLog 统一稳定事件信封

- 日期：2026-08-27
- 目标：多进程并发 append 后，每条事实仍有全局可引用身份、稳定顺序和可幂等导入键。
- 诱发原因：
  - 旧事件只有进程内 seq/trace_id；不同 Python 进程会重复 seq，跨进程无法唯一引用。
  - 进程中断留下半行时，后续 append 可能继续破坏 JSONL。
- 改动内容：
  - 所有 Hook 事件统一增加 schema_version、event_id、idempotency_key、
    producer_id、producer_sequence、timestamp_us、run_id、task_id、node_id、
    primary_parent_event_id、causal_parent_event_ids、attributes 和 redaction。
  - producer_id 由进程 PID 与启动微秒组成；event_id 在 producer 内单调唯一。
  - 写入使用进程内锁与 POSIX flock；发现尾部不是换行时先修复记录边界。
  - 保留旧 seq、ts、source_event_seqs 字段供 Reader 和历史工具兼容。
- 涉及文件：
  - docs/sessionlog/sessionlog_hook.py
  - docs/sessionlog/tests/test_hook.py
  - docs/sessionlog/sessionlog_reader.py
- 验证结果：
  - 覆盖唯一事件 ID、因果父链、并发 append、半行修复和旧字段兼容。
  - SessionLog 全量 38 项通过。
- 风险说明：
  - 纯观测层变更；Hook 仍为 fail-open，不影响 Harness 返回。

### CHG-20260827-060：Skill 未处理异常生成唯一终态

- 日期：2026-08-27
- 目标：Skill 在任意 return 之外抛异常时也必须留下 skill.end，且不能与正常 finally
  路径形成重复终态。
- 诱发原因：
  - 原先只有正常完成、Tool 错误和 max_rounds_reached 等显式出口发完成事件。
  - manifest 解析后的意外异常可能只留下 skill.start 和未闭合 Span。
- 改动内容：
  - Skill Executor 外层 finally 保持 Span 关闭；异常出口补发 status=unhandled_exception
    的完成事件和结构化错误。
  - 使用终态标记确保每次执行至多一条 skill.end；已有业务状态原样保留。
  - 异常仍向框架传播/按原退出规则处理，采集 Hook 不改变工作流结果。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py
  - docs/sessionlog/sessionlog_hook.py
  - packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py
- 验证结果：
  - 覆盖 LLM 前异常、循环内异常、正常 return 和 max rounds；终态均恰好一条。
  - Runtime 与 SessionLog 全量回归通过。
- 风险说明：
  - skill_executor.execute 是 HIGH 风险入口；改动限于 finally 中的终态观测与既有
    Span 关闭，不吞异常、不改变分支隔离。

### CHG-20260827-061：Coordinator 未处理异常生成 Session 终态

- 日期：2026-08-27
- 目标：Coordinator 在正常 final 之外异常退出时，session.start 必须有可区分来源的
  session.end。
- 诱发原因：
  - 旧路径只在正常完成处发 agent.dialogue.end；异常时会留下孤立 Session。
- 改动内容：
  - Coordinator 外层捕获后发 agent.dialogue.error，包含 error_type、error、
    terminal_source 和 unhandled_exception 状态，然后保持原异常处理。
  - Hook 将 error/end 归一为 session.end；终态标记防止同一进程重复写结束事件。
  - 正常业务失败仍可作为响应完成，不被改写成 Workflow 失败。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
  - packages/agentflow-runtime/src/agentflow_runtime/events.py
  - docs/sessionlog/sessionlog_hook.py
  - 对应 Coordinator 与 Hook 测试
- 验证结果：
  - 覆盖首次激活异常、Skill 回流异常和正常 final；Session 起止配对通过。
- 风险说明：
  - Coordinator execute 是 HIGH 风险入口；新增事件是 fail-open 观测，没有修改
    C++ Workflow completed 的判断或业务状态。

### CHG-20260827-062：显式记录 Loop 决策并修正最终轮恢复分支

- 日期：2026-08-27
- 目标：轨迹能解释“为什么继续/恢复/终止”，同时无效 Tool call 在尚有恢复预算时
  必须真正进入下一轮。
- 诱发原因：
  - Coordinator 内部 Tool 纠错与 Skill 语义恢复只体现在 messages 变化和日志文本中。
  - recovery progress 推送成功后缺少无条件 continue；最终可恢复轮也可能直接落入
    exhausted return。
- 改动内容：
  - 新增 coordinator.tool_loop_retry 与 skill_executor.recovery_decision，
    Hook 归一为 loop.control，记录 scope、action、reason、当前次数与上限。
  - recovery_attempts 尚未耗尽时，追加恢复提示、调整 completion budget 后无条件
    continue；只有预算真正耗尽才返回 tool_call_error。
  - 最后一轮检测到可恢复错误时允许使用已获批的恢复轮，不提前宣称耗尽。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/events.py
  - packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
  - packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py
  - docs/sessionlog/sessionlog_hook.py
  - packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py
- 验证结果：
  - 覆盖无效 JSON、截断 Tool args、progress push 成功/失败、恢复成功与预算耗尽。
  - Runtime 全量回归 452 项通过、4 项跳过。
- 风险说明：
  - 恢复控制只修复原本声明但未执行的语义；未增加 transport retry，也未改变正常轮数。

### CHG-20260827-063：采集发送前的精确 LLM HTTP 请求

- 日期：2026-08-27
- 目标：有效 messages 快照之外，还要保存 Provider Client 最终组装并准备发送的请求体，
  以重放 sampling、tools、tool_choice 和兼容层变换。
- 诱发原因：
  - llm.request 位于 Harness 调用前，Provider Client 可能继续调整字段；仅靠它不能证明
    线上实际发送内容。
- 改动内容：
  - OpenAI Compatible Client 在 HTTP 调用前发 llm.request.prepared。
  - SessionLog 归一为 llm.transport.request，保存不含认证 Header 的精确 request body、
    call_id 和完整调用 meta；建树直接字段归一化见 CHG-20260827-069。
  - API key 与认证 Header 不进入事件；Hook 失败不阻断网络请求。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/events.py
  - packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py
  - docs/sessionlog/sessionlog_hook.py
  - packages/agentflow-runtime/tests/test_openai_compatible_client.py
  - docs/sessionlog/tests/test_hook.py
- 验证结果：
  - 覆盖 Coordinator/Skill、tools/tool_choice、sampling 参数、精确请求体和敏感字段排除。
  - Runtime、SessionLog 全量回归通过。
- 风险说明：
  - Client 是共享 HIGH 风险边界；事件在发送前 fail-open，不修改 body、Header、重试和超时。

### CHG-20260827-064：记录运行版本与 Prompt/Skill/Tool 内容指纹

- 日期：2026-08-27
- 目标：同一输入的轨迹必须能判定使用了哪一版 Coordinator、Agent 定义、系统 Prompt、
  Skill 内容和 Tool schema。
- 诱发原因：
  - 只有 model 与文本快照时，代码/配置变化可能被误判成随机模型差异。
- 改动内容：
  - session.start/coordinator.activation 保存 coordinator_version、code_revision、
    agent_definition_sha256 和关键运行上限。
  - Skill start 保存 system_prompt_sha256、Skill 内容摘要、Tool schema 摘要及实际加载列表；
    LLM request/transport request 保存当轮实际消息与请求体。
  - code_revision 读取 AF_CODE_REVISION/GIT_COMMIT；未注入时保持空值，不伪造版本。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
  - packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py
  - packages/agentflow-runtime/src/agentflow_runtime/trace/emit.py
  - docs/sessionlog/sessionlog_hook.py
  - 对应 trace、Coordinator、Skill 与 Hook 测试
- 验证结果：
  - 指纹稳定性、内容变更敏感性和未配置 revision 的显式空值均有覆盖。
- 风险说明：
  - 纯观测字段，不读取 Git 工作树、不在运行时执行外部命令。

### CHG-20260827-065：逐 Artifact 记录内容身份与存储终态

- 日期：2026-08-27
- 目标：轨迹中的 Artifact 不仅要有路径，还要能验证内容并知道是否真正进入 Bridge 存储。
- 诱发原因：
  - Skill end 只有 Artifact 列表；上传成功、跳过、下载失败或存储失败无法区分。
  - 后续回放无法确认同一路径是否仍是当时的字节内容。
- 改动内容：
  - 新增 artifact.storage_result，Hook 归一为 artifact.persist。
  - 每个 Artifact 保存 status、源/目标路径、artifact_id、storage_id/url、mime、
    size_bytes、sha256 和结构化 error；不把二进制内容嵌入 JSONL。
  - 本地收集、E2B 下载、Bridge 上传成功及各失败出口都产生明确结果。
  - 旧 Bridge Artifact 测试改从真实所有者 coordinator_common 导入 URL helper，
    不恢复已经移除的重复导出。
- 涉及文件：
  - packages/agentflow-runtime/src/agentflow_runtime/events.py
  - packages/agentflow-runtime/src/agentflow_runtime/artifact_pipeline.py
  - docs/sessionlog/sessionlog_hook.py
  - frontend/bridge/tests/test_artifact_storage_e2e.py
  - frontend/bridge/tests/test_e2e_comprehensive.py
  - 对应 Runtime/SessionLog Artifact 测试
- 验证结果：
  - Artifact/SessionLog/Bridge 扩展组合 106 项通过。
  - Runtime 全量 452 项通过、4 项跳过；SessionLog 38 项通过。
- 风险说明：
  - Artifact 字节仍由现有文件/Blob 存储负责；该事件只记录内容身份和存储事实。

### CHG-20260827-066：区分 Coordinator 候选结果与 Bridge 最终交付

- 日期：2026-08-27
- 目标：session.end 不能被误解为客户端已收到；轨迹要保存 Bridge 实际公开内容和线协议，
  并与 Coordinator 候选终态建立因果边。
- 诱发原因：
  - Bridge 会把内部 Artifact URL 转成 placeholder/public URL，旧日志只有变换前候选文本。
  - HTTP/SSE 返回边界没有事件，也没有客户端 ACK 能力，却容易被下游误标成已送达。
- 改动内容：
  - 新增 fail-open 的 SessionLog Bridge writer；仅在对应 SessionLog 已存在时追加
    client.delivery，不会单独启用采集。
  - 保存 raw_content、delivered_content、精确 HTTP JSON/SSE frame、
    各自 SHA256、Artifact 列表、transport/stage/status。
  - 读取最新 session.end，把 candidate_event_id 写为 primary/causal parent，并记录
    candidate_matches_raw_content。
  - 明确 delivery_claim=server_side_observation、client_acknowledged=false，
    不虚构网络送达或客户端确认。
  - 同步 REST 路径改为：历史保存带真实 Artifact URL 的 raw 候选，HTTP 返回经过公开化的
    delivered 内容；SSE 对实际 yield 的同一序列化 frame 进行观测。
  - 所有构建、查找、哈希和 append 异常均在公共包装层吞掉并返回 False，不影响聊天响应。
- 涉及文件：
  - frontend/bridge/services/sessionlog_delivery.py
  - frontend/bridge/routes/chat_rest.py
  - frontend/bridge/tests/test_sessionlog_delivery.py
  - frontend/bridge/tests/test_chat_rest_sse.py
  - docs/sessionlog/README.md
- 验证结果：
  - 覆盖 SessionLog 未启用、HTTP raw/public 双视图、SSE final/DONE、异常 fail-open、
    候选父链与哈希匹配；Bridge 定向 35 项通过。
  - Artifact/SessionLog/Bridge 扩展组合 106 项通过。
- 风险说明：
  - chat_rest 是公开 HIGH 风险路由；业务变化仅修正同步响应应返回 public view，
    观测写入全部 fail-open。没有修改 daemon、C++ 协议或 Workflow 状态。

### CHG-20260827-067：补齐 daemon 构建所需的直接标准库依赖

- 日期：2026-08-27
- 目标：当前开发环境必须能从干净依赖关系构建实际运行的 agentflowd。
- 诱发原因：
  - json_adapter.h 使用固定宽度整数类型却依赖其他头文件间接引入 cstdint。
  - runtime.cpp 使用 std::sort 等算法却依赖其他编译单元的偶然 include 顺序。
- 改动内容：
  - json_adapter.h 显式包含 cstdint。
  - runtime.cpp 显式包含 algorithm。
- 涉及文件：
  - include/internal/workflow/dsl/json_adapter.h
  - src/runtime/runtime.cpp
- 验证结果：
  - build/dev 的 agentflowd target 编译并链接成功。
- 风险说明：
  - 仅补充直接编译依赖，不改变 C++ 类型、函数或运行语义。

### CHG-20260827-068：开发环境启用结构化并行 Skill 解析

- 日期：2026-08-27
- 目标：Python 生成的结构化 next_skills 数组必须被 C++ Dispatcher 按对象数组解析，
  不能退化为逗号分隔文本。
- 诱发原因：
  - 未检测到 RapidJSON 时，底座只启用 CSV fallback；真实三 Skill Batch 被错误拆成
    12 个字符串片段。
  - Conda 已安装 RapidJSON，但 CMake 的 NO_DEFAULT_PATH 搜索没有包含 CONDA_PREFIX/include。
- 改动内容：
  - CMake RapidJSON 搜索路径增加 CONDA_PREFIX/include。
  - 重新配置现有 build/dev 缓存后，AGENTFLOW_WITH_RAPIDJSON 生效。
- 涉及文件：
  - CMakeLists.txt
- 验证结果：
  - 配置摘要显示 RapidJSON: ON；agentflowd target 构建成功。
  - 真实 hchat 中一个 Batch 被准确解析为三个结构化 Skill 分支。
- 风险说明：
  - 未改 Dispatcher 逻辑，只让已有结构化 JSON 路径在 Conda 开发环境正确启用。

### CHG-20260827-069：把 LLM 执行身份提升为可直接建树字段

- 日期：2026-08-27
- 目标：所有 LLM 阶段都能直接按业务 Skill 和并行分支查询，禁止从 task_id 尾部猜测。
- 诱发原因：
  - 真实多进程 hchat 证明 Recorder meta 已有正确 Skill 身份，但 SessionLog 只保存嵌套
    meta；三个 LLM 事件 data 顶层的 skill_name 均为空或不存在。
- 改动内容：
  - llm.request、llm.transport.request、llm.response 直接保存 skill_name、
    exec_skill_key、batch_id 和 task_type。
  - Coordinator 的统一归因名为 coordinator；Skill 使用 dispatch manifest 中的业务名。
  - 正常调用不伪造尚未分配的 call_id：llm.request 通过因果边连接
    llm.transport.request，后者与 llm.response 共享同一非空 call_id。
  - 更新 schema 文档，并增加真实 Skill 三阶段身份测试。
- 涉及文件：
  - docs/sessionlog/sessionlog_hook.py
  - docs/sessionlog/README.md
  - docs/sessionlog/tests/test_hook.py
- 验证结果：
  - Hook 聚焦回归 17 项通过。
- 风险说明：
  - handle 覆盖全部轨迹事件，观测影响范围高；本条只增加 LLM data 映射，
    不修改 Recorder、请求体、LLM 调用或业务控制流。

### CHG-20260827-070：Bridge 无共享环境时仍定位 daemon SessionLog

- 日期：2026-08-27
- 目标：最终 HTTP/SSE 响应必须能追加 client.delivery，不能因 Bridge 与 daemon
  是独立进程而静默丢失交付事实。
- 诱发原因：
  - daemon 子进程持有 AF_TRACE_OUTPUT_DIR，长期运行的 Bridge 未继承该变量。
  - Bridge 原 fallback 只查 resource/agent-packages 下的 trace_output，而真实 SessionLog
    位于仓库根 trace_output/sessionlogs。
- 改动内容：
  - 显式 AF_SESSIONLOG_DIR/AF_TRACE_OUTPUT_DIR 仍保持最高优先级。
  - 无显式配置时，依次查找 agent 默认布局和仓库级 daemon 布局，只向已存在文件追加。
  - 保持构建、查找和写入全链路 fail-open，不影响聊天响应。
- 涉及文件：
  - frontend/bridge/services/sessionlog_delivery.py
  - frontend/bridge/tests/test_sessionlog_delivery.py
  - docs/sessionlog/README.md
- 验证结果：
  - Bridge writer 聚焦回归 4 项通过，覆盖无共享环境的仓库级路径。
- 风险说明：
  - 只修改最终交付观测的文件定位；不创建未启用的 SessionLog，不改变 HTTP/SSE 载荷。

### CHG-20260827-071：挂载 Hook 并完成复杂真实轨迹验收

- 日期：2026-08-27
- 目标：用真实 hchat 证明当前采集链可覆盖多并发、本地 Sandbox、网络请求、
  预期失败、长 Tool Loop、Artifact 与最终 Bridge 交付。
- 测试场景：
  - 同一次 run_skills 并发 outfit-stylist、forge-image-gen、video-gen 三个独立分支。
  - outfit 分支先触发缺失文件错误，再通过 weather.py 联网查询上海天气并生成 HTML。
  - image 分支真实执行 generate.py 一次，因 Sandbox 无 Provider key 失败。
  - video 分支以不存在的首帧执行 make_video.py 一次并失败；两条失败分支不取消成功分支。
- 挂载与涉及文件：
  - canonical docs/sessionlog/sessionlog_hook.py 已精确同步到
    examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py。
  - examples/agent/outfit-assistant/.env 保持 AF_SESSIONLOG_ENABLED=1。
  - HARNESS_INPUT.log、HARNESS_OUTPUT.log、HARNESS_HISTORY.log 均由 *.log 忽略，不进入提交。
- 真实结果：
  - dialogue_id=0827165525-a8a67c51e091，SessionLog 共 94 行（含 header），Reader 校验通过。
  - 15 组 llm.request/transport/response 数量一致；三阶段 skill_name 全部非空，
    分布为 coordinator=2、outfit-stylist=3、forge-image-gen=7、video-gen=3。
  - 15 个 transport/response call_id 完全配对；全部 intent→transport→response 因果边闭合。
  - 三个 Skill 的 exec_skill_key 起止集合完全一致且终态唯一，并发重叠 12,188,303 微秒。
  - 13 个 Tool Call 与 13 个 Tool Result 按 tool_call_id 完全一一配对。
  - HTML 产物 8,499 字节，SHA256=
    ecda6adef1b5887a64be49f9e7728c4101c304d4f9350800bc41d3a62e9dd54a，
    与 artifact.persist.entries 和磁盘字节一致；PNG/MP4 均未伪造。
  - 唯一 client.delivery 指向唯一 session.end，并明确记录 server_side_observation、
    client_acknowledged=false；候选相对 URL、Bridge 绝对 URL 和最终 placeholder 的
    三层变换均有独立内容与哈希。
  - Python Span 共 29 条（15 LLM、10 Tool、4 Skill/dispatch），全部有 end_us、
    duration_us、唯一 span_id 和终态。
- 回归结果：
  - Runtime Python：438 passed、4 skipped（真实 Sandbox 用例）。
  - SessionLog：39 passed。
  - Bridge/Artifact/交付组合：77 passed。
  - agentflowd target 构建成功；git diff --check 通过。
- 仍应作为优化样本保留的事实：
  - forge-image-gen 只调用 generate.py 一次，但在失败后使用 7 个 LLM 轮次做环境诊断；
    这是可减少的无效 Loop，不是轨迹丢失或分支重试。
- 风险说明：
  - sessionlog_reader.py 仅用于演示与校验，不是轨迹树实现；本条没有新增业务控制改动。

### CHG-20260827-072：绘制真实会话的运行轨迹树

- 日期：2026-08-27
- 目标：把复杂 hchat 验收轨迹转换为可直接查看、可用于讨论建树语义的静态可视化。
- 改动内容：
  - 新增 dialogue_id=0827165525-a8a67c51e091 的 SVG 运行轨迹树和配套解读。
  - 展示两次 Coordinator 激活、单 Batch 三分支并发、15 次 LLM 调用、13 个 Tool、
    Artifact、Batch 汇合、session.end 和 Bridge client.delivery。
  - 将每次 LLM 的 request→transport.request→response 三阶段折叠为一个可读节点，
    同时保留 Round、Tool 结果、时间与业务终态。
  - 明确区分框架 completed 与 Tool/业务失败，并说明并发轨迹本质是带 fork/join 的 DAG，
    不能在后续 Protobuf 建树时强行丢成普通单父树。
- 涉及文件：
  - docs/harness-v1-code-reading/trajectory-trees/0827165525-a8a67c51e091/trajectory-tree.svg
  - docs/harness-v1-code-reading/trajectory-trees/0827165525-a8a67c51e091/README.md
  - HARNESS_CHANGES.md
- 验证结果：
  - SVG 通过 XML 解析；图中数量、分支起止、Tool 状态和产物哈希均与真实 SessionLog 对照。
- 风险说明：
  - 仅增加静态文档和图，不修改 Harness、SessionLog 或业务控制流。

### CHG-20260828-073：补齐 SessionLog Header 的逻辑会话与版本身份

- 日期：2026-08-28
- 目标：不修改 C++ 底座，使每轮 SessionLog 能稳定关联到 Agent、逻辑多轮会话和
  实际代码版本，同时保留现有物理 Session 槽位字段兼容性。
- 诱发原因：
  - Header 原先仅从 `AF_AGENT_PACKAGE_DIR` 取 Agent 名称；Coordinator 写入 Header
    时该环境变量未注入，因此真实轨迹固定得到 `agent=unknown`。
  - `session_id=0/1/2` 是 C++ Session Pool 的物理执行槽位，可能复用，不能作为
    跨轮或跨用户轨迹树的逻辑会话主键。
  - Coordinator 已支持读取 `AF_CODE_REVISION/GIT_COMMIT`，但开发启动脚本没有在
    缺省情况下注入当前提交，导致 Header 和 session.start 的版本字段为空。
- 改动内容：
  - Header 保留兼容字段 `session_id`，并新增同值的 `session_slot_id`，明确其物理
    槽位语义。
  - 从 `agent + user_id + client_id` 的稳定 SHA-256 摘要派生 `conversation_id`；新增
    `user_id`、`client_id`，供多轮轨迹按逻辑会话聚合。
  - Agent 身份优先读取事件载荷和 `AF_AGENT_PACKAGE_DIR`；均缺失时从标准挂载路径
    `<agent-package>/harness/hooks/sessionlog_hook.py` 恢复，不依赖 C++ 新增注入。
  - Header 同步保存有效模型、`code_revision` 和 `agent_definition_sha256`；写入时直接
    使用首个 `agent.dialogue.start/coordinator.trigger` 的权威载荷。
  - `start_daemon.sh` 仅在 `AF_CODE_REVISION` 与 `GIT_COMMIT` 均未显式设置时解析当前
    Git HEAD，并由 daemon 环境传给 Python 子进程。
  - canonical 与 outfit-assistant 已挂载 Hook 保持逐字一致；README 补充 Header
    字段及多轮聚合语义。
- 涉及文件：
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/tests/test_hook.py`
  - `docs/sessionlog/README.md`
  - `scripts/start_daemon.sh`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - SessionLog 全量 40 项测试通过；新增覆盖完整 Header 和未注入包路径时的挂载位置
    回退。
  - 两份 Hook 逐字一致，Python 编译、Shell 语法和 `git diff --check` 均通过。
  - 真实两轮 hchat 生成 dialogue `0828030150-67c1a995158e` 和
    `0828030231-88b882c104a0`：两轮 `dialogue_id` 不同，`conversation_id` 均为
    `conv_8e83af9023740dedf1d99654`，第二轮包含完整三条历史消息。
  - 两轮 Header 均记录 `agent=outfit-assistant`、`session_slot_id=0`、模型、完整
    Git revision `4e56714cec3df9ce56bbf41a69e19ea8c6449678` 和非空 Agent 定义摘要。
- 风险说明：
  - 只扩展 Header 观测字段并补启动环境，不改变 Coordinator、Skill、Tool、并行
    调度或 C++ SessionRouter 行为；旧 Reader 会忽略新增字段。
  - `code_revision` 表示 Git HEAD；工作树未提交差异仍需结合各类内容摘要识别。

### CHG-20260828-074：增加采集侧 User/Session/Dialogue 目录投影

- 日期：2026-08-28
- 目标：在不改变原始 SessionLog 路径和任何业务执行逻辑的前提下，让采集结果按
  User → 逻辑 Session → Dialogue 的物理目录层级可直接浏览。
- 改动内容：
  - 原始 `trace_output/sessionlogs/{dialogue_id}.jsonl` 保持不变，继续作为唯一写入源。
  - Header 成功落盘后，在
    `sessionlogs/by-user/{safe-user-id}/sessions/{conversation_id}/{dialogue_id}.jsonl`
    建立指向原始文件的相对符号链接；同一 User/Client 的多轮 dialogue 自动归入
    同一逻辑 Session 目录，不复制事件数据。
  - 用户目录分量保留安全的中英文、数字和 `-_.`；其他字符替换后附加原值摘要，
    防止 `..`、斜杠等输入造成路径穿越或名称碰撞。
  - 投影缺少身份或创建失败时 fail-open，只记录告警，原始 Header 和后续事件继续
    正常写入。
  - Bridge 仍向原始文件追加 `client.delivery`，符号链接实时呈现同一内容；未修改
    Bridge、Reader、Coordinator、Executor、DataBus 或 C++。
- 涉及文件：
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/tests/test_hook.py`
  - `docs/sessionlog/tests/README.md`
  - `docs/sessionlog/README.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - SessionLog/Bridge 聚焦回归 46 项通过；两份 Hook 逐字一致，Python 编译与
    `git diff --check` 通过。
  - 真实 hchat 生成 2 个用户、3 个逻辑 Session、4 个 dialogue：用户 A 的会话 A
    含 2 轮，会话 B 含 1 轮，用户 B 的会话 A 含 1 轮。
  - 4 个投影均为符号链接且与原始文件 inode 一致；Header 的 user/conversation/
    dialogue 与目录逐级匹配，每条包含唯一 `client.delivery`，Reader 校验问题数为 0。
- 风险说明：
  - 层级目录是兼容投影视图，不是第二份数据；删除原始 JSONL 会使对应链接失效。
  - 已完成且不再触发 Hook 的历史平铺日志不会自动回填，后续新采集会自动建投影。

### CHG-20260828-075：采集模型移除 Session，仅保留 User/Conversation/Dialogue 唯一文件

- 日期：2026-08-28
- 目标：更正 CHG-073/074 中把可复用运行时槽位暴露为轨迹 Session 的设计；采集侧只保留
  User → Conversation → Dialogue，并且每个 dialogue 只存一份实体 JSONL。
- 诱发原因：
  - `AF_SESSION_ID` 是 DataBus/C++ Session Pool 的运行时槽位，不是跨轮业务身份，写入
    轨迹会给建树制造错误主键。
  - CHG-074 同时保留根目录平铺文件和 `sessions/` 下的符号链接，既多一层无意义命名，
    又让同一 dialogue 在目录上看起来有两个入口。
  - 删除顶层字段后，真实验收又发现 LLM Recorder 的 `meta.session_id` 会被 Hook 原样复制，
    因而需要在观测边界过滤，而不能只改 Header。
- 改动内容：
  - 新轨迹直接写入
    `trace_output/sessionlogs/by-user/{safe-user-id}/{conversation_id}/{dialogue_id}.jsonl`；
    不再写根目录副本，不再创建符号链接，也不增加 `conversations/` 中间目录。
  - Header 与生命周期事件统一为 `dialogue.header/dialogue.start/dialogue.end`；Header、事件
    信封和 `client.delivery` 均不再包含 `session_id/session_slot_id`。
  - 新 Header、Hook 事件和 Bridge 交付事件统一标记 `schema_version=2`；Reader 继续兼容
    schema v1 历史数据。
  - LLM 的实际 request/response 保持完整，只递归过滤 Recorder meta 中纯运行时的
    `session_id/session_slot_id`；Artifact 的 User+Client 路由身份在轨迹中改名为
    `artifact_routing_id`。
  - Bridge 交付观测接口删除未使用的 `session_id` 参数，并在新层级文件上追加
    `client.delivery`；Reader 与 Bridge 均优先发现新布局，同时兼容历史根目录文件、
    CHG-074 的 `sessions/` 投影以及历史 `session.*` 事件。
  - `AF_SESSION_ID` 仅保留在 Hook 创建 DataBusClient 的底座内部调用中，不序列化、不参与
    Conversation 聚合；未修改 C++、Coordinator、Skill Executor 或调度控制流。
- 涉及文件：
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/sessionlog_reader.py`
  - `frontend/bridge/services/sessionlog_delivery.py`
  - `frontend/bridge/routes/chat_rest.py`
  - `docs/sessionlog/tests/conftest.py`
  - `docs/sessionlog/tests/test_hook.py`
  - `docs/sessionlog/tests/test_reader.py`
  - `frontend/bridge/tests/test_sessionlog_delivery.py`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/tests/README.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - SessionLog/Bridge 聚焦回归 49 项通过；两份 Hook 逐字一致。
  - 真实 hchat 生成 2 个用户、3 个 Conversation、4 个 Dialogue：
    `trajectory-layout-user-a/conv_777d383b84d9f64de0f7ed90` 下有两轮，另两个 Conversation
    各一轮。
  - 四条轨迹均为普通实体文件且 Header 的 User/Conversation/Dialogue 与目录一致；对应
    根目录副本数为 0，全部 JSON 中含 `session` 的键数为 0，`session.*` 事件数为 0。
  - 四个 Header 均为 version=2，所有后续 Hook/Bridge 事件均为 schema_version=2；Reader
    仅凭 dialogue_id 即可发现层级文件，读取事件数与直接路径一致。
  - 每条均恰有一个 `dialogue.start`、一个 `dialogue.end` 和一个最终
    `client.delivery`；交付父事件均指向对应 dialogue.end，Reader 校验问题数均为 0。
  - 另用真实 `outfit-stylist` + 天气 Tool 验证多进程追加：同一 dialogue 由 4 个 producer
    PID 产生 1 对 Skill、2 对 Tool，按文件名全目录搜索仍只有一个实体文件，无根目录副本、
    无 Session 命名键，交付父边正确且 Reader 问题数为 0。
- 风险说明：
  - 新写入路径发生了有意的 schema/layout 变更；仓库内 Bridge 和演示 Reader 已兼容新旧
    布局，外部若有硬编码 `sessionlogs/{dialogue_id}.jsonl` 的脚本需改为按层级发现文件。
  - 历史轨迹不会被重写，仍可包含旧字段与旧事件名；兼容读取只用于历史数据，新采集不会
    再生成这些字段。

### CHG-20260828-076：使用前端 client_id 作为轨迹 Conversation 身份

- 日期：2026-08-28
- 目标：按前端与 Bridge 的真实字段语义组织轨迹，避免对已经代表逻辑会话的
  `client_id` 再派生一个无法直接回查的摘要身份。
- 改动内容：
  - 轨迹层的 `conversation_id` 直接等于原始 `client_id`，不再使用
    `agent + user_id + client_id` 计算 SHA-256 摘要。
  - 新轨迹路径的逻辑结构明确为
    `by-user/{user_id}/{client_id}/{dialogue_id}.jsonl`；目录分量仍经过安全处理，
    Header 保留原始 `user_id/client_id`。
  - Reader 与 Bridge 交付定位规则本就按 `by-user/*/*/{dialogue}.jsonl` 发现，
    无需改动运行时接口，并继续兼容旧摘要目录。
  - 同步修正 Hook、Reader 与 Bridge 测试中的目录语义和部署说明。
- 涉及文件：
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/tests/test_hook.py`
  - `docs/sessionlog/tests/test_reader.py`
  - `frontend/bridge/tests/test_sessionlog_delivery.py`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/tests/README.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - SessionLog 与 Bridge delivery 聚焦回归 49 项全部通过。
  - 两份 Hook 逐字一致，Python 编译与 `git diff --check` 通过。
  - 单元测试证明 Header 中 `conversation_id == client_id == "client-a"`，实体文件
    直接位于 `by-user/user-a/client-a/d1.jsonl`。
  - 使用前端同款请求体实测两轮：dialogue
    `0828050725-3c5e2481463d` 与 `0828050728-1f3c818a3091` 均写入
    `by-user/frontend-trajectory-user-20260828/frontend-trajectory-conversation-20260828/`；
    两个 Header 的 `conversation_id/client_id` 完全相同，Reader 均校验通过。
  - 第二轮 `llm.request` 回放出 4 条 messages，确认 Bridge 确实按同一
    `user_id + client_id` 恢复上下文，不是仅在文件系统上恰好同目录。
- 风险说明：
  - 仅改变新轨迹的身份映射与目录名，不改 Coordinator、Skill Executor、Bridge
    请求或 C++ 底座。
  - 已有 `conv_<hash>` 历史目录不会搬迁；Reader 与 Bridge 仍可按 `dialogue_id` 读取。

### CHG-20260828-077：将 Outfit Assistant 主模型切换为 glm-5.3

- 日期：2026-08-28
- 目标：让 Outfit Assistant 的 Coordinator 与 Skill Executor 实际使用火山 Agent Plan
  中配置的 `glm-5.3`，而不是继续被 Agent 定义中的旧模型覆盖。
- 诱发原因：
  - `.env` 用户配置段已经声明 `AF_LLM_MODEL=glm-5.3`，但 `agent.md` 仍指定
    `deepseek-v4-flash`。
  - Runtime 的模型优先级为 `agent.md` 高于节点环境变量和 `AF_LLM_MODEL`，因此真实实验的
    181 次 LLM 调用全部使用了 `deepseek-v4-flash`。
- 改动内容：
  - 将 `examples/agent/outfit-assistant/agent.md` 的主模型改为 `glm-5.3`。
  - 保留 DashScope 的 `AF_FALLBACK_LLM_MODEL=deepseek-v4-flash`；它只在主模型配置缺失时
    兜底，不参与当前正常调用。
  - 不修改 LLM Base URL、API Key、视觉模型或其他 Agent。
- 涉及文件：
  - `examples/agent/outfit-assistant/agent.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - Runtime 配置解析确认 Coordinator 与 Skill Executor 均解析为 `glm-5.3`。
  - `git diff --check` 通过。
- 遗留事项：
  - 密钥轮换与工具结果脱敏仍需在继续真实 hchat 前单独处理；本条只调整模型，不触碰安全
    修复或历史轨迹。


### CHG-20260828-078：阻断 Harness 后续凭据泄露并建立提交门禁

- 日期：2026-08-28
- 目标：在不绑定 OpenAI 或任一供应商格式的前提下，阻止新配置的 API Key、Token、
  Password、Client Secret、Access Key、Cookie、私钥等凭据进入 LLM、非受信任工具、
  子进程参数、Sandbox、轨迹、日志、响应交付或 Git；遵循 Python/Harness 优先和最小
  C++ 改动原则。
- 诱发原因：
  - 旧实现依赖少数固定变量名和供应商前缀，无法覆盖火山、DashScope、E2B、云厂商及
    非标准命名的凭据。
  - Agent 本地 `.env` 曾被跟踪；Python 子进程、MCP、E2B、媒体脚本和 C++ CodeLoader
    存在把环境值带入非受信任边界、shell argv 或日志的路径。
  - Recorder、DataBus、Hook、Span、SessionLog、Bridge 和 stderr 是多条独立落盘链，
    只修一个出口仍会留下旁路。
  - Host Bash、Host 文件工具、通用 URL 抓取和旧媒体脚本可扩大读取本机凭据、路径逃逸、
    SSRF、重定向或未校验 TLS 的风险。
- 改动内容：
  - 新增 `agentflow_runtime.security`：可信加载方按来源注册精确凭据值；统一覆盖 URL 编码、
    Base64、JSON 转义和 Authorization 表示；递归识别 snake_case/camelCase 凭据字段；
    流式分块脱敏；安全报告仅保存标签、路径和计数，不保存原值或原值哈希。
  - LLM HTTP 请求在发送前检测并阻断凭据，响应、流式 chunk、tool arguments 和 Provider
    错误统一脱敏；强制 TLS 校验、禁用重定向和环境代理。
  - Recorder、DataBus、Hook Engine、Tool Registry、MCP、Python SpanWriter、span stderr、
    SessionLog 调试输出、Bridge trace/sessionlog/client delivery 和 Bridge 全部 logging handler
    均在最终出口脱敏；sanitizer 不可用的持久化与日志路径 fail-closed。
  - 三个 Harness 主循环只包装 stderr 日志边界，不修改 stdout JSON 协议；tool 实际执行仍
    使用原始非敏感参数，日志只使用脱敏副本。
  - 非受信任子进程改用严格 allowlist 环境；E2B 不上传、不 source `.env`，通用命令不接收
    Host 凭据；MCP stdio 仅接收其显式可信配置。
  - Host Bash 默认关闭，只有显式将 `AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS` 设为 `1` 才能走旧宿主
    兼容路径；本地隔离 Sandbox 继续由 restricted-local 策略执行。
  - Host read/write/edit 使用 `realpath + commonpath`，拒绝姊妹目录前缀和符号链接逃逸；
    `.env`、云凭据、私钥及 Harness 可执行配置禁止被通用文件工具访问或改写。
  - `web_fetch` 仅允许 http/https，拒绝 URL userinfo、DNS 失败以及任何非全局 IP，禁用环境
    proxy 和 redirect；E2B 的重复 Host fetch 实现收敛到同一安全实现。
  - 图片、视频、视觉分析和天气改为受信任结构化工具：凭据只由工具内部注入，请求 URL、
    本地媒体路径、下载和发布均经过校验；移除生产 Python 中所有未校验 TLS context，私有
    CA 统一通过标准 CA 环境配置。
  - Bridge 创建或更新 Agent `.env` 时强制权限 0600，且不再把 Host 凭据写入 Workspace；
    本地真实 `.env` 保留在工作区、权限改为 0600、从 Git 索引删除并由 `**/.env` 忽略。
  - 开发/生产配置中的硬编码值改为环境引用或空模板；模板增加
    `AF_SECRET_ENV_NAMES`，用于声明不符合常规命名的供应商凭据。
  - 新增 provider-neutral 源码扫描器、单元测试、push/PR CI 和 pre-push 检查；扫描器覆盖
    常规与 camelCase 字段、Authorization、URL userinfo、私钥块、常见高置信前缀，并自动
    读取仓库内声明的自定义凭据变量名。发现报告只打印文件、行号、规则和字段名。
  - C++ 仅保留一处必要改动：CodeLoader 在 fork 后对子进程调用 `setenv`，环境值不再拼入
    shell command、argv 或命令日志；未改 Coordinator、Workflow、Sandbox 调度和业务状态。
- 主要涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/security/`
  - `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/{recorder,databus_client,tool_registry,skill_executor}.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/{hooks,trace,harness/mcp,executors}/`
  - `frontend/bridge/services/secret_safety.py`、Bridge 轨迹出口和 Agent env 注入逻辑
  - `examples/agent/outfit-assistant/harness/` 及对应 Agent Package/Skill Hub 镜像
  - `scripts/quality/scan_secrets.py`、`scripts/prepush_check.sh`、
    `.github/workflows/credential-safety.yml`
  - `src/runtime/worker/exe_loader.cpp` 及专项测试
- 验证结果：
  - Runtime 全量非真实外部依赖回归：497 passed、4 skipped。
  - SessionLog、Bridge 最终出口、Sandbox 上传路径和 scanner 联合回归：107 passed。
  - Sandbox 环境隔离 mock 档：8 passed、4 个真实 E2B 用例未执行。
  - Coordinator/Skill/stderr 相邻回归：97 passed；TLS preload/hook 回归：61 passed。
  - Scanner 单测：12 passed；工作区 tracked 1713 个、staged 0 个、untracked 57 个文件
    均通过凭据扫描。
  - CodeLoader 专项 C++ 测试 2 项通过；生产 Python 危险 TLS/Authorization argv 静态模式
    扫描为 0；`git diff --check` 通过。
  - GitNexus 当前工作树分析为 106 文件、497 符号、35 条流程，风险 CRITICAL；相对 main
    为 1377 文件、8624 符号、176 条流程且明细受 CLI 上限截断。因此未把图分析视为
    clean check，也没有自动提交。
- 边界与使用约束：
  - 该防护完整覆盖当前 Harness 的 Python LLM、Tool、MCP、Sandbox、轨迹和 Bridge 路径；
    历史已生成日志不会自动重写。
  - 非标准变量名必须加入 `AF_SECRET_ENV_NAMES`；显式 unsafe Host opt-in 会有意放开宿主
    命令执行，只用于可信本地兼容测试。
  - 按最小 C++ 原则，没有扩改底座中不属于当前 Python Harness 调用路径的独立旧网络
    客户端；未来若切换到那些 transport，需单独做同等级审计。
  - 本轮未创建真实 E2B 资源，也未调用付费 Provider。

### CHG-20260828-079：修复 SessionLog Hook 挂载副本漂移

- 日期：2026-08-28
- 目标：让实际运行的 Harness 恢复使用 User/Conversation/Dialogue 目录结构，并防止重新同步运行包后回退到旧版平铺日志。
- 诱发原因：
  - 新版 Hook 只更新在 docs/sessionlog，Harness 实际动态加载的是 Agent 包内 harness/hooks。
  - examples 运行副本仍是 version 1，resource 固化包源中又缺少 SessionLog Hook；包同步无法保证新版持续存在。
- 改动内容：
  - 将已测试的 docs SessionLog Hook 逐字同步到 resource 固化包源和 examples 运行副本。
  - 新增部署一致性测试，要求 docs、resource 和 examples 三个受控副本逐字一致。
  - 不修改 C++、Harness 业务控制流或历史轨迹；旧平铺文件不会自动迁移。
- 涉及文件：
  - HARNESS_CHANGES.md
  - docs/sessionlog/tests/test_hook.py
  - examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py
  - resource/agent-packages/outfit-assistant/harness/hooks/sessionlog_hook.py
- 验证结果：
  - docs 源、resource 包源和 examples 运行文件内容一致。
  - SessionLog 全量 47 项测试通过；三个受控 Hook 均通过 Python 编译检查。
  - 新日志路径为 trace_output/sessionlogs/by-user/{user_id}/{client_id}/{dialogue_id}.jsonl，首行为 dialogue.header version 2。

### CHG-20260828-080：修复 Local Sandbox 多实例端口串线

- 日期：2026-08-28
- 目标：保证同一 daemon 内每个 LocalSandbox 拥有自己的 HTTP server endpoint，避免多用户、多会话实验发生跨 Sandbox 串线和级联 `Connection refused`。
- 诱发原因：
  - daemon 把同一个 `sandbox_http_port` 传给所有 LocalSandbox；后启动子进程 bind 失败后，父进程仍先探测该固定端口。
  - 只要旧 Sandbox server 在该端口返回非空响应，`wait_for_server_ready()` 就会把兄弟进程的响应误认为当前子进程已就绪。
  - 原有 `port=0` 路径在 fork 后由父进程另行 bind(0) 猜端口，和已经以 0 启动的 Python server 实际端口没有关联。
- 改动内容：
  - 只修改 LocalSandbox：在 fork 前尝试占用配置端口；若已被占用，则让内核选择空闲 loopback 端口，并把解析出的实际端口传给 Python 子进程。
  - 用进程内 mutex 仅串行化“选端口 → fork → 当前子进程就绪”的短窗口，保证并发创建时前一实例完成 bind 后后一实例才选端口。
  - 将就绪等待内聚到 `launch_http_server()`，构造和 `resume()` 不再重复探测；保留已有 `restricted-local` 安全参数。
  - 未修改配置格式、Docker/E2B、Coordinator、Skill Executor、Bridge 或 SessionLog。
  - 新增真实双实例回归：第一个 Sandbox 使用动态端口，第二个故意收到第一个已占用的端口，断言两者 endpoint 和 workdir 均不同。
- 涉及文件：
  - `src/sandbox/sandbox_local.cpp`
  - `tests/integration/sandbox_lifecycle_test.cpp`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - `test_daemon_gtest` 编译成功；只有项目原有 warning。
  - `SandboxLifecycleTest.LocalHttpEndpointsAreIsolated` 通过；两个真实 server 分别监听 `41239` 和 `43241`，没有复用 endpoint。
  - `git diff --check` 通过。
  - GitNexus 精确实现符号 `launch_http_server` 为 LOW，仅有构造和 `resume()` 两个直接上游；两个入口的 UNKNOWN 已用源码搜索确认工厂构造和虚接口恢复调用点。
- 遗留事项：
  - 本条只关闭端口所有权与就绪误判；Local workdir/宿主绝对路径契约、活跃 Session TTL、Bridge 超时取消和轨迹闭合仍按问题清单逐项处理。
  - 端口冲突修复需要知道 Python 子进程的实际 endpoint，无法在 Harness Python 层单独完成；本条是局限在 `LocalSandbox` 内的必要 C++ 改动。

### CHG-20260829-081：收敛 Local Sandbox 用户路径边界并修复文件工具假成功

- 日期：2026-08-29
- 目标：让 Local Sandbox 在不放宽到整个 Agent 包、不读取其他用户目录的前提下，正确读写当前用户 workspace 和只读 skill 静态资源，并保证文件失败终态真实可回放。
- 诱发原因：
  - Local HTTP server 启动在独立临时 cwd，业务 Hook 和 Skill 使用的却是宿主 Agent Package 下当前用户 workspace 路径。
  - LocalSandbox 与 DockerSandbox 均继承空 `sandbox_mode()`，RPC 将两者统一注入为 `local`，Python 无法只对共享宿主文件系统的 Local 做路径收敛。
  - Server 前置路径检查使用请求 workdir，内部 runtime wrapper 又按 server 自身 cwd 二次检查，合法请求被误拒绝。
  - 内部 wrapper 的早期错误没有 `success` 字段，Server 使用默认值 `True`，把未落盘写入归一化为 `exit_code=0`。
- 改动内容：
  - `HTTPExecutor` 仅在真实 `local` 模式将文件请求映射到 `{AF_AGENT_PACKAGE_DIR}/workspace/{AF_USER_ID}_{AF_CLIENT_ID}`；相对路径也收敛到该根。
  - 当前用户 workspace 可读写；`harness/skills` 仅允许 `read_file/list_dir`；其他用户、其他包路径以及向 skill 目录写入都不授予 workdir，继续由 Server 边界拒绝。
  - Local 相对写入成功后返回真实宿主绝对路径，Artifact 抽取无需再猜 Agent Package 根。
  - HTTP server 把已经通过 `resolve_safe_path` 的绝对路径交给内部文件工具，避免以临时 cwd 重复误拒绝；缺少 `success` 的 error 结果不再默认为成功。
  - C++ 只增加两个一行 override：Local 报告 `local`，Docker 报告 `docker`；Coordinator、Workflow、Session、Bridge 和业务 Prompt 均未修改。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/executors/http.py`
  - `docker/sandbox/http_server/sandbox_http_server.py`
  - `src/sandbox/sandbox_local.h`
  - `src/sandbox/sandbox_docker.h`
  - `packages/agentflow-runtime/tests/test_http_executor.py`
  - `packages/agentflow-runtime/tests/integration/test_file_tools_sandbox_integration.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - HTTPExecutor 单元测试 15 项全部通过。
  - 文件工具真实 HTTP server 集成测试 15 项全部通过；专项用例证明当前用户读写成功、相对产物返回绝对路径、跨用户读取被拒绝、skill 文件可读不可写。
  - `test_daemon_gtest` 重新编译成功；`SandboxLifecycleTest.*` 为 13 passed、1 个既有 skipped。
  - Python 编译和相关文件 `git diff --check` 通过。
- 风险与遗留：
  - 没有把 Agent Package 根设为 Server workdir，因此不会以修复可用性为代价放开跨用户目录。
  - Docker 继续使用容器内路径，不执行 Local 宿主路径转换。
  - 本次真实闭环同时确认 HTTP server 缺少 `list_dir` 与 `web_fetch` 分发；已补入问题清单，按后续条目分别处理。

### CHG-20260829-082：补齐 HTTP Sandbox 的 list_dir 协议

- 日期：2026-08-29
- 目标：让 Local/Docker 的确定性预加载能够真实枚举当前用户 history、upload、outputs 和静态 skills，而不是把协议缺失静默解释成空目录。
- 诱发原因：
  - `HTTPExecutor.list_dir()` 已发送 `tool=list_dir`，但 `sandbox_http_server.py` 的 `TOOL_DISPATCH` 没有登记该操作。
  - runtime tools 目录也没有 `list_dir.py`；Server 因而返回 unknown tool，HTTPExecutor 为保持列表接口又把错误降级成 `[]`。
- 改动内容：
  - 将 `list_dir` 纳入 Server 既有 `resolve_safe_path` 边界和工具分发表。
  - 校验通过后直接复用 `HostExecutor.list_dir()`，不复制遍历逻辑，也不新增可能被 LLM 工具注册器扫描的模块。
  - 保留输出字节截断；当前用户 workspace 和静态 skill 根沿用 CHG-081 的最小授权，兄弟用户目录仍被拒绝。
  - 未修改 C++、Coordinator、Skill Executor、Bridge 或 Prompt。
- 涉及文件：
  - `docker/sandbox/http_server/sandbox_http_server.py`
  - `packages/agentflow-runtime/tests/integration/test_file_tools_sandbox_integration.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 新增真实 HTTP server 用例，验证当前用户目录和静态 skill 目录可枚举、兄弟用户目录返回明确 `Path escape rejected`。
  - 文件工具集成测试 16 项全部通过；HTTPExecutor 单元测试 15 项全部通过。
  - Python 编译和相关文件 `git diff --check` 通过。
- 遗留事项：
  - `web_fetch` 同样存在 client/runtime 已实现但 Server 未登记的问题，已作为下一条独立问题保留。

### CHG-20260829-083：补齐 HTTP Sandbox 的 web_fetch 协议

- 日期：2026-08-29
- 目标：让 Local/Docker 模式下已有的安全 `web_fetch` 工具真正经过 Sandbox HTTP 协议执行，并保留正文和失败语义。
- 诱发原因：
  - `HTTPExecutor.fetch_url()` 会发送 `tool=web_fetch`，runtime 也已有 `harness/tools/web_fetch.py`，但 Server 分发表漏掉该名称。
  - 即使只补分发表，Server 默认分支也只读取 `stdout`；`web_fetch` 与 `read_file` 一样返回 `content`，成功正文仍会丢失。
- 改动内容：
  - 在 `TOOL_DISPATCH` 登记 `web_fetch`。
  - 将 `web_fetch` 纳入 `read_file/load_skill` 的 `content → stdout` 响应归一化分支；错误继续映射为非零 exit code。
  - 未修改 URL 安全策略、网络代理策略、C++、Coordinator、Skill Executor 或业务 Prompt。
- 涉及文件：
  - `docker/sandbox/http_server/sandbox_http_server.py`
  - `docker/sandbox/http_server/tests/test_web_fetch_dispatch.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 新增 3 个无外网协议测试：分发表存在、成功正文进入 stdout、网络错误保持失败。
  - Sandbox HTTP server 测试 10 项全部通过；相邻 HTTPExecutor 与文件工具测试 31 项全部通过。
  - `git diff --check` 通过。
- 遗留事项：
  - 本条验证的是协议闭环；真实公网访问、Sandbox 网络策略和轨迹事件将在最终复杂 hchat 实验中联合验证。

### CHG-20260829-084：防止 Session TTL 回收活跃 Dialogue

- 日期：2026-08-29
- 目标：Session TTL 只回收空闲路由，不得在长 Workflow/Skill 仍运行时销毁 Session、清空 DataBus 或截断轨迹。
- 诱发原因：
  - `is_timeout_expired()` 只比较外部消息入队时更新的 `last_activity_time_`，没有检查异步执行期间一直持有的 dialogue serialization lock。
  - 长任务完成后若不刷新活动时间，旧输入时间戳已经超过 TTL，Session 会在释放 lock 后立即被回收。
- 改动内容：
  - `is_timeout_expired()` 在同一判定中锁住 Session 与 dialogue 状态；存在 `active_dialogue_id_` 时明确返回未过期。
  - `release_dialogue_slot()` 仅在确实释放当前 holder 时调用 `update_activity()`，让空闲 TTL 从业务完成时重新计时。
  - 未改默认 300 秒配置、SessionRouter、Harness Python、Coordinator、Skill Executor 或 Bridge。
- 涉及文件：
  - `src/agent/session.cpp`
  - `tests/unit/src/test_agent_gtest.cpp`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - `test_agent_gtest` 重新编译成功；只有既有 unused warning。
  - `DialogueSerialLockTest.*` 6 项全部通过；新增用例验证空闲 Session 会过期、活跃 dialogue 不过期、完成后活动时间刷新。
  - `git diff --check` 通过。
- 风险与边界：
  - 活跃判断复用既有 RAII dialogue lock，不新增第二套 running/pending 计数。
  - 真正永久挂起的 Workflow 应由节点/Workflow/Bridge 取消链处理，而不再由无业务语义的 Session 空闲 TTL 粗暴终止；该取消链是下一项。

### CHG-20260829-085：Bridge 等待响应超时后取消底层 Dialogue

- 日期：2026-08-29
- 目标：当 Bridge 已拿到 daemon Session、但等待响应超时时，客户端结束与底层执行必须同步，避免后台继续执行和占用 dialogue lock。
- 诱发原因：
  - `_chat_turn_events()` 的两条等待响应超时出口只发送 error 和 transport done，再清理 Bridge 响应队列。
  - 项目已有幂等 `_cancel_dialogue()` 和 daemon `DIALOGUE-INTERRUPT` 完整链路，但超时出口没有复用。
- 改动内容：
  - `agent.execute` 返回后，根据真实 `session_id` 构造或更新本轮取消状态；SSE 继续复用活动对话注册表，同步 REST 使用同结构的局部状态。
  - deadline 已耗尽和 `wait_for_response` 抛出超时的两个出口，在丢弃响应队列前调用既有 `_cancel_dialogue()`。
  - 取消状态保持幂等；若 SSE 断连或显式 cancel 已经发送过中断，超时路径不会重复发送。
  - 未修改 daemon/C++、Workflow、LLM Client、SSE 帧格式或对话历史。
- 涉及文件：
  - `frontend/bridge/routes/chat_rest.py`
  - `frontend/bridge/tests/test_chat_rest_sse.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 专项用例通过：超时事件顺序仍为 session metadata、error、done；Bridge 响应队列仍清理；`DIALOGUE-INTERRUPT` 按 agent/session/user/dialogue 精确发送一次。
  - Python 编译与相关文件 `git diff --check` 通过。
  - 完成 CHG-086 的测试契约同步后，SSE 测试文件 32 项全部通过。
- 风险与边界：
  - 本次只在 `agent.execute` 已返回可验证 `session_id` 后取消，不按 user/client 猜测底层目标，避免误伤并行或后续对话。
  - `agent.execute` RPC 自身超时时 Bridge 尚未得到 Session 标识，当前协议无法安全定位；该边界已从原问题拆出，继续保留为待修复项。

### CHG-20260829-086：同步 Bridge 轨迹投递测试契约

- 日期：2026-08-29
- 目标：让 Bridge 回归测试与既定 user/client/dialogue 轨迹身份契约一致，避免假红掩盖真实回归。
- 诱发原因：生产 `append_bridge_delivery()` 调用已不再传内部 `session_id`，两条旧测试仍直接读取并断言该字段。
- 改动内容：
  - 流式 final/done 测试改为断言所有投递均不含 `session_id`。
  - 同步 REST 投递测试改为断言该字段不存在。
  - 保留 task、status、wire payload、raw/delivered content 的全部原断言；未修改生产代码或轨迹格式。
- 涉及文件：
  - `frontend/bridge/tests/test_chat_rest_sse.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - `frontend/bridge/tests/test_chat_rest_sse.py`：32 passed。
  - 既有 SessionLog 文档与 Hook 测试也明确要求信封和 `client.delivery` 不保存 `session_id/session_slot_id`。
  - `git diff --check` 通过。

### CHG-20260829-087：错误 SSE progress 不再写入助手历史

- 日期：2026-08-29
- 目标：错误或超时轮次的过程展示仍可用于轨迹诊断，但不能伪装成已完成 assistant response 进入下一轮上下文。
- 诱发原因：SSE error 分支在记录 error 与 transport done 后，仍将此前累计的全部 `sse_chunks` 同时写入 legacy conversation store 和 durable SessionProvider。
- 改动内容：
  - 仅删除 error 分支的两次对话持久化；error SSE 帧、`client.delivery(status=error)`、`done_after_error` 和响应队列/取消行为保持不变。
  - 正常 final 分支仍只保存 final chunk；同步 REST 路径不变。
  - 增加真实 FastAPI SSE 回归：先收到 progress、再超时，验证客户端仍收到 progress/error/`[DONE]`，两套对话历史均没有该未完成轮次。
  - 未修改 C++、daemon、Coordinator、Skill Executor、SessionLog Hook 或 SSE wire format。
- 涉及文件：
  - `frontend/bridge/routes/chat_rest.py`
  - `frontend/bridge/tests/test_chat_rest_sse.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - `frontend/bridge/tests/test_chat_rest_sse.py`：33 passed。
  - 新用例确认 error 前 progress 带 `_progress` 和 dialogue identity，error/done 仍可观察；Provider `save_dialogue_calls=[]`，legacy store 按该 dialogue 查询为空。
  - `git diff --check` 通过。
- 边界：
  - 失败事实由轨迹的 error 与 transport terminal 保存，不再混入业务对话历史。
  - 本条不改变 `[DONE]` 的传输语义；正常/异常都发送同一 sentinel 的歧义按下一项独立处理。

### CHG-20260829-088：显式分离 SSE 业务终态与传输终态

- 日期：2026-08-29
- 目标：保留 OpenAI-compatible `[DONE]` 的传输兼容性，同时让客户端和轨迹树无需猜测本轮是成功还是失败。
- 诱发原因：成功帧已有 `_final=true`，错误帧却只携带 error 对象；随后两者都发送同一个 `[DONE]` sentinel。
- 改动内容：
  - 在 `_SSE_DONE` 定义处固化契约：它只表示 transport EOF，不能单独作为业务成功依据。
  - error 帧增加 `_dialogue_id`、`_final=false` 和 `_terminal_status=error`；原 error 对象和 `[DONE]` 顺序不变。
  - 成功路径继续以既有 `_final=true` 为唯一业务成功标记；SessionLog 继续以 `status=error` 与 `done_after_error` 保存异常交付事实。
  - 未删除 `[DONE]`，未改变 OpenAI SSE 基本格式，也未修改 C++、daemon、Coordinator、Skill 或 Hook。
- 涉及文件：
  - `frontend/bridge/routes/chat_rest.py`
  - `frontend/bridge/tests/test_chat_rest_sse.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - SSE 与 `client.delivery` 联合回归：41 passed。
  - 错误流顺序保持 progress、error、`[DONE]`；error 与 progress 的 dialogue identity 相同，且 error 明确为 `_final=false/_terminal_status=error`。
  - `git diff --check` 通过。
- 建树规则：
  - `[DONE]` 只闭合 transport 边；业务成功必须存在 `_final=true` 或 `client.delivery.status=final`。
  - error 帧或 `client.delivery.status=error/done_after_error` 闭合失败边，不得被后续 `[DONE]` 覆盖。

### CHG-20260829-089：execute 超时按精确路由查找 Session 后取消

- 日期：2026-08-29
- 目标：`agent.execute` 响应超时时，如果 daemon 已创建本轮 Session，Bridge 仍能安全取消；不能按 user 猜测或误伤其他 client。
- 诱发原因：Bridge 只从 execute 成功结果取得 `session_id`；前置超时出口没有取消目标，直接丢弃本地队列并结束。
- 改动内容：
  - 在 execute 前建立与等待响应共用的幂等取消状态；execute 超时先调用 `_cancel_dialogue()`，再丢弃本地响应队列。
  - `_cancel_dialogue()` 仅在没有 session_id 时调用已有只读 `session.list(agent_id, user_id)`，同时精确匹配 `user_id+client_id`。
  - 只有唯一匹配才补全 session_id 并发送现有 `DIALOGUE-INTERRUPT`；零匹配、多匹配、查询失败均只记录 warning，不发送控制命令。
  - execute 成功路径继续优先使用 daemon 返回的 session_id，不增加额外查询。
  - 未修改 C++、daemon RPC、SessionRouter、Workflow 或 LLM Client。
- 涉及文件：
  - `frontend/bridge/routes/chat_rest.py`
  - `frontend/bridge/tests/test_chat_rest_sse.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - SSE 完整回归：34 passed；execute 超时用例确认按查得 Session 精确发送一次中断。
  - session.list 路由与 SessionSync 联合回归：合计 77 passed、1 个因真实固化态 `.env` 不存在而按原测试条件 skipped。
  - 负向用例确认同 user、不同 client 的 Session 不会触发 `agent_send_control`。
  - Python 编译与 `git diff --check` 通过。
- 边界：
  - 若 execute 请求根本没有抵达 daemon，session.list 无匹配，正确行为是不发送取消。
  - Workflow 节点预算与单次 LLM/Tool 预算未对齐是独立问题，已按实际代码拆成下一条待修复项。

### CHG-20260829-090：Skill LLM 请求服从节点剩余预算

- 日期：2026-08-29
- 目标：在 240 秒 Workflow 节点硬超时杀进程前，让硬压缩与主 LLM 请求自行结束，并留出写结构化结果、`skill.end` 和 span 的时间。
- 诱发原因：原循环只在每轮顶部比较 elapsed；一次硬压缩和一次主 LLM 都读取固定 `AF_LLM_TIMEOUT`，没有共享节点 deadline。
- 风险分析：
  - GitNexus 对公共 `call_llm` 返回 HIGH：4 个直接调用方、4 类执行流、16 个上游影响点。
  - 因此没有修改该公共函数；实际修改的 Skill `execute` 为 LOW，默认行为只限于独立 Skill 子进程。
- 改动内容：
  - Skill 进入时保存原 `AF_LLM_TIMEOUT`，以 Python 进程启动时间和框架注入的 node timeout 计算绝对 deadline。
  - 每轮压缩前将子进程内 LLM timeout 设为“原配置与可用剩余秒数的较小者”；压缩后、主 LLM 前再次计算，防止两次调用重复消费同一预算。
  - deadline 预留最多 5 秒，并要求至少还有 1 秒请求预算；不足时不再发 LLM，复用结构化 `node_timeout` 结果路径。
  - 所有 return/异常均在现有最外层 `finally` 恢复原环境；`skill.end` 与 Skill/LLM span 沿现有 finally 闭合。
  - 未修改公共 `call_llm`、Recorder、HTTP Client、Coordinator、C++ 或 Workflow JSON。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_recovery_control_flow.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - Skill recovery、interrupt、并行 Tool 联合回归：22 passed。
  - 语义恢复的连续两次 LLM 调用均观察到 timeout 小于 3600 秒节点预算；执行结束后原 `AF_LLM_TIMEOUT=7200` 恢复。
  - Python 编译与 `git diff --check` 通过。
- 遗留事项：
  - Tool 调用预算当时仍为独立问题，后续已由 CHG-20260829-091 解决。

### CHG-20260829-091：Skill Tool 调用服从节点剩余预算

- 日期：2026-08-29
- 目标：在 Workflow 硬超时前停止长 Tool，先保存逐调用结果，再闭合 Tool batch、Skill 终态和 span。
- 诱发原因：Skill 只在轮次顶部检查节点耗时；Tool 调度器既没有 deadline，也无法真正停止已开始的长视频轮询或未知业务 Tool。
- 风险分析：
  - GitNexus 对 `execute_in_batches` 返回 LOW：2 个直接调用方、2 类执行流；Skill `execute` 也为 LOW lower-bound。
  - 新参数均为可选且只由 Skill 传入；Coordinator 继续按原签名调用，执行行为不变。
- 改动内容：
  - 复用 CHG-090 的同一个节点 deadline 与最多 5 秒落盘预留，不新增第二套超时口径。
  - `execute_in_batches` 增加可选 deadline。当前 harness 中业务、媒体、MCP 和未知 Tool 均为保守串行批次；Linux Skill 主线程用 `SIGALRM` 抛出内部 `BaseException`，越过业务 Tool 常见的宽泛 `except Exception` 后在调度层转换为结构化 timeout。
  - deadline 到达后不再启动剩余串行 Tool，但仍为每个调用生成原顺序、原 call id 对应的 `status=node_timeout` 结果，避免 `tool.start` 无结果。
  - 当前可并行批次仅包含白名单只读 Tool 或带显式输出的 bash；Skill 将这些 Tool schema 中声明的 `timeout/poll_timeout` 收缩到相同 deadline。
  - 任一 Tool 触及 deadline 后，先走原有 Tool Result/Hook/消息落盘，再将 Tool batch 标记 ERROR，并复用 `_finish_node_timeout` 生成 Skill 终态；不进入下一轮 LLM。
  - 未修改 Tool Registry、媒体脚本、Coordinator、Workflow JSON、C++ 或 daemon。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/tool_policies.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_tool_policies.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_tool_parallel.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - Tool 分类、并行、串行、保序和 deadline 单测：15 passed。
  - Tool 策略、Skill 并行与 Skill recovery/interrupt 联合回归：31 passed。
  - deadline 用例确认正在阻塞的第一个未知 Tool 被中断，后续未启动 Tool 仍获得保序 timeout 结果；并行 Tool 集成用例确认默认 30 秒 timeout 被节点剩余预算收缩。
  - Python 编译与相关文件 `git diff --check` 通过。
- 边界：
  - 硬中断依赖当前部署的 Linux Skill 主线程模型；若未来把 Skill Executor 改成同进程线程池或非 POSIX 平台，需要将同一 deadline 下沉到进程级 Tool worker。
  - 新增可并行 Tool 时必须提供可取消/有界 timeout；未知工具默认仍保守串行并受硬截止保护。

### CHG-20260829-092：复核第二批缺失终态并纠正 Batch 问题归因

- 日期：2026-08-29
- 目标：确认复杂实验中“第一批有 batch.result、第二批无终态”是否仍是当前 Batch 状态机缺陷；不为已修复的外部生命周期问题重复改高风险 C++。
- 原始证据复核：
  - 旧轨迹 `0828173438-8e50bf4cc390` 第一批 3 个 Skill 均有 `skill.end`，随后保存 `batch.result(status=partial, results=3)`。
  - Coordinator 派发第二个全新 Batch 后，唯一 `outfit-stylist` 分支停在第 3 轮 `llm.request`；它没有对应 `llm.response`，也没有 `skill.end`。因此原清单所写“4 个 skill.start/end 均完整”不符合轨迹。
  - 客户端在整轮约 420 秒时结束；旧实现同时存在活跃 Session 300 秒 TTL、Bridge 超时不取消、LLM/Tool 不服从节点剩余预算。第二批缺失是执行分支被外部生命周期切断后的结果，不是 ALL-join 覆盖或 Batch ID 冲突。
- 当前闭合机制：
  - Python 每次成功 `run_skills` 都生成新的 UUID `batch_id` 和 `exec_skill_key`；重试/补充批次不会复用前一批身份。
  - C++ 以 `session_id+dialogue_id+batch_id` 建立独立状态，每个 `exec_skill_key` 只有一个 terminal slot；失败仅填本分支，重复或迟到 completion 幂等忽略，ALL-join 只触发一次 Coordinator。
  - Coordinator 在后续 LLM 或 max-iteration 早退前先解析并投影 `batch.result`、写 consumed marker；所有正常、错误、中断和上限出口统一投影 `dialogue.end`。
  - CHG-084 防止活跃 Session 被 TTL 回收；CHG-085/089 让 Bridge 超时精确取消底层 Dialogue；CHG-090/091 让 LLM 与 Tool 在节点硬上限前产生结构化终态。
- 代码处理：
  - 当前主链无需新增运行时代码，未修改 C++、Coordinator 或 SessionLog Hook。
  - 仅修正问题清单中的事实、根因和状态，避免后续为错误归因扩大底座改动。
- 验证结果：
  - Coordinator Batch、SessionLog Hook 与 Reader 联合回归：79 passed。
  - C++ `HarnessBatchBackfillsSlotsAndTriggersCoordinatorOnce` 与 `SessionInterruptTerminatesActiveAndQueuedHarnessBatchSkills`：2 passed。
  - C++ 用例覆盖 5 分支并发上限回填、乱序完成、1 个失败形成 partial、重复 completion、consumed Final Gate、迟到 completion，以及活动/排队分支中断后的唯一 Batch terminal。
- 涉及文件：
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`

### CHG-20260829-093：结构化产物成功后立即收敛

- 日期：2026-08-29
- 目标：图片等产物已由结构化工具确认生成后，不再用 Shell/PIL 重复验证；保留多产物任务继续执行的能力。
- 诱发原因：原自动收集提示只说明平台会发布文件，没有把 `status=succeeded` 且 `files` 非空定义为该产物的权威完成条件。
- 风险分析：
  - GitNexus 对模块级提示词常量返回 `UNKNOWN`；文本检索确认它只经 prompts 导出并由 Skill Executor 拼入 system prompt。
  - 没有改 Tool loop、成功状态解析或强制提前退出，避免单图成功错误截断仍有其他待生成产物的任务。
- 改动内容：
  - 在通用 `SKILL_ARTIFACT_AUTO_COLLECT_PROMPT` 中明确：结构化生成工具返回成功状态且 `files` 非空后，该产物完成。
  - 禁止再用 bash、Python/PIL、file、ls 或 read_file 复核同一成功产物。
  - 仅当用户要求的其他产物尚未完成时继续调用工具；全部完成后立即返回 `skill_result`。
  - 未修改各 Skill 副本、Skill Executor 状态机、媒体 Tool、C++ 或 Workflow。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/prompts/skill.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - Prompt 导出检查确认新增收敛条件由 Skill Executor 可见。
  - Skill Tool 并行与 recovery 控制流回归：16 passed。

### CHG-20260829-094：Artifact 只按本次新观测路径持久化

- 日期：2026-08-29
- 目标：消除失败/无产物 Tool 对历史 Artifact 的重复上传和重复轨迹，同时保留真实覆盖生成。
- 诱发原因：Skill Executor 每处理一个 Tool 结果都把跨轮累计的 `generated_artifacts` 全量重新送入产物管线；最终文本扫描也先持久化、后去重。
- 风险分析：
  - GitNexus 对 `skill_executor.execute` 返回 LOW：8 个上游影响点下界、1 个直接入口。
  - 修改限于传给现有产物管线的输入集合，不改变提取、搬运、上传、SessionLog 投影或 C++。
- 改动内容：
  - 每个 Tool 只把该次结果实际提取出的路径交给 `_ensure_artifacts_in_package`，空结果不再触发空持久化事件。
  - 同时记住该次原始路径与归一化路径；最终回复只补收此前未观察到的路径，去重发生在持久化之前。
  - 生成工具若在后续真实调用中再次返回同一路径，仍会进入产物管线，准确记录覆盖生成。
  - `generated_artifacts` 的最终去重列表、Hook 输入和 Coordinator 返回协议保持不变。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_tool_parallel.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - Skill Tool 并行与 recovery 控制流回归：16 passed。
  - 聚焦用例确认“成功工具报告 1 个产物 → 后续工具失败 → 最终文本无新产物”只调用持久化管线 1 次。

### CHG-20260829-095：Bridge access log 脱敏保留 Uvicorn 参数契约

- 日期：2026-08-29
- 目标：Bridge HTTP access log 继续脱敏且不再触发 Uvicorn formatter traceback。
- 诱发原因：原过滤器先调用 `record.getMessage()` 固化文本，再清空 `record.args`；`AccessFormatter` 随后仍要解包 client、method、path、HTTP version、status 五个参数。
- 风险分析：
  - GitNexus 因标准库动态调用 `logging.Filter.filter` 返回 UNKNOWN；文本检索确认该过滤器仅由 Bridge 安装到 active handlers。
  - 分支严格限定为名称 `uvicorn.access` 且参数为 5 元 tuple 的标准记录；其他日志行为保持不变。
- 改动内容：
  - Uvicorn access record 分别脱敏格式字符串和 5 元参数，保留 tuple 形状及整数 status code。
  - 普通日志和 traceback 继续先完整渲染、整体脱敏，再清空参数与异常对象。
  - 共享脱敏器不可用或返回异常形状时，access record 使用同形安全占位参数，fail-closed 且仍可格式化。
- 涉及文件：
  - `frontend/bridge/services/secret_safety.py`
  - `frontend/bridge/tests/test_sessionlog_delivery.py`
  - `docs/harness-v1-code-reading/HARNESS_PROBLEM_LIST_CLASSIFIED.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 修复前的真实 `AccessFormatter` 复现稳定报 `expected 5, got 0`；修复后的正常与 sanitizer-unavailable 两条路径均可格式化。
  - URL 测试凭据不出现在最终 access 文本中，正常请求仍含方法和 `200 OK`。
  - Bridge SessionLog delivery/脱敏回归：9 passed。

### CHG-20260829-096：Bridge 普通 Uvicorn 日志使用已渲染正文

- 日期：2026-08-29
- 问题：通用日志分支清空 `record.args` 后，Uvicorn 彩色 formatter 又使用含占位符的 `color_message`，导致启动日志显示 `%s/%d`。
- 最小改动：仅对非 access 的 `uvicorn.*` 记录移除失效 `color_message`；已完成渲染和脱敏的 `record.msg`、access 五元参数契约及其他日志均不变。
- 涉及文件：`frontend/bridge/services/secret_safety.py`、对应聚焦测试、问题清单和本记录；未修改 C++、Harness Runtime 或 SessionLog Hook。
- 验证：真实 Uvicorn `DefaultFormatter(use_colors=True)` 下参数正常展开且凭据不泄露；Bridge SessionLog/脱敏测试 10 passed。

### CHG-20260829-097：最终 LLM 文本不再充当新 Artifact 来源

- 日期：2026-08-29
- 问题：最终回复只要提到一个磁盘上仍存在的产物路径，就会通过文本正则被当成本轮新产物；即使语义明确写着“生成失败/没有生成”也会误收。
- 风险分析：GitNexus 对 `extract_all_artifacts` 返回 LOW（1 个直接调用者、1 个执行流程），对 `skill_executor.execute` 返回 LOW 下界；测试函数因 pytest 动态发现为 UNKNOWN，文本检索确认没有显式调用者。
- 最小改动：
  - 最终 LLM 文本仍可扫描用于审计，但未被本轮 Tool 观测的路径只记录数量，不再进入搬运、上传和 `generated_artifacts`。
  - Tool Result 的结构化字段与 legacy 文本双通道保持不变；Hook 追加 Artifact、去重、上传、Coordinator 聚合和 C++ 均未修改。
- 涉及文件：`packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`、对应并行测试、问题清单和本记录。
- 验证：现有并行 Tool 场景中，成功 Tool 的产物仍只持久化一次；最终文本额外提到的未观测旧路径不会触发第二次持久化。聚焦测试 8 passed。

### CHG-20260829-098：Tool Artifact 账本只采信结构化字段

- 日期：2026-08-29
- 问题：任意 Tool Result 的 stdout、网页正文或错误文本只要出现一个可访问的图片/文档路径，就会被文本正则当成该 Tool 本轮生成的 Artifact。
- 诱发原因：Skill Executor 对所有 Tool 统一调用 `extract_all_artifacts`，无法区分生成工具的路径输出与 `web_fetch/read_file` 等只读内容中的引用。
- 风险分析：GitNexus 对 `extract_all_artifacts` 返回 LOW（唯一直接调用者为 Skill Executor），`skill_executor.execute` 返回 LOW 下界；未出现 HIGH/CRITICAL 风险。
- 最小改动：
  - Tool Result 写入产物账本时改用已有 `extract_structured_artifacts`，只接受 `files/artifacts/outputs/result/path` 中的列表项。
  - `extract_artifact_paths/extract_all_artifacts` 公共兼容 API 未删除、未改行为；最终 LLM 文本仍可调用它做不落盘审计。
  - 当前 `write_file`、`generate_image`、`generate_video` 均已返回结构化 `files`；未修改 Tool、Sandbox、Artifact 管线、Hook、Coordinator 或 C++。
- 涉及文件：`packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`、对应并行测试、问题清单和本记录。
- 验证：Skill 并行与恢复控制流 16 passed；Artifact 文本/结构化/E2B/上传边界直接相关测试 39 passed。大 Artifact 旧测试文件另有 9 个已存在的 URL helper/包名迁移失败，与本次调用点无关，未为其扩大修改。

### CHG-20260829-099：空 Artifact 字段不再遮蔽后续非空字段

- 日期：2026-08-29
- 问题：结构化 Tool Result 的 `artifacts=[]` 会让提取器提前停止，后续非空 `files` 无法进入产物账本。
- 风险分析：GitNexus 返回 LOW（1 个直接调用者、1 条 Skill Executor 执行流程、3 个上游影响点，结果为 exact）。
- 最小改动：`extract_structured_artifacts` 只在字段值为非空 list 时短路；字段顺序、非空字段优先级、路径过滤、上传、Hook、Coordinator 和 C++ 均未修改。
- 涉及文件：`packages/agentflow-runtime/src/agentflow_runtime/artifact_pipeline.py`、一个直接回归测试、问题清单和本记录。
- 验证：修复前 `{"artifacts":[],"files":["...html"]}` 稳定返回空；修复后定向测试 6 passed，既有结构化 Artifact 契约测试 17 passed。

### CHG-20260829-100：web_fetch 正确解码 gzip 响应

- 日期：2026-08-29
- 问题：`HostExecutor.fetch_url` 将带 `Content-Encoding: gzip` 的压缩字节直接按页面字符集解码，Local Sandbox 的 `web_fetch` 因复用该实现而向模型返回乱码。
- 风险分析：GitNexus 返回 LOW（1 个直接上游调用者，结果为 exact）；HTTP Sandbox 的动态委托未被图索引捕获，但真实 hchat 已确认该链路。
- 最小改动：在现有 `resp.read()` 和字符集解码之间，仅当响应头包含 gzip 时调用标准库解压；无压缩响应、HTML 清理、SSRF 校验、Executor 接口、Sandbox 协议和 C++ 均不变。
- 涉及文件：`packages/agentflow-runtime/src/agentflow_runtime/executors/host.py`、一个直接回归测试、问题清单和本记录。
- 验证：Host/HTTP/SSRF 回归 48 passed，Runtime 非集成测试 470 passed；真实 python.org 响应从 `1f8b` 压缩字节恢复为可读正文。完整 hchat 只调用 1 次 `web_fetch`，SessionLog 26 个事件校验通过、0 个 Artifact，7 个 Span 全部闭合，5 个运行文件秘密扫描通过。

### CHG-20260829-101：建立厂商无关的凭据隔离与提交门禁

- 日期：2026-08-29
- 目标：用户把任意厂商凭据粘贴进消息、Tool 参数或错误文本时，不得进入 LLM 出站请求、SessionLog、普通日志、流式回调或可提交源码。
- 诱发原因：旧逻辑主要依赖已注册环境变量的精确值；未注册的裸 provider key、分片 SSE 回显及不同命名的 secret 字段可绕过边界。
- 最小改动：
  - Runtime 统一按来源名称注册环境凭据，并同时匹配精确值、URL 编码、Base64、JSON 转义和常见 provider 前缀；结构化字段按 key/token/secret/password/credential/signature 等语义脱敏。
  - LLM、Tool、MCP、媒体、Artifact、Hook、Span 和 SessionLog 的外部/持久化边界复用同一策略；脱敏器不可用时写盘侧 fail-closed。
  - 流式 LLM 回调使用有状态分片脱敏，防止同一凭据被拆到多个 delta 后在接收端重组。
  - Daemon 用户输入日志只保留长度和身份，不打印原始正文；Bridge 为普通日志、异常和 Uvicorn 日志安装同一安全过滤器。
  - 增加只输出路径、行号、规则和字段名的源码扫描器与 CI 门禁，不输出匹配值或值派生哈希。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/security/`
  - `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/hooks/engine.py`
  - `docs/sessionlog/sessionlog_hook.py`
  - `frontend/bridge/services/secret_safety.py`
  - `frontend/bridge/main.py`
  - `src/daemon/rpc_handler.cpp`
  - `scripts/quality/scan_secrets.py`
  - `.github/workflows/credential-safety.yml`
- 验证结果：
  - Runtime 非集成测试 479 项通过；SessionLog 54 项通过；相关 Bridge 契约 179 项通过、1 项既有可选配置跳过。
  - Git 追踪文件 1713 个、未追踪候选文件 59 个的凭据扫描均为 0 findings。
  - 单测覆盖任意环境变量名、常见 provider 形状、编码形态、SSE content/tool arguments 分片、错误回显和 sanitizer 不可用。
- 边界：本条防止后续持久化与提交泄露；已进入 Git 历史或外部服务的旧凭据仍需由凭据提供方轮换。

### CHG-20260829-102：复用 Sandbox 时刷新 GC 活跃时间

- 日期：2026-08-29
- 目标：多轮会话继续使用已有 HTTP Sandbox 时，不再因首次创建时间超过 TTL 而被 GC 当成空闲实例回收。
- 诱发原因：Workflow Tool 直接访问已注入 endpoint，不经过 `Sandbox::exec()`；旧 `SandboxLifecycle::create()` 命中已有实例后直接返回，没有更新 `last_active_at`。
- 最小改动：
  - 仅在 `SandboxLifecycle::create()` 的既有复用分支调用一次 `mark_active()`。
  - 每个外部 user turn 都经 `handle_agent_execute → create`，因此长期多轮会话会按真实轮次刷新；创建、GC、endpoint 和 Tool 协议均不改变。
  - 现有 StubSandbox 测试改为调用真实 lifecycle 复用分支，并断言时间戳从 0 刷新。
- 涉及文件：
  - `src/daemon/sandbox_lifecycle.cpp`
  - `tests/integration/sandbox_lifecycle_test.cpp`
- 验证结果：
  - `SandboxLifecycleTest.SameUserReusesSandbox` 通过。
  - 现有 Bridge 单轮 deadline 小于默认 Sandbox TTL；长任务仍由节点预算和 Bridge 取消链在 GC 前结构化终止。
- 边界：不伪造 endpoint 内部请求计数；当前 Harness 的真实活跃边界是外部 dialogue turn，内部节点仍受 CHG-090/091 的有界 deadline 约束。

### CHG-20260829-103：Tool 业务失败上卷为 Skill 业务终态

- 日期：2026-08-29
- 目标：Tool 已明确失败时，即使 LLM 随后生成非空总结，也不能把该 Skill 样本标成业务成功；同时不让业务失败改变 Workflow 进程是否正常完成。
- 诱发原因：旧最终分支只检查 LLM 文本是否非空，固定输出 `status=completed`，没有保留跨轮尚未恢复的 Tool 失败。
- 最小改动：
  - 按 Tool 名和规范化参数维护未恢复失败账本；同一调用后续成功才清除对应失败。
  - 识别结构化 error、false success/ok、非零 exit code 以及 failed/timeout/security blocked 等业务状态。
  - 最终文本非空但仍有未恢复失败时输出 `status=tool_error` 和结构化 error；Python 仍正常退出，Workflow completion 与业务状态继续分离。
  - 单一并发分支失败只使该 Skill/Batch 呈 partial，不取消兄弟分支，也不改变结果保序。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py`
  - `packages/agentflow-runtime/tests/test_skill_executor_tool_parallel.py`
- 验证结果：
  - Skill 并行定向测试 8 项通过；Runtime 非集成全量 479 项通过。
  - 回归明确断言失败场景为 `tool_error`，同时保持 Workflow 正常退出语义。

### CHG-20260829-104：生成媒体落入用户 Conversation 工作区

- 日期：2026-08-29
- 目标：图片、视频和音频的宿主机落盘与文件 Tool 使用同一用户/Conversation 隔离边界，不再写入 Agent 全局 `assets/`。
- 诱发原因：无 Sandbox endpoint 时，`publish_media()` 直接以 Agent 包根解析相对媒体路径，两个用户的同名产物可互相覆盖。
- 最小改动：
  - Host 模式要求同时存在 user/client 身份，并将相对媒体路径投影为 `workspace/{artifact_routing_id}/...`。
  - 已经带有同一 workspace 前缀的路径保持幂等，不重复嵌套；缺少部分身份时明确失败。
  - Sandbox 模式继续写入每个实例自己的工作区，不改变远端上传协议。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/security/trusted_media.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/common/runtime_context.py`
  - `packages/agentflow-runtime/tests/test_security_boundaries.py`
  - 图片、视频生成脚本的固化副本
- 验证结果：
  - 媒体安全边界与 Artifact 路由测试随 Runtime 479 项全量通过。
  - 测试确认首次发布和对已发布路径再次发布均落在同一隔离目录。

### CHG-20260829-105：Artifact 下载改为不可猜测的能力链接

- 日期：2026-08-29
- 目标：知道 Agent 名和文件路径但没有本次响应发出的 capability 时，不能读取 Artifact。
- 诱发原因：旧 GET 路由只做路径与后缀校验；任何 Bridge 调用方都可猜测其他用户的静态路径并下载。
- 最小改动：
  - Bridge 使用持久化本地 HMAC key 为完整 artifact id 签名；新旧 GET 路由均要求 `sig`，缺失或不匹配返回 403。
  - Artifact 提取、会话响应和历史回放在交付前统一补签；普通日志和轨迹脱敏 capability 查询串。
  - 保留路径穿越、symlink 和文件类型白名单；签名只增加访问条件，不放宽原边界。
  - 旧下载测试通过真实签名客户端访问，并保留专门的“无签名 403”负向测试。
- 涉及文件：
  - `frontend/bridge/routes/artifacts.py`
  - `frontend/bridge/routes/artifact_extractor.py`
  - `frontend/bridge/routes/sessions.py`
  - `frontend/bridge/content_serializer.py`
  - 相关 Bridge 与 Runtime 安全测试
- 验证结果：
  - Artifact capability、存储、MIME、双路由和聊天交付联合测试 75 项通过。
  - 本轮涉及的 Bridge 配置、聊天、轨迹和 Artifact 契约 179 项通过、1 项既有可选配置跳过。
- 边界：签名 URL 本身是 bearer capability，获得该 URL 的接收方可按设计回放；它不应进入普通日志或无关用户历史。

### CHG-20260829-106：Artifact 路由键消除身份拼接碰撞

- 日期：2026-08-29
- 目标：`user_id=a_b, client_id=c` 与 `user_id=a, client_id=b_c` 等组合不能映射到同一物理目录，超长合法身份也不能超过文件系统单段限制。
- 诱发原因：旧路由直接拼接 `{user_id}_{client_id}`，分隔符也允许出现在两个分量内。
- 最小改动：
  - 无下划线身份保留兼容格式；存在歧义时使用带 UTF-8 字节长度的 v2 编码。
  - RPC 既有身份长度上限保证路由段不超过 255 字节；缺少任一身份时不生成目录名。
  - 媒体发布、Artifact 上传、URL 生成和轨迹投影共用同一构造函数。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/common/runtime_context.py`
  - `packages/agentflow-runtime/tests/test_artifact_routing_id.py`
  - `frontend/bridge/tests/test_e2e_comprehensive.py`
- 验证结果：
  - 覆盖歧义对、最大长度、部分身份、上传 body 和 URL 的回归均通过。
  - E2E 契约确认 v2 路由 URL 经 Bridge 签名后可下载。

### CHG-20260829-107：sandbox-mode=none 时仍初始化 Daemon SessionManager

- 日期：2026-08-29
- 目标：关闭 Sandbox 的本地模式仍能创建 RpcHandler、维护 user/client 会话并正常启动 daemon。
- 诱发原因：旧初始化只在 sandbox mode 非 none 时构造 `session_manager_`，随后无条件解引用它传给 RpcHandler，导致 none 模式启动阶段空指针。
- 最小改动：
  - 将既有 `SessionManager` 构造移到 Sandbox 条件分支前；SandboxLifecycle、Snapshots、TemplateRegistry 和 GC 仍只在启用 Sandbox 时创建。
  - 未改变 RPC、Session 数据结构或 Sandbox 行为。
- 涉及文件：
  - `src/daemon/daemon.cpp`
- 验证结果：
  - Daemon 定向测试与此前完整 CTest 22/22 通过；最终完整回归在本轮收尾再次执行。
- 风险：GitNexus 对动态入口给出 UNKNOWN；文本检索确认直接调用方为 daemon main 和初始化测试，未把空 caller 当作低风险。

### CHG-20260829-108：同步回归基线到正式运行契约

- 日期：2026-08-29
- 目标：消除测试自身的误判和过期断言，防止假红掩盖真正的 Harness/轨迹回归。
- 诱发原因：
  - descendant 清理测试的 `pgrep -f marker` 会匹配包含 marker 的父 shell。
  - DataBus E2E 使用目录旧名而非 package.json 中的真实 agent id。
  - 新增 evaluation 事件、LLM 重试预算分配、production reload、dev timeout、Artifact capability 和 v2 路由后，部分断言仍按旧契约。
- 最小改动：
  - pgrep/pkill 锚定目标进程 argv[0]；DataBus E2E 使用 `outfit-assistant`。
  - 事件数量更新为 34；120 秒/3 次尝试的首轮 read timeout 断言为 40 秒。
  - production 测试只验证无 reload，不再绑定监听地址；dev RPC timeout 对齐 60 秒配置。
  - Artifact 测试使用真实签名，不关闭生产鉴权；E2E 使用正式 v2 路由构造函数。
- 涉及文件：
  - `tests/unit/src/test_exe_loader_gtest.cpp`
  - `frontend/bridge/tests/test_databus_cleanup_e2e.py`
  - Runtime/Bridge 相关测试文件
- 验证结果：
  - Runtime 479 项通过；Sandbox/脚本 24 项通过；本轮 Bridge 相关 179 项通过、1 项跳过。
  - 生产代码语义未为测试放宽。

### CHG-20260829-109：client.delivery 按 User/Client 精确写入轨迹文件

- 日期：2026-08-29
- 目标：不同用户恰好使用相同 dialogue_id 时，Bridge 最终交付事件不能写进另一个用户的轨迹文件。
- 诱发原因：旧 `_sessionlog_path()` 在整个 SessionLog 根目录按 dialogue 文件名全局搜索，未使用 chat request 已有的 user_id/client_id。
- 最小改动：
  - Bridge append API 增加可选 user/client 参数；正式 REST/SSE/cancel 调用全部传入原请求身份。
  - 新布局优先精确定位 `by-user/{user}/{client}/{dialogue}.jsonl`；兼容旧目录时也只在同一 user 下搜索，不跨用户回退。
  - 未找到精确文件时 fail-open 返回 false，不创建或污染其他用户文件。
  - 保留无身份旧调用方的历史全局查找兼容行为。
- 涉及文件：
  - `frontend/bridge/services/sessionlog_delivery.py`
  - `frontend/bridge/routes/chat_rest.py`
  - `frontend/bridge/tests/test_sessionlog_delivery.py`
  - `frontend/bridge/tests/test_chat_rest_sse.py`
- 验证结果：
  - 两用户同 dialogue_id 定向用例确认只追加目标文件；不存在的第三身份不会写入任一文件。
  - SessionLog delivery、Chat REST 和 Skill 并行联合 53 项通过；本轮 Bridge 相关总计 179 项通过、1 项跳过。
- 风险分析：GitNexus 对 `_sessionlog_path` 返回 HIGH（5 个上游调用点）；修改前已明确警告，最终变更只收紧正式调用的查找范围，旧调用签名保持兼容。
### CHG-20260829-110：流式脱敏只保留可能组成凭据的尾前缀

- 日期：2026-08-29
- 问题：`SecretStreamRedactor` 按最长已注册凭据长度固定缓冲流尾，长 Key 会让完全无关的短中文 delta 也迟迟不进入 hchat。
- 风险分析：GitNexus 对该类返回 MEDIUM，覆盖 38 个上游使用点；修改保持公开接口和最终脱敏结果不变，只收窄内部 holdback。
- 最小改动：
  - 在每次已完成脱敏后，只计算当前文本尾部与任一已注册凭据表示前缀的最长重合。
  - 仅保留该重合部分等待下一 chunk；无关文本立即释放。
  - `flush()`、脱敏报告、跨 content/tool arguments chunk 的凭据阻断语义均不变。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/security/secrets.py`
  - `packages/agentflow-runtime/tests/test_secret_security.py`
  - `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
- 验证结果：聚焦测试覆盖正常中文立即释放、凭据前缀暂存、跨 chunk 完整脱敏；Runtime 非集成回归 514 passed、4 skipped。

### CHG-20260829-111：补齐可复现的 Bridge/Runtime 开发测试依赖

- 日期：2026-08-29
- 目标：独立创建的 `agentflow-dev` 环境应能直接运行 Bridge 与 Runtime 回归，不能依赖当前 shell 偶然已有的包。
- 最小改动：
  - `environment.dev.yml` 增加 uv。
  - Bridge 声明本仓库 `agentflow-runtime` 的 editable 本地依赖并同步 lock。
  - 旧 LLM mock 的远程明文 HTTP 占位地址改为 loopback；内容序列化测试直接核对 Runtime 的正式 Artifact 路由契约。
  - 只调整环境和过期测试，不放宽生产 HTTPS、凭据或 Artifact 安全策略。
- 涉及文件：`environment.dev.yml`、`frontend/bridge/pyproject.toml`、`frontend/bridge/uv.lock` 及对应旧回归测试。
- 验证结果：Bridge 回归 339 passed、2 skipped、7 deselected；Outfit Harness 177 passed。

### CHG-20260829-112：Local Sandbox E2E 权限仅在子进程显式开启

- 日期：2026-08-29
- 问题：正式安全默认值禁止 Local Sandbox 读取宿主凭据后，旧 E2E 仍隐式依赖该危险能力。
- 最小改动：只在测试 fixture 创建的 Sandbox 子进程环境中将 `AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS` 设为启用；不修改父测试进程、daemon、Bridge、默认配置或 C++。
- 涉及文件：`tests/e2e/conftest.py`。
- 验证结果：Local Sandbox 相关回归 18 passed；正式运行仍保持默认拒绝。

### CHG-20260829-113：Bridge 在 daemon 边界前脱敏 agent.execute

- 日期：2026-08-29
- 问题：Runtime DLP 位于 daemon 之后，用户输入可能在到达 Runtime 前先被 C++ 生命周期日志观察到。
- 风险分析：GitNexus 对 `DaemonClient.agent_execute` 返回 HIGH，7 个上游影响点，覆盖 REST、WebSocket、E2B 和直接调用；修改前已明确警告。
- 最小改动：
  - 在所有入口共用的 `agent_execute` 中复制并脱敏完整 params，再调用原 `send_request`。
  - 不原地修改调用者的 content/platform_context；脱敏结果不是 mapping 时 fail-closed。
  - 使用局部导入避免 Bridge client/services 的既有循环依赖；未修改 daemon/C++、RPC schema 或正常响应路径。
- 涉及文件：`frontend/bridge/client.py`、`frontend/bridge/tests/test_client_frame_reassembly.py`。
- 验证结果：
  - 17 个 client frame 回归通过。
  - 真实 hchat canary 在 daemon log、Bridge log、Skill stderr、SessionLog 中精确命中均为 0，安全占位事件存在。

### CHG-20260829-114：Bridge restart 精确管理真实 Uvicorn worker

- 日期：2026-08-29
- 问题：PID 文件可能指向已经退出的 wrapper；缺少 lsof 时端口检测恒为“空闲”，导致 restart 后仍由旧 worker 处理请求。
- 风险分析：GitNexus 对 shell helper 返回 UNKNOWN；文本检索确认其调用只在本脚本以及 `start_dev/start_production` 生命周期入口。
- 最小改动：
  - 优先用 `fuser` 取得实际端口 owner，并校验 cmdline 与本仓库 cwd/app-dir。
  - PID 文件和 `pgrep` 仅作回退，候选同样经过项目身份校验。
  - 端口释放检测改用 Bash `/dev/tcp`，不再依赖未安装的 lsof。
- 涉及文件：`scripts/start_bridge.sh`、`scripts/tests/test_start_bridge.py`。
- 验证结果：脚本语法与 3 个生命周期测试通过；真实 restart 停止旧 PID 后 8199 正常释放并由新 worker 接管。

### CHG-20260829-115：SQLite 用户输入脱敏且故障时不丢轮

- 日期：2026-08-29
- 问题：SessionLog 已安全不代表 Bridge 持久会话库安全；`dialogue_rounds.user_input` 仍会保存用户粘贴的原始 Key。首次 fail-closed 处理又会在脱敏器异常时跳过整个 dialogue。
- 风险分析：GitNexus 对具体 override 返回 UNKNOWN；对抽象接口返回 LOW（7 个上游影响点）。文本检索确认 REST、SSE、legacy chat 与测试通过 provider 动态分派到该实现。
- 最小改动：
  - 只在 SQLite sink 对不可信 `user_input` 做厂商无关脱敏。
  - 不整体改写 `agent_response/artifacts/images`，避免破坏正常 Artifact capability 和回放内容。
  - 脱敏器异常时写固定安全占位输入并继续保存该轮，不保存原文，也不丢失 assistant 结果。
- 涉及文件：`frontend/bridge/providers/local_session_provider.py`、`frontend/bridge/tests/test_local_session_provider.py`。
- 验证结果：
  - Provider 回归 30 passed。
  - 当前 Bridge 真实 hchat 的新 canary 在 SQLite 中精确命中为 0；占位符可检索。
  - 8 月 28 日 row 400/401/411 的 3 条命中是同一旧对话的历史数据；row 412～当前最新 row 673 没有新增配置凭据命中，按要求不清洗旧库。

### CHG-20260829-116：SessionSync 兼容 daemon 的 agent.list 包装对象

- 日期：2026-08-29
- 问题：daemon 实际返回 `{"agents":[...]}`，旧同步器把 dict 当数组迭代并对字符串键调用 `.get`，每 30 秒产生一次异常。
- 风险分析：GitNexus 对 `AgentflowSessionSync.sync` 返回 LOW，2 个上游影响点，属于 Bridge lifespan 后台同步流程。
- 最小改动：入口只做响应形状归一化，兼容旧 list 和正式 `agents` 包装对象；非对象项跳过，session 状态更新逻辑不变。
- 涉及文件：`frontend/bridge/services/session_sync.py`、`frontend/bridge/tests/test_session_sync.py`。
- 验证结果：9 个 SessionSync 回归通过；真实 Bridge 连续多个 30 秒周期存活且没有新的 `session_sync` 异常。

### CHG-20260829-117：为 Batch 终态增加统一 Workflow 状态

- 日期：2026-08-29
- 问题：`batch.result.status` 对单分支保存 `true/false`，对多分支保存 `partial`；虽然都是字符串，但不能直接映射到清晰的 Protobuf Workflow 枚举。
- 风险分析：
  - GitNexus 对动态加载的 SessionLog `handle` 返回 UNKNOWN；文本检索确认它由 Hook loader 按 `HOOK_EVENTS` 注册，并由 Runtime Engine 调用，三份正式 Hook 与 20 余个测试直接覆盖。
  - 改动只增加新字段，保留原始 `status`，不改变 C++、Batch 聚合、Coordinator 决策或旧消费者。
- 最小改动：
  - `batch.result.status` 继续保存小写 raw 值 `true/false/partial`。
  - 同一事件新增 `workflow_status`，稳定映射为 `completed/failed/partial`；未知值保持原值或 `unknown`，不伪造成功。
  - `results[].skill_status` 继续表达业务终态，明确与 Workflow 是否闭合分离。
  - 权威 Hook、示例 Agent Hook 和资源包 Hook 同步修改。
- 涉及文件：
  - `docs/sessionlog/sessionlog_hook.py`
  - `examples/agent/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `resource/agent-packages/outfit-assistant/harness/hooks/sessionlog_hook.py`
  - `docs/sessionlog/README.md`
  - `docs/sessionlog/tests/test_hook.py`
- 验证结果：
  - SessionLog Hook/Reader 54 passed，挂载副本逐字一致。
  - 实际 hchat 的后续 `batch.result` 同时保存 `status=false` 与 `workflow_status=failed`，对应 Skill 业务终态仍为 `llm_error`，证明两个维度没有互相覆盖。

### CHG-20260829-118：建树前最终真实验收

- 日期：2026-08-29
- 性质：验收与文档收口；本条不修改运行时代码或 C++。
- 真实轨迹结果：
  - 9 份多用户/多 Conversation 正式基线加 1 份最终长任务，共 10 个 dialogue 文件。
  - Reader `require_header=true` 全部通过：0 个序列、父事件、Dispatch、LLM、Tool、Skill、Batch 或 Dialogue 配对问题，0 个目录/Header 身份不一致。
  - 合计 40/40 个 LLM request/response、9/9 个 Tool call/result、12/12 个 Skill start/end、10/10 个 Dialogue start/end、10 个 client delivery；248 个 event_id 全部唯一。
  - 40 个 `llm.request.data.skill_name` 全部非空；所有 Skill start/end 都有 `exec_skill_key`。
  - 最终 hchat 在 4 次 Skill `llm_error` 后由 Loop 控制收敛，耗时约 737 秒、低于 Bridge 900 秒上限，最终保存 `dialogue.end=llm_error` 与 `client.delivery=final`，没有悬挂节点。
  - 该长任务横跨本地 Hook 热同步，仅后两个新 `batch.result` 含 `workflow_status=failed`；7 个较早事件保留旧 raw schema，按要求不重写历史日志。全新 dialogue 将从首个 Batch 起携带 canonical 字段。
- 测试结果：
  - 本轮直接受影响回归：Bridge 126 passed、Runtime 安全 56 passed、生命周期/扫描脚本 15 passed、SessionLog 54 passed。
  - 此前同一工作树完整回归：Runtime 非集成 514 passed/4 skipped、Outfit Harness 177 passed、C++ CTest 22/22。
  - 扩大到整个 Bridge 目录时有 554 passed/8 skipped/9 deselected，同时暴露 14 个既有外围失败和 3 个既有收集错误（旧 outfit_vision、package tar、测试漏 import、Python 3.12 event loop、sidecar 演示 fixture、CoreMind/独立 E2E 导入）；它们不在 Harness 执行/轨迹主链，本次未扩大修改。
- 隐私结果：
  - 凭据门禁：tracked 1713 文件、untracked non-ignored 59 文件、10 份实际轨迹均 0 findings。
  - SQLite 仅 row 400/401/411 含 8 月 28 日同一旧对话的历史配置凭据；之后至检查时最新 row 673 无新增命中，按要求不清洗旧数据。
- 外部边界：
  - 文本 Provider 仍可能出现约 120 秒 ReadTimeout；视频 Agent Plan 仍可能返回 `UnsupportedModel`。这些是外部服务/套餐模型能力问题，当前 Harness 会把它们保存为业务失败并闭合 Workflow。
  - 本地 `hchat` 默认 240 秒小于 Bridge 的 900 秒单轮上限；长任务测试应显式使用 `hchat --timeout 960`，否则 CLI 先退出但服务端仍会按正式上限完成或取消。

### CHG-20260829-119：重复脱敏保持嵌套 JSON 转义完整

- 日期：2026-08-29
- 问题：SessionLog 先递归脱敏 payload，再对序列化 JSONL 做最终 sink 脱敏；已脱敏的签名 URL 位于嵌套 messages JSON 时，第二次赋值匹配会把转义引号前的反斜杠吞入 value，替换后留下裸引号。
- 影响：首轮含 Artifact capability 的多轮会话会在后续 `dialogue.start` 第 2 行产生非法 JSON；Reader 必须拒绝该文件，轨迹树从第二轮开始断裂。
- 风险分析：GitNexus 对 `_redact_text` 返回 CRITICAL，涉及 42 个符号、5 条流程；改动前已告知风险，最终只收紧匹配替换的转义保持和 placeholder 幂等行为，不改变公开 API 或脱敏标签。
- 最小改动：
  - suffix 前若 value 以奇数个反斜杠结束，保留一个 JSON escape 反斜杠后再插入 `<redacted:...>`。
  - 已是完整 `<redacted:...>` placeholder 的 value 原样返回，避免重复报告和二次改写。
  - 增加 Runtime 重复脱敏测试与 SessionLog 嵌套签名 URL JSONL 回归。
- 涉及文件：
  - `packages/agentflow-runtime/src/agentflow_runtime/security/secrets.py`
  - `packages/agentflow-runtime/tests/test_secret_security.py`
  - `docs/sessionlog/tests/test_hook.py`
- 验证结果：全新 10 个 dialogue、397 个事件均可直接解析；Reader `require_header=true` 为 0 issues，旧故障位置不再复现。

### CHG-20260829-120：Batch Workflow 终态与 Skill 业务终态彻底分离

- 日期：2026-08-29
- 问题：CHG-117 首次新增 `workflow_status` 时仍从 raw `batch.result.status` 映射；raw 值实际是 `skill_success` 的业务聚合，因此 `tool_error/llm_error` 会被错误写成 Workflow failed。
- 最小改动：
  - 保留 raw `status=true/false/partial` 兼容旧消费者。
  - 仅当全部 `results[].workflow_success` 都是布尔值时聚合 `completed/partial/failed`；缺失或非法时写 `unknown`，不伪造成功。
  - 不改 Coordinator 决策和 C++ 协议；重建已有 `runtime.cpp` 变更后的旧二进制，使 `tool_error` 正确得到 `skill_success=false`。
  - 权威 Hook、正式示例 Hook、资源包 Hook 和 README 同步。
- 验证结果：
  - 11 个真实 Batch 均满足计算不变量，0 个业务状态或 Workflow 状态错配。
  - `completed` Skill 为 `skill_success=true`；3 个 `tool_error` 与 6 个 `llm_error` 均为 `skill_success=false`，但 15 个分支的 `workflow_success` 全为 true；11 个 Batch 的 `workflow_status` 均为 `completed`。
  - SessionLog/Runtime 回归包含本条在内共 537 passed。

### CHG-20260829-121：C++ 测试显式包含算法标准头

- 日期：2026-08-29
- 性质：只修复测试编译，不修改 C++ 业务底座。
- 问题：两个测试翻译单元使用 `std::find/std::count`，但依赖其他头文件间接包含 `<algorithm>`；当前 GCC 15 全目标构建时编译失败。
- 风险分析：GitNexus 对 GTest 宏/测试辅助函数返回 UNKNOWN；文本确认引用全部局限于对应测试文件，不进入生产执行流。
- 最小改动：分别在 `tests/unit/src/test_agent_gtest.cpp` 和 `tests/unit/src/test_workflow_json_gtest.cpp` 增加一行 `#include <algorithm>`。
- 验证结果：全部 C++ 目标构建成功；为避免污染正式轨迹而排除另起 chat 的 E2E 后，17/17 个 C++ 单元/功能测试通过。

### CHG-20260829-122：清空旧数据后的多用户长流程最终验收

- 日期：2026-08-29
- 性质：验收与数据基线重建；不增加运行时代码。
- 数据边界：按用户授权无备份清空旧 SessionLog、Python spans、运行日志、测试产物和 `data/sessions.db` 三张表，再从 0 开始运行；本条不记录本地测试副本配置改动。
- 场景：2 个用户、3 个 Conversation、10 个顺序 dialogue，三条 Conversation 链并发；覆盖图片、视频、公开网络、无效 URL、受保护文件、Shell 安全拒绝、业务失败、长历史和 fake canary。
- 运行结果：10/10 HTTP 200，总墙钟 1434.9 秒；最慢一轮 1159.9 秒，长调用正常闭合且未阻塞另外两条会话。
- 轨迹结果：
  - SQLite 精确为 3 sessions、10 dialogue_rounds、55 LLM traces；会话轮数为 4/3/3。
  - SessionLog 共 397 个事件：55 组 LLM request/transport/response、19 组 Tool call/result、15 组 Skill start/end、11 个 dispatch/batch、10 组 Dialogue start/end 与 10 个 client.delivery。
  - 387 个事件 ID 全部唯一；100 个 Python span 全部唯一，0 orphan parent、0 逆序时间；34 个 Skill LLM span均可经父 SKILL span归因。
  - 6 个图片 Artifact 均有独立 `artifact.persist=uploaded` 与哈希；5 个位于 assets，第 6 个由 Skill 写到 agent 根目录并同时存在于 Artifact Store，没有丢失。
- 隐私结果：扫描 152 个文本与二进制输出；2 个真实配置凭据及 4 种实际传输表示精确命中 0，provider-neutral findings 0，三枚有效形态 fake canary 原文命中 0。
- 回归结果：Runtime 非集成与 SessionLog 共 537 passed；C++ 非 E2E 17/17 passed。
- 外部边界：文本 Provider 仍有 ReadTimeout，视频套餐模型仍返回 `UnsupportedModel`；两者均被记录为业务失败并保持 Workflow 正常闭合，不属于轨迹结构故障。
