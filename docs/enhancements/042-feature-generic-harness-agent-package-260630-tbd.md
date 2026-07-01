# 详细设计: 通用 Harness Agent Package — Coordinator-Driven Agent Loop + 参数化 Skill Task

> **文档级别**: L4 技术设计
> **日期**: 2026-06-30
> **状态**: tbd
> **关联**: `examples/agent/harness_agent/`（现有实现）、031-coordinator-协同机制、038-harness-agent-能力升级、040-harness-agent-MCP-support

---

## 1. 背景

### 1.1 当前模型的问题

现有 `harness-agent` 采用 **dispatcher → skill → coordinator** 三层固定 task 模型：

```
agent.json:
  tasks[0]: dispatcher  → next_skills=["harness_agent_run"]  (硬编码)
  tasks[1]: coordinator → coordinator_collect.json            (透传)
  tasks[2]: skill       → harness_agent_run.json             (LLM while-loop)
```

| 问题 | 具体表现 |
|------|---------|
| **Dispatcher 僵化** | `next_skills` 硬编码为 `["harness_agent_run"]`，dispatcher 不做任何决策，仅透传 |
| **Coordinator 退化** | `coordinator_collect.py` 仅输出 `{"coordinator_status": "ok"}`，不做判定/再规划 |
| **Skill 静态绑定** | 每个 skill 需独立 workflow JSON + node 实现 + agent.json task 定义，加 skill 改三处 |
| **无法指派任务** | Coordinator 只能 `next_skills: ["travel"]`，无法传达"用 travel skill 做什么" |
| **无法指派技能** | Skill node 不知道该加载哪个 SKILL.md，技能知识硬编码在 node 或 prompt 中 |
| **单 skill 限制** | 当前 `harness_agent_run` 是唯一 skill，无法并行多 skill |

### 1.2 框架硬性约束

AgentFlow 框架要求每个 session **必须包含三个 task**：

| task_type | task_index | 职责 | 框架依赖 |
|-----------|-----------|------|---------|
| `dispatcher` | 0 | 接收用户请求，启动初始 skill | `handle_task_intent_request` 中 dispatcher 分支；`session->set_dispatcher_task()` |
| `coordinator` | 1 | 接收 skill 完成通知，判定/再规划 | `handle_task_data_exchange` 中 coordinator 分支；`session->set_coordinator_task()` |
| `skill` | ≥2 | 执行具体技能 | `launch_skill_task()`；`pending_skills_count` 计数 |

**不能消除任何一个 task** — 框架的消息路由、完成通知、并发计数等机制都依赖三个 task 的存在。

### 1.3 设计目标

在保留三层 task 的前提下，**将决策权从 dispatcher 转移到 coordinator**：

```
Dispatcher (透传层)
  │  next_skills=["coordinator-skill"]  (固定，仅启动 coordinator)
  ▼
Coordinator Skill (LLM while-loop, 真正的决策中枢)
  │
  ├─ 唯一工具: run_skills
  │     ├─ 传入: skill 名称 + 任务指令 + 应加载的技能
  │     └─ 返回: 所有 skill 的执行结果
  │
  └─ LLM 自主决策: 调用 run_skills / 输出最终 response
```

**核心变化**：

1. **Dispatcher 退化为透传** — 固定 `next_skills=["coordinator-skill"]`，不做任何决策
2. **Coordinator 升级为决策中枢** — 用 LLM while-loop + `run_skills` 工具自主规划
3. **Skill 参数化** — coordinator 通过 `run_skills` 指派"做什么"和"用什么技能"
4. **通用 skill node** — 一个 `general_task_executor.py` 通过入参差异化，消灭 N 份模板代码
5. **工具分层绑定** — coordinator 只绑 `run_skills`；skill task 绑领域工具

---

## 2. 方案简述

### 2.1 架构总览

框架要求每个 session 包含 dispatcher (task_index=0)、coordinator (task_index=1)、skill (task_index≥2) 三个 task，不可省略。本方案在保留三层结构的前提下，**将决策权从 dispatcher 转移到 coordinator**。

### 2.2 核心抽象：general_task_executor

**关键洞察**：coordinator-skill 和 generic-skill 本质上是同一个东西——**LLM function-calling loop**。差异仅在配置：

| 实例 | system prompt 来源 | 绑定工具 | 职责 |
|------|-------------------|---------|------|
| coordinator-skill | agent.md 正文 | `[run_skills]` | 规划与委派 |
| generic-skill | SKILL.md（coordinator 指定） | `[领域工具集]` | 执行具体任务 |

两者共享同一个 Python 实现：`general_task_executor.py`。通过入参控制行为差异：

```python
# coordinator-skill 实例化
general_task_executor(
    system_prompt = agent.md 正文,        # "你是任务编排 AI..."
    tools = [run_skills],                 # 只能委派
    load_skills = [],                     # 不加载额外技能文档
)

# generic-skill 实例化 (由 coordinator 通过 run_skills 指派)
general_task_executor(
    system_prompt = SKILL.md 内容,        # "## Skill: code-review\n..."
    tools = [bash, read_file, write_file, web_fetch, mcp__*],  # 领域工具
    load_skills = ["problem-localization"],  # coordinator 指定
)
```

### 2.3 架构总览

**Coordinator 就是主 Agent**——它思考、分析、回复用户，`run_skills` 是它委派执行的手段。Skill Task 是被委派的执行单元，做完具体工作后把结果交回。

```
用户请求
  │
  ▼
┌─────────────────────────────────────────────────────────────────┐
│  Dispatcher (task_index=0) — 透传层                             │
│  固定 next_skills=["coordinator-skill"]                          │
└──────────────┬──────────────────────────────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────────────────────────────┐
│  Coordinator (主 Agent 循环)                                     │
│  = general_task_executor(                                       │
│      system_prompt = 框架协议 + agent.md 业务意图,              │
│      tools = [run_skills],                                      │
│    )                                                             │
│                                                                  │
│  它就是用户对话的对象：                                          │
│    - 分析用户需求                                                │
│    - 思考该怎么做                                                │
│    - 需要执行时 → 调用 run_skills 委派 skill                    │
│    - 收到结果后 → 继续分析、补充、或直接回复用户                │
│    - 直接回复用户 → 任务完成                                    │
│                                                                  │
│  事件驱动: 每次被触发执行一轮 LLM 决策                          │
│  "循环"由框架 re-trigger 提供 (skill 完成 → re-trigger)        │
│                                                                  │
│  ┌─── run_skills: 委派执行 ──────────────────────────────┐     │
│  │  传入: skill 名称 + 任务指令(prompt) + 技能文档       │     │
│  │  效果: 框架启动 skill task → 并行执行 → 完成后通知    │     │
│  └───────────────────────────────────────────────────────┘     │
└──────────────┬──────────────────────────────────────────────────┘
               │ run_skills → next_skills → 框架启动 skill task × N
               ▼
┌─────────────────────────────────────────────────────────────────┐
│  Skill Task × N (被委派的执行单元, 可并行)                      │
│  = general_task_executor(                                       │
│      system_prompt = SKILL.md (coordinator 指定),               │
│      tools = [领域工具集],                                      │
│      load_skills = [coordinator 指定],                          │
│    )                                                             │
│                                                                  │
│  它是 coordinator 的手：                                        │
│    - 收到 coordinator 指派的 prompt → 知道"做什么"             │
│    - 加载 SKILL.md → 知道"怎么做"                               │
│    - LLM + 技能知识 + 领域工具 → function-calling loop          │
│    - 做完 → 结果交回 coordinator                                │
└─────────────────────────────────────────────────────────────────┘
               │ skill 完成 → 框架 re-trigger coordinator
               ▼
          Coordinator 继续分析...
```

**一句话**：Coordinator 是脑（分析+决策+回复），Skill 是手（执行具体工作），`run_skills` 是脑指挥手的接口。

### 2.4 关键设计决策

**1. Coordinator 为什么是事件驱动而非 while-loop**

框架的 `next_skills` 在 workflow **完成后**才被处理（`process_next_skills_launch` 在 workflow 完成回调中执行）。coordinator-skill 不能在 while-loop 中"等待 skill 结果"——必须先完成 workflow，让框架启动 skill，然后等框架在 skill 完成后 re-trigger 自己。

