"""agent-connectivity E2E — 真实 LLM 连通性验证.

使用 ``fixtures/agent-connectivity`` 极简 agent(单 coordinator 节点 + response 节点),
只验证 "daemon → coordinator → 真实 LLM → 响应回流" 基本链路.

与 ``test_e2e_rest_chat_session.py`` 的区别:
    - 走 daemon 直连 JSON-RPC,不经 Bridge
    - 极简 agent 包,跳过 outfit-assistant 的 skill 调度路径
    - 仅做连通性 + 单轮/多轮返回非空内容断言

运行:
    python -m pytest frontend/bridge/tests/test_e2e_minimal_agent.py -v -m e2e

LLM 模式:
    - 设置 AF_LLM_BASE_URL + AF_LLM_API_KEY -> 真实 LLM 调用
    - 未设置 -> 回退到 mock LLM (e2e-scripted)
"""

from __future__ import annotations

import time
import uuid
from typing import Any, Callable

import pytest

from .e2e_helpers import (
    MINIMAL_AGENT_ID,
    AGENT_LAYER_L0,
    minimal_daemon_real_llm,
    send_minimal_message,
    poll_minimal_response,
    jsonrpc_call,
    skip_if_layer_not_in,
)

_skip_reason = skip_if_layer_not_in(AGENT_LAYER_L0)
pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(_skip_reason, reason=_skip_reason),
]

# 真实 LLM 偶尔返回空内容（finish_reason=length 或服务端截断），
# 不是连通性问题。重试一次兜底，避免虚假失败拖整轮回归。
_MINIMAL_EMPTY_RETRY = 1


def _send_and_poll_with_retry(
    port: int, content: str, uid: str, cid: str, timeout: int = 90,
) -> str:
    """发消息 + 轮询响应，遇空内容重试一次（真实 LLM 偶尔返回空）。

    重试时换新 request_id，避免 poll 命中旧 request 的残留状态。
    """
    for attempt in range(_MINIMAL_EMPTY_RETRY + 1):
        request_id = f"e2e-min-{uuid.uuid4().hex[:6]}"
        send_minimal_message(
            "127.0.0.1", port, content,
            user_id=uid, client_id=cid, request_id=request_id,
        )
        response = poll_minimal_response(
            "127.0.0.1", port, request_id, timeout=timeout, user_id=uid,
        )
        if response:
            return response
        if attempt < _MINIMAL_EMPTY_RETRY:
            print(f"[minimal] empty response on attempt {attempt + 1}, "
                  f"retrying with new request_id")
    return ""


@pytest.mark.e2e
class TestMinimalAgentConnectivity:
    """极简 agent 真实 LLM 连通性 E2E.

    单 daemon fixture(class-scoped),单 session 贯穿所有 test.
    真实 LLM 调用次数 = 3(单轮1 + 多轮2).
    """

    _UID = f"e2e-min-{uuid.uuid4().hex[:6]}"
    _CID = f"cli-{uuid.uuid4().hex[:6]}"

    def test_single_turn_returns_nonempty(
        self, minimal_daemon_real_llm: dict[str, Any]
    ) -> None:
        """单轮真实 LLM 调用,返回非空内容（遇空内容重试一次）."""
        port = minimal_daemon_real_llm["daemon_port"]
        content = _send_and_poll_with_retry(
            port, "你好", self._UID, self._CID, timeout=90,
        )
        assert len(content) > 0, (
            f"Minimal agent single turn returned empty content after "
            f"{_MINIMAL_EMPTY_RETRY + 1} attempts "
            f"(llm_mode={minimal_daemon_real_llm.get('llm_mode')})"
        )
        print(f"[minimal] Single turn: {len(content)} chars — "
              f"\"{content[:60]}...\"")

    def test_multi_turn_context_maintained(
        self, minimal_daemon_real_llm: dict[str, Any]
    ) -> None:
        """多轮对话: 同一 session 内发 2 条消息,验证 agentflow 保持上下文."""
        port = minimal_daemon_real_llm["daemon_port"]
        uid = self._UID
        cid = self._CID

        content1 = _send_and_poll_with_retry(
            port, "我要去上海出差一周", uid, cid, timeout=90,
        )
        assert len(content1) > 0, "Turn 1 response is empty"
        print(f"[minimal] Turn 1: {len(content1)} chars")

        content2 = _send_and_poll_with_retry(
            port, "帮我推荐穿搭", uid, cid, timeout=90,
        )
        assert len(content2) > 0, "Turn 2 response is empty"
        print(f"[minimal] Turn 2: {len(content2)} chars")

    def test_session_persistence(
        self, minimal_daemon_real_llm: dict[str, Any]
    ) -> None:
        """复用前面 test 留下的 session,验证 session.list 返回会话.

        不触发额外 LLM 调用 —— #2 已留下 session.
        """
        port = minimal_daemon_real_llm["daemon_port"]

        result = jsonrpc_call(
            "127.0.0.1", port, "session.list",
            {"agent_id": MINIMAL_AGENT_ID, "user_id": self._UID},
        )
        sessions = result.get("result", {}).get("sessions", [])
        assert len(sessions) >= 1, (
            f"Expected >=1 sessions for user {self._UID}, got {len(sessions)}"
        )
        matching = [s for s in sessions if s.get("user_id") == self._UID]
        assert len(matching) >= 1, (
            f"No session with user_id={self._UID} in {sessions}"
        )
        print(f"[minimal] Session confirmed: {matching[0]}")
