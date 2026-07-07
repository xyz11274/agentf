"""
E2E: MockDaemonClient — 进程内验证 bridge → daemon 流式参数 + SSE 多 chunk.

使用自定义 MockDaemonClient 替代真实 WebSocket 连接，通过内存队列
模拟 daemon 的多 notification 推送。避免跨进程/跨线程 WebSocket 问题。
"""

from __future__ import annotations

import asyncio
import json
from dataclasses import dataclass, field
from typing import Any
from unittest.mock import AsyncMock

import pytest
from fastapi import FastAPI
from starlette.testclient import TestClient

from client import DaemonClient
from dependencies import get_daemon_client


# ---------------------------------------------------------------------------
# MockDaemonClient — 进程内 daemon 模拟
# ---------------------------------------------------------------------------


class MockDaemonClient(DaemonClient):
    """模拟 DaemonClient，无需 WebSocket 连接。

    通过内存队列模拟 daemon 响应，记录 agent_execute 调用参数供断言。
    """

    def __init__(self) -> None:
        # 跳过父类 __init__，不建立 WebSocket 连接
        self.received_execute_params: dict[str, Any] | None = None
        self._response_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._connected = True  # 模拟已连接状态

    @property
    def is_connected(self) -> bool:
        return True

    async def connect(self) -> None:
        pass  # 无需连接

    async def close(self) -> None:
        pass

    def set_responses(self, responses: list[dict[str, Any]]) -> None:
        """设置模拟的 agent.response 推送序列。"""
        self._response_queue = asyncio.Queue()
        for resp in responses:
            self._response_queue.put_nowait(resp)

    async def agent_execute(
        self,
        agent_id: str,
        session_id: str,
        content: str,
        user_id: str = "",
        client_id: str = "bridge",
        dialogue_id: str = "",
        platform_context: dict[str, Any] | None = None,
        request_id: str = "",
        stream: bool = False,
    ) -> dict[str, Any]:
        self.received_execute_params = {
            "agent_id": agent_id,
            "session_id": session_id,
            "user_id": user_id,
            "client_id": client_id,
            "dialogue_id": dialogue_id,
            "stream": stream,
        }
        return {"task_id": "mock-task-001", "session_id": "mock-ses-001"}

    async def wait_for_response(
        self,
        agent_id: str,
        *,
        dialogue_id: str = "",
        timeout: float = 30.0,
    ) -> dict[str, Any]:
        try:
            return await asyncio.wait_for(
                self._response_queue.get(), timeout=timeout,
            )
        except asyncio.TimeoutError:
            from client import DaemonTimeoutError
            raise DaemonTimeoutError(
                f"timeout waiting for agent.response after {timeout}s",
                code=-32000,
            )

    async def send_request(
        self, method: str, params: dict[str, Any] | None = None, **kwargs: Any,
    ) -> Any:
        return {}

    def discard_response_queue(self, dialogue_id: str) -> None:
        pass


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def mock_daemon() -> MockDaemonClient:
    return MockDaemonClient()


@pytest.fixture
def bridge_client(mock_daemon: MockDaemonClient) -> TestClient:
    """构建 bridge_app，覆写 daemon 依赖为 MockDaemonClient。"""
    app = FastAPI()

    async def _override() -> DaemonClient:
        return mock_daemon

    app.dependency_overrides[get_daemon_client] = _override

    from routes.chat_rest import router as chat_rest_router
    app.include_router(chat_rest_router)
    return TestClient(app)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _parse_sse(text: str) -> list[dict[str, Any] | str]:
    """解析 OpenAI 兼容 SSE 文本为事件列表。"""
    events: list[dict[str, Any] | str] = []
    current: list[str] = []
    for line in text.split("\n"):
        if line.startswith("data: "):
            payload = line[6:].strip()
            if payload == "[DONE]":
                events.append(payload)
            else:
                current.append(payload)
        elif line == "" and current:
            events.append(json.loads("".join(current)))
            current = []
    return events


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class TestExtractFinal:
    """_extract_final 单元测试 —— 守设计文档 §3.2 ``metadata.final`` 是唯一真相源。"""

    # 延迟导入，避免模块级 import 破坏 fixture 作用域
    def _extract_final(self, payload: dict) -> bool:
        from routes.chat_rest import _extract_final
        return _extract_final(payload)

    def test_metadata_final_true(self) -> None:
        """metadata.final="true" → True（§3.2 唯一真相源）"""
        assert self._extract_final({"metadata": {"final": "true"}}) is True

    def test_metadata_final_false(self) -> None:
        """metadata.final="false" → False"""
        assert self._extract_final({"metadata": {"final": "false"}}) is False

    def test_metadata_final_missing_fallback_top_level(self) -> None:
        """metadata.final 缺失时降级读顶层 final=True → True（兼容老 daemon）"""
        assert self._extract_final({"final": True}) is True

    def test_metadata_final_missing_fallback_top_level_false(self) -> None:
        """metadata.final 缺失时降级读顶层 final=False → False"""
        assert self._extract_final({"final": False}) is False

    def test_metadata_final_missing_no_fallback(self) -> None:
        """metadata.final 和顶层 final 都不存在 → False"""
        assert self._extract_final({"content": "hello"}) is False

    def test_metadata_final_empty_string(self) -> None:
        """metadata.final="" → 降级读顶层 → False"""
        assert self._extract_final({"metadata": {"final": ""}}) is False


