"""
穿搭助手多轮对话 E2E 测试 — pytest + daemon fixture 版。

验证多轮身份记忆：agent 在同一 session 内记住用户信息跨轮。

使用 daemon 的 RESPONSE 轮询机制（兼容 daemon 退化限制），每次启动
独立的 daemon class-fixture，不做 LLM 输出内容断言。
"""

from __future__ import annotations

import time
from typing import Any

import pytest

from .e2e_helpers import outfit_daemon, jsonrpc_call


def _poll_response(port: int, task_id: str,
                   timeout: int = 120) -> dict[str, Any]:
    """轮询 daemon 的 RESPONSE 方法，等待 task 完成。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        resp = jsonrpc_call("127.0.0.1", port, "RESPONSE",
                            {"TaskID": task_id}, req_id=2)
        result = resp.get("result", {})
        if result.get("MessageType") == "UserOutput":
            return result
        if result.get("MessageType") == "TaskStatusManagement":
            data = result.get("Data", {})
            if data.get("TaskStatusCode") == 200:
                return result
        time.sleep(0.5)
    raise TimeoutError(f"task {task_id} not completed within {timeout}s")


def _send_turn(port: int, content: str, task_id: str,
               user_id: str = "e2e-mt-user") -> dict:
    """发送用户消息并等待响应。"""
    jsonrpc_call("127.0.0.1", port, "TASKASSIGNMENT", {
        "Header": {
            "MessageType": "UserTaskManagement",
            "TaskID": task_id,
            "Sender": "SystemAgent",
            "Receiver": "HealthAgent",
            "UserName": user_id,
            "DeviceId": "e2e-device",
            "ClientSessionId": "e2e-mt-session",
            "DeviceCategory": "2",
            "UUID": f"uuid-{task_id}",
            "ClientSessionId": f"session-{task_id}",
            "AcmSessionId": "",
            "DialogueId": "",
            "SessionId": "",
            "EventTimeStamp": "2026-07-04T12:00:00Z",
            "SysAgentEntity": "127.0.0.1",
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
    }, task_id=task_id)
    return _poll_response(port, task_id)


@pytest.mark.e2e
class TestMultiturnIdentity:
    """多轮对话：身份记忆（"我叫张三" → "我叫什么？"）。"""

    def test_identity_remembered(self, outfit_daemon: dict[str, Any]) -> None:
        """Turn 1 介绍名字 → Turn 2 问回名字，agent 能正常响应两轮。"""
        port = outfit_daemon["daemon_port"]
        uid = "e2e-mt-id"

        # Turn 1: 介绍名字
        r1 = _send_turn(port, "我叫张三", "e2e-mt-id-t1", uid)
        assert "UserOutputText" in r1.get("Data", {}), "Turn 1 应返回用户回复"

        # Turn 2: 问回名字（同一 session）
        r2 = _send_turn(port, "我叫什么？", "e2e-mt-id-t2", uid)
        assert "UserOutputText" in r2.get("Data", {}), "Turn 2 应返回用户回复"


@pytest.mark.e2e
class TestMultiturnPreference:
    """多轮对话：偏好记忆（"上海出差" → "推荐穿搭"）。"""

    def test_preference_remembered(self, outfit_daemon: dict[str, Any]) -> None:
        """Turn 1 报出差 → Turn 2 要推荐，agent 能正常响应两轮。"""
        port = outfit_daemon["daemon_port"]
        uid = "e2e-mt-pref"

        r1 = _send_turn(port, "我明天要去上海出差，喜欢商务休闲风格",
                        "e2e-mt-pref-t1", uid)
        assert "UserOutputText" in r1.get("Data", {}), "Turn 1 应返回回复"

        r2 = _send_turn(port, "帮我推荐穿搭", "e2e-mt-pref-t2", uid)
        assert "UserOutputText" in r2.get("Data", {}), "Turn 2 应返回回复"