**2. 为什么 coordinator-skill 和 generic-skill 是同一个 node**

两者都是"LLM + 工具 + while-loop"的 function-calling agent。差异仅在：
- **system prompt**：coordinator-skill 用 agent.md（编排指令）；generic-skill 用 SKILL.md（技能指令）
- **工具绑定**：coordinator-skill 只绑 `run_skills`；generic-skill 绑领域工具
- **技能加载**：coordinator-skill 不加载额外 SKILL.md；generic-skill 按 coordinator 指定加载

统一为一个 `general_task_executor.py` 消灭了"coordinator 节点"和"skill 节点"的代码重复。

### 2.5 核心设计原则

| 原则 | 说明 |
|------|------|
| **三层 task 保留** | 框架硬性要求，dispatcher/coordinator/skill 不可省略 |
| **Dispatcher 退化为透传** | 固定 `next_skills=["coordinator-skill"]`，不做决策 |
| **统一执行器** | coordinator-skill 和 generic-skill 共享 `general_task_executor.py`，差异仅在入参 |
| **Coordinator 事件驱动** | 每次 skill 完成触发一次 LLM 决策，框架 re-trigger 提供循环 |
| **Skill 是被参数化的** | 不再是"我是什么"，而是"coordinator 让我做什么" |
| **技能知识从 SKILL.md 动态加载** | 不硬编码在 node 中，coordinator 指定加载哪些 |
| **工具分层绑定** | coordinator-skill 只绑 `run_skills`；generic-skill 绑领域工具 |
| **框架零改动（Phase 1）** | 所有新逻辑在 Python 节点层实现，复用已有 next_skills 管线 |

---

## 3. 约束与假设

- **Phase 1 不改动 C++ 框架核心层**（`src/agent/`、`src/workflow/`、`src/runtime/`）— 所有新逻辑在 agent package 的 Python 节点层实现
- **复用已有 next_skills 管线** — `parse_next_skills()` + `launch_skill_task()` + `pending_skills_count` + `launched_skills` 完全复用
- **Phase 2 可选框架改动** — 新增 `AF_SKILL_PROMPT` / `AF_LOAD_SKILLS` 环境变量传播（约 20 行 C++），提升通用性
- **向后兼容** — 现有 `harness-agent` 不动，新模型作为 `harness_agent_v2` 共存
- **Skill 执行在 Worker 进程** — Python node 通过 `execution_mode: "python_script"` 执行，多 skill 实例天然并行隔离
- **LLM 调用使用 agent.md 配置** — 模型、base_url、api_key 从 agent.md frontmatter 读取，不硬编码

---

## 4. 当前实现分析

### 4.1 现有 harness-agent 数据流

```
用户请求 → SessionRouter → Session
  → Dispatcher task (intent_request)
      → next_skills=["harness_agent_run"] (硬编码)
      → launch_skill_task("harness_agent_run")
  → Skill task (harness_agent_run)
      → llm_think_and_act.py while-loop
      → LLM + tools (echo, bash, read_file, write_file, web_fetch, load_skill, list_skills, MCP...)
      → 输出 {"response": "..."} → DataBus
  → Coordinator task (data_exchange, skill_response 触发)
      → coordinator_collect.py → {"coordinator_status": "ok"}
      → handle_task_data_exchange 读 DataBus "response" → 返回客户端
```

### 4.2 现有框架注入的环境变量

| 环境变量 | 注入位置 | 说明 |
|---------|---------|------|
| `AF_INPUT` | `runtime.cpp` task poller | 用户原始输入 |
| `AF_DIALOGUE_ID` | `runtime.cpp` task poller | 当前 dialogue ID |
| `AF_SESSION_ID` | `runtime.cpp` task poller | 当前 session ID |
| `AF_DATABUS_UDS_FILE` | `runtime.cpp` worker 启动 | DataBus UDS socket 路径 |

### 4.3 现有框架注入的 node inputs

| input key | 注入位置 | 说明 |
|-----------|---------|------|
| `dialogue_id` | `runtime.cpp:1825` | 从 task parameter 传播 |
| `completed_skill_name` | `runtime.cpp:1830` | 前一个完成的 skill 名称（给 coordinator 用） |
| `skill_success` | `runtime.cpp:1835` | 前一个 skill 是否成功 |
| `exec_skill_key` | `runtime.cpp:1842` | skill 专属数据路由 key |
| `self_task_id` | `runtime.cpp:1846` | 当前 task 的 ID |

### 4.4 可直接复用的框架机制

| 机制 | 位置 | 复用方式 |
|------|------|---------|
| `next_skills` 解析 | `builtin_dispatcher.cpp:parse_next_skills()` | `run_skills` 拦截后构造 next_skills 写入 DataBus |
| `SkillSpec` 格式 | `builtin_dispatcher.h` | `name` + `exec_skill_key` + `force` 完全对齐 |
| Skill 启动 | `agent.cpp:launch_skill_task()` | 直接调用，无需改动 |
| 并发控制 | `task.h:max_concurrent_skills` | 直接复用 |
| 去重 + force | `agent.cpp:launched_skills` + `force` | 直接复用 |
| 完成通知 | `response` 队列 + `data_exchange` trigger | 直接复用 |
| `pending_skills_count` | `agent.cpp` handle_task_data_exchange | 直接复用 |
| `candidate_skills` 白名单 | `builtin_dispatcher.cpp` | 直接复用 |
| `cleanup_dialogue_data` | `builtin_dispatcher.cpp` | 直接复用 |
| DataBus UDS SDK | `src/runtime/databus/clients/python/` | Python 节点直接使用 |

---

## 5. 节点数据交互设计

**核心原则：所有跨 task 的数据传递通过 DataBus，不使用环境变量。**

环境变量仅用于框架注入的固定标识（session_id、dialogue_id 等），不适合传递业务数据——有大小限制、只支持字符串、无法传递结构化数据。DataBus 是框架的正式跨 task 通信机制，支持结构化数据、命名空间隔离、按 dialogue 自动清理。

### 5.1 DataBus Key 命名规范

所有 key 遵循框架已有的路径规范：`/session/{session_id}/{namespace}/{dialogue_id}/{key}`

| 命名空间 | 用途 | 生命周期 |
|---------|------|---------|
| `sys` | 框架/协调层写入的系统数据 | dialogue 级，随 cleanup_dialogue_data 清理 |
| `user` | 业务数据（skill 输入输出） | dialogue 级 |
| `ctx` | 跨轮持久化状态（对话历史、checkpoint） | session 级，跨 dialogue 持久化 |

### 5.2 数据交互总览

```
                    DataBus
                    ┌──────────────────────────────────────────────────────┐
                    │                                                      │
                    │  /session/{sid}/sys/{did}/                           │
                    │    ├── next_skills            ← coordinator 写入     │
                    │    ├── launched_skills        ← 框架写入            │
                    │    ├── pending_skills_count   ← 框架写入            │
                    │    ├── response               ← skill 写入          │
                    │    │                                                │
                    │    ├── coord:dispatch          ← coordinator 写入   │  §5.3
                    │    └── skill_result:{name}     ← skill 写入         │  §5.4
                    │                                                      │
                    │  /session/{sid}/user/{did}/                          │
                    │    └── {exec_skill_key}       ← coordinator 写入   │
                    │                                    skill 读取        │
                    │                                                      │
                    │  /session/{sid}/ctx/                                 │
                    │    ├── coord_messages         ← coordinator 维护    │  §5.5
                    │    └── coord_checkpoint       ← coordinator 维护    │  §5.5
                    └──────────────────────────────────────────────────────┘
```

### 5.3 Coordinator → Skill Task：任务指派（通过 DataBus）

Coordinator 通过 `run_skills` 工具调用，将任务指派信息写入 DataBus 的 `coord:dispatch` key。这是一个结构化 JSON，包含所有待调度 skill 的完整信息：

**DataBus Key**: `/session/{sid}/sys/{did}/coord:dispatch`

**Value 格式**:

```json
{
  "skills": [
    {
      "name": "code-review",
      "prompt": "审查 agent.cpp 的并发安全性",
      "load_skills": ["coder-skills/problem-localization"],
      "exec_skill_key": "review_input",
      "force": false
    },
    {
      "name": "security-scan",
      "prompt": "扫描 SQL 注入风险",
      "load_skills": ["security/codeql-scan"],
      "exec_skill_key": "",
      "force": false
    }
  ],
  "reason": "用户要求代码安全审查",
  "dispatch_round": 2
}
```

