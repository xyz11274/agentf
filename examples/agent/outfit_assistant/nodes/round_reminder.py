"""
Round reminder — shared budget awareness module.

All nodes that run iterative LLM loops (skill_executor, coordinator_decide)
use :func:`build_round_reminder` to inject a ``<system-reminder>`` tag
(with role ``"user"``) before each LLM call, so the model can make
informed convergence decisions.

Usage::

    from round_reminder import build_round_reminder

    msg = build_round_reminder(
        round_i=current_iteration,
        max_rounds=config.max_rounds,
        elapsed=time.time() - turn_start,
        node_timeout_sec=240,
        custom_hint="优先输出已有结论",
    )
    if msg:
        messages.append(msg)
"""

from __future__ import annotations


def build_round_reminder(
    *,
    round_i: int,
    max_rounds: int,
    elapsed: float = 0.0,
    node_timeout_sec: int = 240,
    custom_hint: str = "",
) -> dict[str, str] | None:
    """Build a reminder message (role ``"user"``) with a ``<system-reminder>``
    tag when budget is tight.

    Returns ``None`` when no reminder is needed (early rounds with plenty
    of budget).  Two trigger thresholds (independent; fires when **any** is met):

    * **Round threshold** — last 3 rounds (``round_i >= max_rounds - 3``).
    * **Time threshold** — elapsed > 70% of ``node_timeout_sec``.

    ``custom_hint`` is appended to the closing instruction when provided,
    allowing each caller to override the convergence guidance (e.g.
    ``"优先输出已有结论"`` or ``"给出结论或重新委派"``).
    """
    if round_i < max_rounds - 3 and elapsed <= node_timeout_sec * 0.7:
        return None

    parts: list[str] = []
    if round_i >= max_rounds - 3:
        remaining = max_rounds - round_i
        parts.append(f"剩余 {remaining} 轮")
    if elapsed > node_timeout_sec * 0.7:
        parts.append(f"总耗时 {elapsed:.0f}s（接近 {node_timeout_sec}s 节点超时）")

    closing = custom_hint or "本轮请收敛输出，优先给出已有结论，不要再发起新的工具调用"
    text = f"<system-reminder>{'，'.join(parts)}。{closing}</system-reminder>"
    return {"role": "user", "content": text}
