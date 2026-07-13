# 设计增强全面分析：从 037 到 Bridge 体系

> **层级**: L3 架构研究
> **日期**: 2026-07-13
> **范围**: `docs/enhancements/037-*` 至当前，含 `frontend/bridge/docs/` 全量设计文档
> **分析方法**: 按"回合"归类，逐期分析 agentflow 框架侧与 bridge 侧的分别增强

---

## 0. 分水岭：037 之前的遗留状态

037 之前（2026-06-20）的 AgentFlow 状态：

| 维度 | 状态 | 证据 |
|------|------|------|
| agentflowd daemon | ✅ 已存在（036, Apr 23） | JSON-RPC over TCP/WS，17 个方法 |
| C++ 框架层 | ✅ 6 项增强已完成 | 009/021/022/023/029/030 |
| Harness Agent | ⚠️ 仅示例级别 | `examples/agent/harness-agent/`，单 skill，提示词硬编码 |
| Python 执行节点 | ⚠️ 无 pip 包分发 | 节点代码直接在 `nodes/` 下，靠物理复制分发给各 Agent |
| 用户界面 | ❌ 无 | 仅 `test_client.py` + `nc` 命令 |
| Agent Package 创建 | ❌ 手动 | 手动写 `agent.json` + workflow JSON + node 脚本 |
| Skill 热更新 | ❌ 无 | package 部署后不可变更 |
| MCP 集成 | ❌ 无 | `.mcp.json` 存在但 Agent 不读 |

---

## 第一回合：Skills 体系建立（037 ⟷ 038 ⟷ 040）

**时期**: 2026-06-20 ~ 2026-06-22

### 框架侧（agentflow C++）

#### 037 - Skills 动态上传接口（implemented）

| 维度 | 内容 |
|------|------|
| **daemon 新增 RPC** | `agent.upload_skills`、`agent.list_skills`、`agent.remove_skill`（3 个新方法） |
| **C++ 变更** | `rpc_handler.cpp` +250 行、`agent_registry` 新增 `get_package_path()` |
| **结构引入** | `harness/skills/` 作为 Agent package 的动态目录约定 |
| **关键设计** | Base64 zip → 解压 → 校验 SKILL.md → 写入 `harness/skills/{name}/`，`force` 参数控制覆盖 |
| **向后兼容** | 新增 RPC 不影响已有方法 |

#### 038 - Harness Agent 能力升级（tbd → 已实现）

| 维度 | 内容 |
|------|------|
| **Python node 变更** | `llm_think_and_act.py` V2：新增 `load_skills`、`list_skills`、`call_tool` 三个工具 |
| **配置去硬编码** | 所有配置从 `agent.md` frontmatter 读取（model/skills/prompt），Python 代码零硬编码 |
| **多轮对话** | `databus_client.py` 新增 `read_dialogue_context`/`write_dialogue_context`，使用 session 级固定 key |
| **session.list 修复** | 框架侧 `MMAgent` 新增 `get_active_sessions()`，`rpc_handler.cpp` 调用该方法 |
| **框架 C++ 改动** | `agent.h/cpp` 新增 `get_active_sessions()`，约 20 行 C++ |
| **向后兼容** | 示例层改动，不影响已有 Agent |

#### 040 - Harness Agent MCP 协议集成（tbd）

| 维度 | 内容 |
|------|------|
| **新增文件** | `harness/tools/mcp_bridge.py` — MCP 桥接层（零 C++ 改动） |
| **工具注册扩展** | `harness/tools/__init__.py` 新增 `_load_mcp_tools()` |
| **三种传输** | stdio（本地进程）、SSE（远程推送）、Streamable HTTP（推荐远程） |
| **命名空间** | `mcp__{server_name}__{tool_name}` 避免与本地工具冲突 |

### Bridge 侧（Python FastAPI + 设计文档）

此阶段 Bridge 尚未开始实现。Bridge 的代码从一开始（main.py）就是按"对接这些能力的 consumer"设计的。

### 回合总结

```
Agent Package 存储
  └── harness/skills/{name}/SKILL.md    ← 037: 动态上传入口
        ↓ Agent 启动时
  load_skills tool                          ← 038: LLM 自主加载
        ↓
  LLM 消化 SKILL.md 内容 → function-calling ← 038 + 040
        ↓
  tool_registry.execute()
    ├── harness/tools/*.py                 ← 本地工具
    └── MCPBridge → MCP Server             ← 040: 外部工具
```

