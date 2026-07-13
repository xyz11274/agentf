# Agent 节点执行配置注入方案

> **层级**: L3 · `docs/design/node-execution-config.md`
> **状态**: 草案 v0.1
> **创建日期**: 2026-07-11

---

## 1. 背景与问题

### 1.1 现状：两条并行注入路径

当前 agent 节点执行时，配置通过两条独立路径注入到子进程环境变量：

```
路径 A：daemon C++ worker.cpp
  └─ execve 时 push_back 环境变量
     ├─ AF_LLM_MODEL, AF_LLM_BASE_URL, AF_LLM_API_KEY  ← 来自 request.llm_config
     ├─ AF_DATABUS_*, AF_PLUGIN_UDS_FILE               ← 运行时基础设施
     └─ 动态 agent package env                           ← 来自 agent 包配置

路径 B：Bridge Python load_agent
  └─ 写入 agent 包 .env 文件
     ├─ AF_LLM_API_KEY, AF_LLM_BASE_URL, AF_LLM_MODEL  ← 与路径 A 重叠
     ├─ AF_FALLBACK_LLM_*, 协调器/skill 控制参数        ← 路径 A 未覆盖
     └─ AF_BR_VISION_*                                  ← 路径 A 未覆盖
```

### 1.2 核心问题

**问题 1：两条路径重叠，产生歧义**

`AF_LLM_API_KEY`、`AF_LLM_BASE_URL`、`AF_LLM_MODEL` 同时被 daemon 和 Bridge 注入。daemon 的值来自 `agent.md` + `request.llm_config`，Bridge 的值来自 `agent_defaults`。当两者不一致时，execve 的环境变量优先级高于 `.env` 文件，导致 Bridge 的配置被静默覆盖。

**问题 2：框架注入能力不足**

daemon C++ 的 `worker.cpp` 只注入了有限的几个 LLM 配置项：

```cpp
// worker.cpp 支持注入的 LLM 配置
AF_LLM_MODEL     ← 支持
AF_LLM_BASE_URL  ← 支持
AF_LLM_API_KEY   ← 支持
AF_LLM_TEMPERATURE  ← 不支持
AF_LLM_TOP_P        ← 不支持
AF_LLM_MAX_TOKENS   ← 不支持
// ... 更多 LLM 调参都不支持
```

扩展框架 C++ 代码去逐个加字段成本高、周期长，不可持续。

**问题 3：单一模型配置，无法按节点类型区分**

当前所有节点共享同一个 `AF_LLM_MODEL`：

| 节点类型 | 需要的模型 | 当前能否独立配置 |
|---------|-----------|----------------|
| Coordinator | 强推理模型（用于决策编排） | ❌ 共享 AF_LLM_MODEL |
| Skill executor | 可配置不同模型 | ❌ 共享 AF_LLM_MODEL |
| VL 多模态 skill | VL 模型（图片理解） | ⚠️ 走独立的 AF_BR_VISION_* |

**问题 4：运行时上下文 vs 业务配置未区分**

当前 daemon 注入的变量混合了两种不同性质的变量：

| 类别 | 变量 | 性质 |
|------|------|------|
| **运行时上下文** | `AF_NODE_ID`, `AF_DIALOGUE_ID`, `AF_SESSION_ID`, `AF_DATABUS_UDS_FILE`, `AF_SKILL_NAME`, `AF_COMPLETED_SKILL_NAME` | 每次执行不同，由框架动态生成 |
| **业务配置** | `AF_LLM_MODEL`, `AF_LLM_API_KEY`, `AF_LLM_BASE_URL` | 可静态配置，由运维/开发者设置 |

两者混在一起，导致无法清晰界定"框架负责什么、Bridge 负责什么"。

---

## 2. 设计目标

1. **统一配置注入路径**：业务配置只通过 Bridge 一个入口注入，消除并行歧义
2. **按节点类型配置**：Coordinator、Skill executor 可以独立配置模型和调参
3. **可扩展**：新增 LLM 调参（temperature, top_p 等）无需修改 C++ 框架
4. **向后兼容**：现有节点代码无需大规模修改
5. **不修改 C++ 框架**：所有能力在 Bridge Python 层 + 节点 Python 层实现

---

## 3. 方案：管辖域重定义与统一注入

### 3.1 管辖域划分

