"""
Tests for context window compaction — compact_messages, estimate_message_tokens,
and their config helpers.

Verifies:
  - Token estimation is conservative (slightly over-counts)
  - Compaction preserves system messages and newest non-system messages
  - Compaction returns original list when within budget
  - Config functions return sensible defaults
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

MODULE_PATH = Path(__file__).parents[1] / "nodes" / "llm_call.py"
SPEC = importlib.util.spec_from_file_location("outfit_llm_call", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

estimate_message_tokens = MODULE.estimate_message_tokens
compact_messages = MODULE.compact_messages
get_model_max_tokens = MODULE.get_model_max_tokens
get_compaction_threshold = MODULE.get_compaction_threshold


# ═══════════════════════════════════════════════════════════════════
# estimate_message_tokens
# ═══════════════════════════════════════════════════════════════════

def test_estimate_empty_system_message() -> None:
    """Empty system message: structural overhead only."""
    msgs = [{"role": "system", "content": ""}]
    assert estimate_message_tokens(msgs) == 8


def test_estimate_short_message() -> None:
    """Short user message: structural overhead + content/2."""
    msgs = [{"role": "user", "content": "你好"}]
    assert estimate_message_tokens(msgs) == 9  # 8 + 2//2


def test_estimate_longer_message() -> None:
    """Longer message: chars // 2 for mixed Chinese/English."""
    content = "Hello world，这是一个测试消息。"  # 22 chars
    msgs = [{"role": "user", "content": content}]
    expected = 8 + len(content) // 2  # 8 + 11 = 19
    assert estimate_message_tokens(msgs) == expected


