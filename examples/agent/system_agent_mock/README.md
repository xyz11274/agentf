# SystemAgentMock

## 设计目标

SystemAgentMock 旨在演示 AgentFlow 中 dispatcher-skill-coordinator 消息流模式。其主要目标包括：

1. **展示动态技能选择**：演示技能如何在运行时通过 DataBus 的 `selected_skills` 机制动态选择其他技能。

2. **展示并行技能执行**：演示多个技能如何并行启动和执行，并由协调器聚合结果。

3. **展示 V3 节点签名**：使用最新的 V3 签名，通过 `INodeDataBusClient` 实现会话隔离的 DataBus 访问。

4. **支持多轮对话**：支持在单个会话中进行多轮问答，跨对话维护上下文。

## 架构概览

```
┌─────────────────────────────────────────────────────────────────────┐
│                          用户消息                                    │
└────────────────────────────────┬────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     BuiltinDispatcher                               │
│                  (next_skills: ["skill-selection"])                 │
└────────────────────────────────┬────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│                   skill-selection 技能                              │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │     从 [travel, shopping, foodie] 中随机选择 2 个技能        │   │
│  │     将 selected_skills 写入 DataBus (sys 命名空间)           │   │
│  └─────────────────────────────────────────────────────────────┘   │
└────────────────────────────────┬────────────────────────────────────┘
                                 │
                    框架检测到 selected_skills
                                 │
              ┌──────────────────┴──────────────────┐
              ▼                                     ▼
┌─────────────────────────┐           ┌─────────────────────────┐
│     被选中的技能 #1      │           │     被选中的技能 #2      │
│    (例如: travel)        │           │    (例如: shopping)      │
│                         │           │                         │
│  将结果写入 DataBus      │           │  将结果写入 DataBus      │
│  (user 命名空间)         │           │  (user 命名空间)         │
└───────────┬─────────────┘           └───────────┬─────────────┘
            │                                     │
            └──────────────────┬──────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                        协调器 (Coordinator)                         │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │     从 DataBus (user 命名空间) 读取技能结果                  │   │
│  │     聚合结果生成最终响应                                     │   │
│  │     将响应写入 DataBus (sys 命名空间)                        │   │
│  └─────────────────────────────────────────────────────────────┘   │
└────────────────────────────────┬────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│                          用户响应                                   │
└─────────────────────────────────────────────────────────────────────┘
```

## 预期行为

### 阶段 1: 初始分发
1. 用户消息到达 agent 的请求队列
2. 调用 `BuiltinDispatcher::process_request()`
3. Dispatcher 启动 `skill-selection` 技能（来自 `next_skills` 配置）

### 阶段 2: 技能选择
1. `skill-selection` 技能执行
2. 从 [travel, shopping, foodie] 中随机选择 2 个技能
3. 将 `selected_skills` 写入 DataBus sys 命名空间：`["travel", "shopping"]`
4. 技能完成，通知协调器

### 阶段 3: 技能执行
1. 框架检测到 DataBus 中的 `selected_skills`
2. 框架启动被选中的技能（例如 `travel` 和 `shopping`）
3. 每个技能执行并将结果写入 DataBus user 命名空间
4. 技能完成后通知协调器

### 阶段 4: 协调处理
1. 协调器收到所有技能的通知
2. 协调器工作流执行
3. 从 DataBus user 命名空间读取技能结果
4. 聚合并输出：
   - 将 `response` 写入 DataBus sys 命名空间（给用户）
   - 结果写入输出映射

### 阶段 5: 响应返回
1. Dispatcher 检测到 DataBus 中的 `response`
2. 将响应返回给用户

## 工作流定义

### 技能工作流

| 工作流 | 文件 | 描述 |
|--------|------|------|
| skill-selection-workflow | `skill_selection_workflow.json` | 从可用池中随机选择 2 个技能 |
| travel-workflow | `travel_workflow.json` | 返回旅行推荐的模拟技能 |
| shopping-workflow | `shopping_workflow.json` | 返回购物建议的模拟技能 |
| foodie-workflow | `foodie_workflow.json` | 返回美食推荐的模拟技能 |
| coordinator-workflow | `coordinator_workflow.json` | 聚合所有技能的结果 |

### skill-selection-workflow

| 输入 | 类型 | 描述 |
|------|------|------|
| user_text | string | 用户查询文本 |
| session_id | string | 上下文隔离的会话标识符 |

| 输出 | 类型 | 描述 |
|------|------|------|
| next_skills | string | 被选中的技能名称 JSON 数组（首选） |
| selected_skills | string | 被选中的技能名称 JSON 数组（向后兼容） |
| selection_reason | string | 选择原因说明 |

