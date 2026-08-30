# AgentFlow sessionlog：LLM/工具轨迹采集（部署与使用）

> 目标：一条从 dialogue 开始的完整轨迹线，融合 python_spans（生命周期）与 llm/traces（内容），
> 为轨迹树 / skill 编排 / 自进化（Early-Exit、Episode 重放）铺底座。
> 实现方式：**不改引擎代码**，靠 AgentFlow 自带的 hooks 系统（agent 包配置层）。

---

## 一、这是什么

sessionlog 通过 AgentFlow 原库的 hooks 机制（mounts/engine/registry/loader）接住
Coordinator、LLM、工具、Skill、Artifact 与 dialogue 生命周期事件；Bridge 另在最终响应交付边界
追加一条只读观测事件。每个 dialogue 只落一份实体文件，直接位于
`trace_output/sessionlogs/by-user/{user_id}/{client_id}/{dialogue_id}.jsonl`，
供人工浏览和后续建树发现。

```
16 种事件：
dialogue.start / coordinator.activation / loop.control / llm.request /
llm.transport.request / llm.response / tool.call / tool.result /
artifact.persist / skill.dispatch / batch.result / skill.start / skill.end /
messages.snapshot / dialogue.end / client.delivery

编排图（节点=trace_id，边=派发/回传）：
[A coordinator] ──派发边──▶ [B skill] ──回传边──▶ [C coordinator]（新子进程）
```

## 二、文件清单

| 文件 | 用途 | 部署位置 |
|---|---|---|
| `sessionlog_hook.py` | Harness 侧采集 + 落盘（hook 实现） | 复制到 agent 包 `harness/hooks/` |
| `sessionlog_reader.py` | 演示读取、完整性校验与编排图投影（不是建树实现） | 任意可执行位置（演示与验证用，不是建树实现） |
| `frontend/bridge/services/sessionlog_delivery.py` | Bridge 最终交付边界观测 | Bridge 内置，无需复制到 agent 包 |

## 三、部署步骤（到任意 agent 包）

```bash
# 1. 复制 hook 到 agent 包的 hooks 目录
mkdir -p <agent_package>/harness/hooks
cp docs/sessionlog/sessionlog_hook.py <agent_package>/harness/hooks/

# 2. 开启开关（agent 包 .env）
echo 'AF_SESSIONLOG_ENABLED=1' >> <agent_package>/.env

# 3. 验证 loader 能加载（可选）
cd <agentflow_root>
python3 -c "
import sys; sys.path.insert(0, 'packages/agentflow-runtime/src')
from agentflow_runtime.hooks.loader import load_hooks_from_dir
print(load_hooks_from_dir('<agent_package>/harness/hooks'))
# 应输出 ['sessionlog_recorder_v2']
"

# 4. 跑一个任务，检查唯一的层级文件
find <agentflow_root>/trace_output/sessionlogs/by-user -type f
```

> 仅复制 Hook 或修改 agent 包 `.env` 时，后续子进程会自动加载；若更新了 Bridge 的
> `client.delivery` 实现，则需重启 Bridge 进程使 Python 模块生效。

## 四、使用方法

### 4.1 查看一条完整轨迹

```bash
# 概要：校验 + 重放 + 编排图
python3 docs/sessionlog/sessionlog_reader.py <dialogue_id> [sessionlog_path]

# 示例
python3 docs/sessionlog/sessionlog_reader.py 4375731883344659352 \
    trace_output/sessionlogs/by-user/<user>/<client-id>/4375731883344659352.jsonl
```

输出三部分：
1. **校验**：按 producer 分组检查 seq 递增、LLM/Tool/Skill 生命周期配对与血缘引用
2. **重放**：按行序恢复每次 llm.request 的 messages（模型看到的一切）
3. **编排图**：节点=trace_id（coordinator/skill 执行单元），边=派发/回传

### 4.2 数据结构

