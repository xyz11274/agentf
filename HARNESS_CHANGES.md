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

### CHG-20260901-123：落地首个 C++ 轨迹自进化分析单元

- 日期：2026-09-01
- 目标：在不修改 Harness 在线执行链和 AgentFlow C++ 底座的前提下，将单个
  Dialogue 的 SessionLog 转换为可评测 Loop Episode，识别高置信度无效 Loop，
  并产出带证据约束的元反思请求和待验证候选记忆。
- 诱发原因：
  - SessionLog 已能完整记录 Coordinator、Skill、Tool、Batch 与评测事件，但原始
    JSONL 还不能直接回答“哪一次重试无效、依据是什么、可形成什么候选策略”。
  - 直接让模型读取整份原始轨迹会混入 Prompt、Tool Payload 和用户数据，并且缺少
    稳定 Episode、规则版本和证据边界，难以复现与审计。
  - 非重试错误若跨成功阶段累计，或仅凭任意位置出现 `tool_call_id` 就认定结果已被
    消费，会分别造成无效 Loop 假阳性和轨迹缺失假阴性。
- 最小改动：
  - 新增独立 C++17 子项目 `tools/trajectory_loop_analyzer`，不挂入顶层 CMake，
    不改 Runtime、Coordinator、Skill Executor、SessionLog Hook 或 C++ 调度底座。
  - 有界读取单 Dialogue JSONL；按 canonical JSON 和 `idempotency_key` 去重；检查
    producer identity/sequence、timestamp、Dialogue/Conversation/User 身份、LLM
    配对、Tool 配对、重复结果和孤立评测；去重状态只保留 canonical SHA-256。
  - 按 producer 的 LLM request 边界物化 Coordinator/Skill Episode；以
    `exec_skill_key` 隔离并发 Skill，并用 `batch_id`、`tool_call_id` 做强键关联。
  - 将 `skill.end` 的已知业务失败终态投影到 `outcome_failed`，同时原样保留 Batch
    `workflow_status`，不把“生命周期正常闭合但业务失败”误写成 Workflow 失败。
  - 增加 exact repeat、无状态进展、连续非重试错误、Tool 结果未消费、重复 Dispatch、
    A-B-A 振荡和 max-round 复核规则；同一 Tool 成功或连续模式中断会清零错误 streak。
  - 输出结构化诊断、元反思请求和确定性候选记忆；候选只允许
    `status=candidate` 且 `requires_validation=true`，不会自动注入在线 Prompt。
    error 级完整性问题会隔离整份轨迹，不生成元反思请求或候选记忆。
  - 报告不复制原始 Prompt、messages、Tool 参数/结果和评测 details；删除未使用的
    raw-line 内存副本，稳定指纹使用 SHA-256，并显式保存 analyzer、rule set、
    evaluator 和 candidate generator 版本。
  - CLI 支持 compact、strict 和文件输出；阻止输出路径覆盖输入 SessionLog。
- 涉及文件：
  - `tools/trajectory_loop_analyzer/CMakeLists.txt`
  - `tools/trajectory_loop_analyzer/README.md`
  - `tools/trajectory_loop_analyzer/include/agentflow/trajectory/loop_episode_analyzer.h`
  - `tools/trajectory_loop_analyzer/src/loop_episode_analyzer.cpp`
  - `tools/trajectory_loop_analyzer/src/main.cpp`
  - `tools/trajectory_loop_analyzer/tests/test_loop_episode_analyzer.cpp`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 12 类确定性 C++ 契约测试通过，覆盖非重试 Loop、瞬态重试、结果未消费、重复
    Dispatch、缺失 Tool Result、语义重复/冲突、头部身份冲突、必需事件元数据、
    torn tail、成功清零、评测去重回挂、损坏轨迹隔离和 Payload 不复制。
  - 同一测试集在 AddressSanitizer 与 UndefinedBehaviorSanitizer 下通过。
  - 输入/输出同路径测试返回 3，临时 SessionLog 的 SHA-256 前后不变。
  - 对真实 60-event SessionLog 严格分析通过：物化 9 个 Episode（Coordinator 2、
    Skill 7），准确标记 video-gen 第 3、4 次连续 `unsupported_model` 为 2 个无效
    Episode，并聚合为 1 个、支持证据数为 2 的 candidate memory。
  - 同一输入连续两次输出逐字节一致；检查 21 份原始 Prompt/messages/Tool Payload，
    报告精确复制命中为 0。
- 遗留事项：
  - 本单元只生成单 Dialogue 的版本化派生 JSON，不承担跨轨迹 Experience Store、
    Protobuf Candidate 持久化、回放评测、晋升、灰度和回滚；这些属于后续落地单元。
  - 错误分类仍是显式版本化启发式规则；Provider 语义变化时应新增规则版本并使用
    固定基准集重新验证，不能原地改变历史标签。

### CHG-20260901-124：轨迹进化自然语言模板统一为中文

- 日期：2026-09-01
- 问题：项目 Coordinator、Skill 与运行时 Prompt 以中文为主，但首个轨迹分析单元新增的诊断、元反思问题和候选记忆模板使用英文；若直接进入后续元反思或策略增强，会造成中英混杂、Prompt 风格漂移及记忆聚类噪声。
- 风险分析：GitNexus 因该独立工具仍是未跟踪新文件而返回 UNKNOWN；文本补充确认相关模板函数、版本字段及测试只存在于 `tools/trajectory_loop_analyzer`，不被 Harness 在线流程或 AgentFlow C++ 底座调用。
- 最小改动：
  - 仅将 `diagnostics`、`reflection_requests` 和 `candidate_memories` 中面向人或 LLM 的自然语言模板改为简体中文。
  - 字段名、状态、错误类别、规则枚举、阈值、`strategy_key`、指纹材料和 ID 生成逻辑保持不变。
  - `analyzer_version` 更新为 `loop-episode-analyzer-v1.1`，候选模板版本更新为 `candidate-memory-template-zh-v1`；规则语义未变，`rule_set_version` 继续为 `invalid-loop-rules-v1`。
  - 增加中文诊断、元反思问题、候选策略和模板版本断言，并同步 README。
- 涉及文件：
  - `tools/trajectory_loop_analyzer/include/agentflow/trajectory/loop_episode_analyzer.h`
  - `tools/trajectory_loop_analyzer/src/loop_episode_analyzer.cpp`
  - `tools/trajectory_loop_analyzer/tests/test_loop_episode_analyzer.cpp`
  - `tools/trajectory_loop_analyzer/README.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 独立 C++ 工具重新构建成功，12 类内部契约测试全部通过。
  - 10 份 SessionLog 分析全部重新生成；剔除自然语言字段和版本字段后，与旧报告逐字段比较为 0 个结构差异。
  - 检查 59 个诊断、反思和候选自然语言值，全部包含中文；4 组旧英文模板残留为 0。
  - 修改后的源码、文档及分析产物共 18 个文件通过厂商无关凭据扫描，0 findings。

### CHG-20260901-125：落地确定性优先、模型可插拔的 C++ Hybrid Evaluator

- 日期：2026-09-01
- 目标：在不修改 Harness 在线执行链和 AgentFlow C++ 底座的前提下，将
  `trajectory_loop_analyzer` 的去敏 Episode 报告转换为可复现的确定性指标、
  可选模型语义判断和不可越过硬规则的融合结论，为后续轨迹清洗、元反思和进化
  记忆筛选提供稳定 Protobuf 数据。
- 风险分析：
  - GitNexus 对既有 `LoopEpisodeAnalyzer` 返回 LOW，仅 2 个直接上游；本次不修改
    该符号或 Analyzer 输入/输出。
  - 参考的 Python `OpenAiCompatibleClient` 返回 LOW lower-bound、11 个上游；
    本次没有复用或修改在线 Client，而是在独立离线工具中实现受限 Adapter。
  - 新工具在首次分析时尚未进入索引，内部辅助符号返回 UNKNOWN；文本检索确认其
    定义和调用均局限于 `tools/trajectory_hybrid_evaluator`，不被 Runtime、
    Coordinator、Skill Executor 或 C++ 调度底座引用。
- 最小改动：
  - 新增独立 C++17 子项目 `tools/trajectory_hybrid_evaluator`，不挂入顶层 CMake，
    不修改 Harness、SessionLog Hook、Runtime、Coordinator 或 Skill Executor。
  - 只读取 `loop_episode_analysis` JSON，不读取原始 SessionLog；证据包只含
    Episode/Skill/Tool/Dispatch 状态、稳定指纹、诊断、Evaluation 和 Event ID，
    不复制原始 Prompt、Messages、Tool 参数、Tool 结果或源文件路径。
  - 新增专用 `hybrid_evaluation.proto`，保存来源身份、证据包、规则判断、模型
    判断、融合结论、确定性指标和问题；JSON 与二进制 Protobuf 均可输出。
  - 确定性层重新计算 Episode、Continue/Terminal、Tool 失败、不可重试失败、
    Dispatch、无效 Loop 下界及 Workflow/业务状态，不把 Coordinator 正常结束
    自动当成业务成功，也不把“未命中异常”自动标成 `USEFUL`。
  - Evaluation 以 `event_id` 在报告级去重，但保留其对多个 Episode 的挂载；
    同 ID 冲突载荷直接拒绝。不同 Metric 不跨量纲求平均；明确失败只进入
    `UNCERTAIN` 语义复核，不直接判为无效 Loop。
  - 提供三种模型模式：默认 `none`（绝不联网）、按 `packet_id` 回放的 JSONL
    Adapter、显式启用的 OpenAI-compatible HTTP Adapter；支持 `none`、
    `suspicious`、`all` 三种选择范围。
  - 硬规则 `INVALID` 不允许被模型覆盖；一致时标为 `HYBRID`，冲突时保留规则并
    显式记录；规则 `UNCERTAIN/UNASSESSED` 才允许合法模型输出补充语义。
  - 模型输出必须是限长 JSON、白名单 Verdict、`[0,1]` 置信度、中文理由，并至少
    引用一个证据包内 Event ID；未知证据、非法类型、非中文、直接或 Unicode
    转义后的凭据样式均拒绝，原始模型响应不落盘，只保存稳定错误码和指纹。
  - HTTP Key 只能从显式环境变量读取且只进入 Authorization Header；Endpoint
    禁止 userinfo/query/fragment，生产地址强制 HTTPS，HTTP 只允许 Loopback；
    禁止重定向、校验 TLS、默认禁用代理，并限制请求/响应、超时、重试、Token 和
    模型调用总数。Adapter 异常隐藏详情并按 Episode 降级。
  - Trace 完整性失败默认拒绝；显式隔离后仍保存确定性结果，但不调用模型、不生成
    可导出的模型请求。输出路径不能覆盖输入、模型响应或其他输出的同路径/硬链接。
- 涉及文件：
  - `tools/trajectory_hybrid_evaluator/CMakeLists.txt`
  - `tools/trajectory_hybrid_evaluator/README.md`
  - `tools/trajectory_hybrid_evaluator/proto/hybrid_evaluation.proto`
  - `tools/trajectory_hybrid_evaluator/include/agentflow/trajectory/hybrid_evaluator.h`
  - `tools/trajectory_hybrid_evaluator/src/hybrid_evaluator.cpp`
  - `tools/trajectory_hybrid_evaluator/src/model_adapter.cpp`
  - `tools/trajectory_hybrid_evaluator/src/main.cpp`
  - `tools/trajectory_hybrid_evaluator/tests/test_hybrid_evaluator.cpp`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 14 类 C++ 契约测试通过；全新 GCC 15 `-Wall -Wextra -Wpedantic -Werror`
    构建通过；同一测试集在 AddressSanitizer 与 UndefinedBehaviorSanitizer 下通过。
  - 4 个并行进程处理现有 10 份真实分析报告成功：55 个 Episode、19 次 Tool、
    12 次失败 Tool、5 个确定性无效 Episode、22 个语义复核 Episode；全部 Trace
    完整，Episode 指标与 Assessment 数量一致。
  - 同一真实输入连续两次紧凑 JSON 逐字节一致，并成功输出二进制 Protobuf。
  - 可疑模式生成 6 条安全请求；倒序 JSONL 响应按 `packet_id` 回放 6/6 成功；
    Loopback HTTP Adapter 6/6 成功。不可达 Provider 的 6 个模型分支均在 2 次
    有限尝试后降级，2 个硬规则 `INVALID` 结论保持不变。
  - 最终真实 JSON、Protobuf 周边 JSON、模型请求/回放文件和 Mock 日志的常见
    凭据模式扫描均为 0 命中。
- 当前边界：
  - 现有 10 份真实基线尚无 `evaluation.result`，因此真实批处理的
    `evaluation_count=0`；Protobuf、解析、去重、冲突和失败复核语义已由合成契约
    测试覆盖，待采集端产生真实评测事件后可直接进入同一管线。
  - 为避免重新引入用户数据，模型不读取原始任务 Prompt；当前 Verdict 判断步骤
    进展/恢复必要性，最终任务完成度应优先由已挂载 Evaluation 提供。未来若需要
    更强目标对齐，应新增经过隐私审查的 Rubric/Summary，而不是回填原始 Prompt。

### CHG-20260901-126：SessionLog 直连的内容感知对话级评测

- 日期：2026-09-01
- 目标：将原来的 `SessionLog -> analysis.json -> 结构模型评测` 简化为
  `SessionLog -> 规则评测/内容模型评测/反思`，让模型看到完成任务判断所需的
  具体内容，同时保持确定性规则、可回放证据、Protobuf 和隐私边界。
- 风险分析：
  - GitNexus 对 `HybridTrajectoryEvaluator` 返回 LOW，直接影响仅限该独立工具的
    3 个文件；本次不接入 Harness 或 C++ Runtime 在线执行流。
  - `LoopEpisodeAnalyzer::analyze_file` 因独立工具和直接调用未被图索引解析而返回
    UNKNOWN；文本检索确认调用方仅为 Analyzer CLI、测试和本次新增直连入口。本次
    不修改 Analyzer 实现或结构契约，只新增内存调用方。
  - `ModelAdapter` 返回 MEDIUM、5 个直接依赖；为避免扩大影响，接口保持不变，
    对话级评测继续复用现有 JSONL 与 OpenAI-compatible Adapter。
  - 索引刷新后，新增但尚未被 Git 跟踪的 `evaluate_sessionlog` 和
    `make_dialogue_request` 仍返回 UNKNOWN；按规则用文本检索补查，调用只存在于
    独立 CLI、该工具测试和同文件内部，顶层 CMake、Runtime、Harness、Frontend
    与在线 Agent 均无调用。
  - 未出现 HIGH 或 CRITICAL 风险。
- 最小改动：
  - `trajectory_hybrid_evaluator` 根据输入扩展名分流：`.jsonl` 进入内容感知直连，
    `.analysis.json` 保留旧兼容路径；没有修改 Runtime、Coordinator、Skill
    Executor、SessionLog Hook 或 AgentFlow C++ 调度底座。
  - 直连入口在内存中调用 `LoopEpisodeAnalyzer`，再通过新增 `evaluate_json()`
    复用原有确定性评测，不创建、读取或要求用户管理中间 `analysis.json`。
  - 一个 Dialogue 只构造一次模型请求。`suspicious` 仍评测任务结果和整体反思，
    但逐 Episode 判断只包含规则筛出的可疑步骤；`all` 判断全部 Episode；`none`
    只运行规则且不构造具体内容请求。
  - 模型输入同时包含所有 Episode 的结构证据/规则判断，以及去重后的具体
    `dialogue.start/end`、LLM request/response、Skill dispatch/start/end、
    Batch、Tool call/result、Artifact 和 Evaluation 内容；跳过 transport request、
    messages snapshot、Provider 原始响应和 client delivery 等重复副本。
  - 内容在外发前递归脱敏和限长：精确替换常见 Secret 环境变量及
    `AF_SECRET_ENV_NAMES` 指定变量；CLI 将 `--api-key-env` 实际读取值直接注册为
    瞬时脱敏值，因此不依赖供应商 Key 的变量命名。另处理常见 Key/Token/JWT/
    私钥、敏感字段和本地路径，限制单字符串、容器项数、递归深度和总请求大小。
  - Protobuf 增加 Dialogue 任务判断、进化反思、内容事件/脱敏计数和模型范围元数据；
    最终报告仍只保存结构证据、已校验判断和指纹，不保存具体 Prompt、Messages、
    Tool 参数或 Tool 结果。`--requests-out` 是显式的脱敏内容调试产物。
  - 对话级模型响应必须同时给出任务状态、全部选中 Episode 的唯一判断和反思；
    Event/Episode 引用必须属于本次证据，文本和置信度有界且中文理由必填。任一部分
    非法会原子拒绝整次模型层并保留规则结果；确定性 `INVALID` 仍不可被模型覆盖。
  - Trace 完整性失败时不解析具体内容、不构造请求、不调用 Adapter；模型传输或
    输出失败只作为离线模型层降级，不影响工作流或确定性评测完成状态。
  - SessionLog 直连默认模型输出预算提高到 4096 Token；CLI 和 README 改为以
    `.jsonl` 一条命令为主，旧结构化逐 Episode 模式继续兼容。
- 涉及文件：
  - `tools/trajectory_hybrid_evaluator/.gitignore`
  - `tools/trajectory_hybrid_evaluator/CMakeLists.txt`
  - `tools/trajectory_hybrid_evaluator/README.md`
  - `tools/trajectory_hybrid_evaluator/proto/hybrid_evaluation.proto`
  - `tools/trajectory_hybrid_evaluator/include/agentflow/trajectory/hybrid_evaluator.h`
  - `tools/trajectory_hybrid_evaluator/src/hybrid_evaluator.cpp`
  - `tools/trajectory_hybrid_evaluator/src/sessionlog_evaluator.cpp`
  - `tools/trajectory_hybrid_evaluator/src/main.cpp`
  - `tools/trajectory_hybrid_evaluator/tests/test_sessionlog_evaluator.cpp`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - GCC 15、`-Wall -Wextra -Wpedantic -Werror` 构建通过；旧 Hybrid Evaluator
    契约测试和新 SessionLog 直连测试目标全部通过。
  - 新测试覆盖多 Episode 单次调用、具体中文用户内容、Skill Prompt、Tool 参数/
    结果、任意供应商环境 Secret 精确脱敏、确定性硬规则不可覆盖、干净轨迹仍有
    Dialogue 任务评测、非法响应原子降级、损坏轨迹禁止模型和 Dialogue JSONL 回放。
  - 同一完整测试集在 AddressSanitizer 与 UndefinedBehaviorSanitizer 下通过。
  - 现有 10 份真实 SessionLog 直接规则评测 10/10 成功。最大样本物化 8 个
    Episode、44 个内容事件，单次脱敏请求 319184 字节，包含 3 个 Skill Prompt、
    LLM、Tool、Skill 结果和最终回复，低于 2 MiB 上限。
  - 对模型请求、Protobuf-JSON 和二进制 Protobuf 扫描 7 类当前本地真实 Key：
    精确值 0 命中，常见凭据模式 0 个文件命中。
- 未执行与边界：
  - 没有把现有真实 SessionLog 的具体内容发送到 `.env` 指向的外部 Provider；该
    数据外发需要针对具体日志和目标地址的明确授权，因此本次不宣称真实 Provider
    端到端成功。HTTP Adapter 的协议行为由既有测试覆盖，直连层由捕获 Adapter 和
    JSONL 回放覆盖。
  - 当前反思只是经过校验的候选输出，不会自动写入正式进化记忆或增强在线 Prompt；
    晋升、灰度、回滚和跨 Dialogue 聚合仍属于下一个落地单元。

### CHG-20260902-127：补充 Evaluation 字段与内容限长解读

- 日期：2026-09-02
- 目标：将当前 C++ Hybrid Evaluator 的 Protobuf 字段语义、规则/模型融合边界和
  SessionLog 内容限长机制沉淀为可独立查阅的文档，供后续 Evaluation 消费、轨迹
  组织和自进化模块设计使用。
- 改动性质：仅新增文档，不修改 Harness、SessionLog Hook、Loop Analyzer、Hybrid
  Evaluator、模型 Adapter 或 C++ Runtime 的任何运行逻辑。
- 主要内容：
  - 解释顶层版本/血缘字段、确定性 Metrics、Episode Evidence、Rule/Model
    Assessment、融合 Verdict、Dialogue Task Assessment、Reflection 和模型调用元数据；
  - 明确 `metrics.invalid_episode_count` 是确定性无效 Loop 下界，最终语义统计应读取
    `assessments[].final_verdict`；
  - 明确 Workflow 状态、结构化 Business 状态与模型 Task Outcome 相互独立；
  - 汇总 SessionLog 文件/单行、字符串、容器、递归深度、Prompt、HTTP Body、模型
    Token、响应字节和输出字段的全部限制及超限行为；
  - 记录 `suspicious` 主要减少模型输出而不一定减少输入，以及最终 Protobuf 尚未
    持久化 `truncation_count` 的可观测性边界。
- 涉及文件：
  - `docs/trajectory-evaluation/EVALUATION_FIELDS.md`
  - `docs/trajectory-evaluation/CONTENT_LIMITS.md`
  - `HARNESS_CHANGES.md`

### CHG-20260902-128：统一 C++ 轨迹进化管线并落地候选记忆库

- 日期：2026-09-02
- 目标：把 SessionLog 到进化记忆库之前的 Loop 分析、Hybrid Evaluation、候选
  生成、跨 Dialogue 元反思、质量门和持久化合并为一个独立 C++ 目录与统一命令，
  形成可回放、可追责、可幂等重放的离线进化前半闭环。
- 风险分析：
  - 刷新 GitNexus 索引后，HybridTrajectoryEvaluator、LoopEpisodeAnalyzer、
    finalize_batch_identity、config_fingerprint、run_directory 和路径冲突检查
    的已解析影响均为 LOW。
  - evaluate_directory、update_memory_file 等新增直接调用返回 UNKNOWN；文本检索
    确认调用仅位于统一 CLI、公共头文件和本目录测试，不被 Harness、Coordinator、
    Skill Executor、Runtime 或顶层 CMake 引用。
  - finalize_candidate 是候选归一化中央入口，图谱返回 HIGH：7 个上游节点、
    2 条执行流，覆盖候选合并、元反思和记忆合并。已在修改前显式报告风险；该处
    只把失效的 string + "\0" 改为真实字符分隔符，并以多用户碰撞用例和三套
    全量测试约束，没有改变状态、置信度或合并策略。
- 最小改动：
  - 将原有两个离线工具收敛到 tools/trajectory_evolution；Loop Analyzer、Hybrid
    Evaluator、模型 Adapter、候选管线、Protobuf 和测试共享一个 CMake 项目，
    统一二进制为 trajectory_evolution。
  - 保留单 Dialogue 的 .jsonl / 旧 analysis.json 兼容评测；目录模式递归发现
    SessionLog JSONL，逐文件隔离错误并直接输出每个 Dialogue 的 Evaluation，
    不再要求落盘中间 analysis.json。
  - 扩展 Protobuf 契约，新增候选目标/状态、支持与反证证据、批次运行摘要、合成
    指标、元反思结果、记忆更新结果、候选修订和 EvolutionMemoryStore。
  - 确定性候选只从可证明的无效诊断产生；模型候选只消费已原子校验成功的
    Dialogue Reflection。跨 Dialogue 聚合保留 User/Conversation/Dialogue、
    Episode、Diagnostic 和 Event ID。
  - 质量门区分 PROPOSED、INSUFFICIENT_SUPPORT、CONFLICTED 和
    REJECTED_BY_GATE；相同作用域/错误下的 USEFUL 或 NECESSARY_RETRY 会成为
    反证，任一候选都不会自动激活在线策略。
  - 可选元反思每批最多调用一次模型，只能合并目标与作用域完全一致的已有 Seed；
    Seed/Event 引用、中文有界文本、置信度和凭据严格校验，非法响应原子降级。
  - batch_id 覆盖 Evaluator 配置、每个成功评测的完整结果指纹、失败身份与脱敏
    错误、最终候选和元反思结果；修复模型结论变化时被误判为旧批次的问题。
  - 以二进制 Protobuf 作为权威记忆格式，JSON 作为可再生视图；同批次更新幂等，
    跨批次证据去重并保留实质变化快照。写入使用 POSIX flock、同目录临时文件和
    原子 rename，并发进程不会重复增加修订。
  - 单文件评测失败不终止其他 Dialogue；所有输入/输出做冲突检查，输出与模型回放
    不得位于 SessionLog 输入树内，weakly_canonical 同时阻止符号链接绕过。
  - 持久化前扫描显式 Secret 环境值与常见供应商凭据模式；不保存原始 Prompt、
    Messages、Tool Payload、模型原始响应或源文件路径。
  - 新 README 统一说明构建、命令、输出树、质量门、模型预算、并发/幂等语义、
    隐私边界和退出码。
- 涉及文件：
  - tools/trajectory_evolution/.gitignore
  - tools/trajectory_evolution/CMakeLists.txt
  - tools/trajectory_evolution/README.md
  - tools/trajectory_evolution/proto/hybrid_evaluation.proto
  - tools/trajectory_evolution/include/agentflow/trajectory/evolution_pipeline.h
  - tools/trajectory_evolution/include/agentflow/trajectory/hybrid_evaluator.h
  - tools/trajectory_evolution/include/agentflow/trajectory/loop_episode_analyzer.h
  - tools/trajectory_evolution/src/evolution_pipeline.cpp
  - tools/trajectory_evolution/src/hybrid_evaluator.cpp
  - tools/trajectory_evolution/src/loop_episode_analyzer.cpp
  - tools/trajectory_evolution/src/main.cpp
  - tools/trajectory_evolution/src/model_adapter.cpp
  - tools/trajectory_evolution/src/sessionlog_evaluator.cpp
  - tools/trajectory_evolution/tests/test_evolution_pipeline.cpp
  - tools/trajectory_evolution/tests/test_hybrid_evaluator.cpp
  - tools/trajectory_evolution/tests/test_loop_episode_analyzer.cpp
  - tools/trajectory_evolution/tests/test_sessionlog_evaluator.cpp
  - HARNESS_CHANGES.md
- 验证结果：
  - 常规 Debug、全目标 GCC 15 -Wall -Wextra -Wpedantic -Werror，以及
    AddressSanitizer/UndefinedBehaviorSanitizer 严格模式下，4/4 CTest 全部通过。
  - 新测试覆盖跨 Dialogue 门槛、反证、无效轨迹隔离、多用户身份碰撞、Dialogue/
    元反思结果参与批次身份、元反思合并、记忆插入/更新/幂等/修订/Protobuf 往返、
    JSON 视图恢复和单文件失败隔离。
  - 现有 10 份真实 SessionLog（2 用户、3 会话）完成 10/10：生成 20 个逐 Dialogue
    JSON/PB、1 份 Batch JSON/PB 和 1 份 Memory JSON/PB；得到 1 条 PROPOSED
    Retry Policy 候选，由 2 用户、3 会话、3 个 Dialogue、5 个无效 Episode
    支持且无反证。
  - 相同真实批次串行重放后 batch_id 不变、idempotent_replay=true、revision
    保持 1；两个进程并发写同一新记忆库也均成功，最终仅 1 个 applied batch。
  - 普通子目录和符号链接指向输入树时均在写入前以退出码 2 拒绝；真实 Evaluation、
    Batch、Memory JSON/PB 的常见凭据、本地路径和当前 Secret 环境值扫描均 0 命中。
- 当前边界：
  - 本次止于“候选进化记忆库”，没有修改 Harness/C++ 在线底座，也没有把候选提升
    为 VALIDATED、注入 Coordinator/Skill Prompt 或自动修改 Loop 条件。
  - 下一阶段应在隔离环境中生成策略变体并做回放/对照评测，再以独立晋升规则更新
    记忆状态；不能直接把离线反思当成线上策略。

### CHG-20260902-129：稳定 Evolving 真实评测与模型失败归因

- 日期：2026-09-02
- 目标：使用 `doubao-seed-evolving` 完成内容感知轨迹评测，并修复长时请求、深度
  思考配置、结构化输出约束和输出截断归因不清的问题。
- 风险分析：
  - GitNexus 对 `make_dialogue_request` 和 `extract_model_content` 的上游影响均为
    LOW，只到独立轨迹进化工具内部调用点。
  - `OpenAiCompatibleModelAdapter::evaluate` 的名称查询无法解析；改为分析其直接
    helper，并以文本检索确认适配器只由本工具 CLI、工厂和测试使用。
  - 测试函数返回 UNKNOWN，文本确认仅由对应 CTest 入口调用。没有改动 Harness、
    Coordinator、Skill Executor、SessionLog Hook 或 C++ Runtime。
- 最小改动：
  - 新增可选 `--thinking-mode enabled|disabled|auto`，仅在显式传入时向兼容接口发送
    `thinking.type`，不改变其他 Provider 的默认请求。
  - 保留原有 30 秒/5 秒超时默认值，移除固定 10 分钟/2 分钟配置上限；运行时超时
    只受 CLI 数值类型与 libcurl `long` 可表示范围约束，可设置为 30 分钟。
  - Dialogue Prompt 写入 selected episode 的精确数量；为 0 时明确要求返回 `[]`，
    并明确所有模型结论（包括 `status=unknown`）至少引用一个真实 event_id。
  - OpenAI 兼容响应读取 `finish_reason`；`length` 不再被误记为 `invalid_json`，而是
    记录为 `output_token_limit_reached`，保留响应指纹但不让截断内容进入评测。
  - 增加非法 thinking 模式、空选择数组契约和 unknown 结论证据契约的回归断言。
- 涉及文件：
  - `tools/trajectory_evolution/include/agentflow/trajectory/hybrid_evaluator.h`
  - `tools/trajectory_evolution/src/main.cpp`
  - `tools/trajectory_evolution/src/model_adapter.cpp`
  - `tools/trajectory_evolution/src/sessionlog_evaluator.cpp`
  - `tools/trajectory_evolution/tests/test_hybrid_evaluator.cpp`
  - `tools/trajectory_evolution/tests/test_sessionlog_evaluator.cpp`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - GCC 15 严格构建通过，4/4 CTest 全部通过。
  - 使用 Agent Plan `doubao-seed-evolving`、`thinking=disabled`、30 分钟单请求超时
    和 16384 输出 Token 上限，对 2 用户、3 会话、10 个真实 Dialogue 完成评测；
    11 条模型请求（含 1 次元反思）全部成功，无模型错误码或非法响应。
  - 最终产物包含 10 份 Evaluation JSON/PB、1 份 Batch JSON/PB 和 revision=1 的
    Memory JSON/PB；元反思由 47 条 Seed 收敛为 12 条候选记忆。
  - 对最终目录全部 JSON、JSONL 和 Protobuf 扫描常见 API Key、Bearer Token 与
    Secret 模式，命中文件数为 0；凭据只从现有 `.env` 注入进程，未写入命令归档。
  - 已删除兼容性探测、两次失败批次、诊断请求/响应和旧 GLM 批次，只保留
    `trace_output/evolution-model-full-evolving-final2-20260902/`。

### CHG-20260903-130：约束跨轨迹元反思的候选兼容边界

- 日期：2026-09-03
- 问题：`deepseek-v4-flash` 能完成全部逐 Dialogue 评测，但连续把不同
  `target_type` 或 `target_scope` 的 Seed 合并，元反思被严格校验器以
  `incompatible_seed_group` 拒绝。
- 风险分析：GitNexus 对 `make_meta_request` 的上游影响为 LOW，只涉及独立轨迹
  进化工具的 `synthesize_reports → evaluate_files → evaluate_directory` 链路。
- 最小改动：为每个 Seed 增加由 `target_type + target_scope` 确定生成的
  `compatibility_group`，同时向模型提供 `allowed_seed_groups` 硬性白名单；返回的
  每个合并组只能是某一白名单组的子集。原有服务端严格校验、候选门控、记忆合并
  和 Harness/C++ 底座均未改动。
- 涉及文件：
  - `tools/trajectory_evolution/src/evolution_pipeline.cpp`
  - `tools/trajectory_evolution/tests/test_evolution_pipeline.cpp`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 轨迹进化工具重新编译成功，4/4 CTest 全部通过。
  - 使用 `deepseek-v4-flash`、`thinking=disabled` 完成 2 用户、3 会话、10 个真实
    Dialogue 的完整评测；10 条 Dialogue 模型调用和 1 条跨轨迹元反思均成功。
  - 27 个输入 Seed 经元反思形成 9 个合并输出，最终得到 13 条候选并写入
    revision=1 的 Memory；生成 10 份 Evaluation JSON/PB、Batch JSON/PB 和
    Memory JSON/PB。
  - 脱敏请求日志共 11 条，最终目录未命中常见 `ark-...` 凭据模式；三个元反思
    失败的 DeepSeek 输出目录已删除，只保留
    `trace_output/evolution-model-full-deepseek-v4-flash-20260903-final2/`。

### CHG-20260903-131：补充 SessionLog 到进化记忆库的数据链路文档

- 日期：2026-09-03
- 目标：说明原始 SessionLog 如何经过 Event 校验、Loop Episode 重建、确定性规则、
  Dialogue 模型评测、规则/模型融合、跨 Dialogue 候选、元反思和质量门，最终幂等
  写入 Protobuf 进化记忆库。
- 说明：文档区分了进程内中间对象与长期落盘产物，补充并发分支归属、失败隔离、
  证据回链、Batch 身份、文件锁、原子写入和当前闭环边界；不改变程序行为。
- 涉及文件：
  - `docs/trajectory-evaluation/SESSIONLOG_TO_EVOLUTION_MEMORY.md`
  - `HARNESS_CHANGES.md`

### CHG-20260903-132：补充 Batch Evaluation 与 Memory 字段解读

- 日期：2026-09-03
- 目标：逐层解释 `HybridEvaluationBatchReport`、`EvolutionCandidate`、
  `CandidateEvidence`、`MetaReflectionAssessment`、`MemoryUpdateResult`、
  `EvolutionMemoryStore`、Entry 和 Revision 的字段与生命周期。
- 说明：重点区分文件级评测成功与业务成功、路径/内容/评测指纹、Seed 与 Candidate、
  Candidate 质量门状态与 Memory 生命周期状态、元反思输出数与最终候选数，以及 Batch
  幂等和 Store revision；不改变程序行为。
- 涉及文件：
  - `docs/trajectory-evaluation/BATCH_AND_MEMORY_FIELDS.md`
  - `docs/trajectory-evaluation/SESSIONLOG_TO_EVOLUTION_MEMORY.md`
  - `HARNESS_CHANGES.md`

### CHG-20260903-133：新增无向量数据库的进化记忆管理层

- 日期：2026-09-03
- 问题：原 Memory 仅按完全相同的 `candidate_id` 插入或合并，无法直接回答本批
  Candidate 最终对应哪个 Memory，也不能保留跨生成器的语义重复、相关/冲突关系、
  模型整理依据和完整来源映射。文本相近候选会持续膨胀，若直接用模糊规则合并又会
  污染后续策略实验的数据血缘。
- 风险分析：GitNexus 索引比当前 HEAD 落后且仍处于 incomplete 状态；对新增
  `curate_memory_file`、Memory 消息和测试函数的影响分析均返回 `UNKNOWN/not found`，
  没有可作为放行依据的调用图。按规范补充文本检索，确认改动只位于独立
  `tools/trajectory_evolution` 的 CLI、Protobuf、实现和测试，以及既有说明文档；未进入
  Harness、Coordinator、Skill Executor 或 Runtime 在线链路，未得到 HIGH/CRITICAL
  在线流程风险结论。
- 最小改动：
  - 不引入向量数据库或新依赖；在同类型、同规范化作用域内全量扫描，用字符 3-gram
    相似度只做最多 8 项短名单排序，任何相似度都不能自动合并。
  - 保留既有 `merge_memory` / `update_memory_file` 接口；新增可选
    `curate_memory_file` 和 `--memory-semantic-match`。默认行为不调用模型，模糊项以
    `UNRESOLVED` 独立入库。
  - 稳定 ID/历史来源别名直接复用 canonical Memory；规范化策略键完全相同按规则
    合并。可选模型只判断 `DUPLICATE`、`RELATED`、`CONTRADICTS`、`DISTINCT`；只有
    高置信重复才合并，相关和冲突只建立双向关系。模型失败、非法输出、越权引用、
    低置信度或预算耗尽均保守独立入库，不影响其他 Candidate。
  - Protobuf 以追加字段保持兼容：保存每个 Candidate 的映射决策、候选短名单、方法、
    置信度、理由、模型/请求/响应指纹和错误码；Entry 保存所有来源 Candidate ID 和
    Memory 关系；Revision 同时快照 Candidate、生命周期状态、来源映射和关系。
  - 模型调用不持有 Memory 文件锁；提交时在锁内比较基线，发现并发更新后有限次
    重算。PB 仍以临时文件加 rename 原子替换，JSON 仍是可再生视图。
  - 写入前校验 Candidate 指纹/稳定 ID、canonical Memory ID、来源唯一归属和关系
    目标；损坏 Candidate 在 revision 或 batch_id 变化前原子拒绝。同批次重放恢复
    Candidate→Memory 映射，不增加 revision、不再次调用模型，并可补齐旧 Store 的
    curator/source 元数据。
- 涉及文件：
  - `tools/trajectory_evolution/proto/hybrid_evaluation.proto`
  - `tools/trajectory_evolution/include/agentflow/trajectory/evolution_pipeline.h`
  - `tools/trajectory_evolution/src/evolution_pipeline.cpp`
  - `tools/trajectory_evolution/src/main.cpp`
  - `tools/trajectory_evolution/tests/test_evolution_pipeline.cpp`
  - `tools/trajectory_evolution/README.md`
  - `docs/trajectory-evaluation/BATCH_AND_MEMORY_FIELDS.md`
  - `docs/trajectory-evaluation/SESSIONLOG_TO_EVOLUTION_MEMORY.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 普通构建和 GCC 15 警告即错误构建均通过；完整 4/4 CTest 通过。
  - 回归覆盖首次插入、同批幂等、跨批证据合并、JSON 视图恢复、旧 Memory 元数据
    迁移、默认保守入库、模型重复合并、相关/冲突双向关系、低置信拒绝合并、非法模型
    输出降级、损坏 Candidate 原子拒绝和单文件失败隔离。
  - CLI 在未配置模型时显式使用 `--memory-semantic-match` 会在执行前以退出码 2 拒绝；
    默认模式不访问网络。
  - AddressSanitizer + UndefinedBehaviorSanitizer 构建的 4/4 CTest 通过，无内存错误或
    未定义行为报告。
  - 使用现有 4 个真实 Dialogue 做无网络 CLI 验收：4 个全部评测成功，1 个 Candidate
    写入 revision=1；相同输入重放得到同一 batch_id，`idempotent_replay=true`，
    revision 保持 1，并恢复同一 Candidate→Memory 映射。
- 边界：本次完成候选记忆的增量整理、稳定映射和关系管理，仍不实现实验验证、自动
  晋升/拒绝/归档或线上策略注入；这些必须由后续闭环在回放/A-B 证据通过后执行。

### CHG-20260904-134：记忆入库与管理自动化，并增加周期全库复核

- 日期：2026-09-04
- 问题：已有记忆整理需要调用方显式开启，且只处理当前 Batch，没有“新增一定数量后
  自动复核全库”的闭环。长期运行会留下跨批次重复项；冲突关系若被后续相关判断覆盖，
  还可能让未来策略选择器同时注入互斥策略。文件乐观并发重算也可能重复发送完全相同的
  模型请求。
- 风险分析：GitNexus 索引尚未包含本次未提交符号，对 `prefer_relation`、
  `run_full_store_curation`、`run_memory_match`、`curate_memory_in_place` 及新增测试的
  impact 均返回 `UNKNOWN/not found`，不能将 0 caller 当作低风险。补充文本调用链确认
  变更封闭在 `tools/trajectory_evolution` 的离线 CLI、库实现、Protobuf 和测试，以及
  对应说明文档；未修改 Harness、Coordinator、Skill Executor 或 Runtime 在线链路。
- 最小改动：
  - CLI 统一走 `curate_memory_file()`；只要配置模型适配器，兼容短名单的语义整理自动
    执行，不再需要 `--memory-semantic-match`。无模型或单项判断失败时保守独立入库，
    不阻塞其他 Candidate。
  - 每个新候选立即按稳定 ID/历史别名、规范化重复规则和最多 8 项同类型同作用域短名单
    整理。高置信 `DUPLICATE` 合并；`RELATED` / `CONTRADICTS` 保留双方并建立双向关系；
    `DISTINCT` 独立保存。
  - Store 持久化 `new_entries_since_full_curation`，只累计真正新建并保留的 Entry。默认每
    20 条自动在同一次调用内复核全库，可用 `--memory-full-curation-threshold N` 调整。
    全库先确定性合并规范化重复项，再逐条复核兼容短名单；没有可比较项时不要求模型。
  - 全库某项失败不会回滚增量入库或其他复核结果；`full_curation_completed=false` 并保留
    pending 计数，后续新 Batch 自动重试。预算不足在发送请求前整体延期，避免半批消耗。
  - 冲突关系采用安全优先级：一旦确认 `CONTRADICTS`，后续更高置信 `RELATED` 也不能
    降级覆盖。两条记忆均保留；当前层不伪造激活状态，未来策略选择器必须对冲突组至多
    激活一条。
  - 详细短名单、理由、模型身份、请求/响应指纹、错误和每项稳定 `decision_id` 保存在
    Batch；Memory 只保留当前 Candidate、来源别名、紧凑双向关系和 Candidate revision。
    被移除的试验字段号改为 `reserved`，防止旧 Protobuf 数据被误读为新字段。
  - 乐观并发重算在单次 `curate_memory_file()` 生命周期内按请求指纹复用原始模型响应；
    缓存不落盘、不跨批次，模型预算和计数只统计真实外部请求。提交仍使用文件锁、基线
    比较和原子 rename。
  - 所有 Candidate 身份和批次内重复 ID 在 revision、batch_id 或 Memory 内容变化前统一
    校验；全库合并保留被合并项的来源 Candidate ID，使旧 Batch 仍能幂等回放到 canonical
    `memory_id`。
- 涉及文件：
  - `tools/trajectory_evolution/proto/hybrid_evaluation.proto`
  - `tools/trajectory_evolution/include/agentflow/trajectory/evolution_pipeline.h`
  - `tools/trajectory_evolution/src/evolution_pipeline.cpp`
  - `tools/trajectory_evolution/src/main.cpp`
  - `tools/trajectory_evolution/tests/test_evolution_pipeline.cpp`
  - `tools/trajectory_evolution/README.md`
  - `docs/trajectory-evaluation/BATCH_AND_MEMORY_FIELDS.md`
  - `docs/trajectory-evaluation/SESSIONLOG_TO_EVOLUTION_MEMORY.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 普通构建与完整 4/4 CTest 通过。
  - GCC `-Werror` 独立构建与完整 4/4 CTest 通过，无新增编译警告。
  - AddressSanitizer + UndefinedBehaviorSanitizer 独立构建与完整 4/4 CTest 通过，无内存
    错误或未定义行为报告。
  - 回归覆盖自动增量整理、阈值全库复核、失败与预算隔离、无可比较项无需模型、冲突
    不降级、合并后别名幂等回放、批内重复 ID 原子拒绝，以及并发重算不重复请求模型。
  - CLI 帮助只暴露 `--memory-full-curation-threshold`，已移除旧手动语义整理开关。
  - 使用仓库现有 10 条真实 SessionLog 做无网络端到端验收：10/10 Evaluation 成功，
    生成 1 个 Candidate；阈值设为 1 时同次调用得到 `full_curation_triggered=true`、
    `full_curation_completed=true`。再次运行得到 `idempotent_replay=true`，revision 保持
    1，Candidate→Memory 映射不变。产物仅位于被忽略的 `build/` 验证目录。
  - GitNexus `detect-changes --scope all` 返回 low、0 affected process，但只识别到文档
    符号，没有覆盖本批新增 C++ 符号；因此不将该结果当作完整代码影响证明，仍以文本
    调用链、三类构建和回归测试作为代码边界依据。
- 边界：不引入向量数据库、新依赖、独立全库管理程序或在线策略注入；不自动晋升、
  拒绝、归档 Memory。策略选择和冲突组互斥激活属于下一落地单元。

### CHG-20260904-135：为进化记忆库增加确定性容量淘汰

- 日期：2026-09-04
- 问题：增量去重和每 20 条新增记忆触发的全库复核只能合并重复项，不能限制彼此独立
  的 Memory；`entries`、`applied_batch_ids` 和 Entry 内累计证据仍可随长期运行持续
  增长。原有 256 MiB 限制只在规划后报错，既没有选择性淘汰，也会使整个新 Batch
  无法提交。
- 风险分析：
  - Repository：`AgentFlow (/root/lai-code/Agent/AgentFlow)`，Worktree 同路径，分支
    `harness_agent_poc_evo`。GitNexus 已刷新到当前提交 `46a2daa`。
  - `run_full_store_curation` 上游影响为 LOW：1 个直接调用者、4 个受影响符号、2 条
    evolution 内部流程。`curate_memory_in_place` 上游影响为 LOW：2 个直接调用者、
    3 个受影响符号、2 条内部流程。
  - `curate_memory_file`、`EvolutionOptions` 和构造器方法因 C++ 方法解析得到
    UNKNOWN；按规则补充精确文本调用搜索，确认调用点仅在独立
    `tools/trajectory_evolution` CLI、测试和文档中。未修改 Harness、Coordinator、
    Skill Executor 或 Runtime 在线路径，未发现 HIGH/CRITICAL 风险。
- 最小改动：
  - `EvolutionOptions` 增加默认 512 条的 Memory 工作集上限和默认 4096 项的最近
    Batch 精确幂等窗口；CLI 对应
    `--memory-max-entries` / `--memory-replay-window`，两者都必须大于零。
  - 每批增量与可选全库复核完成后，在同一 Memory 事务内先裁剪最旧 Batch ID，再按
    生命周期、Candidate 状态、支持用户/Dialogue/Episode、置信度和最近修订确定性
    淘汰低优先级 Entry；`VALIDATED` 最后淘汰。
  - `CONTRADICTS` 连通组作为原子淘汰单元，不能只删一侧；淘汰后统一重建双向关系。
    若 Entry 数已满足但 Protobuf 仍超过字节限制，继续按相同顺序淘汰，最终保留硬
    字节上限。
  - 每个删除动作写入 `MemoryUpdateDecision(action=EVICTED)`，并增加
    `evicted_entry_count` 与 `pruned_applied_batch_id_count`。详细来源不复制到新的
    Archive Store，继续由既有 Batch Evaluation、Dialogue Evaluation 和 SessionLog
    保存，避免把无界增长转移到另一份 Memory。
  - 幂等重放不重新合并 Candidate、也不增加 revision，但允许执行纯容量迁移，使旧
    Store 在更严格配置下立即收敛。超过最近 Batch 窗口后不保证永久 Batch 级幂等；
    Candidate/别名仍在时稳定映射继续去重，两者均淘汰后旧批可重新进入工作集。
  - Memory Curator 版本提升为 `evolution-memory-curator-v2`；Store wire 结构和
    `store_version=v1` 保持兼容，不引入向量数据库、新依赖或独立管理程序。
- 涉及文件：
  - `tools/trajectory_evolution/proto/hybrid_evaluation.proto`
  - `tools/trajectory_evolution/include/agentflow/trajectory/evolution_pipeline.h`
  - `tools/trajectory_evolution/src/evolution_pipeline.cpp`
  - `tools/trajectory_evolution/src/main.cpp`
  - `tools/trajectory_evolution/tests/test_evolution_pipeline.cpp`
  - `tools/trajectory_evolution/README.md`
  - `docs/trajectory-evaluation/BATCH_AND_MEMORY_FIELDS.md`
  - `docs/trajectory-evaluation/SESSIONLOG_TO_EVOLUTION_MEMORY.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 普通构建、GCC `-Werror` 构建、ASan + UBSan 构建均成功，三套完整 4/4 CTest
    全部通过。
  - 新回归覆盖同优先级最旧淘汰、最近 Batch 窗口、淘汰审计、幂等容量迁移、
    `VALIDATED` 优先保留、冲突组原子淘汰和序列化字节硬上限。
  - 使用现有 10 条 SessionLog 做无网络 CLI 验收，10/10 Evaluation 成功、1 个
    Candidate 写入；设置 Entry/Batch 上限均为 1 后重放得到相同 `batch_id`、
    `idempotent_replay=true`、revision 保持 1，Memory 保持 1 条。
- 边界：当前淘汰依据是候选证据质量与修订新旧，不包含尚未实现的策略激活次数、
  在线收益或 A/B 实验结果；后续选择/验证层产生这些指标后，应将它们接入保留优先级。

### CHG-20260904-136：轨迹进化 Prompt 与运行参数外置化

- 日期：2026-09-04
- 问题：轨迹评测、Dialogue 反思、跨轨迹元反思和记忆整理的模型 Prompt 原本写在
  C++ 字符串中，Evaluation/Evolution/HTTP 默认参数也散落在头文件和 CLI；修改策略
  需要重新编译，无法安全地按实验替换，也无法明确记录一批运行使用了哪一版 Prompt。
- 风险分析：
  - GitNexus 索引已刷新到 21,505 个节点、40,313 条边和 240 条执行流。
  - `parse_arguments`、`ensure_distinct_outputs`、`ensure_dialogue_output_distinct`、
    `make_model_request`、`make_dialogue_request`、`make_meta_request`、
    `config_fingerprint` 和相关构造器的上游影响为 LOW。
  - `make_memory_match_request` / `run_memory_match` 位于增量入库、全库整理和文件锁
    路径，GitNexus 返回 HIGH，已在修改前告知；本次只替换其 Prompt 来源，不改变
    短名单、关系判定、失败隔离、模型预算、锁、并发重算或原子提交语义。
  - 新增加载函数尚未进入索引，impact 为 UNKNOWN；按规范用精确文本搜索确认调用点
    仅在 `tools/trajectory_evolution` 的 CLI、Evaluator、Pipeline、静态兼容访问器和
    新测试中。未修改 Harness、Coordinator、Skill Executor 或 Runtime 在线底座。
- 最小改动：
  - 新增版本化 `config/runtime.json`，集中承载 CLI 实际使用的 Evaluation、Evolution
    和模型 HTTP 参数；CLI 参数仍可逐项覆盖。配置优先从 `--config`、环境变量
    `AGENTFLOW_TRAJECTORY_CONFIG`、仓库默认路径依次发现。
  - 4 类模型请求的 system/user Prompt 共 8 份，全部迁移到 `config/prompts/`；C++
    只保留证据 JSON 构造、响应契约和安全校验，不再内嵌模型策略正文。
  - 启动时一次性读取并严格校验配置和 Prompt，Evaluator/Pipeline 共享同一个
    `shared_ptr<const PromptTemplates>` 快照。文件修改在下一次命令生效，运行中的
    批次不热切换，兼顾免编译替换与同批可复现性。
  - 模板只允许声明的占位符且必须恰好出现一次；替换后的 SessionLog/候选内容不会
    被二次解释为模板语法。配置和 Prompt 有独立大小上限，Prompt 路径必须相对配置
    目录且不能使用 `..`，未知字段和重复字段被拒绝。
  - API Key 值仍只从 `model.api_key_env` 指向的环境变量读取；配置 schema 不接受
    `api_key` 字段。输出冲突检查同时保护本次加载的 runtime config 和 8 个 Prompt。
  - 影响评测语义的规范化参数与全部 Prompt 内容指纹进入
    `evaluator_config_fingerprint`，因此 Prompt 改动可沿 Batch 身份追踪。
  - 协议枚举、严格模型响应校验、凭据检测、路径/请求绝对上限等安全不变量继续留在
    C++，不能通过热配置关闭。
- 涉及文件：
  - `tools/trajectory_evolution/config/runtime.json`
  - `tools/trajectory_evolution/config/prompts/*.txt`
  - `tools/trajectory_evolution/include/agentflow/trajectory/prompt_templates.h`
  - `tools/trajectory_evolution/include/agentflow/trajectory/runtime_config.h`
  - `tools/trajectory_evolution/include/agentflow/trajectory/hybrid_evaluator.h`
  - `tools/trajectory_evolution/include/agentflow/trajectory/evolution_pipeline.h`
  - `tools/trajectory_evolution/src/runtime_config.cpp`
  - `tools/trajectory_evolution/src/hybrid_evaluator.cpp`
  - `tools/trajectory_evolution/src/sessionlog_evaluator.cpp`
  - `tools/trajectory_evolution/src/evolution_pipeline.cpp`
  - `tools/trajectory_evolution/src/main.cpp`
  - `tools/trajectory_evolution/tests/test_runtime_config.cpp`
  - `tools/trajectory_evolution/CMakeLists.txt`
  - `tools/trajectory_evolution/README.md`
  - `docs/trajectory-evaluation/SESSIONLOG_TO_EVOLUTION_MEMORY.md`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 普通构建和 GCC `-Werror` 构建均成功；两套完整 5/5 CTest 全部通过。
  - ASan + UBSan 构建的完整 5/5 CTest 通过，无内存错误或未定义行为报告。
  - 新测试覆盖默认快照、无需重编译的下一次加载生效、运行中快照不突变、证据内
    `{{...}}` 不被二次解析、缺失占位符拒绝和明文 `api_key` 字段拒绝。
  - 使用现有真实 SessionLog，通过显式 `--config` 完成离线 CLI 评测，轨迹完整性
    有效且未请求模型；产物仅写入被忽略的 `build/` 验证目录。
- 边界：这是进程/批次边界的热替换，不监视运行中字段变化。若未来把该 CLI 改为
  常驻服务，应在新 Batch 边界原子交换已完整校验的新快照，仍不能在 Batch 中途重载。

### CHG-20260904-137：Prompt 进化目标替换为经验注入

- 日期：2026-09-04
- 问题：Dialogue Reflection 和 Evolution Candidate 把与 Skill 编排、Loop 收敛并列的
  第三类进化目标定义为 Prompt 修改，不符合后续通过进化记忆检索并注入经验的方案。
- 风险分析：
  - GitNexus 对 `EvolutionReflection`、`CandidateTargetType`、
    `validate_dialogue_response` 和 `TrajectoryEvolutionPipeline::synthesize_reports`
    均返回 `UNKNOWN`；精确引用检查确认活动调用集中在 Dialogue 响应校验、候选生成、
    Protobuf 契约和两组回归测试。
  - 这是公开 Protobuf/JSON 名称变更：保留字段号 4 和枚举数值 2，旧二进制 Protobuf
    仍可解析；旧 JSON 字段 `prompt_changes` 不再接受，应重新评测生成新结果。
- 最小改动：
  - `EvolutionReflection.prompt_changes` 替换为 `experience_injections`；候选类型
    `PROMPT` 替换为 `EXPERIENCE_INJECTION`，旧名称设为 `reserved`，防止误复用。
  - Dialogue 模型输出契约明确要求生成可在相似场景复用并注入后续决策上下文的行为
    经验，禁止返回修改 Prompt 模板或源代码的建议。
  - 候选生成器升级为 `evolution-candidate-generator-v2`，避免新经验候选与旧语义候选
    共享稳定身份；Dialogue 模型契约标识同步升级为 `content-aware-dialogue-v2`。
    模型调用使用的 system/user Prompt 技术字段保持不变。
  - 同步更新字段说明、流程说明和回归断言。
- 涉及文件：
  - `tools/trajectory_evolution/proto/hybrid_evaluation.proto`
  - `tools/trajectory_evolution/src/sessionlog_evaluator.cpp`
  - `tools/trajectory_evolution/src/evolution_pipeline.cpp`
  - `tools/trajectory_evolution/config/prompts/dialogue-system.txt`
  - `tools/trajectory_evolution/config/prompts/dialogue-user.txt`
  - `tools/trajectory_evolution/tests/test_sessionlog_evaluator.cpp`
  - `tools/trajectory_evolution/tests/test_evolution_pipeline.cpp`
  - `tools/trajectory_evolution/README.md`
  - `docs/trajectory-evaluation/EVALUATION_FIELDS.md`
  - `docs/trajectory-evaluation/BATCH_AND_MEMORY_FIELDS.md`
  - `docs/trajectory-evaluation/CONTENT_LIMITS.md`
  - `docs/trajectory-evaluation/SESSIONLOG_TO_EVOLUTION_MEMORY.md`
  - `HARNESS_CHANGES.md`
- 验证结果：Release 模式重新生成 Protobuf 并完整编译成功；5/5 CTest 全部通过。
- 边界：历史 Memory 中由旧 `PROMPT` 语义生成的条目内容不会自动改写成经验；继续实验
  前应使用新字段重新评测并生成 Memory，避免把旧 Prompt 修改建议误作可注入经验。

### CHG-20260907-138：按兼容组隔离跨轨迹元反思

- 日期：2026-09-07
- 问题：元反思原先把所有 Candidate Seed 放入一次模型请求。虽然输入提供了由
  `target_type + target_scope` 构成的白名单，`deepseek-v4-flash` 在 Seed 增长到
  50～60 条时仍可能跨白名单组合并，严格校验器随后以
  `incompatible_seed_group` 原子拒绝整次元反思。
- 风险分析：GitNexus 无法解析匿名命名空间中的 `make_meta_request` 和
  `TrajectoryEvolutionPipeline::synthesize_reports`，返回 `UNKNOWN`，不能作为低风险
  结论。补充精确引用检查确认改动仅位于离线 `tools/trajectory_evolution` 的候选合成
  路径和对应测试；没有修改 AgentFlow C++ 底座、Harness、SessionLog Hook、
  Coordinator 或 Skill Executor。
- 最小改动：
  - 在发送模型请求前，按 `target_type + target_scope` 确定性划分 Seed；每个至少包含
    2 个 Seed 的兼容组独立请求元反思，单例直接保留，不再依赖模型自行遵守跨组边界。
  - 所有请求在发送前统一检查单请求大小和整批模型调用预算；预算不足或任一请求过大时
    不发送部分批次。所有兼容组都通过现有严格校验后才原子替换候选，任一组失败仍保留
    原始 Seed，不会让不合规结果进入 Memory。
  - Batch 继续使用一个 `meta_reflection` 汇总状态；请求/响应指纹和 attempts 对本次全部
    分组请求聚合，`model_requests` 逐请求保留审计记录。服务端 Seed、Evidence、中文文本、
    凭据和大小校验均未放宽。
  - 热加载元反思提示词同时明确 Seed 白名单子集和已选 Seed 证据并集约束；不需要重新
    编译即可替换，C++ 仍负责不可绕过的结构校验。
- 涉及文件：
  - `tools/trajectory_evolution/src/evolution_pipeline.cpp`
  - `tools/trajectory_evolution/tests/test_evolution_pipeline.cpp`
  - `tools/trajectory_evolution/config/prompts/meta-reflection-user.txt`
  - `HARNESS_CHANGES.md`
- 验证结果：
  - 普通构建和 GCC `-Werror` 构建均成功；两套完整 5/5 CTest 全部通过。新增测试覆盖
    两个不同 Skill scope 的兼容组分别调用、分别合并及完整来源追踪。
  - 删除全部旧派生产物、仅保留 16 份原始 SessionLog 后，使用
    `deepseek-v4-flash`、`thinking=disabled` 和 30 分钟单请求超时完成全量重建：
    16/16 Dialogue 模型评测成功，轨迹完整性 16/16 有效，目录失败数为 0。
  - 63 个输入 Seed 被拆为 6 个单兼容组元反思请求；元反思状态为 `SUCCEEDED`，生成
    21 个合并候选，最终保留 25 个 Candidate。Memory 从空库写入 revision 1，最终
    保留 19 条 Entry，并完成周期全库整理。
  - 共记录 54 个脱敏模型请求包；对整个 `trace_output` 扫描当前环境中的真实密钥值和
    常见凭据前缀均为 0 命中文件，且没有 `.tmp` / `.part` 残留。
- 最终产物：
  `trace_output/evolution-model-full-deepseek-v4-flash-experience-v2-rerun-20260904/`。

### CHG-20260907-139：记录独立交付与完整 L4 能力补充方案

- 日期：2026-09-07。
- 目标与诱发原因：当前离线链路已可生成和管理候选记忆，但独立接入、在线应用、
  受控实验和发布回滚仍需补充，将讨论结论固定为实施与验收依据。
- 改动内容：新增现状对照、L0–L4 接入条件、通用轨迹、独立打包、策略运行时、应用
  回执、实验验证、内容版本绑定、持续运行、数据隔离及分阶段验收清单。
- 涉及文件：
  - `docs/trajectory-evaluation/INDEPENDENT_DELIVERY_AND_L4_PLAN.md`
  - `HARNESS_CHANGES.md`
- 验证方式：核对公共接口与实现边界，检查文档相对链接和补丁空白格式。
- 遗留事项：本文是方案，不代表 L4 已实现或通过验证；尚未新增在线接口。

### CHG-20260907-140：同步远程同名分支的通用问答助手示例

- 日期：2026-09-07。
- 目标与诱发原因：远程 `origin/harness_agent_poc_evo` 新增提交，按用户要求更新本地。
- 改动内容：fetch 后以 fast-forward 从 `46a2daa` 更新至 `79e1bb4`，引入 9 个提交，
  最终文件差异为新增 `examples/agent/daily-qa-assistant` 下的 37 个文件。
- 涉及文件：远程新增示例目录及本记录；未重写提交、未创建本地合并提交。
- 验证结果：远程变更与未提交文件无重叠；更新前后本地未暂存差异、暂存差异和未跟踪
  文件列表指纹完全一致；当前分支与远程提交一致。凭据仅用于交互认证，未配置持久存储。
- 遗留事项：此次仅同步远程，未启动新增示例或运行其模型/工具测试；原本地改动仍未提交。

### CHG-20260907-141：独立交付第一单元——标准轨迹、L0 接入检查与安装包

- 日期：2026-09-07。
- 目标：不破坏当前 AgentFlow 的前提下，建立框架中立输入和独立交付边界。
- 风险分析：按要求调用 GitNexus impact；当前索引未解析到离线入口及新增符号，
  返回 UNKNOWN，不能视为低风险或没有调用者。补充检查确认涉及 CLI 单文件入口、
  Pipeline 批量评测、内容评测和离线测试；未修改 AgentFlow C++ 底座、Harness、
  SessionLog Hook、Coordinator 或 Skill Executor，也未重建整个仓库索引。
- 本次改动：
  - 新增 TraceRun JSON/Protobuf 协议：必需运行身份、开始/结束和显式业务/执行结果，
    可选步骤、动作、Loop、因果关系、应用回执及自定义结构化内容。
  - 原 SessionLog 分析器原样保留；标准入口独立解释通用执行步骤，再复用既有
    Evaluation、候选合成、元反思、记忆管理和持久化路径。保留旧输出协议和 C++ API。
  - 标准轨迹校验覆盖去重、冲突、配对、执行归属、生产者序号、悬空因果和环；
    稳定排序不被当作并行之间的因果关系。局部分支失败不覆盖显式 Run 结果。
  - 新增中立模型系统提示词和可选配置；真实中文输入、输出和自定义字段经过既有
    脱敏与限长后进入模型。缺少中立提示词时拒绝标准轨迹模型请求，旧配置继续可用。
  - 作用域同时包含 Agent 与角色，避免不同 Agent 的同名角色混合；标准内容
    指纹稳定化，包含 Run 身份及上下文。通用规则仅保守识别明确的不可重试重复。
  - 新增 doctor，区分声明、结构性数据证据、实际验证和生效级别，支持 strict /
    downgrade 与不适用说明。当前实现上限为 L0，L1–L4 运行检查明确 NOT_VERIFIED。
  - 新增中立公共头、TrajectoryEvolution::core CMake 导出、安装配置/协议/示例，
    保留原命令并新增 TRAJECTORY_EVOLUTION_CONFIG 与 Linux 安装目录配置发现。
- 涉及模块：tools/trajectory_evolution 内的协议、公开接口、离线入口、配置、
  标准分析器、CMake、测试及示例；另更新独立交付计划文档和本记录。
  工作区原有未提交变更保留，未将其全部归入本次改动。
- 验证结果：
  - Release 构建成功；原五组加标准轨迹/CLI 两组，共 7/7 CTest 全部通过。
  - 16 份真实 SessionLog 重评测成功，0 失败；34 个 JSON/Protobuf/Batch 文件
    与本次独立化修改前逐字节一致，Batch ID 也一致。
  - 测试覆盖黑盒输入、并行失败隔离与结果归因、乱序、重复、损坏因果、资源限制、
    实际中文内容、凭据脱敏、伪造证据拒绝、无效轨迹不调用模型、JSON/PB、
    候选入库、重复批次幂等，以及数据字段齐全也不能认证在线 L4。
  - 独立安装 CLI 从仓库外工作目录运行成功；外部 CMake 消费者只链接安装包：
    evaluated=2、failed=0、candidates=1、memory_entries=1、replay_idempotent=true。
    CLI 磁盘批次重放后 Memory 仍为 1 条，revision=1。
  - 本次使用离线可控模型适配器验证内容与模型协议，没有调用真实供应商 API。
- 本地验证产物：build/trajectory-evolution/ 下的 compatibility-l0-before、
  compatibility-l0-after、install、standalone-consumer、standalone-results；
  未覆盖当前正式 evaluation/memory，未提交或推送代码和产物。
- 使用说明：tools/trajectory_evolution/STANDARD_TRACE.md。
- 后续边界：本次不是完整 L4；Shadow 查询/回执、在线注入、动作/Loop 控制、
  实验验证和自动发布仍待后续单元实现。

### CHG-20260907-142：在独立交付说明中补充 GEPA 多轮优化算法

- 日期：2026-09-07。
- 目标：将已讨论的 GEPA 搜索机制纳入原有独立交付与 L4 方案，不另建一套说明或程序。
- 改动内容：
  - 补充框架中立的试验输入 / 输出契约，明确真实策略应用、回执和任务结果归因。
  - 定义完整策略候选、任务级成绩、父版本选择、局部反思变异、小批筛选、完整验证、
    可选合并及最终留出验证；区分 GEPA 机制与本组件的保护条件和保守实现变体。
  - 明确训练 / 搜索验证 / 留出隔离、预算与恢复、并行失败边界、记忆快照冻结以及
    候选入库不等于策略验证，并同步更新验收清单和实施顺序。
- 影响范围：仅修改本条记录和原独立交付说明，不改变函数、接口实现、C++ 底座、
  Harness、模型配置、现有轨迹或评测 / 记忆产物。
- 影响检查：GitNexus MCP 未挂载，强制使用已有命令的一次 CLI 检查返回 ENOENT；
  未安装依赖或重建索引，未取得调用者 / 执行流结论，不将图检查视为通过。此次仅为
  Markdown 修改，不涉及可执行符号，也不提交代码。
- 涉及文件：
  - `docs/trajectory-evaluation/INDEPENDENT_DELIVERY_AND_L4_PLAN.md`
  - `HARNESS_CHANGES.md`
- 验证方式：核对算法与现有 L0–L4、Runtime 和 Memory 边界，检查章节、相对链接与
  补丁空白格式；文档变更不触发模型调用或运行测试。
- 后续边界：GEPA 搜索、试验接口和发布控制仍为待实现设计，当前已交付能力不变。

### CHG-20260907-143：绘制独立交付与 GEPA 流程总览图

- 日期：2026-09-07。
- 目标：参考 ACBench Figure 1 的三栏论文图布局，将当前流程与规划中的自进化闭环
  画成可直接查看、编辑和用于汇报的示意图，不使用 Mermaid 代替图件。
- 改动内容：
  - 原创 SVG 展示离线轨迹 / 评测 / 记忆、GEPA 候选竞争与反思试验、独立 C++ 组件
    及宿主接入；配套 PNG 与单页 PDF，并在原独立交付说明开头嵌入和链接。
  - 实线与虚线区分已实现和规划中能力，明确只有 L0 已验证；矩阵分数仅为示意，
    30%–60% 为课题目标，不是已获得的实验结果。
  - 搜索验证的成绩回流候选池，最终留出不参与搜索，避免图中形成错误的数据反馈。
- 影响范围：仅文档与静态图件，不改变 C++、Harness、评测、记忆管理或模型配置。
  GitNexus 文档目标检查未取得调用图结论，不将工具不可用视为影响检查通过；未安装
  GitNexus 或重建索引，不涉及可执行符号修改。
- 涉及文件：
  - `docs/trajectory-evaluation/figures/trajectory-evolution-overview.svg`
  - `docs/trajectory-evaluation/figures/trajectory-evolution-overview.png`
  - `docs/trajectory-evaluation/figures/trajectory-evolution-overview.pdf`
  - `docs/trajectory-evaluation/INDEPENDENT_DELIVERY_AND_L4_PLAN.md`
  - `HARNESS_CHANGES.md`
- 验证结果：SVG 结构和内部引用有效；PNG 为 4960×2320；PDF 为单页，文字未超出
  页面边界且包含修正后的搜索反馈说明；15 处文档本地链接均有效，补丁格式检查通过。
  已视觉复核中文、线稿图标、箭头和虚线状态边界；未调用业务模型或重跑已有实验。

### CHG-20260907-144：补充 A4 详细运行与分级接入图

- 日期：2026-09-07。
- 目标：把原总览进一步展开为一张 A4 横向详细图，说明宿主具体接入点与完整数据流，
  保留原图，不改变任何运行代码。
- 改动内容：
  - 展示 L0 事件采集、L1 上下文注入、L2 动作控制、L3 循环决策与 L4 真实试验 /
    发布接口，各自明确插入位置、输入和产出。
  - 画出宿主执行、串并行分支、循环返回、旁路采集，及校验、规则 / 内容级模型评测、
    反思、候选质量门、去重 / 关系判断、周期全库整理和容量淘汰。
  - 展开 Memory → PolicyCompiler → 不可变 PolicyBundle → QueryAdvice → 宿主应用
    与回执；标明版本、作用域、冲突、Shadow 和默认行为边界。
  - 展示 GEPA 父子真实试验、小批筛选、完整搜索验证、候选池反馈、可选合并、
    独立留出和发布撤回；试验轨迹复用评测，不立即改变正式记忆快照。
  - 配套 SVG、高清 PNG 和 A4 单页 PDF，在原方案内补充阅读顺序与跨区端口释义。
- 影响范围：仅静态图件、原方案文档与本条记录；不修改 C++、Harness、模型配置、
  轨迹或评测 / 记忆结果。已有本地导出缓存不属于项目功能变更。
- 影响检查：对本仓库文档目标执行一次不安装依赖的 GitNexus CLI 检查，返回 ENOENT；
  调用者、执行流和图风险未获验证，不将工具不可用视为通过；不涉及可执行符号变更。
- 涉及文件：
  - `docs/trajectory-evaluation/figures/trajectory-evolution-runtime-a4.svg`
  - `docs/trajectory-evaluation/figures/trajectory-evolution-runtime-a4.png`
  - `docs/trajectory-evaluation/figures/trajectory-evolution-runtime-a4.pdf`
  - `docs/trajectory-evaluation/INDEPENDENT_DELIVERY_AND_L4_PLAN.md`
  - `HARNESS_CHANGES.md`
- 验证结果：179 处文字经浏览器测量，无超宽、越界或明显重叠；SVG 内部引用与
  19 处文档本地链接有效。PDF 为 A4 横向单页，关键等级 / Hook 名称可正确提取，
  文字未超出页面；PNG 为 5940×4200。已视觉复核并行、循环、搜索反馈与留出发布
  连线；补丁格式检查通过，未调用业务模型或重跑实验。
- 能力边界：当前仅 L0 已验证；L1–L4、Runtime、GEPA 和自动发布均明确标为规划，
  此图不代表新增能力已经实现或策略收益已经验证。


### CHG-20260907-145：落地经验类 Shadow Runtime、查询和未应用轨迹

- 当前状态（2026-09-09）：本条中的 AgentFlow 事件、SessionLog 扩展和示例 Hook 挂载已撤回；独立进化组件保留，见 CHG-20260909-169。
- 日期：2026-09-07。
- 目标：在现有 Memory 之后打通只观察的查询单元；保持 AgentFlow 兼容，不提前实现
  实际注入、动作 / Loop 控制或 GEPA，也不提升 doctor 的 L0 已验证上限。
- 新增组件：
  - 本目录内的 shadow_policy.proto、shadow_runtime.h / .cpp、shadow_cli、
    integrations/agentflow/shadow_hook.py、config/shadow.json 及回归测试。
  - C++ 编译每 Run 不可变快照，过滤身份不明、混合用户、错误作用域、冲突 / 反证、
    未达状态门槛和不安全内容；仅选择 EXPERIENCE_INJECTION。
  - C++ QueryAdvice 使用中文相邻汉字 / 英文单词进行确定性匹配、排序和限量；
    保存 Memory / Candidate 来源、内容修订、快照 ID、输入指纹和命中理由。
  - 复用原二进制的 shadow compile / query JSON stdin 入口及既有安全门，
    不增加在线模型调用、向量库或常驻服务；C++ SDK 随独立 CMake 包导出。
- AgentFlow 最小接入：
  - 新增两个观察事件；SessionLog 原文件及两个包副本同步增加对应采集分支。
  - 本地 examples Agent 使用符号链接挂载唯一 Hook 源文件；默认关闭，不修改
    .env、API、模型、消息、技能派发、业务结果和原有日志目录结构。
  - dialogue.start 固定快照，v1 跨进程重触发 / v2 后续决策从 DataBus 复用；
    key 同时包含宿主 dialogue 清理前缀和完整身份哈希。
  - 初始化失败固定为本 Run 错误；查询失败不影响原执行或其他 Run；
    连接建连 / RPC / 子进程均有限制，连接在返回和异常出口关闭，结束时清理状态。
  - SessionLog 记录 snapshot / query，并关联本次 llm.request 和 LLM span；
    始终 mode=shadow、application_status=not_applied，不伪造 advice.applied。
- 兼容性与边界：
  - 未修改 AgentFlow C++ 底座、Coordinator 或 Skill Executor 的执行实现。
  - 快照只做完整性校验，不是鉴权签名；部署方负责可信身份、Agent / Memory 绑定。
    不自动跨用户共享；宿主需保证一个 Run 只有一个有效 start。
  - Shadow 有受限同步开销，不代表零延迟；词面命中不是语义适用性或效果证明。
  - 核心校验沿用 C++ 安全门；Python Hook 显式登记宿主环境凭据并复用脱敏，
    未登记的任意秘密仍需调用方治理；未调用真实 API，不宣称完成线上安全验收。
- 验证结果：
  - C++ 构建成功，8 / 8 组 CTest 通过；覆盖不可变快照、Protobuf 回放、中文 /
    英文匹配、冲突两端排除、作用域 / 用户隔离、24 路并发和 Shadow 不提升 L1。
  - Python 129 条通过：新增 13 条 + 原 Hook、Span、安全与 SessionLog 116 条。
    新增用例使用真实 C++ 子进程、DataBus SDK 与测试 UDS 服务，验证两用户、
    三会话、12 次查询及跨进程重入；结束后没有影子状态残留，v1 / v2 因果关联正确。
  - 含 Shadow 的合成完整 SessionLog 经现有 evaluate 命令成功评测。
  - 独立安装 CLI 可从非根目录使用；外部 CMake 消费者只链接安装包，返回
    standalone_shadow=matched、application=not_applied、trace_kind=custom。
  - 补丁格式检查通过；安装规则排除 __pycache__ / .pyc。测试和验收内容仅在
    build/shadow-tests、build/shadow-sdk-consumer 与 build/trajectory-evolution 下，
    未覆盖现有真实 SessionLog、Evaluation 或 Memory，未进行 git 提交或推送。
- 影响检查：按 GitNexus 影响分析要求尝试已有 MCP / CLI 路径，工具不可用；
  调用者、执行流及图风险仍未取得结论，不视为通过。已复核实际接入点并进行
  上述回归；未安装 GitNexus、重建索引或重试不可用的 bwrap。
- 使用与后续：
  - 使用说明：tools/trajectory_evolution/SHADOW_RUNTIME.md。
  - 原 README、STANDARD_TRACE.md、独立交付说明同步标注第二单元状态；
    既有规划图没有被当作已实现证明。
  - 下一单元是受控经验注入、真实应用回执和 L1 探针；本轮不启动真实 hchat 实验。


### CHG-20260907-146：最小接入实现受控 L1–L3 与实际应用回执

- 当前状态（2026-09-09）：本条中的 AgentFlow 运行时接入已撤回，包括 Loop 控制、经验注入、逐调用拦截和回执采集；独立进化组件保留，见 CHG-20260909-169。
- 日期：2026-09-07。
- 目标：按本轮“连续实现到 L3、保持最小改动”的要求，在既有 Memory / Shadow 上
  实现经验注入、逐调用动作干预和实例级 Loop 控制；默认关闭，保留 AgentFlow 兼容。
- 独立组件新增：
  - `proto/runtime_policy.proto`、`include/trajectory_evolution/policy_runtime.h`、
    `src/policy_runtime.cpp`、`src/policy_cli.*`、默认关闭的 `config/policy-runtime.json`、
    外部经验模板及 `integrations/agentflow/runtime_hook.py`。
  - 复用原 Shadow 快照、检索、安全检查和同用户 / 非冲突候选过滤；不另建服务或向量库。
  - C++ 纯查询与回执验证：严格 / 降级能力检查、Memory ID + 内容修订显式批准、
    单次调用 ID 过滤、必选参数检查、Continue / Replan / Stop、Protobuf 状态保存。
  - Loop 状态绑定 Run 策略、作用域和执行实例；重复轮次幂等，旧 / 跨实例状态拒绝。
    状态计数在宿主最终决策后写入，不能把被其他 Hook 拒绝的 Stop 留为粘滞停止态。
  - Prompt / 阈值由外部配置提供，Run 内固定版本；不把自然语言记忆自动变成强控制规则。
- AgentFlow 薄接入：
  - AgentContext 新增单次 `request_context`，所有 Hook 完成后构造请求副本；
    未注入时保留原列表路径，不在多轮历史中累计经验。
  - v1 / v2 / Skill Executor 各补一个 Loop 决策边界；停止发生在下一次 LLM span
    创建前，保留已有产物，正常输出 `policy_stopped`，不改 Python 退出码协议。
  - v1 派发仅按原始 request_index 跳过单项；Skill / v2 按 tool_call_id 跳过单次调用。
    不修改 `execute_in_batches`，不扩大原 force 语义，不取消其余并发分支。
  - v2 工具结果新增调用 ID、原始序号及业务错误观察，不能把返回错误内容记成成功。
  - Recorder 的最终请求观察增加 `transport_allowed`；安全门拦截的请求不能产生
    成功应用回执。回执区分建议、实际请求、实际跳过及实际 Loop 决策，不证明模型遵从。
  - 热关闭 / 请求未发生 / 回执组件失败均保留未应用或验证失败记录，清理进程内待办。
  - SessionLog 源文件和两个包副本同步采集 snapshot / query / receipt / error、
    Loop 决策及 v2 工具事件；原用户 / 会话 / Dialogue 目录结构不变。
  - examples Agent 挂载组件入口，默认关闭；没有修改业务 `.env`、API 或模型配置。
- 实际测试发现并修正：
  - `AF_EXEC_SKILL_KEY` 被 C++ 通用 KEY 环境扫描误当作密钥，导致 Skill 策略查询拒绝。
    仅在适配器中将该执行身份改为通过协议传入，不向子进程传同名环境变量；
    实际凭据仍参与安全检查，没有放宽通用安全门。
  - 保持旧按名称跳过工具的结果字段不变；新增原因只用于新逐调用控制。
  - 策略 `decision_id` 独立生成，不误用贯穿多轮的父 Skill span ID。
- 影响边界：
  - 未修改 AgentFlow C++ 底座，未改已有编排 / 并行调度实现，未改 Memory 的入库字段。
  - 执行变化集中在三个 Python 决策入口、通用上下文 / 回执边界和组件内新文件。
  - 未实现 L4 GEPA、自动实验、自动发布或回滚；L2 是前置条件 / 逐项拒绝，不是新调度器。
  - `component_level` 更新为 L3；离线 doctor 仍只验证到 L0，不把宿主声明当在线认证。
- 验证结果：
  - 最终 C++ 构建及 9 / 9 组 CTest 通过；覆盖版本 / 作用域 / 用户隔离、缺失能力降级、
    24 路并发、实际回执、重复 / 被删除 / 被拦截的注入、实例级 Stop 和有界 Replan。
  - Python 完整本地回归 515 条通过，1 条真实 API 测试默认跳过；已另行真实执行该测试。
    覆盖原恢复 / Span / 并行 / 安全测试，以及 v1 / v2 / Skill 实际执行与提前退出。
  - v2 的同批三个同名工具分别为 skipped、completed、error，仍按原始 ID 顺序回灌；
    被拒绝的调用未执行，失败分支未阻止另一个正常分支。
  - 真实 DataBus SDK + 测试 UDS：2 个用户、3 个会话、各 2 个 Run，跨进程重新加载
    Hook，身份及版本无串用，连接关闭，正常 Run 结束后状态无残留。
  - 本地 HTTP 服务实际收到的消息每次只有一个注入块；三条 SessionLog 回执均关联
    各自 `llm.transport.request`；现有 evaluate 程序评测该日志 evaluated=1、failed=0。
  - 使用用户授权的 Agent Plan 进行 3 次真实模型调用，模型 deepseek-v4-flash、
    thinking=disabled，均返回非空回答，3 条回执均为 applied。密钥经关闭回显的 stdin
    进入测试进程，没有写入项目配置、源码或文档；测试产物凭据扫描未发现泄漏。
    结果：`build/trajectory-evolution/policy-live-20260907-b/test_live_model_injection0/live-acceptance.json`。
  - 独立安装消费者只 find_package / 链接 TrajectoryEvolution::core，全部策略断言实际运行通过。
  - 外部沙箱集成组曾因本地代理 / 服务初始化超时失败，已中止；上述完整回归明确排除
    该组，不把它计为通过，没有为此重试 bwrap 或修改沙箱 / 网络环境。
  - 补丁格式和三份 SessionLog 一致性检查通过；未覆盖既有真实轨迹、Evaluation 或 Memory。
- 影响分析：
  - 按仓库 GitNexus 影响分析技能检查 Recorder、Context、三个执行入口、dispatch 与
    Runtime 接入符号。MCP 未挂载；已有 CLI 可以启动，但索引不兼容，定向检查仍为
    UNKNOWN / partial。一次 index-only 刷新因图数据库缓冲区不足失败，未重新安装工具。
  - 调用者 / 执行流 / 图风险未获有效结论，不视为通过；通过逐调用点检查和上述
    回归约束修改范围。没有进行 git 提交或推送。
- 使用说明：`tools/trajectory_evolution/POLICY_RUNTIME.md`；README、STANDARD_TRACE 和
  独立交付方案同步更新。L4 及长期收益验证仍是后续工作，本次不宣称无效 Loop 已下降。

### CHG-20260908-147：更新进化组件 README 的完整流程、配置与接入说明

- 日期：2026-09-08。
- 目标：说明当前实际实现及用户配置、运行、持续入库和接入方式，不扩展执行代码。
- 文档更新：
  - `tools/trajectory_evolution/README.md` 增加使用导航与完整流程，区分离线评测入库、
    Shadow 只读观察、受控 L1–L3 和尚未实现的 L4 / GEPA 自动试验发布。
  - 补充构建依赖、本地配置与 Prompt 副本、环境变量凭据、模型请求路径、可配置的
    30 分钟 HTTP 超时示例，以及模型评测、元反思和记忆整理的调用边界。
  - 补充 AgentFlow Hook 挂载、用户/会话/Run 身份、经验 ID + 内容修订批准、
    L2/L3 规则示例、关闭方法及实际应用回执的检查方式。
  - 补充标准日志、doctor、独立 CMake SDK 与 runtime compile/query/verify 协议，
    明确仅交付日志不等于在线控制接入，宿主需执行决策并提供真实观察。
  - 修正目录支持的输入后缀、持续入库与重放窗口说明；同步更正 STANDARD_TRACE.md
    和 SHADOW_RUNTIME.md 中“L1–L3 尚未实现”的过时表述。
- 验证与边界：
  - 核对当前源码、配置模板、Protobuf 及 CLI 帮助；实际运行示例 doctor，
    trace_valid=true、verified_level=L0、component_level=L3，与文档一致。
  - README JSON 示例与 Bash 语法、本地链接/导航及补丁格式检查通过。
  - 本次仅改 Markdown，未改变 C++/Python、业务配置、模型、Hook 挂载或既有轨迹/
    Evaluation/Memory；没有调用付费模型，没有进行 git 提交或推送。
  - 按仓库要求尝试 GitNexus 入口影响分析，已有 CLI 返回 evaluate_trace 未解析、
    risk=UNKNOWN，调用者/执行流/图风险未获结论，不视为通过；没有再次安装或重建索引。

### CHG-20260908-148：将经验生成、记忆筛选与注入收敛到明确的适用条件

- 日期：2026-09-08。
- 问题：旧经验混有复盘、开发改造建议、测试特例和跨角色职责；作用域常由唯一 Skill
  推测，触发条件可能只是来源标签。Shadow 从问题和建议正文召回，L1 只注入正文，
  丢失适用条件，容易把词面命中说成已经适用。
- 最小实现范围：仅修改 `tools/trajectory_evolution` 的生成/记忆/运行时、外部 Prompt、
  协议及测试，并同步使用文档；本次未修改 AgentFlow C++ 底座或 Python 执行入口。
- 生成和证据：
  - 新增 Protobuf `ExperienceAdvice` 和 `reflection.experience_details`，保留旧字符串
    `experience_injections` 作为可读/兼容视图，不增加 Memory 字段或新的管理程序。
  - 每条经验明确 target_scope、trigger_pattern、proposed_change、evidence_event_ids；
    作用域从真实轨迹执行者白名单选择，证据逐条引用，不继承整个 Dialogue 的浪费步骤。
  - AgentFlow 和框架中立日志复用同一契约；无效经验单项 warning 隔离，任务及步骤
    评测仍可成功。旧字符串响应保留阅读能力，但不视为具备新注入契约。
  - 外部评测/元反思 Prompt 要求原子、条件化、可执行经验，保留核验、恢复依据和
    独立分支；不将一次性限制或测试次数泛化，不混淆业务结果与工作流状态。
- 记忆与运行时：
  - 使用既有 generator_version 区分 `scoped-experience-generator-v1`；元反思分组、
    精确去重、增量语义整理与周期全库复核隔离新旧契约，避免旧经验经合并获得资格。
    Curator 算法版本更新为 v3；同批重放与来源映射保持幂等。
  - 新 Shadow 排除旧/不合格经验并记录 `experience_contract`；匹配词只来自条件，
    原有用户隔离、冲突、证据、状态和大小过滤保持。
  - L1 保留作用域、条件和行为，序列化为固定字段顺序的 JSON 经验记录；超预算时
    跳过整条，不截掉条件只留建议。新增回归复现并排除无序 Map 导致回执重算不同的问题。
  - 外部注入模板不再保证经验适用：要求结合当前条件、权限和预算判断，不能仅为
    套用经验额外探测，不提升文本权限。ID + 内容修订仍需显式批准。
  - 新 Shadow schema_version=2，旧版本 1 仍可只读回放，但不能编译成新 Active。
    Runtime ID 更新为 `runtime-v2:`，禁止旧策略静默改用新渲染语义；需在 Run 间升级。
- 验证：
  - 完整 C++ 构建及 9/9 组 CTest 通过；覆盖 SessionLog/标准轨迹 → 模型响应校验
    → 候选 → Memory → Shadow → L1、逐条错误隔离、作用域/证据保真、新旧契约分组、
    持续入库/周期整理、幂等、32 次重复渲染、24 路并发、整条预算与被删改条件的回执。
  - Python 本地回归 515 条通过，1 条真实 API 测试默认跳过；沿用原隔离外部沙箱组
    的测试范围。使用受控响应和本地真实 C++/HTTP/UDS，不调用付费模型。
  - 新安装包导出新增头文件与 Protobuf；外部 CMake 消费者仅链接
    `TrajectoryEvolution::core` 验证注入接口。真正旧二进制生成的 v1 快照可由新版本
    shadow query 只读回放，matched；旧 Active 编译明确拒绝。
  - 正式 Memory 的 19 条记录及 PB/JSON 校验和未变化；对两个用户分别只读编译后
    入选均为 0，旧经验有明确排除计数，没有偷偷重写或自动批准。
  - 测试安装和验收产物只在 `build/trajectory-evolution/experience-*`；未覆盖正式
    SessionLog/Evaluation/Memory，未改业务 API、密钥或配置，未提交或推送 Git。
- 影响检查与边界：
  - 遵循 GitNexus exploring/impact 技能，编辑前尝试影响分析；已有 CLI 无法解析
    当前演进符号，结果仍为 UNKNOWN，不视为通过。通过逐调用入口核对和上述回归
    约束范围；没有再次安装工具、重建索引或重试不可用的 bwrap。
  - 生成契约和 Prompt 自查不是独立语义审查，不证明收益、模型遵从或长期有效性。
    仍保留人工批准和后续 L4 对照验证边界。
  - 旧库仅保留与隔离；使用更新后的 Prompt 从原始轨迹重新评测才能生成新经验。
    私有 Prompt 副本需同步契约，不能手工修改生成器/快照版本冒充通过。
- 文档：README、POLICY_RUNTIME、SHADOW_RUNTIME、Evaluation 字段和 Batch/Memory 字段说明。

### CHG-20260908-149：明确跨语言接入、L0–L4 嵌入点及通用交付缺口

- 日期：2026-09-08。
- 目标：回应接入步骤复杂、非 C++ 宿主难复用、各等级如何影响 Agent 说明不足的问题。
- 文档修改：
  - README 开头改为接入者导航，区分 C++ 核心、SDK/本地 CLI 通信与宿主执行边界。
    明确其他语言可用 JSON stdin/stdout，不需要 C++ FFI、生成 Protobuf 代码或 DataBus。
  - 新增 L0–L4 的具体代码位置、输入、输出和实际影响；说明公共 Run 初始化/收尾、
    L3 → L1 → L2 的执行顺序，以及最终请求、逐调用结果和实例级控制回执的位置。
  - 区分 L1 软影响、L2 真正拒绝动作、L3 Stop/Replan，以及尚未实现的 L4 隔离试验/
    GEPA/发布回滚。能力是累计条件，不是填写 requested_level 就创建对应 Hook。
  - 补充 AgentFlow 真实 Hook/源码映射，其他宿主不需要照搬 Coordinator、Skill 或目录。
  - 新增 Python 标准库传输示例：直接发送内存 JSON，不要求每次手写三份请求文件。
    明确该示例只是低层调用，不是已完成的通用 SDK。
  - 明确现有约束：evaluate 仍读本地文件/目录，CLI 每次启动进程，快照/Loop 状态/
    回执关联需宿主管理，doctor 声明与在线能力字段不同，尚无 HTTP/gRPC 服务或
    各平台完整预编译发行物；L1/Replan 验证仅支持 messages/system/字符串 content。
  - README 单列待实现的通用接入封装方向：高层配置、Run 句柄、实例隔离、真实观察
    适配、跨语言示例与交付探针；不将规划接口伪装为已实现功能。POLICY_RUNTIME 同步说明。
- 验证：
  - 从 README 提取 runtime_rpc 函数，用 Python 标准库调用真实 C++ CLI，非 AgentFlow
    作用域下完成 compile/query/verify：一个缺参数动作被拒绝，两个允许项实际本地执行，
    回执 applied；没有导入 AgentFlow Runtime、连接 DataBus 或调用模型/网络。
  - 核对 Protobuf、PolicyRuntime、CLI 和宿主 Hook 的真实实现；检查本地链接/导航、
    JSON/Python/Bash 示例语法及补丁格式。
- 影响与范围：
  - 仅修改 README、POLICY_RUNTIME 和本记录，未改变执行源码、协议、配置、Hook、
    正式轨迹/Evaluation/Memory；没有借此扩建服务、安装 SDK、调用付费模型或提交 Git。
  - 按 GitNexus exploring/impact 指引查询当前 AgentFlow 仓库：索引落后 9 个提交、
    FTS 降级，compile_runtime_policy 未解析，风险 UNKNOWN，不视为无影响。
    已按实际调用入口核对文档；没有修复/重建已知不可用索引，也没有重试 bwrap。
  - 本次澄清并记录通用接入缺口，不宣称已经解决所有易用性问题或实现 L4。

### CHG-20260908-150：补充无日志、无 Hook 的独立 Agent 从零接入示例

- 日期：2026-09-08。
- 目标：以完全独立的新 Agent 为前提说明接入，不假设复用 AgentFlow Runtime、
  SessionLog、DataBus、Coordinator/Skill 或既有 Hook。
- 文档修改：
  - README 新增导航和从零示例：由宿主自己的任务入口、步骤和工具调用处生成标准
    TraceRun JSON；Python 标准库仅用于展示，明确示例函数不是已交付的采集 SDK。
  - 示例区分执行终态和业务结果，异常继续传播；事件在发生时复制，避免后续业务
    修改污染历史；完整落盘后发布 .trace.json，采集失败报警且不覆盖业务结果。
  - 给出 doctor、目录 evaluate 命令和真实输出结构，解释初次建立空 Memory、
    后续批次持续入库及模型评测入口；不从两个成功演示任务凭空生成经验。
  - 明确只包装原 Agent 入口时仅有输入/输出；内部模型、动作、循环需要在自有代码
    中继续记录。分别说明 L1–L3 查询、实际应用和回执位置，以及独立 actor 作用域。
  - 补充同步示例的并发、取消、进程崩溃与隐私边界；不把记录 Loop 等同于控制 Loop，
    不将配置或复制 AgentFlow Hook 冒充独立 Agent 的自动接入，L4 仍明确为未实现。
- 验证：
  - 直接提取并运行 README 代码，两个同步工具调用生成 2 条真实标准轨迹；C++
    doctor 均验证为有效 L0，目录 evaluate 成功 2、失败 0、候选 0，按用户/会话
    生成 Evaluation，初始化空 Memory 的 JSON/PB。
  - 额外检查工具异常、派发前缺参数、业务失败但执行正常、业务未知、取消及可变
    输入快照隔离；这 6 条轨迹也通过 C++ doctor。检查写入失败不改变业务返回值。
  - 无 AgentFlow 导入、Hook 挂载、模型/API 或网络调用；测试产物仅在
    build/trajectory-evolution/logless-demo，不修改正式轨迹、评测结果或记忆库。
- 影响与范围：
  - 只修改 README 和本记录，不修改 AgentFlow、C++、协议、配置或现有 Hook。
  - 按仓库 GitNexus 指引尝试 validate_trace 的 upstream impact；当前索引未解析
    该符号，风险 UNKNOWN，未当作无影响。结合实际协议、校验实现和上述运行验证
    核对文档；没有重新安装工具、重建索引或重试不可用的 bwrap，未提交或推送 Git。

### CHG-20260908-151：将 README 整理为用户使用说明书

- 日期：2026-09-08。
- 目标：用户只需知道怎么安装、配置、插入代码、运行及查看结果，不需先学习内部实现。
- 修改：
  - README 改为按使用场景组织的操作说明书，独立同步 Python Agent 给出三个明确
    位置：文件顶部导入、替换原 Agent 调用、任务结束后的后台评测调用。
  - 将上一条已有的同步采集示例提取到 examples/standard-agent/agent_with_trace.py，
    供直接导入；增加可选 agent_id 参数，避免用户为更换 Agent 名称修改示例内部。
    不新增 AgentFlow 依赖，不将此示例称为全等级或生产级通用客户端。
  - 将原有实现、协议、低层接入和高级配置说明保留在 INTEGRATION_REFERENCE.md，
    同步 POLICY_RUNTIME 中的两处章节链接，避免用户操作步骤混入开发细节。
  - 明确当前简易接法仅提供整次任务的 L0 记录；独立 Agent 的 L1–L3 尚无同等
    通用简易封装，L4 未实现，不把规划接口写成可用函数。
  - 安装清单增加迁移后的参考文档，examples 安装排除 Python 缓存；不改 C++ 运行逻辑。
- 验证：
  - 原样执行 README 的导入、入口替换与后台评测代码：2 个用户、3 次任务生成
    3 条有效轨迹，真实 CLI 评测成功 3、失败 0，生成按用户/会话组织的结果及空 Memory。
  - 提取后的示例继续通过工具异常、派发前缺参数和写入失败不覆盖业务返回值检查。
  - 校验 Python/Bash 代码语法及说明书、参考文档、Policy Runtime 的本地链接与锚点。
  - 在 build 内独立安装，确认手册、参考文档、示例随包提供且没有 Python 缓存；
    使用安装版示例、CLI 和配置完成另一条轨迹评测并初始化 Memory。
- 影响与范围：
  - 按 GitNexus exploring/impact 指引查询本仓库，索引落后 9 个提交、FTS 降级，
    run_recorded 未解析，风险 UNKNOWN，未视为无影响；按已有示例、实际接口和运行
    检查确认范围，没有安装或重建工具。此次不改变 AgentFlow 底座、评测算法或协议。
  - 测试与安装产物只在 build/trajectory-evolution/user-manual-*，未调用模型/API，
    未改正式日志、Evaluation、Memory、密钥或业务配置；未提交或推送 Git。

### CHG-20260908-152：纠正独立接入对语言与整体调用入口的错误假设

- 日期：2026-09-08。
- 问题：前一版使用说明把同步 Python 的整体调用包装作为主要接法，未满足
  TypeScript、Rust、异步 ReAct 及无整体 run 函数的独立交付要求。
- 文档修正：
  - README 改为按任务、模型、动作、下一步调度、完成/取消等事件定位接入；
    事件可分布在不同函数、回调、Worker 或进程中，不要求改造成同步 Agent。
  - 同步 Python 示例保留为折叠的可选演示，不再作为通用接入前提或交付完成证明。
  - 独立交付计划及开发参考明确以 C++ HTTP + JSON 服务和共享语义的语言客户端
    为通用接入方向；保留既有 SDK、CLI、离线文件评测与 AgentFlow 接入兼容。
  - 补充任务关联、重复与乱序事件、分支隔离、流式响应、异常/取消、未完成轨迹、
    鉴权与真实执行回执要求；用户不负责拼整份 TraceRun 或管理内部策略快照。
  - 新增原生 TypeScript、Rust 和无整体入口的异步 ReAct 验收要求，不用 Python
    替代宿主，也不以输入/输出的黑盒测试宣称已完成通用 L1–L3 接入。
- 当前边界：
  - 事件式服务和通用客户端仍未实现；本次修正说明与交付约束，不将规划 API 写成
  现成函数，不声称文档修改已经解决运行接入问题。
  - 未修改 C++ 或 Python 执行代码、配置、业务模型、正式轨迹或记忆库，未启动服务。
- 核查：
  - 对照 TraceRun/TraceEvent、RuntimeCommand、实际 CLI 入口与既有独立交付计划；
    确认核心协议不依赖 Python，但当前离线入口没有增量事件收集服务。
  - 按 GitNexus exploring/impact 指引查询 AgentFlow：索引落后 9 个提交、FTS 降级，
    RuntimeCommand 未解析，风险 UNKNOWN；未视为调用方/执行流程已检查通过。
    仅核对文档与实际契约，不重建索引、不新增依赖，未提交或推送 Git。

### CHG-20260908-153：落地框架与语言无关的事件式 L0–L3 接入

- 日期：2026-09-08。
- 目标：不再假定 Python、AgentFlow Hook、已有日志或完整的 agent.run()；任何能够
  发送 HTTP/JSON 的通用语言都可在独立回调、ReAct 路由和 Worker 中接入同一套能力。
- 独立服务与采集：
  - tools/trajectory_evolution 新增 C++ IntegrationService 与 serve --config 入口，
    暴露 POST /v1/invoke、GET /v1/health；13 个业务操作覆盖任务、步骤、模型、动作、
    状态、采集缺口和评测重试。宿主只发送边界数据，不手工拼整份 TraceRun。
  - 身份来自服务令牌映射；每用户隔离轨迹、评测和 Memory。run/conversation/execution/
    step/call 分别关联任务、会话、并发分支与具体调用；乱序结果按调用 ID 归位。
  - 模型与动作分为 prepare/observe，保留真实请求、结果、执行状态、业务状态及因果边。
    单个动作失败不终止兄弟分支；Stop 只作用于当前 execution，不把查询决定冒充已应用。
  - 请求 ID 与内容指纹形成持久幂等账本；原请求重试不重复记事件，换内容返回冲突。
    私有原子快照支持重启与跨回调恢复；正式轨迹由后台发布，重启后可补发。
  - 完整任务自动进入评测、候选/可选模型元反思、入库与管理；继续复用现有 C++ 核心。
    明确的缺口或中断进入 quarantine，capture_complete=false；不将其当完整训练样本。
- L1–L3 执行接入：
  - L1 按冻结的用户记忆/授权产生经验，支持 JSON Pointer 指定真实请求字段；复制请求，
    不污染长期对话历史。回执核验实际发送边界的内容，不只比较指纹或接受宿主声明。
  - RuntimeOptions 新增可选 request_context_pointer/request_context_mode；未配置仍保留
    既有 messages 路径与验证行为，原 AgentFlow Hook 不需切换到 HTTP 服务。
  - L2 逐调用允许/拒绝，保留同名动作与原串并行调度；收齐观察后核验过滤回执。
  - L3 按独立执行分支维护进展与重规划预算，返回 continue/replan/stop；宿主在下一步
    调度位置实际应用并回报。现阶段控制规则由配置授权，不将其宣称为 GEPA 学习结果。
- 所有通用语言的接入面：
  - 交付 OpenAPI 3.1 与唯一请求 JSON Schema、生成一致性检查及可选语言生成入口。
    协议没有语言白名单；其他语言可直接 HTTP 调用或使用自己的 OpenAPI 工具。
  - 随包提供 Python、JavaScript、TypeScript、Go、Java、C#、C、C++、Rust 原生封装。
    JS 从 TS 机械生成，避免维护两套逻辑。C 接口不绑定宿主 JSON 库；不强制加载完整核心。
  - TS/JS/Rust 另有模型/工具执行包装与可跨回调传递的句柄；原始 invoke 客户端返回
    明确错误，语言指南给出观察失败的最小补缺/unknown 收尾方法，不掩盖业务结果。
  - README 改为用户说明书；LANGUAGE_GUIDE/INTEGRATION_API 说明每层具体插入哪里、
    发送什么、如何影响 Agent，以及异步、并发、流式、取消和跨进程恢复的约束。
- 安全与运行边界：
  - 服务默认只监听 loopback；远程通过 HTTPS 网关。令牌只放 Header，拒绝危险字段和
    已知凭据，固定错误码不回显正文；不能承诺识别任意无标记的业务秘密。
  - 限制请求/响应、JSON 深度、事件数、任务状态大小、在途队列、历史存储和连接等待；
    单实例数据锁、私有权限、拒绝符号链接；已发布终态按需读取，不全部常驻内存。
  - 修正新服务的长中文 ID 编码超过文件名长度、线程创建异常退出时的收尾；修正新增
    C# 客户端返回 JsonNode 仍附着父对象的问题。测试不在后台发布完成之前强行停服务。
  - 超时、评测 Prompt 和模型参数沿用外置配置；服务控制配置重启后用于新任务，同任务
    不热换冻结策略。磁盘仍需部署方保留期管理，副作用幂等仍由宿主业务执行器保证。
- 验证：
  - 全部 13 组 CTest 通过：包含既有 C++ 核心、策略/轨迹兼容测试、新服务、HTTP 与协议。
  - Python/JS/Go/Java/C#/C/C++ 七个原生进程共生成 28 条完整轨迹，逐条通过 Schema 与
    doctor；覆盖 L0–L3、中文长 ID、同名动作、乱序完成、分支失败、幂等冲突、Replan/Stop。
    七个隐私拒收场景被隔离，输出中未检出测试令牌与被拒收的测试凭据。
  - 原生 TS/Rust 回调场景另验证 2 用户、3 会话、多输入与多分支；HTTP 模型评测接口桩
    验证真实模型调用链路、记忆入库、经验应用与重启恢复，没有使用付费供应商 API。
  - AgentFlow runtime 与既有 evolution Hook 回归 515 通过、1 跳过；未运行已知需要
    外部沙箱的 integration 目录。初次包含该目录的检查受到本地代理影响，不冒称通过。
  - TS 严格类型检查及 JS 生成一致性通过；OpenAPI Generator 校验无问题，成功生成
    Kotlin 客户端。Kotlin 仅验证生成，未编译/运行；不把生成器支持列表当作全语言实测。
  - 安装版二进制、配置及 Python 客户端独立完成采集→规则评测→初始化 Memory；外部
    C++ 消费者仅链接安装版 SDK，2 条评测成功、重复入库幂等。测试数据/工具链位于 build。
- 影响与范围：
  - 新运行代码、客户端、协议、配置和测试均在 tools/trajectory_evolution 内；同步更新
    使用说明、开发参考、独立交付计划和本记录；保留旧 SessionLog、CLI、SDK、Hook 接法。
  - 本次没有改 AgentFlow 的 C++ 执行底座，没有更换业务 API/Key、删除正式轨迹或 Memory，
    未提交或推送 Git；保留工作区原有的其他修改。
  - 按 GitNexus exploring/impact 要求尝试查询与上游分析；当前索引落后且未解析新增符号，
    返回 UNKNOWN，不作为低风险或无调用者证明；结合实际入口核对和运行回归约束修改。
    没有重新安装/重建 GitNexus，也没有重试已知不可用的 bwrap。
  - 本次交付到 L3。L4 的 GEPA 搜索、对照实验、策略发布和回滚尚未实现；不承诺仅凭
    接入完成就实现目标无效 Loop 降幅，也不把 applied 回执当作模型遵从或收益证明。

### CHG-20260908-154：补充无 SessionLog、无 Hook 的从零接入教程

- 日期：2026-09-08。
- 用户需求：将“假设 AgentFlow 没有 SessionLog，也没有 Hook，如何接入”的场景写成
  逐步操作示例，明确在哪个终端执行、打开哪个文件、插入什么及如何验收。
- 新增内容：
  - tools/trajectory_evolution/examples/agentflow-no-hooks/README.md：12 步教程，覆盖
    启动、凭据、输出查看、同会话多输入、新会话、Coordinator/Executor/公共模型客户端
    的代码位置、DataBus/manifest 上下文传递、失败/取消收尾及 L0–L3 启用条件。
  - 同目录 walkthrough.py 和 service.json：连接真实 C++ 服务的可运行教学例子。
    Coordinator 派发后退出、两个 Skill 独立进程并发、新 Coordinator 进程按调用 ID 合流；
    图片失败不取消穿搭成功分支，最终执行 completed、业务 partial。
  - 包 README 增加直接入口，现有 examples 安装规则自动包含教程、配置和脚本，未改 CMake。
- 验证：
  - 修正示例初版缺少 shadow_options 导致 L3 task.begin 校验失败的问题，仅补齐教学配置。
  - 复测 L0/L1/L2/L3 和第二个会话，共 5 条轨迹 capture_complete=true、后台规则评测
    completed，逐条 doctor 结构校验通过；另有初次 L0/L3 教学验证成功记录。
  - 校验文档的 11 个 Bash、17 个 Python、1 个 JSON 代码块语法及全部本地链接。
    业务插入片段明确保留原调用和业务变量，不冒充可直接替换整个函数的完整补丁。
  - 在 build/evolution-no-hooks-install 验证三份示例文件随包安装、内容一致、配置与客户端
    相对路径可解析。测试数据仅位于 build/evolution-no-hooks-example。
- 范围与边界：
  - 未修改 AgentFlow 运行代码、C++ 底座、现有 Hook、业务 API/Key 或正式轨迹与 Memory；
    未提交或推送 Git。测试启动的教学服务已停止，结果保留在 build 下。
  - 小例子使用固定模型/工具回复，不访问供应商 API，不是真实 AgentFlow 或沙箱验收；
    不以分级调用通过声称验证了经验收益、动作拒绝或循环 Stop，也不把空 Memory 当错误。
  - 按仓库探索/影响检查指引核对实际入口；GitNexus impact 未解析 IntegrationService，
    返回 UNKNOWN，未作为无调用者或低风险证明。进一步核对 HTTP 服务和调用链，
    限定修改为文档与隔离示例，没有安装或重建 GitNexus。

### CHG-20260908-155：用通用边界契约解除框架、语言和执行层级假设

- 日期：2026-09-08。
- 用户要求：不是增加若干框架适配器，而是任何具备相应字段与执行边界的程序都能接入；
  同时语言无关、可扩展，交付到 L3。旧字段不合适时可调整，不强行为兼容保留旧语义。
- 对外契约与配置：
  - 新增 evolution.boundary.v1：run.begin/end/abort/status、activity.begin/end/abort/status、
    loop.check/commit、event.record、contract.describe；共用已有 capture.gap/evaluation.retry。
  - C++ 仅是服务实现语言，对外统一 HTTP/UTF-8 JSON。现有原生客户端的 invoke 可直接使用
    全部新操作，无需新增语言专属 Agent 包装或框架适配器。
  - 新入口不需要 Coordinator/Skill、预注册执行角色、step、执行树、父进程常驻或串行调用。
    输入/输出支持任意 JSON；conversation_id 可选，非聊天任务默认以 run_id 分组。
  - boundary-service.json 使用 application_id、policy_scopes，区分应用、策略分区与动态
    actor_id。旧 agent_id/actors 只保留兼容配置语义；冲突配置拒绝，不静默猜测。
  - activity_types 可配置自定义名称对应 model/action/custom 和必填输入 JSON Pointer；
    extensions 与自定义事件保留领域数据，不能覆盖身份/状态或伪造系统回执。
  - 新增 boundary-v1.schema.json、统一 invoke-v1.schema.json，更新 OpenAPI 与生成一致性检查。
    新旧入口共用原身份隔离、事务持久化、请求幂等、容量与隐私检查，不另写一套存储后端。
- 执行与轨迹：
  - 活动彼此独立，可并发、逆序完成，通过显式 caused_by 表达合流/DAG，不把到达顺序当因果。
    派发前登记预期工作，跨进程只传 run_id/activity_id；发送进程退出后接收方仍可结束记录。
  - 普通分支失败只记录该分支；activity.abort 只关闭当前活动并标记采集不完整，其他活动继续。
    run.end 拒绝未完成活动或未回报控制，未知/缺失轨迹隔离而非伪造成业务全局失败。
  - 循环必须显式 loop.check/commit，活动不再隐含一轮循环。各 loop_id 独立，等待本循环内
    已登记活动时不 Stop/Replan；Stop 后旧 control_id 也不能绕过停止状态再次派发。
  - 模型调用逐次提供 binding，可定位消息数组、文本字段、嵌套字段或根字符串；对照
    actual_input 核验，不改写已冻结策略及其 hash/授权/预算。
  - 同次 Replan 的并发模型分别生成回执，先失败的调用不抹掉后续成功观测；宿主回报的循环
    决定不匹配时不能记作 applied。中断收尾不会伪造已观察到模型调用。
  - 标准分析沿明确的间接因果边识别不可重试重复；同一轮多个无效活动按轮次去重，避免
    将动作/episode 数量当作无效 loop 数。评测使用策略分区归属，保留实际执行者元数据。
  - 更新外置标准评测 Prompt：区分活动、循环、策略分区；extensions 作为不可信业务证据。
    新数据沿既有校验、规则/模型评测、批次反思与记忆管理链路处理。
- 保留兼容与使用文档：
  - 原 SessionLog、离线 CLI、SDK、AgentFlow Hook 和旧 Step 接口保留。先前补充的旧接口
    delegate、execution.end/abort/status、execution/delivery 完成模式也保留并做回归。
  - 新 README、FRAMEWORK_GUIDE 以统一契约和插入位置为主，涵盖同步/异步、直接/间接通信、
    非聊天任务、多执行者与多循环；LANGUAGE_GUIDE 各语言示例统一采用新 invoke 操作。
  - 旧教程移至 LEGACY_STEP_GUIDE/LEGACY_DISTRIBUTED_GUIDE，旧 API 参考明确标注兼容范围；
    配置、协议和文档一并纳入独立安装包。不是要求外部开发者先读内部实现。
- 验证结果：
  - 全部 15 组 CTest 通过，包含标准轨迹/旧 SessionLog、策略、记忆、服务、HTTP、旧异步
    委派与新增 BoundaryTransportTests；新服务测试另覆盖自定义字段、模型绑定、独立循环、
    重规划乱序回执、间接因果重试、无效轮次去重、隐私隔离和重启恢复。
  - Python/JS/C/C++/Go/Java/C# 七种原生进程完成同一套新旧协议测试：42 条完整轨迹，7 条
    隐私隔离轨迹；请求与响应通过 JSON Schema 校验，完整轨迹逐条 doctor 通过。
    结果：build/trajectory-evolution/language-clients.23c8p787/test-summary.json。
  - C++ 发起、Python/JS 独立进程接续同一新协议 run，生产进程退出、接收方重叠执行、
    逆序完成、一个失败另一个成功并合流，轨迹 doctor 通过：
    build/trajectory-evolution/boundary-transport.g4dq5caj/test-summary.json。
  - TS 实际 HTTP 链路产生 12 条完整轨迹、12 次本地模型评测接口桩调用；覆盖两用户三会话
    多输入及新增通用协议任务，模型接收到具体内容/extensions，已有记忆实际注入新协议请求。
    结果：build/trajectory-evolution/integration-http.57rpYd/test-summary.json。
    使用可控评测模型响应，未调用付费供应商；本轮该套测试没有运行 Rust 二进制，不冒称已测。
  - build/evolution-boundary-install 独立安装后，新配置与安装版 Python 客户端完成新协议
    L3 调用、轨迹 doctor、自动规则评测与 Memory 初始化；结果位于
    build/trajectory-evolution/installed-service.obb98l0n/test-summary.json。
  - 核对 114 个文档本地链接无缺失，OpenAPI 生成一致性和 git diff --check 通过。
- 范围与边界：
  - 本次运行代码集中在 tools/trajectory_evolution，未修改 AgentFlow C++ 执行底座，未改
    业务 API/Key 或删除正式轨迹/Memory，未提交或推送；保留工作区已有其他修改。
  - 按仓库要求阅读 GitNexus 探索/影响/重构指引并尝试 query/impact；索引落后且关键新增
    符号未解析，返回 UNKNOWN，不能当作图检查通过。另核对 HTTP/CLI/分析/测试实际调用方，
    以局部实现与回归约束风险；未重新安装 GitNexus、未反复尝试 bwrap。
  - 契约不依赖特定框架，但接入方仍必须提供真实边界并使用返回值；没有零代码接入黑盒或
    已实测全部框架的承诺。applied 是宿主观测核验，不是网络抓包证明或进化收益证明。
    L4/GEPA 自动搜索发布、远程业务取消、磁盘保留策略不在本次实现范围。

### CHG-20260908-156：合并使用手册并清理过时文档

- 日期：2026-09-08。
- 用户要求：Markdown 太多且接法混杂，合为一份，并清除过时内容。
- 文档整理：
  - tools/trajectory_evolution 下原有 13 份 Markdown 合并为唯一 README.md，不再要求
    用户在总览、语言、框架、接口、Step 教程与 Runtime 手册之间来回跳转。
  - 按安装配置、各语言客户端、统一接口字段、L0–L3 真实插入点、异步并发、扩展数据、
    自动评测和记忆管理、排错与限制、离线/SDK/迁移、验收重新组织，不是直接拼接旧文档。
  - 删除根目录 FRAMEWORK_GUIDE、LANGUAGE_GUIDE、LEGACY_STEP_GUIDE、LEGACY_DISTRIBUTED_GUIDE、
    INTEGRATION_REFERENCE、INTEGRATION_API、STANDARD_TRACE、POLICY_RUNTIME、SHADOW_RUNTIME
    共 9 份旧手册；删除 examples/agentflow-no-hooks 下 README/RPC_WALKTHROUGH 和
    examples/distributed-agent 下 README 共 3 份重复教程。均指对应 .md 文件。
  - 清除以旧 Step/框架角色为新接入前提的说明及已过时的能力状态；必要迁移提示集中在
    README 第 10 节，明确当前到 L3、L4 尚未实现以及接入方必须提供真实执行边界。
- 配套调整：
  - CMake 安装清单只安装 README，示例代码、配置、客户端与协议文件仍照常安装。
  - OpenAPI 生成器与生成结果的错误处理说明改指 README#operations，仅修改文档描述。
  - 修复独立交付历史规划中的旧手册链接；旧架构图元数据指向当前手册，并注明历史状态。
  - 修正安装后 SDK 头文件的位置说明，避免源码相对链接在安装包中失效。
  - 不删除组件外的私人研究/历史文档；本 HARNESS_CHANGES 继续单独保存变更历史。
- 验证：
  - 组件重新构建成功，全部 15 组 CTest 通过；OpenAPI 生成一致性与 git diff --check 通过。
  - 检查唯一 README 的本地链接、章节锚点及 19 个 JSON/Python/Shell 示例的语法。
  - 新前缀 build/trajectory-evolution/doc-consolidation.JfunEp/install 下只含一份使用
    Markdown，配置与客户端仍可使用；安装版完成 L3 接入、doctor、自动规则评测和 Memory
    初始化，未调用供应商 API。结果位于 build/trajectory-evolution/installed-service.ibxfqg_7。
- 范围与恢复：
  - 删除前已将原 13 份文档压缩备份到
    build/trajectory-evolution/doc-consolidation.JfunEp/previous-docs.tar.gz，可按原路径恢复；
    备份是本地构建产物，不进入交付文档或安装清单。
  - 本次不改 C++/Python 执行逻辑、API 字段语义、业务配置、凭据、轨迹或 Memory；保留已有
    工作区修改，未提交或推送 Git。
  - 按仓库指引执行 GitNexus impact；文档与生成器目标未被索引解析，结果 UNKNOWN，未视为
    低风险或图检查通过。另核对实际引用、安装依赖与本地回归，未安装或重建 GitNexus。

### CHG-20260908-157：本地低延迟入口、后台采集与可控进化边界

- 用户要求：主 Agent 不等待进化组件的 HTTP、落盘或评测；本地开销尽量小，进化必须可控，
  并为后续 GEPA 保留清晰的接入位置。接受资源不足时显式降级，不伪造完整轨迹。
- 运行代码集中在 tools/trajectory_evolution：
  - 新增 LocalRuntime、local_c C ABI、Python ctypes 与 JS/TS N-API 客户端。应用启动时创建一次，
    业务边界只调用内存状态机；落盘、评测、反思、记忆更新由后台处理。支持接口仍为
    evolution.boundary.v1 的 L0–L3 业务边界，不要求特定 Agent 框架或聊天式主循环。
  - 复用 boundary_runtime 的决定、参数绑定和真实回执校验，仅增加可选的已编译 PolicyRuntime
    参数，避免同一任务逐次重建策略；原 HTTP 路径继续使用原行为，不改 AgentFlow C++ 底座。
  - IntegrationService 增加仅供本地后台调用的配置/记忆读取、终态持久化与数字指标写入。
    不向本地配置快照导出 API Key 或用户凭据，不减弱原有隐私字段、正则及已知秘密检测。
  - 有界任务槽位和内存预留，单请求默认 64 KiB、保留预算 64 MiB、待落盘任务最多 64 个；
    继续服从部署配置中的请求/活跃任务上限。预算不是进程 RSS 或硬实时上限。
  - 同一任务固定策略快照，保留用户/scope/版本授权及 actual_input、executed、loop.commit
    真实回执。新增 set_enabled 暂停/恢复：撤掉进化增强，不绕过宿主权限、业务循环上限，
    不取消兄弟分支或撤回已经发生的副作用；跨切换的旧任务隔离，新任务才重新启用。
  - 容量、竞争、隐私或内部错误显式返回 degraded/capture_complete/policy_applied 标记；
    业务使用原输入和原调度，有缺口的运行 quarantine。后台磁盘/模型失败不阻塞主链，
    通过内存 metrics 与发生降级时的 local-runtime 状态文件观测。
- 本轮验证时发现并修正：
  - 后台扫描短暂锁住活跃任务，造成正常顺序调用偶发 local_busy：改为读取不可变开始快照，
    后台不再例行锁活跃任务，结束后才转移终态。没有以增加业务等待或吞掉缺口掩盖问题。
  - 中断恢复只在内存隔离、状态文件仍未结束：校验身份与路径后持久化恢复终态。
    本地后台持久化同时复用终态缓存回收，避免隔离任务/关闭自动评测时绕过原 HTTP 清理入口；
    只回收可从磁盘读取的内存终态，不删除轨迹文件。
  - 槽位复用后的旧请求 ID 可能重用持久化 run_id：增加实例内生成序号，保留窗口内幂等，
    过期窗口不覆盖历史运行。忙碌的重复 begin 不再误创建第二个运行。
  - 原生接口自描述不再宣称支持需后台 I/O 的 evaluation.retry，并返回实际本地请求上限。
    能明确归属为新任务 begin 的拒收不再污染已有其他任务；无法归属的损失仍保守隔离。
  - Python 进程边界与 ctypes 返回类型、JS 非法字段/非有限数字和 N-API 资源释放补齐。
    Node 测试采用与构建工具链匹配的 C++ 运行库，README 说明 GLIBCXX 加载不兼容的处理。
- 可控进化/GEPA：
  - README 第 8.4 节明确“入库不等于上线”，默认不自动授权经验，运行中的策略不可热换，
    实际应用不等于收益证明。GEPA 后续位于后台候选优化阶段，需固定试验数据、预算、
    留出评测、安全门槛和发布授权；线上只读已发布快照，不等待搜索。
  - 本次未实现 GEPA 搜索器、候选发布登记、流量灰度或自动回滚，不冒称完成 L4。
- 验证与本地产物：
  - 17 组 CTest 全部通过，保留 HTTP、旧 SessionLog、标准轨迹、策略与记忆回归。
    新增原生跨语言回归在修正后台竞争后连续通过三次。
  - C++ 业务线程禁文件/网络/睡眠系统调用后完成 40 轮 L3、323 次边界/状态调用，轨迹校验
    与规则评测、Memory 流程通过；最终完整回归中的中位数约 1.19 ms、P99 约 1.34 ms，
    最大约 5.05 ms。结果：build/trajectory-evolution/local-runtime.6ul0WW/test-summary.json。
    这是小请求、可控工具负载的测量，不是任意输入/并发量的时延保证。
  - Python/JS 原生测试覆盖两用户三会话九次任务、反向完成、一个分支失败不影响其他分支、
    未授权经验不注入、真实输入不符产生失败回执、容量、隐私拒收、暂停及进程中断恢复。
    磁盘不可写时一轮业务边界约 49 ms；模型评测被挂起时两轮约 68 ms，均未等待故障解除。
    结果：build/trajectory-evolution/local-clients.v7rvl2it/test-summary.json。
  - 真正同时写同一 run 的 4 个生产者触发显式降级，结果隔离而非完整学习样本；此用例不是
    “并发无损通过”。常规两用户/多 run 和逻辑重叠活动测试无降级。
  - 独立前缀 build/trajectory-evolution/local-install.kSYsw1 安装版原生/Python 与 HTTP/Python
    均完成 doctor、规则评测和 Memory 初始化；分别见 installed-local.3stryuav 与
    installed-service.3nh21mbc 下的 test-summary.json。所有测试使用假凭据和可控本地模型，
    未调用用户付费 API，测试产物没有实际 Key。
- 交付边界：
  - 原生 run.end 仅进入内存，persisted=false；不得在业务答复前 drain、close 或等待评测。
    关闭阶段可以等待当前评测超时。进程崩溃可能丢失未落盘尾部，不能承诺零丢失。
  - 原生句柄和幂等窗口属于创建客户端；跨进程续写同一 run 仍用 HTTP，或由长生命周期
    所有者收取回调。真正同 run 多线程争锁会降级；无损接入需在已有调度器中依次提交短记录，
    业务工具仍可并行。原生库目前是 Linux/POSIX，不冒称已交付所有系统的原生 SDK。
  - 唯一组件 README 已更新配置、接法、管理与限制；未新增分散说明文档，未改业务 API/Key、
    用户现有轨迹或环境，未提交/推送 Git，保留工作区已有其他修改。
  - GitNexus impact 对新增关键符号仍为 UNKNOWN；detect-changes 显示已有索引的 35 文件、
    45 符号及 low/0 processes，但不能覆盖未解析的新入口，未据此宣称全图无影响。
    已补查实际调用与完整回归；未重建/安装 GitNexus，未重试 bwrap。

### CHG-20260908-158：补充进化算法与数据流详解

- 用户要求：在使用手册之外新增一份算法说明，详细解释采集、日志处理、评测、进化记忆生成、记忆管理、策略生成与更新。
- 新增 tools/trajectory_evolution/ALGORITHMS.md，按实际源码组织 15 节，包含事件因果校验、并发配对、内容感知评测、
  候选支持度公式、元反思、字节三元组 Dice 短名单、四类记忆关系、全库整理、冲突成组淘汰与乐观文件更新。
  补充经验资格筛选、中文双字关键词检索、版本授权、L1–L3 状态机和回执、配置生效时机及贯穿全链路的假设案例。
- 明确区分当前实现与 GEPA/L4 规划；说明入库、PROPOSED、版本授权、实际应用和收益证明不是同一个状态。
  如实记录通用诊断范围、本地竞争降级、截断、无向量检索、记忆与历史产物保留、非自动规则发布等边界。
- README 新增算法文档入口，CMake 安装列表增加该文档；未改运行算法或 AgentFlow C++ 底座，未触碰 API/Key、环境和现有轨迹。
- 验证：15 节/15 个锚点、50 个本地与锚点链接、JSON 示例和代码围栏检查通过；CMake 重新配置通过，独立前缀
  build/trajectory-evolution/algorithm-docs.knDrqM 已安装并逐字比对 README 与 ALGORITHMS。仅文档与安装项变更，未重跑模型或业务测试。
- 影响分析：GitNexus 现有索引落后 9 个提交，查询未解析相关入口，CMake 目标 impact 为 UNKNOWN；不将空结果视为无影响，
  已直接核对实际源码、配置与安装调用。未重建/安装索引，未提交或推送 Git，保留工作区已有其他修改。

### CHG-20260908-159：自动进化改为持久化增量批次，消除重复合成与空闲全库刷新

- 用户要求：已分析日志不重复分析；控制触发频率、防止重复启动积压；空闲不频繁刷新，兼顾并发、失败和重启边界。
- 范围仅为 tools/trajectory_evolution 及本文；未修改 AgentFlow C++ 底座、采集协议和业务控制接口，未新增依赖。
- integration_service.cpp：完整 run 只入队，待学习索引与活跃业务容量分开。按用户分组，默认满 20 条或最老样本等待
  10 分钟后具备执行条件；一次最多 32 条/32 MiB，批次间至少 60 秒。单个服务仅一个评测批次运行，后到 run 留待下批。
  learning 配置可调整数量、等待、间隔、容量和故障退避；单轨迹、元反思与整理共用模型预算，严格模式失败也预留调用预算。
- 增加 learning/schedule.json 和 learning/jobs 的轻量检查点，记录冷却/退避、冻结成员、阶段、错误和候选 PB；
  runs.learning 记录来源/报告指纹、分析状态和重试代次。重启复用报告，入库重试复用候选，同一批次保持记忆入库幂等。
  完成批次先保存终态，再补齐各 run；重试代次用于防止旧批次在恢复时重新接管已单独重试的样本。
- 一个报告损坏、分析失败或某用户轨迹发布失败，不拖住其他有效样本/用户；failed 不自动无限重试，沿用现有
  evaluation.retry 管理操作。共享配置/存储故障保存稳定错误码与退避时间，重启不绕过退避；未改变 HTTP 状态响应字段。
- 模型请求发出、报告未落盘即中断时，标记 analysis_interrupted_unknown_outcome，要求显式重试；成功缓存丢失或被修改
  时不自动覆盖、不默默重新付费。候选模型阶段同样保守恢复。此机制不承诺模型服务商侧 exactly-once，也不改变离线显式重评。
- evolution_pipeline.cpp：新增接收 Memory 的 synthesize_reports 重载，保留旧入口；只用新报告合成候选，跨批证据在记忆中累积。
  新报告仍检查已有记忆的反证，只有新增反证才带回该候选，不再重复扫描最近 32 份历史报告。recent_reports 仅兼容接收。
- local_runtime.cpp / integration_http.cpp：学习线程改为事件/截止时间唤醒；记忆仅在组件提交新修订后重新加载，新增
  memory_refreshes 指标。修正持久化通知与本地槽位发布的先后竞态，保证完成状态可以被后台刷新；主业务边界不新增 I/O。
- 策略含义：memory_version 只是进程内缓存失效标记；Memory revision 和 run.policy/policy_id 仍按原机制保存。
  本次没有引入 GEPA 父子策略谱系、自动策略晋升或运行中策略替换；本地未落盘尾部依然可能在崩溃时丢失。
- 文档：README 8.2 补充配置、调度、重试和容量边界；ALGORITHMS 更新增量合成、检查点、缓存通知与版本含义。
  阈值/等待表示启动资格，不保证处理完成期限；持续输入高于处理能力仍需容量规划。外部修改 Memory PB 需重启本地客户端加载。
- 验证：Release 构建成功，CTest 18/18 通过。新增 LearningScheduleTests 的 10 组场景覆盖数量/最大等待触发、跨重启冷却
  与配置故障退避、用户隔离、后台阻塞时新任务继续、强杀后的未知调用结果、报告/候选复用、幂等恢复、缓存丢失拒绝重评、
  单样本/发布失败隔离、模型预算截批、空闲不刷新和真正更新后刷新；增量反证、旧 HTTP 契约与无 I/O 主链回归均通过。
  测试记录：build/trajectory-evolution/learning-schedule.hclq3bev/test-summary.json，26 次本地假模型请求，付费 API 调用 0 次。
- 独立安装前缀 build/trajectory-evolution/learning-install.3EAqkv 的原生/Python 和 HTTP/Python 测试均通过，轨迹 doctor、
  规则评测、记忆初始化与假凭据未落盘检查通过；结果分别位于 installed-local.fqk_x9vr 和 installed-service.9c5dmhzu。
  配置 JSON、文档链接/围栏/JSON 示例、git diff --check 通过。未触碰真实 API/Key、环境或既有业务轨迹，未提交或推送 Git。

### CHG-20260908-160：原有总览图与 A4 运行图同步当前增量学习和策略机制

- 用户要求：修改之前绘制的图，使其对应当前方案；保留原目录和文件名，不引入新的运行流程。
- 更新 docs/trajectory-evaluation/figures/trajectory-evolution-overview 与 trajectory-evolution-runtime-a4
  两套图件的 SVG、PNG、PDF。总览采用“当前执行 / 增量学习 / GEPA 规划”三栏；详细图保留 A4 横向单页。
- 将 L0–L3 改为已实现，使用现有 run.begin/end、activity.begin/end、loop.check/commit 边界名称；
  展示宿主并发、按 ID 合流、单分支失败不覆盖全局结果、本地先返回业务与后台异步落盘的区别。
- 增加按用户隔离的新样本队列、20 条/10 分钟触发资格、60 秒批次间隔、32 条/32 MiB 上限、
  冻结批次、共享模型预算、分析和候选检查点、单样本失败隔离、显式重试与跨重启退避。
- 分开绘制持久化 Memory revision、进程内 memory_version 缓存计数和 policy_id/run.policy 运行快照，
  用 Run A/R10/P-A、后台更新 R11、新 Run B/P-B 的例子说明旧 run 不换策略。
  GEPA 父子谱系、真实对照、留出验证与晋升/撤回仍单独标为规划，不把缓存计数解释为进化代数。
- 同步 INDEPENDENT_DELIVERY_AND_L4_PLAN.md 的图注、读图顺序和交付说明，删除过时的“仅 L0 已验证”图注、
  旧图端口表和示意成绩数字；图中课题收益目标仍明确为未实测目标。
- 验证：SVG XML、图件链接、关键状态与版本标签检查通过；按实际 Noto Sans CJK 字体检查 252 处文字，
  未检测到宽度溢出或文字间覆盖，并完成渲染后的目视复查。PNG 分别为 3508×2207 和 3508×2480；
  两份 PDF 均为单页，详细图 MediaBox 对应 297×210 mm。git diff --check 通过。
- 仅更新图件与相关 Markdown；未修改运行代码、配置、API/Key 或既有轨迹，未调用模型、未提交或推送 Git。

### CHG-20260908-161：增加 Hermes 式离线评测任务集构建，为 L4 提供固定评测材料

- 用户要求：借鉴 Hermes 的评测集做法，在进入 L4 之前准备可复用的任务集；保持最小改动、框架和语言无关。
- 范围仅为 tools/trajectory_evolution 和本文。增加 dataset.cpp / dataset_cli.cpp、公开 dataset.h、独立
  evaluation_dataset.proto；现有 main.cpp 仅增加 dataset 子命令分派和帮助，CMake 注册新增源文件、协议和测试。
  未修改 AgentFlow C++ 底座、Harness、在线边界、记忆管理、后台调度或现有策略生效机制，未新增第三方依赖。
- 三来源：golden 导入人工 JSONL；synthetic 读取能力说明分批调用模型出题；traces 读取标准 JSON/PB 或兼容 SessionLog。
  轨迹提取复用现有 Evaluator 的结构校验、内容脱敏和限长，无 adapter 准备证据不额外调用评价模型；
  对已经完整掩码的敏感字段删除其键值，再通过共享隐私门槛。原始模型回复、凭据和私人路径不进入数据集或诊断。
- 提示词外置 config/prompts/dataset-system.txt；config/dataset.json 控制来源、分组种子、配比、总量、逻辑模型调用
  预算与出题输出 Token。模型 URL/模型名/Key/超时/thinking/传输重试复用现有 runtime 配置，支持已有环境变量，
  不写入真实 Key、不自动读取 .env。任务 budget 与模型 HTTP timeout 明确区分；本模块不执行任务或实施宿主预算。
- 模型只能返回任务、后续输入、标准、类别、难度和声明式初态；不能自批 reviewed、指定 split 或伪造来源。
  模型生成和轨迹派生任务默认为 pending、训练用途；golden 重新导入保留原有来源与学习暴露，不把旧样本伪装成新留出。
- 以归一化输入、后续输入、初态检测精确重复，合并场景组和来源；冲突标准、重复 ID 对应不同任务、组内划分冲突拒绝。
  用 split_seed/group_id 的 SHA-256 桶做稳定划分，已暴露给学习的组和 learning_groups 全部限制到 train；
  空集合警告，不用事件或同一会话不同轮次拼凑独立验证。近义样本与外部使用历史仍需要导入方标记，未宣称语义去重完成。
- 冻结输出 dataset.pb、train/val/holdout.jsonl 和最后写入的 manifest.json；公开 JSON Schema 与 Protobuf 协议。
  task_version/dataset_version 是评测材料内容版本，不是策略版本。check 交叉核对文件哈希、PB/JSONL、分组与版本。
  目标目录存在时提前拒绝；输出目录 0700、文件 0600。失败可能留下不能通过 check 的不完整目录，不覆盖或删除旧输出。
- 增加 24 条中文声明式用例：并发与逆序合流、依赖、网络重试、权限、循环、前置条件、多轮更新、会话隔离、图片/视频、
  产物真实性、代码回归、数据清洗、隐私与幂等。默认 21 个场景组、14 train / 5 val / 5 holdout，全部 pending。
  实际任务集位于 trace_output/datasets/agent-evaluation-starter-v1；不是付费模型生成，也不是已执行的 Agent 实验。
- 使用说明集中更新 README 10.4，ALGORITHMS 15.3 补充数据算法；解释训练/验证/留出、来源继承、审核、各层预算与失效边界。
  被测 Agent 不能收到整条用例中的 expected_behavior 和故障安排；真实执行器、产物判定、基线/候选对照仍属后续 L4。
- 验证：Release 完整构建成功；CTest 19/19 通过（原有 18 组全部通过，新增 EvaluationDatasetTests 含 32 项测试）。
  新测试覆盖稳定版本、同组隔离、已学习数据拒绝留出、重复冲突、部分失败、不可覆盖、篡改/缺失文件/符号链接、敏感值、
  中文与自定义字段、模型输出伪造/截断/无效 JSON/HTTP 失败、预算、初始化失败重入、标准轨迹和旧 SessionLog。
  新测试记录 build/trajectory-evolution/dataset-tests.8w34edb4/test-summary.json，16 次本地假模型请求，付费调用 0 次。
  独立安装目录 build/trajectory-evolution/dataset-install.gZNgda 的 CLI 构建/校验成功，生成同一内容版本；
  单独 C++ 消费者仅通过安装包 TrajectoryEvolution::core 与 dataset.h 读取任务集成功。
  24 条源任务与 24 条冻结任务通过 JSON Schema；Markdown 围栏与 git diff --check 通过。
- 编辑前对 CLI 入口进行 GitNexus impact，现有索引不能解析该入口；未将 UNKNOWN 视作低风险，沿用已知限制并核对真实调用范围。
  未重建索引、未提交或推送 Git，保留工作区已有其他改动。隐私检查不承诺识别所有个人信息，reviewed 不代表 L4 已就绪。

### CHG-20260908-162：补齐独立任务评测执行、规则/模型验收和报告，不启用进化

- 用户明确要求评测集不是只有题目，须完成可实际运行的评测相关功能，暂不做进化。新增功能集中在
  tools/trajectory_evolution；不修改 AgentFlow C++ 底座、Harness、在线 Hook、Memory、策略或学习调度。
- 增加 task_evaluation.cpp、evaluation_process.cpp、task_evaluation_cli.cpp 和公开 task_evaluation.h；
  原 dataset CLI 仅增加 run 分派，main 仅同步帮助。沿用现有 evaluation_dataset.proto 添加 TrialCheck、EvaluationTrial、
  TaskEvaluationReport，不修改已冻结任务字段和版本语义；不增加 C++ 第三方依赖。
- dataset run 校验冻结数据集、集合选择、审核状态、规则覆盖和预算后，按任务×重复×显式版本执行。
  每次单独工作目录、唯一 trial_id、环境变量白名单；用 stdin/stdout JSON 调用任意语言的受信宿主程序，
  同步/异步、多 Agent、队列/DataBus 内部实现由宿主等待并关联终态。不给被测对象发送 rubric、集合划分或判分规则。
- Linux 进程监督使用 posix_spawn 和参数数组，不经过 shell；非阻塞读写避免大输入/输出互等，限制 stdout+stderr 总量，
  超时与取消终止进程组，异常路径清理子进程。工作目录隔离不冒充安全沙箱，外部数据库/远端任务/权限隔离仍由宿主负责。
- 规则支持 JSON Pointer 的 exists/equals/contains/at_most/at_least；独立读盘验证 JSON/text/PNG 产物，
  拒绝越界、符号链接、缺失、敏感文本和超限内容。PNG 内置检查只证明文件头和尺寸，完整解码与视觉语义不能伪造。
  声明产物缺失属于业务验收失败，不进入环境故障未评分桶；执行状态和业务判定独立保存。
- 模型验收复用既有 ModelAdapter 和 runtime 配置，提示词外置 task-judge-system.txt，接收具体安全证据和成功标准。
  规则失败不再调模型覆盖；模型故障、截断、引用错误、预算耗尽保持 error，不确定保持 uncertain，不默认为通过。
  真实复跑暴露了非 JSON 回复与不存在的引用，已增加显式证据路径白名单、完整 JSON 围栏兼容及可配置有界格式修复。
  max_repair_attempts 默认1、范围0–3；每次计入总模型预算并记录 attempts，只重试判分，不重跑宿主或重复生成图片。
- 可选标准轨迹校验身份、因果闭合、结束状态和最终输出一致性；无效轨迹保留经隐私校验的诊断证据，不送模型。
  不把 Episode 数当 loop 数。缺少轨迹/计数保留不可观测标记，规则无效 loop 统计明确为下界。
- 每次试验落 trials/<trial_id>.json，汇总生成 report.md、report.json、report.pb、results.jsonl，最后写 manifest。
  按版本/类别/集合统计评分覆盖率、明确评分通过率和全量试验通过比例，配对比较按 task_id+repetition，不按返回顺序。
  complete 仅指计划试验均有终态；失败/取消也有记录。运行中断保留已完成条目，不自动重跑、不覆盖旧目录。
- 模型/步骤/任务时间预算分别配置；不把 Agent API 调用次数与评测模型调用预算混为一谈。
  C++ 不原样落 stdout/stderr；敏感响应不发模型，只有通过检查的产物被保留。不能承诺识别所有编码秘密或图像隐私。
- 增加三条已审核可执行示例及独立 Agent Plan 工具型宿主：并发数据源失败隔离汇总、三轮预算修改、图片生成/下载/完整解码。
  示例使用用户指定 Agent Plan 接口，文本 deepseek-v4-flash，图片 doubao-seedream-5.0-lite；严格保留 /api/plan/v3，
  不回退普通按量路由。配置可修改、thinking disabled、模型 HTTP timeout 30分钟；密钥仅从进程环境读取，无真实密钥入库。
  示例图片解码使用 Pillow；该依赖不进入 C++ 核心。通用程序不依赖这两个模型或火山服务。
- 增加 TaskEvaluationTests，最终28项回归覆盖通过/失败/未评分、并发逆序与版本配对、后代超时清理、取消、预算、
  环境白名单、凭据拦截、实际产物、无效/错归属轨迹、模型异常/引用/格式修复和不可覆盖。完整 CTest 20/20 通过。
  真实执行保留所有三轮结果，不删除中间失败：首轮3/3通过；第二轮1通过、2评测协议错误；修复后最终3/3通过，
  模型验收3次均一次成功，实际图片2048×2048可解码。最终报告 trace_output/evaluation/agentplan-task-eval-20260908-verified/report.md。
  最终三份轨迹有效；三轮32个结果文件及模块源码的真实密钥字面值扫描无命中，最终报告文件哈希、JSON Schema、
  Markdown围栏和图片解码复核通过。此验证不是全量 AgentFlow 业务基线，也不代表进化收益已达标。
- README 10.5 集中说明命令、所有语言的宿主入口、异步关联、预算、失败分类、字段和安全边界；ALGORITHMS 15.4
  说明算法，移除对应“真实执行与固定版本对照尚未实现”的过时内容。L4 GEPA搜索、自动选择、发布/回滚仍未实现。
- 修改前进行 GitNexus impact；新增入口与帮助入口未被索引正确覆盖，结果 UNKNOWN，未据此宣称无影响。
  核对实际 CLI/CMake/协议调用范围并运行完整回归；未重建索引，未提交或推送 Git，保留其他既有工作区改动。

### CHG-20260909-163：统一按用户学习、公共经验自动管理与跨库安全编译

- 按用户要求统一离线和在线学习：新增 learn 命令和受信本地 import_file API，复用 IntegrationService 的原有队列、
  单轨迹评测缓存、候选检查点和私人入库流程。HTTP、原生后台、日志导入使用相同服务配置与 data_dir，
  不再从混合用户日志直接生成一份可注入的公共库。旧 evaluate 目录模式仅允许 --no-memory 诊断，不再写 Memory；
  单文件评测和 Pipeline SDK 保留。原日志不改动，已有旧版混合 Memory 不删除、不自动升级为公共经验。
- 目录统一为 by-user/<user>/memory 保存私人库、同级 memory 保存可用公共经验，learning/shared-memory.json 保存
  内部管理账本；每个用户的 shared-relations.json 只包含该用户的跨库关系。公共 PB 为 SharedExperienceStore，
  只含应用、修订与经验 ID/scope/条件/建议，不携带用户身份、私人正文历史或来源候选 ID。
- 新增 shared_memory.cpp/.h：同一后台工作者自动执行泛化提炼、单独复核、独立证据计票、跨库比较、争议暂停、
  支持撤回、恢复与淘汰。默认至少两名用户、每名至少两次完整且已评测 run；核验来源身份和实际内容指纹，
  已记录经验注入的轨迹不作为推广的独立证据，同用户重复记忆只计一票。没有可核验来源则保留私人用途。
- 公共经验状态 pending_support/published/disputed/reserve；任一当前有效跨库冲突使公共经验全局暂停，
  合格私人经验仍可使用。未知关系保守不合用公共，不假定独立；已失效关系按双方内容版本清理。
  更换评审模型/提示词不等于推翻既有反证，保留未消解冲突；坏用户文件/取消鉴权不阻塞其他用户且不自动清除反证。
- 默认最多 64 条可用公共经验、128 条工作集，允许新候选积累独立支持后替代旧条目；各用户支持单独计算新鲜度，
  默认 30 天没有新核验支持则不计票。长期无新证据的条目退出工作集，有界保留淘汰摘要；来源和关系缓存也有限制。
  账本读写采用同一 256 MiB 硬上限，避免允许写出的合法默认缓存无法在重启时读入。
- 公共管理按变更用户 FIFO、单周期模型预算与最小间隔执行，默认每周期最多 8 次逻辑调用、至少间隔 60 秒。
  无新工作时只等待最早证据到期，不全库轮询；缺模型不空转。有界失败重试、调用前检查点、重启恢复与公共投影修订核对。
  未持久化的模型响应无法保证供应商 exactly-once，公共审核中断重试可能再次计费；此限制已写入文档。
- 同一目录以服务锁约束单写入者；learn 不和正在使用该目录的服务/原生进程同时写。支持标准 JSON/PB 副本幂等复用，
  同身份内容不同拒绝覆盖；非法身份、跨应用/未配置用户、不完整轨迹、凭据字段拒收。SessionLog 使用原有专用评测器，
  不误走标准轨迹解析。导入增加递归敏感字段检查，避免只扫序列化文本而遗漏任意格式的敏感键值。
- 编译器保留私人所有权门槛，单独接收受信公共投影；对关系按双方版本建立索引，匹配失败不放行。
  运行时优先合格私人、跨库重复只注入一份；先过滤授权再使用 top-K，修复未授权私人条目挤占公共名额。
  快照预算包含关系字节，裁剪时同步去掉孤立关系。本地后台原子更新私人＋公共视图，业务边界不新增模型/磁盘/网络调用，
  已开始 run 保持固定策略；公共撤销影响后续 run，不冒充能追回已发送上下文。
- 端到端测试发现并最小修复原 finalize_candidate 的 Episode 去重：键由 episode_id 改为
  user/conversation/dialogue/episode_id，防止不同 run 同名 step-1 被少计支持。修复后台私人写入容量可能大于运行时读取容量：
  统一入库上限取 evolution、run 与 shadow 读取上限的最小值，不写出下一轮无法载入的私人库。
- 增加认证用户范围内的 memory.status 及 learn 工作区状态汇总，更新生成的 JSON Schema/OpenAPI；管理接口不放入原生热路径。
  三份公共管理提示词外置，sharing 配置可替换，复用现有模型与环境变量密钥；未写入真实密钥、未改模型供应商配置。
- 新增 SharedMemoryTests、UnifiedLearningTests：固定模型响应与实际文件/服务进程验证多用户推广、去重、冲突恢复、
  容量替代、独立证据过期、版本失效、模型异常、隐私、坏用户隔离、标准 JSON/PB 重复导入、在线实际注入和旧 SessionLog 兼容。
  全量 CTest 22/22 通过，包含低延迟原生无 I/O 约束、异步/跨语言、现有记忆管理和任务评测回归。
  这是机制回归，不是实际供应商模型的语义正确性、隐私零泄露或业务收益证明，也未实现 L4 GEPA 自动发布。
- README 第 8、10.1 节和 ALGORITHMS 第 8.5 节集中说明目录、命令、控制开关、算法、恢复与边界，不额外拆分说明文档。
  代码修改限于 tools/trajectory_evolution，不修改 AgentFlow C++ 调度底座、Harness 或业务 Hook。
  GitNexus impact 对新入口和 finalize_candidate 为 UNKNOWN；按已知索引限制核对实际调用链，不把空图当低风险。
  未提交/推送 Git，未删除用户数据或既有产物。

### CHG-20260909-164：组件内分章手册、配图与默认开启两级记忆

- 按用户要求，将交付文档集中到 tools/trajectory_evolution/docs/，不放在 AgentFlow 根目录的 docs 中。
  共 24 个专章，覆盖安装配置、快速开始、协议、原生/HTTP 接入、同步/异步/多 Agent、采集评测、反思、
  私人/公共记忆、策略应用、学习调度、持久化恢复、基线评测、安全运维、扩展迁移与 GEPA 能力边界。
  每章提供目标、设计思路、具体使用与算法细节、实现定位和相邻章节导航，不依赖个人远程开发环境。
- 公共＋私有记忆专章集中解释晋升资格、独立证据、去重、冲突导致全局暂停、私人优先、关系版本失效、
  支持撤回、恢复、TTL、容量淘汰、后台预算、重试检查点、持久化字段和状态接口，并列出已知限制。
  明确 published/applied 不等于已证明进化收益，L4 GEPA 自动搜索与发布仍未实现。
- 将原 README 与 ALGORITHMS 的有效内容迁入对应章节，入口文件改为导航并保留旧显式锚点。
  移除省略 sharing 默认关闭、跨用户共享尚未实现等过时描述；不改写历史变更记录。
  特别区分 HTTP 常驻服务与原生对象生命周期，说明 Coordinator 退出、DataBus 派发和最终汇总的真实接入条件。
- 新增 51 张可编辑 SVG，沿用浅色分区、蓝绿紫配色和圆角卡片风格；包括架构、时序、状态转换、
  目录、时间线和字段对照，各章至少两张，公共记忆五张。配图无需 Mermaid 插件或联网资源即可显示。
  对全部图片进行实际渲染、字体宽度和画布边界检查，并复查中文断行与时序编号遮挡。
- 功能修改仅将 shared_memory_options 的 enabled 默认值改为 true；省略配置和空对象同样启用，
  显式 enabled=false 保持关闭。auto_approve_experiences 默认仍为 false，不扩大经验自动注入授权。
  补充默认推广和显式关闭不调模型、不修改账本的回归；未改 AgentFlow 调度底座、业务 Hook、环境或供应商配置。
- CMake 安装包含 docs 和 SVG。新增 DocumentationTests 检查 Markdown 围栏、本地链接与锚点、
  交付包路径边界、章节完整性、SVG 安全属性和可见标题，防止排版过程丢失文字。
  全量 CTest 23/23 通过；24 章、51 图、455 个本地链接通过源码及安装包检查。
  独立安装后实际运行服务、采集、后台规则评测与记忆初始化通过，供应商模型调用为 0。
- GitNexus impact 对相关新增入口仍为 UNKNOWN，按已知索引限制核对实际归一化调用链与测试入口，
  不将空图视为低风险。未重建索引，未调用付费模型，未提交/推送 Git，保留既有工作区改动和用户数据。

### CHG-20260909-165：清理图表中的否定式提醒文案

- 按用户要求，清理自进化组件手册的图片和表格，删除专门列举不能代表、不能冒充等内容的说明列；
  其余相关文字改为对象含义、状态、处理方式和使用条件，保留已有功能与技术语义。
- 同步更新 SVG 标题、正文、无障碍说明、章节图注和图集索引；沿用现有配色与版式，并按列数重新排版。
  正文中的技术边界说明保留，L4 仍标明后续规划；不修改程序、配置、接口或运行行为。
- 运行文档链接、锚点、SVG 结构与可见标题校验，并对配图进行渲染和文字宽度检查。
  GitNexus 对文档目录的影响查询为 UNKNOWN；补充核对 CMake 安装、文档测试和 Markdown 引用。
  本次仅修改组件内文档及本记录，未提交或推送 Git。

### CHG-20260909-166：更新 L0–L4 等级图文件与引用

- 针对用户反馈，单独复核“L0–L4：新增能力与真实责任”图，确认仅保留三列，统一列名为等级、接入位置、新增能力。
- 将图片改名为 01-level-capabilities.svg，并更新概念章节与图集引用，避免继续使用同名旧预览。
  原图片内容保留在新路径，未新增一份重复源图；其他历史图件不改动。
- 影响查询为 UNKNOWN，按现有索引限制核对实际文档引用；重新校验文档链接并渲染图片。
  本次只改图件及文档，不修改运行代码、接口或配置，未提交或推送 Git。

### CHG-20260909-167：按交付手册风格复核全文与配图

- 按用户要求删除概念章节中列举能力缺口的整段，以及公共记忆章节中重复的保证与限制清单。
  通读组件 README、算法导航和 24 个专章，将内部审查式提醒改为功能定义、配置条件、操作步骤和故障处理。
  保留身份归属、授权、并发、隐私、持久化和错误状态的技术语义，不改变使用条件或默认行为。
- 同步清理正文、表格、SVG 可见文字、无障碍说明、图注和图集索引中的同类措辞。
  等级图标题统一为“L0–L4：接入位置与能力分级”；沿用已有配色、版式和图件路径。
- 完整闭环章节按采集、学习、授权、应用、回流与实际示例组织；统一策略评测与 GEPA 放入后续规划，
  明确规划中的评测覆盖 L1–L3 策略更新。此次仅整理说明，未实现或变更策略发布、评测门槛等运行逻辑。
- 修复评测算法列表的编号和重复说明，核对产物校验步骤与现有实现一致。
  DocumentationTests 通过，24 章、28 个 Markdown、51 张图和 459 个本地链接通过检查；
  全部配图渲染及文字宽度检查通过，并目视复核等级图和控制范围图。章节列表编号检查通过。
- GitNexus 文档影响查询为 UNKNOWN，按既有索引限制核对安装规则、文档测试和实际引用。
  修改限于组件文档、配图及本记录，未改运行代码、配置、接口或用户数据，未调用模型，未提交或推送 Git。

### CHG-20260909-168：复用真实 Agent 评测、后台版本准入与记忆状态反馈

- 在 `tools/trajectory_evolution` 内新增 AgentEvaluationService，复用既有 task_evaluation 执行器、固定任务集、规则/模型判分和报告。
  L0 测原始 Agent，L1–L3 测完整冻结策略；同一提交/查询接口供后续 L4 搜索器调用，本次未加入 GEPA 搜索循环。
- 行为版本绑定 Agent 部署修订、用户、等级、经验快照和控制规则，独立保存候选及父版本。
  宿主必须实际加载候选并返回 loaded_version；轨迹保留自身 run ID，通过 run.start 上的 agent_version_id/evaluation_trial_id 关联试验。
  完整性、所有权、版本、输出、隐私与产物检查继续复用现有验证，错误及证据不足不作为成功发布。
- 新增可选 policy_evaluation_file 配置，在后台独立线程与子进程中执行任务，对照质量、覆盖率、配对样本及可选时延/token/无效 loop 门槛。
  前台使用缓存中的已发布且仍兼容版本；无可用版本时按 L0 原始逻辑继续，已有 run 不切换版本。
  旧配置不设置此字段则保持原授权行为；AgentFlow C++ 调度底座未改动。
- 后台作业有持久化去重、启动冷却、每小时预算、有限并发、取消、显式重试和崩溃中断状态。
  较旧终态作业移出有界管理索引后保留报告和去重记录；最新/活跃/已发布作业保留。
  冻结测试实例使用独立 data_dir，关闭递归学习；测试轨迹带来源标识，生产导入拒绝作为学习来源。
  增加测试进程 nice、地址空间及 CPU 秒配置，保留可配置的宿主和模型 HTTP 超时。
- 版本评测结果自动写入用户 memory/evaluation-state.json；私有 PB/JSON 新增 evaluation_feedback，按内容版本保存状态、报告与配对计数。
  组合版本失败仅登记复核；同级、单条经验变化、真实 applied 回执及确定配对结果满足 memory_min_pairs 后，才自动验证或归档私有经验。
  不确定/基础设施错误不作为反证，不跨用户改写公共记忆；归档会通知原公共管理机制复核私人来源。
  反馈按报告 ID 幂等，纯管理修订不会重复触发同一行为版本评测。
- HTTP 增加 agent_evaluation 管理操作；原生新增独立 LocalRuntime::manage / te_local_manage 及 Python manage 包装。
  管理调用与低延迟 invoke 分离，复用已运行的后台服务；其他通用语言可以通过 UTF-8 JSON C ABI 接入。
- 更新配置模板、请求/响应 Schema、OpenAPI、使用手册的评测/私有记忆/版本/调度/接入/存储章节和完整闭环 SVG。
  新测试覆盖版本发布与重启、错误版本、并行后台与前台隔离、用户隔离、组合失败不批量归档、单条改善验证与退化归档、原生管理。
  使用确定性业务模型替身验证实际采集和策略接口；未使用生产 API Key 或调用付费模型。
- 影响查询因新入口未入索引返回 UNKNOWN，补充核对实际调用路径并运行组件回归。所有实现位于独立组件及本记录中，未提交或推送 Git。
- 验证：完整 CTest 24 组通过，其中新增评测管理测试含 8 个场景；独立安装的 HTTP 与原生客户端测试通过。
  源码版/安装版文档均通过 24 章、28 个 Markdown、51 张 SVG、475 个本地链接检查，OpenAPI 生成一致性和 git diff --check 通过。
  测试构建使用 CMake 解析出的 protoc，不依赖特定开发机路径；新增 C++ options 字段放在末尾，保留既有聚合初始化兼容性。

### CHG-20260909-169：撤回 AgentFlow 进化接入及残留的 Loop 控制

- 按用户要求撤回整个 AgentFlow 接入改动，不仅撤回最近新增的适配器；独立组件 `tools/trajectory_evolution` 的实现保留。
- 删除 `agentflow_runtime/loop_control.py`，移除 Coordinator v1/v2 和 Skill Executor 的 `decide_loop` 调用及 `policy_stopped` 出口；恢复原有轮次与终止处理。
- 撤回 AgentContext 的 `request_context` / `build_request_messages` 及其脱敏扩展；LLM 请求恢复原消息路径。
- 撤回按 request_index / tool_call_id 拦截的新增分支、v2 工具接入点、进化事件和 Recorder 的进化回执字段；原 Hook 能力、并行调度与安全门保留。
- 三份 SessionLog 同步撤回进化专用事件和字段，保留原日志采集、用户 / 会话目录及既有轨迹修复。
- 删除 examples 中的进化 Runtime Hook 包装文件与 Shadow Hook 符号链接，并清理对应 Python 缓存；组件内源文件保留，未删除 SessionLog 或业务 Hook。
- 核对后，上述 12 个受跟踪的运行时、测试和 SessionLog 文件均与接入前的 HEAD 一致；未整体重置工作区，未改 C++ 底座、业务环境或运行服务。
- 调用核对覆盖三条执行入口、请求上下文、派发 / 工具边界、Hook 目录和三份日志采集；索引为 AgentFlow / 46a2daa，落后 HEAD 9 个提交，影响查询为 UNKNOWN / partial，已补充源码引用核对。
- 原有组件目录中的旧 AgentFlow Hook 适配器及其专用测试本次未改造；已从宿主卸载，不属于当前 AgentFlow 的运行链路。
- 验证：AgentFlow Python 本地回归 481 项通过（未运行外部沙箱集成组），SessionLog 回归 56 项通过；相关宿主源码及缓存无 Loop Control / 进化挂载残留，补丁格式检查通过。未调用生产模型，未提交或推送 Git。
- 后续接入验收目标（用户确认）：组件面向任意通用语言及同步 / 异步、单 / 多 Agent、直接 / 间接通信框架。用户只需在必要环节调用统一打点接口、传入必要字段并配置参数，无需另写框架适配器、日志转换器、配置器或进化控制程序。
- 组件负责跨语言通信、轨迹规范化与因果关联、后台评测反思、记忆管理和策略版本管理，并封装日志采集与策略生效接口；宿主不依赖既有 Hook 或特定框架内部代码，主执行路径不等待后台评测与学习。后续若出现必须由用户补写上述逻辑的接入步骤，应补齐组件封装。

### CHG-20260909-170：统一打点、实际应用顺序与独立组件边界修复

- 新增通用 `operation=point`，原生与 HTTP 共用规范化实现。before/after/abort/event/heartbeat 使用同一 JSON；
  返回不透明 context，跨线程、回调和消息队列原样传递，causes 支持多个完成分支合流。保留原 run/activity/loop 低层接口。
- 打点内部先记录原输入，再准备策略和待执行输入；结束点记录实际输入、executed、结果和 usage，随后核验 applied 回执。
  未报告实际发送或执行证据时按 unknown 隔离，不将“返回增强参数”作为“已应用”。异常内容不落盘，也不输出包含内容的异常文本。
- 修复循环建议提前改变已提交状态：未采纳 replan 不消耗预算、不注入重规划内容；未采纳 stop 不锁死后续活动。
  循环 abort 不虚构实际 loop.decision。多个模型和动作独立记录结果，不按完成顺序匹配。
- 任务 after 提前收尾返回 pending，不中止在途兄弟分支。HTTP 的终态载荷被拒收时，只将对应活动/循环收为 unknown，
  其余分支仍能收尾；坏心跳不结束任务。原生非法或降级 context 不再扩大为其他 run 的全局采集缺口。
  并发缺口计数随具体 run 身份保存，无效 ID 先校验再竞争槽位；旧 run 延迟到达的错误不污染复用该槽位的新 run。
- L1/L2/L3 按已声明的 model/actions/loop 能力独立校验；model+loop 不必额外实现工具边界。
  后台候选编译遵循 instrumentation.capabilities。原始模型输入和轨迹保留完整内容，检索 query 使用 UTF-8 安全的有界首尾摘要。
- 新增从真实边界汇总的模型/工具/循环/步骤计数；未执行的拒绝动作不计入调用数。可选模型 usage 支持 token 汇总，缺失不补零。
  任务评测优先使用通用轨迹观测计数，宿主计数不一致时保留警告；未打点的内部调用不在这些计数的覆盖范围内。
- 新增可配置 run_lease_ms 与无事件心跳，过期采集收为 unknown/quarantined，不取消业务。max_stored_runs 改为热区容量，
  已落盘且无待评测作业的终态记录自动移动到 archive/runs 分片目录；不删除轨迹、历史和幂等记录，旧请求仍可查询和重放。
- 原生入口加入可配置的短竞争预算 contention_budget_us，默认每次锁竞争 1 ms、上限 10 ms，0 保留立即降级；不在业务线程做外部 I/O。
  Python、JS/TS、Rust、Go、Java、C# 提供 point 包装，HTTP 默认一次 20 ms 通信预算；保留 invoke 的原有严格错误与重试语义。
  C ABI 增加 te_remote_point，其他 FFI 语言可复用同一协议。传输失败返回原输入和 allow/continue，并标记采集不完整。
- 任务评测新增可选 instrumentation_config：组件自动准备隔离原生采集配置、冻结版本和试验身份，读取终态轨迹并汇总指标。
  被测 Agent 只需执行业务和已有打点，不再手工生成 evaluation 响应、版本绑定或 trace 对象；测试令牌仅通过子进程环境传递。
  旧评测宿主协议保留，测试实例关闭学习、共享和递归评测，不改生产实例或业务环境。
- README、快速开始及接入/配置/低延迟/评测/运维专章同步更新；请求 Schema、OpenAPI 和 JavaScript 产物同步生成。
  未改 AgentFlow 的 C++ 底座、Python 执行器、SessionLog 或 Hook，也未重新挂载进化组件。
- 验证：完整 CTest 25 组通过；新增统一打点测试覆盖原生和 HTTP、逆序与四并发、失败/跳过/未知、真实回执、隐私拒收、
  任务隔离及慢 HTTP 降级。独立安装的 HTTP/原生采集、后台评测与记忆初始化通过；不调用生产模型或付费 API。
  实际语言运行覆盖 Python、C、C++、JavaScript；Rust、Go、Java、C# 因本机无对应工具链，尚未做编译运行验收。
- 影响查询延续既有索引 UNKNOWN 状态，补充核对 HTTP、原生、C ABI、评测与安装调用路径；不将空调用图作为安全结论。
  本条记录的是接入和执行正确性修复，自动从经验生成 L2/L3 可执行规则、通用 HTTP 评测自动装配、HTTP 服务端异步持久化仍需继续完善；
  本次不将静态规则执行或短超时降级宣称为这些能力已完成。未提交或推送 Git。

### CHG-20260909-171：撤回 v1 接入尝试，明确仅打点的交付约束

- 按用户要求撤回本轮 Coordinator v1、Skill Executor 和共享 LLM 调用入口中的进化接入改动，包括新增的 DataBus 上下文字段、策略分支、装饰器和模型调用包装；三个文件已逐一确认与接入前 HEAD 一致。
- 删除本轮新增的 `agentflow_runtime/evolution_observer.py`；宿主源码中不再保留对该封装的引用。未改动原有业务修复、C++ 底座、SessionLog、启动配置或服务，独立组件中的改动保留。
- 接入负担以不要求用户编写额外程序为边界：用户只配置必要参数，并在明确的位置插入包提供的打点调用、传入已有的必要字段；不要求用户新建文件，或自行实现适配器、Observer、日志转换、上下文管理、评测程序及进化控制逻辑。
- 上述要求同时适用于任意框架及通用编程语言，不以 AgentFlow、Python、既有 Hook 或单进程串行调用为前提；同步、异步、回调、单/多 Agent、直接/间接通信都使用组件提供的统一接入契约。
- 通信、采集、脱敏、关联、持久化、降级、评测反思、记忆管理、策略生成/发布/注入及先后顺序由组件封装。接口不足时修改组件，不将内部职责转移给接入者，也不再未经确认向宿主补专用封装。
- 验证：相关 Python 回归 92 项通过，覆盖派发、并行工具、恢复、终态、打断与安全边界；回退文件无未提交差异，补丁格式检查通过。未重新挂载，未调用生产模型，未提交或推送 Git。

### CHG-20260909-172：包内完成编号关联、异步收尾与通用调用打点

- 统一 point 新增 ref.run/key/parent/after，直接使用原业务任务及调用编号；内部 context、活动编号、等待集合和因果合流由组件管理。
  HTTP 持久化关联及幂等响应；原生保留有界槽位、终态重放和任务级缺口。旧 context 接口继续兼容，不要求新增消息或 DataBus 字段。
- 父工作/任务提前报告结束时保存待完成状态，子工作终态到达后自动闭合；70 分支逆序完成可自动分段合流，不把返回顺序当作调用顺序。
  单支终态内容被拒收时只将该支补为 unknown，其余分支继续完成；修复 HTTP 错误恢复访问空 context 时意外插入字段、导致 ref 恢复被拒的问题。
- Python 提供 call/acall/allow_loop，JS/TS 提供 call/allowLoop，通用 JSON 与 C ABI 保留各语言统一入口。
  封装原输入记录、策略准备、原业务调用、实际结果和回执核验；异常重抛原对象，不记录可能带密钥的异常文本，业务调用不自动重试。
  循环作用域按所属工作隔离，已提交 replan 自动作用于后续同分支模型调用；工具 JSON 文本接口的拒绝结果保持原类型。
- Python 配置入口 evolution_configured 随包安装，无需宿主新建初始化文件；按已有用户选客户端，缺配置时直通原调用。
  本机 HTTP 单次通信默认 20 ms、无重试；这是通信预算，不包括初始化、编码或宿主调度时间。
  发送前检查敏感字段、环境密钥及常见凭据；命中只发送无内容 gap/abort，保留原业务数据，缺失轨迹隔离。
  privacy.public_env_names 可声明非认证类业务编号，显式 EVOLUTION_SECRET_ENV_NAMES 优先，凭据模式及字段检查仍保留。
- 新增 output_metrics 的可配置 JSON Pointer 提取，不要求宿主改写各模型的 usage；校验非负整数，缺失值保持未知。
  修复内部 JSON 数值表示导致有效整数 token 未被提取的问题。
- HTTP 增加有界后台持久化，排队/写入中数据计入容量；原生和 HTTP 自动评测均复用冻结试验版本、隔离目录和通用打点。
  短派发进程退出后，评测监督仍等待已登记工作收尾，统一遵守整体超时。
  配置式 Python HTTP 入口自动使用试验配置和凭据，不回写生产目录；原生试验不从该入口回退到生产 HTTP。
- 控制建议的类型/作用域校验和提示词保留在组件内；本次没有新增向外部模型发送私有记忆并自动生成可执行规则的运行路径。
  配置授权的 L1–L3 执行能力与外部模型自动生成控制规则分开记录。
- 更新接口 Schema/OpenAPI、包安装、使用指南与异步配图；不新增宿主适配器。新增工具链验证实际覆盖 Python、C、C++、JavaScript、Go、Rust、Java、C#，
  共 56 条有效轨迹；原生/HTTP/后台评测与配置客户端等完整 CTest 26 组通过。

### CHG-20260909-173：经明确授权仅以通用 SDK 打点接入 v1

- 在用户明确同意 L1/L2 生效及 L3 可提前结束当前循环后，接入现有 coordinator_decide.py、skill_executor.py、llm_call.py，合计新增 112 行。
  没有新增宿主 Observer、适配器、Hook、配置器、上下文管理文件，没有修改 DataBus 字段、C++ 底座、SessionLog 或业务模型配置。
  旧专用 loop_control.py 未恢复；循环点直接使用组件的通用 allow_loop。
- Coordinator 首次输入建立 run，完成唤醒沿用同一任务，最终答复收尾；派发前用现有 dispatch key 登记工作，提交明确失败时关闭对应记录。
  Skill 的正常/错误终态沿用现有统一出口回报，不让单个业务失败改写整个 C++ 工作流状态；并行工具调度和结果回灌顺序保持原实现。
- 模型调用仅将非认证输入交给组件，API Key 和客户端认证继续留在原调用闭包中；L1 在实际调用前注入。
  L2 作用于实际工具执行点，拒绝返回结构化 JSON 文本；L3 在 Coordinator / Skill 的下一轮位置生效，终止时沿用原结果写入/终态路径。
  默认未配置客户端时无采集网络请求；服务模板默认 L0，L1–L3 需要显式等级、能力和策略授权。共享 Skill/LLM 的 v2 路径不启用本次打点。
- 开启采集的跨进程测试发现 AF_EXEC_SKILL_KEY 会被 KEY 环境变量检测误判为秘密；以通用客户端参数声明它和 AF_COMPLETED_BATCH_KEY 为非敏感编号，
  不在组件里硬编码 AgentFlow 特殊识别。补充该配置与认证密钥仍受保护的回归测试。
- 验证：AgentFlow Python 本地回归 481 项通过（排除外部 integration 组），组件完整 CTest 26 组通过，补丁格式检查通过。
  新宿主测试调用真实 v1 函数与真实 C++ 进化服务，两个独立 Skill 进程并发；模型、工具、DataBus 为测试替身，没有调用付费 API。
  采集完整 90 个事件，6 次 LLM、2 次实际工具执行、6 个循环点、90 个 fixture token；L2 拒绝动作不计入执行次数。
  逆序完成保持 tool_call_id 对应，单支工具失败不影响兄弟任务，L1 注入进入实际请求，两个 L3 终止出口和采集服务断开后的原调用均通过。
  轨迹 doctor 校验通过，capture_complete=true；业务结果保持 unknown，不擅自把工具或任务结果判成业务成功。
  测试凭据未进入采集目录。结果在 build/trajectory-evolution/v1-points.dimqxr6k/test-summary.json。
- 安装及启用说明位于组件 docs/02、08、09，提供通用 point-client.json 模板。未替用户启动常驻生产采集服务或改现有 daemon 环境，未提交或推送 Git。
- 影响检查绑定当前 AgentFlow 仓库；新组件符号延续 UNKNOWN，v1 核对共享 LLM、派发、工具并行和既有终态调用路径后以回归验证，不将空图解释为无影响。

### CHG-20260909-174：实现 GEPA 多轮反思搜索、候选树与持久化策略

- 在 trajectory_evolution 内实现 C++ GEPA 反思搜索，参考 Hermes self-evolution 的训练/验证/留出流程，
  复用现有真实 Agent 任务评测器。主要实现位于 src/gepa_search.cpp，不要求宿主安装 DSPy。
  没有修改 AgentFlow C++ 底座，没有新增或修改 v1 打点、Observer、DataBus 字段、业务模型配置或生产环境。
- AgentEvaluationService 配置 gepa_config 后，带已批准经验的 L1–L3 候选自动进入搜索。
  逐题最优候选并集形成父节点池，按选择次数加权抽样；训练小批次比较父子，改善后运行完整验证集，再继续反思。
  每个子节点记录唯一父节点、深度、完整策略、改动原因、逐题成绩和评测引用，保留重复、退化、越权及异常分支。
  候选变异为一条经验正文或一组授权控制规则；不加入任意代码执行或跨候选合并。
- RuntimeOptions 增加可选 experience_overrides：策略版本覆盖注入文本，原记忆正文、来源 revision、触发条件、scope 与跨库关系不变。
  L2/L3 生成规则复用 control_learning 的来源授权、操作/参数白名单与循环收紧限制。旧配置与不含覆盖字段的策略继续可用。
  搜索试验不写回生产记忆，也不将组合改写的成绩当作单条来源记忆的因果正反证。
- 最终先冻结 winner，再用留出集与当前生产基线配对验收，沿用统一质量、覆盖率、样本、时延、token 和 loop 门槛。
  留出失败不继续针对该留出集反思；仅最新任务且生产父版本未改变时发布。已有 run 不换版本，下一 run 仍检查当前授权和记忆兼容性。
- 在 agent-evaluation/<job-id>/search 下保存 tree.json、versions、reflections、evaluations 和最终决策。
  原子写入、报告哈希与行为版本核验支持恢复；异常退出的同上下文搜索自动排队，完成的试验缓存复用。
  反思中断保留已预留预算，主动取消不自动恢复；同一 seed/上下文不会因 parent 发布变化重复自触发。
  限制轮数、模型请求、宿主试跑、总时间与 patience，预留最终留出成本；缺失目标指标保持未知。
  增加 task_ids 过滤和管理操作 agent_evaluation.tree，更新 JSON Schema/OpenAPI。
- 通过验收且实际发布时导出 published-version.json，包含完整策略、行为 ID 与验收信息。
  policy_version_file 可在无原 memory 数据库的部署加载版本；复用现有打点，保持用户/应用/规则边界，关闭后台进化并继续采集普通业务轨迹。
  文件属于可信部署输入；离线版本冻结当时的经验状态，撤销需要停用或替换版本文件并重启。
- 为现有模型传输增加可选取消回调，GEPA 反思和任务评分均接入；取消/总截止时间不再只能等完整 HTTP timeout。
  未设置回调的原传输行为保持不变，API Key 仍只来自环境变量，不进入反思输入、候选或版本文件。
- 验证：组件全量 CTest 28/28 通过；新增 GEPA 集成测试 8 项覆盖 L1 改写、L2 实际动作拒绝、L3 实际循环停止、
  留出失败、取消响应、中断恢复、预算保留、跨用户隔离、离线注入及泄露测试值拦截。OpenAPI、文档链接、SVG 结构和补丁格式检查通过。
  一次完整案例实际完成 6 轮反思，形成 7 个节点、至少两层父子关系，执行 18 次隔离宿主试验，选中节点 2 并发布。
  测试使用本地模型响应 fixture，不调用付费 API；不将 fixture 成绩作为真实业务收益。
  结果：build/trajectory-evolution/agent-evaluation-tests.sboo1u2h/case.bl9wuica/test-summary.json。
- 文档更新组件 README、docs/24 GEPA 专章及相关章节，重绘候选树/前后台执行示意图，清除本次能力对应的过时规划标记。
  新组件入口的图索引仍为 UNKNOWN，已核对评测、策略编译、HTTP/原生调用路径并完成回归，未将 UNKNOWN 当作低风险。未提交或推送 Git。

### CHG-20260909-175：补齐 GEPA 双亲联合重组、搜索策略与分组反思

- 对照 Hermes 使用的 DSPy GEPA 和上游 GEPA 核心，增加跨分支双亲重组：内部节点与另一分支的叶节点可以共同生成一个后代，
  不再只是分别被选为单一父节点。按共同祖先比较组件，分别继承两边的互补改进；冲突按验证质量及 fitness 选择，平分稳定处理。
  同一双亲／祖先只尝试一次，禁止祖先和后代互相重组；无实际互补、重复、越权或退化组合保留原因并跳过，不覆盖父版本。
- 搜索谱系升级为 schema_version=2、gepa-reflective-dag-v2，记录 parents、kind、ancestor、component_sources、conflicts；
  保留 parent 作为第一父编号兼容字段，深度取最长父路径。每个有效节点仍保存不可变完整策略，可追溯多父来源并恢复进度。
  旧单父记录支持规范化读取；算法指纹升级使新搜索使用新上下文，旧发布包与离线加载保持兼容。
- 增加前沿覆盖去冗余、覆盖频次抽样，以及 current_best、epsilon_greedy、top_k_pareto；保留旧 fair 模式。
  前沿支持逐任务、整体目标、两者并集、任务×目标；组件支持轮换和联合改写。
  默认训练集按 epoch 洗牌，末批不重复补齐；可选满分跳过、目标阈值和不退化接纳，patience 同时考虑整体最优及前沿变化。
- 增加 single、same_parent、independent、P×N 提案批次，以及全部改善者／最佳增益／Top-K 筛选。
  批次计划、任务、组件和完成状态先落盘；按相对各自父版本的平均质量与 fitness 增益排名，再执行完整验证。
  经验可同时改写多条；未提交的生成控制类别保留，提交的类别按完整数组替换，仍遵守来源授权与基础规则。
- 重组不调用反思模型，先在覆盖双亲优势与平分任务的验证子集上比较，平均质量与 fitness 均不低于双方后再完整验证。
  增加同版本、同 split、同上下文的逐题缓存复用，只补跑缺题；拼接报告保存 source_reports、不重复计入试跑数。
  支持显式关闭搜索缓存；最终冻结的留出评测在恢复时仍复用，不把验证或留出正文送入反思。
- 增加可选 ComBEE：复制、洗牌、分组反思，再按组件汇总；默认 single，不增加默认模型请求数。
  分组与汇总提示词均在 config/prompts，所有调用共用模型预算、取消、总截止时间与隐私／授权检查。
  开始前检查完整分组所需请求数，预算不足不启动部分工作；每阶段独立记录，中断不退还已占调用预算。
  修复复制反馈后单组超出原输入上限的边界：压缩为具体关键证据、必要时减少记录并标记截断，仍超限则拒绝。
- 所有节点和重组尝试均占统一预算，反思用尽后仍可在剩余节点、试验和时间预算内重组；继续预留最终留出成本。
  发布仍经过现有质量／效率门禁、最新任务、生产父版本、用户和来源授权检查；不把组合成绩直接当作单条记忆的因果反馈。
- 恢复测试发现隔离试验继承服务文件锁，会在父服务异常退出后阻止重启。
  在包内 evaluation_process.cpp 使用 spawn close-from，仅保留显式标准输入输出，关闭继承的服务锁和其他描述符；
  安装指南注明当前 Linux 运行器需要 glibc 2.34 及以上。未修改 AgentFlow 的 C++ 底座、v1 打点、DataBus、Hook、模型或环境配置。
- 配置与算法集中于原搜索文件及新增 gepa_strategy.h/.cpp，复用原后台任务评测器，不新增宿主接入点或框架适配程序。
  更新既有 README、docs/24 和配图，增加逐项上游对照、完整配置、ComBEE、字段与恢复说明；不另建分散手册。
  保留质量优先、受限策略空间与统一协议，不引入任意宿主代码改写、Python 框架依赖或外部可视化平台。
- 验证：完整 CTest 29/29 组通过，GEPA 集成测试由 8 项扩展为 27 项，另有 C++ 算法测试覆盖内部节点＋叶节点联合重组、
  谱系非法引用、组件冲突、前沿与多提案排名；覆盖缓存开关、合并退化、ComBEE 隐私／预算／中断、双亲恢复与既有离线注入。
  一个 L2 搜索使用 2 次反思、18 次隔离宿主试验，形成 4 节点，节点 3 同时继承节点 1 的经验和节点 2 的动作规则，并通过留出验收发布。
  结果：build/trajectory-evolution/agent-evaluation-tests.hdcgj0qe/case.4wo83gkx/merge-summary.json。
- 独立安装后的 HTTP 与原生采集、自动评测、记忆初始化通过；源码和安装文档的 24 章、51 张 SVG、478 个本地链接校验通过。
  既有 v1 两进程并发打点验收通过，保留失败分支隔离、回执顺序、两个停止出口和采集不可用时的原调用行为，
  结果：build/trajectory-evolution/v1-points.rrngpv4n/test-summary.json。
  所有模型响应为本地测试夹具，没有调用付费 API；测试成绩用于验证机制，不作为业务收益结论。
  影响检查沿用新组件符号 UNKNOWN 的已知索引边界，补充核对后台调用及回归；未提交或推送 Git。

### CHG-20260910-176：修复冷启动核对中的请求透传、失败留痕与指标分类

- 对外 JSON 返回增加独立传输编码：安全整数范围内的整数值保持整数记号，
  覆盖原生、HTTP、C ABI 及降级返回，避免 max_tokens=4096 被传回为 4096.0。
  原有落盘、哈希和版本指纹继续使用旧规范编码，不重写既有版本。
  此修复针对已确认的类型变化；前次 HTTP 400 未保留服务端正文，不将它断言为唯一拒绝原因。
- 隔离评测试跑异常退出时，在清理工作区前读取已采集的结构化证据，核对用户、试验与加载版本并执行隐私检查。
  完整轨迹保留 evidence.trace、实际调用指标及分析；不完整轨迹单列 failure_capture 并标记 complete=false，
  不将部分调用指标当成完整成绩。没有打点或证据不安全时保留固定原因，不保存原始 stderr。
  仍维持 runner_nonzero_exit／timeout 的失败判定，不让失败轨迹被当成合格策略评测。
- 标准轨迹分析按 semantic 区分模型、业务工具及自定义活动，明确 executed=false 的动作不计入工具执行或重试。
  模型调用仍保留在原始轨迹、步骤结果及适用的重复调用诊断中；同名模型与工具不再相互匹配为重试。
  未提供 semantic／executed 的旧轨迹保留原默认语义；analyzer_version 升级为 standard-trace-analyzer-v2。
- 回归新增无策略及降级请求整数透传、规范编码稳定、模型／工具分类、未执行动作、
  原生与 HTTP 异常退出的完整／部分／缺失轨迹及敏感内容拦截。
  完整 CTest 29/29 组通过；最后补充未执行动作边界后重新构建，相关 7/7 组回归通过。
  本轮验证使用本地夹具，未新增付费 API 调用，未据此声称真实 L4 实验已经通过。
- 更新既有采集、评测、数据集章节与字段说明，补充 harness 问题清单中的三项组件问题。
  未修改 AgentFlow C++ 底座、v1、DataBus 或线上配置，未提交或推送 Git。

### CHG-20260910-177：允许真实 v1 显式关闭 thinking，并补充真实 Agent 评测宿主

- 诱发原因：进化评测的 `thinking_mode` 只配置评测模型，不控制被评测 Agent 自身；
  v1 的 HTTP 客户端在流式请求中沿用启用 thinking 的默认值，真实任务仍会产生推理 token。
- 最小修改：`OpenAiCompatibleClient.build_request_body` 支持显式环境变量
  `AF_LLM_THINKING_MODE=enabled|disabled|auto`；设置时传递供应商 thinking 参数，
  未设置时保留原行为。无效取值在发送请求前报错，不静默使用意外配置。
- 新增可选评测宿主 `tools/trajectory_evolution/examples/evaluation-dataset/agentflow-v1-host.py`：
  接收现有 dataset-run 请求，部署隔离的真实 v1 包，启动 `agentflowd`，通过正常 `agent.execute`
  和响应通知执行任务。决策、DataBus、Skill 和工具仍由真实 Agent 执行；没有另写替代 Agent。
  隔离副本同时重定位工作流的工作目录，避免节点启动脚本又读原包的 `.env`；凭据只从进程环境继承。
  回环连接绕过环境代理，子进程和专属短 socket 目录在退出时清理。
- 涉及文件：上述宿主、`packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py`
  及 `packages/agentflow-runtime/tests/test_openai_compatible_client.py`。
- 验证：HTTP 客户端与 Recorder 共 39 项回归通过；真实新试跑已确认 Coordinator 与
  outfit-stylist 的模型均为 `deepseek-v4-flash`，并实际调用沙箱工具。整批及 L4 验收仍在进行，
  不将单条试跑或数据集构建成功记为 L4 已通过。
- 图影响分析：新宿主未被索引；HTTP 客户端方法虽可定位，但图遍历返回 UNKNOWN/部分结果。
  已直接核对 Recorder 与客户端调用点，不将 UNKNOWN 当作低风险。未修改 AgentFlow C++ 底座。

### CHG-20260910-178：评测器保留多轮任务中的多个真实 Run

- 诱发原因：一个评测任务可有 followup_inputs，而真实 v1 每轮对话产生独立 Run；旧评测器仅接受一个 Run，
  会把正常的多轮执行拒绝为多余轨迹。将它们硬拼成一个 Run 又会破坏原身份与父子关系。
- 在组件 task_evaluation.cpp 中按事件时间整理同一试验的多个 Run，逐条校验用户、版本、终态和隐私，
  保留原始 traces、execution_run_ids、逐轮输出与分析。单 Run 响应保持兼容。
  完整指标按任务汇总；任一轮未知的指标仍未知，不按零补齐。进程失败时保留逐轮证据，
  有不完整轮次则不输出全任务计数；进程错误和发布门禁不放宽。
- agent_evaluation.cpp 的记忆反馈遍历多条真实轨迹中的策略应用回执，仍沿用原有来源与归因判断。
  真实 v1 评测宿主移除旧多轮限制；宿主摘要仅保存每轮最终通知与通知数，完整调用证据仍由打点采集。
- 完整回归进一步复现异步两轮之间的提前结束竞态：新增 variant.run_layout=single|per_input，
  预先确定应收集的 Run 数量；包内采集结束检查增加带数量的重载，保留原 C++ 接口。
  第一轮结束而下一轮尚未创建时继续等待，数量不符拒绝完整成绩；失败报告标明预期和实际轮数。
- 验证：TaskEvaluationTests 共 32 项通过，新增原生、HTTP、异步派发、多轮顺序、缺失指标、
  完成后异常退出、部分完成和用户归属错误场景。测试中的模型/工具为协议夹具，真实整批和 L4 仍在运行。
- 更新数据集与评测章节和问题清单。图影响分析为 UNKNOWN，已核对 GEPA 的汇总读取和记忆回执读取；
  未修改 AgentFlow C++ 底座、v1 执行流或 DataBus。

### CHG-20260910-179：评测可直接验收配置声明的必需文件，产物失败保留轨迹

- 诱发原因：仅从 Run 终态读取 artifacts 时，未声明的真实文件不会进入独立读盘验收，
  评测只拿到工具返回和最终回复。多 Run 的产物检查失败还会在写回完整 evidence 前返回。
- 最小修改：评测配置支持按 task_id 或通配键声明必需 artifacts，复用原文件、格式、大小和隐私验证。
  路径、媒体类型、未知任务和重复声明在执行前校验；不扫描工作目录，不向被测模型传递验收配置。
  配置与宿主相同声明合并，类型冲突拒绝；未配置时保持原行为。可选生成分支不应列为必需产物。
- 在产物验收前写回已验证的完整证据，缺失文件或格式错误不丢失真实多轮轨迹、分析和指标。
  真实 v1 宿主默认会话标识采用任务 ID，重复试验仍由独立部署目录隔离，便于配置稳定的产物相对路径。
- 验证：完整 CTest 29/29 组通过（TaskEvaluationTests 32 项），覆盖原生、HTTP、异步宿主不额外声明文件、
  配置非法提前拒绝、缺失产物以及多轮失败时的轨迹保留；保留原路径越界、符号链接和凭据检查。
  实验中已生成图片的独立完整解码检查通过，但未因此将全部媒体任务或 L4 宣称为业务通过。
- 更新原评测章节及问题清单；图入口为已知 UNKNOWN，已直接核对 CLI、后台评测和 GEPA 共用调用点。
  不改变 AgentFlow C++ 底座、v1 决策、工具执行或日志采集协议。

### CHG-20260910-180：评测任务入口由组件管理，不再要求专用启动器

- 问题：已有评测默认要求用户编写读取 stdin、驱动 Agent 的宿主；v1 冷启动又增加了专用启动器，
  换框架仍需重写，不能满足仅做入口绑定和打点的交付约定。
- 新增显式 `variant.entry`，配置 JSON 输入字段绑定和终态输出提取位置。启动命令是原程序命令，
  包内封装调用已有接收函数；没有 entry 的旧 stdin/stdout 协议保持兼容。
- C++ 组件负责按题投递、固定测试用户/会话/策略、等待完整且持久化的 run.end 后推进下一轮，
  不把异步派发返回值或中间进程退出当作任务完成。每个 trial 一个接收者，拒绝额外 Run 和重复绑定，
  回调不自动重试，超时/取消继续清理本次进程组。
- 新增 `evaluation.next/fail` 认证协议以及 Python、JS/TS、Rust、Go、Java、C# 和 C/C++ 入口封装。
  普通应用未启用评测标志时直接返回，无连接、线程或轮询；异步封装使用宿主现有事件循环。
- 原生 LocalRuntime 仅在新入口评测模式下转发至试验采集端，避免重复打开采集目录而争锁；
  生产原生路径不变。评分、记忆管理、候选比较和 GEPA 继续使用原任务评测器。
- 输入模板仅绑定明确的 JSON 值，不执行代码；评分标准不下发，凭据不写入任务包。
  初始状态未绑定、输出路径缺失、轨迹不完整、历史超限等保留明确错误，不静默补齐。
- 移除 `examples/evaluation-dataset/agentflow-v1-host.py`；这是前次实验新增、未提交的专用驱动，
  不删除既有数据集、采集记录或实验结果。examples 目录缺失不再阻断安装；指定示例文件的配置仍需替换数据源。
- 更新默认评测配置、OpenAPI/请求 Schema、README 及原有评测/客户端/测试章节；没有新增 AgentFlow 框架适配器，
  没有修改 v1、DataBus 或 AgentFlow C++ 底座。具体框架仍需在其真实输入接收边界绑定已有函数。
- 验证：任务评测回归 38 项通过；Python/C/C++/JS/Rust/Go/Java/C# 原有跨语言协议回归通过；
  C++ 普通函数与 const 闭包绑定实际编译并验证未启用时不调用；最终全量回归 29/29 个套件通过。
- 影响检查：相关新符号为已知 UNKNOWN，直接核对 dataset CLI、后台 AgentEvaluationService、
  GEPA 及 LocalRuntime/C ABI 调用路径，未把索引未解析当作无影响。

### CHG-20260910-181：真实 Bridge 绑定评测入口，修复已脱敏链接导致的多轮采集拒绝

- 问题：真实 v1 的后续对话包含已脱敏的媒体链接，Python 和 C++ 隐私检查会把
  `sig=<redacted:credential_field>` 再当成凭据，拒绝下一轮打点；业务继续而采集不完整。
- 修改：仅在凭据格式匹配时跳过这一精确的既有脱敏占位符；显式环境密钥仍与原文比较。
  同时处理 Protobuf JSON 对尖括号的转义形式。占位符后的实际内容、其他查询凭据和任意自造占位符继续拒绝。
- Bridge 仅在评测入口标志启用时，将已有 `chat_rest` 接收函数交给包内 `aevaluate`；
  原会话、异步 DataBus 调度与普通启动保持不变，没有增加替代 Agent 或专用启动器。
- 文件：`frontend/bridge/main.py`、`frontend/bridge/tests/test_evaluation_entry_binding.py`，
  组件 `clients/python/evolution_configured.py`、`src/evolution_pipeline.cpp` 及对应回归测试。
- 验证：Python 脱敏边界回归、C++ 脱敏边界回归及 Bridge 启用/未启用入口绑定测试；
  不以协议测试代替完整真实多轮实验验收。

### CHG-20260910-182：L4 按有效经验不足自动补充训练，不再仅覆盖空库

- 诱发原因：原后台只有候选评测/GEPA，没有把训练评测接到学习队列；统一拒绝评测轨迹入库
  会阻断冷启动。只判断库是否为空也会遗漏经验过少、作用域不匹配、归档或冲突过滤后不足的情况。
- 修改：复用既有后台评测线程与任务执行器，用原 Agent 的 L0 无策略版本分批运行训练题。
  以编译和授权后实际可用的经验数判断是否准备好种子；配置 `training.min_experiences`、
  `batch_size`、`max_batches`，默认分别为 3、16、4。足够后自动转入既有策略评测/GEPA。
- 加入可信本地训练报告入库入口：检查完整报告指纹、冻结数据集、train/reviewed、题目版本、
  L0 版本、真实用户/应用/试验 ID 和闭合轨迹；普通日志入口仍拒绝评测轨迹。
  验证集、留出集和注入候选的评测数据不能走此训练入口。成功及有完整证据的业务失败均可学习。
- 复用原分析、元反思、私有记忆管理队列；Run 清单保留 `training_source`，训练身份不增加
  公共经验的独立真实用户支持票数。没有更改公共库的推广和冲突门槛。
- 进度写入既有评测状态文件。同一应用/数据集的题目分配和预算不随记忆 revision 或重启重置。
  等待本批学习完成再补下一批；执行/入库/学习错误、题目耗尽和预算耗尽均有独立状态。
  归档的已完成 Run 仍读取其真实学习结果，不误判失败；没有自动无界重试。
- 离线 `learn --config` 不再因暂时没有待分析日志而提前退出；等待训练与策略评测队列。
  等待使用处理前的工作版本，避免完成通知先于等待而造成空等；数据仍不足以非零退出码报告。
- 文件：组件 `src/agent_evaluation.cpp`、`src/integration_service.cpp`、`src/learn_cli.cpp`、
  对应头文件、默认评测配置、训练/任务/公共经验回归、README 与原有第 19/24 章。
- 影响检查：图索引对新增组件入口仍为 UNKNOWN；直接核对后台刷新、HTTP/原生管理、离线 learn、
  任务评测和 GEPA 调用链。改动位于独立组件，不改 AgentFlow C++ 底座或增加宿主打点。
- 验证：新增 8 项确定性训练补充测试全部通过，覆盖空库、非空但不足、过滤后不可用、
  充足时跳过、重启去重、归档、入库/学习失败和离线等待。最终其余 29/29 个 CTest 套件通过；
  GEPA 搜索套件另行运行通过，Bridge 可选入口绑定 2 项通过，`git diff --check` 通过。
- 边界：经验数量门槛是可配置的数据准备条件，不是质量评分。真实任务的正确率、无效 loop 改善、
  元反思成功率和最终策略收益仍需独立模型实验验收。

### CHG-20260910-183：可选评测入口不阻塞 Bridge HTTP 服务启动

- 问题：在 Bridge lifespan 的启动阶段直接等待完整评测，使 HTTP 端口直到评测结束才启动；
  真实 Agent 执行中回传图片、视频或文件时无法访问同一个 Bridge 的制品接口。
- 修改：仅在启用评测入口时，将包内 `aevaluate` 放入当前事件循环的后台任务，继续完成服务启动；
  服务关闭时取消并等待该任务。仍调用原 `chat_rest`、会话提供者和真实 DataBus 链路。
- 文件：`frontend/bridge/main.py`、`frontend/bridge/tests/test_evaluation_entry_binding.py`；
  原评测指南第 19 章补充需要同时保持 HTTP/消息服务运行时的接入顺序。
  不改变普通启动路径、v1 打点或 AgentFlow C++ 底座。
- 影响检查：按仓库影响分析流程核对 lifespan 的应用注册、启用/未启用分支、聊天与关闭路径；
  图结果仍为已知 UNKNOWN，未把未解析调用关系当作无影响。
- 验证：Bridge 入口绑定、聊天 SSE 和 daemon 路由共 45 项测试通过。真实 v1 三轮预检正常结束，
  3 条 Run 轨迹闭合、17 次 LLM 调用有执行者归属，文件上传成功；图片的后续路径核验仍被判为业务失败，
  未将该失败写成完整业务或 L4 验收通过。

### CHG-20260910-184：保留采集拒绝原因，修复脱敏链接后换行的误判

- 真实训练发现：多轮生成图片后，第二轮的模型返回与 Run 结束打点被拒绝，
  评测返回 `evaluation_entry_trace_incomplete`。原来只记录 `missing_boundary`，无法区分具体原因。
- 诊断补充：采集服务处理 `point` 失败时，在降级响应和 `capture.gap.attributes.error_code`
  中保留经过隐私检查、仅含小写字母/数字/下划线、最长 96 字符的错误码。
  不输出异常原文或被拒绝的载荷；旧 `capture.gap` 请求保持兼容，未知原因仍使用固定降级码。
- 根因：对整段序列化 JSON 做凭据正则匹配时，脱敏占位符被移除，但后续换行是字面量 `\n`；
  正则将换行转义与后续中文一起算作签名值，误报 `credential_value_rejected`。
- 修复：在原隐私检查函数内部解码 JSON，分别检查字符串内容及嵌套 JSON 字符串。
  已知密钥同时检查原文和解码内容；只豁免原有精确占位符，旁接实际签名、其他查询凭据和转义密钥仍拒绝。
  业务请求/响应不修改，采集不完整的轨迹继续隔离，训练入库门槛不放宽。
- 文件：组件 `src/integration_service.cpp`、`src/evolution_pipeline.cpp`、对应测试、
  现有请求 Schema/OpenAPI 和第 7 章。没有修改 AgentFlow C++ 底座或增加宿主打点。
- 影响检查：原采集、元反思响应校验、记忆匹配与持久化共用隐私函数；
  按仓库影响分析流程核对这些调用路径，并补充 JSON 换行、嵌套 JSON、转义凭据和错误码落盘回归。
- 长轨迹回归同时发现固定扫描节点上限会误拒完整的 40 轮原生轨迹；改为按输入字节数限制扫描工作量，
  保留嵌套 JSON 解码层数保护。原生无业务线程 I/O、40 轮采集与后续学习测试已恢复通过。
- 真实三轮复测：3 条 Run 闭合、16 次 LLM 调用有执行者归属，未出现 `capture.gap`，
  已知测试 API 密钥扫描无命中。图片路径核验的业务失败仍被单独判分，未当作采集失败或 L4 验收成功。
- 最终完整回归：30/30 个 CTest 套件通过，包含长轨迹原生运行、补训和 GEPA 搜索。

### CHG-20260910-185：Local 媒体读写与文件工具使用同一私有目录

- 问题：真实多轮训练中，两张图片已经生成，但后续读文件误报不存在。媒体工具在存在沙箱端点时
  将 `assets/card.png` 上传到包根目录，Local 文件工具却在 `workspace/<用户与会话>/assets` 中查找。
- 修改：仅 Local 模式将媒体生成与素材读取统一到现有用户/会话工作目录，返回文件工具和产物收集
  都支持的绝对路径；私有文件不存在时不回退到共享包目录或共享传输端点。
  校验跨用户目录和符号链接越界；Docker/E2B 继续使用原沙箱相对路径传输。
- 文件：`packages/agentflow-runtime/src/agentflow_runtime/security/trusted_media.py`、
  `packages/agentflow-runtime/tests/test_trusted_media_local.py`。不改调度器、v1 打点或 C++ 底座。
- 影响检查：按仓库要求检查两个媒体入口；图查询仍为已知 UNKNOWN，直接确认图片生成、视频生成、
  穿搭图片分析三个调用路径及后续文件工具/产物收集，没有把图中的空调用集合当作无影响。
- 验证：Local 媒体、凭据安全边界、HTTP 文件工具和路由标识共 45 项测试通过，覆盖有/无沙箱端点、
  生成后再读、路径幂等、共享目录不回退、缺失身份、跨用户/符号链接拒绝与远端上传兼容。
- 扩大回归至产物收集、URL、用户上传边界和密钥脱敏后，91/91 项测试通过。
- 真实 v1 训练题 `t-05` 三轮复测由程序判定通过：两个名片版本均能读取，第三轮成功保存核验索引，
  没有第三次生成；14 次 LLM 调用、6 次工具调用，3 条 Run 闭合且无采集缺口。

### CHG-20260910-186：任务评测补充逐轮效率证据，统一 GEPA 与发布门禁的统计口径

- 问题：真实轨迹存在前提不足后的重复尝试，但规则只识别严格的不可重试同参数重试，
  因而规则下界为 0。仅比较下界或总调用次数，不能证明无效 loop 实际减少；原比例门禁还会把 0→0 当作达标。
- 修改：现有任务评分器可通过 `judge.loop_review_prompt` 加载外置逐轮评审模板。
  同一次评分请求同时返回业务验收与循环效率；不新增宿主打点、Agent 调用或前台网络等待。
- 按 Run、执行实例、loop、轮次合并同轮控制/模型/工具事件；模型浪费结论必须引用该轮事件，
  未知轮次、重复项、跨轮引用和无效结构均拒绝。完整评审才计算数量，不确定、未观测、模型错误不填 0。
- 规则 `invalid_loop_lower_bound` 不变；新增证据内 `loop_review` 和汇总 `invalid_loop_assessment`，
  明确区分模型估计与规则下界。GEPA 和后台门禁读取同一口径；正比例下降还要求基线确实大于 0。
  任务正确率和循环效率分开，既有精度门禁不放宽。
- 文件：组件任务评测、GEPA 评分、后台门禁、外置模板、相应测试和任务评测指南。
  无 Protobuf 字段变更，无 AgentFlow C++ 底座修改。新模板计入配置指纹，旧配置保持原评测口径。
- 影响检查：按仓库流程检查任务评分/汇总、GEPA scores 和后台 gate_result；图索引仍为已知 UNKNOWN，
  直接核对 dataset run、自动训练、后台策略评测与 GEPA 的共同调用链。
- 补充边界：统一 Protobuf 数字/字符串轮次表示；显式启用逐轮评审时，规则判失败的任务仍可获得
  效率证据，但业务失败结论不允许被模型覆盖。默认未启用时仍跳过这些模型调用。

### CHG-20260910-187：严格轨迹评测失败保留安全错误码

- 问题：真实训练轨迹入库时，严格内容评测的 `EvaluationError` 落入通用异常处理，
  一律显示 `trace_evaluation_failed`，无法区分响应协议、输出截断、服务或网络错误。
- 修改：只对已知严格评测错误提取内部错误码，通过固定白名单及有界 HTTP/libcurl 数值校验后，
  写入原作业的 `errors[run_id]` 和 Run 的 `evaluation_error`；其他异常继续使用通用错误。
  不保存原始异常、模型响应或密钥，不改变严格评测、同批隔离、计费预算和显式重试行为。
- 文件：组件 `src/integration_service.cpp`、`tests/test_learning_schedule.py`、第 17 章调度文档。
  没有修改 AgentFlow C++ 底座、宿主打点或配置接口。
- 影响检查：检查 `IntegrationService::process_evaluations` 与严格内容评测调用链；
  图索引仍为 UNKNOWN，直接核对离线 learn 和后台工作者的错误、检查点与重试路径。
- 验证：训练补充、任务评测、学习调度、统一入库和 SessionLog 评测 5 个专项套件全部通过；
  新增调度测试覆盖 HTTP 503、非法 JSON、输出截断和响应夹带凭据，均保留失败状态且不泄漏响应。
  这项修改修复诊断信息丢失，不代表此前真实模型评测失败的根因已解决。
- 最终复核：包含 CHG-186 逐轮效率评测与失败分支补充在内，独立组件 30/30 个 CTest 套件通过
  （238.35 秒），`git diff --check` 通过；测试均为本地回归，不作为真实 L4 收益验证结果。

### CHG-20260910-188：模型响应统一兼容单层 JSON 代码块

- 问题：真实 v1 测试轨迹结构正常，但内容模型诊断返回 `invalid_json`。
  任务评分器已经支持单层 Markdown JSON 包装，内容评测器仍直接解析原文，存在不一致。
- 修改：仅移除完整包住响应的单层 JSON/无语言代码块，支持 LF/CRLF；随后执行原有 JSON 对象、
  字段、证据与经验校验。凭据检查仍针对完整原始响应，响应指纹不变，不增加模型调用或自动重试。
  `issues` 增加安全格式标记 `dialogue_model_json_fence_unwrapped`，不记录原始模型内容。
- 文件：组件新增内部 `src/model_json.h`，内容/回合评测、任务评分、元反思、私有与公共记忆整理、
  数据集生成及 GEPA 的模型响应解析统一复用；补充对应回归和第 17 章调度文档。
  配置、轨迹及 HTTP 协议的普通 JSON 解析不变。
  没有更改 AgentFlow C++ 底座或正式运行配置。
- 影响检查：内容评测入口和 `validate_dialogue_response`；图索引对新增组件仍为 UNKNOWN，
  已核对标准轨迹与 SessionLog 共用的直接调用链、后续报告和经验生成入口。
- 真实复测：同一输入、同一火山 `deepseek-v4-flash`、关闭 thinking，1 次请求成功，
  报告确认实际移除了 JSON 代码块，得到 1 条待审核经验建议。此前诊断失败，未写入经验库。
  尚不能据此认定其他全部历史失败同源，也不将单条评测成功当作完整 L4 验收。
- 本地验证：补充单层包装、LF/CRLF、额外说明、多代码块、非对象、严格失败、伪造证据和凭据拒绝测试；
  全量 30/30 个 CTest 套件通过（235.97 秒），`git diff --check` 通过。
- 下游复核：统一解包后再跑全量 30/30 套件通过（246.70 秒），包含元反思、私有去重、公共提炼/审查、
  数据集 review 门槛和 GEPA 发布/隐私门槛的包装响应回归；未增加模型请求与重试。

### CHG-20260910-189：明确任务条件分支与必要收尾的评分口径

- 问题：真实训练任务允许工具失败后如实停止，评分理由却只因缺少视频产物判失败；逐轮评审又将
  向上游返回结果、向用户报告失败按“没有新状态”计为浪费，可能污染后续版本比较。
- 修改：任务评分明确按 `expected_behavior` 中实际触发的分支逐项验收，不自行加严或放宽要求；
  效率评分明确区分必要交付与同一接收对象的重复交付，不将前一轮错误重复归罪于终态报告。
- 文件：`config/prompts/task-judge-system.txt`、`config/prompts/task-loop-review.txt`。仅修改外置提示词，
  不改 AgentFlow 底座、评分门槛、数据集或既有分数。结果仍由真实重跑重新产生。
- 边界：模型评分仍可能误判；这些规则收紧判据，不替代证据核验，也不保证所有语义判断正确。
- 验证：任务评分与文档测试通过；真实三轮任务诊断请求返回合法 JSON。未手动覆盖旧评分。

### CHG-20260910-190：任务评分请求共享重复长文本

- 问题：真实三轮任务的评分请求达到 1,968,500 字节、581,770 个输入 token；同一段消息历史在多个
  事件和后续模型上下文中重复出现，增加评审开销和输入超限风险。不能据此断言它是此前所有 JSON 失败的根因。
- 修改：只在发送评分请求前，对事件输入中出现至少两次且不少于 512 字节的完整字符串使用引用；
  第一处保留完整原文，后续使用 `{"$evidence_ref":"原文路径"}`，`evidence_text_refs` 保存明确的引用关系。
  所有输出、工具结果、状态和产物保持原文。引用后更小才采用，不截断、不丢弃、不以摘要或指纹代替原文；请求字节上限继续生效。
- 配置：`judge.share_repeated_text` 是可选布尔开关，随附配置开启，旧配置省略时维持原请求格式。
- 边界：隐私检查在共享前后执行，原始轨迹、试验报告、确定性检查、事件 ID、循环分组及评分证据引用规则不变。
  原证据中的同名字段不会成为组件生成的位置表，也不改变事件执行次数。
- 文件：组件 `src/task_evaluation.cpp`、外置任务评分提示词及本地回归测试。没有更改 AgentFlow 底座。
- 真实复核纠正：最初的全字段 `null` 占位虽然通过无损还原测试，却被模型误读为没有最终答复。
  已撤掉空值占位，并缩小到重复事件输入；不将该版本的评分用于训练或版本对照。
- 回归覆盖：完整中文文本、数组、特殊 JSON Pointer、真实空值、同名不可信字段的还原，
  最终输出与每个工具结果原文不变，以及旧配置、非法开关和重复凭据拒绝。
- 验证：46 项任务评分回归通过；同一条真实穿搭记录用新引用格式诊断，评分恢复为 passed、循环引用有效，
  原始记录逐字节不变。证据请求从 614,105 字节降为 257,099 字节（减少约 58.1%）；关闭 thinking。
  该诊断不覆盖或代替正式训练、验证与留出评分。
- 最终回归：输入引用版本全量 30/30 个 CTest 套件通过（244.98 秒），`git diff --check` 通过。

### CHG-20260910-191：任务评分支持授权范围内的具体证据路径

- 问题：多轮任务出现 `evaluation_judge_evidence_invalid`。原代码仅接受顶层白名单路径逐字相等，
  即使模型引用的是实际存在的 `/output/final/response` 或某个轨迹事件，也会被拒绝。
- 修改：允许白名单路径及其实际存在的子路径；先按完整路径段检查授权范围，再用原始证据解析 JSON Pointer。
  不存在、越界、伪造根前缀以及请求压缩包装的路径仍被拒绝；同步更新外置提示词及格式恢复说明。
- 文件：组件 `src/task_evaluation.cpp`、任务评分提示词、评分测试及第 19 章文档。无 AgentFlow 底座修改。
- 边界：该错误也可能来自真正不存在的路径，因此不将本次全部失败都归为白名单过严；没有直接接受或改写旧评分。
- 验证：全量 30/30 个 CTest 套件通过（361.74 秒）；覆盖真实子字段、数组索引，以及不存在字段、
  根前缀伪造、越界索引和请求包装路径的拒绝。

### CHG-20260910-192：文本文件工具拒绝含 NUL 的二进制内容

- 问题：真实三轮名片任务用 `read_file` 读取 PNG，执行器以 UTF-8 替换解码后将二进制乱码作为成功文本返回，
  进入后续模型输入和轨迹。该次模型调用随后长时间未返回，但不能据此断言乱码是网络等待的唯一原因。
- 修改：HostExecutor 的文本读取先检查开头 8192 字符，返回前也检查所选内容；遇到 NUL 返回明确的二进制读取错误，
  不返回乱码。工具描述明确限于文本，二进制文件应通过支持相应格式的工具核验元数据。
- 边界：不根据扩展名猜测格式，不宣称该检查识别所有二进制；正常中文、行选择和既有非 UTF-8 替换行为保持不变。
  媒体原始字节下载接口不受影响，不改 AgentFlow C++ 底座。
- 回归覆盖：PNG 头、带行选择的二进制读取、探测窗口之后的 NUL、中文文本及原替换行为、HTTP 沙箱错误传播。
- 验证：文件执行器及真实 HTTP 沙箱相关测试 58/58 通过；测试命令对回环地址绕过环境代理，未更改全局环境配置。

### CHG-20260910-193：循环评审允许引用先前因果证据

- 问题：真实视频轨迹的脱敏诊断请求返回合法 JSON，但对重复操作引用了先前轮次的真实事件；旧校验要求所有
  引用都属于被判定轮次，导致这种因果说明被统一拒绝为 `evaluation_loop_review_invalid`。
- 修改：每条结论仍必须包含本轮事件，同时允许同一 Run 内、本轮第一个事件之前的真实事件作为依据。
  未知、跨 Run、未来事件、重复引用以及完全缺少本轮锚点仍被拒绝；不直接接受或改写旧评分。
- 文件：组件 `src/task_evaluation.cpp`、循环评审外置提示词、任务评分回归测试和第 19 章文档；无 AgentFlow 底座修改。
- 边界：结构校验确保事件引用可信，不替代模型对必要收尾、有效探索和业务效果的语义判断。
- 时序以既有轨迹校验器的因果拓扑排序为准，不依赖异步采集时 JSON 数组的到达顺序；补充乱序输入回归。
- 验证：任务评分及文档测试通过，包含“本轮 + 先前原因”放行、未来引用与无本轮锚点拒绝；开始真实 v1 冷启动复测。
- 完整回归：30/30 个 CTest 套件通过（364.22 秒）。真实三轮名片训练题已通过任务评分；
  本轮仍存在其他题的模型证据路径错误，按 error 保留并排除出合格训练导入，不将局部通过写成全链路完成。

### CHG-20260910-194：评分路径恢复提供具体反馈并隔离待生成结果

- 问题：真实复测连续出现 `evaluation_judge_evidence_invalid`；旧恢复请求没有指出哪些路径不合法，
  模型只能重新猜测。请求还包含尚未计算的 `loop_review` 占位结果，与禁止引用自身评审结果的规则不一致。
- 修改：请求副本移除未生成的循环评审；校验收集不合法路径，隐私检查通过且限长后才传入下一次恢复反馈。
  错误 JSON Pointer 语法也进入原有有界格式恢复；外置提示词说明 JSON 字符串不是嵌套对象，恢复时选取已有授权路径。
- 边界：不自动修改、丢弃错误引用以凑出通过结果，不放宽业务标准、不增加重试次数；原始轨迹不变。
  最终仍不合法的评分保持 error，不能进入合格训练导入或策略发布。没有修改 AgentFlow C++ 底座。
- 回归覆盖：待评审结果不进入模型证据、缺失路径与非法转义的准确反馈、有界重试后不残留前次引用。
- 同源恢复：可解析且隐私校验通过的上次输出进入 `validation_feedback.previous_response`，同时覆盖循环引用错误的修正；
  它不进入执行证据或自动成为最终评分，仍受既有请求大小和重试上限约束。
- 定向真实复测：视频题已一次通过；JSON 修复题路径校验通过后仍出现循环引用错误，继续使用原有一次格式恢复进行验证。
- 恢复补齐后：两道真实题均在最多两次评分请求内完成协议校验，完整 CTest 30/30 通过（245.62 秒）。
  复核发现 JSON 题存在业务误判，另按下一项修正，不把协议通过等同于业务判分正确。

### CHG-20260910-195：任务评分区分安全拦截与实际业务执行

- 问题：真实任务要求实际触发一次 JSON 解析失败，但 `bash` 在本地凭据保护下被拦截，未运行解析器；
  模型却将“命令被拦截 + 写入/读回修正文件”评为全部完成，属于业务误判。
- 修改：只调整外置任务验收提示词，明确调用尝试、安全/权限拒绝、实际业务失败三者不同；
  未执行的强制动作不能由如实说明替代，除非冻结的成功标准明确允许相应降级分支。
- 边界：不关闭本地凭据保护、不扩大命令执行权限、不修改冻结任务标准或手工改写评分。
- 真实复测：JSON 修复题正确判为 failed；视频题仍出现两次无效 JSON，另按下一项修正。

### CHG-20260910-196：模型适配器支持显式启用 JSON 输出模式

- 问题：真实视频任务的评分即使有格式说明和一次恢复，仍偶发返回无效 JSON，无法形成有效训练评分。
- 修改：组件模型配置增加可选布尔值 `json_object_output`；仅设为 true 时发送标准
  `response_format: {"type": "json_object"}`。默认不发送，保留旧配置及不支持此模式的服务兼容性。
- 范围：共用模型适配器及配置读取、任务评分回归和配置指南；未改 AgentFlow C++ 底座或主 Agent 输出格式。
- 边界：不放宽结构、证据、隐私、业务及发布门槛；不增加重试、不改 API 地址，格式模式不保证语义正确。
- 接口验证：授权测试接口返回 HTTP 200、合法 JSON 对象、reasoning_tokens=0；不含用户轨迹的固定文本探测。
- 验证：53 项任务评分用例以及配置、模型边界、文档套件通过。真实 v1 视频题重新执行后，
  一次模型评分通过输出和证据校验；接口返回 UnsupportedModel 时按冻结标准验收如实停止分支，
  不将它记为视频生成成功，也不据此宣称完成 L4。
- 完整回归：30/30 个 CTest 套件通过；真实双用户空库训练继续执行。

### CHG-20260910-197：循环评审补充必要交付的反例核对

- 问题：JSON 模式复测输出合法，但模型把“无效空调度后的首次最终回复”也列为浪费，
  理由同时承认该轮完成交付，重复计算了同一错误的成本。
- 修改：只补充外置循环评审提示词的通用三轮反例与输出前自检；删除候选轮时保持其他轮原样，
  无此前等价交付证据的必要回复/上报轮不能列入浪费。不改变业务评分或手工改写历史结果。
- 边界：循环语义判断仍是模型估计，不能将格式校验通过当成语义绝对正确；后续真实实验继续复核。
- 后续真实核对：训练首题仍出现首次最终回复误计，提示词补充没有完全解决该问题；保留为未解决的评测偏差，
  不将本轮模型计数直接作为已实现无效 loop 降幅的证明。

### CHG-20260910-198：循环引用错误提供具体约束和条目位置

- 问题：正式训练题的 JSON 已合法，但循环引用两次校验失败。原恢复仅提供统一错误码，
  无法区分未知轮次、重复引用、缺少本轮锚点或不允许的时序引用，日志也不能说明具体原因。
- 修改：组件原校验条件拆分为具名约束，反馈只增加约束代码与数组下标；最终失败理由保留约束代码。
  既有 `evaluation_loop_review_invalid` 错误分类、全部校验条件、最多一次恢复及隐私/大小门槛保持不变。
- 文件：任务评分源文件、回归测试、外置循环评审提示词及第 19 章；不改 AgentFlow 底座，不重写旧评分。
- 运行边界：当前进程及其正在执行的训练批次不会中途切换二进制或评分提示词；后续批次按既有机制读取配置。
  本次代码改动待回归后用于修复复测，最终收益对照需要在同一固定配置下重新验证。
- 定向验证：54 项任务评分用例及文档套件通过；覆盖未知/重复轮次、未来引用和缺少本轮锚点的具体反馈，
  原错误分类和最多两次模型请求保持不变。全量组件回归继续运行。
- 完整回归：30/30 个 CTest 套件通过；本轮真实任务继续使用在途进程，未重写已有报告。

### CHG-20260910-199：训练批次逐题排除不完整的超时轨迹

- 问题：真实训练题达到超时，只保留 `failure_capture.complete=false`；它的业务 verdict 为 failed，
  原导入逻辑因此继续要求完整 trace，进而拒绝整批其他完整轨迹。
- 修改：仅对 `execution_status=timeout` 且 `error_code=runner_timeout`、没有完整 trace/traces 的条目计为拒收并跳过。
  正常业务失败但轨迹完整的条目仍可入库；报告中的原始失败和部分轨迹保留，不补造 run.end。
- 边界：全批次 train/reviewed/版本/重复 ID 验证仍先执行；损坏或伪造的完整轨迹仍拒绝，不从混有验证/留出题的报告中选取训练行。
- 范围：组件训练接纳函数和混合批次回归；未修改 AgentFlow C++ 底座。
- 验证：含真实超时执行的混合批次接纳完整行、拒收不完整行；任务评分及冷启动补库套件通过。

### CHG-20260910-200：评测专用子进程回收脱离进程组的后台执行器

- 问题：真实 v1 超时后，Python 执行器仍存活并在约 164 秒后写出 span，重建了已清理的 `.work`。
  原实现只杀一个进程组；独立会话/进程组中的执行器不在其中，随后批次收尾的空目录删除抛错。
- 修改：复用组件 CLI 的私有子进程入口，在独立进程中设置 subreaper，原 Agent 命令仍原样执行。
  结束、超时或评测父进程退出时，回收该子进程所属的全部后代，确认退出后才进入工作目录清理。
- 边界：不在主 Agent/SDK 宿主进程中设置 subreaper，不扫描或杀其他用户的进程；资源限额在原 Agent 启动前生效。
  用户无需编写启动器或框架适配代码；主 Agent 的运行方式和 AgentFlow C++ 底座不改。
- 验证重点：double-fork + setsid 后代、延迟写目录、无关进程不受影响，以及已有同步/异步/多轮入口行为。
- 定向验证：56 项任务评分用例及冷启动补库套件通过；脱离进程组的后代已被回收，工作目录没有被延迟写入重建。
- 全量回归：30/30 个 CTest 套件通过（400.19 秒）。真实 v1 两题复测完成后无遗留后台进程、无 `.work` 残留；
  两题业务失败按冻结标准保留，未把进程正常退出误报为业务成功。

### CHG-20260911-201：逐轮判定与浪费计数分离

- 问题：真实评分将“首次回复是必要收尾，不能省去”放进浪费数组，单靠补充提示词仍出现理由与计数矛盾。
- 修改：同一次模型评分改为逐轮输出依据及 keep/waste/uncertain 判定，程序仅从完整评审的 waste 项计算数量。
  complete 必须覆盖所有观测轮次、无重复和不确定判定；全部依据落盘，必要收尾的 keep 项不进入浪费列表。
- 边界：不按中文关键词猜判分、不手工删除旧报告条目、不增加模型调用次数、不放宽证据与发布门槛。
  旧自定义评分格式继续兼容；新模板使用逐轮格式。模型语义判断仍需真实复核，协议通过不等于判断必然正确。
- 范围：组件任务评分、外置循环评审提示词、回归和说明文档；未修改 AgentFlow C++ 底座或主 Agent 的流程。
- 定向回归：59 项任务评分用例及冷启动补库、文档套件通过。真实两题均使用逐轮格式完成评分；
  必要回复、skill 上报和读回验证均标为 keep，未再出现理由明确说“不能省去”却被计入浪费的矛盾。
  此轮仍暴露业务验收误判，按下一项处理，尚不能用于宣称实现收益。

### CHG-20260911-202：任务验收先核对强制动作，再给总判定

- 问题：真实 JSON 修复题未实际执行解析器，模型再次以“写入和读回成功”判整题通过；
  这会使不完整业务执行成为成功训练反馈，与冻结标准不符。
- 修改：只调整任务验收的外置提示词和请求契约说明，要求先逐项核对实际动作及适用条件分支、最后给判定，
  补充权限拦截不等于业务错误、部分产物成功不能替代必需动作的失败示例。
- 边界：不更改原任务、成功标准、权限保护或评分代码的通过门槛，不将历史评分直接改成预设答案。
  仍须用真实执行结果检验模型判分，提示词本身不是语义正确的证明。
- 真实复测：JSON 修复题一次模型评分即正确判 failed，明确区分被拦截的解析动作与成功的文件写入/读回。
  6 个观测轮次全部有逐轮判定，必要尝试及收尾保留；没有把业务未完成误记为评测执行错误。
- 固定构建后的完整回归：30/30 个 CTest 套件通过（399.73 秒），包含 59 项任务评分用例。
  随后使用冻结评分配置重启真实双用户空库实验，尚未据定向复测结果宣称 GEPA 或收益目标完成。

### CHG-20260911-203：打点按配置保留工具业务失败

- 问题：真实 v1 工具返回 JSON 中含 failed、security_policy_blocked 或非空 error，但函数正常返回，
  通用打点默认将 action.end 记为 completed。错误原文仍在，结构化失败指标却缺失。
- 修改：现有 instrumentation 增加可选 action_outcomes，以 JSON Pointer 和精确值/非空条件映射动作状态及固定错误码。
  统一作用于原生和 HTTP 的 point 标准化，不增加宿主函数、语言适配器或模型请求。
- 边界：默认关闭；显式宿主状态优先。只处理实际执行的 action 正常返回，不改原始 output、模型或整个 Run 的状态，
  不影响并发兄弟分支。重试属性由配置明确给出，不从任意错误文案推断。
- 影响检查：point_request、validate_instrumentation 及边界测试；已核对原生/HTTP、引用式打点和离线回放的共同入口。
  无 AgentFlow C++ 底座修改。在途实验保持原二进制与配置，本修复先在独立构建目录回归。
- 定向验证：IntegrationServiceTests 通过，覆盖对象/JSON 文本、显式状态优先、未执行/中断隔离、
  空错误和非法配置；并发动作中一个映射为失败，另一个正常完成，最终 Run 仍正常闭合，原始工具结果不变。
- 独立构建后的组件回归：30/30 个 CTest 套件通过（278.83 秒）。在途真实实验继续使用冻结版本，
  该通过结果不代替下一次真实采集对新映射的验证。
- 后续真实 v1 隔离复测：3 个完整 Run 中，3 次安全拦截及 1 次文件读取失败正确映射为 action.failed，
  原始工具结果不变，三个 Run 的执行状态均为 completed；业务评分仍分别保留通过/失败，未提升为整轮执行故障。

### CHG-20260911-204：循环引用恢复定位到具体事件，并保留最终诊断

- 问题：60 题真实基线中仍出现 event_not_in_iteration_or_prior_context；恢复只给约束和评审条目下标，
  模型无法直接定位该项中的错误引用。恢复耗尽后又只保存约束名称，丢失条目位置，复核困难。
- 修改：仅在具体事件引用失败时，反馈增加对应轮次及 keep/waste/uncertain 判定、引用下标、限长的错误事件 ID 和本轮真实事件列表；
  模型须重新检查结论与证据，不允许为了通过格式校验而改用无关引用。最终失败的安全诊断写入
  evidence.loop_review.validation_error，恢复成功不残留该字段。
- 边界：原有本轮锚点、先前因果范围、跨 Run/未来/重复引用拒绝、重试次数、评分门槛和隐私检查全部不变；
  不保存未通过校验的模型判分，不修改已生成的分数或在途实验配置。无 AgentFlow C++ 底座改动。
- 影响检查：validate_loop_review、model_grade 及既有恢复测试；图索引对新增组件仍为 UNKNOWN，
  已核对直接调用链为任务评分、后台 Agent 评测与 GEPA，独立构建验证后再用于后续真实复测。
- 验证：60 项任务评分测试与文档定向通过；随后独立构建全量 30/30 个 CTest 套件通过（398.52 秒）。
  覆盖具体错误事件、无重试时保留诊断、恢复后清除错误，以及引用限制和未知效率不变。
- 真实复测：原 daemon、Bridge、DataBus 的 3 道隔离题均以 1 次模型评分形成完整评审，0 个协议错误；
  2 题业务通过，核算题因未实际运行代码正确保持失败。天气评分引用与工具返回一致，必要最终交付未计为浪费。
  该复测没有触发新的引用恢复分支，因此不将其视为模型恢复能力或全部语义偏差已经解决的证明；
  不写入正式经验库，不代替冻结基线、GEPA 或最终收益对照。
- 补充定位：诊断保留该轮 decision 后，60 项任务评分与文档测试再次通过。真实视频题经 1 次协议恢复
  形成完整的 15 轮评审，业务结果因重复尝试保持 failed，评测 errors=0；未扩大引用范围或修改已有评分。
  该结果只证明此次有界恢复成功，仍需继续核对探索与重复尝试的语义判断。

### CHG-20260911-205：明确评测事实来源、禁止尝试和事件引用

- 问题：真实基线的文件缺失题中，评分理由承认又读取技能目录，仍将“不得尝试从其他路径获取”判为满足；
  两题还在有界恢复后因证据路径错误未完成评分。较早复核也发现用户场景参数与工具实测值混淆。
- 修改：仅补充任务/循环评测提示词。区分用户描述、Agent 陈述与工具事实；按标准分别核对行为禁止项和结果禁止项；
  明确事件 ID 不等于数组下标，无法核实位置时引用允许的上层路径；新参数本身不构成合理探索的依据。
- 边界：不增加模型调用，不扩大证据权限，不改变任务标准、评分门槛、循环定义或重试次数，不修改 AgentFlow 底座。
  提示词由任务评测共用入口加载，影响独立评测、后台评测与 GEPA；在途实验保留冻结提示词，修复单独验证。
- 验证：60 项任务评分测试与文档测试通过（36.79 秒）。两道真实 v1 复测均完成执行，
  但各经 2 次评分仍被循环引用校验拒绝，不能认定语义评测已修复；未接受这些模型分数，也未写入经验库。
  错误诊断分别定位到 keep 引用了下一轮文件读取结果、waste 引用了下一轮输入里的空调度报错。
  后者的 run_skills 结果只进入原 hook 和下一轮消息，当前通用打点未独立记录本轮调度结果；
  该采集缺口待补齐，不通过放松未来事件限制来规避，也不在在途基线中混入新的打点版本。

### CHG-20260911-206：单次结果打点保留所在循环

- 问题：通用 phase=event 打点只传递活动关联，处于循环上下文但没有活动的调度结果会丢失轮次归属。
  v1 还需补充独立的调度结果打点，不能以扩大模型的未来事件引用范围替代采集。
- 修改：进化包 event.record 增加可选 control_id；单次事件优先沿用活动，否则沿用明确的循环上下文。
  两类关联互斥、限于同一 Run，不按最新事件或并行分支猜测所属轮次；无关联的旧调用保持 Run 级事件。
- 影响范围：原生/HTTP 共用的打点与事件记录路径，同步维护接口 schema 和协议说明；
  不启动操作、不应用策略、不改变 loop 计数规则、不修改 AgentFlow C++ 底座。
  v1 调度结果补点见下一项；原冻结实验已停止，不向在途基线混入新的采集版本。
- 验证：真实接口封装路径的 Run → loop.guard → model → 单次结果 → 下一轮测试通过；
  覆盖并行循环显式归属、请求幂等、活动优先、跨 Run 拒绝、无关联旧事件兼容。
  IntegrationServiceTests、IntegrationOpenApiContractTests 通过；接口 JSON Schema 同步生成并验证。
  补齐 v1 打点后，进化包全量 CTest 30/30 通过（含 GEPA、跨语言接口、私有/共享记忆和任务评测）。

### CHG-20260911-207：v1 在本轮记录 Coordinator 工具结果

- 问题：run_skills 参数错误、重复拦截或调度失败时，不产生 Skill 分支；结果虽已进入原 Hook 和消息历史，
  通用采集却要等下一轮输入才能看到。模型评审因此可能把当前轮的失败引用到下一轮，触发未来证据校验拒绝。
- 修改：在工具结果产生后、加入消息历史前补一个通用 tool.result 事件，保留原工具名、调用 ID、参数和返回值；
  事件引用键由决策序号和该响应内工具序号组成。Coordinator 循环标记 actor_id，由包继承明确轮次关联。
- 影响范围：v1 execute 的工具结果出口，经 main 和节点入口调用；不依赖旧 Hook，不新增宿主辅助文件，
  不改变业务返回、派发、重试或 C++ 底座。事件不创建活动，不引入待闭合 span，仍经过现有隐私检查。
  GitNexus 已定位 execute 和回归测试；当前索引遍历不完整，未将 UNKNOWN 当作无影响，已核对实际入口与调用处。
- 验证：test_coordinator_run_skills_force.py 33 项通过，包含 manifest 写入失败、空 skills 参数拒绝，
  断言结果只记录一次、位于纠错模型调用前、业务状态不变。相关其他测试 24 项通过；
  resource 示例的 response_key_deleted_after_dispatch 旧夹具未提供可用 Skill，未进入派发分支，
  用 HEAD 版本 execute 复现同一失败，非本次补点回归；未通过修改生产行为使该夹具通过。
- 真实复测：t-06/t-09 共 2 个闭合 Run、14 次模型调用，thinking token 均为 0；
  无重复事件 ID、向后缺失因果引用或未闭合活动，未加载策略。t-09 的空 skills 返回已作为 event-76
  归入 Coordinator 第 3 轮，不再只出现在第 4 轮输入。8 个程序产物扫描未发现配置中 2 个已知密钥值。
  t-06 一次评分通过，t-09 仍因模型构造了无效证据路径而评分失败，未据此接受评分或入库；后续修复见下一项。

### CHG-20260911-208：默认评分使用明确证据地址，并落盘路径失败诊断

- 问题：真实图片任务已执行完成，但模型连续构造不存在的 JSON Pointer，恢复后仍失败；
  最终报告只保留 evidence_invalid，无法直接看到被拒绝的具体路径。
- 修改：外置默认任务评分模板从 allowed_evidence_paths 原样选择根级路径，具体 Run、事件 ID 和事实写入理由；
  不减少模型可读的原始内容，不改变循环引用校验。自定义模板的合法子路径继续兼容。
  路径校验最终失败时，在 evidence.judge_validation_error 保存代码和已有隐私、长度保护下的错误路径；
  不保存未验证的原始评分，恢复成功不残留错误字段，不增加重试。
- 影响范围：进化包任务评分 model_grade，由单条 trial 调用，供离线 dataset run 和后台/GEPA 评测复用；
  图索引未覆盖该符号，已核对真实调用处。无 AgentFlow C++ 底座改动。
- 验证：TaskEvaluationTests 与 DocumentationTests 通过；合法子路径仍可评分，非法路径继续拒绝并保留诊断，
  成功恢复不残留诊断。真实复测待确认，不能仅以协议测试通过认定模型评分已稳定。

### CHG-20260911-209：响应测试旁路保留 DataBus session 上下文

- 问题：真实 v1 结束时，旧 put_e2e_response 直接调用 raw().put，未携带 UDS 所需 session/namespace，
  持续产生 Invalid params 警告。正常答案经 session_router 返回，旁路报错不是任务评分失败的原因。
- 修改：改用已有 put_global，写入 session sys 下无 dialogue 前缀的 `_e2e_response`，同时沿用脱敏；
  返回 False 仍打印警告，未连接仍跳过，异常不传播到业务主路径，保留可选 timing。
- 影响范围：v1/v2 共用的响应测试旁路；图查询未给出完整调用图，已逐一核对导入和调用，
  以及 DataBus put_global 和现有 contract 的 session namespace 语义。未修改 C++、正常响应或进化接口。
- 验证：补充连接正常、写入失败、未连接与 timing 开关组合，断言不访问 raw、不影响函数返回。
  当前 runtime 相关测试共 63 项通过；真实复测结束未再出现该旁路警告。

### CHG-20260911-210：区分空调度校验失败与真正交付

- 问题：修复证据路径后，两道真实任务各经一次模型评分完成，但 t-06 将已有明确工具契约下的空调度
  判为“向用户交付前的必要步骤”；该轮未派发工作也未发送最终答案，实际交付在下一轮，浪费计数被低估。
- 修改：仅补充外置 loop-review 提示词，明确调用/校验报错不等于交付；已知不合法、无新信息且无其他工作
  的空调度与真正交付分开判断。保留首次探明未知限制、产生必要新信息和同轮有其他工作的例外。
- 影响范围：默认逐轮模型评分，不改 Agent 行为、工具校验、采集数据或计数代码，不自动改写模型分数。
  两次协议成功不能证明评分语义稳定，仍需真实复测。

### CHG-20260911-211：run_skills Schema 与已有非空校验对齐

- 问题：执行端拒绝空 skills，但模型收到的数组 Schema 未声明 minItems；形式约束与执行契约不一致，
  增加了空调度后再纠错的机会。现有系统提示虽要求无工作时直接回复，工具声明仍应一致。
- 修改：仅在既有 skills 数组声明 minItems=1，并说明无工作应直接回复，不以空列表结束任务。
  不改变可用 Skill、并行行为、执行侧校验、成功条件或进化接口；无 C++ 修改。
- 影响范围：RUN_SKILLS_TOOL_DEFINITION 经 v1 的 _init_skills 深拷贝后发送给模型；已做图影响查询并核对导入。
  当前真实两道任务的 Agent 均已结束，仅评分等待中，本改动不追改已记录的请求与轨迹。
  后续基线和候选均从同一修复版本运行，此人工契约修复不计作 GEPA 的策略收益。
- 验证：增加声明与非空参数校验的一致性测试，验证空列表拒绝、合法单项仍接受。
  当前 runtime 相关测试 64/64 通过。

### CHG-20260911-212：可选无损共享重复的评测输入结构

- 问题：真实 10 次模型调用对应的轨迹有 30 份 messages 和 tools；工具定义约 146 KB，独立内容约 14 KB。
  旧打包只共享长字符串，重复数组/对象仍增加请求体；本次评分连接发送约 550 KB 后长时间等待响应。
  重复内容已确认，尚不能据此断言全部远端等待都由请求大小造成。
- 修改：增加可选 judge.share_repeated_inputs（默认 false），在原有事件输入去重函数里共享完全相同的
  大块 JSON 值，涵盖消息数组和工具定义对象。首次来源地址保留，后续引用可按显式表递归还原，引用关系无环。
  旧 share_repeated_text 模式仍只共享文本；自定义模板需自行选择新选项并遵守嵌套引用的读取契约。
- 边界：仅组装评测模型请求，原始轨迹和报告不变；输出、状态、产物直接保留。引用前后均检查隐私，
  只在打包后更小时采用，不放宽请求上限、证据校验或模型判分标准。无 AgentFlow C++ 底座改动。
- 影响范围：compact_judge_evidence 仅由任务 model_grade 调用，选项由 initialize_model 验证；
  图索引未覆盖这些符号，已核对实际调用和配置入口。运行中的旧预检仍使用旧请求，未改其证据。
- 验证：TaskEvaluationTests、DocumentationTests 通过；覆盖旧文本还原、新结构递归还原、转义路径、
  不可信同名引用对象、输出/状态不变、请求进一步缩小、敏感数据先拒绝、非法选项在执行前拒绝。
  最终全量 CTest 30/30 通过。开启新打包的真实 v1 t-06/t-09 复测 2/2 通过，评分各调用模型一次；
  t-06 的空调度正确记为 1 轮 waste，真正交付保留，t-09 生成图片成功并评为 0 轮浪费。
  两个 Run 共 136 事件、14 次模型调用，全部闭合、无重复 ID 或缺失前置因果，thinking token 均为 0，
  无策略注入；8 个程序产物未检出配置中 2 个已知密钥及其常见编码形式。未作同轨迹的严格时延对照。

### CHG-20260911-213：任务禁止项优先于一般重试建议

- 问题：真实冷启动训练中，视频题明确允许工具失败后报告并停止、禁止重试，模型却把一次已重试的执行判通过；
  另一执行虽判失败，理由错误地说标准不允许失败分支，并把换模型重试评为必要探索。
  同时发现任务 constraints 已传给执行端，却未进入评分请求，单独写在该字段中的约束可能漏评。
- 修改：任务评分补传已有 constraints；提示词和输出理由契约明确分别核验必需动作、条件分支和禁止项。
  明确禁止后的重试属于业务违规，不能被“效率差不等于业务失败”或一般替代建议覆盖；
  保留合法失败分支、必要收尾及混合工作轮的独立判断，不自动改写模型分数。
- 影响范围：共用任务评分入口及两段外置提示词，供 dataset run、后台评测和 GEPA 使用；
  已做图影响查询并核对 model_grade 的 trial 调用与模板加载处，索引 UNKNOWN 未当作无影响。
  不修改 AgentFlow 底座、数据集、评分门槛或重试预算，不新增模型调用。
- 验证：增加 constraints 原样进入评分、禁止项契约和默认模板加载的回归断言；真实评分语义仍待复测。
  发现问题后已主动停止本轮主进程，不把这批评分用于后续收益结论；前一用户 8 题协议均有效，
  但其中 t-04 的通过结论存在上述错误，协议通过不能替代语义核验。
  首轮真实复测已正确拒绝再次生成视频，但另一次把仅 echo 已知错误的 bash 调用误认成视频重试；
  继续限定禁令的操作对象，区分重做失败业务、打印错误和空调度，不把效率浪费自动升级为业务违规。
  后续真实复测中，两次确有视频重试的执行判失败，一次仅生成一次并报告失败的执行按合规失败分支判通过；
  另一条 17 次模型调用的长轨迹仍因两次 invalid_json 无法评分，保留未评测状态，未记为零浪费。
  TaskEvaluationTests（61 项）、DocumentationTests、AgentEvaluationTests、GepaSearchTests 均通过；
  无效 loop 仍是模型估计，逐轮边界判断继续复核，不把这些预检作为 L4 收益证明。

### CHG-20260911-214：无效评分 JSON 提供无正文解析诊断

- 问题：真实长轨迹在已开启 JSON 输出模式后仍两次返回 invalid_json，旧报告只有统一错误码，
  无法区分缺少分隔符、字符串转义、编码等语法失败，也无法让有界恢复收到具体错误位置。
- 修改：任务评分沿用相同 RapidJSON 解析标志与 Json::convert 校验，在语法失败时记录静态解析错误描述、
  解析码、字节位置和响应字节数；反馈到既有恢复请求，最终失败写入 judge_validation_error。
  不保存或回显未通过校验的响应正文，恢复成功不残留诊断，不改变格式接受范围、评分或重试预算。
- 影响范围：仅任务 model_grade 路径及现有格式恢复测试；已做图查询并核对 trial 的唯一直接调用，
  不修改共享 Json 解析器、AgentFlow 底座、宿主运行或数据集。
- 验证：补充禁用恢复时诊断落盘、恢复时具体反馈、成功无残留和无正文断言，全量 CTest 30/30 通过。
  同一条真实长轨迹复现 8,393 字节响应在第 0 字节非法值；后续重评也有合法 JSON，起始格式问题仍在定位，
  不保存原始响应、不将偶发成功当作已修复。

### CHG-20260911-215：逐轮评审允许引用已汇入的异步子任务结果

- 问题：真实 Coordinator 第 3 轮派发图片任务，子任务生成成功（event-113）经结束事件汇入父任务（event-122），
  但旧校验只接受本轮事件或本轮开始前的事件，错误拒绝这条确有因果关联的结果。
- 修改：在同一 Run 的已验证 DAG 中反向遍历本轮事件的 caused_by，允许这些因果前置事件作为附加证据。
  仍须至少一个本轮锚点；不以时间重叠或父节点的全部后代作为依据，不接收未汇入的并行结果、无关后续事件或其他 Run。
  本轮开始前是否已知信息仍由评审区分，原有轮次划分、分支计数、业务门槛不变。
- 影响范围：仅任务逐轮证据校验、外置提示词及文档；已进行图影响查询并核对 model_grade 的直接调用。
  不改采集协议、AgentFlow C++ 底座、Agent 行为或模型调用预算。
- 验证：新增异步汇入、无锚点、时间重叠但未汇入、未来事件及乱序输入回归；TaskEvaluationTests 62 项及文档测试通过。
  同一条真实长轨迹重新评分一次即返回合法评审，但无效轮次出现新的语义分歧，另列下项处理，未启动收益对比。

### CHG-20260911-216：区分业务必要性与重试链条的结构依赖

- 问题：同一真实轨迹的逐轮评分出现 8 轮与 0 轮浪费的分歧；0 轮版本承认多次重试违反停止要求，
  却以“属于重试的一部分”“包含分析和决策”为由全部保留，混淆内部步骤依赖和业务必要贡献。
- 修改：仅补充外置评审提示词，要求 keep 的额外贡献有实际事件支持；内部分析、参数准备、重复错误记录、
  后续日志引用本轮，不单独构成必要工作。不要求语义上省去无用操作后原始消息和 DAG 逐字不变；
  同时保留首次必要交付，避免把无效重试和必要结果报告重复计算。
- 边界：不改轮次计数、模型分数或通过门槛，不把模型估计改称确定性事实；语义稳定性待真实重评。
- 验证：修改后同一真实长轨迹连续两次重评均一次调用完成，业务判失败、无效轮次均为 8；
  只证明该案例的复核一致，不能保证所有模型判分准确。预检 4 个 Run 共 368 事件、38 次模型调用、17 次工具调用，
  未发现步骤未闭合、重复事件 ID 或缺失前置引用；10 个产物未检出配置中已知密钥及其常见编码。
  正式冷启动从保留的原 60 题数据集重新开始，预检与诊断结果不入训练库；偶发 JSON 格式失败继续监测。
  修改后全量 CTest 30/30 通过。正式新批次 t-01 的必要上游结果报告仍被模型记为浪费，
  该语义疑点保留待复核；不能仅凭格式通过或诊断案例一致宣称逐轮评分完全可靠。

### CHG-20260911-217：训练轨迹分析保留任务验收依据

- 问题：真实 t-08 要求记录解析器实际失败并验证修复。解析命令被安全策略拦截后，Agent 只写入和读回文件，
  任务评分正确判失败；训练导入却只传轨迹和来源 ID，未传验收依据，后续分析误判整体成功并生成替代验证经验。
- 修改：仅在通过完整性、L0、train、版本校验的导入副本中补充 attributes.training_evaluation，
  包含原任务标准、约束、整场试验的判分与检查、对应 Run 列表。已有通用内容通路把它送入分析模型。
  原报告、执行事件与 Agent 请求不变；提示词要求核对真实执行证据，并区分多轮试验整体结论与当前 Run。
- 边界：验收上下文不是新的工具执行结果，不无条件覆盖模型判断；使用既有隐私与大小检查，
  val/holdout、非 L0 和普通评测轨迹仍不可借此入库。相同已验证报告重复导入仍幂等，旧导入不回填、不重分析。
- 影响范围：组件内 import_training_report 和 import_source，分别覆盖后台冷启动与显式训练导入；
  已做图影响查询并核对调用处，不修改 AgentFlow 底座或在线打点。主要回归点为导入幂等、多轮范围和证据完整性。
- 验证：新增验收上下文、事件不变、旧记录幂等、上下文凭据拒绝及模型请求可见性断言；
  隔离构建完成，全量 CTest 30/30 通过（304.04 秒）。真实模型复测仍待完成，不能据此认定语义误判已消除。
  原实验运行期间未替换二进制或冻结提示词；待其自然结束后，随 CHG-218/219 编译并用于新一轮冷启动。

### CHG-20260912-218：精简模型输入与减少本地重复处理

- 问题：同一模型请求在步骤开始、动作开始和动作结束中重复记录，每轮又携带累积历史。
  直接用于离线分析造成正文重复、输入膨胀；真实 214 事件轨迹超过 2 MiB，未能发起分析。
  其他入口还重复发送结构分析、存储字段或组件正文；任务评分的全局锁覆盖 HTTP 等待，使已配置的并发串行化。
- 修改：只精简模型请求副本，原始轨迹、报告、证据和版本仍完整保存。
  内容分析始终共享相同脱敏值，保留每次执行与因果身份；移除重复事件头、结构指纹等非语义字段。
  任务验收不再传整份规则分析与存储元数据，默认共享重复输入；验收仍引用原始轨迹完成校验。
  元反思、记忆匹配、公私关系和数据集生成只传各自所需语义字段；GEPA 保留训练任务、标准、约束与实际操作反馈，
  组件正文只传一次，不再回传累积模型历史。未引入额外的模型摘要调用。
- 提速：脱敏缓存限定单请求，保留原文与结果合计最多 4 MiB、512 项，不跨用户或请求复用、不落盘，
  按每次出现累计原有审计计数。重复值共享表也有大小、条数和路径上限，放不下的内容保留原文。
  内置 HTTP 评分适配器只在锁内预留调用预算，网络等待按已有 parallelism 并行；自定义适配器保持串行约定。
  不改默认并发、超时、调用预算、评分门槛、训练/验证隔离或 AgentFlow 底座。
- 影响范围：内容分析、任务验收、元反思、记忆管理、数据集生成和 GEPA 的请求装配；
  已做图影响查询并核对实际调用处，索引无法解析的新入口没有作为低风险依据。
- 验证：精简前后同一真实样本的用户输入分别为 43,592→21,903 字节、562,705→147,379 字节；
  原先超限的 2,830,012 字节长轨迹现在输入 464,986 字节，保留 214 事件、60 个评测单元。
  单次本地组装实测为 67→50 ms、487→304 ms、2178→964 ms，包含读取、分析与写出，并非纯模型时延基准。
  小/中样本无截断；长样本原有的 42 次内容截断与 129 次脱敏计数未增加或丢失。
  长样本使用指定 API 一次调用成功返回完整 60 项评测。添加引用递归还原、特殊路径、伪引用、脱敏计数、
  唯一内容仍拒绝超限和 HTTP 并发回归；全量 CTest 30/30 通过，最新边界调整的定向回归通过。
  输入缩小不意味着外部 API 耗时同比下降，也不代表 L4 收益已达到验收要求。

### CHG-20260912-219：合并冗长评分规则，明确逐轮判定顺序

- 问题：任务与循环提示词反复追加案例，合计 16,741 字节；真实评分仍会将已违反重试禁令的操作称为必要探索，
  或将不同派发实例的相同轮次混淆。协议恢复重复引用错误事件，又增加等待。
- 修改：仅整理两段外置提示词，合并同义规则并明确先核验约束、再核验独立贡献、最后核验交付。
  任务验收保留分支、禁止项、实际执行与产物证据边界；提示理由简短列出事实，不复述全文。
  默认逐轮输出只选本轮清单中 1–2 个直接证据，前提事实在理由中说明；完整上下文仍供模型阅读。
  后端对既有调用方的因果证据校验范围、返回格式、调用预算和评分门槛不变，不自动把模型分数改成预设结果。
- 验证：合并后初版提示词约 7.7 KiB，任务评分与文档定向回归通过；真实 t-01 一次调用完成，
  写入、上游结果报告和最终交付 4 轮均保留。长任务的引用与效率结论仍继续复核，不把格式成功视为语义正确。
  默认本轮锚点规则复测后，真实 t-10 一次调用（45.6 秒）返回 18 轮完整评审，6 轮违规重试/无依据操作为 waste，
  业务判失败且所有证据引用通过；原先 invalid_json 的 t-15 经一次有界引用修复完成 11 轮评审。
  该轮冻结系统提示词为 7,862 字节，较 16,741 字节减少约 53%；这些样例不构成模型判分永远正确的保证。
  当前实验 378 个文本产物共约 329 MB，未检出配置中已知密钥及其检查的常见编码；该检查不等于覆盖所有隐私类型。
  最终全量 CTest 30/30 通过（263.76 秒）；正式程序重新编译，冻结数据集保持原样，未将诊断重评导入经验库。
  新一轮 t-06 中，完整轨迹明确未做文件检查，模型却以文件存在性未知判 uncertain。
  继续补充已确认必需动作缺失优先判 failed 的顺序，不把 Agent 没做检查与采集缺失混为一谈；
  仅调整外置提示词，同一轨迹单独复核，不回写原评分、不热换在跑实验的冻结配置。
  该轨迹复核一次调用 7.56 秒，正确判 failed，循环与引用校验通过；补充默认提示词长度及判定优先级回归。
  补充后源码模板合计 8,107 字节，仍比原版减少约 52%；64 项任务评测测试及文档测试再次通过。
  新实验首个用户的 8 条导入轨迹均保留训练验收上下文，8 次分析全部成功，元反思成功，私有库生成 6 条候选记录。
  其中满足当前编译条件的经验为 1 条，程序按原门槛继续补充训练，尚未发布 L4 策略。
  第二用户 t-05 的真实执行已闭合，但评分两次引用了不属于本轮或前置因果上下文的 event-35，
  被 evaluation_loop_review_invalid 拒绝；保留 error，不补造引用、不记为零浪费。该输出稳定性问题仍待后续复核。

### CHG-20260912-220：复用逐轮校验结果并补齐锚点恢复上下文

- 问题：同一任务逐轮评审时，每一轮都复制、解析、完整性验证并排序整条 Run，长轨迹重复本地计算。
  missing_iteration_anchor 原来只反馈评审下标，未像其他引用错误一样明确提供对应轮次和本轮事件。
- 修改：校验函数内按原始 Run 数组位置惰性缓存已验证排序；每个匹配 Run 在一次评审中只解析、验证一次，
  不跨调用、用户或协议恢复复用，也不省略其他 Run 的验证。缺少锚点的反馈补充 iteration_id、decision 和
  allowed_iteration_event_ids；不改变引用合法范围，不自动补引用、不改变判分或恢复次数。
- 影响范围：组件的 validate_loop_review 及测试、使用说明；已运行图影响查询并核对 model_grade → 校验的调用。
  不修改 AgentFlow 底座、日志事件、线上打点或在跑实验的冻结二进制与配置。
- 验证：新增禁用恢复时保持 error/未知计数、恢复收到本轮事件、成功无残留诊断的回归；65 项任务评测测试通过。
  本轮真实 t-16 三个 Run、183 个事件单独复核仍连续两次缺少首项锚点，输入 387,263 字节，
  单次调用与诊断约 15.2 / 10.2 秒。仅补充反馈尚未解决模型错引，保留错误，未回写正式评分或导入经验库。
  后一次同轨迹诊断一次调用 47.3 秒完成 19 轮评审，业务判失败、无效轮次为 3，引用全部通过；
  该结果只证明此条评审完成，不把偶发成功视为错引问题已稳定解决。全量 CTest 30/30 通过（265.22 秒）。

### CHG-20260912-221：GEPA 复用部分批次中的有效单题成绩

- 问题：逐题缓存只读取整份 report 已评分的批次，某一题出错会让同批有效题目也失去复用资格。
  本轮真实 L1 搜索中，相同版本的 t-03 和 t-27 已有有效成绩，后续重叠批次仍重复执行。
- 修改：补测时可读取同版本、同 split、同上下文的其他已结束 inconclusive 报告，按题检查全部 repetitions
  及当前目标所需指标；通过现有 scores 校验后才复用。正常业务 failed 仍是有效观测，评分错误、uncertain
  和缺失必需指标不能充当成绩。保留报告哈希校验，完整批次总分和发布门槛不放宽。
- 边界：不跨用户实验、策略版本或 split，不从最终留出对照取样用于搜索；关闭缓存仍完整重测。
  不修改在跑实验或 AgentFlow 底座。已执行图影响查询并核对训练、验证、合并及最终对照的实际调用。
- 验证：新增评分错误、循环指标未知、关闭缓存 3 项回归，通过本地隔离 Agent 与模型响应 fixture 验证：
  三个交叉小批次从 6 次训练试跑降为 4 次，只重测出错/缺指标的题，已评分的业务失败不重复试跑。
  不把该局部测试减少的次数当作真实 Agent 的无效 loop 收益；正式 L4 门槛仍未通过。
  全量 CTest 30/30 通过（282.20 秒），其中 GEPA 集成测试 32 项；在跑正式实验未替换二进制。

### CHG-20260912-222：限制传输重试总等待

- 问题：HTTP 适配器每次重试又获得完整 timeout_ms，配置 30 分钟、重试一次时，最坏网络等待接近 60 分钟。
- 修改：HTTP 阶段使用同一单调时钟截止时间，连接、响应和退避共用 timeout_ms；后续尝试只使用剩余时间，
  不把耗尽时间重置。max_retries 仍限定次数，短暂故障仍可恢复；协议修复仍单独计次，网络故障不进入协议修复。
  参数仍来自外部配置；未缩短用户设置的 30 分钟，未修改 AgentFlow 底座、评分标准或用户 API 配置。
- 影响范围：组件共用 HTTP 适配器及回归、配置说明。已做图影响查询并核对任务评分、轨迹分析、
  记忆管理、数据集生成及 GEPA 的实际调用；UNKNOWN 未当作无影响。不热换在跑实验的冻结二进制和配置。
- 验证：新增总预算耗尽、部分时间后重试、503 恢复及 401 不重试回归，覆盖网络错误不触发语义恢复；
  定向 3 项通过；撤回输出共享后的最终代码全量 CTest 30/30 通过（278.41 秒）。
  真实 t-05 压缩尝试一次调用 29.24 秒完成，业务仍判 failed、10 项评审引用有效，
  但无效轮次从 1 变为 3，其中把首次向上游交付结果误判为可省。因此未保留事件输出共享，
  继续直接发送实际结果正文；不能把协议成功或输入更小视为评测语义准确。诊断未改原评分或入库。
  本次 3 个诊断产物未检出配置中 2 个已知密钥及检查的常见编码；冻结 60 题数据集 5 个文件哈希未变。

### CHG-20260912-223：当前调用为经验检索提供默认状态

- 问题：统一活动打点未显式提供 state 时，检索只看到 run 初始任务，无法利用后续工具结果。
  本轮真实 L1 种子验证的 97 次模型策略查询，state 全为字符串 null；其中 36 次实际请求已有工具错误信息。
  这会让按运行中问题触发的经验缺少检索依据，不能仅凭已加载策略版本判断注入有效。
- 修改：仅在活动未提供 state 或值为 null 时，从当前 input 提取检索副本。system_message 模式沿已配置的
  binding JSON Pointer 读取消息，排除 system/developer 静态指令；其他模式和动作使用当前 input。
  显式 state（含空字符串、空对象、false）保持优先。沿用现有首尾裁剪与字节上限，不新增配置或模型调用。
  不读取其他并发分支、不改原请求和轨迹正文；L0、apply_policy=false 与 loop.check 原有行为不变。
- 缓存边界：执行语义变化加入既有评测上下文指纹，旧报告保留，但不直接续用旧搜索或旧发布准入结果；
  同一新上下文仍可恢复并复用有效成绩，避免新旧执行方式混测。离线策略文件格式不变。
- 影响范围：组件共享活动边界、后台评测上下文及回归/说明；已运行 scoped 图影响查询并核对原生、HTTP、
  configured point 的实际入口。索引未解析的符号未作为无影响依据；未修改 AgentFlow v1 或 C++ 底座。
- 验证：先复现无 state 时未注入，再覆盖嵌套/转义/数组路径、模型别名、纯文本、动作、显式空状态、
  静态指令排除、并发兄弟分支隔离、原始输入保留和注入回执。评测上下文更新与重启加载回归通过；
  隔离构建全量 CTest 30/30 通过（279.77 秒），git diff --check 通过。
  已有真实轨迹本地回放 97 次旧检索全部与原记录一致；使用新默认状态后 70 次检索结果发生变化，
  未调用模型、未改正式成绩或记忆库。该结果证明缺失状态影响检索，不等于真实任务成功率或无效 loop 已改善。
  在跑实验仍使用冻结旧版，未热换二进制、配置或清理其产物。

### CHG-20260912-224：给逐轮评分提供可复制的真实事件锚点

- 问题：应用前述修复后的真实冷启动中，t-02 连续两次将技能第 2 轮的 event-31 引用给第 1 轮，
  被 event_not_in_iteration_or_prior_context 正确拒绝。自由选择事件引用仍会触发额外评分与协议恢复。
- 修改：在既有 loop_inventory 中增加 anchor_event_id，来自该轮一条真实已执行的 action.end。
  默认提示词要求逐字复制这个定位 ID，贡献和前提仍根据整轮原始内容判断、在理由中说明。
  不新增模型调用、不改变轮次分组、业务标准、keep/waste 判定或后端证据校验；不替模型补写引用。
  自定义评分器仍可返回原有合法的多证据格式，未执行动作不能生成这个锚点。
- 影响范围：任务评测输入清单、默认逐轮提示词、回归及说明。已执行 scoped 图影响查询，
  并核对 loop_inventory → model_grade → trial 的实际调用；未将 UNKNOWN 当作无影响。
  不改 AgentFlow 底座，不热换正在运行的实验配置。
- 验证：新增真实 action.end、执行标记和同分支/同轮次归属断言，先复现缺少字段，再通过补丁回归；
  保留并发、乱序、父子因果、非法未来引用拒绝等测试。隔离构建全量 CTest 30/30 通过（279.56 秒），
  git diff --check 通过。单条真实 t-02 诊断输入为 8,192 字节系统提示词及 136,404 字节用户输入，
  4 个锚点均属于对应轮次，未检出配置中 7 个已知密钥及检查的常见编码。
  首次 t-02 请求在总预算 30 分钟耗尽后返回 transport_28，未重置等待预算；同轨迹重试 13.15 秒完成。
  再复核 t-07、t-16，分别 34.16、31.49 秒完成，三条共 30 轮的引用均通过校验，均只用一次评分调用；
  t-02 判 passed，t-07、t-16 仍因违规重试等业务问题判 failed，不把引用修好当业务通过。
  诊断未回写正式成绩或导入经验库。停止旧进程后才部署补丁并从空经验库重启，保留原 60 题数据集；
  单条复测不代替完整 L4 准入与收益验证。

### CHG-20260912-225：为已配置的传输重试保留等待预算

- 问题：共享总截止时间后，首次请求仍可独占全部 timeout_ms；真实实验出现请求等待 30 分钟后失败，
  相同输入再次调用十余秒成功的情况，已有 max_retries 却没有剩余时间恢复。
- 修改：每次 HTTP 尝试使用剩余总时间除以剩余可尝试次数，未用完的时间留给后续尝试。
  连接、响应及退避仍共用原截止时间；max_retries=0 保持一次使用完整预算，401 等非暂态错误不重试。
  不新增参数、不改变模型输入、评分标准、协议修复次数或 AgentFlow 底座，30 分钟总上限保持不变。
- 影响范围：组件共用 HTTP 适配器，涉及轨迹分析、任务评分、元反思、记忆管理、数据集生成及 GEPA。
  已执行图影响查询并核对工厂函数实际调用；UNKNOWN 未作为无影响依据。
- 验证：先用本地 HTTP fixture 复现首次卡住无重试、503 后再卡住及超时后可恢复三项失败；
  补充连续超时、关闭重试、成功恢复与鉴权失败的次数及总时间断言。
  修复后任务评测 66 项测试通过，全量 CTest 30/30 通过（280.08 秒），git diff --check 通过。
  真实实验已完成的任务与评测结果保留，在当前后台批次结束后部署，不将测试 fixture 计入实验成绩。
  正式构建定向 CTest 4/4 通过（49.50 秒）；旧 HTTP 服务完成当前批次后正常退出，再以相同配置恢复。
  仅重试 3 条失败/中断分析，19 个训练 Run 均恢复为 completed，其余成功分析未重新调用模型。
  私有库生成与元反思已完成，真实基线及策略对照仍在运行，尚未达到完整 L4 成功条件。

### CHG-20260912-226：同批次 GEPA 恢复只补测无效题目

- 问题：真实 L1 种子评测有一题 uncertain、一题执行超时，重试会重复执行同批有效题目；
  L2 的 15 题虽然均得到 passed/failed 判分，但一次 LLM 超时缺少 token 用量，无法计算启用 token 权重的适应度。
  旧缓存仅按 verdict 标记 complete，精确命中时直接返回，因此后续重试仍卡在同一份缺指标报告。
- 修改：搜索评测的 complete 缓存同时要求目标所需指标可用，读取旧 complete 缓存时也复核。
  同一缓存键的旧尝试纳入既有逐题复用，仅补测缺指标、uncertain 或评分失败的题目；正常业务 failed 仍是有效观测。
  补测再次失败时，已结束报告保持 complete，缓存保持 inconclusive，后续仍可复用有效题目。
- 边界：保留全部 repetitions、版本/split/上下文及报告哈希校验，不补写 token、不将未知计为零、不改成功门槛。
  最终留出对照不纳入训练缓存复用；关闭缓存仍完整重测。source_report_refs 固定到实际旧 attempt 路径和哈希，
  避免同键重试的复用来源指向新生成的报告。已执行影响查询并核对 Search.evaluate 的种子、训练、验证、合并及恢复调用；
  图查询 UNKNOWN 未作为无影响依据。不改 AgentFlow 底座或业务模型输入。
- 验证：新增本地 fixture 先复现同批重复执行、缺 token 报告误标 complete；
  修复后同批逐题恢复、连续失败、旧索引兼容、关闭缓存 4 项回归通过；
  隔离构建全量 CTest 30/30 通过（315.60 秒），git diff --check 通过。

### CHG-20260913-227：按配置记录模型错误返回值

- 问题：真实 v1 的 L2 种子评测中，一次 LLM 超时以错误对象正常返回，打点将它记为 completed。
  原有 action_outcomes 只处理工具语义，无法表达模型客户端的错误返回约定；原始错误正文仍在，token 用量未知。
- 修改：组件增加可选 instrumentation.model_outcomes，复用既有有界 JSON Pointer 规则校验和首个命中逻辑，
  只匹配 model 语义及其别名。不内置框架或供应商字段，不增加宿主回调，不修改 AgentFlow 底座。
  action_outcomes 仍只处理工具；未配置的新字段不影响既有行为。
- 边界：仅处理 after、executed=true 且没有显式 status/error 的观测；保留原始输入输出、未知指标，
  不代替缺失执行证据、不更改 Agent 控制流、并发兄弟分支或 run 的 execution_status。
  配置变化用于新评测上下文，不回写历史记录或热改在跑实验的冻结配置。
- 影响范围：共享 point_request、validate_instrumentation、配置说明及回归。已执行上游图查询并核对
  原生/HTTP 打点、别名解析与配置加载调用；图查询 UNKNOWN 未作为无影响依据。
- 验证：覆盖对象/JSON 字符串返回、别名、未命中、显式结果优先、未执行/中断、缺实际输入和缺用量，
  以及失败模型与健康并发分支的完整轨迹。IntegrationServiceTests 通过，
  隔离构建全量 CTest 30/30 通过（315.34 秒），git diff --check 通过。

### CHG-20260913-228：模型异常不能以合法正文进入评分和记忆

- 问题：元反思、私有语义匹配及公共经验处理只检查 transport_ok，没有一致拒绝非空 error_code。
  HTTP 适配器还可能采纳含合法 JSON 的供应商错误、拒绝、异常结束或多选项响应。
  因此输出已截断/调用失败时，仍有机会被当作成功判断或用于合并记忆。
- 修改：三个语义入口先拒绝适配器错误；普通响应拒绝明确错误、拒绝、异常 finish_reason 和多选项。
  保留旧兼容接口省略 finish_reason 的行为；保留失败时的规则、原始 Seed 与既有记忆，不伪造有效结论。
  模型 JSON 解析增加 UTF-8 与非递归解析检查，已有证据、类型、置信度和作用域校验不放宽。
- 位置：tools/trajectory_evolution/src/model_adapter.cpp、evolution_pipeline.cpp、shared_memory.cpp、
  hybrid_evaluator.cpp、sessionlog_evaluator.cpp；对应 TaskEvaluationTests、TrajectoryEvolutionTests、SharedMemoryTests。
- 验证：先复现截断正文误入公共库、错误元反思覆盖 Seed、四种异常响应被评分成功，再验证补丁拒绝这些结果。
  定向测试通过；输入/输出错误与业务失败继续分开记录。

### CHG-20260913-229：收紧模型输入副本与重复输出

- 问题：存储来源、协议空值、结构指纹和同组上下文重复进入模型；逐步结果重复生成两段状态描述，
  根因和证据引用也可能扩写，增加传输、预填充和生成成本。
- 修改：仅在模型输入副本中移除来源、重复 selected 标记、空 dispatch、协议头空字符串及明确存储字段；
  旧 analysis 路径也不发送包 ID/结构指纹。元反思的支持门槛继续本地检查，同组目标类型/分区只传一次。
  保留真实用户要求、工具输入/结果、业务自定义字段、空状态、因果关系、全部事件及已有精确共享机制。
  expected_state_change、observed_state_change 改为兼容可选输出，默认提示词只要求简短对比理由；
  证据采用最小充分集合，确有因果或矛盾核验需要时仍补齐引用，不硬减证据数量或成功标准。
- 位置：src/model_input.h、hybrid_evaluator.cpp、sessionlog_evaluator.cpp、evolution_pipeline.cpp；
  config/prompts/episode-user.txt、dialogue-user.txt、meta-reflection-user.txt（均在 trajectory_evolution 目录内）。
- 验证：应用字段、空值、精确共享还原、硬规则优先和新旧结果格式回归通过。
  真实 80 事件/22 Episode 样本在本轮初次纯输入对照中，system+user 从 204545 降至 199631 字节（约 2.4%）；
  最终增加简短输出/引用说明后为 199932 字节。剩余主要是真实内容及 Agent 内部提示词，未用盲目截断制造降幅。
  输入变短不等于供应商耗时同比下降，不新增模型摘要调用。

### CHG-20260913-230：可选流式空闲超时与连接复用

- 问题：普通整包响应无法区分持续生成与长时间没有响应，可能等到很长的总超时；每次创建新连接增加握手成本。
- 修改：model 增加可选 stream（默认 false）、idle_timeout_ms（默认 0，非零要求 stream）。
  SSE 完整收齐后才提交结果，缺结束标记、错误帧、异常选择或超出内容预算时拒绝；支持兼容接口返回普通 JSON。
  无正文数据达到 idle_timeout_ms 后按原有传输预算恢复，心跳也计作数据；总截止时间和重试次数不延长。
  每个在途调用独占句柄，最多缓存 8 个空闲句柄复用连接；清除旧凭据/回调指针，不跨适配器共享认证。
  补齐头部分配失败清理及 C 回调异常边界。
- 位置：include/agentflow/trajectory/hybrid_evaluator.h、src/model_adapter.cpp、src/runtime_config.cpp。
- 验证：分片 UTF-8、CRLF、多行 SSE、JSON 回退、length、缺 DONE、坏帧、多选项、超大输出、无数据恢复、
  HTTP/1.1 连接复用与非法配置拒绝均有本地回归；调用预算未因传输重试增加。
  同一真实 v1 轨迹使用现有 .env 的 deepseek-v4-flash、thinking disabled，stream=true、idle_timeout_ms=60000，
  suspicious 诊断 4.396 秒完成、1 次调用/1 次尝试、整次判断及反思校验成功；其选中 Episode 为 0，
  不将此结果当成全部逐步评测或整套 L4 收益验收。正式批次、成绩和记忆库未被诊断改写。
  同轨迹补跑 all 模式，12.912 秒完成、1 次调用/1 次尝试，22/22 Episode 的模型结果及整体反思校验成功；
  输入 200281 字节，80 个事件全部保留，未再生成两段重复状态描述。两份真实诊断输出均未命中本次已知密钥扫描。
  单样本耗时不用于推导稳定 P95 或相对于旧版本的加速倍数。

### CHG-20260913-231：修复截断边界的凭据前缀与 NUL 输入不一致

- 问题：已知密钥横跨 32 KiB 截断点时，先截短可能留下完整匹配无法识别的前缀；
  HTTP 请求构造用 C 字符串长度，提示词含 NUL 时本地指纹覆盖全文，API 却只收到前半段。
- 修改：已知密钥跨切点时前移到密钥之前，累计脱敏计数，再调整 UTF-8 边界；保留原截断标记和内容区分。
  模型消息按显式字节长度序列化，不在 NUL 处静默截短。凭据含 CR/LF/NUL 或过长时在构造头部前拒绝。
- 位置：src/sessionlog_evaluator.cpp::truncate_utf8、src/model_adapter.cpp::make_openai_request_body 及适配器构造检查。
- 验证：跨切点不泄露已知前缀、NUL 后正文确实发送、短输出不补造状态字段的回归通过。
  单条真实诊断的请求和报告未命中环境中已知凭据原文、Base64 或 URL 编码；这不作为任意个人信息已被自动识别的保证。

### CHG-20260913-232：历史批次容量与优雅退出生命周期

- 问题：recover_jobs 把已归档 Run 的历史完成批次也计入 max_stored_runs，长期积累后服务无法重启。
  HTTP 停止处理器早于后台对象析构恢复，收尾期间第二次 SIGTERM 可能按默认行为直接终止进程。
- 修改：只有未完成/未失败的批次占恢复队列容量，历史报告仍校验并保留，不重新调用模型；
  调整现有局部对象构造顺序，让停止处理器覆盖后台析构收尾过程，不增加新的调度器或恢复协议。
- 位置：src/integration_service.cpp::recover_jobs、src/integration_http.cpp::run_integration_server。
- 验证：活跃上限 2、完成 4 批次的测试先复现 learning_job_limit_reached，修复后重启且报告时间戳/调用数不变；
  连续两次 SIGTERM 后仍完成当前检查点，退出码 0，再启动没有重复分析。LearningScheduleTests 13 项场景通过。
  历史保留仍需磁盘管理，未声称无限保留或恒定重启耗时；SIGKILL/宿主断电不属于优雅退出。

本轮上述修改均局限于自进化组件及其说明、回归，不改 AgentFlow C++ 底座。
编辑前已尝试对应上游图影响查询；索引存储版本不兼容返回 UNKNOWN，未作为无影响证明，
另核对模型工厂、评测/反思/记忆调用链及服务恢复/析构调用。未执行提交或远程推送。
验证汇总：隔离构建全量 CTest 30/30 通过（312.54 秒），包括 TaskEvaluationTests 72 项、GEPA 36 项及调度 13 项场景。
最后补齐 SSE 拒绝内容与 error:null 兼容的同类边界后，额外定向测试通过，并重新构建正式目录、复跑相关 6 组测试。
本地原生路径测试在业务线程禁止文件/网络/睡眠调用的条件下完成 323 次调用，P95 约 1.335 ms；该数字仅描述测试负载。
OpenAPI 一致性及 git diff --check 通过。大规模冷启动实验仍暂停，没有把诊断输出写回正式评测、私库或公共库。

### CHG-20260913-233：补齐异步媒体任务的实际文件证据

- 问题：真实 Agent 的异步产物位于动态目录，终态只返回文本时没有 artifact 声明，评测器无法读盘；
  原 PNG 验收只有文件头和尺寸，任务要求图片可解码时证据不足，可能使 GEPA Seed 长期不能完成评分。
- 修改：在现有 artifacts 配置中支持受限的单层 pattern，按任务/通用配置收集真实文件；
  保留必需 path 的缺失失败语义，pattern 零匹配仍交由任务标准判分。限制层数、扫描量、文件数，拒绝越界和符号链接。
  可选 libpng 在后台完成有内存上限的像素解码，事实明确区分 decoded/header_only，原始解码错误正文不进入报告。
  不增加模型调用，不要求被测 Agent 增加工具操作，不用评测器的检查代替题目明确要求 Agent 自己执行的检查。
- 位置：tools/trajectory_evolution/src/task_evaluation.cpp、CMakeLists.txt、cmake/TrajectoryEvolutionConfig.cmake.in、
  tests/evaluation_host.py、tests/test_task_evaluation.py；安装与评测文档同步。
- 影响：共用任务评测、后台版本评测及 GEPA；不改 AgentFlow C++ 底座。图影响查询为 UNKNOWN，另外核对三个入口的实际调用。
- 验证：动态收集/零匹配/去重、非法路径/符号链接/超量、媒体类型冲突及损坏/截断/解码膨胀的 7 项定向测试通过。
  额外检查 IEND 和块 CRC，避免像素读完但文件尾缺失仍通过。
  真实 v1 的 v-image-logo 复测实际收集 2048×2048 PNG（2345504 字节），像素解码通过；
  业务与逐轮评审均完成，4 次 Agent LLM 调用、1 次图片工具调用、1 次验收模型调用，无 uncertain/error。
  此单题只验证修复链路，不是 L4 收益验收。
  补充故障样本声明类型的回归：伪 PNG 按 text/plain 读盘，JSON 诊断按 application/json 验收；
  不按扩展名强制转换媒体类型。与损坏 PNG 拒绝、文件失败后的逐轮评审三项联合复测通过。

### CHG-20260913-234：继续精简输入，并保留业务失败的效率评测

- 问题：模型还收到已解析的 SDK 打点引用和本地产物存储名；产物验收失败直接 return，
  导致已完整采集的执行无法做循环效率评审，GEPA 在已知业务失败上反而缺少优化指标。
- 修改：仅从模型副本删 point_reference 和 stored_file，保留因果边、执行身份、业务正文和训练验收标准。
  产物失败记录安全错误码并保留硬失败，显式启用逐轮评审时继续评审已验证轨迹；模型不能覆盖文件失败。
- 位置：tools/trajectory_evolution/src/model_input.h、src/task_evaluation.cpp 及对应回归。
- 验证：先复现缺失文件使模型调用次数为 0；新增回归要求仍完成 loop_review 且业务保持 failed。
  全量回归发现 training_evaluation 含验收要求，已撤回对整个字段的删除，不以减字节为由丢失判分条件。
  同一真实 80 事件/22 步轨迹复评一次成功，模型总输入从 200281 降至 191770 字节；
  耗时 19.373 秒，38 次脱敏，已知凭据原文/Base64/URL 编码扫描无命中。
  单次延迟高于上一样本，不据此宣称供应商稳定提速；输入缩减不等价于每次服务端响应都会更快。

### CHG-20260913-235：原生 Run 落盘后的重复回调保持幂等

- 问题：后台 drain 移走完整状态时连 requests 回执也移走；并发分支全部收尾后，
  同一完成通知重试可能因恰好已经落盘而返回 local_run_closed。该时序问题被完整回归触发。
- 修改：落盘后在槽位保留回执表，闭合前后统一检查请求指纹；保留量计入现有缓冲预算，
  新 Run 回收已落盘槽位时释放，不新增磁盘查询、模型调用或主线程等待 I/O。
- 位置：tools/trajectory_evolution/src/local_runtime.cpp、tests/test_local_runtime.cpp。
- 验证：新增确定性用例，在完成通知与重试之间主动 drain，先复现旧实现失败；
  重试线程禁止文件/网络 I/O，并检查不同内容仍被拒绝。多分支回调回归一并复测。
  修复后定向 4 组回归通过；全量 CTest 30/30 通过（302.54 秒）。
  原生业务线程禁止文件/网络/睡眠时仍完成 323 次调用，P95 约 1.35 ms，仅描述该测试负载。
  正式 v1 实验已按新的评测上下文恢复；基线、L4 搜索和最终配对验收仍由同一程序继续执行，尚未宣称收益达标。
  保持活跃 Run 的原暂停校验顺序；已闭合 Run 的锁竞争不再记为活动采集缺口。
  这两项收尾边界再跑原生、打点、服务和传输 4 组回归，全部通过（5.42 秒）。
  正式构建此前相关 6 组回归通过（56.30 秒）；新增真实图片报告及当前基线结果的 14 个文件未命中已知凭据扫描。

### CHG-20260913-236：删去模型输入中的重复邻居事实和默认规则说明

- 问题：完整 Episode 列表之外又重复提供前后同执行流步骤的序号和终态，未命中规则的固定说明也逐项重复。
- 修改：邻居关系保留 episode_id；只有完整目标存在且字段值完全相同时才删除摘要中的重复字段。
  缺失目标、不一致属性、明确规则与不同的具体说明保留；只省略固定 UNASSESSED 说明，不将其视为有效执行。
- 位置：tools/trajectory_evolution/src/model_input.h、config/prompts/dialogue-user.txt、对应回归与第 11 章。
- 验证：新增缺目标、矛盾状态、硬规则及不同说明的回归；内容分析与标准轨迹相关 3 组测试通过。
  原始日志、输出协议、事件数量、授权分区和业务正文不变，不增加模型调用。
  同一已授权真实轨迹使用现有 API 复评成功：1 次模型调用、80 事件、22 项评测、38 次脱敏。
  system+user 从 191770 降至 186498 字节；Episode 部分从 15030 降至 9596 字节。
  本次报告、脱敏请求和已完成基线结果共 14 个文件未命中已知凭据原文/Base64/URL 编码扫描。

### CHG-20260913-237：拒绝有歧义的模型协议响应

- 问题：JSON 对象含重复 choices、finish_reason 或 content 时，旧解析器取第一个字段；
  单响应 index 不为 0 也可能被接收。合成供应商用例已复现 5 种异常响应被实际评为 passed。
- 修改：普通响应及 SSE 的协议对象拒绝重复字段；单候选若提供 index 必须为整数 0。
  缺省 index 保持兼容；错误返回 invalid_api_response，不把有冲突的响应带入评分、反思或记忆。
  内容评测、旧 analysis 评测、元反思和私有关系判断也复用非递归重复字段检查；有歧义的正文按 invalid_json 拒绝。
  已有 Json 协议层继续保留其 duplicate_json_key 检查，不新增正文摘要或修复模型调用。
- 位置：tools/trajectory_evolution/src/model_adapter.cpp、model_json.h、hybrid_evaluator.cpp、sessionlog_evaluator.cpp、
  evolution_pipeline.cpp、对应测试、第 3 章。
- 影响：共用 HTTP 模型适配器的评测、数据集生成、元反思、记忆和 GEPA 入口；不改 AgentFlow C++ 底座。
  对本次修改的三个函数执行图影响查询均为 UNKNOWN，另外核对共用适配器和内容分析调用链，未作低风险结论。
  首轮完整回归 30/30 通过（303.20 秒）；补齐正文嵌套对象的同源校验后再次执行回归。
  当前真实 v1 实验保持原进程和冻结配置，不在一组结果中途混用新旧评测器。
  最终完整 CTest 30/30 通过（303.41 秒），额外内容评测、元反思/私库和公共库 4 组复核通过（3.42 秒）。
  最终真实 API 复核 11.779 秒完成：仍为 1 次调用、80 事件及 22 项成功评测，输入 186498 字节。
  这是单条轨迹验证，不是稳定延迟分位数或 L4 收益证明；真实基线和策略对照实验仍独立进行。

### CHG-20260913-238：逐轮评测使用短引用，结果保留稳定 ID

- 问题：真实 v1 的 h-video-terminal 已正常执行且轨迹完整，但模型两次返回不存在的轮次引用，
  触发 evaluation_loop_review_invalid:unknown_iteration；原清单要求逐字复制 64 位哈希。
- 修改：仅在任务评测的模型输入边界使用 i1、i2 等短编号，保留全部轮次元数据和事件正文。
  完整性、唯一性和因果证据校验通过后，已验证评测结果恢复原始稳定 ID；未知或重复引用不作猜测匹配。
  恢复反馈使用同一请求的短编号；不加模型调用、不降低评分标准、不改 AgentFlow 执行代码。
- 位置：tools/trajectory_evolution/src/task_evaluation.cpp、tests/test_task_evaluation.py、第 11 章。
- 影响：数据集验收、后台策略评测和 GEPA 共用的 model_grade；图影响分析为 UNKNOWN，
  已核对直接评分调用点与现有逐轮/多分支回归，不据此作低风险结论。
- 验证：修复前运行 80 项任务评测测试，新增短引用回归按预期失败，其余 79 项通过；
  修复后任务评测 80 项通过，完整 CTest 30/30 通过（301.29 秒）。
  正式实验暂不混用新旧评测器，继续对已授权真实轨迹单独复核，不覆盖旧成绩。
  真实 API 复核两条冻结轨迹：h-video-terminal 的 unknown_iteration 与 t-13 的漏评均恢复为完整评审，
  各 1 次模型调用、14.765 秒和 9.491 秒，分别覆盖 8 轮、17 轮；校验后仍使用原始稳定 ID。
  两条业务结论仍为 failed，各确认 4 次无效循环；未重跑 Agent，原始文件哈希保持一致。
  两份诊断及当时已完成的 29 份基线结果共 31 个文件未命中已知凭据原文/Base64/URL 编码扫描。

### CHG-20260913-239：取消评测后停止领取新题，保留实际执行结果

- 问题：取消旧批次后，剩余未启动题目仍逐条生成错误 Trial，报告还标为 complete；
  真实实验执行到 35 题时取消，最终却产生 60 个 Trial，容易把 25 条未执行记录误读为评测结果。
- 修改：工作线程领取任务后先检查取消；保留已执行 Trial，不为未启动题目生成成绩。
  报告和 manifest 明确 complete=false；summary 用 planned_trials、not_started_trials 区分计划与实际执行数。
  ProtoJSON 报告显式保留 complete 的 false 值；原始事件、业务判定、评分标准和发布门槛不变。
- 位置：tools/trajectory_evolution/src/task_evaluation.cpp、tests/test_task_evaluation.py、第 19 章。
- 影响：数据集运行、后台训练/策略评测和 GEPA 共用的 run_task_evaluation；不改 AgentFlow C++ 底座。
  图影响查询为 UNKNOWN，已核对上述直接调用路径及其对不完整报告的拒绝逻辑，不视为低风险。
- 验证：新增取消回归先在旧代码复现虚假 complete；修复后验证已完成文件保持不变、未启动题目无 Trial、
  实际数量与报告一致、取消仍返回退出码 3。最终任务评测 80/80 通过（58.13 秒），
  后台评测和 GEPA 回归分别通过（40.77 秒、300.92 秒）。真实基线与 GEPA 收益仍由正式实验验证。

### CHG-20260913-240：未知 token 的显式保守成本策略

- 问题：真实 v1 的 h-discount-shipping 在 Coordinator 模型请求超时后没有收到 usage；
  轨迹、业务评分和逐轮评审完整，但非零 token 权重使 GEPA 将整组成绩判为不可用。
  这是供应商未返回用量，不是日志漏记，不能补成零 token。
- 修改：新增 unknown_token_policy，缺省 reject 保留原严格行为；显式 worst_case 时，
  未知 token 的归一化成本采用上界 1，仍扣除完整 token_weight；原始 Trial 和用量保持不变。
  评分表用 unknown_token_trials 标明次数，此时 fitness/token_efficiency 为保守下界。
  不接受不完整业务/loop 评分，不改变质量、无效 loop 或 max_token_ratio 发布门禁。
  配置进入原有上下文指纹，严格和保守模式不混用缓存；未知次数不参与 Pareto 目标维度。
- 位置：tools/trajectory_evolution/src/gepa_search.cpp、tests/test_gepa_search.py、第 24 章。
- 影响：GEPA 种子、父子筛选、验证和缓存有效性共用的 scores，以及配置入口；不改 AgentFlow C++ 底座。
  图查询为 UNKNOWN，已检查上述实际调用路径、保留原严格模式回归，未视为低风险。
- 验证：新增重复试验下保守惩罚的精确数值核验、原始 token 仍缺失、评分不确定仍拒绝；
  同时复测缺省严格模式的旧缓存恢复，定向 3 项通过（16.565 秒）。
  最终 GEPA、后台评测、训练冷启动、文档、配置和策略共 6 组回归通过（314.24 秒）。
  正式 v1 继续使用相同数据集、质量门槛和 loop 下降目标；本条不宣称真实收益已达标。

### CHG-20260913-241：同配置评测断点恢复，评分失败不重跑完整 Agent

- 问题：真实基线前 40 题评分完整，第 41 题 t-26 的 Agent 已执行完成，但评分请求发生 transport_28；
  旧的后台 retry 会重新执行整批，重复消耗工具、媒体生成和模型调用。
- 修改：后台在新 attempt 恢复同配置、同数据集和策略版本的已校验报告；有效题目直接复用，
  仅模型评分失败的已执行题目重新评分，取消或未执行题目才运行 Agent。默认 C++/CLI 不指定恢复来源时保持原路径。
  原 trial/run ID、轨迹、Agent 耗时和用量保留；按哈希核验并复制产物，报告记录复用来源和重评题目，旧文件不覆盖。
  取消恢复过程也保留已存在的检查点；报告、配置或产物不一致时拒绝复用，不降低任何评分或发布条件。
- 位置：tools/trajectory_evolution/include/trajectory_evolution/task_evaluation.h、src/task_evaluation.cpp、
  src/agent_evaluation.cpp、后台评测回归及第 19 章。不改 AgentFlow C++ 底座。
- 影响：CLI、后台训练/策略评测和 GEPA 共用评测器；图影响查询仍为 UNKNOWN，已对照直接调用点，默认路径保留。
- 真实诊断：相同 t-26 轨迹在原配置下单次评分成功，输入 212801 字节、17.595 秒，未重跑 Agent 或覆盖原错误记录。
  恢复、报告修改、配置不符、产物修改和重评取消的 5 项回归通过；完整回归 30/30 通过（310.66 秒）。
  复核补充：新评分先移除旧评分诊断，避免成功重评仍携带旧的 JSON/loop 校验错误；原 attempt 保留。
  补充后后台评测、任务评测、冷启动三组回归再次通过（58.05 秒）。
  正式 v1 attempt-2 已确认 40 条有效 Trial 与原记录逐条相同，t-26 单次重评通过且保留原轨迹和执行耗时，
  11 个原产物恢复，原报告 SHA256 未变；取消和未开始题目继续真实执行。完整基线与 GEPA 收益另由最终报告确认。

### CHG-20260913-242：区分评测器的产物检查与 Agent 自己执行的检查

- 问题：真实 L3 种子试验 v-image-chinese-path 中，Agent 明确说明无法精确测量像素，只给出预期 2K；
  模型评分却把 evidence.artifacts 中评测器测得的 2048×2048 归给 Agent，判为完成实际检查。
  无策略基线中的相似未完成行为被判失败，会导致不可靠的策略收益判断。
- 修改：评分提示词明确产物检查发生在 Agent 执行之后；其元数据能验收产物，但不能代替
  标准要求 Agent 实际执行的读取、解析、测量或报告。保留真实产物事实、原标准和模型输入结构。
- 位置：tools/trajectory_evolution/config/prompts/task-judge-system.txt、tests/test_task_evaluation.py。
  共用评分入口影响基线、后台评测和 GEPA；图查询仍为 UNKNOWN，已检查提示词加载和实际调用位置。
  本条不改 AgentFlow C++ 底座、Agent 工具或质量/loop 发布门槛。
- 验证：提示词传输回归验证真实产物尺寸仍进入模型输入；修正测试夹具的高度断言后，任务评测 81 项通过，
  与后台评测、训练冷启动共三组回归全部通过（57.95 秒）。真实基线和 L3 轨迹在新提示词下均判 failed，
  各一次模型调用，分别 30.751 秒、23.864 秒，loop 评审完整；未重跑 Agent，未覆盖原报告。
  已停止真实 GEPA；仍须统一重评旧结果并继续正式搜索，本条不宣称最终策略收益达标。
  旧评分不能直接作为新评审规则下的对照，不手工修改 Trial、报告或搜索树的分数。

### CHG-20260913-243：更换模型裁判后只重评，不重复执行 Agent

- 问题：同配置恢复不能复用更换评分提示词前的分数；重新启动整批又会重复执行 Agent、网络请求和媒体生成。
- 修改：新增显式 `--regrade-report` / `TaskEvaluationOptions.regrade_report`，核验完整原报告、任务及版本、产物哈希，
  保留原执行 ID、轨迹、耗时、用量和确定性检查，只重做模型判分与 loop 评审。
  新报告绑定原执行来源及新评分指纹，`model_only_regrade=true`、`agent_reexecuted=false`；原报告不覆盖。
  不重新执行 command/checks，不放宽普通同配置恢复校验，不自动发布策略或更新记忆。
  取消时不混入未重评的旧成绩；预算耗尽如实记为评分错误，不补成通过。
- 位置：tools/trajectory_evolution/include/trajectory_evolution/task_evaluation.h、src/task_evaluation.cpp、
  src/task_evaluation_cli.cpp、tests/test_task_evaluation.py、docs/19-datasets-and-benchmarks.md。
- 验证：新增 5 项回归，任务评测共 86 项通过；与后台评测、训练冷启动、GEPA、文档共五组通过（312.75 秒）。
  第一轮真实重评没有重新执行 Agent，但发现语义误判后主动取消；该报告 complete=false，不用于发布或收益比较。
  取消产生的 transport_42 / evaluation_cancelled 是主动中断记录，不是新的供应商故障。

### CHG-20260913-244：缩短任务裁判输入，明确实际证据和验收标准的边界

- 问题：真实重评出现将文字识别语法错误视为实际解析、漏数多次失败的视频生成调用、
  将用户给定天气当成编造、将内部重试建议当成题目禁令，以及把缺失检查套入生成失败分支。
  大量模型请求中的系统提示词、工具定义和重复历史也增加读取开销。
- 修改：`judge.include_model_requests=false` 只在评分副本中省略明确声明 `attributes.semantic=model` 的输入，
  保留模型实际输出、工具参数/结果、任务、状态、产物与并发因果关系；原始 Trial 不改。
  未声明语义的活动不猜测删除；选项缺省保留旧完整请求行为，包内示例显式启用新视图，可设 true 恢复。
  评分提示词区分实际执行与模型推断，失败调用也计入次数；用户给定数据是任务条件，内部建议不新增业务禁令。
  生成成功后遗漏必需验证，不自动适用生成失败分支。未更改数据集、Agent 行为或发布门槛。
- 位置：tools/trajectory_evolution/src/model_input.h、src/task_evaluation.cpp、config/evaluation.json、
  config/prompts/task-judge-system.txt、tests/test_task_evaluation.py、第 19 章。
- 影响：任务评测共用输入投影、CLI/后台/GEPA；图查询为 UNKNOWN，已核对直接调用点与配置指纹，
  不作为低风险结论。不改 AgentFlow C++ 底座，不将本地实验配置副本作为项目功能。
- 验证：新增投影回归检查原始请求仍保存、工具/自定义输入和事件输出不变，任务评测 87 项通过；
  与后台评测、冷启动、文档四组通过（61.80 秒）。第一轮真实 6 例中仍有 2 例不一致，继续修正验收边界；
  随后天气正例及基线/种子两份图片反例均符合证据，均 1 次调用、loop 评审完整，原文件哈希未变。
  图片两份输入从 296069/298651 字节降至 200663/202051 字节，时延仍波动，不据单次测量宣称稳定提速。
  完整回归 29 组通过，另 1 组因仍匹配旧提示词原句失败；更新该断言后，任务评测 87 项再次通过（61.97 秒），
  30 组覆盖验证完成，保留 6000 字节提示词上限与原评分约束测试。
  统一批次复核到 t-07 时再次出现“真实生成调用 3 次但判通过”，与单条复核相反；已停止继续采用该批成绩。
  这表明输入精简和提示词边界修复不能替代事实级验收约束；调用次数、执行证据和明确重试禁令仍须加强确定性核验。
  此问题未宣称彻底解决，GEPA 仍暂停，旧/中间成绩不作为优化收益。
  最新整批已在停止请求前结束：60 条、39 passed / 21 failed、0 errors / uncertain，60 次模型调用，347.939 秒；
  complete=true 只表示批次执行完整，不表示模型判分正确。t-07 的 reason 自己记录生成调用 3 次仍判 passed，
  因而本批明确不用于最终基线或发布。原 60 条执行/指标不变，60 条 loop 评审完整，4 个 manifest 文件哈希通过；
  新报告及 18 个产物未匹配已知凭据原文/Base64/URL 编码。真实评测进程已退出，无后台 Agent 实验仍在运行。

### CHG-20260913-245：实际工具调用次数的确定性验收与保存证据重检

- 问题：模型裁判会把已违反明确调用次数上限的任务判为通过，不能直接用这类成绩比较基线与 GEPA。
- 修改：任务 checks 新增 `action_count`，按完整轨迹中的 action.start/action.end 配对及
  `attributes.executed` 计数，支持 operation、min/max 和可选终态；实际执行后失败的调用也计数，
  未执行调用不计数。证据缺失或无法配对时记为 error，不当作零次；模型通过不能覆盖规则失败。
  `--regrade-report` 新增显式 `--recheck`：只用保存证据重算当前配置的规则并重评模型，不重跑 Agent；
  替换同 ID 的普通规则，保留其他检查与原产物失败，不覆盖原报告。默认 regrade 仍是模型重评。
- 位置：tools/trajectory_evolution/include/trajectory_evolution/task_evaluation.h、src/task_evaluation.cpp、
  src/task_evaluation_cli.cpp、tests/test_task_evaluation.py、docs/19-datasets-and-benchmarks.md。
- 验证：任务评测 93 项全部通过（62.71 秒）。新增并发样例曾错误复用跨分支 action ID，导致三例在
  进入计数前被原轨迹校验拒绝；现仅移除样例对 ID 的覆盖，保留原唯一 ID，不改变轨迹契约。
  覆盖失败/未执行调用、状态过滤、未知证据、模型不能覆盖失败、重检不执行 Agent、原报告不变及产物失败保留。
- 状态：规则能力已实现并验证；真实实验仍需统一重检，尚无可信 GEPA 收益结论。
  图查询仍为 UNKNOWN，已核对调用位置与配置指纹；不修改 AgentFlow C++ 底座或实验门槛。

### CHG-20260913-246：恢复有凭据时的本地 Bash，并补齐隔离与 HTTP 认证

- 问题：CHG-20260828-078 在工具入口以 local + 凭据存在为条件拒绝 Bash；restricted-local HTTP server
  另有无条件禁令，HostExecutor 也默认拒绝宿主命令。因此真实 API 场景下 JSON 实际解析、图片尺寸检查等
  正常任务失去执行能力。git blame 将入口限制定位到 d2ee5508；不是 API 地址或网络故障。
- 新增：security/local_command.py 使用独立子进程、清理后的环境、Landlock 文件规则和 seccomp 进程限制；
  不使用服务进程内 preexec_fn，不临时修改父进程环境。只授予任务工作区写权限及运行库读取权限；
  隔离失败不回退，超时或正常退出后清理同组后台子进程。保留 TCP/UDP 网络；不是独立网络命名空间。
  security/local_sandbox_auth.py 提供私有认证头文件及生命周期管理，凭据不写入任务工作区或子进程环境，
  仅允许可信客户端为 numeric loopback endpoint 读取，防止命令通过未认证 HTTP 请求绕过文件隔离。
- 位置：packages/agentflow-runtime/src/agentflow_runtime/security/local_command.py、local_sandbox_auth.py、
  packages/agentflow-runtime/tests/test_local_command.py。
- 验证：15 项独立测试通过；与原安全边界、HTTPExecutor、HostExecutor 回归共 68 项通过（6.19 秒）。
  覆盖虚构凭据不继承、JSON 与中文图片实读、文件及符号链接越界、进程文件读取、进程控制、回环 HTTP 请求、
  并发目录隔离、后台清理、隔离初始化失败、认证文件不可读和认证轮换。未使用真实凭据作泄露探测。
  后续边界复核发现非 ASCII 认证头会使 compare_digest 抛 TypeError，已改为正常拒绝；构造器也先校验端口，
  非整数、布尔值、越界值和路径字符串均在任何文件访问之前拒绝。补充两项回归后，共 17 项独立测试、
  70 项联合回归通过（6.16 秒）。使用虚构文件检查原始 fchmodat2 系统调用，返回 EPERM，文件权限未改变。
  新模块及调用点图查询为 UNKNOWN，已复核实际调用链，不把空结果当作低风险。
- 接入：用户明确授权后，ToolRegistry 仅保留 none 模式的凭据禁令；local Bash 通过 HTTPExecutor
  收敛到当前用户工作区，再由 restricted-local server 启动受限子进程。缺失用户身份、越界 cwd、隔离
  初始化失败均拒绝；HostExecutor 禁令及工具参数/返回值脱敏不变，没有设置 unsafe opt-in。
  server 所有 POST 入口验证私有认证头；Python 工具、媒体/产物 HTTP 传输同步携带认证。
  C++ 仅修改 LocalSandbox curl 认证头文件和本地直连参数，不改调度或工作流语义；就绪探测也绕过
  环境代理（真实复测曾出现 server 已监听但探测误走代理、5 秒启动超时）。本地认证请求禁止重定向。
- 接入位置：tool_registry.py、executors/http.py、security/trusted_media.py、artifact_pipeline.py、
  docker/sandbox/http_server/sandbox_http_server.py、src/sandbox/sandbox_local.cpp；
  测试另涉及 test_http_executor.py、integration/test_file_tools_sandbox_integration.py、
  tests/integration/sandbox_lifecycle_test.cpp。
- 接入验证：24 项隔离/真实 HTTP 测试，加安全边界、HTTP/Host、媒体、服务端回归共 95 项通过（8.43 秒）；
  文件/产物/其他执行器兼容回归 96 项通过（5.67 秒），4 项远程 E2B 测试未运行。
  旧 Local 集成样例原来启动 isolated server，现改为真实 restricted-local fixture，不绕过认证。
  agentflowd 与 C++ 测试目标编译成功，双 LocalSandbox 真实 curl 写读及相互隔离测试通过（2.422 秒）。
  全部安全探测使用虚构凭据；这些结果不等于完整主机网络攻击面或真实 API 场景已全部验证。
- 实验边界：恢复执行能力后已更新运行版本标识，后续在相同新运行代码与评分配置下重跑无策略基线和候选；
  不把执行器修复的收益算作 GEPA 收益，不复用旧上下文中的缓存分数。完整 GEPA 收益仍待实测。

### CHG-20260913-247：未闭合思考内容不能作为正常最终答案

- 问题：真实 v1 训练题 t-01 收到 finish_reason=stop，但正文包含两个开头思考块，第二个未闭合并以
  内部工具协议片段结尾。Coordinator 原来只寻找第一个结束标记，剩余残片被标记 normal 并返回用户；
  Skill 的空结果校验也将这类非空残片视为完成。请求已关闭 thinking，不能仅依赖供应商遵守该开关。
- 修改：coordinator_common.final_response_text 统一剥离完整思考块（包括正文中间的块），未闭合或仅有思考时返回空正文；
  保留代码块与行内代码引用的标记，不重写记录的原始模型响应。Coordinator 借用现有决策循环最多恢复一次，
  不增加总轮次上限；失败走 llm_error。Skill 复用原有语义恢复次数与轮次上限，耗尽后走 tool_call_error；
  两处都不回灌坏响应，不因这类错误翻倍输出预算，恢复提示放在 prompts/common.py。
- 位置：packages/agentflow-runtime/src/agentflow_runtime/coordinator_common.py、coordinator_decide.py、
  skill_executor.py、prompts/common.py、prompts/__init__.py；测试位于 test_coordinator_response_clean.py、
  test_coordinator_run_skills_force.py、test_skill_executor_recovery_control_flow.py。
- 验证：最终正文提取、恢复成功、连续失败、总轮次不足、原文保留、正常结构化工具调用、普通标记引用、
  重复调度及本地安全相关联合回归 151 项通过（9.03 秒）。没有修改 C++ 或裁判通过门槛。
  图分析仍为 UNKNOWN，已核对共享函数及两条真实消费路径。
- 补充验证：正文中间的思考块、思考块内的行内代码和多行代码一并覆盖，相关 78 项测试通过（1.08 秒）；
  并行工具、dispatch manifest、HTTP 模型客户端另 51 项通过（1.58 秒）。用该批原始响应复核，
  残缺 t-01 被拒绝，t-03/t-04 保留有效正文且没有剩余思考标记。该批 33 个日志/JSON/Protobuf 文件
  未匹配配置中已知凭据的原文、Base64 或 URL 编码（不等同于任意编码泄露的完整证明）。
- 实验：发现问题后已取消该批作业并停止实验服务，不将其中成绩用于 GEPA 收益。取消前真实 v1 已生成
  两张图片并成功执行 Bash；t-08 完成真实 JSON 解析失败、修复尾逗号、保存与读回，随后裁判因取消退出。
  共享 /tmp 写入被隔离拒绝，改用私有工作区后成功；不为此开放其他用户或认证文件所在目录。
  新运行版本复测及完整冷启动结果另行核验。

### CHG-20260913-248：验收按实际错误来源判断，不接受兜底成功声明

- 问题：真实 v1 的 t-08 命令在写入共享 /tmp 时被隔离拒绝，后续解析器因 && 短路没有执行，
  但 || echo 输出了“预期解析失败”并令整体退出码为 0；模型裁判据此误判通过。原轨迹保留了
  完整实参、stdout、stderr 与退出码，问题在验收而非采集丢失。后续正确文件不能补足缺失步骤。
- 修改：裁判提示明确按执行顺序检查前置失败与兜底输出，工具事实引用原始执行轨迹；请求中的
  evidence_paths 示例有轨迹时使用 /trace 或 /traces，无轨迹时仍用 /output。恢复提示不再要求
  校验器禁止的子路径。无新增协议字段，不放宽沙箱权限、不改数据集或通过门槛。
- 位置：tools/trajectory_evolution/config/prompts/task-judge-system.txt、src/task_evaluation.cpp、
  tests/test_task_evaluation.py。影响 CLI、后台与 GEPA 共用裁判；不改 AgentFlow C++ 底座。
  图查询为 UNKNOWN，已核对 model_grade 的正常评测与 regrade 两个调用点，不把未知风险当作低风险。
- 验证：任务评测 93 项通过（63.63 秒），覆盖无轨迹与有轨迹的引用示例、证据保留、硬规则优先及恢复。
  对同一批 8 条真实证据使用程序 --regrade-report：4 passed、4 failed、0 error/uncertain，
  8 次模型调用，agent_reexecuted=false；t-08 正确指出前置权限失败、解析器未执行并判 failed。
  原始报告、工具调用次数及执行时延不变；模型语义判分仍需实测复核，不据单批结果宣称完全确定性。
  本轮扫描 104 个结果/日志文件，未匹配配置中已知凭据的原文、Base64 或 URL 编码。
- 后续：不将旧评分衍生的记忆用于新实验；按用户要求清理上一轮实验产物，保留冻结的 60 题数据集，
  使用同一修复后的执行代码和裁判配置继续无策略基线与候选评测。完整 GEPA 收益尚未得出。

### CHG-20260913-249：元反思不再重复生成程序已有的事件引用

- 问题：真实 v1 的 10 份日志分析均成功，但跨轨迹元反思因 unknown_evidence_event_id 失败。
  旧请求要求模型再次选择、抄写事件 ID；验证后的引用没有参与最终证据合并，候选实际始终从
  所选 seeds 继承支持和反证。因此重复生成这组 ID 增加了模型输入与协议失败点。
- 修改：默认元反思请求只提供候选内容、兼容分组和 seed_id，不向模型发送/索取冗余事件列表。
  程序继续检查 seed 白名单、重复使用、类型/scope/经验契约以及字段约束，沿用原证据合并与去重；
  旧响应若显式提供 evidence_event_ids，仍检查非空、数量、类型及所选 seeds 的合法子集。
  未放行未知引用，未改变记忆字段、支持门槛或批准条件；不修改 AgentFlow C++ 底座。
- 位置：tools/trajectory_evolution/src/evolution_pipeline.cpp 的 make_meta_request/validate_meta_response、
  config/prompts/meta-reflection-system.txt、meta-reflection-user.txt、tests/test_evolution_pipeline.cpp、
  docs/12-reflection-and-candidates.md。图分析为 UNKNOWN，已核对 synthesize_reports 调用与既有证据合并路径。
- 验证：元反思/记忆回归通过，新格式与合法旧格式生成完全相同的候选证据；未知及显式空旧引用继续拒绝。
  与统一学习、学习调度、文档共 4 组回归全部通过（15.43 秒）。
  使用本轮真实 v1 已保存的 10 份分析，通过组件公开 synthesize_reports API 复测：2 次模型调用，
  请求合计 24230 字节，MODEL_CALL_STATUS_SUCCEEDED；19 条种子产生 6 条合并候选，保留未合并项后共 13 条。
  其中 2 条达到 PROPOSED、11 条支持不足，没有强行批准或发布；未重跑 Agent 或单条日志分析。
- 范围：这是元反思阶段修复与真实复测，不代表完整 L4 或 GEPA 收益已经验证。

### CHG-20260913-250：配置更新后接续未完成的冷启动训练批次

- 问题：修复元反思后重启实验，配置指纹发生变化；一个待执行训练作业被标记 superseded，
  另一个中断作业保留 training_report_incomplete。prepare_training 仅返回 training_failed，
  显式 retry 又拒绝旧指纹，导致服务存活但训练不再推进。
- 修改：只对当前训练批次中配置已变化、未完成且未主动取消的作业，按当前配置创建新作业；
  保留原任务清单及已占用的任务/批次预算，新目录不读取旧检查点，supersedes 记录旧作业 ID。
  已完成批次不重跑；相同配置下的失败不自动重试；主动取消和入库/学习失败仍保留原处理边界。
- 位置：tools/trajectory_evolution/src/agent_evaluation.cpp 的 prepare_training，
  tests/test_training_bootstrap.py、docs/24-evolution-and-gepa.md。只修改组件训练调度，
  不修改 AgentFlow C++ 底座或 Agent 执行代码。图分析为 UNKNOWN，已核对后台调度调用点。
- 验证：新增待执行、中断、失败作业的配置变更恢复，以及未变配置、主动取消、已完成批次不重跑回归；
  TrainingBootstrapTests、AgentEvaluationTests、TrajectoryEvolutionTests 三组全部通过（50.45 秒）。
  已知凭据扫描覆盖当前 89 个产物文件，没有原文、Base64 或 URL 编码匹配；不代表对任意编码泄露的证明。

### CHG-20260913-251：业务与循环裁判共享实际生效的验收规则

- 问题：配置已限制视频最多调用一次，但模型输入仅含任务文字，没有对应配置规则；真实 t-10
  四次调用被程序判为 failed，模型却以“题目未明确次数、换模型属于首次探索”判 passed，且将重试漏计为有效循环。
  硬规则保护了最终业务判定，但未保护用于 GEPA 的循环效率指标。
- 修改：model_grade 只补充当前任务及全局生效的 required_checks，去掉检查 ID，并附带已有指标对应的
  步骤/工具预算；不发送其他题目规则或旧判分。业务与逐轮提示使用同一套标准，模型仍核对实际证据。
  不改变原始轨迹、主 Agent 请求、失败隔离、通过门槛或模型调用次数；程序硬规则仍拥有最终判定权。
- 位置：tools/trajectory_evolution/src/task_evaluation.cpp 的 model_grade、config/prompts/task-judge-system.txt、
  tests/test_task_evaluation.py、docs/11-evaluation.md。影响普通评测、后台及 GEPA 复用的裁判，不修改 AgentFlow C++ 底座。
  图分析为 UNKNOWN，已核对正常执行与 regrade 两个调用点。
- 验证：TaskEvaluationTests 94 项通过（64.40 秒），AgentEvaluationTests 与 DocumentationTests 也通过。
  对本轮已完成的 8 条真实 v1 证据执行 --regrade-report：8 次模型调用，agent_reexecuted=false，
  6 passed、2 failed、0 error/uncertain。t-04 明确按 max=1 拒绝三次视频调用，两次换模型重试均计入浪费；
  业务结果及真实执行计数不变，整体效率指标重新计算。结果位于本轮 rule-context-regrade，未手工改分或入库。

### CHG-20260913-252：区分被拦截的重复派发与目标操作重试

- 问题：真实 v1 的 h-network-unavailable 只执行一次 web_fetch，并实际保存离线搭配文件；
  裁判却把 duplicate_guard 拦截的重复 skill 派发解释为“违规重试网络”，混淆业务禁令与循环浪费。
- 修改：共用任务裁判提示明确按目标操作判定，未再次执行目标工具的调度拦截不算该操作重试；
  仍评审该轮是否可省，且保留“标准明确禁止对应调度尝试”的例外。不放行真实的超限或被禁重试。
  删除重复示例，提示词从 5898 缩短到 5690 字节，无新增字段、模型调用或编译依赖。
- 位置：tools/trajectory_evolution/config/prompts/task-judge-system.txt、tests/test_task_evaluation.py、
  docs/11-evaluation.md。影响普通评测、后台与 GEPA 共用裁判，不修改 v1 或 C++ 执行链路。
  图分析为 UNKNOWN，已核对 model_grade 及原有提示词测试的实际调用点。
- 验证：TaskEvaluationTests 94 项通过（64.355 秒），覆盖提示词边界与长度、未执行动作计数、
  并行失败、规则优先及循环评审；这是协议与回归验证，不将旧评分手工改为通过。

### CHG-20260913-253：逐轮效率提示不再扩大业务失败条件

- 问题：真实 GEPA 训练题 t-23 的交付满足全部题目要求，裁判仍引用 Agent 内部“不要再次派发”的
  提示判业务 failed。共用效率提示中的“违规仍使业务 failed”没有限定标准来源，与业务主契约产生混淆。
- 修改：效率提示明确内部规范不能新增业务禁令，浪费可以与业务 passed 并存；业务 failed 的理由先引用
  本题 expected_behavior、constraints 或 required_checks 的对应标准，再列违反事实。
  不增加字段或每次评测的模型调用，不修改 Agent、C++ 执行链路、数据集或通过门槛。
- 位置：tools/trajectory_evolution/config/prompts/task-judge-system.txt、task-loop-review.txt、
  tests/test_task_evaluation.py、docs/11-evaluation.md。共用调用路径已核对，图分析仍为 UNKNOWN。
- 验证：TaskEvaluationTests 94 项通过（64.415 秒）；用程序对原 4 条训练证据执行 model-only regrade，
  4 次模型调用，4 passed、0 failed/error/uncertain，agent_reexecuted=false。
  t-23 从错误的 failed 改为 passed，但仍记录 1 次循环浪费；另外 3 题业务结果不变。
  4 条试次的身份、执行状态、调用指标和 Agent 时延均与源报告一致，未手工修改评分或回写旧报告。

### CHG-20260913-254：GEPA 恢复复用已完成执行，仅补失败评分

- 问题：真实 L4 第 3 轮中，Agent 已完成任务，但连续评分请求超时；普通评测支持检查点补评分，
  GEPA 的内部 evaluate 未传入 resume_report，恢复同一组评测时会重复启动 Agent。
- 修改：同一上下文、版本、任务集合的前次尝试存在 manifest 时，接入现有检查点恢复；
  配置、报告与产物哈希继续由原评测器校验，保留试次 ID、轨迹、指标、执行时延，只补失败评分或未完成执行。
  不改评分标准、主 Agent、超时参数或预算；cache_evaluations=false 的训练/验证仍重新执行。
- 位置：tools/trajectory_evolution/src/gepa_search.cpp、tests/test_gepa_search.py、
  docs/24-evolution-and-gepa.md。仅修改组件，不修改 AgentFlow C++ 底座。
  图分析为 UNKNOWN，已核对训练、验证和最终成对留出评测的共用调用入口；不视为无影响。
- 验证：新增本地裁判 HTTP 503 回归，修改前复现恢复未走 regrade，修改后通过（6.12 秒），
  两条试次仅补评分，原试次 ID、任务版本、轨迹、指标、执行时延及旧报告字节保持不变。
  GepaSearchTests、AgentEvaluationTests、DocumentationTests 三组全部通过（317.35 秒）。
  原真实 GEPA 第 3 轮恢复后，4 条已完成执行仅补 4 次评分，3 passed、1 failed、0 error；
  regraded_trials=4，新尝试的 .work 为空。逐条核对原试次身份、轨迹、产物、输出、调用指标与执行时延均未变化。
  配置、数据集、通过门槛保持不变，原错误报告保留；这不代表最终 GEPA 收益已经达标。

### CHG-20260913-255：HTTP 429 保留当前 GEPA 代并停止搜索

- 问题：真实实验中评分 API 返回 429 后，GEPA 把无法评分视作普通候选无结论，继续推进；
  实际仅做 3 次反思，却消耗了 10 轮预算，后 7 轮均因缺少有效父版本评分而失败。
- 修改：评分报告先完整保存，再检查明确的模型 HTTP 429；反思 HTTP 429 保留当前 pending 代。
  两者以 gepa_model_rate_limited 返回作业层，不清除 active、不推进后续代或留出评测。
  限流解除后通过原 retry 接续，复用检查点、候选和已完成成绩，不退还已占预算；业务失败不受影响。
- 位置：tools/trajectory_evolution/src/gepa_search.cpp 的 evaluate、reflect、error，
  tests/test_gepa_search.py、docs/24-evolution-and-gepa.md；不修改 AgentFlow C++ 底座。
  图分析为 UNKNOWN，已核对训练、验证、留出评测和反思调用的异常传播路径。
- 验证：新增父版本评分与反思两个 429 回归，旧实现均复现未按预期停止；修复后连同原评分恢复测试
  3 项通过（17.34 秒）；GepaSearchTests、AgentEvaluationTests、DocumentationTests 全部通过（328.66 秒）。
  未手工回退真实旧树的轮次或修改其已冻结的留出版本。

### CHG-20260913-256：记忆管理被限流后停止本批后续模型请求

- 问题：真实批次的增量匹配及全库整理共发起 49 次模型请求，3 次返回 HTTP 429；原逻辑把限流当作
  单条语义失败后继续请求，放大失败开销，且并发重算可能重新发起请求。
- 修改：仅在当前入库调用内记住明确的 HTTP 429，后续不再发起新语义请求；并发写入重算继承此状态。
  精确去重、保守保存和已完成的有效关系仍保留；全库整理记录 http_429 并保留待处理计数。
  后续新批次可以重试，相同已应用批次仍按原规则幂等，不伪造模型成功或退还预算。
- 位置：tools/trajectory_evolution/src/evolution_pipeline.cpp 的 MemoryCurationContext、run_memory_match、
  run_full_store_curation、curate_memory_file，tests/test_evolution_pipeline.cpp、docs/13-private-memory.md。
  仅修改组件，不修改 AgentFlow C++ 底座；图分析为 UNKNOWN，已核对入库、全库整理与并发重算调用。
- 验证：新增增量入库、全库整理、并发重算与后续批次恢复回归，旧实现已复现限流后仍继续调用；修复后通过。
  TrajectoryEvolutionTests、UnifiedLearningTests、LearningScheduleTests、SharedMemoryTests、DocumentationTests
  五组全部通过（15.34 秒）；没有改变真实旧批次的不完整标记或把回归测试当作真实 API 复测结果。

### CHG-20260914-257：GEPA 原始 loop 防回归、经验边界与互补分支合并

- 问题：L1 留出集通过率从 13/15 提高到 15/15，但无效 loop 均值从 1.1333 升至 1.4，
  未通过发布门槛。原搜索优先通过率，并用饱和归一化成本比较效率，可能掩盖原始次数回归。
  同时，轮换选中的经验可能与训练反馈无关，多代正文发生与原触发条件不一致的漂移。
- 修改：逐题成绩保留 invalid_loops 原始均值与 loop_method；启用 loop 目标时，父子筛选、
  验证集最终选优及重组 no_worse 检查同样本、同口径的原始 loop 均值不得增加。
  不修改任务评分、数据集、holdout 发布门槛，也不将未知 loop 当成零。
  随包配置使用 model_choice；保留显式 round_robin 兼容。反思可返回空改动；正文已改写时
  提供 original_content，提示词要求保持原条件、约束与例外，不能把可恢复错误扩大为整项终止。
  controls 删除重复的来源记忆，未增加模型调用；语义泛化仍由真实评测检验。
- L2：原真实实验未开启 control_learning，实际只能优化经验。搜索树与摘要增加
  available_components/search_warnings，未开启或操作白名单缺失时明确提示，既不崩溃也不自动扩权。
  生成规则仍只核验缺失/空参数，不能解释为依赖历史状态的重复派发控制。
- 合并：原先要求两条分支整体均不差于共同祖先，提前排除了逐任务前沿上的局部专长。
  现允许每条分支至少有一项验证任务改善后尝试融合；仍要求非祖先关系、共同祖先、
  不同分支实际贡献互补组件，且融合候选真实子集成绩不差于双亲，最终另过 holdout。
  更新配置指纹，防止旧成绩与新规则在同一恢复上下文混用。
- 公共库排查：管理周期已完成，47 条受管来源均为训练轨迹，7 个合格经验来源被记为
  private_only/independent_evidence_missing，公共提炼 attempts=0。现有 public_evidence 排除
  training_source；未删除该保护。纯离线训练晋升公共经验需要另外定义准入规则，本次不声称已验证公共生成。
- 位置：tools/trajectory_evolution/src/gepa_search.cpp、gepa_strategy.cpp/.h、config/gepa.json、
  config/prompts/gepa-reflection.txt、相关测试、docs/24-evolution-and-gepa.md、14-shared-memory.md；
  本地分类问题清单同步补充。没有修改 AgentFlow C++ 底座、v1 业务代码或凭据。
  图分析：scores/improves/packet/no_worse 为 LOW；select_merge 为 UNKNOWN，已核对直接调用。
  components 为 HIGH，保持其实现不变；搜索由后台评测 worker 调用，不进入主 Agent 执行链路。
- 验证：GEPA 主回归 44 项、配置/策略两项通过（347.59 秒）；补充 L2 缺失操作白名单等
  3 项复测通过（23.606 秒）。覆盖原始 loop 回归、度量缺失/口径差异、低均值互补分支、
  双亲共同生成并发布子节点、L2 实际工具规则、L3 循环规则、原始约束传递和已有恢复流程。
  用户要求暂停真实实验，因此保留旧报告，未继续真实 API 试验；不把本地回归当作留出收益达标。
  AgentEvaluationTests、SharedMemoryTests、UnifiedLearningTests、PolicyRuntimeTests、DocumentationTests
  五组亦全部通过（52.92 秒），git diff --check 通过。

### CHG-20260914-258：从通用打点自动提取工具契约，减少手工接入

- 问题：要求用户重复配置工具名、参数名单、模型注入位置和 token 映射，且 L3 循环搜索
  被动作名单绑定；Python 配置入口缺少异步快捷方法。普通接入因此需要额外维护框架相关信息。
- 修改：在包内共享边界读取实际模型请求的结构化工具声明，自动检查后续动作的顶层必填参数。
  OpenAI 兼容、Anthropic、Responses、Gemini 及旧 functions 格式共用同一实现；
  不从回复文本、用户历史或源码推断工具权限，不把可选、空值或 null 自动判为缺失。
  L0/L1 仅观测；L2 及以上、允许应用策略时才拒绝明确缺少必填参数的动作。
  缺少定义、定义冲突、超过容量时保持 unknown 并保留原调用。只检查存在性，不代替完整 JSON Schema 校验。
- 并发与轨迹：定义限定在当前 run/actor/scope/分支，不跨用户和 run 复用；冲突不采用最后写入者。
  缓存最多 128 个分支，每分支 64 个工具、64 KiB 紧凑定义；轨迹记录 tool_contracts、
  contract_check 及 enforced，不把固定检查伪装成某条进化策略的应用回执。
- 模型点：常见请求自动识别注入位置；响应 usage 自动提取输入、输出、缓存等计数。
  显式 binding、metrics/output_metrics 保持优先；未知格式仍可在同一打点声明位置或计数。
  非法或冲突的计数保持未知，不补零；未新增模型调用或第三方依赖。
  对无法区分接口的纯 user/assistant 请求，复用已有用户文本，不新增可能不兼容的 system 角色。
- GEPA：默认不再要求 control_learning.actions。固定接口契约不是可学习参数；
  available_components 只列实际可变维度，fixed_controls 单列固定校验。
  L2 默认可优化经验，已有显式动作扩展规则继续兼容；L3 有循环授权和基础上限即可独立搜索。
  未实现任意调度顺序改写。配置指纹隔离旧搜索缓存；本条取代 CHG-257 中缺少动作名单即警告的默认行为。
- 接入：ConfiguredPoints 补齐 acall/aallow_loop，沿用包内身份、超时和隐私处理；
  文档区分推荐 entry 接入与旧 stdio 协议，不再要求普通用户另写评测启动器。
  配置章列出仍需提供的任务关系、业务结果、原消息入口、评分标准和授权/预算，区分必要事实与可选映射。
- 位置：tools/trajectory_evolution 内的 model_contract.cpp/.h、boundary_runtime.cpp、
  gepa_search.cpp、Python 配置客户端、相关测试与使用文档。没有修改 v1 业务代码或 AgentFlow C++ 底座。
  图分析：components 为 HIGH，已告知并核对 compose/mutate/merge/packet 等调用；
  边界和新增测试的 UNKNOWN 已用直接调用与入口核对补充，未将其视为无影响。
- 真实 API 实验保持停止；数据集、memory 和已有实验报告保留，本次只运行本地回归。
  测试套件增加后 GEPA 的 CTest 上限调整为 600 秒，不修改生产模型调用超时。
- 验证：完整构建通过；30 组非 GEPA 搜索回归全部通过（183.91 秒），涵盖评测、训练冷启动、
  私有/公共记忆、策略、跨语言/跨进程及日志分析。GEPA 46 项全部通过（373.609 秒），
  包括无动作名单的 L3、显式 L2 兼容、双亲合并、离线版本和中断恢复；旧 360 秒上限的一次超时
  已用完整成功结果复核。最后的请求格式兼容补丁重新构建并通过 11 组定向回归（22.49 秒），
  自动契约场景在原生/HTTP 下各有 6 条闭合轨迹，均通过 doctor，且未保存测试接入令牌。
  git diff --check 通过。以上为本地确定性测试，不作为真实模型效果或留出集收益结论。

### CHG-20260914-259：冷启动训练来源的公共经验准入与定级评测

- 问题：public_evidence 一律排除 training_source，纯离线冷启动虽然已有合格私有经验，
  公共提炼仍不会执行。不能通过重复创建测试用户来满足真实用户支持门槛。
- 修改：复用原有公共管理流程，训练证据必须匹配当前冻结数据集的已审核 train 任务与版本，
  属于已导入的无策略 L0 baseline_training，且采集、分析、来源指纹及 trial 绑定核验通过。
  排除经验注入回声；验证/留出与带策略候选轨迹不进入此通道。还需配置真实 Agent 策略评测入口。
- 支持分别记录：online_verified / supporting_users 为真实在线来源；training_groups /
  supporting_training_groups 按数据集版本与任务组去重，跨测试身份重跑同组只计一次。
  默认 allow_training_sources=true、min_training_groups=2；任一类支持单独达标才能晋升，
  不把两类票混合凑门槛。模型提炼、独立请求复核、跨库去重/冲突、撤回、过期和容量淘汰保持原路径。
  公共 published 只表示可编译候选经验，不代替候选策略的真实 Agent 评测与发布门槛。
- 定级：服务配置可选 evaluation_levels；["L2"] 只自动提交 L2 作业，仍有配对 L0 基线。
  省略保留原来的各级评测，[] 不自动提交策略评测或补充训练；不取消已排队作业，不限制显式评测。
  普通实验不依赖手工工具白名单、必填参数名单或注入/用量映射。
- 位置：tools/trajectory_evolution/src/shared_memory.cpp/.h、integration_service.cpp，
  对应测试及文档第 3、14 章。没有修改 AgentFlow C++ 底座或 v1 业务代码。
  图分析：update_publication 为 HIGH，已在修改前告知；其余已解析调用为 LOW，
  构造入口等 UNKNOWN 已核对直接调用，不按无影响处理。
- 验证：构建成功；AgentEvaluation、TrainingBootstrap、UnifiedLearning、AutomaticContract、
  SharedMemory 五组通过（71.73 秒）；新增指定 L2/空等级两项回归通过（2.351 秒）。
  文档更新后五组定向回归再通过（16.98 秒）。覆盖同组跨用户不增票、独立组晋升、关闭训练支持、
  冲突暂停与证据撤回。首轮真实 API 验证完成 28 次公共提炼/复核/关系调用，产生 2 条 published、
  2 条 pending_support；真实用户支持均为 0。14 项跨库关系均完成，无管理错误。
- 真实 v1 正式 L2 实验已启动，使用原冻结数据集及现有私人/公共经验；收益以其最终报告为准。
  实际执行已观察到私人/公共经验的 host_boundary_verified 回执，以及自动工具契约校验；
  无手工 L2 工具名单，未修改现有宿主业务代码。启动时的环境缺项已补齐并重新运行，失败试跑不计成果。
  一条历史训练轨迹重试仍因模型引用不存在的事件 ID 被拒绝，保持 failed 并排除于公共支持，
  未将此记录改写为成功，也不将启动实验表述为最终验收通过。


### CHG-20260914-260：GEPA 混合批次评分续跑复用完整执行检查点

- 日期：2026-09-14。
- 问题与诱因：同一批次部分题目评分超时、其他题目正常时，跨任务缓存先复用正常题目，再把缺失题目拆成新评测配置；新配置失去原来的执行检查点，可能重新调用已经完成的 Agent。
- 改动：在同键报告存在 execution_status=completed、error_code=evaluation_judge_failed 的记录时，优先交给已有检查点续评路径。仅增加 5 行生产逻辑；不改变传输重试次数、业务判分、无效 loop 口径或发布门槛，不修改 AgentFlow 底座及 v1 业务代码。
- 位置：tools/trajectory_evolution/src/gepa_search.cpp 的 Search::evaluate；新增回归在 tests/test_gepa_search.py。
- 影响分析：Search::evaluate 为 LOW，直接影响 Search::run/validate/merge，间接进入后台 AgentEvaluationService；测试 HTTP 回调的图结果为 UNKNOWN，已确认由本地测试服务器动态调用。
- 验证：构建成功；4 项定向回归通过（28.743 秒），覆盖混合失败续评、全部评分失败、429 保留代次及部分缓存恢复。真实 v1 的 t-10、t-22 续评通过，四题原始轨迹、耗时和调用指标逐项不变，报告记录 regraded_trials=2。
- 全量 GepaSearchTests、GepaConfigurationTests、GepaStrategyTests 均通过（381.18 秒）；实验中候选的业务失败保留，不当作程序异常改写成绩。

### CHG-20260914-261：运行边界采集降级输出安全错误码

- 日期：2026-09-14。
- 问题与诱因：Python HTTP 打点失败时保持业务执行，但调用方未检查返回值时，run 起止采集丢失没有明确诊断信息，评测可能一直等到整题超时。
- 改动：只在 run 边界采集降级时输出经过格式校验的固定错误码，覆盖 HTTP 传输异常与服务端 fail-open 响应；不记录业务输入、身份或原始异常，不改变业务返回值、采集完整性判断及重试策略。
- 位置：tools/trajectory_evolution/clients/python/evolution.py 的 Evolution.point；不修改 AgentFlow C++ 或 v1 业务代码。
- 影响分析：GitNexus LOW，两个直接关联为配置客户端的覆盖类及其 point 方法。
- 验证：3 项定向安全诊断检查通过，InstrumentationTests 通过（3.78 秒）。三轮真实 v1 复现收齐三个 run，125/55/72 个事件全部正常闭合；此前偶发第三轮缺失尚未稳定复现，本条仅改善可定位性，不将其标记为根因已修复。

### CHG-20260914-262：GEPA 支持受限经验停用与恢复

- 日期：2026-09-14。
- 问题与诱因：搜索仅能改写已批准经验的正文，不能评测不注入某条经验的效果；经验无收益或引入干扰时，继续强化文字并不一定改善执行。
- 改动：experience:ID 的值允许为 null，表示从该候选的 approved_experiences 中移除；种子批准的组件轴、原始经验及适用条件继续保留，后续子代可用完整正文恢复同一来源。复用现有策略字段，不新增 Protobuf 字段，不删除记忆、不放松基础规则；引用被停用来源的生成规则仍须通过既有授权校验。
- 位置：tools/trajectory_evolution/src/gepa_search.cpp 的 components、compose、packet，以及 config/prompts/gepa-reflection.txt；不修改 AgentFlow 底座或 v1 执行业务。
- 影响分析：components 为 HIGH，覆盖反思、变异、融合及后台评测；compose、packet 为 LOW。已在修改前告知风险。测试 HTTP 回调为 UNKNOWN，确认是本地 fixture 服务器的动态入口。
- 验证：编译成功；经验停用后下一代恢复、改写保留原始约束、双父分支联合生成三项回归通过（26.728 秒）。完整 GepaSearchTests、GepaConfigurationTests、GepaStrategyTests 均通过（385.97 秒）；真实收益须以程序最终判分为准。

### CHG-20260914-263：GEPA 临时评分错误自动补评一次

- 问题与诱因：真实 v1 候选四题均已完成，但三题评分请求超时；搜索直接标记候选 inconclusive 并跳过验证。之后对相同轨迹重评，四题全部通过且无效 loop 为 0，说明不能把临时评分不可用等同于候选无收益。
- 改动：Search::evaluate 只对完整执行检查点中的临时网络/服务端评分错误自动补评一次，复用既有续评路径；已成功评分和 Agent 执行不重跑。保留总时限与试验预算，不重试执行失败、鉴权失败、协议错误或 429；持续错误保持 inconclusive，不伪造判分。
- 位置：tools/trajectory_evolution/src/gepa_search.cpp、tests/test_gepa_search.py；不修改 AgentFlow 底座、v1、模型提示词或评分口径。
- 影响分析：GitNexus evaluate（限定上述 C++ 文件）为 LOW，直接涉及 run/validate/merge，间接进入后台评测。测试回调为 UNKNOWN，已确认由本地 ThreadingHTTPServer 调用。
- 验证：编译成功；自动恢复、持续失败后的显式重试、429 保留代次三项回归通过（14.705 秒）。检查原 trial_id、轨迹、耗时与调用指标不变，仅失败题目发生补评。完整 GepaSearchTests、GepaConfigurationTests、GepaStrategyTests 均通过（387.82 秒）。

### CHG-20260915-264：固定候选自动导出与真实 v1 固定复现

- 问题与诱因：导出 published-version.json 的逻辑仅在 GEPA 分支内。固定候选即使通过验收并发布，也只有 candidate.json 和发布记录，没有可直接离线加载的版本包；发布保存和文件落盘之间中断也缺少补导出。
- 最小修复：AgentEvaluationService 复用同一导出方法。固定候选输出到作业根目录，GEPA 保留 search/ 路径；仅实际发布的版本导出。将 published 标记和发布登记一同保存，重启时核对原版本、用户、等级、上下文与通过门槛，再补齐缺失文件。空发布占位跳过；旧上下文只补文件，不获得新上下文的准入。已有且一致的文件不重写，不一致时报错，继续使用原有原子写入和 0600 权限。
- 位置：tools/trajectory_evolution/src/agent_evaluation.cpp、tests/test_agent_evaluation.py；更新包文档第 24 章。没有改动 AgentFlow C++ 底座或 v1 执行业务。
- 影响分析：GitNexus 对构造/后台线程入口及新增复现入口为 UNKNOWN，已确认 integration_service 的服务构造、工作线程与脚本 __main__ 调用关系，未按无影响处理。GEPA 定向回归入口为 LOW。
- 固定复现：增加 scripts/reproduce_v1_l3.py 与 configs/reproduction/v1-l3/。保留最终验收的 L0/L3 版本、15 道 holdout 题、评分规则、模型参数和运行材料指纹，调用原有 agentflowd + Bridge 入口；不增加另一份 Agent，不重新搜索或更新记忆。凭据读取已有本地 .env，生成输入放 build，结果放 trace_output/v1-l3-reproduction 的新目录，不覆盖旧报告。
- 验证：重新构建成功；AgentEvaluationTests 全部 20 项通过（66.109 秒），另 3 项 GEPA 发布/拒绝/离线加载回归通过（21.945 秒）。覆盖固定 L0–L3 自动导出、旧记录补导出、幂等、内容不一致拒绝、旧上下文不重新准入、离线经验注入及用户隔离。真实验收作业 6614d774… 已按原发布记录补导出，固定复现 --check 通过；本次未重新执行 30 个真实模型试次，不将历史 50% 无效 loop 降幅作为新的测量结果。

### CHG-20260915-265：策略包独立导入及真实离线路径对照

- 问题与诱因：离线加载先按当前配置读取经验库、编译策略，再要求与发布策略整体兼容；仅请求字段绑定不同也会拒绝导入。初始化还读取旧 runtime/template 文件。评测内联 artifact 绕过这条离线路径，导致评测通过不能证明单文件部署成功。
- 最小修复：HTTP 和原生入口在离线模式直接绑定包内完整策略，不读取或编译历史经验，不加载反思配置和外部经验模板；保留应用/用户隔离、作用域和能力校验，以及当前显式基础规则和更严格预算。作用域沿用宿主现有编码，避免特殊字符身份误判。在线记忆编译/评测逻辑不变，不修改 AgentFlow C++ 底座或 v1 业务代码。
- 评测接入：variants 新增 policy_version_file，验证发布标记和内容版本后冻结 artifact；与内联 artifact 互斥。包内容进入配置指纹，避免同路径换包复用旧结果；策略组走真实离线校验，保留完整试次身份与轨迹验证。版本文件读取上限与发布包保持一致。
- 位置：自进化包 src/agent_evaluation_support.h、integration_service.cpp、local_runtime.cpp、task_evaluation.cpp 及对应测试；文档第 19、24 章。
- 影响分析：共享 apply_agent_version 图分析为 CRITICAL，覆盖 HTTP/native、评测和调用链；修改前已告知。实际修改限制在离线分支，在线入口一起回归。
- 验证：完整构建成功；AgentEvaluation、TaskEvaluation、LocalRuntime、IntegrationService、BoundaryTransport、Instrumentation、Documentation 七组通过（73.59 秒），另三项 GEPA 发布/拒绝/离线回归通过（21.984 秒）。覆盖旧文件缺失、旧经验损坏但不读取、特殊字符作用域、用户隔离、能力缺失与更严格限制、换包指纹变化、无效包拒绝。实际已发布 L3 包在无旧配置/模板/经验的原生实例中成功加载并在匹配条件下完成注入，版本 ID 不变。
- 全新真实 v1 对照已启动：15 个新多轮任务、两组各两次，共 60 个任务试次；只复用导出策略，不复用旧训练/评测配置或经验库。业务收益等待程序完整结果，不把历史降幅作为本轮成绩。
- 真实接入复核：同一两轮任务的 L0/L3 均 execution_status=completed，四个 Run 起止完整；L0 为 0 次注入，L3 为 4 次 llm/applied/host_boundary_verified，加载版本与原发布包一致。其余未实际应用的回执仍为 rejected，不计注入。该复核未使用模型判分，只验证执行/采集/导入链路；L0/L3 分别 5/6 次 LLM 调用，不能用它宣称性能提升。正式 60 试次对照于 02:18 UTC 重新启动。

### CHG-20260915-266：评分执行视图省略重复策略检索载荷

- 问题与诱因：include_model_requests=false 已去掉模型请求，但 policy.query 诊断仍重复携带查询、注入上下文及经验列表；真实 L3 多轮任务的评分输入因此超过 262144 字节。
- 最小修复：只在精简评分视图中，省略明确标为 custom/policy.query 的 decision.query/context/experiences/policy_id。保留动作决定、循环决定、注入回执和所有真实执行输出；不改变原始轨迹、分析、策略、评分标准及长度上限，不处理同名业务字段。不修改 AgentFlow 底座。
- 位置：tools/trajectory_evolution/src/model_input.h 的 task_evidence_view；tests/test_task_evaluation.py；包文档第 19 章。
- 影响分析：图分析返回 CRITICAL，修改前已告知；核对实际调用为 task_evaluation 的模型输入构造，未修改其他分析视图。
- 验证：构建成功，TaskEvaluationTests 全部 97 项通过（65.146 秒）。真实 JSON 修复题的两次 L3 均完成两轮采集和模型评分，未再超限；两次业务判分为 failed，原样保留，不作为策略收益。

### CHG-20260915-267：RPC 参数解析不再误计字符串中的括号

- 问题与诱因：RpcServer 的 extract_json_params 仍手工数大括号/中括号，不区分 JSON 字符串。第一轮回答含 layers:2,} 等不配对文本时，第二轮请求携带历史导致合法参数被截断，daemon 返回参数错误，评测记录 evaluation_entry_handler_failed。
- 最小修复：该函数改用同文件已有 RapidJSON 解析和序列化方法，仅接受对象/数组参数；其他业务、派发、打点和策略逻辑不变。这是实际协议层缺陷，故必要地修改这一处 AgentFlow C++ 底座函数。
- 位置：src/daemon/rpc_server.cpp；tests/unit/src/test_rpc_handler_gtest.cpp。
- 影响分析：GitNexus 返回 UNKNOWN，已通过调用处确认 RpcServer::handle_request 直接使用它，覆盖 TCP/WebSocket/UDS 的请求分发，未按无影响处理。
- 验证：原真实失败轨迹可本地确定性复现：完整请求合法，但旧截取结果非合法 JSON。重新构建 agentflowd 和 daemon 测试；5 项 RpcServerTest 通过，新增真实 UDS 收发测试覆盖字符串括号、转义引号/反斜线、嵌套对象、数组、空参数集合及非法标量。修复后真实 L0/L3 各两次复测共 8 个 Run 全部闭合，4 个试次均正常完成评分，无入口异常；业务通过 2/4，失败按原判分保留。另五组服务、原生、打点及文档回归全部通过（5.65 秒）。

### CHG-20260915-268：评分去重覆盖重复输出与回执，CLI 接通已有断点续跑

- 问题与诱因：三轮真实任务的 L3 轨迹完整，但相同模型/工具输出在 action.end、step.end 中重复，注入回执也重复携带字段；原 compact_judge_evidence 只共享 input，精简后仍超过评分上限。TaskEvaluationOptions 已支持安全续跑，但 CLI 未暴露入口，暂停后难以复用已执行题目。
- 最小修复：share_repeated_inputs=true 时，使用同一无损引用表共享事件 output、attributes 内的载荷、application；不改事件身份与顺序，不改顶层状态及产物，不截断内容，且只有总字节减少才采用。CLI 新增 --resume-report，将路径传给原有检查点实现；保留配置/版本/提示词指纹、报告和产物哈希核验，与 --regrade-report 互斥。
- 位置：自进化包 src/task_evaluation.cpp、task_evaluation_cli.cpp、tests/test_task_evaluation.py；文档第 19 章。不改 AgentFlow 底座、策略包、数据集或评分标准。
- 影响分析：compact_judge_evidence 的图结果为 CRITICAL 且符号数据异常，不作为可信完整调用清单；已告知并人工确认实际调用在模型评测组包。CLI 为 UNKNOWN，确认由 dataset_cli 的 run 子命令调用。
- 验证：构建成功；无损还原（含自定义属性、嵌套引用和同名伪引用）、CLI 只补评不重执行、默认去重三项回归通过（4.991 秒）。新测试覆盖原始轨迹/ID/用量/耗时和源报告不变、重复续跑不调用模型、配置变化和模式冲突拒绝。正式实验保留已完成的通过与失败，仅补评评分失败和续跑未完成题目。


### CHG-20260915-269：报告容量使用完整配置预算，并校验最终序列化大小

- 问题与诱因：累计 trial 的 Protobuf 大小只允许达到 max_report_bytes 的一半，配置 128 MiB 时约 64 MiB 即提前报 evaluation_report_size_limit；最终 JSON 却只检查固定 128 MiB，没有严格遵守较小配置值。
- 最小修复：累计证据使用完整配置预算；最终 JSON 和 Protobuf 均按 max_report_bytes 校验。配置可选范围、评分输入、Agent 执行、策略及数据集均不改变。
- 位置：自进化包 src/task_evaluation.cpp、tests/test_task_evaluation.py；不修改 AgentFlow 底座。
- 影响分析：run_task_evaluation 图查询仍为 UNKNOWN，候选最高 CRITICAL；已告知并确认命令行评测、GEPA、后台评测共用此函数，只修改报告存储边界。
- 验证：构建成功；报告超过原半预算仍可正常保存、trial 能容纳但最终报告超限被拒绝、CLI 复用执行仅补评分、模型重评不改源数据的 3 项定向测试通过。真实实验在 Agent 退出后保存检查点，19 条已有评分逐项完全一致，另 1 条仅补评分，其轨迹、产物、输出、版本、指标与耗时均未改变。
- 完整回归：TaskEvaluationTests 的 99 项测试全部通过，DocumentationTests 通过，总计 67.28 秒；单元测试未调用付费模型。

### CHG-20260915-270：评分输入补充执行事实，逐轮清单按观测顺序排列

- 问题与诱因：模型理由曾把 5 次实际视频调用写成 3 次，或先判业务通过、再承认逐轮存在被题目禁止的重试；哈希顺序的轮次清单增加阅读负担。另有裁判把评测器事后解码产物、内部 Skill 说明当作 Agent 已完成题目要求的文件核验。
- 最小修复：模型输入增加 observed_action_counts，复用已有严格 action 配对计数，包含失败、排除明确未执行，缺少观测为 null；artifact_provenance 明示产物检查来自评测器。短轮次 ID 按各 Run 中锚点的观测顺序生成并补零排序，落盘规范 ID、事件和因果关系不变。提示词明确文字标准不因未配置硬检查而免除，内部规范不能减免必需动作；先核验事实和逐轮贡献，最后输出 verdict。
- 位置：自进化包 src/task_evaluation.cpp、config/prompts/task-judge-system.txt、task-loop-review.txt、tests/test_task_evaluation.py。不修改 AgentFlow 底座或 v1 业务逻辑。
- 影响分析：model_grade 图查询为 CRITICAL；已告知该入口由初次评测、模型重评、GEPA 和后台评测共用。图索引调用映射存在已知不完整/歧义，另核对实际调用入口，不将其当作无影响。
- 验证：102 项任务评测测试通过；覆盖未知/未执行计数、12 轮补零排序及六个独立执行分支，原始轨迹与规范 ID 不变。真实旧轨迹复核中，t-04 的 5 次/1 次视频调用分别失败/通过；t-07 的 8 次/3 次调用均因停止条件违规失败；h-image-english 两条均识别缺少 Agent 实际文件核验。最终两条文件复核未重跑 Agent。模型协议正确不等于语义永远正确，仍保留原始评分与各次复核供审计，不手工覆写结论。

### CHG-20260915-271：允许显式选择暂停批次中已完成的题目重评

- 问题与诱因：仅支持完整报告重评，暂停大批实验后，即使目标题目的两臂均已执行完成，也无法单独诊断评分。
- 最小修复：--regrade-report 在显式配置 task_ids 时可读取不完整批次，但选中题目的全部版本与重复编号必须已完成执行；缺题、取消或失败执行继续拒绝。不指定题目仍要求源批次完整。原 manifest、报告/产物哈希、身份和题目一致性检查保留。
- 位置：自进化包 src/task_evaluation.cpp 的 run_task_evaluation 及 tests/test_task_evaluation.py。
- 验证：暂停三题批次后，仅完成题可重评，取消题、未开始题及未显式选择均被拒绝；模型重评不再执行宿主。真实暂停批次成功复核选中题，来源报告 SHA-256、试验 ID、执行耗时、用量与产物保持不变。

### CHG-20260915-272：GEPA 可保留局部优势继续反思和双亲重组

- 问题与诱因：中间子节点也必须满足整体质量、fitness 与原始 loop 不退化，可能在完整验证前丢弃有价值的局部改进，减少分支互补机会。只优化 loop 而未启用时延目标/门槛，也不能保证 Agent 更快。
- 最小修复：新增可选 acceptance=pareto。相同题目集合、相同 loop 口径下，至少一题质量改善，或某题质量不降且 fitness/循环/token/时延效率之一改善，允许进入完整验证；缺失指标不当作收益。同题失败换来的低耗时不算该题效率优势。重组对子代与双亲、分支与共同祖先使用同一规则；默认严格模式不变，最终 winner 和 holdout 发布门槛不放宽。反思输入明示当前接纳方式，L3 有循环规则组件时检查实际阈值改进。
- 位置：自进化包 src/gepa_strategy.h/.cpp、src/gepa_search.cpp、config/prompts/gepa-reflection.txt 及相应测试。不修改 AgentFlow 底座。
- 影响分析：共享搜索、选父与重组入口图查询为 UNKNOWN/候选 CRITICAL，已告知并核对真实调用；修改限于可选中间筛选，不改变授权、版本持久化和最终准入。
- 验证：GepaSearchTests 完整通过（404.27 秒），策略/配置测试通过；覆盖局部优势、缺失指标、效率分支参与重组、合并退化拦截、无手工工具白名单的 L3 规则执行，以及中间合并通过但最终 loop 门槛不满足时仍不发布。真实收益由新一轮固定数据划分的真实 v1 实验测量，不能由单元测试推断。


### CHG-20260915-273：评分执行视图大幅压缩并保留跨执行交付

- 问题与诱因：真实多轮题 t-24 的模型评分输入约 332720 字节，超过 262144 字节上限。主要膨胀来自 action 与 step 的重复包装、每轮完整模型请求/响应信封、策略查询与注入遥测，以及同一输出在多个事件中的重复；直接删除 step 交付又会让裁判看不到 Skill 返回 Coordinator 的结果，把首次有效交付误判为无效 loop。
- 最小修复：仅改变发送给裁判模型的临时执行视图。原始落盘轨迹、分析证据和报告证据不删减；有完整 action 证据时去掉 action 对应的 step 包装、模型请求、隐藏 reasoning、供应商元数据、策略查询和重复注入遥测，保留 action 结果、工具参数/结果、loop 决策、每个独立执行的首个输入、带 role 的跨边界消息及终态交付。无完整 action 证据的框架继续保留原 step 轨迹。重复 input/output/attributes/application 仍只做可逆精确引用。loop inventory 从同一模型可见视图生成，避免向模型提供不可引用的事件 ID。
- 位置：tools/trajectory_evolution/src/model_input.h、src/task_evaluation.cpp、tests/test_task_evaluation.py；不修改 AgentFlow C++ 底座、v1 业务代码、评分标准、原始轨迹或长度上限。
- 影响分析：model_grade 为 HIGH，覆盖初次评分与重评并进入后台评测和 GEPA；compact_judge_evidence 的图结果为 CRITICAL 且调用映射有已知歧义，已在修改前告知并核对实际调用只位于评分组包路径。task_evidence_view 为 UNKNOWN，已通过文本调用关系确认由 model_grade 使用，未将 UNKNOWN 当作低风险。
- 体积验证：t-24 原始落盘证据仍为 314 个事件；模型视图保留 86 个。按当前 C++ 组包规则复现的用户输入为 131763 字节，较原约 332720 字节减少约 60.4%，低于 262144 字节上限；系统提示词另为 11214 字节，不计入该本地上限。
- 判分验证：同一批四条真实 v1 轨迹仅重评、不重跑 Agent，4/4 全部完成评分且零错误。原来可评分的 t-11、t-26、t-29，其 verdict、每个规范 iteration ID 的 keep/waste 决策及除 loop_review 外的原始证据逐项相同（分别为 4/2、8/0、10/0）；此前超限的 t-24 完整覆盖 19 轮，得到 16 keep、3 waste。四条复评的原执行证据均逐字段不变。
- 回归验证：重新构建成功；5 项压缩/引用/loop 定向测试通过；TaskEvaluationTests 完整通过（69.08 秒）；AgentEvaluationTests 通过（71.92 秒）；GepaSearchTests 通过（401.70 秒）。新增测试明确覆盖跨 Agent 的 role/tool_call_id/content 消息不可被压缩删除。真实复评结果保留在 trace_output/v1-l4-coldstart/l3-current/t24-compact-regrade-role/，三个错误或不完整复评副本及临时配置已删除，无法恢复。

### CHG-20260915-274：GEPA 复用执行检查点恢复评分协议错误

- 问题与诱因：真实 v1 搜索中 Agent 已完成任务，但裁判偶发返回非 JSON，或给出不满足严格逐轮证据约束的 loop review；单次请求内修复耗尽后，搜索把候选直接记为 inconclusive，放大了评分器格式波动并产生无效节点。
- 最小修复：在已有“一次性检查点补评”中加入 invalid_json 和 evaluation_loop_review_invalid。仍严格拒绝无效响应，不放宽 JSON/证据校验；只对 execution_status=completed 且唯一失败来自模型评分的试次补评一次，持续失败仍保持 inconclusive，Agent 不重跑。关闭评测缓存时行为不变。
- 位置：自进化包 src/gepa_search.cpp、tests/test_gepa_search.py；不修改 AgentFlow 底座、v1、提示词、数据集或评分标准。
- 影响分析：Search::evaluate 为 LOW，覆盖搜索运行、验证与融合；测试回调和辅助方法为 UNKNOWN，已通过文本搜索确认仅由本地 HTTP fixture 和三条缓存测试使用。
- 验证：重新构建成功；invalid_json、HTTP 瞬断、loop-review 证据错误、未知 loop 指标和关闭缓存共 5 项定向测试通过（24.35 秒）。恢复前后的 trial_id、执行耗时、指标和原始轨迹一致，仅失败评分被重新调用；完整 GepaSearchTests 51 项通过（401.96 秒）。

### CHG-20260916-275：L2 动作控制由当前工具 schema 自动授权

- 问题与诱因：L2 可变动作规则依赖 `control_learning.actions` 手工列出工具和必填参数；普通接入不配置时，GEPA 实际只能改经验或 L3 loop。若改为从基线执行推导动作空间，又会把评测结果误当成能力声明。
- 最小修复：动作打点从当次模型请求携带的工具 schema 中提取必填参数，仅作为分支内运行时证据。新增通用 `* + $schema_required_nonempty` 候选原语，可学习“当前 schema 必填参数不得缺失或为空”；不枚举工具名，不改写业务参数，不从基线轨迹产生动作空间。已有显式工具规则和授权方式保持兼容。
- L3 搜索：启用控制学习时，无手工工具白名单也会暴露 `action_rules`；`round_robin` 先后搜索 action、loop，再轮转经验组件，保证 L2/L3 在有限节点预算内真正参与进化。
- 位置：自进化包 `runtime_policy.proto`、`boundary_runtime.cpp`、`policy_runtime.cpp`、`control_generation.cpp`、`gepa_search.cpp`、`gepa_strategy.cpp`、两个热替换提示词及对应测试。不修改 AgentFlow C++ 底座或 v1 业务代码。
- 影响分析：GitNexus 对 `Search::components` 为 HIGH，已在修改前告知，并将变更限制在搜索组件暴露顺序；`PolicyRuntime` 为 MEDIUM；`begin_activity` 和 `select_components` 为 UNKNOWN，已通过文本调用关系核对，未将 UNKNOWN 当作低风险。
- 验证：重新构建可执行文件与真实原生动态库；PolicyRuntime 和 GepaStrategy C++ 测试通过。新增真实宿主测试证明通用 L2 规则能拦截空必填参数，且无 actions 白名单；旧显式 L2、无白名单 L3 loop、轮转变异隔离和自动契约原生/HTTP 回归均通过。

### CHG-20260916-276：GEPA 通用动作候选使用封闭原语 ID 并保留拒绝草稿

- 问题与诱因：首次真实 L3 搜索虽然正确提供唯一通用 action 原语，反思输出格式仍要求模型填写自由文本 operation/required_arguments。模型根据训练轨迹生成具体工具规则，被授权边界正确拒绝为 `control_operation_not_authorized`；同时草稿在 compose 成功后才落盘，错误节点只剩错误码，无法直接审计模型实际输出。
- 最小修复：无显式 `limits.actions` 时，模型只提交 `primitive_id=schema_required_nonempty`，组件内部将其规范化为 `* + $schema_required_nonempty`。未知原语、同时携带自由 operation/参数、具体工具规则仍拒绝；已有显式 action 规则和规范化旧格式保持兼容。动作空间仍来自包内能力与当次运行态 schema，不读取基线结果。
- 可审计性：通过隐私校验的反思草稿先原子落盘，再进行候选授权与组合；敏感输出仍在 `validate_local_data` 中先拒绝，不会落盘。单轮和 ComBEE map/reduce 使用同一顺序。
- 位置：自进化包 `control_generation.cpp`、`gepa_search.cpp`、两个热替换提示词及对应测试；不修改 AgentFlow 底座或 v1 业务代码。
- 影响分析：GitNexus 对 `apply_control_draft`、`control_generation_input`、`Search::run` 和 `run_gepa_search` 均返回 UNKNOWN；已用文本关系确认前两者只进入策略组合/候选重建，搜索入口只由后台 AgentEvaluation 调用，未将 UNKNOWN 当作低风险结论。
- 验证：重新构建 CLI、原生动态库与测试；PolicyRuntime、GepaStrategy、通用 L2 真实宿主、拒绝草稿审计、旧显式 L2、无白名单 L3 loop、ComBEE map/reduce 和敏感输出不落盘回归通过。

### CHG-20260916-277：L2/L3 候选必须以真实控制命中证明可执行

- 问题与诱因：候选可包含格式正确但运行时几乎不命中的动作／循环规则，搜索仍可能把经验注入带来的收益归给 L2/L3；Coordinator 的 `run_skills` 也未完整进入动作控制边界，导致“发布了控制规则但从未执行”的节点。
- 最小修复：运行策略保存动作与 loop 的明确匹配模式，通用 L2 原语只核验当前工具 schema 的非空必填字段；生成的 L3 规则只收紧 `no_progress`，不凭空改变宿主总轮次。Agent 评测与 GEPA 动态核验候选声明的控制覆盖，缺少真实 applied/host-boundary 回执时不允许把节点认作对应等级收益。v1 只在既有 `run_skills` 调用处加入统一动作打点；编排调用不计作业务工具次数。
- 位置：自进化包 `runtime_policy.proto`、`boundary_runtime.cpp`、`policy_runtime.cpp`、`control_generation.*`、`agent_evaluation.cpp`、`gepa_search.cpp`、`gepa_strategy.cpp`、提示词与测试；AgentFlow 仅改 `coordinator_decide.py` 的现有派发边界。
- 验证：PolicyRuntime、GepaStrategy、AgentEvaluation 及相关客户端／文档回归通过；覆盖通用 schema 拒绝、循环命中、未命中候选拒绝、真实宿主回执与多分支控制隔离。

### CHG-20260916-278：GEPA 精确恢复单个执行失败并稳定保留多父融合

- 问题与诱因：候选批次中一个 runner 瞬时非零退出会令整个节点 inconclusive，第二个互补分支因此消失，后续无法由多个叶／分支节点共同生成下一代；恢复时还可能按整批重复占用 trial 预算。
- 最小修复：只对明确的 runner 等待、读取、清理或非零退出错误补试一次；复用同批已完成 trial 的精确检查点，只执行缺失／不确定项。续跑预算按尚未解析的 trial 预留，不重复计算已复用试次；持续失败仍为 inconclusive，不伪造成功或放宽最终发布门槛。
- 位置：自进化包 `gepa_search.cpp`、`agent_evaluation_host.py`、`test_gepa_search.py`；不修改 AgentFlow 底座、评分标准或数据集。
- 验证：新增确定性“一次 runner 失败后恢复并参与双亲融合”回归；GepaSearchTests 全量 54 项通过（448.903 秒），包含精确缓存、失败隔离、多父共同生成和发布路径。

### CHG-20260916-279：常用语言客户端补齐调用、循环、缺口与隔离评测封装

- 问题与诱因：此前完整高层接口集中在 Python／JS，其他语言往往需要手工拼 before/after、loop guard 和评测入口，容易漏掉异常终态或重复执行业务副作用。
- 最小修复：Rust、Go、Java、C#、C 与 C++ 补齐同语义的 call、loop、capture-gap 和 evaluation 封装；业务函数最多执行一次，控制拒绝时不执行，业务异常保留原异常并附带 abort 回执。C/C++ 同时提供回调／本地 ABI 包装，C# 增加动态本地 ABI 客户端；HTTP 协议与已有字段不变。
- 位置：`tools/trajectory_evolution/clients/`、组件 CMake、语言契约测试与第 7、8 章文档；不修改 AgentFlow 底座。
- 验证：JS/TS 构建与契约、Go race、Java Maven、C# 真实本地 ABI、C++ EvolutionPoints 测试通过；CTest 的 LocalLanguageClientTests、IntegrationLanguageClientTests、EvolutionPointsTests 通过。Rust 源码接口已补齐，本机 Cargo 1.75 无法读取 edition-2024 lockfile，未将该环境限制记作功能通过。

### CHG-20260916-280：确定低延迟语义路由边界

- 问题与诱因：运行时仅靠词项重叠难以覆盖改写、多语言和否定条件；在主链部署编码模型、远程调用模型或让主 Agent 判断相关性又会增加依赖、时延和语义偏移。
- 方案：语义工作放到经验入库、修订和候选生成阶段，离线编译正向／负向特征、必需／排除事实及阈值；在线只做授权与公共／私有冲突检查、排除 veto、必需事实核验、稀疏倒排召回和带 margin 的确定性评分，不确定时 abstain。L2/L3 继续只由结构化谓词触发，不以文本相似度直接执行控制。
- 位置：`docs/15-policy-snapshots.md`；本条只确立可实现和可验收的设计契约，当前运行时词项算法未被伪装成新语义实现。
- 依据：Doc2Query、Doc2Query-- 与 SPLADE-doc 的离线文档扩展／稀疏检索思想；不引入本地模型服务、向量数据库或主流程远程模型调用。

### CHG-20260916-281：补齐 Rust／Go／Java 原生封装并修复并发关闭

- 问题与诱因：HTTP 高层接口已提供，但 Rust、Go、Java 的进程内入口仍要求接入者自己写 FFI；Java 与 C# 新封装的关闭可能和在途 ABI 调用重叠，造成已释放句柄被再次使用。
- 最小修复：Rust 增加无新增 crate 依赖的动态加载 LocalEvolution，Go 增加 Linux＋cgo 的运行时 dlopen 封装，Java 增加 JNA LocalEvolution；复用稳定 te_local_* ABI，提供打点、调用、循环、缺口、管理及生命周期方法。Java、C# 使用读写生命周期锁，Go 使用已有 RWMutex，保证关闭等待在途原生调用；业务回调最多执行一次。
- 协议边界：本地 manage 只处理 agent_evaluation.*；evaluation.next/fail 继续由 HTTP 隔离评测入口处理。未修改 AgentFlow 底座。
- 验证：Rust Cargo check／Clippy 和真实本地库契约通过，先前 Cargo 版本阻塞已解决；Go race、无 cgo 构建及真实库并发契约通过；Java 真实库生命周期／并发关闭测试 2 项通过。C# 指定匹配的 C++ 运行库后，真实本地库契约和 6 路调用与 Dispose 并发测试通过。八语言 HTTP L0–L3 联合测试验证 56 条完整轨迹。本轮原生验收平台为 Linux。
- 文档：第 6、7 章同步各语言原生入口、部署依赖和评测协议边界。

### CHG-20260916-282：GEPA 连续零应用提前结束与控制来源有界修正

- 问题与诱因：真实 v1 候选的生成规则在 6 道训练题及 6 道验证题中均有查询但零应用，原搜索仍等待完整验证结束；另一候选将基础 loop 的 rule_id 填进 memory_id，被正确拒绝后却只留下笼统错误码。
- 最小修复：启用评测缓存时，对训练中未应用的新规则先执行一个验证小批次。同一规则在两个阶段均有查询机会但零真实应用时，提前结束该候选并记录 unobserved_controls；验证才命中则继续完整验证，探测题通过现有缓存复用。评分不完整、没有查询机会或关闭缓存时不据此提前淘汰；相同 rule_id 仍继承父节点已建立的应用证据。
- 来源修正：保留 control_source_not_approved 等固定错误码。普通反思出现该来源错误且剩余模型预算与 max_attempts 允许时，最多增加一次紧凑修正请求，只发草稿、合法来源和约束，不重复发送轨迹，不重跑 Agent；持续失败仍拒绝，不自动猜测来源。提示词区分基础规则 ID 与经验 ID，以及跨执行实例的 waste 与单实例 no_progress。
- 位置：自进化包 GEPA 搜索实现（src/gepa_search.cpp）、gepa-reflection.txt、现有测试夹具与 GEPA 指南；不修改 AgentFlow C++ 底座、v1 执行流程、数据集、评分标准或发布门槛。
- 影响分析：按 GitNexus 要求检查 Search::run/validate/error/mutate/packet、run_gepa_search 与测试入口；图谱返回 UNKNOWN，已进一步核对文本调用关系，确认影响在后台搜索、融合验证及其测试，未将空调用集当作安全结论。
- 验证：GEPA 全量 60 项通过（476.56 秒），后台 AgentEvaluation 全量 21 项通过（72.82 秒）；补齐评分不完整保护后重新构建，连续零应用、验证命中与缓存不重跑、紧凑来源修正、重试／预算边界及未知 loop 缓存共 7 项通过（42.665 秒）。

### CHG-20260916-283：反思区分可干预动作边界并优先低收益训练证据

- 问题与诱因：真实候选完成 4 题，共 17 次动作查询但零触发。模型试图用动作规则拦截空派发，而 v1 在到达动作打点前已做同类校验；反思摘要省略了 semantic 与真实 action 查询关联，使模型将输出中的调用计划误当成 L2 介入位置。另有成功但含 4 轮浪费的训练题，因按原顺序仅取前 4 条反馈而被排除。
- 最小修复：反思动作摘要保留 semantic，并只以同一条轨迹中匹配 action_id 的真实 policy.query 附加 action_control 摘要；没有证据则为 null，不复制完整查询或模型历史。反馈先失败优先，再按现有 fitness 从低到高选入字节／题数预算；评分函数、原始轨迹和发布门槛不变。提示词明确模型计划、宿主拒绝与实际控制边界的区别，不把重复现有校验说成新增收益。
- 位置：包内 src/gepa_search.cpp、gepa-reflection.txt、现有 GEPA 测试及指南；不移动 v1 校验、不改变动作权限、不修改 AgentFlow 底座。
- 影响分析：GitNexus 对 Search::packet、feedback_trial 返回 UNKNOWN；文本调用核对确认 feedback_trial 只由训练反思组包使用，不进入主 Agent 路径或裁判原始日志。
- 验证：重新构建成功；高浪费成功题优先、模型计划与真实动作查询分离、连续零应用、验证才命中、ComBEE 和多父融合共 6 项通过（46.236 秒）；最终 GEPA 全量 61 项通过（474.67 秒），后台 AgentEvaluation 全量 21 项通过（71.75 秒）。

### CHG-20260916-284：反思保留循环控制的真实连续无进展计数

- 问题与诱因：真实 v1 的 L3 候选在 4 道已完成题目中经过 32 次循环查询，却全部继续执行，没有新规则应用。反思将语义上的重复请求和离线 waste 当成连续状态不变，提出把 max_no_progress 从 3 收紧到 2；实际这些循环的状态指纹持续变化。原摘要只保留 loop.decision，没有与该步骤匹配的运行时计数证据。
- 最小修复：仅在后台反思视图中，通过同一轨迹内的 step_id 为 loop.decision 关联 loop_control，保留查询来源、scope、判定前 observed_no_progress、在途动作标志和策略建议；宿主实际 decision 仍单独保留。计数在 replan 清零前计算，缺失、未启用或有在途动作时不伪造为零，不传送状态全文或指纹。提示词明确实际计数为零时不能声称收紧阈值会命中。
- 边界：不修改 v1 打点、AgentFlow 底座、运行时指纹与计数规则、动作空间、评分或发布门槛；不以强制触发制造收益。按连续未生效约定取消该轮，清理实验结果时保留数据集、公共/私有 memory 和冻结种子。
- 位置：包内 src/gepa_search.cpp、gepa-reflection.txt、test_gepa_search.py 与第 24 章指南。
- 影响分析：AgentFlow 当前工作区的 GitNexus 对 feedback_trial 和 GepaSearchTests 返回 UNKNOWN；文本调用复核确认摘要仅由 Search::packet 送入后台反思，不进入主 Agent 或裁判原始轨迹，未以索引空结果作安全结论。
- 验证：CLI 与原生动态库重新构建成功；新增 3 项分别验证评审 waste 不等于运行时计数、真实重复计数保留，以及 L1 未启用计数保持未知。连同 L2 实际控制、零应用提前结束、验证才命中且不重跑、多父融合与 ComBEE 共 8 项定向回归通过（23.121 秒 + 39.566 秒）。

### CHG-20260916-285：补齐 L1 改写应用门禁并保留训练生效证据

- 问题与诱因：覆盖检查原来只验证生成的 L2/L3 规则，L1 改写即使未进入实际模型请求也可能被当成有效候选；另有规则在训练中实际应用，完整验证却只合并父节点证据，丢失本候选的训练命中，造成误判未生效。
- 最小修复：复用既有 control_coverage，为 L1 改写增加 llm 覆盖项，并以完整改写内容及来源生成标识。只有对应查询和真实模型请求回执一致才计 applied，旧文本证据不能批准新文本。完整验证合并本候选训练和未改变的父节点证据；提前零应用判定仍分别检查训练与验证查询，评分和最终发布收益门槛不变。
- 边界：不要求每条经验在每题命中，不为未改写或主动禁用经验增加注入要求；不修改运行时匹配、主流程模型调用、v1 或 AgentFlow C++ 底座。该修复证明是否使用，不能将已使用直接等同于有效改善。
- 位置：包内 src/control_generation.cpp、src/gepa_search.cpp、两个本地测试夹具和第 24 章指南。
- 影响分析：AgentFlow 当前工作区的 GitNexus 对 control_coverage、Search::validate 与测试入口返回 UNKNOWN；文本复核确认影响后台 GEPA 候选验证与 AgentEvaluation 发布门禁，未将索引空结果作为安全结论。
- 验证：新增“L1 未注入拒绝”“不同改写不能共用应用证据”“仅训练命中仍保留证据”3 项，连同验证才命中、经验禁用再启用、多父融合共 6 项定向测试通过（43.132 秒）。最终 GepaSearchTests 全量 67 项通过（511.38 秒），AgentEvaluationTests 全量 21 项通过（72.84 秒）；共 10 组 CTest 回归全部通过，覆盖原生/HTTP、L0–L3、并发回执与边界控制。另运行真实 v1 函数的本地夹具测试，通过双 Worker、实际注入内容、动作拒绝、停止出口及采集器不可达检查（build/trajectory-evolution/v1-points.0mzj4cw0/test-summary.json）；模型和 DataBus 在该测试中为本地夹具，不将其作为付费真实实验成绩。

### CHG-20260916-286：GEPA 越界组件紧凑修正与单题超时恢复

- 问题与诱因：真实搜索的 action_rules 和两个经验轮次选择正确，但反思模型三次改写了未获选经验，被 `gepa_component_not_authorized` 正确拒绝；唯一 loop_rules 轮次因训练批次的一道题达到 runner 超时而在反思前结束。搜索因此只剩 L1 候选，留出集虽将通过率由 86.7% 提至 93.3%，平均无效 loop 却由 0.333 增至 0.733，时延由 48.56 秒增至 52.57 秒，发布门禁以 `max_latency_ratio` 和 `loop_target_not_met` 拒绝。
- 最小修复：保留组件授权校验。出现 `gepa_component_not_authorized` 时最多追加一次紧凑模型修正，只发送原草稿、获选组件当前值、合法来源与控制约束，不重发训练轨迹、不由程序删改草稿。将 `runner_timeout` 纳入已有的一次性检查点恢复，只补跑未完成试次；持续超时仍为不完整评测。
- 边界：修正计入现有模型调用预算并受 `control_learning.max_attempts` 限制；第二份草稿仍经过全部授权、隐私和策略兼容校验。训练、验证、留出划分、评分目标、最终发布门槛、v1 与 AgentFlow C++ 底座均未修改。
- 位置：包内 src/gepa_search.cpp、gepa-reflection.txt、本地 GEPA/runner 测试夹具与第 24 章指南。
- 影响分析：GitNexus 对 Search::mutate、Search::evaluate 及短符号查询返回 UNKNOWN；文本调用复核确认修改只进入后台 GEPA 反思与隔离评测恢复路径，未将索引空调用集视为低风险。
- 验证：组件越界修正、runner 超时单题恢复、原有非零退出恢复、非法控制来源紧凑修正共 4 项定向回归通过（41.348 秒）。完整回归与真实复启结果待本轮结束后补充。

### CHG-20260917-287：按真实控制机会生成候选，统一搜索目标与有界失败恢复

- 问题与诱因：旧训练记录中 408 次 loop 查询的连续无进展计数均为 0，单纯收紧该阈值没有触发条件。任意批次与组件轮换不匹配、每个父节点重新起算组件进度，以及质量优先排序与最终质量／无效 loop／时延三目标不一致，使搜索长期偏向 L1；未使用经验和公共／私有重复来源还占据生成机会。
- 修改：全树交错轮换组件；仅从训练执行记录建立机会索引，为对应控制组件保留训练样本。反思只接收本轮允许修改的组件，控制证据优先；生成后通过实际查询做差异触发预检，再启动真实候选并核验应用回执。没有依据不伪造规则或强行触发。公共／私有重复来源合为一个搜索组件，支持停用与恢复经验正文。
- L3：增加默认关闭的 replan_on_failure，使用同一执行分支明确的结构化工具失败触发一次有界重规划；遵守原 replan 预算和在途动作屏障，不因失败直接停止兄弟分支。稳定状态计数保持原义，不以离线 waste 或失败数冒充 no_progress。字段为 Protobuf 增量，旧策略默认行为不变。
- 目标：新增同验证集固定 L0 参考与节点 optimization，选择时优先满足质量和成本约束，再比较 loop 目标差距和逐题稳健性；仍保留互补分支，留出集不前移、不进入反思。运行语义加入实验指纹，旧评测缓存不能跨语义复用。
- 位置：全部在 tools/trajectory_evolution 包内的 GEPA、策略运行时、边界采集、控制生成、协议、外置提示词及测试／文档；本条未修改 v1 业务代码或 AgentFlow C++ 底座。
- 影响分析：相关 GitNexus 符号返回 UNKNOWN；fingerprint 同名查询包含另一个文件的 HIGH 候选，按 agent_evaluation.cpp 限定后为未索引。已核对具体文本调用链：后台评测构造／冻结校验，控制生成／编译及共享原生／HTTP 边界；不将未解析结果视为低风险。
- 验证：PolicyRuntime、IntegrationService、GepaStrategy 三组通过；6 项定向 GEPA 测试通过（45.755 秒）。GEPA 全量 71 项通过（613.063 秒），覆盖可触发 L3 生成与实际请求应用、零触发预检、紧凑修正、缓存、裁判恢复和多父融合。初次全量在第 70 个用例附近碰到整组 600 秒上限，CTest 组时限调整为 900 秒，Agent／模型超时不变。后台 AgentEvaluation 21 项通过，语义指纹更新后另复测 L0–L3 发布与重新载入通过；原生／HTTP 多语言、异步点位、任务评测和并发边界回归通过。真实 v1 新作业 ef4908b275d61dd3f8e7d6bd2a2b3a86793325dbb75f0893d9a4ee3588f95d86 已启动；最终效果仍待独立对照，不把夹具成绩当作真实收益。

### CHG-20260917-288：压缩后的 GEPA 反馈不再被当作完整动作轨迹读取

- 问题与诱因：真实 v1 首轮训练完成后，在反思模型调用前出现节点错误。反思输入达到字节预算时，部分反馈会缩为判分、指标和输出摘要；后续控制机会检查仍直接遍历其已省略的 runs/actions 数组。旧测试轨迹较短，没有覆盖此组合。
- 最小修复：control_evidence 在遍历前检查数组是否存在；摘要仍供模型参考，但不作为 L2/L3 可执行触发依据。输入预算、评分、策略运行语义和发布门槛均保持不变，不修改 AgentFlow 底座。
- 位置：包内 src/gepa_search.cpp、tests/test_gepa_search.py。影响图查询为 UNKNOWN，已补查 packet、remember_opportunities 和 Search::run 的调用点，修改只影响后台搜索。
- 验证：新增 20 个追问、8 KiB 输入预算的本地回归，修复前复现同样的反思前节点错误；修复后长反馈、无证据回退、L3 实际应用及零触发预检共 4 项通过（33.871 秒）。全量 GepaSearchTests 通过（619.74 秒）。真实作业经主动停止后以 attempt=2 从同一检查点续跑；已完成 L0/种子各 15 题和首批训练数据不受本次解析修复影响，不混入不同执行语义的缓存。36 条已完成真实轨迹复核未见缺失因果引用、因果环或未闭合活动：662 对 step、379 对 action、36 对 run 均闭合；主动取消中的试次保留取消记录，不算作业务失败。

### CHG-20260917-289：检查点续跑次数与自动故障恢复预算分开

- 问题与诱因：真实多轮训练题 t-14 已完成 23 次模型调用并采集轨迹，但裁判返回 invalid_json。该批次此前人工取消过，续跑已经进入 attempt=2；原代码只在 attempt=1 时自动恢复，导致尚未使用自动补试的批次直接变成不可评分，丢失一轮生成机会。
- 最小修复：在既有 GEPA 评测检查点中持久化 automatic_recovery_used，只有实际启动自动恢复时才消耗一次预算。人工取消、限流后的显式续跑不占用它；持续失败仍停止自动恢复。已完成且需重评的 Agent 执行继续复用原轨迹、trial_id、时延和指标，不放宽评分门槛。
- 位置：包内 src/gepa_search.cpp 与 tests/test_gepa_search.py；AgentFlow 底座、主流程、模型配置及数据集不变。GitNexus 对 evaluate 返回 LOW，直接调用为 Search::run、validate、merge，覆盖两个后台流程；已结合当前源码核对。
- 验证：新增“限流后手动续跑，再遇一次裁判故障”的本地测试；旧二进制复现 attempt=2 直接不可评分。修复后自动补试、持续失败、invalid_json、缓存与限流共 6 项回归通过（36.105 秒），并逐项核验原 trial_id、时延、指标和轨迹保持不变。GepaSearchTests 全量 73 项通过（628.757 秒）。真实作业已以 attempt=3 续跑第 3 个 L3 节点。
- 真实进展：节点 3 只修改 loop_rules，反思输入 57,469 字节，包含完整控制证据与两条压缩摘要，已验证 CHG-288 不再中断该路径。新失败恢复规则在训练查询预检中有 4 个差异触发点；真实 t-10、t-11 各出现一次 replan 应用回执，并核验提示进入实际 model_call 输入。t-11 无效 loop 从 1 降至 0，但当前部分样本整体耗时及无效 loop 尚未优于父策略，不能据此宣称达标或发布。
- 节点 3 完整训练筛选：同 6 题父子通过率均为 5/6，无效 loop 均值 0.667→0.500（下降 25%），Agent 耗时均值 58.55→69.16 秒（增加 18.1%）；生成的 L3 规则实际应用 3 次。节点仅为 screened，保留局部改进供后续验证／融合，不视作通过最终发布门槛；相对 L0 的完整验证尚未进行。

### CHG-20260917-290：L2 增加真实技能编排，参数护栏单独保留

- 问题：原 L2 只有逐动作 allow/reject 和参数约束，无法表达“先 A 完成再启动 B”，不符合技能编排目标。旧真实搜索已停止，未发布其节点。
- 修改：包内新增 schedule_rules、可序列化的依赖状态及 ready/observe 协议。保留宿主硬依赖；无依赖分支并行；成功依赖失败只跳过其后继。重复、乱序、环及冲突单独校验。协议增量兼容，旧策略不增加依赖。
- GEPA：增加独立 schedule_rules 组件及多父重组；反思仅看实际批次的实例、操作和依赖，不重复携带 prompt。生成后预检依赖图是否改变，正式应用核验实际开始/结束顺序和终态，计划或 ACK 不算 applied。训练机会与运行能力不来自技能白名单；留出集不前移。
- 接入：新增共享 C++ dispatch 点，Python HTTP/原生提供 dispatch/resume_dispatch。包持有异步分批状态、领取防重与结果合并；v1 仅在现有派发及整批完成处接入包装，物理批次仍由原 DataBus/handle_run_skills_call 调度。中间 wave 不增加模型决策，也不提前替换 run_skills 的历史 ACK。未修改 AgentFlow C++ 底座。
- 边界：批量完成型宿主按整批释放下一 wave；不是逐分支立即唤醒。采集服务持有活动计划，不能中途移除。观察故障不重试已提交业务副作用；数据缺口不计为有效策略应用。
- 影响：GitNexus 对 PointCalls 返回 CRITICAL，主要影响原生/HTTP 两个客户端；ConfiguredPoints 的直接引用包括 coordinator_decide、llm_call 和 skill_executor。新增 C++ 入口及部分同名符号为 UNKNOWN，已核对共享边界调用链并补跨进程、重复通知与失败隔离测试，不将空调用集视为无风险。
- 快照升级：增加 runtime recompile，验证原策略后复用其经验快照、重新编译新增宿主能力并生成新 policy_id；避免配置新增 schedule 而冻结种子仍无编排能力。原记忆内容、授权校验及旧编译命令不变。
- 验证：GEPA 全量 74 项、后台 AgentEvaluation 全量 21 项通过；PolicyRuntime、IntegrationService、GepaStrategy、原生/HTTP 客户端契约及新增 DispatchClient 回归通过。v1 全量 584 项通过、4 项跳过；真实 DataBus manifest 函数的编排测试覆盖先派发准备/独立分支、后派发图片、归并为一个 tool 结果。重新编译覆盖旧策略完整性、相同经验、新能力、新 ID 及严格能力不足拒绝；评测版本断言同步增加 skill-schedule-v1，旧缓存不能充当新执行语义的结果。本条不代表真实实验收益已达标。

### CHG-20260917-291：跳过等价候选，补齐控制信号与分级实际应用验收

- 现象：真实实验中，空反思将显式默认字段改为省略字段，同一 policy_id 被识别为新 Agent 版本并重跑；L2 忽略同名技能实例，L3 的动态消息字段遮蔽工具重复；空生成规则的 complete 被误解为 L1–L3 全部生效。已停止真实搜索，数据集和记忆库不变。
- 判重：空变更直接保留父策略；搜索按解析后的授权组件比较等价性，记录 duplicate/no_behavior_change，跳过候选执行。不修改已有导出包的版本标识算法，移除规则和经验停用仍可评测。
- L2：按实例识别编排机会，同名操作可按批次原顺序有序执行；保留宿主依赖、冲突回退和独立失败隔离。多个实例不构成串行收益证明，不强制生成规则。
- L3：在包内为每个执行实例记录有界工具输入／输出哈希，补充 repeated_action_steps。新结果、分支隔离、在途屏障、重规划预算、容量回退均保留；不删除业务字段、不调用主流程模型、不将评审 waste 当作运行计数。原状态判定保留，执行语义更新纳入评测指纹。
- 验收：在既有逐规则覆盖外增加分能力 queries/triggered/applied 和 level_complete。GEPA 默认要求目标等级已声明能力的真实应用；L2 有 schedule 能力时必须观察技能编排，不能用参数拦截代替。仅 L1 的 L3 候选可以作为中间分支，但不能通过完整 L3 发布。固定候选评测可显式开启同一 gate.require_level_coverage 检查。
- 全面测试同步修正旧测试入口：异步示例通过 allSettled 等待并发分支，不因单分支业务异常提前退出；v1 跨进程夹具改用现有 ALL 批次完成通知，新增未经 mock 的策略引擎 replan/stop 注入验证。模型、工具和 DataBus 传输仍为确定性测试夹具，不作为真实模型收益成绩。
- 影响范围：自进化包运行时、搜索、验证、测试和文档；未修改 AgentFlow C++ 底座或本轮 v1 生产代码。GitNexus 定向分析为 UNKNOWN，未将空调用集视为低风险；源码复核覆盖 runtime→boundary→原生/HTTP、search→candidate→发布和客户端回执链路。
- 验证：自进化包 34 组 CTest 全部通过，其中 GEPA 搜索 78 项、AgentEvaluation 21 项；v1 回归 584 通过、4 跳过。补充的在途动作计数保留、257 个不同结果后的容量回退测试通过。真实 GEPA 长实验保持停止。
- v1 实际函数跨进程验证：4 条轨迹全部通过 doctor，记录 L1 注入回执 24 次、动作拦截 1 次、技能编排 1 次、L3 回执 3 次（2 次 replan、1 次 stop）；准备/独立分支先派发，图片下一批派发，中间不增加 Coordinator 模型调用。采集不可用时保留原业务执行，测试凭据未进入轨迹。模型、工具和 DataBus 传输为夹具，不计作真实 API 收益。
- 发布链路验证：GEPA 生成同时含技能编排和失败重规划的 L3 候选，经评测发布后，在普通新 run、仅复制一个策略包的空数据目录、服务重启后的新 run 分别核验 L1/L2/L3 实际执行回执；均通过。不读取旧记忆、不调用反思模型、不依赖旧评测运行态。
- 真实实验诊断（停止任务 6470021…）：去重后的 46 条逐题记录中，191 次 loop 查询均为 continue、no_progress 全为 0，单执行实例最多第 5 轮；种子仍是 max_iterations=12、max_no_progress=3、replan_on_failure=false，14 次失败信号查询未触发控制。该次搜索尚未生成 loop 子策略；空编排提案被当成不同序列化版本重跑亦消耗了预算。
- 尚需区分的能力边界：语义上的换参数重复失败不等于相同输入／输出；当前生成器禁止无结构条件地全局降低 max_iterations，尚无针对同类不可重试错误、未恢复前置条件的专门终止谓词。现有执行链回归通过不等于这类无效轮次已经可优化；部分模型评审把必要的首次上游报告计为 waste，也需核对，不能直接当作终止依据。

### CHG-20260917-292：激进控制、重复失败信号与并发评测

- 问题：参数／报错变化使重复失败始终被算作新进展；循环生成范围只允许稳定状态阈值，旧来源规则还可能受 L1 检索影响。真实评测固定服务端口也限制了题目并发。
- 修改：增加持久化的 `runtime_options.aggressive_controls`，默认关闭。开启后，已批准控制来源不受 L1 词面／top-k 门槛限制；同一执行分支的结构化重复操作失败进入 `repeated_failure_steps`，补充现有状态和工具哈希计数。首次失败、成功重置、在途屏障、分支隔离及有界存储均保留。
- 搜索：激进模式允许在授权上下限内降低循环轮次预算；反馈提供实际 iteration 和重复失败计数，提示词同步说明触发口径。修正新增 iteration 读取按 Protobuf uint64 JSON 字符串解析，避免反思前类型错误。普通模式保持原限制，质量与实际应用发布门槛不变。
- 并发：复用包内已有评测线程池，真实 v1 试验改为 4 题并发，每题独立工作目录、数据库、daemon/Bridge 端口；未新增宿主启动器，未修改 AgentFlow C++ 底座。并发减少批量等待，不承诺单次模型响应变快。
- 影响分析：feedback_trial 的未限定查询返回 CRITICAL，限定源码文件后为 UNKNOWN；源码核对调用点为 GEPA 反馈组装与可执行证据预检。用生成、评测、发布、导出重载回归覆盖，不将空图结果视为安全证明。
- 验证：34 组 CTest 全部通过（完整 GEPA 搜索组耗时 666 秒）；3 项重点 GEPA 回归也单独通过，覆盖激进预算生成、普通模式兼容和 L1/L2/L3 发布后实际应用及单文件离线重载；v1 实际函数跨进程打点回归通过。测试模型／工具为夹具，真实 API 的收益以新 GEPA 结果为准。
- 首轮真实搜索修正：激进基线使用 stop 且没有 replan 预算时，候选输入仍暴露 replan_on_failure，导致 L3 节点报 control_failure_replan_not_authorized。现在 loop_primitives 由基础规则逐项生成：始终按证据开放无进展阈值，激进模式开放轮次上限，只有基础规则确实允许有界重规划时才开放失败重规划。模型输入与编译授权保持一致，避免不可执行能力占用搜索轮次。
- 真实实验基线恢复组件默认的一次有界 replan 预算：无进展首次达到阈值时注入重规划上下文，预算用尽后停止；GEPA 也可基于真实失败证据启用 replan_on_failure。该设置写入种子策略并随导出包持久化。

### CHG-20260917-293：候选重组保留独立控制来源的授权

- 问题：控制来源与 L1 可注入经验分离后，Search::compose 清空 approved_experiences 并仅按 L1 组件恢复授权，导致合法 L2/L3 规则引用控制来源时出现 control_source_not_approved；经验变异与多父重组都经过此路径。
- 最小修复：从冻结种子恢复控制来源原有的 memory_id 与 content_revision 授权，不将控制证据变成 L1 注入内容，不增加未批准来源。只修改自进化包的候选重组与回归测试，AgentFlow 底座不变。
- 影响分析：AgentFlow 仓库中 Search::compose 的图分析为 LOW，直接调用为 mutate、merge，共涉及 5 个符号；测试入口图结果 UNKNOWN，已补查 unittest/CMake 调用点。
- 验证：新增独立控制来源回归在旧二进制复现 control_source_not_approved；修复后该回归、L1/L2/L3 实际应用与发布、多父融合断点恢复、停用经验恢复共 4 项通过（39.587 秒）。可执行文件和原生库已重新构建。
- 真实实验仍暂停：种子验证 15/15 完成，L1 108 次查询中 97 次注入不代表相关性合格。逐条复核及本地检索重放发现，天气结果的 status/statu/tool 触发图片经验，user 角色中的 system-reminder 触发 Coordinator 经验，重叠中文子串也会误触发。本条只修复控制授权丢失，尚未修复上述 L1 误注入，不据此恢复实验或宣称可发布。

### CHG-20260917-294：L1 检索排除结构噪声与重复匹配证据

- 问题：JSON 字段名、user 消息里的 system-reminder、英文原词/词干及重叠中文子串被重复计为适用依据；工具标识被拆词后，无关天气、预算、CSV 请求也能触发图片经验。单独的 bash/file 或“用户需求/覆盖”又会误触发特定报错、完成条件。
- 最小修复：在自进化包内共用观察文本投影，只检索消息内容、实际调用及结果，不检索传输字段名、system/developer 指令、工具定义或既有注入块。先清洗再限长，保留原始轨迹和实际模型输入；截断的结构输入不回退到字段名匹配。
- 匹配：英文常见词尾只保留一个归一词项，带下划线的标识不拆分；条件中的明确标识需要对应观察，通用结果标签不能替代。中文优先较长、不重叠片段，短片段只补充相邻具体内容，不能仅靠拆出一小段通用描述过门槛；带引号的英文多词错误文本须完整出现，其单词才参与匹配。没有新增在线模型调用、编码器、向量库或宿主配置。
- 保持不变：私有/公共授权、去重、top-K、字节预算及 L2/L3 独立控制来源仍走原路径；未改 AgentFlow C++ 底座或 v1 打点。评测语义指纹增加 retrieval-evidence-v2，防止旧匹配逻辑的成绩混入新实验。
- 影响分析：AgentFlow 工作区中的 terms 和 bounded_query_text 图分析为 HIGH，已在修改前提示共享检索链风险；ShadowRuntime::query、新增投影和测试入口的图结果 UNKNOWN，另行核对 query_advice、PolicyRuntime、HTTP/原生调用和测试注册，不把空调用图当作无影响。
- 旧真实轨迹本地重放：同一批 89 次完整检索请求，命中请求由 78 降至 31，返回经验条次由 166 降至 41；素材筛选/预算核算误命中图片经验由 8 降至 0，缺首帧误命中写文件经验由 1 降至 0。19 次已经截断的旧请求未纳入比较。这里只重放检索，没有重跑模型，不视作新的 Agent 质量、时延或 GEPA 收益结果，也不把所有剩余命中自动判为正确。
- 回归补充：负向用例在修复前复现，覆盖 JSON、提醒、既有注入、同名片段、特定报错及截断结构；正向用例保留中英文、实际调用/错误、长输入中的末尾观察和并行隔离。公共记忆夹具按“每份来源两次审查 + 非精确关系审查”逐项核对，继续检查重启不重复调用；Java 原生夹具等待后台归档再关闭，未将生产打点改为等待评测。
- 验证：GEPA 全量 80 项通过（732.64 秒），其余 33 组 CTest 复测全部通过（119.64 秒）；Java 原生客户端组另连续 3 次通过。最后补保留具名 tool/function 返回中的实际操作名、排除 user 昵称后，9 组检索/权限/两库/HTTP/原生回归全部通过（19.55 秒），4 项 GEPA 应用、授权、多父恢复及经验恢复回归全部通过（39.174 秒）。
- v1 实际函数跨进程复测：4 条轨迹全部通过 doctor，L1 注入回执 24 次、L2 动作控制 1 次、L2 技能编排 1 次、L3 循环控制 3 次；模型、工具及 DataBus 传输为夹具，不计作真实 API 收益。结果见 build/trajectory-evolution/v1-points.dowcj6rj/test-summary.json。
- 真实 GEPA 保持暂停；数据集、记忆库和原始实验轨迹未改写。下一次真实对照需要创建新作业，不能把旧成绩作为修复后的结果。

### CHG-20260917-295：收窄误匹配，并让 GEPA 同时进化经验适用条件

- 问题：新一轮真实 v1 验证仍把预算报告中的“执行成功”和另一段“可进一步 generate_image”拼成图片结果经验；缺图片素材场景靠“的文件、目录不存在”触发写文件恢复。另发现 GEPA 只能修改正文，无法修补错误的适用范围，因而暂停该作业后修复。
- 检索最小修复：明确标识的支持词限于该标识所在观察段落；中文虚词边界窗口不计作独立具体词项。保留真实结构化操作/结果、中英文正向匹配，不增加模型调用或分词模型。
- 版本内条件：`ExperienceOverride` 新增可选 `trigger_pattern`、`observed_operations`；旧正文-only 格式继续有效。GEPA 的改写、停用恢复、组件融合和导出保留这些条件，来源记忆、scope、授权与原始快照保持不变。空条件恢复原条件，空操作列表解除门槛；省略则继承父版本。
- 执行证据：包复用原有打点祖先、loop/stream 关联，从实际结束且执行过的 action 收集本分支操作名。模型成功处理一批观察后消费已有证据，在途新结果保留、失败请求不消费；不采信计划、自然语言提及、未执行动作或并发兄弟分支。匿名活动不推测共享分支；缓存有界且清理空分支。
- 隔离：上述适用条件仅用于 L1，不能削弱 L2/L3 控制；私有版本被改写后，公共重复项不能回退注入旧条件/正文。`RuntimeQuery.observed_operations` 及条件随 Protobuf/版本持久化。反思反馈补充精简的经验查询与实际应用状态，强调任务要求的验收步骤不可被“成功后免重试”经验省略。
- 未修改 AgentFlow C++ 底座或 v1 打点，不要求宿主新写适配器。评测指纹升级为 retrieval-evidence-v3，旧语义成绩不能用于新作业。
- 影响分析：同一 AgentFlow 工作区的 compile_runtime_policy 为 CRITICAL（17 个可解析上游符号），修改前已提示；覆盖 compose/start_run/make_run/回执验证与在线加载。检索和动态事件入口出现 UNKNOWN，补查实际调用和测试注册，未将零边视为无影响。
- 真实请求本地重放：95 次请求，无错误；经验条次 63→51，命中请求 44→38。预算误触发图片 1→0，缺素材误触发写文件 2→0，不生成视频场景误触发图片 1→0。这里未重跑模型，不视为质量/时延收益，不保证其余词面命中均适用。
- 验证：33 组非 GEPA 回归全部通过（75.42 秒）；条件继承回归通过（6.848 秒），已有经验恢复、多父中断恢复和 L1–L3 发布回执重点回归通过。新测试曾错误假设第二节点必然以第一个候选为父，修正为明确 current_best 和匹配夹具原条件后验证真实继承，未放宽生产验收。
- v1 实际函数跨进程夹具：4 条轨迹通过 doctor，L1=24、L2 动作=1、L2 编排=1、L3=3 次真实回执；无额外宿主修改，模型和 DataBus 使用夹具，不计作真实 API 收益。报告：build/trajectory-evolution/v1-points.fs4is1yq/test-summary.json。24 次模型查询中，12 次具有本分支真实动作证据。
- 完整验证：GEPA 全量 81 项通过（748.867 秒），补充的操作证据门槛回归另行通过（5.165 秒）；公共重复项绕过与文档回归均通过。原始条件、可选新条件及无条件的旧策略均保留覆盖。
- 真实复跑：删除已取消 b0c75142 作业的约 77 MB 旧结果及状态，无备份；数据集、私有 memory.pb、种子策略 SHA-256 前后相同。修复后服务以 tmux evolution-gepa-r5 持久运行，沿用原 60 题（train30/val15/holdout15）、4 并发与发布门槛，新建作业，不复用旧成绩。真实质量/时延收益另按新作业结果判断。

### CHG-20260917-296：修复跨进程沙箱串用与历史结果重复触发经验

- 真实问题：作业 3dd68bca 的种子完成 15 题，13 题通过，平均 69.96 秒、总 loop 7.07、无效 loop 1.00。发现多个真实工具请求访问了已消失的 8080 认证文件，故取消本轮，不将尚未完成的 L0 或取消题计入对照收益。
- 沙箱原因与修复：LocalSandbox 的启动锁只在单进程内生效，探测空闲端口后先关闭 socket 再由 Python 绑定，4 个 daemon 可同时选中同一端口；就绪探针又会接受兄弟 server 的响应。改为保留已绑定 socket 并传递给本次子进程，启动失败回收子进程。仅改 src/sandbox/sandbox_local.cpp 和 sandbox_http_server.py 的本地启动路径，Docker/E2B 原启动参数不变，不放宽认证或网络边界。
- 复现与验证：新增 4 进程延迟启动回归，旧代码返回 1 个共同 endpoint，修复后 4 个独立 endpoint 且各自读写成功；同进程/跨进程隔离连续 5 轮通过。沙箱生命周期 14 项通过、1 项原有跳过；Python 认证/HTTP/沙箱 63 项通过。
- 过度注入原因与修复：自动消息检索反复消费旧工具结果，即使当前分支已转入其他动作。包内 activity_query_state 只使用最新用户请求及最新 assistant/工具结果片段，同批并行结果全部保留，新发生的相同失败仍能命中。显式 state、实际模型请求、原始轨迹、L2/L3 控制逻辑保持不变。执行语义指纹升级 retrieval-evidence-v4，防止混用旧评测缓存。
- 检索验证：原 106 次真实请求本地重放，旧检索命中与记录完全一致；命中请求 46→25、经验条次 67→36，中文图片长任务 13/15→3/15。这里只重放查询，不代表真实 Agent 质量、时延或收益已达标，也不把全部剩余词面命中视为语义正确。
- 回归：33 组非 GEPA CTest 全通过（119.11 秒），条件继承、真实操作证据、完整 L3 发布及多父融合 4 项 GEPA 回归通过（37.624 秒）。v1 跨进程夹具通过，报告 build/trajectory-evolution/v1-points.s93s6hbx/test-summary.json。夹具曾以 20 ms 时间窗口假设线程重叠，在负载下误失败，改用同步屏障证明并发，不放宽原顺序/回执断言。
- 影响分析：AgentFlow 工作区索引为 6d47af6（09:12），结合当前源码核对；launch_http_server 的直接调用为构造和 resume，activity_query_state 为 begin_activity，fingerprint 为评测初始化/后台循环，图分析均 LOW。测试入口为 UNKNOWN，补核主函数及 CMake 注册，不把空调用图当作无影响。
- 真实重启：已删除取消作业 3dd68bca 的约 76 MB 结果和旧状态，无备份；数据集、memory.pb、种子 SHA-256 未变。tmux evolution-gepa-r6 持久运行，新作业 61579880 从头评测，原 60 题、4 并发、模型与发布门槛不变。首批活跃 daemon 的沙箱 endpoint 各自独立；正式收益待本轮 L0/候选对照，不复用被环境问题污染的旧成绩。

### CHG-20260917-297：修复 GEPA 改错组件后的训练证据丢失

- 真实问题：作业 61579880 的首个编排提案改写了未获选的 L1 经验；修正请求没有原训练反馈，模型因此以缺少 schedule_plans 为由返回空提案。原请求实际包含编排批次，不是宿主未采集。发现后取消作业，不把取消题计入效果比较。
- 最小修复：仅调整包内 GEPA 组件修正路径，复用一条原训练反馈，优先保留获选组件的可执行证据及对应任务约束。不重传越界草稿和整批轨迹，不加入验证／留出内容。来源 ID 修正仍不带反馈；输入上限、单次修正、调用预算及实际应用门槛不变。提示词补充 schedule_rules 的修正格式，未修改 AgentFlow 执行代码。
- 影响分析：AgentFlow 工作区 6d47af6 索引中，Search.run 和 packet 图结果 LOW，上游为后台评测循环；run 存在未解析动态调用，结合 run_gepa_search 实际调用点核对。测试 HTTP 回调与 unittest 入口为 UNKNOWN，补查服务器注册和测试入口。
- 验证：新增编排修正用例在原代码复现“零条反馈”；修复后 9 项重点 GEPA 回归全部通过（78.739 秒），覆盖 8 KiB 输入预算、训练证据来源、验证／留出隔离、来源修正预算、实际编排、完整 L3 应用与多父融合。这里为本地模型夹具，不代表真实 API 收益。
- 本轮真实验证参考：同组 15 题，L0 通过率 100%、平均 51.13 秒、总 loop 6.87、无效 loop 1.07；种子通过率 93.33%、平均 53.87 秒、总 loop 6.67、无效 loop 1.40。种子 L1 查询 97／实际应用 22，L3 查询 100／实际应用 6，L2 编排尚未应用；尚无可发布版本，不能把注入变少视为已获得收益。
- 真实重启：已删除取消作业 61579880 的约 211 MB 结果及旧状态，无备份；数据集、私有 memory.pb、种子 SHA-256 均未变化。修复后服务在 tmux evolution-gepa-r7 持久运行，新作业 25cbcb62 从头执行真实 v1 GEPA，沿用原 60 题、4 并发和验收门槛，不复用旧成绩。

### CHG-20260917-298：异步技能结果进入所属循环的进展统计

- 真实问题：作业 25cbcb62 的 t-19 两次 video-gen 均失败，但 Coordinator 的循环查询没有 failed_actions／repeated_failure_steps；schedule.observe 只记轨迹，没有更新循环统计，而外层 run_skills 的派发 ACK 被计为成功进展。连续两个提案没有有效干预后取消搜索，不将取消题视作评测失败。
- 最小修复：在包内复用同步 action 的进展统计，异步子任务按实际操作、输入、输出与终态更新所属 loop。v1 仅在既有 dispatch 打点补 ref.parent，包通过显式因果关系识别派发封装，不猜测操作名；派发 ACK 不充当业务进展，真实派发错误仍保留失败计数。未修改 AgentFlow C++ 底座。
- 边界：重复回调不重复计数；未知或未执行结果不充当已执行进展；在途分支继续受原屏障保护，成功分支保留成功进展，同一操作成功后清除其失败连续性。这里只修复观察事实，仍由原策略决定继续／重规划／停止，不因单个分支失败直接结束整个任务。
- 评测隔离：执行语义指纹升级 control-outcomes-v3，旧成绩不得作为新语义下的评测缓存；不调整发布门槛、不前移留出集、不手改种子或 memory。
- 影响分析：AgentFlow 同一工作区 6d47af6 索引中，observe_schedule、fingerprint、v1 main 为 LOW；begin_activity／end_activity 及测试入口为 UNKNOWN，补核共享边界分发、HTTP／原生调用、异常收尾与 CMake 注册，未将空调用图当作无影响。
- 回归：新增用例在原逻辑复现异步失败被 ACK 掩盖；最终 33 组非 GEPA CTest 全部通过（74.91 秒），5 项 GEPA 证据修正、失败重规划、真实操作门槛、完整 L3 发布与多父融合回归通过（43.840 秒）。新增测试覆盖标准／激进模式、重复回调、两种并行返回顺序、循环隔离、成功恢复、未知子任务与真实派发错误。
- v1 跨进程夹具：4 条轨迹通过 doctor；L1=24、L2 动作=1、L2 编排=1、L3=3 次实际回执，两批编排不额外调用 Coordinator 模型。模型与 DataBus 传输为夹具，不计作真实 API 收益。报告 build/trajectory-evolution/v1-points.c4iadzou/test-summary.json。
- 已取消作业的同题验证参考：15 题，L0／种子通过率均 100%，平均耗时 55.07／59.95 秒，总 loop 7.47／7.00，无效 loop 1.27／2.13。种子 L1 查询 104／实际应用 24，L3 查询 105／实际应用 3，L2 编排查询 20／应用 0；未产生可发布版本。新修复收益必须用新作业验证。
- 清理及真实重启：删除取消作业 25cbcb62 的 239,969,944 字节结果及旧状态／日志，无备份；数据集、memory.pb、种子文件 SHA-256 前后相同。用户明确确认本轮测试数据外发后，服务在 tmux evolution-gepa-r8 提交新作业 86227ede，沿用 60 题、4 并发与原发布门槛，重新计算修复后的成绩，不处理其他历史数据。
- 延伸验证：GEPA 全量 83 项通过（760.149 秒）。v1 夹具追加固定断言，逐条核对异步失败终态、父循环 failed_actions 与派发封装标记；旧夹具的 2 个失败子任务对应计数 0，修复后对应 2。最终 4 条轨迹中 3 个失败技能全部正确进入父循环统计，报告 build/trajectory-evolution/v1-points.9yvh7r39/test-summary.json；L1／L2／L3 回执、并行与失败隔离原断言继续通过。此次追加只改测试，不改生产逻辑；本地夹具结果不计作真实收益。

### CHG-20260917-299：L1–L3 生效路径复审与 L2/L3 触发短板修复

- 真实诊断：已取消的 r8 搜索里，L1 出现 177 次查询／46 次应用；L2 编排 38 次查询／0 次应用；L3 的失败重规划提案与种子 `max_no_progress=1` 的已有行为重合。独立的多技能任务不能作为 A→B 依赖证据；同批次重排也无法替代“新 dialogue 开始前直接规划技能”的跨 dialogue 能力。本条不宣称达到真实收益目标。
- L2 样本：GEPA 优先选择已审查的训练集依赖案例；没有显式标注时，优先给实际执行失败且存在编排机会的训练任务一次反思机会。否定表述如“互不依赖”不再被“依赖”子串误识别。反思反馈在存在依赖案例时排除独立批次，提示词要求给出具体 A→B、失败语义，并保留无依赖分支并行。只使用训练内容，不将验证／留出题目送入反思。
- L2 误编排保护：新生成的 schedule_rules 必须携带本地检查的 `task_terms`，可用 `exclude_terms` 排除独立场景；条件不命中时原批次与宿主硬依赖保持不变。两个字段随 Protobuf 策略和 GEPA 组件持久化；未设置字段的已有手工策略保持原执行语义。训练预检要求规则产生真实依赖图差异，正式评测仍要求实际派发／完成回执。
- L3 信号：同 loop 的结构化重复失败现在在普通模式也有界采集，使已授权的 `replan_on_failure` 规则不依赖全局激进开关；普通模式仍不把该计数并入基础 `max_no_progress` 停止条件。成功重置、在途屏障、并发分支隔离与预算不变。
- L3 宿主覆盖：宿主拒绝建议的 `replan`／`stop` 后，重算进度时同样遵守普通／激进模式分界；普通模式的重复失败不再被误写为基础无进展。新增集成回归先在旧实现复现过早重规划，再用修复后的实现验证。
- 反思证据：控制反馈继续省略完整模型信封，但 L3 循环反思保留压缩后的模型评审浪费证据；被保留的循环事件按真实发生顺序发送。L2 编排／动作反思不附带该评审字段，保持输入预算集中在执行证据上。
- 搜索耗时：L0 基线分数按数据集、评测语义、评测配置及宿主模型选择独立指纹缓存；仅改变 GEPA 搜索提示词或轮次预算时可复用完整且哈希核验的基线分数，不复用候选结果，不保存凭据。执行语义改变会使缓存失效。
- 验证：完整 GEPA 搜索组通过（793.66 秒）。确定性端到端 L2 夹具同时含依赖和独立任务：候选在验证集的依赖任务 `scheduled`、独立任务 `unchanged`，两题均通过；无显式依赖标注时的否定语义、L3 评审证据与时序均有单项回归。非激进 L3 失败重规划具有实际回执。宿主覆盖计数缺陷先在旧二进制复现，再由修复后的 IntegrationService、PolicyRuntime、ShadowRuntime 三组及 4 项 L3 GEPA 回归验证。v1 实际函数跨进程夹具通过：4 条有效轨迹，L1／L2 动作／L2 编排／L3 实际回执分别为 24／1／1／3，L2 两波派发间额外 Coordinator 模型调用为 0；报告在 `build/trajectory-evolution/v1-points.u4sz06wd/test-summary.json`。模型和 DataBus 为夹具，不计作真实 API 性能收益。
- 评测缓存语义指纹从 failure-replan-v1／skill-schedule-v1／control-outcomes-v3 升级到 v2／v2／v4；更新原生全等级发布回归的预期指纹，防止将新执行语义与旧评分混用。最终 33 组非 GEPA 包级回归全部通过（74.78 秒）。
- 影响分析：L2 编排验证链的图分析为 CRITICAL，修改前已提示；GEPA 样本选择为 LOW，部分匿名运行时／测试入口为 UNKNOWN，已按源码核对实际调用点及原生、HTTP、异步回执路径。未修改 AgentFlow C++ 底座。

### CHG-20260917-300：真实 L2 守卫失配与 L3 空产物重复派发

- 真实 v1 r9 验证集各 15 题：L0／L3 种子通过率 80%／100%，平均无效 loop 1.40／1.73，平均时延 41.0／49.4 秒，平均 LLM 调用 6.53／6.87；种子并未达到无效 loop 或时延目标。首个 L2 候选使用正确的 `forge-image-gen → video-gen` 成功依赖，却因 `task_terms` 中“生成视频”不能匹配实际“生成5秒视频”而被训练差分预检判为零触发；作业已取消，未发布策略。
- GEPA 只在规则零触发时，用有依赖标注且真实出现同批两个操作的训练轨迹校准草稿短语：至少保留两个原文命中的短语，拒绝会命中已知独立并行样本的修剪。修正后仍走原有规则编译、来源授权和差分预检，不改线上匹配器，不用验证／留出内容改写候选。反思提示词明确短语必须是训练任务原文连续子串。
- L3 诊断：完成任务后再次派发同一技能时，新 prompt 和不同文字总结使完整结果指纹误计为业务新进展。包内对通用结构化产物字段补充有界检测：同一 loop／操作先成功产生文件，后续又成功但明确返回空产物时，不增加成果修订号；新产物、纯文本结果、并发分支和在途屏障仍按原规则处理。此信号只交给既有 L3 有界 replan／stop，不直接替宿主终止任务。
- 评测语义指纹升级 `control-outcomes-v5`，旧执行语义的候选与基线分数不复用。L2 短语修复、独立任务不误串行、L3 实际应用和发布的 5 项针对性回归通过；直接 action 与异步 schedule 产物分支的 IntegrationService 集成测试通过，原生 L0–L3 发布／重载回归通过。
- 影响分析：`control_preflight`、`record_action_outcome` 与评测指纹的图风险为 LOW；搜索循环存在未解析动态调用，测试 HTTP handler／部分测试函数为 UNKNOWN，已用源码检索确认入口和两条结果结束调用路径。未修改 AgentFlow C++ 底座或 v1 打点。新一轮真实实验因执行环境要求对此轮测试数据外发单独确认而暂未启动；本条不宣称修复后的真实收益。

### CHG-20260917-301：L1–L3 生效路径复核与 L2 多表述守卫

- 复核 r9 的真实验证集：15 题有 19 次 L2 schedule 查询，其中 16 次只含一个 skill，仅一次是 `forge-image-gen` 与 `video-gen` 同批。当前 L2 只为宿主已经提交的一批动作增加依赖，不能在新 dialogue 开始时根据用户问题直接选择并执行跨 dialogue 工作流；因此即使批内规则命中，也不能据此宣称节省首次 Coordinator 调用。L0／L3 种子的 80%／100% 通过率、1.40／1.73 无效 loop 和 41.0／49.4 秒时延仍是旧语义下的诊断数据，不是本次修复后的收益。
- L2 校验允许同一作用域和操作对使用不同的包含／排除短语条件，以便同一依赖覆盖中英文或不同任务表述；相同条件（含短语次序不同）的重复规则仍拒绝。相反方向的排序只有在任务守卫可证明互斥时才允许共存；可能同命中的环仍在编译时拒绝，自身包含／排除相矛盾的规则也拒绝。运行时冲突回退和宿主依赖保护不变。新增用例验证中英文各自命中、互斥任务的相反排序，以及重复、重叠、不可满足守卫被拒绝。
- L3 的空产物识别只使用明确的 `artifacts`、`artifacts_json`、`generated_files` 结果字段，且仅将可解析的空集合视为“无新产物”。格式错误的产物字符串和可能是输入附件的 `files` 字段退回原结果指纹判断，避免把有效工作误记为无进展。
- 本地回归：33 组非 GEPA 包级测试全部通过（74.55 秒），修改后另有 3 项相邻 GEPA 测试通过（21.95 秒）；包括 L0–L3 边界、异步调度、并行失败隔离、重复空产物、新产物与含糊产物字段，以及两条同操作对条件规则通过控制草稿编译和来源授权。图影响分析把 L2 校验入口标为 CRITICAL，索引被生成物污染且结果截断；已提示风险并核对三个直接调用点：策略校验、控制草稿编译及运行时调度。没有修改 AgentFlow C++ 底座。尚未启动新的真实 API 对照实验，不能把本地回归视为目标达成。

### CHG-20260917-302：空产物进展启发式的反例与待审修复

- 再审 r9 真实 v1 验证轨迹发现反例：`v-accessible-html` 第一次调用 `outfit-stylist` 生成 HTML 文件，第二次同技能调用带 `force=true`，要求实际读取该文件逐项核验；第二次没有新文件，但返回了具体核验结果，模型评审判定整题无无效 loop。现有 `material_successes` 启发式会把“同操作先有文件、后无新文件”视为无业务进展，可能误伤这次合法核验。相反，`v-wardrobe-csv` 后两次同技能派发分别是“占位”和“任务已完成”，均无 `force`，评审标出 9 次无效 loop。单靠空产物数无法区分两者。
- 拟议的框架中立修正是由既有 dispatch 打点的可配置布尔路径声明“有意重复执行”，仅使有明确授权的核验不走空产物启发式；无声明时不从具体框架字段猜测。该变更涉及跨客户端派发和 L3 停止边界，影响分析对派发入口给出 CRITICAL／部分截断；源码确认实际链路为 `instrumentation.cpp → dispatch_batch_point → schedule activity → observe_schedule → record_action_outcome`。另一保守选项是撤掉未经真实收益验证的空产物启发式，恢复原结果指纹判定，同时升级评测语义指纹。
- 上述两种代码补丁均被执行环境的安全审核拒绝，未写入；审核担忧削弱停止保护、放大重复执行和资源消耗。当前代码及 `control-outcomes-v5` 指纹保持不变，不能认为反例已修复，也不能以现有集成测试的通过证明这条启发式无误判。需要对风险边界获得明确批准后再修改并复测。

### CHG-20260918-303：按业务因果关系修正采用、编排、循环与反思证据

用户明确授权修正 CHG-302，并要求暂停真实实验，审查自进化的业务逻辑。本轮取消 r11 后保持暂停，保留现场；没有改 AgentFlow 底座或 v1 宿主流程，修改均位于进化包、测试与说明。CHG-302 中的“未写入”描述只代表当时状态，以下修复已落地。

审查标准是“该不该采用、该控制哪个执行实例、失败能传播到哪里、是否有实际执行与收益证据”，而非仅检查代码能否运行。

| 问题与具体场景 | 位置 | 原因 | 修正后的业务行为 |
| --- | --- | --- | --- |
| 公共经验更相关，却被较弱私有经验占掉唯一注入名额 | `src/shadow_runtime.cpp`：`ShadowRuntime::query` | 所有私有条目在相关度比较之前排序 | 无重复/冲突时按检索相关度排序；重复经验的私有副本继承较高匹配分并优先去重；未授权私有不压制公共 |
| 两条独立工作流 A1→B1、A2→B2 被补成所有 A→所有 B，A1 失败后 B2 也跳过 | `src/orchestration.cpp`：`plan_schedule` | 只按操作名做笛卡尔积连接 | 学得规则在多实例歧义场景只调整已知实例依赖；单一配对正常生效，明确的多前置汇合仍保留，同操作串行和手工基础规则兼容 |
| 同批两个同名操作各首次失败一次，下一轮就被视为重复失败 | `src/boundary_runtime.cpp`：`begin_activity`、`observe_schedule`、`record_action_outcome` | 完成次数等同于因果上的重试次数 | 动作开始时冻结已知失败链与代际，同批首次失败不累加为重试；看到失败后启动的后续尝试才延长失败链，同步/异步共用逻辑 |
| 生成文件后合法读回核验，没有新文件便被当成停滞 | `src/boundary_runtime.cpp`：`record_action_outcome` | 把空产物数当成通用业务进展判据 | 撤掉空产物启发式，按真实输入与结果指纹记录进展；相同结果重复、失败控制、硬轮数、重规划预算与在途屏障保留 |
| 同一 scope 的 planner 和 worker 不能分别拥有循环策略，或通用规则遮住专用规则 | `src/control_generation.cpp`、`src/policy_runtime.cpp` | 去重只看 scope，选规则未稳定区分 actor | 按 scope+actor 区分；执行者专用规则优先，同执行者的学习规则覆盖其基础规则，顺序不影响选择 |
| worker 借用了另一个执行者的 replan 预算；覆盖率也把其他 actor 的查询算进来 | `src/control_generation.cpp`、`src/gepa_search.cpp` | 只检查任意基础规则是否允许 replan；查询归因未匹配 actor | 只继承匹配的基础规则与预算，按 actor 归因；不能用别人的权限或查询证明本规则可用 |
| 已有派发前失败检查，GEPA 却看不到可阻止第三次重试的机会 | `src/gepa_search.cpp`：`feedback_trial`、`action_control_evidence` | `guard_only` 实际在 `step.start`，读取位置却是 `action.start`；普通工具重复失败也漏选 | 关联正确的 step，保留 actor 与动作控制；区分未执行的检查、真实执行和 allow/reject，检查和普通工具都可提供真实机会 |
| 参数排在第 40 个字段，展示裁剪后被误判为缺参 | `src/gepa_search.cpp`：`feedback_trial` | 使用给模型看的裁剪片段反推实际参数缺失 | 在完整原始参数上计算 `missing_required_arguments`，展示裁剪不改变可执行条件判断，提示词同步明确此约束 |
| 前面有 20 个无关失败，或 9 批单动作，末尾真正的 loop/编排机会被截掉 | `src/gepa_search.cpp`：`control_feedback_trial` | 展示预算先到先得，普通错误挤掉控制证据 | 优先保留可干预控制与多实例批次，再补上下文并恢复事件顺序；实例 ID 和宿主依赖不按普通数组裁掉；整体输入预算不变 |

逻辑反例与回归：

- `test_policy_runtime.cpp` 新增多工作流失败隔离、明确汇合、歧义不连边、执行者规则顺序、跨 actor 重规划预算、覆盖率归因。原有“两个 planner 自动都成为 image 的依赖”断言被拆成有/无明确依赖两种业务场景，不再把旧实现直接当正确标准；失败报告现在标注具体用例名。
- `test_integration_service.cpp` 覆盖同步与异步同批首次失败、真正后续重试、正常无文件核验、相同结果重复、成功分支和在途分支保护。测试临时目录使用系统临时根并在正常退出时清理，不再在仓库根新建 `integration-service.*`。
- `test_shared_memory.cpp` 覆盖更相关公共经验被采用、私有重复优先、私有未授权时公共副本可用。
- `test_gepa_search.py` 新增六个经采集接口产生轨迹的反例：派发前检查、普通工具重复失败、完整但超长的参数、晚出现的缺参、晚出现的循环证据、晚出现的多动作批次。使用本地模型替身验证选择→反思→生成→预检查→真实控制回执，不发送真实 API 请求。
- 候选评测语义指纹更新为 `skill-schedule-v3 / control-outcomes-v7 / retrieval-evidence-v5 / control-feedback-v2 / actor-loops-v2`，旧候选评测不能沿用。L0 的行为未改变，保留独立基线缓存指纹，不因这些控制修复重测同一基线。
- 已验证：33 组非 GEPA 回归全部通过（74.55 秒）；六个新增 GEPA 反例通过（65.089 秒）；全量 97 项 GEPA 回归分四组核验全部通过，最后一组 25 项通过（239.454 秒）。真实 v1 函数打点回归通过（模型、DataBus 为测试替身），4 份轨迹有效，实际回执包括 llm=24、actions=1、schedule=1、loop=3，报告为 `build/trajectory-evolution/v1-points.wqpc8src/test-summary.json`。最终 `git diff --check` 通过；仓库根目录未新建 `integration-service.*`。

本轮不把查询次数当成采用次数，不把采用次数当成收益，不以测试替身的评分宣称真实性能改善。自然语言条件仍使用当前确定性检索；上述修正不等于实现了通用语义理解。审查与回归阶段保持真实实验暂停。

用户随后明确要求启动新实验。r12 已在 `tmux evolution-gepa-r12` 后台提交，作业为 `1eadf46ac6c1acd76f024eaddb2069821f7945402912f8d884c54c9621d5f29a`，真实 v1、4 题并发、最多 5 小时，沿用现有 `.env` 凭据与火山模型配置。新产物统一写入 `trace_output/v1-l4-coldstart/l3-current/`，服务日志为该目录下的 `service.log`，不在仓库根创建临时目录。沿用既有 L0 参考报告并由程序校验兼容性，候选重新评测；质量、无效 loop 至少下降 15%、平均时延不增加的门槛不变。首批 4 道种子验证题全部通过，但尚不能据此宣称搜索或发布成功。已清除取消的 r11 作业约 308 MB 产物及旧状态，无备份；数据集、公共 memory 与冻结种子的 SHA-256 前后相同，其他私有 memory 和 L0 参考报告保留。

### CHG-20260918-304：r12 到节点 10 的复盘与执行、评测逻辑修复

按用户要求，真实 v1 r12 运行到节点 10 后停止；搜索树为 `cancelled`，对应 tmux 服务已退出。节点 11 是取消时留下的待执行占位，没有完成生成或候选评测，不计入本轮结果。保留数据集、memory、L0 缓存以及节点 1–10 的原始证据，不改写旧报告，不启动节点 11。

现场目录：`trace_output/v1-l4-coldstart/l3-current/evolution/agent-evaluation/1eadf46ac6c1acd76f024eaddb2069821f7945402912f8d884c54c9621d5f29a/search/`。

#### 节点结果

以下为修复前程序的原始评分，用于解释现场，不是按新校验口径重新评测的成绩。`accepted` 是保留搜索分支，不是发布。平均时延仅计 Agent 执行，不等于整个进化作业用时。

| 节点 | 父节点 | 改动 | 原始结果 | 结论 |
| --- | --- | --- | --- | --- |
| 1 | 0 | L2：图像→视频编排 | accepted；验证 13/15；59.65 秒；无效 loop 2.07 | 有顺序边，但错误使用 `require_success=false`；依赖失败后仍启动下游 |
| 2 | 0 | L2：重复失败拦截 | accepted；验证 14/15；52.14 秒；无效 loop 1.87 | 有实际拦截，但重新派发 worker 后，同一目标的失败计数丢失 |
| 3 | 1、2 | 双亲融合 | rejected_merge；训练小批 2/4；71.20 秒；无效 loop 2.50 | 融合已执行，组合后退化而被拒绝，并非没有多父能力 |
| 4 | 1 | L3：失败恢复控制 | error：`control_failure_replan_not_authorized` | 生成能力与基础恢复权限不一致；需要可表达的预算收紧路径 |
| 5 | 0 | L3 | duplicate：无行为变化 | 基础无进展阈值已为 1，原搜索空间不能进一步减少 replan |
| 6 | 2 | L1：补充禁止换提供方重试的经验 | accepted；验证 13/15；51.19 秒；无效 loop 1.13 | 比 L0 无效 loop 下降约 26%，但质量从 14/15 降至 13/15，不满足发布要求 |
| 7 | 1 | L2 | duplicate | 当前选定编排组件没有新可执行改动；不应强造规则 |
| 8 | 1、6 | 双亲融合 | rejected_merge；训练小批 4/4；49.47 秒；无效 loop 1.75 | 同题父节点 6 为 39.04 秒、无效 loop 0.25，融合未改善 |
| 9 | 0 | L2：重复失败拦截 | accepted；验证 13/15；60.29 秒；无效 loop 2.40 | 与节点 2 的执行条件实质相同，却因说明不同重新评测 |
| 10 | 1 | L1 | duplicate：无行为变化 | 模型保留原经验，未对不存在的改动再跑子节点 |

参照：缓存 L0 验证集为 14/15、平均 51.19 秒、平均无效 loop 1.53；种子为 11/15、58.37 秒、1.87。本轮没有发布版本。正常候选退化、无改动、融合未改善，与下面的程序缺陷需要分别看待。

#### 问题与修正

以下路径均相对于 `tools/trajectory_evolution/`。

| 问题 | 位置 | 诱发原因与现场 | 修正 |
| --- | --- | --- | --- |
| 成功依赖写成仅等待完成，错误派发仍可能通过评分 | `src/gepa_search.cpp`、`src/evaluation_contracts.h`、`src/task_evaluation.cpp` | 节点 1 的 t-12：图像失败后，视频 worker 仍启动，随后才检查文件不存在；模型只看最终未生成视频便判通过 | 从明确训练约束、真实同批操作和命中条件修正成功边；增加独立于模型的 `contract.schedule_success` 检查，按实际分支执行判定。仅影响依赖分支，独立分支继续。评测、重评及历史报告评分共用检查 |
| 同一失败任务换执行实例即可绕过重试上限 | `src/failure_ledger.h`、`src/boundary_runtime.cpp`、`src/policy_runtime.cpp`、`proto/runtime_policy.proto` | 节点 2 的 t-19：`generate_video` 被拦截后重新派发，仍生成同一输出路径，但 `prior_failures` 变回 0 | 新增可学习的 `repeat_failed_target`，按明确操作和 1–4 个目标标识参数建立 run 内台账；跨执行实例延续，同批首次失败不叠加，成功重置；不同目标、操作、run 隔离。不把所有技能失败合为全局失败 |
| L3 已在阈值 1 replan，模型无法生成更早结束恢复的策略 | `src/control_generation.cpp`、`src/gepa_search.cpp`、`config/prompts/` | 节点 4 请求未授权恢复；节点 5 明确指出阈值已最小但 replan 后仍重复 | 增加有界恢复预算能力，候选可在匹配 actor 的基础范围内降低 `max_replans`；为 0 时，无进展达到阈值后 stop。普通首次失败不直接终止，在途工作和其他 actor 保护保留；不扩张宿主权限。能力不匹配可做一次有界修正 |
| 规则换一句说明便当成新行为，重复跑完整评测 | `src/gepa_search.cpp`：`proposal` | 节点 9 与节点 2 的有效操作条件相同，只有说明改变 | 按有效执行条件去重，忽略控制说明变化，记录 `no_control_predicate_change`；L1 文本、条件和阈值仍参与比较 |
| 没有采用改动的题随机变快，也能支持局部优势 | `src/gepa_search.cpp`：`applied_changes`、筛选与融合 | 仅比较分数，缺少“该题实际上执行了相对父节点的变化”的证据 | 在子节点实际查询上重放父策略并关联真实回执，输出逐题 L1/动作/编排/循环差异；局部收益只取有差异的题。两个父节点分别比较；训练和验证均无差异时记 `unobserved_changes`。这是采用证据，不代替真实收益对照 |
| 历史应用次数与本次最终评测混在一起 | `src/agent_evaluation.cpp`、`src/gepa_search.cpp` | 父分支或训练中曾经用过规则，不等于当前版本最终运行已覆盖 L1–L3 | 保留历史同规则证据，但单列 `validation_control_coverage`；最终门禁的 `control_coverage` 使用本次评测，历史合并项为 `historical_control_coverage`，不拿历史总数替代当前全级能力验收 |
| 主动取消被显示为错误，评分结束到发布之间存在取消窗口 | `src/agent_evaluation.cpp` | r12 搜索树已取消，外层作业却为 `error/gepa_cancelled` | 显式保存 `cancelled`，重试清理取消标记；发布锁内再检查取消状态，取消不能继续发布。旧作业记录保留现场原值 |
| 自定义元数据类型不符可能中断搜索；评分项可能与内置检查重名 | `src/gepa_search.cpp`、`src/evaluation_contracts.h`、`src/task_evaluation.cpp` | 自定义 `dependency` 为数组，或 `stop_on_failure` 为字符串；用户检查使用内置检查 ID | 只有约定的字符串/布尔类型才解释为依赖契约，其他自定义数据不强行转换；内置检查 ID 在执行前保留，拒绝重名，不产生相互覆盖的评分 |

目标失败台账每个 run 最多 256 个键，计数有上界，不跨 run、用户或会话传播；缺少明确目标参数不猜测目标。运行时与离线差分预检共用台账逻辑；实际执行参数变更时，不把失败归给原来的目标。相关配置、规则字段和能力描述随策略导出、重载和来源校验保存，不要求 v1 增加业务代码。

#### 外部问题与正常搜索结果

- 真实图像 API 返回 `429 / AccountQuotaExceeded`，视频 API 返回 `404 / UnsupportedModel`。这是配额和模型接入条件问题，修复重试、依赖隔离与评分不能恢复外部服务能力；本轮未擅自更换凭据或模型。
- 节点 9 首次评分的两条模型错误已由现有补评机制恢复，只补评原执行，不重跑 Agent。这是已生效的恢复机制，不应作为新增业务缺陷重复修改。
- 节点 3、8 的融合退化，以及节点 7、10 无新改动，是搜索允许的结果；不能为了“每个节点成功”放宽最终质量、无效 loop 与时延门槛。

#### 验证与范围

- 第一轮完整验证：101 项 GEPA 测试全部通过，分四组退出码均为 0；33 组非 GEPA 包级测试全部通过。新增测试覆盖同目标跨实例拒绝、并发首次失败、成功重置、不同目标继续、实际参数变化、明确成功依赖、L3 收紧恢复预算、说明去重、真实回执归因以及取消。
- 最终代码重新构建后，102 项 GEPA 回归全部通过，四组退出码为 `[0, 0, 0, 0]`（最长一组 240.330 秒）；33 组非 GEPA 包级回归再次全部通过（74.73 秒，其中任务评测包含 103 个测试）。日志为 `build/trajectory-evolution/r12-fix-gepa.log`、`build/trajectory-evolution/r12-fix-regression.log`。
- 使用生产代码中的契约检查和目标台账，直接读取 r12 节点 1／2 原始轨迹做只读回放：t-12 被确定性标记为成功依赖违规；t-19 的 event-39／56／73／133 目标失败计数为 `0 → 1 → 2 → 2`，最后一次跨 worker 派发不再变回 0。该回放没有执行 Agent、没有请求模型、没有重写旧报告；结果为 `trace_output/v1-l4-coldstart/l3-current/r12-fix-evidence-check.json`。它验证历史反例可被新逻辑识别，不是修复后的真实性能测量。
- GitNexus 影响分析对规则授权、编译和评分链提示 CRITICAL，修改前已告知；部分新增入口为 UNKNOWN，已核对实际调用方，包括 CLI、固定候选评测、GEPA、原生和 HTTP 边界。修改范围限于进化包、测试和说明，没有修改 AgentFlow C++ 底座或 v1 业务流程。
- 候选评测指纹更新至 `failure-replan-v3 / skill-schedule-v4 / control-outcomes-v8 / control-feedback-v3`；新增确定性契约使用 `schedule-success-v1` 标记。旧执行证据可重算新检查，不必重跑 L0；旧候选成绩不得直接代表修复后行为。

本条记录的是代码与回归修复，不能据此声称已经产生新的可发布策略。下一次真实对照需要使用新二进制与新候选评分，保留数据集、经验及兼容的 L0 执行证据；本轮按“节点 10 后停止”的要求保持暂停。

### CHG-20260918-305：重跑真实 v1 L0 基线并整理进化包内部源码目录

按用户要求，在两把候选图片密钥分别返回 `InvalidSubscription` 和 `AccountQuotaExceeded` 后，未替换当前环境凭据，使用既有测试环境重新执行真实 AgentFlow v1 的 L0 holdout。此次直接运行 `dataset run`，没有传入历史报告或策略包；评测器实际加载同一 `level=L0、policy=null` 版本，15/15 道题均生成闭合轨迹。

- 新结果位于 `trace_output/v1-l4-coldstart/l3-current/l0-fresh-20260918/`，其中 `report.json` 为完整报告，运行日志和退出码也归入该目录。
- 结果为 14/15 通过，平均 Agent 时延 55.364 秒，P95 99.749 秒，平均总 loop 7.067，模型复核的平均无效 loop 1.200，平均 LLM 调用 7.067、工具调用 3.067、token 49,989.5。
- 唯一失败题 `h-video-terminal` 在视频服务返回 `UnsupportedModel` 后又创建了两次相同任务，违反题目明确的“不能反复创建相同任务”；不是评分器额外发明的条件。
- 旧缓存 L0 同为 14/15，但平均时延 51.193 秒、总 loop 6.733、无效 loop 1.533。两次结果存在正常波动，后续策略收益必须与固定对照口径比较，不能把本次重跑当成进化收益。
- 凭据检查未发现 `ark-...` 原文。报告中的 `Authorization: Bearer` 命中只包含 12 字符的环境变量引用，不包含真实值。

为降低 `tools/trajectory_evolution/src/` 的阅读成本，只调整内部文件位置和构建路径，没有更改函数实现或 AgentFlow/v1 业务流程：

- `analysis/`：轨迹、loop 与批次分析；`evaluation/`：数据集、宿主执行与评分；`gepa/`：搜索树、候选融合和控制生成。
- `runtime/`：L1–L3 策略应用；`service/`：HTTP/本地边界和语言绑定；`model/`：模型传输、限长与契约；`memory/`：公共/私有经验管理；`cli/`：命令分发。
- `src/README.md` 新增中文导航，关键模块入口补充中文职责与不变量注释；CMake 使用统一的内部 include 目录列表，包内文档的当前源码路径已同步。
- GEPA 搜索回归现有 102 个串行场景，原 900 秒 CTest 上限会产生假超时；仅将测试上限改为 1800 秒，不改变模型、Agent 或运行时超时。

验证：核心库、CLI、本地接入库及相关测试目标全部编译通过；33 项非 GEPA 包测试通过；102 项 GEPA 搜索测试按四个独立临时目录分片运行，全部通过、退出码均为 0；最终 `git diff --check`、文档测试和 GEPA 配置测试通过。GitNexus 对整个脏工作区汇总为 CRITICAL（126 文件、877 符号），其中包含此前未提交修改，不能归因于本次目录搬迁；因此本条之后未继续扩张业务改动。

### CHG-20260918-306：补齐派发失败回归的打点返回契约

提交前复测发现，`test_dispatch_failure_reaches_after_dispatch_hook` 的打点 mock 只记录事件、默认返回 `None`，未适配新增的 skill guard 返回值读取，因此测试尚未进入 manifest 写入失败场景就异常退出。仅让 mock 返回与真实接口一致的允许决定和原始输入，保留原有断言与失败场景，不修改业务代码。

验证：Python runtime 585 项通过、4 项跳过；进化包 33 组非 GEPA 测试通过；GEPA 全部 102 个场景分四片通过；重新编译后的会话隔离 5 项通过，沙箱生命周期 14 项通过、1 项既有占位测试跳过。构建时排除了 Conda base 与 agentflow-dev 的依赖混用，仅调整本地构建缓存，不提交环境配置。本次未启动真实 API 实验。GitNexus 全量变更超过单次 1000 符号列表上限，补充分批检查分别覆盖 617 和 834 个符号，均无 partial/truncated 标记；恢复后暂存树与分批前一致，累计改动风险仍为 HIGH/CRITICAL。

### CHG-20260918-307：修复 GEPA 证据解析、暂停计时和策略变化归因

- 问题与诱因：
  - 模型请求可以仅应用编排提示而不含经验，Protobuf 会省略空 `experiences`；反思直接读取数组导致 `array_required`，尚未请求模型便失败。
  - 未观测的 loop 计数为 null，不应当作整数或已测得的零；旧规则未命中也不应抹掉新规则的真实训练回执。
  - 人工取消后的停机时间占用搜索预算，修复后续跑可能直接进入时间预留阶段。
  - L1 上下文包含外层 `policy_id`，仅修改 L2/L3 也会改变编号；整段字符串比较会误报 L1 行为变化，使无实际作用的候选获得归因证据。
- 改动：
  - 空经验回执跳过，类型错误仍拒绝；未知计数保留未知。搜索允许未改动的继承规则继续等待场景，新改动及最终发布仍要求实际覆盖。
  - 仅扣除明确取消后的暂停时间，保留原始开始时间及已消耗预算，过期预算不复活。
  - 比较注入效果时只归一化宿主生成的外层版本标记，正文、编排提示和上下文移除仍按真实执行判定。
  - 在安全批次边界校验已有报告哈希并重算旧归因，不请求模型、不重跑 Agent、不修改原始评分。历史父子边保留，被纠正的无作用节点退出新父节点选择。
- 涉及文件（均在 `tools/trajectory_evolution/`）：
  - `src/gepa/gepa_search.cpp`、`src/gepa/gepa_strategy.cpp`。
  - `tests/test_gepa_search.py`、`tests/test_gepa_strategy.cpp`、新增 `tests/test_gepa_evidence.cpp`，以及对应 CMake 测试登记。
- 验证：版本号假差异已用本地夹具复现；修复后 GEPA 配置、证据、策略树、运行时四组 CTest 通过；完整 104 项 GEPA 集成回归按四片运行全部通过。额外覆盖缓存哈希异常不得部分迁移、迁移幂等、原始评分不变、已消耗预算不变及历史子节点可恢复。
- 实验观察：复用 L0 验证集，节点 5 为 13/15 通过、平均无效 loop 1.333、时延 44.659 秒；不满足不降质及无效 loop 至少下降 15% 的发布条件。节点 6 的新增动作规则未实际触发。结果尚不能作为策略发布依据。
- 范围：未改 AgentFlow v1、C++ 底座、同事负责的评测器／数据集／经验库，也未更换凭据和模型。GitNexus 对变化归因上游为 LOW；历史树校验为 UNKNOWN，已补查到 GEPA 恢复及测试调用方。

### CHG-20260918-308：GEPA 默认按效果选优，能力覆盖改为诊断

- 问题：最终选优先比较等级能力是否全部命中，再比较质量与成本；节点 7 虽然验证集只有 11/15 通过，仍因历史能力覆盖齐全压过效果更好的节点 9／10。发布处还会再次按默认全能力门槛拒绝未触发编排的候选。
- 调整：默认 `gate.require_level_coverage=false`；选优保留质量约束、无效 loop、时延、目标得分及逐题稳健性比较，不以全规则／全能力命中作为硬门槛。发布判定同步按已配置效果门槛执行；显式设置 `true` 时仍可要求严格覆盖，但覆盖完整不再抵消效果退化。
- 保留：完整的本轮及历史覆盖统计、不确定评分拦截、相对基线不降质和成本门槛；搜索仍要求新改动实际应用，并验证相对父策略的决策差异。未命中的规则不会被记成已命中，未采用改动的随机收益不会绕过搜索证据检查。
- 兼容：只更新评测作业语义指纹 `effect-selection-v1`，避免旧门禁结论当作新结论复用；不改变 Agent 执行、轨迹字段、规则运行时语义或 L0 执行缓存指纹。
- 涉及文件：`tools/trajectory_evolution/src/gepa/gepa_search.cpp`、`src/evaluation/agent_evaluation.cpp` 的选优／发布条件，对应 GEPA 证据与集成测试、评测上下文测试；同步包内第 19、24 章。未改 AgentFlow v1、C++ 底座、数据集或经验库实现。
- 验证：完整 104 项 GEPA 回归分四片全部通过；21 项后台评测集成测试通过；GEPA 配置、策略树、证据、运行时与文档测试通过。只读调用生产选优函数回放已有真实树，输出 `Recorded tree winner=7 effect-first winner=10`，未调用模型或修改原树、原评分。
- 发布复核：固定节点 10，使用既有真实 v1 宿主及同一套 15 道留出题，通过标准后台固定候选入口做配对验收，不重跑搜索或验证集。作业 `2dae51ed57ee553ca5d02e3276e4d76cac19cbc2dd6b47ca8eb4644615c243e7` 于 2026-09-18 15:40 UTC 完成：L0 13/15 通过、无效 loop 均值 2.333、平均时延 54.55 秒；候选 12/15 通过、无效 loop 均值 2.200、时延 55.75 秒。候选 loop 仅下降 5.7%，质量下降且时延增加，被 `quality_regression`、`max_latency_ratio`、`loop_target_not_met` 拒绝，未发布。编排仍未触发，但已不再作为拒绝原因；L1／L2 动作／L3 分别实际应用 25／1／6 次，其中新增 L3 规则实际应用 3 次。
- 本轮遗留观察：英文任务在停止协调后给出中文兜底回复；图片供应商仍返回配额错误。`h-image-flatlay` 的模型评分还引入了题目未明示的停止条件，需要由评测负责方复核。本次没有修改评分结论、放宽性能门槛或反复抽跑至通过；保留原始实验及本轮完整报告，不能把验证集收益作为已发布收益。
- 影响检查：仓库与工作区均为 `/root/lai-code/Agent/AgentFlow`，索引刷新到当前工作区；GitNexus 对发布判定与语义指纹为 LOW，对选优上游为 CRITICAL 且含不完整图结果，修改前已告知，并结合实际 `run`／`update_frontier` 调用和回归验证。测试方法的动态发现为 UNKNOWN，已核对 unittest 入口。

### CHG-20260918-309：L1–L3 异步结果、重试预算与停止收尾

- 问题：异步 dispatch 的 guard 回执不是实际执行，原目标失败预算没有从 schedule 子动作结算；循环把整个回执参与比较，新回执 ID 可能掩盖重复结果；Coordinator 被停止后只返回固定占位回复；模型打点的回调沿用原 tools，移除工具声明未进入真实请求。
- 运行时修复：异步子动作在 running 时记录目标快照，终态结算，L2 无 loop 也支持；进展比较复用 result_output 绑定，原始输出不变且不复制整段业务结果进属性；补充明确结构化失败观察，已成功结果附带历史错误不恢复失败链。
- 收尾：引用式 loop 点可声明 finalize_on_stop，stop 后只放行一次最终模型答复、不再注入新经验，并阻止后续动作、派发和重复模型调用；停止与收尾分别留痕。收尾提示来自外部文本，缺少模板的旧配置保持原行为。
- 范围：主要为 `tools/trajectory_evolution/src/runtime/`。另有服务配置加载模板的少量代码、包内模板／文档／回归测试，以及响应协议的可选 finalizing 字段；未启用收尾的响应保持原结构。v1 仅在原循环点加一个选项，并让现有模型打点回调读取准备后的 tools；没有新增宿主适配器，没有改 AgentFlow C++ 底座、评测、数据集或经验库实现。工作区原有改动保留。
- 验证：最终 15 组 CTest 全部通过，覆盖本地运行时、异步 dispatch、HTTP／原生边界、契约、多语言客户端及 GEPA 配置／策略／证据；另有 8 项 GEPA 定向集成测试通过。真实 v1 Python 函数配合夹具 DataBus／模型的多进程测试验证收尾请求实际不携带 tools，收尾轨迹结构校验通过。另检查新旧响应契约和文档链接。日志为 `build/trajectory-evolution/l123-final-ctest.log`、`l123-final-v1.log`、`l123-gepa-integration.log`。未调用远程模型、未重跑真实 L0 或 GEPA 实验，不能据此宣称性能目标已达成。
- GEPA 关联补充：用户明确授权后，在 `src/gepa/failure_ledger.h` 增加异步开始快照读取，仅接到 `gepa_search.cpp` 的 `control_preflight` 与 `applied_changes` 两处；运行时异步动作记录明确 target_scope。评分、发布门槛、搜索算法保持不变，原始报告不回写；缺少作用域的旧日志不猜测。回归覆盖预检识别真实重复失败、相同父策略不误报新作用，以及并发首次尝试、重复回执、成功复位、未执行、其他目标／作用域和跨 run 隔离。
- 范围外：自然语言同义结果的语义去重、已成功任务的禁止重复执行属于额外能力，本条不声称已实现；业务评分及 executor 遗留错误聚合仍按协作边界交由相应模块处理。

### CHG-20260919-310：GEPA 生效归因、结构化控制与重复派发收敛

- 问题：文件校验代码中的 `sig = f.read(8)` 被误判为凭据并改写；后端配置失败可通过换输出路径绕过重试计数；成功空转派发缺少预算，L2 拒绝后也未进入 L3 无进展判断；编排依赖中文短语且多实例缺少配对依据；相同建议可重复注入；训练触发过的规则可能在验证完全未触发，却凭其他题分数提高被选优。
- 安全：仅为明确代码赋值位置的 `sig/signature` 函数调用表达式保留原文，兼容 JSON 转义换行；实际注册密钥、带引号字面值、URL 签名和点分凭据继续脱敏。没有关闭凭据保护或修改用户 API 配置。
- L1：同一请求按有效正文与 scope 去重，只为实际注入的来源记录回执；原有阶段条件、私有优先与 L2/L3 独立条件保留。它不承担主模型必然遵从建议的保证。
- L2：增加按结构化错误码和模型/服务配置隔离的 `terminal_failure`，不再用输出路径判断同一配置故障；调度新增 `before_argument/after_argument`，用显式共享资源配对生产者/消费者，可不依赖任务语言。不同资源不连边，无关分支保持并行，原宿主依赖不删除。
- L3：增加可学习的 `operation_budget`，成功/失败均消耗已落定轮数预算，提示词改写不重置；可以按稳定目标隔离，空目标键明确表示该操作在 run 内共享预算。它不是语义完成检测，也不默认施加到所有技能。已确认的拒绝不伪造成工具失败、不继续扣预算；同轮没有其他进展时进入无进展计数，仍保护在途分支与有新结果的独立工作。
- 账本与轨迹：成功保留代次，防止旧并发失败晚到恢复已清零的失败链；异步投影结果中的错误码记录为 `observed_error_code`，供回放使用，不复制完整结果。反思补充异步已落定次数、结构化错误码和有限资源字段，避免运行时支持而反思无生成依据。
- GEPA：候选保留和最终选优分离；选优需在验证集实际改变行为的同题子集上相对至少一个父节点产生不退化增益，并满足已配置时延限制。新增 `validation_gain_attributed`。完整验证/留出门槛与评分权重未放宽；没有把留出题前移进反思，也没有重跑基线。新字段贯通生成、授权往返、运行时、预检、父策略回放和版本序列化。
- 回归审查：全量测试发现仅凭普通调用次数会使 action 候选挤占真正的 loop/缺参机会，已收窄到明确的重复派发 guard；正常多次调用仍可供反思阅读，但不单凭次数提高候选优先级。增加普通调用重复 20 次的反例，原搜索测试断言未放宽。另补充脱敏占位符不参与资源/配置/目标身份比较，防止不同分支被错误关联。
- 范围：`tools/trajectory_evolution/src/runtime/`、`src/gepa/`、`proto/runtime_policy.proto`、外部反思提示及相应测试/包内文档；另有 Python SDK 的局部脱敏修复。未改 AgentFlow C++ 底座、未新增 v1 适配器或打点以外的宿主逻辑；未改本轮数据集、记忆管理与业务评分模块。保留工作区原有协作修改。
- 验证：最终版本编译成功，72 项 Python 安全/模型客户端测试、34 组 CTest（搜索集成单独分组）、全部 104 个 GEPA 搜索集成用例通过。搜索按四组各 26 项并行复测，未放宽原断言。另运行真实 v1 函数配合模拟 DataBus/模型/工具的接入测试：2 个并发 worker、4 条轨迹校验通过，实际应用回执为 L1 24、动作护栏 1、编排 1、loop 4；编排按 prepare/report → image 两波执行，不增加中间模型调用，收尾与采集器不可用回归通过。测试不调用远程模型。日志：`build/trajectory-evolution/l123-core-final.log`、`l123-gepa-repair-{1,2,3,4}.log`、`l123-v1-final.log`；v1 结果为 `build/trajectory-evolution/v1-points.3e1cmkts/test-summary.json`。覆盖新旧策略兼容、字段往返、并发首次尝试、成功复位、配置/目标/run 隔离、L2 拒绝后的 L3 收敛、共享资源配对、异步反思与选优归因。`git diff --check` 通过。
- 边界：本次没有启动真实 API/GEPA 实验，没有删除数据集、memory 或历史产物，不能据单元测试宣称达到优化目标。自然语言同义结果仍不做语义完成判定；L2 资源编排仍作用于已有同批任务，不创建未来批次。安全执行修复影响宿主行为，下轮对照前需要确认旧基线可比性，不能将宿主修复算成策略收益。

### CHG-20260919-311：贯通经验条件、资源编排与失败恢复反思

- 依据：上一轮真实 v1 搜索中，动作规则确实拦截了重复视频请求，但 replan 又重新进入失败工作；编排草稿使用技能 prompt 中的短语，而规则匹配的是 run 输入；源经验中的“省略文件复核”也可能与任务明确要求冲突。另有两条训练试验因采集轨迹沿用 stdout 大小限制而评测异常。本条不改写这些历史评分。
- L1：`src/runtime/policy_runtime.cpp` 使用版本内改写后的适用条件生成注入正文，修复“按新条件检索、展示旧条件”。外部反思提示要求保留当前任务的核验和交付义务，同时改写条件及例外；有害经验可在候选版本内停用。没有用关键词强行判定自然语言义务，也没有改写私有／公共源记忆。
- L2：`src/runtime/schedule_evidence.h` 从同批动作字符串参数提取唯一共享的显式文件路径，生成只用于规则查询的 `__evolution_resource`，不改宿主参数。运行时、训练反馈、预检和父策略回放复用同一投影。URL、脱敏值、多共享资源歧义不用于猜测；原结构化资源参数继续支持。反馈另外提供实际 run 的 `schedule_plans.task_text`，避免从技能 prompt 生成不可能命中的任务条件。
- GEPA/L3：`src/gepa/gepa_search.cpp` 仅在同一 run／execution／actor／scope 存在已落定的终止性错误或重复失败及 replan／重复失败循环证据时，联合选择 `action_rules` 与 `loop_rules`，记录 `component_coupling=same_branch_failure_recovery`。不额外调用模型、不扩大控制权限，不把兄弟分支或在途任务串成失败链。反思可以共同优化重试拦截和分支恢复预算；实际收益继续通过原有预检、父子行为差异和评测门槛确认。
- 必要关联修复：`src/evaluation/task_evaluation.cpp` 将采集轨迹预算独立为 `max_trace_bytes`，默认 32 MiB、最多 64 MiB，按 trial 内全部 run 累计；原 stdout/stderr、模型输入、产物和报告限制不变。超限返回明确的 `evaluation_trace_bytes_limit`，不截断或伪造完整轨迹。未修改判分规则和发布标准。
- 混合批次边界：真实训练记录回放还复现了异常 trial 没有 checks／runs 时 `control_feedback_trial` 抛出 `array_required`，会中断同批正常记录的控制反思。缺省数组现在保留为空，携带原错误码、标记 `control_evidence_preserved=false`；异常不转为业务成功／失败，不授权新规则，不阻塞其他 trial 的正常证据。类型错误仍拒绝。补充专门回归用例。
- 验证：编译通过，34 组非 GEPA 搜索 CTest 和全部 104 项 GEPA 搜索集成用例通过；其中任务评测 104 项包含新增长轨迹预算用例。定向反例覆盖条件改写、两个独立生产／消费链、一条失败不取消另一条、默认与显式配置隔离、动作拒绝后分支停止、另一分支继续、跨 execution／scope／在途不联合。
- 真实数据回放：只读上一轮 seed 的 29 道已有训练记录，不调用 API。6 道（t-19、t-13、t-04、t-16、t-07、t-30）提供了同分支联合恢复证据；t-12 的已有技能参数可通过资源规则探针产生编排边。进一步调用正式 `Search::packet` 验证全部已有 seed 训练报告（含异常 trial），8 个批次压缩后仍可联合选择动作／循环组件，输入为 56,928–64,397 字节，未超过原 64 KiB 限制。探针验证的是数据贯通，不是新生成策略的性能或成功率。原轨迹、数据集、memory 均未删除或重算。
- 日志：`build/trajectory-evolution/l123-chain-regression.log`、`l123-chain-gepa-{1,2,3,4}.log`、`l123-chain-recorded-replay.log`。包内第 16、19、24 章同步说明条件、投影、预算及联合反思。
- v1 接入：实际 v1 Python 函数配合模拟 DataBus／模型／工具的多进程测试通过；2 个并发 worker、4 条轨迹校验通过，L1／动作／编排／loop 应用回执为 24／1／1／4。编排保持两波执行且不增加中间模型调用，采集器不可用时原流程继续。日志为 `build/trajectory-evolution/l123-chain-v1.log`；这是接入回归，不是远程 API 性能实验。
- 范围与检查：本轮未改 AgentFlow C++ 底座或 v1 业务代码，未改数据集和经验库管理。GitNexus 对 GEPA 主循环及评测路径给出 CRITICAL／部分未解析调用，已在修改前说明风险；UNKNOWN 符号结合真实调用方检查和回归验证，不作为无影响结论。尚未启动新的真实 API 搜索，不能将这些机制测试等同于已达到业务优化目标。

### CHG-20260919-312：隔离评测及时报告打点失败，避免已知采集异常空等

- 现象：完整 GEPA 中 t-14 两次在 480 秒超时，证据都只有前两轮；其中一次实时 daemon 日志显示第三轮已完成业务响应。历史原始临时日志随 trial 清理，尚不能确证那两次根打点丢失的具体原因，不将成功复测冒充根因证明。
- 确认的代码缺口：`IntegrationService::call` 将异常打点降级为 fail-open，但没有将采集失败传给隔离评测入口；根打点失败时还没有 run 可以记录 capture.gap。`EvaluationEntry::next` 对已标记 incomplete、尚未 closed 的 run 仍先等待终态，导致已知缺口一直拖到全题超时。
- 修复：仅在组件拥有的 entry 试验内，把当前轮异常记录为固定错误码 `evaluation_entry_capture_<code>`；发现 incomplete 立即返回 `evaluation_entry_trace_incomplete`。保留首个错误，拒绝内容不落盘；旧轮迟到打点不污染当前轮。线上业务 fail-open、普通业务失败、异步 ACK 不代表 run.end、真实终态落盘后才推进等原语义不变。不缩短业务超时，不伪造缺失轨迹，不增加模型调用。
- 范围：生产逻辑只修改 `src/service/integration_service.cpp` 和 `src/evaluation/evaluation_entry.h`；测试修改 `tests/entry_agent.py`、`tests/test_task_evaluation.py`，并补充包内第 19 章。未改 AgentFlow C++ 底座、v1 业务逻辑、数据集、源 memory、判分或发布门槛。图分析对新入口为 UNKNOWN，结合实际 HTTP/native 调用方和异步测试核验，未当作零影响。
- 回归：编译通过，任务评测 106 项全部通过；采集／HTTP／并发／SDK／运行时／GEPA 证据等 9 组 CTest 全部通过。四类异常（根打点被拒、根打点缺失后的 gap、中途 gap、子打点被拒）均在约 248–256 ms 返回明确错误，不再耗尽 5 秒测试预算；正常同步、异步、派发进程、原生与 HTTP 入口仍通过。上一轮迟到失败不影响下一轮。
- v1 接入回归：真实 v1 函数配合模拟 DataBus／模型／工具，2 个并发 worker、4 条轨迹通过；L1／动作／编排／loop 实际应用 24／1／1／4，编排两波执行且中间模型调用为 0，采集器不可用时业务继续。日志 `build/trajectory-evolution/entry-capture-fix-v1.log`。
- 真实 API 复测：同一 t-14、同一 seed L3、同一正式 service.json，2 个并发试次，每次 3 轮、两次图片生成、最终文件交付。两次业务均 passed，6/6 run 完整闭合，无采集／评分错误与警告；执行耗时 217.819 秒、235.685 秒，各 25 次 LLM 调用。模型评审无效 loop 为 9、11，说明本次修复验证的是采集收尾，尚不能宣称策略效率目标达成。未重跑 L0、未发布新策略。
- 产物：`trace_output/v1-l4-coldstart/l3-current/t14-capture-verification/report.json`；扫描该目录 19 个文件，未发现现有 .env 已知凭据原文及其 URL/base64/hex 编码。此检查不等价于证明所有隐私内容都不存在。旧正式 GEPA 及基线缓存、数据集和 memory 未改写。
- L2 核查：node 1 新编排规则在父轨迹预检可产生真实依赖边，但子策略实跑时图像和视频已经由模型分成两批，因此没有同批排序机会，实际应用仍为 0。没有为此放宽发布门槛、强行合批或把随机分数变化算成策略生效；这次未修改 GEPA 搜索策略。
- 日志：`build/trajectory-evolution/entry-capture-fix-build.log`、`entry-capture-fix-targeted.log`（实际运行完整 106 项）、`entry-capture-fix-regression.log`、`t14-capture-verification.log`。`git diff --check` 通过。

### CHG-20260919-313：反思对准实际应用与上层派发，验证失败正常收尾

- 效果复盘：真实 GEPA 节点 12 在 15 道验证题上 14/15 passed、无效 loop 均值 0.800、平均时延 55.966 秒；缓存 L0 为 14/15、0.800、55.801 秒，仍未达标。节点 12 的验证应用回执为模型/动作/编排/loop 19/1/1/2，不能再归结为所有策略未执行。7 次无效 loop 来自已完成后的占位派发，2 次来自视频失败后的再次派发，另有 2 次首次结果回传的评分疑点。
- L1 反馈：`src/gepa/gepa_search.cpp` 的 `packet` 原来用只识别 L2/L3 的控制证据函数排序 L1 样本，导致无关失败优先。现在依据获选经验的实际 applied 回执排序；沿用公共/私有经验的规范 ID 归并，不扩大授权，不把没有回执视为已应用。
- L2/L3 反馈：`control_feedback_trial` 在既有 16 项上限内优先保留已有结算轮次的 guard-only 重复派发机会，再按原顺序输出，避免上层派发被子分支大量重试挤出。没有新增远程模型请求，没有按题名或技能名硬编码规则。
- 稳定注入：`src/runtime/policy_runtime.cpp` 不再向模型正文写入随 run/版本变化的 policy_id；decision/receipt 归因字段保留。新增跨 run、无关控制改动、replan 正文一致性测试，以及旧包装回放兼容测试。
- 搜索收尾：验证目标未达标返回 `validation_rejected`，不启动留出集，不导出选中版本；清除本搜索目录中旧的选择产物。`src/evaluation/agent_evaluation.cpp` 仅补必要结果映射，作业为正常 `rejected`、`gate.stage=validation`，不把策略失败变为程序异常。评分、发布阈值及正常留出验收逻辑不变。
- 搜索实现指纹更新为 v5，避免与旧搜索检查点混用；独立 L0 缓存保持可复用。生产改动局限于上述 3 个源码文件，没有改 AgentFlow C++ 底座、v1 业务、数据集、源 memory 或任务评分器。
- 回归：最终构建通过，完整 GEPA 106 项（36＋35＋35）与相关 CTest 10 组全部通过，包括旧选择产物清理、正常拒绝、发布/导入和基线缓存复用；v1 真实函数配合模拟 DataBus/模型/工具，2 个并发 worker、4 条轨迹通过，模型/动作/编排/loop 实际应用 24/1/1/4。日志 `build/trajectory-evolution/feedback-final-build.log`、`feedback-final-gepa-{0,1,2}.log`、`feedback-final-regression.log`、`feedback-fix-v1.log`。`git diff --check` 通过。
- 对 CHG-312 的业务结论补充：两次 t-14 的 passed 是程序判分，不能等同人工确认全部交付；其中一次第三轮宣称更新 latest-look.md，但缺少实际写入和产物证据。采集 6/6 闭合仍成立；评测疑点交由对应协作范围处理，本轮未改评分来提高结果。
- 完整报告：`tools/trajectory_evolution/docs/2026-09-19-gepa-effectiveness-review.md`。本轮未重新发起真实 API 评测，尚不能证明以上修复已达到无效 loop 下降 15% 的业务目标。

### CHG-20260919-314：补齐紧凑反思证据和异步后续批次护栏

- L1：`src/gepa/gepa_search.cpp` 新增受既有字节预算约束的经验反馈投影。完整样本超预算时保留所选经验的实际 applied 回执、事件身份、有限检索状态与观察操作；同分支前后动作只作时间上下文，排除查询后才到达的结果和兄弟分支。私公重复映射校验批准版本与回执版本。无回执/缺轨迹/极小预算仍退化摘要，不伪造注入；不增加模型请求或输入上限。
- L3：反思的迭代预算证据读取父 runtime policy 的 `aggressive_controls`，修复误读 control_learning 导致机会被裁掉的问题；不更改控制权限或原配置。
- L2：`src/runtime/dispatch_runtime.cpp` 在每一 wave 派发前统一运行已授权的动作护栏，沿原计划继承 actor/scope/loop；过滤后正确映射 ACK，最终结果维持原序。修复首波有检查而异步恢复后绕过检查的缺口。L0、无 action.filter、显式 apply_policy=false 不新增干预。
- 取消语义：`orchestration.cpp` 支持 waiting→cancelled；`boundary_runtime.cpp` 记录未执行取消，不伪造 running、不扣失败或尝试预算、不虚报 schedule 已应用。取消仅影响要求前驱成功的后代，其他分支继续。全拒绝 ACK 使用 cancelled、空 accepted 和逐分支拒绝原因，避免外层派发误计为业务失败。
- v1 只移除 `coordinator_decide.py` 中原首波重复的 guard 打点，保留派发打点与函数签名，没有引入宿主判断器或额外文件。未改 AgentFlow C++ 底座、数据集、记忆管理、评分或发布门槛。
- 验证：最终构建通过，11 组相关 CTest、完整 GEPA 106 项（36/35/35）全部通过。新增 native/HTTP 14 个场景覆盖第二波拒绝、首波全拒、依赖取消、兄弟继续、乱序返回、重复回执、目标预算和权限隔离。v1 真实函数 fixture：4 条完整轨迹、2 个并发 worker，L1/动作/编排/loop 应用 24/1/1/4，编排两波间模型调用为 0。日志为 `build/trajectory-evolution/feedback-dispatch-{build,regression,v1}.log` 与 `feedback-dispatch-gepa-{0,1,2}.log`。
- 报告：更新 `tools/trajectory_evolution/docs/2026-09-19-gepa-effectiveness-review.md`，区分实际生效、业务收益和评分疑点，并明确错误图片经验注入晚于已标注浪费，不反向归因。第 16/24 章同步说明。
- 真实验证：经用户明确授权切换后台服务后，启动 job `903da290bc8b7a0530751aed5a3fedccb9c5733f44caa69e7f3e34f389844e60`，以旧节点 12 为新种子重新测；保留数据集、memory 和兼容 L0 缓存，不复用旧候选成绩。尚无新最终性能结论，不将回归通过视为下降 15% 已达成。
- 首阶段结果：种子验证 15/15 完成，14/15 passed，无效 loop 均值 1.800，平均执行 68.860 秒；模型/动作/编排/loop 应用 24/2/1/2。L0 缓存确实 hit，未重测。种子效率未达标，搜索已进入训练反思，尚未发布。完整报告第 11 节保留这次退化结果，不把调用覆盖等同收益。

### CHG-20260919-315：控制规则增量修改与反思规格一致性

- 真实问题：作业 `903da290…844e60` 节点 1–3 为重复版本；节点 4 新增 Coordinator 规则时用单项数组替换整个 loop_rules，误删 video-gen 的独立停止规则。训练轨迹中确认 4 次本应 stop 的位置变为基础 replan。节点 4 训练从父版 6/6、无效 loop 0.667、76.053 秒退化为 4/6、2.333、101.056 秒；验证 13/15、1.733、55.651 秒，未达发布条件。
- 候选构造：只在 `src/gepa/gepa_search.cpp` 增加 `control_updates` 的 add/set/remove 展开，未提及条目继承，显式删除才移除。索引统一针对父数组，先校验再展开；拒绝越界、非整数、重复索引、未授权组件及与 controls 混用。普通反思、COMBEE、能力修复和编排修复共用，旧 controls 整组替换语义保留；最终策略仍是原有完整格式，不改宿主 API 或底座。
- 反思规格：统一 `gepa-reflection.txt` 的 action 原语权限，明确动作护栏覆盖有实际 guard 的技能派发；消除“两种原语”与后续预算/终止原语冲突。明确 max_iterations=N 允许前 N 轮，不能声称 N=2 阻止第 2 轮。反思与分组汇总默认使用增量规则，避免复制整个规则数组；汇总不再沿用过时的固定迭代上限说明。
- L1 反馈：紧凑输入仅沿委派→批次结束→已执行 schedule.action 的直接因果链补真实结果，保留结构化成功/失败与来源事件；不把 ACK 当最终业务结果，不纳入无关批次、未来或未执行动作，保持原字节预算。
- 兼容：GEPA 搜索指纹更新为 v6；独立 L0 缓存键不变。未改运行时执行逻辑、AgentFlow C++ 底座、v1 业务、数据集、memory、评分权重或发布门槛。保留协作中的其他修改。
- 验证：完整构建成功；GEPA 旧 106 项（36/35/35）全部通过，新增 4 项真实组件链路集成通过，相关 CTest 7 组全部通过，包含新增证据与增量编译单测。新集成覆盖 single L3、COMBEE map/reduce、两类 schedule 修复，检查实际增量草稿、差分预检及运行时应用。`git diff --check` 通过。日志为 `build/trajectory-evolution/control-updates-{build,integration,regression}.log`、`control-updates-gepa-{0,1,2}.log`。
- 实验管理：经用户授权取消旧作业，保留已完成结果、数据集、memory 和基线缓存。取消时第 5 节点尚未反思；4 条已发请求均使用旧提示词，没有实际混用更新后的文本。修正版使用新搜索上下文启动，不回写旧节点成绩。
- 复盘：完整报告 `tools/trajectory_evolution/docs/2026-09-19-gepa-effectiveness-review.md` 第 12 节补充生成器错选能力、规则误删、L1 证据缺失和评分不一致的区别。t-13 收尾模型在移除工具声明后仍提出工具计划，最后退回通用停止答复；本次未把模型不遵从误报为正常交付，也未改变评分掩盖失败。
- 独立复核又发现组件修复阶段残留旧的 controls.* 输出指令，已同步统一为 control_updates。短暂启动的 `9ce57400…dfb690` 在 0 次反思时取消，不作为修正版优化成绩。最终正式作业为 `ef21fe0015ecb5cc1152142570b40ae037e088e97dfc948b08e4d9148dfbfa20`，后台独立进程 PID 3470918，沿用同一数据集、memory、种子与 L0 缓存；启动时进入种子验证，尚未宣称效果达标。

### CHG-20260919-316：统一增量和分组反思的有界纠正入口

- 真实问题：`ef21fe00…bfa20` 节点 1 的越权增量在修复 try 前被拒绝，绕过既有单次组件纠正；节点 2 返回裸顶层规则数组，`unknown_field` 未进入纠正且被落盘为泛化错误。节点 2 实质复述父策略，修复格式不等同产生改进。作业已按用户授权取消，保留结果、数据集、memory 与 L0 缓存。
- 最小修复：`src/gepa/gepa_search.cpp` 将 propose、增量展开、契约修正与 mutate 纳入同一 try。COMBEE 用内部输出引用保留实际失败子草稿，来源修复补父 components，索引不漂移；成功路径与最终策略格式不变。不重跑已完成分组，不扩大所选组件或原语权限。
- 错误处理：`unknown_field` 和 `gepa_draft_invalid` 复用同一个单次、受模型/时间预算约束的纠正；修复再次错误立即拒绝。固定错误码补 `unknown_field`，不记录任意字段文本。敏感内容错误不纳入纠正；不增加自动发布或放宽质量门槛。
- 提示词：新增 `repair_draft_format` 说明。裸数组不推定整组替换或删除未提及父规则，只有明确的增量对应才修正，否则保持父版本；只有已有合法 controls 才保持旧整组替换语义。搜索实现指纹 v7 与旧检查点隔离，独立 L0 缓存不变。
- 范围：生产修改仅包内 GEPA 源码与反思提示词，另更新 `tests/test_gepa_search.py`、第 24 章、效果报告第 13 节和本记录；没有改 AgentFlow C++、v1 业务、数据集、memory、评分或发布阈值。新增回归覆盖普通/COMBEE 越权、格式错误、重复失败拒绝、预算耗尽与父组件传递。
- 验证：旧二进制已复现增量纠正缺失的 RED，新二进制完整构建通过。新增 9 项回归全部通过（51.774 秒），旧组件/来源纠正 6 项通过；完整 GEPA 119 项分 40/40/39 三组，116 项直接通过，3 项因旧测试断言混计 L1/L2 请求失败，修正断言后单独复测 3/3 通过（15 秒），生产代码未因该测试计数问题改变。关联 CTest 7 组全部通过（74.87 秒）。日志为 `build/trajectory-evolution/control-repair-{build,new-tests,regression,format-final}.log`、`control-repair-gepa-{0,1,2}.log`；`git diff --check` 通过。
- 影响核查：图分析对新增函数为 UNKNOWN；propose 上游返回 CRITICAL，已提示并核查唯一直接 Search.run 调用；error 精确文件消歧后为 LOW（run/validate 两处）。独立代码复核未发现授权、预算、父策略污染或敏感内容转发旁路。格式/来源纠正仍须经真实效果评测，不宣称语义等价已获证明。
- 修正版真实作业：`cfe00062ec49258e9834d8fd343dbd443a50f963b6ec60962214acc247f8e9b9`，后台独立进程 PID 3533062。旧服务已停止；新服务沿用原 .env 运行环境、同一 L3 种子、数据集、memory 和 L0 缓存，4 题并发，从新搜索上下文开始。已确认进入种子验证，无额外宿主代码修改；尚无达标或发布结论。
- 种子验证结果：15/15 完成，13/15 passed，无效 loop 均值 1.933、平均执行 55.574 秒，模型/动作/编排/loop 实际应用 21/1/1/2。L0 缓存确认 hit（14/15、0.800、55.801 秒）。全部能力有实际回执但收益未达标，已进入第 1 个训练候选，不发布种子。

### CHG-20260919-317：修复提醒截断经验状态，预检无效删除并精简反思视图

- 依据：真实作业 `cfe00062…e9b9` 节点 1 删除从未命中的规则、节点 3 改写经验却未注入，均没有父子实际行为差异。已取消实验，保留数据、memory、结果及有效 L0 缓存，不继续空耗 API。
- L1：`src/runtime/boundary_runtime.cpp::activity_query_state` 在推进消息切片边界前复用既有检索过滤器；纯预算/系统提醒不再把待消费的真实工具结果切掉。真实新用户输入仍隔离旧结果，显式 state 保持优先，原模型输入和轨迹不变。没有修改检索阈值或增加框架特判。
- GEPA：`src/gepa/gepa_search.cpp` 为纯删除生成控制规则增加父训练轨迹上的反事实预检，比较实际行为并核对回执；无差异跳过子评测。混合 L1 正文、条件或停用变化不走纯删除捷径；新增规则原有门禁、评分和发布标准未改。
- 模型输入：仅保留 selected_components 对应原语/基础规则；纯 L1 请求去掉无关控制配置。普通反思和 COMBEE 复用同一裁剪，合法来源、限制、训练反馈不丢失，授权检查仍独立执行。
- 版本：搜索指纹升级 v8，并同步普通策略评测的检索语义指纹 v6，避免非 GEPA 入口复用旧边界结果；L0 独立指纹保持不变。未修改 AgentFlow C++ 底座、v1 业务、数据集、源记忆或业务评分；代码集中在包内 runtime/gepa、评测缓存指纹一行及对应测试，保留工作区其他协作修改。
- 二次复核：COMBEE 控制子请求裁剪经验组件前，将当前有效正文移到合法来源项，防止误删依据；历史 original_content 不替代当前正文，停用项不恢复。对应回归检查 string/object/null、重复投影、父请求不变。
- 验证：完整构建成功，最终 124 项 GEPA 四组回归直接通过；新增 COMBEE 用例原本同时触发 L1/L2（3+6 次请求），限定 L2 后严格验证 4 map+2 reduce，单项通过，合计 125 项。关联 9 组 CTest 通过：普通评测测试预期同步 v6 后全 21 项重跑通过，其余 8 组直接通过。未放宽缓存隔离、应用回执、评分或发布断言。日志 `build/trajectory-evolution/effective-input-final-{gepa-0,gepa-1,gepa-2,gepa-3,ctest,evaluation}.log`；初版 ctest 日志保留旧测试预期失败，最终评测复测见 evaluation 日志。
- v1 接入回归：实际 v1 函数配合模拟 DataBus/模型/工具，2 个并发 worker、4 条完整轨迹，L1/动作/编排/loop 实际应用 24/1/1/4；prepare、report 同波，image 后波，中间模型调用 0。异步失败计数、独立分支、不可用采集器及隐私断言通过。日志 `build/trajectory-evolution/effective-input-v1.log`；这是接入回归，不是远程 API 性能结果。
- 新真实作业：`7e5e636961ad1996ad6cd7f4ed4bf6e39dcbf7dafbe052a6fc893f06aa8cd5cb`，后台 PID 3637980，旧服务已停止。沿用原环境凭据、数据集、memory、种子与兼容 L0 缓存，4 题并发；启动时进入 15 题种子验证。报告第 14 节与包内第 24 章已更新，尚无达标或发布结论。
- 种子验证已完成：13/15 通过，无效 loop 均值 1.867，平均 Agent 执行 62.461 秒，L1/动作/编排/loop 实际回执 38/0/1/2；兼容 L0 缓存再次确认 hit（14/15、0.800、55.801 秒）。旧种子仍不可发布，进入节点 1 训练；修复注入链路不等于经验有益，不将随机波动或回执增加解释为收益。

### CHG-20260919-318：将派发证据绑定到实际操作，保留纠正证据并处理重复循环草稿

- 依据：`7e5e6369…cd5cb` 节点 7 引用 forge-image-gen 的 guard 轮数 2，却生成 `operation=run_skills`；包装调用不结算子技能预算，真实轮数为 0，因此预检正确拒绝。节点 9 对已有 video-gen 循环规则再次 add 同预算，触发 `control_duplicate_loop_scope`。作业已在节点 10 期间取消，保留结果、数据集、memory 和 L0 缓存。
- 输入：`src/gepa/gepa_search.cpp::observed_operation_bindings` 只为已开放且本轮获选的 operation_budget 添加最多 4 个真实 guard 绑定，包含操作、scope、已落定轮数及当前反馈引用。同身份取真实最大观察、不跨 Run 相加；移除旧绑定后重新计算，仅用原字节预算剩余空间，不裁掉原反馈。普通、COMBEE 和纠正请求共用，实际 HTTP 输入与 request.json 使用同一对象。
- 纠正：`repair_feedback_row` 在 action_rules 获选时优先保留真正放行、未执行且已有落定轮数的重复派发证据；否则沿用原来的相关行/首行。仍只带一行，不增加模型请求上限。重复 scope/actor 的循环草稿复用一次格式纠正，仍受原调用预算和 max_attempts 控制，第二次错误立即拒绝。
- 提示词：说明操作预算统计成功和失败的落定尝试，计数不能移给外层委派；重复循环草稿只有唯一父索引且有实际预算/条件差异时改用 set，否则保持父策略。未改变原语权限、组件选择器、运行时计数、规则校验、评分或发布门槛，不向 run_skills 加全局预算。
- 范围：生产修改仅 GEPA 源码及反思提示词，另补测试、第 24 章和效果报告。搜索指纹 v9；普通策略缓存 v6 与独立 L0 缓存指纹不变。没有修改 AgentFlow 底座或 v1 业务，也没有用验证/留出内容设计规则。
- 验证：完整构建、130 项 GEPA 回归（原 125 项四组 + 新增 5 项）与关联 9 组 CTest 全部通过。新增测试核对实际 HTTP 用户输入与 request.json 完全一致、绑定引用和 32 KiB 预算，以及重复循环纠正/重复失败拒绝/预算不足/禁用纠正。初版 4 个测试因同时配置 actor_id/target_scope 无法启动，修正 fixture 后 5 项全组重跑通过，未放宽生产断言。日志为 `bound-operations-{build,new-final,regression}.log` 与 `bound-operations-gepa-{0,1,2,3}.log`（均在 build/trajectory-evolution）。测试专用构建前后 live CLI/local.so 哈希未变；旧作业取消后才修改提示词并全量构建。
- 修正版真实作业：`a1d722e4a70feebee0772b06b558b9bb119d625945cc3de968dc2c150a3e166b`，独立后台 PID 3790195。从上一轮保留的节点 5 完整策略继续搜索，4 题并发，同一数据集、memory、环境与兼容 L0 缓存，不续写旧候选成绩。启动时进行起始策略验证，尚无可发布结论。

### CHG-20260919-319：按完整轮次评审识别控制机会，修正搜索方向

- 依据：上一作业已取消，15 题起始策略无效 loop 均值仍为 1.200，高于 L0 的 0.800。流程较长被误作 L3 机会、动作反馈丢失轮次评审、已探索控制机会被跳过是本次修复对象；没有重跑 L0。
- 实现：`src/gepa/gepa_search.cpp` 在裁剪前按规范化 run/execution/loop/iteration 绑定完整原始评审，添加小型 iteration_assessment 与 reviewed_waste_after。必要后续工作、未知评审、跨实例均不混算；全空转分支仍允许保留最低轮数后的上限探索。结构无进展/恢复证据保持独立。当前父策略的控制授权来自当前 packet，不借用旧 seed。
- 路由：round_robin 当前控制无机会时，按实际 component_selections 在其他有证据的控制中选择，允许再次探索；有效当前组件与 all 保持原意。辅助探测仅跳过 gepa_feedback_budget_too_small，其他错误仍抛出；不增加模型调用或输入预算。
- 范围：GEPA 源码、反思提示词、相应测试与第 24 章/效果报告。搜索实现指纹 v10，在线 L3 阈值、宿主 v1/C++、记忆、数据集、评分与发布门槛不变。原有协作修改保留。
- 验证：旧 v9 已复现 3 项新增端到端失败；新版本完整构建、133 项 GEPA 四组回归和关联 9 组 CTest 通过。最终补齐纠正反馈 controls 后，C++ 证据回归与 8 项纠正回归再通过。测试初版初始化语法与夹具噪声问题已修正，未放宽生产校验或核心断言；日志为 reviewed-opportunity-final-gepa-{0,1,2,3}.log、reviewed-opportunity-regression-final.log、reviewed-opportunity-repair-final.log（build/trajectory-evolution 下）。
- 真实训练诊断：t-12 的反思仍将 forge-image-gen 的预算误写到 run_skills，预检拒绝、没有执行子评测；t-25 首次反思返回笼统调用错误，重试后又把 outfit-stylist 的浪费归给 Coordinator，返回无改动。产物在 trace_output/v1-l4-coldstart/train-probe-v10；这是暴露错误的诊断，不是收益验证。需要进一步把真实对象做成程序校验的可选绑定，并保留固定错误码/耗时以区分调用故障；未恢复完整 GEPA、未重跑 L0。

### CHG-20260919-320：校验反思的控制对象绑定并记录安全调用诊断

- 依据：仅靠 observed_bindings 提示仍出现真实模型错认操作与执行者。新增 request-local editable_bindings，模型选择绑定、同 scope 来源、预算和理由，程序还原既有 control_updates；没有更改运行时或宿主协议。只覆盖已有 operation_budget、bounded_iteration_cap，其他原语、删除、旧 concrete 格式保持可用。
- 约束：绑定来自本次已授权组件的真实控制证据，最多 6 项、仅用输入剩余预算；不跨 Run 累计次数，不把同名不同 scope 规则覆盖。唯一同对象规则 set 并保留其他条件，歧义拒绝；新 loop 规则继承匹配基础规则。既有更紧预算不能被该简化更新放宽。ID 随请求/阶段变化，COMBEE map 各自解析，reduce 保持具体规则协议。
- 纠正：control_binding_invalid 进入现有一次有界纠正，保留当前紧凑训练反馈、重新生成绑定；不扩大模型总预算。绑定与 concrete 不能在同份草稿混用。预检、执行回执、评分、发布门槛均保留，没有把绑定通过当作收益。
- 诊断：反思返回后保存 transport_ok/error_code/attempts/duration_ms/decoded_body_bytes，再执行原错误检查。错误码严格白名单与数字范围过滤；不新增响应正文、地址、模型标识、凭据或异常原文。没有更改重试和超时。
- 范围：生产仅 GEPA 源码和反思提示词；另补 C++/HTTP 测试及包内说明。搜索指纹 v11，独立 L0 缓存不变。统一构建、136 项 GEPA 回归和关联 9 组 CTest 已通过；新增 C++ 测试初始化括号已修正，原失败构建日志保留。
- 真实验证：train-probe-v11 中两条真实 v1 训练题均完成，但新增 L2/L3 规则实际应用次数均为 0。即使 t-25 的无效 loop 从 8 降到 0，也不能归因于新增 L3；t-12 预算选择 2，错过 prior=1 的冗余派发。再次仅修改提示词仍选 2，因此不把这些结果报告为有效进化。

### CHG-20260919-321：修正早期预算、经验停用与条件化恢复的因果链

- L2：在完整评审、展示裁剪之前寻找同一 Run/scope/operation 的较早浪费后缀；需要所有相关 guard 的评审明确，后续无必要工作，且预算不会覆盖同计数的必要并发调用。typed binding 优先此真实位置，旧 observed_bindings 保留最大计数，不跨 Run 累加。
- L1：策略支持单独停用经验注入，保留来源批准供已编译 L2/L3 使用；不删除或改写源 memory。停用/恢复属于策略版本，可持久化、回放。停止要求模型同时删除引用经验的控制规则。
- L3：增加可选的同分支精确故障码和已验证动作拒绝条件。成功进展清除陈旧故障，pending 与兄弟分支继续保护。旧未配置条件的策略行为保持兼容；新候选削减重规划预算必须携带可执行条件，不能只在 reason 描述特定错误。
- GEPA：训练筛选另检查实际父子行为差异；无差异候选记录 unobserved_changes，不将随机收益算为策略收益或继续消耗完整验证预算。现有质量、无效 loop、时延和发布门槛不变。
- 范围：包内 GEPA/runtime/proto、提示词、对应回归及文档；本轮不改 v1 业务逻辑、数据集、评分或源经验库，不重跑有效 L0。搜索指纹 v12。
- 验证：统一构建和 9 组关联 CTest 通过；GEPA 原 137 项中 136 项通过，旧无条件恢复测试按新约束拆成拒绝/真实有条件执行两项，复测通过；现 138 项用例均已覆盖通过。最终又运行 3 项条件化恢复/消融端到端回归、9 组关联回归（75.11s）。日志 causal-boundaries-{gepa-*,final-gepa,final-regression,final-build}.log 位于 build/trajectory-evolution。
- 边界修复：并行预留的 null 失败项安全跳过；读取 prior 不再写入空项绕过容量；错误码/拒绝数有界校验；成功批次在 commit 按 check 快照清除旧码，不受成功/失败返回顺序影响，也不清除 check 后到达的新失败。
- 真实验证已启动：作业 2b608e71a844dd88f73856d9a4542e34fede77b6326dc1b93f7e634d00045109，真实 v1、4 题并发、原 60 题数据集和原 API。复用有效 L0，先评测已有 seed 再进化；结果未完成前不宣称达到收益目标。
- 真实运行发现自动消融原仅覆盖 same_parent，而实验使用 independent；暂停同一作业补齐独立采样入口。按父版本和经验去重，支持检查点恢复，不变更评分、配置、既有已完成节点或 L0 缓存。后续只对改动链路做针对性回归，跨模块改动或发布前再做全量回归。
- 针对性验证：发现并修正去重扫描提前读取 seed 的 parent:null 导致 integer_required；跳过非消融节点后，3 项独立采样复测通过（27.973s），证据/检查点单测通过；原有 same_parent 与中断续跑已通过，不重复执行。最终日志 independent-ablation-final-{build,regression,evidence}.log 位于 build/trajectory-evolution。使用 retry 恢复上述同一真实作业，未重提作业或重跑 L0/已完成 seed。

### CHG-20260919-322：保护有效探索机会，提前排除无归因收益，连接安全收尾

- 真实依据：上述作业节点 3 的 L1 已有真实注入与浪费证据，却因“未探索编排优先”改道后无改动。节点 5 的 6 题汇总看似改善，但唯一确认输入差分的 t-07 从 2 个无效 loop / 34.13s 退化到 6 个 / 57.22s，其余题随机改善使旧筛选继续消耗验证预算。暂停同一作业，保留已完成记录与 L0 缓存。
- L1/GEPA：已实际应用的经验保留轮转机会，未应用时仍可让位；训练筛选同时检查全批与真实差分子集，沿用原 strict/equal/pareto 语义；旧 screened 节点恢复后在进入验证前复核，不改原评分与发布门槛。
- L3：增加 require_finalization / can_finalize 能力门槛；宿主显式开启且包内有收尾模板才可命中，缺能力回退基础规则。GEPA 用完整原始轨迹识别“连续浪费后只有一次无工具最终答复”的尾部，为可收尾的实际浪费轮提供有条件预算绑定；未完成、工具调用、未知格式、跨 Run 与不支持收尾均不据此截断，不降低 min_iterations。
- 范围：GEPA、runtime、协议字段及对应测试/说明。AgentFlow 宿主、底座、记忆、数据集与评分不变；旧无条件字段的策略兼容。仅执行受影响链路回归，不将构建通过等同于效果达标。
- 验证：CLI/local.so、证据与策略运行时构建通过；GEPA 证据、PolicyRuntime 通过；6 项路由/训练筛选 HTTP 回归分组通过（24.249s、17.679s、21.282s）。新增集成测试修正整数类型及轨迹发布等待后通过（19.35s），未因此修改生产逻辑。日志 effective-opportunity-* 位于 build/trajectory-evolution。通过 retry 恢复同一作业，不重跑有效 L0 或已完成起始策略。

### CHG-20260919-323：合法无改动后探索有依据的受限控制候选

- 范围：生产只改 `src/gepa/gepa_search.cpp`，另补证据单测和第 24 章说明；不改 runtime、AgentFlow 打点、记忆、数据集、评分或发布门槛。
- 实现：模型合法返回但没有行为变化时，最多补一个当前已授权的控制假设。L2 使用完整评审识别的冗余后缀预算；L3 只使用 require_finalization 的安全收尾绑定，不将普通长流程强制截断。已有有效提案、L1 消融不被覆盖。
- 来源：优先使用现有同操作/actor 规则的批准来源，否则必须核实同执行、同 actor/loop/iteration 的真实 L1 查询、已执行模型与应用回执，并匹配当前批准 revision；不任意引用同 scope 经验。`control_probe` 明确记录训练评审假设、来源关联及 evidence_ref，保留原模型 draft。
- 边界：稳定键按父版本与可执行内容去重，恢复或请求内 binding ID 变化不会重复假设；继续使用既有 resolver、编译、预检、真实训练和因果筛选。不新增远程模型调用，不提高原输入或调用预算。
- 验证：新增来源隔离、未评审拒绝、有效提案保护、去重恢复和 L3 条件保留断言；修正缺少宿主基础 loop 的测试夹具后，定向 GEPA evidence 回归通过（0.55 秒），仅增量构建 CLI，未运行全量回归或重建 native。日志为 build/trajectory-evolution/reviewed-control-probe-{build,regression}.log。
- 实验接续：节点 7/8 完整验证结束、节点 9 合并筛选结束后取消并 retry 同一作业；已完成成绩、数据、memory 和 L0 缓存保留。新服务 PID 4116189。此时节点 7/8 的无效 loop 仍分别为 1.200/1.133，高于 L0 的 0.800，没有发布达标结论。

### CHG-20260919-324：保护必要执行、恢复经验探索并精简纠错

- 真实依据：上一作业最终 validation_rejected；节点 7/8/11 平均无效 loop 为 1.200/1.133/1.333，未达到 L0 0.800 的下降目标。t-26/t-30 同 actor 的首次必要执行与后续冗余执行不能共享过低上限；安全收尾识别未读取规范完成字段；停用经验的恢复轮次被让位逻辑抢走。
- L3：从完整训练证据计算 scope/actor 的必要轮数下界，裁剪和缺少 loop 行时仍保留必要 action；安全收尾仅豁免已确认的纯末尾答复。新增/收紧 cap 的真实查询预检拒绝截断必要步骤，继承规则兼容不变。完成状态读取 outcome.execution_status，并兼容旧 status。
- L1：停用经验保留轮转探索恢复的机会；普通未应用经验仍可让位给控制机会，不强制恢复或放宽效果验收。
- 纠正：绑定统一给出合法预算范围，无解绑定不发送；记录固定原因码和更新下标，复用原一次修复，不增加模型调用或保存敏感响应正文。
- 范围：生产只改 src/gepa/gepa_search.cpp，另补定向测试和说明。搜索指纹 v13，独立 L0 缓存不变；不改 runtime、v1 打点、评分、数据集或源 memory。下轮搜索预算设为 24 轮/24 次反思，原 300 trials、5 小时与质量/loop/时延发布门槛不变。
- 验证：仅增量构建 CLI 和 GEPA evidence；5 项 HTTP 定向测试及 GepaEvidenceTests 通过（证据测试 0.87 秒），未重建 native 或跑全量回归。恢复测试初版缺 allowed_conversations，修正测试绑定后通过；旧 cap1<必要 keep2 断言按新保护语义改为拒绝。日志 required-work-{targeted,targeted-final,evidence-final,build-final}.log 位于 build/trajectory-evolution。首次 CTest 名称匹配错误没有执行用例，已使用准确名称和 --no-tests=error 重跑确认。
- 真实验证：新作业 c4f3f6f51b93faa4dd84a78e68f089890eff3a88b50f6f3e67a2373da71b6792，后台 PID 4158466，从旧作业节点 7 的完整 L3 策略继续探索；旧节点未被宣称为达标版本。同目录、数据集、memory 与独立 L0 缓存保留，不因这批 GEPA 修改重新执行 L0。结果需等待真实运行，未降低发布要求。

### CHG-20260919-325：隔离新请求检索、归并重复经验并复用有效评测

- 真实依据：c4f3f6f5…6792 起始策略 15 题为 14/15、无效 loop 0.867、时延 46.206 秒；新增 outfit-stylist 派发预算的节点 2 为 14/15、1.067、50.174 秒，均未达到 L0 0.800 再下降 15% 的目标。t-14 的重复派发被拦截，但产生请求的模型轮次仍是浪费，不能把少执行一次 skill 等同于少一次无效 loop。节点 3 的 t-09 入口退出 137、评测不完整，未计为业务失败。发现检索问题后取消本作业，保留证据、数据、memory 和 L0。
- L1 检索：消息封装只按最后实质 user 及后续本轮内容构造 task 检索视图；新请求不再被历史助手“已经完成”触发。当前引用、工具状态、显式 state、完整模型输入和原始轨迹保留；普通文本/未知格式兼容。不改 Bridge、v1 业务或底座，包内打点与直接查询共用处理。
- GEPA 经验：修复公共 PublishedExperience hash 与 ShadowExperience hash 直接比较的错误。搜索轴、紧凑反馈、策略合成与应用统计共用双方批准且版本固定的别名；合法重复公共经验归到私有项，独立公共项保留，不按文本相似度擅自归并，不删除 L2/L3 的公共批准来源。
- 加速：搜索上下文和候选真实执行上下文分离。完整 train/val 报告可按精确版本、题目集合和执行/评测条件跨作业复用，沿用已有 resume_report 校验；只保存摘要/相对路径，环境值仅参与摘要。holdout 不共享，坏缓存按 miss，当前评分及门槛重新计算。检索语义指纹升 v7/current-turn-v1，旧 L1–L3 成绩隔离，L0 指纹不变。
- 验证：仅增量构建 CLI/native 和相关测试。GepaEvidenceTests、ShadowRuntimeTests、IntegrationServiceTests 通过（共 19.48 秒）；跨搜索缓存 HTTP 测试通过（7.493 秒，验证原 trial_id/时延保持和新执行成本 0），L0–L3 评测发布重载测试通过（5.100 秒）。日志 current-turn-{build,cpp,cache-http,levels-http}.log 位于 build/trajectory-evolution。未运行全量回归，尚不把测试通过宣称为真实收益达标。
- 缓存边界补齐：显式宿主入口文件内容加入候选/作业指纹，排除本包 CLI 的纯重链接变化；动态依赖仍遵循 agent.revision 契约，L0 指纹不改。追加 3 项定向回归全部通过（26.444 秒）：入口文件改变失效、损坏产物恢复转重跑、搜索配置变化复用有效成绩。未重复全量测试；日志 current-turn-cache-{build,boundaries}.log。
- 实验接续：新作业 a3ce77e8861cbf9b4f0306f70d317f4b35a12d531f34982a0af130038ee968d6，后台 PID 24556，同目录、同数据和 memory、同起始策略继续真实 v1 搜索。仅当前运行语义变化使旧候选成绩隔离，不重新测有效 L0；结果尚在执行，不宣称达标。

### CHG-20260919-326：隔离终态配置错误，避免误伤同模型兄弟分支

- 真实依据：a3ce77…e968d6 起始策略验证 15/15、无效 loop 0.467、时延 40.118 秒，相对有效 L0 数值改善，但尚未通过最终发布。训练 t-30 中两个独立 video-gen 分支同用默认模型；海豚返回 UnsupportedModel 后，宇航员首次 generate_video 被全 run 配置失败计数拒绝，导致业务失败。已暂停该作业，不将较好验证成绩掩盖并发缺陷。
- 最小修复：terminal_failure 的账本身份加入可信执行分支；同分支同配置的真正重试仍拦截，兄弟分支首次独立执行。缺少可信身份不推断全 run 故障；schedule 子动作使用各自唯一动作身份，不能共用父执行域。普通 failure_key_arguments 与 operation_budget 身份/预算保持不变。
- 一致性：包内 runtime 的开始/实际输入核验、GEPA 控制预检、父策略回放使用同一身份；不改 v1、底座、数据集、记忆或评分。反思提示词和说明同步限定作用范围。候选执行指纹增加 terminal-branch-v1，GEPA 指纹 v14；独立 L0 保留。
- 验证方式：仅增量构建并运行分支隔离、目标重试回放与上下文指纹相关测试，不重复全量回归。真实训练的历史检索本地复放另外确认 4 处错误完成经验均不再注入，原完整输入和 state 未改变；该复放不代表整体收益证明。
- 验证结果：CLI/native 和相关测试构建通过；GepaEvidenceTests、IntegrationServiceTests 均通过（合计 19.46 秒）；同目标跨执行重试及归因 HTTP 回归通过（7.270 秒），评测上下文/各级发布重载回归通过（5.052 秒）。首次独立 CTest 命令缺少开发环境 PATH 未执行测试，随后补齐 PATH，准确匹配两组并使用 --no-tests=error 重跑通过。日志 terminal-branch-{build,cpp,target-http,context-http}.log。
- 真实复测：新作业 e64d11cdad5dd991a5ee39774ab2a680f0b642572dfeec45cd1f7be62c0760fb，后台 PID 51742，仍在原 coldstart 目录；数据、memory、起始策略与有效 L0 不变。等待真实质量/无效 loop/时延结果，不将定向测试等同于收益达标。
- 已完成的真实证据：e64 起始策略 15 题为 14/15、无效 loop 1.067、时延 48.955 秒，尚未达标；L0 确认缓存命中。训练 t-30 通过，两个独立 execution 的 generate_video 在 event-60/event-74 均 executed=true，分别如实记录 UnsupportedModel，不再把第二个首次调用拦截。GEPA 后续搜索仍在执行，未发布新版本。

### CHG-20260919-327：缺恢复条件的提案复用已有一次纠正

- 真实依据：e64 节点 4 在编译后预检发现恢复预算降低但没有执行条件，直接以 recovery_condition_required 结束；原一次模型纠正只覆盖生成/编译错误，未覆盖这一类预检缺口。拒绝无条件规则本身正确，不应放宽预检。
- 最小修改：只改 src/gepa/gepa_search.cpp 和对应测试。尚未用过纠正、原尝试/模型预算充足时，复用 repair_control_capability，携原草稿与精确诊断。只选问题规则同 scope/actor 且包含真实错误码或拒绝事实的一条训练反馈；没有证据或超原输入预算时保持拒绝，不自动猜填条件。
- 边界：保留 preflight_before_repair，纠正后仍重新解析、编译和完整预检；已进行来源/格式纠正的节点不能再次纠正。模型可合法保持父策略不变，不增加额外评测。运行时、v1、数据集、memory、评分和发布门槛均未改。
- 验证：仅增量构建 CLI，7 项 HTTP 定向测试全部通过（40.657 秒），覆盖实际生效、重复错误、尝试预算、总模型预算、已使用其他纠正、合法弃权及原有条件规则。未重建 native、未跑全量回归，日志为 build/trajectory-evolution/preflight-recovery-{build,targeted}.log。
- 效果边界：条件合法不等于减少已经发生的无效模型轮次。e64 节点 3/5 的完整验证均未达发布目标；细节见包内 2026-09-19-gepa-effectiveness-review.md 第 20 节。有效 L0、已完成评测及当前搜索树保留，不为此重复基线。

### CHG-20260919-328：以实际应用回执约束 L1 反思归因

- 真实依据：e64 节点 5 的反思声称经验 5904… 在训练 t-29 导致图片工作失败；但该题实际回执只有 13c35… 和 c90bc…，5904… 只在 t-07 应用。不是公共别名错误，也不是压缩漏 ID；未应用获选经验的 t-29 完整轨迹占 63,649 字节反思输入中的 37,450 字节，淹没了正确应用证据。
- 最小修改：仅 GEPA 的 L1-only 反馈加入 selected_experience_applications；按真实 advice.applied 回执及批准 revision 核对，记录 run/receipt、canonical 来源，别名保留实际来源和版本。最多 8 个定位点，另存总数和截断标志；只有查询、被拒绝、旧版本或无定位身份均不作实际应用证据。
- 精简：有真实应用样本时，未应用获选经验的题只保留短对照并明确 not_observed，不发送该题大段执行轨迹。全部未命中或经验停用待恢复时保留原反馈，不关闭恢复/条件改写的搜索机会。控制或混合组件选择不变；不增加模型调用或输入上限。
- 范围：src/gepa/gepa_search.cpp 及 tests/test_gepa_evidence.cpp 两项定向测试；不改运行时、v1、源 memory、评分、门槛或既有搜索节点。保留同作业接续兼容及有效 L0，避免重新计算已完成的 19 份评测。
- 验证与接续：仅增量构建 CLI 和 GEPA 证据测试，GepaEvidenceTests 通过（1.23 秒）；未重复 CHG-327 已通过的 7 项恢复测试，也未运行全量回归。日志 experience-attribution-{build,targeted}.log 位于 build/trajectory-evolution。已由后台 PID 138675 通过 retry 接续同一 e64 作业的节点 10；核对 19 份完整评测的 attempt/report_hash 均未变化，模型调用仍从 7 次继续，L0 缓存保持 hit。尚未达到整体发布目标。

### CHG-20260919-329：明确当前循环预算并开放已评审普通 cap 的候选路径

- 训练依据：e64 节点 11 将候选范围 min=max=3 误读成当前上限已是 3，实际上父策略对应的基础上限为 12；同时自动控制假设只接受 require_finalization 的循环绑定，遗漏已有浪费评审依据的普通 cap。
- 最小修改：仅 src/gepa/gepa_search.cpp 和 tests/test_gepa_evidence.cpp。循环绑定 budget 增加可确认的 current；与候选 min/max 分开。同 actor 的唯一生成规则优先，否则匹配基础规则；条件覆盖、来源不确定或多重匹配时省略 current，不串用其他 actor。
- 普通 cap 复用现有候选解析、来源关联、必要步骤下界及完整预检；仅父子行为相同且已有 reviewed_waste_after 时提出一次有界假设，不覆盖模型真实提案。去重键按真实 require_finalization 区分普通上限与收尾控制，不重复相同假设。
- 约束：不凭高轮次数制造浪费证据，不把任意同 scope 记忆当来源。节点 11 的训练分支缺少实际 L1 来源关联，仍可能被来源检查跳过；本修复不保证该节点必定产生可用规则。未改 v1、运行时、数据集、memory、评分、门槛及基线缓存。
- 验证：增量构建 CLI 和 test_gepa_evidence；仅 GepaEvidenceTests 通过（1.00 秒），覆盖普通 cap、收尾标志、去重、无来源、必要步骤、未评审、当前预算及条件覆盖。未重复 HTTP 回归、未跑全量测试；日志 build/trajectory-evolution/l3-budget-probe-{build,test}.log。真实效果仍待原实验接续验证。
- 接续：节点 16 完整验证结束后暂停，由后台 PID 185503 加载修正版并 retry 同一 e64 作业，从节点 17 继续。核对 33 份完整评测 attempt/report_hash 全部不变，L0 缓存仍 hit；未改写先前生成的节点。节点 16 的 14/15、无效 loop 0.400、时延 40.158 秒是在本补丁加载前取得，不能归因于 CHG-329，最终留出与发布仍未完成。
- 播报复核：修正把 control_coverage 累计值当成验证专用值的展示错误，改为分别列 validation_applied、cumulative_applied 和实际规则来源。节点 16 验证实际为 L1 15、L2 动作 1、编排 1、L3 2；后两次 L3 全属基础规则，学习型 L3 为 0。未改原始成绩、评分或门槛；报告第 20 节表格已同步纠正，不能据此声称本节点新增 L3 产生收益。
- 最终收尾：e64 选中节点 16，最终留出 L0 为 14/15、raw invalid loop 1.600、49.610 秒，候选为 12/15、0.533、46.415 秒；因 quality_regression 拒绝发布，没有 published-version.json。按用户预算要求停止迭代，确认无 active job 并停止服务 PID 185503；数据集、memory、缓存和结果全部保留。仅补报告第 21 节，不继续调用模型或改业务代码，未提交 Git。

### CHG-20260920-330：保护可明确归因的必要动作，同步复核经验适用条件

- 用户恢复目标后继续修复。动作预算原先最小值固定为 1，并只检查能否拦截，缺少对训练中必要后续调用的保护。完整评审现在按 scope/operation 汇总必要次数的最大下界，不跨 Run 求和；展示裁剪前保留摘要，新预算预检按 Run、query event 和 action ID 精确检查误拦。
- 整轮 keep 不等于每个动作必要：只在唯一逻辑动作且没有独立最终交付时建立行动级标记；混合动作、未知评审、非字符串最终内容保持未知，不用 stderr 或自由文本猜测。guard/完成记录按真实 action ID 关联，不按操作名合并并发分支。未改变旧策略加载、运行时、v1、评分或发布门槛。
- 两份 GEPA prompt 的经验示例同时返回正文与适用条件，要求复核当前有效条件，未变则原样保留；不新增输入字段或模型调用，旧 content-only 增量协议仍兼容。此举不等于保证模型语义正确。
- 范围：src/gepa/gepa_search.cpp、两份 GEPA prompt 和 test_gepa_evidence.cpp。训练反例支持修复通用漏洞，但没有证明该漏洞直接造成上轮节点 16 的留出质量下降。
- 验证：GepaEvidenceTests 通过（1.20 秒），覆盖跨 Run 下界、混合轮、独立交付、null/数组内容、裁剪和旧规则兼容；L2 typed binding 实际执行定向 HTTP 用例通过。初轮发现旧测试对空 bindings 的假设错误，已修正；未运行全量回归。

### CHG-20260920-331：最终留出单独复用固定 L0，候选仍重新执行

- 新增 GEPA 内部 holdout_baseline_cache.h；不放宽通用 evaluator 的 checkpoint 校验，不改变评测分数、门槛或 v1 代码。验证集与留出集分别缓存，不能把验证 L0 的 0.800 与留出候选混算。
- 缓存身份绑定执行/评测上下文、L0 版本、dataset、holdout 任务与任务版本、重复次数。兼容旧报告时，原 val 缓存只用于核对执行条件，实际数据必须来自完整 holdout baseline；按固定时间/路径顺序选来源，不按分数挑选。
- 校验报告、manifest 各文件及 artifacts 的哈希/大小/路径，拒绝符号链接与越界；锁定源变化后显式失败，重试不能偷偷换源或重跑 L0。合并报告保留原试次、时延和分数，重算同题配对，明确标记 historical_baseline，并检查总报告大小。
- 真正执行与预算只计算新候选；旧候选的留出成绩不复用，留出内容不进入反思。输出包含来源哈希及 reused_baseline_trials；保留原最终质量门槛。
- 验证：C++ 证据用例覆盖 split/任务版本隔离、源损坏、配对、产物复制及超限；两作业 HTTP 用例通过（15.990 秒），确认第二次只执行候选，L0 记录原样保留且低质量仍拒绝。测试样例让两组都失败，拒绝原因应为 quality_below_threshold，不是相对 quality_regression，已纠正测试预期，生产评分未改。
- 实验：启动后台服务 PID 296096，作业 4714940a…37bd2，从上轮验证节点 17 继续。候选缓存因严格环境指纹未命中，短暂停止核对；已确认 L0 指纹与旧记录一致，保留 5 条完成的候选结果后 retry 同一作业。尚未获得本轮最终结果，不宣称目标已达成。数据、memory、旧报告全部保留，未提交 Git。
- 接续状态：起始策略新验证 15/15、无效 loop 0.0667、42.605 秒；验证及留出 L0 分别命中。按用户新要求，本轮搜索及最终留出完成后暂停，不自动新开实验；已设置当前作业结束后停止服务的监控。学习型 L3 在该起始策略验证中仍为 0，不能宣称整体能力目标达成。
