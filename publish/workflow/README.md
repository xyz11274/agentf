# MMWorkflow / MMNode 开发指南

## 内建 workflow

`publish/workflow/` 内提供内建模板：
- `memory/`
- `skills/`
- `tools/`
- `pattern/serial_workflow.json`
- `pattern/parallel_workflow.json`
- `pattern/cycled_workflow.json`
- `pattern/dynamic_workflow.json`

## MMWorkflow DSL 示例

标准示例文件位于：
- `publish/workflow/workflow_sample.jsonc`

## MMWorkflow 开发步骤

1. 定义 workflow 的 id/name/version。
2. 定义 node（声明 `execution_mode` 与可执行绑定）。
3. 定义 edge（数据依赖与流程控制）。
4. 统一使用 edge 控制模型：
   - `control.type`：`if` / `switch-case`
   - `control.*`：`if` 使用 `expression`；`switch-case` 使用共享 `expression` + 每条边自己的 `case_value` / `is_default`
5. 支持嵌套 workflow：
   - 顶层包含：`includes: ["relative/path/subflow.json"]`
   - workflow 节点：`type: "workflow"` + `workflow_ref`

## MMNode 开发入口

- MMNode 开发规范：`publish/workflow/NODE_DEVELOPMENT_SPEC.md`
- MMNode API：`publish/workflow/NODE_API_REFERENCE.md`
- MMNode 模板：`publish/workflow/node_patterns/`
- MMNode Skill 说明：`src/workflow/node/skill.md`

## MMWorkflow Skill 化

- DSL Skill 说明：`src/workflow/dsl/skill.md`

## 可视化兼容

- `publish/agent/visualizer.html` 支持 `.json` / `.jsonc`
- 支持新字段：`includes`、node `workflow_ref`、edge `control.type + control.*`
- 推荐使用 DAG + 条件/分支边建模，不再提供 `goto`/`loop` 控制边

## 打包集成

当 MMAgent `package.json` 配置了 `builtin_workflows` 时，`publish/agent_build.sh` 会将对应内建 workflow 一并打包。
