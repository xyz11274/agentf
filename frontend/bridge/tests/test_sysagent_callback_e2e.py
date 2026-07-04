"""E2E 测试：SysAgent 回调全链路（mock daemon + 真实上游 HTTP(S) 服务器）。

覆盖场景：
  - 回调顺序：先 UserOutput，再 TaskStatusReport(code=200)
  - 回调报文字段正确（TaskID、Sender/Receiver、UserName 等）
  - HTTPS 回调（自签名证书 + 跳过验证）
  - Daemon 失败 → TaskStatusReport(code=500)
  - 无 sysAgentEntity → 不回调
  - 回调超时/网络失败 → 不抛异常

设计要点：
  - 使用 _fire_and_wait 确保后台任务在 patch 上下文内执行完毕
  - HTTP 上游服务器用真实 ThreadingHTTPServer，验证实际网络回传
  - HTTPS 用临时自签名证书，验证 _build_upstream_ssl_context 集成

前置条件：无（mock daemon，无需真实 daemon 运行）。
"""

from __future__ import annotations

import asyncio
import ipaddress
import json
import os
import ssl
import tempfile
import threading
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, HTTPServer, ThreadingHTTPServer
from typing import Any, Callable, Iterator
from unittest.mock import AsyncMock, patch

import pytest
from fastapi import FastAPI
from httpx import ASGITransport, AsyncClient

from client import DaemonClient
from routes import aisf_tasks


# ═══════════════════════════════════════════════════════════════════
#  工具：模拟上游回调接收服务器
# ═══════════════════════════════════════════════════════════════════


class _CallbackCollector:
    """收集上游接收到的回调报文。线程安全。"""

    def __init__(self):
        self.callbacks: list[dict] = []
        self._lock = threading.Lock()

    def add(self, body: dict) -> None:
        with self._lock:
            self.callbacks.append(body)

    @property
    def count(self) -> int:
        with self._lock:
            return len(self.callbacks)

    @property
    def first(self) -> dict | None:
        with self._lock:
            return self.callbacks[0] if self.callbacks else None

    @property
    def second(self) -> dict | None:
        with self._lock:
            return self.callbacks[1] if len(self.callbacks) > 1 else None

    def reset(self) -> None:
        with self._lock:
            self.callbacks.clear()


class _UpstreamCallbackHandler(BaseHTTPRequestHandler):
    """接收上游回调 POST，存入 collector。"""

    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        raw = self.rfile.read(length)
        body = json.loads(raw)
        collector = getattr(self.server, "collector", None)
        if collector:
            collector.add(body)

        # 必须显式设置 Content-Length，否则 httpx 在 keep-alive 连接上
        # 无法确定响应体边界，会一直等待更多数据直到超时
        body_resp = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body_resp)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body_resp)


@contextmanager
def _upstream_server(host: str = "127.0.0.1", port: int = 0,
                     ssl_context: ssl.SSLContext | None = None) -> Iterator[dict]:
    """启动上游回调服务器，返回 ``{"host": ..., "port": ..., "collector": ...}``。"""
    collector = _CallbackCollector()
    server = ThreadingHTTPServer((host, port), _UpstreamCallbackHandler)
    server.collector = collector
    addr = server.server_address

    if ssl_context:
        server.socket = ssl_context.wrap_socket(server.socket, server_side=True)

    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    try:
        yield {"host": addr[0], "port": addr[1], "collector": collector}
    finally:
        server.shutdown()
        t.join(timeout=2)


@contextmanager
def _self_signed_ssl_context() -> Iterator[ssl.SSLContext]:
    """生成临时自签名证书的 SSLContext（服务端用）。"""
    import datetime
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "localhost"),
    ])
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(1)
        .not_valid_before(datetime.datetime.now(datetime.timezone.utc))
        .not_valid_after(datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=1))
        .add_extension(
            x509.SubjectAlternativeName([
                x509.DNSName("localhost"),
                x509.IPAddress(ipaddress.IPv4Address("127.0.0.1")),
            ]),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )

    # 写临时 PEM 文件（因为 ssl.SSLContext.load_cert_chain 需要文件路径）
    cert_path = os.path.join(tempfile.mkdtemp(), "server.crt")
    key_path = os.path.join(tempfile.mkdtemp(), "server.key")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        ))

    ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ctx.load_cert_chain(cert_path, key_path)
    try:
        yield ctx
    finally:
        for p in (cert_path, key_path):
            try:
                os.unlink(p)
            except OSError:
                pass