def test_estimate_multiple_messages() -> None:
    """Multiple messages are summed independently."""
    msgs = [
        {"role": "system", "content": "You are a helpful assistant."},
        {"role": "user", "content": "hi"},
        {"role": "assistant", "content": "hello"},
    ]
    total = estimate_message_tokens(msgs)
    expected = (8 + 29 // 2) + (8 + 2 // 2) + (8 + 5 // 2)
    assert total == expected


def test_estimate_with_tool_calls() -> None:
    """Tool calls add extra token cost for name and arguments."""
    msgs = [{
        "role": "assistant",
        "content": "",
        "tool_calls": [{
            "id": "call_1",
            "function": {
                "name": "run_skills",
                "arguments": '{"skills": [{"name": "outfit-stylist", "prompt": "test"}]}',
            },
        }],
    }]
    total = estimate_message_tokens(msgs)
    # 8 overhead + 10 for name "run_skills" // 2
    # + 58 for arguments // 3
    assert total == 8 + 10 // 2 + 58 // 3  # 8 + 5 + 19 = 32


def test_estimate_mixed_tool_calls() -> None:
    """Messages without tool_calls handled correctly alongside ones with."""
    msgs = [
        {"role": "user", "content": "hi"},
        {"role": "assistant", "content": "", "tool_calls": []},
        {"role": "assistant", "content": "done"},
    ]
    assert estimate_message_tokens(msgs) > 0


def test_estimate_conservative_overcount() -> None:
    """Estimate should be >= actual tokens for safety."""
    # With pure ASCII, 2 chars/token means we overcount
    # (actual is ~4 chars/token for English). This is intentional.
    msgs = [{"role": "user", "content": "a" * 1000}]
    estimated = estimate_message_tokens(msgs)
    assert estimated >= 1000 // 4  # actual minimum (4 chars/token)
    assert estimated == 8 + 1000 // 2  # our conservative estimate


# ═══════════════════════════════════════════════════════════════════
# compact_messages
# ═══════════════════════════════════════════════════════════════════

def test_compact_below_budget_returns_original() -> None:
    """When message tokens are under budget, returns same list object."""
    msgs = [
        {"role": "system", "content": "sys"},
        {"role": "user", "content": "hello"},
    ]
    result = compact_messages(msgs, max_tokens=10000, compaction_threshold=0.8)
    assert result is msgs  # same object reference = no copy


def test_compact_preserves_system_message() -> None:
    """System message is never dropped."""
    msgs = [
        {"role": "system", "content": "You are helpful. " * 500},   # ~1500 tok
        {"role": "user", "content": "a" * 2000},                     # ~1008 tok
    ]
    budget = 1500  # threshold = 1500 * 0.8 = 1200, system alone is ~1508 > 1200
    result = compact_messages(msgs, max_tokens=budget, compaction_threshold=0.8)
    # System must survive even if it exceeds budget alone
    assert result[0]["role"] == "system"
    assert len(result) >= 1


def test_compact_drops_oldest_non_system_first() -> None:
    """Newest messages kept, oldest dropped when over budget."""
    msgs = [
        {"role": "system", "content": "sys"},
        {"role": "user", "content": "old message that should be dropped"},
        {"role": "assistant", "content": "old reply that should be dropped"},
        {"role": "user", "content": "new question"},
    ]
    # Budget tight enough to hold system + newest user only
    tight_budget = 50
    result = compact_messages(msgs, max_tokens=tight_budget, compaction_threshold=0.5)
    # budget = 25 tok → system(10) + latest msg only
    assert result[0]["role"] == "system"
    assert result[-1]["content"] == "new question"
    assert len(result) == 2  # system + newest


def test_compact_multiple_system_messages_preserved() -> None:
    """All system messages preserved when there are multiple."""
    msgs = [
        {"role": "system", "content": "Framework protocol: be concise."},
        {"role": "system", "content": "Business rules: always ask for city."},
        {"role": "user", "content": "a" * 100},
        {"role": "assistant", "content": "b" * 100},
        {"role": "user", "content": "c" * 100},
    ]
    # Use a budget that fits system + newest messages
    # 2 system messages ≈ 49 tok, user("c"*100) ≈ 58 tok → total 107 tok
    budget = 250  # threshold = 250 * 0.5 = 125 tok
    result = compact_messages(msgs, max_tokens=budget, compaction_threshold=0.5)
    system_count = sum(1 for m in result if m["role"] == "system")
    assert system_count == 2, "all system messages must survive"
    # Newest user message should be kept
    user_msgs = [m for m in result if m["role"] == "user"]
    assert len(user_msgs) >= 1
    assert user_msgs[-1]["content"] == "c" * 100


def test_compact_exact_budget_no_compression() -> None:
    """Exactly at threshold uses same object."""
    msgs = [{"role": "system", "content": "a" * 70}]  # 8 + 35 = 43 tok
    budget = 54  # threshold ≈ 43 tok (43/54 ≈ 0.796)
    result = compact_messages(msgs, max_tokens=budget, compaction_threshold=0.8)
    assert result is msgs


def test_compact_restores_chronological_order() -> None:
    """After compaction, non-system messages are in original chronological order."""
    msgs = [
        {"role": "system", "content": "sys"},
        {"role": "user", "content": "first"},
        {"role": "assistant", "content": "reply1"},
        {"role": "user", "content": "second"},
        {"role": "assistant", "content": "reply2"},
    ]
    # Budget that fits system + some non-system messages
    budget = 100
    threshold = 0.4  # budget = 40 tok
    result = compact_messages(msgs, max_tokens=budget, compaction_threshold=threshold)
    roles = [m["role"] for m in result]
    assert roles[0] == "system"
    # Non-system roles should preserve original order (e.g. user before assistant)
    non_system_roles = [r for r in roles if r != "system"]
    if len(non_system_roles) >= 2:
        # "first" (user) comes before "reply1" (assistant) in original
        user_idx = non_system_roles.index("user")
        asst_idx = non_system_roles.index("assistant")
        assert user_idx < asst_idx, "user messages must precede later assistant messages"


# ═══════════════════════════════════════════════════════════════════
# Config helpers
# ═══════════════════════════════════════════════════════════════════

def test_get_model_max_tokens_default() -> None:
    """No env var → returns 128000."""
    val = get_model_max_tokens()
    assert val == 128000


def test_get_compaction_threshold_default() -> None:
    """No env var → returns 0.8."""
    val = get_compaction_threshold()
    assert val == 0.8


def test_get_compaction_threshold_clamped(monkeypatch) -> None:
    """Values outside 0.1–1.0 are clamped."""
    monkeypatch.setenv("AF_COMPACTION_THRESHOLD", "2.5")
    assert get_compaction_threshold() == 1.0

    monkeypatch.setenv("AF_COMPACTION_THRESHOLD", "-1.0")
    assert get_compaction_threshold() == 0.1


def test_get_compaction_threshold_invalid_fallback(monkeypatch) -> None:
    """Invalid string falls back to default 0.8."""
    monkeypatch.setenv("AF_COMPACTION_THRESHOLD", "not-a-number")
    assert get_compaction_threshold() == 0.8


def test_get_model_max_tokens_env_override(monkeypatch) -> None:
    """AF_MODEL_MAX_TOKENS env var overrides default."""
    monkeypatch.setenv("AF_MODEL_MAX_TOKENS", "8192")
    assert get_model_max_tokens() == 8192


def test_get_model_max_tokens_invalid_fallback(monkeypatch) -> None:
    """Invalid AF_MODEL_MAX_TOKENS falls back to 128000."""
    monkeypatch.setenv("AF_MODEL_MAX_TOKENS", "bad")
    assert get_model_max_tokens() == 128000
