"""
Round reminder — shared budget awareness module.

All nodes that run iterative LLM loops (skill_executor, coordinator_decide)
use :func:`build_round_reminder` to inject a ``<system-reminder>`` tag
(with role ``"user"``) before each LLM call, so the model can make
informed convergence decisions.

Usage::

    from round_reminder import build_round_reminder

    # Per-round budget awareness (always inject, so LLM can plan convergence):
    msg = build_round_reminder(
        round_i=current_iteration,
        max_rounds=config.max_rounds,
        elapsed=time.time() - turn_start,
        node_timeout_sec=240,
        always=True,                      # inject every round, not just last 3
        custom_hint="优先输出已有结论",
    )
    if msg:
        messages.append(msg)

Design rationale for ``always=True`` (added 2026-07-06 after runway incident):
早期实现只在"最后 3 轮"或"超时 70%"才触发，导致 LLM 在前 N-3 轮毫无预算
感知，反复调用 tool 不给最终答案（实测发生过 16 轮调 weather/get_user_profile
耗光 max_rounds 才停的 runway 循环）。让 LLM 从第 1 轮就看到剩余预算，能
显著降低陷入"调 tool 不收敛"的概率——这是与 skill_executor.py 里的硬约束
（node_timeout break + tool_call_loop detection）互为兜底的**软提示**层。
"""

from __future__ import annotations


def build_round_reminder(
    *,
    round_i: int,
    max_rounds: int,
    elapsed: float = 0.0,
    node_timeout_sec: int = 240,
    custom_hint: str = "",
    always: bool = False,
) -> dict[str, str] | None:
    """Build a reminder message (role ``"user"``) with a ``<system-reminder>``
    tag carrying round/time budget awareness.

    Two trigger modes:

    * **Threshold mode** (``always=False``, default, backward-compatible):
      only fires when budget is tight — last 3 rounds
      (``round_i >= max_rounds - 3``) OR elapsed > 70% of ``node_timeout_sec``.
      Returns ``None`` for early rounds with plenty of budget.

    * **Always mode** (``always=True``): injects a reminder **every** round,
      carrying full budget telemetry (used/remaining rounds + time + progress %).
      Use this for nodes prone to runway tool-call loops (skill_executor) so
      the LLM can plan convergence from round 1 instead of only panicking in
      the last 3 rounds.

    Budget telemetry in always-mode (so LLM can make precise convergence calls):

        剩余 7/10 轮，已用 45/240s (19%)，节点超时上限 240s

    When both thresholds are crossed in threshold-mode, both parts fire.
    ``custom_hint`` overrides the convergence closing instruction when provided,
    allowing each caller to tailor guidance (e.g. ``"优先输出已有结论"``
    for skill_executor vs ``"给出结论或重新委派"`` for coordinator).
    """
    if not always:
        # Threshold mode (backward-compatible): skip when budget is loose.
        if round_i < max_rounds - 3 and elapsed <= node_timeout_sec * 0.7:
            return None

    if always:
        # Full telemetry every round — give LLM the data to plan convergence.
        return _build_always_reminder(
            round_i=round_i,
            max_rounds=max_rounds,
            elapsed=elapsed,
            node_timeout_sec=node_timeout_sec,
            custom_hint=custom_hint,
        )

    # Threshold mode — only include parts that crossed their threshold.
    parts: list[str] = []
    if round_i >= max_rounds - 3:
        remaining = max_rounds - round_i
        parts.append(f"剩余 {remaining} 轮")
    if elapsed > node_timeout_sec * 0.7:
        parts.append(f"总耗时 {elapsed:.0f}s（接近 {node_timeout_sec}s 节点超时）")

    closing = custom_hint or "本轮请收敛输出，优先给出已有结论，不要再发起新的工具调用"
    text = f"<system-reminder>{'，'.join(parts)}。{closing}</system-reminder>"
    return {"role": "user", "content": text}


def _build_always_reminder(
    *,
    round_i: int,
    max_rounds: int,
    elapsed: float,
    node_timeout_sec: int,
    custom_hint: str,
) -> dict[str, str]:
    """Always-mode reminder with full budget telemetry.

    Exposes used/remaining for BOTH axes (rounds + time) plus progress %,
    so the LLM can make precise convergence decisions from round 1 instead
    of only panicking in the last 3 rounds.

    Closing instruction escalates with budget consumption, **叠加** custom_hint
    (而非覆盖)：调用方的 custom_hint 永远在前，升档梯度紧跟其后，让 LLM 同时
    看到"调用方任务语义 + 预算紧张程度"两层信号。
    - <50% rounds AND <50% time: permissive 升档
    - >=50% either: "倾向收敛，避免发起非必要的新工具调用"
    - >=70% either OR last 3 rounds: "本轮必须收敛，给出已有结论"
    """
    rounds_used = round_i + 1          # 1-based for human-readable display
    rounds_remaining = max_rounds - rounds_used
    time_pct = (elapsed / node_timeout_sec * 100) if node_timeout_sec > 0 else 0
    rounds_pct = (rounds_used / max_rounds * 100) if max_rounds > 0 else 0

    parts = [
        f"剩余 {rounds_remaining}/{max_rounds} 轮",
        f"已用 {elapsed:.0f}/{node_timeout_sec}s ({time_pct:.0f}%)",
        f"节点超时上限 {node_timeout_sec}s",
    ]

    # Escalating closing instruction based on budget consumption.
    if time_pct >= 70 or rounds_pct >= 70 or rounds_remaining <= 3:
        escalation = "本轮必须收敛，给出已有结论或当前执行结果，不要再发起新的工具调用"
    elif time_pct >= 50 or rounds_pct >= 50:
        escalation = "倾向收敛输出，避免发起非必要的新工具调用"
    else:
        escalation = "可继续探索工具以完成任务，但注意收敛预算"

    # 叠加而非覆盖：custom_hint 在前（任务语义）+ escalation 在后（预算梯度）。
    # 之前 custom_hint 一旦提供就完全屏蔽升档，导致 skill_executor 永远收到同一句
    # 收敛话而升档白写（70% 超时和 5% 超时下 LLM 收到同一句）。
    if custom_hint:
        closing = f"{custom_hint}。{escalation}"
    else:
        closing = escalation

    text = f"<system-reminder>{'，'.join(parts)}。{closing}</system-reminder>"
    return {"role": "user", "content": text}
