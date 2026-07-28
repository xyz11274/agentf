"""Process-level E2E coverage for the C++ CoreMind SideCar."""

from __future__ import annotations

import contextlib
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from collections.abc import Iterator
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import httpx
import pytest

PROJECT_ROOT = Path(__file__).resolve().parents[3]
SIDECAR_SOURCE = PROJECT_ROOT / "sidecar"
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
FIXTURE_AGENT = Path(__file__).parent / "fixtures" / "coremind_context_agent"


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class _BridgeHandler(BaseHTTPRequestHandler):
    requests: list[dict[str, Any]] = []

    def do_POST(self) -> None:  # noqa: N802
        body = self.rfile.read(int(self.headers.get("content-length", "0")))
        self.requests.append({
            "path": self.path,
            "body": body,
            "request_id": self.headers.get("x-request-id"),
            "authorization": self.headers.get("authorization"),
            "content_type": self.headers.get("content-type"),
        })
        response = json.dumps({
            "task_id": "test-task",
            "content": "node-ok",
        }).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json; charset=utf-8")
        self.send_header("content-length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, *_args: Any) -> None:
        return


@pytest.fixture(scope="module")
def sidecar_binary(tmp_path_factory: pytest.TempPathFactory) -> Path:
    if shutil.which("cmake") is None:
        pytest.skip("cmake is required for the C++ SideCar E2E")

    build_dir = tmp_path_factory.mktemp("coremind-sidecar-build")
    prefix_paths = [
        path
        for path in (
            "/opt/homebrew/opt/cpp-httplib",
            "/opt/homebrew/opt/jsoncpp",
        )
        if Path(path).exists()
    ]
    command = [
        "cmake", "-S", str(SIDECAR_SOURCE), "-B", str(build_dir),
        "-DCOREMIND_ENABLED=OFF",
    ]
    if prefix_paths:
        command.append(f"-DCMAKE_PREFIX_PATH={';'.join(prefix_paths)}")
    configured = subprocess.run(command, capture_output=True, text=True)
    if configured.returncode != 0 and (
        "httplib" in configured.stderr or "jsoncpp" in configured.stderr
    ):
        pytest.skip(f"SideCar build dependencies unavailable: {configured.stderr[-500:]}")
    assert configured.returncode == 0, configured.stdout + configured.stderr

    built = subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel", "2"],
        capture_output=True,
        text=True,
    )
    assert built.returncode == 0, built.stdout + built.stderr
    return build_dir / "coremind_bridge"


@contextlib.contextmanager
def _bridge_server() -> Iterator[tuple[str, list[dict[str, Any]]]]:
    _BridgeHandler.requests = []
    server = ThreadingHTTPServer(("127.0.0.1", 0), _BridgeHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}", _BridgeHandler.requests
    finally:
        server.shutdown()
        thread.join(timeout=3)
        server.server_close()


