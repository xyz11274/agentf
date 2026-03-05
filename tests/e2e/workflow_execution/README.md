# Workflow执行场景测试

**测试目标**: 验证 MMWorkflow 从加载到执行完成的完整流程

---

## 测试链路

```
MMAgentTask → Scheduler → Deployer → MMWorkerEngine → Node执行 → 状态更新
```

---

## 测试用例

| 用例ID | 名称 | 描述 |
|--------|------|------|
| TC-E2E-101 | SingleNodeExecution | 单节点 MMWorkflow 执行 |
| TC-E2E-102 | MultiNodeDAG | 多节点 DAG 执行 |
| TC-E2E-103 | ConditionalBranch | 条件分支执行 |
| TC-E2E-104 | LoopControlFlow | 循环控制流执行 |
| TC-E2E-105 | NestedWorkflow | 嵌套 MMWorkflow 调用（MMNode 调用子 MMWorkflow） |

---

## 测试数据

`data/` 包含不同类型的 MMWorkflow 定义用于测试执行流程。