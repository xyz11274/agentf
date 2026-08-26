# Harness 改动记录

本文件用于持续记录 Harness 开发过程中的实际改动，作为代码审查、问题回溯、
轨迹采集设计和后续 Rust 重写的变更依据。

## 记录规则

- 代码、测试、配置、脚本、开发环境或项目文档发生改动时，必须在同一轮工作中更新本文件。
- 纯只读分析、代码讲解、测试执行和临时诊断文件不单独记为改动；其结论可写入相关改动的验证部分。
- 不覆盖或改写历史条目；需要更正时新增条目并引用原条目编号。
- 每条记录至少包含：问题或目标、改动内容、涉及文件、诱发原因、验证结果和遗留事项。
- 新记录按时间倒序插入到“改动条目”标题下方。

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