class TestE2eStreamParamPropagation:

    def test_stream_true_when_stream_query_param(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """?stream=true → agent.execute params 含 stream=True → SSE 多 chunk。

        llm_chunk chunk 的 content 是节点推的原始 OpenAI SSE chunk JSON，
        bridge 原样透传（不再二次包装 OpenAI 信封）——见 streaming-response.md §3.4。
        text chunk 的 content 是纯文本，bridge 包装 OpenAI 信封供前端 SSE parser 解析。
        """
        mock_daemon.set_responses([
            {"content": '{"choices":[{"delta":{"content":"Hello "},"index":0}]}',
             "final": False, "metadata": {"type": "llm_chunk"}},
            {"content": "world!", "final": True,
             "metadata": {"type": "text"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent?stream=true",
            json={"user_id": "u1", "client_id": "c1", "content": "hi"},
        )
        assert resp.status_code == 200, f"got {resp.status_code}: {resp.text[:200]}"
        assert resp.headers["content-type"].startswith("text/event-stream")

        # 验证 stream 参数传递
        assert mock_daemon.received_execute_params is not None
        assert mock_daemon.received_execute_params["stream"] is True, (
            f"expected stream=True, got: {mock_daemon.received_execute_params}"
        )
        assert mock_daemon.received_execute_params["dialogue_id"] != ""

        # 验证 SSE 多 chunk
        events = _parse_sse(resp.text)
        assert len(events) == 3, f"got {len(events)} events: {events}"
        # llm_chunk: 原样透传节点推的 OpenAI raw JSON（不二次包装）
        assert events[0]["choices"][0]["delta"]["content"] == "Hello ", (
            f"llm_chunk must pass through raw OpenAI JSON: {events[0]}"
        )
        # text: bridge 包装 OpenAI 信封（§3.2 核心原则 #2：text 用 message.content）
        assert events[1]["choices"][0]["message"]["content"] == "world!"
        assert events[2] == "[DONE]"

    def test_stream_false_when_no_query_param(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """无 ?stream → 同步 JSON 响应，agent.execute 不含 stream=True。"""
        mock_daemon.set_responses([
            {"content": "Hi!", "final": True, "metadata": {"type": "text"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent",
            json={"user_id": "u1", "client_id": "c1", "content": "hello"},
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["success"] is True

        assert mock_daemon.received_execute_params is not None
        stream_val = mock_daemon.received_execute_params.get("stream", False)
        assert stream_val is False, f"sync mode must NOT pass stream=True, got stream={stream_val}"

    def test_three_chunks_streaming(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """3 final=false llm_chunk + 1 final=true text → 4 content events + [DONE]。

        llm_chunk 原样透传节点推的 OpenAI raw JSON；text chunk 包装 OpenAI 信封。
        """
        mock_daemon.set_responses([
            {"content": '{"choices":[{"delta":{"content":"A"},"index":0}]}',
             "final": False, "metadata": {"type": "llm_chunk"}},
            {"content": '{"choices":[{"delta":{"content":"B"},"index":0}]}',
             "final": False, "metadata": {"type": "llm_chunk"}},
            {"content": '{"choices":[{"delta":{"content":"C"},"index":0}]}',
             "final": False, "metadata": {"type": "llm_chunk"}},
            {"content": "ABC", "final": True, "metadata": {"type": "text"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent?stream=true",
            json={"user_id": "u1", "client_id": "c1", "content": "hi"},
        )
        assert resp.status_code == 200

        events = _parse_sse(resp.text)
        assert len(events) == 5, f"got {len(events)} events"
        # llm_chunk (events 0-2) 使用 delta.content；text (event 3) 使用 message.content
        for i in range(3):
            assert events[i]["choices"][0]["delta"]["content"] == chr(ord("A") + i)
        assert events[3]["choices"][0]["message"]["content"] == "ABC"
        assert events[-1] == "[DONE]"

    def test_stream_false_explicit(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """?stream=false → 同步 JSON 响应。"""
        mock_daemon.set_responses([
            {"content": "OK", "final": True, "metadata": {"type": "text"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent?stream=false",
            json={"user_id": "u1", "client_id": "c1", "content": "hi"},
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["success"] is True
        assert body["data"]["content"] == "OK"

        # stream=False 时不应传 stream=True
        assert mock_daemon.received_execute_params is not None
        assert mock_daemon.received_execute_params.get("stream") is False

    def test_single_final_chunk_streaming(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """daemon 直接返回 final=true → 1 个 content chunk + [DONE]。"""
        mock_daemon.set_responses([
            {"content": "Final", "final": True, "metadata": {"type": "text"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent?stream=true",
            json={"user_id": "u1", "client_id": "c1", "content": "hi"},
        )
        assert resp.status_code == 200
        events = _parse_sse(resp.text)
        assert len(events) == 2
        assert events[0]["choices"][0]["message"]["content"] == "Final"
        assert events[1] == "[DONE]"

    def test_dialogue_id_on_first_sse_chunk(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """首个 SSE chunk 应携带 _dialogue_id 用于前端 trace 过滤。"""
        mock_daemon.set_responses([
            {"content": "Hi", "final": True, "metadata": {"type": "text"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent?stream=true",
            json={"user_id": "u1", "client_id": "c1", "content": "hi"},
        )
        assert resp.status_code == 200
        events = _parse_sse(resp.text)
        assert len(events) == 2
        assert "_dialogue_id" in events[0], "first SSE chunk must carry _dialogue_id"
        assert isinstance(events[0]["_dialogue_id"], str)
        assert len(events[0]["_dialogue_id"]) > 0

    def test_progress_chunk_sse_format(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """progress chunk 的 SSE 帧格式验证 —— 守 §3.2.1 线格式。

        progress chunk 应有 ``_chunk_type=progress``、``_source``、``_skill``、
        ``_round`` 字段；content 使用 ``choices[0].message.content`` 结构
        （同步响应格式，完整内容，直接渲染）。
        """
        mock_daemon.set_responses([
            {"content": "coordinator 决策中…", "final": False,
             "metadata": {"type": "progress", "source": "coordinator"}},
            {"content": "weather_query 执行中", "final": False,
             "metadata": {"type": "progress", "source": "skill_executor",
                          "skill": "weather_query", "round": "1"}},
            {"content": "最终回复", "final": True,
             "metadata": {"type": "text", "source": "coordinator"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent?stream=true",
            json={"user_id": "u3", "client_id": "c3", "content": "hi"},
        )
        assert resp.status_code == 200
        events = _parse_sse(resp.text)
        assert len(events) == 4, f"expect 3 data + [DONE], got {len(events)}"

        # progress chunk 1: coordinator 决策中
        p1 = events[0]
        assert p1.get("_chunk_type") == "progress", f"expect progress, got {p1.get('_chunk_type')}"
        assert p1.get("_source") == "coordinator"
        assert p1["choices"][0]["message"]["content"] == "coordinator 决策中…"

        # progress chunk 2: skill_executor 执行中
        p2 = events[1]
        assert p2.get("_chunk_type") == "progress"
        assert p2.get("_source") == "skill_executor"
        assert p2.get("_skill") == "weather_query"
        assert p2.get("_round") == "1"
        assert p2["choices"][0]["message"]["content"] == "weather_query 执行中"

        # text: 最终回复
        assert events[2]["choices"][0]["message"]["content"] == "最终回复"
        assert events[3] == "[DONE]"

    def test_multi_type_message_sequence(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """多类型消息完整序列 —— 按 §3.3 消息序列 progress → llm_chunk → text。

        验证 Bridge 逐条 SSE data: 行的 _chunk_type、content 结构和最终 [DONE]。
        """
        mock_daemon.set_responses([
            # coordinator 1st round: progress
            {"content": "coordinator 决策中…", "final": False,
             "metadata": {"type": "progress", "source": "coordinator", "skill": ""}},
            # skill_executor round 0: progress
            {"content": "查询天气…", "final": False,
             "metadata": {"type": "progress", "source": "skill_executor",
                          "skill": "weather_query", "round": "0"}},
            # coordinator_decide: llm_chunk (delta.content)
            {"content": '{"choices":[{"delta":{"content":"北京"},"index":0}]}',
             "final": False, "metadata": {"type": "llm_chunk", "source": "coordinator"}},
            # coordinator_decide: llm_chunk
            {"content": '{"choices":[{"delta":{"content":"25度"},"index":0}]}',
             "final": False, "metadata": {"type": "llm_chunk", "source": "coordinator"}},
            # coordinator 最终回复: text (message.content)
            {"content": "北京今天25度", "final": True,
             "metadata": {"type": "text", "source": "coordinator"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent?stream=true",
            json={"user_id": "u4", "client_id": "c4", "content": "天气"},
        )
        assert resp.status_code == 200
        events = _parse_sse(resp.text)
        assert len(events) == 6, f"expect 5 data + [DONE], got {len(events)}"

        # event 0: progress (coordinator)
        assert events[0]["_chunk_type"] == "progress"
        assert events[0]["_source"] == "coordinator"
        assert events[0]["choices"][0]["message"]["content"] == "coordinator 决策中…"

        # event 1: progress (skill_executor)
        assert events[1]["_chunk_type"] == "progress"
        assert events[1]["_source"] == "skill_executor"
        assert events[1]["_skill"] == "weather_query"
        assert events[1]["_round"] == "0"

        # event 2-3: llm_chunk (使用 delta.content 结构)
        assert events[2]["_chunk_type"] == "llm_chunk"
        assert events[2]["choices"][0]["delta"]["content"] == "北京"
        assert events[3]["_chunk_type"] == "llm_chunk"
        assert events[3]["choices"][0]["delta"]["content"] == "25度"

        # event 4: text (使用 message.content 结构)
        assert events[4]["_chunk_type"] == "text"
        assert events[4]["choices"][0]["message"]["content"] == "北京今天25度"

        # event 5: [DONE]
        assert events[5] == "[DONE]"

    def test_sync_path_drops_non_final_chunks(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """同步路径不变量：非 final chunk 不进入最终 content。

        过程信息（progress / skill 思辨 / llm_token）在同步路径丢弃，仅在 SSE
        流式路径透传给前端作过程展示。同步路径的 content 必须只含 final 部分，
        否则持久化到 SessionProvider 的 agent_response 会夹带中间产物——违反
        "前端刷新加载历史 = 实时最终渲染"的稳定态约束。
        """
        mock_daemon.set_responses([
            {"content": "skill思辨片段", "final": False, "metadata": {"type": "progress"}},
            {"content": "skill产物", "final": False, "metadata": {"type": "llm_chunk"}},
            {"content": "coordinator最终回答", "final": True, "metadata": {"type": "text"}},
        ])

        resp = bridge_client.post(
            "/api/chat/mock-agent",
            json={"user_id": "u2", "client_id": "c2", "content": "hi"},
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["success"] is True
        content = body["data"]["content"]
        assert content == "coordinator最终回答", (
            f"sync path must drop non-final chunks, got: {content!r}"
        )
        assert "skill思辨片段" not in content, "过程信息泄漏到最终回答"
        assert "skill产物" not in content, "过程信息泄漏到最终回答"

    def test_error_when_agent_execute_fails(
        self, mock_daemon: MockDaemonClient, bridge_client: TestClient,
    ) -> None:
        """agent_execute 抛出异常时应返回 SSE error 事件。"""
        from client import DaemonError

        async def failing_execute(agent_id: str, **kwargs: Any) -> dict:
            raise DaemonError("daemon exploded")

        mock_daemon.agent_execute = failing_execute  # type: ignore[assignment]

        resp = bridge_client.post(
            "/api/chat/mock-agent?stream=true",
            json={"user_id": "u1", "client_id": "c1", "content": "hi"},
        )
        assert resp.status_code == 200
        events = _parse_sse(resp.text)
        assert len(events) == 2
        assert "error" in events[0]
        assert "daemon exploded" in events[0]["error"]["message"]
        assert events[1] == "[DONE]"
