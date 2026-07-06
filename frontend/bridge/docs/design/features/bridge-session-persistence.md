# Bridge 会话持久化 Provider 层设计

> **层级**: L3 · `docs/design/features/bridge-session-persistence.md`
> **更新日期**: 2026-07-05

---

## 1. 背景与目标

### 1.1 问题

agentflow 的 DataBus 定位为**运行时工作内存**，dialogue 完成后数据即清理（`cleanup_dialogue_data`）。
agentflow 内部维护的 session 是执行槽位，pool 回收后即重置（`reset_for_pool`），不是持久化的用户会话。

前端需要：
- 刷新后仍能看到历史会话列表
- 能查看历史对话消息
- 不依赖 DataBus 做这些

### 1.2 核心设计决策

| 决策 | 内容 |
|------|------|
| **Provider 在 Bridge 层** | 持久化逻辑集中在 Bridge 内，不暴露给 Python node |
| **Python node 不逆向调 Bridge** | Node 只管自己的工作，不向 Bridge 发起业务调用 |
| **Bridge session ≠ agentflow session** | 两者是完全不同的概念，必须明确区分 |
| **Trace 推到 Bridge 落库** | LLM trace 通过 daemon → Bridge 管道，存到 Provider |
| **会话列表持久化** | Bridge 维护自己的会话表，外部消费者可重复获取 |

---

## 2. 对话轮次定义

### 2.1 一轮 = 一次用户输入 + 一次最终回答

```
一轮对话 =
  1 × messages.role="user"   (用户原始输入)
  1 × messages.role="assistant" (Agent 最终响应)

一轮内部 agentflow 可能执行多次 LLM 调用：
  coordinator LLM 调用    → traces.trace_type="llm_call"
  skill_executor LLM 调用  → traces.trace_type="llm_call"
  技能执行结果              → traces.trace_type="skill_exec"
  ...
这些都属于"执行细节"，不是对话内容，不入 KMM。

展示上：用户看到一轮问答的前端展示
存储上：messages 存对话，traces 存执行细节
同步到 KMM：只有 messages 中的 user + assistant 纯文本内容
```

### 2.2 KMM 的能力边界

根据 HealthAgent 中 KMM 的 API 调研，KMM 提供的端点均为固定 schema，不支持任意 JSON 存储：

| KMM 端点 | 用途 | 限制 |
|---------|------|------|
| `/kmm/v1/user/memory/history/add` | 存单条历史消息 | 仅 `{role, content, date}` 文本 |
| `/v1/user/context/add` | 存上下文 QA 对 | 仅 `{query, answer}` 文本成对 |
| `/kmm/v1/user/memories/get` | 召回长期记忆 | 固定格式 `{abstract, rewriteQ, ...}` |
| 其余 12 个端点 | 记忆/画像/场景管理 | 均为结构化 schema |

**KMM 不支持的数据**：`artifacts`(多模态附件)、`traces`(执行轨迹)、`sessions`(会话元数据) 均**只存本地**，不同步到 KMM。

### 2.3 双 Provider 模式（限幅使用）

| 场景 | LocalSessionProvider | KMMSessionProvider（可选） |
|------|:-------------------:|:------------------------:|
| 存 `messages`（对话文本） | ✅ 全量保存 | ✅ 同步纯文本（user + assistant） |
| 存 `messages.artifacts` | ✅ 全量保存 | ❌ 不存 |
| 存 `traces` | ✅ 全量保存 | ❌ 不存 |
| 存 `sessions` 元数据 | ✅ 全量保存 | ❌ 不存 |
| 读取历史上下文给 agentflow 注入 | ✅ `get_messages()` 取最近 N 条 | ⬜ 优先用 KMM，不可用时降级到 Local |

### 2.4 上下文注入逻辑

Bridge 在发给 agentflow 前，按以下优先级获取历史上下文：

```
if KMMSessionProvider 已配置 and KMM 可用:
    context = await KMMSessionProvider.get_context(user_id, session_id)
    # KMM 返回的 context 包含最近 N 轮 QA 对的纯文本
else:
    messages = await LocalSessionProvider.get_messages(session_id, limit=20)
    context = 将 messages 格式化为 {role, content} 列表
```

