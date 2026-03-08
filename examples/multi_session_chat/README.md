# multi_session_chat

多轮对话 Agent 示例，展示 AgentFlow 的同质化 session 池、多语言节点、typed workflow I/O、DataBus Set/Get，以及基于 Pub/Sub 的 LLM 编排。

## 目录结构
```text
multi_session_chat/
  server/        # 独立 C++ 进程（HTTP + WebSocket + 对话管理）
  package/       # Agent 包（agent.json、workflow、prompt、shell 脚本）
  nodes/
    go/          # contextupdater workflow
    python/      # skill-date-workflow
    java/        # skill-delegate-workflow
  scripts/       # 构建与运行脚本
  tests/         # 自动化对话测试
```

## Agent 包结构

- `agent.json` 使用 `session_pool_size + sessions + tasks`：
  - 顶层 `tasks[0]` 为 `contextupdater`；
  - `sessions[0]` 是唯一的同质化 session 模板；
  - 每个 session 固定包含 1 个 `dispatcher`、1 个 `coordinator` 和多个 `skill` 任务；
  - 每个 task 还可以设置 `workflow_pool_size`，声明该任务绑定 workflow 的实例池规模；
  - 每个 `skill` 任务通过 `instance_pool_size` 配置实例池，默认 `2`。
- `package.json` 将任务类型映射到 workflow：
  - `dispatcher -> session-dispatcher-workflow`
  - `coordinator -> session-coordinator-workflow`
  - `contextupdater -> session-context-workflow`
  - `skill:* -> 对应 skill workflow`

## 关键 workflow

| Workflow | 关键节点 | 语言/模式 |
|---|---|---|
| `session-dispatcher-workflow` | `load_context` / `build_catalog` / `semantic_path` / `temporal_path` / `planner_path` / `merge_candidates` | Shell |
| `session-coordinator-workflow` | `load_context` / `build_catalog` / `result_aware_path` / `dependency_path` / `finalizer_path` / `merge_plan` | Shell |
| `session-context-workflow` | `read_prompt` / `read_history` / `summarize` / `fill_context` | Go |
| `skill-search-workflow` | `online-search` | Shell |
| `skill-summary-workflow` | `result-summary` | Shell |
| `skill-date-workflow` | `date-calc` | Python |
| `skill-delegate-workflow` | `agent-delegate` | Java |

## 当前编排语义

1. `dispatcher` 接收用户意图请求，结合 session context 和 skill catalog，并行走 3 条 LLM 选择路径。
2. `merge_candidates` 汇总 3 条路径的输出，生成排好序且带执行状态的 `selected_skills`。
3. `coordinator` 接收 `selected_skills`、原始请求、session context、部分 skill 结果，再并行走 3 条重排路径。
4. `merge_plan` 生成新的 `orchestration_plan`，同时在结果充分时输出 `final_response`。
5. skill workflow 通过 typed input/output 与 DataBus 交互，不再依赖额外运行时文件。

补充：

- `chat_server` 会先处理一小类无需编排的对话意图：问候语、以及“总结上一条消息”这类可直接从会话历史回答的请求；
- 这类请求会直接读取 `history_json` 生成回复，避免把纯会话意图误路由到 skill，属于对“能力边界”和“路由对齐”的前置保护；
- 其余请求仍进入 dispatcher/coordinator/skill 流程。

### 能力契约与拒答

- skill 选择不再只看关键词，还会结合 skill catalog 中的 `handles / does_not_handle / abstain_when` 约束。
- skill 执行结果会附带 `can_handle / status / grounded / confidence / reason` 等字段。
- 超出能力边界时，skill 必须显式拒答，而不是返回看似正常的兜底文本。
- coordinator 只会合并经过能力校验的结果；若没有有效结果，会回退到安全提示，而不是拼接错误结论。

## DataBus 与 LLM

- Shell 节点通过 `publish/sdk/shell/agentflow_databus.sh` 调用：
  - `af_databus_set`
  - `af_databus_get`
  - `af_databus_publish`
  - `af_databus_next_event`
  - `af_databus_request_llm`
- `select_skill_path.sh` 使用 Pub/Sub 发送 `agentflow.llm.request`，等待 `agentflow.llm.done.<id>` 返回结果。
- `load_session_context.sh`、`skill_search.sh`、`skill_summary.sh` 均通过 DataBus Set/Get 传递数据。

## 快速运行

### 前置依赖
- C++ 工具链（CMake + clang/gcc）
- Go 1.18+
- Python 3.8+
- Java 11+（可选，用于 delegate skill）

### 一键构建启动
```bash
bash examples/multi_session_chat/scripts/build_and_run.sh
```

打开 `http://127.0.0.1:8088/`。

## LLM 配置
```bash
export AF_LLM_BASE_URL="https://api.openai.com/v1"
export AF_LLM_API_KEY="sk-..."
export AF_LLM_MODEL="gpt-4o-mini"
```

未设置时示例仍可运行，但会退化为 mock/规则化结果。

## 自动化测试
```bash
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```

## 详细文档
见 [docs/multi-session-chat-example.md](../../docs/multi-session-chat-example.md)