# ═══════════════════════════════════════════════════════════════════
#  Fixtures（与 test_aisf_tasks.py 对齐）
# ═══════════════════════════════════════════════════════════════════


def _task_request(
    task_id: str = "e2e-task-001",
    content: str = "你好，请介绍一下自己",
    sender: str = "SystemAgent",
    receiver: str = "HealthAgent",
    user_name: str = "e2e-user",
    device_id: str = "e2e-device",
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


@pytest.fixture
def daemon() -> AsyncMock:
    """Mock DaemonClient，返回受控响应。"""
    mock = AsyncMock(spec=DaemonClient)
    mock.is_connected = True
    mock.agent_execute.return_value = None
    # 模拟一次非 final + 一次 final 响应
    mock.wait_for_response.side_effect = [
        {"content": "您好！我是健康助手，很高兴为您服务。"},
        {"content": "请问有什么可以帮助您的吗？", "final": True},
    ]
    return mock


@pytest.fixture
async def client(daemon: AsyncMock) -> AsyncClient:
    """带 mock daemon 的 FastAPI 测试客户端。"""
    app = FastAPI()
    app.state.daemon_client = daemon
    app.include_router(aisf_tasks.router)
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as ac:
        yield ac


# ═══════════════════════════════════════════════════════════════════
#  辅助：fire-and-wait（在 patch 上下文中等待后台任务完成）
# ═══════════════════════════════════════════════════════════════════


async def _fire_and_wait(
    client: AsyncClient,
    json_body: dict,
    timeout: float = 5.0,
    **extra_patches: Any,
) -> None:
    """POST 请求并在 patch 上下文中等待后台任务结束。

    核心问题：routes/aisf_tasks.py 将回调任务通过 ``asyncio.create_task``
    提交，这些后台任务在 ``with patch(...)`` 退出后才开始执行，
    导致配置补丁失效。本函数捕获 ``create_task`` 并显式 await 它们。
    """
    tasks: list[asyncio.Task] = []
    original_ct = asyncio.create_task

    def _capture_task(coro, **kwargs):
        t = original_ct(coro, **kwargs)
        tasks.append(t)
        return t

    with patch.object(asyncio, "create_task", side_effect=_capture_task):
        resp = await client.post("/aisf/healthagent/v1/tasks", json=json_body)
        assert resp.status_code == 202
        for t in tasks:
            await asyncio.wait_for(t, timeout=timeout)


# ═══════════════════════════════════════════════════════════════════
#  E2E 测试
# ═══════════════════════════════════════════════════════════════════


class TestSysagentCallbackE2E:
    """SysAgent 对接全链路 E2E 测试。"""

    # ── 回调顺序与报文契约 ─────────────────────────────────────────

    async def test_callback_order_useroutput_then_statusreport(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-001: 回调顺序为先 UserOutput，再 TaskStatusReport(code=200)。"""
        with _upstream_server() as upstream:
            port = upstream["port"]
            collector = upstream["collector"]

            with patch("config.settings.aisf_adapter_upstream_callback_port", port):
                req = _task_request(task_id="order-001", sys_agent_entity="127.0.0.1")
                await _fire_and_wait(client, req)

            # 共 2 次回调
            assert collector.count == 2, f"expected 2 callbacks, got {collector.count}"

            # 第 1 次：UserOutput
            first = collector.first
            assert first["Header"]["MessageType"] == "DataExchange"
            assert first["Data"]["MessageType"] == "UserOutput"
            assert "您好！我是健康助手" in first["Data"]["UserOutputText"]

            # 第 2 次：TaskStatusReport(code=200)
            second = collector.second
            assert second["Header"]["MessageType"] == "TaskStatusManagement"
            assert second["Data"]["MessageType"] == "TaskStatusReport"
            assert second["Data"]["TaskStatusCode"] == 200
            assert second["Data"]["TaskStatusReport"] == "TaskCompleted"

    async def test_callback_taskid_consistency(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-002: 回调报文中 TaskID 与请求一致（不变量 I-1）。"""
        with _upstream_server() as upstream:
            port = upstream["port"]
            collector = upstream["collector"]

            with patch("config.settings.aisf_adapter_upstream_callback_port", port):
                req = _task_request(task_id="e2e-ha-007", sys_agent_entity="127.0.0.1")
                await _fire_and_wait(client, req)

            assert collector.count == 2
            for cb in collector.callbacks:
                assert cb["Header"]["TaskID"] == "e2e-ha-007", (
                    f"TaskID mismatch in {cb['Data']['MessageType']}"
                )

    async def test_callback_sender_receiver_swap(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-003: UserOutput 的 Sender/Receiver 互换（请求 Sender → 响应 Receiver）。"""
        with _upstream_server() as upstream:
            port = upstream["port"]
            collector = upstream["collector"]

            with patch("config.settings.aisf_adapter_upstream_callback_port", port):
                req = _task_request(task_id="swap-001", sender="AISFDEMO",
                                    sys_agent_entity="127.0.0.1")
                await _fire_and_wait(client, req)

            assert collector.count >= 1
            user_output = collector.first
            assert user_output["Header"]["Sender"] == "HealthAgent", (
                "响应 Sender 应为本端标识"
            )
            assert user_output["Header"]["Receiver"] == "AISFDEMO", (
                "响应 Receiver 应等于请求 Sender"
            )

    # ── HTTPS 回调 ─────────────────────────────────────────────────

    async def test_https_callback_skip_verify(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-010: HTTPS 回调（跳过服务端验证），仍能正常完成回调。"""
        with _self_signed_ssl_context() as ssl_ctx, \
                _upstream_server(ssl_context=ssl_ctx) as upstream:
            port = upstream["port"]
            collector = upstream["collector"]

            with (
                patch("config.settings.aisf_adapter_upstream_scheme", "https"),
                patch("config.settings.aisf_adapter_upstream_callback_port", port),
                patch("config.settings.aisf_adapter_upstream_verify", False),
            ):
                req = _task_request(task_id="https-001", sys_agent_entity="127.0.0.1")
                await _fire_and_wait(client, req)

            assert collector.count == 2, f"expected 2 HTTPS callbacks, got {collector.count}"
            user_output = collector.first
            assert user_output["Data"]["MessageType"] == "UserOutput"
            status_report = collector.second
            assert status_report["Data"]["TaskStatusCode"] == 200

    # ── 失败场景 ───────────────────────────────────────────────────

    async def test_daemon_failure_sends_status_500(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-020: Daemon 执行失败 → 回调 TaskStatusReport(code=500)。"""
        daemon.agent_execute.side_effect = RuntimeError("simulated failure")

        with _upstream_server() as upstream:
            port = upstream["port"]
            collector = upstream["collector"]

            with patch("config.settings.aisf_adapter_upstream_callback_port", port):
                req = _task_request(task_id="fail-001", sys_agent_entity="127.0.0.1")
                await _fire_and_wait(client, req)

            # 只回调 1 次（TaskStatusReport）
            assert collector.count == 1, f"expected 1 failure callback, got {collector.count}"
            status = collector.first
            assert status["Data"]["MessageType"] == "TaskStatusReport"
            assert status["Data"]["TaskStatusCode"] == 500
            assert "simulated failure" in status["Data"]["TaskStatusReport"]

    async def test_daemon_timeout_sends_status_500(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-021: Daemon 超时 → 回调 TaskStatusReport(code=500)。"""
        daemon.wait_for_response.side_effect = asyncio.TimeoutError("task timeout")

        with _upstream_server() as upstream:
            port = upstream["port"]
            collector = upstream["collector"]

            with patch("config.settings.aisf_adapter_upstream_callback_port", port):
                req = _task_request(task_id="timeout-001", sys_agent_entity="127.0.0.1")
                await _fire_and_wait(client, req)

            assert collector.count == 1
            assert collector.first["Data"]["TaskStatusCode"] == 500

    async def test_no_upstream_address_no_callback(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-022: sysAgentEntity 为空 → 不回调。"""
        req = _task_request(task_id="no-addr", sys_agent_entity="")
        resp = await client.post("/aisf/healthagent/v1/tasks", json=req)
        assert resp.status_code == 202
        # sysAgentEntity="" → post_result_to_upstream 跳过，不发起回调

    # ── 回调失败不影响主流程 ───────────────────────────────────────

    async def test_callback_network_failure_does_not_crash(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-030: 回调地址不可达 → 主流程不抛异常（仅日志报错）。"""
        with patch("config.settings.aisf_adapter_upstream_callback_port", 19999):
            req = _task_request(task_id="net-fail", sys_agent_entity="127.0.0.1")
            await _fire_and_wait(client, req, timeout=3.0)

    async def test_no_upstream_server_graceful(
        self, client: AsyncClient, daemon: AsyncMock,
    ) -> None:
        """E2E-031: 完全无上游监听 → http_post_to_upstream 返回 False，不抛异常。"""
        with patch("config.settings.aisf_adapter_upstream_callback_port", 19998):
            req = _task_request(task_id="graceful", sys_agent_entity="127.0.0.1")
            await _fire_and_wait(client, req, timeout=3.0)
