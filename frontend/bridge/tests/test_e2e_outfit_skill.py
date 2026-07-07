"""
穿搭助手 skill 加载 E2E 测试 — pytest + daemon fixture 版。

验证 coordinator 能正常调度 skill（weather_query 等）并返回响应。
每个 test class 启动独立的 daemon fixture，不依赖真实 LLM API。

与 test_e2e_outfit_agent.py 的区别：这里专注 skill 加载 + 工具共存测试。
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


def _send_msg(port: int, content: str, task_id: str,
              uid: str = "e2e-sk-user") -> dict:
    """发送消息并等响应。"""
    jsonrpc_call("127.0.0.1", port, "TASKASSIGNMENT", {
        "Header": {
            "MessageType": "UserTaskManagement",
            "TaskID": task_id,
            "Sender": "SystemAgent",
            "Receiver": "HealthAgent",
            "UserName": uid,
            "DeviceId": "e2e-device",
            "ClientSessionId": "e2e-sk-session",
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
    })
    return _poll_response(port, task_id)


@pytest.mark.e2e
class TestSkillLoading:
    """weather_query skill 加载和执行。"""

    def test_weather_query(self, outfit_daemon: dict[str, Any]) -> None:
        """发送天气查询，验证 skill 被正常调度并返回。"""
        port = outfit_daemon["daemon_port"]
        result = _send_msg(port, "北京今天天气怎么样？", "e2e-sk-weather")
        assert "UserOutputText" in result.get("Data", {}), "天气查询应返回用户回复"


@pytest.mark.e2e
class TestToolCoexistence:
    """原始工具（profile_manager）与 skill 共存。"""

    def test_profile_manager_tool(self, outfit_daemon: dict[str, Any]) -> None:
        """profile_manager 工具仍正常工作。"""
        port = outfit_daemon["daemon_port"]
        result = _send_msg(port, "我叫张三", "e2e-sk-profile")
        assert "UserOutputText" in result.get("Data", {}), "画像工具应返回回复"


@pytest.mark.e2e
class TestMultiTurnWithSkill:
    """多轮对话 + skill 共存：先设用户信息，再问天气推荐。"""

    def test_multiturn_skill_coexistence(self, outfit_daemon: dict[str, Any]) -> None:
        """Turn 1 设画像 → Turn 2 混合天气 and 穿搭。"""
        port = outfit_daemon["daemon_port"]
        uid = "e2e-sk-mt"

        r1 = _send_msg(port, "我叫张三，明天要去上海出差",
                       "e2e-sk-mt-t1", uid)
        assert "UserOutputText" in r1.get("Data", {}), "Turn 1 应返回回复"

        r2 = _send_msg(port, "帮我推荐穿搭", "e2e-sk-mt-t2", uid)
        assert "UserOutputText" in r2.get("Data", {}), "Turn 2 应返回回复"
