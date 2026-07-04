"""集成测试：AISF 调度入口 /aisf/healthagent/v1/tasks。

覆盖场景：
  - 异步模式返回 202 + 正确 task_id
  - 后台按 task_id 执行 daemon 调用并回调上游（完整报文校验）
  - 多个并发 task_id 互不串扰（隔离性）
  - Daemon 出错时回传 TaskStatusReport(status=500)
  - Daemon 不可用 → 503
  - 非 TASKASSIGNMENT 事件 → 202 ignored
  - 无效 JSON → 400
"""

from __future__ import annotations

import asyncio
import json
from unittest.mock import AsyncMock, call, patch

import pytest
from fastapi import FastAPI
from httpx import ASGITransport, AsyncClient

from client import DaemonClient
from routes import aisf_tasks


# ═══════════════════════════════════════════════════════════════════
#  Factory
# ═══════════════════════════════════════════════════════════════════


def _task_request(
    task_id: str = "task-001",
    content: str = "用户表示肚子疼，咨询健康建议",
    sender: str = "SystemAgent",
    receiver: str = "HealthAgent",
    user_name: str = "user-001",
    device_id: str = "device-001",
    sys_agent_entity: str = "127.0.0.1",
) -> dict:
    """构建标准 TASKASSIGNMENT 请求体（对标 AISF-interface.md §1）。"""
    return {
        "Header": {
            "MessageType": "UserTaskManagement",
            "TaskID": task_id,
            "Sender": sender,
            "Receiver": receiver,
            "UserName": user_name,
            "DeviceId": device_id,
            "DeviceCategory": "2",
            "UUID": f"uuid-{task_id}",
            "ClientSessionId": f"session-{task_id}",
            "AcmSessionId": "",
            "DialogueId": "",
            "SessionId": "",
            "EventTimeStamp": "2026-07-03T12:00:00Z",
            "SysAgentEntity": sys_agent_entity,
            "TeamEnvironmentID": "te_test_001",
            "ClientExField": "",
        },
        "Data": {
            "MessageType": "TaskAssignment",
            "UserInputText": content,
            "RewriteQ": content,
            "UserIntent": "",
            "UserInputObject": '{"code":200003,"data":{"intent":"","params":null}}',
            "UserSpecifiedAgent": "",
            "APPName": "",
            "ToolName": "",
            "IsHaveSendHuoShan": "0",
            "LongHistory": "",
        },
    }


def _rebuild_json_body(header: dict, data: dict) -> dict:
    """模拟上游解析回调报文时的 JSON 反序列化（验证 Header/Data 可完整回传）。"""
    return json.loads(json.dumps({"Header": header, "Data": data}))


# ═══════════════════════════════════════════════════════════════════
#  Fixtures
# ═══════════════════════════════════════════════════════════════════


@pytest.fixture
def daemon() -> AsyncMock:
    client = AsyncMock(spec=DaemonClient)
    client.is_connected = True
    client.agent_execute.return_value = {
        "task_id": "task-001",
        "dialogue_id": "dialogue-001",
    }
    client.wait_for_response.return_value = {
        "content": "根据您描述的肚子疼，建议注意饮食清淡，避免辛辣刺激食物，"
                   "如果持续疼痛请及时就医。",
        "final": True,
    }
    return client


@pytest.fixture
async def client(daemon: AsyncMock) -> AsyncClient:
    """创建隔离的 FastAPI 应用，注入 mock daemon。"""
    app = FastAPI()
    app.include_router(aisf_tasks.router)
    app.state.daemon_client = daemon
    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://bridge"
    ) as http:
        yield http


# ═══════════════════════════════════════════════════════════════════
#  Helper
# ═══════════════════════════════════════════════════════════════════


async def _fire_and_wait_background(
    client: AsyncClient,
    daemon: AsyncMock,
    json_body: dict,
    timeout: float = 5.0,
) -> tuple[AsyncMock, asyncio.Task | None]:
    """POST 请求并等待后台 `asyncio.create_task` 完成，返回 mock 和 task。"""
    tasks: list[asyncio.Task] = []
    original_ct = asyncio.create_task

    def _capture_task(coro, **kwargs):
        t = original_ct(coro, **kwargs)
        tasks.append(t)
        return t

    with patch.object(asyncio, "create_task", side_effect=_capture_task):
        resp = await client.post("/aisf/healthagent/v1/tasks", json=json_body)
        for t in tasks:
            await asyncio.wait_for(t, timeout=timeout)
    return resp, tasks[0] if tasks else None