两者都使用同一接口 `SessionProvider.get_context()`（暂作为扩展方法），输出格式一致。

---

## 3. 初始上下文构建与记忆存取

### 3.1 上下文构建流程

Bridge 在收到用户新消息后、发给 agentflow 之前，构造 message list：

```
Step 1: Bridge 收到用户消息
    user_input = "我昨天感冒了"
    user_id = "alice", client_id = "web"

Step 2: 获取 session
    session = provider.get_or_create_session(user_id, client_id)
    # 返回 {id: "ses_a1b2", user_id: "alice", ...}

Step 3: 获取历史上下文（二选一）
    if KMM 配置且可用:
        ctx = KMMSessionProvider.get_context(user_id, session.id)
        # 返回 [{role:"user", content:"..."}, {role:"assistant", content:"..."}, ...]
        # KMM 只返回纯文本，不含 artifacts
    else:
        ctx = LocalSessionProvider.get_messages(session.id, limit=20)
        # 返回最近的 messages（含 artifacts 等全部字段）

Step 4: 构造 message list
    # 格式：OpenAI-compatible messages array
    messages = ctx + [{"role": "user", "content": user_input}]
    # = [
    #   {"role": "user",      "content": "上次的问题"},
    #   {"role": "assistant", "content": "上次的回答"},
    #   {"role": "user",      "content": "我昨天感冒了"},   ← 新输入
    # ]

Step 5: 发送给 agentflow daemon
    daemon_client.execute(
        agentflow_session_id=...,
        messages=messages,         ← 就是这个 messages 数组
        user_id="alice",
        client_id="web",
    )
```

**agentflow 内部接收**：daemon 将 `messages` 数组作为初始输入传给 Python node。Python node 自行在头部插入 system prompt 后调 LLM：

```python
# Python node 内部
system_prompt = "你是健康助手..."
full_messages = [{"role": "system", "content": system_prompt}] + messages
response = call_llm(full_messages)
```

### 3.2 记忆的存取

#### 3.2.1 记忆 recall（对话前）

记忆 recall 由 Bridge 在 Step 3 中连同历史上下文一起完成。如果 KMM 配置了记忆服务，Bridge 在取上下文的同时也召回相关记忆：

```python
async def _build_context(session, user_id, user_input):
    ctx_messages = []  # 历史对话消息
    memories = []      # 记忆条目
    
    if kmm_available:
        # KMM 的 get_context 接口同时返回上下文和长期记忆
        result = await kmm_provider.get_context(
            user_id=user_id, session_id=session["id"],
            query=user_input,  # 用当前输入做记忆召回
            limit_rounds=5,
            need_long_memory=True,
        )
        ctx_messages = result.messages   # 历史 QA
        memories = result.long_memory    # 相关记忆
    else:
        # LocalProvider 暂不提供记忆 recall
        ctx_messages = await local_provider.get_messages(session["id"], limit=20)
        memories = []
    
    return ctx_messages, memories
```

**记忆的注入方式**：记忆内容拼在最终的 message list 中，作为 system prompt 的一部分或单独的 system 角色消息：

```python
# Bridge 构造的最终 message list
if memories:
    memory_text = "用户相关信息：\n" + "\n".join(memories)
    messages = [
        {"role": "system", "content": memory_text},
        *ctx_messages,
        {"role": "user", "content": user_input},
    ]
else:
    messages = ctx_messages + [{"role": "user", "content": user_input}]
```

#### 3.2.2 记忆 save（对话后）

对话完成后，Bridge 在 `_on_chat_done` 中同步保存记忆：

```python
async def _on_chat_done(user_id, session_id, user_input, full_response, ...):
    # 1. 存本地（总是）
    local_provider.save_dialogue(...)
    local_provider.save_trace(...)
    
    # 2. 存 KMM 上下文（可选）
    #    KMM 的 context/add 只存 QA 对，不做记忆提炼
    if kmm_available:
        await kmm_provider.save_qa(user_id, session_id, DialogQA(
            user_input=user_input,
            agent_response=full_response,
        ))
```

