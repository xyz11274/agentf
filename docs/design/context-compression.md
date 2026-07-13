# AgentFlow 执行面上下文压缩机制设计

> **层级**: L3 · `docs/design/context-compression.md`
> **状态**: 草案 v0.5
> **创建日期**: 2026-07-11
> **参考**: 
>   - [工具结果多级压缩方法论](../.agents/ai-coding-spec/reference/raw_docs/agent框架/agent系统/context_engineering/3.上下文压缩/7.工具结果多级压缩.md) — L0/L1/L2/L3 四级压缩体系，本设计对应 L2 微压缩（折叠到文件留索引）

---

## 1. 问题与方案

### 1.1 现状

当前 `compact_messages()` 在每轮 LLM 调用前执行，策略是**一刀切丢弃最旧的非 system 消息**：

```
输入: 22条消息, 150000 tok → budget=102400
  ↓ 从最新消息开始保留，丢弃最旧的非 system 消息
输出: 13条消息, 102000 tok（丢了 9 条，包括最早的 user 指令）
```

问题：tool_result 体积最大价值最低，但旧算法保留它而丢弃了用户指令。

### 1.2 方案：折叠到文件留索引

**核心思路**：tool_result 不丢弃，而是折叠到文件系统，留索引引用。

```
折叠前:
  {"role": "tool", "content": "{"weather": "晴", "temperature": 35, "humidity": "60%", ...}"}  ← 1500 tok

折叠后:
  {"role": "tool", "content": "[tool_result saved to /tmp/agentflow/folded/session_xxx/dialogue_xxx/tool_result_0.txt]"}
  ← 25 tok，节省 1475 tok
```

**为什么不丢弃？** tool_result 虽然体积大，但 LLM 后续可能需要回顾。折叠到文件后，LLM 可通过内置工具读取，上下文窗口不被占用。

### 1.3 压缩策略

两级策略，不做复杂分级：

| 预算占用 | 策略 | 做法 |
|---------|------|------|
| ≤ budget | 不压缩 | 直接调用 LLM |
| > budget | **折叠 tool_result** | 从最旧到最新，逐条折叠到文件 |
| 所有 tool_result 已折叠，仍超 | **逐级丢弃** | 按 P5→P4→P3→P2→P1 优先级逐级压缩，详见 §2.2 |

| 本设计 | 方法论级别 | 说明 |
|-------|-----------|------|
| 不压缩 | L0 | 原文完整，预算内不处理 |
| **折叠 tool_result** | **L2 微压缩** | **核心：折叠到文件留索引** |
| 逐级丢弃 + 硬压缩 | L1/L3 简化 | 折叠后仍超 budget，按优先级逐级丢弃/硬压缩，详见 §2.2 压缩序列 |

---

## 2. 核心机制

### 2.1 保护轮次与压缩序列

#### 保护轮次

压缩序列必须保护三类内容，不可压缩：

| 保护级 | 保护对象 | 原因 | 配置 |
|--------|---------|------|------|
| R0 | **system** | 框架指令，永不压缩 | 硬编码 |
| R1 | **第一轮 user + assistant** | 用户的原始意图，压缩后目标丢失 | 硬编码 |
| R2 | **最近 N 轮完整对话** | 保持局部连贯性 | `AF_PROTECT_LAST_ROUNDS`（默认 3，待确认） |

> 保护级用 R（Round）命名，与下文消息优先级 P（Priority）区分。R 级内容永不被任何 P 级压缩触碰。

**"一轮"的定义**：一个完整的交互周期——从用户输入开始，到 assistant 输出最终回复结束，中间可能包含多次 tool_call + tool_result。

```
第一轮（保护）:  user_input → tool_call → tool_result → assistant_output
第二轮（保护）:  user_input → tool_call → tool_result → assistant_output
第三轮（保护）:  user_input → tool_call → tool_result → assistant_output
第四轮（可压缩）: user_input → tool_call → tool_result → assistant_output
...
```

#### 消息优先级

超出保护轮次的消息，按以下优先级从低到高压缩：

| 优先级 | 消息类型 | 压缩手段 | 每节省 token |
|--------|---------|---------|-------------|
| P5（最低） | **reminder** | 直接丢弃（本轮会重新生成） | 小（~50 tok） |
| P4 | **tool_result** | 折叠到文件，留索引引用 | **大（~1K-50K tok）** |
| P3 | **tool_call** | 折叠到文件，留索引引用 | 中（~200-2K tok） |
| P2 | **历史对话轮次** | 硬压缩：LLM 语义摘要，替换旧轮次 | **大（~500-5K tok 每轮）** |
| P1 | **user_instruction** | 保留最近 N 条，丢弃更早的 | 小（~50-500 tok） |
| P0（最高） | **system** | 永不压缩 | — |

### 2.2 压缩序列

```
每轮 call_llm 前执行：

  current = estimate_message_tokens(messages)
  budget = model_max_tokens × compaction_threshold   # 默认 128000 × 0.8 = 102400

  如果 current ≤ budget:
      → 不压缩，直接调用 LLM（skip）

  否则，在保护轮次之外，按优先级从低到高逐级执行：

  Step 1: 丢弃全部 reminder（P5，本轮会重新生成）
          → 如果 ≤ budget，停止

  Step 2: 折叠 tool_result（P4，从最旧到最新，跳过保护轮次内的）
          → 每折叠一条，重新估算。如果 ≤ budget，停止

  Step 3: 折叠 tool_call（P3，从最旧到最新，跳过保护轮次内的，保留最近 max(3, N) 条）
          → N = AF_PROTECT_LAST_ROUNDS，保证最近轮次内的 tool_call 链完整
          → 每折叠一条，重新估算。如果 ≤ budget，停止

  Step 4: 硬压缩（P2，LLM 语义摘要，跳过保护轮次，保留最近 3 轮完整对话）
          → 将保护轮次之外的老旧 user/assistant 对话轮次压缩为一段语义摘要
          → 替换原始轮次为一条 summary 消息
          → 如果 ≤ budget，停止
          → 如果硬压缩不可用（LLM 调用失败），跳过此步

  Step 5: 丢弃 assistant_content（P2 降级，保留最近 2 条，从最旧开始丢弃）
          → 在保护轮次之外丢弃

  Step 6: 丢弃 user_instruction（P1，保留最近 2 条，从最旧开始丢弃）
          → 在保护轮次之外丢弃

  Step 7: 如果执行完 Step 1-6 仍 > budget：
          → 强制保留 system + 第一轮 + 最近 1 轮，丢弃其余全部
          → 输出警告日志
```

