## 2026-03-07 src/agent session/task 配置模型重构

> **已废弃**：此配置模型已被 2026-03-17 的扁平化结构取代。参见下方章节。

### 背景

`src/agent` 原先依赖隐式约定：
- `agent.json` 只覆盖少量字段；
- 默认 0 号 session 之外的 session 主要通过运行时动态创建；
- session 内部固定使用 `task0` / `task1` 表示 dispatcher / coordinator；
- context workflow 通过旧的 context task 语义分散存在，skill 任务也缺乏实例池配置。

这使得多 session 协作、skill 扩容、context 任务演进都不够直观。

### 本次重构（已被扁平化结构取代）

- `agent.json` 改为 `session_pool_size + tasks + sessions` 组合：
  - `tasks[]` 中使用 `contextupdater` 声明 agent 级上下文更新任务；
  - `sessions[]` 固定只保留 1 份同质化 session 模板，由 `session_pool_size` 复制出整个 session 池；
  - session 只声明 `dispatcher` / `coordinator` / `skill` 三类任务，`skill.instance_pool_size` 默认值为 `2`，`task.workflow_pool_size` 默认值为 `1`。
- `SessionRouter` 统一维护默认请求接收队列、响应发送队列、路由线程和 aging 线程；消息按 `user_id + client_id` 命中或分配预创建 session。
- `skill` 任务新增 `skill.name`、`skill.config_dir`、`skill.instance_pool_size`，初始化时按配置创建任务实例池；所有 task 统一支持 `workflow_pool_size`，用于声明对应 workflow 的多实例池规模，runtime 启动时会把这些 workflow 实例池预注册到 deployer / scheduler。
- `MMAgent::initialize_components()` 改为优先按 `agent.json.sessions` 预创建 session pool；请求先进入 router，再映射到具体 session。
- examples / publish / tests 已同步切换到新 schema。

### 验证结果

- `./build.sh build`：通过
- `ctest --test-dir build -R 'E2EMessageRouting|E2EBasicAgent' --output-on-failure`：通过
- `./build.sh test`：在重构过程中发现并修复兼容性问题，最终全量验证通过

本文档说明了 AgentFlow 项目目录结构的优化工作。
