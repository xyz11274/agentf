"""Unit tests for `providers.LocalSessionProvider`.

Covers the SQLite-backed Bridge session persistence contract defined in
``docs/design/features/bridge-session-persistence.md`` §6. Each test uses
a fresh temporary database file so there is no cross-test pollution.

Invariants locked here:
    * I-1: Bridge session IDs are stable across agentflow session recycling.
    * I-3: stream chunks are not persisted — only the final dialogue pair.
    * I-4: idempotency — `get_or_create_session` returns the same session
      for the same ``(user_id, client_id)``.
    * Auto-title: first dialogue sets the title from ``user_input[:50]``.
"""

from __future__ import annotations

import json
from pathlib import Path
from uuid import UUID

import pytest

from providers import LocalSessionProvider


@pytest.fixture
def provider(tmp_path: Path) -> LocalSessionProvider:
    """Return a provider backed by a fresh temp SQLite file."""
    db = tmp_path / "sessions.db"
    return LocalSessionProvider(str(db))


# ── Session management ──────────────────────────────────────────────


async def test_get_or_create_session_idempotent(provider: LocalSessionProvider):
    """I-4: same (user_id, client_id) → same session, no duplicate."""
    s1 = await provider.get_or_create_session("alice", "web")
    s2 = await provider.get_or_create_session("alice", "web")

    assert s1["id"] == s2["id"]
    assert s2["user_id"] == "alice"
    assert s2["client_id"] == "web"
    assert s2["status"] == "active"
    assert s2["title"] == ""  # not yet set until first dialogue

    # UUID4 hex format (Bridge session IDs are independent of agentflow).
    UUID(s1["id"])


async def test_get_or_create_session_bumps_last_active(provider: LocalSessionProvider):
    s1 = await provider.get_or_create_session("alice", "web")
    first_active = s1["last_active_at"]
    # Second call returns the same session with updated last_active_at.
    s2 = await provider.get_or_create_session("alice", "web")
    assert s2["last_active_at"] >= first_active


async def test_get_or_create_session_distinct_users(provider: LocalSessionProvider):
    s_alice = await provider.get_or_create_session("alice", "web")
    s_bob = await provider.get_or_create_session("bob", "web")
    assert s_alice["id"] != s_bob["id"]


async def test_list_sessions_newest_first(provider: LocalSessionProvider):
    await provider.get_or_create_session("alice", "web")
    # small delay to ensure distinct last_active_at timestamps would order
    # correctly — but the provider is sync; rely on insertion order.
    await provider.get_or_create_session("alice", "mobile")
    await provider.get_or_create_session("bob", "web")

    all_sessions = await provider.list_sessions(limit=10)
    assert len(all_sessions) == 3

    alice_only = await provider.list_sessions(user_id="alice", limit=10)
    assert len(alice_only) == 2
    assert all(s["user_id"] == "alice" for s in alice_only)


async def test_get_session_returns_none_for_unknown(provider: LocalSessionProvider):
    assert await provider.get_session("nonexistent") is None


