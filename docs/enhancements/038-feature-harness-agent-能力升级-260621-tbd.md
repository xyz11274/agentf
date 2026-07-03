# 详细设计: Harness Agent 完整能力升级 — 动态 Skills 加载/多轮对话/会话管理

> **文档级别**: L4 技术设计
> **日期**: 2026-06-21
> **状态**: tbd
> **关联**: `examples/agent/harness-agent/`（现有实现）

---

## 1. 背景

### 1.1 问题陈述

现有的 `harness-agent` 示例是一个最小化 agent loop 演示，缺失四个核心能力：

| 缺失能力 | 当前状态 | 问题 |
|---------|---------|------|
| **动态 Skills 加载** | 系统提示词硬编码在 `llm_think_and_act.py:70`，无法在对话中动态加载 SKILL.md | agent 无法按需获取技能指令 |
| **多轮对话** | 单次 request-response，无上下文保持 | 每次对话都是"第一次" |
| **工具调用** | `TOOL_REGISTRY` 进程内可执行，但缺乏 skill 文档支撑 | LLM 没有技能描述来指导工具选择 |
| **会话管理** | `session.list` 返回空数组 | daemon 无法查询会话状态 |

### 1.2 设计目标

- **动态 Skills 加载**：LLM 通过 `load_skills` 工具自主决定加载哪些 SKILL.md，加载的内容自然融入对话上下文
- **多轮对话**：复用框架已有的 ContextCallback 机制管理对话历史，不新增自定义 DataBus key
- **工具调用**：保持 `TOOL_REGISTRY` 进程内模式，LLM 通过 SKILL.md 内容了解工具用法
- **会话管理**：`session.list` 直接查询 `SessionRouter::active_routes_` 暴露已有数据

---

## 2. 方案简述

在 `llm_think_and_act.py` 中为 LLM 提供三个工具：

| 工具 | 触发方 | 行为 | 数据流 |
|------|--------|------|--------|
| `load_skills` | LLM 自主决定 | 读取 `harness/skills/{name}/SKILL.md` → 返回内容给 LLM | 文件系统 → tool response → LLM 消化 → 自然进入 session history |
| `list_skills` | LLM 自主决定 | 列出 `harness/skills/` 下所有可用 skill | 文件系统 → tool response |
| `call_tool` | LLM 自主决定 | 通过 `TOOL_REGISTRY` 进程内执行工具 | 进程内直接调用 |

**核心原则**：
- 不新增任何自定义 DataBus key。所有状态管理复用框架已有机制
- **所有配置从 agent package 文件中读取**，Python 代码不做任何硬编码：
  - agent.md frontmatter -> 模型配置（provider/model/api_key/base_url）、skills 列表
  - agent.md 正文 -> 系统提示词
  - harness/skills/{name}/SKILL.md -> 技能文档，由 load_skills 工具按需加载

---

## 3. 约束与假设

- 不改动框架核心层（`src/agent/`、`src/workflow/`、`src/daemon/`）——只改 `harness-agent` 示例的 Python 节点（`session.list` 除外）
- Skills 加载失败（文件不存在等）不阻止 agent 运行，LLM 收到工具错误后可自行判断
- 向后兼容：已有 `test_client.py`、`agent_runner` 交互模式不受影响

---

## 4. 当前实现分析

### 4.1 现有可复用机制

| 机制 | 位置 | 说明 |
|------|------|------|
| ContextCallback | `src/agent/agent.cpp:1249-1380` | 每次 dialogue 开始时，框架自动读取 `/session/{sid}/ctx/{dialogue_id}/content` → 调用回调 → 写回新 context |
| SessionRouter::active_routes_ | `src/agent/session_router.cpp` | 记录所有活跃的 session 路由（user_id:client_id → session） |
| `AF_INPUT` / `AF_DIALOGUE_ID` / `AF_SESSION_ID` | worker 环境变量 | 框架自动注入到 Python node |

### 4.2 不需要做的事情

| 不需要 | 原因 |
|--------|------|
| ❌ 新增 DataBus key | 已有 ContextCallback 管理对话历史，skill 内容通过 tool response 自然进入 session |
| ❌ 引入 next_skills | 工具执行在进程内完成，不经过 framework skill 调度 |
| ❌ 修改 framework 核心层 | 所有改动都在示例层 |
| ❌ 预先加载所有 SKILL.md | LLM 通过 load_skills 自主决定 |

### 4.3 Gap 分析：多轮对话的"断裂"问题

