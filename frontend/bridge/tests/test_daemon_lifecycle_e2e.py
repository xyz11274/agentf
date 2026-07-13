"""Real-process regression test for agentflowd shutdown and restart."""

from __future__ import annotations

import asyncio
import json
import os
import signal
import socket
import subprocess
import time
from pathlib import Path
from typing import Any

import pytest
import websockets


PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
HARNESS_AGENT = PROJECT_ROOT / "frontend" / "bridge" / "templates" / "agent_template" / "harness_agent"


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


async def _request(port: int, method: str, params: dict[str, Any]) -> dict[str, Any]:
    async with websockets.connect(
        f"ws://127.0.0.1:{port}", open_timeout=1, close_timeout=1
    ) as ws:
        await ws.send(json.dumps({
            "jsonrpc": "2.0",
            "method": method,
            "params": params,
            "id": 1,
        }))
        return json.loads(await asyncio.wait_for(ws.recv(), timeout=3))


def _wait_ready(process: subprocess.Popen[bytes], port: int) -> None:
    deadline = time.monotonic() + 12
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"daemon exited during startup with code {process.returncode}")
        try:
            response = asyncio.run(_request(port, "agent.list", {}))
            if "result" in response:
                return
        except Exception as exc:  # readiness polling intentionally retries
            last_error = exc
        time.sleep(0.1)
    raise AssertionError(f"daemon did not become ready: {last_error}")


def _start_daemon(port: int, log_path: Path) -> tuple[subprocess.Popen[bytes], Any]:
    log_file = log_path.open("wb")
    process = subprocess.Popen(
        [
            str(DAEMON_BINARY),
            "--listen",
            f"127.0.0.1:{port}",
            "--package",
            str(HARNESS_AGENT),
        ],
        cwd=HARNESS_AGENT,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
    return process, log_file


def _stop_daemon(process: subprocess.Popen[bytes], log_file: Any) -> int:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
            raise AssertionError("daemon did not exit within 12s after SIGTERM")
        finally:
            log_file.close()
    else:
        log_file.close()
    return int(process.returncode or 0)


@pytest.mark.e2e
@pytest.mark.slow
def test_graceful_shutdown_and_restart(tmp_path: Path) -> None:
    """TC-brg-015: daemon 执行真实请求后可优雅退出并在同端口重启。

    业务规则:
    - daemon 的运行时对象必须在同步原语仍有效时完成清理。
    - 正常退出后同一服务地址必须可重新启动并接受请求。

    错误模型:
    - 全局析构顺序错误导致 ``mutex lock failed``；退出残留阻止重启。

    失败意味着:
    - 服务发布、重启或测试隔离仍可能随机失败。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    port = _free_port()
    logs: list[Path] = []

    for attempt in (1, 2):
        log_path = tmp_path / f"daemon-{attempt}.log"
        logs.append(log_path)
        process, log_file = _start_daemon(port, log_path)
        try:
            _wait_ready(process, port)
            response = asyncio.run(_request(port, "agent.execute", {
                "agent_id": "basic-agent",
                "user_id": f"lifecycle-user-{attempt}",
                "client_id": f"lifecycle-client-{attempt}",
                "content": "请执行一次生命周期检查",
                "dialogue_id": f"lifecycle-{attempt}",
            }))
            assert response.get("result", {}).get("status") == "submitted"
            time.sleep(0.5)
        finally:
            exit_code = _stop_daemon(process, log_file)

        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        assert exit_code == 0, log_text[-4000:]
        assert "agentflowd shutdown complete" in log_text
        assert "mutex lock failed" not in log_text.lower()
        assert "pthread_mutex" not in log_text.lower()

    assert len(logs) == 2
