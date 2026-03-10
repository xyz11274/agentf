# openjiuwen_multi_session_chat

基于 AgentFlow + Python 节点的多轮对话 Agent 示例，功能目标对齐 `examples/multi_session_chat`，但所有 workflow node 都使用 `python_script`，并通过示例内的 openJiuwen 兼容层优先对接真实 `openjiuwen` 能力。

## 目录结构

```text
openjiuwen_multi_session_chat/
  package/       # Agent 包（agent.json、package.json、workflow、prompt、runtime）
  nodes/python/  # 全部 workflow node 与 openJiuwen 兼容层
  server/webui/  # Web UI 静态资源（复用 shared chat server binary）
  scripts/       # 构建与启动脚本
  tests/         # 自动化对话测试
```

## 设计目标

- 对外行为保持与 `multi_session_chat` 一致：多轮对话、会话池、dispatcher/coordinator/skill 编排、历史总结与能力拒答。
- 对内实现切换为 Python-only nodes，便于承接 openJiuwen 的 Workflow/Skill/Context/LLM 抽象。
- 保持对 AgentFlow runtime、worker、DataBus 的兼容，不引入新的运行时要求。

## openJiuwen 集成策略

示例内置了 `nodes/python/compat/` 适配层：

- `runtime_bridge.py`
  - 负责 DataBus 访问、环境读取、openJiuwen 模块探测。
- `workflow_adapter.py`
  - 在检测到 `openjiuwen` 后，优先尝试使用其 `WorkflowCard` / `Workflow` / `LLMComponent` / `WorkflowAgent` 能力组织推理调用；
  - 调用前后会把 `workflow_instance_id` / `worker_id` / request / response / lifecycle 事件同步回 AgentFlow DataBus；
  - 未安装或调用失败时，回退到 AgentFlow DataBus 的 LLM 请求通道。
- `skill_adapter.py`
  - 统一 skill catalog、能力契约、dispatcher/coordinator 合并逻辑、各 skill 的回退实现。
- `context_adapter.py`
  - 统一历史读取、上下文总结、会话 context 写回。

这意味着：

- **安装了 `openjiuwen` 时**，节点会优先走 openJiuwen 风格的工作流/推理桥接路径；
- **未安装时**，示例仍然可以在仓库内直接构建、运行和测试。

## 关键 workflow

| Workflow | 关键节点 | 语言 |
|---|---|---|
| `session-dispatcher-workflow` | `load_context` / `build_catalog` / `semantic_path` / `temporal_path` / `planner_path` / `merge_candidates` | Python |
| `session-coordinator-workflow` | `load_context` / `build_catalog` / `result_aware_path` / `dependency_path` / `finalizer_path` / `merge_plan` | Python |
| `session-context-workflow` | `read_prompt` / `read_history` / `summarize` / `fill_context` | Python |
| `skill-search-workflow` | `online-search` | Python |
| `skill-summary-workflow` | `result-summary` | Python |
| `skill-date-workflow` | `date-calc` | Python |
| `skill-delegate-workflow` | `agent-delegate` | Python |

## 示例特点

- `agent.json` 延续 `session_pool_size + sessions + tasks` 结构；
- dispatcher/coordinator 仍是特殊 task，workflow 输出 `selected_skills` / `orchestration_plan` 后由 AgentFlow runtime 继续调度 skill workflow；
- 所有节点都通过 typed input/output 与 DataBus 交换数据；
- 即便存在真实 `openjiuwen` 代码，节点也不会绕开 AgentFlow runtime/worker：执行时会把 worker 注入的上下文重新绑定到 DataBus；
- 会话问候语、上一条消息总结等纯对话意图仍由外层 chat server 做前置短路，避免误触发 skill。

## 运行要求

- C++ 工具链（用于构建 AgentFlow 与 shared chat server）
- Python 3.11+（推荐，兼容 openJiuwen）
- 可选：`pip install -U openjiuwen`

## 启动

```bash
bash examples/openjiuwen_multi_session_chat/scripts/build_and_run.sh
```

打开 `http://127.0.0.1:8088/`。

## LLM 配置

```bash
export AF_LLM_BASE_URL="https://api.openai.com/v1"
export AF_LLM_API_KEY="sk-..."
export AF_LLM_MODEL="gpt-4o-mini"
```

未配置时示例仍可运行，使用规则化/回退逻辑；配置且安装 `openjiuwen` 后，节点会优先尝试其桥接能力。

## 自动化测试

```bash
bash examples/openjiuwen_multi_session_chat/tests/auto_dialog_test.sh
```

## 详细文档

见 [docs/openjiuwen-agentflow-example.md](../../docs/openjiuwen-agentflow-example.md)。