**终止条件**：
- 正常终止：`current ≤ budget`（Step 1-6 中任一步达到即停）
- 硬压缩跳过：硬压缩 LLM 调用失败时，跳过 Step 4，进入 Step 5
- 强制终止：所有可压缩内容已处理完毕仍 > budget（Step 7，极端兜底）

### 2.3 硬压缩（LLM 语义摘要）

#### 触发条件

硬压缩在 Step 4 执行，条件是：
1. Step 1-3 执行完毕仍 > budget
2. 配置中启用了硬压缩（`AF_HARD_COMPRESSION_ENABLED=true`，默认关闭）
3. 存在可压缩的历史对话轮次（早于保护轮次之外）

```
最近 3 轮保留完整
  ↓
[user_1, assistant_1, user_2, assistant_2, user_3, assistant_3]  ← 这 3 轮保留
  ↓
[user_4, assistant_4, user_5, assistant_5, ...]  ← 更早的轮次压缩为摘要
  ↓
替换为一条 summary 消息:
  {"role": "assistant", "content": "早期对话摘要: 用户询问了天气和穿搭建议，助手推荐了...", "_meta": {"kind": "context_summary"}}
```

#### 执行流程

```
1. 从 messages 中识别 user/assistant 对话轮次对
2. 保留第一轮 + 最近 N 轮完整（保护轮次）
3. 将中间的老旧轮次提取为 source_text
4. 估算 source_text token 数，如果超过模型窗口，从最旧部分截断
5. 构造压缩 prompt，调用 LLM 生成摘要
6. 将摘要替换原始轮次
7. 如果 LLM 调用失败，跳过此步（不阻塞流程）
```

#### 失败保护

硬压缩的 LLM 调用本身可能因长度问题失败，三种保护机制：

| 失败场景 | 处理方式 |
|---------|---------|
| source_text 超过模型窗口 | 从最旧部分截断，保留最新部分 |
| LLM 输出被 finish=length 截断 | 已生成的摘要仍可用，返回部分结果 |
| LLM 调用完全失败（网络/超时） | 静默跳过，进入 Step 5 丢弃策略 |

#### 压缩 Prompt

```text
你是 AgentFlow 的上下文压缩器。你的任务是将一段历史对话压缩为简洁的摘要。

要求：
1. 提取用户的原始输入是什么——第一轮用户的原始诉求，以及被压缩轮次中用户的后续输入
2. 提取已经做完了什么、结论是什么——已执行了哪些工具/技能，得到了什么结果/结论，已决策的事项
3. 提取已经进行过什么尝试、失败的原因——尝试过哪些方案，哪些失败了/被放弃了，原因是什么
4. 提取接下来需要做什么——未完成的事项，下一步计划，需要用户提供什么信息
5. 输出不超过 500 字符
6. 只总结已有信息，不引入新内容
7. 不确定的信息标注"不确定"
8. 忽略工具调用的技术细节，只关注业务语义

历史对话：
{source_text}

结构化摘要：
```

#### 成本

- 每次硬压缩消耗 1 次 LLM 调用（额外成本）
- 摘要输入：被压缩的原始轮次 token 数（可能较大）
- 摘要输出：~200-500 tok
- 建议使用较便宜的模型执行压缩（如 `AF_HARD_COMPRESSION_MODEL=gpt-4o-mini`）

#### 约束

- 硬压缩不改变 tool_result/tool_call 的折叠文件（已在 Step 2-3 中处理）
- 硬压缩不得压缩 system 消息
- 硬压缩不得压缩最近 N 轮对话（保持局部连贯性）
- 硬压缩失败时静默跳过，不阻塞 LLM 主调用
- 硬压缩摘要在 trace 中记录为 `compression_level: "hard_summary"`

### 2.4 折叠操作（tool_result / tool_call）

```
对每条 tool_result 或 tool_call 消息（从最旧到最新）：
  1. 写文件: /tmp/agentflow/folded/{session_id}/{dialogue_id}/{type}_{seq}.txt
     文件内容: 原始 content 全文（纯文本，不包 JSON 壳）
  2. 替换消息 content:
     "[tool_result saved to /tmp/agentflow/folded/{session_id}/{dialogue_id}/{type}_{seq}.txt]"
  3. 标记 msg["_folded"] = True
  4. 更新 manifest.json
  5. 重新估算 total tokens。如果 ≤ budget，停止
```

### 2.5 文件结构

```
/tmp/agentflow/folded/
  └── {session_id}/
      └── {dialogue_id}/
          ├── tool_result_0.txt
          ├── tool_result_1.txt
          ├── tool_call_0.txt
          └── manifest.json     ← 索引清单
```

**tool_result 文件内容**（纯文本，直接写原始内容）：
```
{"weather": "晴", "temperature": 35, "humidity": "60%", "wind": "3级"}
```

**tool_call 文件内容**（纯文本）：
```
[function: get_weather]
arguments: {"city": "北京"}
```

**manifest.json**：
```json
{
  "session_id": "session_xxx",
  "dialogue_id": "dialogue_xxx",
  "folded_items": [
    {"seq": 0, "type": "tool_result", "tool_call_id": "call_abc", "original_tokens": 1500, "file": "tool_result_0.txt"},
    {"seq": 1, "type": "tool_call", "tool_call_id": "call_def", "original_tokens": 320, "file": "tool_call_0.txt"}
  ]
}
```

---

## 3. 算法伪代码