@pytest.fixture
def upstream_mock() -> AsyncMock:
    """Mock 所有 `http_post_to_upstream` 引用，捕获回调报文。

    post_result_to_upstream 内部调用 server 模块的 http_post_to_upstream，
    而失败路径直接调用 routes 模块的 http_post_to_upstream（也是 server 的引用），
    因此需要两个补丁指向同一个 mock。
    """
    mock = AsyncMock()
    mock.return_value = True
    with (
        patch("aisf_adapter.sysagent.server.http_post_to_upstream", mock),
        patch("routes.aisf_tasks.http_post_to_upstream", mock),
    ):
        yield mock


# ═══════════════════════════════════════════════════════════════════
#  Tests
# ═══════════════════════════════════════════════════════════════════


class TestReceiveTask:
    """POST /aisf/healthagent/v1/tasks 接收任务"""

    async def test_async_mode_returns_202(self, client: AsyncClient, daemon: AsyncMock) -> None:
        """TC-aisf-001: 异步模式返回 202 accepted + 正确 task_id。"""
        resp, _ = await _fire_and_wait_background(
            client, daemon, _task_request()
        )

        assert resp.status_code == 202
        body = resp.json()
        assert body["status"] == "accepted"
        assert body["task_id"] == "task-001"
        assert body["mode"] == "async"

    async def test_invalid_json_returns_400(self, client: AsyncClient) -> None:
        """TC-aisf-002: 无效 JSON → 400。"""
        resp = await client.post(
            "/aisf/healthagent/v1/tasks",
            content="not valid json",
            headers={"Content-Type": "application/json"},
        )
        assert resp.status_code == 400
        assert "invalid JSON" in resp.text

    async def test_non_taskassignment_ignored(self, client: AsyncClient) -> None:
        """TC-aisf-003: 非 TASKASSIGNMENT 事件 → 202 ignored。"""
        body = {
            "Header": {"MessageType": "UserTaskManagement", "TaskID": "t2", "Sender": "SystemAgent"},
            "Data": {"MessageType": "TaskStop", "TaskStatusCode": 100},
        }
        resp = await client.post("/aisf/healthagent/v1/tasks", json=body)
        assert resp.status_code == 202
        assert resp.json()["status"] == "ignored"
        assert resp.json()["task_id"] == "t2"

    async def test_daemon_unavailable_returns_503(self, client: AsyncClient) -> None:
        """TC-aisf-004: daemon 不可用 → 503。"""
        # 覆盖 app 让 daemon_client = None
        app = FastAPI()
        app.include_router(aisf_tasks.router)
        app.state.daemon_client = None
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://bridge"
        ) as http:
            req = _task_request()
            resp = await http.post("/aisf/healthagent/v1/tasks", json=req)
        assert resp.status_code == 503
        assert resp.json()["error"] == "daemon_unavailable"
        assert resp.json()["task_id"] == "task-001"


