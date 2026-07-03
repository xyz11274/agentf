# AgentFlow MMAgent Publishing Guide

本文档面向业务开发团队，说明如何开发并发布一个新的业务 MMAgent 包。

## 1. MMAgent 包开发
参考 `publish/agent/AGENT_PACKAGE_SPEC.md`，按规范准备 MMAgent 包内容。

### 1.1 必备文件
- `agent.json`：MMAgent 名称、版本、session pool 拓扑配置；使用扁平化 `tasks` 结构，通过 `scope` 属性区分 Agent 级任务（如 `contextupdater`）和 Session 级任务（`dispatcher` / `coordinator` / `skill`）；task 级可通过 `workflow_pool_size` 声明 workflow 多实例池，skill 任务可通过 `skill.instance_pool_size` 声明实例池
- `agentCard.md`：MMAgent Prompt/能力说明
- `workflow_XX.json`：流程定义
- `node_xxx.$TYPE`：可执行部件（`.sh/.py/.so/.jar`）
- `package.json`：外层包描述与 workflow 文件加载列表

### 1.2 打包
使用：

```bash
./publish/agent_build.sh build <agent_source_dir> [output_dir]
```

产物：
- 外层：`$xxxAgent_$PKGTYPE_$PKGVER.tar.gz`
- 内层：`$xxxAgent_$AGTVER.tar.gz`

## 2. MMWorkflow 开发
MMWorkflow 规范样例位于：
- `publish/workflow/workflow_sample.jsonc`

开发时重点定义：
- `nodes`：负责执行，明确执行模式与执行部件
- `edges`：负责控制流程与数据传递
- 统一控制字段：`edge.control.type + edge.control.*`，支持 `if` 与 `switch-case`
- Workflow 图保持有向无环；分支通过 `if` / `switch-case` 表达，避免隐式回跳控制

可复用内置模板：
- `publish/workflow/memory`
- `publish/workflow/skills`
- `publish/workflow/tools`

固定流程模式：
- `publish/workflow/pattern/serial_workflow.json`
- `publish/workflow/pattern/parallel_workflow.json`
- `publish/workflow/pattern/cycled_workflow.json`
- `publish/workflow/pattern/dynamic_workflow.json`

嵌套编排支持：
- `includes`: 在 workflow 顶层引入其他 workflow 文件
- `type: "workflow"` 节点 + `workflow_ref`：将子 workflow 作为节点引用

## 3. MMNode 定制开发
MMNode 规范文档：
- `publish/workflow/NODE_DEVELOPMENT_SPEC.md`

MMNode API 手册：
- `publish/workflow/NODE_API_REFERENCE.md`

固定 MMNode 模式模板：
- `publish/workflow/node_patterns/qa_node_template.md`
- `publish/workflow/node_patterns/api_tool_select_template.md`
- `publish/workflow/node_patterns/mcp_tool_select_template.md`
- `publish/workflow/node_patterns/skills_select_template.md`
- MMNode skill 规范：`src/workflow/node/skill.md`
- MMWorkflow skill 规范：`src/workflow/dsl/skill.md`

建议实现统一入口（输入/输出 JSON、明确退出码），并在 workflow node 中声明 `execution_mode` 与 `command`。
多语言执行由 worker 执行引擎统一承载，node 侧主要声明执行配置与输入输出契约。

## 4. 可视化调试
- 使用 `publish/agent/visualizer.html` 可视化 workflow。
- 支持 `.json/.jsonc`，并可展示 `includes`、`workflow_ref`、`edge.control.*`。
- 节点 `name/type` 缺省时会自动回退到 `id/unknown`，避免示例文件因字段缺失导致加载失败。
- 可视化面向 DAG + 条件/分支边；`switch-case` 默认与普通分支边一致展示。

## 5. 服务集成
- 参考 `publish/INTEGRATION_GUIDE.md` 了解如何将 AgentFlow 框架集成到服务中。
- 包含 Agent、Runtime、Worker 的配置与生命周期管理。
- 提供单进程、分布式、容器化等多种部署模式示例。

### 5.1 消息驱动组件
- **MessageDispatcher**：消息驱动执行的中心枢纽，支持 Subscribe/Publish 和 Request-Response 模式。
- **WorkflowScheduler**：处理嵌套 Workflow 执行，支持深度限制和父子关系追踪。
- 详见 `publish/INTEGRATION_GUIDE.md` 第 4.5 节。
