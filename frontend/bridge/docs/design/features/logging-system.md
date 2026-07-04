# 日志系统设计

> **层级**: L2 · `docs/design/features/logging-system.md`
> **状态**: 草案
> **创建者**: AtomCode（trace 空条目排查驱动）
> **创建日期**: 2026-07-04

---

## What

统一 AgentFlow 三组件（daemon C++、bridge Python、Agent 节点 Python 脚本）的日志输出方式，解决当前日志散落、stderr 丢失、格式不统一的问题。

## Why

### 当前问题清单

| # | 问题 | 影响 |
|---|------|------|
| 1 | Agent 节点 Python 脚本（`llm_call.py`、`databus_client.py`）用 `print(msg, file=sys.stderr)` 写日志，但 daemon 启动时 `\|` 只管道 stdout，**stderr 不入文件** | 运行时诊断日志全部丢失，只有终端可见 |
| 2 | 三类组件各自使用不同的日志方式——daemon C++ 用 `Logger::instance()`、bridge 用 Python `logging`、节点脚本用 `print(stderr)` | 格式不统一，无法用同一工具聚合分析 |
| 3 | 节点脚本的 `_log_warn` 没有日志级别控制（永远是 WARN） | 无法按 debug/info/warn/error 过滤 |
| 4 | 无日志轮转策略 | `agentflowd.log` 单文件持续增长，历史日志手动 `cp` 备份 |
| 5 | `/tmp/af_*.log` 测试残留堆积 | 污染 `/tmp`，与运行日志混淆 |
| 6 | 多个 daemon 实例写同一日志文件 | 日志行交错，无法区分属于哪个进程 |

### 触发场景

- **Trace 空条目排查**：`_verify_meta_written` 的 `CRITICAL` 日志因 stderr 不入文件而无法在日志中查到（本问题的直接触发点）
- **生产排障**：Agent 运行时异常无法从日志文件回溯，必须复现到终端
- **CI 验证**：自动化测试无法通过日志断言验证节点行为

---

## How

### 核心原则

1. **所有日志必须落地文件**：没有组件可以只写终端不写文件
2. **统一日志目录**：全部写入 `logs/` 目录
3. **进程唯一日志文件**：每个 daemon 进程写自己的文件，避免交错
4. **结构化格式**：`[时间] [级别] [组件] 消息`
5. **可配置级别**：每组件支持独立日志级别

### 架构图

