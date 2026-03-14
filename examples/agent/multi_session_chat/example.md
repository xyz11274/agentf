# Multi-Session Chat Example (AgentFlow)

> 如果你需要查看 **Python-only + openJiuwen 兼容层** 的版本，请同时参考
> [`docs/openjiuwen-agentflow-example.md`](./openjiuwen-agentflow-example.md)
> 与 `examples/agent/openjiuwen_multi_session_chat/`。

## 目录
- 根目录：`examples/agent/multi_session_chat/`
- TUI 入口：`examples/agent/multi_session_chat/multi_session_chat_tui.cpp`
- Agent 包：`examples/agent/multi_session_chat/package/`
- 多语言节点：`examples/agent/multi_session_chat/nodes/`
- 自动化测试：`examples/agent/multi_session_chat/tests/`

## 示例目标

该示例用于演示：

1. 同质化 session 池与 `SessionRouter`
2. `dispatcher/coordinator/skill/contextupdater` 任务模型
3. workflow typed `input/output` 与 `value/ref` 传递
4. DataBus 的 Set/Get 与 Pub/Sub
5. Shell / Go / Python / Java 多语言节点协同

## Agent 包配置

### session 池

- `agent.json` 只保留 1 份 session 模板；
- `session_pool_size` 决定池中可复用 session 数量；
- 每个 session 模板固定包含：
  - 1 个 `dispatcher`
  - 1 个 `coordinator`
  - 多个 `skill`
- 每个 task 还可以通过 `workflow_pool_size` 声明绑定 workflow 的实例池大小；runtime 会在注册/启动时为这些 task 预创建 workflow 实例池，并交给 deployer/scheduler 管理；
- `contextupdater` 作为 agent 顶层任务单独配置，不属于 session 内任务。

### workflow 映射

`examples/agent/multi_session_chat/package/package.json` 中的核心映射如下：

| 任务/用途 | workflow |
|---|---|
| `dispatcher` | `session-dispatcher-workflow` |
| `coordinator` | `session-coordinator-workflow` |
| `contextupdater` | `session-context-workflow` |
| `skill:search` | `skill-search-workflow` |
| `skill:summary` | `skill-summary-workflow` |
| `skill:date` | `skill-date-workflow` |
| `skill:delegate` | `skill-delegate-workflow` |

dispatcher / coordinator 仍是特殊 task：

- dispatcher workflow 输入意图消息，输出 `selected_skills`；
- coordinator workflow 基于 `selected_skills` 和已有结果生成编排；
- agent 在读取 workflow 输出后，会把被选中的 skill 实例化成 runtime task，再由 scheduler 绑定到具体 workflow 实例。

## Workflow 架构

### 1. session-dispatcher-workflow

该 workflow 负责把用户请求转换为排好序、带执行状态的 skill 清单。

节点：

| 节点 | 作用 |
|---|---|
| `load_context` | 从 DataBus 加载 `session:<sid>:context` |
| `build_catalog` | 生成可供选择的 skill catalog |
| `semantic_path` | 从语义角度挑选 skills |
| `temporal_path` | 从时间/时效角度挑选 skills |
| `planner_path` | 从任务规划角度挑选 skills |
| `merge_candidates` | 合并 3 条路径结果，输出 `selected_skills` 与 `dispatcher_done` |

特点：

- 3 条选择路径并行执行；
- 每条路径都会读取：
  - `session_id`
  - `user_text`
  - `session_context`
  - `skill_catalog`
- 路径内部通过 LLM + 规则组合，返回 `0..N` 个候选 skill；
- `merge_candidates` 输出的 `selected_skills` 为 JSON 字符串，包含顺序、状态、原因等信息。

### 2. session-coordinator-workflow

该 workflow 负责根据 dispatcher 输出和部分 skill 结果，重新编排最终执行计划。

节点：

| 节点 | 作用 |
|---|---|
| `load_context` | 读取 session context |
| `build_catalog` | 构造 skill catalog |
| `result_aware_path` | 结合已有 skill 结果做重排 |
| `dependency_path` | 结合依赖关系做重排 |
| `finalizer_path` | 判断是否已具备直接回复条件 |
| `merge_plan` | 合并结果，输出 `orchestration_plan`、`selected_skills`、`final_response` |

输入：

- `selected_skills`
- `partial_skill_results`
- `user_text`
- `session_context`
- `skill_catalog`

输出：

- `orchestration_plan`
- `selected_skills`
- `final_response`
- `coordinator_done`

### 3. skill workflows