```python
import os
import json

def compress_messages(
    messages: list,
    budget: int,
    session_id: str = "",
    dialogue_id: str = "",
    fold_dir: str = "/tmp/agentflow/folded",
    protect_last_rounds: int = 3,          # R2: 最近 N 轮完整保护（第一轮 R1 硬编码单独保护）
    hard_compression_enabled: bool = False,  # 硬压缩开关，默认关闭
    hard_compression_model: str = "",        # 压缩用模型，空=使用主模型
) -> tuple[list, str]:
    """
    按优先级从低到高逐级压缩消息列表。
    
    压缩序列：
      1. 丢弃 reminder（P5）
      2. 折叠 tool_result 到文件（P4）
      3. 折叠 tool_call 到文件（P3）
      4. 硬压缩：LLM 语义摘要旧对话轮次（P2，可选，默认关闭）
      5. 丢弃 assistant_content，保留最近 N 条（P2 降级）
      6. 丢弃 user_instruction，保留最近 N 条（P1）
      7. 紧急兜底
    
    每一步完成后检查 budget，≤ budget 即停。
    
    返回 (compressed_messages, level)：
      level = "skip" / "drop_reminder" / "fold_tool_result" / "fold_tool_call" 
            / "hard_summary" / "drop_assistant" / "drop_user" / "emergency"
    """
    current = estimate_message_tokens(messages)
    if current <= budget:
        return messages, "skip"
    
    result = list(messages)
    seq = [0]
    
    # ── Step 1: 丢弃 reminder（P5，本轮会重新生成） ──
    result = [m for m in result if not _is_reminder(m)]
    if estimate_message_tokens(result) <= budget:
        return result, "drop_reminder"
    
    # ── Step 2: 折叠 tool_result 到文件（P4） ──
    result = _fold_by_role(result, "tool", "tool_result", budget, seq, session_id, dialogue_id, fold_dir)
    if estimate_message_tokens(result) <= budget:
        return result, "fold_tool_result"
    
    # ── Step 3: 折叠 tool_call 到文件（P3，保留最近 N 条） ──
    # tool_call 保护数沿用 protect_last_rounds（最近 N 轮内的 tool_call 自然受保护，
    # 轮次外的最近 3 条 tool_call 也保留，给 LLM 看到最近的工具调用链）
    tool_call_protect = max(3, protect_last_rounds)
    result = _fold_by_role(result, "assistant", "tool_call", budget, seq, session_id, dialogue_id, fold_dir,
                           protect_field="tool_calls", protect_last=tool_call_protect)
    if estimate_message_tokens(result) <= budget:
        return result, "fold_tool_call"
    
    # ── Step 4: 硬压缩（P2，LLM 语义摘要，可选，默认关闭） ──
    if hard_compression_enabled:
        before = estimate_message_tokens(result)
        result = _hard_summarize(result, budget, protect_last_rounds, hard_compression_model)
        if estimate_message_tokens(result) < before:  # 硬压缩生效（摘要替换了旧轮次）
            return result, "hard_summary"
    
    # ── Step 5: 丢弃 assistant_content（P2 降级，保留最近 N 条） ──
    result = _drop_by_role(result, "assistant", budget, protect_last=2,
                           exclude_if_has="tool_calls")
    if estimate_message_tokens(result) <= budget:
        return result, "drop_assistant"
    
    # ── Step 6: 丢弃 user_instruction（P1，保留最近 N 条） ──
    result = _drop_by_role(result, "user", budget, protect_last=2,
                           exclude_if=_is_reminder)
    if estimate_message_tokens(result) <= budget:
        return result, "drop_user"
    
    # ── Step 7: 极端兜底 ──
    result = _emergency_trim(result, budget)
    return result, "emergency"


def _is_reminder(msg: dict) -> bool:
    return (msg.get("role") == "user" 
            and isinstance(msg.get("content"), str)
            and "<system-reminder>" in msg["content"])


def _fold_by_role(result: list, role: str, ftype: str, budget: int, seq: list,
                  session_id: str, dialogue_id: str, fold_dir: str,
                  protect_field: str = None, protect_last: int = 0) -> list:
    """按 role 折叠消息到文件。从最旧到最新。"""
    # 收集需要折叠的消息及其索引
    candidates = []
    for i, msg in enumerate(result):
        if msg.get("role") != role:
            continue
        if msg.get("_folded"):
            continue
        if protect_field and not msg.get(protect_field):
            continue
        if protect_field and msg.get(protect_field):
            # tool_call: 需要保留最近 N 条
            pass
        candidates.append(i)
    
    # 保护最近 N 条
    if protect_last > 0 and len(candidates) > protect_last:
        candidates = candidates[:-protect_last]
    
    # 从最旧到最新折叠
    for i in candidates:
        _fold_one(result[i], seq, session_id, dialogue_id, fold_dir, ftype)
        if estimate_message_tokens(result) <= budget:
            return result
    
    return result  # 折叠后仍超


def _drop_by_role(result: list, role: str, budget: int, protect_last: int = 0,
                  exclude_if_has: str = None, exclude_if=None) -> list:
    """按 role 丢弃消息。从最旧到最新，保留最近 N 条。"""
    # 收集可丢弃的索引
    candidates = []
    for i, msg in enumerate(result):
        if msg.get("role") != role:
            continue
        if exclude_if_has and msg.get(exclude_if_has):
            continue  # 跳过已折叠的 tool_call
        if exclude_if and exclude_if(msg):
            continue
        candidates.append(i)
    
    # 保留最近 N 条
    if protect_last > 0 and len(candidates) > protect_last:
        candidates = candidates[:-protect_last]
    
    # 从最旧到最新丢弃（从后往前删，避免索引偏移）
    for i in reversed(candidates):
        del result[i]
        if estimate_message_tokens(result) <= budget:
            return result
    
    return result  # 丢弃后仍超


def _emergency_trim(result: list, budget: int) -> list:
    """极端兜底：只保留 system + 最近 1 条 user + 最近 1 条 assistant。"""
    system = [m for m in result if m.get("role") == "system"]
    users = [m for m in result if m.get("role") == "user" and not _is_reminder(m)]
    assistants = [m for m in result if m.get("role") == "assistant"]
    
    kept = list(system)
    if users:
        kept.append(users[-1])  # 最近 1 条 user
    if assistants:
        kept.append(assistants[-1])  # 最近 1 条 assistant
    
    print(f"[flow]   compaction: EMERGENCY trim — {len(result)}→{len(kept)} msgs "
          f"({estimate_message_tokens(result)}→{estimate_message_tokens(kept)} tok)", file=sys.stderr)
    return kept


def _hard_summarize(result: list, budget: int, protect_last_rounds: int = 3,
                    model: str = "") -> list:
    """硬压缩：LLM 语义摘要旧对话轮次。

    保护轮次外的 user/assistant 对话轮次压缩为一段语义摘要。
    如果 LLM 调用失败，静默跳过，返回原列表。

    保护规则：
      1. 第一轮（用户原始意图）— 不压缩
      2. 最近 N 轮（局部连贯性）— 不压缩
      3. 保护轮次之间的老旧轮次 — 压缩为摘要
    tool_call/tool_result 已在 Step 2-3 中折叠到文件，不参与硬压缩。
    """
    # 识别 user/assistant 对话轮次对
    rounds = []
    current_round = []
    for msg in result:
        role = msg.get("role", "")
        if role == "user" and current_round:
            rounds.append(current_round)
            current_round = [msg]
        elif role == "assistant" and not msg.get("tool_calls"):
            current_round.append(msg)
        elif role == "user":
            current_round = [msg]
    if current_round:
        rounds.append(current_round)
    
    # 保护轮次：第一轮 + 最近 N 轮
    if len(rounds) <= 1 + protect_last_rounds:
        return result  # 不够保护轮次，不压缩
    
    # 第一轮保护
    first_round = [rounds[0]]
    # 最近 N 轮保护
    last_rounds = rounds[-protect_last_rounds:]
    # 中间可压缩的轮次
    summary_rounds = rounds[1:-protect_last_rounds] if protect_last_rounds > 0 else rounds[1:]
    
    if not summary_rounds:
        return result  # 没有可压缩的轮次
    
    # 提取摘要源文本
    source_text = ""
    for r in summary_rounds:
        for msg in r:
            content = msg.get("content", "")
            if content:
                source_text += f"[{msg['role']}]: {content}\n"
    
    # 调用 LLM 生成摘要
    summary = _call_summary_llm(source_text, model)
    if not summary:
        return result  # LLM 调用失败，静默跳过
    
    # 替换原始轮次为摘要消息
    summary_msg = {
        "role": "assistant",
        "content": f"[上下文压缩摘要] {summary}",
        "_meta": {"kind": "context_summary"},
    }
    
    # 构建新列表：system + 摘要 + 第一轮 + 最近 N 轮
    system = [m for m in result if m.get("role") == "system"]
    other = []
    for r in first_round + last_rounds:
        other.extend(r)
    
    compressed = system + [summary_msg] + other
    
    if estimate_message_tokens(compressed) <= budget:
        print(f"[flow]   compaction: hard_summary — "
              f"{len(summary_rounds)} rounds → 1 summary "
              f"({estimate_message_tokens(result)}→{estimate_message_tokens(compressed)} tok)",
              file=sys.stderr)
        return compressed
    
    return result  # 摘要后仍超，返回原列表


def _call_summary_llm(source_text: str, model: str = "") -> str | None:
    """调用 LLM 生成对话摘要。

    使用专用压缩 prompt，不污染主任务 LLM 调用。
    失败时返回 None，调用方静默跳过。

    注意：source_text 可能很大（被压缩轮次的全部内容）。
    如果估计 token 超过模型窗口，从最旧部分开始截断，直到能放下。
    截断后仍超窗口则静默跳过。
    """
    # 估算 source_text 的 token 数
    # prompt 模板固定部分约 200 tok，输出预留 500 tok
    prompt_overhead = 200
    output_reserve = 500
    model_max = _get_model_max_for_summary(model)
    available = model_max - prompt_overhead - output_reserve
    
    if available <= 0:
        return None  # 模型窗口太小，无法压缩
    
    estimated = len(source_text) // 2  # 保守估算：2 字符/token
    if estimated > available:
        # source_text 太长，从最旧部分截断
        ratio = available / estimated
        cut = int(len(source_text) * ratio)
        # 保留尾部（最新部分），截断头部（最旧部分）
        source_text = f"...(省略前 {len(source_text) - cut} 字符)...\n{source_text[-cut:]}"
        print(f"[flow]   compaction: hard_summary source truncated "
              f"({estimated}→{available} tok)", file=sys.stderr)
    
    prompt = (
        "你是 AgentFlow 的上下文压缩器。将以下历史对话压缩为结构化的摘要。\n\n"
        "提取以下四类信息：\n"
        "1. 用户的原始输入是什么——第一轮用户的原始诉求，以及被压缩轮次中用户的后续输入\n"
        "2. 已经做完了什么、结论是什么——已执行了哪些工具/技能，得到了什么结果/结论，已决策的事项\n"
        "3. 已经进行过什么尝试、失败的原因——尝试过哪些方案，哪些失败了/被放弃了，原因是什么\n"
        "4. 接下来需要做什么——未完成的事项，下一步计划，需要用户提供什么信息\n\n"
        "要求：\n"
        "- 输出不超过 500 字符\n"
        "- 只总结已有信息，不引入新内容\n"
        "- 不确定的信息标注\"不确定\"\n"
        "- 忽略工具调用的技术细节，只关注业务语义\n\n"
        f"历史对话：\n{source_text}\n\n结构化摘要："
    )
    try:
        resp = call_llm(
            messages=[{"role": "user", "content": prompt}],
            model=model or os.environ.get("AF_LLM_MODEL", "gpt-4o-mini"),
            max_completion_tokens=500,
            temperature=0.3,
        )
        # 检查 finish_reason
        finish = resp.get("choices", [{}])[0].get("finish_reason", "")
        if finish == "length":
            print(f"[flow]   compaction: hard_summary output truncated (finish=length)", file=sys.stderr)
            # 截断的输出仍可用，返回已有内容
        content = resp.get("choices", [{}])[0].get("message", {}).get("content", "")
        return content.strip() if content else None
    except Exception as exc:
        print(f"[flow]   compaction: hard_summary LLM call failed: {exc}", file=sys.stderr)
        return None


def _get_model_max_for_summary(model: str) -> int:
    """获取压缩模型的最大上下文窗口（简化版）。"""
    # 常见模型窗口大小
    known = {
        "gpt-4o": 128000,
        "gpt-4o-mini": 128000,
        "deepseek-v4-flash": 128000,
        "qwen-vl-max": 32000,
    }
    for prefix, size in known.items():
        if model.startswith(prefix):
            return size
    return 128000  # 默认 128K


def _fold_one(msg: dict, seq: list[int], session_id: str, dialogue_id: str, fold_dir: str, ftype: str = "tool_result"):
    """将单条消息折叠到文件，替换 content 为索引引用。"""
    content = msg.get("content", "")
    tool_call_id = msg.get("tool_call_id", "")
    tool_calls = msg.get("tool_calls")
    
    n = seq[0]
    seq[0] += 1
    
    subdir = os.path.join(fold_dir, session_id or "_", dialogue_id or "_")
    os.makedirs(subdir, exist_ok=True)
    
    file_path = os.path.join(subdir, f"{ftype}_{n}.txt")
    with open(file_path, "w", encoding="utf-8") as f:
        if tool_calls:
            for tc in tool_calls:
                fn = tc.get("function", {})
                f.write(f"[function: {fn.get('name', '?')}]\n")
                f.write(f"arguments: {fn.get('arguments', '')}\n")
        else:
            f.write(content)
    
    _update_manifest(subdir, n, ftype, tool_call_id, estimate_message_tokens([msg]),
                     session_id, dialogue_id)
    
    msg["content"] = f"[{ftype} saved to {file_path}]"
    msg["_folded"] = True
    msg["_folded_file"] = file_path
    if tool_calls:
        msg.pop("tool_calls", None)


def _update_manifest(subdir: str, seq: int, ftype: str, tool_call_id: str, tokens: int,
                     session_id: str = "", dialogue_id: str = ""):
    """更新 manifest.json。"""
    manifest_path = os.path.join(subdir, "manifest.json")
    manifest = {"session_id": session_id, "dialogue_id": dialogue_id, "folded_items": []}
    if os.path.exists(manifest_path):
        try:
            with open(manifest_path, "r") as f:
                manifest = json.load(f)
        except (json.JSONDecodeError, IOError):
            pass
    manifest["session_id"] = session_id
    manifest["dialogue_id"] = dialogue_id
    manifest["folded_items"].append({
        "seq": seq,
        "type": ftype,
        "tool_call_id": tool_call_id,
        "original_tokens": tokens,
        "file": f"{ftype}_{seq}.txt",
    })
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
```

