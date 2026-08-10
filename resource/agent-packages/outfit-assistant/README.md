# Outfit Assistant

穿搭助手 — 基于 AgentFlow 框架的 coordinator-driven agent，结合天气、场合和用户画像提供穿搭建议。

> **执行架构**：对齐 `docs/design/agent-template-delta.md` 的"四层架构：模版 → pip 包 + 入口文件"。
> daemon 直接 `execve` `nodes/*.py` 入口文件（`execution_mode: python_script`），入口文件只有一行 `from agentflow_runtime.* import ...`，真正实现在 pip 包 `agentflow-runtime` 中。
> 没有 shell wrapper、没有 sentinel 检测——venv 由 `setup.sh` 手动初始化，框架 worker 通过 `PYTHON_PATH` 找 `.venv/bin/python3`。

## 完整执行全景图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          用 户 请 求                                         │
│              "北京今天天气如何，适合穿什么"                                      │
└────────────────────────┬────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  ┌─ agent.json ───────────────────────────────────────────────────────────┐ │
│  │  id=0 dispatcher  → next_skills=["dummy_pre_skill"]                   │ │
│  │  id=1 coordinator (coordinator_workflow.json)                          │ │
│  │  id=2 skill: dummy_pre_skill (dummy_pre_skill_workflow.json)           │ │
│  │  id=3 skill: skill-runner (skill_workflow.json)                        │ │
│  └─────────────────────────────────────────────────────────────────────────┘ │
│                                                                              │
│  daemon (C++) → 接收 JSON-RPC → 创建 session → 路由给 dispatcher task       │
└────────────────────────┬────────────────────────────────────────────────────┘
                         │
          ┌──────────────┴──────────────┐
          │                             │
          ▼                             ▼
┌─────────────────────┐   ┌─────────────────────────────┐
│  STEP 1              │   │  STEP 2                     │
│  dispatcher (id=0)   │──▶│  dummy_pre_skill (id=2)     │
│  task_type=dispatcher │   │  task_type=skill            │
│  (框架内置，无节点)    │   │  ↓                          │
│                     │   │  dummy_pre_skill.py          │
│  next_skills:       │   │  ─────────────────           │
│  ["dummy_pre_skill"]│   │  入口文件（1 行 import）      │
│                     │   │  纯 passthrough，无 next_skills│
│  生成 dialogue_id   │   │  框架自动 forward 给        │
│  写 user_prompt 到  │   │  coordinator task            │
│  DataBus            │   │                              │
└─────────────────────┘   └──────────┬───────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  STEP 3: coordinator (id=1) — coordinator_workflow.json                     │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  coordinator_decide.py                                                │  │
│  │  ─────────────────────                                                │  │
│  │                                                                       │  │
│  │  ① 读 .env → API_KEY / AF_LLM_BASE_URL                               │  │
│  │                                                                       │  │
│  │  ② 读 agent.md frontmatter → model_name = "MiniMax-M3"                │  │
│  │                                                                       │  │
│  │  ③ 构建 system prompt =                                                │  │
│  │     COORDINATOR_PROTOCOL + agent.md body                               │  │
│  │     (描述：可用 weather_query / outfit-advisor / occasion-guide skill) │  │
│  │                                                                       │  │
│  │  ④ LLM 调用 (MiniMax-M3, tools=[run_skills])                          │  │
│  │     ├─ finish=tool_calls → 调 run_skills → 走 STEP 4                   │  │
│  │     └─ finish=stop → 直接回复用户 → 输出 {"response": "..."}           │  │
│  │                                                                       │  │
│  │  ⑤ 若 run_skills:                                                      │  │
│  │     写 dispatch manifest 到 DataBus (put_global, 无 dialogue 前缀)     │  │
│  │     输出 next_skills=[{name:"skill-runner", exec_skill_key:"dispatch:1"}]│ │
│  └───────────────────────────────────────────────────────────────────────┘  │
└────────────────────────┬────────────────────────────────────────────────────┘
                         │ 输出 next_skills
                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  STEP 4: skill-runner (id=3) — skill_workflow.json                         │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  skill_executor.py                                                    │  │
