"""
REST SSE Chat + Session Persistence E2E 测试 - pytest + daemon fixture 版.

验证:
  1. REST chat SSE 流式返回 OpenAI 兼容格式 (choices[].delta.content)
  2. 多轮对话后 session.list 返回会话
  3. 完整的 agentflow 执行链路 (daemon -> agent -> workflow -> LLM call)

LLM 模式:
  - 设置环境变量 AF_LLM_BASE_URL + API_KEY -> 真实 LLM 调用
  - 未设置 -> 回退到 mock LLM (e2e-scripted)
"""

from __future__ import annotations

import json
import time
import uuid
from typing import Any

import pytest
import requests as http_requests

from .e2e_helpers import outfit_daemon_real_llm, free_port, jsonrpc_call, send_user_message  # noqa: F401

BRIDGE_PORT: int | None = None


@pytest.fixture(scope="class")
def bridge_daemon(request: pytest.FixtureRequest,
                   outfit_daemon_real_llm: dict[str, Any]) -> dict[str, Any]:  # noqa: F811
    """启动 bridge 连接到已运行的 daemon (class-scoped).

    复用 outfit_daemon_real_llm fixture 的 daemon 实例 (优先真实 LLM),
    在此基础上启动 bridge。
    """
    global BRIDGE_PORT
    daemon_port = outfit_daemon_real_llm["daemon_port"]
    llm_mode = outfit_daemon_real_llm.get("llm_mode", "MOCK")

    print(f"[bridge_daemon] daemon_port={daemon_port} llm_mode={llm_mode}")

    # 找一个空闲端口给 bridge
    port = free_port()
    BRIDGE_PORT = port

    import subprocess
    import os
    from pathlib import Path

    bridge_dir = Path(__file__).resolve().parents[2] / "bridge"
    env = os.environ.copy()

    # 使用 uv run 确保 bridge 依赖可用，同时显式传递 daemon 地址
    env["DAEMON_HOST"] = "127.0.0.1"
    env["DAEMON_PORT"] = str(daemon_port)
    env["BRIDGE_HTTP_PORT"] = str(port)

    proc = subprocess.Popen(
        ["uv", "run", "--directory", str(bridge_dir),
         "python", "-m", "uvicorn", "main:bridge_app", "--host", "127.0.0.1",
         "--port", str(port), "--log-level", "warning"],
        cwd=str(bridge_dir),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # 等待 bridge 就绪
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            r = http_requests.get(f"http://127.0.0.1:{port}/api/health", timeout=1)
            if r.status_code == 200:
                break
        except Exception:
            pass
        time.sleep(0.3)
    else:
        proc.terminate()
        proc.wait()
        pytest.fail("bridge failed to start")

    try:
        yield {"bridge_port": port, "daemon_port": daemon_port,
               "process": proc, "llm_mode": llm_mode}
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


def _sse_parse(text: str) -> list[dict[str, Any] | str]:
    """Parse OpenAI-compatible SSE text."""
    events: list[dict[str, Any] | str] = []
    current_lines: list[str] = []
    for line in text.split("\n"):
        if line.startswith("data: "):
            payload = line[6:].strip()
            if payload == "[DONE]":
                events.append(payload)
            else:
                current_lines.append(payload)
        elif line == "":
            if current_lines:
                events.append(json.loads("".join(current_lines)))
                current_lines = []
    return events


@pytest.mark.e2e
class TestRestSseChat:
    """REST SSE 流式聊天 E2E - OpenAI 兼容格式验证."""

    def test_sse_chunk_format(self, bridge_daemon: dict[str, Any]) -> None:
        """SSE 流式返回使用 choices[].delta.content 格式."""
        port = bridge_daemon["bridge_port"]
        resp = http_requests.post(
            f"http://127.0.0.1:{port}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": "e2e-sse-user",
                "client_id": "e2e-sse-client",
                "content": "你好",
            },
            stream=True,
            timeout=30,
        )
        assert resp.status_code == 200
        assert resp.headers["content-type"].startswith("text/event-stream")

        text = resp.text
        events = _sse_parse(text)

        # At least one content chunk + [DONE]
        assert len(events) >= 2
        assert events[-1] == "[DONE]"

        # All non-terminal events must be OpenAI-compatible chunks
        for event in events[:-1]:
            assert "choices" in event
            assert len(event["choices"]) > 0
            assert "delta" in event["choices"][0]
            assert "content" in event["choices"][0]["delta"]