```
┌──────────────────────────────────────────────────────────────────┐
│                        logs/ 目录                                 │
│                                                                  │
│  agentflowd.log       ← daemon C++ Logger + 嵌入 Python stderr   │
│  bridge.log           ← bridge Python logging                    │
│  agentflowd-{pid}.log ← (可选) 多 daemon 实例场景                 │
│                                                                  │
│  统一格式: [2026-07-04 11:26:33.408] [INFO] [DAEMON] 消息         │
│            [2026-07-04 11:26:33.408] [WARN] [llm_call] 消息       │
│            [2026-07-04 11:26:33.408] [INFO] [bridge.chat] 消息    │
└──────────────────────────────────────────────────────────────────┘

┌─ daemon 进程 ────────────────────────────────────────────────────┐
│                                                                  │
│  ┌─────────────────────┐      ┌──────────────────────────────┐   │
│  │ C++ Logger           │      │ 嵌入 Python                  │   │
│  │ Logger::instance()   │      │ coordinator_decide.py        │   │
│  │ → stdout             │      │ skill_executor.py            │   │
│  │ → --log-file FILE    │      │ llm_call.py / databus_client │   │
│  └─────────┬───────────┘      │ → sys.stderr                  │   │
│            │                  └──────────────┬───────────────┘   │
│            │  daemon 内部                      │                  │
│            ▼ 重定向 stderr → stdout            │                  │
│  ┌─────────────────────────────────────────────▼──────────────┐  │
│  │           2>&1 (daemon 启动时)                              │  │
│  │           → agentflowd.log                                 │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘

┌─ bridge 进程 ────────────────────────────────────────────────────┐
│                                                                  │
│  Python logging.getLogger("agentflow.bridge")                    │
│  → stdout → bridge.log (shell 重定向)                             │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 可观测性分层（当前实现事实）

当前系统中的"日志"实际包含三个语义层，每层的用途、格式和消费者完全不同，**不应混为一谈**。

| 层 | 名称 | 内容 | 格式 | 消费者 | 存储 |
|----|------|------|------|--------|------|
| **L1** | **系统日志** | 进程启停、配置加载、连接状态、异常堆栈 | 自由文本 | 运维排障 | 日志文件 `logs/*.log` |
| **L2** | **业务可观测性事件** | 用户行为、API 调用、业务流程关键节点 | 结构化 `topic.op key=val key=val` | 监控告警、用量统计、审计 | 日志文件（可被日志系统消费） |
| **L3** | **LLM 调用 Trace** | 每次 LLM 调用的入参、出参、延迟、Token 用量 | JSON 结构化对象 | Web UI 展示、评测、调优 | DataBus `llm/traces/`（内存 KV，REST API 查询） |

#### L1 — 系统日志

自由文本，可读性优先，面向人。

| 来源 | 实现 | 示例 |
|------|------|------|
| Daemon C++ `Logger` | `Logger::instance().info(...)` | `[2026-07-04 11:26:33.408] [INFO] [DAEMON] Agent started: outfit-assistant` |
| Bridge `agentflow.bridge.*` | `logging.getLogger(...)` | `2026-07-04 11:29:38,353 [INFO] agentflow.bridge.routes.chat: connection open` |
| 节点脚本 `_log_warn` | `print(msg, file=sys.stderr)` | `[llm_call] WARN: put 'traces/...' failed: ...` |

#### L2 — 业务可观测性事件

结构化 key=value 格式，**机器可解析**，面向监控和自动化分析。这是当前系统最成熟的可观测性层。

**命名规范**：

```
{topic}.{operation} {key}={value} [{key}={value} ...]
```

topic 按领域划分：

| topic | 操作示例 | 定义位置 |
|-------|---------|----------|
| `chat_turn` | `start`、`complete`、`connect`、`disconnect` | `routes/chat.py` |
| `session_mgmt` | `list`、`destroy`、`clear_dialogue` | `routes/sessions.py` |
| `agent_lifecycle` | `list`、`load`、`unload`、`pause`、`resume` | `routes/agents.py` |
| `daemon_connect` | `connected`、`closed` | `client.py` |
| `daemon_health` | *(由 poll loop 产生)* | `main.py` |
| `trace_query` | `list_keys`、`get` | `routes/llm_trace.py` |

**示例**：

```
chat_turn.start    agent=outfit-assistant user=web-user client=client_mr5sshd2zp882x content_len=19
chat_turn.complete agent=outfit-assistant dialogue=0704032343-9de5941766b4 task=msg_3_1047152985 elapsed=0.3s chunks=1 final=True
session_mgmt.list  agent=outfit-assistant user=web-user ok
agent_lifecycle.list ok count=0
trace_query.list_keys agent=outfit-assistant user=web-user error=TimeoutError
```

**实现方式**：通过标准 Python `logging` 模块的 `logger.info()` / `logger.warning()` 输出，但消息体采用固定 key=value 格式。没有独立的 SDK 或管道。

**优势**：
- 零依赖 — 不需要额外的可观测性基础设施
- 兼容日志文件聚合工具（ELK、Loki、Datadog 均可通过 `{topic}.{op}` 正则过滤）
- 当前 bridge.log 中已积累大量此类事件，可直接用于用量统计、错误率监控

**劣势**：
- 与系统日志混在同一文件、同一级别，无法独立路由
- 不支持结构化字段值嵌套（如 JSON 对象）
- 没有语义 Schema 定义（哪些字段是必填／可选）

#### L3 — LLM 调用 Trace

完全独立于日志系统的**调用链可观测性**。详见 `architecture/modules/llm-trace.md`。

| 属性 | 说明 |
|------|------|
| 写入端 | `_TraceRecorder.trace_call()` |
| 存储 | DataBus `llm/traces/{call_id}/{request\|response\|meta}` |
| 读取端 | bridge REST `GET /api/agents/{agent_id}/llm-traces` |
| 数据格式 | JSON 结构化（完整 HTTP request/response body） |
| 生命周期 | DataBus 内存 KV，进程重启即丢失 |

不纳入本日志系统设计范围，仅在此标识边界。

#### 分层关系图

```
┌─────────────────────────────────────────────────────────────┐
│                    logs/ 目录                                  │
│                                                             │
│  agentflowd.log  ←  L1 系统日志（C++ Logger + 节点 stderr）   │
│                      L2 业务事件（节点 _log_warn 中部分）      │
│                                                             │
│  bridge.log      ←  L1 系统日志（Python logging）             │
│                      L2 业务事件（chat_turn / session_mgmt）   │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│                    DataBus                                    │
│                                                             │
│  llm/traces/     ←  L3 LLM Trace（独立于日志文件系统）         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

> **关键结论**：L2 业务可观测性事件是当前最有价值的可观测性数据，但它**与 L1 系统日志共用同一条管道**（同一 logger、同一文件、同一级别）。未来的日志系统改进应优先解决"将 L2 事件与 L1 噪音分离"的问题，而不是仅仅统一 stderr 路径。

#### 方案 A（推荐）：daemon 启动时统一捕获 stderr

**改动最小，立即见效**。

Daemon 启动命令将 stderr 合并到 stdout：

```bash
# 之前（stderr 丢失）
agentflowd --package ... --listen ... | tee logs/agentflowd.log

# 之后（stderr 也落地）
agentflowd --package ... --listen ... 2>&1 | tee logs/agentflowd.log
```

**优点**：
- 零代码改动
- Python 节点的 `print(stderr)` 立即生效
- 兼容 daemon 自带的 `--log-file`（stderr 仍走原始 fd）

**缺点**：
- 多 daemon 实例仍共享同一文件
- 格式仍不统一（C++ 结构化日志 vs Python `print` 自由文本）

**配套改进**：
- 启动脚本模板中加入 `2>&1`
- 在 `start.sh` 或 README 启动命令中标准化

#### 方案 B（推荐）：Python 节点改为 Python logging + daemon stderr 重定向

**中等改动，格式统一**。

1. 节点脚本的 `_log_warn` 从 `print(stderr)` 改为 `logging.getLogger("agentflow.node")`

```python
# llm_call.py
import logging
_logger = logging.getLogger("agentflow.node.llm_call")

def _log_warn(msg: str) -> None:
    _logger.warning(msg)
```

2. Daemon 启动时设置环境变量 `PYTHON_LOG_CFG` 或由引导脚本配置 logging

```python
# daemon 嵌入 Python 初始化时
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
```

3. Daemon 启动时依然需要 `2>&1` 使 Python logging 的输出落地

**优点**：
- 格式统一（时间戳 + 级别 + 模块名 + 消息）
- 支持日志级别（debug/info/warning/error）
- 命名空间与 bridge 一致（`agentflow.*`）

**缺点**：
- 需改 `llm_call.py`、`databus_client.py`
- 需要确认 daemon 嵌入 Python 的初始化时机

#### 方案 C（远期）：daemon 原生捕获嵌入 Python stderr → C++ Logger

**最大改动，彻底统一**。

在 daemon C++ 初始化嵌入 Python 时，将 Python 的 `sys.stderr` 重定向到 C++ Logger 的写文件 fd。

```cpp
// daemon 初始化嵌入 Python 时
void redirect_python_stderr() {
    FILE* log_fp = fopen(config_.log_file.c_str(), "a");
    if (log_fp) {
        PySys_SetObject("stderr", PyFile_FromFd(fileno(log_fp), ...));
    }
}
```

**优点**：
- Python 日志与 C++ 日志写入同一个 fd，天然交错正确
- 不需要 shell 层 `2>&1` 技巧
- 按 pid 区分多实例（daemon 自带 log_file 可含 pid）

**缺点**：
- 改 daemon C++，需重新编译
- Python `print` 可能被缓冲

### 实施方案

**Phase 1（建议本周）**：方案 A + 启动脚本标准化

- [ ] 更新启动命令文档/脚本，统一使用 `2>&1 | tee`
- [ ] 清理 `/tmp/af_*.log` 遗留测试文件
- [ ] 在 `agentflowd.log` 头部写入 daemon PID 和启动时间

**Phase 2（建议下周）**：方案 B + 日志轮转

- [ ] 节点脚本 `_log_warn` 改为 `logging.getLogger`
- [ ] 统一格式、增加级别控制
- [ ] 接入 `logrotate` 或 Python `TimedRotatingFileHandler`

**Phase 3（按需）**：方案 C

- [ ] daemon C++ 嵌入 Python stderr 重定向
- [ ] 按进程 PID 自动分日志文件

---

## 当前实现事实

> 以下描述实际代码现状，与设计方案可能不一致。

### Daemon C++ Logger

| 项目 | 值 |
|------|----|
| 类 | `Logger`（单例） |
| 输出 | `--log-file FILE` 或 stdout（默认 stdout） |
| 级别 | `--log-level` / `AF_LOG_LEVEL` 环境变量 |
| 格式 | `[2026-07-04 11:26:33.408] [INFO] [TAG] message` |
| 源码 | `src/daemon/` |

### Bridge Python Logging

| 项目 | 值 |
|------|----|
| 配置 | `main.py:logging.basicConfig(level=logging.INFO, format=...)` |
| 输出 | stdout（shell 重定向到 `logs/bridge.log`） |
| 格式 | `2026-07-04 11:11:04,858 [INFO] agentflow.bridge: message` |

### Agent 节点 Python 脚本

| 项目 | 值 |
|------|----|
| 函数 | `_log_warn(msg)` — `print(msg, file=sys.stderr)` |
| 输出 | stderr（当前不入文件，仅终端可见） |
| 格式 | `[llm_call] WARN: message` |
| 涉及文件 | `llm_call.py`、`databus_client.py` |

### TUI Agent 应用

| 项目 | 值 |
|------|----|
| 路径 | `examples/agent/*/runtime/logs/*.log` |
| 格式 | 各自独立，无统一规范 |

---

## 设计决策

### 决策 1：日志目录统一为 `logs/`

- **选项**：`logs/` vs `var/log/` vs 系统日志
- **选择**：`logs/`（项目根下，与当前一致）
- **理由**：开发模式方便查找，生产部署可 symlink 到 `var/log/`

### 决策 2：不使用 Python logging 直接写文件（Phase 1）

- **选项**：节点脚本直接 `FileHandler` vs 通过 stderr 合并
- **选择**：stderr 合并（方案 A）
- **理由**：零代码改动，daemon 重启即生效。Python logging 直接写文件会与 daemon C++ Logger 产生两个独立的写 fd，导致日志行交错更严重

### 决策 3：节点脚本日志格式与 bridge 对齐

- **选项**：`agentflow.node.llm_call` vs 自定义标签
- **选择**：`agentflow.node.<module_name>`
- **理由**：与 bridge 的 `agentflow.bridge.*` 命名空间一致，日志分析工具可以按 `agentflow.*` 前缀统一过滤

---

## 关联文档

| 文档 | 说明 |
|------|------|
| `features/daemon-monitoring.md` | Daemon 健康监控（另一可观测性维度） |
| `architecture/modules/llm-trace.md` | LLM 执行轨迹（业务层 trace，与系统日志不同） |
| `../../../docs/usage/demo_guide.md` §5 | 运维启动方式 |