**事件信封**（每行一个事件）：
```json
{"schema_version":2,"event_id":"python:668:1787707357000000:3",
 "idempotency_key":"python:668:1787707357000000:3","type":"llm.response",
 "semantic_kind":"llm","lifecycle_phase":"response","producer_sequence":3,
 "timestamp_us":1787707359000000,"producer_id":"python:668:1787707357000000",
 "trace_id":"py_local_668_1787707357","run_id":"...","dialogue_id":"...",
 "task_id":"...","node_id":"coordinator_decide","round":1,
 "primary_parent_event_id":"python:668:1787707357000000:2",
 "causal_parent_event_ids":["python:668:1787707357000000:2"],
 "data":{...}}
```

**主要事件 data**：

文件第一行是 `dialogue.header`，保存 `dialogue_id`、`user_id`、`client_id`、
`conversation_id`、`agent`、`model`、`code_revision` 和
`agent_definition_sha256`。前端和 Bridge 中的 `client_id` 就是逻辑会话标识，
因此采集层不再计算摘要：`conversation_id` 与原始 `client_id` 完全同值。
同一 `user_id + client_id` 下的多轮 dialogue 直接聚合在同一目录。
采集格式不保存运行时 Session 槽位，也不包含 `session_id/session_slot_id`。

采集目录同时呈现以下层级：

```text
trace_output/sessionlogs/
└── by-user/
    └── <safe-user-id>/
        └── <safe-client-id>/
            ├── <dialogue_id-1>.jsonl
            └── <dialogue_id-2>.jsonl
```

`by-user` 下的 JSONL 就是唯一写入源，不是符号链接，也没有根目录平铺副本；Bridge
定位同一文件并直接追加 `client.delivery`。Reader 只给 `dialogue_id` 时会在层级中发现
该文件，并继续兼容历史平铺文件和早期投影目录。目录中的用户分量会保留安全的中英文、
数字及 `-_.`，其他字符会替换并附加摘要，防止路径穿越；Header 中的原始
`user_id/client_id` 始终是权威值。会话目录使用 `client_id` 的安全目录分量；
身份缺失时分别落入 `unknown-user` 或 `unknown-conversation`，不影响 Harness 业务流。

| type | data 字段 | 触发挂载点 |
|---|---|---|
| `dialogue.start` | `{input, trigger_type, coordinator_version, code_revision, agent_definition_sha256, model, ...}` | AGENT_DIALOGUE_START（每个 dialogue 一次） |
| `coordinator.activation` | `{trigger_type, model, completed_skill, completed_batch_key, batch_id, activation_index, skill_completion_index, ...}` | COORDINATOR_TRIGGER（每次 v1 激活） |
| `loop.control` | `{scope, action, reason, attempt/index, limits, ...}` | Coordinator Tool 纠错与 Skill 语义恢复决策 |
| `llm.request` | `{skill_name, exec_skill_key, batch_id, task_type, model, messages(全量), msg_count, loop indices, ...}` | *_BEFORE_LLM_CALL（Harness 有效输入） |
| `llm.transport.request` | `{call_id, skill_name, exec_skill_key, batch_id, task_type, request(不含认证 Header 的精确 HTTP body), meta, ...}` | LLM_REQUEST_PREPARED（发送前） |
| `llm.response` | `{call_id, skill_name, exec_skill_key, batch_id, task_type, request, provider_response, response(Hook 后最终值), meta, loop indices, ...}` | COORDINATOR_LLM_RESULT / SKILL_EXECUTOR_LLM_RESULT |
| `tool.call` | `{skill_name, tool_names, tool_calls, round}` | SKILL_EXECUTOR_BEFORE_TOOL_CALL |
| `tool.result` | `{skill_name, tool_names, results_count, tool_results, duration_ms, round}` | SKILL_EXECUTOR_TOOL_RESULT |
| `artifact.persist` | `{status, skill_name, exec_skill_key, batch_id, artifact_routing_id, attempted_count, uploaded_count, entries[{source_path, storage_path, artifact_id, size_bytes, sha256, status}], error, ...}` | ARTIFACT_STORAGE_RESULT（逐 Artifact） |
| `skill.dispatch` | `{status, requested_skill_names, skill_names, round, batch_id, exec_skill_keys, rejected, warnings}` | COORDINATOR_AFTER_SKILL_DISPATCH |
| `batch.result` | `{completed_batch_key, batch_id, status(raw 业务聚合), workflow_status(results[].workflow_success → completed/failed/partial/unknown), successful_count, total_count, results(完整)}` | COORDINATOR_BATCH_RESULT |
| `skill.start` | `{skill_name, model, max_rounds, tools_count, exec_skill_key, skill/prompt/tool digests, ...}` | SKILL_EXECUTOR_TRIGGER |
| `skill.end` | `{skill_name, status, round(实际使用轮数), skill_result, error, exec_skill_key, batch_id, artifacts}` | SKILL_EXECUTOR_AFTER_SKILL_COMPLETE / 未处理异常 |
| `messages.snapshot` | `{messages, message_count}` | COORDINATOR_MESSAGES_SNAPSHOT（append-only） |
| `dialogue.end` | `{status, response_text, error_type/error(异常时), terminal_source}` | AGENT_DIALOGUE_END / AGENT_DIALOGUE_ERROR |
| `client.delivery` | `{transport, stage, status, raw_content, delivered_content, wire_payload, hashes, candidate_event_id, client_acknowledged=false, ...}` | Bridge REST/SSE 响应边界（server-side observation） |

