# Flow Trace 分析报告

## 测试目标

使用SystemAgentMock验证flow trace日志是否符合Agent运行流程。

## 预期流程

SystemAgentMock的预期消息流程：
1. User message triggers dispatcher
2. Dispatcher launches skill-selection skill (from next_skills config)
3. skill-selection randomly picks 2 skills from [travel, shopping, foodie]
4. Selected skills execute and store results in DataBus
5. Coordinator aggregates results from all skills
6. User receives aggregated response

## 实际Trace日志分析

### Dialogue: `13616292488101129513`

```
序号 | 时间         | 类型          | from                   | to                     | event
----|-------------|---------------|------------------------|------------------------|---------------------
1   | 30.647      | task-exec     | external               | dispatcher             | task-created
2   | 30.647      | skill-exec    | dispatcher             | skill-selection        | skill-launched
3   | 30.656      | workflow-exec | 0_task_0               | skill-selection-workflow | workflow-started
4   | 30.657      | node-exec     | skill-selection-workflow | Random Skill Selector | node-started
5   | 30.687      | node-exec     | skill-selection-workflow | Random Skill Selector | node-completed (30ms)
6   | 30.688      | workflow-exec | 0_task_0               | skill-selection-workflow | workflow-completed
7   | 30.698      | workflow-exec | 0_task_1               | coordinator-workflow   | workflow-started
8   | 30.698      | node-exec     | coordinator-workflow   | Aggregate Skill Results | node-started
9   | 30.729      | node-exec     | coordinator-workflow   | Aggregate Skill Results | node-completed (30ms)
10  | 30.729      | workflow-exec | 0_task_1               | coordinator-workflow   | workflow-completed
```

### 流程对比

| 预期步骤 | 实际Trace | 状态 |
|----------|-----------|------|
| 1. User message triggers dispatcher | task-created: external -> dispatcher | ✓ |
| 2. Dispatcher launches skill-selection | skill-launched: dispatcher -> skill-selection | ✓ |
| 3. skill-selection workflow executes | workflow-started/completed: skill-selection-workflow | ✓ |
| 4. Skills execute | (由coordinator内部处理，非独立workflow) | ~ |
| 5. Coordinator aggregates | workflow-started/completed: coordinator-workflow | ✓ |
| 6. Response sent | (测试超时，未完成) | ✗ |

### 多Dialogue并行处理

日志中可以看到多个dialogue并行处理：

```
Dialogue 12998438490268443778:
- 17:15:30.762 task-created
- 17:15:30.762 skill-launched
- 17:15:30.779 workflow-started
...

Dialogue 10363149888760767602:
- 17:15:30.763 task-created
- 17:15:30.763 skill-launched
- 17:15:30.787 workflow-started
...
```

两个dialogue的事件交错出现，证明并行处理正常工作。

### Dialogue ID传递验证

**修复前**：coordinator-workflow事件缺少dialogue_id

```json
{"time":"...","type":"workflow-exec","session":"0","from":"0_task_1","to":"coordinator-workflow","event":"workflow-started"}
```

**修复后**：所有事件都正确包含dialogue_id

```json
{"time":"...","type":"workflow-exec","session":"0","dialogue":"13616292488101129513","from":"0_task_1","to":"coordinator-workflow","event":"workflow-started"}
```

## 结论

### 成功验证项

1. ✓ **任务创建**：task-created事件正确记录external -> dispatcher
2. ✓ **技能启动**：skill-launched事件正确记录dispatcher -> skill-selection
3. ✓ **Workflow生命周期**：workflow-started -> node-started -> node-completed -> workflow-completed
4. ✓ **Node耗时**：node-completed事件包含duration_us字段
5. ✓ **Dialogue隔离**：多dialogue并行处理，ID正确传递
6. ✓ **Session标识**：所有事件包含正确的session_id

### 待改进项

1. ✗ **response-sent事件**：未捕获到响应发送事件（可能是测试超时）
2. ~ **Skill执行详情**：travel/shopping/foodie skills可能在coordinator内部执行，无独立workflow
3. ✗ **inputs/outputs**：当前实现未记录详细数据（可按需添加）

## 修复内容

### build_runtime_task中添加dialogue_id传递

```cpp
// Pass dialogue_id from message metadata to task parameters for workflow propagation
if(const auto dialogue_id_it = message.metadata.find("dialogue_id");
   dialogue_id_it != message.metadata.end() && !dialogue_id_it->second.empty()) {
  task->set_parameter("dialogue_id", dialogue_id_it->second);
  Logger::instance().info_f("[BUILD_RUNTIME_TASK] Set dialogue_id=%s for task=%s",
                            dialogue_id_it->second.c_str(), task_config.task_id.c_str());
}
```

这个修复确保了dialogue_id从消息metadata正确传递到task parameters，从而使得workflow执行时能够正确记录dialogue_id。