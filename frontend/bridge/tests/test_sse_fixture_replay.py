"""
SSE 流式响应回放测试 — 验证 sidecar 的 flag 路由逻辑。

从真实 Bridge 流式响应保存的 fixture 数据中回放 SSE 事件，
模拟 sidecar 的 flag 判断逻辑（_progress → flag=1），验证：
  1. flag 路由规则的正确性
  2. 交替模式（flag=1/0 多次切换）
  3. 非 progress 消息的 flag=0 默认值
"""
import json
from pathlib import Path

import pytest

FIXTURE_PATH = Path(__file__).resolve().parent / "fixtures" / "sse_capture.jsonl"


def _load_fixture() -> list[dict]:
    if not FIXTURE_PATH.exists():
        pytest.skip(f"SSE fixture not found: {FIXTURE_PATH}")
    events: list[dict] = []
    with open(FIXTURE_PATH) as f:
        for line in f:
            line = line.strip()
            if line:
                events.append(json.loads(line))
    return events


def _is_progress(payload: dict) -> bool:
    """模拟 sidecar 的 is_progress 判断 (sidecar_router.cpp L336-337)。"""
    return bool(payload.get("_progress", False)) or payload.get("_chunk_type", "") == "progress"


def _get_content(payload: dict) -> str:
    """提取 SSE payload 中的文本内容。"""
    choices = payload.get("choices", [])
    if not choices:
        return ""
    choice = choices[0]
    delta = choice.get("delta", {})
    if isinstance(delta, dict):
        content = delta.get("content", "")
        if content:
            return content
    msg = choice.get("message", {})
    if isinstance(msg, dict):
        return msg.get("content", "")
    return ""


# ── 测试用例 ──


def test_fixture_exists_and_nonempty() -> None:
    events = _load_fixture()
    assert events, "SSE fixture 不应为空"
    assert len(events) >= 10, f"期望 >= 10 个事件，实际 {len(events)}"
    print(f"  总事件数: {len(events)}")


def test_flag_routing_matches_progress() -> None:
    events = _load_fixture()
    progress_count = sum(1 for e in events if _is_progress(e))
    non_progress_count = sum(1 for e in events if not _is_progress(e))
    assert progress_count > 0, f"应有 progress 事件，实际 {progress_count}"
    assert non_progress_count > 0, f"应有非 progress 事件，实际 {non_progress_count}"
    print(f"  progress (→flag=1): {progress_count}")
    print(f"  non-progress (→flag=0): {non_progress_count}")


def test_alternating_flag_blocks() -> None:
    events = _load_fixture()
    flag_blocks: list[tuple[int, int]] = []
    for e in events:
        flag = 1 if _is_progress(e) else 0
        if not flag_blocks or flag_blocks[-1][0] != flag:
            flag_blocks.append((flag, 1))
        else:
            flag_blocks[-1] = (flag, flag_blocks[-1][1] + 1)
    assert len(flag_blocks) >= 3, (
        f"期望至少 3 次 flag 切换，实际 {len(flag_blocks)} 块"
    )
    non_progress_blocks = [b for b in flag_blocks if b[0] == 0]
    assert len(non_progress_blocks) >= 1, "应有至少一个 flag=0 块"
    print(f"  flag 切换次数: {len(flag_blocks)}，flag=0 块: {len(non_progress_blocks)}")


def test_content_extraction() -> None:
    events = _load_fixture()
    all_content = ""
    empty = 0
    for e in events:
        text = _get_content(e)
        if not text:
            empty += 1
        all_content += text
    assert len(all_content) > 0, "提取内容不应为空"
    assert empty / len(events) < 0.5, f"空内容比例过高: {empty}/{len(events)}"
    print(f"  总内容: {len(all_content)} 字符，空内容: {empty}/{len(events)}")


def test_llm_chunk_has_delta_content() -> None:
    events = _load_fixture()
    llm_chunks = [e for e in events if e.get("_chunk_type") == "llm_chunk"]
    assert len(llm_chunks) > 0, "应有 llm_chunk"
    for chunk in llm_chunks[:5]:
        choices = chunk.get("choices", [])
        assert choices, f"缺少 choices: {list(chunk.keys())[:3]}"
        delta = choices[0].get("delta", {})
        assert "content" in delta, f"delta 缺少 content: {delta}"
    print(f"  llm_chunk ✅ ({len(llm_chunks)} 个)")


def test_progress_has_message_content() -> None:
    events = _load_fixture()
    text_events = [e for e in events if _is_progress(e) or e.get("_chunk_type") != "llm_chunk"]
    if not text_events:
        pytest.skip("无 text/progress 事件")
    for evt in text_events[:5]:
        choices = evt.get("choices", [])
        assert choices, f"缺少 choices"
        msg = choices[0].get("message", {})
        assert "content" in msg, f"message 缺少 content"
    print(f"  progress/text ✅ ({len(text_events)} 个)")