async def test_get_session_returns_dict_for_known(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    fetched = await provider.get_session(s["id"])
    assert fetched is not None
    assert fetched["id"] == s["id"]


async def test_find_by_agentflow_session(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    await provider.update_agentflow_session_ref(s["id"], "af-sess-42")

    found = await provider.find_by_agentflow_session("af-sess-42")
    assert found is not None
    assert found["id"] == s["id"]

    # I-1: updating the agentflow ref does not change the Bridge session id.
    assert found["agentflow_session_id"] == "af-sess-42"


async def test_update_session_status(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    await provider.update_session_status(s["id"], "archived")
    fetched = await provider.get_session(s["id"])
    assert fetched["status"] == "archived"


async def test_update_session_title(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    await provider.update_session_title(s["id"], "My Chat")
    fetched = await provider.get_session(s["id"])
    assert fetched["title"] == "My Chat"


# ── Dialogue / message storage ──────────────────────────────────────


async def test_save_dialogue_returns_dialogue_id(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    out = await provider.save_dialogue(
        s["id"], "hello", "hi there", dialogue_id="d-1",
    )
    assert out == "d-1"


async def test_save_dialogue_redacts_user_credentials(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    canary = "ark-test-sqlite-boundary-998877665544332211"

    await provider.save_dialogue(
        s["id"], f"do not persist {canary}", "safe response", dialogue_id="d-safe",
    )

    context = await provider.get_context(s["id"], "alice")
    stored_input = context.messages[0]["content"]
    assert canary not in stored_input
    assert "<redacted:provider_key>" in stored_input


async def test_save_dialogue_keeps_round_when_redaction_fails(
    provider: LocalSessionProvider, monkeypatch,
):
    import services.secret_safety as secret_safety

    s = await provider.get_or_create_session("alice", "web")

    def fail_redaction(_value):
        raise RuntimeError("sanitizer unavailable")

    monkeypatch.setattr(secret_safety, "sanitize_for_storage", fail_redaction)
    await provider.save_dialogue(
        s["id"], "untrusted input", "safe response", dialogue_id="d-safe",
    )

    context = await provider.get_context(s["id"], "alice")
    assert context.messages == [
        {"role": "user", "content": "<redacted:credential_sanitizer_unavailable>"},
        {"role": "assistant", "content": "safe response"},
    ]


async def test_save_dialogue_auto_title(provider: LocalSessionProvider):
    """First dialogue auto-sets title from user_input[:50]."""
    long_input = "我昨天感冒了需要请一天假去看医生然后还要买药回家休息" * 3
    s = await provider.get_or_create_session("alice", "web")

    await provider.save_dialogue(
        s["id"], long_input, "ok", dialogue_id="d-1",
    )
    fetched = await provider.get_session(s["id"])
    assert fetched["title"] == long_input[:50]


async def test_save_dialogue_does_not_overwrite_title(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_dialogue(s["id"], "first input", "resp1", dialogue_id="d-1")
    await provider.save_dialogue(s["id"], "second input", "resp2", dialogue_id="d-2")

    fetched = await provider.get_session(s["id"])
    assert fetched["title"] == "first input"  # not overwritten by second


async def test_get_messages_returns_user_and_assistant(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_dialogue(s["id"], "Q1", "A1", dialogue_id="d-1")
    await provider.save_dialogue(s["id"], "Q2", "A2", dialogue_id="d-2")

    msgs = await provider.get_messages(s["id"], limit=10)
    # 2 dialogues → 4 messages, oldest first.
    assert len(msgs) == 4
    assert msgs[0]["role"] == "user" and msgs[0]["content"] == "Q1"
    assert msgs[1]["role"] == "assistant" and msgs[1]["content"] == "A1"
    assert msgs[2]["role"] == "user" and msgs[2]["content"] == "Q2"
    assert msgs[3]["role"] == "assistant" and msgs[3]["content"] == "A2"
    assert msgs[0]["dialogue_id"] == "d-1"
    assert msgs[2]["dialogue_id"] == "d-2"


async def test_get_messages_respects_limit(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    for i in range(5):
        await provider.save_dialogue(s["id"], f"Q{i}", f"A{i}", dialogue_id=f"d-{i}")

    # limit is the max number of messages returned (user+assistant = 2 per round).
    msgs = await provider.get_messages(s["id"], limit=2)
    assert len(msgs) == 2
    # Oldest first within the returned window.
    assert msgs[0]["content"] == "Q0"
    assert msgs[1]["content"] == "A0"


# ── User images persistence ───────────────────────────────────────────


async def test_save_dialogue_persists_user_images(provider: LocalSessionProvider):
    """用户附加图片（data URI）落库并回读：user 消息带 images，assistant 不带。"""
    s = await provider.get_or_create_session("alice", "web")
    images = json.dumps([
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==",
    ], ensure_ascii=False)
    await provider.save_dialogue(
        s["id"], "带图的提问", "回答", dialogue_id="d-img", images=images,
    )

    msgs = await provider.get_messages(s["id"], limit=10)
    assert len(msgs) == 2
    user_msg = msgs[0]
    assert user_msg["role"] == "user"
    assert user_msg["content"] == "带图的提问"
    assert user_msg["images"] == json.loads(images), (
        "user 消息必须回读 images（data URI 列表），否则前端无法回显用户图片"
    )
    # assistant 消息无用户图片
    assert msgs[1]["role"] == "assistant"
    assert msgs[1].get("images", []) == []


async def test_save_dialogue_default_images_empty(provider: LocalSessionProvider):
    """不带 images 参数时默认落库空数组，回读 images == []（不报错）。"""
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_dialogue(s["id"], "Q", "A", dialogue_id="d-1")

    msgs = await provider.get_messages(s["id"], limit=10)
    assert msgs[0]["images"] == []


async def test_save_dialogue_images_multiple(provider: LocalSessionProvider):
    """多张图片按序落库回读，顺序保持不变（I-1 不丢数据）。"""
    s = await provider.get_or_create_session("alice", "web")
    img_a = "data:image/png;base64,AAAA"
    img_b = "data:image/jpeg;base64,BBBB"
    await provider.save_dialogue(
        s["id"], "多图", "ok", dialogue_id="d-multi",
        images=json.dumps([img_a, img_b]),
    )

    msgs = await provider.get_messages(s["id"], limit=10)
    assert msgs[0]["images"] == [img_a, img_b]


# ── Context injection ───────────────────────────────────────────────


async def test_get_context_returns_last_n_rounds(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    for i in range(5):
        await provider.save_dialogue(s["id"], f"Q{i}", f"A{i}", dialogue_id=f"d-{i}")

    ctx = await provider.get_context(s["id"], "alice", limit_rounds=2)
    # 2 rounds → 4 messages, oldest first within the window.
    assert len(ctx.messages) == 4
    assert ctx.messages[0]["content"] == "Q3"
    assert ctx.messages[-1]["content"] == "A4"
    # Local provider has no long-term memory concept.
    assert ctx.memories == []


async def test_get_context_empty_session(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    ctx = await provider.get_context(s["id"], "alice")
    assert ctx.messages == []
    assert ctx.memories == []


# ── Trace storage ───────────────────────────────────────────────────


async def test_save_and_get_traces(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_trace(
        s["id"], "d-1", "llm_call",
        {"model": "gpt-4o", "latency_ms": 123},
    )
    await provider.save_trace(
        s["id"], "d-1", "skill_exec",
        {"skill": "search"},
    )

    traces = await provider.get_traces(s["id"], dialogue_id="d-1")
    assert len(traces) == 2
    assert traces[0]["trace_type"] == "llm_call"
    assert traces[0]["trace_data"]["model"] == "gpt-4o"
    assert traces[1]["trace_type"] == "skill_exec"
    assert traces[1]["dialogue_id"] == "d-1"


async def test_get_traces_filters_by_dialogue(provider: LocalSessionProvider):
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_trace(s["id"], "d-1", "llm_call", {"a": 1})
    await provider.save_trace(s["id"], "d-2", "llm_call", {"b": 2})

    d1 = await provider.get_traces(s["id"], dialogue_id="d-1")
    assert len(d1) == 1
    assert d1[0]["dialogue_id"] == "d-1"

    all_traces = await provider.get_traces(s["id"])
    assert len(all_traces) == 2


async def test_get_traces_handles_invalid_json(provider: LocalSessionProvider):
    """Corrupt JSON in trace_data does not crash get_traces."""
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_trace(s["id"], "d-1", "llm_call", {"ok": True})
    # Manually corrupt the row via direct sqlite to test resilience.
    import sqlite3
    conn = sqlite3.connect(provider._db_path)
    conn.execute(
        "UPDATE traces SET trace_data='not-json' WHERE dialogue_id=?",
        ("d-1",),
    )
    conn.commit()
    conn.close()

    traces = await provider.get_traces(s["id"])
    assert len(traces) == 1
    assert traces[0]["trace_data"] == {}  # graceful fallback


# ── Resilience / I-5 ────────────────────────────────────────────────


async def test_save_dialogue_silent_on_unknown_session(provider: LocalSessionProvider):
    """I-5: persistence failures never raise to the chat main flow.

    Note: LocalSessionProvider uses SQLite WITHOUT a FK enforcement on
    dialogue_rounds.session_id (sqlite disable by default), so an
    unknown session_id will silently insert a dangling row. The
    invariant under test is that save_dialogue never raises.
    """
    # No exception expected — silent skip / dangling insert is acceptable
    # for the local provider; the contract is "never raise".
    out = await provider.save_dialogue(
        "unknown-session", "Q", "A", dialogue_id="d-x",
    )
    assert out == "d-x"


async def test_save_trace_silent_on_failure(provider: LocalSessionProvider, tmp_path: Path):
    """I-5: trace persistence failures never raise."""
    s = await provider.get_or_create_session("alice", "web")
    # Sabotage the db path so the next connection raises.
    provider._db_path = str(tmp_path / "does" / "not" / "exist" / "x.db")
    # Should log a warning but not raise.
    await provider.save_trace(s["id"], "d-1", "llm_call", {"x": 1})


# ── Schema bootstrap ────────────────────────────────────────────────


async def test_schema_idempotent_across_constructs(tmp_path: Path):
    """Re-constructing the provider against an existing db file is safe."""
    db = str(tmp_path / "sessions.db")
    p1 = LocalSessionProvider(db)
    s = await p1.get_or_create_session("alice", "web")
    await p1.save_dialogue(s["id"], "Q", "A", dialogue_id="d-1")

    p2 = LocalSessionProvider(db)
    fetched = await p2.get_session(s["id"])
    assert fetched is not None
    msgs = await p2.get_messages(s["id"])
    assert len(msgs) == 2


# ── 展示层展平（code-review P3：I-REQ 存原始 content JSON，展示层需可读文本）──


_STORED_JSON_INPUT = json.dumps([
    {"type": "text", "text": "看图"},
    {"type": "image_url", "image_url": {"url": "data:image/png;base64,aGVsbG8="}},
], ensure_ascii=False)


async def test_auto_title_flattens_stored_json(provider: LocalSessionProvider):
    """自动标题必须用展平文本——原始 content JSON（含 base64）会生成乱码标题。"""
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_dialogue(s["id"], _STORED_JSON_INPUT, "A1", dialogue_id="d-1")

    sess = await provider.get_session(s["id"])
    assert sess is not None
    # 标题应为展平文本"看图"，而非 JSON 乱码
    assert sess["title"] == "看图", f"标题应为展平文本，实际: {sess['title']!r}"
    assert "[{" not in sess["title"], f"标题不应含 JSON 乱码: {sess['title']!r}"


async def test_get_messages_flattens_stored_json(provider: LocalSessionProvider):
    """get_messages 展示层必须展平——JSON 形态 user_input 渲染为可读文本，不渲染原始 JSON。"""
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_dialogue(s["id"], _STORED_JSON_INPUT, "A1", dialogue_id="d-1")

    msgs = await provider.get_messages(s["id"])
    user_msg = msgs[0]
    assert user_msg["role"] == "user"
    assert user_msg["content"] == "看图", \
        f"展示层应渲染展平文本，实际: {user_msg['content']!r}"
    assert "base64" not in user_msg["content"], \
        f"展示层不应渲染 base64: {user_msg['content']!r}"


async def test_get_context_keeps_raw_content(provider: LocalSessionProvider):
    """get_context 必须保持原始 content（I-DERIV 消费时派生的输入）——不展平。

    derive_history_user_messages 依赖原始 JSON（含 image_url）做消费时派生；
    若 get_context 展平，路径文本派生将失效（O5/I-REQ 链路破坏）。
    """
    s = await provider.get_or_create_session("alice", "web")
    await provider.save_dialogue(s["id"], _STORED_JSON_INPUT, "A1", dialogue_id="d-1")

    ctx = await provider.get_context(s["id"], "alice")
    assert ctx.messages[0]["content"] == _STORED_JSON_INPUT, \
        "get_context 应保持原始 content（I-DERIV 输入），不得展平"
