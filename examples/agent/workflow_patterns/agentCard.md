# WorkflowPatternsAgent

演示 AgentFlow 常见 workflow 编排模式的示例 Agent。

## 包含的 Workflow

### conditional_workflow.json — 条件分支

演示基于数据值的条件分支执行：根据 `request_type` 字段将任务路由到不同的处理节点（A/B/C 三路并行 + 聚合）。

### loop_workflow.json — switch-case 路由

演示 `switch-case` 分支路由：加载请求 → 根据 route 选择 A/B/default 分支。

### sample_workflow.json — 顺序处理管道

演示带条件边的标准数据处理管道：输入验证 → 数据转换（Python） → 数据增强 → 过滤 → 输出。

## 用途

这些 workflow 文件均为**设计参考模板**，展示 DSL 字段的典型用法，不依赖外部脚本即可作为编排逻辑的起点。
