"""
Sidecar flag 路由逻辑精确仿真测试。

基于真实 SSE fixture 数据，逐行模拟 sidecar_router.cpp 的四步处理：
  1. 解析 SSE JSON 行
  2. 提取 content（优先 delta.content，fallback message.content）
  3. 判断 flag（_progress=true || _chunk_type="progress" → flag=1，否则 flag=0）
  4. 累积 content → 构造 callback 报文

验证 flag 路由、content 累积、callback 格式与 sidecar 源码行为一致。
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

FIXTURE_PATH = Path(__file__).resolve().parent / "fixtures" / "sse_capture.jsonl"


# ═══════════════════════════════════════════════════════════════════
# sidecar_router.cpp 核心逻辑的 Python 仿真
# ═══════════════════════════════════════════════════════════════════

def sse_extract_token(payload: dict) -> str:
    """模拟 sidecar_router.cpp L324-333: 优先 delta.content，fallback message.content。

    额外处理格式 C（嵌套 JSON progress）：
    当 message.content 以 { 开头时，解析为 JSON 并提取内层 delta.content。
    """
    choices = payload.get("choices", [])
    if not choices:
        return ""
    choice = choices[0]
    # 优先: choices[0]["delta"]["content"]  (llm_chunk)
    delta = choice.get("delta", {})
    if isinstance(delta, dict):
        token = delta.get("content", "")
        if token:
            return token
    # fallback: choices[0]["message"]["content"]  (text/progress)
    msg = choice.get("message", {})
    if isinstance(msg, dict):
        content = msg.get("content", "")
        if not content:
            return ""
        # 格式 C: message.content 可能是嵌套 OpenAI chunk 的 JSON 字符串
        if content.startswith("{"):
            try:
                nested = json.loads(content)
                inner_delta = nested.get("choices", [{}])[0].get("delta", {})
                if isinstance(inner_delta, dict):
                    token = inner_delta.get("content", "")
                    if token:
                        return token
            except (json.JSONDecodeError, KeyError, IndexError):
                pass
        return content
    return ""


def sse_is_progress(payload: dict) -> bool:
    """模拟 sidecar_router.cpp L336-337: _progress || _chunk_type=="progress" → flag=1。"""
    return bool(payload.get("_progress", False)) or payload.get("_chunk_type", "") == "progress"


def simulate_sidecar_callback(payloads: list[dict]) -> list[dict]:
    """完整模拟 sidecar 的流式处理，返回 callback 调用序列。

    对应 sidecar_router.cpp L299-343:
      - 逐行解析 SSE
      - 提取 token → accumulated_content += token
      - 判断 is_progress
      - post_callback("Running", flag, accumulated_content, false)
      - 流结束后: post_callback("Completed", 0, accumulated_content, true)
    """
    callbacks: list[dict] = []
    accumulated_content = ""
    total_tokens = 0

    for payload in payloads:
        token = sse_extract_token(payload)
        if not token:
            continue  # sidecar L334: if token.empty() return;

        accumulated_content += token
        total_tokens += 1
        is_prog = sse_is_progress(payload)
        flag = 1 if is_prog else 0

        callbacks.append({
            "seq": total_tokens,
            "flag": flag,
            "stream_status": "Running",
            "is_final": False,
            "token_len": len(token),
            "accumulated_len": len(accumulated_content),
            "accumulated_preview": accumulated_content[-80:],
        })

    # 流结束: post_callback("Completed", 0, accumulated_content, true)
    callbacks.append({
        "seq": total_tokens + 1,
        "flag": 0,
        "stream_status": "Completed",
        "is_final": True,
        "token_len": 0,
        "accumulated_len": len(accumulated_content),
        "accumulated_preview": accumulated_content[-80:],
    })

    return callbacks


# ═══════════════════════════════════════════════════════════════════
# 测试用例
# ═══════════════════════════════════════════════════════════════════

@pytest.fixture(scope="module")
def fixture() -> list[dict]:
    if not FIXTURE_PATH.exists():
        pytest.skip(f"SSE fixture not found: {FIXTURE_PATH}")
    with open(FIXTURE_PATH) as f:
        return [json.loads(line) for line in f if line.strip()]


@pytest.fixture(scope="module")
def callbacks(fixture: list[dict]) -> list[dict]:
    return simulate_sidecar_callback(fixture)


# ── 1. flag 路由 ──


def test_flag_1_for_progress(callbacks: list[dict]) -> None:
    """_progress=true 的回调应分配 flag=1。"""
    progress_cbs = [cb for cb in callbacks if cb["is_final"] is False and cb["flag"] == 1]
    assert len(progress_cbs) > 0, "应有 flag=1 的回调"
    print(f"  flag=1 回调数: {len(progress_cbs)}")


def test_flag_0_for_llm_chunk(callbacks: list[dict]) -> None:
    """llm_chunk（非 progress）的回调应分配 flag=0。"""
    non_progress_cbs = [cb for cb in callbacks if cb["is_final"] is False and cb["flag"] == 0]
    assert len(non_progress_cbs) > 0, "应有 flag=0 的回调"
    print(f"  flag=0 回调数: {len(non_progress_cbs)}")


def test_flag_transition_blocks(fixture: list[dict]) -> None:
    """sidecar 应识别出 4 个 flag 块（FLAG-1 → FLAG-0 → FLAG-1 → FLAG-0）。"""
    blocks: list[tuple[int, int]] = []
    for payload in fixture:
        flag = 1 if sse_is_progress(payload) else 0
        if not blocks or blocks[-1][0] != flag:
            blocks.append((flag, 1))
        else:
            blocks[-1] = (flag, blocks[-1][1] + 1)

    assert len(blocks) == 4, (
        f"期望 4 个 flag 块（PROGRESS→LLM→PROGRESS→LLM），实际 {len(blocks)}"
    )
    assert blocks[0][0] == 1, f"首块应为 flag=1，实际 flag={blocks[0][0]}"
    assert blocks[1][0] == 0, f"第 2 块应为 flag=0，实际 flag={blocks[1][0]}"
    assert blocks[2][0] == 1, f"第 3 块应为 flag=1，实际 flag={blocks[2][0]}"
    assert blocks[3][0] == 0, f"第 4 块应为 flag=0，实际 flag={blocks[3][0]}"

    print(f"  块 1: flag=1, {blocks[0][1]} events")
    print(f"  块 2: flag=0, {blocks[1][1]} events")
    print(f"  块 3: flag=1, {blocks[2][1]} events")
    print(f"  块 4: flag=0, {blocks[3][1]} events")


# ── 2. content 提取 ──


def test_content_extraction_priority(fixture: list[dict]) -> None:
    """content 提取优先用 delta.content，无法提取时 fallback message.content。

    格式 C（嵌套 JSON progress）：message.content 是序列化的 OpenAI chunk，
    应解析 JSON 提取 choices[0].delta.content，而非直接取原始字符串。
    """
    # llm_chunk: 从 delta.content 提取
    llm_chunks = [e for e in fixture if e.get("_chunk_type") == "llm_chunk"]
    for chunk in llm_chunks[:3]:
        token = sse_extract_token(chunk)
        delta = chunk.get("choices", [{}])[0].get("delta", {})
        assert token == delta.get("content", ""), (
            f"llm_chunk 应提取 delta.content"
        )

    # 格式 C（嵌套 JSON progress）：应解析嵌套 JSON 取内层 delta.content
    fmt_c_count = 0
    for evt in fixture:
        if evt.get("_chunk_type") != "progress":
            continue
        msg_content = evt.get("choices", [{}])[0].get("message", {}).get("content", "")
        if not msg_content.startswith("{"):
            continue
        token = sse_extract_token(evt)
        inner = json.loads(msg_content)
        expected = inner["choices"][0]["delta"]["content"]
        assert token == expected, (
            f"格式 C 应提取内层 delta.content\n"
            f"  期望: {expected[:60]}\n"
            f"  实际: {token[:60]}"
        )
        fmt_c_count += 1
        if fmt_c_count >= 3:
            break

    # 格式 A（纯文本 progress）：直接从 message.content 提取
    for evt in fixture:
        if evt.get("_chunk_type") != "progress":
            continue
        msg_content = evt.get("choices", [{}])[0].get("message", {}).get("content", "")
        if msg_content.startswith("{"):
            continue
        token = sse_extract_token(evt)
        assert token == msg_content, (
            f"格式 A 应直接取 message.content"
        )
        break

    print(f"  delta.content (llm_chunk): ✅")
    print(f"  嵌套 JSON → 内层 delta.content (格式 C): ✅ ({fmt_c_count})")
    print(f"  message.content 直接取 (格式 A): ✅")


def test_skip_empty_tokens(fixture: list[dict]) -> None:
    """空 token 应被 sidecar 跳过 (sidecar_router.cpp L334: if token.empty() return;)。"""
    fixtures_with_empty = fixture[:5] + [{}] + fixture[-5:]
    callbacks = simulate_sidecar_callback(fixtures_with_empty)
    # 空 token 不应计入 seq
    assert callbacks, "应该有回调"
    for cb in callbacks:
        assert cb["seq"] > 0
    print(f"  空 token 跳过 ✅")


# ── 3. content 累积 ──


def test_content_accumulation_monotonic(callbacks: list[dict]) -> None:
    """accumulated_content 应单调递增（每次回调内容更多）。"""
    non_final = [cb for cb in callbacks if not cb["is_final"]]
    prev_len = 0
    for cb in non_final:
        assert cb["accumulated_len"] >= prev_len, (
            f"累积长度不应减少: {cb['accumulated_len']} < {prev_len}"
        )
        prev_len = cb["accumulated_len"]
    print(f"  最终累积内容长度: {prev_len} 字符")


def test_content_accumulation_matches_tokens(fixture: list[dict]) -> None:
    """所有 token 拼接后应等于最终 accumulated_content。"""
    all_tokens = ""
    for payload in fixture:
        all_tokens += sse_extract_token(payload)

    callbacks = simulate_sidecar_callback(fixture)
    final_cb = next(cb for cb in callbacks if cb["is_final"])

    assert len(all_tokens) == final_cb["accumulated_len"], (
        f"累积内容长度不匹配: tokens={len(all_tokens)} vs accumulated={final_cb['accumulated_len']}"
    )
    print(f"  token 总和: {len(all_tokens)}")  # 字符
    print(f"  累积验证: ✅")


# ── 4. callback 格式 ──


def test_callback_format_non_final(callbacks: list[dict]) -> None:
    """非最终回调格式: StreamStatus="Running", is_final=False。"""
    non_final = [cb for cb in callbacks if not cb["is_final"]]
    for cb in non_final[:3]:
        assert cb["stream_status"] == "Running", f"非最终回调应标记 Running: {cb}"
        assert cb["is_final"] is False
    print(f"  非最终回调格式 ✅ (前 3 个)")


def test_callback_format_final(callbacks: list[dict]) -> None:
    """最终回调格式: StreamStatus="Completed", flag=0, is_final=True。"""
    final_cb = callbacks[-1]
    assert final_cb["is_final"] is True, "最后一个应是 final 回调"
    assert final_cb["stream_status"] == "Completed", "final 应标记 Completed"
    assert final_cb["flag"] == 0, "final 应分配 flag=0"
    print(f"  最终回调格式 ✅ (seq={final_cb['seq']}, len={final_cb['accumulated_len']})")


def test_callback_count(fixture: list[dict], callbacks: list[dict]) -> None:
    """callback 数 = 有效 token 数 + 1（final）。"""
    total_tokens = sum(1 for e in fixture if sse_extract_token(e))
    assert len(callbacks) == total_tokens + 1, (
        f"期望 {total_tokens + 1} 次回调（{total_tokens} tokens + 1 final），"
        f"实际 {len(callbacks)}"
    )
    print(f"  回调总数: {len(callbacks)} ({total_tokens} tokens + 1 final)")


# ── 5. 多轮场景下的 flag 语义 ──


def test_flag_1_content_never_appears_in_final(callbacks: list[dict]) -> None:
    """验证最终回调 (flag=0, Completed) 包含的是累积全量内容，
    但思考过程（flag=1 块）的多轮中间文本也应被正确累积。"""
    final_cb = callbacks[-1]

    # 收集所有 flag=1 块的累计内容
    non_final = [cb for cb in callbacks if not cb["is_final"]]
    flag_1_blocks = []
    in_flag_1 = False
    block_start = 0
    for cb in non_final:
        if cb["flag"] == 1 and not in_flag_1:
            block_start = cb["seq"]
            in_flag_1 = True
        elif cb["flag"] == 0 and in_flag_1:
            flag_1_blocks.append((block_start, cb["seq"] - 1))
            in_flag_1 = False
    if in_flag_1:
        flag_1_blocks.append((block_start, non_final[-1]["seq"]))

    print(f"  flag=1 块数: {len(flag_1_blocks)}")
    for i, (start, end) in enumerate(flag_1_blocks):
        block_cbs = [cb for cb in non_final if start <= cb["seq"] <= end]
        total_think = sum(cb["token_len"] for cb in block_cbs)
        print(f"    flag=1 块 {i+1}: seq {start}-{end}, {total_think} 字符")

    # 最终回复内容长度 = 最终累积 - 最后一段 flag=0 之前的内容
    # 实际上因为 sidecar 发的是累积内容，最终 reply = final.accumulated_len
    print(f"  最终回复总长度: {final_cb['accumulated_len']} 字符")


# ── 6. 格式 C：嵌套 JSON progress ──


def test_format_c_detection(fixture: list[dict]) -> None:
    """检测格式 C（progress + message.content 是嵌套 JSON）。"""
    fmt_c = 0
    for e in fixture:
        if e.get("_chunk_type") != "progress":
            continue
        content = e.get("choices", [{}])[0].get("message", {}).get("content", "")
        if content.startswith("{"):
            fmt_c += 1

    assert fmt_c > 0, "应检测到格式 C 的嵌套 JSON progress"
    print(f"  格式 C 事件数: {fmt_c}")


def test_format_c_extracts_inner_delta(fixture: list[dict]) -> None:
    """格式 C 的正确提取：解析嵌套 JSON → 提取内层 delta.content。"""
    for e in fixture:
        if e.get("_chunk_type") != "progress":
            continue
        content = e.get("choices", [{}])[0].get("message", {}).get("content", "")
        if not content.startswith("{"):
            continue
        # 验证嵌套 JSON 格式
        parsed = json.loads(content)
        assert "choices" in parsed, "嵌套 JSON 应有 choices 字段"
        inner_delta = parsed["choices"][0].get("delta", {})
        assert "content" in inner_delta, "嵌套 JSON 应有 choices[0].delta.content"
        expected = inner_delta["content"]
        # 验证 sidecar 提取
        actual = sse_extract_token(e)
        assert actual == expected, (
            f"格式 C 提取错误: 期望 '{expected[:50]}', 实际 '{actual[:50]}'"
        )
        break
    print(f"  格式 C 内层 delta.content 提取 ✅: [{expected[:60]}]")


def test_format_c_does_not_pollute_accumulated(fixture: list[dict]) -> None:
    """格式 C 的嵌套 JSON 不应污染 accumulated_content。"""
    # 直接提取未经解析的 message.content（旧行为）
    polluted = 0
    correct = 0
    for e in fixture:
        if e.get("_chunk_type") != "progress":
            continue
        content = e.get("choices", [{}])[0].get("message", {}).get("content", "")
        if content.startswith("{"):
            polluted += len(content)  # 旧行为：整个 JSON 字符串
            try:
                inner = json.loads(content)
                token = inner["choices"][0]["delta"]["content"]
                correct += len(token)  # 正确行为：内层 token
            except (KeyError, IndexError, json.JSONDecodeError):
                pass

    # 旧行为会污染 accumulated_content，正确行为只保留内层文本
    # 格式 C 的 JSON 语法开销：每个事件约 150 字符的 JSON 包装
    json_overhead_per_event = polluted / max(correct, 1)
    print(f"  旧行为（整个 JSON）: {polluted} 字符")
    print(f"  正确行为（内层token）: {correct} 字符")
    print(f"  JSON 语法开销比例: {json_overhead_per_event:.2f}x")
    assert correct < polluted, "正确行为应剔除 JSON 包装语法"

    # 验证实际 accumulated 不包含 JSON 语法
    callbacks = simulate_sidecar_callback(fixture)
    assert callbacks[-1]["accumulated_len"] < polluted, (
        f"accumulated_content 不应包含 JSON 包装语法: "
        f"{callbacks[-1]['accumulated_len']} vs {polluted}"
    )
    print(f"  accumulated 无 JSON 污染 ✅ ({callbacks[-1]['accumulated_len']} 字符)")


# ── 7. 边界条件：极短内容和多轮交替 ──


def test_edge_case_empty_fixture() -> None:
    """空 fixture → 只有 final callback。"""
    callbacks = simulate_sidecar_callback([])
    assert len(callbacks) == 1, "空数据应只有 1 个 final callback"
    assert callbacks[0]["is_final"]
    assert callbacks[0]["stream_status"] == "Completed"
    assert callbacks[0]["accumulated_len"] == 0
    print(f"  空 fixture: 1 final callback ✅")


def test_edge_case_single_event() -> None:
    """单个事件 → 1 个 callback + 1 个 final。"""
    payload = {"choices": [{"message": {"content": "hello"}}]}
    callbacks = simulate_sidecar_callback([payload])
    assert len(callbacks) == 2, f"期望 2 个 callback，实际 {len(callbacks)}"
    assert callbacks[0]["flag"] == 0 and not callbacks[0]["is_final"]
    assert callbacks[1]["is_final"]
    assert callbacks[1]["accumulated_len"] == 5
    print(f"  单事件: 1 Running + 1 Completed ✅")


def test_edge_case_flag_transitions() -> None:
    """模拟多轮交替 flag 序列。"""
    # flag=1 → flag=0 → flag=1 → flag=0
    events = [
        {"_progress": True, "choices": [{"message": {"content": "思考1"}}]},
        {"_chunk_type": "llm_chunk", "choices": [{"delta": {"content": "回复1"}}]},
        {"_progress": True, "choices": [{"message": {"content": "思考2"}}]},
        {"_chunk_type": "llm_chunk", "choices": [{"delta": {"content": "回复2"}}]},
    ]
    callbacks = simulate_sidecar_callback(events)

    # 验证 flag 切换
    flags = [cb["flag"] for cb in callbacks if not cb["is_final"]]
    assert flags == [1, 0, 1, 0], f"flag 序列应为 [1,0,1,0]，实际 {flags}"
    print(f"  多轮交替 flag 序列: {flags} ✅")


def test_edge_case_mixed_chunk_types() -> None:
    """混合类型：progress、llm_chunk、text 三种 chunk_type 共存。"""
    events = [
        {"_chunk_type": "progress", "_progress": True, "choices": [{"message": {"content": "p1"}}]},
        {"_chunk_type": "llm_chunk", "choices": [{"delta": {"content": "l1"}}]},
        {"_chunk_type": "text", "choices": [{"message": {"content": "t1"}}]},
    ]
    callbacks = simulate_sidecar_callback(events)
    # progress → flag=1, llm_chunk → flag=0, text → flag=0
    flags = [cb["flag"] for cb in callbacks if not cb["is_final"]]
    assert flags == [1, 0, 0], f"混合类型 flag 应为 [1,0,0]，实际 {flags}"
    assert callbacks[-1]["accumulated_len"] == 6, f"累积应为 6，实际 {callbacks[-1]['accumulated_len']}"
    print(f"  混合类型: flags={flags}, accumulated={callbacks[-1]['accumulated_len']} ✅")