---

## 第二回合：前端 + Bridge 体系建立（039 → Bridge ADRs）

**时期**: 2026-06-22 ~ 2026-06-25

### 框架侧（agentflow C++）

此回合框架侧无改动。所有工作在 Bridge 侧。

### Bridge 侧（Python FastAPI）

#### 039 - Vue 前端架构设计

| 维度 | 内容 |
|------|------|
| **架构模式** | Vue 3 + Vite + TypeScript → Python FastAPI Bridge → agentflowd daemon |
| **端口约定** | Vue:15173, Bridge:8000, daemon:17891 |
| **关键设计决策** | 前端不直连 daemon，Bridge 直读文件系统，daemon 连接仅用于 agent.execute/load/unload/session |
| **前端路由** | `/packages` 列表 → `/packages/:name` 编辑器 → `/chat` 问答 |

#### ADR-0001: FastAPI 框架

| 维度 | 内容 |
|------|------|
| **决策** | 使用 Python FastAPI 而非 Node.js/Go |
| **理由** | 快速开发、原生 WebSocket 支持、pydantic 类型校验、与团队技能匹配 |

#### ADR-0002: 单 WebSocket 长连接

| 维度 | 内容 |
|------|------|
| **决策** | Bridge 与 daemon 之间维护**单个持久 WebSocket**，所有请求共享 |
| **请求分派** | 递增 `request_id` → `_pending` dict → asyncio.Future |
| **通知消费** | `agent.response` 推送 → `_response_queues[dialogue_id]` |
| **重连保证** | `_daemon_status_poll_loop` 后台自动重连 |

#### ADR-0003: Daemon 状态缓存

| 维度 | 内容 |
|------|------|
| **决策** | `health` + `runtime.status` 结果缓存 30s，不每次请求都调 daemon |
| **理由** | 保护 daemon 不被监控请求淹没 |

#### ADR-0004: LLM Trace DataBus 桥接

| 维度 | 内容 |
|------|------|
| **决策** | LLM 调用链数据存 DataBus，Bridge 通过 daemon 的 databus.* RPC 读取 |
| **理由** | DataBus 是独立子系统，Bridge 不能直连 |

#### ADR-0005: CoreMind 服务发现

| 维度 | 内容 |
|------|------|
| **决策** | CoreMind SideCar 通过 UDS 与 Bridge 通信，Bridge 转发到 daemon |
| **理由** | 服务发现独立于 daemon 的主 RPC 通道 |

### 回合总结

```
Vue 前端 ─HTTP/WS→ Bridge ─WS JSON-RPC→ agentflowd daemon
                     │
                     ├── 文件系统直读（Agent Package CRUD）
                     ├── 会话持久化（SessionProvider）
                     └── LLM Trace 缓存
```

---

## 第三回合：Bridge 八大功能模块构建

**时期**: 2026-06-25 ~ 2026-07-03

### 框架侧（agentflow C++）

#### 041 - Daemon DataBus RPC 代理（wip）

| 维度 | 内容 |
|------|------|
| **daemon 新增 RPC** | `databus.get`、`databus.list_keys`（2 个新方法） |
| **关键设计** | 不是 DataBus 的透明代理，而是 **SessionRouter 桥接**：外部传 `(user_id, client_id)`，daemon 内部解析为 session_id |
| **C++ 变更** | `rpc_handler.cpp` + `src/daemon/` handler 实现 |
| **动机** | Bridge 需要查询 LLM Trace 数据（`llm_trace.py`），但 DataBus UDS 是 per-session 动态生成，Bridge 无法直连 |
| **当前状态** | wip — 设计中 |

### Bridge 侧（Python FastAPI）

此阶段 Bridge 构建了完整的 8 大功能模块：

#### 1. Package 管理 API

| 设计文档 | 核心能力 |
|---------|---------|
| `features/package-management.md` | 文件树浏览、CRUD、打包(tar.gz)、上传、构建 |
| `features/agent-template-merge.md` | `create-from-template`：用户只需要 `agent.md` + skill，框架自动补齐 |
| `architecture/modules/package-api.md` | 文件系统操作：list/read/write/rename/delete |

#### 2. Agent 管理 API