---

## 4. 决策示例

### 示例 1：轻度超 budget，只折叠 tool_result

```
输入（按时间顺序，从旧到新）:
  system(5)  user(1)  assistant(1)  tool_call(3)  tool_result(3)  reminder(1)
  共 14 条消息, 110000 tok

budget = 102400
110000 > 102400 → 触发压缩

Step 1: 丢弃 reminder → 108000 tok
Step 2: 折叠最旧 tool_result(1) → 106000 tok
Step 3: 折叠中间 tool_result(1) → 103000 tok
Step 4: 折叠最新 tool_result(1) → 100000 tok ✓

输出: fold_tool_result
  system(5)  user(1)  assistant(1)  tool_call(3)  tool_result(3, 已折叠)
  共 13 条消息, 100000 tok

文件:
  /tmp/agentflow/folded/{sid}/{did}/tool_result_0.txt
  /tmp/agentflow/folded/{sid}/{did}/tool_result_1.txt
  /tmp/agentflow/folded/{sid}/{did}/tool_result_2.txt
```

### 示例 2：重度超 budget，执行到折叠 tool_call

```
输入:
  system(5)  user(3)  assistant(3)  tool_call(5)  tool_result(5)  reminder(1)
  共 22 条消息, 200000 tok

budget = 102400
200000 > 102400 → 触发压缩

Step 1: 丢弃 reminder → 198000
Step 2: 折叠全部 tool_result(5) → 130000
Step 3: 折叠 tool_call(2, 从最旧，保留最近 3) → 102000 ✓

输出: fold_tool_call
  system(5)  user(3)  assistant(3)  tool_call(3, 保留)  tool_result(5, 已折叠)
  共 19 条消息, 102000 tok

特点:
  - 所有 user / assistant 被保留 ✓
  - 最近 3 条 tool_call 保留，更早的折叠到文件 ✓
  - 所有 tool_result 折叠到文件 ✓
```