```
┌─────────────────────────────────────────────────────┐
│                 daemon C++ 框架职责                   │
│  ─────────────────────────────────────────────────── │
│  运行时上下文（每次执行不同，框架动态生成）              │
│  ├─ AF_NODE_ID             节点标识                   │
│  ├─ AF_DIALOGUE_ID         对话轮次 ID                │
│  ├─ AF_SESSION_ID          会话 ID                    │
│  ├─ AF_DATABUS_UDS_FILE    DataBus 通信路径           │
│  ├─ AF_SKILL_NAME          当前执行的 skill 名         │
│  ├─ AF_COMPLETED_SKILL_NAME 上一轮完成的 skill 名      │
│  └─ AF_SKILL_SUCCESS / RESULT / ARTIFACTS 等         │
└─────────────────────────────────────────────────────┘
                            │
                            │ execve 注入
                            ▼
┌─────────────────────────────────────────────────────┐
│                  Bridge 框架职责                       │
│  ─────────────────────────────────────────────────── │
│  业务配置（可静态配置，按环境/节点类型区分）              │
│  ├─ 全局 LLM 配置（回退）                              │
│  │   ├─ AF_LLM_MODEL / AF_LLM_BASE_URL / AF_LLM_API_KEY │
│  │   └─ AF_LLM_TEMPERATURE / AF_LLM_TOP_P (新增)       │
│  ├─ Coordinator 专用配置                              │
│  │   ├─ AF_COORDINATOR_LLM_MODEL (新增)               │
│  │   ├─ AF_COORDINATOR_MAX_COMPLETION_TOKENS          │
│  │   └─ AF_COORDINATOR_MAX_ITERATIONS                 │
│  ├─ Skill executor 专用配置                           │
│  │   ├─ AF_SKILL_LLM_MODEL (新增)                     │
│  │   ├─ AF_SKILL_MAX_COMPLETION_TOKENS                │
│  │   └─ AF_SKILL_LLM_RECOVERY_ATTEMPTS               │
│  └─ VL 多模态配置                                     │
│      └─ AF_BR_VISION_*                                │
└─────────────────────────────────────────────────────┘
                            │
                            │ load_agent 写入 .env
                            ▼
                   ┌───────────────────┐
                   │   Agent 节点进程    │
                   │   (子进程继承全部)  │
                   └───────────────────┘
```

### 3.2 核心变更

**daemon C++ 停止注入的业务配置**（不修改 C++ 代码，而是通过配置/约定）：

| 变量 | 当前注入方 | 目标注入方 |
|------|-----------|-----------|
| `AF_LLM_MODEL` | daemon + Bridge | Bridge 唯一 |
| `AF_LLM_BASE_URL` | daemon + Bridge | Bridge 唯一 |
| `AF_LLM_API_KEY` | daemon + Bridge | Bridge 唯一 |

> **理由**：daemon 的 `request.llm_config` 数据来源于 `agent.md` 的 `model` 字段。节点代码中 `resolve_llm_config(agent_md_model)` 已直接读取 `agent.md model` 并作为最高优先级。daemon 注入的 `AF_LLM_MODEL` 是冗余的——即使去掉，节点代码仍能正确获取模型配置。

**Bridge 新增的配置项**：

| 环境变量 | 对应 agent_defaults 路径 | 用途 |
|---------|------------------------|------|
| `AF_LLM_TEMPERATURE` | `agent_defaults.llm.temperature` | LLM 温度参数 |
| `AF_LLM_TOP_P` | `agent_defaults.llm.top_p` | LLM top_p 采样 |
| `AF_COORDINATOR_LLM_MODEL` | `agent_defaults.coordinator.model` | Coordinator 专用模型 |
| `AF_COORDINATOR_LLM_TEMPERATURE` | `agent_defaults.coordinator.temperature` | Coordinator 专用温度 |
| `AF_SKILL_LLM_MODEL` | `agent_defaults.skill.model` | Skill 专用模型 |
| `AF_SKILL_LLM_TEMPERATURE` | `agent_defaults.skill.temperature` | Skill 专用温度 |

### 3.3 AgentConfig 按节点类型读取

```python
class AgentConfig:
    @staticmethod
    def llm_model(agent_md_model: str = "", node_type: str = "") -> str:
        """按节点类型解析模型名。

        优先级：
          1. agent.md model 字段（最高）
          2. 节点专用 env var（如 AF_COORDINATOR_LLM_MODEL）
          3. 全局 AF_LLM_MODEL
          4. AF_FALLBACK_LLM_MODEL
        """
        if not _is_placeholder(agent_md_model):
            return agent_md_model

        # 节点专用配置
        if node_type == "coordinator":
            v = _get("AF_COORDINATOR_LLM_MODEL")
            if v:
                return v
        elif node_type == "skill":
            v = _get("AF_SKILL_LLM_MODEL")
            if v:
                return v

        # 全局回退
        v = _get("AF_LLM_MODEL")
        if v:
            return v
        v = _get("AF_FALLBACK_LLM_MODEL")
        if v:
            return v
        return "gpt-4o-mini"
```