正常模型调用中，`llm.request` 是 Recorder 分配 `call_id` 之前的 Harness 输入观察，
不会伪造尚不存在的 ID；后续 `llm.transport.request` 通过 `causal_parent_event_ids`
指向该观察，且与 `llm.response` 共享同一个非空 `call_id`。因此回放以
`llm.transport.request.request` 为实际发送事实，以 `llm.response.response` 为实际生效事实。

**编排图建边**：
- **节点** = trace_id + pid（每个 coordinator/skill 子进程一个执行单元）
- **派发边**（A→B）= `skill.dispatch` → `skill.start`，`exec_skill_key` 精确匹配（兜底名称+时序）
- **回传边**（B→C）= `skill.end` → `coordinator.activation(skill_completion)`；
  并行路径按 `batch_id` 精确匹配，旧单 Skill 轨迹按 skill_name + 时序兜底
  （C 是**新子进程**，非 A 的延续）。未进入 Python Executor 的框架失败分支
  使用 `batch.result` 生成合成节点及 dispatch/return 边。

`batch.result.status` 保留框架原始 `true/false/partial` 以兼容旧消费者；
建树和 Protobuf 应直接使用 `workflow_status`。它只描述 Workflow 分支是否正常闭合，
每个分支的业务成功或失败必须读取 `results[].skill_status`，两者不得互相覆盖。

Reader 校验同时覆盖进程内 seq、LLM/Tool 配对、血缘引用、成功派发后的
Skill start/terminal、框架 Batch 兜底以及 dialogue.start/dialogue.end 完整性。

`dialogue.end` 表示 Coordinator 产生的候选终态；`client.delivery` 表示 Bridge 已准备或
写出对应 HTTP/SSE 数据，并通过 `candidate_event_id` 指回该候选。它不等价于客户端 ACK，
因此固定记录 `delivery_claim=server_side_observation`、`client_acknowledged=false`。

运行时 `AF_SESSION_ID` 仍只用于 Hook 连接 DataBus，这是 AgentFlow 底座内部协议，不写入
轨迹，也不参与 User/Conversation/Dialogue 聚合。Reader 和 Bridge 仍能读取历史
`session.header/session.start/session.end`，但新采集只产生 `dialogue.*`。