**写入时机**：coordinator 的 `run_skills` tool_call 被拦截后：
1. 先写入 `coord:dispatch`（包含每个 skill 的 prompt + load_skills）
2. 再写入 `next_skills`（触发框架调度）
3. skill task 启动时从 `coord:dispatch` 读取属于自己的那份指派信息

**skill task 读取方式**：

```python
# general_task_executor.py 中
dispatch = databus.get("coord:dispatch")  # 读整个 dispatch 记录
dispatch_data = json.loads(dispatch)
# 找到属于自己的 skill 指派
my_spec = next(s for s in dispatch_data["skills"] if s["name"] == skill_name)
skill_prompt = my_spec["prompt"]
load_skills_list = my_spec["load_skills"]
```

**为什么不用环境变量**：
- prompt 可能很长（几百字的任务指令），环境变量有大小限制
- load_skills 是数组，环境变量需要序列化/反序列化
- DataBus 支持命名空间隔离和自动清理，环境变量不会随 dialogue 清理
- 多个并行 skill 各自需要不同的 prompt，环境变量无法区分

### 5.4 Skill Task → Coordinator：结果回传（通过 DataBus）

| 数据 | DataBus Key | 写入方 | 读取方 | 说明 |
|------|------------|--------|--------|------|
| **skill 结果** | node stdout → `skill_result` | general_task_executor.py | 框架 process_node → DataBus | skill 的执行输出 |
| **skill 状态** | node stdout → `status` | general_task_executor.py | 框架 | "completed" / "failed" / "max_rounds_reached" |
| **完成通知** | response 队列 + data_exchange trigger | 框架 | coordinator | `completed_skill_name` + `skill_success` |

**回传链路**（已有，完全复用）：

```
general_task_executor.py stdout → node.cpp 解析 JSON → runtime.cpp process_node
  → DataBus publish → workflow_completed → agent.cpp handle_task_data_exchange
  → coordinator response 队列 → coordinator trigger → coordinator workflow 执行
```

### 5.5 Coordinator 重入状态（通过 DataBus 持久化）

Coordinator 是事件驱动的：每次被框架触发执行一次，然后交出控制权。**重入时必须恢复完整状态**，否则丢失对话上下文和已调度信息。

**持久化的状态**（全部存储在 DataBus `ctx` 命名空间，跨 dialogue 持久化）：

| 状态 | DataBus Key | 类型 | 说明 |
|------|------------|------|------|
| **对话历史** | `/session/{sid}/ctx/coord_messages` | JSON array | 完整的 messages 列表（含 system/user/assistant/tool 消息） |
| **调度检查点** | `/session/{sid}/ctx/coord_checkpoint` | JSON object | 当前轮次、已启动 skill 集合、阶段标记 |

**checkpoint 格式**：

```json
{
  "phase": "evaluating",
  "dispatch_round": 2,
  "launched_skills": ["code-review", "security-scan"],
  "completed_skills": ["code-review"],
  "pending_skills": ["security-scan"],
  "user_input_received": true
}
```

**重入恢复流程**：

```
coordinator-skill 被框架触发
  │
  ├─ ① 从 DataBus 读取 coord_messages → 恢复对话历史
  ├─ ② 从 DataBus 读取 coord_checkpoint → 恢复调度状态
  ├─ ③ 从框架注入的 node inputs 读取 completed_skill_name + skill_success
  │
  ├─ ④ 构造本轮输入:
  │     if phase == "initial":
  │         messages += [{"role":"user", "content": 用户原始请求}]
  │     elif phase == "evaluating":
  │         messages += [{"role":"user", "content": f"Skill {name} 完成，结果: ..."}]
  │
  ├─ ⑤ LLM 决策 (工具: run_skills)
  │     ├─ tool_call: run_skills → 更新 checkpoint → 写 next_skills → 结束
  │     └─ 无 tool_call → 写 response → 结束
  │
  ├─ ⑥ 持久化: 写回 coord_messages + coord_checkpoint 到 DataBus
  └─ ⑦ 输出 coordinator_status → workflow 完成 → 交出控制权
```

---

## 6. Coordinator Agent Runtime 设计

### 6.1 系统提示词槽位设计

**核心问题**：业务开发者在 `agent.md` 中写的是业务意图（"你是代码审查助手，负责..."），不应该知道 `run_skills` 工具的存在、事件驱动重入机制、或框架调度协议。框架必须自动注入 coordinator 协议层。

**系统提示词 = 框架协议槽位 + 业务意图槽位**

```
┌─────────────────────────────────────────────────────┐
│  System Prompt (最终传给 LLM 的)                     │
│                                                      │
│  ┌─── 框架协议槽位 (自动注入, 业务不可见) ───────┐  │
│  │                                               │  │
│  │  ## 你的工作方式                               │  │
│  │  你通过 run_skills 工具委派 skill 执行子任务。 │  │
│  │  你不能直接执行任何操作——只能委派。           │  │
│  │                                               │  │
│  │  ## run_skills 使用方式                        │  │
│  │  - name: skill 名称                           │  │
│  │  - prompt: 告诉 skill 做什么 (必填)           │  │
│  │  - load_skills: skill 应加载的技能文档         │  │
│  │  - exec_skill_key: 结构化输入的 DataBus key   │  │
│  │                                               │  │
│  │  ## 决策规则                                   │  │
│  │  - 需要执行子任务 → 调用 run_skills           │  │
│  │  - 所有子任务完成且结果满意 → 直接回复用户    │  │
│  │  - 结果不完整 → 再次调用 run_skills 补充      │  │
│  │  - 每次只做一轮决策，不要连续调用 run_skills  │  │
│  │                                               │  │
│  └───────────────────────────────────────────────┘  │
│                                                      │
│  ┌─── 业务意图槽位 (来自 agent.md 正文) ─────────┐  │
│  │                                               │  │
│  │  # 代码审查助手                                │  │
│  │  你负责审查代码的安全性和质量。               │  │
│  │  重点关注：并发安全、SQL 注入、内存泄漏。     │  │
│  │                                               │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

**框架协议模板**（硬编码在 `general_task_executor.py` 中，coordinator-skill 实例化时自动注入）：

```python
COORDINATOR_PROTOCOL = """## 你的工作方式

你是一个任务编排 AI，通过 run_skills 工具委派 skill 执行子任务。
你不能直接读取文件、执行命令或搜索——所有具体工作必须委派给 skill。

## run_skills 使用方式

调用 run_skills 时，为每个 skill 指定：
- name (必填): skill 名称，对应 harness/skills/ 下的目录
- prompt (必填): 给这个 skill 的任务指令，告诉它应该做什么
- load_skills (可选): skill 应加载的技能文档名称列表
- exec_skill_key (可选): 结构化输入数据的 DataBus key

## 决策规则

- 需要执行子任务 → 调用 run_skills 委派
- 所有子任务完成且结果满意 → 直接回复用户（不调用工具）
- 结果不完整或有新需求 → 再次调用 run_skills 补充
- 可以在一次 run_skills 中委派多个独立子任务（并行执行）
- 每次只做一轮决策，等待 skill 结果后再做下一轮
"""
```

**agent.md 只写业务意图**：

```markdown
---
name: harness-agent-v2
model:
  provider: openai
  name: gpt-4o-mini
  base_url: https://api.example.com/v1
  api_key: ${API_KEY}
---

# 代码审查助手