### 示例 3：极端超 budget，执行到丢弃 user

```
输入:
  system(5)  user(8)  assistant(8)  tool_call(8)  tool_result(8)  reminder(1)
  共 38 条消息, 300000 tok

budget = 102400
300000 > 102400 → 触发压缩

Step 1: 丢弃 reminder → 298000
Step 2: 折叠全部 tool_result(8) → 220000
Step 3: 折叠全部 tool_call(5, 保留最近 3) → 150000
Step 4: 丢弃 assistant(6, 保留最近 2) → 90000 ✓

输出: drop_assistant
  system(5)  user(8)  assistant(2, 保留)  tool_call(3, 保留)  tool_result(8, 已折叠)
  共 26 条消息, 90000 tok

特点:
  - 所有 user 指令被保留 ✓
  - 极端情况下才丢弃 assistant_content
  - 工具调用相关（tool_call + tool_result）全部可恢复（文件）
```

---

## 5. 配置

### 5.1 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `AF_MODEL_MAX_TOKENS` | 128000 | 模型最大上下文窗口 |
| `AF_COMPACTION_THRESHOLD` | 0.8 | 触发压缩的阈值比例（0.1-1.0）。budget = max_tokens × threshold |
| `AF_FOLD_DIR` | `/tmp/agentflow/folded` | 折叠文件存储目录 |
| `AF_PROTECT_LAST_ROUNDS` | `3` | 保护最近 N 轮完整对话（第一轮由 R1 硬编码单独保护，不从此值扣除） | ⚠️ 待确认 |
| `AF_HARD_COMPRESSION_ENABLED` | `false` | 是否启用硬压缩（LLM 语义摘要），默认关闭 |
| `AF_HARD_COMPRESSION_MODEL` | `""` | 硬压缩使用的模型名，空=使用主模型 |

