## 2026-03-17 agent.json 配置结构冗余分析

### 背景

对 agent.json 配置结构进行冗余分析，回答以下问题：
1. 顶层的 `sessions` 数组是否有必要
2. skill task 的 `skill` 属性做什么用的，是否有必要
3. task 定义是否可以融合到 workflow 定义中

**约束：仅更新 REFACTORING.md，不变更代码。**

### Session 在 AgentFlow 框架中的作用

Session 是 AgentFlow 中**消息路由和状态隔离**的核心单元：

```
┌─────────────────────────────────────────────────────────────────┐
│                         MMAgent                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    SessionRouter                         │   │
│  │  • 按 user_id:client_id 路由消息到 Session               │   │
│  │  • 管理 Session 池（分配/回收/老化）                      │   │
│  │  • 维护 route_key -> Session 映射                        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│          ┌───────────────────┼───────────────────┐              │
│          ▼                   ▼                   ▼              │
│  ┌───────────────┐   ┌───────────────┐   ┌───────────────┐     │
│  │   Session 0   │   │   Session 1   │   │   Session N   │     │
│  │ ┌───────────┐ │   │ ┌───────────┐ │   │ ┌───────────┐ │     │
│  │ │Dispatcher │ │   │ │Dispatcher │ │   │ │Dispatcher │ │     │
│  │ └───────────┘ │   │ └───────────┘ │   │ └───────────┘ │     │
│  │ ┌───────────┐ │   │ ┌───────────┐ │   │ ┌───────────┐ │     │
│  │ │Coordinator│ │   │ │Coordinator│ │   │ │Coordinator│ │     │
│  │ └───────────┘ │   │ └───────────┘ │   │ └───────────┘ │     │
│  │ ┌─────┐┌─────┐│   │ ┌─────┐┌─────┐│   │ ┌─────┐┌─────┐│     │
│  │ │Skill││Skill││   │ │Skill││Skill││   │ │Skill││Skill││     │
│  │ └─────┘└─────┘│   │ └─────┘└─────┘│   │ └─────┘└─────┘│     │
│  └───────────────┘   └───────────────┘   └───────────────┘     │
│                                                                 │
│  DataBus: /session/{session_id}/... 隔离各 Session 数据         │
└─────────────────────────────────────────────────────────────────┘
```

### 分析结果

#### 问题1：顶层的 `sessions` 是否有必要

**结论：`sessions` 数组结构冗余，可简化为扁平化 `tasks` 结构**

**分析依据：**

1. 解析代码强制要求只有一个 session 模板（`src/agent/package.cpp:217-218`）:
```cpp
if(session_array.Size() != 1) {
  return fail_agent_json_parse("sessions must contain exactly one homogeneous session template");
}
```

2. 当前结构：
```json
{
  "session_pool_size": 2,
  "sessions": [
    { "id": 0, "tasks": [...] }  // 只允许一个模板，id 永远是 0
  ]
}
```

3. 问题：
   - 数组结构暗示可以定义多个 session，但实际被限制为只能有一个
   - `id` 字段在这种情况下无意义（永远是0）
   - 增加了不必要的嵌套层级

#### 问题2：skill task 的 `skill` 属性用途

**结论：`skill` 属性必要，不可移除**

**分析依据：**

`skill` 属性包含三个字段（`include/internal/agent/package.h:29-33`）：

| 字段 | 用途 | 代码位置 |
|------|------|----------|
| `skill_name` | 标识技能，dispatcher 通过此名称查找并启动技能 | `src/agent/agent.cpp:62-63`, `agent.cpp:80-86` |
| `skill_config_dir` | 技能配置文件目录路径 | `src/agent/package.cpp:131-133` |
| `instance_pool_size` | 创建技能任务实例池，支持并行执行 | `src/agent/agent.cpp:475`, `agent.cpp:502-505` |

#### 问题3：skill 配置放 workflow 还是 task 更合适