| 设计文档 | 核心能力 |
|---------|---------|
| `architecture/modules/agent-api.md` | `GET/POST /api/agents/*` 代理 7 个 daemon RPC |
| `features/agent-management.md` | 生命周期：load/unload/pause/resume + 配置注入 |

#### 3. 聊天 API（双模式 + SSE）

| 设计文档 | 核心能力 |
|---------|---------|
| `features/chat.md` | WebSocket 流式 + REST 同步 + SSE 流式 |
| `architecture/modules/chat-api.md` | 协议定义：ChatRequest → ChatChunk → ChatDone |
| `modules/streaming-response.md` | SSE 推送设计：chunk→thinking→tool_call→artifact |
| `modules/dialogue-interrupt-propagation.md` | 中断传播：stop 信号 → `agent.send_control(DIALOGUE-INTERRUPT)` |
| `modules/dialogue-serial-lock-and-cancel.md` | 串行锁：同一 `(user_id,client_id)` 只能一个活跃对话 |

#### 4. Session 管理 API

| 设计文档 | 核心能力 |
|---------|---------|
| `architecture/modules/session-api.md` | `session.list/destroy/clear_dialogue` 代理 |
| `features/session-management.md` | `session_id` 隐藏在前端之外，前端以 `client_id` 为会话标识 |
| `features/bridge-session-persistence.md` | 持久化 Session 历史：`SessionProvider`（local SQLite） |
| `features/session-dialogue-mapping.md` | `(user_id, client_id)` ↔ `dialogue_id` 映射 |

#### 5. Daemon 监控 API

| 设计文档 | 核心能力 |
|---------|---------|
| `architecture/modules/daemon-api.md` | `health` + `runtime.status` 后台缓存 |
| `features/daemon-monitoring.md` | `_daemon_status_poll_loop` 30s 间隔刷新 |

#### 6. LLM Trace 查询 API

| 设计文档 | 核心能力 |
|---------|---------|
| `architecture/modules/llm-trace.md` | `databus.list_keys` + `databus.get` 代理 |
| ADR-0004 | DataBus 是独立子系统，Bridge 通过 daemon 访问 |

#### 7. Skill Hub API

| 设计文档 | 核心能力 |
|---------|---------|
| `architecture/modules/skill-hub-api.md` | 扫描 `harness/skills/` 目录，返回 skill 元数据 |
| `features/skill-hub.md` | 技能中心：浏览、搜索、预览 SKILL.md |

#### 8. CoreMind SideCar 桥接

| 设计文档 | 核心能力 |
|---------|---------|
| `architecture/modules/coremind-bridge.md` | C++ SideCar ↔ Bridge UDS ↔ daemon |
| `architecture/modules/service-discovery.md` | 服务发现：SideCar 注册 → Bridge 转发 |

### 技术通信协议

| 设计文档 | 核心能力 |
|---------|---------|
| `architecture/modules/daemon-client-protocol.md` | `DaemonClient` 连接/请求/通知/重连协议 |
| `architecture/modules/config-system.md` | 3 层配置源：代码默认 → YAML → 环境变量 |
| `modules/llm-client-provider-integration.md` | LLM 客户端提供者集成 |
| `modules/llm-uds-client-design.md` | LLM UDS 客户端设计 |
| `modules/uds-server-openai-compatible-design.md` | OpenAI 兼容的 UDS Server |

---

## 第四回合：Harness Agent 架构升级（042 系列 + Agent Template Delta）

**时期**: 2026-06-30 ~ 2026-07-11

### 框架侧（agentflow C++）

#### 042 - Generic Harness Agent Package（implemented）

这个 enhancement 的**主体在 Python 节点层**，但利用了此前所有 C++ 框架增强的积累：

| 此前 C++ 增强 | 在本方案中的作用 |
|--------------|----------------|
| 009 Coordinator Task0 默认化 | agent.json 中 `tasks[0]` dispatcher 固定到 coordinator-skill |
| 022 Multi-Response | `run_skills` 产出 `next_skills` 后 wait，再收到 skill 结果时由 coordinator 正常走 response 推送 |
| 023 SkillSpec + exec_skill_key | coordinator 通过 DataBus `skill_prompt:{name}` + `exec_skill_key` 参数化 skill |
| 030 AsyncPending | skill executor 的非阻塞 LLM 调用 |
| 036 agentflowd daemon | 整个方案的运行时基础 |
| 041 databus.* RPC | coordinator 和 skill 之间通过 DataBus 交换 `skill_prompt`/`load_skills`/`skill_result` |

