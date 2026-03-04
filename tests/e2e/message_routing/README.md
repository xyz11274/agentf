# 消息路由场景测试

**测试目标**: 验证消息从进入到任务生成的完整链路

---

## 测试链路

```
reqmsg → Agent.default_session → Agent.new_session → SessionTaskCoordinator → AgentTask
```

---

## 测试用例

| 用例ID | 名称 | 描述 |
|--------|------|------|
| TC-E2E-001 | BasicRouting | 基础消息路由（user_id + client_id 创建新 session） |
| TC-E2E-002 | MissingMetadata | 消息 metadata 缺失 user_id/client_id 处理 |
| TC-E2E-003 | CoordinatorWorkflow | coordinator_workflow 默认路由 |
| TC-E2E-004 | ExplicitWorkflowId | 显式 workflow_id 覆盖 |

---

## 测试数据

`data/agent_package/` 包含完整的 Agent 包定义，用于测试消息路由。