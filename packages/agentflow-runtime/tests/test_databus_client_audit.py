"""DataBusClient.response_push 集中审计日志测试。

架构修复 W2(2026-08-14):response_push 调用点分散在 5 个模块(skill_executor /
coordinator_decide / coordinator_agent_loop / recorder / openai_compatible_client),
统一封装 databus_client.response_push 已有失败日志与 timing,但缺**集中审计日志**
(哪一模块、何时推送、type/source/status 上下文)。补审计日志使全链路推送可追溯,
与 F7.1 外部请求日志同范式。

不变量:response_push 成功时 stderr 必须打印含调用上下文(source/type/status)的
审计行;失败时保留原有 WARN + 审计行(可定位推送失败的模块与内容)。
"""

from __future__ import annotations

from unittest.mock import MagicMock

from agentflow_runtime.databus_client import DataBusClient


def _make_bus(client: MagicMock | None = None) -> DataBusClient:
    """构造 DataBusClient 实例,绕过 __init__ 的真实 UDS 连接。"""
    bus = DataBusClient.__new__(DataBusClient)
    bus._session_id = "0"
    bus._dialogue_id = "d_audit"
    bus._client = client
    return bus


def test_response_push_success_prints_audit_log(capsys) -> None:
    """成功推送打印集中审计日志,含调用上下文(source/type/status)。"""
    client = MagicMock()
    client.response_push.return_value = True
    bus = _make_bus(client)

    ok = bus.response_push(
        "progress text", type="progress", source="skill_executor",
        skill="outfit-stylist", status="running",
    )

    assert ok is True
    err = capsys.readouterr().err
    assert "[audit] databus.response_push" in err, f"缺审计日志, stderr: {err}"
    assert "source=skill_executor" in err, f"审计日志缺 source, stderr: {err}"
    assert "type=progress" in err, f"审计日志缺 type, stderr: {err}"
    assert "status=running" in err, f"审计日志缺 status, stderr: {err}"


def test_response_push_failure_prints_warn_and_audit(capsys) -> None:
    """失败时保留 WARN(可定位根因)+ 审计日志(可定位模块与内容)。"""
    client = MagicMock()
    client.response_push.side_effect = RuntimeError("uds broken")
    bus = _make_bus(client)

    ok = bus.response_push("progress text", type="progress", source="coordinator_decide")

    assert ok is False
    err = capsys.readouterr().err
    assert "response_push failed" in err, f"缺失败 WARN, stderr: {err}"
    assert "uds broken" in err, f"WARN 缺异常信息, stderr: {err}"
    assert "[audit] databus.response_push" in err, f"失败也应打印审计日志, stderr: {err}"
    assert "source=coordinator_decide" in err, f"审计日志缺 source, stderr: {err}"


def test_response_push_disconnected_skips_audit(capsys) -> None:
    """未连接(UDS 不可用)时返回 False,不打印审计日志(无推送发生)。"""
    bus = _make_bus(None)

    ok = bus.response_push("progress text", type="progress", source="recorder")

    assert ok is False
    err = capsys.readouterr().err
    assert "[audit] databus.response_push" not in err, f"未推送不应有审计日志, stderr: {err}"
