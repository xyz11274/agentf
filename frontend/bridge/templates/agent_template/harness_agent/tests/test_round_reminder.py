import importlib.util
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "nodes" / "round_reminder.py"
SPEC = importlib.util.spec_from_file_location("outfit_round_reminder", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

build_round_reminder = MODULE.build_round_reminder


def test_early_round_returns_none():
    """Early rounds with plenty of budget → no reminder."""
    msg = build_round_reminder(round_i=1, max_rounds=20)
    assert msg is None


def test_last_three_rounds_include_remaining_count():
    """Last 3 rounds trigger reminder with remaining-rounds info."""
    msg = build_round_reminder(round_i=18, max_rounds=20)
    assert msg is not None
    assert "剩余 2 轮" in msg["content"]
    assert msg["role"] == "user"


def test_time_threshold_fires_with_elapsed_messages():
    """>70% of node_timeout_sec elapsed → reminder with timeout info."""
    msg = build_round_reminder(
        round_i=5, max_rounds=20, elapsed=180, node_timeout_sec=240,
    )
    assert msg is not None
    assert "接近 240s" in msg["content"]


def test_both_thresholds_combined():
    """Both round and time thresholds met → combined reminder."""
    msg = build_round_reminder(
        round_i=18, max_rounds=20, elapsed=200, node_timeout_sec=240,
    )
    assert msg is not None
    assert "剩余" in msg["content"]
    assert "接近" in msg["content"]


def test_custom_hint_appears_in_content():
    """custom_hint overrides default convergence guidance."""
    msg = build_round_reminder(
        round_i=18, max_rounds=20, custom_hint="必须给出结论",
    )
    assert msg is not None
    assert "必须给出结论" in msg["content"]
    assert "不要再发起" not in msg["content"]


def test_max_rounds_one_round_fires_immediately():
    """max_rounds=1 → first round (i=0) triggers reminder."""
    msg = build_round_reminder(round_i=0, max_rounds=1)
    assert msg is not None
    assert "剩余 1 轮" in msg["content"]


def test_elapsed_exact_boundary_no_reminder():
    """elapsed exactly at 70% threshold → not >, so no time reminder."""
    msg = build_round_reminder(
        round_i=1, max_rounds=10, elapsed=168, node_timeout_sec=240,
    )
    # elapsed=168 = 240*0.7 exactly → not > → no time-based trigger
    # round_i=1 < 10-3=7 → no round-based trigger
    assert msg is None