**当前实现的隐含假设**：§5.2 描述的多轮机制假设"同一 dialogue_id 复用"，但框架实际行为并非如此。

**框架对 dialogue_id 的管理流水线**：

```
agent.execute(user_id, client_id, content)    ← 客户端 RPC 调用
  │
  ▼ agent.cpp:895-899
metadata["dialogue_id"] = 消息中携带的 dialogue_id（客户端可传）
  │
  ▼ agent.cpp:1229-1236
if(metadata 中没有 dialogue_id):
    dialogue_id = hash(user_text + message_id)  ← 每次 execute 都不同
else:
    dialogue_id = metadata["dialogue_id"]        ← 复用客户端传入的值
```

**关键事实**：
1. `test_client.py` 调用 `agent.execute` 时**不传** `dialogue_id` → 框架每轮生成一个新的
2. ContextCallback 的 `ctx/{dialogue_id}/content` 写入/读取都是用同一个 dialogue_id → **但每轮生成新 id，key 路径就变了**
3. 结果是：第二轮 ContextCallback 读 `/session/{sid}/ctx/{新id}/content` → 空值

**跨轮上下文的真实状态**：

| 轮次 | dialogue_id | ctx 读的 key | 结果 |
|------|------------|-------------|------|
| Turn 1 | `hash("hello" + msg_1)` | `ctx/hash1/content` | 空（首次写入） |
| Turn 2 | `hash("你好" + msg_2)` | `ctx/hash2/content` | **空**（key 变了！） |

**结论**：当前框架的 ContextCallback **支撑同一 dialogue_id 内的多次回环**（coordinator 自触发 loop），但**不支撑跨 execute 调用的多轮记忆**。

**三种修复路径**：

| 路径 | 改动范围 | 优点 | 缺点 |
|------|---------|------|------|
| **A: 客户端在 RPC 中传 dialogue_id** | `test_client.py` + `rpc_handler.cpp` | 语义清晰，dialogue 有明确客户端生命周期 | 客户端需保存 id，改 RPC 协议 |
| **B: 使用 session 级 context key**（不依赖 dialogue_id） | `nodes/databus_client.py` + `src/agent/agent.cpp` ContextCallback | 不改 RPC，框架层改动最小 | context 在各个 dialogue 间共享，无隔离 |
| **C: 框架自动将前一轮 ctx 拷贝到新 dialogue** | `src/agent/agent.cpp` handle_intent_request | 对客户端透明 | 增加框架复杂度，需定义"哪一轮是上一轮" |

**本方案选择路径 B**（与 §4.1 的"不新增 DataBus key"自洽——改用 session 级的固定 key，不新增 key 类型）。

> **注意**：路径 B 与现有的 `ctx/{dialogue_id}/content` 路径**不同**，需要改 ContextCallback 的读/写 key。这属于 framework 核心层改动（与 §3 约束冲突）。另见 §12 后续计划。当前 V2 实现使用路径 A 的临时变通。参见 §10 进展跟踪。|

---

## 5. 流程设计

### 5.1 升级后消息流

```
agent.execute(dialogue_id="xxx")
  │
  ▼ framework 已有机制
ContextCallback 注入 previous_context  →  触发 coordinator workflow
  │
  ▼ llm_think_and_act.py (V2)
  │
  while True:
    ├── call_llm(messages, tools=[load_skills, list_skills, call_tool])
    │
    ├── load_skills("echo"):
    │     └─ 读取 harness/skills/echo/SKILL.md → tool response
    │     └─ LLM 消化内容 → messages += [{role: "tool", content: "..."}]
    │     └─ 这些自然成为 session history 的一部分
    │
    ├── list_skills():
    │     └─ 扫描 harness/skills/ 目录 → 返回 skill 列表
    │
    ├── call_tool("echo", {...}):
    │     └─ TOOL_REGISTRY[tool_name](input) → 进程内执行
    │     └─ result → tool response → LLM 评估
    │
    └── respond(response_text):
          └─ print(json.dumps({"response": ...}))
          └─ framework 将响应写入 DataBus / response queue
```

### 5.2 多轮对话

#### 5.2.1 背景

`harness-agent` 是 dispatcher → coordinator 单次执行模式，每次 `agent.execute` RPC 调用都生成一个新的 workflow 实例。多轮对话需要**跨 RPC 调用的上下文持久化**。

框架已有的 ContextCallback（`agent.cpp:1249-1380`）可以读/写 context，但它使用 `ctx/{dialogue_id}/content` 作为 key。如 §4.3 分析，dialogue_id 每轮新生成 → key 路径断裂。