**重要：KMM 不自动生成记忆。** 记忆的提炼、摘要需要业务侧显式调用 KMM 的记忆 API（`/kmm/v1/user/memories/get`、`/kmm/v1/user/memory/short/qa_mgr/operate` 等）。当前阶段 Bridge 不做主动记忆管理——记忆只通过对话历史（`get_messages`）隐式存在。

如果后续需要主动记忆能力，有两条路：

| 方案 | 做法 | 适合阶段 |
|------|------|---------|
| **Bridge 定时提炼** | Bridge 在 idle 时对近期对话做摘要，调 KMM 记忆 API 写入 | 二期 |
| **Node 内提炼** | Python node 在执行中使用 LLM 提炼关键信息，通过 Bridge trace 通道回调记忆 | 需打通 node→Bridge 通道（当前设计不允许） |

### 3.3 数据存储映射总结

| 数据 | 存 LocalProvider | 存 KMM | 负责方 | 时机 |
|------|:---------------:|:------:|--------|------|
| 历史消息（纯文本） | ✅ | ✅ | Bridge | `[DONE]` 后 |
| 多模态 artifacts | ✅ | ❌ | Bridge | `[DONE]` 后 |
| Trace（LLM 执行细节） | ✅ | ❌ | Bridge | `[DONE]` 后异步 |
| 长期记忆 | ✅（可选） | ✅ | KMM 自处理 | KMM 收到 QA 后自行提炼 |
| 短期记忆 | ❌ | ✅ | KMM 自处理 | KMM 收到 QA 后自行处理 |
| session 元数据 | ✅ | ❌ | Bridge | 创建/更新时 |

---

## 4. 两个 Session 概念（关键设计约束）

### 3.1 agentflow session（内部执行槽位）

| 属性 | 说明 |
|------|------|
| 生命周期 | 随 `agent.load` 创建，随 `agent.unload` 或 `session.destroy` 销毁 |
| ID 格式 | Pool 模式为数字 `"0"` `"1"`；动态创建为 UUID |
| 数据量 | DataBus 内 `/session/{sid}/` 下所有 data，dialogue 完成后清理 |
| 暴露方式 | 通过 daemon RPC `session.list` / `session.destroy` |
| 清理时机 | 显式调 `session.destroy`、agent unload、pool `reset_for_pool` |
| **使用方** | **agentflow 内部使用**，Bridge 只读 `session.list` 做同步 |

### 3.2 Bridge session（持久化用户会话）

| 属性 | 说明 |
|------|------|
| 生命周期 | 从第一条消息创建，到用户显式删除，跨重启持久化 |
| ID 格式 | `{user_id}:{client_id}` 或 UUID |
| 存储位置 | Bridge Provider（SQLite / 数据库） |
| 数据内容 | 会话元数据（title、创建时间、最后活动时间）+ 消息列表 |
| 暴露方式 | Bridge REST API `/api/sessions/*` |
| 清理时机 | 用户主动删除、或配置的保留策略 |
| **使用方** | **前端展示、外部系统消费** |

### 3.3 映射关系

```
Bridge session（持久）
  │ 通过 user_id + client_id 标识
  │
  ├── 对应 1 个 agentflow session（当前活跃的）
  │     agentflow session 销毁后，下一个请求可能会分配不同的 session ID
  │     但 Bridge session 不变（同一个 user_id + client_id）
  │
  └── 聚合多条 dialogue 记录
        dialogue 完成后 DataBus 清空，但 Bridge 里消息还在
```

**Bridge 不依赖 agentflow session ID 做持久化键。** 即使 agentflow 侧 session ID 变化（pool 回收后重新分配），Bridge 侧的同一条持久会话不受影响。

---

## 5. 整体架构

