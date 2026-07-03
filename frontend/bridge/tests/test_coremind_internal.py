"""Contract tests for the trusted SideCar-to-Bridge endpoint."""

from __future__ import annotations

from unittest.mock import AsyncMock

import pytest
from fastapi import FastAPI
from httpx import ASGITransport, AsyncClient

from client import DaemonClient
from dependencies import get_daemon_client
from routes import coremind_internal


@pytest.fixture
def daemon() -> AsyncMock:
    client = AsyncMock(spec=DaemonClient)
    client.agent_execute.return_value = {"message_id": "msg-1", "status": "submitted"}
    client.wait_for_response.return_value = {"content": "node-ok", "final": True}
    return client


@pytest.fixture
async def client(daemon: AsyncMock, monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("AISF_INTERNAL_TOKEN", "trusted-test-token")
    app = FastAPI()
    app.include_router(coremind_internal.router)
    app.dependency_overrides[get_daemon_client] = lambda: daemon
    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://bridge"
    ) as http:
        yield http


def _envelope() -> dict:
    return {
        "schema_version": "1.0",
        "request_id": "request-001",
        "source": "coremind-sidecar",
        "original_request": {
            "Header": {
                "TaskID": "task-001",
                "UserName": "user-001",
                "DeviceId": "device-001",
            },
            "Data": {
                "MessageType": "TaskAssignment",
                "UserInputText": "请执行一次节点检查",
            },
        },
        "platform_context": {
            "services": {
                "IMInferScheduler": {
                    "status": "resolved",
                    "instances": [{"host": "10.0.0.5", "port": 27321}],
                }
            }
        },
    }


async def test_internal_callback_rejects_untrusted_context(client: AsyncClient) -> None:
    """TC-brg-022: 外部伪造的平台上下文不得进入 AgentFlow。

    业务规则:
    - 只有通过内部凭据认证的 SideCar 请求可以携带 platform_context。

    错误模型:
    - 仅检查 source 字段，导致外部调用者伪造服务地址。

    失败意味着:
    - Node 可能连接攻击者指定的服务实例。
    """
    response = await client.post("/internal/coremind/v1/callback", json=_envelope())
    assert response.status_code == 401


async def test_internal_callback_forwards_context_to_agentflow(
    client: AsyncClient,
    daemon: AsyncMock,
) -> None:
    """TC-brg-023: 可信上下文绑定本次请求并进入 AgentFlow。

    业务规则:
    - Bridge 必须保持服务快照结构，并与 request_id 一起提交给 daemon。

    错误模型:
    - Bridge 只转发用户文本，静默丢失 SideCar 的服务发现结果。

    失败意味着:
    - Workflow/Node 无法消费本次请求的已解析服务实例。
    """
    envelope = _envelope()
    response = await client.post(
        "/internal/coremind/v1/callback",
        json=envelope,
        headers={"Authorization": "Bearer trusted-test-token"},
    )

    assert response.status_code == 200
    assert response.json()["data"]["content"] == "node-ok"
    daemon.agent_execute.assert_awaited_once_with(
        "outfit-assistant",
        session_id="",
        content="请执行一次节点检查",
        user_id="user-001",
        client_id="device-001",
        dialogue_id="request-001",
        platform_context=envelope["platform_context"],
        request_id="request-001",
    )


async def test_internal_callback_rejects_inconsistent_service_snapshot(
    client: AsyncClient,
    daemon: AsyncMock,
) -> None:
    """TC-brg-025: 不一致的服务快照不得进入 AgentFlow。

    业务规则:
    - resolved 服务必须至少有一个实例，unavailable 服务必须给出错误码。

    错误模型:
    - 接受语义自相矛盾的上下文，导致 Node 在运行期才以模糊错误失败。

    失败意味着:
    - 内部契约无法在 Bridge 边界 fail fast。
    """
    envelope = _envelope()
    envelope["platform_context"]["services"]["IMInferScheduler"]["instances"] = []
    response = await client.post(
        "/internal/coremind/v1/callback",
        json=envelope,
        headers={"Authorization": "Bearer trusted-test-token"},
    )
    assert response.status_code == 422
    daemon.agent_execute.assert_not_awaited()
