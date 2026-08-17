#!/usr/bin/env python3
"""
Message token estimation & context compaction.

Split out of ``llm_call.py``: owns the token-estimation and compaction
ladder (fold-to-file + priority-based drop, design:
docs/design/context-compression.md).

Dependency direction: this module may import ``llm_call.call_llm``
(``_call_summary_llm`` needs the LLM entry point); ``llm_call`` never
imports back — the dependency graph stays acyclic.
"""

import json
import os
import sys
from copy import deepcopy
import time
from dataclasses import dataclass, field


# ── Token estimation & message compaction ──

@dataclass
class FoldSpec:
    """折叠落盘上下文(架构修复 S2, 2026-08-14)。

    ``seq/session_id/dialogue_id/fold_dir`` 四个强相关参数总是成组传递,
    封装为上下文对象消除 ``_fold_by_role`` 的 10 参数模糊接口。
    """

    seq: list = field(default_factory=lambda: [0])
    session_id: str = ""
    dialogue_id: str = ""
    fold_dir: str = "/tmp/agentflow/folded"

def estimate_message_tokens(messages: list) -> int:
    """Estimate total tokens in a message list (conservative overestimate).

    Each message carries ~8 tokens of structural overhead (role key, newlines).
    Content tokens estimated at 2 chars/token for mixed Chinese/English.
    Tool-call arguments add extra token cost.

    The estimate is intentionally conservative (over-counts) to trigger
    compaction slightly earlier than strictly necessary — safer for preventing
    finish=length truncation.
    """
    total = 0
    for msg in messages:
        total += 8  # structural overhead
        content = msg.get("content", "")
        if isinstance(content, str):
            # ~2 chars per token for mixed Chinese/English (conservative)
            total += len(content) // 2
        # Tool calls content
        for tc in msg.get("tool_calls") or []:
            fn = tc.get("function", {})
            total += len(fn.get("name", "")) // 2
            total += len(str(fn.get("arguments", ""))) // 3
    return total


def compact_messages(messages: list, max_tokens: int,
                     compaction_threshold: float = 0.8) -> list:
    """Compact message list by dropping oldest non-system messages.

    Keeps all system messages, then keeps as many of the newest non-system
    messages as fit within ``max_tokens * compaction_threshold``.

    Returns the compacted list (original message objects, not copies).
    If no compaction is needed, returns the original list unchanged.
    """
    budget = int(max_tokens * compaction_threshold)
    current = estimate_message_tokens(messages)
    if current <= budget:
        return messages

    # Separate system vs non-system
    system_msgs = [m for m in messages if m.get("role") == "system"]
    other_msgs = [m for m in messages if m.get("role") != "system"]

    # Build result: start with system, add as many newest non-system messages
    # as fit within budget. Skip (don't break) messages that exceed budget
    # so smaller older messages can still be kept.
    kept = list(system_msgs)
    for msg in reversed(other_msgs):
        candidate = kept + [msg]
        if estimate_message_tokens(candidate) <= budget:
            kept = candidate

    # kept is [system, newest_msg, ...] — reverse non-system part to
    # restore chronological order: [system, oldest_kept, ..., newest]
    kept_system = [m for m in kept if m.get("role") == "system"]
    kept_other  = [m for m in kept if m.get("role") != "system"]
    kept_other.reverse()
    return kept_system + kept_other


# ═══════════════════════════════════════════════════════════════════
# Context compression: fold-to-file + priority-based drop
# (design: docs/design/context-compression.md)
# ═══════════════════════════════════════════════════════════════════

