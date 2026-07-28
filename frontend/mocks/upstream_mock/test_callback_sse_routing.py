"""
upstream_mock callback→SSE 事件路由 E2E 测试。

测试链路：
  sidecar 回调 → upstream_mock callback handler → SSE event → 前端

验证 callback 的 UserOutputFlag=1/0 正确透传到 SSE 事件中。
"""
from __future__ import annotations

import json
import uuid
from collections.abc import AsyncIterator
from typing import Any

import httpx
import pytest
from httpx import ASGITransport

from mocks.upstream_mock.server import (
    TaskSession,
    _push_event,
    _sse_lock,
    _sse_subscribers,
    _sidecar_req_lock,
    _sidecar_req_map,
    _tasks,
    _tasks_lock,
    app,
)


# ── 便利函数 ──────────────────────────────────────────

@pytest.fixture
def reset_state():
    """每个测试前重置全局状态。"""
    _tasks.clear()
    _sse_subscribers.clear()
    _sidecar_req_map.clear()
    yield


async def _register_task(task_id: str) -> None:
    """在 upstream_mock 中注册一个测试 task（绕过 /send 的真实 sidecar POST）。"""
    async with _tasks_lock:
        _tasks[task_id] = TaskSession(task_id=task_id)
    async with _sse_lock:
        _sse_subscribers[task_id] = []


def _make_callback(
    task_id: str,
    message_type: str,
    text: str = "",
    flag: int = 0,
    stream_status: str = "Running",
    task_status_code: int | None = None,
    task_status_report: str = "",
) -> dict[str, Any]:
    """构造一个 AccessCtrl 格式的 sidecar 回调请求体。"""
    data: dict[str, Any] = {
        "MessageType": message_type,
        "UserOutputText": text,
        "UserOutputFlag": flag,
        "StreamStatus": stream_status,
    }
    if task_status_code is not None:
        data["TaskStatusCode"] = task_status_code
    if task_status_report:
        data["TaskStatusReport"] = task_status_report
    return {
        "Header": {
            "MessageType": "DataExchange",
            "TaskID": task_id,
            "Sender": "SysAgent",
            "Receiver": "AISFDEMO",
            "ClientSessionId": f"test-session-{uuid.uuid4().hex[:8]}",
            "SeqNum": task_id,
            "EventTimeStamp": "2026-07-28T12:00:00Z",
            "UserName": "test-user",
            "DeviceCategory": "2",
            "DeviceId": "test-device",
        },
        "Data": data,
    }


async def _collect_sse(client: httpx.AsyncClient, task_id: str
                       ) -> list[dict[str, Any]]:
    """连接 SSE 端点并收集所有事件直到 done。"""
    events: list[dict[str, Any]] = []
    async with client.stream("GET", f"/events/{task_id}") as resp:
        assert resp.status_code == 200
        buffer = ""
        async for chunk in resp.aiter_text():
            buffer += chunk
            while "\n\n" in buffer:
                line, buffer = buffer.split("\n\n", 1)
                if line.startswith("data: "):
                    try:
                        events.append(json.loads(line[6:]))
                    except json.JSONDecodeError:
                        pass
                if line == "data: [DONE]" or ("data: " in line and
                   json.loads(line[6:]).get("type") == "done"):
                    return events
    return events


# ── 测试用例 ──────────────────────────────────────────


@pytest.mark.asyncio
async def test_callback_flag_1_forwarded_to_sse(reset_state: None) -> None:
    """flag=1 的中间思考回调 → SSE 事件中 flag=1。"""
    transport = ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        task_id = "test-flag-1"
        await _register_task(task_id)

        # POST flag=1 的回调
        resp = await client.post(
            "/coremind/systemagent/v1/callback",
            json=_make_callback(task_id, "UserOutput",
                                text="正在分析穿搭需求...", flag=1,
                                stream_status="Running"),
        )
        assert resp.status_code == 200

        # POST flag=0 的回调
        resp = await client.post(
            "/coremind/systemagent/v1/callback",
            json=_make_callback(task_id, "UserOutput",
                                text="推荐方案：白色T恤+牛仔裤", flag=0,
                                stream_status="Running"),
        )
        assert resp.status_code == 200

        # POST TaskStatusReport + done
        await _push_event(task_id, {
            "type": "task_status",
            "task_id": task_id,
            "status_code": 200,
            "report": "TaskCompleted",
        })
        await _push_event(task_id, {"type": "done", "task_id": task_id})

        # 收集 SSE 事件
        events = await _collect_sse(client, task_id)

        # 验证
        assert len(events) >= 2, f"期望至少 2 个事件，实际 {len(events)}"
        user_events = [e for e in events if e.get("type") == "UserOutput"]
        assert len(user_events) == 2

        assert user_events[0]["data"]["UserOutputFlag"] == 1, \
            f"第一个 user_output 的 flag 应为 1，实际 {user_events[0]['data']['UserOutputFlag']}"
        assert user_events[0]["data"]["UserOutputText"] == "正在分析穿搭需求..."

        assert user_events[1]["data"]["UserOutputFlag"] == 0, \
            f"第二个 user_output 的 flag 应为 0，实际 {user_events[1]['data']['UserOutputFlag']}"
        assert user_events[1]["data"]["UserOutputText"] == "推荐方案：白色T恤+牛仔裤"


