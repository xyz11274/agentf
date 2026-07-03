# Gap 02: LLM 配置通道缺失

## 状态
❌ **未实现**

## 问题描述

`DaemonConfig` 结构体中没有 `llm_config` 字段，`daemon_config.json` 即使写了 LLM 配置也读不进去。当前 LLM 配置只能通过**环境变量**（`AF_LLM_BASE_URL` / `AF_LLM_API_KEY` / `AF_LLM_MODEL`）手动 export，不走统一配置管理。

## 代码位置

**缺失字段**：
- `include/internal/daemon/config.h:19-54` — `DaemonConfig` 结构体，没有 `llm_config`

**缺失解析**：
- `src/daemon/config.cpp:188-310` — `load_config_file()` 函数，没有解析 `llm_config` 段

## 当前链路

```
用户手动 export 环境变量
    ↓
AF_LLM_BASE_URL / AF_LLM_API_KEY / AF_LLM_MODEL
    ↓
Worker 启动 Node 时通过 exec_config.env_vars 注入（worker.cpp:57-65）
    ↓
Node 进程通过 os.environ / $ENV 读取
```

## 目标链路（方案中规划）

```
daemon_config.json 中的 llm_config
    ↓
DaemonConfig.llm_config（新增字段）
    ↓
MMWorkerEngine::ExecutionRequest::llm_config
    ↓
Worker 注入环境变量
    ↓
Node 进程读取 + StreamingLLMClient 使用
```

## 影响

- 无法通过 `daemon_config.json` 统一管理 LLM 配置
- 每次启动 daemon 前必须手动 export 环境变量
- 无法在 `agenmd build` 生成的配置中包含 LLM 信息
- 多 agent 难以使用不同的 LLM 配置

## 建议方向

1. 在 `DaemonConfig` 中新增 `LlmConfig` 子结构体
2. 在 `load_config_file()` 中解析 `llm_config` JSON 段
3. 修改 rpc_handler 或 agent 加载路径，将配置传递到 `ExecutionRequest`

## 参考

- `include/internal/runtime/worker.h:66-71` — ExecutionRequest 中已有的 `llm_config` 结构
- `src/runtime/worker/worker.cpp:57-65` — Worker 注入 LLM 环境变量的逻辑（消费端已有）
- `docs/usage/plan/agent.md支持方案分析.md:170-190` — 方案中的映射设计
- `docs/usage/plan/1.方案和gap分析.md:720-727` — 方案中的 gap 分析