**新增概念**：

| 概念 | 说明 | 实现层面 |
|------|------|---------|
| `general_task_executor.py` | 统一 node，coordinator 和 generic-skill 共享 | Python node |
| `run_skills` tool | coordinator 的唯一工具，委派 skill 执行 | Python node |
| `coord:dispatch` DataBus key | coordinator 写入 skill 指派信息 | DataBus 协议 |
| `skill_prompt:{name}` | coordinator 写入 skill 的任务指令 | DataBus 协议 |
| `coord_messages` / `coord_checkpoint` | coordinator 重入状态持久化 | DataBus ctx 命名空间 |

#### 042 - Coordinator Progress Push（tbd）

| 维度 | 内容 |
|------|------|
| **C++ 框架改动** | `agent.cpp` `COORDINATORDataExchange` handler 中新增 progress push 点（约 20 行） |
| **Python node 改动** | `coordinator_decide.py` P0 阶段条件性地 `collect_skill_results`，P1 dispatch 后删除 response key |
| **新增推送路径** | `session_router_->enqueue_response_message()` 第 6 个调用点，`final="false"` |
| **向后兼容** | 新增推送不触发 cleanup/destroy，不覆盖最终 response |

#### Agent Template Delta（草案）

| 维度 | 内容 |
|------|------|
| **架构** | 四层：用户自定义层 → 入口文件层 → pip 运行时层 → adapter 层 |
| **pip 包** | `agentflow-runtime`：通用 Agent 执行引擎，pip install 到 Agent 包 `.venv` |
| **shell wrapper** | `.sh` 检测 `.venv` → 选 python → exec `.py`（daemon exe_loader 不够灵活） |
| **sentinel 机制** | `.venv/.agentflow_installed` 作为 venv 就绪的原子标记 |
| **改动范围** | 新建 `packages/agentflow-runtime/`，不改 C++ daemon |

### Bridge 侧

Bridge 在此阶段主要在 **消费** 框架侧提供的能力：

| Bridge 能力 | 消费的框架增强 |
|------------|--------------|
| `POST /api/chat/{agent_id}` (SSE 流式) | 022 Multi-Response + 036 daemon agent.response 推送 |
| `POST /api/packages/create-from-template` | agent-template-delta 的 template 合并机制 |
| `routes/chat.py` 中断 stop | 023 DIALOGUE-INTERRUPT 控制指令 |
| `routes/llm_trace.py` | 041 databus.get/list_keys |

---

## 按层汇总

### agentflow C++ 框架层（6 项 enhancement）

| 增强 | 时期 | 核心改动 | 改动量 |
|------|------|---------|--------|
| 037 Skills 动态上传 | Jun 20 | `rpc_handler.cpp` +3 RPC 方法，`agent_registry` 扩展 | ~300 行 |
| 038 session.list 修复 | Jun 21 | `agent.h/cpp` 新增 `get_active_sessions()` | ~20 行 |
| 041 DataBus RPC 代理 | Jun 25 | `rpc_handler.cpp` + `databus.get/list_keys` | ~200 行（wip） |
| 042 Progress Push | Jul 4 | `agent.cpp` COORDINATORDataExchange handler 新增推送点 | ~20 行 |
| 042 Generic Harness Agent | Jun 30 | **零框架改动**（纯 Python node） | 0 行 C++ |
| Agent Template Delta | Jul 11 | **零框架改动**（新建 pip 包，不改 daemon） | 0 行 C++ |

### Bridge Python 层（8 大模块 + 5 个 ADR）

| 模块 | 时期 | 文件数 | 核心模式 |
|------|------|--------|---------|
| Package 管理 | Jun 25 ~ Jul 3 | 1 router + 设计文档 ×3 | 直读文件系统，不经过 daemon |
| Agent 管理 | Jun 25 ~ Jul 3 | 1 router + 设计文档 ×2 | HTTP → JSON-RPC 代理 |
| 聊天 (WS/SSE/REST) | Jun 25 ~ Jul 3 | 2 router + 设计文档 ×5 | DaemonClient 长连接 + notification 队列 |
| Session 管理 | Jun 25 ~ Jul 3 | 1 router + 设计文档 ×4 | HTTP → JSON-RPC 代理 + SessionProvider |
| Daemon 监控 | Jun 25 ~ Jul 3 | 1 router + 设计文档 ×2 | 后台缓存（30s 间隔） |
| LLM Trace | Jun 25 ~ Jul 3 | 1 router + 设计文档 ×1 | databus.* RPC 代理 |
| Skill Hub | Jun 25 ~ Jul 3 | 1 router + 设计文档 ×2 | 直读文件系统 |
| CoreMind 桥接 | Jun 25 ~ Jul 3 | 1 router + 设计文档 ×2 | UDS ↔ daemon 转发 |