### 5.2 AgentConfig 扩展

```python
class AgentConfig:
    @staticmethod
    def fold_dir() -> str:
        return os.environ.get("AF_FOLD_DIR", "/tmp/agentflow/folded")
    
    @staticmethod
    def compaction_threshold() -> float:
        val = _get_float("AF_COMPACTION_THRESHOLD", 0.8)
        return max(0.1, min(1.0, val))
    
    @staticmethod
    def protect_last_rounds() -> int:
        return _get_int("AF_PROTECT_LAST_ROUNDS", 3)
    
    @staticmethod
    def hard_compression_enabled() -> bool:
        return os.environ.get("AF_HARD_COMPRESSION_ENABLED", "false").lower() == "true"
    
    @staticmethod
    def hard_compression_model() -> str:
        return os.environ.get("AF_HARD_COMPRESSION_MODEL", "")
```

---

## 6. 日志格式

```
# 未触发
[flow]   compaction: skip (50000 tok, budget=102400)

# 丢弃 reminder
[flow]   compaction: drop_reminder (108000→102000 tok, ratio=0.06, dropped=1)

# 折叠 tool_result
[flow]   compaction: fold_tool_result (110000→84000 tok, ratio=0.24, folded=3)
[flow]   compaction files: /tmp/agentflow/folded/{sid}/{did}/tool_result_0.txt ...

# 折叠 tool_call
[flow]   compaction: fold_tool_call (130000→102000 tok, ratio=0.22, folded=2)
[flow]   compaction files: /tmp/agentflow/folded/{sid}/{did}/tool_call_0.txt ...

# 硬压缩（LLM 语义摘要）
[flow]   compaction: hard_summary (150000→95000 tok, ratio=0.37, 5 rounds→1 summary)

# 丢弃 assistant
[flow]   compaction: drop_assistant (150000→90000 tok, ratio=0.40, dropped=6)

# 丢弃 user
[flow]   compaction: drop_user (90000→85000 tok, ratio=0.06, dropped=1)

# 极端兜底
[flow]   compaction: EMERGENCY trim — 38→8 msgs (300000→85000 tok)
```

**日志字段**：
| 字段 | 含义 |
|------|------|
| `skip/drop_reminder/fold_tool_result/...` | 压缩级别（哪一步终止） |
| `110000→84000 tok` | 压缩前 → 压缩后 token 数 |
| `ratio=0.24` | 压缩率 |
| `folded=3` | 折叠了 3 条消息 |
| `dropped=6` | 丢弃了 6 条消息 |

---

## 7. 向后兼容

| 场景 | 行为 |
|------|------|
| 环境变量未设置 | 默认折叠策略 |
| 旧 agent 包 | 默认折叠策略，行为优于旧版 |
| 希望旧行为 | 设置 `AF_COMPACTION_THRESHOLD=1.0`（永不触发） |

---

## 8. 不变量

| # | 不变量 | 验证 |
|---|--------|------|
| I-1 | 压缩后消息总 token ≤ `model_max_tokens × compaction_threshold` | 断言 |
| I-2 | system 消息数量不变 | 计数 |
| I-3 | 保留的消息保持原始顺序 | 索引检查 |
| I-4 | 折叠的文件内容与原始消息内容一致 | 内容校验 |
| I-5 | 同一轮不会重复折叠同一条消息 | `_folded` 标记检查 |
| I-6 | 压缩序列严格按优先级执行：reminder → tool_result → tool_call → assistant → user | 步骤日志检查 |
| I-7 | 每一步完成后检查 budget，≤ budget 即停，不执行后续步骤 | 断点检查 |
| I-8 | 每次压缩输出 `[flow] compaction:` 日志行，含终止步骤名 | 日志检查 |
| I-9 | 折叠文件在 session 结束时清理 | 无残留 |

---

## 9. 实现计划

### Phase 1

在 `llm_call.py` 中新增/修改：

```
llm_call.py:
  - compact_messages()              ← 保持不变（仅旧 agent 包兼容保留，不再被主链路调用）
  - compress_messages()             ← 新增（完整压缩序列入口）
  - _is_reminder()                  ← 新增（判断 reminder 消息）
  - _fold_by_role()                 ← 新增（按 role 折叠消息）
  - _drop_by_role()                 ← 新增（按 role 丢弃消息）
  - _emergency_trim()               ← 新增（极端兜底）
  - _fold_one()                     ← 新增（折叠单条到文件）
  - _update_manifest()              ← 新增（更新 manifest.json）
  - estimate_message_tokens()       ← 保持不变
  - AgentConfig.fold_dir()              ← 新增
  - AgentConfig.protect_last_rounds()   ← 新增（R2 最近 N 轮保护）
  - AgentConfig.hard_compression_*()    ← 新增（硬压缩配置）
  - _hard_summarize()                   ← 新增（硬压缩：LLM 语义摘要）
  - _call_summary_llm()                 ← 新增（硬压缩 LLM 调用）
  - _get_model_max_for_summary()        ← 新增（硬压缩模型窗口查询）
```

coordinator 和 skill_executor 的压缩调用点从 `compact_messages()` 改为 `compress_messages()`。

### Phase 2（后续）

- **清理逻辑**：session 结束时清理 `/tmp/agentflow/folded/{session_id}/`。清理时机可以是 session 销毁（SESSION-DESTROY 命令）或 agent stop 时。未清理的遗留文件由外层运维策略（如 tmpwatch 或 systemd-tmpfiles）兜底

> 折叠文件为纯文本 `.txt` 格式，LLM 可通过已有的 `bash` 工具用 `cat` 命令读取，无需新增专用工具。

### 测试

