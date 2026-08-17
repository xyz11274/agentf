"""AgentContext — hook handler 的干预载体（领域对象）。

设计文档：docs/design/features/hooks/hook-system.md §2.4.1。

最小字段集决策（用户 2026-08-14 确认）：
- 只承载"进程内可变状态"（messages / artifacts / 干预标志）；
- 会话身份等只读信息 handler 需要时自行读 runtime_context（AF_* accessor），不冗余进本对象；
- 进程内对象（coordinator / skill_executor 各自构造），不跨进程序列化。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class AgentContext:
    """Agent 运行时上下文 — hook handler 干预的载体（最小字段集）。

    三类干预语义：
    - 注入：messages.append / artifacts.append（往对话注入信息）
    - 修改：改写 messages 内容（如注入 system 指令、改写用户消息）
    - 阻断：置 blocked=True（跳过当前动作，如阻断 skill 分发 / 跳过工具）
    """

    # ── 对话状态（可变，供注入/修改）──
    messages: list[dict[str, Any]] = field(default_factory=list)  # LLM 消息列表
    artifacts: list[str] = field(default_factory=list)            # 产物路径（可变，供注入）

    # ── 干预标志（handler 置位）──
    blocked: bool = False        # True = 阻断当前动作
    block_reason: str = ""       # 阻断原因（可观测性，配合 blocked 使用）