### 前端 Vue 层

| 模块 | 时期 | 关键组件 |
|------|------|---------|
| Package 编辑器 | Jun 22 ~ 至今 | `PackageCreator.vue`（DAG 编排器）、`FileTree.vue`、`CodeEditor.vue` |
| 聊天界面 | Jun 22 ~ 至今 | `ChatView.vue`、`AgentList.vue`、`SessionList.vue` |
| 包列表管理 | Jun 22 ~ 至今 | `PackagesPage.vue`（新建/上传/更新/刷新） |
| Workflow DAG | Jun 22 ~ 至今 | `WorkflowDAG.vue`（Vue-Flow 可视化） |
| Skill Hub | Jun 22 ~ 至今 | `SkillHubPage.vue`、`SkillDetailPage.vue` |
| 状态管理 | Jun 22 ~ 至今 | 6 个 Pinia store + 2 个 composable |

---

## 两方设计文档体系对比

| 维度 | agentflow 侧（`docs/enhancements/`） | Bridge 侧（`frontend/bridge/docs/`） |
|------|--------------------------------------|--------------------------------------|
| **文档级别** | L4 技术设计 | L2 ~ L4（L2 features + L3 ADRs + L4 modules） |
| **文档结构** | 单一 `.md` 文件，按 enhancement 独立 | 分层目录：`adrs/`、`architecture/modules/`、`features/`、`modules/` |
| **状态管理** | tbd → wip → implemented | 头部 meta 标记 + ADR 状态（Proposed/Accepted） |
| **覆盖范围** | C++ 框架 + Python node | Python bridge + 与 daemon 的契约 |
| **变更粒度** | 每篇约300~1500行，覆盖一个完整功能 | 每篇约100~300行，粒度更细，可独立审阅 |
| **定位** | 记录"框架做了什么改变" | 记录"Bridge 如何消费框架能力" |

---

## 关键设计决策时间线

```
Jun 20  037 Skills 动态上传              daemon RPC 扩展
Jun 21  038 Harness Agent 升级           Python node 能力
Jun 22  039 Vue 前端                     前端+Bridge 体系启动
Jun 22  040 MCP 协议集成                  Python node 外部工具
Jun 25  041 DataBus RPC 代理             daemon RPC 扩展
Jun 25  ADR-0001 FastAPI 框架            技术选型
Jun 25  ADR-0002 单 WebSocket 连接       通信模型
Jun 25  ADR-0003 状态缓存                监控策略
Jun 25  ADR-0004 LLM Trace 桥接          数据访问策略
Jun 25  ADR-0005 CoreMind 服务发现       跨进程通信
Jun 30  042 通用 Harness Agent Package   架构升级（Python 层）
Jul  3  Agent Template Merge             Package 创建体验
Jul  4  042 Coordinator Progress Push    C++ 框架补充
Jul 11  Agent Template Delta             四层架构 + pip 包
```

---

## 结论

从 037 开始的增强有一条清晰的演化主线：

1. **Skills 基座建立**（037→038→040）：先有动态上传能力，然后在 Agent 内部消费，最后扩展到 MCP 生态工具
2. **Bridge 基础设施**（039→ADRs→8大模块）：先定架构和通信协议，然后并行构建全部功能模块
3. **架构升级**（042→Template Delta）：在不改 C++ 框架的前提下，通过 Python 节点层重构实现通用 Harness Agent

**关键事实**：037 之后，C++ 框架层的新增改动极小（037~300 行、041~200 行、042~20 行），绝大部分增强在 Python 节点层和 Bridge 层完成。这说明 agentflowd 和 C++ 框架在 036 之后已经提供了足够的基础抽象，后续演进主要依赖"外挂式"增强。

---

## 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-07-13 | AtomCode | 初始版本：全量分析 037 至 Bridge 体系的 4 个回合增强 |