| 测试 | 覆盖 |
|------|------|
| `test_compress_skip_within_budget` | 预算内不触发 |
| `test_compress_drop_reminder` | reminder 被优先丢弃 |
| `test_compress_fold_tool_result` | tool_result 先被折叠 |
| `test_compress_fold_tool_call` | tool_call 在 tool_result 之后被折叠 |
| `test_compress_drop_assistant` | assistant 在 tool_call 之后被丢弃 |
| `test_compress_drop_user` | user 在 assistant 之后被丢弃 |
| `test_compress_protect_last_n` | 最近 N 轮被保护（R1 第一轮 + R2 最近 N 轮） |
| `test_compress_emergency_trim` | 极端兜底只保留 system + 第一轮 + 最近 1 轮 |
| `test_compress_files_match_content` | 折叠文件内容与原文一致 |
| `test_compress_manifest_created` | manifest.json 正确 |
| `test_compress_hard_summary_enabled` | 硬压缩启用时，旧轮次被替换为摘要消息 |
| `test_compress_hard_summary_disabled` | 硬压缩关闭时（默认），跳过 Step 4 进入 Step 5 |
| `test_compress_hard_summary_skip_on_failure` | LLM 调用失败时静默跳过，进入 Step 5 |
| `test_compress_hard_summary_truncate_source` | source_text 超模型窗口时从最旧部分截断 |
| `test_compress_hard_summary_protect_first_round` | 第一轮（R1）永不压缩 |
| `test_compress_integration_coordinator` | coordinator 集成 |
| `test_compress_integration_skill` | skill_executor 集成 |

---

## 10. 对 Trace 系统的影响分析

AgentFlow 现有三层 trace 系统，压缩机制对每层的影响不同：

### 10.1 C++ Runtime Trace（TraceManager / TraceSpan）

| 项目 | 说明 |
|------|------|
| **记录内容** | 框架层 span：node 执行、workflow 推进、agent 调度。不记录消息内容 |
| **受影响？** | **否**。压缩发生在 Python 节点进程内部，对 C++ 运行时透明 |
| **压缩日志** | `[flow] compaction:` 行通过 stderr 输出，被 daemon 日志捕获，可在 runtime log 中检索 |

### 10.2 Python 节点 Trace（DataBus `llm` 命名空间）

`_TraceRecorder` (`llm_call.py:_TraceRecorder`) 将每次 LLM 调用的请求/响应记录到 DataBus：

| Trace 键 | 记录内容 | 压缩影响 |
|----------|---------|---------|
| `traces/{call_id}/request` | 发送给 LLM 的完整请求体（含 messages 列表） | **有影响**。记录的是压缩后的 messages——tool_result 内容变为 `[tool_result saved to /tmp/...]`，非原始内容 |
| `traces/{call_id}/response` | LLM 原始响应体 | **无影响**。响应不受压缩影响 |
| `traces/{call_id}/meta` | token 用量、finish_reason、latency | **有影响**。`prompt_tokens` 反映的是压缩后的消息长度，不等于原始消息长度 |
| `traces/index` | call_id 有序列表 | **无影响** |

**影响分析**：

- **Trace 回放时看到的是压缩后的消息**。如果调试人员通过 trace 查看"LLM 收到了什么"，看到的是 `[tool_result saved to /tmp/...]` 而非原始内容
- **原始内容在折叠文件中**，但折叠文件是 ephemeral 的（`/tmp/` 目录，session 结束后清理）
- **prompt_tokens 统计值会偏低**，因为压缩后的消息更短——但这实际上是**正确的**，因为 `prompt_tokens` 反映的是实际发送给 LLM 的 token 数

**建议**：

- 在 `traces/{call_id}/meta` 中增加 `compressed: true/false` 标志，标注该次请求是否经过压缩
- 在 `traces/{call_id}/meta` 中增加 `compression_level` 字段，记录压缩终止步骤（`skip/fold_tool_result/fold_tool_call/drop_assistant/drop_user`）
- 如需完整重建"LLM 实际看到的上下文"，需同时保留 trace + 折叠文件（manifest.json 提供索引）

### 10.3 Observability 事件（`emit_event`）

`observability.py:emit_event` 输出结构化 JSON 事件到 stderr：

| 事件 | 受影响字段 | 影响 |
|------|-----------|------|
| `coordinator.llm_call_start` | `"messages": len(messages)` | 计数为压缩后的消息数，比原始少 |
| `skill_executor.llm_round_start` | `"messages": len(messages)` | 同上 |
| 其他事件 | 无 messages 计数 | 无影响 |

**影响分析**：

- `messages` 计数反映的是压缩后的消息数——这是**正确的**，因为实际发送给 LLM 的就是压缩后的消息
- 但如果用于调试"历史对话轮次"，这个计数会偏低

**建议**：

- 在事件 data 中增加 `"compressed": true` 标志（当压缩发生时）
- 如需原始消息数，可从 `traces/{call_id}/meta` 的 `request_summary` 中反推

### 10.4 总结

| Trace 层 | 影响程度 | 是否需要修改 |
|---------|---------|------------|
| C++ Runtime Trace | 无 | 否 |
| Python DataBus Trace | 中（request 中 messages 被压缩，meta 中 prompt_tokens 变少） | 建议在 meta 中增加 `compressed` 标志 |
| Observability 事件 | 低（messages 计数反映压缩后数量） | 可选在 data 中增加 `compressed` 标志 |

> **核心原则**：trace 记录的是"实际发送给 LLM 的请求"，压缩后的消息就是实际发送的，所以 trace 记录压缩后的内容是**正确的**。折叠文件提供原始内容的可恢复路径。如需完整重建，需要 trace + manifest.json 配合使用。

### 10.5 关键问题：Trace 里看到的是什么？

#### Q1: 第一轮用户问答（未压缩）的 trace 是什么样的？

未压缩时，`traces/{call_id}/request` 中包含完整的 tool_result 内容：

```json
{
  "model": "deepseek-v4-flash",
  "messages": [
    {"role": "system", "content": "..."},
    {"role": "user", "content": "北京今天多少度？"},
    {"role": "assistant", "content": "", "tool_calls": [{"function": {"name": "get_weather", "arguments": "{\"city\": \"北京\"}"}}]},
    {"role": "tool", "tool_call_id": "call_abc", "content": "{\"weather\": \"晴\", \"temperature\": 35, \"humidity\": \"60%\"}"},
    {"role": "assistant", "content": "北京今天晴天，35°C"}
  ]
}
```

#### Q2: 压缩之后获取到的 trace 是什么样的？