```
                    外部消费者
┌──────────────────────────────────────────────────────┐
│ 前端 Vue / CURL / harness 评测                       │
│   HTTP REST          HTTP REST                       │
└──────────┬──────────────────────────┬────────────────┘
           │                          │
           ▼                          ▼
┌──────────────────────┐  ┌────────────────────────────┐
│  GET /api/sessions   │  │  GET /api/sessions/{id}/   │
│  GET /api/messages   │  │  messages / traces / ...    │
└──────────┬───────────┘  └───────────┬────────────────┘
           │                          │
           ▼                          ▼
┌─────────────────────────────────────────────────────────┐
│              Bridge (Python FastAPI — Connector层)      │
│                                                         │
│  ┌──────────────┐    ┌────────────────────────────┐     │
│  │ Provider      │    │ 事件驱动同步                 │     │
│  │                   │    │                            │     │
│  │ (Local SQLite │    │ session.chat → 自动存消息   │     │
│  │  + KMM 可选)  │    │ session.list → 同步会话列表  │     │
│  │              │    │ trace 推送 → 存 trace        │     │
│  │ 会话表        │    │ dialogue [DONE] → 标记完成   │     │
│  │ 消息表        │    │                            │     │
│  │ trace 表      │    │                            │     │
│  └──────────────┘    └────────────────────────────┘     │
│                        │                                │
└────────────────────────┼────────────────────────────────┘
                         │ TCP JSON-RPC
                         ▼
┌─────────────────────────────────────────────────────────┐
│              agentflow daemon (Runtime 层)               │
│                                                         │
│  Session pool ["0", "1", "2"] (执行槽位，用完回收)       │
│  DataBus /session/{sid}/... (工作台，dialogue 完成清理)   │
│                         │                               │
│                    ┌────┴────┐                          │
│                    │   UDS   │                          │
│                    └────┬────┘                          │
│                         ▼                               │
│  ┌─────────────────────────────────────────────────┐   │
│  │         Agent 层（承载 Agent 定义）               │   │
│  │  Python Node (组装 system prompt + 调 LLM)      │   │
│  └─────────────────────────────────────────────────┘   │
│                                                         │
│  不感知 Bridge session，不感知持久化                       │
└─────────────────────────────────────────────────────────┘
                         │ HTTP（可选）
                         ▼
               ┌──────────────────┐
               │  KMM 服务         │
               │  (外部持久化, 可选) │
               └──────────────────┘
```

### 数据流方向

```
仅 Provider → Bridge（持久化数据）
  ├── get_messages(session_id) → 历史消息列表
  └── get_session() / list_sessions() → 会话元数据

Bridge → agentflow（完整上下文作为输入）
  └── 一次性发送 message list（历史消息 + 新用户消息）
       agentflow 不感知 Provider，不感知持久化

agentflow → Bridge（结果回调）
  ├── 流式响应（SSE chunk → 透传前端）
  ├── [DONE] + 完整响应
  └── trace 数据（通过 databus.* RPC 拉取）

Bridge → Provider（写入）
  └── save_dialogue(user_input + agent_response)

仅 Bridge → 前端（REST API）
  ├── GET /api/sessions ← 持久化会话列表
  ├── GET /api/sessions/{id}/messages ← 历史消息
  └── GET /api/sessions/{id}/traces ← 执行轨迹

不存在的路径：
  ❌ Python node → Bridge
  ❌ Node 调 Provider
  ❌ 前端直接连 DataBus
```

### 核心流程：每次用户消息的完整生命周期