def compress_messages(
    messages: list,
    budget: int,
    session_id: str = "",
    dialogue_id: str = "",
    fold_dir: str = "/tmp/agentflow/folded",
    protect_last_rounds: int = 3,
    hard_compression_enabled: bool = False,
    hard_compression_model: str = "",
    node_type: str = "",
) -> tuple[list, str]:
    """按优先级从低到高逐级压缩消息列表。

    压缩序列（每步完成后检查 budget，≤ budget 即停）：
      1. 丢弃 reminder（P5）
      2. 折叠 tool_result 到文件（P4）
      3. 折叠 tool_call 到文件（P3）
      4. 硬压缩：LLM 语义摘要旧对话轮次（P2，可选，默认关闭）
      5. 丢弃 assistant_content，保留最近 2 条（P2 降级）
      6. 丢弃 user_instruction，保留最近 2 条（P1）
      7. 极端兜底

    返回 (compressed_messages, level)：
      level = "skip" / "drop_reminder" / "fold_tool_result" / "fold_tool_call"
            / "hard_summary" / "drop_assistant" / "drop_user" / "emergency"
    """
    current = estimate_message_tokens(messages)
    _t0 = time.monotonic()
    if current <= budget:
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] compaction level=skip duration_ms={_ms:.1f}", file=sys.stderr)
        return messages, "skip"

    # 深拷贝元素：折叠/丢弃会原地改 msg，不能污染调用方的 messages
    result = [deepcopy(m) for m in messages]
    fold_spec = FoldSpec(seq=[0], session_id=session_id,
                         dialogue_id=dialogue_id, fold_dir=fold_dir)
    before_tok = current

    def _ratio(after_tok: int) -> float:
        return round(1 - after_tok / before_tok, 2) if before_tok else 0.0

    # ── Step 1: 丢弃 reminder（P5，本轮会重新生成） ──
    before_count = len(result)
    result = [m for m in result if not _is_reminder(m)]
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        dropped = before_count - len(result)
        print(f"[flow]   compaction: drop_reminder "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"dropped={dropped})", file=sys.stderr)
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] compaction level=drop_reminder duration_ms={_ms:.1f}", file=sys.stderr)
        return result, "drop_reminder"

    # ── Step 2: 折叠 tool_result 到文件（P4） ──
    before_count = len(result)
    result, folded = _fold_by_role(result, "tool", "tool_result", budget, fold_spec)
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        print(f"[flow]   compaction: fold_tool_result "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"folded={folded})", file=sys.stderr)
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] compaction level=fold_tool_result duration_ms={_ms:.1f}", file=sys.stderr)
        return result, "fold_tool_result"

    # ── Step 3: 折叠 tool_call 到文件（P3，保留最近 max(3, N) 条） ──
    # N = protect_last_rounds，保证最近轮次内的 tool_call 链完整
    tool_call_protect = max(3, protect_last_rounds)
    before_count = len(result)
    result, folded = _fold_by_role(result, "assistant", "tool_call", budget, fold_spec,
                                   protect_field="tool_calls", protect_last=tool_call_protect)
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        print(f"[flow]   compaction: fold_tool_call "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"folded={folded})", file=sys.stderr)
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] compaction level=fold_tool_call duration_ms={_ms:.1f}", file=sys.stderr)
        return result, "fold_tool_call"

    # ── Step 4: 硬压缩（P2，LLM 语义摘要，可选，默认关闭） ──
    if hard_compression_enabled:
        before = estimate_message_tokens(result)
        result = _hard_summarize(result, budget, protect_last_rounds,
                                hard_compression_model, node_type)
        after_tok = estimate_message_tokens(result)
        if after_tok < before:
            print(f"[flow]   compaction: hard_summary "
                  f"({before}→{after_tok} tok, ratio={_ratio(after_tok)})",
                  file=sys.stderr)
            _ms = (time.monotonic() - _t0) * 1000
            print(f"[timing] compaction level=hard_summary duration_ms={_ms:.1f}", file=sys.stderr)
            return result, "hard_summary"

    # ── Step 5: 丢弃 assistant_content（P2 降级，保留最近 2 条） ──
    # 注意：已折叠的 assistant（_folded=True）不能再丢弃——其 content 是文件索引引用
    before_count = len(result)
    result, dropped = _drop_by_role(result, "assistant", budget, protect_last=2,
                                    exclude_if_has="_folded")
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        print(f"[flow]   compaction: drop_assistant "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"dropped={dropped})", file=sys.stderr)
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] compaction level=drop_assistant duration_ms={_ms:.1f}", file=sys.stderr)
        return result, "drop_assistant"

    # ── Step 6: 丢弃 user_instruction（P1，保留最近 2 条） ──
    before_count = len(result)
    result, dropped = _drop_by_role(result, "user", budget, protect_last=2,
                                    exclude_if=_is_reminder)
    after_tok = estimate_message_tokens(result)
    if after_tok <= budget:
        print(f"[flow]   compaction: drop_user "
              f"({before_tok}→{after_tok} tok, ratio={_ratio(after_tok)}, "
              f"dropped={dropped})", file=sys.stderr)
        _ms = (time.monotonic() - _t0) * 1000
        print(f"[timing] compaction level=drop_user duration_ms={_ms:.1f}", file=sys.stderr)
        return result, "drop_user"

    # ── Step 7: 极端兜底 ──
    result = _emergency_trim(result, budget)
    _ms = (time.monotonic() - _t0) * 1000
    print(f"[timing] compaction level=emergency duration_ms={_ms:.1f}", file=sys.stderr)
    return result, "emergency"


