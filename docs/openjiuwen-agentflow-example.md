# openJiuwen × AgentFlow 多轮对话示例

## 目标

`examples/agent/openjiuwen_multi_session_chat` 是一个新的 Agent 示例，目标是：

1. 对外复刻 `examples/agent/multi_session_chat` 的多轮对话能力；
2. 对内将全部 workflow node 统一为 Python 实现；
3. 在不破坏 AgentFlow runtime / worker / DataBus 契约的前提下，尽可能复用 openJiuwen 的 Workflow、Skill、Context、LLM 抽象；
4. 在 `openjiuwen` 缺失时仍能完成仓库内构建、运行和自动化测试。

## 目录

```text
examples/agent/openjiuwen_multi_session_chat/
  package/
    agent.json
    package.json
    *.json
    prompts/
  nodes/python/
    compat/
      runtime_bridge.py
      workflow_adapter.py
      skill_adapter.py
      context_adapter.py
    *.py
  server/webui/
  scripts/build_and_run.sh
  tests/auto_dialog_test.sh
```

## 架构映射

### AgentFlow 侧

- `agent.json` 继续使用：
  - `session_pool_size`
  - `sessions`
  - `tasks`
  - `workflow_pool_size`
- dispatcher / coordinator 仍是特殊 task：
  - dispatcher workflow 输出 `selected_skills`
  - coordinator workflow 输出 `orchestration_plan` / `final_response`
  - AgentFlow runtime 再将 skill workflow 作为 task 实例调度执行

### openJiuwen 侧

示例没有把 AgentFlow 替换成 openJiuwen 运行时，而是通过 Python 兼容层做桥接：

| 兼容层 | 作用 |
|---|---|
| `runtime_bridge.py` | 探测 `openjiuwen`、封装 DataBus、读取 LLM 配置 |
| `workflow_adapter.py` | 优先创建/调用 openJiuwen 风格的 `WorkflowCard` / `Workflow` / `WorkflowAgent` / `LLMComponent` |
| `skill_adapter.py` | 提供 skill catalog、能力契约、dispatcher/coordinator 规则、skill 回退逻辑 |
| `context_adapter.py` | 提供历史读取、总结、context 写回与纯会话短路逻辑 |

## “真正对接 openJiuwen” 的含义

示例适配层不是空壳：

- 仓库会先把 `vendor/openjiuwen/python` 加入 Python 路径，再尝试导入：
  - `openjiuwen.core.workflow`
  - `openjiuwen.core.foundation.llm`
  - `openjiuwen.core.runner.runner`
  - `openjiuwen.core.single_agent.legacy`
  - `openjiuwen.core.application.workflow_agent`
- 若本机额外安装了上游 `openjiuwen`，也可继续用于覆盖 vendored 兼容层联调；
- 对于技能选择和上下文总结这类需要模型能力的节点，适配层会优先尝试构建 openJiuwen 风格的最小 workflow/agent 调用链；
- 在真实 openJiuwen 调用前后，适配层会把 request、workflow binding、response、lifecycle 事件同步回 AgentFlow DataBus；
- 若依赖缺失、配置不完整或调用失败，再显式回退到 AgentFlow DataBus 的 LLM 请求通道或规则化实现。

因此：

- **有 vendored / 上游 openJiuwen 时**：节点能利用其 workflow / LLM / agent 抽象；
- **无 LLM 凭证时**：仍能保证示例在本仓库内可验证、可测试。

## Workflow 设计

### 1. Dispatcher workflow

`session-dispatcher-workflow` 中的所有节点均为 `python_script`：

- `load_context`
- `build_catalog`
- `semantic_path`
- `temporal_path`
- `planner_path`
- `merge_candidates`

其中 `select_skill_path_node.py` 会：

1. 读取 AgentFlow DataBus 中的 `user_text` / `session_context` / `skill_catalog`
2. 调用 `workflow_adapter.py`
3. 优先尝试 openJiuwen 推理
4. 再由 `skill_adapter.py` 结合能力契约生成候选 skill 清单

### 2. Coordinator workflow

`session-coordinator-workflow` 同样全部为 Python 节点，使用相同的适配层完成：

