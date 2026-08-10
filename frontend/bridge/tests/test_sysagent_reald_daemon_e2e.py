"""真实 daemon E2E 测试：POST /aisf/healthagent/v1/tasks → Bridge → daemon → 上游回调。

全链路不跳过任何环节：
  1. 启动真实 agentflowd daemon（+ mock LLM 服务器）
  2. 启动上游回调 HTTP 服务器
  3. 创建 FastAPI app（挂载 aisf_tasks 路由 + 真实 DaemonClient）
  4. POST /aisf/healthagent/v1/tasks 发送 TASKASSIGNMENT
  5. 等待后台 _execute_and_callback 执行完毕
  6. 验证上游回调服务器收到 UserOutput → TaskStatusReport(code=200)

前置条件：
  - build/agentflowd 已编译
  - examples/agent/outfit-assistant 及其 .venv 已就绪

运行：
  python3 -m pytest tests/test_sysagent_reald_daemon_e2e.py -v -m e2e
"""

from __future__ import annotations

import asyncio
import json
import os
import signal
import socket
import subprocess
import threading
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Iterator

import pytest
from fastapi import FastAPI
from httpx import ASGITransport, AsyncClient

from client import DaemonClient
from config import settings
from routes.aisf_tasks import router as aisf_router

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
# 三态模型（删 examples 不崩不变量）：import 时自动从 ② 固化态重建 ③
from e2e_helpers import ensure_outfit_agent_3 as _ensure_3  # noqa: E402
OUTFIT_AGENT = _ensure_3()
LLM_REPLY = "你好，我是服装搭配助手，很高兴为您服务！"


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


# ═══════════════════════════════════════════════════════════════════
#  Mock LLM 服务器
# ═══════════════════════════════════════════════════════════════════


class _ScriptedLLMHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        self.rfile.read(length)
        payload = json.dumps({
            "id": "chatcmpl-e2e",
            "object": "chat.completion",
            "created": 1,
            "model": "e2e-scripted",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": LLM_REPLY},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args) -> None:
        pass


@contextmanager
def _scripted_llm() -> Iterator[str]:
    port = _free_port()
    server = ThreadingHTTPServer(("127.0.0.1", port), _ScriptedLLMHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}/v1"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


# ═══════════════════════════════════════════════════════════════════
#  上游回调服务器
# ═══════════════════════════════════════════════════════════════════


class _UpstreamHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        raw = self.rfile.read(length)
        body = json.loads(raw)
        collector = getattr(self.server, "collector", None)
        if collector is not None:
            collector.append(body)
        body_resp = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body_resp)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body_resp)

    def log_message(self, fmt, *args) -> None:
        pass


@contextmanager
def _upstream_server() -> Iterator[dict]:
    collector: list[dict] = []
    port = _free_port()
    server = ThreadingHTTPServer(("127.0.0.1", port), _UpstreamHandler)
    server.collector = collector
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield {"port": port, "callbacks": collector}
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


# ═══════════════════════════════════════════════════════════════════
#  工具函数
# ═══════════════════════════════════════════════════════════════════