你负责审查代码的安全性和质量。
重点关注：并发安全、SQL 注入、内存泄漏。
审查结果应包含具体的问题位置和修复建议。
```

**组装逻辑**：

```python
# general_task_executor.py 中，当 role == "coordinator" 时
system_prompt = COORDINATOR_PROTOCOL + "\n\n---\n\n" + agent_md_body
```

### 6.2 职责与执行机制

Coordinator 是整个 agent loop 的 **决策中枢**，采用 **事件驱动** 模式（非 while-loop）：

**触发时机**：
1. **首次触发**：dispatcher 透传用户请求 → 启动 `coordinator-skill` → skill 完成 → 框架 re-trigger coordinator
2. **后续触发**：每当 coordinator 指派的 skill 完成时，框架自动 re-trigger coordinator

**每次执行（单轮 LLM 调用）**：
1. **恢复重入状态** — 从 DataBus 读取 `coord_messages` + `coord_checkpoint`
2. **构造本轮输入** — 根据 checkpoint.phase 决定是"初始规划"还是"评估结果"
3. **LLM 决策** — 绑定工具 `[run_skills]`，决定下一步
4. **持久化重入状态** — 写回 `coord_messages` + `coord_checkpoint` 到 DataBus
5. **交出控制权** — 输出 coordinator_status，workflow 完成

**"循环"由框架提供**：coordinator 每次执行完 workflow 后，如果产出了 `next_skills`，框架启动新 skill → skill 完成 → re-trigger coordinator → 形成循环。coordinator 自身不需要 Python while-loop。

### 6.2 工具绑定

Coordinator **只绑定一个工具**：`run_skills`。

```python
RUN_SKILLS_TOOL = {
    "type": "function",
    "function": {
        "name": "run_skills",
        "description": (
            "委派一个或多个 skill 去执行子任务。每个 skill 会收到你指定的任务指令和技能文档，"
            "独立完成工作后返回结果。所有 skill 并行执行，全部完成后统一返回。"
            "你可以根据结果决定是否需要再委派新的 skill，或直接回复用户。"
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "skills": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "name": {
                                "type": "string",
                                "description": "Skill 名称，对应 harness/skills/ 下的目录名"
                            },
                            "prompt": {
                                "type": "string",
                                "description": "给这个 skill 的任务指令。"
                                             "告诉 skill executor 应该完成什么具体工作。"
                                             "这是 skill 的核心输入——skill 将围绕这个指令执行。"
                            },
                            "load_skills": {
                                "type": "array",
                                "items": {"type": "string"},
                                "description": "该 skill 应加载的额外技能文档名称列表。"
                                             "这些 SKILL.md 的内容会注入 skill 的 system prompt。"
                            },
                            "exec_skill_key": {
                                "type": "string",
                                "description": "DataBus key，指向该 skill 的结构化输入数据。"
                                             "用于传递不适合放在 prompt 中的大型结构化数据。"
                            },
                            "force": {
                                "type": "boolean",
                                "description": "强制重新执行，即使该 skill 已启动过。默认 false。"
                            }
                        },
                        "required": ["name", "prompt"]
                    },
                    "description": "要执行的 skill 列表，每个 skill 带有任务指派信息"
                },
                "reason": {
                    "type": "string",
                    "description": "为什么需要这些 skill（用于审计和可追溯性）"
                }
            },
            "required": ["skills"]
        }
    }
}
```

**设计要点**：

- `prompt` 是必填项 — coordinator 必须明确告诉 skill "做什么"
- `load_skills` 是可选 — skill 可以不加载额外技能文档（纯工具调用场景）
- `exec_skill_key` 是可选 — 简单任务只需 prompt，复杂任务才需要结构化输入
- `force` 对齐框架已有 `SkillSpec.force` — 允许重跑已完成的 skill

### 6.3 `run_skills` 拦截与框架对接

当 coordinator LLM 产出 `run_skills` tool_call 时，**不作为普通工具执行**，而是拦截并对接框架的 next_skills 管线：

```python
def handle_run_skills_call(args: dict, databus: DataBusClient,
                           session_id: str, dialogue_id: str) -> str:
    """拦截 run_skills tool_call，写入 DataBus，触发框架调度。"""

    skills = args.get("skills", [])
    reason = args.get("reason", "")

    # ① 为每个 skill 写入任务指令和技能列表到 DataBus
    for spec in skills:
        name = spec["name"]
        prompt = spec.get("prompt", "")
        load_list = spec.get("load_skills", [])

        if prompt:
            databus.put(f"skill_prompt:{name}", prompt)
        if load_list:
            databus.put(f"load_skills:{name}", json.dumps(load_list))

    # ② 构造 next_skills 数据（与框架 SkillSpec 格式对齐）
    next_skills = []
    for spec in skills:
        entry = {"name": spec["name"]}
        if spec.get("exec_skill_key"):
            entry["exec_skill_key"] = spec["exec_skill_key"]
        if spec.get("force"):
            entry["force"] = True
        next_skills.append(entry)

    # ③ 写入 next_skills 到 DataBus → 框架接管调度
    databus.put("next_skills", json.dumps(next_skills))

    # ④ 返回确认（coordinator 将在下一轮收到 skill 结果）
    return json.dumps({
        "status": "dispatched",
        "skills": [s["name"] for s in skills],
        "message": f"已调度 {len(skills)} 个 skill，等待执行结果...",
        "reason": reason
    }, ensure_ascii=False)
```

**关键**：步骤 ① 必须在步骤 ③ 之前执行，确保 skill task 启动时 `skill_prompt` 和 `load_skills` 已在 DataBus 中就绪。

### 6.4 Coordinator 事件驱动实现

Coordinator 是 **事件驱动** 的：每次被框架触发时执行一次 LLM 决策，然后交出控制权。框架的 skill 完成 → re-trigger 机制提供"循环"。

```python
#!/usr/bin/env python3
"""
Coordinator Decide — 事件驱动的 coordinator 节点。

每次被框架触发时执行一次 LLM 决策：
  - 首次触发: dispatcher 转发用户请求后，skill 完成触发
  - 后续触发: 每当 coordinator 指派的 skill 完成时，框架 re-trigger

执行逻辑（单轮）:
  1. 从 DataBus 读取已完成的 skill 结果
  2. 恢复跨轮对话历史
  3. LLM 决策 (工具: run_skills)
  4. 持久化对话历史
  5. 输出 next_skills 或 response → 本轮结束，交出控制权
"""

import json, os, sys, yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient

NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")


def parse_agent_md(path):
    if not os.path.isfile(path):
        return {}, ""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    parts = raw.split("---", 2)
    if len(parts) < 3:
        return {}, raw.strip()
    try:
        frontmatter = yaml.safe_load(parts[1]) or {}
    except Exception:
        frontmatter = {}
    return frontmatter, parts[2].strip()


def call_llm(messages, tools=None, model="gpt-4o-mini", base_url="", api_key=""):
    import requests
    url = base_url.rstrip("/") + "/chat/completions"
    headers = {"Content-Type": "application/json", "Authorization": f"Bearer {api_key}"}
    body = {"model": model, "messages": messages, "max_completion_tokens": 2000}
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"
    resp = requests.post(url, json=body, headers=headers, timeout=60)
    resp.raise_for_status()
    return resp.json()


def handle_run_skills_call(args, databus, session_id, dialogue_id):
    """拦截 run_skills tool_call → 写入 DataBus → 触发框架调度。"""
    skills = args.get("skills", [])
    reason = args.get("reason", "")

    # ① 写入每个 skill 的任务指令和技能列表
    for spec in skills:
        name = spec["name"]
        prompt = spec.get("prompt", "")
        load_list = spec.get("load_skills", [])
        if prompt:
            databus.put(f"skill_prompt:{name}", prompt)
        if load_list:
            databus.put(f"load_skills:{name}", json.dumps(load_list))

    # ② 构造 next_skills（对齐 SkillSpec 格式）
    next_skills = []
    for spec in skills:
        entry = {"name": spec["name"]}
        if spec.get("exec_skill_key"):
            entry["exec_skill_key"] = spec["exec_skill_key"]
        if spec.get("force"):
            entry["force"] = True
        next_skills.append(entry)

    # ③ 写入 next_skills → 框架接管
    databus.put("next_skills", json.dumps(next_skills))

    return json.dumps({
        "status": "dispatched",
        "skills": [s["name"] for s in skills],
        "message": f"已调度 {len(skills)} 个 skill，等待执行结果..."
    }, ensure_ascii=False)


def collect_skill_results(databus, launched_skills):
    """从 DataBus 读取已完成的 skill 结果。"""
    results = []
    for name in launched_skills:
        raw = databus.get(f"skill_result:{name}")
        if raw:
            try:
                results.append({"skill": name, "result": json.loads(raw)})
            except json.JSONDecodeError:
                results.append({"skill": name, "result": raw})
    return results


