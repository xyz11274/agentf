# AgentFlow sessionlog：LLM/工具轨迹采集（部署与使用）

> 目标：一条从会话开始的完整轨迹线，融合 python_spans（生命周期）与 llm/traces（内容），
> 为轨迹树 / skill 编排 / 自进化（Early-Exit、Episode 重放）铺底座。
> 实现方式：**不改引擎代码**，靠 AgentFlow 自带的 hooks 系统（agent 包配置层）。

---

## 一、这是什么

sessionlog 通过 AgentFlow 原库的 hooks 机制（mounts/engine/registry/loader），在 11 个关键动作点
（LLM 调用前后、工具前后、skill 派发/完成、会话起止）接住事件，把 LLM/工具/skill 的轨迹
**融合成一条线性事件流**落盘到 `trace_output/sessionlogs/{dialogue_id}.jsonl`。

```
8 种事件（嵌套动作对）：
session.start / llm.request / llm.response / tool.call / tool.result /
skill.dispatch / skill.start / skill.end / session.end

编排图（节点=trace_id，边=派发/回传）：
[A coordinator] ──派发边──▶ [B skill] ──回传边──▶ [C coordinator]（新子进程）
```

## 二、文件清单

| 文件 | 用途 | 部署位置 |
|---|---|---|
| `sessionlog_hook.py` | 采集 + 落盘（hook 实现） | 复制到 agent 包 `harness/hooks/` |
| `sessionlog_reader.py` | 读取/校验/重放/编排图 | 任意可执行位置（验证用） |

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

# 4. 跑一个任务，检查落盘
ls <agentflow_root>/trace_output/sessionlogs/     # 出现 {dialogue_id}.jsonl
```

> 无需重启 daemon：子进程启动时自动 source .env + 加载 hooks（coordinator_decide.py:355 / skill_executor.py:434）。

## 四、使用方法

### 4.1 查看一条完整轨迹

```bash
# 概要：校验 + 重放 + 编排图
python3 docs/sessionlog/sessionlog_reader.py <dialogue_id> [sessionlog_path]

# 示例
python3 docs/sessionlog/sessionlog_reader.py 4375731883344659352 \
    trace_output/sessionlogs/4375731883344659352.jsonl
```

输出三部分：
1. **校验**：按 trace_id 分组检查 seq 递增、llm.request/response 配对、血缘引用
2. **重放**：按行序恢复每次 llm.request 的 messages（模型看到的一切）
3. **编排图**：节点=trace_id（coordinator/skill 执行单元），边=派发/回传

### 4.2 数据结构

**事件信封**（每行一个事件）：
```json
{"type":"llm.response","seq":3,"ts":1787707359000,"pid":668,
 "trace_id":"py_local_668_1787707357","dialogue_id":"...","session_id":"0",
 "node_id":"coordinator_decide","round":1,
 "span_id":"sp_...","parent_span_id":"sp_...","source_event_seqs":[2],
 "data":{...}}
```

**8 种事件 data**：

| type | data 字段 | 触发挂载点 |
|---|---|---|
| `session.start` | `{trigger_type, model, completed_skill}` | AGENT_DIALOGUE_START |
| `llm.request` | `{model, messages(全量), msg_count}` | *_BEFORE_LLM_CALL（ctx.messages） |
| `llm.response` | `{call_id, response(全量含logprobs), meta(task_context)}` | *_AFTER_LLM_CALL（读 DataBus） |
| `tool.call` | `{skill_name, tool_names, round}` | SKILL_EXECUTOR_BEFORE_TOOL_CALL |
| `tool.result` | `{skill_name, tool_names, results_count, duration_ms, round}` | SKILL_EXECUTOR_AFTER_TOOL_CALL |
| `skill.dispatch` | `{skill_names, round, exec_skill_keys}` | COORDINATOR_AFTER_SKILL_DISPATCH |
| `skill.start` | `{skill_name, model, max_rounds, tools_count, exec_skill_key}` | SKILL_EXECUTOR_TRIGGER |
| `skill.end` | `{skill_name, status, round, exec_skill_key}` | SKILL_EXECUTOR_AFTER_SKILL_COMPLETE |
| `session.end` | `{status, response_text}` | AGENT_DIALOGUE_END |

**编排图建边**：
- **节点** = trace_id（每个 coordinator/skill 子进程一个执行单元）
- **派发边**（A→B）= `skill.dispatch` → `skill.start`，`exec_skill_key` 精确匹配（兜底名称+时序）
- **回传边**（B→C）= `skill.end` → 新决策段 `session.start(completed_skill)`，skill_name + 时序
  （C 是**新子进程**，非 A 的延续）
