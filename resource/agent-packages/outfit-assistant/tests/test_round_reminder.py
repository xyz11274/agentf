from __future__ import annotations

from agentflow_runtime import round_reminder as MODULE


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


# ---------------------------------------------------------------------------
# always-mode (added 2026-07-06): every-round budget telemetry
# ---------------------------------------------------------------------------


def test_always_returns_reminder_on_first_round():
    """always=True fires on round 0 with full telemetry — no None skip."""
    msg = build_round_reminder(
        round_i=0, max_rounds=10, elapsed=5, node_timeout_sec=240, always=True,
    )
    assert msg is not None
    assert msg["role"] == "user"


def test_always_includes_remaining_and_used_metrics():
    """always-mode reminder must carry both axes (rounds + time) telemetry.

    不变量 I-3 元数据一致性：LLM 收到的预算信息必须与实际消耗一致。
    """
    msg = build_round_reminder(
        round_i=4, max_rounds=10, elapsed=50, node_timeout_sec=240, always=True,
    )
    assert msg is not None
    content = msg["content"]
    # 剩余轮次（5 = 10 - 5）
    assert "剩余 5/10 轮" in content
    # 已用时间（50s）
    assert "已用 50/240s" in content
    # 超时上限
    assert "节点超时上限 240s" in content


def test_always_escalates_closing_instruction_with_budget_consumption():
    """Closing instruction escalates at 50%/70% consumption thresholds.

    不变量 I-2 边界硬约束：三个收敛档位必须按时切换。
    """
    # <50% on both axes → permissive
    msg_early = build_round_reminder(
        round_i=1, max_rounds=10, elapsed=10, node_timeout_sec=240, always=True,
    )
    assert "可继续探索工具" in msg_early["content"]

    # >=50% either → lean toward收敛
    msg_mid = build_round_reminder(
        round_i=5, max_rounds=10, elapsed=10, node_timeout_sec=240, always=True,
    )
    assert "倾向收敛" in msg_mid["content"]

    # >=70% either OR last 3 rounds → force收敛
    msg_late = build_round_reminder(
        round_i=8, max_rounds=10, elapsed=10, node_timeout_sec=240, always=True,
    )
    assert "本轮必须收敛" in msg_late["content"]


def test_always_custom_hint_overrides_escalation():
    """custom_hint 叠加而非覆盖升档：custom_hint 在前 + escalation 在后。

    设计意图（2026-07-06 review 后改）：让 LLM 同时看到"调用方任务语义 + 预算紧张程度"
    两层信号。之前 custom_hint 一旦提供就完全屏蔽升档，导致 skill_executor 永远收到
    同一句收敛话而升档白写（70% 超时和 5% 超时下 LLM 收到同一句）。
    """
    msg = build_round_reminder(
        round_i=8, max_rounds=10, elapsed=200, node_timeout_sec=240,
        always=True, custom_hint="本轮必须交还结果给 coordinator",
    )
    assert msg is not None
    # custom_hint 在前
    assert "本轮必须交还结果给 coordinator" in msg["content"]
    # 升档（70% 超时 → force收敛）紧跟其后叠加出现
    assert "本轮必须收敛" in msg["content"]


def test_always_includes_progress_percentage():
    """always-mode reminder includes time progress %."""
    msg = build_round_reminder(
        round_i=0, max_rounds=10, elapsed=120, node_timeout_sec=240, always=True,
    )
    assert msg is not None
    # 120/240 = 50%
    assert "(50%)" in msg["content"]


# ---------------------------------------------------------------------------
# _detect_tool_call_loop threshold boundary (added 2026-07-06 review)
# ---------------------------------------------------------------------------


def test_detect_loop_threshold_one_fires_on_first_occurrence():
    """threshold=1 表示"当前轮出现即触发"，无需 history。

    不变量 I-2 边界硬约束：阈值边界语义必须与文档一致。
    """
    import importlib.util
    from pathlib import Path
    skill_exec_path = Path(__file__).parents[1] / "nodes" / "skill_executor.py"
    spec = importlib.util.spec_from_file_location("outfit_skill_exec", skill_exec_path)
    skill_exec = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(skill_exec)
    _detect = skill_exec._detect_tool_call_loop
    # signature 构造是 (name, args_str) tuple，history 元素是 list[signature]，
    # 即 list[list[tuple]]——每轮的 signatures 列表。
    sig = ("weather", '{"city":"北京"}')

    # threshold=1 → 当前轮即触发，history 空
    assert _detect([], [sig], threshold=1) is True
    # threshold=2 → 需 history 里 1 轮 match（当前轮 + hist 尾 1 轮 = 总 2）
    assert _detect([], [sig], threshold=2) is False
    assert _detect([[sig]], [sig], threshold=2) is True
    # threshold=3 → 需 hist 尾部连续 2 轮全同 + 当前 = 总 3
    assert _detect([[sig], [sig]], [sig], threshold=3) is True
    # hist 尾部有异类 → break，不计数
    other_sig = ("profile", '{"user":"u"}')
    assert _detect([[sig], [other_sig]], [sig], threshold=3) is False