def execute():
    user_input = os.environ.get("AF_INPUT", "")
    dialogue_id = os.environ.get("AF_DIALOGUE_ID", "")
    session_id = os.environ.get("AF_SESSION_ID", "")

    # 框架注入的 skill 完成信息
    completed_skill = os.environ.get("completed_skill_name", "")  # 从 node inputs
    skill_success = os.environ.get("skill_success", "")            # 从 node inputs

    frontmatter, system_prompt = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})

    api_key = model_cfg.get("api_key", "")
    if not api_key or "${" in api_key:
        print(json.dumps({"error": "API key not resolved"}, ensure_ascii=False))
        sys.exit(1)

    databus = DataBusClient()

    # 读取跨轮对话历史
    history = databus.read_messages() if databus.is_connected() else []

    messages = [{"role": "system", "content": system_prompt or
                 "你是一个任务编排 AI。分析用户需求，委派 skill 执行子任务。"}]
    messages.extend(history)

    # 构造本轮输入
    if completed_skill:
        # 后续触发：skill 完成通知
        skill_results = collect_skill_results(databus, [completed_skill])
        messages.append({"role": "user", "content":
            f"Skill '{completed_skill}' 已完成 (success={skill_success})。\n"
            f"结果: {json.dumps(skill_results, ensure_ascii=False, indent=2)}\n"
            f"请评估结果，决定是否需要再委派 skill 或直接回复用户。"})
    else:
        # 首次触发：用户原始请求
        messages.append({"role": "user", "content": user_input})

    # 单轮 LLM 决策
    tools = [RUN_SKILLS_TOOL]  # 只绑 run_skills

    raw = call_llm(
        messages, tools=tools,
        model=model_cfg.get("name", "gpt-4o-mini"),
        base_url=model_cfg.get("base_url", ""),
        api_key=model_cfg.get("api_key", ""))

    choice = raw.get("choices", [{}])[0]
    msg = choice.get("message", {})
    tool_calls = msg.get("tool_calls")

    if tool_calls:
        messages.append(msg)
        for tc in tool_calls:
            func = tc.get("function", {})
            name = func.get("name", "")
            args = json.loads(func.get("arguments", "{}"))

            if name == "run_skills":
                result_str = handle_run_skills_call(args, databus, session_id, dialogue_id)
            else:
                result_str = json.dumps({"error": f"Unknown tool: {name}"})

            messages.append({
                "role": "tool",
                "tool_call_id": tc.get("id", name),
                "content": result_str,
            })

        # 持久化对话历史（coordinator 交出控制权，等框架 re-trigger）
        messages_to_save = messages[1:]
        if databus.is_connected():
            databus.write_messages(messages_to_save)

        # 输出 coordinator_status（不写 response，避免覆盖 skill 结果）
        print(json.dumps({"coordinator_status": "skills_dispatched"},
                         ensure_ascii=False))
    else:
        # LLM 无 tool_call → 输出最终响应
        response_text = msg.get("content", "")
        print(json.dumps({"response": response_text}, ensure_ascii=False))

        messages_to_save = messages[1:]
        if databus.is_connected():
            databus.write_messages(messages_to_save)


if __name__ == "__main__":
    execute()
```

### 6.5 Coordinator Workflow JSON

```json
{
  "id": "task-executor-workflow",
  "version": "2.0",
  "name": "Coordinator Loop",
  "nodes": [
    {
      "id": "coordinator",
      "name": "Coordinator Agent Loop",
      "type": "task",
      "execution_mode": "python_script",
      "command": "./nodes/general_task_executor.py",
      "working_dir": "examples/agent/harness_agent_v2",
      "json_output": true,
      "inputs": [
        {"name": "input",               "type": "string", "required": true},
        {"name": "session_id",          "type": "string", "required": true},
        {"name": "dialogue_id",         "type": "string", "required": false},
        {"name": "completed_skill_name", "type": "string", "required": false},
        {"name": "skill_success",       "type": "string", "required": false}
      ],
      "outputs": [
        {"name": "response", "type": "string"}
      ],
      "timeout_ms": 60000
    }
  ],
  "edges": []
}
```

---

## 7. Skill Task Agent Runtime 设计

### 7.1 职责定义

Skill Task 是 **被参数化的 mini-agent**，职责：

1. **读取任务指令** — 从 `AF_SKILL_PROMPT` / DataBus 获取 coordinator 指派的 prompt
2. **加载技能知识** — 从 `load_skills` 列表加载 SKILL.md，注入 system prompt
3. **获取结构化输入** — 从 `exec_skill_key` 指向的 DataBus key 获取输入数据
4. **执行任务** — LLM + 技能知识 + 领域工具 while-loop
5. **输出结果** — 结构化结果写入 DataBus，通知 coordinator

### 7.2 工具绑定

Skill Task 绑定 **领域工具集**，从 `harness/tools/` + MCP 自动发现：

```python
# general_task_executor.py 的工具来源
tools = tool_registry.get_tools()  # 本地工具 + MCP 工具
```

**与 coordinator 的工具隔离**：

| 角色 | 绑定工具 | 说明 |
|------|---------|------|
| Coordinator | `[run_skills]` | 只做规划和委派，不直接操作文件/搜索/执行 |
| Skill Task | `[echo, bash, read_file, write_file, web_fetch, load_skill, list_skills, mcp__*]` | 做具体工作，操作文件、搜索、执行命令 |

**隔离方式**：两个 node 是独立的 Python 进程（不同 workflow 实例），各自 `import tools as tool_registry` 时绑定不同的工具子集。具体实现：

```python
# general_task_executor.py
tools = [RUN_SKILLS_TOOL]  # 只绑 run_skills

# general_task_executor.py
tools = tool_registry.get_tools()  # 绑领域工具集（不含 run_skills）
```

### 7.3 入参契约

Skill Executor 的所有输入通过 **框架环境变量** + **DataBus** 获取：

| 输入 | 来源 | 获取方式 | 说明 |
|------|------|---------|------|
| `AF_INPUT` | 框架注入 | `os.environ` | coordinator 传入的原始消息（兜底） |
| `AF_DIALOGUE_ID` | 框架注入 | `os.environ` | 当前 dialogue ID |
| `AF_SESSION_ID` | 框架注入 | `os.environ` | 当前 session ID |
| `AF_SKILL_NAME` | 框架注入 (Phase 2) / DataBus (Phase 1) | `os.environ` / DataBus | 当前 skill 名称 |
| `skill_prompt:{name}` | coordinator 写入 | DataBus `sys` namespace | coordinator 指派的任务指令 |
| `load_skills:{name}` | coordinator 写入 | DataBus `sys` namespace | 应加载的技能文档列表 |
| `exec_skill_key` | 框架注入 | node inputs → `os.environ` | 结构化输入的 DataBus key |
| `/session/{sid}/user/{did}/{exec_skill_key}` | coordinator 写入 | DataBus `user` namespace | 结构化输入数据 |

### 7.4 Skill Executor 主循环实现

```python
#!/usr/bin/env python3
"""
Universal Skill Executor — 通用的参数化 skill task node。

被 coordinator 通过 run_skills 派发，接收：
  - 任务指令 (skill_prompt): 告诉这个 skill "做什么"
  - 技能文档 (load_skills): 告诉这个 skill "怎么做"
  - 结构化输入 (exec_skill_key): 传递大型/结构化数据

执行 LLM + 技能知识 + 领域工具的 while-loop，输出结果。
"""

import json, os, sys, yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient

NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")
SKILLS_BASE = os.path.join(AGENT_PACKAGE_DIR, "harness", "skills")

# 加载领域工具集
sys.path.insert(0, os.path.join(AGENT_PACKAGE_DIR, "harness"))
import tools as tool_registry


def get_env(key, default=""):
    return os.environ.get(key, default)


def parse_agent_md(path):
    if not os.path.isfile(path):
        return {}, ""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    parts = raw.split("---", 2)
    if len(parts) < 3:
        return {}, raw.strip()
    try:
        frontmatter = yaml.safe_load(parts[1]) or {}
    except Exception:
        frontmatter = {}
    return frontmatter, parts[2].strip()


# ── SKILL.md 加载 ─────────────────────────────────────────

def load_skill_md(skill_name):
    """读取 harness/skills/{skill_name}/SKILL.md"""
    path = os.path.join(SKILLS_BASE, skill_name, "SKILL.md")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        return f.read()