**DataBus 副作用:**
- 将 `next_skills` 写入 sys 命名空间（首选）
- 将 `selected_skills` 写入 sys 命名空间（向后兼容）

### travel-workflow / shopping-workflow / foodie-workflow

| 输入 | 类型 | 描述 |
|------|------|------|
| user_text | string | 用户查询文本 |
| session_id | string | 会话标识符 |

| 输出 | 类型 | 描述 |
|------|------|------|
| skill_result | string | 技能的模拟结果数据 |
| status | string | 执行状态 ("success") |
| confidence | double | 置信度分数 (0.7-0.95) |

**DataBus 副作用:**
- 将 `{skill_name}_result` 写入 user 命名空间
- 将 `{skill_name}_confidence` 写入 user 命名空间

### coordinator-workflow

| 输入 | 类型 | 描述 |
|------|------|------|
| session_id | string | 会话标识符 |
| user_text | string | 用户查询文本 |

| 输出 | 类型 | 描述 |
|------|------|------|
| response | string | 给用户的聚合响应 |
| final_answer | string | 与 response 相同 |
| coordinator_done | bool | 完成标志 |

**DataBus 副作用:**
- 从 user 命名空间读取 `travel_result`, `shopping_result`, `foodie_result`
- 将 `response` 写入 sys 命名空间

## 运行 Agent

### 前置条件
1. 构建项目: `./build.sh build`
2. 确保 SO 库已构建在 `lib/` 目录中

### 运行 E2E 测试
```bash
ctest --test-dir build -R SystemAgentMock --output-on-failure
```

### 预期测试输出
```
=== SystemAgentMock E2E Test ===
Agent path: /path/to/examples/agent/system_agent_mock
All SO libraries found
Agent: system-agent-mock
Agent initialized
Agent started

=== Sending Message ===
Content: What are the best travel, shopping, and food options?

=== Response Received ===
Aggregated response for query: 'What are the best travel, shopping, and food options?'

[Travel Skill]
Travel recommendations for 'What are the best travel, shopping, and food options?': ...

[Shopping Skill]
Shopping suggestions for 'What are the best travel, shopping, and food options?': ...

Total 2 skills contributed to this response.

=== Test PASSED ===
```

## 核心框架机制

| 机制 | 描述 |
|------|------|
| `next_skills` | **首选** - DataBus 键，触发框架启动更多技能 |
| `selected_skills` | **已弃用** - 为向后兼容保留，建议迁移到 `next_skills` |
| `next_skills` (Dispatcher配置) | Dispatcher 配置，指定初始启动的技能 |
| 会话隔离 DataBus | V3 签名提供数据存储的会话隔离 |
| Sys 命名空间 | 框架通信通道，用于配置和响应 |
| User 命名空间 | 用户定义的数据存储，用于技能结果 |

### 动态技能选择机制

技能可以通过在 DataBus 的 sys 命名空间中写入 `next_skills` 来动态触发其他技能的执行：

```cpp
// 推荐：使用 next_skills
client->sys()->put("next_skills", SoDataVariant("[\"travel\", \"shopping\"]"));

// 向后兼容：同时写入 selected_skills
client->sys()->put("selected_skills", SoDataVariant("[\"travel\", \"shopping\"]"));
```

框架会按以下优先级检查：
1. 首先检查 `next_skills`（推荐）
2. 如果未找到，检查 `selected_skills`（向后兼容）

## 文件结构

```
examples/agent/system_agent_mock/
├── agent.json                    # Agent 配置
├── package.json                  # 包元数据
├── README.md                     # 本文档
├── skill_selection_workflow.json # 技能选择工作流
├── travel_workflow.json          # 旅行技能工作流
├── shopping_workflow.json        # 购物技能工作流
├── foodie_workflow.json          # 美食技能工作流
├── coordinator_workflow.json     # 协调器工作流
├── nodes/                        # C++ 节点实现
│   ├── skill_selection_node.cpp
│   ├── travel_node.cpp
│   ├── shopping_node.cpp
│   ├── foodie_node.cpp
│   └── coordinator_node.cpp
├── skills/                       # 技能配置目录
│   ├── skill_selection/
│   ├── travel/
│   ├── shopping/
│   └── foodie/
└── lib/                          # 编译后的 SO 库
    ├── libskill_selection_node.so
    ├── libtravel_node.so
    ├── libshopping_node.so
    ├── libfoodie_node.so
    └── libcoordinator_node.so
```