| Workflow | 语言 | 输出 |
|---|---|---|
| `skill-search-workflow` | Shell | `search_result` |
| `skill-summary-workflow` | Shell | `summary_result` |
| `skill-date-workflow` | Python | `date_result` |
| `skill-delegate-workflow` | Java | `delegate_result` |

skill workflow 统一通过 typed input/output 与 DataBus 交互，不再依赖额外的运行时中间文件。

补充约束：

- skill 输出除了业务结果，还会携带 `can_handle`、`status`、`grounded`、`confidence`、`reason` 等能力字段；
- 当输入超出 skill 边界时，skill 必须返回拒答/不支持状态，而不是生成泛化兜底文本；
- coordinator 只合并通过能力校验的 skill 结果，避免把无关 skill 的输出拼成错误答案。
- 对于问候语、以及“总结上一条消息”这类可直接由会话历史回答的纯对话意图，TUI 会在进入 skill 编排前直接返回结果，避免把历史总结错误地下发给不匹配的 skill。

### 4. session-context-workflow

`session-context-workflow` 使用 Go 节点生成 session 级 context，典型链路为：

1. 读取 prompt 模板
2. 读取对话历史
3. 调用 LLM/摘要逻辑
4. 写回 `session:<sid>:context`

## DataBus 集成

### Set/Get

示例中常用的 DataBus 键：

| 键名 | 说明 |
|---|---|
| `user_text` | 当前用户请求 |
| `input` | 原始 payload（session/user/history） |
| `session:<sid>:history_json` | 最近历史消息 |
| `session:<sid>:context` | session 级上下文 |
| `selected_skills` | dispatcher/coordinator 当前 skill 清单 |
| `partial_skill_results` | 已执行 skill 的中间结果 |
| `orchestration_plan` | coordinator 编排计划 |
| `final_response` | 最终回复 |
| `session:<sid>:skill_result` | 单个 session 的最近 skill 结果 |

### Pub/Sub

Shell 节点通过 `publish/sdk/shell/agentflow_databus.sh` 使用 Pub/Sub：

1. 发布 `agentflow.llm.request`
2. worker/runtime 连接推理服务并流式写回
3. 节点订阅：
   - `agentflow.llm.chunk.<request_id>`
   - `agentflow.llm.done.<request_id>`
   - `agentflow.llm.error.<request_id>`

当前示例中的 `select_skill_path.sh` 即采用该机制完成 dispatcher/coordinator 各路径的 LLM 选择。

## TUI 交互流程

`examples/agent/multi_session_chat/multi_session_chat_tui.cpp` 中的桥接逻辑如下：

1. TUI 先将进程级 stdout/stderr 重定向到 `package/runtime/logs/multi_session_chat_tui.log`
2. 交互窗口仅保留 `you>` / `assistant>` / `status>` 输出
3. 对问候语与“总结上一条消息”执行本地短路回复
4. 其余请求通过 `ChatAgentBridge` 触发 `dispatcher`
5. AgentFlow runtime 继续调度 `session-coordinator-workflow` 与 skill workflows
6. TUI 只收集最终响应文本，不把运行时日志打印回交互窗口

这条链路继续走 AgentFlow DataBus 与运行时调度，但不再暴露 WebUI/HTTP/WebSocket 前端。

## 多语言节点与 SDK

| 语言 | SDK/桥接 |
|---|---|
| Shell | `publish/sdk/shell/agentflow_databus.sh` |
| Python | `publish/sdk/python/agentflow_databus.py` |
| Go | `publish/sdk/go/agentflow_databus.go` |
| Java | `publish/sdk/java/AgentFlowDataBus.java` |
| C++ | `include/external/runtime/databus_sdk.h` |

## LLM 配置

支持 OpenAI 兼容接口：

```bash
export AF_LLM_BASE_URL="https://api.openai.com/v1"
export AF_LLM_API_KEY="sk-..."
export AF_LLM_MODEL="gpt-4o-mini"
```

未配置时示例仍会运行，但使用 mock/规则化结果。

## 构建与运行

### 一键启动
```bash
bash examples/agent/multi_session_chat/scripts/build_and_run.sh
```

### 手动运行
```bash
./build.sh build
bash examples/agent/multi_session_chat/nodes/java/build.sh
./build/example_multi_session_chat_tui examples/agent/multi_session_chat/package
```

## 自动化测试

```bash
bash examples/agent/multi_session_chat/tests/auto_dialog_test.sh
```

## 说明

- `session_dispatcher_workflow.json` 与 `session_coordinator_workflow.json` 已经改为 shell + DataBus 驱动；
- `default_dispatcher_workflow.json` 及对应旧脚本已删除；
- 示例优先展示新的 workflow input/output、DataBus Set/Get、DataBus Pub/Sub 能力。
