# AgentFlow 端到端测试 (E2E Tests)

本目录存放端到端测试用例，用于验证完整的业务链路。

**最后更新**: 2026年3月3日

---

## 目录结构

```
e2e/
├── README.md                    # 本文档
├── CMakeLists.txt               # E2E测试构建配置
├── message_routing/             # 消息路由场景测试
│   ├── README.md               # 场景说明
│   ├── test_message_routing.cpp # 测试实现
│   └── data/                   # 测试数据
│       └── agent_package/      # Agent包定义
├── workflow_execution/          # Workflow执行场景测试
│   ├── README.md
│   ├── test_workflow_execution.cpp
│   └── data/
├── session_isolation/           # Session隔离场景测试
│   ├── README.md
│   ├── test_session_isolation.cpp
│   └── data/
└── data_sharing/               # DataBus数据共享场景测试
    ├── README.md
    ├── test_data_sharing.cpp
    └── data/
```

---

## 测试场景

### 1. 消息路由场景 (message_routing)

**测试目标**: 验证消息从进入到任务生成的完整链路

**链路**:
```
reqmsg → MMAgent.default_session → MMAgent.new_session → SessionTaskCoordinator → MMAgentTask
```

**测试用例**:
- TC-E2E-001: 基础消息路由（user_id + client_id 创建新 session）
- TC-E2E-002: 消息 metadata 缺失 user_id/client_id 处理
- TC-E2E-003: coordinator_workflow 默认路由
- TC-E2E-004: 显式 workflow_id 覆盖

### 2. Workflow执行场景 (workflow_execution)

**测试目标**: 验证 MMWorkflow 从加载到执行完成的完整流程

**链路**:
```
MMAgentTask → Scheduler → Deployer → MMWorkerEngine → Node执行 → 状态更新
```

**测试用例**:
- TC-E2E-101: 单节点 MMWorkflow 执行
- TC-E2E-102: 多节点 DAG 执行
- TC-E2E-103: 条件分支执行
- TC-E2E-104: 循环控制流执行
- TC-E2E-105: 嵌套 MMWorkflow 调用（MMNode 调用子 MMWorkflow）

### 3. Session隔离场景 (session_isolation)

**测试目标**: 验证不同 Session 之间的隔离性

**测试用例**:
- TC-E2E-201: 多 Session 并发创建
- TC-E2E-202: Session 任务隔离
- TC-E2E-203: Session 消息队列隔离
- TC-E2E-204: Session coordinator 隔离

### 4. 数据共享场景 (data_sharing)

**测试目标**: 验证 MMDataBus 跨实例数据共享

**链路**:
```
Node1.output → MMDataBus.put → DataRef → Node2.input → MMDataBus.get
```

**测试用例**:
- TC-E2E-301: 进程内数据共享
- TC-E2E-302: DataRef 引用传递
- TC-E2E-303: 订阅通知机制
- TC-E2E-304: Owner 索引查询

---

## 运行测试

```bash
# 运行所有E2E测试
ctest --test-dir build -R E2E --output-on-failure

# 运行特定场景
./build/bin/test_e2e_message_routing
./build/bin/test_e2e_workflow_execution
./build/bin/test_e2e_session_isolation
./build/bin/test_e2e_data_sharing

# 运行单个测试用例
./build/bin/test_e2e_message_routing --gtest_filter=E2EMessageRouting.BasicRouting
```

---

## 测试数据规范

### Agent包结构

每个场景的 `data/agent_package/` 目录需包含：

```
data/agent_package/
├── package.json           # Agent包定义
├── workflows/
│   ├── workflow_a.json   # Workflow定义
│   └── workflow_b.json
└── scripts/
    └── test_script.sh    # 测试脚本（可选）
```

### package.json 示例

```json
{
  "name": "e2e-test-agent",
  "version": "1.0.0",
  "workflows": ["workflows/workflow_a.json"],
  "coordinator_workflow": "workflows/coordinator.json",
  "session_context_workflow": "workflows/session_context.json",
  "task_context_workflow": "workflows/task_context.json"
}
```

---

## 设计原则

1. **独立性**: 每个测试用例独立运行，不依赖其他用例状态
2. **可重复性**: 测试可多次运行，结果一致
3. **自包含**: 测试数据、配置、预期结果都在用例目录内
4. **真实模拟**: 尽量模拟真实业务场景，而非简单Mock

---

## 扩展指南

### 添加新场景

1. 在 `e2e/` 下创建新目录，如 `new_scenario/`
2. 创建 `README.md` 描述场景和测试用例
3. 创建 `test_new_scenario.cpp` 实现测试
4. 创建 `data/` 存放测试数据
5. 更新 `e2e/CMakeLists.txt` 添加编译配置
6. 更新本文件添加场景说明