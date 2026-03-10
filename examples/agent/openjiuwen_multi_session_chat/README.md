# openjiuwen_multi_session_chat

基于 AgentFlow + Python 节点的多轮对话 Agent 示例，功能目标对齐 `examples/agent/multi_session_chat`，但所有 workflow node 都使用 `python_script`，并通过示例内的 openJiuwen 兼容层优先对接真实 `openjiuwen` 能力。当前交互入口为 **TUI**，不再提供 WebUI/HTTP/WebSocket 前端。

## 目录结构

```text
openjiuwen_multi_session_chat/
  package/       # Agent 包（agent.json、package.json、workflow、prompt、runtime）
  nodes/python/  # 全部 workflow node 与 openJiuwen 兼容层
  scripts/       # 构建与启动脚本
  tests/         # 自动化对话测试
  openjiuwen_multi_session_chat_tui.cpp
```

## 设计目标

- 对外行为保持与 `multi_session_chat` 一致：多轮对话、会话池、dispatcher/coordinator/skill 编排、历史总结与能力拒答。
- 对内实现切换为 Python-only nodes，便于承接 openJiuwen 的 Workflow/Skill/Context/LLM 抽象。
- 保持对 AgentFlow runtime、worker、DataBus 的兼容，不引入新的运行时要求。
- TUI 将运行时日志重定向到 `package/runtime/logs/openjiuwen_multi_session_chat_tui.log`，保证交互输出干净。

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
  - 统一历史读取、上下文总结、会话 context 写回，并为 TUI 提供稳定的纯会话直答语义。

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

## 启动

```bash
bash examples/agent/openjiuwen_multi_session_chat/scripts/build_and_run.sh
```

## LLM 配置

```bash
export AF_LLM_BASE_URL="https://api.openai.com/v1"
export AF_LLM_API_KEY="sk-..."
export AF_LLM_MODEL="gpt-4o-mini"
```

未配置时示例仍可运行，使用规则化/回退逻辑；配置且安装 `openjiuwen` 后，节点会优先尝试其桥接能力。

## 自动化测试

```bash
bash examples/agent/openjiuwen_multi_session_chat/tests/auto_dialog_test.sh
```

## 详细文档

见 [docs/openjiuwen-agentflow-example.md](../../../docs/openjiuwen-agentflow-example.md)。
