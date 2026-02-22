# Workflow / Node 开发指南

## 内建 workflow

`publish/workflow/` 内提供内建模板：
- `memory/`
- `skills/`
- `tools/`
- `pattern/serial_workflow.json`
- `pattern/parallel_workflow.json`
- `pattern/cycled_workflow.json`
- `pattern/dynamic_workflow.json`

## Workflow DSL 示例

标准示例文件位于：
- `publish/workflow/workflow_sample.jsonc`

## Workflow 开发步骤

1. 定义 workflow 的 id/name/version。
2. 定义 node（声明 `execution_mode` 与可执行绑定）。
3. 定义 edge（数据依赖与流程控制）。
4. 统一使用 edge 控制模型：
   - `control.type`：`if` / `loop` / `fork` / `join` / `async` / `goto`
   - `control.*`：对应模式参数（如 `expression`、`max_iterations`、`join_group` 等）
5. 支持嵌套 workflow：
   - 顶层包含：`includes: ["relative/path/subflow.json"]`
   - workflow 节点：`type: "workflow"` + `workflow_ref`

## Node 开发入口

- Node 开发规范：`publish/workflow/NODE_DEVELOPMENT_SPEC.md`
- Node API：`publish/workflow/NODE_API_REFERENCE.md`
- Node 模板：`publish/workflow/node_patterns/`
- Node Skill 说明：`src/workflow/node/skill.md`

## Workflow Skill 化

- DSL Skill 说明：`src/workflow/dsl/skill.md`

## 可视化兼容

- `publish/agent/visualizer.html` 支持 `.json` / `.jsonc`
- 支持新字段：`includes`、node `workflow_ref`、edge `control.type + control.*`
- 支持有环流程（`goto`/`loop`）的布局展示

## 打包集成

当 Agent `package.json` 配置了 `builtin_workflows` 时，`publish/agent_build.sh` 会将对应内建 workflow 一并打包。