def _wait_port(port: int, process: subprocess.Popen, timeout: float = 15) -> None:
    """等待 TCP 端口就绪。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"process exited (rc={process.returncode})")
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                s.connect(("127.0.0.1", port))
                return
        except (ConnectionRefusedError, OSError):
            pass
        time.sleep(0.2)
    raise RuntimeError(f"port {port} not ready within {timeout}s")


def _task_request(task_id: str = "e2e-real-001",
                  content: str = "你好") -> dict:
    return {
        "Header": {
            "MessageType": "UserTaskManagement",
            "TaskID": task_id,
            "Sender": "SystemAgent",
            "Receiver": "HealthAgent",
            "UserName": "e2e-user",
            "DeviceId": "e2e-device",
            "DeviceCategory": "2",
            "UUID": f"uuid-{task_id}",
            "ClientSessionId": f"session-{task_id}",
            "AcmSessionId": "",
            "DialogueId": "",
            "SessionId": "",
            "EventTimeStamp": "2026-07-03T12:00:00Z",
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
    }


# ═══════════════════════════════════════════════════════════════════
#  Fixture
# ═══════════════════════════════════════════════════════════════════


@pytest.fixture(scope="module")
def real_infra(tmp_path_factory: pytest.TempPathFactory) -> Iterator[dict[str, Any]]:
    """启动 daemon + mock LLM + 上游回调服务器，返回连接信息。"""
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = _free_port()

    with _scripted_llm() as llm_url, _upstream_server() as upstream:
        # Agent nodes call load_dotenv(override=True), so the real DashScope
        # URL in the agent .env file overrides any process env var we set.
        # Temporarily swap .env so the scripted LLM endpoint wins.
        agent_env_file = OUTFIT_AGENT / ".env"
        agent_env_backup = (
            agent_env_file.read_text() if agent_env_file.is_file() else None
        )
        agent_env_file.write_text(
            f"AF_LLM_BASE_URL={llm_url}\n"
            f"AF_LLM_API_KEY=e2e-test-key\n"
            f"AF_LLM_MODEL=e2e-scripted\n"
            f"AF_FALLBACK_LLM_BASE_URL={llm_url}\n"
            f"AF_FALLBACK_LLM_API_KEY=e2e-test-key\n"
            f"AF_FALLBACK_LLM_MODEL=e2e-scripted\n"
        )
        env = {
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            ),
            "AF_LLM_API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            # 本测试聚焦 Bridge → daemon → 上游回调链路（LLM 已 mock），
            # 不依赖远程 E2B 沙箱：跳过沙箱创建，避免 e2b 创建耗时
            # 30s（create_latency≈30s）拖垮 agent.execute 响应超时。
            "AF_SANDBOX_MODE": "none",
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
        }
        tmp_path = tmp_path_factory.mktemp("real-e2e")

        daemon = subprocess.Popen(
            [
                str(DAEMON_BINARY),
                "--listen", f"127.0.0.1:{daemon_port}",
                "--package", str(OUTFIT_AGENT),
            ],
            cwd=PROJECT_ROOT,
            env=env,
            stdout=(tmp_path / "daemon.log").open("wb"),
            stderr=subprocess.STDOUT,
        )
        try:
            _wait_port(daemon_port, daemon)
            # 额外等 agent 注册
            _await_agent_ready(daemon_port, "outfit-assistant", daemon)

            yield {
                "daemon_port": daemon_port,
                "upstream_port": upstream["port"],
                "callbacks": upstream["callbacks"],
            }
        finally:
            if daemon.poll() is None:
                daemon.send_signal(signal.SIGTERM)
                try:
                    daemon.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=3)
            if agent_env_backup is not None:
                agent_env_file.write_text(agent_env_backup)


def _await_agent_ready(port: int, agent_id: str, process: subprocess.Popen) -> None:
    """通过 WebSocket 等待 agent 注册。使用 asyncio.run 隔离事件循环。"""
    import websockets
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited (rc={process.returncode})")
        try:
            resp = asyncio.run(_ws_list_agents(port))
            agents = resp.get("result", {}).get("agents", [])
            if any(a.get("agent_id") == agent_id for a in agents):
                return
        except Exception:
            pass
        time.sleep(0.2)
    raise RuntimeError(f"agent {agent_id} not registered within 15s")


async def _ws_list_agents(port: int) -> dict:
    import websockets
    async with websockets.connect(
        f"ws://127.0.0.1:{port}", open_timeout=3, close_timeout=1,
    ) as ws:
        await ws.send(json.dumps({
            "jsonrpc": "2.0", "method": "agent.list",
            "params": {}, "id": 1,
        }))
        return json.loads(await asyncio.wait_for(ws.recv(), timeout=3))


# ═══════════════════════════════════════════════════════════════════
#  测试：全链路 Bridge → daemon → 上游回调
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
@pytest.mark.slow
class TestSysagentRealDaemonE2E:
    """全链路 E2E（@e2e 标记，默认跳过）。"""

    async def test_full_bridge_to_upstream_callback(
        self, real_infra: dict[str, Any],
    ) -> None:
        """发 TASKASSIGNMENT → Bridge 路由 → daemon 执行 → HTTP 回调上游。

        验证：
          - 返回 202 accepted
          - daemon 执行 outfit-assistant agent 并返回 LLM 回复
          - 上游服务器收到 2 次回调
          - 回调顺序：UserOutput 先 → TaskStatusReport(code=200) 后
          - UserOutputText 包含 mock LLM 回复
        """
        dport = real_infra["daemon_port"]
        uport = real_infra["upstream_port"]
        callbacks = real_infra["callbacks"]

        # ── 1. 创建桥接 app + 连接 DaemonClient ───────────────────
        app = FastAPI()
        app.include_router(aisf_router)

        # ASGITransport 不执行 lifespan，需手动连接 daemon
        daemon_client = DaemonClient(
            host="127.0.0.1", port=dport, timeout=30,
        )
        await daemon_client.connect()
        await asyncio.sleep(0.3)  # 等待 _ws_reader task 启动+握手
        app.state.daemon_client = daemon_client

        # ── 2. 注入配置 ────────────────────────────────────────────
        settings.aisf_adapter_upstream_callback_port = uport
        settings.aisf_adapter_upstream_scheme = "http"
        # 保证 daemon 任务有足够时间
        settings.aisf_adapter_timeout = 60.0
        settings.request_timeout_seconds = 30.0

        # ── 3. 发送请求 ───────────────────────────────────────────
        async with AsyncClient(
            transport=ASGITransport(app=app), base_url="http://test",
        ) as client:
            resp = await client.post(
                "/aisf/healthagent/v1/tasks",
                json=_task_request(task_id="e2e-real-001", content="你好"),
            )
        assert resp.status_code == 202, f"预期 202，实际 {resp.status_code}: {resp.text}"
        task_id = resp.json()["task_id"]
        assert task_id == "e2e-real-001"

        # ── 4. 等待回调 ───────────────────────────────────────────
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if len(callbacks) >= 2:
                break
            await asyncio.sleep(0.3)

        # ── 5. 验证 ───────────────────────────────────────────────
        assert len(callbacks) == 2, (
            f"预期 2 次回调，实际 {len(callbacks)}:\n"
            + "\n".join(json.dumps(cb, ensure_ascii=False)[:200] for cb in callbacks)
        )

        # 第 1 次：UserOutput
        user_output = callbacks[0]
        assert user_output["Header"]["MessageType"] == "DataExchange", (
            f"UserOutput MessageType mismatch: {user_output['Header']['MessageType']}"
        )
        assert user_output["Data"]["MessageType"] == "UserOutput"
        assert LLM_REPLY in user_output["Data"]["UserOutputText"], (
            f"UserOutputText 应包含 '{LLM_REPLY}'，实际: {user_output['Data']['UserOutputText']}"
        )
        assert user_output["Header"]["TaskID"] == "e2e-real-001"
        assert user_output["Header"]["Sender"] == "HealthAgent"
        assert user_output["Header"]["Receiver"] == "SystemAgent"

        # 第 2 次：TaskStatusReport(code=200)
        status = callbacks[1]
        assert status["Header"]["MessageType"] == "TaskStatusManagement"
        assert status["Data"]["MessageType"] == "TaskStatusReport"
        assert status["Data"]["TaskStatusCode"] == 200, (
            f"预期 code=200，实际 {status['Data']['TaskStatusCode']}"
        )
        assert status["Data"]["TaskStatusReport"] == "TaskCompleted"