```
用户新消息 "我昨天感冒了"
  │
  ├─ Step 1: Bridge 收到消息
  │    user_id = "alice", client_id = "web"
  │
  ├─ Step 2: 获取历史上下文
  │    bsession = provider.get_or_create_session("alice", "web")
  │    history  = provider.get_messages(bsession["id"], limit=20)
  │    # history = [{role:"user", content:"你好"},{role:"assistant", content:"你好！"},...]
  │
  ├─ Step 3: 构造 message list（仅历史消息 + 新用户输入，不含 system prompt）
  │    messages = history + [{role:"user", content:"我昨天感冒了"}]
  │    # system prompt 不在 Bridge 构造，由 agentflow 内部 Python node 组装
  │
  ├─ Step 4: 发给 agentflow
  │    daemon_client.chat(
  │        session_id=agentflow_session_id,
  │        messages=messages,      ← 仅含历史对话 + 本条输入
  │        user_id="alice",
  │        client_id="web",
  │    )
  │
  ├─ Step 5: agentflow 内部执行 workflow
  │    │
  │    ├─ agentflow 执行模型（skill 并行调度）：
  │    │    │
  │    │    ├─ coordinator LLM 决策 → 写 next_skills 到 DataBus
  │    │    │    next_skills = ["skill_A", "skill_B", "skill_C"]
  │    │    │
  │    │    ├─ BuiltinDispatcher 读取 next_skills
  │    │    │    for skill in next_skills:
  │    │    │        launch_skill_task(skill)   # 逐个启动
  │    │    │        多个 skill 可同时运行（受 max_concurrent_skills 限制）
  │    │    │
  │    │    ├─ 每个 skill 完成后：
  │    │    │    process_skill_response()
  │    │    │    → 转发结果给 coordinator（re-trigger）
  │    │    │    → coordinator 评估此 skill 的结果
  │    │    │    → 决定再发 next_skills 或输出最终回答
  │    │    │    注意：coordinator 在每完成一个 skill 时就会被 re-trigger
  │    │    │    不是等所有并行 skill 都完成后才触发
  │    │    │
  │    │    └─ 最终：coordinator 确认所有子任务完成
  │    │        → 输出 final=true 响应
  │    │
  │    │  stream chunk 中每个消息携带 progress_skill 字段标记来源 skill，
  │    │  当多个 skill 并行执行时，chunk 可能交叉到达，前端按 progress_skill 归类展示
  │    │
  │    ├─ [Python node] 组装完整 prompt
  │    │    system_prompt = load_system_prompt()        # node 内部定义
  │    │    full_messages = [{"role":"system", "content": system_prompt}]
  │    │                   + messages                    # + Bridge 传来的消息列表
  │    │    llm_response = call_llm(full_messages)        # 调 LLM
  │    │
  │    ├─ [Python node] 其他处理（技能执行、记忆操作...）
  │    │    # 所有中间 LLM 调用、skill 执行结果均作为 trace 写入 DataBus
  │    │
  │    └─ [workflow] 全部节点完成 → 返回响应（含 final=true）
  │
  ├─ Step 6: 接收流式响应 → SSE 聚合
  │    │
  │    ├─ 流式 chunk 实时透传前端（显示用）
  │    │    每个 chunk 携带 progress_skill 标记，供前端按 skill 染色/分区
  │    │
  │    └─ Bridge 内部聚合（ResponseAggregator）：
  │         ┌──────────────────────────────────────────────┐
  │         │  class ResponseAggregator:                    │
  │         │    content_parts = []  ← stream chunk 累加   │
  │         │    final_response = "" ← coordinator 权威响应  │
  │         │    has_final = False                          │
  │         │                                               │
  │         │    add_chunk(chunk):                          │
  │         │      if chunk.progress: return  # 跳过进度消息 │
  │         │      content_parts.append(chunk.content)       │
  │         │                                               │
  │         │    set_final(content):                        │
  │         │      final_response = content                 │
  │         │      has_final = True                         │
  │         │                                               │
  │         │    get_persisted() -> str:                    │
  │         │      # 优先存 final true 的权威响应            │
  │         │      if has_final: return final_response       │
  │         │      # 兜底：聚合 stream chunk                 │
  │         │      return "".join(content_parts)             │
  │         └──────────────────────────────────────────────┘
  │
  ├─ Step 7: 响应完整接收（[DONE] 信号）
  │    │
  │    ├─ response = aggregator.get_persisted()
  │    │   artifacts = aggregator.artifacts  # 最终 chunk 携带的完整 artifacts
  │    │
  │    └─ 持久化：见下方 _on_chat_done
  │
  └─ Step 8: _on_chat_done（落库）
       provider.save_dialogue(bsession["id"],
           user_input="我昨天感冒了",
           agent_response=response,
           dialogue_id=dialogue_id,
       )
       # 异步拉 trace
       _pull_and_save_traces(agentflow_session_id, dialogue_id, bsession["id"])
```

**关键设计约束**：
- agentflow 不感知 Provider，不感知 Bridge session。它只收到一个 message list
- **system prompt 不在 Bridge 构造**，由 agentflow 工作流内的 Python node 自行组装
- Bridge 的职责止于：取历史 → 拼 message list → 发给 daemon
- Python node 内部如需调 LLM，应使用 agentflow 自身的 LLM 调用机制，不从 node 调 Provider