**结论：skill 配置应保留在 task 中，不应移到 workflow**

**核心问题：多个 skill 可以共享同一个 workflow**

```json
// agent.json 中多个 skill 使用同一个 workflow 文件
{
  "tasks": [
    {
      "type": "skill",
      "workflow_file": "generic_skill_workflow.json",  // 同一个 workflow
      "skill": { "name": "travel", "config_dir": "skills/travel" }
    },
    {
      "type": "skill",
      "workflow_file": "generic_skill_workflow.json",  // 同一个 workflow
      "skill": { "name": "shopping", "config_dir": "skills/shopping" }
    }
  ]
}
```

**分离设计的优势：**
- **Workflow** = 执行逻辑（nodes、edges、控制流）
- **Task** = 角色配置（哪个技能、配置路径、并行度）
- 一个 workflow 可被多个 skill 共享，各 skill 有独立配置

### 修改项一：agent.json 结构扁平化

**核心思路**：代码中 Session 机制保持不变，只是 JSON 配置扁平化。JSON 中完全移除 "session" 命名和层级。

```json
// 当前结构（冗余嵌套）
{
  "name": "BasicAgent",
  "version": "1.0.0",
  "session_pool_size": 2,
  "tasks": [                           // Agent 级 tasks
    { "type": "contextupdater", ... }
  ],
  "sessions": [
    {
      "id": 0,                         // 无意义，永远是 0
      "tasks": [                       // Session 级 tasks
        { "type": "dispatcher", ... },
        { "type": "coordinator", ... },
        { "type": "skill", ... }
      ]
    }
  ]
}

// 扁平化后（JSON 中无 session 层级和命名）
{
  "name": "BasicAgent",
  "version": "1.0.0",
  "session_pool_size": 2,
  "tasks": [
    { "type": "contextupdater", "scope": "agent", ... },  // Agent 级
    { "type": "dispatcher", ... },                         // Session 级（缺省 scope="session"）
    { "type": "coordinator", ... },                        // Session 级（缺省 scope="session"）
    { "type": "skill", ... }                               // Session 级（缺省 scope="session"）
  ]
}
```

**Task 新增 `scope` 属性**：

| 属性 | 类型 | 缺省值 | 说明 |
|------|------|--------|------|
| `scope` | string | `"session"` | 任务作用域：`"agent"` 或 `"session"` |

**验证规则**：
- `scope: "agent"` 的任务：任意类型，按需配置
- `scope: "session"` 或缺省的任务：必须包含 1 个 dispatcher + 1 个 coordinator + 至少 1 个 skill

**优点**：
1. JSON 完全无 "session" 层级和命名，更简洁
2. 消除 `id`、`task_count`、`description` 等冗余字段
3. 代码中 Session 创建逻辑保持不变
4. 向后兼容：可同时支持新旧格式

### 修改项二：package.json 简化

**移除字段**：

| 字段 | 原因 |
|------|------|
| `task_workflows` | 与 `agent.json` 的 `tasks[].workflow_file` 重复 |
| `dispatcher_workflow` | 从未被使用的 fallback，应在 `agent.json` 定义 |
| `coordinator_workflow` | 从未被使用的 fallback，应在 `agent.json` 定义 |
| `package_type` | 未在解析代码中生效 |
| `agent_version` | 与 `version` 含义重复 |

**简化后的 package.json**：

```json
{
  "name": "basic-agent",
  "version": "1.0.0",
  "description": "...",
  "author": "AgentFlow Team",
  "entry_point": "basic-agent.so",
  "workflows": [
    "workflow.json",
    "coordinator_workflow.json"
  ],
  "builtin_workflows": ["memory"],
  "dependencies": {
    "core-lib": "1.0.0"
  },
  "metadata": {
    "type": "agent",
    "category": "example"
  }
}
```

### 影响范围

**需更新的源码文件**：