def load_skills_from_list(skill_names):
    """批量加载 SKILL.md，返回 {name: content}"""
    result = {}
    for name in skill_names:
        content = load_skill_md(name)
        result[name] = content if content else f"[Skill '{name}' 文档未找到]"
    return result


# ── 结构化输入解析 ────────────────────────────────────────

def resolve_skill_input(databus, exec_skill_key):
    """从 DataBus 读取 exec_skill_key 指向的结构化输入。"""
    if not exec_skill_key or not databus.is_connected():
        return {}
    raw = databus.get(exec_skill_key)
    if not raw:
        return {}
    try:
        return json.loads(raw) if isinstance(raw, str) else raw
    except json.JSONDecodeError:
        return {"raw_input": raw}


# ── LLM 调用 ─────────────────────────────────────────────

def call_llm(messages, tools=None, model="gpt-4o-mini", base_url="", api_key=""):
    import requests
    url = base_url.rstrip("/") + "/chat/completions"
    headers = {"Content-Type": "application/json", "Authorization": f"Bearer {api_key}"}
    body = {"model": model, "messages": messages, "max_completion_tokens": 2000}
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"
    resp = requests.post(url, json=body, headers=headers, timeout=60)
    resp.raise_for_status()
    return resp.json()


# ── 主执行逻辑 ────────────────────────────────────────────

def execute():
    # ① 读取框架注入的环境变量
    user_input     = get_env("AF_INPUT", "")
    dialogue_id    = get_env("AF_DIALOGUE_ID", "")
    session_id     = get_env("AF_SESSION_ID", "")
    exec_skill_key = get_env("exec_skill_key", "")

    databus = DataBusClient()

    # ② 推断当前 skill_name
    #    Phase 1: 从 DataBus 已写入的 skill_prompt:* key 推断
    #    Phase 2: 框架直接注入 AF_SKILL_NAME
    skill_name = get_env("AF_SKILL_NAME", "")
    if not skill_name:
        # 兜底：从 exec_skill_key 或 user_input 推断
        skill_name = get_env("AF_SKILL_NAME_FALLBACK", "generic-skill")

    # ③ 从 DataBus 读取 coordinator 指派的任务指令
    skill_prompt = ""
    if databus.is_connected():
        raw = databus.get(f"skill_prompt:{skill_name}")
        if raw:
            skill_prompt = raw

    # ④ 从 DataBus 读取应加载的技能列表
    load_skill_names = []
    if databus.is_connected():
        raw = databus.get(f"load_skills:{skill_name}")
        if raw:
            try:
                load_skill_names = json.loads(raw)
            except json.JSONDecodeError:
                pass

    # 确保自身技能文档被加载
    if skill_name and skill_name not in load_skill_names:
        load_skill_names.insert(0, skill_name)

    # ⑤ 解析结构化输入
    skill_input = resolve_skill_input(databus, exec_skill_key)

    # ⑥ 确定最终的任务指令
    task_instruction = skill_prompt \
                    or skill_input.get("prompt", "") \
                    or user_input

    # ⑦ 加载技能文档 → 构造 system prompt
    skill_docs = load_skills_from_list(load_skill_names)
    system_parts = []
    for name, content in skill_docs.items():
        system_parts.append(f"## Skill: {name}\n\n{content}")
    system_prompt = "\n\n---\n\n".join(system_parts) if system_parts else \
                    "你是一个任务执行 agent。完成指派给你的任务。"

    # ⑧ 构造 LLM 消息
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user",   "content": task_instruction},
    ]
    if skill_input.get("context"):
        messages.append({"role": "user", "content":
            f"附加上下文:\n{json.dumps(skill_input['context'], ensure_ascii=False)}"})

    # ⑨ 获取领域工具集（不含 run_skills）
    available_tools = tool_registry.get_tools()

    # ⑩ 读取模型配置
    frontmatter, _ = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    api_key = model_cfg.get("api_key", "")
    if not api_key or "${" in api_key:
        print(json.dumps({"error": "API key not resolved"}, ensure_ascii=False))
        sys.exit(1)

    # ⑪ LLM while-loop 执行任务
    max_rounds = int(get_env("AF_SKILL_MAX_ROUNDS", "8"))
    for round_i in range(max_rounds):
        raw = call_llm(
            messages, tools=available_tools,
            model=model_cfg.get("name", "gpt-4o-mini"),
            base_url=model_cfg.get("base_url", ""),
            api_key=model_cfg.get("api_key", ""))

        choice = raw.get("choices", [{}])[0]
        msg = choice.get("message", {})
        tool_calls = msg.get("tool_calls")

        if tool_calls:
            messages.append(msg)
            for tc in tool_calls:
                func = tc.get("function", {})
                name = func.get("name", "")
                try:
                    args = json.loads(func.get("arguments", "{}"))
                except json.JSONDecodeError:
                    args = {}
                result = tool_registry.execute(name, args)
                messages.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id", name),
                    "content": result,
                })
        else:
            # 任务完成 → 输出结果
            response_text = msg.get("content", "")
            output = {
                "skill_result": response_text,
                "status": "completed",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
            }
            print(json.dumps(output, ensure_ascii=False))
            return

    # 超过最大轮次
    output = {
        "skill_result": messages[-1].get("content", "") if messages else "",
        "status": "max_rounds_reached",
        "skill_name": skill_name,
        "rounds_used": max_rounds,
    }
    print(json.dumps(output, ensure_ascii=False))


if __name__ == "__main__":
    execute()
```

### 7.5 Skill Workflow JSON

```json
{
  "id": "task-executor-workflow",
  "version": "2.0",
  "name": "Generic Skill Executor",
  "nodes": [
    {
      "id": "skill_executor",
      "name": "Universal Skill Executor",
      "type": "task",
      "execution_mode": "python_script",
      "command": "./nodes/general_task_executor.py",
      "working_dir": "examples/agent/harness_agent_v2",
      "json_output": true,
      "inputs": [
        {"name": "input",               "type": "string", "required": true},
        {"name": "session_id",          "type": "string", "required": true},
        {"name": "dialogue_id",         "type": "string", "required": false},
        {"name": "exec_skill_key",      "type": "string", "required": false},
        {"name": "skill_name",          "type": "string", "required": false},
        {"name": "completed_skill_name", "type": "string", "required": false},
        {"name": "skill_success",       "type": "string", "required": false}
      ],
      "outputs": [
        {"name": "skill_result", "type": "string"},
        {"name": "status",       "type": "string"}
      ],
      "timeout_ms": 60000
    }
  ],
  "edges": []
}
```

---

## 8. Agent Package 配置

### 8.1 agent.json

```json
{
  "name": "HarnessAgentV2",
  "version": "2.0.0",
  "description": "Coordinator-driven agent loop with parameterized skill tasks",
  "session_pool_size": 1,
  "max_workflows": 10,
  "max_concurrent_workflows": 5,
  "data_bus_capacity_mb": 5,
  "tasks": [
    {
      "id": 0,
      "type": "dispatcher",
      "description": "Passthrough — forwards to coordinator-skill",
      "next_skills": ["coordinator-skill"],
      "max_concurrent_skills": 5,
      "workflow_pool_size": 1
    },
    {
      "id": 1,
      "type": "coordinator",
      "description": "Event-driven coordinator — evaluates results, dispatches skills",
      "workflow_file": "task_executor_workflow.json",
      "workflow_pool_size": 1
    },
    {
      "id": 2,
      "type": "skill",
      "description": "Coordinator skill — LLM decision loop (event-driven)",
      "workflow_file": "coordinator_skill_workflow.json",
      "skill": {
        "name": "coordinator-skill",
        "instance_pool_size": 1
      },
      "workflow_pool_size": 1
    },
    {
      "id": 3,
      "type": "skill",
      "description": "Generic parameterized skill executor",
      "workflow_file": "skill_workflow.json",
      "skill": {
        "name": "generic-skill",
        "instance_pool_size": 5
      },
      "workflow_pool_size": 5
    }
  ]
}
```

**关键变化**：

| 字段 | 旧值 | 新值 | 说明 |
|------|------|------|------|
| `tasks[0].next_skills` | `["harness_agent_run"]` | `["coordinator-skill"]` | dispatcher 不直接启动业务 skill，而是启动 coordinator-skill |
| `tasks[0].max_concurrent_skills` | N/A | `5` | dispatcher 可并行调度 5 个 skill（含 coordinator-skill） |
| `tasks[2]` (新增) | N/A | `coordinator-skill` | coordinator 的 LLM 决策作为 skill task 执行，结果触发框架 coordinator task |
| `tasks[3].skill.name` | `"harness_agent_run"` | `"generic-skill"` | 通用 skill，不绑定具体业务 |
| `tasks[3].skill.instance_pool_size` | `1` | `5` | 允许 5 个 skill 实例并行 |
| `tasks[3].workflow_pool_size` | `1` | `5` | 5 个 workflow 实例并行 |

**架构说明**：

- **Dispatcher (task_index=0)**：透传层，固定 `next_skills=["coordinator-skill"]`，不做任何决策
- **Coordinator (task_index=1)**：框架级协调，接收 skill 完成通知，读 DataBus 判定结果
- **Coordinator-skill (task_index=2)**：coordinator 的 LLM 决策逻辑，作为 skill 执行。每次被 dispatcher 或自身产出的 next_skills 启动，执行一次 LLM 决策后结束
- **Generic-skill (task_index=3)**：参数化的业务 skill executor，由 coordinator-skill 通过 `run_skills` 调度

### 8.2 agent.md

```markdown
---
name: harness-agent-v2
description: "Coordinator-driven agent loop with parameterized skill tasks"
model:
  provider: openai
  name: gpt-4o-mini
  base_url: https://api.example.com/v1
  api_key: ${API_KEY}