- 基于部分 skill 结果重排
- 依赖分析
- 最终回复判定
- orchestration plan 合并

### 3. Context workflow

`session-context-workflow` 不再依赖 Go：

- `context_node.py read_prompt`
- `context_node.py read_history`
- `context_node.py summarize`
- `context_node.py fill_context`

其中 `summarize` 会优先尝试使用 openJiuwen 风格的 LLM 调用完成历史摘要。

### 4. Skill workflows

所有 skill workflow 都改为 Python：

| Workflow | Node | 说明 |
|---|---|---|
| `skill-search-workflow` | `skill_search_node.py` | 搜索/检索建议 |
| `skill-summary-workflow` | `skill_summary_node.py` | 基于历史/中间结果做总结 |
| `skill-date-workflow` | `skill_date_node.py` | 日期计算 |
| `skill-delegate-workflow` | `skill_delegate_node.py` | 计划/步骤建议 |

## DataBus 对接

示例继续使用 AgentFlow DataBus 作为跨节点共享状态：

- `user_text`
- `selected_skills`
- `partial_skill_results`
- `orchestration_plan`
- `final_response`
- `session:<sid>:history_json`
- `session:<sid>:context`

适配层不会绕过 DataBus，而是把 openJiuwen 的上下文/工作流抽象映射回这些键，确保：

- workflow JSON 的 typed IO 保持可见；
- runtime / worker / scheduler / deployer 的现有执行语义不变；
- chat server 无需为 openJiuwen 单独增加一套状态协议。

### runtime / worker / workflow-instance 绑定

真实 openJiuwen 路径执行时，兼容层会消费 AgentFlow worker 注入的 `AF_*` 环境变量，包括：

- `AF_SESSION_ID`
- `AF_WORKFLOW_ID`
- `AF_WORKFLOW_INSTANCE_ID`
- `AF_WORKER_ID`
- `AF_TASK_ID`
- `AF_REQUEST_ID`
- `AF_USER_ID`
- `AF_CLIENT_ID`
- `AF_OUTPUT_FORMAT`

并将这些上下文重新绑定回 AgentFlow DataBus，形成以下几类桥接状态：

- `agentflow:openjiuwen:reasoner_request`
- `agentflow:openjiuwen:reasoner_databus_request`
- `agentflow:openjiuwen:openjiuwen_workflow_binding`
- `agentflow:openjiuwen:openjiuwen_runner_state`
- `agentflow:openjiuwen:last_response`
- `session:<sid>:openjiuwen:*`
- `workflow_instance:<workflow_instance_id>:openjiuwen:*`
- `worker:<worker_id>:openjiuwen:*`

同时还会发布生命周期事件：

- `agentflow.openjiuwen.reasoner_started`
- `agentflow.openjiuwen.reasoner_completed`
- `agentflow.openjiuwen.reasoner_fallback`

这样即便真实执行逻辑进入 openJiuwen：

- **执行入口**仍然来自 AgentFlow worker；
- **执行归属**仍然保留 AgentFlow 的 workflow instance / worker 元数据；
- **共享状态和审计信息**仍然写回 AgentFlow DataBus，而不是脱离现有运行时机制。

## 运行与测试

### 启动

```bash
bash examples/agent/openjiuwen_multi_session_chat/scripts/build_and_run.sh
```

### 自动化验证

```bash
bash examples/agent/openjiuwen_multi_session_chat/tests/auto_dialog_test.sh
```

该脚本验证：

- greeting 不被错误路由到 skill
- “总结上一条消息”能从会话历史得到正确回复
- 不支持的天气问题不会被日期 skill 或错误聚合结果污染
- 历史消息会正确落盘

该示例对应的仓库级 CTest 名称为 `E2EOpenJiuwenMultiSessionChat`，可通过
`ctest --test-dir build -R E2EOpenJiuwenMultiSessionChat --output-on-failure`
单独执行。

## 与原 `multi_session_chat` 的关系

- 原示例：强调 AgentFlow 的混合语言节点编排能力；
- 新示例：强调 Python-only node + openJiuwen 集成能力；
- 两者共享相同的运行时理念与对话行为目标，但新示例更适合作为 openJiuwen × AgentFlow 的集成参考实现。