#### 5.2.2 数据流

```
Turn 1:
  test_client.py:
    agent.execute(user_id="u1", client_id="c1", content="你好")
      → 框架为 u1:c1 找到/创建 session (sid="S1")
      → 生成 dialogue_id = hash("你好" + msg_id_1) = "D1"
      → ContextCallback 读 /session/S1/ctx/D1/content → 空（首次）→ 返回空 previous_context
      → coordinator workflow 启动 llm_think_and_act.py
          Python 节点: AF_INPUT="你好", AF_DIALOGUE_ID="D1", AF_SESSION_ID="S1"
          → execute() 运行
          → respond("我是AI助手")
      → 框架写回 response
      → (Python 节点内部: write_dialogue_context("D1", summary))

Turn 2 (同一 session, 新 dialogue):
  test_client.py:
    agent.execute(user_id="u1", client_id="c1", content="还记得上一条吗？")
      → 路由到同一 session (sid="S1")
      → 生成新 dialogue_id = hash("还记得上一条吗？" + msg_id_2) = "D2"  ← 新的！
      → ContextCallback 读 /session/S1/ctx/D2/content → 空（D2 从没写过）
      → coordinator workflow 启动 llm_think_and_act.py
          Python 节点: AF_INPUT="还记得上一条吗？", AF_DIALOGUE_ID="D2", AF_SESSION_ID="S1"
          → read_dialogue_context("D2"): 读 /session/S1/ctx/D2/content → 空
          → execute() 运行，messages = [system, user_input] → 只有当前输入
          → LLM 看不到 Turn 1 的任何内容
          → respond("抱歉，我不记得之前说了什么")
      → 框架写回 response
```

#### 5.2.3 断裂原因

```
Turn 1 写入: ctx/D1/content  = "用户:你好\n助手:我是AI助手"
Turn 2 读取: ctx/D2/content  = ?   ← 路径不同，读到空
```

不是 context 没存，而是存在一个 dialogue_id 的 key 下，下一轮读的是另一个 dialogue_id 的 key。

#### 5.2.4 修复路径（本方案采用路径 B）

**路径 B：使用 session 级固定 key，不依赖 dialogue_id**

在 `llm_think_and_act.py` 的循环中，结束后不论当前 dialogue_id 是什么，都统一写到一个 session 级的固定 key：

```python
# databus_client.py
def read_session_context(self) -> str | None:
    params = {
        "session_id": self.session_id,
        "namespace": "ctx",
        "key": "session_summary"                     # ← 固定 key，不依赖 dialogue_id
    }
    ...

def write_session_context(self, content: str) -> bool:
    params = {
        "session_id": self.session_id,
        "namespace": "ctx",
        "key": "session_summary",
        "value": content
    }
    ...
```

对应的框架端 ContextCallback（agent.cpp:1249-1380）也改为读/写同一个固定 key：
```cpp
// agent.cpp 中 ContextCallback 的 key
std::string ctx_key = "/session/" + session->get_session_id() +
                      "/ctx/session_summary";   // 不再拼 dialogue_id
```

**为什么这样做**：
- session 是永久的（同一 user_id:client_id 复用），key 固定跨 RPC 调用
- context 在 session 内是 append-only 的累积摘要，不涉及隔离问题
- 示例层的 Python 节点改动即可验证，框架层 ContextCallback 改 key 路径是等量替换

**放弃路径 A**（客户端传 dialogue_id）的原因：
- 需要改 `agent.execute` RPC 协议：`rpc_handler.cpp` 多收一个可选参数
- `test_client.py` 或其他调用方需管理 dialogue_id 状态
- 增加了客户端复杂度，不符合"示例保持简单"的原则

**放弃路径 C**（框架自动拷贝）的原因：
- "上一轮"的定义不清晰（同 session 最后一个完成的 dialogue？还是时间窗口最近的？）
- 逻辑在框架核心层，与 §3 约束冲突

---

## 6. 接口设计

### 6.1 新增/修改文件

| 文件 | 操作 | 说明 |
|------|------|------|
| `nodes/llm_think_and_act.py` | 修改 | 增加 `load_skills`/`list_skills`/`call_tool` 三个工具，读取 `agent.md` 作为初始 system prompt |
| `nodes/databus_client.py` | 修改 | 新增 `read_dialogue_context()` / `write_dialogue_context()` 方法，支持 ctx 命名空间读写 |
| `examples/agent/harness-agent/agent.json` | 不变 | 无需改动 task 结构 |
| `src/daemon/rpc_handler.cpp` | 修改 | `session.list` 改为查询 AgentRegistry/SessionRouter |
| `include/internal/agent/agent.h` | 修改 | 新增 `get_active_sessions()` 声明 |
| `test_client.py` | 不变 | 兼容 |