def _is_reminder(msg: dict) -> bool:
    """判断是否为 reminder 消息（system-reminder 注入的 user 消息）。"""
    return (msg.get("role") == "user"
            and isinstance(msg.get("content"), str)
            and "<system-reminder>" in msg["content"])


def _fold_by_role(result: list, role: str, ftype: str, budget: int,
                  fold_spec: FoldSpec,
                  protect_field: str = None, protect_last: int = 0) -> tuple[list, int]:
    """按 role 折叠消息到文件。从最旧到最新。返回 (result, folded_count)。"""
    candidates = []
    for i, msg in enumerate(result):
        if msg.get("role") != role:
            continue
        if msg.get("_folded"):
            continue
        if protect_field and not msg.get(protect_field):
            continue
        candidates.append(i)

    # 保护最近 N 条
    if protect_last > 0 and len(candidates) > protect_last:
        candidates = candidates[:-protect_last]

    # 从最旧到最新折叠
    folded = 0
    for i in candidates:
        _fold_one(result[i], fold_spec, ftype)
        folded += 1
        if estimate_message_tokens(result) <= budget:
            return result, folded

    return result, folded  # 折叠后仍超


def _drop_by_role(result: list, role: str, budget: int, protect_last: int = 0,
                  exclude_if_has: str = None, exclude_if=None) -> tuple[list, int]:
    """按 role 丢弃消息。从最旧到最新，保留最近 N 条。返回 (result, dropped_count)。"""
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
    dropped = 0
    for i in reversed(candidates):
        del result[i]
        dropped += 1
        if estimate_message_tokens(result) <= budget:
            return result, dropped

    return result, dropped  # 丢弃后仍超


def _emergency_trim(result: list, budget: int) -> list:
    """极端兜底：只保留 system + 第一轮 + 最近 1 轮。

    第一轮 = 最早一组 user/assistant（R1 保护用户原始意图）。
    最近 1 轮 = 最晚一组 user/assistant（保局部连贯性）。
    """
    system = [m for m in result if m.get("role") == "system"]
    non_sys = [m for m in result if m.get("role") != "system"]

    # 找第一轮和最近 1 轮的边界
    first_user_idx = next((i for i, m in enumerate(non_sys) if m["role"] == "user"), None)
    # 最近一轮的 user 起点（从尾往前找第一个 user）
    last_user_idx = None
    for i in range(len(non_sys) - 1, -1, -1):
        if non_sys[i]["role"] == "user" and not _is_reminder(non_sys[i]):
            last_user_idx = i
            break

    kept = list(system)
    if first_user_idx is not None:
        # 第一轮到下一个 user 之前（或整个尾部如果只有一轮）
        first_round_end = next((i for i in range(first_user_idx + 1, len(non_sys))
                               if non_sys[i]["role"] == "user"), len(non_sys))
        kept.extend(non_sys[first_user_idx:first_round_end])
    if last_user_idx is not None and last_user_idx != first_user_idx:
        kept.extend(non_sys[last_user_idx:])

    print(f"[flow]   compaction: EMERGENCY trim — {len(result)}→{len(kept)} msgs "
          f"({estimate_message_tokens(result)}→{estimate_message_tokens(kept)} tok)",
          file=sys.stderr)
    return kept