class TestBackgroundExecution:
    """后台异步执行与上游回调"""

    async def test_agent_execute_called_with_correct_params(
        self, client: AsyncClient, daemon: AsyncMock
    ) -> None:
        """TC-aisf-010: daemon.agent_execute 携带正确的参数调用。"""
        await _fire_and_wait_background(
            client, daemon,
            _task_request(task_id="task-x", content="今天天气如何", user_name="alice"),
        )

        daemon.agent_execute.assert_awaited_once()
        call_kwargs = daemon.agent_execute.call_args
        assert call_kwargs[0][0] == "outfit-assistant"  # agent_id (positional)
        assert call_kwargs[1]["content"] == "今天天气如何"
        assert call_kwargs[1]["user_id"] == "alice"
        assert call_kwargs[1]["client_id"] == "device-001"
        # 关键约束：request_id 必须与 Header.TaskID 一致
        assert call_kwargs[1]["request_id"] == "task-x"

    async def test_background_polls_until_final(
        self, client: AsyncClient, daemon: AsyncMock
    ) -> None:
        """TC-aisf-011: 后台轮询直到 `final=True`。"""
        # 模拟两次非 final 响应后返回 final
        daemon.wait_for_response.side_effect = [
            {"content": "chunk-1"},
            {"content": "chunk-2", "final": True},
        ]
        await _fire_and_wait_background(
            client, daemon, _task_request()
        )

        assert daemon.wait_for_response.await_count == 2

    async def test_upstream_callback_with_correct_task_id(
        self, client: AsyncClient, daemon: AsyncMock, upstream_mock: AsyncMock
    ) -> None:
        """TC-aisf-012: 回调上游的报文中 Header.TaskID 与请求一致（不变量 I-1）。"""
        await _fire_and_wait_background(
            client, daemon,
            _task_request(task_id="ha_001", content="肚子疼咨询"),
        )

        # 应有 2 次回调：UserOutput → TaskStatusReport
        assert upstream_mock.call_count == 2, (
            f"expected 2 upstream callbacks, got {upstream_mock.call_count}"
        )
        output_call, status_call = upstream_mock.call_args_list

        # --- 第 1 次：UserOutput ---
        assert output_call.args[0] == "127.0.0.1"  # sysAgentEntity
        output_body = output_call.args[1]
        assert output_body["Header"]["MessageType"] == "DataExchange"
        assert output_body["Header"]["TaskID"] == "ha_001", (
            "UserOutput.Header.TaskID 必须与请求一致"
        )
        # Sender/Receiver 互换：响应 Sender=本端标识，Receiver=请求 Sender
        assert output_body["Header"]["Sender"] == "HealthAgent", "响应 Sender 应为本端标识"
        assert output_body["Header"]["Receiver"] == "SystemAgent", (
            "响应 Receiver 应等于请求 Header.Sender"
        )
        assert output_body["Header"]["UserName"] == "user-001"

        assert output_body["Data"]["MessageType"] == "UserOutput"
        # 请求内容正确的带回去：UserOutputText = daemon 最终回复
        expected_content = (
            "根据您描述的肚子疼，建议注意饮食清淡，避免辛辣刺激食物，"
            "如果持续疼痛请及时就医。"
        )
        assert output_body["Data"]["UserOutputText"] == expected_content, (
            "UserOutputText 应包含 daemon 返回的最终回复内容"
        )
        # 补全字段检查（见 §2.1）
        assert output_body["Data"]["IsVoiceBroadcast"] == 0
        assert output_body["Data"]["UserOutputFlag"] == 0

        # --- 第 2 次：TaskStatusReport ---
        assert status_call.args[0] == "127.0.0.1"  # sysAgentEntity
        status_body = status_call.args[1]
        assert status_body["Header"]["MessageType"] == "TaskStatusManagement"
        assert status_body["Header"]["TaskID"] == "ha_001", (
            "TaskStatusReport.Header.TaskID 必须与请求一致"
        )
        assert status_body["Data"]["MessageType"] == "TaskStatusReport"
        assert status_body["Data"]["TaskStatusCode"] == 200
        assert status_body["Data"]["TaskStatusReport"] == "TaskCompleted"

    async def test_upstream_callback_fields_roundtrip(
        self, client: AsyncClient, daemon: AsyncMock, upstream_mock: AsyncMock
    ) -> None:
        """TC-aisf-013: 回调字段与请求元数据一一对应（回传完整性）。"""
        # 使用非默认 sender 名称，显式验证 Sender/Receiver 互换
        req = _task_request(
            task_id="roundtrip-001",
            sender="AISFDEMO",
            user_name="test_user",
            device_id="test_device",
            sys_agent_entity="10.0.0.5",
        )
        await _fire_and_wait_background(client, daemon, req)

        output_call, _ = upstream_mock.call_args_list
        output_body = output_call.args[1]

        # ── Header 回传完整性 ──
        header = output_body["Header"]
        assert header["TaskID"] == "roundtrip-001"
        assert header["UserName"] == "test_user"
        assert header["DeviceId"] == "test_device"
        # Sender/Receiver 互换：本端固定 Sender=HealthAgent，Receiver=请求方
        assert header["Sender"] == "HealthAgent", (
            "响应 Sender 必须为本端标识（固定值）"
        )
        assert header["Receiver"] == "AISFDEMO", (
            "响应 Receiver 必须等于请求 Header.Sender（互换）"
        )

        # ── Data 回传完整性 ──
        data = output_body["Data"]
        assert data["MessageType"] == "UserOutput"
        # UserOutputText = daemon 最终回复
        expected_content = daemon.wait_for_response.return_value["content"]
        assert data["UserOutputText"] == expected_content, (
            "UserOutputText 应等于 agent 执行结果"
        )

        # ── JSON 序列化/反序列化不丢字段 ──
        roundtrip = _rebuild_json_body(header, data)
        assert roundtrip["Header"]["TaskID"] == "roundtrip-001"
        assert roundtrip["Header"]["UserName"] == "test_user"
        assert roundtrip["Header"]["Sender"] == "HealthAgent"
        assert roundtrip["Header"]["Receiver"] == "AISFDEMO"
        assert roundtrip["Data"]["MessageType"] == "UserOutput"
        assert roundtrip["Data"]["UserOutputText"] == expected_content

    async def test_multiple_concurrent_tasks_isolation(
        self, client: AsyncClient, daemon: AsyncMock, upstream_mock: AsyncMock
    ) -> None:
        """TC-aisf-014: 多个并发 task_id 互不串扰（隔离性）。"""
        req_a = _task_request(task_id="task-a", content="请求A", sys_agent_entity="10.0.0.1")
        req_b = _task_request(task_id="task-b", content="请求B", sys_agent_entity="10.0.0.2")

        tasks: list[asyncio.Task] = []
        original_ct = asyncio.create_task

        def _capture_tasks(coro, **kwargs):
            t = original_ct(coro, **kwargs)
            tasks.append(t)
            return t

        with patch.object(asyncio, "create_task", side_effect=_capture_tasks):
            resp_a = await client.post("/aisf/healthagent/v1/tasks", json=req_a)
            resp_b = await client.post("/aisf/healthagent/v1/tasks", json=req_b)

            assert resp_a.status_code == 202
            assert resp_a.json()["task_id"] == "task-a"
            assert resp_b.status_code == 202
            assert resp_b.json()["task_id"] == "task-b"

            for t in tasks:
                await asyncio.wait_for(t, timeout=5.0)

        # 两个请求都执行了 agent_execute
        assert daemon.agent_execute.await_count == 2

        # 按 request_id 区分两个请求的 daemon 调用
        calls_by_task = {}
        for c in daemon.agent_execute.call_args_list:
            rid = c.kwargs.get("request_id", "")
            calls_by_task[rid] = c.kwargs

        assert "task-a" in calls_by_task, "task-a 未调用 daemon"
        assert "task-b" in calls_by_task, "task-b 未调用 daemon"
        assert calls_by_task["task-a"]["content"] == "请求A"
        assert calls_by_task["task-b"]["content"] == "请求B"
        # user_id/client_id 必须各自独立
        assert calls_by_task["task-a"]["user_id"] == "user-001"
        assert calls_by_task["task-b"]["user_id"] == "user-001"

    async def test_daemon_error_sends_failure_callback(
        self, client: AsyncClient, daemon: AsyncMock, upstream_mock: AsyncMock
    ) -> None:
        """TC-aisf-020: daemon 执行失败时回调 TaskStatusReport(500)。"""
        daemon.agent_execute.side_effect = RuntimeError("connection refused")

        await _fire_and_wait_background(
            client, daemon,
            _task_request(task_id="fail-001", content="出错测试"),
        )

        # 应回调一次（TaskStatusReport with code=500）
        assert upstream_mock.call_count == 1, (
            f"expected 1 failure callback, got {upstream_mock.call_count}"
        )
        status_call = upstream_mock.call_args_list[0]
        status_body = status_call.args[1]
        assert status_body["Header"]["TaskID"] == "fail-001"
        assert status_body["Data"]["TaskStatusCode"] == 500
        assert "ExecutionFailed" in status_body["Data"]["TaskStatusReport"]

    async def test_daemon_timeout_sends_failure_callback(
        self, client: AsyncClient, daemon: AsyncMock, upstream_mock: AsyncMock
    ) -> None:
        """TC-aisf-021: daemon 超时也回调 TaskStatusReport(500)。"""
        daemon.wait_for_response.side_effect = asyncio.TimeoutError("task timeout")

        await _fire_and_wait_background(
            client, daemon,
            _task_request(task_id="timeout-001", content="超时测试"),
        )

        assert upstream_mock.call_count == 1
        status_body = upstream_mock.call_args_list[0].args[1]
        assert status_body["Data"]["TaskStatusCode"] == 500

    async def test_no_upstream_address_no_callback(
        self, client: AsyncClient, daemon: AsyncMock, upstream_mock: AsyncMock
    ) -> None:
        """TC-aisf-022: 无 sysAgentEntity（上游地址）时不回调。"""
        req = _task_request(task_id="no-addr", content="无地址", sys_agent_entity="")
        result, _ = await _fire_and_wait_background(client, daemon, req)

        # 异步模式仍返回 202
        assert result.status_code == 202
        # 但不应有任何上游回调
        upstream_mock.assert_not_called()