---

## 6. Provider 层设计

### 4.1 数据模型

```sql
-- Bridge 会话（持久化，与 agentflow session 无关）
CREATE TABLE sessions (
    id TEXT PRIMARY KEY,               -- Bridge 侧 session ID（UUID）
    user_id TEXT NOT NULL,
    client_id TEXT,
    agentflow_session_id TEXT,         -- 当前关联的 agentflow session ID（只供引用用）
    title TEXT,                        -- 会话标题。首次消息到达时自动截取用户输入前 50 字
    created_at TEXT NOT NULL,
    last_active_at TEXT NOT NULL,
    status TEXT DEFAULT 'active',       -- active / archived / deleted
    metadata TEXT                      -- JSON 扩展
);

-- 消息表（一问一答 + 扩展消息都存这里）
-- 通过 dialogue_id 隐式表达"一轮"：同一 dialogue_id 下顺序的 user + assistant 为一轮
-- artifacts 存为该 chunk 附带的 ArtifactInfo 列表（JSON），不在文本中嵌入占位符
CREATE TABLE messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,            -- 关联到 sessions(id)
    dialogue_id TEXT,                    -- 对应 agentflow 的 dialogue_id
    role TEXT NOT NULL,                  -- user / assistant / system
    content TEXT NOT NULL,               -- 文本内容。多模态产出存到 artifacts，不在 content 里嵌入标记
    artifacts TEXT DEFAULT '[]',         -- ArtifactInfo 列表 JSON：[{type, url, name, mime_type}, ...]
    created_at TEXT NOT NULL,
    metadata TEXT,                       -- JSON 扩展
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);

-- LLM trace（执行细节，不是对话内容）
CREATE TABLE traces (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    dialogue_id TEXT,
    trace_type TEXT NOT NULL,           -- llm_call / skill_exec / etc.
    trace_data TEXT NOT NULL,           -- JSON
    created_at TEXT NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);
```

**数据模型说明**：

`dialogue_rounds`和`messages`是两种粒度的展示方案，用途不同：

| 表 | 粒度 | 用途 | 一期建？ |
|----|------|------|:-------:|
| `dialogue_rounds` | 一问一答 | 对话历史列表、上下文注入 | ✅ 必建 |
| `messages` | 单条消息 | 逐条展示（如多轮技能内部消息） | ⬜ 可选 |

一期只建 `dialogue_rounds`，`messages` 表为扩展预留。

**标题自动生成**：`sessions.title` 在 `save_dialogue` 首次被调用时，取 `user_input` 的前 50 字符作为标题。后续调用不覆盖（除非显式调 `update_session_title`）。

### 4.2 Provider 接口