@contextlib.contextmanager
def _sidecar(
    binary: Path,
    bridge_url: str,
    extra_env: dict[str, str] | None = None,
) -> Iterator[str]:
    port = _free_port()
    process = subprocess.Popen(
        [
            str(binary),
            "--listen", f"127.0.0.1:{port}",
            "--bridge-url", bridge_url,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env={
            **os.environ,
            "NO_PROXY": "127.0.0.1,localhost",
            **(extra_env or {}),
        },
    )
    base_url = f"http://127.0.0.1:{port}"
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                pytest.fail(f"SideCar exited during startup:\n{process.stdout.read()}")
            try:
                if httpx.get(f"{base_url}/health", timeout=0.3).status_code == 200:
                    break
            except httpx.HTTPError:
                time.sleep(0.05)
        else:
            pytest.fail("SideCar did not become healthy within 10 seconds")
        yield base_url
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if process.stdout is not None:
            process.stdout.close()


@pytest.mark.e2e
def test_sidecar_local_platform_contract(sidecar_binary: Path) -> None:
    """TC-brg-019: C++ SideCar 本地端点与客户端契约一致。

    业务规则:
    - 健康探测、能力宣告和 Agent 管理必须由唯一外部入口直接提供。

    错误模型:
    - `/health` 与 `/healthcheck` 漂移，或本地管理路由遗漏。

    失败意味着:
    - 启动脚本或 Python 客户端会把健康的 SideCar 判为不可用。
    """
    with _sidecar(sidecar_binary, "http://127.0.0.1:1") as base_url:
        for path in ("/health", "/healthcheck"):
            response = httpx.get(f"{base_url}{path}")
            assert response.status_code == 200
            assert response.json() == {
                "coremind": True,
                "sidecar": True,
                "status": "healthy",
            }

        ability = httpx.get(f"{base_url}/sysagent/v1/scheduleTaskAbility")
        assert ability.status_code == 200
        assert ability.json()["endpoint"] == "/coremind/systemagent/v1/callback"

        for action in ("Install", "Update", "Uninstall"):
            response = httpx.post(f"{base_url}/aisf/sysagent/v1/agent{action}")
            assert response.status_code == 200
            assert response.json()["action"] == action.lower()


@pytest.mark.e2e
@pytest.mark.parametrize("path", [
    "/coremind/systemagent/v1/callback",
    "/health/v1/tasks",
    "/aisf/outfit/v1/tasks",
])
def test_sidecar_builds_trusted_context_for_bridge(
    sidecar_binary: Path,
    path: str,
) -> None:
    """TC-brg-020: SideCar 生成请求级可信上下文并单向转发到 Bridge。

    业务规则:
    - 三个公开入口必须统一构造内部 envelope，且 Bridge 不反向查询 SideCar。

    错误模型:
    - 路由别名漏接、原始请求丢失或平台上下文未绑定当前请求。

    失败意味着:
    - Bridge/Node 无法消费 SideCar 的服务发现结果。
    """
    payload = {
        "Header": {"UserName": "6d64bb1e63e24c5d", "ClientSessionId": "trace-request-id",
                    "TaskID": f"task-{path.rsplit('/', 1)[-1]}"},
        "Data": {"UserInputText": "test request"},
    }
    with (
        _bridge_server() as (bridge_url, received),
        _sidecar(sidecar_binary, bridge_url) as base_url,
    ):
        response = httpx.post(
            f"{base_url}{path}",
            json=payload,
            headers={"x-request-id": "trace-sidecar-e2e"},
        )

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("application/json")
    assert response.json()["Data"]["UserOutputText"] == "node-ok"
    assert len(received) == 1
    assert received[0]["path"] == "/api/chat/outfit-assistant?stream=false"
    envelope = json.loads(received[0]["body"])
    assert envelope["user_id"]  # user_id is extracted from Header.UserName
    assert envelope["client_id"]  # client_id is extracted from Header.ClientSessionId
    assert envelope["content"]  # multimodal OpenAI format
    assert received[0]["request_id"] == "trace-sidecar-e2e"
    assert received[0]["content_type"].startswith("application/json")


@pytest.mark.e2e
def test_sidecar_reports_dependency_failures(sidecar_binary: Path) -> None:
    """TC-brg-021: SideCar 对不可用依赖返回有限且明确的失败。

    业务规则:
    - Bridge 或 CoreMind 不可用时不得返回伪成功或无限等待。

    错误模型:
    - 连接失败被吞掉，或本地 E2E 构建伪造服务发现成功。

    失败意味着:
    - 上游无法区分已接受任务与内部依赖中断。
    """
    with _sidecar(sidecar_binary, "http://127.0.0.1:1") as base_url:
        forwarded = httpx.post(
            f"{base_url}/coremind/systemagent/v1/callback",
            json={"eventType": "TASKASSIGNMENT"},
            timeout=3,
        )
        resolved = httpx.get(f"{base_url}/resolve/IMInferScheduler")
        batch = httpx.post(
            f"{base_url}/resolve/batch",
            json={"services": ["IMInferScheduler"]},
        )

    assert forwarded.status_code == 502
    assert forwarded.json()["error"] == "BRIDGE_UNREACHABLE"
    assert resolved.status_code == 503
    assert "disabled" in resolved.json()["error"]
    assert batch.status_code == 200
    assert batch.json()["status"] == "partial"
    assert batch.json()["services"]["IMInferScheduler"]["status"] == "error"


def _wait_http(url: str, process: subprocess.Popen, timeout: float = 12.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            pytest.fail(f"process exited before readiness: {url}")
        try:
            if httpx.get(url, timeout=0.4).status_code == 200:
                return
        except httpx.HTTPError:
            time.sleep(0.05)
    pytest.fail(f"process did not become ready: {url}")


def _stop(process: subprocess.Popen, log_file) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)
    log_file.close()


@pytest.mark.e2e
@pytest.mark.slow
def test_user_request_reaches_node_with_trusted_context(
    sidecar_binary: Path,
    tmp_path: Path,
) -> None:
    """TC-brg-024: 用户请求经 SideCar、Bridge、AgentFlow 到达真实 Node。

    业务规则:
    - Node 必须收到 SideCar 为本次请求解析的服务快照和 request_id。

    错误模型:
    - 任一跨进程边界丢失 platform_context，或实际链路绕过 Bridge/AgentFlow。

    失败意味着:
    - 单组件测试通过，但生产完整链路中的 Node 仍无法使用服务发现结果。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = _free_port()
    bridge_port = _free_port()
    daemon_log_file = (tmp_path / "daemon.log").open("wb")
    bridge_log_file = (tmp_path / "bridge.log").open("wb")
    env = {
        **os.environ,
        "AF_BR_DAEMON_HOST": "127.0.0.1",
        "AF_BR_DAEMON_PORT": str(daemon_port),
        "AF_BR_LISTEN": f"127.0.0.1:{bridge_port}",
        "AF_BR_COREMIND_AGENT_ID": "coremind-context-agent",
        "PYTHONPATH": str(PROJECT_ROOT / "frontend" / "bridge"),
        "NO_PROXY": "127.0.0.1,localhost",
        "no_proxy": "127.0.0.1,localhost",
    }
    daemon = subprocess.Popen(
        [
            str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
            "--package", str(FIXTURE_AGENT),
        ],
        cwd=PROJECT_ROOT,
        env=env,
        stdout=daemon_log_file,
        stderr=subprocess.STDOUT,
    )
    bridge: subprocess.Popen | None = None
    try:
        # The Bridge health endpoint does not require daemon readiness; wait for
        # the daemon TCP listener before starting it.
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", daemon_port), timeout=0.3):
                    break
            except OSError:
                if daemon.poll() is not None:
                    pytest.fail((tmp_path / "daemon.log").read_text(errors="replace"))
                time.sleep(0.05)
        bridge = subprocess.Popen(
            [
                sys.executable, "-m", "uvicorn", "main:app",
                "--app-dir", str(PROJECT_ROOT / "frontend" / "bridge"),
                "--host", "127.0.0.1", "--port", str(bridge_port),
                "--log-level", "warning",
            ],
            cwd=PROJECT_ROOT,
            env=env,
            stdout=bridge_log_file,
            stderr=subprocess.STDOUT,
        )
        _wait_http(f"http://127.0.0.1:{bridge_port}/health/api/health", bridge)
        with _sidecar(
            sidecar_binary,
            f"http://127.0.0.1:{bridge_port}/healthagent",
            {
                "COREMIND_REQUIRED_SERVICES": "IMInferScheduler",
                "COREMIND_STUB_SERVICES": "IMInferScheduler=10.0.0.5:27321",
            },
        ) as sidecar_url:
            response = httpx.post(
                f"{sidecar_url}/coremind/systemagent/v1/callback",
                headers={"X-Request-Id": "request-full-chain"},
                json={
                    "Header": {
                        "TaskID": "task-full-chain",
                        "UserName": "e2e-user",
                        "DeviceId": "e2e-device",
                    },
                    "Data": {
                        "MessageType": "TaskAssignment",
                        "UserInputText": "请验证可信上下文",
                    },
                },
                timeout=30,
            )
        assert response.status_code == 200, response.text
        assert response.json()["data"]["content"] == (
            "NODE_CONTEXT_OK:request-full-chain:10.0.0.5:27321"
        )
    finally:
        if bridge is not None:
            _stop(bridge, bridge_log_file)
        else:
            bridge_log_file.close()
        _stop(daemon, daemon_log_file)


# ── UDS LLM 测试 ─────────────────────────────────────────────────

UDS_TEST_SOCKET = "/tmp/test_coremind_sidecar_uds.sock"


def send_uds_frame(sock: socket.socket, data: dict[str, Any]) -> None:
    """发送 UDS 帧：4-byte BE 长度 + UTF-8 JSON。"""
    payload = json.dumps(data, ensure_ascii=False).encode("utf-8")
    header = struct.pack(">I", len(payload))
    sock.sendall(header + payload)


def recv_uds_frame(sock: socket.socket) -> dict[str, Any] | None:
    """接收 UDS 帧：先读 4-byte 长度，再读负载。"""
    len_buf = sock.recv(4)
    if len(len_buf) < 4:
        return None
    frame_len = struct.unpack(">I", len_buf)[0]
    chunks: list[bytes] = []
    remaining = frame_len
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return json.loads(b"".join(chunks))


class _IMInferHandler(BaseHTTPRequestHandler):
    """Mock IMInferScheduler — 接收 LLM 请求并返回固定响应。"""
    received_requests: list[dict[str, Any]] = []

    def do_POST(self) -> None:  # noqa: N802
        body = self.rfile.read(int(self.headers.get("content-length", "0")))
        self.received_requests.append({
            "path": self.path,
            "body": json.loads(body),
        })
        resp = json.dumps({
            "content": "Mock IMInferScheduler E2E OK",
            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
        }).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *_args: Any) -> None:
        return


@contextlib.contextmanager
def _im_infer_server() -> Iterator[tuple[str, int]]:
    """启动 mock IMInferScheduler HTTP 服务器。"""
    _IMInferHandler.received_requests = []
    server = ThreadingHTTPServer(("127.0.0.1", 0), _IMInferHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield "127.0.0.1", server.server_port
    finally:
        server.shutdown()
        thread.join(timeout=3)
        server.server_close()


@pytest.mark.e2e
def test_sidecar_uds_llm_forwarding(sidecar_binary: Path) -> None:
    """TC-brg-026: SideCar UDS LLM 代理 — 接收 Node 的 llm.chat 请求并转发到后端。

    业务规则:
    - Node 通过 UDS 发送 llm.chat 请求（4-byte 长度帧协议）
    - SideCar 接收后通过 httplib 转发到 LLM 后端（本地 dev 路径）
    - 响应以 llm_final 事件回传

    错误模型:
    - UDS 帧协议不匹配（长度/JSON 格式）
    - 后端不可用时返回 llm_error 而非 hang
    """
    uds_path = UDS_TEST_SOCKET
    if os.path.exists(uds_path):
        os.unlink(uds_path)

    with _im_infer_server() as (im_host, im_port):
        port = _free_port()
        proc = subprocess.Popen(
            [
                str(sidecar_binary),
                "--listen", f"127.0.0.1:{port}",
                "--bridge-url", "http://127.0.0.1:1",
                "--uds-socket", uds_path,
                "--llm-api-url", f"http://{im_host}:{im_port}",
            ],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env={**os.environ, "NO_PROXY": "127.0.0.1,localhost"},
        )
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    pytest.fail(f"SideCar exited:\n{proc.stdout.read()}")
                try:
                    if httpx.get(f"http://127.0.0.1:{port}/health", timeout=0.3).status_code == 200:
                        break
                except httpx.HTTPError:
                    time.sleep(0.05)
            else:
                pytest.fail("SideCar did not become healthy")

            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(5.0)
            sock.connect(uds_path)

            request = {
                "method": "llm.chat",
                "request_id": "e2e-uds-001",
                "params": {
                    "request": {
                        "model": "aisf/test-model",
                        "messages": [{"role": "user", "content": "Hello"}],
                        "stream": False,
                    },
                },
            }
            send_uds_frame(sock, request)

            response = recv_uds_frame(sock)
            assert response is not None, "No UDS response received"
            assert response.get("event_type") == "llm_final", \
                f"Expected llm_final, got {response.get('event_type')}"
            assert response.get("done") is True

            assert _IMInferHandler.received_requests, \
                "IMInferScheduler received no requests"
            received_path = _IMInferHandler.received_requests[0]["path"]
            # dev 模式走 OpenAI 兼容路径 /chat/completions
            assert received_path == "/chat/completions", \
                f"Expected /chat/completions, got {received_path}"
        finally:
            if os.path.exists(uds_path):
                os.unlink(uds_path)
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2)
