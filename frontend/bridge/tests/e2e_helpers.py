"""
E2E 测试共享辅助函数 — daemon 生命周期 + mock LLM + JSON-RPC 通信。

设计原则：
  1. 每个 test class 启动自己的 daemon（≤5 次请求后 daemon 退化，class scope 避免累积）
  2. 使用 mock LLM server（e2e-scripted），不依赖真实 API
  3. 验证执行链完整性（节点 Exit code=0、response 送达），不做 LLM 输出内容断言
  4. fixture 确保 daemon 在 teardown 时被终止
"""

from __future__ import annotations

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

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
OUTFIT_AGENT = PROJECT_ROOT / "examples" / "agent" / "outfit_assistant"
LLM_REPLY = "你好，我是服装搭配助手，很高兴为您服务！"


def free_port() -> int:
    """获取随机可用端口。"""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


# ═══════════════════════════════════════════════════════════════════
# Mock LLM 服务器（返回固定回复 e2e-scripted）
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
def scripted_llm() -> Iterator[str]:
    """启动 mock LLM HTTP server，yield base URL。"""
    port = free_port()
    server = ThreadingHTTPServer(("127.0.0.1", port), _ScriptedLLMHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}/v1"
    finally:
        server.shutdown()
        server.server_close()


# ═══════════════════════════════════════════════════════════════════
# Daemon 生命周期
# ═══════════════════════════════════════════════════════════════════


def wait_port(port: int, process: subprocess.Popen, timeout: float = 15) -> None:
    """等待 TCP 端口就绪。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited (rc={process.returncode})")
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                s.connect(("127.0.0.1", port))
                return
        except (ConnectionRefusedError, OSError):
            pass
        time.sleep(0.2)
    raise RuntimeError(f"port {port} not ready within {timeout}s")


def await_agent_ready(port: int, agent_id: str, process: subprocess.Popen,
                      timeout: float = 20) -> None:
    """等待 agent 注册完成（通过 list_agents JSON-RPC）。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited (rc={process.returncode})")
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(1)
                s.connect(("127.0.0.1", port))
                req = json.dumps({
                    "jsonrpc": "2.0", "method": "list_agents",
                    "params": {}, "id": 1,
                })
                s.sendall((req + "\n").encode("utf-8"))
                chunks = []
                while True:
                    ch = s.recv(1)
                    if not ch or ch == b"\n":
                        break
                    chunks.append(ch)
                resp = json.loads(b"".join(chunks).decode("utf-8"))
                agents = resp.get("result", {}).get("agents", [])
                if any(a.get("id") == agent_id for a in agents):
                    return
        except (ConnectionRefusedError, OSError, json.JSONDecodeError, KeyError):
            pass
        time.sleep(0.3)
    raise RuntimeError(f"agent {agent_id} not ready within {timeout}s")


@pytest.fixture(scope="class")
def outfit_daemon(request: pytest.FixtureRequest,
                  tmp_path_factory: pytest.TempPathFactory) -> Iterator[dict[str, Any]]:
    """启动 outfits_assistant daemon（class-scoped，每个 class 一个独立实例）。

    每个 class 启动一个新的 daemon 进程，class 内测试共享。
    该限制确保 daemon 退化问题（~5 次请求后退化）不跨 class 累积。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = free_port()
    tmp_path = tmp_path_factory.mktemp(f"e2e-outfit-{request.cls.__name__}")

    with scripted_llm() as llm_url:
        env = {
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            ),
            "API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
        }

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
            wait_port(daemon_port, daemon)
            await_agent_ready(daemon_port, "outfit-assistant", daemon)
            yield {"daemon_port": daemon_port, "daemon": daemon}
        finally:
            if daemon.poll() is None:
                daemon.send_signal(signal.SIGTERM)
                try:
                    daemon.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=3)


# ═══════════════════════════════════════════════════════════════════
# JSON-RPC 通信
# ═══════════════════════════════════════════════════════════════════


def jsonrpc_call(host: str, port: int, method: str,
                 params: dict, req_id: int = 1) -> dict:
    """发送 JSON-RPC 请求到 daemon，返回 \n 分隔的单行响应。"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(30)
        sock.connect((host, port))
        payload = json.dumps({
            "jsonrpc": "2.0", "method": method,
            "params": params, "id": req_id,
        })
        sock.sendall((payload + "\n").encode("utf-8"))
        chunks = []
        while True:
            ch = sock.recv(1)
            if not ch or ch == b"\n":
                break
            chunks.append(ch)
        return json.loads(b"".join(chunks).decode("utf-8"))


def send_user_message(host: str, port: int, content: str,
                      task_id: str = "e2e-outfit-001") -> dict:
    """通过 daemon 直接发送用户消息（JSON-RPC TASKASSIGNMENT），返回响应。"""
    return jsonrpc_call(host, port, "TASKASSIGNMENT", {
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
    }, task_id)