```python
class SessionProvider(ABC):
    """Bridge 侧会话持久化接口"""

    # ── 会话管理 ──
    @abstractmethod
    async def get_or_create_session(
        self, user_id: str, client_id: str
    ) -> dict:
        """按 (user_id, client_id) 查找或创建 Bridge session"""
        ...

    @abstractmethod
    async def list_sessions(
        self, user_id: str = "", limit: int = 20, offset: int = 0
    ) -> list[dict]:
        """获取会话列表（前端展示用）"""
        ...

    @abstractmethod
    async def get_session(
        self, session_id: str
    ) -> dict | None:
        """按 ID 获取单个会话详情"""
        ...

    @abstractmethod
    async def find_by_agentflow_session(
        self, agentflow_session_id: str
    ) -> dict | None:
        """按 agentflow session ID 查找关联的 Bridge session"""
        ...

    @abstractmethod
    async def update_agentflow_session_ref(
        self, session_id: str, agentflow_session_id: str
    ):
        """更新当前关联的 agentflow session ID"""
        ...

    @abstractmethod
    async def update_session_status(
        self, session_id: str, status: str
    ):
        """更新会话状态（active / archived / deleted）"""
        ...

    @abstractmethod
    async def update_session_title(
        self, session_id: str, title: str
    ):
        """更新会话标题（首次消息到达时自动设置）"""
        ...

    # ── 消息/对话存储（由 Bridge 在 dialogue [DONE] 后调用）──
    @abstractmethod
    async def save_dialogue(
        self, session_id: str,
        user_input: str, agent_response: str,
        dialogue_id: str, agent_name: str = "",
    ) -> str:
        """存完整一问一答"""
        ...

    @abstractmethod
    async def get_messages(
        self, session_id: str, limit: int = 50
    ) -> list[dict]:
        """获取历史消息"""
        ...

    # ── 上下文注入（对话前构造 message list）──

    @dataclass
    class ContextResult:
        """统一上下文输出格式。KMM 和 Local Provider 都返回此结构"""
        messages: list[dict]   # [{"role":"user","content":"..."}, ...]
        memories: list[str]    # 相关记忆条目（纯文本）

    @abstractmethod
    async def get_context(
        self, session_id: str, user_id: str,
        query: str = "", limit_rounds: int = 5
    ) -> ContextResult:
        """获取历史上下文 + 相关记忆（对话前调用）。
        
        KMMSessionProvider：调 KMM /v1/user/context/query → 解析返回
        LocalSessionProvider：调 get_messages() → 空记忆列表
        """

    # ── Trace ──
    @abstractmethod
    async def save_trace(
        self, session_id: str,
        dialogue_id: str, trace_type: str, trace_data: dict
    ):
        """存 LLM trace"""
        ...

    @abstractmethod
    async def get_traces(
        self, session_id: str, dialogue_id: str = ""
    ) -> list[dict]:
        """查询 trace"""
        ...
```

### 4.3 双实现

| 实现 | 存储 | 适用场景 | 存储内容 |
|------|------|---------|---------|
| `LocalSessionProvider` | SQLite 文件 | 本地开发、单机部署 | 会话、消息、**trace** |
| `KMMSessionProvider` | HTTP → KMM 服务 | aisf 平台集成（可选） | 会话、消息（**不含 trace**，KMM 无 trace 概念） |

`LocalSessionProvider` 是首要实现，`KMMSessionProvider` 仅在需要与 aisf 平台 KMM 服务对接时启用。
Trace 数据只落 `LocalSessionProvider`，KMM 侧不存 trace。

---

## 7. 事件驱动的同步机制

Bridge 不主动管理 agentflow session，而是通过事件驱动的同步机制获取信息：

### 5.1 Dialogue 完成时存储

```python
# routes/chat_rest.py — 流式响应结束后

async def _on_chat_done(
    user_id: str, client_id: str,
    user_input: str, full_response: str,
    dialogue_id: str, agentflow_session_id: str,
):
    # 1. 获取或创建 Bridge session
    bsession = await provider.get_or_create_session(user_id, client_id)
    
    # 2. 更新 agentflow session 引用
    await provider.update_agentflow_session_ref(
        bsession["id"], agentflow_session_id
    )
    
    # 3. 存一问一答
    await provider.save_dialogue(
        bsession["id"], user_input, full_response,
        dialogue_id=dialogue_id, agent_name="",
    )
```

### 5.2 会话列表同步

```python
# services/session_sync.py — 定时/事件驱动同步

class AgentflowSessionSync:
    """将 agentflow session 状态同步到 Bridge session"""
    
    async def sync(self):
        # 1. 调 daemon session.list 获取 agentflow 当前 sessions
        af_sessions = await daemon_client.list_sessions()
        
        # 2. 更新 Bridge provider 中 agentflow_session_id 的状态
        for af_sess in af_sessions:
            bsession = await provider.find_by_agentflow_session(af_sess["id"])
            if bsession:
                await provider.update_status(bsession["id"], af_sess.get("status"))
```

### 5.3 Trace 推送与映射

**现有 trace 通道**（ADR-0004）：Bridge 通过 daemon RPC `databus.get` / `databus.list_keys` 按需拉取 trace。这个通道保持不变——Bridge 在 dialogue 完成后，通过 daemon 拉取该 dialogue 关联的 trace 数据。

**新增：agentflow_session_id → session_id 映射**

Trace 数据来自 daemon，daemon 只感知 `agentflow_session_id` 和 `dialogue_id`，不知道 `session_id`。Bridge 收到 trace 后需要做一次映射查询：