def _hard_summarize(result: list, budget: int, protect_last_rounds: int = 3,
                    model: str = "", node_type: str = "") -> list:
    """硬压缩：LLM 语义摘要旧对话轮次。

    保护规则：
      1. 第一轮（用户原始意图）— 不压缩
      2. 最近 N 轮（局部连贯性）— 不压缩
      3. 保护轮次之间的老旧轮次 — 压缩为摘要
    tool_call/tool_result 已在 Step 2-3 中折叠到文件，不参与硬压缩。
    如果 LLM 调用失败，静默跳过，返回原列表。
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
    last_rounds = rounds[-protect_last_rounds:] if protect_last_rounds > 0 else []
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
    summary = _call_summary_llm(source_text, model, node_type)
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
        return compressed

    return result  # 摘要后仍超，返回原列表


def _call_summary_llm(source_text: str, model: str = "", node_type: str = "") -> str | None:
    """调用 LLM 生成对话摘要。失败时返回 None，调用方静默跳过。

    注意：source_text 可能很大。如果估计 token 超过模型窗口，
    从最旧部分截断，保留最新部分。截断后仍超窗口则静默跳过。

    模型选择：显式参数 > 当前节点模型（AgentConfig.llm_model(node_type=...)）
    """
    from agentflow_runtime.common.agent_config import AgentConfig
    from agentflow_runtime.llm_call import call_llm

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
        # source_text 太长，从最旧部分截断，保留尾部（最新部分）
        ratio = available / estimated
        cut = int(len(source_text) * ratio)
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
        # 压缩用模型：显式参数 > 当前节点模型（llm_model 永不返回空，有兜底）
        resolved_model = model or AgentConfig.llm_model(node_type=node_type)
        resp = call_llm(
            messages=[{"role": "user", "content": prompt}],
            model=resolved_model,
            max_completion_tokens=500,
            extra_body={"temperature": 0.3},
        )
        # 检查 finish_reason
        finish = resp.get("choices", [{}])[0].get("finish_reason", "")
        if finish == "length":
            print("[flow]   compaction: hard_summary output truncated (finish=length)", file=sys.stderr)
            # 截断的输出仍可用，返回已有内容
        content = resp.get("choices", [{}])[0].get("message", {}).get("content", "")
        return content.strip() if content else None
    except Exception as exc:
        print(f"[flow]   compaction: hard_summary LLM call failed: {exc}", file=sys.stderr)
        return None


def _get_model_max_for_summary(model: str) -> int:
    """获取压缩模型的最大上下文窗口（简化版）。"""
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


def _fold_one(msg: dict, fold_spec: FoldSpec, ftype: str = "tool_result") -> None:
    """将单条消息折叠到文件，替换 content 为索引引用（原地修改 msg）。"""
    content = msg.get("content", "")
    tool_call_id = msg.get("tool_call_id", "")
    tool_calls = msg.get("tool_calls")

    n = fold_spec.seq[0]
    fold_spec.seq[0] += 1

    # sanitize session_id/dialogue_id 防路径注入（替换 ../ / 等危险字符）
    safe_session = "".join(c if c.isalnum() or c in "-_" else "_" for c in (fold_spec.session_id or "_"))
    safe_dialog = "".join(c if c.isalnum() or c in "-_" else "_" for c in (fold_spec.dialogue_id or "_"))
    subdir = os.path.join(fold_spec.fold_dir, safe_session, safe_dialog)
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

    _update_manifest(subdir, n, ftype, tool_call_id,
                     estimate_message_tokens([msg]), fold_spec.session_id, fold_spec.dialogue_id)

    msg["content"] = f"[{ftype} saved to {file_path}]"
    msg["_folded"] = True
    msg["_folded_file"] = file_path
    if tool_calls:
        msg.pop("tool_calls", None)


def _update_manifest(subdir: str, seq_n: int, ftype: str, tool_call_id: str,
                     tokens: int, session_id: str = "", dialogue_id: str = "") -> None:
    """更新 manifest.json（折叠文件索引清单）。"""
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
        "seq": seq_n,
        "type": ftype,
        "tool_call_id": tool_call_id,
        "original_tokens": tokens,
        "file": f"{ftype}_{seq_n}.txt",
    })
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)


def get_model_max_tokens() -> int:
    """Read AF_MODEL_MAX_TOKENS env var, defaulting to a safe 128k.

    Delegates to :class:`common.agent_config.AgentConfig`.
    """
    from agentflow_runtime.common.agent_config import AgentConfig
    return AgentConfig.model_max_tokens()


def get_compaction_threshold() -> float:
    """Read AF_COMPACTION_THRESHOLD env var (0.0–1.0), default 0.8.

    Delegates to :class:`common.agent_config.AgentConfig`.
    """
    from agentflow_runtime.common.agent_config import AgentConfig
    return AgentConfig.compaction_threshold()