压缩后，`traces/{call_id}/request` 中 tool_result 被替换为文件路径索引：

```json
{
  "model": "deepseek-v4-flash",
  "messages": [
    {"role": "system", "content": "..."},
    {"role": "user", "content": "北京今天多少度？"},
    {"role": "assistant", "content": "", "tool_calls": [{"function": {"name": "get_weather", "arguments": "{\"city\": \"北京\"}"}}]},
    {"role": "tool", "tool_call_id": "call_abc", "content": "[tool_result saved to /tmp/agentflow/folded/session_xxx/dialogue_xxx/tool_result_0.txt]"},
    {"role": "assistant", "content": "北京今天晴天，35°C"}
  ]
}
```

`traces/{call_id}/meta` 中会记录压缩后的 token 用量：

```json
{
  "model": "deepseek-v4-flash",
  "prompt_tokens": 320,
  "completion_tokens": 45,
  "total_tokens": 365,
  "request_summary": "sys+user+asst+tool+asst",
  "latency_ms": 1200
}
```

#### Q3: Trace 是全部对话结束后才可获取吗？

**不是。Trace 是实时写入的**，每次 `call_llm()` 完成后立即写入 DataBus `llm` 命名空间：

```
call_llm() 返回后
  → _TraceRecorder.trace_call() 写入 DataBus
  → Bridge 通过 /api/agents/{id}/llm-traces 端点可实时查询
```

Bridge 的 trace 查询接口（`llm_trace.py`）通过 daemon 的 `databus.get` / `databus.list_keys` RPC 实时读取 DataBus。对话进行中即可获取到当前已完成的 trace。

#### Q4: 获取到的是真实的 tool_result，还是被压缩后的？

**获取到的是压缩后的版本**。因为数据流是：

```
messages (原始) → compress_messages() → messages (压缩) → call_llm() → trace 记录
```

`compress_messages()` 在 `call_llm()` 之前执行，trace 记录的是压缩后的 messages。原始 tool_result 内容不在 trace 中，仅在折叠文件中（`/tmp/agentflow/folded/`）。

**如果需要原始内容，需要同时查询 trace + 读取折叠文件**：

```
trace 中:  [tool_result saved to /tmp/.../tool_result_0.txt]
              ↓  cat 读取
原始内容:  {"weather": "晴", "temperature": 35, "humidity": "60%"}
```

#### Q5: 多次 LLM 调用之间，trace 是新增记录还是累加修改？

**每次 `call_llm()` 产生一条全新的 trace 记录，互不覆盖，不累加修改。**

每条 trace 记录包含：
- 独立的 `call_id`（UUID，每次调用生成新的）
- 独立的 `traces/{call_id}/request`（该次调用发送的 messages，压缩后的版本）
- 独立的 `traces/{call_id}/response`（该次 LLM 返回的原始响应）
- 独立的 `traces/{call_id}/meta`（该次调用的 token 用量、延迟等）

以 skill_executor 的多轮循环为例：

```
Round 1: messages = [system, user, ...]  →  compress  →  call_llm  →  Trace(call_id_1)
           ↓ 处理 tool_call，追加 tool_result + assistant 回复到 messages
Round 2: messages = [system, user, ..., tool_result, assistant, ...]  →  compress  →  call_llm  →  Trace(call_id_2)
           ↓ 继续追加
Round 3: messages = [system, user, ..., tool_result, assistant, ...]  →  compress  →  call_llm  →  Trace(call_id_3)
```

**结果**：3 轮产生 3 条独立的 trace 记录：

| trace | call_id | 记录内容 |
|-------|---------|---------|
| Trace 1 | `call_id_1` | Round 1 发送的 messages（压缩后），含 1 组 tool_call+tool_result |
| Trace 2 | `call_id_2` | Round 2 发送的 messages（压缩后），含 2 组 tool_call+tool_result |
| Trace 3 | `call_id_3` | Round 3 发送的 messages（压缩后），含 3 组 tool_call+tool_result |

**关键点**：
- Trace 1 在 Round 1 完成后写入，永不修改
- Trace 2 在 Round 2 完成后写入，独立于 Trace 1
- 如果 Round 1 未触发压缩，Trace 1 中 tool_result 是完整内容
- 如果 Round 2 触发了压缩，Trace 2 中 tool_result 是文件索引
- 每条 trace 独立可查，可对比"压缩前 vs 压缩后"的差异

`traces/index` 列表会累积所有 call_id，用于枚举和查询：

```json
// traces/index（随时间增长）
["call_id_1"]                          ← Round 1 完成后
["call_id_1", "call_id_2"]            ← Round 2 完成后
["call_id_1", "call_id_2", "call_id_3"]  ← Round 3 完成后
```

但每个 `traces/{call_id}/request` 的内容是独立的，写入后不修改。

---

## 11. 术语表

| 术语 | 说明 |
|------|------|
| **折叠（Fold）** | 将消息内容保存到文件，替换为索引引用。不是丢弃 |
| **丢弃（Drop）** | 完全移除消息，不保留。只用于 reminder |
| **预算（Budget）** | `model_max_tokens × compaction_threshold` |
| **保护（Protect）** | 最近 N 条消息不折叠或丢弃 |
| **索引（Index）** | 折叠后消息中的文件路径引用 |

---

## 12. 参考

- 知识库方法论：`reference/raw_docs/agent框架/agent系统/context_engineering/3.上下文压缩/`
  - [7.工具结果多级压缩.md](../.agents/ai-coding-spec/reference/raw_docs/agent框架/agent系统/context_engineering/3.上下文压缩/7.工具结果多级压缩.md) — tool_result 四级压缩体系（L0不压缩/L1预压缩/L2微压缩折叠/L3硬压缩摘要）
  - 本设计对应 L2 微压缩（折叠到文件留索引）。**L1 因 head+tail 截断导致中间信息丢失，需要配套文件保存才能恢复，实际等价于 L2 实现**，故本项目直接从 L2 开始
- 现有实现：`examples/agent/outfit-assistant/nodes/llm_call.py` — `compact_messages()` 函数
- 模板：`frontend/bridge/templates/agent_template/harness_agent/nodes/` — 通用 harness agent 模板