skills:
  - code-review
  - problem-localization
  - security-scan
  - echo
---

# Harness Agent V2

你是一个任务编排 AI。你的职责是：

1. **分析用户需求** — 理解用户想要完成什么
2. **规划 skill 调度** — 决定需要哪些 skill，每个 skill 做什么子任务
3. **委派执行** — 通过 run_skills 工具将子任务分配给 skill
4. **评估结果** — 收到 skill 结果后，判断是否需要补充或可以完成
5. **输出响应** — 任务完成时，综合所有 skill 结果回复用户

## 规则
- 你只能通过 run_skills 委派 skill 执行子任务，不能直接操作文件或执行命令
- 每个 skill 必须收到明确的 prompt，告诉它应该做什么
- 如果需要多个独立子任务，可以在一次 run_skills 中并行委派
- 如果 skill 结果不完整，可以再次调用 run_skills 补充
- 当所有子任务完成且结果满意时，直接回复用户
```

---

## 9. 完整数据流时序

### 9.1 单 skill 场景

```
用户: "审查 src/agent/agent.cpp 的并发安全性"

T1  Coordinator Loop
    │
    ├─ LLM 决策: run_skills({
    │     skills: [{
    │       name: "code-review",
    │       prompt: "审查 src/agent/agent.cpp 中 launch_skill_task 方法的并发安全性，重点关注 mutex 和 atomic 的使用",
    │       load_skills: ["coder-skills/problem-localization"]
    │     }]
    │   })
    │
    ├─ handle_run_skills_call:
    │   ├─ DataBus.put("skill_prompt:code-review", "审查 src/agent/agent.cpp ...")
    │   ├─ DataBus.put("load_skills:code-review", '["coder-skills/problem-localization"]')
    │   └─ DataBus.put("next_skills", '[{"name":"code-review"}]')
    │
    └─ 返回 tool_result: {"status":"dispatched","skills":["code-review"],...}

T2  框架接管
    │
    ├─ handle_task_data_exchange 读 next_skills
    ├─ launch_skill_task("code-review", exec_skill_key="", ...)
    │   ├─ message.metadata["skill_name"] = "code-review"
    │   └─ task.set_parameter("message_content", coordinator 传入的消息)
    │
    └─ runtime task poller 调度 skill workflow

T3  Skill Task: general_task_executor.py
    │
    ├─ 读 DataBus "skill_prompt:code-review" → "审查 src/agent/agent.cpp ..."
    ├─ 读 DataBus "load_skills:code-review" → ["coder-skills/problem-localization"]
    ├─ load_skill_md("code-review") → SKILL.md 内容
    ├─ load_skill_md("coder-skills/problem-localization") → SKILL.md 内容
    │
    ├─ system_prompt = "## Skill: code-review\n...\n---\n## Skill: problem-localization\n..."
    ├─ user_message = "审查 src/agent/agent.cpp 中 launch_skill_task 方法的并发安全性..."
    │
    ├─ LLM while-loop:
    │   ├─ call_llm → tool_call: read_file({"path": "src/agent/agent.cpp"})
    │   ├─ execute → 文件内容
    │   ├─ call_llm → tool_call: bash({"command": "grep -n mutex src/agent/agent.cpp"})
    │   ├─ execute → grep 结果
    │   └─ call_llm → 最终分析文本
    │
    └─ stdout: {"skill_result": "并发安全性审查结果...", "status": "completed"}

T4  框架回传
    │
    ├─ node.cpp 解析 stdout → outputs["skill_result"] = "..."
    ├─ runtime.cpp process_node → DataBus publish
    ├─ workflow_completed → agent.cpp handle_task_data_exchange
    │   ├─ pending_skills_count-- → 0
    │   └─ 触发 coordinator: completed_skill_name="code-review", skill_success="true"
    │
    └─ coordinator workflow 再次执行

T5  Coordinator Loop (第二轮)
    │
    ├─ 读取 skill 结果: {"skill":"code-review","result":"并发安全性审查结果..."}
    ├─ messages.append({"role":"user","content":"Skill 执行结果:..."})
    │
    └─ LLM 决策: 无 tool_call → 输出最终 response
        → {"response": "审查完成。launch_skill_task 方法在以下方面存在并发风险：..."}
```

### 9.2 并行多 skill 场景

```
用户: "审查代码安全性并搜索相关 CVE"

T1  Coordinator Loop
    │
    └─ LLM 决策: run_skills({
          skills: [
            {name:"code-review", prompt:"审查 SQL 注入和 XSS 风险", load_skills:["security/codeql-scan"]},
            {name:"web-search",   prompt:"搜索 2026 年 C++ 并发安全 CVE", load_skills:["browser-skills/web-search"]}
          ]
        })

T2  handle_run_skills_call:
    ├─ DataBus.put("skill_prompt:code-review", "审查 SQL 注入...")
    ├─ DataBus.put("load_skills:code-review", '["security/codeql-scan"]')
    ├─ DataBus.put("skill_prompt:web-search", "搜索 2026 年...")
    ├─ DataBus.put("load_skills:web-search", '["browser-skills/web-search"]')
    └─ DataBus.put("next_skills", '[{"name":"code-review"},{"name":"web-search"}]')

T3  框架并行启动两个 skill task:
    ├─ Skill Task 1: code-review → general_task_executor.py (进程 A)
    └─ Skill Task 2: web-search  → general_task_executor.py (进程 B)

T4  两个 skill 各自执行 LLM while-loop，结果写入 DataBus

T5  两个 skill 都完成 → pending_skills_count=0 → 触发 coordinator

T6  Coordinator 聚合两个 skill 结果 → LLM 评估 → 输出最终 response
```

---

## 10. 框架侧改动（Phase 2，可选）

Phase 1 所有新逻辑在 Python 节点层实现，**零框架改动**。Phase 2 可选新增以下环境变量传播，提升通用性：

### 10.1 新增 `AF_SKILL_NAME` 环境变量传播

**当前**：`skill_name` 存在于 `task.skill_name`，但未传播到 node 的环境变量。

**改动**：

```cpp
// runtime.cpp — task poller 中，skill task 注入 AF_SKILL_NAME
if(task_type == "skill") {
    std::string skill_name = task->get_skill_name();
    if(!skill_name.empty()) {
        inputs["skill_name"] = skill_name;
        // Python node 可通过 os.environ["AF_SKILL_NAME"] 或 inputs["skill_name"] 获取
    }
}
```

**改动量**：约 5 行 C++。

### 10.2 新增 `AF_SKILL_PROMPT` 传播

**当前**：`skill_prompt` 由 coordinator 写入 DataBus `skill_prompt:{name}` key，skill executor 从 DataBus 读取。

**Phase 2 优化**：将 `skill_prompt` 通过 message metadata → task parameter → node inputs 传播，减少一次 DataBus 读取。

```cpp
// agent.cpp — launch_skill_task 中
skill_msg.metadata["skill_prompt"] = skill_prompt;  // 新增