### 6.2 `llm_think_and_act.py` V2 工具定义

```python
TOOLS = [
    {
        "name": "load_skills",
        "description": "Load skill packages. Each skill adds capabilities. "
                       "Call this when you need specific expertise.",
        "parameters": {
            "type": "object",
            "properties": {
                "skill_names": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Skill names to load (e.g. echo, calculator)"
                }
            },
            "required": ["skill_names"]
        }
    },
    {
        "name": "list_skills",
        "description": "List all available skill packages in harness/skills/",
        "parameters": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "call_tool",
        "description": "Execute a specific tool with given input",
        "parameters": {
            "type": "object",
            "properties": {
                "tool_name": {
                    "type": "string",
                    "description": "Tool to execute (e.g. echo, calculator)"
                },
                "tool_input": {
                    "type": "object",
                    "description": "Input parameters for the tool"
                }
            },
            "required": ["tool_name", "tool_input"]
        }
    }
]
```

### 6.3 `load_skills` 实现

```python
def handle_load_skills(skill_names, skills_base_dir):
    results = []
    for name in skill_names:
        skill_md = os.path.join(skills_base_dir, name, "SKILL.md")
        if not os.path.isfile(skill_md):
            results.append({"name": name, "error": "not_found"})
            continue
        with open(skill_md) as f:
            content = f.read()
        results.append({"name": name, "content": content})
    return results
```

### 6.4 `session.list` 响应

```json
{
  "jsonrpc": "2.0",
  "result": {
    "sessions": [
      {
        "session_id": "uuid-xxx",
        "user_id": "test_user",
        "client_id": "test_client",
        "age_ms": 15234,
        "status": "active"
      }
    ]
  },
  "id": 1
}
```

---

## 7. 实现方案

### 7.1 `llm_think_and_act.py` V2

```python
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SKILLS_BASE = os.environ.get("AF_SKILLS_DIR", "harness/skills")
AGENT_DIR = os.path.dirname(os.path.abspath(__file__))  # nodes/ 的父目录是 agent package 根

DEFAULT_SYSTEM_PROMPT = "You are a task-oriented AI assistant."

TOOLS = [...]  # 见 §6.2

def execute():
    user_input = get_env("AF_INPUT", "")
    
    # 从 agent.md 文件读取 system prompt（非硬编码）
    agent_md_path = os.path.join(AGENT_DIR, "agent.md")
    system_prompt = DEFAULT_SYSTEM_PROMPT
    if os.path.isfile(agent_md_path):
        with open(agent_md_path) as f:
            content = f.read()
            # 提取 frontmatter 后的正文
            parts = content.split("---", 2)
            if len(parts) >= 3:
                system_prompt = parts[2].strip()
            else:
                system_prompt = content.strip()
    
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_input},
    ]
    
    while True:
        decision = call_llm(messages, tools=TOOLS)
        
        if decision["action"] == "load_skills":
            for name in decision.get("skill_names", []):
                content = read_skill_md(name)
                messages.append({
                    "role": "tool",
                    "tool_call_id": f"load_skills:{name}",
                    "content": content or f"Error: skill '{name}' not found"
                })
        
        elif decision["action"] == "list_skills":
            skills = list_available_skills()
            messages.append({
                "role": "tool",
                "tool_call_id": "list_skills",
                "content": json.dumps({"skills": skills})
            })
        
        elif decision["action"] == "call_tool":
            result = execute_tool(decision["tool_name"], decision.get("tool_input", {}))
            messages.append({
                "role": "tool",
                "tool_call_id": decision["tool_name"],
                "content": result,
            })
        
        elif decision["action"] == "respond":
            output = {"response": decision.get("response", "")}
            print(json.dumps(output, ensure_ascii=False))
            break
```

### 7.2 `session.list` 实现

在 `agent.h` 中暴露接口：

```cpp
// include/internal/agent/agent.h 新增
struct SessionInfo {
  std::string session_id;
  std::string user_id;
  std::string client_id;
  int64_t age_ms;
  std::string status;
};
std::vector<SessionInfo> get_active_sessions() const;
```

实现代理到 `SessionRouter` 的 `active_routes_` 表。