| 文件 | 修改内容 |
|------|----------|
| `include/internal/agent/package.h` | `TaskDefinition` 新增 `scope` 字段，`Config::sessions` 改为扁平化 `Config::tasks` |
| `src/agent/package.cpp` | 解析逻辑：按 scope 分类 tasks，新增 scope 字段解析 |
| `src/agent/agent.cpp` | 访问 `sessions.front().tasks` 改为 `session_tasks` |

**需更新的配置文件**：

**agent.json 文件（10个）**：
- `examples/agent/basic-agent/agent.json`
- `examples/agent/nested-workflow-agent/agent.json`
- `examples/agent/workflow-patterns-agent/agent.json`
- `examples/agent/claw-agent/agent.json`
- `examples/agent/llm-query-agent/agent.json`
- `examples/agent/system-agent-mock/agent.json`
- `examples/agent/openjiuwen-studio-agent/agent.json`
- `examples/agent/multi_session_chat/package/agent.json`
- `examples/agent/openjiuwen_multi_session_chat/package/agent.json`
- `examples/agent/multi_language_databus/agent.json`

**package.json 文件（9个）**：
- `examples/agent/basic-agent/package.json`
- `examples/agent/nested-workflow-agent/package.json`
- `examples/agent/workflow-patterns-agent/package.json`
- `examples/agent/claw-agent/package.json`
- `examples/agent/llm-query-agent/package.json`
- `examples/agent/system-agent-mock/package.json`
- `examples/agent/openjiuwen-studio-agent/package.json`
- `examples/agent/multi_session_chat/package/package.json`
- `examples/agent/openjiuwen_multi_session_chat/package/package.json`

**需更新的文档**：
- `publish/agent/AGENT_PACKAGE_SPEC.md` - Agent 包规范
- `publish/INTEGRATION_GUIDE.md` - 集成指南
- `docs/agentflow_user_manual.md` - 用户手册

### 不建议修改

1. **`skill` 属性**：保留在 task 中，支持 workflow 复用
2. **Task 与 Workflow 分离**：保留，架构合理
3. **Session 池机制**：代码层面完全保留，仅 JSON 结构扁平化

### 验证方案

如果未来实施代码修改：
1. 修改解析代码后运行单元测试
2. 更新所有示例 agent.json 和 package.json
3. 运行 `./build.sh test` 验证全量测试通过

### 实施状态（2026-03-17）

**已完成实施**

1. **源码修改**：
   - `include/internal/agent/package.h`：`TaskDefinition` 新增 `scope` 字段，`Config` 新增 `session_tasks` 字段，移除冗余字段
   - `src/agent/package.cpp`：新增 `parse_flattened_tasks()` 和 `validate_session_tasks()` 函数，支持新旧格式向后兼容
   - `src/agent/agent.cpp`：新增 `create_session_from_tasks()` 函数，移除对 package.json 冗余字段的依赖
   - `include/internal/agent/agent.h`：更新函数声明

2. **测试更新**：
   - `tests/unit/src/test_builtin_workflow_gtest.cpp`：移除对已删除字段的断言
   - `tests/e2e/workflow_execution/test_basic-agent_e2e.cpp`：移除对 `coordinator_workflow` 的断言
   - `examples/features/agent_runner.cpp`：移除对 `coordinator_workflow` 的依赖

3. **配置文件更新**：
   - 10 个 agent.json 文件已转换为扁平化格式
   - 9 个 package.json 文件已移除冗余字段
   - `publish/agent/agent.json` 和 `publish/agent/package.json` 已同步更新

4. **向后兼容**：
   - 解析代码同时支持新旧格式
   - 旧格式（`sessions` 数组）仍可正常解析
   - 新格式（扁平化 `tasks` + `scope` 属性）为推荐格式

5. **验证结果**：
   - `./build.sh build`：通过
   - `ctest --test-dir build --output-on-failure`：18/19 测试通过（E2ELLMQueryAgent 因 API Key 缺失失败，与本次修改无关）