// agent.cpp — build_runtime_task 中
if(const auto it = message.metadata.find("skill_prompt"); it != message.metadata.end()) {
    task->set_parameter("skill_prompt", it->second);
}

// runtime.cpp — task poller 中
std::string skill_prompt;
if(task->get_parameter("skill_prompt", skill_prompt) && !skill_prompt.empty()) {
    inputs["skill_prompt"] = skill_prompt;
}
```

**改动量**：约 15 行 C++。

### 10.3 新增 `AF_LOAD_SKILLS` 传播

与 `AF_SKILL_PROMPT` 类似的传播链路。

**改动量**：约 15 行 C++。

---

## 11. 目录结构

```
examples/agent/harness_agent_v2/
├── agent.json                    # agent package 配置（§8.1）
├── agent.md                      # system prompt + 模型配置（§8.2）
├── task_executor_workflow.json         # coordinator workflow（§6.5）
├── skill_workflow.json           # 通用 skill workflow（§7.5）
├── nodes/
│   ├── general_task_executor.py       # coordinator 主循环（§6.4）
│   ├── general_task_executor.py         # 通用 skill executor（§7.4）
│   ├── databus_client.py         # DataBus UDS 客户端（复用）
│   └── llm_trace_recorder.py    # LLM 调用追踪（复用）
├── harness/
│   ├── tools/                    # 领域工具集（skill task 绑定）
│   │   ├── __init__.py           # 工具注册 + MCP 发现（复用）
│   │   ├── bash.py
│   │   ├── echo.py
│   │   ├── read_file.py
│   │   ├── write_file.py
│   │   ├── web_fetch.py
│   │   ├── load_skill.py
│   │   ├── list_skills.py
│   │   └── mcp_bridge.py
│   └── skills/                   # 技能文档目录
│       ├── code-review/
│       │   └── SKILL.md
│       ├── problem-localization/
│       │   └── SKILL.md
│       ├── security-scan/
│       │   └── SKILL.md
│       ├── web-search/
│       │   └── SKILL.md
│       └── echo/
│           └── SKILL.md
├── test_client.py                # 测试客户端
└── README.md
```

---

## 12. 与现有 harness-agent 的对比

| 维度 | harness-agent (V1) | harness-agent-v2 (本方案) |
|------|-------------------|--------------------------|
| **架构** | dispatcher → skill → coordinator (三层) | dispatcher(透传) → coordinator-skill → coordinator(事件驱动) → skill(参数化) |
| **决策方** | dispatcher (硬编码 next_skills) | coordinator-skill LLM (自主规划, 事件驱动) |
| **Skill 数量** | 1 个固定 skill | N 个动态 skill，按需调度 |
| **Skill 绑定** | 每个 skill 独立 workflow + node | 一个通用 general_task_executor.py |
| **任务指派** | 无法指派（skill 不知道"做什么"） | prompt + load_skills 参数化 |
| **工具绑定** | skill 绑所有工具 | coordinator 只绑 run_skills；skill 绑领域工具 |
| **并行** | 单 skill | 多 skill 并行（instance_pool_size=5） |
| **再规划** | coordinator 透传，无再规划 | coordinator LLM 评估结果后可再委派 |
| **加 skill** | 改 agent.json + 写 workflow + 写 node | 放 SKILL.md 到 harness/skills/ |
| **终止语义** | DataBus 键存在性检查 | LLM 自主决定不再调用 run_skills |

---

## 13. 测试方案

### 13.1 E2E 测试场景

| TC | 场景 | 步骤 | 预期 |
|----|------|------|------|
| 1 | 单 skill 委派 | 用户请求 → coordinator 调 run_skills → 1 个 skill 完成 → coordinator 输出 | 最终 response 包含 skill 结果 |
| 2 | 并行多 skill | coordinator 一次 run_skills 委派 2 个 skill | 两个 skill 并行执行，结果都被聚合 |
| 3 | 多轮再规划 | 第一轮 skill 结果不完整 → coordinator 再次 run_skills | 第二轮 skill 被调度，最终结果完整 |
| 4 | Skill 加载技能文档 | coordinator 指定 load_skills | skill_executor 的 system prompt 包含 SKILL.md 内容 |
| 5 | Skill 使用领域工具 | skill LLM 调用 read_file / bash | 工具正常执行，结果回填 |
| 6 | Coordinator 不直接操作 | coordinator LLM 尝试调 read_file | 工具不存在，LLM 被迫用 run_skills |
| 7 | 最大轮次保护 | coordinator 持续调 run_skills 超过上限 | 达到 max_rounds 后输出部分结果 |
| 8 | Skill 最大轮次 | skill LLM 持续调工具超过上限 | skill 输出 status=max_rounds_reached |
| 9 | exec_skill_key 路由 | coordinator 传入 exec_skill_key | skill 从 DataBus 读取结构化输入 |
| 10 | force 重跑 | coordinator 对已完成 skill 设 force=true | skill 被重新执行 |

### 13.2 单元测试

| 测试 | 验证点 |
|------|--------|
| `test_handle_run_skills_call` | DataBus 写入 skill_prompt / load_skills / next_skills 的 key 和值正确 |
| `test_collect_skill_results` | 从 DataBus 正确读取多个 skill 的结果 |
| `test_load_skill_md` | SKILL.md 文件存在/不存在时的行为 |
| `test_resolve_skill_input` | exec_skill_key 有值/无值/DataBus 不可用时的行为 |
| `test_run_skills_tool_schema` | 工具定义 JSON Schema 合法，required 字段正确 |

---

## 14. 实施计划

### Phase 1: Python 节点层实现（零框架改动）

| 步骤 | 内容 | 优先级 |
|------|------|--------|
| 1 | 创建 `harness_agent_v2` 目录结构 | P1 |
| 2 | 实现 `general_task_executor.py`（含 `run_skills` 工具定义 + 拦截逻辑） | P1 |
| 3 | 实现 `general_task_executor.py`（含 SKILL.md 加载 + 结构化输入解析 + LLM while-loop） | P1 |
| 4 | 编写 `task_executor_workflow.json` + `skill_workflow.json` + `agent.json` + `agent.md` | P1 |
| 5 | 复用 `harness/tools/` + `databus_client.py` + `llm_trace_recorder.py` | P1 |
| 6 | E2E 测试：单 skill + 多 skill + 多轮再规划 | P1 |
| 7 | 编写 README.md | P2 |

### Phase 2: 框架侧优化（可选，低优先级）

| 步骤 | 内容 | 优先级 |
|------|------|--------|
| 1 | 新增 `AF_SKILL_NAME` 环境变量传播（runtime.cpp 约 5 行） | P3 |
| 2 | 新增 `AF_SKILL_PROMPT` 传播链路（agent.cpp + runtime.cpp 约 15 行） | P3 |
| 3 | 新增 `AF_LOAD_SKILLS` 传播链路（约 15 行） | P3 |
| 4 | 单元测试覆盖新增传播 | P3 |

---

## 15. 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [031-coordinator-协同机制](031-feature-coordinator-协同机制设计分析-260405-tbd.md) | 本方案是 031 中"coordinator 自主规划 + 事件驱动"的具体落地 |
| [038-harness-agent-能力升级](038-feature-harness-agent-能力升级-260621-tbd.md) | 本方案是 038 的架构升级版，从"单 skill + load_skills 工具"走向"多 skill + run_skills 委派" |
| [040-harness-agent-MCP](040-feature-harness-agent-mcp-support-260622-tbd.md) | MCP 工具桥接层完全复用，general_task_executor.py 的领域工具集包含 MCP 工具 |
| [023-next-skills-格式扩展](023-feature-next-skills-格式扩展-260319-implemented.md) | `run_skills` 产出的 next_skills 格式与 023 完全对齐（SkillSpec） |
| [009-coordinator-task0-默认化](009-feature-mmnode-执行引擎下沉-coordinator-task0-默认化-260228-implemented.md) | 本方案延续 009 的 coordinator 作为 task_index=0 的设计 |

---

## 16. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-06-30 | AtomCode | 初始版本：完整设计文档，含节点数据交互、coordinator/skill runtime、工具绑定、数据流时序、实施计划 |
