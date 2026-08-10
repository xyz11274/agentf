"""Tests: coordinator 提示词在「信息严重不足」时允许向用户提问澄清。

锁定的不变量（对应 2026-08-07 修复）：
  当用户请求缺少无法取合理默认的关键决策信息（如地点/场合/风格）时，
  coordinator 应允许直接向用户提问澄清，而不是被「收敛优先」一味压成
  直接回复或强行委派 skill。

反例（修复前）：PROTOCOL_TEMPLATE 的「收敛优先(最高优先级)」写着
  "不确定是否还需要更多 skill 时，默认直接回复用户"，
  且 DECISION_SUCCESS_PROMPT 只允许"通过新 skill 补齐信息缺口"——
  澄清不需要新 skill，导致 LLM 从不向用户提问。
"""

import sys
from pathlib import Path

# coordinator_decide.py 使用 `from prompts import ...`（sys.path 注入
# src/agentflow_runtime 后顶层导入）。测试走同款机制，保证与 runtime 一致。
_SRC_RUNTIME = Path(__file__).resolve().parents[1] / "src" / "agentflow_runtime"
sys.path.insert(0, str(_SRC_RUNTIME))

from prompts.coordinator import (  # noqa: E402
    DECISION_SUCCESS_PROMPT,
    PROTOCOL_TEMPLATE,
)


def test_protocol_allows_asking_user_when_info_severely_missing():
    """PROTOCOL_TEMPLATE 必须允许「信息严重不足时向用户提问」，
    不能被收敛优先条款完全压制。"""
    assert "向用户提问" in PROTOCOL_TEMPLATE or "提问澄清" in PROTOCOL_TEMPLATE


def test_protocol_does_not_blanket_forbid_confirmation():
    """「想再确认一下」不得被一刀切禁止——必须限定为
    「提不出具体信息缺口的确认」才被禁止。"""
    assert "提不出具体的信息缺口" in PROTOCOL_TEMPLATE


def test_decision_success_permits_clarify_question():
    """DECISION_SUCCESS_PROMPT 在存在信息缺口时，
    应允许先向用户提问澄清（而非只能通过新 skill 补齐）。"""
    assert "向用户提问" in DECISION_SUCCESS_PROMPT or "提问澄清" in DECISION_SUCCESS_PROMPT


def test_decision_success_still_requires_concrete_gap():
    """提问澄清必须基于明确的信息缺口，不能退化为"想再确认一下"。"""
    assert "信息缺口" in DECISION_SUCCESS_PROMPT