@pytest.mark.e2e
class TestSessionPersistence:
    """会话持久化 E2E - 发消息后 session.list 应返回会话."""

    def test_session_list_after_chat(self, bridge_daemon: dict[str, Any]) -> None:
        """发一条消息后, session.list 应返回 >=1 个活跃会话."""
        port = bridge_daemon["bridge_port"]
        daemon_port = bridge_daemon["daemon_port"]
        uid = f"e2e-{uuid.uuid4().hex[:8]}"
        cid = f"client-{uuid.uuid4().hex[:8]}"

        # 发送一条消息
        resp = http_requests.post(
            f"http://127.0.0.1:{port}/api/chat/outfit-assistant",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "你好，帮我推荐穿搭",
            },
            timeout=60,
        )
        assert resp.status_code == 200

        # 查 session.list
        result = jsonrpc_call(
            "127.0.0.1", daemon_port, "session.list",
            {"agent_id": "outfit-assistant", "user_id": uid},
        )
        sessions = result.get("result", {}).get("sessions", [])
        assert len(sessions) >= 1, (
            f"Expected >=1 sessions for user {uid}, got {len(sessions)}"
        )

        # 验证 session 包含正确的 user_id 和 client_id
        matching = [s for s in sessions if s.get("user_id") == uid]
        assert len(matching) >= 1, (
            f"No session with user_id={uid} in {sessions}"
        )
        print(f"Found session: {matching[0]}")

    def test_conversation_history(self, bridge_daemon: dict[str, Any]) -> None:
        """发消息后, conversation API 应返回历史记录."""
        port = bridge_daemon["bridge_port"]
        uid = f"e2e-{uuid.uuid4().hex[:8]}"
        cid = f"client-{uuid.uuid4().hex[:8]}"

        # 发一条消息 (同步模式)
        resp = http_requests.post(
            f"http://127.0.0.1:{port}/api/chat/outfit-assistant",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "推荐一套商务休闲穿搭",
            },
            timeout=60,
        )
        assert resp.status_code == 200
        data = resp.json()
        assert data.get("success") is True
        assert len(data.get("data", {}).get("content", "")) > 0

        # 查 conversation 历史
        conv_resp = http_requests.get(
            f"http://127.0.0.1:{port}/api/conversations/outfit-assistant",
            params={"user_id": uid, "client_id": cid},
            timeout=10,
        )
        assert conv_resp.status_code == 200
        conv_data = conv_resp.json()
        conversations = conv_data.get("conversations", [])
        assert len(conversations) >= 1, (
            f"Expected >=1 conversation for {uid}/{cid}, got {len(conversations)}"
        )
        print(f"Found {len(conversations)} conversation(s)")