│  │  ──────────────────                                                   │  │
│  │                                                                       │  │
│  │  ① 读 AF_EXEC_SKILL_KEY = "dispatch:1"                                │  │
│  │     → databus.get_global("dispatch:1") 获取 dispatch manifest          │  │
│  │     → 得到 {skill_name:"weather_query", prompt:"...", load_skills:[]}  │  │
│  │                                                                       │  │
│  │  ② 加载工具 (skill-scoped + global):                                   │  │
│  │     全局: bash, echo, read_file, write_file, web_fetch                 │  │
│  │     skill 自有: weather_query.py (scripts/ 下的 TOOL_NAME 模块)        │  │
│  │                                                                       │  │
│  │  ③ 读 SKILL.md → 构建 system prompt                                    │  │
│  │                                                                       │  │
│  │  ④ LLM while-loop (最多 8 轮):                                         │  │
│  │     ├─ finish=tool_calls → 执行工具（如 weather_query tool）            │  │
│  │     └─ finish=stop → 整理结果 → 输出                                   │  │
│  │                                                                       │  │
│  │  ⑤ 写 skill_result 到 DataBus (skill_result:skill-runner)              │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
│  框架自动重触发 coordinator (completed_skill_name=skill-runner)              │
└────────────────────────┬────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  STEP 5: coordinator 再次执行 (skill 完成通知)                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  coordinator_decide.py (第二次被触发)                                  │  │
│  │                                                                       │  │
│  │  ① 收到 AF_COMPLETED_SKILL_NAME='skill-runner'                        │  │
│  │     AF_SKILL_SUCCESS='true'                                            │  │
│  │                                                                       │  │
│  │  ② collect_skill_results → 读 DataBus skill_result                    │  │
│  │     → 得到天气数据（29°C、多云、湿度 58%）                              │  │
│  │                                                                       │  │
│  │  ③ 读对话历史 (trace)                                                  │  │
│  │     recorder.read_session_messages() → llm/session_messages           │  │
│  │                                                                       │  │
│  │  ④ 追加用户消息：                                                       │  │
│  │     "Skill 'skill-runner'已完成...结果:{天气数据}...请评估结果..."       │  │
│  │                                                                       │  │
│  │  ⑤ LLM 再次调用 → trace 记录 (#3)                                     │  │
│  │     ├─ finish=tool_calls → 再 dispatch（结果不满意）→ 回 STEP 4         │  │
│  │     └─ finish=stop → 最终回复                                          │  │
│  │                                                                       │  │
│  │  ⑥ 写对话历史 (trace)                                                  │  │
│  │     recorder.write_session_messages() → llm/session_messages          │  │
│  │                                                                       │  │
│  │  ⑦ 输出 {"response": "最终给用户的穿搭建议"} → 回客户端                  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
└────────────────────────┬────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  客 户 端 收 到 响 应                                                       │
│                                                                              │
│  ┌─ 已知问题 ────────────────────────────────────────────────────────────┐  │
│  │  ① agent.poll_response JSON-RPC 不可靠（RESPONSE_POLLER 10ms 内删除   │  │
│  │     DataBus response key），需通过 bridge WebSocket 或 REST 获取        │  │
│  │                                                                       │  │
│  │  ② LLM 的推理过程（<think>、评估结果、判定）和最终回复都写在 content    │  │
│  │     同一个字段，coordinator_decide 没有过滤就发出去了（待修复）          │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 关键数据流

```
┌─────────────┐    put_global("dispatch:1")    ┌──────────────┐
│ coordinator │ ───────────────────────────────▶│    DataBus    │
│  _decide    │                                  │  (global)     │
│             │◀───────────────────────────────│              │
│             │    get_global("dispatch:1")      │              │
└─────────────┘                                  └──────┬───────┘
       │                                                │
       │  sys().put({dialogue}/next_skills,...)          │
       │                                                │
       │  框架 launch skill-runner                        │
       │  (AF_EXEC_SKILL_KEY="dispatch:1")                │
       │                                                │
       ▼                                                ▼
┌─────────────┐                                  ┌──────────────┐
│  skill      │  put("skill_result:skill-runner") │    DataBus    │
│  _executor  │ ────────────────────────────────▶│  (sys scoped) │
│             │                                   │              │
│  读 dispatch manifest → weather_query tool      │              │
│  调 QWeather API → 29°C / 多云                  │              │
└─────────────┘                                   └──────────────┘
```

### Trace 数据流（写入 / 读取 / 消费）

Trace 使用独立的 `llm` 命名空间（`/session/{sid}/llm/...`），与业务数据的 `sys` 命名空间隔离。

```
                        写 入（Write）
┌─────────────────────────────────────────────────────────────────────────┐
│  llm_call.py: _TraceRecorder.trace_call()                               │
│                                                                         │
│  每次 LLM 调用（coordinator_decide 或 skill_executor）都会记录：          │
│                                                                         │
│  ① HTTP 调用前写入 request                                               │
│     llm().put("traces/{call_id}/request", request_body)                 │
│                                                                         │
│  ② HTTP 返回后写入 response + meta                                      │
│     llm().put("traces/{call_id}/response", response_data)               │
│     llm().put("traces/{call_id}/meta", {                                │
│       model, latency_ms, finish_reason, tokens, ...                     │
│     })                                                                  │
│                                                                         │
│  ③ 维护索引                                                             │
│     llm().put("traces/index", [call_id, ...])                           │
│                                                                         │
│  ④ 写对话历史（coordinator_decide 在每轮决策后调用）                     │
│     recorder.write_session_messages(messages)                           │
│     → llm/session_messages                                              │
└─────────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                      读 取（Read）
┌─────────────────────────────────────────────────────────────────────────┐
│  ① coordinator_decide.py（节点内读取对话历史）                            │
│     recorder = get_recorder()                                           │
│     history = recorder.read_session_messages()                          │
│     → llm/session_messages → 恢复跨触发对话上下文                        │
│                                                                         │
│  ② bridge REST API（外部读取 trace）                                     │
│     GET /api/agents/{agent_id}/llm-traces?user_id=&client_id=           │
│       → daemon JSON-RPC databus.list_keys(prefix="llm/traces/")         │
│       → 返回 traces/index 中的 call_id 列表                             │
│       → 逐个 databus.get("llm/traces/{call_id}/meta")                   │
│                                                                         │
│     GET /api/agents/{agent_id}/llm-traces/{call_id}                     │
│       → databus.get("llm/traces/{call_id}/request")                    │
│       → databus.get("llm/traces/{call_id}/response")                   │
│       → databus.get("llm/traces/{call_id}/meta")                       │
│       → 返回三条记录（完整请求/响应/元数据）                              │
└─────────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                      消 费（Consume）
┌─────────────────────────────────────────────────────────────────────────┐
│  消费者             用途                     读取方式                    │
│ ─────────────────────────────────────────────────────────────────────  │
│  web UI            展示 LLM 调用链、延迟、      bridge REST API           │
│                    tokens、finish_reason                                 │
│                                                                         │
│  web UI            查看完整请求/响应体        bridge REST API             │
│                    （调试/审计）              ({call_id}/detail)          │
│                                                                         │
│  coordinator_decide 恢复跨触发对话历史         llm/session_messages       │
│                                                                         │
│  E2E 测试           验证 skill 是否执行        bridge REST API           │
│                     （通过 trace 判断）        (test_concurrency_guard)   │
└─────────────────────────────────────────────────────────────────────────┘
```

### Trace 在各执行步骤中的位置

```
用户请求
  ↓
STEP 1-2: dispatcher → dummy_pre_skill     [无 LLM 调用，无 trace]
  ↓
STEP 3: coordinator_decide 首次 LLM 决策    [Trace #1]
  ↓  ↓ 若 run_skills
STEP 4: skill_executor LLM while-loop       [Trace #2 ~ #N，每轮 LLM 调用写一次]
  ↓  skill 完成
STEP 5: coordinator_decide 评估             [Trace #N+1]
  ↓  ↓ 写对话历史                           [Trace: session_messages]
最终响应 → 客户端
```

```
DataBus 存储结构（llm 命名空间）:

/session/{sid}/llm/
├── traces/
│   ├── index                              → ["a1b2c3", "d4e5f6", ...]
│   ├── a1b2c3/
│   │   ├── request                        → {messages: [...], tools: [...], model: "..."}
│   │   ├── response                       → {choices: [...], usage: {...}}
│   │   └── meta                           → {latency_ms: 3821, finish_reason: "stop", ...}
│   ├── d4e5f6/
│   │   ├── request                        → {...}
│   │   ├── response                       → {...}
│   │   └── meta                           → {...}
│   └── ...
└── session_messages                       → [{role: "user", content: "..."}, ...]
```

## 架构总览 (V3)

```
用户请求
  ↓
Dispatcher (next_skills=["dummy_pre_skill"])
  ↓
dummy_pre_skill [type=skill, hook 点] ── dummy_pre_skill.py
  │   纯 passthrough，不处理不输出 next_skills
  │   完成后 framework 自动 forward 给 coordinator task
  ↓ (framework forward，从 {dialogue_id}/user_prompt 恢复原始输入)
coordinator [type=coordinator] ── coordinator_decide.py
  │   单轮 LLM 决策：调 run_skills 工具 or 输出最终回复
  │
  ├─ 若 LLM 调 run_skills → 写 dispatch manifest 到 DataBus
  │     ↓ (coordinator 输出 next_skills，framework launch)
  │   skill-runner [type=skill] ── skill_executor.py
  │     读 exec_skill_key → DataBus dispatch manifest → 真实 skill_name + prompt
  │     从 harness/skills/{skill_name}/scripts/ 加载 skill 自带 tool
  │     LLM while-loop 执行任务
  │     ↓
  │   skill 完成 → framework 自动重触发 coordinator (从 user_prompt 恢复输入)
  │
  └─ coordinator 评估结果 → 再 dispatch 或输出最终响应 → 客户端
```

### 四个 task 的职责

| task | type | 节点 | 职责 |
|------|------|------|------|
| dispatcher (id=0) | dispatcher | 无（仅路由） | next_skills 指向 dummy_pre_skill，生成 dialogue_id，写 user_prompt |
| dummy_pre_skill (id=2) | skill | dummy_pre_skill.py | 前置 hook 点，纯 passthrough，完成后 framework forward 给 coordinator |
| coordinator (id=1) | coordinator | coordinator_decide.py | LLM 决策核心：首次规划、评估 skill 结果、决定再 dispatch 或回复 |
| skill-runner (id=3) | skill | skill_executor.py | 通用 skill 执行器，通过 exec_skill_key 路由到任意 skill |

## 各层职责

```
┌─────────────────────────────────────────────────────────────────────────┐
│  agent.json  定义 task 拓扑、workflow 关联、pool 大小                    │
├─────────────────────────────────────────────────────────────────────────┤
│  agent.md    提供 model 配置 + 业务描述（给 LLM 的 system prompt）        │
├─────────────────────────────────────────────────────────────────────────┤
│  workflows/  定义节点执行顺序、input/output 映射                         │
├─────────────────────────────────────────────────────────────────────────┤
│  nodes/      Python 节点脚本（coordinator_decide / skill_executor / …） │
├─────────────────────────────────────────────────────────────────────────┤
│  harness/    技能文档（SKILL.md）+ 工具脚本（scripts/）+ 全局工具（tools/）│
├─────────────────────────────────────────────────────────────────────────┤
│  .env        环境变量（API keys、模型配置、业务密钥）                     │
└─────────────────────────────────────────────────────────────────────────┘
```

## 设计决策

### 1. 为什么用 dummy_pre_skill hook 而非 dispatcher 直接路由到 coordinator？

**结论：为了提供可扩展的 hook 点，同时利用 framework 的原始输入恢复机制。**

AgentFlow 框架的 dispatcher→coordinator 路径有两条：

| 路径 | 机制 | 原始输入保留 |
|------|------|------|
| A: dispatcher 有 next_skills → launch skill task | `launch_skill_task()` 启动 skill | ✅ 保留 message.content，写 user_prompt 到 DataBus |
| B: skill 完成 → forward to coordinator | `process_skill_response` forward | ✅ 从 `{dialogue_id}/user_prompt` 恢复（agent.cpp:1796） |

**dummy_pre_skill 的作用**：
1. 作为 dispatcher 的 next_skills 目标（走路径 A），framework 生成 dialogue_id 并写 `user_prompt` 到 DataBus
2. 完成后走路径 B（forward to coordinator），coordinator 从 `user_prompt` 恢复原始输入
3. 作为 hook 点，未来可扩展输入校验、鉴权、富化、遥测等前置处理

### 2. 为什么只有一个 skill-runner task（而非每个 skill 一个 task）？

**结论：exec_skill_key 路由 + DataBus dispatch manifest 实现"一个 task 跑任意 skill"。**

coordinator_decide 调 run_skills 时：
1. 把每个 skill 的真实 `skill_name` + `prompt` + `load_skills` 写入 DataBus `dispatch:{N}` manifest（`put_global`，无 dialogue 前缀）
2. 输出 `next_skills: [{name:"skill-runner", exec_skill_key:"dispatch:N", force:true}]`

框架通过 `AF_EXEC_SKILL_KEY` 环境变量把每个 skill-runner 实例路由到不同的 dispatch manifest。`force=true` 绕过框架的同名 skill 去重，允许并行多个 skill-runner 实例。

**新增 skill = 只加 SKILL.md + agent.md 注册一行，不改 agent.json。**

### 3. 为什么 tool 放在 skill 的 scripts/ 下（而非全局 harness/tools/）？

**结论：单一职责 + skill 自包含 + 全局工具兜底。**

当前架构支持两层工具加载：

```
harness/tools/                    ← 全局工具，所有 skill 可见
  bash.py, read_file.py, write_file.py, web_fetch.py, ...

harness/skills/{skill_name}/
├── SKILL.md                      ← skill 文档（LLM 读这个理解 skill 能力）
└── scripts/
    └── weather_query.py          ← skill 专属 tool（skill_executor 从这里加载）
```

`_LocalToolRegistry(skill_name)` 先加载全局工具，再加载当前 skill 的 `scripts/` 下的工具。同名时 skill-scoped 覆盖全局。

### 4. trace 可观测性

每次 LLM 调用通过 `llm_call.py` 的 `_TraceRecorder` 写入 DataBus `llm` 命名空间：

| DataBus key | 内容 |
|-------------|------|
| `llm/traces/{call_id}/request` | 完整 HTTP 请求体（messages + tools） |
| `llm/traces/{call_id}/response` | 完整 HTTP 响应体 |
| `llm/traces/{call_id}/meta` | latency、tokens、finish_reason、摘要、**任务上下文** |
| `llm/traces/index` | call_id 有序列表，供枚举 |
| `llm/session_messages` | 跨触发的对话历史（session 级持久化） |

HTTP 传输用 Python `requests` 库（`verify=False` 跳过 SSL 证书验证，等效于旧版 curl `-k` 行为）。

#### meta 中的任务上下文字段

`_TraceRecorder.trace_call` 在写 meta 时从框架注入的 `AF_*` 环境变量采集调用方身份，使每条 LLM trace 记录都能被归属到 coordinator / skill 的控制流中，无需框架侧改动：

| meta 字段 | 来源环境变量 | 含义 |
|-----------|-------------|------|
| `dialogue_id` | `AF_DIALOGUE_ID` | 当前对话 ID，用于把一次对话的所有 LLM 调用聚成一组 |
| `session_id` | `AF_SESSION_ID` | 当前 agent session ID |
| `task_id` | `AF_SELF_TASK_ID` | 当前 task 的 ID（coordinator task / skill task 各自不同） |
| `skill_name` | `AF_SKILL_NAME` | 当前执行的 skill 名（仅 skill task 非空） |
| `completed_skill` | `AF_COMPLETED_SKILL_NAME` | 刚完成的 skill 名（仅 coordinator 回流触发时非空，是控制权回到 coordinator 的显式信号） |
| `task_type` | 推断 | `"skill"` / `"coordinator"`（无 `AF_TASK_TYPE` 环境变量，按 skill_name / completed_skill 推断） |

下游分析还原控制流的判据：
- `skill_name` 非空 → subagent 正在执行
- `completed_skill` 非空 → coordinator 重新拿到控制权（skill 完成回流）
- 同一 `dialogue_id` 下按时序排列 → 完整的 coordinator → skill → coordinator 调用链


### 5. 跨触发对话历史

coordinator_decide 每次触发只做一轮 LLM 决策，然后 yield control 等待 skill 完成。对话历史通过 `llm/session_messages` 在 DataBus 跨触发持久化：

- 首次触发（dummy_pre_skill forward）：读空历史 + 用户输入 → LLM 决策 → 写回历史
- skill 完成触发：读历史 + skill 结果通知 → LLM 评估 → 写回历史或输出最终回复

## 目录结构

```
outfit-assistant/
├── agent.json                          # 4 task 定义
├── agent.md                            # 业务意图 + model 配置 + skills 列表
├── package.json                        # workflow 注册
├── .env / .env.example                 # API key 配置
├── requirements.txt                    # Python 依赖
├── setup.sh                            # 环境初始化脚本（手动跑一次，对齐 design §5.4）
├── dummy_pre_skill_workflow.json       # dummy_pre_skill hook workflow
├── coordinator_workflow.json           # coordinator task workflow (coordinator_decide)
├── skill_workflow.json                 # skill-runner task workflow
├── nodes/
│   ├── dummy_pre_skill.py              # 前置 hook 占位节点
│   ├── coordinator_decide.py           # LLM 决策核心（run_skills 工具）
│   ├── skill_executor.py               # 通用 skill 执行器 + 双层 tool 加载
│   ├── databus_client.py               # DataBus 封装（dialogue_id 前缀对齐 + global 方法）
│   └── llm_call.py                     # LLM 调用 (requests) + trace 记录
├── harness/
│   ├── skills/
│   │   ├── weather_query/
│   │   │   ├── SKILL.md
│   │   │   └── scripts/weather_query.py    # QWeather API tool
│   │   ├── outfit-advisor/SKILL.md
│   │   ├── occasion-guide/SKILL.md
│   │   ├── outfit-stylist/SKILL.md         # V1 遗留（含 terminal_command 指令）
│   │   ├── forge-image-gen/                 # 图片生成（DashScope / MiniMax）
│   │   ├── video-gen/                       # 视频生成（DashScope / MiniMax）
│   │   ├── dummy_pre_skill/SKILL.md         # hook 占位
│   │   └── skill-runner/SKILL.md            # 占位
│   └── tools/
│       ├── bash.py                           # bash 执行
│       ├── echo.py                           # echo 测试
│       ├── read_file.py                      # 读文件
│       ├── write_file.py                     # 写文件
│       ├── web_fetch.py                      # 网页抓取
│       ├── list_skills.py                    # 列出技能
│       ├── load_skill.py                     # 加载技能文档
│       └── mcp_bridge.py                     # MCP 桥接
└── data/
    └── occasion_guide.json
```

## 当前已知问题

| 问题 | 影响 | 状态 |
|------|------|------|
| LLM 推理泄漏到最终回复 | 用户看到"评估结果""原因分析"等内部推理 | ❌ 待修复 |
| `agent.poll_response` 不可靠 | test_client 超时（RESPONSE_POLLER 10ms 内删除 DataBus key） | ⚠️ 框架层问题 |
| daemon 约 5 请求后退化 | `mutex lock failed: Invalid argument` 崩溃 | ⚠️ 框架层问题 |
| `outfit-stylist` SKILL.md 仍描述 V1 能力 | LLM 尝试用 `terminal_command`（已不存在的工具） | ✅ 已补 `harness/tools/` |

## 快速开始

### 1. 环境初始化

```bash
cd examples/agent/outfit-assistant
bash setup.sh                    # 创建 .venv + 装依赖
cp .env.example .env             # 填入真实 API key
$EDITOR .env                     # 填 QWEATHER_API_KEY / QWEATHER_API_HOST / API_KEY
```

### 2. 启动 daemon

在项目根目录执行：

```bash
cd /path/to/aisf_agentflow

# 加载 .env 环境变量
set -a; . examples/agent/outfit-assistant/.env; set +a

# 启动 daemon（PYTHON_PATH 必须用绝对路径，指向 .venv）
PYTHON_PATH="$(pwd)/examples/agent/outfit-assistant/.venv" \
  ./build/agentflowd \
  --listen 0.0.0.0:17891 \
  --package "$(pwd)/examples/agent/outfit-assistant"
```

> **关键**：`PYTHON_PATH` 必须是绝对路径。框架 worker 用这个路径找 `.venv/bin/python3` 执行节点脚本；相对路径会因 working_dir 切换而失效。

### 3. 测试

```bash
python3 examples/agent/harness_agent_v2/test_client.py \
  --port 17891 \
  --agent-id outfit-assistant \
  --message "今天北京天气如何"
```

预期输出包含真实天气数据（来自 QWeather API，非 LLM 编造）。

## 加新 skill

只需两步，不改任何代码：

```bash
# 1. 创建 skill 目录 + SKILL.md（+ 可选 scripts/ 带 tool）
mkdir -p harness/skills/my-skill/scripts
cat > harness/skills/my-skill/SKILL.md << 'EOF'
---
name: my-skill
description: 这个 skill 做什么
---

# My Skill

## 行为
- 具体能力描述

## 自带工具（可选）
- `my_tool`（scripts/my_tool.py）：tool 说明
EOF

# 2. 在 agent.md 的 skills 列表加一行
#    skills:
#      - my-skill
```

coordinator_decide 读 agent.md 的 skills 列表写进 system prompt，LLM 自主决定何时 dispatch 这个 skill。skill_executor 会从 `harness/skills/my-skill/scripts/` 自动加载 tool。

## 扩展 dummy_pre_skill hook

dummy_pre_skill 当前是纯 passthrough。未来可在此扩展前置处理：

```python
# nodes/dummy_pre_skill.py 示例扩展
def execute():
    user_input = os.environ.get("AF_INPUT", "")

    # 示例：输入校验
    if not user_input or len(user_input) > 10000:
        print(json.dumps({"error": "invalid input"}, ensure_ascii=False))
        return

    # 示例：敏感词过滤 / 鉴权 / 富化
    # processed = enrich(user_input)

    # 完成后 framework 自动 forward 给 coordinator（从 user_prompt 恢复原始输入）
    print(json.dumps({}, ensure_ascii=False))
```

**hook 契约**：
- 禁止输出 next_skills（否则走 launch 路径，coordinator 收不到原始输入）
- 禁止写对话历史（coordinator 拥有历史所有权）
- 可读 AF_INPUT 做前置处理，但 framework 传给 coordinator 的仍是原始 user_prompt

## 环境变量

| 变量 | 用途 | 必填 |
|------|------|------|
| `API_KEY` | LLM API key | 是 |
| `QWEATHER_API_KEY` | 和风天气 API key | weather_query skill 需要 |
| `QWEATHER_API_HOST` | 和风天气 API host | weather_query skill 需要 |
| `AF_LLM_BASE_URL` | LLM base URL（覆盖 agent.md） | 否 |
| `AF_LLM_MODEL` | LLM 模型名（覆盖 agent.md） | 否 |
| `AF_MAX_SKILL_TASK_ITERATIONS` | skill LLM while-loop 最大轮数 | 否 |
| `PYTHON_PATH` | .venv 目录（框架 worker 用） | 启动 daemon 时必填 |

## 依赖

- `pyyaml>=6.0` — 解析 agent.md / SKILL.md 的 YAML frontmatter
- `python-dotenv>=1.0` — .env 加载
- `agentflow_databus` — 来自项目 `src/runtime/databus/clients/python`，由节点通过 `sys.path` 注入，无需 pip 安装