```python
async def _pull_and_save_traces(
    agentflow_session_id: str, dialogue_id: str,
    session_id: str,
):
    """Dialogue 完成后，拉取该 dialogue 的 trace 并落库"""
    
    # 1. 通过 daemon 拉取 trace 原始数据
    #    使用已有的 databus.list_keys + databus.get RPC（ADR-0004）
    trace_keys = await daemon_client.list_keys(
        f"/session/{agentflow_session_id}/llm/{dialogue_id}/"
    )
    for key in trace_keys:
        raw = await daemon_client.databus_get(key)
        
        # 2. 映射：agentflow_session_id + dialogue_id → session_id
        #    （session_id 已由 _on_chat_done 提前保存到调用上下文）
        await provider.save_trace(
            session_id=session_id,
            dialogue_id=dialogue_id,
            trace_type="llm_call",
            trace_data=raw,
        )
```

**映射时机**：在 `_on_chat_done` 中，`save_dialogue` 之后立即拉取 trace，此时 `session_id` 已在调用上下文中，无需额外查询：

```python
async def _on_chat_done(
    user_id, client_id, user_input, full_response,
    dialogue_id, agentflow_session_id,
):
    bsession = await provider.get_or_create_session(user_id, client_id)
    
    # 并存：save_dialogue + pull traces
    await asyncio.gather(
        provider.save_dialogue(
            bsession["id"], user_input, full_response,
            dialogue_id=dialogue_id,
        ),
        _pull_and_save_traces(
            agentflow_session_id, dialogue_id, bsession["id"],
        ),
    )
```

---

## 8. 实施计划

### Phase 1：LocalSessionProvider + 核心流程

| 步骤 | 内容 | 验收标准 |
|------|------|---------|
| 1 | `SessionProvider` 抽象接口 | 接口定义完整，含会话、消息、trace |
| 2 | `LocalSessionProvider`（SQLite 建表 + CRUD） | 单元测试覆盖 CRUD |
| 3 | `routes/sessions.py` — 会话列表 + 消息查询 REST API | 前端可独立验证 API 返回正确数据 |
| 4 | `chat_rest.py` — `[DONE]` 后调 `save_dialogue()` + 拉 trace | 新对话后 SQLite 中有数据 |
| 5 | **前端一次性切换**：会话列表 + 历史消息从 Bridge API 读，停用 daemon `session.list` 轮询 | 前端页面刷新后会话列表和消息正常显示 |
| 6 | 测试：`tests/test_local_session_provider.py` | 覆盖率 > 80% |

#### §6.1 前端切换（步骤 5）

Bridge Provider 就绪后，前端一次性切到 Bridge API，不保留双读过渡：

| 现有行为 | 切换后 |
|---------|:------:|
| `websocket` 轮询 daemon `session.list` | **改为调 Bridge `GET /api/sessions`** |
| 历史消息从 daemon 读取 | **改为调 Bridge `GET /api/sessions/{id}/messages`** |
| 新建会话输入 client_id + user_id | 写 Bridge Provider（不变） |

### Phase 2：事件驱动同步

| 步骤 | 内容 |
|------|------|
| 1 | `AgentflowSessionSync` 定时同步 agentflow session 状态 |
| 2 | `trace` 推送 → Provider 落库 |
| 3 | 前端 trace 展示从 Provider 读 |

### Phase 3：KMM 对接（可选）

| 步骤 | 内容 |
|------|------|
| 1 | `KMMSessionProvider` 实现 |
| 2 | auto 降级模式（KMM 不可用时切 Local） |

---

## 9. 关键不变量

| ID | 不变量 |
|----|--------|
| I-1 | Bridge session ID 不依赖 agentflow session ID。agentflow 侧 session 回收后，Bridge session 不受影响 |
| I-2 | Python node 不发起对 Bridge 的 HTTP/UDS 调用 |
| I-3 | 流式 chunk 不入 Provider（只在 `[DONE]` 后落完整响应） |
| I-4 | agentflow 内循环（协调器/技能循环）不入 Provider（只最终回答） |
| I-5 | Provider 异常不阻塞对话主流程（静默跳过持久化） |