---

## 4. 详细设计

### 4.1 数据流

```
┌──────────────────────────────────────────────────────────────────┐
│                        配置定义阶段                                 │
│                                                                    │
│  configs/{env}/bridge.yaml                                        │
│  └─ agent_defaults:                                               │
│       ├─ llm: { model, base_url, api_key, temperature, top_p }    │
│       ├─ coordinator: { model, max_iterations, temperature }      │
│       ├─ skill: { model, max_iterations, temperature }            │
│       └─ vision: { base_url, model, api_key }                     │
│           │                                                       │
│           ▼                                                       │
│  Bridge load_agent                                                │
│  └─ 写入 agent 包 .env（_AGENT_ENV_DEFAULTS 映射）                 │
│                                                                    │
│  运行时注入阶段                                                    │
│                                                                    │
│  daemon worker.cpp                                                │
│  └─ execve 子进程                                                  │
│       ├─ 运行时上下文（AF_NODE_ID, AF_DIALOGUE_ID, ...）           │
│       └─ 子进程继承 .env 文件（业务配置）                           │
│           │                                                       │
│           ▼                                                       │
│  Agent 节点代码                                                    │
│  └─ AgentConfig.llm_model(node_type="coordinator")                │
│       ├─ agent.md model 优先                                      │
│       ├─ 次选 AF_COORDINATOR_LLM_MODEL                            │
│       ├─ 再选 AF_LLM_MODEL                                        │
│       └─ 最后 AF_FALLBACK_LLM_MODEL                               │
└──────────────────────────────────────────────────────────────────┘
```

### 4.2 配置示例

```yaml
# configs/prod/bridge.yaml → agent_defaults
agent_defaults:
  # ── 全局 LLM 配置（所有节点的通用回退） ──
  llm:
    api_key: "${AF_LLM_API_KEY}"
    base_url: "${AF_LLM_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}"
    model: "${AF_LLM_MODEL:-deepseek-v4-flash}"
    temperature: 0.7
    top_p: 0.9

  # ── Coordinator 专用配置（覆盖全局） ──
  coordinator:
    model: ""                    # 空=使用全局 AF_LLM_MODEL
    temperature: 0.3             # 决策场景用较低温度
    max_iterations: 10
    max_completion_tokens: 8192

  # ── Skill executor 专用配置（覆盖全局） ──
  skill:
    model: "qwen-vl-max"         # skill 用 VL 模型
    temperature: 0.5
    max_iterations: 8
    max_completion_tokens: 8192
    llm_recovery_attempts: 2
    node_timeout_sec: 240
    model_max_tokens: 128000
    compaction_threshold: 0.8
```

### 4.3 迁移路径

| 阶段 | 内容 | 影响 |
|------|------|------|
| **Phase 1** | 新增 `AF_COORDINATOR_LLM_MODEL`、`AF_SKILL_LLM_MODEL` 等 env var，更新 `_AGENT_ENV_DEFAULTS` 和 `AgentConfig` | 新增配置，不影响现有行为 |
| **Phase 2** | 节点代码按 `node_type` 调用 `AgentConfig.llm_model(node_type="coordinator")` | 逐步迁移，回退到 `AF_LLM_MODEL` |
| **Phase 3** | 从 daemon worker.cpp 停止注入 `AF_LLM_MODEL`、`AF_LLM_BASE_URL`、`AF_LLM_API_KEY`（可选） | 消除并行注入，C++ 修改 |

---

## 5. 已确认的设计决策

| # | 事项 | 决策 | 理由 |
|---|------|------|------|
| 1 | daemon 停止注入 `AF_LLM_*` | ✅ 可行，`request.llm_config` 仅有两个消费方：env 注入（我们想停的）+ 已废弃的 `StreamingLLMClient`（pitfall #4），无其他消费方 | 安全 |
| 2 | 节点专用模型命名 | `AF_COORDINATOR_LLM_MODEL`、`AF_SKILL_LLM_MODEL`（`AF_LLM_COORDINATOR_MODEL` 方案弃用） | 与 `AF_LLM_*` 系列同源 |
| 3 | `agent.md model` 优先级 | 保持最高优先级，用户配置 > 框架默认。仅在模型不在可用范围内时往下走 fallback 链 | 用户配置优先 |
| 4 | LLM 调参参数提供方式 | 由 `AgentConfig` 统一提供，各节点通过 `common.agent_config` 读取，不直接 `os.environ.get()` | 统一入口，消除不确定性 |