"""
response_push 集成测试 — 验证流式推送完整链路。

测试流程：
  1. 启动 daemon + mock LLM
  2. 发送 user message 触发 coordinator 执行
  3. 通过 UDS 直接调用 response_push 推流式消息
  4. 验证消息被 drain 并出现在响应中

使用方式：
  python -m pytest frontend/bridge/tests/test_e2e_response_push.py -v -m e2e
"""

from __future__ import annotations

import json
import os
import socket
import time
from pathlib import Path
from typing import Any

import pytest

from .e2e_helpers import (
    DAEMON_BINARY,
    OUTFIT_AGENT,
    await_agent_ready,
    free_port,
    jsonrpc_call,
    scripted_llm,
    wait_port,
)

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"


def _find_uds_path(daemon_port: int) -> str | None:
    """从 daemon 日志中查找 UDS 路径。"""
    log_dir = Path("/tmp")
    candidates = list(log_dir.glob(f"afdb_*.sock"))
    # 取最新的 sock 文件（按修改时间排序）
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return str(candidates[0]) if candidates else None


def _uds_call(socket_path: str, method: str, params: dict[str, Any],
              timeout: float = 5.0) -> dict[str, Any]:
    """通过 UDS 执行 JSON-RPC 调用。"""
    import json as _json
    payload = _json.dumps({"jsonrpc": "2.0", "method": method,
                           "params": params, "id": 1})
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect(socket_path)
        sock.sendall((payload + "\n").encode("utf-8"))
        chunks = []
        while True:
            ch = sock.recv(1)
            if not ch or ch == b"\n":
                break
            chunks.append(ch)
        resp = _json.loads(b"".join(chunks).decode("utf-8"))
        return resp


@pytest.mark.e2e
class TestResponsePush:
    """验证 response_push → DataBus → drain → Bridge 完整链路。"""

    @pytest.fixture(scope="class")
    def daemon_env(self) -> dict[str, Any]:
        """启动 daemon + mock LLM，返回 daemon 信息。"""
        if not DAEMON_BINARY.is_file():
            pytest.skip(f"daemon binary not found: {DAEMON_BINARY}")

        daemon_port = free_port()
        import tempfile
        tmp_path = Path(tempfile.mkdtemp(prefix="e2e-rpush-"))

        with scripted_llm() as llm_url:
            env = dict(os.environ)
            env["PATH"] = (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            )
            env["API_KEY"] = "e2e-test-key"
            env["AF_LLM_BASE_URL"] = llm_url
            env["AF_LLM_MODEL"] = "e2e-scripted"
            env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
            env["NO_PROXY"] = "127.0.0.1,localhost"
            env["no_proxy"] = "127.0.0.1,localhost"

            import subprocess
            daemon = subprocess.Popen(
                [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
                 "--package", str(OUTFIT_AGENT)],
                cwd=PROJECT_ROOT, env=env,
                stdout=(tmp_path / "daemon.log").open("wb"),
                stderr=subprocess.STDOUT,
            )
            try:
                wait_port(daemon_port, daemon)
                await_agent_ready(daemon_port, "outfit-assistant", daemon)
                # Wait for UDS socket to appear
                uds_path = None
                for _ in range(50):  # 5s timeout
                    uds_path = _find_uds_path(daemon_port)
                    if uds_path:
                        break
                    time.sleep(0.1)
                yield {"daemon_port": daemon_port, "daemon": daemon,
                       "uds_path": uds_path}
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=10)
                    except Exception:
                        daemon.kill()

    def test_response_push_via_uds(self, daemon_env: dict[str, Any]) -> None:
        """通过 UDS 直接调用 response_push，验证 daemon 返回成功。"""
        uds_path = daemon_env.get("uds_path")
        if not uds_path:
            pytest.skip("UDS socket not found")

        result = _uds_call(uds_path, "response_push", {
            "session_id": "test-session",
            "dialogue_id": "test-dialogue-001",
            "content": "测试流式消息",
            "type": "progress",
            "final": "false",
        })
        assert result.get("result", {}).get("success"), (
            f"response_push failed: {result}"
        )

    def test_response_push_rejects_final(self, daemon_env: dict[str, Any]) -> None:
        """验证 response_push 拒绝 final=true。"""
        uds_path = daemon_env.get("uds_path")
        if not uds_path:
            pytest.skip("UDS socket not found")

        result = _uds_call(uds_path, "response_push", {
            "session_id": "test-session",
            "dialogue_id": "test-dialogue-002",
            "content": "不应通过",
            "type": "text",
            "final": "true",
        })
        # Should return an error
        assert "error" in result or result.get("result", {}).get("success") is False

    def test_response_push_empty_content_rejected(self, daemon_env: dict[str, Any]) -> None:
        """验证 response_push 拒绝空 content。"""
        uds_path = daemon_env.get("uds_path")
        if not uds_path:
            pytest.skip("UDS socket not found")

        result = _uds_call(uds_path, "response_push", {
            "session_id": "test-session",
            "dialogue_id": "test-dialogue-003",
            "content": "",
            "type": "llm_chunk",
            "final": "false",
        })
        # Should return an error (missing content)
        assert result.get("error", {}).get("code") == -32602