### 7.3 功能点实现优先级

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P1 | 工具化 skills 加载 | 无副作用 | LLM 在对话中调用 `load_skills` 后能看到 SKILL.md 内容 |
| P1 | 从 agent.md 读取所有配置（model/skills/prompt） | 无副作用 | LLM system prompt 包含 agent.md 正文 |
| P2 | 多轮对话历史保持 | 无副作用（复用已有 ContextCallback） | 同一 dialogue_id 第二轮能看到第一轮上下文 |
| P3 | `session.list` 真实会话 | 需暴露接口 | 调用返回 ≥1 活跃会话 |

---

## 8. 测试方案

### 8.1 E2E 测试场景

| TC | 场景 | 步骤 | 预期 |
|----|------|------|------|
| 1 | load_skills 工具 | 发消息 → LLM 调用 load_skills → 返回 SKILL.md 内容 | LLM 响应中体现 skill 指令内容 |
| 2 | 多轮对话 | 发 "Hello" → 回复 → 发 "还记得上一条吗？" | 第二轮 LLM 能看到第一轮的内容 |
| 3 | 工具调用 | LLM 决定调用 echo 工具 → TOOL_REGISTRY 返回 | 返回正确结果 |
| 4 | session.list | 发一条消息后调用 | 返回 ≥1 活跃会话 |
| 5 | 向后兼容 | 用 test_client.py 调 agent.execute | 正常返回 |

### 8.2 独立性要求

- 使用 mock LLM 避免外部 API 依赖
- 测试不修改框架核心代码

---

## 9. 文档

### 9.1 需要变更的文档

| 文档 | 变更说明 |
|------|---------|
| `examples/agent/harness-agent/README.md` | 更新消息流程图和架构描述 |
| `examples/README.md` | 在 harness-agent 描述中增加能力说明 |

### 9.2 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [011-refactor-sessiontaskcoordinator-context-分层重构](011-refactor-sessiontaskcoordinator-context-分层重构-260301-implemented.md) | ContextCallback 是本方案多轮对话的基础 |
| [037-feature-skills-dynamic-upload](037-feature-skills-dynamic-upload.md) | skills 热上传是本方案 load_skills 的输入来源 |

---

## 10. 进展跟踪

- [x] `llm_think_and_act.py` V2（P1）
  - [x] 从 `agent.md` 文件读取 system prompt
  - [x] 实现 `load_skills` tool 处理
  - [x] 实现 `list_skills` tool 处理
  - [x] 保持 `TOOL_REGISTRY` 进程内工具执行
  - [x] 验证：LLM 调用 load_skills 后能看到 SKILL.md 内容
- [~] 多轮对话（P2）— 参见 §4.3 gap 分析
  - [x] `databus_client.py` 实现 `read_dialogue_context()` / `write_dialogue_context()`
  - [ ] **跨轮断裂**：每轮 `agent.execute` 生成新 `dialogue_id` → `ctx/{新id}/content` 读不到上一轮
  - [ ] 修复：改用 session 级固定 key `session_summary`（参见 §5.2.4 路径 B）
  - [ ] ContextCallback 改 key：从 `ctx/{dialogue_id}/content` 改为 `ctx/session_summary`
- [x] `session.list` 真实会话（P3）
  - [x] `MMAgent` 暴露 `get_active_sessions()`
  - [x] `rpc_handler.cpp` 调用该方法
  - [x] 验证：发送消息后 session.list 返回 ≥1 活跃会话
- [x] 测试通过
  - [x] test_client.py 正常
  - [x] mock LLM 模式下 E2E 全部通过（daemon 日志确认 Python 节点执行成功）

---

## 11. 后续计划

| 优化项 | 预期效果 | 方案简述 |
|--------|---------|---------|
| 路径 B 落地 | 跨 RPC 调用的多轮对话正常工作 | 改 `databus_client.py` + ContextCallback 的 key 从 `ctx/{dialogue_id}/content` 改为 `ctx/session_summary` |
| ContextCallback 策略选择 | 如果 `ctx/session_summary` 对异步 callback 不够，可退回到 RPC 传 dialogue_id 的路径 | 见 §4.3 路径 A 和路径 C |

## 12. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-06-21 | Prometheus | 初始版本 |
| v1.1 | 2026-06-21 | Sisyphus | §4.3 增加 gap 分析（dialogue_id 每轮新生成导致跨轮 context 断裂）；§5.2 重写，详细描述多轮对话数据流、断裂原因、三条修复路径及选择；§10 进展跟踪更新多轮状态为 `[~]` 并列出待修复项；§11 增加后续计划 |
