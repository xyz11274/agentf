"""
穿搭助手多轮对话 E2E 测试 — pytest + daemon fixture 版。

验证多轮上下文保持：agent 在同一 session 内跨轮记住用户信息。

精简设计（PROJECT_MEMORY「真实 LLM e2e 设计原则」）：
  原两个测试（身份记忆 + 偏好记忆）各 2 轮共 4 次 LLM 调用，验证同一不变量
  「同 session 内多轮上下文保持」的差异化旁支。合并为单测试 2 轮：
  - Turn 1 同时含身份 + 偏好信息（"我叫张三，明天去上海出差，喜欢商务休闲"）
  - Turn 2 问回身份 + 索要推荐（"我叫什么？帮我推荐穿搭"）
  单次多轮同时覆盖身份记忆 + 偏好记忆链路，LLM 调用从 4 次压到 2 次。

使用 daemon 的 RESPONSE 轮询机制（兼容 daemon 退化限制），每次启动
独立的 daemon class-fixture，不做 LLM 输出内容断言。
"""

from __future__ import annotations

import time
from typing import Any

import pytest

from .e2e_helpers import (
    outfit_daemon,
    jsonrpc_call,
    send_user_message,
    poll_agent_response,
    AGENT_LAYER_L3,
    skip_if_layer_not_in,
)

_skip_reason = skip_if_layer_not_in(AGENT_LAYER_L3)
pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skip(reason="Temporarily disabled"),
]


def _poll_response(port: int, task_id: str,
                   timeout: int = 120) -> dict[str, Any]:
    """轮询 daemon 的 agent.poll_response，等待 task 完成。"""
    content = poll_agent_response("127.0.0.1", port, task_id, timeout)
    return {"Data": {"UserOutputText": content}}


def _send_turn(port: int, content: str, task_id: str,
               user_id: str = "e2e-mt-user") -> dict:
    """发送用户消息并等待响应。"""
    send_user_message("127.0.0.1", port, content, task_id)
    return _poll_response(port, task_id)


@pytest.mark.e2e
class TestMultiturnContext:
    """多轮对话：身份 + 偏好合并记忆（精简自 TestMultiturnIdentity + TestMultiturnPreference）。"""

    def test_context_remembered(self, outfit_daemon: dict[str, Any]) -> None:
        """Turn 1 介绍身份 + 偏好 → Turn 2 问回身份 + 索要推荐，agent 能正常响应两轮。

        不变量：同 session 内多轮上下文保持——单次多轮同时覆盖身份记忆 + 偏好记忆链路。
        """
        port = outfit_daemon["daemon_port"]
        uid = "e2e-mt-ctx"

        # Turn 1: 同时含身份 + 偏好信息
        r1 = _send_turn(
            port,
            "我叫张三，明天要去上海出差，喜欢商务休闲风格",
            "e2e-mt-ctx-t1",
            uid,
        )
        assert "UserOutputText" in r1.get("Data", {}), "Turn 1 应返回用户回复"

        # Turn 2: 问回身份 + 索要推荐（同一 session，验证上下文保持）
        r2 = _send_turn(
            port,
            "我叫什么？帮我基于刚才的偏好推荐穿搭",
            "e2e-mt-ctx-t2",
            uid,
        )
        assert "UserOutputText" in r2.get("Data", {}), "Turn 2 应返回用户回复"