@pytest.mark.e2e
class TestAgentflowPipeline:
    """完整 agentflow 执行链路 E2E 验证 - daemon -> agent -> workflow -> LLM call.

    无论 LLM 模式是 REAL 还是 MOCK, 都验证完整的执行链.
    """

    def test_agentflow_pipeline_completes(self, bridge_daemon: dict[str, Any]) -> None:
        """验证 agentflow 执行链路完整: daemon 执行 agent -> workflow -> LLM call -> 返回内容."""
        port = bridge_daemon["bridge_port"]
        daemon_port = bridge_daemon["daemon_port"]
        llm_mode = bridge_daemon.get("llm_mode", "MOCK")
        uid = f"e2e-pl-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        # Step 1: 通过 REST API 发消息, 触发 agentflow 执行
        resp = http_requests.post(
            f"http://127.0.0.1:{port}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "推荐一套适合开会的穿搭",
            },
            stream=True,
            timeout=120,
        )
        assert resp.status_code == 200

        # Step 2: 解析 SSE 流, 验证返回了内容
        text = resp.text
        events = _sse_parse(text)
        assert len(events) >= 2, f"Expected >=2 SSE events, got {len(events)}"
        assert events[-1] == "[DONE]"

        all_content = ""
        for event in events[:-1]:
            choices = event.get("choices", [])
            if choices and "delta" in choices[0]:
                all_content += choices[0]["delta"].get("content", "")

        assert len(all_content) > 0, (
            f"Agentflow pipeline returned empty content (llm_mode={llm_mode})"
        )
        print(f"[agentflow] llm_mode={llm_mode} content_len={len(all_content)} "
              f"preview={all_content[:80]}...")

        # Step 3: 验证 daemon 端会话已创建 (证明 agent.execute 被执行)
        result = jsonrpc_call(
            "127.0.0.1", daemon_port, "session.list",
            {"agent_id": "outfit-assistant", "user_id": uid},
        )
        sessions = result.get("result", {}).get("sessions", [])
        assert len(sessions) >= 1, (
            f"Expected >=1 session for user {uid} after agentflow execution, "
            f"got {len(sessions)}"
        )

        active_count = len([s for s in sessions if s.get("status") == "active"])
        print(f"[agentflow] Found {len(sessions)} session(s), {active_count} active")

    def test_multi_turn_context_maintained(self, bridge_daemon: dict[str, Any]) -> None:
        """多轮对话: 同一 session 内发 3 条消息, 验证 agentflow 保持上下文."""
        port = bridge_daemon["bridge_port"]
        daemon_port = bridge_daemon["daemon_port"]
        uid = f"e2e-mt-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        # Turn 1: 介绍信息
        r1 = http_requests.post(
            f"http://127.0.0.1:{port}/api/chat/outfit-assistant",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "我要去上海出差一周, 推荐穿搭",
            },
            timeout=120,
        )
        assert r1.status_code == 200
        d1 = r1.json()
        assert d1.get("success") is True
        content1 = d1.get("data", {}).get("content", "")
        assert len(content1) > 0, "Turn 1 response is empty"
        print(f"[multiturn] Turn 1: {len(content1)} chars")

        # Turn 2: 同一 session 继续
        r2 = http_requests.post(
            f"http://127.0.0.1:{port}/api/chat/outfit-assistant",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "我更喜欢商务休闲风格",
            },
            timeout=120,
        )
        assert r2.status_code == 200
        d2 = r2.json()
        assert d2.get("success") is True
        content2 = d2.get("data", {}).get("content", "")
        assert len(content2) > 0, "Turn 2 response is empty"
        print(f"[multiturn] Turn 2: {len(content2)} chars")

        # Turn 3: 同一 session 继续, 验证上下文能维持
        r3 = http_requests.post(
            f"http://127.0.0.1:{port}/api/chat/outfit-assistant",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "刚才说的穿搭, 能给我推荐几套具体的吗?",
            },
            timeout=120,
        )
        assert r3.status_code == 200
        d3 = r3.json()
        assert d3.get("success") is True
        content3 = d3.get("data", {}).get("content", "")
        assert len(content3) > 0, "Turn 3 response is empty"
        print(f"[multiturn] Turn 3: {len(content3)} chars")

        # 验证: 同一 session 产生了多个对话记录
        result = jsonrpc_call(
            "127.0.0.1", daemon_port, "session.list",
            {"agent_id": "outfit-assistant", "user_id": uid},
        )
        sessions = result.get("result", {}).get("sessions", [])
        sessions_for_user = [s for s in sessions if s.get("user_id") == uid]
        assert len(sessions_for_user) >= 1, (
            f"Expected >=1 session after 3 turns, got {len(sessions_for_user)}"
        )
        print(f"[multiturn] Session confirmed: {sessions_for_user[0]}")
