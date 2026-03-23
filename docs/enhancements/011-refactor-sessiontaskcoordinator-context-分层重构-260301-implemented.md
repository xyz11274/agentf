## 2026-03-01 SessionTaskCoordinator & Context 分层重构

- `MMAgentTask` 增加 `workflow_file`，并在任务实例化时透传，供调度阶段做 workflow 一致性校验。
- `AgentCoordinator` 重命名为 `SessionTaskCoordinator`，作为 session 0 号任务化协调单元，下沉至 `MMAgentSession` 持有。
- `AgentContext` 增加可选 `context_workflow` 及默认 `context_updater` 任务。
- 上下文能力扩展至 `MMAgent/Session/Task/MMNode` 四级作用域，缺失 workflow 配置时走默认处理模式。
- package 增加可选字段：`agent_context_workflow`、`session_context_workflow`、`task_context_workflow`、`node_context_workflow`。