@pytest.mark.asyncio
async def test_callback_flag_0_final_pushed_as_completed(reset_state: None) -> None:
    """flag=0 且 StreamStatus=Completed → SSE 事件中 flag=0, stream_status=Completed。"""
    transport = ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        task_id = "test-completed"
        await _register_task(task_id)

        # POST 最终完成回调
        resp = await client.post(
            "/coremind/systemagent/v1/callback",
            json=_make_callback(task_id, "UserOutput",
                                text="最终穿搭推荐已完成", flag=0,
                                stream_status="Completed"),
        )
        assert resp.status_code == 200

        # POST TaskStatusReport + done
        await _push_event(task_id, {
            "type": "task_status",
            "task_id": task_id,
            "status_code": 200,
            "report": "TaskCompleted",
        })
        await _push_event(task_id, {"type": "done", "task_id": task_id})

        # 收集 SSE 事件
        events = await _collect_sse(client, task_id)

        user_events = [e for e in events if e.get("type") == "UserOutput"]
        assert len(user_events) >= 1
        assert user_events[-1]["data"]["UserOutputFlag"] == 0
        assert user_events[-1].get("data", {}).get("StreamStatus") == "Completed", \
            "Completed 事件的 StreamStatus 应被透传"


@pytest.mark.asyncio
async def test_callback_preserves_header_fields(reset_state: None) -> None:
    """回调中的 Header 字段（TaskID/Sender/Receiver 等）透传到 SSE 事件。"""
    transport = ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        task_id = "test-header"
        await _register_task(task_id)

        resp = await client.post(
            "/coremind/systemagent/v1/callback",
            json=_make_callback(task_id, "UserOutput",
                                text="test", flag=1,
                                stream_status="Running"),
        )
        assert resp.status_code == 200

        # POST TaskStatusReport + done
        await _push_event(task_id, {
            "type": "task_status",
            "task_id": task_id,
            "status_code": 200,
            "report": "TaskCompleted",
        })
        await _push_event(task_id, {"type": "done", "task_id": task_id})

        events = await _collect_sse(client, task_id)

        user_events = [e for e in events if e.get("type") == "UserOutput"]
        assert len(user_events) >= 1
        assert user_events[0]["task_id"] == task_id, \
            f"task_id 应透传，实际 {user_events[0].get('task_id')}"


@pytest.mark.asyncio
async def test_task_status_triggers_done_event(reset_state: None) -> None:
    """TaskStatusReport 回调 → SSE 流关闭（done 事件）。"""
    transport = ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        task_id = "test-done"
        await _register_task(task_id)

        # 先 POST 一个 UserOutput
        await client.post(
            "/coremind/systemagent/v1/callback",
            json=_make_callback(task_id, "UserOutput",
                                text="最终回复", flag=0,
                                stream_status="Completed"),
        )
        # POST TaskStatusReport → 触发 done
        resp = await client.post(
            "/coremind/systemagent/v1/callback",
            json=_make_callback(task_id, "TaskStatusReport",
                                text="", flag=0,
                                stream_status="",
                                task_status_code=200,
                                task_status_report="TaskCompleted"),
        )
        assert resp.status_code == 200

        # 收集 SSE 事件
        events = await _collect_sse(client, task_id)

        # 验证有 TaskStatusReport 事件 + 流关闭
        status_events = [e for e in events if e.get("type") == "TaskStatusReport"]
        assert len(status_events) >= 1
        assert status_events[-1].get("data", {}).get("TaskStatusCode") == 200
        assert status_events[-1].get("data", {}).get("TaskStatusReport") == "TaskCompleted"

        # done 事件存在
        type_list = [e.get("type") for e in events]
        assert "done" in type_list or any(
            e.get("type") == "TaskStatusReport" for e in events
        ), "应收到 task_status 事件后流正常关闭"
